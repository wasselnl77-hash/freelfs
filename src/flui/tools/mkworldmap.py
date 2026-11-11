#!/usr/bin/env python3
# mkworldmap.py -- turn Natural Earth 1:110m shapefiles into worldmap_data.c
# SPDX-License-Identifier: MIT
#
# Source data (public domain, no attribution required; "Made with Natural
# Earth. Free vector and raster map data @ naturalearthdata.com"):
#   https://naciscdn.org/naturalearth/110m/physical/ne_110m_land.zip
#   https://naciscdn.org/naturalearth/110m/cultural/ne_110m_admin_0_countries.zip
# (Natural Earth 4.1.0; terms: https://www.naturalearthdata.com/about/terms-of-use/)
#
# Usage (stdlib only, run isolated):
#   python3 -I mkworldmap.py LAND.shp COUNTRIES.shp > worldmap_data.c
#
# Output: two int16 arrays in units of 0.05 degrees, longitude then latitude.
#   fl_wm_land:    rings (closed polygons), each "n x0 y0 ... xn-1 yn-1", ends with n = 0
#   fl_wm_borders: open polylines in the same format; only edges that two countries share
# Rings are simplified (Douglas-Peucker, 0.06 degrees), which is far below one
# pixel at any size the widget draws.
import struct, sys, math

Q = 20           # units per degree
TOL = 0.06       # degrees


def shp(path):
    d = open(path, 'rb').read()
    pos, out = 100, []
    while pos < len(d):
        _, cl = struct.unpack('>ii', d[pos:pos + 8])
        pos += 8
        c = d[pos:pos + cl * 2]
        pos += cl * 2
        if struct.unpack('<i', c[:4])[0] == 0:
            out.append([])
            continue
        np_, npts = struct.unpack('<ii', c[36:44])
        parts = list(struct.unpack('<%di' % np_, c[44:44 + 4 * np_])) + [npts]
        o = 44 + 4 * np_
        pts = [struct.unpack('<2d', c[o + 16 * i:o + 16 * i + 16]) for i in range(npts)]
        out.append([pts[parts[i]:parts[i + 1]] for i in range(np_)])
    return out


def dp(pts, tol, closed):
    """Douglas-Peucker; a closed ring keeps its first point."""
    if len(pts) < 3:
        return pts
    keep = [False] * len(pts)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        a, b = stack.pop()
        ax, ay = pts[a]
        bx, by = pts[b]
        dx, dy = bx - ax, by - ay
        L = math.hypot(dx, dy)
        best, bi = -1, -1
        for i in range(a + 1, b):
            px, py = pts[i]
            if L == 0:
                dist = math.hypot(px - ax, py - ay)
            else:
                dist = abs(dx * (ay - py) - (ax - px) * dy) / L
            if dist > best:
                best, bi = dist, i
        if best > tol:
            keep[bi] = True
            stack += [(a, bi), (bi, b)]
    return [p for p, k in zip(pts, keep) if k]


def quant(pts):
    out = []
    for x, y in pts:
        q = (int(round(x * Q)), int(round(y * Q)))
        if not out or out[-1] != q:
            out.append(q)
    return out


def emit(name, rings, f):
    f.write("const int16_t %s[] = {\n" % name)
    total = 0
    line = ""
    for r in rings:
        vals = [len(r)] + [v for p in r for v in p]
        total += len(vals)
        for v in vals:
            s = "%d," % v
            if len(line) + len(s) > 118:
                f.write(line + "\n")
                line = ""
            line += s
        f.write(line + "\n")
        line = ""
    f.write("0};\n")
    return total + 1


def main():
    land = shp(sys.argv[1])
    countries = shp(sys.argv[2])

    rings = []
    for rec in land:
        for ring in rec:
            r = dp(ring[:-1], TOL, True)           # drop the repeated closing point
            q = quant(r)
            if len(q) > 1 and q[0] == q[-1]:
                q.pop()
            if len(q) >= 3:
                rings.append(q)

    # borders: undirected edges used by two different countries
    key = lambda p: (round(p[0] * 1e4), round(p[1] * 1e4))
    edges = {}
    for ci, rec in enumerate(countries):
        for ring in rec:
            for a, b in zip(ring, ring[1:]):
                ka, kb = key(a), key(b)
                if ka == kb:
                    continue
                e = (ka, kb) if ka < kb else (kb, ka)
                edges.setdefault(e, set()).add(ci)
    pos = {}
    for rec in countries:
        for ring in rec:
            for p in ring:
                pos[key(p)] = p
    shared = [e for e, cs in edges.items() if len(cs) >= 2]
    adj = {}
    for a, b in shared:
        adj.setdefault(a, []).append(b)
        adj.setdefault(b, []).append(a)
    seen = set()

    def walk(start, nxt):
        line = [start, nxt]
        seen.add((min(start, nxt), max(start, nxt)))
        while len(adj[line[-1]]) == 2:
            a, b = line[-2], line[-1]
            n = [x for x in adj[b] if x != a][0]
            e = (min(b, n), max(b, n))
            if e in seen:
                break
            seen.add(e)
            line.append(n)
        return line

    lines = []
    for v in sorted(adj):                       # open ends and junctions first
        if len(adj[v]) != 2:
            for n in adj[v]:
                if (min(v, n), max(v, n)) not in seen:
                    lines.append(walk(v, n))
    for v in sorted(adj):                       # then the loops
        for n in adj[v]:
            if (min(v, n), max(v, n)) not in seen:
                lines.append(walk(v, n))
    borders = []
    for l in lines:
        q = quant(dp([pos[k] for k in l], TOL, False))
        if len(q) >= 2:
            borders.append(q)

    f = sys.stdout
    f.write("/* worldmap_data.c -- GENERATED by tools/mkworldmap.py, do not edit.\n"
            " * Natural Earth 4.1.0, 1:110m land and admin-0 countries (shared borders only).\n"
            " * Public domain. https://www.naturalearthdata.com/about/terms-of-use/\n"
            " * Units: 0.05 degrees (x = longitude, y = latitude). SPDX-License-Identifier: CC0-1.0 */\n"
            "#include <stdint.h>\n\n"
            )
    a = emit("fl_wm_land", rings, f)
    b = emit("fl_wm_borders", borders, f)
    sys.stderr.write("land: %d rings, %d values; borders: %d lines, %d values\n" % (len(rings), a, len(borders), b))


main()
