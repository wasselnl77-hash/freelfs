# Security audit, round 23 (2026-11-18)

Scope: the shipped system (rootfs ~/freelfs-work/rootfs-x86_64, the released live ISO
~/freelfs-work/freelfs-live.iso = the prealpha-20261118 download), the build scripts, the
installer, flpkg, FreeBWS, and the tracked repo. Read-only: no builds, no QEMU, no commits.

Evidence notes: the ISO's live/root.sfs was read with 7z into the session scratchpad (not the
repo). Owner and group numbers in the squashfs listing are the pre-boot values; fix-owners sets
the group seat on the helpers at first boot. Live behaviour (sysctl values, listening sockets)
is inferred from the config and must be confirmed on a booted image.

Haiku A owns the fl* auth policy. Items below that touch it are listed as findings, not redesigned.

## Ranked findings

### CRITICAL

**C1. The released live ISO has an empty root password, and so does the tester account.**
- Root: build/40-system-config.sh:71-75 (`dev_login_step`: `passwd -d root`, issue banner) and
  build/60-packages.sh:94-100 (`firstboot_step`: `passwd -d root` in the image root). The ISO is
  built from that root (build/53-iso.sh squash step), so root.sfs inherits it.
- Verified: the published ISO (live/root.sfs /etc/shadow, 2026-11-18 00:06) has `root::` (empty)
  and `tester::` (empty). The USB image (build/52-usb.sh) and the disk image (build/50-image.sh)
  are built from the same 60-packages root, so they inherit the empty root hash unless their own
  scripts change it (not checked in the built images).
- The inittab starts agetty on tty1-6 and the serial console (config/etc/inittab), and there is no
  /etc/securetty, so anyone at the keyboard or on the serial port can type `root` and press Enter.
- The release page (site/download.html:27) and release notes (~/freelfs-work/release/rel.json)
  publish this build.
- Fix: in build/53-iso.sh (before mksquashfs) set root's hash to `!` (locked) and fail the build if
  the root hash is empty or the root.sfs shadow is not as expected. Make `dev_login_step` and
  `firstboot_step` run only with an explicit FLFS_DEV_IMAGE=1 that the release scripts refuse to
  set. Pull the r22 ISO from site/download.html until the rebuilt image passes the check.

**C2. On the live ISO, the tester account (empty password, group seat) is root-equivalent.**
- ISO /etc/group: `seat:x:98:tester`, also audio, video, render. ISO /etc/passwd: tester uid 1000,
  shell /usr/bin/flsh.
- An empty hash skips the password question: src/flauth/flauth.c:211-212 (`ok = 1`) and
  src/flauth/flauth-check.h:27 (flauth_ok returns 1). flpkg-seat, flpower, flmount, flpersist,
  fltime-set, flsplash-set, flinstall-helper and fllock-check are setuid root, mode 4750, group
  seat (flpkg db pending_owners, rootfs var/lib/flpkg/db.sqlite).
- Concrete: src/flinstall/helper.c (main loop, ~2400-2425) takes commands on stdin (`set disk`,
  `part`, `install`) and has no confirmation token. It erases any whole disk that is not the live
  medium and is not mounted. Its own header says so. The erase confirmation exists only in the
  window.
- Fix: do not give the live user the seat group unless the live session asks for a password (for
  example, a first-boot "set a password" prompt, which then also covers root). Alternatively,
  require an explicit confirm token (random per-session value shown in the window) for `install`.
  This overlaps Haiku A's policy work; send the findings there.

### HIGH

**H1. Root and the first user share one password on installed systems, with no lockout and no
restriction on where root may log in.**
- src/flinstall/common.h:25 `FI_ROOT_SAME_PASSWORD 1` and src/flinstall/helper.c:1600 write the user's
  hash to root. Root can log in on every getty and on the serial port: no /etc/securetty, no PAM,
  so no faillock. login(1) allows 5 tries per connection and agetty respawns, so tries are unlimited.
- The installer's minimum password is 4 characters (src/flinstall/common.h:33, `FI_PW_MIN 4`).
- The installer hashes with SHA-512 crypt (src/flinstall/helper.c:1372-1379), while
  config/etc/login.defs sets `ENCRYPT_METHOD YESCRYPT`. Both are acceptable, but they should match.
