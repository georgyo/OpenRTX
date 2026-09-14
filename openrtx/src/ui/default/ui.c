/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The graphical user interface (GUI) works by splitting the screen in
 * horizontal rows, with row height depending on vertical resolution.
 *
 * The general screen layout is composed by an upper status bar at the
 * top of the screen and a lower status bar at the bottom.
 * The central portion of the screen is filled by two big text/number rows
 * And a small row.
 *
 * Below is shown the row height for two common display densities.
 *
 *        160x128 display (MD380)            Recommended font size
 *      ┌─────────────────────────┐
 *      │  top_status_bar (16px)  │  8 pt (11 px) font with 2 px vertical padding
 *      │      top_pad (4px)      │  4 px padding
 *      │      Line 1 (20px)      │  8 pt (11 px) font with 4 px vertical padding
 *      │      Line 2 (20px)      │  8 pt (11 px) font with 4 px vertical padding
 *      │                         │
 *      │      Line 3 (40px)      │  16 pt (xx px) font with 6 px vertical padding
 *      │ RSSI+squelch bar (20px) │  20 px
 *      │      bottom_pad (4px)   │  4 px padding
 *      └─────────────────────────┘
 *
 *         128x64 display (GD-77)
 *      ┌─────────────────────────┐
 *      │  top_status_bar (11 px) │  6 pt (9 px) font with 1 px vertical padding
 *      │      top_pad (1px)      │  1 px padding
 *      │      Line 1 (10px)      │  6 pt (9 px) font without vertical padding
 *      │      Line 2 (10px)      │  6 pt (9 px) font with 2 px vertical padding
 *      │      Line 3 (18px)      │  12 pt (xx px) font with 0 px vertical padding
 *      │ RSSI+squelch bar (11px) │  11 px
 *      │      bottom_pad (1px)   │  1 px padding
 *      └─────────────────────────┘
 *
 *         128x48 display (RD-5R)
 *      ┌─────────────────────────┐
 *      │  top_status_bar (11 px) │  6 pt (9 px) font with 1 px vertical padding
 *      ├─────────────────────────┤  1 px line
 *      │      Line 2 (10px)      │  8 pt (11 px) font with 4 px vertical padding
 *      │      Line 3 (18px)      │  8 pt (11 px) font with 4 px vertical padding
 *      └─────────────────────────┘
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <math.h>
#include "ui/ui_default.h"
#include "rtx/rtx.h"
#include "interfaces/platform.h"
#include "interfaces/display.h"
#include "interfaces/cps_io.h"
#include "interfaces/nvmem.h"
#include "interfaces/delays.h"
#include <string.h>
#include "core/battery.h"
#include "core/input.h"
#include "core/utils.h"
#include "hwconfig.h"
#include "core/voicePromptUtils.h"
#include "core/beeps.h"
#include "core/frs.h"

/* UI main screen functions, their implementation is in "ui_main.c" */
extern void _ui_drawMainBackground();
extern void _ui_drawMainTop(ui_state_t* ui_state);
extern void _ui_drawVFOMiddle();
extern void _ui_drawMEMMiddle();
extern void _ui_drawVFOBottom();
extern void _ui_drawMEMBottom();
extern void _ui_drawMainVFO(ui_state_t* ui_state);
extern void _ui_drawMainVFOInput(ui_state_t* ui_state);
extern void _ui_drawMainMEM(ui_state_t* ui_state);
extern void _ui_drawMainFRS(ui_state_t* ui_state);
extern void _ui_drawMainFRSInput(ui_state_t* ui_state);
/* UI menu functions, their implementation is in "ui_menu.c" */
extern void _ui_drawMenuTop(ui_state_t* ui_state);
extern void _ui_drawMenuBank(ui_state_t* ui_state);
extern void _ui_drawMenuChannel(ui_state_t* ui_state);
extern void _ui_drawMenuContacts(ui_state_t* ui_state);
#ifdef CONFIG_GPS
extern void _ui_drawMenuGPS();
extern void _ui_drawSettingsGPS(ui_state_t* ui_state);
#endif
extern void _ui_drawSettingsAccessibility(ui_state_t* ui_state);
extern void _ui_drawMenuSettings(ui_state_t* ui_state);
extern void _ui_drawMenuBackupRestore(ui_state_t* ui_state);
extern void _ui_drawMenuBackup(ui_state_t* ui_state);
extern void _ui_drawMenuRestore(ui_state_t* ui_state);
extern void _ui_drawMenuInfo(ui_state_t* ui_state);
extern void _ui_drawMenuAbout(ui_state_t* ui_state);
#ifdef CONFIG_RTC
extern void _ui_drawSettingsTimeDate();
extern void _ui_drawSettingsTimeDateSet(ui_state_t* ui_state);
#endif
extern void _ui_drawSettingsDisplay(ui_state_t* ui_state);
extern void _ui_drawSettingsM17(ui_state_t* ui_state);
extern void _ui_drawSettingsFM(ui_state_t* ui_state);
extern void _ui_drawSettingsFRS(ui_state_t* ui_state);
extern void _ui_drawFRSCode(ui_state_t* ui_state);
extern void _ui_drawSettingsDMR(ui_state_t* ui_state);
extern void _ui_drawSettingsVoicePrompts(ui_state_t* ui_state);
extern void _ui_drawSettingsReset2Defaults(ui_state_t* ui_state);
extern void _ui_drawSettingsRadio(ui_state_t* ui_state);
extern bool _ui_drawMacroMenu(ui_state_t* ui_state);
extern void _ui_reset_menu_anouncement_tracking();
// TODO: get these from ui strings / currentLanguage
const char *menu_items[] =
{
    "Banks",
    "Channels",
    "Contacts",
#ifdef CONFIG_GPS
    "GPS",
#endif
    "Settings",
    "Info",
    "About"
};

const char *settings_items[] =
{
    "Display",
#ifdef CONFIG_RTC
    "Time & Date",
#endif
#ifdef CONFIG_GPS
    "GPS",
#endif
    "Radio",
#ifdef CONFIG_M17
    "M17",
#endif
#ifdef CONFIG_DMR
    "DMR",
#endif
    "FM",
    "FRS",
    "Accessibility",
    "Default Settings"
};

const char *display_items[] =
{
#ifdef CONFIG_SCREEN_BRIGHTNESS
    "Brightness",
#endif
#ifdef CONFIG_SCREEN_CONTRAST
    "Contrast",
#endif
    "Timer",
    "Battery Icon"
};

#ifdef CONFIG_GPS
const char *settings_gps_items[] =
{
    "GPS Enabled",
#ifdef CONFIG_RTC
    "GPS Set Time",
    "UTC Timezone"
#endif
};
#endif

const char *settings_radio_items[] =
{
    "Offset",
    "Direction",
    "Step",
};

const char * settings_m17_items[] =
{
    "Callsign",
    "Meta Txt",
    "CAN",
    "CAN RX Check"
};

const char* settings_fm_items[] =
{
    "CTCSS Tone",
    "CTCSS En."
};

const char *settings_frs_items[] =
{
    "FRS Mode",
    "Reset Codes"
};

#ifdef CONFIG_DMR
const char *settings_dmr_items[] =
{
    "DMR ID",
    "Talkgroup",
    "Call type",
    "Color code",
    "Timeslot",
    "Monitor",
    "Access",
    "Hang time"
};
#endif

const char * settings_accessibility_items[] =
{
    "Macro Latch",
    "Voice",
    "Phonetic"
};

const char *backup_restore_items[] =
{
    "Backup",
    "Restore"
};

const char *info_items[] =
{
    "",
    "Bat. Voltage",
    "Bat. Charge",
    "RSSI",
    "Used heap",
    "Band",
    "VHF",
    "UHF",
    "Hw Version",
#ifdef PLATFORM_TTWRPLUS
    "Radio",
    "Radio FW",
#endif
};

const char *authors[] =
{
    "Niccolo' IU2KIN",
    "Silvano IU2KWO",
    "Federico IU2NUO",
    "Fred IU2NRO",
    "Joseph VK7JS",
    "Morgan ON4MOD",
    "Marco DM4RCO"
};

static const char *symbols_ITU_T_E161[] =
{
    " 0",
    ",.?1",
    "abc2ABC",
    "def3DEF",
    "ghi4GHI",
    "jkl5JKL",
    "mno6MNO",
    "pqrs7PQRS",
    "tuv8TUV",
    "wxyz9WXYZ",
    "-/*",
    "#"
};

static const char *symbols_ITU_T_E161_callsign[] =
{
    "0 ",
    "1",
    "ABC2",
    "DEF3",
    "GHI4",
    "JKL5",
    "MNO6",
    "PQRS7",
    "TUV8",
    "WXYZ9",
    "-/",
    ""
};

// Calculate number of menu entries
const uint8_t menu_num = sizeof(menu_items)/sizeof(menu_items[0]);
const uint8_t settings_num = sizeof(settings_items)/sizeof(settings_items[0]);
const uint8_t display_num = sizeof(display_items)/sizeof(display_items[0]);
#ifdef CONFIG_GPS
const uint8_t settings_gps_num = sizeof(settings_gps_items)/sizeof(settings_gps_items[0]);
#endif
const uint8_t settings_radio_num = sizeof(settings_radio_items)/sizeof(settings_radio_items[0]);
#ifdef CONFIG_M17
const uint8_t settings_m17_num = sizeof(settings_m17_items)/sizeof(settings_m17_items[0]);
#endif
const uint8_t settings_fm_num = sizeof(settings_fm_items) / sizeof(settings_fm_items[0]);
const uint8_t settings_frs_num = sizeof(settings_frs_items) / sizeof(settings_frs_items[0]);
#ifdef CONFIG_DMR
const uint8_t settings_dmr_num = sizeof(settings_dmr_items) /
                                 sizeof(settings_dmr_items[0]);
#endif
const uint8_t settings_accessibility_num = sizeof(settings_accessibility_items)/sizeof(settings_accessibility_items[0]);
const uint8_t backup_restore_num = sizeof(backup_restore_items)/sizeof(backup_restore_items[0]);
const uint8_t info_num = sizeof(info_items)/sizeof(info_items[0]);
const uint8_t author_num = sizeof(authors)/sizeof(authors[0]);

const color_t color_black = {0, 0, 0, 255};
const color_t color_grey = {60, 60, 60, 255};
const color_t color_white = {255, 255, 255, 255};
const color_t yellow_fab413 = {250, 180, 19, 255};

layout_t layout;
state_t last_state;
bool macro_latched;
long long frs_refuse_tick = 0;
static ui_state_t ui_state;
static bool macro_menu = false;
static bool layout_ready = false;
static bool redraw_needed = true;

static bool standby = false;
static long long last_event_tick = 0;

// UI event queue
static uint8_t evQueue_rdPos;
static uint8_t evQueue_wrPos;
static event_t evQueue[MAX_NUM_EVENTS];


static void _ui_calculateLayout(layout_t *layout)
{
    // Horizontal line height
    static const uint16_t hline_h = 1;
    // Compensate for fonts printing below the start position
    static const uint16_t text_v_offset = 1;

    // Calculate UI layout depending on vertical resolution
    // Tytera MD380, MD-UV380
    #if CONFIG_SCREEN_HEIGHT > 127

    // Height and padding shown in diagram at beginning of file
    static const uint16_t top_h = 16;
    static const uint16_t top_pad = 4;
    static const uint16_t line1_h = 20;
    static const uint16_t line2_h = 20;
    static const uint16_t line3_h = 20;
    static const uint16_t line3_large_h = 40;
    static const uint16_t line4_h = 20;
    static const uint16_t menu_h = 16;
    static const uint16_t bottom_h = 23;
    static const uint16_t bottom_pad = top_pad;
    static const uint16_t status_v_pad = 2;
    static const uint16_t small_line_v_pad = 2;
    static const uint16_t big_line_v_pad = 6;
    static const uint16_t horizontal_pad = 4;

    // Top bar font: 8 pt
    static const fontSize_t   top_font = FONT_SIZE_8PT;
    static const symbolSize_t top_symbol_size = SYMBOLS_SIZE_8PT;
    // Text line font: 8 pt
    static const fontSize_t line1_font = FONT_SIZE_8PT;
    static const symbolSize_t line1_symbol_size = SYMBOLS_SIZE_8PT;
    static const fontSize_t line2_font = FONT_SIZE_8PT;
    static const symbolSize_t line2_symbol_size = SYMBOLS_SIZE_8PT;
    static const fontSize_t line3_font = FONT_SIZE_8PT;
    static const symbolSize_t line3_symbol_size = SYMBOLS_SIZE_8PT;
    static const fontSize_t line4_font = FONT_SIZE_8PT;
    static const symbolSize_t line4_symbol_size = SYMBOLS_SIZE_8PT;
    // Message font
    const fontSize_t message_font = FONT_SIZE_6PT;
    // Frequency line font: 16 pt
    static const fontSize_t line3_large_font = FONT_SIZE_16PT;
    // Bottom bar font: 8 pt
    static const fontSize_t bottom_font = FONT_SIZE_8PT;
    // TimeDate/Frequency input font
    static const fontSize_t input_font = FONT_SIZE_12PT;
    // Menu font
    static const fontSize_t menu_font = FONT_SIZE_8PT;

    // Radioddity GD-77
    #elif CONFIG_SCREEN_HEIGHT > 63

    // Height and padding shown in diagram at beginning of file
    static const uint16_t top_h = 11;
    static const uint16_t top_pad = 1;
    static const uint16_t line1_h = 10;
    static const uint16_t line2_h = 10;
    static const uint16_t line3_h = 10;
    static const uint16_t line3_large_h = 16;
    static const uint16_t line4_h = 10;
    static const uint16_t menu_h = 10;
    static const uint16_t bottom_h = 15;
    static const uint16_t bottom_pad = 0;
    static const uint16_t status_v_pad = 1;
    static const uint16_t small_line_v_pad = 1;
    static const uint16_t big_line_v_pad = 0;
    static const uint16_t horizontal_pad = 4;

    // Top bar font: 6 pt
    static const fontSize_t   top_font = FONT_SIZE_6PT;
    static const symbolSize_t top_symbol_size = SYMBOLS_SIZE_6PT;
    // Middle line fonts: 5, 8, 8 pt
    static const fontSize_t line1_font = FONT_SIZE_6PT;
    static const symbolSize_t line1_symbol_size = SYMBOLS_SIZE_6PT;
    static const fontSize_t line2_font = FONT_SIZE_6PT;
    static const symbolSize_t line2_symbol_size = SYMBOLS_SIZE_6PT;
    static const fontSize_t line3_font = FONT_SIZE_6PT;
    static const symbolSize_t line3_symbol_size = SYMBOLS_SIZE_6PT;
    static const fontSize_t line3_large_font = FONT_SIZE_10PT;
    static const fontSize_t line4_font = FONT_SIZE_6PT;
    static const symbolSize_t line4_symbol_size = SYMBOLS_SIZE_6PT;
    // Message font
    const fontSize_t message_font = FONT_SIZE_6PT;
    // Bottom bar font: 6 pt
    static const fontSize_t bottom_font = FONT_SIZE_6PT;
    // TimeDate/Frequency input font
    static const fontSize_t input_font = FONT_SIZE_8PT;
    // Menu font
    static const fontSize_t menu_font = FONT_SIZE_6PT;

    // Radioddity RD-5R
    #elif CONFIG_SCREEN_HEIGHT > 47

    // Height and padding shown in diagram at beginning of file
    static const uint16_t top_h = 11;
    static const uint16_t top_pad = 1;
    static const uint16_t line1_h = 0;
    static const uint16_t line2_h = 10;
    static const uint16_t line3_h = 10;
    static const uint16_t line3_large_h = 18;
    static const uint16_t line4_h = 10;
    static const uint16_t menu_h = 10;
    static const uint16_t bottom_h = 0;
    static const uint16_t bottom_pad = 0;
    static const uint16_t status_v_pad = 1;
    static const uint16_t small_line_v_pad = 1;
    static const uint16_t big_line_v_pad = 0;
    static const uint16_t horizontal_pad = 4;

    // Top bar font: 6 pt
    static const fontSize_t   top_font = FONT_SIZE_6PT;
    static const symbolSize_t top_symbol_size = SYMBOLS_SIZE_6PT;
    // Middle line fonts: 16, 16
    static const fontSize_t line2_font = FONT_SIZE_6PT;
    static const fontSize_t line3_font = FONT_SIZE_6PT;
    static const fontSize_t line4_font = FONT_SIZE_6PT;
    static const fontSize_t line3_large_font = FONT_SIZE_12PT;
    // TimeDate/Frequency input font
    static const fontSize_t input_font = FONT_SIZE_8PT;
    // Menu font
    static const fontSize_t menu_font = FONT_SIZE_6PT;
    // Message font
    const fontSize_t message_font = FONT_SIZE_6PT;
    // Not present on this resolution
    static const fontSize_t line1_font = 0;
    static const fontSize_t bottom_font = 0;

    #else
    #error Unsupported vertical resolution!
    #endif

    // Calculate printing positions
    static const uint16_t top_pos   = top_h - status_v_pad - text_v_offset;
    static const uint16_t line1_pos = top_h + top_pad + line1_h - small_line_v_pad - text_v_offset;
    static const uint16_t line2_pos = top_h + top_pad + line1_h + line2_h - small_line_v_pad - text_v_offset;
    static const uint16_t line3_pos = top_h + top_pad + line1_h + line2_h + line3_h - small_line_v_pad - text_v_offset;
    static const uint16_t line4_pos = top_h + top_pad + line1_h + line2_h + line3_h + line4_h - small_line_v_pad - text_v_offset;
    static const uint16_t line3_large_pos = top_h + top_pad + line1_h + line2_h + line3_large_h - big_line_v_pad - text_v_offset;
    static const uint16_t bottom_pos = CONFIG_SCREEN_HEIGHT - bottom_pad - status_v_pad - text_v_offset;

    layout_t new_layout =
    {
        hline_h,
        top_h,
        line1_h,
        line2_h,
        line3_h,
        line3_large_h,
        line4_h,
        menu_h,
        bottom_h,
        bottom_pad,
        status_v_pad,
        horizontal_pad,
        text_v_offset,
        {horizontal_pad, top_pos},
        {horizontal_pad, line1_pos},
        {horizontal_pad, line2_pos},
        {horizontal_pad, line3_pos},
        {horizontal_pad, line3_large_pos},
        {horizontal_pad, line4_pos},
        {horizontal_pad, bottom_pos},
        top_font,
        top_symbol_size,
        line1_font,
        line1_symbol_size,
        line2_font,
        line2_symbol_size,
        line3_font,
        line3_symbol_size,
        line3_large_font,
        line4_font,
        line4_symbol_size,
        bottom_font,
        input_font,
        menu_font,
        message_font
    };

    memcpy(layout, &new_layout, sizeof(layout_t));
}

