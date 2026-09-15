/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * GPIO external interrupts (EXTI) for the STM32F4 and STM32H7 families.
 *
 * Both families route pin N of the port selected in SYSCFG_EXTICR to EXTI
 * line N (RM0090 section 12.2, RM0433 section 20.4): lines 0-4 have their
 * own NVIC interrupt, lines 5-9 and 10-15 share one each. The differences
 * are only in the register names: the F4 has EXTI_IMR/FTSR/RTSR/PR and the
 * SYSCFG clock on APB2, the H7 has EXTI_FTSR1/RTSR1 plus the per-core mask
 * and pending registers EXTI_D1 (CPU1, the Cortex-M7) and the SYSCFG clock
 * on APB4.
 *
 * This file defines the EXTI0..EXTI4, EXTI9_5 and EXTI15_10 interrupt
 * handlers, so it can be compiled only in targets where nobody else does:
 * chSelector_UV3x0.c and chSelector_MD9600.c define EXTI15_10_IRQHandler for
 * the MD-UV3x0, DM-1701 and MD-9600 targets. Today it is built for the
 * CS7000 and the CS7000-PLUS only, for the HR_C6000 interrupt lines.
 *
 * STM32H743 note: on the CS7000-PLUS the C6000 lines RF_TX_INTER and
 * SYS_INTER land on the pads PC2_C and PC3_C (schematic V01), which reach
 * the PC2/PC3 GPIO logic through the analogue switches controlled by
 * SYSCFG_PMCR PC2SO/PC3SO (DS12110 pin table, note 6). The switches are
 * closed at reset and nothing in the tree opens them, so the pads behave as
 * ordinary GPIO/EXTI inputs; this driver deliberately does not touch
 * SYSCFG_PMCR.
 * UNVERIFIED on hardware: EXTI on PC2_C/PC3_C with the switches closed.
 *
 * The handlers run in the Miosix "naked" form, with the thread context saved
 * around the dispatcher: a callback may therefore wake a thread with
 * Thread::IRQwakeup() and request a switch with Scheduler::IRQfindNextThread()
 * exactly as the DMA handlers of the display and audio drivers do.
 */

#include <kernel/scheduler/scheduler.h>
#include <miosix.h>
#include <stddef.h>
#include <stdint.h>
#include "peripherals/gpio_irq.h"

#if defined(STM32H743xx)
#include "stm32h7xx.h"
#elif defined(STM32F405xx)
#include "stm32f4xx.h"
#else
#error "gpio_irq_stm32.cpp: unsupported STM32 family"
#endif

/*
 * NVIC priority of the EXTI interrupts: below the Miosix tick and the SPI DMA
 * transfers (priority 15 is the lowest), same as the USB and GPS drivers.
 */
static constexpr uint32_t EXTI_IRQ_PRIORITY = 14;

/* Register access and highest gpio port index (A = 0) by family. */
#if defined(STM32H743xx)
#define EXTI_IMR_REG (EXTI_D1->IMR1)
#define EXTI_PR_REG (EXTI_D1->PR1)
#define EXTI_FTSR_REG (EXTI->FTSR1)
#define EXTI_RTSR_REG (EXTI->RTSR1)
#define SYSCFG_CLK_REG (RCC->APB4ENR)
#define SYSCFG_CLK_EN RCC_APB4ENR_SYSCFGEN
static constexpr uintptr_t MAX_PORT_INDEX = 10; /* GPIOK */
#else
#define EXTI_IMR_REG (EXTI->IMR)
#define EXTI_PR_REG (EXTI->PR)
#define EXTI_FTSR_REG (EXTI->FTSR)
#define EXTI_RTSR_REG (EXTI->RTSR)
#define SYSCFG_CLK_REG (RCC->APB2ENR)
#define SYSCFG_CLK_EN RCC_APB2ENR_SYSCFGEN
static constexpr uintptr_t MAX_PORT_INDEX = 8; /* GPIOI */
#endif

/* Masks of the lines served by the shared NVIC interrupts. */
static constexpr uint32_t EXTI9_5_LINES = 0x03E0;
static constexpr uint32_t EXTI15_10_LINES = 0xFC00;

struct extiLine {
    void (*cb)(void *arg);
    void *arg;
};

static struct extiLine lines[16];

/**
 * NVIC interrupt serving an EXTI line.
 */
static IRQn_Type lineIrq(const uint32_t line)
{
    if (line < 5)
        return static_cast<IRQn_Type>(EXTI0_IRQn + line);

    if (line < 10)
        return EXTI9_5_IRQn;

    return EXTI15_10_IRQn;
}

/**
 * Mask of the EXTI lines sharing the NVIC interrupt of a line.
 */
static uint32_t irqGroupMask(const uint32_t line)
{
    if (line < 5)
        return 1U << line;

    if (line < 10)
        return EXTI9_5_LINES;

    return EXTI15_10_LINES;
}

/**
 * Index of the gpio port in SYSCFG_EXTICR (A = 0, B = 1, ...), or a negative
 * value if the descriptor does not point to an STM32 gpio port. The STM32
 * gpio device descriptors carry the address of the port register block,
 * consecutive blocks being 1 kB apart.
 */
static int portIndex(const struct gpioPin *pin)
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(pin->port->priv);

    if (base < GPIOA_BASE)
        return -1;

    const uintptr_t idx = (base - GPIOA_BASE) / 0x400;
    if ((idx > MAX_PORT_INDEX) || ((base - GPIOA_BASE) % 0x400 != 0))
        return -1;

    return static_cast<int>(idx);
}

