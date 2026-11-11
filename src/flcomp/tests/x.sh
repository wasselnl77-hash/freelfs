#!/bin/sh
# Headless flcomp test of Xwayland support (xwm.c, xwsel.c, xwdeco.c): Xwayland starts on the first
# X connection and leaves with the last client, X windows get a frame, titles, focus, stacking,
# move, resize, maximize, fullscreen (performance mode), menus (override-redirect), transient
# dialogs, undecorated windows, workspaces, Alt-Tab, the overview and the clipboard both ways.
# Needs Xwayland (run it in the build chroot, as the other tests); skips without. `make check-x`;
# X_OUT=DIR keeps PPM screenshots there. Everything happens in a scratch directory.
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
xwl=$(command -v Xwayland)
[ -n "$xwl" ] || { echo "skip: Xwayland is not installed"; exit 0; }
dir=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$dir"' EXIT
export XDG_RUNTIME_DIR=$dir HOME=$dir FLUI_CONF=$dir/desktop.conf
export FLCOMP_WALLPAPER= FLCOMP_HEADLESS=800x500 FLCOMP_PANEL= FLCOMP_SESSION= FLCOMP_SCREENSHOT=$dir/shot.ppm
export FLCOMP_TEST_FIFO=$dir/in FLCOMP_KEYBOARD_CONF=$dir/none FLCOMP_XWAYLAND=$xwl
unset WAYLAND_DISPLAY DISPLAY
shm=$(pwd)/tests/shmclient xcl=$(pwd)/tests/xclient
mkfifo "$dir/in"
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

