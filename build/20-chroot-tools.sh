#!/bin/bash
# M2 part 1 (LFS 13.1 chapter 7): inside the rootless chroot, create the
# directory tree and essential files, then build the last temporary tools
# (gettext, bison, perl, zlib, mpdecimal, Python, texinfo, util-linux) and
# drop the cross toolchain. Resumable: re-run after a failure.
#
# usage: 20-chroot-tools.sh   (as your normal user, after 12-temp-tools.sh)

set -euo pipefail
. "$(dirname "$0")/config.sh"
. "$(dirname "$0")/lib.sh"
chroot_env "$@"

[ -e "$FLFS_STAMPS/6.18-gcc-pass2" ] || die "run ./build/12-temp-tools.sh first"

ETC=$FLFS_TOP/config/etc

dirs_step() {
	mkdir -p /{boot,home,mnt,opt,srv}
	mkdir -p /etc/{opt,sysconfig}
	mkdir -p /lib/firmware
	mkdir -p /media/{floppy,cdrom}
	mkdir -p /usr/{,local/}{include,src}
	mkdir -p /usr/lib/locale
	mkdir -p /usr/local/{bin,lib,sbin}
	mkdir -p /usr/{,local/}share/{color,dict,doc,info,locale,man}
	mkdir -p /usr/{,local/}share/{misc,terminfo,zoneinfo}
	mkdir -p /usr/{,local/}share/man/man{1..8}
	mkdir -p /var/{cache,local,log,mail,opt,spool}
	mkdir -p /var/lib/{color,misc,locate}
	ln -sfn /run /var/run
	ln -sfn /run/lock /var/lock
	install -d -m 0750 /root
	install -d -m 1777 /tmp /var/tmp
}

files_step() {
	ln -sfn /proc/self/mounts /etc/mtab
	install -m 644 "$ETC"/{hosts,passwd,group} /etc/
	touch /var/log/{btmp,lastlog,faillog,wtmp}
	chmod 664 /var/log/lastlog     # group utmp: see config/ownership.list
	chmod 600 /var/log/btmp
}

gettext_build() {
	./configure --disable-shared
	make
	cp gettext-tools/src/{msgfmt,msgmerge,xgettext} /usr/bin
}

bison_build() {
	./configure --prefix=/usr --docdir="/usr/share/doc/bison-$(pkg_version bison)"
	make
	make install
}

# perl_configure -- the book's Configure options; ch.8 adds its own on top.
perl_configure() {
	local v
	v=$(pkg_version perl)
	v=${v%.*}
	sh Configure -des \
		-D prefix=/usr \
		-D vendorprefix=/usr \
		-D useshrplib \
		-D privlib="/usr/lib/perl5/$v/core_perl" \
		-D archlib="/usr/lib/perl5/$v/core_perl" \
		-D sitelib="/usr/lib/perl5/$v/site_perl" \
		-D sitearch="/usr/lib/perl5/$v/site_perl" \
		-D vendorlib="/usr/lib/perl5/$v/vendor_perl" \
		-D vendorarch="/usr/lib/perl5/$v/vendor_perl" \
		"$@"
}

perl_build() {
	perl_configure
	make
	make install
}

zlib_build() {
	./configure --prefix=/usr
	make
	make install
	rm -f /usr/lib/libz.a
}

mpdecimal_build() {
	./configure --prefix=/usr --disable-static \
		--docdir="/usr/share/doc/mpdecimal-$(pkg_version mpdecimal)"
	make
	make install
}

python_build() {
	./configure --prefix=/usr \
		--enable-shared \
		--without-ensurepip \
		--without-static-libpython
	make
	make install
}

texinfo_build() {
	./configure --prefix=/usr
	make
	make install
}

util_linux_build() {
	mkdir -p /var/lib/hwclock
	./configure --libdir=/usr/lib \
		--runstatedir=/run \
		--disable-chfn-chsh \
		--disable-login \
		--disable-nologin \
		--disable-su \
		--disable-setpriv \
		--disable-runuser \
		--disable-pylibmount \
		--disable-static \
		--disable-liblastlog2 \
		--without-python \
		--disable-makeinstall-chown \
		ADJTIME_PATH=/var/lib/hwclock/adjtime \
		--docdir="/usr/share/doc/util-linux-$(pkg_version util-linux)"
	make
	make install
}

cleanup_step() {
	rm -rf /usr/share/{info,man,doc}/*
	find /usr/{lib,libexec} -name '*.la' -delete
	rm -rf /tools
}

run_step  7.5-directories  dirs_step
run_step  7.6-files        files_step
build_pkg 7.7-gettext      gettext    gettext_build
build_pkg 7.8-bison        bison      bison_build
build_pkg 7.9-perl         perl       perl_build
build_pkg 7.10-zlib        zlib       zlib_build
build_pkg 7.11-mpdecimal   mpdecimal  mpdecimal_build
build_pkg 7.12-python      Python     python_build
build_pkg 7.13-texinfo     texinfo    texinfo_build
build_pkg 7.14-util-linux  util-linux util_linux_build
run_step  7.15-cleanup     cleanup_step

msg "chapter 7 done: python3 $(python3 -c 'import sys; print(sys.version.split()[0])'), perl $(perl -e 'print $^V')"
