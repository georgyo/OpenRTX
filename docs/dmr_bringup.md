# DMR bring-up checklist

This document lists, in the order in which they should be checked, every
assumption of the DMR series that comes from the HR_C6000 manual, the
CS7000-M17 Plus schematic or the observation of other firmwares and has not
been confirmed on a CS7000 or CS7000-M17 Plus. Each entry names the code that
carries the matching `UNVERIFIED on hardware` comment, what to observe, and
what to change if the observation disagrees.

The list grows with the stages of the series (see [dmr_mode.md](dmr_mode.md));
an entry is removed, and the comment in the code with it, once somebody has
checked it on a radio and recorded the result here.

## 0. First run on a radio (2026-11)

A first attempt on a CS7000-M17 Plus, simplex on TG 1 / TS 1 / CC 1 against
a second DMR radio, gave no decodable transmission and an unstable display
on reception. No register was read back on that occasion. The review that
followed found these defects, all fixed since:

| Defect | Effect | Fix |
|---|---|---|
| SYS_INTER was dispatched from PC1, which on the Plus carries the 2T/5T analogue audio node; the schematic puts SYS_INTER on PC3_C | every edge of the received audio ran the system event reads at an arbitrary time | `DMR_SYS_INT` is PC3 on the Plus, a pulse that finds register 0x82 empty is dropped |
| register 0x51 sync class 00 ("no sync", voice bursts B to F) was read as "data burst", burst B as a terminator | a received call was closed on its second burst and reopened by the next late entry | `r51VoiceSeq()`, header and terminator are class 10 |
| register 0x22 was never written in DMR and FM leaves both vocoder directions stopped (0x46) | no voice payload taken for transmission nor delivered on reception after a visit to FM | `dmrMode()` writes 0x26 (decoder started), `dmrTxStart()` 0x86 (encoder started) |
| `radio_enableRx()` restarted the sync search on every configuration update and after every transmission | a call being received was dropped by a key press | the DMR opMode handler is the only owner of the modem |
| the first LC after every return to reception was discarded | a call opened by late entry only | no LC is discarded: the chip has just written the LC RAM when it reports a header or a late entry (first narrowed to the LC after a modem start, removed altogether after the third run, 0.2) |
| a PTT released before the first burst stayed latched | the next press was ignored | latch cleared on release |
| settings saved by an older firmware loaded the DMR fields as zero | hang time 0, colour code 0, impolite access | fields missing from the record keep their defaults |

What to check first with the fixed firmware, on the DMR monitor screen
(section 2) and with a DMR ID set in `Settings > DMR` (without one the
PTT is refused and `No DMR ID` is shown for three seconds):

1. Reception of the other radio: `SYS` and `PC3` count about 16 per second
   and `PC1` no longer matters; `0x51` reads `12` (header), then `11`,
   `20`, `30`, `40`, `50`, `60` (voice A to F, possibly with bit 7 set:
   `91`, `A0` ...), then `22` (terminator); the main screen shows the
   caller and holds it. With `Settings > DMR > Test tone` on, a 1 kHz tone
   plays for the duration of the call: the tree has no AMBE vocoder, so
   speech is never heard.
2. Transmission: `TS` counts 33 per second while the PTT is held, `TX
   bursts` counts 16 per second, `RFTX` and `PC2` count with it. The other
   radio shows the ID and opens on silence, or on a tone with
   `Settings > DMR > Test tone` on. `RF failed` in the top bar
   means the chip gave no timeslot interrupts, `Busy` that it flagged the
   request rejected (0x82 bit 7) while the transmission went on all the
   same (`Settings > DMR > Access` set to impolite rules the flag out).
3. If the other radio still does not open while `TS` and `TX bursts`
   count: the PA is keyed for the whole call while the chip gates its
   modulation DACs per slot (5.1); keying per slot from RF_TX_INTER, which
   needs `RFTX` to count, is the next change to make.

### 0.1 Second run and the trace on the USB serial port

A second run with those fixes gave the same two symptoms: nothing decoded
by the other radio, and on reception with the monitor set to any colour
code the main screen showed the colour code and the error count changing
from burst to burst. Again nothing was read back, so the firmware now
records what the modem does and sends it out by itself.

While the radio is in DMR mode the CS7000-M17 Plus sends a trace on its USB
serial port (`CONFIG_DMR_TRACE`, `openrtx/src/core/dmr_trace.c`): one text
line per record, written by the DMR opMode handler and sent by the device
thread. Capture and decode it on a Linux host with:

```
stty -F /dev/ttyACM0 raw -echo
cat /dev/ttyACM0 > dmr-trace.log
python3 scripts/dmr_trace.py dmr-trace.log
```

The `stty` line matters: with the default settings the host echoes every
line back to the radio. Records written while nobody reads the port are
dropped; when a reader appears the configuration and the registers are
recorded again, so the capture can be started at any time.

A line is the kind, one letter, then six hexadecimal digits of time in
milliseconds, then the data bytes, two digits each:

| Kind | When | Data bytes |
|---|---|---|
| `E` | modem programmed, or a reader appeared | RX colour code, TX colour code, timeslot, monitor, polite, repeater, call type, 0x10 word, 0x12 word, own ID (3), destination (3), RX frequency (4), TX frequency (4), hang time in seconds, test tone, SYS line, bit mask of the armed interrupt lines (bit n = PCn, `DMRBB_DRV_LINES_ARMED`: `0D` on the Plus with SYS on PC3, `0F` on the CS7000) |
| `R` | after every `E`, twelve of them | first register address, then sixteen registers of U_SPI page 4 read back from the chip: rows 0x00 to 0x60, 0x80, 0xA0, 0xB0, 0xC0 and 0xE0 |
| `A` | after the `R` rows of every configuration dump | the six bytes of the transmit sync word read back from the auxiliary registers (U_SPI page 1) 0x04 to 0x09, expected `D5D7F77FD757` |
| `T` | timeslot interrupt, after the controller ran | controller state, local timecode, slot lock, 0x52, 0x42, 0x41 written for the next slot (`FF` none), 0x50 written (`FF` none), TS count (2), TS tick (2), SYS count (2), RF_TX count (2) |
| `S` | system interrupt, after the controller ran | controller state, local timecode, 0x82, 0x51, 0x52, 0x5F, 0x42, 0x84, 0x86, 0x90, 0x98, RSSI (2), 0x0F, voice payload read, LC RAM (12) |
| `V` | system interrupt that read a voice payload | the 27 bytes |
| `W` | modem control call | 1 start RX, 2 start TX with passive timing, 3 start TX with active timing, 4 idle |
| `K` | RF stage keyed or released | key asked, RF state after, status of the radio driver |
| `P` | every second and on a PTT change | controller state, RF state, PTT, markers, call state, slot lock, counters of PC0 to PC3 (2 each), voice bursts asked for transmission (2), of which filled by the encoder (2), voice bursts received (2), RSSI in dBm (2, signed), audio thread running, audio failures, transmit requests the chip flagged rejected (0x82 bit 7 while transmitting, since the modem was started, saturating at 255) |
| `D` | records were lost | their number, eight digits, in place of the time |

Multi-byte values are most significant byte first. The sub-status bytes of
an `S` record are fresh only when their flag in 0x82 is set.

The transmit side of the test tone makes the result audible on the other
radio: with `Settings > DMR > Test tone` on, the voice bursts carry the
AMBE+2 frames of a 1031 Hz tone instead of the silence pattern, so a
receiver that decodes the transmission plays a tone. The `V` records of a
reception show the same nine bytes when the other radio is this firmware,
and `B9E881526173002A6B` when it sends silence.

### 0.2 Third run and the review

A third run gave the same picture: the transmission was still not decoded
by the other radio and reception was unstable. No trace capture was made
on that occasion, so once more there was no read-back to reason from. The
code was then reviewed from scratch,
layer by layer (pins and interrupts, clocks, the two SPI ports, the
register tables, the receive and transmit sequences, call control, the
opMode handler, RF keying), against the HR_C6000 manuals, the CS7000-M17
Plus schematic and two firmwares that work on this chip, OpenGD77 and
OpenMDUV380. What was found and changed:

