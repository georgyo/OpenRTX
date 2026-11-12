/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <cstring>
#include "protocols/DMR/CallController.hpp"
#include "protocols/DMR/LinkControl.hpp"

extern "C" {
#include "rtx/rtx.h"
}

using namespace DMR;

/*
 * Wrap-safe elapsed time in milliseconds.
 */
static inline uint32_t elapsed(uint32_t now, uint32_t since)
{
    return now - since;
}

CallController::CallController() : port(nullptr), sink(nullptr), st(OFF)
{
    memset(&cfg, 0x00, sizeof(cfg));
    cfg.dstId = 9;
    cfg.callType = GROUP;
    cfg.colorCode = 1;
    cfg.timeslot = 1;
    cfg.polite = true;
    cfg.hangTimeMs = T_CALLHT_MS;
    cfg.wakeupAttempts = N_WAKEUP;
    cfg.syncWuMs = T_SYNCWU_MS;
    cfg.txTimeoutMs = T_TO_S * 1000;
    cfg.lcssMode = LCSS_MODE_CHIP;

    memset(&rep, 0x00, sizeof(rep));
    memset(txLc, 0x00, sizeof(txLc));
    memset(voiceBuf, 0x00, sizeof(voiceBuf));

    timecode = 0;
    tcValid = false;
    agree = 0;
    disagree = 0;
    slotLock = 0;
    lastSync = R5F_SYNC_MS;
    lastTsMs = 0;
    lastTsCount = 0;
    lastTsTick = 0;
    lastBsSyncMs = 0;
    idleSlots = 0;
    callSlot = 0;
    callTimecode = 0;
    callCc = 0;
    callDmo = false;
    lastVoiceMs = 0;
    hangStartMs = 0;
    ptt = false;
    pttLatched = false;
    activeTiming = true;
    armed = false;
    txTimedOut = false;
    headers = 0;
    seq = 0;
    superframes = 0;
    wakeups = 0;
    wakeupPhase = WU_SEND;
    armStartMs = 0;
    txStartMs = 0;
    wakeupSentMs = 0;
    rfUnkeyPending = false;
    rfUnkeyMs = 0;
}

void CallController::setPort(ModemPort *p)
{
    port = p;
}

void CallController::setVoiceSink(VoiceSink *s)
{
    sink = s;
}

void CallController::configure(const Config &c)
{
    uint8_t rxWas = rxSlot();

    cfg = c;
    if (cfg.timeslot < 1)
        cfg.timeslot = 1;
    if (cfg.timeslot > 2)
        cfg.timeslot = 2;
    cfg.colorCode &= 0x0F;
    if (cfg.lcssMode > LCSS_MODE_MANUAL)
        cfg.lcssMode = LCSS_MODE_CHIP;

    /*
     * The slots of a receiver that has slot interrupts take the new
     * colour code gate at the next one; a searching receiver has none
     */
    if ((port != nullptr) && (st == RX_SEARCH) && (rxSlot() != rxWas))
        port->setNextSlot(rxSlot());
}

void CallController::enable(uint32_t nowMs)
{
    if (port == nullptr)
        return;

    /* The markers outlive a restart: the owner clears them */
    bool noId = rep.noId;
    bool busy = rep.busy;
    bool wakeupFailed = rep.wakeupFailed;
    bool rfFail = rep.rfFailed;

    memset(&rep, 0x00, sizeof(rep));
    rep.noId = noId;
    rep.busy = busy;
    rep.wakeupFailed = wakeupFailed;
    rep.rfFailed = rfFail;

    ptt = false;
    pttLatched = false;
    rfUnkeyPending = false;
    lastBsSyncMs = nowMs - CARRIER_TIMEOUT_MS;
    lastSync = R5F_SYNC_MS;
    callSlot = 0;
    callDmo = false;

    goRx(nowMs, RX_SEARCH);
    publish();
}

void CallController::disable()
{
    if ((st != OFF) && (port != nullptr))
        port->idle();

    st = OFF;
    ptt = false;
    pttLatched = false;
    slotLock = 0;
    tcValid = false;
    unkeyRf();
    rep.lcOk = false;
    rep.hangActive = false;
    rep.needsReconfigure = false;
    publish();
}

void CallController::clearMarkers()
{
    rep.noId = false;
    rep.busy = false;
    rep.wakeupFailed = false;
    rep.rfFailed = false;
}

void CallController::rfFailed(uint32_t nowMs)
{
    if ((st == OFF) || (port == nullptr) || !isTx())
        return;

    rep.rfFailed = true;
    abortTx(nowMs);
    publish();
}

/*
 * Timing
 */

bool CallController::isTx() const
{
    return st >= TX_ARM;
}

bool CallController::carrierPresent(uint32_t nowMs) const
{
    return (lastSync == R5F_SYNC_BS)
        && (elapsed(nowMs, lastBsSyncMs) < CARRIER_TIMEOUT_MS);
}

void CallController::resetTiming()
{
    tcValid = false;
    agree = 0;
    disagree = 0;
    slotLock = 0;
}

