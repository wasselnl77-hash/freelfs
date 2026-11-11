#!/bin/bash
# Setup (LFS 13.1 section 4.2): create the minimal directory layout in the
# target root. Runs as your normal user; safe to re-run.

set -euo pipefail
. "$(dirname "$0")/config.sh"
. "$(dirname "$0")/lib.sh"

[ "$EUID" -ne 0 ] || die "run this as your normal user, not root"

mkdir -p "$FLFS"/{etc,var} "$FLFS"/usr/{bin,lib,sbin}
for i in bin lib sbin; do
	[ -L "$FLFS/$i" ] || ln -s "usr/$i" "$FLFS/$i"
done
case $FLFS_ARCH in
x86_64) mkdir -p "$FLFS/lib64" ;;
esac
mkdir -p "$FLFS_STAMPS" "$FLFS_SOURCES"
# /tools holds the cross toolchain until chapter 7 removes it.
[ -e "$FLFS_STAMPS/7.15-cleanup" ] || mkdir -p "$FLFS_TOOLS"
msg "target root prepared at $FLFS"
