/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "SDL2/SDL.h"
#include "core/audio_path.h"
#include "core/dmr_audio.h"
#include "emulator.h"
#include "interfaces/delays.h"
#include "protocols/DMR/CallController.hpp"
#include "protocols/DMR/Constants.hpp"
#include "protocols/DMR/LinkControl.hpp"
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
 * handler is stepped with update() as rtx_task() would do. The speaker is
 * SDL's dummy audio device, so that the DMR audio thread opens a real
 * output stream on the emulator's SINK_SPK; the emulator has no microphone
 * device, so the encoder side is refused and silence is transmitted. The
 * RF stage is the emulator's radio_linux.cpp, whose keying calls are
 * counted (radioEmu_record()).
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
    const struct radioEmuRecord *rf;

    Bench()
    {
        static bool sdlAudio = false;
        if (!sdlAudio) {
            setenv("SDL_AUDIODRIVER", "dummy", 1);
            SDL_InitSubSystem(SDL_INIT_AUDIO);
            sdlAudio = true;
        }

        dmrEmu_reset();
        radioEmu_reset();
        emulator_state.PTTstatus = false;
        state.settings.dmr_hangTime = 0;
        status = initialStatus();
        rec = dmrEmu_record();
        rf = radioEmu_record();
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

    /* Terminator out: back to RX, hang time zero -> idle at once; the RF
     * stays keyed for the slot the terminator occupies */
    b.tick();
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);
    REQUIRE(b.rec->startRxCount == 2);
    REQUIRE(b.rec->r40 == R40_RX);
    REQUIRE(b.status.opStatus == TX);
    REQUIRE(b.status.dmr_callState == DMR_CALL_IDLE);
    REQUIRE(b.status.dmr_lcOk == false);
    REQUIRE(b.status.dmr_markers == 0);
    REQUIRE(b.rec->terminators == 1);

    /* The slot after: RF released */
    b.tick();
    REQUIRE(b.status.opStatus == RX);
    REQUIRE(dmr.rfKeyed() == false);

    b.ticks(3);
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

    /* Voice bursts reach the audio layer, the terminator ends it */
    REQUIRE(b.runUntil(CallController::RX_HANG));
    REQUIRE(dmr.audio().rxFrames == 12);
    REQUIRE(dmr.audio().rxQueued == 12);
    REQUIRE(b.status.dmr_audio.framesRx == 12);
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

TEST_CASE("DMR OpMode: a modem parameter change during TX waits for the end",
          "[dmr][rtx]")
{
    Bench b;
    b.start();
    b.ticks(6);
    REQUIRE(b.rec->configureCount == 1);

    emulator_state.PTTstatus = true;
    unsigned n = 0;
    while (b.ctrlState() != CallController::TX_VOICE) {
        b.tick();
        REQUIRE(++n < 12);
    }
    uint32_t bursts = b.rec->voiceBursts;

    /* Colour code changed mid-call: the modem is left alone, voice goes on */
    b.status.dmr_rxColorCode = 3;
    b.status.dmr_txColorCode = 3;
    dmr.update(&b.status, true);
    REQUIRE(b.rec->configureCount == 1);
    REQUIRE(b.ctrlState() == CallController::TX_VOICE);
    b.ticks(4);
    REQUIRE(b.ctrlState() == CallController::TX_VOICE);
    REQUIRE(b.rec->voiceBursts == bursts + 2);
    REQUIRE(b.status.opStatus == TX);

    /* A modem-neutral change on top of it does not lose the pending one */
    b.status.dmr_dstId = 9;
    dmr.update(&b.status, true);
    REQUIRE(b.rec->configureCount == 1);
    REQUIRE(b.ctrlState() == CallController::TX_VOICE);

    /* Release: superframe, terminator, then the deferred reprogramming */
    emulator_state.PTTstatus = false;
    n = 0;
    while (b.ctrlState() != CallController::TX_TERM) {
        b.tick();
        REQUIRE(++n < 14);
    }
    REQUIRE(b.rec->configureCount == 1);
    uint32_t startRx = b.rec->startRxCount;

    /* The terminator slot: still keyed, the reprogramming waits for it */
    b.tick();
    REQUIRE(b.rec->terminators == 1);
    REQUIRE(b.rec->configureCount == 1);
    REQUIRE(dmr.rfKeyed() == true);
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);

    /* RF released: reprogrammed and restarted */
    b.tick();
    REQUIRE(dmr.rfKeyed() == false);
    REQUIRE(b.rec->configureCount == 2);
    REQUIRE(b.rec->config.colorCode == 3);
    REQUIRE(b.rec->startRxCount > startRx);
    REQUIRE(b.ctrlState() == CallController::RX_SEARCH);
    REQUIRE(b.status.opStatus == RX);
    REQUIRE(dmr.controller().config().colorCode == 3);
    REQUIRE(dmr.controller().config().dstId == 9);

    b.ticks(2);
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);
    REQUIRE(b.rec->configureCount == 2);
}

