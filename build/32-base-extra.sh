#!/bin/bash
# M2 part 4: FreeLFS packages beyond the LFS book that the base system needs:
# btrfs-progs for the root file system (with lzo) and dhcpcd for networking.
# Versions and build steps follow BLFS. Runs in the rootless chroot.
#
# usage: 32-base-extra.sh   (as your normal user, after 31-base-system.sh)

set -euo pipefail
. "$(dirname "$0")/config.sh"
. "$(dirname "$0")/lib.sh"
chroot_env "$@"

[ -e "$FLFS_STAMPS/8.84-sysvinit" ] || die "run ./build/31-base-system.sh first"

lzo_build() {
	./configure --prefix=/usr \
		--enable-shared \
		--disable-static \
		--docdir="/usr/share/doc/lzo-$(pkg_version lzo)"
	make
	run_check all make check
	make install
}

btrfs_progs_build() {
	local i
	./configure --prefix=/usr \
		--disable-static \
		--disable-documentation
	make
	make install
	for i in 5 8; do
		install -m644 Documentation/*.$i "/usr/share/man/man$i" 2>/dev/null || true
	done
}

dhcpcd_build() {
	install -m700 -d /var/lib/dhcpcd
	grep -q '^dhcpcd:' /etc/group || groupadd -g 52 dhcpcd
	grep -q '^dhcpcd:' /etc/passwd ||
		useradd -c 'dhcpcd PrivSep' -d /var/lib/dhcpcd -g dhcpcd \
			-s /bin/false -u 52 dhcpcd
	need_owner /var/lib/dhcpcd 52 52 0700
	./configure --prefix=/usr \
		--sysconfdir=/etc \
		--libexecdir=/usr/lib/dhcpcd \
		--dbdir=/var/lib/dhcpcd \
		--runstatedir=/run \
		--privsepuser=dhcpcd
	make
	make install
}

build_pkg x.1-lzo          lzo         lzo_build
build_pkg x.2-btrfs-progs  btrfs-progs btrfs_progs_build
build_pkg x.3-dhcpcd       dhcpcd      dhcpcd_build

msg "extras done: $(btrfs --version | head -n1), $(dhcpcd --version | head -n1)"
