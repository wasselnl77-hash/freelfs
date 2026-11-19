# FreeLFS command reference

Every terminal command a FreeLFS user can run, what it does, and the options it takes. It is a companion to the manual: where a topic needs more room, the entry links to the chapter in [docs/manual](manual/README.md).

FreeLFS is pre-alpha. Commands, options and file locations can change between rounds. Nothing here is a promise.

## How to read this reference

Each entry has these parts:

- **Usage** is the line the program prints when it is called wrongly. It is copied from the source, so it is exact.
- **Options** lists every flag and subcommand with a one-line explanation.
- **Examples** shows one to three typical calls.
- **Needs** says what the command asks for. *Nothing* means any user can run it. *Group seat* means it runs as root for a person in group `seat`; whether it also asks the password depends on the action (manual chapter 15.4: removable drives, power and the lock screen do not, packages, time, snapshots, internal disks and the installer do). *Password* means it asks the person's password first (through `flauth`). *Root* means it has to be run as root.
- **Logs** says where its output or messages go. "Terminal" means the output is only printed.

Where the source did not show something, the entry says *not yet verified*.

Facts were checked against the source tree (`src/`, `recipes/`, `config/`, `build/`) as it stood on 2026-11-17. The `flpkg` section reflects the working tree at that date, which another change was editing at the time.

## Contents

