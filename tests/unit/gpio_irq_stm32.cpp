/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Host tests for the STM32 gpio external interrupt driver
 * platform/drivers/GPIO/gpio_irq_stm32.cpp, compiled against fake EXTI,
 * SYSCFG, RCC and NVIC registers. The same test source is built twice: with
 * STM32H743xx defined against tests/unit/stubs/stm32h7xx (EXTI_D1, FTSR1,
 * APB4) and with STM32F405xx defined against tests/unit/fake_stm32 (EXTI
 * IMR/FTSR/PR, APB2).
 *
 * The fake pending register is plain memory, not "write 1 to clear": the
 * value left in it by a handler is the set of lines the driver acknowledged,
 * and the tests model the hardware clear by zeroing it themselves before
 * re-entering a handler.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>

#include "peripherals/gpio_irq.h"

#if defined(STM32H743xx)
#include "stm32h7xx.h"
#elif defined(STM32F405xx)
#include "stm32f4xx.h"
#else
#error "select the family with STM32H743xx or STM32F405xx"
#endif

/* Interrupt handlers of the driver, C++ linkage as in the vector table. */
void EXTI0_IRQHandler();
void EXTI1_IRQHandler();
void EXTI2_IRQHandler();
void EXTI3_IRQHandler();
void EXTI4_IRQHandler();
void EXTI9_5_IRQHandler();
void EXTI15_10_IRQHandler();

/*
 * Family-specific register access. The H7 build checks the CPU1 view of the
 * mask and pending registers (EXTI_D1), which aliases the EXTI block at
 * offset 0x80 as on the real chip.
 */
#if defined(STM32H743xx)

RCC_TypeDef stm32h7FakeRcc;
EXTI_TypeDef stm32h7FakeExti;
SYSCFG_TypeDef stm32h7FakeSyscfg;

#define REG_IMR (EXTI_D1->IMR1)
#define REG_PR (EXTI_D1->PR1)
#define REG_FTSR (EXTI->FTSR1)
#define REG_RTSR (EXTI->RTSR1)
#define REG_SYSCFG_CLK (RCC->APB4ENR)
#define SYSCFG_CLK_EN RCC_APB4ENR_SYSCFGEN

static void resetRegisters()
{
    memset(&stm32h7FakeRcc, 0, sizeof(stm32h7FakeRcc));
    memset(&stm32h7FakeExti, 0, sizeof(stm32h7FakeExti));
    memset(&stm32h7FakeSyscfg, 0, sizeof(stm32h7FakeSyscfg));
}

#else /* STM32F405xx */

RCC_TypeDef fakeRcc;
EXTI_TypeDef fakeExti;
SYSCFG_TypeDef fakeSyscfg;

#define REG_IMR (EXTI->IMR)
#define REG_PR (EXTI->PR)
#define REG_FTSR (EXTI->FTSR)
#define REG_RTSR (EXTI->RTSR)
#define REG_SYSCFG_CLK (RCC->APB2ENR)
#define SYSCFG_CLK_EN RCC_APB2ENR_SYSCFGEN

static void resetRegisters()
{
    memset(&fakeRcc, 0, sizeof(fakeRcc));
    memset(&fakeExti, 0, sizeof(fakeExti));
    memset(&fakeSyscfg, 0, sizeof(fakeSyscfg));
}

#endif

static uint32_t regImr()
{
    return REG_IMR;
}

static uint32_t regPr()
{
    return REG_PR;
}

static void setPr(const uint32_t value)
{
    REG_PR = value;
}

static uint32_t regFtsr()
{
    return REG_FTSR;
}

static uint32_t regRtsr()
{
    return REG_RTSR;
}

static uint32_t regExticr(const unsigned idx)
{
    return SYSCFG->EXTICR[idx];
}

static void setExticr(const unsigned idx, const uint32_t value)
{
    SYSCFG->EXTICR[idx] = value;
}

static bool syscfgClockEnabled()
{
    return (REG_SYSCFG_CLK & SYSCFG_CLK_EN) != 0;
}

static bool nvicEnabled(const IRQn_Type irq)
{
    return fakeNvic().enabled[irq];
}

static uint32_t nvicPriority(const IRQn_Type irq)
{
    return fakeNvic().priority[irq];
}

static unsigned nvicPendingCleared(const IRQn_Type irq)
{
    return fakeNvic().pendingCleared[irq];
}

/*
 * Gpio ports and pins: the port descriptor carries the address of the
 * register block, from which the driver derives the SYSCFG_EXTICR code, as
 * the GpioA..GpioE descriptors of gpio_stm32.c do.
 */
static const struct gpioDev portA = { nullptr, GPIOA };
static const struct gpioDev portB = { nullptr, GPIOB };
static const struct gpioDev portC = { nullptr, GPIOC };
static const struct gpioDev notAPort = { nullptr, nullptr };