| # | Defect | Source | Change |
|---|---|---|---|
| 1 | the transmit sync word, auxiliary registers 0x04 to 0x09, was never written: every burst went out with the chip's reset value, the BS sourced data sync | manual, auxiliary register table; ETSI TS 102 361-1 table 9.2 | `dmrMode()` writes D5 D7 F7 7F D7 57, the MS sourced data sync, as OpenGD77 and OpenMDUV380 do |
| 2 | register 0x46 was 0 in DMR: it was taken from `txDigitalPathI`, a calibration array that is never loaded from the flash, so one of the two modulation points had no amplitude | `nvm_readCalibData()`; manual: 0x45 and 0x46 are the two two-point modulation amplitudes | DMR uses the pair FM and M17 use, 0x45 from the array at flash 0x10C4 and 0x46 from the one at 0x10CC. The absolute DMR deviation is `UNVERIFIED on hardware` |
| 3 | register 0x12 was 0x98 in DMR: with bit 7 set the transmit ramp of the modulation DACs starts from 0 V instead of the 1.65 V mid level | manual 4.7.2; the CS7000 init ends with 0x12 = 0x00 | 0x18 in DMR, the same 2.4 ms advance with bit 7 clear; the `RF advance` action toggles 0x18 and 0x15; `fmMode()` restores 0x00 |
| 4 | register 0x36 was 0x10 in DMR: bit 4 is `mcu_pkt_clk_down`, an active-high shut-off of the vocoder packet interface clock, not an enable | manual, chapter 8 register table | 0x36 = 0x00 in DMR, the reset value, as OpenGD77 and OpenMDUV380 do |
| 5 | 0x85 = 0x87 = 0xFF enabled every TX start and TX end sub-interrupt, among them `Rdy_lst_inter` (0x86 bit 4), one more SYS_INTER pulse in every slot | manual, chapter 8 (0x85, 0x87) and 5.4.4 | 0x85 = 0x00, 0x87 = 0x84: "transmission completely terminated" and "Short LC received" only |
| 6 | U_SPI was requested at 12.5 MHz | manual, U_SPI timing: 4 MHz maximum | 4 MHz requested, which gives 3.125 MHz on the Plus and 2.625 MHz on the CS7000 (F405) |
| 7 | V_SPI was bit-banged in SPI mode 0: data in was read before the clock edge that shifts the bit out, so a 27-byte read came back one bit late | manual Figure 4.16 | V_SPI has its own transfer function: data out changes after the rising edge, data in is read at the falling edge, clock idle low, 2 us between the chip select and the first edge |
| 8 | the received Short LC was read from RX RAM 0x24 to 0x28, the address and layout of the transmit side table, and the "voice through RX RAM 0x30" alternative read the raw burst, not a voice payload | manual table 5.3, receive side RAM allocation | the Short LC is read from RX RAM 0x2B to 0x2F as 36 contiguous bits; the RAM voice path and its `DMRBB_DRV_VOICE_PATH` knob are removed, V_SPI is the only voice transport |
| 9 | on the Plus PC1, the 2T/5T analogue receive audio node, was a digital input armed as a falling-edge interrupt, and the result of an interrupt attach was ignored | schematic V01 | PC1 stays an analogue pin and is armed only while the `SYS line` action selects it; the attach results are kept in `DMRBB_DRV_LINES_ARMED` |
| 10 | the first Link Control after every modem start was discarded: the first call after entering DMR, after a setting change or after a reconfiguration was lost unless a late entry followed | ETSI TS 102 361-1 5.1.2.2: a transmission starts with one header | no LC is discarded (`skipLc` removed) |
| 11 | the chip's "abnormal exit" interrupt (0x82 bit 1) ended the call, while it is the chip's per-slot voice sync timer and fires during a call | manual, chapter 8 (0x0D, 0x98) | the interrupt no longer ends a call: the terminator, another data burst in the call's slot and the 720 ms voice timeout do |
| 12 | a burst counted as voice of the call on its sync class and burst number alone, every one of them latched the direct mode slot again, and a late entry opened a call whatever the check flag | manual, chapter 8 (0x51 bit 2 check result, bit 3 privacy indicator); ETSI TS 102 361-1 9.1.2 (burst A carries no EMB) | voice bursts need 0x51 bits 2 and 3 clear and, on bursts B to F, the colour code latched when the call opened; a late entry needs the check flag clear; the direct mode slot is latched again only from a validated burst after the gate lapsed |
| 13 | the chip was never told to search for the sync again while slot interrupts kept coming: it keeps counting slots on a time axis whose signal is gone and follows a new sync only within 1.25 ms of it, while a direct mode station starts a new axis at every PTT; the watchdog reprogrammed the whole modem after 2 s of ordinary silence | manual 5.4.4; ETSI TS 102 361-1 5.2.1.3 | a new search (state `RX_SEARCH`) at the end of a direct mode call, after an own transmission and after 20 idle slots without a validated event; the hang time is a flag and a timer independent of the state; the watchdog no longer reprograms the modem after ordinary silence |
| 14 | a "request rejected" flag from the chip (0x82 bit 7) aborted the transmission and latched the PTT, and nothing shows that the flag is reliable on this radio | manual 5.1.1 | the flag is counted (`txRejected`) and the busy marker is shown while the transmission goes on, as OpenGD77 and OpenMDUV380 carry on |
| 15 | "monitor any colour code" opened the software filter only: with 0x41 bit 3 clear the chip deframes only bursts carrying its local colour code, register 0x1F, which held the TX colour code while receiving | manual, chapter 8 (0x41 `CC_Match_Ctrl`, 0x1F) | with that monitor level the receive slot command is 0x58 instead of 0x50; 0x1F holds the RX colour code while receiving and the TX colour code for a transmission |
| 16 | 0x52 bit 2 read at a slot tick was taken as the timecode of the slot starting there, which on a repeater put the own bursts in the other timeslot | the use OpenGD77 and OpenMDUV380 make of the bit on repeaters | the bit is taken as the timecode of the slot that just ended; direct mode, where the parity is free, is unaffected |

Checked against a primary source and found right:

- the pin assignment and the EXTI setup on the STM32H743: TIME_SLOT_INTER
  on PC0, RF_TX_INTER on PC2_C, SYS_INTER on PC3_C (schematic V01), the
  handlers present in the vector table of the built image;
- the chip clock tree: the 29.4912 MHz crystal with the PLL words of the
  manual's table for it gives a 9.8304 MHz system clock, 2048 x 4800,
  hence exactly 4800 symbols per second and 30 ms slots;
- the 450 kHz IF word, 0x07 to 0x09 = 0B B8 00: 0x0BB800 / 2^24 x
  9.8304 MHz = 450 kHz, the second IF of the AK2365A;
- the U_SPI framing and opcodes (manual table 5.1);
- the real-time margin of the polled rtx thread: the next slot is
  programmed within about 3 ms of TIME_SLOT_INTER, which leaves about
  24 ms of the 27 ms (t2) the manual allows;
- the Link Control bytes and the FEC of the protocol library;
- the receive polarity, 0x01 = 0xB0: the value another radio with the
  same AK2365A IF chain and a low-side first LO uses.

Still open, all `UNVERIFIED on hardware`:

- the PA and the carrier are keyed for the whole call, where OpenGD77 and
  OpenMDUV380 key per burst from RF_TX_INTER (5.1, 5.6);
- the absolute 4FSK deviation with the FM amplitude pair (3.9, 5.2);
- register 0x25 = 0x0E, DAC power switched by slot, against the manual's
  note for a reference oscillator that is DC-coupled to the modulation
  DAC, as it is on this board;
- what the chip does while it searches for a sync, namely whether the
  slot interrupts stop: the watchdog is idle in `RX_SEARCH` on that
  assumption;
- 0x41 = 0x58 and the per-direction colour code in 0x1F have never been
  seen on a radio (3.3).

What the next capture should show first, in this order:

1. the `A` record reads `D5D7F77FD757`;
2. the `R` row 0x40 has non-zero bytes for 0x45 and 0x46;
3. during the other radio's transmission the `S` records read 0x51 = `12`
   for the header, then `91`, `A0`, `B0`, `C0`, `D0`, `E0` for the voice
   bursts A to F (`11`, `20` to `60` if the chip leaves bit 7 clear), with
   a steady colour code in the upper nibble of 0x52;
4. while the PTT is held the `T` records alternate between 0x41 = `80`
   and `00`, and their RF_TX count rises.

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
  `PC3SO`. `platform_init()` of the CS7000-PLUS target clears both bits
  (switches closed) whatever the reset value, so a falling-edge EXTI on
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
- Assumption: the CS7000 pinmap says TS = PC0, SYS = PC1, TX = PC2 (from
  the reverse engineering of the F405 CS7000 firmware); the CS7000-M17 Plus
  pinmap follows its schematic, TS = PC0, RF_TX = PC2_C, SYS = PC3_C, PC1
  being the 2T/5T analogue tone node. `DMR_SYS_INT_ALT` names the other
  candidate on each board.
- Check: the counters of check 1.1 tell which line pulses every 60 ms, see
  3.1.
