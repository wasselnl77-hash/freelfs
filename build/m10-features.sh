#!/bin/bash
# M10 desktop features in QEMU on the real image: workspaces and the overview, Snappel (Print), General
# Manager (Ctrl+Shift+Esc), accent colour, system sounds, fledit, Archives, Images/Documents/Music/Videos
# (with the media keys), Files opening by MIME type, the Software Store, autostart, flsnap on the real btrfs.
#
#   ./build/m10-features.sh [2G 1G]     (needs the image: ./build/gentle.sh ./build/50-image.sh)
#
# Steps in build/m10-features.cmds (serial console) and build/m10-features.tty (keyboard and screenshots).
# Sample files come from build/m10-samples.py (a second, read-only virtio disk). Throwaway image copy
# (-snapshot). Results: docs/m7/m10-<name>-<mem>.png, $FLFS_WORK/logs/m10-features-<mem>.log.
# The snapshot boot and rollback, and the installer, are build/m10-snapshots.sh and build/m10-install.sh.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/config.sh"
. "$here/lib.sh"
mems=("$@")
[ ${#mems[@]} -gt 0 ] || mems=(2G 1G)
mkdir -p "$FLFS_WORK/scratch/m10" "$FLFS_WORK/logs"
[ -f "$FLFS_WORK/m10-samples.img" ] || "$here/m10-samples.py" "$FLFS_WORK/m10-samples.img"
for mem in "${mems[@]}"; do
	shots=$FLFS_WORK/scratch/m10/$mem
	rm -rf "$shots"
	mkdir -p "$shots"
	tty=$(mktemp "$FLFS_WORK/scratch/m10/tty.XXXXXX")
	sed -e "s/@MEM@/$mem/g" -e "s|@S@|$shots|g" "$here/m10-features.tty" > "$tty"
	FLFS_VM_MEM=$mem FLFS_VM_TTY=$tty "$here/gentle.sh" "$here/vm-run.py" "$here/m10-features.cmds" -snapshot -usb -device usb-tablet \
		-drive "file=$FLFS_WORK/m10-samples.img,format=raw,if=virtio,readonly=on" \
		2>&1 | tee "$FLFS_WORK/logs/m10-features-$mem.log"
	rm -f "$tty"
	for f in "$shots"/*.ppm; do
		[ -e "$f" ] || continue
		name=$(basename "$f" .ppm)
		magick "$f" "$FLFS_TOP/docs/m7/m10-${name}-${mem}.png"
		rm -f "$f"
	done
done
