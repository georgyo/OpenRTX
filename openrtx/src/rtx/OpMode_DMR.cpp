/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "hwconfig.h"

#ifdef CONFIG_DMR

#include <cstring>
#include "core/state.h"
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "interfaces/radio.h"
#include "protocols/DMR/LinkControl.hpp"
#include "rtx/OpMode_DMR.hpp"
#include "rtx/rtx.h"

using namespace DMR;

OpMode_DMR::OpMode_DMR()
    : rxAudioPath(-1)
    , txAudioPath(-1)
    , ready(false)
    , enabled(false)
    , cfgValid(false)
    , cfgPending(false)
    , rxAudioFailed(false)
    , txAudioFailed(false)
    , txVoiceStarted(false)
    , markerRaised(false)
    , markerMs(0)
    , txTimeoutMs(T_TO_S * 1000)
    , rfState(RF_OFF)
    , markers(0)
{
    memset(&bbCfg, 0x00, sizeof(bbCfg));
    memset(&snap, 0x00, sizeof(snap));
    memset(&mon, 0x00, sizeof(mon));
    cfg = ctrl.config();
    ctrl.setPort(&port);
    ctrl.setVoiceSink(&audioLink);
}

OpMode_DMR::~OpMode_DMR()
{
    disable();
}

void OpMode_DMR::enable()
{
    /*
     * The configuration is not known here: rtx_task() calls update() with
     * newCfg set right after enable(), the modem is programmed and the
     * controller started there.
     */
    ready = (dmrbb_init() == 0);
    enabled = true;
    cfgValid = false;
    cfgPending = false;
    rxAudioFailed = false;
    txAudioFailed = false;
    txVoiceStarted = false;
    markerRaised = false;
    rfState = RF_OFF;
    markers = ready ? 0 : DMR_MARK_NOT_SUPPORTED;
    audioLink.reset();

    /* Vocoder backend up, counters cleared, no thread until a call */
    dmrAudio_init();

    /* The controller keeps its markers across restarts: a fresh mode entry
     * starts without any. */
    ctrl.clearMarkers();
    memset(&mon, 0x00, sizeof(mon));
}

void OpMode_DMR::disable()
{
    bool wasEnabled = enabled;

    /*
     * A mode switch in the middle of a transmission: the PA is dropped
     * before anything else, the teardown below (encoder thread join, modem
     * terminate, path releases) would otherwise leave an unmodulated
     * carrier on the air for its whole duration. rtx_task() has already
     * put the chip in the new mode by now, so the modem side of
     * radio_disableRtx() is a no-op on the CS7000 radios and only the
     * TX_PWR_EN line matters.
     */
    if (rfState == RF_TX)
        radio_disableRtx();

    /*
     * The audio thread goes first: its output stream is the Cx000 DAC of
     * the same chip the modem driver reprograms at terminate. Note that
     * rtx_task() calls radio_setOpmode() before disable() on a mode switch,
     * which on the CS7000 radios already terminates the modem driver and
     * sets the chip back to FM: the thread's drain then runs on a chip in
     * FM mode whatever the order here, see docs/dmr_bringup.md 4.4.
     */
    dmrAudio_stop();

    if (enabled) {
        ctrl.disable();
        if (ready)
            dmrbb_terminate();
    }

    enabled = false;
    ready = false;
    cfgValid = false;
    cfgPending = false;
    markerRaised = false;
    markers = 0;
    rfState = RF_OFF;

    platform_ledOff(GREEN);
    platform_ledOff(RED);

    /* Then its paths and its backend */
    audioPath_release(rxAudioPath);
    audioPath_release(txAudioPath);
    rxAudioPath = -1;
    txAudioPath = -1;
    if (wasEnabled)
        dmrAudio_terminate();
    radio_disableRtx();
}

