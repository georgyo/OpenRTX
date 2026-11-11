/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <cstring>
#include "protocols/DMR/FEC/Rs12_9.hpp"

using namespace DMR::FEC;

// Coefficients of g(x) = x^3 + 0x0E x^2 + 0x38 x + 0x40, equation (B.10)
static const uint8_t genPoly[3] = { 0x40, 0x38, 0x0E };

// Field primitive element
static const uint8_t alpha = 0x02;

uint8_t Rs12_9::gfMul(uint8_t a, uint8_t b)
{
    uint8_t result = 0;

    // Shift-and-add multiplication with reduction modulo the primitive
    // polynomial a^8 + a^4 + a^3 + a^2 + 1, equation (B.14): 0x11D
    while (b != 0) {
        if (b & 1)
            result ^= a;

        bool carry = (a & 0x80) != 0;
        a <<= 1;
        if (carry)
            a ^= 0x1D;

        b >>= 1;
    }

    return result;
}

/*
 * Exponentiation a^n in the field by repeated multiplication.
 */
static uint8_t gfPow(uint8_t a, uint16_t n)
{
    uint8_t result = 1;

    for (uint16_t i = 0; i < n; i++)
        result = Rs12_9::gfMul(result, a);

    return result;
}

/*
 * Multiplicative inverse a^-1 = a^254 (a^255 = 1 for any nonzero a).
 */
static uint8_t gfInv(uint8_t a)
{
    return gfPow(a, 254);
}

/*
 * Systematic encoding: the parity is the remainder of m(x) x^3 divided by
 * g(x), computed with a linear feedback shift register (equations B.11 to
 * B.13; the last row of table B.18 is g(x) itself).
 */
void Rs12_9::encode(const uint8_t message[9], uint8_t parity[3])
{
    uint8_t reg[3] = { 0, 0, 0 };

    for (uint8_t i = 0; i < 9; i++) {
        uint8_t feedback = message[i] ^ reg[2];

        reg[2] = reg[1] ^ gfMul(genPoly[2], feedback);
        reg[1] = reg[0] ^ gfMul(genPoly[1], feedback);
        reg[0] = gfMul(genPoly[0], feedback);
    }

    parity[0] = reg[2];
    parity[1] = reg[1];
    parity[2] = reg[0];
}

void Rs12_9::encodeLc(const uint8_t lc[9], uint32_t mask, uint8_t codeword[12])
{
    memcpy(codeword, lc, 9);
    encode(lc, &codeword[9]);

    codeword[9] ^= (mask >> 16) & 0xFF;
    codeword[10] ^= (mask >> 8) & 0xFF;
    codeword[11] ^= mask & 0xFF;
}

/*
 * Syndrome decoder for a single symbol error. The syndromes S_i = c(a^i),
 * i = 1..3, are evaluated with Horner's rule on the codeword polynomial
 * c(x) = c[0] x^11 + ... + c[11]. A single error of magnitude e at degree j
 * gives S_i = e a^(i j), hence S2/S1 = a^j locates the error and S1 / a^j is
 * its magnitude; S1 S3 == S2^2 confirms the single-error hypothesis.
 */
int Rs12_9::decode(uint8_t codeword[12])
{
    uint8_t syndrome[3];

    for (uint8_t i = 0; i < 3; i++) {
        uint8_t x = gfPow(alpha, i + 1);
        uint8_t acc = 0;

        for (uint8_t k = 0; k < 12; k++)
            acc = gfMul(acc, x) ^ codeword[k];

        syndrome[i] = acc;
    }

    if ((syndrome[0] == 0) && (syndrome[1] == 0) && (syndrome[2] == 0))
        return 0;

    if ((syndrome[0] == 0) || (syndrome[1] == 0))
        return -1;

    uint8_t locator = gfMul(syndrome[1], gfInv(syndrome[0]));
    if (gfMul(syndrome[1], locator) != syndrome[2])
        return -1;

    for (uint8_t j = 0; j < 12; j++) {
        if (gfPow(alpha, j) == locator) {
            uint8_t magnitude = gfMul(syndrome[0], gfInv(locator));
            codeword[11 - j] ^= magnitude;
            return 1;
        }
    }

    return -1;
}

int Rs12_9::decodeLc(const uint8_t codeword[12], uint32_t mask, uint8_t lc[9])
{
    uint8_t unmasked[12];

    memcpy(unmasked, codeword, 12);
    unmasked[9] ^= (mask >> 16) & 0xFF;
    unmasked[10] ^= (mask >> 8) & 0xFF;
    unmasked[11] ^= mask & 0xFF;

    int result = decode(unmasked);
    if (result < 0)
        return -1;

    memcpy(lc, unmasked, 9);
    return result;
}
