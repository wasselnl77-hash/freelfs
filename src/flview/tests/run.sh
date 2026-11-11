#!/bin/sh
# Headless check, no compositor: three generated PNGs, first frame to a PPM,
# then the keys Right, W, Delete run in the first frame (FLVIEW_KEYS): the
# wallpaper setting must be written and the picture must be in the Trash.
# Needs python3 (standard library only). usage: tests/run.sh [flview]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
bin=${1:-$here/../flview}
tmp=$(mktemp -d)
trap 'rm -r "$tmp"' EXIT
export HOME=$tmp/home XDG_CONFIG_HOME=$tmp/home/.config XDG_DATA_HOME=$tmp/home/.local/share
mkdir -p "$HOME" "$tmp/pics"

python3 -I - "$tmp/pics" <<'PY'
import sys, zlib, struct
def png(path, w, h, f):
    raw = b''.join(b'\0' + bytes(c for x in range(w) for c in f(x, y)) for y in range(h))
    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))
d = sys.argv[1]
png(d + '/img1.png', 320, 200, lambda x, y: (x * 255 // 320, y * 255 // 200, 80))
png(d + '/img2.png', 200, 320, lambda x, y: (200, x * 255 // 200, y * 255 // 320))
png(d + '/img10.png', 400, 300, lambda x, y: (60, 120, (x + y) * 255 // 700))
PY

FLUI_PPM="$tmp/a.ppm" FLUI_PPM_DELAY=300 "$bin" "$tmp/pics/img1.png"
head -c 2 "$tmp/a.ppm" | grep -q P6
# not just the background: the picture and the strip add colours
n=$(python3 -I - "$tmp/a.ppm" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
print(len(set(d[15::97])))
PY
)
[ "$n" -gt 40 ] || { echo "FAIL: first frame looks empty ($n colours)"; exit 1; }

FLVIEW_KEYS=Right,w,Delete FLUI_PPM="$tmp/b.ppm" FLUI_PPM_DELAY=300 "$bin" "$tmp/pics/img1.png"
[ ! -e "$tmp/pics/img2.png" ] || { echo "FAIL: img2.png was not trashed"; exit 1; }
[ -e "$XDG_DATA_HOME/Trash/files/img2.png" ] || { echo "FAIL: not in the Trash"; exit 1; }
grep -q "^Path=.*img2.png" "$XDG_DATA_HOME/Trash/info/img2.png.trashinfo"
grep -q "^wallpaper=$XDG_DATA_HOME/freelfs/wallpapers/img2.png$" "$XDG_CONFIG_HOME/freelfs/desktop.conf" ||
	{ echo "FAIL: wallpaper not set"; exit 1; }
[ -s "$XDG_DATA_HOME/freelfs/wallpapers/img2.png" ]
echo "flview: ok"
