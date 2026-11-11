#!/bin/sh
# Snappel against a headless flcomp: no display, no root, HOME is a scratch directory.
# Needs ../flcomp built (flcomp, tests/shmclient) and python3. `make check`;
# SNAPPEL_OUT=DIR keeps the compositor's screenshots there (PPM).
#   - snappel --full: the file, the notification, --output (exit 0, nothing in ~/pictures)
#   - --region: the overlay, a selection, Save; Esc is exit 1 with --output; Copy puts image/png on
#     the clipboard; --window: a click on a window gets that window alone
#   - the Snappel window: Full screen, then Box in the editor, Save: the red box is in the file
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
here=$(pwd)
comp=$here/../flcomp
dir=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$dir"' EXIT
export XDG_RUNTIME_DIR=$dir HOME=$dir XDG_CONFIG_HOME=$dir/cfg FLUI_CONF=$dir/desktop.conf
mkdir -p "$dir/cfg" "$dir/bin"
unset WAYLAND_DISPLAY
export FLCOMP_WALLPAPER= FLCOMP_HEADLESS=1280x800 FLCOMP_PANEL= FLCOMP_SESSION= FLCOMP_SCREENSHOT=$dir/shot.ppm
export FLCOMP_TEST_FIFO=$dir/in FLCOMP_KEYBOARD_CONF=$dir/none
mkfifo "$dir/in"
# a flnotify that only writes down what it was asked
cat >"$dir/bin/flnotify" <<EOS
#!/bin/sh
echo "\$@" >>"$dir/notify.log"
EOS
chmod +x "$dir/bin/flnotify"
export PATH=$dir/bin:$here:$PATH
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

"$comp/flcomp" 2>"$dir/log" &
cpid=$!
i=0
while [ ! -S "$dir/wayland-0" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
[ -S "$dir/wayland-0" ] || { cat "$dir/log"; echo "FAIL flcomp did not start"; exit 1; }

shot() { kill -USR1 $cpid; sleep 0.5; [ -n "$SNAPPEL_OUT" ] && cp "$dir/shot.ppm" "$SNAPPEL_OUT/$1.ppm"; return 0; }
send() { echo "$1" >"$dir/in"; sleep 0.15; }
click() { send "move $1 $2"; send "button 272 1"; send "button 272 0"; sleep 0.3; }
# png FILE -> "W H amber red" : size and how many pixels have the two colours
png() {
	python3 -I - "$1" <<'PY'
import struct, sys, zlib
b = open(sys.argv[1], 'rb').read()
assert b[:8] == b'\x89PNG\r\n\x1a\n'
pos, idat = 8, b''
while pos < len(b):
    n, t = struct.unpack('>I4s', b[pos:pos + 8]); body = b[pos + 8:pos + 8 + n]
    if t == b'IHDR': w, h, depth, ctype = struct.unpack('>IIBB', body[:10])
    elif t == b'IDAT': idat += body
    pos += 12 + n
assert depth == 8 and ctype == 2
raw = zlib.decompress(idat); bpp, stride = 3, w * 3
prev = bytes(stride); amber = red = box = 0
for y in range(h):
    f = raw[y * (stride + 1)]; cur = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
    for i in range(stride):
        a = cur[i - bpp] if i >= bpp else 0; u = prev[i]; c = prev[i - bpp] if i >= bpp else 0
        if f == 1: cur[i] = (cur[i] + a) & 255
        elif f == 2: cur[i] = (cur[i] + u) & 255
        elif f == 3: cur[i] = (cur[i] + (a + u) // 2) & 255
        elif f == 4:
            p = a + u - c; pa, pb, pc = abs(p - a), abs(p - u), abs(p - c)
            cur[i] = (cur[i] + (a if pa <= pb and pa <= pc else (u if pb <= pc else c))) & 255
    prev = bytes(cur)
    for x in range(w):
        px = prev[x * 3:x * 3 + 3]
        amber += px == b'\xe0\xa0\x30'; red += px == b'\xff\x00\x00'; box += px == b'\xe5\x48\x4d'
print(w, h, amber, red, box)
PY
}
shotpx() {	# FILE X Y -> RRGGBB
	python3 -I - "$@" <<'PY'
import sys
f = open(sys.argv[1], 'rb'); x = int(sys.argv[2]); y = int(sys.argv[3])
assert f.readline() == b'P6\n'; w, h = map(int, f.readline().split()); f.readline()
d = f.read(); o = (y * w + x) * 3
print('%02x%02x%02x' % tuple(d[o:o + 3]))
PY
}

"$comp/tests/shmclient" -p 40 -c 272c33 -s 900 >"$dir/panel.out" &
sleep 0.5
# window one (amber, 200 x 100) is at (480, 270)..(680, 370) (first window, no cascade); window two (red,
# 150 x 80) is at (529, 304) and covers part of it
"$comp/tests/shmclient" -w 200 -h 100 -c e0a030 -t one -s 900 >"$dir/w1.out" &
sleep 0.5
"$comp/tests/shmclient" -w 150 -h 80 -c ff0000 -t two -s 900 >"$dir/w2.out" &
sleep 0.6

# ---- --full ---------------------------------------------------------------------------------
snappel --full 2>"$dir/full.err"
rc=$?
f=$(ls "$dir"/pictures/screenshots/snappel-*.png 2>/dev/null | head -1)
[ $rc = 0 ] && [ -n "$f" ] && ok "snappel --full saved $(basename "$f")" || bad "snappel --full: rc=$rc $(cat "$dir/full.err")"
case "$(basename "$f")" in
	snappel-[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]-[0-9][0-9][0-9][0-9][0-9][0-9].png) ok "the name has the date and time" ;;
	*) bad "file name $f" ;;
