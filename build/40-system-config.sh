#!/bin/bash
# M2 part 5 (LFS chapters 8.84-8.85 and 9): strip and clean the base system,
# then install FreeLFS's own configuration from config/etc: the inittab,
# rc.conf and rc boot scripts, fstab, profile and friends. Runs in the
# rootless chroot. Resumable.
#
# usage: 40-system-config.sh   (as your normal user, after 32-base-extra.sh)

set -euo pipefail
. "$(dirname "$0")/config.sh"
. "$(dirname "$0")/lib.sh"
chroot_env "$@"

[ -e "$FLFS_STAMPS/x.3-dhcpcd" ] || die "run ./build/32-base-extra.sh first"

ETC=$FLFS_TOP/config/etc

# Strip debug symbols. The loader, libc and a few other libraries keep
# theirs in separate .dbg files, as in the book (valgrind and gdb need them).
# Every file is stripped as a copy that then replaces the original, so the
# programs running this step (bash, find, strip) are not hit; hard-linked
# files are rewritten in place to keep their links.
strip_step() {
	local f lib keep tmp=/tmp/strip.$$
	cd /usr/lib
	keep="$(ls ld-linux*[^g]) libc.so.6 libthread_db.so.1 libquadmath.so.0.0.0
		libitm.so.1.0.0 libatomic.so.1.2.0 $(ls libstdc++.so.6.0.*[0-9])"
	for lib in $keep; do
		objcopy --only-keep-debug --compress-debug-sections=zstd "$lib" "$lib.dbg"
		cp "$lib" "$tmp"
		strip --strip-unneeded "$tmp"
		objcopy --add-gnu-debuglink="$lib.dbg" "$tmp"
		install -m755 "$tmp" "$lib"
	done
	keep=$(echo $keep)

	find /usr/lib /usr/libexec /usr/bin /usr/sbin -type f ! -name '*.dbg' \
		\( -name '*.so*' -o -name '*.a' -o -perm -u+x \) -print0 |
		while IFS= read -r -d '' f; do
			case " $keep " in *" ${f#/usr/lib/} "*) continue ;; esac
			cp -p "$f" "$tmp"
			strip --strip-unneeded "$tmp" 2>/dev/null || continue # not ELF
			! cmp -s "$f" "$tmp" || continue
			if [ "$(stat -c %h "$f")" -gt 1 ]; then
				cat "$tmp" > "$f"
			else
				mv -f "$tmp" "$f"
			fi
		done
	rm -f "$tmp"
}

cleanup_step() {
	rm -rf /tmp/{*,.[!.]*}
	find /usr/lib /usr/libexec -name '*.la' -delete
	find /usr -depth -name "$FLFS_ARCH-freelfs-linux-gnu*" -exec rm -rf {} +
}

etc_step() {
	install -m644 "$ETC"/{fstab,hostname,hosts,inputrc,issue,os-release,profile,rc.conf,shells} /etc/
	install -m644 "$ETC/inittab" /etc/inittab
	install -d /etc/rc.d/init.d /etc/profile.d
	install -m644 "$ETC/rc.d/functions" /etc/rc.d/
	install -m755 "$ETC"/rc.d/{rc,rc.sysinit,rc.shutdown} /etc/rc.d/
	install -m755 "$ETC"/rc.d/init.d/* /etc/rc.d/init.d/
	install -Dm644 "$FLFS_TOP/config/ownership.list" /usr/share/freelfs/ownership.list
	touch /etc/sysctl.conf
	ldconfig
}

# Development images only: root has no password, so the console login works
# straight away. The installer (M9) sets real passwords.
dev_login_step() {
	passwd -d root
	sed -i '/^FreeLFS/a Development image: log in as root, there is no password.' /etc/issue
}

run_step  9.1-strip       strip_step
run_step  9.2-cleanup     cleanup_step
run_step  9.3-etc         etc_step
run_step  9.4-dev-login   dev_login_step

msg "system configured: $(. /etc/os-release && echo "$PRETTY_NAME")"
