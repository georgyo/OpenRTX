/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/DMR/FEC/ShortLc.hpp"
#include "protocols/DMR/FEC/Crc.hpp"
#include "protocols/DMR/FEC/Hamming.hpp"

using namespace DMR::FEC;

static constexpr uint8_t ROWS = 4;
static constexpr uint8_t COLS = ShortLc::FRAGMENT_BITS;
static constexpr uint8_t INFO_ROWS = 3;
static constexpr uint8_t MESSAGE_BITS = 28;
static constexpr uint8_t ROW_BITS = 12;

/*
 * Information content of the three coded rows (figure B.6): the 36 bits
 * LC(27)..LC(0), CR(7)..CR(0) taken twelve at a time, row 0 holding
 * LC(27)..LC(16).
 */
static uint16_t rowData(uint32_t message, uint8_t crc, uint8_t row)
{
    uint64_t bits = ((uint64_t)(message & ShortLc::MESSAGE_MASK) << 8) | crc;
    uint8_t shift = (INFO_ROWS - 1 - row) * ROW_BITS;

    return (uint16_t)((bits >> shift) & 0x0FFF);
}

/*
 * Bit of the transmit sequence at the given row and column of the encode
 * matrix: columns are read top to bottom, left to right (clause B.2.3), so
 * the sequence index is col * 4 + row; fragment n carries indices 17n to
 * 17n + 16, first one in its bit 16.
 */
static inline uint8_t txBit(const uint32_t fragments[ShortLc::FRAGMENTS],
                            uint8_t row, uint8_t col)
{
    uint8_t pos = col * ROWS + row;

    return (fragments[pos / COLS] >> (COLS - 1 - (pos % COLS))) & 1;
}

uint8_t ShortLc::crc(uint32_t message)
{
    return Crc::crc8Bits(message & MESSAGE_MASK, MESSAGE_BITS);
}

void ShortLc::encode(uint32_t message, uint32_t fragments[FRAGMENTS])
{
    uint32_t matrix[ROWS];
    uint8_t check = crc(message);

    for (uint8_t row = 0; row < INFO_ROWS; row++)
        matrix[row] = Hamming::encode(Hamming::H17_12,
                                      rowData(message, check, row));

    /* Even column parity, PC(16)..PC(0) */
    matrix[INFO_ROWS] = 0;
    for (uint8_t row = 0; row < INFO_ROWS; row++)
        matrix[INFO_ROWS] ^= matrix[row];

    for (uint8_t i = 0; i < FRAGMENTS; i++)
        fragments[i] = 0;

    for (uint8_t col = 0; col < COLS; col++) {
        for (uint8_t row = 0; row < ROWS; row++) {
            uint8_t pos = col * ROWS + row;
            if ((matrix[row] >> (COLS - 1 - col)) & 1)
                fragments[pos / COLS] |= 1u << (COLS - 1 - (pos % COLS));
        }
    }
}

int ShortLc::decode(const uint32_t fragments[FRAGMENTS], uint32_t &message)
{
    uint32_t matrix[ROWS];
    int corrected = 0;

    for (uint8_t row = 0; row < ROWS; row++) {
        matrix[row] = 0;
        for (uint8_t col = 0; col < COLS; col++)
            matrix[row] = (matrix[row] << 1) | txBit(fragments, row, col);
    }

    for (uint8_t row = 0; row < INFO_ROWS; row++) {
        uint32_t word = matrix[row];
        int res = Hamming::decode(Hamming::H17_12, word);
        if (res < 0)
            return -1;

        matrix[row] = word;
        corrected += res;
    }

    /*
     * Column parity, figure B.6: every column has even weight. The three
     * coded rows are valid Hamming codewords here, so a remaining odd
     * column is attributed to the parity row, which carries no information;
     * the CRC below is the final guard against a miscorrected row.
     */
    uint32_t parity = 0;
    for (uint8_t row = 0; row < ROWS; row++)
        parity ^= matrix[row];

    corrected += __builtin_popcount(parity);

    uint64_t bits = 0;
    for (uint8_t row = 0; row < INFO_ROWS; row++)
        bits = (bits << ROW_BITS) | Hamming::data(Hamming::H17_12, matrix[row]);

    uint32_t decoded = (uint32_t)(bits >> 8) & MESSAGE_MASK;
    if ((uint8_t)(bits & 0xFF) != crc(decoded))
        return -1;

    message = decoded;
    return corrected;
}

bool ShortLc::fromChipBytes(const uint8_t bytes[CHIP_BYTES], uint32_t &message)
{
    uint32_t decoded = ((uint32_t)bytes[0] << 20) | ((uint32_t)bytes[1] << 12)
                     | ((uint32_t)bytes[2] << 4) | (bytes[3] >> 4);

    if (bytes[4] != crc(decoded))
        return false;

    message = decoded;
    return true;
}

void ShortLc::toChipBytes(uint32_t message, uint8_t bytes[CHIP_BYTES])
{
    message &= MESSAGE_MASK;
    bytes[0] = (uint8_t)(message >> 20);
    bytes[1] = (uint8_t)(message >> 12);
    bytes[2] = (uint8_t)(message >> 4);
    bytes[3] = (uint8_t)(message << 4);
    bytes[4] = crc(message);
}