- Fix: default `FI_ROOT_SAME_PASSWORD 0` (root locked, `!`); administration goes through flauth.
  Add an /etc/securetty listing only tty1 and the serial console if root login is kept. Raise
  FI_PW_MIN to 8 and use yescrypt. This is a design decision, so ask the user before changing it.

**H2. Empty-password checks exist in three places and are the same weakness.**
- flgreet-ui.c:~104 (`hash[0] == 0` returns "accepted"), flauth.c:211, fllock-check.c:113. An
  account with an empty hash on an installed system (for example a failed shadow edit in
  helper.c:1597-1603, or the USB image) passes the greeter, the lock screen and flauth.
- Fix: keep the empty-password path only in dev builds, behind the same flag as C1.

**H3. Kernel sysctls are not set, so the kernel defaults apply.**
- config/etc/sysctl.conf is empty (the install step only touches it). The only shipped file is
  usr/lib/sysctl.d/70-freelfs-gaming.conf (vm.max_map_count). rc.sysinit:152 runs `sysctl --system`.
- Expected on the built kernel, from the config: kernel.kptr_restrict = 0 (not set),
  kernel.dmesg_restrict = 0 (CONFIG_SECURITY_DMESG_RESTRICT not set), kernel.unprivileged_bpf_disabled
  = 0 (BPF_SYSCALL=y in config/kernel/freelfs.config and BPF_UNPRIV_DEFAULT_OFF not set), no
  yama ptrace_scope (CONFIG_SECURITY_YAMA not set), fs.suid_dumpable = 0 (default), fs.protected_*
  = 1 (default). The last two are defaults; confirm on a booted system with `sysctl -a`.
- Fix: add /etc/sysctl.d/60-freelfs-hardening.conf:
  ```
  kernel.kptr_restrict = 2
  kernel.dmesg_restrict = 1
  kernel.unprivileged_bpf_disabled = 2
  kernel.yama.ptrace_scope = 1          # needs CONFIG_SECURITY_YAMA=y
  fs.protected_symlinks = 1
  fs.protected_hardlinks = 1
  fs.protected_fifos = 2
  fs.protected_regular = 2
  fs.suid_dumpable = 0
  kernel.kexec_load_disabled = 1
  net.ipv4.conf.all.accept_redirects = 0
  net.ipv4.conf.all.send_redirects = 0
  net.ipv4.conf.all.rp_filter = 1
  ```

**H4. Kernel config: the hardening switches are missing from the fragment and the stale kernel
config does not match it.**
- config/kernel/freelfs.config has none of: CONFIG_SECURITY_YAMA, CONFIG_SECURITY_LOCKDOWN_LSM,
  CONFIG_MODULE_SIG (+ FORCE), CONFIG_HARDENED_USERCOPY, CONFIG_INIT_ON_ALLOC_DEFAULT_ON,
  CONFIG_FORTIFY_SOURCE, CONFIG_BPF_UNPRIV_DEFAULT_OFF, CONFIG_SECURITY_DMESG_RESTRICT.
- Present, good: CONFIG_STACKPROTECTOR_STRONG=y, CONFIG_RANDOMIZE_BASE=y, CONFIG_RANDOMIZE_MEMORY=y,
  CONFIG_STRICT_DEVMEM=y, CONFIG_USERFAULTFD off, CONFIG_SECURITY_LOADPIN off (fine).
- Not verifiable: the ISO kernel (live/vmlinuz-7.3.4-freelfs) has no IKCFG blob, so its real
  .config cannot be read. The rootfs config (boot/config-6.18.59-freelfs, dated Oct 28) is stale,
  and it has CONFIG_BPF_SYSCALL unset although the fragment (Nov 17) sets it, so the rootfs does
  not reflect the current fragment.
- Also on: CONFIG_IO_URING=y, CONFIG_KEXEC=y, CONFIG_IA32_EMULATION=y, CONFIG_USER_NS=y. io_uring
  is a large kernel attack surface for a desktop that does not need it; disable unless a shipped
  program needs it.
- Fix: add the options above to the fragment and make check_config (build/41-kernel.sh) also fail on
  missing *non-default* options. Ship /proc/config.gz (CONFIG_IKCONFIG=y, IKCONFIG_PROC=y) so
  the audit can read the real config. Module signing needs a key-management decision (ask the
  user).

