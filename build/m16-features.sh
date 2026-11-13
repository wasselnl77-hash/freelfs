#!/bin/bash
# Round 16 tests (QEMU, KVM): the desktop and the live session. Needs the SERIAL TWIN of the live ISO (the same root
# with console=ttyS0,115200 last: ./build/53-iso.sh --cmdline console=ttyS0,115200, copy it away) and, for flauth,
# the disk image. Pictures and logs go to OUT (default ~/freelfs-work/scratch/m16).
#
#   build/m16-features.sh TWIN.iso [IMAGE.img]
#
# usb-plug     a USB disk (exFAT) hot-plugged: flmount list / mount / eject, a file written to it
# usb-notify   the "USB stick connected" card (picture)
# poweroff-menu  the panel's Shut down and its confirm window: the VM really powers off
# clipboard    clipboard history: hold Ctrl+V opens the list, a pick pastes (pictures)
# window-qol   right click on a title bar (menu), drag to an edge (snap), middle click closes (pictures)
# files-dclick Files: double click opens a file in the editor
# freebws-popup  window.open from a local page: the popup posts back to its opener and closes
# persist-1..3 a stick with freelfs-persist.img keeps /home over a reboot
# flauth       (image) the password window for removing a package: wrong password, then the right one
# Not here: Ctrl+Alt+T on the login screen opens nothing, and `flpower off` as tester -- both are the first lines of usb-plug.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
twin=${1:?usage: m16-features.sh TWIN.iso [IMAGE.img]}
image=${2:-}
out=${OUT:-$HOME/freelfs-work/scratch/m16}
only=${STEPS:-}	# STEPS="window-qol persist flauth": only these (default: all)
want() { [ -z "$only" ] || [[ " $only " == *" $1 "* ]]; }
mkdir -p "$out"
run() { # run NAME [QEMU args]
	local n=$1; shift
	sed "s|@OUT@|$out|g; s|@HERE@|$here/m16|g" "$here/m16/$n.cmds" > "$out/$n.run"
	echo "== $n"
	nice -n 10 python3 -u "$here/live-run.py" --iso "$twin" --mem 1G "$out/$n.run" -- -device usb-tablet "$@" | tee "$out/$n.log"
}
usb=$out/usb.img
[ -f "$usb" ] || { truncate -s 256M "$usb"; /usr/sbin/mkfs.exfat -L TESTUSB "$usb" > /dev/null; }
want usb-plug && run usb-plug -device qemu-xhci,id=xhci
want usb-notify && run usb-notify -device qemu-xhci,id=xhci
want poweroff-menu && run poweroff-menu
want clipboard && run clipboard
want window-qol && run window-qol
want files-dclick && run files-dclick
want freebws-popup && run freebws-popup
# persistence: a data partition (exFAT, with the ISO as a file) that has room for the image
disk=$HOME/freelfs-work/scratch/m16-stick.img	# on the disk: it is 1.8 GB, /tmp may be a RAM disk
if want persist; then
rm -f "$disk"
FLFS_DISK_EXTRA_MB=700 python3 "$here/iso-test-disk.py" "$twin" "$disk" exfat
for n in persist-1-create persist-2-write persist-3-check; do
	sed "s|@OUT@|$out|g" "$here/m16/$n.cmds" > "$out/$n.run"
	echo "== $n"
	nice -n 10 python3 -u "$here/live-run.py" --disk "$disk" --mem 1G "$out/$n.run" | tee "$out/$n.log"
done
fi
if [ -n "$image" ] && want flauth; then
	echo "== flauth (image $image)"
	cp "$here/m16/flauth.cmds" "$out/flauth.run"
	sed "s|\$OUT|$out|g" "$here/m16/flauth.tty" > "$out/flauth.tty"
	FLFS_IMG=$image FLFS_VM_MEM=1G FLFS_VM_TTY=$out/flauth.tty python3 "$here/vm-run.py" "$out/flauth.run" -snapshot -usb -device usb-tablet | tee "$out/flauth.log"
fi
echo "pictures in $out (convert the .ppm files; look at them)"
