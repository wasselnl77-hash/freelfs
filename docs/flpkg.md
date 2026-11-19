# flpkg — the FreeLFS package manager

flpkg builds packages from recipes, installs and removes them in atomic
transactions, resolves dependencies, and keeps the system's package
database. It is written in C (C17) and uses SQLite, zstd and OpenSSL's
libcrypto. It reads and writes tar archives with its own code.

## 1. Command line

Subcommands, with pacman-style flags as aliases.

| Subcommand | Alias | What it does |
|---|---|---|
| `flpkg install PKG...` | `-S` | Install packages from the repos or from `.flpkg` files, with their dependencies |
| `flpkg remove PKG...` | `-R` | Remove packages (`--cascade` also removes what depends on them, `--recursive` also removes dependencies nothing else needs) |
| `flpkg upgrade` | `-Su` | Upgrade every installed package that has a newer version in the repos |
| `flpkg sync` | `-Sy` | Refresh the repo indexes |
| `flpkg search TEXT` | `-Ss` | Search names and summaries in the synced repo indexes |
| `flpkg check-updates [--offline] [--count]` | | List what `upgrade` would do, one tab-separated line per package, for other tools (section 4) |
| `flpkg info PKG` | `-Qi` | Show an installed package (or `-Si`: a repo package) |
| `flpkg list` | `-Q` | List installed packages |
| `flpkg files PKG` | `-Ql` | List the files of an installed package |
| `flpkg owns PATH` | `-Qo` | Which package owns a file |
| `flpkg verify [PKG]` | `-Qk` | Check installed files against the database (size, sha256, mode, owner) |
| `flpkg build [DIR]` | | Build the recipe in DIR into packages |
| `flpkg repo add REPO PKGFILE...` | | Add packages to a repo and re-sign its index |
| `flpkg key generate PATH` | | Create a signing key pair (PATH.key, PATH.pub) |
| `flpkg fix-owners [--keep]` | | Apply ownership that a rootless install could not set. Opens the database read-only when nothing is pending; `--keep` (live system) leaves the list in place so the database is never written |
| `flpkg history` | | Show past transactions |

Common options: `--root DIR` (operate on another root), `--config FILE`,
`--yes` (no confirmation), `--overwrite GLOB` (allow replacing untracked
files), `--nodeps`, `--asdeps`.

`-Syu` = sync + upgrade. Aliases may be combined the pacman way.

## 2. Recipes

A recipe is `recipes/<base>/recipe`, a bash file of variables and functions.
Patches and other local files sit next to it.

```sh
pkgname=(gcc gcc-libs)          # one name, or several for a split recipe
pkgbase=gcc                     # optional; defaults to the first name
version=16.2.0
release=1
summary="The GNU Compiler Collection"
url=https://gcc.gnu.org/
license=(GPL-3.0-or-later)
arch=(x86_64)                   # or (any); a recipe must list the machine it builds for
groups=(base-devel)
source=(https://ftp.gnu.org/gnu/gcc/gcc-$version/gcc-$version.tar.xz
        gcc-fix.patch)
sha256=(…  …)                   # one per source; SKIP is not allowed
depends=(glibc zlib)            # runtime
makedepends=(gmp mpfr mpc)      # build only
checkdepends=(dejagnu)          # test suite only
provides=() conflicts=() replaces=()
sysusers=("dhcpcd 52 52 /var/lib/dhcpcd /usr/bin/false 'dhcpcd PrivSep'")
                                # name uid gid home shell comment
sysgroups=("render 30")                                     # name gid
owner=("/usr/bin/wall 0 5 2755")                            # path uid gid mode
options=()                      # !strip, !purge_la, !purge_infodir, !pathcheck
build_style=autotools           # optional: autotools, meson or python supply
configure_args=(--disable-static)  # build(), check() and package() the recipe
                                # leaves out; pyname= names the Python module

prepare() { … }                 # optional: patches, seds
build()   { … }
check()   { … }                 # optional; runs unless --nocheck
package() { … }                 # single package: install into "$pkgdir"
package_gcc-libs() { … }        # split: one function per name; may override
                                # summary, depends, provides, conflicts,
                                # replaces, groups, owner, sysusers
```

