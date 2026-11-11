#!/bin/sh
# Headless flcomp test of the lock screen (fl_shell lock/unlock), the notification surface
# (set_notifications) and power saving. Same harness as run.sh; nothing outside a scratch
# directory is touched. `make check-lock`; LOCK_OUT=DIR keeps screenshots there (PPM).
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
dir=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$dir"' EXIT
export XDG_RUNTIME_DIR=$dir HOME=$dir FLUI_CONF=$dir/desktop.conf
export FLCOMP_WALLPAPER= FLCOMP_HEADLESS=640x400 FLCOMP_PANEL= FLCOMP_SESSION= FLCOMP_SCREENSHOT=$dir/shot.ppm
export FLCOMP_TEST_FIFO=$dir/in FLCOMP_KEYBOARD_CONF=$dir/none
shm=$(pwd)/tests/shmclient
# the "lock program": a shmclient lock screen, unless $dir/lock-broken exists (then it dies at once)
cat >"$dir/fllock" <<EOF
#!/bin/sh
echo run >>"$dir/lock-runs"
[ -e "$dir/lock-broken" ] && exit 1
exec "$shm" -L -c 00ff00 -s 20 >>"$dir/lock.out"
EOF
chmod +x "$dir/fllock"
export FLCOMP_LOCK=$dir/fllock
mkfifo "$dir/in"
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

