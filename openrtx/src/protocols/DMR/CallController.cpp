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
    wdRestarted = false;
    skipLc = false;
    callSlot = 0;
    callTimecode = 0;
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
    cfg = c;
    if (cfg.timeslot < 1)
        cfg.timeslot = 1;
    if (cfg.timeslot > 2)
        cfg.timeslot = 2;
    cfg.colorCode &= 0x0F;
}

void CallController::enable(uint32_t nowMs)
{
    if (port == nullptr)
        return;

    /* The markers outlive a restart: the owner clears them */
    bool noId = rep.noId;
    bool busy = rep.busy;
    bool wakeupFailed = rep.wakeupFailed;

    memset(&rep, 0x00, sizeof(rep));
    rep.noId = noId;
    rep.busy = busy;
    rep.wakeupFailed = wakeupFailed;

    ptt = false;
    pttLatched = false;
    lastBsSyncMs = nowMs - CARRIER_TIMEOUT_MS;
    lastSync = R5F_SYNC_MS;
    wdRestarted = false;
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
    switch (st) {
        case RX_CALL:
            /* No voice for two superframes: the call is gone */
            if (elapsed(nowMs, lastVoiceMs) > VOICE_TIMEOUT_MS)
                enterHang(nowMs);
            break;

        case RX_HANG:
            if (elapsed(nowMs, hangStartMs) >= cfg.hangTimeMs) {
                st = RX_IDLE;
                rep.hangActive = false;
            }
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
        case RX_HANG:
            rep.callState = DMR_CALL_RX_HANG;
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
            rep.callState = DMR_CALL_IDLE;
            break;
    }
}

/*
 * Reception
 */

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
    port->startRx();
    resetTiming();
    requestAxis(nowMs);

    /*
     * UNVERIFIED on hardware: the RX RAM may still hold the LC of the
     * previous call on the first system interrupt after a mode switch
     * (OpenGD77 observation), so the first LC read is discarded.
     */
    skipLc = true;
    st = next;
}

void CallController::enterHang(uint32_t nowMs)
{
    st = RX_HANG;
    hangStartMs = nowMs;
    rep.hangActive = true;

    if (cfg.hangTimeMs == 0)
        runTimers(nowMs);
}

bool CallController::tryOpenCall(const struct dmrbbSnapshot &s, uint32_t nowMs)
{
    if (skipLc) {
        skipLc = false;
        return false;
    }

    uint8_t cc = s.r52 >> 4;
    uint8_t slot = slotOf(s);

    if (!colorCodeAccepted(cc) || !slotAccepted(slot))
        return false;

    /* PF or R set, reserved FID, unknown FLCO: rejected by unpack() */
    FullLC lc;
    if (!lc.fromChipLc(s.lc))
        return false;

    if (!lc.matches(cfg.ownId, cfg.dstId, cfg.callType, cfg.monitor))
        return false;

    bool bs = ((s.r5f & R5F_SYNC_MASK) == R5F_SYNC_BS);

    st = RX_CALL;
    callSlot = slot;
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

    return true;
}

void CallController::processControlFrame(const struct dmrbbSnapshot &s,
                                         uint32_t nowMs)
{
    if (s.r51 & R51_CRC_BAD)
        return;

    uint8_t cls = s.r51 & R51_SYNC_CLASS_MASK;

    if (cls == R51_CLASS_VOICE) {
        /* Voice burst A..F, sequence 1..6 in 0x51[7:4] */
        if ((st == RX_CALL) && burstIsCall(s)) {
            lastVoiceMs = nowMs;
            if (s.voiceValid && !rep.lcManufacturer && (sink != nullptr))
                sink->rxVoiceFrame(s.voice);
        }
        return;
    }

    if (cls == R51_CLASS_RC)
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
                    enterHang(nowMs);
            } else {
                tryOpenCall(s, nowMs);
            }
            break;

        case DT_TERMINATOR_LC:
            if (!onCall)
                break;
            if (st == RX_CALL)
                enterHang(nowMs);
            else if ((st == RX_HANG) && terminatorOfCall(s))
                hangStartMs = nowMs; /* hang time terminator: keep shown */
            break;

        default:
            /* Any other data burst on the call's slot ends the call */
            if ((st == RX_CALL) && onCall)
                enterHang(nowMs);
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
    buildTxLc();

    /* Active timing: the modem provides a fresh axis from now on */
    if (active) {
        requestAxis(nowMs);
        wdRestarted = false;
    }
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
        if (st == TX_WAKEUP && wakeupPhase == WU_WAIT) {
            /* Already receiving: idle if slots ticked, else searching */
            st = (slotLock != 0) ? RX_IDLE : RX_SEARCH;
        } else {
            goRx(nowMs, RX_IDLE);
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
    requestAxis(nowMs);
    wdRestarted = false;
}

