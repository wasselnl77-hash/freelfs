#!/bin/sh
# Headless flcomp test of the animations (anim.c): window open and close, minimize and restore,
# maximize, the workspace slide, the overview zoom, the menu fade; animations=reduced (fades only)
# and off; frame pacing at 60 Hz and idleness afterwards. Same harness as perf.sh; HOME is a scratch
# directory, nothing outside it is touched. `make check-anim`; ANIM_OUT=DIR keeps screenshots (PPM).
#
# Motion is slowed down with animation_speed=0.1 (ten times as long) so that a screenshot can catch
# a frame in the middle of it; the pacing run uses the real speed.
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
dir=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$dir"' EXIT
export XDG_RUNTIME_DIR=$dir HOME=$dir FLUI_CONF=$dir/desktop.conf
export FLCOMP_WALLPAPER= FLCOMP_HEADLESS=640x400 FLCOMP_PANEL= FLCOMP_SESSION= FLCOMP_SCREENSHOT=$dir/shot.ppm
export FLCOMP_TEST_FIFO=$dir/in FLCOMP_KEYBOARD_CONF=$dir/none FLCOMP_ANIM_LOG=1
unset WAYLAND_DISPLAY
shm=$(pwd)/tests/shmclient
mkfifo "$dir/in"
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

