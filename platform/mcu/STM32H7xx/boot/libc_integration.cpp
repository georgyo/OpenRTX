/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <reent.h>
#include "filesystem/file_access.h"
#include "hwconfig.h"

/*
 * stdout/stderr go to the USB CDC console only when the build asks for it
 * with ENABLE_STDIO, exactly like the STM32F4/MK22/AT32 ports route them to
 * their virtual COM port. Without it printf() is a no-op on the radio and the
 * USB stack (still initialised under CONFIG_USB_SERIAL) is free for binary
 * users of usb_serial_write()/usb_serial_read().
 */
#if defined(CONFIG_USB_SERIAL) && defined(ENABLE_STDIO)
#define USB_STDIO
#endif

#ifdef USB_STDIO
#include "interfaces/usb_serial.h"

static pthread_mutex_t stdio_usb_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Input bytes translated per usb_serial_write() call and the on-stack buffer
 * that holds the result (every '\n' may become "\r\n"). One chunk is exactly
 * one CDC TX FIFO (64 bytes at full speed), so a larger buffer would not save
 * any round trips; keeping it small matters because _write_r() runs on the
 * caller's stack, below newlib's vfprintf frame and above tud_task(), which
 * usb_serial_write() may pump from this thread. The UI thread has a 2 KiB
 * stack; stdio must not be used from the 512-byte rtx/audio threads at all.
 */
#define USB_STDIO_CHUNK 64
#define USB_STDIO_OUTBUF (2 * USB_STDIO_CHUNK)

/*
 * If the previous _write_r ended by sending a lone '\r' (e.g. printf split
 * "foo\r" and "\n" across two writes), the next write must send only '\n' to
 * complete CRLF — not '\r\n' again — or the wire sees '\r\r\n' (staircase).
 */
static bool usb_out_pending_cr;
#endif

using namespace std;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \internal
 * _write_r, write to a file
 */
int _write_r(struct _reent *ptr, int fd, const void *buf, size_t cnt)
{
#ifdef USB_STDIO
    if(fd == STDOUT_FILENO || fd == STDERR_FILENO)
    {
        pthread_mutex_lock(&stdio_usb_mutex);
        /*
         * Best-effort lossy console output.
         *
         * This function always returns cnt regardless of how many bytes
         * usb_serial_write() actually sent.  This is intentional: the USB
         * serial port is a debug console, not a reliable byte stream.
         * Propagating partial counts would cause stdio to retry, which can
         * stall the caller when the CDC TX FIFO is full or the device is not
         * yet mounted.  Callers that require reliable delivery must not use
         * stdio for that purpose.
         *
         * Before usb_serial_init() completes, usb_mounted is false and
         * usb_serial_write() returns -1 immediately; output is silently
         * discarded until the USB device is enumerated.
         *
         * Translate \n -> \r\n so output renders correctly in serial
         * terminals.
         * The translation is done here in the stdio layer,
         * not in the driver, so the driver can be used for binary
         * protocols (TNC, NMEA passthrough) without corruption.
         *
         * Note: the Linux host TTY (/dev/ttyACMx) has ICRNL enabled by
         * default, which translates incoming \r (0x0D) to \n (0x0A) before
         * the data reaches the application.  This converts our \r\n into
         * \n\n, causing double-spacing and loss of carriage return in
         * terminal emulators. minicom and HTerm work fine out of the box.
         * picocom should work with --imap lfcrlf
         */
        const char *p    = (const char *) buf;
        size_t      left = cnt;
        size_t      i    = 0;

        while(i < left)
        {
            if(usb_out_pending_cr)
            {
                if(p[i] == '\n')
                {
                    usb_out_pending_cr = false;
                    i++;
                    pthread_mutex_unlock(&stdio_usb_mutex);
                    usb_serial_write("\n", 1);
                    pthread_mutex_lock(&stdio_usb_mutex);
                    continue;
                }
                usb_out_pending_cr = false;
            }

            size_t chunk = left - i;
            if(chunk > USB_STDIO_CHUNK)
                chunk = USB_STDIO_CHUNK;

            char   out[USB_STDIO_OUTBUF];
            size_t o = 0;
            for(size_t j = 0; j < chunk; j++)
            {
                char c = p[i + j];
                if(c == '\n')
                {
                    if(o > 0 && out[o - 1] == '\r')
                        out[o++] = '\n';
                    else
                    {
                        out[o++] = '\r';
                        out[o++] = '\n';
                    }
                }
                else
                {
                    out[o++] = c;
                }
            }
            if(o > 0 && out[o - 1] == '\r')
                usb_out_pending_cr = true;
            else
                usb_out_pending_cr = false;

            i += chunk;
            /*
             * Do not hold stdio_usb_mutex across usb_serial_write(): that path
             * can block ~50 ms retrying CDC TX while pumping would otherwise be
             * starved and other threads could not use stdio.
             */
            pthread_mutex_unlock(&stdio_usb_mutex);
            usb_serial_write(out, o);
            pthread_mutex_lock(&stdio_usb_mutex);
        }

        pthread_mutex_unlock(&stdio_usb_mutex);
        return (int)cnt;
    }
#else
    (void) fd;
    (void) buf;
    (void) cnt;
#endif

    /* If fd is not stdout or stderr */
    ptr->_errno = EBADF;
    return -1;
}

/**
 * \internal
 * _read_r, read from a file.
 */
int _read_r(struct _reent *ptr, int fd, void *buf, size_t cnt)
{
    (void) ptr;
    (void) fd;
    (void) buf;
    (void) cnt;

    /* If fd is not stdin */
    ptr->_errno = EBADF;
    return -1;
}

#ifdef __cplusplus
}
#endif
