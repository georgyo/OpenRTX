/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "emulator/emulator.h"
#include "interfaces/radio.h"
#include <cstdio>
#include <cstring>
#include <string>

static struct radioEmuRecord record = { 0, 0, 0, false };
static enum opstatus status = OFF;
static bool txFails = false;

const struct radioEmuRecord *radioEmu_record()
{
    return &record;
}

void radioEmu_reset()
{
    memset(&record, 0x00, sizeof(record));
    status = OFF;
    txFails = false;
}

void radioEmu_setTxFails(bool fail)
{
    txFails = fail;
}

void radio_init(const rtxStatus_t *rtxState)
{
    (void) rtxState;
    puts("radio_linux: init() called");
}

void radio_terminate()
{
    puts("radio_linux: terminate() called");
}

void radio_setOpmode(const enum opmode mode)
{
    std::string mStr(" ");
    if(mode == OPMODE_NONE) mStr = "NONE";
    if(mode == OPMODE_FM)   mStr = "FM";
    if(mode == OPMODE_DMR)  mStr = "DMR";
    if(mode == OPMODE_M17)  mStr = "M17";

    printf("radio_linux: setting opmode to %s\n", mStr.c_str());
}

bool radio_checkRxDigitalSquelch()
{
//     puts("radio_linux: radio_checkRxDigitalSquelch(), returning 'false'");
    return false;
}

void radio_enableRx()
{
    record.enableRx++;
    record.keyed = false;
    status = RX;
    puts("radio_linux: enableRx() called");
}

void radio_enableTx()
{
    record.enableTx++;

    /* As the CS7000 driver on a PLL lock failure: nothing keyed, OFF */
    if(txFails)
    {
        record.keyed = false;
        status = OFF;
        puts("radio_linux: enableTx() called, refused");
        return;
    }

    record.keyed = true;
    status = TX;
    puts("radio_linux: enableTx() called");
}

void radio_disableRtx()
{
    record.disableRtx++;
    record.keyed = false;
    status = OFF;
    puts("radio_linux: disableRtx() called");
}

void radio_updateConfiguration()
{
    puts("radio_linux: updateConfiguration() called");
}

rssi_t radio_getRssi()
{
    // Commented to reduce verbosity on Linux
    // printf("radio_linux: requested RSSI at freq %d, returning -100dBm\n", rxFreq);
    return static_cast< rssi_t >(emulator_state.RSSI);
}

enum opstatus radio_getStatus()
{
    return status;
}
