#!/bin/bash
# Integration tests for flpkg build: split packages, owners, links,
# reproducibility and the checks that must stop a build.
# usage: tests/test-build.sh   (from src/flpkg, after make)
# SPDX-License-Identifier: MIT

set -u
cd "$(dirname "$0")/.."
export FLPKG_LIBDIR=$PWD
flpkg=$PWD/flpkg
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
conf=$work/flpkg.conf
printf 'builddir = %s/build\ncache = %s/cache\nsignatures = never\n' "$work" "$work" > "$conf"
fails=0

ok() { printf '  %-50s ok\n' "$1"; }
bad() { printf '  %-50s FAIL\n' "$1"; fails=$((fails + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

build() { # DIR OUT -- build quietly, return flpkg's status
	"$flpkg" --config "$conf" build -o "$2" "tests/recipes/$1" > "$work/$1.log" 2>&1
}

mkdir -p "$work/a" "$work/b"
check "split recipe builds" 'build split "$work/a"'
check "two packages come out" '[ -f "$work/a/hello-1.0-2-any.flpkg" ] && [ -f "$work/a/hello-doc-1.0-2-any.flpkg" ]'
files=$(tar --zstd -xOf "$work/a/hello-1.0-2-any.flpkg" .FILES 2> /dev/null)
check "owner override applied" 'grep -q "^f 2755 0 5 .* usr/bin/hello$" <<< "$files"'
check "hard link keeps its target's attributes" 'grep -q "^h 2755 0 5 - usr/bin/hello usr/bin/hello2$" <<< "$files"'
check "symlink recorded" 'grep -q "^l 0777 0 0 - hello usr/bin/hi$" <<< "$files"'
info=$(tar --zstd -xOf "$work/a/hello-doc-1.0-2-any.flpkg" .PKGINFO 2> /dev/null)
check "split package overrides its summary" 'grep -q "^summary = Documentation for hello$" <<< "$info"'
check "archive starts with .PKGINFO, .FILES" '[ "$(tar --zstd -tf "$work/a/hello-1.0-2-any.flpkg" | head -2 | tr "\n" " ")" = ".PKGINFO .FILES " ]'
check "flpkg reads its own package" '"$flpkg" info -f "$work/a/hello-1.0-2-any.flpkg" | grep -q "^Version *: 1.0-2$"'
check "second build" 'build split "$work/b"'
check "builds are byte-identical" 'cmp -s "$work/a/hello-1.0-2-any.flpkg" "$work/b/hello-1.0-2-any.flpkg" && cmp -s "$work/a/hello-doc-1.0-2-any.flpkg" "$work/b/hello-doc-1.0-2-any.flpkg"'
check "checksum mismatch stops the build" '! build bad-sha "$work/a" && grep -q "sha256 mismatch" "$work/bad-sha.log"'
check "/usr/local stops the build" '! build bad-path "$work/a" && grep -q "must not use" "$work/bad-path.log"'
check "an empty package stops the build" '! build empty "$work/a" && grep -q "is empty" "$work/empty.log"'
check "no package from failed builds" '! ls "$work/a" | grep -q "^bad-\|^empty-"'
check "build_style=autotools needs no functions" 'build styled "$work/a"'
check "\$srcdir and \$pkgdir stay out of child processes" 'build envcheck "$work/a"'
check "configure_args reached configure" '[ "$(tar --zstd -xOf "$work/a/styled-1.0-1-any.flpkg" usr/bin/styled 2> /dev/null | tail -1)" = "echo moin" ]'

echo "build tests: $fails failed"
[ "$fails" -eq 0 ]