TEST_CASE("DMR OpMode: audio paths follow the call", "[dmr][rtx]")
{
    Bench b;
    state.settings.dmr_hangTime = 1;
    b.start();
    b.ticks(6);
    REQUIRE(dmr.rxAudioPathId() < 0);
    REQUIRE(dmr.txAudioPathId() < 0);

    /* Speaker path for the received call and its hang time */
    dmrEmu_injectGroupCall(CALLER, TALKGROUP, 1, 2, 12);
    REQUIRE(b.runUntil(CallController::RX_CALL));
    pathId spk = dmr.rxAudioPathId();
    REQUIRE(spk >= 0);
    REQUIRE(audioPath_getStatus(spk) == PATH_OPEN);
    REQUIRE(audioPath_getInfo(spk).source == SOURCE_MCU);
    REQUIRE(audioPath_getInfo(spk).sink == SINK_SPK);
    REQUIRE(audioPath_getInfo(spk).prio == PRIO_RX);
    REQUIRE(dmr.txAudioPathId() < 0);

    REQUIRE(b.runUntil(CallController::RX_HANG));
    b.drain();
    REQUIRE(dmr.rxAudioPathId() == spk);
    REQUIRE(audioPath_getStatus(spk) == PATH_OPEN);

    sleepFor(0, 1100);
    b.tick();
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);
    REQUIRE(dmr.rxAudioPathId() < 0);
    REQUIRE(audioPath_getStatus(spk) == PATH_CLOSED);

    /* Microphone path for the whole transmission */
    emulator_state.PTTstatus = true;
    b.tick();
    REQUIRE(b.ctrlState() >= CallController::TX_ARM);
    pathId mic = dmr.txAudioPathId();
    REQUIRE(mic >= 0);
    REQUIRE(audioPath_getStatus(mic) == PATH_OPEN);
    REQUIRE(audioPath_getInfo(mic).source == SOURCE_MIC);
    REQUIRE(audioPath_getInfo(mic).sink == SINK_MCU);
    REQUIRE(audioPath_getInfo(mic).prio == PRIO_TX);
    REQUIRE(dmr.rxAudioPathId() < 0);

    unsigned n = 0;
    while (b.ctrlState() != CallController::TX_VOICE) {
        b.tick();
        REQUIRE(++n < 12);
    }
    REQUIRE(audioPath_getStatus(mic) == PATH_OPEN);

    /* Released at burst A: one superframe of silence, then the terminator */
    emulator_state.PTTstatus = false;
    n = 0;
    while (b.ctrlState() != CallController::TX_TERM) {
        b.tick();
        REQUIRE(++n < 16);
    }
    REQUIRE(b.rec->voiceBursts == 6);
    REQUIRE(dmr.txAudioPathId() == mic);

    /* Terminator out: the microphone is held while the RF is, released
     * with it at the next slot */
    b.tick();
    REQUIRE(b.ctrlState() < CallController::TX_ARM);
    REQUIRE(dmr.rfKeyed() == true);
    REQUIRE(dmr.txAudioPathId() == mic);
    REQUIRE(audioPath_getStatus(mic) == PATH_OPEN);
    b.tick();
    REQUIRE(dmr.rfKeyed() == false);
    REQUIRE(dmr.txAudioPathId() < 0);
    REQUIRE(audioPath_getStatus(mic) == PATH_CLOSED);
    REQUIRE(dmr.rxAudioPathId() < 0);
}

TEST_CASE("DMR OpMode: received voice flows into the audio thread",
          "[dmr][rtx]")
{
    Bench b;
    state.settings.dmr_hangTime = 1;
    b.start();
    b.ticks(6);
    REQUIRE(dmrAudio_running() == false);
    REQUIRE(b.status.dmr_audio.framesRx == 0);
    REQUIRE(b.status.dmr_audio.blocksOut == 0);

    /* The call opens: speaker path granted, playback thread started */
    dmrEmu_injectGroupCall(CALLER, TALKGROUP, 1, 2, 12);
    REQUIRE(b.runUntil(CallController::RX_CALL));
    pathId spk = dmr.rxAudioPathId();
    REQUIRE(audioPath_getStatus(spk) == PATH_OPEN);
    REQUIRE(dmrAudio_running() == true);

    /* Blocks flow to the speaker while the call is up, silence so far */
    sleepFor(0, 150);
    b.run(1);
    REQUIRE(b.status.dmr_audio.blocksOut >= 1);
    REQUIRE(b.status.dmr_audio.framesRx == 0);
    REQUIRE(b.status.dmr_audio.underruns == 0);

    /* Every voice burst is pushed; the terminator stops the thread */
    REQUIRE(b.runUntil(CallController::RX_HANG));
    REQUIRE(dmr.audio().rxFrames == 12);
    REQUIRE(dmr.audio().rxQueued == 12);
    REQUIRE(b.status.dmr_audio.framesRx == 12);
    REQUIRE(b.status.dmr_audio.varying == 12);
    REQUIRE(dmrAudio_running() == false);
    REQUIRE(audioPath_getStatus(spk) == PATH_OPEN);
    b.drain();

    /* A call opening during the hang time starts the thread again */
    dmrEmu_injectGroupCall(CALLER, TALKGROUP, 1, 2, 6);
    REQUIRE(b.runUntil(CallController::RX_CALL));
    REQUIRE(dmr.rxAudioPathId() == spk);
    REQUIRE(dmrAudio_running() == true);
    REQUIRE(b.runUntil(CallController::RX_HANG));
    REQUIRE(dmr.audio().rxQueued == 18);
    REQUIRE(dmrAudio_running() == false);
    b.drain();

    /* Hang time over: path released */
    sleepFor(0, 1100);
    b.tick();
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);
    REQUIRE(dmr.rxAudioPathId() < 0);
    REQUIRE(audioPath_getStatus(spk) == PATH_CLOSED);
    REQUIRE(dmrAudio_running() == false);

    /* The counters are kept until the next enable() */
    REQUIRE(b.status.dmr_audio.framesRx == 18);
    dmr.disable();
    b.start();
    REQUIRE(b.status.dmr_audio.framesRx == 0);
}

TEST_CASE("DMR OpMode: no microphone device, the encoder is refused and "
          "silence is sent",
          "[dmr][rtx]")
{
    Bench b;
    b.start();
    b.ticks(6);

    emulator_state.PTTstatus = true;
    unsigned n = 0;
    while (b.ctrlState() != CallController::TX_VOICE) {
        b.tick();
        REQUIRE(++n < 12);
    }
    REQUIRE(audioPath_getStatus(dmr.txAudioPathId()) == PATH_OPEN);
    REQUIRE(dmrAudio_running() == false);

    b.ticks(12);
    REQUIRE(dmr.audio().txRequests == 6);
    REQUIRE(dmr.audio().txFrames == 0);
    REQUIRE(b.status.dmr_audio.framesTx == 0);
    REQUIRE(memcmp(b.rec->voice, AMBE_SILENCE, VOICE_PAYLOAD_BYTES) == 0);

    emulator_state.PTTstatus = false;
    n = 0;
    while (b.ctrlState() >= CallController::TX_ARM) {
        b.tick();
        REQUIRE(++n < 30);
    }
    b.tick();
    REQUIRE(dmr.rfKeyed() == false);
    REQUIRE(dmr.txAudioPathId() < 0);
    REQUIRE(dmrAudio_running() == false);
}

