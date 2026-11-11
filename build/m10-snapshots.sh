#!/bin/bash
# M10: snapshots on the real btrfs. Three boots of a writable COPY of the image (the image itself is not
# touched; the copy is deleted at the end):
#   1  a snapshot with flsnap, a change after it, a second snapshot
#   2  flboot: "Start an earlier state" -> snapshot 1 (the oldest; read-only root); login, the notice, General Manager;
#      then `flsnap rollback`
#   3  the default entry: the root is the old state again, the state we left is a snapshot too
#
#   ./build/m10-snapshots.sh [MEM]      (default 2G; needs the image: ./build/gentle.sh ./build/50-image.sh)
#
# Steps in build/m10-snap{1,2,3}.cmds, build/m10-snap2.tty. Pictures: docs/m7/m10-snap-*.png (and the
# flboot screens with the list of snapshots). Log: $FLFS_WORK/logs/m10-snapshots.log
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/config.sh"
. "$here/lib.sh"
mem=${1:-2G}
work=$FLFS_WORK/scratch/m10-snap
shots=$work/shots
rm -rf "$work"
mkdir -p "$shots"
copy=$work/disk.img
msg "copy of the image: $copy"
cp --sparse=always "$FLFS_IMG" "$copy"
export FLFS_IMG=$copy FLFS_VM_MEM=$mem
log=$FLFS_WORK/logs/m10-snapshots.log
: > "$log"
sed -e "s|@S@|$shots|g" "$here/m10-snap2.tty" > "$work/snap2.tty"
{
	msg "boot 1: snapshots"
	"$here/gentle.sh" "$here/vm-run.py" "$here/m10-snap1.cmds"
	msg "boot 2: Start an earlier state (flboot: Down, Enter, then Down, Enter for snapshot 1, Enter)"
	FLFS_VM_KEYS="down ret shot:$shots/flboot-list.ppm down ret shot:$shots/flboot-ask.ppm ret" FLFS_VM_TTY=$work/snap2.tty \
		"$here/gentle.sh" "$here/vm-run.py" "$here/m10-snap2.cmds" -usb -device usb-tablet
	msg "boot 3: the default entry after the rollback"
	"$here/gentle.sh" "$here/vm-run.py" "$here/m10-snap3.cmds"
} 2>&1 | tee -a "$log"
for f in "$shots"/*.ppm; do
	[ -e "$f" ] || continue
	magick "$f" "$FLFS_TOP/docs/m7/m10-$(basename "$f" .ppm)-${mem}.png"
done
rm -rf "$work"
