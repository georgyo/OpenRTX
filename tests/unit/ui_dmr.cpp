/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include <pthread.h>

extern "C" {
#include "core/event.h"
#include "core/input.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/ui.h"
#include "interfaces/delays.h"
#include "interfaces/keyboard.h"
#include "rtx/rtx.h"
#include "ui/ui_default.h"
}

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
