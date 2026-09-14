/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include "emulator.h"
#include "interfaces/delays.h"
#include "protocols/DMR/CallController.hpp"
#include "protocols/DMR/Constants.hpp"
#include "protocols/DMR/ModemPort.hpp"
#include "rtx/OpMode_DMR.hpp"
#include "rtx/rtx.h"

extern "C" {
#include "core/state.h"
}

using namespace DMR;

/*
 * Headless test of the OpMode_DMR handler on the Linux emulator, in the
 * style of M17_opmode_ptt.cpp: the fake modem of dmr_linux.cpp is scripted
 * from here (no clock thread runs), PTT is driven through the
 * emulator_state.PTTstatus flag read by platform_getPttStatus() and the
 * handler is stepped with update() as rtx_task() would do.
 */

static const uint32_t OWN_ID = 2345678;
static const uint32_t TALKGROUP = 31665;
static const uint32_t CALLER = 1234567;

// Statically allocated to match its usage in rtx.cpp
static OpMode_DMR dmr;

static rtxStatus_t initialStatus()
{
    rtxStatus_t status;
    memset(&status, 0x00, sizeof(status));
    status.opMode = OPMODE_DMR;
    status.opStatus = OFF;
    status.txDisable = 0;
    status.rxFrequency = 430000000;
    status.txFrequency = 430000000;
    status.dmr_srcId = OWN_ID;
    status.dmr_dstId = TALKGROUP;
    status.dmr_callType = GROUP;
    status.dmr_rxColorCode = 1;
    status.dmr_txColorCode = 1;
    status.dmr_timeslot = 2;
    status.dmr_monitor = 0;
    status.dmr_polite = 1;

    return status;
}

/*
 * Test bench: a fresh fake modem, PTT released, the handler disabled at the
 * end so that the next test starts clean.
 */
class Bench
{
public:
    rtxStatus_t status;
    const struct dmrEmuRecord *rec;

    Bench()
    {
        dmrEmu_reset();
        emulator_state.PTTstatus = false;
        state.settings.dmr_hangTime = 0;
        status = initialStatus();
        rec = dmrEmu_record();
    }

    ~Bench()
    {
        emulator_state.PTTstatus = false;
        dmr.disable();
    }

    /* enable() then the first update() with the configuration */
    void start()
    {
        dmr.enable();
        dmr.update(&status, true);
    }

    /* One idle timeslot of the fake modem, then one handler step */
    void tick()
    {
        dmrEmu_pushTs();
        dmr.update(&status, false);
    }

    void ticks(unsigned n)
    {
        for (unsigned i = 0; i < n; i++)
            tick();
    }

    /* Handler steps without modem events */
    void run(unsigned n)
    {
        for (unsigned i = 0; i < n; i++)
            dmr.update(&status, false);
    }

    /* Play the scripted events until the controller reaches 'target' */
    bool runUntil(CallController::State target, unsigned limit = 400)
    {
        for (unsigned i = 0; i < limit; i++) {
            if (dmr.controller().state() == target)
                return true;
            dmr.update(&status, false);
        }
        return dmr.controller().state() == target;
    }

    /* Play every scripted event */
    void drain()
    {
        while (dmrEmu_pending() > 0)
            dmr.update(&status, false);
    }

    CallController::State ctrlState() const
    {
        return dmr.controller().state();
    }
};

TEST_CASE("DMR OpMode: sizes", "[dmr][rtx]")
{
    /* The handler is a static object: nothing of it lives on the 512 byte
     * rtx thread stack, update() keeps a handful of scalars there. */
    printf("sizeof(OpMode_DMR) = %zu bytes\n", sizeof(OpMode_DMR));
    printf("sizeof(DMR::CallController) = %zu bytes\n", sizeof(CallController));
    printf("sizeof(struct dmrbbSnapshot) = %zu bytes\n",
           sizeof(struct dmrbbSnapshot));
    REQUIRE(sizeof(OpMode_DMR) < 512);
}

