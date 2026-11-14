# 20. Building FreeLFS from source

Summary: how to build the whole system, the package repository, the disk image and the live ISO from the
FreeLFS tree on a Linux host, as a normal user, without sudo, and what each script in `build/` does.

## Contents

1. What you get at the end
2. Host requirements (what `build/00-host-check.sh` checks)
3. Settings you can change (`build/config.sh`)
4. The build stages in order
5. The package stage in detail (`build/60-packages.sh`)
6. Images and the ISO (`50-image.sh`, `51-qemu.sh`, `52-usb.sh`, `53-iso.sh`)
7. Locks: pinned sources, pinned packages, the ownership list, stamps
8. Rootless builds: how the chroot works without root
9. Build times
10. Reproducibility
11. Helper scripts
12. Troubleshooting and resuming

---

## 1. What you get at the end

The build produces these files, all under your work directory (default `~/freelfs-work`):

| Output | Made by | What it is |
|---|---|---|
| `rootfs-x86_64/` | stages 01 to 41 | the target root, a plain directory your user owns |
| `rootfs-x86_64/.bootstrap/pkg/out/*.flpkg` | `60-packages.sh` | one package file per recipe |
| `rootfs-x86_64/.bootstrap/pkg/root/` | `60-packages.sh` | the system installed from the packages (group `base` and `desktop`) |
| `freelfs-x86_64.img` | `50-image.sh` | the GPT disk image (EFI system partition and btrfs), 80 GB sparse by default |
| `freelfs-usb.img` | `52-usb.sh` | the same image for a USB stick, 8 GB by default |
| `freelfs-live.iso` | `53-iso.sh` | the live ISO (a CD image that is also a GPT disk) |
| `repo/x86_64/` | `70-repo.sh` | a static, signed package repository |

The default `FLFS_WORK` is `$HOME/freelfs-work`. Every output above lives under it.

## 2. Host requirements

`build/00-host-check.sh` prints an OK, WARN or FAIL line for each check and exits 1 when a required tool
fails. The required tools follow the LFS 13.1 host requirements. The minimum versions in the script are:

| Tool | Minimum | Tool | Minimum |
|---|---|---|---|
| Bash | 3.2 | Make | 4.0 |
| Binutils (`ld`) | 2.13.1 | Patch | 2.5.4 |
| Bison | 2.7 | Perl | 5.8.8 |
| Coreutils (`sort`) | 8.1 | Python 3 | 3.4 |
| Diffutils | 2.8.1 | Sed | 4.1.5 |
| Findutils | 4.2.31 | Tar | 1.22 |
| Gawk | 4.0.1 | Texinfo | 5.0 |
| GCC and G++ | 5.2 | Xz | 5.0.0 |
| Grep | 2.5.1 | Gzip | 1.3.12 |
| M4 | 1.4.10 | | |

Other requirements the script checks:

- Linux kernel 5.4 or newer.
- UNIX 98 pseudo terminals (`devpts` mounted on `/dev/pts`, `/dev/ptmx` present).
- `sh` is bash, `awk` is GNU awk, `yacc` is Bison.
- `g++` can compile and link a test program.

Optional tools (a WARN, not a failure, when missing): `qemu-system-x86_64`, `qemu-system-aarch64`, `zstd`,
`mkfs.btrfs`, `mkfs.fat`, `losetup`, `git`, `curl`, `sqlite3`, and OVMF firmware (UEFI for QEMU, looked
for in `/usr/share/OVMF`, `/usr/share/edk2`, `/usr/share/ovmf` and `/usr/share/qemu`).

The script also prints the CPU thread count and the RAM. It notes when there are less than 16 GB of RAM,
because large builds (LLVM, Firefox) then need extra swap.

Rules that the scripts enforce, not the script above:

- Run as your normal user. `lib.sh` refuses to run as root (`clean_env` and `chroot_env` both die with
  "do not build as root").
- User namespaces must work, because the chroot stages run through `unshare --map-root-user` (see section 8).
- Disk space: plan for the sparse 80 GB image plus the build tree. The build tree is the large part.