**H5. No firewall, and no userspace to run one.**
- No nft, iptables, ip6tables, ufw or firewalld in the ISO (checked the squashfs listing). The
  kernel has CONFIG_NF_TABLES=y. Nothing listens on TCP or UDP at idle (see H6 for the list), so the
  exposure is latent: avahi (UDP 5353) starts whenever cups starts, and anything that binds a port
  later is open.
- Fix: ship nftables (recipe) and /etc/nftables.conf, plus an rc service enabled by default:
  ```
  table inet filter {
    chain input {
      type filter hook input priority 0; policy drop;
      iif lo accept
      ct state invalid drop
      ct state established,related accept
      ip protocol icmp accept
      meta l4proto ipv6-icmp accept
      udp sport 67 udp dport 68 accept     # DHCP replies (dhcpcd)
      udp dport 5353 accept                # mDNS, only while avahi runs; drop this line if avahi stays off
    }
    chain forward { type filter hook forward priority 0; policy drop; }
    chain output { type filter hook output priority 0; policy accept; }
  }
  ```
  Remove the 5353 line when avahi is not running, or make avahi a service that the rule set
  follows.

**H6. Listening sockets at idle (from the rc services and configs).**
- dbus-daemon: unix system bus only (rootfs usr/share/dbus-1/system.conf:42). No TCP listen.
- syslog: `syslogd -m 0` without `-r` (config/etc/rc.d/init.d/syslog:5). /dev/log only.
- seatd: unix socket only. iwd: D-Bus and netlink, no TCP. bluetoothd: unix and HCI only.
- dhcpcd: client on UDP 68 (privsep user dhcpcd). fl-sntp: UDP client only (src/fltime/fl-sntp.c:73-78).
- cupsd: `Listen /run/cups/cups.sock`, `WebInterface No` (rootfs etc/cups/cupsd.conf:18, 28). It is
  started on demand by flsocketd (config/etc/rc.d/init.d/cups:11), which makes the socket
  mode 0666, so any local user can start cupsd and avahi.
- avahi-daemon: started by flcupsd (config/cups/flcupsd), answers mDNS on UDP 5353 on every link.
  publish-workstation and publish-hinfo are off (rootfs etc/avahi/avahi-daemon.conf:49-50).
- No sshd, no telnet. Good.

**H7. Setuid programs that do not need setuid (stock packages).**
- usr/bin/traceroute (4755): needs raw sockets only. Use `setcap cap_net_raw+ep` or drop setuid.
- usr/bin/ping, ping6 (4755): same. Prefer file capabilities.
- usr/bin/mount, umount (4755): no fstab line carries `user`, so only flmount should mount. Drop
  setuid (flmount already does the work for the seat group).
- usr/bin/su (4755): no PAM and `SU_WHEEL_ONLY no` (config/etc/login.defs). Any local user who
  knows root's password (H1) can switch. Set `SU_WHEEL_ONLY yes` (the installed user is in wheel,
  helper.c:1298) or drop su.
- usr/bin/newuidmap, newgidmap (4755): needed only for rootless user-namespace id maps. etc/subuid
  and etc/subgid are empty, so they do nothing today. Drop setuid until a rootless container
  tool needs them.
- usr/bin/fusermount3 (4755): needed for FUSE. user_allow_other is commented out in etc/fuse.conf
  (good); keep it that way.
- usr/bin/chage, chfn, chsh, passwd, gpasswd, newgrp (4755): normal shadow-utils setuid. Keep.
- usr/bin/wall, write (2755 root:tty): normal. Keep.
- usr/libexec/dbus-daemon-launch-helper (4750 root:messagebus): normal. Keep.

**H8. Setuid FreeLFS helpers (4750 root:seat), purpose and argument notes.** These are the root
surface and Haiku A owns the policy. Listed here for completeness.
- usr/bin/flauth: asks for the caller's password, stamps /run/flauth/ok-UID, then runs CMD as the
  caller (flauth.c:260-268). Test hooks FLAUTH_DIR, FLAUTH_SHADOW, FLAUTH_DIALOG are read only when
  euid != 0 (flauth.c:43, 48, 138, 195): correct.
- usr/bin/flmount: mount, unmount and eject for the seat group. clearenv and fixed PATH (flmount.c:186-187).
- usr/bin/flpersist: creates the persistence image on the boot stick (flpersist.c:135 creates it
  0600 root, then mkfs with clearenv and fixed PATH).
