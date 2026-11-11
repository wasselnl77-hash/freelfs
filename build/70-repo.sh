#!/bin/bash
# D3: turn the built packages into a static, signed package repository.
#
#   ./build/70-repo.sh            (as your normal user, after 60-packages.sh;
#                                  wrap in build/gentle.sh for low priority)
#
# Takes the .flpkg files 60-packages.sh left in $FLFS/.bootstrap/pkg/out, copies
# new or changed ones into the repo directory, signs each (NAME.flpkg.sig,
# minisign format), and rebuilds + signs index.zst (newest version of every
# package). The result is plain files: any static host serves it. Nothing is
# uploaded; see docs/flpkg.md ("Publishing a repo") for how to publish.
#
# The signing key lives OUTSIDE the source tree, by default
# ~/freelfs-work/repo-key/freelfs.key (mode 0600, directory 0700), and is made
# on the first run. Only the public half goes into the tree, to
# config/flpkg/keys/freelfs-repo.pub: the flpkg recipe ships it as
# /etc/flpkg/keys/freelfs-repo.pub, so installed systems trust this repo.
# Lose the key and installed systems will refuse the repo until they get a new
# public key; back it up (build/backup.sh does not include it on purpose).
#
# Settings (environment):
#   FLFS_PKG_OUT    package files   (default $FLFS/.bootstrap/pkg/out)
#   FLFS_REPO_DIR   output repo     (default $FLFS_WORK/repo/$FLFS_ARCH)
#   FLFS_REPO_KEY   key base path   (default $FLFS_WORK/repo-key/freelfs)
#   FLFS_REPO_PUBDIR  where the public key + repo-url go (default config/flpkg)
#   FLFS_REPO_URL   where it will be hosted; written to config/flpkg/repo-url
#                   so the flpkg recipe adds "repo freelfs URL" to flpkg.conf
#   FLPKG           the flpkg binary (default: the one 60-packages.sh built,
#                   run through the target's dynamic loader, or flpkg in PATH)
#   FLFS_REPO_KEEP_OLD=1   keep older versions in the repo directory

set -euo pipefail
. "$(dirname "$0")/config.sh"
. "$(dirname "$0")/lib.sh"
[ "$EUID" -ne 0 ] || die "do not run this as root"

PKG_OUT=${FLFS_PKG_OUT:-$FLFS/.bootstrap/pkg/out}
REPO=${FLFS_REPO_DIR:-$FLFS_WORK/repo/$FLFS_ARCH}
KEY=${FLFS_REPO_KEY:-$FLFS_WORK/repo-key/freelfs}
PUBDIR=${FLFS_REPO_PUBDIR:-$FLFS_TOP/config/flpkg}
[ -d "$PKG_OUT" ] || die "no packages in $PKG_OUT (run build/60-packages.sh first)"
case $(realpath -m "$KEY") in
"$FLFS_TOP"/*) die "the signing key must not live inside the source tree ($KEY)" ;;
esac

# flpkg: the host's, or the one built in the chroot, run with the target's loader
if [ -n "${FLPKG:-}" ]; then
	flpkg() { "$FLPKG" "$@"; }
elif command -v flpkg > /dev/null 2>&1; then
	flpkg() { command flpkg "$@"; }
else
	BIN=$FLFS/.bootstrap/pkg/flpkg-src/flpkg
	[ -x "$BIN" ] || die "no flpkg: set FLPKG, or run build/60-packages.sh"
	# the target's OpenSSL must not read the host's openssl.cnf (crypto policies)
	EMPTY_CNF=$(mktemp)
	trap 'rm -f "$EMPTY_CNF"' EXIT
	flpkg() {
		OPENSSL_CONF=$EMPTY_CNF "$FLFS/lib64/ld-linux-x86-64.so.2" \
			--library-path "$FLFS/usr/lib" "$BIN" "$@"
	}
fi

# 1. the signing key (never inside the tree, never overwritten)
if [ ! -e "$KEY.key" ]; then
	msg "generating the repo signing key $KEY.key"
	(umask 077 && mkdir -p "$(dirname "$KEY")" && flpkg --config /dev/null key generate "$KEY")
fi
chmod 700 "$(dirname "$KEY")"
chmod 600 "$KEY.key"
[ -f "$KEY.pub" ] || die "$KEY.pub is missing; restore it or delete $KEY.key to start a new key"

# 2. the public key goes into the image config (public half only)
mkdir -p "$PUBDIR/keys"
if [ -f "$PUBDIR/keys/freelfs-repo.pub" ] && ! cmp -s "$KEY.pub" "$PUBDIR/keys/freelfs-repo.pub"; then
	warn "config/flpkg/keys/freelfs-repo.pub differs from $KEY.pub; replacing it (installed systems need the new key)"
fi
install -m644 "$KEY.pub" "$PUBDIR/keys/freelfs-repo.pub"
if [ -n "${FLFS_REPO_URL:-}" ]; then
	printf '%s\n' "$FLFS_REPO_URL" > "$PUBDIR/repo-url"
fi

# 3. packages new or changed since the last run
mkdir -p "$REPO"
new=()
shopt -s nullglob
for f in "$PKG_OUT"/*.flpkg; do
	b=$(basename "$f")
	if [ ! -f "$REPO/$b" ] || [ ! -f "$REPO/$b.sig" ] || ! cmp -s "$f" "$REPO/$b"; then
		new+=("$f")
	fi
done
[ -e "$REPO/index.zst" ] || [ ${#new[@]} -gt 0 ] || die "no .flpkg files in $PKG_OUT"
msg "${#new[@]} new or changed package(s) to sign"

# 4. sign them and rebuild the signed index (a single flpkg run)
opts=(--key "$KEY.key")
[ -n "${FLFS_REPO_KEEP_OLD:-}" ] || opts+=(--remove-old)
flpkg --config /dev/null repo add "${opts[@]}" "$REPO" "${new[@]}"
install -m644 "$KEY.pub" "$REPO/freelfs-repo.pub" # for people to fetch; trust comes from the image

# 5. check it the way a client does: sync from a scratch root with only this key
chk=$(mktemp -d)
trap 'rm -rf "$chk"; rm -f "${EMPTY_CNF:-}"' EXIT
mkdir -p "$chk/keys" "$chk/root"
cp "$KEY.pub" "$chk/keys/"
cat > "$chk/flpkg.conf" <<CONF
cache = /var/cache/flpkg
keys = $chk/keys
repo freelfs file://$REPO
CONF
flpkg --config "$chk/flpkg.conf" --root "$chk/root" sync

n=$(ls "$REPO"/*.flpkg | wc -l)
msg "repo ready: $REPO ($n packages, $(du -sh "$REPO" | cut -f1))"
cat <<TXT

To publish, copy the directory's contents to any static host so that
  <URL>/index.zst, <URL>/index.zst.sig and <URL>/NAME-VERSION-ARCH.flpkg[.sig]
are reachable, e.g. for a Hugging Face dataset:
  hf upload YOURNAME/freelfs-repo $REPO x86_64 --repo-type dataset
  (URL: https://huggingface.co/datasets/YOURNAME/freelfs-repo/resolve/main/x86_64)
then set FLFS_REPO_URL to that URL and rebuild the flpkg package, or on a
running system add "repo freelfs URL" to /etc/flpkg.conf. Nothing was uploaded.
TXT