Known host limit recorded in the repo: `/tmp` on the development host was a 3.6 GB tmpfs, so build scratch
space should go elsewhere (`TMPDIR=~/freelfs-work/scratch/tmp`, from `docs/INTEGRATION-TODO.md`).

## 3. Settings you can change

`build/config.sh` is sourced by every stage and never run by itself. Override any value from the environment,
for example `FLFS_ARCH=aarch64 ./build/...`. An aarch64 build is not expected to work: the aarch64 port has not started (chapter 28).

| Variable | Default | Meaning |
|---|---|---|
| `FLFS_ARCH` | `x86_64` | target architecture; `FLFS_TGT` becomes `ARCH-freelfs-linux-gnu` |
| `FLFS_WORK` | `~/freelfs-work` | work area on the host |
| `FLFS_CACHE` | `$FLFS_WORK/cache/sources` | downloaded upstream sources |
| `FLFS` | `$FLFS_WORK/rootfs-$FLFS_ARCH` | the target root (empty inside the chroot) |
| `FLFS_JOBS` | RAM in GB minus 4, at least 1, at most the CPU count | parallel jobs; also sets `MAKEFLAGS`, `NINJAJOBS` and `TESTSUITEFLAGS` |
| `FLFS_CHECK` | `core` | package test suites: `none`, `core` (glibc, binutils, gmp, mpfr, mpc) or `all` |
| `FLFS_IMAGE_ROOT` | `$FLFS/.bootstrap/pkg/root` | the root that `50-image.sh` turns into an image |
| `FLFS_IMG` | `$FLFS_WORK/freelfs-$FLFS_ARCH.img` | the image file |
| `FLFS_IMG_SIZE` | `80G` | image size (sparse file) |
| `SOURCE_DATE_EPOCH` | `1767225600` (2026-01-01) | fixed build time for reproducible output |

`config.sh` also sets `LC_ALL=POSIX`, and exports `FLFS`, `FLFS_TGT` and `FLFS_ARCH`.

The job count formula (`FLFS_JOBS`) is written for the current 7 GB build host. A 16 GB build machine is planned
for about mid-January 2027 (chapter 28); the limits will then be raised. The environment variable overrides the formula.

## 4. The build stages in order

`build/all.sh` runs these stages in this order: `00-host-check`, `01-prepare`, `02-fetch`, `11-cross-toolchain`,
`12-temp-tools`, `20-chroot-tools`, `30-base-toolchain`, `31-base-system`, `32-base-extra`, `40-system-config`,
`41-kernel`, `60-packages`, and finally `50-image`. Run the others by hand as needed.

Each stage is resumable. A finished step writes a stamp file, and the next run skips it (see section 7).