TEST_CASE("DMR OpMode: the encoder queue is flushed once when the voice "
          "bursts begin, before the first one",
          "[dmr][rtx]")
{
    Bench b;
    b.start();
    b.ticks(6);
    REQUIRE(dmr.audio().txFlushes == 0);

    /* Through the headers: no voice burst requested, no flush yet */
    emulator_state.PTTstatus = true;
    unsigned n = 0;
    while (b.ctrlState() != CallController::TX_VOICE) {
        b.tick();
        REQUIRE(++n < 12);
        if (b.ctrlState() != CallController::TX_VOICE)
            REQUIRE(dmr.audio().txFlushes == 0);
    }

    /* The controller entered TX_VOICE on the last header: flushed at once,
     * the first voice burst is still to come */
    REQUIRE(dmr.audio().txFlushes == 1);
    REQUIRE(dmr.audio().txRequests == 0);

    /* Once per transmission */
    b.ticks(8);
    REQUIRE(dmr.audio().txRequests == 4);
    REQUIRE(dmr.audio().txFlushes == 1);

    /* The next transmission flushes again */
    emulator_state.PTTstatus = false;
    n = 0;
    while (b.ctrlState() >= CallController::TX_ARM) {
        b.tick();
        REQUIRE(++n < 30);
    }
    REQUIRE(dmr.audio().txFlushes == 1);

    b.ticks(6);
    emulator_state.PTTstatus = true;
    n = 0;
    while (b.ctrlState() != CallController::TX_VOICE) {
        b.tick();
        REQUIRE(++n < 12);
    }
    REQUIRE(dmr.audio().txFlushes == 2);
    REQUIRE(dmr.audio().txRequests == 6);

    emulator_state.PTTstatus = false;
}

TEST_CASE("DMR OpMode: a voice prompt suspends the RX path, playback resumes "
          "after it",
          "[dmr][rtx]")
{
    Bench b;
    state.settings.dmr_hangTime = 1;
    b.start();
    b.ticks(6);

    dmrEmu_injectGroupCall(CALLER, TALKGROUP, 1, 2, 30);
    REQUIRE(b.runUntil(CallController::RX_CALL));
    pathId spk = dmr.rxAudioPathId();
    REQUIRE(dmrAudio_running() == true);

    /* Play the script until n voice bursts were received */
    auto runUntilRx = [&](uint32_t n) {
        for (unsigned i = 0; (i < 400) && (dmr.audio().rxFrames < n); i++)
            dmr.update(&b.status, false);
        return dmr.audio().rxFrames == n;
    };

    /* Two bursts in, then the UI plays a prompt on the speaker */
    REQUIRE(runUntilRx(2));
    REQUIRE(dmr.audio().rxQueued == 2);
    pathId prompt = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_PROMPT);
    REQUIRE(prompt > 0);
    REQUIRE(audioPath_getStatus(spk) == PATH_SUSPENDED);

    /* The thread quits at its next period; the bursts meanwhile are lost */
    bool stopped = false;
    for (int i = 0; (i < 50) && !stopped; i++) {
        sleepFor(0, 10);
        stopped = (dmrAudio_running() == false);
    }
    REQUIRE(stopped);
    REQUIRE(runUntilRx(4));
    REQUIRE(b.ctrlState() == CallController::RX_CALL);
    REQUIRE(dmr.audio().rxQueued == 2);
    REQUIRE(dmr.rxAudioPathId() == spk);
    REQUIRE(dmrAudio_running() == false);

    /* Prompt over: the path is open again and playback restarts */
    audioPath_release(prompt);
    REQUIRE(audioPath_getStatus(spk) == PATH_OPEN);
    b.run(1);
    REQUIRE(dmrAudio_running() == true);
    uint32_t lost = dmr.audio().rxFrames - dmr.audio().rxQueued;
    REQUIRE(runUntilRx(dmr.audio().rxFrames + 1));
    REQUIRE(dmr.audio().rxQueued == dmr.audio().rxFrames - lost);

    REQUIRE(b.runUntil(CallController::RX_HANG));
    REQUIRE(dmr.audio().rxFrames == 30);
    REQUIRE(dmr.audio().rxQueued == 30 - lost);
    REQUIRE(dmrAudio_running() == false);
    b.drain();
}

TEST_CASE("DMR OpMode: disable releases the audio paths mid-call", "[dmr][rtx]")
{
    Bench b;
    state.settings.dmr_hangTime = 1;
    b.start();
    b.ticks(6);

    dmrEmu_injectGroupCall(CALLER, TALKGROUP, 1, 2, 12);
    REQUIRE(b.runUntil(CallController::RX_CALL));
    pathId spk = dmr.rxAudioPathId();
    REQUIRE(audioPath_getStatus(spk) == PATH_OPEN);

    dmr.disable();
    REQUIRE(b.ctrlState() == CallController::OFF);
    REQUIRE(dmr.rxAudioPathId() < 0);
    REQUIRE(dmr.txAudioPathId() < 0);
    REQUIRE(audioPath_getStatus(spk) == PATH_CLOSED);
    REQUIRE(b.rec->idleCount == 1);

    /* Same while transmitting */
    dmrEmu_reset();
    b.start();
    b.ticks(6);
    emulator_state.PTTstatus = true;
    b.ticks(2);
    REQUIRE(b.ctrlState() >= CallController::TX_ARM);
    pathId mic = dmr.txAudioPathId();
    REQUIRE(audioPath_getStatus(mic) == PATH_OPEN);

    dmr.disable();
    REQUIRE(dmr.txAudioPathId() < 0);
    REQUIRE(audioPath_getStatus(mic) == PATH_CLOSED);

    /* Nothing leaks: the same paths can be requested again */
    pathId again = audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX);
    REQUIRE(again >= 0);
    audioPath_release(again);
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

