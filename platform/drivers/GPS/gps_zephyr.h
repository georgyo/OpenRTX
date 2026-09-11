/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef GPS_ZEPHYR_H
#define GPS_ZEPHYR_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialise the GPS driver.
 * This function does not turn on the GPS module.
 */
void gpsZephyr_init();

/**
 * Terminate the GPS driver.
 */
void gpsZephyr_terminate();

/**
 * Retrieve a NMEA sentence from the GPS.
 * The sentence written in the destination buffer is always NUL-terminated,
 * thus at most maxLength - 1 characters are stored. If the sentence is longer
 * than that, the characters not written in the destination are lost.
 *
 * @param priv: unused parameter, for function signature compatibility.
 * @param buf: pointer to NMEA sentence destination buffer.
 * @param maxLength: size of the destination buffer, including the terminator.
 * @return the length of the extracted sentence or -1 if the sentence has been
 * truncated because longer than maxLength - 1 characters. If the ring buffer
 * is empty, zero is returned.
 */
int gpsZephyr_getNmeaSentence(void *priv, char *buf, const size_t maxLength);

#ifdef __cplusplus
}
#endif

#endif /* GPS_STM32_H */
