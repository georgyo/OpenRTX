/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host tests of the DMR audio thread (core/dmr_audio.c), in the style of
 * audio_threading.cpp: the audio devices are a fake driver whose sync()
 * blocks until the test grants a "tick", so that the test decides when a
 * 480-sample block is consumed by the speaker or delivered by the
 * microphone, and records every block written to the output stream. The
 * vocoder backend is a fake provided here as well: decode() fills the block
 * with a pattern carrying the first payload byte, encode() recovers the
 * block index from the level of a square wave the fake microphone delivers.
 * pthread_join() is wrapped (-Wl,--wrap=pthread_join) to reproduce the
 * Miosix semantics where a second concurrent join fails at once.
 */

#include <catch2/catch_test_macros.hpp>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <atomic>
#include <vector>
#include "interfaces/audio.h"
#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "core/dmr_audio.h"
#include "core/dmr_vocoder.h"
#include "protocols/DMR/Silence.h"

static const size_t BLOCK = DMR_VOCODER_PCM_SAMPLES;
static const size_t AMBE = DMR_VOCODER_AMBE_BYTES;

/*
 * Miosix-like pthread_join(): only one thread at a time can join a given
 * thread, any other joiner gets EINVAL immediately.
 */
extern "C" int __real_pthread_join(pthread_t thread, void **retval);

static std::atomic<int> joiners(0);
static std::atomic<int> rejectedJoins(0);

extern "C" int __wrap_pthread_join(pthread_t thread, void **retval)
{
    int expected = 0;
    if (joiners.compare_exchange_strong(expected, 1) == false) {
        rejectedJoins += 1;
        return EINVAL;
    }

    int ret = __real_pthread_join(thread, retval);
    joiners.store(0);

    return ret;
}

/*
 * Fake vocoder backend, the one dmrVocoder_get() returns in this test
 * executable (neither in-tree backend is linked).
 */
static std::atomic<int> vocoderInits(0);
static std::atomic<int> vocoderTerminates(0);

static int16_t rxPattern(uint8_t index, size_t i)
{
    return (int16_t)(((int)index << 8) | (int)(i & 0xFF));
}

static int fakeVocoderInit(void)
{
    vocoderInits += 1;
    return 0;
}

static void fakeVocoderTerminate(void)
{
    vocoderTerminates += 1;
}

static bool fakeDecode(const uint8_t ambe[DMR_VOCODER_AMBE_BYTES],
                       int16_t pcm[DMR_VOCODER_PCM_SAMPLES])
{
    for (size_t i = 0; i < BLOCK; i++)
        pcm[i] = rxPattern(ambe[0], i);

    return true;
}

/*
 * The fake microphone delivers, for block k, a square wave of amplitude
 * 500 * (k + 1) alternating at every sample; the DC block filter of the
 * encoder path passes it within a quarter of a percent, so the level of the
 * second half of the block gives k back.
 */
static const int TX_STEP = 500;

static bool fakeEncode(const int16_t pcm[DMR_VOCODER_PCM_SAMPLES],
                       uint8_t ambe[DMR_VOCODER_AMBE_BYTES])
{
    long sum = 0;
    for (size_t i = BLOCK / 2; i < BLOCK; i++)
        sum += (pcm[i] < 0) ? -pcm[i] : pcm[i];

    long level = sum / (long)(BLOCK / 2);
    memset(ambe, 0x00, AMBE);
    ambe[0] = (uint8_t)(((level + TX_STEP / 2) / TX_STEP) - 1);
    ambe[1] = 0xFA;
    return true;
}

static const struct dmrVocoder fakeVocoder = {
    "fake", fakeVocoderInit, fakeVocoderTerminate, fakeDecode, fakeEncode,
};

extern "C" const struct dmrVocoder *dmrVocoder_get(void)
{
    return &fakeVocoder;
}

/*
 * Fake audio driver paced by the test: every sync() waits for a tick.
 */
static pthread_mutex_t fakeMtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fakeCv = PTHREAD_COND_INITIALIZER;
static int ticks = 0;
static std::vector<int16_t> recorded; /* blocks written to the speaker  */
static int inBlocks = 0;              /* blocks delivered by the mic    */
static std::atomic<int> startCalls(0);
static std::atomic<int> stopCalls(0);
static std::atomic<int> haltCalls(0);
static std::atomic<int> syncWaits(0); /* sync() calls blocked on a tick */
static std::atomic<int> lateSyncs(0); /* next output sync()s answer -1  */

