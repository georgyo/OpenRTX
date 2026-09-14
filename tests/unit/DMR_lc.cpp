/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>

#include "protocols/DMR/Constants.hpp"
#include "protocols/DMR/LinkControl.hpp"

extern "C" {
#include "core/crc.h"
}

using namespace DMR;

static const uint32_t test_ids[] = { 0, 1, 0xFFFFFF };

TEST_CASE("DMR sync patterns are symbol-wise complements", "[dmr][lc]")
{
    /* TS 102 361-1 Table 9.2 note: voice and data are complements */
    const uint64_t flip = 0xAAAAAAAAAAAAULL;

    REQUIRE((SYNC_BS_VOICE ^ flip) == SYNC_BS_DATA);
    REQUIRE((SYNC_MS_VOICE ^ flip) == SYNC_MS_DATA);
    REQUIRE((SYNC_DMO_TS1_VOICE ^ flip) == SYNC_DMO_TS1_DATA);
    REQUIRE((SYNC_DMO_TS2_VOICE ^ flip) == SYNC_DMO_TS2_DATA);

    /* Every pattern fits in 48 bits and every pattern is distinct */
    const uint64_t patterns[] = { SYNC_BS_VOICE,     SYNC_BS_DATA,
                                  SYNC_MS_VOICE,     SYNC_MS_DATA,
                                  SYNC_MS_RC,        SYNC_DMO_TS1_VOICE,
                                  SYNC_DMO_TS1_DATA, SYNC_DMO_TS2_VOICE,
                                  SYNC_DMO_TS2_DATA, SYNC_RESERVED };

    for (size_t i = 0; i < 10; i++) {
        REQUIRE((patterns[i] & ~SYNC_MASK) == 0);
        for (size_t j = i + 1; j < 10; j++)
            REQUIRE(patterns[i] != patterns[j]);
    }
}

TEST_CASE("DMR constants match the ETSI tables", "[dmr][lc]")
{
    REQUIRE(DT_PI_HEADER == 0);
    REQUIRE(DT_VOICE_LC_HEADER == 1);
    REQUIRE(DT_TERMINATOR_LC == 2);
    REQUIRE(DT_CSBK == 3);
    REQUIRE(DT_IDLE == 9);
    REQUIRE(DT_RATE_1_DATA == 10);

    REQUIRE(FLCO_GRP_V_CH_USR == 0);
    REQUIRE(FLCO_UU_V_CH_USR == 3);
    REQUIRE(FLCO_TALKER_ALIAS_HDR == 4);
    REQUIRE(FLCO_GPS_INFO == 8);

    REQUIRE(CSBKO_BS_DWN_ACT == 0x38);
    REQUIRE(CRC_MASK_CSBK == 0xA5A5);

    REQUIRE(T_CALLHT_MS == 3000);
    REQUIRE(T_TO_S == 180);
    REQUIRE(T_SYNCWU_MS == 360);
    REQUIRE(N_WAKEUP == 2);
    REQUIRE(SUPERFRAME_MS == 360);
    REQUIRE(BURST_MS == 30);

    REQUIRE(sizeof(AMBE_SILENCE) == 27);
    REQUIRE(AMBE_SILENCE[0] == 0xB9);
    REQUIRE(AMBE_SILENCE[8] == 0x6B);
    REQUIRE(memcmp(AMBE_SILENCE, AMBE_SILENCE + 9, 9) == 0);
    REQUIRE(memcmp(AMBE_SILENCE, AMBE_SILENCE + 18, 9) == 0);
}

TEST_CASE("DMR Full LC hand-built group call", "[dmr][lc]")
{
    /* Grp_V_Ch_Usr from 1 to talkgroup 9 */
    const uint8_t expected[9] = { 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0x09, 0x00, 0x00, 0x01 };

    FullLC lc;
    lc.clear();
    lc.flco = FLCO_GRP_V_CH_USR;
    lc.dst = 9;
    lc.src = 1;

    uint8_t packed[9];
    lc.pack(packed);
    REQUIRE(memcmp(packed, expected, 9) == 0);

    FullLC parsed;
    REQUIRE(parsed.unpack(expected) == true);
    REQUIRE(parsed.isVoice());
    REQUIRE(parsed.isGroup());
    REQUIRE_FALSE(parsed.isPrivate());
    REQUIRE_FALSE(parsed.manufacturer);
    REQUIRE(parsed.dst == 9);
    REQUIRE(parsed.src == 1);
    REQUIRE(parsed.serviceOptions == 0);
}

