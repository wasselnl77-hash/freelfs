# 12. Filesystem layout: disks, folders, subvolumes and snapshots

This chapter describes where FreeLFS puts the system, the homes, the removable drives and the snapshots, and which files hold the settings.

Status: written from the source at round 17 (2026-11-14). The layout changed in round 15 (2026-11-13); older installs are not moved by `flpkg upgrade` (see 12.3).

## Contents

1. [Disk layout](#121-disk-layout)
2. [The mounts in /etc/fstab](#122-the-mounts-in-etcfstab)
3. [The system folder: /usr, /sysflfs and the links](#123-the-system-folder-usr-sysflfs-and-the-links)
4. [Top-level folders](#124-top-level-folders)
5. [Homes and their folders](#125-homes-and-their-folders)
6. [Removable drives under /drives](#126-removable-drives-under-drives)
7. [Settings files in /etc](#127-settings-files-in-etc)
8. [State and logs in /var and /run](#128-state-and-logs-in-var-and-run)
9. [Snapshots](#129-snapshots)
10. [The live system is different](#1210-the-live-system-is-different)
11. [Persistence on a Ventoy stick](#1211-persistence-on-a-ventoy-stick)
12. [Known issues](#1212-known-issues)

## 12.1 Disk layout

An installed FreeLFS uses a GPT disk with two partitions (`src/flinstall/helper.c`, `build/50-image.sh`):

| # | Partition | File system | Label | Mounted at | Size |
|---|---|---|---|---|---|
| 1 | EFI system partition (ESP) | FAT (vfat) | `ESP` | `/boot` | 512 MiB (installer) |
| 2 | Root | btrfs | `freelfs` | `/` (and the subvolumes below) | the rest of the disk |

The btrfs file system has three subvolumes, created in one step by `mkfs.btrfs --rootdir` (see `build/50-image.sh`):

| Subvolume | Mounted at | Contents |
|---|---|---|
| `@` (the default subvolume) | `/` | the system |
| `@home` | `/home` | every person's home folder |
| `@snapshots` | `/.snapshots` | read-only snapshots of `@` (see 12.9) |

The ESP holds the boot loader (`EFI/BOOT/BOOTX64.EFI`), the kernels (`vmlinuz-*`), `ucode.img` (when the microcode package is installed), and `flboot/flboot.conf` plus `flboot/snapshots` (see [11](11-boot-process.md)).

The installer's "Install next to existing partitions" mode uses only free space and needs at least a 6.5 GiB gap on a GPT disk. "Manual" mode lets you choose the partitions; a root partition made there is always btrfs with this layout (`src/flinstall/README`). Those two modes are not yet tested on a real disk (`docs/INTEGRATION-TODO.md`).

## 12.2 The mounts in /etc/fstab

The installed `/etc/fstab` (`config/etc/fstab`):

| Device | Mount point | Type | Options |
|---|---|---|---|
| `LABEL=freelfs` | `/` | btrfs | `defaults,noatime,compress=zstd:1` |
| `LABEL=freelfs` | `/home` | btrfs | `defaults,noatime,compress=zstd:1,subvol=@home,nofail` |
| `LABEL=freelfs` | `/.snapshots` | btrfs | `defaults,noatime,subvol=@snapshots,nofail` |
| `LABEL=ESP` | `/boot` | vfat | `defaults,umask=0022,quiet,nofail` |
| proc, sysfs, devpts, devtmpfs, tmpfs | `/proc`, `/sys`, `/dev/pts`, `/dev`, `/dev/shm`, `/run`, `/tmp`, `/sys/fs/cgroup` | kernel file systems | `nosuid`, `noexec` and `nodev` where it applies; `/tmp` is a tmpfs with mode 1777 |

The installer writes its own `/etc/fstab` and refers to file systems **by UUID**, never by label, so that a second FreeLFS on the same machine cannot clash with it. The installer adds a `swap` line only for a swap partition made in Manual mode (`src/flinstall/helper.c`). The live system has its own fstab (see 12.10).

## 12.3 The system folder: /usr, /sysflfs and the links

The system is in `/usr`, laid out the FHS way (`bin`, `lib`, `libexec`, `sbin`, `share`, `include`, ...). Everything FreeLFS installs goes there.

| Path | What it is |
|---|---|
| `/usr` | the real system directory (`@` subvolume) |
| `/sysflfs` | a link to `usr`. The FreeLFS name for the system; old `/sysflfs/...` paths still work |
| `/bin`, `/lib`, `/sbin` | links to `usr/bin`, `usr/lib`, `usr/sbin` (merged /usr) |
| `/lib64` | a real directory (the dynamic loader's path is fixed by the ABI) |
| `/etc`, `/var`, `/run` | real directories (`/var/run` and `/var/lock` link to `/run` and `/run/lock`) |
| `/mnt`, `/media` | links to `/drives` |
| `/etc/mtab` | link to `/proc/self/mounts` |

The layout history (`docs/PLAN.md`, `docs/ROADMAP-NEXT.md` round 15):

- Until 2026-11-13 it was the other way round: the system was in a real `/sysflfs`, and `/usr` held the homes as links on `@home`.
- Since base-files release 4 (2026-11-13) the system is in `/usr`, and the homes are `/home/NAME`.
- Installs made before that cannot be moved by `flpkg upgrade`. The note for moving an older install is in `docs/PLAN.md` ("Folder layout, second version"). That is not verified here.

File managers hide the system and the mount folders from the root view. The list is in `/.hidden` (`recipes/base-files`): `bin dev etc lib lib64 media mnt opt proc root run sbin srv sys tmp usr var`. So `/` in Files shows `boot`, `drives`, `home` and `sysflfs`.

## 12.4 Top-level folders

```text
/
├── boot/           ESP (FAT): kernels, flboot, ucode.img
├── home/           @home subvolume: one folder per person (/home/NAME)
├── drives/         removable and extra disks (see 12.6)
├── .snapshots/     @snapshots subvolume, mode 0700 (see 12.9)
├── etc/            configuration (12.7)
├── usr/            the system (12.3); /sysflfs is a link to it
├── var/            logs, databases, caches, spool (12.8)
├── run/            runtime state, tmpfs, cleared at boot (12.8)
├── tmp/            tmpfs, mode 1777
├── root/           the administrator's home (mode 0750)
├── srv/, opt/      empty, for the administrator
└── proc/ sys/ dev/   kernel file systems
```

## 12.5 Homes and their folders

- The home of a person is `/home/NAME` (`useradd` default `HOME=/home`, from `/etc/default/useradd`, `recipes/shadow`).
- New users get the skeleton in `/etc/skel` (`recipes/base-files`): folders `desktop`, `documents`, `downloads`, `music`, `pictures`, `videos`, and `.config/user-dirs.dirs` which points the XDG folders at them. The folder names are lowercase.
- The default shell for new users is `/usr/bin/flsh`. The default groups are `seat`, `audio`, `video`, `render` (`/etc/default/useradd`). See [15](15-users-security.md) for what the groups allow.
- Per-person files FreeLFS writes (all in the home folder):

| Path | Written by |
|---|---|
| `~/.config/freelfs/desktop.conf` | desktop settings (theme, accent, outputs, power saving, workspaces, ...); flcomp, flpanel, flsettings, fltheme, fldisplay |
| `~/.config/flsh/flshrc` | the shell's own settings (flsh reads it, and `/etc/flshrc`) |
| `~/.config/autostart/*.desktop` | programs started at login (General Manager's Startup tab edits these) |
| `~/.config/freelfs/themes/` | your own themes (fltheme) |
| `~/.local/share/freelfs/wallpapers/` | imported wallpapers (converted once to `.flw`) |
| `~/.local/share/flatpak/` | the person's Flatpak apps and runtimes (`flatpak --user`) |
| `~/.local/state/freelfs/log/` | copies of the desktop log (`flcomp.log`) |
| `~/pictures/screenshots/` | screenshots (Snappel) |
| `~/freelfs-report-*.tar.gz` | reports made by `flreport` |

## 12.6 Removable drives under /drives

`flmount` (setuid, group `seat`; see [15](15-users-security.md)) mounts removable partitions at `/drives/LABEL`. The name is made from the label: letters, digits, `.`, `_` and `-` are kept, a space becomes `_`, at most 40 characters; a label with nothing usable falls back to the device name. Each mount is `nosuid,nodev,noexec`.

- A stick or disk that appears while the desktop runs is mounted by a udev rule (`src/flmount/90-flmount.rules`). A notice "USB stick 'LABEL' connected" offers to open Files at the mount point.
- At boot, `rc.sysinit` runs `flmount --auto`, which mounts every removable partition that is present.
- Ownership: on vfat, exfat and ntfs the person who mounted it gets the uid and gid. On ext4 and btrfs the top folder is given to the person when root owns it.
- A stick removed without unmounting is detached by `flmount --gone`.
- The live boot stick is made writable and bound to `/drives/LABEL` (flmount header).
- Internal disks are not shown unless `flmount --internal` is used (Files does not use it).

## 12.7 Settings files in /etc

| File | Purpose | Chapter |
|---|---|---|
| `/etc/fstab` | mounts | 12.2 |
| `/etc/inittab` | init and the tty consoles | [14](14-services-init.md) |
| `/etc/rc.conf` | services, network, console, hardware clock | [14](14-services-init.md) |
| `/etc/rc.d/` | boot scripts: `functions`, `rc`, `rc.sysinit`, `rc.shutdown`, `init.d/*` | [14](14-services-init.md) |
| `/etc/hostname`, `/etc/hosts`, `/etc/issue`, `/etc/os-release` | identity | |
| `/etc/passwd`, `/etc/group`, `/etc/shadow`, `/etc/gshadow` | accounts (declared by packages, not shipped) | [15](15-users-security.md) |
| `/etc/default/useradd` | defaults for new users | 12.5 |
| `/etc/skel/` | the new home's skeleton | 12.5 |
| `/etc/profile`, `/etc/profile.d/*.sh` | login shell setup; `desktop.sh` starts the desktop | [11](11-boot-process.md) |
| `/etc/shells` | allowed login shells | |
| `/etc/keyboard.conf` | keyboard layout (`LAYOUT=`, optional `VARIANT=`, `OPTIONS=`) | [17](17-hardware-drivers.md) |
| `/etc/localtime`, `/etc/timezone` | time zone (written by Settings > Date & time) | [16](16-networking.md) |
| `/etc/fltime.conf` | NTP on or off (`NTP=0`) and servers (`SERVERS="..."`) | [16](16-networking.md) |
| `/etc/flpkg.conf` | package manager settings: repos, keys, signature policy, snapshots | [13](13-packages-flpkg.md) |
| `/etc/flpkg/keys/*.pub` | trusted signing keys of the package repo | [13](13-packages-flpkg.md) |
| `/etc/flsnap.conf` | snapshot count (`keep = 8`) | 12.9 |
| `/etc/iwd/main.conf` | Wi-Fi daemon settings | [16](16-networking.md) |
| `/etc/dhcpcd.conf` | wired DHCP; `denyinterfaces wl*` leaves Wi-Fi to iwd | [16](16-networking.md) |
| `/etc/resolv.conf` | DNS (written by iwd's resolvconf helper, or by dhcpcd) | [16](16-networking.md) |
| `/etc/udev/udev.conf`, `/usr/lib/udev/rules.d/` | device manager | [14](14-services-init.md) |

Config files in `/etc` owned by packages are replaced by the new version on upgrade. The package manager can keep a changed file as `FILE.flpkg-save` when `backup_config = yes` is set (see [13](13-packages-flpkg.md)).

## 12.8 State and logs in /var and /run

| Path | What | Notes |
|---|---|---|
| `/var/lib/flpkg/db.sqlite` | the installed-package database (14 MB on the live system) | flpkg; the journal of an unfinished transaction is in the same folder |
| `/var/lib/flpkg/index-serial.REPO` | the last accepted repo index serial | refuses an older index |
| `/var/cache/flpkg/` | downloaded packages and indexes | |
| `/var/lib/freelfs/ownership.done` | stamp that the ownership list was applied | used when there is no flpkg |
| `/var/lib/iwd/` | known Wi-Fi networks (`*.psk`, `*.open`, `*.8021x`) | iwd |
| `/var/lib/bluetooth/` | paired Bluetooth devices | bluez |
| `/var/spool/cups/`, `/var/cache/cups/`, `/var/log/cups/` | printing | CUPS, started on demand |
| `/var/log/iwd.log` | Wi-Fi daemon messages (association, DHCP, DNS) | `flreport` collects it |
| `/var/log/bluetooth.log` | bluetoothd messages | |
| `/var/log/dmesg.log` | kernel messages at boot | written by rc.sysinit |
| `/var/log/wtmp`, `lastlog`, `btmp`, `faillog` | login records | |
| `/var/log/messages` and friends | sysklogd (`syslog` service) | the file names are not verified |
| `/run/utmp` | who is logged in now | |
| `/run/user/UID` | the person's runtime folder (`XDG_RUNTIME_DIR`), mode 0700 | created by flgreet |
| `/run/user/UID/session.log`, `flcomp.log` | the session's and the compositor's logs | |
| `/run/flauth/` | password stamps (5-minute validity) | flauth |
| `/run/flshutdown`, `/run/flshutdown.wd` | the shutdown is in progress; its watchdog pid | rc |
| `/run/flpower-saver` | the CPU settings that power saving replaced | flpower |
| `/run/cups/cups.sock` | the CUPS socket that flsocketd listens on | |
| `/run/*.pid` | pid files of the services (`seatd.pid`, `iwd.pid`, ...) | |
| `/run/live/` | the live system's squashfs, overlay and persistence mounts | live only |

## 12.9 Snapshots

Snapshots are read-only copies of `@` (`src/flsnap/flsnap.c`):

- `/.snapshots/N` is snapshot N, and `/.snapshots/N.info` holds its date, reason and package list.
- A snapshot is taken **before every package transaction**: the flpkg pre-transaction hook runs `flsnap create --auto` (`flpkg.conf` has `snapshots = 1`).
- `flsnap` keeps the last **8** snapshots (`keep` in `/etc/flsnap.conf`). The oldest go first.
- After each change `flsnap` rewrites `/boot/flboot/snapshots`, the list of the boot menu's "Start an earlier state" screen (see [11](11-boot-process.md)).

Commands (see [13](13-packages-flpkg.md) and [19](19-admin-reference.md)):

| Command | What it does | Password |
|---|---|---|
| `flsnap create [--reason TEXT]` | take a snapshot now | no |
| `flsnap list`, `flsnap show N`, `flsnap status` | show snapshots; `status` says which one is booted | no |
| `flsnap delete N...`, `flsnap prune [--keep K]` | remove snapshots | yes (flauth) |
| `flsnap rollback [N]` | make snapshot N the current state | yes (flauth) |

`flsnap rollback` moves `@` to `@snapshots/M` (so the state being left is kept as a snapshot too), makes a writable copy of snapshot N as the new `@`, and sets it as the default subvolume. The next boot starts it. "Make this the current state" in the desktop notice does the same.

A snapshot booted from the menu is read-only: nothing changed in `/` is kept, while `/tmp`, `/run` and `/home` still work.

## 12.10 The live system is different

The live ISO (`build/53-iso.sh`, `build/52-usb.sh`) runs from RAM:

| Part | Live system |
|---|---|
| `/` | an overlay: `root.sfs` (squashfs, read-only) under a tmpfs upper layer `/run/live/rw` (the writes go to RAM) |
| `/etc/fstab` | a live fstab with proc, sys, devpts, `/run`, `/dev`, shm, `/tmp` and cgroup2 only. **No `@home`, `@snapshots` or ESP lines**, so a disk of the host with the same label is never mounted |
| Homes | `/home/tester` (user `tester`, no password) |
| `/home` | RAM, or `freelfs-persist.img` from the boot stick when `flpersist create` made one (see 12.11) |
| Snapshots | off (`snapshots = 0` in the live flpkg.conf) |
| Fsck | none |
| Swap | none |

The live system is in RAM, so its size limit is RAM. See [18](18-performance-ram.md).

## 12.11 Persistence on a Ventoy stick

`flpersist create SIZE` makes a file `freelfs-persist.img` on the data partition of a stick that holds the ISO as a file (Ventoy). At the next boot flinitrd mounts that file as `/home`, so the live session keeps files between boots. A stick written with `dd` has no file system to hold the image. `flpersist status` says `active`, `ready` or `none`. See [11](11-boot-process.md) and [19](19-admin-reference.md).

## 12.12 Known issues

| Issue | Status |
|---|---|
| Older installs (before round 15) cannot be moved by `flpkg upgrade` | by design; the manual step is in `docs/PLAN.md`, not verified here |
| The installer's Manual and "next to existing partitions" modes on a real disk | not yet verified |
| Snapshots and kernel modules can disagree after a kernel update | open (see [11](11-boot-process.md)) |
| The live system's `/tmp` and `/home` share the RAM limit | by design |
| Terminal start directory: apps started from the desktop open in `$HOME` (round 14 fix) | built; not verified in a session |
