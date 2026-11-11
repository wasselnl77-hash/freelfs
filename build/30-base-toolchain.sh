#!/bin/bash
# M2 part 2 (LFS 13.1 chapter 8, sections 3-32): the final system's C
# library, compression libraries and toolchain, ending with the final GCC and
# its sanity check. Runs in the rootless chroot. Resumable.
#
# Test suites follow FLFS_CHECK (config.sh): "core" runs the critical ones
# (glibc, binutils, gmp, mpfr, mpc), "all" runs every one, "none" skips them.
#
# usage: 30-base-toolchain.sh   (as your normal user, after 20-chroot-tools.sh)

set -euo pipefail
. "$(dirname "$0")/config.sh"
. "$(dirname "$0")/lib.sh"
chroot_env "$@"

[ -e "$FLFS_STAMPS/7.15-cleanup" ] || die "run ./build/20-chroot-tools.sh first"

ETC=$FLFS_TOP/config/etc

# std_autotools ARGS -- configure --prefix=/usr ARGS, make, test, install
std_autotools() {
	./configure --prefix=/usr "$@"
	make
	run_check all make check
	make install
}

man_pages_build() {
	rm -f man3/crypt*
	make -R GIT=false prefix=/usr install
}

iana_etc_build() {
	cp services protocols /etc
}

# Locales the test suites of later packages need; the installer adds more.
LOCALES="C:UTF-8:C.UTF-8 cs_CZ:UTF-8:cs_CZ.UTF-8 de_DE:ISO-8859-1:de_DE
	de_DE@euro:ISO-8859-15:de_DE@euro de_DE:UTF-8:de_DE.UTF-8
	el_GR:ISO-8859-7:el_GR en_GB:ISO-8859-1:en_GB en_GB:UTF-8:en_GB.UTF-8
	en_HK:ISO-8859-1:en_HK en_PH:ISO-8859-1:en_PH en_US:ISO-8859-1:en_US
	en_US:UTF-8:en_US.UTF-8 es_ES:ISO-8859-15:es_ES@euro
	es_MX:ISO-8859-1:es_MX fa_IR:UTF-8:fa_IR fr_FR:ISO-8859-1:fr_FR
	fr_FR@euro:ISO-8859-15:fr_FR@euro fr_FR:UTF-8:fr_FR.UTF-8
	is_IS:ISO-8859-1:is_IS is_IS:UTF-8:is_IS.UTF-8 it_IT:ISO-8859-1:it_IT
	it_IT:ISO-8859-15:it_IT@euro it_IT:UTF-8:it_IT.UTF-8 ja_JP:EUC-JP:ja_JP
	ja_JP:UTF-8:ja_JP.UTF-8 nl_NL@euro:ISO-8859-15:nl_NL@euro
	ru_RU:KOI8-R:ru_RU.KOI8-R ru_RU:UTF-8:ru_RU.UTF-8 se_NO:UTF-8:se_NO.UTF-8
	ta_IN:UTF-8:ta_IN.UTF-8 tr_TR:UTF-8:tr_TR.UTF-8
	zh_CN:GB18030:zh_CN.GB18030 zh_HK:BIG5-HKSCS:zh_HK.BIG5-HKSCS
	zh_TW:UTF-8:zh_TW.UTF-8"

