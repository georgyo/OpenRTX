/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <cstring>
#include "protocols/DMR/FEC/EmbeddedLc.hpp"
#include "protocols/DMR/FEC/Hamming.hpp"
#include "protocols/DMR/FEC/Crc.hpp"

using namespace DMR::FEC;

static constexpr uint8_t ROWS = 8;
static constexpr uint8_t COLS = 16;
static constexpr uint8_t LC_ROWS = 7;

/*
 * Information content of the seven LC rows (figure B.3): each row carries
 * 11 bits, rows 0 and 1 are eleven LC bits each, rows 2 to 6 are ten LC
 * bits followed by one checksum bit CS(4)..CS(0).
 */
static uint16_t rowData(const uint8_t lc[9], uint8_t checksum, uint8_t row)
{
    uint16_t word = 0;
    uint8_t lcBits = (row < 2) ? 11 : 10;
    uint8_t first = (row < 2) ? (71 - 11 * row) : (49 - 10 * (row - 2));

    for (uint8_t i = 0; i < lcBits; i++) {
        uint8_t n = first - i;
        uint8_t bit = (lc[(71 - n) / 8] >> (n % 8)) & 1;
        word = (word << 1) | bit;
    }

    if (row >= 2)
        word = (word << 1) | ((checksum >> (6 - row)) & 1);

    return word;
}

/*
 * Inverse of rowData(): scatter the 11 information bits of a row into the
 * LC octets and the checksum.
 */
static void unpackRow(uint16_t word, uint8_t row, uint8_t lc[9],
                      uint8_t &checksum)
{
    uint8_t lcBits = (row < 2) ? 11 : 10;
    uint8_t first = (row < 2) ? (71 - 11 * row) : (49 - 10 * (row - 2));

    for (uint8_t i = 0; i < lcBits; i++) {
        uint8_t n = first - i;
        if ((word >> (10 - i)) & 1)
            lc[(71 - n) / 8] |= 1 << (n % 8);
    }

    if ((row >= 2) && (word & 1))
        checksum |= 1 << (6 - row);
}

static inline uint8_t txBit(const uint32_t fragments[4], uint8_t row,
                            uint8_t col)
{
    // Columns are read top to bottom, left to right: bit index in the
    // 128-bit transmit sequence is col * 8 + row
    uint8_t pos = col * ROWS + row;

    return (fragments[pos / 32] >> (31 - (pos % 32))) & 1;
}

void EmbeddedLc::encode(const uint8_t lc[9], uint32_t fragments[4])
{
    uint16_t matrix[ROWS];
    uint8_t checksum = Crc::checksum5(lc);

    for (uint8_t row = 0; row < LC_ROWS; row++)
        matrix[row] = Hamming::encode(Hamming::H16_11,
                                      rowData(lc, checksum, row));

    matrix[LC_ROWS] = 0;
    for (uint8_t row = 0; row < LC_ROWS; row++)
        matrix[LC_ROWS] ^= matrix[row];

    memset(fragments, 0, 4 * sizeof(uint32_t));
    for (uint8_t col = 0; col < COLS; col++) {
        for (uint8_t row = 0; row < ROWS; row++) {
            uint8_t pos = col * ROWS + row;
            if ((matrix[row] >> (COLS - 1 - col)) & 1)
                fragments[pos / 32] |= 1u << (31 - (pos % 32));
        }
    }
}

int EmbeddedLc::decode(const uint32_t fragments[4], uint8_t lc[9])
{
    uint16_t matrix[ROWS];
    int corrected = 0;

    for (uint8_t row = 0; row < ROWS; row++) {
        matrix[row] = 0;
        for (uint8_t col = 0; col < COLS; col++)
            matrix[row] = (matrix[row] << 1) | txBit(fragments, row, col);
    }

    for (uint8_t row = 0; row < LC_ROWS; row++) {
        uint32_t word = matrix[row];
        int res = Hamming::decode(Hamming::H16_11, word);
        if (res < 0)
            return -1;

        matrix[row] = word;
        corrected += res;
    }

    /*
     * Column parity check, figure B.3: every column has even weight. The
     * seven LC rows are valid Hamming codewords at this point, so any
     * remaining odd column is attributed to an error in the parity row
     * itself, which carries no information; the checksum below is the final
     * guard against miscorrected rows.
     */
    uint16_t parity = 0;
    for (uint8_t row = 0; row < ROWS; row++)
        parity ^= matrix[row];

    corrected += __builtin_popcount(parity);

    uint8_t decoded[9];
    uint8_t checksum = 0;

    memset(decoded, 0, sizeof(decoded));
    for (uint8_t row = 0; row < LC_ROWS; row++)
        unpackRow(Hamming::data(Hamming::H16_11, matrix[row]), row, decoded,
                  checksum);

    if (checksum != Crc::checksum5(decoded))
        return -1;

    memcpy(lc, decoded, 9);
    return corrected;
}
