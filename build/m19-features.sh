#!/bin/bash
# Round 19 honest tests (QEMU, KVM). Needs the SERIAL TWIN of the live ISO (./build/53-iso.sh --cmdline
# console=ttyS0,115200, copied away). Everything goes through the real UI as the live user `tester`; a step that does
# not do what it should makes live-run.py exit 1.
#
#   build/m19-features.sh TWIN.iso      STEPS="eject-launch ..." to run some
#
# eject-launch  a partitioned stick (sda1), Files opened on it, EJECT in Files; then General Manager, Settings and
#               Terminal from the start menu must each open a window (pixels), and a text file saved in fledit lands
#               in Home (also when the editor was started from "/"). The r18 laptop bug.
# icons         the Papirus theme is in the image and the panel's start menu shows icons (pictures, colours)
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
twin=${1:?usage: m19-features.sh TWIN.iso}
out=${OUT:-$HOME/freelfs-work/scratch/m19}
only=${STEPS:-}
want() { [ -z "$only" ] || [[ " $only " == *" $1 "* ]]; }
mkdir -p "$out"
rc=0
usb=$out/usb.img
# a PARTITIONED stick, as sold: an MBR with one exFAT partition (sda1)
if ! /usr/sbin/sfdisk -d "$usb" 2> /dev/null | grep -q "^[^ ]* *: start="; then
	rm -f "$usb" "$usb.part"
	truncate -s 256M "$usb"
	printf 'label: dos\nstart=2048, type=7\n' | /usr/sbin/sfdisk --quiet "$usb"
	truncate -s $((256*1024*1024 - 2048*512)) "$usb.part"
	/usr/sbin/mkfs.exfat -L TESTUSB "$usb.part" > /dev/null
	dd if="$usb.part" of="$usb" bs=512 seek=2048 conv=notrunc status=none
	rm -f "$usb.part"
fi
run() { # run NAME [QEMU args]
	local n=$1; shift
	sed "s|@OUT@|$out|g; s|@HERE@|$here/m19|g; s|@USB@|$usb|g" "$here/m19/$n.cmds" > "$out/$n.run"
	echo "== $n"
	nice -n 10 python3 -u "$here/live-run.py" --iso "$twin" --mem 1G "$out/$n.run" -- -device usb-tablet "$@" > "$out/$n.log" 2>&1
	local r=$?
	grep -E "^(FAIL|NOT ok|RESULT|pixels|colours)" "$out/$n.log" | tail -n 30
	[ $r -eq 0 ] || { echo "-> $n FAILED (exit $r)"; rc=1; }
}
want eject-launch && run eject-launch -device qemu-xhci,id=xhci
want icons && run icons
echo "pictures and logs in $out"
exit $rc
