/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_VOCODER_H
#define DMR_VOCODER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DMR vocoder backend seam.
 *
 * The HR_C6000 baseband does not contain a vocoder: it exchanges opaque voice
 * payloads with the MCU and the codec runs in software. A DMR voice burst
 * carries 216 bits of voice, three 72-bit AMBE+2 frames of 20 ms each, so one
 * payload of DMR_VOCODER_AMBE_BYTES stands for 60 ms of speech, that is
 * DMR_VOCODER_PCM_SAMPLES samples at DMR_VOCODER_SAMPLE_RATE (ETSI TS 102
 * 361-1 §6.1 Figure 6.2 for the payload, TS 102 361-2 §5.2.1 for the codec).
 *
 * No AMBE/AMBE+2 implementation lives in this tree: DVSI's patent US8359197B2
 * runs until 2028-05-20 and the project does not ship codec code of any
 * origin. What the tree provides is this interface plus two backends:
 *
 *  - the null backend (dmr_vocoder_null.c): decode() produces silence, or a
 *    1 kHz test tone when settings.dmr_testTone is set; encode() produces the
 *    AMBE+2 silence pattern. Built for every target;
 *  - the loopback backend (dmr_vocoder_loopback.c): encode() packs a coarse
 *    descriptor of the PCM block (level, dominant frequency, envelope) into
 *    the 27 bytes and decode() plays a tone back from it, so that the whole
 *    RX/TX plumbing can be exercised and heard on the Linux emulator. It is
 *    not a codec and interoperates with nothing.
 *
 * A third choice of the dmr_vocoder Meson option, 'external', builds neither
 * backend: whoever holds a licence for a real vocoder links an object that
 * provides dmrVocoder_get() (see docs/dmr_mode.md, "Vocoder").
 *
 * Calling convention: init() is called once before any other function,
 * terminate() once after the last; decode() and encode() are called from the
 * DMR audio thread only, are not reentrant and must not block for longer than
 * a fraction of the 60 ms they stand for. On a radio they run on the MCU with
 * the stack of that thread.
 */

#define DMR_VOCODER_AMBE_BYTES 27    /* Bytes of one voice payload        */
#define DMR_VOCODER_PCM_SAMPLES 480  /* PCM samples per payload, 60 ms    */
#define DMR_VOCODER_SAMPLE_RATE 8000 /* PCM sample rate, Hz               */

/*
 * Null backend test tone: 1 kHz, -12 dBFS (32767 * 10^(-12/20) = 8231).
 */
#define DMR_VOCODER_TONE_HZ 1000
#define DMR_VOCODER_TONE_AMPLITUDE 8231

struct dmrVocoder {
    /**
     * Backend name, for the logs and the DMR monitor screen.
     */
    const char *name;

    /**
     * Initialise the backend.
     *
     * @return zero on success, a negative errno value otherwise: the audio
     * layer then behaves as with the null backend.
     */
    int (*init)(void);

    /**
     * Release the backend.
     */
    void (*terminate)(void);

    /**
     * Decode one voice payload into 60 ms of PCM.
     *
     * The PCM buffer is always filled on return: with audio when the function
     * returns true, with silence when it returns false, so that a caller
     * which ignores the return value still plays something sensible. A
     * backend returns false when it has no audio for the payload (the null
     * backend with the test tone off, a loopback payload with a bad
     * signature, a corrupted AMBE frame); the caller may then substitute its
     * own silence or skip the playback.
     *
     * @param ambe: 27 bytes, three AMBE+2 frames.
     * @param pcm: 480 signed 16-bit samples at 8 kHz.
     * @return true when pcm holds audio, false when it holds silence.
     */
    bool (*decode)(const uint8_t ambe[DMR_VOCODER_AMBE_BYTES],
                   int16_t pcm[DMR_VOCODER_PCM_SAMPLES]);

    /**
     * Encode 60 ms of PCM into one voice payload.
     *
     * The payload buffer is always filled on return: with encoded voice when
     * the function returns true, with the AMBE+2 silence pattern when it
     * returns false (the null backend always, another backend when it could
     * not encode). The call controller transmits the buffer either way; the
     * DMR silence pattern is what the networks expect in a burst without
     * speech.
     *
     * @param pcm: 480 signed 16-bit samples at 8 kHz.
     * @param ambe: buffer for 27 bytes.
     * @return true when ambe holds encoded voice, false when it holds the
     * silence pattern.
     */
    bool (*encode)(const int16_t pcm[DMR_VOCODER_PCM_SAMPLES],
                   uint8_t ambe[DMR_VOCODER_AMBE_BYTES]);
};

/**
 * Get the vocoder backend of this build. The pointer is valid for the whole
 * life of the firmware.
 *
 * Provided by dmr_vocoder_null.c or dmr_vocoder_loopback.c according to the
 * dmr_vocoder Meson option, or by an out-of-tree object when the option is
 * 'external'. This is the only symbol such an object must define.
 *
 * @return the vocoder backend.
 */
const struct dmrVocoder *dmrVocoder_get(void);

/**
 * The null backend: always compiled with CONFIG_DMR, whichever backend
 * dmrVocoder_get() returns. Directly reachable for the tests.
 */
extern const struct dmrVocoder dmrVocoder_null;

/**
 * The loopback backend: compiled for the Linux emulator (and for a radio
 * when the dmr_vocoder option forces it, which is only useful for a bench
 * test of the V_SPI transfers between two OpenRTX radios).
 */
extern const struct dmrVocoder dmrVocoder_loopback;

/**
 * Build a loopback payload standing for 60 ms of a steady tone, without
 * going through a PCM buffer: what the emulator injects in the voice bursts
 * of a synthetic call to make it audible through the loopback decoder.
 *
 * @param freqHz: tone frequency, 0 for silence.
 * @param amplitude: peak amplitude, 0 to 32767.
 * @param ambe: buffer for 27 bytes.
 */
void dmrVocoderLoopback_tonePayload(uint16_t freqHz, int16_t amplitude,
                                    uint8_t ambe[DMR_VOCODER_AMBE_BYTES]);

#ifdef __cplusplus
}
#endif

#endif /* DMR_VOCODER_H */
