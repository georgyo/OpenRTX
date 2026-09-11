/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HRC6000_H
#define HRC6000_H

#include <stddef.h>
#include <stdint.h>

/*
 * Minimal stand-in for the HR_C6000 baseband driver, shadowing the real
 * header when platform/drivers/audio/Cx000_dac.cpp is compiled for the host
 * unit tests. Register accesses are recorded into a global state defined by
 * the test program.
 */
#define FAKE_HR_C6000 1

struct FakeC6000State {
    volatile bool fifoNotEmpty;  ///< Value of bit 0 of register 0x88.
    const uint8_t *lastAudio;    ///< Pointer passed to the last sendAudio().
    volatile size_t audioChunks; ///< Number of sendAudio() calls.
    void (*onCfgWrite)(uint8_t reg,
                       uint8_t value); ///< Hook called on register write.
};

extern struct FakeC6000State fakeC6000;

class HR_C6000
{
public:
    void init()
    {
    }

    inline void writeCfgRegister(const uint8_t reg, const uint8_t value)
    {
        if (fakeC6000.onCfgWrite != NULL)
            fakeC6000.onCfgWrite(reg, value);
    }

    inline uint8_t readCfgRegister(const uint8_t reg)
    {
        if (reg == 0x88)
            return fakeC6000.fifoNotEmpty ? 0x01 : 0x00;

        return 0;
    }

    inline void sendAudio(const uint8_t *audio)
    {
        fakeC6000.lastAudio = audio;
        fakeC6000.audioChunks += 1;
    }
};

#endif /* HRC6000_H */
