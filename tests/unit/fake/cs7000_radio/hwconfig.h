/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HWCONFIG_H
#define HWCONFIG_H

/*
 * Stand-in for the CS7000-PLUS hwconfig.h, pinmap.h and stm32h7xx.h,
 * shadowing the real ones when radio_CS7000.cpp and the HR_C6000 baseband
 * driver are compiled for the host unit tests (tests/unit/radio_cs7000.cpp).
 * It carries what those sources use: the C6000 object, the gpio expander
 * and ports, the pinmap entries of the RF control lines, the PLL, detector
 * and ADC descriptors, the ADC channel numbers, and fake DAC / RCC register
 * blocks. The objects are defined by the test program.
 */

#include <stdint.h>
#include "drivers/SPI/spi_custom.h"
#include "drivers/baseband/AK2365A.h"
#include "drivers/baseband/SKY72310.h"
#include "peripherals/adc.h"
#include "peripherals/gpio.h"
#include "peripherals/spi.h"

#ifdef __cplusplus

#include "drivers/baseband/HR_C6000.h"

extern HR_C6000 C6000;

extern "C" {
#endif

#define FAKE_CS7000_RADIO_HWCONFIG 1

/* The CS7000-PLUS flavour of radio_CS7000.cpp: DAC1 and RCC->APB1LENR */
#define PLATFORM_CS7000P 1
#define CONFIG_DMR
#define CONFIG_M17

/*
 * Fake STM32H7 register blocks: the DAC feeding APC/TV (TX power) and the
 * RX filter tuning voltage, and the clock enable of the DAC.
 */
typedef struct {
    volatile uint32_t CR;
    volatile uint32_t DHR12R1;
    volatile uint32_t DHR8R1;
} FakeDacRegs;

typedef struct {
    volatile uint32_t APB1LENR;
} FakeRccRegs;

extern FakeDacRegs fakeDac;
extern FakeRccRegs fakeRcc;

#define DAC1 (&fakeDac)
#define RCC (&fakeRcc)
#define DAC_CR_EN1 (1U << 0)
#define RCC_APB1LENR_DAC12EN (1U << 16)

/*
 * Native gpio port of the analog inputs: an opaque token, the fake
 * gpio_setMode() defined by the test records the calls.
 */
extern const int fakeGpioA;
#define GPIOA (&fakeGpioA)
void gpio_setMode(const void *port, const uint8_t pin, const uint16_t mode);

enum AdcChannels {
    ADC_VOL_CH = 8,
    ADC_VBAT_CH = 3,
    ADC_RTX_CH = 15,
    ADC_RSSI_CH = 9,
    ADC_MIC_CH = 7,
    ADC_CTCSS_CH = 14,
    ADC_VOX_CH = 4,
};

/* Fake devices, defined by the test program */
extern const struct Adc adc1;
extern const struct spiCustomDevice det_spi;
extern const struct spiCustomDevice pll_spi;
extern const struct gpioDev extGpio;
extern const struct gpioDev GpioB;
extern const struct ak2365a detector;
extern const struct sky73210 pll;

/* Pinmap entries used by radio_CS7000.cpp, same bits as the real pinmap.h */
#define AIN_RSSI GPIOA, 0
#define AIN_RTX GPIOA, 3
#define AIN_CTCSS GPIOA, 2
#define APC_TV GPIOA, 4

#define DET_PDN &extGpio, 6
#define RF_APC_SW &extGpio, 3
#define VCOVCC_SW &extGpio, 8
#define TX_PWR_EN &extGpio, 9
#define RX_PWR_EN &extGpio, 15
#define VCO_PWR_EN &extGpio, 11
#define CTCSS_AMP_EN &extGpio, 22
#define AF_MUTE &extGpio, 23

#ifdef __cplusplus
}
#endif

#endif /* HWCONFIG_H */
