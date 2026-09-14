/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <string.h>
#include "core/frs.h"

/*
 * Targets whose radio_checkRxDigitalSquelch() always returns false: with a
 * decode-enabled tone the FM squelch would never open (OpMode_FM.cpp), so on
 * these radios the privacy code is transmitted only.
 */
#if defined(PLATFORM_MD3x0) || defined(PLATFORM_MD9600) \
    || defined(PLATFORM_LINUX)
#define FRS_RX_TONE_DECODE 0
#else
#define FRS_RX_TONE_DECODE 1
#endif

const freq_t frs_channel_freq[FRS_CHANNEL_NUM] = {
    462562500, 462587500, 462612500, 462637500,
    462662500, 462687500, 462712500,           /* 1-7   */
    467562500, 467587500, 467612500, 467637500,
    467662500, 467687500, 467712500,           /* 8-14  */
    462550000, 462575000, 462600000, 462625000,
    462650000, 462675000, 462700000, 462725000 /* 15-22 */
};

/*
 * Privacy code n -> ctcss_tone[] index. The 38 codes skip the 69.3, 159.8,
 * 165.5, 171.3, 177.3, 183.5, 189.9, 196.6, 199.5, 206.5, 229.1 and 254.1 Hz
 * tones of the OpenRTX table, so the mapping cannot be a plain offset.
 */
const uint8_t frs_code_ctcss[FRS_CODE_NUM] = {
    0,  2,  3,  4,  5,  6,  7,  8,  9,  10, /*  1-10:  67.0 -  94.8 Hz */
    11, 12, 13, 14, 15, 16, 17, 18, 19, 20, /* 11-20:  97.4 - 131.8 Hz */
    21, 22, 23, 24, 25, 27, 29, 31, 33, 35, /* 21-30: 136.5 - 186.2 Hz */
    37, 40, 42, 43, 44, 46, 47, 48          /* 31-38: 192.8 - 250.3 Hz */
};

freq_t frs_getFrequency(uint8_t channel)
{
    if (channel >= FRS_CHANNEL_NUM)
        return 0;

    return frs_channel_freq[channel];
}

uint32_t frs_getPower(uint8_t channel)
{
    /* Channels 8-14 (0-based 7..13) are the 0.5 W ERP interstitials. */
    if ((channel >= 7) && (channel <= 13))
        return 500;

    return 2000;
}

uint8_t frs_codeToCtcss(uint8_t code)
{
    if ((code == 0) || (code > FRS_CODE_NUM))
        return 0;

    return frs_code_ctcss[code - 1];
}

bool frs_isSupported(const hwInfo_t *hw)
{
    if ((hw == NULL) || (hw->uhf_band == 0))
        return false;

    /* hwInfo_t band limits are in MHz: 462.5500 - 467.7125 MHz needed. */
    return (hw->uhf_minFreq <= 462) && (hw->uhf_maxFreq >= 468);
}

channel_t frs_buildChannel(uint8_t channel, uint8_t code)
{
    channel_t ch;

    memset(&ch, 0, sizeof(ch));
    ch.mode = OPMODE_FM;
    ch.bandwidth = BW_12_5;
    ch.rx_only = 0;
    ch.power = frs_getPower(channel);
    ch.rx_frequency = frs_getFrequency(channel);
    ch.tx_frequency = ch.rx_frequency;
    sniprintf(ch.name, CPS_STR_SIZE, "FRS %d", channel + 1);

    ch.fm.txTone = frs_codeToCtcss(code);
    ch.fm.rxTone = ch.fm.txTone;
    ch.fm.txToneEn = (code != 0) && (code <= FRS_CODE_NUM);
    ch.fm.rxToneEn = ch.fm.txToneEn && FRS_RX_TONE_DECODE;

    return ch;
}
