/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host tests for the HR_Cx000 DAC output stream driver, built against the
 * fake HR_C6000 in tests/unit/fake. They cover the thread synchronisation of
 * the driver: a thread blocked in sync() must be released when the stream is
 * halted from another thread or when the DAC FIFO stops draining, and a
 * stream taking over a beep must start from the beginning of its buffer.
 */

#include <catch2/catch_test_macros.hpp>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "drivers/baseband/HR_C6000.h"
#include "Cx000_dac.h"

#ifndef FAKE_HR_C6000
#error "The fake HR_C6000 header must shadow the real one in this test"
#endif

struct FakeC6000State fakeC6000;

static HR_C6000 dev;
static struct streamCtx ctx;
static int16_t buf[320];

static const int STALL_TICKS = 25; // Same as DAC_STALL_TICKS in the driver

struct Waiter {
    pthread_t thread;
    volatile int result;
    volatile bool started;
    volatile bool done;
};

static void *waiterFunc(void *arg)
{
    Waiter *w = static_cast<Waiter *>(arg);
    w->started = true;
    w->result = Cx000_dac_audio_driver.sync(&ctx, 0);
    w->done = true;
    return NULL;
}

/*
 * Start a thread calling sync() and wait until it is (very likely) blocked
 * on the condition variable: the thread signals when it is about to enter
 * sync(), then it is given a generous margin. A fixed sleep alone is not
 * enough on a loaded machine running the sanitizer builds.
 */
static void startWaiter(Waiter *w)
{
    w->result = 12345;
    w->started = false;
    w->done = false;
    REQUIRE(pthread_create(&w->thread, NULL, waiterFunc, w) == 0);

    for (int i = 0; (i < 5000) && (w->started == false); i++)
        usleep(1000);

    REQUIRE(w->started == true);
    usleep(100000);
}

/*
 * Wait until the waiter thread returned from sync(), at most timeoutMs.
 * Returns true if the thread returned.
 */
static bool waiterReturned(Waiter *w, int timeoutMs)
{
    for (int i = 0; i < timeoutMs; i++) {
        if (w->done) {
            pthread_join(w->thread, NULL);
            return true;
        }

        usleep(1000);
    }

    return false;
}

static void setupStream()
{
    // A failed test case may have left a stream running: stop it so that
    // one failure does not cascade into every following case.
    static bool first = true;
    if (first == false) {
        Cx000_dac_audio_driver.terminate(&ctx);
        Cx000dac_terminate();
    }
    first = false;

    memset(&fakeC6000, 0, sizeof(fakeC6000));
    memset(&ctx, 0, sizeof(ctx));
    ctx.buffer = buf;
    ctx.bufSize = 320;
    ctx.bufMode = BUF_CIRC_DOUBLE;
    ctx.sampleRate = 8000;
}

TEST_CASE("Cx000 DAC: sync() wakes up at the half-buffer sync point",
          "[audio][cx000dac]")
{
    setupStream();
    Cx000dac_init(&dev);
    REQUIRE(Cx000_dac_audio_driver.start(0, NULL, &ctx) == 0);
    REQUIRE(ctx.running == 1);

    Waiter w;
    startWaiter(&w);

    // Five chunks of 32 samples cross the half of a 320 samples buffer
    for (int i = 0; i < 5; i++)
        Cx000dac_task();

    REQUIRE(waiterReturned(&w, 2000));
    REQUIRE(w.result == 0);
    REQUIRE(fakeC6000.audioChunks == 5);
    REQUIRE(fakeC6000.lastAudio == (const uint8_t *)&buf[128]);

    Cx000_dac_audio_driver.terminate(&ctx);
    Cx000dac_terminate();
}

TEST_CASE("Cx000 DAC: halting the stream releases a thread blocked in sync()",
          "[audio][cx000dac]")
{
    setupStream();
    Cx000dac_init(&dev);
    REQUIRE(Cx000_dac_audio_driver.start(0, NULL, &ctx) == 0);

    Waiter w;
    startWaiter(&w);

    // Stream terminated from another thread, as done by the audio stream
    // module when the path of the stream gets suspended or closed.
    Cx000_dac_audio_driver.terminate(&ctx);
    REQUIRE(ctx.running == 0);

    // The stream is off: the task function has no reason to signal anything
    for (int i = 0; i < 20; i++)
        Cx000dac_task();

    REQUIRE(waiterReturned(&w, 2000));
    REQUIRE(fakeC6000.audioChunks == 0);

    // A new sync() call on the halted stream returns immediately
    REQUIRE(Cx000_dac_audio_driver.sync(&ctx, 0) == -1);

    Cx000dac_terminate();
}

