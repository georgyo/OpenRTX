/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * HR_C6000 DMR modem driver of the Connect Systems CS7000 and CS7000-M17
 * Plus: the interfaces/dmr_baseband.h implementation shared by the two
 * targets, whose C6000 wiring is identical (same "user" SPI on SPI2, same
 * V_SPI pins PB3..PB6, same interrupt pins PC0..PC3).
 *
 * Register numbers and sequences come from the HR_C6000 User Manual (OpenGD77
 * team translation), the G4EML register table and the OEM capture in
 * HR_C6000_CS7000.cpp, with OpenGD77 as confirmation only. Nothing has been
 * run on a radio: every "UNVERIFIED on hardware" comment is an entry of
 * docs/dmr_bringup.md.
 *
 * Structure:
 *
 *  - Interrupt mailbox. The C6000 signals the slot timing and the Layer-2
 *    events with ~300ns active-low pulses (manual §4.4) caught by the
 *    falling-edge EXTI shim of gpio_irq_stm32.cpp. The callbacks run in
 *    interrupt context and only latch: an event bit in `pending`, a counter
 *    per line and the tick of the slot interrupt. PC0..PC3 are all four
 *    attached, each with its own counter, because the schematic of the
 *    CS7000-M17 Plus (V01) and the pinmap disagree on which pin carries
 *    SYS_INTER (PC3_C versus PC1, see pinmap.h) and nothing has been
 *    validated yet: the dispatch to the event bits uses the pinmap
 *    constants DMR_TS_INT / DMR_TX_INT and, for SYS_INTER, a runtime
 *    choice between DMR_SYS_INT (default) and DMR_SYS_INT_ALT made from
 *    the monitor screen's "SYS line" action (DMRBB_DRV_SYS_LINE,
 *    interfaces/dmr_baseband.h); the line not chosen only counts.
 *
 *  - Bus access. All the SPI traffic happens in the thread calling
 *    dmrbb_waitEvent() (the rtx thread): the "user" SPI is shared with the
 *    4ms OpenMusic refill of the audio thread through the c6000_spi mutex
 *    and the 2us chip select guards of ScopedChipSelect, none of which is
 *    usable from an interrupt.
 *
 *  - Waiting. Miosix (the version in this tree) has no timed variant of
 *    Thread::IRQwait(), and a thread in Thread::sleep() is not released by
 *    IRQwakeup(), so dmrbb_waitEvent() polls the mailbox every millisecond
 *    with the interrupts latched by the ISRs in between. The 1ms latency is
 *    small against the t2 = 27ms window the chip gives to program the next
 *    slot (manual Figure 5.12) and the slot tick itself is timestamped in
 *    the ISR.
 *
 *  - Voice payload. 27 bytes per voice burst (three AMBE+2 frames, manual
 *    §4.6.1) exchanged on the vocoder SPI port ("V_SPI", C6000 slave) with
 *    a bit-banged master on the spi_bitbang driver: read command 0x83,
 *    address 0x00, 27 bytes; write command 0x03, address 0x00, 27 bytes, one
 *    operation per chip select. UNVERIFIED on hardware, with the TX RAM
 *    0x30 / RX RAM 0x30 transport over the "user" SPI as the selectable
 *    alternative (DMRBB_PAGE_DRIVER / DMRBB_DRV_VOICE_PATH).
 *
 *  - Transmission. The RF chain (TX VCO, PLL lock, APC, PA) is keyed by
 *    radio_enableTx() of radio_CS7000.cpp, which in DMR mode writes nothing
 *    to the chip; the modem side is armed by dmrbb_startTx() (0x21, 0x40)
 *    on the request of the call controller and bursts go out only when
 *    dmrbb_setNextSlot(0x80) selects a TX slot at a later slot tick. The
 *    two are called by the DMR opMode handler in the same pass, modem
 *    first, RF right after: the arming write radiates nothing by itself,
 *    so the first burst always finds the PA keyed and the PLL locked (or
 *    the PA left off when the lock failed). Ending: the controller idles
 *    the slot after the terminator (0x41 = 0x00) and calls dmrbb_startRx(),
 *    then radio_enableRx() drops the PA and retunes; radio_disableRtx()
 *    idles the modem before dropping the PA. The PA stays keyed for the
 *    whole call in direct mode, RF_TX_INTER is only counted and
 *    timestamped (DMRBB_DRV_RFTX_TICK / _LEAD), see radio_CS7000.cpp for
 *    the per-slot keying alternative.
 *
 *  - Ownership of the chip. dmrbb_configure() puts the C6000 in DMR mode and
 *    marks the driver active; dmrbb_terminate() idles the chip and marks it
 *    inactive, after which the slot/mode writes are no-ops. This matters
 *    because rtx.cpp calls radio_setOpmode() for the new mode (fmMode())
 *    before disabling the old one: radio_CS7000.cpp terminates this driver
 *    before fmMode() and the later dmrbb_idle()/dmrbb_terminate() of
 *    OpMode_DMR::disable() must not touch the FM configuration. The same
 *    two calls tell the OpenMusic DAC driver (Cx000_dac.cpp) whether the
 *    vocoder bits of register 0x06 must survive a beep: only while this
 *    driver owns the chip, FM and M17 keep their wholesale 0x22 / 0x20.
 */

