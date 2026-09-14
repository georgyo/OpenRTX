/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_MODEMPORT_H
#define DMR_MODEMPORT_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <cstdint>
#include "protocols/DMR/Constants.hpp"

namespace DMR
{

/*
 * Register values the call controller programs into the modem, kept in one
 * place so that they can be retuned during the hardware bring-up without
 * touching the state machine. Sources: "HR_C6000 User Manual" (OpenGD77 team
 * translation, OpenRTX-external-docs), chapter 8 register list and §5.4.5
 * application examples; "G4EML register table"; OpenGD77 observation is used
 * as confirmation only. Nothing has been run on a CS7000 yet.
 */

/*
 * Register 0x40, TX/RX enable and timing (G4EML; manual §5.4.4): bit7 TxEn,
 * bit6 RxEn, bit5 MasterMode (1 = active, own time axis; 0 = passive, time
 * axis derived from the received sync), bit3 1 = MCU computes the CRCs,
 * [1:0] 11 = normal operation.
 */
static constexpr uint8_t R40_IDLE = 0x03;   /* both directions off        */
static constexpr uint8_t R40_RX = 0xC3;     /* TX+RX enabled, passive     */
static constexpr uint8_t R40_TX_RMO = 0xC3; /* RMO TX: passive, aligned to
                                               the repeater (OpenGD77;
                                               UNVERIFIED on hardware)     */
static constexpr uint8_t R40_TX_DMO = 0xE3; /* DMO TX: active timing
                                               (OpenGD77; manual §5.4.5
                                               uses 0xA3; UNVERIFIED on
                                               hardware)                   */

/*
 * Register 0x41, what to do in the next slot (G4EML; manual §5.4.5): bit7
 * TxNxtSlotEn, bit6 RxNxtSlotEn, bit5 SyncFail (drop the sync and search
 * again), bit4 begin_v_layer2 (route the received voice to the vocoder
 * port), bit3 CC_Match_Ctrl (chip-side colour code gating, not used: the
 * MCU filters).
 */
static constexpr uint8_t R41_IDLE = 0x00;   /* nothing in the next slot   */
static constexpr uint8_t R41_RX = 0x50;     /* receive + voice to vocoder.
                                               UNVERIFIED on hardware: the
                                               manual's minimal form is
                                               0x40 (blind reception),
                                               OpenGD77 uses 0x50         */
static constexpr uint8_t R41_TX = 0x80;     /* transmit the 0x50 frame    */
static constexpr uint8_t R41_RESYNC = 0x20; /* force a new sync search    */

/*
 * Register 0x50, LocalDataType of the next TX burst (manual Table 5.5;
 * G4EML): [7:4] data type of TS 102 361-1 Table 9.22, bit3 voice burst,
 * bit2 data header flag, [1:0] LCSS of TS 102 361-1 Table 9.20.
 */
static constexpr uint8_t R50_VOICE_LC_HEADER = 0x10; /* type 1, LCSS 00  */
static constexpr uint8_t R50_TERMINATOR = 0x20;      /* type 2, LCSS 00  */
static constexpr uint8_t R50_CSBK = 0x30;            /* type 3, LCSS 00  */
static constexpr uint8_t R50_VOICE = 0x08;           /* bit3: voice      */

/**
 * Register 0x50 value for voice burst 'seq' (0 = A ... 5 = F): 0x08 |
 * (seq << 4) with LCSS left at 00, the OpenGD77 convention under which the
 * chip fills the embedded LC on its own. UNVERIFIED on hardware: the manual
 * §5.4.5 example writes 0x08, 0x19, 0x2B, 0x3B, 0x4A, 0x58, i.e. LCSS 00,
 * 01, 11, 11, 10, 00 following the fragment order of TS 102 361-1 §9.3.3;
 * if the hotspot does not decode the embedded LC during bring-up, change
 * this one function.
 *
 * @param seq: voice burst index, 0 to 5.
 * @return value for register 0x50.
 */
static inline uint8_t r50Voice(uint8_t seq)
{
    return R50_VOICE | (uint8_t)((seq % SUPERFRAME_BURST) << 4);
}

/*
 * Register 0x21, LocalAccessPolicy1 (G4EML): [7:4] access policy for the
 * two call types, 00 impolite / 01 polite to all / 10 polite to own colour
 * code; bit1 clears the vocoder encode buffer (self-resetting), bit0 the
 * decode buffer. OpenGD77 writes 0xA0 at init and 0xA2 before every TX.
 * UNVERIFIED on hardware.
 */
static constexpr uint8_t R21_POLITE = 0xA2;   /* polite to CC, clear buf */
static constexpr uint8_t R21_IMPOLITE = 0x02; /* impolite, clear buffer   */

/*
 * Register 0x82 interrupt flags (G4EML; manual §5.4.5).
 */
static constexpr uint8_t R82_TX_REJECTED = 0x80;   /* channel busy       */
static constexpr uint8_t R82_TX_START = 0x40;      /* sub-status 0x84    */
static constexpr uint8_t R82_TX_END = 0x20;        /* sub-status 0x86    */
static constexpr uint8_t R82_LATE_ENTRY = 0x10;    /* post-access LC     */
static constexpr uint8_t R82_CTRL_FRAME = 0x08;    /* LC/CSBK/EMB parsed */
static constexpr uint8_t R82_DATA_MSG = 0x04;      /* sub-status 0x90    */
static constexpr uint8_t R82_ABNORMAL_EXIT = 0x02; /* sub-status 0x98    */
static constexpr uint8_t R82_PHY_RX = 0x01;        /* test mode only     */

/*
 * Register 0x51 fields (G4EML).
 */
static constexpr uint8_t R51_CRC_BAD = 0x04;
static constexpr uint8_t R51_SYNC_CLASS_MASK = 0x03;
static constexpr uint8_t R51_CLASS_HEADER = 0x00; /* data burst with sync */
static constexpr uint8_t R51_CLASS_VOICE = 0x01;  /* voice burst          */
static constexpr uint8_t R51_CLASS_DATA = 0x02;   /* data burst           */
static constexpr uint8_t R51_CLASS_RC = 0x03;     /* reverse channel      */

/*
 * Register 0x52 fields (G4EML): [7:4] colour code, bit3 CACH AT, bit2 CACH
 * TC (0 = TS1, 1 = TS2), [1:0] LCSS.
 */
static constexpr uint8_t R52_TC = 0x04;
static constexpr uint8_t R52_AT = 0x08;

/*
 * Register 0x5F [1:0], received sync type (G4EML).
 */
static constexpr uint8_t R5F_SYNC_MASK = 0x03;
static constexpr uint8_t R5F_SYNC_MS = 0x00;    /* MS sourced, direct mode */
static constexpr uint8_t R5F_SYNC_BS = 0x01;    /* BS sourced, repeater    */
static constexpr uint8_t R5F_SYNC_TDMA1 = 0x02; /* DMO timeslot 1          */
static constexpr uint8_t R5F_SYNC_TDMA2 = 0x03; /* DMO timeslot 2          */

/**
 * Abstract port towards the DMR modem, the only thing the call controller
 * talks to. The firmware adapter forwards the calls to the dmr_baseband.h C
 * API; the unit tests inject a recording mock.
 *
 * The compound operations startRx(), startTx() and idle() have default
 * implementations expressed through the register-level primitives, so that
 * a mock records the exact register values; an adapter may override them
 * with the driver's own sequence.
 */
class ModemPort
{
public:
    virtual ~ModemPort() = default;

