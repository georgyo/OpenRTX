/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_BASEBAND_H
#define DMR_BASEBAND_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Platform-independent interface towards the DMR modem ("baseband") of the
 * radio. The modem is a chip doing the 4FSK modulation, the TDMA framing and
 * all the forward error correction of ETSI TS 102 361-1 on its own: the MCU
 * only programs what to do in the next 30 ms timeslot and reads back what was
 * decoded in the previous one. The interface is written around the HR_C6000
 * of the CS7000 and CS7000-PLUS but does not expose anything the state
 * machine (DMR::CallController) could not drive on another Layer-2 modem.
 *
 * Register numbers below refer to the CONFIG page of the HR_C6000, chapter 8
 * of the "HR_C6000 User Manual" (OpenGD77 team translation, OpenRTX-external-
 * docs) and the G4EML register table transcribing it. Sequences are those of
 * manual §5.4.4 (timing) and §5.4.5 (voice transmission and reception).
 * Nothing below has been run on a radio yet: values marked "UNVERIFIED on
 * hardware" come from the manual or from OpenGD77 observation only and are
 * to be confirmed on the CS7000 during bring-up.
 *
 * Threading: every function is called from the rtx thread only. Interrupt
 * handlers never touch the SPI bus: they latch an event, a tick and a
 * counter, and dmrbb_waitEvent() performs the register reads in the
 * caller's context.
 */

/**
 * Events reported by dmrbb_waitEvent(), as a bit mask.
 */
enum dmrbbEvent {
    DMRBB_EV_TS = 1 << 0,      /**< TIME_SLOT_INTER: a 30 ms slot started.
                                    HR_C6000 manual §4.4 / §5.4.4, pin PC0. */
    DMRBB_EV_SYS = 1 << 1,     /**< SYS_INTER: register 0x82 has flags.
                                    Manual §5.4.5, 4 ms after the slot end. */
    DMRBB_EV_RFTX = 1 << 2,    /**< RF_TX_INTER: own TX burst is imminent,
                                    0x12[5:0] x 100 us in advance. */
    DMRBB_EV_TIMEOUT = 1 << 3, /**< Nothing happened within the timeout. */
};

/**
 * Snapshot of the modem state, filled by dmrbb_waitEvent(). Register reads
 * happen only when the corresponding interrupt bit is set, the other fields
 * keep the value of the previous snapshot.
 */
struct dmrbbSnapshot {
    /*
     * Register 0x82, Layer-2 interrupt flags (G4EML table; manual §5.4.5):
     * bit7 TX request rejected (channel busy per 0x20/0x21 access policy),
     * bit6 TX started (sub-status 0x84), bit5 TX ended (sub-status 0x86),
     * bit4 late entry / post-access into an ongoing call, bit3 control
     * frame parsed (LC/CSBK/EMB in RX RAM, see 0x51/0x52), bit2 service
     * data message received (sub-status 0x90), bit1 abnormal voice exit
     * (sub-status 0x98[2:0]), bit0 PHY-only receive (test mode).
     */
    uint8_t irq82;
    uint8_t sub84; /**< 0x84, TX start sub-status, read when 0x82 bit6 */
    uint8_t sub86; /**< 0x86, TX end sub-status, read when 0x82 bit5   */
    uint8_t sub90; /**< 0x90, data message status, read when 0x82 bit2 */
    uint8_t sub98; /**< 0x98, abnormal exit cause, read when 0x82 bit1 */

    /*
     * Register 0x51, DLLRecvDataType: [7:4] data type (TS 102 361-1 Table
     * 9.22) when the sync class is data, or voice frame sequence 1..6 (A..F)
     * when the sync class is voice; bit3 received PI; bit2 CRC bad; [1:0]
     * sync class: 00 sync header, 01 voice, 10 data, 11 RC.
     */
    uint8_t r51;

    /*
     * Register 0x52: [7:4] received colour code, bit3 CACH AT, bit2 CACH TC
     * (timeslot: 0 = TS1, 1 = TS2), [1:0] received LCSS. Read on every TS
     * event for the timecode tracking and on SYS events for the filters.
     */
    uint8_t r52;

    /*
     * Register 0x5F [1:0], received sync type: 00 MS (direct mode), 01 BS
     * (repeater), 10 TDMA TS1 (DMO), 11 TDMA TS2 (DMO).
     */
    uint8_t r5f;

    /*
     * Register 0x42 [7:5], slot status read in the TS event: 001 working
     * slot with TX/RX off, 101 TX enabled, 011 RX enabled, xx0 non-working
     * slot. Diagnostic only.
     */
    uint8_t r42;

