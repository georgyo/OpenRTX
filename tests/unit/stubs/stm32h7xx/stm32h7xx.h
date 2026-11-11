/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef STM32H7XX_FAKE_H
#define STM32H7XX_FAKE_H

/*
 * Host-side stand-in for the CMSIS "stm32h7xx.h" device header, used to
 * compile platform/mcu/STM32H7xx/drivers/rcc.cpp and
 * platform/drivers/GPIO/gpio_irq_stm32.cpp in the unit tests.
 *
 * Only the RCC, PWR and FLASH register blocks touched by the clock tree setup
 * and the SYSCFG, EXTI, GPIO and NVIC pieces touched by the gpio interrupt
 * driver are provided. Register layouts, bit positions and peripheral base
 * addresses are copied verbatim from stm32h743xx.h, so a test decoding the
 * values that startPll() writes sees exactly what the real hardware would.
 *
 * The register blocks are ordinary RAM: nothing ever sets a "ready" flag. To
 * let the busy-wait loops in startPll() terminate, every ready flag is
 * defined as the enable bit it depends on (a clock is "ready" the instant it
 * is switched on) and the SWS status field mirrors the SW selection field.
 * Tests must never rely on those flags. Likewise the EXTI pending register
 * is plain memory, not "write 1 to clear": the value a handler leaves in it
 * is the set of lines it acknowledged.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    volatile uint32_t CR;
    volatile uint32_t HSICFGR;
    volatile uint32_t CRRCR;
    volatile uint32_t CSICFGR;
    volatile uint32_t CFGR;
    uint32_t RESERVED1;
    volatile uint32_t D1CFGR;
    volatile uint32_t D2CFGR;
    volatile uint32_t D3CFGR;
    uint32_t RESERVED2;
    volatile uint32_t PLLCKSELR;
    volatile uint32_t PLLCFGR;
    volatile uint32_t PLL1DIVR;
    volatile uint32_t PLL1FRACR;
    volatile uint32_t PLL2DIVR;
    volatile uint32_t PLL2FRACR;
    volatile uint32_t PLL3DIVR;
    volatile uint32_t PLL3FRACR;
    uint32_t RESERVED3;
    volatile uint32_t D1CCIPR;
    volatile uint32_t D2CCIP1R;
    volatile uint32_t D2CCIP2R;
    volatile uint32_t D3CCIPR;
    uint32_t RESERVED4;
    volatile uint32_t CIER;
    volatile uint32_t CIFR;
    volatile uint32_t CICR;
    uint32_t RESERVED5;
    volatile uint32_t BDCR;
    volatile uint32_t CSR;
    uint32_t RESERVED6;
    volatile uint32_t AHB3RSTR;
    volatile uint32_t AHB1RSTR;
    volatile uint32_t AHB2RSTR;
    volatile uint32_t AHB4RSTR;
    volatile uint32_t APB3RSTR;
    volatile uint32_t APB1LRSTR;
    volatile uint32_t APB1HRSTR;
    volatile uint32_t APB2RSTR;
    volatile uint32_t APB4RSTR;
    volatile uint32_t GCR;
    uint32_t RESERVED8;
    volatile uint32_t D3AMR;
    uint32_t RESERVED11[9];
    volatile uint32_t RSR;
    volatile uint32_t AHB3ENR;
    volatile uint32_t AHB1ENR;
    volatile uint32_t AHB2ENR;
    volatile uint32_t AHB4ENR;
    volatile uint32_t APB3ENR;
    volatile uint32_t APB1LENR;
    volatile uint32_t APB1HENR;
    volatile uint32_t APB2ENR;
    volatile uint32_t APB4ENR;
    uint32_t RESERVED12;
    volatile uint32_t AHB3LPENR;
    volatile uint32_t AHB1LPENR;
    volatile uint32_t AHB2LPENR;
    volatile uint32_t AHB4LPENR;
    volatile uint32_t APB3LPENR;
    volatile uint32_t APB1LLPENR;
    volatile uint32_t APB1HLPENR;
    volatile uint32_t APB2LPENR;
    volatile uint32_t APB4LPENR;
    uint32_t RESERVED13[4];
} RCC_TypeDef;

typedef struct {
    volatile uint32_t CR1;
    volatile uint32_t CSR1;
    volatile uint32_t CR2;
    volatile uint32_t CR3;
    volatile uint32_t CPUCR;
    uint32_t RESERVED0;
    volatile uint32_t D3CR;
    uint32_t RESERVED1;
    volatile uint32_t WKUPCR;
    volatile uint32_t WKUPFR;
    volatile uint32_t WKUPEPR;
} PWR_TypeDef;

typedef struct {
    volatile uint32_t ACR;
    volatile uint32_t KEYR1;
    volatile uint32_t OPTKEYR;
    volatile uint32_t CR1;
    volatile uint32_t SR1;
    volatile uint32_t CCR1;
    volatile uint32_t OPTCR;
    volatile uint32_t OPTSR_CUR;
    volatile uint32_t OPTSR_PRG;
    volatile uint32_t OPTCCR;
} FLASH_TypeDef;

/* Fake register blocks, defined by the test executable. */
extern RCC_TypeDef stm32h7FakeRcc;
extern PWR_TypeDef stm32h7FakePwr;
extern FLASH_TypeDef stm32h7FakeFlash;