esac
set -- $(png "$f")
[ "$1 $2" = "1280 800" ] && ok "the picture is the whole desktop, 1280x800" || bad "size $1 x $2"
[ "$3" -gt 5000 ] && [ "$4" -gt 5000 ] && ok "both windows are in it ($3, $4 px)" || bad "windows in the picture: $3 $4"
sleep 0.3
grep -q "Screenshot saved" "$dir/notify.log" && ok "a notification was sent" || bad "notification: $(cat "$dir/notify.log" 2>/dev/null)"
n=$(ls "$dir"/pictures/screenshots | wc -l)
snappel --full --output "$dir/out.png" 2>/dev/null
rc=$?
[ $rc = 0 ] && [ "$(png "$dir/out.png" | cut -d' ' -f1,2)" = "1280 800" ] && ok "--output FILE writes the png there, exit 0" || bad "--output: rc=$rc"
[ "$(ls "$dir"/pictures/screenshots | wc -l)" = "$n" ] && ok "--output leaves ~/pictures alone" || bad "--output saved a second copy"
[ "$(grep -c "Screenshot saved" "$dir/notify.log")" = 1 ] && ok "--output sends no notification" || bad "notification with --output"
snappel --full --output /nonexistent/dir/x.png 2>/dev/null
[ $? != 0 ] && ok "an unwritable --output is an error" || bad "unwritable output exit status"

# ---- --region ---------------------------------------------------------------------------------
snappel --region --output "$dir/region.png" 2>"$dir/region.err" &
spid=$!
sleep 1.2
shot snappel-region-1
n=$(python3 -I - "$dir/shot.ppm" <<'PY'
import sys
f = open(sys.argv[1], 'rb'); f.readline(); w, h = map(int, f.readline().split()); f.readline(); d = f.read()
# the overlay dims the desktop: the empty desktop is 0x1d2125 * 0.55, the panel is hidden
print(sum(1 for i in range(0, len(d), 3) if d[i:i + 3] == bytes([0x0f, 0x12, 0x14])))
PY
)
[ "$n" -gt 100000 ] && ok "the overlay shows the frozen screen, dimmed ($n px)" || bad "overlay not dimmed ($n)"
[ "$(shotpx "$dir/shot.ppm" 600 790)" != "272c33" ] && ok "the overlay covers the panel" || bad "panel still visible under the overlay"
send "move 470 260"
send "button 272 1"
send "move 600 350"
send "move 790 460"
send "button 272 0"
sleep 0.4
shot snappel-region-2
# the selection (470,260)-(790,460) is shown undimmed
[ "$(shotpx "$dir/shot.ppm" 490 300)" = "e0a030" ] && ok "the selection shows the screen at full brightness" || bad "selection pixel $(shotpx "$dir/shot.ppm" 490 300)"
[ "$(shotpx "$dir/shot.ppm" 400 200)" != "e0a030" ] && ok "outside the selection stays dim" || bad "outside the selection"
send "key 28 1"
send "key 28 0"
wait $spid
rc=$?
[ $rc = 0 ] && ok "Enter saves; exit 0" || bad "region save exit $rc: $(cat "$dir/region.err")"
set -- $(png "$dir/region.png")
[ "$1 $2" = "320 200" ] && ok "the file is the selection, 320x200" || bad "region size $1 x $2"
[ "$3" -gt 5000 ] && ok "and has the amber window in it ($3 px)" || bad "region content: amber $3"
sleep 0.3
shot snappel-region-3
[ "$(shotpx "$dir/shot.ppm" 600 790)" = "272c33" ] && ok "the overlay is gone, the panel is back" || bad "overlay left behind"

