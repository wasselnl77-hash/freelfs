#!/bin/bash
# Check that the build host has everything needed to bootstrap FreeLFS.
# Required tools follow the LFS host requirements; optional ones are needed
# for later milestones (disk image, QEMU testing, flboot).

LC_ALL=C
fail=0
warn=0

ok()   { printf '  OK    %s\n' "$*"; }
bad()  { printf '  FAIL  %s\n' "$*"; fail=1; }
soft() { printf '  WARN  %s\n' "$*"; warn=1; }

# ver_check NAME COMMAND MIN_VERSION
ver_check() {
	local name=$1 cmd=$2 min=$3 v
	if ! type -p "$cmd" >/dev/null; then
		bad "$(printf '%-11s not found' "$name")"
		return
	fi
	v=$("$cmd" --version 2>&1 | grep -Eo '[0-9]+\.[0-9.]+' | head -n1)
	if printf '%s\n' "$min" "$v" | sort -V -C; then
		ok "$(printf '%-11s %-10s (>= %s)' "$name" "$v" "$min")"
	else
		bad "$(printf '%-11s %-10s (needs >= %s)' "$name" "$v" "$min")"
	fi
}

# alias_check NAME LINK  -- LINK must resolve to a program called NAME
alias_check() {
	if "$2" --version 2>&1 | grep -qi "$1"; then
		ok "$(printf '%-11s -> %s' "$2" "$1")"
	else
		bad "$(printf '%-11s is not %s' "$2" "$1")"
	fi
}

echo "Required tools:"
ver_check Bash      bash     3.2
ver_check Binutils  ld       2.13.1
ver_check Bison     bison    2.7
ver_check Coreutils sort     8.1
ver_check Diffutils diff     2.8.1
ver_check Findutils find     4.2.31
ver_check Gawk      gawk     4.0.1
ver_check GCC       gcc      5.2
ver_check G++       g++      5.2
ver_check Grep      grep     2.5.1
ver_check Gzip      gzip     1.3.12
ver_check M4        m4       1.4.10
ver_check Make      make     4.0
ver_check Patch     patch    2.5.4
ver_check Perl      perl     5.8.8
ver_check Python    python3  3.4
ver_check Sed       sed      4.1.5
ver_check Tar       tar      1.22
ver_check Texinfo   makeinfo 5.0
ver_check Xz        xz       5.0.0

echo "Kernel:"
kver=$(uname -r | grep -Eo '^[0-9.]+')
if printf '%s\n' 5.4 "$kver" | sort -V -C; then
	ok "Linux      $kver (>= 5.4)"
else
	bad "Linux      $kver (needs >= 5.4)"
fi
if mount | grep -q 'devpts on /dev/pts' && [ -e /dev/ptmx ]; then
	ok "UNIX 98 PTYs available"
else
	bad "UNIX 98 PTYs not available"
fi

echo "Aliases:"
alias_check bash  sh
alias_check GNU   awk
alias_check Bison yacc

echo "Compiler:"
tmp=$(mktemp -d)
printf 'int main(){}\n' > "$tmp/t.cpp"
if g++ -o "$tmp/t" "$tmp/t.cpp" 2>/dev/null; then
	ok "g++ can compile and link"
else
	bad "g++ cannot compile and link"
fi
rm -rf "$tmp"

echo "Optional (later milestones):"
for t in qemu-system-x86_64 qemu-system-aarch64 zstd mkfs.btrfs mkfs.fat \
         losetup git curl sqlite3; do
	if type -p "$t" >/dev/null; then ok "$t"; else soft "$t not found"; fi
done
ovmf=
for f in /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd \
         /usr/share/edk2/x64/OVMF_CODE.4m.fd /usr/share/ovmf/OVMF.fd \
         /usr/share/qemu/ovmf-x86_64-4m-code.bin /usr/share/qemu/ovmf-x86_64-code.bin; do
	[ -f "$f" ] && { ovmf=$f; break; }
done
if [ -n "$ovmf" ]; then ok "OVMF firmware ($ovmf)"; else soft "OVMF firmware not found"; fi

echo
echo "CPU threads: $(nproc)"
mem_gb=$(awk '/MemTotal/ { printf "%d", $2 / 1048576 + 0.5 }' /proc/meminfo)
echo "RAM: ${mem_gb} GB"
[ "$mem_gb" -lt 16 ] && echo "  note: <16 GB RAM; large builds (LLVM, Firefox) will need extra swap."
if [ "$fail" -ne 0 ]; then
	echo "Host check FAILED: fix the FAIL lines above before building."
	exit 1
fi
[ "$warn" -ne 0 ] && echo "Required tools OK; some optional tools are missing."
[ "$warn" -eq 0 ] && echo "Host check passed."
exit 0
