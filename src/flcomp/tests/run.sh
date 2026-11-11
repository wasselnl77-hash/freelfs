#!/bin/sh
# Headless flcomp test: no display, no input hardware, no root.
# Starts flcomp with FLCOMP_HEADLESS, connects tests/shmclient as panel and
# windows, drives fake input through FLCOMP_TEST_FIFO and checks screenshots.
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
dir=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$dir"' EXIT
export XDG_RUNTIME_DIR=$dir
export FLCOMP_WALLPAPER= FLCOMP_HEADLESS=640x400 FLCOMP_PANEL= FLCOMP_SESSION= FLCOMP_SCREENSHOT=$dir/shot.ppm
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

shot() { kill -USR1 $comp; sleep 0.4; }
# pixel FILE X Y -> RRGGBB
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

./tests/shmclient -l -s 100 >"$dir/listen.out" &
./tests/shmclient -p 40 -c 272c33 -s 100 >"$dir/panel.out" &
panel_pid=$!
sleep 0.5
./tests/shmclient -w 200 -h 100 -c 3f9b63 -t one -U -s 100 >"$dir/w1.out" &
sleep 0.5
shot
expect_px "background" 5 5 1d2125
expect_px "panel at bottom" 300 380 272c33
# window one is centred with cascade offset: x=160..360 y=70..170
expect_px "window one" 200 100 3f9b63
expect_px "above panel, below window" 100 340 1d2125
grep -q "window_added id=1 app_id=shmclient title=one" "$dir/listen.out" && ok "window_added" || bad "window_added"
grep -q "window_changed id=1 title=one focused=1 minimized=0" "$dir/listen.out" && ok "window focused" || bad "window focused"
grep -q "output 640x400" "$dir/w1.out" && ok "wl_output mode" || bad "wl_output mode"

# popup with grab: opens at parent + (30,30), a click elsewhere dismisses it
send "move 170 80"
send "button 272 1"
send "button 272 0"
sleep 0.3
shot
expect_px "popup placed by anchor and gravity" 200 110 ff00ff
grep -q "popup_configure 30 30 50 30" "$dir/w1.out" && ok "popup configure" || bad "popup configure"
send "move 500 200"
send "button 272 1"
send "button 272 0"
shot
expect_px "popup dismissed by outside click" 200 110 3f9b63
grep -q popup_done "$dir/w1.out" && ok "popup_done" || bad "popup_done"

# second window over the first, via a second client
./tests/shmclient -w 150 -h 80 -c ff0000 -t two -s 10 >"$dir/w2.out" &
sleep 0.5
shot
# cascade: window two at x=209..359 y=104..184
expect_px "window two on top" 220 120 ff0000
expect_px "window one still under" 350 100 3f9b63

# pointer: click window one's visible part -> raised + focused; client one gets button
send "move 340 80"
send "button 272 1"
send "button 272 0"
shot
expect_px "click raises window one" 300 160 3f9b63
grep -q "pointer_enter 180 10" "$dir/w1.out" && ok "pointer_enter coords" || bad "pointer_enter coords"
grep -q "pointer_button 272 1" "$dir/w1.out" && ok "button delivered" || bad "button delivered"
grep -q "window_changed id=1 title=one focused=1" "$dir/listen.out" && ok "focus event" || bad "focus event"

# click on the panel does not change focus, clicking the desktop is harmless
send "move 100 390"
send "button 272 1"
send "button 272 0"
grep -q "pointer_button" "$dir/panel.out" && ok "panel gets pointer" || bad "panel gets pointer"

# keyboard goes to the focused window
send "key 30 1"
send "key 30 0"
grep -q "key 30 1" "$dir/w1.out" && ok "key to focused window" || bad "key to focused window"

# Super tap -> menu_toggle
send "key 125 1"
send "key 125 0"
grep -q menu_toggle "$dir/listen.out" && ok "menu_toggle on Super" || bad "menu_toggle on Super"

