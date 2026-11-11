#!/bin/sh
# fllock end to end: a headless flcomp, a window, Super+L, typing a wrong and a right password
# (checked by fllock-check against a made-up shadow file), unlock. Needs ../flcomp built
# (flcomp, tests/shmclient), python3 and openssl. `make e2e`; E2E_OUT=DIR keeps the screenshots (PPM).
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
here=$(pwd)
comp=$here/../flcomp
dir=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$dir"' EXIT
export XDG_RUNTIME_DIR=$dir HOME=$dir
export FLCOMP_WALLPAPER= FLCOMP_HEADLESS=${E2E_SIZE:-800x500} FLCOMP_PANEL= FLCOMP_SESSION= FLCOMP_SCREENSHOT=$dir/shot.ppm
export FLCOMP_TEST_FIFO=$dir/in FLCOMP_KEYBOARD_CONF=$dir/none
me=$(id -un)
hash=$(openssl passwd -6 -salt abcdefgh abc) || exit 1
printf '%s:%s:19000:0:99999:7:::\n' "$me" "$hash" >"$dir/shadow"
export FLLOCK_SHADOW=$dir/shadow FLLOCK_LOCKFILE=$dir/lockfile FLLOCK_CHECK=$here/fllock-check
export FLCOMP_LOCK=$here/fllock
mkfifo "$dir/in"
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
"$comp/flcomp" 2>"$dir/log" &
cpid=$!
i=0
while [ ! -S "$dir/wayland-0" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
shot() { kill -USR1 $cpid; sleep 0.5; [ -n "$E2E_OUT" ] && cp "$dir/shot.ppm" "$E2E_OUT/$1.ppm"; return 0; }
pixel() {
	python3 -I - "$@" <<'PY'
import sys
f = open(sys.argv[1], 'rb'); x = int(sys.argv[2]); y = int(sys.argv[3])
assert f.readline() == b'P6\n'; w, h = map(int, f.readline().split()); f.readline()
d = f.read(); o = (y * w + x) * 3
print('%02x%02x%02x' % tuple(d[o:o + 3]))
PY
}
send() { echo "$1" >"$dir/in"; sleep 0.08; }
tap() { send "key $1 1"; send "key $1 0"; }
BG=1d2125
sz=${E2E_SIZE:-800x500}
cx=$((${sz%x*} / 2 - 60))
cy=$((${sz#*x} / 2 - 60))
"$comp/tests/shmclient" -w 200 -h 100 -c 3f9b63 -t one -s 40 >"$dir/w1.out" &
sleep 0.6
shot desktop
[ "$(pixel "$dir/shot.ppm" $cx $cy)" = 3f9b63 ] && ok "the window is there" || bad "window not mapped"

send "key 125 1"; send "key 38 1"; send "key 38 0"; send "key 125 0"
sleep 1.2
shot 1-locked
[ "$(pixel "$dir/shot.ppm" $cx $cy)" = $BG ] && ok "Super+L: the window is gone, fllock shows" || bad "not locked"
grep -q "pointer_button\|key " "$dir/w1.out" && bad "window saw input" || ok "no input for the window"

tap 45; tap 21; tap 44	# x y z
shot 2-typing
tap 28
sleep 0.4
shot 3-checking
sleep 1.6
shot 4-wrong
[ "$(pixel "$dir/shot.ppm" $cx $cy)" = $BG ] && ok "a wrong password keeps it locked" || bad "unlocked by a wrong password"
tap 30; tap 48; tap 46	# a b c
tap 28
sleep 1.5
shot 5-unlocked
[ "$(pixel "$dir/shot.ppm" $cx $cy)" = 3f9b63 ] && ok "the right password unlocks" || bad "right password did not unlock"
sleep 0.3
if pgrep -f "$here/fllock" >/dev/null; then bad "fllock still running"; else ok "fllock ended"; fi
kill -0 $cpid 2>/dev/null && ok "compositor alive" || bad "compositor died"
[ $fails = 0 ] && echo "all tests passed" || { echo "$fails test(s) failed"; cat "$dir/log"; }
exit $fails
