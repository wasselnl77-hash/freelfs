#!/bin/sh
# General Manager end to end, headless: a flcomp with no display, the real window, fake key presses
# through FLCOMP_TEST_FIFO. Checks that Ctrl+Shift+Esc starts it, that typing in the search field,
# Down and Delete end a victim process of our own, and keeps PPM screenshots of each tab in $OUT.
#
#   GM_SYSROOT=~/freelfs-work/rootfs-x86_64 FLCOMP=/path/to/flcomp OUT=/some/dir tests/e2e.sh
#
# GM_SYSROOT: when the binaries were built against a sysroot, they run through its ld.so.
# HOME and XDG_* point into a scratch directory; only processes this script started are signalled.
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
here=$(pwd)
[ -x "$FLCOMP" ] || { echo "set FLCOMP to a built flcomp"; exit 2; }
[ -x ./general-manager ] || { echo "build general-manager first"; exit 2; }
dir=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; kill $victim 2>/dev/null; rm -rf "$dir"' EXIT
export XDG_RUNTIME_DIR=$dir HOME=$dir XDG_CONFIG_HOME=$dir/cfg XDG_DATA_HOME=$dir/data
export FLCOMP_WALLPAPER= FLCOMP_HEADLESS=${SIZE:-1100x740} FLCOMP_PANEL= FLCOMP_SESSION= FLCOMP_SCREENSHOT=$dir/shot.ppm
export FLCOMP_TEST_FIFO=$dir/in FLCOMP_KEYBOARD_CONF=$dir/none FLUI_CONF=$dir/desktop.conf
mkdir -p "$dir/bin" "$dir/cfg/autostart" "$dir/data"
if [ -n "$GM_SYSROOT" ]; then
	run="$GM_SYSROOT/usr/lib/ld-linux-x86-64.so.2 --library-path $GM_SYSROOT/usr/lib:$GM_SYSROOT/lib"
	export FONTCONFIG_FILE=$dir/fonts.conf
	printf '<?xml version="1.0"?><!DOCTYPE fontconfig SYSTEM "fonts.dtd"><fontconfig><dir>%s/usr/share/fonts</dir><cachedir>%s/fc</cachedir></fontconfig>\n' \
		"$GM_SYSROOT" "$dir" >"$dir/fonts.conf"
fi
printf '#!/bin/sh\necho started >>%s/started\nexec %s %s/general-manager "$@"\n' "$dir" "$run" "$here" >"$dir/bin/general-manager"
chmod +x "$dir/bin/general-manager"
# a program with a name nothing else has, that does nothing
cp "$(command -v sleep)" "$dir/bin/gmvictim"
PATH=$dir/bin:$PATH
export PATH
mkfifo "$dir/in"
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

$run "$FLCOMP" 2>"$dir/log" &
comp=$!
i=0
while [ ! -S "$dir/wayland-0" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
[ -S "$dir/wayland-0" ] || { cat "$dir/log"; echo "FAIL flcomp did not start"; exit 1; }
shot() { kill -USR1 $comp; sleep 0.5; [ -n "$OUT" ] && cp "$dir/shot.ppm" "$OUT/$1.ppm"; return 0; }
send() { echo "$1" >"$dir/in"; sleep 0.08; }
tap() { send "key $1 1"; send "key $1 0"; }
chord() { for m in $1; do send "key $m 1"; done; tap "$2"; for m in $1; do send "key $m 0"; done; }
count_px() {	# file colour(hex) -> how many pixels have exactly that colour
	python3 -I - "$1" "$2" <<'PY'
import sys
f = open(sys.argv[1], 'rb'); assert f.readline() == b'P6\n'; w, h = map(int, f.readline().split()); f.readline()
d = f.read(); want = bytes.fromhex(sys.argv[2])
print(sum(1 for o in range(0, len(d), 3) if d[o:o + 3] == want))
PY
}
CTRL=29 SHIFT=42 ESC=1 DEL=111 DOWN=108 KF=33
sleep 0.5

chord "$CTRL $SHIFT" $ESC
i=0
while [ ! -f "$dir/started" ] && [ $i -lt 30 ]; do sleep 0.1; i=$((i + 1)); done
[ -f "$dir/started" ] && ok "Ctrl+Shift+Esc starts general-manager" || bad "Ctrl+Shift+Esc did not start it"
sleep 2
shot gm-e2e-processes
n=$(count_px "$dir/shot.ppm" 3f9b63)
[ "$n" -gt 200 ] && ok "window drawn ($n accent pixels)" || bad "window not drawn ($n accent pixels)"

# a victim to end: search for it, select its row, Delete
"$dir/bin/gmvictim" 600 &
victim=$!
sleep 1.5
chord "$CTRL" $KF
for k in 34 50 47 23 46 20 23 50; do tap $k; done	# g m v i c t i m
sleep 1.3
shot gm-e2e-search
tap $DOWN	# the section row, then the group; Down hands the focus from the search field to the table
tap $DOWN
sleep 0.3
shot gm-e2e-selected
kill -0 $victim 2>/dev/null && ok "victim alive before Delete" || bad "victim died early"
tap $DEL
sleep 1.5
shot gm-e2e-ended
if kill -0 $victim 2>/dev/null; then bad "Delete did not end the victim"; else ok "Delete ended the selected task"; fi

# the tabs (Ctrl+2 ... Ctrl+5)
for t in 3:performance 4:startup 5:users 6:details; do
	chord "$CTRL" "${t%%:*}"
	sleep 1.2
	shot "gm-e2e-${t##*:}"
done
echo "$fails failure(s)"
[ "$fails" -eq 0 ]