TEST_CASE("DMR OpMode: the bring-up counters are published", "[dmr][rtx]")
{
    Bench b;
    b.start();
    const dmrMonitor_t *mon = &b.status.dmr_mon;
    REQUIRE(mon->tsCount == 0);
    REQUIRE(mon->sysCount == 0);

    /* Timeslot events: counters and the registers read in the TS pass */
    b.ticks(6);
    REQUIRE(mon->tsCount == 6);
    REQUIRE(mon->sysCount == 0);
    REQUIRE(mon->rftxCount == 0);
    REQUIRE(mon->r52 == 0x14);
    /* The fake's TC starts at 0 and toggles per slot: the sixth carries 1 */
    REQUIRE(mon->timecode == 1);
    REQUIRE(b.status.dmr_slotLock == 2);

    /* A header burst: flags, registers, line counters and the decoded LC */
    struct dmrbbSnapshot s;
    memset(&s, 0x00, sizeof(s));
    s.irq82 = R82_CTRL_FRAME;
    s.sub84 = 0x80;
    s.sub86 = 0x40;
    s.r51 = (uint8_t)((DT_VOICE_LC_HEADER << 4) | R51_CLASS_HEADER);
    s.r52 = 0x14;
    s.r5f = R5F_SYNC_BS;
    s.r42 = 0x60;
    s.rssi = 4321;
    s.fskErr = 3;

    /* EXTI pulses counted by the "ISR": seven on PC0, one on PC3 */
    for (unsigned i = 0; i < 7; i++)
        dmrEmu_pulseLine(0);
    dmrEmu_pulseLine(3);

    FullLC lc;
    lc.clear();
    lc.flco = FLCO_GRP_V_CH_USR;
    lc.fid = FID_STANDARD;
    lc.dst = TALKGROUP;
    lc.src = CALLER;
    lc.toChipLc(s.lc);

    dmrEmu_pushSys(&s);
    dmr.update(&b.status, false);
    REQUIRE(mon->tsCount == 6);
    REQUIRE(mon->sysCount == 1);
    REQUIRE(mon->irq82 == R82_CTRL_FRAME);
    REQUIRE(mon->sub84 == 0x80);
    REQUIRE(mon->sub86 == 0x40);
    REQUIRE(mon->r51 == s.r51);
    REQUIRE(mon->r52 == 0x14);
    REQUIRE(mon->r5f == R5F_SYNC_BS);
    REQUIRE(mon->rssi == 4321);
    REQUIRE(mon->fskErr == 3);
    REQUIRE(mon->lineCount[0] == 7);
    REQUIRE(mon->lineCount[1] == 0);
    REQUIRE(mon->lineCount[3] == 1);
    REQUIRE(memcmp(mon->lc, s.lc, CHIP_LC_BYTES) == 0);
    REQUIRE(mon->lcSrc == CALLER);
    REQUIRE(mon->lcDst == TALKGROUP);
    REQUIRE(mon->lcFlco == FLCO_GRP_V_CH_USR);
    REQUIRE(mon->lcFid == FID_STANDARD);
    REQUIRE(mon->lcValid == 0x03);

    /* A voice burst keeps the LC of the header */
    s.r51 = (uint8_t)((1 << 4) | R51_CLASS_VOICE);
    s.voiceValid = true;
    memset(s.lc, 0xFF, sizeof(s.lc));
    dmrEmu_pushSys(&s);
    dmr.update(&b.status, false);
    REQUIRE(mon->sysCount == 2);
    REQUIRE(mon->r51 == s.r51);
    REQUIRE(mon->lcSrc == CALLER);
    REQUIRE(mon->lc[0] == 0x00);

    /* A terminator with a bad CRC: the chip's verdict is reported */
    s.r51 = (uint8_t)((DT_TERMINATOR_LC << 4) | R51_CLASS_HEADER | R51_CRC_BAD);
    s.voiceValid = false;
    dmrEmu_pushSys(&s);
    dmr.update(&b.status, false);
    REQUIRE(mon->sysCount == 3);
    REQUIRE(mon->lc[0] == 0xFF);
    REQUIRE((mon->lcValid & 0x01) == 0);

    /* Timeouts keep the block, the timecode follows the controller */
    b.run(3);
    REQUIRE(mon->tsCount == 6);
    REQUIRE(mon->sysCount == 3);
    REQUIRE(mon->lineCount[0] == 7);
    REQUIRE(mon->lineCount[3] == 1);
    REQUIRE(mon->lcValid == 0x00);

    /*
     * A line pulsing without an event (SYS_INTER on PC3 while the pinmap
     * expects it on PC1, no sync so no slot ticks): the counters reach the
     * block through the timeout passes, the register fields stay
     */
    dmrEmu_pulseLine(3);
    dmrEmu_pulseLine(3);
    uint32_t timeouts = b.rec->timeouts;
    b.run(1);
    REQUIRE(b.rec->timeouts == timeouts + 1);
    REQUIRE(mon->lineCount[3] == 3);
    REQUIRE(mon->lineCount[0] == 7);
    REQUIRE(mon->tsCount == 6);
    REQUIRE(mon->sysCount == 3);
    REQUIRE(mon->irq82 == R82_CTRL_FRAME);
    REQUIRE(mon->rssi == 4321);
    REQUIRE(mon->r51 == s.r51);

    /* A new enable() starts from zero */
    dmr.disable();
    b.start();
    REQUIRE(mon->tsCount == 0);
    REQUIRE(mon->sysCount == 0);
    REQUIRE(mon->lcSrc == 0);
    REQUIRE(mon->irq82 == 0);
    REQUIRE(b.rec->configureCount == 2);
    REQUIRE(b.rec->config.modeReg == DMRBB_MODE_REG_DEFAULT);

    /* The monitor screen's mode word reprograms the modem, once */
    b.status.dmr_modeReg = 0x6A;
    dmr.update(&b.status, true);
    REQUIRE(b.rec->configureCount == 3);
    REQUIRE(b.rec->config.modeReg == 0x6A);
    dmr.update(&b.status, true);
    REQUIRE(b.rec->configureCount == 3);
    b.status.dmr_modeReg = 0;
    dmr.update(&b.status, true);
    REQUIRE(b.rec->configureCount == 4);
    REQUIRE(b.rec->config.modeReg == DMRBB_MODE_REG_DEFAULT);
}

/*
 * Stage 5: the RF stage around a transmission, the end of transmission
 * conditions and the LCSS convention, all on the fake modem with the fake RF
 * stage of radio_linux.cpp.
 */

