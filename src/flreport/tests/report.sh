#!/bin/sh
# flreport on a scratch home: the archive exists, holds the sections, and a planted log is in it.
# SPDX-License-Identifier: MIT
cd "$(dirname "$0")/.." || exit 1
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
mkdir -p "$d/home" "$d/rt" "$d/home/.local/state/freelfs/log"
echo "marker-line-from-an-app" >"$d/home/.local/state/freelfs/log/someapp.log"
echo "marker-line-from-flcomp" >"$d/rt/flcomp.log"
out=$(env -u XDG_STATE_HOME -u XDG_CONFIG_HOME -u XDG_DATA_HOME HOME=$d/home XDG_RUNTIME_DIR=$d/rt ./flreport -q -o "$d" | tail -n 1)
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
[ -f "$out" ] && ok "archive $out" || { bad "no archive"; exit 1; }
case $out in "$d"/freelfs-report-*.tar.gz) ok "name" ;; *) bad "name $out" ;; esac
list=$(tar tzf "$out")
for want in summary.txt net.txt mounts.txt hw/pci.txt hw/usb.txt hw/graphics.txt logs/dmesg.txt logs/flcomp.log; do
	echo "$list" | grep -q "/$want\$" && ok "has $want" || bad "missing $want"
done
tar xzOf "$out" --wildcards '*/logs/flcomp.log' | grep -q marker-line-from-flcomp && ok "flcomp.log content" || bad "flcomp.log content"
tar xzOf "$out" --wildcards "*/logs/state-someapp.log" | grep -q marker-line-from-an-app && ok "app log content" || bad "app log content"
tar xzOf "$out" --wildcards '*/summary.txt' | grep -q "FreeLFS report" && ok "summary" || bad "summary"
# an unwritable output directory falls back to /tmp
mkdir "$d/ro" && chmod 500 "$d/ro"
out2=$(env -u XDG_STATE_HOME -u XDG_CONFIG_HOME -u XDG_DATA_HOME HOME=$d/home XDG_RUNTIME_DIR=$d/rt ./flreport -q -o "$d/ro" | tail -n 1)
case $out2 in /tmp/freelfs-report-*) ok "fallback to /tmp"; rm -f "$out2" ;; *) bad "fallback: $out2" ;; esac
[ "$fails" = 0 ]
