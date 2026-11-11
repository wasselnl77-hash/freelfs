#!/bin/bash
# M2 part 3 (LFS 13.1 chapter 8, sections 33-85): the rest of the base
# system on top of the final toolchain. FreeLFS has no systemd: udev comes
# from the systemd source on its own, plus sysklogd and sysvinit (the LFS
# 12.4 sysv way), and GRUB is left out for our own bootloader. Runs in the
# rootless chroot. Resumable.
#
# usage: 31-base-system.sh   (as your normal user, after 30-base-toolchain.sh)

set -euo pipefail
. "$(dirname "$0")/config.sh"
. "$(dirname "$0")/lib.sh"
chroot_env "$@"

[ -e "$FLFS_STAMPS/8.32-toolchain-check" ] || die "run ./build/30-base-toolchain.sh first"

std_autotools() {
	./configure --prefix=/usr "$@"
	make
	run_check all make check
	make install
}

# pip_build NAME -- build and install a Python module from its source tree
pip_build() {
	pip3 wheel -w dist --no-cache-dir --no-build-isolation --no-deps "$PWD"
	pip3 install --no-index --find-links dist "$1"
}

ncurses_build() {
	./configure --prefix=/usr \
		--mandir=/usr/share/man \
		--with-shared \
		--without-debug \
		--without-normal \
		--with-cxx-shared \
		--enable-pc-files \
		--with-pkg-config-libdir=/usr/lib/pkgconfig
	make
	make DESTDIR="$PWD/dest" install
	sed -e 's/^#if.*XOPEN.*$/#if 1/' -i dest/usr/include/curses.h
	cp --remove-destination -a dest/* /
	local lib
	for lib in ncurses form panel menu; do
		ln -sf "lib${lib}w.so" "/usr/lib/lib${lib}.so"
		ln -sf "${lib}w.pc" "/usr/lib/pkgconfig/${lib}.pc"
	done
	ln -sf libncursesw.so /usr/lib/libcurses.so
	cp -R doc -T "/usr/share/doc/ncurses-$(pkg_version ncurses)"
}

sed_build() {
	./configure --prefix=/usr
	make
	make html
	run_check all make check
	make install
	install -Dm644 doc/sed.html -t "/usr/share/doc/sed-$(pkg_version sed)"
}

gettext_build() {
	std_autotools --disable-static --docdir="/usr/share/doc/gettext-$(pkg_version gettext)"
	chmod 0755 /usr/lib/preloadable_libintl.so
}

bison_build() {
	std_autotools --docdir="/usr/share/doc/bison-$(pkg_version bison)"
}

grep_build() {
	sed -i "s/echo/#echo/" src/egrep.sh
	std_autotools
}

bash_build() {
	./configure --prefix=/usr \
		--without-bash-malloc \
		--with-installed-readline \
		--docdir="/usr/share/doc/bash-$(pkg_version bash)"
	make
	# The book runs these as an unprivileged user through expect; the
	# namespace only has root, so a few tests may differ.
	run_check all env LC_ALL=C.UTF-8 make tests
	make install
}

libtool_build() {
	std_autotools
	rm -f /usr/lib/libltdl.a
}

gdbm_build() {
	std_autotools --disable-static --enable-libgdbm-compat
}

gperf_build() {
	std_autotools --docdir="/usr/share/doc/gperf-$(pkg_version gperf)"
}

expat_build() {
	local v
	v=$(pkg_version expat)
	std_autotools --disable-static --docdir="/usr/share/doc/expat-$v"
	install -m644 doc/*.{html,css} "/usr/share/doc/expat-$v"
}

inetutils_build() {
	sed -i 's/def HAVE_TERMCAP_TGETENT/ 1/' telnet/telnet.c
	std_autotools \
		--bindir=/usr/bin \
		--localstatedir=/var \
		--disable-logger \
		--disable-whois \
		--disable-rcp \
		--disable-rexec \
		--disable-rlogin \
		--disable-rsh \
		--disable-servers
	mv /usr/{,s}bin/ifconfig
}

less_build() {
	std_autotools --sysconfdir=/etc
}

perl_build() {
	local v
	v=$(pkg_version perl)
	v=${v%.*}
	export BUILD_ZLIB=False BUILD_BZIP2=0
	sh Configure -des \
		-D prefix=/usr \
		-D vendorprefix=/usr \
		-D privlib="/usr/lib/perl5/$v/core_perl" \
		-D archlib="/usr/lib/perl5/$v/core_perl" \
		-D sitelib="/usr/lib/perl5/$v/site_perl" \
		-D sitearch="/usr/lib/perl5/$v/site_perl" \
		-D vendorlib="/usr/lib/perl5/$v/vendor_perl" \
		-D vendorarch="/usr/lib/perl5/$v/vendor_perl" \
		-D man1dir=/usr/share/man/man1 \
		-D man3dir=/usr/share/man/man3 \
		-D pager="/usr/bin/less -isR" \
		-D useshrplib \
		-D usethreads
	make
	run_check all env TEST_JOBS="$FLFS_JOBS" make test_harness
	make install
}

automake_build() {
	./configure --prefix=/usr --docdir="/usr/share/doc/automake-$(pkg_version automake)"
	make
	run_check all make -j"$(( FLFS_JOBS > 4 ? FLFS_JOBS : 4 ))" check
	make install
}

openssl_build() {
	local v
	v=$(pkg_version openssl)
	./config --prefix=/usr \
		--openssldir=/etc/ssl \
		--libdir=lib \
		shared \
		zlib-dynamic
	make
	run_check all make test
	make INSTALL_LIBS= MANSUFFIX=ssl install
	rm -rf "/usr/share/doc/openssl-$v"
	mv /usr/share/doc/openssl "/usr/share/doc/openssl-$v"
	cp -fr doc/* "/usr/share/doc/openssl-$v"
}

libelf_build() {
	./configure --prefix=/usr \
		--disable-debuginfod \
		--enable-libdebuginfod=dummy
	make -C lib
	make -C libelf
	run_check all make -k check
	make -C libelf install
	install -m644 config/libelf.pc /usr/lib/pkgconfig
	rm -f /usr/lib/libelf.a
}

# libffi defaults to tuning for the build machine's CPU; use the baseline.
libffi_build() {
	local arch
	case $FLFS_ARCH in
		x86_64) arch=x86-64 ;;
		aarch64) arch=armv8-a ;;
	esac
	std_autotools --disable-static --with-gcc-arch="$arch"
}

sqlite_build() {
	./configure --prefix=/usr \
		--disable-static \
		--enable-fts{4,5} \
		CPPFLAGS="-D SQLITE_ENABLE_COLUMN_METADATA=1 \
		          -D SQLITE_ENABLE_UNLOCK_NOTIFY=1 \
		          -D SQLITE_ENABLE_DBSTAT_VTAB=1 \
		          -D SQLITE_SECURE_DELETE=1"
	make LDFLAGS.rpath=""
	make install
}

mpdecimal_build() {
	./configure --prefix=/usr \
		--disable-static \
		--docdir="/usr/share/doc/mpdecimal-$(pkg_version mpdecimal)"
	make
	run_check all make check_local
	make install
}

python_build() {
	patch -Np1 -i "$FLFS_SOURCES/Python-$(pkg_version Python)-openssl_4-1.patch"
	./configure --prefix=/usr \
		--enable-shared \
		--with-system-expat \
		--enable-optimizations \
		--without-static-libpython
	make
	run_check all make test TESTOPTS="--timeout 120"
	make install
	cat > /etc/pip.conf <<-EOF
		[global]
		root-user-action = ignore
		disable-pip-version-check = true
	EOF
}

meson_build() {
	pip_build meson
	install -Dm644 data/shell-completions/bash/meson /usr/share/bash-completion/completions/meson
	install -Dm644 data/shell-completions/zsh/_meson /usr/share/zsh/site-functions/_meson
}

kmod_build() {
	mkdir -p build
	cd build
	meson setup --prefix=/usr .. --buildtype=release -D manpages=false
	ninja
	ninja install
}

coreutils_build() {
	patch -Np1 -i "$FLFS_SOURCES/coreutils-$(pkg_version coreutils)-i18n-1.patch"
	autoreconf -f
	automake -af
	FORCE_UNSAFE_CONFIGURE=1 ./configure --prefix=/usr
	make
	# The book's non-root half of the suite needs a second uid; run the
	# root half only.
	run_check all make check-root
	make install
	mv /usr/bin/chroot /usr/sbin
	mv /usr/share/man/man1/chroot.1 /usr/share/man/man8/chroot.8
	sed -i 's/"1"/"8"/' /usr/share/man/man8/chroot.8
}

findutils_build() {
	std_autotools --localstatedir=/var/lib/locate
}

groff_build() {
	PAGE=A4 ./configure --prefix=/usr
	make -j1
	run_check all make check
	make install
}

iproute2_build() {
	sed -i /ARPD/d Makefile
	rm -f man/man8/arpd.8
	make NETNS_RUN_DIR=/run/netns
	make SBINDIR=/usr/sbin install
	install -Dm644 COPYING README* -t "/usr/share/doc/iproute2-$(pkg_version iproute2)"
}

kbd_build() {
	local v
	v=$(pkg_version kbd)
	patch -Np1 -i "$FLFS_SOURCES/kbd-$v-backspace-1.patch"
	sed -i '/RESIZECONS_PROGS=/s/yes/no/' configure
	sed -i 's/resizecons.8 //' docs/man/man8/Makefile.in
	std_autotools --disable-vlock
	cp -R docs/doc -T "/usr/share/doc/kbd-$v"
}

tar_build() {
	patch -Np1 -i "$FLFS_SOURCES/tar-$(pkg_version tar)-acl_fix-1.patch"
	FORCE_UNSAFE_CONFIGURE=1 ./configure --prefix=/usr
	make
	run_check all make check
	make install
	make -C doc install-html docdir="/usr/share/doc/tar-$(pkg_version tar)"
}

texinfo_build() {
	std_autotools
	make TEXMF=/usr/share/texmf install-tex
	local f
	cd /usr/share/info
	rm -f dir
	for f in *; do
		install-info "$f" dir 2>/dev/null || true
	done
}

vim_build() {
	local v
	v=$(pkg_version vim)
	echo '#define SYS_VIMRC_FILE "/etc/vimrc"' >> src/feature.h
	./configure --prefix=/usr
	make
	sed '/test_plugin_glvs/d' -i src/testdir/Make_all.mak
	run_check all env TERM=xterm-256color LANG=en_US.UTF-8 make -j1 test
	make install
	ln -sf vim /usr/bin/vi
	local l
	for l in /usr/share/man/{,*/}man1/vim.1; do
		ln -sf vim.1 "$(dirname "$l")/vi.1"
	done
	v=${v%.*}
	ln -sfn "../vim/vim${v//./}/doc" "/usr/share/doc/vim-$(pkg_version vim)"
	cat > /etc/vimrc <<-"EOF"
		" Begin /etc/vimrc

		" Ensure defaults are set before customizing settings, not after
		source $VIMRUNTIME/defaults.vim
		let skip_defaults_vim=1

		set nocompatible
		set backspace=2
		set mouse=
		syntax on
		if (&term == "xterm") || (&term == "putty")
		  set background=dark
		endif

		" End /etc/vimrc
	EOF
}