TEST_CASE("DMR OpMode: enable goes OFF -> RX and ticks to RX_IDLE",
          "[dmr][rtx]")
{
    Bench b;
    REQUIRE(b.ctrlState() == CallController::OFF);

    b.start();
    REQUIRE(dmr.modemReady() == true);
    REQUIRE(b.rec->initCount == 1);
    REQUIRE(b.rec->configureCount == 1);
    REQUIRE(b.rec->config.colorCode == 1);
    REQUIRE(b.rec->config.timeslot == 2);
    REQUIRE(b.rec->config.ownId == OWN_ID);
    REQUIRE(b.rec->config.polite == true);
    REQUIRE(b.rec->config.modeReg == DMRBB_MODE_REG_DEFAULT);
    REQUIRE(b.rec->startRxCount == 1);
    REQUIRE(b.rec->r40 == R40_RX);
    REQUIRE(b.rec->r41 == R41_RX);

    /* OFF -> RX_SEARCH, RX status published, nothing received yet */
    REQUIRE(b.ctrlState() == CallController::RX_SEARCH);
    REQUIRE(b.status.opStatus == RX);
    REQUIRE(b.status.dmr_callState == DMR_CALL_IDLE);
    REQUIRE(b.status.dmr_slotLock == 0);
    REQUIRE(b.status.dmr_lcOk == false);
    REQUIRE(b.status.dmr_markers == 0);
    REQUIRE(dmr.rxSquelchOpen() == false);

    /* Without modem events the handler idles in RX */
    b.run(3);
    REQUIRE(b.ctrlState() == CallController::RX_SEARCH);
    REQUIRE(b.rec->timeouts == 4);

    /* The first timeslot tick: RX_IDLE, slots ticking */
    b.tick();
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);
    REQUIRE(b.status.dmr_slotLock == 1);
    REQUIRE(b.rec->tsDelivered == 1);

    /* Timecode locked after five agreeing slots */
    b.ticks(5);
    REQUIRE(b.status.dmr_slotLock == 2);
    REQUIRE(b.status.opStatus == RX);
    REQUIRE(b.status.dmr_callState == DMR_CALL_IDLE);

    /* Every tick re-arms reception of the next slot */
    REQUIRE(b.rec->r41 == R41_RX);
    REQUIRE(b.rec->startTxCount == 0);
}

TEST_CASE("DMR OpMode: disable idles the modem", "[dmr][rtx]")
{
    Bench b;
    b.start();
    b.ticks(6);
    uint32_t timeouts = b.rec->timeouts;

    dmr.disable();
    REQUIRE(b.ctrlState() == CallController::OFF);
    REQUIRE(b.rec->idleCount == 1);
    REQUIRE(b.rec->terminateCount == 1);
    REQUIRE(b.rec->r40 == R40_IDLE);
    REQUIRE(dmr.modemReady() == false);
    REQUIRE(dmr.rxSquelchOpen() == false);

    /* update() after disable() is a no-op: the modem is not even polled */
    b.run(2);
    REQUIRE(b.ctrlState() == CallController::OFF);
    REQUIRE(b.rec->timeouts == timeouts);
}

TEST_CASE("DMR OpMode: PTT with TX disabled does not leave RX", "[dmr][rtx]")
{
    Bench b;
    b.start();
    b.ticks(6);
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);

    /* PTT pressed while the UI is in a menu (txDisable set): stay in RX */
    b.status.txDisable = 1;
    emulator_state.PTTstatus = true;
    b.ticks(4);
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);
    REQUIRE(b.status.opStatus == RX);
    REQUIRE(b.status.dmr_callState == DMR_CALL_IDLE);
    REQUIRE(b.rec->startTxCount == 0);
    REQUIRE(b.rec->txSlots == 0);
    REQUIRE(b.status.dmr_markers == 0);

    /* PTT released, then back to the main screen: still RX */
    emulator_state.PTTstatus = false;
    b.ticks(2);
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);

    b.status.txDisable = 0;
    b.ticks(2);
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);
    REQUIRE(b.rec->startTxCount == 0);
}

