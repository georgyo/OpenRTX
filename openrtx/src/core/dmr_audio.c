/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "hwconfig.h"

#ifdef CONFIG_DMR

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include "core/audio_stream.h"
#include "core/dmr_audio.h"
#include "core/dmr_vocoder.h"
#include "core/dsp.h"
#include "core/threads.h"
#include "protocols/DMR/Silence.h"

#ifndef CONFIG_MIC_GAIN
#define CONFIG_MIC_GAIN 1
#endif

#ifndef CONFIG_MIC_OVERSAMPLE
#define CONFIG_MIC_OVERSAMPLE 1
#endif

#define FRAME_BYTES DMR_VOCODER_AMBE_BYTES
#define BLOCK_SAMPLES DMR_VOCODER_PCM_SAMPLES
#define SAMPLE_RATE DMR_VOCODER_SAMPLE_RATE

/* Double-buffered streams: one 60 ms block per half */
#define OUT_BUF_SAMPLES (BLOCK_SAMPLES * 2)
#define IN_BUF_SAMPLES (BLOCK_SAMPLES * 2 * CONFIG_MIC_OVERSAMPLE)

enum { MODE_NONE = 0, MODE_RX, MODE_TX };

static const struct dmrVocoder *vocoder;
static bool vocoderOk;
static uint8_t initCnt;

/*
 * Thread state. 'running' is set by the starter before the thread exists
 * and cleared by the thread itself on its way out, or by stopThread() after
 * the join; 'alive' says the pthread has been created and not joined yet,
 * so that a thread which quit on its own is still reclaimed. Every
 * transition is serialised by init_mutex, like audio_codec.c.
 */
static volatile bool running;
static volatile bool reqStop;
static bool alive;
static uint8_t mode;
static pathId audioPath;
static streamId stream;

static pthread_t thread;
static pthread_attr_t threadAttr;
static pthread_mutex_t data_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t init_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Payload queue, one direction at a time */
static uint8_t queue[DMR_AUDIO_QUEUE_DEPTH][FRAME_BYTES];
static uint8_t readPos;
static uint8_t writePos;
static uint8_t numElements;
static uint8_t lastRx[FRAME_BYTES];

static struct dmrAudioStats stats;

/* Output stream buffer: two blocks, static so that it never sits on the
 * thread stack. The input buffer depends on the oversampling and is
 * allocated by the thread, as the M17 encoder does. */
static stream_sample_t outBuf[OUT_BUF_SAMPLES];

static void *rxFunc(void *arg);
static void *txFunc(void *arg);
static bool startThread(const pathId path, uint8_t newMode);
static void stopThread(void);

void dmrAudio_init(void)
{
    pthread_mutex_lock(&init_mutex);
    initCnt += 1;
    if (initCnt == 1) {
        vocoder = dmrVocoder_get();
        vocoderOk = (vocoder != NULL) && (vocoder->init() == 0);
        memset(&stats, 0x00, sizeof(stats));
    }
    pthread_mutex_unlock(&init_mutex);
}

void dmrAudio_terminate(void)
{
    pthread_mutex_lock(&init_mutex);
    if (initCnt > 0)
        initCnt -= 1;

    if (initCnt == 0) {
        stopThread();
        if (vocoderOk)
            vocoder->terminate();
        vocoderOk = false;
    }
    pthread_mutex_unlock(&init_mutex);
}

bool dmrAudio_startRx(const pathId path)
{
    return startThread(path, MODE_RX);
}

bool dmrAudio_startTx(const pathId path)
{
    return startThread(path, MODE_TX);
}

void dmrAudio_stop(void)
{
    pthread_mutex_lock(&init_mutex);
    stopThread();
    pthread_mutex_unlock(&init_mutex);
}

bool dmrAudio_running(void)
{
    return running;
}