Version constraints in `depends` and friends: `name`, `name>=1.2`,
`name<=1.2`, `name=1.2-3`, `name>1`, `name<2`.

### The build environment
- `$srcdir`: sources are unpacked here. Each function starts in
  `$srcdir/$worksrc` if that exists (default `worksrc=$pkgbase-$version`;
  set `worksrc=` in the recipe for odd tarballs), otherwise in `$srcdir`.
- `$pkgdir`: the package root to install into (`make DESTDIR="$pkgdir" install`).
- `$startdir`: the recipe directory (for patches).
- `MAKEFLAGS`, `NINJAJOBS`, `SOURCE_DATE_EPOCH`, `LC_ALL=POSIX`, `PATH=/usr/bin:/usr/sbin`, and `CFLAGS`/`CXXFLAGS`/`LDFLAGS` from `/etc/flpkg.conf` (empty by default: upstream defaults, as in LFS).
- Every function runs under `set -e -o pipefail` in a clean `env -i` environment.
  `srcdir`, `pkgdir`, `startdir` and `pkgname` are shell variables of the
  recipe, not exported: programs the build runs do not see them (vim's
  configure, for one, would take `$srcdir` for its own).

### After `package()`
Unless `options` says otherwise, flpkg:
- strips ELF files (`--strip-unneeded`; `--strip-debug` for static archives),
- deletes libtool `.la` files and `/usr/share/info/dir`,
- refuses files under `/usr/local`, `/var/run`, `/run`, `/tmp`, `/dev`, `/proc`, `/sys`, and `/bin`, `/lib`, `/sbin` (those are symlinks into `/usr`).

## 3. Package files

`NAME-VERSION-RELEASE-ARCH.flpkg` is a zstd-compressed POSIX (pax) tar
archive. Entries in order:
1. `.PKGINFO`: metadata, `key = value` lines.
2. `.FILES`: the manifest, one line per entry (see below).
3. The payload, sorted bytewise by path.

Reproducible by construction: every mtime is `SOURCE_DATE_EPOCH`, owners
come from `owner=()` (otherwise 0:0), no user or group names, no atime or
ctime, fixed compression parameters.

`.PKGINFO`:
```
name = zlib
base = zlib
version = 1.3.2-1
summary = Compression library implementing the deflate method
url = https://zlib.net/
arch = x86_64
license = Zlib
builddate = 1767225600
size = 386264
depend = glibc
provide = libz.so=1-64
group = base
sysuser = dhcpcd 52 52 /var/lib/dhcpcd /usr/bin/false dhcpcd PrivSep
sysgroup = render 30
```

