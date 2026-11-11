#!/bin/sh
# flcomp's resident memory (PSS from /proc/PID/smaps_rollup) in a headless desktop with the real panel
# and two windows: at idle, after a burst of every kind of animation, and a few seconds later. Compare two
# binaries: FLCOMP=old tests/pss.sh; FLCOMP=new tests/pss.sh. FLPANEL names the panel (default flpanel).
# HOME and the runtime directory are scratch; nothing outside them is touched.
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
comp_bin=${FLCOMP:-./flcomp}
dir=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$dir"' EXIT
export XDG_RUNTIME_DIR=$dir HOME=$dir FLUI_CONF=$dir/desktop.conf
export FLCOMP_WALLPAPER= FLCOMP_HEADLESS=${SIZE:-1280x800} FLCOMP_PANEL=${FLPANEL:-flpanel} FLCOMP_SESSION=
export FLCOMP_TEST_FIFO=$dir/in FLCOMP_KEYBOARD_CONF=$dir/none FLCOMP_SCREENSHOT=$dir/shot.ppm
unset WAYLAND_DISPLAY
shm=$(pwd)/tests/shmclient
mkfifo "$dir/in"
echo "animations=${ANIM:-on}" >"$dir/desktop.conf"
"$comp_bin" 2>"$dir/log" &
comp=$!
i=0
while [ ! -S "$dir/wayland-0" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
pss() { awk '/^Pss:/{a=$2} /^Pss_Anon:/{b=$2} /^Pss_File:/{c=$2} /^Rss:/{r=$2} END{printf "Pss %d kB (anon %d, file %d), Rss %d kB\n", a, b, c, r}' /proc/$comp/smaps_rollup; }
send() { echo "$1" >"$dir/in"; sleep 0.12; }
chord() { for m in $1; do send "key $m 1"; done; send "key $2 1"; send "key $2 0"; for m in $1; do send "key $m 0"; done; }
sleep 1
"$shm" -w 640 -h 420 -c 3f9b63 -t one -s 100 >/dev/null &
"$shm" -w 500 -h 300 -c 3366aa -t two -s 100 >/dev/null &
sleep 3
echo "idle, two windows:        $(pss)"
for n in 1 2 3; do
	chord "125 29" 106; sleep 0.5; chord "125 29" 105; sleep 0.5
	chord "125" 15; sleep 0.5; chord "" 1; sleep 0.5
	chord "125" 103; sleep 0.6; chord "125" 108; sleep 0.6
	"$shm" -w 400 -h 300 -c aa6633 -t tmp$n -s 1 >/dev/null; sleep 0.6
done
echo "right after the motion:   $(pss)"
sleep 4
echo "four seconds later:       $(pss)"
grep -c "^anim:" "$dir/log" | sed 's/^/animation bursts logged: /'
