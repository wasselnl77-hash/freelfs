#!/bin/bash
# M1 part 2 (LFS 13.1 chapter 6): cross-compile the temporary tools into the
# target root, then binutils and gcc that run *on* the target. After this the
# target can be entered with chroot. Resumable: re-run after a failure.
#
# usage: 12-temp-tools.sh   (as your normal user, after 11-cross-toolchain.sh)

set -euo pipefail
. "$(dirname "$0")/config.sh"
. "$(dirname "$0")/lib.sh"
clean_env "$@"

[ -e "$FLFS_STAMPS/5.6-libstdcxx" ] || die "run ./build/11-cross-toolchain.sh first"

# Most chapter 6 packages are plain autotools cross builds.
cross_autotools() {
	./configure --prefix=/usr \
		--host="$FLFS_TGT" \
		--build="$(build-aux/config.guess)" \
		"$@"
	make
	make DESTDIR="$FLFS" install
}

m4_build() {
	cat > "$FLFS/usr/share/config.site" <<-EOF
		ac_cv_func_posix_spawn_file_actions_addchdir=yes
		ac_cv_func_posix_spawn_file_actions_addfchdir=yes
	EOF
	cross_autotools
}

ncurses_build() {
	mkdir build
	pushd build
	../configure --prefix="$FLFS_TOOLS" AWK=gawk
	make -C include
	make -C progs tic
	install progs/tic "$FLFS_TOOLS/bin"
	popd
	./configure --prefix=/usr \
		--host="$FLFS_TGT" \
		--build="$(./config.guess)" \
		--mandir=/usr/share/man \
		--with-manpage-format=normal \
		--with-shared \
		--without-normal \
		--with-cxx-shared \
		--without-debug \
		--without-ada \
		--disable-stripping \
		AWK=gawk
	make
	make DESTDIR="$FLFS" install
	ln -sf libncursesw.so "$FLFS/usr/lib/libncurses.so"
	sed -e 's/^#if.*XOPEN.*$/#if 1/' -i "$FLFS/usr/include/curses.h"
}

bash_build() {
	./configure --prefix=/usr \
		--build="$(sh support/config.guess)" \
		--host="$FLFS_TGT" \
		--without-bash-malloc \
		--docdir="/usr/share/doc/bash-$(pkg_version bash)"
	make
	make DESTDIR="$FLFS" install
	ln -sf bash "$FLFS/bin/sh"
}

coreutils_build() {
	cross_autotools --enable-install-program=hostname
	mv "$FLFS/usr/bin/chroot" "$FLFS/usr/sbin"
	mkdir -p "$FLFS/usr/share/man/man8"
	mv "$FLFS/usr/share/man/man1/chroot.1" "$FLFS/usr/share/man/man8/chroot.8"
	sed -i 's/"1"/"8"/' "$FLFS/usr/share/man/man8/chroot.8"
}

diffutils_build() {
	cross_autotools gl_cv_func_strcasecmp_works=yes
}

file_build() {
	# A host build of `file` is needed to compile the magic database.
	mkdir build
	pushd build
	../configure --disable-bzlib --disable-libseccomp --disable-xzlib --disable-zlib
	make
	popd
	./configure --prefix=/usr --host="$FLFS_TGT" --build="$(./config.guess)"
	make FILE_COMPILE="$(pwd)/build/src/file"
	make DESTDIR="$FLFS" install
	rm -f "$FLFS/usr/lib/libmagic.la"
}

findutils_build() {
	cross_autotools --localstatedir=/var/lib/locate
}

gawk_build() {
	sed -i 's/extras//' Makefile.in
	cross_autotools
}

gzip_build() {
	./configure --prefix=/usr --host="$FLFS_TGT"
	make
	make DESTDIR="$FLFS" install
}

xz_build() {
	cross_autotools --disable-static --docdir="/usr/share/doc/xz-$(pkg_version xz)"
	rm -f "$FLFS/usr/lib/liblzma.la"
}

binutils_pass2() {
	sed '6031s/$add_dir//' -i ltmain.sh
	mkdir build && cd build
	../configure \
		--prefix=/usr \
		--build="$(../config.guess)" \
		--host="$FLFS_TGT" \
		--disable-nls \
		--enable-shared \
		--enable-gprofng=no \
		--disable-werror \
		--enable-64-bit-bfd \
		--enable-new-dtags \
		--enable-default-hash-style=gnu
	make
	make DESTDIR="$FLFS" install
	rm -f "$FLFS"/usr/lib/lib{bfd,ctf,ctf-nobfd,opcodes,sframe}.{a,la}
}

gcc_pass2() {
	local p
	for p in mpfr gmp mpc; do
		tar -xf "$(src_tarball $p)"
		mv "$p"-*/ "$p"
	done
	case $FLFS_ARCH in
	x86_64) sed -e '/m64=/s/lib64/lib/' -i.orig gcc/config/i386/t-linux64 ;;
	esac
	mkdir build && cd build
	../configure \
		--build="$(../config.guess)" \
		--host="$FLFS_TGT" \
		--target="$FLFS_TGT" \
		--prefix=/usr \
		--with-build-sysroot="$FLFS" \
		--enable-default-pie \
		--enable-default-ssp \
		--disable-fixincludes \
		--disable-nls \
		--disable-multilib \
		--disable-libatomic \
		--disable-libgomp \
		--disable-libquadmath \
		--disable-libsanitizer \
		--disable-libssp \
		--disable-libvtv \
		--enable-languages=c,c++ \
		CXX_FOR_TARGET="$FLFS_TGT-gcc -nostdinc++" \
		LDFLAGS_FOR_TARGET="-L$PWD/$FLFS_TGT/libgcc" \
		target_configargs=gcc_cv_target_thread_file=posix
	make
	make DESTDIR="$FLFS" install
	ln -sf gcc "$FLFS/usr/bin/cc"
}

build_pkg 6.2-m4             m4        m4_build
build_pkg 6.3-ncurses        ncurses   ncurses_build
build_pkg 6.4-bash           bash      bash_build
build_pkg 6.5-coreutils      coreutils coreutils_build
build_pkg 6.6-diffutils      diffutils diffutils_build
build_pkg 6.7-file           file      file_build
build_pkg 6.8-findutils      findutils findutils_build
build_pkg 6.9-gawk           gawk      gawk_build
build_pkg 6.10-grep          grep      cross_autotools
build_pkg 6.11-gzip          gzip      gzip_build
build_pkg 6.12-make          make      cross_autotools
build_pkg 6.13-patch         patch     cross_autotools
build_pkg 6.14-sed           sed       cross_autotools
build_pkg 6.15-tar           tar       cross_autotools
build_pkg 6.16-xz            xz        xz_build
build_pkg 6.17-binutils-pass2 binutils binutils_pass2
build_pkg 6.18-gcc-pass2     gcc       gcc_pass2

# Smoke test: run target binaries through the target's own loader and libc
# (the host glibc is older, so they cannot run directly yet).
ld=$(ls "$FLFS"/usr/lib/ld-linux*.so.* | head -n1)
for bin in bash ls gcc make; do
	"$ld" --library-path "$FLFS/usr/lib" "$FLFS/usr/bin/$bin" --version | head -n1
done
msg "temporary tools complete; next is M2 (chroot)"
