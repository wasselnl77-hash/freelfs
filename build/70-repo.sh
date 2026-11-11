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
#   FLFS_REPO_VALID_DAYS   how long clients accept the signed index (default 14).
#                   It is signed into the index as valid-until together with an
#                   ever-increasing serial: flpkg refuses an expired index and
#                   one older than the last it accepted. So run this script
#                   again (it re-signs even when no package changed) before the
#                   validity runs out, from the machine that holds the key.
#   FLFS_REPO_INTRODUCE    key rotation: base path of a NEW key (NEW.pub, made with
#                   'flpkg key generate NEW'). Writes rotate.pub[.sig] into the repo,
#                   signed with the current key; clients then trust both keys from
#                   their next sync (the new public key also goes to config/flpkg/keys).
#   FLFS_REPO_RETIRE_DAYS  with FLFS_REPO_INTRODUCE: days until clients stop trusting
#                   the old key (default 60). Procedure and key custody: docs/SECURITY.md

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

# 2. the public key goes into the image config (public half only). The default key
#    "freelfs" is installed as freelfs-repo.pub, any other (a rotation) under its own
#    name, so during a transition the image carries both.
mkdir -p "$PUBDIR/keys"
pubname=$(basename "$KEY").pub
[ "$pubname" != freelfs.pub ] || pubname=freelfs-repo.pub
if [ -f "$PUBDIR/keys/$pubname" ] && ! cmp -s "$KEY.pub" "$PUBDIR/keys/$pubname"; then
	warn "config/flpkg/keys/$pubname differs from $KEY.pub; replacing it (installed systems need the new key)"
fi
install -m644 "$KEY.pub" "$PUBDIR/keys/$pubname"
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
# The index gets a serial (above the previous one) and valid-until (default 14 days).
VALID_DAYS=${FLFS_REPO_VALID_DAYS:-14}
case $VALID_DAYS in ''|*[!0-9]*) die "FLFS_REPO_VALID_DAYS must be a whole number of days" ;; esac
[ "$VALID_DAYS" -ge 1 ] || die "FLFS_REPO_VALID_DAYS must be at least 1"
opts=(--key "$KEY.key" --valid-days "$VALID_DAYS")
[ -n "${FLFS_REPO_KEEP_OLD:-}" ] || opts+=(--remove-old)
flpkg --config /dev/null repo add "${opts[@]}" "$REPO" "${new[@]}"
install -m644 "$KEY.pub" "$REPO/$pubname" # for people to fetch; trust comes from the image

# 4b. key rotation: introduce a new key, signed by the current one (docs/SECURITY.md)
if [ -n "${FLFS_REPO_INTRODUCE:-}" ]; then
	NEWKEY=$FLFS_REPO_INTRODUCE
	[ -f "$NEWKEY.pub" ] || die "$NEWKEY.pub does not exist (make it with: flpkg key generate $NEWKEY)"
	RETIRE_DAYS=${FLFS_REPO_RETIRE_DAYS:-60}
	case $RETIRE_DAYS in ''|*[!0-9]*) die "FLFS_REPO_RETIRE_DAYS must be a whole number of days" ;; esac
	flpkg --config /dev/null key introduce --key "$KEY.key" --new "$NEWKEY.pub" \
		--retire-days "$RETIRE_DAYS" "$REPO"
	newname=$(basename "$NEWKEY").pub
	install -m644 "$NEWKEY.pub" "$PUBDIR/keys/$newname"
	msg "rotation announced. Keep signing with the OLD key until clients had time to sync"
	msg "(at least one release cycle), then run with FLFS_REPO_KEY=$NEWKEY and rebuild flpkg + image."
	msg "Remove the old public key from config/flpkg/keys only after the retirement date."
fi

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
msg "the signed index is valid for $VALID_DAYS days: sign again (run this script) before $(date -u -d "+$VALID_DAYS days" +%F)"
cat <<TXT

To publish, copy the directory's contents to any static host so that
  <URL>/index.zst, <URL>/index.zst.sig and <URL>/NAME-VERSION-ARCH.flpkg[.sig]
are reachable, e.g. for a Hugging Face dataset:
  hf upload YOURNAME/freelfs-repo $REPO x86_64 --repo-type dataset
  (URL: https://huggingface.co/datasets/YOURNAME/freelfs-repo/resolve/main/x86_64)
then set FLFS_REPO_URL to that URL and rebuild the flpkg package, or on a
running system add "repo freelfs URL" to /etc/flpkg.conf. Nothing was uploaded.
TXT