- If wrong: switch the driver to the other line at runtime with the
  `SYS line` action of the monitor screen (the `DMRBB_DRV_SYS_LINE` knob,
  section 6.3) and carry on with the checks; then fix `DMR_SYS_INT` per
  target. On the CS7000 the driver arms PC0..PC3 all four, so no rebuild
  is needed to see the answer nor to act on it. On the Plus PC1 is that
  analogue node (`CONFIG_DMR_SYS_INT_ALT_ANALOG` in its pinmap): the pin
  stays in analogue mode, without interrupt and with its counter at 0,
  and becomes a digital input with an interrupt only while the `SYS line`
  action selects it. Which lines the interrupt layer accepted is kept in
  `DMRBB_DRV_LINES_ARMED` (driver pseudo page, bit n = PCn) and is the
  last byte of the `E` trace record: `0D` on the Plus with SYS on PC3,
  `0F` on the CS7000 or with PC1 selected; a missing bit 0 means no slot
  is ever programmed.

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
15, 7, 11 and 8 rows (`enum dmrMonitorRows` in `ui_default.h`), each value
section starting on a new page and spanning as many pages as the screen
height needs, the action section always one page whose rows scroll behind
the selection: six rows per page on the 160x128 displays, hence
2 + 3 + 2 + 2 + 1 = 10 pages, five rows per page on 128x64, hence
3 + 3 + 2 + 3 + 1 = 12 pages (`_ui_dmrMonitorPages()` in `ui_menu.c`; the
title reads `DMR monitor n/N`):

| Section | Rows | Source |
|---|---|---|
| 1. Counters | `TS`, `SYS`, `RFTX` interrupt counts; `PC0`..`PC3` EXTI pulses per line; `Lock` (0 none, 1 slots ticking, 2 timecode locked); `Timecode`; last `0x82`, `0x84`, `0x86` | `dmrbbSnapshot` counters and `lineCount[]`, refreshed on every pass of the opMode handler including the 30 ms timeouts (a line pulsing without raising an event, PC3 while the pinmap expects SYS on PC1, is counted all the same; PC1 of the Plus is armed, and counts, only while it is the selected SYS line); flags of the last SYS event |
| 2. Registers | last `0x51`, `0x52`, `0x5F`, `0x42`; `CC` (0x52[7:4]); `TC` (0x52 bit2); `Sync` (0x5F[1:0]: MS, BS, TDMA1, TDMA2); `RSSI` (0x43:0x44); `FSK err` (0x0F); `rd 0x10`, `rd 0x06`, `rd 0x12`, `rd 0x14-16` read back over U_SPI at every redraw | registers read in the TS/SYS passes; `dmrbb_readRegister()` |
| 3. Link control | `LC 0-5`, `LC 6-11` (the 12 LC RAM octets in hex), `Src`, `Dst`, `FLCO`, `FID` decoded from them, `CRC` (`ok`/`bad` from 0x51 bit2, `*` appended when the 9 octets do not parse as a standard Full LC) | RX RAM of the last header, terminator or late entry |
| 4. Audio (stages 4 and 5) | `Vocoder` (backend name), `Aud RX` (payloads pushed to playback), `Aud vary` (of which differing from the previous one), `Aud out` (480-sample blocks written to the speaker), `Aud TX` (payloads encoded from the microphone), `Underrun` (silent blocks between two payloads of a call), `Overrun` (payloads dropped from a full queue); since stage 5 `TX bursts` (own voice bursts sent), `TX silent` (of which carrying the AMBE silence pattern: all of them with the null backend), `TX under` (silence sent while the encoder thread ran, the microphone stream fell behind), `RF keyed` (times the RF stage was keyed for a transmission, one per accepted PTT press) | `rtxStatus_t::dmr_audio`, `dmrAudio_getStats()` of `core/dmr_audio.c`; `dmrMonitor_t` for the TX rows |
| 5. Actions | `Resync`, `Mode 0x10`, `Reg 0x06`, `BER test`, `Two-point`, `LCSS mode`, `RF advance`, `SYS line`, the value column showing the register and the value the next ENTER writes (`LCSS mode`: the convention it selects; `SYS line`: the line in force and the one it selects) | `dmrbb_writeRegister()` on the CONFIG page (U_SPI opmode 4); `LCSS mode` writes `state.dmr_lcssMode` instead; `SYS line` writes the driver pseudo page 0xFF |

The actions, all `UNVERIFIED on hardware` (`openrtx/src/ui/default/ui.c`,
`_ui_dmr_monitorAction()`), are only run while the RTX stage is in DMR on a
working modem: in FM or M17 (`Settings > DMR` is reachable from any mode) or
on a radio without a DMR modem the value column of the action page reads
`DMR off` and ENTER writes nothing, since the chip is then in FM mode and
`0x10 = 0x6A` or the two-point tone would silently take it out of it.

| Action | Writes | Meaning |
|---|---|---|
| Resync | 0x81 = 0xFE, 0x41 = 0x20 | "SyncFail": drop the received timing, search for sync again (manual 5.4.5). The Layer-2 interrupt mask of `dmrMode()` is written first, so that a Resync also ends the extra unmask of a `BER test` |
| Mode 0x10 | 0x10 = 0x6A, then 0x6E, alternately | Mode word: 0x6E is the OpenGD77 value (DMR, Tier II, timeslot mode, Layer 2, repeater, aligned), 0x6A the manual's example. The chosen word is also pushed to the RTX stage (`rtxStatus_t::dmr_modeReg`), so the opMode handler reprograms the modem with it and keeps it through every later reconfiguration; it is forgotten at power off |
| Reg 0x06 | 0x06 = 0x21, 0x01, 0x24, 0x20, 0x00 in turn | Vocoder port variants: V_SPI under MCU control (OEM value), TX/RX RAM under MCU control, HR_V3000 hook-up value, V_SPI without MCU control, all off. The driver itself exchanges the voice payloads on V_SPI whatever the value |
| BER test | 0x83 = 0xFF, 0x81 = 0xFF, 0x10 = 0x02, 0x40 = 0x40, 0x41 = 0x41 | PHY-layer BER test of manual 5.4.6: 36 raw bytes per frame in RX RAM 0x30, 0x82 bit0 per frame. Bit0 is the "PHY-only" interrupt `dmrMode()` masks with 0x81 = 0xFE, so the stale flags are cleared and the mask opened first; `Resync` or the next modem reconfiguration put 0xFE back |
| Two-point | 0x12 bit6 toggled (read-modify-write) | 40 Hz square wave on MOD1/MOD2 to balance the two-point modulation. A tone left on does not reach FM or M17: `fmMode()` writes 0x12 = 0x00 |
| LCSS mode | no register write: `state.dmr_lcssMode` toggled, pushed to the RTX stage as `rtxStatus_t::dmr_lcssMode` | Convention of the LCSS bits of register 0x50 in voice bursts, applied by the call controller to the next transmission without reprogramming the modem: `50 chip` = 0x08 \| seq << 4 with LCSS 00, the chip filling the embedded LC fragments (OpenGD77, default); `50 manual` = the manual's 5.4.5 sequence 0x08, 0x19, 0x2B, 0x3B, 0x4A, 0x58. The value column names the convention the next ENTER selects; forgotten at power off |
| RF advance | 0x12[5:0] and bit7 = 0x18 or 0x15, alternately (read-modify-write, bit6 preserved); the word without bit6 is also pushed to the RTX stage as `rtxStatus_t::dmr_rfAdvance` | RF_TX_INTER advance: the driver default 0x18 (2.4 ms) or OpenGD77's 0x15 (2.1 ms), both with bit 7 clear, so that the transmit ramp of the modulation DACs starts from the 1.65 V mid level and not from 0 V (manual 4.7.2); the default when the register holds neither. The `Two-point` bit is left as it is in the immediate write, so the two can be combined until the next reconfiguration; the opMode handler reprograms the modem with the chosen word (`dmrbbConfig::rfAdvance`, 6.4) and keeps it through every later reconfiguration until power off |
| SYS line | driver pseudo page 0xFF, `DMRBB_DRV_SYS_LINE` (0x02) = 3 (PC3) or 1 (PC1), alternately; the value column reads `PC1->PC3` / `PC3->PC1` | Which EXTI line the CS7000 driver dispatches as SYS_INTER: PC3_C (the schematic, default on the Plus) or PC1 (default on the CS7000), see 1.2, 3.1 and 6.3. On the Plus PC1 is an analogue pin without interrupt until this action selects it, and goes back to analogue mode when PC3 is selected again. Not a chip register: the choice survives the modem reconfigurations on its own and is forgotten at power off; the emulator and the stub accept the write and ignore it |

Two things to keep in mind:

- The modem is reprogrammed from scratch (`dmrbb_configure()`, table 4.1 of
  the design note) whenever the colour code, timeslot, ID, access policy or
  mode word or RF advance word changes. Ordinary silence no longer does
  it: the timeslot watchdog asks for a reprogramming only when it first
  looks after 2 s without timeslot interrupts in a state that should have
  them, that is when the rtx thread itself was held up that long. `Reg
  0x06` and `Two-point` therefore stay until one of those settings
  changes or DMR is left. What the call controller does rewrite on its
  own is 0x41, at every timeslot interrupt, and the reception sequence
  (0x40 = 0xC3, then 0x41 = 0x20, 0x00, 0x20, 0x50) every time it starts a
  new sync search: after 20 idle slots (600 ms) that no validated event
  confirmed, at the end of a direct mode call, after an own transmission
  and 200 ms after the timeslot interrupts stop. That undoes the 0x40 /
  0x41 of a `BER test` unless the controller is in `RX_SEARCH`, without
  timeslot interrupts, where it writes nothing. `Mode 0x10`, `LCSS mode`,
  `RF advance` (the advance word, not the two-point bit) and `SYS line`
  survive a reprogramming, see above.
