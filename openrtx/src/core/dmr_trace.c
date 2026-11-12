/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/dmr_trace.h"

#ifdef CONFIG_DMR_TRACE

#include <stdatomic.h>
#include <string.h>

#ifdef CONFIG_USB_SERIAL
#include "interfaces/usb_serial.h"
#endif

/* Records kept while the reader is behind, a power of two */
#define TRACE_RECORDS 256
#define TRACE_MASK (TRACE_RECORDS - 1)

/* Lines sent per pass of the device thread */
#define TRACE_LINES_PER_TASK 4

struct traceRecord {
    uint32_t ms;
    char kind;
    uint8_t len;
    uint8_t data[DMR_TRACE_DATA_BYTES];
};

/*
 * Single writer, single reader: the writer only moves head, the reader only
 * moves tail, both free-running counters whose difference is the fill level.
 */
static struct traceRecord records[TRACE_RECORDS];
static atomic_uint head;
static atomic_uint tail;
static atomic_uint lost;
static atomic_bool dumpRequest;

static const char hexDigits[] = "0123456789ABCDEF";

static size_t putHex(char *dst, uint32_t value, unsigned digits)
{
    for (unsigned i = 0; i < digits; i++) {
        unsigned shift = 4 * (digits - 1 - i);
        dst[i] = hexDigits[(value >> shift) & 0x0F];
    }

    return digits;
}

void dmrTrace_put(char kind, uint32_t ms, const uint8_t *data, size_t len)
{
    unsigned h = atomic_load(&head);
    unsigned t = atomic_load(&tail);

    if ((h - t) >= TRACE_RECORDS) {
        atomic_fetch_add(&lost, 1);
        return;
    }

    if (len > DMR_TRACE_DATA_BYTES)
        len = DMR_TRACE_DATA_BYTES;

    struct traceRecord *r = &records[h & TRACE_MASK];
    r->ms = ms;
    r->kind = kind;
    r->len = (uint8_t)len;
    if (len > 0)
        memcpy(r->data, data, len);

    atomic_store(&head, h + 1);
}

size_t dmrTrace_pop(char *line, size_t max)
{
    if (max < DMR_TRACE_LINE_BYTES)
        return 0;

    size_t n = 0;

    /* Records lost since the last line: reported before the next one */
    unsigned dropped = atomic_exchange(&lost, 0);
    if (dropped != 0) {
        line[n++] = 'D';
        n += putHex(&line[n], dropped, 8);
        line[n++] = '\n';
        return n;
    }

    unsigned t = atomic_load(&tail);
    if (t == atomic_load(&head))
        return 0;

    const struct traceRecord *r = &records[t & TRACE_MASK];
    line[n++] = r->kind;
    n += putHex(&line[n], r->ms, 6);
    for (unsigned i = 0; i < r->len; i++)
        n += putHex(&line[n], r->data[i], 2);
    line[n++] = '\n';

    atomic_store(&tail, t + 1);

    return n;
}

void dmrTrace_requestDump(void)
{
    atomic_store(&dumpRequest, true);
}

bool dmrTrace_dumpRequested(void)
{
    return atomic_exchange(&dumpRequest, false);
}

void dmrTrace_task(void)
{
#ifdef CONFIG_USB_SERIAL
    /* Whether the last line was taken by the host */
    static bool flowing = false;
    char line[DMR_TRACE_LINE_BYTES];

    for (unsigned i = 0; i < TRACE_LINES_PER_TASK; i++) {
        size_t len = dmrTrace_pop(line, sizeof(line));
        if (len == 0)
            break;

        /*
         * Nobody reads the port: the line is dropped and the buffer keeps
         * draining at one record per pass, so that a reader appearing later
         * gets what happens from then on, not a backlog.
         */
        if (usb_serial_write(line, len) < 0) {
            flowing = false;
            break;
        }

        if (!flowing) {
            flowing = true;
            dmrTrace_requestDump();
        }
    }
#endif
}

#endif /* CONFIG_DMR_TRACE */
