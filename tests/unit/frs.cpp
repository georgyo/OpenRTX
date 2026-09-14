/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstring>

extern "C" {
#include "core/frs.h"
#include "interfaces/platform.h"
}

/* 47 CFR 95.563, channels 1-22 */
static const freq_t expected_freq[FRS_CHANNEL_NUM] = {
    462562500, 462587500, 462612500, 462637500, 462662500, 462687500,
    462712500, 467562500, 467587500, 467612500, 467637500, 467662500,
    467687500, 467712500, 462550000, 462575000, 462600000, 462625000,
    462650000, 462675000, 462700000, 462725000
};

/* Motorola/Midland/Cobra/Uniden privacy codes 1-38, in tenths of Hz */
static const uint16_t expected_tone[FRS_CODE_NUM] = {
    670,  719,  744,  770,  797,  825,  854,  885,  915,  948,
    974,  1000, 1035, 1072, 1109, 1148, 1188, 1230, 1273, 1318,
    1365, 1413, 1462, 1514, 1567, 1622, 1679, 1738, 1799, 1862,
    1928, 2035, 2107, 2181, 2257, 2336, 2418, 2503
};

TEST_CASE("FRS channel frequencies", "[frs]")
{
    for (uint8_t ch = 0; ch < FRS_CHANNEL_NUM; ch++) {
        INFO("Channel " << (int)(ch + 1));
        REQUIRE(frs_channel_freq[ch] == expected_freq[ch]);
        REQUIRE(frs_getFrequency(ch) == expected_freq[ch]);
        /* 12.5 kHz raster */
        REQUIRE(frs_getFrequency(ch) % 12500 == 0);
    }

    REQUIRE(frs_getFrequency(FRS_CHANNEL_NUM) == 0);
    REQUIRE(frs_getFrequency(255) == 0);
}

TEST_CASE("FRS channel power limits", "[frs]")
{
    for (uint8_t ch = 0; ch < FRS_CHANNEL_NUM; ch++) {
        INFO("Channel " << (int)(ch + 1));
        if ((ch >= 7) && (ch <= 13))
            REQUIRE(frs_getPower(ch) == 500);
        else
            REQUIRE(frs_getPower(ch) == 2000);
    }
}

TEST_CASE("FRS privacy code to CTCSS tone mapping", "[frs]")
{
    for (uint8_t code = 1; code <= FRS_CODE_NUM; code++) {
        INFO("Code " << (int)code);
        uint8_t idx = frs_codeToCtcss(code);
        REQUIRE(idx == frs_code_ctcss[code - 1]);
        REQUIRE(idx < CTCSS_FREQ_NUM);
        REQUIRE(ctcss_tone[idx] == expected_tone[code - 1]);
    }

    /* Tones must be strictly increasing, no duplicate codes */
    for (uint8_t code = 2; code <= FRS_CODE_NUM; code++)
        REQUIRE(frs_code_ctcss[code - 1] > frs_code_ctcss[code - 2]);

    REQUIRE(frs_codeToCtcss(0) == 0);
    REQUIRE(frs_codeToCtcss(FRS_CODE_NUM + 1) == 0);
    REQUIRE(frs_codeToCtcss(255) == 0);
}

TEST_CASE("FRS channel descriptor", "[frs]")
{
    SECTION("Channel 12 with code 12")
    {
        channel_t ch = frs_buildChannel(11, 12);
        REQUIRE(ch.mode == OPMODE_FM);
        REQUIRE(ch.bandwidth == BW_12_5);
        REQUIRE(ch.rx_only == 0);
        REQUIRE(ch.rx_frequency == 467662500);
        REQUIRE(ch.tx_frequency == 467662500);
        REQUIRE(ch.power == 500);
        REQUIRE(ch.fm.txTone == 12);
        REQUIRE(ch.fm.rxTone == 12);
        REQUIRE(ch.fm.txToneEn == 1);
        /* The Linux emulator has no tone decoder: TX-only tone */
        REQUIRE(ch.fm.rxToneEn == 0);
        REQUIRE(strcmp(ch.name, "FRS 12") == 0);
    }

    SECTION("Channel 1 without code")
    {
        channel_t ch = frs_buildChannel(0, 0);
        REQUIRE(ch.rx_frequency == 462562500);
        REQUIRE(ch.tx_frequency == 462562500);
        REQUIRE(ch.power == 2000);
        REQUIRE(ch.fm.txToneEn == 0);
        REQUIRE(ch.fm.rxToneEn == 0);
        REQUIRE(ch.fm.txTone == 0);
        REQUIRE(strcmp(ch.name, "FRS 1") == 0);
    }

    SECTION("Out of range code is treated as off")
    {
        channel_t ch = frs_buildChannel(0, FRS_CODE_NUM + 1);
        REQUIRE(ch.fm.txToneEn == 0);
        REQUIRE(ch.fm.txTone == 0);
    }
}

TEST_CASE("FRS hardware support check", "[frs]")
{
    /* The Linux emulator declares a 400-480 MHz UHF band */
    REQUIRE(frs_isSupported(platform_getHwInfo()) == true);

    hwInfo_t hw;
    memset(&hw, 0, sizeof(hw));
    hw.uhf_band = 1;
    hw.uhf_minFreq = 400;
    hw.uhf_maxFreq = 480;
    REQUIRE(frs_isSupported(&hw) == true);

    hw.uhf_maxFreq = 450;
    REQUIRE(frs_isSupported(&hw) == false);

    hw.uhf_maxFreq = 480;
    hw.uhf_minFreq = 470;
    REQUIRE(frs_isSupported(&hw) == false);

    hw.uhf_minFreq = 400;
    hw.uhf_band = 0;
    hw.vhf_band = 1;
    REQUIRE(frs_isSupported(&hw) == false);

    REQUIRE(frs_isSupported(NULL) == false);
}
