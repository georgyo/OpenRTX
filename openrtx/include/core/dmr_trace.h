/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_TRACE_H
#define DMR_TRACE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hwconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Trace of the DMR modem for the bring-up on a radio without a debugger:
 * the DMR opMode handler records what the modem reports and what it is told
 * to do, the device thread sends the records as text lines over the USB
 * serial port. Built only with CONFIG_DMR_TRACE, every call is a no-op
 * otherwise.
 *
 * One record is one line: the kind, a single letter, then the time of the
 * record as six hexadecimal digits (milliseconds, 24 bits), then the data
 * bytes as two hexadecimal digits each, then a line feed. No separators, so
 * that the longest line fits one full-speed USB packet. The data of each
 * kind is laid out by its writer, OpMode_DMR.cpp, and described in
 * docs/dmr_bringup.md; scripts/dmr_trace.py decodes a capture.
 *
 * Records written while nobody reads the port are dropped. The line "D"
 * carries the number of records lost because the buffer was full.
 */

/** Data bytes of one record, at most. */
#define DMR_TRACE_DATA_BYTES 27

/** Bytes of the longest line, line feed included, without terminator. */
#define DMR_TRACE_LINE_BYTES (1 + 6 + (2 * DMR_TRACE_DATA_BYTES) + 1)

#ifdef CONFIG_DMR_TRACE

/**
 * Append one record. To be called by one thread only, the rtx thread: it
 * never blocks, a record that does not fit is counted as lost.
 *
 * @param kind: record kind, a printable character.
 * @param ms: time of the record, milliseconds.
 * @param data: data bytes.
 * @param len: number of data bytes, DMR_TRACE_DATA_BYTES at most.
 */
void dmrTrace_put(char kind, uint32_t ms, const uint8_t *data, size_t len);

/**
 * Take the oldest record out as a text line. To be called by one thread
 * only, the one sending the lines out.
 *
 * @param line: destination, DMR_TRACE_LINE_BYTES at least.
 * @param max: size of the destination.
 * @return length of the line, zero when there is none.
 */
size_t dmrTrace_pop(char *line, size_t max);

/**
 * Ask the writer for a fresh description of the configuration: a reader has
 * just appeared and missed the one written when the mode was entered.
 */
void dmrTrace_requestDump(void);

/**
 * Tell whether a description of the configuration was asked for since the
 * last call.
 *
 * @return true once per request.
 */
bool dmrTrace_dumpRequested(void);

/**
 * Send the pending records to the USB serial port, a few per call. Called
 * by the device thread at every pass; does nothing on a target without the
 * port.
 */
void dmrTrace_task(void);

#else

static inline void dmrTrace_put(char kind, uint32_t ms, const uint8_t *data,
                                size_t len)
{
    (void)kind;
    (void)ms;
    (void)data;
    (void)len;
}

static inline bool dmrTrace_dumpRequested(void)
{
    return false;
}

static inline void dmrTrace_task(void)
{
}

#endif /* CONFIG_DMR_TRACE */

#ifdef __cplusplus
}
#endif

#endif /* DMR_TRACE_H */
