/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "hwconfig.h"

#ifdef CONFIG_DMR

#include <math.h>
#include <stdint.h>
#include <string.h>
#include "core/dmr_vocoder.h"

/*
 * Loopback vocoder backend for the Linux emulator: not a codec, a coarse
 * descriptor of 60 ms of audio packed into the 27 bytes of a voice payload,
 * enough to reproduce a tone with the same level, frequency and envelope on
 * the other side. The point is to exercise and hear the whole RX/TX plumbing
 * (audio streams, jitter buffer, slot timing, V_SPI transfers) with something
 * that is obviously not AMBE: a real network would hear noise from these
 * payloads and this decoder plays silence for real AMBE.
 *
 * Payload layout, little endian:
 *
 *   [0..1]   signature "LB"
 *   [2..3]   RMS level of the block, 0..32767
 *   [4..5]   dominant frequency, Hz, from the zero crossings
 *   [6..25]  envelope: peak |sample| >> 7 of each of the 20 blocks of 24
 *            samples (3 ms)
 *   [26]     XOR of bytes 0..25
 *
 * The decoder plays a sine at the dominant frequency, shaped by the envelope
 * and scaled to the RMS, with a phase accumulator kept across calls so that
 * consecutive payloads join without a click.
 */

#define LB_MAGIC0 0x4C /* 'L' */
#define LB_MAGIC1 0x42 /* 'B' */
#define LB_ENV_BLOCKS 20
#define LB_ENV_SAMPLES (DMR_VOCODER_PCM_SAMPLES / LB_ENV_BLOCKS)
#define LB_ENV_OFFSET 6
#define LB_CHECK_OFFSET 26
#define LB_TWO_PI 6.28318530717958647692f

static uint32_t phase; /* Decoder phase accumulator, one turn = 2^32 */

static uint32_t isqrt32(uint32_t x)
{
    uint32_t r = 0;
    uint32_t bit = 1u << 30;

    while (bit > x)
        bit >>= 2;

    while (bit != 0) {
        if (x >= r + bit) {
            x -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }

        bit >>= 2;
    }

    return r;
}

static uint8_t checksum(const uint8_t ambe[DMR_VOCODER_AMBE_BYTES])
{
    uint8_t x = 0;

    for (size_t i = 0; i < LB_CHECK_OFFSET; i++)
        x ^= ambe[i];

    return x;
}

/*
 * Dominant frequency from the zero crossings, with a dead band of one eighth
 * of the peak so that the noise around zero of a quiet block is not counted.
 * The half periods are counted between the first and the last crossing, so
 * that the partial periods at the block edges do not bias the result: a tone
 * is measured within a few Hz (one sample of jitter at each end over the
 * span of the block).
 */
static uint16_t dominantFreq(const int16_t pcm[DMR_VOCODER_PCM_SAMPLES],
                             int32_t peak)
{
    int32_t thr = peak / 8;
    int sign = 0;
    uint32_t crossings = 0;
    size_t first = 0;
    size_t last = 0;

    for (size_t i = 0; i < DMR_VOCODER_PCM_SAMPLES; i++) {
        int s = 0;
        if (pcm[i] > thr)
            s = 1;
        else if (pcm[i] < -thr)
            s = -1;

        if ((s != 0) && (sign != 0) && (s != sign)) {
            if (crossings == 0)
                first = i;
            last = i;
            crossings++;
        }

        if (s != 0)
            sign = s;
    }

    if (crossings < 2)
        return 0;

    uint32_t span = (uint32_t)(last - first);
    uint32_t f = ((crossings - 1) * DMR_VOCODER_SAMPLE_RATE + span)
               / (2 * span);

    return (f > 0xFFFF) ? 0xFFFF : (uint16_t)f;
}

static int loopbackInit(void)
{
    phase = 0;
    return 0;
}

static void loopbackTerminate(void)
{
}