TEST_CASE("DMR OpMode TX: the RF stage is keyed once, before the first "
          "header, and released after the terminator burst",
          "[dmr][rtx][tx]")
{
    Bench b;
    b.start();
    b.ticks(6);
    uint32_t enableRx = b.rf->enableRx;
    REQUIRE(b.rf->enableTx == 0);
    REQUIRE(b.rf->disableRtx == 0);
    REQUIRE(dmr.rfKeyed() == false);

    /* PTT: keyed on the pass that arms the modem, no slot programmed yet */
    emulator_state.PTTstatus = true;
    b.tick();
    REQUIRE(b.ctrlState() == CallController::TX_ARM);
    REQUIRE(b.rf->enableTx == 1);
    REQUIRE(b.rf->keyed == true);
    REQUIRE(dmr.rfKeyed() == true);
    REQUIRE(b.rec->txSlots == 0);
    REQUIRE(b.rec->startTxCount == 1);
    REQUIRE(b.status.opStatus == TX);
    REQUIRE(b.status.dmr_mon.rfKeyed == 1);

    /* Exactly three header slots, one every other timeslot */
    unsigned n = 0;
    while (b.ctrlState() != CallController::TX_VOICE) {
        b.tick();
        REQUIRE(b.rf->enableTx == 1);
        REQUIRE(++n < 10);
    }
    REQUIRE(b.rec->headers == 3);
    REQUIRE(b.rec->txSlots == 3);
    REQUIRE(b.rec->voiceBursts == 0);
    REQUIRE(b.rec->r41 == R41_TX);

    /* Voice: seq 0..5 twice, one 27-byte write per own slot, silence */
    uint32_t voiceWrites = b.rec->voiceWrites;
    for (unsigned i = 0; i < 12; i++) {
        /* The other slot: idle, nothing written */
        b.tick();
        REQUIRE(b.rec->r41 == R41_IDLE);
        REQUIRE(b.rec->voiceWrites == voiceWrites);

        /* The own slot: burst i % 6 */
        b.tick();
        REQUIRE(b.rec->r41 == R41_TX);
        REQUIRE(b.rec->r50 == r50Voice(i % 6));
        REQUIRE(b.rec->voiceWrites == voiceWrites + 1);
        REQUIRE(memcmp(b.rec->voice, AMBE_SILENCE, VOICE_PAYLOAD_BYTES) == 0);
        voiceWrites++;
        REQUIRE(b.rf->enableTx == 1);
        REQUIRE(b.rf->disableRtx == 0);
    }
    REQUIRE(b.rec->voiceBursts == 12);
    REQUIRE(dmr.audio().txRequests == 12);
    REQUIRE(dmr.audio().txFrames == 0);
    REQUIRE(dmr.audio().txUnderruns == 0);
    REQUIRE(b.status.dmr_mon.txBursts == 12);
    REQUIRE(b.status.dmr_mon.txSilent == 12);
    REQUIRE(b.status.dmr_mon.txUnderruns == 0);

    /* Burst A of the next superframe, then release: B..F, terminator */
    b.tick();
    b.tick();
    REQUIRE(b.rec->r50 == r50Voice(0));
    emulator_state.PTTstatus = false;
    for (unsigned seq = 1; seq < 6; seq++) {
        b.tick();
        REQUIRE(b.rec->r41 == R41_IDLE);
        b.tick();
        REQUIRE(b.rec->r41 == R41_TX);
        REQUIRE(b.rec->r50 == r50Voice(seq));
        REQUIRE(b.ctrlState() == CallController::TX_VOICE);
    }
    b.tick();
    REQUIRE(b.rec->r41 == R41_IDLE);
    struct dmrAudioStats audioStats;
    dmrAudio_getStats(&audioStats);
    uint32_t stops = audioStats.stops;
    pathId mic = dmr.txAudioPathId();
    REQUIRE(audioPath_getStatus(mic) == PATH_OPEN);
    b.tick();
    REQUIRE(b.ctrlState() == CallController::TX_TERM);
    REQUIRE(b.rec->r50 == R50_TERMINATOR);
    REQUIRE(b.rec->terminators == 1);
    REQUIRE(b.rec->voiceBursts == 18);
    REQUIRE(b.rf->enableTx == 1);
    REQUIRE(b.rf->disableRtx == 0);

    /* No audio thread stop in the pass that programmed the terminator: the
     * join on the microphone stream would sit between that write and the
     * idling of the slot after, see OpMode_DMR::followState() */
    dmrAudio_getStats(&audioStats);
    REQUIRE(audioStats.stops == stops);
    REQUIRE(audioPath_getStatus(mic) == PATH_OPEN);

    /* The terminator slot: the modem is back in RX, the RF still keyed,
     * the encoder and the microphone path still held */
    b.tick();
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);
    REQUIRE(b.rec->r40 == R40_RX);
    REQUIRE(b.rf->disableRtx == 0);
    REQUIRE(b.rf->keyed == true);
    REQUIRE(dmr.rfKeyed() == true);
    REQUIRE(b.status.opStatus == TX);
    REQUIRE(dmr.txAudioPathId() == mic);
    REQUIRE(audioPath_getStatus(mic) == PATH_OPEN);
    dmrAudio_getStats(&audioStats);
    REQUIRE(audioStats.stops == stops);

    /* The slot after: released once, reception back on, and only then the
     * encoder stopped and the microphone released */
    b.tick();
    REQUIRE(b.rf->disableRtx == 1);
    REQUIRE(b.rf->enableRx == enableRx + 1);
    REQUIRE(b.rf->enableTx == 1);
    REQUIRE(b.rf->keyed == false);
    REQUIRE(dmr.rfKeyed() == false);
    REQUIRE(b.status.opStatus == RX);
    REQUIRE(b.status.dmr_mon.rfKeyed == 1);
    REQUIRE(dmr.txAudioPathId() < 0);
    REQUIRE(audioPath_getStatus(mic) == PATH_CLOSED);
    dmrAudio_getStats(&audioStats);
    REQUIRE(audioStats.stops == stops + 1);

    b.ticks(6);
    REQUIRE(b.rf->disableRtx == 1);
    REQUIRE(b.rf->enableTx == 1);
    REQUIRE(b.rec->txSlots == 22);

    /* A second transmission keys again, and is released once again after
     * its own terminator */
    emulator_state.PTTstatus = true;
    b.tick();
    REQUIRE(b.rf->enableTx == 2);
    REQUIRE(b.status.dmr_mon.rfKeyed == 2);
    emulator_state.PTTstatus = false;
    n = 0;
    while (b.ctrlState() >= CallController::TX_ARM) {
        b.tick();
        REQUIRE(++n < 40);
    }
    REQUIRE(b.rf->disableRtx == 1);
    b.tick();
    REQUIRE(b.rf->disableRtx == 2);
    REQUIRE(b.rec->terminators == 2);
}

TEST_CASE("DMR OpMode TX: the chip's TX-end report releases the RF before "
          "the next timeslot",
          "[dmr][rtx][tx]")
{
    Bench b;
    b.start();
    b.ticks(6);

    emulator_state.PTTstatus = true;
    b.ticks(8);
    REQUIRE(b.ctrlState() == CallController::TX_VOICE);
    emulator_state.PTTstatus = false;
    unsigned n = 0;
    while (b.ctrlState() != CallController::TX_TERM) {
        b.tick();
        REQUIRE(++n < 16);
    }
    b.tick();
    REQUIRE(b.ctrlState() < CallController::TX_ARM);
    REQUIRE(dmr.rfKeyed() == true);
    REQUIRE(b.rf->disableRtx == 0);

    /* 0x82 bit5 with the per-slot Layer 2 deadline sub-status (0x86 bit4,
     * Rdy_lst_inter) while the terminator is on the air: still keyed */
    struct dmrbbSnapshot s;
    memset(&s, 0x00, sizeof(s));
    s.irq82 = R82_TX_END;
    s.sub86 = R86_RDY_LST;
    dmrEmu_pushSys(&s);
    dmr.update(&b.status, false);
    REQUIRE(dmr.rfKeyed() == true);
    REQUIRE(b.rf->disableRtx == 0);
    REQUIRE(b.status.dmr_mon.sub86 == R86_RDY_LST);
    REQUIRE(b.status.opStatus == TX);

    /* 0x82 bit5 with sub-status 0x86 bit7: service TX finished */
    s.sub86 = R86_TX_COMPLETE;
    dmrEmu_pushSys(&s);
    dmr.update(&b.status, false);
    REQUIRE(dmr.rfKeyed() == false);
    REQUIRE(b.rf->disableRtx == 1);
    REQUIRE(b.status.dmr_mon.sub86 == R86_TX_COMPLETE);
    REQUIRE(b.status.opStatus == RX);

    /* The following timeslot changes nothing */
    b.tick();
    REQUIRE(b.rf->disableRtx == 1);
}

