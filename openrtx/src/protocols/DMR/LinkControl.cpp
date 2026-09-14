/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <cstring>
#include "protocols/DMR/LinkControl.hpp"

extern "C" {
#include "core/crc.h"
}

using namespace DMR;

static void putAddress(uint8_t *out, uint32_t addr)
{
    out[0] = (addr >> 16) & 0xFF;
    out[1] = (addr >> 8) & 0xFF;
    out[2] = addr & 0xFF;
}

static uint32_t getAddress(const uint8_t *in)
{
    return ((uint32_t)in[0] << 16) | ((uint32_t)in[1] << 8) | in[2];
}

static bool flcoIsKnown(uint8_t flco)
{
    switch (flco) {
        case FLCO_GRP_V_CH_USR:
        case FLCO_UU_V_CH_USR:
        case FLCO_TALKER_ALIAS_HDR:
        case FLCO_TALKER_ALIAS_BLK1:
        case FLCO_TALKER_ALIAS_BLK2:
        case FLCO_TALKER_ALIAS_BLK3:
        case FLCO_GPS_INFO:
            return true;
        default:
            return false;
    }
}

void FullLC::clear()
{
    memset(this, 0x00, sizeof(FullLC));
}

void FullLC::pack(uint8_t out[FULL_LC_BYTES]) const
{
    /* TS 102 361-1 §7.1 Figure 7.1: PF at bit 7, R at bit 6, FLCO 5..0 */
    out[0] = (pf ? 0x80 : 0x00) | (r ? 0x40 : 0x00) | (flco & 0x3F);
    out[1] = fid;
    out[2] = serviceOptions;
    putAddress(&out[3], dst & ADDRESS_MASK);
    putAddress(&out[6], src & ADDRESS_MASK);
}

bool FullLC::unpack(const uint8_t in[FULL_LC_BYTES])
{
    pf = (in[0] & 0x80) != 0;
    r = (in[0] & 0x40) != 0;
    flco = in[0] & 0x3F;
    fid = in[1];
    serviceOptions = in[2];
    dst = getAddress(&in[3]);
    src = getAddress(&in[6]);
    manufacturer = (fid != FID_STANDARD);

    /*
     * TS 102 361-2 Tables 7.1/7.2: the reserved bit shall be zero. A set
     * Protect Flag means the payload is not in the clear, nothing can be
     * made of it here.
     */
    if (pf || r)
        return false;

    /* TS 102 361-1 Table 9.21: 0x01-0x03 reserved for future standard use */
    if ((fid != FID_STANDARD) && (fid <= FID_RESERVED_MAX))
        return false;

    return flcoIsKnown(flco);
}

bool FullLC::isVoice() const
{
    return (flco == FLCO_GRP_V_CH_USR) || (flco == FLCO_UU_V_CH_USR);
}

bool FullLC::isGroup() const
{
    return flco == FLCO_GRP_V_CH_USR;
}

bool FullLC::isPrivate() const
{
    return flco == FLCO_UU_V_CH_USR;
}

bool FullLC::isAllCall() const
{
    return isGroup() && ((dst & ADDRESS_MASK) == ADDRESS_ALL);
}

bool FullLC::matches(uint32_t ownId, uint32_t talkgroup, uint8_t callType,
                     uint8_t monitor) const
{
    if (!isVoice())
        return false;

    /* Monitor 1 (own colour code) and 2 (promiscuous): any voice call */
    if (monitor != 0)
        return true;

    ownId &= ADDRESS_MASK;
    talkgroup &= ADDRESS_MASK;

    /* A call to everybody is always heard */
    if (isAllCall())
        return true;

    /* A call addressed to this radio is always heard, once it has an ID */
    if (isPrivate())
        return (ownId != 0) && (dst == ownId);

    /* Group call: only the selected talkgroup, and only in group mode */
    switch (callType) {
        case GROUP:
            return dst == talkgroup;
        case PRIVATE:
        case ALL:
        default:
            return false;
    }
}

void FullLC::toChipLc(uint8_t out[CHIP_LC_BYTES]) const
{
    /*
     * UNVERIFIED on hardware: the HR_C6000 manual describes the LC RAM as
     * 12 octets, the 9 LC octets followed by the RS(12,9) parity the chip
     * computes itself (TS 102 361-1 §B.3.6); the three parity positions
     * are written as zero and the chip is expected to overwrite them.
     */
    pack(out);
    memset(&out[FULL_LC_BYTES], 0x00, CHIP_LC_BYTES - FULL_LC_BYTES);
}

bool FullLC::fromChipLc(const uint8_t in[CHIP_LC_BYTES])
{
    return unpack(in);
}

void DMR::buildBsDwnAct(uint32_t src, uint8_t out[CSBK_DATA_BYTES])
{
    memset(out, 0x00, CSBK_DATA_BYTES);

    /* TS 102 361-1 §7.2 Figure 7.8: LB bit 7, PF bit 6, CSBKO 5..0 */
    out[0] = 0x80 | CSBKO_BS_DWN_ACT;
    out[1] = FID_STANDARD;
    /* octets 2-3 reserved, octets 4-6 BS address: all zero */
    putAddress(&out[7], src & ADDRESS_MASK);
}

uint16_t DMR::csbkCrc(const uint8_t data[CSBK_DATA_BYTES])
{
    /*
     * TS 102 361-1 §B.3.8: G(x) = x^16 + x^12 + x^5 + 1, initial remainder
     * 0x0000, result complemented (added to I(x) = all ones); then §B.3.12
     * Table B.21 mask for the CSBK data type.
     */
    uint16_t crc = crc_ccitt(data, CSBK_DATA_BYTES);
    crc ^= 0xFFFF;
    crc ^= (uint16_t)CRC_MASK_CSBK;

    return crc;
}

void DMR::buildBsDwnActWithCrc(uint32_t src, uint8_t out[CSBK_BYTES])
{
    buildBsDwnAct(src, out);

    uint16_t crc = csbkCrc(out);
    out[10] = (crc >> 8) & 0xFF;
    out[11] = crc & 0xFF;
}

FullLC DMR::lcFromChannel(const channel_t &channel, const contact_t *contact,
                          uint32_t ownId, uint32_t fallbackDst,
                          uint8_t fallbackType)
{
    FullLC lc;
    lc.clear();

    uint32_t dst = fallbackDst;
    uint8_t type = fallbackType;

    if ((contact != NULL) && (channel.mode == OPMODE_DMR)
        && (contact->mode == OPMODE_DMR)) {
        dst = contact->info.dmr.id;
        type = contact->info.dmr.contactType;
    }

    switch (type) {
        case PRIVATE:
            lc.flco = FLCO_UU_V_CH_USR;
            lc.dst = dst & ADDRESS_MASK;
            break;

        case ALL:
            /* TS 102 361-2 §5.3.2.1: all unit Id, broadcast option set */
            lc.flco = FLCO_GRP_V_CH_USR;
            lc.dst = ADDRESS_ALL;
            lc.serviceOptions = SO_BROADCAST;
            break;

        case GROUP:
        default:
            lc.flco = FLCO_GRP_V_CH_USR;
            lc.dst = dst & ADDRESS_MASK;
            break;
    }

    lc.fid = FID_STANDARD;
    lc.src = ownId & ADDRESS_MASK;

    return lc;
}
