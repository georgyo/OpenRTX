/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <vector>

#include "protocols/DMR/CallController.hpp"
#include "protocols/DMR/Constants.hpp"
#include "protocols/DMR/LinkControl.hpp"
#include "protocols/DMR/ModemPort.hpp"

extern "C" {
#include "rtx/rtx.h"
}

using namespace DMR;

/*
 * Drives the DMR call controller with synthetic timeslot and system events
 * and records every register write on a mock modem port. Nothing here talks
 * to a chip: the register values are the named constants of ModemPort.hpp.
 */

enum WriteKind : uint8_t { W_R41, W_R50, W_R40, W_R21, W_LC, W_VOICE };

struct Write {
    WriteKind kind;
    uint8_t value;
    uint8_t data[VOICE_PAYLOAD_BYTES];
};

class MockPort : public ModemPort
{
public:
    std::vector<Write> writes;

    void setNextSlot(uint8_t r41) override
    {
        push(W_R41, r41);
    }

    void setTxFrameType(uint8_t r50) override
    {
        push(W_R50, r50);
    }

    void setMode(uint8_t r40) override
    {
        push(W_R40, r40);
    }

    void setAccess(uint8_t r21) override
    {
        push(W_R21, r21);
    }

    void writeTxLc(const uint8_t lc[CHIP_LC_BYTES]) override
    {
        Write w;
        w.kind = W_LC;
        w.value = 0;
        memset(w.data, 0x00, sizeof(w.data));
        memcpy(w.data, lc, CHIP_LC_BYTES);
        writes.push_back(w);
    }

    void writeVoice(const uint8_t ambe[VOICE_PAYLOAD_BYTES]) override
    {
        Write w;
        w.kind = W_VOICE;
        w.value = 0;
        memcpy(w.data, ambe, VOICE_PAYLOAD_BYTES);
        writes.push_back(w);
    }

    void clear()
    {
        writes.clear();
    }

    size_t count(WriteKind kind) const
    {
        size_t n = 0;
        for (const Write &w : writes)
            if (w.kind == kind)
                n++;
        return n;
    }

    size_t count(WriteKind kind, uint8_t value) const
    {
        size_t n = 0;
        for (const Write &w : writes)
            if ((w.kind == kind) && (w.value == value))
                n++;
        return n;
    }

    const Write *last(WriteKind kind) const
    {
        for (size_t i = writes.size(); i > 0; i--)
            if (writes[i - 1].kind == kind)
                return &writes[i - 1];
        return nullptr;
    }

    /* Values of one register in the order they were written */
    std::vector<uint8_t> values(WriteKind kind) const
    {
        std::vector<uint8_t> v;
        for (const Write &w : writes)
            if (w.kind == kind)
                v.push_back(w.value);
        return v;
    }

private:
    void push(WriteKind kind, uint8_t value)
    {
        Write w;
        w.kind = kind;
        w.value = value;
        memset(w.data, 0x00, sizeof(w.data));
        writes.push_back(w);
    }
};

class MockSink : public VoiceSink
{
public:
    size_t rxFrames = 0;
    uint8_t lastRx[VOICE_PAYLOAD_BYTES] = { 0 };
    size_t txRequests = 0;
    bool provide = false;
    uint8_t fill = 0x5A;

    void rxVoiceFrame(const uint8_t ambe[VOICE_PAYLOAD_BYTES]) override
    {
        rxFrames++;
        memcpy(lastRx, ambe, VOICE_PAYLOAD_BYTES);
    }

    bool txVoiceFrameNeeded(uint8_t ambe[VOICE_PAYLOAD_BYTES]) override
    {
        txRequests++;
        if (!provide)
            return false;
        memset(ambe, fill, VOICE_PAYLOAD_BYTES);
        return true;
    }
};

/*
 * Test bench: controller + mock port + a clock advancing 30 ms per slot.
 */
class Bench
{
public:
    MockPort port;
    MockSink sink;
    CallController ctrl;
    CallController::Config cfg;
    uint32_t now = 1000;
    uint8_t timecode = 0; /* timecode of the slot that starts at the TS */

    Bench()
    {
        cfg = ctrl.config();
        cfg.ownId = 2345678;
        cfg.dstId = 31665;
        cfg.callType = GROUP;
        cfg.colorCode = 1;
        cfg.timeslot = 2;
        cfg.monitor = 0;
        cfg.polite = true;
        cfg.repeater = false;
        cfg.hangTimeMs = 3000;
        cfg.wakeupAttempts = 2;
        cfg.syncWuMs = 360;
        cfg.txTimeoutMs = 0;
        ctrl.setPort(&port);
        ctrl.setVoiceSink(&sink);
    }

    void start()
    {
        ctrl.configure(cfg);
        ctrl.enable(now);
    }

    static dmrbbSnapshot blank()
    {
        dmrbbSnapshot s;
        memset(&s, 0x00, sizeof(s));
        return s;
    }

    /* One timeslot interrupt: the chip reports the CACH TC of the slot */
    void ts(bool agree = true)
    {
        now += BURST_MS;
        dmrbbSnapshot s = blank();
        uint8_t rxTc = agree ? timecode : (timecode ^ 1);
        s.r52 = (uint8_t)((cfg.colorCode << 4) | (rxTc ? R52_TC : 0));
        ctrl.onTimeslot(s, now);
        timecode ^= 1;
    }

    /* N timeslot interrupts */
    void tsN(unsigned n)
    {
        for (unsigned i = 0; i < n; i++)
            ts();
    }

    /*
     * A burst decoded 4 ms after the slot end. 'slot' is the timeslot of
     * the burst for a BS sourced burst (r52 TC) or a DMO burst (TDMA
     * sync); 0 with sync MS.
     */
    dmrbbSnapshot burst(uint8_t sync, uint8_t slot, uint8_t cc)
    {
        dmrbbSnapshot s = blank();
        s.irq82 = R82_CTRL_FRAME;
        s.r5f = sync;
        s.r52 = (uint8_t)(cc << 4);
        if ((sync == R5F_SYNC_BS) && (slot == 2))
            s.r52 |= R52_TC;
        return s;
    }

    /* The system interrupt of the slot that ended at the last TS */
    void sys(dmrbbSnapshot &s)
    {
        now += 4;
        ctrl.onSysEvent(s, now);
    }

