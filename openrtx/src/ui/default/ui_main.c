/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/platform.h"
#include "interfaces/cps_io.h"
#include "interfaces/delays.h"
#include <stdio.h>
#include <stdint.h>
#include "ui/ui_default.h"
#include <string.h>
#include "ui/ui_strings.h"
#include "core/utils.h"
#include "ui/utils.h"

void _ui_drawMainBackground()
{
    // Print top bar line of hline_h pixel height
    gfx_drawHLine(layout.top_h, layout.hline_h, color_grey);
    // Print bottom bar line of 1 pixel height
    gfx_drawHLine(CONFIG_SCREEN_HEIGHT - layout.bottom_h - 1, layout.hline_h, color_grey);
}

void _ui_drawMainTop(ui_state_t * ui_state)
{
#ifdef CONFIG_RTC
    // Print clock on top bar
    datetime_t local_time = utcToLocalTime(last_state.time,
                                           last_state.settings.utc_timezone);
    gfx_print(layout.top_pos, layout.top_font, TEXT_ALIGN_CENTER,
              color_white, "%02d:%02d:%02d", local_time.hour,
              local_time.minute, local_time.second);
#endif
    // If the radio has no built-in battery, print input voltage
#ifdef CONFIG_BAT_NONE
    gfx_print(layout.top_pos, layout.top_font, TEXT_ALIGN_RIGHT,
              color_white,"%u.%uV", last_state.v_bat/1000, (last_state.v_bat % 1000) /100);
#else
    if(last_state.settings.showBatteryIcon) {
        // print battery icon on top bar, use 4 px padding
        uint16_t bat_width = CONFIG_SCREEN_WIDTH / 9;
        uint16_t bat_height = layout.top_h - (layout.status_v_pad * 2);
        point_t bat_pos = {CONFIG_SCREEN_WIDTH - bat_width - layout.horizontal_pad,
                        layout.status_v_pad};
        gfx_drawBattery(bat_pos, bat_width, bat_height, last_state.charge);
    } else {
        // print the battery percentage
        point_t bat_pos = {layout.top_pos.x, layout.top_pos.y - 2};
        gfx_print(bat_pos , FONT_SIZE_6PT, TEXT_ALIGN_RIGHT,
        color_white,"%d%%", last_state.charge);
    }
#endif
    if (ui_state->input_locked == true)
      gfx_drawSymbol(layout.top_pos, layout.top_symbol_size, TEXT_ALIGN_LEFT,
                     color_white, SYMBOL_LOCK);
}

void _ui_drawBankChannel()
{
    // Print Bank number, channel number and Channel name
    uint16_t b = (last_state.bank_enabled) ? last_state.bank : 0;
    gfx_print(layout.line1_pos, layout.line1_font, TEXT_ALIGN_CENTER,
              color_white, "%01d-%03d: %.12s",
              b, last_state.channel_index + 1, last_state.channel.name);
}

const char* _ui_getToneEnabledString(bool tone_tx_enable, bool tone_rx_enable,
        bool use_abbreviation)
{
    const char *strings[2][4] = {
        {
            currentLanguage->None,
            currentLanguage->Encode,
            currentLanguage->Decode,
            currentLanguage->Both,
        }, {
            "N",
            "T",
            "R",
            "B"
        }
    };


    uint8_t index = (tone_rx_enable << 1) | (tone_tx_enable);
    return strings[use_abbreviation][index];
}

/*
 * Destination of a DMR call as shown on screen: "TG 31665" for a group call,
 * "PC 2345678" for a private call, "ALL" for a broadcast. The small screens
 * use the compact form without the space.
 */
static void _ui_dmrDestString(char *buf, size_t len, uint8_t callType,
                              uint32_t id)
{
    #if CONFIG_SCREEN_HEIGHT > 127
    static const char *sep = " ";
    #else
    static const char *sep = "";
    #endif

    switch(callType)
    {
        case GROUP:
            sniprintf(buf, len, "TG%s%lu", sep, (unsigned long) id);
            break;

        case PRIVATE:
            sniprintf(buf, len, "PC%s%lu", sep, (unsigned long) id);
            break;

        default:
            sniprintf(buf, len, "%s", currentLanguage->broadcast);
            break;
    }
}

/*
 * Destination being typed after '#': "TG 123_", with the prefix of the
 * current call type.
 */
