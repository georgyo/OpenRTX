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
    , markerRaised(false)
    , markerMs(0)
    , rfState(RF_OFF)
    , markers(0)
{
    memset(&bbCfg, 0x00, sizeof(bbCfg));
    memset(&snap, 0x00, sizeof(snap));
    cfg = ctrl.config();
    ctrl.setPort(&port);
    ctrl.setVoiceSink(&audioStub);
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
    markerRaised = false;
    rfState = RF_OFF;
    markers = ready ? 0 : DMR_MARK_NOT_SUPPORTED;
    audioStub.reset();
}

void OpMode_DMR::disable()
{
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
    audioPath_release(rxAudioPath);
    audioPath_release(txAudioPath);
    rxAudioPath = -1;
    txAudioPath = -1;
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

    struct dmrbbConfig bb;
    bb.colorCode = status->dmr_txColorCode & 0x0F;
    bb.timeslot = c.timeslot;
    bb.ownId = c.ownId;
    bb.polite = c.polite;
    bb.modeReg = DMRBB_MODE_REG_DEFAULT;

    bool modemChange = force || !cfgValid || (c.colorCode != cfg.colorCode)
                    || (c.timeslot != cfg.timeslot) || (c.ownId != cfg.ownId)
                    || (c.polite != cfg.polite)
                    || (bb.colorCode != bbCfg.colorCode);

    cfg = c;
    bbCfg = bb;
    cfgValid = true;

    /*
     * Reprogramming the modem restarts the controller: while transmitting it
     * is deferred until the transmission is over.
     */
    if (modemChange && (ctrl.state() >= CallController::TX_ARM)) {
        cfgPending = true;
        return;
    }

    ctrl.configure(cfg);
    cfgPending = false;

    if (modemChange) {
        dmrbb_configure(&bbCfg);
        ctrl.enable((uint32_t)getTick());
    }
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

    if (cfgPending && (ctrl.state() < CallController::TX_ARM))
        applyConfig(status, true);

    if (ctrl.report().needsReconfigure) {
        /* No timeslot interrupt for two seconds: reprogram the modem */
        dmrbb_configure(&bbCfg);
        ctrl.enable(now);
    }

    followState(now);
    publish(status);

    /* Operating status for the RSSI filter and the UI */
    status->opStatus = (ctrl.state() >= CallController::TX_ARM) ? TX : RX;
}

void OpMode_DMR::followState(uint32_t nowMs)
{
    const CallController::Report &r = ctrl.report();
    bool tx = (ctrl.state() >= CallController::TX_ARM);

    /* RF stage: TX while the controller transmits, RX otherwise */
    if (tx && (rfState != RF_TX)) {
        radio_enableTx();
        rfState = RF_TX;
    } else if (!tx && (rfState != RF_RX)) {
        radio_enableRx();
        rfState = RF_RX;
    }

    /*
     * Speaker path: opened when a call is received, kept through the hang
     * time (a repeater keeps the channel for the answer), released after.
     */
    if (r.callState == DMR_CALL_RX) {
        if (audioPath_getStatus(rxAudioPath) == PATH_CLOSED)
            rxAudioPath = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_RX);
    } else if (r.callState != DMR_CALL_RX_HANG) {
        if (rxAudioPath >= 0) {
            audioPath_release(rxAudioPath);
            rxAudioPath = -1;
        }
    }

    /* Microphone path: for the whole transmission, wake-up included */
    if (tx) {
        if (audioPath_getStatus(txAudioPath) == PATH_CLOSED)
            txAudioPath = audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX);
    } else if (txAudioPath >= 0) {
        audioPath_release(txAudioPath);
        txAudioPath = -1;
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
}

#endif /* CONFIG_DMR */
