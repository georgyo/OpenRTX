/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PINMAP_H
#define PINMAP_H

#include "stm32h7xx.h"

/*
 * Power control. MAIN_PWR_DET (schematic net POW_DET) senses the battery
 * through D605 and the R626/R627 100k/47k divider.
 */
#define BAT_DETECT    GPIOB,2     // BAT_DET
#define MAIN_PWR_DET  GPIOA,6     // POW_DET
#define MAIN_PWR_SW   &extGpio,20 // POW_C

/* Display */
#define LCD_D0        GPIOD,0
#define LCD_D1        GPIOD,1
#define LCD_D2        GPIOD,2
#define LCD_D3        GPIOD,3
#define LCD_D4        GPIOD,4
#define LCD_D5        GPIOD,5
#define LCD_D6        GPIOD,6
#define LCD_D7        GPIOD,7
#define LCD_WR        GPIOD,13
#define LCD_RD        GPIOD,14
#define LCD_DC        GPIOD,12
#define LCD_RST       GPIOE,0
#define LCD_CS        GPIOE,1
#define LCD_BKLIGHT   GPIOC,9

/*
 * Keyboard. Rows and columns, except for column 5, are shared with the LCD
 * data lines. Commens reports the resistor number in the schematic for reference.
 */
#define KB_ROW1       LCD_D0    // R905
#define KB_ROW2       LCD_D1    // R906
#define KB_ROW3       LCD_D2    // R907
#define KB_ROW4       LCD_D3    // R908
#define KB_COL1       LCD_D7    // R902
#define KB_COL2       LCD_D6    // R903
#define KB_COL3       LCD_D5    // R904
#define KB_COL4       LCD_D4    // R909
#define KB_COL5       GPIOB,7   // R926
#define KBD_BKLIGHT   &extGpio,16

/* Push-to-talk and side keys */
#define PTT_SW        GPIOA,8
#define PTT_EXT       GPIOE,11
#define SIDE_KEY1     GPIOD,15
#define SIDE_KEY2     GPIOE,12
#define SIDE_KEY3     GPIOE,13
#define ALARM_KEY     GPIOE,10

/* Channel selection rotary encoder: 4-bit absolute code, 16 positions */
#define CH_SELECTOR_0 GPIOE,14    // ECN0
#define CH_SELECTOR_1 GPIOE,15    // ECN1
#define CH_SELECTOR_2 GPIOB,10    // ECN2
#define CH_SELECTOR_3 GPIOB,11    // ECN3

/* LEDs */
#define GREEN_LED     &extGpio,12
#define RED_LED       &extGpio,13

/*
 * Analog inputs. The RX baseband (RX-AUDIO) comes from the AK2365A
 * discriminator through the active filter, the TX baseband (TX-AUDIO) from
 * the microphone preamplifier.
 */
#define AIN_VOLUME    GPIOC,5   // VOL_DET, volume potentiometer
#define AIN_VOX       GPIOC,4   // Hardware VOX circuit
#define AIN_MIC       GPIOA,7   // TX-A
#define AIN_RSSI      GPIOB,0
#define AIN_RTX       GPIOA,3   // RX-A
#define AIN_CTCSS     GPIOA,2   // QT_DQT_IN

/* Tone generator */
#define BEEP_OUT      GPIOA,5   // System "beep"

/* External flash */
#define FLASH_CS      &GpioE,4
#define FLASH_CLK     GPIOE,2
#define FLASH_SDO     GPIOE,5
#define FLASH_SDI     GPIOE,6

/* PLL */
#define PLL_CS        &GpioD,9
#define PLL_CLK       GPIOD,10
#define PLL_DAT       GPIOD,8
#define PLL_LD        &GpioD,11

/* HR_C6000 */
#define C6K_CS        &GpioB,12
#define C6K_CLK       GPIOB,13
#define C6K_MISO      GPIOB,14
#define C6K_MOSI      GPIOB,15
#define C6K_SLEEP     &extGpio,7

/*
 * HR_C6000 vocoder SPI port ("V_SPI", manual §4.6.1): the C6000 is the SPI
 * slave, the MCU the master. Names follow the schematic roles (CS7000-M17 Plus
 * schematic V01, same nets on the CS7000): V_SDO, C6000 pin 42, is an output
 * of the C6000 and lands on PB4, the MCU MISO; V_SDI, C6000 pin 41, is an
 * input of the C6000 and lands on PB5, the MCU MOSI; V_SCLK on PB3, V_CS on
 * PB6. The former VOC_MOSI (PB4) / VOC_MISO (PB5) names had the roles swapped.
 * UNVERIFIED on hardware: the orientation, see docs/dmr_bringup.md.
 */