/* The idle half index lives in ctx->priv */
static int idleHalf(struct streamCtx *ctx)
{
    return (int)(intptr_t)ctx->priv;
}

static void setIdleHalf(struct streamCtx *ctx, int half)
{
    ctx->priv = (void *)(intptr_t)half;
}

/*
 * Wait for a tick, at most two seconds so that a broken test fails instead
 * of hanging. Returns false when the stream was stopped meanwhile.
 */
static bool waitTick(struct streamCtx *ctx)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 2;

    pthread_mutex_lock(&fakeMtx);
    syncWaits += 1;
    while ((ticks == 0) && (ctx->running != 0)) {
        if (pthread_cond_timedwait(&fakeCv, &fakeMtx, &deadline) == ETIMEDOUT)
            break;
    }

    bool ok = (ctx->running != 0) && (ticks > 0);
    if (ok)
        ticks -= 1;
    pthread_mutex_unlock(&fakeMtx);

    return ok;
}

static int fakeStart(const uint8_t instance, const void *config,
                     struct streamCtx *ctx)
{
    (void)instance;
    (void)config;

    setIdleHalf(ctx, 0);
    ctx->running = 1;
    startCalls += 1;
    return 0;
}

static int fakeData(struct streamCtx *ctx, stream_sample_t **buf)
{
    if (ctx->running == 0)
        return -1;

    *buf = ctx->buffer + (idleHalf(ctx) * (ctx->bufSize / 2));
    return ctx->bufSize / 2;
}

static int fakeOutSync(struct streamCtx *ctx, uint8_t dirty)
{
    if (ctx->running == 0)
        return -1;

    size_t half = ctx->bufSize / 2;
    if (dirty != 0) {
        stream_sample_t *data = ctx->buffer + (idleHalf(ctx) * half);
        pthread_mutex_lock(&fakeMtx);
        recorded.insert(recorded.end(), data, data + half);
        pthread_mutex_unlock(&fakeMtx);
        setIdleHalf(ctx, idleHalf(ctx) ^ 1);
    }

    /*
     * A late arrival, as the Cx000 DAC driver reports it: the sync point
     * had already passed, the block is taken and -1 returned at once.
     */
    if (lateSyncs.load() > 0) {
        lateSyncs -= 1;
        return -1;
    }

    return waitTick(ctx) ? 0 : -1;
}

static int fakeInSync(struct streamCtx *ctx, uint8_t dirty)
{
    (void)dirty;

    if (ctx->running == 0)
        return -1;

    if (waitTick(ctx) == false)
        return -1;

    /* A new block "acquired" into the other half */
    size_t half = ctx->bufSize / 2;
    setIdleHalf(ctx, idleHalf(ctx) ^ 1);
    stream_sample_t *data = ctx->buffer + (idleHalf(ctx) * half);

    pthread_mutex_lock(&fakeMtx);
    int k = inBlocks++;
    pthread_mutex_unlock(&fakeMtx);

    int16_t amp = (int16_t)(TX_STEP * (k + 1));
    for (size_t i = 0; i < half; i++)
        data[i] = ((i & 1) != 0) ? amp : (int16_t)-amp;

    return 0;
}

static void fakeStop(struct streamCtx *ctx)
{
    pthread_mutex_lock(&fakeMtx);
    ctx->running = 0;
    stopCalls += 1;
    pthread_cond_broadcast(&fakeCv);
    pthread_mutex_unlock(&fakeMtx);
}

static void fakeHalt(struct streamCtx *ctx)
{
    pthread_mutex_lock(&fakeMtx);
    ctx->running = 0;
    haltCalls += 1;
    pthread_cond_broadcast(&fakeCv);
    pthread_mutex_unlock(&fakeMtx);
}

static const struct audioDriver fakeOutDriver = { fakeStart, fakeData,
                                                  fakeOutSync, fakeStop,
                                                  fakeHalt };
