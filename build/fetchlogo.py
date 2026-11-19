#!/usr/bin/env python3
"""Generate the FreeLFS logo as fastfetch text logos (config/fastfetch/).

The mark is docs/logo.svg: a 20x20 viewBox with three bars, rotated 6 degrees
clockwise about (10, 10). The bars are rasterised by supersampling and written
as two text variants:

  freelfs-blocks.txt   half-block characters, 22 columns x 9 rows (each
                       character cell is two vertical pixels)
  freelfs-braille.txt  braille dots, 18 columns x 8 rows (each cell is 2x4 dots)

Every line starts with the fastfetch colour marker $1, so the logo takes the
first color of config/fastfetch/config.jsonc (FreeLFS green).

Python 3 standard library only. Run from anywhere:  python3 build/fetchlogo.py
"""

import math
import os

BARS = [(3, 3, 14, 3.4), (3, 8.3, 10, 3.4), (3, 13.6, 6, 3.4)]  # x, y, w, h
ANGLE = math.radians(6)  # SVG rotate(6 10 10): clockwise on screen
CENTRE = 10.0
VIEW = 20.0
SS = 4  # supersampling per axis

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "config", "fastfetch")

BLOCK_CHARS = {(0, 0): " ", (1, 0): "▀", (0, 1): "▄", (1, 1): "█"}  # top, bottom
BRAILLE_BASE = 0x2800
# braille dot bit for (column, row) inside a 2x4 cell
BRAILLE_BIT = {(0, 0): 0x01, (0, 1): 0x02, (0, 2): 0x04, (1, 0): 0x08,
               (1, 1): 0x10, (1, 2): 0x20, (0, 3): 0x40, (1, 3): 0x80}


def inside_logo(vx, vy):
    """True if view point (vx, vy) lies in one of the rotated bars."""
    # inverse of the clockwise rotation: rotate the point back by -6 degrees
    dx, dy = vx - CENTRE, vy - CENTRE
    c, s = math.cos(-ANGLE), math.sin(-ANGLE)
    ux = CENTRE + dx * c - dy * s
    uy = CENTRE + dx * s + dy * c
    for x, y, w, h in BARS:
        if x <= ux <= x + w and y <= uy <= y + h:
            return True
    return False


def raster(width, height, side):
    """Grid of booleans, width x height, with the 20x20 logo centred in a side x side square."""
    scale = side / VIEW
    ox = (width - side) / 2.0
    oy = (height - side) / 2.0
    grid = []
    for j in range(height):
        row = []
        for i in range(width):
            hits = 0
            for b in range(SS):
                for a in range(SS):
                    px = i + (a + 0.5) / SS
                    py = j + (b + 0.5) / SS
                    if inside_logo((px - ox) / scale, (py - oy) / scale):
                        hits += 1
            row.append(hits >= SS * SS / 2.0)
        grid.append(row)
    return grid


def blocks(grid):
    lines = []
    for r in range(0, len(grid), 2):
        top, bot = grid[r], grid[r + 1]
        line = "".join(BLOCK_CHARS[(int(top[c]), int(bot[c]))] for c in range(len(top)))
        lines.append(line)
    return lines


def braille(grid):
    lines = []
    for r in range(0, len(grid), 4):
        line = []
        for c in range(0, len(grid[0]), 2):
            code = 0
            for dy in range(4):
                for dx in range(2):
                    if grid[r + dy][c + dx]:
                        code |= BRAILLE_BIT[(dx, dy)]
            line.append(chr(BRAILLE_BASE + code))
        lines.append("".join(line))
    return lines


def write(name, lines):
    path = os.path.join(OUT, name)
    with open(path, "w", encoding="utf-8") as f:
        for line in lines:
            f.write("$1" + line + "\n")
    print("wrote", os.path.normpath(path), len(lines), "rows")


def main():
    os.makedirs(OUT, exist_ok=True)
    # half blocks: 22 x 18 pixels, logo 18 x 18 pixels (9 rows of cells)
    write("freelfs-blocks.txt", blocks(raster(22, 18, 18)))
    # braille: 36 x 32 dots, logo 32 x 32 dots (8 rows of cells, 18 columns)
    write("freelfs-braille.txt", braille(raster(36, 32, 32)))


if __name__ == "__main__":
    main()
