/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef EMULATOR_H
#define EMULATOR_H

#include "interfaces/dmr_baseband.h"
#include "interfaces/keyboard.h"
#include <stdbool.h>
#include <stdint.h>
#include "SDL2/SDL.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CONFIG_SCREEN_WIDTH
#define CONFIG_SCREEN_WIDTH 160
#endif

#ifndef CONFIG_SCREEN_HEIGHT
#define CONFIG_SCREEN_HEIGHT 128
#endif

enum choices {
    VAL_RSSI = 1,
    VAL_BAT,
    VAL_MIC,
    VAL_VOL,
    VAL_CH,
    VAL_PTT,
    PRINT_STATE,
    EXIT
};

typedef struct {
    float RSSI;
    float vbat;
    float micLevel;
    float volumeLevel;
    float chSelector;
    bool PTTstatus;
    bool powerOff;
    uint32_t dmrCallSrc; /* --dmr-call source ID, 0 when not given */
    uint32_t dmrCallDst; /* --dmr-call destination talkgroup       */
} emulator_state_t;

extern emulator_state_t emulator_state;

/**
 * Parse the emulator command line options, before openrtx_init():
 *  --dmr-call src,dst   replay a synthetic DMR group call from radio ID
 *                       src to talkgroup dst while the radio is in DMR
 *                       mode; the OPENRTX_DMR_CALL environment variable
 *                       carries the same value.
 *
 * @param argc: argument count from main().
 * @param argv: argument vector from main().
 */
void emulator_parseArgs(int argc, char **argv);

void emulator_start();

keyboard_t emulator_getKeys();

/*
 * Fake DMR modem, platform/drivers/baseband/dmr_linux.cpp: implements the
 * dmr_baseband.h interface with a script queue. In the running emulator a
 * 30 ms thread produces the idle timeslot ticks and plays the scripted
 * events at the slot cadence; in the unit tests no thread runs, the test
 * pushes events and dmrbb_waitEvent() returns them at once (or
 * DMRBB_EV_TIMEOUT when the queue is empty).
 */

/**
 * Record of everything the firmware wrote to the fake modem.
 */
struct dmrEmuRecord {
    uint8_t r40;       /* last register 0x40 value (dmrbb_startRx/Tx/idle) */
    uint8_t r41;       /* last dmrbb_setNextSlot() value                   */
    uint8_t r50;       /* last dmrbb_setTxFrameType() value                */
    uint8_t r21;       /* last access policy written by dmrbb_startTx()    */
    uint8_t lc[12];    /* last dmrbb_writeTxLc() payload              */
    uint8_t voice[27]; /* last dmrbb_writeVoice() payload             */
    bool activeTiming; /* last dmrbb_startTx() argument               */
    struct dmrbbConfig config; /* last dmrbb_configure() argument     */

    uint32_t r40Writes;
    uint32_t r41Writes;
    uint32_t r50Writes;
    uint32_t lcWrites;
    uint32_t voiceWrites;
    uint32_t txSlots;     /* dmrbb_setNextSlot(0x80) calls           */
    uint32_t headers;     /* TX slots with 0x50 = voice LC header    */
    uint32_t voiceBursts; /* TX slots with 0x50 bit3 set             */
    uint32_t terminators; /* TX slots with 0x50 = terminator         */
    uint32_t csbks;       /* TX slots with 0x50 = CSBK               */

    uint32_t initCount;
    uint32_t terminateCount;
    uint32_t configureCount;
    uint32_t startRxCount;
    uint32_t startTxCount;
    uint32_t idleCount;

    uint32_t tsDelivered;  /* timeslot events returned by waitEvent  */
    uint32_t sysDelivered; /* system events returned by waitEvent    */
    uint32_t timeouts;     /* DMRBB_EV_TIMEOUT returns               */
};

/**
 * Empty the event and script queues, clear the record, restore the default
 * init result. The real-time clock setting is not touched.
 */
void dmrEmu_reset(void);

/**
 * Fake init failure hook: what dmrbb_init() returns from now on.
 *
 * @param result: 0 for success, -1 to emulate a radio without a DMR modem.
 */
void dmrEmu_setInitResult(int result);

/**
 * Queue one idle timeslot event. The CACH timecode reported in register
 * 0x52 alternates at every timeslot, the colour code is the configured one.
 */
void dmrEmu_pushTs(void);

/**
 * Queue one system event with the given snapshot (irq82, r51, r52, r5f, lc
 * and voice fields are what the state machine reads).
 *
 * @param s: snapshot returned by dmrbb_waitEvent() with DMRBB_EV_SYS.
 */
void dmrEmu_pushSys(const struct dmrbbSnapshot *s);

/**
 * Script a base-station sourced group voice call: three voice LC headers,
 * the given number of voice bursts (A..F cycling) and a terminator, each on
 * its own timeslot with the other slot idle in between. In the unit tests
 * the events are played one timeslot per dmrbb_waitEvent() call; in the
 * emulator at the 30 ms cadence.
 *
 * @param src: source radio ID.
 * @param dst: destination talkgroup.
 * @param cc: colour code of the call.
 * @param ts: timeslot of the call, 1 or 2.
 * @param bursts: number of voice bursts.
 */
void dmrEmu_injectGroupCall(uint32_t src, uint32_t dst, uint8_t cc, uint8_t ts,
                            unsigned bursts);

/**
 * @return the record of the firmware's writes.
 */
const struct dmrEmuRecord *dmrEmu_record(void);

/**
 * @return number of events queued and not yet returned by waitEvent(),
 * scripted ones included.
 */
unsigned dmrEmu_pending(void);

/**
 * Enable the real-time 30 ms clock thread, started by dmrbb_init(). The
 * running emulator enables it, the unit tests leave it off.
 *
 * @param enable: true to run the clock.
 */
void dmrEmu_setClock(bool enable);

/**
 * Ask the clock thread to replay a group call from the emulator shell or
 * the --dmr-call option: colour code and timeslot are the configured ones.
 * The call repeats every few seconds until dmrEmu_requestCall(0, 0).
 *
 * @param src: source radio ID, 0 to stop.
 * @param dst: destination talkgroup.
 */
void dmrEmu_requestCall(uint32_t src, uint32_t dst);

#ifdef __cplusplus
}
#endif

#endif /* EMULATOR_H */
