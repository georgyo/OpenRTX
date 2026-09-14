/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include "emulator/emulator.h"
#include "interfaces/delays.h"
#include "interfaces/dmr_baseband.h"
#include "protocols/DMR/Constants.hpp"
#include "protocols/DMR/LinkControl.hpp"
#include "protocols/DMR/ModemPort.hpp"

/*
 * Fake DMR modem of the Linux emulator, see emulator.h for the interface the
 * tests and the emulator shell use. Two queues: the event queue, what the
 * next dmrbb_waitEvent() calls return, and the script queue, timeslot-paced
 * events (a scripted call) that a tick moves to the event queue one timeslot
 * at a time. In the running emulator a thread ticks every 30 ms and fills
 * the idle slots with plain timeslot events; in the unit tests a tick
 * happens inside dmrbb_waitEvent() when the event queue is empty and the
 * script is not, so that the test consumes one timeslot per call.
 *
 * Everything is a plain array with static storage: this object is used by
 * the static OpMode_DMR up to the end of the process, it must not have a
 * destructor of its own.
 */

using namespace DMR;

namespace
{

struct Event {
    uint32_t mask; /* dmrbbEvent bits                                 */
    uint8_t slot;  /* scripted TS: required timeslot 1/2, 0 = any     */
    struct dmrbbSnapshot snap;
};

template <unsigned N> struct Ring {
    Event buf[N];
    unsigned head;
    unsigned tail;
    unsigned count;

    bool empty() const
    {
        return count == 0;
    }

    bool full() const
    {
        return count == N;
    }

    bool push(const Event &e)
    {
        if (full())
            return false;
        buf[tail] = e;
        tail = (tail + 1) % N;
        count++;
        return true;
    }

    Event &front()
    {
        return buf[head];
    }

    void pop()
    {
        head = (head + 1) % N;
        count--;
    }

    void clear()
    {
        head = 0;
        tail = 0;
        count = 0;
    }
};

constexpr unsigned EVENT_QUEUE_SIZE = 256;
constexpr unsigned SCRIPT_QUEUE_SIZE = 2048;
constexpr unsigned TICK_US = 30000;

/* Synthetic call replay: first call after 2 s, then every 9 s */
constexpr uint32_t REPLAY_FIRST_TICKS = 67;
constexpr uint32_t REPLAY_PERIOD_TICKS = 300;
constexpr unsigned REPLAY_BURSTS = 50;

pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cv = PTHREAD_COND_INITIALIZER;

Ring<EVENT_QUEUE_SIZE> events;
Ring<SCRIPT_QUEUE_SIZE> script;
struct dmrEmuRecord rec;
uint8_t regs[8][256];

int initResult = 0;
bool clockEnabled = false;
bool threadRunning = false;
pthread_t clockThread;

bool running = false; /* 0x40 has TxEn/RxEn: timeslots tick   */
uint8_t tcNext = 0;   /* CACH TC of the next timeslot started */
uint32_t tsCount = 0;
uint32_t sysCount = 0;

uint32_t replaySrc = 0;
uint32_t replayDst = 0;
uint32_t replayTicks = 0;

/*
 * Helpers, all called with the mutex held.
 */

void pushEvent(const Event &e)
{
    if (!events.push(e))
        return;
    pthread_cond_signal(&cv);
}

void pushTsEvent(uint8_t cc)
{
    Event e;
    memset(&e, 0x00, sizeof(e));
    e.mask = DMRBB_EV_TS;
    e.snap.r52 = (uint8_t)((cc << 4) | (tcNext ? R52_TC : 0));
    e.snap.tsCount = ++tsCount;
    e.snap.sysCount = sysCount;
    e.snap.tsTick = (uint32_t)getTick();
    tcNext ^= 1;
    pushEvent(e);
}

void pushSysEvent(const struct dmrbbSnapshot &s)
{
    Event e;
    memset(&e, 0x00, sizeof(e));
    e.mask = DMRBB_EV_SYS;
    e.snap = s;
    e.snap.tsCount = tsCount;
    e.snap.sysCount = ++sysCount;
    pushEvent(e);
}

/*
 * One timeslot: play the script or an idle tick. A scripted timeslot that
 * asks for a given slot waits for the timecode to come round; the system
 * events following it in the script belong to the same tick.
 */
void tick()
{
    if (script.empty()) {
        if (running)
            pushTsEvent(rec.config.colorCode);
        return;
    }

    Event &head = script.front();
    if (head.mask & DMRBB_EV_TS) {
        uint8_t cc = head.snap.r52 >> 4;
        if ((head.slot != 0) && ((uint8_t)(head.slot - 1) != tcNext)) {
            pushTsEvent(cc);
            return;
        }

        script.pop();
        pushTsEvent(cc);
    }

    while (!script.empty() && ((script.front().mask & DMRBB_EV_TS) == 0)) {
        pushSysEvent(script.front().snap);
        script.pop();
    }
}

void scriptTs(uint8_t slot, uint8_t cc)
{
    Event e;
    memset(&e, 0x00, sizeof(e));
    e.mask = DMRBB_EV_TS;
    e.slot = slot;
    e.snap.r52 = (uint8_t)(cc << 4);
    script.push(e);
}

void scriptSys(const struct dmrbbSnapshot &s)
{
    Event e;
    memset(&e, 0x00, sizeof(e));
    e.mask = DMRBB_EV_SYS;
    e.snap = s;
    script.push(e);
}

/*
 * A base-station sourced burst on the given slot, decoded 4 ms after the
 * slot end, i.e. after the next timeslot started.
 */
struct dmrbbSnapshot bsBurst(uint8_t cc, uint8_t ts)
{
    struct dmrbbSnapshot s;
    memset(&s, 0x00, sizeof(s));
    s.irq82 = R82_CTRL_FRAME;
    s.r5f = R5F_SYNC_BS;
    s.r52 = (uint8_t)((cc << 4) | ((ts == 2) ? R52_TC : 0));
    return s;
}

void scriptBurst(uint8_t cc, uint8_t ts, const struct dmrbbSnapshot &s)
{
    uint8_t other = (ts == 2) ? 1 : 2;
    scriptTs(ts, cc);
    scriptTs(other, cc);
    scriptSys(s);
}

void scriptGroupCall(uint32_t src, uint32_t dst, uint8_t cc, uint8_t ts,
                     unsigned bursts)
{
    if ((ts != 1) && (ts != 2))
        ts = 1;
    cc &= 0x0F;

    FullLC lc;
    lc.clear();
    lc.flco = FLCO_GRP_V_CH_USR;
    lc.fid = FID_STANDARD;
    lc.dst = dst & ADDRESS_MASK;
    lc.src = src & ADDRESS_MASK;

    /* Three headers, TS 102 361-2 §5.2.1.1 */
    for (unsigned i = 0; i < 3; i++) {
        struct dmrbbSnapshot s = bsBurst(cc, ts);
        s.r51 = (uint8_t)((DT_VOICE_LC_HEADER << 4) | R51_CLASS_HEADER);
        lc.toChipLc(s.lc);
        scriptBurst(cc, ts, s);
    }

    /* Voice bursts A..F, the payload is a recognisable pattern */
    for (unsigned i = 0; i < bursts; i++) {
        uint8_t seq = (uint8_t)((i % SUPERFRAME_BURST) + 1);
        struct dmrbbSnapshot s = bsBurst(cc, ts);
        s.r51 = (uint8_t)((seq << 4) | R51_CLASS_VOICE);
        s.voiceValid = true;
        memset(s.voice, (int)(0xA0 | seq), VOICE_PAYLOAD_BYTES);
        scriptBurst(cc, ts, s);
    }

    /* Terminator with LC */
    struct dmrbbSnapshot s = bsBurst(cc, ts);
    s.r51 = (uint8_t)((DT_TERMINATOR_LC << 4) | R51_CLASS_HEADER);
    lc.toChipLc(s.lc);
    scriptBurst(cc, ts, s);
}

void classifyTxSlot()
{
    rec.txSlots++;
    if (rec.r50 == R50_VOICE_LC_HEADER)
        rec.headers++;
    else if (rec.r50 == R50_TERMINATOR)
        rec.terminators++;
    else if (rec.r50 == R50_CSBK)
        rec.csbks++;
    else if (rec.r50 & R50_VOICE)
        rec.voiceBursts++;
}

void *clockLoop(void *arg)
{
    (void)arg;

    while (true) {
        usleep(TICK_US);

        pthread_mutex_lock(&mtx);
        if (!threadRunning) {
            pthread_mutex_unlock(&mtx);
            break;
        }

        if (running) {
            /* Synthetic call replay, only while receiving */
            if ((replaySrc != 0) && script.empty() && (rec.r40 == R40_RX)) {
                replayTicks++;
                if (replayTicks >= REPLAY_PERIOD_TICKS) {
                    replayTicks = 0;
                    printf("dmr_linux: replaying call %lu -> TG %lu\n",
                           (unsigned long)replaySrc, (unsigned long)replayDst);
                    scriptGroupCall(replaySrc, replayDst, rec.config.colorCode,
                                    rec.config.timeslot, REPLAY_BURSTS);
                }
            }

            tick();
        }
        pthread_mutex_unlock(&mtx);
    }

    return NULL;
}

void startClock()
{
    if (threadRunning)
        return;

    threadRunning = true;
    if (pthread_create(&clockThread, NULL, clockLoop, NULL) != 0) {
        threadRunning = false;
        printf("dmr_linux: cannot start the clock thread\n");
    }
}

void stopClock()
{
    if (!threadRunning)
        return;

    threadRunning = false;
    pthread_mutex_unlock(&mtx);
    pthread_join(clockThread, NULL);
    pthread_mutex_lock(&mtx);
}

void emuLog(const char *msg)
{
    if (clockEnabled)
        printf("dmr_linux: %s\n", msg);
}

} /* namespace */

