#!/bin/bash
# M4: assemble the bootable disk image from a FreeLFS root, without root.
#
#   partition 1  EFI system partition (FAT32, label ESP): flboot as
#                EFI/BOOT/BOOTX64.EFI, the kernels from /boot, and
#                flboot/flboot.conf; mounted at /boot on the system
#   partition 2  btrfs (label freelfs) with subvolumes @ (the default one,
#                mounted at /), @home (mounted at /usr: the homes) and
#                @snapshots
#
# Every tool comes from the FreeLFS root itself (sfdisk, mkfs.fat, mtools,
# mkfs.btrfs), run through its own loader. The btrfs file system is filled
# from the root laid out as bind mounts in a throwaway mount namespace, so
# files are stored as owned by root. Partition and file system ids are
# fixed: the same root gives the same image, and flboot.conf can name the
# root partition by PARTUUID.
#
# usage: 50-image.sh   (as your normal user, after 60-packages.sh;
#                       FLFS_IMAGE_ROOT picks another root, FLFS_IMG and
#                       FLFS_IMG_SIZE the file and its size; FLFS_IMAGE_HOOK
#                       names a program called as "HOOK --image-hook STAGE
#                       ROOT" before the file system is made: see 52-usb.sh)

set -euo pipefail
. "$(dirname "$0")/config.sh"
. "$(dirname "$0")/lib.sh"

[ "$EUID" -ne 0 ] || die "run this as your normal user, not root"
ROOT=$FLFS_IMAGE_ROOT
[ -x "$ROOT/usr/bin/bash" ] || die "no system in $ROOT (run ./build/60-packages.sh)"
[ -f "$ROOT/usr/lib/flboot/flboot.efi" ] || die "$ROOT has no flboot (install the flboot package)"
[ "$(uname -m)" = "$FLFS_ARCH" ] || die "building a $FLFS_ARCH image needs a $FLFS_ARCH host"

case $FLFS_ARCH in
	x86_64) loader=$ROOT/usr/lib/ld-linux-x86-64.so.2 efi_name=BOOTX64.EFI
		root_type=4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709 ;; # Linux root (x86-64)
	aarch64) loader=$ROOT/usr/lib/ld-linux-aarch64.so.1 efi_name=BOOTAA64.EFI
		root_type=B921B045-1DF0-41C3-AF44-4C6F280D3FAE ;; # Linux root (AArch64)
esac
esp_type=C12A7328-F81F-11D2-BA4B-00A0C93EC93B

# Fixed ids (a dev image; the installer makes fresh ones).
DISK_ID=f1ee1f50-0000-4000-8000-000000000000
ESP_PARTUUID=f1ee1f50-0000-4000-8000-000000000001
ROOT_PARTUUID=f1ee1f50-0000-4000-8000-000000000002
BTRFS_UUID=f1ee1f50-0000-4000-8000-0000000000b7
ESP_VOLID=F1EE0001

# tool NAME ARGS -- run FreeLFS's own NAME (from usr/bin or usr/sbin)
tool() {
	local t=$ROOT/usr/bin/$1
	[ -x "$t" ] || t=$ROOT/usr/sbin/$1
	[ -x "$t" ] || die "$1 is not installed in $ROOT"
	shift
	# glibc's character set modules too (mtools converts file names)
	GCONV_PATH=$ROOT/usr/lib/gconv "$loader" --library-path "$ROOT/usr/lib" "$t" "$@"
}

ESP_MB=512
work=$FLFS_WORK/image-work
rm -rf "$work"
mkdir -p "$work"

# Sizes in MiB: 1 for the GPT and alignment, the ESP, the rest btrfs (minus
# 1 for the backup GPT).
disk_mb=$(numfmt --from=iec "$FLFS_IMG_SIZE")
disk_mb=$((disk_mb / 1048576))
root_mb=$((disk_mb - 1 - ESP_MB - 1))
[ "$root_mb" -gt 2048 ] || die "FLFS_IMG_SIZE is too small"

