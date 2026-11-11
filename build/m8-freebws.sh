#!/bin/bash
# M8 step 3: boot the image in QEMU at 2 GB and at 1 GB, start FreeBWS from the
# start menu, load its test page, take screenshots (docs/m7/freebws-*.png) and
# record the resident size of every WebKit process.
#
#   ./build/m8-freebws.sh [2G 1G]     (needs the image: ./build/gentle.sh ./build/50-image.sh)
#
# Five parts in one boot: one tab through five origins and back, three and four tabs,
# links (target=_blank, Ctrl-Enter, window.open), discarding of idle background tabs,
# the native DSSL page against the engine's. The VM runs on a throwaway copy of the
# image (-snapshot), so the account, the layout and the wrapper the test makes do not
# stay. Results: docs/m7/freebws-<name>-<mem>.png and
# $FLFS_WORK/logs/m8-freebws-<mem>.log (per process rss and pss in kB; "total_kb" is
# the sum over freebws, WPEWebProcess, WPENetworkProcess and flcomp; rss counts
# shared libraries once per process, pss divides them, free -m is the system's view).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/config.sh"
. "$here/lib.sh"
mems=("$@")
[ ${#mems[@]} -gt 0 ] || mems=(2G 1G)
for mem in "${mems[@]}"; do
	tty=$(mktemp /tmp/flfs-m8-tty.XXXXXX)
	sed "s/@MEM@/$mem/g" "$here/m8-freebws.tty" > "$tty"
	rm -f /tmp/flfs-m8-*-"$mem".ppm
	FLFS_VM_MEM=$mem FLFS_VM_TTY=$tty "$here/gentle.sh" "$here/vm-run.py" "$here/m8-freebws.cmds" -snapshot \
		2>&1 | tee "$FLFS_WORK/logs/m8-freebws-$mem.log"
	rm -f "$tty"
	for f in /tmp/flfs-m8-*-"$mem".ppm; do
		[ -e "$f" ] || continue
		name=$(basename "$f" .ppm); name=${name#flfs-m8-}
		convert "$f" "$FLFS_TOP/docs/m7/freebws-${name}.png"
	done
done
