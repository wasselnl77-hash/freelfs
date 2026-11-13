#!/bin/bash
# image-test.sh -- run flinstall-helper for real, but only on a regular file in a scratch directory.
# SPDX-License-Identifier: MIT
#
#   tests/image-test.sh SCRATCH_DIR [HELPER]
#
# No privileges, no device: the helper's --image mode partitions and formats a sparse file, and
# builds the btrfs layout with mkfs.btrfs --rootdir inside a user and mount namespace (the same way
# build/50-image.sh does). FreeLFS's own tools (sfdisk, mkfs.btrfs, mkfs.fat, mtools, ...) are taken
# from a FreeLFS root (FLFS_IMAGE_ROOT, default ~/freelfs-work/rootfs-x86_64/.bootstrap/pkg/root)
# and run through its dynamic loader. The "live system" is a small stand-in tree made here from
# that root's /etc (settings) plus the changes build/53-iso.sh makes (user tester, live fstab, ...).
# Set FULL=1 to install the whole root instead of the stand-in (takes minutes and 3 GB of scratch).
#
# Never run this with a device path: the script only ever names files under SCRATCH_DIR.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
scratch=${1:?usage: image-test.sh SCRATCH_DIR [HELPER]}
helper=${2:-$here/../flinstall-helper}
ROOT=${FLFS_IMAGE_ROOT:-$HOME/freelfs-work/rootfs-x86_64/.bootstrap/pkg/root}
case $scratch in /dev/*) echo "refusing a path under /dev" >&2; exit 2 ;; esac
mkdir -p "$scratch"
scratch=$(cd "$scratch" && pwd)
[ -x "$helper" ] || { echo "no helper at $helper (make first)" >&2; exit 2; }
# where a FreeLFS root keeps things: /usr/... (base-files 4 on; /sysflfs is a link to it) or
# /sysflfs/... (base-files 1 to 3)
d() { for b in usr sysflfs; do if [ -e "$ROOT/$b/$1" ]; then echo "$ROOT/$b/$1"; return 0; fi; done; return 0; }
loader=$(d lib/ld-linux-x86-64.so.2)
libs=$(dirname "$loader")
[ -x "$loader" ] || { echo "no dynamic loader in $ROOT" >&2; exit 2; }

# ---- tool wrappers: FreeLFS's tools through its own loader ----------------------------------------
tools=$scratch/tools
rm -rf "$tools"; mkdir -p "$tools"
for t in sfdisk wipefs mkfs.btrfs mkfs.fat mkfs.ext4 mkswap mcopy mdir blkid mtype; do
	p=$(d bin/$t); [ -x "$p" ] || p=$(d sbin/$t)
	[ -x "$p" ] || { echo "missing tool $t in $ROOT" >&2; exit 2; }
	printf '#!/bin/sh\nGCONV_PATH=%s/gconv exec %s --library-path %s %s "$@"\n' "$libs" "$loader" "$libs" "$p" > "$tools/$t"
	chmod +x "$tools/$t"
done
# flboot-install hard-codes /usr/lib/flboot; point it into the root
sed "s|/usr/lib/flboot/|$(dirname "$(d lib/flboot/flboot.efi)")/|" "$(d bin/flboot-install)" > "$tools/flboot-install"
chmod +x "$tools/flboot-install"
export PATH=$tools:$PATH

# ---- the stand-in live system: the layout of base-files plus what 53-iso.sh changes ------------------
src=$scratch/live-src
if [ "${FULL:-0}" = 1 ]; then
	src=$ROOT
else
	chmod -R u+w "$src" 2> /dev/null || true
	rm -rf "$src"; mkdir -p "$src"
	S=$src/usr
	mkdir -p "$S"/{bin,sbin,lib,libexec,include,src,share/zoneinfo/Europe}
	mkdir -p "$src"/{boot,dev,proc,sys,run,tmp,drives,.snapshots,lib64,home,root,opt,srv,var}
	mkdir -p "$src"/etc/{skel/{desktop,documents,downloads,music,pictures,videos,.config},profile.d}
	ln -s usr "$src/sysflfs"; ln -s usr/bin "$src/bin"; ln -s usr/lib "$src/lib"; ln -s usr/sbin "$src/sbin"
	ln -s drives "$src/mnt"
	zi=$(dirname "$(d share/zoneinfo/UTC)")
	cp "$zi/UTC" "$S/share/zoneinfo/UTC"; cp "$zi/Europe/Berlin" "$S/share/zoneinfo/Europe/Berlin"
	ln -s /usr/share/zoneinfo/UTC "$src/etc/localtime"
	printf '#!/bin/sh\n' > "$S/bin/flsh"; chmod +x "$S/bin/flsh"
	s=$src/etc
	printf '%s\n' "root:x:0:0:root:/root:/bin/bash" "bin:x:1:1:bin:/dev/null:/usr/bin/false" \
		"nobody:x:65534:65534:Nobody:/dev/null:/usr/bin/false" "tester:x:1000:999:Tester:/home/tester:/usr/bin/flsh" > "$s/passwd"
	printf '%s\n' "root::20454:0:99999:7:::" "tester::20454:0:99999:7:::" > "$s/shadow"; chmod 600 "$s/shadow"
	printf '%s\n' "root:x:0:" "audio:x:11:tester" "video:x:12:tester" "render:x:30:tester" "netdev:x:86:" "wheel:x:97:" \
		"seat:x:98:tester" "users:x:999:" > "$s/group"
	printf '%s\n' "root:::" "audio:::tester" "video:::tester" "render:::tester" "wheel:::" "seat:::tester" "users:::" > "$s/gshadow"
	printf '%s\n' "# flpkg" "snapshots = 0                   # live system: no btrfs" "                                # flboot offers" > "$s/flpkg.conf"
	printf '%s\n' "" "FreeLFS 0.1-dev (\\l)" "Development image: log in as root, there is no password." \
		"" "Live system: log in as tester, there is no password. Nothing is" \
		"written to the disks of this computer; changes are lost at shutdown." > "$s/issue"
	printf '%s\n' "proc /proc proc nosuid 0 0" "tmpfs /tmp tmpfs defaults 0 0" > "$s/fstab"
	echo freelfs > "$s/hostname"
	printf 'XDG_DESKTOP_DIR="$HOME/desktop"\n' > "$s/skel/.config/user-dirs.dirs"
	mkdir -p "$src/home/tester"; echo hi > "$src/home/tester/note"
fi
echo test-kernel > "$scratch/vmlinuz-test"


# ---- helpers ---------------------------------------------------------------------------------------
sub() { # a tool of the root, run through its loader
	local t
	t=$(d bin/$1); [ -n "$t" ] || t=$(d sbin/$1)
	GCONV_PATH=$libs/gconv "$loader" --library-path "$libs" "$t" "${@:2}"
}
fail=0
ok()  { echo "  ok   $*"; }
bad() { echo "  FAIL $*"; fail=1; }
check() { # description, command...
	local what=$1; shift
	if "$@" > /dev/null 2>&1; then ok "$what"; else bad "$what"; fi
}

helper_run() { # image, then helper args; the request is on stdin
	local image=$1; shift
	"$helper" --image "$image" --source "$src" --kernel "$scratch/vmlinuz-test" --workdir "$scratch" "$@"
}

request() { # image, helper args; MODE, OPS, USERNAME, NOINSTALL from the environment
	local image=$1; shift
	{
		printf '%s\n' "set mode ${MODE:-whole}" "set disk $image" "set user ${USERNAME-anna}" "set fullname Anna Test" \
			"set password secret1" "set lang de_DE.UTF-8" "set keyboard de" "set tz Europe/Berlin"
		[ -z "${OPS:-}" ] || printf '%s\n' "$OPS"
		echo check
		[ "${NOINSTALL:-0}" = 1 ] || echo install
	} | helper_run "$image" "$@"
}

extract_subvol() { # image startMiB sizeMiB outdir subvolid
	local part=$scratch/root.part
	dd if="$1" of="$part" bs=1M skip="$2" count="$3" conv=sparse status=none
	rm -rf "$4"; mkdir -p "$4"
	sub btrfs restore -S -i -r "$5" "$part" "$4" > /dev/null 2>&1 || true
	rm -f "$part"
}

# ---- 1: the whole disk (8 GiB image), dry run first, then for real ------------------------------------
img=$scratch/disk.img
rm -f "$img"; truncate -s 8G "$img"
echo "== 1. whole disk, image $img"
echo "-- dry run"
MODE=whole request "$img" --dry-run | grep -E '^(PLAN|STEP|DONE|FAIL|ERR)' | cut -c1-200
cmp -s <(head -c 1048576 "$img") <(head -c 1048576 /dev/zero) && ok "dry run wrote nothing" || bad "dry run changed the image"
echo "-- for real"
MODE=whole request "$img" > "$scratch/run1.txt" 2>&1 || true
grep -E '^(STEP|DONE|FAIL|ERR)' "$scratch/run1.txt" | cut -c1-200
grep -q '^DONE' "$scratch/run1.txt" && ok "install reported DONE" || bad "install did not finish"
[ "$(sub sfdisk -d "$img" | grep -c start=)" = 2 ] && ok "GPT with two partitions" || bad "GPT with two partitions"
extract_subvol "$img" 513 7679 "$scratch/out-root" 258
S=$scratch/out-root/etc
check "user anna in passwd, uid 1000"       grep -q '^anna:x:1000:999:Anna Test:/home/anna:' "$S/passwd"
check "live user tester removed everywhere"  bash -c "! grep -q tester '$S/passwd' '$S/shadow' '$S/group' '$S/gshadow'"
check "password stored as a sha512 hash"     bash -c "grep '^anna:' '$S/shadow' | grep -q '^anna:[$]6[$]' && ! grep -q secret1 '$S/shadow'"
check "root has the same password as anna"   bash -c "[ \"\$(grep ^root: '$S/shadow' | cut -d: -f2)\" = \"\$(grep ^anna: '$S/shadow' | cut -d: -f2)\" ]"
check "anna in seat and wheel"               bash -c "grep -q '^seat:.*anna' '$S/group' && grep -q '^wheel:.*anna' '$S/group'"
check "keyboard.conf has LAYOUT=de"          grep -qx 'LAYOUT=de' "$S/keyboard.conf"
check "fstab: three btrfs UUID lines"        bash -c "[ \$(grep -c '^UUID=.*btrfs' '$S/fstab') = 3 ] && grep -q ' /home  btrfs.*subvol=@home' '$S/fstab'"
check "localtime -> Europe/Berlin"           bash -c "[ \"\$(readlink '$S/localtime')\" = /usr/share/zoneinfo/Europe/Berlin ]"
check "zoneinfo/UTC not damaged"             bash -c "[ -f '$scratch/out-root/usr/share/zoneinfo/UTC' ] && [ ! -L '$scratch/out-root/usr/share/zoneinfo/UTC' ]"
check "language script"                      grep -q 'LANG=de_DE.UTF-8' "$S/profile.d/20-lang.sh"
check "snapshots turned on again"            grep -q '^snapshots = 5' "$S/flpkg.conf"
check "live notes gone from issue"           bash -c "! grep -q -E 'Live system|Development image' '$S/issue'"
check "/boot is empty in @"                  bash -c "[ -z \"\$(ls -A '$scratch/out-root/boot')\" ]"
check "/home is empty in @ (no live home)"   bash -c "[ -d '$scratch/out-root/home' ] && [ -z \"\$(ls -A '$scratch/out-root/home')\" ]"
check "/sysflfs is a link to usr"            bash -c "[ \"\$(readlink '$scratch/out-root/sysflfs')\" = usr ]"
extract_subvol "$img" 513 7679 "$scratch/out-home" 257
check "@home: only anna's home"              bash -c "[ -d '$scratch/out-home/anna/documents' ] && [ \"\$(ls -A '$scratch/out-home')\" = anna ]"
dd if="$img" of="$scratch/esp.part" bs=1M skip=1 count=512 status=none
export MTOOLS_SKIP_CHECK=1 MTOOLSRC=/dev/null
esp_ls=$(sub mdir -i "$scratch/esp.part" -/ ::/ 2>&1 || true)
check "ESP has EFI/BOOT/BOOTX64.EFI"         grep -qi BOOTX64 <<< "$esp_ls"
check "ESP has the kernel"                   grep -qi vmlinuz <<< "$esp_ls"
rootuuid=$(sub sfdisk -d "$img" | sed -n 's/.*type=4F68[^,]*, uuid=\([^,]*\),.*/\1/p' | tr A-F a-f)
conf=$(sub mtype -i "$scratch/esp.part" ::/flboot/flboot.conf 2>&1 || true)
check "flboot.conf names the root PARTUUID"  grep -q "root=PARTUUID=$rootuuid" <<< "$conf"
rm -f "$scratch/esp.part"

