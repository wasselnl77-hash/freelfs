#!/bin/sh
# Headless flcomp test of the keyboard shortcuts: Super+Left/Right/Up/Down, the Alt-Tab
# switcher (Alt-Shift-Tab), Super+E, Super+T and Ctrl-Alt-T, and Print (PNG in ~/pictures).
# Same harness as run.sh; HOME and PATH point into a scratch directory, nothing outside it is
# touched. `make check-keys`; with KEYS_OUT=DIR it also keeps screenshots there (PPM).
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
dir=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$dir"' EXIT
export XDG_RUNTIME_DIR=$dir HOME=$dir
export FLCOMP_WALLPAPER= FLCOMP_HEADLESS=640x400 FLCOMP_PANEL= FLCOMP_SESSION= FLCOMP_SCREENSHOT=$dir/shot.ppm
export FLCOMP_TEST_FIFO=$dir/in FLCOMP_KEYBOARD_CONF=$dir/none
# no screen locker (a system with fllock installed would lock on Super+L and ignore Print)
export FLCOMP_LOCK=
mkdir "$dir/bin"
for p in flterm flfiles; do
	printf '#!/bin/sh\ntouch "%s/ran-%s"\n' "$dir" "$p" >"$dir/bin/$p"
	chmod +x "$dir/bin/$p"
done
PATH=$dir/bin:$PATH
export PATH
mkfifo "$dir/in"
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

# no Snappel, even if the system the test runs on has one: Print uses the built-in screenshot
FLCOMP_SNAPPEL=snappel-not-installed ./flcomp 2>"$dir/log" &
comp=$!
i=0
while [ ! -S "$dir/wayland-0" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
[ -S "$dir/wayland-0" ] || { cat "$dir/log"; echo "FAIL flcomp did not start"; exit 1; }

shot() { kill -USR1 $comp; sleep 0.4; [ -n "$KEYS_OUT" ] && cp "$dir/shot.ppm" "$KEYS_OUT/$1.ppm"; return 0; }
pixel() {
	python3 -I - "$@" <<'PY'
import sys
f = open(sys.argv[1], 'rb'); x = int(sys.argv[2]); y = int(sys.argv[3])
assert f.readline() == b'P6\n'; w, h = map(int, f.readline().split()); f.readline()
d = f.read(); o = (y * w + x) * 3
print('%02x%02x%02x' % tuple(d[o:o + 3]))
PY
}
expect_px() {	# name x y colour
	got=$(pixel "$dir/shot.ppm" "$2" "$3")
	[ "$got" = "$4" ] && ok "$1" || bad "$1: ($2,$3) is $got, wanted $4"
}
send() { echo "$1" >"$dir/in"; sleep 0.12; }
# chord MOD KEY...: hold the modifiers (key codes), tap the last key, release
tap() { send "key $1 1"; }
untap() { send "key $1 0"; }
chord() {	# chord "125" 105
	for m in $1; do tap "$m"; done
	tap "$2"; untap "$2"
	for m in $1; do untap "$m"; done
}
SUPER=125 ALT=56 CTRL=29 SHIFT=42 TAB=15
LEFT=105 RIGHT=106 UP=103 DOWN=108 KE=18 KT=20 KL=38 PRINT=99

./tests/shmclient -l -s 100 >"$dir/listen.out" &
./tests/shmclient -p 40 -c 272c33 -s 100 >"$dir/panel.out" &
sleep 0.5
./tests/shmclient -w 200 -h 100 -c 3f9b63 -t one -s 100 >"$dir/w1.out" &
sleep 0.4
./tests/shmclient -w 150 -h 80 -c ff0000 -t two -s 100 >"$dir/w2.out" &
sleep 0.5
shot start
# work area is 640 x 360 (the 40 px panel at the bottom); window two is focused

# ---- Super+arrows ----------------------------------------------------------
menus=$(grep -c menu_toggle "$dir/listen.out")
chord $SUPER $LEFT
sleep 0.3
shot snap-left
grep -q "configure 320x360" "$dir/w2.out" && ok "Super+Left asks for the left half (320x360)" || bad "Super+Left configure: $(grep configure "$dir/w2.out" | tail -1)"
expect_px "left half: top-left corner" 2 2 ff0000
expect_px "left half: right edge of the half" 317 300 ff0000
expect_px "left half: right half is free" 400 100 1d2125
chord $SUPER $RIGHT
sleep 0.3
shot back-from-left
chord $SUPER $RIGHT
sleep 0.3
shot snap-right
expect_px "Super+Right after Super+Left goes back to the old place, then right half" 637 357 ff0000
expect_px "right half: left half is free" 100 300 1d2125
chord $SUPER $UP
sleep 0.3
shot maximize
grep -q "configure 640x360 states: 1" "$dir/w2.out" && ok "Super+Up maximizes" || bad "Super+Up configure: $(grep configure "$dir/w2.out" | tail -1)"
expect_px "maximized fills the work area" 2 2 ff0000
chord $SUPER $DOWN
sleep 0.3
shot restore
grep -q "configure 150x80" "$dir/w2.out" && ok "Super+Down restores the size from before the snap" || bad "restore configure: $(grep configure "$dir/w2.out" | tail -2)"
expect_px "restored: desktop is free again" 600 300 1d2125
chord $SUPER $DOWN
sleep 0.3
grep -q "window_changed id=2 title=two focused=[01] minimized=1" "$dir/listen.out" && ok "Super+Down on a normal window minimizes it" || bad "Super+Down minimize: $(grep 'id=2' "$dir/listen.out" | tail -1)"
[ "$(grep -c menu_toggle "$dir/listen.out")" = "$menus" ] && ok "no menu toggled by Super combinations" || bad "Super combinations toggled the menu"
send "key 125 1"
send "key 125 0"
sleep 0.2
[ "$(grep -c menu_toggle "$dir/listen.out")" = "$((menus + 1))" ] && ok "Super alone still toggles the menu" || bad "Super alone"

# ---- Alt-Tab switcher -------------------------------------------------------
./tests/shmclient -w 120 -h 70 -c 0000ff -t three -s 100 >"$dir/w3.out" &
sleep 0.5
# three windows (one minimized): the row has two tiles, 176 px each: box 384 x 144 at (128, 108)
tap $ALT
tap $TAB; untap $TAB
sleep 0.3
shot switcher
expect_px "switcher: box line" 128 108 3a4048
expect_px "switcher: box body" 132 112 272c33
expect_px "switcher: second tile (picked) has the accent frame" 324 120 3f9b63
expect_px "switcher: first tile has the plain frame" 140 120 3a4048
tap $SHIFT
tap $TAB; untap $TAB
untap $SHIFT
sleep 0.3
shot switcher-back
expect_px "Alt-Shift-Tab goes back: first tile picked" 140 120 3f9b63
untap $ALT
sleep 0.3
shot switcher-gone
expect_px "switcher gone after Alt is released" 132 112 1d2125
grep -q "window_changed id=[0-9]* title=three focused=1" "$dir/listen.out" && ok "windows keep focus order while switching" || bad "focus after switching"

# ---- starting programs -----------------------------------------------------
chord $SUPER $KE
sleep 0.5
[ -e "$dir/ran-flfiles" ] && ok "Super+E starts flfiles" || bad "Super+E"
chord $SUPER $KT
sleep 0.5
[ -e "$dir/ran-flterm" ] && ok "Super+T starts flterm" || bad "Super+T"
rm -f "$dir/ran-flterm"
send "key $CTRL 1"
chord "$ALT" $KT
send "key $CTRL 0"
sleep 0.5
[ -e "$dir/ran-flterm" ] && ok "Ctrl-Alt-T starts flterm" || bad "Ctrl-Alt-T"
chord $SUPER $KL
sleep 0.2
kill -0 $comp 2>/dev/null && ok "Super+L without fllock does nothing" || bad "compositor died on Super+L"

# ---- Print ---------------------------------------------------------------
send "move 500 150"
chord "" $PRINT
sleep 0.6
shot printed
png=$(ls "$dir"/pictures/screenshot-*.png 2>/dev/null | head -1)
[ -n "$png" ] && ok "Print saved $(basename "$png")" || bad "Print: no file in $dir/pictures"
case "$(basename "$png")" in
	screenshot-[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]-[0-9][0-9][0-9][0-9][0-9][0-9].png) ok "file name has the date and time" ;;
	*) bad "file name: $png" ;;
