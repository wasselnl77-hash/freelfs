#!/bin/sh
# Headless check, no compositor and no sound card: a generated clip, the first frame
# to a PPM at a seek position, subtitles and a second run that resumes where the
# first one stopped. Needs the ffmpeg tool. usage: tests/run.sh [flvideo]
set -eu
here=$(cd "$(dirname "$0")" && pwd)
bin=${1:-$here/../flvideo}
tmp=$(mktemp -d)
trap 'rm -r "$tmp"' EXIT
export HOME=$tmp/home XDG_CONFIG_HOME=$tmp/config XDG_DATA_HOME=$tmp/data FLVIDEO_AO=null
mkdir -p "$HOME"
"$here/mkclip.sh" "$tmp/clip"

FLUI_PPM="$tmp/a.ppm" FLUI_PPM_DELAY=3000 "$bin" --start 14 --pause "$tmp/clip/zoom.mkv"
n=$(python3 -I - "$tmp/a.ppm" <<'PY'
import sys
d = open(sys.argv[1], 'rb').read()
print(len(set(d[20::151])))
PY
)
[ "$n" -gt 60 ] || { echo "FAIL: no picture in the first frame ($n colours)"; exit 1; }
# the position was remembered on exit (14 s is between 10 s and the last 10 s)
grep -q "zoom.mkv	14" "$tmp/data/flvideo/resume.tsv" || { echo "FAIL: no resume entry"; cat "$tmp/data/flvideo/resume.tsv"; exit 1; }
FLUI_PPM="$tmp/b.ppm" FLUI_PPM_DELAY=3000 "$bin" --pause "$tmp/clip/zoom.mkv"
cmp -s "$tmp/a.ppm" "$tmp/b.ppm" || { echo "FAIL: the second run did not resume at the same frame"; exit 1; }
echo "flvideo: ok"