- PTT is disabled on every menu screen. For the checks that need
  transmission, run the action, press ESC back to the main screen, key the
  radio (a DMR transmission with silence frames), release, and come back to
  the monitor: the counters and readbacks are kept.

## 3. Hardware checks, in order

Each check names the unknown, what to do, what is expected and what to change
where if it fails. Record the outcome in the table of section 7.

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
  during the call. On a channel without a DMR signal `TS` stands still or
  advances in runs of about 20: the controller drops a time axis that no
  validated burst confirmed for 20 slots and searches again.
- If wrong: the line that counts with the call is not the one `SYS`
  follows: switch the driver with the `SYS line` action of the monitor
  screen (6.3), check that `SYS` now counts, then fix `DMR_SYS_INT` in the
  pinmap of that target. On the Plus `PC1` stays at 0 unless the `SYS
  line` action selected it, and a count seen while it is selected means
  nothing on its own, the pin carries receive audio. Neither line counts
  while `PC0` does: SYS interrupts are masked, see 3.4, or the line was
  not armed (last byte of the `E` trace record, 1.2). `PC0` never counts:
  no sync, see
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
  the driver table. Neither works: 0x40 / 0x41 sequence (3.3) or the RX
  path (3.8).

### 3.3 RX next-slot value 0x41: 0x50, 0x58 with the monitor on any colour code, and the colour code in 0x1F

- Where: `DMR::R41_RX` (0x50) and `DMR::R41_RX_ANY_CC` (0x58) in
  `openrtx/include/protocols/DMR/ModemPort.hpp`, `CallController::rxSlot()`,
  `dmrbb_startRx()` and `dmrbb_setColorCode()` in the driver, the
  `startRx()` / `startTx()` of the modem port in
  `openrtx/include/rtx/OpMode_DMR.hpp`.
- Assumption: with 0x41 bit 3 (`CC_Match_Ctrl`) clear the chip deframes
  only the bursts that carry its local colour code, register 0x1F [7:4]
  (manual chapter 8), before the software filter sees anything. Every
  receive slot is therefore programmed with 0x50 while `Settings > DMR >
  Monitor` is `Off` or `TG`, and with 0x58 (bit 3 set, no colour code
  match needed) while it is `ALL`. Register 0x1F is one register for both
  directions: `dmrbb_configure()` writes the TX colour code, and on a
  channel whose two colour codes differ the modem port writes the RX one
  before every `dmrbb_startRx()` and the TX one before every
  `dmrbb_startTx()`; with equal codes it is left alone. Neither 0x58 nor
  the swap has been seen on a radio; the OEM power-on table writes 0x04,
  bit 2, to 0x41 with the note "No CC match for RX", while the manual puts
  `CC_Match_Ctrl` at bit 3 and describes no bit 2.
- Do: during a call, section 2: watch `0x51` and `SYS`. Then, with the
  other radio on another colour code, set the monitor to `ALL` and back to
  `Off`, and read the `T` records of the trace (0.1).
- Expect: `0x51` cycling through `12` (voice LC header, data sync class
  10), then `11` (voice burst A, voice sync class 01), `20`, `30`, `40`,
  `50`, `60` (bursts B..F, class 00 "no sync"; bit 7 may be set), then `22`
  (terminator); `SYS` counting per burst. With 0x50 the voice payload is
  routed to the vocoder port as well (bit4 "begin_v_layer2"), which stage 4
  needs: whether V_SPI then returns non-zero payloads is a stage-4 check.
  With the monitor on `ALL` the `T` records show 0x41 = `58` on the
  receive slots and the call on the other colour code is shown; with `Off`
  they show `50` and, if the gate works as the manual words it, that call
  raises no SYS event at all.
- If wrong: headers appear (`0x51` = `12`) but no voice events
  follow: try `R41_RX` = 0x40 (blind reception of the manual's minimal
  sequence) in `ModemPort.hpp` and the driver, rebuild; `Resync` after
  changing nothing else re-runs the sync search on the current values.
  Nothing is received with the monitor on `ALL` while `Off` works: the
  chip does not take 0x58, try `R41_NO_CC_MATCH` = 0x04 (the OEM bit). A
  call on another colour code is not shown with `ALL` although `T` reads
  `58`: the gate is not what bit 3 controls, same experiment. Reception on
  a channel with different RX and TX colour codes works only after a
  transmission, or only before one: the 0x1F swap. The monitor screen has
  no `rd 0x1F` row; the last byte of the `R` row 0x10 of the trace is 0x1F
  as it was when the configuration was dumped, the RX colour code in the
  upper nibble while receiving.

### 3.4 Interrupt masks 0x81 / 0x85 / 0x87 polarity

- Where: the 0x83 / 0x81 / 0x85 / 0x87 rows of `dmrMode()` in the driver
  (0xFF / 0xFE / 0x00 / 0x84; FM writes 0x81 = 0x00 "all masked"). 0x85
  and 0x87 are the enables of the TX start and TX end sub-interrupts
  (manual chapter 8): none of the TX start ones, nothing in the firmware
  acting on them, and of the TX end ones only bit 7, "transmission
  completely terminated", on which the call controller releases the RF,
  and bit 2, "Short LC received" (6.1). The per-slot `Rdy_lst_inter` (0x86
  bit 4) stays off: it is one more SYS_INTER pulse in every slot and is not
  needed when 0x41 is programmed at the timeslot interrupt (manual 5.4.4).
  That 1 means enabled is read from the register names and from 0x81.
- Do: sections 1 and 2 during a call and after a transmission.
- Expect: `SYS` counting, `0x82` non-zero during the call; after a
  transmission `0x82` shows bit5 (`20`) with `0x86` = `80`; `0x84` stays
  `00` and bit6 of `0x82` (`40`, TX start) is not seen, their
  sub-interrupts being disabled; `SYS` does not count once per slot while
  transmitting.
- If wrong: `TS` counts but `SYS` never does: 0x81 polarity is inverted,
  write 0x01 instead of 0xFE. `0x82` never shows bit5 with `0x86` = `80`
  at the end of a transmission (the RF is then released by the timeslot
  after the terminator, as when the report comes late), or `SYS` counts
  once per slot during a transmission with `0x86` = `10`: the polarity of
  0x85 / 0x87 is inverted, write 0xFF / 0x7B.

### 3.5 Status registers 0x51 / 0x52 / 0x5F bit meanings

- Where: `R51_*`, `R52_*`, `R5F_*` in `ModemPort.hpp`, their use in
  `openrtx/src/protocols/DMR/CallController.cpp`.
- Do: section 2 during a call on a repeater with a known colour code, then
  on a hotspot in direct mode.
- Expect: `CC` = the repeater's colour code (0x52[7:4]); `TC` alternating
  0/1 with the timeslot (0x52 bit 2 read at a timeslot interrupt is taken
  as the timecode of the slot that just ended, so the local `Timecode`,
  the one of the slot starting, is its complement; see 5.4 for the check
  on a repeater); `Sync` = `BS` on the repeater, `MS` on a hotspot in
  DMO (`TDMA1` / `TDMA2` only from a radio transmitting in DMO); `0x51`
  sequencing as in 3.3 with `CRC` = `ok` on the header and the terminator;
  `Lock` = 2 and `Timecode` matching the slot the call is on; section 3
  shows a `Src` / `Dst` that matches the hotspot's log.
- If wrong: the colour code appears in the low nibble, the sequence never
  reaches `60`, or `CRC` reads `bad` on every header: fix the constants in
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

### 3.7 U_SPI clock: 4 MHz requested, the documented maximum (3.125 MHz on the Plus, 2.625 MHz on the CS7000)

- Where: `audio_init()` in `platform/drivers/audio/audio_CS7000.cpp`
  (`spiStm32_init(&c6000_spi, 4000000, SPI_FLAG_CPHA)`, SPI2 of both
  targets). The manual gives 4 MHz as the maximum SCLK rate of U_SPI; the
  SPI driver rounds the request down to the nearest divider, 3.125 MHz on
  the STM32H743 and 2.625 MHz on the STM32F405. The rate is the one of
  every mode, FM and M17 included.
- Do: section 2 right after selecting DMR: `rd 0x10` must read the mode
  word (`6E`), `rd 0x06` = `21`, `rd 0x12` = the driver's value (`18`).
  Run `Mode 0x10` and `Reg 0x06` and read them back. Press a key in FM
  for a beep.
- Expect: every readback equal to what was written; beeps and the FM and
  M17 audio as before (the OpenMusic DAC refill of
  `platform/drivers/audio/Cx000_dac.cpp`, 64 bytes every 4 ms, takes about
  0.2 ms of clocking at 3.125 MHz).
