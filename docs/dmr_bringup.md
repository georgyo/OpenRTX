# DMR bring-up checklist

Nothing of the DMR series has been run on a radio. This document lists, in
the order in which they should be checked, every assumption that comes from
the HR_C6000 manual, the CS7000-M17 Plus schematic or the observation of other
firmwares but has not been confirmed on a CS7000 or CS7000-M17 Plus. Each
entry names the code that carries the matching `UNVERIFIED on hardware`
comment, what to observe, and what to change if the observation disagrees.

The list grows with the stages of the series (see [dmr_mode.md](dmr_mode.md));
an entry is removed, and the comment in the code with it, once somebody has
checked it on a radio and recorded the result here.

## 1. GPIO external interrupts (`platform/drivers/GPIO/gpio_irq_stm32.cpp`)

The HR_C6000 signals the slot timing and the layer-2 events with ~300 ns
active-low pulses on TIME_SLOT_INTER, SYS_INTER and RF_TX_INTER (manual: "the
interrupt low pulse is valid, the pulse width is 3 system working clocks" of
9.8304 MHz). They can only be caught with a falling-edge EXTI interrupt;
polling the pins is not viable. `gpioIrq_attach()` programs the STM32 EXTI
for one callback per line. The handlers of the modem driver
(`platform/drivers/baseband/dmr_CS7000.cpp`, `lineIsr()`) only latch: an
event bit in a mailbox word, a counter per line and the tick of the slot
interrupt. They do not wake anybody: Miosix in this tree has no timed
`IRQwait()` and `Thread::sleep()` is not released by `IRQwakeup()`, so
`dmrbb_waitEvent()` in the rtx thread polls the mailbox every millisecond
and does all the register reads and writes there; nothing touches the SPI
bus in interrupt context. On a logic analyser this shows as up to about
2 ms (the 1 ms polling period plus the sleep granularity of the kernel
tick) between the TIME_SLOT_INTER pulse and the 0x52 / 0x42 reads followed
by the 0x41 write of the next slot, well inside the t2 = 27 ms the chip
gives to program it; a measured latency of that order is the design, not a
fault.

### 1.1 PC2_C / PC3_C reach the EXTI logic with SYSCFG_PMCR at reset

- Where: header comment of `gpio_irq_stm32.cpp`.
- Assumption: on the STM32H743 of the CS7000-M17 Plus, the schematic (V01)
  routes RF_TX_INTER and SYS_INTER to the pads PC2_C and PC3_C. The datasheet
  (DS12110, pin table note 6) says the `_C` pads reach the PC2/PC3 digital
  logic through analogue switches controlled by `SYSCFG_PMCR` bits `PC2SO` and
  `PC3SO`, which are closed at reset. Nothing in the tree opens them, and the
  driver deliberately never writes `SYSCFG_PMCR`, so a falling-edge EXTI on
  PC2 or PC3 should see the pulses.
- Check: in DMR receive near an active repeater or hotspot, the per-line
  counters of the DMR monitor screen (section 2, rows `PC0`..`PC3`) must
  show PC0 ticking at about 33 Hz (TIME_SLOT_INTER) and PC3 or PC1 pulsing
  roughly every 60 ms (SYS_INTER), PC2 staying idle in receive.
- If wrong: if PC2/PC3 never fire while PC0 does, the switches are open on
  this board (an option byte or the bootloader may have set `PC2SO`/`PC3SO`).
  Clear them once at init in `platform.c` of the CS7000-PLUS target, with a
  comment, before attaching the lines. Do not clear them blindly beforehand:
  with the switches closed the ADC3 inputs on PC2/PC3 and the digital pads
  share the pin, and `radio_CS7000.cpp` notes that ADC3 inputs on this package
  are PC0-PC3.

### 1.2 Which pin carries SYS_INTER (PC1 or PC3)

- Where: `platform/targets/CS7000/pinmap.h` and
  `platform/targets/CS7000-PLUS/pinmap.h` (`DMR_SYS_INT`), attached by the
  stage-3 modem driver (part B).
- Assumption: the pinmaps say TS = PC0, SYS = PC1, TX = PC2 (from the reverse
  engineering of the F405 CS7000 firmware); the CS7000-M17 Plus schematic says
  TS = PC0, RF_TX = PC2_C, SYS = PC3_C and puts the 2T/5T analogue tone
  detector on PC1. The two boards may differ.
- Check: the counters of check 1.1 tell which line pulses every 60 ms, see
  3.1.
- If wrong: fix `DMR_SYS_INT` per target. The EXTI shim attaches PC0..PC3 all
  four during bring-up, so no rebuild is needed to see the answer.

### 1.3 NVIC priority 14 for the EXTI lines

- Where: `EXTI_IRQ_PRIORITY` in `gpio_irq_stm32.cpp`.
- Facts, from the kernel in tree: Miosix calls `NVIC_SetPriorityGrouping(7)`
  (`lib/miosix-kernel/miosix/arch/cortexM7/portability.cpp`, and the
  Cortex-M4F port for the CS7000), which leaves no pre-emption bits: no
  interrupt ever nests inside another, whatever its number, as
  `platform/targets/CS7000-PLUS/hwconfig.h` already notes. In that flat 0-15
  scheme SVC and SysTick (the kernel tick) are at 3, MemManage at 2, the
  audio DMA streams at 10 (`stm32_dac.cpp`, `stm32_adc.cpp`), USB and GPS
  at 14, the serial ports at 15. The number
  therefore only decides which of several interrupts pending at the same
  moment runs first; 14 puts the EXTI lines with the other peripherals, and
  no value of it can make a callback pre-empt an SPI transfer or be
  pre-empted by one.
- Assumption: the ~300 ns pulses are never lost, because the EXTI pending
  bit latches the edge in hardware and the handler only records it. What a
  long-running handler ahead of the EXTI one (a display or audio DMA
  completion) can do is delay the callback by its own length, never drop the
  edge; the slot budget (t2 = 27 ms, section 1) absorbs that.
- Check: the `TS` counter of the monitor screen must keep counting at 33 per
  second while the display refreshes and, from stage 4, while the OpenMusic
  DAC stream runs; a "TS lost" row is to be added once the modem driver
  tracks the 30 ms period.
- If wrong: `TS` skipping while some other interrupt is busy means that
  handler runs for longer than a slot: shorten it (or move its work to a
  thread). Changing `EXTI_IRQ_PRIORITY` cannot help and is not the remedy.

### 1.4 The handler symbols

- Where: bottom of `gpio_irq_stm32.cpp`.
- Constraint, not a hardware unknown: the file defines
  `EXTI0..EXTI4_IRQHandler`, `EXTI9_5_IRQHandler` and `EXTI15_10_IRQHandler`
  with C++ linkage, as the vector tables in `platform/mcu/STM32{F4,H7}xx/boot/
  startup.cpp` declare them. `chSelector_UV3x0.c` and `chSelector_MD9600.c`
  define `EXTI15_10_IRQHandler` for the MD-UV3x0, DM-1701 and MD-9600, so the
  shim is listed only in the CS7000 and CS7000-PLUS source lists of
  `meson.build`. A target that needs both must split the handler.
- Check: the CS7000 and CS7000-PLUS firmwares link (done in CI); the vector
  table entry for EXTI0 must point into `gpio_irq_stm32.cpp` (`arm-none-eabi-nm
  openrtx_cs7000p | grep EXTI0`).

## 2. The DMR monitor screen (`Settings > DMR > DMR monitor`)

The checks below are run from the DMR monitor screen, the bring-up tool of
stage 3. It shows what the DMR opMode handler publishes into
`rtxStatus_t::dmr_mon` on every pass (`OpMode_DMR::monitor()`), reads a few
registers back, and can write the registers whose value is in doubt without a
rebuild. Nothing on it is needed for normal operation.

