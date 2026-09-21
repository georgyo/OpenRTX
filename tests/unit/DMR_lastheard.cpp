/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstring>

extern "C" {
#include "core/dmr_lastheard.h"
#include "core/settings.h"
#include "core/state.h"
}

/*
 * The DMR last heard ring (core/dmr_lastheard.c): insertion order,
 * de-duplication, depth and the private call-back selection the UI makes
 * from it (settings.dmr_talkgroup = source, call type PRIVATE).
 */

static void fill(unsigned n, uint32_t base = 1000)
{
    for (unsigned i = 0; i < n; i++)
        dmrLastHeard_insert(base + i, 9, 0, 100 * (i + 1));
}

TEST_CASE("Last heard: empty after init and clear", "[dmr][lastheard]")
{
    dmrLastHeard_init();
    REQUIRE(dmrLastHeard_count() == 0);

    struct dmrLastHeardEntry e;
    memset(&e, 0xAA, sizeof(e));
    REQUIRE(dmrLastHeard_get(0, &e) == false);
    REQUIRE(e.src == 0xAAAAAAAA);

    fill(3);
    REQUIRE(dmrLastHeard_count() == 3);
    dmrLastHeard_clear();
    REQUIRE(dmrLastHeard_count() == 0);
    REQUIRE(dmrLastHeard_get(0, &e) == false);
}

TEST_CASE("Last heard: newest first, oldest dropped", "[dmr][lastheard]")
{
    dmrLastHeard_init();

    dmrLastHeard_insert(2345678, 31665, 0, 1000);
    dmrLastHeard_insert(1234567, 9, 0, 2000);
    dmrLastHeard_insert(7654321, 2345678, 3, 3000);
    REQUIRE(dmrLastHeard_count() == 3);

    struct dmrLastHeardEntry e;
    REQUIRE(dmrLastHeard_get(0, &e) == true);
    REQUIRE(e.src == 7654321);
    REQUIRE(e.dst == 2345678);
    REQUIRE(e.flco == 3);
    REQUIRE(e.timestamp == 3000);
    REQUIRE(dmrLastHeard_get(1, &e) == true);
    REQUIRE(e.src == 1234567);
    REQUIRE(e.dst == 9);
    REQUIRE(dmrLastHeard_get(2, &e) == true);
    REQUIRE(e.src == 2345678);
    REQUIRE(e.dst == 31665);
    REQUIRE(e.timestamp == 1000);
    REQUIRE(dmrLastHeard_get(3, &e) == false);

    /* Ten distinct calls: the list holds the last eight */
    dmrLastHeard_clear();
    fill(DMR_LASTHEARD_NUM + 2);
    REQUIRE(dmrLastHeard_count() == DMR_LASTHEARD_NUM);
    REQUIRE(dmrLastHeard_get(0, &e) == true);
    REQUIRE(e.src == 1000 + DMR_LASTHEARD_NUM + 1);
    REQUIRE(dmrLastHeard_get(DMR_LASTHEARD_NUM - 1, &e) == true);
    REQUIRE(e.src == 1002);
    REQUIRE(dmrLastHeard_get(DMR_LASTHEARD_NUM, &e) == false);
}

TEST_CASE("Last heard: a repeated call moves to the top", "[dmr][lastheard]")
{
    dmrLastHeard_init();
    fill(4);
    REQUIRE(dmrLastHeard_count() == 4);

    /* Same source and destination as the third newest entry */
    dmrLastHeard_insert(1001, 9, 0, 9000);
    REQUIRE(dmrLastHeard_count() == 4);

    struct dmrLastHeardEntry e;
    REQUIRE(dmrLastHeard_get(0, &e) == true);
    REQUIRE(e.src == 1001);
    REQUIRE(e.timestamp == 9000);
    REQUIRE(dmrLastHeard_get(1, &e) == true);
    REQUIRE(e.src == 1003);
    REQUIRE(dmrLastHeard_get(2, &e) == true);
    REQUIRE(e.src == 1002);
    REQUIRE(dmrLastHeard_get(3, &e) == true);
    REQUIRE(e.src == 1000);

    /* Same source to another destination is a distinct call */
    dmrLastHeard_insert(1001, 31665, 0, 9500);
    REQUIRE(dmrLastHeard_count() == 5);
    REQUIRE(dmrLastHeard_get(0, &e) == true);
    REQUIRE(e.dst == 31665);
    REQUIRE(dmrLastHeard_get(1, &e) == true);
    REQUIRE(e.src == 1001);
    REQUIRE(e.dst == 9);

    /* The newest entry repeated: nothing moves, the timestamp refreshes */
    dmrLastHeard_insert(1001, 31665, 0, 9900);
    REQUIRE(dmrLastHeard_count() == 5);
    REQUIRE(dmrLastHeard_get(0, &e) == true);
    REQUIRE(e.timestamp == 9900);
    REQUIRE(dmrLastHeard_get(1, &e) == true);
    REQUIRE(e.dst == 9);

    /* A full list: the repeated oldest entry comes to the top, nothing
     * is dropped */
    dmrLastHeard_clear();
    fill(DMR_LASTHEARD_NUM);
    dmrLastHeard_insert(1000, 9, 0, 5555);
    REQUIRE(dmrLastHeard_count() == DMR_LASTHEARD_NUM);
    REQUIRE(dmrLastHeard_get(0, &e) == true);
    REQUIRE(e.src == 1000);
    REQUIRE(dmrLastHeard_get(DMR_LASTHEARD_NUM - 1, &e) == true);
    REQUIRE(e.src == 1001);
}

TEST_CASE("Last heard: call-back selection sets a private call",
          "[dmr][lastheard]")
{
    /*
     * The UI's ENTER on an entry does what is checked here with the
     * settings: the source of the entry becomes the destination of a
     * private call. The screen itself is exercised in ui_dmr.cpp.
     */
    dmrLastHeard_init();
    dmrLastHeard_insert(2345678, 31665, 0, 1000);
    dmrLastHeard_insert(1234567, 9, 0, 2000);

    state.settings = default_settings;
    REQUIRE(state.settings.dmr_callType == GROUP);

    struct dmrLastHeardEntry e;
    REQUIRE(dmrLastHeard_get(1, &e) == true);
    state.settings.dmr_talkgroup = e.src;
    state.settings.dmr_callType = PRIVATE;

    REQUIRE(state.settings.dmr_talkgroup == 2345678);
    REQUIRE(state.settings.dmr_callType == PRIVATE);
    REQUIRE(state.settings.dmr_talkgroup <= DMR_ID_MAX);
}
