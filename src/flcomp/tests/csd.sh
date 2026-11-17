#!/bin/sh
# A client with client-side decorations, like Firefox/GTK on Wayland (tests/csdclient: shadow margin, set_window_geometry):
# maximize and unmaximize, fullscreen, tiling (Super+Left), move and resize with a pointer serial. Checks what the
# client is told (configure size and states) and where the geometry ends up on the screen (the shadow margin must stay
# outside the work area). Headless, needs python3. `make check-csd`.
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
dir=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$dir"' EXIT
export XDG_RUNTIME_DIR=$dir WAYLAND_DISPLAY=wayland-0
export FLCOMP_WALLPAPER= FLCOMP_HEADLESS=800x500 FLCOMP_PANEL= FLCOMP_SESSION= FLCOMP_SCREENSHOT=$dir/shot.ppm
export FLCOMP_TEST_FIFO=$dir/in FLCOMP_KEYBOARD_CONF=$dir/none
mkfifo "$dir/in"
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
./flcomp 2>"$dir/log" &
comp=$!
i=0
while [ ! -S "$dir/wayland-0" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
[ -S "$dir/wayland-0" ] || { cat "$dir/log"; echo "FAIL flcomp did not start"; exit 1; }
shot() { kill -USR1 $comp; sleep 0.5; }
send() { echo "$1" >"$dir/in"; sleep 0.15; }

# bbox SHOT RRGGBB -> "x1 y1 x2 y2" of that colour, or "none"
bbox() {
	python3 -I - "$1" "$2" <<'PY'
import sys
f = open(sys.argv[1], 'rb'); assert f.readline() == b'P6\n'; w, h = map(int, f.readline().split()); f.readline()
d = f.read(); want = bytes.fromhex(sys.argv[2])
xs = []; ys = []
for y in range(h):
    for x in range(w):
        o = (y * w + x) * 3
        if d[o:o + 3] == want: xs.append(x); ys.append(y)
print('none' if not xs else '%d %d %d %d' % (min(xs), min(ys), max(xs), max(ys)))
PY
}
last() { grep "$2" "$dir/$1.out" | tail -n 1; }

# ---- 1. maximize and unmaximize: the client drops its margin, the geometry fills the work area ----------------------
./tests/csdclient -g 300x200 -m 24 -c 5577aa -S "sleep 600;max;sleep 1500;unmax;sleep 1500" >"$dir/a.out" &
cl=$!
sleep 1.0; shot
set -- $(bbox "$dir/shot.ppm" 5577aa)
[ "$1" != none ] && [ $(($3 - $1 + 1)) -eq 300 ] && [ $(($4 - $2 + 1)) -eq 200 ] && ok "the window shows its 300x200 geometry, not the 24 px shadow ($*)" || bad "normal window geometry on screen: $*"
sleep 1.0; shot
grep -q "configure 800x500 states: 1" "$dir/a.out" || grep -q "configure 800x500 states:.* 1\b" "$dir/a.out"
c=$(grep "configure 800x500" "$dir/a.out" | tail -n 1)
case "$c" in *" 1"*) ok "set_maximized: configure 800x500 with the maximized state ($c)";; *) bad "set_maximized: no 800x500 maximized configure ($(grep configure "$dir/a.out" | tr '\n' '|'))";; esac
case "$c" in *" 4"*) ok "... and the activated state";; *) bad "no activated state in the maximized configure: $c";; esac
grep -q "commit geom 800x500 margin 0" "$dir/a.out" && ok "the client drew 800x500 without a margin" || bad "no 800x500 commit: $(grep commit "$dir/a.out" | tail -n 2 | tr '\n' '|')"
set -- $(bbox "$dir/shot.ppm" 5577aa)
[ "$*" = "0 0 799 499" ] && ok "maximized geometry fills the work area exactly (0 0 799 499)" || bad "maximized geometry on screen: $*"
wait $cl
c=$(grep "^configure" "$dir/a.out" | tail -n 1)
case "$c" in "configure 300x200 states:"*) case "$c" in *" 1"*) bad "unmaximized still has the maximized state: $c";; *) ok "unset_maximized: back to 300x200 without the maximized state";; esac;; *) bad "unset_maximized: last configure is: $c";; esac
grep -q "commit geom 300x200 margin 24" "$dir/a.out" && ok "the shadow margin is back after unmaximize" || bad "no commit with the margin after unmaximize"