Pages are flipped with LEFT and RIGHT (UP, DOWN and ENTER flip them too,
except on the action page, where UP and DOWN select the action and ENTER runs
it); ESC returns to `Settings > DMR`. The rows come in five sections of 12,
13, 7, 11 and 7 rows (`enum dmrMonitorRows` in `ui_default.h`), each value
section starting on a new page and spanning as many pages as the screen
height needs, the action section always one page whose rows scroll behind
the selection: six rows per page on the 160x128 displays, hence
2 + 3 + 2 + 2 + 1 = 10 pages, five rows per page on 128x64, hence
3 + 3 + 2 + 3 + 1 = 12 pages (`_ui_dmrMonitorPages()` in `ui_menu.c`; the
title reads `DMR monitor n/N`):

| Section | Rows | Source |
|---|---|---|
| 1. Counters | `TS`, `SYS`, `RFTX` interrupt counts; `PC0`..`PC3` EXTI pulses per line; `Lock` (0 none, 1 slots ticking, 2 timecode locked); `Timecode`; last `0x82`, `0x84`, `0x86` | `dmrbbSnapshot` counters and `lineCount[]`, refreshed on every pass of the opMode handler including the 30 ms timeouts (a line pulsing without raising an event, PC3 while the pinmap expects SYS on PC1, is counted all the same); flags of the last SYS event |
| 2. Registers | last `0x51`, `0x52`, `0x5F`, `0x42`; `CC` (0x52[7:4]); `TC` (0x52 bit2); `Sync` (0x5F[1:0]: MS, BS, TDMA1, TDMA2); `RSSI` (0x43:0x44); `FSK err` (0x0F); `rd 0x10`, `rd 0x06`, `rd 0x12`, `rd 0x14-16` read back over U_SPI at every redraw | registers read in the TS/SYS passes; `dmrbb_readRegister()` |
| 3. Link control | `LC 0-5`, `LC 6-11` (the 12 LC RAM octets in hex), `Src`, `Dst`, `FLCO`, `FID` decoded from them, `CRC` (`ok`/`bad` from 0x51 bit2, `*` appended when the 9 octets do not parse as a standard Full LC) | RX RAM of the last header, terminator or late entry |
| 4. Audio (stages 4 and 5) | `Vocoder` (backend name), `Aud RX` (payloads pushed to playback), `Aud vary` (of which differing from the previous one), `Aud out` (480-sample blocks written to the speaker), `Aud TX` (payloads encoded from the microphone), `Underrun` (silent blocks between two payloads of a call), `Overrun` (payloads dropped from a full queue); since stage 5 `TX bursts` (own voice bursts sent), `TX silent` (of which carrying the AMBE silence pattern: all of them with the null backend), `TX under` (silence sent while the encoder thread ran, the microphone stream fell behind), `RF keyed` (times the RF stage was keyed for a transmission, one per accepted PTT press) | `rtxStatus_t::dmr_audio`, `dmrAudio_getStats()` of `core/dmr_audio.c`; `dmrMonitor_t` for the TX rows |
| 5. Actions | `Resync`, `Mode 0x10`, `Reg 0x06`, `BER test`, `Two-point`, `LCSS mode`, `RF advance`, the value column showing the register and the value the next ENTER writes (`LCSS mode`: the convention it selects) | `dmrbb_writeRegister()` on the CONFIG page (U_SPI opmode 4); `LCSS mode` writes `state.dmr_lcssMode` instead |

The actions, all `UNVERIFIED on hardware` (`openrtx/src/ui/default/ui.c`,
`_ui_dmr_monitorAction()`), are only run while the RTX stage is in DMR on a
working modem: in FM or M17 (`Settings > DMR` is reachable from any mode) or
on a radio without a DMR modem the value column of the action page reads
`DMR off` and ENTER writes nothing, since the chip is then in FM mode and
`0x10 = 0x6A` or the two-point tone would silently take it out of it.

| Action | Writes | Meaning |
|---|---|---|
| Resync | 0x81 = 0xFE, 0x41 = 0x20 | "SyncFail": drop the received timing, search for sync again (manual 5.4.5). The Layer-2 interrupt mask of `dmrMode()` is written first, so that a Resync also ends the extra unmask of a `BER test` |
| Mode 0x10 | 0x10 = 0x6A, then 0x6E, alternately | Mode word: 0x6E is the OpenGD77 value (DMR, Tier II, timeslot mode, Layer 2, repeater, aligned), 0x6A the manual's example. The chosen word is also pushed to the RTX stage (`rtxStatus_t::dmr_modeReg`), so the opMode handler reprograms the modem with it and keeps it through the watchdog reconfigurations; it is forgotten at power off |
| Reg 0x06 | 0x06 = 0x21, 0x01, 0x24, 0x20, 0x00 in turn | Vocoder port variants: V_SPI under MCU control (OEM value), TX/RX RAM under MCU control, HR_V3000 hook-up value, V_SPI without MCU control, all off |
| BER test | 0x83 = 0xFF, 0x81 = 0xFF, 0x10 = 0x02, 0x40 = 0x40, 0x41 = 0x41 | PHY-layer BER test of manual 5.4.6: 36 raw bytes per frame in RX RAM 0x30, 0x82 bit0 per frame. Bit0 is the "PHY-only" interrupt `dmrMode()` masks with 0x81 = 0xFE, so the stale flags are cleared and the mask opened first; `Resync` or the next modem reconfiguration put 0xFE back |
| Two-point | 0x12 bit6 toggled (read-modify-write) | 40 Hz square wave on MOD1/MOD2 to balance the two-point modulation |
| LCSS mode | no register write: `state.dmr_lcssMode` toggled, pushed to the RTX stage as `rtxStatus_t::dmr_lcssMode` | Convention of the LCSS bits of register 0x50 in voice bursts, applied by the call controller to the next transmission without reprogramming the modem: `50 chip` = 0x08 \| seq << 4 with LCSS 00, the chip filling the embedded LC fragments (OpenGD77, default); `50 manual` = the manual's 5.4.5 sequence 0x08, 0x19, 0x2B, 0x3B, 0x4A, 0x58. The value column names the convention the next ENTER selects; forgotten at power off |
| RF advance | 0x12[5:0] and bit7 = 0x98 or 0x15, alternately (read-modify-write, bit6 preserved) | RF_TX_INTER advance: the OEM init word 0x98 (2.4 ms, RF ramp smoothing) or OpenGD77's 0x15 (2.1 ms, no smoothing); the OEM word when the register holds neither. The `Two-point` bit is left as it is, so the two can be combined |

Two things to keep in mind:

- The modem is reprogrammed from scratch (`dmrbb_configure()`, table 4.1 of
  the design note) whenever the colour code, timeslot, ID, access policy or
  mode word changes, and after 2 s without timeslot interrupts. `Reg 0x06`,
  `BER test`, `Two-point` and `RF advance` are therefore overwritten within
  2 s while no signal is being received; read `rd 0x06` / `rd 0x12` right
  after the action, or run the check with a signal present. `Mode 0x10` and
  `LCSS mode` survive, see above.
- PTT is disabled on every menu screen. For the checks that need
  transmission, run the action, press ESC back to the main screen, key the
  radio (a DMR transmission with silence frames), release, and come back to
  the monitor: the counters and readbacks are kept.

## 3. Hardware checks, in order

Each check names the unknown, what to do, what is expected and what to change
where if it fails. Record the outcome in the table of section 6.

### 3.1 Interrupt lines: which pin carries SYS_INTER (PC1 or PC3) and whether RF_TX_INTER reaches PC2

- Where: `DMR_SYS_INT` / `DMR_TX_INT` in `platform/targets/CS7000/pinmap.h`
  and `platform/targets/CS7000-PLUS/pinmap.h`; the lines the stage-3 modem
  driver attaches (`platform/drivers/baseband/dmr_CS7000.cpp`); checks 1.1
  and 1.2 above.
