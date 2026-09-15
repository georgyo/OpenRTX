/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef FAKE_STM32XX_H
#define FAKE_STM32XX_H

/*
 * Minimal stand-in for the STM32 CMSIS device headers, used to compile the
 * register-level STM32 drivers on the host for unit testing. Only the
 * definitions needed by Lptim.hpp, DmaStream.hpp and, in its STM32F4 build,
 * gpio_irq_stm32.cpp are provided (the STM32H7 build of the latter uses the
 * EXTI_D1 layout of tests/unit/stubs/stm32h7xx). Register writes are
 * recorded in a log so that tests can check their order.
 *
 * The EXTI pending register is plain memory, not "write 1 to clear": the
 * value a handler leaves in it is the set of lines it acknowledged.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/**
 * Log of register writes, as (register name, value written) pairs.
 */
using RegWriteLog = std::vector<std::pair<std::string, uint32_t>>;

/**
 * Fake peripheral register: behaves like a uint32_t but records every write.
 */
struct FakeReg {
    const char *name;
    RegWriteLog *log;
    uint32_t value;

    FakeReg &operator=(const uint32_t v)
    {
        value = v;
        if (log != nullptr)
            log->emplace_back(name, v);
        return *this;
    }

    FakeReg &operator|=(const uint32_t v)
    {
        return (*this = (value | v));
    }

    FakeReg &operator&=(const uint32_t v)
    {
        return (*this = (value & v));
    }

    operator uint32_t() const
    {
        return value;
    }
};

/*
 * Low-power timer
 */
typedef struct {
    FakeReg ISR;
    FakeReg ICR;
    FakeReg IER;
    FakeReg CFGR;
    FakeReg CR;
    FakeReg CMP;
    FakeReg ARR;
    FakeReg CNT;
} LPTIM_TypeDef;

#define LPTIM_ISR_ARROK (1U << 4)
#define LPTIM_ICR_ARROKCF (1U << 4)
#define LPTIM_CFGR_PRESC_Pos (9U)
#define LPTIM_CFGR_PRESC (7U << LPTIM_CFGR_PRESC_Pos)
#define LPTIM_CR_ENABLE (1U << 0)
#define LPTIM_CR_CNTSTRT (1U << 2)

/*
 * DMA controller and streams
 */
typedef struct {
    FakeReg CR;
    FakeReg NDTR;
    FakeReg PAR;
    FakeReg M0AR;
    FakeReg M1AR;
    FakeReg FCR;
} DMA_Stream_TypeDef;

typedef struct {
    FakeReg LISR;
    FakeReg HISR;
    FakeReg LIFCR;
    FakeReg HIFCR;
} DMA_TypeDef;

typedef struct {
    FakeReg AHB1ENR;
    FakeReg APB2ENR;
} RCC_TypeDef;

extern DMA_TypeDef fakeDma[2];
extern DMA_Stream_TypeDef fakeDmaStreams[16];
extern RCC_TypeDef fakeRcc;

#define DMA1 (&fakeDma[0])
#define DMA2 (&fakeDma[1])
#define DMA1_Stream0 (&fakeDmaStreams[0])
#define DMA2_Stream0 (&fakeDmaStreams[8])
#define DMA1_BASE ((uint32_t)0x40020000UL)
#define DMA2_BASE ((uint32_t)0x40020400UL)
#define RCC (&fakeRcc)
#define RCC_AHB1ENR_DMA1EN (1U << 0)
#define RCC_AHB1ENR_DMA2EN (1U << 1)
#define RCC_APB2ENR_SYSCFGEN (1U << 14)
#define RCC_SYNC() \
    do {           \
    } while (0)

#define DMA_SxCR_EN (1U << 0)
#define DMA_SxCR_DMEIE (1U << 1)
#define DMA_SxCR_TEIE (1U << 2)
#define DMA_SxCR_HTIE (1U << 3)
#define DMA_SxCR_TCIE (1U << 4)
#define DMA_SxCR_DIR_0 (1U << 6)
#define DMA_SxCR_CIRC (1U << 8)
#define DMA_SxCR_MINC (1U << 10)
#define DMA_SxCR_PSIZE_Pos (11U)
#define DMA_SxCR_MSIZE_Pos (13U)

#define DMA_LISR_FEIF0 (1U << 0)
#define DMA_LISR_DMEIF0 (1U << 2)
#define DMA_LISR_TEIF0 (1U << 3)
#define DMA_LISR_HTIF0 (1U << 4)
#define DMA_LISR_TCIF0 (1U << 5)

/*
 * System configuration controller and external interrupt controller, STM32F4
 * layout (stm32f405xx.h).
 */
typedef struct {
    FakeReg MEMRMP;
    FakeReg PMC;
    FakeReg EXTICR[4];
    uint32_t RESERVED[2];
    FakeReg CMPCR;
} SYSCFG_TypeDef;

typedef struct {
    FakeReg IMR;
    FakeReg EMR;
    FakeReg RTSR;
    FakeReg FTSR;
    FakeReg SWIER;
    FakeReg PR;
} EXTI_TypeDef;

