#!/bin/bash
# Round 23: the files of a live ISO, read without booting it (7z reads the ISO and its squashfs).
#
#   build/m23-static.sh ISO [dev]
#
# A release ISO: root locked, no serial port in /etc/securetty, no bootstrap.pub, the hardening and firewall
# files, login.defs, the setuid list exactly the documented one (docs/SECURITY.md 6.5). With "dev": the dev
# twin (empty root, ttyS0 allowed). Exit 1 when a check fails. Run it on an older ISO as a negative control.
set -uo pipefail
iso=${1:?usage: m23-static.sh ISO [dev]}
mode=${2:-release}
w=$(mktemp -d "${TMPDIR:-$HOME/freelfs-work/scratch}/m23-static.XXXXXX")
trap 'rm -rf "$w"' EXIT
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }
check() { local t=$1; shift; if "$@" > /dev/null 2>&1; then ok "$t"; else bad "$t"; fi; }
7z e -y -o"$w" "$iso" live/root.sfs > /dev/null 2>&1 || { echo "FAIL cannot read live/root.sfs from $iso"; exit 1; }
7z x -y -o"$w/r" "$w/root.sfs" etc usr/lib/sysctl.d usr/libexec/fl-firewall-mdns > /dev/null 2>&1
7z l -slt "$w/root.sfs" > "$w/list.txt" 2>/dev/null
rh=$(awk -F: '$1 == "root" {print $2}' "$w/r/etc/shadow")
if [ "$mode" = dev ]; then
	check "dev twin: root hash is empty" test -z "$rh"
	check "dev twin: ttyS0 is in securetty" grep -qx ttyS0 "$w/r/etc/securetty"
	check "dev twin: the issue says Development image" grep -q 'Development image' "$w/r/etc/issue"
else
	check "root is LOCKED in /etc/shadow (hash '$rh')" test "${rh#[!*]}" != "$rh"
	check "root's hash is not empty" test -n "$rh"
	check "securetty exists and has no serial port" bash -c "test -s '$w/r/etc/securetty' && ! grep -q '^tty[SUA]' '$w/r/etc/securetty'"
	check "securetty lists tty1 to tty6" bash -c "for i in 1 2 3 4 5 6; do grep -qx tty\$i '$w/r/etc/securetty' || exit 1; done"
	check "the issue file has no development-image line" bash -c "! grep -q 'Development image' '$w/r/etc/issue'"
	check "bootstrap.pub is not in the image" test ! -e "$w/r/etc/flpkg/keys/bootstrap.pub"
	check "the repo key is in the image" test -s "$w/r/etc/flpkg/keys/freelfs-repo.pub"
fi
check "tester has an empty password (the live session)" grep -q '^tester::' "$w/r/etc/shadow"
check "login.defs: CONSOLE /etc/securetty" grep -Eq '^CONSOLE[[:space:]]+/etc/securetty' "$w/r/etc/login.defs"
check "login.defs: SU_WHEEL_ONLY yes" grep -Eq '^SU_WHEEL_ONLY[[:space:]]+yes' "$w/r/etc/login.defs"
check "the hardening sysctl file is there" test -s "$w/r/etc/sysctl.d/10-freelfs-hardening.conf"
check "it sets kptr_restrict, dmesg_restrict, bpf, yama, protected_*, suid_dumpable" bash -c "f='$w/r/etc/sysctl.d/10-freelfs-hardening.conf'; for k in kernel.kptr_restrict kernel.dmesg_restrict kernel.unprivileged_bpf_disabled kernel.yama.ptrace_scope fs.protected_symlinks fs.protected_hardlinks fs.protected_fifos fs.protected_regular fs.suid_dumpable net.ipv4.conf.all.rp_filter; do grep -q \"^\$k = \" \$f || { echo missing \$k; exit 1; }; done"
check "it does not turn user namespaces off" bash -c "! grep -v '^#' '$w/r/etc/sysctl.d/10-freelfs-hardening.conf' | grep -q max_user_namespaces"
check "the firewall rules and the service are there" bash -c "test -s '$w/r/etc/nftables.conf' && grep -q 'policy drop' '$w/r/etc/nftables.conf' && test -x '$w/r/etc/rc.d/init.d/firewall'"
check "the firewall is first in SERVICES" grep -q '^SERVICES=(firewall ' "$w/r/etc/rc.conf"
check "nft is in the image" grep -q ' usr/sbin/nft$' "$w/list.txt"
check "the mDNS hook is in the image" grep -q 'usr/libexec/fl-firewall-mdns$' "$w/list.txt"
# setuid files: parse the 7z listing (Path + Attributes)
python3 -I - "$w/list.txt" > "$w/suid.txt" <<'PY'
import sys
path = attr = None
out = []
for line in open(sys.argv[1], errors="replace"):
    line = line.rstrip("\n")
    if line.startswith("Path = "):
        path = line[7:]
    elif line.startswith("Mode = ") and path:
        a = line[7:].strip()
        if a[:1] == "-" and len(a) >= 10 and a[3] in "sS":
            out.append("/" + path)
        path = None
print(" ".join(sorted(out)))
PY
want="/usr/bin/chage /usr/bin/chfn /usr/bin/chsh /usr/bin/flauth /usr/bin/flmount /usr/bin/flpersist /usr/bin/flpkg-seat /usr/bin/flpower /usr/bin/flsnap /usr/bin/flsplash-set /usr/bin/fltime-set /usr/bin/fusermount3 /usr/bin/gpasswd /usr/bin/newgidmap /usr/bin/newgrp /usr/bin/newuidmap /usr/bin/passwd /usr/bin/su /usr/bin/traceroute /usr/libexec/dbus-daemon-launch-helper /usr/libexec/flinstall-helper /usr/libexec/fllock-check"
have=$(cat "$w/suid.txt")
if [ "$have" = "$want" ]; then ok "the setuid files are exactly the documented list"; else bad "setuid list differs: have [$have]"; fi
for f in ping ping6 mount umount; do
	case " $have " in *" /usr/bin/$f "*) bad "$f is still setuid" ;; *) ok "$f is not setuid" ;; esac
done
echo "RESULT: $([ $fails = 0 ] && echo PASS || echo "FAIL ($fails)")"
[ $fails = 0 ]
