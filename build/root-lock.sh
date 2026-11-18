#!/bin/bash
# Lock or unlock root in the build root ($FLFS_IMAGE_ROOT), rootless. Round 23.
#   build/root-lock.sh          lock root (shadow "!"): release state, the default after 60-packages.sh
#   build/root-lock.sh --dev    empty root hash: ONLY for test images (disk image, USB image, twin ISOs);
#                               needs FLFS_DEV_ROOT=1 in the environment as a second key
# 50-image.sh, 52-usb.sh and 53-iso.sh refuse to build a release image from a root with an empty hash.
set -euo pipefail
. "$(dirname "$0")/config.sh"
. "$(dirname "$0")/lib.sh"
sh=$FLFS_IMAGE_ROOT/etc/shadow
[ -f "$sh" ] || die "no $sh (run ./build/60-packages.sh)"
case ${1:-} in
	--dev)
		[ "${FLFS_DEV_ROOT:-0}" = 1 ] || die "--dev needs FLFS_DEV_ROOT=1 (test images only)"
		set_root_hash "$sh" ""
		grep -qx ttyS0 "$FLFS_IMAGE_ROOT/etc/securetty" || echo ttyS0 >> "$FLFS_IMAGE_ROOT/etc/securetty"
		install -m644 "$FLFS_BOOTSTRAP/pkg/keys/bootstrap.pub" "$FLFS_IMAGE_ROOT/etc/flpkg/keys/" # test images keep the build-repo key
		grep -q '^Development image' "$FLFS_IMAGE_ROOT/etc/issue" ||
			sed -i '/^FreeLFS/a Development image: log in as root, there is no password.' "$FLFS_IMAGE_ROOT/etc/issue"
		msg "root UNLOCKED (empty hash) in $FLFS_IMAGE_ROOT: a test image, not a release" ;;
	"")
		set_root_hash "$sh" '!'
		sed -i '/^ttyS/d;/^console/d' "$FLFS_IMAGE_ROOT/etc/securetty"
		rm -f "$FLFS_IMAGE_ROOT/etc/flpkg/keys/bootstrap.pub" # only the repo key is trusted in a release image
		sed -i '/^Development image:/d' "$FLFS_IMAGE_ROOT/etc/issue"
		msg "root locked in $FLFS_IMAGE_ROOT" ;;
	*) die "usage: $0 [--dev]" ;;
esac