static void _ui_dmrDestInputString(char *buf, size_t len, ui_state_t* ui_state)
{
    const char *prefix = "TG";
    if(last_state.settings.dmr_callType == PRIVATE)
        prefix = "PC";
    else if(last_state.settings.dmr_callType == ALL)
        prefix = "ID";

    if(ui_state->new_dmr_digits == 0)
        sniprintf(buf, len, "%s _", prefix);
    else
        sniprintf(buf, len, "%s %lu_", prefix,
                  (unsigned long) ui_state->new_dmr_number);
}

void _ui_drawModeInfoDMR(ui_state_t *ui_state, const rtxStatus_t *status)
{
    const channel_t *ch = &last_state.channel;
    char dst[16] = { 0 };
    char slot[16] = { 0 };

    if(_ui_dmrCallReceived(status))
    {
        // Call type of the received call from its FLCO and destination:
        // unit-to-unit is a private call, the all-ones ID a broadcast
        uint8_t callType = GROUP;
        if(status->dmr_rxFlco == 3)
            callType = PRIVATE;
        else if(status->dmr_rxDstId == DMR_ID_MAX)
            callType = ALL;

        _ui_dmrDestString(dst, sizeof(dst), callType, status->dmr_rxDstId);
        sniprintf(slot, sizeof(slot), "CC%d TS%d",
                  status->dmr_rxColorCodeSeen, status->dmr_rxTimeslot);

        #if CONFIG_SCREEN_HEIGHT > 127
        // Caller on line 1, destination on line 2, channel on line 3: the
        // frequency is not drawn during a received call
        gfx_drawSymbol(layout.line1_pos, layout.line1_symbol_size,
                       TEXT_ALIGN_LEFT, color_white, SYMBOL_CALL_RECEIVED);
        gfx_print(layout.line1_pos, layout.line1_font, TEXT_ALIGN_CENTER,
                  color_white, "%lu", (unsigned long) status->dmr_rxSrcId);
        gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                  color_white, "-> %s", dst);
        gfx_print(layout.line3_pos, layout.line3_font, TEXT_ALIGN_CENTER,
                  color_white, "%s", slot);
        #else
        // Caller on line 2, destination on line 3
        gfx_drawSymbol(layout.line2_pos, layout.line2_symbol_size,
                       TEXT_ALIGN_LEFT, color_white, SYMBOL_CALL_RECEIVED);
        gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                  color_white, "%lu", (unsigned long) status->dmr_rxSrcId);
        gfx_print(layout.line3_pos, layout.line3_font, TEXT_ALIGN_CENTER,
                  color_white, "-> %s", dst);
        #endif

        return;
    }

    // Idle or transmitting: destination, colour code and timeslot of the
    // channel. While transmitting the destination is the one the RTX stage
    // is sending to.
    bool tx = (status->dmr_callState == DMR_CALL_TX) ||
              (status->dmr_callState == DMR_CALL_TX_WAKEUP);

    if(ui_state->edit_mode)
        _ui_dmrDestInputString(dst, sizeof(dst), ui_state);
    else if(tx)
        _ui_dmrDestString(dst, sizeof(dst), status->dmr_callType,
                          status->dmr_dstId);
    else
        _ui_dmrDestString(dst, sizeof(dst), last_state.settings.dmr_callType,
                          last_state.settings.dmr_talkgroup);

    #if CONFIG_SCREEN_HEIGHT > 127
    sniprintf(slot, sizeof(slot), "CC%d TS%d%s", ch->dmr.rxColorCode,
              ch->dmr.dmr_timeslot,
              (last_state.settings.dmr_monitor != 0) ? " MON" : "");
    gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_LEFT,
              color_white, "%s%s", tx ? "TX -> " : "", dst);
    gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_RIGHT,
              color_white, "%s", slot);
    #else
    sniprintf(slot, sizeof(slot), "C%dT%d%s", ch->dmr.rxColorCode,
              ch->dmr.dmr_timeslot,
              (last_state.settings.dmr_monitor != 0) ? " M" : "");
    gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
              color_white, "%s%s %s", tx ? "TX->" : "", dst, slot);
    #endif
}

/*
 * Check whether a digital call is being received: the main screens then show
 * the caller in place of the frequency and channel name.
 */
static bool _ui_callReceived()
{
    rtxStatus_t status = rtx_getCurrentStatus();

    #ifdef CONFIG_M17
    if((status.opMode == OPMODE_M17) && (status.lsfOk != false))
        return true;
    #endif

    if((status.opMode == OPMODE_DMR) && _ui_dmrCallReceived(&status))
        return true;

    return false;
}

