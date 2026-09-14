# FRS mode

FRS mode turns the radio into a 22-channel Family Radio Service handheld: the
channel plan, bandwidth, power class and simplex operation come from
47 CFR part 95 subpart B, and the "privacy codes" use the numbering shared by
Motorola, Midland, Cobra and Uniden FRS radios, so a code agreed with someone
on a consumer radio can be typed in directly.

The mode is a setting: `Settings > FRS > FRS Mode`. While it is on the radio
shows the FRS screen instead of the VFO and memory screens, and the VFO the
user had before is kept aside and restored when the mode is turned off.

## Read this first: regulatory status

The FCC only allows FRS operation with transmitters certified for FRS
([47 CFR 95.335](https://www.ecfr.gov/current/title-47/section-95.335),
[95.561](https://www.ecfr.gov/current/title-47/section-95.561)). None of the
radios OpenRTX runs on, the Connect Systems CS7000-M17 Plus included, carry
that certification, and OpenRTX does not add it. FRS mode only tunes the radio
to the FRS channel plan; whether transmitting there is permitted depends on
the operator's authorisation and jurisdiction, and it is the operator's
responsibility to check. Consistent with the rest of OpenRTX, the firmware
does not block transmission. Outside the United States the same frequencies
belong to other services (the 462/467 MHz range is not PMR446).

Relevant rules, with the values used by the firmware:

| Rule | What it sets |
|---|---|
| [95.563](https://www.ecfr.gov/current/title-47/section-95.563) | The 22 channel frequencies (table below) |
| [95.567](https://www.ecfr.gov/current/title-47/section-95.567) | 2 W ERP on channels 1-7 and 15-22, 0.5 W ERP on channels 8-14 |
| [95.573](https://www.ecfr.gov/current/title-47/section-95.573) | 12.5 kHz authorised bandwidth |
| [95.575](https://www.ecfr.gov/current/title-47/section-95.575) | 2.5 kHz peak frequency deviation |
| [Subpart B](https://www.ecfr.gov/current/title-47/chapter-I/subchapter-D/part-95/subpart-B) | The complete FRS rules |

## Channels

| Channel | Frequency (MHz) | Power class | Channel | Frequency (MHz) | Power class |
|---|---|---|---|---|---|
| 1 | 462.5625 | 2 W | 12 | 467.6625 | 0.5 W |
| 2 | 462.5875 | 2 W | 13 | 467.6875 | 0.5 W |
| 3 | 462.6125 | 2 W | 14 | 467.7125 | 0.5 W |
| 4 | 462.6375 | 2 W | 15 | 462.5500 | 2 W |
| 5 | 462.6625 | 2 W | 16 | 462.5750 | 2 W |
| 6 | 462.6875 | 2 W | 17 | 462.6000 | 2 W |
| 7 | 462.7125 | 2 W | 18 | 462.6250 | 2 W |
| 8 | 467.5625 | 0.5 W | 19 | 462.6500 | 2 W |
| 9 | 467.5875 | 0.5 W | 20 | 462.6750 | 2 W |
| 10 | 467.6125 | 0.5 W | 21 | 462.7000 | 2 W |
| 11 | 467.6375 | 0.5 W | 22 | 462.7250 | 2 W |

Every channel is narrowband FM (12.5 kHz), simplex, with the 1750 Hz tone
burst disabled.

**Power on channels 8-14.** The firmware requests 500 mW on these channels and
2 W on the others, but every OpenRTX RF driver clamps the transmit power to
the 1-5 W range of the power amplifier, so on real hardware channels 8-14
transmit at the 1 W driver floor, above the 0.5 W ERP limit of 95.567. The
main screen therefore shows the power class as `Lo`/`Hi` rather than a
misleading `0.5W`. Lowering the driver floor is hardware-specific work that is
not part of this feature.

## Privacy codes

Codes 1-38 are CTCSS tones, one per channel and remembered per channel:

| Code | Tone (Hz) | Code | Tone (Hz) | Code | Tone (Hz) | Code | Tone (Hz) |
|---|---|---|---|---|---|---|---|
| 1 | 67.0 | 11 | 97.4 | 21 | 136.5 | 31 | 192.8 |
| 2 | 71.9 | 12 | 100.0 | 22 | 141.3 | 32 | 203.5 |
| 3 | 74.4 | 13 | 103.5 | 23 | 146.2 | 33 | 210.7 |
| 4 | 77.0 | 14 | 107.2 | 24 | 151.4 | 34 | 218.1 |
| 5 | 79.7 | 15 | 110.9 | 25 | 156.7 | 35 | 225.7 |
| 6 | 82.5 | 16 | 114.8 | 26 | 162.2 | 36 | 233.6 |
| 7 | 85.4 | 17 | 118.8 | 27 | 167.9 | 37 | 241.8 |
| 8 | 88.5 | 18 | 123.0 | 28 | 173.8 | 38 | 250.3 |
| 9 | 91.5 | 19 | 127.3 | 29 | 179.9 | | |
| 10 | 94.8 | 20 | 131.8 | 30 | 186.2 | | |

Code 0 (`Off`) transmits and receives without a tone. The numbering is the one
printed in the Motorola Talkabout, Midland, Cobra and Uniden manuals; it skips
twelve of the fifty tones in the OpenRTX CTCSS table (69.3, 159.8, 165.5,
171.3, 177.3, 183.5, 189.9, 196.6, 199.5, 206.5, 229.1 and 254.1 Hz), so a
privacy code is not a plain index into that table.

**DCS codes are not implemented.** The same manuals continue with codes 39-121
(Motorola) or 39-121 of a separate DCS list (Midland and Cobra number their
DCS codes 1-83, and their code *n* is Motorola code *n* + 38). These are
digital-coded squelch codes, which OpenRTX does not support in any mode yet.
Adding them is future work that starts in the RTX layer, not in FRS mode.

**Receive tone squelch.** Where the hardware can decode CTCSS, a privacy code
both transmits the tone and mutes the receiver until the same tone is heard
(shown as `B` next to the tone on the main screen). The TYT MD-380/390 and
MD-9600 drivers, and the Linux emulator, cannot detect a received tone
(`radio_checkRxDigitalSquelch()` always returns false); enabling decode there
would keep the receiver muted forever, so on these targets the code is
transmit-only and the screen shows `T`.

## Using it

Turn the mode on in `Settings > FRS > FRS Mode` (ENTER, then UP/DOWN or the
knob, ENTER to leave the entry). The radio must cover 462-468 MHz on its UHF
band; on a VHF-only radio the switch is refused with the error prompt.

On the FRS screen:

| Key | Action |
|---|---|
| UP / knob right | Next channel (22 wraps to 1) |
| DOWN / knob left | Previous channel (1 wraps to 22) |
| `3` .. `9` | Go to that channel |
| `1` or `2`, then a digit | Go to channel 10-22 (`1 2` = channel 12); ENTER or a 2 s pause accepts a single digit |
| `*` or side key F2 | Open the privacy code list for this channel |
| ENTER | Menu |
| PTT | Transmit on the channel with the channel's code |
| ESC, `#` | No function (there is no VFO to leave, and no 1750 Hz tone on FRS) |

In the privacy code list UP/DOWN or the knob move through `Off` and codes
1-38 with their tone, digits jump to a code (`* 1 2 ENTER` sets code 12,
`* 0 ENTER` turns the code off), ENTER applies and ESC or `*` cancels.
In the MONI macro menu keys 2 and 3 step the code down and up; keys 1, 4, 5
and 6 (tone mode, bandwidth, FM/M17, power) are refused because the channel
plan fixes those parameters. The same goes for loading a bank or a codeplug
channel and for the CTCSS and Radio (offset, direction, step) settings, which
read `FRS` while the mode is on. `Settings > FRS > Reset Codes` (ENTER twice)
clears the code on every channel.

Turning FRS mode off, or `Settings > Default Settings`, returns to the VFO
screen with the frequency the radio had before FRS was enabled.

## Persistence

The mode, the current channel and the 22 codes are stored with the other
settings, so the radio boots straight into FRS mode on the last channel. The
VFO saved at power-off is the parked one, not the FRS channel.

The fields are appended to the settings record, so a record written by an
older firmware loads with FRS off and no codes:

- CS7000 / CS7000-M17 Plus and the Linux emulator: older records load as-is,
  the new fields are written at the next power-off.
- TYT MD-380/390, MD-UV380/390, MD-9600: the settings and VFO area is
  CRC-checked as a whole, so the first boot after the update falls back to
  default settings and VFO once (the same happens for any change to the
  settings layout).
- GD-77, DM-1801, DM-1701, T-TWR Plus, RT-4D: these targets do not persist
  settings at all, so FRS mode does not survive a power cycle there.

## Testing on the Linux emulator

The emulator (`nix build .#emulator`, or `meson compile -C build_linux
openrtx_linux`) accepts key sequences on its shell, which makes the mode easy
to exercise headless:

```
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./openrtx_linux
> key ENTER DOWN DOWN DOWN DOWN ENTER
> key DOWN DOWN DOWN DOWN DOWN DOWN ENTER
> key ENTER UP ENTER ESC ESC ESC
> key 1 2
> key STAR 1 2 ENTER
> screenshot frs.bmp
```

Two shell details matter: a `key` command takes at most 12 keys, and macro
menu combinations must be sent with `keycombo MONI 3` (`key MONI 3` presses
the keys one after the other). The unit tests `FRS Test` and `UI FRS Test`
(`meson test -C build_linux`) cover the tables and the screen state machine.
