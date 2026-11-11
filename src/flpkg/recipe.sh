#!/bin/bash
# flpkg's recipe helper. flpkg runs it; it is not meant to be run by hand.
#
#   recipe.sh dump RECIPE             print the recipe's variables and functions
#   recipe.sh run RECIPE FUNC [OUT]   run FUNC; with OUT, write the variables
#                                     there afterwards (a split package's
#                                     package_NAME may override them)
#
# Output records are "key<TAB>value<NUL>", one per scalar and one per array
# element. flpkg sets srcdir, pkgdir, startdir, worksrc and the build
# environment before calling "run".
# SPDX-License-Identifier: MIT

scalars=(pkgbase version release summary url worksrc build_style)
arrays=(pkgname arch license groups source sha256 depends makedepends
	checkdepends provides conflicts replaces sysusers sysgroups owner options)

dump() {
	local v x f
	for v in "${scalars[@]}"; do
		[[ -v $v ]] && printf '%s\t%s\0' "$v" "${!v}"
	done
	for v in "${arrays[@]}"; do
		local -n ref=$v
		for x in "${ref[@]}"; do
			printf '%s\t%s\0' "$v" "$x"
		done
		unset -n ref
	done
	for f in $(compgen -A function); do
		case $f in
			prepare | build | check | package | package_*) printf 'function\t%s\0' "$f" ;;
		esac
	done
}

# Standard functions for a build_style, for those the recipe leaves out.
# configure_args=() adds options to the configure (or meson setup) step;
# pyname= is the Python module to install (default: pkgbase).
default_functions() {
	case ${build_style:-} in
		"") ;;
		autotools)
			declare -F build > /dev/null || build() {
				./configure --prefix=/usr "${configure_args[@]}"
				make
			}
			declare -F check > /dev/null || check() { make check; }
			declare -F package > /dev/null || package() { make DESTDIR="$pkgdir" install; }
			;;
		meson)
			declare -F build > /dev/null || build() {
				meson setup build --prefix=/usr --buildtype=release "${configure_args[@]}"
				ninja -C build ${NINJAJOBS:+-j$NINJAJOBS}
			}
			declare -F check > /dev/null || check() { ninja -C build test; }
			declare -F package > /dev/null || package() { DESTDIR="$pkgdir" ninja -C build install; }
			;;
		cmake)
			declare -F build > /dev/null || build() {
				cmake -S . -B build -G Ninja -D CMAKE_INSTALL_PREFIX=/usr \
					-D CMAKE_BUILD_TYPE=Release -D CMAKE_SKIP_INSTALL_RPATH=ON "${configure_args[@]}"
				ninja -C build ${NINJAJOBS:+-j$NINJAJOBS}
			}
			declare -F check > /dev/null || check() { ninja -C build test; }
			declare -F package > /dev/null || package() { DESTDIR="$pkgdir" ninja -C build install; }
			;;
		python)
			declare -F build > /dev/null || build() {
				pip3 wheel -w dist --no-cache-dir --no-build-isolation --no-deps "$PWD"
			}
			declare -F package > /dev/null || package() {
				pip3 install --root="$pkgdir" --ignore-installed --no-deps --no-index \
					--no-warn-script-location --find-links dist "${pyname:-$pkgbase}"
			}
			;;
		*)
			echo "flpkg: unknown build_style '$build_style'" >&2
			exit 1
			;;
	esac
}

[ $# -ge 2 ] || { echo "usage: recipe.sh dump|run RECIPE [FUNC [OUT]]" >&2; exit 2; }
cmd=$1 recipe=$2
configure_args=()

# flpkg hands these over in the environment; the recipe sees them, but the
# programs it runs must not (vim's configure, for one, reads $srcdir).
export -n srcdir pkgdir startdir worksrc pkgname 2> /dev/null

# shellcheck source=/dev/null
. "$recipe" || { echo "flpkg: cannot read $recipe" >&2; exit 1; }
: "${pkgbase:=${pkgname[0]}}"
default_functions

case $cmd in
	dump)
		dump
		;;
	run)
		func=$3 out=${4:-}
		declare -F "$func" > /dev/null || { echo "flpkg: recipe has no $func()" >&2; exit 1; }
		# Functions start in the unpacked source tree when there is one.
		if [ -n "${worksrc:-}" ] && [ -d "$srcdir/$worksrc" ]; then
			cd "$srcdir/$worksrc" || exit 1
		else
			cd "$srcdir" || exit 1
		fi
		set -e -o pipefail
		"$func"
		set +e
		if [ -n "$out" ]; then
			dump > "$out" || exit 1
		fi
		;;
	*)
		echo "flpkg: unknown recipe.sh command $cmd" >&2
		exit 2
		;;
esac
