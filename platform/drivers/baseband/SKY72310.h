/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SKY73210_H
#define SKY73210_H

#include "peripherals/gpio.h"
#include "peripherals/spi.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * SKY73210 device data.
 */
struct sky73210 {
    const struct spiDevice *spi; ///< SPI bus device driver
    const struct gpioPin cs;     ///< Chip select gpio
    const struct gpioPin ld;     ///< Lock detect gpio, port NULL if not wired
    const uint32_t refClk;       ///< Reference clock frequency, in Hz
};

/**
 * Initialise the PLL.
 *
 * @param dev: pointer to device data.
 * @param gain: phase detector gain.
 */
void SKY73210_init(const struct sky73210 *dev, const uint8_t gain);

/**
 * Terminate PLL driver.
 *
 * @param dev: pointer to device data.
 */
void SKY73210_terminate(const struct sky73210 *dev);

/**
 * Change VCO frequency.
 *
 * @param dev: pointer to device data.
 * @param freq: new VCO frequency, in Hz.
 * @param clkDiv: reference clock division factor.
 */
void SKY73210_setFrequency(const struct sky73210 *dev, const uint32_t freq,
                           uint8_t clkDiv);

/**
 * Check whether the PLL is locked, by reading the lock detect (LD) output.
 * The LD pin signals an out-of-lock condition as an active low level.
 *
 * @param dev: pointer to device data.
 * @return true if the PLL is locked or if the device has no lock detect gpio.
 */
bool SKY73210_isLocked(const struct sky73210 *dev);

/**
 * Wait for the PLL to lock, polling the lock detect output.
 *
 * @param dev: pointer to device data.
 * @param timeoutUs: maximum time to wait, in microseconds.
 * @return true if the PLL locked before the timeout expired, false otherwise.
 */
bool SKY73210_waitLock(const struct sky73210 *dev, uint32_t timeoutUs);

#ifdef __cplusplus
}
#endif

#endif /* SKY73210_H */
