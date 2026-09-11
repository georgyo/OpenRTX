/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KNOB_DIRECTION_H
#define KNOB_DIRECTION_H

#include <stdint.h>
#include "interfaces/keyboard.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \brief Compute the rotation direction of an absolute-position knob.
 *
 * The knob is read as an absolute position code and compared with the value
 * of the previous poll. The direction is derived from the modular distance
 * between the two readings, so that a wrap-around (e.g. from the last detent
 * to the first one) and a move of more than one detent between two polls are
 * both reported correctly, as long as the knob moved less than half a turn.
 *
 * @param pos: current knob position, in [1, positions].
 * @param prev: knob position at the previous poll, in [1, positions].
 * @param positions: total number of knob positions (detents).
 * @return KNOB_RIGHT for a clockwise move, KNOB_LEFT for a counter-clockwise
 * one, zero if the position did not change.
 */
static inline keyboard_t kbd_knobDirection(const uint8_t pos,
                                           const uint8_t prev,
                                           const uint8_t positions)
{
    if (pos == prev)
        return 0;

    // Modular forward distance, in [1, positions - 1]
    uint8_t delta = (uint8_t)((pos + positions - prev) % positions);

    if (delta < (positions / 2))
        return KNOB_RIGHT;

    return KNOB_LEFT;
}

#ifdef __cplusplus
}
#endif

#endif /* KNOB_DIRECTION_H */