    void header(uint8_t sync, uint8_t slot, uint8_t cc, const FullLC &lc,
                bool crcBad = false)
    {
        dmrbbSnapshot s = burst(sync, slot, cc);
        s.r51 = (uint8_t)((DT_VOICE_LC_HEADER << 4) | R51_CLASS_HEADER);
        if (crcBad)
            s.r51 |= R51_CRC_BAD;
        lc.toChipLc(s.lc);
        sys(s);
    }

    void headerRaw(uint8_t sync, uint8_t slot, uint8_t cc,
                   const uint8_t raw[FULL_LC_BYTES])
    {
        dmrbbSnapshot s = burst(sync, slot, cc);
        s.r51 = (uint8_t)((DT_VOICE_LC_HEADER << 4) | R51_CLASS_HEADER);
        memcpy(s.lc, raw, FULL_LC_BYTES);
        sys(s);
    }

    void lateEntry(uint8_t sync, uint8_t slot, uint8_t cc, const FullLC &lc)
    {
        dmrbbSnapshot s = burst(sync, slot, cc);
        s.irq82 = R82_LATE_ENTRY;
        lc.toChipLc(s.lc);
        sys(s);
    }

    void voice(uint8_t sync, uint8_t slot, uint8_t cc, uint8_t seq,
               uint8_t fill = 0xA5)
    {
        dmrbbSnapshot s = burst(sync, slot, cc);
        s.r51 = (uint8_t)((seq << 4) | R51_CLASS_VOICE);
        s.voiceValid = true;
        memset(s.voice, fill, VOICE_PAYLOAD_BYTES);
        sys(s);
    }

    void terminator(uint8_t sync, uint8_t slot, uint8_t cc)
    {
        dmrbbSnapshot s = burst(sync, slot, cc);
        s.r51 = (uint8_t)((DT_TERMINATOR_LC << 4) | R51_CLASS_HEADER);
        sys(s);
    }

    void dataBurst(uint8_t sync, uint8_t slot, uint8_t cc, uint8_t type)
    {
        dmrbbSnapshot s = burst(sync, slot, cc);
        s.r51 = (uint8_t)((type << 4) | R51_CLASS_DATA);
        sys(s);
    }

    void flags(uint8_t irq)
    {
        dmrbbSnapshot s = blank();
        s.irq82 = irq;
        sys(s);
    }

    void timeout(uint32_t ms)
    {
        now += ms;
        ctrl.onTimeout(now);
    }

    /* Reach RX_IDLE with a locked timecode: the LC skip is consumed too */
    void settle()
    {
        start();
        tsN(6);
        FullLC stale;
        stale.clear();
        stale.src = 1;
        stale.dst = cfg.dstId;
        header(R5F_SYNC_MS, 0, cfg.colorCode, stale);
        port.clear();
        sink.rxFrames = 0;
    }

    /* A whole voice superframe of the call, on the given slot */
    void superframe(uint8_t sync, uint8_t slot, uint8_t cc)
    {
        for (uint8_t seq = 1; seq <= 6; seq++) {
            ts();
            ts();
            voice(sync, slot, cc, seq);
        }
    }

    FullLC groupCall(uint32_t src, uint32_t dst) const
    {
        FullLC lc;
        lc.clear();
        lc.flco = FLCO_GRP_V_CH_USR;
        lc.src = src;
        lc.dst = dst;
        return lc;
    }

    FullLC privateCall(uint32_t src, uint32_t dst) const
    {
        FullLC lc = groupCall(src, dst);
        lc.flco = FLCO_UU_V_CH_USR;
        return lc;
    }

    /* Bring a repeater carrier up: BS sync bursts until locked */
    void repeaterCarrier()
    {
        for (unsigned i = 0; i < 8; i++) {
            ts();
            dataBurst(R5F_SYNC_BS, 1, cfg.colorCode, DT_IDLE);
        }
    }
};

TEST_CASE("DMR call controller size and defaults", "[dmr][callctrl]")
{
    WARN("sizeof(DMR::CallController) = "
         << sizeof(CallController) << " bytes, Report "
         << sizeof(CallController::Report) << ", Config "
         << sizeof(CallController::Config));
    REQUIRE(sizeof(CallController) <= 256);

    CallController c;
    REQUIRE(c.state() == CallController::OFF);
    REQUIRE(c.config().hangTimeMs == T_CALLHT_MS);
    REQUIRE(c.config().wakeupAttempts == N_WAKEUP);
    REQUIRE(c.config().syncWuMs == T_SYNCWU_MS);
    REQUIRE(c.config().txTimeoutMs == T_TO_S * 1000);
    REQUIRE(c.config().polite == true);
    REQUIRE(c.report().callState == DMR_CALL_IDLE);
}

TEST_CASE("DMR register constants", "[dmr][callctrl]")
{
    REQUIRE(R41_IDLE == 0x00);
    REQUIRE(R41_RX == 0x50);
    REQUIRE(R41_TX == 0x80);
    REQUIRE(R41_RESYNC == 0x20);
    REQUIRE(R50_VOICE_LC_HEADER == 0x10);
    REQUIRE(R50_TERMINATOR == 0x20);
    REQUIRE(R50_CSBK == 0x30);
    REQUIRE(R40_RX == 0xC3);
    REQUIRE(R40_TX_DMO == 0xE3);
    REQUIRE(R40_TX_RMO == 0xC3);
    REQUIRE(R40_IDLE == 0x03);

    /* Voice A..F: 0x08 | seq << 4, LCSS left at 00 */
    const uint8_t expected[6] = { 0x08, 0x18, 0x28, 0x38, 0x48, 0x58 };
    for (uint8_t i = 0; i < 6; i++)
        REQUIRE(r50Voice(i) == expected[i]);
}

TEST_CASE("DMR enable starts reception, disable idles", "[dmr][callctrl]")
{
    Bench b;
    b.start();

    REQUIRE(b.ctrl.state() == CallController::RX_SEARCH);
    REQUIRE(b.port.values(W_R40) == std::vector<uint8_t>{ R40_RX });
    REQUIRE(b.port.values(W_R41) == std::vector<uint8_t>{ R41_RESYNC, R41_RX });
    REQUIRE(b.ctrl.report().slotLock == 0);

    b.ts();
    REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
    REQUIRE(b.ctrl.report().slotLock == 1);
    REQUIRE(b.port.last(W_R41)->value == R41_RX);

    b.port.clear();
    b.ctrl.disable();
    REQUIRE(b.ctrl.state() == CallController::OFF);
    REQUIRE(b.port.values(W_R40) == std::vector<uint8_t>{ R40_IDLE });
    REQUIRE(b.ctrl.report().callState == DMR_CALL_IDLE);

    /* Events in OFF do nothing */
    b.port.clear();
    b.ts();
    b.timeout(3000);
    REQUIRE(b.port.writes.empty());
    REQUIRE(b.ctrl.state() == CallController::OFF);
}