#include <miosix.h>
#include <stdint.h>
#include <string.h>
#include "drivers/SPI/spi_bitbang.h"
#include "drivers/audio/Cx000_dac.h"
#include "drivers/baseband/HR_C6000.h"
#include "drivers/baseband/dmr_CS7000.h"
#include "hwconfig.h"
#include "interfaces/delays.h"
#include "interfaces/dmr_baseband.h"
#include "peripherals/gpio.h"
#include "peripherals/gpio_irq.h"

/*
 * Pin number of a pinmap entry of the form "GPIOx,n": the argument is
 * expanded to "GPIOx,n" before being substituted, so the inner macro sees
 * two arguments.
 */
#define PIN_OF_PAIR(port, pin) (pin)
#define PIN_NUMBER(x) PIN_OF_PAIR(x)

/* V_SPI command bytes, manual §4.6.1 Figure 4.17 */
static constexpr uint8_t VSPI_CMD_READ = 0x83;
static constexpr uint8_t VSPI_CMD_WRITE = 0x03;
static constexpr uint8_t VSPI_ADDR = 0x00;

/* TX/RX RAM address of the 216-bit voice payload (manual Table 5.2) */
static constexpr uint8_t RAM_VOICE_ADDR = 0x30;
static constexpr uint8_t RAM_LC_ADDR = 0x00;

/*
 * RX RAM address of the CACH Short LC: 0x24-0x26 plus bits 7:4 of 0x27 hold
 * the 28 control bits (SLCO and 24 payload bits, TS 102 361-1 §7.1.4,
 * §9.1.7), 0x28 the 8 check bits (CRC-8, Annex B.3.7). The address is the
 * one of the TX-side RAM allocation (manual Table 5.2, "Short LC
 * 0x24~0x28", the RAM the MCU fills for a transmission): the RX-side table
 * (5.3) is omitted in the translated manual, so the RX RAM is assumed to
 * mirror the TX map, as RAM_LC_ADDR at 0x00 already assumes. Five bytes are
 * read as they are, the decoding is left to the protocol layer. UNVERIFIED
 * on hardware: the RX address and layout, and whether 0x28 is the CRC as
 * received or the chip's own check byte.
 */
static constexpr uint8_t RAM_SHORT_LC_ADDR = 0x24;
static constexpr uint8_t SHORT_LC_BYTES = 5;

/* Register 0x82 flags, manual §5.1.1, see HR_C6000::Irq82 */
static constexpr uint8_t IRQ_TX_START = HR_C6000::IRQ_TX_START;
static constexpr uint8_t IRQ_TX_END = HR_C6000::IRQ_TX_END;
static constexpr uint8_t IRQ_LATE_ENTRY = HR_C6000::IRQ_LATE_ENTRY;
static constexpr uint8_t IRQ_RX_DATA = HR_C6000::IRQ_RX_DATA;
static constexpr uint8_t IRQ_RX_INFO = HR_C6000::IRQ_RX_INFO;
static constexpr uint8_t IRQ_ABNORMAL_EXIT = HR_C6000::IRQ_ABNORMAL_EXIT;