void CallController::requestAxis(uint32_t nowMs)
{
    /*
     * A new time axis was asked to the modem (startRx, startTx): the
     * watchdog deadline is measured from the request, not from the last
     * interrupt of the previous axis.
     */
    lastTsMs = nowMs;
}

uint32_t CallController::missedSlots(const struct dmrbbSnapshot &s) const
{
    /*
     * Timeslot interrupts that arrived before the thread woke up are merged
     * into one event by the driver's mailbox (one pending bit), but the
     * local timecode must toggle once per slot. The ISR counter tells how
     * many interrupts were merged; the interrupt timestamp covers the case
     * of edges lost by the interrupt line itself, where the counter moves
     * by one while more than one slot went by. A driver that fills neither
     * (both left at zero) is trusted to deliver every interrupt.
     */
    uint32_t missed = 0;

    if (s.tsCount != lastTsCount) {
        uint32_t delta = s.tsCount - lastTsCount;
        missed = delta - 1;
    }

    if (s.tsTick != lastTsTick) {
        uint32_t gap = s.tsTick - lastTsTick;
        uint32_t slots = (gap + TS_TICK_TOLERANCE_MS) / BURST_MS;
        if ((slots > 1) && ((slots - 1) > missed))
            missed = slots - 1;
    }

    return missed;
}

void CallController::trackTimecode(uint8_t rxTc)
{
    /*
     * The local timecode toggles at every timeslot interrupt and is compared
     * with the CACH TC the chip reports in register 0x52 bit2, which is
     * taken as the TC of the slot starting at this interrupt (UNVERIFIED on
     * hardware: if the chip still reports the previous slot's CACH at the
     * interrupt, invert rxTc here). Lock after TIMECODE_LOCK_COUNT agreeing
     * slots, re-align after TIMECODE_DROP_COUNT disagreeing ones (OpenGD77
     * practice). The re-alignment is suspended while a direct mode call is
     * open and while transmitting: there is no CACH to follow in DMO
     * (TS 102 361-1 §4.2.2, the CACH exists on the BS outbound only) and
     * the slot parity of an ongoing call or transmission must never flip.
     */
    if (!tcValid) {
        timecode = rxTc;
        tcValid = true;
        agree = 1;
        disagree = 0;
        slotLock = 1;
        return;
    }

    timecode ^= 1;
    if (slotLock == 0)
        slotLock = 1;

    bool frozen = ((st == RX_CALL) && callDmo) || ((st == TX_ARM) && armed)
               || ((st == TX_WAKEUP) && (wakeupPhase != WU_WAIT))
               || (st >= TX_HEADER);

    if (rxTc == timecode) {
        disagree = 0;
        if (agree < TIMECODE_LOCK_COUNT)
            agree++;
        if (agree >= TIMECODE_LOCK_COUNT)
            slotLock = 2;
    } else {
        agree = 0;
        if (disagree < TIMECODE_DROP_COUNT)
            disagree++;
        if ((disagree >= TIMECODE_DROP_COUNT) && !frozen) {
            slotLock = 1;
            timecode = rxTc;
            disagree = 0;
        }
    }
}

void CallController::runTimers(uint32_t nowMs)
{
    runRfTimer(nowMs);

    /*
     * T_CallHt runs on its own flag and timer: the hang time of a direct
     * mode call and of an own transmission is spent searching for a new
     * sync (RX_SEARCH), only a repeater call keeps its slots (RX_HANG).
     */
    if (rep.hangActive && (elapsed(nowMs, hangStartMs) >= cfg.hangTimeMs)) {
        rep.hangActive = false;
        if (st == RX_HANG)
            st = RX_IDLE;
    }

    switch (st) {
        case RX_CALL:
            /* No voice for two superframes: the call is gone */
            if (elapsed(nowMs, lastVoiceMs) > VOICE_TIMEOUT_MS)
                enterHang(nowMs, callDmo);
            break;

        case TX_WAKEUP:
            /*
             * Waiting for the repeater's answer: on a silent channel there
             * is no timeslot interrupt to drive this, so T_SyncWu is kept
             * here (TS 102 361-1 Annex F.2). A carrier that has just been
             * locked wins over the expiry.
             */
            if ((wakeupPhase == WU_WAIT)
                && !(carrierPresent(nowMs) && (slotLock == 2))
                && (elapsed(nowMs, wakeupSentMs) >= cfg.syncWuMs))
                enterWakeup(nowMs);
            break;

        default:
            break;
    }
}

void CallController::publish()
{
    rep.state = st;
    rep.slotLock = slotLock;
    rep.timecode = timecode;

    switch (st) {
        case RX_CALL:
            rep.callState = DMR_CALL_RX;
            break;
        case TX_WAKEUP:
            rep.callState = DMR_CALL_TX_WAKEUP;
            break;
        case TX_ARM:
        case TX_HEADER:
        case TX_VOICE:
        case TX_TERM:
            rep.callState = DMR_CALL_TX;
            break;
        default:
            /* The hang time is shown whatever the receiver is doing */
            rep.callState = rep.hangActive ? DMR_CALL_RX_HANG : DMR_CALL_IDLE;
            break;
    }
}