TEST_CASE("DMR events without a port are ignored", "[dmr][callctrl]")
{
    CallController c;
    dmrbbSnapshot s;
    memset(&s, 0x00, sizeof(s));
    c.enable(0);
    c.onTimeslot(s, 30);
    c.onSysEvent(s, 34);
    c.onTimeout(100);
    c.setPtt(true, 100);
    REQUIRE(c.state() == CallController::OFF);
}

TEST_CASE("DMR timecode lock after 5 agreeing slots, drop after 4",
          "[dmr][callctrl]")
{
    Bench b;
    b.start();

    /* First TS initialises, then 4 more agreeing ones lock */
    b.tsN(4);
    REQUIRE(b.ctrl.report().slotLock == 1);
    b.ts();
    REQUIRE(b.ctrl.report().slotLock == 2);

    /* Three disagreeing slots keep the lock, the fourth drops it */
    b.ts(false);
    b.ts(false);
    b.ts(false);
    REQUIRE(b.ctrl.report().slotLock == 2);
    b.ts(false);
    REQUIRE(b.ctrl.report().slotLock == 1);

    /* After the re-alignment the local timecode follows the chip again */
    b.timecode ^= 1;
    b.tsN(5);
    REQUIRE(b.ctrl.report().slotLock == 2);
}

TEST_CASE("DMR RX group call: header, voice, terminator, hang",
          "[dmr][callctrl]")
{
    Bench b;
    b.settle();
    REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
    REQUIRE(b.ctrl.report().slotLock == 2);

    FullLC lc = b.groupCall(1234567, 31665);
    b.ts();
    b.header(R5F_SYNC_BS, 2, 1, lc);

    const CallController::Report &r = b.ctrl.report();
    REQUIRE(b.ctrl.state() == CallController::RX_CALL);
    REQUIRE(r.callState == DMR_CALL_RX);
    REQUIRE(r.lcOk == true);
    REQUIRE(r.lcManufacturer == false);
    REQUIRE(r.rxSrc == 1234567);
    REQUIRE(r.rxDst == 31665);
    REQUIRE(r.rxFlco == FLCO_GRP_V_CH_USR);
    REQUIRE(r.rxColorCodeSeen == 1);
    REQUIRE(r.rxTimeslot == 2);
    REQUIRE(r.rxSyncType == 1);
    REQUIRE(r.hangActive == false);

    /* Two superframes of voice, every burst delivered to the sink */
    b.superframe(R5F_SYNC_BS, 2, 1);
    b.superframe(R5F_SYNC_BS, 2, 1);
    REQUIRE(b.sink.rxFrames == 12);
    REQUIRE(b.sink.lastRx[0] == 0xA5);
    REQUIRE(b.sink.lastRx[26] == 0xA5);
    REQUIRE(b.ctrl.state() == CallController::RX_CALL);

    /* RMO: both slots are received, 0x41 = 0x50 on every TS */
    REQUIRE(b.port.count(W_R41, R41_IDLE) == 0);
    REQUIRE(b.port.count(W_R41, R41_RX) > 20);

    /* Terminator: hang time */
    b.ts();
    b.ts();
    b.terminator(R5F_SYNC_BS, 2, 1);
    REQUIRE(b.ctrl.state() == CallController::RX_HANG);
    REQUIRE(r.callState == DMR_CALL_RX_HANG);
    REQUIRE(r.hangActive == true);
    REQUIRE(r.lcOk == true);
    REQUIRE(r.rxSrc == 1234567);

    /* Repeater hang terminators keep the call shown */
    for (unsigned i = 0; i < 90; i++) {
        b.ts();
        b.ts();
        b.terminator(R5F_SYNC_BS, 2, 1);
    }
    REQUIRE(b.ctrl.state() == CallController::RX_HANG);

    /* T_CallHt after the last one: idle */
    for (unsigned i = 0; i < 99; i++)
        b.ts();
    REQUIRE(b.ctrl.state() == CallController::RX_HANG);
    b.ts();
    b.ts();
    REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
    REQUIRE(r.callState == DMR_CALL_IDLE);
    REQUIRE(r.hangActive == false);

    /* Voice after the hang expiry is not forwarded */
    b.sink.rxFrames = 0;
    b.ts();
    b.voice(R5F_SYNC_BS, 2, 1, 1);
    REQUIRE(b.sink.rxFrames == 0);
}

TEST_CASE("DMR RX call: new header during hang reopens the call",
          "[dmr][callctrl]")
{
    Bench b;
    b.settle();

    FullLC lc = b.groupCall(1234567, 31665);
    b.ts();
    b.header(R5F_SYNC_BS, 2, 1, lc);
    b.ts();
    b.ts();
    b.terminator(R5F_SYNC_BS, 2, 1);
    REQUIRE(b.ctrl.state() == CallController::RX_HANG);

    b.ts();
    b.ts();
    b.header(R5F_SYNC_BS, 2, 1, lc);
    REQUIRE(b.ctrl.state() == CallController::RX_CALL);
    REQUIRE(b.ctrl.report().hangActive == false);
}

