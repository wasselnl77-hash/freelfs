#!/bin/sh
# Headless flcomp test of the workspaces (grid, Super+Ctrl+arrows, Super+Shift+arrows, the panel's
# requests, the Super+Tab overview, desktop.conf workspaces=), fl_shell version 5 and the
# fl_screencopy capture protocol, and the Print / Shift+Print / Super+Shift+S hand-over to Snappel.
# Same harness as keys.sh; HOME is a scratch directory. `make check-ws`; with WS_OUT=DIR it keeps
# screenshots there (PPM).
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
dir=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$dir"' EXIT
export XDG_RUNTIME_DIR=$dir HOME=$dir FLUI_CONF=$dir/desktop.conf
export FLCOMP_WALLPAPER= FLCOMP_HEADLESS=640x400 FLCOMP_PANEL= FLCOMP_SESSION= FLCOMP_SCREENSHOT=$dir/shot.ppm
export FLCOMP_TEST_FIFO=$dir/in FLCOMP_KEYBOARD_CONF=$dir/none
mkdir "$dir/bin"
printf '#!/bin/sh\necho "$@" >>"%s/snappel.log"\n' "$dir" >"$dir/bin/snappel"
chmod +x "$dir/bin/snappel"
mkfifo "$dir/in"
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

# first run: no Snappel on the PATH, Print falls back to the built-in screenshot
# (a system that has Snappel installed must not leak into this run)
FLCOMP_SNAPPEL=snappel-not-installed ./flcomp 2>"$dir/log" &
comp=$!
i=0
while [ ! -S "$dir/wayland-0" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
[ -S "$dir/wayland-0" ] || { cat "$dir/log"; echo "FAIL flcomp did not start"; exit 1; }

shot() { kill -USR1 $comp; sleep 0.4; [ -n "$WS_OUT" ] && cp "$dir/shot.ppm" "$WS_OUT/$1.ppm"; return 0; }
# count FILE RRGGBB -> how many pixels have that colour
count() {
	python3 -I - "$@" <<'PY'
import sys
f = open(sys.argv[1], 'rb')
assert f.readline() == b'P6\n'; w, h = map(int, f.readline().split()); f.readline()
d = f.read(); c = bytes.fromhex(sys.argv[2])
print(sum(1 for i in range(0, len(d), 3) if d[i:i + 3] == c))
PY
}
pixel() {
	python3 -I - "$@" <<'PY'
import sys
f = open(sys.argv[1], 'rb'); x = int(sys.argv[2]); y = int(sys.argv[3])
assert f.readline() == b'P6\n'; w, h = map(int, f.readline().split()); f.readline()
d = f.read(); o = (y * w + x) * 3
print('%02x%02x%02x' % tuple(d[o:o + 3]))
PY
}
# seen NAME FILE COLOUR MIN MAX: the colour covers between MIN and MAX pixels
seen() {
	n=$(count "${2}" "$3")
	[ "$n" -ge "$4" ] && [ "$n" -le "$5" ] && ok "$1 ($n px)" || bad "$1: $n px of $3, wanted $4..$5"
}
send() { echo "$1" >"$dir/in"; sleep 0.12; }
tap() { send "key $1 1"; }
untap() { send "key $1 0"; }
chord() {	# chord "125 29" 106: hold the modifiers, tap the key
	for m in $1; do tap "$m"; done
	tap "$2"; untap "$2"
	for m in $1; do untap "$m"; done
	sleep 0.2
}
last_ws() { grep '^workspaces' "$dir/listen.out" | tail -1; }
SUPER=125 CTRL=29 SHIFT=42 TAB=15 ENTER=28 ESC=1 PRINT=99 KS=31
LEFT=105 RIGHT=106 UP=103 DOWN=108

./tests/shmclient -l -W -s 900 >"$dir/listen.out" &
./tests/shmclient -p 40 -c 272c33 -s 900 >"$dir/panel.out" &
sleep 0.5
grep -q "^workspaces 2x2 current=0" "$dir/listen.out" && ok "bind: the grid is 2x2 and the first workspace is in view" || bad "bind: $(grep workspaces "$dir/listen.out")"
./tests/shmclient -w 200 -h 100 -c e0a030 -t one -s 900 >"$dir/w1.out" &
sleep 0.5
./tests/shmclient -w 150 -h 80 -c ff0000 -t two -s 900 >"$dir/w2.out" &
sleep 0.5
grep -q "window_workspace id=1 ws=0" "$dir/listen.out" && grep -q "window_workspace id=2 ws=0" "$dir/listen.out" && ok "new windows open on the workspace in view" || bad "window_workspace: $(grep window_workspace "$dir/listen.out")"
shot start
seen "both windows are drawn" "$dir/shot.ppm" e0a030 1000 40000

# ---- switching -------------------------------------------------------------
chord "$SUPER $CTRL" $RIGHT
shot ws-1-empty
[ "$(last_ws)" = "workspaces 2x2 current=1" ] && ok "Super+Ctrl+Right goes to workspace 2" || bad "switch right: $(last_ws)"
seen "workspace 2 is empty" "$dir/shot.ppm" e0a030 0 0
seen "workspace 2 shows no red either" "$dir/shot.ppm" ff0000 0 0
chord "$SUPER $CTRL" $RIGHT
[ "$(last_ws)" = "workspaces 2x2 current=1" ] && ok "no wrap past the last column" || bad "wrapped: $(last_ws)"
./tests/shmclient -w 120 -h 70 -c 0000ff -t three -s 900 >"$dir/w3.out" &
sleep 0.5
grep -q "window_workspace id=3 ws=1" "$dir/listen.out" && ok "a window opened on workspace 2 belongs there" || bad "window 3: $(grep 'id=3' "$dir/listen.out")"
shot ws-1-blue
seen "the new window is drawn on workspace 2" "$dir/shot.ppm" 0000ff 5000 9000
chord "$SUPER $CTRL" $LEFT
shot ws-0
[ "$(last_ws)" = "workspaces 2x2 current=0" ] && ok "Super+Ctrl+Left goes back" || bad "switch left: $(last_ws)"
seen "back: window one is there" "$dir/shot.ppm" e0a030 1000 40000
seen "back: the blue window stayed away" "$dir/shot.ppm" 0000ff 0 0
grep -q "window_changed id=2 title=two focused=1" "$dir/listen.out" && ok "the newest window of the workspace gets the focus" || bad "focus after switching"

# ---- moving a window -------------------------------------------------------
chord "$SUPER $SHIFT" $DOWN
shot ws-2-moved
[ "$(last_ws)" = "workspaces 2x2 current=2" ] && ok "Super+Shift+Down: the view followed the window to workspace 3" || bad "move down: $(last_ws)"
grep -q "window_workspace id=2 ws=2" "$dir/listen.out" && ok "window_workspace after the move" || bad "window_workspace: $(grep 'id=2' "$dir/listen.out" | tail -2)"
seen "the moved window is in view" "$dir/shot.ppm" ff0000 1000 40000
seen "the other one stayed behind" "$dir/shot.ppm" e0a030 0 0
chord "$SUPER $SHIFT" $DOWN
[ "$(last_ws)" = "workspaces 2x2 current=2" ] && ok "Super+Shift+Down at the bottom row does nothing" || bad "move past the edge: $(last_ws)"
chord "$SUPER $CTRL" $UP
[ "$(last_ws)" = "workspaces 2x2 current=0" ] && ok "Super+Ctrl+Up goes up a row" || bad "switch up: $(last_ws)"

# ---- the panel's requests --------------------------------------------------
./tests/shmclient -l -W -s 4 -C "ws:3 wsmove:1,3" >"$dir/ctl.out" &
sleep 2
[ "$(last_ws)" = "workspaces 2x2 current=3" ] && ok "set_workspace from the panel" || bad "set_workspace: $(last_ws)"
grep -q "window_workspace id=1 ws=3" "$dir/listen.out" && ok "move_window from the panel" || bad "move_window: $(grep 'id=1' "$dir/listen.out" | tail -1)"
shot ws-3
seen "workspace 4 shows window one" "$dir/shot.ppm" e0a030 1000 40000
send "move 5 5"
sleep 0.2
./tests/shmclient -l -W -s 2 -C "ws:0" >"$dir/ctl2.out" &
sleep 1.8

# ---- the overview ----------------------------------------------------------
chord "$SUPER" $TAB
shot ws-overview
# 640 x 360 work area, 2x2: tiles 257 x 145 at (56, 28) and (327, 28), (56, 187) and (327, 187)
expect() { got=$(pixel "$dir/shot.ppm" "$2" "$3"); [ "$got" = "$4" ] && ok "$1" || bad "$1: ($2,$3) is $got, wanted $4"; }
seen "overview: window pictures are small" "$dir/shot.ppm" e0a030 20 3000
seen "overview: red picture present" "$dir/shot.ppm" ff0000 20 3000
expect "overview: the dark backdrop" 630 5 15181c
expect "overview: the panel stays" 300 380 272c33
n=$(count "$dir/shot.ppm" 3f9b63)
[ "$n" -gt 400 ] && ok "overview: the tile in view has an accent frame" || bad "overview: accent frame $n"
send "key $RIGHT 1"; send "key $RIGHT 0"
shot ws-overview-right
send "key $ENTER 1"; send "key $ENTER 0"
sleep 0.3
[ "$(last_ws)" = "workspaces 2x2 current=1" ] && ok "overview: arrows and Enter go to the chosen workspace" || bad "overview enter: $(last_ws)"
shot ws-after-overview
seen "overview closed: blue window drawn full size" "$dir/shot.ppm" 0000ff 5000 9000
# click on a tile
chord "$SUPER" $TAB
send "move 100 80"
send "button 272 1"
send "button 272 0"
sleep 0.3
[ "$(last_ws)" = "workspaces 2x2 current=0" ] && ok "overview: a click on a tile goes there" || bad "overview click: $(last_ws)"
# a click on a window picture focuses that window
chord "$SUPER" $TAB
send "move 430 235"
shot ws-overview-hover
send "button 272 1"
send "button 272 0"
sleep 0.3
shot ws-after-click
[ "$(last_ws)" = "workspaces 2x2 current=3" ] && ok "overview: a click on a window picture goes to its workspace" || bad "overview window click: $(last_ws)"
[ "$(grep '^window_changed' "$dir/listen.out" | grep 'focused=1' | tail -1 | cut -d' ' -f2)" = "id=1" ] && ok "...and focuses that window" || bad "focus after window click: $(grep '^window_changed' "$dir/listen.out" | tail -1)"
# Escape closes it without switching
chord "$SUPER" $TAB
chord "" $ESC
expect "overview: Escape puts it away" 630 5 1d2125
chord "$SUPER" $TAB
chord "" 4
[ "$(last_ws)" = "workspaces 2x2 current=2" ] && ok "overview: the digit 3 goes to workspace 3" || bad "overview digit: $(last_ws)"

# ---- desktop.conf ------------------------------------------------------------
printf 'workspaces=3x1\n' >"$dir/desktop.conf"
sleep 0.6
[ "$(last_ws)" = "workspaces 3x1 current=2" ] && ok "workspaces=3x1 in desktop.conf is applied live" || bad "conf 3x1: $(last_ws)"
grep -q "window_workspace id=1 ws=2" "$dir/listen.out" && ok "a window past the new end moves to the last workspace" || bad "conf: window 1 $(grep 'id=1' "$dir/listen.out" | tail -1)"
printf 'workspaces=1x1\n' >"$dir/desktop.conf"
sleep 0.6
[ "$(last_ws)" = "workspaces 1x1 current=0" ] && ok "a smaller grid pulls the view and the windows back" || bad "conf 1x1: $(last_ws)"
grep -q "window_workspace id=3 ws=0" "$dir/listen.out" && ok "windows of removed workspaces moved to the last one" || bad "conf: window 3 $(grep 'id=3' "$dir/listen.out" | tail -1)"
shot ws-1x1
chord "$SUPER $CTRL" $RIGHT
[ "$(last_ws)" = "workspaces 1x1 current=0" ] && ok "one workspace: switching does nothing" || bad "1x1 switch"
printf 'workspaces=2x2\n' >"$dir/desktop.conf"
sleep 0.6

# ---- screencopy --------------------------------------------------------------
./tests/shmclient -l -s 3 -S list >"$dir/sc-list.out" &
sleep 2.5
n=$(grep -c "^sc_window " "$dir/sc-list.out")
[ "$n" = 3 ] && grep -q "sc_windows_done" "$dir/sc-list.out" && ok "list_windows: three windows, then windows_done" || bad "list_windows: $(cat "$dir/sc-list.out")"
grep -q "sc_window id=1 .* app=shmclient title=one" "$dir/sc-list.out" && ok "list_windows has geometry, app id and title" || bad "list_windows detail"
./tests/shmclient -l -s 3 -S out -O "$dir/cap-out.ppm" >"$dir/sc-out.out" &
sleep 2.5
grep -q "capture_buffer fmt=1 640x400" "$dir/sc-out.out" && grep -q "capture_ready 640x400" "$dir/sc-out.out" && ok "capture_output: 640x400, ready" || bad "capture_output: $(cat "$dir/sc-out.out")"
[ "$(pixel "$dir/cap-out.ppm" 300 380)" = "272c33" ] && ok "the capture has the panel" || bad "capture panel pixel"
./tests/shmclient -l -s 3 -S window:1 -O "$dir/cap-win.ppm" >"$dir/sc-win.out" &
sleep 2.5
grep -q "capture_buffer fmt=0 200x100" "$dir/sc-win.out" && grep -q "capture_ready 200x100" "$dir/sc-win.out" && ok "capture_window: the window's 200x100, ready" || bad "capture_window: $(cat "$dir/sc-win.out")"
[ "$(pixel "$dir/cap-win.ppm" 100 50)" = "e0a030" ] && ok "the window capture is the window's own pixels" || bad "window capture pixel"
./tests/shmclient -l -s 3 -S region:10,20,100,50 -O "$dir/cap-reg.ppm" >"$dir/sc-reg.out" &
sleep 2.5
grep -q "capture_ready 100x50" "$dir/sc-reg.out" && ok "capture_region: 100x50" || bad "capture_region: $(cat "$dir/sc-reg.out")"
./tests/shmclient -l -s 3 -S window:99 >"$dir/sc-bad.out" &
sleep 2.5
grep -q "capture_failed" "$dir/sc-bad.out" && ok "an unknown window id fails" || bad "bad id: $(cat "$dir/sc-bad.out")"

# ---- Print, Shift+Print, Super+Shift+S ---------------------------------------------
chord "" $PRINT
sleep 0.6
[ -n "$(ls "$dir"/pictures/screenshot-*.png 2>/dev/null)" ] && ok "without Snappel, Print saves the screen itself" || bad "Print fallback"
kill $comp
wait $comp 2>/dev/null
# second run: Snappel on the PATH
rm -f "$dir/wayland-0" "$dir/wayland-0.lock"
PATH=$dir/bin:$PATH ./flcomp 2>"$dir/log2" &
comp=$!
i=0
while [ ! -S "$dir/wayland-0" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
chord "" $PRINT
chord "$SHIFT" $PRINT
chord "$SUPER $SHIFT" $KS
sleep 0.4
log=$(cat "$dir/snappel.log" 2>/dev/null)
[ "$(printf '%s\n' "$log" | sed -n 1p)" = "--full" ] && [ "$(printf '%s\n' "$log" | sed -n 2p)" = "--region" ] && ok "Print and Shift+Print start snappel --full and --region" || bad "snappel log: '$log'"
[ "$(wc -l <"$dir/snappel.log")" = 3 ] && ok "Super+Shift+S starts snappel with no argument" || bad "Super+Shift+S: $(wc -l <"$dir/snappel.log") calls"
kill $comp 2>/dev/null
echo "flcomp log:"; sed 's/^/  /' "$dir/log"
[ $fails = 0 ] && echo "all tests passed" || echo "$fails test(s) failed"
exit $fails
