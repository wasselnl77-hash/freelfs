#!/bin/sh
# Headless test of the workspace pager in the panel: the real flpanel under flcomp (no display, no
# root, HOME is a scratch directory). Clicking a cell switches, the wheel steps, a right click shows
# the overview. Needs ../flcomp built (flcomp, tests/shmclient), python3 and a dbus-daemon
# (DBUS_DAEMON may name it). `make check`; PAGER_OUT=DIR keeps the screenshots (PPM).
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
here=$(pwd)
comp=$here/../flcomp
dir=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$dir"' EXIT
export XDG_RUNTIME_DIR=$dir HOME=$dir XDG_CONFIG_HOME=$dir/cfg FLUI_CONF=$dir/desktop.conf
mkdir -p "$dir/cfg"
unset WAYLAND_DISPLAY
W=1000; H=600
export FLCOMP_WALLPAPER= FLCOMP_HEADLESS=${W}x${H} FLCOMP_SESSION= FLCOMP_SCREENSHOT=$dir/shot.ppm
export FLCOMP_TEST_FIFO=$dir/in FLCOMP_KEYBOARD_CONF=$dir/none FLCOMP_PANEL=$here/flpanel
export FLCOMP_BACKLIGHT=$dir/none-backlight FLPANEL_POWER_SUPPLY=$dir/none-ps
mkfifo "$dir/in"
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
cat >"$dir/bus.conf" <<XML
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig><type>session</type><keep_umask/><listen>unix:path=$dir/bus</listen><auth>EXTERNAL</auth>
<policy context="default"><allow send_destination="*" eavesdrop="true"/><allow eavesdrop="true"/><allow own="*"/></policy></busconfig>
XML
${DBUS_DAEMON:-dbus-daemon} --config-file="$dir/bus.conf" --nofork --nopidfile 2>"$dir/bus.log" &
i=0
while [ ! -S "$dir/bus" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
export DBUS_SESSION_BUS_ADDRESS=unix:path=$dir/bus

"$comp/flcomp" 2>"$dir/log" &
cpid=$!
i=0
while [ ! -S "$dir/wayland-0" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
sleep 1.2
"$comp/tests/shmclient" -l -W -s 900 >"$dir/listen.out" &
"$comp/tests/shmclient" -w 200 -h 100 -c e0a030 -t one -s 900 >"$dir/w1.out" &
sleep 0.6
"$comp/tests/shmclient" -w 150 -h 80 -c ff0000 -t two -s 900 >"$dir/w2.out" &
sleep 0.6

shot() { sleep 0.3; kill -USR1 $cpid; sleep 0.5; [ -n "$PAGER_OUT" ] && cp "$dir/shot.ppm" "$PAGER_OUT/$1.ppm"; return 0; }
send() { echo "$1" >"$dir/in"; sleep 0.15; }
last_ws() { grep '^workspaces' "$dir/listen.out" | tail -1; }
# cells FILE W H -> "x1 y1 cw ch": the accent coloured cell (the workspace in view) in the right part of the panel
cells() {
	python3 -I - "$@" <<'PY'
import sys
f = open(sys.argv[1], 'rb'); f.readline(); w, h = map(int, f.readline().split()); f.readline(); d = f.read()
pts = [(x, y) for y in range(h - 44, h) for x in range(w // 2, w)
       if d[(y * w + x) * 3:(y * w + x) * 3 + 3] == bytes.fromhex('3f9b63')]
xs = [p[0] for p in pts]; ys = [p[1] for p in pts]
print(min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1)
PY
}
count() {
	python3 -I - "$@" <<'PY'
import sys
f = open(sys.argv[1], 'rb'); f.readline(); w, h = map(int, f.readline().split()); f.readline(); d = f.read()
c = bytes.fromhex(sys.argv[2])
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

shot pager-0
grep -q "^workspaces 2x2 current=0" "$dir/listen.out" && ok "the compositor says 2x2" || bad "workspaces event: $(grep workspaces "$dir/listen.out")"
set -- $(cells "$dir/shot.ppm")
X=$1; Y=$2; CW=$3; CH=$4
[ "$CW" -ge 8 ] && [ "$CH" -ge 5 ] && ok "the pager shows the current workspace as an accent cell (${CW}x${CH} at $X,$Y)" || bad "no pager found: $*"
# the cells are CW + 2 apart; the pager is 2 columns, so the second cell is to the right of the first
click() { send "move $1 $2"; send "button 272 1"; send "button 272 0"; sleep 0.3; }
click $((X + CW + 2 + CW / 2)) $((Y + CH / 2))
[ "$(last_ws)" = "workspaces 2x2 current=1" ] && ok "a click on the second cell switches there" || bad "click cell 2: $(last_ws)"
shot pager-1
[ "$(count "$dir/shot.ppm" e0a030)" = 0 ] && ok "the windows are gone from workspace 2" || bad "windows still drawn"
click $((X + CW / 2)) $((Y + CH + 2 + CH / 2))
[ "$(last_ws)" = "workspaces 2x2 current=2" ] && ok "a click on the third cell (second row) switches there" || bad "click cell 3: $(last_ws)"
send "move $((X + CW / 2)) $((Y + CH / 2))"
send "axis -1"
[ "$(last_ws)" = "workspaces 2x2 current=1" ] || [ "$(last_ws)" = "workspaces 2x2 current=2" ] && ok "the wheel over the pager steps through the workspaces ($(last_ws))" || bad "wheel"
click $((X + CW / 2)) $((Y + CH / 2))
[ "$(last_ws)" = "workspaces 2x2 current=0" ] && ok "the first cell goes back" || bad "click cell 1: $(last_ws)"
shot pager-back
[ "$(count "$dir/shot.ppm" e0a030)" -gt 1000 ] && ok "the windows are back" || bad "windows not back"
# a window on another workspace marks its cell: send window one to workspace 4 and look at the pager
"$comp/tests/shmclient" -l -W -s 3 -C "wsmove:1,3" >/dev/null &
sleep 2
shot pager-moved
n=$(count "$dir/shot.ppm" 8a9199)
[ "$n" -ge 4 ] && ok "a cell with a window in it has a mark" || bad "no mark in the cells ($n)"
# a right click on the pager shows the overview
send "move $((X + CW / 2)) $((Y + CH / 2))"
send "button 273 1"
send "button 273 0"
sleep 0.3
shot pager-overview
[ "$(pixel "$dir/shot.ppm" 20 20)" = "15181c" ] || [ "$(pixel "$dir/shot.ppm" 600 20)" = "15181c" ] && ok "a right click on the pager opens the overview" || bad "no overview"
[ $fails = 0 ] || { echo "--- flcomp log"; tail -8 "$dir/log"; }
kill $cpid 2>/dev/null
[ $fails = 0 ] && echo "all tests passed" || echo "$fails test(s) failed"
exit $fails