TEST_CASE("DMR RX call ends on voice loss, data burst or abnormal exit",
          "[dmr][callctrl]")
{
    Bench b;
    FullLC lc = b.groupCall(1234567, 31665);

    SECTION("no voice for two superframes")
    {
        b.settle();
        b.ts();
        b.header(R5F_SYNC_BS, 2, 1, lc);
        b.superframe(R5F_SYNC_BS, 2, 1);
        REQUIRE(b.ctrl.state() == CallController::RX_CALL);
        /* 720 ms of silence: 24 slots, the 25th trips the timer */
        b.tsN(24);
        REQUIRE(b.ctrl.state() == CallController::RX_CALL);
        b.ts();
        REQUIRE(b.ctrl.state() == CallController::RX_HANG);
    }

    SECTION("data burst on the call's slot")
    {
        b.settle();
        b.ts();
        b.header(R5F_SYNC_BS, 2, 1, lc);
        b.ts();
        b.ts();
        b.dataBurst(R5F_SYNC_BS, 2, 1, DT_CSBK);
        REQUIRE(b.ctrl.state() == CallController::RX_HANG);
    }

    SECTION("data burst on the other slot is ignored")
    {
        b.settle();
        b.ts();
        b.header(R5F_SYNC_BS, 2, 1, lc);
        b.ts();
        b.dataBurst(R5F_SYNC_BS, 1, 1, DT_IDLE);
        REQUIRE(b.ctrl.state() == CallController::RX_CALL);
    }

    SECTION("abnormal exit flag")
    {
        b.settle();
        b.ts();
        b.header(R5F_SYNC_BS, 2, 1, lc);
        b.ts();
        b.flags(R82_ABNORMAL_EXIT);
        REQUIRE(b.ctrl.state() == CallController::RX_HANG);
    }

    SECTION("header of another call on the slot")
    {
        b.settle();
        b.ts();
        b.header(R5F_SYNC_BS, 2, 1, lc);
        b.ts();
        b.ts();
        b.header(R5F_SYNC_BS, 2, 1, b.groupCall(7, 99));
        REQUIRE(b.ctrl.state() == CallController::RX_HANG);
        REQUIRE(b.ctrl.report().rxSrc == 1234567);
    }

    SECTION("bad CRC bursts are ignored")
    {
        b.settle();
        b.ts();
        b.header(R5F_SYNC_BS, 2, 1, lc, true);
        REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
    }
}

TEST_CASE("DMR RX filters: colour code, timeslot, address, monitor",
          "[dmr][callctrl]")
{
    Bench b;
    FullLC tg = b.groupCall(1234567, 31665);
    FullLC otherTg = b.groupCall(1234567, 91);
    FullLC toMe = b.privateCall(1234567, 2345678);
    FullLC toOther = b.privateCall(1234567, 3456789);
    FullLC all = b.groupCall(1234567, ADDRESS_ALL);

    SECTION("monitor 0: everything filtered")
    {
        b.settle();
        b.ts();
        b.header(R5F_SYNC_BS, 2, 2, tg); /* wrong CC */
        REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
        b.ts();
        b.header(R5F_SYNC_BS, 1, 1, tg); /* wrong TS, RMO */
        REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
        b.ts();
        b.header(R5F_SYNC_BS, 2, 1, otherTg); /* other TG */
        REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
        b.ts();
        b.header(R5F_SYNC_BS, 2, 1, toOther); /* private to somebody */
        REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
        b.ts();
        b.header(R5F_SYNC_BS, 2, 1, toMe); /* private to me */
        REQUIRE(b.ctrl.state() == CallController::RX_CALL);
        REQUIRE(b.ctrl.report().rxFlco == FLCO_UU_V_CH_USR);
        b.ts();
        b.ts();
        b.terminator(R5F_SYNC_BS, 2, 1);
        b.ts();
        b.ts();
        b.header(R5F_SYNC_BS, 2, 1, all); /* all call */
        REQUIRE(b.ctrl.state() == CallController::RX_CALL);
        REQUIRE(b.ctrl.report().rxDst == ADDRESS_ALL);
    }

    SECTION("monitor 1: own CC and TS, any address")
    {
        b.cfg.monitor = 1;
        b.settle();
        b.ts();
        b.header(R5F_SYNC_BS, 2, 2, tg); /* wrong CC */
        REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
        b.ts();
        b.header(R5F_SYNC_BS, 1, 1, tg); /* wrong TS */
        REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
        b.ts();
        b.header(R5F_SYNC_BS, 2, 1, toOther); /* private to somebody */
        REQUIRE(b.ctrl.state() == CallController::RX_CALL);
        REQUIRE(b.ctrl.report().rxDst == 3456789);
        b.ts();
        b.ts();
        b.terminator(R5F_SYNC_BS, 2, 1);
        b.ts();
        b.ts();
        b.header(R5F_SYNC_BS, 2, 1, otherTg); /* other TG */
        REQUIRE(b.ctrl.state() == CallController::RX_CALL);
        REQUIRE(b.ctrl.report().rxDst == 91);
    }

    SECTION("monitor 2: anything")
    {
        b.cfg.monitor = 2;
        b.settle();
        b.ts();
        b.header(R5F_SYNC_BS, 1, 7, otherTg);
        REQUIRE(b.ctrl.state() == CallController::RX_CALL);
        REQUIRE(b.ctrl.report().rxColorCodeSeen == 7);
        REQUIRE(b.ctrl.report().rxTimeslot == 1);
    }

    SECTION("DMO with MS sync carries no timeslot: accepted")
    {
        b.settle();
        b.ts();
        b.header(R5F_SYNC_MS, 0, 1, tg);
        REQUIRE(b.ctrl.state() == CallController::RX_CALL);
        REQUIRE(b.ctrl.report().rxSyncType == 0);
        REQUIRE(b.ctrl.report().rxTimeslot == 2); /* configured one */
    }

    SECTION("DMO with TDMA sync of the other slot: filtered")
    {
        b.settle();
        b.ts();
        b.header(R5F_SYNC_TDMA1, 1, 1, tg);
        REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
        b.ts();
        b.header(R5F_SYNC_TDMA2, 2, 1, tg);
        REQUIRE(b.ctrl.state() == CallController::RX_CALL);
        REQUIRE(b.ctrl.report().rxTimeslot == 2);
    }

    SECTION("private call: no own ID means no private calls")
    {
        b.cfg.ownId = 0;
        b.settle();
        b.ts();
        b.header(R5F_SYNC_BS, 2, 1, toMe);
        REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
    }

    SECTION("PRIVATE call type does not open group calls")
    {
        b.cfg.callType = PRIVATE;
        b.cfg.dstId = 1234567;
        b.settle();
        b.ts();
        b.header(R5F_SYNC_BS, 2, 1, tg);
        REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
        b.ts();
        b.header(R5F_SYNC_BS, 2, 1, toMe);
        REQUIRE(b.ctrl.state() == CallController::RX_CALL);
    }
}