`.FILES`, one line per entry, seven fields separated by single spaces.
Backslash, space and newline in paths and link targets are written as
`\\`, `\040` and `\012` (as in mtree):
```
d 0755 0 0 - - usr/lib
f 0755 0 0 107592 <sha256> usr/lib/libz.so.1.3.2
l 0777 0 0 - libz.so.1.3.2 usr/lib/libz.so
```
`d` directory, `f` regular file (size, sha256), `l` symlink (target),
`h` hard link (target path; it carries its target's mode and owner).

Signature: `<package>.sig` next to the package, in minisign format
(Ed25519 over the BLAKE2b-512 hash of the file, with a trusted comment
naming the file).

## 4. Repositories

A repo is a directory, a `file://` path or an `http(s)://` URL (fetched
with curl) holding packages, their signatures, and `index.zst` + `index.zst.sig`. The index is every
package's `.PKGINFO`, plus `filename`, `csize` and `sha256` lines, records
separated by blank lines. `flpkg repo add` rebuilds and re-signs it.

`/etc/flpkg.conf`:
```
# repo NAME URL       repos are searched in this order
repo core file:///var/cache/flpkg/repos/core
keys = /etc/flpkg/keys          # trusted *.pub files
cache = /var/cache/flpkg
jobs = 0                        # 0: all CPUs
backup_config = no              # yes: save a replaced /etc file as FILE.flpkg-save
                                # when the user had changed it
cflags =
```

### Online repo (D3)

A repo is only static files, so any static host can serve it. Trust comes
from the signatures, not from the transport: `flpkg sync` checks
`index.zst` against the trusted keys (`/etc/flpkg/keys/*.pub`, minisign
format, Ed25519), and every download is checked against the sha256 in the
signed index and against its own `.sig`. The trusted comment of a signature
names the file, so a signature cannot be moved to another file. A tampered
or unknown-key index or package is refused and deleted.

The signed comment of `index.zst.sig` also carries a `serial` (rises with every
signing) and `valid-until` (default 14 days, `repo add --valid-days N`). `flpkg
sync` and `check-updates` refuse an expired index and one whose serial is lower than
the last accepted (kept in `/var/lib/flpkg/index-serial.REPO`), with a message that
says what to do; so a mirror cannot freeze or roll back a machine. The repo must
therefore be re-signed (re-run `70-repo.sh`) before it runs out. Several public keys
may be trusted, and a new key can be introduced by the old one (`flpkg key introduce`,
`flpkg key list`): see docs/SECURITY.md.

- `flpkg sync` (`-Sy`): fetch and verify the indexes into `/var/cache/flpkg/repos`.
- `flpkg search`, `flpkg info --repo`: read those indexes.
- `flpkg upgrade [--sync]` (`-Su`, `-Syu`): download, verify and install
  through the normal transaction (so the pre-transaction btrfs snapshot,
  `snapshot_before()` in `txn_commit`, applies to repo upgrades too).
- `flpkg check-updates`: for other programs (the update notifier, D1). It
  fetches and verifies the indexes into a private temporary directory (no
  root, the system cache is not touched; `--offline` uses the last `sync`
  instead), then prints what `upgrade` would install, nothing else on
  stdout, one line per package, tab-separated:
  `NAME  INSTALLED  AVAILABLE  DOWNLOAD_BYTES  REPO`. `INSTALLED` is `-`
  for a package that is new (a dependency, or one that replaces an installed
  package). `--count` prints just the number of lines. Exit 0 when the check
  worked, whether or not there are updates; 1 on any error (offline, bad
  signature), 2 on bad usage.

### One command for all software (round 22)

`flpkg install|remove|upgrade|search|list`, typed by a person, goes through `/usr/lib/flpkg/sources.sh` (source:
`src/flpkg/sources.sh`), which uses the commands above for the FreeLFS repo and adds three more sources, in this order:
FreeLFS repo, Flathub (`flatpak --user`, no root), Nix (package `nix`) and Distrobox containers (Ubuntu apt,
Arch pacman, Fedora dnf; packages `distrobox` and `podman`, real since round 24a: see below).

- `flpkg install NAME`: every source that has NAME is listed ("1) FreeLFS repository (preselected) 2) Flathub ..."); Enter
  takes the FreeLFS one. `--from=freelfs|flatpak|nix|apt|pacman|dnf` skips the question. Nothing found in the first three:
  it offers to try apt, pacman or dnf, and creating a container always asks first (with the download size).
- Containers (round 24a). `flpkg install --from=apt|pacman|dnf NAME` asks, then runs `distrobox create --yes -n freelfs-BACKEND
  -i IMAGE --no-entry --unshare-process` (rootless podman; images `docker.io/library/ubuntu:24.04`, `archlinux:latest`,
  `fedora:45`), refreshes the package lists in the box, installs NAME with the box's own package manager, then exports it:
  `distrobox-export --app NAME` (menu entry `freelfs-BACKEND-NAME.desktop` in `~/.local/share/applications`) and
  `--bin` into `~/.local/bin` (the command has the same name). `flpkg remove NAME` removes the export and the package;
  `flpkg upgrade` runs `distrobox upgrade` for every box. The question names the download and disk size and, on a live system
  without persistence, warns that the container lives in RAM and shows the free RAM. On a live system the home is overlayfs,
  which the kernel cannot stack, so flpkg writes `~/.config/containers/storage.conf` with `mount_program` fuse-overlayfs; on an
  installed system podman uses the kernel overlay. Containers run without their own cgroup (no systemd, nobody delegates one);
  hence `--unshare-process`. Container traffic is the host's: the firewall's inbound rules apply to it unchanged.

- On a live system without persistence a Flatpak install warns that it goes into RAM, with its size, and asks.
- `flpkg remove NAME` uses the source that installed it (FreeLFS: flpkg's database; the others:
  `~/.local/state/flpkg/sources.list`, plus Flatpak apps found by name).
- `flpkg upgrade` runs each source's own update; the FreeLFS part is `flpkg-seat upgrade` (one flauth prompt, none for the
  live tester). `flpkg search` and `flpkg list` show every source on a terminal (or with `--all`); into a pipe they stay the
  plain FreeLFS output the Store and Packages read. Native options (`--cascade`, `--recursive`, `--needed`, `--asdeps`, `--nodeps`,
  `--unsigned`, `--overwrite GLOB`) are passed on to the FreeLFS source (as root; the password-free route for a person,
  `flpkg-seat`, takes names only, so a person is told to use root); unknown options are an error. Root with `-y`, `--nodeps`,
  `--unsigned` or a package file (flpkg-seat, build scripts), `--root`, `--config` or `FLPKG_NO_SOURCES=1` run the native commands.