/*
 * Reception
 */

uint8_t CallController::rxSlot() const
{
    /*
     * Monitor "any colour code": the chip itself must stop requiring its
     * local colour code on what it deframes (register 0x41 bit3), the
     * software filter of colorCodeAccepted() comes after it.
     */
    return (cfg.monitor >= 2) ? R41_RX_ANY_CC : R41_RX;
}

uint8_t CallController::slotOf(const struct dmrbbSnapshot &s) const
{
    /*
     * A BS sourced burst carries the timeslot in the CACH TC bit (TS 102
     * 361-1 §7.1.4), a DMO burst identifies its slot through the TDMA sync
     * pattern (§9.1.1 Table 9.2) and an MS sourced burst tells nothing.
     */
    switch (s.r5f & R5F_SYNC_MASK) {
        case R5F_SYNC_BS:
            return (s.r52 & R52_TC) ? 2 : 1;
        case R5F_SYNC_TDMA1:
            return 1;
        case R5F_SYNC_TDMA2:
            return 2;
        default:
            return 0;
    }
}

bool CallController::burstIsCall(const struct dmrbbSnapshot &s) const
{
    uint8_t slot = slotOf(s);
    return (slot == 0) || (callSlot == 0) || (slot == callSlot);
}

bool CallController::voiceOfCall(const struct dmrbbSnapshot &s) const
{
    /*
     * A voice burst A to F of the open call. The tests are the ones both
     * OpenGD77 and OpenMDUV380 apply to every burst while receiving: the
     * chip's check flag (0x51 bit 2, manual chapter 8 "received data
     * verification result") and privacy indicator (bit 3) clear, and the
     * colour code of the call. The colour code travels in the EMB of bursts
     * B to F; burst A (sync class 01) carries the voice sync in its place
     * (TS 102 361-1 §9.1.2), so register 0x52 is not tested on it.
     * UNVERIFIED on hardware: both reference firmwares test burst A too
     * and it passes on their radios, the chip presumably keeps the last
     * colour code there.
     */
    if ((r51VoiceSeq(s.r51) == 0) || !burstIsCall(s))
        return false;

    if ((s.r51 & (R51_CRC_BAD | R51_PI)) != 0)
        return false;

    bool burstA = ((s.r51 & R51_SYNC_CLASS_MASK) == R51_CLASS_VOICE);
    return burstA || ((s.r52 >> 4) == callCc);
}

bool CallController::colorCodeAccepted(uint8_t cc) const
{
    return (cfg.monitor >= 2) || (cc == cfg.colorCode);
}

bool CallController::slotAccepted(uint8_t slot) const
{
    return (cfg.monitor >= 2) || (slot == 0) || (slot == cfg.timeslot);
}

bool CallController::terminatorOfCall(const struct dmrbbSnapshot &s) const
{
    /*
     * TS 102 361-1 §5.2.1.4: the hang time terminators carry the source and
     * destination of the voice call in progress. After an own transmission
     * the call in progress is the one of the LC left in the TX RAM.
     */
    FullLC lc;
    if (!lc.fromChipLc(s.lc))
        return false;

    if (rep.lcOk)
        return (lc.src == rep.rxSrc) && (lc.dst == rep.rxDst);

    return memcmp(&s.lc[3], &txLc[3], 6) == 0;
}

void CallController::goRx(uint32_t nowMs, State next)
{
    /*
     * startRx() makes the chip drop its time axis and search for a sync
     * (register 0x41 bit5 SyncFail, "the physical layer is required to
     * search again"). Every caller but the wake-up wait passes RX_SEARCH:
     * the timeslot watchdog is idle in that state only, and whether the
     * chip keeps giving timeslot interrupts while it searches is
     * UNVERIFIED on hardware.
     */
    port->startRx();
    if (rxSlot() != R41_RX)
        port->setNextSlot(rxSlot());
    resetTiming();
    requestAxis(nowMs);
    idleSlots = 0;
    st = next;
}

void CallController::enterHang(uint32_t nowMs, bool research)
{
    hangStartMs = nowMs;
    rep.hangActive = (cfg.hangTimeMs != 0);

    /*
     * Direct mode has no timing master (TS 102 361-1 §5.2.1.3): the next
     * transmission, the answer in particular, comes with a time axis of
     * its own, while the chip only follows a sync within 1.25 ms of the
     * axis it has (manual §5.4.4). The call's axis is therefore dropped
     * with the call, as OpenGD77 and OpenMDUV380 do at the end of every
     * direct mode call. A repeater call keeps the repeater's axis.
     */
    if (research)
        goRx(nowMs, RX_SEARCH);
    else
        st = rep.hangActive ? RX_HANG : RX_IDLE;
}