#define RCC (&stm32h7FakeRcc)
#define PWR (&stm32h7FakePwr)
#define FLASH (&stm32h7FakeFlash)

/* Memory map (stm32h743xx.h) */
#define PERIPH_BASE (0x40000000UL)
#define D2_APB1PERIPH_BASE PERIPH_BASE
#define D2_APB2PERIPH_BASE (PERIPH_BASE + 0x00010000UL)
#define D2_AHB1PERIPH_BASE (PERIPH_BASE + 0x00020000UL)
#define D2_AHB2PERIPH_BASE (PERIPH_BASE + 0x08020000UL)
#define D1_APB1PERIPH_BASE (PERIPH_BASE + 0x10000000UL)
#define D1_AHB1PERIPH_BASE (PERIPH_BASE + 0x12000000UL)
#define D3_APB1PERIPH_BASE (PERIPH_BASE + 0x18000000UL)
#define D3_AHB1PERIPH_BASE (PERIPH_BASE + 0x18020000UL)
#define APB1PERIPH_BASE PERIPH_BASE
#define APB2PERIPH_BASE (PERIPH_BASE + 0x00010000UL)
#define AHB1PERIPH_BASE (PERIPH_BASE + 0x00020000UL)
#define AHB2PERIPH_BASE (PERIPH_BASE + 0x08000000UL)

#define LPTIM1_BASE (D2_APB1PERIPH_BASE + 0x2400UL)
#define TIM8_BASE (D2_APB2PERIPH_BASE + 0x0400UL)
#define USART6_BASE (D2_APB2PERIPH_BASE + 0x1400UL)
#define SPI4_BASE (D2_APB2PERIPH_BASE + 0x3400UL)
#define ADC1_BASE (D2_AHB1PERIPH_BASE + 0x2000UL)
#define WWDG1_BASE (D1_APB1PERIPH_BASE + 0x3000UL)
#define SPI6_BASE (D3_APB1PERIPH_BASE + 0x1400UL)

/* PWR */
#define PWR_CR3_SCUEN_Pos (2U)
#define PWR_CR3_SCUEN_Msk (0x1UL << PWR_CR3_SCUEN_Pos)
#define PWR_CR3_SCUEN PWR_CR3_SCUEN_Msk
#define PWR_D3CR_VOS_Pos (14U)
#define PWR_D3CR_VOS_Msk (0x3UL << PWR_D3CR_VOS_Pos)
#define PWR_D3CR_VOS_0 (0x1UL << PWR_D3CR_VOS_Pos)
#define PWR_D3CR_VOS_1 (0x2UL << PWR_D3CR_VOS_Pos)
#define PWR_D3CR_VOSRDY PWR_D3CR_VOS_0 /* fake: ready once VOS is written */

