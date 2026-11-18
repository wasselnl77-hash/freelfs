#!/bin/bash
# Round 23: the security tests, one table (PASS / FAIL / NOT RUN, never hidden), then the suites of older rounds that the
# round touches (installer, idle RAM, real-ISO shutdown). CASCADE=1 runs the whole build/m22-all.sh after them.
#
#   build/m23-all.sh REAL.iso TWIN.iso NOMODESET-TWIN.iso OLD-TWIN.iso
#
# REAL is the release ISO (root locked); TWIN is a dev twin (FLFS_DEV_ROOT=1 ./build/53-iso.sh --cmdline console=ttyS0,115200);
# OLD-TWIN is any older ISO (the negative control of m23-static must FAIL on it). ONLY="name ..." runs some.
# Logs: $OUT (default ~/freelfs-work/scratch/m23-final), table: OUT/table.txt.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
real=${1:?usage: m23-all.sh REAL.iso TWIN.iso NOMODESET-TWIN.iso OLD-TWIN.iso}
twin=${2:?}; nm=${3:?}; old=${4:?}
out=${OUT:-$HOME/freelfs-work/scratch/m23-final}
R=$HOME/freelfs-work/rootfs-x86_64
only=${ONLY:-}
mkdir -p "$out"
: > "$out/table.txt"
cd "$here/.." || exit 1
suite() { # suite NAME CMD...
	local n=$1; shift
	if [ -n "$only" ] && [[ " $only " != *" $n "* ]]; then return; fi
	echo "== $n" >&2
	local t0=$SECONDS
	if "$@" > "$out/$n.log" 2>&1; then r=PASS; else r="FAIL (exit $?)"; fi
	printf '%-28s %-14s %4d s   %s\n' "$n" "$r" $((SECONDS - t0)) "$out/$n.log" | tee -a "$out/table.txt"
}
want() { [ -z "$only" ] || [[ " $only " == *" $1 "* ]]; }
ld="-Wl,--dynamic-linker=$R/lib64/ld-linux-x86-64.so.2 -Wl,-rpath=$R/usr/lib"

# ---- host side (no VM)
suite host-flauth-policy sh src/flauth/tests/policy.sh
suite host-fllock-check bash -c "d=\$(mktemp -d) && gcc --sysroot=$R -std=c17 -D_GNU_SOURCE -O1 -o src/fllock/fllock-check src/fllock/fllock-check.c -lcrypt $ld && sh src/fllock/tests/check.sh; rc=\$?; rm -f src/fllock/fllock-check; exit \$rc"
suite host-flinstall-image bash -c "d=\$(mktemp -d $HOME/freelfs-work/scratch/m23-fi.XXXXXX) && gcc --sysroot=$R -std=c17 -D_GNU_SOURCE -O1 -Wno-format-truncation -o \$d/helper src/flinstall/helper.c -lcrypt $ld && nice -n 15 src/flinstall/tests/image-test.sh \$d/work \$d/helper; rc=\$?; rm -rf \$d; exit \$rc"
# ---- the ISOs' files
suite m23-static-real build/m23-static.sh "$real"
suite m23-static-twin build/m23-static.sh "$twin" dev
if want m23-static-control; then # the negative control: an older ISO must FAIL the release checks
	if build/m23-static.sh "$old" > "$out/m23-static-control.log" 2>&1; then r="CONTROL BROKEN (old ISO passed)"; else r="PASS (the old ISO fails, as it must)"; fi
	printf '%-28s %s   %s\n' m23-static-control "$r" "$out/m23-static-control.log" | tee -a "$out/table.txt"
fi
# ---- QEMU: a local HTTP server for the outbound test
port=$((20000 + RANDOM % 20000))
mkdir -p "$out/www"; echo ok > "$out/www/index.html"
(cd "$out/www" && exec python3 -m http.server "$port" --bind 0.0.0.0 > "$out/http.log" 2>&1) & http=$!
trap 'kill $http 2>/dev/null' EXIT
sleep 1
suite m23-real nice -n 10 python3 -u build/m23-real.py --iso "$real" --mem 1G --port "$port"
if want m23-sys; then
	sed "s|@PORT@|$port|g;s|@OUT@|$out|g" build/m23/sys.cmds > "$out/sys.run"
	suite m23-sys nice -n 10 python3 -u build/live-run.py --iso "$twin" --mem 1G "$out/sys.run"
fi
suite idle-ram-1G bash -c "python3 build/iso-test.py --iso '$twin' --mem 1G --profile '$out/idle-profile.txt' && used=\$(awk '/^== used = MemTotal - MemAvailable/ {getline; print; exit}' '$out/idle-profile.txt') && echo \"used: \$used kB\" && [ \"\$used\" -lt 92160 ]"
suite m10-install python3 build/m10-install.py --iso "$twin"
suite m18-realiso python3 build/m18-realiso.py --iso "$real"
if [ -z "$only" ] && [ "${CASCADE:-0}" = 1 ]; then
	OUT=$out/m22 build/m22-all.sh "$real" "$twin" "$nm" "$old" > "$out/m22-all.log" 2>&1
	cat "$out/m22/table.txt" >> "$out/table.txt"
fi
echo "table in $out/table.txt"
