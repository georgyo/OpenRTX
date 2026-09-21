/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_CS7000_H
#define DMR_CS7000_H

/**
 * Bring-up knobs and counters of the CS7000 / CS7000-PLUS implementation of
 * interfaces/dmr_baseband.h, in addition to the ones the interface itself
 * declares on the driver pseudo page (DMRBB_PAGE_DRIVER): all reachable with
 * dmrbb_readRegister() / dmrbb_writeRegister() from the DMR monitor screen,
 * none of them generates SPI traffic. Addresses 0x00 and 0x10-0x1F are
 * taken by dmr_baseband.h (voice path, line counters).
 *
 *  - DMRBB_DRV_LCSS_MODE (read/write): convention for the LCSS bits of
 *    register 0x50 in voice bursts. DMRBB_LCSS_PASSTHROUGH (default): the
 *    value given to dmrbb_setTxFrameType() is written as it is, i.e. the
 *    OpenGD77 convention 0x08 | (seq << 4) with LCSS = 00, under which the
 *    chip fills the embedded LC fragments itself from the TX RAM.
 *    DMRBB_LCSS_MANUAL: a voice burst value is translated into the manual
 *    §5.4.5 sequence 0x08, 0x19, 0x2B, 0x3B, 0x4A, 0x58 (LCSS 00, 01, 11,
 *    11, 10, 00 for A..F), the header/terminator/CSBK values pass through.
 *    UNVERIFIED on hardware: which of the two the CS7000 needs, to be told
 *    by a hotspot with EmbeddedLCOnly = 1 (late entry), see
 *    docs/dmr_bringup.md.
 *
 *  - DMRBB_DRV_RFTX_TICK(b) (read only): byte b (0 = least significant) of
 *    the 32-bit getTick() value latched by the last RF_TX_INTER interrupt.
 *  - DMRBB_DRV_RFTX_LEAD(b) (read only): byte b of the 16-bit distance, in
 *    milliseconds, between the last TIME_SLOT_INTER and the last
 *    RF_TX_INTER, for the check of the register 0x12 advance (2.4 ms with
 *    the assumed 0x98: the interrupt should land ~27.6 ms after the slot
 *    tick, i.e. 0x12[5:0] x 100 us before the next burst).
 */
#define DMRBB_DRV_LCSS_MODE 0x01
#define DMRBB_DRV_RFTX_TICK(b) (0x20 + (b))
#define DMRBB_DRV_RFTX_LEAD(b) (0x24 + (b))

/* Values of DMRBB_DRV_LCSS_MODE */
#define DMRBB_LCSS_PASSTHROUGH 0
#define DMRBB_LCSS_MANUAL 1

#endif /* DMR_CS7000_H */