static void _ui_drawLowBatteryScreen()
{
    gfx_clearScreen();
    uint16_t bat_width = CONFIG_SCREEN_WIDTH / 2;
    uint16_t bat_height = CONFIG_SCREEN_HEIGHT / 3;
    point_t bat_pos = {CONFIG_SCREEN_WIDTH / 4, CONFIG_SCREEN_HEIGHT / 8};
    gfx_drawBattery(bat_pos, bat_width, bat_height, 10);
    point_t text_pos_1 = {0, CONFIG_SCREEN_HEIGHT * 2 / 3};
    point_t text_pos_2 = {0, CONFIG_SCREEN_HEIGHT * 2 / 3 + 16};

    gfx_print(text_pos_1,
              FONT_SIZE_6PT,
              TEXT_ALIGN_CENTER,
              color_white,
              currentLanguage->forEmergencyUse);
    gfx_print(text_pos_2,
              FONT_SIZE_6PT,
              TEXT_ALIGN_CENTER,
              color_white,
              currentLanguage->pressAnyButton);
}

static freq_t _ui_freq_add_digit(freq_t freq, uint8_t pos, uint8_t number)
{
    freq_t coefficient = 100;
    for(uint8_t i=0; i < FREQ_DIGITS - pos; i++)
    {
        coefficient *= 10;
    }
    return freq += number * coefficient;
}

#ifdef CONFIG_RTC
static void _ui_timedate_add_digit(datetime_t *timedate, uint8_t pos,
                                   uint8_t number)
{
    vp_flush();
    vp_queueInteger(number);
    if (pos == 2 || pos == 4)
        vp_queuePrompt(PROMPT_SLASH);
    // just indicates separation of date and time.
    if (pos==6) // start of time.
        vp_queueString("hh:mm", vpAnnounceCommonSymbols|vpAnnounceLessCommonSymbols);
    if (pos == 8)
        vp_queuePrompt(PROMPT_COLON);
    vp_play();

    switch(pos)
    {
        // Set date
        case 1:
            timedate->date += number * 10;
            break;
        case 2:
            timedate->date += number;
            break;
        // Set month
        case 3:
            timedate->month += number * 10;
            break;
        case 4:
            timedate->month += number;
            break;
        // Set year
        case 5:
            timedate->year += number * 10;
            break;
        case 6:
            timedate->year += number;
            break;
        // Set hour
        case 7:
            timedate->hour += number * 10;
            break;
        case 8:
            timedate->hour += number;
            break;
        // Set minute
        case 9:
            timedate->minute += number * 10;
            break;
        case 10:
            timedate->minute += number;
            break;
    }
}
#endif

static bool _ui_freq_check_limits(freq_t freq)
{
    bool valid = false;
    const hwInfo_t* hwinfo = platform_getHwInfo();
    if(hwinfo->vhf_band)
    {
        // hwInfo_t frequencies are in MHz
        if(freq >= (hwinfo->vhf_minFreq * 1000000) &&
           freq <= (hwinfo->vhf_maxFreq * 1000000))
        valid = true;
    }
    if(hwinfo->uhf_band)
    {
        // hwInfo_t frequencies are in MHz
        if(freq >= (hwinfo->uhf_minFreq * 1000000) &&
           freq <= (hwinfo->uhf_maxFreq * 1000000))
        valid = true;
    }
    return valid;
}

static bool _ui_channel_valid(channel_t* channel)
{
return _ui_freq_check_limits(channel->rx_frequency) &&
       _ui_freq_check_limits(channel->tx_frequency);
}

static bool _ui_drawDarkOverlay()
{
    color_t alpha_grey = {0, 0, 0, 255};
    point_t origin = {0, 0};
    gfx_drawRect(origin, CONFIG_SCREEN_WIDTH, CONFIG_SCREEN_HEIGHT, alpha_grey, true);
    return true;
}

static int _ui_fsm_loadChannel(int16_t channel_index, bool *sync_rtx)
{
    channel_t channel;
    int32_t selected_channel = channel_index;
    // If a bank is active, get index from current bank
    if(state.bank_enabled)
    {
        bankHdr_t bank = { 0 };
        cps_readBankHeader(&bank, state.bank);
        if((channel_index < 0) || (channel_index >= bank.ch_count))
            return -1;
        channel_index = cps_readBankData(state.bank, channel_index);
    }

    int result = cps_readChannel(&channel, channel_index);
    // Read successful and channel is valid
    if((result != -1) && _ui_channel_valid(&channel))
    {
        // Set new channel index
        state.channel_index = selected_channel;
        // Copy channel read to state
        state.channel = channel;
        *sync_rtx = true;
    }

    return result;
}

static void _ui_fsm_confirmVFOInput(bool *sync_rtx)
{
    vp_flush();
    // Switch to TX input
    if(ui_state.input_set == SET_RX)
    {
        ui_state.input_set = SET_TX;
        // Reset input position
        ui_state.input_position = 0;
        // announce the rx frequency just confirmed with Enter.
        vp_queueFrequency(ui_state.new_rx_frequency);
        // defer playing till the end.
        // indicate that the user has moved to the tx freq field.
        vp_announceInputReceiveOrTransmit(true, vpqDefault);
    }
    else if(ui_state.input_set == SET_TX)
    {
        // Save new frequency setting
        // If TX frequency was not set, TX = RX
        if(ui_state.new_tx_frequency == 0)
        {
            ui_state.new_tx_frequency = ui_state.new_rx_frequency;
        }
        // Apply new frequencies if they are valid
        if(_ui_freq_check_limits(ui_state.new_rx_frequency) &&
           _ui_freq_check_limits(ui_state.new_tx_frequency))
        {
            state.channel.rx_frequency = ui_state.new_rx_frequency;
            state.channel.tx_frequency = ui_state.new_tx_frequency;
            *sync_rtx = true;
            // force init to clear any prompts in progress.
            // defer play because play is called at the end of the function
            //due to above freq queuing.
            vp_announceFrequencies(state.channel.rx_frequency,
                                   state.channel.tx_frequency, vpqInit);
        }
        else
        {
            vp_announceError(vpqInit);
        }

        state.ui_screen = MAIN_VFO;
    }

    vp_play();
}

static void _ui_fsm_insertVFONumber(kbd_msg_t msg, bool *sync_rtx)
{
    // Advance input position
    ui_state.input_position += 1;
    // clear any prompts in progress.
    vp_flush();
    // Save pressed number to calculate frequency and show in GUI
    ui_state.input_number = input_getPressedNumber(msg);
    // queue the digit just pressed.
    vp_queueInteger(ui_state.input_number);
    // queue  point if user has entered three digits.
    if (ui_state.input_position == 3)
        vp_queuePrompt(PROMPT_POINT);

    if(ui_state.input_set == SET_RX)
    {
        if(ui_state.input_position == 1)
            ui_state.new_rx_frequency = 0;
        // Calculate portion of the new RX frequency
        ui_state.new_rx_frequency = _ui_freq_add_digit(ui_state.new_rx_frequency,
                                                       ui_state.input_position,
                                                       ui_state.input_number);
        if(ui_state.input_position >= FREQ_DIGITS)
        {// queue the rx freq just completed.
            vp_queueFrequency(ui_state.new_rx_frequency);
            /// now queue tx as user has changed fields.
            vp_queuePrompt(PROMPT_TRANSMIT);
            // Switch to TX input
            ui_state.input_set = SET_TX;
            // Reset input position
            ui_state.input_position = 0;
            // Reset TX frequency
            ui_state.new_tx_frequency = 0;
        }
    }
    else if(ui_state.input_set == SET_TX)
    {
        if(ui_state.input_position == 1)
            ui_state.new_tx_frequency = 0;
        // Calculate portion of the new TX frequency
        ui_state.new_tx_frequency = _ui_freq_add_digit(ui_state.new_tx_frequency,
                                                       ui_state.input_position,
                                                       ui_state.input_number);
        if(ui_state.input_position >= FREQ_DIGITS)
        {
            // Save both inserted frequencies
            if(_ui_freq_check_limits(ui_state.new_rx_frequency) &&
               _ui_freq_check_limits(ui_state.new_tx_frequency))
            {
                state.channel.rx_frequency = ui_state.new_rx_frequency;
                state.channel.tx_frequency = ui_state.new_tx_frequency;
                *sync_rtx = true;
                // play is called at end.
                vp_announceFrequencies(state.channel.rx_frequency,
                                       state.channel.tx_frequency, vpqInit);
            }

            state.ui_screen = MAIN_VFO;
        }
    }

    vp_play();
}

#ifdef CONFIG_SCREEN_BRIGHTNESS
static void _ui_changeBrightness(int variation)
{
    state.settings.brightness += variation;

    // Max value for brightness is 100, min value is set to 5 to avoid complete
    //  display shutdown.
    if(state.settings.brightness > 100) state.settings.brightness = 100;
    if(state.settings.brightness < 5)   state.settings.brightness = 5;

    display_setBacklightLevel(state.settings.brightness);
}
#endif

#ifdef CONFIG_SCREEN_CONTRAST
static void _ui_changeContrast(int variation)
{
    if(variation >= 0)
        state.settings.contrast =
        (255 - state.settings.contrast < variation) ? 255 : state.settings.contrast + variation;
    else
        state.settings.contrast =
        (state.settings.contrast < -variation) ? 0 : state.settings.contrast + variation;

    display_setContrast(state.settings.contrast);
}
#endif

static void _ui_changeTimer(int variation)
{
    if ((state.settings.display_timer == TIMER_OFF && variation < 0) ||
        (state.settings.display_timer == TIMER_1H && variation > 0))
    {
        return;
    }

    state.settings.display_timer += variation;
}

static void _ui_changeMacroLatch(bool newVal)
{
    state.settings.macroMenuLatch = newVal ? 1 : 0;
    vp_announceSettingsOnOffToggle(&currentLanguage->macroLatching,
                                   vp_getVoiceLevelQueueFlags(),
                                   state.settings.macroMenuLatch);
}

#ifdef CONFIG_M17
static inline void _ui_changeM17Can(int variation)
{
    uint8_t can = state.settings.m17_can;
    state.settings.m17_can = (can + variation) % 16;
}
#endif

static void _ui_changeVoiceLevel(int variation)
{
    if ((state.settings.vpLevel == vpNone && variation < 0) ||
        (state.settings.vpLevel == vpHigh && variation > 0))
        {
            return;
        }

    state.settings.vpLevel += variation;

    // Force these flags to ensure the changes are spoken for levels 1 through 3.
    enum vpQueueFlags flags = vpqInit
                         | vpqAddSeparatingSilence
                         | vpqPlayImmediately;

    if (!vp_isPlaying())
    {
        flags |= vpqIncludeDescriptions;
    }

    vp_announceSettingsVoiceLevel(flags);
}

static void _ui_changePhoneticSpell(bool newVal)
{
    state.settings.vpPhoneticSpell = newVal ? 1 : 0;

    vp_announceSettingsOnOffToggle(&currentLanguage->phonetic,
                                   vp_getVoiceLevelQueueFlags(),
                                   state.settings.vpPhoneticSpell);
}

/*
 * FRS mode helpers.
 *
 * While state.settings.frs_mode is set, state.channel holds the FRS channel
 * built from the current FRS channel number and its privacy code, and the
 * VFO the user had before is parked in state.vfo_channel. Every change of
 * channel or code goes through _ui_frs_apply(), which rebuilds the channel
 * and requests an RTX synchronisation.
 */
static void _ui_frs_apply(bool *sync_rtx)
{
    uint8_t channel = state.settings.frs_channel;

    state.channel = frs_buildChannel(channel,
                                     state.settings.frs_codes[channel]);
    *sync_rtx = true;
}

/*
 * Feedback for a key that is disabled while FRS mode is active: "FRS, error"
 * with voice prompts on, a short low beep otherwise. The beep is silent at
 * the default voice prompt level (vpNone), so the refusal is also shown as a
 * "FRS!" marker in the top bar for FRS_REFUSE_MARKER_TIME, see ui_updateGUI().
 */
static void _ui_frs_refuse()
{
    frs_refuse_tick = getTick();

    if (state.settings.vpLevel > vpBeep)
    {
        vp_announceText(currentLanguage->frs, vpqInit);
        vp_announceError(vpqPlayImmediately);
    }
    else
    {
        vp_beep(BEEP_FUNCTION_LATCH_OFF, SHORT_BEEP);
    }
}