### Nix (round 24b)

`flpkg install --from=nix NAME` installs from nixpkgs, through the real `nix` client (package `nix`, recipes/nix: Nix 2.35.6 built from the
official source tag, multi-user). The backend is `src/flpkg/sources-nix.sh`, which `sources.sh` sources with one line. It searches with
`nix search nixpkgs TEXT --json`, installs with `nix profile install nixpkgs#ATTR`, and removes, lists and upgrades with `nix profile`.

- **Search (round 24a fix).** `nix search nixpkgs` evaluates all of nixpkgs: it needs about 3.3 GB of RAM (the kernel killed it in a
  3.5 GB guest), so flpkg does not use it. Search reads a cached list built once from the channel's `packages.json.br` (10 MB download,
  streamed through python, about 9 MB result, kept in `~/.cache/flpkg/nix-index.tsv` for a week; on a live system that is RAM).
  `--from=nix NAME` uses the name as typed (or the exact entry of the list when it exists) and needs no list.
- **State directory.** Nix is built with `localstatedir=/var`, so its state and the daemon socket are in `/var/nix` (the store stays
  `/nix/store`). `nix` prints ANSI codes even into a pipe: flpkg sets `NO_COLOR=1` for it.
- **Live RAM.** `nix profile install hello` took about 115 MB of RAM on the live system (43 MB store, 70 MB `~/.cache/nix`): files, not
  processes. At idle only `flsocketd` (1.6 MB) stays; the daemon exits 60 s after the last client.
- **Service.** Nix is a client; `nix-daemon` does the work. The rc service `nix` (config/etc/rc.d/init.d/nix) runs flsocketd on
  `/var/nix/daemon-socket/socket`, and the first connection starts nix-daemon through `/usr/lib/nix/nix-daemon-ondemand`,
  which stops it after 60 s with no client. Idle RAM is flsocketd only (about 0.2 MB).
- **Store.** `/nix/store` is group `nixbld`, mode 1775. Build users `nixbld1`..`nixbld8` (uids 30001-30008) and group `nixbld`
  (30000) come from the recipe's `sysusers`/`sysgroups`. `/etc/nix/nix.conf` sets experimental features (nix-command, flakes),
  sandbox, two build jobs, and the CA bundle. `~/.nix-profile/bin` is on PATH through the profile script.
- **Live system without persistence.** `/nix` lives in RAM. The first install asks first and gives the size nix predicts
  (`nix build --dry-run`). `--yes` (the Store asks itself) skips the question.
- **Not in the base.** Nix is not part of any group, so it is not installed by default.

### Publishing on GitHub Releases (round 22)