- If wrong: any mismatch or unstable readback at a rate inside the
  manual's limit is no longer a matter of the clock rate: look at the
  clock phase (`SPI_FLAG_CPHA`) and at the 2 us chip-select guard of
  `HR_Cx000.cpp` first, then halve the request once more to tell. Beeps
  or audio breaking up since the rate was lowered: the refill no longer
  fits its 4 ms period next to the other traffic on SPI2, measure it.

### 3.8 AK2365A IF path into the C6000 ADC

- Where: filter and AGC values in `platform/drivers/baseband/AK2365A.c` and
  `radio_enableRx()` in `platform/drivers/baseband/radio_CS7000.cpp`
  (BPF_6 + NARROW, AGC 0x02 = 0x1E, 0x0B, 0x0C as in FM).
- Do: with a DMR signal generator (or a hotspot at high level) and no sync
  after 3.2: `BER test` (which also unmasks 0x82 bit0 with 0x81 = 0xFF, the
  PHY-only interrupt `dmrMode()` leaves masked), then watch `SYS` and
  `0x82`. The call controller undoes the 0x40 / 0x41 of the test whenever
  it writes them itself: 0x41 at every timeslot interrupt, the whole
  reception sequence when it starts a new sync search (section 2). With
  no timeslot interrupts, in `RX_SEARCH`, it writes nothing and the test
  stays; otherwise read the counters right away, or run the action again.
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

- Where: 0x45 / 0x46 in `radio_updateConfiguration()` (`radio_CS7000.cpp`):
  the same amplitude pair in every mode, 0x45 from the calibration array
  at flash 0x10C4 (`txDigitalPathQ`) and 0x46 from the one at 0x10CC
  (`txAnalogPathI`), the two amplitude arrays `nvm_readCalibData()` loads.
  The other two arrays of `struct CS7000Calib` (`txDigitalPathI`,
  `txAnalogPathQ`) are never read from the flash and hold zeros. The
  ratio between the two points is a property of the board, and this pair
  is the one FM and the M17 4FSK are modulated with; the absolute DMR
  deviation it gives is `UNVERIFIED on hardware`. 0x12 bit 7 is clear in
  the driver's word, 0x18.
- Do: `Two-point` (0x12 bit6 set, `rd 0x12` reads `58` with the driver's
  0x18), ESC to the main screen, key the radio on a dummy load with a
  deviation meter or spectrum analyser, release, run `Two-point` again to
  clear the bit. Then key the radio normally and measure the 4FSK deviation.
- Expect: a 40 Hz square wave with flat top on both edges (MOD1 and MOD2
  balanced); symbol deviation +/- 1.944 kHz (+/- 648 Hz inner symbols) and
  the DMR emission mask; in the trace, `R` row 0x40 with non-zero 0x45
  and 0x46.
- If wrong: tilted square wave: adjust the 0x45 / 0x46 ratio; deviation
  off by a constant factor with a good balance: scale both values for DMR
  in `radio_updateConfiguration()`, there is no other pair in the flash
  image the firmware loads; see 5.2.

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
  0x26, 0x36 and 0x12 restored by `fmMode()` to 0xFE, 0x12 and 0x00; 0x25
  needs no restoring, it holds 0x0E in every mode, the value of the
  `init()` table that `dmrMode()` writes again, and the 0x25 `fmMode()`
  writes is the auxiliary register of that number; 0x06 back to the
  wholesale writes); `TS` / `SYS` unaffected by the beep in DMR; the
  caller shown within 360 ms of the late entry through 0x82 bit4 (`0x82`
  = `10`), which opens a call only with the check flag of 0x51 (bit 2)
  clear.
- If wrong: FM audio dead after DMR: compare `fmMode()` with the DMR table
  for a register it does not restore, and check `rd 0x06` on the monitor
  screen reads `20` or `22` after a beep in FM (the DMR driver was not
  terminated before `fmMode()`, `radio_setOpmode()`). Beeps stop the
  interrupts in DMR: `rd 0x06` reads `22` / `20`, the flag was not set. No
  late entry: the embedded LC assembly is not enabled in the driver (0x82
  bit4 masked) or 3.4; or the `S` records of the trace show `0x82` = `10`
  with bit 2 of 0x51 set and a good LC, in which case the check flag says
  nothing about a late entry and its test in
  `CallController::onSysEvent()` is to be dropped.

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
    0x23, the gating registers 0x26 / 0x25 / 0x36 probably mute LINEOUT in
    DMR mode: the DMR table writes 0xFD (AF output enabled, undocumented,
    the value of `radio_enableAfOutput()`), 0x0E (DAC power switched by
    slot) and 0x00 (every codec and vocoder clock running, the FM receive
    voice channel of bit 1 closed), where FM has 0xFE or 0xFD, 0x0E and
    0x12. The monitor screen has no raw write action (its actions are the
    ones of section 2, `_ui_dmr_monitorAction()` in `ui.c`): add one
    toggling bit1 of 0x36 and one writing 0x25 to that list, as 4.2 does
    for its sweep (or write them with a debugger), then listen. Record the
    combination that works;
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
  up). The values written stay until a DMR setting changes or the mode is
  left (section 2); the DMR table has 0x36 = 0x00, so its bit1 variant is
  0x02. Run the sweep with the call up.
- Expect: silence in every combination (`Aud out` counting, `Aud RX`
  counting, nothing audible but the amplifier hiss).
- If voice is heard in some combination: the chip does contain a decoder,
  or IC611 "HR_V3000S" (I2C, PB8/PB9) is a vocoder companion driven through
  it. Record the combination; the null backend is then bypassed by leaving
  the chip in that configuration and not opening the MCU audio stream (a
  `dmrbb` flag), which becomes the fast path of the design.

### 4.3 V_SPI voice payload logging: the bytes must vary per burst

