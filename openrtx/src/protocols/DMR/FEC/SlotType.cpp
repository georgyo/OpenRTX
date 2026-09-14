/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/DMR/FEC/SlotType.hpp"
#include "protocols/DMR/FEC/Golay20_8.hpp"

using namespace DMR::FEC;

uint32_t SlotType::encode(uint8_t colourCode, uint8_t dataType)
{
    uint8_t data = ((colourCode & 0x0F) << 4) | (dataType & 0x0F);

    return Golay20_8::encode(data);
}

int SlotType::decode(uint32_t pdu, uint8_t &colourCode, uint8_t &dataType)
{
    uint8_t data;
    int result = Golay20_8::decode(pdu, data);

    if (result < 0)
        return -1;

    colourCode = data >> 4;
    dataType = data & 0x0F;

    return result;
}

uint32_t SlotType::fromBurst(const uint8_t burst[33])
{
    uint32_t pdu = 0;

    // First half: bits 98..107, second half: bits 156..165 of the burst
    for (uint8_t i = 0; i < 10; i++) {
        unsigned pos = 98 + i;
        pdu = (pdu << 1) | ((burst[pos / 8] >> (7 - (pos % 8))) & 1);
    }

    for (uint8_t i = 0; i < 10; i++) {
        unsigned pos = 156 + i;
        pdu = (pdu << 1) | ((burst[pos / 8] >> (7 - (pos % 8))) & 1);
    }

    return pdu;
}