    /*
     * RX RAM bytes 0x00-0x0B (U_SPI page DATA, read opcode 0x82): the 9 LC
     * or 10 CSBK octets followed by the parity/CRC. Read when 0x82 bit3 or
     * bit4 is set. UNVERIFIED on hardware: the first read after a mode
     * switch may return the previous call's LC (OpenGD77 observation), the
     * state machine skips it.
     */
    uint8_t lc[12];

    /*
     * Voice payload of the burst, 27 bytes (216 bits, three AMBE+2 frames,
     * TS 102 361-1 §6.1), read over V_SPI with command 0x83 when the sync
     * class is voice and the CRC is good. voiceValid is false otherwise.
     * UNVERIFIED on hardware: V_SPI (0x06 = 0x21) versus RX RAM 0x30.
     */
    uint8_t voice[27];
    bool voiceValid;

    uint16_t rssi;      /**< Registers 0x43:0x44, read ~15 ms after the TS */
    uint8_t fskErr;     /**< Register 0x0F, FSK error count per slot       */

    uint32_t tsCount;   /**< TIME_SLOT_INTER counter (monitor screen)  */
    uint32_t sysCount;  /**< SYS_INTER counter                          */
    uint32_t rftxCount; /**< RF_TX_INTER counter                        */
    uint32_t tsTick;    /**< getTick() latched by the TS interrupt      */
};

/**
 * Static configuration of the modem, applied by dmrbb_configure().
 */
struct dmrbbConfig {
    uint8_t colorCode; /**< Colour code 0-15, register 0x1F [7:4]          */
    uint8_t timeslot;  /**< Timeslot 1 or 2, kept for the driver's record  */
    uint32_t ownId;    /**< Own DMR ID, registers 0x14-0x16 (UNVERIFIED on
                            hardware: G4EML says L/M/H byte order, the MD-3x0
                            OEM capture looks H/M/L; harmless because
                            address matching is done on the MCU)           */
    bool polite;       /**< Channel access: 1 polite to own colour code
                            (0x21 = 0xA0), 0 impolite (0x21 = 0x00)        */
    uint8_t modeReg;   /**< Value for register 0x10, default 0x6E: DMR,
                            Tier II, timeslot mode, Layer 2, repeater,
                            aligned (G4EML bit map; manual §5.4.3 gives
                            0x68/0x6A). UNVERIFIED on hardware, selectable
                            from the monitor screen                       */
};

/**
 * Default value for dmrbbConfig::modeReg, register 0x10: DMR, Tier II,
 * timeslot mode, Layer 2, repeater, aligned (G4EML bit map; manual §5.4.3
 * gives 0x68/0x6A). UNVERIFIED on hardware, selectable from the monitor
 * screen once it exists.
 */
#define DMRBB_MODE_REG_DEFAULT 0x6E

/**
 * Initialise the DMR modem driver: interrupt lines, ISR mailbox, SPI ports.
 * Does not put the chip into DMR mode, see dmrbb_configure().
 *
 * @return 0 on success, -1 on targets without a DMR modem (stub driver):
 * the caller keeps the DMR operating mode off and shows a marker.
 */
int dmrbb_init(void);

/**
 * Shut down the DMR modem driver: mask the interrupt lines, idle the chip
 * (register 0x40 = 0x03) and release the ISR mailbox.
 */
void dmrbb_terminate(void);

/**
 * Put the chip in DMR mode and program the static registers: mode 0x10,
 * colour code 0x1F, own ID 0x14-0x16, access policy 0x20/0x21, sync enables
 * 0x5F = 0xF0, data type coding 0x4B/0x4C, TX advance 0x2E = 0x04, interrupt
 * masks 0x81/0x85/0x87 (full table in the CS7000 driver). Called when the
 * DMR operating mode is entered and whenever the configuration changes;
 * leaves 0x40 = 0x03 (idle) and 0x41 = 0x00 until dmrbb_startRx().
 *
 * @param cfg: modem configuration.
 */
void dmrbb_configure(const struct dmrbbConfig *cfg);

/**
 * Start receiving with passive timing derived from the received sync:
 * register 0x40 = 0xC3 (TxEn + RxEn, passive), then 0x41 = 0x20 (SyncFail:
 * force a new sync search) and 0x41 = 0x50 (RxNxtSlotEn + begin_v_layer2,
 * voice routed to the vocoder port). Manual §5.4.5 "voice reception" gives
 * 0x40 = 0x43 / 0x41 = 0x40 (blind RX) as the minimal form; UNVERIFIED on
 * hardware which of 0x50 and 0x40 the CS7000 needs, see DMR::R41_RX.
 */