# ---- 2: refusals ------------------------------------------------------------------------------------------
echo "== 2. refusals (nothing is written)"
for n in bin users root Anna "a b" "" nobody; do
	r=$(USERNAME=$n NOINSTALL=1 MODE=whole request "$img" --dry-run 2>&1 | grep '^ERR' | tail -1 || true)
	if [ -n "$r" ]; then ok "name '$n' refused: ${r#ERR	}"; else bad "name '$n' accepted"; fi
done
r=$(USERNAME=tester9 NOINSTALL=1 MODE=whole request "$img" --dry-run 2>&1 | grep '^ERR' | tail -1 || true)
[ -z "$r" ] && ok "a fine name is accepted" || bad "tester9 refused: $r"
r=$(printf "check\n" | "$helper" --image /dev/null --dry-run 2>&1 | head -1 || true)
case $r in *"regular file"*) ok "--image refuses a device: $r" ;; *) bad "--image /dev/null: $r" ;; esac

# ---- 3: next to existing partitions (free space only) ----------------------------------------------------
echo "== 3. install next to existing partitions"
img2=$scratch/disk2.img
rm -f "$img2"; truncate -s 20G "$img2"
sub sfdisk --quiet "$img2" <<SCRIPT
label: gpt
unit: sectors
first-lba: 2048

