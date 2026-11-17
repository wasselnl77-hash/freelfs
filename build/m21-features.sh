#!/bin/bash
# Round 21 tests (QEMU, KVM), through the real UI as the live user `tester`, booted FROM a virtual Ventoy-style USB stick
# (an exFAT partition holding the ISO + a FAT boot partition, build/m21/mkstick.py), as the user's laptop boots.
# Needs the SERIAL TWIN of the live ISO (./build/53-iso.sh --cmdline console=ttyS0,115200, copied away).
#
#   build/m21-features.sh TWIN.iso [OLD-TWIN.iso]      STEPS="stick-boot ..." to run some
#
# stick-boot   1 GB, no RAM copy: the stick belongs to the live user (uid 1000), a file is written from the shell and through
#              Files, eject in Files and flmount eject/unmount are REFUSED with "FreeLFS is running from this stick. Shut down
#              first, or boot with 'Load into RAM'", apps still open
# toram        6 GB: loaded into RAM automatically (image + 4 GB); the stick is released and an ordinary drive: eject works, apps
#              still open, plugged in again it is mounted writable
# toram-off    6 GB with flive.toram=0 on the boot entries: stays on the stick
# toram-menu   3 GB, the boot menu entry "Load into RAM" (keys sent to flboot): forces the copy
# portal-folder the file chooser (what Firefox's upload opens): folders open at once by double click, Backspace, sidebar and keys
#              (it did not redraw after a click: "I cannot open any folder"), and the chosen file's URI is answered
# taskbar-tip  hovering a taskbar entry shows the window's title (pixels in the panel)
# control      (needs OLD-TWIN.iso, the r20 ISO's twin) the same stick-boot checks on the old ISO: MUST FAIL (negative control)
# flboot-fw    flboot's "Firmware setup (BIOS)" entry in OVMF: lands in the firmware's setup screen; "Other drives and systems"
#              lists the firmware's boot options and chain-loads another disk (see build/m21/flboot-fw.sh)
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
twin=${1:?usage: m21-features.sh TWIN.iso [OLD-TWIN.iso]}
old=${2:-}
out=${OUT:-$HOME/freelfs-work/scratch/m21-tests}
only=${STEPS:-}
want() { [ -z "$only" ] || [[ " $only " == *" $1 "* ]]; }
mkdir -p "$out"
rc=0
stick() { # stick ISO NAME [EXTRA...] -> $out/NAME.img (cached by the ISO's size and time)
	local iso=$1 name=$2; shift 2
	local img=$out/$name.img stamp=$out/$name.stamp want
	want="$(stat -c '%s %Y' "$iso") $*"
	if [ ! -s "$img" ] || [ "$(cat "$stamp" 2> /dev/null)" != "$want" ]; then
		rm -f "$img"
		local args=()
		[ $# -eq 0 ] || args=(--extra "$*")
		python3 "$here/m21/mkstick.py" "$iso" "$img" --name freelfs-live-r21.iso "${args[@]}" > /dev/null || return 1
		echo "$want" > "$stamp"
	fi
}
run() { # run NAME STICK MEM [LIVE-RUN ARGS]
	local n=$1 img=$2 mem=$3; shift 3
	sed "s|@OUT@|$out|g" "$here/m21/$n.cmds" > "$out/$n.run"
	echo "== $n ($mem)"
	nice -n 10 python3 -u "$here/live-run.py" --stick "$img" --mem "$mem" "$@" "$out/$n.run" -- -device usb-tablet > "$out/$n.log" 2>&1
	local r=$?
	grep -E "^(FAIL|NOT ok|RESULT|pixels|colours)" "$out/$n.log" | tail -n 30
	[ $r -eq 0 ] || { echo "-> $n FAILED (exit $r)"; rc=1; }
	return $r
}
iso_step() { # iso_step NAME MEM: a cmds file on the ISO booted as a CD (no stick)
	local n=$1 mem=$2
	sed "s|@OUT@|$out|g" "$here/m21/$n.cmds" > "$out/$n.run"
	echo "== $n ($mem)"
	nice -n 10 python3 -u "$here/live-run.py" --iso "$twin" --mem "$mem" "$out/$n.run" -- -device usb-tablet > "$out/$n.log" 2>&1
	local r=$?
	grep -E "^(FAIL|NOT ok|RESULT|pixels|colours)" "$out/$n.log" | tail -n 20
	[ $r -eq 0 ] || { echo "-> $n FAILED (exit $r)"; rc=1; }
}
want taskbar-tip && iso_step taskbar-tip 1G
if want portal-folder; then
	R=$HOME/freelfs-work/rootfs-x86_64
	PKG_CONFIG_SYSROOT_DIR=$R PKG_CONFIG_LIBDIR=$R/usr/lib/pkgconfig:$R/usr/share/pkgconfig \
		gcc --sysroot=$R -O2 -o "$out/portal-client" "$here/m21/portal-client.c" $(PKG_CONFIG_SYSROOT_DIR=$R PKG_CONFIG_LIBDIR=$R/usr/lib/pkgconfig:$R/usr/share/pkgconfig pkg-config --cflags --libs gio-2.0) \
		-Wl,--allow-shlib-undefined || rc=1
	iso_step portal-folder 1G
fi
if want stick-boot; then stick "$twin" new-stick && run stick-boot "$out/new-stick.img" 1G; fi
if want toram; then stick "$twin" new-stick && run toram "$out/new-stick.img" 6G; fi
if want toram-off; then stick "$twin" new-stick-off flive.toram=0 && run toram-off "$out/new-stick-off.img" 6G; fi
if want toram-menu; then
	stick "$twin" new-stick && run toram-menu "$out/new-stick.img" 3G --bootkeys "1:spc,1.5:spc,2:spc,2.5:spc,3:spc,3.5:spc,4:spc,4.5:spc,5:spc,6:spc,6.5:down,7:ret"
fi
if want control; then
	if [ -n "$old" ]; then
		stick "$old" old-stick && {
			echo "(negative control: the old ISO must FAIL the stick-boot checks)"
			rc0=$rc
			if run stick-boot "$out/old-stick.img" 1G; then echo "CONTROL BROKEN: the old ISO passed"; rc=1; else echo "control: failed as it must"; rc=$rc0; fi
		}
	else
		echo "control: NOT RUN (no old twin given)"; rc=1
	fi
fi
echo "pictures and logs in $out"
exit $rc
