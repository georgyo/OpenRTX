/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef UI_DEFAULT_H
#define UI_DEFAULT_H

#include <stdbool.h>
#include "core/state.h"
#include "core/graphics.h"
#include "interfaces/keyboard.h"
#include <stdint.h>
#include "core/event.h"
#include "hwconfig.h"
#include "core/ui.h"
#include "rtx/rtx.h"
#include "ui/ui_strings.h"

// Maximum menu entry length
#define MAX_ENTRY_LEN 21
// Frequency digits
#define FREQ_DIGITS 7
// Time & Date digits
#define TIMEDATE_DIGITS 10
// Max number of UI events
#define MAX_NUM_EVENTS 16

enum uiScreen
{
    MAIN_VFO = 0,
    MAIN_VFO_INPUT,
    MAIN_MEM,
    MODE_VFO,
    MODE_MEM,
    MENU_TOP,
    MENU_BANK,
    MENU_CHANNEL,
    MENU_CONTACTS,
    MENU_GPS,
    MENU_SETTINGS,
    MENU_BACKUP_RESTORE,
    MENU_BACKUP,
    MENU_RESTORE,
    MENU_INFO,
    MENU_ABOUT,
    SETTINGS_TIMEDATE,
    SETTINGS_TIMEDATE_SET,
    SETTINGS_DISPLAY,
    SETTINGS_GPS,
    SETTINGS_RADIO,
    SETTINGS_M17,
    SETTINGS_FM,
    SETTINGS_ACCESSIBILITY,
    SETTINGS_RESET2DEFAULTS,
    MAIN_FRS,
    MAIN_FRS_INPUT,
    FRS_CODE,
    SETTINGS_FRS,
    SETTINGS_DMR,
    DMR_MONITOR_SCREEN,
    LOW_BAT
};

enum SetRxTx
{
    SET_RX = 0,
    SET_TX
};

// This enum is needed to have item numbers that match
// menu elements even if some elements may be missing (GPS)
enum menuItems
{
    M_BANK = 0,
    M_CHANNEL,
    M_CONTACTS,
#ifdef CONFIG_GPS
    M_GPS,
#endif
    M_SETTINGS,
    M_INFO,
    M_ABOUT
};

enum settingsItems
{
    S_DISPLAY = 0,
#ifdef CONFIG_RTC
    S_TIMEDATE,
#endif
#ifdef CONFIG_GPS
    S_GPS,
#endif
    S_RADIO,
#ifdef CONFIG_M17
    S_M17,
#endif
#ifdef CONFIG_DMR
    S_DMR,
#endif
    S_FM,
    S_FRS,
    S_ACCESSIBILITY,
    S_RESET2DEFAULTS,
};

enum backupRestoreItems
{
    BR_BACKUP = 0,
    BR_RESTORE
};

enum displayItems
{
#ifdef CONFIG_SCREEN_BRIGHTNESS
    D_BRIGHTNESS = 0,
#endif
#ifdef CONFIG_SCREEN_CONTRAST
    D_CONTRAST,
#endif
    D_TIMER,
    D_BATTERY
};

#ifdef CONFIG_GPS
enum settingsGPSItems
{
    G_ENABLED = 0,
#ifdef CONFIG_RTC
    G_SET_TIME,
    G_TIMEZONE
#endif
};
#endif

enum settingsAccessibilityItems
{
    A_MACRO_LATCH = 0,
    A_LEVEL,
    A_PHONETIC,
};

enum settingsRadioItems
{
    R_OFFSET,
    R_DIRECTION,
    R_STEP,
};

enum settingsM17Items
{
    M17_CALLSIGN = 0,
    M17_METATEXT,
    M17_CAN,
    M17_CAN_RX
};

enum settingsFMItems
{
    CTCSS_Tone,
    CTCSS_Enabled
};

enum settingsFRSItems
{
    FRS_MODE = 0,
    FRS_RESET_CODES
};

enum settingsDMRItems
{
    DMR_ID = 0,
    DMR_TALKGROUP,
    DMR_CALLTYPE,
    DMR_COLORCODE,
    DMR_TIMESLOT,
    DMR_MONITOR,
    DMR_ACCESS,
    DMR_HANGTIME,
    DMR_TESTTONE,
    DMR_MONITOR_ENTRY
};

// Longest DMR ID or talkgroup entry, in digits: DMR_ID_MAX is 16777215
#define DMR_ID_DIGITS 8

#ifdef CONFIG_DMR
/*
 * Rows of the DMR monitor (bring-up) screen, Settings > DMR > DMR monitor.
 * The rows are grouped in five sections, each starting on a new page and
 * spanning as many pages as the screen height requires: 1. interrupt
 * counters, slot lock and 0x82 flags; 2. status registers; 3. link control;
 * 4. audio layer (vocoder backend and the dmr_audio counters); 5. actions,
 * register writes selected with UP/DOWN and run with ENTER. Register
 * numbers are those of the HR_C6000 CONFIG page.
 */
