#!/bin/bash
# Desktop features in QEMU (notifications, lock screen, Settings > Date & time, time zone, fl-sntp,
# the ad blocker's first start at the given memory, an error page, setuid helpers):
#
#   ./build/m9-features.sh [2G 1G]     (needs the image: ./build/gentle.sh ./build/50-image.sh)
#
# Steps in build/m9-features.cmds (serial console) and build/m9-features.tty (keyboard and
# screenshots). Throwaway image copy (-snapshot). The test password of the account "tester" is
# set inside the VM only. Results: docs/m7/m9-<name>-<mem>.png, $FLFS_WORK/logs/m9-features-<mem>.log.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/config.sh"
. "$here/lib.sh"
mems=("$@")
[ ${#mems[@]} -gt 0 ] || mems=(2G 1G)
for mem in "${mems[@]}"; do
	tty=$(mktemp /tmp/flfs-m9-tty.XXXXXX)
	sed "s/@MEM@/$mem/g" "$here/m9-features.tty" > "$tty"
	rm -f /tmp/flfs-m9-*-"$mem".ppm
	FLFS_VM_MEM=$mem FLFS_VM_TTY=$tty "$here/gentle.sh" "$here/vm-run.py" "$here/m9-features.cmds" -snapshot -usb -device usb-tablet \
		2>&1 | tee "$FLFS_WORK/logs/m9-features-$mem.log"
	rm -f "$tty"
	for f in /tmp/flfs-m9-*-"$mem".ppm; do
		[ -e "$f" ] || continue
		name=$(basename "$f" .ppm); name=${name#flfs-}
		convert "$f" "$FLFS_TOP/docs/m7/${name}.png"
	done
done
