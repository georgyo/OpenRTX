/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_FEC_SHORTLC_H
#define DMR_FEC_SHORTLC_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <cstdint>

namespace DMR
{
namespace FEC
{

/**
 * Variable length BPTC for the Short Link Control carried by the CACH,
 * ETSI TS 102 361-1 V2.5.1 clauses 7.1.4, 9.1.7 and B.2.3 figure B.6.
 *
 * The 28-bit Short LC (4-bit SLCO followed by 24 bits of data, clause
 * 9.1.7) and its 8-bit CRC (clause B.3.7) fill three rows of 12 bits, each
 * protected by a Hamming (17,12,3) code, plus a fourth row of even column
 * parity: a 4 x 17 encode matrix read column by column, top to bottom and
 * left to right, into four 17-bit fragments carried by the payload of four
 * consecutive CACH bursts (figure 7.7).
 *
 * Fragment bits are stored right aligned, MSB first: the first bit of a
 * CACH payload is bit 16 of its fragment.
 *
 * The HR_C6000 decodes the BPTC itself and hands the MCU the 28 bits and
 * the CRC in its RX RAM (manual, RAM map "Short LC 0x24-0x28"): see
 * fromChipBytes() for that layout.
 */
namespace ShortLc
{

/**
 * Number of CACH bursts, and fragments, of one Short LC message.
 */
static constexpr uint8_t FRAGMENTS = 4;

/**
 * Bits of a fragment, the payload of one CACH burst.
 */
static constexpr uint8_t FRAGMENT_BITS = 17;

/**
 * Mask of the 28 bits of a Short LC message.
 */
static constexpr uint32_t MESSAGE_MASK = 0x0FFFFFFF;

/**
 * Bytes of the Short LC as the HR_C6000 lays it out in its RX RAM.
 */
static constexpr uint8_t CHIP_BYTES = 5;

/**
 * Encode a Short LC message into its four CACH fragments, computing the
 * 8-bit CRC of clause B.3.7 over the 28 message bits.
 *
 * @param message: 28-bit Short LC, LC(27) = SLCO MSB in bit 27.
 * @param fragments: destination for the four 17-bit fragments, in burst
 * order.
 */
void encode(uint32_t message, uint32_t fragments[FRAGMENTS]);

/**
 * Reassemble and decode four CACH fragments into a Short LC message,
 * correcting a single bit error per row and verifying the CRC. Odd columns
 * left after row correction are counted as errors in the column parity row,
 * which carries no information.
 *
 * @param fragments: the four 17-bit fragments, in burst order.
 * @param message: destination for the 28-bit Short LC.
 * @return number of corrected bit errors or -1 if the message cannot be
 * decoded, in which case message is left untouched.
 */
int decode(const uint32_t fragments[FRAGMENTS], uint32_t &message);

/**
 * 8-bit CRC of a Short LC message, clause B.3.7: the 28 bits as M(x) of
 * degree 27, bit 3 of octet 0 (LC(27)) being the x^27 term.
 *
 * @param message: 28-bit Short LC.
 * @return the CRC, MSB corresponding to x^7.
 */
uint8_t crc(uint32_t message);

/**
 * Read a Short LC from the HR_C6000 RX RAM layout: bytes 0x24, 0x25, 0x26
 * carry LC(27)..LC(4), the upper nibble of 0x27 LC(3)..LC(0) and 0x28 the
 * 8-bit CRC (HR_C6000 user manual, RAM map). The chip has already undone
 * the BPTC; the CRC is verified here.
 *
 * UNVERIFIED on hardware: the manual gives this layout in its RAM map
 * table but its worked examples never read it back, and whether the chip
 * verifies the CRC on its own or leaves that to the MCU is not stated.
 * The bring-up check is listed in docs/dmr_bringup.md.
 *
 * @param bytes: the five RX RAM bytes 0x24..0x28.
 * @param message: destination for the 28-bit Short LC.
 * @return true when the CRC matches, false otherwise (message untouched).
 */
bool fromChipBytes(const uint8_t bytes[CHIP_BYTES], uint32_t &message);

/**
 * Inverse of fromChipBytes(), for the tests and the emulator: lay a
 * Short LC and its CRC out as the chip's RX RAM bytes 0x24..0x28.
 *
 * @param message: 28-bit Short LC.
 * @param bytes: destination, five bytes.
 */
void toChipBytes(uint32_t message, uint8_t bytes[CHIP_BYTES]);

} // namespace ShortLc
} // namespace FEC
} // namespace DMR

#endif // DMR_FEC_SHORTLC_H
