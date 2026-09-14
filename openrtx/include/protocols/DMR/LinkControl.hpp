/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_LINKCONTROL_H
#define DMR_LINKCONTROL_H

#ifndef __cplusplus
#error This header is C++ only!
#endif

#include <cstdint>
#include "protocols/DMR/Constants.hpp"

extern "C" {
#include "core/cps.h"
}

namespace DMR
{

/**
 * Full Link Control message, TS 102 361-1 §7.1 Figure 7.1 with the voice
 * call payloads of TS 102 361-2 §7.1.1.1 (Grp_V_Ch_Usr) and §7.1.1.2
 * (UU_V_Ch_Usr):
 *
 *   octet 0: PF | R | FLCO[5:0]
 *   octet 1: FID
 *   octet 2: Service Options
 *   octets 3-5: destination address (group or target), MSB first
 *   octets 6-8: source address, MSB first
 *
 * Octets 2-8 are always parsed with this layout, also for the non-voice
 * opcodes (talker alias, GPS info) where they carry other data: for those
 * isVoice() is false and the address fields are meaningless.
 */
struct FullLC {
    bool pf;                /* Protect Flag, must be 0                    */
    bool r;                 /* Reserved bit, must be 0                    */
    uint8_t flco;           /* Full LC Opcode, see DMR::Flco              */
    uint8_t fid;            /* Feature set ID, 0 = standard               */
    uint8_t serviceOptions; /* Service Options, see DMR::ServiceOptions   */
    uint32_t dst;           /* Group or target address, 24 bits           */
    uint32_t src;           /* Source address, 24 bits                    */
    bool manufacturer;      /* FID is a manufacturer's feature set (MFID) */

    /**
     * Reset every field to zero: a group voice LC from 0 to 0.
     */
    void clear();

    /**
     * Serialise the LC into the 9 octet Full LC format. Addresses are
     * truncated to 24 bits, the opcode to 6 bits.
     *
     * @param out: destination buffer, 9 bytes.
     */
    void pack(uint8_t out[FULL_LC_BYTES]) const;

    /**
     * Parse a 9 octet Full LC. The fields are filled regardless of the
     * outcome.
     *
     * @param in: source buffer, 9 bytes.
     * @return false when the Protect Flag or the reserved bit are set, when
     * the FID is one of the values reserved for future standardisation or
     * when the FLCO is not one of the opcodes of TS 102 361-2 Annex B.1.
     * A manufacturer FID is accepted and reported through 'manufacturer'.
     */
    bool unpack(const uint8_t in[FULL_LC_BYTES]);

    /**
     * @return true when the LC describes a voice call, i.e. the opcode is
     * Grp_V_Ch_Usr or UU_V_Ch_Usr.
     */
    bool isVoice() const;

    /**
     * @return true for a group voice call (Grp_V_Ch_Usr).
     */
    bool isGroup() const;

    /**
     * @return true for a unit to unit voice call (UU_V_Ch_Usr).
     */
    bool isPrivate() const;

    /**
     * @return true for a group voice call to an "All unit Id" address.
     */
    bool isAllCall() const;

    /**
     * Address filter for an incoming voice call. Colour code and timeslot
     * are not part of the LC and have to be checked by the caller.
     *
     * @param ownId: this radio's DMR ID; when zero no private call matches.
     * @param talkgroup: configured destination (a talkgroup for GROUP, a
     * target ID for PRIVATE, ignored for ALL).
     * @param callType: configured call type, a dmrContactType_t value.
     * @param monitor: 0 = filter on the addresses, 1 = own colour code and
     * any address, 2 = any address ("promiscuous"). For 1 and 2 every voice
     * LC matches.
     * @return true when the call is to be presented to the user.
     */
    bool matches(uint32_t ownId, uint32_t talkgroup, uint8_t callType,
                 uint8_t monitor) const;

    /**
     * Lay the LC out in the 12 octet format of the baseband LC RAM: the 9
     * LC octets followed by three zero octets where the chip appends the
     * RS(12,9) parity (TS 102 361-1 §B.3.6).
     *
     * @param out: destination buffer, 12 bytes.
     */
    void toChipLc(uint8_t out[CHIP_LC_BYTES]) const;

    /**
     * Parse the 12 octet LC RAM content, ignoring the parity octets.
     *
     * @param in: source buffer, 12 bytes.
     * @return the result of unpack().
     */
    bool fromChipLc(const uint8_t in[CHIP_LC_BYTES]);
};

/**
 * Build the BS Outbound Activation CSBK (BS_Dwn_Act, TS 102 361-2 §7.1.2.1
 * Table 7.5a, CSBKO 111000b of Annex B.2) without its CRC:
 *
 *   octet 0: LB = 1, PF = 0, CSBKO = 0x38 -> 0xB8
 *   octet 1: FID = 0
 *   octets 2-3: reserved, 0
 *   octets 4-6: BS address, 0 (any BS)
 *   octets 7-9: source address
 *
 * @param src: source address, 24 bits.
 * @param out: destination buffer, 10 bytes.
 */
void buildBsDwnAct(uint32_t src, uint8_t out[CSBK_DATA_BYTES]);

/**
 * Build the complete 12 octet BS_Dwn_Act CSBK: the 10 octets above followed
 * by the CSBK CRC, i.e. the CRC-CCITT of TS 102 361-1 §B.3.8 (initial
 * remainder 0, complemented) masked with the CSBK Data Type CRC mask 0xA5A5
 * of Table B.21, MSB first.
 *
 * @param src: source address, 24 bits.
 * @param out: destination buffer, 12 bytes.
 */
void buildBsDwnActWithCrc(uint32_t src, uint8_t out[CSBK_BYTES]);

/**
 * Compute the CSBK CRC of a 10 octet CSBK payload: CRC-CCITT (TS 102 361-1
 * §B.3.8) XOR-ed with the CSBK mask (§B.3.12).
 *
 * @param data: CSBK payload, 10 bytes.
 * @return the masked 16 bit CRC.
 */
uint16_t csbkCrc(const uint8_t data[CSBK_DATA_BYTES]);

/**
 * Derive the Full LC to transmit from the current channel and contact.
 *
 * The contact is honoured only when it is a DMR contact attached to a DMR
 * channel; otherwise the fallback destination and call type (from the
 * settings) are used. Call types map as follows (dmrContactType_t):
 *
 *   GROUP   -> Grp_V_Ch_Usr, destination = contact ID / fallbackDst
 *   PRIVATE -> UU_V_Ch_Usr,  destination = contact ID / fallbackDst
 *   ALL     -> Grp_V_Ch_Usr, destination = 0xFFFFFF, Broadcast option set
 *              (TS 102 361-2 §5.3.2.1)
 *
 * @param channel: current channel.
 * @param contact: contact selected on the channel, or NULL.
 * @param ownId: this radio's DMR ID, used as source address.
 * @param fallbackDst: destination used when there is no usable contact.
 * @param fallbackType: call type used when there is no usable contact.
 * @return the Full LC.
 */
FullLC lcFromChannel(const channel_t &channel, const contact_t *contact,
                     uint32_t ownId, uint32_t fallbackDst,
                     uint8_t fallbackType);

} /* namespace DMR */

#endif /* DMR_LINKCONTROL_H */