| Stage | Book chapter | What it does |
|---|---|---|
| `00-host-check.sh` | (FreeLFS) | Checks the host (section 2). Writes nothing. |
| `01-prepare.sh` | LFS 4.2 | Creates the target root directories: `etc`, `var`, `usr/{bin,lib,sbin}`, the `bin`/`lib`/`sbin` links into `usr`, `lib64`, the stamp and source directories, and `tools` until LFS chapter 7 removes it. |
| `02-fetch.sh` | (FreeLFS) | Downloads every URL in `sources.lock` into the cache, checks the SHA256 of each file and refuses a mismatch. Then hard-links each file into `$FLFS/sources`. |
| `11-cross-toolchain.sh` | LFS ch. 5 | Cross binutils and GCC pass 1 into `$FLFS/tools`; Linux API headers, glibc and libstdc++ into the target root. Runs on the host, in a clean environment (`env -i`). |
| `12-temp-tools.sh` | LFS ch. 6 | Cross-compiles the temporary tools (m4, ncurses, bash, coreutils and the rest) into the target root, then builds binutils and GCC that run on the target. The last of these steps is stamped `6.18-gcc-pass2`, which `20-chroot-tools.sh` checks for. |
| `20-chroot-tools.sh` | LFS ch. 7 | Inside the chroot: creates the directory tree and the essential files (`/etc/passwd`, `group`, `hosts`, logs), then builds gettext, bison, perl, zlib, mpdecimal, Python, texinfo and util-linux. Drops the cross toolchain (stamp `7.15-cleanup`). |
| `30-base-toolchain.sh` | LFS ch. 8, sec. 3 to 32 | Inside the chroot: the final glibc, compression libraries and toolchain, ending with the final GCC and its sanity check (stamp `8.32-toolchain-check`). Test suites follow `FLFS_CHECK`. |
| `31-base-system.sh` | LFS ch. 8, sec. 33 to 85 | The rest of the base system. No systemd: udev is built from the systemd source on its own, plus sysklogd and sysvinit (the LFS 12.4 sysv way). No GRUB: FreeLFS uses its own boot loader, flboot. Stamp `8.84-sysvinit`. |
| `32-base-extra.sh` | (BLFS) | btrfs-progs (with lzo, for the root file system) and dhcpcd (networking). Stamp `x.3-dhcpcd`. |
| `40-system-config.sh` | LFS ch. 8.84 to 9 | Strips the base system (debug symbols of the loader and libc go to separate `.dbg` files). Installs FreeLFS's own configuration from `config/etc`: inittab, `rc.conf`, the `rc` boot scripts, `fstab`, the profile. Stamp `9.3-etc`. |
| `41-kernel.sh` | LFS ch. 10 | Builds the kernel that `sources.lock` pins (the `linux` source), from the architecture's defconfig merged with `config/kernel/freelfs.config`, then checks that every setting of the fragment survived `olddefconfig`. Installs the kernel to `/boot` and modules to `/usr/lib/modules`. Stamp `10.3-kernel`. (The script's header still says "LTS"; the package recipe `linux` now builds the newest stable 7.x, see chapter 21.) |
| `60-packages.sh` | (FreeLFS) | Builds every recipe in `recipes/` into a package, signs them into a local repository and installs groups `base` and `desktop` into an empty root. Section 5. |
| `50-image.sh` | (FreeLFS) | Turns the root from `60-packages.sh` into the GPT disk image. Section 6. |
| `51-qemu.sh` | (FreeLFS) | Boots the image in QEMU with UEFI firmware. Section 6. |
| `52-usb.sh` | (FreeLFS) | Makes the USB-stick image file. Never writes to a device. Section 6. |
| `53-iso.sh` | (FreeLFS) | Makes the live ISO. Section 6. |
| `61-repro-check.sh` | (FreeLFS) | Builds every recipe a second time and compares the packages. Section 10. |
| `70-repo.sh` | (FreeLFS) | Turns the packages into a signed, static repository (section 7 below and chapter 13 for the format). |

The chroot stages (`20` to `41`, `60`, `61`) run through `build/chroot.sh` in a user namespace (section 8).
Stages `00`, `01`, `02`, `11`, `12`, `50`, `51`, `52`, `53` and `70` run on the host.

Typical first run, on a host that has the tools:

```sh
cd ~/Dokumente/FreeLFS            # the tree (any path works)
./build/00-host-check.sh          # must end with "Host check passed."
./build/all.sh                    # long: hours (section 9)
./build/51-qemu.sh                # boot the image in QEMU
```

Use `./build/gentle.sh` in front of a long stage to keep the desktop usable (section 12).

## 5. The package stage in detail (`build/60-packages.sh`)

This is the stage that makes FreeLFS a distribution: every recipe in `recipes/` becomes a `.flpkg`.

What it does, in order (each is a stamped step):

1. `p.0-flpkg` (`tool_step`): copies `src/flpkg` into `$WORK/flpkg-src` and builds `flpkg` there. It is stamped:
   after you change `src/flpkg`, delete the stamp `p.0-flpkg` so that the new tool is built.
2. `p.1-setup` (`setup_step`): makes a bootstrap signing key `$WORK/keys/bootstrap` (once, with
   `flpkg key generate`) and writes `$WORK/flpkg.conf` (build directory, cache, keys, job count and a `repo base`
   line pointing at `$WORK/repo`).
