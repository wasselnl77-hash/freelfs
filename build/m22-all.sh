#!/bin/bash
# Round 22: the m22 tests, then every older suite (build/m21-all.sh), one table, PASS / FAIL / NOT RUN.
#
#   build/m22-all.sh REAL.iso TWIN.iso NOMODESET-TWIN.iso OLD-TWIN.iso
#
# TWIN is the serial twin of the new ISO (53-iso.sh --cmdline console=ttyS0,115200); OLD-TWIN is only passed on to m21-all.sh
# (the exFAT checks have their own in-guest negative control: a plain write without fsync must show as dirty). Needs ~/freelfs-work/publish/repo-x86_64 (build/70-repo.sh and
# build/70-publish-repo.sh, with the bumped flreport in it): a local HTTP server stands in for GitHub Releases.
# ONLY="name ..." runs some. Logs: $OUT (default ~/freelfs-work/scratch/m22-final), table: OUT/table.txt.
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
real=${1:?usage: m22-all.sh REAL.iso TWIN.iso NOMODESET-TWIN.iso OLD-TWIN.iso}
twin=${2:?}; nm=${3:?}; old=${4:?}
out=${OUT:-$HOME/freelfs-work/scratch/m22-final}
pub=${FLFS_PUBLISH_DIR:-$HOME/freelfs-work/publish}
R=$HOME/freelfs-work/rootfs-x86_64
only=${ONLY:-}
# the update the ISO is offered over HTTP: flreport at the release in the ISO (OLDV) and the one bumped after it (NEWV)
OLDV=${FLREPORT_OLD:-0.1-8}
NEWV=${FLREPORT_NEW:-0.1-9}
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

# host-side: a flpkg built against the target's libraries (run through its loader), the front end, the repo flow
if [ ! -x src/flpkg/flpkg ]; then
	(cd src/flpkg && make -j4 flpkg CC="gcc --sysroot=$R" LDFLAGS="-Wl,--dynamic-linker=$R/lib64/ld-linux-x86-64.so.2 -Wl,-rpath=$R/usr/lib") > "$out/host-flpkg-build.log" 2>&1
fi
suite host-flpkg-sources env FLPKG_HOST_BIN=$PWD/src/flpkg/flpkg build/m22/flpkg-sources.sh
suite host-repo-update build/m22/repo-update.sh
suite host-flpkg-native make -C src/flpkg check CC="gcc --sysroot=$R" LDFLAGS="-Wl,--dynamic-linker=$R/lib64/ld-linux-x86-64.so.2 -Wl,-rpath=$R/usr/lib"
suite host-flpkg-seat sh src/flpackages/tests/seat.sh
suite host-serial-getty bash -c 'd=$(mktemp -d); mkdir -p $d/a $d/b; echo 4 > $d/a/type; echo 0 > $d/b/type;
	[ "$(FLSERIAL_TEST=1 FLSERIAL_SYS=$d/a FLSERIAL_DEV=/dev/null sh config/etc/rc.d/serial-getty)" = getty ] &&
	[ "$(FLSERIAL_TEST=1 FLSERIAL_SYS=$d/b FLSERIAL_DEV=/dev/null sh config/etc/rc.d/serial-getty)" = none ] &&
	[ "$(FLSERIAL_TEST=1 FLSERIAL_SYS=$d/none FLSERIAL_DEV=/dev/null sh config/etc/rc.d/serial-getty)" = none ] &&
	[ "$(FLSERIAL_TEST=1 FLSERIAL_SYS=$d/a FLSERIAL_DEV=/nonexistent sh config/etc/rc.d/serial-getty)" = none ]'
suite host-flgm env CC="gcc --sysroot=$R" sh src/flgm/tests/check.sh
suite host-flportal sh src/flportal/tests/portal.sh

# QEMU: HTTP server for the repo (good, and a copy whose index has a flipped byte)
if want live || want m22-ui || want m22-portal; then
	if [ -s "$pub/repo-x86_64/index.zst" ]; then
		rm -rf "$pub/tampered"; mkdir -p "$pub/tampered"
		cp "$pub/repo-x86_64/index.zst" "$pub/repo-x86_64/index.zst.sig" "$pub/tampered/"
		printf x >> "$pub/tampered/index.zst"
		port=$((20000 + RANDOM % 20000))
		(cd "$pub" && exec python3 -m http.server "$port" --bind 0.0.0.0 > "$out/http.log" 2>&1) & http=$!
		trap 'kill $http 2>/dev/null' EXIT
		sleep 1
	else
		port=0
	fi
	stickrun() { # stickrun NAME ISO
		local n=$1 iso=$2
		python3 build/m21/mkstick.py "$iso" "$out/$n.img" --name freelfs-live-r22.iso > /dev/null 2>&1 || return 1
		sed "s|@OUT@|$out|g;s|@PORT@|$port|g;s|@OLDV@|$OLDV|g;s|@NEWV@|$NEWV|g" build/m22/live.cmds > "$out/$n.run"
		nice -n 10 python3 -u build/live-run.py --stick "$out/$n.img" --mem 1G "$out/$n.run" -- -device usb-tablet
	}
	if want live; then
		if [ "$port" = 0 ]; then printf '%-28s NOT RUN (no staged repo)\n' live | tee -a "$out/table.txt"; else suite live stickrun live "$twin"; fi
	fi
	if want m22-ui; then
		if [ "$port" = 0 ]; then printf '%-28s NOT RUN (no staged repo)\n' m22-ui | tee -a "$out/table.txt"; else
		sed "s|@OUT@|$out|g;s|@PORT@|$port|g;s|@OLDV@|$OLDV|g;s|@NEWV@|$NEWV|g" build/m22/ui.cmds > "$out/ui.run"
		suite m22-ui nice -n 10 python3 -u build/live-run.py --iso "$twin" --mem 1G "$out/ui.run" -- -device usb-tablet; fi
	fi
	if want m22-portal; then
		gcc --sysroot=$R -O2 -o "$out/portal-client" build/m21/portal-client.c $(PKG_CONFIG_SYSROOT_DIR=$R PKG_CONFIG_LIBDIR=$R/usr/lib/pkgconfig:$R/usr/share/pkgconfig pkg-config --cflags --libs gio-2.0) -Wl,--allow-shlib-undefined
		sed "s|@OUT@|$out|g" build/m22/portal-log.cmds > "$out/portal-log.run"
		suite m22-portal nice -n 10 python3 -u build/live-run.py --iso "$twin" --mem 1G "$out/portal-log.run" -- -device usb-tablet
	fi
fi
# idle RAM at 1 GB: used (MemTotal - MemAvailable) under 90 MB
if want idle-ram-1G; then
	suite idle-ram-1G bash -c "python3 build/iso-test.py --iso '$twin' --mem 1G --profile '$out/idle-profile.txt' && used=\$(awk '/^== used = MemTotal - MemAvailable/ {getline; print; exit}' '$out/idle-profile.txt') && echo \"used: \$used kB\" && [ \"\$used\" -lt 92160 ]"
fi
if [ -z "$only" ]; then
	OUT=$out/m21 build/m21-all.sh "$real" "$twin" "$nm" "$old" > "$out/m21-all.log" 2>&1
	cat "$out/m21/table.txt" >> "$out/table.txt"
fi
echo "table in $out/table.txt"
