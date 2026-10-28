/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AK2365A_H
#define AK2365A_H

#include "peripherals/gpio.h"
#include "peripherals/spi.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * IF band-pass filter selection, see AK2365 datasheet BPF_BW[2:0].
 * The values are the 6dB bandwidth; the attenuation bandwidth is respectively
 * +-15kHz, +-12.5kHz, +-11kHz, +-9kHz and +-7.5kHz.
 */
enum AK2365A_BPF
{
    AK2365A_BPF_7p5,    ///< F0: +-7.5kHz, BPF_BW[2] = 1 (reg. 0x0B)
    AK2365A_BPF_6,      ///< F1: +-6kHz,   BPF_BW[1:0] = 00 (reg. 0x01)
    AK2365A_BPF_4p5,    ///< F2: +-4.5kHz, BPF_BW[1:0] = 01
    AK2365A_BPF_3,      ///< F3: +-3kHz,   BPF_BW[1:0] = 10
    AK2365A_BPF_2       ///< F4: +-2kHz,   BPF_BW[1:0] = 11
};

/**
 * Demodulated signal level selection, see AK2365 datasheet BAND (reg. 0x01).
 */
enum AK2365A_BAND
{
    AK2365A_BAND_NARROW,    ///< 100mVrms output at +-1.5kHz deviation
    AK2365A_BAND_WIDE       ///< 100mVrms output at +-3.0kHz deviation
};

/**
 * AK2365A device data.
 */
struct ak2365a
{
    const struct spiDevice *spi;   ///< SPI bus device driver
    const struct gpioPin   cs;     ///< Chip select gpio
    const struct gpioPin   res;    ///< Reset gpio
};


/**
 * Initialise the FM detector IC.
 *
 * @param dev: pointer to device data.
 */
void AK2365A_init(const struct ak2365a *dev);

/**
 * Terminate the driver and set the IC in reset state.
 *
 * @param dev: pointer to device data.
 */
void AK2365A_terminate(const struct ak2365a *dev);

/**
 * Set the bandwidth of the internal IF filter and the demodulated signal level,
 * then start the receiver (operating mode 7).
 *
 * @param dev: pointer to device data.
 * @param bw: IF filter bandwidth.
 * @param band: demodulated signal level.
 */
void AK2365A_setFilterBandwidth(const struct ak2365a *dev,
                                const enum AK2365A_BPF bw,
                                const enum AK2365A_BAND band);

#ifdef __cplusplus
}
#endif

#endif /* AK2365A_H */