/* FLASH */
#define FLASH_ACR_LATENCY_Pos (0U)
#define FLASH_ACR_LATENCY_Msk (0xFUL << FLASH_ACR_LATENCY_Pos)
#define FLASH_ACR_LATENCY_3WS (0x00000003UL)
#define FLASH_ACR_WRHIGHFREQ_Pos (4U)
#define FLASH_ACR_WRHIGHFREQ_1 (0x2UL << FLASH_ACR_WRHIGHFREQ_Pos)

/* RCC_CR */
#define RCC_CR_HSEON_Pos (16U)
#define RCC_CR_HSEON_Msk (0x1UL << RCC_CR_HSEON_Pos)
#define RCC_CR_HSEON RCC_CR_HSEON_Msk
#define RCC_CR_HSERDY RCC_CR_HSEON_Msk /* fake: ready once enabled */
#define RCC_CR_PLL1ON_Pos (24U)
#define RCC_CR_PLL1ON_Msk (0x1UL << RCC_CR_PLL1ON_Pos)
#define RCC_CR_PLL1ON RCC_CR_PLL1ON_Msk
#define RCC_CR_PLL1RDY RCC_CR_PLL1ON_Msk /* fake: ready once enabled */
#define RCC_CR_PLL2ON_Pos (26U)
#define RCC_CR_PLL2ON_Msk (0x1UL << RCC_CR_PLL2ON_Pos)
#define RCC_CR_PLL2ON RCC_CR_PLL2ON_Msk
#define RCC_CR_PLL2RDY RCC_CR_PLL2ON_Msk /* fake: ready once enabled */

/* RCC_CFGR */
#define RCC_CFGR_SW_Pos (0U)
#define RCC_CFGR_SW_Msk (0x7UL << RCC_CFGR_SW_Pos)
#define RCC_CFGR_SW RCC_CFGR_SW_Msk
#define RCC_CFGR_SW_PLL1 (0x00000003UL)
#define RCC_CFGR_SWS RCC_CFGR_SW_Msk       /* fake: status mirrors SW */
#define RCC_CFGR_SWS_PLL1 RCC_CFGR_SW_PLL1 /* fake: status mirrors SW */
#define RCC_CFGR_TIMPRE_Pos (15U)
#define RCC_CFGR_TIMPRE_Msk (0x1UL << RCC_CFGR_TIMPRE_Pos)
#define RCC_CFGR_TIMPRE RCC_CFGR_TIMPRE_Msk

/* RCC_D1CFGR */
#define RCC_D1CFGR_HPRE_Pos (0U)
#define RCC_D1CFGR_HPRE_Msk (0xFUL << RCC_D1CFGR_HPRE_Pos)
#define RCC_D1CFGR_HPRE_DIV1 (0U)
#define RCC_D1CFGR_HPRE_DIV2 (0x8UL << RCC_D1CFGR_HPRE_Pos)
#define RCC_D1CFGR_D1PPRE_Pos (4U)
#define RCC_D1CFGR_D1PPRE_Msk (0x7UL << RCC_D1CFGR_D1PPRE_Pos)
#define RCC_D1CFGR_D1PPRE_DIV1 (0U)
#define RCC_D1CFGR_D1PPRE_DIV2 (0x4UL << RCC_D1CFGR_D1PPRE_Pos)
#define RCC_D1CFGR_D1CPRE_Pos (8U)
#define RCC_D1CFGR_D1CPRE_Msk (0xFUL << RCC_D1CFGR_D1CPRE_Pos)
#define RCC_D1CFGR_D1CPRE_DIV1 (0U)

