#!/bin/sh
# Headless test of the workspace pager in the panel: the real flpanel under flcomp (no display, no
# root, HOME is a scratch directory). Clicking a cell switches, the wheel steps, a right click shows
# the menu (overview, add or remove a workspace, settings). Needs ../flcomp built (flcomp, tests/shmclient;
# FLCOMP_DIR names another build), python3 and a dbus-daemon (DBUS_DAEMON may name it). `make check`;
# PAGER_OUT=DIR keeps the screenshots (PPM); PAGER_THEME=light runs it with the light theme.
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
here=$(pwd)
comp=${FLCOMP_DIR:-$here/../flcomp}
dir=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$dir"' EXIT
export XDG_RUNTIME_DIR=$dir HOME=$dir XDG_CONFIG_HOME=$dir/cfg FLUI_CONF=$dir/desktop.conf
mkdir -p "$dir/cfg" "$dir/bin"
# "Workspace settings" runs flsettings --page workspaces: a stand-in that writes down its arguments
printf '#!/bin/sh\necho "$*" >> "%s/flsettings.args"\n' "$dir" >"$dir/bin/flsettings"
chmod +x "$dir/bin/flsettings"
export PATH=$dir/bin:$PATH
[ -n "$PAGER_THEME" ] && echo "theme=$PAGER_THEME" >"$dir/desktop.conf"
# the colours the checks look for: accent, dim text and the overview's backdrop of the theme
if [ "$PAGER_THEME" = light ]; then ACC=2c7a4b DIM=4f5761 BACK=cdd2d8; else ACC=3f9b63 DIM=8a9199 BACK=15181c; fi
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
# cells FILE -> "x1 y1 cw ch": the accent coloured cell (the workspace in view) in the right part of the panel
cells() {
	python3 -I - "$@" $ACC <<'PY'
import sys
f = open(sys.argv[1], 'rb'); f.readline(); w, h = map(int, f.readline().split()); f.readline(); d = f.read()
pts = [(x, y) for y in range(h - 44, h) for x in range(w // 2, w)
       if d[(y * w + x) * 3:(y * w + x) * 3 + 3] == bytes.fromhex(sys.argv[2])]
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
n=$(count "$dir/shot.ppm" $DIM)
[ "$n" -ge 4 ] && ok "a cell with a window in it has a mark" || bad "no mark in the cells ($n)"
# ---- the right-click menu ----------------------------------------------------------------------------
# menu_at FILE -> "x y": the top left of the menu above the panel (a flat accent edge over 200 px)
menu_at() {
	python3 -I - "$@" $ACC <<'PY'
import sys
f = open(sys.argv[1], 'rb'); f.readline(); w, h = map(int, f.readline().split()); f.readline(); d = f.read()
acc = bytes.fromhex(sys.argv[2])
for y in range(h - 44 - 200, h - 44):
    run = 0
    for x in range(w):
        if d[(y * w + x) * 3:(y * w + x) * 3 + 3] == acc:
            run += 1
            if run >= 200:
                print(x - run + 1, y); sys.exit(0)
        else:
            run = 0
PY
}
# a right click on the pager (where the cell in view is, from a fresh picture)
open_menu() {
	shot tmp
	set -- $(cells "$dir/shot.ppm")
	[ -n "$1" ] || { bad "open_menu: no pager"; return 1; }
	send "move $(($1 + $3 / 2)) $(($2 + $4 / 2))"
	send "button 273 1"
	send "button 273 0"
	sleep 0.4
}
# menu_item NAME ITEM: take the picture NAME of the open menu, then click item 0 overview, 1 add, 2 remove, 3 settings
menu_item() {
	shot "$1"
	set -- "$1" "$2" $(menu_at "$dir/shot.ppm")
	[ -n "$3" ] || { bad "menu_item: no menu in the picture"; return 1; }
	case $2 in 0) off=23;; 1) off=62;; 2) off=92;; *) off=131;; esac
	click $(($3 + 100)) $(($4 + off))
	sleep 0.6
}
conf_has() { grep -qx "workspaces=$1" "$dir/desktop.conf" 2>/dev/null; }
# the file as a user would have written it (the theme stays), flcomp and the panel pick it up
setconf() { { [ -n "$PAGER_THEME" ] && echo "theme=$PAGER_THEME"; echo "$1"; } >"$dir/desktop.conf"; }
conf_now() { grep '^workspaces=' "$dir/desktop.conf" 2>/dev/null | tail -1; }

open_menu
shot pager-menu
mpos=$(menu_at "$dir/shot.ppm")
[ -n "$mpos" ] && ok "a right click on the pager opens the menu (at $mpos)" || bad "no menu"
# its right edge is inside the screen and it sits right above the panel
set -- $mpos
[ "$(($2 + 150))" = "$((H - 44))" ] && ok "the menu is directly above the panel" || bad "menu y=$2"
menu_item pager-menu-settings 3
[ "$(cat "$dir/flsettings.args" 2>/dev/null)" = "--page workspaces" ] && ok "Workspace settings runs flsettings --page workspaces" || bad "flsettings args: $(cat "$dir/flsettings.args" 2>/dev/null)"
shot tmp
[ -z "$(menu_at "$dir/shot.ppm")" ] && ok "choosing an item closes the menu" || bad "menu still open"
[ -z "$(conf_now)" ] && ok "nothing was written to desktop.conf so far" || bad "conf: $(conf_now)"