./flcomp 2>"$dir/log" &
comp=$!
i=0
while [ ! -S "$dir/wayland-0" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
[ -S "$dir/wayland-0" ] || { cat "$dir/log"; echo "FAIL flcomp did not start"; exit 1; }

shot() { kill -USR1 $comp; sleep 0.4; [ -n "$LOCK_OUT" ] && cp "$dir/shot.ppm" "$LOCK_OUT/$1.ppm"; return 0; }
pixel() {
	python3 -I - "$@" <<'PY'
import sys
f = open(sys.argv[1], 'rb'); x = int(sys.argv[2]); y = int(sys.argv[3])
assert f.readline() == b'P6\n'; w, h = map(int, f.readline().split()); f.readline()
d = f.read(); o = (y * w + x) * 3
print('%02x%02x%02x' % tuple(d[o:o + 3]))
PY
}
# number of pixels in the screenshot that are not colour $1
others() {
	python3 -I - "$dir/shot.ppm" "$1" <<'PY'
import sys
f = open(sys.argv[1], 'rb'); f.readline(); w, h = map(int, f.readline().split()); f.readline()
d = f.read(); c = bytes.fromhex(sys.argv[2])
print(sum(1 for i in range(0, len(d), 3) if d[i:i+3] != c))
PY
}
expect_px() {	# name x y colour
	got=$(pixel "$dir/shot.ppm" "$2" "$3")
	[ "$got" = "$4" ] && ok "$1" || bad "$1: ($2,$3) is $got, wanted $4"
}
send() { echo "$1" >"$dir/in"; sleep 0.15; }
click() { send "move $1 $2"; send "button 272 1"; send "button 272 0"; }

"$shm" -p 40 -c 272c33 -s 60 >"$dir/panel.out" &
sleep 0.4
"$shm" -l -s 60 >"$dir/listen.out" &
"$shm" -w 200 -h 100 -c 3f9b63 -t one -s 60 >"$dir/w1.out" &
sleep 0.6

# ---- the notification surface ----------------------------------------------------------
"$shm" -N -w 100 -h 50 -c ff00ff -s 60 >"$dir/n.out" &
sleep 0.5
shot notify
# top right, 16 px in: x 524..624, y 16..66; the left half is drawn, the right half is transparent
expect_px "notification card at the top right" 540 30 ff00ff
got=$(pixel "$dir/shot.ppm" 600 30)
[ "$got" != "ff00ff" ] && ok "transparent half shows what is below ($got)" || bad "transparent half is drawn"
click 540 30
grep -q "pointer_button 272 1" "$dir/n.out" && ok "click on a card reaches it" || bad "click on a card"
n1=$(grep -c "pointer_button 272 1" "$dir/n.out")
click 600 30
n2=$(grep -c "pointer_button 272 1" "$dir/n.out")
[ "$n1" = "$n2" ] && ok "the transparent half takes no click" || bad "the transparent half took a click ($n1 -> $n2)"

# ---- Super+L locks at once and starts the lock program ----------------------------------
send "key 125 1"
send "key 38 1"
send "key 38 0"
send "key 125 0"
sleep 0.8
shot locked
[ -e "$dir/lock-runs" ] && ok "Super+L ran the lock program" || bad "Super+L did not run it"
expect_px "locked: lock surface covers the desktop" 5 5 00ff00
expect_px "locked: no panel" 300 380 00ff00
expect_px "locked: no window" 200 100 00ff00
expect_px "locked: no notification card" 540 30 00ff00

# input only goes to the lock surface
wc0=$(grep -c window_changed "$dir/listen.out")
: >"$dir/w1.out"
click 300 200
send "key 30 1"
send "key 30 0"
grep -q "pointer_button\|key 30" "$dir/w1.out" && bad "the window got input while locked" || ok "the window gets no input while locked"
grep -q "pointer_button 272 1" "$dir/lock.out" && ok "the lock surface gets the click" || bad "the lock surface got no click"
grep -q "key 30 1" "$dir/lock.out" && ok "and the key" || bad "the lock surface got no key"
send "key 125 1"
send "key 125 0"
grep -q menu_toggle "$dir/listen.out" && bad "Super opened the menu while locked" || ok "no menu_toggle while locked"
send "key 29 1"
send "key 56 1"
send "key 15 1"
send "key 15 0"
send "key 56 0"
send "key 29 0"
send "key 14 1"
send "key 14 0"
sleep 0.2
kill -0 $comp 2>/dev/null && ok "Ctrl-Alt-Tab/Backspace do nothing while locked" || bad "compositor quit"
[ "$(grep -c window_changed "$dir/listen.out")" = "$wc0" ] && ok "focus stays while locked" || { bad "focus moved while locked"; grep window_changed "$dir/listen.out"; }
sleep 0.2
shot locked2
expect_px "still locked after the keys" 300 380 00ff00

# a second lock request is refused (the compositor kills that client)
"$shm" -L -c ff0000 -s 3 >"$dir/lock2.out" 2>&1 &
sleep 0.6
shot second
expect_px "second lock client did not replace the first" 5 5 00ff00
# somebody else cannot unlock
"$shm" -l -C "unlock:1" -s 3 >"$dir/unl.out" 2>&1 &
sleep 1.8
shot nounlock
expect_px "unlock from another client is ignored" 5 5 00ff00

# ---- the lock client crashes: still locked, started again ---------------------------------
n0=$(wc -l <"$dir/lock-runs")
pkill -9 -f "shmclient -L -c 00ff00"
sleep 0.15
shot dead
expect_px "locked while the lock program is dead" 5 5 1d2125
expect_px "no window while the lock program is dead" 200 100 1d2125
sleep 1.0
n1=$(wc -l <"$dir/lock-runs")
[ "$n1" -gt "$n0" ] && ok "the lock program was started again" || bad "no restart ($n0 -> $n1)"
shot restarted
expect_px "lock screen is back" 5 5 00ff00

# ---- a lock program that keeps failing: solid colour and a hint ------------------------------
touch "$dir/lock-broken"
pkill -9 -f "shmclient -L -c 00ff00"
sleep 3.5
shot hint
cnt=$(others 1d2125)
[ "$cnt" -gt 500 ] && ok "hint text is shown ($cnt pixels)" || bad "no hint text ($cnt pixels)"
expect_px "hint screen hides the desktop" 300 380 1d2125
click 300 200
send "key 28 1"
send "key 28 0"
sleep 2.6
shot stillbroken
rm -f "$dir/lock-broken"
send "key 28 1"
send "key 28 0"
sleep 0.9
shot recovered
expect_px "Enter on the hint screen starts the lock program again" 5 5 00ff00

# ---- unlock by the owner ---------------------------------------------------------------------
pkill -f "shmclient -L -c 00ff00"
# the dying client leaves the lock in place; a fresh one that unlocks after one second
sleep 0.2
cat >"$dir/fllock" <<EOF
#!/bin/sh
echo run >>"$dir/lock-runs"
exec "$shm" -L -c 00ff00 -C "unlock:1" -s 4 >>"$dir/lock.out"
EOF
sleep 3.2
shot unlocked
expect_px "unlocked: window is back" 200 100 3f9b63
expect_px "unlocked: panel is back" 300 380 272c33
expect_px "unlocked: card is back" 540 30 ff00ff
: >"$dir/w1.out"
send "key 31 1"
send "key 31 0"
grep -q "key 31 1" "$dir/w1.out" && ok "keyboard goes to the window again" || bad "keyboard after unlock: $(cat "$dir/w1.out" | tail -3)"

# ---- the lid ------------------------------------------------------------------------------------
send "lid 1"
sleep 0.5
shot lid
expect_px "closing the lid locks" 5 5 00ff00
sleep 3
shot lid-unlocked
expect_px "and the lock program unlocked again" 200 100 3f9b63

# ---- power saving: the backlight goes to 70 % and back -----------------------------------------------
mkdir "$dir/bl"
echo 100 >"$dir/bl/max_brightness"
echo 80 >"$dir/bl/brightness"
kill $comp 2>/dev/null; wait $comp 2>/dev/null
export FLCOMP_BACKLIGHT=$dir/bl
./flcomp 2>"$dir/log2" &
comp=$!
i=0
while [ ! -S "$dir/wayland-0" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
sleep 0.3
echo "power_saver=1" >"$dir/desktop.conf"
sleep 0.6
b=$(cat "$dir/bl/brightness")
[ "$b" = 56 ] && ok "power saving dims to 70% (80 -> $b)" || bad "dim: brightness is $b, wanted 56"
echo "power_saver=0" >"$dir/desktop.conf"
sleep 0.6
b=$(cat "$dir/bl/brightness")
[ "$b" = 80 ] && ok "and restores it ($b)" || bad "restore: brightness is $b, wanted 80"
echo "power_saver=1" >"$dir/desktop.conf"
sleep 0.4
echo 40 >"$dir/bl/brightness"
echo "power_saver=0" >"$dir/desktop.conf"
sleep 0.5
b=$(cat "$dir/bl/brightness")
[ "$b" = 40 ] && ok "a brightness changed by hand is kept ($b)" || bad "manual brightness lost: $b"
kill -0 $comp 2>/dev/null && ok "compositor alive" || bad "compositor died"
kill $comp 2>/dev/null; wait $comp 2>/dev/null

# ---- the login screen's compositor (--firstboot) never locks: Super+L and the lid do nothing ------------------
unset FLCOMP_BACKLIGHT
n0=$(wc -l <"$dir/lock-runs")
printf '#!/bin/sh\nsleep 6\n' >"$dir/setup"
chmod +x "$dir/setup"
rm -f "$dir/wayland-0"
FLCOMP_SETUP=$dir/setup ./flcomp --firstboot 2>"$dir/log3" &
comp=$!
i=0
while [ ! -S "$dir/wayland-0" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
sleep 0.3
send "key 125 1"; send "key 38 1"; send "key 38 0"; send "key 125 0"
send "lid 1"
sleep 0.8
n1=$(wc -l <"$dir/lock-runs")
[ "$n0" = "$n1" ] && ok "--firstboot: Super+L and the lid do not start the lock program" || bad "--firstboot ran the lock program"
shot firstboot
expect_px "--firstboot: not locked (the flat desktop colour, no lock surface)" 5 5 1d2125
kill $comp 2>/dev/null; wait $comp 2>/dev/null
echo "flcomp log:"; sed 's/^/  /' "$dir/log"
[ $fails = 0 ] && echo "all tests passed" || echo "$fails test(s) failed"
exit $fails
