/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_FEC_BPTC196_96_H
#define DMR_FEC_BPTC196_96_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <cstdint>

namespace DMR
{
namespace FEC
{

/**
 * Block Product Turbo Code (196,96), ETSI TS 102 361-1 V2.5.1 clause B.1.1.
 *
 * The 96 information bits I(95)..I(0) and three reserved bits R(2)..R(0) are
 * arranged in a 9 x 11 matrix (figure B.1), each row is protected by a
 * Hamming (15,11,3) code and each column by a Hamming (13,9,3) code. A
 * fourth reserved bit R(3) is prepended and the resulting 196 bits are
 * interleaved with Interleave Index = Index x 181 mod 196 (equation B.1,
 * tables B.2 and B.3) before being placed in the 196-bit payload of a
 * general data burst (clause 6.2).
 *
 * Bit strings are handled as packed octets, MSB first: I(95) is bit 7 of
 * the first information octet and TX(195) is bit 7 of the first transmit
 * octet. The 196-bit transmit array occupies 25 octets, the last four bits
 * of which are unused and set to zero.
 */
namespace Bptc196_96
{

/// Number of octets holding the 196-bit transmit array.
static constexpr uint8_t TX_BYTES = 25;

/// Number of octets holding the 96 information bits.
static constexpr uint8_t DATA_BYTES = 12;

/**
 * Interleaving function of equation (B.1).
 *
 * @param index: index of a bit in the encoder matrix, 0 to 195.
 * @return index of the same bit in the transmit array.
 */
inline uint8_t interleaveIndex(uint8_t index)
{
    return (static_cast<uint16_t>(index) * 181) % 196;
}

/**
 * Encode 96 information bits into the 196-bit transmit array.
 *
 * @param data: 96 information bits, I(95) first.
 * @param tx: destination for the 196-bit transmit array, TX(195) first.
 */
void encode(const uint8_t data[DATA_BYTES], uint8_t tx[TX_BYTES]);

/**
 * Decode a 196-bit transmit array, correcting bit errors with iterative
 * row and column decoding.
 *
 * @param tx: 196-bit transmit array, TX(195) first.
 * @param data: destination for the 96 information bits, I(95) first.
 * @return number of corrected bit errors or -1 if the array cannot be
 * corrected, in which case data is left untouched.
 */
int decode(const uint8_t tx[TX_BYTES], uint8_t data[DATA_BYTES]);

/**
 * Extract the 196-bit transmit array from a general data burst (clause
 * 6.2 figure 6.5): the two 98-bit info halves surrounding the Slot Type and
 * SYNC fields.
 *
 * @param burst: 264-bit burst, first transmitted bit in the MSB.
 * @param tx: destination for the 196-bit transmit array.
 */
void fromBurst(const uint8_t burst[33], uint8_t tx[TX_BYTES]);

} // namespace Bptc196_96
} // namespace FEC
} // namespace DMR

#endif // DMR_FEC_BPTC196_96_H
