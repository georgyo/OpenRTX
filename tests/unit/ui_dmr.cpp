/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <pthread.h>
#include <unistd.h>
#include <string>

extern "C" {
#include "core/dmr_lastheard.h"
#include "core/event.h"
#include "core/graphics.h"
#include "core/input.h"
#include "core/settings.h"
#include "core/state.h"
#include "core/ui.h"
#include "interfaces/cps_io.h"
#include "interfaces/delays.h"
#include "interfaces/dmr_baseband.h"
#include "interfaces/keyboard.h"
#include "interfaces/nvmem.h"
#include "core/voicePrompts.h"
#include "core/voicePromptUtils.h"
#include "rtx/rtx.h"
#include "ui/ui_default.h"
}
#include "emulator.h"
#include "protocols/DMR/FEC/ShortLc.hpp"
#include "protocols/DMR/LinkControl.hpp"
#include "protocols/DMR/ModemPort.hpp"

/*
 * Drives the default UI state machine of the Linux build (160x128, GPS, RTC,
 * M17 and DMR enabled) through the DMR mode selection, the main screen keys
 * and Settings > DMR with synthetic key events, in the style of ui_frs.cpp.
 * The main screen is also rendered into the framebuffer, without a display,
 * to make sure the DMR lines do not crash in any call state.
 */

static const freq_t VFO_FREQ = 430000000;

/* The fourth contact of the bench codeplug: 15 characters, private */
#define WIDE_CONTACT_NAME "ABCDEFGHIJKLMNO"
#define WIDE_CONTACT_ID 7654321

/*
 * Scratch directory of the bench: the codeplug and, through XDG_STATE_HOME,
 * the emulator's NVM file, so that the test never touches $HOME (unwritable
 * in a build sandbox) and two runs never share a file.
 */
static char scratchDir[] = "/tmp/ui_dmr_test.XXXXXX";
static char cpsPath[64];

static void bench_scratch()
{
    static bool ready = false;
    if (ready)
        return;

    REQUIRE(mkdtemp(scratchDir) != NULL);
    snprintf(cpsPath, sizeof(cpsPath), "%s/codeplug.rtxc", scratchDir);
    setenv("XDG_STATE_HOME", scratchDir, 1);
    ready = true;
}

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
        bench_scratch();
        rtx_init(&rtxMutex);
        rtxReady = true;

        /*
         * The firmware opens (or creates) a codeplug at boot (openrtx.c):
         * the contacts lookups of the DMR screens need one here too. Two
         * DMR contacts and an M17 one, see the contacts tests, then a
         * private contact with the widest name a DMR destination can carry
         * (DMR_DEST_NAME_LEN - 1 characters) for the last heard list.
         */
        cps_create(cpsPath);
        REQUIRE(cps_open(cpsPath) == 0);

        contact_t tg, pc, m17, wide;
        memset(&tg, 0, sizeof(tg));
        memset(&pc, 0, sizeof(pc));
        memset(&m17, 0, sizeof(m17));
        memset(&wide, 0, sizeof(wide));
        strcpy(tg.name, "BM Italy");
        tg.mode = OPMODE_DMR;
        tg.info.dmr.id = 2222;
        tg.info.dmr.contactType = GROUP;
        strcpy(pc.name, "IZ1ABC Mario");
        pc.mode = OPMODE_DMR;
        pc.info.dmr.id = 2345678;
        pc.info.dmr.contactType = PRIVATE;
        strcpy(m17.name, "M17 contact");
        m17.mode = OPMODE_M17;
        strcpy(wide.name, WIDE_CONTACT_NAME);
        wide.mode = OPMODE_DMR;
        wide.info.dmr.id = WIDE_CONTACT_ID;
        wide.info.dmr.contactType = PRIVATE;
        REQUIRE(cps_insertContact(tg, 0) == 0);
        REQUIRE(cps_insertContact(pc, 1) == 0);
        REQUIRE(cps_insertContact(m17, 2) == 0);
        REQUIRE(cps_insertContact(wide, 3) == 0);
    }
    _ui_dmrContactNameCacheClear();
    dmrLastHeard_clear();

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
    cfg.dmr_lcssMode = state.dmr_lcssMode;
    cfg.dmr_rfAdvance = state.dmr_rfAdvance;

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

    /* Six favourite slots of four bytes, empty by default */
    REQUIRE(DMR_FAV_NUM == 6);
    REQUIRE(sizeof(default_settings.dmr_favourites) == 24);
    for (unsigned i = 0; i < DMR_FAV_NUM; i++)
        REQUIRE(default_settings.dmr_favourites[i] == 0);
    REQUIRE(DMR_FAV_PACK(31665, GROUP) == 31665u);
    REQUIRE(DMR_FAV_PACK(2345678, PRIVATE) == (2345678u | (1u << 24)));
    REQUIRE(DMR_FAV_ID(DMR_FAV_PACK(DMR_ID_MAX, ALL)) == DMR_ID_MAX);
    REQUIRE(DMR_FAV_TYPE(DMR_FAV_PACK(DMR_ID_MAX, ALL)) == ALL);
    REQUIRE(DMR_FAV_TYPE(DMR_FAV_PACK(9, 7)) == 3);
}