enum dmrMonitorRows
{
    DMRMON_TS = 0,        // TIME_SLOT_INTER count
    DMRMON_SYS,           // SYS_INTER count
    DMRMON_RFTX,          // RF_TX_INTER count
    DMRMON_PC0,           // EXTI pulses on PC0..PC3
    DMRMON_PC1,
    DMRMON_PC2,
    DMRMON_PC3,
    DMRMON_LOCK,          // slot lock: none, synced, timecode locked
    DMRMON_TIMECODE,      // local timecode
    DMRMON_R82,           // last 0x82 interrupt flags
    DMRMON_R84,           // last 0x84 TX start sub-status
    DMRMON_R86,           // last 0x86 TX end sub-status
    DMRMON_R51,           // last 0x51 data type / CRC / sync class
    DMRMON_R52,           // last 0x52 colour code / CACH
    DMRMON_R5F,           // last 0x5F sync type
    DMRMON_R42,           // last 0x42 slot status
    DMRMON_CC,            // colour code, 0x52[7:4]
    DMRMON_TC,            // CACH timecode, 0x52 bit2
    DMRMON_SYNC,          // sync type, 0x5F[1:0]
    DMRMON_RSSI,          // 0x43:0x44
    DMRMON_FSKERR,        // 0x0F
    DMRMON_RB10,          // register 0x10 read back at draw time
    DMRMON_RB06,          // register 0x06 read back
    DMRMON_RB12,          // register 0x12 read back
    DMRMON_RB14,          // registers 0x14, 0x15, 0x16 read back (own ID)
    DMRMON_LC_LO,         // LC RAM bytes 0-5
    DMRMON_LC_HI,         // LC RAM bytes 6-11
    DMRMON_LC_SRC,        // decoded source
    DMRMON_LC_DST,        // decoded destination
    DMRMON_LC_FLCO,       // decoded FLCO
    DMRMON_LC_FID,        // decoded FID
    DMRMON_LC_CRC,        // chip CRC verdict and parse result
    DMRMON_AUD_VOCODER,   // vocoder backend name
    DMRMON_AUD_RX,        // payloads pushed to the playback queue
    DMRMON_AUD_VARY,      // of which differing from the previous one
    DMRMON_AUD_OUT,       // 480-sample blocks written to the speaker
    DMRMON_AUD_TX,        // payloads encoded from the microphone
    DMRMON_AUD_UNDER,     // silent blocks inserted inside a call
    DMRMON_AUD_OVER,      // payloads dropped from a full queue
    DMRMON_AUD_TXBURSTS,  // own voice bursts sent
    DMRMON_AUD_TXSILENT,  // of which carrying the silence pattern
    DMRMON_AUD_TXUNDER,   // silence sent while the encoder ran
    DMRMON_RFKEYED,       // times the RF stage was keyed for TX
    DMRMON_ACT_RESYNC,    // 0x81 = 0xFE, 0x41 = 0x20
    DMRMON_ACT_MODE,      // 0x10 = 0x6E / 0x6A
    DMRMON_ACT_R06,       // 0x06 = next variant
    DMRMON_ACT_BER,       // 0x83 = 0xFF, 0x81 = 0xFF, 0x10 = 0x02,
                          // 0x40 = 0x40, 0x41 = 0x41
    DMRMON_ACT_TWOPOINT,  // 0x12 bit6 toggle
    DMRMON_ACT_LCSS,      // 0x50 LCSS convention of the voice bursts,
                          // chip-filled / manual, through the RTX status
    DMRMON_ACT_RFADV,     // 0x12 = 0x98 / 0x15 (RF_TX_INTER advance)
    DMRMON_ROWS_NUM
};

// First row of each section of the monitor screen, in order
#define DMRMON_SECTION_NUM   5
#define DMRMON_ACTION_FIRST  DMRMON_ACT_RESYNC
#define DMRMON_ACTION_NUM    (DMRMON_ROWS_NUM - DMRMON_ACT_RESYNC)

// U_SPI page of the HR_C6000 main configuration registers (opmode 4)
#define DMRMON_PAGE_CONFIG   4

// Values register 0x06 cycles through with the "0x06 variant" action
#define DMRMON_R06_VARIANTS  5

// Values of register 0x12 the "RF advance" action alternates between: the
// OEM init word (bit7 RF ramp smoothing + RF_TX_INTER 2.4 ms in advance) and
// the OpenGD77 one (2.1 ms, no smoothing). UNVERIFIED on hardware.
#define DMRMON_R12_OEM       0x98
#define DMRMON_R12_GD77      0x15
#endif

