/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_FEC_CRC_H
#define DMR_FEC_CRC_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <cstddef>
#include <cstdint>

namespace DMR
{
namespace FEC
{

/**
 * Cyclic redundancy checks and checksums of ETSI TS 102 361-1 V2.5.1
 * clauses B.3.7, B.3.8, B.3.11 and B.3.12.
 */
namespace Crc
{

/**
 * Data Type CRC masks, clause B.3.12 table B.21. The mask is XORed with the
 * computed CRC before the FEC encoding on transmission and after the FEC
 * decoding on reception. The 24-bit masks apply to the Reed-Solomon (12,9)
 * parity of the Full LC (clauses 7.1.1 and 7.1.2), the 16-bit ones to the
 * CRC-CCITT of the corresponding data or control bursts.
 */
enum Mask : uint32_t {
    MASK_PI_HEADER = 0x6969,
    MASK_VOICE_LC_HEADER = 0x969696,
    MASK_TERMINATOR_WITH_LC = 0x999999,
    MASK_CSBK = 0xA5A5,
    MASK_MBC_HEADER = 0xAAAA,
    MASK_DATA_HEADER = 0xCCCC,
    MASK_USBD = 0x3333,
    MASK_RATE_1_2_DATA = 0x0F0,
    MASK_RATE_3_4_DATA = 0x1FF,
    MASK_RATE_1_DATA = 0x10F,
    MASK_REVERSE_CHANNEL = 0x7A,
};

/**
 * 8-bit CRC, clause B.3.7: remainder of x^8 M(x) divided by
 * G8(x) = x^8 + x^2 + x + 1, no initial value and no inversion. Used by the
 * 4-burst CACH Short LC (28-bit message) and by the Hash Address of the
 * Activity Update Short LC (24-bit destination address, TS 102 361-2).
 *
 * @param message: message bits, right aligned, MSB corresponding to the
 * highest-degree term of M(x).
 * @param nBits: number of message bits, at most 32.
 * @return 8-bit CRC.
 */
uint8_t crc8Bits(uint32_t message, uint8_t nBits);

/**
 * 8-bit CRC of clause B.3.7 over a sequence of octets, the MSB of the first
 * octet being the highest-degree term of M(x).
 *
 * @param data: message octets.
 * @param len: number of message octets.
 * @return 8-bit CRC.
 */
uint8_t crc8(const uint8_t *data, size_t len);

/**
 * CRC-CCITT, clause B.3.8: FH(x) = (x^16 M(x) mod GH(x)) + IH(x) with
 * GH(x) = x^16 + x^12 + x^5 + 1, an initial remainder of 0x0000 and the
 * inversion polynomial IH(x) = all ones (XOR with 0xFFFF).
 *
 * @param data: message octets, the MSB of the first octet being the
 * highest-degree term of M(x).
 * @param len: number of message octets.
 * @param mask: optional Data Type CRC mask (clause B.3.12), zero for none.
 * @return 16-bit CRC, MSB first in the CRC field.
 */
uint16_t crcCcitt(const uint8_t *data, size_t len, uint16_t mask = 0);

/**
 * 5-bit checksum of the embedded Full LC, clause B.3.11: sum of the nine LC
 * octets modulo 31.
 *
 * @param lc: 72-bit Full LC.
 * @return checksum in the range 0 to 30.
 */
uint8_t checksum5(const uint8_t lc[9]);

} // namespace Crc
} // namespace FEC
} // namespace DMR

#endif // DMR_FEC_CRC_H
