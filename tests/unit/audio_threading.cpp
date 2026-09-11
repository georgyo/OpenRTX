/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host tests for the thread safety of the audio codec and audio path modules.
 * The audio device is a fake driver whose sync() takes 100ms, mimicking the
 * DAC/DMA sync point wait of the real drivers, and pthread_join() is wrapped
 * (-Wl,--wrap=pthread_join) to reproduce the Miosix Thread::join() semantics
 * where a second concurrent join on the same thread fails at once with EINVAL
 * instead of blocking.
 */

#include <catch2/catch_test_macros.hpp>
#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <atomic>
#include "interfaces/audio.h"
#include "core/audio_path.h"
#include "core/audio_stream.h"
#include "core/audio_codec.h"

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
 * Fake audio driver and audio devices
 */
static int fakeStart(const uint8_t instance, const void *config,
                     struct streamCtx *ctx)
{
    (void)instance;
    (void)config;

    ctx->running = 1;
    return 0;
}

static int fakeData(struct streamCtx *ctx, stream_sample_t **buf)
{
    *buf = ctx->buffer;
    return ctx->bufSize / 2;
}

static int fakeSync(struct streamCtx *ctx, uint8_t dirty)
{
    (void)dirty;

    if (ctx->running == 0)
        return -1;

    usleep(100000);
    return 0;
}

static void fakeStop(struct streamCtx *ctx)
{
    ctx->running = 0;
}

static void fakeHalt(struct streamCtx *ctx)
{
    ctx->running = 0;
}

static const struct audioDriver fakeDriver = { fakeStart, fakeData, fakeSync,
                                               fakeStop, fakeHalt };

const struct audioDevice outputDevices[] = {
    { &fakeDriver, NULL, 0, SINK_MCU },
    { &fakeDriver, NULL, 0, SINK_RTX },
    { &fakeDriver, NULL, 0, SINK_SPK },
};

const struct audioDevice inputDevices[] = {
    { &fakeDriver, NULL, 0, SOURCE_MCU },
    { &fakeDriver, NULL, 0, SOURCE_RTX },
    { &fakeDriver, NULL, 0, SOURCE_MIC },
};

static std::atomic<int> connectCalls(0);
static std::atomic<int> disconnectCalls(0);

void audio_connect(const enum AudioSource source, const enum AudioSink sink)
{
    (void)source;
    (void)sink;

    connectCalls += 1;
}

void audio_disconnect(const enum AudioSource source, const enum AudioSink sink)
{
    (void)source;
    (void)sink;

    disconnectCalls += 1;
}

bool audio_checkPathCompatibility(const enum AudioSource p1Source,
                                  const enum AudioSink p1Sink,
                                  const enum AudioSource p2Source,
                                  const enum AudioSink p2Sink)
{
    // Paths sharing the source or the sink are incompatible
    if (p1Source == p2Source)
        return false;

    if (p1Sink == p2Sink)
        return false;

    return true;
}

/*
 * Wait until a flag is set, at most timeoutMs. Returns the flag value.
 */
static bool waitFlag(volatile bool *flag, int timeoutMs)
{
    for (int i = 0; (i < timeoutMs) && (*flag == false); i++)
        usleep(1000);

    return *flag;
}

/*
 * Voice prompt playing, PTT pressed: the UI thread stops the prompt decoder
 * (vp_tick -> vp_stop -> codec_stop) while the rtx thread starts the encoder
 * on a higher priority path (OpMode_M17::txState -> codec_startEncode).
 */
struct PttRace {
    pathId vpPath;
    pathId txPath;
    volatile bool uiDone;
    volatile bool rtxDone;
    volatile bool encodeStarted;
};

static void *uiThread(void *arg)
{
    PttRace *race = static_cast<PttRace *>(arg);

    codec_stop(race->vpPath);
    race->uiDone = true;
    return NULL;
}

static void *rtxThread(void *arg)
{
    PttRace *race = static_cast<PttRace *>(arg);

    usleep(20000);
    race->txPath = audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX);
    race->encodeStarted = codec_startEncode(race->txPath);
    race->rtxDone = true;
    return NULL;
}

TEST_CASE("codec_stop() racing codec_startEncode() does not hang the caller",
          "[audio][codec]")
{
    PttRace race;
    memset(&race, 0, sizeof(race));
    rejectedJoins.store(0);

    codec_init();
    race.vpPath = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_PROMPT);
    REQUIRE(race.vpPath > 0);
    REQUIRE(codec_startDecode(race.vpPath) == true);
    usleep(30000); // Decoder thread is now blocked in the driver sync()

    pthread_t ui, rtx;
    REQUIRE(pthread_create(&ui, NULL, uiThread, &race) == 0);
    REQUIRE(pthread_create(&rtx, NULL, rtxThread, &race) == 0);

    // Two sync periods are enough for the decoder to stop
    REQUIRE(waitFlag(&race.uiDone, 3000) == true);
    REQUIRE(waitFlag(&race.rtxDone, 1000) == true);
    pthread_join(ui, NULL);
    pthread_join(rtx, NULL);

    // Stops are serialised: no join has been rejected and the encoder took
    // over once the decoder was really stopped.
    REQUIRE(rejectedJoins.load() == 0);
    REQUIRE(race.encodeStarted == true);
    REQUIRE(codec_running() == true);

    codec_stop(race.txPath);
    REQUIRE(codec_running() == false);

    audioPath_release(race.txPath);
    audioPath_release(race.vpPath);
    codec_terminate();
}