static const struct audioDriver fakeInDriver = { fakeStart, fakeData,
                                                 fakeInSync, fakeStop,
                                                 fakeHalt };

/* SINK_RTX has no device, like the emulator's microphone */
const struct audioDevice outputDevices[] = {
    { &fakeOutDriver, NULL, 0, SINK_MCU },
    { NULL, NULL, 0, SINK_RTX },
    { &fakeOutDriver, NULL, 0, SINK_SPK },
};

const struct audioDevice inputDevices[] = {
    { &fakeInDriver, NULL, 0, SOURCE_MCU },
    { &fakeInDriver, NULL, 0, SOURCE_RTX },
    { &fakeInDriver, NULL, 0, SOURCE_MIC },
};

void audio_connect(const enum AudioSource source, const enum AudioSink sink)
{
    (void)source;
    (void)sink;
}

void audio_disconnect(const enum AudioSource source, const enum AudioSink sink)
{
    (void)source;
    (void)sink;
}

bool audio_checkPathCompatibility(const enum AudioSource p1Source,
                                  const enum AudioSink p1Sink,
                                  const enum AudioSource p2Source,
                                  const enum AudioSink p2Sink)
{
    /* Paths sharing the source or the sink are incompatible */
    if (p1Source == p2Source)
        return false;

    if (p1Sink == p2Sink)
        return false;

    return true;
}

/*
 * Test helpers
 */

static void grant(int n = 1)
{
    pthread_mutex_lock(&fakeMtx);
    ticks += n;
    pthread_cond_broadcast(&fakeCv);
    pthread_mutex_unlock(&fakeMtx);
}

static size_t recordedSamples()
{
    pthread_mutex_lock(&fakeMtx);
    size_t n = recorded.size();
    pthread_mutex_unlock(&fakeMtx);
    return n;
}

template <typename F>
static bool waitFor(F pred, int timeoutMs = 2000)
{
    for (int i = 0; i < timeoutMs; i++) {
        if (pred())
            return true;
        usleep(1000);
    }

    return pred();
}

/* Wait for the thread to be blocked in a sync() with no tick pending */
static bool waitParked(int expectedWaits)
{
    return waitFor([&]() {
        pthread_mutex_lock(&fakeMtx);
        bool parked = (syncWaits.load() >= expectedWaits) && (ticks == 0);
        pthread_mutex_unlock(&fakeMtx);
        return parked;
    });
}

/*
 * Stop the audio thread while it is parked in the fake sync(): the stop
 * request is only seen once sync() returns, as on a radio where the DMA
 * sync point comes round every 60 ms. Ticks are granted until the join is
 * over.
 */
static void stopAudio()
{
    struct Stopper {
        volatile bool done;
    } st = { false };

    pthread_t t;
    REQUIRE(pthread_create(
                &t, NULL,
                [](void *arg) -> void * {
                    dmrAudio_stop();
                    static_cast<Stopper *>(arg)->done = true;
                    return NULL;
                },
                &st)
            == 0);

    for (int i = 0; (i < 2000) && (st.done == false); i++) {
        grant();
        usleep(1000);
    }

    REQUIRE(st.done == true);
    pthread_join(t, NULL);
}

static void makeFrame(uint8_t index, uint8_t frame[DMR_VOCODER_AMBE_BYTES])
{
    memset(frame, (int)(0x40 + index), AMBE);
    frame[0] = index;
}

static bool blockIs(size_t block, uint8_t index)
{
    pthread_mutex_lock(&fakeMtx);
    bool ok = recorded.size() >= (block + 1) * BLOCK;
    for (size_t i = 0; ok && (i < BLOCK); i++)
        ok = (recorded[block * BLOCK + i] == rxPattern(index, i));
    pthread_mutex_unlock(&fakeMtx);
    return ok;
}

static bool blockIsSilence(size_t block)
{
    pthread_mutex_lock(&fakeMtx);
    bool ok = recorded.size() >= (block + 1) * BLOCK;
    for (size_t i = 0; ok && (i < BLOCK); i++)
        ok = (recorded[block * BLOCK + i] == 0);
    pthread_mutex_unlock(&fakeMtx);
    return ok;
}

/*
 * Bench: fresh fakes, the audio layer initialised, everything stopped and
 * released at the end.
 */
