#!/bin/bash
# M2 part 6 (LFS 10.3): the LTS kernel, built from the architecture's
# defconfig plus config/kernel/freelfs.config, installed to /boot and
# /usr/lib/modules. Runs in the rootless chroot. Resumable.
#
# usage: 41-kernel.sh   (as your normal user, after 40-system-config.sh)

set -euo pipefail
. "$(dirname "$0")/config.sh"
. "$(dirname "$0")/lib.sh"
chroot_env "$@"

[ -e "$FLFS_STAMPS/9.3-etc" ] || die "run ./build/40-system-config.sh first"

FRAGMENT=$FLFS_TOP/config/kernel/freelfs.config

# check_config -- every setting in the fragment must be in the final
# .config; olddefconfig silently drops a symbol whose dependencies are off.
check_config() {
	local line sym bad=0
	while read -r line; do
		case $line in
			CONFIG_*=*)
				grep -qx -- "$line" .config || { echo "not applied: $line"; bad=1; }
				;;
			"# CONFIG_"*" is not set")
				sym=${line#\# }
				sym=${sym%% *}
				! grep -q "^$sym=" .config || { echo "not applied: $line"; bad=1; }
				;;
		esac
	done < "$FRAGMENT"
	return "$bad"
}

kernel_build() {
	local karch image kver
	case $FLFS_ARCH in
		x86_64) karch=x86_64 image=arch/x86/boot/bzImage ;;
		aarch64) karch=arm64 image=arch/arm64/boot/Image ;;
	esac
	export ARCH=$karch
	make mrproper
	make defconfig
	scripts/kconfig/merge_config.sh -m .config "$FRAGMENT"
	make olddefconfig
	check_config
	make
	make INSTALL_MOD_STRIP=1 modules_install
	kver=$(make -s kernelrelease)
	install -d /boot
	cp "$image" "/boot/vmlinuz-$kver"
	cp System.map "/boot/System.map-$kver"
	cp .config "/boot/config-$kver"
	ln -sfn "vmlinuz-$kver" /boot/vmlinuz
}

build_pkg 10.3-kernel linux kernel_build

msg "kernel installed: $(readlink /boot/vmlinuz)"