TEST_CASE("DMR RX: protected, reserved and manufacturer LCs", "[dmr][callctrl]")
{
    Bench b;
    FullLC tg = b.groupCall(1234567, 31665);

    SECTION("PF set: rejected")
    {
        b.settle();
        uint8_t raw[FULL_LC_BYTES];
        tg.pack(raw);
        raw[0] |= 0x80;
        b.ts();
        b.headerRaw(R5F_SYNC_BS, 2, 1, raw);
        REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
    }

    SECTION("R set: rejected")
    {
        b.settle();
        uint8_t raw[FULL_LC_BYTES];
        tg.pack(raw);
        raw[0] |= 0x40;
        b.ts();
        b.headerRaw(R5F_SYNC_BS, 2, 1, raw);
        REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
    }

    SECTION("reserved FID: rejected")
    {
        b.settle();
        FullLC lc = tg;
        lc.fid = 0x02;
        b.ts();
        b.header(R5F_SYNC_BS, 2, 1, lc);
        REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
    }

    SECTION("manufacturer FID: shown, voice not forwarded")
    {
        b.settle();
        FullLC lc = tg;
        lc.fid = 0x10;
        b.ts();
        b.header(R5F_SYNC_BS, 2, 1, lc);
        REQUIRE(b.ctrl.state() == CallController::RX_CALL);
        REQUIRE(b.ctrl.report().lcManufacturer == true);
        REQUIRE(b.ctrl.report().rxSrc == 1234567);
        b.superframe(R5F_SYNC_BS, 2, 1);
        REQUIRE(b.sink.rxFrames == 0);
    }

    SECTION("talker alias FLCO in a late entry: not a voice call")
    {
        b.settle();
        FullLC lc = tg;
        lc.flco = FLCO_TALKER_ALIAS_HDR;
        b.ts();
        b.lateEntry(R5F_SYNC_BS, 2, 1, lc);
        REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
    }
}

TEST_CASE("DMR RX late entry opens the call", "[dmr][callctrl]")
{
    Bench b;
    b.settle();

    FullLC lc = b.groupCall(1234567, 31665);
    b.ts();
    b.lateEntry(R5F_SYNC_BS, 2, 1, lc);
    REQUIRE(b.ctrl.state() == CallController::RX_CALL);
    REQUIRE(b.ctrl.report().rxSrc == 1234567);

    /* Voice arriving with the same event is forwarded from now on */
    b.superframe(R5F_SYNC_BS, 2, 1);
    REQUIRE(b.sink.rxFrames == 6);

    /* Late entry while the call is open changes nothing */
    b.ts();
    b.lateEntry(R5F_SYNC_BS, 2, 1, b.groupCall(7, 99));
    REQUIRE(b.ctrl.report().rxSrc == 1234567);
}

TEST_CASE("DMR RX: the first LC after a mode switch is stale",
          "[dmr][callctrl]")
{
    Bench b;
    b.start();
    b.tsN(6);

    FullLC lc = b.groupCall(1234567, 31665);
    b.header(R5F_SYNC_BS, 2, 1, lc);
    REQUIRE(b.ctrl.state() == CallController::RX_IDLE);

    b.ts();
    b.header(R5F_SYNC_BS, 2, 1, lc);
    REQUIRE(b.ctrl.state() == CallController::RX_CALL);

    /* Voice bursts do not consume the skip */
    b.ctrl.disable();
    b.ctrl.enable(b.now);
    b.tsN(6);
    b.voice(R5F_SYNC_BS, 2, 1, 1);
    b.header(R5F_SYNC_BS, 2, 1, lc);
    REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
}

TEST_CASE("DMR RX in direct mode opens only the call's slot", "[dmr][callctrl]")
{
    Bench b;
    b.settle();

    /* Idle: every slot is received */
    b.tsN(4);
    REQUIRE(b.port.count(W_R41, R41_RX) == 4);
    REQUIRE(b.port.count(W_R41, R41_IDLE) == 0);

    /*
     * The header is decoded right after the TS that started the following
     * slot: the call's slot is the one that just ended.
     */
    FullLC lc = b.groupCall(1234567, 31665);
    b.ts();
    uint8_t callTc = b.ctrl.report().timecode ^ 1;
    b.header(R5F_SYNC_MS, 0, 1, lc);
    REQUIRE(b.ctrl.state() == CallController::RX_CALL);

    /* From now on 0x41 alternates 0x50 / 0x00 following the timecode */
    b.port.clear();
    for (unsigned i = 0; i < 12; i++) {
        b.ts();
        uint8_t next = b.ctrl.report().timecode ^ 1;
        uint8_t expected = (next == callTc) ? R41_RX : R41_IDLE;
        REQUIRE(b.port.last(W_R41)->value == expected);
    }
    REQUIRE(b.port.count(W_R41, R41_RX) == 6);
    REQUIRE(b.port.count(W_R41, R41_IDLE) == 6);

    /* Disagreeing CACH bits do not flip the parity of a DMO call */
    for (unsigned i = 0; i < 8; i++)
        b.ts(false);
    REQUIRE(b.ctrl.report().timecode == (b.timecode ^ 1));
    REQUIRE(b.ctrl.state() == CallController::RX_CALL);
}

TEST_CASE("DMR TX: PTT without an ID is refused", "[dmr][callctrl]")
{
    Bench b;
    b.cfg.ownId = 0;
    b.settle();

    b.ctrl.setPtt(true, b.now);
    REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
    REQUIRE(b.ctrl.report().noId == true);
    REQUIRE(b.port.count(W_R40) == 0);
    REQUIRE(b.port.count(W_R21) == 0);

    /* Held PTT does not retry, marker cleared by the next press */
    b.tsN(4);
    REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
    b.ctrl.setPtt(false, b.now);
    REQUIRE(b.ctrl.report().noId == true);
    b.ctrl.clearMarkers();
    REQUIRE(b.ctrl.report().noId == false);
}

