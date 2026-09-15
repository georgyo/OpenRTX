/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host tests of the HR_C6000 DMR driver of the CS7000 / CS7000-PLUS: the real
 * HR_Cx000.cpp, HR_C6000.cpp, HR_C6000_CS7000.cpp and dmr_CS7000.cpp are
 * compiled against a recording fake "user" SPI device, a fake V_SPI device,
 * fake gpio ports, a fake gpio interrupt shim and fake delays (the fake
 * hwconfig.h in tests/unit/fake/cs7000 shadows the target one).
 *
 * The fake "user" SPI splits the traffic into transactions delimited by the
 * chip select (GpioB pin 12) and decodes the HR_C6000 command format: page in
 * the low bits of the command byte, read flag in bit 7, one address byte,
 * then data with auto-incrementing address. Writes land in per-page register
 * arrays, reads are served from them, so a test can both assert the exact
 * byte stream and preload what the chip would answer.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <vector>

#include "drivers/baseband/HR_C6000.h"
#include "hwconfig.h"
#include "interfaces/delays.h"
#include "interfaces/dmr_baseband.h"
#include "peripherals/gpio_irq.h"

#ifndef FAKE_CS7000_HWCONFIG
#error "The fake CS7000 hwconfig.h must shadow the real one in this test"
#endif

/*
 * Fake gpio ports. Chip select transitions are forwarded to the SPI fakes,
 * mode changes are recorded for the input configuration checks.
 */
struct GpioEvent {
    const struct gpioDev *port;
    uint8_t pin;
    uint16_t mode;
};

static std::vector<GpioEvent> gpioModes;

static constexpr uint8_t USPI_CS_PIN = 12;
static constexpr uint8_t VSPI_CS_PIN = 6;

/*
 * Recording SPI bus with the HR_C6000 command decoding.
 */
struct Transaction {
    std::vector<uint8_t> tx; /* bytes sent, 0x00 for receive-only bytes  */
    size_t rxBytes;          /* bytes received back                       */
};

struct FakeBus {
    std::vector<Transaction> log;
    bool csLow = false;
    uint8_t pages[8][256];
    uint8_t cmd = 0;
    uint16_t addr = 0;
    size_t pos = 0; /* byte position inside the current transaction */

    void reset()
    {
        log.clear();
        csLow = false;
        memset(pages, 0, sizeof(pages));
        cmd = 0;
        addr = 0;
        pos = 0;
    }

    void select()
    {
        REQUIRE(csLow == false);
        csLow = true;
        log.push_back(Transaction());
        pos = 0;
    }

    /*
     * No check on the chip select state: the HR_C6000 constructor raises the
     * line once at static initialisation, before any transaction.
     */
    void deselect()
    {
        csLow = false;
    }

    /* Page from the command byte, 16-bit addresses take one more byte */
    size_t headerLen() const
    {
        return (cmd & 0x40) ? 3 : 2;
    }

    uint8_t *page()
    {
        return pages[cmd & 0x07];
    }

    void transfer(const uint8_t *txBuf, uint8_t *rxBuf, size_t size)
    {
        REQUIRE(csLow == true);
        Transaction &t = log.back();

        for (size_t i = 0; i < size; i++) {
            uint8_t out = (txBuf != NULL) ? txBuf[i] : 0x00;
            uint8_t in = 0x00;

            if (pos == 0) {
                cmd = out;
            } else if (pos == 1) {
                addr = out;
            } else if ((pos == 2) && (cmd & 0x40)) {
                addr |= (uint16_t)((out & 0x07) << 8);
            } else {
                size_t off = pos - headerLen();
                uint16_t a = (uint16_t)((addr + off) & 0xFF);

                if (cmd & 0x80)
                    in = page()[a];
                else
                    page()[a] = out;
            }

            t.tx.push_back(out);
            if (rxBuf != NULL) {
                rxBuf[i] = in;
                t.rxBytes += 1;
            }

            pos += 1;
        }
    }
};

static FakeBus uspi;
static FakeBus vspi;

static int fakeMode(const struct gpioDev *dev, const uint8_t pin,
                    const uint16_t mode)
{
    gpioModes.push_back({ dev, pin, mode });
    return 0;
}

static void fakeSet(const struct gpioDev *dev, const uint8_t pin)
{
    if ((dev == &GpioB) && (pin == USPI_CS_PIN))
        uspi.deselect();
    if ((dev == &GpioB) && (pin == VSPI_CS_PIN))
        vspi.deselect();
}