start=2048, size=2097152, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, uuid=11111111-2222-4333-8444-555555555555, name="old"
start=8390656, size=2097152, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, uuid=66666666-7777-4888-8999-aaaaaaaaaaaa, name="old2"
SCRIPT
echo "oldpart-data" | dd of="$img2" bs=1M seek=1 conv=notrunc status=none
old_dump=$(sub sfdisk -d "$img2" | grep 'name="old')
MODE=free request "$img2" > "$scratch/run3.txt" 2>&1 || true
grep -E '^(PLAN|DONE|FAIL|ERR)' "$scratch/run3.txt" | cut -c1-160
grep -q '^DONE' "$scratch/run3.txt" && ok "free-space install DONE" || bad "free-space install failed"
after=$(sub sfdisk -d "$img2")
check "old partitions unchanged" bash -c "[ \"\$(grep 'name=\"old' <<< '$after')\" = '$old_dump' ]"
check "two new partitions added" bash -c "[ \$(grep -c 'start=' <<< '$after') = 4 ]"
check "old partition data untouched" bash -c "dd if='$img2' bs=1M skip=1 count=1 status=none | grep -q oldpart-data"
echo "$after" | grep -E 'start=' | sed 's|.*img||' | cut -c1-90

# ---- 4: manual layout: ESP, root, swap and an ext4 data partition on a blank image -----------------------------
echo "== 4. manual layout"
img3=$scratch/disk3.img
rm -f "$img3"; truncate -s 14G "$img3"
OPS=$(printf '%s\n' "part newtable" \
	"part create 1048576 536870912 esp" \
	"part create 538968064 6442450944 root" \
	"part create 6981419008 1073741824 swap" \
	"part create 8055160832 2147483648 data" \
	"part format new1 vfat ESP" "part mount new1 /boot" \
	"part format new2 btrfs freelfs" "part mount new2 /" \
	"part format new3 swap" "part mount new3 swap" \
	"part format new4 ext4 stuff" "part mount new4 /drives/stuff")
