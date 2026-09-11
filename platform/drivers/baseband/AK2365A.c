/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/delays.h"
#include "drivers/baseband/AK2365A.h"

static inline void writeReg(const struct ak2365a *dev, uint8_t reg, uint8_t value)
{
    uint8_t data[2];

    data[0] = (reg & 0x3f) << 1;
    data[1] = value;

    gpioPin_clear(&dev->cs);
    spi_send(dev->spi, &data, 2);
    gpioPin_set(&dev->cs);
}


void AK2365A_init(const struct ak2365a *dev)
{
    gpioPin_setMode(&dev->cs, OUTPUT);
    gpioPin_setMode(&dev->res, OUTPUT);

    gpioPin_clear(&dev->res);
    gpioPin_set(&dev->cs);

    delayUs(100);
    gpioPin_set(&dev->res);

    /*
     * Calibration procedure, see datasheet section 15: once in operating mode 6
     * the circuits needed for calibration are ready after 500us, calibration
     * takes 1.3ms and the discriminator needs a further 1.5ms to settle after
     * calibration is completed.
     */
    writeReg(dev, 0x04, 0xAA);   // Software reset
    writeReg(dev, 0x01, 0xC1);   // Operating mode 6, LO freq 50.4MHz
    delayUs(500);
    writeReg(dev, 0x02, 0x33);   // Enable calibration
    writeReg(dev, 0x03, 0x00);   // IF buffer gain 5dB
    writeReg(dev, 0x0B, 0x01);   // AGC auto, AGC1 gain 21dB
    writeReg(dev, 0x0C, 0x80);   // AGC2 gain 12dB
    delayMs(3);
}

void AK2365A_terminate(const struct ak2365a *dev)
{
    gpioPin_clear(&dev->res);
}

void AK2365A_setFilterBandwidth(const struct ak2365a *dev,
                                const enum AK2365A_BPF bw,
                                const enum AK2365A_BAND band)
{
    uint8_t reg01 = 0xE1;        // Operating mode 7, LO freq. 50.4MHz
    uint8_t reg0B = 0x01;        // AGC auto, AGC1 gain 21dB

    if(band == AK2365A_BAND_WIDE)
        reg01 |= 0x10;           // BAND = 1

    switch(bw)
    {
        case AK2365A_BPF_7p5:
            reg0B |= 0x80;       // BPF_BW[2] = 1, F0 filter
            break;

        case AK2365A_BPF_6:
            break;               // BPF_BW[1:0] = 00, F1 filter

        case AK2365A_BPF_4p5:
            reg01 |= 0x04;       // BPF_BW[1:0] = 01, F2 filter
            break;

        case AK2365A_BPF_3:
            reg01 |= 0x08;       // BPF_BW[1:0] = 10, F3 filter
            break;

        case AK2365A_BPF_2:
            reg01 |= 0x0C;       // BPF_BW[1:0] = 11, F4 filter
            break;
    }

    writeReg(dev, 0x01, reg01);
    delayMs(1);
    writeReg(dev, 0x01, reg01);
    writeReg(dev, 0x02, 0x1E);   // AGC time = 3, AGC step 2dB
    writeReg(dev, 0x03, 0x00);   // IF buffer gain 5dB
    writeReg(dev, 0x0B, reg0B);
    writeReg(dev, 0x0C, 0x80);   // AGC2 gain 12dB
}