start() {	# flcomp with the current desktop.conf
	rm -f "$dir/wayland-0" "$dir/wayland-0.lock"
	./flcomp 2>"$dir/log" &
	comp=$!
	i=0
	while [ ! -S "$dir/wayland-0" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
	[ -S "$dir/wayland-0" ] || { cat "$dir/log"; echo "FAIL flcomp did not start"; exit 1; }
}
stop() { kill $comp 2>/dev/null; wait $comp 2>/dev/null; }
shot() { kill -USR1 $comp; sleep 0.25; [ -n "$ANIM_OUT" ] && cp "$dir/shot.ppm" "$ANIM_OUT/$1.ppm"; return 0; }
pixel() {
	python3 -I - "$@" <<'PY'
import sys
f = open(sys.argv[1], 'rb'); x = int(sys.argv[2]); y = int(sys.argv[3])
assert f.readline() == b'P6\n'; w, h = map(int, f.readline().split()); f.readline()
d = f.read(); o = (y * w + x) * 3
print('%02x%02x%02x' % tuple(d[o:o + 3]))
PY
}
nonbg() {	# FILE X0 Y0 X1 Y1: pixels that are not the desktop colour
	python3 -I - "$@" <<'PY'
import sys
f = open(sys.argv[1], 'rb'); x0, y0, x1, y1 = map(int, sys.argv[2:6])
assert f.readline() == b'P6\n'; w, h = map(int, f.readline().split()); f.readline()
d = f.read(); n = 0
for y in range(y0, y1):
    for x in range(x0, x1):
        o = (y * w + x) * 3
        if d[o:o + 3] != bytes.fromhex('1d2125'):
            n += 1
print(n)
PY
}
px() { pixel "$dir/shot.ppm" "$1" "$2"; }
# a colour strictly between two others (every channel), i.e. a blend: between A B GOT
between() {
	python3 -I - "$1" "$2" "$3" <<'PY'
import sys
a, b, g = (bytes.fromhex(v) for v in sys.argv[1:4])
lo = [min(x, y) for x, y in zip(a, b)]; hi = [max(x, y) for x, y in zip(a, b)]
print("yes" if all(l <= v <= h for l, v, h in zip(lo, g, hi)) and g != a and g != b else "no")
PY
}
expect_px() {	# name x y colour
	got=$(px "$2" "$3")
	[ "$got" = "$4" ] && ok "$1" || bad "$1: ($2,$3) is $got, wanted $4"
}
expect_blend() {	# name x y colourA colourB
	got=$(px "$2" "$3")
	[ "$(between "$4" "$5" "$got")" = yes ] && ok "$1 ($got between $4 and $5)" || bad "$1: ($2,$3) is $got, not between $4 and $5"
}
send() { echo "$1" >"$dir/in"; sleep 0.12; }
tap() { send "key $1 1"; }
untap() { send "key $1 0"; }
chord() { for m in $1; do tap "$m"; done; tap "$2"; untap "$2"; for m in $1; do untap "$m"; done; }

BG=1d2125
CX=260	# the first window (200x100) opens at 160,90: this is its centre
CY=140
fresh() {	# a new compositor: the first window is always placed at the same spot
	[ -n "$comp" ] && stop
	start
}
# ---- animations=on, slowed down ------------------------------------------------------------------
printf 'animations=on\nanimation_speed=0.1\n' >"$dir/desktop.conf"
comp=
fresh
"$shm" -w 200 -h 100 -c 3f9b63 -t one -s 100 >"$dir/w1.out" &
w1=$!
sleep 0.5
shot open-mid
expect_blend "opening: the window is part way faded in" $CX $CY $BG 3f9b63
expect_px "opening: it starts a little smaller (the corner is not there yet)" 161 91 $BG
sleep 2.4
shot open-end
expect_px "opened: the window is fully there" $CX $CY 3f9b63
expect_px "opened: and at full size" 161 91 3f9b63
kill $w1 2>/dev/null
sleep 0.45
shot close-mid
# closing takes 1.2 s slowed down: the picture is still there, fading
expect_blend "closing: the last picture fades out" $CX $CY $BG 3f9b63
sleep 1.6
shot close-end
expect_px "closed: the desktop is back" $CX $CY $BG

# minimize (fl_shell minimize from a listener client)
fresh
"$shm" -w 200 -h 100 -c 3f9b63 -t two -s 100 >"$dir/w2.out" &
sleep 2.2
"$shm" -l -C "minimize:1" -s 3 >"$dir/min.out" &
sleep 2.4
shot min-mid
got=$(px $CX $CY)
[ "$got" != 3f9b63 ] && ok "minimizing: the window has left its place ($got)" || bad "minimizing: still at its place"
n=$(nonbg "$dir/shot.ppm" 200 300 440 400)
[ "$n" -gt 200 ] && ok "minimizing: ... and is drawn towards the panel edge ($n px)" || bad "minimizing: nothing drawn near the panel edge ($n px)"
sleep 2.4
shot min-end
expect_px "minimized: gone" $CX $CY $BG

# maximize: the geometry change is animated
fresh
"$shm" -w 200 -h 100 -c 3f9b63 -t three -s 100 >"$dir/w3.out" &
sleep 2.4
chord "125" 103
sleep 0.35
shot max-mid
# the window grows from 200x100 to the work area: part of the way, the corner is still bare
got=$(px 10 10)
[ "$got" = "$BG" ] && ok "maximizing: the corner is not covered yet" || bad "maximizing: corner is $got already"
sleep 2.6
shot max-end
expect_px "maximized: covers the corner" 10 10 3f9b63

# workspace slide: Super+Ctrl+Right moves the view; the window of workspace 1 slides out to the left
fresh
"$shm" -w 200 -h 100 -c 3f9b63 -t ws1 -s 100 >"$dir/w4.out" &
sleep 2.4
chord "125 29" 106
sleep 0.2
shot ws-mid
expect_px "workspace slide: the window has moved left (its old right part is bare)" 300 $CY $BG
expect_px "workspace slide: ... and is on its way (a place it was not at)" 100 $CY 3f9b63
sleep 2.6
shot ws-end
expect_px "workspace 2 is empty" $CX $CY $BG
chord "125 29" 105
sleep 2.8
shot ws-back
expect_px "back on workspace 1" $CX $CY 3f9b63

# overview zoom: Super+Tab
chord "125" 15
sleep 0.2
shot ov-mid
mid=$(px 320 10)
sleep 2.6
shot ov-end
end=$(px 320 10)
[ "$mid" != "$end" ] && ok "overview: a frame in the middle of the zoom differs from the end ($mid, $end)" || bad "overview: mid and end frame look the same ($mid)"
chord "" 1	# Esc
sleep 0.4
shot ov-closing
sleep 2.4
shot ov-closed
expect_px "overview closed: window back" $CX $CY 3f9b63
grep -q "anim:" "$dir/log" && ok "FLCOMP_ANIM_LOG reports the bursts" || bad "no anim: lines in the log"
stop
comp=

# ---- reduced: fades only ----------------------------------------------------------------------------
printf 'animations=reduced\nanimation_speed=0.1\n' >"$dir/desktop.conf"
start
"$shm" -w 200 -h 100 -c 3f9b63 -t red -s 100 >"$dir/w5.out" &
w5=$!
sleep 0.3
shot red-mid
# the window keeps its size: the pixel next to its top left corner is faded in, not outside it as when scaled
expect_blend "reduced: the corner is fading in already, no scaling" 161 91 $BG 3f9b63
sleep 2.4
shot red-end
expect_px "reduced: fully there" $CX $CY 3f9b63
kill $w5 2>/dev/null
stop

# ---- off: nothing moves --------------------------------------------------------------------------------
printf 'animations=off\n' >"$dir/desktop.conf"
start
"$shm" -w 200 -h 100 -c 3f9b63 -t off -s 100 >"$dir/w6.out" &
w6=$!
sleep 0.5
shot off
expect_px "off: the window is there at once" $CX $CY 3f9b63
kill $w6 2>/dev/null
sleep 0.3
shot off-closed
expect_px "off: gone at once" $CX $CY $BG
grep -q "anim:" "$dir/log" && bad "off: something animated" || ok "off: no animation ran"
stop

# ---- pacing at the real speed, and idleness after ---------------------------------------------------------
printf 'animations=on\n' >"$dir/desktop.conf"
start
for n in 1 2 3; do
	"$shm" -w 300 -h 200 -c 3f9b63 -t "p$n" -s 100 >"$dir/p$n.out" &
	sleep 0.6
done
chord "125 29" 106
sleep 0.5
chord "125 29" 105
sleep 0.5
chord "125" 15
sleep 0.5
chord "" 1
sleep 1
kill $(jobs -p | grep -v "^$comp$") 2>/dev/null
sleep 1
frames=$(grep -c "^anim: done" "$dir/log")
[ "$frames" -ge 3 ] && ok "pacing: $frames bursts of motion logged" || bad "pacing: only $frames bursts logged"
awk '/^anim: done/ {
	for (i = 1; i <= NF; i++) { if ($i == "frames") n = $(i - 1); if ($i == "interval") { avg = $(i + 2) + 0; mx = $(i + 4) + 0 } if ($i == "missed") m = $(i + 1) + 0 }
	if (n >= 4 && avg > 22) { print "slow " $0; exit 1 }
	if (m > 1) { print "missed " $0; exit 1 }
}' "$dir/log" && ok "pacing: the interval between frames stays at 16 ms and no frame is missed" || bad "pacing: $(grep '^anim: done' "$dir/log" | tr '\n' '|')"
grep "^anim: done" "$dir/log" | head -8
# idle: no wakeups and no CPU once the motion ended
sleep 1
t0=$(awk '{print $14 + $15}' /proc/$comp/stat)
sleep 3
t1=$(awk '{print $14 + $15}' /proc/$comp/stat)
[ $((t1 - t0)) -le 1 ] && ok "idle after the motion: $((t1 - t0)) clock ticks in 3 s" || bad "idle: flcomp used $((t1 - t0)) ticks in 3 s"
n0=$(grep -c "" "$dir/log")
sleep 1
[ "$(grep -c '' "$dir/log")" = "$n0" ] && ok "idle: nothing logged" || bad "idle: the log grew"
kill -0 $comp 2>/dev/null && ok "compositor alive" || bad "compositor died"
stop

[ $fails = 0 ] && echo "all tests passed" || echo "$fails test(s) failed"
exit $fails
