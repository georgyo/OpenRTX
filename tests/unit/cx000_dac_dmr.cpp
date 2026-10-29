/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host test of the HR_Cx000 DAC driver in DMR mode, built against the fake
 * HR_C6000 in tests/unit/fake: with the chip programmed for DMR, register
 * 0x06 holds the vocoder interface selection (0x21: V_SPI vocoder under MCU
 * control) and the "OpenMusic" bit 1 used for beeps and audio streams must be
 * set and cleared without touching the other bits, otherwise a key beep would
 * silently switch the vocoder port off. The DMR modem driver announces its
 * ownership of the chip with Cx000dac_setVocoderBits(): set, the driver
 * toggles the bit with a read-modify-write (0x23 / 0x21); clear, FM and M17
 * keep the wholesale 0x22 / 0x20 they have always written.
 */

#include <catch2/catch_test_macros.hpp>
#include <string.h>
#include "drivers/baseband/HR_C6000.h"
#include "Cx000_dac.h"

#ifndef FAKE_HR_C6000
#error "The fake HR_C6000 header must shadow the real one in this test"
#endif

struct FakeC6000State fakeC6000;

static HR_C6000 dev;
static struct streamCtx ctx;
static int16_t buf[320];

static void setup()
{
    memset(&fakeC6000, 0, sizeof(fakeC6000));
    memset(&ctx, 0, sizeof(ctx));
    ctx.buffer = buf;
    ctx.bufSize = 320;
    ctx.bufMode = BUF_CIRC_DOUBLE;
    ctx.sampleRate = 8000;

    // Register 0x06 as dmrMode() leaves it, the DMR driver owning the chip
    fakeC6000.reg06 = 0x21;
    Cx000dac_setVocoderBits(true);
}

/*
 * Return the value of the n-th write of register 0x06, or 0xFF if there
 * were fewer writes than that.
 */
static uint8_t writeOf06(size_t n)
{
    size_t seen = 0;
    for (size_t i = 0; (i < fakeC6000.cfgWrites) && (i < FAKE_C6000_LOG_SIZE);
         i++) {
        if (fakeC6000.cfgLog[i][0] != 0x06)
            continue;

        if (seen == n)
            return fakeC6000.cfgLog[i][1];

        seen += 1;
    }

    return 0xFF;
}

TEST_CASE("Cx000 DAC in DMR mode: a beep only toggles bit 1 of register 0x06",
          "[audio][cx000dac][dmr]")
{
    setup();
    Cx000dac_init(&dev);

    REQUIRE(Cx000dac_startBeep(1000) == 0);
    REQUIRE(fakeC6000.reg06 == 0x23);
    REQUIRE(writeOf06(0) == 0x23);

    Cx000dac_stopBeep();
    REQUIRE(fakeC6000.reg06 == 0x21);
    REQUIRE(writeOf06(1) == 0x21);

    // Exactly the two writes above touched 0x06
    REQUIRE(writeOf06(2) == 0xFF);

    Cx000dac_terminate();
}

TEST_CASE("Cx000 DAC in DMR mode: an audio stream keeps the vocoder bits",
          "[audio][cx000dac][dmr]")
{
    setup();
    Cx000dac_init(&dev);

    REQUIRE(Cx000_dac_audio_driver.start(0, NULL, &ctx) == 0);
    REQUIRE(fakeC6000.reg06 == 0x23);

    // The stream is fed, the register is left alone
    for (int i = 0; i < 5; i++)
        Cx000dac_task();

    REQUIRE(fakeC6000.reg06 == 0x23);

    Cx000_dac_audio_driver.terminate(&ctx);
    REQUIRE(ctx.running == 0);
    REQUIRE(fakeC6000.reg06 == 0x21);

    Cx000dac_terminate();
}

TEST_CASE("Cx000 DAC in DMR mode: a stopped stream restores the FM value too",
          "[audio][cx000dac][dmr]")
{
    // Same driver, register 0x06 as fmMode() would leave it on a chip that
    // sets 0x20 there: nothing but bit 1 changes in that case either.
    setup();
    fakeC6000.reg06 = 0x20;
    Cx000dac_init(&dev);

    REQUIRE(Cx000dac_startBeep(1000) == 0);
    REQUIRE(fakeC6000.reg06 == 0x22);
    Cx000dac_stopBeep();
    REQUIRE(fakeC6000.reg06 == 0x20);

    Cx000dac_terminate();
}

TEST_CASE("Cx000 DAC without the DMR driver writes 0x22 / 0x20 wholesale",
          "[audio][cx000dac][dmr]")
{
    // The DMR driver terminated: FM and M17 on the CS7000 leave 0x21 in
    // the register from init(), and the beep must write exactly what it
    // wrote before the DMR series, whatever the register holds.
    setup();
    Cx000dac_setVocoderBits(false);
    Cx000dac_init(&dev);

    REQUIRE(Cx000dac_startBeep(1000) == 0);
    REQUIRE(fakeC6000.reg06 == 0x22);
    REQUIRE(writeOf06(0) == 0x22);
    Cx000dac_stopBeep();
    REQUIRE(fakeC6000.reg06 == 0x20);
    REQUIRE(writeOf06(1) == 0x20);

    REQUIRE(Cx000_dac_audio_driver.start(0, NULL, &ctx) == 0);
    REQUIRE(fakeC6000.reg06 == 0x22);
    Cx000_dac_audio_driver.terminate(&ctx);
    REQUIRE(fakeC6000.reg06 == 0x20);
    REQUIRE(writeOf06(4) == 0xFF);

    // The flag set again: back to the read-modify-write
    Cx000dac_setVocoderBits(true);
    fakeC6000.reg06 = 0x21;
    REQUIRE(Cx000dac_startBeep(1000) == 0);
    REQUIRE(fakeC6000.reg06 == 0x23);
    Cx000dac_stopBeep();
    REQUIRE(fakeC6000.reg06 == 0x21);

    Cx000dac_terminate();
}
