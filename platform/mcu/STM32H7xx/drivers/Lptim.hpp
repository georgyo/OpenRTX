/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef LPTIM_H
#define LPTIM_H

#include <cstdint>

/**
 * Handler class for STM32H7 LPTIM peripheral.
 */
class Lptim
{
public:

    /**
     * Constructor.
     *
     * @param tim: base address of timer peripheral to manage.
     * @param baseFreq: timer base input frequency, in Hz.
     */
    constexpr Lptim(const uintptr_t tim, const uint32_t baseFreq) : tim(tim),
            baseFreq(baseFreq) {}

    ~Lptim() = default;

    /**
     * Configure timer prescaler and auto-reload registers for a given update
     * frequency.
     *
     * @param updFreq: desidered update frequency, in Hz.
     * @return effective timer update frequency, in Hz.
     */
    uint32_t setUpdateFrequency(const uint32_t updFreq) const
    {
        /*
         * Timer update frequency is given by:
         * Fupd = (Fbus / prescaler) / autoreload
         *
         * In LPTIM the prescaler can only assume values being powers of two up
         * to 128. To find the correct prescaler and autoreload values we start
         * by setting the the prescaler to 1 and proceed iteratively until the
         * autoreload value is less than the maximum allowed.
         */
        uint32_t div;
        uint32_t arr;
        uint32_t psc;
        for(div = 0; div < 8; div += 1)
        {
            psc = 1 << div;
            arr = (baseFreq / psc) / updFreq;
            if(arr < 0xFFFF)
                break;
        }

        /*
         * The configuration register can be written only when the timer is
         * disabled, while the autoreload register can be written only when the
         * timer is enabled (RM0433, LPTIM_CFGR and LPTIM_ARR descriptions).
         * The write to ARR completes asynchronously in the timer clock domain
         * and is signalled by the ARROK flag: wait for it, with a bounded
         * timeout, before returning so that the timer can be started safely.
         */
        LPTIM_TypeDef *lptim = reinterpret_cast< LPTIM_TypeDef * >(tim);

        lptim->CR   = 0;
        lptim->CFGR = div << LPTIM_CFGR_PRESC_Pos;
        lptim->CR   = LPTIM_CR_ENABLE;
        lptim->ICR  = LPTIM_ICR_ARROKCF;
        lptim->ARR  = arr - 1;

        uint32_t timeout = ARR_UPDATE_TIMEOUT;
        while(((lptim->ISR & LPTIM_ISR_ARROK) == 0) && (timeout > 0))
            timeout -= 1;

        return (baseFreq / psc) / arr;
    }

    /**
     * Clear and start the timer's counter.
     */
    inline void start() const
    {
        reinterpret_cast< LPTIM_TypeDef * >(tim)->CNT = 0;
        reinterpret_cast< LPTIM_TypeDef * >(tim)->CR |= LPTIM_CR_CNTSTRT;
    }

    /**
     * Stop the timer's counter.
     */
    inline void stop() const
    {
        // It seems that the only way to stop the LPTIM is to turn it off. Then
        // we have to turn it on again to allow writing the configuration registers.
        reinterpret_cast< LPTIM_TypeDef * >(tim)->CR = 0;
        reinterpret_cast< LPTIM_TypeDef * >(tim)->CR = LPTIM_CR_ENABLE;
    }

    /**
     * Get current value of timer's counter.
     *
     * @return value of timer's counter.
     */
    inline uint16_t value() const
    {
        /*
         * RM0433: when the LPTIM runs from an asynchronous clock a single
         * read of CNT may return an unreliable value, so read it twice and
         * keep the value of two matching reads. The kernel clock can be
         * faster than the bus reads (pll2_p at 168MHz on the CS7000P), in
         * which case two reads never match: bound the attempts and return
         * the last value read rather than spinning forever.
         */
        LPTIM_TypeDef *t = reinterpret_cast< LPTIM_TypeDef * >(tim);
        uint16_t a = 0;
        uint16_t b = 0;

        for(int i = 0; i < 3; i++)
        {
            a = t->CNT;
            b = t->CNT;
            if(a == b)
                break;
        }

        return b;
    }

private:

    /*
     * Upper bound, in loop iterations, for the wait on the ARROK flag. The
     * register update takes a few timer clock cycles, the bound only prevents
     * a hang if the timer clock is not running.
     */
    static constexpr uint32_t ARR_UPDATE_TIMEOUT = 10000;

    const uintptr_t tim;
    const uint32_t  baseFreq;
};

#endif /* LPTIM_H */