TEST_CASE("DMR OpMode: PTT without a DMR ID stays RX with the marker",
          "[dmr][rtx]")
{
    Bench b;
    b.status.dmr_srcId = 0;
    b.start();
    b.ticks(6);
    REQUIRE(b.rec->config.ownId == 0);

    emulator_state.PTTstatus = true;
    b.ticks(3);
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);
    REQUIRE(b.status.opStatus == RX);
    REQUIRE(b.status.dmr_callState == DMR_CALL_IDLE);
    REQUIRE(b.rec->startTxCount == 0);
    REQUIRE(b.rec->txSlots == 0);
    REQUIRE((b.status.dmr_markers & DMR_MARK_NO_ID) != 0);
    REQUIRE((b.status.dmr_markers & DMR_MARK_NOT_SUPPORTED) == 0);

    /* Holding the PTT changes nothing, the marker stays up */
    b.ticks(3);
    REQUIRE((b.status.dmr_markers & DMR_MARK_NO_ID) != 0);
    REQUIRE(b.rec->startTxCount == 0);

    /* Released: the marker is kept for a while */
    emulator_state.PTTstatus = false;
    b.ticks(2);
    REQUIRE((b.status.dmr_markers & DMR_MARK_NO_ID) != 0);

    /* An ID is configured: the modem is reprogrammed with it */
    b.status.dmr_srcId = OWN_ID;
    dmr.update(&b.status, true);
    REQUIRE(b.rec->configureCount == 2);
    REQUIRE(b.rec->config.ownId == OWN_ID);
    b.ticks(6);
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);

    /* The next press clears the marker and transmits */
    emulator_state.PTTstatus = true;
    b.tick();
    REQUIRE(b.ctrlState() >= CallController::TX_ARM);
    REQUIRE(b.status.dmr_markers == 0);
    REQUIRE(b.rec->startTxCount == 1);
}

TEST_CASE("DMR OpMode: PTT with a valid ID transmits and returns to RX",
          "[dmr][rtx]")
{
    Bench b;
    b.start();
    b.ticks(6);
    REQUIRE(b.status.dmr_slotLock == 2);

    /* Press: simplex channel, no carrier -> direct mode, active timing */
    emulator_state.PTTstatus = true;
    b.tick();
    REQUIRE(b.ctrlState() >= CallController::TX_ARM);
    REQUIRE(b.status.opStatus == TX);
    REQUIRE(b.status.dmr_callState == DMR_CALL_TX);
    REQUIRE(b.rec->startTxCount == 1);
    REQUIRE(b.rec->activeTiming == true);
    REQUIRE(b.rec->r40 == R40_TX_DMO);
    REQUIRE(b.rec->r21 == R21_POLITE);
    REQUIRE(dmr.rxSquelchOpen() == false);

    /* Three voice LC headers on the own slot */
    unsigned n = 0;
    while (b.ctrlState() != CallController::TX_VOICE) {
        b.tick();
        REQUIRE(++n < 10);
    }
    REQUIRE(b.rec->headers == 3);
    REQUIRE(b.rec->lcWrites == 3);
    REQUIRE(b.rec->txSlots == 3);

    const uint8_t expectedLc[CHIP_LC_BYTES] = { 0x00, 0x00, 0x00, 0x00,
                                                0x7B, 0xB1, 0x23, 0xCA,
                                                0xCE, 0x00, 0x00, 0x00 };
    REQUIRE(memcmp(b.rec->lc, expectedLc, CHIP_LC_BYTES) == 0);

    /* Voice: one burst every other slot, silence from the audio stub */
    b.ticks(24);
    REQUIRE(b.ctrlState() == CallController::TX_VOICE);
    REQUIRE(b.rec->voiceBursts == 12);
    REQUIRE(b.rec->voiceWrites == 12);
    REQUIRE(dmr.audio().txRequests == 12);
    REQUIRE(memcmp(b.rec->voice, AMBE_SILENCE, VOICE_PAYLOAD_BYTES) == 0);
    REQUIRE(b.rec->r50 == r50Voice(5));
    REQUIRE(b.status.opStatus == TX);
    REQUIRE(b.status.dmr_callState == DMR_CALL_TX);

    /* Release: the superframe is completed, then one terminator */
    emulator_state.PTTstatus = false;
    n = 0;
    while (b.ctrlState() != CallController::TX_TERM) {
        b.tick();
        REQUIRE(++n < 14);
    }
    REQUIRE(b.rec->terminators == 1);
    REQUIRE(b.rec->voiceBursts == 12);

    /* Terminator out: back to RX, hang time zero -> idle at once */
    b.tick();
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);
    REQUIRE(b.rec->startRxCount == 2);
    REQUIRE(b.rec->r40 == R40_RX);
    REQUIRE(b.status.opStatus == RX);
    REQUIRE(b.status.dmr_callState == DMR_CALL_IDLE);
    REQUIRE(b.status.dmr_lcOk == false);
    REQUIRE(b.status.dmr_markers == 0);
    REQUIRE(b.rec->terminators == 1);

    b.ticks(4);
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);
    REQUIRE(b.rec->txSlots == 16);
}

