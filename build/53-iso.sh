#!/bin/bash
# M9: the live ISO, ~/freelfs-work/freelfs-live.iso, rootless.
#
#   live/root.sfs         squashfs (zstd) of the installed root (base + desktop)
#                         with the user "tester" (empty password, home
#                         /usr/tester), a live fstab (no @home, @snapshots or
#                         ESP: nothing mounts or writes the host's disks)
#   EFI/BOOT/BOOTX64.EFI  flboot (also in the ESP image below)
#   El Torito + GPT       a FAT ESP image (flboot, kernel, initrd.img,
#                         flboot.conf) is the El Torito UEFI boot image AND, as
#                         an appended partition, GPT partition 2: the file boots
#                         as a CD, written to a stick with dd, or (Ventoy) as a
#                         file on a stick; src/flinitrd finds it again
#
# Needs the "live" packages in the build root (squashfs-tools, libisoburn,
# flinitrd: ./build/gentle.sh ./build/60-packages.sh linux squashfs-tools
# libburn libisofs libisoburn flinitrd) and the image root from 60-packages.sh.
# The squashfs and the ISO are made inside the chroot (user namespace, no
# sudo); the root is overlaid there (overlayfs in the namespace), so the real
# root is never changed. Run it as ./build/gentle.sh ./build/53-iso.sh.
#
# usage: 53-iso.sh [--cmdline "EXTRA KERNEL OPTIONS"] [--level N]
#   --cmdline  adds to the kernel command line (a test can put
#              console=ttyS0,115200 last, to read the boot on the serial port)
#   --level    zstd level of the squashfs (default 15)
# Output: $FLFS_WORK/freelfs-live.iso

set -euo pipefail
. "$(dirname "$0")/config.sh"
. "$(dirname "$0")/lib.sh"

extra="" level=15
while [ $# -gt 0 ]; do
	case $1 in
		--cmdline) extra=$2; shift ;;
		--level) level=$2; shift ;;
		--in-chroot) ;;
		*) die "usage: $0 [--cmdline OPTIONS] [--level N]" ;;
	esac
	shift
done

# ---- on the host: enter the chroot, then move the result out -----------------------
if [ -z "${FLFS_IN_CHROOT:-}" ]; then
	[ "$EUID" -ne 0 ] || die "run this as your normal user, not root"
	[ -x "$FLFS_IMAGE_ROOT/usr/bin/bash" ] || die "no system in $FLFS_IMAGE_ROOT (run ./build/60-packages.sh)"
	mkdir -p "$FLFS_BOOTSTRAP/live"
	printf '%s\n' "$extra" > "$FLFS_BOOTSTRAP/live/cmdline.extra"
	"$FLFS_TOP/build/chroot.sh" /bin/bash --noprofile --norc \
		/.bootstrap/freelfs/build/53-iso.sh --in-chroot --level "$level"
	mv -f "$FLFS_BOOTSTRAP/live/freelfs-live.iso" "$FLFS_WORK/freelfs-live.iso"
	msg "live ISO: $FLFS_WORK/freelfs-live.iso ($(du -h "$FLFS_WORK/freelfs-live.iso" | cut -f1))"
	exit 0
fi

# ---- inside the chroot -----------------------------------------------------------------
chroot_env
ROOT=/.bootstrap/pkg/root
W=/.bootstrap/live
for t in mksquashfs xorriso gzip python3; do
	command -v "$t" > /dev/null || die "$t is not in the build root (build the live group: see the top of this script)"