# ---- 2. a client that keeps its margin when maximized: set_window_geometry places the geometry, not the buffer ---------
./tests/csdclient -g 300x200 -m 24 -k -c 5577aa -S "sleep 600;max;sleep 2500" >"$dir/b.out" &
cl=$!
sleep 2.6; shot
set -- $(bbox "$dir/shot.ppm" 5577aa)
[ "$*" = "0 0 799 499" ] && ok "kept margin: the geometry (not the shadow) is at the work area (0 0 799 499)" || bad "kept margin: maximized geometry on screen: $* (want 0 0 799 499)"
wait $cl

# ---- 3. fullscreen ---------------------------------------------------------------------------------------------------
./tests/csdclient -g 300x200 -m 24 -c 5577aa -S "sleep 600;full;sleep 1500;unfull;sleep 1200" >"$dir/f.out" &
cl=$!
sleep 2.4
c=$(grep "configure 800x500" "$dir/f.out" | tail -n 1)
case "$c" in *" 2"*) ok "set_fullscreen: configure 800x500 with the fullscreen state ($c)";; *) bad "set_fullscreen configure: $(grep configure "$dir/f.out" | tr '\n' '|')";; esac
wait $cl

# ---- 4. tiling: Super+Left is a half; the client is told it is tiled ---------------------------------------------------
./tests/csdclient -g 300x200 -m 24 -c 5577aa -S "sleep 3500" >"$dir/t.out" &
cl=$!
sleep 1.2
send "key 125 1"; send "key 105 1"; send "key 105 0"; send "key 125 0"
sleep 0.8
c=$(grep "configure 400x500" "$dir/t.out" | tail -n 1)
[ -n "$c" ] && ok "Super+Left: configure 400x500 ($c)" || bad "Super+Left configure: $(grep configure "$dir/t.out" | tr '\n' '|')"
case "$c" in *" 5"*" 7"*" 8"*) ok "... with the tiled-left, -top and -bottom states (5 7 8)";; *) bad "a half-screen window is not marked tiled: $c";; esac
wait $cl

# ---- 5. move and resize with the serial of a pointer press (the CSD title bar and the border) -------------------------
./tests/csdclient -g 300x200 -m 24 -c 5577aa -S "sleep 500;move;sleep 3500" >"$dir/m.out" &
cl=$!
sleep 1.0; shot
set -- $(bbox "$dir/shot.ppm" 5577aa); x1=$1; y1=$2
px=$((x1 + 100)); py=$((y1 + 20))
send "move $px $py"; send "button 272 1"
sleep 0.4
grep -q "request move" "$dir/m.out" && ok "xdg_toplevel.move was sent with a press serial" || bad "no move request: $(cat "$dir/m.out" | tr '\n' '|')"
send "move $((px + 80)) $((py + 50))"; send "move $((px + 120)) $((py + 70))"; send "button 272 0"
sleep 0.5; shot
set -- $(bbox "$dir/shot.ppm" 5577aa)
dx=$(($1 - x1)); dy=$(($2 - y1))
[ "$dx" -ge 100 ] && [ "$dx" -le 140 ] && [ "$dy" -ge 60 ] && [ "$dy" -le 80 ] && ok "dragging after xdg_toplevel.move moved the window by $dx,$dy" || bad "window moved by $dx,$dy after the move request (want about 120,70)"
wait $cl
./tests/csdclient -g 300x200 -m 24 -c 5577aa -S "sleep 500;resize br;sleep 3500" >"$dir/r.out" &
cl=$!
sleep 1.0; shot
set -- $(bbox "$dir/shot.ppm" 5577aa); x2=$3; y2=$4
send "move $((x2 - 5)) $((y2 - 5))"; send "button 272 1"
sleep 0.4
grep -q "request resize" "$dir/r.out" && ok "xdg_toplevel.resize was sent with a press serial" || bad "no resize request: $(cat "$dir/r.out" | tr '\n' '|')"
send "move $((x2 + 40)) $((y2 + 30))"; send "move $((x2 + 60)) $((y2 + 40))"
sleep 0.5
c=$(grep "^configure" "$dir/r.out" | tail -n 1)
case "$c" in "configure 3"[3-9]"0x2"[3-9]"0 states:"*|"configure 3"[3-9]*x2*) case "$c" in *" 3"*) ok "resize: the client got a bigger size with the resizing state ($c)";; *) bad "resize: no resizing state: $c";; esac;; *) bad "resize: last configure is $c (want about 360x240)";; esac
send "button 272 0"
wait $cl
kill $comp 2>/dev/null
[ $fails -eq 0 ] && echo "csd: all ok" || echo "csd: $fails FAILED"
exit $fails