static void _ui_frs_toggle(bool *sync_rtx)
{
    enum vpQueueFlags queueFlags = vp_getVoiceLevelQueueFlags();

    if (state.settings.frs_mode == 0)
    {
        // The UHF band must cover 462-468 MHz
        if (frs_isSupported(platform_getHwInfo()) == false)
        {
            _ui_frs_refuse();
            return;
        }

        // Park the VFO: when the menu was entered from MEM mode the VFO
        // has already been saved in state.vfo_channel.
        if (ui_state.last_main_state == MAIN_VFO)
            state.vfo_channel = state.channel;

        state.settings.frs_mode = 1;
        _ui_frs_apply(sync_rtx);
        ui_state.last_main_state = MAIN_FRS;
    }
    else
    {
        state.settings.frs_mode = 0;
        state.channel = state.vfo_channel;
        *sync_rtx = true;
        ui_state.last_main_state = MAIN_VFO;
    }

    vp_announceSettingsOnOffToggle(&currentLanguage->frsMode, queueFlags,
                                   state.settings.frs_mode);
}

static void _ui_frs_setChannel(uint8_t channel, bool *sync_rtx)
{
    state.settings.frs_channel = channel;
    _ui_frs_apply(sync_rtx);

    // The channel name is "FRS n": spoken as "channel n, F R S n"
    vp_announceChannelName(&state.channel, channel + 1,
                           vp_getVoiceLevelQueueFlags());
}

/*
 * Digit typed on the FRS screens. Channels 3-9 are selected at once, since
 * no two-digit channel starts with them; 1 and 2 are kept pending on the
 * MAIN_FRS_INPUT screen until a second digit, ENTER or the entry timeout.
 * A second digit that does not complete a channel ("2 5") restarts the entry
 * with that digit, which then follows the same rules: "2 5" lands on channel
 * 5 at once.
 */
static void _ui_frs_inputDigit(uint8_t digit, bool *sync_rtx)
{
    if (state.ui_screen == MAIN_FRS_INPUT)
    {
        uint8_t channel = ui_state.input_number * 10 + digit;
        if ((channel >= 1) && (channel <= FRS_CHANNEL_NUM))
        {
            _ui_frs_setChannel(channel - 1, sync_rtx);
            state.ui_screen = MAIN_FRS;
            return;
        }

        // Not a channel: restart the entry with this digit
    }

    if (digit == 0)
    {
        _ui_frs_refuse();
        return;
    }

    vp_announceInputChar('0' + digit);

    if (digit >= 3)
    {
        _ui_frs_setChannel(digit - 1, sync_rtx);
        state.ui_screen = MAIN_FRS;
        return;
    }

    ui_state.input_number = digit;
    ui_state.input_position = 1;
    ui_state.last_keypress = getTick();
    state.ui_screen = MAIN_FRS_INPUT;
}

static void _ui_frs_setCode(uint8_t code, bool *sync_rtx)
{
    uint8_t channel = state.settings.frs_channel;

    state.settings.frs_codes[channel] = code;
    _ui_frs_apply(sync_rtx);

    // "Code 12, tone 100.0 hertz" or "Code off"
    vp_announceText(currentLanguage->code, vpqInit);
    if (code == 0)
        vp_queueStringTableEntry(&currentLanguage->off);
    else
        vp_queueInteger(code);
    vp_announceCTCSS(state.channel.fm.rxToneEn, state.channel.fm.rxTone,
                     state.channel.fm.txToneEn, state.channel.fm.txTone,
                     vpqIncludeDescriptions | vpqPlayImmediately);
}

/*
 * Open the privacy code picker with the current code highlighted, so that a
 * neighbouring code is one key away.
 */
static void _ui_frs_openCodePicker()
{
    ui_state.menu_selected = state.settings.frs_codes[state.settings.frs_channel];
    ui_state.input_number = 0;
    ui_state.input_position = 0;
    state.ui_screen = FRS_CODE;
}

/*
 * Digit typed in the privacy code picker: each digit refines the highlight,
 * "1 2" lands on code 12 and "5" on code 5. A pending first digit is kept
 * only when a two-digit code can still start with it (1..3).
 */
static void _ui_frs_codeDigit(uint8_t digit)
{
    uint8_t code = ui_state.input_number * 10 + digit;

    if ((ui_state.input_position == 1) && (code <= FRS_CODE_NUM))
    {
        ui_state.menu_selected = code;
        ui_state.input_position = 0;
    }
    else
    {
        ui_state.menu_selected = digit;
        ui_state.input_number = digit;
        ui_state.input_position = ((digit >= 1) && (digit <= 3)) ? 1 : 0;
    }
}

static void _ui_frs_resetCodes(bool *sync_rtx)
{
    memset(state.settings.frs_codes, 0, sizeof(state.settings.frs_codes));
    if (state.settings.frs_mode != 0)
        _ui_frs_apply(sync_rtx);

    vp_announceText(currentLanguage->resetCodes, vp_getVoiceLevelQueueFlags());
}

#ifdef CONFIG_DMR
/*
 * DMR mode helpers.
 *
 * The identity (DMR ID), the destination (talkgroup or private ID, with its
 * call type) and the channel access policy live in the settings, as the M17
 * destination does. The colour code and timeslot of the VFO live in
 * state.channel.dmr and are seeded from the settings when the VFO is switched
 * to DMR, since channel.dmr shares its storage with the FM and M17 channel
 * data. Settings > DMR edits both, so that a fresh VFO keeps the last values.
 */
static void _ui_dmr_seedChannel()
{
    state.channel.dmr.rxColorCode   = state.settings.dmr_colorCode;
    state.channel.dmr.txColorCode   = state.settings.dmr_colorCode;
    state.channel.dmr.dmr_timeslot  = state.settings.dmr_timeslot;
    state.channel.dmr.contact_index = 0;
}

/*
 * The DMR block of the channel overlays the FM tone settings: seeding it
 * (and the macro keys editing it) would leave the FM VFO tone squelched
 * once the mode key comes back to FM. The FM block is therefore saved when
 * the mode key leaves FM for DMR and put back when the cycle lands on FM
 * again, be it directly or through M17. A VFO that was already in DMR when
 * the key was first pressed (restored from NVM or from the codeplug) has
 * nothing to put back: its FM block is reset to "no tones" instead.
 */
static fmInfo_t dmr_savedFm;
static bool dmr_savedFmValid = false;
static bool dmr_fmDirty = false;

static void _ui_dmr_enterFromFm()
{
    dmr_savedFm = state.channel.fm;
    dmr_savedFmValid = true;
    state.channel.mode = OPMODE_DMR;
    _ui_dmr_seedChannel();
}

static void _ui_dmr_leave()
{
    dmr_fmDirty = true;
}

static void _ui_dmr_restoreFm()
{
    if(dmr_savedFmValid)
        state.channel.fm = dmr_savedFm;
    else if(dmr_fmDirty)
        memset(&state.channel.fm, 0, sizeof(state.channel.fm));

    dmr_savedFmValid = false;
    dmr_fmDirty = false;
}

static void _ui_dmr_changeColorCode(int variation)
{
    uint8_t colorCode = (state.settings.dmr_colorCode + 16 + variation) % 16;

    state.settings.dmr_colorCode = colorCode;
    if(state.channel.mode == OPMODE_DMR)
    {
        state.channel.dmr.rxColorCode = colorCode;
        state.channel.dmr.txColorCode = colorCode;
    }
}

static void _ui_dmr_toggleTimeslot()
{
    uint8_t timeslot = (state.settings.dmr_timeslot == 1) ? 2 : 1;

    state.settings.dmr_timeslot = timeslot;
    if(state.channel.mode == OPMODE_DMR)
        state.channel.dmr.dmr_timeslot = timeslot;
}

static void _ui_dmr_changeCallType(int variation)
{
    uint8_t callType = (state.settings.dmr_callType + 3 + variation) % 3;

    state.settings.dmr_callType = callType;
}

static void _ui_dmr_changeMonitor(int variation)
{
    uint8_t monitor = (state.settings.dmr_monitor + 3 + variation) % 3;

    state.settings.dmr_monitor = monitor;
}

static void _ui_dmr_changeHangTime(int variation)
{
    int hangTime = state.settings.dmr_hangTime + variation;

    if(hangTime < 0) hangTime = 0;
    if(hangTime > 7) hangTime = 7;
    state.settings.dmr_hangTime = hangTime;
}

/*
 * "Group 31665", "Private 2345678" or "ALL": the current destination.
 */
static void _ui_dmr_announceDestination(enum vpQueueFlags queueFlags)
{
    switch(state.settings.dmr_callType)
    {
        case GROUP:
            vp_announceText(currentLanguage->group, vpqInit);
            vp_queueInteger(state.settings.dmr_talkgroup);
            break;
        case PRIVATE:
            vp_announceText(currentLanguage->privateCall, vpqInit);
            vp_queueInteger(state.settings.dmr_talkgroup);
            break;
        default:
            vp_announceText(currentLanguage->broadcast, vpqInit);
            break;
    }

    if((queueFlags & vpqPlayImmediately) ||
       ((queueFlags & vpqPlayImmediatelyAtMediumOrHigher) &&
        (state.settings.vpLevel >= vpMedium)))
        vp_play();
}

/*
 * Numeric entry of a DMR ID or talkgroup, shared by the main screens ('#')
 * and by Settings > DMR. A digit that would take the number past DMR_ID_MAX,
 * or past the eight digits it can span, is ignored; "0" is a valid entry and
 * means "unset" for the DMR ID. ENTER with no digit typed keeps the old value.
 */
static void _ui_dmr_numberReset()
{
    ui_state.new_dmr_number = 0;
    ui_state.new_dmr_digits = 0;
}

static void _ui_dmr_numberDigit(uint8_t digit)
{
    uint32_t number = (ui_state.new_dmr_number * 10) + digit;

    if((ui_state.new_dmr_digits >= DMR_ID_DIGITS) || (number > DMR_ID_MAX))
        return;

    ui_state.new_dmr_number = number;
    ui_state.new_dmr_digits++;
    vp_announceInputChar('0' + digit);
}

static void _ui_dmr_numberDel()
{
    if(ui_state.new_dmr_digits == 0)
        return;

    ui_state.new_dmr_number /= 10;
    ui_state.new_dmr_digits--;
}

/*
 * Keys of the number entry: ENTER accepts the number typed and leaves the
 * entry, ESC leaves it without a change, the arrows delete the last digit.
 * Returns true when the entry has been left; *accepted is set when a number
 * was confirmed, the caller then finds it in ui_state.new_dmr_number. The
 * caller stores it itself: settings_t is packed, so no pointer to one of its
 * fields is taken here.
 */
static bool _ui_dmr_numberInput(kbd_msg_t msg, bool *accepted)
{
    *accepted = false;

    if(msg.keys & KEY_ENTER)
    {
        *accepted = (ui_state.new_dmr_digits != 0);
        return true;
    }
    else if(msg.keys & KEY_ESC)
    {
        return true;
    }
    else if(msg.keys & KEY_UP || msg.keys & KEY_DOWN ||
            msg.keys & KEY_LEFT || msg.keys & KEY_RIGHT)
    {
        _ui_dmr_numberDel();
    }
    else if(input_isNumberPressed(msg))
    {
        _ui_dmr_numberDigit(input_getPressedNumber(msg));
    }

    return false;
}

/*
 * Talkgroup or private ID entry opened with '#' on the main screens. '#'
 * pressed again cancels the entry, as ESC does.
 */
static void _ui_dmr_destinationInput(kbd_msg_t msg, bool *sync_rtx,
                                     enum vpQueueFlags queueFlags)
{
    if(msg.keys & KEY_HASH)
    {
        ui_state.edit_mode = false;
        return;
    }

    bool accepted = false;
    if(_ui_dmr_numberInput(msg, &accepted))
    {
        if(accepted &&
           (ui_state.new_dmr_number != state.settings.dmr_talkgroup))
        {
            state.settings.dmr_talkgroup = ui_state.new_dmr_number;
            *sync_rtx = true;
        }

        ui_state.edit_mode = false;
        _ui_dmr_announceDestination(queueFlags);
    }
}

static void _ui_dmr_openDestinationInput(enum vpQueueFlags queueFlags)
{
    ui_state.edit_mode = true;
    _ui_dmr_numberReset();
    vp_announceText(currentLanguage->talkgroup, queueFlags);
}

static void _ui_dmr_toggleCallType(bool *sync_rtx, enum vpQueueFlags queueFlags)
{
    // A broadcast destination set in the menu goes back to a group call
    state.settings.dmr_callType = (state.settings.dmr_callType == GROUP)
                                ? PRIVATE : GROUP;
    *sync_rtx = true;
    _ui_dmr_announceDestination(queueFlags);
}
#endif

/*
 * Macro key 5: FM -> DMR -> M17 -> FM, skipping the modes the radio does not
 * support. A VFO switched to DMR gets its colour code and timeslot from the
 * settings and the FM tone settings are kept aside until the cycle is back
 * to FM, see the DMR helpers above.
 */
static void _ui_cycleOpMode()
{
    switch(state.channel.mode)
    {
        case OPMODE_FM:
            #if defined(CONFIG_DMR)
            _ui_dmr_enterFromFm();
            #elif defined(CONFIG_M17)
            state.channel.mode = OPMODE_M17;
            #endif
            break;

        #ifdef CONFIG_DMR
        case OPMODE_DMR:
            _ui_dmr_leave();
            #ifdef CONFIG_M17
            state.channel.mode = OPMODE_M17;
            #else
            state.channel.mode = OPMODE_FM;
            _ui_dmr_restoreFm();
            #endif
            break;
        #endif

        default:
            // M17, or an invalid mode: never lock the user out
            state.channel.mode = OPMODE_FM;
            #ifdef CONFIG_DMR
            _ui_dmr_restoreFm();
            #endif
            break;
    }
}

bool _ui_checkStandby(long long time_since_last_event)
{
    if (standby)
    {
        return false;
    }

    switch (state.settings.display_timer)
    {
        case TIMER_OFF:
            return false;
        case TIMER_5S:
        case TIMER_10S:
        case TIMER_15S:
        case TIMER_20S:
        case TIMER_25S:
        case TIMER_30S:
            return time_since_last_event >= (5000 * state.settings.display_timer);
        case TIMER_1M:
        case TIMER_2M:
        case TIMER_3M:
        case TIMER_4M:
        case TIMER_5M:
            return time_since_last_event >=
                (60000 * (state.settings.display_timer - (TIMER_1M - 1)));
        case TIMER_15M:
        case TIMER_30M:
        case TIMER_45M:
            return time_since_last_event >=
                (60000 * 15 * (state.settings.display_timer - (TIMER_15M - 1)));
        case TIMER_1H:
            return time_since_last_event >= 60 * 60 * 1000;
    }

    // unreachable code
    return false;
}

static void _ui_enterStandby()
{
    if(standby)
        return;

    standby = true;
    redraw_needed = false;
    display_setBacklightLevel(0);
}

static bool _ui_exitStandby(long long now)
{
    last_event_tick = now;

    if(!standby)
        return false;

    standby = false;
    redraw_needed = true;
    display_setBacklightLevel(state.settings.brightness);

    return true;
}

// TODO: find a better home for this function
int _ui_handleToneSelectScroll(bool direction_up)
{
    bool tone_tx_enable = state.channel.fm.txToneEn;
    bool tone_rx_enable = state.channel.fm.rxToneEn;
    uint8_t tone_flags = tone_tx_enable << 1 | tone_rx_enable;

    if(direction_up)
        tone_flags++;
    else
        tone_flags--;

    tone_flags %= 4;
    tone_tx_enable = tone_flags >> 1;
    tone_rx_enable = tone_flags & 1;
    state.channel.fm.txToneEn = tone_tx_enable;
    state.channel.fm.rxToneEn = tone_rx_enable;

    return 1;
}