TEST_CASE("DMR OpMode TX: a rejected transmission unkeys at once with the "
          "busy marker",
          "[dmr][rtx][tx]")
{
    Bench b;
    b.start();
    b.ticks(6);

    emulator_state.PTTstatus = true;
    b.ticks(3);
    REQUIRE(b.ctrlState() == CallController::TX_HEADER);
    REQUIRE(b.rf->enableTx == 1);
    REQUIRE(b.rf->disableRtx == 0);

    /* 0x82 bit7: the modem refused to transmit over the busy channel */
    struct dmrbbSnapshot s;
    memset(&s, 0x00, sizeof(s));
    s.irq82 = R82_TX_REJECTED;
    dmrEmu_pushSys(&s);
    dmr.update(&b.status, false);
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);
    REQUIRE(b.rf->disableRtx == 1);
    REQUIRE(b.rf->keyed == false);
    REQUIRE(dmr.rfKeyed() == false);
    REQUIRE(b.status.opStatus == RX);
    REQUIRE(b.status.dmr_callState == DMR_CALL_IDLE);
    REQUIRE((b.status.dmr_markers & DMR_MARK_BUSY) != 0);
    REQUIRE(b.rec->r40 == R40_RX);
    REQUIRE(dmr.txAudioPathId() < 0);

    /* PTT still held: nothing, marker kept */
    b.ticks(4);
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);
    REQUIRE(b.rf->enableTx == 1);
    REQUIRE((b.status.dmr_markers & DMR_MARK_BUSY) != 0);

    /* Released and pressed again: the marker clears, a new transmission */
    emulator_state.PTTstatus = false;
    b.tick();
    emulator_state.PTTstatus = true;
    b.tick();
    REQUIRE(b.ctrlState() >= CallController::TX_ARM);
    REQUIRE(b.status.dmr_markers == 0);
    REQUIRE(b.rf->enableTx == 2);
}

TEST_CASE("DMR OpMode TX: T_TO ends the transmission after the superframe",
          "[dmr][rtx][tx]")
{
    Bench b;
    dmr.setTxTimeout(1000);
    b.start();
    b.ticks(6);
    REQUIRE(dmr.controller().config().txTimeoutMs == 1000);

    emulator_state.PTTstatus = true;
    unsigned n = 0;
    while (b.ctrlState() != CallController::TX_VOICE) {
        b.tick();
        REQUIRE(++n < 10);
    }
    b.ticks(12);
    REQUIRE(b.ctrlState() == CallController::TX_VOICE);
    REQUIRE(b.rec->voiceBursts == 6);

    /* One second in: the transmission ends at the superframe boundary,
     * the terminator right after burst F */
    sleepFor(1, 100);
    n = 0;
    while (b.ctrlState() != CallController::TX_TERM) {
        b.tick();
        REQUIRE(++n < 16);
    }
    REQUIRE(b.rec->terminators == 1);
    REQUIRE(b.rec->voiceBursts == 6);
    REQUIRE(b.rec->r50 == R50_TERMINATOR);

    /* PTT still held: back to RX, released the slot after, no restart */
    b.tick();
    REQUIRE(b.ctrlState() < CallController::TX_ARM);
    b.tick();
    REQUIRE(b.rf->disableRtx == 1);
    REQUIRE(b.status.opStatus == RX);
    b.ticks(6);
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);
    REQUIRE(b.rf->enableTx == 1);
    REQUIRE(b.status.dmr_markers == 0);

    /* Released and pressed again: a new transmission */
    emulator_state.PTTstatus = false;
    b.tick();
    emulator_state.PTTstatus = true;
    b.tick();
    REQUIRE(b.ctrlState() >= CallController::TX_ARM);
    REQUIRE(b.rf->enableTx == 2);

    dmr.setTxTimeout(T_TO_S * 1000);
}

TEST_CASE("DMR OpMode TX: a silent repeater channel is woken up, the RF "
          "keyed per attempt, and the failure unkeys with the marker",
          "[dmr][rtx][tx]")
{
    Bench b;
    b.status.txFrequency = 435000000;
    b.start();
    b.ticks(6);
    REQUIRE(dmr.controller().config().repeater == true);

    /* No base station carrier: wake-up, keyed for the CSBK */
    emulator_state.PTTstatus = true;
    b.tick();
    REQUIRE(b.ctrlState() == CallController::TX_WAKEUP);
    REQUIRE(b.status.dmr_callState == DMR_CALL_TX_WAKEUP);
    REQUIRE(b.rf->enableTx == 1);
    REQUIRE(b.rec->activeTiming == true);
    REQUIRE(b.status.opStatus == TX);

    unsigned n = 0;
    while (b.rec->csbks == 0) {
        b.tick();
        REQUIRE(++n < 4);
    }
    uint8_t expected[CSBK_DATA_BYTES];
    buildBsDwnAct(OWN_ID, expected);
    REQUIRE(memcmp(b.rec->lc, expected, CSBK_DATA_BYTES) == 0);
    REQUIRE(b.rec->r50 == R50_CSBK);
    REQUIRE(b.rf->disableRtx == 0);

    /* CSBK slot: modem back in RX, RF still keyed; released the slot after */
    b.tick();
    REQUIRE(b.rec->r40 == R40_RX);
    REQUIRE(dmr.rfKeyed() == true);
    b.tick();
    REQUIRE(dmr.rfKeyed() == false);
    REQUIRE(b.rf->disableRtx == 1);
    REQUIRE(b.ctrlState() == CallController::TX_WAKEUP);
    REQUIRE(b.status.dmr_callState == DMR_CALL_TX_WAKEUP);
    REQUIRE(b.status.opStatus == RX);

    /* T_SyncWu without an answer: second attempt */
    sleepFor(0, T_SYNCWU_MS + 40);
    b.tick();
    REQUIRE(b.ctrlState() == CallController::TX_WAKEUP);
    REQUIRE(b.rf->enableTx == 2);
    n = 0;
    while (b.rec->csbks < 2) {
        b.tick();
        REQUIRE(++n < 4);
    }
    b.ticks(2);
    REQUIRE(b.rf->disableRtx == 2);

    /* Second T_SyncWu: given up, marker, RX */
    sleepFor(0, T_SYNCWU_MS + 40);
    b.tick();
    REQUIRE(b.ctrlState() < CallController::TX_ARM);
    REQUIRE((b.status.dmr_markers & DMR_MARK_WAKEUP_FAILED) != 0);
    REQUIRE(b.status.dmr_callState == DMR_CALL_IDLE);
    REQUIRE(dmr.rfKeyed() == false);
    REQUIRE(b.rf->enableTx == 2);
    REQUIRE(b.rf->disableRtx == 2);
    REQUIRE(b.rec->headers == 0);
    REQUIRE(dmr.txAudioPathId() < 0);

    /* PTT held: nothing more */
    b.ticks(4);
    REQUIRE(b.rf->enableTx == 2);
    emulator_state.PTTstatus = false;
}

