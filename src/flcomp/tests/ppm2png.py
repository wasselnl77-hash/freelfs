#!/usr/bin/env python3
# ppm2png.py IN.ppm OUT.png [SCALE]: the headless screenshots (P6) as PNG, optionally enlarged by an integer factor.
# SPDX-License-Identifier: MIT
import struct
import sys
import zlib


def main():
    src, dst = sys.argv[1], sys.argv[2]
    scale = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    with open(src, 'rb') as f:
        assert f.readline() == b'P6\n'
        w, h = map(int, f.readline().split())
        f.readline()
        d = f.read()
    rows = []
    for y in range(h):
        row = d[y * w * 3:(y + 1) * w * 3]
        if scale > 1:
            row = b''.join(row[i:i + 3] * scale for i in range(0, len(row), 3))
        for _ in range(scale):
            rows.append(b'\x00' + row)
    raw = b''.join(rows)

    def chunk(t, body):
        c = struct.pack('>I', len(body)) + t + body
        return c + struct.pack('>I', zlib.crc32(t + body) & 0xffffffff)

    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w * scale, h * scale, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    with open(dst, 'wb') as f:
        f.write(png)


main()
