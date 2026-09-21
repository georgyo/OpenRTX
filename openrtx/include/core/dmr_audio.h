/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_AUDIO_H
#define DMR_AUDIO_H

#include <stdbool.h>
#include <stdint.h>
#include "core/audio_path.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMR audio layer: the thread between the voice payloads of the call
 * controller and the audio streams of the radio, modelled on the M17 codec
 * thread of audio_codec.c.
 *
 * Reception: the DMR opMode handler pushes the 27-byte payload of every
 * received voice burst (60 ms) into a short queue; the thread decodes each
 * one through the vocoder backend of the build (dmr_vocoder.h) into 480
 * samples and writes them to an 8 kHz output stream opened on the sink of
 * the audio path (SINK_SPK: the Cx000 "OpenMusic" DAC on the CS7000 radios,
 * SDL on the emulator). A burst that did not arrive in time is played as
 * silence and counted as an underrun; a payload pushed on a full queue
 * drops the oldest one and counts as an overrun.
 *
 * Transmission: the thread reads 480-sample blocks from an 8 kHz input
 * stream on the source of the path (SOURCE_MIC, with the CONFIG_MIC_GAIN /
 * CONFIG_MIC_OVERSAMPLE conditioning of the M17 encoder), encodes each block
 * into a payload and queues it; the opMode handler pops one per own voice
 * burst. With the null backend every payload is the AMBE+2 silence pattern,
 * so a transmission is a well-formed call carrying silence.
 *
 * One thread at a time, either direction: starting one direction stops the
 * other. The start and stop transitions are serialised, so that a stop from
 * the UI thread racing a start from the rtx thread cannot leave a thread
 * behind; a thread which quits on its own (the path was closed or suspended
 * by a higher priority one, the stream failed) is reclaimed by the next
 * start or stop. The thread runs with DMR_AUDIO_THREAD_STKSIZE of stack.
 */

/**
 * Depth of the payload queues, in 60 ms payloads: two bursts of jitter on
 * top of the one being played or transmitted.
 */
#define DMR_AUDIO_QUEUE_DEPTH 3

/**
 * Counters of the audio layer, for the DMR monitor screen. Reset when the
 * layer is initialised for the first time (dmrAudio_init()), never wrapped
 * back to zero otherwise.
 */
struct dmrAudioStats {
    uint32_t framesRx;  /**< Payloads handed to dmrAudio_pushFrame() while
                             reception was running, dropped ones included */
    uint32_t framesTx;  /**< Payloads encoded from the input stream        */
    uint32_t blocksOut; /**< 480-sample blocks written to the output
                             stream, silence included                      */
    uint16_t underruns; /**< Silent blocks played between two payloads of
                             the same reception: bursts lost or late. The
                             silence before the first payload and after the
                             last one is not counted                       */
    uint16_t overruns;  /**< Payloads dropped from a full queue, either
                             direction                                     */
    uint16_t varying;   /**< Received payloads differing from the one
                             pushed before them: stays at zero when the
                             modem hands over the same bytes every burst,
                             what a dead V_SPI read would look like        */
};

/**
 * Initialise the audio layer and the vocoder backend. Recursive calls are
 * allowed and must be balanced by as many calls to dmrAudio_terminate().
 * The counters are reset by the first call.
 */
void dmrAudio_init(void);

/**
 * Stop the thread, if running, and release the vocoder backend once the
 * last dmrAudio_init() has been balanced.
 */
void dmrAudio_terminate(void);

/**
 * Start playing received voice: open the output stream on the sink of the
 * given path and start the thread. Returns true at once when the thread is
 * already playing on that path; stops a thread running on another path
 * unless that path is open with a priority at least as high.
 *
 * @param path: audio path, must be open.
 * @return true when the thread is running, false when the path is not open,
 * was refused, the stream could not be opened or the thread not created.
 */
bool dmrAudio_startRx(const pathId path);

/**
 * Start encoding own voice: open the input stream on the source of the
 * given path and start the thread. Same rules as dmrAudio_startRx().
 *
 * @param path: audio path, must be open.
 * @return true when the thread is running, false otherwise.
 */
bool dmrAudio_startTx(const pathId path);

/**
 * Stop the thread, whichever direction it runs, release its stream and wait
 * for its termination. Safe to call when nothing runs, and from more than
 * one thread at once.
 */
void dmrAudio_stop(void);

/**
 * @return true while the thread runs, in either direction. False once the
 * thread quit on its own: the caller may then start it again if the path
 * came back.
 */
bool dmrAudio_running(void);

/**
 * Queue a received voice payload for playback. Never blocks: on a full
 * queue the oldest payload is dropped and counted as an overrun.
 *
 * @param ambe: 27 bytes, three AMBE+2 frames.
 * @return true when the payload was taken, false when reception is not
 * running (the caller may ignore it).
 */
bool dmrAudio_pushFrame(const uint8_t ambe[27]);

/**
 * Take the next encoded payload for an own voice burst. Never blocks.
 *
 * @param ambe: buffer for 27 bytes, untouched when nothing is returned.
 * @return true when a payload was written, false when the queue is empty or
 * encoding is not running: the caller then transmits the silence pattern.
 */
bool dmrAudio_popFrame(uint8_t ambe[27]);

/**
 * Copy the counters.
 *
 * @param stats: destination.
 */
void dmrAudio_getStats(struct dmrAudioStats *stats);

/**
 * @return the name of the vocoder backend of this build, "none" before the
 * first dmrAudio_init().
 */
const char *dmrAudio_backendName(void);

#ifdef __cplusplus
}
#endif

#endif /* DMR_AUDIO_H */
