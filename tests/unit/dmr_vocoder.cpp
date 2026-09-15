/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "protocols/DMR/Constants.hpp"

extern "C" {
#include "core/dmr_vocoder.h"
#include "core/state.h"
#include "protocols/DMR/Silence.h"
}

/*
 * Host tests of the two in-tree DMR vocoder backends: the null backend
 * (silence or the 1 kHz test tone, silence pattern on encode) and the
 * emulator loopback backend (tone descriptor round trip). Both are reached
 * through their exported structs, whatever backend dmrVocoder_get() returns
 * in this build.
 */

static const size_t N = DMR_VOCODER_PCM_SAMPLES;
static const size_t A = DMR_VOCODER_AMBE_BYTES;
static const double FS = DMR_VOCODER_SAMPLE_RATE;

/*
 * Buffers with guard zones on both sides: a backend writing outside its
 * arrays trips the guards (and ASan, when enabled).
 */
struct PcmBuf {
    int16_t front[16];
    int16_t pcm[DMR_VOCODER_PCM_SAMPLES];
    int16_t back[16];

    PcmBuf()
    {
        memset(front, 0x5A, sizeof(front));
        memset(pcm, 0x00, sizeof(pcm));
        memset(back, 0x5A, sizeof(back));
    }

    bool guardsIntact() const
    {
        for (size_t i = 0; i < 16; i++) {
            if ((front[i] != 0x5A5A) || (back[i] != 0x5A5A))
                return false;
        }
        return true;
    }
};

struct AmbeBuf {
    uint8_t front[16];
    uint8_t ambe[DMR_VOCODER_AMBE_BYTES];
    uint8_t back[16];

    AmbeBuf()
    {
        memset(front, 0xC3, sizeof(front));
        memset(ambe, 0x00, sizeof(ambe));
        memset(back, 0xC3, sizeof(back));
    }

    bool guardsIntact() const
    {
        for (size_t i = 0; i < 16; i++) {
            if ((front[i] != 0xC3) || (back[i] != 0xC3))
                return false;
        }
        return true;
    }
};

static double rms(const int16_t *pcm, size_t n)
{
    double e = 0.0;
    for (size_t i = 0; i < n; i++)
        e += (double)pcm[i] * (double)pcm[i];
    return sqrt(e / (double)n);
}

static size_t zeroCrossings(const int16_t *pcm, size_t n)
{
    size_t c = 0;
    for (size_t i = 1; i < n; i++) {
        if (((pcm[i - 1] < 0) && (pcm[i] >= 0))
            || ((pcm[i - 1] >= 0) && (pcm[i] < 0)))
            c++;
    }
    return c;
}

/*
 * Magnitude of the DFT bin at a given frequency, over the 480 samples: the
 * bin spacing is 8000 / 480 = 16.67 Hz, a 1 kHz tone lands on bin 60.
 */
static double dftMagnitude(const int16_t *pcm, size_t n, double freq)
{
    double re = 0.0;
    double im = 0.0;
    for (size_t i = 0; i < n; i++) {
        double w = 2.0 * M_PI * freq * (double)i / FS;
        re += (double)pcm[i] * cos(w);
        im -= (double)pcm[i] * sin(w);
    }
    return sqrt(re * re + im * im) / (double)n;
}

static double dominantBin(const int16_t *pcm, size_t n)
{
    double best = 0.0;
    double bestFreq = 0.0;
    for (size_t k = 1; k < n / 2; k++) {
        double f = (double)k * FS / (double)n;
        double m = dftMagnitude(pcm, n, f);
        if (m > best) {
            best = m;
            bestFreq = f;
        }
    }
    return bestFreq;
}

static void fillTone(int16_t *pcm, size_t n, double freq, double amplitude)
{
    for (size_t i = 0; i < n; i++)
        pcm[i] = (int16_t)(amplitude * sin(2.0 * M_PI * freq * (double)i / FS));
}