bool dmrAudio_pushFrame(const uint8_t ambe[27])
{
    pthread_mutex_lock(&data_mutex);

    if ((running == false) || (mode != MODE_RX)) {
        pthread_mutex_unlock(&data_mutex);
        return false;
    }

    /* Full: the oldest payload makes room, the call stays on time */
    if (numElements >= DMR_AUDIO_QUEUE_DEPTH) {
        readPos = (readPos + 1) % DMR_AUDIO_QUEUE_DEPTH;
        numElements -= 1;
        stats.overruns += 1;
    }

    memcpy(queue[writePos], ambe, FRAME_BYTES);
    writePos = (writePos + 1) % DMR_AUDIO_QUEUE_DEPTH;
    numElements += 1;

    stats.framesRx += 1;
    if (memcmp(lastRx, ambe, FRAME_BYTES) != 0) {
        stats.varying += 1;
        memcpy(lastRx, ambe, FRAME_BYTES);
    }

    pthread_mutex_unlock(&data_mutex);
    return true;
}

bool dmrAudio_popFrame(uint8_t ambe[27])
{
    pthread_mutex_lock(&data_mutex);

    if ((running == false) || (mode != MODE_TX) || (numElements == 0)) {
        pthread_mutex_unlock(&data_mutex);
        return false;
    }

    memcpy(ambe, queue[readPos], FRAME_BYTES);
    readPos = (readPos + 1) % DMR_AUDIO_QUEUE_DEPTH;
    numElements -= 1;

    pthread_mutex_unlock(&data_mutex);
    return true;
}

void dmrAudio_flushTx(void)
{
    pthread_mutex_lock(&data_mutex);

    /* Keep the newest payload: the next burst carries current audio */
    if ((running == true) && (mode == MODE_TX) && (numElements > 1)) {
        readPos = (readPos + numElements - 1) % DMR_AUDIO_QUEUE_DEPTH;
        numElements = 1;
    }

    pthread_mutex_unlock(&data_mutex);
}

void dmrAudio_getStats(struct dmrAudioStats *s)
{
    pthread_mutex_lock(&data_mutex);
    *s = stats;
    pthread_mutex_unlock(&data_mutex);
}

const char *dmrAudio_backendName(void)
{
    if (vocoder == NULL)
        return "none";

    return vocoder->name;
}

/*
 * Reception: pop a payload per block, decode it into the idle half of the
 * output stream, silence when none arrived.
 */
static void *rxFunc(void *arg)
{
    pathId path = *((pathId *)arg);
    uint8_t frame[FRAME_BYTES];
    bool primed = false; /* a payload has been played in this run    */
    uint32_t gap = 0;    /* silent blocks since the last payload     */

    /*
     * Line up with the stream before writing into it, as the M17 decoder
     * does: writing the half being played crackles.
     */
    outputStream_sync(stream, false);

    while (reqStop == false) {
        /* Path closed or suspended by a higher priority one: quit */
        if (audioPath_getStatus(path) != PATH_OPEN)
            break;

        bool newData = false;

        pthread_mutex_lock(&data_mutex);
        if (numElements != 0) {
            memcpy(frame, queue[readPos], FRAME_BYTES);
            readPos = (readPos + 1) % DMR_AUDIO_QUEUE_DEPTH;
            numElements -= 1;
            newData = true;
        }
        pthread_mutex_unlock(&data_mutex);

        stream_sample_t *buf = outputStream_getIdleBuffer(stream);
        if (buf == NULL)
            break;

        if (newData) {
            if (vocoderOk)
                vocoder->decode(frame, buf);
            else
                memset(buf, 0x00, BLOCK_SAMPLES * sizeof(stream_sample_t));
        } else {
            memset(buf, 0x00, BLOCK_SAMPLES * sizeof(stream_sample_t));
        }

        pthread_mutex_lock(&data_mutex);
        stats.blocksOut += 1;
        if (newData) {
            stats.underruns += gap;
            gap = 0;
            primed = true;
        } else if (primed) {
            gap += 1;
        }
        pthread_mutex_unlock(&data_mutex);

        /*
         * The return value is not a reason to quit, as in the M17 decoder:
         * the Cx000 DAC driver answers false when the sync point had
         * already passed on entry (the thread came one period late), a
         * glitch to play through, not a dead stream. A stopped or suspended
         * path is caught at the top of the loop, and a stream that is gone
         * by outputStream_getIdleBuffer() returning NULL.
         */
        outputStream_sync(stream, true);
    }

    /* Stop the stream and wait for its effective termination */
    audioStream_stop(stream);

    running = false;
    return NULL;
}

