#!/bin/bash
# Save or restore a snapshot of the target root (without sources and build
# scratch space), e.g. the temporary system before chapter 8 as the book
# suggests. Runs in a user namespace so files are archived as owned by root.
#
#   ./build/backup.sh save NAME      -> $FLFS_WORK/backups/NAME.tar.zst
#   ./build/backup.sh restore NAME   replaces $FLFS with the snapshot

set -euo pipefail
. "$(dirname "$0")/config.sh"
. "$(dirname "$0")/lib.sh"

[ $# -eq 2 ] || die "usage: $0 save|restore NAME"
[ "$EUID" -ne 0 ] || die "run this as your normal user"
file=$FLFS_WORK/backups/$2.tar.zst

case $1 in
	save)
		mkdir -p "${file%/*}"
		unshare --map-root-user tar -C "$FLFS" --exclude=./sources \
			--exclude=./.bootstrap --exclude=./proc --exclude=./sys \
			--exclude=./dev --exclude=./run -cpf - . |
			zstd -T0 -q -o "$file"
		msg "saved $(du -h "$file" | cut -f1) to $file"
		;;
	restore)
		[ -f "$file" ] || die "no snapshot $file"
		unshare --map-root-user bash -c '
			cd "$1" && find . -mindepth 1 -maxdepth 1 \
				! -name sources ! -name .bootstrap -exec rm -rf {} + &&
			zstd -dc "$2" | tar -xpf -' _ "$FLFS" "$file"
		msg "restored $file into $FLFS (stamps in $FLFS_STAMPS are unchanged)"
		;;
	*) die "usage: $0 save|restore NAME" ;;
esac
