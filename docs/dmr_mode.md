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

DMR support lands in six stages. **Only stage 1 is in the tree.** With it,
DMR is a mode the user can select and configure; selecting it does nothing on
the air yet, the RTX stage keeps the radio idle (`OPMODE_DMR` is routed to the
empty opMode handler in `rtx.cpp`) and the analog FM and M17 modes keep
working after visiting it.

| Stage | Title | Status |
|---|---|---|
| 1 | DMR data model, settings, UI mode selection and ETSI protocol library (no RF) | in tree |
| 2 | Call-control state machine, `dmr_baseband` interface, `OpMode_DMR` and the emulator modem model | planned |
| 3 | HR_C6000 DMR driver, EXTI shim, `radio_CS7000` branches and the DMR monitor screen (RX sync, colour code, slot and LC with caller display, no audio) | planned |
| 4 | RX voice plumbing: vocoder backend interface, `dmr_audio` thread, PCM to the speaker through the C6000 DAC, and the internal-vocoder experiment | planned |
| 5 | TX voice calls: simplex (DMO, active timing) first, then repeater (RMO, passive timing with wake-up) | planned |
| 6 | Contacts and talkgroup UI, last heard, private calls from history, voice prompts and documentation polish | planned |

**Nothing of the DMR series has been verified on a radio.** Every value that
comes from the HR_C6000 manual, the CS7000 schematic or the observation of
other firmwares, but has not been confirmed on a CS7000, is marked
`UNVERIFIED on hardware` in the code and is listed in `docs/dmr_bringup.md`
once stage 3 arrives. Stage 1 contains no such value: it does not touch the
hardware.

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