glibc_build() {
	patch -Np1 -i "$FLFS_SOURCES/glibc-fhs-1.patch"
	patch -Np1 -i "$FLFS_SOURCES/glibc-$(pkg_version glibc)-upstream_fixes-1.patch"
	mkdir build
	cd build
	../configure --prefix=/usr \
		--disable-werror \
		--disable-nscd \
		libc_cv_slibdir=/usr/lib \
		--enable-stack-protector=strong \
		--enable-kernel=5.10
	make
	run_check core make check
	find . -name '*.out' -exec grep -l "Timed out" {} + || true

	touch /etc/ld.so.conf
	sed '/test-installation/s@$(PERL)@echo not running@' -i ../Makefile
	make install
	sed '/RTLDLIST=/s@/usr@@g' -i /usr/bin/ldd

	local l i f n
	for l in $LOCALES; do
		IFS=: read -r i f n <<< "$l"
		localedef -i "$i" -f "$f" "$n"
	done

	# No systemd: plain files and DNS.
	cat > /etc/nsswitch.conf <<-EOF
		# Begin /etc/nsswitch.conf

		passwd: files
		group: files
		shadow: files

		hosts: files dns
		networks: files

		protocols: files
		services: files
		ethers: files
		rpc: files

		# End /etc/nsswitch.conf
	EOF

	mkdir tz
	tar -xf "$(src_tarball tzdata)" -C tz
	local zi=/usr/share/zoneinfo tz
	mkdir -p $zi/{posix,right}
	for tz in etcetera southamerica northamerica europe africa antarctica \
		asia australasia backward; do
		zic -L /dev/null -d $zi tz/$tz
		zic -L /dev/null -d $zi/posix tz/$tz
		zic -L tz/leapseconds -d $zi/right tz/$tz
	done
	cp tz/{zone.tab,zone1970.tab,iso3166.tab} $zi
	zic -d $zi -p America/New_York
	ln -sfn /usr/share/zoneinfo/UTC /etc/localtime   # the installer sets the real zone

	cat > /etc/ld.so.conf <<-EOF
		# Begin /etc/ld.so.conf
		/usr/local/lib
		/opt/lib

		# Add an include directory
		include /etc/ld.so.conf.d/*.conf

	EOF
	mkdir -p /etc/ld.so.conf.d
}

zlib_build() {
	std_autotools
	rm -f /usr/lib/libz.a
}

bzip2_build() {
	local v
	v=$(pkg_version bzip2)
	patch -Np1 -i "$FLFS_SOURCES/bzip2-$v-install_docs-1.patch"
	sed -i 's@\(ln -s -f \)$(PREFIX)/bin/@\1@' Makefile
	sed -i "s@(PREFIX)/man@(PREFIX)/share/man@g" Makefile
	make -f Makefile-libbz2_so
	make clean
	make
	make PREFIX=/usr install
	cp -a libbz2.so.* /usr/lib
	ln -sf "libbz2.so.$v" /usr/lib/libbz2.so
	ln -sf "libbz2.so.$v" /usr/lib/libbz2.so.1
	cp bzip2-shared /usr/bin/bzip2
	ln -sf bzip2 /usr/bin/bzcat
	ln -sf bzip2 /usr/bin/bunzip2
	rm -f /usr/lib/libbz2.a
}

xz_build() {
	std_autotools --disable-static --docdir="/usr/share/doc/xz-$(pkg_version xz)"
}

lz4_build() {
	make BUILD_STATIC=no PREFIX=/usr
	run_check all make -j1 check
	make BUILD_STATIC=no PREFIX=/usr install
}

zstd_build() {
	make prefix=/usr
	run_check all make check
	make prefix=/usr install
	rm -f /usr/lib/libzstd.a
}

readline_build() {
	local v
	v=$(pkg_version readline)
	sed -i '/MV.*old/d' Makefile.in
	sed -i '/{OLDSUFF}/c:' support/shlib-install
	sed -i 's/-Wl,-rpath,[^ ]*//' support/shobj-conf
	sed -e '270a\
     else\
       chars_avail = 1;' \
		-e '288i\   result = -1;' \
		-i.orig input.c
	./configure --prefix=/usr \
		--disable-static \
		--with-curses \
		--docdir="/usr/share/doc/readline-$v"
	make SHLIB_LIBS="-lncursesw"
	make install
	install -m644 doc/*.{ps,pdf,html,dvi} "/usr/share/doc/readline-$v"
}

pcre2_build() {
	std_autotools --docdir="/usr/share/doc/pcre2-$(pkg_version pcre2)" \
		--enable-unicode \
		--enable-jit \
		--enable-pcre2-16 \
		--enable-pcre2-32 \
		--enable-pcre2grep-libz \
		--enable-pcre2grep-libbz2 \
		--enable-pcre2test-libreadline \
		--disable-static
}

bc_build() {
	CC='gcc -std=c99' ./configure --prefix=/usr -G -O3 -r
	make
	run_check all make test
	make install
}

flex_build() {
	std_autotools --disable-static --docdir="/usr/share/doc/flex-$(pkg_version flex)"
	ln -sf flex /usr/bin/lex
	ln -sf flex.1 /usr/share/man/man1/lex.1
}

tcl_build() {
	local src v tdbc itcl
	src=$(pwd)
	v=$(pkg_version tcl)
	v=${v%.*}
	cd unix
	./configure --prefix=/usr --mandir=/usr/share/man --disable-rpath
	make

	tdbc=$(basename "$src"/pkgs/tdbc1*)
	itcl=$(basename "$src"/pkgs/itcl4*)
	sed -e "s|$src/unix|/usr/lib|" \
		-e "s|$src|/usr/include|" \
		-i tclConfig.sh
	sed -e "s|$src/unix/pkgs/$tdbc|/usr/lib/$tdbc|" \
		-e "s|$src/pkgs/$tdbc/generic|/usr/include|" \
		-e "s|$src/pkgs/$tdbc/library|/usr/lib/tcl$v|" \
		-e "s|$src/pkgs/$tdbc|/usr/include|" \
		-i "pkgs/$tdbc/tdbcConfig.sh"
	sed -e "s|$src/unix/pkgs/$itcl|/usr/lib/$itcl|" \
		-e "s|$src/pkgs/$itcl/generic|/usr/include|" \
		-e "s|$src/pkgs/$itcl|/usr/include|" \
		-i "pkgs/$itcl/itclConfig.sh"

	run_check all env LC_ALL=C.UTF-8 make test
	make install
	chmod 644 "/usr/lib/libtclstub$v.a"
	chmod u+w "/usr/lib/libtcl$v.so"
	make install-private-headers
	ln -sf "tclsh$v" /usr/bin/tclsh
	mv /usr/share/man/man3/{Thread,Tcl_Thread}.3
}

expect_build() {
	local v
	v=$(pkg_version expect)
	# Expect needs working ptys: this fails early if the chroot has none.
	python3 -c 'from pty import spawn; spawn(["echo", "ok"])'
	patch -Np1 -i "$FLFS_SOURCES/expect-$v-gcc15-1.patch"
	./configure --prefix=/usr \
		--with-tcl=/usr/lib \
		--enable-shared \
		--disable-rpath \
		--mandir=/usr/share/man \
		--with-tclinclude=/usr/include
	make
	run_check all make test
	make install
	ln -sf "expect$v/libexpect$v.so" /usr/lib
}

dejagnu_build() {
	local v
	v=$(pkg_version dejagnu)
	mkdir build
	cd build
	../configure --prefix=/usr
	makeinfo --html --no-split -o doc/dejagnu.html ../doc/dejagnu.texi
	makeinfo --plaintext -o doc/dejagnu.txt ../doc/dejagnu.texi
	run_check all make check
	make install
	install -d -m755 "/usr/share/doc/dejagnu-$v"
	install -m644 doc/dejagnu.{html,txt} "/usr/share/doc/dejagnu-$v"
}

ninja_build() {
	sed -i '/int Guess/a \
  int   j = 0;\
  char* jobs = getenv( "NINJAJOBS" );\
  if ( jobs != NULL ) j = atoi( jobs );\
  if ( j > 0 ) return j;\
' src/ninja.cc
	python3 configure.py --bootstrap --verbose
	install -m755 ninja /usr/bin/
	install -Dm644 misc/bash-completion /usr/share/bash-completion/completions/ninja
	install -Dm644 misc/zsh-completion /usr/share/zsh/site-functions/_ninja
}

pkgconf_build() {
	local mv
	mv=$(pkg_version meson)
	tar -xf "$(src_tarball meson)"
	mkdir build
	cd build
	python3 "../meson-$mv/meson.py" setup --prefix=/usr --buildtype=release ..
	ninja
	run_check all ninja test
	ninja install
	rm -rf "/usr/share/doc/pkgconf-$(pkg_version pkgconf)"
	mv /usr/share/doc/pkgconf{,-"$(pkg_version pkgconf)"}
	ln -sf pkgconf /usr/bin/pkg-config
	ln -sf pkgconf.1 /usr/share/man/man1/pkg-config.1
}

binutils_build() {
	mkdir build
	cd build
	../configure --prefix=/usr \
		--sysconfdir=/etc \
		--enable-ld=default \
		--enable-plugins \
		--enable-shared \
		--disable-werror \
		--enable-64-bit-bfd \
		--enable-new-dtags \
		--with-system-zlib \
		--with-lib-path=/usr/lib \
		--enable-default-hash-style=gnu
	make tooldir=/usr
	run_check core make -k check
	find . -name '*.log' -exec grep '^FAIL:' {} + || true
	make tooldir=/usr install
	rm -rf /usr/lib/lib{bfd,ctf,ctf-nobfd,gprofng,opcodes,sframe}.a \
		/usr/share/doc/gprofng/
}

# GMP tunes itself to the build machine's CPU by default. A distro must run
# on any CPU of the architecture, so on x86_64 build every variant and pick
# one at run time (--enable-fat), as other distros do.
gmp_build() {
	local cpu=()
	case $FLFS_ARCH in
		x86_64) cpu=(--build=x86_64-pc-linux-gnu --enable-fat) ;;
		aarch64) cpu=(--build=aarch64-unknown-linux-gnu) ;;
	esac
	sed -i '/long long t1;/,+1s/()/(...)/' configure
	./configure --prefix=/usr \
		"${cpu[@]}" \
		--enable-cxx \
		--disable-static \
		--docdir="/usr/share/doc/gmp-$(pkg_version gmp)"
	make
	make html
	run_check core make check
	echo "==> gmp tests passed: $(find . -name '*.log' -exec cat {} + | grep -c ^PASS || true)"
	make install
	make install-html
}

mpfr_build() {
	./configure --prefix=/usr \
		--disable-static \
		--enable-thread-safe \
		--docdir="/usr/share/doc/mpfr-$(pkg_version mpfr)"
	make
	make html
	run_check core make check
	make install
	make install-html
}

mpc_build() {
	./configure --prefix=/usr \
		--disable-static \
		--docdir="/usr/share/doc/mpc-$(pkg_version mpc)"
	make
	make html
	run_check core make check
	make install
	make install-html
}

attr_build() {
	std_autotools --disable-static --sysconfdir=/etc \
		--docdir="/usr/share/doc/attr-$(pkg_version attr)"
}

acl_build() {
	std_autotools --disable-static --docdir="/usr/share/doc/acl-$(pkg_version acl)"
}

libcap_build() {
	sed -i '/install -m.*STA/d' libcap/Makefile
	make prefix=/usr lib=lib
	run_check all make test
	make prefix=/usr lib=lib install
}

libxcrypt_build() {
	sed -i '/strchr/s/const//' lib/crypt-{sm3,gost}-yescrypt.c
	std_autotools \
		--enable-hashes=strong,glibc \
		--enable-obsolete-api=no \
		--disable-static \
		--disable-failure-tokens
}

shadow_build() {
	find man -name Makefile.in -exec sed -i 's/getspnam\.3 / /' {} \;
	find man -name Makefile.in -exec sed -i 's/passwd\.5 / /' {} \;
	sed -e 's:#ENCRYPT_METHOD SHA512:ENCRYPT_METHOD YESCRYPT:' \
		-e 's:/var/spool/mail:/var/mail:' \
		-e '/PATH=/{s@/sbin:@@;s@/bin:@@}' \
		-i etc/login.defs
	touch /usr/bin/passwd
	./configure --sysconfdir=/etc \
		--disable-static \
		--with-{b,yes}crypt \
		--without-libbsd \
		--disable-logind \
		--with-group-name-max-length=32
	make
	make exec_prefix=/usr install
	make -C man install-man
	pwconv
	grpconv
	mkdir -p /etc/default
	useradd -D --gid 999
	sed -i '/MAIL/s/yes/no/' /etc/default/useradd
	touch /etc/sub{u,g}id
}

gawk_build() {
	local v
	v=$(pkg_version gawk)
	sed -i 's/extras//' Makefile.in
	./configure --prefix=/usr
	make
	run_check all make check
	rm -f "/usr/bin/gawk-$v"
	make install
	ln -sf gawk.1 /usr/share/man/man1/awk.1
	install -Dm644 doc/{awkforai.txt,*.{eps,pdf,jpg}} -t "/usr/share/doc/gawk-$v"
}

gcc_build() {
	case $FLFS_ARCH in
		x86_64) sed -e '/m64=/s/lib64/lib/' -i.orig gcc/config/i386/t-linux64 ;;
	esac
	mkdir build
	cd build
	../configure --prefix=/usr \
		LD=ld \
		--enable-languages=c,c++ \
		--enable-default-pie \
		--enable-default-ssp \
		--enable-host-pie \
		--enable-targets=all \
		--disable-multilib \
		--disable-bootstrap \
		--disable-fixincludes \
		--with-system-zlib
	make
	ulimit -s -H unlimited 2>/dev/null || ulimit -s "$(ulimit -H -s)"
	run_check all make -k check
	[ "$FLFS_CHECK" != all ] || ../contrib/test_summary -t | grep -A7 Summ || true
	make install
	ln -sfr /usr/bin/cpp /usr/lib
	ln -sf gcc.1 /usr/share/man/man1/cc.1
	ln -sfr "$(gcc -print-prog-name=liblto_plugin.so)" /usr/lib/bfd-plugins/
	mkdir -p /usr/share/gdb/auto-load/usr/lib
	mv /usr/lib/*gdb.py /usr/share/gdb/auto-load/usr/lib
}

# The book's sanity check of the final toolchain: programs must use the
# final loader, startup files, headers and C library in /usr.
toolchain_check() {
	cd "$(mktemp -d)"
	echo 'int main(){}' | cc -x c - -v -Wl,--verbose &> dummy.log
	readelf -l a.out | grep -q ': /lib64/ld-linux-x86-64.so.2\]\|: /lib/ld-linux-aarch64.so.1\]'
	grep -E -o '/usr/lib.*/S?crt[1in].*succeeded' dummy.log
	[ "$(grep -E -c '/usr/lib.*/S?crt[1in].*succeeded' dummy.log)" -eq 3 ]
	grep -B4 '^ /usr/include' dummy.log
	grep 'SEARCH.*/usr/lib' dummy.log | sed 's|; |\n|g'
	grep -q '/lib.*/libc.so.6 ' dummy.log
	grep 'found' dummy.log
	grep -q 'found ld-linux.*at /usr/lib/ld-linux' dummy.log
	./a.out
	cd /
}