TEST_CASE("codec_stop() from two threads at once stops the codec exactly once",
          "[audio][codec]")
{
    rejectedJoins.store(0);

    codec_init();
    pathId path = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_PROMPT);
    REQUIRE(path > 0);
    REQUIRE(codec_startDecode(path) == true);
    usleep(30000);

    struct Stopper {
        pathId path;
        volatile bool done;
    };

    Stopper s1 = { path, false };
    Stopper s2 = { path, false };
    auto stopFunc = [](void *arg) -> void * {
        Stopper *s = static_cast<Stopper *>(arg);
        codec_stop(s->path);
        s->done = true;
        return NULL;
    };

    pthread_t t1, t2;
    REQUIRE(pthread_create(&t1, NULL, stopFunc, &s1) == 0);
    REQUIRE(pthread_create(&t2, NULL, stopFunc, &s2) == 0);

    REQUIRE(waitFlag(&s1.done, 3000) == true);
    REQUIRE(waitFlag(&s2.done, 3000) == true);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    REQUIRE(rejectedJoins.load() == 0);
    REQUIRE(codec_running() == false);

    audioPath_release(path);
    codec_terminate();
}

/*
 * Audio path tables hammered from three threads: rtx thread opening and
 * closing the RX path on squelch, UI thread requesting the voice prompt path
 * which suspends the RX one, codec thread polling the path status.
 */
struct PathStress {
    volatile bool stop;
    volatile int rounds;
    volatile int lastId;  // Highest path ID allocated so far
    volatile bool failed; // Prompt path request refused
};

static void *rxPathThread(void *arg)
{
    PathStress *st = static_cast<PathStress *>(arg);

    while (st->stop == false) {
        pathId id = audioPath_request(SOURCE_RTX, SINK_SPK, PRIO_RX);
        if (id > st->lastId)
            st->lastId = id;

        audioPath_getStatus(id);
        usleep(50);
        audioPath_release(id);
        st->rounds += 1;
    }

    return NULL;
}

static void *promptPathThread(void *arg)
{
    PathStress *st = static_cast<PathStress *>(arg);

    while (st->stop == false) {
        pathId id = audioPath_request(SOURCE_MCU, SINK_SPK, PRIO_PROMPT);

        // Higher priority than the RX path: never refused
        if (id <= 0)
            st->failed = true;

        if (id > st->lastId)
            st->lastId = id;

        audioPath_getInfo(id);
        usleep(50);
        audioPath_release(id);
    }

    return NULL;
}

static void *pollPathThread(void *arg)
{
    PathStress *st = static_cast<PathStress *>(arg);

    while (st->stop == false) {
        pathId last = st->lastId;
        for (pathId id = last - 4; id <= last + 4; id++)
            audioPath_getStatus(id);
    }

    return NULL;
}

TEST_CASE("audio path tables survive concurrent request/release/getStatus",
          "[audio][path]")
{
    PathStress st = { false, 0, 0, false };
    connectCalls.store(0);
    disconnectCalls.store(0);

    // Path kept open for the whole test: the RX path is suspended by the
    // prompt path and resumed on its release.
    pathId txPath = audioPath_request(SOURCE_MIC, SINK_MCU, PRIO_TX);
    REQUIRE(txPath > 0);

    pthread_t rx, prompt, poll;
    REQUIRE(pthread_create(&rx, NULL, rxPathThread, &st) == 0);
    REQUIRE(pthread_create(&prompt, NULL, promptPathThread, &st) == 0);
    REQUIRE(pthread_create(&poll, NULL, pollPathThread, &st) == 0);

    usleep(300000);
    st.stop = true;
    pthread_join(rx, NULL);
    pthread_join(prompt, NULL);
    pthread_join(poll, NULL);

    REQUIRE(st.rounds > 0);
    REQUIRE(st.failed == false);
    REQUIRE(audioPath_getStatus(txPath) == PATH_OPEN);
    audioPath_release(txPath);

    // Every path released: the tables are empty and every hardware path
    // opened has been closed again.
    for (pathId id = 1; id <= st.lastId; id++)
        REQUIRE(audioPath_getStatus(id) == PATH_CLOSED);

    REQUIRE(connectCalls.load() == disconnectCalls.load());
}
