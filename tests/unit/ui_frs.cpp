/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <pthread.h>

extern "C" {
#include "core/event.h"
#include "core/frs.h"
#include "core/input.h"
#include "core/settings.h"
#include "core/state.h"
#include "interfaces/delays.h"
#include "interfaces/keyboard.h"
#include "rtx/rtx.h"
#include "ui/ui_default.h"
}

/*
 * Drives the default UI state machine of the Linux build (160x128, GPS, RTC
 * and M17 enabled) through the FRS screens with synthetic key events. The
 * screen is never drawn: only ui_updateFSM() and the resulting radio state
 * are exercised.
 */

static const freq_t VFO_FREQ = 430000000;

/* Press (and release) a key combination, return the RTX sync request. */
static bool press(uint32_t keys)
{
    kbd_msg_t msg;
    msg.value = 0;
    msg.keys = keys;
    ui_pushEvent(EVENT_KBD, msg.value);

    bool sync = false;
    ui_updateFSM(&sync);
    ui_saveState();

    /* Key release */
    ui_pushEvent(EVENT_KBD, 0);
    bool dummy = false;
    ui_updateFSM(&dummy);
    ui_saveState();

    return sync;
}

static void pressN(uint32_t keys, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        press(keys);
}

/* Periodic status event, as pushed by state_task() every 100 ms. */
static bool status_event()
{
    ui_pushEvent(EVENT_STATUS, 0);
    bool sync = false;
    ui_updateFSM(&sync);
    ui_saveState();
    return sync;
}

/* Fresh radio on the VFO screen, with the given settings applied. */
static void boot(const settings_t &settings)
{
    static bool rtxReady = false;
    static pthread_mutex_t rtxMutex = PTHREAD_MUTEX_INITIALIZER;

    if (!rtxReady) {
        rtx_init(&rtxMutex);
        rtxReady = true;
    }

    memset(&state, 0, sizeof(state));
    state.charge = 100;
    state.v_bat = 8000;
    state.settings = settings;
    state.channel = cps_getDefaultChannel();
    state.vfo_channel = state.channel;
    state.ui_screen = MAIN_VFO;
    ui_init();
    ui_saveState();
}

static void boot()
{
    boot(default_settings);
}

/* From a main screen: Menu > Settings > FRS */
static void goto_frs_settings()
{
    press(KEY_ENTER);
    REQUIRE(state.ui_screen == MENU_TOP);
    pressN(KEY_DOWN, M_SETTINGS);
    press(KEY_ENTER);
    REQUIRE(state.ui_screen == MENU_SETTINGS);
    pressN(KEY_DOWN, S_FRS);
    press(KEY_ENTER);
    REQUIRE(state.ui_screen == SETTINGS_FRS);
}

/* Fresh radio with FRS mode enabled through the settings menu. */
static void boot_frs()
{
    boot();
    goto_frs_settings();
    press(KEY_ENTER);
    press(KEY_UP);
    press(KEY_ENTER);
    pressN(KEY_ESC, 3);
    REQUIRE(state.settings.frs_mode == 1);
    REQUIRE(state.ui_screen == MAIN_FRS);
}

static uint8_t current_code()
{
    return state.settings.frs_codes[state.settings.frs_channel];
}

TEST_CASE("FRS mode resumes at boot", "[ui][frs]")
{
    settings_t settings = default_settings;
    settings.frs_mode = 1;
    settings.frs_channel = 4;
    settings.frs_codes[4] = 7;
    boot(settings);

    REQUIRE(state.ui_screen == MAIN_FRS);
    REQUIRE(state.channel.rx_frequency == 462662500);
    REQUIRE(state.channel.tx_frequency == 462662500);
    REQUIRE(state.channel.fm.txTone == frs_codeToCtcss(7));
    REQUIRE(state.channel.fm.txToneEn == 1);
    /* The VFO loaded from NVM is parked, not lost */
    REQUIRE(state.vfo_channel.rx_frequency == VFO_FREQ);

    /* Menu exit lands back on the FRS screen */
    press(KEY_ENTER);
    REQUIRE(state.ui_screen == MENU_TOP);
    press(KEY_ESC);
    REQUIRE(state.ui_screen == MAIN_FRS);
}

