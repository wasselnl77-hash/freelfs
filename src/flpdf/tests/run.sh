#!/bin/sh
# Headless check, no compositor: writes a three-page PDF with bookmarks (python3,
# standard library only), draws the first frame to a PPM, and checks that a
# search highlight changes the picture and that a broken file is reported.
# usage: tests/run.sh [flpdf]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
bin=${1:-$here/../flpdf}
tmp=$(mktemp -d)
trap 'rm -r "$tmp"' EXIT
export HOME=$tmp XDG_CONFIG_HOME=$tmp/.config

python3 -I - "$tmp/t.pdf" <<'PY'
import sys
objs = []
def add(s):
    objs.append(s)
    return len(objs)
pages = [3, 5, 7]            # object numbers fixed below
# 1 catalog, 2 pages, 3.. page/content pairs, then outline
add("<< /Type /Catalog /Pages 2 0 R /Outlines 9 0 R >>")
add("<< /Type /Pages /Kids [3 0 R 5 0 R 7 0 R] /Count 3 >>")
texts = ["First page, nothing special", "Second page has the needle in it", "Third page"]
for i, t in enumerate(texts):
    add("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents %d 0 R /Resources << /Font << /F1 << /Type /Font /Subtype /Type1 /BaseFont /Helvetica >> >> >> >>" % (4 + 2 * i))
    body = "BT /F1 24 Tf 72 700 Td (%s) Tj ET" % t
    add("<< /Length %d >>\nstream\n%s\nendstream" % (len(body), body))
add("<< /Type /Outlines /First 10 0 R /Last 11 0 R /Count 2 >>")
add("<< /Title (Start) /Parent 9 0 R /Next 11 0 R /Dest [3 0 R /Fit] >>")
add("<< /Title (Second) /Parent 9 0 R /Prev 10 0 R /Dest [5 0 R /Fit] >>")
out = "%PDF-1.4\n"
offs = []
for n, o in enumerate(objs, 1):
    offs.append(len(out))
    out += "%d 0 obj\n%s\nendobj\n" % (n, o)
x = len(out)
out += "xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
for o in offs:
    out += "%010d 00000 n \n" % o
out += "trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, x)
open(sys.argv[1], "w").write(out)
PY

FLUI_PPM="$tmp/a.ppm" FLUI_PPM_DELAY=300 "$bin" "$tmp/t.pdf"
FLUI_PPM="$tmp/b.ppm" FLUI_PPM_DELAY=300 "$bin" --find needle "$tmp/t.pdf"
head -c 2 "$tmp/a.ppm" | grep -q P6
cmp -s "$tmp/a.ppm" "$tmp/b.ppm" && { echo "FAIL: a search changed nothing"; exit 1; }
# white page on a dark window: both must be in the first frame
python3 -I - "$tmp/a.ppm" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
w = d.count(b'\xff\xff\xff')
if w < 20000:
    sys.exit("FAIL: no page in the first frame (%d white pixels)" % w)
PY
echo garbage > "$tmp/bad.pdf"
if FLUI_PPM="$tmp/c.ppm" FLUI_PPM_DELAY=100 "$bin" "$tmp/bad.pdf" 2> "$tmp/err"; then :; fi
grep -q "Cannot open" "$tmp/err" || { echo "FAIL: no error for a broken file"; exit 1; }
echo "flpdf: ok"