    /**
     * Program register 0x41 for the next slot, see dmrbb_setNextSlot().
     *
     * @param r41: one of R41_IDLE, R41_RX, R41_TX, R41_RESYNC.
     */
    virtual void setNextSlot(uint8_t r41) = 0;

    /**
     * Program register 0x50 for the next TX burst, see dmrbb_setTxFrameType().
     *
     * @param r50: one of the R50_* values or r50Voice().
     */
    virtual void setTxFrameType(uint8_t r50) = 0;

    /**
     * Write register 0x40, see the R40_* values.
     *
     * @param r40: register value.
     */
    virtual void setMode(uint8_t r40) = 0;

    /**
     * Write register 0x21, see the R21_* values.
     *
     * @param r21: register value.
     */
    virtual void setAccess(uint8_t r21) = 0;

    /**
     * Write the 12 octet LC/CSBK to TX RAM, see dmrbb_writeTxLc().
     *
     * @param lc: 12 bytes.
     */
    virtual void writeTxLc(const uint8_t lc[CHIP_LC_BYTES]) = 0;

    /**
     * Write the 27 byte voice payload of the next burst, see
     * dmrbb_writeVoice().
     *
     * @param ambe: 27 bytes.
     */
    virtual void writeVoice(const uint8_t ambe[VOICE_PAYLOAD_BYTES]) = 0;

    /**
     * Enter reception with passive timing: 0x40 = R40_RX, 0x41 = R41_RESYNC
     * then R41_RX (manual §5.4.5, OpenGD77 sequence). See dmrbb_startRx().
     */
    virtual void startRx()
    {
        setMode(R40_RX);
        setNextSlot(R41_RESYNC);
        setNextSlot(R41_RX);
    }

    /**
     * Enter transmission: 0x40 = R40_TX_DMO (active timing) or R40_TX_RMO
     * (passive timing). The access policy (0x21) is written separately by
     * the controller through setAccess(). See dmrbb_startTx().
     *
     * @param activeTiming: true for DMO, false for RMO.
     */
    virtual void startTx(bool activeTiming)
    {
        setMode(activeTiming ? R40_TX_DMO : R40_TX_RMO);
    }

    /**
     * Stop both directions: 0x40 = R40_IDLE. See dmrbb_idle().
     */
    virtual void idle()
    {
        setMode(R40_IDLE);
    }
};

} /* namespace DMR */

#endif /* DMR_MODEMPORT_H */