TEST_CASE("Silence pattern is shared between C and C++", "[dmr][vocoder]")
{
    REQUIRE(sizeof(dmr_ambeSilence) == DMR::VOICE_PAYLOAD_BYTES);
    REQUIRE(sizeof(DMR::AMBE_SILENCE) == DMR_VOCODER_AMBE_BYTES);
    REQUIRE((const void *)DMR::AMBE_SILENCE == (const void *)dmr_ambeSilence);
    REQUIRE(dmr_ambeSilence[0] == 0xB9);
    REQUIRE(dmr_ambeSilence[8] == 0x6B);
    REQUIRE(memcmp(dmr_ambeSilence, dmr_ambeSilence + 9, 9) == 0);
    REQUIRE(memcmp(dmr_ambeSilence, dmr_ambeSilence + 18, 9) == 0);
}

TEST_CASE("Backend of the build", "[dmr][vocoder]")
{
    const struct dmrVocoder *v = dmrVocoder_get();
    REQUIRE(v != nullptr);
    REQUIRE(v->name != nullptr);
    REQUIRE(v->init != nullptr);
    REQUIRE(v->terminate != nullptr);
    REQUIRE(v->decode != nullptr);
    REQUIRE(v->encode != nullptr);

#ifdef CONFIG_DMR_VOCODER_LOOPBACK
    REQUIRE(v == &dmrVocoder_loopback);
    REQUIRE(strcmp(v->name, "loopback") == 0);
#else
    REQUIRE(v == &dmrVocoder_null);
    REQUIRE(strcmp(v->name, "null") == 0);
#endif

    REQUIRE(v->init() == 0);
    v->terminate();
}

TEST_CASE("Null backend encode gives the silence pattern", "[dmr][vocoder]")
{
    const struct dmrVocoder *v = &dmrVocoder_null;
    REQUIRE(v->init() == 0);

    PcmBuf in;
    fillTone(in.pcm, N, 440.0, 10000.0);

    AmbeBuf out;
    REQUIRE(v->encode(in.pcm, out.ambe) == false);
    REQUIRE(memcmp(out.ambe, dmr_ambeSilence, A) == 0);
    REQUIRE(out.guardsIntact());
    REQUIRE(in.guardsIntact());

    v->terminate();
}

TEST_CASE("Null backend decode is silence with the test tone off",
          "[dmr][vocoder]")
{
    const struct dmrVocoder *v = &dmrVocoder_null;
    REQUIRE(v->init() == 0);
    state.settings.dmr_testTone = 0;

    AmbeBuf in;
    memset(in.ambe, 0xA5, A);

    PcmBuf out;
    memset(out.pcm, 0x11, sizeof(out.pcm));
    REQUIRE(v->decode(in.ambe, out.pcm) == false);
    for (size_t i = 0; i < N; i++)
        REQUIRE(out.pcm[i] == 0);
    REQUIRE(out.guardsIntact());
    REQUIRE(in.guardsIntact());

    v->terminate();
}

TEST_CASE("Null backend decode is a 1 kHz tone with the test tone on",
          "[dmr][vocoder]")
{
    const struct dmrVocoder *v = &dmrVocoder_null;
    REQUIRE(v->init() == 0);
    state.settings.dmr_testTone = 1;

    AmbeBuf in;
    memcpy(in.ambe, dmr_ambeSilence, A);

    PcmBuf first;
    PcmBuf second;
    REQUIRE(v->decode(in.ambe, first.pcm) == true);
    REQUIRE(v->decode(in.ambe, second.pcm) == true);
    REQUIRE(first.guardsIntact());
    REQUIRE(second.guardsIntact());

    SECTION("Frequency: 60 periods in 60 ms")
    {
        /*
         * 1 kHz at 8 kHz is 8 samples per period, 60 periods per block: 60
         * falling crossings plus 59 rising ones, the first sample of the
         * block being the sixtieth rising edge itself.
         */
        REQUIRE(zeroCrossings(first.pcm, N) == 119);
        REQUIRE(dominantBin(first.pcm, N) == 1000.0);
        REQUIRE(dftMagnitude(first.pcm, N, 1000.0)
                > 10.0 * dftMagnitude(first.pcm, N, 500.0));
    }

    SECTION("Level: -12 dBFS, peak 8231, RMS 5820")
    {
        int16_t peak = 0;
        for (size_t i = 0; i < N; i++) {
            int16_t a = (int16_t)abs(first.pcm[i]);
            if (a > peak)
                peak = a;
        }
        REQUIRE(peak == DMR_VOCODER_TONE_AMPLITUDE);

        double r = rms(first.pcm, N);
        REQUIRE(r > 5700.0);
        REQUIRE(r < 5950.0);
    }

    SECTION("Phase continuity across two calls")
    {
        /*
         * The two blocks joined are 120 periods of the same tone: the
         * second block starts where the first one would continue (the
         * rising edge at the boundary is the one crossing more than twice
         * a block) and, 480 being a multiple of the period, it is the same
         * block sample for sample.
         */
        int16_t both[2 * DMR_VOCODER_PCM_SAMPLES];
        memcpy(both, first.pcm, sizeof(first.pcm));
        memcpy(both + N, second.pcm, sizeof(second.pcm));
        REQUIRE(zeroCrossings(both, 2 * N) == 239);
        REQUIRE(memcmp(first.pcm, second.pcm, sizeof(first.pcm)) == 0);

        /* No sample-to-sample jump larger than the tone's own step */
        for (size_t i = 1; i < 2 * N; i++)
            REQUIRE(abs(both[i] - both[i - 1]) <= 5821);
    }

    SECTION("The setting is read at every call")
    {
        state.settings.dmr_testTone = 0;
        PcmBuf third;
        REQUIRE(v->decode(in.ambe, third.pcm) == false);
        REQUIRE(rms(third.pcm, N) == 0.0);
        REQUIRE(third.guardsIntact());
    }

    state.settings.dmr_testTone = 0;
    v->terminate();
}

