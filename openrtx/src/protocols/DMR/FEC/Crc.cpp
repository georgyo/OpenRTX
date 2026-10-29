/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/DMR/FEC/Crc.hpp"

using namespace DMR::FEC;

// G8(x) = x^8 + x^2 + x + 1, clause B.3.7 equation (B.16)
static const uint8_t poly8 = 0x07;

/*
 * Shift one message bit into the CRC-8 remainder.
 */
static inline uint8_t crc8Bit(uint8_t crc, uint8_t bit)
{
    uint8_t msb = (crc >> 7) ^ (bit & 1);

    crc <<= 1;
    if (msb)
        crc ^= poly8;

    return crc;
}

uint8_t Crc::crc8Bits(uint32_t message, uint8_t nBits)
{
    uint8_t crc = 0;

    for (uint8_t i = nBits; i > 0; i--)
        crc = crc8Bit(crc, message >> (i - 1));

    return crc;
}

uint8_t Crc::crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0;

    for (size_t i = 0; i < len; i++) {
        for (uint8_t b = 8; b > 0; b--)
            crc = crc8Bit(crc, data[i] >> (b - 1));
    }

    return crc;
}

uint16_t Crc::crcCcitt(const uint8_t *data, size_t len, uint16_t mask)
{
    // GH(x) = x^16 + x^12 + x^5 + 1, clause B.3.8 equation (B.18)
    static const uint16_t poly = 0x1021;
    uint16_t crc = 0;

    for (size_t i = 0; i < len; i++) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;

        for (uint8_t b = 0; b < 8; b++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ poly;
            else
                crc <<= 1;
        }
    }

    // IH(x) inversion, equation (B.19), then the Data Type CRC mask
    return crc ^ 0xFFFF ^ mask;
}

uint8_t Crc::checksum5(const uint8_t lc[9])
{
    uint16_t sum = 0;

    for (uint8_t i = 0; i < 9; i++)
        sum += lc[i];

    return sum % 31;
}