TEST_CASE("DMR TX direct mode: headers, voice, terminator", "[dmr][callctrl]")
{
    Bench b;
    b.settle();

    /* PTT: access policy and active timing */
    b.ctrl.setPtt(true, b.now);
    REQUIRE(b.ctrl.state() == CallController::TX_ARM);
    REQUIRE(b.ctrl.report().callState == DMR_CALL_TX);
    REQUIRE(b.port.values(W_R21) == std::vector<uint8_t>{ R21_POLITE });
    REQUIRE(b.port.values(W_R40) == std::vector<uint8_t>{ R40_TX_DMO });
    b.port.clear();

    /* Headers: three of them in the own slot, idle in the other one */
    unsigned n = 0;
    while (b.ctrl.state() != CallController::TX_VOICE) {
        b.ts();
        REQUIRE(++n < 10);
    }
    REQUIRE(b.port.count(W_R50, R50_VOICE_LC_HEADER) == 3);
    REQUIRE(b.port.count(W_LC) == 3);
    REQUIRE(b.port.count(W_R41, R41_TX) == 3);
    REQUIRE(b.port.count(W_R50) == 3);

    const uint8_t expectedLc[CHIP_LC_BYTES] = { 0x00, 0x00, 0x00, 0x00,
                                                0x7B, 0xB1, 0x23, 0xCA,
                                                0xCE, 0x00, 0x00, 0x00 };
    REQUIRE(memcmp(b.port.last(W_LC)->data, expectedLc, CHIP_LC_BYTES) == 0);

    /* Headers on the own slot, the other slot idles in between */
    std::vector<uint8_t> r41 = b.port.values(W_R41);
    REQUIRE(r41.size() >= 5);
    REQUIRE(r41.size() <= 6);
    for (size_t i = 0; i < r41.size(); i++) {
        if (r41[i] != R41_TX)
            REQUIRE(r41[i] == R41_IDLE);
        if (i > 0)
            REQUIRE(r41[i] != r41[i - 1]);
    }

    /* Voice: A..F cycling, 27 byte silence, other slot idle */
    b.port.clear();
    b.sink.txRequests = 0;
    for (unsigned i = 0; i < 24; i++)
        b.ts();
    REQUIRE(b.ctrl.state() == CallController::TX_VOICE);
    REQUIRE(b.sink.txRequests == 12);
    REQUIRE(b.port.count(W_VOICE) == 12);
    REQUIRE(b.port.count(W_R41, R41_TX) == 12);
    REQUIRE(b.port.count(W_R41, R41_IDLE) == 12);

    std::vector<uint8_t> r50 = b.port.values(W_R50);
    REQUIRE(r50.size() == 12);
    for (size_t i = 0; i < r50.size(); i++)
        REQUIRE(r50[i] == r50Voice(i % 6));

    for (const Write &w : b.port.writes)
        if (w.kind == W_VOICE)
            REQUIRE(memcmp(w.data, AMBE_SILENCE, VOICE_PAYLOAD_BYTES) == 0);

    /* The sink's frames are used when it provides them */
    b.sink.provide = true;
    b.ts();
    b.ts();
    REQUIRE(b.port.last(W_VOICE)->data[0] == 0x5A);
    REQUIRE(b.port.last(W_VOICE)->data[26] == 0x5A);
    b.sink.provide = false;

    /* Release after burst A: B to F go out, then the terminator */
    b.port.clear();
    b.ctrl.setPtt(false, b.now);
    REQUIRE(b.ctrl.state() == CallController::TX_VOICE);
    n = 0;
    while (b.ctrl.state() != CallController::TX_TERM) {
        b.ts();
        REQUIRE(++n < 14);
    }
    r50 = b.port.values(W_R50);
    REQUIRE(r50
            == std::vector<uint8_t>{ r50Voice(1), r50Voice(2), r50Voice(3),
                                     r50Voice(4), r50Voice(5),
                                     R50_TERMINATOR });
    REQUIRE(b.port.count(W_R50, R50_TERMINATOR) == 1);

    /* Next TS: idle the slot, back to RX with the hang timer */
    b.port.clear();
    b.ts();
    REQUIRE(b.ctrl.state() == CallController::RX_HANG);
    REQUIRE(b.port.values(W_R41)
            == std::vector<uint8_t>{ R41_IDLE, R41_RESYNC, R41_RX });
    REQUIRE(b.port.values(W_R40) == std::vector<uint8_t>{ R40_RX });
    REQUIRE(b.ctrl.report().hangActive == true);
    REQUIRE(b.ctrl.report().lcOk == false);
    REQUIRE(b.port.count(W_R50) == 0);

    /* Hang expires, PTT works again */
    b.tsN(101);
    REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
    b.ctrl.setPtt(true, b.now);
    REQUIRE(b.ctrl.state() == CallController::TX_ARM);
}

TEST_CASE("DMR TX: PTT released before the header sends nothing",
          "[dmr][callctrl]")
{
    Bench b;
    b.settle();

    b.ctrl.setPtt(true, b.now);
    REQUIRE(b.ctrl.state() == CallController::TX_ARM);
    b.port.clear();
    b.ctrl.setPtt(false, b.now);
    REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
    REQUIRE(b.port.count(W_R50) == 0);
    REQUIRE(b.port.values(W_R40) == std::vector<uint8_t>{ R40_RX });
}

TEST_CASE("DMR TX: released right after the headers sends a terminator",
          "[dmr][callctrl]")
{
    Bench b;
    b.settle();
    b.ctrl.setPtt(true, b.now);
    b.ts();
    b.ctrl.setPtt(false, b.now);

    unsigned n = 0;
    while (b.ctrl.state() != CallController::TX_TERM) {
        b.ts();
        REQUIRE(++n < 12);
    }
    REQUIRE(b.port.count(W_R50, R50_VOICE_LC_HEADER) == 3);
    REQUIRE(b.port.count(W_R50, R50_VOICE) == 0);
    REQUIRE(b.port.count(W_R50, R50_TERMINATOR) == 1);
}

TEST_CASE("DMR TX: private and all calls, impolite access", "[dmr][callctrl]")
{
    Bench b;

    SECTION("private call")
    {
        b.cfg.callType = PRIVATE;
        b.cfg.dstId = 3456789;
        b.cfg.polite = false;
        b.settle();
        b.ctrl.setPtt(true, b.now);
        REQUIRE(b.port.values(W_R21) == std::vector<uint8_t>{ R21_IMPOLITE });
        b.tsN(2);
        const Write *lc = b.port.last(W_LC);
        REQUIRE(lc != nullptr);
        REQUIRE(lc->data[0] == FLCO_UU_V_CH_USR);
        REQUIRE(lc->data[3] == 0x34);
        REQUIRE(lc->data[4] == 0xBF);
        REQUIRE(lc->data[5] == 0x15);
    }

    SECTION("all call")
    {
        b.cfg.callType = ALL;
        b.settle();
        b.ctrl.setPtt(true, b.now);
        b.tsN(2);
        const Write *lc = b.port.last(W_LC);
        REQUIRE(lc != nullptr);
        REQUIRE(lc->data[0] == FLCO_GRP_V_CH_USR);
        REQUIRE(lc->data[2] == SO_BROADCAST);
        REQUIRE(lc->data[3] == 0xFF);
        REQUIRE(lc->data[4] == 0xFF);
        REQUIRE(lc->data[5] == 0xFF);
    }
}