static void _ui_fsm_menuMacro(kbd_msg_t msg, bool *sync_rtx)
{
    // If there is no keyboard left and right select the menu entry to edit
#if defined(CONFIG_UI_NO_KEYBOARD)
    if (msg.keys & KNOB_LEFT)
    {
        ui_state.macro_menu_selected--;
        ui_state.macro_menu_selected += 9;
        ui_state.macro_menu_selected %= 9;
    }
    if (msg.keys & KNOB_RIGHT)
    {
        ui_state.macro_menu_selected++;
        ui_state.macro_menu_selected %= 9;
    }
    if ((msg.keys & KEY_ENTER) && !msg.long_press)
        ui_state.input_number = ui_state.macro_menu_selected + 1;
    else
        ui_state.input_number = 0;
#else // CONFIG_UI_NO_KEYBOARD
    ui_state.input_number = input_getPressedNumber(msg);
#endif // CONFIG_UI_NO_KEYBOARD
    // CTCSS Encode/Decode Selection
    enum vpQueueFlags queueFlags = vp_getVoiceLevelQueueFlags();

    // FRS mode: keys 2 and 3 step the privacy code of the current channel
    // in place of the raw CTCSS tone; tone mode (1), bandwidth (4), opmode
    // (5) and power (6) are fixed by the FRS channel plan.
    if(state.settings.frs_mode != 0)
    {
        uint8_t code = state.settings.frs_codes[state.settings.frs_channel];

        switch(ui_state.input_number)
        {
            case 2:
                _ui_frs_setCode((code == 0) ? FRS_CODE_NUM : code - 1, sync_rtx);
                ui_state.input_number = 0;
                break;
            case 3:
                _ui_frs_setCode((code >= FRS_CODE_NUM) ? 0 : code + 1, sync_rtx);
                ui_state.input_number = 0;
                break;
            case 1:
            case 4:
            case 5:
            case 6:
                _ui_frs_refuse();
                ui_state.input_number = 0;
                break;
        }
    }

    switch(ui_state.input_number)
    {
        case 1:
            if(state.channel.mode == OPMODE_FM)
            {
                _ui_handleToneSelectScroll(true);
                *sync_rtx                 = true;
                vp_announceCTCSS(
                    state.channel.fm.rxToneEn, state.channel.fm.rxTone,
                    state.channel.fm.txToneEn, state.channel.fm.txTone,
                    queueFlags | vpqIncludeDescriptions);
            }
            #ifdef CONFIG_DMR
            else if(state.channel.mode == OPMODE_DMR)
            {
                // Next colour code
                _ui_dmr_changeColorCode(+1);
                *sync_rtx = true;
                vp_announceColorCode(state.channel.dmr.rxColorCode,
                                     state.channel.dmr.txColorCode,
                                     queueFlags | vpqIncludeDescriptions);
            }
            #endif
            break;
        case 2:
            if (state.channel.mode == OPMODE_FM)
            {
                if (state.channel.fm.txTone == 0)
                {
                    state.channel.fm.txTone = CTCSS_FREQ_NUM-1;
                }
                else
                {
                    state.channel.fm.txTone--;
                }

                state.channel.fm.txTone %= CTCSS_FREQ_NUM;
                state.channel.fm.rxTone = state.channel.fm.txTone;
                *sync_rtx = true;
                vp_announceCTCSS(state.channel.fm.rxToneEn,
                                 state.channel.fm.rxTone,
                                 state.channel.fm.txToneEn,
                                 state.channel.fm.txTone,
                                 queueFlags);
            }
            #ifdef CONFIG_DMR
            else if(state.channel.mode == OPMODE_DMR)
            {
                // Other timeslot
                _ui_dmr_toggleTimeslot();
                *sync_rtx = true;
                vp_announceTimeslot(state.channel.dmr.dmr_timeslot,
                                    queueFlags | vpqIncludeDescriptions);
            }
            #endif
            break;

        case 3:
            if(state.channel.mode == OPMODE_FM)
            {
                state.channel.fm.txTone++;
                state.channel.fm.txTone %= CTCSS_FREQ_NUM;
                state.channel.fm.rxTone = state.channel.fm.txTone;
                *sync_rtx = true;
                vp_announceCTCSS(state.channel.fm.rxToneEn,
                                 state.channel.fm.rxTone,
                                 state.channel.fm.txToneEn,
                                 state.channel.fm.txTone,
                                 queueFlags |vpqIncludeDescriptions);
            }
            #ifdef CONFIG_DMR
            else if(state.channel.mode == OPMODE_DMR)
            {
                // Next monitor level: off, own colour code, any
                _ui_dmr_changeMonitor(+1);
                *sync_rtx = true;
                vp_announceSettingsInt(&currentLanguage->monitor, queueFlags,
                                       state.settings.dmr_monitor);
            }
            #endif
            break;
        case 4:
            if(state.channel.mode == OPMODE_FM)
            {
                state.channel.bandwidth++;
                state.channel.bandwidth %= 2;
                *sync_rtx = true;
                vp_announceBandwidth(state.channel.bandwidth, queueFlags);
            }
            break;
        case 5:
            // Cycle through radio modes
            _ui_cycleOpMode();
            *sync_rtx = true;
            vp_announceRadioMode(state.channel.mode, queueFlags);
            break;
        case 6:

            switch(state.channel.power)
            {
                case 1000:
                    state.channel.power = 2500;
                    break;

                case 2500:
                    state.channel.power = 5000;
                    break;

                default:
                    state.channel.power = 1000;
            }

            *sync_rtx = true;
            vp_announcePower(state.channel.power, queueFlags);
            break;
#ifdef CONFIG_SCREEN_BRIGHTNESS
        case 7:
            _ui_changeBrightness(-5);
            vp_announceSettingsInt(&currentLanguage->brightness, queueFlags,
                                   state.settings.brightness);
            break;
        case 8:
            _ui_changeBrightness(+5);
            vp_announceSettingsInt(&currentLanguage->brightness, queueFlags,
                                   state.settings.brightness);
            break;
#endif
        case 9:
            if (!ui_state.input_locked)
                ui_state.input_locked = true;
            else
                ui_state.input_locked = false;
            break;
    }

#if defined(PLATFORM_TTWRPLUS)
    if(msg.keys & KEY_VOLDOWN)
#else
    if(msg.keys & KEY_LEFT || msg.keys & KEY_DOWN || msg.keys & KNOB_LEFT)
#endif // PLATFORM_TTWRPLUS
    {
#ifdef CONFIG_KNOB_ABSOLUTE // If the radio has an absolute position knob
        state.settings.sqlLevel = platform_getChSelector() - 1;
#endif // CONFIG_KNOB_ABSOLUTE
        if(state.settings.sqlLevel > 0)
        {
            state.settings.sqlLevel -= 1;
            *sync_rtx = true;
            vp_announceSquelch(state.settings.sqlLevel, queueFlags);
        }
    }

#if defined(PLATFORM_TTWRPLUS)
    else if(msg.keys & KEY_VOLUP)
#else
    else if(msg.keys & KEY_RIGHT || msg.keys & KEY_UP || msg.keys & KNOB_RIGHT)
#endif // PLATFORM_TTWRPLUS
    {
#ifdef CONFIG_KNOB_ABSOLUTE
        state.settings.sqlLevel = platform_getChSelector() - 1;
#endif
        if(state.settings.sqlLevel < 15)
        {
            state.settings.sqlLevel += 1;
            *sync_rtx = true;
            vp_announceSquelch(state.settings.sqlLevel, queueFlags);
        }
    }
}

static void _ui_menuUp(uint8_t menu_entries)
{
    if(ui_state.menu_selected > 0)
        ui_state.menu_selected -= 1;
    else
        ui_state.menu_selected = menu_entries - 1;
    vp_playMenuBeepIfNeeded(ui_state.menu_selected==0);
}

static void _ui_menuDown(uint8_t menu_entries)
{
    if(ui_state.menu_selected < menu_entries - 1)
        ui_state.menu_selected += 1;
    else
        ui_state.menu_selected = 0;
    vp_playMenuBeepIfNeeded(ui_state.menu_selected==0);
}

static void _ui_menuBack(uint8_t prev_state)
{
    if(ui_state.edit_mode)
    {
        ui_state.edit_mode = false;
    }
    else
    {
        // Return to previous menu
        state.ui_screen = prev_state;
        // Reset menu selection
        ui_state.menu_selected = 0;
        vp_playMenuBeepIfNeeded(true);
    }
}

static void _ui_textInputReset(char *buf, size_t bufSize)
{
    ui_state.input_number = 0;
    ui_state.input_position = 0;
    ui_state.input_set = 0;
    ui_state.last_keypress = 0;
    memset(buf, 0, bufSize);
    buf[0] = '_';
}

static void _ui_textInputKeypad(char *buf, uint8_t max_len, kbd_msg_t msg,
                         bool callsign)
{
    long long now = getTick();
    // Get currently pressed number key
    uint8_t num_key = input_getPressedChar(msg);

    bool key_timeout = ((now - ui_state.last_keypress) >= input_longPressTimeout);
    bool same_key = ui_state.input_number == num_key;
    // Get number of symbols related to currently pressed key
    uint8_t num_symbols = 0;
    if(callsign)
    {
        num_symbols = strlen(symbols_ITU_T_E161_callsign[num_key]);
        if(num_symbols == 0)
            return;
    }
    else
        num_symbols = strlen(symbols_ITU_T_E161[num_key]);

    // Return if max length is reached or finished editing last character
    if((ui_state.input_position >= max_len) || ((ui_state.input_position == (max_len-1)) && (key_timeout || !same_key)))
        return;

    // Skip keypad logic for first keypress
    if(ui_state.last_keypress != 0)
    {
        // Same key pressed and timeout not expired: cycle over chars of current key
        if(same_key && !key_timeout)
        {
            ui_state.input_set = (ui_state.input_set + 1) % num_symbols;
        }
        // Different key pressed: save current char and change key
        else
        {
            ui_state.input_position += 1;
            ui_state.input_set = 0;
        }
    }
    // Show current character on buffer
    if(callsign)
        buf[ui_state.input_position] = symbols_ITU_T_E161_callsign[num_key][ui_state.input_set];
    else
    {
        buf[ui_state.input_position] = symbols_ITU_T_E161[num_key][ui_state.input_set];
    }
    // Announce the character
    vp_announceInputChar(buf[ui_state.input_position]);
    // Update reference values
    ui_state.input_number = num_key;
    ui_state.last_keypress = now;
}

static void _ui_textInputConfirm(char *buf)
{
    buf[ui_state.input_position + 1] = '\0';
}

static void _ui_textInputDel(char *buf)
{
    // announce the char about to be backspaced.
    // Note this assumes editing callsign.
    // If we edit a different buffer which allows the underline char, we may
    // not want to exclude it, but when editing callsign, we do not want to say
    // underline since it means the field is empty.
    if(buf[ui_state.input_position]
    && buf[ui_state.input_position]!='_')
        vp_announceInputChar(buf[ui_state.input_position]);

    buf[ui_state.input_position] = '\0';
    // Move back input cursor
    if(ui_state.input_position > 0)
    {
        ui_state.input_position--;
    // If we deleted the initial character, reset starting condition
    }
    else
        ui_state.last_keypress = 0;
    ui_state.input_set = 0;
}

static void _ui_numberInputKeypad(uint32_t *num, kbd_msg_t msg)
{
    long long now = getTick();

#ifdef CONFIG_UI_NO_KEYBOARD
    // If knob is turned, increment or Decrement
    if (msg.keys & KNOB_LEFT)
    {
        *num = *num + 1;
        if (*num % 10 == 0)
            *num = *num - 10;
    }

    if (msg.keys & KNOB_RIGHT)
    {
        if (*num == 0)
            *num = 9;
        else
        {
            *num = *num - 1;
            if (*num % 10 == 9)
                *num = *num + 10;
        }
    }

    // If enter is pressed, advance to the next digit
    if (msg.keys & KEY_ENTER)
    {
        uint32_t tmp = 0;
        if (!__builtin_mul_overflow(*num, 10, &tmp))
            *num = tmp;
    }

    // Announce the character
    vp_announceInputChar('0' + *num % 10);

    // Update reference values
    ui_state.input_number = *num % 10;
#else
    // Get currently pressed number key
    uint8_t num_key = input_getPressedNumber(msg);

    uint32_t tmp = 0;

    // Add a another digit onto the offset only if
    // it won't cause an overflow
    if (__builtin_mul_overflow(*num, 10, &tmp) ||
        __builtin_add_overflow(tmp, num_key, &tmp))
        return;

    *num = tmp;

    // Announce the character
    vp_announceInputChar('0' + num_key);

    // Update reference values
    ui_state.input_number = num_key;
#endif

    ui_state.last_keypress = now;
}

static void _ui_numberInputDel(uint32_t *num)
{
    // announce the digit about to be backspaced.
    vp_announceInputChar('0' + *num % 10);

    // Move back input cursor
    if(ui_state.input_position > 0)
        ui_state.input_position--;
    else
        ui_state.last_keypress = 0;

    ui_state.input_set = 0;
}

void ui_init()
{
    last_event_tick = getTick();
    redraw_needed = true;
    _ui_calculateLayout(&layout);
    layout_ready = true;
    // Initialize struct ui_state to all zeroes
    // This syntax is called compound literal
    // https://stackoverflow.com/questions/6891720/initialize-reset-struct-to-zero-null
    ui_state = (const struct ui_state_t){ 0 };
    frs_refuse_tick = 0;

    // A settings record copied from another radio, or a corrupted one, may
    // ask for FRS mode on a radio whose UHF band does not reach 462-468 MHz.
    // platform_init() has already run, so the hardware information is valid:
    // clear the flag rather than park the VFO on a channel the RTX cannot
    // tune. The settings menu applies the same check before enabling it.
    if ((state.settings.frs_mode != 0) &&
        (frs_isSupported(platform_getHwInfo()) == false))
    {
        state.settings.frs_mode = 0;
    }

    // Resume FRS mode: the channel loaded from NVM is the user's VFO, park it
    // and materialise the FRS channel. The UI thread starts with an RTX
    // synchronisation pending, which picks the new channel up.
    if (state.settings.frs_mode != 0)
    {
        bool sync = false;
        state.vfo_channel = state.channel;
        _ui_frs_apply(&sync);
        state.ui_screen = MAIN_FRS;
        ui_state.last_main_state = MAIN_FRS;
    }
}

void ui_drawSplashScreen()
{
    gfx_clearScreen();

    #if CONFIG_SCREEN_HEIGHT > 64
    static const point_t    logo_orig = {0, (CONFIG_SCREEN_HEIGHT / 2) - 6};
    static const point_t    call_orig = {0, CONFIG_SCREEN_HEIGHT - 8};
    static const fontSize_t logo_font = FONT_SIZE_12PT;
    static const fontSize_t call_font = FONT_SIZE_8PT;
    #else
    static const point_t    logo_orig = {0, 19};
    static const point_t    call_orig = {0, CONFIG_SCREEN_HEIGHT - 8};
    static const fontSize_t logo_font = FONT_SIZE_8PT;
    static const fontSize_t call_font = FONT_SIZE_6PT;
    #endif

    gfx_print(logo_orig, logo_font, TEXT_ALIGN_CENTER, yellow_fab413, "O P N\nR T X");
    gfx_print(call_orig, call_font, TEXT_ALIGN_CENTER, color_white, state.settings.callsign);

    vp_announceSplashScreen();
}

void ui_saveState()
{
    last_state = state;
}

#ifdef CONFIG_GPS
static uint16_t priorGPSSpeed = 0;
static int16_t  priorGPSAltitude = 0;
static int16_t  priorGPSDirection = 500; // impossible value init.
static uint8_t  priorGPSFixQuality= 0;
static uint8_t  priorGPSFixType = 0;
static uint8_t  priorSatellitesInView = 0;
static uint32_t vpGPSLastUpdate = 0;

static enum vpGPSInfoFlags GetGPSDirectionOrSpeedChanged()
{
    if (!state.settings.gps_enabled)
        return vpGPSNone;

