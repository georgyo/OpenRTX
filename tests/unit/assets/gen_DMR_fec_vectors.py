#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Golden vector generator for tests/unit/DMR_fec.cpp.
#
# Oracle: dmr_utils3 0.1.31 (GPLv3, https://github.com/n0mjs710/dmr_utils,
# `pip install dmr_utils3`, needs the bitarray package) with one typo fixed
# in the installed copy before running:
#   - bptc.py encode_emblc(): the second row of the third fragment reads
#     _binlc[24] twice; the ETSI TS 102 361-1 figure B.3 interleave reads
#     column 9 of the encode matrix as _binlc[25].
#
# dmr_utils3 qr.decode() and golay.decode_2087() do not terminate when the
# most significant codeword bit is set (their syndrome loops start one bit
# too low), so only the ENCODE_1676 / ENCODE_2087 tables are used as oracle.
#
# The generator matrices of ETSI TS 102 361-1 V2.5.1 Annex B are transcribed
# below and used as an independent second source: every table computed from
# them is compared with the dmr_utils3 result before being emitted.
#
# Usage: python3 gen_DMR_fec_vectors.py > DMR_fec_vectors.inc
# The output is included by tests/unit/DMR_fec.cpp.

import random
import sys

from bitarray import bitarray
from dmr_utils3 import bptc, golay, qr, hamming, rs129, crc
from dmr_utils3.golay_tables import ENCODE_2087
from dmr_utils3.qr import ENCODE_1676

# ---------------------------------------------------------------------------
# Generator matrices (parity part only), rows MSB-first. TS 102 361-1 B.3.x
# ---------------------------------------------------------------------------

# Table B.11: Golay (20,8), 12 parity bits per row
GOLAY_20_8 = [
    "001111011010",
    "110110011001",
    "011011001101",
    "001101100111",
    "110111000110",
    "101010010111",
    "100100111110",
    "100011101011",
]

# Table B.12: QR (16,7,6), 9 parity bits per row
QR_16_7 = [
    "001001111",
    "100011110",
    "110110111",
    "111100010",
    "111001001",
    "011100101",
    "001110011",
]

# Table B.13: Hamming (17,12,3), 5 parity bits
H17_12 = ["11011", "11111", "11101", "11100", "01110", "00111",
          "10001", "11010", "01101", "10100", "01010", "00101"]

# Table B.14: Hamming (13,9,3), 4 parity bits
H13_9 = ["1111", "1110", "0111", "1010", "0101", "1011", "1100", "0110",
         "0011"]

# Table B.15: Hamming (15,11,3), 4 parity bits
H15_11 = ["1001", "1101", "1111", "1110", "0111", "1010", "0101", "1011",
          "1100", "0110", "0011"]

# Table B.16: Hamming (16,11,4), 5 parity bits
H16_11 = ["10011", "11010", "11111", "11100", "01110", "10101", "01011",
          "10110", "11001", "01101", "00111"]

# Table B.17: Hamming (7,4,3), 3 parity bits
H7_4 = ["101", "111", "110", "011"]


def parity(rows, data):
    """Parity of `data` (k bits, row 0 = MSB) using the generator rows."""
    k = len(rows)
    r = len(rows[0])
    p = 0
    for i in range(k):
        if (data >> (k - 1 - i)) & 1:
            p ^= int(rows[i], 2)
    return p


def check(cond, msg):
    if not cond:
        raise SystemExit("cross-check failed: " + msg)


# ---------------------------------------------------------------------------
# Cross-checks against dmr_utils3
# ---------------------------------------------------------------------------

# Golay (20,8): dmr_utils3 stores the parity as ((p & 0xFF) << 8) | (p >> 8)
golay_table = [parity(GOLAY_20_8, d) for d in range(256)]
for d in range(256):
    t = ENCODE_2087[d]
    p = ((t >> 12) & 0xF) | ((t & 0xFF) << 4)
    check(p == golay_table[d], "Golay(20,8) parity of %d" % d)

# QR (16,7,6): dmr_utils3 stores the 16-bit codeword
qr_table = [parity(QR_16_7, d) for d in range(128)]
for d in range(128):
    check(ENCODE_1676[d] == (d << 9) | qr_table[d], "QR(16,7,6) of %d" % d)


def ba(value, n):
    b = bitarray(endian='big')
    for i in range(n - 1, -1, -1):
        b.append((value >> i) & 1)
    return b


def ba2int(b):
    v = 0
    for bit in b:
        v = (v << 1) | int(bit)
    return v


for d in range(2048):
    check(ba2int(hamming.enc_15113(ba(d, 11))) == parity(H15_11, d),
          "Hamming(15,11) of %d" % d)
    check(ba2int(hamming.enc_16114(ba(d, 11))) == parity(H16_11, d),
          "Hamming(16,11) of %d" % d)
for d in range(512):
    check(ba2int(hamming.enc_1393(ba(d, 9))) == parity(H13_9, d),
          "Hamming(13,9) of %d" % d)

# ---------------------------------------------------------------------------
# Over-the-air voice LC header captured burst (dmr_utils3 bptc.py self-test)
# ---------------------------------------------------------------------------
CAPTURED_BURST = bytes.fromhex(
    "2b6004101f842dd00df07d41046dff57d75df5de30152e2070b20f803f88c695e2")