esac
expect_px "indicator shown after saving (box body)" 210 290 272c33
# decode the PNG (every filter type) and look at some pixels: no pointer, no indicator in it
res=$(python3 -I - "$png" <<'PY'
import struct, sys, zlib
b = open(sys.argv[1], 'rb').read()
assert b[:8] == b'\x89PNG\r\n\x1a\n'
pos, idat = 8, b''
while pos < len(b):
    n, t = struct.unpack('>I4s', b[pos:pos + 8])
    body = b[pos + 8:pos + 8 + n]
    if t == b'IHDR':
        w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
    elif t == b'IDAT':
        idat += body
    pos += 12 + n
assert depth == 8 and ctype == 2
raw = zlib.decompress(idat)
bpp, stride = 3, w * 3
rows, prev = [], bytes(stride)
for y in range(h):
    f = raw[y * (stride + 1)]
    cur = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
    for i in range(stride):
        a = cur[i - bpp] if i >= bpp else 0
        u = prev[i]
        c = prev[i - bpp] if i >= bpp else 0
        if f == 1: cur[i] = (cur[i] + a) & 255
        elif f == 2: cur[i] = (cur[i] + u) & 255
        elif f == 3: cur[i] = (cur[i] + (a + u) // 2) & 255
        elif f == 4:
            p = a + u - c
            pa, pb, pc = abs(p - a), abs(p - u), abs(p - c)
            pr = a if pa <= pb and pa <= pc else (u if pb <= pc else c)
            cur[i] = (cur[i] + pr) & 255
    rows.append(bytes(cur)); prev = bytes(cur)
def px(x, y): return '%02x%02x%02x' % tuple(rows[y][x * 3:x * 3 + 3])
print(w, h, px(300, 380), px(210, 290), px(500, 150), px(620, 20))
PY
)
set -- $res
[ "$1 $2" = "640 400" ] && ok "PNG is 640x400 RGB" || bad "PNG size: $res"
[ "$3" = "272c33" ] && ok "PNG shows the panel" || bad "PNG panel pixel $3"
[ "$4" != "272c33" ] && ok "PNG has no indicator box" || bad "PNG contains the indicator"
[ "$5" = "1d2125" ] && ok "PNG has no pointer" || bad "PNG pixel under the pointer: $5"
[ "$6" = "1d2125" ] && ok "PNG shows the desktop" || bad "PNG desktop pixel $6"
[ "$(ls "$dir"/pictures | grep -c '\.part$')" = 0 ] && ok "no half-written file left" || bad "left a .part file"

kill $comp 2>/dev/null
echo "flcomp log:"; sed 's/^/  /' "$dir/log"
[ $fails = 0 ] && echo "all tests passed" || echo "$fails test(s) failed"
exit $fails