# the overview item is the old right click action
open_menu
menu_item pager-menu-overview 0
shot pager-overview
[ "$(pixel "$dir/shot.ppm" 20 20)" = "$BACK" ] || [ "$(pixel "$dir/shot.ppm" 600 20)" = "$BACK" ] && ok "the overview item opens the overview" || bad "no overview"
send "key 1 1"
send "key 1 0"
sleep 0.5
shot tmp
[ "$(pixel "$dir/shot.ppm" 20 20)" != "$BACK" ] && ok "Esc closes the overview" || bad "overview still open"

# Add: one column more
open_menu
menu_item pager-menu-add 1
conf_has 3x2 && ok "Add workspace writes workspaces=3x2" || bad "add: $(conf_now)"
sleep 0.5
[ "$(last_ws)" = "workspaces 3x2 current=0" ] && ok "the compositor applies 3x2 at once" || bad "after add: $(last_ws)"
shot pager-3x2
set -- $(cells "$dir/shot.ppm")
X=$1; Y=$2; CW=$3; CH=$4
click $((X + 2 * (CW + 2) + CW / 2)) $((Y + CH + 2 + CH / 2))
[ "$(last_ws)" = "workspaces 3x2 current=5" ] && ok "the pager has the new cells: the sixth one switches there" || bad "click cell 6: $(last_ws)"
# Remove, three times: 2x2, 1x2, 1x1
click $((X + 2 * (CW + 2) + CW / 2)) $((Y + CH / 2))
open_menu
menu_item pager-menu-remove 2
conf_has 2x2 && ok "Remove workspace writes workspaces=2x2" || bad "remove: $(conf_now)"
sleep 0.5
case $(last_ws) in "workspaces 2x2 "*) ok "the compositor applies 2x2 ($(last_ws))";; *) bad "after remove: $(last_ws)";; esac
open_menu
menu_item tmp 2
conf_has 1x2 && ok "Remove again: 1x2" || bad "remove 2: $(conf_now)"
sleep 0.5
shot pager-1x2
set -- $(cells "$dir/shot.ppm")
[ "$4" -ge 5 ] && ok "the pager is one column of two cells (cell ${3}x${4})" || bad "pager 1x2: $*"
open_menu
menu_item tmp 2
conf_has 1x1 && ok "Remove once more: 1x1, a row goes when there is one column" || bad "remove 3: $(conf_now)"
sleep 0.5
[ "$(last_ws)" = "workspaces 1x1 current=0" ] && ok "the compositor has one workspace" || bad "after 1x1: $(last_ws)"
shot pager-1x1
[ -z "$(cells "$dir/shot.ppm" 2>/dev/null)" ] && ok "with one workspace the pager is gone" || bad "pager still drawn"
[ "$(count "$dir/shot.ppm" e0a030)" -gt 1000 ] && ok "the windows of the removed workspaces moved to the last one" || bad "window one is not on the remaining workspace"

# the limits: 8 x 8 is the most; a bare number in the file is N x 1; a column in a 1 x N grid is a row
setconf workspaces=8x7
sleep 1
open_menu
menu_item pager-menu-8x7 1
conf_has 8x8 && ok "Add at 8 columns makes a row: 8x8" || bad "add 8x7: $(conf_now)"
sleep 0.5
open_menu
shot pager-menu-8x8
menu_item tmp 1
conf_has 8x8 && ok "Add at 8x8 does nothing" || bad "add at 8x8: $(conf_now)"
shot tmp
[ -n "$(menu_at "$dir/shot.ppm")" ] && ok "a dim item does not close the menu" || bad "menu closed by a dim item"
send "key 1 1"
send "key 1 0"
sleep 0.4
shot tmp
[ -z "$(menu_at "$dir/shot.ppm")" ] && ok "Esc closes the menu" || bad "menu still open after Esc"
shot pager-8x8
set -- $(cells "$dir/shot.ppm")
[ -n "$1" ] && ok "the pager draws 64 cells in the panel (cell ${3}x${4})" || bad "no 8x8 pager"
setconf workspaces=3
sleep 1
open_menu
menu_item tmp 1
conf_has 4x1 && ok "a bare 3 in the file is 3x1; Add: 4x1" || bad "bare number: $(conf_now)"
sleep 0.5
setconf workspaces=1x3
sleep 1
open_menu
menu_item tmp 2
conf_has 1x2 && ok "Remove in 1x3: 1x2" || bad "1x3: $(conf_now)"
[ $fails = 0 ] || { echo "--- flcomp log"; tail -8 "$dir/log"; }
kill $cpid 2>/dev/null
[ $fails = 0 ] && echo "all tests passed" || echo "$fails test(s) failed"
exit $fails