TEST_CASE("DMR OpMode TX: a repeater answering the wake-up gets the call "
          "in passive timing",
          "[dmr][rtx][tx]")
{
    Bench b;
    b.status.txFrequency = 435000000;
    b.start();
    b.ticks(6);

    emulator_state.PTTstatus = true;
    b.tick();
    REQUIRE(b.ctrlState() == CallController::TX_WAKEUP);
    unsigned n = 0;
    while (b.rec->csbks == 0) {
        b.tick();
        REQUIRE(++n < 4);
    }
    b.ticks(2);
    REQUIRE(b.rf->enableTx == 1);
    REQUIRE(b.rf->disableRtx == 1);

    /* The repeater comes up: BS sourced bursts lock the timecode */
    dmrEmu_injectGroupCall(CALLER, 9, 1, 1, 30);
    REQUIRE(b.runUntil(CallController::TX_HEADER, 100));
    REQUIRE(b.rec->startTxCount == 2);
    REQUIRE(b.rec->activeTiming == false);
    REQUIRE(b.rec->r40 == R40_TX_RMO);
    REQUIRE(b.rf->enableTx == 2);
    REQUIRE(b.status.dmr_callState == DMR_CALL_TX);
    REQUIRE(b.status.opStatus == TX);

    REQUIRE(b.runUntil(CallController::TX_VOICE, 40));
    REQUIRE(b.rec->headers == 3);
    REQUIRE(b.rec->csbks == 1);
    REQUIRE((b.status.dmr_markers & DMR_MARK_WAKEUP_FAILED) == 0);

    emulator_state.PTTstatus = false;
    n = 0;
    while (b.ctrlState() >= CallController::TX_ARM) {
        b.tick();
        REQUIRE(++n < 40);
    }
    b.ticks(2);
    REQUIRE(b.rf->disableRtx == 2);
    b.drain();
}

/* Transmit until the terminator has been programmed and the controller is
 * back in reception, the RF still keyed for the burst on the air. */
static void toTerminatorSlot(Bench &b)
{
    b.start();
    b.ticks(6);
    emulator_state.PTTstatus = true;
    b.ticks(8);
    REQUIRE(b.ctrlState() == CallController::TX_VOICE);
    emulator_state.PTTstatus = false;
    unsigned n = 0;
    while (b.ctrlState() != CallController::TX_TERM) {
        b.tick();
        REQUIRE(++n < 16);
    }
    b.tick();
    REQUIRE(b.ctrlState() < CallController::TX_ARM);
    REQUIRE(dmr.rfKeyed() == true);
    REQUIRE(b.rf->disableRtx == 0);
}

TEST_CASE("DMR OpMode TX: a configuration change in the slot after the "
          "terminator waits for the RF release",
          "[dmr][rtx][tx]")
{
    Bench b;
    toTerminatorSlot(b);
    REQUIRE(b.rec->configureCount == 1);

    /* A colour code change synced now: the modem is not reprogrammed under
     * the keyed PA (a system event without flags stands for a pass in
     * which nothing releases the RF) */
    struct dmrbbSnapshot s;
    memset(&s, 0x00, sizeof(s));
    b.status.dmr_rxColorCode = 2;
    b.status.dmr_txColorCode = 2;
    dmrEmu_pushSys(&s);
    dmr.update(&b.status, true);
    REQUIRE(b.rec->configureCount == 1);
    REQUIRE(dmr.rfKeyed() == true);
    REQUIRE(b.rf->disableRtx == 0);
    REQUIRE(b.status.opStatus == TX);

    /* The next timeslot releases the RF, then the deferred reprogramming
     * runs with the PA off */
    b.tick();
    REQUIRE(dmr.rfKeyed() == false);
    REQUIRE(b.rf->disableRtx == 1);
    REQUIRE(b.rec->configureCount == 2);
    REQUIRE(b.rec->keyedAtConfigure == false);
    REQUIRE(b.rec->config.colorCode == 2);
    REQUIRE(dmr.controller().config().colorCode == 2);
    REQUIRE(b.ctrlState() == CallController::RX_SEARCH);
}

TEST_CASE("DMR OpMode TX: a mode switch during a transmission drops the RF "
          "before the teardown",
          "[dmr][rtx][tx]")
{
    Bench b;
    b.start();
    b.ticks(6);
    emulator_state.PTTstatus = true;
    b.ticks(8);
    REQUIRE(b.ctrlState() == CallController::TX_VOICE);
    REQUIRE(dmr.rfKeyed() == true);
    REQUIRE(b.rf->disableRtx == 0);

    /* disable() as rtx_task() calls it on a mode change: the PA goes off
     * first, the modem is terminated with the RF already released, and the
     * usual final release of the RF stage follows */
    dmr.disable();
    REQUIRE(b.ctrlState() == CallController::OFF);
    REQUIRE(b.rec->terminateCount == 1);
    REQUIRE(b.rec->keyedAtTerminate == false);
    REQUIRE(b.rf->disableRtx == 2);
    REQUIRE(b.rf->keyed == false);
    REQUIRE(dmr.rfKeyed() == false);
    REQUIRE(dmr.txAudioPathId() < 0);

    /* Not transmitting: the single release of the plain teardown */
    emulator_state.PTTstatus = false;
    dmrEmu_reset();
    radioEmu_reset();
    b.start();
    b.ticks(6);
    dmr.disable();
    REQUIRE(b.rec->keyedAtTerminate == false);
    REQUIRE(b.rf->disableRtx == 1);
}

