#!/bin/bash
# M7: the disk image for a USB stick (the real laptop). The same image as
# 50-image.sh, with two differences: it is 8 GB by default, and it has a user
# "tester" with an empty password. The keyboard is not preset: the first boot
# asks for it, as on a fresh install.
#
# This only makes the file, $FLFS_WORK/freelfs-usb.img. It never writes to a
# device: it prints the dd command, with a placeholder, for you to run.
#
# usage: 52-usb.sh   (as your normal user, after 60-packages.sh;
#                     FLFS_IMG_SIZE changes the 8G)

set -euo pipefail

# what the image step does with the staged root, before the file system is made
if [ "${1:-}" = --image-hook ]; then
	stage=$2 root=$3 w=$FLFS_USB_WORK
	die() { echo "52-usb: $*" >&2; exit 1; }
	settings=$(readlink -f "$stage/@/etc") data=$(readlink -f "$stage/@/var")
	[ ! -e "$settings/keyboard.conf" ] || die "the root has a keyboard preset (/etc/keyboard.conf)"
	! grep -q '^tester:' "$settings/passwd" || die "the root already has a user tester"
	! grep -q '^[^:]*:[^:]*:1000:' "$settings/passwd" || die "uid 1000 is taken"
	for f in passwd shadow group gshadow; do
		cp -p "$settings/$f" "$w/$f"
		chmod u+w "$w/$f"
	done
	grp=$(awk -F: '$1 == "users" {print $3}' "$settings/group")
	[ -n "$grp" ] || die "no group users"
	echo "tester:x:1000:$grp:Tester:/usr/tester:/usr/bin/flsh" >> "$w/passwd"
	echo "tester::20454:0:99999:7:::" >> "$w/shadow"
	# the groups a new user gets from /etc/default/useradd
	python3 -I - "$w/group" "$w/gshadow" <<'PY'
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
	for f in passwd shadow group gshadow; do
		chmod --reference="$settings/$f" "$w/$f"
	done
	# the home is /usr/tester: /usr is the @home subvolume. The build runs as one
	# uid, so the files are made as root; flpkg fix-owners (rc.sysinit) sets
	# the real owner at the first boot, from rows added to a copy of its database.
	mkdir "$stage/@home/tester"
	cp -a "$settings/skel/." "$stage/@home/tester/"
	chmod 700 "$stage/@home/tester"
	cp "$data/lib/flpkg/db.sqlite" "$w/db.sqlite"
	chmod u+w "$w/db.sqlite"
	python3 -I - "$w/db.sqlite" "$stage/@home" "$grp" <<'PY'
import os, sqlite3, sys
db, home, gid = sys.argv[1], sys.argv[2], int(sys.argv[3])
c = sqlite3.connect(db)
top = os.path.join(home, "tester")
for dirpath, dirs, files in os.walk(top):
    for n in dirs + files + ["."]:
        p = os.path.normpath(os.path.join(dirpath, n))
        mode = 0o700 if p == top else os.lstat(p).st_mode & 0o7777
        rel = "usr/" + os.path.relpath(p, home)
        c.execute("INSERT OR REPLACE INTO pending_owners VALUES(?,?,?,?)", (rel, 1000, gid, mode))
c.commit()
c.close()
PY
	for f in passwd shadow group gshadow; do
		mount --bind "$w/$f" "$settings/$f"
	done
	mount --bind "$w/db.sqlite" "$data/lib/flpkg/db.sqlite"
	exit 0
fi

# A USB stick image is 8G unless FLFS_IMG_SIZE says otherwise; config.sh
# would default to 80G.
export FLFS_IMG_SIZE=${FLFS_IMG_SIZE:-8G}
. "$(dirname "$0")/config.sh"
. "$(dirname "$0")/lib.sh"

[ "$EUID" -ne 0 ] || die "run this as your normal user, not root"
export FLFS_IMG=$FLFS_WORK/freelfs-usb.img
export FLFS_USB_WORK=$FLFS_WORK/usb-work
rm -rf "$FLFS_USB_WORK"
mkdir -p "$FLFS_USB_WORK"
trap 'rm -rf "$FLFS_USB_WORK"' EXIT
FLFS_IMAGE_HOOK=$(readlink -f "$0") "$(dirname "$0")/50-image.sh"

msg "USB image: $FLFS_IMG ($FLFS_IMG_SIZE), user tester without a password, keyboard asked at first boot"
cat <<MSG

To write it to a stick, plug the stick in and find its device with lsblk (the
whole disk, like /dev/sdX, not a partition like /dev/sdX1; check the size).
Everything on the stick is lost. Then, as root:

    sudo dd if=$FLFS_IMG of=/dev/sdX bs=4M conv=fsync status=progress

This script has not written to any device. The stick needs at least
$FLFS_IMG_SIZE. In the laptop's firmware, turn Secure Boot off (flboot is not
signed) and pick the stick in the boot menu.
MSG