static void fakeClear(const struct gpioDev *dev, const uint8_t pin)
{
    if ((dev == &GpioB) && (pin == USPI_CS_PIN))
        uspi.select();
    if ((dev == &GpioB) && (pin == VSPI_CS_PIN))
        vspi.select();
}

static bool fakeRead(const struct gpioDev *dev, const uint8_t pin)
{
    (void)dev;
    (void)pin;
    return false;
}

static const struct gpioApi fakeGpioApi = { fakeMode, fakeSet, fakeClear,
                                            fakeRead };
const struct gpioDev GpioB = { &fakeGpioApi, nullptr };
const struct gpioDev GpioC = { &fakeGpioApi, nullptr };

static int uspiTransfer(const struct spiDevice *dev, const void *txBuf,
                        void *rxBuf, const size_t size)
{
    (void)dev;
    uspi.transfer(static_cast<const uint8_t *>(txBuf),
                  static_cast<uint8_t *>(rxBuf), size);
    return 0;
}

static int vspiTransfer(const struct spiDevice *dev, const void *txBuf,
                        void *rxBuf, const size_t size)
{
    (void)dev;
    vspi.transfer(static_cast<const uint8_t *>(txBuf),
                  static_cast<uint8_t *>(rxBuf), size);
    return 0;
}

static const struct spiDevice uspiDev = { uspiTransfer, nullptr, nullptr };
const struct spiCustomDevice voc_spi = { vspiTransfer, nullptr, nullptr,
                                         nullptr };

HR_C6000 C6000(&uspiDev, { &GpioB, USPI_CS_PIN });

/*
 * Fake bitbang driver init: the V_SPI device above is a plain recording
 * device, nothing to set up.
 */
static int vocSpiInits = 0;

extern "C" int spiBitbang_init(const struct spiCustomDevice *dev)
{
    REQUIRE(dev == &voc_spi);
    vocSpiInits += 1;
    return 0;
}

extern "C" void spiBitbang_terminate(const struct spiCustomDevice *dev)
{
    (void)dev;
}

/*
 * Fake gpio interrupt shim: attachments are recorded and the callbacks can
 * be fired by the test as the interrupt handler would.
 */
struct IrqLine {
    const struct gpioDev *port;
    uint8_t pin;
    enum gpioIrqEdge edge;
    void (*cb)(void *arg);
    void *arg;
};

static std::vector<IrqLine> irqAttached;
static std::vector<IrqLine> irqAttachLog;
static unsigned irqDetaches = 0;

extern "C" int gpioIrq_attach(const struct gpioPin *pin, enum gpioIrqEdge edge,
                              void (*cb)(void *arg), void *arg)
{
    for (const auto &l : irqAttached) {
        if (l.pin == pin->pin)
            return -1;
    }

    IrqLine l = { pin->port, pin->pin, edge, cb, arg };
    irqAttached.push_back(l);
    irqAttachLog.push_back(l);
    return 0;
}

extern "C" void gpioIrq_detach(const struct gpioPin *pin)
{
    for (size_t i = 0; i < irqAttached.size(); i++) {
        if (irqAttached[i].pin == pin->pin) {
            irqAttached.erase(irqAttached.begin() + i);
            irqDetaches += 1;
            return;
        }
    }
}

static void fireLine(uint8_t pin)
{
    for (const auto &l : irqAttached) {
        if (l.pin == pin) {
            l.cb(l.arg);
            return;
        }
    }

    FAIL("line not attached");
}

/*
 * Fake delays: a tick counter advanced by sleepFor(), so that the polling
 * loop of dmrbb_waitEvent() terminates.
 */
static long long fakeTick = 1000;
static unsigned sleeps = 0;

extern "C" void delayUs(unsigned int useconds)
{
    (void)useconds;
}

extern "C" void delayMs(unsigned int mseconds)
{
    (void)mseconds;
}

extern "C" void sleepFor(unsigned int seconds, unsigned int mseconds)
{
    fakeTick += (seconds * 1000) + mseconds;
    sleeps += 1;
}

extern "C" long long getTick()
{
    return fakeTick;
}

/*
 * Test helpers.
 */
static void resetAll()
{
    uspi.reset();
    vspi.reset();
    gpioModes.clear();
    irqAttached.clear();
    irqAttachLog.clear();
    irqDetaches = 0;
    sleeps = 0;
    vocSpiInits = 0;
}

