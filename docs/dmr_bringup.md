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
13, 7, 7 and 5 rows, each starting on a new page and spanning as many pages as
the screen height needs: six rows per page on the 160x128 displays, hence
2 + 3 + 2 + 2 + 1 = 10 pages, five rows per page on 128x64, hence
3 + 3 + 2 + 2 + 1 = 11 pages (`_ui_dmrMonitorPages()` in `ui_menu.c`; the title reads
`DMR monitor n/N`):

| Section | Rows | Source |
|---|---|---|
| 1. Counters | `TS`, `SYS`, `RFTX` interrupt counts; `PC0`..`PC3` EXTI pulses per line; `Lock` (0 none, 1 slots ticking, 2 timecode locked); `Timecode`; last `0x82`, `0x84`, `0x86` | `dmrbbSnapshot` counters and `lineCount[]`, refreshed on every pass of the opMode handler including the 30 ms timeouts (a line pulsing without raising an event, PC3 while the pinmap expects SYS on PC1, is counted all the same); flags of the last SYS event |
| 2. Registers | last `0x51`, `0x52`, `0x5F`, `0x42`; `CC` (0x52[7:4]); `TC` (0x52 bit2); `Sync` (0x5F[1:0]: MS, BS, TDMA1, TDMA2); `RSSI` (0x43:0x44); `FSK err` (0x0F); `rd 0x10`, `rd 0x06`, `rd 0x12`, `rd 0x14-16` read back over U_SPI at every redraw | registers read in the TS/SYS passes; `dmrbb_readRegister()` |
| 3. Link control | `LC 0-5`, `LC 6-11` (the 12 LC RAM octets in hex), `Src`, `Dst`, `FLCO`, `FID` decoded from them, `CRC` (`ok`/`bad` from 0x51 bit2, `*` appended when the 9 octets do not parse as a standard Full LC) | RX RAM of the last header, terminator or late entry |
| 4. Audio (stage 4) | `Vocoder` (backend name), `Aud RX` (payloads pushed to playback), `Aud vary` (of which differing from the previous one), `Aud out` (480-sample blocks written to the speaker), `Aud TX` (payloads encoded from the microphone), `Underrun` (silent blocks between two payloads of a call), `Overrun` (payloads dropped from a full queue) | `rtxStatus_t::dmr_audio`, `dmrAudio_getStats()` of `core/dmr_audio.c` |
| 5. Actions | `Resync`, `Mode 0x10`, `Reg 0x06`, `BER test`, `Two-point`, the value column showing the register and the value the next ENTER writes | `dmrbb_writeRegister()` on the CONFIG page (U_SPI opmode 4) |

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

Two things to keep in mind:

- The modem is reprogrammed from scratch (`dmrbb_configure()`, table 4.1 of
  the design note) whenever the colour code, timeslot, ID, access policy or
  mode word changes, and after 2 s without timeslot interrupts. `Reg 0x06`,
  `BER test` and `Two-point` are therefore overwritten within 2 s while no
  signal is being received; read `rd 0x06` / `rd 0x12` right after the
  action, or run the check with a signal present. `Mode 0x10` survives, see
  above.
- PTT is disabled on every menu screen. For the checks that need
  transmission, run the action, press ESC back to the main screen, key the
  radio (a DMR transmission with silence frames), release, and come back to
  the monitor: the counters and readbacks are kept.

## 3. Hardware checks, in order

Each check names the unknown, what to do, what is expected and what to change
where if it fails. Record the outcome in the table of section 5.

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

## 5. Results

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

## 6. Flashing back to the OEM DMR firmware

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