    uint32_t now = getTick();
    if (now - vpGPSLastUpdate < 8000)
        return vpGPSNone;

    enum vpGPSInfoFlags whatChanged = vpGPSNone;

    if (state.gps_data.fix_quality != priorGPSFixQuality)
    {
        whatChanged |= vpGPSFixQuality;
        priorGPSFixQuality= state.gps_data.fix_quality;
    }

    if (state.gps_data.fix_type != priorGPSFixType)
    {
        whatChanged |= vpGPSFixType;
        priorGPSFixType = state.gps_data.fix_type;
    }

    if (state.gps_data.speed != priorGPSSpeed)
    {
        whatChanged |= vpGPSSpeed;
        priorGPSSpeed = state.gps_data.speed;
    }

    if (state.gps_data.altitude != priorGPSAltitude)
    {
        whatChanged |= vpGPSAltitude;
        priorGPSAltitude = state.gps_data.altitude;
    }

    if (state.gps_data.tmg_true != priorGPSDirection)
    {
        whatChanged |= vpGPSDirection;
        priorGPSDirection = state.gps_data.tmg_true;
    }

    if (state.gps_data.satellites_in_view != priorSatellitesInView)
    {
        whatChanged |= vpGPSSatCount;
        priorSatellitesInView = state.gps_data.satellites_in_view;
    }

    if (whatChanged)
        vpGPSLastUpdate=now;

    return whatChanged;
}
#endif // CONFIG_GPS

void ui_updateFSM(bool *sync_rtx)
{
    // Check for events
    if(evQueue_wrPos == evQueue_rdPos) return;

    // Pop an event from the queue
    uint8_t newTail = (evQueue_rdPos + 1) % MAX_NUM_EVENTS;
    event_t event   = evQueue[evQueue_rdPos];
    evQueue_rdPos   = newTail;

    // There is some event to process, we need an UI redraw.
    // UI redraw request is cancelled if we're in standby mode.
    redraw_needed = true;
    if(standby) redraw_needed = false;

    // Check if battery has enough charge to operate.
    // Check is skipped if there is an ongoing transmission, since the voltage
    // drop caused by the RF PA power absorption causes spurious triggers of
    // the low battery alert.
    bool txOngoing = platform_getPttStatus();
#if !defined(PLATFORM_TTWRPLUS)
    if ((!state.emergency) && (!txOngoing) && (state.charge <= 0))
    {
        state.ui_screen = LOW_BAT;
        if(event.type == EVENT_KBD && event.payload)
        {
            state.ui_screen = state.settings.frs_mode ? MAIN_FRS : MAIN_VFO;
            state.emergency = true;
        }
        return;
    }
#endif // PLATFORM_TTWRPLUS

    // Unlatch and exit from macro menu on PTT press
    if(macro_latched && txOngoing)
    {
        macro_latched = false;
        macro_menu = false;
    }

    long long now = getTick();
    // Process pressed keys
    if(event.type == EVENT_KBD)
    {
        kbd_msg_t msg;
        msg.value = event.payload;
        bool f1Handled = false;
        enum vpQueueFlags queueFlags = vp_getVoiceLevelQueueFlags();
        // If we get out of standby, we ignore the kdb event
        // unless is the MONI key for the MACRO functions
        if (_ui_exitStandby(now) && !(msg.keys & KEY_MONI))
            return;
        // If MONI is pressed, activate MACRO functions
        bool moniPressed = msg.keys & KEY_MONI;
        if(moniPressed || macro_latched)
        {
            macro_menu = true;

            if(state.settings.macroMenuLatch == 1)
            {
                // long press moni on its own latches function.
                if (moniPressed && msg.long_press && !macro_latched)
                {
                    macro_latched = true;
                    vp_beep(BEEP_FUNCTION_LATCH_ON, LONG_BEEP);
                }
                else if (moniPressed && macro_latched)
                {
                    macro_latched = false;
                    vp_beep(BEEP_FUNCTION_LATCH_OFF, LONG_BEEP);
                }
            }

            _ui_fsm_menuMacro(msg, sync_rtx);
            return;
        }
        else
        {
            macro_menu = false;
        }
#if defined(PLATFORM_TTWRPLUS)
        // T-TWR Plus has no KEY_MONI, using KEY_VOLDOWN long press instead
        if ((msg.keys & KEY_VOLDOWN) && msg.long_press)
        {
            macro_menu = true;
            macro_latched = true;
        }
#endif // PLA%FORM_TTWRPLUS

        if(state.tone_enabled && !(msg.keys & KEY_HASH))
        {
            state.tone_enabled = false;
            *sync_rtx = true;
        }

        int priorUIScreen = state.ui_screen;
        switch(state.ui_screen)
        {
            // VFO screen
            case MAIN_VFO:
            {
                // Enable Tx in MAIN_VFO mode
                if (state.txDisable)
                {
                    state.txDisable = false;
                    *sync_rtx = true;
                }

                // Break out of the FSM if the keypad is locked but allow the
                // use of the hash key in FM mode for the 1750Hz tone.
                bool skipLock =  (state.channel.mode == OPMODE_FM)
                              && (msg.keys == KEY_HASH);

                if ((ui_state.input_locked == true) && (skipLock == false))
                    break;

                if(ui_state.edit_mode)
                {
                    #ifdef CONFIG_DMR
                    // DMR talkgroup or private ID entry
                    if(state.channel.mode == OPMODE_DMR)
                    {
                        _ui_dmr_destinationInput(msg, sync_rtx, queueFlags);
                        break;
                    }
                    #endif
                    #ifdef CONFIG_M17
                    if(state.channel.mode == OPMODE_M17)
                    {
                        if(msg.keys & KEY_ENTER)
                        {
                            _ui_textInputConfirm(ui_state.new_callsign);
                            // Save selected dst ID and disable input mode
                            strncpy(state.settings.m17_dest, ui_state.new_callsign, 10);
                            ui_state.edit_mode = false;
                            *sync_rtx = true;
                            vp_announceM17Info(NULL,  ui_state.edit_mode,
                                               queueFlags);
                        }
                        else if(msg.keys & KEY_HASH)
                        {
                            // Save selected dst ID and disable input mode
                            strncpy(state.settings.m17_dest, "", 1);
                            ui_state.edit_mode = false;
                            *sync_rtx = true;
                            vp_announceM17Info(NULL,  ui_state.edit_mode,
                                               queueFlags);
                        }
                        else if(msg.keys & KEY_ESC)
                            // Discard selected dst ID and disable input mode
                            ui_state.edit_mode = false;
                        else if(msg.keys & KEY_UP || msg.keys & KEY_DOWN ||
                                msg.keys & KEY_LEFT || msg.keys & KEY_RIGHT)
                            _ui_textInputDel(ui_state.new_callsign);
                        else if(input_isCharPressed(msg))
                            _ui_textInputKeypad(ui_state.new_callsign, 9, msg, true);
                        break;
                    }
                    #endif
                }
                else
                {
                    if(msg.keys & KEY_ENTER)
                    {
                        // Save current main state
                        ui_state.last_main_state = state.ui_screen;
                        // Open Menu
                        state.ui_screen = MENU_TOP;
                        // The selected item will be announced when the item is first selected.
                    }
                    else if(msg.keys & KEY_ESC)
                    {
                        // Save VFO channel
                        state.vfo_channel = state.channel;
                        int result = _ui_fsm_loadChannel(state.channel_index, sync_rtx);
                        // Read successful and channel is valid
                        if(result != -1)
                        {
                            // Switch to MEM screen
                            state.ui_screen = MAIN_MEM;
                            // anounce the active channel name.
                            vp_announceChannelName(&state.channel,
                                                   state.channel_index,
                                                   queueFlags);
                        }
                    }
                    else if(msg.keys & KEY_HASH)
                    {
                        #ifdef CONFIG_DMR
                        // Talkgroup or private ID entry when using DMR
                        if(state.channel.mode == OPMODE_DMR)
                        {
                            _ui_dmr_openDestinationInput(queueFlags);
                        }
                        else
                        #endif
                        #ifdef CONFIG_M17
                        // Only enter edit mode when using M17
                        if(state.channel.mode == OPMODE_M17)
                        {
                            // Enable dst ID input
                            ui_state.edit_mode = true;
                            // Reset text input variables
                            _ui_textInputReset(ui_state.new_callsign,
                                    sizeof(ui_state.new_callsign));
                            vp_announceM17Info(NULL,  ui_state.edit_mode,
                                               queueFlags);
                        }
                        else
                        #endif
                        {
                            if(!state.tone_enabled)
                            {
                                state.tone_enabled = true;
                                *sync_rtx = true;
                            }
                        }
                    }
                    #ifdef CONFIG_DMR
                    else if((msg.keys & KEY_STAR) &&
                            (state.channel.mode == OPMODE_DMR))
                    {
                        _ui_dmr_toggleCallType(sync_rtx, queueFlags);
                    }
                    #endif
                    else if(msg.keys & KEY_UP || msg.keys & KNOB_RIGHT)
                    {
                        // Increment TX and RX frequency of 12.5KHz
                        if(_ui_freq_check_limits(state.channel.rx_frequency + freq_steps[state.step_index]) &&
                           _ui_freq_check_limits(state.channel.tx_frequency + freq_steps[state.step_index]))
                        {
                            state.channel.rx_frequency += freq_steps[state.step_index];
                            state.channel.tx_frequency += freq_steps[state.step_index];
                            *sync_rtx = true;
                            vp_announceFrequencies(state.channel.rx_frequency,
                                                   state.channel.tx_frequency,
                                                   queueFlags);
                        }
                    }
                    else if(msg.keys & KEY_DOWN || msg.keys & KNOB_LEFT)
                    {
                        // Decrement TX and RX frequency of 12.5KHz
                        if(_ui_freq_check_limits(state.channel.rx_frequency - freq_steps[state.step_index]) &&
                           _ui_freq_check_limits(state.channel.tx_frequency - freq_steps[state.step_index]))
                        {
                            state.channel.rx_frequency -= freq_steps[state.step_index];
                            state.channel.tx_frequency -= freq_steps[state.step_index];
                            *sync_rtx = true;
                            vp_announceFrequencies(state.channel.rx_frequency,
                                                   state.channel.tx_frequency,
                                                   queueFlags);
                        }
                    }
                    else if(msg.keys & KEY_F1)
                    {
                        if (state.settings.vpLevel > vpBeep)
                        {// quick press repeat vp, long press summary.
                            if (msg.long_press)
                                vp_announceChannelSummary(&state.channel, 0,
                                                          state.bank, vpAllInfo);
                            else
                                vp_replayLastPrompt();
                            f1Handled = true;
                        }
                    }
                    else if(input_isNumberPressed(msg))
                    {
                        // Open Frequency input screen
                        state.ui_screen = MAIN_VFO_INPUT;
                        // Reset input position and selection
                        ui_state.input_position = 1;
                        ui_state.input_set = SET_RX;
                        // do not play  because we will also announce the number just entered.
                        vp_announceInputReceiveOrTransmit(false, vpqInit);
                        vp_queueInteger(input_getPressedNumber(msg));
                        vp_play();

                        ui_state.new_rx_frequency = 0;
                        ui_state.new_tx_frequency = 0;
                        // Save pressed number to calculare frequency and show in GUI
                        ui_state.input_number = input_getPressedNumber(msg);
                        // Calculate portion of the new frequency
                        ui_state.new_rx_frequency = _ui_freq_add_digit(ui_state.new_rx_frequency,
                                                                       ui_state.input_position,
                                                                       ui_state.input_number);
                    }
                }
            }
                break;
            // VFO frequency input screen
            case MAIN_VFO_INPUT:
                if(msg.keys & KEY_ENTER)
                {
                    _ui_fsm_confirmVFOInput(sync_rtx);
                }
                else if(msg.keys & KEY_ESC)
                {
                    // Cancel frequency input, return to VFO mode
                    state.ui_screen = MAIN_VFO;
                }
                else if(msg.keys & KEY_UP || msg.keys & KEY_DOWN)
                {
                    if(ui_state.input_set == SET_RX)
                    {
                        ui_state.input_set = SET_TX;
                        vp_announceInputReceiveOrTransmit(true, queueFlags);
                    }
                    else if(ui_state.input_set == SET_TX)
                    {
                        ui_state.input_set = SET_RX;
                        vp_announceInputReceiveOrTransmit(false, queueFlags);
                    }
                    // Reset input position
                    ui_state.input_position = 0;
                }
                else if(input_isNumberPressed(msg))
                {
                    _ui_fsm_insertVFONumber(msg, sync_rtx);
                }
                break;
            // MEM screen
            case MAIN_MEM:
                // Enable Tx in MAIN_MEM mode
                if (state.txDisable)
                {
                    state.txDisable = false;
                    *sync_rtx = true;
                }
                if (ui_state.input_locked)
                    break;
                // M17 Destination callsign input
                if(ui_state.edit_mode)
                {
                    #ifdef CONFIG_DMR
                    // DMR talkgroup or private ID entry
                    if(state.channel.mode == OPMODE_DMR)
                    {
                        _ui_dmr_destinationInput(msg, sync_rtx, queueFlags);
                        break;
                    }
                    #endif
                    {
                        if(msg.keys & KEY_ENTER)
                        {
                            _ui_textInputConfirm(ui_state.new_callsign);
                            // Save selected dst ID and disable input mode
                            strncpy(state.settings.m17_dest, ui_state.new_callsign, 10);
                            ui_state.edit_mode = false;
                            *sync_rtx = true;
                        }
                        else if(msg.keys & KEY_HASH)
                        {
                            // Save selected dst ID and disable input mode
                            strncpy(state.settings.m17_dest, "", 1);
                            ui_state.edit_mode = false;
                            *sync_rtx = true;
                        }
                        else if(msg.keys & KEY_ESC)
                            // Discard selected dst ID and disable input mode
                            ui_state.edit_mode = false;
                        else if(msg.keys & KEY_F1)
                        {
                            if (state.settings.vpLevel > vpBeep)
                            {
                                // Quick press repeat vp, long press summary.
                                if (msg.long_press)
                                {
                                    vp_announceChannelSummary(
                                            &state.channel,
                                            state.channel_index,
                                            state.bank,
                                            vpAllInfo);
                                }
                                else
                                {
                                    vp_replayLastPrompt();
                                }

                                f1Handled = true;
                            }
                        }
                        else if(msg.keys & KEY_UP || msg.keys & KEY_DOWN ||
                                msg.keys & KEY_LEFT || msg.keys & KEY_RIGHT)
                            _ui_textInputDel(ui_state.new_callsign);
                        else if(input_isCharPressed(msg))
                            _ui_textInputKeypad(ui_state.new_callsign, 9, msg, true);
                        break;
                    }
                }
                else
                {
                    if(msg.keys & KEY_ENTER)
                    {
                        // Save current main state
                        ui_state.last_main_state = state.ui_screen;
                        // Open Menu
                        state.ui_screen = MENU_TOP;
                    }
                    else if(msg.keys & KEY_ESC)
                    {
                        // Restore VFO channel
                        state.channel = state.vfo_channel;
                        // Update RTX configuration
                        *sync_rtx = true;
                        // Switch to VFO screen
                        state.ui_screen = MAIN_VFO;
                    }
                    else if(msg.keys & KEY_HASH)
                    {
                        #ifdef CONFIG_DMR
                        // Talkgroup or private ID entry when using DMR
                        if(state.channel.mode == OPMODE_DMR)
                        {
                            _ui_dmr_openDestinationInput(queueFlags);
                        }
                        else
                        #endif
                        // Only enter edit mode when using M17
                        if(state.channel.mode == OPMODE_M17)
                        {
                            // Enable dst ID input
                            ui_state.edit_mode = true;
                            // Reset text input variables
                            _ui_textInputReset(ui_state.new_callsign,
                                    sizeof(ui_state.new_callsign));
                        }
                        else
                        {
                            if(!state.tone_enabled)
                            {
                                state.tone_enabled = true;
                                *sync_rtx = true;
                            }
                        }
                    }
                    #ifdef CONFIG_DMR
                    else if((msg.keys & KEY_STAR) &&
                            (state.channel.mode == OPMODE_DMR))
                    {
                        _ui_dmr_toggleCallType(sync_rtx, queueFlags);
                    }
                    #endif
                    else if(msg.keys & KEY_F1)
                    {
                        if (state.settings.vpLevel > vpBeep)
                        {// quick press repeat vp, long press summary.
                            if (msg.long_press)
                            {
                                vp_announceChannelSummary(&state.channel,
                                                          state.channel_index+1,
                                                          state.bank, vpAllInfo);
                            }
                            else
                            {
                                vp_replayLastPrompt();
                            }

                            f1Handled = true;
                        }
                    }
                    else if(msg.keys & KEY_UP || msg.keys & KNOB_RIGHT)
                    {
                        _ui_fsm_loadChannel(state.channel_index + 1, sync_rtx);
                        vp_announceChannelName(&state.channel,
                                               state.channel_index+1,
                                               queueFlags);
                    }
                    else if(msg.keys & KEY_DOWN || msg.keys & KNOB_LEFT)
                    {
                        _ui_fsm_loadChannel(state.channel_index - 1, sync_rtx);
                        vp_announceChannelName(&state.channel,
                                               state.channel_index+1,
                                               queueFlags);
                    }
                }
                break;
            // FRS main screen
            case MAIN_FRS:
                // Enable Tx in MAIN_FRS mode
                if (state.txDisable)
                {
                    state.txDisable = false;
                    *sync_rtx = true;
                }
                if (ui_state.input_locked)
                    break;

                if(msg.keys & KEY_ENTER)
                {
                    // Save current main state
                    ui_state.last_main_state = state.ui_screen;
                    // Open Menu
                    state.ui_screen = MENU_TOP;
                }
                else if(msg.keys & KEY_UP || msg.keys & KNOB_RIGHT)
                {
                    // Next channel, 22 wraps to 1
                    uint8_t next = state.settings.frs_channel + 1;
                    if(next >= FRS_CHANNEL_NUM)
                        next = 0;
                    _ui_frs_setChannel(next, sync_rtx);
                }
                else if(msg.keys & KEY_DOWN || msg.keys & KNOB_LEFT)
                {
                    // Previous channel, 1 wraps to 22
                    uint8_t prev = state.settings.frs_channel;
                    if(prev == 0)
                        prev = FRS_CHANNEL_NUM;
                    _ui_frs_setChannel(prev - 1, sync_rtx);
                }
                else if(msg.keys & KEY_F1)
                {
                    if (state.settings.vpLevel > vpBeep)
                    {
                        // quick press repeat vp, long press summary.
                        if (msg.long_press)
                        {
                            vp_announceChannelSummary(&state.channel,
                                                      state.settings.frs_channel + 1,
                                                      0, vpAllInfo);
                        }
                        else
                        {
                            vp_replayLastPrompt();
                        }

                        f1Handled = true;
                    }
                }
                else if(msg.keys & KEY_STAR || msg.keys & KEY_F2)
                {
                    _ui_frs_openCodePicker();
                }
                else if(input_isNumberPressed(msg))
                {
                    _ui_frs_inputDigit(input_getPressedNumber(msg), sync_rtx);
                }
                // ESC (VFO/MEM switch) and # (1750 Hz tone, M17 destination)
                // have no function in FRS mode and are ignored.
                break;
            // FRS privacy code picker
            case FRS_CODE:
                if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                    _ui_menuUp(FRS_CODE_NUM + 1);
                else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                    _ui_menuDown(FRS_CODE_NUM + 1);
                else if(msg.keys & KEY_ENTER)
                {
                    _ui_frs_setCode(ui_state.menu_selected, sync_rtx);
                    ui_state.menu_selected = 0;
                    state.ui_screen = MAIN_FRS;
                }
                else if(msg.keys & KEY_ESC || msg.keys & KEY_STAR)
                {
                    // Cancel, the code is unchanged
                    ui_state.menu_selected = 0;
                    state.ui_screen = MAIN_FRS;
                }
                else if(msg.keys & KEY_F1)
                {
                    // Repeat the last prompt, as on the FRS screen
                    if (state.settings.vpLevel > vpBeep)
                    {
                        vp_replayLastPrompt();
                        f1Handled = true;
                    }
                }
                else if(input_isNumberPressed(msg))
                {
                    _ui_frs_codeDigit(input_getPressedNumber(msg));
                }
                break;
            // FRS channel number entry screen
            case MAIN_FRS_INPUT:
                if(msg.keys & KEY_ENTER)
                {
                    // Accept the pending digit as the channel
                    _ui_frs_setChannel(ui_state.input_number - 1, sync_rtx);
                    state.ui_screen = MAIN_FRS;
                }
                else if(msg.keys & KEY_ESC || msg.keys & KEY_UP ||
                        msg.keys & KEY_DOWN || msg.keys & KNOB_LEFT ||
                        msg.keys & KNOB_RIGHT)
                {
                    // Cancel channel entry, the channel is unchanged
                    state.ui_screen = MAIN_FRS;
                }
                else if(input_isNumberPressed(msg))
                {
                    _ui_frs_inputDigit(input_getPressedNumber(msg), sync_rtx);
                }
                break;
            // Top menu screen
            case MENU_TOP:
                if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                    _ui_menuUp(menu_num);
                else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                    _ui_menuDown(menu_num);
                else if(msg.keys & KEY_ENTER)
                {
                    switch(ui_state.menu_selected)
                    {
                        case M_BANK:
                            state.ui_screen = MENU_BANK;
                            break;
                        case M_CHANNEL:
                            state.ui_screen = MENU_CHANNEL;
                            break;
                        case M_CONTACTS:
                            state.ui_screen = MENU_CONTACTS;
                            break;
#ifdef CONFIG_GPS
                        case M_GPS:
                            state.ui_screen = MENU_GPS;
                            break;
#endif
                        case M_SETTINGS:
                            state.ui_screen = MENU_SETTINGS;
                            break;
                        case M_INFO:
                            state.ui_screen = MENU_INFO;
                            break;
                        case M_ABOUT:
                            state.ui_screen = MENU_ABOUT;
                            break;
                    }
                    // Reset menu selection
                    ui_state.menu_selected = 0;
                }
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(ui_state.last_main_state);
                break;
            // Zone menu screen
            case MENU_BANK:
            // Channel menu screen
            case MENU_CHANNEL:
            // Contacts menu screen
            case MENU_CONTACTS:
                if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                    // Using 1 as parameter disables menu wrap around
                    _ui_menuUp(1);
                else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                {
                    if(state.ui_screen == MENU_BANK)
                    {
                        bankHdr_t bank;
                        // manu_selected is 0-based
                        // bank 0 means "All Channel" mode
                        // banks (1, n) are mapped to banks (0, n-1)
                        if(cps_readBankHeader(&bank, ui_state.menu_selected) != -1)
                            ui_state.menu_selected += 1;
                    }
                    else if(state.ui_screen == MENU_CHANNEL)
                    {
                        channel_t channel;
                        if(cps_readChannel(&channel, ui_state.menu_selected + 1) != -1)
                            ui_state.menu_selected += 1;
                    }
                    else if(state.ui_screen == MENU_CONTACTS)
                    {
                        contact_t contact;
                        if(cps_readContact(&contact, ui_state.menu_selected + 1) != -1)
                            ui_state.menu_selected += 1;
                    }
                }
                else if(msg.keys & KEY_ENTER)
                {
                    // Codeplug channels cannot be loaded while FRS mode is on
                    if(state.settings.frs_mode != 0)
                    {
                        if(state.ui_screen != MENU_CONTACTS)
                            _ui_frs_refuse();
                    }
                    else if(state.ui_screen == MENU_BANK)
                    {
                        bankHdr_t newbank;
                        int result = 0;
                        // If "All channels" is selected, load default bank
                        if(ui_state.menu_selected == 0)
                            state.bank_enabled = false;
                        else
                        {
                            state.bank_enabled = true;
                            result = cps_readBankHeader(&newbank, ui_state.menu_selected - 1);
                        }
                        if(result != -1)
                        {
                            state.bank = ui_state.menu_selected - 1;
                            // If we were in VFO mode, save VFO channel
                            if(ui_state.last_main_state == MAIN_VFO)
                                state.vfo_channel = state.channel;
                            // Load bank first channel
                            _ui_fsm_loadChannel(0, sync_rtx);
                            // Switch to MEM screen
                            state.ui_screen = MAIN_MEM;
                        }
                    }
                    else if(state.ui_screen == MENU_CHANNEL)
                    {
                        // If we were in VFO mode, save VFO channel
                        if(ui_state.last_main_state == MAIN_VFO)
                            state.vfo_channel = state.channel;
                        _ui_fsm_loadChannel(ui_state.menu_selected, sync_rtx);
                        // Switch to MEM screen
                        state.ui_screen = MAIN_MEM;
                    }
                }
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_TOP);
                break;
#ifdef CONFIG_GPS
            // GPS menu screen
            case MENU_GPS:
                if ((msg.keys & KEY_F1) && (state.settings.vpLevel > vpBeep))
                {// quick press repeat vp, long press summary.
                    if (msg.long_press)
                        vp_announceGPSInfo(vpGPSAll);
                    else
                        vp_replayLastPrompt();
                    f1Handled = true;
                }
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_TOP);
                break;
#endif
            // Settings menu screen
            case MENU_SETTINGS:
                if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                    _ui_menuUp(settings_num);
                else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                    _ui_menuDown(settings_num);
                else if(msg.keys & KEY_ENTER)
                {

                    switch(ui_state.menu_selected)
                    {
                        case S_DISPLAY:
                            state.ui_screen = SETTINGS_DISPLAY;
                            break;
#ifdef CONFIG_RTC
                        case S_TIMEDATE:
                            state.ui_screen = SETTINGS_TIMEDATE;
                            break;
#endif
#ifdef CONFIG_GPS
                        case S_GPS:
                            state.ui_screen = SETTINGS_GPS;
                            break;
#endif
                        case S_RADIO:
                            state.ui_screen = SETTINGS_RADIO;
                            break;
#ifdef CONFIG_M17
                        case S_M17:
                            state.ui_screen = SETTINGS_M17;
                            break;
#endif
#ifdef CONFIG_DMR
                        case S_DMR:
                            state.ui_screen = SETTINGS_DMR;
                            break;
#endif
                        case S_FM:
                            state.ui_screen = SETTINGS_FM;
                            break;
                        case S_FRS:
                            state.ui_screen = SETTINGS_FRS;
                            break;
                        case S_ACCESSIBILITY:
                            state.ui_screen = SETTINGS_ACCESSIBILITY;
                            break;
                        case S_RESET2DEFAULTS:
                            state.ui_screen = SETTINGS_RESET2DEFAULTS;
                            break;
                        default:
                            state.ui_screen = MENU_SETTINGS;
                    }
                    // Reset menu selection
                    ui_state.menu_selected = 0;
                }
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_TOP);
                break;
            // Flash backup and restore menu screen
            case MENU_BACKUP_RESTORE:
                if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                    _ui_menuUp(settings_num);
                else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                    _ui_menuDown(settings_num);
                else if(msg.keys & KEY_ENTER)
                {

                    switch(ui_state.menu_selected)
                    {
                        case BR_BACKUP:
                            state.ui_screen = MENU_BACKUP;
                            break;
                        case BR_RESTORE:
                            state.ui_screen = MENU_RESTORE;
                            break;
                        default:
                            state.ui_screen = MENU_BACKUP_RESTORE;
                    }
                    // Reset menu selection
                    ui_state.menu_selected = 0;
                }
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_TOP);
                break;
            case MENU_BACKUP:
            case MENU_RESTORE:
                if(msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_TOP);
                break;
            // Info menu screen
            case MENU_INFO:
                if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                    _ui_menuUp(info_num);
                else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                    _ui_menuDown(info_num);
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_TOP);
                break;
            // About screen, scroll without rollover
            case MENU_ABOUT:
                if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                {
                    if(ui_state.menu_selected > 0)
                        ui_state.menu_selected -= 1;
                }
                else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                    ui_state.menu_selected += 1;
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_TOP);
                break;