`build/70-repo.sh` (with `FLFS_REPO_URL=https://github.com/wasselnl77-hash/freelfs/releases/download/repo-x86_64`, already
in `config/flpkg/repo-url`) makes and signs the repo; `build/70-publish-repo.sh` stages it as hard links in
`~/freelfs-work/publish/repo-x86_64/` and writes `publish/upload.sh` (`gh release create` once, then `gh release upload
--clobber` for the changed files, the index last, stale assets deleted). It uploads nothing. The private key stays in
`~/freelfs-work/repo-key/` (`.gitignore` covers `*.key` and `repo-key/`; the script refuses to stage a key). The index is
valid 14 days: run `70-repo.sh`, `70-publish-repo.sh` and `upload.sh` again before it expires.

### Publishing a repo

`build/70-repo.sh` (after `60-packages.sh`) copies the new or changed
packages from `$FLFS/.bootstrap/pkg/out` into `~/freelfs-work/repo/<arch>/`,
signs them, rebuilds and signs `index.zst`, and checks the result by syncing
it into a scratch root. The signing key is made on the first run in
`~/freelfs-work/repo-key/freelfs.{key,pub}` (outside the source tree, mode
0600; never commit or upload `.key`; back it up, because installed systems
only trust this key). The public key is copied to
`config/flpkg/keys/freelfs-repo.pub`; the flpkg recipe ships it in
`/etc/flpkg/keys/`, so images trust the repo. With `FLFS_REPO_URL` set, the
script also writes `config/flpkg/repo-url` and the flpkg recipe puts
`repo freelfs URL` into the image's `/etc/flpkg.conf`.

To publish, upload the contents of the repo directory unchanged to any
static host so that `URL/index.zst`, `URL/index.zst.sig` and
`URL/NAME-VERSION-ARCH.flpkg[.sig]` resolve. A Hugging Face dataset works
(the `resolve/main` path serves raw files):
```
hf upload YOURNAME/freelfs-repo ~/freelfs-work/repo/x86_64 x86_64 --repo-type dataset
# URL: https://huggingface.co/datasets/YOURNAME/freelfs-repo/resolve/main/x86_64
```
GitHub Pages, an S3 bucket or `rsync` to any web server work the same way.
Re-run `70-repo.sh` and upload again for each new build; clients pick it up
with `flpkg sync`. On a running system: add `repo freelfs URL` to
`/etc/flpkg.conf` and the public key to `/etc/flpkg/keys/`.

### Update notifier (D1)

`flupdate-notify` (src/flupdate, started by the session) runs
`flpkg check-updates` and `flatpak remote-ls --updates --user` five
minutes after login and then daily, and posts one notification, "N updates
available"; clicking it runs `flstore --updates`. It never installs
anything.

## 5. Database

`/var/lib/flpkg/db.sqlite`:
- `packages`: name, base, version, summary, url, arch, license, builddate,
  installdate, size, reason (`explicit` or `dep`).
- `relations`: package, kind (`depend`, `provide`, `conflict`, `replace`,
  `group`), value.
- `files`: path (unique), package, type, mode, uid, gid, size, sha256,
  target.
- `dirs`: path, package (a directory can belong to several packages; it is
  removed when the last owner goes and it is empty).
- `pending_owners`: path, uid, gid, mode, for ownership a rootless install
  could not set (`flpkg fix-owners` applies them; `rc.sysinit` runs it).
- `history`: time, action, package, old version, new version.

## 6. Transactions

`install`, `upgrade` and `remove` are one transaction each:
1. **Resolve** the target set (section 7).
2. **Check**: signatures and sha256 of every package; file conflicts with
   other packages and with untracked files on disk.
3. **Plan**: every new file gets a hidden temporary name
   (`.flpkg.<id>.<n>.new`) in its destination directory, and every file
   that will be replaced or removed a hidden backup name (`….old`), so
   every rename stays on one file system and is atomic. The plan goes to
   `/var/lib/flpkg/journal`, synced before anything changes.
4. **Stage**: write the new files under their temporary names, checking
   each against the manifest (size, sha256).