TEST_CASE("DMR TX: T_TO ends the transmission after the superframe",
          "[dmr][callctrl]")
{
    Bench b;
    b.cfg.txTimeoutMs = 1000;
    b.settle();

    b.ctrl.setPtt(true, b.now);
    unsigned n = 0;
    while (b.ctrl.state() != CallController::TX_VOICE) {
        b.ts();
        REQUIRE(++n < 10);
    }

    /* 1 s of voice, then the superframe completes and the terminator */
    n = 0;
    while (b.ctrl.state() != CallController::TX_TERM) {
        b.ts();
        REQUIRE(++n < 60);
    }
    std::vector<uint8_t> r50 = b.port.values(W_R50);
    REQUIRE(r50.size() >= 3 + 6 + 1);
    REQUIRE(r50[r50.size() - 1] == R50_TERMINATOR);
    REQUIRE(r50[r50.size() - 2] == r50Voice(5));
    REQUIRE(b.ctrl.report().callState == DMR_CALL_TX);

    /* Held PTT does not restart until released */
    b.tsN(3);
    REQUIRE(b.ctrl.state() == CallController::RX_HANG);
    b.tsN(2);
    REQUIRE(b.ctrl.state() == CallController::RX_HANG);
    b.ctrl.setPtt(false, b.now);
    b.ctrl.setPtt(true, b.now);
    REQUIRE(b.ctrl.state() == CallController::TX_ARM);
}

TEST_CASE("DMR TX rejected: back to RX with the busy marker", "[dmr][callctrl]")
{
    Bench b;
    b.settle();

    b.ctrl.setPtt(true, b.now);
    b.tsN(3);
    REQUIRE(b.ctrl.state() == CallController::TX_HEADER);
    b.port.clear();
    b.flags(R82_TX_REJECTED);
    REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
    REQUIRE(b.ctrl.report().busy == true);
    REQUIRE(b.ctrl.report().callState == DMR_CALL_IDLE);
    REQUIRE(b.port.values(W_R40) == std::vector<uint8_t>{ R40_RX });

    /* PTT still held: nothing until it is released */
    b.tsN(4);
    REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
    b.ctrl.setPtt(false, b.now);
    b.ctrl.setPtt(true, b.now);
    REQUIRE(b.ctrl.report().busy == false);
    REQUIRE(b.ctrl.state() == CallController::TX_ARM);
}

TEST_CASE("DMR TX repeater mode with carrier: passive timing",
          "[dmr][callctrl]")
{
    Bench b;
    b.settle();
    b.repeaterCarrier();
    REQUIRE(b.ctrl.report().slotLock == 2);
    b.port.clear();

    /* Not a repeater channel, but a locked BS sync: RMO */
    b.ctrl.setPtt(true, b.now);
    REQUIRE(b.ctrl.state() == CallController::TX_ARM);
    REQUIRE(b.port.values(W_R40) == std::vector<uint8_t>{ R40_TX_RMO });
    REQUIRE(b.port.count(W_R50, R50_CSBK) == 0);

    unsigned n = 0;
    while (b.ctrl.state() != CallController::TX_VOICE) {
        b.ts();
        REQUIRE(++n < 10);
    }
    REQUIRE(b.port.count(W_R50, R50_VOICE_LC_HEADER) == 3);

    /* The headers went out on the configured timeslot */
    REQUIRE(b.ctrl.config().timeslot == 2);
}

TEST_CASE("DMR TX repeater mode waits for the timecode lock", "[dmr][callctrl]")
{
    Bench b;
    b.settle();

    /* A BS sync seen, but the timecode not locked yet */
    b.ts();
    b.dataBurst(R5F_SYNC_BS, 1, 1, DT_IDLE);
    b.ts(false);
    b.ts(false);
    b.ts(false);
    b.ts(false);
    b.timecode ^= 1;
    REQUIRE(b.ctrl.report().slotLock == 1);
    b.port.clear();

    b.cfg.repeater = true;
    b.ctrl.configure(b.cfg);
    b.ctrl.setPtt(true, b.now);
    REQUIRE(b.ctrl.state() == CallController::TX_ARM);
    REQUIRE(b.port.count(W_R40) == 0);

    /* Carrier kept up while the lock builds: still receiving */
    for (unsigned i = 0; i < 5; i++) {
        b.ts();
        b.dataBurst(R5F_SYNC_BS, 1, 1, DT_IDLE);
    }
    REQUIRE(b.ctrl.report().slotLock == 2);
    REQUIRE(b.port.values(W_R40) == std::vector<uint8_t>{ R40_TX_RMO });
    REQUIRE(b.port.count(W_R41, R41_RX) >= 4);

    unsigned n = 0;
    while (b.ctrl.state() != CallController::TX_HEADER) {
        b.ts();
        REQUIRE(++n < 4);
    }
}