- Where: `dmrbb_waitEvent()` of `dmr_CS7000.cpp` (V_SPI command 0x83, 27
  bytes into `dmrbbSnapshot::voice`, `voiceValid` on a voice burst: 0x51
  class 00 or 01 with burst number 1 to 6 and the check flag, bit 2,
  clear), register 0x06 = 0x21 (V_SPI), register 0x36 = 0x00 (vocoder
  packet interface clock running; whether that clock serves V_SPI is not
  documented), and the V_SPI transfer function `spiVoc_func()` in
  `hwconfig.c` of both targets: clock idle low, data out changed after
  the rising edge, data in read at the falling edge (manual Figure 4.16),
  2 us between the chip select and the first clock edge, about 500 kHz.
  V_SPI is the only voice transport: RX RAM 0x30 and 0x60 hold the raw
  264-bit burst of each slot, not a 27-byte payload, and the driver has
  no RAM alternative.
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
    or the burst number of 0x51 is misread (3.5), or the check flag of
    0x51 is set on every voice burst (the `S` records of the trace show
    0x51 and the "voice payload read" byte; a flag that says nothing
    about voice bursts on this chip would have to be dropped from the
    driver and from `CallController::voiceOfCall()`). Bit3 of 0x06
    (SPIFrom) is already 0 in 0x21, which the manual gives as "V_SPI out
    = received DMR payload", and every value the `Reg 0x06` action cycles
    (0x21, 0x01, 0x24, 0x20, 0x00) keeps it at 0, so that sweep does not
    touch it; only if a bit3 = 1 experiment is wanted, add 0x29 to the
    `Reg 0x06` action values (`dmrMonR06Values` in `ui_menu.c`).
  - `Aud RX` counting, `Aud vary` at 0 or 1: the same bytes every burst
    (all zeros, or a stale buffer): the V_SPI read is wired to the wrong
    port or clocked wrongly (VOC_* nets, PB3-PB6, C6000 slave), or reads
    the TX buffer. The `V` records of the trace show the bytes: compare
    them with the silence pattern of 0.1 while the other radio sends
    silence. Bytes that are the expected ones shifted by one bit: the
    sampling edge of `spiVoc_func()`. All zeros with a good 0x51: the
    routing of the payload to the port (0x06, bit 4 of 0x41) or the
    packet interface clock (0x36 bit 4 is its shut-off and must read 0 in
    the `R` row 0x30 of the trace).
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
impolite, then 0x40 = 0xE3 direct mode / 0xC3 repeater mode; on a channel
whose two colour codes differ, 0x1F = the TX colour code first) and bursts
go out only from the `0x41 = 0x80` of a later slot tick, with the frame
type in 0x50, the link control in TX RAM 0x00-0x0B (opcode 0x02, one chip
select), the 27 voice bytes on V_SPI (command 0x03, address 0x00) and the
sync word the chip takes from the auxiliary registers 0x04 to 0x09, where
`dmrMode()` puts the MS sourced data sync D5 D7 F7 7F D7 57 (ETSI TS 102
361-1 table 9.2; the reset value is the BS sourced one). The PA stays
keyed for the whole call, from the arming of the modem to one slot after
the terminator (or the wake-up CSBK) was programmed, earlier on the chip's
"service transmission completely terminated" report (0x82 bit5 with 0x86
bit7; the per-slot 0x86 bit4 sub-interrupt is disabled by 0x87 = 0x84 and
would not release it either), or after 35 ms when neither arrives; a
"request rejected" flag (0x82 bit7) does not release it, the transmission
goes on with the `Busy` marker; the falling edge of
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
0x3B / 0x4A / 0x58) and `RF advance` (0x12 = 0x18 or 0x15, since stage 6
kept through the reconfigurations, see 6.4). Two read-only
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
  28 ms (2.4 ms before the next slot boundary with 0x12 = 0x18). On the
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
  the release (`10` or `40` would mean a sub-interrupt that 0x87 = 0x84
  keeps off came through, the mask polarity of 3.4, and got past the
  controller's check) or widen `CallController::RF_UNKEY_MS`
  by a slot. No PC2 pulses: 3.1 (pin mapping) or 0x12 = 0x00 (the
  advance disabled, `rd 0x12` must read `18`). Bursts do not fall on the
  slot boundary
  (visible against a repeater or in a two-slot capture): 0x2E = 0x04 (400
  us internal TX delay compensation) is wrong for this board. The
  adjacent-slot carrier bothers a repeater or fails an emissions check:
  per-slot keying from `DMRBB_EV_RFTX` in the rtx thread (`TX_PWR_EN` on at
  the RF_TX event, off at the next slot tick) is the follow-up, the
  74HC595 update takes tens of microseconds and fits the 2.4 ms lead.

### 5.2 Deviation: two-point balance, then 4FSK on air

- Where: 0x45 / 0x46 in `radio_updateConfiguration()`, the amplitude pair
  FM and M17 use (0x45 from the calibration array at flash 0x10C4, 0x46
  from the one at 0x10CC, see 3.9), the two-point bias 0x47 / 0x48 / 0x04
  from `radio_init()`, the `Two-point` action (0x12 bit 6) of the monitor
  screen. The pair is right for the balance of the two points; the
  absolute deviation it gives in DMR is `UNVERIFIED on hardware`.
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
  monitor screen when the register write row exists, or rebuild). No
  deviation at all, or one point only: read `R` row 0x40 of the trace,
  0x45 and 0x46 must both be non-zero. Deviation off by a constant factor
  with a good balance: scale both values for DMR in
  `radio_updateConfiguration()`; the arrays at flash 0x10BC and 0x10D4,
  which `nvm_readCalibData()` does not load, are where to look in a dump
  of the calibration area for a separate DMR pair. Deviation right but
  BER bad: 0x2E timing or the IQ swap bit of 0x01 (0xB0 keeps the OEM
  value).

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
  the firmware keeps programming its slots whatever the chip answers; if
  the chip flags the request rejected (0x82 bit 7, `0x82` = `80` on the
  monitor screen, `HR_C6000::txRejected()`) the `Busy` marker is shown on
  the main screen and the last byte of the `P` trace records counts the
  flags. Whether the chip raises the flag at all, and whether it then
  keeps the bursts off the air, is what the waterfall and the hotspot log
  tell. Impolite: no flag, the radio transmits regardless.
- If wrong: no header logged at all but RF is seen: the sync word (the
  `A` record of the trace must read `D5D7F77FD757`), deviation (5.2),
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
  by the network (noise on the far end): the V_SPI orientation or its
  clock edges (`spiVoc_func()`, 4.3); V_SPI is the only transport, there
  is no TX RAM alternative. `Busy` never shown over a busy slot: 0x21 =
  0xA2 versus the manual's 0xA3 (3.4 for the 0x82 polarity as well).
  `Busy` shown on a free channel, or a transmission that the chip keeps
  off the air while the slot is free: the chip's verdict is not usable
  on this radio, set `Access` to `Impolite`.

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
- If wrong: TX in the wrong slot: the convention of 0x52 bit 2. Read at a
  timeslot interrupt, the bit is taken as the timecode of the slot that
  just ended, as OpenGD77 and OpenMDUV380 do: the own burst is programmed
  (0x41 = 0x80) at the interrupt where the bit reads the own timeslot (0
  for TS1, 1 for TS2) and goes out in the slot starting at the next one.
  The `T` records of the trace show 0x52 and the 0x41 written at every
  tick. If the repeater sees the bursts in the other timeslot, the bit
  describes the slot starting: drop the complement in the
  `trackTimecode()` call of `CallController::onTimeslot()`. Otherwise the
  slot filter of the controller. The repeater never wakes
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
  360 ms of the header, from the first call after entering DMR on (no LC
  is discarded), and the hang time after the OEM release: the caller kept
  on screen while the controller itself is back in `RX_SEARCH`, a direct
  mode call ending with a new sync search, so that the next transmission
  of the OEM radio, which comes with a time axis of its own, is received
  from its header. With the null
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
| Voice payload transport | V_SPI only, command 0x03 address 0x00, 27 bytes (0x06 = 0x21), clock idle low, data out changed after the rising edge and data in read at the falling edge (manual Figure 4.16), packet interface clock on (0x36 = 0x00) | `dmrbb_writeVoice()`, `spiVoc_func()` in `hwconfig.c` | noise on the far end: the clock edges of `spiVoc_func()`, or the V_SPI pin orientation (pinmap.h) |
| 0x21 arming value | 0xA2 polite / 0x02 impolite (bit 1 clears the encode buffer) | `HR_C6000::dmrTxStart()` | busy never flagged or TX never starts: manual's 0xA3 / plain 0xA0 |
| Transmit sync word | AUX 0x04 to 0x09 = D5 D7 F7 7F D7 57, MS sourced data sync (ETSI TS 102 361-1 table 9.2), from which the chip also derives the voice sync; the reset value is the BS sourced pattern | `dmrMode()`, `dmrTxSync[]` | `A` record of the trace not `D5D7F77FD757`: the auxiliary page write or the block reset before it; right word but no receiver opens: the remaining rows |
| 0x40 arming value | 0xE3 direct / 0xC3 repeater (TxEn + RxEn) | same | manual's 0xA3 (TxEn only) if the chip refuses to transmit with RxEn set |
| RF timing: 0x12 advance, 0x2E delay, PA latency | 0x18 (2.4 ms, bit 7 clear: DAC ramp from the 1.65 V mid level), 0x04 (400 us), PA keyed for the whole call | `dmrMode()`, `radio_enableTx()` | bursts off the slot boundary: 0x2E; adjacent-slot emissions: per-slot keying from `DMRBB_EV_RFTX` |
| 4FSK deviation | the amplitude pair of FM and M17 on 0x45 / 0x46 (calibration arrays at flash 0x10C4 and 0x10CC) gives +/- 1.944 kHz in DMR | `radio_updateConfiguration()` | deviation off by a constant factor: scale the pair for DMR, see 5.2 |
| 0x25 DAC gating | 0x0E, DAC power switched by slot, the value of the init table in every mode, is right for a board whose reference oscillator takes its control voltage, DC-coupled, from the modulation DAC; the manual's note for that case asks for the DAC always on (bit 5 set, bit 3 clear) | `dmrMode()` | DMR carrier off frequency against the FM carrier, or pulled at the start of each burst: measure the control voltage of the VCTCXO in FM receive, DMR receive and DMR transmit, then try the manual's setting from a monitor action |
| 0x22 encoder start (manual step 4, 0x22 = 0x80) | 0x86 at every arming (EncodeStart); `dmrMode()` leaves 0x26, decoder started and encoder stopped, and nothing writes 0x22 when a transmission ends | `HR_C6000::dmrTxStart()`, `dmrMode()` | no voice bits go out: the manual's 0x80 after the header, 0x40 after the terminator; received payloads stop after the first own transmission: write 0x26 again on the return to reception |
| TX flags 0x82 bit 7 / 6 / 5 and sub-status 0x84 / 0x86 bit 7 | rejected / started / ended, voice bit 7. Bit 7 does not stop the transmission: it is counted (`txRejected`, last byte of the `P` trace record) and raises the `Busy` marker; whether the chip raises it at all and whether it then keeps the bursts off the air is not known. Bit 6 and 0x84 are not expected, the TX start sub-interrupts being disabled (0x85 = 0x00) | `dmrbb_waitEvent()`, `CallController::onSysEvent()` | monitor screen `0x82` / `0x84` / `0x86` rows and the `S` records tell |
| 0x86 sub-status bits under 0x82 bit 5 | bit 7 alone ends the transmission; bit 4 (Layer 2 per-slot deadline, Rdy_lst_inter) and the data bits would come while the last burst is on the air and are ignored for the RF release; 0x87 = 0x84 enables bit 7 and bit 2 (Short LC received) only | `CallController::onSysEvent()`, `R86_TX_COMPLETE`, the 0x87 row of `dmrMode()` | terminator cut: a sub-status other than bit 7 turns out to be the real end; no TX end report at all: the polarity of 0x87 (3.4) |
| RF release margin after the last burst | one slot after the terminator / wake-up CSBK was programmed, or 35 ms without a timeslot interrupt, is enough for the PA to see the whole burst (manual 5.4.5 step 12: "after the next or a few 30 ms") | `CallController::unkeyAfterBurst()`, `RF_UNKEY_MS` | terminator cut on the waterfall or the hotspot: one more slot |
| Whole-call PA keying | acceptable for the adjacent-slot emissions of a handheld | `OpMode_DMR::keyRf()` | per-slot keying from `DMRBB_EV_RFTX` (driver follow-up) |
| Wake-up CSBK timing | sent with the chip's own timing, the RF released one slot after it while the modem is put back into passive RX in the same slot (0x40 = 0xC3, 0x41 = 0x20 / 0x50 written with the CSBK on the air) | `CallController` TX_WAKEUP | repeater never sees the CSBK: keep 0x40 = 0xE3 until the RF release |
| Sync search after an own transmission | the controller runs `dmrbb_startRx()` (0x41 bit 5, drop the time axis) at the timeslot interrupt where the terminator goes out and waits in `RX_SEARCH`, where the watchdog is idle; `radio_disableRtx()` / `radio_enableRx()` one slot later touch the RF chain only | `CallController::txSlot()`, `OpMode_DMR::keyRf(false)` | terminator cut although the PA is still keyed: the search drops the axis under the burst, start it at the RF release instead. `TS` not counting after a transmission is expected if the chip stops its slot interrupts while it searches |