void OpMode_DMR::applyConfig(const rtxStatus_t *const status, bool force)
{
    CallController::Config c = ctrl.config();

    c.ownId = status->dmr_srcId & ADDRESS_MASK;
    c.dstId = status->dmr_dstId & ADDRESS_MASK;
    c.callType = status->dmr_callType;
    c.colorCode = status->dmr_rxColorCode & 0x0F;
    c.timeslot = status->dmr_timeslot;
    c.monitor = status->dmr_monitor;
    c.polite = (status->dmr_polite != 0);

    /*
     * A split channel is a repeater channel: transmit in repeater mode,
     * waking the repeater up when it is silent. A simplex channel is direct
     * mode unless a base station carrier is being received, which the
     * controller decides on its own.
     */
    c.repeater = (status->rxFrequency != status->txFrequency);
    c.hangTimeMs = (uint16_t)(state.settings.dmr_hangTime * 1000);
    c.txTimeoutMs = txTimeoutMs;

    /*
     * LCSS convention of the voice bursts (register 0x50), a runtime choice
     * of the DMR monitor screen until the on-air test settles it
     * (UNVERIFIED on hardware, see DMR::r50Voice()).
     */
    c.lcssMode = (status->dmr_lcssMode != 0) ? LCSS_MODE_MANUAL :
                                               LCSS_MODE_CHIP;

    struct dmrbbConfig bb;
    bb.colorCode = status->dmr_txColorCode & 0x0F;
    bb.timeslot = c.timeslot;
    bb.ownId = c.ownId;
    bb.polite = c.polite;

    /*
     * Mode register 0x10: the driver default unless the DMR monitor screen
     * asked for the alternative word, which then survives the watchdog
     * reconfigurations too (UNVERIFIED on hardware which one the CS7000
     * needs, see docs/dmr_bringup.md).
     */
    bb.modeReg = (status->dmr_modeReg != 0) ? status->dmr_modeReg :
                                              DMRBB_MODE_REG_DEFAULT;

    /*
     * A reconfiguration deferred by an earlier call stays due: cfg/bbCfg
     * already hold the new values, so the comparison alone would miss it.
     */
    bool modemChange = force || !cfgValid || cfgPending
                    || (c.colorCode != cfg.colorCode)
                    || (c.timeslot != cfg.timeslot) || (c.ownId != cfg.ownId)
                    || (c.polite != cfg.polite)
                    || (bb.colorCode != bbCfg.colorCode)
                    || (bb.modeReg != bbCfg.modeReg);

    cfg = c;
    bbCfg = bb;
    cfgValid = true;

    /*
     * Reprogramming the modem restarts the controller: while transmitting it
     * is deferred until the transmission is over, that is until the last
     * burst has gone out and the RF is released, not only until the
     * controller has left its TX states (the terminator or the wake-up
     * CSBK is still on the air during the slot after).
     */
    if (modemChange
        && ((ctrl.state() >= CallController::TX_ARM) || ctrl.report().rfKey)) {
        cfgPending = true;
        return;
    }

    ctrl.configure(cfg);
    cfgPending = false;

    if (modemChange)
        reprogramModem((uint32_t)getTick());
}

void OpMode_DMR::reprogramModem(uint32_t nowMs)
{
    /*
     * The RF stage is never left keyed through a reprogramming (the
     * watchdog asks for one from a transmission that lost its timeslot
     * interrupts): the PA goes off before the modem is reset, so that no
     * unmodulated carrier goes out while the table is written.
     */
    if (rfState == RF_TX)
        keyRf(false, nowMs);

    /*
     * Reprogramming the modem restarts the controller, which drops the call
     * being received: the playback thread is stopped first, before the
     * driver rewrites the registers its DAC stream shares (0x06 on the
     * CS7000 radios). followState() releases the speaker path afterwards,
     * the report of the restarted controller no longer being in a call.
     */
    dmrAudio_stop();
    dmrbb_configure(&bbCfg);
    ctrl.enable(nowMs);
}

void OpMode_DMR::update(rtxStatus_t *const status, const bool newCfg)
{
    if (!enabled)
        return;

    if (!ready) {
        /*
         * No DMR modem on this radio: stay off with the marker raised, the
         * update pace being the one of the empty opMode handler.
         */
        status->opStatus = OFF;
        platform_ledOff(GREEN);
        platform_ledOff(RED);
        publish(status);
        sleepFor(0u, 30u);
        return;
    }

    if (newCfg || !cfgValid)
        applyConfig(status, !cfgValid);

    uint32_t ev = dmrbb_waitEvent(&snap, 30);
    uint32_t now = (uint32_t)getTick();

    if (ev & DMRBB_EV_TS)
        ctrl.onTimeslot(snap, now);
    if (ev & DMRBB_EV_SYS)
        ctrl.onSysEvent(snap, now);
    if ((ev & (DMRBB_EV_TS | DMRBB_EV_SYS)) == 0)
        ctrl.onTimeout(now);

    bool ptt = platform_getPttStatus() && (status->txDisable == 0);
    ctrl.setPtt(ptt, now);

    /* A deferred reconfiguration waits for the last burst to go out too */
    if (cfgPending && (ctrl.state() < CallController::TX_ARM)
        && !ctrl.report().rfKey)
        applyConfig(status, true);

    /* No timeslot interrupt for two seconds: reprogram the modem */
    if (ctrl.report().needsReconfigure)
        reprogramModem(now);

    followState(now);
    monitor(ev);
    publish(status);

    /* Operating status for the RSSI filter and the UI: TX while keyed */
    status->opStatus = (rfState == RF_TX) ? TX : RX;
}