TEST_CASE("DMR OpMode TX: an RF stage that does not key aborts the "
          "transmission with the RF failed marker",
          "[dmr][rtx][tx]")
{
    Bench b;
    b.start();
    b.ticks(6);
    uint32_t enableRx = b.rf->enableRx;

    /* The synthesizer does not lock: radio_enableTx() reports OFF */
    radioEmu_setTxFails(true);
    emulator_state.PTTstatus = true;
    b.tick();
    REQUIRE(b.rf->enableTx == 1);
    REQUIRE(b.rf->keyed == false);
    REQUIRE(dmr.rfKeyed() == false);
    REQUIRE(b.rf->enableRx == enableRx + 1);
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);
    REQUIRE(b.status.opStatus == RX);
    REQUIRE(b.status.dmr_callState == DMR_CALL_IDLE);
    REQUIRE(b.status.dmr_markers == DMR_MARK_RF_FAILED);
    REQUIRE(b.status.dmr_mon.rfKeyed == 0);
    REQUIRE(dmr.txAudioPathId() < 0);
    REQUIRE(b.rec->startTxCount == 1);
    REQUIRE(b.rec->r40 == R40_RX);

    /* Nothing goes out while the PTT stays pressed */
    b.ticks(6);
    REQUIRE(b.rf->enableTx == 1);
    REQUIRE(b.rec->txSlots == 0);
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);
    REQUIRE(b.status.dmr_markers == DMR_MARK_RF_FAILED);

    /* A new press tries again, the marker cleared; the same outcome */
    emulator_state.PTTstatus = false;
    b.tick();
    emulator_state.PTTstatus = true;
    b.tick();
    REQUIRE(b.rf->enableTx == 2);
    REQUIRE(dmr.rfKeyed() == false);
    REQUIRE(b.status.dmr_markers == DMR_MARK_RF_FAILED);
    REQUIRE(b.ctrlState() == CallController::RX_IDLE);

    /* The lock back: the transmission goes ahead */
    radioEmu_setTxFails(false);
    emulator_state.PTTstatus = false;
    b.tick();
    emulator_state.PTTstatus = true;
    b.tick();
    REQUIRE(b.rf->enableTx == 3);
    REQUIRE(dmr.rfKeyed() == true);
    REQUIRE(b.ctrlState() == CallController::TX_ARM);
    REQUIRE(b.status.opStatus == TX);
    REQUIRE(b.status.dmr_markers == 0);
    REQUIRE(b.status.dmr_mon.rfKeyed == 1);
    emulator_state.PTTstatus = false;
    b.ticks(20);
    REQUIRE(dmr.rfKeyed() == false);
}

TEST_CASE("DMR OpMode TX: a call opening while the RF is still keyed after "
          "the terminator takes the microphone down",
          "[dmr][rtx][tx]")
{
    Bench b;
    state.settings.dmr_hangTime = 1;
    toTerminatorSlot(b);
    pathId mic = dmr.txAudioPathId();
    REQUIRE(audioPath_getStatus(mic) == PATH_OPEN);

    /* A voice LC header from a base station on the own colour code and
     * slot, reported in the slot after the terminator, before any timeslot
     * interrupt or TX-end report released the RF */
    FullLC lc;
    lc.clear();
    lc.flco = FLCO_GRP_V_CH_USR;
    lc.fid = FID_STANDARD;
    lc.dst = TALKGROUP;
    lc.src = CALLER;
    struct dmrbbSnapshot s;
    memset(&s, 0x00, sizeof(s));
    s.irq82 = R82_CTRL_FRAME;
    s.r5f = R5F_SYNC_BS;
    s.r52 = (1 << 4) | R52_TC;
    s.r51 = (uint8_t)((DT_VOICE_LC_HEADER << 4) | R51_CLASS_HEADER);
    lc.toChipLc(s.lc);

    /* The first LC after the modem went back to RX is skipped as stale
     * (goRx()), the second header of the call opens it */
    dmrEmu_pushSys(&s);
    dmr.update(&b.status, false);
    REQUIRE(b.ctrlState() == CallController::RX_HANG);
    dmrEmu_pushSys(&s);
    dmr.update(&b.status, false);
    REQUIRE(b.ctrlState() == CallController::RX_CALL);
    REQUIRE(dmr.rfKeyed() == true);
    REQUIRE(dmr.txAudioPathId() < 0);
    REQUIRE(audioPath_getStatus(mic) == PATH_CLOSED);
    REQUIRE(dmr.rxAudioPathId() >= 0);
    REQUIRE(audioPath_getStatus(dmr.rxAudioPathId()) == PATH_OPEN);
    REQUIRE(dmrAudio_running() == true);
    REQUIRE(b.status.dmr_callState == DMR_CALL_RX);

    /* The RF goes at the next slot, the call carries on */
    b.tick();
    REQUIRE(dmr.rfKeyed() == false);
    REQUIRE(b.ctrlState() == CallController::RX_CALL);
    REQUIRE(dmrAudio_running() == true);
}

TEST_CASE("DMR OpMode TX: the LCSS convention follows the RTX status",
          "[dmr][rtx][tx]")
{
    Bench b;
    b.status.dmr_lcssMode = 1;
    b.start();
    b.ticks(6);
    REQUIRE(dmr.controller().config().lcssMode == LCSS_MODE_MANUAL);
    REQUIRE(b.rec->configureCount == 1);

    emulator_state.PTTstatus = true;
    unsigned n = 0;
    while (b.ctrlState() != CallController::TX_VOICE) {
        b.tick();
        REQUIRE(++n < 10);
    }
    for (unsigned i = 0; i < 12; i++) {
        b.tick();
        REQUIRE(b.rec->r41 == R41_IDLE);
        b.tick();
        REQUIRE(b.rec->r41 == R41_TX);
        REQUIRE(b.rec->r50 == R50_VOICE_MANUAL[i % 6]);
    }
    REQUIRE(b.rec->voiceBursts == 12);
    emulator_state.PTTstatus = false;
    b.ticks(16);
    REQUIRE(b.ctrlState() < CallController::TX_ARM);

    /* Back to the chip-filled convention: no modem reprogramming */
    b.status.dmr_lcssMode = 0;
    dmr.update(&b.status, true);
    REQUIRE(dmr.controller().config().lcssMode == LCSS_MODE_CHIP);
    REQUIRE(b.rec->configureCount == 1);
}
