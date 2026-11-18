#!/bin/bash
# Round 21: every test suite in a row, one line each in a table (PASS, FAIL or NOT RUN, never hidden).
#
#   build/m21-all.sh REAL.iso TWIN.iso NOMODESET-TWIN.iso OLD-TWIN.iso
#
# Needs the disk image (./build/50-image.sh) for boot-test, m10-features, m10-snapshots, m11, m12 and m16.
# Logs: $OUT (default ~/freelfs-work/scratch/m21-final), one file per suite; the table is OUT/table.txt.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
real=${1:?usage: m21-all.sh REAL.iso TWIN.iso NOMODESET-TWIN.iso OLD-TWIN.iso}
twin=${2:?}; nm=${3:?}; old=${4:?}
out=${OUT:-$HOME/freelfs-work/scratch/m21-final}
img=${FLFS_IMG:-$HOME/freelfs-work/freelfs-x86_64.img}
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
need_img() { [ -s "$img" ]; }
if need_img; then
	suite boot-2G env FLFS_VM_MEM=2G python3 build/boot-test.py
	suite boot-1G env FLFS_VM_MEM=1G python3 build/boot-test.py
	suite m10-features-1G build/m10-features.sh 1G
	suite m10-snapshots-1G build/m10-snapshots.sh 1G
	suite m11-x11-1G build/m11-x11-gpu.sh x11 1G
	suite m12-sandbox-1G build/m12-sandbox.sh 1G
	suite m16 build/m16-features.sh "$twin" "$img"
else
	for n in boot-2G boot-1G m10-features-1G m10-snapshots-1G m11-x11-1G m12-sandbox-1G m16; do printf '%-28s NOT RUN (no disk image)\n' $n | tee -a "$out/table.txt"; done
fi
suite iso-test-1G python3 build/iso-test.py --iso "$twin" --mem 1G --profile "$out/idle-profile.txt"
# round 23: the real ISO has root locked, and m10-install logs in as root on the serial console (the live ISO, then the installed disk): it runs on the dev twin
suite m10-install python3 build/m10-install.py --iso "$twin"
suite m17 build/m17-features.sh "$twin"
suite m18-honest build/m18-honest.sh "$twin"
suite m18-realiso python3 build/m18-realiso.py --iso "$real"
suite m19 build/m19-features.sh "$twin"
suite m20 build/m20-features.sh "$twin" "$nm"
suite m21-features build/m21-features.sh "$twin" "$old"
suite m21-taskbar env STEPS=taskbar-tip build/m21-features.sh "$twin"
suite m21-portal-folder env STEPS=portal-folder build/m21-features.sh "$twin"
python3 build/m21/mkstick.py "$twin" "$out/flboot.stick" --name freelfs-live-r21.iso > /dev/null 2>&1
suite m21-flboot build/m21-flboot.sh "$out/flboot.stick"
suite m21-freebws build/m21-freebws.sh "$twin" 3
# the negative control: the same flows on the old ISO must FAIL (a pass here would mean the test cannot see the bug)
if [ -z "$only" ] || [[ " $only " == *" m21-freebws-control "* ]]; then
	echo "== m21-freebws-control" >&2
	if build/m21-freebws.sh "$old" 3 > "$out/m21-freebws-control.log" 2>&1; then r="CONTROL BROKEN (old ISO passed)"; else r="PASS (the old ISO fails, as it must)"; fi
	printf '%-28s %s   %s\n' m21-freebws-control "$r" "$out/m21-freebws-control.log" | tee -a "$out/table.txt"
fi
suite host-flgm make -C src/flgm check CC="gcc --sysroot=$HOME/freelfs-work/rootfs-x86_64"
suite host-flboot-menu env FONT=$HOME/freelfs-work/rootfs-x86_64/usr/share/consolefonts/Lat2-Terminus16.psfu.gz src/flboot/tests/menu-test.sh
echo "table in $out/table.txt"