3. `pkg-NAME` for each recipe (`build_one`): runs `flpkg build --nocheck --sources /sources -o $OUT recipes/NAME`.
   If the recipe is in group `desktop` or `live`, the new package is installed into the build root at once
   (`flpkg install -y --nodeps --unsigned`), so that later desktop packages can build against it.
4. `p.9-repo` (`repo_step`): `flpkg repo add --key ... $REPO $OUT/*.flpkg` makes the local signed repository.
5. `p.9-root` (`root_step`): `flpkg --root $ROOT sync`, then `flpkg --root $ROOT install -y base desktop`.
6. `p.9-firstboot` (`firstboot_step`): what the installer will do later, done here for the development image:
   shadow files (`pwconv`, `grpconv`), no password for root, a note in `/etc/issue`, time zone UTC, empty
   `subuid`/`subgid` files, the public repository key copied to `/etc/flpkg/keys`, and the package cache removed.

Which recipes are built:

- With no arguments, every directory in `recipes/` whose group list does not contain `deferred`.
- With names (`./build/60-packages.sh flui flcomp`), only those, plus the repository and root steps. The
  stamps of the named packages are removed first, so they are rebuilt. Dependencies are ordered with `tsort`
  (`order_recipes`), but only among the names given.

Important: `60-packages.sh` always passes `--nocheck`. The `check()` function of a recipe is not run during
this stage. Run the test suites yourself (chapter 25), or use `FLFS_CHECK` in the base stages.

Groups (from the recipes): `base` (94 recipes), `desktop` (171), `live` (5: `flinitrd`, `libburn`, `libisofs`,
`libisoburn`, `squashfs-tools`), `lts` (1: `linux-lts`), `deferred` (2: `libclc`, `spirv-llvm-translator`,
built only when named), and `flpackages`, which has no group. The root step installs `base` and `desktop`
only; `lts` and `live` are not in the image.

Output: `$WORK/out/*.flpkg`, the repository in `$WORK/repo`, the installed root in `$WORK/root`.
The image is then made from that root:

```sh
FLFS_IMAGE_ROOT=$FLFS/.bootstrap/pkg/root ./build/50-image.sh
```

The last line `60-packages.sh` prints says the same thing. (`$FLFS` there is the target root, for example
`~/freelfs-work/rootfs-x86_64`.)

## 6. Images and the ISO

### 6.1 `50-image.sh`: the disk image

Makes `FLFS_IMG` (default `~/freelfs-work/freelfs-x86_64.img`), a GPT disk:

- Partition 1: EFI system partition, FAT32, label `ESP`, 512 MB. It holds flboot as `EFI/BOOT/BOOTX64.EFI`,
  the kernels from `/boot`, and `flboot/flboot.conf`. It is mounted at `/boot` on the running system.
- Partition 2: btrfs, label `freelfs`, with subvolumes `@` (the default one, mounted at `/`), `@home`
  (mounted at `/home`) and `@snapshots`.

The partition and file system IDs are fixed (`DISK_ID`, `ESP_PARTUUID`, `ROOT_PARTUUID`, `BTRFS_UUID`), so the
same root gives the same image and `flboot.conf` can name the root by PARTUUID. The image is a development
image, not an installed system: the installer makes fresh IDs.

No root is needed. Every tool (`sfdisk`, `mkfs.fat`, `mtools`, `mkfs.btrfs`) is run from the FreeLFS root
itself through its own dynamic loader (`tool()` in the script). The btrfs file system is filled from the root
laid out as bind mounts in a throwaway mount namespace, so files are stored as owned by root.

The image step refuses to run on a host of another architecture (`uname -m` must equal `FLFS_ARCH`).

### 6.2 `51-qemu.sh`: boot the image

Boots the image the way a PC does: OVMF (UEFI) starts flboot from the ESP, flboot starts the kernel. Uses KVM
when `/dev/kvm` is usable.