bool CallController::syncConfirmed(const struct dmrbbSnapshot &s) const
{
    /*
     * Events showing that the chip's time axis is the one of a real
     * signal. Voice bursts carry no check of their own and the empty slot
     * of an idle receiver may report noise as a frame, so outside a call
     * only a data burst with a good CRC and an accepted colour code or a
     * late entry count. On a repeater channel the voice of a call that was
     * not opened (another talkgroup or timeslot) counts too when it comes
     * with the BS sync and the accepted colour code, so that the
     * repeater's axis and the timecode lock survive somebody else's call.
     */
    if (s.irq82 & R82_LATE_ENTRY)
        return true;

    if ((s.irq82 & R82_CTRL_FRAME) == 0)
        return false;

    uint8_t cls = s.r51 & R51_SYNC_CLASS_MASK;
    uint8_t cc = s.r52 >> 4;

    if (cls == R51_CLASS_DATA)
        return ((s.r51 & R51_CRC_BAD) == 0) && colorCodeAccepted(cc);

    if ((r51VoiceSeq(s.r51) == 0) || ((s.r51 & (R51_CRC_BAD | R51_PI)) != 0))
        return false;

    if (st == RX_CALL)
        return voiceOfCall(s);

    if (!cfg.repeater || ((s.r5f & R5F_SYNC_MASK) != R5F_SYNC_BS))
        return false;

    /* Burst A carries no colour code, see voiceOfCall() */
    return (cls == R51_CLASS_VOICE) || colorCodeAccepted(cc);
}

bool CallController::tryOpenCall(const struct dmrbbSnapshot &s, uint32_t nowMs)
{
    uint8_t cc = s.r52 >> 4;
    uint8_t slot = slotOf(s);

    if (!colorCodeAccepted(cc) || !slotAccepted(slot))
        return false;

    /* PF or R set, reserved FID, unknown FLCO: rejected by unpack() */
    FullLC lc;
    if (!lc.fromChipLc(s.lc))
        return false;

    /* An LC RAM never written reads as a call from nobody */
    if (lc.src == 0)
        return false;

    if (!lc.matches(cfg.ownId, cfg.dstId, cfg.callType, cfg.monitor))
        return false;

    bool bs = ((s.r5f & R5F_SYNC_MASK) == R5F_SYNC_BS);

    st = RX_CALL;
    callSlot = slot;
    callCc = cc;
    callDmo = !bs;

    /*
     * Local timecode of the call's slot, used in direct mode to open only
     * that slot. The burst reported by this system interrupt belongs to the
     * slot that ended at the last timeslot interrupt, whose timecode is the
     * complement of the current one (t3 = 4 ms after the slot end, manual
     * Figure 5.12). UNVERIFIED on hardware.
     */
    callTimecode = bs ? (uint8_t)(slot - 1) : (uint8_t)(timecode ^ 1);

    rep.lcOk = true;
    rep.lcManufacturer = lc.manufacturer;
    rep.rxSrc = lc.src;
    rep.rxDst = lc.dst;
    rep.rxFlco = lc.flco;
    rep.rxTimeslot = (slot != 0) ? slot : cfg.timeslot;
    rep.rxSyncType = bs ? 1 : 0;
    rep.hangActive = false;
    lastVoiceMs = nowMs;
    idleSlots = 0;

    return true;
}

void CallController::processControlFrame(const struct dmrbbSnapshot &s,
                                         uint32_t nowMs)
{
    uint8_t cls = s.r51 & R51_SYNC_CLASS_MASK;

    if (cls == R51_CLASS_RC)
        return;

    if (cls != R51_CLASS_DATA) {
        if ((st == RX_CALL) && voiceOfCall(s)) {
            /*
             * Direct mode: the call's slot is latched again from this burst
             * only when the gate had lapsed before it, that is when both
             * slots were open. While the gate holds only the latched slot
             * is received, and a burst reported late (after the next
             * timeslot interrupt) would latch the other one.
             */
            if (callDmo && (elapsed(nowMs, lastVoiceMs) >= DMO_GATE_HOLD_MS))
                callTimecode = (uint8_t)(timecode ^ 1);

            lastVoiceMs = nowMs;
            if (s.voiceValid && !rep.lcManufacturer && (sink != nullptr))
                sink->rxVoiceFrame(s.voice);
        }
        return;
    }

    if (s.r51 & R51_CRC_BAD)
        return;

    uint8_t type = s.r51 >> 4;

    /*
     * A burst of another colour code is co-channel interference from
     * another site (TS 102 361-1 §5.2.1.4 note): it neither ends the call
     * nor extends its hang time.
     */
    bool onCall = burstIsCall(s) && colorCodeAccepted(s.r52 >> 4);

    switch (type) {
        case DT_VOICE_LC_HEADER:
            if ((st == RX_CALL) && burstIsCall(s)) {
                /* Header on the call's slot: repeated, or a new call */
                if (!tryOpenCall(s, nowMs))
                    enterHang(nowMs, false);
            } else {
                tryOpenCall(s, nowMs);
            }
            break;

        case DT_TERMINATOR_LC:
            if (!onCall)
                break;
            if (st == RX_CALL)
                enterHang(nowMs, callDmo);
            else if (rep.hangActive && terminatorOfCall(s))
                hangStartMs = nowMs; /* hang time terminator: keep shown */
            break;

        default:
            /*
             * Any other data burst on the call's slot ends the call. The
             * chip is in sync with whoever sent it: the axis is kept.
             */
            if ((st == RX_CALL) && onCall)
                enterHang(nowMs, false);
            break;
    }
}

