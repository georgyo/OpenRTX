/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/DMR/FEC/Hamming.hpp"

using namespace DMR::FEC;

/*
 * Parity parts of the generator matrices, ETSI TS 102 361-1 tables B.13 to
 * B.17. Entry i of each table holds the parity bits contributed by data bit
 * i, with i = 0 being the LSB (last row of the printed matrix).
 */

// Table B.17, Hamming (7,4,3)
static const uint8_t matrix7_4[4] = { 0x3, 0x6, 0x7, 0x5 };

// Table B.14, Hamming (13,9,3)
static const uint8_t matrix13_9[9] = {
    0x3, 0x6, 0xC, 0xB, 0x5, 0xA, 0x7, 0xE, 0xF,
};

// Table B.15, Hamming (15,11,3)
static const uint8_t matrix15_11[11] = {
    0x3, 0x6, 0xC, 0xB, 0x5, 0xA, 0x7, 0xE, 0xF, 0xD, 0x9,
};

// Table B.16, Hamming (16,11,4)
static const uint8_t matrix16_11[11] = {
    0x07, 0x0D, 0x19, 0x16, 0x0B, 0x15, 0x0E, 0x1C, 0x1F, 0x1A, 0x13,
};

// Table B.13, Hamming (17,12,3)
static const uint8_t matrix17_12[12] = {
    0x05, 0x0A, 0x14, 0x0D, 0x1A, 0x11, 0x07, 0x0E, 0x1C, 0x1D, 0x1F, 0x1B,
};

const Hamming::Code Hamming::H7_4 = { 4, 3, false, matrix7_4 };
const Hamming::Code Hamming::H13_9 = { 9, 4, false, matrix13_9 };
const Hamming::Code Hamming::H15_11 = { 11, 4, false, matrix15_11 };
const Hamming::Code Hamming::H16_11 = { 11, 5, true, matrix16_11 };
const Hamming::Code Hamming::H17_12 = { 12, 5, false, matrix17_12 };

uint8_t Hamming::parity(const Code &code, uint16_t data)
{
    uint8_t p = 0;

    for (uint8_t i = 0; i < code.dataBits; i++) {
        if (data & (1 << i))
            p ^= code.matrix[i];
    }

    return p;
}

uint32_t Hamming::encode(const Code &code, uint16_t data)
{
    uint32_t d = data & ((1u << code.dataBits) - 1);

    return (d << code.parityBits) | parity(code, d);
}

/*
 * Syndrome decoder: the syndrome is the received parity field XORed with the
 * parity of the received data field. A single error in data bit i yields the
 * syndrome matrix[i], a single error in parity bit j yields 1 << j.
 *
 * For the extended (16,11,4) code every codeword has even weight, hence an
 * odd number of errors leaves the received word with odd weight: a nonzero
 * syndrome together with an even weight is a double error, which is reported
 * as uncorrectable instead of being miscorrected.
 */
int Hamming::decode(const Code &code, uint32_t &codeword)
{
    uint32_t parityMask = (1u << code.parityBits) - 1;
    uint16_t rxData = data(code, codeword);
    uint8_t syndrome = (codeword & parityMask) ^ parity(code, rxData);

    if (syndrome == 0)
        return 0;

    uint32_t wordMask = (1u << (code.dataBits + code.parityBits)) - 1;
    if (code.extended && ((__builtin_popcount(codeword & wordMask) & 1) == 0))
        return -1;

    for (uint8_t i = 0; i < code.dataBits; i++) {
        if (syndrome == code.matrix[i]) {
            codeword ^= 1u << (code.parityBits + i);
            return 1;
        }
    }

    for (uint8_t j = 0; j < code.parityBits; j++) {
        if (syndrome == (1 << j)) {
            codeword ^= 1u << j;
            return 1;
        }
    }

    return -1;
}