/*
 * dmr_baseband.h
 */

int dmrbb_init(void)
{
    pthread_mutex_lock(&mtx);
    rec.initCount++;

    if (initResult != 0) {
        pthread_mutex_unlock(&mtx);
        emuLog("init() refused, no DMR modem");
        return initResult;
    }

    running = false;
    replayTicks = 0;

    if (clockEnabled) {
        if (emulator_state.dmrCallSrc != 0) {
            replaySrc = emulator_state.dmrCallSrc;
            replayDst = emulator_state.dmrCallDst;
        }
        if (replaySrc != 0)
            replayTicks = REPLAY_PERIOD_TICKS - REPLAY_FIRST_TICKS;
        startClock();
    }

    pthread_mutex_unlock(&mtx);
    emuLog("init() called");
    return 0;
}

void dmrbb_terminate(void)
{
    pthread_mutex_lock(&mtx);
    rec.terminateCount++;
    rec.r40 = R40_IDLE;
    running = false;
    stopClock();
    pthread_mutex_unlock(&mtx);
    emuLog("terminate() called");
}

void dmrbb_configure(const struct dmrbbConfig *cfg)
{
    pthread_mutex_lock(&mtx);
    rec.configureCount++;
    rec.config = *cfg;
    rec.r40 = R40_IDLE;
    rec.r41 = R41_IDLE;
    running = false;
    pthread_mutex_unlock(&mtx);

    if (clockEnabled)
        printf("dmr_linux: configure() CC %u TS %u ID %lu %s mode 0x%02X\n",
               cfg->colorCode, cfg->timeslot, (unsigned long)cfg->ownId,
               cfg->polite ? "polite" : "impolite", cfg->modeReg);
}

