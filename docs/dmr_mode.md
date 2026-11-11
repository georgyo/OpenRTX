# DMR mode

DMR mode makes the radio a DMR Tier II transceiver: the digital mode used by
most commercial handhelds and by the amateur DMR networks, defined by
[ETSI TS 102 361-1](https://www.etsi.org/deliver/etsi_ts/102300_102399/10236101/02.05.01_60/ts_10236101v020501p.pdf)
(air interface) and
[ETSI TS 102 361-2](https://www.etsi.org/deliver/etsi_ts/102300_102399/10236102/02.04.01_60/ts_10236102v020401p.pdf)
(voice and generic services). This document describes what the firmware does
today, what it will do at the end of the series of changes that brings DMR
in, and the rules the series follows.

## Read this first: status of the series

DMR support lands in six stages. **Stages 1 to 4 are in the tree.** With
them, DMR is a mode the user can select and configure, the RTX stage runs
the complete Layer-2 call state machine (`OpMode_DMR` in `rtx.cpp`) against
an abstract modem interface, stage 3 adds what is needed to bring that
interface up on the CS7000 radios: the STM32 external interrupt shim
(`openrtx/include/peripherals/gpio_irq.h`), the HR_C6000 DMR modem driver,
and the DMR monitor screen with its bring-up checklist
([dmr_bringup.md](dmr_bringup.md)), and stage 4 the audio plumbing around a
pluggable vocoder whose in-tree backends are not codecs (see
[Vocoder](#vocoder)): a received call plays silence, the test tone, or on
the emulator a tone, at the right times. On the Linux emulator the modem is
a scriptable fake, so calls are received and transmitted headlessly and a
replayed call is audible. Nothing has been run on a radio: the analog FM
and M17 modes keep working after visiting DMR, and the monitor screen
exists so that the first person with a radio can find out which of the
documented assumptions hold.

| Stage | Title | Status |
|---|---|---|
| 1 | DMR data model, settings, UI mode selection and ETSI protocol library (no RF) | in tree |
| 2 | Call-control state machine, `dmr_baseband` interface, `OpMode_DMR` and the emulator modem model | in tree |
| 3 | HR_C6000 DMR driver, EXTI shim, `radio_CS7000` branches and the DMR monitor screen (RX sync, colour code, slot and LC with caller display, no audio) | in tree |
| 4 | RX voice plumbing: vocoder backend interface, `dmr_audio` thread, PCM to the speaker through the C6000 DAC, and the internal-vocoder experiment | in tree (the vocoder seam, the null / loopback backends, the audio thread and the emulator demo; the internal-vocoder experiment is a bring-up check, see [dmr_bringup.md](dmr_bringup.md)) |
| 5 | TX voice calls: simplex (DMO, active timing) first, then repeater (RMO, passive timing with wake-up) | planned |
| 6 | Contacts and talkgroup UI, last heard, private calls from history, voice prompts and documentation polish | planned |

**Nothing of the DMR series has been verified on a radio.** Every value that
comes from the HR_C6000 manual, the CS7000 schematic or the observation of
other firmwares, but has not been confirmed on a CS7000, is marked
`UNVERIFIED on hardware` in the code and is listed in `docs/dmr_bringup.md`
together with the check that settles it. Stage 1 does not touch the hardware; its only such
assumption is the 12-octet layout of the HR_C6000 link control RAM used by
`FullLC::toChipLc()` and `FullLC::fromChipLc()` (the 9 LC octets followed by
the 3 RS(12,9) parity octets the chip is expected to compute itself), taken
from the chip manual and to be confirmed in stage 3.

## Targets

`CONFIG_DMR` is defined for the Connect Systems CS7000-M17, the CS7000-M17
Plus and the Linux emulator (`platform/targets/*/hwconfig.h`). Both CS7000
radios carry the HR_C6000 baseband, which does the DMR physical layer and
layer 2 (4FSK, sync, CACH/EMB, link control FEC, slot timing). The MD-3x0
family (HR_C5000) does not define `CONFIG_DMR` in this series and keeps
building unchanged; its codeplug channels of DMR type are still displayed.

## Selecting the mode

Macro key 5 (MONI + 5) cycles the VFO through FM, DMR and M17. On entering
DMR the VFO takes the colour code and timeslot from the settings (see below),
because the channel record shares its storage between the FM, DMR and M17
parameters. A memory channel of DMR type keeps the colour codes and timeslot
of the codeplug.

DMR mode and [FRS mode](frs_mode.md) exclude each other in the simplest way:
while FRS mode is on the mode key is refused with the `FRS!` marker, as every
other key that would change the FRS channel plan, and turning FRS mode on
while the VFO is in DMR parks the DMR VFO and restores it, mode included,
when FRS mode is turned off.

## Main screen

On the 160x128 displays the line under the top bar shows the destination on
the left and, in the small font on the right, the channel in the compact form
`C<colour code>T<timeslot>`:

```
TG 31665               C1T2
PC 2345678           C1T2 M
ALL                    C1T2
```

`TG` is a group call to a talkgroup, `PC` a private call to a radio ID, `ALL`
a broadcast; `M` is shown while the monitor setting is not off. During a
received call the frequency gives way to the caller and the destination:

```
  2345678          (caller ID)
  -> TG 31665
  CC1 TS2
```

While transmitting the destination line reads `TX -> TG 31665` and while a
destination is being typed it reads `TG 316_`; in both cases the channel is
left out, since `TX -> PC 16777215` alone fills the line. The 128x64 displays
put the same information on one line, `TG31665 C1T2`, and show the caller on
line 2 with the destination on line 3 during a call.

Keys on the main screens while the VFO is in DMR:

| Key | Function |
|---|---|
| `#` | Talkgroup or private ID entry: digits, ENTER accepts, ESC or `#` cancels, the arrows delete the last digit |
| `*` | Toggles the call type between group and private |
| MONI + 1 | Next colour code (0-15, wraps) |
| MONI + 2 | Other timeslot |
| MONI + 3 | Next monitor level: off, talkgroup, all |
| MONI + 5 | Next mode |

The macro menu shows `CC`, `TS` and `M` in place of the CTCSS rows. A number
entry never goes past 16777215, the largest 24-bit DMR ID; a digit that would
take it past that value is ignored.

## Settings

`Settings > DMR`:

| Item | Values | Stored in | Notes |
|---|---|---|---|
| DMR ID | 0-16777215 | `settings.dmr_id` | Own radio ID. 0 means "unset", shown as `OFF`; a later stage refuses to transmit without an ID |
| Talkgroup | 0-16777215 | `settings.dmr_talkgroup` | Destination: talkgroup for a group call, radio ID for a private call |
| Call type | Group, Private, ALL | `settings.dmr_callType` | |
| Color code | 0-15 | `settings.dmr_colorCode`, `channel.dmr` | Edits the VFO too while it is in DMR |
| Timeslot | 1, 2 | `settings.dmr_timeslot`, `channel.dmr` | Edits the VFO too while it is in DMR |
| Monitor | Off, TG, ALL | `settings.dmr_monitor` | Off: colour code, timeslot and destination must match. TG: own colour code, any talkgroup. ALL: any colour code and timeslot |
| Access | Polite, Impolite | `settings.dmr_polite` | Polite: do not transmit over a call on the own colour code |
| Hang time | 0-7 s | `settings.dmr_hangTime` | How long a finished call stays on screen and keeps the channel; default 3 s, the `T_CallHt` of TS 102 361-2 |
| Test tone | Off, On | `settings.dmr_testTone` | Debug: the null vocoder of stage 4 plays a tone instead of silence on received voice |
| DMR monitor | (ENTER) | - | Opens the DMR monitor screen, see below |

The DMR ID and the talkgroup are typed in: ENTER opens the entry, digits
compose the number, ENTER stores it, ESC leaves the old value, the arrows
delete the last digit. The other items change with the arrows, in edit mode
(ENTER) or directly with LEFT and RIGHT; ENTER on `DMR monitor` opens the
screen.

## DMR monitor screen

`Settings > DMR > DMR monitor` is the bring-up tool of stage 3: a read-out
of what the DMR opMode handler sees on the modem, plus a handful of raw
register writes for the assumptions that only a radio can settle. It is not
needed to operate the radio. Its content comes from the
`rtxStatus_t::dmr_mon` block the opMode handler publishes on every pass
(interrupt counters, per-EXTI-line counters, the registers read in the
timeslot and system passes, the last link control RAM with its decoded
fields) and from `dmrbb_readRegister()` for a few registers read back at
every redraw.

The rows are grouped in five sections of 12, 13, 7, 7 and 5 rows, each
starting on a new page; a section spans as many pages as the display height
needs (six rows per page on 160x128: 2 + 3 + 2 + 2 + 1 = 10 pages; five on
128x64: 3 + 3 + 2 + 2 + 1 = 11 pages), the top bar reading `DMR monitor n/N`:

1. counters: `TS`, `SYS`, `RFTX` interrupts, `PC0`..`PC3` pulses per EXTI
   line, slot lock, timecode, last `0x82` / `0x84` / `0x86`;
2. registers: last `0x51`, `0x52`, `0x5F`, `0x42`, the colour code,
   timecode and sync type decoded from them, RSSI, FSK error, and `0x10`,
   `0x06`, `0x12`, `0x14`-`0x16` read back;
3. link control: the 12 LC RAM octets in hex, source, destination, FLCO, FID
   and the CRC verdict of the last header;
4. audio (stage 4): the vocoder backend name and the `dmr_audio` counters,
   see [Counters and the monitor screen](#counters-and-the-monitor-screen);
5. actions: `Resync` (0x81 = 0xFE, the Layer-2 mask of `dmrMode()`, then
   0x41 = 0x20), `Mode 0x10` (0x6E / 0x6A, also pushed to the RTX stage as
   `rtxStatus_t::dmr_modeReg` so that it survives the modem reconfigurations
   until power off), `Reg 0x06` (cycles 0x21, 0x01, 0x24, 0x20, 0x00),
   `BER test` (0x83 = 0xFF and 0x81 = 0xFF to unmask the PHY-only interrupt
   the test reports on, then 0x10 = 0x02, 0x40 = 0x40, 0x41 = 0x41) and
   `Two-point` (0x12 bit6 toggled); the value column shows the register and
   the value the next ENTER writes. The actions run only while the RTX stage
   is in DMR on a working modem: elsewhere (the screen is reachable from
   `Settings > DMR` in any mode) the column reads `DMR off` and ENTER writes
   nothing, so that FM and M17 are never left with a chip taken out of FM
   mode.

LEFT and RIGHT flip the pages (UP, DOWN and ENTER do too, except on the
action page where UP and DOWN select the action and ENTER runs it), ESC
returns to `Settings > DMR`. PTT is disabled on this screen as on every
menu. What to look for on each page, and what to change if the expectation
fails, is written down check by check in [dmr_bringup.md](dmr_bringup.md).

The destination lives in the settings rather than in the channel, mirroring
the M17 destination, because on the CS7000 radios there is no codeplug yet to
hold contacts; the `contact_index` of a DMR codeplug channel is used only
where `cps_readContact()` is real.

## Persistence

The DMR settings are part of `settings_t` and are saved with the other
settings at power off (`nvm_writeSettingsAndVfo()`). They were appended after
the FRS fields; a settings record written by an older firmware, or one
carrying out-of-range values, is brought back to the defaults field by field
at boot (`state_init()`): ID and talkgroup above 16777215, an unknown call
type, a timeslot other than 1 or 2, a monitor level above 2.

`settings_t` must stay below 255 bytes to fit an EEEP record on the CS7000
(`platform/drivers/NVM/nvmem_CS7000.c`); the unit test pins this and prints
the current size.

## RTX status

`rtxStatus_t` carries the DMR configuration the UI thread pushes on every
synchronisation (`dmr_srcId`, `dmr_dstId`, `dmr_callType`, `dmr_rxColorCode`,
`dmr_txColorCode`, `dmr_timeslot`, `dmr_monitor`, `dmr_polite`, and
`dmr_modeReg`, the mode word override of the monitor screen, 0 for the driver
default), a receive report the DMR opMode handler fills (`dmr_lcOk`,
`dmr_rxSrcId`, `dmr_rxDstId`, `dmr_rxFlco`, `dmr_rxColorCodeSeen`,
`dmr_rxTimeslot`, `dmr_rxSyncType`, `dmr_callState`, `dmr_slotLock`,
`dmr_markers`) and the bring-up block `dmr_mon` (`dmrMonitor_t`, 64 bytes:
interrupt and EXTI line counters, the last 0x82/0x84/0x86 flags,
0x51/0x52/0x5F/0x42, RSSI, FSK error, timecode, the last LC RAM with its
decoded source, destination, FLCO, FID and CRC verdict) and, since stage 4,
the audio layer counters `dmr_audio` (`struct dmrAudioStats` of
`core/dmr_audio.h`, six 32-bit counters, 24 bytes). The report, the block and the counters are
owned by the opMode handler: `rtx_task()` keeps them across the
copy of a new configuration, together with `opStatus`. The call state is one
of `DMR_CALL_IDLE`, `DMR_CALL_RX`, `DMR_CALL_RX_HANG`, `DMR_CALL_TX` and
`DMR_CALL_TX_WAKEUP`.

## Stage 2: call control, `OpMode_DMR` and the emulator modem

### Layers

```
rtx.cpp -> OpMode_DMR (openrtx/src/rtx/OpMode_DMR.cpp)
              |  DMR::CallController (openrtx/src/protocols/DMR/CallController.cpp)
              |  DMR::ModemPort (openrtx/include/protocols/DMR/ModemPort.hpp)
              v
openrtx/include/interfaces/dmr_baseband.h          C ABI towards the modem
   platform/drivers/baseband/dmr_linux.cpp         emulator: scriptable fake modem
   platform/drivers/stubs/dmr_baseband_stub.c      every radio: "not supported"
```

`dmr_baseband.h` is the only thing the state machine needs from a modem: a
static configuration (`dmrbb_configure()`: colour code, timeslot, own ID,
access policy, mode register), the start of reception or transmission, what
to do in the next 30 ms timeslot (`dmrbb_setNextSlot()`, register 0x41 of the
HR_C6000), the type of the next transmitted burst (`dmrbb_setTxFrameType()`,
register 0x50), the link control and voice payload of that burst, and a
blocking `dmrbb_waitEvent()` returning the timeslot and system interrupts
with a snapshot of the registers they refer to; the snapshot also carries
the interrupt counters and, for the bring-up, one pulse counter per EXTI
line PC0..PC3 (`lineCount[]`, refreshed on every wait, timeouts included;
the emulator counts the pulses a test injects with `dmrEmu_pulseLine()`,
the stub reports zero). The raw
`dmrbb_readRegister()` / `dmrbb_writeRegister()` pair serves the monitor
screen only. The register numbers are the
HR_C6000 ones and every value that only the manual or another firmware
vouches for is marked `UNVERIFIED on hardware`, both in the header and in
`ModemPort.hpp` where the state machine's constants live in one place.

### State machine

`DMR::CallController` is a pure state machine: no clock, no sleeping, no
hardware, driven by `onTimeslot()`, `onSysEvent()`, `onTimeout()` and
`setPtt()` with the time passed in, producing register writes on a
`ModemPort` and a `Report` for the UI. Its states:

| State | Meaning |
|---|---|
| `OFF` | Disabled |
| `RX_SEARCH` | Reception started, no timeslot interrupt yet |
| `RX_IDLE` | Slots ticking, no call; the timecode is tracked from the CACH (locked after 5 agreeing slots, re-aligned after 4 disagreeing ones) |
| `RX_CALL` | A call passed the colour code, timeslot and address filters (monitor level 0: all three; 1: own colour code and slot, any talkgroup; 2: anything): voice bursts are forwarded, the caller is published |
| `RX_HANG` | The call ended (terminator or data burst of the call's colour code, two silent superframes or an abnormal exit): kept on screen for the hang time, extended only by the hang time terminators of that call (same colour code, same source and destination, TS 102 361-1 §5.2.1.4), reopened by a new header |
| `TX_ARM` | PTT accepted: direct mode with the chip's own timing on a simplex channel without a base station carrier, repeater mode aligned to the received sync otherwise (waiting for the timecode lock) |
| `TX_WAKEUP` | Repeater channel without carrier: a `BS_Dwn_Act` CSBK is sent and the repeater's sync awaited for T_SyncWu, at most N_Wakeup = 2 times |
| `TX_HEADER` | Three voice LC headers on the own slot, the other slot idle |
| `TX_VOICE` | Voice bursts A to F, the payload asked from the audio layer (silence in this stage); PTT release or T_TO completes the superframe, and at least one superframe is sent even when the PTT is released during the headers (TS 102 361-2 §5.2.1.1: the EOT is the entire last superframe through F, then the terminator) |
| `TX_TERM` | Terminator with LC, then back to reception with the hang timer running |

PTT is refused, with a marker, without a DMR ID; a rejected (channel busy),
timed out or failed transmission needs the PTT released before a new press
is accepted. A timeslot watchdog re-runs the reception sequence after 200 ms
without timeslot interrupts and asks `OpMode_DMR` for a full modem
reconfiguration after 2 s. It runs once timeslots have been seen, or from
the moment a transmission asks the modem for its own timing: the chip gives
timeslot interrupts only after a time axis is established (HR_C6000 manual
§5.4.4), so a silent channel in `RX_SEARCH` is left alone and the markers
survive; likewise while waiting for a repeater's answer to a wake-up, where
T_SyncWu bounds the wait on its own (evaluated on timeouts as well, since
the silent channel gives no timeslot interrupt). Timeslot interrupts merged
into one event by a late poll of the rtx thread (the CS7000 driver polls
its interrupt mailbox every millisecond, the handlers only latch) are
recovered from the ISR counter and timestamp carried by the snapshot, so
the slot parity of a call or transmission does not flip.

`OpMode_DMR` owns the controller, an adapter from `ModemPort` to
`dmr_baseband.h` and an audio placeholder (`DmrAudioStub`, silence on TX,
received payloads counted and dropped: the vocoder seam of stage 4). It
reads the PTT with `txDisable` honoured on every pass, requests the speaker
path (`SOURCE_MCU -> SINK_SPK`, priority RX) during a received call and its
hang time and the microphone path (`SOURCE_MIC -> SINK_MCU`, priority TX)
during a transmission, keys the RF stage through `radio_enableRx()` /
`radio_enableTx()`, drives the LEDs (green: call received, red: transmit),
and publishes the report into the `dmr_rx*`, `dmr_callState`, `dmr_slotLock`
and `dmr_markers` fields of `rtxStatus_t`. A change of colour code,
timeslot, own ID or access policy reprograms the modem (`dmrbb_configure()`)
and restarts the controller, deferred until the end of an ongoing
transmission; a change of destination, call type or monitor level only
updates the controller. A split channel (TX frequency different from RX) is
treated as a repeater channel.

### Top-bar markers

The top bar shows, in the place and look of the FRS marker, one of:

| Marker | Raised when |
|---|---|
| `No DMR ID` | PTT pressed with `Settings > DMR > DMR ID` unset. Registering an ID with RadioID is required by the networks |
| `Busy` | The modem rejected the transmission because the channel is busy (polite access) |
| `Wake-up failed` | A repeater channel did not answer the wake-up after N_Wakeup attempts |
| `DMR not supported` | The radio has no DMR modem driver: the mode stays off |

The first three go away at the next PTT press or after three seconds; the
last one stays as long as DMR is selected.

### On the radios: the stub

`platform/drivers/stubs/dmr_baseband_stub.c` is linked for the CS7000, the
CS7000-M17 Plus (until the HR_C6000 driver of stage 3 replaces it) and the
MD-3x0 (which does not define `CONFIG_DMR` and never calls it). Its
`dmrbb_init()` returns -1, so on a radio `OpMode_DMR::enable()` leaves the
mode off, `rtxStatus_t::opStatus` stays `OFF`, the `DMR not supported` marker
is shown, PTT does nothing and every other function is a no-op; the RF stage
is never keyed. FM and M17 are unaffected by a visit to DMR.

### On the Linux emulator: the fake modem

`platform/drivers/baseband/dmr_linux.cpp` implements `dmr_baseband.h` as a
scriptable fake. In the running emulator a thread ticks every 30 ms,
producing idle timeslot events (with the CACH timecode alternating) while
reception or transmission is on, and plays scripted calls at the slot
cadence. A synthetic group call can be replayed:

```
openrtx_linux --dmr-call 2345678,31665        # or OPENRTX_DMR_CALL=2345678,31665
```

then switch the VFO to DMR (MONI + 5): about two seconds later the main
screen shows the caller `2345678` calling `TG 31665` on the configured
colour code and timeslot, for three seconds of voice bursts, then the hang
time; the next call starts nine seconds after the previous one ended (about
twelve seconds between call starts). From the emulator shell,
`dmrcall 2345678 31665` starts the replay and `dmrcall` stops it. Pressing
`p` (or `ptt` in the shell) transmits: the console prints the modem calls
(`dmr_linux: startTx(active timing) called`, ...).

In the unit tests no thread runs: `dmrEmu_pushTs()`, `dmrEmu_pushSys()` and
`dmrEmu_injectGroupCall()` (declared in
`platform/targets/linux/emulator/emulator.h`) queue events that
`dmrbb_waitEvent()` returns at once, one timeslot per call, or
`DMRBB_EV_TIMEOUT` when nothing is queued; `dmrEmu_record()` gives the
record of every write (last 0x40/0x41/0x50 values, LC and voice payloads,
counts of headers, voice bursts, terminators and driver calls);
`dmrEmu_setInitResult(-1)` emulates a radio without a modem.

### Tests

- `dmr_callctrl_test` (`tests/unit/DMR_callctrl.cpp`): the controller
  against a recording mock port, register value by register value,
  including the hang time refresh filter, the early PTT release (one
  superframe of silence before the terminator), a missed timeslot
  interrupt during a transmission, and the watchdog on a silent channel.
- `dmr_opmode_test` (`tests/unit/DMR_opmode.cpp`): `OpMode_DMR` on the
  fake modem: enable to `RX_IDLE`, PTT with `txDisable`, PTT without an ID
  (marker), a transmission (three headers, twelve voice bursts of silence,
  terminator, back to RX), an injected group call (published in
  `rtxStatus_t`, `rxSquelchOpen()`, hang time), colour code filtering and
  the monitor level, modem reconfiguration on a parameter change and its
  deferral during a transmission, the speaker and microphone audio paths
  (requested for a call and its hang time / for a transmission, released
  after, and by `disable()` mid-call), the "not supported" path, and the
  publication of the bring-up block (counters, registers, decoded LC, CRC
  verdict, the mode word override, the counters through the timeout
  passes). It prints the sizes: `OpMode_DMR` is 440 bytes as a static
  object (344 in stage 2, plus the 64 byte `dmrMonitor_t` block and the
  larger snapshot of stage 3, the voice sink with its counters, the two
  audio path IDs and the flags of stage 4), `DMR::CallController` 176,
  `struct dmrbbSnapshot` 88 (72 plus the four `lineCount[]` counters).
- Stack: the handler is a static object, so only the frames of `update()`
  and what it calls sit on the 512 byte rtx thread stack. Measured with
  `-fstack-usage` on the Cortex-M7 build (`arm-miosix-eabi-g++ -Os`,
  target `openrtx_cs7000p`): `rtx_task()` 48 bytes, `update()` 32,
  `applyConfig()` 64 (a `CallController::Config` and a `dmrbbConfig`),
  `onSysEvent()` 24, `processControlFrame()` 24, `tryOpenCall()` 56 and
  `terminatorOfCall()` 40 (a `DMR::FullLC`, 20 bytes, plus the call),
  `buildTxLc()` 32, `onTimeslot()` 16, `txSlot()` 24. The deepest chain,
  `rtx_task -> update -> onSysEvent -> processControlFrame -> tryOpenCall`,
  is about 184 bytes; the configuration chain `update -> applyConfig ->
  enable` about 120. The stub `dmrbb_waitEvent()` of the radios adds
  nothing today; the HR_C6000 driver of stage 3 will add its SPI buffers
  there and must be re-measured. A runtime audit with `memory_profiling`
  (`getAbsoluteFreeStack()` of the rtx thread with DMR active) is still
  pending: the emulator cannot provide it (the functions return 0 outside
  Miosix), it is part of the stage 3 hardware bring-up.
- `ui_dmr_test`: also the marker text selection and the monitor screen
  (opening from `Settings > DMR`, the page walk on both key sets, the
  rendering of every row from a scripted call on the fake modem, and each
  action's register writes as recorded by `dmrEmu_record()->regLog`).

## Stage 4: RX voice plumbing, the `dmr_audio` thread and the emulator demo

Stage 4 closes the receive audio path from the modem to the speaker, and
the transmit path from the microphone to the modem, around the vocoder seam
of the [Vocoder](#vocoder) section. Nothing here decodes AMBE: with the null
backend a received call plays silence (or the test tone) at the right
times, with the loopback backend the emulator plays a tone.

### The receive path

```
HR_C6000 -> V_SPI 27 bytes -> dmrbbSnapshot::voice -> CallController
   -> DmrAudioLink::rxVoiceFrame() -> dmrAudio_pushFrame() [queue of 3]
   -> DMR audio thread: dmrVocoder_get()->decode() -> 480 samples
   -> 8 kHz output stream on SINK_SPK (audio_stream.c, double buffered,
      one 60 ms block per half)
   -> Cx000_dac.cpp "OpenMusic" (U_SPI SOUND page, 32 samples every 4 ms,
      register 0x06 bit1 read-modify-write) -> C6000 DAC -> LINEOUT
   -> TC75S51F -> volume -> TDA2822G -> speaker
```

`OpMode_DMR` requests `SOURCE_MCU -> SINK_SPK` at `PRIO_RX` when the call
controller opens a call (`DMR_CALL_RX`), keeps the path through the hang
time (the amplifier stays on for the answer) and releases it when the hang
time ends, as in stage 2. New in stage 4: as soon as the path is granted it
calls `dmrAudio_startRx()`, which opens the output stream on the sink of
the path and starts the audio thread; every voice burst the controller
hands to the `VoiceSink` (`DmrAudioLink` in `OpMode_DMR.hpp`) goes to
`dmrAudio_pushFrame()`. The thread pops one payload per 60 ms period of the
stream, decodes it into the idle half of the double buffer and syncs; when
no payload is queued it writes silence. The result of the sync is not a
reason to quit, as in the M17 decoder: the Cx000 DAC driver answers false
when the sync point had already passed on entry (the thread came one
period late), a glitch to play through, and the thread only leaves when
the path is no longer open or the stream is gone (no idle buffer). When
the call ends (terminator or voice timeout, `DMR_CALL_RX_HANG`) the thread
is stopped and the stream released while the path stays open; a call
opening during the hang time starts it again. A reprogramming of the modem
(`OpMode_DMR::reprogramModem()`: a colour code, timeslot, ID or access
policy change, or the two-second watchdog) and `disable()` stop the thread
before the driver rewrites the chip.

The audio path manager can suspend the RX path under a higher priority one
on the same sink, typically a voice prompt (`PRIO_PROMPT`) or a beep: the
stream layer then terminates the stream, the thread finds the path no
longer `PATH_OPEN` at its next period and quits on its own. Payloads pushed
meanwhile are dropped (`dmrAudio_pushFrame()` returns false, the opMode
counts them as received but not queued). When the prompt releases the sink
the RX path is open again and the next `update()` pass restarts the thread;
the old one is reclaimed by that start, no thread is ever detached. A start
that fails on an open path (no output device on that sink, as the emulator
has no microphone device for the transmit side) is not retried until the
path is requested anew.

### The transmit side

The microphone path (`SOURCE_MIC -> SINK_MCU`, `PRIO_TX`) is requested for
the whole transmission as in stage 2, wake-up included, so that the
microphone is up before it is needed; stage 4 adds `dmrAudio_startTx()` on
it once the controller reaches `TX_HEADER` (not during a repeater wake-up,
whose hundreds of milliseconds would only fill the queue and drop payloads
as overruns): an 8 kHz input stream on the source (with the
`CONFIG_MIC_OVERSAMPLE` decimation, DC block and `CONFIG_MIC_GAIN` of the
M17 encoder), the thread encodes each 480-sample block through the backend
and queues the payload; `DmrAudioLink::txVoiceFrameNeeded()` pops one per
own voice burst and the controller writes it to the modem, or the AMBE+2
silence pattern when the queue is empty. When the controller enters
`TX_VOICE` (on the last header, one own slot before the first voice burst)
`followState()` calls `DmrAudioLink::startVoice()` -> `dmrAudio_flushTx()`
once, which drops what the three headers left in the queue but the newest
payload: the first voice burst carries the last 60 ms of microphone audio
rather than what was said while the headers went out, and the queue runs
at about one payload for the rest of the transmission instead of sitting
full (three payloads, 180 ms of extra mouth-to-ear delay, no room for
jitter). With the null backend every payload is the silence pattern, so
the air interface carries a well-formed call with silence; keying the PA
for it is stage 5. On the emulator there is no microphone device, the
start is refused and silence is sent.

### The `dmr_audio` module

`openrtx/include/core/dmr_audio.h` / `openrtx/src/core/dmr_audio.c`, a C
module modelled on `audio_codec.c`:

| Function | Role |
|---|---|
| `dmrAudio_init()` / `dmrAudio_terminate()` | reference counted; the first init calls the backend's `init()` and clears the counters, the last terminate stops the thread and calls `terminate()`. `OpMode_DMR::enable()` / `disable()` pair them |
| `dmrAudio_startRx(path)` / `dmrAudio_startTx(path)` | open the stream on the sink / source of an open path and start the thread; true at once when already running on that path; another path takes over only with a higher priority (the codec's rule) |
| `dmrAudio_stop()` | stop the thread, release its stream, join. Idempotent and safe from two threads at once |
| `dmrAudio_running()` | true while the thread runs; false once it quit on its own (path suspended, stream error) |
| `dmrAudio_pushFrame(ambe)` | RX: queue a payload, never blocks; on a full queue the oldest is dropped and counted |
| `dmrAudio_popFrame(ambe)` | TX: take the next encoded payload, false when empty or not encoding |
| `dmrAudio_flushTx()` | TX: drop every queued payload but the newest one, not counted as overruns; called once when the voice bursts of a transmission begin |
| `dmrAudio_getStats()` | copy of `struct dmrAudioStats` |
| `dmrAudio_backendName()` | `dmrVocoder_get()->name` |

The queue holds `DMR_AUDIO_QUEUE_DEPTH` = 3 payloads (the one being played
plus two bursts of jitter) in either direction, one direction at a time.
Start and stop transitions are serialised by a mutex held until the new
thread exists, so that a stop from the UI thread cannot slip between the
stop of the old thread and the start of the new one, and the join is
exclusive (Miosix fails a second concurrent join at once). The thread has
`DMR_AUDIO_THREAD_STKSIZE` = 4096 bytes of stack (`core/threads.h`), sized
for the in-tree backends: the output buffer is static, the oversampled
input buffer is heap allocated for the life of the thread, the backends
keep a payload and the DSP state on the stack. An out-of-tree backend
documents its own figure.

### Counters and the monitor screen

`struct dmrAudioStats` is published on every pass into
`rtxStatus_t::dmr_audio` (kept across configuration copies like the
receive report) and shown by a fifth section of the DMR monitor screen,
"Audio", between the link control and the actions:

| Row | Field | Meaning |
|---|---|---|
| `Vocoder` | `dmrAudio_backendName()` | `null`, `loopback`, or the name an external backend gives |
| `Aud RX` | `framesRx` | payloads handed to the audio layer while playback was running, dropped ones included |
| `Aud vary` | `varying` | of which differed from the previous one: stays at 0 when the modem hands over the same bytes at every burst, what a dead V_SPI read looks like; follows `Aud RX` with real voice |
| `Aud out` | `blocksOut` | 480-sample blocks written to the speaker stream, silence included |
| `Aud TX` | `framesTx` | payloads encoded from the microphone |
| `Underrun` | `underruns` | silent blocks played between two payloads of the same reception (a lost or late burst); the silence before the first payload and after the last one is not counted |
| `Overrun` | `overruns` | payloads dropped from a full queue, either direction |

The section moves the actions to the last page as before; the screen has
10 pages on the 160x128 displays and 11 on 128x64.

### The emulator demo

With the default `dmr_vocoder=auto`, the Linux emulator links the loopback
backend and `dmr_linux.cpp` fills the voice bursts of a replayed call with
`dmrVocoderLoopback_tonePayload()`: 1 kHz on bursts A..C and 700 Hz on
D..F at about -12 dBFS, the level stepping down by an inaudible notch from
A to F so that every burst differs from the previous one. So

```
openrtx_linux --dmr-call 2345678,9      # a call to the default talkgroup
```

then MONI + 5 to switch to DMR: two seconds later the call is on the main
screen and audible through SDL as a 180 ms alternation of the two tones for
three seconds, every twelve seconds. Two seconds after each replayed call
the fake modem prints what the audio layer did with it:

```
dmr_linux: audio loopback: rx 50, tx 0, blocks out 62, underruns 7, overruns 0, varying 50
```

which is also the headless check of the path: with
`SDL_AUDIODRIVER=dummy SDL_VIDEODRIVER=dummy` and the shell commands
`sleep 2500`, `keycombo MONI 5`, `sleep 14000`, `quit` on stdin, the 50
payloads reach the thread, every one differs from the previous, and one
480-sample block per burst goes out (`blocks out` also counts the silence
before the first burst and the gaps). A few underruns per call are normal
on the emulator: the fake modem's 30 ms clock thread (`usleep`) and SDL's
audio clock are not locked and drift apart over a call, whereas on a radio
the slot timing and the DAC clock both come from the HR_C6000. The call is
filtered like any other, so it must be addressed to the configured
talkgroup (or `Settings > DMR > Monitor` set to accept it).

### Tests

- `dmr_audio_test` (`tests/unit/dmr_audio.cpp`, in the style of
  `audio_threading_test`): the module with a fake audio driver paced by
  the test, whose `sync()` blocks until the test grants a period and which
  records every block written to the speaker, and a fake vocoder backend
  (decode fills a pattern carrying the payload's first byte, encode
  recovers the block index from the level of a square wave the fake
  microphone delivers). Covered: init/terminate reference counting;
  `startRx` then ten payloads played in order, 480 samples each; a missing
  payload played as 480 zeros and counted once voice resumes, but not
  before the first payload nor after the last; a late sync point (the
  fake driver answering -1 once, as the Cx000 DAC does) played through
  without stopping the stream; a full queue dropping the oldest payload;
  `stop` releasing the stream and joining; a path suspended by a prompt
  making the thread quit and a restart resuming playback; `startTx`
  pulling 480-sample blocks with `popFrame` returning the encoded payloads
  in order and the queue overflow; `flushTx` keeping the newest payload
  only, not counting it as an overrun and leaving a receive queue alone; a
  start refused on a sink without a device; the TX path taking over the RX one and not the
  reverse; two concurrent stops; a hundred start/stop rounds, half of them
  with the thread parked in the driver. `pthread_join()` is wrapped as in
  the audio threading test; the sanitizer build runs it too.
- `dmr_opmode_test`, extended: SDL's dummy audio device is initialised so
  that the emulator's `SINK_SPK` stream really opens. An injected call
  starts the thread on the granted `SOURCE_MCU -> SINK_SPK` path, blocks
  flow while the call is up, every voice burst is pushed and counted, the
  thread stops at the hang time and restarts for a call during it, the
  path is released after; a transmission with no microphone device is
  refused and sends the silence pattern; the encoder queue is flushed once
  per transmission, when the controller enters `TX_VOICE` and before the
  first voice burst is requested; a voice prompt suspends the RX path, the
  thread quits, the bursts meanwhile are lost, and playback resumes after
  the prompt.
- `ui_dmr_test`, extended: the audio section's rows, names and values,
  the `Vocoder` row showing the backend the build links (`loopback` with
  `CONFIG_DMR_VOCODER_LOOPBACK`, `null` otherwise, never `none`).

## Licensing

OpenRTX is GPL-3.0-or-later and the DMR series keeps it that way:

- The protocol code (`openrtx/src/protocols/DMR`) is written clean-room from
  the ETSI specifications, with clause references in the comments. Nothing is
  copied from OpenGD77, md380tools, MMDVMHost or dmrlib, which are used as
  documentation only. Where a single constant table is taken from MMDVMHost
  (GPL-2.0-or-later), the file says so.
- **No AMBE or AMBE+2 vocoder code, of any origin, enters the tree.** The
  HR_C6000 does not contain a vocoder: the chip does 4FSK, sync, CACH, EMB,
  link control, FEC and slot timing, and exchanges opaque 27-byte AMBE+2
  voice payloads with an external vocoder over its V_SPI port, which on the
  CS7000 boards is wired to the STM32 (HR_C6000 user manual section 4.6,
  CS7000-M17 Plus schematic; OpenRTX's own CS7000 initialisation already
  writes register 0x06 = 0x21, "V_SPI vocoder, MCU controls the vocoder").
  The OEM firmware runs AMBE+2 in software on the MCU, which a GPL tree cannot
  do: AMBE+2 is covered by DVSI's US patent 8,359,197 until 2028, mbelib is
  decode-only and carries a patent notice, the OEM blobs are proprietary and
  upstream OpenRTX declined AMBE code in the past (issue #33). The reasoning
  is laid out in the feasibility study of the series.
- The vocoder is therefore a pluggable backend (`core/dmr_vocoder.h`, see
  the [Vocoder](#vocoder) section). The tree ships a null backend (silence
  on TX, silence or a debug test tone on RX, enabled by the `dmr_testTone`
  setting) and an emulator loopback backend, so that the slot machinery, the
  V_SPI transfers, the audio paths and the interoperation with repeaters and
  hotspots are exercised end to end. The `dmr_vocoder` Meson option lets
  whoever holds a licence link an out-of-tree backend. Whether the chip can
  decode voice by itself after all is the first experiment of stage 4; the
  documentation says it cannot.

Until a vocoder backend is available, a DMR call carries no audible voice:
the radio shows the caller, the talkgroup, the colour code and the slot, and
transmits headers, terminators and silence frames.

## Vocoder

### What is shipped, and what is not

A DMR voice burst carries 216 bits of voice: three 72-bit AMBE+2 frames of
20 ms each (ETSI TS 102 361-1 §6.1, Figure 6.2; TS 102 361-2 §5.2.1 names
the codec). The HR_C6000 does not contain the codec. Its user manual (§4.6,
"Vocoder interface") describes a V_SPI port on which the chip hands the 27
bytes of every received voice burst to an external vocoder and takes the 27
bytes of every burst to transmit from it, and on the CS7000 boards that
port is wired to the STM32 (CS7000-M17 Plus schematic V01, nets VOC_*):
the OEM firmware runs AMBE+2 in software on the MCU, as the TYT and
Radioddity firmwares do on their HR_C5000/HR_C6000 radios. OpenRTX's own
CS7000 initialisation writes register 0x06 = 0x21, "V_SPI vocoder, MCU
controls the vocoder", since before this series.

OpenRTX cannot ship that codec:

- AMBE+2 is covered by DVSI's US patent 8,359,197 B2 ("Half-rate vocoder",
  <https://patents.google.com/patent/US8359197B2/en>), in force until
  2028-05-20; DVSI licenses it as a closed library or in the AMBE-3000 chip
  (<https://www.dvsinc.com/soft_products/ambe_p2.shtml>);
- `mbelib` (<https://github.com/szechyjs/mbelib>) is decode-only and its
  COPYRIGHT file warns that the algorithm is patented and that using it may
  require a licence;
- the OEM firmware blobs are proprietary;
- upstream OpenRTX declined AMBE code in the past
  (<https://github.com/OpenRTX/OpenRTX/issues/33>) and a GPL-3.0-or-later
  tree cannot carry a patent-encumbered implementation of any origin.

So **no AMBE or AMBE+2 code, table or blob is in this tree, and none will
be accepted.** What the tree has is the seam and two backends that are not
codecs:

| Backend | File | What it does |
|---|---|---|
| `null` | `openrtx/src/core/dmr_vocoder_null.c` | `decode()` fills 60 ms of silence, or a 1 kHz sine at -12 dBFS (phase continuous from one burst to the next) when `Settings > DMR > Test tone` is on; `encode()` fills the AMBE+2 silence pattern. Built for every target with `CONFIG_DMR` |
| `loopback` | `openrtx/src/core/dmr_vocoder_loopback.c` | `encode()` packs a coarse descriptor of the 480 samples into the 27 bytes (signature `LB`, RMS level, dominant frequency from the zero crossings, a 20-point peak envelope, XOR check); `decode()` plays a sine back from it, shaped by the envelope and scaled to the level. A payload without the signature (real AMBE, the silence pattern, zeros) decodes to silence. Built for the Linux emulator, so that the RX/TX plumbing is testable end to end and audible through SDL; interoperates with nothing |

The silence pattern itself (`openrtx/src/protocols/DMR/Silence.c`,
`dmr_ambeSilence`, also reachable as `DMR::AMBE_SILENCE`) is the one
constant table taken from a third party, MMDVMHost `DMRDefines.h`
(GPL-2.0-or-later, attributed in the file): 27 bytes of already encoded
silence, not codec code.

### The interface

`openrtx/include/core/dmr_vocoder.h` is a C ABI so that a backend can be
written in C or C++ and linked from outside the tree:

```c
struct dmrVocoder {
    const char *name;
    int  (*init)(void);
    void (*terminate)(void);
    bool (*decode)(const uint8_t ambe[27], int16_t pcm[480]);
    bool (*encode)(const int16_t pcm[480], uint8_t ambe[27]);
};

const struct dmrVocoder *dmrVocoder_get(void);
```

One payload of 27 bytes is three AMBE+2 frames, 60 ms, 480 samples at
8 kHz. `decode()` and `encode()` always fill their output buffer: the return
value says whether it holds audio (`decode()`) or encoded voice (`encode()`),
`false` meaning the buffer holds silence, respectively the AMBE+2 silence
pattern, because the backend had nothing better (test tone off, bad
signature, corrupted frame, no encoder). The caller may then substitute its
own silence or skip the playback. `init()` runs once before the first
frame and `terminate()` once after the last; the two codec functions are
called from the DMR audio thread only and are not reentrant.

### Selecting a backend: the `dmr_vocoder` Meson option

```
meson setup -Ddmr_vocoder=<auto|null|loopback|external> <build dir>
```

| Value | Radios | Linux emulator |
|---|---|---|
| `auto` (default) | `null` | `loopback`: an injected call (`--dmr-call`) is audible as a tone |
| `null` | `null` | `null` |
| `loopback` | `null` compiled, `loopback` selected (a bench test of the V_SPI transfers between two OpenRTX radios, nothing else) | `loopback` |
| `external` | neither backend compiled: the out-of-tree object provides `dmrVocoder_get()` | `null` (the setting is a firmware one; the emulator and the host tests have no out-of-tree object to link) |

The selection is a link-time one: `dmrVocoder_get()` is defined by
`dmr_vocoder_null.c`, or by `dmr_vocoder_loopback.c` when
`CONFIG_DMR_VOCODER_LOOPBACK` is defined, and by nobody in the firmware
with `external`. The null backend is still compiled next to the loopback
one (the loopback `decode()` has no use for silence frames, the audio layer
does) and both structs, `dmrVocoder_null` and `dmrVocoder_loopback`, are
reachable by name for the tests whatever the option. The T-TWR Plus (Zephyr, `CMakeLists.txt`)
compiles `dmr_vocoder_null.c`, which is empty there because the board has no
`CONFIG_DMR`.

### Linking an external backend

With `-Ddmr_vocoder=external` the firmware references `dmrVocoder_get()` and
nothing defines it: the link of the radio targets fails until an object
providing it is added, which is the point. The Linux target is not
affected: `openrtx_linux` and the unit tests are built with the null
backend under `external`, as under `null`, so that `meson test` on the host
(and the emulator's `doCheck` of the flake) keep working from the same
configuration; nothing in the tree links an out-of-tree object on the
host. Whoever holds a licence for a real vocoder (DVSI's
library, a hardware vocoder on a serial port, an implementation covered by
a licence in their jurisdiction) writes, out of tree:

```c
#include "core/dmr_vocoder.h"

static const struct dmrVocoder myBackend = {
    .name = "my vocoder", .init = myInit, .terminate = myTerminate,
    .decode = myDecode, .encode = myEncode,
};

const struct dmrVocoder *dmrVocoder_get(void)
{
    return &myBackend;
}
```

and links it, for instance with a Meson wrap or an `-l` in the cross file's
`link_args`. The backend runs on the DMR audio thread of the radio, with
its stack; a backend needing more stack, a large heap or a coprocessor is
expected to document it and to fail `init()` (negative errno) when it cannot
run, in which case the audio layer behaves as with the null backend. Such an
object must never be committed to this repository, offered in an OpenRTX
release or distributed with the firmware, since the combination would not
be distributable under the GPL.

### The test tone

`Settings > DMR > Test tone` (`settings.dmr_testTone`, off by default and
not meant to stay on) makes the null backend play a 1 kHz sine at -12 dBFS
for every received voice burst instead of silence. It exists for the
hardware bring-up of the audio path: with a call being received, the tone
must come out of the speaker for the 60 ms of every burst of the call, which
proves the OpenMusic DAC path, the amplifier and mute sequencing and the
slot timing of the pushes without any codec (`docs/dmr_bringup.md`). The
setting is read at every burst, so the tone starts and stops as it is
toggled; a transmission is not affected, the null encoder always sends the
silence pattern.

### Tests

`dmr_vocoder_test` (`tests/unit/dmr_vocoder.cpp`): the silence pattern is
one array shared by C and C++; `dmrVocoder_get()` returns the backend the
build selected; the null backend encodes to the silence pattern and decodes
to zeros with the tone off and to a 1 kHz, -12 dBFS sine with the tone on,
continuous across two calls (zero crossings, peak, RMS, DFT bin, largest
sample-to-sample step); the loopback backend round-trips a 1 kHz and a
433 Hz tone with the RMS within 5 % and the same dominant DFT bin, keeps an
envelope and the phase across payloads, plays silence for the silence
pattern, for zeros and for a corrupted payload of its own; every buffer
handed to the backends sits between guard zones that must come back intact
(and the sanitizer build catches what the guards do not); a full-scale
block holding -32768, whose magnitude does not fit an int16, keeps its
level through the loopback round trip. The test builds both backends
whatever the `dmr_vocoder` option (the null backend is in every host
executable, the loopback one is added to this test when the option left
it out).

## Hardware validation policy

Nobody has run this code on a radio yet. The series is developed against the
Linux emulator and host unit tests, so:

- the UI, the settings and the RTX plumbing of stage 1 are covered by
  `tests/unit/ui_dmr.cpp` (both the 160x128 and 128x64 layouts compile; the
  test drives the 160x128 one);
- register values, interrupt pins, bit polarities and audio gating of the
  HR_C6000 driver (stage 3 and later) are marked `UNVERIFIED on hardware`
  where they are not confirmed by the manual, and the DMR monitor screen of
  stage 3 exists to check them in the field;
- a stage is not called validated on hardware until the checklist of its
  design note has been run on a CS7000-M17 Plus and the results are written
  down in `docs/dmr_bringup.md`.

The checklist for stage 1 on a radio: cycle the modes with MONI + 5 and see
the DMR lines; check that FM and M17 still work after visiting DMR; change
the DMR settings, power cycle, and see them kept.

## Testing on the Linux emulator

```
meson setup build_linux
meson compile -C build_linux openrtx_linux openrtx_linux_smallscreen
meson test -C build_linux --print-errorlogs
```

`ui_dmr_test` covers the mode cycling and the RTX synchronisation, the seeding
of the DMR VFO from the settings, the exclusion with FRS mode, every item of
`Settings > DMR` (the test tone and the monitor entry included), the `#` and
`*` keys, the macro menu keys, the rendering of the main screen in the idle,
receive, hang, transmit and wake-up call states, the DMR monitor screen with
its pages and actions, the preservation of the receive report in
`rtx_task()`, the size and the defaults of `settings_t`.

To hear a replayed call, see [The emulator demo](#the-emulator-demo).
