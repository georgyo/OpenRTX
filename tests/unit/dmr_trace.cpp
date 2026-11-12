/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>
#include "core/dmr_trace.h"

/* Empty the buffer, whatever the tests before left in it */
static void drain()
{
    char line[DMR_TRACE_LINE_BYTES];
    while (dmrTrace_pop(line, sizeof(line)) != 0)
        ;
    (void)dmrTrace_dumpRequested();
}

static std::string pop()
{
    char line[DMR_TRACE_LINE_BYTES];
    size_t len = dmrTrace_pop(line, sizeof(line));
    return std::string(line, len);
}

TEST_CASE("DMR trace records come out as text lines in order", "[dmr][trace]")
{
    drain();

    const uint8_t a[] = { 0x00, 0x1F, 0xA5 };
    const uint8_t b[] = { 0xFF };
    dmrTrace_put('S', 0x00123456, a, sizeof(a));
    dmrTrace_put('T', 0xFF000001, b, sizeof(b));
    dmrTrace_put('W', 7, nullptr, 0);

    /* Kind, 24 bits of time, data, line feed: no separators */
    REQUIRE(pop() == "S123456001FA5\n");
    REQUIRE(pop() == "T000001FF\n");
    REQUIRE(pop() == "W000007\n");
    REQUIRE(pop().empty());
}

TEST_CASE("DMR trace longest record fits one full-speed USB packet",
          "[dmr][trace]")
{
    drain();

    uint8_t d[DMR_TRACE_DATA_BYTES + 4];
    for (size_t i = 0; i < sizeof(d); i++)
        d[i] = static_cast<uint8_t>(i);

    /* Longer than a record: cut to DMR_TRACE_DATA_BYTES */
    dmrTrace_put('V', 1, d, sizeof(d));
    std::string line = pop();
    REQUIRE(line.size() == DMR_TRACE_LINE_BYTES);
    REQUIRE(line.size() <= 64);
    REQUIRE(line.substr(line.size() - 3) == "1A\n");

    /* A destination too small for the longest line is refused */
    dmrTrace_put('V', 1, d, 1);
    char small[DMR_TRACE_LINE_BYTES - 1];
    REQUIRE(dmrTrace_pop(small, sizeof(small)) == 0);
    REQUIRE(pop() == "V00000100\n");
}

TEST_CASE("DMR trace counts the records lost on a full buffer", "[dmr][trace]")
{
    drain();

    const uint8_t d = 0x42;
    for (unsigned i = 0; i < 256; i++)
        dmrTrace_put('T', i, &d, 1);

    /* Buffer full: three more are lost, the ones inside are kept */
    dmrTrace_put('T', 1000, &d, 1);
    dmrTrace_put('T', 1001, &d, 1);
    dmrTrace_put('T', 1002, &d, 1);

    REQUIRE(pop() == "D00000003\n");
    REQUIRE(pop() == "T00000042\n");

    unsigned left = 0;
    std::string last;
    for (std::string l = pop(); !l.empty(); l = pop()) {
        last = l;
        left++;
    }
    REQUIRE(left == 255);
    REQUIRE(last == "T0000FF42\n");

    /* Room again */
    dmrTrace_put('T', 2, &d, 1);
    REQUIRE(pop() == "T00000242\n");
}

TEST_CASE("DMR trace dump request is reported once", "[dmr][trace]")
{
    drain();

    REQUIRE(dmrTrace_dumpRequested() == false);
    dmrTrace_requestDump();
    REQUIRE(dmrTrace_dumpRequested() == true);
    REQUIRE(dmrTrace_dumpRequested() == false);
}
