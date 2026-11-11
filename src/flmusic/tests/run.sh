#!/bin/sh
# Headless check, no compositor and no sound card: a private session bus, flmusic on
# a generated library with a fakesink, driven through MPRIS with flmusic --ctl and
# dbus-send. Needs dbus-daemon, dbus-send, python3 and gst-launch-1.0's plugins.
# usage: tests/run.sh [flmusic]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
bin=${1:-$here/../flmusic}
tmp=$(mktemp -d)
bus=
cleanup() {
	[ -z "$bus" ] || kill "$bus" 2> /dev/null || true
	rm -r "$tmp"
}
trap cleanup EXIT
export HOME=$tmp/home XDG_CACHE_HOME=$tmp/cache XDG_CONFIG_HOME=$tmp/config FLMUSIC_SINK=fakesink
mkdir -p "$HOME"

cc -o "$tmp/tagenc" "$here/tagenc.c" $(pkg-config --cflags --libs gstreamer-1.0)
TAGENC=$tmp/tagenc python3 -I "$here/mkaudio.py" "$tmp/music" > /dev/null

eval "$(dbus-launch --sh-syntax)"
bus=$DBUS_SESSION_BUS_PID
export FLMUSIC_DIR=$tmp/music
a="$tmp/music/Marta Veil/Glass Meadow/01 Clear Morning.ogg"
b="$tmp/music/Marta Veil/Glass Meadow/02 Dew.ogg"
c="$tmp/music/Marta Veil/Glass Meadow/03 Long Grass.ogg"
FLUI_PPM="$tmp/a.ppm" FLUI_PPM_DELAY=9000 "$bin" "$a" "$b" "$c" &
app=$!
for i in 1 2 3 4 5 6 7 8 9 10; do
	"$bin" --ctl status 2> /dev/null && break
	sleep 1
done
get() {
	dbus-send --session --print-reply --dest=org.mpris.MediaPlayer2.flmusic /org/mpris/MediaPlayer2 \
		org.freedesktop.DBus.Properties.Get string:org.mpris.MediaPlayer2.Player string:"$1"
}
title() { get Metadata | grep -A1 xesam:title | tail -1 | sed 's/.*string "\(.*\)"/\1/'; }

"$bin" --ctl status | grep -q Playing || { echo "FAIL: not playing after start"; exit 1; }
[ "$(title)" = "Clear Morning" ] || { echo "FAIL: first title is '$(title)'"; exit 1; }
"$bin" --ctl pause
sleep 1
"$bin" --ctl status | grep -q Paused || { echo "FAIL: pause"; exit 1; }
"$bin" --ctl play-pause
sleep 1
"$bin" --ctl status | grep -q Playing || { echo "FAIL: play-pause"; exit 1; }
"$bin" --ctl next
sleep 1
[ "$(title)" = "Dew" ] || { echo "FAIL: next gave '$(title)'"; exit 1; }
"$bin" --ctl previous
sleep 1
# less than 3 s in: previous goes to the track before, i.e. the first one
[ "$(title)" = "Clear Morning" ] || { echo "FAIL: previous gave '$(title)'"; exit 1; }
p1=$(get Position | tail -1 | awk '{print $3}')
sleep 2
p2=$(get Position | tail -1 | awk '{print $3}')
[ "$p2" -gt "$p1" ] || { echo "FAIL: position does not advance ($p1 -> $p2)"; exit 1; }
"$bin" --ctl stop
"$bin" --ctl status | grep -q Stopped || { echo "FAIL: stop"; exit 1; }
wait "$app" || true
head -c 2 "$tmp/a.ppm" | grep -q P6
[ "$(wc -l < "$tmp/cache/flmusic/library.tsv")" -ge 3 ] || { echo "FAIL: no library cache"; exit 1; }
echo "flmusic: ok"