// FRS channel number entry: auto-accept timeout after the first digit, in ms
#define FRS_INPUT_TIMEOUT 2000

// How long the "FRS!" marker stays in the top bar after a refused key, in ms
#define FRS_REFUSE_MARKER_TIME 1000

/**
 * Struct containing a set of positions and sizes that get
 * calculated for the selected display size.
 * Using these parameters make the UI automatically adapt
 * To displays of different sizes
 */
typedef struct layout_t
{
    uint16_t hline_h;
    uint16_t top_h;
    uint16_t line1_h;
    uint16_t line2_h;
    uint16_t line3_h;
    uint16_t line3_large_h;
    uint16_t line4_h;
    uint16_t menu_h;
    uint16_t bottom_h;
    uint16_t bottom_pad;
    uint16_t status_v_pad;
    uint16_t horizontal_pad;
    uint16_t text_v_offset;
    point_t top_pos;
    point_t line1_pos;
    point_t line2_pos;
    point_t line3_pos;
    point_t line3_large_pos;
    point_t line4_pos;
    point_t bottom_pos;
    fontSize_t top_font;
    symbolSize_t top_symbol_size;
    fontSize_t line1_font;
    symbolSize_t line1_symbol_size;
    fontSize_t line2_font;
    symbolSize_t line2_symbol_size;
    fontSize_t line3_font;
    symbolSize_t line3_symbol_size;
    fontSize_t line3_large_font;
    fontSize_t line4_font;
    symbolSize_t line4_symbol_size;
    fontSize_t bottom_font;
    fontSize_t input_font;
    fontSize_t menu_font;
    fontSize_t message_font;
} layout_t;

/**
 * This structs contains state variables internal to the
 * UI that need to be kept between executions of the UI
 * This state does not need to be saved on device poweroff
 */
typedef struct ui_state_t
{
    // Index of the currently selected menu entry
    uint8_t menu_selected;
    // If true we can change a menu entry value with UP/DOWN
    bool edit_mode;
    bool input_locked;
    // Variables used for VFO input
    uint8_t input_number;
    uint8_t input_position;
    uint8_t input_set;
    long long last_keypress;
    freq_t new_rx_frequency;
    freq_t new_tx_frequency;
    char new_rx_freq_buf[14];
    char new_tx_freq_buf[14];
    size_t m17_meta_text_scroll_position;
    long long m17_meta_text_last_scroll_tick;
    char new_meta_text[53];
    bool edit_meta_text;
#ifdef CONFIG_RTC
    // Variables used for Time & Date input
    datetime_t new_timedate;
    char new_date_buf[9];
    char new_time_buf[9];
#endif
    char new_callsign[10];
    // DMR ID or talkgroup being typed, with the number of digits entered
    uint32_t new_dmr_number;
    uint8_t new_dmr_digits;
#ifdef CONFIG_DMR
    // DMR monitor screen: page shown, last 0x10 value written by the "Mode"
    // action and index of the next "0x06 variant" value
    uint8_t dmr_mon_page;
    uint8_t dmr_mon_modeReg;
    uint8_t dmr_mon_r06;
    uint8_t dmr_mon_lcss;
#endif
    freq_t new_offset;
    // Which state to return to when we exit menu
    uint8_t last_main_state;
#if defined(CONFIG_UI_NO_KEYBOARD)
    uint8_t macro_menu_selected;
#endif // UI_NO_KEYBOARD
}
ui_state_t;

extern layout_t layout;
extern state_t last_state;
extern bool    macro_latched;
// Tick of the last key refused by FRS mode, zero when none
extern long long frs_refuse_tick;

/**
 * Check whether the marker for a key refused by FRS mode is to be shown.
 *
 * @param now: current tick.
 * @return true while the last refusal is younger than FRS_REFUSE_MARKER_TIME.
 */
static inline bool _ui_frsRefuseMarkerVisible(long long now)
{
    return (frs_refuse_tick != 0) &&
           ((now - frs_refuse_tick) < FRS_REFUSE_MARKER_TIME);
}

#ifdef CONFIG_DMR
/**
 * Text of the DMR top-bar marker for the given RTX status, from the marker
 * flags the DMR opMode handler raises. When several are set the one the user
 * can act upon first wins: a radio without a DMR modem, a missing ID, a busy
 * channel, a repeater that did not wake up.
 *
 * @param status: current RTX status.
 * @return marker text, or NULL when no marker is to be shown.
 */
static inline const char *_ui_dmrMarkerText(uint8_t markers)
{
    if(markers & DMR_MARK_NOT_SUPPORTED)
        return currentLanguage->dmrNotSupported;
    if(markers & DMR_MARK_NO_ID)
        return currentLanguage->dmrNoId;
    if(markers & DMR_MARK_BUSY)
        return currentLanguage->dmrBusy;
    if(markers & DMR_MARK_WAKEUP_FAILED)
        return currentLanguage->dmrWakeupFailed;
    return NULL;
}