- Do: select DMR (MONI + 5) near an active repeater or hotspot on the
  configured colour code, open the monitor, section 1, and watch `TS`, `SYS`
  and `PC0`..`PC3` for a few seconds while the repeater carries a call. Then
  go back to the main screen, key the radio for two seconds, and come back.
- Expect: `TS` and `PC0` counting at about 33 per second once sync is found
  (`Lock` 1, then 2); `SYS` and exactly one of `PC1` / `PC3` counting about
  16 per second during the call (one SYS event per 60 ms burst on the
  monitored slot, twice that with both slots busy); `PC2` at 0 while
  receiving, then `RFTX` and `PC2` both counting about 16 per second after
  the transmission. `0x82` shows `08` (control frame) or `10` (late entry)
  during the call.
- If wrong: `PC3` counts and `PC1` stays at 0 on the Plus: set
  `DMR_SYS_INT` to PC3 in the CS7000-PLUS pinmap (the schematic). `PC1`
  counts: the pinmap is right for that board. Neither counts while `PC0`
  does: SYS interrupts are masked, see 3.5. `PC0` never counts: no sync, see
  3.3 and 3.7. `PC2` never counts during TX while `RFTX` is expected: the
  `_C` pad switch, check 1.1; if `PC2` counts but `RFTX` does not, the
  driver attaches the wrong line for RF_TX.

### 3.2 Mode word 0x10: 0x6E or 0x6A

- Where: `DMRBB_MODE_REG_DEFAULT` in `openrtx/include/interfaces/dmr_baseband.h`
  and the 0x10 row of `dmrMode()` in the stage-3 driver.
- Do: with the repeater active and `TS` stuck at 0 after several seconds,
  run `Mode 0x10` once (writes 0x6A, `rd 0x10` must read `6A`) and watch
  `TS` again; run it again to go back to 0x6E.
- Expect: `TS` counting with one of the two words, `rd 0x10` reading what
  was written (which also answers 3.7 for this register).
- If wrong: only 0x6A works: change `DMRBB_MODE_REG_DEFAULT` to 0x6A and
  the driver table. Neither works: 0x40 / 0x41 sequence (3.4) or the RX
  path (3.8).

### 3.3 RX next-slot value 0x41: 0x50 or 0x40

- Where: `DMR::R41_RX` in `openrtx/include/protocols/DMR/ModemPort.hpp`,
  `dmrbb_startRx()` in the driver.
- Do: during a call, section 2: watch `0x51` and `SYS`.
- Expect: `0x51` cycling through `10` (voice LC header, class 00), then
  `11`, `21`, ... `61` (voice bursts A..F, class 01), then `20`
  (terminator); `SYS` counting per burst. With 0x50 the voice payload is
  routed to the vocoder port as well (bit4 "begin_v_layer2"), which stage 4
  needs: whether V_SPI then returns non-zero payloads is a stage-4 check.
- If wrong: headers appear (`0x51` = `10`) but no voice-class events
  follow: try `R41_RX` = 0x40 (blind reception of the manual's minimal
  sequence) in `ModemPort.hpp` and the driver, rebuild; `Resync` after
  changing nothing else re-runs the sync search on the current values.

### 3.4 Interrupt masks 0x81 / 0x85 / 0x87 polarity

- Where: the 0x83 / 0x81 / 0x85 / 0x87 rows of `dmrMode()` in the driver
  (0xFF / 0xFE / 0xFF / 0xFF assumed; FM writes 0x81 = 0x00 "all masked",
  the MD-3x0 OEM capture writes 0x85 / 0x87 = 0x00 during setup).
- Do: sections 1 and 2 during a call and after a transmission.
- Expect: `SYS` counting, `0x82` non-zero during the call; after keying,
  `0x82` shows bit6 / bit5 (`40` / `20`) with `0x84` = `80` (voice TX start)
  and `0x86` non-zero.
- If wrong: `TS` counts but `SYS` never does: 0x81 polarity is inverted,
  write 0x01 instead of 0xFE. `SYS` counts on 0x82 bit6/bit5 but `0x84` /
  `0x86` stay 0: invert 0x85 / 0x87 (0x00).

### 3.5 Status registers 0x51 / 0x52 / 0x5F bit meanings

- Where: `R51_*`, `R52_*`, `R5F_*` in `ModemPort.hpp`, their use in
  `openrtx/src/protocols/DMR/CallController.cpp`.
- Do: section 2 during a call on a repeater with a known colour code, then
  on a hotspot in direct mode.
- Expect: `CC` = the repeater's colour code (0x52[7:4]); `TC` alternating
  0/1 with the timeslot; `Sync` = `BS` on the repeater, `MS` on a hotspot in
  DMO (`TDMA1` / `TDMA2` only from a radio transmitting in DMO); `0x51`
  sequencing as in 3.3 with `CRC` = `ok` on the header and the terminator;
  `Lock` = 2 and `Timecode` matching the slot the call is on; section 3
  shows a `Src` / `Dst` that matches the hotspot's log.
- If wrong: the colour code appears in the low nibble, the sequence never
  reaches `61`, or `CRC` reads `bad` on every header: fix the constants in
  `ModemPort.hpp` (bit positions of CC, TC, CRC, sync class) and the
  parsing in `CallController.cpp`. `Src` / `Dst` swapped or shifted: the
  12-octet LC RAM layout assumed by `FullLC::fromChipLc()`
  (`openrtx/src/protocols/DMR/LinkControl.cpp`) is wrong, compare `LC 0-5` /
  `LC 6-11` with the hotspot's LC.

### 3.6 Own ID registers 0x14-0x16 byte order

- Where: `dmrbb_configure()` in the driver (G4EML: L/M/H; the MD-3x0 OEM
  capture looks H/M/L).
- Do: set `Settings > DMR > DMR ID` to 2345678 (0x23CACE), select DMR, open
  the monitor, section 2, `rd 0x14-16`.
- Expect: `CE CA 23` if the driver writes L/M/H as assumed and the chip
  keeps the bytes; `23 CA CE` means the driver was changed to H/M/L; any
  other reading means the registers are write-only or masked.
- If wrong: harmless for calls (address matching is done on the MCU), fix
  the order in the driver for the record. If the readback is garbage while
  `rd 0x10` reads correctly, the registers are write-only: note it and move
  on.

### 3.7 U_SPI clock: 12.5 MHz (current SPI2 setting) versus the documented 4 MHz maximum

- Where: SPI2 setup in `platform/targets/CS7000-PLUS/hwconfig.c` and
  `platform/targets/CS7000/hwconfig.c` (`c6000_spi`).
- Do: section 2 right after selecting DMR: `rd 0x10` must read the mode
  word (`6E`), `rd 0x06` = `21`, `rd 0x12` = the driver's value (0x98
  assumed). Run `Mode 0x10` and `Reg 0x06` and read them back.
- Expect: every readback equal to what was written.
- If wrong: any mismatch or unstable readback: lower the SPI2 clock to
  4 MHz in `hwconfig.c` (both targets) and repeat; the OpenMusic DAC refill
  (`platform/drivers/audio/Cx000_dac.cpp`, 64 bytes every 4 ms) still fits
  at 4 MHz but the margin should be re-checked with a beep playing in FM.

### 3.8 AK2365A IF path into the C6000 ADC

- Where: filter and AGC values in `platform/drivers/baseband/AK2365A.c` and
  `radio_enableRx()` in `platform/drivers/baseband/radio_CS7000.cpp`
  (BPF_6 + NARROW, AGC 0x02 = 0x1E, 0x0B, 0x0C as in FM).
- Do: with a DMR signal generator (or a hotspot at high level) and no sync
  after 3.2: `BER test` (which also unmasks 0x82 bit0 with 0x81 = 0xFF, the
  PHY-only interrupt `dmrMode()` leaves masked), then watch `SYS` and
  `0x82`. Without timeslot interrupts the opMode handler reprograms the
  modem after 2 s, undoing the test: read the counters right away, or run
  the action again.
