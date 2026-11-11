# FreeLFS build helpers. Sourced by build scripts after config.sh.

FLFS_TOP=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

msg()  { printf '\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# download URL DEST -- fetch to DEST.part, then move into place on success
download() {
	local url=$1 dest=$2
	curl -fL -# --retry 5 --retry-all-errors --connect-timeout 20 \
		-o "$dest.part" "$url" || {
		rm -f "$dest.part"
		return 1
	}
	mv "$dest.part" "$dest"
}

# lock_entries -- print "sha256 file url" for every entry in sources.lock
lock_entries() {
	grep -Ev '^\s*(#|$)' "$FLFS_TOP/sources.lock"
}

# --- package build harness, used by the 1x-*.sh stage scripts ---------------

# clean_env ARGS -- re-exec the calling stage script in a minimal environment
# (the env -i trick LFS uses for its build user), so nothing from the host
# shell leaks into the build. Refuses to run as root: a typo in a build step
# must not be able to overwrite the host system.
clean_env() {
	[ "$EUID" -ne 0 ] || die "do not build as root; run this as your normal user"
	[ -z "${FLFS_IN_CHROOT:-}" ] || die "$(basename "$0") runs on the host, not in the chroot"
	if [ -z "${FLFS_CLEAN:-}" ]; then
		exec env -i FLFS_CLEAN=1 HOME="$HOME" TERM="${TERM:-dumb}" \
			FLFS="$FLFS" FLFS_WORK="$FLFS_WORK" FLFS_ARCH="$FLFS_ARCH" \
			FLFS_JOBS="$FLFS_JOBS" FLFS_CHECK="$FLFS_CHECK" \
			/bin/bash --noprofile --norc "$0" "$@"
	fi
	set +h
	umask 022
	PATH=/usr/bin
	[ -L /bin ] || PATH=/bin:$PATH
	PATH=$FLFS_TOOLS/bin:$PATH
	export PATH CONFIG_SITE=$FLFS/usr/share/config.site

	[ -w "$FLFS_STAMPS" ] || die "target not prepared (run ./build/01-prepare.sh)"
	mkdir -p "$FLFS_LOGS"
}

# src_tarball NAME -- path of NAME's source tarball: NAME-<version>.tar.*,
# also matching the odd ones out (expect5.45.4, tcl8.6.18-src, tzdata2026c,
# btrfs-progs-v7.3)
src_tarball() {
	local f
	f=$(lock_entries | awk -v n="$1" \
		'$2 ~ "^" n "-?v?[0-9][^-]*(-src)?\\.(tar\\.(gz|xz|bz2|zst)|tgz)$" { print $2; exit }')
	[ -n "$f" ] || die "no source tarball for '$1' in sources.lock"
	echo "$FLFS_SOURCES/$f"
}

# pkg_version NAME -- version of NAME as pinned in sources.lock
pkg_version() {
	local f
	f=$(basename "$(src_tarball "$1")")
	f=${f#"$1"}
	f=${f#-}
	f=${f#v}
	f=${f%.tgz}
	f=${f%.tar.*}
	echo "${f%-src}"
}

# chroot_env ARGS -- re-exec the calling stage script inside the target root
# through build/chroot.sh (a user namespace: no sudo). Inside, $FLFS is empty,
# so "$FLFS/usr" is /usr, and the project tree is at /.bootstrap/freelfs.
chroot_env() {
	if [ -z "${FLFS_IN_CHROOT:-}" ]; then
		[ "$EUID" -ne 0 ] || die "do not build as root; run this as your normal user"
		exec "$FLFS_TOP/build/chroot.sh" \
			/bin/bash --noprofile --norc "/.bootstrap/freelfs/build/$(basename "$0")" "$@"
	fi
	# Never trust FLFS_IN_CHROOT alone: on the host, $FLFS= would mean /.
	[ "$EUID" -eq 0 ] && [ -d /.bootstrap/stamps ] && [ -d /sources ] ||
		die "FLFS_IN_CHROOT is set, but this is not the FreeLFS chroot"
	set +h
	umask 022
	# Only root exists here, so tar must not try to restore the uids
	# recorded in a tarball (that fails with EINVAL). This covers every tar
	# call, including the ones inside build steps.
	export TAR_OPTIONS=--no-same-owner
	mkdir -p "$FLFS_LOGS"
}

# need_owner PATH UID GID MODE -- (chroot) PATH needs an owner the namespace
# cannot give it. Fails unless config/ownership.list has the line, so the
# image step never misses one; sets MODE now.
need_owner() {
	grep -Eq "^$1[[:space:]]+$2[[:space:]]+$3[[:space:]]+$4([[:space:]]|$)" \
		"$FLFS_TOP/config/ownership.list" ||
		die "add '$1 $2 $3 $4' to config/ownership.list"
	chmod "$4" "$1"
}

# run_check LEVEL CMD... -- run a test suite if FLFS_CHECK covers LEVEL (core
# or all). A failure is logged, not fatal: some tests cannot pass in a
# single-uid namespace. ./build/test-report.sh lists the failures.
run_check() {
	local level=$1
	shift
	case "$FLFS_CHECK:$level" in
		all:* | core:core) ;;
		*)
			echo "==> tests skipped (FLFS_CHECK=$FLFS_CHECK)"
			return 0
			;;
	esac
	echo "==> running tests: $*"
	"$@" || echo "==> TESTS FAILED (exit $?): $*"
}

# _run STEP DIR FUNC [ARGS...] -- run FUNC in DIR with errexit, logging to
# $FLFS_LOGS/STEP.log, and stamp STEP as done on success.
_run() {
	local step=$1 dir=$2 log=$FLFS_LOGS/$1.log rc start=$SECONDS
	shift 2

	# The subshell must not run in a tested context (if/||), or bash
	# silently ignores its set -e.
	set +e
	(
		set -euo pipefail
		cd "$dir"
		"$@"
	) > "$log" 2>&1
	rc=$?
	set -e

	if [ "$rc" -ne 0 ]; then
		tail -n 25 "$log" >&2
		die "$step failed (exit $rc); full log: $log"
	fi
	touch "$FLFS_STAMPS/$step"
	msg "$step: ok ($(( (SECONDS - start) / 60 ))m$(( (SECONDS - start) % 60 ))s)"
}

# run_step STEP FUNC [ARGS...] -- a stamped, logged step that is not a
# package build (creating directories, writing config files, ...). FUNC runs
# in /.
run_step() {
	local step=$1
	shift
	if [ -e "$FLFS_STAMPS/$step" ]; then
		msg "$step: already done"
		return 0
	fi
	msg "$step"
	_run "$step" / "$@"
}

# build_pkg STEP PKG FUNC [ARGS...] -- unpack PKG, run FUNC ARGS inside its
# source tree with errexit, log to $FLFS_LOGS/STEP.log, and stamp STEP as
# done so re-running a stage script resumes where it stopped.
build_pkg() {
	local step=$1 pkg=$2 tarball dir
	shift 2

	if [ -e "$FLFS_STAMPS/$step" ]; then
		msg "$step: already built"
		return 0
	fi
	tarball=$(src_tarball "$pkg")
	msg "$step: building $(basename "$tarball")"

	rm -rf "$FLFS_BUILD"
	mkdir -p "$FLFS_BUILD"
	tar --no-same-owner -xf "$tarball" -C "$FLFS_BUILD"
	dir=$(find "$FLFS_BUILD" -mindepth 1 -maxdepth 1 -type d | head -n1)
	_run "$step" "$dir" "$@"
	rm -rf "$FLFS_BUILD"
}
