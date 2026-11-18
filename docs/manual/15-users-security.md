# 15. Users, login, root actions and security

This chapter covers accounts and groups, the login screen, the password prompt, the setuid helpers and what each one allows, the checks on the desktop's privileged requests, the browser sandbox, and the Secure Boot status.

Status: round 17 (2026-11-14). Everything here was tested in QEMU and headless. Nothing has been tested on a real machine yet beyond the earlier laptop rounds.

## Contents

1. [Accounts and groups](#151-accounts-and-groups)
2. [Login: flgreet and flgreet-ui](#152-login-flgreet-and-flgreet-ui)
3. [The password prompt: flauth](#153-the-password-prompt-flauth)
4. [Setuid helpers and what each allows](#154-setuid-helpers-and-what-each-allows)
5. [Privileged requests to the compositor](#155-privileged-requests-to-the-compositor)
6. [The lock screen](#156-the-lock-screen)
7. [The login shell and the default users](#157-the-login-shell-and-the-default-users)
8. [The browser sandbox (FreeBWS)](#158-the-browser-sandbox-freebws)
9. [Updates and trust: signed packages](#159-updates-and-trust-signed-packages)
10. [Secure Boot](#1510-secure-boot)
11. [Development and install defaults](#1511-development-and-install-defaults)
12. [Known issues](#1512-known-issues)
13. [Root, where it may log in, kernel hardening and the firewall](#1513-root-where-it-may-log-in-kernel-hardening-and-the-firewall)

## 15.1 Accounts and groups

The standard accounts are declared by packages (`sysusers` and `sysgroups` in `recipes/base-files` and others), not shipped in a file that an upgrade would replace. The groups that matter for security (`recipes/base-files`, `sysgroups`):

| Group | GID | Who is in it | What membership gives |
|---|---|---|---|
| `root` | 0 | root | everything |
| `seat` | 98 | every person at the machine: the groups a new user gets (`seat`, `audio`, `video`, `render`) | use of the seat: the screen, the input devices, and the setuid helpers in 15.4 that are marked `root:seat 4750` |
| `wheel` | 97 | nobody by default | iwd's D-Bus policy lets it talk to iwd besides root (the comment in `config/iwd/seat-dbus.conf`) |
| `netdev` | 86 | nobody by default | named by the Wi-Fi page README as a group that may use iwd on D-Bus; it is declared in `base-files` |
| `messagebus` | 18 | the D-Bus daemon user | the D-Bus helper `dbus-daemon-launch-helper` |
| `utmp` | 13 | the login record writers | `/var/log/lastlog`, `/run/utmp` |
| `users` | 999 | every new user (the default group) | - |
| `tty` | 5 | the `wall` and `write` helpers | ownership in `config/ownership.list` |

A new account gets the default group `users` (999), the groups `seat,audio,video,render`, the home `/home/NAME` and the shell `/usr/bin/flsh` (`/etc/default/useradd`, `recipes/shadow`). The installer adds the first account with `seat` and refuses names that clash with a system name or with a folder in `/home` (`src/flinstall/README`).

## 15.2 Login: flgreet and flgreet-ui

`flgreet` runs on tty1 (`/etc/inittab`) and is restarted by init when it exits (`src/flgreet/README`):

1. As root, flgreet takes tty1 and starts `flcomp --firstboot` with `FLCOMP_SETUP=/usr/libexec/flgreet-ui`, a clean environment and `XDG_RUNTIME_DIR=/run/flgreet`.
2. `flgreet-ui` (root, inside that compositor) draws the screen: the accounts, a password field, **Log in**, and **Shut down** (a second click within 4 seconds confirms).
3. flgreet accepts only a name that is in `/etc/passwd` with a UID from **1000 to 59999**. It creates `/run/user/UID` (mode 0700, owned by the user) and runs `login -f NAME`.
4. The password is checked with `crypt_r` against `/etc/shadow`:
   - an empty hash logs in without a password **only for `tester` while `/run/live` exists** (the live session); on any other account or system it is refused ("This account has no password and cannot log in");
   - a hash that starts with `!` or `*` is locked, and that account cannot log in;
   - a wrong password waits one second before the next try.
5. The password is never printed or logged, and its buffer is wiped after use.

If flcomp cannot start or crashes, flgreet runs `/sbin/agetty` on tty1, so the console always works. Ctrl+Alt+F2 to F6 give text logins at any time.

Programs cannot be started from the login screen: flcomp runs with `--firstboot`, so Ctrl+Alt+T, Super+T, Super+E and Ctrl+Shift+Esc do nothing there (round 16 change, `docs/ROADMAP-NEXT.md`).

## 15.3 The password prompt: flauth

`flauth` (`src/flauth`, recipe 0.1 r1) asks the person for their password before a root action. It is setuid root, group `seat` (`4750`), and does not use PAM. The password is checked against `/etc/shadow` with `crypt()`, and the dialog (`flauth-dialog`) runs as the person, never as root. The policy is the same in every helper: the code is `src/flauth/flauth-check.h` (copied into each component that needs it). The policy as a whole is in `docs/SECURITY.md`, section 5.

| Rule | Value |
|---|---|
| Wrong passwords in a row | three; then the account is locked out of flauth for **30 s**, then 60 s, and so on up to **10 minutes** |
| Each wrong password | costs one extra second |
| Stamp after success | `/run/flauth/ok-UID.CLASS`, valid **2 minutes**, for that one class and for the program that asked (its executable, resolved through `/proc`) |
| Stamp of a destructive class | one-shot: the first check that passes uses it up |
| Empty password | refused on every system (`set a password first`), except the live session's `tester` on the live system (`/run/live` present): no dialog, no stamp needed |
| Exit codes | 0 right (or the command ran), 1 wrong password, 2 cancelled, 3 locked out, broken or misused, 4 no password can unlock the account, or none is set |

The classes (`flauth --class`):

| Class | Used for | Stamp |
|---|---|---|
| `pkg` | `flpkg-seat` install, remove, upgrade | reused for 2 minutes by the same program (several changes from the Store need one password) |
| `time` | `fltime-set`, every command | reused for 2 minutes |
| `splash` | `flsplash-set` | reused for 2 minutes |
| `mount-internal` | `flmount` on an internal disk (only with `--internal`) | reused for 2 minutes |
| `disk-write` | `flinstall-helper`, the install itself | one-shot |
| `persist-create` | `flpersist create` | one-shot |
| `snap-rollback` | `flsnap rollback` | one-shot |
| `snap-delete` | `flsnap delete`, `rm`, `prune` | one-shot |

The flauth test hooks (`FLAUTH_SHADOW`, `FLAUTH_DIR`, `FLAUTH_DIALOG`, `FLAUTH_LIVE`, `FLAUTH_USER`) are ignored by the setuid copy. `make -C src/flauth check` runs the policy tests without root.

## 15.4 Setuid helpers and what each allows

The setuid helpers are programs whose effective user is root. The recipes set their owner and mode (`owner=` in each `recipe`). Each one checks its own input. All are `root:seat 4750`: only members of group `seat` can run them at all.

Who may do what, and whether it needs the person's password (policy in `docs/SECURITY.md`, section 5):

| Action | Helper | Password |
|---|---|---|
| Mount, unmount, eject a removable drive; the live boot stick is mounted without a prompt | `flmount` | **no** |
| Mount, unmount, eject an internal disk (`--internal`) | `flmount` | **every time**, class `mount-internal` |
| Power off, restart, log out, power saving | `flpower` | **no** |
| Check the lock screen's password | `fllock-check` | **no** (it checks the person's own password, one try a second) |
| Install and remove packages, update all (system) | `flpkg-seat` | **every time** (class `pkg`, reused for 2 minutes) |
| Write a disk: the installer's install step | `flinstall-helper` | **every time**, one-shot; only when started by `/usr/bin/flinstall` |
| Create the live session's persistent home file | `flpersist create` | **every time**, one-shot |
| Make a snapshot current again (rollback) | `flsnap rollback` | **every time**, one-shot |
| Delete snapshots (`delete`, `rm`, `prune`) | `flsnap` | **every time**, one-shot |
| Take a snapshot (`create`), list, show, status | `flsnap` | **no**; a person's `create` never removes old snapshots |
| Date, time zone, automatic time | `fltime-set` | **every time** (class `time`, reused for 2 minutes) |
| Boot screen style | `flsplash-set` | **every time** (class `splash`, reused for 2 minutes) |

| Program | Recipe | Owner and mode | What it allows | What it refuses |
|---|---|---|---|---|
| `/usr/bin/flpower` | `flpanel` | `root:seat 4750` | `off`, `restart`, `logout` (own session only), `saver on`, `saver off` | any other word; the power saving writes only two fixed sysfs files per CPU with values from a fixed list (`src/flpanel/flpower.c`) |
| `/usr/bin/flpkg-seat` | `flpackages` | `root:seat 4750` | `install NAME...`, `remove NAME...`, `upgrade`, after the password (class `pkg`) | file names, options and any name that starts with `-` or ends in `.flpkg`; the environment is cleared |
| `/usr/bin/fltime-set` | `fltime` | `root:seat 4750` | `zone NAME` (a real zoneinfo file only), `ntp on\|off`, `sync`, `time EPOCH` (only while NTP is off), after the password (class `time`) | any path outside zoneinfo; a bad zone name is refused before the question; exit 3 and 4 on refusals |
| `/usr/libexec/fllock-check` | `fllock` | `root:seat 4750` | reads one password line from stdin and tells the lock screen whether it is right for the person who started it | everything else. Exit 0 right, 1 wrong, 2 no password can unlock the account |
| `/usr/bin/flsnap` | `flsnap` | `root:seat 4750` | `create`, `list`, `show`, `status` (no password); `rollback`, `delete`, `rm`, `prune` after the password (one-shot) | any other argument; the environment and descriptors are discarded when it runs with more rights than the caller; an elevated `create` does not prune |
| `/usr/bin/flauth` | `flauth` | `root:seat 4750` | the password prompt, then the command given after `--`; `--class` is required | a command runs as the person, not as root; an empty password other than the live tester |
| `/usr/bin/flmount` | `flmount` | `root:seat 4750` | `list`, `mount DEV`, `unmount DEV`, `eject DEV` for `/dev/NAME` that the kernel lists under `/sys/class/block` (mounts at `/drives/LABEL`, `nosuid,nodev,noexec`); internal disks only with `--internal` and the password; for root only, `--hotplug`, `--auto`, `--gone` | any path that the caller names |
| `/usr/bin/flpersist` | `flmount` | `root:seat 4750` | `create SIZE [--btrfs]` (after the password, one-shot; makes `freelfs-persist.img` on the live stick) and `status` | any other path |
| `/usr/libexec/flinstall-helper` | `flinstall` | `root:seat 4750` | the installer's steps, only on the live system, only a whole disk that the kernel lists as not the live medium, not read-only and not mounted; the disk write needs the password (one-shot) and a parent process that is `/usr/bin/flinstall` | anything else; a start by any other program; the test hooks, when the caller is less privileged than the program |
| `/usr/libexec/dbus-daemon-launch-helper` | `dbus` | `root:messagebus 4750` | starts D-Bus services for the system bus | - |
| `/usr/bin/fusermount3` | `fuse3` | `root:root 4755` | FUSE mounts for any user (the standard FUSE helper) | - |

**Note on `flinstall-helper`:** the disk write needs the person's password (class `disk-write`, one-shot) like the other destructive actions. The live `tester` has an empty password, so on the live system the install needs no question, as before; the live-only rule means an installed system never takes that path. The installer's window answers a refused install with `NEEDAUTH disk-write` and runs flauth. The helper must also be started by `/usr/bin/flinstall`. The live tester can therefore install without a prompt, and any other program on the live system needs the password for that step.

**Note on `flpower`:** a member of `seat` can power the machine off without a password (`flpower off`), by design: the panel's power menu needs no prompt.

## 15.5 Privileged requests to the compositor

flcomp's private `fl_shell` protocol (see [10](10-architecture.md)) has some requests that change the whole session. flcomp accepts them only from programs that are the desktop's own:

| Request | Who may send it |
|---|---|
| `logout` | flpanel, flgreet-ui and the other trusted programs listed below |
| `set_output`, `output_configure`, `output_revert` | the same |
| `set_desktop` (the icon layer) | the same |
| `lock` | the same (fllock and flpanel) |

The trusted programs are `flpanel`, `fllock`, `flgreet`, `flgreet-ui`, `flsettings` and `fldisplay`. flcomp identifies a client by the executable its process runs (`/proc/PID/exe`, which a process cannot fake). The executable must be in `/usr/bin`, `/usr/sbin` or `/usr/libexec`, and its name must be one of those. Test runs set `FLCOMP_TRUST_ANY` to lift the check (`src/flcomp/flshell.c`).

## 15.6 The lock screen

- **Super+L** locks the screen at once and starts `fllock` (`/usr/bin/fllock`).
- The **lid** locks the screen too, unless `lock_on_lid=0` in `desktop.conf` (default 1).
- The lock is the compositor's: while locked nothing but the lock client's surface is drawn. There is no panel, no window, no notification, and no launching of programs. Ctrl+Alt+F1 to F12 and the media keys still work.
- If the lock program dies, the screen stays locked and flcomp starts it again after 300 ms. Up to three quick deaths in a row are retried.
- The password check is done by `fllock-check` (setuid, 15.4), never by the lock program itself.

The lock screen does not use flauth, because it is the person's own session (`src/flcomp/README`, "Lock screen").

## 15.7 The login shell and the default users

- `/usr/bin/flsh` is the default shell (`/etc/default/useradd`). It is a POSIX shell written by FreeLFS (`src/flsh`). The bash shell stays for scripts and builds.
- The live system's account is `tester`, with an empty password, in group `seat`. The ISO and the USB image are built that way (`build/53-iso.sh`, `build/52-usb.sh`).
- `/etc/profile` runs `/etc/profile.d/*.sh` for every login shell (`src/flcomp/desktop.sh` starts the desktop on tty1).

## 15.8 The browser sandbox (FreeBWS)

FreeBWS is the browser (`src/freebws`, on WPE WebKit 2.54.2). Its web processes run in WebKit's **bubblewrap sandbox**, which is the part of a browser that handles hostile pages and images (`docs/SECURITY.md`, section 3). It has been **on** since the security round of 2026-11-12. Before that it was off.

How it works:

- WebKit starts each web process as `bwrap ... WPEWebProcess` with new user, mount, pid and uts namespaces, a seccomp filter, a private `/tmp` and an empty runtime folder.
- `/etc`, `/usr` and `/lib*` are read-only. The GPU nodes (`/dev/dri`, `/dev/udmabuf`), the fonts and GStreamer data are visible. The session bus is reachable only through `xdg-dbus-proxy`.
- The home folder, `~/.ssh`, the rest of `/run`, `/mnt` and `/media` are **not** visible to a web process.
- The IPC namespace is **not** unshared for WPE web processes (WebKit does that only for GTK and Wayland); this was measured in QEMU.
- The **network process is not sandboxed** by WebKit. It handles the cache, cookies, TLS and writes downloads. It runs no JavaScript.

What FreeBWS does on top:

- Before it creates a web context, `freebws` runs bwrap itself with the same kind of namespaces (`freebws --sandbox-check` prints `sandbox: bubblewrap works ...` and returns 0 when it works). WebKit does not check this, and it would abort the browser at the first web process if bwrap cannot start.
- Downloads: a response that is a file or attachment becomes a download. The policy was added in the security round of 2026-11-12 (`docs/SECURITY.md`, section 3).

Turning it off: `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` disables the sandbox in WebKit. It is named "dangerous" by WebKit for that reason. Use it only to test.

Status: the QEMU tests passed (`build/m12-sandbox.sh`, a download's sha256 equal, a fallback when bwrap is removed). Not yet tested on the laptop or a real GPU in the sandbox (`docs/SECURITY.md`, section 3). The network process is outside the sandbox, and the session bus is reachable from web processes only through the proxy.

## 15.9 Updates and trust: signed packages

- Packages and the repo index are signed with Ed25519 (minisign format). Clients trust the `*.pub` keys in `/etc/flpkg/keys`, which the flpkg package installs.
- `signatures = required` in `/etc/flpkg.conf` makes an unsigned package an error. A `.flpkg` file given on the command line is checked too, unless `--unsigned` is given.
- The index carries `serial` and `valid-until`. An expired index or a lower serial is refused. Key rotation is possible with a signed statement (`flpkg key introduce`).
- The full design, the key custody rules and the cadence for re-signing are in `docs/SECURITY.md` section 1 and in [13](13-packages-flpkg.md).
- Known vulnerabilities are tracked with `build/cve-check.py` against the NVD (`docs/CVE-REPORT.md` and `docs/CVE-KERNEL.md`). The count recorded on 2026-11-12, before and after the fixes: critical 13 to 0, high 87 to 1, medium 123 to 5, low 13 to 1 (`docs/ROADMAP-NEXT.md`, security round). The kernel triage is in `docs/CVE-KERNEL.md`. These are the project's own counts and have not been re-checked for this chapter.

## 15.10 Secure Boot

**Secure Boot is not implemented.** `flboot` and the kernel are not signed, so **Secure Boot must be turned off** in the firmware. The live images and the release notes say so (`docs/SECURITY.md`, section 4).

The options, in the project's order of effort (`docs/SECURITY.md`, section 4):

1. Own keys enrolled in the firmware, with `sbsign` for `BOOTX64.EFI` and the kernel, and module signing.
2. A Microsoft-signed `shim` with a MOK enrolled on the first boot. This needs physical presence and works on unmodified machines.
3. Own shim signed by Microsoft. Not realistic at this stage (`docs/SECURITY.md`).

Secure Boot verifies only PE images. The initrd and the command line that flboot passes are not verified, and the root file system has no dm-verity. The project's recommendation is to keep "Secure Boot off" in the release notes and to do option 1 first.

## 15.11 Development and install defaults

| Where | Root password | Other accounts | Source |
|---|---|---|---|
| Live ISO, USB image and the released images | **locked** (`!`): nobody logs in as root, on no terminal. The build refuses an image whose root hash is empty | `tester`, **empty password only while `/run/live` exists** (the live session), group `seat` | `build/53-iso.sh`, `build/lib.sh` (`check_root_locked`) |
| Development / test image (built with `FLFS_DEV_ROOT=1`) | **none** (empty), `ttyS0` allowed in `/etc/securetty`, `/etc/issue` says "Development image: log in as root, there is no password." | `tester` as above | `build/root-lock.sh --dev`, `build/53-iso.sh` |
| Installed system (`flinstall`) | **your choice in the account step**: the same password as the first user (default), or its own password. Never empty | the first user, with groups `seat,wheel,audio,video,render` | `src/flinstall/` |

A password is at least **8** characters in the installer (`FI_PW_MIN`), up to 128 (`FI_PW_MAX`). The installer asks it twice; root's own password, when chosen, twice as well.

## 15.12 Known issues

| Issue | Status |
|---|---|
| Secure Boot not implemented | by design for now; keep it off |
| The stamp of `pkg`, `time`, `splash`, `mount-internal` is reused for 2 minutes by the same program, so a hostile program that runs as the person and uses that program's executable can reuse it (destructive classes are one-shot) | by design (policy in SECURITY.md section 5); not verified against a hostile local user |
| The live `tester` has an empty password: on the live system any seat member can install without a question (only through `/usr/bin/flinstall`) | by design for the live session; the installed system refuses empty passwords, and root is locked on the live ISO |
| No login rate limit beyond three tries per `login` run | open (section 15.13) |
| Module signing, KEXEC and io_uring removal from the kernel | planned (the sysctls switch kexec and io_uring off) |
| Development images have an empty root password (`FLFS_DEV_ROOT=1`) | by design for test images only; the release build refuses them |
| Browser sandbox: not run on the laptop or a real GPU | open |
| Vulnerability counts are from 2026-11-12 | re-check before each publication (`build/cve-check.py`) |
| flauth lock-out timing and the stamp on a shared machine | tested in QEMU; not on hardware |
| Repo index re-signing every 10 days once the repo is hosted | open (see [13](13-packages-flpkg.md)) |

## 15.13 Root, where it may log in, kernel hardening and the firewall

Details and the reasons are in `docs/SECURITY.md` section 6. In short:

- **Root.** The live ISO has root **locked**. On the live system you are `tester` (empty password, only while `/run/live` exists); administrator
  actions go through the password dialog (flauth) or are not asked on the live system for `tester`. On an installed system root has the first user's password or its own, as chosen
  in the installer (account step, "Use the same password for the administrator (root)").
- **Where root may log in.** `/etc/securetty` lists tty1 to tty6: no root login over the serial port. `su` works for group `wheel` only.
- **Setuid files.** `ping`, `ping6`, `mount`, `umount`, `newuidmap` and `newgidmap` are no longer setuid; `traceroute` is setuid for group `seat` only. SECURITY.md section 6.5 lists every remaining
  setuid and setgid file and why it stays.
- **Kernel.** `/etc/sysctl.d/10-freelfs-hardening.conf` is applied at boot: restricted kernel pointers and `dmesg`, no unprivileged BPF, Yama ptrace scope 1, protected links and files,
  no kexec, no io_uring, network hardening. User namespaces stay on for Flatpak and bubblewrap. `/proc/config.gz` shows the kernel configuration (Yama, lockdown available but off,
  hardened usercopy, zeroed allocations, fortify, `BPF_UNPRIV_DEFAULT_OFF`).
- **Firewall.** `nft` and `/etc/nftables.conf`, started first by the rc service `firewall`: inbound traffic is dropped unless it answers something this computer started
  (loopback, ICMP essentials and DHCP replies are allowed; mDNS only while avahi runs). Outbound traffic is not filtered. Open a port by adding a line to the `input` chain.
- **Packages.** Only the FreeLFS repo key is trusted in release images.
