# ppmcheck.py FILE flat|legacy -- checks a flsplash --render picture (test helper of build/m20, run in the guest)
import re, sys
d = open(sys.argv[1], 'rb').read()
m = re.match(rb'P6\s+(\d+)\s+(\d+)\s+255\s', d)
w, h = int(m.group(1)), int(m.group(2))
p = d[m.end():]
bg = bytes.fromhex('0d1110')
px = lambda x, y: p[(y * w + x) * 3:(y * w + x) * 3 + 3]
corners = [px(0, 0), px(w - 1, 0), px(0, h - 1), px(w - 1, h - 1)]
cols = len(set(p[i:i + 3] for i in range(0, len(p) - 2, 3)))
other = sum(1 for i in range(0, len(p) - 2, 3) if p[i:i + 3] != bg)
print(sys.argv[2], 'corners', [c.hex() for c in corners], 'colours', cols, 'non-background', other)
if sys.argv[2] == 'flat':
    assert all(c == bg for c in corners), 'corner is not #0d1110'
    assert other > 2000, 'no logo'
else:
    assert corners[0] != bg, 'legacy corner has the flat colour'
    assert cols > 500, 'legacy picture has few colours'