TEST_CASE("Boot validation empties malformed favourite slots", "[ui][dmr]")
{
    /*
     * state_init() reads the settings back from the emulator's NVM file:
     * write a record with two good slots and four malformed ones first.
     */
    settings_t s = default_settings;
    s.dmr_favourites[0] = DMR_FAV_PACK(31665, GROUP);
    s.dmr_favourites[1] = DMR_FAV_PACK(2345678, PRIVATE);
    s.dmr_favourites[2] = DMR_FAV_PACK(9, 3);       /* unknown type      */
    s.dmr_favourites[3] = (1u << 26) | 9;           /* bits above type   */
    s.dmr_favourites[4] = DMR_FAV_PACK(0, PRIVATE); /* ID zero with type */
    s.dmr_favourites[5] = 0xFFFFFFFF;
    channel_t vfo = cps_getDefaultChannel();
    bench_scratch();
    nvm_init();
    REQUIRE(nvm_writeSettingsAndVfo(&s, &vfo) == 0);

    state_init();
    REQUIRE(state.settings.dmr_favourites[0] == DMR_FAV_PACK(31665, GROUP));
    REQUIRE(state.settings.dmr_favourites[1] == DMR_FAV_PACK(2345678, PRIVATE));
    for (unsigned i = 2; i < DMR_FAV_NUM; i++)
        REQUIRE(state.settings.dmr_favourites[i] == 0);
    REQUIRE(state.dmr_destName[0] == '\0');

    /* Leave a clean record behind for the other tests */
    s = default_settings;
    REQUIRE(nvm_writeSettingsAndVfo(&s, &vfo) == 0);
    pthread_mutex_destroy(&state_mutex);
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
    /*
     * The RTX stage is shared by every test of this process: a call
     * injected by another test leaves its report in place until the DMR
     * handler is enabled again, so go through FM first.
     */
    boot();
    sync_rtx_stage();
    dmrEmu_reset();
    press(KEY_MONI | KEY_5);
    REQUIRE(state.channel.mode == OPMODE_DMR);
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
    REQUIRE(strcmp(_ui_dmrMarkerText(DMR_MARK_RF_FAILED),
                   currentLanguage->dmrRfFailed)
            == 0);
    REQUIRE(strcmp(_ui_dmrMarkerText(DMR_MARK_RF_FAILED), "RF failed") == 0);
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

/* Same for a write to the driver's pseudo page */
static void requireDriverWrite(const struct dmrEmuRegWrite *w, uint8_t addr,
                               uint8_t value)
{
    INFO("write page " << (int)w->page << " addr 0x" << std::hex << (int)w->addr
                       << " = 0x" << (int)w->value);
    REQUIRE(w->page == DMRBB_PAGE_DRIVER);
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
    /* 12, 13, 7, 7 and 8 rows in six per page: 2 + 3 + 2 + 2, plus the
     * single action page (8 actions scrolling behind the selection), as
     * documented */
    if ((CONFIG_SCREEN_WIDTH == 160) && (CONFIG_SCREEN_HEIGHT == 128))
        REQUIRE(pages == 10);
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
    REQUIRE(sectionStart[DMRMON_AUD_VOCODER]);
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
    REQUIRE(monitorEntry(DMRMON_AUD_VOCODER) == "Vocoder");
    REQUIRE(monitorEntry(DMRMON_AUD_OVER) == "Overrun");
    REQUIRE(monitorEntry(DMRMON_AUD_TXBURSTS) == "TX bursts");
    REQUIRE(monitorEntry(DMRMON_AUD_TXSILENT) == "TX silent");
    REQUIRE(monitorEntry(DMRMON_AUD_TXUNDER) == "TX under");
    REQUIRE(monitorEntry(DMRMON_RFKEYED) == "RF keyed");
    REQUIRE(monitorEntry(DMRMON_ACT_RESYNC) == currentLanguage->resync);
    REQUIRE(monitorEntry(DMRMON_ACT_BER) == currentLanguage->berTest);
    REQUIRE(monitorEntry(DMRMON_ACT_TWOPOINT) == currentLanguage->twoPointTest);
    REQUIRE(monitorEntry(DMRMON_ACT_LCSS) == currentLanguage->lcssMode);
    REQUIRE(monitorEntry(DMRMON_ACT_RFADV) == currentLanguage->rfAdvance);
    REQUIRE(monitorEntry(DMRMON_ACT_SYSLINE) == currentLanguage->sysLine);
    for (const stringsTable_t *lang = languages;
         lang < languages + NUM_LANGUAGES; lang++) {
        REQUIRE(lang->lcssMode != NULL);
        REQUIRE(lang->rfAdvance != NULL);
        REQUIRE(lang->sysLine != NULL);
    }
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
    /* The backend this build links, initialised by the RTX stage: "none"
     * would mean OpMode_DMR::enable() never reached dmrAudio_init() */
#ifdef CONFIG_DMR_VOCODER_LOOPBACK
    REQUIRE(monitorValue(DMRMON_AUD_VOCODER) == "loopback");
#else
    REQUIRE(monitorValue(DMRMON_AUD_VOCODER) == "null");
#endif
    REQUIRE(monitorValue(DMRMON_AUD_RX) == "0");
    REQUIRE(monitorValue(DMRMON_AUD_VARY) == "0");
    REQUIRE(monitorValue(DMRMON_AUD_OUT) == "0");
    REQUIRE(monitorValue(DMRMON_AUD_TX) == "0");
    REQUIRE(monitorValue(DMRMON_AUD_UNDER) == "0");
    REQUIRE(monitorValue(DMRMON_AUD_OVER) == "0");
    REQUIRE(monitorValue(DMRMON_AUD_TXBURSTS) == "0");
    REQUIRE(monitorValue(DMRMON_AUD_TXSILENT) == "0");
    REQUIRE(monitorValue(DMRMON_AUD_TXUNDER) == "0");
    REQUIRE(monitorValue(DMRMON_RFKEYED) == "0");
    REQUIRE(monitorValue(DMRMON_ACT_RESYNC) == "41<-20");
    REQUIRE(monitorValue(DMRMON_ACT_MODE) == "10<-6A");
    REQUIRE(monitorValue(DMRMON_ACT_R06) == "06<-21");
    REQUIRE(monitorValue(DMRMON_ACT_BER) == "81 10 40 41");
    REQUIRE(monitorValue(DMRMON_ACT_TWOPOINT) == "12^40");
    REQUIRE(monitorValue(DMRMON_ACT_LCSS) == "50 manual");
    REQUIRE(monitorValue(DMRMON_ACT_RFADV) == "12<-98");

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

    /* Resync: the Layer-2 mask of dmrMode() back, then 0x41 = 0x20 */
    press(KEY_ENTER);
    REQUIRE(rec->regWrites == 2);
    requireWrite(lastWrite(1), 0x81, 0xFE);
    requireWrite(lastWrite(), 0x41, 0x20);
    REQUIRE(dmrbb_readRegister(DMRMON_PAGE_CONFIG, 0x41) == 0x20);

    /* Mode: the driver default is 0x6E, the first press tries 0x6A */
    press(KEY_DOWN);
    render();
    REQUIRE(_ui_dmrMonitorSelectedAction() == 1);
    REQUIRE(monitorValue(DMRMON_RB10) == "00");
    bool sync = press(KEY_ENTER);
    REQUIRE(rec->regWrites == 3);
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
    REQUIRE(rec->regWrites == 4);
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
    REQUIRE(rec->regWrites == 10);
    render();
    REQUIRE(monitorValue(DMRMON_RB06) == "21");

    /*
     * BER test: the stale flags cleared and the PHY-only interrupt
     * unmasked (0x83 = 0xFF, 0x81 = 0xFF), then 0x10 = 0x02, 0x40 = 0x40,
     * 0x41 = 0x41 in this order
     */
    press(KEY_DOWN);
    press(KEY_ENTER);
    REQUIRE(rec->regWrites == 15);
    requireWrite(lastWrite(4), 0x83, 0xFF);
    requireWrite(lastWrite(3), 0x81, 0xFF);
    requireWrite(lastWrite(2), 0x10, 0x02);
    requireWrite(lastWrite(1), 0x40, 0x40);
    requireWrite(lastWrite(0), 0x41, 0x41);
    REQUIRE(dmrbb_readRegister(DMRMON_PAGE_CONFIG, 0x81) == 0xFF);

    /* Resync puts the Layer-2 mask back */
    pressN(KEY_DOWN, DMRMON_ACTION_NUM - 3);
    render();
    REQUIRE(_ui_dmrMonitorSelectedAction() == 0);
    press(KEY_ENTER);
    REQUIRE(rec->regWrites == 17);
    requireWrite(lastWrite(1), 0x81, 0xFE);
    REQUIRE(dmrbb_readRegister(DMRMON_PAGE_CONFIG, 0x81) == 0xFE);
    pressN(KEY_DOWN, 3);

    /* Two-point test: bit6 of 0x12 toggled from its current value */
    dmrbb_writeRegister(DMRMON_PAGE_CONFIG, 0x12, 0x98);
    press(KEY_DOWN);
    render();
    REQUIRE(_ui_dmrMonitorSelectedAction()
            == DMRMON_ACT_TWOPOINT - DMRMON_ACTION_FIRST);
    press(KEY_ENTER);
    requireWrite(lastWrite(), 0x12, 0xD8);
    REQUIRE(dmrbb_readRegister(DMRMON_PAGE_CONFIG, 0x12) == 0xD8);
    press(KEY_ENTER);
    requireWrite(lastWrite(), 0x12, 0x98);
    REQUIRE(rec->regWrites == 20);
    render();
    REQUIRE(monitorValue(DMRMON_RB12) == "98");

    /* Own ID registers are read back in address order */
    dmrbb_writeRegister(DMRMON_PAGE_CONFIG, 0x14, 0xCE);
    dmrbb_writeRegister(DMRMON_PAGE_CONFIG, 0x15, 0xCA);
    dmrbb_writeRegister(DMRMON_PAGE_CONFIG, 0x16, 0x23);
    render();
    REQUIRE(monitorValue(DMRMON_RB14) == "CE CA 23");
    REQUIRE(monitorEntry(DMRMON_RB14) == "rd 0x14-16");
    REQUIRE(rec->regWrites == 23);

    /*
     * LCSS mode: no register write, the convention goes to the RTX stage
     * through the state like the mode word; the value column names the
     * convention the next ENTER selects
     */
    press(KEY_DOWN);
    render();
    REQUIRE(_ui_dmrMonitorSelectedAction()
            == DMRMON_ACT_LCSS - DMRMON_ACTION_FIRST);
    REQUIRE(monitorValue(DMRMON_ACT_LCSS) == "50 manual");
    REQUIRE(state.dmr_lcssMode == 0);
    sync = press(KEY_ENTER);
    REQUIRE(sync == true);
    REQUIRE(state.dmr_lcssMode == 1);
    REQUIRE(rec->regWrites == 23);
    render();
    REQUIRE(monitorValue(DMRMON_ACT_LCSS) == "50 chip");
    status = sync_rtx_stage();
    REQUIRE(status.dmr_lcssMode == 1);
    REQUIRE(rec->configureCount == configured + 2);
    sync = press(KEY_ENTER);
    REQUIRE(sync == true);
    REQUIRE(state.dmr_lcssMode == 0);
    render();
    REQUIRE(monitorValue(DMRMON_ACT_LCSS) == "50 manual");
    status = sync_rtx_stage();
    REQUIRE(status.dmr_lcssMode == 0);
    REQUIRE(rec->regWrites == 23);

    /* RF advance: 0x12 alternates between the OEM and OpenGD77 words */
    press(KEY_DOWN);
    render();
    REQUIRE(_ui_dmrMonitorSelectedAction()
            == DMRMON_ACT_RFADV - DMRMON_ACTION_FIRST);
    REQUIRE(_ui_dmrMonitorSelectedAction() == DMRMON_ACTION_NUM - 2);
    REQUIRE(monitorValue(DMRMON_RB12) == "98");
    REQUIRE(monitorValue(DMRMON_ACT_RFADV) == "12<-15");
    REQUIRE(state.dmr_rfAdvance == 0);
    sync = press(KEY_ENTER);
    requireWrite(lastWrite(), 0x12, DMRMON_R12_GD77);
    render();
    REQUIRE(monitorValue(DMRMON_RB12) == "15");
    REQUIRE(monitorValue(DMRMON_ACT_RFADV) == "12<-98");

    /* The word is pushed to the RTX stage like the mode word: the modem is
     * reprogrammed with it, so that every later reconfiguration keeps it */
    REQUIRE(sync == true);
    REQUIRE(state.dmr_rfAdvance == DMRMON_R12_GD77);
    status = sync_rtx_stage();
    REQUIRE(status.dmr_rfAdvance == DMRMON_R12_GD77);
    REQUIRE(rec->configureCount == configured + 3);
    REQUIRE(rec->config.rfAdvance == DMRMON_R12_GD77);
    sync_rtx_stage();
    REQUIRE(rec->configureCount == configured + 3);

    sync = press(KEY_ENTER);
    requireWrite(lastWrite(), 0x12, DMRMON_R12_OEM);
    REQUIRE(rec->regWrites == 25);
    REQUIRE(sync == true);
    REQUIRE(state.dmr_rfAdvance == DMRMON_R12_OEM);
    status = sync_rtx_stage();
    REQUIRE(status.dmr_rfAdvance == DMRMON_R12_OEM);
    REQUIRE(rec->configureCount == configured + 4);
    REQUIRE(rec->config.rfAdvance == DMRMON_R12_OEM);
    render();
    REQUIRE(monitorValue(DMRMON_RB12) == "98");

    /* With the two-point test tone on (bit6, 0xD8 as the Two-point action
     * leaves it) the advance is toggled and the tone bit preserved, both
     * in the value column and in the write; a register holding neither
     * word of the pair gets the OEM one */
    dmrbb_writeRegister(DMRMON_PAGE_CONFIG, 0x12, 0xD8);
    render();
    REQUIRE(monitorValue(DMRMON_RB12) == "D8");
    REQUIRE(monitorValue(DMRMON_ACT_RFADV) == "12<-55");
    press(KEY_ENTER);
    requireWrite(lastWrite(), 0x12, 0x55);
    render();
    REQUIRE(monitorValue(DMRMON_RB12) == "55");
    REQUIRE(monitorValue(DMRMON_ACT_RFADV) == "12<-D8");
    press(KEY_ENTER);
    requireWrite(lastWrite(), 0x12, 0xD8);
    dmrbb_writeRegister(DMRMON_PAGE_CONFIG, 0x12, 0x00);
    render();
    REQUIRE(monitorValue(DMRMON_ACT_RFADV) == "12<-98");
    press(KEY_ENTER);
    requireWrite(lastWrite(), 0x12, DMRMON_R12_OEM);
    REQUIRE(rec->regWrites == 30);
    render();
    REQUIRE(monitorValue(DMRMON_RB12) == "98");

    /* The tone bit never reaches the RTX stage: the word pushed after the
     * 0x55 write was 0x15, after 0xD8 and 0x98 it is 0x98 */
    REQUIRE(state.dmr_rfAdvance == DMRMON_R12_OEM);
    status = sync_rtx_stage();
    REQUIRE(status.dmr_rfAdvance == DMRMON_R12_OEM);
    REQUIRE(rec->config.rfAdvance == DMRMON_R12_OEM);
    REQUIRE(rec->configureCount == configured + 4);

    /*
     * SYS line: the driver knob on the pseudo page, not a chip register and
     * not pushed to the RTX stage. The fake modem starts on the pinmap line
     * (PC1), the value column names the line the next ENTER selects
     */
    press(KEY_DOWN);
    render();
    REQUIRE(_ui_dmrMonitorSelectedAction()
            == DMRMON_ACT_SYSLINE - DMRMON_ACTION_FIRST);
    REQUIRE(_ui_dmrMonitorSelectedAction() == DMRMON_ACTION_NUM - 1);
    REQUIRE(dmrbb_readRegister(DMRBB_PAGE_DRIVER, DMRBB_DRV_SYS_LINE)
            == DMRBB_SYS_LINE_PINMAP);
    REQUIRE(monitorValue(DMRMON_ACT_SYSLINE) == "PC1->PC3");
    sync = press(KEY_ENTER);
    REQUIRE(sync == false);
    REQUIRE(rec->regWrites == 31);
    requireDriverWrite(lastWrite(), DMRBB_DRV_SYS_LINE,
                       DMRBB_SYS_LINE_SCHEMATIC);
    REQUIRE(dmrbb_readRegister(DMRBB_PAGE_DRIVER, DMRBB_DRV_SYS_LINE)
            == DMRBB_SYS_LINE_SCHEMATIC);
    render();
    REQUIRE(monitorValue(DMRMON_ACT_SYSLINE) == "PC3->PC1");
    sync = press(KEY_ENTER);
    REQUIRE(sync == false);
    requireDriverWrite(lastWrite(), DMRBB_DRV_SYS_LINE, DMRBB_SYS_LINE_PINMAP);
    REQUIRE(rec->regWrites == 32);
    render();
    REQUIRE(monitorValue(DMRMON_ACT_SYSLINE) == "PC1->PC3");
    status = sync_rtx_stage();
    REQUIRE(rec->configureCount == configured + 4);

    /* The action page scrolls: the last action is drawn, selected */
    REQUIRE(_ui_dmrMonitorCurrentPage() == pages - 1);

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
    REQUIRE(rec->regWrites == 32);

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

/*
 * On the action page every action is pressed once; the value column must
 * carry the hint and the fake modem must see no register write.
 */
static void requireActionsRefused()
{
    const struct dmrEmuRecord *rec = dmrEmu_record();
    uint32_t writes = rec->regWrites;
    uint8_t pages = _ui_dmrMonitorPages();

    press(KEY_LEFT);
    render();
    REQUIRE(_ui_dmrMonitorCurrentPage() == pages - 1);

    for (unsigned i = 0; i < DMRMON_ACTION_NUM; i++) {
        REQUIRE(_ui_dmrMonitorSelectedAction() == i);
        REQUIRE(monitorValue(DMRMON_ACTION_FIRST + i) == "DMR off");
        bool sync = press(KEY_ENTER);
        REQUIRE(sync == false);
        REQUIRE(rec->regWrites == writes);
        press(KEY_DOWN);
        render();
    }

    REQUIRE(state.dmr_modeReg == 0);
    REQUIRE(state.dmr_lcssMode == 0);
    REQUIRE(state.dmr_rfAdvance == 0);
}

TEST_CASE("DMR monitor: the actions are refused unless the RTX runs DMR",
          "[ui][dmr][monitor]")
{
    /*
     * Settings > DMR, and the monitor with it, are reachable in FM: the
     * screen opens and reads back, the actions do not write. The chip is
     * in FM mode there and 0x10 = 0x6A or the two-point tone would take it
     * out of it.
     */
    boot();
    rtxStatus_t status = sync_rtx_stage();
    REQUIRE(status.opMode == OPMODE_FM);
    dmrEmu_reset();

    goto_dmr_monitor();
    render();
    requireActionsRefused();
    press(KEY_ESC);
    REQUIRE(state.ui_screen == SETTINGS_DMR);
    pressN(KEY_ESC, 3);
    REQUIRE(state.ui_screen == MAIN_VFO);

    /* A radio without a DMR modem, switched to DMR: refused as well */
    dmrEmu_setInitResult(-1);
    press(KEY_MONI | KEY_5);
    REQUIRE(state.channel.mode == OPMODE_DMR);
    status = sync_rtx_stage();
    REQUIRE(status.opMode == OPMODE_DMR);
    REQUIRE((status.dmr_markers & DMR_MARK_NOT_SUPPORTED) != 0);

    goto_dmr_monitor();
    render();
    requireActionsRefused();
    press(KEY_ESC);
    pressN(KEY_ESC, 3);
    REQUIRE(state.ui_screen == MAIN_VFO);

    /* The modem is there again: the actions work as in the case above */
    dmrEmu_setInitResult(0);
    pressN(KEY_MONI | KEY_5, 2);
    REQUIRE(state.channel.mode == OPMODE_FM);
    sync_rtx_stage();
    dmrEmu_reset();
    press(KEY_MONI | KEY_5);
    status = sync_rtx_stage();
    REQUIRE(status.opMode == OPMODE_DMR);
    REQUIRE(status.dmr_markers == 0);

    goto_dmr_monitor();
    press(KEY_LEFT);
    render();
    REQUIRE(monitorValue(DMRMON_ACT_RESYNC) == "41<-20");
    press(KEY_ENTER);
    REQUIRE(dmrEmu_record()->regWrites == 2);
    press(KEY_ESC);
}

TEST_CASE("The main screen destination line says TX or WAKE", "[ui][dmr]")
{
    boot_dmr();

    /* The prefix of the destination, both layouts */
    REQUIRE(strcmp(_ui_dmrTxPrefix(DMR_CALL_IDLE, false), "") == 0);
    REQUIRE(strcmp(_ui_dmrTxPrefix(DMR_CALL_RX, false), "") == 0);
    REQUIRE(strcmp(_ui_dmrTxPrefix(DMR_CALL_RX_HANG, true), "") == 0);
    REQUIRE(strcmp(_ui_dmrTxPrefix(DMR_CALL_TX, false), "TX -> ") == 0);
    REQUIRE(strcmp(_ui_dmrTxPrefix(DMR_CALL_TX, true), "TX->") == 0);
    REQUIRE(strcmp(_ui_dmrTxPrefix(DMR_CALL_TX_WAKEUP, false), "WAKE ") == 0);
    REQUIRE(strcmp(_ui_dmrTxPrefix(DMR_CALL_TX_WAKEUP, true), "WAKE:") == 0);

    /* Both render on the main screen with the longest destination */
    ui_state_t ui_state;
    memset(&ui_state, 0, sizeof(ui_state));
    rtxStatus_t status = rtx_getCurrentStatus();
    status.opMode = OPMODE_DMR;
    status.dmr_callType = PRIVATE;
    status.dmr_dstId = DMR_ID_MAX;
    status.dmr_callState = DMR_CALL_TX_WAKEUP;
    _ui_drawModeInfoDMR(&ui_state, &status);
    status.dmr_callState = DMR_CALL_TX;
    _ui_drawModeInfoDMR(&ui_state, &status);

    /* The longest line fits the width of this layout, see the idle line
     * guard above (the compact layout draws the prefix and destination
     * alone, centred, while transmitting) */
    char line[32];
    sniprintf(line, sizeof(line), "%sPC %lu",
              _ui_dmrTxPrefix(DMR_CALL_TX_WAKEUP, CONFIG_SCREEN_HEIGHT < 128),
              (unsigned long)DMR_ID_MAX);
    uint16_t width = gfx_getTextWidth(layout.line2_font, line);
    INFO("line \"" << line << "\" is " << width << " px wide");
    REQUIRE(width <= CONFIG_SCREEN_WIDTH - 2 * layout.horizontal_pad);

    /*
     * The compact layout, whatever this build's screen: 128 pixels wide, a
     * 4 pixel padding on each side and FONT_SIZE_6PT on line 2 (the <= 64
     * pixel layouts of ui.c). The channel string is dropped from the line
     * while transmitting, so the widest forms are prefix + destination.
     */
    const char *compact[] = {
        "TX->PC 16777215",  "TX->TG 16777215",  "TX->ALL",
        "WAKE:PC 16777215", "WAKE:TG 16777215", "WAKE:ALL"
    };
    for (const char *l : compact) {
        uint16_t w = gfx_getTextWidth(FONT_SIZE_6PT, l);
        INFO("compact line \"" << l << "\" is " << w << " px wide");
        REQUIRE(w <= 128 - 2 * 4);
    }

    /* The busy marker of a rejected transmission, from the RTX flags */
    REQUIRE(strcmp(_ui_dmrMarkerText(DMR_MARK_BUSY), currentLanguage->dmrBusy)
            == 0);
}

/*
 * Stage 6: contacts, favourites, last heard, the hang time display, the
 * Short LC busy hints and the voice prompt of a received call.
 */

/* Run the RTX stage until the DMR call state reaches 'target' */
static bool rtx_until(uint8_t target, unsigned limit = 400)
{
    for (unsigned i = 0; i < limit; i++) {
        if (rtx_getCurrentStatus().dmr_callState == target)
            return true;
        rtx_task();
    }
    return rtx_getCurrentStatus().dmr_callState == target;
}

/* A system event carrying a Short LC as the chip would report it */
static void inject_short_lc(uint32_t message, bool goodCrc = true)
{
    struct dmrbbSnapshot s;
    memset(&s, 0x00, sizeof(s));
    s.irq82 = DMR::R82_TX_END;
    s.sub86 = DMR::R86_SHORT_LC;
    DMR::FEC::ShortLc::toChipBytes(message, s.shortLc);
    if (!goodCrc)
        s.shortLc[4] ^= 0xFF;
    dmrEmu_pushSys(&s);
    rtx_task();
}

static uint32_t act_updt(uint8_t ts1, uint8_t ts2, uint32_t dst1, uint32_t dst2)
{
    DMR::ActivityUpdate au;
    au.activity[0] = ts1;
    au.activity[1] = ts2;
    au.hash[0] = DMR::ActivityUpdate::hashAddress(dst1);
    au.hash[1] = DMR::ActivityUpdate::hashAddress(dst2);
    return au.pack();
}

/* A base-station terminator with LC for the given call, on the call's slot */
static void inject_terminator(uint32_t src, uint32_t dst, uint8_t ts)
{
    DMR::FullLC lc;
    lc.clear();
    lc.flco = DMR::FLCO_GRP_V_CH_USR;
    lc.fid = DMR::FID_STANDARD;
    lc.src = src;
    lc.dst = dst;

    struct dmrbbSnapshot s;
    memset(&s, 0x00, sizeof(s));
    s.irq82 = DMR::R82_CTRL_FRAME;
    s.r5f = DMR::R5F_SYNC_BS;
    s.r52 = (uint8_t)((1 << 4) | ((ts == 2) ? DMR::R52_TC : 0));
    s.r51 = (uint8_t)((DMR::DT_TERMINATOR_LC << 4) | DMR::R51_CLASS_HEADER);
    lc.toChipLc(s.lc);
    dmrEmu_pushSys(&s);
    rtx_task();
}

/* From a main screen: Menu > Contacts */
static void goto_contacts()
{
    press(KEY_ENTER);
    REQUIRE(state.ui_screen == MENU_TOP);
    pressN(KEY_DOWN, M_CONTACTS);
    press(KEY_ENTER);
    REQUIRE(state.ui_screen == MENU_CONTACTS);
}

TEST_CASE("Contacts: ENTER on a DMR contact sets the destination and its name",
          "[ui][dmr][contacts]")
{
    boot_dmr();

    /* The codeplug of the bench holds the two DMR contacts and an M17 one */
    contact_t c;
    REQUIRE(cps_readContact(&c, 0) == 0);
    REQUIRE(strcmp(c.name, "BM Italy") == 0);
    REQUIRE(cps_readContact(&c, 3) == 0);
    REQUIRE(strcmp(c.name, WIDE_CONTACT_NAME) == 0);
    REQUIRE(cps_readContact(&c, 4) == -1);

    SECTION("A group contact becomes a group call, named on the main screen")
    {
        goto_contacts();
        bool sync = press(KEY_ENTER);
        REQUIRE(sync == true);
        REQUIRE(state.ui_screen == MAIN_VFO);
        REQUIRE(state.settings.dmr_talkgroup == 2222);
        REQUIRE(state.settings.dmr_callType == GROUP);
        REQUIRE(strcmp(state.dmr_destName, "BM Italy") == 0);
        render();

        /* Typing a destination drops the name */
        press(KEY_HASH);
        typeNumber("31665");
        press(KEY_ENTER);
        REQUIRE(state.settings.dmr_talkgroup == 31665);
        REQUIRE(state.dmr_destName[0] == '\0');
    }

    SECTION("A private contact becomes a private call, '*' drops the name")
    {
        goto_contacts();
        press(KEY_DOWN);
        press(KEY_ENTER);
        REQUIRE(state.ui_screen == MAIN_VFO);
        REQUIRE(state.settings.dmr_talkgroup == 2345678);
        REQUIRE(state.settings.dmr_callType == PRIVATE);
        REQUIRE(strcmp(state.dmr_destName, "IZ1ABC Mario") == 0);
        render();

        press(KEY_STAR);
        REQUIRE(state.settings.dmr_callType == GROUP);
        REQUIRE(state.dmr_destName[0] == '\0');
    }

    SECTION("An M17 contact is refused in DMR")
    {
        goto_contacts();
        pressN(KEY_DOWN, 2);
        press(KEY_ENTER);
        REQUIRE(state.ui_screen == MENU_CONTACTS);
        REQUIRE(state.settings.dmr_talkgroup == 9);
        REQUIRE(state.dmr_destName[0] == '\0');
    }

    SECTION("Settings > DMR edits drop the name")
    {
        goto_contacts();
        press(KEY_ENTER);
        REQUIRE(strcmp(state.dmr_destName, "BM Italy") == 0);
        goto_dmr_settings();
        pressN(KEY_DOWN, DMR_CALLTYPE);
        press(KEY_RIGHT);
        REQUIRE(state.settings.dmr_callType == PRIVATE);
        REQUIRE(state.dmr_destName[0] == '\0');
    }
}

TEST_CASE("Contacts: ENTER on a DMR contact does nothing in FM",
          "[ui][dmr][contacts]")
{
    boot();
    REQUIRE(state.channel.mode == OPMODE_FM);
    goto_contacts();
    bool sync = press(KEY_ENTER);
    REQUIRE(sync == false);
    REQUIRE(state.ui_screen == MENU_CONTACTS);
    REQUIRE(state.settings.dmr_talkgroup == 9);
    REQUIRE(state.dmr_destName[0] == '\0');
}

TEST_CASE("Contact names are looked up and cached", "[ui][dmr][contacts]")
{
    boot();
    char name[DMR_DEST_NAME_LEN];

    REQUIRE(_ui_dmrContactName(2222, GROUP, name) == true);
    REQUIRE(strcmp(name, "BM Italy") == 0);
    REQUIRE(_ui_dmrContactName(2345678, PRIVATE, name) == true);
    REQUIRE(strcmp(name, "IZ1ABC Mario") == 0);

    /* The type is part of the key, an unknown ID is unknown */
    REQUIRE(_ui_dmrContactName(2222, PRIVATE, name) == false);
    REQUIRE(_ui_dmrContactName(1234567, PRIVATE, name) == false);

    /* Cached answers survive the codeplug going away */
    cps_close();
    REQUIRE(_ui_dmrContactName(2222, GROUP, name) == true);
    REQUIRE(strcmp(name, "BM Italy") == 0);
    REQUIRE(_ui_dmrContactName(1234567, PRIVATE, name) == false);
    REQUIRE(cps_open(cpsPath) == 0);
}

/* From a main screen: Menu > Settings > DMR > Favourites */
static void goto_favourites()
{
    goto_dmr_settings();
    pressN(KEY_DOWN, DMR_FAVOURITES_ENTRY);
    press(KEY_ENTER);
    REQUIRE(state.ui_screen == DMR_FAVOURITES);
}

TEST_CASE("Settings > DMR > Favourites stores and recalls destinations",
          "[ui][dmr][favourites]")
{
    boot_dmr();
    char buf[MAX_ENTRY_LEN];

    goto_favourites();
    render();

    /* All six slots empty */
    for (unsigned i = 0; i < DMR_FAV_NUM; i++) {
        REQUIRE(_ui_getFavouriteEntryName(buf, sizeof(buf), i) == 0);
        REQUIRE(atoi(buf) == (int)(i + 1));
        REQUIRE(_ui_getFavouriteValueName(buf, sizeof(buf), i) == 0);
        REQUIRE(strcmp(buf, currentLanguage->empty) == 0);
    }
    REQUIRE(_ui_getFavouriteEntryName(buf, sizeof(buf), DMR_FAV_NUM) == -1);

    /* ENTER on an empty slot changes nothing */
    bool sync = press(KEY_ENTER);
    REQUIRE(sync == false);
    REQUIRE(state.ui_screen == DMR_FAVOURITES);

    /* '*' stores the current destination (TG 9) in slot 1 */
    press(KEY_STAR);
    REQUIRE(state.settings.dmr_favourites[0] == DMR_FAV_PACK(9, GROUP));
    REQUIRE(_ui_getFavouriteValueName(buf, sizeof(buf), 0) == 0);
    REQUIRE(strcmp(buf, "TG 9") == 0);

    /* A private destination into slot 2, a broadcast into slot 6 */
    state.settings.dmr_talkgroup = 2345678;
    state.settings.dmr_callType = PRIVATE;
    ui_saveState();
    press(KEY_DOWN);
    press(KEY_STAR);
    REQUIRE(state.settings.dmr_favourites[1] == DMR_FAV_PACK(2345678, PRIVATE));
    REQUIRE(_ui_getFavouriteValueName(buf, sizeof(buf), 1) == 0);
    REQUIRE(strcmp(buf, "PC 2345678") == 0);

    state.settings.dmr_callType = ALL;
    ui_saveState();
    press(KEY_UP);
    press(KEY_UP);
    press(KEY_STAR);
    REQUIRE(state.settings.dmr_favourites[5] == DMR_FAV_PACK(2345678, ALL));
    REQUIRE(_ui_getFavouriteValueName(buf, sizeof(buf), 5) == 0);
    REQUIRE(strcmp(buf, currentLanguage->broadcast) == 0);
    render();

    /* ENTER on slot 1 makes it the destination and returns to the VFO;
     * TG 9 is no contact, so the destination has no name */
    pressN(KEY_DOWN, 1);
    sync = press(KEY_ENTER);
    REQUIRE(sync == true);
    REQUIRE(state.ui_screen == MAIN_VFO);
    REQUIRE(state.settings.dmr_talkgroup == 9);
    REQUIRE(state.settings.dmr_callType == GROUP);
    REQUIRE(state.dmr_destName[0] == '\0');

    /* Slot 2 holds the ID of a contact: its name is looked up */
    goto_favourites();
    press(KEY_DOWN);
    sync = press(KEY_ENTER);
    REQUIRE(sync == true);
    REQUIRE(state.ui_screen == MAIN_VFO);
    REQUIRE(state.settings.dmr_talkgroup == 2345678);
    REQUIRE(state.settings.dmr_callType == PRIVATE);
    REQUIRE(strcmp(state.dmr_destName, "IZ1ABC Mario") == 0);

    /* ENTER on the slot holding the current destination still syncs the
     * RTX stage: the screen is left from inside Settings > DMR, whose
     * edits reach the RTX on the way out (the ESC branch does the same) */
    goto_favourites();
    press(KEY_DOWN);
    state.channel.dmr.rxColorCode = 5;
    ui_saveState();
    sync = press(KEY_ENTER);
    REQUIRE(sync == true);
    REQUIRE(state.ui_screen == MAIN_VFO);
    REQUIRE(state.settings.dmr_talkgroup == 2345678);
    REQUIRE(strcmp(state.dmr_destName, "IZ1ABC Mario") == 0);
    state.channel.dmr.rxColorCode = 1;
    ui_saveState();

    /* '#' empties a slot, ESC goes back to the Favourites item */
    goto_favourites();
    press(KEY_DOWN);
    press(KEY_HASH);
    REQUIRE(state.settings.dmr_favourites[1] == 0);
    press(KEY_ESC);
    REQUIRE(state.ui_screen == SETTINGS_DMR);

    /* A zero destination cannot be stored */
    state.settings.dmr_talkgroup = 0;
    ui_saveState();
    press(KEY_ENTER);
    REQUIRE(state.ui_screen == DMR_FAVOURITES);
    press(KEY_STAR);
    REQUIRE(state.settings.dmr_favourites[0] == DMR_FAV_PACK(9, GROUP));
}

TEST_CASE("Last heard: injected calls fill the list, MONI+4 shows it",
          "[ui][dmr][lastheard]")
{
    fresh_dmr_rtx();
    state.settings.dmr_hangTime = 0;
    state.settings.dmr_monitor = 1;
    ui_saveState();
    sync_rtx_stage();
    for (unsigned i = 0; i < 6; i++) {
        dmrEmu_pushTs();
        rtx_task();
    }

    /* Empty list: the screen opens and closes */
    REQUIRE(dmrLastHeard_count() == 0);
    press(KEY_MONI | KEY_4);
    REQUIRE(state.ui_screen == DMR_LAST_HEARD);
    render();
    press(KEY_ENTER);
    REQUIRE(state.ui_screen == DMR_LAST_HEARD);
    press(KEY_ESC);
    REQUIRE(state.ui_screen == MAIN_VFO);

    /* Two calls, twelve bursts each: one entry per call */
    dmrEmu_injectGroupCall(2345678, 31665, 1, 1, 12);
    REQUIRE(rtx_until(DMR_CALL_RX));
    REQUIRE(rtx_until(DMR_CALL_IDLE));
    REQUIRE(dmrLastHeard_count() == 1);

    dmrEmu_injectGroupCall(1234567, 9, 1, 1, 12);
    REQUIRE(rtx_until(DMR_CALL_RX));
    REQUIRE(rtx_until(DMR_CALL_IDLE));
    REQUIRE(dmrLastHeard_count() == 2);

    struct dmrLastHeardEntry e;
    REQUIRE(dmrLastHeard_get(0, &e) == true);
    REQUIRE(e.src == 1234567);
    REQUIRE(e.dst == 9);
    REQUIRE(e.flco == 0);
    REQUIRE(dmrLastHeard_get(1, &e) == true);
    REQUIRE(e.src == 2345678);
    REQUIRE(e.dst == 31665);

    /* The rows: newest first, the contact's name when known, the
     * destination on its own line, never cut, and the age */
    char buf[MAX_ENTRY_LEN];
    REQUIRE(_ui_getLastHeardEntryName(buf, sizeof(buf), 0) == 0);
    REQUIRE(strcmp(buf, "1234567") == 0);
    REQUIRE(_ui_getLastHeardDestName(buf, sizeof(buf), 0) == 0);
    REQUIRE(strcmp(buf, "-> TG 9") == 0);
    REQUIRE(_ui_getLastHeardEntryName(buf, sizeof(buf), 1) == 0);
    REQUIRE(strcmp(buf, "IZ1ABC Mario") == 0);
    REQUIRE(_ui_getLastHeardDestName(buf, sizeof(buf), 1) == 0);
    REQUIRE(strcmp(buf, "-> TG 31665") == 0);
    REQUIRE(_ui_getLastHeardEntryName(buf, sizeof(buf), 2) == -1);
    REQUIRE(_ui_getLastHeardDestName(buf, sizeof(buf), 2) == -1);
    REQUIRE(_ui_getLastHeardValueName(buf, sizeof(buf), 0) == 0);
    REQUIRE(strcmp(buf, "0s") == 0);
    REQUIRE(_ui_getLastHeardValueName(buf, sizeof(buf), 2) == -1);

    /* MONI + 4 on the VFO screen opens the list, UP/DOWN browse it */
    press(KEY_MONI | KEY_4);
    REQUIRE(state.ui_screen == DMR_LAST_HEARD);
    render();
    press(KEY_DOWN);
    press(KEY_DOWN);
    press(KEY_UP);
    render();

    /* ENTER: private call to the selected source, named, back to the VFO */
    bool sync = press(KEY_ENTER);
    REQUIRE(sync == true);
    REQUIRE(state.ui_screen == MAIN_VFO);
    REQUIRE(state.settings.dmr_talkgroup == 2345678);
    REQUIRE(state.settings.dmr_callType == PRIVATE);
    REQUIRE(strcmp(state.dmr_destName, "IZ1ABC Mario") == 0);
    render();

    /* An unknown source has no name */
    press(KEY_MONI | KEY_4);
    press(KEY_ENTER);
    REQUIRE(state.settings.dmr_talkgroup == 1234567);
    REQUIRE(state.settings.dmr_callType == PRIVATE);
    REQUIRE(state.dmr_destName[0] == '\0');

    /* ESC leaves the destination alone; the MEM screen opens it too */
    state.settings.dmr_talkgroup = 9;
    state.settings.dmr_callType = GROUP;
    state.ui_screen = MAIN_MEM;
    ui_saveState();
    press(KEY_MONI | KEY_4);
    REQUIRE(state.ui_screen == DMR_LAST_HEARD);
    press(KEY_ESC);
    REQUIRE(state.ui_screen == MAIN_MEM);
    REQUIRE(state.settings.dmr_talkgroup == 9);

    /* Not from a menu, not in FM */
    state.ui_screen = MAIN_VFO;
    ui_saveState();
    press(KEY_ENTER);
    REQUIRE(state.ui_screen == MENU_TOP);
    press(KEY_MONI | KEY_4);
    REQUIRE(state.ui_screen == MENU_TOP);
    press(KEY_ESC);
    pressN(KEY_MONI | KEY_5, 2);
    REQUIRE(state.channel.mode == OPMODE_FM);
    press(KEY_MONI | KEY_4);
    REQUIRE(state.ui_screen == MAIN_VFO);

    /* The same call again moves to the top, no new entry */
    pressN(KEY_MONI | KEY_5, 1);
    REQUIRE(state.channel.mode == OPMODE_DMR);
    sync_rtx_stage();
    for (unsigned i = 0; i < 6; i++) {
        dmrEmu_pushTs();
        rtx_task();
    }
    dmrEmu_injectGroupCall(2345678, 31665, 1, 1, 6);
    REQUIRE(rtx_until(DMR_CALL_RX));
    REQUIRE(rtx_until(DMR_CALL_IDLE));
    REQUIRE(dmrLastHeard_count() == 2);
    REQUIRE(dmrLastHeard_get(0, &e) == true);
    REQUIRE(e.src == 2345678);
}

TEST_CASE("Hang time: the caller stays shown, hang terminators extend it",
          "[ui][dmr][hang]")
{
    fresh_dmr_rtx();
    state.settings.dmr_hangTime = 1;
    state.settings.dmr_monitor = 1;
    ui_saveState();
    sync_rtx_stage();
    for (unsigned i = 0; i < 6; i++) {
        dmrEmu_pushTs();
        rtx_task();
    }

    dmrEmu_injectGroupCall(2345678, 31665, 1, 1, 6);
    REQUIRE(rtx_until(DMR_CALL_RX));
    rtxStatus_t status = rtx_getCurrentStatus();
    REQUIRE(_ui_dmrCallReceived(&status) == true);
    render();

    /* The terminator ends the call: the caller stays for the hang time */
    REQUIRE(rtx_until(DMR_CALL_RX_HANG));
    status = rtx_getCurrentStatus();
    REQUIRE(_ui_dmrCallReceived(&status) == true);
    REQUIRE(status.dmr_rxSrcId == 2345678);
    REQUIRE(status.dmr_rxDstId == 31665);
    render();

    /* 600 ms in: a hang time terminator of the same call restarts it */
    sleepFor(0, 600);
    rtx_task();
    REQUIRE(rtx_getCurrentStatus().dmr_callState == DMR_CALL_RX_HANG);
    inject_terminator(2345678, 31665, 1);

    /* 1.2 s after the call ended it is still shown */
    sleepFor(0, 600);
    rtx_task();
    status = rtx_getCurrentStatus();
    REQUIRE(status.dmr_callState == DMR_CALL_RX_HANG);
    REQUIRE(_ui_dmrCallReceived(&status) == true);
    REQUIRE(status.dmr_rxSrcId == 2345678);
    render();

    /* A terminator of another call does not extend it: 1.1 s later idle */
    inject_terminator(1111111, 31665, 1);
    sleepFor(0, 1100);
    rtx_task();
    status = rtx_getCurrentStatus();
    REQUIRE(status.dmr_callState == DMR_CALL_IDLE);
    REQUIRE(_ui_dmrCallReceived(&status) == false);
    render();

    /* The last heard list saw one call */
    REQUIRE(dmrLastHeard_count() == 1);
}

TEST_CASE("Short LC: the Activity Update raises the TS busy hints",
          "[ui][dmr][shortlc]")
{
    fresh_dmr_rtx();
    char buf[MAX_ENTRY_LEN];

    rtxStatus_t status = rtx_getCurrentStatus();
    REQUIRE(status.dmr_tsBusy == 0);

    /* Group voice on TS1 to 31665, nothing on TS2 */
    uint32_t msg = act_updt(DMR::ACT_GROUP_VOICE, DMR::ACT_NONE, 31665, 0);
    inject_short_lc(msg);
    status = rtx_getCurrentStatus();
    REQUIRE(status.dmr_tsBusy == 0x01);
    REQUIRE(status.dmr_mon.shortLc == msg);
    REQUIRE(status.dmr_mon.shortLcOk == 1);
    REQUIRE(status.dmr_mon.shortLcBad == 0);

    /* Main screen: "1*" next to the channel; both slots: "1* 2*" */
    ui_state_t ui_state;
    memset(&ui_state, 0, sizeof(ui_state));
    render();
    _ui_drawModeInfoDMR(&ui_state, &status);

    msg = act_updt(DMR::ACT_INDIVIDUAL_DATA, DMR::ACT_GROUP_VOICE, 1234567, 9);
    inject_short_lc(msg);
    status = rtx_getCurrentStatus();
    REQUIRE(status.dmr_tsBusy == 0x03);
    REQUIRE(status.dmr_mon.shortLcOk == 2);
    _ui_drawModeInfoDMR(&ui_state, &status);

    /* Monitor screen rows */
    goto_dmr_monitor();
    render();
    REQUIRE(_ui_getDMRMonitorEntryName(buf, sizeof(buf), DMRMON_SLC) == 0);
    REQUIRE(strcmp(buf, "Short LC") == 0);
    REQUIRE(_ui_getDMRMonitorValueName(buf, sizeof(buf), DMRMON_SLC) == 0);
    char expected[MAX_ENTRY_LEN];
    snprintf(expected, sizeof(expected), "%07lX 2/0", (unsigned long)msg);
    REQUIRE(strcmp(buf, expected) == 0);
    REQUIRE(_ui_getDMRMonitorEntryName(buf, sizeof(buf), DMRMON_TSBUSY) == 0);
    REQUIRE(strcmp(buf, "TS busy") == 0);
    REQUIRE(_ui_getDMRMonitorValueName(buf, sizeof(buf), DMRMON_TSBUSY) == 0);
    REQUIRE(strcmp(buf, "TS1 TS2") == 0);

    /* A bad CRC is counted and changes nothing else */
    inject_short_lc(msg, false);
    status = rtx_getCurrentStatus();
    REQUIRE(status.dmr_tsBusy == 0x03);
    REQUIRE(status.dmr_mon.shortLcOk == 2);
    REQUIRE(status.dmr_mon.shortLcBad == 1);

    /* No activity on either slot */
    inject_short_lc(act_updt(DMR::ACT_NONE, DMR::ACT_NONE, 0, 0));
    status = rtx_getCurrentStatus();
    REQUIRE(status.dmr_tsBusy == 0);
    render();
    REQUIRE(_ui_getDMRMonitorValueName(buf, sizeof(buf), DMRMON_TSBUSY) == 0);
    REQUIRE(strcmp(buf, "-") == 0);

    /* A Null message leaves the hints as they are */
    inject_short_lc(act_updt(DMR::ACT_GROUP_VOICE, DMR::ACT_NONE, 9, 0));
    inject_short_lc(0x00000000);
    REQUIRE(rtx_getCurrentStatus().dmr_tsBusy == 0x01);

    /* The MEM screen draws them too, next to the bank and channel name */
    pressN(KEY_ESC, 4);
    REQUIRE(state.ui_screen == MAIN_VFO);
    state.ui_screen = MAIN_MEM;
    ui_saveState();
    render();
    status = rtx_getCurrentStatus();
    REQUIRE(status.dmr_tsBusy == 0x01);
    ui_state.input_locked = true;
    _ui_drawModeInfoDMR(&ui_state, &status);
    state.ui_screen = MAIN_VFO;
    ui_saveState();

    /* Every language carries the new strings */
    for (const stringsTable_t *lang = languages;
         lang < languages + NUM_LANGUAGES; lang++) {
        REQUIRE(lang->lastHeard != NULL);
        REQUIRE(lang->favourites != NULL);
        REQUIRE(lang->empty != NULL);
    }
}

TEST_CASE("The TS busy tags fit the top bar next to the clock and the lock",
          "[ui][dmr][shortlc]")
{
    /*
     * Mirrors _ui_drawModeInfoDMR(): the tags "1*2*" (both slots busy, the
     * widest form) in layout.message_font at the left of the top bar,
     * shifted right by DMR_TSBUSY_LOCK_OFFSET when the keypad is locked,
     * must end before the clock _ui_drawMainTop() centres in
     * layout.top_font. The MEM screen keeps its bank and channel name on
     * line 1, which is why the tags live in the top bar on both layouts.
     */
    const char *clock = "23:59:59";
    uint16_t clockWidth = gfx_getTextWidth(layout.top_font, clock);
    uint16_t clockLeft = (CONFIG_SCREEN_WIDTH - clockWidth) / 2;
    uint16_t tagsWidth = gfx_getTextWidth(layout.message_font, "1*2*");
    uint16_t tagsRight = layout.top_pos.x + DMR_TSBUSY_LOCK_OFFSET + tagsWidth;
    INFO("tags end at " << tagsRight << " px, the clock starts at " << clockLeft
                        << " px");
    REQUIRE(tagsRight < clockLeft);
    REQUIRE(gfx_getTextWidth(layout.message_font, "1*") < tagsWidth);
}

TEST_CASE("The last heard rows fit the widest IDs and cut a name to fit",
          "[ui][dmr][lastheard]")
{
    /*
     * Mirrors _ui_drawDMRLastHeard(): the name or source ID shares its
     * line with the age (right-aligned, "99h" at the widest), the
     * destination has the line below for itself, both in layout.menu_font
     * inside the horizontal padding. An 8-digit source with the widest age
     * and "-> " with the widest destination must fit as they are; the
     * widest contact name is cut by _ui_getLastHeardEntryName() until it
     * fits, the destination is never touched.
     */
    const uint16_t usable = CONFIG_SCREEN_WIDTH - 2 * layout.horizontal_pad;
    const uint16_t gap = 4;
    uint16_t ageWidth = gfx_getTextWidth(layout.menu_font, "99h");
    uint16_t srcWidth = gfx_getTextWidth(layout.menu_font, "16777215");
    INFO("source " << srcWidth << " px + age " << ageWidth << " px in "
                   << usable << " px");
    REQUIRE(srcWidth + gap + ageWidth <= usable);

    const char *dests[] = { "-> PC 16777215", "-> TG 16777215", "-> ALL",
                            "-> PC16777215", "-> TG16777215" };
    for (const char *d : dests) {
        uint16_t w = gfx_getTextWidth(layout.menu_font, d);
        INFO(d << " (" << w << " px) in " << usable << " px");
        REQUIRE(w <= usable);
    }

    /* A call from the contact with the 15-character name */
    fresh_dmr_rtx();
    state.settings.dmr_hangTime = 0;
    state.settings.dmr_monitor = 1;
    ui_saveState();
    sync_rtx_stage();
    for (unsigned i = 0; i < 6; i++) {
        dmrEmu_pushTs();
        rtx_task();
    }
    dmrEmu_injectGroupCall(WIDE_CONTACT_ID, 16777214, 1, 1, 12);
    REQUIRE(rtx_until(DMR_CALL_RX));
    REQUIRE(rtx_until(DMR_CALL_IDLE));
    REQUIRE(dmrLastHeard_count() == 1);

    char buf[MAX_ENTRY_LEN];
    REQUIRE(_ui_getLastHeardEntryName(buf, sizeof(buf), 0) == 0);
    REQUIRE(strlen(buf) >= 1);
    REQUIRE(strncmp(buf, WIDE_CONTACT_NAME, strlen(buf)) == 0);
    uint16_t nameWidth = gfx_getTextWidth(layout.menu_font, buf);
    INFO("name cut to '" << buf << "' (" << nameWidth << " px)");
    REQUIRE(nameWidth + gap + ageWidth <= usable);
    bool cut = (strlen(buf) < strlen(WIDE_CONTACT_NAME));
    bool wholeFits =
        (gfx_getTextWidth(layout.menu_font, WIDE_CONTACT_NAME) + gap + ageWidth
         <= usable);
    REQUIRE(cut == !wholeFits);

    REQUIRE(_ui_getLastHeardDestName(buf, sizeof(buf), 0) == 0);
    REQUIRE(strcmp(buf, "-> TG 16777214") == 0);

    /* The list draws with the cut name */
    press(KEY_MONI | KEY_4);
    REQUIRE(state.ui_screen == DMR_LAST_HEARD);
    render();
    press(KEY_ESC);
}

TEST_CASE("A received call is announced once by the voice prompts",
          "[ui][dmr][vp]")
{
    fresh_dmr_rtx();
    state.settings.dmr_hangTime = 0;
    state.settings.dmr_monitor = 1;
    state.settings.vpLevel = vpHigh;
    ui_saveState();
    sync_rtx_stage();
    for (unsigned i = 0; i < 6; i++) {
        dmrEmu_pushTs();
        rtx_task();
    }

    /* The first status event of the call queues the announcement, the
     * next one for the same call does not queue it again */
    REQUIRE(vp_sequenceNotEmpty() == false);
    dmrEmu_injectGroupCall(2345678, 31665, 1, 1, 12);
    REQUIRE(rtx_until(DMR_CALL_RX));
    render();
    REQUIRE(vp_sequenceNotEmpty() == true);
    vp_flush();
    REQUIRE(vp_sequenceNotEmpty() == false);
    render();
    REQUIRE(vp_sequenceNotEmpty() == false);
    REQUIRE(rtx_until(DMR_CALL_IDLE));
    render();
    REQUIRE(vp_sequenceNotEmpty() == false);

    /* A new call is announced again */
    dmrEmu_injectGroupCall(1234567, 9, 1, 1, 12);
    REQUIRE(rtx_until(DMR_CALL_RX));
    render();
    REQUIRE(vp_sequenceNotEmpty() == true);
    vp_flush();
    REQUIRE(rtx_until(DMR_CALL_IDLE));
    render();

    /* The prompt itself, for every call type */
    vp_announceDmrCall(2345678, 31665, GROUP, vpqInit);
    vp_announceDmrCall(2345678, 1234567, PRIVATE,
                       (enum vpQueueFlags)(vpqInit | vpqIncludeDescriptions));
    vp_announceDmrCall(2345678, DMR_ID_MAX, ALL, vpqInit);
    vp_flush();
    state.settings.vpLevel = vpNone;
    ui_saveState();
}