- usr/bin/flpkg-seat: install and remove only, names validated (boot-test expects a rejection of
  `../x`). Runs /usr/bin/flpkg as root after the stamp.
- usr/bin/flpower: shut down, restart and power-save words only (flpower.c:33-39, 215-217).
- usr/bin/flsnap: btrfs snapshots. Not in the flpkg database, so after first boot it stays root:root
  4750 and only root can run it. Probably a bug (see M4).
- usr/bin/flsplash-set: boot-screen style. Test hook FLSPLASH_SET_DIR gated on geteuid (flsplash-set.c:36, 45).
- usr/bin/fltime-set: time, zone and NTP. Rejects `../` in zone names (m9 test).
- usr/libexec/flinstall-helper: installer, see C2 (no confirm token). Test options refused when elevated (helper.c:2363-2368).
- usr/libexec/fllock-check: lock-screen password check on stdin (fllock-check.c:54, 101 gated on geteuid).

### MEDIUM

**M1. The flpkg trust chain is strong, with one gap.**
- Good: signatures required by default (src/flpkg/config.c:50); the index is signed and checked
  before use (repo.c:305-306); expiry enforced (trust.c:144-153, 14-day validity from repo.c:580);
  serial rollback refused (trust.c:156-163); key rotation must be signed by a key already trusted
  (trust.c rotate_apply, lines 387-415); every package's sha256 is checked against the signed
  index (cmd_pkg.c:477-478, repo.c:252-257); curl is limited to http and https (repo.c:184), with
  CA verification on.
- Gap: config/etc or the image trusts bootstrap.pub in every image (60-packages.sh firstboot copies
  $KEY.pub into /etc/flpkg/keys; the ISO has etc/flpkg/keys/bootstrap.pub). Its private key is
  ~/freelfs-work/rootfs-x86_64/.bootstrap/pkg/keys/bootstrap.key (0600, not in the ISO, not in git).
  Anyone holding that key can sign packages that installed systems accept from any repo they add.
- Fix: do not install bootstrap.pub in release images. Keep it for the local build repo only.
- Also: the signing key ~/freelfs-work/repo-key/freelfs.key is unencrypted (flpkg sign.c:214
  refuses encrypted keys). Keep it off every machine except the signing host, and add a passphrase
  when flpkg supports it.

**M2. Browser web process sandbox: on by default, but the network process is not sandboxed.**
- src/freebws/freebws.c:2346-2364: sandbox=auto by default; bwrap is probed at runtime, not setuid
  (usr/bin/bwrap is 0755). Good.
- freebws.c:2351: the WebKit network process is not sandboxed by WebKit. It handles all
  downloads and TLS. Consider seccomp or a bwrap wrapper for it.
- The release notes say "the browser is not sandboxed yet" (rel.json), which is out of date.
  Fix the text.

**M3. Flatpak and unprivileged user namespaces.**
- CONFIG_USER_NS=y and no AppArmor or sysctl limit, so any local user can create user namespaces.
  Flatpak (usr/bin/flatpak) and bwrap depend on this. It widens the kernel attack surface available
  to a local attacker. Ask the user whether Flatpak is needed in the release. If not, set
  `user.max_user_namespaces = 0` (or drop Flatpak). If yes, document it.

**M4. flsnap is in no package database and is root:root 4750.** Non-root seat users cannot run it.
Add flsnap to the owner rules (pending_owners: `usr/bin/flsnap 0 98 2536`) or record the intent.

**M5. Adblock lists are fetched unsigned.**
- src/freebws/adblock.c fetch_list: HTTPS through libsoup (TLS verified); the list is checked for
  size and "!" only. Integrity is by TLS alone. No signature.
- The lists can carry `+js(...)` scriptlet rules (adblock.c:612, 1105-1120). Call arguments are
  JSON-quoted (json_q, adblock.c:1112), so this is data, not code injection. A poisoned list can
  still choose which bundled scriptlets run on which site.
- Fix: keep the lists TLS-only but pin an expected hash per list in the build, or ship signed list
  bundles, and keep the last good copy when verification fails.

**M6. Persistence image is plaintext on the stick.** flpersist creates freelfs-persist.img with
ext4 or btrfs and no encryption. Whoever holds the stick reads /home. Document it, or offer LUKS.

**M7. Shipped known-vulnerable packages.** docs/CVE-REPORT.md lists sqlite 3.54.1 with one high.
The kernel CVE triage (docs/CVE-KERNEL.md) reports 0 open for 7.3.4. The release notes admit known
vulnerable packages. Fine for pre-alpha, but the release page should link the report.

