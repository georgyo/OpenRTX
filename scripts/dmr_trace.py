#!/usr/bin/env python3
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors

"""Decode a capture of the DMR bring-up trace.

The radio sends the trace on its USB serial port while it is in DMR mode,
see docs/dmr_bringup.md ("Trace on the USB serial port"). Capture it with:

    stty -F /dev/ttyACM0 raw -echo
    cat /dev/ttyACM0 > dmr-trace.log

then run this script on the file. Each input line is one record: the kind
(one letter), six hexadecimal digits of time in milliseconds, then the data
bytes. The record layouts are the ones of OpMode_DMR.cpp.
"""

import argparse
import sys

STATES = ["OFF", "RX_SEARCH", "RX_IDLE", "RX_CALL", "RX_HANG", "TX_ARM",
          "TX_WAKEUP", "TX_HEADER", "TX_VOICE", "TX_TERM"]
RF_STATES = ["off", "rx", "tx"]
SYNC_CLASS = ["none", "voice", "data", "rc"]
SYNC_TYPE = ["ms", "bs", "tdma1", "tdma2"]
DATA_TYPES = {0: "PI header", 1: "voice LC header", 2: "terminator LC",
              3: "CSBK", 4: "MBC header", 5: "MBC cont", 6: "data header",
              7: "rate 1/2", 8: "rate 3/4", 9: "idle", 10: "rate 1"}
CONTROL = {1: "startRx", 2: "startTx passive", 3: "startTx active",
           4: "idle"}
IRQ_BITS = ["phy", "abnormal", "rxinfo", "rxdata", "late", "txend",
            "txstart", "rejected"]
SILENCE = bytes.fromhex("B9E881526173002A6B")
TONE = bytes.fromhex("CEA8FE83ACC458200A")


def state(value):
    return STATES[value] if value < len(STATES) else "?%d" % value


def be(data):
    return int.from_bytes(data, "big")


def flags(value):
    names = [IRQ_BITS[i] for i in range(8) if value & (1 << i)]
    return "+".join(names) if names else "-"


def burst(r51):
    cls = r51 & 0x03
    kind = r51 >> 4
    crc = "crc-bad" if r51 & 0x04 else "crc-ok"
    if cls == 2:
        return "data %s %s" % (DATA_TYPES.get(kind, "type %d" % kind), crc)
    if cls == 3:
        return "rc"
    seq = kind & 0x07
    if 1 <= seq <= 6:
        return "voice %s (%s sync)" % ("ABCDEF"[seq - 1], SYNC_CLASS[cls])
    return "%s sync, type %d" % (SYNC_CLASS[cls], kind)


def lc(data):
    return "flco=%02X fid=%02X so=%02X dst=%d src=%d par=%s" % (
        data[0], data[1], data[2], be(data[3:6]), be(data[6:9]),
        data[9:12].hex().upper())


def voice(data):
    frames = [data[i:i + 9] for i in (0, 9, 18)]
    names = []
    for frame in frames:
        if frame == SILENCE:
            names.append("silence")
        elif frame == TONE:
            names.append("tone")
        else:
            names.append(frame.hex().upper())
    return " ".join(names)


def decode(kind, d):
    if kind == "E":
        return ("config cc rx=%d tx=%d ts=%d monitor=%d polite=%d "
                "repeater=%d callType=%d 0x10=%02X 0x12=%02X own=%d dst=%d "
                "rx=%d Hz tx=%d Hz hang=%ds tone=%d sysLine=PC%d "
                "voicePath=%d" % (
                    d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8],
                    be(d[9:12]), be(d[12:15]), be(d[15:19]), be(d[19:23]),
                    d[23], d[24], d[25], d[26]))
    if kind == "R":
        return "reg %02X: %s" % (d[0], " ".join("%02X" % v for v in d[1:]))
    if kind == "T":
        r41 = "--" if d[5] == 0xFF else "%02X" % d[5]
        r50 = "--" if d[6] == 0xFF else "%02X" % d[6]
        return ("slot %-9s tc=%d lock=%d 0x52=%02X 0x42=%02X next 0x41=%s "
                "0x50=%s ts#%d tick=%d sys#%d rftx#%d" % (
                    state(d[0]), d[1], d[2], d[3], d[4], r41, r50,
                    be(d[7:9]), be(d[9:11]), be(d[11:13]), be(d[13:15])))
    if kind == "S":
        return ("sys  %-9s tc=%d 0x82=%02X(%s) 0x51=%02X(%s) 0x52=%02X(cc=%d "
                "tc=%d) 0x5F=%02X(%s) 0x42=%02X 84=%02X 86=%02X 90=%02X "
                "98=%02X rssi=%d err=%d voice=%d lc[%s]" % (
                    state(d[0]), d[1], d[2], flags(d[2]), d[3], burst(d[3]),
                    d[4], d[4] >> 4, (d[4] >> 2) & 1, d[5],
                    SYNC_TYPE[d[5] & 3], d[6], d[7], d[8], d[9], d[10],
                    be(d[11:13]), d[13], d[14], lc(d[15:27])))
    if kind == "V":
        return "voice payload: " + voice(d)
    if kind == "W":
        return "modem " + CONTROL.get(d[0], "?%d" % d[0])
    if kind == "K":
        return "rf key=%d rfState=%s radioStatus=%d" % (
            d[0], RF_STATES[d[1]] if d[1] < 3 else "?", d[2])
    if kind == "P":
        rssi = be(d[20:22])
        if rssi >= 0x8000:
            rssi -= 0x10000
        return ("stat %-9s rf=%s ptt=%d markers=%02X call=%d lock=%d "
                "PC0#%d PC1#%d PC2#%d PC3#%d txBursts=%d txFilled=%d "
                "rxVoice=%d rssi=%d dBm audio=%d audioFail=%02X" % (
                    state(d[0]), RF_STATES[d[1]] if d[1] < 3 else "?", d[2],
                    d[3], d[4], d[5], be(d[6:8]), be(d[8:10]), be(d[10:12]),
                    be(d[12:14]), be(d[14:16]), be(d[16:18]), be(d[18:20]),
                    rssi, d[22], d[23]))
    return "? " + d.hex().upper()


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("capture", help="file captured from the serial port")
    parser.add_argument("--no-slots", action="store_true",
                        help="leave the slot ticks out")
    args = parser.parse_args()

    with open(args.capture, "r", errors="replace") as capture:
        for raw in capture:
            line = raw.strip()
            if len(line) < 7:
                continue
            kind = line[0]
            if kind == "D":
                print("          *** %d records lost ***" % int(line[1:], 16))
                continue
            if kind == "T" and args.no_slots:
                continue
            try:
                ms = int(line[1:7], 16)
                data = bytes.fromhex(line[7:])
                text = decode(kind, data)
            except (ValueError, IndexError):
                text = "undecodable: " + line
                ms = 0
            print("%9.3f %s" % (ms / 1000.0, text))

    return 0


if __name__ == "__main__":
    sys.exit(main())
