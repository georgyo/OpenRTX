/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/DMR/FEC/Golay20_8.hpp"

using namespace DMR::FEC;

/*
 * Parity part of the Golay (20,8) generator matrix, ETSI TS 102 361-1
 * table B.11. Entry i holds the parity bits contributed by data bit i, with
 * i = 0 being the LSB (last row of the table).
 */
static const uint16_t encodeMatrix[8] = {
    0x8EB, 0x93E, 0xA97, 0xDC6, 0x367, 0x6CD, 0xD99, 0x3DA,
};

uint16_t Golay20_8::parity(uint8_t data)
{
    uint16_t p = 0;

    for (uint8_t i = 0; i < 8; i++) {
        if (data & (1 << i))
            p ^= encodeMatrix[i];
    }

    return p;
}

uint32_t Golay20_8::encode(uint8_t data)
{
    return (static_cast<uint32_t>(data) << 12) | parity(data);
}

/*
 * Bounded-distance decoder: the syndrome of a received word is the parity
 * of the received data field XORed with the received parity field. An error
 * pattern touching data bits {i, j, k} and some parity bits produces the
 * syndrome encodeMatrix[i] ^ encodeMatrix[j] ^ encodeMatrix[k] ^ parityErrors.
 * Since the minimum distance is 7, at most one pattern of weight <= 3 has a
 * given syndrome, so the first match found below is the correct one.
 */
int Golay20_8::decode(uint32_t codeword, uint8_t &data)
{
    uint8_t rxData = (codeword >> 12) & 0xFF;
    uint16_t syndrome = (codeword & 0xFFF) ^ parity(rxData);

    // Errors confined to the parity field
    if (__builtin_popcount(syndrome) <= 3) {
        data = rxData;
        return __builtin_popcount(syndrome);
    }

    // One data bit in error, up to two parity bits
    for (uint8_t i = 0; i < 8; i++) {
        uint16_t s = syndrome ^ encodeMatrix[i];
        if (__builtin_popcount(s) <= 2) {
            data = rxData ^ (1 << i);
            return 1 + __builtin_popcount(s);
        }
    }

    // Two data bits in error, up to one parity bit
    for (uint8_t i = 0; i < 8; i++) {
        for (uint8_t j = i + 1; j < 8; j++) {
            uint16_t s = syndrome ^ encodeMatrix[i] ^ encodeMatrix[j];
            if (__builtin_popcount(s) <= 1) {
                data = rxData ^ (1 << i) ^ (1 << j);
                return 2 + __builtin_popcount(s);
            }
        }
    }

    // Three data bits in error
    for (uint8_t i = 0; i < 8; i++) {
        for (uint8_t j = i + 1; j < 8; j++) {
            for (uint8_t k = j + 1; k < 8; k++) {
                uint16_t s = encodeMatrix[i] ^ encodeMatrix[j]
                           ^ encodeMatrix[k];
                if (s == syndrome) {
                    data = rxData ^ (1 << i) ^ (1 << j) ^ (1 << k);
                    return 3;
                }
            }
        }
    }

    return -1;
}
