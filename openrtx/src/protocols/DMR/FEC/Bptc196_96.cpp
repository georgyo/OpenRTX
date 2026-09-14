/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <cstring>
#include "protocols/DMR/FEC/Bptc196_96.hpp"
#include "protocols/DMR/FEC/Hamming.hpp"

using namespace DMR::FEC;

/*
 * Encoder matrix layout (figure B.1 and table B.2), as a flat array of 196
 * bits indexed as in table B.2:
 *
 *  - index 0 is R(3);
 *  - rows 0..8 start at index 1 + 15 * row: eleven information bits followed
 *    by the four Hamming (15,11,3) parity bits H_R(3)..H_R(0). Row 0 holds
 *    R(2), R(1), R(0), I(95)..I(88); rows 1..8 hold I(87)..I(0);
 *  - rows 9..12 (index 136 onwards) hold the Hamming (13,9,3) column parity
 *    bits H_Cn(3)..H_Cn(0), one row per parity bit.
 */
static constexpr uint8_t ROWS = 9;
static constexpr uint8_t COLS = 15;
static constexpr uint8_t DATA_COLS = 11;
static constexpr uint8_t COL_PAR = 136;
static constexpr uint8_t MAX_ITER = 5;

static inline uint8_t getBit(const uint8_t *bytes, unsigned pos)
{
    return (bytes[pos / 8] >> (7 - (pos % 8))) & 1;
}

static inline void setBit(uint8_t *bytes, unsigned pos, uint8_t value)
{
    uint8_t mask = 1 << (7 - (pos % 8));

    if (value)
        bytes[pos / 8] |= mask;
    else
        bytes[pos / 8] &= ~mask;
}

/*
 * Position in the encoder matrix of information bit I(n): the 99 payload
 * positions R(2), R(1), R(0), I(95)..I(0) are laid out row by row.
 */
static inline uint8_t infoIndex(uint8_t n)
{
    uint8_t payloadPos = 3 + (95 - n);

    return 1 + (payloadPos / DATA_COLS) * COLS + (payloadPos % DATA_COLS);
}

static inline uint8_t rowIndex(uint8_t row, uint8_t col)
{
    return 1 + row * COLS + col;
}

static inline uint8_t colParityIndex(uint8_t col, uint8_t bit)
{
    // Parity bit 3 is in the topmost of the four rows
    return COL_PAR + (3 - bit) * COLS + col;
}

static uint32_t readRow(const uint8_t *matrix, uint8_t row)
{
    uint32_t word = 0;

    for (uint8_t col = 0; col < COLS; col++)
        word = (word << 1) | matrix[rowIndex(row, col)];

    return word;
}

static void writeRow(uint8_t *matrix, uint8_t row, uint32_t word)
{
    for (uint8_t col = 0; col < COLS; col++)
        matrix[rowIndex(row, col)] = (word >> (COLS - 1 - col)) & 1;
}

static uint32_t readCol(const uint8_t *matrix, uint8_t col)
{
    uint32_t word = 0;

    for (uint8_t row = 0; row < ROWS; row++)
        word = (word << 1) | matrix[rowIndex(row, col)];

    for (uint8_t bit = 4; bit > 0; bit--)
        word = (word << 1) | matrix[colParityIndex(col, bit - 1)];

    return word;
}

static void writeCol(uint8_t *matrix, uint8_t col, uint32_t word)
{
    for (uint8_t row = 0; row < ROWS; row++)
        matrix[rowIndex(row, col)] = (word >> (12 - row)) & 1;

    for (uint8_t bit = 0; bit < 4; bit++)
        matrix[colParityIndex(col, bit)] = (word >> bit) & 1;
}

void Bptc196_96::encode(const uint8_t data[DATA_BYTES], uint8_t tx[TX_BYTES])
{
    uint8_t matrix[196];

    memset(matrix, 0, sizeof(matrix));

    for (uint8_t n = 0; n < 96; n++)
        matrix[infoIndex(n)] = getBit(data, 95 - n);

    for (uint8_t row = 0; row < ROWS; row++) {
        uint32_t word = readRow(matrix, row) >> 4;
        writeRow(matrix, row, Hamming::encode(Hamming::H15_11, word));
    }

    for (uint8_t col = 0; col < COLS; col++) {
        uint32_t word = readCol(matrix, col) >> 4;
        writeCol(matrix, col, Hamming::encode(Hamming::H13_9, word));
    }

    memset(tx, 0, TX_BYTES);
    for (uint8_t i = 0; i < 196; i++)
        setBit(tx, interleaveIndex(i), matrix[i]);
}

int Bptc196_96::decode(const uint8_t tx[TX_BYTES], uint8_t data[DATA_BYTES])
{
    uint8_t matrix[196];
    int corrected = 0;

    for (uint8_t i = 0; i < 196; i++)
        matrix[i] = getBit(tx, interleaveIndex(i));

    /*
     * Iterative decoding: rows and columns are decoded in turn until a pass
     * makes no change. A row or column which is not correctable at one pass
     * may become so once the crossing dimension has fixed some of its bits.
     */
    for (uint8_t iter = 0; iter < MAX_ITER; iter++) {
        bool changed = false;

        for (uint8_t row = 0; row < ROWS; row++) {
            uint32_t word = readRow(matrix, row);
            int res = Hamming::decode(Hamming::H15_11, word);
            if (res > 0) {
                writeRow(matrix, row, word);
                corrected += res;
                changed = true;
            }
        }

        for (uint8_t col = 0; col < COLS; col++) {
            uint32_t word = readCol(matrix, col);
            int res = Hamming::decode(Hamming::H13_9, word);
            if (res > 0) {
                writeCol(matrix, col, word);
                corrected += res;
                changed = true;
            }
        }

        if (!changed)
            break;
    }

    // Final consistency check: every row and column must be a codeword
    for (uint8_t row = 0; row < ROWS; row++) {
        uint32_t word = readRow(matrix, row);
        if (Hamming::decode(Hamming::H15_11, word) != 0)
            return -1;
    }

    for (uint8_t col = 0; col < COLS; col++) {
        uint32_t word = readCol(matrix, col);
        if (Hamming::decode(Hamming::H13_9, word) != 0)
            return -1;
    }

    memset(data, 0, DATA_BYTES);
    for (uint8_t n = 0; n < 96; n++)
        setBit(data, 95 - n, matrix[infoIndex(n)]);

    return corrected;
}

void Bptc196_96::fromBurst(const uint8_t burst[33], uint8_t tx[TX_BYTES])
{
    memset(tx, 0, TX_BYTES);

    // Info bits 0..97 precede the Slot Type + SYNC + Slot Type fields (68
    // bits), info bits 98..195 follow them
    for (uint8_t i = 0; i < 98; i++) {
        setBit(tx, i, getBit(burst, i));
        setBit(tx, 98 + i, getBit(burst, 166 + i));
    }
}