CAPTURED_LC = bytes.fromhex("001020000c302f9be5")

cb = bitarray(endian='big')
cb.frombytes(CAPTURED_BURST)
info = cb[0:98] + cb[166:264]
slot = cb[98:108] + cb[156:166]
sync = cb[108:156]

enc = bptc.encode_header_lc(CAPTURED_LC)
check(enc == info, "captured burst BPTC/RS encode")
check(ba2int(slot) == (0x11 << 12) | golay_table[0x11],
      "captured burst slot type (CC 1, Voice LC Header)")
# TS 102 361-1 table 9.2: BS sourced data 0xDFF57D75DF5D, MS sourced data
# 0xD5D7F77FD757
check(sync.tobytes() in (bytes.fromhex("DFF57D75DF5D"),
                         bytes.fromhex("D5D7F77FD757")),
      "captured burst sync is a data sync pattern")
sys.stderr.write("captured burst sync: %s\n" % sync.tobytes().hex())
# The RS(12,9) parity is validated by the bit-exact match of the captured
# burst above (the burst carries LC + masked parity through the BPTC).
sys.stderr.write("captured LC masked RS parity: %s\n"
                 % rs129.lc_header_encode(CAPTURED_LC).hex())

# ---------------------------------------------------------------------------
# Emit
# ---------------------------------------------------------------------------
rng = random.Random(0x0D3A)


def c_array(name, ctype, values, fmt, per_line):
    out = "static const %s %s[%d] = {\n" % (ctype, name, len(values))
    for i in range(0, len(values), per_line):
        out += "    " + ", ".join(fmt % v for v in values[i:i + per_line])
        out += ",\n"
    out += "};\n"
    return out


print("// Generated by gen_DMR_fec_vectors.py (see header comment), "
      "do not edit")
print()
print(c_array("golay20_8_parity", "uint16_t", golay_table, "0x%03X", 8))
print(c_array("qr16_7_6_parity", "uint16_t", qr_table, "0x%03X", 8))

# Hamming vectors: (data, parity) pairs, 16 per code
for name, rows in (("hamming7_4", H7_4), ("hamming13_9", H13_9),
                   ("hamming15_11", H15_11), ("hamming16_11", H16_11),
                   ("hamming17_12", H17_12)):
    k = len(rows)
    vals = []
    for _ in range(16):
        d = rng.randrange(1 << k)
        vals.append(d)
        vals.append(parity(rows, d))
    print(c_array(name + "_vectors", "uint16_t", vals, "0x%04X", 8))

# BPTC(196,96) + RS(12,9) vectors: 9-byte LC -> 3-byte masked parity and
# 25-byte interleaved encode matrix (196 bits, MSB first, last nibble zero)
lcs = [
    CAPTURED_LC,
    bytes.fromhex("000000000009000001"),      # group call, dst 9, src 1
    bytes.fromhex("030000000001000009"),      # private call, dst 1, src 9
    bytes.fromhex("000000ffffffffffff"),      # all-ones addresses
    bytes.fromhex("000000000000000000"),
]
for _ in range(3):
    lcs.append(bytes(rng.randrange(256) for _ in range(9)))

print("struct BptcVector {")
print("    uint8_t lc[9];")
print("    uint8_t headerParity[3];")
print("    uint8_t terminatorParity[3];")
print("    uint8_t header[25];")
print("    uint8_t terminator[25];")
print("    uint8_t checksum5;")
print("    uint32_t embedded[4];")
print("};")
print()
print("static const BptcVector bptc_vectors[%d] = {" % len(lcs))
for lc in lcs:
    hp = rs129.lc_header_encode(lc)
    tp = rs129.lc_terminator_encode(lc)
    hb = bptc.encode_header_lc(lc).tobytes()
    tb = bptc.encode_terminator_lc(lc).tobytes()
    check(len(hb) == 25 and len(tb) == 25, "bptc length")
    check(bptc.decode_full_lc(bptc.encode_header_lc(lc)).tobytes() == lc,
          "bptc decode identity")
    cs = ba2int(crc.csum5(lc))
    emb = bptc.encode_emblc(lc)
    frags = [ba2int(emb[i]) for i in (1, 2, 3, 4)]
    check(bptc.decode_emblc(emb[1] + emb[2] + emb[3] + emb[4]) == lc,
          "emblc decode identity")

    def hexlist(b):
        return ", ".join("0x%02X" % x for x in b)
    print("    {")
    print("        { %s }," % hexlist(lc))
    print("        { %s }," % hexlist(hp))
    print("        { %s }," % hexlist(tp))
    print("        { %s,\n          %s,\n          %s,\n          %s },"
          % (hexlist(hb[0:7]), hexlist(hb[7:14]), hexlist(hb[14:21]),
             hexlist(hb[21:25])))
    print("        { %s,\n          %s,\n          %s,\n          %s },"
          % (hexlist(tb[0:7]), hexlist(tb[7:14]), hexlist(tb[14:21]),
             hexlist(tb[21:25])))
    print("        %d," % cs)
    print("        { %s }," % ", ".join("0x%08X" % f for f in frags))
    print("    },")
print("};")
print()
print("static const uint8_t captured_voice_header_burst[33] = {")
for i in range(0, 33, 11):
    print("    " + ", ".join("0x%02X" % x for x in CAPTURED_BURST[i:i + 11])
          + ",")
print("};")