build_pkg 8.3-man-pages     man-pages  man_pages_build
build_pkg 8.4-iana-etc      iana-etc   iana_etc_build
build_pkg 8.5-glibc         glibc      glibc_build
build_pkg 8.6-zlib          zlib       zlib_build
build_pkg 8.7-bzip2         bzip2      bzip2_build
build_pkg 8.8-xz            xz         xz_build
build_pkg 8.9-lz4           lz4        lz4_build
build_pkg 8.10-zstd         zstd       zstd_build
build_pkg 8.11-file         file       std_autotools
build_pkg 8.12-readline     readline   readline_build
build_pkg 8.13-pcre2        pcre2      pcre2_build
build_pkg 8.14-m4           m4         std_autotools
build_pkg 8.15-bc           bc         bc_build
build_pkg 8.16-flex         flex       flex_build
build_pkg 8.17-tcl          tcl        tcl_build
build_pkg 8.18-expect       expect     expect_build
build_pkg 8.19-dejagnu      dejagnu    dejagnu_build
build_pkg 8.20-ninja        ninja      ninja_build
build_pkg 8.21-pkgconf      pkgconf    pkgconf_build
build_pkg 8.22-binutils     binutils   binutils_build
build_pkg 8.23-gmp          gmp        gmp_build
build_pkg 8.24-mpfr         mpfr       mpfr_build
build_pkg 8.25-mpc          mpc        mpc_build
build_pkg 8.26-attr         attr       attr_build
build_pkg 8.27-acl          acl        acl_build
build_pkg 8.28-libcap       libcap     libcap_build
build_pkg 8.29-libxcrypt    libxcrypt  libxcrypt_build
build_pkg 8.30-shadow       shadow     shadow_build
build_pkg 8.31-gawk         gawk       gawk_build
build_pkg 8.32-gcc          gcc        gcc_build
run_step  8.32-toolchain-check         toolchain_check

msg "final toolchain ready: $(gcc --version | head -n1), $(ld --version | head -n1)"