/* An address inside a port block is not a port either. */
static const uintptr_t midPortAddr = GPIOC_BASE + 0x10;
static const struct gpioDev misaligned = { nullptr, (void *)midPortAddr };

static const struct gpioPin pa0 = { &portA, 0 };
static const struct gpioPin pc0 = { &portC, 0 };
static const struct gpioPin pc1 = { &portC, 1 };
static const struct gpioPin pc2 = { &portC, 2 };
static const struct gpioPin pc3 = { &portC, 3 };
static const struct gpioPin pc5 = { &portC, 5 };
static const struct gpioPin pb7 = { &portB, 7 };
static const struct gpioPin pb11 = { &portB, 11 };
static const struct gpioPin pc16 = { &portC, 16 };
static const struct gpioPin bogus = { &notAPort, 0 };
static const struct gpioPin offPort = { &misaligned, 0 };

/**
 * Callback record: number of calls, argument and pending register as seen
 * from inside the callback.
 */
struct CallRecord {
    unsigned calls = 0;
    void *lastArg = nullptr;
    uint32_t prInCallback = 0;
};

static void recordCall(void *arg)
{
    CallRecord *rec = static_cast<CallRecord *>(arg);
    rec->calls += 1;
    rec->lastArg = arg;
    rec->prInCallback = regPr();
}

/**
 * Detach every pin used by the tests, so that each test starts from an
 * empty driver state, then clear the fake registers and the fake NVIC.
 */
static void cleanup()
{
    gpioIrq_detach(&pa0);
    gpioIrq_detach(&pc0);
    gpioIrq_detach(&pc1);
    gpioIrq_detach(&pc2);
    gpioIrq_detach(&pc3);
    gpioIrq_detach(&pc5);
    gpioIrq_detach(&pb7);
    gpioIrq_detach(&pb11);
    resetRegisters();
    fakeNvic() = FakeNvic();
}

static constexpr uint32_t EXTI_IRQ_PRIORITY = 14;

TEST_CASE("Attaching PC0 on the falling edge programs SYSCFG, EXTI and NVIC",
          "[stm32][gpio_irq]")
{
    cleanup();
    CallRecord rec;

    /* Other lines of EXTICR1 already routed: they must be preserved. */
    setExticr(0, 0x1110);

    REQUIRE(gpioIrq_attach(&pc0, GPIO_IRQ_FALLING, recordCall, &rec) == 0);

    REQUIRE(syscfgClockEnabled());
    REQUIRE(SYSCFG_EXTICR1_EXTI0_PC == 2);
    REQUIRE(regExticr(0) == (0x1110 | SYSCFG_EXTICR1_EXTI0_PC));
    REQUIRE(regFtsr() == (1U << 0));
    REQUIRE(regRtsr() == 0);
    REQUIRE(regImr() == (1U << 0));

    REQUIRE(nvicEnabled(EXTI0_IRQn));
    REQUIRE(nvicPriority(EXTI0_IRQn) == EXTI_IRQ_PRIORITY);
    REQUIRE(nvicPendingCleared(EXTI0_IRQn) == 1);

    /* Nothing else was unmasked or enabled. */
    REQUIRE(nvicEnabled(EXTI1_IRQn) == false);
    REQUIRE(nvicEnabled(EXTI9_5_IRQn) == false);
    REQUIRE(nvicEnabled(EXTI15_10_IRQn) == false);

    cleanup();
}

TEST_CASE("Interrupt entry runs the callback once and acknowledges the line",
          "[stm32][gpio_irq]")
{
    cleanup();
    CallRecord rec;
    REQUIRE(gpioIrq_attach(&pc0, GPIO_IRQ_FALLING, recordCall, &rec) == 0);

    /* Line 0 pending, plus a line outside the group that must be left. */
    setPr((1U << 0) | (1U << 15));
    EXTI0_IRQHandler();

    REQUIRE(rec.calls == 1);
    REQUIRE(rec.lastArg == &rec);

    /* Acknowledged before the callback ran, and only line 0. */
    REQUIRE(rec.prInCallback == (1U << 0));
    REQUIRE(regPr() == (1U << 0));

    /* Hardware clear, then a spurious entry: no callback. */
    setPr(0);
    EXTI0_IRQHandler();
    REQUIRE(rec.calls == 1);

    /* A new edge is served again. */
    setPr(1U << 0);
    EXTI0_IRQHandler();
    REQUIRE(rec.calls == 2);

    cleanup();
}