/*
 * Transmission
 */

void CallController::buildTxLc()
{
    FullLC lc;
    lc.clear();

    switch (cfg.callType) {
        case PRIVATE:
            lc.flco = FLCO_UU_V_CH_USR;
            lc.dst = cfg.dstId & ADDRESS_MASK;
            break;

        case ALL:
            /* TS 102 361-2 §5.3.2.1: all unit Id, broadcast option */
            lc.flco = FLCO_GRP_V_CH_USR;
            lc.dst = ADDRESS_ALL;
            lc.serviceOptions = SO_BROADCAST;
            break;

        case GROUP:
        default:
            lc.flco = FLCO_GRP_V_CH_USR;
            lc.dst = cfg.dstId & ADDRESS_MASK;
            break;
    }

    lc.fid = FID_STANDARD;
    lc.src = cfg.ownId & ADDRESS_MASK;
    lc.toChipLc(txLc);
}

void CallController::armTx(uint32_t nowMs, bool active)
{
    activeTiming = active;
    port->setAccess(cfg.polite ? R21_POLITE : R21_IMPOLITE);
    port->startTx(active);
    armed = true;
    keyRf();
    buildTxLc();

    /* Active timing: the modem provides a fresh axis from now on */
    if (active)
        requestAxis(nowMs);
}

void CallController::enterTxArm(uint32_t nowMs)
{
    armStartMs = nowMs;
    armed = false;
    txTimedOut = false;
    headers = 0;
    seq = 0;
    superframes = 0;
    wakeups = 0;
    st = TX_ARM;
    rep.hangActive = false;

    /*
     * Direct mode (own timing) when there is no repeater to follow: no
     * timing at all, or the last sync came from an MS. Repeater mode
     * (passive timing) on a repeater channel or when a BS sync is being
     * received with the timecode locked. A repeater channel without an
     * outbound carrier is woken up first.
     */
    bool rmo = cfg.repeater || (carrierPresent(nowMs) && (slotLock == 2));

    if (!rmo) {
        armTx(nowMs, true);
        return;
    }

    if (carrierPresent(nowMs)) {
        if (slotLock == 2)
            armTx(nowMs, false);
        /* else: wait for the timecode lock in TX_ARM */
        return;
    }

    enterWakeup(nowMs);
}

void CallController::enterWakeup(uint32_t nowMs)
{
    if (wakeups >= cfg.wakeupAttempts) {
        /* TS 102 361-1 Annex F.2: give up after N_Wakeup attempts */
        rep.wakeupFailed = true;
        pttLatched = true;
        unkeyRf();
        if (st == TX_WAKEUP && wakeupPhase == WU_WAIT) {
            /* Already receiving: idle if slots ticked, else searching */
            st = (slotLock != 0) ? RX_IDLE : RX_SEARCH;
            idleSlots = 0;
        } else {
            goRx(nowMs, RX_SEARCH);
        }
        return;
    }

    wakeups++;
    st = TX_WAKEUP;
    wakeupPhase = WU_SEND;

    /*
     * BS_Dwn_Act CSBK (TS 102 361-2 §7.1.2.1) in the TX RAM, CRC left to
     * the chip like the LC parity (UNVERIFIED on hardware). The wake-up
     * burst is sent with the chip's own timing since there is no carrier
     * to follow (UNVERIFIED on hardware).
     */
    memset(txLc, 0x00, sizeof(txLc));
    buildBsDwnAct(cfg.ownId, txLc);
    activeTiming = true;
    port->setAccess(cfg.polite ? R21_POLITE : R21_IMPOLITE);
    port->startTx(true);
    keyRf();
    requestAxis(nowMs);
}

void CallController::abortTx(uint32_t nowMs)
{
    pttLatched = true;
    unkeyRf();
    goRx(nowMs, RX_SEARCH);
}

/*
 * RF keying, reported to the owner through Report::rfKey: the RF stage is
 * keyed as soon as the modem is armed for transmission (register 0x40 with
 * TxEn), that is before the first header slot is programmed, and released
 * only once the last burst has gone out. That burst occupies the slot after
 * the timeslot interrupt it was programmed at, so the release happens at
 * the following timeslot interrupt, at the chip's TX-end report (0x82 bit5
 * with sub-status 0x86 bit7, R86_TX_COMPLETE) if it comes first, or on the
 * RF_UNKEY_MS fallback when neither arrives (manual §5.4.5 step 12: "after
 * the next or a few 30 ms of the end of the speech frame is sent, configure
 * reg0x40 to 0x03";
 * UNVERIFIED on hardware whether one slot of margin is enough for the PA
 * to see the whole terminator burst). An aborted transmission (RF stage
 * not keying, wake-up failed, PTT released before anything went out,
 * watchdog) releases it at once.
 */
void CallController::keyRf()
{
    rep.rfKey = true;
    rfUnkeyPending = false;
}

