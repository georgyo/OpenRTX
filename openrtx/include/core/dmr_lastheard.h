/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_LASTHEARD_H
#define DMR_LASTHEARD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * DMR "last heard" list: a RAM ring of the calls received since power on,
 * newest first, filled by the DMR opMode handler on every call header
 * (once per call, not per burst) and read by the UI. A call from the same
 * source to the same destination as an entry already in the list moves
 * that entry to the top with its timestamp refreshed, so that the list
 * holds distinct calls rather than a log. Not persisted.
 *
 * The insert side runs on the rtx thread and the read side on the UI
 * thread: the functions serialise on a mutex.
 */

/**
 * Depth of the list.
 */
#define DMR_LASTHEARD_NUM 8

/**
 * One received call.
 */
struct dmrLastHeardEntry {
    uint32_t src;       /**< Source address of the call, 24 bits         */
    uint32_t dst;       /**< Destination address, 24 bits                */
    uint8_t flco;       /**< FLCO of the call: 0 group, 3 unit-to-unit   */
    uint32_t timestamp; /**< getTick() of the last header, milliseconds  */
};

/**
 * Initialise the list: empty. Called once at startup.
 */
void dmrLastHeard_init(void);

/**
 * Empty the list.
 */
void dmrLastHeard_clear(void);

/**
 * Insert a call at the top of the list. An entry with the same source and
 * destination is moved to the top and its timestamp refreshed instead of
 * being duplicated; when the list is full the oldest entry is dropped.
 *
 * @param src: source address.
 * @param dst: destination address.
 * @param flco: FLCO of the call.
 * @param timestamp: current time, milliseconds.
 */
void dmrLastHeard_insert(uint32_t src, uint32_t dst, uint8_t flco,
                         uint32_t timestamp);

/**
 * @return number of entries held, 0 to DMR_LASTHEARD_NUM.
 */
uint8_t dmrLastHeard_count(void);

/**
 * Read one entry.
 *
 * @param index: 0 for the newest entry, up to dmrLastHeard_count() - 1.
 * @param entry: destination for the entry.
 * @return true when the index holds an entry, false otherwise (entry is
 * left untouched).
 */
bool dmrLastHeard_get(uint8_t index, struct dmrLastHeardEntry *entry);

#ifdef __cplusplus
}
#endif

#endif /* DMR_LASTHEARD_H */