What the core side of stage 5 expects to see, all `UNVERIFIED on hardware`
and to be ticked in the table of section 7 together with 5.1-5.5:

- On a scope, `TX_PWR_EN` rises before the first header burst and falls one
  slot after the terminator burst (5.1).
- `RF keyed` on the monitor screen counts 1 per PTT press, `TX bursts`
  equals `TX silent` with the null vocoder backend, `TX under` stays 0.
- A repeater wake-up shows `WAKE` on the destination line, then either
  `TX` or the `Wake-up failed` marker within about 0.8 s (two unanswered
  wake-ups of `T_SyncWu`, 5.4).
- A polite-access PTT over a busy slot goes on like any other: `RF keyed`
  counts, `TX_PWR_EN` rises and the slots are programmed; `Busy` is shown
  if the chip raises its "request rejected" flag, and the last byte of the
  `P` trace records counts the flags (5.3).

## 6. Stage 6: Short LC, last heard and the SYS line knob (`dmr_CS7000.cpp`, `OpMode_DMR.cpp`)

Stage 6 adds the day-to-day layer on top of the call flow: the last-heard
list, contacts and favourites, the private call back, and the timeslot
busy hint of the main screen derived from the Short LC the repeater sends
in the CACH (TS 102 361-1 §7.1.4; the Activity Update of TS 102 361-2
tells which slot is busy and with which hashed talkgroup). On the driver
side the Short LC is a new snapshot field and the SYS_INTER line becomes a
runtime choice. Nothing has been run on a radio: all of it is `UNVERIFIED
on hardware`.

### 6.1 Short LC in the snapshot (`dmrbbSnapshot::shortLc`, RX RAM 0x2B-0x2F)

- Where: `readSysEvent()` in `platform/drivers/baseband/dmr_CS7000.cpp`,
  `HR_C6000::SUB86_SHORT_LC`, the `shortLc[5]` field of
  `openrtx/include/interfaces/dmr_baseband.h`.
- Assumption: the manual (register 0x82 bit 5 description, §5.1.1) lists
  "Short LC receive interrupt" as bit 2 of the end-of-transmission
  sub-status register 0x86, which the 0x87 = 0x84 of `dmrMode()` enables
  together with bit 7. The RAM address is the one of the receive side
  allocation (Table 5.3): "ShortLC 0x2b~0x2f, 36bit ShortLC data of the
  receiving end CACH field, the low 4 bits of 0x2F are unused". The 36
  bits are read as contiguous: 0x2B-0x2D plus bits 7:4 of 0x2E are the 28
  control bits (SLCO and 24 payload bits), bits 3:0 of 0x2E and bits 7:4
  of 0x2F the 8 check bits (CRC-8, Annex B.3.7); the TX side table (5.2)
  has its Short LC elsewhere, at 0x24-0x28, with the check bits in a byte
  of their own. The driver reads the five bytes, in one chip select, when
  0x82 bit 5 and 0x86 bit 2 are both set, right after 0x86 and before the
  status registers; the snapshot keeps the previous bytes otherwise. The
  order of the 36 bits (message first, most significant bit first, then
  the CRC) is an assumption, the manual's worked examples never read them
  back. Nothing says whether the check bits are the CRC as received or
  the chip's own verdict, nor whether the flag fires on a radio that is
  only receiving (it sits among the TX end bits).
- Do: near a repeater with traffic on the other slot (or both), section 1
  of the monitor screen: `0x82` and `0x86`. Then the main screen: the
  `1*` / `2*` busy tags at the left of the top bar, against what the
  repeater's dashboard says about the other slot.
- Expect: `0x82` showing `20` (or `28`, `30` with a control frame) now and
  then during the repeater's activity with `0x86` = `04`; the busy hint
  following the other slot's activity within a second of the dashboard.
- If wrong: bytes that change but fail the CRC-8: the order of the 36
  bits (see above), try the CRC in a byte of its own as on the TX side
  (`DMR::FEC::ShortLc::fromChipBytes()`). Bytes that never change: dump
  the RX RAM around 0x2B on every SYS event in a debug build and look for
  five bytes changing every 120 ms with a CRC-8 that checks. The CRC-8 of
  the host-side decoder is checked against the standard
  poly 0x07 check value and the figure B.6 bit placement
  (`tests/unit/DMR_fec.cpp`), so a systematic failure on the radio points
  at the layout, not at the decoder. `0x82` bit 5 never set while
  receiving: the chip does not report Short LCs on the RX side, or 0x87
  masks it (3.4: `dmrMode()` writes 0x87 = 0x84, bit 2 enabled if 1 means
  enabled); failing
  that, the same RX RAM dump, compared with the repeater's CACH, tells
  whether the chip decodes the Short LC at all.

### 6.2 Last heard on a busy repeater and the call back

- Where: the last-heard ring, the last-heard screen and the contacts /
  favourites entries of the UI (core and UI side, see
  [dmr_mode.md](dmr_mode.md)).
- Do: monitor a busy talkgroup for a few minutes, open the last-heard
  screen: the ring must hold the last eight distinct callers in order of
  arrival, a caller talking twice moving to the top, not duplicated. Then
  select one, ENTER: the destination line shows `PC <id>`, key the radio
  for two seconds and check on the other radio (or the network dashboard)
  that a private call from your ID reached that ID. Power cycle: the
  favourites and the selected destination must be back, the last-heard
  ring empty (RAM only).
- Expect: as described; the hang-time display keeps the last caller on
  the main screen for the configured hang time after the terminator, a
  repeater's own hang-time terminators do not clear it early.
- If wrong: duplicates or a wrong order are a core bug (the ring is
  tested on the host, so a difference on the radio means the IDs reaching
  it differ: check `Src` on the monitor screen); a private call that does
  not arrive: the FLCO (3 = unit to unit) in the LC header, visible in the
  hotspot log as `to <id>`.

### 6.3 The SYS line knob (`DMRBB_DRV_SYS_LINE`)

- Where: `openrtx/include/interfaces/dmr_baseband.h` (`DMRBB_DRV_SYS_LINE`
  = 0x02 on the driver pseudo page 0xFF, values `DMRBB_SYS_LINE_PC1` =
  1 and `DMRBB_SYS_LINE_PC3` = 3), the ISR of `dmr_CS7000.cpp`, the
  `SYS line` action of the monitor screen (section 2), which toggles the
  knob between the two lines and shows the line in force (`PC1->PC3`: PC1
  now, ENTER selects PC3). The host tests `tests/unit/hr_c6000_dmr.cpp`
  (driver, built a second time with the line assignment of the Plus as
  `hr_c6000_dmr_plus_test`) and `tests/unit/ui_dmr.cpp` (action) exercise
  it.
- Assumption: on the CS7000 PC0..PC3 are all attached and counted
  whatever the choice; only the line that raises `DMRBB_EV_SYS` (and the
  0x82 reads behind it) moves. On the Plus the alternative, PC1, is an
  analogue node of the board (`CONFIG_DMR_SYS_INT_ALT_ANALOG` in the
  pinmap): it stays in analogue mode, neither attached nor counted, until
  the knob selects it, becomes a digital input with an interrupt for as
  long as it is selected, and goes back to analogue mode afterwards; PC3
  stays attached and counted meanwhile. The result of every attach is
  kept and read back at `DMRBB_DRV_LINES_ARMED` (0x03 on the same pseudo
  page, bit n = PCn), and only the lines the driver armed are detached.
  The default is the pinmap constant `DMR_SYS_INT` (PC3_C on the
  Plus, PC1 on the CS7000); the alternative is `DMR_SYS_INT_ALT`. Any other value
  is ignored. The knob survives a reconfiguration and is forgotten at
  power off.