class Bench
{
public:
    pathId path;
    struct dmrAudioStats stats;

    Bench() : path(-1)
    {
        pthread_mutex_lock(&fakeMtx);
        ticks = 0;
        recorded.clear();
        inBlocks = 0;
        pthread_mutex_unlock(&fakeMtx);
        startCalls.store(0);
        stopCalls.store(0);
        haltCalls.store(0);
        syncWaits.store(0);
        lateSyncs.store(0);
        rejectedJoins.store(0);
        memset(&stats, 0x00, sizeof(stats));
        dmrAudio_init();
    }

    ~Bench()
    {
        if (dmrAudio_running())
            stopAudio();
        dmrAudio_stop();
        audioPath_release(path);
        dmrAudio_terminate();
    }

    void refresh()
    {
        dmrAudio_getStats(&stats);
    }

    /* Open the speaker path and start playback: the thread parks in the
     * initial sync() of the stream */
    void startRx()
    {
        path = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_RX);
        REQUIRE(path > 0);
        REQUIRE(dmrAudio_startRx(path) == true);
        REQUIRE(dmrAudio_running() == true);
        REQUIRE(waitParked(1));
    }

    /* Open the microphone path and start encoding */
    void startTx()
    {
        path = audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX);
        REQUIRE(path > 0);
        REQUIRE(dmrAudio_startTx(path) == true);
        REQUIRE(dmrAudio_running() == true);
        REQUIRE(waitParked(1));
    }

    /* One 60 ms period of the speaker: a block is consumed */
    void playBlock(size_t expectBlocks)
    {
        int waits = syncWaits.load();
        grant();
        REQUIRE(waitFor(
            [&]() { return recordedSamples() >= expectBlocks * BLOCK; }));
        REQUIRE(waitParked(waits + 1));
    }
};

TEST_CASE("dmr_audio: init and terminate drive the vocoder backend once",
          "[dmr][audio]")
{
    int inits = vocoderInits.load();
    int terms = vocoderTerminates.load();

    dmrAudio_init();
    REQUIRE(vocoderInits.load() == inits + 1);
    REQUIRE(strcmp(dmrAudio_backendName(), "fake") == 0);

    /* Nested init/terminate pairs do not touch the backend */
    dmrAudio_init();
    dmrAudio_terminate();
    REQUIRE(vocoderInits.load() == inits + 1);
    REQUIRE(vocoderTerminates.load() == terms);

    dmrAudio_terminate();
    REQUIRE(vocoderTerminates.load() == terms + 1);

    /* Nothing runs, nothing is accepted */
    uint8_t frame[AMBE];
    REQUIRE(dmrAudio_running() == false);
    REQUIRE(dmrAudio_pushFrame(frame) == false);
    REQUIRE(dmrAudio_popFrame(frame) == false);
}

TEST_CASE("dmr_audio: startRx plays ten payloads in order, 480 samples each",
          "[dmr][audio]")
{
    Bench b;
    b.startRx();
    REQUIRE(startCalls.load() == 1);
    REQUIRE(audioPath_getStatus(b.path) == PATH_OPEN);

    uint8_t frame[AMBE];
    for (uint8_t i = 0; i < 10; i++) {
        makeFrame(i, frame);
        REQUIRE(dmrAudio_pushFrame(frame) == true);
        b.playBlock(i + 1);
        REQUIRE(blockIs(i, i));
    }

    REQUIRE(recordedSamples() == 10 * BLOCK);
    b.refresh();
    REQUIRE(b.stats.framesRx == 10);
    REQUIRE(b.stats.blocksOut == 10);
    REQUIRE(b.stats.underruns == 0);
    REQUIRE(b.stats.overruns == 0);
    REQUIRE(b.stats.varying == 10);
    REQUIRE(b.stats.framesTx == 0);

    /* A payload identical to the previous one is not "varying" */
    REQUIRE(dmrAudio_pushFrame(frame) == true);
    b.playBlock(11);
    b.refresh();
    REQUIRE(b.stats.framesRx == 11);
    REQUIRE(b.stats.varying == 10);

    /* A pop is refused while playing */
    REQUIRE(dmrAudio_popFrame(frame) == false);
}