TEST_CASE("FRS mode is enabled from the settings menu", "[ui][frs]")
{
    boot();
    goto_frs_settings();

    press(KEY_ENTER); /* edit mode */
    bool sync = press(KEY_UP);
    REQUIRE(sync == true);
    REQUIRE(state.settings.frs_mode == 1);
    REQUIRE(state.settings.frs_channel == 0);
    REQUIRE(state.channel.rx_frequency == 462562500);
    REQUIRE(state.channel.tx_frequency == 462562500);
    REQUIRE(state.channel.mode == OPMODE_FM);
    REQUIRE(state.channel.bandwidth == BW_12_5);
    REQUIRE(state.channel.power == 2000);
    REQUIRE(state.channel.rx_only == 0);
    REQUIRE(state.vfo_channel.rx_frequency == VFO_FREQ);
    press(KEY_ENTER); /* leave edit mode */

    press(KEY_ESC);
    REQUIRE(state.ui_screen == MENU_SETTINGS);
    press(KEY_ESC);
    REQUIRE(state.ui_screen == MENU_TOP);
    press(KEY_ESC);
    REQUIRE(state.ui_screen == MAIN_FRS);
}

TEST_CASE("FRS channel stepping wraps at both ends", "[ui][frs]")
{
    boot_frs();

    REQUIRE(press(KNOB_RIGHT) == true);
    REQUIRE(state.settings.frs_channel == 1);
    REQUIRE(state.channel.rx_frequency == 462587500);

    press(KEY_DOWN);
    press(KEY_DOWN);
    REQUIRE(state.settings.frs_channel == 21);
    REQUIRE(state.channel.rx_frequency == 462725000);

    press(KEY_UP);
    REQUIRE(state.settings.frs_channel == 0);

    press(KNOB_LEFT);
    REQUIRE(state.settings.frs_channel == 21);
    press(KNOB_RIGHT);
    REQUIRE(state.settings.frs_channel == 0);
}

TEST_CASE("FRS channel keypad entry", "[ui][frs]")
{
    boot_frs();

    SECTION("Two digits select a channel")
    {
        press(KEY_1);
        REQUIRE(state.ui_screen == MAIN_FRS_INPUT);
        REQUIRE(state.settings.frs_channel == 0);
        press(KEY_2);
        REQUIRE(state.ui_screen == MAIN_FRS);
        REQUIRE(state.settings.frs_channel == 11);
        REQUIRE(state.channel.rx_frequency == 467662500);
        REQUIRE(state.channel.power == 500);
    }

    SECTION("Digits 3-9 select at once")
    {
        press(KEY_7);
        REQUIRE(state.ui_screen == MAIN_FRS);
        REQUIRE(state.settings.frs_channel == 6);
        REQUIRE(state.channel.rx_frequency == 462712500);
    }

    SECTION("Channels 10 and 20 are reachable")
    {
        press(KEY_1);
        press(KEY_0);
        REQUIRE(state.settings.frs_channel == 9);
        press(KEY_2);
        press(KEY_0);
        REQUIRE(state.settings.frs_channel == 19);
        press(KEY_2);
        press(KEY_2);
        REQUIRE(state.settings.frs_channel == 21);
    }

    SECTION("Invalid second digit restarts the entry, ENTER accepts")
    {
        press(KEY_2);
        press(KEY_5);
        REQUIRE(state.ui_screen == MAIN_FRS_INPUT);
        REQUIRE(state.settings.frs_channel == 0);
        press(KEY_ENTER);
        REQUIRE(state.ui_screen == MAIN_FRS);
        REQUIRE(state.settings.frs_channel == 4);
    }

    SECTION("ESC cancels, zero is refused")
    {
        press(KEY_1);
        press(KEY_ESC);
        REQUIRE(state.ui_screen == MAIN_FRS);
        REQUIRE(state.settings.frs_channel == 0);

        press(KEY_0);
        REQUIRE(state.ui_screen == MAIN_FRS);
        REQUIRE(state.settings.frs_channel == 0);

        press(KEY_2);
        press(KEY_0);
        REQUIRE(state.settings.frs_channel == 19);
    }

    SECTION("Pending digit is accepted after the timeout")
    {
        press(KEY_2);
        REQUIRE(state.ui_screen == MAIN_FRS_INPUT);
        REQUIRE(state.txDisable == true);

        status_event();
        REQUIRE(state.ui_screen == MAIN_FRS_INPUT);

        sleepFor(0, FRS_INPUT_TIMEOUT + 100);
        bool sync = status_event();
        REQUIRE(sync == true);
        REQUIRE(state.ui_screen == MAIN_FRS);
        REQUIRE(state.settings.frs_channel == 1);
        REQUIRE(state.txDisable == false);
    }
}