/* Register 0x86 sub-status, bit 2 "Short LC received" (G4EML table) */
static constexpr uint8_t SUB86_SHORT_LC = HR_C6000::SUB86_SHORT_LC;

/* Register 0x51 fields, G4EML table */
static constexpr uint8_t R51_CRC_BAD = 0x04;
static constexpr uint8_t R51_CLASS_MASK = 0x03;
static constexpr uint8_t R51_CLASS_VOICE = 0x01;

/* Voice payload transport, DMRBB_DRV_VOICE_PATH */
static constexpr uint8_t VOICE_PATH_VSPI = 0;
static constexpr uint8_t VOICE_PATH_RAM = 1;

/* Polling period of dmrbb_waitEvent(), milliseconds */
static constexpr unsigned WAIT_POLL_MS = 1;

/*
 * Interrupt lines. The four EXTI lines PC0..PC3 are all attached; the
 * mapping of the C6000 pins on them comes from pinmap.h, with the SYS_INTER
 * line selectable at runtime between the pinmap constant (SYS_LINE, default)
 * and the schematic's alternative (ALT_LINE), see DMRBB_DRV_SYS_LINE.
 */
static constexpr unsigned NUM_LINES = 4;
static constexpr uint8_t TS_LINE = PIN_NUMBER(DMR_TS_INT);
static constexpr uint8_t SYS_LINE = PIN_NUMBER(DMR_SYS_INT);
static constexpr uint8_t TX_LINE = PIN_NUMBER(DMR_TX_INT);
static constexpr uint8_t ALT_LINE = PIN_NUMBER(DMR_SYS_INT_ALT);

static_assert(TS_LINE < NUM_LINES, "DMR_TS_INT must be PC0..PC3");
static_assert(SYS_LINE < NUM_LINES, "DMR_SYS_INT must be PC0..PC3");
static_assert(TX_LINE < NUM_LINES, "DMR_TX_INT must be PC0..PC3");
static_assert(ALT_LINE < NUM_LINES, "DMR_SYS_INT_ALT must be PC0..PC3");
static_assert(SYS_LINE == DMRBB_SYS_LINE_PINMAP,
              "DMRBB_SYS_LINE_PINMAP must match DMR_SYS_INT");
static_assert(ALT_LINE == DMRBB_SYS_LINE_SCHEMATIC,
              "DMRBB_SYS_LINE_SCHEMATIC must match DMR_SYS_INT_ALT");

static const struct gpioPin irqPins[NUM_LINES] = {
    { &GpioC, 0 },
    { &GpioC, 1 },
    { &GpioC, 2 },
    { &GpioC, 3 },
};

/* V_SPI chip select, driven by hand around each transfer */
static const struct gpioPin vocCs = { &GpioB, PIN_NUMBER(VOC_CS) };

/* Interrupt mailbox, written in ISR context and read under interrupt lock */
static volatile uint32_t pending = 0;
static volatile uint32_t tsCount = 0;
static volatile uint32_t sysCount = 0;
static volatile uint32_t rftxCount = 0;
static volatile uint32_t lineCount[NUM_LINES] = { 0, 0, 0, 0 };
static volatile uint32_t tsTick = 0;
static volatile uint32_t rftxTick = 0;
static volatile uint16_t rftxLead = 0;

/* Driver state, rtx thread only */
static struct dmrbbConfig config;
static bool active = false;
static bool attached = false;
static volatile uint8_t voicePath = VOICE_PATH_VSPI;

/*
 * Line dispatched as SYS_INTER, read in interrupt context: SYS_LINE (the
 * pinmap, PC1) unless the monitor screen switched it to ALT_LINE (the
 * schematic, PC3_C). UNVERIFIED on hardware which one each board needs.
 */
static volatile uint8_t sysLine = SYS_LINE;

/**
 * Interrupt callback shared by the four lines, interrupt context: latch and
 * count, nothing else.
 *
 * @param arg: line number, 0..3.
 */