void OpMode_DMR::keyRf(bool key, uint32_t nowMs)
{
    if (key && (rfState != RF_TX)) {
        radio_enableTx();

        /*
         * The driver refuses to key the PA when the synthesizer does not
         * lock (radio_CS7000.cpp, issue #436) and reports OFF: nothing
         * would go out, so the transmission is aborted at once with the
         * "RF failed" marker instead of running headers, voice and
         * terminator into a parked VCO while the display says TX. The
         * receiver is put back on, the PTT stays latched until released.
         */
        if (radio_getStatus() != TX) {
            ctrl.rfFailed(nowMs);
            radio_enableRx();
            rfState = RF_RX;
            return;
        }

        rfState = RF_TX;
        if (mon.rfKeyed < UINT16_MAX)
            mon.rfKeyed++;
    } else if (!key && (rfState != RF_RX)) {
        /*
         * The RF chain is released as the FM and M17 handlers do: the TX
         * stage off, then the receiver back on. The modem itself was put
         * back into reception by the controller when it programmed the
         * last burst.
         */
        if (rfState == RF_TX)
            radio_disableRtx();
        radio_enableRx();
        rfState = RF_RX;
    }
}

void OpMode_DMR::followState(uint32_t nowMs)
{
    const CallController::Report &r = ctrl.report();

    /*
     * RF stage: keyed from the moment the modem is armed, before the first
     * header slot is programmed, until the terminator (or the wake-up CSBK)
     * has gone out; released at once by a rejected or aborted transmission.
     * The controller tracks that in its report. An RF stage that refuses
     * to key aborts the transmission here, hence the state is read after.
     */
    keyRf(r.rfKey, nowMs);
    bool tx = (ctrl.state() >= CallController::TX_ARM);

    /*
     * Microphone path: for the whole transmission, wake-up included, so
     * that the microphone is up before it is needed. The encoder thread
     * only from the headers on: its payloads go out from the first voice
     * burst, and a queue filled through a repeater wake-up would have its
     * oldest payloads dropped as overruns for nothing. What the three
     * headers leave in the queue is flushed down to the newest payload when
     * the voice bursts begin, so that the first one carries the audio of
     * the last 60 ms, not of the headers (once per transmission, before
     * the first burst is programmed: the controller enters TX_VOICE on the
     * last header, the first voice burst comes at the next own slot).
     *
     * The encoder is stopped, and the path released, only once the RF is
     * released: stopping it joins the thread, which may be blocked on the
     * microphone stream for up to one 60 ms block, and that stall must not
     * sit between the programming of the terminator and the idling of the
     * slot after it, nor delay the RF release. No payload is requested in
     * TX_TERM, so a running encoder is harmless there. A call opening
     * while the RF is still keyed takes the microphone down first, so
     * that the playback can take over the audio thread.
     */
    bool voice = (ctrl.state() == CallController::TX_VOICE);
    if (tx && (ctrl.state() != CallController::TX_TERM)) {
        if (audioPath_getStatus(txAudioPath) == PATH_CLOSED) {
            txAudioPath = audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX);
            txAudioFailed = false;
        }

        if ((ctrl.state() >= CallController::TX_HEADER)
            && (audioPath_getStatus(txAudioPath) == PATH_OPEN) && !txAudioFailed
            && (dmrAudio_running() == false))
            txAudioFailed = !dmrAudio_startTx(txAudioPath);

        if (voice && !txVoiceStarted)
            audioLink.startVoice();
    } else if (!tx && (txAudioPath >= 0)
               && (!r.rfKey || (r.callState == DMR_CALL_RX))) {
        dmrAudio_stop();
        audioPath_release(txAudioPath);
        txAudioPath = -1;
    }
    txVoiceStarted = voice;

    /*
     * Speaker path: opened when a call is received, kept through the hang
     * time (a repeater keeps the channel for the answer), released after.
     * The playback thread runs while voice is being received: it is stopped
     * when the call ends, so that the hang time is not silence "underruns",
     * and started again by the call opening during the hang time. A path
     * suspended by a higher priority one (a voice prompt) makes the thread
     * quit on its own; it is restarted here once the path is open again. A
     * start refused on an open path (no device) is not retried until the
     * path is requested anew.
     */
    if (r.callState == DMR_CALL_RX) {
        if (audioPath_getStatus(rxAudioPath) == PATH_CLOSED) {
            rxAudioPath = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_RX);
            rxAudioFailed = false;
        }

        if ((audioPath_getStatus(rxAudioPath) == PATH_OPEN) && !rxAudioFailed
            && (dmrAudio_running() == false))
            rxAudioFailed = !dmrAudio_startRx(rxAudioPath);
    } else if (r.callState == DMR_CALL_RX_HANG) {
        if ((rxAudioPath >= 0) && dmrAudio_running())
            dmrAudio_stop();
    } else if (rxAudioPath >= 0) {
        dmrAudio_stop();
        audioPath_release(rxAudioPath);
        rxAudioPath = -1;
    }

    /*
     * Markers: the controller keeps them until the next PTT press, the UI
     * shows them for MARKER_TIME_MS at most.
     */
    uint8_t m = 0;
    if (r.noId)
        m |= DMR_MARK_NO_ID;
    if (r.busy)
        m |= DMR_MARK_BUSY;
    if (r.wakeupFailed)
        m |= DMR_MARK_WAKEUP_FAILED;
    if (r.rfFailed)
        m |= DMR_MARK_RF_FAILED;

    if (m != 0) {
        if (!markerRaised) {
            markerRaised = true;
            markerMs = nowMs;
        } else if ((nowMs - markerMs) >= MARKER_TIME_MS) {
            ctrl.clearMarkers();
            markerRaised = false;
            m = 0;
        }
    } else {
        markerRaised = false;
    }

    markers = m;

    /* LEDs: red while transmitting, green while a call is received */
    if (tx) {
        platform_ledOff(GREEN);
        platform_ledOn(RED);
    } else {
        platform_ledOff(RED);
        if (r.callState == DMR_CALL_RX)
            platform_ledOn(GREEN);
        else
            platform_ledOff(GREEN);
    }
}