# cancel
snappel --region --output "$dir/never.png" 2>/dev/null &
spid=$!
sleep 1.2
send "key 1 1"
send "key 1 0"
wait $spid
rc=$?
[ $rc = 1 ] && [ ! -e "$dir/never.png" ] && ok "Esc cancels: exit 1, no file" || bad "cancel: exit $rc"

# copy to the clipboard, then ask for it as image/png
snappel --region 2>/dev/null &
spid=$!
sleep 1.2
send "move 470 260"
send "button 272 1"
send "move 790 460"
send "button 272 0"
sleep 0.4
# the toolbar is below the selection, right aligned: Save, Copy, Edit, Cancel, 76 px each from x = 468
click 586 490
wait $spid
sleep 0.5
SHM_CLIP_MIME=image/png "$comp/tests/shmclient" -w 60 -h 40 -c 222222 -t clipdst -Q -s 3 >"$dir/clip.out" &
sleep 1.5
grep -q "clip: [1-9][0-9]* bytes magic=89504e47" "$dir/clip.out" && ok "Copy: image/png is on the clipboard ($(grep clip: "$dir/clip.out" | cut -d' ' -f2) bytes)" || bad "clipboard: $(cat "$dir/clip.out" | grep -E 'clip|selection')"
grep -q "Screenshot copied" "$dir/notify.log" && ok "Copy tells you" || bad "copy notification"
sleep 2

# ---- --window ---------------------------------------------------------------------------------
snappel --window --output "$dir/win.png" 2>"$dir/win.err" &
spid=$!
sleep 1.2
send "move 490 320"
shot snappel-window-1
click 490 320
wait $spid
rc=$?
set -- $(png "$dir/win.png")
[ $rc = 0 ] && [ "$1 $2" = "200 100" ] && ok "--window: a click on the amber window gives that window, 200x100" || bad "--window: rc=$rc size $1 x $2: $(cat "$dir/win.err")"
[ "$3" -gt 19000 ] && ok "and it is all window ($3 px of 20000)" || bad "window content $3"
send "move 5 5"

# ---- the Snappel window -----------------------------------------------------------------------
# a fresh compositor, so the window is the first one placed: (60, 0)
kill $cpid
wait $cpid 2>/dev/null
rm -f "$dir/wayland-0" "$dir/wayland-0.lock"
"$comp/flcomp" 2>>"$dir/log" &
cpid=$!
i=0
while [ ! -S "$dir/wayland-0" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
"$comp/tests/shmclient" -p 40 -c 272c33 -s 900 >"$dir/panel2.out" &
sleep 0.5
snappel 2>"$dir/gui.err" &
gpid=$!
sleep 1.5
shot snappel-start
# the window is 1040 x 700 at (60, 0); the Full screen card is at (304, 345) inside it
click 364 345
sleep 1
shot snappel-editor
# Box in the toolbar (105, 59), the picture, then Save at (916, 676)
click 165 59
send "move 700 300"
send "button 272 1"
send "move 800 360"
send "move 900 420"
send "button 272 0"
sleep 0.3
shot snappel-editor-box
click 976 676
sleep 0.5
f=$(ls -t "$dir"/pictures/screenshots/snappel-*.png | head -1)
set -- $(png "$f")
[ "$1 $2" = "1280 800" ] && [ "$5" -gt 200 ] && ok "editor: Box drawn and saved (red frame, $5 px, in a 1280x800 file)" || bad "editor save: $1 x $2, box px $5, $(ls "$dir"/pictures/screenshots)"
n=$(ls "$dir"/pictures/screenshots | wc -l)
# Undo takes the box away again: Undo (top right), Save
click 1060 59
click 976 676
f=$(ls -t "$dir"/pictures/screenshots/snappel-*.png | head -1)
set -- $(png "$f")
[ "$5" = 0 ] && ok "Undo removes the annotation (no red left)" || bad "undo: $5 px"
kill $gpid 2>/dev/null

kill $cpid 2>/dev/null
echo "flcomp log:"; sed 's/^/  /' "$dir/log" | tail -5
[ $fails = 0 ] && echo "all tests passed" || echo "$fails test(s) failed"
exit $fails
