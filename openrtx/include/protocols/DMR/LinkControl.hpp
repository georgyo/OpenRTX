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
     * @return true for a group voice call to one of the "All unit Idn"
     * addresses 0xFFFFF0 to 0xFFFFFF of TS 102 361-1 Annex A Table A.1.
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
 *   octets 4-6: BS address, left at 0 as a placeholder: the radio has no
 *               repeater ID setting yet (a later stage fills it from the
 *               codeplug or the settings). Table 7.5a defines no wildcard
 *               and Annex A of TS 102 361-1 lists 0 as the Null address;
 *               it works because BS address checking is optional for the
 *               BS (TS 102 361-2 §5.1.1.1.1 validates colour code and
 *               slot type only).
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

/**
 * Short Link Control opcodes, TS 102 361-2 V2.4.1 Annex B.3 table B.3.
 */
enum Slco : uint8_t {
    SLCO_NULL_MSG = 0, /* Nul_Msg,  §7.1.3.1 */
    SLCO_ACT_UPDT = 1, /* Act_Updt, §7.1.3.2 */
};

/**
 * Time Slot Activity ID of the Activity Update Short LC, TS 102 361-2
 * §7.1.3.2 table 7.10. Values not listed are reserved.
 */
enum SlotActivity : uint8_t {
    ACT_NONE = 0,               /* No activity on the BS time slot        */
    ACT_GROUP_CSBK = 2,         /* Group CSBK activity                    */
    ACT_INDIVIDUAL_CSBK = 3,    /* Individual CSBK activity               */
    ACT_GROUP_VOICE = 8,        /* Group voice activity                   */
    ACT_INDIVIDUAL_VOICE = 9,   /* Individual voice activity              */
    ACT_INDIVIDUAL_DATA = 10,   /* Individual data activity               */
    ACT_GROUP_DATA = 11,        /* Group data activity                    */
    ACT_EMERGENCY_GROUP = 12,   /* Emergency group voice activity         */
    ACT_EMERGENCY_INDIVID = 13, /* Emergency individual voice activity    */
};

/**
 * Activity Update Short LC (Act_Updt), TS 102 361-2 §7.1.3.2 table 7.10:
 * what a base station announces in its CACH about both of its time slots.
 * Layout of the 28-bit message, LC(27) first:
 *
 *   LC(27..24): SLCO = 0001
 *   LC(23..20): Time Slot 1 Activity ID
 *   LC(19..16): Time Slot 2 Activity ID
 *   LC(15..8):  Hashed address time slot 1
 *   LC(7..0):   Hashed address time slot 2
 *
 * The hashed address is the 8-bit CRC of TS 102 361-1 §B.3.7 over the 24
 * bits of the destination address of the call on that slot.
 */
struct ActivityUpdate {
    uint8_t activity[2]; /* Activity ID of time slot 1 and 2, SlotActivity */
    uint8_t hash[2];     /* Hashed destination of time slot 1 and 2       */

    /**
     * Parse a Short LC message.
     *
     * @param shortLc: 28-bit Short LC, SLCO in bits 27..24.
     * @return true when the message is an Activity Update, false otherwise
     * (the fields are left untouched).
     */
    bool unpack(uint32_t shortLc);

    /**
     * Serialise into a 28-bit Short LC message, for the tests and the
     * emulator.
     *
     * @return the message, SLCO = 1 in bits 27..24.
     */
    uint32_t pack() const;

    /**
     * Whether a time slot is busy: any activity other than "no activity",
     * reserved values included, since a reserved code is still not idle.
     *
     * @param timeslot: 1 or 2.
     * @return true when the slot carries activity.
     */
    bool busy(uint8_t timeslot) const;

    /**
     * Whether the call announced on a time slot is a voice call (group,
     * individual or emergency).
     *
     * @param timeslot: 1 or 2.
     * @return true for a voice activity ID.
     */
    bool voice(uint8_t timeslot) const;

    /**
     * Whether the hashed destination of a time slot matches an address.
     *
     * @param timeslot: 1 or 2.
     * @param dst: 24-bit destination address.
     * @return true when the hash of dst equals the announced one.
     */
    bool matches(uint8_t timeslot, uint32_t dst) const;

    /**
     * Hashed address of TS 102 361-2 table 7.10 note 1: the 8-bit CRC of
     * TS 102 361-1 §B.3.7 over the 24-bit destination address.
     *
     * @param dst: 24-bit address.
     * @return the hash.
     */
    static uint8_t hashAddress(uint32_t dst);
};

} /* namespace DMR */

#endif /* DMR_LINKCONTROL_H */