void OpMode_DMR::monitor(uint32_t ev)
{
    mon.timecode = ctrl.report().timecode;

    /* Transmit side of the audio layer, see DmrAudioLink */
    mon.txBursts = audioLink.txRequests;
    mon.txSilent = audioLink.txRequests - audioLink.txFrames;
    mon.txUnderruns = (audioLink.txUnderruns > UINT16_MAX) ?
                          UINT16_MAX :
                          (uint16_t)audioLink.txUnderruns;

    /*
     * The interrupt counters are refreshed by the driver on every wait,
     * timeouts included: a line that pulses without raising an event (PC3
     * while the pinmap expects SYS on PC1) is only visible through them, so
     * they are published before the event gating of the register fields.
     */
    mon.tsCount = snap.tsCount;
    mon.sysCount = snap.sysCount;
    mon.rftxCount = snap.rftxCount;
    memcpy(mon.lineCount, snap.lineCount, sizeof(mon.lineCount));

    if (ev == DMRBB_EV_TIMEOUT)
        return;

    /* Per-slot measurements come with every event */
    mon.rssi = snap.rssi;
    mon.fskErr = snap.fskErr;

    if (ev & DMRBB_EV_TS) {
        mon.r52 = snap.r52;
        mon.r42 = snap.r42;
    }

    if (ev & DMRBB_EV_SYS) {
        mon.irq82 = snap.irq82;
        mon.sub84 = snap.sub84;
        mon.sub86 = snap.sub86;
        mon.r51 = snap.r51;
        mon.r52 = snap.r52;
        mon.r5f = snap.r5f;

        /*
         * The LC RAM is kept from the last header, terminator or late
         * entry: what the state machine decodes, shown raw next to its
         * parsed fields and to the chip's own CRC verdict.
         */
        bool header = ((snap.irq82 & R82_CTRL_FRAME) != 0)
                   && ((snap.r51 & R51_SYNC_CLASS_MASK) == R51_CLASS_HEADER);
        if (header || ((snap.irq82 & R82_LATE_ENTRY) != 0)) {
            FullLC lc;
            bool parsed = lc.fromChipLc(snap.lc);

            memcpy(mon.lc, snap.lc, sizeof(mon.lc));
            mon.lcSrc = lc.src;
            mon.lcDst = lc.dst;
            mon.lcFlco = lc.flco;
            mon.lcFid = lc.fid;
            mon.lcValid = ((snap.r51 & R51_CRC_BAD) == 0) ? 0x01 : 0x00;
            if (parsed)
                mon.lcValid |= 0x02;
        }
    }
}

void OpMode_DMR::publish(rtxStatus_t *const status)
{
    const CallController::Report &r = ctrl.report();

    status->dmr_lcOk = r.lcOk;
    status->dmr_rxSrcId = r.rxSrc;
    status->dmr_rxDstId = r.rxDst;
    status->dmr_rxFlco = r.rxFlco;
    status->dmr_rxColorCodeSeen = r.rxColorCodeSeen;
    status->dmr_rxTimeslot = r.rxTimeslot;
    status->dmr_rxSyncType = r.rxSyncType;
    status->dmr_callState = r.callState;
    status->dmr_slotLock = r.slotLock;
    status->dmr_markers = markers;
    status->dmr_mon = mon;
    dmrAudio_getStats(&status->dmr_audio);
}

#endif /* CONFIG_DMR */
