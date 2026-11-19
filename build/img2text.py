#!/usr/bin/env python3
"""img2text -- turn a PNG into braille, half-block or dot text art.

    build/img2text.py IMAGE.png [-w COLS] [-m braille|blocks|dots] [-t 0-255]
                      [--invert] [--fastfetch] [-o OUT.txt]

  -w COLS      width in characters (height follows the image's aspect ratio)
  -m MODE      braille: 2x4 dots per character (finest)
               blocks:  half blocks, 1x2 pixels per character (solid look)
               dots:    one '•' per pixel-cell (coarse, round)
  -t LEVEL     brightness/alpha threshold, default 128
  --invert     draw the dark parts instead of the bright/opaque ones
  --fastfetch  wrap the art in $1 so fastfetch colours it with logo color 1
  -o FILE      write to FILE instead of stdout

A pixel counts as "on" when it is opaque (alpha >= threshold) and, for images
without transparency, brighter than the threshold. Pure Python, no
dependencies: 8-bit non-interlaced PNG (gray, RGB, palette, with or without
alpha).
SPDX-License-Identifier: MIT
"""
import argparse
import struct
import sys
import zlib


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        sys.exit("img2text: not a PNG file")
    pos, idat, plte, trns = 8, b"", None, None
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + n]
        pos += 12 + n
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, inter = struct.unpack(">IIBBBBB", chunk)
        elif kind == b"PLTE":
            plte = chunk
        elif kind == b"tRNS":
            trns = chunk
        elif kind == b"IDAT":
            idat += chunk
        elif kind == b"IEND":
            break
    if depth != 8 or inter:
        sys.exit("img2text: only 8-bit, non-interlaced PNGs are supported")
    chans = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * chans
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(h):
        f = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - chans] if x >= chans else 0
            b = prev[x]
            c = prev[x - chans] if x >= chans else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        rows.append(line)
        prev = line
    # to (luma, alpha) per pixel
    px = []
    for line in rows:
        out = []
        for x in range(w):
            v = line[x * chans:(x + 1) * chans]
            if ctype == 0:
                out.append((v[0], 255))
            elif ctype == 4:
                out.append((v[0], v[1]))
            elif ctype == 2:
                out.append(((v[0] * 3 + v[1] * 6 + v[2]) // 10, 255))
            elif ctype == 6:
                out.append(((v[0] * 3 + v[1] * 6 + v[2]) // 10, v[3]))
            else:  # palette
                r, g, b = plte[v[0] * 3:v[0] * 3 + 3]
                al = trns[v[0]] if trns and v[0] < len(trns) else 255
                out.append(((r * 3 + g * 6 + b) // 10, al))
        px.append(out)
    has_alpha = ctype in (4, 6) or trns is not None
    return w, h, px, has_alpha


def main():
    ap = argparse.ArgumentParser(description="PNG to braille/block/dot text art")
    ap.add_argument("image")
    ap.add_argument("-w", "--width", type=int, default=24)
    ap.add_argument("-m", "--mode", choices=("braille", "blocks", "dots"), default="braille")
    ap.add_argument("-t", "--threshold", type=int, default=128)
    ap.add_argument("-c", "--coverage", type=float, default=0.5,
                    help="part of a dot's area that must be filled (0-1), default 0.5")
    ap.add_argument("--keep-specks", action="store_true",
                    help="keep single stray dots (removed by default)")
    ap.add_argument("--invert", action="store_true")
    ap.add_argument("--fastfetch", action="store_true")
    ap.add_argument("-o", "--output")
    a = ap.parse_args()

    w, h, px, has_alpha = read_png(a.image)

    def on(x, y):  # x, y in 0..1
        X = min(w - 1, int(x * w))
        Y = min(h - 1, int(y * h))
        l, al = px[Y][X]
        v = al >= a.threshold if has_alpha else l >= a.threshold
        return v != a.invert

    # sub-pixels per character cell and character aspect (cells are ~2x taller than wide)
    sx, sy = {"braille": (2, 4), "blocks": (1, 2), "dots": (1, 2)}[a.mode]
    cols = max(1, a.width)
    rows = max(1, round(cols * h / w / 2))  # terminal cells are about twice as tall as wide
    gw, gh = cols * sx, rows * sy

    # each dot looks at an S x S grid inside its own area and is on only if
    # at least COVERAGE of that area is filled: no stray dots on the edges
    S = 6

    def sub(i, j):
        hits = 0
        for u in range(S):
            for v in range(S):
                hits += on((i + (u + 0.5) / S) / gw, (j + (v + 0.5) / S) / gh)
        return hits >= a.coverage * S * S

    # compute every dot once, then drop lonely dots (fewer than 2 neighbours)
    grid = [[sub(i, j) for i in range(gw)] for j in range(gh)]
    if not a.keep_specks:
        clean = [row[:] for row in grid]
        for j in range(gh):
            for i in range(gw):
                if grid[j][i]:
                    n = sum(grid[y][x] for y in range(max(0, j - 1), min(gh, j + 2))
                            for x in range(max(0, i - 1), min(gw, i + 2))) - 1
                    if n < 2:
                        clean[j][i] = False
        grid = clean

    def sub(i, j):  # noqa: F811 -- from here on, read the cleaned grid
        return grid[j][i]

    lines = []
    for r in range(rows):
        s = ""
        for c in range(cols):
            i0, j0 = c * sx, r * sy
            if a.mode == "braille":
                bits = 0
                for dx, dy, b in ((0, 0, 1), (0, 1, 2), (0, 2, 4), (1, 0, 8),
                                  (1, 1, 16), (1, 2, 32), (0, 3, 64), (1, 3, 128)):
                    if sub(i0 + dx, j0 + dy):
                        bits |= b
                s += chr(0x2800 + bits) if bits else " "
            elif a.mode == "blocks":
                t, b = sub(i0, j0), sub(i0, j0 + 1)
                s += "█" if t and b else "▀" if t else "▄" if b else " "
            else:
                s += "•" if (sub(i0, j0) or sub(i0, j0 + 1)) else " "
        lines.append(s.rstrip())
    while lines and not lines[-1]:
        lines.pop()
    while lines and not lines[0]:
        lines.pop(0)
    text = "\n".join(lines)
    if a.fastfetch:
        text = "$1" + text
    if a.output:
        open(a.output, "w", encoding="utf-8").write(text + "\n")
    else:
        print(text)


if __name__ == "__main__":
    main()
