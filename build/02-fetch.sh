#!/bin/bash
# Download every source in sources.lock into the host cache and verify its
# SHA256, then make them available in $FLFS/sources for the build and chroot.

set -euo pipefail
. "$(dirname "$0")/config.sh"
. "$(dirname "$0")/lib.sh"

[ -f "$FLFS_TOP/sources.lock" ] || die "no sources.lock (run build/update-lock.sh)"
mkdir -p "$FLFS_CACHE"

total=0 fetched=0
while read -r sha file url; do
	dest=$FLFS_CACHE/$file
	total=$((total + 1))
	if [ -f "$dest" ] && echo "$sha  $dest" | sha256sum -c --status; then
		continue
	fi
	msg "downloading $file"
	download "$url" "$dest" || die "download failed: $url"
	if ! echo "$sha  $dest" | sha256sum -c --status; then
		rm -f "$dest"
		die "$file: checksum mismatch (expected $sha)"
	fi
	fetched=$((fetched + 1))
done < <(lock_entries)
msg "$total sources verified in $FLFS_CACHE ($fetched downloaded)"

# Hard-link into the target root (same filesystem as the cache: no extra space).
mkdir -p "$FLFS_SOURCES"
while read -r sha file url; do
	[ -e "$FLFS_SOURCES/$file" ] ||
		ln "$FLFS_CACHE/$file" "$FLFS_SOURCES/" 2>/dev/null ||
		cp "$FLFS_CACHE/$file" "$FLFS_SOURCES/"
done < <(lock_entries)
msg "sources available in $FLFS_SOURCES"