TEST_CASE("FRS privacy code picker", "[ui][frs]")
{
    boot_frs();

    SECTION("Digits and ENTER set a code")
    {
        press(KEY_STAR);
        REQUIRE(state.ui_screen == FRS_CODE);
        REQUIRE(state.txDisable == true);
        press(KEY_1);
        press(KEY_2);
        bool sync = press(KEY_ENTER);
        REQUIRE(sync == true);
        REQUIRE(state.ui_screen == MAIN_FRS);
        REQUIRE(current_code() == 12);
        REQUIRE(state.channel.fm.txTone == 12);
        REQUIRE(state.channel.fm.rxTone == 12);
        REQUIRE(state.channel.fm.txToneEn == 1);
        /* No tone decoder on the Linux target */
        REQUIRE(state.channel.fm.rxToneEn == 0);
        REQUIRE(state.txDisable == false);
    }

    SECTION("ESC and * cancel")
    {
        press(KEY_STAR);
        press(KEY_DOWN);
        press(KEY_ESC);
        REQUIRE(state.ui_screen == MAIN_FRS);
        REQUIRE(current_code() == 0);

        press(KEY_STAR);
        press(KEY_DOWN);
        press(KEY_STAR);
        REQUIRE(state.ui_screen == MAIN_FRS);
        REQUIRE(current_code() == 0);
    }

    SECTION("Zero turns the code off")
    {
        press(KEY_STAR);
        press(KEY_5);
        press(KEY_ENTER);
        REQUIRE(current_code() == 5);
        REQUIRE(state.channel.fm.txToneEn == 1);

        press(KEY_STAR);
        press(KEY_0);
        press(KEY_ENTER);
        REQUIRE(current_code() == 0);
        REQUIRE(state.channel.fm.txToneEn == 0);
        REQUIRE(state.channel.fm.txTone == 0);
    }

    SECTION("A digit that cannot extend the entry restarts it")
    {
        press(KEY_STAR);
        press(KEY_3);
        press(KEY_9);
        press(KEY_ENTER);
        REQUIRE(current_code() == 9);

        press(KEY_STAR);
        press(KEY_3);
        press(KEY_8);
        press(KEY_ENTER);
        REQUIRE(current_code() == 38);
        REQUIRE(state.channel.fm.txTone == frs_code_ctcss[37]);
    }

    SECTION("The picker opens on the current code")
    {
        press(KEY_STAR);
        press(KEY_2);
        press(KEY_0);
        press(KEY_ENTER);
        REQUIRE(current_code() == 20);

        press(KEY_F2);
        REQUIRE(state.ui_screen == FRS_CODE);
        press(KEY_DOWN);
        press(KEY_ENTER);
        REQUIRE(current_code() == 21);

        press(KEY_STAR);
        press(KEY_UP);
        press(KEY_UP);
        press(KEY_ENTER);
        REQUIRE(current_code() == 19);
    }
}

TEST_CASE("FRS macro menu keys", "[ui][frs]")
{
    boot_frs();

    press(KEY_MONI | KEY_3);
    REQUIRE(current_code() == 1);
    REQUIRE(state.channel.fm.txTone == frs_code_ctcss[0]);
    press(KEY_MONI | KEY_3);
    REQUIRE(current_code() == 2);
    press(KEY_MONI | KEY_2);
    press(KEY_MONI | KEY_2);
    press(KEY_MONI | KEY_2);
    REQUIRE(current_code() == FRS_CODE_NUM);
    press(KEY_MONI | KEY_3);
    REQUIRE(current_code() == 0);

    /* Tone mode, bandwidth, opmode and power are fixed */
    press(KEY_MONI | KEY_1);
    REQUIRE(state.channel.fm.txToneEn == 0);
    REQUIRE(state.channel.fm.rxToneEn == 0);
    press(KEY_MONI | KEY_4);
    REQUIRE(state.channel.bandwidth == BW_12_5);
    press(KEY_MONI | KEY_5);
    REQUIRE(state.channel.mode == OPMODE_FM);
    press(KEY_MONI | KEY_6);
    REQUIRE(state.channel.power == 2000);

    REQUIRE(state.ui_screen == MAIN_FRS);
}

