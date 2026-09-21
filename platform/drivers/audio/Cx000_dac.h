/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef Cx000_DAC_H
#define Cx000_DAC_H

#include <stdbool.h>
#include <stdint.h>
#include "interfaces/audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Driver to use the HR_Cx000 internal DAC as audio output stream device.
 * Input data format is signed 16-bit and only a single instance of this driver
 * is allowed.
 */

extern const struct audioDriver Cx000_dac_audio_driver;

/**
 * Start generation of a "beep" tone from DAC output.
 *
 * @param freq: tone frequency in Hz.
 * @return zero on success, a negative error code otherwise.
 */
int Cx000dac_startBeep(const uint16_t freq);

/**
 * Stop an ongoing "beep" tone.
 */
void Cx000dac_stopBeep();

/**
 * Select how the "OpenMusic" bit of register 0x06 is set and cleared. By
 * default the register is written wholesale (0x22 / 0x20, the FM and M17
 * value). While the DMR modem driver owns the HR_C6000 the other bits of the
 * register select the vocoder interface and must survive a beep: the DMR
 * driver sets this flag when it configures the chip and clears it when it
 * terminates, the bit is then toggled with a read-modify-write.
 *
 * @param keep: true to preserve the other bits of register 0x06.
 */
void Cx000dac_setVocoderBits(const bool keep);

#ifdef __cplusplus
}   // extern "C"

class HR_C6000;

/**
 * Initialize the driver.
 */
void Cx000dac_init(HR_C6000 *device);

/**
 * Shutdown the driver.
 */
void Cx000dac_terminate();

/**
 * Driver task function, to be called at least once every 4ms to ensure a
 * proper operation.
 */
void Cx000dac_task();

#endif

#endif /* Cx000_DAC_H */