TEST_CASE("DMR OpMode: an injected group call is received and published",
          "[dmr][rtx]")
{
    Bench b;
    state.settings.dmr_hangTime = 1;
    b.start();
    b.ticks(6);

    dmrEmu_injectGroupCall(CALLER, TALKGROUP, 1, 2, 12);
    REQUIRE(dmrEmu_pending() > 0);

    /* The headers open the call */
    REQUIRE(b.runUntil(CallController::RX_CALL));
    REQUIRE(b.status.dmr_callState == DMR_CALL_RX);
    REQUIRE(b.status.dmr_lcOk == true);
    REQUIRE(b.status.dmr_rxSrcId == CALLER);
    REQUIRE(b.status.dmr_rxDstId == TALKGROUP);
    REQUIRE(b.status.dmr_rxFlco == FLCO_GRP_V_CH_USR);
    REQUIRE(b.status.dmr_rxColorCodeSeen == 1);
    REQUIRE(b.status.dmr_rxTimeslot == 2);
    REQUIRE(b.status.dmr_rxSyncType == 1);
    REQUIRE(b.status.opStatus == RX);
    REQUIRE(dmr.rxSquelchOpen() == true);

    /* Voice bursts reach the audio placeholder, the terminator ends it */
    REQUIRE(b.runUntil(CallController::RX_HANG));
    REQUIRE(dmr.audio().rxFrames == 12);
    REQUIRE(b.status.dmr_callState == DMR_CALL_RX_HANG);
    REQUIRE(b.status.dmr_lcOk == true);
    REQUIRE(b.status.dmr_rxSrcId == CALLER);
    REQUIRE(dmr.rxSquelchOpen() == false);
    b.drain();
    REQUIRE(b.ctrlState() == CallController::RX_HANG);

    /* Hang time over: idle */
    sleepFor(0, 1100);
    b.tick();
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);
    REQUIRE(b.status.dmr_callState == DMR_CALL_IDLE);
    REQUIRE(b.rec->startTxCount == 0);
}

TEST_CASE("DMR OpMode: a call on another colour code is ignored", "[dmr][rtx]")
{
    Bench b;
    b.start();
    b.ticks(6);

    dmrEmu_injectGroupCall(CALLER, TALKGROUP, 5, 2, 6);
    b.drain();
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);
    REQUIRE(b.status.dmr_callState == DMR_CALL_IDLE);
    REQUIRE(b.status.dmr_lcOk == false);
    REQUIRE(dmr.audio().rxFrames == 0);
    REQUIRE(b.status.dmr_rxColorCodeSeen == 5);

    /* Monitor "all": the same call is accepted */
    b.status.dmr_monitor = 2;
    dmr.update(&b.status, true);
    REQUIRE(b.rec->configureCount == 1);
    dmrEmu_injectGroupCall(CALLER, TALKGROUP, 5, 2, 6);
    REQUIRE(b.runUntil(CallController::RX_CALL));
    REQUIRE(b.status.dmr_rxSrcId == CALLER);
}

