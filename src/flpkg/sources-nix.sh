#!/bin/bash
# flpkg sources, the Nix source (round 24b). Sourced by sources.sh (one hook line there); it defines
# nix_ok, nixc and search_nix, which sources.sh calls, and nothing else is changed in sources.sh.
#
#   search   nixpkgs through the flake registry: nix search nixpkgs TEXT (JSON, read by python3)
#   install  nix profile install nixpkgs#ATTR (through the daemon)
#   remove   nix profile remove NAME
#   list     nix profile list
#   upgrade  nix profile upgrade '.*'  (sources.sh)
#
# Nix is the client; the work is done by nix-daemon, which the rc service "nix" starts on demand (a
# connection to the socket below starts it; it stops after 60 s idle). So the service must be running:
# otherwise nix would fall back to single-user mode, which cannot write /nix as a person.
#
# On a live system without persistence /nix is in RAM: the first install asks first, with the size
# that nix itself predicts (nix build --dry-run prints the download and unpacked sizes).
# SPDX-License-Identifier: MIT

NIX_SOCKET=${FLPKG_NIX_SOCKET:-/var/nix/daemon-socket/socket}

nix_ok() { have "$NIX"; }

# nixc: the nix client. "profile install" first checks the service and warns on a live system without
# persistence; sources.sh calls nixc for that and needs no change.
nixc() {
	if [ "${1:-}" = profile ] && [ "${2:-}" = install ]; then
		nix_service_check || die "the Nix service is not running (as root: rc nix start)"
		if live_ram; then
			local id=${3#nixpkgs#} size
			size=$(nix_size "$id")
			warn_ram "$id" "$size"
		fi
	fi
	NO_COLOR=1 "$NIX" --extra-experimental-features 'nix-command flakes' "$@" # no ANSI codes in the names it prints
}

nix_service_check() { [ -S "$NIX_SOCKET" ] || [ -n "${FLPKG_NIX_NO_SERVICE_CHECK:-}" ]; }

# nix_size ATTR -> "0.25 MiB download, 0.92 MiB unpacked" (what nix will fetch), or nothing
nix_size() {
	"$NIX" --extra-experimental-features 'nix-command flakes' build --dry-run --no-link "nixpkgs#$1" 2>&1 |
		sed -n 's/.*(\([0-9.]* [KMGT]i\?B download[^)]*\)).*/\1/p' | head -n 1
}

# The package index. `nix search` evaluates all of nixpkgs: about 3.3 GB of RAM (the kernel killed it in a 3.5 GB guest), so
# search reads a cached list instead: the channel's packages.json.br (10 MB download, 418 MB unpacked JSON) is streamed through
# python one package at a time (little RAM) into ~/.cache/flpkg/nix-index.tsv ("attr, version, description"), rebuilt when older
# than 7 days. FLPKG_NIX_INDEX_URL / FLPKG_NIX_INDEX_FILE are test hooks.
NIX_INDEX_URL=${FLPKG_NIX_INDEX_URL:-https://channels.nixos.org/nixpkgs-unstable/packages.json.br}
nix_index_file() { echo "${FLPKG_NIX_INDEX_FILE:-${XDG_CACHE_HOME:-${HOME:-/tmp}/.cache}/flpkg/nix-index.tsv}"; }
nix_index_build() { # nix_index_build FILE
	local f=$1
	mkdir -p "${f%/*}" || return 1
	say "building the Nix package list (one download of about 10 MB, a minute; kept in ${f%/*} for a week)"
	curl -fsSL "$NIX_INDEX_URL" | brotli -dc | python3 -I -c '
import json, sys
dec = json.JSONDecoder()
buf = sys.stdin.read(1 << 20)
pos = buf.find("\"packages\"")
pos = buf.index("{", pos + 10) + 1
out = sys.stdout
def need():
    global buf, pos
    more = sys.stdin.read(1 << 20)
    if not more:
        return False
    buf = buf[pos:] + more
    pos = 0
    return True
while True:
    while True:
        while pos < len(buf) and buf[pos] in " ,\n\t":
            pos += 1
        if pos < len(buf):
            break
        if not need():
            sys.exit(0)
    if buf[pos] == "}":
        break
    try:
        key, p2 = dec.raw_decode(buf, pos)
        p2 = buf.index(":", p2) + 1
        while buf[p2] in " \n\t":
            p2 += 1
        val, p3 = dec.raw_decode(buf, p2)
    except (ValueError, IndexError):
        if not need():
            sys.exit(0)
        continue
    pos = p3
    meta = val.get("meta") or {}
    if val.get("system") != "x86_64-linux" or meta.get("available") is False:
        continue
    desc = (meta.get("description") or "").replace("\t", " ").replace("\n", " ")
    out.write("%s\t%s\t%s\n" % (key, val.get("version", ""), desc))
' > "$f.new" && [ -s "$f.new" ] && mv "$f.new" "$f" || { rm -f "$f.new"; return 1; }
}
nix_index() { # prints the index file name, builds or refreshes it when needed (a stale one is kept when offline)
	local f
	f=$(nix_index_file)
	if [ -n "${NIX_INDEX_NOBUILD:-}" ]; then [ -s "$f" ] && echo "$f"; return; fi
	if [ ! -s "$f" ] || [ -n "$(find "$f" -mtime +7 2> /dev/null)" ]; then
		nix_index_build "$f" || [ -s "$f" ] || return 1
	fi
	echo "$f"
}

# search_nix TEXT -> "nix<TAB>attr<TAB>version<TAB>description"
search_nix() {
	nix_ok || return 0
	nix_service_check || return 0
	local f
	f=$(nix_index) || return 0
	awk -F'\t' -v t="$(printf '%s' "$1" | tr 'A-Z' 'a-z')" '
		tolower($1) == t { print "0\tnix\t" $1 "\t" $2 "\t" $3; next }
		index(tolower($1), t) { print "1\tnix\t" $1 "\t" $2 "\t" $3; next }
		index(tolower($3), t) { print "2\tnix\t" $1 "\t" $2 "\t" $3 }' "$f" | sort -s -t$'\t' -k1,1n | head -n 40 | cut -f2- | exact_first_col "$1"
}
