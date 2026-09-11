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
 * definitions needed by Lptim.hpp and DmaStream.hpp are provided. Register
 * writes are recorded in a log so that tests can check their order.
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
 * Interrupt controller
 */
typedef enum {
    DMA1_Stream0_IRQn = 11,
    DMA1_Stream1_IRQn = 12,
    DMA1_Stream2_IRQn = 13,
    DMA1_Stream3_IRQn = 14,
    DMA1_Stream4_IRQn = 15,
    DMA1_Stream5_IRQn = 16,
    DMA1_Stream6_IRQn = 17,
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

static inline void NVIC_EnableIRQ(IRQn_Type irq)
{
    (void)irq;
}

static inline void NVIC_DisableIRQ(IRQn_Type irq)
{
    (void)irq;
}

static inline void NVIC_ClearPendingIRQ(IRQn_Type irq)
{
    (void)irq;
}

static inline void NVIC_SetPendingIRQ(IRQn_Type irq)
{
    (void)irq;
}

static inline void NVIC_SetPriority(IRQn_Type irq, uint32_t prio)
{
    (void)irq;
    (void)prio;
}

#endif /* FAKE_STM32XX_H */