TEST_CASE("FRS privacy codes are remembered per channel", "[ui][frs]")
{
    boot_frs();

    press(KEY_3); /* channel 3 */
    press(KEY_STAR);
    press(KEY_5);
    press(KEY_ENTER);
    REQUIRE(state.settings.frs_codes[2] == 5);

    press(KEY_UP); /* channel 4 */
    REQUIRE(state.settings.frs_channel == 3);
    REQUIRE(state.channel.fm.txToneEn == 0);

    press(KEY_DOWN); /* channel 3 */
    REQUIRE(state.settings.frs_channel == 2);
    REQUIRE(state.channel.fm.txToneEn == 1);
    REQUIRE(state.channel.fm.txTone == frs_code_ctcss[4]);
}

TEST_CASE("FRS mode locks the channel", "[ui][frs]")
{
    boot_frs();
    freq_t freq = state.channel.rx_frequency;

    SECTION("Codeplug channels cannot be loaded")
    {
        press(KEY_ENTER);
        pressN(KEY_DOWN, M_CHANNEL);
        press(KEY_ENTER);
        REQUIRE(state.ui_screen == MENU_CHANNEL);
        press(KEY_ENTER);
        REQUIRE(state.ui_screen == MENU_CHANNEL);
        REQUIRE(state.channel.rx_frequency == freq);
        press(KEY_ESC);
        press(KEY_ESC);
        REQUIRE(state.ui_screen == MAIN_FRS);

        press(KEY_ENTER);
        pressN(KEY_DOWN, M_BANK);
        press(KEY_ENTER);
        REQUIRE(state.ui_screen == MENU_BANK);
        press(KEY_ENTER);
        REQUIRE(state.ui_screen == MENU_BANK);
        REQUIRE(state.channel.rx_frequency == freq);
    }

    SECTION("ESC and # do nothing on the FRS screen")
    {
        press(KEY_HASH);
        REQUIRE(state.tone_enabled == false);
        REQUIRE(state.ui_screen == MAIN_FRS);
        press(KEY_ESC);
        REQUIRE(state.ui_screen == MAIN_FRS);
        REQUIRE(state.channel.rx_frequency == freq);
    }

    SECTION("FM settings are read-only")
    {
        press(KEY_ENTER);
        pressN(KEY_DOWN, M_SETTINGS);
        press(KEY_ENTER);
        pressN(KEY_DOWN, S_FM);
        press(KEY_ENTER);
        REQUIRE(state.ui_screen == SETTINGS_FM);
        press(KEY_ENTER);
        press(KEY_UP);
        REQUIRE(state.channel.fm.txTone == 0);
        REQUIRE(state.channel.fm.txToneEn == 0);
    }

    SECTION("Radio settings are read-only")
    {
        press(KEY_ENTER);
        pressN(KEY_DOWN, M_SETTINGS);
        press(KEY_ENTER);
        pressN(KEY_DOWN, S_RADIO);
        press(KEY_ENTER);
        REQUIRE(state.ui_screen == SETTINGS_RADIO);
        press(KEY_ENTER);
        press(KEY_1);
        press(KEY_ENTER);
        REQUIRE(state.channel.tx_frequency == freq);
        press(KEY_DOWN);
        press(KEY_ENTER);
        press(KEY_UP);
        REQUIRE(state.channel.tx_frequency == freq);
    }
}