static void lineIsr(void *arg)
{
    const uint8_t line = static_cast<uint8_t>(reinterpret_cast<uintptr_t>(arg));

    lineCount[line] += 1;

    if (line == TS_LINE) {
        tsCount += 1;
        tsTick = static_cast<uint32_t>(getTick());
        pending |= DMRBB_EV_TS;
    } else if (line == sysLine) {
        sysCount += 1;
        pending |= DMRBB_EV_SYS;
    } else if (line == TX_LINE) {
        /*
         * Own burst imminent, 0x12[5:0] x 100us ahead: timestamped for the
         * check of the advance against the slot tick, nothing is keyed
         * from here (the PA is on for the whole call).
         */
        rftxCount += 1;
        rftxTick = static_cast<uint32_t>(getTick());
        rftxLead = static_cast<uint16_t>(rftxTick - tsTick);
        pending |= DMRBB_EV_RFTX;
    }

    /*
     * The line not selected as SYS_INTER (PC3 by default, PC1 after the
     * switch) only counts: the monitor screen shows which line pulses
     * every 60ms, the knob follows it, then DMR_SYS_INT is fixed per
     * target in pinmap.h.
     */
}

/**
 * Detach the interrupt lines.
 */
static void detachLines()
{
    if (attached == false)
        return;

    for (unsigned i = 0; i < NUM_LINES; i++)
        gpioIrq_detach(&irqPins[i]);

    attached = false;
}

/**
 * Attach the four interrupt lines, falling edge, dropping whatever is still
 * pending from a previous configuration.
 */
