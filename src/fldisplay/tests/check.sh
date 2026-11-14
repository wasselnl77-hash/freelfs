#!/bin/sh
# fldisplay against a headless flcomp with two outputs: list, set (mode, scale, position,
# enabled, primary), saved output.NAME= lines, revert, refusals, nightlight. `make check`.
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
comp=../flcomp/flcomp
[ -x $comp ] || { echo "SKIP: build flcomp first"; exit 0; }
fld=$(pwd)/fldisplay
dir=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$dir"' EXIT
export XDG_RUNTIME_DIR=$dir HOME=$dir FLUI_CONF=$dir/desktop.conf
export FLCOMP_WALLPAPER= FLCOMP_HEADLESS=1280x800 FLCOMP_HEADLESS_OUTPUTS=2 FLCOMP_PANEL= FLCOMP_SESSION=
export FLCOMP_SCREENSHOT=$dir/shot.ppm FLCOMP_TRUST_ANY=1 FLCOMP_KEYBOARD_CONF=$dir/none
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
$comp 2>"$dir/log" &
cp=$!
i=0
while [ ! -S "$dir/wayland-0" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
[ -S "$dir/wayland-0" ] || { cat "$dir/log"; echo "FAIL flcomp did not start"; exit 1; }
width() { kill -USR1 $cp; sleep 0.4; head -c 20 "$dir/shot.ppm" | sed -n 2p; }
list() { $fld list; }

l=$(list)
[ "$(echo "$l" | wc -l)" = 2 ] && ok "two outputs listed" || bad "list: $l"
echo "$l" | head -1 | grep -qP '^HEADLESS-1\tconnected\t1280x800@60\t1\t0,0\tenabled\tprimary\t1280x800@60,' && ok "list line format" || bad "format: $l"
echo "$l" | tail -1 | grep -qP '^HEADLESS-2\tconnected\t1280x800@60\t1\t1280,0\tenabled\t-\t' && ok "second output to the right" || bad "second: $l"
[ "$(width)" = "2560 800" ] && ok "desktop is both outputs (2560x800)" || bad "width $(width)"

$fld set HEADLESS-2 mode=1024x768@60 && list | grep -qP '^HEADLESS-2\tconnected\t1024x768@60' && ok "mode switch" || bad "mode switch"
$fld set HEADLESS-2 scale=1.25 && list | grep -qP '^HEADLESS-2\t.*\t1.25\t' && ok "scale 1.25" || bad "scale"
# 1024x768 at 125 % is 819x614 logical: the desktop is 1280 + 819
[ "$(width)" = "2099 800" ] && ok "desktop width follows the scale (2099)" || bad "scaled width $(width)"
grep -q '^output.HEADLESS-2=mode=1024x768@60 scale=1.25 pos=1280,0 enabled=1 primary=0$' $dir/desktop.conf && ok "saved in desktop.conf" || { bad "conf"; cat $dir/desktop.conf; }
$fld set HEADLESS-2 pos=0,800 && list | grep -qP '^HEADLESS-2\t.*\t0,800\t' && ok "position" || bad "position"
[ "$(width)" = "1280 1414" ] && ok "stacked below (1280x1414)" || bad "stacked $(width)"
$fld revert && list | grep -qP '^HEADLESS-2\t.*\t1.25\t1280,0\t' && ok "revert restores position" || bad "revert"
$fld set HEADLESS-2 primary=1 && list | grep -qP '^HEADLESS-2\t.*\tprimary\t' && list | grep -qP '^HEADLESS-1\t.*\t-\t' && ok "primary moves" || bad "primary"
$fld set HEADLESS-1 enabled=0 && list | grep -qP '^HEADLESS-1\t.*\t-\t' && list | grep '^HEADLESS-1' | grep -q disabled && ok "disable" || bad "disable"
$fld set HEADLESS-2 enabled=0 2>"$dir/err" && bad "last output switched off" || { grep -q refused "$dir/err" && ok "last output cannot be disabled"; }
$fld set HEADLESS-1 enabled=1 && ok "enable again" || bad "enable"
$fld set HEADLESS-2 mode=999x1@60 2>/dev/null && bad "bad mode accepted" || ok "bad mode refused"
$fld set HEADLESS-2 scale=2 mode=1024x768@60 2>/dev/null && bad "scale 2 on 1024x768 accepted (desktop 512 wide)" || ok "too large a scale refused"
$fld set NOPE enabled=1 2>/dev/null && bad "unknown output accepted" || ok "unknown output refused"

$fld nightlight on && sleep 3 && grep -q 'night light on (4000K' "$dir/log" && ok "nightlight on fades (4000K)" || { bad "nightlight"; tail -3 "$dir/log"; }
grep -q '^nightlight=on' $dir/desktop.conf && grep -q '^night_light=1' $dir/desktop.conf && ok "nightlight keys" || bad "keys"
$fld nightlight off && sleep 3 && grep -q 'night light off' "$dir/log" && ok "nightlight off" || bad "nightlight off"
now=$(date +%H:%M)
$fld nightlight auto temp=3000 from=00:00 to=23:59 && sleep 3 && grep -q 'night light on (3000K' "$dir/log" && ok "auto inside the span is on" || bad "auto on"
$fld nightlight auto from=00:00 to=00:01 && sleep 3 && [ "$(date +%H%M)" = 0000 ] || { grep -c 'night light off' "$dir/log" | grep -q '^[2-9]' && ok "auto outside the span is off"; }

kill $cp 2>/dev/null
[ $fails = 0 ] && echo "fldisplay: all passed" || echo "fldisplay: $fails FAILED"
exit $fails
