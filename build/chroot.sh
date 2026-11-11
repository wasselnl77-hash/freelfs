#!/bin/bash
# Enter the FreeLFS target root without sudo.
#
#   ./build/chroot.sh              interactive shell inside the target
#   ./build/chroot.sh CMD ARGS...  run CMD inside the target
#
# This runs in a new user + mount + pid namespace (unshare --map-root-user),
# where your user is mapped to root. Only that one uid exists inside, so
# chown to any other user or group fails; config/ownership.list records those
# exceptions for the image step. Every mount made here disappears with the
# namespace, so there is nothing to unmount and the host is never touched.

set -euo pipefail
. "$(dirname "$0")/config.sh"
. "$(dirname "$0")/lib.sh"

if [ -z "${FLFS_NS:-}" ]; then
	[ "$EUID" -ne 0 ] || die "run this as your normal user, not root"
	[ -z "${FLFS_IN_CHROOT:-}" ] || die "already inside the chroot"
	[ -x "$FLFS/usr/bin/bash" ] || die "no system in $FLFS yet (run M1 first)"
	mkdir -p "$FLFS"/{dev,proc,sys,run} "$FLFS_BOOTSTRAP"/{freelfs,logs} "$FLFS_LOGS"
	FLFS_NS=1 exec unshare --map-root-user --mount --pid --fork --kill-child \
		/bin/bash "$0" "$@"
fi

# Now "root" in the new namespaces, still in the host's filesystem.
mount --rbind /dev "$FLFS/dev"
mount -t proc proc "$FLFS/proc"
mount --rbind /sys "$FLFS/sys"
mount -t tmpfs -o mode=0755 tmpfs "$FLFS/run"
mount --bind "$FLFS_TOP" "$FLFS_BOOTSTRAP/freelfs"
mount --bind "$FLFS_LOGS" "$FLFS_BOOTSTRAP/logs"

[ $# -gt 0 ] || set -- /bin/bash --login
exec chroot "$FLFS" /usr/bin/env -i \
	HOME=/root TERM="${TERM:-dumb}" PS1='(freelfs chroot) \u:\w\$ ' \
	PATH=/usr/bin:/usr/sbin FLFS_IN_CHROOT=1 FLFS_ARCH="$FLFS_ARCH" \
	FLFS_JOBS="$FLFS_JOBS" FLFS_CHECK="$FLFS_CHECK" \
	"$@"