- Do: check 3.1 first. If `PC3` counts while `SYS` stays at 0 (the
  pinmap is wrong for that board), `SYS line` on the action page (the
  column reads `PC3->PC1` afterwards), then section 1: `SYS` must now
  count with `PC3`, `0x82` must show the flags of the call (`08` / `10`),
  the caller must appear on the main screen within 360 ms and the audio
  counters of section 4 must move. `SYS line` again to go back to PC1.
  On the Plus the default is already PC3 and `PC1` reads 0: only if `SYS`
  stays at 0 with `PC3` at 0 too during a call, select PC1 and see whether
  `SYS` then follows the call.
- Expect: with the right line selected the whole RX path of stage 3 and
  4 works without a rebuild; with the wrong one `SYS` stays at 0 and no
  caller is ever shown, while `TS` and `PC0` keep counting.
- If wrong: `SYS` counts on neither choice while one of `PC1` / `PC3`
  pulses: the pulse is on a line the knob does not accept (PC0 / PC2
  are TS / RF_TX by the pinmap and the schematic alike); `SYS` counts but
  `0x82` stays `00`: the pulses are not the C6000's (the 2T/5T detector on
  PC1 of the Plus), select the other line.

### 6.4 RF advance word through the configuration (`dmrbbConfig::rfAdvance`)

- Where: `dmrbb_configure()` in `dmr_CS7000.cpp`, the `rfAdvance` field of
  `struct dmrbbConfig` (`openrtx/include/interfaces/dmr_baseband.h`,
  `DMRBB_RF_ADVANCE_DEFAULT` = 0x18).
- Assumption: the `RF advance` action writes 0x12 at once (bit6, the
  two-point tone, preserved) and pushes the advance word (without bit6)
  to the RTX stage as `rtxStatus_t::dmr_rfAdvance`; the opMode handler
  copies it into `bbCfg.rfAdvance` (0 = driver default, mapped to 0x18)
  and reprograms the modem when it changes, as it does for the mode word.
  `dmrbb_configure()` writes the table's 0x18 and then, for a value other
  than 0 and 0x18, rewrites 0x12 right after the mode word, so the choice
  holds through every later reconfiguration (a colour code change, for
  instance) until power off. The two-point bit does not survive a
  reconfiguration (the table clears it), as before; neither word reaches
  FM or M17, `fmMode()` writes 0x12 = 0x00.
- Do: `RF advance` on the monitor screen (`rd 0x12` = `15`), then change
  the colour code and back (two reconfigurations), read `rd 0x12`.
- Expect: `15` after the action and still `15` after the reconfigurations;
  `RF advance` again: `18`, and `18` after another colour code change.
- If wrong: `rd 0x12` reads `18` after the reconfiguration: the value did
  not reach the modem, check `dmrbb_configure()` writes 0x12 after the
  mode table (`tests/unit/hr_c6000_dmr.cpp` checks the order on the host)
  and that the U_SPI readback of 3.7 works.

## 7. Results

Fill one column per radio; leave a cell empty until the check was run.

| # | Check | Expected | CS7000 (F405) observed | CS7000-M17 Plus (H743) observed | Date / who |
|---|---|---|---|---|---|
| 0.2 | Trace capture of the configuration | `A` = D5D7F77FD757; `R` row 0x40 with 0x45 and 0x46 non-zero; last byte of `E` = 0D on the Plus, 0F on the CS7000 | | | |
| 1.1 | PC2_C / PC3_C reach EXTI | PC2 / PC3 count | | | |
| 1.2 | SYS_INTER pin | PC1 or PC3 counts at 16/s | | | |
| 1.3 | EXTI latency behind other handlers | no TS skipped | | | |
| 3.1 | RF_TX_INTER on PC2 | PC2 and RFTX count in TX | | | |
| 3.2 | Mode word | 0x6E / 0x6A gives TS | | | |
| 3.3 | 0x41 RX value, colour code gate | 0x51 cycles 12, 11, 20..60, 22; with the monitor on ALL 0x41 = 58 and a call on another colour code is shown | | | |
| 3.4 | 0x81/0x85/0x87 polarity | SYS counts in RX; 0x82 = 20 with 0x86 = 80 at the end of a TX, 0x84 stays 00 | | | |
| 3.5 | 0x51/0x52/0x5F bits | CC, TC, Sync, CRC as listed | | | |
| 3.6 | 0x14-0x16 byte order | CE CA 23 for ID 2345678 | | | |
| 3.7 | U_SPI readback at the 4 MHz request | rd 0x10/0x06/0x12 as written (6E / 21 / 18), beeps intact | | | |
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
| 5.3 | Busy channel, polite / impolite | transmits in both; with polite, whether the chip flags it (`Busy` marker, count in the `P` record) and whether RF goes out | | | |
| 5.4 | Repeater: BS sync, lock, slot, wake-up, rebroadcast | no TX before Lock 2, right TS, `WAKE` on the destination line then `TX` or `Wake-up failed` within ~0.8 s, repeater wakes, second radio hears | | | |
| 5.5 | OEM interop, both directions | caller shown for the whole call, idle at release, silence not noise | | | |
| 5.6 | TX_PWR_EN edges on the scope | up before the first header, down one slot after the terminator | | | |
| 5.6 | Monitor TX counters | `RF keyed` 1 per PTT, `TX bursts` = `TX silent`, `TX under` 0 | | | |
| 5.6 | Wake-up outcome | `WAKE`, then `TX` or `Wake-up failed` within ~0.8 s | | | |
| 5.6 | Polite PTT on a busy slot | the transmission goes on (`RF keyed` +1), `Busy` if the chip flags the request | | | |
| 6.1 | Short LC flag and RAM | `0x82` bit 5 with `0x86` = 04 during repeater traffic; bytes change, CRC-8 ok | | | |
| 6.1 | TS busy hint | follows the other slot's activity within 1 s | | | |
| 6.2 | Last heard on a busy repeater | eight distinct callers, newest first, no duplicates | | | |
| 6.2 | Call back from last heard | private call reaches the selected ID | | | |
| 6.2 | Contacts / favourites survive power cycle | destination and favourites restored, LC sent matches (hotspot log) | | | |
| 6.2 | Hang-time display | caller kept for the hang time, repeater hang terminators do not clear it | | | |
| 6.3 | SYS line action | `SYS` follows the line selected on the monitor screen, caller shown after the switch | | | |
| 6.4 | RF advance kept through reconfiguration | `rd 0x12` = 15 after a colour code change, 18 after the action is pressed again | | | |
| 6 | Voice prompts at call start | ID / talkgroup announced, RX audio resumes | | | |

## 8. Flashing back to the OEM DMR firmware

Nobody on this series has flashed a radio back, and nothing in this tree
describes the procedure. What is known, from the research notes behind the
series and the sources they cite:

- The Connect Systems "CS7000 M17 PLUS software" page lists, for the
  CS7000-M17 Plus: the OEM bootloader `M17PLUS_Bootloader_B7.06.dfu`, the
  host firmware `Firmware V9.00.96.bin` (dated 20260303; the DMR + analog
  firmware, with CPS V1.02.21.00 and a "DMR Tuner" V2.18), "Open RTX
  V0.4.4", DfuSeDemo and STM32CubeProgrammer. A "Factory Tool_M17_DMR V1.0"
  appears in a search summary of the page only; it has not been seen.
- The Connect Systems application note "Converting DMR to M17 Protocol"
  (July 2024) states that the radio carries either the DMR (with analog)
  or the M17 firmware, never both, and describes the erase and load of
  `openrtx_cs7000.dfu` with DfuSeDemo, i.e. the OpenRTX direction only.
- The `dmr/flashing_dmr.md` page of the M17-Project `cs7000-wiki` ("Flashing
  DMR to the CS7000-M17 with OpenRTX firmware loaded") has Windows, Mac
  and Linux headings and no steps.

What is not documented anywhere the series could find, and is therefore
not written here: whether the OEM bootloader `.dfu` has to be loaded
before the host firmware, which tool loads `Firmware V9.00.96.bin` (a
`.bin`, not a `.dfu`: DfuSeDemo, STM32CubeProgrammer at a given address,
the CPS, or the factory tool), the flash addresses, the DFU entry
sequence for the OEM bootloader, and the files for the original CS7000
(STM32F405; the OpenRTX driver comments cite its OEM firmware V8.01.05 for
the calibration and two-point bias maths, no download was located). Keep
a copy of the OEM files before flashing OpenRTX, and when somebody does
the procedure, write it down here step by step with the tool versions.