TEST_CASE("Loopback backend round trip of a tone", "[dmr][vocoder]")
{
    const struct dmrVocoder *v = &dmrVocoder_loopback;
    REQUIRE(v->init() == 0);
    REQUIRE(strcmp(v->name, "loopback") == 0);

    PcmBuf in;
    AmbeBuf ambe;
    PcmBuf out;

    SECTION("1 kHz at -12 dBFS")
    {
        fillTone(in.pcm, N, 1000.0, 8231.0);
        REQUIRE(v->encode(in.pcm, ambe.ambe) == true);
        REQUIRE(ambe.guardsIntact());
        REQUIRE(memcmp(ambe.ambe, dmr_ambeSilence, A) != 0);

        REQUIRE(v->decode(ambe.ambe, out.pcm) == true);
        REQUIRE(out.guardsIntact());

        double rIn = rms(in.pcm, N);
        double rOut = rms(out.pcm, N);
        REQUIRE(fabs(rOut - rIn) < 0.05 * rIn);
        REQUIRE(dominantBin(out.pcm, N) == 1000.0);
        REQUIRE(dftMagnitude(out.pcm, N, 1000.0)
                > 10.0 * dftMagnitude(out.pcm, N, 700.0));
    }

    SECTION("433 Hz, a tone between two DFT bins, loud")
    {
        fillTone(in.pcm, N, 433.0, 20000.0);
        REQUIRE(v->encode(in.pcm, ambe.ambe) == true);
        REQUIRE(v->decode(ambe.ambe, out.pcm) == true);

        double rIn = rms(in.pcm, N);
        double rOut = rms(out.pcm, N);
        REQUIRE(fabs(rOut - rIn) < 0.05 * rIn);

        /* The encoder measures the tone within a few Hz: bin 26 is 433.3 */
        double fOut = (double)zeroCrossings(out.pcm, N) * FS / (2.0 * N);
        REQUIRE(fabs(fOut - 433.0) < 20.0);
        REQUIRE(dominantBin(out.pcm, N) == 26.0 * FS / (double)N);
        REQUIRE(ambe.guardsIntact());
        REQUIRE(out.guardsIntact());
    }

    SECTION("Silence encodes to something the decoder plays as silence")
    {
        memset(in.pcm, 0x00, sizeof(in.pcm));
        REQUIRE(v->encode(in.pcm, ambe.ambe) == true);
        memset(out.pcm, 0x33, sizeof(out.pcm));
        REQUIRE(v->decode(ambe.ambe, out.pcm) == true);
        REQUIRE(rms(out.pcm, N) == 0.0);
    }

    SECTION("An envelope survives the round trip")
    {
        /* Tone in the first half of the block only */
        fillTone(in.pcm, N, 1000.0, 16000.0);
        memset(in.pcm + N / 2, 0x00, (N / 2) * sizeof(int16_t));
        REQUIRE(v->encode(in.pcm, ambe.ambe) == true);
        REQUIRE(v->decode(ambe.ambe, out.pcm) == true);

        double rFirst = rms(out.pcm, N / 2);
        double rSecond = rms(out.pcm + N / 2, N / 2);
        REQUIRE(rFirst > 10000.0);
        REQUIRE(rSecond == 0.0);
    }

    SECTION("Phase continuity across consecutive payloads")
    {
        fillTone(in.pcm, N, 1000.0, 8000.0);
        REQUIRE(v->encode(in.pcm, ambe.ambe) == true);

        PcmBuf second;
        REQUIRE(v->decode(ambe.ambe, out.pcm) == true);
        REQUIRE(v->decode(ambe.ambe, second.pcm) == true);

        /*
         * Same reasoning as for the null backend tone, with one crossing
         * of tolerance for a sample landing on zero either way in the
         * float sine.
         */
        int16_t both[2 * DMR_VOCODER_PCM_SAMPLES];
        memcpy(both, out.pcm, sizeof(out.pcm));
        memcpy(both + N, second.pcm, sizeof(second.pcm));
        size_t single = zeroCrossings(out.pcm, N);
        REQUIRE(single >= 118);
        REQUIRE(single <= 120);
        size_t joined = zeroCrossings(both, 2 * N);
        REQUIRE(joined >= 2 * single);
        REQUIRE(joined <= 2 * single + 1);
        for (size_t i = 1; i < 2 * N; i++)
            REQUIRE(abs(both[i] - both[i - 1]) <= 6000);
    }

    v->terminate();
}