TEST_CASE("Rising and both edges select the trigger registers",
          "[stm32][gpio_irq]")
{
    cleanup();
    CallRecord rec;

    /* Stale falling trigger on line 1 from a previous user. */
    REQUIRE(gpioIrq_attach(&pc1, GPIO_IRQ_FALLING, recordCall, &rec) == 0);
    REQUIRE(regFtsr() == (1U << 1));
    gpioIrq_detach(&pc1);
    REQUIRE(regFtsr() == 0);

    REQUIRE(gpioIrq_attach(&pc1, GPIO_IRQ_RISING, recordCall, &rec) == 0);
    REQUIRE(regRtsr() == (1U << 1));
    REQUIRE(regFtsr() == 0);
    REQUIRE(regImr() == (1U << 1));
    REQUIRE(nvicEnabled(EXTI1_IRQn));
    gpioIrq_detach(&pc1);

    REQUIRE(gpioIrq_attach(&pc2, GPIO_IRQ_BOTH, recordCall, &rec) == 0);
    REQUIRE(regRtsr() == (1U << 2));
    REQUIRE(regFtsr() == (1U << 2));
    REQUIRE(regImr() == (1U << 2));
    REQUIRE(nvicEnabled(EXTI2_IRQn));

    setPr(1U << 2);
    EXTI2_IRQHandler();
    REQUIRE(rec.calls == 1);

    cleanup();
}

TEST_CASE("The four C6000 lines PC0..PC3 attach side by side",
          "[stm32][gpio_irq]")
{
    cleanup();
    CallRecord rec[4];

    REQUIRE(gpioIrq_attach(&pc0, GPIO_IRQ_FALLING, recordCall, &rec[0]) == 0);
    REQUIRE(gpioIrq_attach(&pc1, GPIO_IRQ_FALLING, recordCall, &rec[1]) == 0);
    REQUIRE(gpioIrq_attach(&pc2, GPIO_IRQ_FALLING, recordCall, &rec[2]) == 0);
    REQUIRE(gpioIrq_attach(&pc3, GPIO_IRQ_FALLING, recordCall, &rec[3]) == 0);

    /* EXTICR1: PC in every nibble. */
    REQUIRE(regExticr(0) == 0x2222);
    REQUIRE(regFtsr() == 0x000F);
    REQUIRE(regRtsr() == 0);
    REQUIRE(regImr() == 0x000F);

    /* Each line has its own vector and only serves itself. */
    setPr(0x000F);
    EXTI3_IRQHandler();
    REQUIRE(rec[0].calls == 0);
    REQUIRE(rec[1].calls == 0);
    REQUIRE(rec[2].calls == 0);
    REQUIRE(rec[3].calls == 1);
    REQUIRE(regPr() == (1U << 3));

    setPr(0x0007);
    EXTI1_IRQHandler();
    REQUIRE(rec[1].calls == 1);
    REQUIRE(rec[1].lastArg == &rec[1]);
    REQUIRE(regPr() == (1U << 1));

    cleanup();
}

TEST_CASE("A line in use cannot be attached twice", "[stm32][gpio_irq]")
{
    cleanup();
    CallRecord first;
    CallRecord second;

    REQUIRE(gpioIrq_attach(&pc0, GPIO_IRQ_FALLING, recordCall, &first) == 0);

    /* Same pin, and another port on the same line. */
    REQUIRE(gpioIrq_attach(&pc0, GPIO_IRQ_RISING, recordCall, &second) == -1);
    REQUIRE(gpioIrq_attach(&pa0, GPIO_IRQ_RISING, recordCall, &second) == -1);

    /* The first setup is untouched. */
    REQUIRE(regExticr(0) == SYSCFG_EXTICR1_EXTI0_PC);
    REQUIRE(regFtsr() == (1U << 0));
    REQUIRE(regRtsr() == 0);

    setPr(1U << 0);
    EXTI0_IRQHandler();
    REQUIRE(first.calls == 1);
    REQUIRE(second.calls == 0);

    cleanup();
}

TEST_CASE("Detaching masks the line, drops the callback and frees the line",
          "[stm32][gpio_irq]")
{
    cleanup();
    CallRecord rec;
    REQUIRE(gpioIrq_attach(&pc0, GPIO_IRQ_FALLING, recordCall, &rec) == 0);

    gpioIrq_detach(&pc0);

    REQUIRE(regImr() == 0);
    REQUIRE(regFtsr() == 0);
    REQUIRE(regRtsr() == 0);
    REQUIRE(nvicEnabled(EXTI0_IRQn) == false);

    /* A pending edge after the detach is acknowledged but not served. */
    setPr(1U << 0);
    EXTI0_IRQHandler();
    REQUIRE(rec.calls == 0);
    REQUIRE(regPr() == (1U << 0));

    /* Detaching twice is harmless and the line is available again. */
    gpioIrq_detach(&pc0);
    REQUIRE(gpioIrq_attach(&pa0, GPIO_IRQ_FALLING, recordCall, &rec) == 0);
    REQUIRE(regExticr(0) == SYSCFG_EXTICR1_EXTI0_PA);
    REQUIRE(regImr() == (1U << 0));

    cleanup();
}