/* RCC_D2CFGR */
#define RCC_D2CFGR_D2PPRE1_Pos (4U)
#define RCC_D2CFGR_D2PPRE1_Msk (0x7UL << RCC_D2CFGR_D2PPRE1_Pos)
#define RCC_D2CFGR_D2PPRE1_DIV1 (0U)
#define RCC_D2CFGR_D2PPRE1_DIV2 (0x4UL << RCC_D2CFGR_D2PPRE1_Pos)
#define RCC_D2CFGR_D2PPRE2_Pos (8U)
#define RCC_D2CFGR_D2PPRE2_Msk (0x7UL << RCC_D2CFGR_D2PPRE2_Pos)
#define RCC_D2CFGR_D2PPRE2_DIV1 (0U)
#define RCC_D2CFGR_D2PPRE2_DIV2 (0x4UL << RCC_D2CFGR_D2PPRE2_Pos)

/* RCC_D3CFGR */
#define RCC_D3CFGR_D3PPRE_Pos (4U)
#define RCC_D3CFGR_D3PPRE_Msk (0x7UL << RCC_D3CFGR_D3PPRE_Pos)
#define RCC_D3CFGR_D3PPRE_DIV1 (0U)
#define RCC_D3CFGR_D3PPRE_DIV2 (0x4UL << RCC_D3CFGR_D3PPRE_Pos)

/* RCC_PLLCKSELR */
#define RCC_PLLCKSELR_PLLSRC_Pos (0U)
#define RCC_PLLCKSELR_PLLSRC_Msk (0x3UL << RCC_PLLCKSELR_PLLSRC_Pos)
#define RCC_PLLCKSELR_PLLSRC_HSE (0x2UL << RCC_PLLCKSELR_PLLSRC_Pos)
#define RCC_PLLCKSELR_DIVM1_Pos (4U)
#define RCC_PLLCKSELR_DIVM1_Msk (0x3FUL << RCC_PLLCKSELR_DIVM1_Pos)
#define RCC_PLLCKSELR_DIVM1_0 (0x01UL << RCC_PLLCKSELR_DIVM1_Pos)
#define RCC_PLLCKSELR_DIVM1_2 (0x04UL << RCC_PLLCKSELR_DIVM1_Pos)
#define RCC_PLLCKSELR_DIVM2_Pos (12U)
#define RCC_PLLCKSELR_DIVM2_Msk (0x3FUL << RCC_PLLCKSELR_DIVM2_Pos)

/* RCC_PLLCFGR */
#define RCC_PLLCFGR_PLL1RGE_Pos (2U)
#define RCC_PLLCFGR_PLL1RGE_2 (0x2UL << RCC_PLLCFGR_PLL1RGE_Pos)
#define RCC_PLLCFGR_PLL2VCOSEL (0x1UL << 5U)
#define RCC_PLLCFGR_DIVP1EN (0x1UL << 16U)
#define RCC_PLLCFGR_DIVQ1EN (0x1UL << 17U)
#define RCC_PLLCFGR_DIVR1EN (0x1UL << 18U)
#define RCC_PLLCFGR_DIVP2EN (0x1UL << 19U)

/* RCC_PLL1DIVR / RCC_PLL2DIVR */
#define RCC_PLL1DIVR_N1_Pos (0U)
#define RCC_PLL1DIVR_N1_Msk (0x1FFUL << RCC_PLL1DIVR_N1_Pos)
#define RCC_PLL1DIVR_P1_Pos (9U)
#define RCC_PLL1DIVR_P1_Msk (0x7FUL << RCC_PLL1DIVR_P1_Pos)
#define RCC_PLL1DIVR_Q1_Pos (16U)
#define RCC_PLL1DIVR_Q1_Msk (0x7FUL << RCC_PLL1DIVR_Q1_Pos)
#define RCC_PLL1DIVR_R1_Pos (24U)
#define RCC_PLL1DIVR_R1_Msk (0x7FUL << RCC_PLL1DIVR_R1_Pos)
#define RCC_PLL2DIVR_N2_Pos (0U)
#define RCC_PLL2DIVR_N2_Msk (0x1FFUL << RCC_PLL2DIVR_N2_Pos)
#define RCC_PLL2DIVR_P2_Pos (9U)
#define RCC_PLL2DIVR_P2_Msk (0x7FUL << RCC_PLL2DIVR_P2_Pos)