static struct dmrbbConfig defaultConfig()
{
    struct dmrbbConfig cfg;
    cfg.colorCode = 1;
    cfg.timeslot = 2;
    cfg.ownId = 0x23C34E; /* 2345806 */
    cfg.polite = true;
    cfg.modeReg = DMRBB_MODE_REG_DEFAULT;
    return cfg;
}

/* Bring the driver up: init, configure, clear the record */
static void bringUp(struct dmrbbConfig cfg = defaultConfig())
{
    resetAll();
    REQUIRE(dmrbb_init() == 0);
    dmrbb_configure(&cfg);
    uspi.log.clear();
    vspi.log.clear();
}

static std::vector<uint8_t> cfgWrite(uint8_t reg, uint8_t value)
{
    return { 0x04, reg, value };
}

static void requireTx(const Transaction &t, const std::vector<uint8_t> &bytes)
{
    REQUIRE(t.tx == bytes);
}

/*
 * The dmrMode() register table (architecture table 4.1), in order.
 */
static const std::vector<std::vector<uint8_t>> dmrModeStream = {
    cfgWrite(0x00, 0x3F), cfgWrite(0x10, 0x6E), cfgWrite(0x11, 0x80),
    cfgWrite(0x01, 0xB0), cfgWrite(0x07, 0x0B), cfgWrite(0x08, 0xB8),
    cfgWrite(0x09, 0x00), cfgWrite(0x06, 0x21), cfgWrite(0x0D, 0x10),
    cfgWrite(0x12, 0x98), cfgWrite(0x13, 0x00), cfgWrite(0x14, 0x00),
    cfgWrite(0x15, 0x00), cfgWrite(0x16, 0x00), cfgWrite(0x1A, 0xFF),
    cfgWrite(0x1B, 0xFF), cfgWrite(0x1C, 0xFF), cfgWrite(0x1D, 0xFF),
    cfgWrite(0x1E, 0xFF), cfgWrite(0x1F, 0x10), cfgWrite(0x20, 0x00),
    cfgWrite(0x21, 0xA0), cfgWrite(0x2E, 0x04), cfgWrite(0x4B, 0x1B),
    cfgWrite(0x4C, 0x00), cfgWrite(0x5F, 0xF0), cfgWrite(0x36, 0x10),
    cfgWrite(0x25, 0x0E), cfgWrite(0x26, 0xFD), cfgWrite(0xE0, 0x8B),
    cfgWrite(0xE2, 0x06), cfgWrite(0xE3, 0x52), cfgWrite(0xE5, 0x1A),
    cfgWrite(0x83, 0xFF), cfgWrite(0x81, 0xFE), cfgWrite(0x85, 0xFF),
    cfgWrite(0x87, 0xFF), cfgWrite(0xC0, 0x0A), cfgWrite(0x60, 0x00),
    cfgWrite(0x40, 0x03), cfgWrite(0x41, 0x00),
};

TEST_CASE("HR_C6000 dmrMode() emits the table 4.1 byte stream in order",
          "[hr_c6000][dmr]")
{
    resetAll();
    C6000.dmrMode();

    REQUIRE(uspi.log.size() == dmrModeStream.size());
    for (size_t i = 0; i < dmrModeStream.size(); i++) {
        INFO("transaction " << i);
        requireTx(uspi.log[i], dmrModeStream[i]);
    }

    /* Every write is its own chip select window, all closed */
    REQUIRE(uspi.csLow == false);
}

