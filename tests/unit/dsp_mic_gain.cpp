/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include "core/dsp.h"

// Same gain the hwconfig files of the 12-bit ADC targets use.
static constexpr int32_t MIC_GAIN = 32;

TEST_CASE("Saturation keeps in-range values untouched", "[dsp][saturate]")
{
    REQUIRE(dsp_saturate16(0) == 0);
    REQUIRE(dsp_saturate16(1234) == 1234);
    REQUIRE(dsp_saturate16(-1234) == -1234);
    REQUIRE(dsp_saturate16(INT16_MAX) == INT16_MAX);
    REQUIRE(dsp_saturate16(INT16_MIN) == INT16_MIN);
}

TEST_CASE("Saturation clamps out-of-range values", "[dsp][saturate]")
{
    REQUIRE(dsp_saturate16(INT16_MAX + 1) == INT16_MAX);
    REQUIRE(dsp_saturate16(INT16_MIN - 1) == INT16_MIN);
    REQUIRE(dsp_saturate16(2047 * MIC_GAIN) == INT16_MAX);
    REQUIRE(dsp_saturate16(-2048 * MIC_GAIN) == INT16_MIN);
    REQUIRE(dsp_saturate16(INT32_MAX) == INT16_MAX);
    REQUIRE(dsp_saturate16(INT32_MIN) == INT16_MIN);
}

TEST_CASE("Mic gain stage saturates a full-scale 12-bit sine without wrapping",
          "[dsp][saturate]")
{
    struct dcBlock dcb;
    dsp_resetState(dcb);

    // Full-scale 12-bit ADC samples: 2047 amplitude on top of mid-scale DC.
    const int samples = 8000;
    int positive = 0;
    int negative = 0;
    int clipped = 0;

    for (int i = 0; i < samples; i++) {
        double phase = 2.0 * M_PI * 1000.0 * i / 8000.0;
        int16_t raw = (int16_t)(2048 + std::lround(2047.0 * std::sin(phase)));
        int16_t sample = dsp_dcBlockFilter(&dcb, raw);
        int32_t gained = (int32_t)sample * MIC_GAIN;
        int16_t out = dsp_saturate16(gained);

        // Skip the DC blocker settling time, then check the sign is preserved
        // (a wrapped sample would flip it) and that peaks land on the rails.
        if (i < 1000)
            continue;

        if (gained > 0) {
            REQUIRE(out > 0);
            positive++;
        } else if (gained < 0) {
            REQUIRE(out < 0);
            negative++;
        }

        if ((out == INT16_MAX) || (out == INT16_MIN))
            clipped++;
    }

    REQUIRE(positive > 0);
    REQUIRE(negative > 0);
    REQUIRE(clipped > 0);
}
