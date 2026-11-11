#!/bin/bash
# M8: everything after the WPE WebKit build, unattended. Waits for the running
# `60-packages.sh ... wpewebkit` to end, then (always through gentle.sh, two
# jobs): builds flui, flcomp, freebws; makes the image; runs the FreeBWS test in
# QEMU at 2 GB and 1 GB (build/m8-freebws.sh); runs boot-test.py at 2 GB and
# 1 GB; makes the ISO. Stops at the first failure; every step is resumable by
# starting the script again. Log: $FLFS_WORK/logs/m8-finish.log.
#
#   nohup ./build/m8-finish.sh > /dev/null 2>&1 &
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
. "$here/config.sh"
. "$here/lib.sh"
log=$FLFS_WORK/logs/m8-finish.log
exec >> "$log" 2>&1
export FLFS_JOBS=1
step() { echo "=== $(date +%T) $*"; }

# WPE WebKit: wait for a build that is running, or start (resume) one. The
# recipe keeps ninja's build dir in /.bootstrap/pkg/wpewebkit-resume when
# ninja stops, so a restart goes on where it was.
if [ ! -e "$FLFS_STAMPS/pkg-wpewebkit" ]; then
	step "wpewebkit"
	while pgrep -f 'build/60-packages.sh' > /dev/null; do sleep 60; done
	if [ ! -e "$FLFS_STAMPS/pkg-wpewebkit" ]; then
		"$here/gentle.sh" "$here/60-packages.sh" wpewebkit || { step "FAILED: wpewebkit (see logs/pkg-wpewebkit.log)"; exit 1; }
	fi
fi

step "packages: flui flcomp flpanel flsettings flcrash flboot freebws"
"$here/gentle.sh" "$here/60-packages.sh" flui flcomp flpanel flsettings flcrash flboot freebws || { step "FAILED: packages"; exit 1; }
step "image"
"$here/gentle.sh" "$here/50-image.sh" || { step "FAILED: image"; exit 1; }
step "freebws in QEMU"
"$here/m8-freebws.sh" 2G 1G || step "FAILED: m8-freebws.sh (go on)"
for mem in 2G 1G; do
	step "boot-test at $mem"
	FLFS_VM_MEM=$mem "$here/gentle.sh" "$here/boot-test.py" > "$FLFS_WORK/logs/m8-boot-test-$mem.log" 2>&1
	echo "boot-test $mem: exit $?  (tail follows)"; tail -n 6 "$FLFS_WORK/logs/m8-boot-test-$mem.log"
done
step "iso"
"$here/gentle.sh" "$here/53-iso.sh" || { step "FAILED: iso"; exit 1; }
step "done"