done
[ -x /usr/lib/flinitrd/init ] || die "flinitrd is not in the build root (build the live group)"
[ -f "$ROOT/usr/lib/flboot/flboot.efi" ] || die "$ROOT has no flboot"
kernels=("$ROOT"/boot/vmlinuz-*)
[ ${#kernels[@]} -eq 1 ] && [ -f "${kernels[0]}" ] || die "expected exactly one kernel in $ROOT/boot"
kernel=${kernels[0]}
kver=${kernel##*/vmlinuz-}
case $FLFS_ARCH in
	x86_64) loader=$ROOT/usr/lib/ld-linux-x86-64.so.2 efi_name=BOOTX64.EFI ;;
	aarch64) loader=$ROOT/usr/lib/ld-linux-aarch64.so.1 efi_name=BOOTAA64.EFI ;;
esac
# the kernel needs everything on the path to the squashfs built in
for o in SQUASHFS_ZSTD ISO9660_FS OVERLAY_FS BLK_DEV_LOOP EXFAT_FS BLK_DEV_INITRD; do
	grep -qx "CONFIG_$o=y" "$ROOT/boot/config-$kver" ||
		die "kernel $kver lacks CONFIG_$o=y (rebuild: ./build/60-packages.sh linux)"
done
grep -qx 'CONFIG_SQUASHFS=y' "$ROOT/boot/config-$kver" || die "kernel $kver lacks CONFIG_SQUASHFS=y"

# tool NAME ARGS -- FreeLFS's own mtools/dosfstools from the image root
tool() {
	local t=$ROOT/usr/bin/$1
	[ -x "$t" ] || t=$ROOT/usr/sbin/$1
	[ -x "$t" ] || die "$1 is not installed in $ROOT"
	shift
	GCONV_PATH=$ROOT/usr/lib/gconv "$loader" --library-path "$ROOT/usr/lib" "$t" "$@"
}

rm -rf "$W/iso" "$W/stage" "$W/up" "$W/wk" "$W/esp.img" "$W/initrd.img" "$W/freelfs-live.iso"
mkdir -p "$W"/{iso/live,iso/EFI/BOOT,stage,up,wk}

# ---- 1. root.sfs ------------------------------------------------------------------------------
squash_step() {
	unshare --mount --propagation unchanged /bin/bash -euo pipefail -c '
		root=$1 w=$2 level=$3 jobs=$4
		mount -t overlay overlay \
			-o "lowerdir=$root,upperdir=$w/up,workdir=$w/wk" "$w/stage"
		settings=$(readlink -f "$w/stage/etc") data=$(readlink -f "$w/stage/var")
		[ -d "$settings" ] && [ -d "$data" ] || { echo "cannot find /etc and /var in the root" >&2; exit 1; }
		! grep -q "^tester:" "$settings/passwd" || { echo "the root already has a user tester" >&2; exit 1; }
		! grep -q "^[^:]*:[^:]*:1000:" "$settings/passwd" || { echo "uid 1000 is taken" >&2; exit 1; }
		[ ! -e "$settings/keyboard.conf" ] || { echo "the root has a keyboard preset" >&2; exit 1; }
		grp=$(awk -F: "\$1 == \"users\" {print \$3}" "$settings/group")
		[ -n "$grp" ] || { echo "no group users" >&2; exit 1; }
		echo "tester:x:1000:$grp:Tester:/usr/tester:/usr/bin/flsh" >> "$settings/passwd"
		echo "tester::20454:0:99999:7:::" >> "$settings/shadow"
		python3 -I - "$settings/group" "$settings/gshadow" <<-"PY"
		import sys
		for path in sys.argv[1:]:
		    out = []
		    for line in open(path).read().splitlines():
		        f = line.split(":")
		        if f[0] in ("seat", "audio", "video", "render") and len(f) >= 4:
		            f[3] = f[3] + ",tester" if f[3] else "tester"
		        out.append(":".join(f))
		    open(path, "w").write("\n".join(out) + "\n")
		PY
		# the home; the build runs as one uid, so rows in flpkg database tell
		# the first boot (flpkg fix-owners in rc.sysinit) who owns it
		home=$w/stage/usr/tester
		mkdir "$home"
		cp -a "$settings/skel/." "$home/"
		chmod 700 "$home"
		python3 -I - "$data/lib/flpkg/db.sqlite" "$w/stage/usr" "$grp" <<-"PY"
		import os, sqlite3, sys
		db, usr, gid = sys.argv[1], sys.argv[2], int(sys.argv[3])
		c = sqlite3.connect(db)
		top = os.path.join(usr, "tester")
		for dirpath, dirs, files in os.walk(top):
		    for n in dirs + files + ["."]:
		        p = os.path.normpath(os.path.join(dirpath, n))
		        mode = 0o700 if p == top else os.lstat(p).st_mode & 0o7777
		        rel = "usr/" + os.path.relpath(p, usr)
		        c.execute("INSERT OR REPLACE INTO pending_owners VALUES(?,?,?,?)", (rel, 1000, gid, mode))
		c.commit()
		c.close()
		PY
		# the live fstab: no @home, @snapshots or ESP (a disk of the host that
		# carries the same labels must never be mounted, let alone written), no
		# fsck, no swap; the root is the overlay flinitrd mounted
		cat > "$settings/fstab" <<-"FSTAB"
		# /etc/fstab -- FreeLFS live system. The root is a tmpfs overlay on the
		# squashfs; nothing here touches the disks of the machine.
		proc            /proc           proc      nosuid,noexec,nodev          0    0
		sysfs           /sys            sysfs     nosuid,noexec,nodev          0    0
		devpts          /dev/pts        devpts    gid=5,mode=620               0    0
		tmpfs           /run            tmpfs     defaults                     0    0
		devtmpfs        /dev            devtmpfs  mode=0755,nosuid             0    0
		tmpfs           /dev/shm        tmpfs     nosuid,nodev                 0    0
		tmpfs           /tmp            tmpfs     nosuid,nodev,mode=1777       0    0
		cgroup2         /sys/fs/cgroup  cgroup2   nosuid,noexec,nodev          0    0
		FSTAB
		sed -i "s/^snapshots = .*/snapshots = 0                   # live system: no btrfs/" "$settings/flpkg.conf"
		# RAM: login prompts on tty3 to tty6 are not needed in the live session (tty1 is the desktop, tty2 stays)
		sed -i "/^[3-6]:2345:respawn:\/sbin\/agetty/d" "$settings/inittab"
		printf "%s\n" "" "Live system: log in as tester, there is no password. Nothing is" \
			"written to the disks of this computer; changes are lost at shutdown." >> "$settings/issue"
		mksquashfs "$w/stage" "$w/iso/live/root.sfs" -quiet -noappend -comp zstd \
			-Xcompression-level "$level" -b 256K -no-xattrs -processors "$jobs" -mem 768M \
			-wildcards -e "boot/*" -e "var/cache/flpkg/*"
	' _ "$ROOT" "$W" "$level" "$FLFS_JOBS"
}
msg "root.sfs: squashfs (zstd level $level) of $ROOT plus the live changes"
squash_step
msg "root.sfs: $(du -h "$W/iso/live/root.sfs" | cut -f1)"

# ---- 2. the initramfs ---------------------------------------------------------------
msg "initramfs: flinitrd"
python3 -I - /usr/lib/flinitrd/init "$W/initrd.cpio" <<'PY'
import os, stat, struct, sys
init, out = sys.argv[1], sys.argv[2]
mtime = int(os.environ["SOURCE_DATE_EPOCH"])
entries = [  # name, mode, data, rdev major, minor
    ("dev", stat.S_IFDIR | 0o755, b"", 0, 0),
    ("dev/console", stat.S_IFCHR | 0o600, b"", 5, 1),
    ("dev/null", stat.S_IFCHR | 0o666, b"", 1, 3),
    ("proc", stat.S_IFDIR | 0o755, b"", 0, 0),
    ("sys", stat.S_IFDIR | 0o755, b"", 0, 0),
    ("run", stat.S_IFDIR | 0o755, b"", 0, 0),
    ("newroot", stat.S_IFDIR | 0o755, b"", 0, 0),
    ("init", stat.S_IFREG | 0o755, open(init, "rb").read(), 0, 0),
]
buf = bytearray()
for ino, (name, mode, data, maj, mnr) in enumerate(entries, 1):
    nm = name.encode() + b"\0"
    hdr = "070701" + "".join("%08X" % v for v in (
        ino, mode, 0, 0, 2 if stat.S_ISDIR(mode) else 1, mtime, len(data), 0, 0, maj, mnr, len(nm), 0))
    buf += hdr.encode() + nm
    buf += b"\0" * (-len(buf) % 4)
    buf += data
    buf += b"\0" * (-len(buf) % 4)
nm = b"TRAILER!!!\0"
buf += ("070701" + "".join("%08X" % v for v in (0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(nm), 0))).encode() + nm
buf += b"\0" * (-len(buf) % 4)
open(out, "wb").write(buf)
PY
gzip -9nc "$W/initrd.cpio" > "$W/initrd.img"
rm -f "$W/initrd.cpio"

# ---- 3. the ESP image (El Torito boot image and GPT partition 2) -----------------------
# FAT16 and under 32 MB. Tested: OVMF did not boot a 40 MB FAT32 El Torito
# image (its boot catalog size is 0: more than 65535 512-byte sectors cannot be
# stated) but did boot a 20 MB FAT16 one.
esp_kb=$(( ($(stat -c %s "$kernel") + $(stat -c %s "$W/initrd.img") + $(stat -c %s "$ROOT/usr/lib/flboot/flboot.efi")) / 1024 + 4096 ))
esp_mb=$(( (esp_kb + 1023) / 1024 ))
[ "$esp_mb" -ge 16 ] || esp_mb=16
[ "$esp_mb" -le 31 ] || die "kernel and initrd need an ESP image of ${esp_mb} MB; El Torito boot images must stay under 32 MB"
msg "ESP image: ${esp_mb} MB FAT16 with flboot, kernel $kver, initrd.img"
truncate -s "${esp_mb}M" "$W/esp.img"
tool mkfs.fat -F 16 -n FLLIVE -i F1EE1157 "$W/esp.img" > /dev/null
export MTOOLS_SKIP_CHECK=1 MTOOLSRC=/dev/null
tool mmd -i "$W/esp.img" ::/EFI ::/EFI/BOOT ::/flboot
tool mcopy -i "$W/esp.img" "$ROOT/usr/lib/flboot/flboot.efi" "::/EFI/BOOT/$efi_name"
tool mcopy -i "$W/esp.img" "$kernel" "::/vmlinuz-$kver"
# the installer (flinstall-helper) copies this kernel to the disk: the squashfs has no /boot
cp "$kernel" "$W/iso/live/vmlinuz-$kver"
tool mcopy -i "$W/esp.img" "$W/initrd.img" ::/initrd.img
# CPU microcode (package "microcode", 8-9 MB uncompressed cpio): not in the ESP image (El Torito
# images must stay under 32 MB, the live system does not need it), but the installer copies it
# from here to the disk's ESP next to the kernel; flboot.conf there gets initrd=\ucode.img
[ ! -f "$ROOT/boot/ucode.img" ] || cp "$ROOT/boot/ucode.img" "$W/iso/live/ucode.img"
VT_COLORS="vt.default_red=29,224,63,216,111,198,86,216,122,239,95,232,143,215,127,240 vt.default_grn=33,108,155,181,168,120,182,220,129,128,191,201,192,160,208,242 vt.default_blu=37,108,99,106,220,221,194,224,137,128,131,138,236,232,218,244"
# console=tty0 last: it is /dev/console, so the keyboard and the screen of a
# real machine work in flinitrd's prompt
extra=$(cat "$FLFS_BOOTSTRAP/live/cmdline.extra" 2> /dev/null || true)
base="initrd=\\initrd.img ro panic=30 console=ttyS0,115200 console=tty0 $VT_COLORS"
quiet="quiet"
cat > "$W/flboot.conf" <<-EOF
	# flboot.conf -- written by build/53-iso.sh for the live ISO
	timeout 3
	cmdline $base $quiet $extra
	entry FreeLFS live (verbose: flive.debug)|\\vmlinuz-$kver|$base flive.debug $extra
EOF
tool mcopy -i "$W/esp.img" "$W/flboot.conf" ::/flboot/flboot.conf
cp "$ROOT/usr/lib/flboot/flboot.efi" "$W/iso/EFI/BOOT/$efi_name"
cat > "$W/iso/README.txt" <<'EOF'
FreeLFS live ISO.

  Ventoy:  copy this file (keep a name like freelfs-live*.iso) to the Ventoy
           partition, pick it in the Ventoy UEFI menu. Secure Boot must be off.
  dd:      write it to a USB stick (everything on the stick is lost), or burn it.
  Login:   user tester, no password. Nothing is written to the computer's disks.
EOF

# ---- 4. the ISO ---------------------------------------------------------------------------------
msg "ISO: xorriso (El Torito UEFI, GPT with the ESP as partition 2)"
xorriso -as mkisofs -quiet \
	-iso-level 3 -full-iso9660-filenames -rational-rock -joliet -joliet-long \
	-volid FREELFS_LIVE -appid "FreeLFS live" -publisher "FreeLFS" \
	-preparer "FreeLFS build/53-iso.sh" \
	-appended_part_as_gpt \
	-append_partition 2 C12A7328-F81F-11D2-BA4B-00A0C93EC93B "$W/esp.img" \
	-iso_mbr_part_type EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 \
	-eltorito-platform efi \
	-e --interval:appended_partition_2:all:: -no-emul-boot \
	-isohybrid-gpt-basdat \
	-o "$W/freelfs-live.iso" "$W/iso" 2>&1 | grep -v '^$' || true
[ -s "$W/freelfs-live.iso" ] || die "xorriso made no ISO"
rm -rf "$W/iso/live" "$W/stage" "$W/up" "$W/wk"
msg "ISO ready: $(du -h "$W/freelfs-live.iso" | cut -f1)"