TEST_CASE("DMR Full LC group and private round trips", "[dmr][lc]")
{
    const uint8_t opcodes[] = { FLCO_GRP_V_CH_USR, FLCO_UU_V_CH_USR };

    for (uint8_t flco : opcodes) {
        for (uint32_t dst : test_ids) {
            for (uint32_t src : test_ids) {
                INFO("flco " << (int)flco << " dst " << dst << " src " << src);

                FullLC lc;
                lc.clear();
                lc.flco = flco;
                lc.dst = dst;
                lc.src = src;
                lc.serviceOptions = SO_EMERGENCY | SO_OVCM;

                uint8_t buf[9];
                lc.pack(buf);
                REQUIRE((buf[0] & 0xC0) == 0);
                REQUIRE(buf[1] == 0);

                FullLC out;
                REQUIRE(out.unpack(buf) == true);
                REQUIRE(out.flco == flco);
                REQUIRE(out.fid == 0);
                REQUIRE(out.dst == dst);
                REQUIRE(out.src == src);
                REQUIRE(out.serviceOptions == (SO_EMERGENCY | SO_OVCM));
                REQUIRE_FALSE(out.pf);
                REQUIRE_FALSE(out.r);
                REQUIRE_FALSE(out.manufacturer);
                REQUIRE(out.isVoice());
                REQUIRE(out.isGroup() == (flco == FLCO_GRP_V_CH_USR));
                REQUIRE(out.isPrivate() == (flco == FLCO_UU_V_CH_USR));

                uint8_t again[9];
                out.pack(again);
                REQUIRE(memcmp(buf, again, 9) == 0);
            }
        }
    }
}

TEST_CASE("DMR Full LC pack truncates to the field widths", "[dmr][lc]")
{
    FullLC lc;
    lc.clear();
    lc.flco = 0xC3; /* PF and R bits set in the opcode must not leak */
    lc.dst = 0x12ABCDEF;
    lc.src = 0xFF123456;

    uint8_t buf[9];
    lc.pack(buf);
    REQUIRE(buf[0] == 0x03);
    REQUIRE(buf[3] == 0xAB);
    REQUIRE(buf[4] == 0xCD);
    REQUIRE(buf[5] == 0xEF);
    REQUIRE(buf[6] == 0x12);
    REQUIRE(buf[7] == 0x34);
    REQUIRE(buf[8] == 0x56);
}

TEST_CASE("DMR Full LC rejects PF and reserved bit", "[dmr][lc]")
{
    uint8_t buf[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x01 };
    FullLC lc;

    buf[0] = 0x80; /* PF */
    REQUIRE(lc.unpack(buf) == false);
    REQUIRE(lc.pf);
    REQUIRE_FALSE(lc.r);

    buf[0] = 0x40; /* R */
    REQUIRE(lc.unpack(buf) == false);
    REQUIRE(lc.r);
    REQUIRE_FALSE(lc.pf);

    buf[0] = 0xC0; /* both */
    REQUIRE(lc.unpack(buf) == false);

    buf[0] = 0x00;
    REQUIRE(lc.unpack(buf) == true);
}

TEST_CASE("DMR Full LC feature set ID handling", "[dmr][lc]")
{
    uint8_t buf[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x01 };
    FullLC lc;

    /* Standard feature set */
    REQUIRE(lc.unpack(buf) == true);
    REQUIRE_FALSE(lc.manufacturer);

    /* Reserved for future standardisation: rejected */
    for (uint8_t fid = 1; fid <= 3; fid++) {
        INFO("fid " << (int)fid);
        buf[1] = fid;
        REQUIRE(lc.unpack(buf) == false);
    }

    /* Manufacturer feature sets: accepted, flagged */
    const uint8_t mfids[] = { 0x04, 0x10, 0x68, 0x7F, 0x80, 0xFF };
    for (uint8_t fid : mfids) {
        INFO("fid " << (int)fid);
        buf[1] = fid;
        REQUIRE(lc.unpack(buf) == true);
        REQUIRE(lc.manufacturer);
        REQUIRE(lc.fid == fid);
        REQUIRE(lc.dst == 9);
        REQUIRE(lc.src == 1);
    }
}