TEST_CASE("DMR TX repeater wake-up succeeds", "[dmr][callctrl]")
{
    Bench b;
    b.cfg.repeater = true;
    b.settle();
    /* No carrier: 20 slots without a burst */
    b.tsN(20);
    b.tsN(2);
    b.port.clear();

    b.ctrl.setPtt(true, b.now);
    REQUIRE(b.ctrl.state() == CallController::TX_WAKEUP);
    REQUIRE(b.ctrl.report().callState == DMR_CALL_TX_WAKEUP);
    REQUIRE(b.port.values(W_R21) == std::vector<uint8_t>{ R21_POLITE });
    REQUIRE(b.port.values(W_R40) == std::vector<uint8_t>{ R40_TX_DMO });

    /* CSBK in the own slot, then back to RX */
    unsigned n = 0;
    while (b.port.count(W_R50, R50_CSBK) == 0) {
        b.ts();
        REQUIRE(++n < 4);
    }
    const Write *csbk = b.port.last(W_LC);
    REQUIRE(csbk != nullptr);
    uint8_t expected[CSBK_DATA_BYTES];
    buildBsDwnAct(2345678, expected);
    REQUIRE(memcmp(csbk->data, expected, CSBK_DATA_BYTES) == 0);
    REQUIRE(csbk->data[0] == 0xB8);
    REQUIRE(csbk->data[7] == 0x23);
    REQUIRE(csbk->data[8] == 0xCA);
    REQUIRE(csbk->data[9] == 0xCE);
    REQUIRE(b.port.last(W_R41)->value == R41_TX);

    b.port.clear();
    b.ts();
    REQUIRE(b.port.values(W_R41)
            == std::vector<uint8_t>{ R41_IDLE, R41_RESYNC, R41_RX });
    REQUIRE(b.port.values(W_R40) == std::vector<uint8_t>{ R40_RX });
    REQUIRE(b.ctrl.state() == CallController::TX_WAKEUP);

    /* The repeater answers: BS sync, timecode lock, then the headers */
    b.port.clear();
    b.timecode = 0;
    for (unsigned i = 0; i < 6; i++) {
        b.ts();
        b.dataBurst(R5F_SYNC_BS, 1, 1, DT_IDLE);
    }
    REQUIRE(b.ctrl.report().slotLock == 2);
    REQUIRE(b.port.values(W_R40) == std::vector<uint8_t>{ R40_TX_RMO });
    REQUIRE(b.port.count(W_R50, R50_CSBK) == 0);

    n = 0;
    while (b.ctrl.state() != CallController::TX_VOICE) {
        b.ts();
        REQUIRE(++n < 10);
    }
    REQUIRE(b.port.count(W_R50, R50_VOICE_LC_HEADER) == 3);
    REQUIRE(b.ctrl.report().wakeupFailed == false);
}

TEST_CASE("DMR TX repeater wake-up fails after N_Wakeup attempts",
          "[dmr][callctrl]")
{
    Bench b;
    b.cfg.repeater = true;
    b.settle();
    b.tsN(20);
    b.tsN(2);
    b.port.clear();

    b.ctrl.setPtt(true, b.now);
    REQUIRE(b.ctrl.state() == CallController::TX_WAKEUP);

    /* Two attempts, T_SyncWu apart, then give up */
    unsigned n = 0;
    while (b.ctrl.state() == CallController::TX_WAKEUP) {
        b.ts();
        REQUIRE(++n < 60);
    }
    REQUIRE(b.port.count(W_R50, R50_CSBK) == 2);
    REQUIRE(b.port.count(W_R40, R40_TX_DMO) == 2);
    REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
    REQUIRE(b.ctrl.report().wakeupFailed == true);
    REQUIRE(b.ctrl.report().callState == DMR_CALL_IDLE);
    REQUIRE(b.port.count(W_R50, R50_VOICE_LC_HEADER) == 0);

    /* Roughly 2 x T_SyncWu plus the bursts: 12 to 30 slots */
    REQUIRE(n >= 20);
    REQUIRE(n <= 36);

    /* Still receiving, PTT needs a release */
    b.tsN(2);
    REQUIRE(b.port.last(W_R41)->value == R41_RX);
    b.ctrl.setPtt(false, b.now);
    b.ctrl.setPtt(true, b.now);
    REQUIRE(b.ctrl.state() == CallController::TX_WAKEUP);
    REQUIRE(b.ctrl.report().wakeupFailed == false);
}

TEST_CASE("DMR TX repeater wake-up aborted by the PTT release",
          "[dmr][callctrl]")
{
    Bench b;
    b.cfg.repeater = true;
    b.settle();
    b.tsN(20);
    b.tsN(2);

    b.ctrl.setPtt(true, b.now);
    b.tsN(3);
    b.port.clear();
    b.ctrl.setPtt(false, b.now);
    REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
    REQUIRE(b.port.values(W_R40) == std::vector<uint8_t>{ R40_RX });
}

TEST_CASE("DMR timeslot watchdog", "[dmr][callctrl]")
{
    Bench b;
    b.settle();
    b.port.clear();

    /* Short gaps do nothing */
    b.timeout(30);
    b.timeout(30);
    b.timeout(30);
    REQUIRE(b.port.writes.empty());
    REQUIRE(b.ctrl.report().needsReconfigure == false);

    /* 200 ms without a TS: startRx once */
    b.timeout(120);
    REQUIRE(b.port.values(W_R40) == std::vector<uint8_t>{ R40_RX });
    REQUIRE(b.port.values(W_R41) == std::vector<uint8_t>{ R41_RESYNC, R41_RX });
    REQUIRE(b.ctrl.report().slotLock == 0);
    REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
    b.timeout(30);
    b.timeout(30);
    REQUIRE(b.port.count(W_R40) == 1);

    /* 2 s: reconfiguration requested, once */
    while (b.ctrl.report().needsReconfigure == false)
        b.timeout(30);
    REQUIRE(b.port.count(W_R40) == 2);
    REQUIRE(b.ctrl.state() == CallController::RX_SEARCH);
    b.timeout(30);
    b.timeout(30);
    REQUIRE(b.port.count(W_R40) == 2);

    /* The owner reconfigures the modem and re-enables: fresh start */
    b.ctrl.enable(b.now);
    REQUIRE(b.ctrl.report().needsReconfigure == false);
    b.ts();
    REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
    REQUIRE(b.ctrl.report().slotLock == 1);

    /* A TS arriving clears the restart flag: the next gap restarts again */
    b.port.clear();
    b.timeout(210);
    REQUIRE(b.port.count(W_R40) == 1);
}

TEST_CASE("DMR timeslot watchdog during TX aborts the call", "[dmr][callctrl]")
{
    Bench b;
    b.settle();
    b.ctrl.setPtt(true, b.now);
    b.tsN(4);
    REQUIRE(b.ctrl.state() == CallController::TX_HEADER);

    b.timeout(250);
    REQUIRE(b.ctrl.state() == CallController::RX_SEARCH);
    REQUIRE(b.port.last(W_R40)->value == R40_RX);

    /* Held PTT stays ignored */
    b.tsN(3);
    REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
}

TEST_CASE("DMR hang time zero goes straight to idle", "[dmr][callctrl]")
{
    Bench b;
    b.cfg.hangTimeMs = 0;
    b.settle();

    FullLC lc = b.groupCall(1234567, 31665);
    b.ts();
    b.header(R5F_SYNC_BS, 2, 1, lc);
    b.ts();
    b.ts();
    b.terminator(R5F_SYNC_BS, 2, 1);
    REQUIRE(b.ctrl.state() == CallController::RX_IDLE);
    REQUIRE(b.ctrl.report().hangActive == false);
}