- Expect: in BER mode the chip raises 0x82 bit0 once per received frame:
  `SYS` counting and `0x82` = `01` means the IF path and the 4FSK demodulator
  work and the problem is in the Layer-2 configuration (3.2-3.5); `RSSI` and
  `FSK err` move with the generator level.
- If wrong: no PHY frames: sweep the AK2365A AGC registers (0x02 step/time,
  0x0B, 0x0C gains) and confirm BPF_6 / NARROW are selected for DMR in
  `radio_enableRx()`; check the 450 kHz IF word (0x07-0x09 = 0x0B, 0xB8,
  0x00) in the driver. `SYS` counts but `0x82` never shows bit0: the 0x81
  polarity is inverted (3.4). Leave BER mode with `Resync` (mask back to
  0xFE) or by switching to FM and back.

### 3.9 Two-point modulation balance and 4FSK deviation

- Where: 0x45 / 0x46 from `txDigitalPathQ` / `txDigitalPathI` in
  `radio_updateConfiguration()` (`radio_CS7000.cpp`), 0x12 bit7 in the
  driver.
- Do: `Two-point` (0x12 bit6 set, `rd 0x12` reads `D8` with the assumed
  0x98), ESC to the main screen, key the radio on a dummy load with a
  deviation meter or spectrum analyser, release, run `Two-point` again to
  clear the bit. Then key the radio normally and measure the 4FSK deviation.
- Expect: a 40 Hz square wave with flat top on both edges (MOD1 and MOD2
  balanced); symbol deviation +/- 1.944 kHz (+/- 648 Hz inner symbols) and
  the DMR emission mask with the digital calibration pair.
- If wrong: tilted square wave: adjust the 0x45 / 0x46 ratio; deviation off:
  the `txAnalogPath*` pair may be the right one, or the calibration
  interpolation, in `radio_updateConfiguration()`.

### 3.10 V_SPI voice payload

Moved to section 4.3: the monitor screen's `Aud RX` and `Aud vary` rows of
stage 4 show whether the payloads arrive and whether they vary per burst.

### 3.11 Round trip and side effects

- Do: on a freshly booted radio that has not entered DMR, press a key in
  FM (beep) and receive M17 audio; then FM -> DMR -> FM and FM -> DMR ->
  M17 -> FM with the mode key; in DMR press a key (beep) and check the
  counters keep running; late entry: start listening in the middle of a
  call (Pi-Star with `EmbeddedLCOnly=1`).
- Expect: key beeps and M17 RX audio exactly as before the DMR series on the
  radio that never entered DMR: in FM and M17 the OpenMusic driver
  (`Cx000_dac.cpp`) writes register 0x06 wholesale, 0x22 / 0x20, and the
  0x21 that `init()` leaves there on the CS7000 does not matter; only while
  the DMR driver owns the chip (`Cx000dac_setVocoderBits(true)` from
  `dmrbb_configure()`, cleared by `dmrbb_terminate()`) is the bit toggled
  with a read-modify-write, 0x23 / 0x21, `UNVERIFIED on hardware` for the
  playback. FM RX audio, CTCSS and M17 working after the visit (registers
  0x26 / 0x36 / 0x25 restored by `fmMode()`, 0x06 back to the wholesale
  writes); `TS` / `SYS` unaffected by the beep in DMR; the caller shown
  within 360 ms of the late entry through 0x82 bit4 (`0x82` = `10`).
- If wrong: FM audio dead after DMR: compare `fmMode()` with the DMR table
  for a register it does not restore, and check `rd 0x06` on the monitor
  screen reads `20` or `22` after a beep in FM (the DMR driver was not
  terminated before `fmMode()`, `radio_setOpmode()`). Beeps stop the
  interrupts in DMR: `rd 0x06` reads `22` / `20`, the flag was not set. No
  late entry: the embedded LC assembly is not enabled in the driver (0x82
  bit4 masked) or 3.4.

## 4. Stage 4: the audio path (`openrtx/src/core/dmr_audio.c`, `Cx000_dac.cpp`)

Stage 4 plays received voice payloads through the vocoder backend into the
Cx000 "OpenMusic" DAC path. On a radio the backend is the null one: a
received call produces silence, or the 1 kHz test tone of
`Settings > DMR > Test tone`, for the 60 ms of every voice burst. The
monitor screen's "Audio" section (`Vocoder`, `Aud RX`, `Aud vary`,
`Aud out`, `Aud TX`, `Underrun`, `Overrun`) shows what the audio layer did.
Nothing below has been run on a radio.

### 4.1 Test tone on a received call: OpenMusic during DMR RX, AF_MUTE / amplifier sequencing, 0x26 / 0x25 / 0x36 gating

- Where: `Cx000_dac.cpp` (`setOpenMusic()`, register 0x06 bit1
  read-modify-write over the DMR value 0x21; the 0x88 bit0 FIFO poll),
  `audio_CS7000.cpp` (`audio_connect(SOURCE_MCU, SINK_SPK)`: AUDIO_AMP_EN,
  AF_MUTE, speaker mutes as for M17), `dmr_audio.c` (`dmrAudio_startRx()`
  on the granted path).
- Do: set `Settings > DMR > Test tone` on, receive a call from a hotspot
  (or another radio) on the configured colour code, timeslot and talkgroup.
  Open the monitor screen during the call (PTT is disabled there, reception
  is not).
- Expect: the tone comes out of the speaker for the whole call, continuous
  from burst to burst (no 60 ms gaps, no clicks), the volume knob acts on
  it, it stops at the terminator and the amplifier goes quiet through the
  hang time; `Aud RX` counts one per burst (about 16.7 per second),
  `Aud out` runs a little ahead of it (the silence before the first burst),
  `Underrun` and `Overrun` stay at 0 or a handful, `TS` / `SYS` keep
  running at the same rate as without audio (the 4 ms OpenMusic refill and
  the slot passes share SPI2 without stealing interrupts).
- If wrong:
  - silence, `Aud RX` counting, `Aud out` counting: the DAC path is not
    audible in DMR mode. Read `rd 0x06` on the monitor screen: it must be
    0x23 (0x21 | OpenMusic) while the call plays; if it is 0x22 the
    vocoder bits were overwritten (a wholesale write of 0x06 somewhere). If
    0x23, the gating registers 0x26 / 0x25 / 0x36 (undocumented, set for FM
    by `fmMode()` / `radio_enableAfOutput()`, not touched by
    `dmrbb_configure()`) probably mute LINEOUT in DMR mode. The monitor
    screen has no raw write action (its actions are `Resync`, `Mode 0x10`,
    `Reg 0x06`, `BER test` and `Two-point`, `_ui_dmr_monitorAction()` in
    `ui.c`): add a `dmrbb_writeRegister(4, 0x26, 0xFD)` action and one
    toggling bit1 of 0x36 to that list, as 4.2 does for its sweep (or write
    them with a debugger), then listen. Record the combination that works;
    the DMR table of `dmr_CS7000.cpp` gets it. As a last resort the I2S3
    path (H7 IOSWP / F4 I2S3ext) is the open question of the design note.
  - silence and `Aud RX` at 0 while the call is on screen: the payloads
    never reach the audio layer, see 4.3 (V_SPI) first; `Aud out` counting
    means the thread runs and plays silence.
  - `Aud out` at 0: `dmrAudio_startRx()` failed on the granted path (no
    output device on `SINK_SPK` in the target's `outputDevices[]`, or
    `audioStream_start()` refused). Check `audio_CS7000.cpp`.
  - gaps or clicks every 60 ms: the DAC FIFO underruns between blocks
    (`Underrun` at 0 rules out the payload side). The 4 ms refill of
    `Cx000_dac.cpp` competes with the slot passes on SPI2; measure the
    `dmrbb_waitEvent()` SPI time and, if needed, raise the DAC thread's
    priority or shorten the SYS pass.
  - the tone continues after the terminator: the thread is not stopped at
    the hang time (`OpMode_DMR::followState()`), or the DAC keeps playing
    its last half; `audioStream_stop()` waits for the FIFO.
  - `TS` / `SYS` stop or slow down while the tone plays: OpenMusic's 0x88
    bit0 poll or the SOUND page writes interfere with the DMR interrupts
    (the 0x88 register overlap of the design note). Try 0x06 = 0x21 with
    OpenMusic off (tone off) to confirm, then look at `Cx000_dac.cpp`'s
    write sequence.