void _ui_drawModeInfo(ui_state_t* ui_state)
{
    char bw_str[8] = { 0 };

    switch(last_state.channel.mode)
    {
        case OPMODE_FM:

            // Get Bandwidth string
            if(last_state.channel.bandwidth == BW_12_5)
                sniprintf(bw_str, 8, "NFM");
            else if(last_state.channel.bandwidth == BW_25)
                sniprintf(bw_str, 8, "FM");

            // Get encdec string
            bool tone_tx_enable = last_state.channel.fm.txToneEn;
            bool tone_rx_enable = last_state.channel.fm.rxToneEn;
            // Print Bandwidth, Tone and encdec info
            if (tone_tx_enable || tone_rx_enable)
            {
                uint16_t tone = ctcss_tone[last_state.channel.fm.txTone];
                gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                          color_white, "%s %d.%d %s", bw_str, (tone / 10),
                          (tone % 10), _ui_getToneEnabledString(tone_tx_enable, tone_rx_enable, true));
            }
            else
            {
                gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                          color_white, "%s", bw_str );
            }
            break;

        case OPMODE_DMR:
        {
            rtxStatus_t rtxStatus = rtx_getCurrentStatus();
            _ui_drawModeInfoDMR(ui_state, &rtxStatus);
            break;
        }

        #ifdef CONFIG_M17
        case OPMODE_M17:
        {
            // Print M17 Destination ID on line 3 of 3
            rtxStatus_t rtxStatus = rtx_getCurrentStatus();

            if(rtxStatus.lsfOk)
            {
                // Destination address
                gfx_drawSymbol(layout.line2_pos, layout.line2_symbol_size, TEXT_ALIGN_LEFT,
                               color_white, SYMBOL_CALL_RECEIVED);

                gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                          color_white, "%s", rtxStatus.M17_dst);

                // Source address
                gfx_drawSymbol(layout.line1_pos, layout.line1_symbol_size, TEXT_ALIGN_LEFT,
                               color_white, SYMBOL_CALL_MADE);

                gfx_print(layout.line1_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                          color_white, "%s", rtxStatus.M17_src);

                // RF link (if present)
                if(rtxStatus.M17_link[0] != '\0')
                {
                    gfx_drawSymbol(layout.line4_pos, layout.line3_symbol_size, TEXT_ALIGN_LEFT,
                                   color_white, SYMBOL_ACCESS_POINT);

                    gfx_print(layout.line4_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                              color_white, "%s", rtxStatus.M17_link);
                }

                // Meta text (if present)
                if(rtxStatus.M17_meta_text[0] != '\0')
                {
                    char msg[14];
                    
                    // Always display current position
                    scrollTextPeek(rtxStatus.M17_meta_text, msg, sizeof(msg),
                                   ui_state->m17_meta_text_scroll_position);
                    // Only advance scroll every 100ms
                    long long now = getTick();
                    if(now - ui_state->m17_meta_text_last_scroll_tick >= 100)
                    {
                        scrollTextAdvance(rtxStatus.M17_meta_text, sizeof(msg),
                                          &ui_state->m17_meta_text_scroll_position);
                        ui_state->m17_meta_text_last_scroll_tick = now;
                    }
                    gfx_print(layout.line3_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                              color_white, "%s", msg);
                }
                // Reflector (if present)
                else if(rtxStatus.M17_refl[0] != '\0')
                {                    
                    gfx_drawSymbol(layout.line3_pos, layout.line4_symbol_size, TEXT_ALIGN_LEFT,
                                   color_white, SYMBOL_NETWORK);

                    gfx_print(layout.line3_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                              color_white, "%s", rtxStatus.M17_refl);
                }
                
                // Reset scroll position when meta text becomes empty
                if(rtxStatus.M17_meta_text[0] == '\0')
                {
                    ui_state->m17_meta_text_scroll_position = 0;
                }
            }
            else
            {
                const char *dst = NULL;
                if(ui_state->edit_mode)
                {
                    dst = ui_state->new_callsign;
                }
                else
                {
                    if(strnlen(rtxStatus.destination_address, 10) == 0)
                        dst = currentLanguage->broadcast;
                    else
                        dst = rtxStatus.destination_address;
                }

                gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
                          color_white, "M17 #%s", dst);
            }
            break;
        }
        #endif
    }
}

