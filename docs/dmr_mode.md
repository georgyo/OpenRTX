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

DMR support lands in six stages. **Stages 1 and 2 are in the tree.** With
them, DMR is a mode the user can select and configure, and the RTX stage runs
the complete Layer-2 call state machine (`OpMode_DMR` in `rtx.cpp`) against
an abstract modem interface. On the Linux emulator the modem is a scriptable
fake, so calls are received and transmitted headlessly; on the radios the
modem driver is still a stub that reports "not supported", so selecting DMR
does nothing on the air yet and the analog FM and M17 modes keep working
after visiting it.

| Stage | Title | Status |
|---|---|---|
| 1 | DMR data model, settings, UI mode selection and ETSI protocol library (no RF) | in tree |
| 2 | Call-control state machine, `dmr_baseband` interface, `OpMode_DMR` and the emulator modem model | in tree |
| 3 | HR_C6000 DMR driver, EXTI shim, `radio_CS7000` branches and the DMR monitor screen (RX sync, colour code, slot and LC with caller display, no audio) | planned |
| 4 | RX voice plumbing: vocoder backend interface, `dmr_audio` thread, PCM to the speaker through the C6000 DAC, and the internal-vocoder experiment | planned |
| 5 | TX voice calls: simplex (DMO, active timing) first, then repeater (RMO, passive timing with wake-up) | planned |
| 6 | Contacts and talkgroup UI, last heard, private calls from history, voice prompts and documentation polish | planned |

**Nothing of the DMR series has been verified on a radio.** Every value that
comes from the HR_C6000 manual, the CS7000 schematic or the observation of
other firmwares, but has not been confirmed on a CS7000, is marked
`UNVERIFIED on hardware` in the code and is listed in `docs/dmr_bringup.md`
once stage 3 arrives. Stage 1 does not touch the hardware; its only such
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

The DMR ID and the talkgroup are typed in: ENTER opens the entry, digits
compose the number, ENTER stores it, ESC leaves the old value, the arrows
delete the last digit. The other items change with the arrows, in edit mode
(ENTER) or directly with LEFT and RIGHT.

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
`dmr_txColorCode`, `dmr_timeslot`, `dmr_monitor`, `dmr_polite`) and a receive
report the DMR opMode handler will fill from stage 2 on (`dmr_lcOk`,
`dmr_rxSrcId`, `dmr_rxDstId`, `dmr_rxFlco`, `dmr_rxColorCodeSeen`,
`dmr_rxTimeslot`, `dmr_rxSyncType`, `dmr_callState`, `dmr_slotLock`). The
report fields are owned by the opMode handler: `rtx_task()` keeps them across
the copy of a new configuration, together with `opStatus`. The call state is
one of `DMR_CALL_IDLE`, `DMR_CALL_RX`, `DMR_CALL_RX_HANG`, `DMR_CALL_TX` and
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
with a snapshot of the registers they refer to. The register numbers are the
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
the silent channel gives no timeslot interrupt). Timeslot interrupts merged into one event by a late wake-up of the
rtx thread are recovered from the ISR counter and timestamp carried by the
snapshot, so the slot parity of a call or transmission does not flip.

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
  after, and by `disable()` mid-call), and the "not supported" path. It
  prints the sizes: `OpMode_DMR` is 344 bytes as a static object,
  `DMR::CallController` 176, `struct dmrbbSnapshot` 72.
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
- `ui_dmr_test`: also the marker text selection.

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
- The vocoder is therefore a pluggable backend (`core/dmr_vocoder.h`, stage
  4). The tree ships a null backend (silence on TX, silence or a debug test
  tone on RX, enabled by the `dmr_testTone` setting) and an emulator loopback
  backend, so that the slot machinery, the V_SPI transfers, the audio paths
  and the interoperation with repeaters and hotspots are exercised end to
  end. A Meson option will let whoever holds a licence link an out-of-tree
  backend. Whether the chip can decode voice by itself after all is the first
  experiment of stage 4; the documentation says it cannot.

Until a vocoder backend is available, a DMR call carries no audible voice:
the radio shows the caller, the talkgroup, the colour code and the slot, and
transmits headers, terminators and silence frames.

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
`Settings > DMR`, the `#` and `*` keys, the macro menu keys, the rendering of
the main screen in the idle, receive, hang, transmit and wake-up call states,
the preservation of the receive report in `rtx_task()`, the size and the
defaults of `settings_t`.
