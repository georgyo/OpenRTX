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
#include "core/cps.h"
}

using namespace DMR;

TEST_CASE("DMR codeplug structures keep their packed size", "[dmr][cps]")
{
    REQUIRE(sizeof(dmrInfo_t) == 4);
    REQUIRE(sizeof(dmrContact_t) == 5);
    REQUIRE(sizeof(dmrInfo_t) <= sizeof(m17Info_t));
    REQUIRE(sizeof(dmrContact_t) <= sizeof(m17Contact_t));
}

TEST_CASE("DMR dmrInfo_t bitfield ordering on this compiler", "[dmr][cps]")
{
    dmrInfo_t info;
    memset(&info, 0x00, sizeof(info));
    info.rxColorCode = 0x1;
    info.txColorCode = 0xF;
    info.dmr_timeslot = 2;
    info.contact_index = 0x1234;

    uint8_t raw[4];
    memcpy(raw, &info, sizeof(raw));

    /* First declared bitfield in the low nibble, second in the high one */
    REQUIRE(raw[0] == 0xF1);
    REQUIRE(raw[1] == 0x02);

    /* contact_index little endian at offset 2 */
    REQUIRE(raw[2] == 0x34);
    REQUIRE(raw[3] == 0x12);

    /* Both colour codes cover the full 0..15 range independently */
    for (uint8_t cc = 0; cc < 16; cc++) {
        info.rxColorCode = cc;
        info.txColorCode = 15 - cc;
        REQUIRE(info.rxColorCode == cc);
        REQUIRE(info.txColorCode == 15 - cc);
    }
}

TEST_CASE("DMR dmrContact_t bitfield ordering on this compiler", "[dmr][cps]")
{
    dmrContact_t contact;
    memset(&contact, 0x00, sizeof(contact));
    contact.id = 0x00ABCDEF;
    contact.contactType = PRIVATE;
    contact.rx_tone = 1;

    uint8_t raw[5];
    memcpy(raw, &contact, sizeof(raw));

    REQUIRE(raw[0] == 0xEF);
    REQUIRE(raw[1] == 0xCD);
    REQUIRE(raw[2] == 0xAB);
    REQUIRE(raw[3] == 0x00);
    REQUIRE(raw[4] == 0x05); /* contactType bits 0-1, rx_tone bit 2 */

    contact.contactType = ALL;
    contact.rx_tone = 0;
    memcpy(raw, &contact, sizeof(raw));
    REQUIRE(raw[4] == 0x02);
}

static channel_t makeChannel(uint8_t mode)
{
    channel_t ch;
    memset(&ch, 0x00, sizeof(ch));
    ch.mode = mode;
    ch.dmr.rxColorCode = 1;
    ch.dmr.txColorCode = 1;
    ch.dmr.dmr_timeslot = 1;
    ch.dmr.contact_index = 0;

    return ch;
}

static contact_t makeContact(uint8_t mode, uint32_t id, uint8_t type)
{
    contact_t ct;
    memset(&ct, 0x00, sizeof(ct));
    strncpy(ct.name, "test", sizeof(ct.name) - 1);
    ct.mode = mode;
    ct.info.dmr.id = id;
    ct.info.dmr.contactType = type;

    return ct;
}

TEST_CASE("DMR lcFromChannel maps a group contact", "[dmr][cps]")
{
    channel_t ch = makeChannel(OPMODE_DMR);
    contact_t ct = makeContact(OPMODE_DMR, 91, GROUP);

    FullLC lc = lcFromChannel(ch, &ct, 1234567, 9, PRIVATE);
    REQUIRE(lc.flco == FLCO_GRP_V_CH_USR);
    REQUIRE(lc.fid == FID_STANDARD);
    REQUIRE(lc.dst == 91);
    REQUIRE(lc.src == 1234567);
    REQUIRE(lc.serviceOptions == 0);
    REQUIRE_FALSE(lc.pf);
    REQUIRE_FALSE(lc.r);
    REQUIRE_FALSE(lc.manufacturer);
    REQUIRE(lc.isGroup());

    uint8_t buf[9];
    lc.pack(buf);
    const uint8_t expected[9] = { 0x00, 0x00, 0x00, 0x00, 0x00,
                                  0x5B, 0x12, 0xD6, 0x87 };
    REQUIRE(memcmp(buf, expected, 9) == 0);
}