# ---- the EFI system partition ---------------------------------------------------
msg "ESP: flboot, kernels, flboot.conf"
truncate -s "${ESP_MB}M" "$work/esp.img"
tool mkfs.fat -F 32 -n ESP -i "$ESP_VOLID" "$work/esp.img" > /dev/null
export MTOOLS_SKIP_CHECK=1 MTOOLSRC=/dev/null
tool mmd -i "$work/esp.img" ::/EFI ::/EFI/BOOT ::/flboot
tool mcopy -i "$work/esp.img" "$ROOT/usr/lib/flboot/flboot.efi" "::/EFI/BOOT/$efi_name"
for f in "$ROOT"/boot/*; do
	[ -f "$f" ] && tool mcopy -i "$work/esp.img" "$f" "::/${f##*/}"
done
# the console in the FreeLFS colours (flboot's palette): background,
# red, green, yellow, blue, magenta, cyan, text, then the bright ones
VT_COLORS="vt.default_red=29,224,63,216,111,198,86,216,122,239,95,232,143,215,127,240 vt.default_grn=33,108,155,181,168,120,182,220,129,128,191,201,192,160,208,242 vt.default_blu=37,108,99,106,220,221,194,224,137,128,131,138,236,232,218,244"
cat > "$work/flboot.conf" <<-EOF
	# flboot.conf -- written by build/50-image.sh for this image
	timeout 3
	cmdline root=PARTUUID=$ROOT_PARTUUID rootwait ro quiet panic=30 console=tty0 console=ttyS0,115200 $VT_COLORS
EOF
tool mcopy -i "$work/esp.img" "$work/flboot.conf" ::/flboot/flboot.conf

# ---- the btrfs root ------------------------------------------------------------------
msg "btrfs: subvolumes @, @home, @snapshots"
truncate -s "${root_mb}M" "$work/root.img"
unshare --map-root-user --mount /bin/bash -euo pipefail -c '
	root=$1 stage=$2 img=$3
	shift 3
	mkdir -p "$stage"
	mount -t tmpfs -o mode=0755 tmpfs "$stage"
	mkdir "$stage/@" "$stage/@home" "$stage/@snapshots"
	mount --rbind "$root" "$stage/@"
	# @home is mounted at /usr: it carries the links in /usr (bin -> the
	# system, ...) as well, so /usr/bin works with it mounted
	cp -a "$root/usr/." "$stage/@home/"
	# /boot is the ESP on the running system: leave the mount point empty
	mount -t tmpfs -o mode=0755 tmpfs "$stage/@/boot"
	# build/52-usb.sh adds a preset user here: it may bind-mount changed
	# files over the staged root and add to the staged home, never touching
	# the real root
	[ -z "${FLFS_IMAGE_HOOK:-}" ] || "$FLFS_IMAGE_HOOK" --image-hook "$stage" "$root"
	"$@" -f -L freelfs --rootdir "$stage" \
		--subvol default:@ --subvol @home --subvol @snapshots "$img"
' _ "$ROOT" "$work/stage" "$work/root.img" \
	"$loader" --library-path "$ROOT/usr/lib" "$ROOT/usr/bin/mkfs.btrfs" \
	-U "$BTRFS_UUID" > "$work/mkfs.log" 2>&1 || { cat "$work/mkfs.log" >&2; die "mkfs.btrfs failed"; }

# ---- the disk ----------------------------------------------------------------------
msg "GPT disk: $FLFS_IMG ($FLFS_IMG_SIZE, sparse)"
rm -f "$FLFS_IMG"
truncate -s "${disk_mb}M" "$FLFS_IMG"
tool sfdisk --quiet "$FLFS_IMG" <<-EOF
	label: gpt
	label-id: $DISK_ID
	unit: sectors
	first-lba: 2048

	start=2048, size=$((ESP_MB * 2048)), type=$esp_type, uuid=$ESP_PARTUUID, name="EFI system partition"
	start=$(((1 + ESP_MB) * 2048)), size=$((root_mb * 2048)), type=$root_type, uuid=$ROOT_PARTUUID, name="FreeLFS"
EOF
dd if="$work/esp.img" of="$FLFS_IMG" bs=1M seek=1 conv=notrunc,sparse status=none
dd if="$work/root.img" of="$FLFS_IMG" bs=1M seek=$((1 + ESP_MB)) conv=notrunc,sparse status=none
rm -rf "$work"

msg "image ready: $FLFS_IMG ($(du -h "$FLFS_IMG" | cut -f1) used); boot it with ./build/51-qemu.sh"