#define SYSCFG_EXTICR1_EXTI0_PA (0x0000U)
#define SYSCFG_EXTICR1_EXTI0_PB (0x0001U)
#define SYSCFG_EXTICR1_EXTI0_PC (0x0002U)
#define SYSCFG_EXTICR2_EXTI5_PC (0x0020U)
#define SYSCFG_EXTICR3_EXTI11_PB (0x1000U)
#define EXTI_IMR_MR0 (1U << 0)
#define EXTI_PR_PR0 (1U << 0)
#define EXTI_FTSR_TR0 (1U << 0)
#define EXTI_RTSR_TR0 (1U << 0)

/*
 * General purpose I/O: only the addresses of the port blocks matter to the
 * gpio interrupt driver, the layout is there for gpio_stm32.h.
 */
typedef struct {
    FakeReg MODER;
    FakeReg OTYPER;
    FakeReg OSPEEDR;
    FakeReg PUPDR;
    FakeReg IDR;
    FakeReg ODR;
    FakeReg BSRR;
    FakeReg LCKR;
    FakeReg AFR[2];
} GPIO_TypeDef;

#define AHB1PERIPH_BASE ((uint32_t)0x40020000UL)
#define GPIOA_BASE (AHB1PERIPH_BASE + 0x0000UL)
#define GPIOB_BASE (AHB1PERIPH_BASE + 0x0400UL)
#define GPIOC_BASE (AHB1PERIPH_BASE + 0x0800UL)
#define GPIOD_BASE (AHB1PERIPH_BASE + 0x0C00UL)
#define GPIOE_BASE (AHB1PERIPH_BASE + 0x1000UL)
#define GPIOF_BASE (AHB1PERIPH_BASE + 0x1400UL)
#define GPIOG_BASE (AHB1PERIPH_BASE + 0x1800UL)
#define GPIOH_BASE (AHB1PERIPH_BASE + 0x1C00UL)
#define GPIOI_BASE (AHB1PERIPH_BASE + 0x2000UL)

extern SYSCFG_TypeDef fakeSyscfg;
extern EXTI_TypeDef fakeExti;

#define SYSCFG (&fakeSyscfg)
#define EXTI (&fakeExti)

/* The gpio ports are never dereferenced by the drivers under test. */
#define GPIOA ((GPIO_TypeDef *)GPIOA_BASE)
#define GPIOB ((GPIO_TypeDef *)GPIOB_BASE)
#define GPIOC ((GPIO_TypeDef *)GPIOC_BASE)
#define GPIOD ((GPIO_TypeDef *)GPIOD_BASE)
#define GPIOE ((GPIO_TypeDef *)GPIOE_BASE)

/*
 * Interrupt controller
 */
typedef enum {
    EXTI0_IRQn = 6,
    EXTI1_IRQn = 7,
    EXTI2_IRQn = 8,
    EXTI3_IRQn = 9,
    EXTI4_IRQn = 10,
    DMA1_Stream0_IRQn = 11,
    DMA1_Stream1_IRQn = 12,
    DMA1_Stream2_IRQn = 13,
    DMA1_Stream3_IRQn = 14,
    DMA1_Stream4_IRQn = 15,
    DMA1_Stream5_IRQn = 16,
    DMA1_Stream6_IRQn = 17,
    EXTI9_5_IRQn = 23,
    EXTI15_10_IRQn = 40,
    DMA1_Stream7_IRQn = 47,
    DMA2_Stream0_IRQn = 56,
    DMA2_Stream1_IRQn = 57,
    DMA2_Stream2_IRQn = 58,
    DMA2_Stream3_IRQn = 59,
    DMA2_Stream4_IRQn = 60,
    DMA2_Stream5_IRQn = 68,
    DMA2_Stream6_IRQn = 69,
    DMA2_Stream7_IRQn = 70,
} IRQn_Type;

/**
 * Fake NVIC: records what the drivers do with every interrupt number.
 */
struct FakeNvic {
    static constexpr unsigned NUM_IRQ = 160;

    bool enabled[NUM_IRQ] = {};
    uint32_t priority[NUM_IRQ] = {};
    unsigned pendingCleared[NUM_IRQ] = {};
    unsigned pendingSet[NUM_IRQ] = {};
};

inline FakeNvic &fakeNvic()
{
    static FakeNvic nvic;
    return nvic;
}

static inline void NVIC_EnableIRQ(IRQn_Type irq)
{
    fakeNvic().enabled[irq] = true;
}

static inline void NVIC_DisableIRQ(IRQn_Type irq)
{
    fakeNvic().enabled[irq] = false;
}

static inline void NVIC_ClearPendingIRQ(IRQn_Type irq)
{
    fakeNvic().pendingCleared[irq] += 1;
}

static inline void NVIC_SetPendingIRQ(IRQn_Type irq)
{
    fakeNvic().pendingSet[irq] += 1;
}

static inline void NVIC_SetPriority(IRQn_Type irq, uint32_t prio)
{
    fakeNvic().priority[irq] = prio;
}

#endif /* FAKE_STM32XX_H */
