#!/bin/sh
# wl_shm buffer whose stride is its width (not 4 x width): flcomp must answer with a protocol error and stay up,
# not read past the pool. Headless, no root.
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
dir=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$dir"' EXIT
export XDG_RUNTIME_DIR=$dir FLCOMP_WALLPAPER= FLCOMP_HEADLESS=640x400 FLCOMP_PANEL= FLCOMP_SESSION=
export FLCOMP_KEYBOARD_CONF=$dir/none
./flcomp 2>"$dir/log" &
comp=$!
i=0
while [ ! -S "$dir/wayland-0" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
[ -S "$dir/wayland-0" ] || { cat "$dir/log"; echo "FAIL flcomp did not start"; exit 1; }
fails=0
./tests/shmclient -E -w 1000 -h 1000 -s 2 >"$dir/e.out" 2>&1
grep -q '^protocol_error' "$dir/e.out" && echo "ok   stride == width: the client got a protocol error" || { echo "FAIL no protocol error ($(cat "$dir/e.out"))"; fails=1; }
kill -0 $comp 2>/dev/null && echo "ok   flcomp is still running" || { echo "FAIL flcomp died"; fails=1; }
# and a good client still works afterwards
./tests/shmclient -w 100 -h 60 -s 1 >"$dir/g.out" 2>&1; kill -0 $comp 2>/dev/null && echo "ok   a normal client afterwards" || { echo "FAIL flcomp died after a normal client"; fails=1; }
exit $fails
