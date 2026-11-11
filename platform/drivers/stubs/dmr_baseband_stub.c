/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "interfaces/delays.h"
#include "interfaces/dmr_baseband.h"

/*
 * DMR modem driver for the targets without a DMR modem, or whose modem driver
 * does not exist yet: dmrbb_init() reports "not supported", so that the DMR
 * operating mode stays off with a marker on screen, and everything else is a
 * no-op. On the CS7000 radios this stub is linked until the HR_C6000 driver
 * of the next stage replaces it.
 */

int dmrbb_init(void)
{
    return -1;
}

void dmrbb_terminate(void)
{
}

void dmrbb_configure(const struct dmrbbConfig *cfg)
{
    (void)cfg;
}

void dmrbb_startRx(void)
{
}

void dmrbb_startTx(bool activeTiming)
{
    (void)activeTiming;
}

void dmrbb_idle(void)
{
}

void dmrbb_setNextSlot(uint8_t r41)
{
    (void)r41;
}

void dmrbb_setTxFrameType(uint8_t r50)
{
    (void)r50;
}

void dmrbb_writeTxLc(const uint8_t lc[12])
{
    (void)lc;
}

void dmrbb_writeVoice(const uint8_t ambe[27])
{
    (void)ambe;
}

uint32_t dmrbb_waitEvent(struct dmrbbSnapshot *s, unsigned timeoutMs)
{
    (void)s;

    /* Nothing will ever happen: pace the caller like a silent modem */
    sleepFor(0u, timeoutMs);
    return DMRBB_EV_TIMEOUT;
}

uint8_t dmrbb_readRegister(uint8_t page, uint8_t addr)
{
    (void)page;
    (void)addr;
    return 0;
}

void dmrbb_writeRegister(uint8_t page, uint8_t addr, uint8_t v)
{
    (void)page;
    (void)addr;
    (void)v;
}