# Alt-Tab: focus goes to the window below the top one
send "key 56 1"
send "key 15 1"
send "key 15 0"
send "key 56 0"
sleep 0.2
grep -q "window_changed id=2 title=two focused=1" "$dir/listen.out" && ok "alt-tab focuses window two" || bad "alt-tab"

# alpha window blends over the background: blue at 50% over red window two
./tests/shmclient -w 60 -h 30 -c 0000ff -a 128 -t alpha -s 3 >"$dir/wa.out" &
sleep 0.5
shot
got=$(pixel "$dir/shot.ppm" 290 160)
r=$((0x$(echo $got | cut -c1-2))); b=$((0x$(echo $got | cut -c5-6)))
[ $r -ge 125 ] && [ $r -le 130 ] && [ $b -ge 126 ] && [ $b -le 130 ] && ok "alpha blend ($got)" || bad "alpha blend: $got"
sleep 3

# move by title bar drag: window "mover" starts a move on button press
./tests/shmclient -w 100 -h 60 -c 0000ff -t mover -M -s 8 >"$dir/wm.out" &
sleep 0.5
shot
# newest window: cascade slot 4 -> x=(640-100)/2-60+96=306.. y=(360-60)/2-60+96=186..
expect_px "mover mapped" 320 200 0000ff
send "move 320 200"
send "button 272 1"
send "move 340 190"
send "move 370 180"
send "button 272 0"
shot
expect_px "mover moved right/up" 390 190 0000ff
expect_px "mover left behind" 310 245 1d2125

# resize by bottom-right drag
./tests/shmclient -w 100 -h 60 -c ffff00 -t sizer -Z -s 6 >"$dir/wz.out" &
sleep 0.5
shot
send "move 400 240"
send "button 272 1"
send "move 440 262"
send "move 450 270"
send "button 272 0"
shot
grep -q "configure 150x90 states: 4" "$dir/wz.out" && ok "resize configure 150x90" || bad "resize configure: $(grep configure "$dir/wz.out" | tail -2)"
expect_px "sizer grew" 440 255 ffff00

# maximized window fills the work area above the panel
./tests/shmclient -w 100 -h 60 -c 00ffff -t big -X -s 5 >"$dir/wx.out" &
sleep 0.6
shot
expect_px "maximized top-left" 2 2 00ffff
expect_px "maximized bottom-right of work area" 637 357 00ffff
expect_px "panel stays over maximized window" 300 380 272c33
grep -q "configure 640x360 states: 1" "$dir/wx.out" && ok "maximized configure size and state" || bad "maximized configure: $(grep configure "$dir/wx.out" | head -2)"

# panel asks to minimize the maximized window (id 6) and to switch the layout
./tests/shmclient -l -s 4 -C "minimize:6 layout:de" >"$dir/ctl.out" &
sleep 1.8
shot
grep -q "window_changed id=6 title=big focused=0 minimized=1" "$dir/listen.out" && ok "minimize event" || bad "minimize event"
expect_px "minimized window gone" 2 2 1d2125
grep -q "keyboard_layout de" "$dir/listen.out" && ok "layout switch event" || bad "layout switch"

# start menu: shown above the panel, click outside dismisses it, focus returns
./tests/shmclient -m -s 6 >"$dir/menu.out" &
sleep 0.5
shot
expect_px "menu above panel" 20 300 3f9b63
send "move 300 100"
send "button 272 1"
send "button 272 0"
shot
grep -q menu_dismissed "$dir/listen.out" && ok "menu_dismissed" || bad "menu_dismissed"
grep -q kb_enter "$dir/menu.out" && ok "menu has keyboard focus" || bad "menu keyboard focus"