### 4.2 Internal-vocoder experiment: does the chip produce audio by itself?

The premise of the original brief was that the HR_C6000 decodes AMBE
internally. The manual (§4.6) and OpenGD77 say it does not; this experiment
settles it on the CS7000. Documentation predicts silence in every
combination.

- Where: monitor screen actions `Reg 0x06` (cycles 0x21, 0x01, 0x24, 0x20,
  0x00) and the raw writes `dmrbb_writeRegister(4, 0x22, v)` /
  `dmrbb_writeRegister(4, 0x36, v)` (add them to the action list if the
  screen does not offer them when the experiment is run).
- Do: with `Test tone` off (null backend: the MCU plays silence, so
  anything heard comes from the chip), receive a long call and, on the
  monitor screen, sweep 0x06 over {0x21, 0x01, 0x24, 0x20, 0x00}, and for
  each value 0x22 over {0x26, 0x20} and 0x36 over its bit1 variants; listen
  on LINEOUT (test point after the TC75S51F, or the speaker with the volume
  up). Remember that `dmrbb_configure()` rewrites the registers within 2 s
  of a silent channel: run the sweep with the call up.
- Expect: silence in every combination (`Aud out` counting, `Aud RX`
  counting, nothing audible but the amplifier hiss).
- If voice is heard in some combination: the chip does contain a decoder,
  or IC611 "HR_V3000S" (I2C, PB8/PB9) is a vocoder companion driven through
  it. Record the combination; the null backend is then bypassed by leaving
  the chip in that configuration and not opening the MCU audio stream (a
  `dmrbb` flag), which becomes the fast path of the design.

### 4.3 V_SPI voice payload logging: the bytes must vary per burst

- Where: `dmrbb_waitEvent()` of `dmr_CS7000.cpp` (V_SPI command 0x83, 27
  bytes into `dmrbbSnapshot::voice`, `voiceValid` on a voice sync class
  with a good CRC), register 0x06 (0x21 = V_SPI, 0x01 = RX RAM 0x30 over
  U_SPI, the `DMRBB_DRV_VOICE_PATH` driver knob).
- Do: receive a call from a hotspot running MMDVMHost with `Debug=1` in
  the DMR section (it dumps the AMBE bytes of every burst) and watch
  `Aud RX` and `Aud vary` on the monitor screen; with a debugger or a
  `printf` on the USB serial, log the 27 bytes of a few consecutive
  bursts.
- Expect: `Aud vary` follows `Aud RX` within one or two (every burst
  differs from the previous one: AMBE frames of speech never repeat), and
  the logged bytes match the hotspot's dump for the same burst (same 27
  bytes, MMDVMHost prints them in air order).
- If wrong:
  - `Aud RX` at 0 during a call: `voiceValid` is never set. The sync class
    of 0x51 or the CRC bit is misread (3.5), or V_SPI returns nothing with
    0x06 = 0x21. Bit3 of 0x06 (SPIFrom) is already 0 in 0x21, which the
    manual gives as "V_SPI out = received DMR payload", and every value
    the `Reg 0x06` action cycles (0x21, 0x01, 0x24, 0x20, 0x00) keeps it
    at 0, so that sweep does not touch it. First try the other transport,
    `DMRBB_DRV_VOICE_PATH` = 1 (RX RAM 0x30 over U_SPI, a rebuild of
    `dmr_CS7000.cpp`); only if a bit3 = 1 experiment is wanted, add 0x29
    to the `Reg 0x06` action values (`dmrMonR06Values` in `ui_menu.c`).
  - `Aud RX` counting, `Aud vary` at 0 or 1: the same bytes every burst
    (all zeros, or a stale buffer): the V_SPI read is wired to the wrong
    port or clocked wrongly (VOC_* nets, PB3-PB6, C6000 slave), or reads
    the TX buffer. Try the RX RAM path.
  - bytes vary but do not match the hotspot dump: bit or byte order of
    the V_SPI transfer, or the three 72-bit frames are interleaved by the
    chip as the manual's Figure 6.2 (the payload is the 216 bits after
    de-interleaving); with a real backend out of tree, the decoded audio
    tells; without one, compare the position of the 49 bits of each frame.

### 4.4 No rattle when the call ends and the DAC FIFO stalls; C6K_SLEEP

- Where: `rxFunc()` of `dmr_audio.c` (`audioStream_stop()` on the way
  out), `Cx000_dac.cpp` (stop: waits for the FIFO, clears OpenMusic),
  `OpMode_DMR::followState()` (the thread is stopped at the hang time, the
  path released after it), `OpMode_DMR::disable()` and
  `OpMode_DMR::reprogramModem()` (the thread is stopped first, then the
  controller is disabled or restarted and `dmrbb_terminate()` /
  `dmrbb_configure()` rewrite the chip, then the paths are released).
  Mode-switch caveat: `rtx_task()` (`rtx.cpp`) calls `radio_setOpmode()`
  before `disable()`, and on the CS7000 radios `radio_setOpmode()` already
  runs `dmrbb_terminate()` and `C6000.fmMode()` (`radio_CS7000.cpp`), so
  when DMR mode is left with the mode key the thread's drain runs on a chip
  already back in FM mode with 0x06 rewritten, and the `dmrbb_terminate()`
  of `disable()` is a no-op by then. A power-off goes through
  `rtx_terminate()` -> `disable()` with the driver still in DMR mode.
- Do: with the test tone on, let a call end normally (terminator), let one
  end by voice loss (switch the hotspot off mid-call, 720 ms timeout),
  leave DMR mode with the mode key during a call, and power the radio off
  during a call.
- Expect: the tone stops cleanly in all four cases, no rattle, buzz or
  repeated last block from the speaker while the hang time runs or after
  the mode switch; the C6K_SLEEP line (`platform/targets/CS7000*/pinmap.h`)
  never toggles while an audio path is open (it is driven only by
  `radio_init()` / `radio_terminate()` and the sleep sequence, never by the
  DMR driver or the audio layer; a scope on the pin during a call stays
  flat).
