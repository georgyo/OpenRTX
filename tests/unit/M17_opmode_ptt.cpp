/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include "emulator.h"
#include "rtx/OpMode_M17.hpp"
#include "rtx/rtx.h"

/*
 * Headless test of the OpMode_M17 state machine on the Linux emulator.
 *
 * The emulator sources the RX baseband from /tmp/baseband.raw, which is the
 * path hardcoded in audio_linux.c: the file is created here (if absent) so
 * that the demodulator can open its input stream. PTT is driven through the
 * emulator_state.PTTstatus flag read by platform_getPttStatus().
 */

static const char *basebandFile = "/tmp/baseband.raw";

static void createBasebandFile()
{
    // Do not clobber a baseband capture a developer may be using with the
    // emulator: any existing file is good enough for this test.
    FILE *fp = fopen(basebandFile, "rb");
    if (fp != NULL) {
        fclose(fp);
        return;
    }

    fp = fopen(basebandFile, "wb");
    REQUIRE(fp != NULL);

    int16_t zeros[1024];
    memset(zeros, 0x00, sizeof(zeros));
    REQUIRE(fwrite(zeros, sizeof(int16_t), 1024, fp) == 1024);
    fclose(fp);
}

static rtxStatus_t initialStatus()
{
    rtxStatus_t status;
    memset(&status, 0x00, sizeof(status));
    status.opMode = OPMODE_M17;
    status.opStatus = OFF;
    status.txDisable = 0;
    strncpy(status.source_address, "N0CALL", sizeof(status.source_address));

    return status;
}

// Statically allocated to match its usage in rtx.cpp
static OpMode_M17 m17;

TEST_CASE("M17 OpMode: PTT with TX disabled does not leave RX", "[m17][rtx]")
{
    createBasebandFile();
    emulator_state.PTTstatus = false;

    rtxStatus_t status = initialStatus();
    m17.enable();

    // OFF -> RX on the first update after enable
    m17.update(&status, true);
    REQUIRE(status.opStatus == RX);

    // Settle in RX
    m17.update(&status, false);
    REQUIRE(status.opStatus == RX);

    // PTT pressed while the UI is in a menu (txDisable set): must stay in RX
    status.txDisable = 1;
    emulator_state.PTTstatus = true;
    for (int i = 0; i < 3; i++) {
        m17.update(&status, false);
        REQUIRE(status.opStatus == RX);
    }

    // PTT released, then back to the main screen: still RX
    emulator_state.PTTstatus = false;
    m17.update(&status, false);
    REQUIRE(status.opStatus == RX);

    status.txDisable = 0;
    m17.update(&status, false);
    REQUIRE(status.opStatus == RX);

    m17.disable();
}

TEST_CASE("M17 OpMode: OFF state returns to RX when PTT is not pressed",
          "[m17][rtx]")
{
    createBasebandFile();
    emulator_state.PTTstatus = false;

    rtxStatus_t status = initialStatus();
    m17.enable();

    m17.update(&status, true);
    REQUIRE(status.opStatus == RX);

    // PTT pressed with TX enabled: RX -> OFF, waiting for the TX to start
    emulator_state.PTTstatus = true;
    m17.update(&status, false);
    REQUIRE(status.opStatus == OFF);

    // TX gets inhibited before the OFF state is serviced: back to RX, not
    // stuck in OFF forever.
    status.txDisable = 1;
    m17.update(&status, false);
    REQUIRE(status.opStatus == RX);

    m17.update(&status, false);
    REQUIRE(status.opStatus == RX);

    // Same when the PTT is released while in OFF state
    status.txDisable = 0;
    m17.update(&status, false);
    REQUIRE(status.opStatus == OFF);

    emulator_state.PTTstatus = false;
    m17.update(&status, false);
    REQUIRE(status.opStatus == RX);

    m17.update(&status, false);
    REQUIRE(status.opStatus == RX);

    m17.disable();
}
