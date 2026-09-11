/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host tests for the STM32H7 LPTIM driver, compiled against the fake register
 * definitions in tests/unit/fake_stm32.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>

#include "stm32h7xx.h"
#include "../../platform/mcu/STM32H7xx/drivers/Lptim.hpp"

/**
 * Fake LPTIM peripheral with a write log attached to each register.
 */
struct FakeLptim {
    RegWriteLog log;
    LPTIM_TypeDef regs;

    FakeLptim()
        : regs{ { "ISR", &log, 0 },  { "ICR", &log, 0 }, { "IER", &log, 0 },
                { "CFGR", &log, 0 }, { "CR", &log, 0 },  { "CMP", &log, 0 },
                { "ARR", &log, 0 },  { "CNT", &log, 0 } }
    {
    }

    Lptim driver(const uint32_t baseFreq = 168000000)
    {
        return Lptim(reinterpret_cast<uintptr_t>(&regs), baseFreq);
    }
};

TEST_CASE("LPTIM configuration is written with the timer disabled and the "
          "autoreload with the timer enabled",
          "[stm32][lptim]")
{
    FakeLptim tim;
    tim.regs.CR.value = LPTIM_CR_ENABLE; // Timer left enabled by a stop()
    tim.regs.ISR.value = LPTIM_ISR_ARROK;

    tim.driver().setUpdateFrequency(48000);

    REQUIRE(tim.log.size() == 5U);
    REQUIRE(tim.log[0].first == "CR");
    REQUIRE(tim.log[0].second == 0U);
    REQUIRE(tim.log[1].first == "CFGR");
    REQUIRE(tim.log[2].first == "CR");
    REQUIRE(tim.log[2].second == LPTIM_CR_ENABLE);
    REQUIRE(tim.log[3].first == "ICR");
    REQUIRE(tim.log[3].second == LPTIM_ICR_ARROKCF);
    REQUIRE(tim.log[4].first == "ARR");
}

TEST_CASE("LPTIM prescaler and autoreload values", "[stm32][lptim]")
{
    FakeLptim tim;
    tim.regs.ISR.value = LPTIM_ISR_ARROK;

    SECTION("48 kHz from 168 MHz needs no prescaler")
    {
        uint32_t freq = tim.driver().setUpdateFrequency(48000);

        REQUIRE(((tim.regs.CFGR & LPTIM_CFGR_PRESC) >> LPTIM_CFGR_PRESC_Pos)
                == 0U);
        REQUIRE(tim.regs.ARR == 3499U);
        REQUIRE(freq == 48000U);
    }

    SECTION("2 kHz CTCSS sample rate from 168 MHz needs a prescaler of 2")
    {
        uint32_t freq = tim.driver().setUpdateFrequency(2000);

        // 168 MHz / 2000 Hz = 84000 does not fit in the 16-bit autoreload
        REQUIRE(((tim.regs.CFGR & LPTIM_CFGR_PRESC) >> LPTIM_CFGR_PRESC_Pos)
                == 1U);
        REQUIRE(tim.regs.ARR == 41999U);
        REQUIRE(freq == 2000U);
    }

    SECTION("8 kHz from 168 MHz needs no prescaler")
    {
        uint32_t freq = tim.driver().setUpdateFrequency(8000);

        REQUIRE(((tim.regs.CFGR & LPTIM_CFGR_PRESC) >> LPTIM_CFGR_PRESC_Pos)
                == 0U);
        REQUIRE(tim.regs.ARR == 20999U);
        REQUIRE(freq == 8000U);
    }
}

TEST_CASE("LPTIM autoreload update wait is bounded", "[stm32][lptim]")
{
    FakeLptim tim;
    tim.regs.ISR.value = 0; // ARROK never set

    // Must return even if the timer never acknowledges the ARR write
    REQUIRE(tim.driver().setUpdateFrequency(48000) == 48000U);
    REQUIRE(tim.regs.ARR == 3499U);
}

TEST_CASE("LPTIM start and stop", "[stm32][lptim]")
{
    FakeLptim tim;
    tim.regs.ISR.value = LPTIM_ISR_ARROK;
    Lptim drv = tim.driver();

    drv.setUpdateFrequency(48000);
    drv.start();
    REQUIRE(tim.regs.CR == (LPTIM_CR_ENABLE | LPTIM_CR_CNTSTRT));

    drv.stop();
    REQUIRE(tim.regs.CR == LPTIM_CR_ENABLE);
}
