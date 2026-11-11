#!/bin/bash
# Run every bootstrap stage in order, from host check to disk image. Each
# stage skips what it already finished, so this is also how to continue
# after fixing a failure.
#
# usage: ./build/all.sh   (as your normal user; then ./build/51-qemu.sh)

set -euo pipefail
cd "$(dirname "$0")"
for stage in 00-host-check 01-prepare 02-fetch 11-cross-toolchain \
	12-temp-tools 20-chroot-tools 30-base-toolchain 31-base-system \
	32-base-extra 40-system-config 41-kernel 60-packages; do
	printf '\n\033[1;34m######## %s\033[0m\n' "$stage"
	"./$stage.sh"
done

# The image holds the base system installed from packages (60-packages).
printf '\n\033[1;34m######## 50-image\033[0m\n'
./50-image.sh