/* RCC_D2CCIP1R / RCC_D2CCIP2R */
#define RCC_D2CCIP1R_SPI45SEL_Pos (16U)
#define RCC_D2CCIP1R_SPI45SEL_Msk (0x7UL << RCC_D2CCIP1R_SPI45SEL_Pos)
#define RCC_D2CCIP1R_SPI45SEL RCC_D2CCIP1R_SPI45SEL_Msk
#define RCC_D2CCIP2R_USART16SEL_Pos (3U)
#define RCC_D2CCIP2R_USART16SEL_Msk (0x7UL << RCC_D2CCIP2R_USART16SEL_Pos)
#define RCC_D2CCIP2R_USART16SEL RCC_D2CCIP2R_USART16SEL_Msk

/* RCC_APB4ENR */
#define RCC_APB4ENR_SYSCFGEN_Pos (1U)
#define RCC_APB4ENR_SYSCFGEN_Msk (0x1UL << RCC_APB4ENR_SYSCFGEN_Pos)
#define RCC_APB4ENR_SYSCFGEN RCC_APB4ENR_SYSCFGEN_Msk

/*
 * System configuration controller (prefix of the CMSIS layout, up to the
 * registers the drivers use).
 */
typedef struct {
    uint32_t RESERVED1;
    volatile uint32_t PMCR;
    volatile uint32_t EXTICR[4];
    volatile uint32_t CFGR;
    uint32_t RESERVED2;
    volatile uint32_t CCCSR;
    volatile uint32_t CCVR;
    volatile uint32_t CCCR;
    volatile uint32_t PWRCR;
} SYSCFG_TypeDef;

#define SYSCFG_PMCR_PC2SO_Pos (26U)
#define SYSCFG_PMCR_PC2SO_Msk (0x1UL << SYSCFG_PMCR_PC2SO_Pos)
#define SYSCFG_PMCR_PC2SO SYSCFG_PMCR_PC2SO_Msk
#define SYSCFG_PMCR_PC3SO_Pos (27U)
#define SYSCFG_PMCR_PC3SO_Msk (0x1UL << SYSCFG_PMCR_PC3SO_Pos)
#define SYSCFG_PMCR_PC3SO SYSCFG_PMCR_PC3SO_Msk

#define SYSCFG_EXTICR1_EXTI0_Pos (0U)
#define SYSCFG_EXTICR1_EXTI0_Msk (0xFUL << SYSCFG_EXTICR1_EXTI0_Pos)
#define SYSCFG_EXTICR1_EXTI0 SYSCFG_EXTICR1_EXTI0_Msk
#define SYSCFG_EXTICR1_EXTI0_PA (0x00000000U)
#define SYSCFG_EXTICR1_EXTI0_PB (0x00000001U)
#define SYSCFG_EXTICR1_EXTI0_PC (0x00000002U)
#define SYSCFG_EXTICR2_EXTI5_PC (0x00000020U)
#define SYSCFG_EXTICR3_EXTI11_PB (0x00001000U)

/*
 * External interrupt/event controller: the full block, and the CPU1 view
 * (IMR1/EMR1/PR1 and following) at offset 0x80 into it, as EXTI_D1 is on
 * the real chip.
 */