TEST_CASE("DMR Full LC opcode validation", "[dmr][lc]")
{
    uint8_t buf[9] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x01 };
    FullLC lc;

    /* FLCO 4..8 are accepted but are not voice calls */
    for (uint8_t flco = 4; flco <= 8; flco++) {
        INFO("flco " << (int)flco);
        buf[0] = flco;
        REQUIRE(lc.unpack(buf) == true);
        REQUIRE(lc.flco == flco);
        REQUIRE_FALSE(lc.isVoice());
        REQUIRE_FALSE(lc.isGroup());
        REQUIRE_FALSE(lc.isPrivate());
        REQUIRE_FALSE(lc.matches(1, 9, GROUP, 2));
    }

    /* Everything else in the 6-bit range is rejected */
    for (uint8_t flco = 0; flco < 64; flco++) {
        bool known = (flco == 0) || (flco == 3) || ((flco >= 4) && (flco <= 8));
        INFO("flco " << (int)flco);
        buf[0] = flco;
        REQUIRE(lc.unpack(buf) == known);
        REQUIRE(lc.flco == flco);
    }
}

TEST_CASE("DMR Full LC matches() truth table", "[dmr][lc]")
{
    const uint32_t ownId = 1234567;
    const uint32_t tg = 9;

    FullLC grpToTg, grpToOther, grpToOwnId, pvtToOwn, pvtToOther, allCall;

    grpToTg.clear();
    grpToTg.flco = FLCO_GRP_V_CH_USR;
    grpToTg.dst = tg;
    grpToTg.src = 100;

    grpToOther = grpToTg;
    grpToOther.dst = 91;

    grpToOwnId = grpToTg;
    grpToOwnId.dst = ownId;

    pvtToOwn.clear();
    pvtToOwn.flco = FLCO_UU_V_CH_USR;
    pvtToOwn.dst = ownId;
    pvtToOwn.src = 100;

    pvtToOther = pvtToOwn;
    pvtToOther.dst = 7654321;

    allCall = grpToTg;
    allCall.dst = ADDRESS_ALL;

    SECTION("monitor 0, GROUP")
    {
        REQUIRE(grpToTg.matches(ownId, tg, GROUP, 0));
        REQUIRE_FALSE(grpToOther.matches(ownId, tg, GROUP, 0));
        REQUIRE_FALSE(grpToOwnId.matches(ownId, tg, GROUP, 0));
        REQUIRE(pvtToOwn.matches(ownId, tg, GROUP, 0));
        REQUIRE_FALSE(pvtToOther.matches(ownId, tg, GROUP, 0));
        REQUIRE(allCall.matches(ownId, tg, GROUP, 0));
    }

    SECTION("monitor 0, PRIVATE: talkgroup holds the target ID")
    {
        REQUIRE_FALSE(grpToTg.matches(ownId, 7654321, PRIVATE, 0));
        REQUIRE_FALSE(grpToOther.matches(ownId, 7654321, PRIVATE, 0));
        REQUIRE(pvtToOwn.matches(ownId, 7654321, PRIVATE, 0));
        REQUIRE_FALSE(pvtToOther.matches(ownId, 7654321, PRIVATE, 0));
        REQUIRE(allCall.matches(ownId, 7654321, PRIVATE, 0));
    }

    SECTION("monitor 0, ALL")
    {
        REQUIRE_FALSE(grpToTg.matches(ownId, tg, ALL, 0));
        REQUIRE_FALSE(grpToOther.matches(ownId, tg, ALL, 0));
        REQUIRE(pvtToOwn.matches(ownId, tg, ALL, 0));
        REQUIRE_FALSE(pvtToOther.matches(ownId, tg, ALL, 0));
        REQUIRE(allCall.matches(ownId, tg, ALL, 0));
    }

    SECTION("monitor 1 and 2 accept every voice call")
    {
        const uint8_t types[] = { GROUP, PRIVATE, ALL };
        for (uint8_t type : types) {
            for (uint8_t monitor = 1; monitor <= 2; monitor++) {
                INFO("type " << (int)type << " monitor " << (int)monitor);
                REQUIRE(grpToTg.matches(ownId, tg, type, monitor));
                REQUIRE(grpToOther.matches(ownId, tg, type, monitor));
                REQUIRE(grpToOwnId.matches(ownId, tg, type, monitor));
                REQUIRE(pvtToOwn.matches(ownId, tg, type, monitor));
                REQUIRE(pvtToOther.matches(ownId, tg, type, monitor));
                REQUIRE(allCall.matches(ownId, tg, type, monitor));
            }
        }
    }

    SECTION("an unset ID (0) never matches a private call")
    {
        FullLC pvtToZero = pvtToOwn;
        pvtToZero.dst = 0;
        REQUIRE_FALSE(pvtToZero.matches(0, tg, GROUP, 0));
        REQUIRE_FALSE(pvtToOwn.matches(0, tg, GROUP, 0));
        REQUIRE(grpToTg.matches(0, tg, GROUP, 0));
    }

    SECTION("non-voice LCs never match")
    {
        FullLC alias = grpToTg;
        alias.flco = FLCO_TALKER_ALIAS_HDR;
        REQUIRE_FALSE(alias.matches(ownId, tg, GROUP, 0));
        REQUIRE_FALSE(alias.matches(ownId, tg, GROUP, 1));
        REQUIRE_FALSE(alias.matches(ownId, tg, GROUP, 2));
    }
}