```sh
./build/51-qemu.sh                 # graphical window and serial console in this terminal
./build/51-qemu.sh --serial        # serial console only
./build/51-qemu.sh --iso FILE      # boot the live ISO as a CD, no disk
./build/51-qemu.sh -- QEMU-ARGS    # extra arguments for QEMU
```

Settings: `FLFS_VM_MEM` (default `2G`), `FLFS_VM_CPUS` (default 2), `FLFS_VM_SERIAL` (default `mon:stdio`),
`FLFS_VM_VGA` (default `-vga std`; `virtio-vga-gl` for a virgl GPU), `FLFS_VM_NIC`, and `FLFS_VM_VARS` (the
firmware variable file; default `$FLFS_WORK/ovmf-vars.fd`). In serial mode Ctrl-A X quits QEMU.

### 6.3 `52-usb.sh`: the USB-stick image

The same image as `50-image.sh`, with two differences: it is 8 GB by default, and it has a user `tester`
with an empty password. The keyboard is not preset, so the first boot asks for it. The script writes only
`$FLFS_WORK/freelfs-usb.img`. It prints a `dd` command with a placeholder device name for you to run yourself.
It never writes to a block device.

### 6.4 `53-iso.sh`: the live ISO

Makes `~/freelfs-work/freelfs-live.iso`, rootless:

- `live/root.sfs`: a squashfs (zstd, level 15 by default, `--level N`) of the installed root with the user
  `tester` (empty password, home `/home/tester`) and a live `fstab` with no `@home`, `@snapshots` or ESP. Nothing
  in the live session mounts or writes the host's disks.
- `EFI/BOOT/BOOTX64.EFI` (flboot), El Torito UEFI boot image, and an appended FAT ESP image (flboot, kernel,
  `initrd.img`, `flboot.conf`) that is also GPT partition 2. So the same file boots as a CD, can be written to a
  stick with `dd`, or can be a file on a Ventoy stick. `src/flinitrd` finds the ISO again on the stick.
- `--cmdline "EXTRA OPTIONS"` adds kernel options. A test can add `console=ttyS0,115200` last, to read the boot
  on the serial port (the "serial twin" in chapter 25).

It needs the `live` packages in the build root: `./build/gentle.sh ./build/60-packages.sh linux squashfs-tools libburn libisofs libisoburn flinitrd`.
The squashfs and the ISO are made inside the chroot, with the root overlaid by overlayfs in the namespace, so
the real root is never changed. Run it as `./build/gentle.sh ./build/53-iso.sh`.

The top-level `README.md` says the M9 ISO was 843 MB at that time. The current size is not verified.

## 7. Locks: pinned sources, pinned packages, the ownership list, stamps

"Lock" in this tree means four different things. Keep them apart.

**`sources.lock`: pinned upstream sources.** One line per source: `SHA256  FILE  URL`. It is generated by
`build/update-lock.sh` from `sources.list` (one URL per line, `#` for comments), and says "do not edit" in its header.
`update-lock.sh` downloads every URL into the cache (it reuses files already there), hashes them and writes the
new lock through a temporary file that is renamed into place. Optional `-m` (md5sums) and `-s` (sha256sums)
options cross-check each hash against upstream reference lists (for example the LFS md5sums or kernel.org's
`sha256sums.asc`); a mismatch stops the update. `02-fetch.sh` downloads and verifies against this lock.

To add a source: add its URL to `sources.list`, run `./build/update-lock.sh`, review the diff of `sources.lock`.

**Recipe checksums.** Each recipe lists `sha256=()` for its own sources, one entry per `source=()` entry. `flpkg`
refuses a recipe whose checksum is not 64 lowercase hex characters, or whose `sha256` count differs from the
`source` count. `SKIP` is not allowed (chapter 21).

**`config/ownership.list`: owners the rootless build cannot set.** The chroot maps one user ID, so `chown` to any
other user or group fails there. A build step that needs such an owner calls `need_owner PATH UID GID MODE`,
which fails unless the line is listed. Current entries: `/var/log/lastlog` (group 13, `utmp`), `/usr/bin/wall`
and `/usr/bin/write` (group 5, `tty`, setgid), and `/var/lib/dhcpcd` (uid and gid 52). The image step applies
these owners. Package recipes do not use this file; they use `owner=()` (chapter 21).