typedef struct {
    volatile uint32_t RTSR1;
    volatile uint32_t FTSR1;
    volatile uint32_t SWIER1;
    volatile uint32_t D3PMR1;
    volatile uint32_t D3PCR1L;
    volatile uint32_t D3PCR1H;
    uint32_t RESERVED1[2];
    volatile uint32_t RTSR2;
    volatile uint32_t FTSR2;
    volatile uint32_t SWIER2;
    volatile uint32_t D3PMR2;
    volatile uint32_t D3PCR2L;
    volatile uint32_t D3PCR2H;
    uint32_t RESERVED2[2];
    volatile uint32_t RTSR3;
    volatile uint32_t FTSR3;
    volatile uint32_t SWIER3;
    volatile uint32_t D3PMR3;
    volatile uint32_t D3PCR3L;
    volatile uint32_t D3PCR3H;
    uint32_t RESERVED3[10];
    volatile uint32_t IMR1;
    volatile uint32_t EMR1;
    volatile uint32_t PR1;
    uint32_t RESERVED4;
    volatile uint32_t IMR2;
    volatile uint32_t EMR2;
    volatile uint32_t PR2;
    uint32_t RESERVED5;
    volatile uint32_t IMR3;
    volatile uint32_t EMR3;
    volatile uint32_t PR3;
} EXTI_TypeDef;

typedef struct {
    volatile uint32_t IMR1;
    volatile uint32_t EMR1;
    volatile uint32_t PR1;
    uint32_t RESERVED1;
    volatile uint32_t IMR2;
    volatile uint32_t EMR2;
    volatile uint32_t PR2;
    uint32_t RESERVED2;
    volatile uint32_t IMR3;
    volatile uint32_t EMR3;
    volatile uint32_t PR3;
} EXTI_Core_TypeDef;

#define EXTI_IMR1_IM0 (0x1UL << 0U)
#define EXTI_PR1_PR0 (0x1UL << 0U)
#define EXTI_FTSR1_TR0 (0x1UL << 0U)
#define EXTI_RTSR1_TR0 (0x1UL << 0U)

/*
 * General purpose I/O: only the addresses of the port blocks matter to the
 * gpio interrupt driver, the layout is there for gpio_stm32.h.
 */
typedef struct {
    volatile uint32_t MODER;
    volatile uint32_t OTYPER;
    volatile uint32_t OSPEEDR;
    volatile uint32_t PUPDR;
    volatile uint32_t IDR;
    volatile uint32_t ODR;
    volatile uint32_t BSRR;
    volatile uint32_t LCKR;
    volatile uint32_t AFR[2];
} GPIO_TypeDef;

#define EXTI_BASE (D3_APB1PERIPH_BASE + 0x0000UL)
#define EXTI_D1_BASE (EXTI_BASE + 0x0080UL)
#define SYSCFG_BASE (D3_APB1PERIPH_BASE + 0x0400UL)
#define GPIOA_BASE (D3_AHB1PERIPH_BASE + 0x0000UL)
#define GPIOB_BASE (D3_AHB1PERIPH_BASE + 0x0400UL)
#define GPIOC_BASE (D3_AHB1PERIPH_BASE + 0x0800UL)
#define GPIOD_BASE (D3_AHB1PERIPH_BASE + 0x0C00UL)
#define GPIOE_BASE (D3_AHB1PERIPH_BASE + 0x1000UL)
#define GPIOF_BASE (D3_AHB1PERIPH_BASE + 0x1400UL)
#define GPIOG_BASE (D3_AHB1PERIPH_BASE + 0x1800UL)
#define GPIOH_BASE (D3_AHB1PERIPH_BASE + 0x1C00UL)
#define GPIOI_BASE (D3_AHB1PERIPH_BASE + 0x2000UL)
#define GPIOJ_BASE (D3_AHB1PERIPH_BASE + 0x2400UL)
#define GPIOK_BASE (D3_AHB1PERIPH_BASE + 0x2800UL)

/* Fake register blocks, defined by the test executable. */
extern SYSCFG_TypeDef stm32h7FakeSyscfg;
extern EXTI_TypeDef stm32h7FakeExti;

#define SYSCFG (&stm32h7FakeSyscfg)
#define EXTI (&stm32h7FakeExti)
#define EXTI_D1 ((EXTI_Core_TypeDef *)(&stm32h7FakeExti.IMR1))

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
    EXTI9_5_IRQn = 23,
    EXTI15_10_IRQn = 40,
} IRQn_Type;

#ifdef __cplusplus
}
#endif

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

#endif /* STM32H7XX_FAKE_H */
