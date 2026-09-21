/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_CS7000_H
#define DMR_CS7000_H

/**
 * Bring-up counters of the CS7000 / CS7000-PLUS implementation of
 * interfaces/dmr_baseband.h, in addition to the ones the interface itself
 * declares on the driver pseudo page (DMRBB_PAGE_DRIVER): readable with
 * dmrbb_readRegister(DMRBB_PAGE_DRIVER, addr), no SPI traffic. The DMR
 * monitor screen has no row for them yet (a debugger or a follow-up row
 * reads them), see docs/dmr_bringup.md 5.1. Addresses 0x00 and 0x10-0x1F are
 * taken by dmr_baseband.h (voice path, line counters). The LCSS convention
 * of the voice bursts is not a driver knob: the call controller writes the
 * register 0x50 value of the convention selected on the monitor screen
 * (rtxStatus_t::dmr_lcssMode), the driver passes it through.
 *
 *  - DMRBB_DRV_SYS_LINE (read/write): EXTI line (PC0..PC3 pin number) the
 *    driver dispatches as SYS_INTER. DMRBB_SYS_LINE_PINMAP (1, PC1, the
 *    default: the pinmap constant DMR_SYS_INT from the reverse engineering
 *    of the F405 CS7000 firmware) or DMRBB_SYS_LINE_SCHEMATIC (3, PC3_C:
 *    the CS7000-M17 Plus schematic V01, pinmap constant DMR_SYS_INT_ALT).
 *    Any other value is ignored. All four lines PC0..PC3 stay attached and
 *    counted whatever the choice, only the line raising DMRBB_EV_SYS (and
 *    the register reads behind it) moves, so a tester can switch on the
 *    radio without reflashing once the counters of the monitor screen
 *    tell which line pulses. UNVERIFIED on hardware: which of the two is
 *    right, per board, see docs/dmr_bringup.md checks 1.2 and 3.1. The
 *    knob survives a reconfiguration and is forgotten at power off.
 *
 *  - DMRBB_DRV_RFTX_TICK(b) (read only): byte b (0 = least significant) of
 *    the 32-bit getTick() value latched by the last RF_TX_INTER interrupt.
 *  - DMRBB_DRV_RFTX_LEAD(b) (read only): byte b of the 16-bit distance, in
 *    milliseconds, between the last TIME_SLOT_INTER and the last
 *    RF_TX_INTER, for the check of the register 0x12 advance (2.4 ms with
 *    the assumed 0x98: the interrupt should land ~27.6 ms after the slot
 *    tick, i.e. 0x12[5:0] x 100 us before the next burst).
 */
#define DMRBB_DRV_RFTX_TICK(b) (0x20 + (b))
#define DMRBB_DRV_RFTX_LEAD(b) (0x24 + (b))

#define DMRBB_DRV_SYS_LINE 0x02
#define DMRBB_DRV_RFTX_TICK(b) (0x20 + (b))
#define DMRBB_DRV_RFTX_LEAD(b) (0x24 + (b))

/* Values of DMRBB_DRV_SYS_LINE, pin numbers on port C */
#define DMRBB_SYS_LINE_PINMAP 1
#define DMRBB_SYS_LINE_SCHEMATIC 3

#endif /* DMR_CS7000_H */
