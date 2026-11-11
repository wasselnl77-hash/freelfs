#!/bin/sh
# Render one page of Themes to a PNG without a compositor (FLUI_PPM), with a scratch HOME and the
# given desktop.conf lines. usage: tests/shot.sh PAGE OUT.png [KEY=VALUE]...
#   FLTHEME_SIZE=WxH, FLTHEME_TEST_SAVE=text (typed in the name field), FLTHEME=path to the binary
# SPDX-License-Identifier: MIT
page=$1 out=$2
shift 2
here=$(cd "$(dirname "$0")/.." && pwd)
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
mkdir -p "$d/cfg/freelfs"
for l in "$@"; do echo "$l" >>"$d/cfg/freelfs/desktop.conf"; done
HOME=$d XDG_CONFIG_HOME=$d/cfg XDG_DATA_HOME=$d/data FLUI_PPM=$d/o.ppm "${FLTHEME:-$here/fltheme}" --page "$page" || exit 1
python3 -I "$here/../flcomp/tests/ppm2png.py" "$d/o.ppm" "$out"