TEST_CASE("dmr_audio: a missing payload is played as silence and counted",
          "[dmr][audio]")
{
    Bench b;
    b.startRx();

    uint8_t frame[AMBE];
    makeFrame(1, frame);
    REQUIRE(dmrAudio_pushFrame(frame) == true);
    b.playBlock(1);
    REQUIRE(blockIs(0, 1));

    /* No payload for the next period: 480 zeros go out */
    b.playBlock(2);
    REQUIRE(blockIsSilence(1));
    b.refresh();
    REQUIRE(b.stats.blocksOut == 2);
    REQUIRE(b.stats.underruns == 0); /* counted once voice resumes */

    makeFrame(2, frame);
    REQUIRE(dmrAudio_pushFrame(frame) == true);
    b.playBlock(3);
    REQUIRE(blockIs(2, 2));
    b.refresh();
    REQUIRE(b.stats.blocksOut == 3);
    REQUIRE(b.stats.underruns == 1);
    REQUIRE(b.stats.framesRx == 2);

    /* Silence at the end of the call is not an underrun */
    b.playBlock(4);
    b.playBlock(5);
    REQUIRE(blockIsSilence(3));
    REQUIRE(blockIsSilence(4));
    b.refresh();
    REQUIRE(b.stats.underruns == 1);
}

TEST_CASE("dmr_audio: a late sync point is a glitch, not the end of "
          "playback",
          "[dmr][audio]")
{
    Bench b;
    b.startRx();

    uint8_t frame[AMBE];
    makeFrame(1, frame);
    REQUIRE(dmrAudio_pushFrame(frame) == true);
    b.playBlock(1);
    REQUIRE(blockIs(0, 1));

    /*
     * The driver answers -1 to the sync() after the next block, the way
     * the Cx000 DAC does when the thread comes one period late: the thread
     * goes straight on with the following block (silence, no payload) and
     * parks in the sync() after it, no stream stopped, no restart needed.
     */
    lateSyncs.store(1);
    makeFrame(2, frame);
    REQUIRE(dmrAudio_pushFrame(frame) == true);
    int waits = syncWaits.load();
    grant();
    REQUIRE(waitFor([&]() { return recordedSamples() >= 3 * BLOCK; }));
    REQUIRE(waitParked(waits + 1));
    REQUIRE(lateSyncs.load() == 0);
    REQUIRE(blockIs(1, 2));
    REQUIRE(blockIsSilence(2));
    REQUIRE(dmrAudio_running() == true);
    REQUIRE(stopCalls.load() == 0);
    REQUIRE(startCalls.load() == 1);

    /* Playback goes on as before */
    makeFrame(3, frame);
    REQUIRE(dmrAudio_pushFrame(frame) == true);
    b.playBlock(4);
    REQUIRE(blockIs(3, 3));
    b.refresh();
    REQUIRE(b.stats.blocksOut == 4);
    REQUIRE(b.stats.framesRx == 3);
    REQUIRE(b.stats.underruns == 1);

    /* A stopped stream still ends the thread */
    stopAudio();
    REQUIRE(dmrAudio_running() == false);
    REQUIRE(stopCalls.load() == 1);
}

TEST_CASE("dmr_audio: silence before the first payload is not an underrun",
          "[dmr][audio]")
{
    Bench b;
    b.startRx();

    /* The call opened on the header, voice arrives two periods later */
    b.playBlock(1);
    b.playBlock(2);
    REQUIRE(blockIsSilence(0));
    REQUIRE(blockIsSilence(1));

    uint8_t frame[AMBE];
    makeFrame(7, frame);
    REQUIRE(dmrAudio_pushFrame(frame) == true);
    b.playBlock(3);
    REQUIRE(blockIs(2, 7));
    b.refresh();
    REQUIRE(b.stats.underruns == 0);
    REQUIRE(b.stats.blocksOut == 3);
}

