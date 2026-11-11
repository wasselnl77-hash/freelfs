#!/bin/bash
# M1 part 1 (LFS 13.1 chapter 5): the cross toolchain.
# binutils + gcc for $FLFS_TGT go into $FLFS/tools; Linux API headers, glibc
# and libstdc++ go into the target root. Resumable: re-run after a failure.
#
# usage: 11-cross-toolchain.sh   (as your normal user, after 01-prepare.sh + 02-fetch.sh)

set -euo pipefail
. "$(dirname "$0")/config.sh"
. "$(dirname "$0")/lib.sh"
clean_env "$@"

binutils_pass1() {
	mkdir build && cd build
	../configure --prefix="$FLFS_TOOLS" \
		--with-sysroot="$FLFS" \
		--target="$FLFS_TGT" \
		--disable-nls \
		--enable-gprofng=no \
		--disable-werror \
		--enable-new-dtags \
		--enable-default-hash-style=gnu
	make
	make install
}

# Shared by gcc pass 1/2 and libstdc++: in-tree math libs, lib64 -> lib.
gcc_prepare() {
	local p
	for p in mpfr gmp mpc; do
		tar -xf "$(src_tarball $p)"
		mv "$p"-*/ "$p"
	done
	case $FLFS_ARCH in
	x86_64) sed -e '/m64=/s/lib64/lib/' -i.orig gcc/config/i386/t-linux64 ;;
	esac
}

gcc_pass1() {
	gcc_prepare
	mkdir build && cd build
	../configure \
		--target="$FLFS_TGT" \
		--prefix="$FLFS_TOOLS" \
		--with-glibc-version="$(pkg_version glibc)" \
		--with-sysroot="$FLFS" \
		--with-newlib \
		--without-headers \
		--enable-default-pie \
		--enable-default-ssp \
		--disable-fixincludes \
		--disable-nls \
		--disable-shared \
		--disable-multilib \
		--disable-threads \
		--disable-libatomic \
		--disable-libgomp \
		--disable-libquadmath \
		--disable-libssp \
		--disable-libvtv \
		--disable-libstdcxx \
		--enable-languages=c,c++
	make
	make install
	cat ../gcc/{limitx,glimits,limity}.h > \
		"$("$FLFS_TGT-gcc" -print-file-name=include)/limits.h"
}

linux_headers() {
	make mrproper
	make headers
	find usr/include -type f ! -name '*.h' -delete
	cp -r usr/include "$FLFS/usr"
}

glibc_build() {
	case $FLFS_ARCH in
	x86_64)
		ln -sf ../lib/ld-linux-x86-64.so.2 "$FLFS/lib64"
		ln -sf ../lib/ld-linux-x86-64.so.2 "$FLFS/lib64/ld-lsb-x86-64.so.3"
		;;
	esac
	patch -Np1 -i "$FLFS_SOURCES/glibc-fhs-1.patch"
	patch -Np1 -i "$FLFS_SOURCES/glibc-$(pkg_version glibc)-upstream_fixes-1.patch"
	mkdir build && cd build
	echo "rootsbindir=/usr/sbin" > configparms
	../configure \
		--prefix=/usr \
		--host="$FLFS_TGT" \
		--build="$(../scripts/config.guess)" \
		--disable-nscd \
		libc_cv_slibdir=/usr/lib \
		--enable-kernel=5.10
	make
	make DESTDIR="$FLFS" install
	sed '/RTLDLIST=/s@/usr@@g' -i "$FLFS/usr/bin/ldd"
}

# The book's sanity check: the cross compiler must link against the target's
# glibc, with the target's dynamic loader.
toolchain_check() {
	local interp
	case $FLFS_ARCH in
	x86_64) interp=/lib64/ld-linux-x86-64.so.2 ;;
	aarch64) interp=/lib/ld-linux-aarch64.so.1 ;;
	esac
	echo 'int main(){}' | "$FLFS_TGT-gcc" -x c - -v -Wl,--verbose > dummy.log 2>&1
	"$FLFS_TGT-readelf" -l a.out | grep -F "[Requesting program interpreter: $interp]"
	grep -E -o "$FLFS/lib.*/S?crt[1in].*succeeded" dummy.log
	grep -E "/lib.*/libc.so.6 " dummy.log | grep -F "$FLFS/"
	grep -F "found ld-linux" dummy.log
	rm a.out dummy.log
}

libstdcxx() {
	mkdir build && cd build
	../libstdc++-v3/configure \
		--host="$FLFS_TGT" \
		--build="$(../config.guess)" \
		CXX="$FLFS_TGT-gcc" \
		--prefix=/usr \
		--disable-multilib \
		--disable-nls \
		--disable-libstdcxx-pch \
		--with-gxx-include-dir="/tools/$FLFS_TGT/include/c++/$(pkg_version gcc)"
	make
	make DESTDIR="$FLFS" install
	rm -f "$FLFS"/usr/lib/lib{stdc++{,exp,fs},supc++}.la
}

build_pkg 5.2-binutils-pass1  binutils binutils_pass1
build_pkg 5.3-gcc-pass1       gcc      gcc_pass1
build_pkg 5.4-linux-headers   linux    linux_headers
build_pkg 5.5-glibc           glibc    glibc_build
build_pkg 5.5-toolchain-check glibc    toolchain_check
build_pkg 5.6-libstdcxx       gcc      libstdcxx

msg "cross toolchain complete: $FLFS_TOOLS/bin/$FLFS_TGT-gcc"
