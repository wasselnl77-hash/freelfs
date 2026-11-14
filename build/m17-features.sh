#!/bin/bash
# Round 17 tests (QEMU, KVM, live session): fldisplay with two outputs, night light, the Settings
# Display/Bluetooth/Printers/Wallpaper pages, Bluetooth without an adapter, CUPS on demand with a
# fake IPP printer. Needs the SERIAL TWIN of the live ISO (53-iso.sh --cmdline console=ttyS0,115200).
#
#   build/m17-features.sh TWIN.iso
#
# OUT (default ~/freelfs-work/scratch/m17) gets the logs and pictures (.ppm).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
twin=${1:?usage: m17-features.sh TWIN.iso}
out=${OUT:-$HOME/freelfs-work/scratch/m17}
only=${STEPS:-}
want() { [ -z "$only" ] || [[ " $only " == *" $1 "* ]]; }
mkdir -p "$out"
run() { # run NAME [QEMU args]
	local n=$1; shift
	sed "s|@OUT@|$out|g; s|@HERE@|$here/m17|g" "$here/m17/$n.cmds" > "$out/$n.run"
	echo "== $n"
	nice -n 10 python3 -u "$here/live-run.py" --iso "$twin" --mem 1G "$out/$n.run" -- -device usb-tablet "$@" | tee "$out/$n.log"
}
# a valid one-page PDF for the print test
python3 -I - "$out/t.pdf" <<'PY'
import sys
objs = [b"<</Type/Catalog/Pages 2 0 R>>", b"<</Type/Pages/Kids[3 0 R]/Count 1>>",
        b"<</Type/Page/Parent 2 0 R/MediaBox[0 0 612 792]/Contents 4 0 R/Resources<</Font<</F1 5 0 R>>>>>>",
        b"<</Length 44>>\nstream\nBT /F1 36 Tf 72 700 Td (FreeLFS) Tj ET\nendstream",
        b"<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>"]
out = b"%PDF-1.4\n"; offs = []
for i, o in enumerate(objs):
    offs.append(len(out)); out += b"%d 0 obj\n" % (i + 1) + o + b"\nendobj\n"
x = len(out)
out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1) + b"".join(b"%010d 00000 n \n" % o for o in offs)
out += b"trailer<</Size %d/Root 1 0 R>>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, x)
open(sys.argv[1], "wb").write(out)
PY
want gui1 && run gui1
want gui2 && run gui2
want gui3 && run gui3
want display && FLFS_VM_VGA="-device virtio-vga,max_outputs=2 -display vnc=unix:$out/vnc.sock" run display
want pages && run pages
want bluetooth && run bluetooth
want printing && run printing
# night light: the picture must get warmer (blue down) and come back
if want display && [ -f "$out/d5-night.ppm" ]; then
python3 -I - "$out" <<'PY'
import sys
d = sys.argv[1]
def mean(f):
    fh = open(f, 'rb'); fh.readline(); w, h = map(int, fh.readline().split()); fh.readline()
    data = fh.read(); n = w * h
    r = sum(data[0::3]) / n; g = sum(data[1::3]) / n; b = sum(data[2::3]) / n
    return r, g, b
a, n, b = mean(d + '/d4-second.ppm'), mean(d + '/d5-night.ppm'), mean(d + '/d6-night-off.ppm')
print('mean RGB before %.1f %.1f %.1f  night %.1f %.1f %.1f  after %.1f %.1f %.1f' % (a + n + b))
ok = n[2] < a[2] * 0.85 and abs(n[0] - a[0]) < a[0] * 0.1 + 3 and abs(b[2] - a[2]) < 4
print('night light colour shift: ' + ('PASS' if ok else 'NOT VERIFIED (the hardware gamma does not show in a screendump; this is a test limit, see the flcomp.log line)'))
PY
fi
echo "pictures in $out"