TEST_CASE("dmr_audio: a full queue drops the oldest payload and counts it",
          "[dmr][audio]")
{
    Bench b;
    b.startRx();

    uint8_t frame[AMBE];
    for (uint8_t i = 0; i < DMR_AUDIO_QUEUE_DEPTH + 1; i++) {
        makeFrame(i, frame);
        REQUIRE(dmrAudio_pushFrame(frame) == true);
    }

    b.refresh();
    REQUIRE(b.stats.framesRx == DMR_AUDIO_QUEUE_DEPTH + 1);
    REQUIRE(b.stats.overruns == 1);

    /* Payload 0 is gone, 1..3 are played in order */
    for (size_t i = 0; i < DMR_AUDIO_QUEUE_DEPTH; i++) {
        b.playBlock(i + 1);
        REQUIRE(blockIs(i, (uint8_t)(i + 1)));
    }

    b.refresh();
    REQUIRE(b.stats.underruns == 0);
    REQUIRE(b.stats.blocksOut == DMR_AUDIO_QUEUE_DEPTH);
}

TEST_CASE("dmr_audio: stop releases the stream and joins the thread",
          "[dmr][audio]")
{
    Bench b;
    b.startRx();

    uint8_t frame[AMBE];
    makeFrame(3, frame);
    REQUIRE(dmrAudio_pushFrame(frame) == true);
    b.playBlock(1);

    stopAudio();
    REQUIRE(dmrAudio_running() == false);
    REQUIRE(stopCalls.load() == 1);
    REQUIRE(rejectedJoins.load() == 0);
    REQUIRE(audioPath_getStatus(b.path) == PATH_OPEN);

    /* Nothing is accepted any more, the stream slot is free again */
    REQUIRE(dmrAudio_pushFrame(frame) == false);
    stream_sample_t buf[16];
    streamId s = audioStream_start(b.path, buf, 16, 8000,
                                   STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    REQUIRE(s >= 0);
    audioStream_terminate(s);

    /* A second stop is a no-op */
    dmrAudio_stop();
    REQUIRE(stopCalls.load() == 1);
}

TEST_CASE("dmr_audio: a path suspended by a prompt stops playback, a restart "
          "resumes it",
          "[dmr][audio]")
{
    Bench b;
    b.startRx();

    uint8_t frame[AMBE];
    makeFrame(1, frame);
    REQUIRE(dmrAudio_pushFrame(frame) == true);
    b.playBlock(1);

    /* A voice prompt takes the speaker: the RX path is suspended */
    pathId prompt = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_PROMPT);
    REQUIRE(prompt > 0);
    REQUIRE(audioPath_getStatus(b.path) == PATH_SUSPENDED);

    /* The thread finds out at its next period and quits on its own */
    grant();
    REQUIRE(waitFor([]() { return dmrAudio_running() == false; }));
    REQUIRE(stopCalls.load() + haltCalls.load() == 1);
    REQUIRE(dmrAudio_pushFrame(frame) == false);

    /* Not restartable while suspended */
    REQUIRE(dmrAudio_startRx(b.path) == false);

    /* Prompt over: the path is open again and playback restarts */
    audioPath_release(prompt);
    REQUIRE(audioPath_getStatus(b.path) == PATH_OPEN);
    int waits = syncWaits.load();
    REQUIRE(dmrAudio_startRx(b.path) == true);
    REQUIRE(dmrAudio_running() == true);
    REQUIRE(waitParked(waits + 1));
    REQUIRE(startCalls.load() == 2);
    REQUIRE(rejectedJoins.load() == 0);

    makeFrame(2, frame);
    REQUIRE(dmrAudio_pushFrame(frame) == true);
    b.playBlock(2);
    REQUIRE(blockIs(1, 2));
}