./flcomp 2>"$dir/log" &
comp=$!
i=0
while [ ! -S "$dir/wayland-0" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
[ -S "$dir/wayland-0" ] || { cat "$dir/log"; echo "FAIL flcomp did not start"; exit 1; }
i=0
while ! grep -q "DISPLAY=" "$dir/log" && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
disp=$(sed -n 's/.*DISPLAY=\(:[0-9]*\),.*/\1/p' "$dir/log")
[ -n "$disp" ] && ok "DISPLAY $disp is exported and the X socket exists" || { cat "$dir/log"; bad "no DISPLAY in the log"; exit 1; }

shot() { kill -USR1 $comp; sleep 0.5; [ -n "$X_OUT" ] && cp "$dir/shot.ppm" "$X_OUT/$1.ppm"; return 0; }
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
send() { echo "$1" >"$dir/in"; sleep 0.15; }
click() { send "move $1 $2"; send "button 272 1"; send "button 272 0"; sleep 0.2; }
waitfor() {	# file pattern [seconds]
	n=0; max=$(( ${3:-6} * 10 ))
	while ! grep -q "$2" "$1" 2>/dev/null; do
		n=$((n + 1)); [ $n -gt $max ] && return 1; sleep 0.1
	done
}
has() { grep -q "$2" "$1" && ok "$3" || { bad "$3 ($1: no '$2')"; }; }
xw_pid() { pgrep -P $comp -x Xwayland | head -1; }
lastcfg() { grep "^$2configure" "$1" | tail -1; }	# lastcfg FILE [prefix]
SUPER=125 CTRL=29 ALT=56 SHIFT=42 TAB=15 RIGHT=106 LEFT=105

# 1. nothing runs until an X client connects
[ -z "$(xw_pid)" ] && ok "no Xwayland while there is no X client" || bad "Xwayland runs without a client"

"$shm" -l -s 120 >"$dir/listen.out" &
"$shm" -p 40 -c 272c33 -s 120 >"$dir/panel.out" &
sleep 0.5

# 2. the first X client starts Xwayland; its window gets a frame, a title, focus
DISPLAY=$disp "$xcl" -t alpha -s 100 -a 9000:retitle=alpha-two -a 10000:getclip >"$dir/a.out" 2>&1 &
apid=$!
waitfor "$dir/a.out" "^configure" 20 && ok "the window is up (Xwayland started by the connection)" || { bad "alpha never configured"; cat "$dir/log" "$dir/xwayland.log" | grep -v "Warning\|^>"; }
[ -n "$(xw_pid)" ] && ok "Xwayland is running now" || bad "no Xwayland"
sleep 0.5
read -r _ ax ay aw ah <<EOF
$(lastcfg "$dir/a.out")
EOF
echo "     alpha content at $ax,$ay ${aw}x$ah"
has "$dir/listen.out" "window_added id=1 app_id=xclient title=alpha" "panel is told: app id from WM_CLASS, title"
has "$dir/a.out" "^focus in" "the new window has the X input focus"
shot xwl-1
expect_px "client pixels" $((ax + 150)) $((ay + 100)) 3f9b63
expect_px "title bar (focused)" $((ax + 85)) $((ay - 15)) 343a43
expect_px "left border" $((ax - 2)) $((ay + 50)) 343a43
expect_px "panel still there" 400 480 272c33

# 3. a click arrives with the right root coordinates
click $((ax + 100)) $((ay + 60))
has "$dir/a.out" "^button 100 60 $((ax + 100)) $((ay + 60))" "pointer: window and root coordinates match the compositor's"

# 4. move by the title bar, resize by the corner
send "move $((ax + 85)) $((ay - 15))"; send "button 272 1"; send "move $((ax + 145)) $((ay + 25))"; send "button 272 0"
sleep 0.5
read -r _ bx by bw bh <<EOF
$(lastcfg "$dir/a.out")
EOF
[ "$bx" = $((ax + 60)) ] && [ "$by" = $((ay + 40)) ] && ok "dragging the title bar moves the window and tells X ($bx,$by)" || bad "move: now $bx,$by, wanted $((ax + 60)),$((ay + 40))"
send "move $((bx + bw + 1)) $((by + bh + 1))"; send "button 272 1"; send "move $((bx + bw + 41)) $((by + bh + 31))"; send "button 272 0"
sleep 0.6
read -r _ cx cy cw ch <<EOF
$(lastcfg "$dir/a.out")
EOF
[ "$cw" = $((bw + 40)) ] && [ "$ch" = $((bh + 30)) ] && ok "dragging the corner resizes: ${cw}x$ch" || bad "resize: ${cw}x$ch, wanted $((bw + 40))x$((bh + 30))"

# 5. unmanaged windows (menus): where the client puts them, above the frames, pointer goes to them
DISPLAY=$disp "$xcl" -t menu -O -x 600 -y 60 -c b04a4a -w 90 -h 70 -s 30 -a 2500:close >"$dir/m.out" 2>&1 &
waitfor "$dir/m.out" "^mapped" 15 && sleep 0.4
shot xwl-2
expect_px "override-redirect window drawn at its own place" 640 90 b04a4a
click 640 90
has "$dir/m.out" "^button" "pointer events reach the override-redirect window"
grep -q "window_added.*title=menu" "$dir/listen.out" && bad "a menu showed up in the panel" || ok "no panel entry, no frame for a menu"
expect_px "no frame around it" 597 57 1d2125
sleep 3

# 6. maximize and fullscreen requests of the client (EWMH), the performance mode follows
DISPLAY=$disp "$xcl" -t beta -c 3d6fa8 -s 100 -a 500:maximize=1 -a 2000:maximize=0 -a 3000:fullscreen=1 -a 5000:fullscreen=0 >"$dir/b.out" 2>&1 &
bpid=$!
waitfor "$dir/b.out" "^mapped" 15
sleep 1.0
shot xwl-3
grep "^configure" "$dir/b.out" | grep -q " 0 30 800 430$" && ok "maximize: the client fills the work area below its title bar" || bad "maximize: $(grep configure "$dir/b.out" | tr '\n' ';')"
grep -q "state maxv maxh" "$dir/b.out" && ok "_NET_WM_STATE says maximized" || bad "no maximized state: $(grep state "$dir/b.out" | tr '\n' ';')"
expect_px "maximized: title bar at the top" 400 5 343a43
sleep 2.3
shot xwl-4
grep "^configure" "$dir/b.out" | grep -q " 0 0 800 500$" && ok "fullscreen: the whole output" || bad "fullscreen: $(grep configure "$dir/b.out" | tr '\n' ';')"
expect_px "fullscreen covers the panel" 400 480 3d6fa8
grep -q "fullscreen window replaces the desktop" "$dir/log" && ok "performance mode takes the fullscreen X window" || { bad "no performance mode"; grep -v "Warning\|^>" "$dir/log" | tail -8; }
sleep 2.7
shot xwl-5
expect_px "back from fullscreen: the panel is there" 400 480 272c33
kill $bpid 2>/dev/null; wait $bpid 2>/dev/null
sleep 0.5

# 7. undecorated (Motif hints) and a transient dialog
chord_ws() { send "key $SUPER 1"; send "key $CTRL 1"; send "key $1 1"; send "key $1 0"; send "key $CTRL 0"; send "key $SUPER 0"; sleep 0.4; }
chord_ws $RIGHT	# the next workspace is empty: no other window in the way
DISPLAY=$disp "$xcl" -t gamma -c 3d6fa8 -D -T -w 200 -h 120 -s 100 >"$dir/g.out" 2>&1 &
gpid=$!
waitfor "$dir/g.out" "^dialog configure" 15 && sleep 0.4
shot xwl-6
read -r _ gx gy gw gh <<EOF
$(lastcfg "$dir/g.out")
EOF
expect_px "no frame when the client asks for none" $((gx + 5)) $((gy - 6)) 1d2125
expect_px "undecorated client pixels" $((gx + 20)) $((gy + 60)) 3d6fa8
read -r _ _ dx dy dw dh <<EOF
$(lastcfg "$dir/g.out" "dialog ")
EOF
[ -n "$dx" ] && [ "$dx" -gt "$gx" ] && ok "the dialog is placed over its parent ($dx,$dy)" || bad "dialog at '$dx,$dy' parent at $gx,$gy"
n=$(grep -c "window_added.*app_id=xclient title=gamma\|window_added.*title=xclient dialog" "$dir/listen.out")
[ "$n" = 1 ] && ok "the dialog is not a panel entry of its own" || bad "panel entries for gamma and its dialog: $n"
kill $gpid 2>/dev/null; wait $gpid 2>/dev/null
chord_ws $LEFT

# 8. workspaces and Alt-Tab with X windows
chord_ws $RIGHT
shot xwl-7
expect_px "alpha is gone from the other workspace" $((cx + 100)) $((cy + 60)) 1d2125
chord_ws $LEFT
shot xwl-8
expect_px "and back on the first one" $((cx + 100)) $((cy + 60)) 3f9b63
"$shm" -w 150 -h 90 -c ff00ff -t waylandwin -s 100 >"$dir/w.out" 2>&1 &
sleep 0.8
send "key $ALT 1"; send "key $TAB 1"; send "key $TAB 0"
shot xwl-9
send "key $ALT 0"
sleep 0.4
grep "window_changed id=[0-9]* title=gamma focused=1" "$dir/listen.out" >/dev/null || grep -q "focused=1" "$dir/listen.out" && ok "Alt-Tab goes through X and Wayland windows" || bad "Alt-Tab"
send "key $SUPER 1"; send "key $TAB 1"; send "key $TAB 0"; send "key $SUPER 0"
sleep 0.5
shot xwl-10
send "key 1 1"; send "key 1 0"	# Esc closes the overview
sleep 0.3

# 9. the clipboard, Wayland to X and X to Wayland
"$shm" -w 80 -h 60 -c 00ffff -t clipsrc -P "from-wayland" -s 20 >"$dir/cs.out" 2>&1 &
sleep 1.2
DISPLAY=$disp "$xcl" -t paster -c 3d6fa8 -s 8 -a 600:getclip >"$dir/p.out" 2>&1 &
waitfor "$dir/p.out" "^paste:" 10
has "$dir/p.out" "^paste: from-wayland" "clipboard: a Wayland client's text pastes into an X client"
DISPLAY=$disp "$xcl" -t copier -c 3d6fa8 -s 8 -a 400:setclip=from-x11 >"$dir/c.out" 2>&1 &
sleep 1.5
"$shm" -w 80 -h 60 -c ffff00 -t clipdst -Q -s 5 >"$dir/cq.out" 2>&1 &
sleep 2
grep -q "from-x11" "$dir/cq.out" && ok "clipboard: an X client's text reaches a Wayland client" || bad "X to Wayland: $(grep -i clip "$dir/cq.out" | head -3)"

# 10. the title changes live (alpha, 9 s in), and the close button asks the client to quit
waitfor "$dir/listen.out" "title=alpha-two" 12 && ok "a changed title reaches the panel" || bad "retitle did not arrive"
shot xwl-11
read -r _ cx cy cw ch <<EOF
$(lastcfg "$dir/a.out")
EOF
click $((cx + cw + 3 - 23)) $((cy - 15))
waitfor "$dir/a.out" "^delete" 5 && ok "the close button sends WM_DELETE_WINDOW" || bad "close button"
has "$dir/listen.out" "window_removed id=1" "the window is gone from the panel's list"

# 11. Xwayland leaves when the last X client is gone, and comes back for the next one
pkill -f "tests/xclient" 2>/dev/null
n=0; while [ -n "$(xw_pid)" ] && [ $n -lt 80 ]; do n=$((n + 1)); sleep 0.1; done
[ -z "$(xw_pid)" ] && ok "Xwayland ended with its last client" || bad "Xwayland still runs"
sleep 0.3
DISPLAY=$disp "$xcl" -t again -s 6 >"$dir/r.out" 2>&1 &
waitfor "$dir/r.out" "^mapped" 20 && ok "a new X client starts Xwayland again" || bad "second start failed"
[ -n "$(xw_pid)" ] && ok "restarted: one Xwayland again" || bad "no Xwayland after restart"

# 12. flcomp ends: Xwayland and the socket go
kill $comp; wait $comp 2>/dev/null
sleep 1
[ -z "$(pgrep -x Xwayland)" ] && ok "no Xwayland left behind" || bad "Xwayland left behind"
[ ! -e "/tmp/.X11-unix/X${disp#:}" ] && ok "X socket removed" || bad "X socket left behind"

echo
if [ $fails -eq 0 ]; then echo "all Xwayland tests passed"; else echo "$fails FAILED"; fi
exit $((fails > 0))
