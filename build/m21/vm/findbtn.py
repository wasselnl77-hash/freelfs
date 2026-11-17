#!/usr/bin/env python3
"""findbtn.py FILE.png R G B MINRUN : centre (x y) of the first horizontal run of that colour of at least MINRUN pixels"""
import sys, zlib, struct
d = open(sys.argv[1], "rb").read()
pos = 8; idat = b""
while pos < len(d):
    n, t = struct.unpack(">I4s", d[pos:pos + 8]); body = d[pos + 8:pos + 8 + n]
    if t == b"IHDR": w, h, bd, ct = struct.unpack(">IIBB", body[:10])
    if t == b"IDAT": idat += body
    pos += 12 + n
bpp = {2: 3, 6: 4}[ct]; raw = zlib.decompress(idat); stride = w * bpp
rows = []; prev = bytearray(stride); o = 0
for y in range(h):
    f = raw[o]; line = bytearray(raw[o + 1:o + 1 + stride]); o += 1 + stride
    for i in range(stride):
        a = line[i - bpp] if i >= bpp else 0; b = prev[i]; c = prev[i - bpp] if i >= bpp else 0
        if f == 1: line[i] = (line[i] + a) & 255
        elif f == 2: line[i] = (line[i] + b) & 255
        elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
        elif f == 4:
            p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
            line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
    rows.append(line); prev = line
want = bytes(int(v) for v in sys.argv[2:5]); minrun = int(sys.argv[5])
for y in range(h):
    run = 0
    for x in range(w):
        if rows[y][x * bpp:x * bpp + 3] == want:
            run += 1
            if run >= minrun:
                print(x - run // 2, y + 20); sys.exit(0)
        else: run = 0
sys.exit(1)