TEST_CASE("HR_C6000 DMR register helpers", "[hr_c6000][dmr]")
{
    resetAll();

    C6000.setColorCode(7);
    C6000.setLocalAddress(0x123456);
    C6000.setAccessPolicy(false);
    C6000.setIrqMask(0xFE);
    C6000.clearIrq(0x08);
    C6000.dmrRxStart();
    C6000.dmrIdle();
    C6000.writeAuxRegister(0x54, 0x78);

    std::vector<std::vector<uint8_t>> expected = {
        cfgWrite(0x1F, 0x70), cfgWrite(0x14, 0x56), cfgWrite(0x15, 0x34),
        cfgWrite(0x16, 0x12), cfgWrite(0x20, 0x00), cfgWrite(0x21, 0x00),
        cfgWrite(0x81, 0xFE), cfgWrite(0x83, 0x08), cfgWrite(0x40, 0xC3),
        cfgWrite(0x41, 0x20), cfgWrite(0x41, 0x50), cfgWrite(0x40, 0x03),
        { 0x01, 0x54, 0x78 },
    };

    REQUIRE(uspi.log.size() == expected.size());
    for (size_t i = 0; i < expected.size(); i++) {
        INFO("transaction " << i);
        requireTx(uspi.log[i], expected[i]);
    }

    /* Reads: register, 16-bit address register, RX RAM block */
    uspi.reset();
    uspi.pages[4][0x82] = 0x48;
    uspi.pages[4][0x1A] = 0xAB; /* 0x11A with the 16-bit form */
    for (uint8_t i = 0; i < 12; i++)
        uspi.pages[2][0x00 + i] = (uint8_t)(0xA0 + i);

    REQUIRE(C6000.readIrqFlags() == 0x48);
    requireTx(uspi.log[0], { 0x84, 0x82, 0x00 });

    REQUIRE(C6000.readCfgRegister16(0x11A) == 0xAB);
    requireTx(uspi.log[1], { 0xC4, 0x1A, 0x01, 0x00 });

    uint8_t lc[12];
    C6000.readDataRam(0x00, lc, 12);
    REQUIRE(uspi.log.size() == 3);
    REQUIRE(uspi.log[2].tx.size() == 14);
    REQUIRE(uspi.log[2].tx[0] == 0x82);
    REQUIRE(uspi.log[2].tx[1] == 0x00);
    REQUIRE(uspi.log[2].rxBytes == 12);
    for (uint8_t i = 0; i < 12; i++)
        REQUIRE(lc[i] == (uint8_t)(0xA0 + i));
}

TEST_CASE("dmrbb_configure programs the chip and attaches PC0..PC3",
          "[dmr_cs7000]")
{
    resetAll();
    REQUIRE(dmrbb_init() == 0);
    REQUIRE(vocSpiInits == 1);

    struct dmrbbConfig cfg = defaultConfig();
    dmrbb_configure(&cfg);

    std::vector<std::vector<uint8_t>> expected = dmrModeStream;
    expected.push_back(cfgWrite(0x1F, 0x10));
    expected.push_back(cfgWrite(0x14, 0x4E));
    expected.push_back(cfgWrite(0x15, 0xC3));
    expected.push_back(cfgWrite(0x16, 0x23));
    expected.push_back(cfgWrite(0x20, 0x00));
    expected.push_back(cfgWrite(0x21, 0xA0));

    REQUIRE(uspi.log.size() == expected.size());
    for (size_t i = 0; i < expected.size(); i++) {
        INFO("transaction " << i);
        requireTx(uspi.log[i], expected[i]);
    }

    /* Four EXTI lines on port C, falling edge, inputs first */
    REQUIRE(irqAttachLog.size() == 4);
    for (uint8_t i = 0; i < 4; i++) {
        REQUIRE(irqAttachLog[i].port == &GpioC);
        REQUIRE(irqAttachLog[i].pin == i);
        REQUIRE(irqAttachLog[i].edge == GPIO_IRQ_FALLING);
        REQUIRE(irqAttachLog[i].cb != nullptr);
    }

    unsigned inputs = 0;
    for (const auto &m : gpioModes) {
        if ((m.port == &GpioC) && (m.pin < 4) && (m.mode == INPUT))
            inputs += 1;
    }
    REQUIRE(inputs == 4);

    /* V_SPI chip select set up as an output, high */
    bool vocCsOutput = false;
    for (const auto &m : gpioModes) {
        if ((m.port == &GpioB) && (m.pin == VSPI_CS_PIN) && (m.mode == OUTPUT))
            vocCsOutput = true;
    }
    REQUIRE(vocCsOutput);

    /* Reconfiguring detaches and re-attaches the lines, no duplicate */
    irqAttachLog.clear();
    dmrbb_configure(&cfg);
    REQUIRE(irqDetaches == 4);
    REQUIRE(irqAttachLog.size() == 4);
    REQUIRE(irqAttached.size() == 4);

    dmrbb_terminate();
}

TEST_CASE("dmrbb_configure with a non-default mode word and impolite access",
          "[dmr_cs7000]")
{
    resetAll();
    REQUIRE(dmrbb_init() == 0);

    struct dmrbbConfig cfg = defaultConfig();
    cfg.modeReg = 0x6A;
    cfg.polite = false;
    cfg.colorCode = 15;
    dmrbb_configure(&cfg);

    size_t n = dmrModeStream.size();
    REQUIRE(uspi.log.size() == n + 7);
    requireTx(uspi.log[n], cfgWrite(0x10, 0x6A));
    requireTx(uspi.log[n + 1], cfgWrite(0x1F, 0xF0));
    requireTx(uspi.log[n + 5], cfgWrite(0x20, 0x00));
    requireTx(uspi.log[n + 6], cfgWrite(0x21, 0x00));

    dmrbb_terminate();
}