1. [Packages](#1-packages): distrobox, flpackages, flpkg, flpkg-seat, flstore, flupdate-notify
2. [System](#2-system): efibootmgr, fl-sntp, flauth, flboot, flboot-install, flinstall, flmount, flpersist, flpower, flsnap, flsnap-notice, flsocketd, flsplash, flsplash-set, fltime-set
3. [Hardware](#3-hardware): fldisplay, flsound
4. [Desktop](#4-desktop): flcomp, flgreet, flnotify, flpanel, fllock, flportal, flsettings, flsetup, fltheme
5. [Apps](#5-apps): flarchive, fledit, flfiles, flmusic, flpdf, flsh, flterm, flview, flvideo, freebws, freebws-abpconv, snappel
6. [Debugging and developer tools](#6-debugging-and-developer-tools): dssl2css, fl-drawtest, flcrash, flreport, general-manager
7. [Programs the system runs (not for typing)](#7-programs-the-system-runs-not-for-typing)
8. [Common standard commands](#8-common-standard-commands): network, hardware, printing, sound, Flatpak, containers, power, logs
9. [Services: start, stop and enable (rc scripts)](#9-services-start-stop-and-enable-rc-scripts)
10. [Environment variables](#10-environment-variables)
11. [Kernel boot parameters](#11-kernel-boot-parameters)
12. [Where settings and logs live](#12-where-settings-and-logs-live)

---

## 1. Packages

### distrobox

Containers of other distributions (Ubuntu, Arch, Fedora) with their apps exported to the start menu. FreeLFS ships it as a set of shell scripts. Normally you do not run it by hand: `flpkg install --from=apt|pacman|dnf NAME` creates the container and installs the package for you.

- **Usage:** upstream distrobox usage (`distrobox --help`). FreeLFS uses these subcommands itself: `create`, `enter`, `list`, `upgrade`, `export`.
- **Needs:** `podman` (package dependency, round 24a: podman, crun, conmon, passt/pasta, netavark, fuse-overlayfs). Nothing runs in the background.
- **Create by hand:** use `distrobox create --unshare-process ...`. FreeLFS has no systemd, so containers run without their own cgroup (`cgroups = "disabled"` in `/etc/containers/containers.conf`) and Distrobox's default `--pid host` is refused; `flpkg` adds `--unshare-process` itself.
- **Examples:**
  ```
  distrobox list
  distrobox enter -n freelfs-apt
  ```
- **Logs:** Terminal.
- **See also:** [flpkg](#flpkg) (the `--from=` sources), manual [13](manual/13-packages-flpkg.md).

### flpackages

The Packages app: a window that lists installed packages and lets you search, install and remove. It runs flpkg through `flpkg-seat`, so it asks for your password once.

- **Usage:** `flpackages` (no command-line options). The source reads `FLPACKAGES_TAB=flathub` to open on the Flathub tab, and `FLPACKAGES_CONF`, `FLPACKAGES_FLPKG`, `FLPACKAGES_FLATPAK`, `FLPACKAGES_SEAT` to point at other programs (for tests).
- **Needs:** Nothing to start. Installing or removing asks for the password through flauth.
- **Logs:** Shown in the window. Not yet verified beyond that.

### flpkg

The package manager of the FreeLFS repository. It installs, removes and updates packages, checks signatures, keeps a database of what is installed, and takes a btrfs snapshot before each change (through `flsnap`). It also builds packages from recipes.

There are two layers. The command you type is `flpkg`. For most names it first asks the **sources front end** (`/usr/lib/flpkg/sources.sh`), which also knows Flathub, Nix and the distrobox containers. The FreeLFS repository is the first source, and the real package work is done by the native `flpkg` program. Some options skip the front end (see the known gaps listed under flpkg below).

- **Usage:**
  ```
  usage: flpkg [--root DIR] [--config FILE] COMMAND [ARGS]
  ```
  Commands are listed below. Run `flpkg help` for the summary, `flpkg version` for the version.

**Global options** (before the command):

| Option | Meaning |
|---|---|
| `--root DIR` | work on another root directory (for example a chroot). The front end is skipped |
| `--config FILE` | use another configuration file instead of `/etc/flpkg.conf`. The front end is skipped |
| `-v`, `--verbose` | more output (repeat for more) |

**Commands:**

| Command | Pacman alias | What it does |
|---|---|---|
| `install NAME...` | `-S`, `-U` | install packages and their dependencies. `FILE.flpkg` installs a file you have |
| `remove NAME...` | `-R`, `-Rs`, `-Rc` | remove packages |
| `upgrade` | `-Su` | upgrade every installed package that has a newer version in the repos. `-Syu` does a sync first |
| `sync` | `-Sy` | refresh the repo indexes (the signed index is checked before it is used) |
| `search TEXT...` | `-Ss` | search names and summaries in the synced indexes. Every word must match |
| `check-updates [--offline] [--count]` | none | list what `upgrade` would do, one tab-separated line per package. Needs no root |
| `list [--explicit\|--deps]` | `-Q` | list installed packages (`name version`) |
| `info PKG` | `-Qi` | show an installed package |
| `info --repo PKG` | `-Si` | show a package from the repo |
| `info -f FILE.flpkg` | none | show a package file you have |
| `files PKG` | `-Ql` | list the files of an installed package |
| `files -f FILE.flpkg` | none | list the files of a package file |
| `owns PATH...` | `-Qo` | which package owns a file (directories too) |
| `verify [PKG...]` | `-Qk` | check installed files (size, sha256, mode, owner, symlink target) |
| `build [options] [DIR]` | none | build the recipe in DIR into a package (see below) |
| `repo add [options] DIR [FILE.flpkg...]` | none | copy packages into a repo directory and sign its index |
| `key generate PATH` | none | make a signing key: `PATH.key` (secret) and `PATH.pub` |
| `key introduce --key OLD.key --new NEW.pub [--retire-days N] DIR` | none | write a key rotation statement, signed by the old key |
| `key list` | none | the trusted keys and when each one retires |
| `fix-owners [--keep]` | none | apply owners and modes that a rootless build could not set |
| `snapshots` | none | list the btrfs snapshots (runs `flsnap list`) |
| `history` | none | past transactions |
| `version` | none | print the version |
| `help` | none | print the usage summary |

**Front-end commands** (`install`, `remove`, `upgrade`, `search`, `list`, used by a person at a terminal):

- `flpkg install [--from=SOURCE] [--yes] NAME...` (the front end's own usage line: `usage: flpkg install [--from=SOURCE] NAME...`)
- `flpkg remove [--from=SOURCE] NAME...`, which removes from the source that installed NAME
- `flpkg upgrade` runs each source's own update
- `flpkg search TEXT` searches all sources (FreeLFS and Flathub first)
- `flpkg list` lists what is installed from every source

| Source | Name for `--from=` | What it is |
|---|---|---|
| FreeLFS repository | `freelfs` | the native flpkg (needs the password for changes) |
| Flathub | `flatpak` (or `flathub`) | apps, installed per user with `flatpak --user` (no root) |
| Nix | `nix` | nixpkgs, when the `nix` package is installed (`flpkg install nix`, recipes/nix) and its service runs: `flpkg search --from=nix TEXT`, `flpkg install --from=nix NAME`, remove, list, upgrade. The daemon starts on the first use and stops after 60 s idle (rc service `nix`). |
| Ubuntu | `apt` | apt inside a distrobox container named `freelfs-apt` (image `docker.io/library/ubuntu:24.04`, about 30 MB download, 150 MB on disk) |
| Arch | `pacman` | pacman inside `freelfs-pacman` (`docker.io/library/archlinux`, about 135 MB download, 450 MB on disk) |
| Fedora | `dnf` | dnf inside `freelfs-dnf` (`docker.io/library/fedora:45`, about 75 MB download, 250 MB on disk) |

The order when no `--from=` is given is: FreeLFS, Flathub, Nix, Ubuntu, Arch, Fedora. When several sources have the name, you are asked which one to use (you can type a number, or press Enter for the first). With `--yes` no question is asked. Creating a container is asked first (it downloads an image), unless `--yes` is given.

Which source installed which package is written to `$FLPKG_STATE/sources.list` (by default `~/.local/state/flpkg/sources.list`), one line per package.

**Options for `flpkg install`** (native program):

| Option | Meaning |
|---|---|
| `--yes`, `-y` | do not ask "Proceed?" |
| `--nodeps` | install without resolving dependencies |
| `--asdeps` | mark the packages as dependencies instead of as explicitly installed |
| `--needed` | skip packages already installed at the same version |
| `--overwrite GLOB` | let files matching GLOB be overwritten (takes a value) |
| `--unsigned` | accept a `.flpkg` file without a valid signature |

**Options for `flpkg remove`**: `--yes`/`-y`, `--cascade` (also remove what would be left without a dependency), `--recursive` (also remove dependencies nothing else needs), `--nodeps`.

**Options for `flpkg upgrade`**: `--yes`/`-y`, `--sync` (refresh the indexes first; `-Syu` sets it), `--nodeps`.

**Options for `flpkg check-updates`**: `--offline` (use the indexes from the last `sync` instead of fetching them; it never changes the system), `--count` (print only the number). Exit status: 0 checked, 1 error (no network, bad signature), 2 bad usage.

**Options for `flpkg search` and `flpkg list`** (front end): `--all` shows every source even when the output is not a terminal. Without it, when the output goes to a pipe, the plain FreeLFS output is printed (tools such as the Store rely on this).

**Options for `flpkg build`:**

| Option | Meaning |
|---|---|
| `-o DIR`, `--outdir DIR` | where to write the packages (default: the current folder) |
| `-s DIR`, `--sources DIR` | look for source files here first (can repeat) |
| `--nocheck` | skip the recipe's `check()` function (long form only) |
| `--keep` | keep the work directory afterwards (long form only) |
| `-h`, `--help` | print the usage |

**Options for `flpkg repo add`:** `--key FILE` (sign with this key; default `signkey` in the config), `--remove-old` (remove older versions from the index), `--valid-days N` (how long clients accept the index, default 14), `--serial N` (index serial number; default is the next one).

**Options for `flpkg key introduce`:** `--key OLD.key`, `--new NEW.pub`, `--retire-days N` (default 60; a negative number retires the old key at once, for tests).

**Pacman-style aliases.** Only the flags in the table above work. Any other `-X` flag gives `unknown option`.

**Examples**

```
flpkg search text editor
flpkg install fledit
flpkg install --from=flatpak org.mozilla.firefox
flpkg install --from=apt htop
flpkg remove --from=flatpak org.mozilla.firefox
flpkg check-updates --count
flpkg verify fledit
flpkg owns /usr/bin/flpkg
```

**Known gaps in flpkg** (found while reading the source; not yet verified on a running system):

- **Front-end options are dropped.** The front end collects options it does not know (for example `--cascade`, `--recursive`, `--needed`, `--asdeps`) into a list, and never passes that list on. A person who types `flpkg remove --cascade NAME` at a terminal therefore gets a plain remove. `flpkg -Rc NAME` has the same problem. `--overwrite GLOB` is read as a package name in the front end.
- **Some options skip the front end and run as the person.** `--yes`/`-y`, `--nodeps`, `--explicit`, `--deps`, `--unsigned`, `--repo`, and any `.flpkg` file make `flpkg` run the native program directly. That program needs root for changes, so a person who is not root cannot install that way (not yet verified: the exact error).
- **Short forms.** The native `install` accepts only `-y` as a short option. The manual's table mentions `-d`, `-a`, `-o`, `-n`, `-u`; in the source those are not recognized.
- **`key generate` usage.** The usage line in `main.c` says `key generate NAME`, while the code says `key generate PATH` and writes `PATH.key` and `PATH.pub`. This entry follows the code.

**Config file: `/etc/flpkg.conf`.** One `key = value` per line, and `repo NAME URL` lines.

| Key | Default | Meaning |
|---|---|---|
| `cache` | `/var/cache/flpkg` | downloaded packages and indexes |
| `builddir` | `/var/tmp/flpkg` | where recipes are built |
| `keys` | `/etc/flpkg/keys` | trusted public keys |
| `signkey` | none | secret key used by `repo add` |
| `jobs` | number of CPUs | parallel build jobs |
| `snapshots` | not yet verified | a snapshot setting read by flpkg (how it is used is not yet verified; flsnap has its own `keep=`) |
| `compression_level` | 19 | zstd level for packages |
| `backup_config` | yes | a yes/no setting read by flpkg (what it does is not yet verified) |
| `cflags`, `cxxflags`, `ldflags` | none | flags for builds |
| `signatures` | `required` | `required`, `optional` or `never` |
| `source_date_epoch` | 1767225600 | fixed timestamp for reproducible builds |

**Environment variables for flpkg:** `FLPKG_NO_SOURCES=1` (skip the front end), `FLPKG_SOURCES` (another front-end script), `FLPKG_NATIVE` (the native program the front end uses), `FLPKG_SEAT` (`flpkg-seat`), `FLPKG_FLATPAK`, `FLPKG_NIX`, `FLPKG_DISTROBOX` (other programs), `FLPKG_STATE` (where `sources.list` lives), `FLPKG_LIVE_DIR` (default `/run/live`), `FLPKG_ASSUME_TTY=1` (treat output as a terminal), `FLPKG_LIBDIR`, `SOURCE_DATE_EPOCH`.

**Needs:** `install`, `remove`, `upgrade`, `sync`, `repo add`, `fix-owners` need root. Through the front end, a person gets a password prompt once (through `flpkg-seat`, then `flauth`). `search`, `list`, `info`, `files`, `owns`, `verify`, `check-updates`, `history`, `version` need nothing. `key generate` and `build` run as the person.

**Logs:** Terminal. Each transaction is recorded in the database (`flpkg history`, `/var/lib/flpkg/db.sqlite`). While a transaction runs, its plan is in `/var/lib/flpkg/journal`. Before each transaction a snapshot is taken (see `flsnap`).

**See also:** manual [13](manual/13-packages-flpkg.md) (packages and repos), [19](manual/19-admin-reference.md), [flpkg-seat](#flpkg-seat), [flstore](#flstore).

### flpkg-seat

The helper behind the Store and the Packages app. It is installed setuid root for group `seat`. It checks the package names, asks for the person's password through `flauth`, and then runs the native `flpkg` as root.

- **Usage:**
  ```
  usage: flpkg-seat install|remove NAME... | flpkg-seat upgrade
  ```
- **Options:** `install NAME...` (up to many names), `remove NAME...`, `upgrade`. Names must be valid package names.
- **Needs:** Group seat, then Password (through flauth, class `pkg`, reused for two minutes by the same program). Not meant to be typed by hand.
- **Examples:** The desktop calls it. By hand: `flpkg-seat upgrade`.
- **Logs:** Terminal, with the native flpkg's output.

### flstore

The Software Store: Flathub apps and FreeLFS packages in one window, with install, update and remove. It reads the Flathub catalogue and the FreeLFS repo.

- **Usage:**
  ```
  usage: flstore [--updates]
  The Software Store: Flathub apps and FreeLFS packages.
  ```
- **Options:**
  - `--updates`: open on the Updates tab (this is what the update notice's button runs).
- **Needs:** Nothing to open. Installing asks for the password through `flpkg-seat`.
- **Logs:** Shown in the window. Not yet verified beyond that.
- **See also:** [flupdate-notify](#flupdate-notify), manual [13](manual/13-packages-flpkg.md).

### flupdate-notify

Checks once a day for updates (FreeLFS repo and Flathub) and posts one notification. It never installs anything. The desktop session starts it.

- **Usage:** No usage line. Options: `--once` (check now and exit).
- **Options:** `--once` as above. Test hooks are listed under [Environment variables](#10-environment-variables).
- **Timing:** first check 300 s after login (`FLUPDATE_FIRST`), then every 86400 s (`FLUPDATE_INTERVAL`). A failed check is retried after 3600 s (`FLUPDATE_RETRY`).
- **Needs:** Nothing. It refuses to run as root.
- **Logs:** A notification. The notification id is kept in `$XDG_RUNTIME_DIR/flupdate-notify.id`. Clicking the notice runs `flstore --updates`.

---

## 2. System

### efibootmgr

Lists and edits the firmware's boot entries (upstream tool, installed at `/usr/sbin/efibootmgr`). FreeLFS does not need it for normal boots: flboot is installed as the default file `EFI/BOOT/BOOTX64.EFI`, which firmware starts without any boot entry.

- **Usage:** upstream (`efibootmgr --help`). Not yet verified against this build.
- **Examples:**
  ```
  efibootmgr -v
  ```
- **Needs:** Root. Works only on a UEFI system.
- **Logs:** Terminal.

### fl-sntp

The clock client. It asks time servers over SNTP and sets the clock. The `ntp` service runs it as a daemon. Settings > Date & time switches it on and off.

- **Usage:**
  ```
  usage: fl-sntp [-1] [-q SERVER[:PORT]] [-c CONF] [-s STATUSFILE]
  ```
- **Options:**

| Option | What it does |
|---|---|
| (none) | daemon: ask the servers, set the clock, and ask again later |
| `-1` | ask once, set the clock, and exit |
| `-q SERVER[:PORT]` | ask one server and print the offset and delay in milliseconds. The clock is not touched, and no privileges are needed |
| `-c FILE` | another configuration file (default `/etc/fltime.conf`) |
| `-s FILE` | another status file (default `/run/fltime.status`) |

- **Configuration:** `/etc/fltime.conf` with `NTP=0` or `NTP=1`, and `SERVERS="a b"` (default `pool.ntp.org`, each `HOST` or `HOST:PORT`).
- **Examples:**
  ```
  fl-sntp -q pool.ntp.org
  cat /run/fltime.status
  ```
- **Needs:** Root for setting the clock. `-q` needs nothing.
- **Logs:** `/run/fltime.status` (`state=synced` or `waiting`, `last=`, `offset_ms=`, `server=`). The file is removed when the daemon stops.

### flauth

The password prompt for root actions. It checks your password against `/etc/shadow` (with `crypt()`), shows a small window, and then either exits or runs a command for you. A right password gives a stamp for one action class (`--class`) and for the program that asked, valid two minutes (`/run/flauth/ok-UID.CLASS`). Destructive classes (`disk-write`, `persist-create`, `snap-rollback`, `snap-delete`) are one-shot. An empty password is refused, except for the live session's `tester` on the live system. See `docs/SECURITY.md` section 5.

- **Usage:**
  ```
  usage: flauth --class CLASS [--reason TEXT] [-- CMD ARGS...]
  ```
- **Options:**
  - `--class CLASS` (required): the kind of action the stamp is for: `pkg`, `time`, `splash`, `mount-internal` (reused for two minutes by the same program), or `disk-write`, `persist-create`, `snap-rollback`, `snap-delete` (one-shot).
  - `--reason TEXT`: the sentence the dialog shows. Default: "This needs your permission."
  - `--` followed by `CMD ARGS...`: run that command as you, after the password is right.
- **Exit codes:** 0 right (or the command ran), 1 wrong password, 2 cancelled, 3 locked out for a while (three wrong passwords in a row lock flauth for 30 s, then longer, up to 10 minutes), broken or misused, 4 no password can unlock this account, or the account has no password (set one first).
- **Examples:**
  ```
  flauth --class pkg --reason "Install a printer." -- /usr/sbin/lpadmin -h
  flauth --class snap-rollback --reason "Start an earlier state." -- /usr/bin/flsnap rollback 3
  ```
- **Needs:** Nothing (it is setuid root for group seat, mode 4750). The live session's tester (empty password, `/run/live` present) gets no dialog. An empty password anywhere else is refused.
- **Logs:** None written. The stamp is in `/run/flauth`.

### flboot

FreeLFS's own UEFI boot loader and boot menu. There is no command line for it; you use it at boot. It lives on the EFI system partition at `/boot/EFI/BOOT/BOOTX64.EFI` (`BOOTAA64.EFI` on aarch64). Its menu lists:

- every kernel on the partition (`/boot/vmlinuz-*`), newest first; the LTS kernel comes after the others, marked as the fallback;
- the entries in `/boot/flboot/flboot.conf`;
- **Safe graphics**: the newest kernel with `nomodeset nouveau.modeset=0`. Use it when the screen fails to come up with a GPU driver;
- **Start an earlier state**: shows the btrfs snapshots (only when there are some) and starts one of them read-only;
- **Firmware setup (BIOS)**, when the firmware supports it;
- **Other drives and systems**: boot options from the firmware and other FAT volumes with a boot file (for example Windows).

Keys in the menu: Up/Down (or j/k) choose, Enter starts, Esc goes back from the snapshot screen. Any key stops the countdown.

**`/boot/flboot/flboot.conf`**, one setting per line, `#` starts a comment:

| Line | Meaning |
|---|---|
| `timeout SECONDS` | countdown. `0` starts the default at once, `-1` waits for a key |
| `cmdline OPTIONS...` | kernel command line for every kernel found |
| `default TEXT` | kernel version or entry title to pre-select |
| `entry TITLE\|PATH\|OPTIONS` | an extra entry. PATH is on the partition, with `\` separators |

The installed system has no initramfs, so `root=` must name the root partition, for example `root=PARTUUID=...`.

**To undo a broken update:** start the snapshot from before it ("Start an earlier state"), check that all is well, then make it the current state with `flsnap rollback` (or from the notice on the desktop, or Settings > Snapshots) and restart.

- **Needs:** Nothing at the menu. Installing or changing the file needs root (see `flboot-install`).
- **Logs:** None. The menu runs before any system is up.
- **See also:** manual [11](manual/11-boot-process.md), [flboot-install](#flboot-install), [flsnap](#flsnap).

### flboot-install

Puts flboot on the EFI system partition. flpkg runs it again whenever the flboot package changes.

- **Usage:**
  ```
  usage: flboot-install [--esp DIR] [--update]
  ```
- **Options:**
  - `--esp DIR`: the partition's mount point (default `/boot`).
  - `--update`: used by flpkg's trigger. Does nothing when there is no EFI partition.
- **What it does:** copies `/usr/lib/flboot/flboot.efi` to `DIR/EFI/BOOT/BOOTX64.EFI` (or `BOOTAA64.EFI`), and writes `DIR/flboot/flboot.conf` the first time, with the running root file system as `root=`. It never overwrites an existing `flboot.conf`.
- **Examples:**
  ```
  sudo flboot-install
  sudo flboot-install --esp /mnt/esp
  ```
- **Needs:** Root.
- **Logs:** Terminal.

### flinstall

The graphical installer ("FreeLFS install"): language, keyboard, Wi-Fi, disk, account, time zone, then installs to the disk. It runs `flinstall-helper` (its root helper) for the disk work. It erases the disk you choose. Read manual [03](manual/03-installing.md) first.

- **Usage:**
  ```
  usage: flinstall
  The FreeLFS installer (needs flinstall-helper)
  ```
- **Options:** none. Test hooks are listed under [Environment variables](#10-environment-variables) (`FLINSTALL_*`).
- **Needs:** Starts from the desktop or the live system. Disk work is done by `flinstall-helper`, which runs as root for group seat.
- **Logs:** Shown in the window. Not yet verified beyond that.

*flinstall-helper* is the root part of the installer. Its usage line is:

```
usage: flinstall-helper [--dry-run] [--image FILE [--source DIR] [--kernel FILE] [--iwd DIR] [--workdir DIR]]
```

Do not run it by hand on a machine you use. `--dry-run` shows the plan and changes nothing.

### flmount

Mounts, unmounts and ejects removable drives, and mounts them under `/drives`. The udev rule (`90-flmount.rules`) and the Files app (`flfiles`) call it. Internal disks are left alone unless you say `--internal`.

- **Usage:**
  ```
  usage: flmount list | mount DEV | unmount DEV | remount DEV | eject DEV   (--internal: also internal disks)
  ```
- **Subcommands:**

| Subcommand | What it does |
|---|---|
| `list` | show the drives and partitions it knows, and whether they are mounted |
| `mount DEV` | mount `DEV` (for example `/dev/sdb1`) under `/drives/NAME` |
| `unmount DEV` | unmount it |
| `remount DEV` | change its mount options (for example to read-write) |
| `eject DEV` | unmount and power the drive down so it can be removed |

- **Options:** `--internal` (before the subcommand): also treat internal disks as drives.
- **Examples:**
  ```
  flmount list
  flmount mount /dev/sdb1
  flmount eject /dev/sdb
  ```
- **Needs:** Group seat (setuid root, mode 4750). No password for removable drives and the live boot stick; an internal disk (`--internal`) asks for the password every time (flauth class `mount-internal`).
- **Logs:** Terminal, and desktop notices through `flnotify`. Not yet verified for a log file.
- **See also:** manual [07](manual/07-files-drives-usb.md).

### flpersist

Makes a persistence image on a live USB stick, so the live system keeps your files between boots. It works only when the live system was started from an ISO file on a stick (Ventoy), and it creates `freelfs-persist.img` on the stick.

- **Usage:**
  ```
  usage: flpersist create SIZE [--btrfs] | status
  ```
- **Subcommands:**

| Subcommand | What it does |
|---|---|
| `create SIZE` | make the image. SIZE is at least 64M, for example `4G` or `512M` |
| `create SIZE --btrfs` | the same, with btrfs inside |
| `status` | prints `active` (in use), `ready` (image exists), or `none` |

- **Examples:**
  ```
  flpersist status
  flpersist create 4G --btrfs
  ```
- **Needs:** Group seat (setuid root, mode 4750), and a live session. The password every time (flauth class `persist-create`, one-shot); the live `tester` is not asked.
- **Logs:** Terminal.
- **See also:** manual [07](manual/07-files-drives-usb.md), [02](manual/02-getting-started.md).

### flpower

Power and session actions for the desktop panel: power off, restart, log out, and turning the screen saver on or off. The panel calls it.

- **Usage:**
  ```
  usage: flpower off|restart|logout|saver on|saver off
  ```
- **Subcommands:**

| Subcommand | What it does |
|---|---|
| `off` | shut the machine down |
| `restart` | restart the machine |
| `logout` | end the desktop session (the same as Ctrl+Alt+Backspace) |
| `saver on` / `saver off` | start or stop the screen saver |

- **Examples:**
  ```
  flpower restart
  flpower saver on
  ```
- **Needs:** Group seat (setuid root, mode 4750). No password.
- **Logs:** Terminal. How `off` and `restart` reach the shutdown scripts is not yet verified (see [Services](#9-services-start-stop-and-enable-rc-scripts) for the shutdown order).

### flsnap

Btrfs snapshots of the root file system. Before each package change, flpkg takes a snapshot. You can also make, list, delete and roll back snapshots. It is setuid root for group seat.

- **Usage:**
  ```
  usage: flsnap create [--reason TEXT] [--packages FILE] [--auto]
         flsnap list [--machine] | show N | status
         flsnap delete N... | prune [--keep K]
         flsnap rollback [N]     make snapshot N (default: the one running) the current state
  Snapshots of the root file system (btrfs @snapshots); keep= in /etc/flsnap.conf.
  ```
- **Subcommands and options:**

| Subcommand | What it does |
|---|---|
| `create [--reason TEXT] [--packages FILE] [--auto]` | take a snapshot now. `--reason` is the note shown in the list (default "manual"). `--packages` takes a file that lists the packages. `--auto` is for hooks: it does nothing on a system without the layout, and only warns if the snapshot fails |
| `list [--machine]` (also `ls`) | list the snapshots, newest first. `--machine` prints tab-separated fields: number, epoch, date, reason, package count, booted (1 or 0) |
| `show N` | details of snapshot N |
| `status` | the state of the snapshot setup and what the system runs from |
| `delete N...` (also `rm`) | delete snapshots. Asks for the password |
| `prune [--keep K]` | delete the oldest snapshots, keeping K (1 to 99; default from `keep=` in `/etc/flsnap.conf`). Asks for the password |
| `rollback [N]` | make snapshot N the current state. Without N it uses the snapshot the system is running from. Restart to use it. Asks for the password |

- **Examples:**
  ```
  flsnap create --reason "before a big test"
  flsnap list
  flauth -- /usr/bin/flsnap rollback 12
  ```
  (Calling `flsnap rollback 12` directly asks for the password on its own, through `flauth`.)
- **Needs:** Group seat. `create`, `list`, `show`, `status` need nothing else. `rollback` and `delete`/`rm` need the password every time (one-shot), `prune` is for root only.
- **Logs:** Terminal. Each snapshot has an information file in `/.snapshots` (exact name not yet verified). Settings > Snapshots shows the same list.
- **See also:** manual [09](manual/09-troubleshooting.md) ("Going back to an earlier state"), [flsnap-notice](#flsnap-notice).

### flsnap-notice

When the desktop starts from an earlier state (a snapshot), it says so with a button that makes that state the current one. The session starts it once; on a normal boot it does nothing.

- **Usage:** none. Run with no arguments.
- **Needs:** Nothing.
- **Logs:** A notification.

### flsocketd

A small program that owns a Unix socket and starts a real service the first time something connects to it. FreeLFS uses it for printing (`/run/cups/cups.sock`), so CUPS sleeps until a job or `lp` needs it.

- **Usage:**
  ```
  usage: flsocketd [-m MODE] [-g GROUP] SOCKET -- COMMAND [ARGS...]
  ```
- **Options:**
  - `-m MODE`: permissions of the socket, in octal (default `0666`).
  - `-g GROUP`: group of the socket.
- **Examples:**
  ```
  flsocketd -m 0660 -g seat /run/example.sock -- /usr/libexec/example-daemon
  ```
- **Needs:** Root for the system service. Not for typing.
- **Logs:** Output goes where the caller sends it (the cups service sends it to `/dev/null`).

### flsplash

The boot screen. It runs as root from `rc.sysinit`, draws the logo on the screen (no compositor), and hands the screen over to the login screen. Normally you do not run it.

- **Usage:**
  ```
  usage: flsplash [--handover | --text | --render MS W H FILE.ppm [flat|legacy]]
  ```
- **Options:**

| Option | What it does |
|---|---|
| (none) | start the boot screen. It forks into the background and exits 0 whatever happens |
| `--handover` | wait for the animation to end, then let go of the screen for the login screen |
| `--text` | switch to the boot text at once (also puts the console back to text mode) |
| `--render MS W H FILE.ppm [flat\|legacy]` | write one picture of the boot screen at MS milliseconds, on a W x H screen, to a file. Used for screenshots and tests |

- **Style:** `splash=flat` (default) or `splash=legacy` in `/etc/freelfs/splash.conf`, changed with `flsplash-set`.
- **Keyboard:** Esc shows the boot text.
- **Kernel option:** `nosplash` turns the boot screen off (see [Kernel boot parameters](#11-kernel-boot-parameters)).
- **Needs:** Root.
- **Logs:** `/run/flsplash.log` (the boot steps that failed). The state files are `/run/flsplash.pid`, `.done`, `.released`.

### flsplash-set

Changes the boot screen style between `flat` and `legacy`. The Themes app calls it. Changing the style asks for your password.

- **Usage:**
  ```
  usage: flsplash-set flat|legacy
  ```
- **Options:** `flat` or `legacy` only.
- **Examples:**
  ```
  flsplash-set legacy
  ```
- **Needs:** Group seat, then Password (through `flauth`). It writes `/etc/freelfs/splash.conf`.
- **Exit codes:** 0 written, 2 bad argument, 1 failed, 77 the password is needed and flauth is missing, otherwise flauth's own code.
- **Logs:** Terminal.

### fltime-set

The few privileged time actions that Settings does: time zone, NTP on or off, sync now, and setting the clock by hand. It accepts exactly these four commands and checks every argument.

- **Usage:**
  ```
  usage: fltime-set zone NAME | ntp on|off | sync | time EPOCH
  ```
- **Subcommands:**

| Subcommand | What it does |
|---|---|
| `zone NAME` | set the time zone, for example `Europe/Amsterdam` (writes `/etc/localtime` and `/etc/timezone`). NAME must be a real zone file |
| `ntp on` / `ntp off` | switch NTP on or off (in `/etc/fltime.conf`) and start or stop the `ntp` service |
| `sync` | ask the `ntp` service to synchronise now |
| `time EPOCH` | set the clock to EPOCH (seconds since 1970). Only works while NTP is off |

- **Exit codes:** 0 done, 2 usage, 3 refused argument, 4 refused (NTP is on), 1 failed.
- **Examples:**
  ```
  fltime-set zone Europe/Amsterdam
  fltime-set ntp on
  ```
- **Needs:** Group seat (setuid root, mode 4750). The password for every command (flauth class `time`, reused for two minutes); the live `tester` is not asked.
- **Logs:** Terminal, and `/run/fltime.status` for NTP.

---

## 3. Hardware

### fldisplay

Lists and changes the screens (outputs) through the compositor, and sets the night light. Changes apply at once and are saved in the desktop settings. There is no automatic revert: if you do not keep a change, run `fldisplay revert`.

- **Usage:**
  ```
  usage: fldisplay list | set NAME [mode=WxH@HZ] [scale=S] [pos=X,Y] [enabled=0|1] [primary=1]
         fldisplay revert | nightlight on|off|auto [temp=K] [from=HH:MM] [to=HH:MM]
  ```
- **Subcommands and options:**

| Subcommand | What it does |
|---|---|
| `list` | one tab-separated line per output: name, connected or disconnected, current mode (or `-`), scale, position (X,Y), enabled or disabled, primary or `-`, and the list of modes |
| `set NAME ...` | change one output. `mode=WxH@HZ` (a mode from the list), `scale=1\|1.25\|1.5\|1.75\|2`, `pos=X,Y` (logical pixels), `enabled=0\|1`, `primary=1` |
| `revert` | put back the settings from before the last change |
| `nightlight on\|off\|auto` | warm the colours at night. `temp=2700..6500` (kelvin), `from=HH:MM` and `to=HH:MM` (the times for `auto`) |

- **Examples:**
  ```
  fldisplay list
  fldisplay set HDMI-A-1 mode=1920x1080@60 scale=1.25
  fldisplay nightlight auto temp=3500 from=21:00 to=07:00
  ```
- **Needs:** Nothing. It talks to the running compositor (flcomp) and needs a desktop session.
- **Logs:** Saved in the desktop settings (`desktop.conf`, see [section 12](#12-where-settings-and-logs-live)). Errors on the terminal.

### flsound

Plays the desktop's system sounds (login, notification, error, tick, screenshot and others). The compositor runs it for sound effects. It also lets you write your own sound into a sound name.

- **Usage:**
  ```
  usage: flsound [--force] NAME | flsound --write NAME FILE.wav | flsound --list
  ```
- **Options:**

| Option | What it does |
|---|---|
| `NAME` | play the sound called NAME. Does nothing when sounds are turned off in the settings |
| `--force` | play it even when sounds are off |
| `--list` | list the sound names |
| `--write NAME FILE.wav` | make a WAV file the sound called NAME |

- **Examples:**
  ```
  flsound --list
  flsound --force notification
  ```
- **Needs:** A working sound output (PipeWire). Nothing else.
- **Logs:** Terminal.

---

## 4. Desktop

### flcomp

The FreeLFS Wayland compositor: it draws the desktop, handles windows and input, and starts the panel and the session services. A person starts it through the login screen, or from `tty1` through `/etc/profile.d/desktop.sh`.

- **Usage:**
  ```
  usage: flcomp [--firstboot]
    FLCOMP_HEADLESS=WxH   no display or input, for tests
    FLCOMP_SCREENSHOT=F   write a PPM of the screen to F on SIGUSR1
    FLCOMP_PANEL=CMD      panel program (default flpanel, empty for none)
    FLCOMP_SESSION=CMD    session services (default /usr/lib/flcomp/session, empty for none)
    FLCOMP_SETUP=CMD      program run by --firstboot (default flsetup)
  ```
- **Options:**
  - `--firstboot`: start the first-boot setup (runs `flsetup`) instead of the desktop.
  - `-h`, `--help`: print the usage above.
- **Keys:** Ctrl+Alt+Backspace ends the session at once, with no confirmation. Ctrl+Alt+F1 to F12 switch consoles. See [manual 05](manual/05-keyboard-shortcuts.md).
- **Examples:**
  ```
  flcomp
  FLCOMP_HEADLESS=1280x800 flcomp
  ```
- **Needs:** A session (`seat` for the screen and input; the login screen takes care of this).
- **Logs:** `$XDG_RUNTIME_DIR/flcomp.log`, with a copy in `~/.local/state/freelfs/log/`. The session services log to `$XDG_RUNTIME_DIR/session.log`.
- **See also:** manual [04](manual/04-the-desktop.md), [23](manual/23-compositor-protocols.md).

### flgreet

The login screen. It runs on `tty1` (started by `/etc/inittab`), checks your password, and then starts the desktop. If `flcomp` cannot start, it falls back to a text login.

- **Usage:** none. It is started by init, not typed.
- **Needs:** Root (it does the login).
- **Logs:** not yet verified.
- **Style:** `FLGREET_STYLE` (see [Environment variables](#10-environment-variables)).

### flnotify

Shows a desktop notification from the terminal or a script, and controls "do not disturb", the notification history and the notification centre. It talks to the notification service of the desktop.

- **Usage:**
  ```
  usage: flnotify [-a APP] [-i ICON] [-u low|normal|critical] [-t MS] [-A KEY=LABEL]...
                  [-r ID] [-T] [-w] SUMMARY [BODY]
         flnotify --dnd on|off|toggle|status | --clear | --center | --count
  ```
- **Options:**

| Option | What it does |
|---|---|
| `-a APP`, `--app APP` | name of the application shown in the notice |
| `-i ICON`, `--icon ICON` | icon name |
| `-u low\|normal\|critical`, `--urgency` | urgency |
| `-t MS`, `--expire MS` | how long it stays, in milliseconds |
| `-r ID`, `--replace ID` | replace the notice with this id |
| `-A KEY=LABEL`, `--action` | add a button (can repeat) |
| `-T`, `--transient` | do not keep it in the history |
| `-w`, `--wait` | wait for the notice to close (what it prints then is not yet verified) |
| `--dnd on\|off\|toggle\|status` | "do not disturb" |
| `--clear` | empty the history |
| `--center` | open or close the notification centre |
| `--count` | number of entries in the history |

- **Output:** prints the id of the new notification. Exit 0 on success, 1 on errors (no session bus, no daemon), 2 for bad usage.
- **Examples:**
  ```
  flnotify -a Backup "Backup finished" "All files copied"
  flnotify --dnd on
  ```
- **Needs:** A desktop session (session bus).
- **Logs:** The notification history of the desktop.

### flpanel

The panel at the bottom of the screen: menu, workspaces, clock, tray, volume, battery and notifications. It is started by `flcomp`. The options are for screenshots and tests.

- **Usage:**
  ```
  usage: flpanel [--demo] [--menu] [--volume] [--workspaces] [--cards] [--center] [--battery]
                 [--bluetooth] [--quick]
    --demo  fake windows and a fixed clock (for screenshots)
    --menu  start with the menu open
    --volume  start with the volume flyout open
    --workspaces  demo with the pager's menu open (FLUI_PPM=FILE writes FILE and FILE.N)
    --cards, --center, --battery  demo with notification cards, the centre or the
            battery flyout shown (FLUI_PPM=FILE writes file, file.3, file.4, file.5)
    --bluetooth, --quick  demo with the Bluetooth or the quick settings flyout shown
    --properties PATH  demo with the Properties window of PATH open
  ```
- **Options:** the list above. `--demo` (and the options that imply it) only changes what is shown.
- **Needs:** A desktop session.
- **Logs:** Programs started from the panel write `~/.local/state/freelfs/log/NAME.log`. The panel's own log file name is not yet verified.
- **See also:** manual [04](manual/04-the-desktop.md).

### fllock

The lock screen. The desktop starts it (see `FLCOMP_LOCK` in [section 10](#10-environment-variables)); it asks for your password and unlocks. The password check runs in `fllock-check`, which is setuid for group seat.

- **Usage:**
  ```
  usage: fllock
         fllock --demo [--typed N] [--wrong|--busy|--stuck]   (screenshots; FLUI_PPM=FILE)
  ```
- **Options:** `--demo`, `--typed N` (how many characters are shown as typed), `--wrong`, `--busy`, `--stuck` (show the state after a wrong password, a check in progress, or a stuck check). These are for screenshots.
- **Needs:** A session. The password check is done by `fllock-check` (group seat). Your password is typed in the window.
- **Logs:** None written.
- **See also:** manual [04](manual/04-the-desktop.md) ("The lock screen"), [09](manual/09-troubleshooting.md).

### flportal

The portal service for desktop apps (for example Flatpak apps). It answers file-open and file-save dialogs, the colour scheme, and the Settings portal, so sandboxed apps follow the desktop theme. The session starts it through D-Bus activation.

- **Usage:**
  ```
  usage: flportal [-v]
  ```
- **Options:** `-v` writes more to its log.
- **Needs:** A session bus. Not for typing.
- **Logs:** `~/.local/state/freelfs/log/flportal.log` (collected by `flreport`).

### flsettings

Settings. It has one page for each area of the system. It opens on the page you ask for.

- **Usage:**
  ```
  usage: flsettings [--page general|display|wallpaper|mouse|keyboard|workspaces|datetime|wifi|bluetooth|printers|snapshots|about]
         flsettings --set-wallpaper FILE.png|FILE.jpg
  Theme, panel labels, display, mouse and touchpad, keyboard, workspaces, date and time, Wi-Fi, Bluetooth, printers
  ```
- **Options:**

| Option | What it does |
|---|---|
| `--page NAME` | open on NAME. Accepted names: `general` (default), `display`, `wallpaper`, `mouse` (also `input`, `touchpad`), `keyboard`, `workspaces` (also `workspace`, `desktops`), `datetime` (also `time`, `clock`, `timezone`), `wifi` (also `network`), `bluetooth` (also `bt`), `printers` (also `printer`), `snapshots` (also `snapshot`, `rollback`), `about` (also `report`) |
| `--set-wallpaper FILE` | set the wallpaper from a PNG or JPEG file, without opening a window (the image viewer uses this) |

- **Examples:**
  ```
  flsettings --page wifi
  flsettings --page snapshots
  flsettings --set-wallpaper ~/pictures/hill.jpg
  ```
- **Needs:** A desktop session. Some pages make changes that need the group seat or a password (see the page itself).
- **Logs:** `~/.local/state/freelfs/log/flsettings.log` when started from the panel. Settings are kept in `desktop.conf`.
- **See also:** manual [08](manual/08-settings-reference.md).

### flsetup

The first-boot keyboard choice. It writes the keyboard layout the desktop uses, then exits. `flcomp --firstboot` runs it.

- **Usage:**
  ```
  usage: flsetup
  Choose the keyboard layout and write <path>
  ```
  (The usage text prints the path of the keyboard config file it writes.)
- **Options:** none. `-h` and `--help` print the usage.
- **Needs:** A desktop session (the setup runs in one).
- **Logs:** Terminal.

### fltheme

Themes: the colour scheme, the accent, fonts, window buttons, the panel, animations and the boot screen style, saved as presets. This is also the command line for the same settings.

- **Usage:**
  ```
  usage: fltheme [--page presets|colours|fonts|windows|panel|animations]
         fltheme --hints | --list | --reset | --apply NAME | --save NAME
  Colour scheme, accent, fonts, window buttons, panel, animations, presets
  ```
- **Options:**

| Option | What it does |
|---|---|
| `--page NAME` | open the Themes window on that page. Names: `presets`, `colours` (also `colors`, `accent`, `wallpaper`), `fonts`, `windows`, `panel`, `animations` (also `animation`), `boot` |
| `--hints` | write the colour scheme and UI font for GTK programs (`settings.ini` in `gtk-3.0` and `gtk-4.0`), which Flatpak apps read |
| `--list` | list the built-in presets (a `*` marks the one in use) and your own presets |
| `--reset` | go back to the default theme |
| `--apply NAME` | apply a preset |
| `--save NAME` | save the current theme as a preset named NAME |

- **Examples:**
  ```
  fltheme --list
  fltheme --apply "my blue"
  fltheme --save "my blue"
  ```
- **Needs:** A desktop session for `--page`. The other options work from a terminal too. Changing the boot screen asks for the password (see `flsplash-set`).
- **Logs:** Settings are saved in `desktop.conf` and the GTK settings files.
- **See also:** manual [08](manual/08-settings-reference.md).

---

## 5. Apps

### flarchive

Archive manager: opens, extracts and creates archives (zip, tar and its compressed forms, 7z, rar and more).

- **Usage:**
  ```
  usage: flarchive [ARCHIVE]
         flarchive --extract-here ARCHIVE...
         flarchive --compress FILE...
         flarchive --new
  Opens, extracts and creates zip, tar.*, 7z, rar and more.
  ```
- **Options:**

| Option | What it does |
|---|---|
| `ARCHIVE` | open the window on this archive |
| `--extract-here ARCHIVE...` | extract each archive next to itself (whether it shows a window first is not yet verified) |
| `--compress FILE...` | make a new archive from these files (the window behaviour is not yet verified) |
| `--new` | open the window with a new, empty archive |

- **Examples:**
  ```
  flarchive photos.zip
  flarchive --extract-here backup.tar.gz
  ```
- **Needs:** A desktop session for the window.
- **Logs:** Terminal, and the desktop log of the program.

### fledit

The text editor. Ctrl+S saves, Ctrl+O opens, Ctrl+F finds, Ctrl+G goes to a line.

- **Usage:**
  ```
  usage: fledit [FILE...]
  A small text editor. Ctrl-S saves, Ctrl-O opens, Ctrl-F finds, Ctrl-G goes to a line.
  ```
- **Options:** One or more files to open.
- **Examples:**
  ```
  fledit ~/notes.txt
  fledit ~/.config/flsh/flshrc
  ```
- **Needs:** A desktop session.
- **Logs:** The state folder is `$XDG_STATE_HOME` (see the source for the file it writes).

### flfiles

The file manager (Files). It shows folders, drives in `/drives`, and opens files with the right app. Ctrl+H shows hidden files.

- **Usage:**
  ```
  usage: flfiles [FOLDER]
  Browse files. Ctrl-H shows hidden files.
  ```
- **Options:** the folder to open first (default: your home folder).
- **Examples:**
  ```
  flfiles ~/downloads
  flfiles /drives
  ```
- **Needs:** A desktop session. Drives appear through `flmount`.
- **Logs:** Terminal, and the desktop log.
- **See also:** manual [07](manual/07-files-drives-usb.md).

### flmusic

A music player: a library of your music folder, with albums, artists and songs. Space plays and pauses. It can be controlled from media keys through MPRIS.

- **Usage:**
  ```
  usage: flmusic [--tab albums|artists|songs] [--search TEXT] [FOLDER | FILE...]
         flmusic --ctl play|pause|play-pause|stop|next|previous|status
  A library of ~/music (or FOLDER). Space plays and pauses. The --ctl form talks to a running
  player over MPRIS (for media keys).
  ```
- **Options:**

| Option | What it does |
|---|---|
| `--tab albums\|artists\|songs` | the first tab to show |
| `--search TEXT` | start with this search |
| `FOLDER` | use this folder as the library (instead of `~/music` or `~/Music`) |
| `FILE...` | play these files (up to 64) |
| `--ctl ACTION` | control a running player: `play`, `pause`, `play-pause`, `stop`, `next`, `previous`, `status` |

- **Examples:**
  ```
  flmusic
  flmusic --ctl next
  flmusic --tab artists ~/music
  ```
- **Needs:** A desktop session for the window. `--ctl` needs a running player.
- **Environment:** `FLMUSIC_DIR` (the library folder), `FLMUSIC_SINK` (the audio output).
- **Logs:** Terminal.

### flpdf

The document viewer: PDF, XPS, EPUB and CBZ. Ctrl+F finds, Ctrl+wheel zooms, Q quits.

- **Usage:**
  ```
  usage: flpdf [--page N] [--zoom PERCENT] [--find TEXT] [--no-contents] FILE
  View PDF, XPS, EPUB and CBZ files. Ctrl+F finds, Ctrl+wheel zooms, Q quits.
  ```
- **Options:**

| Option | What it does |
|---|---|
| `--page N` | open on page N (starts at 1) |
| `--zoom PERCENT` | start at this zoom |
| `--find TEXT` | search for TEXT at once |
| `--no-contents` | do not build the table of contents (faster for big files) |

- **Examples:**
  ```
  flpdf manual.pdf --page 12
  flpdf book.epub --find "chapter 3"
  ```
- **Needs:** A desktop session.
- **Logs:** Terminal.

### flsh

The FreeLFS shell. It runs POSIX shell syntax (so scripts and tutorials work), and adds colours, grey suggestions from your history, Tab menus and "did you mean" messages.

- **Usage:**
  ```
  usage: flsh [-ilsc] [-euxfnvCa] [-o OPTION] [SCRIPT [ARG...] | -c COMMAND [NAME [ARG...]]]

    -c COMMAND   run COMMAND and exit
    -s           read commands from standard input
    -i           interactive, even without a terminal
    -l, --login  a login shell: read /etc/profile and ~/.profile
    --version    print the version

  Interactive shells read /etc/flshrc and ~/.config/flsh/flshrc. Type help in flsh for
  the builtins and keys.
  ```
- **Options:** the list above, plus the POSIX shell options (`-e`, `-u`, `-x`, `-f`, `-n`, `-v`, `-C`, `-a`) and `-o NAME` (for example `-o pipefail`). `--` ends the options.
- **Keys:** Tab completes; Up/Down search history by what you typed; Right takes the grey suggestion; Ctrl+R searches all history; Ctrl+A/E go to the start and end; Ctrl+W/U/K delete; Ctrl+D leaves on an empty line; Alt+. inserts the last argument of the previous command. `NO_COLOR` turns colours off.
- **Builtins:** `help` lists all. Those most used: `cd`, `pwd`, `echo`, `export`, `alias`, `history` (`history [N] [clear] [search TEXT]`), `jobs`, `fg`, `bg`, `kill`, `read`, `type`, `command`, `umask`, `ulimit`, `complete` (for writing your own Tab completions), `source`/`.`, `trap`, `wait`.
- **Examples:**
  ```
  flsh -c 'echo hello'
  flsh script.sh one two
  flsh --login
  ```
- **Files:** `/etc/flshrc`, `~/.config/flsh/flshrc`, `/etc/profile`, `~/.profile` (login), history `~/.local/share/flsh/history`, completions `/usr/share/flsh/completions/NAME.flsh` and `~/.config/flsh/completions/NAME.flsh`.
- **Needs:** Nothing.
- **Logs:** The history file (above). Nothing else.

### flterm

The terminal. It runs `flsh` in a window, or the command you give it.

- **Usage:**
  ```
  flterm [-e COMMAND [ARGS...]]
  ```
- **Options:** `-e COMMAND [ARGS...]`: run that command instead of the shell.
- **Keys:** Shift+PageUp/PageDown scroll the 2000-line history; Ctrl+Shift+C or Ctrl+Insert copy; Ctrl+Shift+V, Shift+Insert or middle click paste. The wheel scrolls the history, or sends Up/Down on full-screen programs such as `less`.
- **Examples:**
  ```
  flterm
  flterm -e htop
  ```
- **Needs:** A desktop session.
- **Logs:** Terminal. flterm itself writes no log file that the source shows.

### flview

Picture viewer: PNG, JPEG and WebP. Arrow keys go to the next and previous picture. Delete moves a picture to the Trash. W sets it as the wallpaper.

- **Usage:**
  ```
  usage: flview [--rotate N] [--zoom PERCENT] [--no-strip] [FILE|FOLDER]
  View pictures (PNG, JPEG, WebP). Arrow keys: next, previous. Del: Trash. W: wallpaper.
  ```
- **Options:**

| Option | What it does |
|---|---|
| `--rotate N` | rotate the picture by N (the unit and the effect are not yet verified) |
| `--zoom PERCENT` | start at this zoom (the source calls this a test value; not yet verified for normal use) |
| `--no-strip` | hide the strip at the bottom of the window (the source's name for the thumbnails strip) |
| `FILE` or `FOLDER` | the picture (or the first picture of the folder) |

- **Examples:**
  ```
  flview ~/pictures
  flview --no-strip holiday.jpg
  ```
- **Needs:** A desktop session.
- **Logs:** Terminal.

### flvideo

Video player. Space pauses, F goes full screen, V and A switch subtitles and audio, Ctrl+O or the Open button picks a file.

- **Usage:**
  ```
  usage: flvideo [--start SECONDS] [--pause] [FILE]
  Play a video. Space pauses, F is fullscreen, V and A switch subtitles and audio,
  Ctrl+O or the Open button picks a file.
  ```
- **Options:**

| Option | What it does |
|---|---|
| `--start SECONDS` | start at this position |
| `--pause` | start paused |
| `FILE` | the video. Without it, the window opens and you use the Open button or Ctrl+O |

- **Environment:** `FLVIDEO_HWDEC` (hardware decoding mode), `FLVIDEO_AO` (audio output), `FLVIDEO_DEBUG` (more messages). See the flvideo README for values (not yet verified here).
- **Examples:**
  ```
  flvideo ~/videos/clip.mp4 --start 90
  ```
- **Needs:** A desktop session.
- **Logs:** Player messages go to the terminal (warnings unless `FLVIDEO_DEBUG` is set).

### freebws

The web browser (FreeBWS): WPE WebKit in a window with a toolbar, tabs, bookmarks, downloads, a built-in ad blocker and dark mode. Web pages run in a sandbox. See [manual 27](manual/27-freebws-internals.md) for the internals.

- **Usage:** `freebws [URL]` (the default is `freebws://start`). Other forms:
  - `freebws --sandbox-check`: prints whether the web-process sandbox (bubblewrap) can be used on this machine; exit 0 means yes.
  - `freebws --adblock-update`: update the ad-block lists. The browser runs it in a child process (the exact steps are not yet verified).
  - `freebws --adblock-build`: rebuild the converted lists from the lists on disk.
- **Address bar and keys:** Ctrl+L, F6 or Alt+D select the address bar; F5 or Ctrl+R reloads; Alt+Left and Alt+Right go back and forward; Ctrl+T opens a tab; Ctrl+W closes one; Ctrl+Shift+T reopens the last closed tab; Ctrl+D bookmarks; Ctrl+Shift+B shows the bookmarks bar; Ctrl+Shift+O opens bookmarks. An address without a scheme gets `https://` when it looks like a host; anything else is a search (Brave Search by default).
- **Pages:** `freebws://settings`, `freebws://bookmarks`, `freebws://downloads`.
- **Config:** `~/.config/freebws/config` (keys include `adblock`, `bookmarks_bar`, `dark`, `buffers` (`shm`, `dmabuf` or `auto`), `sandbox` (`auto`, `on` or `off`), `search`, `useragent`, `downloads_dir`, `cookies`, `watchdog_test`).
- **Environment:** the `FREEBWS_*` variables are listed in [section 10](#10-environment-variables).
- **Examples:**
  ```
  freebws https://example.org
  freebws --sandbox-check
  FREEBWS_DEBUG=1 freebws
  ```
- **Needs:** A desktop session. Nothing else.
- **Logs:** `~/.cache/freebws/log` (starts, watchdog actions, crashes, popups). `FREEBWS_DEBUG=1` adds every load event.

### freebws-abpconv

Converts Adblock Plus filter lists into the files FreeBWS uses. The browser calls it when it updates its lists.

- **Usage:**
  ```
  abpconv [-n MAXRULES] OUTDIR LIST...
  ```
  (Installed as `freebws-abpconv`.) It writes `OUTDIR/net-N.json`, `generic.txt` and `site.tsv`, and prints the counts.
- **Options:** `-n MAXRULES` (how many network rules go in each file).
- **Needs:** Nothing. Not for typing in normal use.
- **Logs:** Terminal.

### snappel

The screenshot tool. Full screen, a region, a window, with a timer and a small editor for drawing on the picture. Screenshots are saved in `~/pictures/screenshots` (named `snappel-YYYY-MM-DD-HHMMSS.png`).

- **Usage:**
  ```
  usage: snappel [--full | --region | --window] [--delay SECONDS] [--edit]
    no option  the Snappel window
    --full     the whole screen, saved to ~/pictures/screenshots with a notification
    --region   select an area on the frozen screen
    --window   click a window
    --edit     open the editor with the picture instead of saving it at once
  ```
- **Options:**

| Option | What it does |
|---|---|
| (none) | open the Snappel window |
| `--full` | take the whole screen and save it at once |
| `--region` | select an area |
| `--window` | click a window to capture it |
| `--delay SECONDS` | wait 0 to 60 seconds first (other values are set to 0) |
| `--edit` | open the editor with the picture instead of saving it |
| `--output NAME` | capture this output only (used with `--full`) |

- **Examples:**
  ```
  snappel --full --delay 3
  snappel --region --edit
  ```
- **Needs:** Runs under the compositor (`flcomp`). Outside it, `--full` reports "no screen capture here".
- **Logs:** Terminal, and a notification when the file is saved.

---

## 6. Debugging and developer tools

### dssl2css

Converts DSSL (the FreeLFS layout language, used by FreeBWS pages) into CSS. It is the native front end of the DSSL library.

- **Usage:**
  ```
  usage: dssl2css [options] FILE...      (FILE - is standard input)
    -o FILE         write the output to FILE (default: standard output)
    --json          the resolved document (spec chapter 10) as canonical JSON, one line per file
    --tree          the syntax tree and parser errors (spec chapter 2) as JSON
    --features      list what needs the engine, one line per occurrence; no CSS
    --check         diagnostics only
    --banner        start the CSS with a comment naming the file
    -q              no diagnostics on standard error
    --strict-engine exit with status 3 when a file needs the JavaScript engine
    --version, --help
  Files are one cascade in the order given: tokens and motions flow from file to file.
  Exit status: 0 ok, 1 errors in the DSSL (output is still written), 2 usage or I/O error,
  3 needs the engine (with --strict-engine).
  ```
- **Examples:**
  ```
  dssl2css -o page.css page.dssl
  dssl2css --check styles/tokens.dssl
  ```
- **Needs:** Nothing.
- **Logs:** Diagnostics on the terminal.

### fl-drawtest

Draws a desktop sketch with EGL on the screen, straight through KMS, to test the graphics stack. It takes the screen itself, so stop the desktop first (from a text console).

- **Usage:**
  ```
  usage: fl-drawtest [-t SECONDS] [-o FILE.ppm]
  ```
- **Options:**

| Option | What it does |
|---|---|
| `-t SECONDS` | how long to draw (fractions allowed) |
| `-o FILE.ppm` | save one picture of the drawing to this file |

- **Examples:**
  ```
  fl-drawtest -t 5
  fl-drawtest -o /tmp/drawtest.ppm
  ```
- **Needs:** Root or group seat (the program asks the seat manager for the screen; the seat manager must be running and you must be in its group).
- **Logs:** Terminal.

### flcrash

The "Something went wrong" screen. It appears when the desktop session dies, shows what happened, and offers to restart. The session starts it; its options are for tests and screenshots.

- **Usage:**
  ```
  usage: flcrash --cant PATH...  (exit 0: go ahead, 1: Never mind)
  usage: flcrash --status N [--secs N] [--crashes N] [--prog NAME] [--log FILE]
         flcrash --demo [CODE] [--view log] [--size WxH]  (FLCRASH_PPM=FILE: write a picture)
           CODE: SIGSEGV SIGABRT SIGBUS SIGILL SIGFPE SIGKILL SIGHUP NO-SEAT NO-DISPLAY
                 NO-RUNTIME NO-MEMORY START-FAILED EXIT-<n> RESTART-LOOP
         flcrash --text
  ```
- **Options:** `--demo [CODE]` shows the screen for a made-up crash code (for screenshots). `--text` uses the plain text version. `--size WxH` sets the size. `--view log` shows the log. `--status N` is what the session passes in. `--cant PATH...` asks whether a path can be used (internal).
- **Needs:** Nothing for `--demo`. Not for typing otherwise.
- **Logs:** The log file given by `--log` (for `--status`), and the session log (`$XDG_RUNTIME_DIR/session.log`).

### flreport

Collects what is needed to see why something does not work: logs, kernel messages, devices, settings, mounts and network. It writes one archive you can copy to a USB stick and attach to a bug report. It keeps no passwords and no file contents, except the settings it lists.

- **Usage:**
  ```
  usage: flreport [-o DIR] [-q]
  ```
- **Options:**

| Option | What it does |
|---|---|
| `-o DIR` | write the archive to DIR instead of your home folder |
| `-q` | print nothing except the path at the end |
| `-h`, `--help` | print the usage |

- **Output:** `~/freelfs-report-YYYYMMDD-HHMMSS.tar.gz` (or in DIR). The path is the last line printed.
- **Examples:**
  ```
  flreport
  flreport -o /run/media/ME/STICK -q
  ```
- **Needs:** Nothing. Runs as you. Parts that need more rights are left out.
- **Logs:** It reads `/var/log` (for example `iwd.log`, `dmesg.log`), the session log and the program logs in `~/.local/state/freelfs/log/`.
- **See also:** manual [09](manual/09-troubleshooting.md) ("Reporting a bug").

### general-manager

The task manager (General Manager, also named `flgm`). It shows running processes, the performance of CPU, memory, disk, network and GPU, startup items and users. Ctrl+Shift+Esc opens it from the desktop.

- **Usage:**
  ```
  usage: general-manager [--tab processes|performance|startup|users|details]
                         [--perf cpu|memory|disk|network|gpu] [--sort cpu|memory|disk|name]
                         [--expand APP|all] [--select TEXT] [--search TEXT] [--cores] [--size WxH]
  Ctrl+Shift+Esc opens it from the desktop.
  ```
- **Options:**

| Option | What it does |
|---|---|
| `--tab NAME` | the first tab. The name can be shortened (for example `perf`) |
| `--perf cpu\|memory\|disk\|network\|gpu` | the graph shown on the performance tab |
| `--sort cpu\|memory\|disk\|name` | sort order |
| `--expand APP\|all` | expand one application group, or all of them |
| `--select TEXT` | select a process by this text (what exactly is selected is not yet verified) |
| `--search TEXT` | filter the process list |
| `--cores` | show the CPU cores separately (the view is not yet verified) |
| `--size WxH` | window size |

- **Environment:** `GM_INTERVAL_MS` (refresh interval in milliseconds, at least 50; default 1000).
- **Examples:**
  ```
  general-manager --tab performance --perf memory
  general-manager --search firefox
  ```
- **Needs:** A desktop session.
- **Logs:** Shown in the window.

---

## 7. Programs the system runs (not for typing)

These are installed under `/usr`, `/etc` or `/usr/lib` and are started by the boot, the session or flpkg. Listed so you know what they are when you see them in `ps`. Do not start them by hand unless a note says otherwise.

| Program | Where | Started by | What it does |
|---|---|---|---|
| `flcupsd` | `/usr/libexec/flcupsd` | `flsocketd` (cups service) | starts `cupsd` and avahi on the first connection; stops them after 60 s idle |
| `flgreet-ui` | `/usr/libexec/flgreet-ui` | `flgreet` | the login window itself |
| `flinitrd` | `/usr/lib/flinitrd/init` | the kernel (live ISO) | the first program of the live initramfs: looks for the ISO or stick and starts the live system |
| `flinstall-helper` | `/usr/libexec/flinstall-helper` | `flinstall` | the installer's root part (see section 2) |
| `flpkg recipe.sh` | `/usr/lib/flpkg/recipe.sh` | `flpkg build` | runs a recipe (`recipe.sh dump` or `run`) |
| `flpkg sources.sh` | `/usr/lib/flpkg/sources.sh` | `flpkg` | the front end for all sources (see [flpkg](#flpkg)) |
| `flpkg triggers` | `/usr/lib/flpkg/triggers` | `flpkg` after a transaction | post-install steps (for example `flboot-install --update`) |
| `pre-transaction.d/10-flsnap` | `/usr/libexec/flpkg/pre-transaction.d/` | `flpkg` before a transaction | takes a snapshot first |
| `flcomp session` | `/usr/lib/flcomp/session` | `flcomp` | starts the session services (sound, Flathub setup, notices) |
| `/etc/profile.d/desktop.sh` | `/etc/profile.d/` | login shell on tty1 | starts the desktop (set `FLCOMP_NOSTART=1` to stay in the shell) |
| `flsplash` | `/usr/sbin/flsplash` | `rc.sysinit` | the boot screen (see section 2) |
| `flsocketd` | `/usr/sbin/flsocketd` | `rc` (cups) | see section 2 |
| `fl-sntp` | `/usr/sbin/fl-sntp` | `rc` (ntp) | see section 2 |
| `flnotify` | `/usr/bin/flnotify` | many programs | see section 4 |
| `flsnap-notice` | `/usr/bin/flsnap-notice` | the session | see section 2 |
| `flstore --build-index` | (inside flstore) | the Store | builds its catalogue index. Not for typing |

---

## 8. Common standard commands

These are upstream programs that FreeLFS ships. They work the way their own manual says. The notes say what is FreeLFS-specific. Where the upstream behaviour is not checked against this build, the entry says so.

### Network

- **`ip`** (iproute2). Addresses, routes and links.
  ```
  ip -brief addr
  ip route
  ip link set eth0 up
  ```
  FreeLFS notes: `/etc/rc.d/init.d/network status` runs `ip -brief addr`. Needs root for changes.

- **`iwctl`** (iwd). The Wi-Fi client. Commands are iwd's own (`iwctl --help`); not yet verified against this build. Typical:
  ```
  iwctl device list
  iwctl station wlan0 scan
  iwctl station wlan0 get-networks
  iwctl station wlan0 connect "NETWORK NAME"
  ```
  FreeLFS notes: Settings > Wi-Fi does the same. The daemon is the `iwd` service and logs to `/var/log/iwd.log`. `flreport` collects that log.

- **`dhcpcd`**. DHCP client. FreeLFS runs it through the `network` service (`dhcp`). Run by hand only for debugging, and stop the service first.

- **`resolvconf`** (iwd's stand-in). Writes `/etc/resolv.conf` when iwd gets DNS from DHCP. Not typed by hand.

### Hardware and system information

- **`lspci`** (pciutils). PCI devices, including the graphics card. `lspci -k` shows the driver in use.
- **`lsusb`** (usbutils). USB devices. `lsusb -t` shows the tree.
- **`lsmod`**, **`modprobe`** (kmod). Loaded kernel modules, and loading one: `sudo modprobe NAME`. Only root can load modules.
- **`dmesg`** (util-linux). Kernel messages. Boot messages are also in `/var/log/dmesg.log` (written by `rc.sysinit`). `dmesg | tail -50` is the usual first look.
- **`free -h`**, **`df -h`**, **`ps aux`**, **`uname -a`** (procps-ng, coreutils). Memory, disk space, processes, kernel version. `flreport` uses all of these.
- **`lsblk`**, **`mount`**, **`umount`** (util-linux). Disks and mounts. Not yet verified which util-linux tools are built. For removable drives, use `flmount` (section 2), which mounts under `/drives`.
- **`btrfs`** (btrfs-progs). The btrfs tool. `btrfs subvolume list /` shows the snapshots. Prefer `flsnap` for snapshots.
- **`efibootmgr`**. See section 2.
- **`lspci`, `lsusb`, `dmesg`**: also used by `flreport`, so they are certainly present.

### Bluetooth

- **`bluetoothctl`** (bluez). Pairs and connects devices. Typical session:
  ```
  bluetoothctl
  [bluetooth]# power on
  [bluetooth]# scan on
  [bluetooth]# pair AA:BB:CC:DD:EE:FF
  [bluetooth]# connect AA:BB:CC:DD:EE:FF
  ```
  FreeLFS notes: `bluetoothd` runs only while a Bluetooth adapter exists (the `bluetooth` service and a udev rule start and stop it). Settings > Bluetooth uses `bluetoothctl` too. Log: `/var/log/bluetooth.log`. The commands above are upstream; not yet verified on this build.

### Printing

FreeLFS uses CUPS, started on demand (see the `cups` service in [section 9](#9-services-start-stop-and-enable-rc-scripts)). The first print or printer command starts `cupsd`, and it exits after 60 s idle.

- **`lp`**: print a file. `lp -d PRINTER file.pdf`. Not yet verified that `lp` is linked in `/usr/bin` in this build (upstream CUPS installs it there).
- **`lpstat -p -d`**: list printers and the default one. `lpstat -o` lists jobs.
- **`cancel JOB`** (or `cancel -a`): cancel a job.
- **`lpadmin`**: add or change printers (root). FreeLFS links `lpadmin`, `lpinfo`, `lpmove`, `cupsctl`, `cupsaccept`, `cupsreject`, `cupsenable` and `cupsdisable` into `/usr/bin` (from the cups recipe).
- **`lpinfo -v`**: list devices. `lpoptions`: per-user printer options (not yet verified).

Settings > Printers is the easy way to add a printer. Logs: `/var/log/cups/`.

### Sound

- **PipeWire** and **WirePlumber** run the sound. They start from the desktop session (`flcomp session`). Each one is optional.
- **`wpctl status`** lists the outputs and their volume (WirePlumber). The troubleshooting chapter uses it. Not yet verified in this build (the manual says so too).
- **`flsound`** (section 3) plays the desktop sounds.

### Flatpak

Flathub apps, installed per person with `flatpak --user` (no root). FreeLFS adds the Flathub remote the first time you have a network connection in a session.

```
flatpak --user search firefox
flatpak --user install flathub org.mozilla.firefox
flatpak --user update
flatpak --user uninstall org.mozilla.firefox
flatpak run org.mozilla.firefox
flatpak --user list --app
```

For most things `flpkg install --from=flatpak NAME` does the same. The Store is the graphical front end. Apps live in `~/.local/share/flatpak`. On the live system without persistence, apps are kept in RAM and are gone at shutdown.

### Containers

- **`distrobox`**: see [section 1](#distrobox). Containers are named `freelfs-apt`, `freelfs-pacman` and `freelfs-dnf`. `distrobox enter -n freelfs-apt` gives a shell inside Ubuntu.
- **`podman`**: shipped since round 24a (5.8.10, rootless, daemonless, nothing at boot). Runtime crun, monitor conmon, rootless network pasta (package passt), `netavark` (prebuilt upstream binary; podman refuses to start without it), `fuse-overlayfs` (used only when the home is itself on overlayfs, i.e. the live system; flpkg writes `~/.config/containers/storage.conf` then). Needs `/etc/subuid` and `/etc/subgid` (the installer adds a range for the created user, the live boot one for `tester`) and the setuid `newuidmap`/`newgidmap` (docs/SECURITY.md 6.5). Images and containers live in `~/.local/share/containers`; on a live system without persistence that is RAM, and flpkg warns with the size before it creates a container. Try: `podman run --rm docker.io/library/debian:stable-slim cat /etc/os-release`.
- **`nix`**: not shipped in this round (`recipes-wip/nix` has no recipe file yet). `flpkg install --from=nix` says so.

### Power and session

- **Desktop:** the panel's power menu, `flpower off|restart|logout`, or Ctrl+Alt+Backspace (ends the session at once, unsaved work is lost).
- **Console:** the `/etc/inittab` file runs `shutdown -t1 -a -r now` on Ctrl+Alt+Delete (upstream sysvinit). The names `shutdown`, `reboot` and `poweroff` come from the sysvinit package. Not yet verified which of those names are installed in this build; use `flpower` from the desktop if unsure.
- **Runlevels:** 0 halt, 1 single user, 3 multi-user (console, the default), 6 reboot, as in `/etc/inittab`.

### Time

- **`date`** (coreutils): the time. `date -u` for UTC.
- The time zone and NTP are set in Settings > Date & time, which calls `fltime-set` (section 2). `fl-sntp -q SERVER` asks one server.

### Logs and where to look

| Question | Command or file |
|---|---|
| What happened at boot? | `dmesg \| less`, `/var/log/dmesg.log`, `/run/flsplash.log` |
| Wi-Fi problems | `/var/log/iwd.log` |
| Bluetooth problems | `/var/log/bluetooth.log` |
| Desktop problems | `$XDG_RUNTIME_DIR/flcomp.log`, `$XDG_RUNTIME_DIR/session.log`, `~/.local/state/freelfs/log/` |
| A program that crashed | `~/.local/state/freelfs/log/NAME.log` |
| Browser problems | `~/.cache/freebws/log` |
| Printing | `/var/log/cups/` |
| Everything, for a bug report | `flreport` |

---

## 9. Services: start, stop and enable (rc scripts)

FreeLFS does not use systemd. It uses small rc scripts. The boot runs `/etc/rc.d/rc 3` (the default runlevel in `/etc/inittab`). That starts the services listed in `SERVICES` in `/etc/rc.conf`, in that order. Each service is a script in `/etc/rc.d/init.d/`. Shutdown stops the enabled services in reverse order.

**Services shipped in this round:**

| Service | What it does | Notes |
|---|---|---|
| `syslog` | system logger (sysklogd) | logs in `/var/log/` |
| `dbus` | the D-Bus system bus | needed by iwd, bluetooth and others |
| `network` | wired networking | runs DHCP on all interfaces, or the `NETWORK=` entries in `rc.conf` |
| `seatd` | the seat manager (gives the screen and input to the person sitting at the machine) | needed by the desktop |
| `iwd` | Wi-Fi | needs dbus |
| `bluetooth` | `bluetoothd` | starts only while a Bluetooth adapter exists |
| `cups` | printing, on demand (through flsocketd) | `start` runs flsocketd; cupsd itself starts at the first connection and exits after 60 s idle |
| `ntp` | sets the clock (fl-sntp) | on, unless `NTP=0` in `/etc/fltime.conf` |

**Commands (as root):**

| Task | Command |
|---|---|
| See what is enabled | `grep ^SERVICES /etc/rc.conf` |
| Enable at the next boot | add the name to `SERVICES=(...)` in `/etc/rc.conf` |
| Disable at the next boot | put `!` in front of the name in `SERVICES`, for example `!cups` |
| Start now | `/etc/rc.d/init.d/NAME start` |
| Stop now | `/etc/rc.d/init.d/NAME stop` |
| Restart | `/etc/rc.d/init.d/NAME restart` |
| Status | `/etc/rc.d/init.d/NAME status` (prints running or stopped; returns 3 when stopped) |
| Network status | `/etc/rc.d/init.d/network status` (shows `ip -brief addr`) |
| Start everything again | `/etc/rc.d/rc 3` |

Points to remember:

- The order in `SERVICES` is the start order. Put a service after the one it needs: `dbus` before `iwd`, and `seatd` before the desktop.
- Shutdown stops only the **enabled** services. A service that is running but disabled is not stopped at shutdown, so stop it by hand first.
- `rc.conf` also holds `HARDWARECLOCK`, `KEYMAP`, `CONSOLEFONT`, `MODULES=()` (kernel modules to load in order) and the `NETWORK=()` entries.
- A user script at `/etc/rc.local`, if it exists, runs at the end of the start.
- The `network` entries look like `"eth0 dhcp"` or `"eth0 static 192.168.1.10/24 192.168.1.1"`.

Adding a service is described in [manual 14](manual/14-services-init.md) (section 14.8).

---

## 10. Environment variables

Set them in front of a command (`FLPANEL_ROOT=/tmp/x flpanel`), or export them in your shell. Variables marked *test* are for FreeLFS's own tests and screenshots. Most of them do nothing in normal use.

### FreeBWS (the browser)

| Variable | Meaning |
|---|---|
| `FREEBWS_HOME=DIR` | put the configuration, data and cache under DIR (test) |
| `FREEBWS_DATADIR=DIR` | the package's data files (default `/usr/share/freebws`) |
| `FREEBWS_BUFFERS=shm\|dmabuf` | how frames reach the screen: `shm` (no GPU, copy) or `dmabuf` (GPU). Otherwise chosen automatically |
| `FREEBWS_RENDER_NODE=/dev/dri/renderD129` | pick the GPU render node for dmabuf |
| `FREEBWS_SANDBOX=auto\|on\|off` | the web-process sandbox. `off` helps to compare when a site breaks |
| `FREEBWS_SANDBOX_RO=/a:/b` | extra read-only paths for the sandbox |
| `FREEBWS_CACHE=viewer\|document\|browser` | WebKit cache model (test) |
| `FREEBWS_FEATURES=Name=0\|1,...` | WebKit feature flags; replaces the default list (test) |
| `FREEBWS_MP=limit_mb,conservative,strict,kill,poll_s` | memory pressure settings, or `0` (test) |
| `FREEBWS_SHARE=0\|1` | put all tabs in one web process (related views) |
| `FREEBWS_DISCARD_IDLE=seconds` | discard background tabs after this many seconds (`0`: never) |
| `FREEBWS_DISCARD_MB=N` | discard background tabs when less than N MB is free |
| `FREEBWS_PAGECACHE=...` | page cache setting (test) |
| `FREEBWS_COOKIES=...` | WebKit cookie accept policy |
| `FREEBWS_DSSL=engine` | always use the JavaScript engine for DSSL pages (test) |
| `FREEBWS_ADB_SKIP=net,css,js,yt` | leave parts of the ad blocker out, to find what breaks a site (test) |
| `FREEBWS_ADB_LOG=1` | log the ad blocker's decisions (test) |
| `FREEBWS_ADB_UPDATE=1` | update the ad-block lists when started (test) |
| `FREEBWS_DEBUG=1` | verbose log in `~/.cache/freebws/log` (every load event and redirect) |
| `FREEBWS_EVAL=js` | run a script in each page 3 seconds after it loads, and log the result (test) |
| `FREEBWS_POLICY_LOG=1` | log navigation policy decisions (test) |
| `FREEBWS_POPUP_UNRELATE=1` | the old popup behaviour (test) |
| `FREEBWS_HANGKILL=1` | kill a hung tab even when the tabs do not share one process (test) |
| `FREEBWS_WATCHDOG_TEST=1` | test the frame watchdog (test) |

### FlComp (the compositor and session)

| Variable | Meaning |
|---|---|
| `FLCOMP_NOSTART=1` | do not start the desktop on `tty1` (stay in the shell) |
| `FLCOMP_PANEL=CMD` | the panel program (default `flpanel`; empty for none) |
| `FLCOMP_SESSION=CMD` | the session services script (default `/usr/lib/flcomp/session`; empty for none) |
| `FLCOMP_SETUP=CMD` | the first-boot program (default `flsetup`) |
| `FLCOMP_LOCK=CMD` | the lock program (default `fllock`); an empty value turns locking off |
| `FLCOMP_RENDER_NODE=/dev/dri/renderDN` | the GPU render node to use |
| `FLCOMP_NO_GL=1` | do not use GL (CPU only) |
| `FLCOMP_NO_SOUND=1` | no system sounds |
| `FLCOMP_NO_XWAYLAND=1` | do not start Xwayland (X11 programs will not show) |
| `FLCOMP_XWAYLAND=PATH` | the Xwayland program to start |
| `FLCOMP_XWAYLAND_LOG_STDERR=1` | send Xwayland's output to the terminal instead of its file |
| `FLCOMP_XDEBUG=1` | log the X11 window manager (test) |
| `FLCOMP_WINLOG=...` | log the windows (test) |
| `FLCOMP_ANIM_LOG=1` | log animations (test) |
| `FLCOMP_SCREENSHOT=FILE` | write a picture of the screen to FILE on SIGUSR1 (test) |
| `FLCOMP_SNAPPEL=CMD` | the screenshot program (default `snappel`) |
| `FLCOMP_WALLPAPER=FILE` | a wallpaper file to use |
| `FLCOMP_SHARE=DIR` | the share directory (default `/usr/share/flcomp`) |
| `FLCOMP_KEYBOARD_CONF=FILE` | the keyboard layout file |
| `FLCOMP_BACKLIGHT=...` | the backlight to use (test) |
| `FLCOMP_NO_CLIPHIST=1` | do not keep a clipboard history |
| `FLCOMP_TRUST_ANY=1` | accept any client as trusted (set by whoever starts flcomp; the login screen clears it) |
| `FLCOMP_HEADLESS=WxH` | no display or input, for tests |
| `FLCOMP_HEADLESS_OUTPUTS=...` | headless outputs (test) |
| `FLCOMP_TEST_FIFO=...` | test control pipe (test) |

### Packages and updates

| Variable | Meaning |
|---|---|
| `FLPKG_NO_SOURCES=1` | skip the sources front end (plain native flpkg) |
| `FLPKG_SOURCES`, `FLPKG_NATIVE`, `FLPKG_SEAT`, `FLPKG_FLATPAK`, `FLPKG_NIX`, `FLPKG_DISTROBOX` | other programs to use (test) |
| `FLPKG_STATE=DIR` | where `sources.list` is kept (default `~/.local/state/flpkg`) |
| `FLPKG_LIVE_DIR=DIR` | the live system folder (default `/run/live`) |
| `FLPKG_ASSUME_TTY=1` | treat output as a terminal (test) |
| `FLPKG_LIBDIR=DIR` | the library directory (default `/usr/lib/flpkg`) |
| `SOURCE_DATE_EPOCH=N` | fixed build time for reproducible builds |
| `FLPACKAGES_TAB=flathub` | open the Packages app on the Flathub tab |
| `FLPACKAGES_CONF`, `FLPACKAGES_FLPKG`, `FLPACKAGES_FLATPAK`, `FLPACKAGES_SEAT` | other config and programs for the Packages app (test) |
| `FLSTORE_PAGE=updates` | open the Store on the Updates tab |
| `FLSTORE_CONF`, `FLSTORE_FLPKG`, `FLSTORE_SEAT`, `FLSTORE_FLATPAK`, `FLSTORE_CURL`, `FLSTORE_APPSTREAM_URL`, `FLSTORE_ICON_URL`, `FLSTORE_LIVE_DIR`, `FLSTORE_SELF`, `FLSTORE_SHOT` | other programs and sources for the Store (test) |
| `FLUPDATE_FIRST=SECONDS` | first update check after login (default 300) |
| `FLUPDATE_INTERVAL=SECONDS` | time between checks (default 86400) |
| `FLUPDATE_RETRY=SECONDS` | retry after a failed check (default 3600) |
| `FLUPDATE_FLPKG`, `FLUPDATE_FLATPAK`, `FLUPDATE_FLNOTIFY`, `FLUPDATE_STORE`, `FLUPDATE_STATE` | other programs and the state folder (test) |
| `FLSNAP_CONF=FILE` | another `flsnap.conf` (test) |
| `FLSNAP_FLPKG=PROGRAM` | another flpkg (test) |
| `FLSNAP_MOCK=DIR` | use plain folders instead of btrfs (test) |

### Password and root helpers

| Variable | Meaning |
|---|---|
| `FLAUTH_TRIED=1` | set by a helper after it asked for the password once, to stop a loop. Not for typing |
| `FLAUTH_DIR=DIR`, `FLAUTH_SHADOW=FILE`, `FLAUTH_DIALOG=PROGRAM` | replace `/run/flauth`, `/etc/shadow` and the dialog (test; ignored by the installed setuid copy) |
| `FLPERSIST=PROGRAM` | which `flpersist` the Settings app calls (test) |
| `FLLOCK_LOCKFILE`, `FLLOCK_SHADOW`, `FLLOCK_CHECK` | lock file and check program for `fllock` (test) |

### Time, power and display

| Variable | Meaning |
|---|---|
| `FLTIME_CONF=FILE` | another `fltime.conf` for `fl-sntp` and Settings (default `/etc/fltime.conf`) |
| `FLTIME_STATUS=FILE`, `FLTIME_SET=PROGRAM` | status file and `fltime-set` program (test) |
| `FL_ZONEINFO=DIR`, `FL_TZ_ETC=DIR`, `TZ` | the time zone database (`TZ` is the standard one) |
| `FLPOWER_STATE=FILE`, `FLPOWER_CPU=...` | power state and CPU files, read by `flpower` for non-root tests |
| `FLPANEL_ROOT=DIR`, `FLPANEL_POWER_SUPPLY=DIR`, `FLPANEL_APPS=...`, `FLPANEL_NOBAT=1`, `FLPANEL_NO_SOUND=1` | panel test hooks |
| `FL_KEYBOARD_CONF=FILE` | the keyboard layout file |
| `FLUI_CONF=FILE` | another `desktop.conf` (default under `~/.config/freelfs/`) |
| `FLUI_PPM=FILE`, `FLUI_PPM_DELAY=ms` | write a picture of a flui program to FILE (screenshots) |
| `FLSETTINGS_SIZE=WxH`, `FLSETTINGS_TEST_SEARCH`, `FLSETTINGS_TEST_MOUSE`, `FLSETTINGS_BACKLIGHT`, `FLREPORT=PROGRAM`, `FLPERSIST=PROGRAM` | Settings test hooks and helper programs |
| `FLTHEME_SIZE=WxH`, `FLTHEME_TEST_SAVE` | Themes screenshot size and test hook |
| `FLSPLASH_CONF=FILE`, `FLSPLASH_SET_DIR=DIR`, `FLSPLASH_SET_CONF=FILE` | boot screen config and test paths. `FLSPLASH_SET_DIR` and `FLSPLASH_SET_CONF` apply only without root |
| `FLGREET_STYLE` | the login screen style (see the flgreet README; not yet verified) |
| `FLINSTALL_HELPER=PROGRAM`, `FLINSTALL_STEP`, `FLINSTALL_RESTART`, `FLINSTALL_FAKE`, `FLINSTALL_TEST` | installer test hooks. Do not set them on a real install |
| `FLBOOT_TEXT=1`, `FLBOOT_FAKE_OTHER=1` | boot menu test hooks |

### Apps

| Variable | Meaning |
|---|---|
| `FLMUSIC_DIR=DIR` | the music library folder (default `~/music`, or `~/Music`) |
| `FLMUSIC_SINK=NAME` | the audio output to use |
| `FLVIEW_KEYS=FILE` | a script of keys to replay (test) |
| `FLVIDEO_HWDEC=MODE` | hardware decoding mode (see the flvideo README) |
| `FLVIDEO_AO=NAME` | audio output (see the flvideo README) |
| `FLVIDEO_DEBUG=1` | more player messages |
| `FLPORTAL_VERBOSE=1` | more messages from flportal |
| `FLPORTAL_SNAPPEL=...`, `FLPORTAL_SHOT`, `FLPORTAL_ANY_SENDER`, `FLPORTAL_TEST_*` | flportal test hooks |
| `FLCRASH_PPM=FILE`, `FLCRASH_PROTECT=...`, `FLCRASH_NO_SOUND=1` | flcrash picture, extra protection and sound (test) |
| `GM_INTERVAL_MS=ms` | refresh interval of General Manager (at least 50) |
| `NO_COLOR=1` | turn off colours in flsh (the source checks it) |
| `FLSTORE_*` | see the Packages table above |
| `FLPANEL_*` | see the table above |

### Standard variables used by FreeLFS tools

| Variable | Meaning |
|---|---|
| `HOME`, `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_CACHE_HOME`, `XDG_STATE_HOME` | where settings, data, cache and state go (defaults under `~`) |
| `XDG_RUNTIME_DIR` | the session folder (`/run/user/UID`); the logs and sockets go here |
| `XDG_DATA_DIRS`, `XDG_CONFIG_DIRS`, `XDG_CURRENT_DESKTOP` | desktop data and configuration search paths |
| `WAYLAND_DISPLAY` | the Wayland socket name, set by flcomp |
| `DBUS_SESSION_BUS_ADDRESS` | the session bus, `unix:path=$XDG_RUNTIME_DIR/bus` |
| `TMPDIR` | where temporary files go |
| `TERM` | the terminal type (flterm uses `xterm-256color`) |
| `SHELL` | the shell `flterm` starts |

---

## 11. Kernel boot parameters

Boot parameters go on the kernel command line. On an installed system they are in `/boot/flboot/flboot.conf` (`cmdline` for every kernel, or an `entry` line for one boot entry). In the boot menu, the entries you see are already set up for you. 
| Parameter | Where it comes from | What it does |
|---|---|---|
| `nomodeset` | boot menu entry "Safe graphics" (also added by flboot for that entry), or by you | no GPU driver takes the screen. The firmware's framebuffer shows the desktop. Use it when the screen fails with a driver |
| `nouveau.modeset=0` | "Safe graphics" | stop the open NVIDIA driver from loading. Used together with `nomodeset` |
| `nosplash` | by you | the boot screen (`flsplash`) does nothing and the boot text is shown |
| `quiet` | live ISO menu | less boot text |
| `console=tty0`, `console=ttyS0,115200` | live ISO | the screen and a serial port for the console |
| `panic=30` | live ISO | reboot 30 seconds after a kernel panic |
| `root=...` | installed system (set by flboot-install) | the root partition, for example `root=PARTUUID=...` |
| `rootflags=subvol=@snapshots/N` | "Start an earlier state" entries | start the read-only snapshot N |
| `initrd=\initrd.img`, `initrd=\ucode.img` | live ISO and installed system | the initramfs and the CPU microcode files on the boot partition |
| `flive.timeout=SECONDS` | live system (flinitrd) | how long to wait for disks (default 20) |
| `flive.iso=PATTERN` | live system | which ISO file to use (default `freelfs-live*.iso`) |
| `flive.debug` | live system ("verbose" entry) | say what flinitrd tries |
| `flive.toram=1` | live system ("Load into RAM" entry) | copy the system into RAM so the stick can be removed. `flive.toram=0` forbids it. Without either, it is automatic when the machine has about 4 GiB more RAM than the image |
| `flive.nopersist` | live system | start without the persistence image on the stick |

Notes:

- `nomodeset` is also read by the compositor: with it, flcomp does not wait for a GPU driver to load.
- `nosplash` is read by flsplash from `/proc/cmdline`.
- Other kernel parameters (for example `amdgpu.*`, `i915.*`, `nvidia-drm.*`) are passed through unchanged. FreeLFS does not define them.

---

## 12. Where settings and logs live

| What | Where |
|---|---|
| Package manager settings | `/etc/flpkg.conf`; trusted keys `/etc/flpkg/keys/` |
| Installed packages database | `/var/lib/flpkg/db.sqlite` |
| Package cache and build directory | `/var/cache/flpkg`, `/var/tmp/flpkg` |
| Which source installed what (per person) | `~/.local/state/flpkg/sources.list` |
| Snapshot settings | `/etc/flsnap.conf` (`keep=`, `device=`) |
| Snapshots | btrfs `@snapshots`, seen as `/.snapshots` |
| Boot loader and menu | `/boot/EFI/BOOT/BOOTX64.EFI`, `/boot/flboot/flboot.conf`, `/boot/flboot/snapshots` |
| Boot screen style | `/etc/freelfs/splash.conf` |
| Clock and time zone | `/etc/localtime`, `/etc/timezone`, `/etc/fltime.conf`, status `/run/fltime.status` |
| Services | `/etc/rc.conf`, scripts in `/etc/rc.d/init.d/` |
| Login and init | `/etc/inittab` |
| Wi-Fi | `/etc/iwd/main.conf`, log `/var/log/iwd.log` |
| Desktop settings | `~/.config/freelfs/desktop.conf` (or `$FLUI_CONF`) |
| Browser settings | `~/.config/freebws/config`, bookmarks `~/.local/share/freebws/bookmarks.json`, log `~/.cache/freebws/log` |
| Shell history and config | `~/.local/share/flsh/history`, `~/.config/flsh/flshrc`, `/etc/flshrc` |
| Desktop program logs | `~/.local/state/freelfs/log/NAME.log`, desktop log copies there too |
| Session logs | `$XDG_RUNTIME_DIR/session.log`, `$XDG_RUNTIME_DIR/flcomp.log` |
| Login records and system logs | `/var/log/` (`dmesg.log`, `iwd.log`, `bluetooth.log`, `cups/`) |
| Bug report archive | `~/freelfs-report-*.tar.gz` (made by `flreport`) |
| Flatpak apps (per person) | `~/.local/share/flatpak` |
| Podman images and Distrobox containers (per person) | `~/.local/share/containers` (RAM on a live system without persistence); exports in `~/.local/bin` and `~/.local/share/applications` |

For more detail, see the manual: [12 filesystem layout](manual/12-filesystem-layout.md), [15 users and security](manual/15-users-security.md), [19 admin reference](manual/19-admin-reference.md), [29 glossary](manual/29-glossary.md).
