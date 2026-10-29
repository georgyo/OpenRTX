/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "hwconfig.h"

#ifdef CONFIG_DMR

#include <string.h>
#include "core/dmr_vocoder.h"
#include "core/state.h"
#include "protocols/DMR/Silence.h"

/*
 * Null vocoder backend: no codec at all. Received voice becomes silence, or
 * the 1 kHz test tone of Settings > DMR > Test tone, which lets the audio
 * path (DAC, amplifier, mutes, slot timing of the pushes) be checked on a
 * radio with a received call and nothing else; own voice becomes the AMBE+2
 * silence pattern, so that a transmission is a well-formed call carrying
 * silence.
 *
 * The tone is a table of one period: 1 kHz at 8 kHz is eight samples, the
 * amplitude -12 dBFS. The phase index survives across calls, so the tone is
 * continuous from one 60 ms block to the next whatever the block boundary.
 */
static const int16_t toneTable[8] = { 0,
                                      5820, /* 8231 * sin(45 deg) */
                                      DMR_VOCODER_TONE_AMPLITUDE,
                                      5820,
                                      0,
                                      -5820,
                                      -DMR_VOCODER_TONE_AMPLITUDE,
                                      -5820 };

static uint8_t tonePhase;

static int nullInit(void)
{
    tonePhase = 0;
    return 0;
}

static void nullTerminate(void)
{
}

static bool nullDecode(const uint8_t ambe[DMR_VOCODER_AMBE_BYTES],
                       int16_t pcm[DMR_VOCODER_PCM_SAMPLES])
{
    (void)ambe;

    /*
     * A one-bit field read without the state mutex, like the other core
     * modules reading their settings: a single byte load, never torn, and
     * a change taking effect one block late is fine for a debug switch.
     */
    if (state.settings.dmr_testTone == 0) {
        memset(pcm, 0x00, DMR_VOCODER_PCM_SAMPLES * sizeof(int16_t));
        return false;
    }

    for (size_t i = 0; i < DMR_VOCODER_PCM_SAMPLES; i++) {
        pcm[i] = toneTable[tonePhase];
        tonePhase = (tonePhase + 1) & 0x07;
    }

    return true;
}

static bool nullEncode(const int16_t pcm[DMR_VOCODER_PCM_SAMPLES],
                       uint8_t ambe[DMR_VOCODER_AMBE_BYTES])
{
    (void)pcm;

    memcpy(ambe, dmr_ambeSilence, DMR_VOCODER_AMBE_BYTES);
    return false;
}

const struct dmrVocoder dmrVocoder_null = {
    .name = "null",
    .init = nullInit,
    .terminate = nullTerminate,
    .decode = nullDecode,
    .encode = nullEncode,
};

/*
 * The backend of the build, unless the loopback one was selected: then
 * dmr_vocoder_loopback.c provides dmrVocoder_get().
 */
#ifndef CONFIG_DMR_VOCODER_LOOPBACK
const struct dmrVocoder *dmrVocoder_get(void)
{
    return &dmrVocoder_null;
}
#endif

#endif /* CONFIG_DMR */
