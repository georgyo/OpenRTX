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

/*
 * Bits of ADC_CR which are "read, set": writing 1 issues a command and writing
 * 0 has no effect, so every read-modify-write of the register must write them
 * as zero not to re-issue a command (same mask as ADC_CR_BITS_PROPERTY_RS in
 * the ST LL driver).
 */
#define ADC_CR_RS_BITS (ADC_CR_ADCAL | ADC_CR_JADSTP | ADC_CR_ADSTP      \
                        | ADC_CR_JADSTART | ADC_CR_ADSTART | ADC_CR_ADDIS \
                        | ADC_CR_ADEN)

/**
 * \internal
 * Issue an ADC_CR command (ADSTP, JADSTP, ADDIS, ...) without re-issuing the
 * commands whose bits are currently set.
 *
 * @param pAdc: pointer to the ADC peripheral.
 * @param cmd: command bit(s) to set.
 */
static inline void adc_command(ADC_TypeDef *pAdc, const uint32_t cmd)
{
    pAdc->CR = (pAdc->CR & ~ADC_CR_RS_BITS) | cmd;
}

/**
 * \internal
 * Bring an ADC which is not in its reset state back to the disabled state.
 *
 * The code which ran before OpenRTX, i.e. the bootloader when the firmware is
 * started right after being flashed and without a power cycle, may leave the
 * ADC enabled, converting continuously or feeding a DMA stream. The start-up
 * sequence requires ADEN = 0: with the ADC enabled, the writes of the regulator
 * and boost settings, of the calibration command, of the sampling times and of
 * the channel selection are all ignored by the hardware and every sample then
 * returns whatever the leftover configuration keeps converting.
 *
 * @param pAdc: pointer to the ADC peripheral.
 * @return zero on success, -EIO if the ADC did not stop or did not switch off
 * in time.
 */
static int adc_disable(ADC_TypeDef *pAdc)
{
    int ret = 0;

    // Deep power-down, the reset state, implies disabled
    if((pAdc->CR & ADC_CR_DEEPPWD) != 0)
        return 0;

    if((pAdc->CR & ADC_CR_ADEN) == 0)
        return 0;

    // Stop the ongoing regular and injected conversions, if any
    if((pAdc->CR & ADC_CR_ADSTART) != 0)
        adc_command(pAdc, ADC_CR_ADSTP);

    if((pAdc->CR & ADC_CR_JADSTART) != 0)
        adc_command(pAdc, ADC_CR_JADSTP);

    if(adc_waitBits(&pAdc->CR, ADC_CR_ADSTART | ADC_CR_JADSTART, 0,
                    ADC_STARTUP_TIMEOUT) == false)
        ret = -EIO;

    // Switch the ADC off, ADEN is cleared by hardware once it is off
    adc_command(pAdc, ADC_CR_ADDIS);

    if(adc_waitBits(&pAdc->CR, ADC_CR_ADEN, 0, ADC_STARTUP_TIMEOUT) == false)
        ret = -EIO;

    return ret;
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

    // The ADC is not necessarily in its reset state, see adc_disable()
    if(adc_disable(pAdc) < 0)
        ret = -EIO;

    // Enable ADC voltage regulator, enable boost mode. Wait until LDO regulator
    // is ready.
    pAdc->CR = ADC_CR_ADVREGEN
             | ADC_CR_BOOST_1
             | ADC_CR_BOOST_0;

    if(adc_waitBits(&pAdc->ISR, ADC_ISR_LDORDY, ADC_ISR_LDORDY,
                    ADC_STARTUP_TIMEOUT) == false)
        ret = -EIO;

    /*
     * The configuration registers are not written by the sequence below and
     * keep whatever the previous owner of the ADC put there: restore their
     * reset values so that no interrupt enable, trigger, DMA request,
     * oversampling, offset or differential input leaks into the conversions.
     */
    pAdc->IER    = 0;
    pAdc->ISR    = ADC_ISR_ADRDY | ADC_ISR_EOSMP | ADC_ISR_EOC | ADC_ISR_EOS
                 | ADC_ISR_OVR | ADC_ISR_JEOC | ADC_ISR_JEOS | ADC_ISR_AWD1
                 | ADC_ISR_AWD2 | ADC_ISR_AWD3 | ADC_ISR_JQOVF;
    pAdc->CFGR   = ADC_CFGR_JQDIS;
    pAdc->CFGR2  = 0;
    pAdc->DIFSEL = 0;
    pAdc->OFR1   = 0;
    pAdc->OFR2   = 0;
    pAdc->OFR3   = 0;
    pAdc->OFR4   = 0;

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