#ifdef CONFIG_RTC
            // Time&Date settings screen
            case SETTINGS_TIMEDATE:
                if(msg.keys & KEY_ENTER)
                {
                    // Switch to set Time&Date mode
                    state.ui_screen = SETTINGS_TIMEDATE_SET;
                    // Reset input position and selection
                    ui_state.input_position = 0;
                    memset(&ui_state.new_timedate, 0, sizeof(datetime_t));
                    vp_announceBuffer(&currentLanguage->timeAndDate,
                                      true, false, "dd/mm/yy");
                }
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_SETTINGS);
                break;
            // Time&Date settings screen, edit mode
            case SETTINGS_TIMEDATE_SET:
                if(msg.keys & KEY_ENTER)
                {
                    // Save time only if all digits have been inserted
                    if(ui_state.input_position < TIMEDATE_DIGITS)
                        break;
                    // Return to Time&Date menu, saving values
                    // NOTE: The user inserted a local time, we must save an UTC time
                    datetime_t utc_time = localTimeToUtc(ui_state.new_timedate,
                                                         state.settings.utc_timezone);
                    platform_setTime(utc_time);
                    state.time = utc_time;
                    vp_announceSettingsTimeDate();
                    state.ui_screen = SETTINGS_TIMEDATE;
                }
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(SETTINGS_TIMEDATE);
                else if(input_isNumberPressed(msg))
                {
                    // Discard excess digits
                    if(ui_state.input_position > TIMEDATE_DIGITS)
                        break;
                    ui_state.input_position += 1;
                    ui_state.input_number = input_getPressedNumber(msg);
                    _ui_timedate_add_digit(&ui_state.new_timedate, ui_state.input_position,
                                            ui_state.input_number);
                }
                break;
#endif
            case SETTINGS_DISPLAY:
                if(msg.keys & KEY_LEFT || (ui_state.edit_mode &&
                   (msg.keys & KEY_DOWN || msg.keys & KNOB_LEFT)))
                {
                    switch(ui_state.menu_selected)
                    {
#ifdef CONFIG_SCREEN_BRIGHTNESS
                        case D_BRIGHTNESS:
                            _ui_changeBrightness(-5);
                            vp_announceSettingsInt(&currentLanguage->brightness, queueFlags,
                                                   state.settings.brightness);
                            break;
#endif
#ifdef CONFIG_SCREEN_CONTRAST
                        case D_CONTRAST:
                            _ui_changeContrast(-4);
                            vp_announceSettingsInt(&currentLanguage->brightness, queueFlags,
                                                   state.settings.contrast);
                            break;
#endif
                        case D_TIMER:
                            _ui_changeTimer(-1);
                            vp_announceDisplayTimer();
                            break;
                        case D_BATTERY:
                            state.settings.showBatteryIcon = !state.settings.showBatteryIcon;
                            break;
                        default:
                            state.ui_screen = SETTINGS_DISPLAY;
                    }
                }
                else if(msg.keys & KEY_RIGHT || (ui_state.edit_mode &&
                        (msg.keys & KEY_UP || msg.keys & KNOB_RIGHT)))
                {
                    switch(ui_state.menu_selected)
                    {
#ifdef CONFIG_SCREEN_BRIGHTNESS
                        case D_BRIGHTNESS:
                            _ui_changeBrightness(+5);
                            vp_announceSettingsInt(&currentLanguage->brightness, queueFlags,
                                                   state.settings.brightness);
                            break;
#endif
#ifdef CONFIG_SCREEN_CONTRAST
                        case D_CONTRAST:
                            _ui_changeContrast(+4);
                            vp_announceSettingsInt(&currentLanguage->brightness, queueFlags,
                                                   state.settings.contrast);
                            break;
#endif
                        case D_TIMER:
                            _ui_changeTimer(+1);
                            vp_announceDisplayTimer();
                            break;
                        default:
                            state.ui_screen = SETTINGS_DISPLAY;
                    }
                }
                else if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                    _ui_menuUp(display_num);
                else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                    _ui_menuDown(display_num);
                else if(msg.keys & KEY_ENTER)
                    ui_state.edit_mode = !ui_state.edit_mode;
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_SETTINGS);
                break;