TEST_CASE("dmr_audio: startTx pulls 480-sample blocks and popFrame returns "
          "the encoded payloads",
          "[dmr][audio]")
{
    Bench b;
    b.startTx();
    REQUIRE(startCalls.load() == 1);

    uint8_t frame[AMBE];
    memset(frame, 0xEE, AMBE);
    REQUIRE(dmrAudio_popFrame(frame) == false);
    REQUIRE(frame[0] == 0xEE);
    REQUIRE(dmrAudio_pushFrame(frame) == false);

    /* One block from the microphone: one payload */
    int waits = syncWaits.load();
    grant();
    REQUIRE(waitFor([&]() {
        struct dmrAudioStats s;
        dmrAudio_getStats(&s);
        return s.framesTx == 1;
    }));
    REQUIRE(waitParked(waits + 1));
    REQUIRE(dmrAudio_popFrame(frame) == true);
    REQUIRE(frame[0] == 0);
    REQUIRE(frame[1] == 0xFA);
    REQUIRE(dmrAudio_popFrame(frame) == false);

    /* Four blocks without a pop: the oldest payload is dropped */
    for (int i = 0; i < DMR_AUDIO_QUEUE_DEPTH + 1; i++) {
        waits = syncWaits.load();
        grant();
        REQUIRE(waitParked(waits + 1));
    }

    b.refresh();
    REQUIRE(b.stats.framesTx == DMR_AUDIO_QUEUE_DEPTH + 2);
    REQUIRE(b.stats.overruns == 1);
    REQUIRE(b.stats.framesRx == 0);
    REQUIRE(b.stats.blocksOut == 0);

    for (int i = 0; i < DMR_AUDIO_QUEUE_DEPTH; i++) {
        REQUIRE(dmrAudio_popFrame(frame) == true);
        REQUIRE(frame[0] == 2 + i);
    }
    REQUIRE(dmrAudio_popFrame(frame) == false);

    stopAudio();
    REQUIRE(dmrAudio_running() == false);
    REQUIRE(haltCalls.load() == 1); /* input streams are terminated */
}

TEST_CASE("dmr_audio: flushTx keeps the newest payload only", "[dmr][audio]")
{
    Bench b;
    uint8_t frame[AMBE];

    /* Nothing running: a no-op */
    dmrAudio_flushTx();
    REQUIRE(dmrAudio_popFrame(frame) == false);

    b.startTx();

    /* Three blocks fill the queue, one more drops the oldest */
    for (int i = 0; i < DMR_AUDIO_QUEUE_DEPTH + 1; i++) {
        int waits = syncWaits.load();
        grant();
        REQUIRE(waitParked(waits + 1));
    }
    b.refresh();
    REQUIRE(b.stats.framesTx == DMR_AUDIO_QUEUE_DEPTH + 1);
    REQUIRE(b.stats.overruns == 1);

    /* The voice bursts begin: only block 3, the newest, is left */
    dmrAudio_flushTx();
    REQUIRE(dmrAudio_popFrame(frame) == true);
    REQUIRE(frame[0] == DMR_AUDIO_QUEUE_DEPTH);
    REQUIRE(dmrAudio_popFrame(frame) == false);
    b.refresh();
    REQUIRE(b.stats.overruns == 1); /* the flush is not an overrun */

    /* Encoding goes on: the next block is the next payload */
    int waits = syncWaits.load();
    grant();
    REQUIRE(waitParked(waits + 1));
    REQUIRE(dmrAudio_popFrame(frame) == true);
    REQUIRE(frame[0] == DMR_AUDIO_QUEUE_DEPTH + 1);

    /* A single queued payload survives a flush */
    waits = syncWaits.load();
    grant();
    REQUIRE(waitParked(waits + 1));
    dmrAudio_flushTx();
    REQUIRE(dmrAudio_popFrame(frame) == true);
    REQUIRE(frame[0] == DMR_AUDIO_QUEUE_DEPTH + 2);
    REQUIRE(dmrAudio_popFrame(frame) == false);

    stopAudio();
    REQUIRE(dmrAudio_running() == false);
}

TEST_CASE("dmr_audio: flushTx leaves the receive queue alone", "[dmr][audio]")
{
    Bench b;
    b.startRx();

    uint8_t frame[AMBE];
    makeFrame(1, frame);
    REQUIRE(dmrAudio_pushFrame(frame) == true);
    makeFrame(2, frame);
    REQUIRE(dmrAudio_pushFrame(frame) == true);

    dmrAudio_flushTx();
    b.playBlock(1);
    b.playBlock(2);
    REQUIRE(blockIs(0, 1));
    REQUIRE(blockIs(1, 2));
}

