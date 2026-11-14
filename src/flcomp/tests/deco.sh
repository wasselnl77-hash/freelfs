#!/bin/sh
# xdg-decoration: a client that asks for server-side (or does not choose) gets flcomp's title bar and frame; a client
# that asks for client-side keeps its own; the title bar menu opens on a right click. Headless, needs python3.
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
dir=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$dir"' EXIT
export XDG_RUNTIME_DIR=$dir
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

# probe SHOT -> "x1 y1 x2 y2 above_left above_mid" : bounding box of the client's colour (5577aa) and what is 6 px above it
probe() {
	python3 -I - "$1" <<'PY'
import sys
f = open(sys.argv[1], 'rb'); assert f.readline() == b'P6\n'; w, h = map(int, f.readline().split()); f.readline()
d = f.read()
def px(x, y): o = (y * w + x) * 3; return '%02x%02x%02x' % tuple(d[o:o + 3])
xs = []; ys = []
for y in range(h):
    for x in range(w):
        if px(x, y) == '5577aa': xs.append(x); ys.append(y)
if not xs: print('none'); sys.exit()
x1, x2, y1, y2 = min(xs), max(xs), min(ys), max(ys)
print(x1, y1, x2, y2, px(x1 + 3, y1 - 6), px((x1 + x2) // 2, y1 - 6), px(x1 - 2, (y1 + y2) // 2), px((x1 + x2) // 2, y2 + 2))
PY
}

# 1. asks for server side: the manager answers server_side and a frame is drawn
./tests/shmclient -w 240 -h 140 -c 5577aa -K s -s 4 >"$dir/s.out" &
cl=$!
sleep 1.2; shot
grep -q "decoration server_side" "$dir/s.out" && ok "server-side asked: the compositor answers server_side" || bad "no server_side answer ($(cat "$dir/s.out"))"
set -- $(probe "$dir/shot.ppm")
if [ "$1" = none ]; then bad "ssd window not drawn"; else
	# bar colour (raised 343a43 when focused, panel 272c33 otherwise) above the client, and the border at its left
	case "$5" in 343a43|272c33) ok "title bar drawn above the client ($5)";; *) bad "no title bar above the client: $5";; esac
	case "$7" in 343a43|272c33) ok "frame drawn beside the client ($7)";; *) bad "no frame beside the client: $7";; esac
	case "$8" in 343a43|272c33) ok "frame drawn below the client ($8)";; *) bad "no frame below the client: $8";; esac
	sx=$((($1 + $3) / 2)); ty=$(($2 - 15))
fi
# right click on the title bar opens the window menu: the picture changes around the pointer
cp "$dir/shot.ppm" "$dir/before.ppm"
echo "move $sx $ty" >"$dir/in"; sleep 0.2
echo "button 273 1" >"$dir/in"; sleep 0.1; echo "button 273 0" >"$dir/in"; sleep 0.5; shot
diff=$(python3 -I - "$dir/before.ppm" "$dir/shot.ppm" <<'PY'
import sys
a = open(sys.argv[1], 'rb').read(); b = open(sys.argv[2], 'rb').read()
print(sum(1 for i in range(0, min(len(a), len(b)), 3) if a[i:i + 3] != b[i:i + 3]))
PY
)
[ "$diff" -gt 800 ] && ok "right click on the server-drawn title bar opens the menu ($diff pixels changed)" || bad "no menu after a right click on the title bar ($diff pixels changed)"
wait $cl
# 2. asks for client side: no frame
./tests/shmclient -w 240 -h 140 -c 5577aa -K c -s 2 >"$dir/c.out" &
cl=$!
sleep 1.2; shot
grep -q "decoration client_side" "$dir/c.out" && ok "client-side asked: the compositor answers client_side" || bad "no client_side answer ($(cat "$dir/c.out"))"
set -- $(probe "$dir/shot.ppm")
case "$5$7" in *343a43*|*272c33*) bad "client-side window got a frame ($5 $7)";; *) ok "client-side window has no frame";; esac
wait $cl
# 3. no protocol use at all (GTK-like): nothing is added
./tests/shmclient -w 240 -h 140 -c 5577aa -s 2 >"$dir/n.out" &
cl=$!
sleep 1.2; shot
set -- $(probe "$dir/shot.ppm")
case "$5$7" in *343a43*|*272c33*) bad "a client that did not use the protocol got a frame";; *) ok "no protocol: no frame";; esac
wait $cl
# 4. unset_mode: server side (the compositor's preference)
./tests/shmclient -w 240 -h 140 -c 5577aa -K u -s 2 >"$dir/u.out" &
cl=$!
sleep 1.2
grep -q "decoration server_side" "$dir/u.out" && ok "unset_mode: server_side" || bad "unset_mode: $(cat "$dir/u.out")"
wait $cl
kill $comp 2>/dev/null
[ $fails -eq 0 ] && echo "deco: all ok" || echo "deco: $fails FAILED"
exit $fails