TEST_CASE("Loopback backend rejects what it did not encode", "[dmr][vocoder]")
{
    const struct dmrVocoder *v = &dmrVocoder_loopback;
    REQUIRE(v->init() == 0);

    AmbeBuf ambe;
    PcmBuf out;

    SECTION("The AMBE silence pattern")
    {
        memcpy(ambe.ambe, dmr_ambeSilence, A);
        memset(out.pcm, 0x77, sizeof(out.pcm));
        REQUIRE(v->decode(ambe.ambe, out.pcm) == false);
        REQUIRE(rms(out.pcm, N) == 0.0);
        REQUIRE(out.guardsIntact());
    }

    SECTION("A corrupted payload of its own")
    {
        dmrVocoderLoopback_tonePayload(1000, 8000, ambe.ambe);
        REQUIRE(ambe.guardsIntact());
        REQUIRE(v->decode(ambe.ambe, out.pcm) == true);
        REQUIRE(rms(out.pcm, N) > 5000.0);

        ambe.ambe[3] ^= 0x01;
        REQUIRE(v->decode(ambe.ambe, out.pcm) == false);
        REQUIRE(rms(out.pcm, N) == 0.0);
    }

    SECTION("All zeros")
    {
        memset(ambe.ambe, 0x00, A);
        REQUIRE(v->decode(ambe.ambe, out.pcm) == false);
        REQUIRE(rms(out.pcm, N) == 0.0);
    }

    v->terminate();
}

TEST_CASE("Loopback tone payload helper", "[dmr][vocoder]")
{
    const struct dmrVocoder *v = &dmrVocoder_loopback;
    REQUIRE(v->init() == 0);

    AmbeBuf ambe;
    PcmBuf out;

    dmrVocoderLoopback_tonePayload(1000, 8231, ambe.ambe);
    REQUIRE(v->decode(ambe.ambe, out.pcm) == true);
    REQUIRE(dominantBin(out.pcm, N) == 1000.0);
    double r = rms(out.pcm, N);
    REQUIRE(r > 5500.0);
    REQUIRE(r < 6100.0);

    dmrVocoderLoopback_tonePayload(0, 8231, ambe.ambe);
    REQUIRE(v->decode(ambe.ambe, out.pcm) == true);
    REQUIRE(rms(out.pcm, N) == 0.0);

    REQUIRE(ambe.guardsIntact());
    REQUIRE(out.guardsIntact());
    v->terminate();
}