5. **Commit**: rename each replaced file to its backup name and the new one
   into place; rename removed files to backups.
6. **Record**: update the database in one SQLite transaction, which also
   stores the transaction id. That commit is the point of no return.
7. **Clean up**: delete the backups, remove emptied directories, run the
   triggers (section 8), delete the journal.

An error before step 6 undoes everything at once. After a crash or power
loss, the next flpkg command reads the journal: if the database holds the
transaction id, it finishes step 7; otherwise it rolls back, deciding for
each file from which of its names exist. The test suite crashes flpkg
after steps 4, 5 and 6 and checks both outcomes. In M4, a btrfs snapshot
before each transaction adds a second safety net.

Config files: anything under `/etc`. The new version always replaces the
installed one. With `backup_config = yes`, a file the user had changed
(sha256 differs from what the old package installed) is first saved as
`FILE.flpkg-save`.

Users and groups: `sysgroups` and `sysusers` are added straight to
`/etc/group`, `/etc/passwd` (and `/etc/gshadow`, `/etc/shadow` when they
exist) before files are committed, and never removed. No package ships
those files, so the "always replace /etc files" rule never drops an
account: the base package declares the standard accounts the same way.

## 7. Dependency resolution

- Versions: `[epoch:]version-release`, compared segment by segment like
  rpm (`1.10 > 1.9`, `1.0a < 1.0b`, `1.0 < 1.0.1`).
- Installing: every `depend` must be satisfied by an installed package, a
  package in the same transaction, or a repo package (exact name first,
  then the first provider in repo order). Pulled-in packages are marked
  `dep`.
- `conflicts`: a conflicting installed package is removed only if the new
  package `replaces` it; otherwise the transaction stops.
- `upgrade` also replaces installed packages that a repo package lists in
  `replaces`.
- Removing: refused while an installed package still depends on the target,
  unless `--cascade`.
- Groups: `flpkg install base` installs every package in group `base`.

## 8. Triggers

flpkg itself runs these when a transaction touched matching paths, once
per transaction, and only if the command exists. They are listed in
`/usr/lib/flpkg/triggers` (shipped with flpkg, not by packages). With
`--root`, each trigger runs chrooted into the root, in a mount namespace of
its own with `/proc`, `/sys` and `/dev` mounted there; the mounts vanish
with the trigger.

| Paths | Command |
|---|---|
| `usr/lib/*.so*`, `etc/ld.so.conf*` | `ldconfig` |
| `usr/lib/modules/*` | `depmod -a <version>` |
| `usr/lib/udev/hwdb.d/*`, `etc/udev/hwdb.d/*` | `udev-hwdb update` |
| `usr/share/info/*` | rebuild `usr/share/info/dir` |
| `usr/share/fonts/*` | `fc-cache` |
| `usr/share/icons/*` | `gtk-update-icon-cache` |
| `usr/share/mime/*` | `update-mime-database` |
| `usr/share/glib-2.0/schemas/*` | `glib-compile-schemas` |
| `usr/share/applications/*` | `update-desktop-database` |

## 9. Rootless use

The FreeLFS bootstrap runs in a user namespace where only uid 0 exists.
flpkg works there unchanged: when `chown` to another uid fails, the
ownership goes into `pending_owners` instead, and `flpkg fix-owners` (run
by `rc.sysinit` at first boot) applies it. This replaces
`config/ownership.list` once the base system comes from packages.

## 10. M3 work plan

1. Spec (this file).
2. Core library: tar reader/writer, zstd streams, sha256/BLAKE2b,
   `.PKGINFO`/`.FILES`, version comparison. Unit tests.
3. `flpkg build`: recipe helper, source fetching and verification,
   packaging, signing.
4. Database and `install`/`remove`/`upgrade` with transactions and
   rollback; `fix-owners`; triggers.
5. Dependency resolution, groups, repos and their signed indexes.
6. Recipes for every M1/M2 package; build them all, install `base` into an
   empty root, build the image from it, boot test.
7. Reproducibility: build every package twice, compare hashes.
8. Self-hosting: inside the booted image, flpkg rebuilds packages.
