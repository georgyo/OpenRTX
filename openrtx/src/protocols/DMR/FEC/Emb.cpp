/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "protocols/DMR/FEC/Emb.hpp"
#include "protocols/DMR/FEC/QR16_7_6.hpp"

using namespace DMR::FEC;

uint16_t Emb::encode(uint8_t colourCode, bool pi, uint8_t lcss)
{
    uint8_t data = ((colourCode & 0x0F) << 3) | (pi ? 0x04 : 0x00)
                 | (lcss & 0x03);

    return QR16_7_6::encode(data);
}

int Emb::decode(uint16_t pdu, uint8_t &colourCode, bool &pi, uint8_t &lcss)
{
    uint8_t data;
    int result = QR16_7_6::decode(pdu, data);

    if (result < 0)
        return -1;

    colourCode = data >> 3;
    pi = (data & 0x04) != 0;
    lcss = data & 0x03;

    return result;
}