void dmrbb_startRx(void)
{
    pthread_mutex_lock(&mtx);
    rec.startRxCount++;
    rec.r40 = R40_RX;
    rec.r40Writes++;
    rec.r41 = R41_RESYNC;
    rec.r41Writes++;
    rec.r41 = R41_RX;
    rec.r41Writes++;
    running = true;
    pthread_mutex_unlock(&mtx);
    emuLog("startRx() called");
}

void dmrbb_startTx(bool activeTiming)
{
    pthread_mutex_lock(&mtx);
    rec.startTxCount++;
    rec.activeTiming = activeTiming;
    rec.r21 = rec.config.polite ? R21_POLITE : R21_IMPOLITE;
    rec.r40 = activeTiming ? R40_TX_DMO : R40_TX_RMO;
    rec.r40Writes++;
    running = true;
    pthread_mutex_unlock(&mtx);
    emuLog(activeTiming ? "startTx(active timing) called" :
                          "startTx(passive timing) called");
}

void dmrbb_idle(void)
{
    pthread_mutex_lock(&mtx);
    rec.idleCount++;
    rec.r40 = R40_IDLE;
    rec.r40Writes++;
    running = false;
    pthread_mutex_unlock(&mtx);
    emuLog("idle() called");
}

void dmrbb_setNextSlot(uint8_t r41)
{
    pthread_mutex_lock(&mtx);
    rec.r41 = r41;
    rec.r41Writes++;
    if (r41 == R41_TX)
        classifyTxSlot();
    pthread_mutex_unlock(&mtx);
}