void CallController::abortTx(uint32_t nowMs, bool busy)
{
    rep.busy = busy;
    pttLatched = true;
    goRx(nowMs, RX_IDLE);
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
    port->setTxFrameType(r50Voice(seq));
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
                        port->setNextSlot(R41_RX);
                    return;
                } else {
                    port->setNextSlot(R41_RX);
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
                        port->setNextSlot(R41_RX);
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
            /* Terminator going out now: idle the next slot, back to RX */
            port->setNextSlot(R41_IDLE);
            goRx(nowMs, RX_HANG);
            hangStartMs = nowMs;
            rep.hangActive = true;
            rep.lcOk = false;
            if (cfg.hangTimeMs == 0)
                runTimers(nowMs);
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
        pttLatched = false;
        /* Nothing on the air yet: just go back to RX */
        if ((st == TX_ARM) || (st == TX_WAKEUP))
            abortTx(nowMs, false);
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
    wdRestarted = false;
    rep.needsReconfigure = false;

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
    if (tcValid && (missedSlots(s) & 1))
        timecode ^= 1;
    lastTsCount = s.tsCount;
    lastTsTick = s.tsTick;

    trackTimecode((s.r52 & R52_TC) ? 1 : 0);
    runTimers(nowMs);

    uint8_t next = timecode ^ 1;
    bool ownNext = (next == (uint8_t)((cfg.timeslot - 1) & 1));

    switch (st) {
        case RX_SEARCH:
            st = RX_IDLE;
            port->setNextSlot(R41_RX);
            break;

        case RX_IDLE:
        case RX_HANG:
            port->setNextSlot(R41_RX);
            break;

        case RX_CALL:
            /*
             * Direct mode: open only the call's slot, the other one may
             * decode noise as valid data (OpenGD77 observation). Repeater
             * mode: receive both, the timeslot is filtered in software.
             */
            if (callDmo && (next != callTimecode))
                port->setNextSlot(R41_IDLE);
            else
                port->setNextSlot(R41_RX);
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

    if (irq & (R82_CTRL_FRAME | R82_LATE_ENTRY)) {
        lastSync = s.r5f & R5F_SYNC_MASK;
        if (lastSync == R5F_SYNC_BS)
            lastBsSyncMs = nowMs;
        rep.rxColorCodeSeen = s.r52 >> 4;
    }

    if ((irq & R82_TX_REJECTED) && isTx())
        abortTx(nowMs, true);

    if (isTx()) {
        publish();
        return;
    }

    if (irq & R82_CTRL_FRAME)
        processControlFrame(s, nowMs);

    /* Late entry: LC recovered from the embedded signalling */
    if ((irq & R82_LATE_ENTRY) && (st != RX_CALL))
        tryOpenCall(s, nowMs);

    if ((irq & R82_ABNORMAL_EXIT) && (st == RX_CALL))
        enterHang(nowMs);

    publish();
}

void CallController::onTimeout(uint32_t nowMs)
{
    if ((st == OFF) || (port == nullptr))
        return;

    runTimers(nowMs);

    /*
     * Timeslot watchdog: the interrupts stop when the chip loses its time
     * axis. Re-run the RX sequence after TS_WATCHDOG_MS (OpenGD77 practice),
     * ask for a full reconfiguration after TS_RECONFIGURE_MS. The chip
     * provides the interrupts only once a time axis exists (manual §5.4.4),
     * so while searching for a signal there is nothing to watch: a silent
     * channel in RX_SEARCH is left alone. A transmission requests its own
     * axis and is watched from that request, except while waiting for the
     * repeater's answer to a wake-up: the channel is silent by definition
     * and T_SyncWu, run by runTimers(), bounds the wait.
     */
    if ((st == RX_SEARCH) || ((st == TX_WAKEUP) && (wakeupPhase == WU_WAIT))) {
        publish();
        return;
    }

    uint32_t silent = elapsed(nowMs, lastTsMs);

    if ((silent >= TS_RECONFIGURE_MS) && !rep.needsReconfigure) {
        rep.needsReconfigure = true;
        if (isTx())
            pttLatched = true;
        goRx(nowMs, RX_SEARCH);
    } else if ((silent >= TS_WATCHDOG_MS) && !wdRestarted) {
        wdRestarted = true;
        if (isTx())
            pttLatched = true;
        goRx(nowMs, isTx() ? RX_SEARCH : st);
    }

    publish();
}
