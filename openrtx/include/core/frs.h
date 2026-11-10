/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef FRS_H
#define FRS_H

#include <stdbool.h>
#include <stdint.h>
#include "core/cps.h"
#include "core/settings.h"
#include "interfaces/platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Family Radio Service (FRS) channel plan, 47 CFR 95 subpart B.
 *
 * The 22 channels are narrowband FM (12.5 kHz channel spacing, 2.5 kHz peak
 * deviation, 95.573/95.575), simplex only. Channels 1-7 and 15-22 may run up
 * to 2 W ERP, channels 8-14 are limited to 0.5 W ERP (95.567). Every OpenRTX
 * hardware driver clamps the requested TX power to the 1-5 W range of the RF
 * power amplifier, so channels 8-14 transmit at the 1 W driver floor: the
 * 500 mW request is kept in the channel so that the UI can show the correct
 * power class, but it is not enforced by the RF driver.
 *
 * The "privacy codes" are the CTCSS tone numbers used by Motorola, Midland,
 * Cobra and Uniden FRS radios: code 0 is off, codes 1-38 select a CTCSS tone.
 * Codes 39-121 of the same numbering are DCS codes, which OpenRTX does not
 * support yet and are therefore not implemented.
 */

/*
 * FRS_CHANNEL_NUM, the number of FRS channels (numbered 1..22 on the radio,
 * stored 0-based), is defined in core/settings.h: settings_t keeps one
 * privacy code per channel.
 */

/** Number of CTCSS privacy codes, numbered 1..38 (0 = no code). */
#define FRS_CODE_NUM 38

/**
 * FRS channel frequencies, in Hz, indexed by 0-based channel number.
 */
extern const freq_t frs_channel_freq[FRS_CHANNEL_NUM];

/**
 * CTCSS tone index (into ctcss_tone[]) of each privacy code, indexed by
 * code - 1.
 */
extern const uint8_t frs_code_ctcss[FRS_CODE_NUM];

/**
 * Get the frequency of an FRS channel.
 *
 * @param channel: 0-based channel number.
 * @return channel frequency in Hz, or zero if the channel is out of range.
 */
freq_t frs_getFrequency(uint8_t channel);

/**
 * Get the maximum transmit power allowed on an FRS channel.
 *
 * @param channel: 0-based channel number.
 * @return power limit in mW: 2000 for channels 1-7 and 15-22, 500 for
 * channels 8-14.
 */
uint32_t frs_getPower(uint8_t channel);

/**
 * Convert a privacy code to the corresponding entry of the ctcss_tone[]
 * table.
 *
 * @param code: privacy code, 1..38.
 * @return index into ctcss_tone[], or zero when the code is zero (off) or out
 * of range.
 */
uint8_t frs_codeToCtcss(uint8_t code);

/**
 * Check whether the radio hardware can operate on the FRS channels.
 *
 * @param hw: hardware information of the radio.
 * @return true if the UHF band of the radio covers 462-468 MHz.
 */
bool frs_isSupported(const hwInfo_t *hw);

/**
 * Build the channel descriptor for an FRS channel: narrowband FM, simplex,
 * with the channel power limit and the CTCSS tone of the given privacy code.
 *
 * @param channel: 0-based channel number.
 * @param code: privacy code, 0 for none.
 * @return channel descriptor.
 */
channel_t frs_buildChannel(uint8_t channel, uint8_t code);

#ifdef __cplusplus
}
#endif

#endif /* FRS_H */
