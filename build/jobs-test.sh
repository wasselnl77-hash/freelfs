#!/bin/bash
# Dry run of the per-package job counts. Builds nothing and needs no chroot:
# it prints the RAM, zram and CPU numbers of this host, the light and heavy
# counts from build/config.sh, and the count each recipe would get. A recipe
# with jobs=heavy gets the heavy count, every other recipe the light count.
#
# usage: build/jobs-test.sh [PACKAGE...]   (default: every recipe in recipes/)
#        FLFS_JOBS=2 build/jobs-test.sh    (shows the forced count)

set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
FLFS_TOP=$(dirname "$here")
. "$here/config.sh"

echo "host: $flfs_ncpu CPUs, MemTotal $flfs_mem_mb MB, zram counted as $flfs_zram_mb MB (half its size)"
echo "light: $FLFS_JOBS_LIGHT jobs   heavy: $FLFS_JOBS_HEAVY jobs   general (FLFS_JOBS): $FLFS_JOBS"
echo

if [ $# -gt 0 ]; then
	dirs=()
	for p in "$@"; do dirs+=("$FLFS_TOP/recipes/$p"); done
else
	dirs=("$FLFS_TOP"/recipes/*)
fi

printf '%-26s %-6s %s\n' PACKAGE CLASS JOBS
fails=0
for dir in "${dirs[@]}"; do
	[ -f "$dir/recipe" ] || { echo "no recipe in $dir" >&2; fails=$((fails + 1)); continue; }
	# jobs is set in the recipe itself; sourcing a recipe only assigns variables.
	class=$(bash -c '. "$1/recipe"; echo "${jobs:-light}"' _ "$dir")
	case $class in
		heavy) n=$FLFS_JOBS_HEAVY ;;
		light) n=$FLFS_JOBS_LIGHT ;;
		*) echo "$(basename "$dir"): jobs must be heavy or light, not '$class'" >&2
		   fails=$((fails + 1)); continue ;;
	esac
	[ "$n" -ge 1 ] 2> /dev/null || { echo "$(basename "$dir"): bad job count '$n'" >&2; fails=$((fails + 1)); continue; }
	printf '%-26s %-6s %s\n' "$(basename "$dir")" "$class" "$n"
done

[ "$fails" -eq 0 ] || { echo "$fails problem(s)" >&2; exit 1; }