TEST_CASE("dmrbb slot and mode control emit the documented bytes",
          "[dmr_cs7000]")
{
    bringUp();

    dmrbb_startRx();
    dmrbb_startTx(true);
    dmrbb_startTx(false);
    dmrbb_idle();
    dmrbb_setNextSlot(0x80);
    dmrbb_setNextSlot(0x00);
    dmrbb_setTxFrameType(0x10);
    dmrbb_setTxFrameType(0x08 | (3 << 4));

    std::vector<std::vector<uint8_t>> expected = {
        cfgWrite(0x40, 0xC3), cfgWrite(0x41, 0x20), cfgWrite(0x41, 0x50),
        cfgWrite(0x21, 0xA2), cfgWrite(0x40, 0xE3), cfgWrite(0x21, 0xA2),
        cfgWrite(0x40, 0xC3), cfgWrite(0x40, 0x03), cfgWrite(0x41, 0x80),
        cfgWrite(0x41, 0x00), cfgWrite(0x50, 0x10), cfgWrite(0x50, 0x38),
    };

    REQUIRE(uspi.log.size() == expected.size());
    for (size_t i = 0; i < expected.size(); i++) {
        INFO("transaction " << i);
        requireTx(uspi.log[i], expected[i]);
    }

    /* Impolite access policy in startTx */
    struct dmrbbConfig cfg = defaultConfig();
    cfg.polite = false;
    bringUp(cfg);
    dmrbb_startTx(false);
    REQUIRE(uspi.log.size() == 2);
    requireTx(uspi.log[0], cfgWrite(0x21, 0x02));
    requireTx(uspi.log[1], cfgWrite(0x40, 0xC3));

    dmrbb_terminate();
}

TEST_CASE("dmrbb_writeTxLc writes 12 bytes into TX RAM in one chip select",
          "[dmr_cs7000]")
{
    bringUp();

    uint8_t lc[12];
    for (uint8_t i = 0; i < 12; i++)
        lc[i] = (uint8_t)(0x10 + i);

    dmrbb_writeTxLc(lc);

    REQUIRE(uspi.log.size() == 1);
    std::vector<uint8_t> expected = { 0x02, 0x00 };
    expected.insert(expected.end(), lc, lc + 12);
    requireTx(uspi.log[0], expected);
    REQUIRE(uspi.csLow == false);

    /* Landed at TX RAM 0x00..0x0B */
    for (uint8_t i = 0; i < 12; i++)
        REQUIRE(uspi.pages[2][i] == lc[i]);

    dmrbb_terminate();
}

TEST_CASE("dmrbb_writeVoice uses V_SPI, or the TX RAM when selected",
          "[dmr_cs7000]")
{
    bringUp();

    uint8_t ambe[27];
    for (uint8_t i = 0; i < 27; i++)
        ambe[i] = (uint8_t)(0xC0 + i);

    dmrbb_writeVoice(ambe);

    /* V_SPI: command 0x03, address 0x00, 27 bytes, chip select held low */
    REQUIRE(uspi.log.empty());
    REQUIRE(vspi.log.size() == 1);
    std::vector<uint8_t> expected = { 0x03, 0x00 };
    expected.insert(expected.end(), ambe, ambe + 27);
    requireTx(vspi.log[0], expected);
    REQUIRE(vspi.csLow == false);

    /* Fallback: TX RAM 0x30 over the "user" SPI */
    REQUIRE(dmrbb_readRegister(DMRBB_PAGE_DRIVER, DMRBB_DRV_VOICE_PATH) == 0);
    dmrbb_writeRegister(DMRBB_PAGE_DRIVER, DMRBB_DRV_VOICE_PATH, 1);
    REQUIRE(dmrbb_readRegister(DMRBB_PAGE_DRIVER, DMRBB_DRV_VOICE_PATH) == 1);

    vspi.log.clear();
    dmrbb_writeVoice(ambe);
    REQUIRE(vspi.log.empty());
    REQUIRE(uspi.log.size() == 1);
    expected = { 0x02, 0x30 };
    expected.insert(expected.end(), ambe, ambe + 27);
    requireTx(uspi.log[0], expected);

    dmrbb_writeRegister(DMRBB_PAGE_DRIVER, DMRBB_DRV_VOICE_PATH, 0);
    dmrbb_terminate();
}

