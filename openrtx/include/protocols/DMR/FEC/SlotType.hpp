/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_FEC_SLOTTYPE_H
#define DMR_FEC_SLOTTYPE_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <cstdint>

namespace DMR
{
namespace FEC
{

/**
 * Slot Type (SLOT) PDU, ETSI TS 102 361-1 V2.5.1 clause 9.1.3 table 9.4:
 * 4-bit Colour Code, 4-bit Data Type (clause 9.3.6) and 12 bits of Golay
 * (20,8) parity (clause 9.3.7). The 20-bit PDU is split in two halves
 * around the SYNC field of a general data burst (clause 6.2).
 */
namespace SlotType
{

/**
 * Encode a Slot Type PDU.
 *
 * @param colourCode: 4-bit colour code.
 * @param dataType: 4-bit data type.
 * @return 20-bit PDU, colour code in bits 19..16, data type in bits 15..12
 * and parity in bits 11..0.
 */
uint32_t encode(uint8_t colourCode, uint8_t dataType);

/**
 * Decode a Slot Type PDU, correcting up to three bit errors.
 *
 * @param pdu: 20-bit PDU.
 * @param colourCode: destination for the colour code.
 * @param dataType: destination for the data type.
 * @return number of corrected bit errors or -1 if the PDU cannot be
 * decoded, in which case the outputs are left untouched.
 */
int decode(uint32_t pdu, uint8_t &colourCode, uint8_t &dataType);

/**
 * Extract the Slot Type PDU from a general data burst (clause 6.2): ten bits
 * on each side of the 48-bit SYNC field.
 *
 * @param burst: 264-bit burst, first transmitted bit in the MSB.
 * @return 20-bit PDU.
 */
uint32_t fromBurst(const uint8_t burst[33]);

} // namespace SlotType
} // namespace FEC
} // namespace DMR

#endif // DMR_FEC_SLOTTYPE_H
