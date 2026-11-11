#!/bin/bash
# Run a build stage so the desktop stays usable while it works:
#
#   ./build/gentle.sh ./build/60-packages.sh [ARGS...]
#
# Lowest CPU and disk priority, no sleep while it runs, and a memory limit
# (FLFS_MEM_HIGH, default: all RAM minus 4 GB for the desktop and an
# integrated GPU's video memory). Past the limit the kernel slows the build
# down and reclaims its memory instead of taking memory from the desktop.
# Needs no sudo: the limit is a systemd user scope.
set -euo pipefail

total_mb=$(awk '/^MemTotal:/ {print int($2 / 1024)}' /proc/meminfo)
high_mb=${FLFS_MEM_HIGH:-$((total_mb - 4096))}
[ "$high_mb" -ge 1024 ] || high_mb=1024

exec systemd-run --user --scope --quiet -p MemoryHigh="${high_mb}M" \
	systemd-inhibit --what=sleep:idle --why="FreeLFS build" \
	nice -n 19 ionice -c 3 "$@"