MODE=manual OPS="$OPS" request "$img3" > "$scratch/run4.txt" 2>&1 || true
grep -E '^(PLAN|DONE|FAIL|ERR)' "$scratch/run4.txt" | cut -c1-160
grep -q '^DONE' "$scratch/run4.txt" && ok "manual install DONE" || bad "manual install failed"
[ "$(sub sfdisk -d "$img3" | grep -c start=)" = 4 ] && ok "four partitions" || bad "four partitions"
extract_subvol "$img3" 514 6144 "$scratch/out-root3" 258
F=$scratch/out-root3/etc/fstab
check "fstab has swap and /drives/stuff, folder made" bash -c "grep -q ' none  swap' '$F' && grep -q '/drives/stuff  ext4' '$F' && [ -d '$scratch/out-root3/drives/stuff' ]"
echo "-- bad manual layouts are refused"
tryops() { MODE=manual NOINSTALL=1 OPS="$(printf '%s\n' "$@")" request "$img3" --dry-run 2>&1 | grep '^ERR' | tail -1 || true; }
r=$(tryops 'part newtable' 'part create 1048576 536870912 esp' 'part format new1 vfat ESP' 'part mount new1 /boot')
[ -n "$r" ] && ok "no root partition refused: ${r#ERR	}" || bad "no root partition accepted"
r=$(tryops 'part newtable' 'part create 1048576 536870912 esp' 'part create 538968064 1073741824 root' 'part format new1 vfat' 'part mount new1 /boot' 'part format new2 btrfs' 'part mount new2 /')
[ -n "$r" ] && ok "root too small refused: ${r#ERR	}" || bad "small root accepted"
r=$(tryops 'part newtable' 'part create 1048576 536870912 esp' 'part create 2097152 6442450944 root')
[ -n "$r" ] && ok "overlapping partitions refused: ${r#ERR	}" || bad "overlap accepted"
r=$(tryops 'part delete /dev/sda1')
[ -n "$r" ] && ok "delete of a partition that is not there refused: ${r#ERR	}" || bad "bad delete accepted"

echo
if [ $fail = 0 ]; then echo "all checks passed"; else echo "SOME CHECKS FAILED"; exit 1; fi