**Stamps: resumable steps.** Each finished step writes an empty file in `$FLFS_BOOTSTRAP/stamps` (inside the target
root, `.bootstrap/stamps`). The next run prints "already done" and skips it. Examples: `7.15-cleanup`,
`8.32-toolchain-check`, `8.84-sysvinit`, `x.3-dhcpcd`, `9.3-etc`, `10.3-kernel`, `p.0-flpkg`, `p.1-setup`,
`pkg-NAME` (one per recipe), `p.9-repo`, `p.9-root`, `p.9-firstboot`. To redo a step, delete its stamp. Deleting
a stamp does not undo the work; the step runs again on top of what is there.

Each step writes a log to `$FLFS_WORK/logs/STEP.log`, and a failure prints the last 25 lines and the log path.
A successful step prints its duration (`step: ok (3m12s)`), which is the only timing the build records.

**Repository signing key.** `70-repo.sh` makes an Ed25519 signing key (minisign format) outside the source tree,
by default `~/freelfs-work/repo-key/freelfs.key` (mode 0600, directory 0700). Only the public half goes into the
tree (`config/flpkg/keys/freelfs-repo.pub`). Losing the private key means installed systems refuse the repository
until they receive a new public key. `build/backup.sh` does not back up the key, on purpose.

## 8. Rootless builds: how the chroot works without root

Nothing in the build needs sudo or root.

- Stages `01`, `02`, `11` and `12` run as your user. They run through `clean_env`, which re-executes the stage in
  `env -i` with a minimal environment (`PATH` is set to the cross tools first, `CONFIG_SITE` to the target's
  `config.site`). This is how LFS's `lfs` build user works, and it keeps host settings out of the build.
- Stages `20` to `41`, `60` and `61` run through `chroot_env`, which calls `build/chroot.sh`. `chroot.sh` starts
  `unshare --map-root-user --mount --pid --fork --kill-child`. Inside, your user appears as root, and the mounts
  (`/dev`, `/proc`, `/sys`, a tmpfs on `/run`, the project tree at `/.bootstrap/freelfs`, the logs at
  `/.bootstrap/logs`) exist only in that namespace. They disappear when it ends, so the host is never touched.
- Inside the namespace `TAR_OPTIONS=--no-same-owner` is set, because tar cannot restore owners it cannot give.
- The limit: only one user ID exists in the namespace. Files cannot get other owners. That is what
  `config/ownership.list` is for (section 7).

The disk image and the ISO use the same idea: bind mounts in a throwaway namespace, and the FreeLFS tools
(`mkfs.btrfs --rootdir`, `mkfs.fat`, `mtools`, `squashfs`) run through their own loader.

## 9. Build times

The repository does not record a full build time. What is written down:

- gcc, LLVM, Mesa and Firefox "each take hours" (`docs/PLAN.md`, section 5, Risks).
- The FreeBWS WebKit rebuild (`wpewebkit`, 1252 ninja steps) was planned as "4 to 8 hours at 3 jobs" plus RAM for
  the final link (`docs/INTEGRATION-TODO.md`). The earlier build's log covered about three days of wall clock, because
  the build was resumed several times.
- Each stage prints its own duration when it succeeds (section 7), so your own run gives the real numbers.

Plan on hours for the first full build. Later builds reuse the stamps and the cache.

## 10. Reproducibility

Packages must come out the same bytes every time, so that anyone can check that a published package matches its
recipe. The mechanisms:

- `SOURCE_DATE_EPOCH=1767225600` is exported for every build. `flpkg` passes it to each recipe, and `pack.c` uses it
  as the mtime of every archive entry, sorts the entries bytewise, uses fixed compression parameters, and takes owners
  from `owner=()` (default 0:0).
- The kernel recipes pin `KBUILD_BUILD_TIMESTAMP`, `KBUILD_BUILD_USER=freelfs`, `KBUILD_BUILD_HOST=freelfs` and
  `KBUILD_BUILD_VERSION=1`.
