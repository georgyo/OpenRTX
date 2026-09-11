/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "stm32h7xx.h"
#include <stdbool.h>
#include <pthread.h>
#include <errno.h>
#include "adc_stm32.h"

/*
 * Upper bounds, in loop iterations, for the busy-wait loops of the ADC start-up
 * sequence. They only serve to avoid hanging the boot process if the ADC never
 * reaches the expected state: the ADC becomes ready in a few ADC clock cycles
 * and the calibration takes at most 165010 ADC clock cycles (~3.3ms at 50MHz).
 */
#define ADC_STARTUP_TIMEOUT   1000000
#define ADC_CAL_TIMEOUT       10000000

/**
 * \internal
 * Busy-wait until the bits selected by mask in a register are equal to a given
 * value, giving up after a maximum number of iterations.
 *
 * @param reg: pointer to the register to poll.
 * @param mask: bitmask selecting the bits to check.
 * @param value: expected value of the selected bits.
 * @param timeout: maximum number of polling iterations.
 * @return true if the condition was met, false on timeout.
 */
static bool adc_waitBits(volatile const uint32_t *reg, const uint32_t mask,
                         const uint32_t value, uint32_t timeout)
{
    while((*reg & mask) != value)
    {
        if(timeout == 0)
            return false;

        timeout -= 1;
    }

    return true;
}

int adcStm32_init(const struct Adc *adc)
{
    int ret = 0;

    /*
     * Configure ADC for synchronous clock mode (clocked by AHB clock), divided
     * by 4. This gives an ADC clock of 50MHz  when AHB clock is 200MHz.
     *
     * NOTE: ADC1 and ADC2 share the same clock tree!
     */

    switch((uint32_t) adc->priv)
    {
        case ADC1_BASE:
        case ADC2_BASE:
            RCC->AHB1ENR |= RCC_AHB1ENR_ADC12EN;
            __DSB();
            ADC12_COMMON->CCR = ADC_CCR_CKMODE_1
                              | ADC_CCR_CKMODE_0;
            break;

        case ADC3_BASE:
            RCC->AHB4ENR |= RCC_AHB4ENR_ADC3EN;
            __DSB();
            ADC3_COMMON->CCR = ADC_CCR_CKMODE_1
                             | ADC_CCR_CKMODE_0;
            break;

        default:
            return -EINVAL;
            break;
    }

    ADC_TypeDef *pAdc = ((ADC_TypeDef *) adc->priv);

    // Enable ADC voltage regulator, enable boost mode. Wait until LDO regulator
    // is ready.
    pAdc->CR = ADC_CR_ADVREGEN
             | ADC_CR_BOOST_1
             | ADC_CR_BOOST_0;

    if(adc_waitBits(&pAdc->ISR, ADC_ISR_LDORDY, ADC_ISR_LDORDY,
                    ADC_STARTUP_TIMEOUT) == false)
        ret = -EIO;

    // Start calibration, both offset and linearity. Calibration requires the
    // ADC to be disabled (ADEN = 0).
    pAdc->CR |= ADC_CR_ADCAL
             |  ADC_CR_ADCALLIN;

    if(adc_waitBits(&pAdc->CR, ADC_CR_ADCAL, 0, ADC_CAL_TIMEOUT) == false)
        ret = -EIO;

    /*
     * ADC clock is 50MHz. We set the sample time of each channel to 387.5 ADC
     * cycles, giving a total conversion time of ~7us.
     */
    pAdc->SMPR2 = 0x36DB6DB6;
    pAdc->SMPR1 = 0x36DB6DB6;

    /*
     * Finally, turn on the ADC and wait until it is ready (ADRDY = 1).
     * ADEN is kept being set until ADRDY is seen because, if ADEN is set less
     * than four ADC clock cycles after ADCAL has been cleared, it gets reset by
     * the calibration logic (workaround from ST HAL, ADC_Enable()).
     */
    uint32_t timeout = ADC_STARTUP_TIMEOUT;

    pAdc->ISR = ADC_ISR_ADRDY;      // Clear the ADRDY flag

    do
    {
        pAdc->CR |= ADC_CR_ADEN;

        if(timeout == 0)
        {
            ret = -EIO;
            break;
        }

        timeout -= 1;
    }
    while((pAdc->ISR & ADC_ISR_ADRDY) == 0);

    if(adc->mutex != NULL)
        pthread_mutex_init((pthread_mutex_t *) adc->mutex, NULL);

    return ret;
}

void adcStm32_terminate(const struct Adc *adc)
{
    // A conversion may be in progress, wait until it finishes
    if(adc->mutex != NULL)
        pthread_mutex_lock((pthread_mutex_t *) adc->mutex);

    ((ADC_TypeDef *) adc->priv)->CR = 0;

    switch((uint32_t) adc->priv)
    {
        case ADC1_BASE:
        case ADC2_BASE:
            if((ADC1->CR == 0) && (ADC2->CR == 0))
                RCC->AHB1ENR &= ~RCC_AHB1ENR_ADC12EN;
            __DSB();
            break;

        case ADC3_BASE:
            RCC->AHB4ENR &= ~RCC_AHB4ENR_ADC3EN;
            __DSB();
            break;

        default:
            break;
    }

    if(adc->mutex != NULL)
        pthread_mutex_destroy((pthread_mutex_t *) adc->mutex);
}

uint16_t adcStm32_sample(const struct Adc *adc, const uint32_t channel)
{
    // STM32H7 ADCs have twenty input channels, INP0 to INP19.
    if(channel > 19)
        return 0;

    ADC_TypeDef *pAdc = ((ADC_TypeDef *) adc->priv);

    pAdc->SQR1 = channel << ADC_SQR1_SQ1_Pos;
    pAdc->PCSEL = 1 << channel;
    pAdc->CR |= ADC_CR_ADSTART;

    while((pAdc->ISR & ADC_ISR_EOC) == 0) ;

    return pAdc->DR;
}