void dmrbb_startRx(void);

/**
 * Prepare the chip for transmission: register 0x21 = 0xA2 (polite to own
 * colour code, clear the vocoder encode buffer) or 0x02 (impolite) per the
 * configured policy, then 0x40 = 0xE3 (TxEn + RxEn, active timing: direct
 * mode, the chip generates the 30 ms time axis) or 0x40 = 0xC3 (passive
 * timing: repeater mode, bursts aligned to the received sync). Manual
 * §5.4.5 "voice transmission" uses 0xA3; UNVERIFIED on hardware.
 *
 * @param activeTiming: true for DMO (own timing), false for RMO.
 */
void dmrbb_startTx(bool activeTiming);

/**
 * Stop both directions: register 0x40 = 0x03 (TxEn and RxEn cleared, normal
 * mode). The TS interrupts stop.
 */
void dmrbb_idle(void);

/**
 * Program what the chip does in the NEXT timeslot, register 0x41. Must be
 * written within t2 = 27 ms of the TS event (manual Figure 5.12): 0x00
 * nothing, 0x50 receive (see dmrbb_startRx), 0x80 transmit the frame
 * described by 0x50, 0x20 drop sync and search again.
 *
 * @param r41: value for register 0x41, see the DMR::R41_* constants.
 */
void dmrbb_setNextSlot(uint8_t r41);

/**
 * Set the type of the frame transmitted in the next slot, register 0x50
 * (LocalDataType, manual Table 5.5): [7:4] data type, bit3 voice, [1:0]
 * LCSS. 0x10 voice LC header, 0x20 terminator with LC, 0x30 CSBK, 0x08 |
 * (n << 4) voice burst n = 0..5 (A..F). UNVERIFIED on hardware: whether
 * the chip fills the embedded LC fragments itself when LCSS is left at 00
 * (OpenGD77 practice) or needs the manual's 0x08/0x19/0x2B/0x3B/0x4A/0x58
 * sequence, see DMR::r50Voice().
 *
 * @param r50: value for register 0x50, see the DMR::R50_* constants.
 */
void dmrbb_setTxFrameType(uint8_t r50);

/**
 * Write the 12 octet link control (or CSBK) for the next header/terminator
 * /CSBK burst into TX RAM 0x00-0x0B, U_SPI page DATA (opcode 0x02), manual
 * Table 5.2. The chip appends the RS(12,9) or CRC-CCITT parity itself when
 * register 0x40 bit3 is 0 (UNVERIFIED on hardware).
 *
 * @param lc: 12 bytes, 9 LC octets (or 10 CSBK octets) followed by zeros.
 */
void dmrbb_writeTxLc(const uint8_t lc[12]);

/**
 * Write the 27 byte voice payload of the next own voice burst, V_SPI
 * command 0x03 address 0x00 (manual §5.2, register 0x06 = 0x21), or TX RAM
 * 0x30-0x4A as the fallback (UNVERIFIED on hardware). Written before
 * dmrbb_setNextSlot(0x80) in the same TS pass.
 *
 * @param ambe: 27 bytes, three AMBE+2 frames.
 */
void dmrbb_writeVoice(const uint8_t ambe[27]);

/**
 * Block until the modem raises an event or the timeout expires, then read
 * the registers associated with the event (0x82 -> 0x51/0x52/0x5F -> RX RAM
 * -> V_SPI, then 0x83 = flags to acknowledge; 0x52/0x42 on TS) into the
 * snapshot. All SPI traffic happens in the calling thread.
 *
 * @param s: snapshot filled with the event data.
 * @param timeoutMs: maximum wait, in milliseconds.
 * @return bit mask of dmrbbEvent, DMRBB_EV_TIMEOUT when nothing arrived.
 */
uint32_t dmrbb_waitEvent(struct dmrbbSnapshot *s, unsigned timeoutMs);

/**
 * Raw register access for the DMR monitor screen only.
 *
 * @param page: U_SPI page (1 AUX, 4 CONFIG, ...).
 * @param addr: register address.
 * @return register value.
 */
uint8_t dmrbb_readRegister(uint8_t page, uint8_t addr);

/**
 * Raw register write for the DMR monitor screen only.
 *
 * @param page: U_SPI page.
 * @param addr: register address.
 * @param v: value to write.
 */
void dmrbb_writeRegister(uint8_t page, uint8_t addr, uint8_t v);

#ifdef __cplusplus
}
#endif

#endif /* DMR_BASEBAND_H */
