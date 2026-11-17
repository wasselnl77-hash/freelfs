#!/bin/bash
# Round 20 tests (QEMU, KVM), through the real UI as the live user `tester`. Needs the SERIAL TWIN of the live ISO
# (./build/53-iso.sh --cmdline console=ttyS0,115200, copied away). For nomodeset: a second twin built with
# --cmdline "console=ttyS0,115200 nomodeset" (that is what the "Safe graphics" boot entry adds).
#
#   build/m20-features.sh TWIN.iso [NOMODESET-TWIN.iso]     STEPS="ventoy-eject ..." to run some
#
# ventoy-eject    Ventoy layout stick, a device-mapper device over it (a Ventoy boot): the stick is "boot", no eject
#                 button, flmount refuses, the stick stays, General Manager still opens (pixels)
# ventoy-replug   Ventoy layout stick, no system on it: eject in Files works, apps open, plug it in again twice,
#                 each time readable (no input/output error)
# freebws-watchdog  the frame watchdog falls back to shm (config + log + restart on the same page) and the hang
#                 watchdog ends a stalled web process (a local server that stalls for ever)
# desktop-rubber  rubber-band selections dragged across the desktop leave nothing behind (the Acer's green outlines)
# wifi-modules    the Wi-Fi modules (Atheros, Realtek, Intel, MediaTek, Broadcom) and their firmware are in the image, lspci/lsusb/ip
#                 work for an ordinary user, flreport lists the network controllers. Real Wi-Fi is NOT testable in QEMU.
# splash-style    flat (default) and legacy boot screen, flsplash-set, the boot-step log (no "rc: step failed")
# nomodeset       (second twin) boots with nomodeset: simpledrm, flcomp says so, the desktop is up, the panel and
#                 a window draw, the night light works in software (flcomp log), Files opens
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
twin=${1:?usage: m20-features.sh TWIN.iso [NOMODESET-TWIN.iso]}
twin2=${2:-}
out=${OUT:-$HOME/freelfs-work/scratch/m20}
only=${STEPS:-}
want() { [ -z "$only" ] || [[ " $only " == *" $1 "* ]]; }
mkdir -p "$out"
rc=0
R=$HOME/freelfs-work/rootfs-x86_64
gcc --sysroot=$R -O2 -o "$out/dmlinear" "$here/m20/dmlinear.c" || exit 1
# a Ventoy stick: MBR, p1 exFAT "Ventoy" (most of the stick), p2 FAT "VTOYEFI" (32 MB at the end)
ven=$out/ventoy.img
if [ ! -s "$ven" ]; then
	rm -f "$ven" "$ven".p1 "$ven".p2
	truncate -s 300M "$ven"
	printf 'label: dos\nstart=2048, size=%d, type=7\nsize=65536, type=ef\n' $(( (300*1024*1024)/512 - 2048 - 65536 )) | /usr/sbin/sfdisk --quiet "$ven"
	truncate -s $((65536*512)) "$ven.p2"; /usr/sbin/mkfs.vfat -n VTOYEFI "$ven.p2" > /dev/null
	truncate -s $(( ((300*1024*1024)/512 - 2048 - 65536) * 512 )) "$ven.p1"; /usr/sbin/mkfs.exfat -L Ventoy "$ven.p1" > /dev/null
	dd if="$ven.p1" of="$ven" bs=512 seek=2048 conv=notrunc status=none
	dd if="$ven.p2" of="$ven" bs=512 seek=$(( (300*1024*1024)/512 - 65536 )) conv=notrunc status=none
	rm -f "$ven.p1" "$ven.p2"
fi
run() { # run NAME ISO [QEMU args]
	local n=$1 iso=$2; shift 2
	sed "s|@OUT@|$out|g; s|@HERE@|$here/m20|g; s|@VENTOY@|$ven|g" "$here/m20/$n.cmds" > "$out/$n.run"
	echo "== $n"
	nice -n 10 python3 -u "$here/live-run.py" --iso "$iso" --mem 1G "$out/$n.run" -- -device usb-tablet "$@" > "$out/$n.log" 2>&1
	local r=$?
	grep -E "^(FAIL|NOT ok|RESULT|pixels|colours)" "$out/$n.log" | tail -n 30
	[ $r -eq 0 ] || { echo "-> $n FAILED (exit $r)"; rc=1; }
}
want ventoy-eject && run ventoy-eject "$twin" -device qemu-xhci,id=xhci
want ventoy-replug && run ventoy-replug "$twin" -device qemu-xhci,id=xhci
want freebws-watchdog && run freebws-watchdog "$twin"
want desktop-rubber && run desktop-rubber "$twin"
want wifi-modules && run wifi-modules "$twin"
want splash-style && run splash-style "$twin"
if want nomodeset; then
	if [ -n "$twin2" ]; then
		run nomodeset "$twin2" -vga std
	else
		echo "nomodeset: NOT RUN (no second twin given)"; rc=1
	fi
fi
echo "pictures and logs in $out"
exit $rc