TEST_CASE("A refused key raises the FRS marker", "[ui][frs]")
{
    boot_frs();
    REQUIRE(frs_refuse_tick == 0);

    /* Settings > FM: the CTCSS tone row is locked by FRS mode */
    press(KEY_ENTER);
    pressN(KEY_DOWN, M_SETTINGS);
    press(KEY_ENTER);
    pressN(KEY_DOWN, S_FM);
    press(KEY_ENTER);
    REQUIRE(state.ui_screen == SETTINGS_FM);
    REQUIRE(_ui_frsRefuseMarkerVisible(getTick()) == false);

    long long before = getTick();
    press(KEY_ENTER);
    REQUIRE(state.ui_screen == SETTINGS_FM);
    REQUIRE(frs_refuse_tick >= before);
    REQUIRE(_ui_frsRefuseMarkerVisible(getTick()) == true);

    /* Browsing the list is allowed and does not touch the marker */
    long long refused = frs_refuse_tick;
    press(KEY_DOWN);
    REQUIRE(frs_refuse_tick == refused);

    /* The marker expires on its own */
    sleepFor(0, FRS_REFUSE_MARKER_TIME + 100);
    REQUIRE(_ui_frsRefuseMarkerVisible(getTick()) == false);

    /* Macro keys refused on the FRS screen raise it too */
    pressN(KEY_ESC, 3);
    REQUIRE(state.ui_screen == MAIN_FRS);
    before = getTick();
    press(KEY_MONI | KEY_5);
    REQUIRE(frs_refuse_tick >= before);
    REQUIRE(_ui_frsRefuseMarkerVisible(getTick()) == true);
}

TEST_CASE("FRS mode is disabled from the settings menu", "[ui][frs]")
{
    boot_frs();
    press(KEY_5);
    REQUIRE(state.settings.frs_channel == 4);

    goto_frs_settings();
    press(KEY_ENTER);
    bool sync = press(KEY_DOWN);
    REQUIRE(sync == true);
    REQUIRE(state.settings.frs_mode == 0);
    press(KEY_ENTER);
    pressN(KEY_ESC, 3);

    REQUIRE(state.ui_screen == MAIN_VFO);
    REQUIRE(state.channel.rx_frequency == VFO_FREQ);
    REQUIRE(state.channel.bandwidth == BW_25);
    /* The FRS channel is kept for the next time the mode is enabled */
    REQUIRE(state.settings.frs_channel == 4);
}

TEST_CASE("Reset Codes clears every privacy code", "[ui][frs]")
{
    boot_frs();
    press(KEY_STAR);
    press(KEY_7);
    press(KEY_ENTER);
    press(KEY_UP);
    press(KEY_STAR);
    press(KEY_8);
    press(KEY_ENTER);
    REQUIRE(state.settings.frs_codes[0] == 7);
    REQUIRE(state.settings.frs_codes[1] == 8);

    goto_frs_settings();
    press(KEY_DOWN);
    press(KEY_ENTER);
    bool sync = press(KEY_ENTER);
    REQUIRE(sync == true);
    for (size_t i = 0; i < FRS_CHANNEL_NUM; i++)
        REQUIRE(state.settings.frs_codes[i] == 0);
    REQUIRE(state.channel.fm.txToneEn == 0);
    REQUIRE(state.settings.frs_mode == 1);
}

TEST_CASE("Reset to defaults leaves FRS mode", "[ui][frs]")
{
    boot_frs();
    press(KEY_ENTER);
    pressN(KEY_DOWN, M_SETTINGS);
    press(KEY_ENTER);
    pressN(KEY_DOWN, S_RESET2DEFAULTS);
    press(KEY_ENTER);
    REQUIRE(state.ui_screen == SETTINGS_RESET2DEFAULTS);
    press(KEY_ENTER);
    bool sync = press(KEY_ENTER);
    REQUIRE(sync == true);
    REQUIRE(state.settings.frs_mode == 0);
    pressN(KEY_ESC, 2);
    REQUIRE(state.ui_screen == MAIN_VFO);
    REQUIRE(state.channel.rx_frequency == VFO_FREQ);
}

TEST_CASE("TX is enabled only on the FRS main screen", "[ui][frs]")
{
    boot_frs();
    press(KEY_UP);
    REQUIRE(state.txDisable == false);

    press(KEY_STAR);
    REQUIRE(state.ui_screen == FRS_CODE);
    REQUIRE(state.txDisable == true);
    press(KEY_ESC);
    REQUIRE(state.ui_screen == MAIN_FRS);
    REQUIRE(state.txDisable == false);

    press(KEY_1);
    REQUIRE(state.ui_screen == MAIN_FRS_INPUT);
    REQUIRE(state.txDisable == true);
    press(KEY_ESC);
    REQUIRE(state.txDisable == false);
}
