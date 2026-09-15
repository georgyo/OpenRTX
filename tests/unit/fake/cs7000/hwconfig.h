/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HWCONFIG_H
#define HWCONFIG_H

/*
 * Stand-in for the CS7000 / CS7000-PLUS hwconfig.h and pinmap.h, shadowing
 * the real ones when the HR_C6000 baseband and DMR modem drivers are compiled
 * for the host unit tests (tests/unit/hr_c6000_dmr.cpp). It carries only what
 * those drivers use: the C6000 object, the gpio ports of the interrupt and
 * V_SPI pins, the pinmap entries of those pins and the V_SPI device. The
 * objects are defined by the test program.
 */

#include <stdint.h>
#include "drivers/SPI/spi_custom.h"
#include "peripherals/gpio.h"
#include "peripherals/spi.h"

#ifdef __cplusplus

#include "drivers/baseband/HR_C6000.h"

extern HR_C6000 C6000;

extern "C" {
#endif

#define FAKE_CS7000_HWCONFIG 1

/* Fake gpio ports, defined by the test program */
extern const struct gpioDev GpioB;
extern const struct gpioDev GpioC;

/*
 * Pinmap entries in the "port,pin" form of the real pinmap.h. The port
 * tokens are never dereferenced by the drivers under test, which only take
 * the pin number.
 */
#define GPIOB 1
#define GPIOC 2

#define VOC_CS GPIOB, 6
#define VOC_SCK GPIOB, 3
#define VOC_SDO_C6K GPIOB, 4
#define VOC_SDI_C6K GPIOB, 5

#define DMR_TS_INT GPIOC, 0
#define DMR_SYS_INT GPIOC, 1
#define DMR_TX_INT GPIOC, 2
#define DMR_SYS_INT_ALT GPIOC, 3

/* Fake V_SPI device, defined by the test program */
extern const struct spiCustomDevice voc_spi;

#define CONFIG_DMR

#ifdef __cplusplus
}
#endif

#endif /* HWCONFIG_H */