void CallController::unkeyRf()
{
    rep.rfKey = false;
    rfUnkeyPending = false;
}

void CallController::unkeyAfterBurst(uint32_t nowMs)
{
    if (!rep.rfKey)
        return;

    rfUnkeyPending = true;
    rfUnkeyMs = nowMs + RF_UNKEY_MS;
}

void CallController::runRfTimer(uint32_t nowMs)
{
    if (rfUnkeyPending && (elapsed(nowMs, rfUnkeyMs) < 0x80000000u))
        unkeyRf();
}

void CallController::programHeader()
{
    port->writeTxLc(txLc);
    port->setTxFrameType(R50_VOICE_LC_HEADER);
    port->setNextSlot(R41_TX);
    headers++;
}

void CallController::programVoice()
{
    bool filled = false;
    if (sink != nullptr)
        filled = sink->txVoiceFrameNeeded(voiceBuf);
    if (!filled)
        memcpy(voiceBuf, AMBE_SILENCE, VOICE_PAYLOAD_BYTES);

    /* Payload first, then the slot programming (OpenGD77 order) */
    port->writeVoice(voiceBuf);
    port->setTxFrameType(r50Voice(seq, cfg.lcssMode));
    port->setNextSlot(R41_TX);
    seq = (seq + 1) % SUPERFRAME_BURST;
    if ((seq == 0) && (superframes < UINT8_MAX))
        superframes++;
}

void CallController::programTerminator()
{
    /* Terminator with LC: the LC is still in the TX RAM */
    memcpy(voiceBuf, AMBE_SILENCE, VOICE_PAYLOAD_BYTES);
    port->writeVoice(voiceBuf);
    port->setTxFrameType(R50_TERMINATOR);
    port->setNextSlot(R41_TX);
}

void CallController::txSlot(uint32_t nowMs, bool ownSlot)
{
    switch (st) {
        case TX_ARM:
            if (!armed) {
                if (carrierPresent(nowMs) && (slotLock == 2)) {
                    armTx(nowMs, false);
                } else if (elapsed(nowMs, armStartMs) >= cfg.syncWuMs) {
                    /* Carrier seen but never locked: wake the repeater */
                    enterWakeup(nowMs);
                    if (st == TX_WAKEUP)
                        txSlot(nowMs, ownSlot);
                    else
                        port->setNextSlot(rxSlot());
                    return;
                } else {
                    port->setNextSlot(rxSlot());
                    return;
                }
            }

            if (ownSlot) {
                txStartMs = nowMs;
                programHeader();
                st = TX_HEADER;
            } else {
                port->setNextSlot(R41_IDLE);
            }
            break;

        case TX_WAKEUP:
            switch (wakeupPhase) {
                case WU_SEND:
                    if (ownSlot) {
                        port->writeTxLc(txLc);
                        port->setTxFrameType(R50_CSBK);
                        port->setNextSlot(R41_TX);
                        wakeupPhase = WU_SENT;
                    } else {
                        port->setNextSlot(R41_IDLE);
                    }
                    break;

                case WU_SENT:
                    /* CSBK going out now: back to RX, wait for the BS */
                    port->setNextSlot(R41_IDLE);
                    goRx(nowMs, TX_WAKEUP);
                    unkeyAfterBurst(nowMs);
                    wakeupSentMs = nowMs;
                    wakeupPhase = WU_WAIT;
                    break;

                case WU_WAIT:
                default:
                    /* T_SyncWu itself is kept by runTimers() */
                    if (carrierPresent(nowMs) && (slotLock == 2)) {
                        st = TX_ARM;
                        armTx(nowMs, false);
                        txSlot(nowMs, ownSlot);
                    } else {
                        port->setNextSlot(rxSlot());
                    }
                    break;
            }
            break;

        case TX_HEADER:
            if (!ownSlot) {
                port->setNextSlot(R41_IDLE);
                break;
            }
            programHeader();
            if (headers >= HEADER_REPEAT) {
                st = TX_VOICE;
                seq = 0;
                superframes = 0;
            }
            break;

        case TX_VOICE:
            if (!ownSlot) {
                port->setNextSlot(R41_IDLE);
                break;
            }

            if ((cfg.txTimeoutMs != 0)
                && (elapsed(nowMs, txStartMs) >= cfg.txTimeoutMs))
                txTimedOut = true;

            /*
             * Release or timeout: the transmission ends with a complete
             * voice superframe followed by the terminator, TS 102 361-2
             * §5.2.1.1 and §5.2.2.1 ("EOT shall be accomplished by
             * transmitting the entire last voice superframe (through voice
             * burst F), and then sending the ... Terminator with LC"), the
             * header being followed by voice superframes (TS 102 361-1
             * §5.1.2.2). A PTT released during the headers therefore sends
             * one superframe of silence before the terminator.
             */
            if ((!ptt || txTimedOut) && (seq == 0) && (superframes > 0)) {
                programTerminator();
                st = TX_TERM;
                if (txTimedOut)
                    pttLatched = true;
            } else {
                programVoice();
            }
            break;

        case TX_TERM:
            /*
             * Terminator going out now: idle the next slot, back to RX.
             * The RF stays keyed until the burst has gone out.
             */
            port->setNextSlot(R41_IDLE);
            goRx(nowMs, RX_SEARCH);
            unkeyAfterBurst(nowMs);
            hangStartMs = nowMs;
            rep.hangActive = (cfg.hangTimeMs != 0);
            rep.lcOk = false;
            break;

        default:
            break;
    }
}