#define VOC_CS        GPIOB,6
#define VOC_SCK       GPIOB,3
#define VOC_SDO_C6K   GPIOB,4   // C6000 V_SDO -> MCU MISO
#define VOC_SDI_C6K   GPIOB,5   // C6000 V_SDI <- MCU MOSI

#define I2S_FS        GPIOA,15
#define I2S_CLK       GPIOC,10
#define I2S_RX        GPIOC,11
#define I2S_TX        GPIOC,12

/*
 * HR_C6000 interrupt lines, ~300ns active-low pulses (manual §4.4), caught
 * with falling-edge EXTI interrupts by the DMR modem driver.
 *
 * Schematic discrepancy, UNVERIFIED on hardware: these assignments come from
 * the reverse engineering of the CS7000 (STM32F405) OEM firmware and say
 * SYS_INTER = PC1, RF_TX_INTER = PC2. The CS7000-M17 Plus schematic V01 says
 * TIME_SLOT_INTER = PC0, RF_TX_INTER = PC2_C, SYS_INTER = PC3_C and puts the
 * 2T/5T analogue tone detector on PC1. The driver (dmr_CS7000.cpp) attaches
 * PC0..PC3 all four with a counter each and dispatches TS and TX with the
 * constants below; for SYS it starts from DMR_SYS_INT and can be switched
 * to DMR_SYS_INT_ALT at runtime from the DMR monitor screen ("SYS line"
 * action, driver knob DMRBB_DRV_SYS_LINE of interfaces/dmr_baseband.h),
 * so the answer can be found on the radio without reflashing. Once a board
 * has been checked, fix DMR_SYS_INT for that target (docs/dmr_bringup.md,
 * checks 1.2 and 3.1). RF_RX_INTER is not wired to the MCU.
 */
#define DMR_TS_INT      GPIOC,0
#define DMR_SYS_INT     GPIOC,1
#define DMR_TX_INT      GPIOC,2
#define DMR_SYS_INT_ALT GPIOC,3   // SYS_INTER per the CS7000-M17 Plus schematic

/* AK2365A */
#define DET_PDN       &extGpio,6
#define DET_CS        &extGpio,0
#define DET_CLK       GPIOE,3
#define DET_DAT       GPIOC,13
#define DET_RST       &GpioC,14
#define DET_BUSY      GPIOB,1

/* RTX control */
#define RF_APC_SW     &extGpio,3
#define VCOVCC_SW     &extGpio,8
#define TX_PWR_EN     &extGpio,9
#define RX_PWR_EN     &extGpio,15
#define VCO_PWR_EN    &extGpio,11
#define CTCSS_AMP_EN  &extGpio,22
#define APC_TV        GPIOA,4

/* Audio control */
#define AUDIO_AMP_EN  &extGpio,14
#define INT_SPK_MUTE  &extGpio,17
#define EXT_SPK_MUTE  &extGpio,19
#define MIC_PWR_EN    &extGpio,18
#define INT_MIC_SEL   &extGpio,10
#define EXT_MIC_SEL   &extGpio,5
#define AF_MUTE       &extGpio,23
#define PHONE_DETECT  GPIOA,13

/* GPS, USART6 RX only: the module's TX line is not wired to a USART TX pin */
#define GPS_TXD       GPIOC,8
#define GPS_RXD       GPIOC,7

/* Accessory connector */
#define PHONE_TXD     GPIOA,0
#define PHONE_RXD     GPIOA,1

/* SN74HC595 gpio extenders */
#define GPIOEXT_CLK   GPIOE,7
#define GPIOEXT_DAT   GPIOE,9
#define GPIOEXT_STR   &GpioE,8

/* Bluetooth module */
#define BLTH_DETECT   GPIOA,14
#define BLTH_PWR_EN   &extGpio,4
#define BLTH_RXD      GPIOA,9
#define BLTH_TXD      GPIOA,10

/* I2C bus, KXTJ3-1057 accelerometer */
#define I2C_SDA       GPIOB,9
#define I2C_SCL       GPIOB,8

/* Vibration motor */
#define VIBR_MOTOR    &extGpio,2

/* USB OTG FS */
#define USB_DN        GPIOA,11
#define USB_DP        GPIOA,12

#endif /* PINMAP_H */
