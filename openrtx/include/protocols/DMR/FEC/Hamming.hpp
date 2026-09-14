/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_FEC_HAMMING_H
#define DMR_FEC_HAMMING_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <cstdint>

namespace DMR
{
namespace FEC
{

/**
 * Systematic Hamming codes of ETSI TS 102 361-1 V2.5.1 clauses B.3.3, B.3.4
 * and B.3.5. All the codes are described by the parity part of their
 * generator matrix; a codeword is laid out as:
 *
 * +--------+---------+
 * |  data  | parity  |
 * +--------+---------+
 * | k bit  |  r bit  |
 * +--------+---------+
 *
 * with the first row of the generator matrix being the MSB of the data
 * field and the leftmost parity column being the MSB of the parity field.
 */
namespace Hamming
{

/**
 * Description of a systematic Hamming code.
 */
struct Code {
    uint8_t dataBits;      ///< Number of data bits (k).
    uint8_t parityBits;    ///< Number of parity bits (r).
    bool extended;         ///< True when all codewords have even weight,
                           ///< allowing double error detection (d = 4).
    const uint8_t *matrix; ///< Parity bits contributed by each data bit,
                           ///< entry 0 being the data LSB.
};

/**
 * Hamming (7,4,3) code, clause B.3.5 table B.17, protecting the CACH TACT
 * bits (clause 9.1.4).
 */
extern const Code H7_4;

/**
 * Hamming (13,9,3) code, clause B.3.4 table B.14, protecting the columns of
 * the BPTC (196,96) code (clause B.1.1).
 */
extern const Code H13_9;

/**
 * Hamming (15,11,3) code, clause B.3.4 table B.15, protecting the rows of
 * the BPTC (196,96) code (clause B.1.1).
 */
extern const Code H15_11;

/**
 * Hamming (16,11,4) code, clause B.3.4 table B.16, protecting the rows of
 * the embedded signalling BPTC (clause B.2.1).
 */
extern const Code H16_11;

/**
 * Hamming (17,12,3) code, clause B.3.3 table B.13, protecting the rows of
 * the CACH short LC BPTC (clause B.2.3).
 */
extern const Code H17_12;

/**
 * Compute the parity bits of a data block.
 *
 * @param code: code description.
 * @param data: input data, bits above dataBits are ignored.
 * @return parity field.
 */
uint8_t parity(const Code &code, uint16_t data);

/**
 * Encode a data block into a systematic codeword.
 *
 * @param code: code description.
 * @param data: input data, bits above dataBits are ignored.
 * @return codeword, data in the upper bits and parity in the lower ones.
 */
uint32_t encode(const Code &code, uint16_t data);

/**
 * Decode a codeword in place, correcting a single bit error anywhere in the
 * codeword. For the extended (d = 4) code two bit errors are detected.
 *
 * @param code: code description.
 * @param codeword: codeword to be checked and corrected.
 * @return number of corrected bit errors (0 or 1) or -1 if the codeword
 * cannot be corrected, in which case it is left untouched.
 */
int decode(const Code &code, uint32_t &codeword);

/**
 * Extract the data block from a codeword.
 *
 * @param code: code description.
 * @param codeword: input codeword.
 * @return data field.
 */
inline uint16_t data(const Code &code, uint32_t codeword)
{
    return (codeword >> code.parityBits) & ((1u << code.dataBits) - 1);
}

} // namespace Hamming
} // namespace FEC
} // namespace DMR

#endif // DMR_FEC_HAMMING_H