void dmrbb_setTxFrameType(uint8_t r50)
{
    pthread_mutex_lock(&mtx);
    rec.r50 = r50;
    rec.r50Writes++;
    pthread_mutex_unlock(&mtx);
}

void dmrbb_writeTxLc(const uint8_t lc[12])
{
    pthread_mutex_lock(&mtx);
    memcpy(rec.lc, lc, sizeof(rec.lc));
    rec.lcWrites++;
    pthread_mutex_unlock(&mtx);
}

void dmrbb_writeVoice(const uint8_t ambe[27])
{
    pthread_mutex_lock(&mtx);
    memcpy(rec.voice, ambe, sizeof(rec.voice));
    rec.voiceWrites++;
    pthread_mutex_unlock(&mtx);
}

uint32_t dmrbb_waitEvent(struct dmrbbSnapshot *s, unsigned timeoutMs)
{
    pthread_mutex_lock(&mtx);

    if (!clockEnabled) {
        /* Scripted: one timeslot of the script per call, no waiting */
        if (events.empty() && !script.empty())
            tick();
    } else if (events.empty()) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += timeoutMs / 1000;
        deadline.tv_nsec += (long)(timeoutMs % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }

        while (events.empty()) {
            int ret = pthread_cond_timedwait(&cv, &mtx, &deadline);
            if (ret == ETIMEDOUT)
                break;
        }
    }

    uint32_t mask = DMRBB_EV_TIMEOUT;
    if (!events.empty()) {
        const Event &e = events.front();
        mask = e.mask;
        *s = e.snap;
        events.pop();

        if (mask & DMRBB_EV_TS)
            rec.tsDelivered++;
        if (mask & DMRBB_EV_SYS)
            rec.sysDelivered++;
    } else {
        rec.timeouts++;
    }

    pthread_mutex_unlock(&mtx);
    return mask;
}

uint8_t dmrbb_readRegister(uint8_t page, uint8_t addr)
{
    return regs[page & 0x07][addr];
}

void dmrbb_writeRegister(uint8_t page, uint8_t addr, uint8_t v)
{
    regs[page & 0x07][addr] = v;
}

/*
 * emulator.h hooks
 */

void dmrEmu_reset(void)
{
    pthread_mutex_lock(&mtx);
    events.clear();
    script.clear();
    memset(&rec, 0x00, sizeof(rec));
    memset(regs, 0x00, sizeof(regs));
    initResult = 0;
    running = false;
    tcNext = 0;
    tsCount = 0;
    sysCount = 0;
    replaySrc = 0;
    replayDst = 0;
    replayTicks = 0;
    pthread_mutex_unlock(&mtx);
}

void dmrEmu_setInitResult(int result)
{
    pthread_mutex_lock(&mtx);
    initResult = result;
    pthread_mutex_unlock(&mtx);
}

void dmrEmu_pushTs(void)
{
    pthread_mutex_lock(&mtx);
    pushTsEvent(rec.config.colorCode);
    pthread_mutex_unlock(&mtx);
}

void dmrEmu_pushSys(const struct dmrbbSnapshot *s)
{
    pthread_mutex_lock(&mtx);
    pushSysEvent(*s);
    pthread_mutex_unlock(&mtx);
}

void dmrEmu_injectGroupCall(uint32_t src, uint32_t dst, uint8_t cc, uint8_t ts,
                            unsigned bursts)
{
    pthread_mutex_lock(&mtx);
    scriptGroupCall(src, dst, cc, ts, bursts);
    pthread_mutex_unlock(&mtx);
}

const struct dmrEmuRecord *dmrEmu_record(void)
{
    return &rec;
}

unsigned dmrEmu_pending(void)
{
    pthread_mutex_lock(&mtx);
    unsigned n = events.count + script.count;
    pthread_mutex_unlock(&mtx);
    return n;
}

void dmrEmu_setClock(bool enable)
{
    pthread_mutex_lock(&mtx);
    clockEnabled = enable;
    pthread_mutex_unlock(&mtx);
}

void dmrEmu_requestCall(uint32_t src, uint32_t dst)
{
    pthread_mutex_lock(&mtx);
    replaySrc = src;
    replayDst = dst;
    replayTicks = (src != 0) ? REPLAY_PERIOD_TICKS : 0;
    pthread_mutex_unlock(&mtx);
}