static void attachLines()
{
    detachLines();

    {
        miosix::FastInterruptDisableLock dLock;
        pending = 0;
    }

    for (unsigned i = 0; i < NUM_LINES; i++) {
        gpioPin_setMode(&irqPins[i], INPUT);
        gpioIrq_attach(&irqPins[i], GPIO_IRQ_FALLING, lineIsr,
                       reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
    }

    attached = true;
}

/**
 * V_SPI read of one voice burst: command 0x83, address 0x00, then 27 bytes
 * with the chip select held low (manual §4.6.1).
 * UNVERIFIED on hardware: command format, clock phase and pin orientation.
 *
 * @param buf: destination of the 27 bytes.
 */
static void vspiRead(uint8_t *buf)
{
    const struct spiDevice *spi =
        reinterpret_cast<const struct spiDevice *>(&voc_spi);
    const uint8_t cmd[2] = { VSPI_CMD_READ, VSPI_ADDR };

    spi_acquire(spi);
    gpioPin_clear(&vocCs);
    spi_send(spi, cmd, sizeof(cmd));
    spi_receive(spi, buf, 27);
    gpioPin_set(&vocCs);
    spi_release(spi);
}

/**
 * V_SPI write of one voice burst: command 0x03, address 0x00, then 27 bytes
 * with the chip select held low (manual §4.6.1).
 * UNVERIFIED on hardware, see vspiRead().
 *
 * @param buf: the 27 bytes to be sent.
 */
static void vspiWrite(const uint8_t *buf)
{
    const struct spiDevice *spi =
        reinterpret_cast<const struct spiDevice *>(&voc_spi);
    const uint8_t cmd[2] = { VSPI_CMD_WRITE, VSPI_ADDR };

    spi_acquire(spi);
    gpioPin_clear(&vocCs);
    spi_send(spi, cmd, sizeof(cmd));
    spi_send(spi, buf, 27);
    gpioPin_set(&vocCs);
    spi_release(spi);
}

/**
 * Register reads of a SYS event, in the documented order: 0x82, the
 * sub-status registers whose bit is set, 0x51/0x52/0x5F/0x42, the RX RAM
 * link control when a control frame was parsed (or a late entry happened),
 * the voice payload when the burst is a voice burst with a good CRC, RSSI
 * and FSK error count, then 0x83 acknowledging exactly the flags read.
 *
 * @param s: snapshot to fill.
 */
static void readSysEvent(struct dmrbbSnapshot *s)
{
    const uint8_t irq = C6000.readIrqFlags();
    s->irq82 = irq;

    if (irq & IRQ_TX_START)
        s->sub84 = C6000.readCfgRegister(0x84);
    if (irq & IRQ_TX_END) {
        s->sub86 = C6000.readCfgRegister(0x86);

        /*
         * Short LC of the CACH: the manual files "Short LC received" under
         * the end-of-transmission sub-status (0x82 bit 5, 0x86 bit 2); the
         * 36 bits are read from RX RAM 0x24-0x28, the TX-side address of
         * Table 5.2 assumed to be mirrored on the RX side. Read right
         * away, before anything else could overwrite the RAM; the snapshot
         * keeps the previous value when the flag is clear. UNVERIFIED on
         * hardware, see RAM_SHORT_LC_ADDR.
         */
        if (s->sub86 & SUB86_SHORT_LC)
            C6000.readDataRam(RAM_SHORT_LC_ADDR, s->shortLc, SHORT_LC_BYTES);
    }
    if (irq & IRQ_RX_INFO)
        s->sub90 = C6000.readCfgRegister(0x90);
    if (irq & IRQ_ABNORMAL_EXIT)
        s->sub98 = C6000.readCfgRegister(0x98);

    s->r51 = C6000.readCfgRegister(0x51);
    s->r52 = C6000.readCfgRegister(0x52);
    s->r5f = C6000.readCfgRegister(0x5F);
    s->r42 = C6000.readCfgRegister(0x42);

    if (irq & (IRQ_RX_DATA | IRQ_LATE_ENTRY))
        C6000.readDataRam(RAM_LC_ADDR, s->lc, sizeof(s->lc));

    /*
     * Voice burst: sync class 01 with a good CRC, reported together with the
     * "received data" flag. The read happens before the acknowledge so that
     * the chip cannot overwrite the payload with the next burst in between.
     */
    s->voiceValid = false;
    if ((irq & IRQ_RX_DATA) && ((s->r51 & R51_CLASS_MASK) == R51_CLASS_VOICE)
        && ((s->r51 & R51_CRC_BAD) == 0)) {
        if (voicePath == VOICE_PATH_RAM)
            C6000.readDataRam(RAM_VOICE_ADDR, s->voice, sizeof(s->voice));
        else
            vspiRead(s->voice);

        s->voiceValid = true;
    }

    const uint8_t rssiHi = C6000.readCfgRegister(0x43);
    const uint8_t rssiLo = C6000.readCfgRegister(0x44);
    s->rssi = static_cast<uint16_t>((rssiHi << 8) | rssiLo);
    s->fskErr = C6000.readCfgRegister(0x0F);

    C6000.clearIrq(irq);
}

int dmrbb_init(void)
{
    /*
     * V_SPI master: clock and MOSI outputs low, MISO input, chip select
     * high. The lines stay driven after dmrbb_terminate() as well, a floating
     * V_SCLK would clock garbage into the vocoder port of the C6000.
     */
    spiBitbang_init(&voc_spi);
    gpioPin_setMode(&vocCs, OUTPUT);
    gpioPin_set(&vocCs);

    {
        miosix::FastInterruptDisableLock dLock;
        pending = 0;
        tsCount = 0;
        sysCount = 0;
        rftxCount = 0;
        tsTick = 0;
        rftxTick = 0;
        rftxLead = 0;
        for (unsigned i = 0; i < NUM_LINES; i++)
            lineCount[i] = 0;
    }

    return 0;
}

void dmrbb_terminate(void)
{
    detachLines();

    if (active) {
        C6000.dmrIdle();
        active = false;
    }

    Cx000dac_setVocoderBits(false);
}

void dmrbb_configure(const struct dmrbbConfig *cfg)
{
    config = *cfg;

    C6000.dmrMode();

    /*
     * The mode word is part of the dmrMode() table with its default; the
     * monitor screen can try the manual's 0x6A through dmrbbConfig::modeReg.
     */
    if (cfg->modeReg != DMRBB_MODE_REG_DEFAULT)
        C6000.writeCfgRegister(0x10, cfg->modeReg);

    /*
     * Same for the RF_TX_INTER advance word 0x12: the table writes the OEM
     * 0x98, a different value asked for from the monitor screen (OpenGD77's
     * 0x15, carried by rtxStatus_t::dmr_rfAdvance into cfg->rfAdvance) is
     * rewritten here so that it survives the reconfigurations the opMode
     * handler does on its own. UNVERIFIED on hardware.
     */
    if ((cfg->rfAdvance != 0) && (cfg->rfAdvance != DMRBB_RF_ADVANCE_DEFAULT))
        C6000.writeCfgRegister(0x12, cfg->rfAdvance);

    C6000.setColorCode(cfg->colorCode);
    C6000.setLocalAddress(cfg->ownId);
    C6000.setAccessPolicy(cfg->polite);

    /* Beeps and audio streams must leave the vocoder bits of 0x06 alone */
    Cx000dac_setVocoderBits(true);

    attachLines();
    active = true;
}

void dmrbb_startRx(void)
{
    if (active == false)
        return;

    C6000.dmrRxStart();
}

void dmrbb_startTx(bool activeTiming)
{
    if (active == false)
        return;

    /*
     * Register 0x21: access policy plus "clear the vocoder encode buffer"
     * (bit 1, self-clearing), 0xA2 polite to own colour code or 0x02
     * impolite (G4EML table, OpenGD77). Register 0x40: TxEn + RxEn with
     * active timing (0xE3, direct mode: the chip generates the 30ms time
     * axis) or passive timing (0xC3, repeater mode: bursts aligned to the
     * received sync). UNVERIFIED on hardware: manual §5.4.5 uses 0xA3.
     *
     * The RF chain is keyed separately by radio_enableTx(): this only arms
     * the modem, the first burst needs a later dmrbb_setNextSlot(0x80). A
     * transmission refused by the chip (channel busy under the polite
     * policy) shows up as bit 7 of register 0x82 in the next snapshot,
     * HR_C6000::txRejected(); TX start and end as bits 6 and 5 with the
     * sub-status registers 0x84 / 0x86 (sub84 / sub86 of the snapshot).
     */
    C6000.dmrTxStart(activeTiming, config.polite);
}

void dmrbb_idle(void)
{
    if (active == false)
        return;

    C6000.dmrIdle();
}

void dmrbb_setNextSlot(uint8_t r41)
{
    if (active == false)
        return;

    C6000.writeCfgRegister(0x41, r41);
}

void dmrbb_setTxFrameType(uint8_t r50)
{
    if (active == false)
        return;

    /*
     * Written as given: the LCSS convention of the voice bursts (OpenGD77's
     * 0x08 | seq << 4 or the manual's §5.4.5 sequence) is the call
     * controller's choice, DMR::r50Voice(), UNVERIFIED on hardware.
     */
    C6000.writeCfgRegister(0x50, r50);
}

void dmrbb_writeTxLc(const uint8_t lc[12])
{
    if (active == false)
        return;

    /* TX RAM 0x00-0x0B, one transfer with the chip select held low */
    C6000.writeDataRam(RAM_LC_ADDR, lc, 12);
}

void dmrbb_writeVoice(const uint8_t ambe[27])
{
    if (active == false)
        return;

    /*
     * UNVERIFIED on hardware: V_SPI is the transport selected by register
     * 0x06 = 0x21; the TX RAM 0x30-0x4A alternative (manual Table 5.2) is
     * reachable from the monitor screen through DMRBB_DRV_VOICE_PATH.
     */
    if (voicePath == VOICE_PATH_RAM)
        C6000.writeDataRam(RAM_VOICE_ADDR, ambe, 27);
    else
        vspiWrite(ambe);
}

uint32_t dmrbb_waitEvent(struct dmrbbSnapshot *s, unsigned timeoutMs)
{
    const long long deadline = getTick() + static_cast<long long>(timeoutMs);
    uint32_t ev = 0;

    for (;;) {
        {
            miosix::FastInterruptDisableLock dLock;
            ev = pending;
            pending = 0;
            s->tsCount = tsCount;
            s->sysCount = sysCount;
            s->rftxCount = rftxCount;
            s->tsTick = tsTick;
            for (unsigned i = 0; i < NUM_LINES; i++)
                s->lineCount[i] = lineCount[i];
        }

        if (ev != 0)
            break;

        if (getTick() >= deadline)
            return DMRBB_EV_TIMEOUT;

        sleepFor(0u, WAIT_POLL_MS);
    }

    /*
     * Slot tick: timecode and slot status for the tracking of the next
     * slot. When a SYS event is pending as well, its reads below refresh
     * 0x52/0x42 once more.
     */
    if (ev & DMRBB_EV_TS) {
        s->r52 = C6000.readCfgRegister(0x52);
        s->r42 = C6000.readCfgRegister(0x42);
    }

    if (ev & DMRBB_EV_SYS)
        readSysEvent(s);

    return ev;
}

uint8_t dmrbb_readRegister(uint8_t page, uint8_t addr)
{
    if (page == DMRBB_PAGE_DRIVER) {
        if (addr == DMRBB_DRV_VOICE_PATH)
            return voicePath;

        if (addr == DMRBB_DRV_SYS_LINE)
            return sysLine;

        if ((addr >= DMRBB_DRV_RFTX_TICK(0))
            && (addr <= DMRBB_DRV_RFTX_LEAD(1))) {
            uint32_t tick;
            uint16_t lead;
            {
                miosix::FastInterruptDisableLock dLock;
                tick = rftxTick;
                lead = rftxLead;
            }

            if (addr < DMRBB_DRV_RFTX_LEAD(0)) {
                const unsigned b = addr - DMRBB_DRV_RFTX_TICK(0);
                return static_cast<uint8_t>(tick >> (8 * b));
            }

            const unsigned b = addr - DMRBB_DRV_RFTX_LEAD(0);
            return static_cast<uint8_t>(lead >> (8 * b));
        }

        for (unsigned i = 0; i < NUM_LINES; i++) {
            const uint8_t base = static_cast<uint8_t>(DMRBB_DRV_LINE_COUNT(i));
            if ((addr >= base) && (addr < (base + 4))) {
                uint32_t cnt;
                {
                    miosix::FastInterruptDisableLock dLock;
                    cnt = lineCount[i];
                }

                return static_cast<uint8_t>(cnt >> (8 * (addr - base)));
            }
        }

        return 0;
    }

    switch (page) {
        case static_cast<uint8_t>(C6000_SpiOpModes::AUX):
            return C6000.readAuxRegister(addr);

        case static_cast<uint8_t>(C6000_SpiOpModes::DATA): {
            uint8_t value = 0;
            C6000.readDataRam(addr, &value, 1);
            return value;
        }

        case static_cast<uint8_t>(C6000_SpiOpModes::CONFIG):
            return C6000.readCfgRegister(addr);

        case static_cast<uint8_t>(C6000_SpiOpModes::DATA_R):
            return C6000.readPageRegister(page, addr);

        default:
            return 0;
    }
}

void dmrbb_writeRegister(uint8_t page, uint8_t addr, uint8_t v)
{
    if (page == DMRBB_PAGE_DRIVER) {
        if (addr == DMRBB_DRV_VOICE_PATH)
            voicePath = (v != 0) ? VOICE_PATH_RAM : VOICE_PATH_VSPI;

        /*
         * SYS_INTER line: only the two candidates are accepted, a pulse on
         * the line already latched between the check and the store is
         * counted on the old assignment, which is harmless.
         */
        if ((addr == DMRBB_DRV_SYS_LINE)
            && ((v == SYS_LINE) || (v == ALT_LINE)))
            sysLine = v;

        return;
    }

    switch (page) {
        case static_cast<uint8_t>(C6000_SpiOpModes::AUX):
            C6000.writeAuxRegister(addr, v);
            break;

        case static_cast<uint8_t>(C6000_SpiOpModes::DATA):
            C6000.writeDataRam(addr, &v, 1);
            break;

        case static_cast<uint8_t>(C6000_SpiOpModes::CONFIG):
            C6000.writeCfgRegister(addr, v);
            break;

        case static_cast<uint8_t>(C6000_SpiOpModes::DATA_R):
            C6000.writePageRegister(page, addr, v);
            break;

        default:
            break;
    }
}
