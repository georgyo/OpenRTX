/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include "drivers/keyboard/knob_direction.h"

// Ground truth: signed modular distance in (-8, 8] for a 16 position knob.
static int signedDelta(int pos, int prev)
{
    int delta = (pos - prev) % 16;

    if (delta < 0)
        delta += 16;

    if (delta > 8)
        delta -= 16;

    return delta;
}

TEST_CASE("Knob direction: unchanged position reports no movement",
          "[keyboard]")
{
    for (uint8_t p = 1; p <= 16; p++)
        REQUIRE(kbd_knobDirection(p, p, 16) == 0);
}

TEST_CASE("Knob direction: single detent moves and wrap-around", "[keyboard]")
{
    REQUIRE(kbd_knobDirection(2, 1, 16) == KNOB_RIGHT);
    REQUIRE(kbd_knobDirection(1, 2, 16) == KNOB_LEFT);
    REQUIRE(kbd_knobDirection(1, 16, 16) == KNOB_RIGHT);
    REQUIRE(kbd_knobDirection(16, 1, 16) == KNOB_LEFT);
    REQUIRE(kbd_knobDirection(9, 8, 16) == KNOB_RIGHT);
    REQUIRE(kbd_knobDirection(8, 9, 16) == KNOB_LEFT);
}

TEST_CASE("Knob direction: multi-detent moves crossing position 8",
          "[keyboard]")
{
    // The former comparison-based logic reported these as the opposite
    // direction, because it mistook them for a wrap-around.
    REQUIRE(kbd_knobDirection(9, 7, 16) == KNOB_RIGHT);
    REQUIRE(kbd_knobDirection(7, 9, 16) == KNOB_LEFT);
    REQUIRE(kbd_knobDirection(10, 6, 16) == KNOB_RIGHT);
    REQUIRE(kbd_knobDirection(6, 10, 16) == KNOB_LEFT);

    // Wrap-around touching position 8 was not handled at all.
    REQUIRE(kbd_knobDirection(1, 15, 16) == KNOB_RIGHT);
    REQUIRE(kbd_knobDirection(15, 1, 16) == KNOB_LEFT);
    REQUIRE(kbd_knobDirection(2, 16, 16) == KNOB_RIGHT);
    REQUIRE(kbd_knobDirection(16, 2, 16) == KNOB_LEFT);
}

TEST_CASE("Knob direction: exhaustive check against modular ground truth",
          "[keyboard]")
{
    for (int prev = 1; prev <= 16; prev++) {
        for (int pos = 1; pos <= 16; pos++) {
            int delta = signedDelta(pos, prev);
            keyboard_t keys = kbd_knobDirection(pos, prev, 16);

            INFO("prev=" << prev << " pos=" << pos << " delta=" << delta);

            if (delta == 0) {
                REQUIRE(keys == 0);
            } else if ((delta > 0) && (delta < 8)) {
                REQUIRE(keys == KNOB_RIGHT);
            } else if (delta < 0) {
                REQUIRE(keys == KNOB_LEFT);
            } else {
                // Exactly half a turn is ambiguous: any single direction is
                // acceptable, but never both at once.
                REQUIRE((keys == KNOB_LEFT || keys == KNOB_RIGHT));
            }
        }
    }
}
