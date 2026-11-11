#!/bin/sh
# mkclip.sh DIR -- a 30 s test clip made with the ffmpeg tool: a Mandelbrot zoom, two
# audio tracks (a tone each, languages eng and deu), written as DIR/zoom.mkv, plus
# DIR/zoom.srt with a few subtitles that mpv picks up on its own. Everything is
# generated; nothing is copied from anywhere.
set -eu
d=$1
mkdir -p "$d"
ffmpeg -v error -y -filter_complex \
	"mandelbrot=size=640x360:rate=24[v];sine=frequency=440:sample_rate=44100[a1];sine=frequency=330:sample_rate=44100[a2]" \
	-map "[v]" -map "[a1]" -map "[a2]" -t 30 -c:v mpeg4 -q:v 5 -c:a aac -b:a 64k \
	-metadata:s:a:0 language=eng -metadata:s:a:0 title=Original \
	-metadata:s:a:1 language=deu -metadata:s:a:1 title=Kommentar "$d/zoom.mkv"
cat > "$d/zoom.srt" <<'EOF'
1
00:00:01,000 --> 00:00:05,000
A slow zoom into the edge of the set.

2
00:00:06,000 --> 00:00:11,000
Each frame is generated, not recorded.

3
00:00:12,000 --> 00:00:17,500
Two audio tracks: pick one under Audio.

4
00:00:18,000 --> 00:00:25,000
Subtitles come from the .srt file next to the video.
EOF
