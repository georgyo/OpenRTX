/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_FEC_EMBEDDEDLC_H
#define DMR_FEC_EMBEDDEDLC_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <cstdint>

namespace DMR
{
namespace FEC
{

/**
 * Variable length BPTC for the embedded Full LC, ETSI TS 102 361-1 V2.5.1
 * clause B.2.1 figure B.3 and clause 7.1.3.
 *
 * The 72 LC bits and the 5-bit checksum of clause B.3.11 fill a 7 x 11
 * matrix, each row is protected by a Hamming (16,11,4) code and an eighth
 * row holds an even parity check of every column. The 8 x 16 encode matrix
 * is read column by column, top to bottom and left to right, into four
 * 32-bit fragments which are carried by the embedded signalling field of
 * voice bursts B to E of a superframe (clause 7.1.3, LCSS framing in the
 * EMB field).
 *
 * Fragment bits are stored MSB first: TX(31) of figure B.3 is bit 31.
 */
namespace EmbeddedLc
{

/**
 * Encode a Full LC into its four embedded signalling fragments.
 *
 * @param lc: 72-bit Full LC, LC(71) first.
 * @param fragments: destination for the four 32-bit fragments, in burst
 * order.
 */
void encode(const uint8_t lc[9], uint32_t fragments[4]);

/**
 * Reassemble and decode four embedded signalling fragments into a Full LC,
 * correcting a single bit error per row and verifying the 5-bit checksum.
 * Odd columns left after row correction are counted as errors in the
 * column parity row, which carries no information.
 *
 * @param fragments: the four 32-bit fragments, in burst order.
 * @param lc: destination for the 72-bit Full LC.
 * @return number of corrected bit errors or -1 if the message cannot be
 * decoded, in which case lc is left untouched.
 */
int decode(const uint32_t fragments[4], uint8_t lc[9]);

} // namespace EmbeddedLc
} // namespace FEC
} // namespace DMR

#endif // DMR_FEC_EMBEDDEDLC_H