TEST_CASE("dmrbb_waitEvent on a SYS event performs the documented reads",
          "[dmr_cs7000]")
{
    bringUp();

    /* What the chip would answer: control frame parsed, LC header */
    uspi.pages[4][0x82] = 0x08;
    uspi.pages[4][0x51] = 0x12; /* type 1, CRC ok, sync class data */
    uspi.pages[4][0x52] = 0x14; /* CC 1, TC = 1 */
    uspi.pages[4][0x5F] = 0x01; /* BS sync */
    uspi.pages[4][0x42] = 0x60;
    uspi.pages[4][0x43] = 0x12;
    uspi.pages[4][0x44] = 0x34;
    uspi.pages[4][0x0F] = 0x05;
    for (uint8_t i = 0; i < 12; i++)
        uspi.pages[2][i] = (uint8_t)(0x50 + i);

    struct dmrbbSnapshot s;
    memset(&s, 0, sizeof(s));

    /* SYS_INTER on the pinmap line (PC1) */
    fireLine(1);
    uint32_t ev = dmrbb_waitEvent(&s, 30);

    REQUIRE(ev == DMRBB_EV_SYS);
    REQUIRE(sleeps == 0);
    REQUIRE(s.irq82 == 0x08);
    REQUIRE(s.r51 == 0x12);
    REQUIRE(s.r52 == 0x14);
    REQUIRE(s.r5f == 0x01);
    REQUIRE(s.r42 == 0x60);
    REQUIRE(s.rssi == 0x1234);
    REQUIRE(s.fskErr == 0x05);
    REQUIRE(s.voiceValid == false);
    REQUIRE(s.sysCount == 1);
    REQUIRE(s.tsCount == 0);
    for (uint8_t i = 0; i < 12; i++)
        REQUIRE(s.lc[i] == (uint8_t)(0x50 + i));

    /*
     * Read order: 0x82, no sub-status (only bit 3 set), 0x51, 0x52, 0x5F,
     * 0x42, RX RAM 12 bytes, no voice, 0x43, 0x44, 0x0F, then 0x83 = 0x08.
     */
    std::vector<std::vector<uint8_t>> expected = {
        { 0x84, 0x82, 0x00 }, { 0x84, 0x51, 0x00 }, { 0x84, 0x52, 0x00 },
        { 0x84, 0x5F, 0x00 }, { 0x84, 0x42, 0x00 },
    };
    REQUIRE(uspi.log.size() == 10);
    for (size_t i = 0; i < expected.size(); i++) {
        INFO("transaction " << i);
        requireTx(uspi.log[i], expected[i]);
    }
    REQUIRE(uspi.log[5].tx.size() == 14);
    REQUIRE(uspi.log[5].tx[0] == 0x82);
    REQUIRE(uspi.log[5].tx[1] == 0x00);
    requireTx(uspi.log[6], { 0x84, 0x43, 0x00 });
    requireTx(uspi.log[7], { 0x84, 0x44, 0x00 });
    requireTx(uspi.log[8], { 0x84, 0x0F, 0x00 });
    requireTx(uspi.log[9], cfgWrite(0x83, 0x08));
    REQUIRE(vspi.log.empty());

    /* Nothing pending any more: timeout after the requested time */
    uspi.log.clear();
    long long before = getTick();
    ev = dmrbb_waitEvent(&s, 30);
    REQUIRE(ev == DMRBB_EV_TIMEOUT);
    REQUIRE(getTick() - before >= 30);
    REQUIRE(uspi.log.empty());

    dmrbb_terminate();
}

TEST_CASE("dmrbb_waitEvent reads the sub-status registers whose bit is set",
          "[dmr_cs7000]")
{
    bringUp();

    uspi.pages[4][0x82] = 0xE6; /* rejected, TX start, TX end, info, abnormal */
    uspi.pages[4][0x84] = 0x80;
    uspi.pages[4][0x86] = 0x81;
    uspi.pages[4][0x90] = 0x82;
    uspi.pages[4][0x98] = 0x03;

    struct dmrbbSnapshot s;
    memset(&s, 0, sizeof(s));
    fireLine(1);
    REQUIRE(dmrbb_waitEvent(&s, 30) == DMRBB_EV_SYS);

    REQUIRE(s.irq82 == 0xE6);
    REQUIRE(s.sub84 == 0x80);
    REQUIRE(s.sub86 == 0x81);
    REQUIRE(s.sub90 == 0x82);
    REQUIRE(s.sub98 == 0x03);

    requireTx(uspi.log[0], { 0x84, 0x82, 0x00 });
    requireTx(uspi.log[1], { 0x84, 0x84, 0x00 });
    requireTx(uspi.log[2], { 0x84, 0x86, 0x00 });
    requireTx(uspi.log[3], { 0x84, 0x90, 0x00 });
    requireTx(uspi.log[4], { 0x84, 0x98, 0x00 });
    requireTx(uspi.log[5], { 0x84, 0x51, 0x00 });

    /* No control frame: the RX RAM is not read */
    for (const auto &t : uspi.log)
        REQUIRE(t.tx[0] != 0x82);

    requireTx(uspi.log.back(), cfgWrite(0x83, 0xE6));

    dmrbb_terminate();
}