TEST_CASE("EXTI9_5 dispatches to the pending lines of its group",
          "[stm32][gpio_irq]")
{
    cleanup();
    CallRecord rec5;
    CallRecord rec7;

    REQUIRE(gpioIrq_attach(&pc5, GPIO_IRQ_FALLING, recordCall, &rec5) == 0);
    REQUIRE(gpioIrq_attach(&pb7, GPIO_IRQ_FALLING, recordCall, &rec7) == 0);

    /* EXTICR2: line 5 in bits 7:4 = PC, line 7 in bits 15:12 = PB */
    REQUIRE(regExticr(1)
            == (SYSCFG_EXTICR2_EXTI5_PC | (SYSCFG_EXTICR1_EXTI0_PB << 12)));
    REQUIRE(regImr() == ((1U << 5) | (1U << 7)));
    REQUIRE(nvicEnabled(EXTI9_5_IRQn));
    REQUIRE(nvicPriority(EXTI9_5_IRQn) == EXTI_IRQ_PRIORITY);

    setPr(1U << 7);
    EXTI9_5_IRQHandler();
    REQUIRE(rec5.calls == 0);
    REQUIRE(rec7.calls == 1);
    REQUIRE(regPr() == (1U << 7));

    setPr((1U << 5) | (1U << 7));
    EXTI9_5_IRQHandler();
    REQUIRE(rec5.calls == 1);
    REQUIRE(rec7.calls == 2);

    /* Line 6 pending without a callback: acknowledged, nobody called. */
    setPr(1U << 6);
    EXTI9_5_IRQHandler();
    REQUIRE(rec5.calls == 1);
    REQUIRE(rec7.calls == 2);
    REQUIRE(regPr() == (1U << 6));

    /* The shared interrupt stays enabled until the last line goes. */
    gpioIrq_detach(&pc5);
    REQUIRE(regImr() == (1U << 7));
    REQUIRE(nvicEnabled(EXTI9_5_IRQn));

    gpioIrq_detach(&pb7);
    REQUIRE(regImr() == 0);
    REQUIRE(nvicEnabled(EXTI9_5_IRQn) == false);

    cleanup();
}

TEST_CASE("EXTI15_10 dispatches to its line and leaves the others pending",
          "[stm32][gpio_irq]")
{
    cleanup();
    CallRecord rec;

    REQUIRE(gpioIrq_attach(&pb11, GPIO_IRQ_FALLING, recordCall, &rec) == 0);

    /* EXTICR3: line 11 in bits 15:12 = PB */
    REQUIRE(regExticr(2) == SYSCFG_EXTICR3_EXTI11_PB);
    REQUIRE(regImr() == (1U << 11));
    REQUIRE(nvicEnabled(EXTI15_10_IRQn));

    /* Line 3 belongs to EXTI3: it must not be acknowledged from here. */
    setPr((1U << 11) | (1U << 3));
    EXTI15_10_IRQHandler();
    REQUIRE(rec.calls == 1);
    REQUIRE(regPr() == (1U << 11));

    /* The dedicated handlers ignore the group lines. */
    setPr(1U << 11);
    EXTI3_IRQHandler();
    EXTI4_IRQHandler();
    REQUIRE(rec.calls == 1);
    REQUIRE(regPr() == 0);

    gpioIrq_detach(&pb11);
    REQUIRE(nvicEnabled(EXTI15_10_IRQn) == false);

    cleanup();
}

TEST_CASE("Invalid pins and callbacks are refused", "[stm32][gpio_irq]")
{
    cleanup();
    CallRecord rec;

    REQUIRE(gpioIrq_attach(&pc16, GPIO_IRQ_FALLING, recordCall, &rec) == -1);
    REQUIRE(gpioIrq_attach(&bogus, GPIO_IRQ_FALLING, recordCall, &rec) == -1);
    REQUIRE(gpioIrq_attach(&offPort, GPIO_IRQ_FALLING, recordCall, &rec) == -1);
    REQUIRE(gpioIrq_attach(&pc0, GPIO_IRQ_FALLING, nullptr, &rec) == -1);
    REQUIRE(gpioIrq_attach(nullptr, GPIO_IRQ_FALLING, recordCall, &rec) == -1);

    REQUIRE(regImr() == 0);
    REQUIRE(syscfgClockEnabled() == false);
    REQUIRE(nvicEnabled(EXTI0_IRQn) == false);

    /* Line 0 is still free. */
    REQUIRE(gpioIrq_attach(&pc0, GPIO_IRQ_FALLING, recordCall, &rec) == 0);

    gpioIrq_detach(&pc16);
    gpioIrq_detach(nullptr);
    REQUIRE(regImr() == (1U << 0));

    cleanup();
}