- If wrong: a rattle at the end of a call means the DAC keeps consuming
  the last half after the stream was stopped without the FIFO being
  cleared; look at the stop sequence of `Cx000_dac.cpp` (OpenMusic cleared
  before or after the FIFO drains) and at the order in `followState()`. A
  rattle, or a stall of the rtx thread (the display freezes for up to
  1.5 s, the DAC driver's stall fallback), only on the mode-key case means
  the drain does not complete on a chip in FM mode: the fix is then in
  `rtx.cpp` (call `disable()` before `radio_setOpmode()`) or in
  `radio_CS7000.cpp`'s `radio_setOpmode()`, not in `disable()`. C6K_SLEEP
  toggling with a path open means a sleep/wake sequence is triggered by
  the mode switch while the DAC is streaming; `radio_disableRtx()` and the
  DMR `dmrbb_terminate()` must not touch it.

## 5. Stage 5: the TX path (`radio_CS7000.cpp`, `dmr_CS7000.cpp`)

Stage 5 transmits: `radio_enableTx()` keys the RF chain in DMR exactly as
in FM and M17 (TX VCO through `VCOVCC_SW`, SKY72310 on the TX frequency,
lock detect on `PLL_LD` before anything else, APC voltage on the DAC, then
`TX_PWR_EN`), and writes nothing to the HR_C6000: the modem is armed by the
call controller through `dmrbb_startTx()` (0x21 = 0xA2 polite / 0x02
impolite, then 0x40 = 0xE3 direct mode / 0xC3 repeater mode) and bursts go
out only from the `0x41 = 0x80` of a later slot tick, with the frame type in
0x50, the link control in TX RAM 0x00-0x0B (opcode 0x02, one chip select)
and the 27 voice bytes on V_SPI (command 0x03, address 0x00). The PA stays
keyed for the whole call, from the arming of the modem to one slot after
the terminator (or the wake-up CSBK) was programmed, earlier on the chip's
"service transmission completely terminated" report (0x82 bit5 with 0x86
bit7; the per-slot 0x86 bit4 sub-interrupt on the same 0x82 bit does not
release it), or after 35 ms when neither arrives; the falling edge of
`TX_PWR_EN` comes from `radio_disableRtx()`, followed by `radio_enableRx()`.
`RF_TX_INTER` is only counted and timestamped. A PLL that does not lock
(`radio_getStatus()` reads `OFF` after `radio_enableTx()`) aborts the
transmission at once with the `RF failed` marker, nothing being shaped into
the parked VCO. The test coverage is in `tests/unit/hr_c6000_dmr.cpp` (byte
streams), `tests/unit/radio_cs7000.cpp` (keying order against the FM path)
and `tests/unit/DMR_opmode.cpp` (keying around the call on the emulator).
Nothing below has been run on a radio: the whole section is `UNVERIFIED on
hardware`.

Before keying a radio for the first time: dummy load or attenuator on the
antenna port, low power, a DMR ID of your own (BrandMeister drops
unregistered and 1xxxxxx IDs), and a receiver (SDR, test set or the second
radio) to see what actually goes out. The two knobs of this stage are
actions of the monitor screen (section 2): `LCSS mode` (the convention of
the LCSS bits of 0x50 in voice bursts, `state.dmr_lcssMode`: `50 chip` =
OpenGD77's 0x08 | seq << 4, `50 manual` = the manual's 0x08 / 0x19 / 0x2B /
0x3B / 0x4A / 0x58) and `RF advance` (0x12 = 0x98 or 0x15). Two read-only
counters of the CS7000 driver have no monitor row yet and are read with a
debugger or `dmrbb_readRegister(DMRBB_PAGE_DRIVER, addr)`
(`platform/drivers/baseband/dmr_CS7000.h`): `DMRBB_DRV_RFTX_TICK`
(0x20-0x23, tick of the last `RF_TX_INTER`) and `DMRBB_DRV_RFTX_LEAD`
(0x24-0x25, milliseconds from the last slot tick to the last `RF_TX_INTER`);
a monitor row for the lead is a follow-up. The per-transmission counters
of the main flow are on the audio page: `TX bursts`, `TX silent`, `TX
under` and `RF keyed`.

### 5.1 PA keying, PLL lock and the RF_TX_INTER lead

- Where: `radio_enableTx()` / `radio_disableRtx()` in `radio_CS7000.cpp`,
  the ISR of `dmr_CS7000.cpp` for the timestamps.
- Do: scope `TX_PWR_EN` (74HC595 bit 9, net 5TC), `PLL_LD` (PD11) and PC2
  (`RF_TX_INTER`), plus the RF envelope on a detector or the SDR waterfall.
  Key the radio in DMR on the dummy load for 5 s and release. Read `RFTX`
  and `RF keyed` on the monitor screen and, with a debugger (no monitor row
  yet), `DMRBB_DRV_RFTX_LEAD`.
- Expect: `TX_PWR_EN` rises once per PTT press (`RF keyed` counts 1 per
  press), after `PLL_LD` asserts and within a few milliseconds of the
  press, before the first header burst; it stays high until the terminator
  has gone out, one slot (30 ms) after the controller went back to RX, or
  less if the chip reports 0x82 bit5 with 0x86 bit7 first (a single
  falling edge, from `radio_disableRtx()`, `radio_enableRx()` following at
  once); no RF before `TX_PWR_EN`. PC2 pulses once per own slot (16.7/s)
  while transmitting, `RFTX` counts at that rate, the lead reads 27 or
  28 ms (2.4 ms before the next slot boundary with 0x12 = 0x98). On the
  waterfall: bursts of 27.5 ms every 60 ms in one slot, an unmodulated
  carrier in the other slot (direct mode, whole-call keying), nothing
  outside the channel mask; the last burst on the air (the terminator) is
  complete, not cut by the falling edge. A mode change (MONI + 5) or a
  channel change to an FM channel while transmitting drops `TX_PWR_EN`
  before the chip is reprogrammed to FM, with no carrier left on the air
  during the teardown.
- If wrong: RF without `TX_PWR_EN` or `TX_PWR_EN` before `PLL_LD`: the
  driver order is broken, compare with the FM path (both are asserted by
  `tests/unit/radio_cs7000.cpp`). `PLL_LD` never asserts: the TX VCO does
  not reach the frequency (issue #436 territory), the driver stays OFF,
  the handler aborts the transmission and shows `RF failed` in the top bar
  (`OpMode_DMR::keyRf()`); try another frequency. The terminator is cut
  (the hotspot logs a call timeout instead of `end of voice
  transmission`): the release comes too early, check the `0x86` row after
  the release (`10` or `40` would mean a sub-interrupt other than bit7 got
  through the controller's check) or widen `CallController::RF_UNKEY_MS`
  by a slot. No PC2 pulses: 3.1 (pin mapping) or 0x12 = 0x00 (the
  advance disabled, `rd 0x12`). Bursts do not fall on the slot boundary
  (visible against a repeater or in a two-slot capture): 0x2E = 0x04 (400
  us internal TX delay compensation) is wrong for this board. The
  adjacent-slot carrier bothers a repeater or fails an emissions check:
  per-slot keying from `DMRBB_EV_RFTX` in the rtx thread (`TX_PWR_EN` on at
  the RF_TX event, off at the next slot tick) is the follow-up, the
  74HC595 update takes tens of microseconds and fits the 2.4 ms lead.

### 5.2 Deviation: two-point balance, then 4FSK on air

- Where: 0x45 / 0x46 from `txDigitalPathQ` / `txDigitalPathI` in
  `radio_updateConfiguration()`, the two-point bias 0x47 / 0x48 / 0x04 from
  `radio_init()`, the `Two-point` action (0x12 bit 6) of the monitor screen.
- Do: 3.9 first (`Two-point` action, key on the dummy load, 40 Hz square
  wave on the deviation meter, `Two-point` again to clear the bit). Then a
  plain DMR TX into a DMR test set or an SDR with a 4FSK/DMR decoder
  (`dsd-fme`, `SDRangel`, an MMDVM modem in calibration mode `MMDVMCal`
  DMR BER mode): symbol deviation and the emission mask.
- Expect: square wave with flat tops on both edges after the balance;
  +/- 1.944 kHz for the outer symbols and +/- 648 Hz for the inner ones
  (TS 102 361-1 Table 10.2, +/- 10 %), the DMR emission mask met, BER 0 %
  on the MMDVMCal / test set reading.
- If wrong: tilted square wave: the 0x45 / 0x46 ratio (write both from the
  monitor screen when the register write row exists, or rebuild). Deviation
  off by a constant factor with a good balance: the `txAnalogPath*` pair may
  be the right one for this radio, or the interpolation is off: try
  `txAnalogPathI` / `txAnalogPathQ` and a fixed pair from the OEM
  calibration dump (`tests/platform/calib_read.c`). Deviation right but BER
  bad: 0x2E timing or the IQ swap bit of 0x01 (0xB0 keeps the OEM value).

### 5.3 Hotspot acceptance (Pi-Star / MMDVMHost, simplex hotspot)

- Where: the whole TX path, judged by the hotspot's log
  (`/var/log/pi-star/MMDVM-*.log` on Pi-Star, `Log` section of `MMDVM.ini`
  otherwise) with `[DMR] EmbeddedLCOnly=0` first.
- Do: hotspot on the radio's frequency, same colour code, TS2 (simplex
  hotspots use TS2), radio in direct mode (no BS sync seen, `Sync` on the
  monitor screen not `BS`). Key for 5 s with the null vocoder (silence
  frames), release. Then key again with the hotspot's RF blocked for the
  first second (hand over the antenna, or key out of range and walk in) to
  test the late entry, and repeat the late entry with `EmbeddedLCOnly=1`,
  which makes MMDVMHost rebuild the LC from the embedded fragments only.
  Then the busy test: with the polite policy, key while the hotspot is
  transmitting on the slot (someone talking on the talkgroup, or the
  hotspot's own `Voice` announcement); repeat with `Access = Impolite`.
- Expect: log lines `DMR Slot 2, received RF voice header from <ID> to TG
  <n>`, then, at the release, `DMR Slot 2, received RF end of voice
  transmission from <ID> to TG <n>, 5.0 seconds, BER: 0.0%` (a duration
  matching the PTT and a BER under 1 %); BrandMeister's last-heard shows
  the call. Late entry: `DMR Slot 2, received RF late entry from <ID> to TG
  <n>` in both `EmbeddedLCOnly` settings. Busy with the polite policy:
  no RF, the `busy` marker on the main screen (0x82 bit 7, `0x82` = `80`
  on the monitor screen, `HR_C6000::txRejected()`); impolite: the radio
  transmits regardless.
- If wrong: no header logged at all but RF is seen: deviation (5.2),
  colour code, or the LC header carries zero parity: register 0x40 bit 3
  (0xE3 has it clear = "the chip computes the RS(12,9) / CRC parity",
  UNVERIFIED): if the hotspot logs nothing while a wideband capture shows
  bursts with a valid sync, compute the RS(12,9) parity on the MCU with the
  stage-1 FEC module (`openrtx/src/protocols/DMR/FEC`, `FullLC::toChipLc()`
  already lays the 9 octets out) and set bit 3 (0xEB / 0xCB). Header
  logged but `end of voice transmission` missing or with a wrong duration:
  the terminator (0x50 = 0x20, LC still in TX RAM) or the voice sequence.
  BER above a few percent: 5.2 or the 0x2E / 0x12 timing. No late entry
  with `EmbeddedLCOnly=1` while it works with 0: the LCSS convention:
  run the `LCSS mode` action of the monitor screen so that its value
  column reads `50 chip` (i.e. the manual sequence 0x08 / 0x19 / 0x2B /
  0x3B / 0x4A / 0x58 is now selected, `dmr_lcssMode` = 1), go back to the
  main screen and repeat; if neither works, the chip does not build the
  embedded LC from the TX RAM and the fragments have to be supplied by the
  MCU (a follow-up). Silence frames not decoded as silence
  by the network (noise on the far end): the V_SPI orientation or the TX
  RAM alternative (`DMRBB_DRV_VOICE_PATH` = 1, with `Reg 0x06` = 0x01).
  Busy never rejected: 0x21 = 0xA2 versus the manual's 0xA3 (3.4 for the
  0x82 polarity as well).

### 5.4 Repeater acceptance (RMO, passive timing, wake-up)

- Where: `CallController` repeater branch through `dmrbb_startTx(false)`
  (0x40 = 0xC3, bursts aligned to the received sync) and the wake-up CSBK
  (0x50 = 0x30, TX RAM 0x00-0x09).
- Do: a repeater with a known talkgroup on one slot, the radio configured
  with the repeater's colour code and slot, a second radio (or
  BrandMeister last-heard with a registered ID) listening. First with the
  repeater on air (someone talking, or right after its own hang time):
  check `Sync` reads `BS` and `Lock` reads 2 (timecode) on the monitor
  screen, key for 5 s. Then with the repeater idle (carrier dropped): key
  and watch the repeater come up.
- Expect: no TX before `Lock` = 2 (the controller waits for the timecode);
  the bursts land in the configured slot (the repeater rebroadcasts the
  call on that slot, the second radio hears it on the same TS, and the
  repeater does not log a colour code / slot error); from idle: one or
  two wake-up CSBKs (`0x50` = 0x30 visible as a short RF blip), the
  repeater's carrier comes up within `T_SyncWu`, then the normal call;
  after two unanswered wake-ups the `wake-up failed` marker and no call.
- If wrong: TX in the wrong slot: the timecode polarity of 0x52 bit 2
  (3.5), or the slot filter of the controller. The repeater never wakes
  up: the CSBK content (BS_Dwn_Act, CSBKO 0x38 with the FID and the own
  ID, TS 102 361-2 §7.1.3), or the CSBK CRC (0x40 bit 3 again: the chip is
  expected to append the CRC-CCITT with the 0xA5A5 mask); a `BER test`
  capture of the repeater's answer helps. Bursts collide with the
  repeater's (the second radio hears both garbled): 0x2E, or the passive
  timing of 0x40 = 0xC3 lost to 0xE3 somewhere (`rd 0x40` reads `C3` while
  transmitting).

### 5.5 OEM-vs-OpenRTX interoperability in direct mode

- Do: an OEM-firmware CS7000 (or any DMR radio) as the second radio, same
  frequency, colour code and talkgroup, direct mode: call from OpenRTX to
  OEM, then from OEM to OpenRTX (the stage-3 RX path), both group and
  private calls.
- Expect: the OEM radio shows the OpenRTX ID / talkgroup as the caller for
  the whole call and returns to idle at the release (terminator decoded:
  no hang beyond its own hang time); OpenRTX shows the OEM caller within
  360 ms of the header and `RX_HANG` after the OEM release. With the null
  vocoder the OEM radio hears silence, not noise.
- If wrong: the OEM radio shows the caller only after the first second:
  the headers (three, in the first three own slots) are not decoded, 0x40
  bit 3 (5.3). It shows the caller but the display stays after the release:
  the terminator. Noise instead of silence: the silence pattern reached the
  chip byte-swapped or on the wrong port (V_SPI orientation, 5.3).

### 5.6 Unknowns this stage depends on, all `UNVERIFIED on hardware`

| Unknown | Assumed | Where | If wrong |
|---|---|---|---|
| 0x40 bit 3, who computes the RS(12,9) / CRC parity of headers, terminators and CSBKs | 0 = the chip, from the 9 or 10 octets in TX RAM | `HR_C6000::dmrTxStart()`, `dmrbb_writeTxLc()` | headers carry zero parity, nobody decodes the call: compute RS(12,9) / CRC-CCITT on the MCU with the stage-1 FEC module, write the 12 bytes, set bit 3 |
| LCSS bits of 0x50 in voice bursts | OpenGD77: 0x08 \| seq << 4, LCSS 00, chip fills the fragments | `DMR::r50Voice()`, `LCSS mode` action (`dmr_lcssMode`) | no late entry on the hotspot: `LCSS mode` action (manual sequence); if still none, MCU-built fragments |
| Voice payload transport | V_SPI, command 0x03 address 0x00, 27 bytes (0x06 = 0x21) | `dmrbb_writeVoice()`, `DMRBB_DRV_VOICE_PATH` | noise on the far end: knob = 1 (TX RAM 0x30) with `Reg 0x06` = 0x01, or the V_SPI pin orientation (pinmap.h) |
| 0x21 arming value | 0xA2 polite / 0x02 impolite (bit 1 clears the encode buffer) | `HR_C6000::dmrTxStart()` | busy never rejected or TX never starts: manual's 0xA3 / plain 0xA0 |
| 0x40 arming value | 0xE3 direct / 0xC3 repeater (TxEn + RxEn) | same | manual's 0xA3 (TxEn only) if the chip refuses to transmit with RxEn set |
| RF timing: 0x12 advance, 0x2E delay, PA latency | 0x98 (2.4 ms, ramp smoothing), 0x04 (400 us), PA keyed for the whole call | `dmrMode()`, `radio_enableTx()` | bursts off the slot boundary: 0x2E; adjacent-slot emissions: per-slot keying from `DMRBB_EV_RFTX` |
| 4FSK deviation pair | `txDigitalPathQ` / `txDigitalPathI` on 0x45 / 0x46 | `radio_updateConfiguration()` | deviation off: the analog pair, see 5.2 |
| 0x22 encoder start (manual step 4, 0x22 = 0x80) | not written: the MCU is the vocoder (0x06 = 0x21), no chip-side encoder to start | - | TX start flag (0x84 bit 7) never seen or no voice bits go out: write 0x22 = 0x80 after the header, 0x40 after the terminator |
| TX flags 0x82 bit 7 / 6 / 5 and sub-status 0x84 / 0x86 bit 7 | rejected / started / ended, voice bit 7; 0x82 bit 7 arrives through SYS_INTER for the busy path | `dmrbb_waitEvent()`, `HR_C6000::txRejected()` etc. | monitor screen `0x82` / `0x84` / `0x86` rows tell |
| 0x86 sub-status bits under 0x82 bit 5 | bit 7 alone ends the transmission; bit 4 (Layer 2 per-slot deadline, Rdy_lst_inter) and the data bits come while the last burst is on the air and are ignored for the RF release; 0x87 = 0xFF lets all of them through | `CallController::onSysEvent()`, `R86_TX_COMPLETE` | terminator cut: mask 0x87 down to 0x80 in `dmrMode()`, or a sub-status other than bit 7 turns out to be the real end |
| RF release margin after the last burst | one slot after the terminator / wake-up CSBK was programmed, or 35 ms without a timeslot interrupt, is enough for the PA to see the whole burst (manual 5.4.5 step 12: "after the next or a few 30 ms") | `CallController::unkeyAfterBurst()`, `RF_UNKEY_MS` | terminator cut on the waterfall or the hotspot: one more slot |
| Whole-call PA keying | acceptable for the adjacent-slot emissions of a handheld | `OpMode_DMR::keyRf()` | per-slot keying from `DMRBB_EV_RFTX` (driver follow-up) |
| Wake-up CSBK timing | sent with the chip's own timing, the RF released one slot after it while the modem is put back into passive RX in the same slot (0x40 = 0xC3, 0x41 = 0x20 / 0x50 written with the CSBK on the air) | `CallController` TX_WAKEUP | repeater never sees the CSBK: keep 0x40 = 0xE3 until the RF release |
| `radio_disableRtx()` / `radio_enableRx()` one slot after the controller's own `dmrbb_startRx()` | a second resync 30 ms later is harmless (the chip is already in RX) | `OpMode_DMR::keyRf(false)` | `TS` stops counting after a transmission: drop the `dmrbb_startRx()` of the RF release |

## 6. Results

Fill one column per radio; leave a cell empty until the check was run.

| # | Check | Expected | CS7000 (F405) observed | CS7000-M17 Plus (H743) observed | Date / who |
|---|---|---|---|---|---|
| 1.1 | PC2_C / PC3_C reach EXTI | PC2 / PC3 count | | | |
| 1.2 | SYS_INTER pin | PC1 or PC3 counts at 16/s | | | |
| 1.3 | EXTI latency behind other handlers | no TS skipped | | | |
| 3.1 | RF_TX_INTER on PC2 | PC2 and RFTX count in TX | | | |
| 3.2 | Mode word | 0x6E / 0x6A gives TS | | | |
| 3.3 | 0x41 RX value | 0x51 cycles 10, 11..61, 20 | | | |
| 3.4 | 0x81/0x85/0x87 polarity | SYS counts, 0x84 = 80 in TX | | | |
| 3.5 | 0x51/0x52/0x5F bits | CC, TC, Sync, CRC as listed | | | |
| 3.6 | 0x14-0x16 byte order | CE CA 23 for ID 2345678 | | | |
| 3.7 | U_SPI 12.5 MHz readback | rd 0x10/0x06/0x12 as written | | | |
| 3.8 | AK2365A IF path | BER frames, RSSI tracks level | | | |
| 3.9 | Two-point / deviation | flat 40 Hz square, +/- 1.944 kHz | | | |
| 3.11 | Round trip, beeps, late entry | FM/M17 beeps and audio as before, FM/M17 intact after DMR, TS unaffected, caller in 360 ms | | | |
| 4.1 | Test tone on a received call | tone for the whole call, `Aud RX` counts, TS/SYS unaffected | | | |
| 4.2 | Internal-vocoder experiment | silence in every 0x06 / 0x22 / 0x36 combination | | | |
| 4.3 | V_SPI payload | `Aud vary` follows `Aud RX`, bytes match the hotspot dump | | | |
| 4.4 | Call end, DAC FIFO stall, C6K_SLEEP | no rattle, C6K_SLEEP flat | | | |
| 5.1 | PA keying, PLL lock, RFTX lead | TX_PWR_EN after PLL_LD and before the first header, once per PTT (`RF keyed` +1), off one slot after the terminator; PC2 16.7/s; lead 27-28 ms | | | |
| 5.1 | PLL lock failure | `RF failed` marker, no TX_PWR_EN, back in RX | | | |
| 5.1 | Mode / channel change while transmitting | TX_PWR_EN off before the FM reprogramming, no carrier during the teardown | | | |
| 5.1 | Null backend counters | `TX bursts` = `TX silent`, `TX under` 0 | | | |
| 5.2 | 4FSK deviation on air | +/- 1.944 kHz / +/- 648 Hz, mask met, BER 0 % | | | |
| 5.3 | Hotspot: header, end, duration, BER | `received RF voice header`, `end of voice transmission` 5.0 s (terminator complete), BER < 1 % | | | |
| 5.3 | Hotspot: late entry, EmbeddedLCOnly 0 / 1 | `received RF late entry` in both; `LCSS mode` value that works | | | |
| 5.3 | Busy channel, polite / impolite | rejected with `Busy` marker, RF keyed for the arming pass only / transmits | | | |
| 5.4 | Repeater: BS sync, lock, slot, wake-up, rebroadcast | no TX before Lock 2, right TS, `WAKE` on the destination line then `TX` or `Wake-up failed` within ~0.8 s, repeater wakes, second radio hears | | | |
| 5.5 | OEM interop, both directions | caller shown for the whole call, idle at release, silence not noise | | | |

## 7. Flashing back to the OEM DMR firmware

Connect Systems publishes, on the CS7000 M17 PLUS software page, the OEM
bootloader (`M17PLUS_Bootloader_B7.06.dfu`) and the DMR + analog host
firmware (V9.00.96) together with DfuSeDemo / STM32CubeProgrammer, and its
application note "Converting DMR to M17 Protocol" states that the radio
carries either the DMR or the M17 (OpenRTX) firmware, never both, and
describes the erase-and-load procedure with DfuSeDemo in the OpenRTX
direction. The reverse procedure is not documented in this tree: the
`dmr/flashing_dmr.md` page of the M17-Project `cs7000-wiki` is a placeholder
with no steps yet, and nobody on this series has done it. Until somebody
writes it down here after doing it, the only guidance is the outline above:
put the radio in DFU mode, load the OEM bootloader `.dfu` with
DfuSeDemo / STM32CubeProgrammer, then the OEM firmware with the tool Connect
Systems ships for it, and keep a copy of the OEM firmware files before
flashing OpenRTX.