void _ui_drawFrequency()
{
    freq_t freq = platform_getPttStatus() ? last_state.channel.tx_frequency
                                          : last_state.channel.rx_frequency;

    char freq_str[16] = {0};
    sniprintf(freq_str, sizeof(freq_str), "%03lu.%05lu",
              (freq / 1000000lu), (freq % 1000000lu) / 10);

    size_t len = strlen(freq_str);
    char main_str[16] = {0};
    char small_str[3] = {0};
    strncpy(main_str, freq_str, len - 2);
    strncpy(small_str, freq_str + len - 2, 2);

    fontSize_t small_font = FONT_SIZE_5PT;
    if (layout.line3_large_font > FONT_SIZE_6PT)
    {
        small_font = (fontSize_t)(layout.line3_large_font - 2);
    }

    uint16_t main_width = gfx_getTextWidth(layout.line3_large_font, main_str);
    uint16_t small_width = gfx_getTextWidth(small_font, small_str);
    uint16_t total_width = main_width + small_width;
    int16_t start_x = (CONFIG_SCREEN_WIDTH - total_width) / 2;

    point_t main_pos = { (uint16_t)start_x, layout.line3_large_pos.y };
    gfx_print(main_pos, layout.line3_large_font, TEXT_ALIGN_LEFT,
              color_white, "%s", main_str);

    point_t small_pos = { (uint16_t)(start_x + main_width), layout.line3_large_pos.y };
    gfx_print(small_pos, small_font, TEXT_ALIGN_LEFT,
              color_white, "%s", small_str);
}

void _ui_drawVFOMiddleInput(ui_state_t* ui_state)
{
    // Add inserted number to string, skipping "Rx: "/"Tx: " and "."
    uint8_t insert_pos = ui_state->input_position + 3;
    if(ui_state->input_position > 3) insert_pos += 1;
    char input_char = ui_state->input_number + '0';

    if(ui_state->input_set == SET_RX)
    {
        if(ui_state->input_position == 0)
        {
            gfx_print(layout.line2_pos, layout.input_font, TEXT_ALIGN_CENTER,
                      color_white, ">Rx:%03lu.%04lu",
                      (unsigned long)ui_state->new_rx_frequency/1000000,
                      (unsigned long)(ui_state->new_rx_frequency%1000000)/100);
        }
        else
        {
            // Replace Rx frequency with underscorses
            if(ui_state->input_position == 1)
                strcpy(ui_state->new_rx_freq_buf, ">Rx:___.____");
            ui_state->new_rx_freq_buf[insert_pos] = input_char;
            gfx_print(layout.line2_pos, layout.input_font, TEXT_ALIGN_CENTER,
                      color_white, ui_state->new_rx_freq_buf);
        }
        gfx_print(layout.line3_large_pos, layout.input_font, TEXT_ALIGN_CENTER,
                  color_white, " Tx:%03lu.%04lu",
                  (unsigned long)last_state.channel.tx_frequency/1000000,
                  (unsigned long)(last_state.channel.tx_frequency%1000000)/100);
    }
    else if(ui_state->input_set == SET_TX)
    {
        gfx_print(layout.line2_pos, layout.input_font, TEXT_ALIGN_CENTER,
                  color_white, " Rx:%03lu.%04lu",
                  (unsigned long)ui_state->new_rx_frequency/1000000,
                  (unsigned long)(ui_state->new_rx_frequency%1000000)/100);
        // Replace Rx frequency with underscorses
        if(ui_state->input_position == 0)
        {
            gfx_print(layout.line3_large_pos, layout.input_font, TEXT_ALIGN_CENTER,
                      color_white, ">Tx:%03lu.%04lu",
                      (unsigned long)ui_state->new_rx_frequency/1000000,
                      (unsigned long)(ui_state->new_rx_frequency%1000000)/100);
        }
        else
        {
            if(ui_state->input_position == 1)
                strcpy(ui_state->new_tx_freq_buf, ">Tx:___.____");
            ui_state->new_tx_freq_buf[insert_pos] = input_char;
            gfx_print(layout.line3_large_pos, layout.input_font, TEXT_ALIGN_CENTER,
                      color_white, ui_state->new_tx_freq_buf);
        }
    }
}

void _ui_drawMainBottom()
{
    // Squelch bar
    rssi_t   rssi = last_state.rssi;
    uint8_t  squelch = last_state.settings.sqlLevel;
    uint8_t  volume = last_state.volume;
    uint16_t meter_width = CONFIG_SCREEN_WIDTH - 2 * layout.horizontal_pad;
    uint16_t meter_height = layout.bottom_h;
    point_t meter_pos = { layout.horizontal_pad,
                          CONFIG_SCREEN_HEIGHT - meter_height - layout.bottom_pad};
    uint8_t mic_level = platform_getMicLevel();
    switch(last_state.channel.mode)
    {
        case OPMODE_FM:
            gfx_drawSmeter(meter_pos,
                           meter_width,
                           meter_height,
                           rssi,
                           squelch,
                           volume,
                           true,
                           yellow_fab413);
            break;
        case OPMODE_DMR:
            gfx_drawSmeterLevel(meter_pos,
                                meter_width,
                                meter_height,
                                rssi,
                                mic_level,
                                volume,
                                true);
            break;
        #ifdef CONFIG_M17
        case OPMODE_M17:
            gfx_drawSmeterLevel(meter_pos,
                                meter_width,
                                meter_height,
                                rssi,
                                mic_level,
                                volume,
                                true);
            break;
        #endif
    }
}

