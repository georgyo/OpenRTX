/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/dmr_lastheard.h"
#include <pthread.h>
#include <string.h>

/*
 * The ring is kept ordered, entry 0 being the newest: an insertion shifts
 * the older entries down. Eight entries of 16 bytes make that cheaper than
 * a circular index would make the reads.
 */
static struct dmrLastHeardEntry entries[DMR_LASTHEARD_NUM];
static uint8_t count = 0;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

void dmrLastHeard_init(void)
{
    dmrLastHeard_clear();
}

void dmrLastHeard_clear(void)
{
    pthread_mutex_lock(&mutex);
    memset(entries, 0x00, sizeof(entries));
    count = 0;
    pthread_mutex_unlock(&mutex);
}

void dmrLastHeard_insert(uint32_t src, uint32_t dst, uint8_t flco,
                         uint32_t timestamp)
{
    pthread_mutex_lock(&mutex);

    /*
     * A call already in the list moves to the top: the entries above it
     * shift down by one. A new call shifts the whole list down, dropping
     * the last entry when the list is full.
     */
    uint8_t slot = count;
    for (uint8_t i = 0; i < count; i++) {
        if ((entries[i].src == src) && (entries[i].dst == dst)) {
            slot = i;
            break;
        }
    }

    if (slot >= DMR_LASTHEARD_NUM)
        slot = DMR_LASTHEARD_NUM - 1;
    else if (slot == count)
        count++;

    for (uint8_t i = slot; i > 0; i--)
        entries[i] = entries[i - 1];

    entries[0].src = src;
    entries[0].dst = dst;
    entries[0].flco = flco;
    entries[0].timestamp = timestamp;

    pthread_mutex_unlock(&mutex);
}

uint8_t dmrLastHeard_count(void)
{
    pthread_mutex_lock(&mutex);
    uint8_t n = count;
    pthread_mutex_unlock(&mutex);

    return n;
}

bool dmrLastHeard_get(uint8_t index, struct dmrLastHeardEntry *entry)
{
    bool found = false;

    pthread_mutex_lock(&mutex);
    if (index < count) {
        *entry = entries[index];
        found = true;
    }
    pthread_mutex_unlock(&mutex);

    return found;
}
