#!/bin/bash
# M3: reproducibility check. Builds every recipe a second time, from
# scratch, and compares each package with the one 60-packages.sh made. For a
# package that differs, lists the files whose contents differ.
# Resumable: each second build is a stamped step.
#
# usage: 61-repro-check.sh   (as your normal user, after 60-packages.sh)

set -euo pipefail
. "$(dirname "$0")/config.sh"
. "$(dirname "$0")/lib.sh"
chroot_env "$@"

[ -e "$FLFS_STAMPS/p.9-repo" ] || die "run ./build/60-packages.sh first"

WORK=/.bootstrap/pkg
OUT2=$WORK/out2
export FLPKG_LIBDIR=$WORK/flpkg-src
flpkg() { "$FLPKG_LIBDIR/flpkg" --config "$WORK/flpkg.conf" "$@"; }

rebuild() {
	mkdir -p "$OUT2"
	rm -f "$OUT2/$1"-[0-9]*.flpkg
	flpkg build --nocheck --sources /sources -o "$OUT2" "$FLFS_TOP/recipes/$1"
}

for r in "$FLFS_TOP"/recipes/*/; do
	r=$(basename "$r")
	run_step "repro-$r" rebuild "$r"
done

same=0 differ=0
for f in "$WORK"/out/*.flpkg; do
	g=$OUT2/${f##*/}
	if cmp -s "$f" "$g"; then
		same=$((same + 1))
		continue
	fi
	differ=$((differ + 1))
	echo "DIFFERS: ${f##*/}"
	{ diff <(tar --zstd -xOf "$f" .FILES) <(tar --zstd -xOf "$g" .FILES) || true; } |
		awk '/^[<>] f /{print "    " $NF}' | sort -u | head -n 10
done
msg "reproducibility: $same identical, $differ different"