void _ui_drawMainVFO(ui_state_t* ui_state)
{
    gfx_clearScreen();
    _ui_drawMainTop(ui_state);
    _ui_drawModeInfo(ui_state);

    // Show VFO frequency unless a digital call is being received
    if(_ui_callReceived() == false)
        _ui_drawFrequency();

    _ui_drawMainBottom();
}

void _ui_drawMainVFOInput(ui_state_t* ui_state)
{
    gfx_clearScreen();
    _ui_drawMainTop(ui_state);
    _ui_drawVFOMiddleInput(ui_state);
    _ui_drawMainBottom();
}

/*
 * FRS main screen: privacy code on line 1, mode, frequency and power class
 * on line 2, channel number in the large font. The values come from the
 * FRS channel materialised in last_state.channel.
 */
void _ui_drawMainFRS(ui_state_t* ui_state)
{
    const channel_t *ch = &last_state.channel;
    uint8_t channel = last_state.settings.frs_channel;
    uint8_t code = last_state.settings.frs_codes[channel];

    gfx_clearScreen();
    _ui_drawMainTop(ui_state);

    // Line 1: privacy code and its CTCSS tone
    if((code == 0) || (ch->fm.txToneEn == 0))
    {
        gfx_print(layout.line1_pos, layout.line1_font, TEXT_ALIGN_CENTER,
                  color_white, "%s %s", currentLanguage->code,
                  currentLanguage->off);
    }
    else
    {
        uint16_t tone = ctcss_tone[ch->fm.txTone];
        gfx_print(layout.line1_pos, layout.line1_font, TEXT_ALIGN_LEFT,
                  color_white, "%s %d", currentLanguage->code, code);
        gfx_print(layout.line1_pos, layout.line1_font, TEXT_ALIGN_RIGHT,
                  color_white, "%d.%d Hz %s", (tone / 10), (tone % 10),
                  _ui_getToneEnabledString(ch->fm.txToneEn, ch->fm.rxToneEn,
                                           true));
    }

    // Line 2: bandwidth, channel frequency and power class
    freq_t freq = platform_getPttStatus() ? ch->tx_frequency
                                          : ch->rx_frequency;
    gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_LEFT,
              color_white, (ch->bandwidth == BW_12_5) ? "NFM" : "FM");
    gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_CENTER,
              color_white, "%03lu.%05lu", (unsigned long)(freq / 1000000lu),
              (unsigned long)(freq % 1000000lu) / 10);
    gfx_print(layout.line2_pos, layout.line2_font, TEXT_ALIGN_RIGHT,
              color_white, (ch->power < 1000) ? "Lo" : "Hi");

    // Line 3: channel number
    gfx_print(layout.line3_large_pos, layout.line3_large_font,
              TEXT_ALIGN_CENTER, color_white, "%s %d", currentLanguage->frs,
              channel + 1);

    _ui_drawMainBottom();
}

/*
 * FRS channel number entry: the pending first digit with an underscore for
 * the second one, plus a hint of the valid range.
 */
void _ui_drawMainFRSInput(ui_state_t* ui_state)
{
    gfx_clearScreen();
    _ui_drawMainTop(ui_state);

    gfx_print(layout.line1_pos, layout.line1_font, TEXT_ALIGN_CENTER,
              color_white, currentLanguage->channelRange);
    gfx_print(layout.line3_large_pos, layout.line3_large_font,
              TEXT_ALIGN_CENTER, color_white, "%s %d_", currentLanguage->frs,
              ui_state->input_number);

    _ui_drawMainBottom();
}

void _ui_drawMainMEM(ui_state_t* ui_state)
{
    gfx_clearScreen();
    _ui_drawMainTop(ui_state);
    _ui_drawModeInfo(ui_state);

    // Show channel data unless a digital call is being received
    if(_ui_callReceived() == false)
    {
        _ui_drawBankChannel();
        _ui_drawFrequency();
    }

    _ui_drawMainBottom();
}
