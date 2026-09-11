/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include "stm32h7xx.h"
#include "rcc.h"

/*
 * Runs the STM32H743 clock tree setup of platform/mcu/STM32H7xx/drivers/rcc.cpp
 * against fake RCC/PWR/FLASH register blocks and checks that:
 *
 * - the bus frequencies resulting from the programmed PLL and prescalers stay
 *   within the limits of DS12110 "General operating conditions" at VOS1:
 *   f_SYSCLK <= 400MHz, f_HCLK <= 200MHz, f_PCLKx <= 100MHz;
 * - getBusClock() reports exactly those frequencies, because every APB
 *   peripheral driver (USART6 baud rate, SPI4/5/6 dividers) derives its
 *   timing from it. PR #330 broke this invariant by making the buses match a
 *   wrong 200MHz table instead of fixing the table.
 */

RCC_TypeDef stm32h7FakeRcc;
PWR_TypeDef stm32h7FakePwr;
FLASH_TypeDef stm32h7FakeFlash;

/* HSE_VALUE of the CS7000-PLUS target (meson.build stm32h743_def). */
static constexpr uint32_t HSE_FREQ = 25000000;

/* DS12110, VOS1 (PWR_D3CR VOS = 0b11) */
static constexpr uint32_t MAX_SYSCLK = 400000000;
static constexpr uint32_t MAX_HCLK = 200000000;
static constexpr uint32_t MAX_PCLK = 100000000;

/* RM0433 RCC_D1CFGR: HPRE[3:0] and D1CPRE[3:0] encode 1, 2, 4 ... 512 */
static uint32_t ahbDivider(uint32_t field)
{
    if ((field & 0x8) == 0)
        return 1;

    static const uint32_t div[] = { 2, 4, 8, 16, 64, 128, 256, 512 };
    return div[field & 0x7];
}

/* RM0433 RCC_DxCFGR: PPRE[2:0] encode 1, 2, 4, 8, 16 */
static uint32_t apbDivider(uint32_t field)
{
    if ((field & 0x4) == 0)
        return 1;

    return 2u << (field & 0x3);
}

struct clockTree {
    uint32_t sysclk;
    uint32_t hclk;
    uint32_t pclk1;
    uint32_t pclk2;
    uint32_t pclk3;
    uint32_t pclk4;
};

/* Decode the clock tree from the register values, as the hardware does. */
static clockTree decodeClockTree()
{
    uint32_t m = (RCC->PLLCKSELR & RCC_PLLCKSELR_DIVM1_Msk)
              >> RCC_PLLCKSELR_DIVM1_Pos;
    uint32_t n = ((RCC->PLL1DIVR & RCC_PLL1DIVR_N1_Msk) >> RCC_PLL1DIVR_N1_Pos)
               + 1;
    uint32_t p = ((RCC->PLL1DIVR & RCC_PLL1DIVR_P1_Msk) >> RCC_PLL1DIVR_P1_Pos)
               + 1;

    REQUIRE(m != 0);
    uint64_t vco = (uint64_t)HSE_FREQ / m * n;
    uint64_t pll1p = vco / p;

    uint32_t d1cpre = (RCC->D1CFGR & RCC_D1CFGR_D1CPRE_Msk)
                   >> RCC_D1CFGR_D1CPRE_Pos;
    uint32_t hpre = (RCC->D1CFGR & RCC_D1CFGR_HPRE_Msk) >> RCC_D1CFGR_HPRE_Pos;
    uint32_t d1ppre = (RCC->D1CFGR & RCC_D1CFGR_D1PPRE_Msk)
                   >> RCC_D1CFGR_D1PPRE_Pos;
    uint32_t ppre1 = (RCC->D2CFGR & RCC_D2CFGR_D2PPRE1_Msk)
                  >> RCC_D2CFGR_D2PPRE1_Pos;
    uint32_t ppre2 = (RCC->D2CFGR & RCC_D2CFGR_D2PPRE2_Msk)
                  >> RCC_D2CFGR_D2PPRE2_Pos;
    uint32_t d3ppre = (RCC->D3CFGR & RCC_D3CFGR_D3PPRE_Msk)
                   >> RCC_D3CFGR_D3PPRE_Pos;

    clockTree tree;
    tree.sysclk = pll1p / ahbDivider(d1cpre);
    tree.hclk = tree.sysclk / ahbDivider(hpre);
    tree.pclk1 = tree.hclk / apbDivider(ppre1);
    tree.pclk2 = tree.hclk / apbDivider(ppre2);
    tree.pclk3 = tree.hclk / apbDivider(d1ppre);
    tree.pclk4 = tree.hclk / apbDivider(d3ppre);

    return tree;
}

/*
 * SystemInit() zeroes the RCC configuration registers before startPll() runs
 * (system_stm32h7xx.c), so zeroed fake registers reproduce the real entry
 * state.
 */
static clockTree runStartPll()
{
    memset(&stm32h7FakeRcc, 0x00, sizeof(stm32h7FakeRcc));
    memset(&stm32h7FakePwr, 0x00, sizeof(stm32h7FakePwr));
    memset(&stm32h7FakeFlash, 0x00, sizeof(stm32h7FakeFlash));

    startPll();

    return decodeClockTree();
}