TEST_CASE("dmrbb_waitEvent reads the voice payload of a good voice burst",
          "[dmr_cs7000]")
{
    bringUp();

    uspi.pages[4][0x82] = 0x08;
    uspi.pages[4][0x51] = 0x21; /* voice sequence 2, CRC ok, class voice */
    uspi.pages[4][0x52] = 0x10;
    for (uint8_t i = 0; i < 27; i++)
        vspi.pages[3][i] = (uint8_t)(0x80 + i);

    struct dmrbbSnapshot s;
    memset(&s, 0, sizeof(s));
    fireLine(1);
    REQUIRE(dmrbb_waitEvent(&s, 30) == DMRBB_EV_SYS);

    REQUIRE(s.voiceValid == true);
    for (uint8_t i = 0; i < 27; i++)
        REQUIRE(s.voice[i] == (uint8_t)(0x80 + i));

    /* V_SPI: command 0x83, address 0x00, 27 clocks, one chip select */
    REQUIRE(vspi.log.size() == 1);
    REQUIRE(vspi.log[0].tx.size() == 29);
    REQUIRE(vspi.log[0].tx[0] == 0x83);
    REQUIRE(vspi.log[0].tx[1] == 0x00);
    REQUIRE(vspi.log[0].rxBytes == 27);
    REQUIRE(vspi.csLow == false);

    /* The voice read comes after the LC read and before the acknowledge */
    size_t lcIdx = 0;
    size_t ackIdx = 0;
    for (size_t i = 0; i < uspi.log.size(); i++) {
        if (uspi.log[i].tx[0] == 0x82)
            lcIdx = i;
        if ((uspi.log[i].tx[0] == 0x04) && (uspi.log[i].tx[1] == 0x83))
            ackIdx = i;
    }
    REQUIRE(lcIdx < ackIdx);
    requireTx(uspi.log[ackIdx], cfgWrite(0x83, 0x08));

    /* A bad CRC leaves the payload alone */
    uspi.log.clear();
    vspi.log.clear();
    uspi.pages[4][0x51] = 0x25;
    fireLine(1);
    REQUIRE(dmrbb_waitEvent(&s, 30) == DMRBB_EV_SYS);
    REQUIRE(s.voiceValid == false);
    REQUIRE(vspi.log.empty());

    /* RX RAM 0x30 alternative when selected */
    dmrbb_writeRegister(DMRBB_PAGE_DRIVER, DMRBB_DRV_VOICE_PATH, 1);
    uspi.log.clear();
    uspi.pages[4][0x51] = 0x21;
    for (uint8_t i = 0; i < 27; i++)
        uspi.pages[2][0x30 + i] = (uint8_t)(0x40 + i);

    fireLine(1);
    REQUIRE(dmrbb_waitEvent(&s, 30) == DMRBB_EV_SYS);
    REQUIRE(s.voiceValid == true);
    REQUIRE(vspi.log.empty());
    for (uint8_t i = 0; i < 27; i++)
        REQUIRE(s.voice[i] == (uint8_t)(0x40 + i));

    bool ramRead = false;
    for (const auto &t : uspi.log) {
        if ((t.tx[0] == 0x82) && (t.tx[1] == 0x30) && (t.rxBytes == 27))
            ramRead = true;
    }
    REQUIRE(ramRead);

    dmrbb_writeRegister(DMRBB_PAGE_DRIVER, DMRBB_DRV_VOICE_PATH, 0);
    dmrbb_terminate();
}