TEST_CASE("DMR OpMode: a modem parameter change reprograms the modem once",
          "[dmr][rtx]")
{
    Bench b;
    b.start();
    b.ticks(6);
    REQUIRE(b.rec->configureCount == 1);
    REQUIRE(b.rec->startRxCount == 1);

    /* Same configuration again: nothing happens */
    dmr.update(&b.status, true);
    REQUIRE(b.rec->configureCount == 1);
    REQUIRE(b.rec->startRxCount == 1);
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);

    /* Destination only: the controller follows, the modem is untouched */
    b.status.dmr_dstId = 9;
    b.status.dmr_callType = PRIVATE;
    dmr.update(&b.status, true);
    REQUIRE(b.rec->configureCount == 1);
    REQUIRE(dmr.controller().config().dstId == 9);
    REQUIRE(dmr.controller().config().callType == PRIVATE);
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);

    /* Colour code and timeslot: reprogrammed and restarted, once */
    b.status.dmr_rxColorCode = 3;
    b.status.dmr_txColorCode = 3;
    b.status.dmr_timeslot = 1;
    dmr.update(&b.status, true);
    REQUIRE(b.rec->configureCount == 2);
    REQUIRE(b.rec->config.colorCode == 3);
    REQUIRE(b.rec->config.timeslot == 1);
    REQUIRE(b.rec->startRxCount == 2);
    REQUIRE(b.ctrlState() == CallController::RX_SEARCH);

    b.ticks(2);
    REQUIRE(b.rec->configureCount == 2);
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);

    /* A repeater channel is remembered for the TX mode decision */
    b.status.txFrequency = 435000000;
    dmr.update(&b.status, true);
    REQUIRE(dmr.controller().config().repeater == true);
    REQUIRE(b.rec->configureCount == 2);
}

TEST_CASE("DMR OpMode: a radio without a DMR modem stays off with a marker",
          "[dmr][rtx]")
{
    Bench b;
    dmrEmu_setInitResult(-1);

    b.start();
    REQUIRE(dmr.modemReady() == false);
    REQUIRE(b.rec->initCount == 1);
    REQUIRE(b.rec->configureCount == 0);
    REQUIRE(b.rec->startRxCount == 0);
    REQUIRE(b.ctrlState() == CallController::OFF);
    REQUIRE(b.status.opStatus == OFF);
    REQUIRE(b.status.dmr_callState == DMR_CALL_IDLE);
    REQUIRE((b.status.dmr_markers & DMR_MARK_NOT_SUPPORTED) != 0);
    REQUIRE(dmr.rxSquelchOpen() == false);

    /* PTT does nothing, no events are consumed */
    emulator_state.PTTstatus = true;
    b.run(2);
    REQUIRE(b.status.opStatus == OFF);
    REQUIRE(b.rec->startTxCount == 0);
    REQUIRE(b.rec->timeouts == 0);
    REQUIRE((b.status.dmr_markers & DMR_MARK_NOT_SUPPORTED) != 0);
    emulator_state.PTTstatus = false;

    dmr.disable();
    REQUIRE(b.rec->terminateCount == 0);

    /* The modem comes back: the mode works again */
    dmrEmu_setInitResult(0);
    b.start();
    REQUIRE(dmr.modemReady() == true);
    REQUIRE(b.status.opStatus == RX);
    REQUIRE(b.status.dmr_markers == 0);
    REQUIRE(b.rec->configureCount == 1);
}