#ifdef CONFIG_GPS
            case SETTINGS_GPS:
                if(msg.keys & KEY_LEFT || msg.keys & KEY_RIGHT ||
                   (ui_state.edit_mode &&
                   (msg.keys & KEY_DOWN || msg.keys & KNOB_LEFT ||
                    msg.keys & KEY_UP || msg.keys & KNOB_RIGHT)))
                {
                    switch(ui_state.menu_selected)
                    {
                        case G_ENABLED:
                            if(state.settings.gps_enabled)
                                state.settings.gps_enabled = 0;
                            else
                                state.settings.gps_enabled = 1;
                            vp_announceSettingsOnOffToggle(&currentLanguage->gpsEnabled,
                                                           queueFlags,
                                                           state.settings.gps_enabled);
                            break;
#ifdef CONFIG_RTC
                        case G_SET_TIME:
                            state.settings.gpsSetTime = !state.settings.gpsSetTime;
                            vp_announceSettingsOnOffToggle(&currentLanguage->gpsSetTime,
                                                           queueFlags,
                                                           state.settings.gpsSetTime);
                            break;
                        case G_TIMEZONE:
                            if(msg.keys & KEY_LEFT || msg.keys & KEY_DOWN ||
                               msg.keys & KNOB_LEFT)
                                state.settings.utc_timezone -= 1;
                            else if(msg.keys & KEY_RIGHT || msg.keys & KEY_UP ||
                                    msg.keys & KNOB_RIGHT)
                                state.settings.utc_timezone += 1;
                            vp_announceTimeZone(state.settings.utc_timezone, queueFlags);
                            break;
#endif
                        default:
                            state.ui_screen = SETTINGS_GPS;
                    }
                }
                else if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                    _ui_menuUp(settings_gps_num);
                else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                    _ui_menuDown(settings_gps_num);
                else if(msg.keys & KEY_ENTER)
                    ui_state.edit_mode = !ui_state.edit_mode;
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_SETTINGS);
                break;
#endif
            // Radio Settings
            case SETTINGS_RADIO:
                // If the entry is selected with enter we are in edit_mode
                if (ui_state.edit_mode)
                {
                    switch(ui_state.menu_selected)
                    {
                        case R_OFFSET:
                            // Handle offset frequency input
#if defined(CONFIG_UI_NO_KEYBOARD)
                            if(msg.long_press && msg.keys & KEY_ENTER)
                            {
                                // Long press on CONFIG_UI_NO_KEYBOARD causes digits to advance by one
                                ui_state.new_offset /= 10;
#else
                            if(msg.keys & KEY_ENTER)
                            {
#endif
                                // Apply new offset if it is within the hardware
                                // limits of the radio
                                freq_t new_freq = state.channel.rx_frequency + ui_state.new_offset;
                                if (_ui_freq_check_limits(new_freq))
                                {
                                    state.channel.tx_frequency = new_freq;
                                    vp_queueStringTableEntry(&currentLanguage->offset);
                                    vp_queueFrequency(ui_state.new_offset);
                                    ui_state.edit_mode = false;
                                }
                            }
                            else
                            if(msg.keys & KEY_ESC)
                            {
                                // Announce old frequency offset
                                vp_queueStringTableEntry(&currentLanguage->offset);
                                vp_queueFrequency((int32_t)state.channel.tx_frequency - (int32_t)state.channel.rx_frequency);
                            }
                            else if(msg.keys & KEY_UP || msg.keys & KEY_DOWN ||
                                    msg.keys & KEY_LEFT || msg.keys & KEY_RIGHT)
                            {
                                _ui_numberInputDel(&ui_state.new_offset);
                            }
#if defined(CONFIG_UI_NO_KEYBOARD)
                            else if(msg.keys & KNOB_LEFT || msg.keys & KNOB_RIGHT || msg.keys & KEY_ENTER)
#else
                            else if(input_isNumberPressed(msg))
#endif
                            {
                                _ui_numberInputKeypad(&ui_state.new_offset, msg);
                                ui_state.input_position += 1;
                            }
                            else if (msg.long_press && (msg.keys & KEY_F1) && (state.settings.vpLevel > vpBeep))
                            {
                                vp_queueFrequency(ui_state.new_offset);
                                f1Handled=true;
                            }
                            break;
                        case R_DIRECTION:
                            if(msg.keys & KEY_UP || msg.keys & KEY_DOWN ||
                               msg.keys & KEY_LEFT || msg.keys & KEY_RIGHT ||
                               msg.keys & KNOB_LEFT || msg.keys & KNOB_RIGHT)
                            {
                                // Invert frequency offset direction
                                if (state.channel.tx_frequency >= state.channel.rx_frequency)
                                    state.channel.tx_frequency -= 2 * ((int32_t)state.channel.tx_frequency - (int32_t)state.channel.rx_frequency);
                                else // Switch to positive offset
                                    state.channel.tx_frequency -= 2 * ((int32_t)state.channel.tx_frequency - (int32_t)state.channel.rx_frequency);
                            }
                            break;
                        case R_STEP:
                            if (msg.keys & KEY_UP || msg.keys & KEY_RIGHT || msg.keys & KNOB_RIGHT)
                            {
                                // Cycle over the available frequency steps
                                state.step_index++;
                                state.step_index %= n_freq_steps;
                            }
                            else if(msg.keys & KEY_DOWN || msg.keys & KEY_LEFT || msg.keys & KNOB_LEFT)
                            {
                                state.step_index += n_freq_steps;
                                state.step_index--;
                                state.step_index %= n_freq_steps;
                            }
                            break;
                        default:
                            state.ui_screen = SETTINGS_RADIO;
                    }
                    // If ENTER or ESC are pressed, exit edit mode, R_OFFSET is managed separately
                    if((ui_state.menu_selected != R_OFFSET && msg.keys & KEY_ENTER) || msg.keys & KEY_ESC)
                        ui_state.edit_mode = false;
                }
                else if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                    _ui_menuUp(settings_radio_num);
                else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                    _ui_menuDown(settings_radio_num);
                else if((msg.keys & KEY_ENTER) && (state.settings.frs_mode != 0))
                {
                    // Offset, direction and step are fixed by the FRS plan
                    _ui_frs_refuse();
                }
                else if(msg.keys & KEY_ENTER) {
                    ui_state.edit_mode = true;
                    // If we are entering R_OFFSET clear temp offset
                    if (ui_state.menu_selected == R_OFFSET)
                        ui_state.new_offset = 0;
                    // Reset input position
                    ui_state.input_position = 0;
                }
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_SETTINGS);
                break;
#ifdef CONFIG_M17
            // M17 Settings
            case SETTINGS_M17:
                if(ui_state.edit_mode)
                {
                    switch (ui_state.menu_selected)
                    {
                        case M17_CALLSIGN:
                            // Handle text input for M17 callsign
                            if(msg.keys & KEY_ENTER)
                            {
                                _ui_textInputConfirm(ui_state.new_callsign);
                                // Save selected callsign and disable input mode
                                strncpy(state.settings.callsign, ui_state.new_callsign, 10);
                                ui_state.edit_mode = false;
                                vp_announceBuffer(&currentLanguage->callsign,
                                                  false, true, state.settings.callsign);
                            }
                            else if(msg.keys & KEY_ESC)
                            {
                                // Discard selected callsign and disable input mode
                                ui_state.edit_mode = false;
                                vp_announceBuffer(&currentLanguage->callsign,
                                                  false, true, state.settings.callsign);
                            }
                            else if(msg.keys & KEY_UP || msg.keys & KEY_DOWN ||
                                     msg.keys & KEY_LEFT || msg.keys & KEY_RIGHT)
                            {
                                _ui_textInputDel(ui_state.new_callsign);
                            }
                            else if(input_isCharPressed(msg))
                            {
                                _ui_textInputKeypad(ui_state.new_callsign, 9, msg, true);
                            }
                            else if (msg.long_press && (msg.keys & KEY_F1) && (state.settings.vpLevel > vpBeep))
                            {
                                vp_announceBuffer(&currentLanguage->callsign,
                                                  true, true, ui_state.new_callsign);
                                f1Handled=true;
                            }
                            break;
                        case M17_METATEXT:
                            // Handle text input for M17 message text
                            if(msg.keys & KEY_ENTER)
                            {
                                _ui_textInputConfirm(ui_state.new_message);
                                // Save selected message and disable input mode
                                strncpy(state.settings.M17_meta_text, ui_state.new_message, 52);
                                ui_state.edit_message = false;
                                ui_state.edit_mode = false;
                                vp_announceBuffer(&currentLanguage->metaText,
                                                  false, true, state.settings.M17_meta_text);
                            }
                            else if(msg.keys & KEY_ESC)
                            {
                                // Discard selected message and disable input mode
                                ui_state.edit_message = false;
                                ui_state.edit_mode = false;
                                vp_announceBuffer(&currentLanguage->metaText,
                                                  false, true, state.settings.M17_meta_text);
                            }
                            else if(msg.keys & KEY_UP || msg.keys & KEY_DOWN ||
                                     msg.keys & KEY_LEFT || msg.keys & KEY_RIGHT)
                            {
                                _ui_textInputDel(ui_state.new_message);
                            }
                            else if(input_isCharPressed(msg))
                            {
                                _ui_textInputKeypad(ui_state.new_message, 52, msg, false);
                            }
                            else if (msg.long_press && (msg.keys & KEY_F1) && (state.settings.vpLevel > vpBeep))
                            {
                                vp_announceBuffer(&currentLanguage->metaText,
                                                  true, true, ui_state.new_message);
                                f1Handled=true;
                            }
                            break;
                        case M17_CAN:
                            if(msg.keys & KEY_DOWN || msg.keys & KNOB_LEFT)
                                _ui_changeM17Can(-1);
                            else if(msg.keys & KEY_UP || msg.keys & KNOB_RIGHT)
                                _ui_changeM17Can(+1);
                            else if(msg.keys & KEY_ENTER)
                                ui_state.edit_mode = !ui_state.edit_mode;
                            else if(msg.keys & KEY_ESC)
                                ui_state.edit_mode = false;
                            break;
                        case M17_CAN_RX:
                            if(msg.keys & KEY_LEFT || msg.keys & KEY_RIGHT ||
                                (ui_state.edit_mode &&
                                 (msg.keys & KEY_DOWN || msg.keys & KNOB_LEFT ||
                                  msg.keys & KEY_UP || msg.keys & KNOB_RIGHT)))
                            {
                                state.settings.m17_can_rx =
                                    !state.settings.m17_can_rx;
                            }
                            else if(msg.keys & KEY_ENTER)
                                ui_state.edit_mode = !ui_state.edit_mode;
                            else if(msg.keys & KEY_ESC)
                                ui_state.edit_mode = false;
                    }
                }
                else
                {
                    if(msg.keys & KEY_ENTER)
                    {
                        // Enable edit mode
                        ui_state.edit_mode = true;

                        // If callsign input, reset text input variables
                        if(ui_state.menu_selected == M17_CALLSIGN)
                        {
                            _ui_textInputReset(ui_state.new_callsign,
                                    sizeof(ui_state.new_callsign));
                            vp_announceBuffer(&currentLanguage->callsign,
                                            true, true, ui_state.new_callsign);
                        }
                        // If message input, reset text input variables
                        if(ui_state.menu_selected == M17_METATEXT)
                        {
                            //   ui_state.edit_mode = false;
                            ui_state.edit_message = true;
                            _ui_textInputReset(ui_state.new_message,
                                    sizeof(ui_state.new_message));
                            vp_announceBuffer(&currentLanguage->metaText,
                                            true, true, ui_state.new_message);
                        }
                    }
                    else if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                        _ui_menuUp(settings_m17_num);
                    else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                        _ui_menuDown(settings_m17_num);
                    else if((msg.keys & KEY_RIGHT) && (ui_state.menu_selected == M17_CAN))
                            _ui_changeM17Can(+1);
                    else if((msg.keys & KEY_LEFT)  && (ui_state.menu_selected == M17_CAN))
                            _ui_changeM17Can(-1);
                    else if(msg.keys & KEY_ESC)
                    {
                        *sync_rtx = true;
                        _ui_menuBack(MENU_SETTINGS);
                    }
                }
                break;