TEST_CASE("Cx000 DAC: a stop completes even if the DAC FIFO never drains",
          "[audio][cx000dac]")
{
    setupStream();
    Cx000dac_init(&dev);
    REQUIRE(Cx000_dac_audio_driver.start(0, NULL, &ctx) == 0);

    // HR_C6000 reports the FIFO as never empty, e.g. because it has been
    // reconfigured for transmission while the stream was playing.
    fakeC6000.fifoNotEmpty = true;

    Waiter w;
    startWaiter(&w);

    // What audioStream_stop() does before calling sync()
    Cx000_dac_audio_driver.stop(&ctx);

    // Sync points must keep coming after the stall timeout: one every five
    // chunks of 32 samples.
    for (int i = 0; i < STALL_TICKS + 5; i++)
        Cx000dac_task();

    REQUIRE(waiterReturned(&w, 2000));
    REQUIRE(w.result == 0);
    REQUIRE(fakeC6000.audioChunks == 5);

    // The stop request has been honoured at the sync point
    REQUIRE(ctx.running == 0);

    Cx000dac_terminate();
}

TEST_CASE("Cx000 DAC: a stalled FIFO which recovers resumes normal operation",
          "[audio][cx000dac]")
{
    setupStream();
    Cx000dac_init(&dev);
    REQUIRE(Cx000_dac_audio_driver.start(0, NULL, &ctx) == 0);

    // A FIFO which is not empty for less than the stall timeout just delays
    // the next chunk.
    fakeC6000.fifoNotEmpty = true;
    for (int i = 0; i < STALL_TICKS - 1; i++)
        Cx000dac_task();

    REQUIRE(fakeC6000.audioChunks == 0);

    fakeC6000.fifoNotEmpty = false;
    Cx000dac_task();
    REQUIRE(fakeC6000.audioChunks == 1);
    REQUIRE(fakeC6000.lastAudio == (const uint8_t *)&buf[0]);

    Cx000_dac_audio_driver.terminate(&ctx);
    Cx000dac_terminate();
}

/*
 * Emulation of the real-time audio thread preempting Cx000dac_start() while
 * the latter is switching on the "OpenMusic" mode: the hook runs the driver
 * task in a separate thread and gives it some time to complete. If the driver
 * correctly holds its lock across the mode switch, the task blocks and
 * completes only after start() returned.
 */
static pthread_t preemptThread;
static volatile bool preemptArmed = false;
static volatile bool preemptDone = false;

static void *preemptFunc(void *arg)
{
    (void)arg;
    Cx000dac_task();
    preemptDone = true;
    return NULL;
}

static void preemptOnOpenMusic(uint8_t reg, uint8_t value)
{
    if ((preemptArmed == false) || (reg != 0x06) || (value != 0x22))
        return;

    preemptArmed = false;
    preemptDone = false;
    pthread_create(&preemptThread, NULL, preemptFunc, NULL);

    for (int i = 0; (i < 50) && (preemptDone == false); i++)
        usleep(1000);
}

TEST_CASE("Cx000 DAC: a stream taking over a beep starts from its beginning",
          "[audio][cx000dac]")
{
    setupStream();
    Cx000dac_init(&dev);

    // Beep playing, phase accumulator advanced by a few task() calls
    REQUIRE(Cx000dac_startBeep(1000) == 0);
    for (int i = 0; i < 3; i++)
        Cx000dac_task();

    REQUIRE(fakeC6000.audioChunks == 3);

    // Start the stream with the audio task "preempting" the mode switch
    fakeC6000.onCfgWrite = preemptOnOpenMusic;
    preemptArmed = true;
    REQUIRE(Cx000_dac_audio_driver.start(0, NULL, &ctx) == 0);
    REQUIRE(preemptArmed == false);
    pthread_join(preemptThread, NULL);
    fakeC6000.onCfgWrite = NULL;

    // The "preempting" task ran in stream mode after start() and sent the
    // first chunk of the stream.
    REQUIRE(fakeC6000.audioChunks == 4);
    REQUIRE(fakeC6000.lastAudio == (const uint8_t *)&buf[0]);

    // Following chunks are read from inside the buffer
    Cx000dac_task();
    REQUIRE(fakeC6000.audioChunks == 5);
    REQUIRE(fakeC6000.lastAudio == (const uint8_t *)&buf[32]);

    Cx000_dac_audio_driver.terminate(&ctx);
    Cx000dac_terminate();
}

static volatile bool stopTask = false;

static void *taskFunc(void *arg)
{
    (void)arg;

    while (stopTask == false)
        Cx000dac_task();

    return NULL;
}

TEST_CASE("Cx000 DAC: beeps and streams can be switched while the task runs",
          "[audio][cx000dac]")
{
    setupStream();
    Cx000dac_init(&dev);

    // Audio thread: run the driver task at full speed for the whole test
    stopTask = false;

    pthread_t taskThread;
    REQUIRE(pthread_create(&taskThread, NULL, taskFunc, NULL) == 0);

    // UI/codec threads: alternate beeps and streams
    for (int i = 0; i < 200; i++) {
        REQUIRE(Cx000dac_startBeep(1000) == 0);
        usleep(50);
        REQUIRE(Cx000_dac_audio_driver.start(0, NULL, &ctx) == 0);
        usleep(50);
        Cx000_dac_audio_driver.terminate(&ctx);
        REQUIRE(ctx.running == 0);
        Cx000dac_stopBeep();
    }

    stopTask = true;
    pthread_join(taskThread, NULL);
    Cx000dac_terminate();
}