TEST_CASE("DMR lcFromChannel maps a private contact", "[dmr][cps]")
{
    channel_t ch = makeChannel(OPMODE_DMR);
    contact_t ct = makeContact(OPMODE_DMR, 7654321, PRIVATE);

    FullLC lc = lcFromChannel(ch, &ct, 1234567, 9, GROUP);
    REQUIRE(lc.flco == FLCO_UU_V_CH_USR);
    REQUIRE(lc.dst == 7654321);
    REQUIRE(lc.src == 1234567);
    REQUIRE(lc.serviceOptions == 0);
    REQUIRE(lc.isPrivate());
}

TEST_CASE("DMR lcFromChannel maps an all call", "[dmr][cps]")
{
    channel_t ch = makeChannel(OPMODE_DMR);
    contact_t ct = makeContact(OPMODE_DMR, 12345, ALL);

    FullLC lc = lcFromChannel(ch, &ct, 1234567, 9, GROUP);
    REQUIRE(lc.flco == FLCO_GRP_V_CH_USR);
    REQUIRE(lc.dst == ADDRESS_ALL);
    REQUIRE(lc.src == 1234567);
    REQUIRE(lc.serviceOptions == SO_BROADCAST);
    REQUIRE(lc.isAllCall());

    /* Same through the fallback path */
    FullLC fb = lcFromChannel(ch, NULL, 1234567, 9, ALL);
    REQUIRE(fb.dst == ADDRESS_ALL);
    REQUIRE(fb.flco == FLCO_GRP_V_CH_USR);
    REQUIRE(fb.serviceOptions == SO_BROADCAST);
}

TEST_CASE("DMR lcFromChannel falls back to the settings", "[dmr][cps]")
{
    channel_t ch = makeChannel(OPMODE_DMR);

    SECTION("no contact")
    {
        FullLC lc = lcFromChannel(ch, NULL, 1234567, 9, GROUP);
        REQUIRE(lc.flco == FLCO_GRP_V_CH_USR);
        REQUIRE(lc.dst == 9);
        REQUIRE(lc.src == 1234567);

        FullLC pvt = lcFromChannel(ch, NULL, 1234567, 7654321, PRIVATE);
        REQUIRE(pvt.flco == FLCO_UU_V_CH_USR);
        REQUIRE(pvt.dst == 7654321);
    }

    SECTION("contact of another mode is ignored")
    {
        contact_t ct = makeContact(OPMODE_M17, 91, GROUP);
        FullLC lc = lcFromChannel(ch, &ct, 1234567, 9, GROUP);
        REQUIRE(lc.dst == 9);
    }

    SECTION("contact on a non-DMR channel is ignored")
    {
        channel_t fm = makeChannel(OPMODE_FM);
        contact_t ct = makeContact(OPMODE_DMR, 91, GROUP);
        FullLC lc = lcFromChannel(fm, &ct, 1234567, 9, GROUP);
        REQUIRE(lc.dst == 9);
    }

    SECTION("unknown call type is a group call")
    {
        FullLC lc = lcFromChannel(ch, NULL, 1234567, 9, 0xFF);
        REQUIRE(lc.flco == FLCO_GRP_V_CH_USR);
        REQUIRE(lc.dst == 9);
    }

    SECTION("addresses are truncated to 24 bits")
    {
        FullLC lc = lcFromChannel(ch, NULL, 0xFF000001, 0xFF000009, GROUP);
        REQUIRE(lc.src == 1);
        REQUIRE(lc.dst == 9);
    }
}