/*
 * Transmission: read a block from the input stream, condition it as the M17
 * encoder does (decimation, DC block, gain), encode and queue it.
 */
static void *txFunc(void *arg)
{
    pathId path = *((pathId *)arg);
    struct dcBlock dcBlock;
    struct decimatorState decimator;
    uint8_t frame[FRAME_BYTES];

    dsp_resetState(dcBlock);
    dsp_resetState(decimator);

    while (reqStop == false) {
        if (audioPath_getStatus(path) != PATH_OPEN)
            break;

        dataBlock_t audio = inputStream_getData(stream);
        if ((audio.data == NULL) || (audio.len == 0))
            break;

        /*
         * Each block is BLOCK_SAMPLES * CONFIG_MIC_OVERSAMPLE raw samples,
         * one half of the double buffer: decimate in place, then condition.
         */
        size_t outIdx = 0;
        for (size_t i = 0; i < audio.len; i++) {
            int16_t sample = audio.data[i];

#if CONFIG_MIC_OVERSAMPLE > 1
            if (!dsp_decimator(&decimator, &sample, CONFIG_MIC_OVERSAMPLE))
                continue;
#endif

            sample = dsp_dcBlockFilter(&dcBlock, sample);
            audio.data[outIdx++] =
                dsp_saturate16((int32_t)sample * CONFIG_MIC_GAIN);
        }

        /* A short block (stream error) is padded with silence */
        for (; outIdx < BLOCK_SAMPLES; outIdx++)
            audio.data[outIdx] = 0;

        if (vocoderOk)
            vocoder->encode(audio.data, frame);
        else
            memcpy(frame, dmr_ambeSilence, FRAME_BYTES);

        pthread_mutex_lock(&data_mutex);
        if (numElements >= DMR_AUDIO_QUEUE_DEPTH) {
            readPos = (readPos + 1) % DMR_AUDIO_QUEUE_DEPTH;
            numElements -= 1;
            stats.overruns += 1;
        }

        memcpy(queue[writePos], frame, FRAME_BYTES);
        writePos = (writePos + 1) % DMR_AUDIO_QUEUE_DEPTH;
        numElements += 1;
        stats.framesTx += 1;
        pthread_mutex_unlock(&data_mutex);
    }

    audioStream_terminate(stream);

    running = false;
    return NULL;
}

/*
 * Wrapper of txFunc() owning the oversampled input buffer: allocated here,
 * as the stack of the thread is not sized for it, and freed on the way out
 * whatever made the loop quit.
 */
static void *txEntry(void *arg)
{
    stream_sample_t *inBuf = (stream_sample_t *)arg;
    void *ret = txFunc(&audioPath);
    free(inBuf);
    return ret;
}