TEST_CASE("dmrbb_waitEvent on a slot tick reads 0x52 and 0x42 and timestamps",
          "[dmr_cs7000]")
{
    bringUp();

    uspi.pages[4][0x52] = 0x14;
    uspi.pages[4][0x42] = 0x60;

    struct dmrbbSnapshot s;
    memset(&s, 0, sizeof(s));

    fakeTick = 5000;
    fireLine(0);
    fakeTick = 5003;
    REQUIRE(dmrbb_waitEvent(&s, 30) == DMRBB_EV_TS);

    REQUIRE(s.tsCount == 1);
    REQUIRE(s.sysCount == 0);
    REQUIRE(s.tsTick == 5000);
    REQUIRE(s.r52 == 0x14);
    REQUIRE(s.r42 == 0x60);
    REQUIRE(uspi.log.size() == 2);
    requireTx(uspi.log[0], { 0x84, 0x52, 0x00 });
    requireTx(uspi.log[1], { 0x84, 0x42, 0x00 });

    dmrbb_terminate();
}

TEST_CASE("dmrbb_waitEvent coalesces pending events and counts every line",
          "[dmr_cs7000]")
{
    bringUp();

    uspi.pages[4][0x82] = 0x10; /* late entry: LC read, no voice */
    uspi.pages[4][0x51] = 0x00;

    struct dmrbbSnapshot s;
    memset(&s, 0, sizeof(s));

    fireLine(0);
    fireLine(0);
    fireLine(1);
    fireLine(2);
    fireLine(3);
    fireLine(3);
    fireLine(3);

    uint32_t ev = dmrbb_waitEvent(&s, 30);
    REQUIRE(ev == (DMRBB_EV_TS | DMRBB_EV_SYS | DMRBB_EV_RFTX));
    REQUIRE(s.tsCount == 2);
    REQUIRE(s.sysCount == 1);
    REQUIRE(s.rftxCount == 1);
    REQUIRE(s.irq82 == 0x10);

    /* Late entry reads the RX RAM as well */
    bool lcRead = false;
    for (const auto &t : uspi.log) {
        if ((t.tx[0] == 0x82) && (t.tx[1] == 0x00) && (t.rxBytes == 12))
            lcRead = true;
    }
    REQUIRE(lcRead);
    requireTx(uspi.log.back(), cfgWrite(0x83, 0x10));

    /* Per-line counters through the driver pseudo page */
    auto lineCount = [](unsigned n) {
        uint32_t v = 0;
        for (unsigned b = 0; b < 4; b++) {
            v |= (uint32_t)dmrbb_readRegister(DMRBB_PAGE_DRIVER,
                                              DMRBB_DRV_LINE_COUNT(n) + b)
              << (8 * b);
        }
        return v;
    };
    REQUIRE(lineCount(0) == 2);
    REQUIRE(lineCount(1) == 1);
    REQUIRE(lineCount(2) == 1);
    REQUIRE(lineCount(3) == 3);

    /* PC3 alone is counted but raises no event */
    uspi.log.clear();
    fireLine(3);
    REQUIRE(dmrbb_waitEvent(&s, 5) == DMRBB_EV_TIMEOUT);
    REQUIRE(uspi.log.empty());
    REQUIRE(lineCount(3) == 4);

    dmrbb_terminate();
}

TEST_CASE("dmrbb_terminate detaches the lines and idles the chip once",
          "[dmr_cs7000]")
{
    bringUp();

    dmrbb_terminate();
    REQUIRE(irqAttached.empty());
    REQUIRE(irqDetaches == 4);
    REQUIRE(uspi.log.size() == 1);
    requireTx(uspi.log[0], cfgWrite(0x40, 0x03));

    /* Inactive driver: no bus traffic from the slot control functions */
    uspi.log.clear();
    dmrbb_terminate();
    dmrbb_startRx();
    dmrbb_startTx(true);
    dmrbb_idle();
    dmrbb_setNextSlot(0x50);
    dmrbb_setTxFrameType(0x10);
    uint8_t lc[12] = { 0 };
    dmrbb_writeTxLc(lc);
    uint8_t ambe[27] = { 0 };
    dmrbb_writeVoice(ambe);
    REQUIRE(uspi.log.empty());
    REQUIRE(vspi.log.empty());

    /* The raw register access of the monitor screen keeps working */
    uspi.pages[4][0x10] = 0x6E;
    REQUIRE(dmrbb_readRegister(4, 0x10) == 0x6E);
    dmrbb_writeRegister(4, 0x41, 0x20);
    REQUIRE(uspi.pages[4][0x41] == 0x20);
    dmrbb_writeRegister(1, 0x54, 0x78);
    REQUIRE(uspi.pages[1][0x54] == 0x78);
    uspi.pages[2][0x05] = 0x5A;
    REQUIRE(dmrbb_readRegister(2, 0x05) == 0x5A);
}
