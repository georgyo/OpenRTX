/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_FEC_EMB_H
#define DMR_FEC_EMB_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <cstdint>

namespace DMR
{
namespace FEC
{

/**
 * Embedded signalling (EMB) PDU, ETSI TS 102 361-1 V2.5.1 clause 9.1.2
 * table 9.3: 4-bit Colour Code, 1-bit Pre-emption and power control
 * Indicator, 2-bit Link Control Start/Stop (clause 9.3.3) and 9 bits of
 * quadratic residue (16,7,6) parity (clause 9.3.4). The 16-bit PDU is split
 * in two halves around the embedded signalling field of a voice burst
 * (clause 6.1).
 */
namespace Emb
{

/**
 * Encode an EMB PDU.
 *
 * @param colourCode: 4-bit colour code.
 * @param pi: pre-emption and power control indicator.
 * @param lcss: 2-bit link control start/stop.
 * @return 16-bit PDU, colour code in bits 15..12, PI in bit 11, LCSS in
 * bits 10..9 and parity in bits 8..0.
 */
uint16_t encode(uint8_t colourCode, bool pi, uint8_t lcss);

/**
 * Decode an EMB PDU, correcting up to two bit errors.
 *
 * @param pdu: 16-bit PDU.
 * @param colourCode: destination for the colour code.
 * @param pi: destination for the pre-emption and power control indicator.
 * @param lcss: destination for the link control start/stop.
 * @return number of corrected bit errors or -1 if the PDU cannot be
 * decoded, in which case the outputs are left untouched.
 */
int decode(uint16_t pdu, uint8_t &colourCode, bool &pi, uint8_t &lcss);

} // namespace Emb
} // namespace FEC
} // namespace DMR

#endif // DMR_FEC_EMB_H