/*
 * Inputs
 */

void CallController::setPtt(bool p, uint32_t nowMs)
{
    if (p == ptt)
        return;

    ptt = p;

    if (!p) {
        /* Nothing on the air yet: just go back to RX */
        if ((st == TX_ARM) || (st == TX_WAKEUP))
            abortTx(nowMs);
        pttLatched = false;
        publish();
        return;
    }

    if (pttLatched || (st == OFF) || isTx() || (port == nullptr))
        return;

    clearMarkers();

    if ((cfg.ownId & ADDRESS_MASK) == 0) {
        rep.noId = true;
        pttLatched = true;
        publish();
        return;
    }

    enterTxArm(nowMs);
    publish();
}

void CallController::onTimeslot(const struct dmrbbSnapshot &s, uint32_t nowMs)
{
    if ((st == OFF) || (port == nullptr))
        return;

    lastTsMs = nowMs;
    rep.needsReconfigure = false;
    bool wasSearching = (st == RX_SEARCH);

    /* A last burst programmed at the previous interrupt has gone out */
    if (rfUnkeyPending)
        unkeyRf();

    /*
     * Interrupts merged into this event: the timecode toggles once per
     * missed slot before the regular tracking, so that the parity of an
     * ongoing call or transmission survives a late wake-up of the thread.
     * UNVERIFIED on hardware: register 0x42[7:5] reports whether the slot
     * that just started is the working one (manual §5.4.4, "001" working
     * slot with the transceiver closed, "101" sending, "011" receiving,
     * "xx0" non-working); once its meaning is confirmed on the radio it can
     * cross-check the own-slot parity during a transmission.
     */
    uint32_t missed = missedSlots(s);
    if (tcValid && (missed & 1))
        timecode ^= 1;
    lastTsCount = s.tsCount;
    lastTsTick = s.tsTick;

    trackTimecode((s.r52 & R52_TC) ? 1 : 0);
    runTimers(nowMs);

    uint8_t next = timecode ^ 1;
    bool ownNext = (next == (uint8_t)((cfg.timeslot - 1) & 1));

    switch (st) {
        case RX_SEARCH:
            /*
             * A search started in this very pass, by the voice timeout of
             * a direct mode call, has not seen a slot of its own yet
             */
            if (!wasSearching)
                break;

            st = rep.hangActive ? RX_HANG : RX_IDLE;
            port->setNextSlot(rxSlot());
            break;

        case RX_IDLE:
        case RX_HANG:
            /*
             * Once the chip has a time axis it keeps counting 30 ms slots
             * on it when the signal is gone and follows a new sync only
             * within 1.25 ms of it (manual §5.4.4): an axis that nothing
             * confirms for more than IDLE_RESEARCH_SLOTS slots, whether it
             * came from a call that ended or from noise, is dropped and
             * the search started again (OpenGD77 and OpenMDUV380 do so
             * after 20 timeslot interrupts in idle). Merged interrupts
             * count for the slots they stand for.
             */
            if (missed > IDLE_RESEARCH_SLOTS)
                missed = IDLE_RESEARCH_SLOTS;

            if ((missed + idleSlots) >= IDLE_RESEARCH_SLOTS) {
                goRx(nowMs, RX_SEARCH);
            } else {
                idleSlots = (uint8_t)(idleSlots + missed + 1);
                port->setNextSlot(rxSlot());
            }
            break;

        case RX_CALL:
            /*
             * Direct mode: open only the call's slot, the other one may
             * decode noise as valid data (OpenGD77 observation), and only
             * while bursts keep coming: after two missed bursts both slots
             * are opened again, so that a wrong parity cannot lock the
             * call out. Repeater mode: receive both, the timeslot is
             * filtered in software.
             */
            if (callDmo && (next != callTimecode)
                && (elapsed(nowMs, lastVoiceMs) < DMO_GATE_HOLD_MS))
                port->setNextSlot(R41_IDLE);
            else
                port->setNextSlot(rxSlot());
            break;

        default:
            txSlot(nowMs, ownNext);
            break;
    }

    publish();
}