int gpioIrq_attach(const struct gpioPin *pin, enum gpioIrqEdge edge,
                   void (*cb)(void *arg), void *arg)
{
    if ((pin == NULL) || (pin->port == NULL) || (cb == NULL))
        return -1;

    if (pin->pin > 15)
        return -1;

    const int port = portIndex(pin);
    if (port < 0)
        return -1;

    const uint32_t line = pin->pin;
    const uint32_t bit = 1U << line;
    const uint32_t crIdx = line / 4;
    const uint32_t crShift = (line % 4) * 4;

    miosix::FastInterruptDisableLock dLock;

    if (lines[line].cb != NULL)
        return -1;

    /* Route the port to the EXTI line through the SYSCFG multiplexer. */
    SYSCFG_CLK_REG |= SYSCFG_CLK_EN;
    (void)SYSCFG_CLK_REG; /* read back: delay after enabling the clock */

    SYSCFG->EXTICR[crIdx] = (SYSCFG->EXTICR[crIdx] & ~(0xFU << crShift))
                          | (static_cast<uint32_t>(port) << crShift);

    /* Select the edges, then drop any event pending from an old setup. */
    if ((edge == GPIO_IRQ_FALLING) || (edge == GPIO_IRQ_BOTH))
        EXTI_FTSR_REG |= bit;
    else
        EXTI_FTSR_REG &= ~bit;

    if ((edge == GPIO_IRQ_RISING) || (edge == GPIO_IRQ_BOTH))
        EXTI_RTSR_REG |= bit;
    else
        EXTI_RTSR_REG &= ~bit;

    EXTI_PR_REG = bit;

    /* Install the callback before unmasking the line. */
    lines[line].cb = cb;
    lines[line].arg = arg;

    EXTI_IMR_REG |= bit;

    const IRQn_Type irq = lineIrq(line);
    NVIC_ClearPendingIRQ(irq);
    NVIC_SetPriority(irq, EXTI_IRQ_PRIORITY);
    NVIC_EnableIRQ(irq);

    return 0;
}

void gpioIrq_detach(const struct gpioPin *pin)
{
    if ((pin == NULL) || (pin->pin > 15))
        return;

    const uint32_t line = pin->pin;
    const uint32_t bit = 1U << line;

    miosix::FastInterruptDisableLock dLock;

    if (lines[line].cb == NULL)
        return;

    EXTI_IMR_REG &= ~bit;
    EXTI_FTSR_REG &= ~bit;
    EXTI_RTSR_REG &= ~bit;
    EXTI_PR_REG = bit;

    lines[line].cb = NULL;
    lines[line].arg = NULL;

    /*
     * A shared NVIC interrupt stays enabled while another line of its group
     * is attached.
     */
    const uint32_t group = irqGroupMask(line);
    bool inUse = false;

    for (uint32_t i = 0; i < 16; i++) {
        if (((group & (1U << i)) != 0) && (lines[i].cb != NULL))
            inUse = true;
    }

    if (inUse == false)
        NVIC_DisableIRQ(lineIrq(line));
}

/**
 * Interrupt dispatcher shared by all the EXTI handlers: acknowledges the
 * pending lines of the group and runs their callbacks. Pending bits are
 * cleared before the callbacks run, so that an edge arriving during a
 * callback is not lost. Lines without a callback are acknowledged too, to
 * avoid an interrupt storm from a line unmasked by someone else.
 *
 * C linkage: the naked handlers below branch to it by name.
 */
extern "C" void __attribute__((used)) gpioIrqStm32_dispatch(uint32_t group)
{
    const uint32_t pending = EXTI_PR_REG & group;
    EXTI_PR_REG = pending;

    for (uint32_t i = 0; i < 16; i++) {
        if ((pending & (1U << i)) == 0)
            continue;

        if (lines[i].cb != NULL)
            lines[i].cb(lines[i].arg);
    }
}

/*
 * Interrupt handlers. The vector tables of platform/mcu/STM32{F4,H7}xx/boot/
 * startup.cpp declare them with C++ linkage, hence no extern "C" here.
 *
 * On the Cortex-M targets they are naked and save/restore the thread context
 * around the dispatcher (Miosix requirement for an ISR that may cause a
 * context switch); on the host, for the unit tests, they call the dispatcher
 * directly.
 */
#if defined(_ARCH_CORTEXM7) || defined(_ARCH_CORTEXM4)
#define EXTI_HANDLER(name, group)                 \
    void __attribute__((naked, used)) name()      \
    {                                             \
        saveContext();                            \
        asm volatile("movw r0, #" #group);        \
        asm volatile("bl gpioIrqStm32_dispatch"); \
        restoreContext();                         \
    }
#else
#define EXTI_HANDLER(name, group)     \
    void name()                       \
    {                                 \
        gpioIrqStm32_dispatch(group); \
    }
#endif

EXTI_HANDLER(EXTI0_IRQHandler, 0x0001)
EXTI_HANDLER(EXTI1_IRQHandler, 0x0002)
EXTI_HANDLER(EXTI2_IRQHandler, 0x0004)
EXTI_HANDLER(EXTI3_IRQHandler, 0x0008)
EXTI_HANDLER(EXTI4_IRQHandler, 0x0010)
EXTI_HANDLER(EXTI9_5_IRQHandler, 0x03E0)
EXTI_HANDLER(EXTI15_10_IRQHandler, 0xFC00)