#endif

/**
 * Prefix of the destination line of the main screen while transmitting:
 * "TX -> " during a transmission, "WAKE " while a repeater is being woken
 * up (the wake-up CSBK is on the air and the repeater's answer awaited; no
 * arrow, "WAKE -> PC 16777215" would not fit the 160 pixel line), an empty
 * string otherwise. The small screens use the compact form.
 *
 * @param callState: rtxStatus_t::dmr_callState.
 * @param compact: true for the 128x64 layout.
 * @return prefix, never NULL.
 */
static inline const char *_ui_dmrTxPrefix(uint8_t callState, bool compact)
{
    switch(callState)
    {
        case DMR_CALL_TX:
            return compact ? "TX->" : "TX -> ";
        case DMR_CALL_TX_WAKEUP:
            return "WAKE ";
        default:
            return "";
    }
}

extern const char *menu_items[];
extern const char *settings_items[];
extern const char *display_items[];
extern const char *settings_gps_items[];
extern const char *settings_radio_items[];
extern const char *settings_m17_items[];
extern const char *settings_fm_items[];
extern const char *settings_frs_items[];
extern const char *settings_dmr_items[];
extern const char * settings_accessibility_items[];
extern const char *backup_restore_items[];
extern const char *info_items[];
extern const char *authors[];
extern const uint8_t menu_num;
extern const uint8_t settings_num;
extern const uint8_t display_num;
extern const uint8_t settings_gps_num;
extern const uint8_t settings_radio_num;
extern const uint8_t settings_m17_num;
extern const uint8_t settings_fm_num;
extern const uint8_t settings_frs_num;
extern const uint8_t settings_dmr_num;
extern const uint8_t settings_accessibility_num;
extern const uint8_t backup_restore_num;
extern const uint8_t info_num;
extern const uint8_t author_num;

/**
 * Check whether the DMR opMode handler reports a call being received: while it
 * does, the main screen shows the caller in place of the frequency.
 *
 * @param status: rtx status.
 * @return true during a received call and its hang time.
 */
static inline bool _ui_dmrCallReceived(const rtxStatus_t *status)
{
    return (status->dmr_lcOk != false) &&
           ((status->dmr_callState == DMR_CALL_RX) ||
            (status->dmr_callState == DMR_CALL_RX_HANG));
}

/**
 * Draw the DMR lines of the main screen: destination, colour code and
 * timeslot when idle or transmitting, caller and destination during a
 * received call. Implemented in ui_main.c, exposed for the unit tests.
 *
 * @param ui_state: UI state, for the number being typed.
 * @param status: rtx status the call information is taken from.
 */
void _ui_drawModeInfoDMR(ui_state_t *ui_state, const rtxStatus_t *status);

#ifdef CONFIG_DMR
/**
 * Number of pages of the DMR monitor screen for the current layout: every
 * section of enum dmrMonitorRows starts on a new page and spans as many
 * pages as its rows need.
 *
 * @return page count, at least DMRMON_SECTION_NUM.
 */
uint8_t _ui_dmrMonitorPages(void);

/**
 * Rows shown by a page of the DMR monitor screen.
 *
 * @param page: page number, 0-based, wrapped into the page count.
 * @param first: set to the first row of the page (enum dmrMonitorRows).
 * @param count: set to the number of rows of the page.
 */
void _ui_dmrMonitorPageRows(uint8_t page, uint8_t *first, uint8_t *count);

/**
 * Label of a row of the DMR monitor screen, list getter signature.
 */
int _ui_getDMRMonitorEntryName(char *buf, uint8_t max_len, uint8_t index);

/**
 * Value of a row of the DMR monitor screen, list getter signature: from the
 * rtx status, the register readback and the UI state captured by the last
 * _ui_drawDMRMonitor().
 */
int _ui_getDMRMonitorValueName(char *buf, uint8_t max_len, uint8_t index);

/**
 * Draw the DMR monitor screen. Implemented in ui_menu.c.
 *
 * @param ui_state: UI state, for the page and the selected action.
 */
void _ui_drawDMRMonitor(ui_state_t *ui_state);

/**
 * @return the page drawn by the last _ui_drawDMRMonitor(), 0-based.
 */
uint8_t _ui_dmrMonitorCurrentPage(void);

/**
 * @return the action highlighted by the last _ui_drawDMRMonitor(), as an
 * offset from DMRMON_ACTION_FIRST.
 */
uint8_t _ui_dmrMonitorSelectedAction(void);
#endif

extern const color_t color_black;
extern const color_t color_grey;
extern const color_t color_white;
extern const color_t yellow_fab413;

#endif /* UI_DEFAULT_H */