static bool startThread(const pathId path, uint8_t newMode)
{
    /* Bad incoming path */
    if (audioPath_getStatus(path) != PATH_OPEN)
        return false;

    /*
     * Serialise with stopThread() and with other starters: the mutex is
     * held until the new thread is created, so that a concurrent stop
     * cannot slip in between the stop of the old thread and the start of
     * the new one.
     */
    pthread_mutex_lock(&init_mutex);
    if (running) {
        /* Same direction on the same open path: already running */
        if ((path == audioPath) && (newMode == mode)) {
            pthread_mutex_unlock(&init_mutex);
            return true;
        }

        /*
         * Another path takes over only if it has a higher priority or the
         * current one is closed/suspended.
         */
        pathInfo_t newPath = audioPath_getInfo(path);
        pathInfo_t curPath = audioPath_getInfo(audioPath);
        if ((curPath.status == PATH_OPEN) && (curPath.prio >= newPath.prio)) {
            pthread_mutex_unlock(&init_mutex);
            return false;
        }
    }

    /* Reclaim the previous thread, running or already gone */
    stopThread();

    /*
     * The stream is opened here, in the caller's context, so that a sink or
     * source without a device (the emulator has no microphone) fails the
     * start at once instead of leaving a thread to find out.
     */
    stream_sample_t *inBuf = NULL;
    if (newMode == MODE_RX) {
        memset(outBuf, 0x00, sizeof(outBuf));
        stream = audioStream_start(path, outBuf, OUT_BUF_SAMPLES, SAMPLE_RATE,
                                   STREAM_OUTPUT | BUF_CIRC_DOUBLE);
    } else {
        inBuf = malloc(IN_BUF_SAMPLES * sizeof(stream_sample_t));
        if (inBuf == NULL) {
            pthread_mutex_unlock(&init_mutex);
            return false;
        }

        stream = audioStream_start(path, inBuf, IN_BUF_SAMPLES,
                                   SAMPLE_RATE * CONFIG_MIC_OVERSAMPLE,
                                   STREAM_INPUT | BUF_CIRC_DOUBLE);
    }

    if (stream < 0) {
        free(inBuf);
        pthread_mutex_unlock(&init_mutex);
        return false;
    }

    running = true;
    reqStop = false;
    mode = newMode;
    audioPath = path;

    readPos = 0;
    writePos = 0;
    numElements = 0;
    memset(lastRx, 0x00, sizeof(lastRx));

    pthread_attr_init(&threadAttr);

#if defined(_MIOSIX)
    pthread_attr_setstacksize(&threadAttr, DMR_AUDIO_THREAD_STKSIZE);

    /* Same priority as the rtx thread, which paces the payloads */
    struct sched_param param;
    param.sched_priority = THREAD_PRIO_HIGH;
    pthread_attr_setschedparam(&threadAttr, &param);
#elif defined(__ZEPHYR__)
    void *threadStack = malloc(DMR_AUDIO_THREAD_STKSIZE * sizeof(uint8_t));
    pthread_attr_setstack(&threadAttr, threadStack, DMR_AUDIO_THREAD_STKSIZE);
#endif

    int ret;
    if (newMode == MODE_RX)
        ret = pthread_create(&thread, &threadAttr, rxFunc, &audioPath);
    else
        ret = pthread_create(&thread, &threadAttr, txEntry, inBuf);

    if (ret != 0) {
        running = false;
        mode = MODE_NONE;
        if (newMode == MODE_RX)
            audioStream_stop(stream);
        else
            audioStream_terminate(stream);
        free(inBuf);
#if defined(__ZEPHYR__)
        free(threadStack);
#endif
    } else {
        alive = true;
    }

    bool started = running;
    pthread_mutex_unlock(&init_mutex);

    return started;
}

/*
 * Stop the thread and wait for its termination. Must be called with
 * init_mutex locked: the join is exclusive and the function idempotent, a
 * second caller finds nothing alive and returns. A thread which already
 * quit on its own is joined here all the same, so that its resources are
 * reclaimed and no detach is ever needed.
 */
static void stopThread(void)
{
    if (alive == false)
        return;

    reqStop = true;
    pthread_join(thread, NULL);
    alive = false;
    running = false;
    mode = MODE_NONE;

#ifdef __ZEPHYR__
    void *addr;
    size_t size;

    pthread_attr_getstack(&threadAttr, &addr, &size);
    free(addr);
#endif
}

#endif /* CONFIG_DMR */