#endif
            case SETTINGS_FM:
                if (ui_state.edit_mode)
                {
                    if (msg.keys & KEY_ESC)
                        ui_state.edit_mode = false;

                    switch (ui_state.menu_selected)
                    {
                        case CTCSS_Tone:
                            if (msg.keys & KEY_DOWN || msg.keys & KNOB_LEFT)
                            {
                                if (state.channel.fm.txTone == 0)
                                {
                                    state.channel.fm.txTone =
                                        CTCSS_FREQ_NUM - 1;
                                }
                                else
                                {
                                    state.channel.fm.txTone--;
                                }
                            }
                            else if (msg.keys & KEY_UP || msg.keys & KNOB_RIGHT)
                            {
                                state.channel.fm.txTone++;
                            } else if (msg.keys & KEY_ENTER) {
                                ui_state.edit_mode = false;
                            }
                            state.channel.fm.txTone %= CTCSS_FREQ_NUM;
                            state.channel.fm.rxTone = state.channel.fm.txTone;
                            *sync_rtx = true;
                            break;
                        case CTCSS_Enabled:
                            if (msg.keys & KEY_DOWN || msg.keys & KNOB_LEFT)
                            {
                                _ui_handleToneSelectScroll(true);
                            } else if (msg.keys & KEY_UP || msg.keys & KNOB_RIGHT)
                            {
                                _ui_handleToneSelectScroll(false);
                            } else if (msg.keys & KEY_ENTER) {
                                ui_state.edit_mode = false;
                            }

                            *sync_rtx = true;
                            break;
                    }
                }
                else if (msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                    _ui_menuUp(settings_fm_num);
                else if (msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                    _ui_menuDown(settings_fm_num);
                else if (msg.keys & KEY_ENTER)
                {
                    // The tone is set by the FRS privacy code
                    if (state.settings.frs_mode != 0)
                        _ui_frs_refuse();
                    else
                        ui_state.edit_mode = !ui_state.edit_mode;
                }
                else if (msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_SETTINGS);
                else if (msg.keys & KEY_ENTER)
                    ui_state.edit_mode = !ui_state.edit_mode;
                else if (msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_SETTINGS);
                break;

            // FRS settings
            case SETTINGS_FRS:
                if(msg.keys & KEY_LEFT || msg.keys & KEY_RIGHT ||
                   (ui_state.edit_mode &&
                   (msg.keys & KEY_DOWN || msg.keys & KNOB_LEFT ||
                    msg.keys & KEY_UP || msg.keys & KNOB_RIGHT)))
                {
                    // Reset Codes is confirmed with ENTER only
                    if(ui_state.menu_selected == FRS_MODE)
                        _ui_frs_toggle(sync_rtx);
                }
                else if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                    _ui_menuUp(settings_frs_num);
                else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                    _ui_menuDown(settings_frs_num);
                else if(msg.keys & KEY_ENTER)
                {
                    if(ui_state.edit_mode &&
                       (ui_state.menu_selected == FRS_RESET_CODES))
                    {
                        _ui_frs_resetCodes(sync_rtx);
                        ui_state.edit_mode = false;

                        // The menu gives no confirmation of its own: while
                        // the mode is on, return to the FRS screen so that
                        // the cleared "Code OFF" line is seen.
                        if(state.settings.frs_mode != 0)
                        {
                            ui_state.menu_selected = 0;
                            state.ui_screen = MAIN_FRS;
                        }
                    }
                    else
                        ui_state.edit_mode = !ui_state.edit_mode;
                }
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_SETTINGS);
                break;

#ifdef CONFIG_DMR
            // DMR settings
            case SETTINGS_DMR:
                if(ui_state.edit_mode &&
                   ((ui_state.menu_selected == DMR_ID) ||
                    (ui_state.menu_selected == DMR_TALKGROUP)))
                {
                    // Numeric entry of the DMR ID or of the talkgroup
                    bool accepted = false;
                    if(_ui_dmr_numberInput(msg, &accepted))
                    {
                        ui_state.edit_mode = false;
                        if(ui_state.menu_selected == DMR_ID)
                        {
                            if(accepted)
                                state.settings.dmr_id = ui_state.new_dmr_number;
                            vp_announceSettingsInt(&currentLanguage->dmrId,
                                                   queueFlags,
                                                   state.settings.dmr_id);
                        }
                        else
                        {
                            if(accepted)
                                state.settings.dmr_talkgroup =
                                    ui_state.new_dmr_number;
                            _ui_dmr_announceDestination(queueFlags);
                        }
                    }
                }
                else if(msg.keys & KEY_LEFT || msg.keys & KEY_RIGHT ||
                        (ui_state.edit_mode &&
                        (msg.keys & KEY_DOWN || msg.keys & KNOB_LEFT ||
                         msg.keys & KEY_UP || msg.keys & KNOB_RIGHT)))
                {
                    uint32_t down = KEY_LEFT | KEY_DOWN | KNOB_LEFT;
                    int variation = (msg.keys & down) ? -1 : +1;
                    switch(ui_state.menu_selected)
                    {
                        case DMR_CALLTYPE:
                            _ui_dmr_changeCallType(variation);
                            _ui_dmr_announceDestination(queueFlags);
                            break;
                        case DMR_COLORCODE:
                            _ui_dmr_changeColorCode(variation);
                            vp_announceSettingsInt(
                                &currentLanguage->colorCode, queueFlags,
                                state.settings.dmr_colorCode);
                            break;
                        case DMR_TIMESLOT:
                            _ui_dmr_toggleTimeslot();
                            vp_announceSettingsInt(&currentLanguage->timeslot,
                                                   queueFlags,
                                                   state.settings.dmr_timeslot);
                            break;
                        case DMR_MONITOR:
                            _ui_dmr_changeMonitor(variation);
                            vp_announceSettingsInt(&currentLanguage->monitor,
                                                   queueFlags,
                                                   state.settings.dmr_monitor);
                            break;
                        case DMR_ACCESS:
                            state.settings.dmr_polite =
                                !state.settings.dmr_polite;
                            vp_announceText(state.settings.dmr_polite
                                            ? currentLanguage->polite
                                            : currentLanguage->impolite,
                                            queueFlags);
                            break;
                        case DMR_HANGTIME:
                            _ui_dmr_changeHangTime(variation);
                            vp_announceSettingsInt(&currentLanguage->hangTime,
                                                   queueFlags,
                                                   state.settings.dmr_hangTime);
                            break;
                        default:
                            // The DMR ID and the talkgroup are typed in
                            break;
                    }
                }
                else if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                    _ui_menuUp(settings_dmr_num);
                else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                    _ui_menuDown(settings_dmr_num);
                else if(msg.keys & KEY_ENTER)
                {
                    ui_state.edit_mode = !ui_state.edit_mode;
                    if(ui_state.edit_mode)
                        _ui_dmr_numberReset();
                }
                else if(msg.keys & KEY_ESC)
                {
                    ui_state.edit_mode = false;
                    *sync_rtx = true;
                    _ui_menuBack(MENU_SETTINGS);
                }
                break;
#endif

            case SETTINGS_ACCESSIBILITY:
                if(msg.keys & KEY_LEFT || (ui_state.edit_mode &&
                   (msg.keys & KEY_DOWN || msg.keys & KNOB_LEFT)))
                {
                    switch(ui_state.menu_selected)
                    {
                        case A_MACRO_LATCH:
                            _ui_changeMacroLatch(false);
                            break;
                        case A_LEVEL:
                            _ui_changeVoiceLevel(-1);
                            break;
                        case A_PHONETIC:
                            _ui_changePhoneticSpell(false);
                            break;
                        default:
                            state.ui_screen = SETTINGS_ACCESSIBILITY;
                    }
                }
                else if(msg.keys & KEY_RIGHT || (ui_state.edit_mode &&
                        (msg.keys & KEY_UP || msg.keys & KNOB_RIGHT)))
                {
                    switch(ui_state.menu_selected)
                    {
                        case A_MACRO_LATCH:
                            _ui_changeMacroLatch(true);
                            break;
                        case A_LEVEL:
                            _ui_changeVoiceLevel(1);
                            break;
                        case A_PHONETIC:
                            _ui_changePhoneticSpell(true);
                            break;
                        default:
                            state.ui_screen = SETTINGS_ACCESSIBILITY;
                    }
                }
                else if(msg.keys & KEY_UP || msg.keys & KNOB_LEFT)
                    _ui_menuUp(settings_accessibility_num);
                else if(msg.keys & KEY_DOWN || msg.keys & KNOB_RIGHT)
                    _ui_menuDown(settings_accessibility_num);
                else if(msg.keys & KEY_ENTER)
                    ui_state.edit_mode = !ui_state.edit_mode;
                else if(msg.keys & KEY_ESC)
                    _ui_menuBack(MENU_SETTINGS);
                break;
            case SETTINGS_RESET2DEFAULTS:
                if(! ui_state.edit_mode){
                    //require a confirmation ENTER, then another
                    //edit_mode is slightly misused to allow for this
                    if(msg.keys & KEY_ENTER)
                    {
                        ui_state.edit_mode = true;
                    }
                    else if(msg.keys & KEY_ESC)
                    {
                        _ui_menuBack(MENU_SETTINGS);
                    }
                } else {
                    if(msg.keys & KEY_ENTER)
                    {
                        ui_state.edit_mode = false;
                        state_resetSettingsAndVfo();
                        // Defaults clear FRS mode: leave the menu on the
                        // VFO screen with the default channel applied.
                        ui_state.last_main_state = MAIN_VFO;
                        *sync_rtx = true;
                        _ui_menuBack(MENU_SETTINGS);
                    }
                    else if(msg.keys & KEY_ESC)
                    {
                        ui_state.edit_mode = false;
                        _ui_menuBack(MENU_SETTINGS);
                    }
                }
                break;
        }

        // Enable Tx only if in MAIN_VFO, MAIN_MEM or MAIN_FRS states
        bool inMemOrVfo = (state.ui_screen == MAIN_VFO) || (state.ui_screen == MAIN_MEM)
                       || (state.ui_screen == MAIN_FRS);
        if ((macro_menu == true) || ((inMemOrVfo == false) && (state.txDisable == false)))
        {
            state.txDisable = true;
            *sync_rtx = true;
        }
        if (!f1Handled && (msg.keys & KEY_F1) && (state.settings.vpLevel > vpBeep))
        {
            vp_replayLastPrompt();
        }
        else if ((priorUIScreen!=state.ui_screen) && state.settings.vpLevel > vpLow)
        {
            // When we switch to VFO or Channel screen, we need to announce it.
            // Likewise for information screens.
            // All other cases are handled as needed.
            vp_announceScreen(state.ui_screen);
        }
        // generic beep for any keydown if beep is enabled.
        // At vp levels higher than beep, keys will generate voice so no need
        // to beep or you'll get an unwanted click.
        if ((msg.keys &0xffff) && (state.settings.vpLevel == vpBeep))
            vp_beep(BEEP_KEY_GENERIC, SHORT_BEEP);
        // If we exit and re-enter the same menu, we want to ensure it speaks.
        if (msg.keys & KEY_ESC)
            _ui_reset_menu_anouncement_tracking();
    }
    else if(event.type == EVENT_STATUS)
    {
#ifdef CONFIG_GPS
        if ((state.ui_screen == MENU_GPS) &&
            (!vp_isPlaying()) &&
            (state.settings.vpLevel > vpLow) &&
            (!txOngoing && !rtx_rxSquelchOpen()))
        {// automatically read speed and direction changes only!
            enum vpGPSInfoFlags whatChanged = GetGPSDirectionOrSpeedChanged();
            if (whatChanged != vpGPSNone)
                vp_announceGPSInfo(whatChanged);
        }
#endif //            CONFIG_GPS

        // FRS channel entry: accept the pending digit once the timeout has
        // elapsed. This does not go through the key handling epilogue, so
        // re-enable TX for the main screen here.
        if ((state.ui_screen == MAIN_FRS_INPUT) &&
            ((now - ui_state.last_keypress) >= FRS_INPUT_TIMEOUT))
        {
            _ui_frs_setChannel(ui_state.input_number - 1, sync_rtx);
            state.ui_screen = MAIN_FRS;
            state.txDisable = false;
        }

        if (txOngoing || rtx_rxSquelchOpen() || (state.volume != last_state.volume))
        {
            _ui_exitStandby(now);
            return;
        }

        if (_ui_checkStandby(now - last_event_tick))
        {
            _ui_enterStandby();
        }
    }
}

bool ui_updateGUI()
{
    if(redraw_needed == false)
        return false;

    if(!layout_ready)
    {
        _ui_calculateLayout(&layout);
        layout_ready = true;
    }
    // Draw current GUI page
    switch(last_state.ui_screen)
    {
        // VFO main screen
        case MAIN_VFO:
            _ui_drawMainVFO(&ui_state);
            break;
        // VFO frequency input screen
        case MAIN_VFO_INPUT:
            _ui_drawMainVFOInput(&ui_state);
            break;
        // MEM main screen
        case MAIN_MEM:
            _ui_drawMainMEM(&ui_state);
            break;
        // FRS main screen
        case MAIN_FRS:
            _ui_drawMainFRS(&ui_state);
            break;
        // FRS channel number entry screen
        case MAIN_FRS_INPUT:
            _ui_drawMainFRSInput(&ui_state);
            break;
        // FRS privacy code picker
        case FRS_CODE:
            _ui_drawFRSCode(&ui_state);
            break;
        // Top menu screen
        case MENU_TOP:
            _ui_drawMenuTop(&ui_state);
            break;
        // Zone menu screen
        case MENU_BANK:
            _ui_drawMenuBank(&ui_state);
            break;
        // Channel menu screen
        case MENU_CHANNEL:
            _ui_drawMenuChannel(&ui_state);
            break;
        // Contacts menu screen
        case MENU_CONTACTS:
            _ui_drawMenuContacts(&ui_state);
            break;
#ifdef CONFIG_GPS
        // GPS menu screen
        case MENU_GPS:
            _ui_drawMenuGPS();
            break;
#endif
        // Settings menu screen
        case MENU_SETTINGS:
            _ui_drawMenuSettings(&ui_state);
            break;
        // Flash backup and restore screen
        case MENU_BACKUP_RESTORE:
            _ui_drawMenuBackupRestore(&ui_state);
            break;
        // Flash backup screen
        case MENU_BACKUP:
            _ui_drawMenuBackup(&ui_state);
            break;
        // Flash restore screen
        case MENU_RESTORE:
            _ui_drawMenuRestore(&ui_state);
            break;
        // Info menu screen
        case MENU_INFO:
            _ui_drawMenuInfo(&ui_state);
            break;
        // About menu screen
        case MENU_ABOUT:
            _ui_drawMenuAbout(&ui_state);
            break;
#ifdef CONFIG_RTC
        // Time&Date settings screen
        case SETTINGS_TIMEDATE:
            _ui_drawSettingsTimeDate();
            break;
        // Time&Date settings screen, edit mode
        case SETTINGS_TIMEDATE_SET:
            _ui_drawSettingsTimeDateSet(&ui_state);
            break;
#endif
        // Display settings screen
        case SETTINGS_DISPLAY:
            _ui_drawSettingsDisplay(&ui_state);
            break;
#ifdef CONFIG_GPS
        // GPS settings screen
        case SETTINGS_GPS:
            _ui_drawSettingsGPS(&ui_state);
            break;
#endif
#ifdef CONFIG_M17
        // M17 settings screen
        case SETTINGS_M17:
            _ui_drawSettingsM17(&ui_state);
            break;
#endif
        // FM settings screen
        case SETTINGS_FM:
            _ui_drawSettingsFM(&ui_state);
            break;
        // FRS settings screen
        case SETTINGS_FRS:
            _ui_drawSettingsFRS(&ui_state);
            break;
#ifdef CONFIG_DMR
        // DMR settings screen
        case SETTINGS_DMR:
            _ui_drawSettingsDMR(&ui_state);
            break;
#endif
        case SETTINGS_ACCESSIBILITY:
            _ui_drawSettingsAccessibility(&ui_state);
            break;
        // Screen to support resetting Settings and VFO to defaults
        case SETTINGS_RESET2DEFAULTS:
            _ui_drawSettingsReset2Defaults(&ui_state);
            break;
        // Screen to set frequency offset and step
        case SETTINGS_RADIO:
            _ui_drawSettingsRadio(&ui_state);
            break;
        // Low battery screen
        case LOW_BAT:
            _ui_drawLowBatteryScreen();
            break;
    }

    // If MACRO menu is active draw it
    if(macro_menu)
    {
        _ui_drawDarkOverlay();
        _ui_drawMacroMenu(&ui_state);
    }

    // A key refused by FRS mode is flagged in the top bar of whatever screen
    // is shown, so the refusal is visible even with voice prompts and beeps
    // off. Drawn last to stay on top of the macro menu overlay; it goes away
    // with the periodic status event redraws. The main screens keep the clock
    // in the centre and the battery at the right, so there the marker goes at
    // the left; the menu screens centre a title that can reach the left edge
    // of the 160-pixel displays, so there it goes at the right. A backing box
    // keeps it legible over whatever is beneath.
    if(_ui_frsRefuseMarkerVisible(getTick()))
    {
        bool mainScreen = (last_state.ui_screen == MAIN_VFO)
                       || (last_state.ui_screen == MAIN_VFO_INPUT)
                       || (last_state.ui_screen == MAIN_MEM)
                       || (last_state.ui_screen == MAIN_FRS)
                       || (last_state.ui_screen == MAIN_FRS_INPUT);
        char marker[8];
        sniprintf(marker, sizeof(marker), "%s!", currentLanguage->frs);

        uint16_t width = gfx_getTextWidth(layout.top_font, marker)
                       + layout.top_pos.x + 1;
        point_t  box   = {mainScreen ? 0 : CONFIG_SCREEN_WIDTH - width, 0};
        gfx_drawRect(box, width, layout.top_h, color_black, true);
        gfx_print(layout.top_pos, layout.top_font,
                  mainScreen ? TEXT_ALIGN_LEFT : TEXT_ALIGN_RIGHT,
                  yellow_fab413, "%s", marker);
    }

    redraw_needed = false;
    return true;
}

bool ui_pushEvent(const uint8_t type, const uint32_t data)
{
    uint8_t newHead = (evQueue_wrPos + 1) % MAX_NUM_EVENTS;

    // Queue is full
    if(newHead == evQueue_rdPos) return false;

    // Preserve atomicity when writing the new element into the queue.
    event_t event;
    event.type    = type;
    event.payload = data;

    evQueue[evQueue_wrPos] = event;
    evQueue_wrPos = newHead;

    return true;
}

void ui_terminate()
{
}