static bool loopbackEncode(const int16_t pcm[DMR_VOCODER_PCM_SAMPLES],
                           uint8_t ambe[DMR_VOCODER_AMBE_BYTES])
{
    uint64_t energy = 0;
    int32_t peak = 0;

    for (size_t b = 0; b < LB_ENV_BLOCKS; b++) {
        int32_t blockPeak = 0;

        for (size_t i = 0; i < LB_ENV_SAMPLES; i++) {
            int32_t s = pcm[b * LB_ENV_SAMPLES + i];
            int32_t a = (s < 0) ? -s : s;

            /* |-32768| saturates to 32767: the envelope byte is 8 bits */
            if (a > INT16_MAX)
                a = INT16_MAX;

            energy += (uint64_t)(s * s);
            if (a > blockPeak)
                blockPeak = a;
        }

        ambe[LB_ENV_OFFSET + b] = (uint8_t)(blockPeak >> 7);
        if (blockPeak > peak)
            peak = blockPeak;
    }

    uint32_t rms = isqrt32((uint32_t)(energy / DMR_VOCODER_PCM_SAMPLES));
    uint16_t freq = dominantFreq(pcm, peak);

    ambe[0] = LB_MAGIC0;
    ambe[1] = LB_MAGIC1;
    ambe[2] = (uint8_t)(rms & 0xFF);
    ambe[3] = (uint8_t)(rms >> 8);
    ambe[4] = (uint8_t)(freq & 0xFF);
    ambe[5] = (uint8_t)(freq >> 8);
    ambe[LB_CHECK_OFFSET] = checksum(ambe);

    return true;
}

static bool loopbackDecode(const uint8_t ambe[DMR_VOCODER_AMBE_BYTES],
                           int16_t pcm[DMR_VOCODER_PCM_SAMPLES])
{
    if ((ambe[0] != LB_MAGIC0) || (ambe[1] != LB_MAGIC1)
        || (ambe[LB_CHECK_OFFSET] != checksum(ambe))) {
        memset(pcm, 0x00, DMR_VOCODER_PCM_SAMPLES * sizeof(int16_t));
        return false;
    }

    uint32_t rms = (uint32_t)ambe[2] | ((uint32_t)ambe[3] << 8);
    uint32_t freq = (uint32_t)ambe[4] | ((uint32_t)ambe[5] << 8);
    uint32_t step =
        (uint32_t)(((uint64_t)freq << 32) / DMR_VOCODER_SAMPLE_RATE);
    uint64_t energy = 0;

    for (size_t b = 0; b < LB_ENV_BLOCKS; b++) {
        float amp = (float)((int32_t)ambe[LB_ENV_OFFSET + b] << 7);

        for (size_t i = 0; i < LB_ENV_SAMPLES; i++) {
            float angle = (float)phase * (LB_TWO_PI / 4294967296.0f);
            int32_t s = (int32_t)(amp * sinf(angle));

            pcm[b * LB_ENV_SAMPLES + i] = (int16_t)s;
            energy += (uint64_t)(s * s);
            phase += step;
        }
    }

    /*
     * Match the level of the original block: the envelope carries peaks, the
     * RMS of a sine drawn under them is what the encoder measured only for a
     * steady tone.
     */
    uint32_t genRms = isqrt32((uint32_t)(energy / DMR_VOCODER_PCM_SAMPLES));
    if ((genRms > 0) && (genRms != rms)) {
        float gain = (float)rms / (float)genRms;

        for (size_t i = 0; i < DMR_VOCODER_PCM_SAMPLES; i++) {
            float s = (float)pcm[i] * gain;
            if (s > 32767.0f)
                s = 32767.0f;
            if (s < -32768.0f)
                s = -32768.0f;
            pcm[i] = (int16_t)s;
        }
    }

    return true;
}

void dmrVocoderLoopback_tonePayload(uint16_t freqHz, int16_t amplitude,
                                    uint8_t ambe[DMR_VOCODER_AMBE_BYTES])
{
    int16_t pcm[DMR_VOCODER_PCM_SAMPLES];
    float step = LB_TWO_PI * (float)freqHz / (float)DMR_VOCODER_SAMPLE_RATE;

    for (size_t i = 0; i < DMR_VOCODER_PCM_SAMPLES; i++)
        pcm[i] = (int16_t)((float)amplitude * sinf(step * (float)i));

    loopbackEncode(pcm, ambe);
}

const struct dmrVocoder dmrVocoder_loopback = {
    .name = "loopback",
    .init = loopbackInit,
    .terminate = loopbackTerminate,
    .decode = loopbackDecode,
    .encode = loopbackEncode,
};

#ifdef CONFIG_DMR_VOCODER_LOOPBACK
const struct dmrVocoder *dmrVocoder_get(void)
{
    return &dmrVocoder_loopback;
}
#endif

#endif /* CONFIG_DMR */