- Other recipes pin their own timestamps and options where needed (for example a configure date for perl, and
  Python without profile-guided optimisation or its test suite; `docs/PLAN.md`, M3).
- `61-repro-check.sh` builds every recipe a second time, from scratch, into `out2`, and compares each package with
  the first one. It lists the files whose contents differ.

The M3 result is recorded in `docs/PLAN.md`: all 90 packages of that time were byte-identical across two
independent builds, after fixing one real bug that the check found (`flpkg` exported `$srcdir`, so `man-pages`
installed nothing). The repository now has about 270 recipes. A full re-check of all of them is not recorded
in the repository ("not yet verified").

## 11. Helper scripts

| Script | Purpose |
|---|---|
| `build/gentle.sh CMD...` | Runs a command with the lowest CPU and disk priority, no sleep while it runs, and a memory high-water mark (`FLFS_MEM_HIGH`, default RAM minus 4 GB) through a systemd user scope. Needs no sudo. |
| `build/chroot.sh [CMD...]` | Opens a shell (or runs CMD) inside the target root (section 8). |
| `build/backup.sh save NAME` / `restore NAME` | Saves or restores a snapshot of the target root, without sources and scratch space, to `$FLFS_WORK/backups/NAME.tar.zst`. Runs in a user namespace so files are archived as owned by root. |
| `build/update-lock.sh` | Regenerates `sources.lock` (section 7). |
| `build/test-report.sh` | Lists the package test suites that failed. A failing suite does not stop the build (`run_check` in `lib.sh` logs it); this script shows them. |
| `build/cve-check.py` | Checks the versions in `recipes/*/recipe` against public vulnerability data (OSV for PyPI, NVD for the rest, with the name map in `build/cve-names.conf`). `--md docs/CVE-REPORT.md` writes the report. |
| `build/cve-kernel.py` | Triages the kernel CVE candidates against the kernel config. Writes `docs/CVE-KERNEL.md`. |
| `build/m8-finish.sh`, `build/flatpak-finish.sh` | Unattended "finish" runs for two rounds (the WebKit build, and Flatpak). Read the header before running; they wait for other builds and use `gentle.sh`. |
| `build/m10-snapshots.sh`, `build/m10-install.py`, `build/mk-mp3.py`, `build/splash-shots.py` | Test helpers. Chapter 25 describes the tests. |
| `build/live-run.py`, `build/boot-test.py`, `build/iso-test.py`, `build/iso-test-disk.py`, `build/vm-run.py` | QEMU test drivers (chapter 25). |

## 12. Troubleshooting and resuming

- A failed step stops the stage and prints the last 25 lines of its log, plus the log path. Read the whole log in
  `$FLFS_WORK/logs/`.
- Fix the cause and run the same stage again. Finished steps are skipped (section 7).
- To rebuild one package: `./build/60-packages.sh NAME`. The stamp for that package is removed and the repository
  and root are rebuilt.
- Keep the desktop usable during long builds: `./build/gentle.sh ./build/60-packages.sh`.
- The `FLFS_JOBS` default is conservative. If a link step runs out of memory, lower `FLFS_JOBS`; if the build is
  slow and memory is free, raise it.
- The 7 GB build host cannot link clang (about 8 GB is needed). The Intel 3D drivers of Mesa therefore wait for the
  16 GB build machine planned for about mid-January 2027 (chapter 28). The recipes `llvm`, `spirv-llvm-translator` and `libclc` are kept in the repository
  in the group `deferred` (`docs/HARDWARE.md`, chapter 28).
- Re-running `./build/60-packages.sh` with no arguments rebuilds nothing that already has a stamp. A recipe that
  changed on disk still needs its stamp removed, or it is named on the command line.

Sources of truth in the tree: the stage scripts in `build/`, `build/lib.sh` and `build/config.sh`, `src/flpkg/build.c`
for what `flpkg build` does, and `docs/PLAN.md` for the history of the design.
