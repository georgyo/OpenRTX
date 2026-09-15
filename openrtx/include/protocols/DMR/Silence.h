/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_SILENCE_H
#define DMR_SILENCE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bytes of one DMR voice payload: 216 bits, three AMBE+2 frames (ETSI TS 102
 * 361-1 §6.1, Figure 6.2). The same value as DMR::VOICE_PAYLOAD_BYTES of
 * Constants.hpp, repeated here so that C code (the vocoder backends) can use
 * the silence pattern without the C++ header.
 */
#define DMR_AMBE_SILENCE_BYTES 27

/**
 * AMBE+2 silence pattern: one burst of already encoded "silence" frames, what
 * a voice burst carries when no vocoder output is available. Defined in
 * Silence.c, see the attribution there.
 */
extern const uint8_t dmr_ambeSilence[DMR_AMBE_SILENCE_BYTES];

#ifdef __cplusplus
}
#endif

#endif /* DMR_SILENCE_H */