**M8. seat group membership is the admin group.** config/iwd/seat-dbus.conf and
config/bluez/seat-dbus.conf: "every desktop user is in group seat". Any seat member can reach iwd
and org.bluez. config/bluez/seat-dbus.conf:17-24 also allows any local process to send the bluez
Agent1, MediaEndpoint1, Profile1 interfaces (default context). Restrict those interfaces to the
bluetoothd process or to the seat group.

### LOW

**L1. /run mount.** config/etc/fstab maps `tmpfs /run ... defaults`, which is mode 1777 if it wins.
rc.sysinit:10 and :20 mount /run with mode=0755 first, so it is probably fine. Make the fstab line
`mode=0755,nosuid,nodev` to remove the doubt.

**L2. Test secrets in the repo.** No private keys, tokens or API keys in tracked files (regex scan
over `git grep`, paths only). Test credentials exist in build/boot-test.py (Wi-Fi test passphrase,
about line 97) and build/m16/flauth.cmds (test account password, line 1). Both are for throwaway
test VMs. Move them to an untracked file or mark them as test-only in the file header.

**L3. cupsd socket.** flcupsd and flsocketd make /run/cups/cups.sock 0666 (config/etc/rc.d/init.d/cups:11).
Any local user can start cupsd and avahi. Use 0660 with group seat.

**L4. Stale rootfs group file.** rootfs etc/group has wheel 97 but not seat 98 (base-files creates
seat 98, recipes/base-files/recipe:21). The ISO has seat, so this affects only the rootfs tree.
Rebuild the tree before the next ISO.

**L5. /etc/issue text.** The installer removes only the live-system note (helper.c:1532-1534). The
"Development image: log in as root, there is no password" line from build/40-system-config.sh:75
can survive onto installed systems. Remove it in the installer as well. Fix together with C1.

**L6. Persistence and home modes are fine.** /home 0755, /root 0750, /home/tester 0700
(installed home 0700, helper.c:1614), /tmp and /var/tmp 1777, /etc/shadow and gshadow 0400,
/var/lib/flpkg/db.sqlite 0644 (file lists only, no secrets), /etc/flpkg/keys 0755 (public keys),
/run/flauth 0700 root (flauth.c:64). No world-writable directory without the sticky bit in the ISO.

### Positive checks (no action)

- No sudo, doas, polkit or sshd in the image.
- No PAM, so no pam_unix surprises, but also no faillock (see H1).
- Stack protector strong and KASLR on.
- Setuid helpers clear the environment and use a fixed PATH (flmount, flpersist, flpower, fltime-set, flsplash-set, flpkg-seat).
- Test hooks in the setuid helpers are gated on geteuid.
- flpkg defaults to signatures=required, and the installed flpkg.conf (ISO etc/flpkg.conf) does not change it.

## Secrets scan (item 8)

Tracked files: no private key blocks, no GitHub, AWS, OpenAI, Hugging Face or Google API key
patterns, no `password=`/`token=` literal assignments with values (scan prints paths only).
Findings: L2 only. The public key config/flpkg/keys/freelfs-repo.pub is correct to track.
The private signing key (~/freelfs-work/repo-key/freelfs.key, 0600) and the bootstrap key
(~/freelfs-work/rootfs-x86_64/.bootstrap/pkg/keys/bootstrap.key, 0600) are outside the repo, and
build/70-publish-repo.sh refuses to stage a *.key file.

## Items I could not verify here

- Live sysctl values, the real listening sockets, and whether tester can actually run flinstall-helper
  from the shell (needs a booted live image; the boot tests in build/boot-test.py could check this).
- The real .config of the 7.3.4 kernel in the ISO (no IKCFG blob).
- Ownership after fix-owners on a booted system.

## Fix order

1. C1 and C2 together: lock root in release builds, stop giving the live user seat without a password,
   gate the installer behind a confirm token. Pull the r22 ISO from the site until fixed.
2. H1, H2: root locked by default on installs; drop empty-password paths outside dev builds.
3. H3, H4, H5: sysctl.d file, kernel fragment additions, nftables ruleset.
4. H6, H7, M4: trim setuid, fix flsnap ownership, restrict cups socket.
5. M1 to M8 and the lows, in the next round.