TEST_CASE("STM32H7 startPll selects PLL1 fed by HSE at VOS1", "[stm32h7][rcc]")
{
    runStartPll();

    REQUIRE((PWR->D3CR & PWR_D3CR_VOS_Msk)
            == (PWR_D3CR_VOS_1 | PWR_D3CR_VOS_0));
    REQUIRE((RCC->PLLCKSELR & RCC_PLLCKSELR_PLLSRC_Msk)
            == RCC_PLLCKSELR_PLLSRC_HSE);
    REQUIRE((RCC->CFGR & RCC_CFGR_SW_Msk) == RCC_CFGR_SW_PLL1);
}

TEST_CASE("STM32H7 bus clocks stay within the DS12110 limits at VOS1",
          "[stm32h7][rcc]")
{
    clockTree tree = runStartPll();

    REQUIRE(tree.sysclk == 400000000);
    REQUIRE(tree.sysclk <= MAX_SYSCLK);
    REQUIRE(tree.hclk <= MAX_HCLK);
    REQUIRE(tree.pclk1 <= MAX_PCLK);
    REQUIRE(tree.pclk2 <= MAX_PCLK);
    REQUIRE(tree.pclk3 <= MAX_PCLK);
    REQUIRE(tree.pclk4 <= MAX_PCLK);
}

TEST_CASE("STM32H7 getBusClock matches the programmed prescalers",
          "[stm32h7][rcc]")
{
    clockTree tree = runStartPll();

    REQUIRE(getBusClock(PERIPH_BUS_AHB) == tree.hclk);
    REQUIRE(getBusClock(PERIPH_BUS_APB1) == tree.pclk1);
    REQUIRE(getBusClock(PERIPH_BUS_APB2) == tree.pclk2);
    REQUIRE(getBusClock(PERIPH_BUS_APB3) == tree.pclk3);
    REQUIRE(getBusClock(PERIPH_BUS_APB4) == tree.pclk4);
    REQUIRE(getBusClock(PERIPH_BUS_NUM) == 0);
}

TEST_CASE("STM32H7 rcc_getPeriphClock maps peripherals to their bus",
          "[stm32h7][rcc]")
{
    runStartPll();

    REQUIRE(rcc_getPeriphClock((void *)LPTIM1_BASE)
            == getBusClock(PERIPH_BUS_APB1));
    REQUIRE(rcc_getPeriphClock((void *)USART6_BASE)
            == getBusClock(PERIPH_BUS_APB2));
    REQUIRE(rcc_getPeriphClock((void *)SPI4_BASE)
            == getBusClock(PERIPH_BUS_APB2));
    REQUIRE(rcc_getPeriphClock((void *)WWDG1_BASE)
            == getBusClock(PERIPH_BUS_APB3));
    REQUIRE(rcc_getPeriphClock((void *)SPI6_BASE)
            == getBusClock(PERIPH_BUS_APB4));
    REQUIRE(rcc_getPeriphClock((void *)ADC1_BASE)
            == getBusClock(PERIPH_BUS_AHB));
}

TEST_CASE("STM32H7 APB2 peripherals keep their rates after the clock setup",
          "[stm32h7][rcc]")
{
    clockTree tree = runStartPll();

    // USART6 (GPS) and SPI4/5 (external flash) take pclk2 as kernel clock:
    // RM0433 RCC_D2CCIP2R USART16SEL = 000, RCC_D2CCIP1R SPI45SEL = 000.
    REQUIRE((RCC->D2CCIP2R & RCC_D2CCIP2R_USART16SEL_Msk) == 0);
    REQUIRE((RCC->D2CCIP1R & RCC_D2CCIP1R_SPI45SEL_Msk) == 0);

    // gps_stm32.cpp: BRR = round(fck / baud) for 9600 Bd, 16x oversampling
    uint32_t fck = rcc_getPeriphClock((void *)USART6_BASE);
    uint32_t quot = (2 * fck) / 9600;
    uint32_t brr = (quot / 2) + (quot & 1);
    REQUIRE(brr == 10417);
    REQUIRE(brr <= 0xFFFF);

    // spi_stm32h7.c: SCK = pclk2 / 2^(MBR+1), rounded down to the requested
    // 25MHz of nvmem_CS7000.c
    uint32_t spiClk = getBusClock(PERIPH_BUS_APB2);
    for (uint8_t mbr = 0; mbr < 7; mbr++) {
        spiClk = getBusClock(PERIPH_BUS_APB2) / (1 << (mbr + 1));
        if (spiClk <= 25000000)
            break;
    }
    REQUIRE(spiClk == 25000000);

    // RM0433 RCC_CFGR TIMPRE = 0: the APB timer kernel clock is pclk when the
    // APB prescaler is 1 and 2 x pclk otherwise. TIM8 (backlight_CS7000.c)
    // thus keeps a 200MHz clock.
    REQUIRE((RCC->CFGR & RCC_CFGR_TIMPRE) == 0);
    uint32_t ppre2 = (RCC->D2CFGR & RCC_D2CFGR_D2PPRE2_Msk)
                  >> RCC_D2CFGR_D2PPRE2_Pos;
    uint32_t timClk = (apbDivider(ppre2) == 1) ? tree.pclk2 : 2 * tree.pclk2;
    REQUIRE(timClk == 200000000);
}
