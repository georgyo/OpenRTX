/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "drivers/GPIO/gpio_shiftReg.h"
#include "drivers/SPI/spi_bitbang.h"
#include "drivers/SPI/spi_custom.h"
#include "drivers/SPI/spi_stm32.h"
#include "drivers/ADC/adc_stm32.h"
#include "drivers/GPS/gps_stm32.h"
#include "hwconfig.h"
#include <pthread.h>
#include "drivers/baseband/SKY72310.h"
#include "drivers/baseband/AK2365A.h"
#include "core/gps.h"

/**
 * SPI bitbang function for SN74HC595 gpio extender.
 *
 * Hand-tuned to be as fast as possible, gives the following clock performance
 * when compiled with -Os and run on STM32H743 at 400MHz:
 *
 * - Freq 7MHz
 * - Pos. width 70ns
 * - Neg. with 70ns
 */
static uint8_t spiSr_func(const void *priv, uint8_t value)
{
    (void) priv;

    for(uint8_t cnt = 0; cnt < 8; cnt++)
    {
        GPIOE->BSRR = (1 << 23);    // Clear PE7 (CLK)

        if(value & (0x80 >> cnt))
            GPIOE->BSRR = 1 << 9;   // Set PE9 (MOSI)
        else
            GPIOE->BSRR = 1 << 25;  // Clear PE9 (MOSI)

        // ~70ns delay
        asm volatile("           mov   r1, #5     \n"
                     "___loop_2: cmp   r1, #0     \n"
                     "           itt   ne         \n"
                     "           subne r1, r1, #1 \n"
                     "           bne   ___loop_2  \n":::"r1");

        GPIOE->BSRR = (1 << 7);                 // Set PE7 (CLK)

        // ~70ns delay
        asm volatile("           mov   r1, #6     \n"
                     "___loop_3: cmp   r1, #0     \n"
                     "           itt   ne         \n"
                     "           subne r1, r1, #1 \n"
                     "           bne   ___loop_3  \n":::"r1");
    }

    return 0;
}

static const struct spiConfig spiDetCfg =
{
    .clk       = { DET_CLK },
    .mosi      = { DET_DAT },
    .miso      = { DET_DAT },
    .clkPeriod = SCK_PERIOD_FROM_FREQ(1000000),
    .flags     = SPI_HALF_DUPLEX
};

static const struct spiConfig spiPllCfg =
{
    .clk       = { PLL_CLK },
    .mosi      = { PLL_DAT },
    .miso      = { PLL_DAT },
    .clkPeriod = SCK_PERIOD_FROM_FREQ(1000000),
    .flags     = SPI_HALF_DUPLEX
};

/*
 * HR_C6000 vocoder SPI port (V_SPI), bit-banged SPI master, mode 0 (clock
 * idle low, data sampled on the rising edge), about 500kHz with the 1us
 * half-period of the bitbang driver: the manual (§4.6.1) allows up to 4MHz.
 * UNVERIFIED on hardware: the clock phase. The manual's timing figure is
 * missing from the translation; the "user" SPI of the same chip is driven
 * with SPI_FLAG_CPHA (see audio_CS7000.cpp), so flipping the flag here is the
 * first thing to try if the 27-byte voice reads come back as zeros.
 */
static const struct spiConfig spiVocCfg =
{
    .clk       = { VOC_SCK },
    .mosi      = { VOC_SDI_C6K },
    .miso      = { VOC_SDO_C6K },
    .clkPeriod = SCK_PERIOD_FROM_FREQ(1000000),
    .flags     = 0
};

static const struct gpioPin shiftRegStrobe = { GPIOEXT_STR };
static pthread_mutex_t adc1Mutex;
static pthread_mutex_t c6000_mutex;

SPI_CUSTOM_DEVICE_DEFINE(spiSr, spiSr_func, NULL, NULL)
SPI_BITBANG_DEVICE_DEFINE(det_spi, spiDetCfg, NULL)
SPI_BITBANG_DEVICE_DEFINE(pll_spi, spiPllCfg, NULL)
SPI_BITBANG_DEVICE_DEFINE(voc_spi, spiVocCfg, NULL)
SPI_STM32_DEVICE_DEFINE(flash_spi, SPI4, NULL)
SPI_STM32_DEVICE_DEFINE(c6000_spi, SPI2, &c6000_mutex)
GPIO_SHIFTREG_DEVICE_DEFINE(extGpio, (const struct spiDevice *) &spiSr, shiftRegStrobe, 24)
ADC_STM32_DEVICE_DEFINE(adc1, ADC1, &adc1Mutex, ADC_COUNTS_TO_UV(3300000, 16))

const struct ak2365a detector =
{
    .spi = (const struct spiDevice *) &det_spi,
    .cs  = { DET_CS  },
    .res = { DET_RST }
};

const struct sky73210 pll =
{
    .spi    = (const struct spiDevice *) &pll_spi,
    .cs     = { PLL_CS },
    .ld     = { PLL_LD },
    .refClk = 16800000
};

const struct gpsDevice gps =
{
    .enable = gpsStm32_enable,
    .disable = gpsStm32_disable,
    .getSentence = gpsStm32_getNmeaSentence
};