TEST_CASE("DMR BS_Dwn_Act CSBK bytes", "[dmr][lc]")
{
    uint8_t csbk[10];

    buildBsDwnAct(0x000001, csbk);
    const uint8_t expected1[10] = { 0xB8, 0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x00, 0x00, 0x01 };
    REQUIRE(memcmp(csbk, expected1, 10) == 0);

    buildBsDwnAct(0x12D687, csbk); /* 1234567 */
    const uint8_t expected2[10] = { 0xB8, 0x00, 0x00, 0x00, 0x00,
                                    0x00, 0x00, 0x12, 0xD6, 0x87 };
    REQUIRE(memcmp(csbk, expected2, 10) == 0);

    /* Only 24 bits of the source are used */
    buildBsDwnAct(0xFFFFFFFF, csbk);
    REQUIRE(csbk[0] == 0xB8);
    REQUIRE(csbk[6] == 0x00);
    REQUIRE(csbk[7] == 0xFF);
    REQUIRE(csbk[8] == 0xFF);
    REQUIRE(csbk[9] == 0xFF);
}

TEST_CASE("DMR CSBK CRC: CRC-CCITT, complemented, masked", "[dmr][lc]")
{
    /*
     * The CRC-CCITT of TS 102 361-1 B.3.8 with initial remainder 0 over
     * "123456789" is the well known 0x31C3 (before the complement).
     */
    const uint8_t check[9] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    REQUIRE(crc_ccitt(check, 9) == 0x31C3);

    /* All zero payload: CRC-CCITT is 0, complemented 0xFFFF, masked */
    const uint8_t zero[10] = { 0 };
    REQUIRE(csbkCrc(zero) == (0xFFFF ^ 0xA5A5));

    uint8_t full[12];
    buildBsDwnActWithCrc(0x000001, full);

    uint8_t payload[10];
    memcpy(payload, full, 10);
    uint16_t crc = crc_ccitt(payload, 10) ^ 0xFFFF ^ 0xA5A5;
    REQUIRE(full[0] == 0xB8);
    REQUIRE(full[9] == 0x01);
    REQUIRE(full[10] == ((crc >> 8) & 0xFF));
    REQUIRE(full[11] == (crc & 0xFF));

    /* Unmasking and removing the complement gives back the plain CRC */
    uint16_t received = ((uint16_t)full[10] << 8) | full[11];
    REQUIRE((received ^ 0xA5A5 ^ 0xFFFF) == crc_ccitt(payload, 10));
}

TEST_CASE("DMR chip LC RAM layout", "[dmr][lc]")
{
    FullLC lc;
    lc.clear();
    lc.flco = FLCO_UU_V_CH_USR;
    lc.dst = 0xABCDEF;
    lc.src = 0x123456;

    uint8_t ram[12];
    memset(ram, 0xFF, sizeof(ram));
    lc.toChipLc(ram);

    const uint8_t expected[12] = { 0x03, 0x00, 0x00, 0xAB, 0xCD, 0xEF,
                                   0x12, 0x34, 0x56, 0x00, 0x00, 0x00 };
    REQUIRE(memcmp(ram, expected, 12) == 0);

    /* Parity octets, whatever the chip put there, are ignored on read */
    ram[9] = 0x5A;
    ram[10] = 0xA5;
    ram[11] = 0x11;

    FullLC back;
    REQUIRE(back.fromChipLc(ram) == true);
    REQUIRE(back.flco == FLCO_UU_V_CH_USR);
    REQUIRE(back.dst == 0xABCDEF);
    REQUIRE(back.src == 0x123456);
    REQUIRE(back.isPrivate());
}