# udev on its own from the systemd source, as the LFS 12.4 sysv book does.
# Builds only udevadm, the hwdb tool, libudev, the rules and the helpers.
udev_build() {
	sed -i '/systemd-sysctl/s/^/#/' rules.d/99-systemd.rules.in
	sed -e '/NETWORK_DIRS/s/systemd/udev/' \
		-i src/libsystemd/sd-network/network-util.h
	mkdir -p build
	cd build
	meson setup .. \
		--prefix=/usr \
		--buildtype=release \
		-D mode=release \
		-D dev-kvm-mode=0660 \
		-D link-udev-shared=false \
		-D logind=false \
		-D vconsole=false

	local helpers
	helpers=$(grep "'name' :" ../src/udev/meson.build |
		awk '{print $3}' | tr -d ",'" | grep -v 'udevadm')
	# shellcheck disable=SC2046
	ninja udevadm systemd-hwdb \
		$(ninja -n | grep -Eo '(src/(lib)?udev|rules.d|hwdb.d)/[^ ]*') \
		$(realpath libudev.so --relative-to .) \
		$helpers

	install -m755 -d {/usr/lib,/etc}/udev/{hwdb.d,rules.d,network}
	install -m755 -d /usr/{lib,share}/pkgconfig
	install -m755 udevadm /usr/bin/
	install -m755 systemd-hwdb /usr/bin/udev-hwdb
	ln -sfn ../bin/udevadm /usr/sbin/udevd
	cp -a libudev.so{,*[0-9]} /usr/lib/
	install -m644 ../src/libudev/libudev.h /usr/include/
	install -m644 src/libudev/*.pc /usr/lib/pkgconfig/
	install -m644 src/udev/*.pc /usr/share/pkgconfig/
	install -m644 ../src/udev/udev.conf /etc/udev/
	install -m644 rules.d/* ../rules.d/README /usr/lib/udev/rules.d/
	install -m644 $(find ../rules.d/*.rules -not -name '*power-switch*') \
		/usr/lib/udev/rules.d/
	install -m644 hwdb.d/* ../hwdb.d/{*.hwdb,README} /usr/lib/udev/hwdb.d/
	install -m755 $helpers /usr/lib/udev
	install -m644 ../network/99-default.link /usr/lib/udev/network

	# LFS's extra rules and helpers (cdrom symlinks, persistent net names)
	local lfs
	lfs=udev-lfs-$(pkg_version udev-lfs)
	tar -xf "$(src_tarball udev-lfs)"
	make -f "$lfs/Makefile.lfs" install

	udev-hwdb update
}

man_db_build() {
	std_autotools \
		--docdir="/usr/share/doc/man-db-$(pkg_version man-db)" \
		--sysconfdir=/etc \
		--disable-setuid \
		--enable-cache-owner=bin \
		--with-browser=/usr/bin/lynx \
		--with-vgrind=/usr/bin/vgrind \
		--with-grap=/usr/bin/grap \
		--with-systemdtmpfilesdir= \
		--with-systemdsystemunitdir=
}

procps_build() {
	./configure --prefix=/usr \
		--docdir="/usr/share/doc/procps-ng-$(pkg_version procps-ng)" \
		--disable-static \
		--disable-kill \
		--enable-watch8bit
	make
	run_check all make check
	make install
}

util_linux_build() {
	./configure --bindir=/usr/bin \
		--libdir=/usr/lib \
		--runstatedir=/run \
		--sbindir=/usr/sbin \
		--disable-chfn-chsh \
		--disable-login \
		--disable-nologin \
		--disable-su \
		--disable-setpriv \
		--disable-runuser \
		--disable-pylibmount \
		--disable-liblastlog2 \
		--disable-static \
		--without-python \
		--without-systemd \
		--without-systemdsystemunitdir \
		--disable-makeinstall-chown \
		ADJTIME_PATH=/var/lib/hwclock/adjtime \
		--docdir="/usr/share/doc/util-linux-$(pkg_version util-linux)"
	make
	touch /etc/fstab
	run_check all make -k check
	make install
	# wall and write are setgid tty; chown cannot do that here.
	need_owner /usr/bin/wall 0 5 2755
	[ ! -e /usr/bin/write ] || need_owner /usr/bin/write 0 5 2755
}

e2fsprogs_build() {
	mkdir -p build
	cd build
	../configure --prefix=/usr \
		--sysconfdir=/etc \
		--enable-elf-shlibs \
		--disable-libblkid \
		--disable-libuuid \
		--disable-uuidd \
		--disable-fsck
	make
	run_check all make check
	make install
	rm -f /usr/lib/{libcom_err,libe2p,libext2fs,libss}.a
	gunzip -f /usr/share/info/libext2fs.info.gz
	install-info --dir-file=/usr/share/info/dir /usr/share/info/libext2fs.info
	makeinfo -o doc/com_err.info ../lib/et/com_err.texinfo
	install -m644 doc/com_err.info /usr/share/info
	install-info --dir-file=/usr/share/info/dir /usr/share/info/com_err.info
	sed 's/metadata_csum_seed,//' -i /etc/mke2fs.conf
}

sysklogd_build() {
	./configure --prefix=/usr \
		--sysconfdir=/etc \
		--runstatedir=/run \
		--without-logger \
		--disable-static \
		--docdir="/usr/share/doc/sysklogd-$(pkg_version sysklogd)"
	make
	make install
	cat > /etc/syslog.conf <<-"EOF"
		# Begin /etc/syslog.conf

		auth,authpriv.* -/var/log/auth.log
		*.*;auth,authpriv.none -/var/log/sys.log
		daemon.* -/var/log/daemon.log
		kern.* -/var/log/kern.log
		mail.* -/var/log/mail.log
		user.* -/var/log/user.log
		*.emerg *

		# Do not open any internet ports.
		secure_mode 2

		# End /etc/syslog.conf
	EOF
}

sysvinit_build() {
	patch -Np1 -i "$FLFS_SOURCES/sysvinit-$(pkg_version sysvinit)-consolidated-1.patch"
	make
	make install
}

build_pkg 8.33-ncurses      ncurses     ncurses_build
build_pkg 8.34-sed          sed         sed_build
build_pkg 8.35-psmisc       psmisc      std_autotools
build_pkg 8.36-gettext      gettext     gettext_build
build_pkg 8.37-bison        bison       bison_build
build_pkg 8.38-grep         grep        grep_build
build_pkg 8.39-bash         bash        bash_build
build_pkg 8.40-libtool      libtool     libtool_build
build_pkg 8.41-gdbm         gdbm        gdbm_build
build_pkg 8.42-gperf        gperf       gperf_build
build_pkg 8.43-expat        expat       expat_build
build_pkg 8.44-inetutils    inetutils   inetutils_build
build_pkg 8.45-less         less        less_build
build_pkg 8.46-perl         perl        perl_build
build_pkg 8.47-autoconf     autoconf    std_autotools
build_pkg 8.48-automake     automake    automake_build
build_pkg 8.49-openssl      openssl     openssl_build
build_pkg 8.50-libelf       elfutils    libelf_build
build_pkg 8.51-libffi       libffi      libffi_build
build_pkg 8.52-sqlite       sqlite-autoconf sqlite_build
build_pkg 8.53-mpdecimal    mpdecimal   mpdecimal_build
build_pkg 8.54-python       Python      python_build
build_pkg 8.55-flit-core    flit_core   pip_build flit_core
build_pkg 8.56-packaging    packaging   pip_build packaging
build_pkg 8.57-wheel        wheel       pip_build wheel
build_pkg 8.58-setuptools   setuptools  pip_build setuptools
build_pkg 8.59-meson        meson       meson_build
build_pkg 8.60-kmod         kmod        kmod_build
build_pkg 8.61-coreutils    coreutils   coreutils_build
build_pkg 8.62-diffutils    diffutils   std_autotools
build_pkg 8.63-findutils    findutils   findutils_build
build_pkg 8.64-groff        groff       groff_build
build_pkg 8.66-gzip         gzip        std_autotools
build_pkg 8.67-iproute2     iproute2    iproute2_build
build_pkg 8.68-kbd          kbd         kbd_build
build_pkg 8.69-libpipeline  libpipeline std_autotools
build_pkg 8.70-make         make        std_autotools
build_pkg 8.71-patch        patch       std_autotools
build_pkg 8.72-tar          tar         tar_build
build_pkg 8.73-texinfo      texinfo     texinfo_build
build_pkg 8.74-vim          vim         vim_build
build_pkg 8.75-markupsafe   markupsafe  pip_build Markupsafe
build_pkg 8.76-jinja2       jinja2      pip_build Jinja2
build_pkg 8.77-udev         systemd     udev_build
build_pkg 8.79-man-db       man-db      man_db_build
build_pkg 8.80-procps-ng    procps-ng   procps_build
build_pkg 8.81-util-linux   util-linux  util_linux_build
build_pkg 8.82-e2fsprogs    e2fsprogs   e2fsprogs_build
build_pkg 8.83-sysklogd     sysklogd    sysklogd_build
build_pkg 8.84-sysvinit     sysvinit    sysvinit_build

msg "base system built"