TEST_CASE("dmr_audio: a start on a path with no device fails at once",
          "[dmr][audio]")
{
    Bench b;

    /* A closed path */
    REQUIRE(dmrAudio_startRx(-1) == false);
    REQUIRE(dmrAudio_startTx(-1) == false);

    /* An open path whose sink has no device: no thread is left behind */
    b.path = audioPath_request(SOURCE_MCU, SINK_RTX, PRIO_TX);
    REQUIRE(b.path > 0);
    REQUIRE(dmrAudio_startRx(b.path) == false);
    REQUIRE(dmrAudio_running() == false);
    REQUIRE(startCalls.load() == 0);

    uint8_t frame[AMBE];
    REQUIRE(dmrAudio_pushFrame(frame) == false);
    dmrAudio_stop();
    REQUIRE(stopCalls.load() + haltCalls.load() == 0);
}

TEST_CASE("dmr_audio: the transmit path takes over the receive one, not the "
          "other way round",
          "[dmr][audio]")
{
    Bench b;
    b.startRx();

    /* The microphone path outranks the speaker one: RX is stopped */
    pathId mic = audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX);
    REQUIRE(mic > 0);

    struct Starter {
        pathId path;
        volatile bool done;
        volatile bool result;
    } st = { mic, false, false };

    pthread_t t;
    REQUIRE(pthread_create(
                &t, NULL,
                [](void *arg) -> void * {
                    Starter *s = static_cast<Starter *>(arg);
                    s->result = dmrAudio_startTx(s->path);
                    s->done = true;
                    return NULL;
                },
                &st)
            == 0);

    for (int i = 0; (i < 2000) && (st.done == false); i++) {
        grant();
        usleep(1000);
    }
    REQUIRE(st.done == true);
    pthread_join(t, NULL);
    REQUIRE(st.result == true);
    REQUIRE(dmrAudio_running() == true);
    REQUIRE(stopCalls.load() == 1);

    /* While the microphone path is open, the speaker one is refused */
    REQUIRE(dmrAudio_startRx(b.path) == false);
    REQUIRE(dmrAudio_running() == true);

    stopAudio();
    audioPath_release(mic);
}

TEST_CASE("dmr_audio: two stops at once stop the thread exactly once",
          "[dmr][audio]")
{
    Bench b;
    b.startRx();

    struct Stopper {
        volatile bool done;
    };

    Stopper s1 = { false };
    Stopper s2 = { false };
    auto stopFunc = [](void *arg) -> void * {
        dmrAudio_stop();
        static_cast<Stopper *>(arg)->done = true;
        return NULL;
    };

    pthread_t t1, t2;
    REQUIRE(pthread_create(&t1, NULL, stopFunc, &s1) == 0);
    REQUIRE(pthread_create(&t2, NULL, stopFunc, &s2) == 0);

    for (int i = 0; (i < 2000) && !(s1.done && s2.done); i++) {
        grant();
        usleep(1000);
    }
    REQUIRE(s1.done == true);
    REQUIRE(s2.done == true);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    REQUIRE(rejectedJoins.load() == 0);
    REQUIRE(dmrAudio_running() == false);
    REQUIRE(stopCalls.load() == 1);
}

TEST_CASE("dmr_audio: start and stop a hundred times", "[dmr][audio]")
{
    Bench b;
    uint8_t frame[AMBE];

    for (int round = 0; round < 100; round++) {
        int waits = syncWaits.load();

        if ((round % 2) == 0) {
            b.path = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_RX);
            REQUIRE(b.path > 0);
            REQUIRE(dmrAudio_startRx(b.path) == true);
            makeFrame((uint8_t)round, frame);
            REQUIRE(dmrAudio_pushFrame(frame) == true);
        } else {
            b.path = audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX);
            REQUIRE(b.path > 0);
            REQUIRE(dmrAudio_startTx(b.path) == true);
        }

        REQUIRE(dmrAudio_running() == true);

        /* Every other round the thread is stopped while parked, the others
         * right away, before it even reached the stream */
        if ((round % 4) < 2)
            REQUIRE(waitParked(waits + 1));

        stopAudio();
        REQUIRE(dmrAudio_running() == false);
        audioPath_release(b.path);
        b.path = -1;
    }

    REQUIRE(rejectedJoins.load() == 0);
    REQUIRE(startCalls.load() == 100);
    REQUIRE(stopCalls.load() + haltCalls.load() == 100);
    b.refresh();
    REQUIRE(b.stats.framesRx == 50);
}
