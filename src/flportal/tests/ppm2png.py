#!/usr/bin/env python3
"""ppm2png.py IN.ppm OUT.png -- P6 to PNG with zlib only. SPDX-License-Identifier: MIT"""
import struct
import sys
import zlib

d = open(sys.argv[1], "rb").read()
parts = d.split(b"\n", 3)
w, h = map(int, parts[1].split())
px = parts[3]
raw = b"".join(b"\0" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))


def chunk(t, b):
    c = struct.pack(">I", len(b)) + t + b
    return c + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)


open(sys.argv[2], "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                              + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))
