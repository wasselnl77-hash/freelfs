#!/bin/sh
# Headless flcomp test of performance mode (perf.c): a fullscreen window alone is drawn
# (unredirect), anything that needs the screen again brings the desktop back, and
# performance=off switches it all off. Same harness as run.sh; nothing outside a scratch
# directory is touched. `make check-perf`; PERF_OUT=DIR keeps screenshots there (PPM).
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
dir=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$dir"' EXIT
export XDG_RUNTIME_DIR=$dir HOME=$dir FLUI_CONF=$dir/desktop.conf
export FLCOMP_WALLPAPER= FLCOMP_HEADLESS=640x400 FLCOMP_PANEL= FLCOMP_SESSION= FLCOMP_SCREENSHOT=$dir/shot.ppm
export FLCOMP_TEST_FIFO=$dir/in FLCOMP_KEYBOARD_CONF=$dir/none
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
shot() { kill -USR1 $comp; sleep 0.4; [ -n "$PERF_OUT" ] && cp "$dir/shot.ppm" "$PERF_OUT/$1.ppm"; return 0; }
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
logged() { grep -q "$1" "$dir/log" && ok "$2" || bad "$2 (log: $(tr '\n' '|' <"$dir/log"))"; }
notlogged() { grep -q "$1" "$dir/log" && bad "$2" || ok "$2"; }

# ---- performance=auto (the default) ---------------------------------------------------------------
echo "power_saver=1" >"$dir/desktop.conf"	# so that the frame cap would apply
start
"$shm" -p 40 -c 272c33 -s 100 >"$dir/panel.out" &
sleep 0.4
"$shm" -w 200 -h 100 -c 3f9b63 -t desk -s 100 >"$dir/w1.out" &
sleep 0.4
shot normal
expect_px "a normal window and the panel are drawn as before (window)" 200 100 3f9b63
expect_px "panel" 300 380 272c33
notlogged "performance mode on" "no performance mode without a fullscreen window"
notlogged "replaces the desktop" "nothing unredirected without a fullscreen window"

"$shm" -F -c ff8800 -t game -s 100 >"$dir/game.out" &
game=$!
sleep 0.7
shot fullscreen
expect_px "fullscreen window covers the screen (top left)" 2 2 ff8800
expect_px "fullscreen window covers the panel" 300 380 ff8800
expect_px "fullscreen window (bottom right)" 637 397 ff8800
logged "performance mode on" "performance mode switches on for a fullscreen window"
logged "fullscreen window replaces the desktop" "the fullscreen window is drawn alone"

# a second, ordinary window on top: not fullscreen on top any more, the desktop path draws both
"$shm" -w 200 -h 100 -c 0000ff -t dialog -s 100 >"$dir/w2.out" &
dlg=$!
sleep 0.7
shot dialog
expect_px "a window above the fullscreen one is shown (drawn the normal way)" 250 130 0000ff
expect_px "the fullscreen window is still under it" 20 20 ff8800
logged "is back on the desktop" "the desktop path is back while another window is on top"
kill $dlg 2>/dev/null
sleep 0.5
shot dialog-gone
expect_px "dialog gone: the fullscreen window again" 250 130 ff8800
kill $game 2>/dev/null
sleep 0.5
shot back
expect_px "fullscreen window closed: the first window is back" 200 100 3f9b63
expect_px "and the panel" 300 380 272c33
logged "performance mode off" "performance mode switches off again"
kill -0 $comp 2>/dev/null && ok "compositor alive" || bad "compositor died"
stop

# ---- performance=off: nothing is skipped ------------------------------------------------------------
printf 'power_saver=1\nperformance=off\n' >"$dir/desktop.conf"
start
"$shm" -F -c ff8800 -t game -s 100 >"$dir/game.out" &
sleep 0.7
shot off
expect_px "performance=off: the picture is the same" 2 2 ff8800
notlogged "performance mode on" "performance=off: mode stays off"
notlogged "replaces the desktop" "performance=off: no unredirect"
stop

# ---- performance=on: on without a fullscreen window --------------------------------------------------
printf 'power_saver=1\nperformance=on\n' >"$dir/desktop.conf"
start
"$shm" -w 200 -h 100 -c 3f9b63 -t desk -s 100 >"$dir/w1.out" &
sleep 0.7
shot on
expect_px "performance=on: an ordinary window is drawn normally" 200 100 3f9b63
logged "performance mode on" "performance=on: mode is on with no fullscreen window"
notlogged "replaces the desktop" "performance=on: still nothing unredirected without a fullscreen window"
# the setting follows the file at run time
printf 'power_saver=1\nperformance=off\n' >"$dir/desktop.conf"
sleep 0.8
"$shm" -w 20 -h 20 -t poke -s 1 >/dev/null 2>&1	# a frame, so that the cap is asked again
sleep 0.5
logged "performance mode off" "changing performance= in desktop.conf applies at once"
stop

[ $fails = 0 ] && echo "all tests passed" || echo "$fails test(s) failed"
exit $fails
