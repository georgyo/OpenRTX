/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/DMR/FEC/QR16_7_6.hpp"

using namespace DMR::FEC;

/*
 * Parity part of the quadratic residue (16,7,6) generator matrix, ETSI
 * TS 102 361-1 table B.12. Entry i holds the parity bits contributed by data
 * bit i, with i = 0 being the LSB (last row of the table).
 */
static const uint16_t encodeMatrix[7] = {
    0x073, 0x0E5, 0x1C9, 0x1E2, 0x1B7, 0x11E, 0x04F,
};

uint16_t QR16_7_6::parity(uint8_t data)
{
    uint16_t p = 0;

    for (uint8_t i = 0; i < 7; i++) {
        if (data & (1 << i))
            p ^= encodeMatrix[i];
    }

    return p;
}

uint16_t QR16_7_6::encode(uint8_t data)
{
    return (static_cast<uint16_t>(data & 0x7F) << 9) | parity(data);
}

/*
 * Bounded-distance decoder, see Golay20_8::decode() for the rationale: with
 * a minimum distance of 6 every error pattern of weight <= 2 has a unique
 * syndrome.
 */
int QR16_7_6::decode(uint16_t codeword, uint8_t &data)
{
    uint8_t rxData = (codeword >> 9) & 0x7F;
    uint16_t syndrome = (codeword & 0x1FF) ^ parity(rxData);

    // Errors confined to the parity field
    if (__builtin_popcount(syndrome) <= 2) {
        data = rxData;
        return __builtin_popcount(syndrome);
    }

    // One data bit in error, up to one parity bit
    for (uint8_t i = 0; i < 7; i++) {
        uint16_t s = syndrome ^ encodeMatrix[i];
        if (__builtin_popcount(s) <= 1) {
            data = rxData ^ (1 << i);
            return 1 + __builtin_popcount(s);
        }
    }

    // Two data bits in error
    for (uint8_t i = 0; i < 7; i++) {
        for (uint8_t j = i + 1; j < 7; j++) {
            if ((encodeMatrix[i] ^ encodeMatrix[j]) == syndrome) {
                data = rxData ^ (1 << i) ^ (1 << j);
                return 2;
            }
        }
    }

    return -1;
}
