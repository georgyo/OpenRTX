/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <string>

extern "C" {
#include "core/event.h"
#include "core/graphics.h"
#include "core/input.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/ui.h"
#include "interfaces/delays.h"
#include "interfaces/dmr_baseband.h"
#include "interfaces/keyboard.h"
#include "rtx/rtx.h"
#include "ui/ui_default.h"
}
#include "emulator.h"

/*
 * Drives the default UI state machine of the Linux build (160x128, GPS, RTC,
 * M17 and DMR enabled) through the DMR mode selection, the main screen keys
 * and Settings > DMR with synthetic key events, in the style of ui_frs.cpp.
 * The main screen is also rendered into the framebuffer, without a display,
 * to make sure the DMR lines do not crash in any call state.
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

/* Type a decimal number on the keypad, most significant digit first. */
static void typeNumber(const char *digits)
{
    static const uint32_t keys[10] = { KEY_0, KEY_1, KEY_2, KEY_3, KEY_4,
                                       KEY_5, KEY_6, KEY_7, KEY_8, KEY_9 };

    for (const char *p = digits; *p != '\0'; p++)
        press(keys[*p - '0']);
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

/* Fresh radio with the VFO switched to DMR through the macro menu. */
static void boot_dmr()
{
    boot();
    press(KEY_MONI | KEY_5);
    REQUIRE(state.channel.mode == OPMODE_DMR);
    REQUIRE(state.ui_screen == MAIN_VFO);
}

/* From a main screen: Menu > Settings > DMR */
static void goto_dmr_settings()
{
    press(KEY_ENTER);
    REQUIRE(state.ui_screen == MENU_TOP);
    pressN(KEY_DOWN, M_SETTINGS);
    press(KEY_ENTER);
    REQUIRE(state.ui_screen == MENU_SETTINGS);
    pressN(KEY_DOWN, S_DMR);
    press(KEY_ENTER);
    REQUIRE(state.ui_screen == SETTINGS_DMR);
}

/*
 * Push the UI state into the RTX stage the way the UI thread does, and run
 * one RTX task pass so that the configuration is taken up.
 */
static rtxStatus_t sync_rtx_stage()
{
    static rtxStatus_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.opMode = state.channel.mode;
    cfg.bandwidth = state.channel.bandwidth;
    cfg.rxFrequency = state.channel.rx_frequency;
    cfg.txFrequency = state.channel.tx_frequency;
    cfg.txPower = state.channel.power;
    cfg.dmr_srcId = state.settings.dmr_id;
    cfg.dmr_dstId = state.settings.dmr_talkgroup;
    cfg.dmr_callType = state.settings.dmr_callType;
    cfg.dmr_rxColorCode = state.channel.dmr.rxColorCode;
    cfg.dmr_txColorCode = state.channel.dmr.txColorCode;
    cfg.dmr_timeslot = state.channel.dmr.dmr_timeslot;
    cfg.dmr_monitor = state.settings.dmr_monitor;
    cfg.dmr_polite = state.settings.dmr_polite;
    cfg.dmr_modeReg = state.dmr_modeReg;

    rtx_configure(&cfg);
    rtx_task();

    return rtx_getCurrentStatus();
}

TEST_CASE("settings_t fits an EEEP record and carries the DMR defaults",
          "[ui][dmr]")
{
    printf("sizeof(settings_t) = %zu\n", sizeof(settings_t));
    WARN("sizeof(settings_t) = " << sizeof(settings_t));
    REQUIRE(sizeof(settings_t) < 255);

    REQUIRE(default_settings.dmr_id == 0);
    REQUIRE(default_settings.dmr_talkgroup == 9);
    REQUIRE(default_settings.dmr_callType == GROUP);
    REQUIRE(default_settings.dmr_colorCode == 1);
    REQUIRE(default_settings.dmr_timeslot == 1);
    REQUIRE(default_settings.dmr_monitor == 0);
    REQUIRE(default_settings.dmr_polite == 1);
    REQUIRE(default_settings.dmr_hangTime == 3);
    REQUIRE(default_settings.dmr_testTone == 0);
    REQUIRE(DMR_ID_MAX == 16777215u);
}

TEST_CASE("Macro key 5 cycles FM, DMR and M17", "[ui][dmr]")
{
    boot();
    REQUIRE(state.channel.mode == OPMODE_FM);

    /* CTCSS on both sides of the FM VFO: the DMR block overlays it */
    state.channel.fm.rxToneEn = 1;
    state.channel.fm.rxTone = 12;
    state.channel.fm.txToneEn = 1;
    state.channel.fm.txTone = 25;
    ui_saveState();

    bool sync = press(KEY_MONI | KEY_5);
    REQUIRE(sync == true);
    REQUIRE(state.channel.mode == OPMODE_DMR);
    /* The VFO takes the colour code and timeslot of the settings */
    REQUIRE(state.channel.dmr.rxColorCode == 1);
    REQUIRE(state.channel.dmr.txColorCode == 1);
    REQUIRE(state.channel.dmr.dmr_timeslot == 1);
    REQUIRE(state.channel.dmr.contact_index == 0);

    rtxStatus_t status = sync_rtx_stage();
    REQUIRE(status.opMode == OPMODE_DMR);
    REQUIRE(status.dmr_dstId == 9);
    REQUIRE(status.dmr_callType == GROUP);
    REQUIRE(status.dmr_rxColorCode == 1);
    REQUIRE(status.dmr_timeslot == 1);
    REQUIRE(status.dmr_polite == 1);
    REQUIRE(status.dmr_callState == DMR_CALL_IDLE);

    /* The M17 opMode handler is not started here: it needs the emulator
     * baseband file, see M17_opmode_ptt.cpp */
    sync = press(KEY_MONI | KEY_5);
    REQUIRE(sync == true);
    REQUIRE(state.channel.mode == OPMODE_M17);

    sync = press(KEY_MONI | KEY_5);
    REQUIRE(sync == true);
    REQUIRE(state.channel.mode == OPMODE_FM);
    status = sync_rtx_stage();
    REQUIRE(status.opMode == OPMODE_FM);

    /* The FM tone settings survived the round trip */
    REQUIRE(state.channel.fm.rxToneEn == 1);
    REQUIRE(state.channel.fm.rxTone == 12);
    REQUIRE(state.channel.fm.txToneEn == 1);
    REQUIRE(state.channel.fm.txTone == 25);
}

TEST_CASE("A visit to DMR does not touch the FM tone settings", "[ui][dmr]")
{
    SECTION("Colour code and timeslot edited in DMR, back through M17")
    {
        boot();
        state.channel.fm.rxToneEn = 0;
        state.channel.fm.rxTone = 3;
        state.channel.fm.txToneEn = 1;
        state.channel.fm.txTone = 7;
        ui_saveState();

        press(KEY_MONI | KEY_5);
        REQUIRE(state.channel.mode == OPMODE_DMR);
        pressN(KEY_MONI | KEY_1, 5);
        press(KEY_MONI | KEY_2);
        REQUIRE(state.channel.dmr.rxColorCode == 6);
        REQUIRE(state.channel.dmr.dmr_timeslot == 2);

        press(KEY_MONI | KEY_5);
        REQUIRE(state.channel.mode == OPMODE_M17);
        press(KEY_MONI | KEY_5);
        REQUIRE(state.channel.mode == OPMODE_FM);
        REQUIRE(state.channel.fm.rxToneEn == 0);
        REQUIRE(state.channel.fm.rxTone == 3);
        REQUIRE(state.channel.fm.txToneEn == 1);
        REQUIRE(state.channel.fm.txTone == 7);

        /* A second visit with the tones changed in between */
        state.channel.fm.txTone = 9;
        ui_saveState();
        pressN(KEY_MONI | KEY_5, 3);
        REQUIRE(state.channel.mode == OPMODE_FM);
        REQUIRE(state.channel.fm.txTone == 9);
        REQUIRE(state.channel.fm.rxTone == 3);
    }

    SECTION("A VFO booted in DMR comes back to FM without tones")
    {
        settings_t settings = default_settings;
        settings.dmr_colorCode = 15;
        settings.dmr_timeslot = 2;
        boot(settings);
        state.channel.mode = OPMODE_DMR;
        state.channel.dmr.rxColorCode = 15;
        state.channel.dmr.txColorCode = 15;
        state.channel.dmr.dmr_timeslot = 2;
        state.channel.dmr.contact_index = 0;
        ui_saveState();

        press(KEY_MONI | KEY_5);
        REQUIRE(state.channel.mode == OPMODE_M17);
        press(KEY_MONI | KEY_5);
        REQUIRE(state.channel.mode == OPMODE_FM);
        REQUIRE(state.channel.fm.rxToneEn == 0);
        REQUIRE(state.channel.fm.rxTone == 0);
        REQUIRE(state.channel.fm.txToneEn == 0);
        REQUIRE(state.channel.fm.txTone == 0);
    }
}

TEST_CASE("Settings seed a DMR VFO", "[ui][dmr]")
{
    settings_t settings = default_settings;
    settings.dmr_colorCode = 7;
    settings.dmr_timeslot = 2;
    boot(settings);

    press(KEY_MONI | KEY_5);
    REQUIRE(state.channel.mode == OPMODE_DMR);
    REQUIRE(state.channel.dmr.rxColorCode == 7);
    REQUIRE(state.channel.dmr.txColorCode == 7);
    REQUIRE(state.channel.dmr.dmr_timeslot == 2);
}

TEST_CASE("FRS mode and DMR are exclusive", "[ui][dmr][frs]")
{
    SECTION("The mode key is refused while FRS mode is on")
    {
        settings_t settings = default_settings;
        settings.frs_mode = 1;
        boot(settings);
        REQUIRE(state.ui_screen == MAIN_FRS);

        long long before = getTick();
        press(KEY_MONI | KEY_5);
        REQUIRE(state.channel.mode == OPMODE_FM);
        REQUIRE(frs_refuse_tick >= before);
    }

    SECTION("Enabling FRS mode parks the DMR VFO and restores it")
    {
        boot_dmr();
        press(KEY_ENTER);
        pressN(KEY_DOWN, M_SETTINGS);
        press(KEY_ENTER);
        pressN(KEY_DOWN, S_FRS);
        press(KEY_ENTER);
        REQUIRE(state.ui_screen == SETTINGS_FRS);
        press(KEY_ENTER);
        press(KEY_UP);
        REQUIRE(state.settings.frs_mode == 1);
        REQUIRE(state.channel.mode == OPMODE_FM);
        REQUIRE(state.vfo_channel.mode == OPMODE_DMR);

        press(KEY_DOWN);
        REQUIRE(state.settings.frs_mode == 0);
        REQUIRE(state.channel.mode == OPMODE_DMR);
        REQUIRE(state.channel.rx_frequency == VFO_FREQ);
        REQUIRE(state.channel.dmr.rxColorCode == 1);
    }
}

TEST_CASE("Settings > DMR edits the settings and the DMR VFO", "[ui][dmr]")
{
    boot_dmr();
    goto_dmr_settings();

    SECTION("DMR ID is typed in, zero means unset")
    {
        press(KEY_ENTER);
        typeNumber("2345678");
        press(KEY_ENTER);
        REQUIRE(state.settings.dmr_id == 2345678);

        /* ESC discards the entry */
        press(KEY_ENTER);
        typeNumber("42");
        press(KEY_ESC);
        REQUIRE(state.settings.dmr_id == 2345678);

        /* ENTER with nothing typed keeps the value */
        press(KEY_ENTER);
        press(KEY_ENTER);
        REQUIRE(state.settings.dmr_id == 2345678);

        /* The arrows delete the last digit */
        press(KEY_ENTER);
        typeNumber("123");
        press(KEY_UP);
        press(KEY_ENTER);
        REQUIRE(state.settings.dmr_id == 12);

        /* Zero is accepted and means "unset" */
        press(KEY_ENTER);
        typeNumber("0");
        press(KEY_ENTER);
        REQUIRE(state.settings.dmr_id == 0);
        REQUIRE(state.ui_screen == SETTINGS_DMR);
    }

    SECTION("The entry never exceeds the 24-bit ID range")
    {
        press(KEY_ENTER);
        typeNumber("16777216");
        press(KEY_ENTER);
        REQUIRE(state.settings.dmr_id == 1677721);

        press(KEY_ENTER);
        typeNumber("999999999");
        press(KEY_ENTER);
        REQUIRE(state.settings.dmr_id == 9999999);

        press(KEY_ENTER);
        typeNumber("16777215");
        press(KEY_ENTER);
        REQUIRE(state.settings.dmr_id == DMR_ID_MAX);
    }

    SECTION("Talkgroup is typed in")
    {
        press(KEY_DOWN);
        press(KEY_ENTER);
        typeNumber("31665");
        press(KEY_ENTER);
        REQUIRE(state.settings.dmr_talkgroup == 31665);
        REQUIRE(state.settings.dmr_id == 0);
    }

    SECTION("Call type cycles group, private and all")
    {
        pressN(KEY_DOWN, DMR_CALLTYPE);
        press(KEY_ENTER);
        press(KEY_UP);
        REQUIRE(state.settings.dmr_callType == PRIVATE);
        press(KEY_UP);
        REQUIRE(state.settings.dmr_callType == ALL);
        press(KEY_UP);
        REQUIRE(state.settings.dmr_callType == GROUP);
        press(KEY_DOWN);
        REQUIRE(state.settings.dmr_callType == ALL);
        press(KEY_ENTER);

        /* Left and right work without entering edit mode */
        press(KEY_RIGHT);
        REQUIRE(state.settings.dmr_callType == GROUP);
        press(KEY_LEFT);
        REQUIRE(state.settings.dmr_callType == ALL);
    }

    SECTION("Colour code wraps and follows into the VFO")
    {
        pressN(KEY_DOWN, DMR_COLORCODE);
        press(KEY_ENTER);
        press(KEY_UP);
        REQUIRE(state.settings.dmr_colorCode == 2);
        REQUIRE(state.channel.dmr.rxColorCode == 2);
        REQUIRE(state.channel.dmr.txColorCode == 2);
        pressN(KEY_DOWN, 3);
        REQUIRE(state.settings.dmr_colorCode == 15);
        REQUIRE(state.channel.dmr.rxColorCode == 15);
        press(KEY_UP);
        REQUIRE(state.settings.dmr_colorCode == 0);
        REQUIRE(state.channel.dmr.txColorCode == 0);
    }

    SECTION("Timeslot toggles and follows into the VFO")
    {
        pressN(KEY_DOWN, DMR_TIMESLOT);
        press(KEY_ENTER);
        press(KEY_UP);
        REQUIRE(state.settings.dmr_timeslot == 2);
        REQUIRE(state.channel.dmr.dmr_timeslot == 2);
        press(KEY_DOWN);
        REQUIRE(state.settings.dmr_timeslot == 1);
        REQUIRE(state.channel.dmr.dmr_timeslot == 1);
    }

    SECTION("Monitor cycles off, talkgroup and all")
    {
        pressN(KEY_DOWN, DMR_MONITOR);
        press(KEY_ENTER);
        press(KEY_UP);
        REQUIRE(state.settings.dmr_monitor == 1);
        press(KEY_UP);
        REQUIRE(state.settings.dmr_monitor == 2);
        press(KEY_UP);
        REQUIRE(state.settings.dmr_monitor == 0);
        press(KEY_DOWN);
        REQUIRE(state.settings.dmr_monitor == 2);
    }

    SECTION("Access toggles polite and impolite")
    {
        pressN(KEY_DOWN, DMR_ACCESS);
        press(KEY_ENTER);
        press(KEY_UP);
        REQUIRE(state.settings.dmr_polite == 0);
        press(KEY_DOWN);
        REQUIRE(state.settings.dmr_polite == 1);
    }

    SECTION("Hang time is clamped to 0-7 s")
    {
        pressN(KEY_DOWN, DMR_HANGTIME);
        press(KEY_ENTER);
        pressN(KEY_UP, 6);
        REQUIRE(state.settings.dmr_hangTime == 7);
        pressN(KEY_DOWN, 9);
        REQUIRE(state.settings.dmr_hangTime == 0);
        press(KEY_UP);
        REQUIRE(state.settings.dmr_hangTime == 1);
    }

    SECTION("Leaving the screen synchronises the RTX stage")
    {
        bool sync = press(KEY_ESC);
        REQUIRE(sync == true);
        REQUIRE(state.ui_screen == MENU_SETTINGS);
        pressN(KEY_ESC, 2);
        REQUIRE(state.ui_screen == MAIN_VFO);
        REQUIRE(state.channel.mode == OPMODE_DMR);
    }
}

TEST_CASE("Settings > DMR edits do not touch an FM VFO", "[ui][dmr]")
{
    boot();
    goto_dmr_settings();
    pressN(KEY_DOWN, DMR_COLORCODE);
    press(KEY_RIGHT);
    REQUIRE(state.settings.dmr_colorCode == 2);
    REQUIRE(state.channel.mode == OPMODE_FM);
    REQUIRE(state.channel.fm.txTone == 0);
    REQUIRE(state.channel.fm.rxTone == 0);
}

TEST_CASE("'#' on the main screen enters the talkgroup", "[ui][dmr]")
{
    boot_dmr();

    SECTION("Digits and ENTER set the talkgroup")
    {
        press(KEY_HASH);
        REQUIRE(state.tone_enabled == false);
        typeNumber("31665");
        bool sync = press(KEY_ENTER);
        REQUIRE(sync == true);
        REQUIRE(state.settings.dmr_talkgroup == 31665);
        REQUIRE(state.ui_screen == MAIN_VFO);
        REQUIRE(state.channel.rx_frequency == VFO_FREQ);
    }

    SECTION("ESC and # cancel the entry")
    {
        press(KEY_HASH);
        typeNumber("12");
        press(KEY_ESC);
        REQUIRE(state.settings.dmr_talkgroup == 9);

        press(KEY_HASH);
        typeNumber("12");
        press(KEY_HASH);
        REQUIRE(state.settings.dmr_talkgroup == 9);

        /* The entry is closed: digits open the frequency input again */
        press(KEY_1);
        REQUIRE(state.ui_screen == MAIN_VFO_INPUT);
    }

    SECTION("The arrows delete the last digit")
    {
        press(KEY_HASH);
        typeNumber("123");
        press(KEY_DOWN);
        press(KEY_ENTER);
        REQUIRE(state.settings.dmr_talkgroup == 12);
        REQUIRE(state.channel.rx_frequency == VFO_FREQ);
    }

    SECTION("ENTER with nothing typed keeps the talkgroup")
    {
        press(KEY_HASH);
        press(KEY_ENTER);
        REQUIRE(state.settings.dmr_talkgroup == 9);
        REQUIRE(state.ui_screen == MAIN_VFO);
    }
}

TEST_CASE("'*' toggles group and private call", "[ui][dmr]")
{
    boot_dmr();

    bool sync = press(KEY_STAR);
    REQUIRE(sync == true);
    REQUIRE(state.settings.dmr_callType == PRIVATE);
    press(KEY_STAR);
    REQUIRE(state.settings.dmr_callType == GROUP);

    /* A broadcast set in the menu goes back to a group call */
    state.settings.dmr_callType = ALL;
    press(KEY_STAR);
    REQUIRE(state.settings.dmr_callType == GROUP);
    REQUIRE(state.ui_screen == MAIN_VFO);
}

TEST_CASE("Macro menu keys 1-3 set colour code, timeslot and monitor",
          "[ui][dmr]")
{
    boot_dmr();

    bool sync = press(KEY_MONI | KEY_1);
    REQUIRE(sync == true);
    REQUIRE(state.channel.dmr.rxColorCode == 2);
    REQUIRE(state.channel.dmr.txColorCode == 2);
    REQUIRE(state.settings.dmr_colorCode == 2);
    pressN(KEY_MONI | KEY_1, 14);
    REQUIRE(state.channel.dmr.rxColorCode == 0);

    sync = press(KEY_MONI | KEY_2);
    REQUIRE(sync == true);
    REQUIRE(state.channel.dmr.dmr_timeslot == 2);
    REQUIRE(state.settings.dmr_timeslot == 2);
    press(KEY_MONI | KEY_2);
    REQUIRE(state.channel.dmr.dmr_timeslot == 1);

    sync = press(KEY_MONI | KEY_3);
    REQUIRE(sync == true);
    REQUIRE(state.settings.dmr_monitor == 1);
    press(KEY_MONI | KEY_3);
    REQUIRE(state.settings.dmr_monitor == 2);
    press(KEY_MONI | KEY_3);
    REQUIRE(state.settings.dmr_monitor == 0);

    /* The CTCSS settings of the FM VFO are untouched */
    REQUIRE(state.settings.dmr_colorCode == 0);
    press(KEY_MONI | KEY_5);
    press(KEY_MONI | KEY_5);
    REQUIRE(state.channel.mode == OPMODE_FM);
    press(KEY_MONI | KEY_3);
    REQUIRE(state.channel.fm.txTone == 1);
    REQUIRE(state.settings.dmr_monitor == 0);
}

TEST_CASE("The main screen renders in every DMR call state", "[ui][dmr]")
{
    boot_dmr();
    ui_state_t ui_state;
    memset(&ui_state, 0, sizeof(ui_state));

    /* Idle VFO screen, group call destination */
    ui_pushEvent(EVENT_STATUS, 0);
    bool sync = false;
    ui_updateFSM(&sync);
    ui_saveState();
    REQUIRE(ui_updateGUI() == true);

    /* Idle with a private call, a broadcast and the monitor on */
    state.settings.dmr_callType = PRIVATE;
    state.settings.dmr_talkgroup = DMR_ID_MAX;
    state.settings.dmr_monitor = 2;
    ui_saveState();
    rtxStatus_t status = rtx_getCurrentStatus();
    status.opMode = OPMODE_DMR;
    _ui_drawModeInfoDMR(&ui_state, &status);
    state.settings.dmr_callType = ALL;
    ui_saveState();
    _ui_drawModeInfoDMR(&ui_state, &status);

    /* Destination being typed */
    ui_state.edit_mode = true;
    ui_state.new_dmr_number = 12345678;
    ui_state.new_dmr_digits = 8;
    _ui_drawModeInfoDMR(&ui_state, &status);
    ui_state.edit_mode = false;

    /* Received group call and its hang time */
    status.dmr_lcOk = true;
    status.dmr_callState = DMR_CALL_RX;
    status.dmr_rxSrcId = 2345678;
    status.dmr_rxDstId = 31665;
    status.dmr_rxFlco = 0;
    status.dmr_rxColorCodeSeen = 1;
    status.dmr_rxTimeslot = 2;
    REQUIRE(_ui_dmrCallReceived(&status) == true);
    _ui_drawModeInfoDMR(&ui_state, &status);
    status.dmr_callState = DMR_CALL_RX_HANG;
    _ui_drawModeInfoDMR(&ui_state, &status);

    /* Received private call and broadcast */
    status.dmr_rxFlco = 3;
    _ui_drawModeInfoDMR(&ui_state, &status);
    status.dmr_rxFlco = 0;
    status.dmr_rxDstId = DMR_ID_MAX;
    _ui_drawModeInfoDMR(&ui_state, &status);

    /* A call without a valid LC is not shown as received */
    status.dmr_lcOk = false;
    REQUIRE(_ui_dmrCallReceived(&status) == false);

    /* Transmitting and waking up a repeater */
    status.dmr_callState = DMR_CALL_TX;
    status.dmr_callType = GROUP;
    status.dmr_dstId = 31665;
    REQUIRE(_ui_dmrCallReceived(&status) == false);
    _ui_drawModeInfoDMR(&ui_state, &status);
    status.dmr_callState = DMR_CALL_TX_WAKEUP;
    _ui_drawModeInfoDMR(&ui_state, &status);

    /* The MEM screen goes through the same code */
    state.ui_screen = MAIN_MEM;
    ui_saveState();
    ui_pushEvent(EVENT_STATUS, 0);
    ui_updateFSM(&sync);
    ui_saveState();
    REQUIRE(ui_updateGUI() == true);

    /* Settings > DMR screen, with and without a number being typed */
    state.ui_screen = MAIN_VFO;
    ui_saveState();
    goto_dmr_settings();
    REQUIRE(ui_updateGUI() == true);
    press(KEY_ENTER);
    typeNumber("123");
    REQUIRE(ui_updateGUI() == true);
    press(KEY_ESC);

    /* Macro menu overlay with the DMR rows */
    pressN(KEY_ESC, 3);
    REQUIRE(state.ui_screen == MAIN_VFO);
    kbd_msg_t msg;
    msg.value = 0;
    msg.keys = KEY_MONI;
    ui_pushEvent(EVENT_KBD, msg.value);
    ui_updateFSM(&sync);
    ui_saveState();
    REQUIRE(ui_updateGUI() == true);

    /* Key release closes the macro menu */
    ui_pushEvent(EVENT_KBD, 0);
    ui_updateFSM(&sync);
    ui_saveState();
    REQUIRE(ui_updateGUI() == true);
}

#if CONFIG_SCREEN_HEIGHT > 127
TEST_CASE("The idle DMR line fits the destination and the channel", "[ui][dmr]")
{
    /*
     * Mirrors the strings of _ui_drawModeInfoDMR(): destination on the left
     * of line 2 in layout.line2_font, channel on the right in
     * layout.message_font, inside the horizontal padding. The widest idle
     * destination is a private call to the largest ID, the widest channel
     * string is colour code 15, timeslot 2 and the monitor on. While
     * transmitting or typing the destination has the line for itself.
     */
    const char *idle[] = { "PC 16777215", "TG 16777215", "ALL" };
    const char *alone[] = { "TX -> PC 16777215", "TX -> TG 16777215",
                            "TX -> ALL", "PC 16777215_", "ID 16777215_" };
    const char *slot = "C15T2 M";
    const uint16_t usable = CONFIG_SCREEN_WIDTH - 2 * layout.horizontal_pad;
    const uint16_t gap = 4;

    uint16_t slotWidth = gfx_getTextWidth(layout.message_font, slot);
    for (const char *dst : idle) {
        uint16_t dstWidth = gfx_getTextWidth(layout.line2_font, dst);
        INFO(dst << " (" << dstWidth << " px) + " << slot << " (" << slotWidth
                 << " px) in " << usable << " px");
        REQUIRE(dstWidth + gap + slotWidth <= usable);
    }

    for (const char *dst : alone) {
        uint16_t dstWidth = gfx_getTextWidth(layout.line2_font, dst);
        INFO(dst << " (" << dstWidth << " px) in " << usable << " px");
        REQUIRE(dstWidth <= usable);
    }
}
#endif

TEST_CASE("The RTX stage keeps the DMR receive report across a configuration",
          "[ui][dmr][rtx]")
{
    boot_dmr();
    rtxStatus_t status = sync_rtx_stage();
    REQUIRE(status.opMode == OPMODE_DMR);
    REQUIRE(status.dmr_callState == DMR_CALL_IDLE);
    REQUIRE(status.dmr_lcOk == false);

    /* A configuration carrying report fields does not overwrite them */
    static rtxStatus_t cfg;
    cfg = status;
    cfg.dmr_lcOk = true;
    cfg.dmr_callState = DMR_CALL_RX;
    cfg.dmr_rxSrcId = 1234;
    cfg.dmr_srcId = 2345678;
    rtx_configure(&cfg);
    rtx_task();
    status = rtx_getCurrentStatus();
    REQUIRE(status.dmr_srcId == 2345678);
    REQUIRE(status.dmr_lcOk == false);
    REQUIRE(status.dmr_callState == DMR_CALL_IDLE);
    REQUIRE(status.dmr_rxSrcId == 0);
}

TEST_CASE("Reset to defaults restores the DMR settings", "[ui][dmr]")
{
    settings_t settings = default_settings;
    settings.dmr_id = 2345678;
    settings.dmr_talkgroup = 31665;
    settings.dmr_monitor = 2;
    boot(settings);

    press(KEY_ENTER);
    pressN(KEY_DOWN, M_SETTINGS);
    press(KEY_ENTER);
    pressN(KEY_DOWN, S_RESET2DEFAULTS);
    press(KEY_ENTER);
    press(KEY_ENTER);
    press(KEY_ENTER);
    REQUIRE(state.settings.dmr_id == 0);
    REQUIRE(state.settings.dmr_talkgroup == 9);
    REQUIRE(state.settings.dmr_monitor == 0);
}

TEST_CASE("The DMR top-bar marker follows the RTX marker flags", "[ui][dmr]")
{
    /* No flag, no marker */
    REQUIRE(_ui_dmrMarkerText(0) == NULL);

    /* One flag at a time */
    REQUIRE(strcmp(_ui_dmrMarkerText(DMR_MARK_NO_ID), currentLanguage->dmrNoId)
            == 0);
    REQUIRE(strcmp(_ui_dmrMarkerText(DMR_MARK_BUSY), currentLanguage->dmrBusy)
            == 0);
    REQUIRE(strcmp(_ui_dmrMarkerText(DMR_MARK_WAKEUP_FAILED),
                   currentLanguage->dmrWakeupFailed)
            == 0);
    REQUIRE(strcmp(_ui_dmrMarkerText(DMR_MARK_NOT_SUPPORTED),
                   currentLanguage->dmrNotSupported)
            == 0);

    /* Precedence: not supported, no ID, busy, wake-up failed */
    REQUIRE(strcmp(_ui_dmrMarkerText(DMR_MARK_NOT_SUPPORTED | DMR_MARK_NO_ID
                                     | DMR_MARK_BUSY),
                   currentLanguage->dmrNotSupported)
            == 0);
    REQUIRE(strcmp(_ui_dmrMarkerText(DMR_MARK_NO_ID | DMR_MARK_BUSY
                                     | DMR_MARK_WAKEUP_FAILED),
                   currentLanguage->dmrNoId)
            == 0);
    REQUIRE(strcmp(_ui_dmrMarkerText(DMR_MARK_BUSY | DMR_MARK_WAKEUP_FAILED),
                   currentLanguage->dmrBusy)
            == 0);

    /* The RTX driver hands the byte out only while in DMR */
    rtxStatus_t cur = rtx_getCurrentStatus();
    uint8_t expected = (cur.opMode == OPMODE_DMR) ? cur.dmr_markers : 0;
    REQUIRE(rtx_getDmrMarkers() == expected);

    /* Every language carries the four strings */
    for (const stringsTable_t *lang = languages;
         lang < languages + NUM_LANGUAGES; lang++) {
        REQUIRE(lang->dmrNoId != NULL);
        REQUIRE(lang->dmrBusy != NULL);
        REQUIRE(lang->dmrWakeupFailed != NULL);
        REQUIRE(lang->dmrNotSupported != NULL);
    }
}

/*
 * DMR monitor (bring-up) screen: Settings > DMR > DMR monitor.
 */

/* From a main screen: Menu > Settings > DMR > DMR monitor */
static void goto_dmr_monitor()
{
    goto_dmr_settings();
    pressN(KEY_DOWN, DMR_MONITOR_ENTRY);
    press(KEY_ENTER);
    REQUIRE(state.ui_screen == DMR_MONITOR_SCREEN);
}

/* Redraw the current screen, the way the UI thread does on a status event */
static void render()
{
    ui_pushEvent(EVENT_STATUS, 0);
    bool sync = false;
    ui_updateFSM(&sync);
    ui_saveState();
    REQUIRE(ui_updateGUI() == true);
}

/*
 * Fresh radio with the RTX stage switched to DMR on a reset fake modem: the
 * RTX stage keeps the DMR opMode handler enabled across the tests, so it is
 * first taken through FM, the modem record cleared, then brought back to DMR
 * (enable() clears the bring-up counters).
 */
static void fresh_dmr_rtx()
{
    boot();
    rtxStatus_t status = sync_rtx_stage();
    REQUIRE(status.opMode == OPMODE_FM);
    dmrEmu_reset();

    press(KEY_MONI | KEY_5);
    REQUIRE(state.channel.mode == OPMODE_DMR);
    status = sync_rtx_stage();
    REQUIRE(status.opMode == OPMODE_DMR);
    REQUIRE(status.dmr_mon.tsCount == 0);
    REQUIRE(status.dmr_mon.sysCount == 0);
}

/* The n-th most recent register write of the fake modem, 0 = last */
static const struct dmrEmuRegWrite *lastWrite(unsigned back = 0)
{
    const struct dmrEmuRecord *rec = dmrEmu_record();
    REQUIRE(rec->regWrites > back);
    return &rec->regLog[(rec->regWrites - 1 - back) % DMR_EMU_REG_LOG];
}

static void requireWrite(const struct dmrEmuRegWrite *w, uint8_t addr,
                         uint8_t value)
{
    INFO("write page " << (int)w->page << " addr 0x" << std::hex << (int)w->addr
                       << " = 0x" << (int)w->value);
    REQUIRE(w->page == DMRMON_PAGE_CONFIG);
    REQUIRE(w->addr == addr);
    REQUIRE(w->value == value);
}

static std::string monitorValue(uint8_t row)
{
    char buf[MAX_ENTRY_LEN];
    REQUIRE(_ui_getDMRMonitorValueName(buf, sizeof(buf), row) == 0);
    return std::string(buf);
}

static std::string monitorEntry(uint8_t row)
{
    char buf[MAX_ENTRY_LEN];
    REQUIRE(_ui_getDMRMonitorEntryName(buf, sizeof(buf), row) == 0);
    return std::string(buf);
}

TEST_CASE("Settings > DMR: test tone and DMR monitor items",
          "[ui][dmr][monitor]")
{
    boot_dmr();
    goto_dmr_settings();
    REQUIRE(settings_dmr_num == DMR_MONITOR_ENTRY + 1);

    SECTION("Test tone toggles settings.dmr_testTone")
    {
        pressN(KEY_DOWN, DMR_TESTTONE);
        press(KEY_ENTER);
        press(KEY_UP);
        REQUIRE(state.settings.dmr_testTone == 1);
        press(KEY_DOWN);
        REQUIRE(state.settings.dmr_testTone == 0);
        press(KEY_ENTER);

        /* Left and right work without entering edit mode */
        press(KEY_RIGHT);
        REQUIRE(state.settings.dmr_testTone == 1);
        REQUIRE(ui_updateGUI() == true);
        press(KEY_LEFT);
        REQUIRE(state.settings.dmr_testTone == 0);
        REQUIRE(state.ui_screen == SETTINGS_DMR);
    }

    SECTION("DMR monitor opens with ENTER and ESC comes back to its item")
    {
        pressN(KEY_DOWN, DMR_MONITOR_ENTRY);
        REQUIRE(ui_updateGUI() == true);
        press(KEY_ENTER);
        REQUIRE(state.ui_screen == DMR_MONITOR_SCREEN);
        REQUIRE(state.txDisable == true);
        render();

        press(KEY_ESC);
        REQUIRE(state.ui_screen == SETTINGS_DMR);
        press(KEY_ENTER);
        REQUIRE(state.ui_screen == DMR_MONITOR_SCREEN);
        press(KEY_ESC);
        REQUIRE(state.ui_screen == SETTINGS_DMR);
        pressN(KEY_ESC, 3);
        REQUIRE(state.ui_screen == MAIN_VFO);
        REQUIRE(state.channel.mode == OPMODE_DMR);
    }

    SECTION("Every language carries the monitor strings")
    {
        for (const stringsTable_t *lang = languages;
             lang < languages + NUM_LANGUAGES; lang++) {
            REQUIRE(lang->dmrMonitor != NULL);
            REQUIRE(lang->testTone != NULL);
            REQUIRE(lang->resync != NULL);
            REQUIRE(lang->berTest != NULL);
            REQUIRE(lang->twoPointTest != NULL);
        }
    }
}

TEST_CASE("DMR monitor: the pages cover every row, section by section",
          "[ui][dmr][monitor]")
{
    boot_dmr();
    dmrEmu_reset();
    goto_dmr_monitor();

    uint8_t pages = _ui_dmrMonitorPages();
    REQUIRE(pages >= DMRMON_SECTION_NUM);
    printf("DMR monitor: %u pages on %ux%u\n", pages, CONFIG_SCREEN_WIDTH,
           CONFIG_SCREEN_HEIGHT);

    /* Consecutive pages walk the rows in order, the sections start a page */
    uint8_t expectFirst = 0;
    bool sectionStart[DMRMON_ROWS_NUM] = { false };
    uint8_t actionPages = 0;
    for (uint8_t p = 0; p < pages; p++) {
        uint8_t first = 0xFF;
        uint8_t count = 0;
        _ui_dmrMonitorPageRows(p, &first, &count);
        INFO("page " << (int)p << " first " << (int)first << " count "
                     << (int)count);
        REQUIRE(count > 0);
        REQUIRE(first == expectFirst);
        expectFirst = first + count;
        sectionStart[first] = true;
        if (first >= DMRMON_ACTION_FIRST)
            actionPages++;
    }
    REQUIRE(expectFirst == DMRMON_ROWS_NUM);
    REQUIRE(sectionStart[DMRMON_TS]);
    REQUIRE(sectionStart[DMRMON_R51]);
    REQUIRE(sectionStart[DMRMON_LC_LO]);
    REQUIRE(sectionStart[DMRMON_ACT_RESYNC]);
    REQUIRE(actionPages == 1);

    /* Wrapped page numbers */
    uint8_t first = 0;
    uint8_t count = 0;
    _ui_dmrMonitorPageRows(pages, &first, &count);
    REQUIRE(first == DMRMON_TS);
    _ui_dmrMonitorPageRows(pages - 1, &first, &count);
    REQUIRE(first == DMRMON_ACT_RESYNC);
    REQUIRE(count == DMRMON_ACTION_NUM);

    /* RIGHT walks the pages and wraps, LEFT walks back; each page renders */
    render();
    REQUIRE(_ui_dmrMonitorCurrentPage() == 0);
    for (uint8_t p = 1; p <= pages; p++) {
        press(KEY_RIGHT);
        render();
        REQUIRE(_ui_dmrMonitorCurrentPage() == (p % pages));
    }
    press(KEY_LEFT);
    render();
    REQUIRE(_ui_dmrMonitorCurrentPage() == pages - 1);
    pressN(KEY_LEFT, pages - 1);
    render();
    REQUIRE(_ui_dmrMonitorCurrentPage() == 0);

    /* UP, DOWN and ENTER flip the pages too while not on the actions */
    press(KEY_DOWN);
    render();
    REQUIRE(_ui_dmrMonitorCurrentPage() == 1);
    press(KEY_UP);
    render();
    REQUIRE(_ui_dmrMonitorCurrentPage() == 0);
    press(KEY_ENTER);
    render();
    REQUIRE(_ui_dmrMonitorCurrentPage() == 1);
    REQUIRE(dmrEmu_record()->regWrites == 0);

    /* The row labels: registers by number, actions by their strings */
    REQUIRE(monitorEntry(DMRMON_TS) == "TS");
    REQUIRE(monitorEntry(DMRMON_PC3) == "PC3");
    REQUIRE(monitorEntry(DMRMON_R82) == "0x82");
    REQUIRE(monitorEntry(DMRMON_R5F) == "0x5F");
    REQUIRE(monitorEntry(DMRMON_LC_HI) == "LC 6-11");
    REQUIRE(monitorEntry(DMRMON_ACT_RESYNC) == currentLanguage->resync);
    REQUIRE(monitorEntry(DMRMON_ACT_BER) == currentLanguage->berTest);
    REQUIRE(monitorEntry(DMRMON_ACT_TWOPOINT) == currentLanguage->twoPointTest);
    char buf[MAX_ENTRY_LEN];
    REQUIRE(_ui_getDMRMonitorEntryName(buf, sizeof(buf), DMRMON_ROWS_NUM)
            == -1);
    REQUIRE(_ui_getDMRMonitorValueName(buf, sizeof(buf), DMRMON_ROWS_NUM)
            == -1);

    press(KEY_ESC);
    REQUIRE(state.ui_screen == SETTINGS_DMR);
}

TEST_CASE("DMR monitor: the counters of the RTX status are rendered",
          "[ui][dmr][monitor]")
{
    fresh_dmr_rtx();

    /* Six idle timeslots, then a header burst with recognisable registers */
    for (unsigned i = 0; i < 6; i++) {
        dmrEmu_pushTs();
        rtx_task();
    }

    static const uint8_t lc[12] = { 0x00, 0x00, 0x00, 0x00, 0x7B, 0xB1,
                                    0x23, 0xCA, 0xCE, 0x00, 0x00, 0x00 };
    struct dmrbbSnapshot s;
    memset(&s, 0x00, sizeof(s));
    s.irq82 = 0x08;
    s.sub84 = 0x80;
    s.sub86 = 0x40;
    s.r51 = 0x10;
    s.r52 = 0x14;
    s.r5f = 0x01;
    s.rssi = 1234;
    s.fskErr = 7;
    memcpy(s.lc, lc, sizeof(s.lc));
    for (unsigned i = 0; i < 6; i++)
        dmrEmu_pulseLine(0);
    dmrEmu_pulseLine(3);
    dmrEmu_pushSys(&s);
    rtx_task();

    rtxStatus_t status = rtx_getCurrentStatus();
    REQUIRE(status.dmr_mon.tsCount == 6);
    REQUIRE(status.dmr_mon.sysCount == 1);
    REQUIRE(status.dmr_mon.lineCount[0] == 6);
    REQUIRE(status.dmr_mon.lineCount[3] == 1);
    REQUIRE(status.dmr_mon.irq82 == 0x08);
    REQUIRE(status.dmr_mon.lcSrc == 2345678);
    REQUIRE(status.dmr_mon.lcDst == 31665);
    REQUIRE(status.dmr_mon.lcValid == 0x03);
    REQUIRE(status.dmr_slotLock == 2);

    /* A configuration pushed by the UI keeps the block */
    status = sync_rtx_stage();
    REQUIRE(status.dmr_mon.tsCount == 6);
    REQUIRE(status.dmr_mon.sysCount == 1);

    goto_dmr_monitor();
    render();
    REQUIRE(monitorValue(DMRMON_TS) == "6");
    REQUIRE(monitorValue(DMRMON_SYS) == "1");
    REQUIRE(monitorValue(DMRMON_RFTX) == "0");
    REQUIRE(monitorValue(DMRMON_PC0) == "6");
    REQUIRE(monitorValue(DMRMON_PC1) == "0");
    REQUIRE(monitorValue(DMRMON_PC2) == "0");
    REQUIRE(monitorValue(DMRMON_PC3) == "1");
    REQUIRE(monitorValue(DMRMON_LOCK) == "2");
    REQUIRE(monitorValue(DMRMON_R82) == "08");
    REQUIRE(monitorValue(DMRMON_R84) == "80");
    REQUIRE(monitorValue(DMRMON_R86) == "40");
    REQUIRE(monitorValue(DMRMON_R51) == "10");
    REQUIRE(monitorValue(DMRMON_R52) == "14");
    REQUIRE(monitorValue(DMRMON_R5F) == "01");
    REQUIRE(monitorValue(DMRMON_CC) == "1");
    REQUIRE(monitorValue(DMRMON_TC) == "1");
    REQUIRE(monitorValue(DMRMON_SYNC) == "BS");
    REQUIRE(monitorValue(DMRMON_RSSI) == "1234");
    REQUIRE(monitorValue(DMRMON_FSKERR) == "7");
    REQUIRE(monitorValue(DMRMON_LC_LO) == "000000007BB1");
    REQUIRE(monitorValue(DMRMON_LC_HI) == "23CACE000000");
    REQUIRE(monitorValue(DMRMON_LC_SRC) == "2345678");
    REQUIRE(monitorValue(DMRMON_LC_DST) == "31665");
    REQUIRE(monitorValue(DMRMON_LC_FLCO) == "0");
    REQUIRE(monitorValue(DMRMON_LC_FID) == "00");
    REQUIRE(monitorValue(DMRMON_LC_CRC) == "ok");
    REQUIRE(monitorValue(DMRMON_ACT_RESYNC) == "41<-20");
    REQUIRE(monitorValue(DMRMON_ACT_MODE) == "10<-6A");
    REQUIRE(monitorValue(DMRMON_ACT_R06) == "06<-21");
    REQUIRE(monitorValue(DMRMON_ACT_BER) == "10 40 41");
    REQUIRE(monitorValue(DMRMON_ACT_TWOPOINT) == "12^40");

    /* More slots: the screen follows on the next draw */
    for (unsigned i = 0; i < 4; i++) {
        dmrEmu_pushTs();
        rtx_task();
    }
    render();
    REQUIRE(monitorValue(DMRMON_TS) == "10");

    /* A bad CRC and a manufacturer LC are flagged */
    s.r51 = 0x24;
    s.lc[1] = 0x10;
    dmrEmu_pushSys(&s);
    rtx_task();
    render();
    REQUIRE(monitorValue(DMRMON_SYS) == "2");
    REQUIRE(monitorValue(DMRMON_R51) == "24");
    REQUIRE(monitorValue(DMRMON_LC_FID) == "10");
    REQUIRE(monitorValue(DMRMON_LC_CRC) == "bad");
    s.lc[0] = 0x80;
    dmrEmu_pushSys(&s);
    rtx_task();
    render();
    REQUIRE(monitorValue(DMRMON_LC_CRC) == "bad*");

    press(KEY_ESC);
    REQUIRE(state.ui_screen == SETTINGS_DMR);
}

TEST_CASE("DMR monitor: the actions write the documented registers",
          "[ui][dmr][monitor]")
{
    fresh_dmr_rtx();
    goto_dmr_monitor();
    const struct dmrEmuRecord *rec = dmrEmu_record();
    REQUIRE(rec->regWrites == 0);

    /* The action page is the last one: LEFT wraps to it */
    uint8_t pages = _ui_dmrMonitorPages();
    press(KEY_LEFT);
    render();
    REQUIRE(_ui_dmrMonitorCurrentPage() == pages - 1);
    REQUIRE(_ui_dmrMonitorSelectedAction() == 0);

    /* Resync: 0x41 = 0x20 */
    press(KEY_ENTER);
    REQUIRE(rec->regWrites == 1);
    requireWrite(lastWrite(), 0x41, 0x20);
    REQUIRE(dmrbb_readRegister(DMRMON_PAGE_CONFIG, 0x41) == 0x20);

    /* Mode: the driver default is 0x6E, the first press tries 0x6A */
    press(KEY_DOWN);
    render();
    REQUIRE(_ui_dmrMonitorSelectedAction() == 1);
    REQUIRE(monitorValue(DMRMON_RB10) == "00");
    bool sync = press(KEY_ENTER);
    REQUIRE(rec->regWrites == 2);
    requireWrite(lastWrite(), 0x10, 0x6A);
    render();
    REQUIRE(monitorValue(DMRMON_ACT_MODE) == "10<-6E");
    REQUIRE(monitorValue(DMRMON_RB10) == "6A");

    /* The word is pushed to the RTX stage: the modem is reprogrammed with
     * it, so that the watchdog reconfiguration keeps it */
    REQUIRE(sync == true);
    REQUIRE(state.dmr_modeReg == 0x6A);
    uint32_t configured = rec->configureCount;
    rtxStatus_t status = sync_rtx_stage();
    REQUIRE(status.dmr_modeReg == 0x6A);
    REQUIRE(rec->configureCount == configured + 1);
    REQUIRE(rec->config.modeReg == 0x6A);
    sync_rtx_stage();
    REQUIRE(rec->configureCount == configured + 1);

    sync = press(KEY_ENTER);
    requireWrite(lastWrite(), 0x10, 0x6E);
    REQUIRE(rec->regWrites == 3);
    REQUIRE(sync == true);
    REQUIRE(state.dmr_modeReg == 0x6E);
    sync_rtx_stage();
    REQUIRE(rec->configureCount == configured + 2);
    REQUIRE(rec->config.modeReg == 0x6E);

    /* 0x06 variant: the five values in turn, then round again */
    static const uint8_t variants[DMRMON_R06_VARIANTS] = { 0x21, 0x01, 0x24,
                                                           0x20, 0x00 };
    press(KEY_DOWN);
    for (unsigned i = 0; i < DMRMON_R06_VARIANTS; i++) {
        press(KEY_ENTER);
        requireWrite(lastWrite(), 0x06, variants[i]);
    }
    press(KEY_ENTER);
    requireWrite(lastWrite(), 0x06, 0x21);
    REQUIRE(rec->regWrites == 9);
    render();
    REQUIRE(monitorValue(DMRMON_RB06) == "21");

    /* BER test: 0x10 = 0x02, 0x40 = 0x40, 0x41 = 0x41 in this order */
    press(KEY_DOWN);
    press(KEY_ENTER);
    REQUIRE(rec->regWrites == 12);
    requireWrite(lastWrite(2), 0x10, 0x02);
    requireWrite(lastWrite(1), 0x40, 0x40);
    requireWrite(lastWrite(0), 0x41, 0x41);

    /* Two-point test: bit6 of 0x12 toggled from its current value */
    dmrbb_writeRegister(DMRMON_PAGE_CONFIG, 0x12, 0x98);
    press(KEY_DOWN);
    render();
    REQUIRE(_ui_dmrMonitorSelectedAction() == DMRMON_ACTION_NUM - 1);
    press(KEY_ENTER);
    requireWrite(lastWrite(), 0x12, 0xD8);
    REQUIRE(dmrbb_readRegister(DMRMON_PAGE_CONFIG, 0x12) == 0xD8);
    press(KEY_ENTER);
    requireWrite(lastWrite(), 0x12, 0x98);
    REQUIRE(rec->regWrites == 15);
    render();
    REQUIRE(monitorValue(DMRMON_RB12) == "98");

    /* Own ID registers are read back in address order */
    dmrbb_writeRegister(DMRMON_PAGE_CONFIG, 0x14, 0xCE);
    dmrbb_writeRegister(DMRMON_PAGE_CONFIG, 0x15, 0xCA);
    dmrbb_writeRegister(DMRMON_PAGE_CONFIG, 0x16, 0x23);
    render();
    REQUIRE(monitorValue(DMRMON_RB14) == "CE CA 23");
    REQUIRE(monitorEntry(DMRMON_RB14) == "rd 0x14-16");
    REQUIRE(rec->regWrites == 18);

    /* UP and DOWN wrap over the actions, the knob works as well */
    press(KEY_DOWN);
    render();
    REQUIRE(_ui_dmrMonitorSelectedAction() == 0);
    press(KEY_UP);
    render();
    REQUIRE(_ui_dmrMonitorSelectedAction() == DMRMON_ACTION_NUM - 1);
    press(KNOB_RIGHT);
    render();
    REQUIRE(_ui_dmrMonitorSelectedAction() == 0);
    press(KNOB_LEFT);
    render();
    REQUIRE(_ui_dmrMonitorSelectedAction() == DMRMON_ACTION_NUM - 1);

    /* Leaving the page resets the selection; nothing else was written */
    press(KEY_RIGHT);
    render();
    REQUIRE(_ui_dmrMonitorCurrentPage() == 0);
    press(KEY_LEFT);
    render();
    REQUIRE(_ui_dmrMonitorSelectedAction() == 0);
    REQUIRE(rec->regWrites == 18);

    /* Reopening the screen starts from the mode word in force */
    press(KEY_ESC);
    press(KEY_ENTER);
    press(KEY_LEFT);
    render();
    REQUIRE(monitorValue(DMRMON_ACT_MODE) == "10<-6A");

    /* The settings were not touched by the register writes */
    REQUIRE(state.settings.dmr_colorCode == 1);
    REQUIRE(state.settings.dmr_testTone == 0);

    press(KEY_ESC);
    REQUIRE(state.ui_screen == SETTINGS_DMR);
    press(KEY_ESC);
    REQUIRE(state.ui_screen == MENU_SETTINGS);
}
