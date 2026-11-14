# 13. Packages with flpkg: commands, groups, repos, snapshots, updates and the Store

This chapter covers the package manager flpkg: its commands (from `src/flpkg/main.c`), recipes and groups, signed repositories, transactions, btrfs snapshots, the update notice, and the Flatpak side with the Software Store.

Status: round 17 (2026-11-14). FreeLFS is a **rolling release**. There is **no public package repository yet**: this tree has no repo URL, and nothing has been uploaded.

## Contents

1. [What flpkg is](#131-what-flpkg-is)
2. [Commands](#132-commands)
3. [Options of the commands](#133-options-of-the-commands)
4. [Pacman-style aliases](#134-pacman-style-aliases)
5. [Recipes and building packages](#135-recipes-and-building-packages)
6. [Groups](#136-groups)
7. [Repositories and signatures](#137-repositories-and-signatures)
8. [Transactions and triggers](#138-transactions-and-triggers)
9. [Snapshots with flsnap](#139-snapshots-with-flsnap)
10. [Updates: the notice and the check](#1310-updates-the-notice-and-the-check)
11. [Flatpak and the Software Store](#1311-flatpak-and-the-software-store)
12. [The rolling release](#1312-the-rolling-release)
13. [Known issues](#1313-known-issues)

## 13.1 What flpkg is

flpkg (`src/flpkg`, C17, SQLite, zstd, OpenSSL's libcrypto; recipe version 0.1 release 5) builds packages from recipes, installs and removes them in atomic transactions, resolves dependencies, and keeps the database.

| Path | What it is |
|---|---|
| `/usr/bin/flpkg` | the program |
| `/usr/lib/flpkg/recipe.sh` | runs recipes (`recipe.sh dump` and `run`) |
| `/usr/lib/flpkg/triggers` | the post-transaction triggers (see 13.8) |
| `/etc/flpkg.conf` | settings (13.7) |
| `/etc/flpkg/keys/` | trusted signing keys (`*.pub`) |
| `/var/lib/flpkg/db.sqlite` | the installed-package database |
| `/var/lib/flpkg/journal` | the plan of a transaction in progress |
| `/var/cache/flpkg/` | downloaded packages and indexes |
| `/usr/libexec/flpkg/pre-transaction.d/` | hooks run before every transaction (the snapshot hook `10-flsnap`) |

flpkg runs as root. The desktop never runs it as root: the Store and the Packages app call `flpkg-seat` (setuid, group `seat`), which asks for the person's password first (see [15](15-users-security.md)). Reads (`list`, `info`, `files`, `owns`, `check-updates`) run as an ordinary user.

## 13.2 Commands

From `flpkg help` (`src/flpkg/main.c`):

| Command | Alias | What it does |
|---|---|---|
| `flpkg install PKG...` | `-S` (also `-U` for files) | install packages and their dependencies from the repos, or `FILE.flpkg` files |
| `flpkg remove PKG...` | `-R` | remove packages |
| `flpkg upgrade` | `-Su` | upgrade every installed package that has a newer version in the repos |
| `flpkg sync` | `-Sy` | refresh the repo indexes |
| `flpkg search TEXT...` | `-Ss` | search names and summaries in the synced indexes |
| `flpkg check-updates [--offline] [--count]` | none | list what `upgrade` would do, one tab-separated line per package; no root needed |
| `flpkg list` | `-Q` | list installed packages (`--explicit`, `--deps`) |
| `flpkg info PKG` or `info -f FILE.flpkg` | `-Qi` | show a package; `info --repo PKG` shows a repo package (`-Si`) |
| `flpkg files PKG` or `files -f FILE.flpkg` | `-Ql` | list a package's files |
| `flpkg owns PATH...` | `-Qo` | which package owns a file |
| `flpkg verify [PKG]` | `-Qk` | check installed files against the database (size, sha256, mode, owner) |
| `flpkg build [DIR]` | none | build the recipe in DIR into a package (13.5) |
| `flpkg repo add REPO FILE...` | none | add packages to a repo and re-sign its index |
| `flpkg key generate NAME` | none | create a signing key pair (`NAME.key`, `NAME.pub`) |
| `flpkg key introduce --key OLD.key --new NEW.pub DIR` | none | write a key rotation statement (13.7) |
| `flpkg key list` | none | trusted keys and their retirement dates |
| `flpkg fix-owners [--keep]` | none | apply ownership that a rootless build could not set; `--keep` leaves the database alone (used on the live system) |
| `flpkg snapshots` | none | list the btrfs snapshots (runs `flsnap list`) |
| `flpkg history` | none | past transactions |
| `flpkg version` | none | the flpkg version |

Global options before the command: `--root DIR` (work on another root, e.g. a chroot), `--config FILE` (another `flpkg.conf`), `-v` or `--verbose`.

## 13.3 Options of the commands

| Command | Options |
|---|---|
| `install` | `--nodeps` (`-d`), `--yes` (`-y`), `--asdeps` (`-a`, mark as dependency), `--overwrite GLOB` (`-o`), `--needed` (`-n`, skip what is already the same version), `--unsigned` (`-u`, accept a `.flpkg` file without a valid signature) |
| `remove` | `--cascade` (`-c`, also remove what depends on the target), `--recursive` (`-r`, also remove dependencies nothing else needs), `--nodeps` (`-d`), `--yes` (`-y`) |
| `upgrade` | `--sync` (`-s`, runs `sync` first; `-Syu` is `upgrade --sync`), `--yes` (`-y`), `--nodeps` (`-d`) |
| `check-updates` | `--offline` (use the last synced indexes), `--count` (print only the number). Exit 0 = checked, 1 = error (no network, bad signature), 2 = bad usage |
| `search` | the text to look for, several words allowed |
| `fix-owners` | `--keep` |
| `list` | `--explicit` (only what you asked for), `--deps` (only dependencies) |
| `repo add` | `--key FILE`, `--remove-old`, `--valid-days N`, `--serial N` |
| `build` | `-o DIR` (where to write the package), `-s DIR` (look for sources here first; repeatable), `--nocheck` (skip `check()`), `--keep` (keep the work directory) |

`upgrade` shows the plan and asks for confirmation unless `--yes` is given. `install` checks the signature of a `.flpkg` file given on the command line, unless `--unsigned` is given or `signatures = never` is set in `flpkg.conf`. Names are resolved through the repos, not files.

## 13.4 Pacman-style aliases

| Flag | Command |
|---|---|
| `-S` `-U` | `install` |
| `-Sy` | `sync` |
| `-Su` | `upgrade` |
| `-Syu` | `upgrade --sync` |
| `-Ss` | `search` |
| `-Si` | `info --repo` |
| `-R` | `remove` |
| `-Rs` | `remove --recursive` |
| `-Rc` | `remove --cascade` |
| `-Q` | `list` |
| `-Qi` | `info` |
| `-Ql` | `files` |
| `-Qo` | `owns` |
| `-Qk` | `verify` |

## 13.5 Recipes and building packages

A recipe is `recipes/NAME/recipe`: a bash file of variables and functions. Patches and other local files sit next to it. The tree has 274 recipe folders. Example (`recipes/flpkg/recipe`, trimmed):

```sh
pkgname=flpkg
version=0.1
release=5
summary="The FreeLFS package manager"
license=(MIT)
arch=(x86_64)
groups=(base)
depends=(glibc sqlite zstd openssl bash tar)
source=()
sha256=()

build()   { cp -r "$startdir/../../src/flpkg" "$srcdir/flpkg"; cd "$srcdir/flpkg"; make clean; make; }
check()   { cd "$srcdir/flpkg"; make check; }
package() { cd "$srcdir/flpkg"; make DESTDIR="$pkgdir" install; }
```

Fields (from `docs/flpkg.md`): `pkgname` (one name, or several for a split recipe), `version`, `release`, `summary`, `url`, `license`, `arch` (a recipe must list the machine it builds for; all recipes say `x86_64`), `groups`, `depends` (runtime), `makedepends` (build only), `checkdepends`, `provides`, `conflicts`, `replaces`, `source` and `sha256` (one checksum per source, `SKIP` is not allowed), `sysusers` and `sysgroups` (accounts the package declares), `owner` (`"PATH UID GID MODE"`, used for setuid helpers), `options` (`!strip`, `!purge_la`, `!purge_infodir`, `!pathcheck`), `build_style` (autotools, meson or python). Functions: `prepare()`, `build()`, `check()` (skipped with `--nocheck`) and `package()`, or `package_NAME()` for each name of a split recipe.

Version constraints in `depends`: `name`, `name>=1.2`, `name<=1.2`, `name=1.2-3`, `name>1`, `name<2`.

Building:

```sh
flpkg build recipes/zlib          # recipe -> zlib-1.3.2-1-x86_64.flpkg (in .)
flpkg build -o ~/repo recipes/zlib
```

The package is a zstd-compressed tar archive with its metadata and file manifest, named `NAME-VERSION-RELEASE-ARCH.flpkg`. Builds are reproducible: `SOURCE_DATE_EPOCH`, sorted entries, fixed owners and times. `build/61-repro-check.sh` builds everything again and compares the result byte for byte.

The FreeLFS build scripts that drive flpkg (in the build tree, not on an installed system):

| Script | Does |
|---|---|
| `build/60-packages.sh` | builds every recipe except group `deferred` into the target root's repository (resumable) |
| `build/61-repro-check.sh` | builds again and compares |
| `build/70-repo.sh` | makes the signed static repo directory and the key |
| `build/gentle.sh` | runs a command with low CPU and I/O priority |

## 13.6 Groups

Each recipe names one group in `groups=(...)`. Counted from `recipes/*/recipe` at round 17:

| Group | Recipes | Meaning |
|---|---|---|
| `base` | 94 | the base system (LFS toolchain, libraries, flpkg, flboot, flsnap, boot scripts, dhcpcd, btrfs-progs and more). `flpkg install base` installs all of them |
| `desktop` | 171 | the desktop, its apps, Mesa, PipeWire, WirePlumber, BlueZ, CUPS, iwd, Flatpak, the browser and its engine, the Store |
| `live` | 5 | tools for the live ISO: flinitrd, squashfs-tools, libisofs, libburn, libisoburn |
| `lts` | 1 | `linux-lts` (6.18.63): built, not installed in the default image (`flpkg install linux-lts`) |
| `deferred` | 2 | `libclc`, `spirv-llvm-translator`: kept for the 16 GB build machine (Intel 3D drivers); skipped by `build/60-packages.sh` |
| none | 1 | `flpackages` declares no group (the Packages app; its helper `flpkg-seat` is what the Store uses) |

The clang part of LLVM is deferred too: the `llvm` recipe is in group `desktop` and builds the core libraries only (see [17](17-hardware-drivers.md)).

## 13.7 Repositories and signatures

`/etc/flpkg.conf` (written by `recipes/flpkg/recipe`):

```text
repo freelfs URL              # only if config/flpkg/repo-url exists at build time
keys = /etc/flpkg/keys        # trusted *.pub signing keys
signatures = required         # required, optional or never
backup_config = no            # yes: keep a changed /etc file as FILE.flpkg-save
snapshots = 1                 # 0: no btrfs snapshot before a transaction
```

Notes:

- **This tree has no repo line.** `config/flpkg/repo-url` is not in the tree, and the hosting is not chosen (`docs/INTEGRATION-TODO.md`: "flpkg has no repository configured (no hosting yet)").
- Repos are searched in the order given. A repo is a directory, a `file://` path or an `http(s)://` URL (fetched with curl). It holds packages, their signatures, and `index.zst` with `index.zst.sig`.
- The index is signed with **Ed25519** in the minisign file format. The signed comment carries `serial:N` and `valid-until:TIME`. `flpkg sync` and `check-updates` **refuse an expired index** and **an index with a lower serial** than the last one accepted (`/var/lib/flpkg/index-serial.REPO`), which stops replay of an old index. The default validity is 14 days (`FLFS_REPO_VALID_DAYS`, `build/70-repo.sh`). Once the repo is hosted it must be re-signed at least every 10 days (`docs/INTEGRATION-TODO.md`).
- Keys rotate with `flpkg key introduce`: a rotation statement (`rotate.pub` and its signature) is published next to the index, and clients learn the new key from it. `flpkg key list` shows the trusted keys and their retirement dates. `docs/SECURITY.md` section 1 describes key custody and the rotation steps.
- A package file given on the command line is checked against its signature unless `--unsigned` is given.
- The serial check refuses only a **lower** serial. An index with the same serial as the last accepted one is accepted again. This follows the code in `src/flpkg`; the design notes are in `docs/SECURITY.md`, "Index expiry and replay protection".

## 13.8 Transactions and triggers

`install`, `upgrade` and `remove` are each one **transaction**:

1. **Resolve** the target set. Each dependency must be installed, in the same transaction, or in a repo. Pulled-in packages are marked as dependencies.
2. **Check** signatures and sha256 of every package, and file conflicts with other packages and with untracked files.
3. **Plan.** Every new file gets a hidden temporary name, and every file to be replaced or removed a hidden backup name. The plan goes to `/var/lib/flpkg/journal` and is synced before anything changes.
4. **Stage.** Write the new files under their temporary names. Each is checked against the manifest (size and sha256).
5. **Commit.** Rename replaced files to their backup names and the new files into place. Removed files go to backups.
6. **Record.** Update the database in one SQLite transaction. That commit is the point of no return.
7. **Clean up.** Delete backups and the journal, then run the triggers.

At commit time, after the dependency and file-conflict checks and before any file changes, every hook in `/usr/libexec/flpkg/pre-transaction.d/` runs in name order. The snapshot hook takes a btrfs snapshot (13.9). An error before step 6 undoes everything. After a crash or power loss, the next flpkg command finishes or rolls back from the journal.

Users and groups declared in `sysgroups` and `sysusers` are added to `/etc/group`, `/etc/passwd` and the shadow files before the files are committed, and never removed.

Config files under `/etc` are replaced by the new version. With `backup_config = yes`, a file the user changed is first saved as `FILE.flpkg-save`.

Triggers run once per transaction, only when a matching path changed, and only if the command exists (`/usr/lib/flpkg/triggers`):

| Changed paths | Command |
|---|---|
| `usr/lib/*.so*`, `etc/ld.so.conf*` | `ldconfig` |
| `usr/lib/modules/*` | `depmod -a VERSION` |
| `usr/lib/udev/hwdb.d/*`, `etc/udev/hwdb.d/*` | `udev-hwdb update` |
| `usr/share/info/*` | rebuild the info `dir` file |
| `usr/share/fonts/*` | `fc-cache` |
| `usr/share/icons/*` | `gtk-update-icon-cache` |
| `usr/share/mime/*` | `update-mime-database` |
| `usr/share/glib-2.0/schemas/*` | `glib-compile-schemas` |
| `usr/share/applications/*` | `update-desktop-database` |

A trigger for `usr/lib/flboot/*` runs `flboot-install --update`, so the boot loader on the ESP follows the package (see [11](11-boot-process.md)).

## 13.9 Snapshots with flsnap

- Before each transaction the hook `10-flsnap` runs `flsnap create --auto --reason "flpkg: SUMMARY" --packages FILE`, unless `snapshots = 0`.
- `flsnap` keeps the newest **8** (`keep` in `/etc/flsnap.conf`). It writes the boot menu's list after every change.
- `flsnap rollback [N]` makes snapshot N the current state; the state being left becomes a snapshot itself. Deleting and pruning also ask for the password.
- Full details, paths and the boot menu entry: [12](12-filesystem-layout.md) (12.9) and [11](11-boot-process.md) (11.9).

Commands: `flpkg snapshots` (the list), `flsnap list|show N|status|create|delete N|prune|rollback` (see [19](19-admin-reference.md)).

Caveat: a snapshot does not hold the kernel, which lives on the ESP. A snapshot booted with a newer kernel can lack that kernel's modules (see [11](11-boot-process.md)).

## 13.10 Updates: the notice and the check

- `flupdate-notify` (`src/flupdate`, started by the session) **never installs anything**. It checks five minutes after login (`FLUPDATE_FIRST`, default 300 s), then every 24 hours (`FLUPDATE_INTERVAL`, 86400), and retries after an hour if the check failed (`FLUPDATE_RETRY`, 3600). `flupdate-notify --once` checks now.
- A check runs `flpkg check-updates` (the FreeLFS repo) and `flatpak remote-ls --updates --user` (Flathub, the person's own apps). It runs as the user, refuses to run as root, and writes nothing to the system.
- If either has updates it posts **one** notification, "N updates available", which replaces the previous one. Clicking it, or its "Open Store" button, runs `flstore --updates`.
- `flpkg check-updates` is the machine-readable form: one tab-separated line per package. `--offline` uses the last `flpkg sync` results.

To apply updates by hand, as root:

```sh
flpkg sync                     # refresh the indexes (root: the Store's README says it is a root command)
flpkg check-updates            # what would change, no root needed
flpkg upgrade --sync           # the same as -Syu: sync, plan, confirm, install
```

In the desktop, the Store's **Updates** tab does this through `flpkg-seat`, after the password.

## 13.11 Flatpak and the Software Store

- **Flatpak** 1.18.4 (`recipes/flatpak`) is built for per-user installs only: `flatpak --user`, no root and no polkit. The system helper is disabled, so `flatpak --system` works only as root. There is no systemd: D-Bus services start from their `.service` files.
- **Flathub** is added for the person at their first session with a network connection (`src/flcomp/session`, `flathub_setup`). It is retried with growing pauses while offline. It is **not** added on the live system, whose disk is RAM (the appstream data alone is about 110 MB there; see [18](18-performance-ram.md)).
- The per-user apps live in `~/.local/share/flatpak` (see [12](12-filesystem-layout.md)).
- **Software Store** (`flstore`, `src/flstore`, recipe 0.1 r3) replaces the Packages app. Its window has three areas:
  - **Explore**: featured apps, recently updated apps, eight categories, search, FreeLFS packages;
  - **Installed**: what is installed from both sources;
  - **Updates**: the update list from both sources (`flstore --updates` opens it).
- Sources: Flathub through `flatpak --user` (install, update, remove, open, "Remove unused runtimes"). FreeLFS packages through the flpkg database: reads run as the person (`flpkg list`, `flpkg search ""`), changes go through `flpkg-seat install|remove` after the password.
- On the live system the Store does not download the catalog. It offers "Install FreeLFS" (the installer) and, as a second choice, "Try one small app anyway", which loads the catalog and shows the free memory first.
- Portals: Flatpak apps reach the desktop through xdg-desktop-portal and the FreeLFS backend flportal (see [15](15-users-security.md)). Not yet tried with a real Flatpak app (`docs/INTEGRATION-TODO.md`).

## 13.12 The rolling release

FreeLFS follows upstream without numbered releases:

- Every package is the **newest stable release of its series that fixes the known problems**. Nothing is backported. A vulnerability is fixed by taking the newer upstream release.
- The kernel is the newest stable 7.x (`linux` 7.3.4), with the 6.18 LTS as a fallback (`linux-lts`).
- The safety nets are the btrfs snapshot before each change and the boot menu's earlier states.
- The process, the CVE checks (`build/cve-check.py`) and the cadence are in `docs/SECURITY.md` section 0 and 2.

## 13.13 Known issues

| Issue | Status |
|---|---|
| No public repo: no URL, nothing uploaded | open; flpkg is ready (`docs/flpkg.md`, section 4) |
| The repo must be re-signed at least every 10 days once it is hosted | open |
| `flpkg sync` is a root command today (per the Store's README) | open |
| Snapshot kernel modules | open (see [11](11-boot-process.md)) |
| Flatpak apps not yet tried end to end (file picker, document portal) | open |
| The Store's install and update on a real machine | not yet verified |
| The `llvm` recipe builds the core libraries; clang is deferred | by design (`docs/HARDWARE.md`) |
| Transactions with a file conflict or a dependency problem stop before the snapshot | by design: nothing is changed in that case |