# clipboard: client P sets the selection when it gets focus, Q reads it
./tests/shmclient -w 60 -h 40 -c 111111 -t clipsrc -P "hello flcomp" -s 6 >"$dir/cp.out" &
sleep 0.6
./tests/shmclient -w 60 -h 40 -c 222222 -t clipdst -Q -s 5 >"$dir/cq.out" &
sleep 0.8
grep -q "clip: hello flcomp" "$dir/cq.out" && ok "clipboard copy between clients" || bad "clipboard: $(cat "$dir/cq.out" | grep clip)"

# dmabuf: a client sends its buffers through zwp_linux_dmabuf_v1 (a memfd stands
# in for the dmabuf; the compositor reads linear buffers by mmap), as XRGB, ARGB
# with alpha, and ABGR (red and blue swapped in memory)
./tests/shmclient -w 60 -h 40 -c ff8800 -t dmabuf -D -s 6 >"$dir/db.out" &
sleep 0.6
grep -q "dmabuf_modifier 34325258 0:0" "$dir/db.out" && ok "dmabuf formats announced" || bad "dmabuf formats: $(grep dmabuf "$dir/db.out" | head -2)"
shot
# newest window: cascade slot after the clipboard windows; find it by colour
dbfound=$(python3 -I - "$dir/shot.ppm" <<'PY'
import sys
f = open(sys.argv[1], 'rb'); f.readline(); w, h = map(int, f.readline().split()); f.readline()
d = f.read(); n = sum(1 for i in range(0, len(d), 3) if d[i:i+3] == b'\xff\x88\x00')
print(n)
PY
)
[ "$dbfound" -ge 2000 ] && ok "dmabuf window shows its pixels ($dbfound)" || bad "dmabuf window pixels: $dbfound"
./tests/shmclient -w 60 -h 40 -c 0088ff -t dmabuf-abgr -B -s 6 >"$dir/db2.out" &
sleep 0.6
shot
dbfound=$(python3 -I - "$dir/shot.ppm" <<'PY'
import sys
f = open(sys.argv[1], 'rb'); f.readline(); w, h = map(int, f.readline().split()); f.readline()
d = f.read(); n = sum(1 for i in range(0, len(d), 3) if d[i:i+3] == b'\x00\x88\xff')
print(n)
PY
)
[ "$dbfound" -ge 2000 ] && ok "ABGR dmabuf colours swapped back ($dbfound)" || bad "ABGR dmabuf pixels: $dbfound"
./tests/shmclient -w 60 -h 40 -c 0088ff -a 255 -t dmabuf-argb -D -s 3 >"$dir/db3.out" &
sleep 0.6
grep -q "mapped 60x40" "$dir/db3.out" && ok "ARGB dmabuf accepted" || bad "ARGB dmabuf"

# clients dying mid-grab, panel dying: the compositor must survive
./tests/shmclient -w 80 -h 80 -c 123456 -t victim -M -X -s 30 >"$dir/v.out" &
victim=$!
sleep 0.5
send "move 330 250"
send "button 272 1"
send "move 340 255"
grep -q pointer_button "$dir/v.out" && ok "victim was grabbed" || bad "victim not grabbed (grab path untested)"
kill -9 $victim
wait $victim 2>/dev/null
sleep 0.2
send "move 350 260"
send "button 272 0"
kill -9 $panel_pid
wait $panel_pid 2>/dev/null
sleep 0.3
shot
kill -0 $comp 2>/dev/null && ok "survives clients dying mid-grab and the panel dying" || bad "compositor died"

# Ctrl-Alt-Backspace ends the session with status 0
send "key 29 1"
send "key 56 1"
send "key 14 1"
sleep 0.4
if kill -0 $comp 2>/dev/null; then
	bad "Ctrl-Alt-Backspace did not quit"
	kill $comp
else
	wait $comp && ok "Ctrl-Alt-Backspace quits with 0" || bad "exit status $?"
fi
echo "flcomp log:"; sed 's/^/  /' "$dir/log"
[ $fails = 0 ] && echo "all tests passed" || echo "$fails test(s) failed"
exit $fails
