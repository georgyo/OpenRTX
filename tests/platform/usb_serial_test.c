/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "core/graphics.h"
#include "hwconfig.h"
#include "interfaces/platform.h"
#include "interfaces/delays.h"
#include "interfaces/display.h"
#include "interfaces/usb_serial.h"

// NOTE: ENABLE_STDIO needs to be enabled for this test to actually do
// its job (add 'ENABLE_STDIO': '' to cs7000p_def in meson.build); without
// it printf() output is discarded and only the display shows activity.

int main(void)
{
    platform_init();
    usb_serial_init(); // Should be called in the main thread with real OpenRTX
    gfx_init();

    display_setBacklightLevel(255);

    int i = 0;
    int pass = 0;

    while (1) {
        // The control transfers of the enumeration advance one stage per
        // call: service the stack every 5 ms, as the firmware does.
        usb_serial_task();
        sleepFor(0, 5u);

        pass += 1;
        if (pass < 50)
            continue;

        pass = 0;
        gfx_clearScreen();

        point_t pos_line = { 5, 15 };
        color_t color_white = { 255, 255, 255, 255 };

        gfx_print(pos_line, FONT_SIZE_6PT, TEXT_ALIGN_LEFT, color_white,
                  "Connect to PC for serial");

        gfx_render();

        printf("Hello via serial #%d\n", i);
        i++;
    }

    return 0;
}