void CallController::onSysEvent(const struct dmrbbSnapshot &s, uint32_t nowMs)
{
    if ((st == OFF) || (port == nullptr))
        return;

    runTimers(nowMs);

    uint8_t irq = s.irq82;

    /*
     * The sync type is taken only from a burst the chip vouches for: a
     * data burst with a good CRC, a burst with the voice sync or a late
     * entry. Both slots are open while idle and the empty one may report
     * noise as a frame.
     */
    uint8_t cls = s.r51 & R51_SYNC_CLASS_MASK;
    bool lateEntry = ((irq & R82_LATE_ENTRY) != 0);
    bool ctrlFrame = ((irq & R82_CTRL_FRAME) != 0);
    bool goodData = (cls == R51_CLASS_DATA) && ((s.r51 & R51_CRC_BAD) == 0);

    if (lateEntry || (ctrlFrame && (goodData || (cls == R51_CLASS_VOICE)))) {
        lastSync = s.r5f & R5F_SYNC_MASK;
        if (lastSync == R5F_SYNC_BS)
            lastBsSyncMs = nowMs;
    }

    /*
     * The colour code travels in the slot type of a data burst and in the
     * EMB of voice bursts B to F (TS 102 361-1 §9.1.2, §9.1.3): burst A
     * carries the sync pattern in its place and tells nothing about it.
     */
    if (lateEntry || (ctrlFrame && goodData))
        rep.rxColorCodeSeen = s.r52 >> 4;

    if (syncConfirmed(s))
        idleSlots = 0;

    /*
     * "This transmission request is rejected because the channel is busy"
     * (0x82 bit7, manual §5.1.1), the chip's verdict under the polite
     * access policy. The transmission goes on: OpenGD77 and OpenMDUV380
     * arm the chip with the same policy word, do nothing on this flag and
     * keep programming their slots. The flag is counted and shown as the
     * busy marker. UNVERIFIED on hardware: whether the chip raises it at
     * all, and whether it keeps the bursts off the air when it does.
     */
    if ((irq & R82_TX_REJECTED) && isTx()) {
        rep.busy = true;
        if (rep.txRejected < UINT8_MAX)
            rep.txRejected++;
    }

    /*
     * The chip reports the end of the transmission: release the RF. Only
     * the "service transmission completely terminated" sub-status counts
     * (0x86 bit7): bit5 of 0x82 is shared with the per-slot Layer 2
     * deadline interrupt (0x86 bit4, Rdy_lst_inter) and the data events,
     * which come while the last burst is still on the air. Those are left
     * to the next timeslot interrupt or the RF_UNKEY_MS fallback.
     */
    if ((irq & R82_TX_END) && ((s.sub86 & R86_TX_COMPLETE) != 0)
        && rfUnkeyPending)
        unkeyRf();

    if (isTx()) {
        publish();
        return;
    }

    if (irq & R82_CTRL_FRAME)
        processControlFrame(s, nowMs);

    /*
     * Late entry: LC recovered from the embedded signalling. The chip gives
     * this interrupt no status register of its own; the check flag of 0x51
     * is required clear as for every other use of the LC RAM (OpenGD77
     * practice). UNVERIFIED on hardware: if the trace shows 0x51 bit 2 set
     * at a late entry with a good LC, drop this test.
     */
    if ((irq & R82_LATE_ENTRY) && (st != RX_CALL)
        && ((s.r51 & R51_CRC_BAD) == 0))
        tryOpenCall(s, nowMs);

    /*
     * The "abnormal voice exit" interrupt (0x82 bit1, cause in 0x98) does
     * not end a call: it is the chip's own voice sync timer, one per slot
     * (manual chapter 8, registers 0x0D and 0x98), and it fires while a
     * call goes on: OpenGD77 reads 0x98 and does nothing, OpenMDUV380
     * mutes one frame when the cause names the call's slot. A call ends
     * on its terminator, on another data burst in its slot or after
     * VOICE_TIMEOUT_MS without voice. The cause stays in the snapshot for
     * the bring-up trace.
     */

    publish();
}

void CallController::onTimeout(uint32_t nowMs)
{
    if ((st == OFF) || (port == nullptr))
        return;

    runTimers(nowMs);

    /*
     * Timeslot watchdog. Once it has a time axis the chip gives a timeslot
     * interrupt every 30 ms, also when the signal is gone (manual §5.4.4),
     * and none before: so a silent channel in RX_SEARCH is left alone, and
     * so is the wait for the repeater's answer to a wake-up, which
     * T_SyncWu, run by runTimers(), bounds. In any other state interrupts
     * missing for TS_WATCHDOG_MS mean the axis is gone: the search is
     * started again (OpenGD77 practice) and the controller waits in
     * RX_SEARCH, a call being received ending with its hang time. A
     * transmission requests its own axis, is watched from that request
     * and ends with the "RF failed" marker. A full reconfiguration is
     * asked for only when the silence is first seen after
     * TS_RECONFIGURE_MS, that is when the thread itself was held up that
     * long.
     */
    if ((st == RX_SEARCH) || ((st == TX_WAKEUP) && (wakeupPhase == WU_WAIT))) {
        publish();
        return;
    }

    uint32_t silent = elapsed(nowMs, lastTsMs);

    if (silent >= TS_WATCHDOG_MS) {
        rep.needsReconfigure = (silent >= TS_RECONFIGURE_MS);

        if (isTx()) {
            rep.rfFailed = true;
            pttLatched = true;
            unkeyRf();
            goRx(nowMs, RX_SEARCH);
        } else if (st == RX_CALL) {
            enterHang(nowMs, true);
        } else {
            goRx(nowMs, RX_SEARCH);
        }
    }

    publish();
}
