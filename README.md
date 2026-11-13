# FreeLFS

> **Pre-alpha. Do not use this for anything real.**
>
> FreeLFS is an experiment in progress. It has been tested almost only in
> virtual machines (QEMU) and on one real laptop, where it showed serious bugs.
> - **Things will break.** Expect crashes, missing features and incompatible
>   changes without notice.
> - **Nothing is stable yet:** not the package format, not the repo, not the
>   config files.
> - **No real security support:** known vulnerabilities are tracked
>   ([docs/SECURITY.md](docs/SECURITY.md)) and the browser's web processes run
>   in a bubblewrap sandbox, but there is no security team, no advisories and
>   no guaranteed update schedule.
> - **The installer erases the disk you pick.** Only try it on a spare machine
>   or in a VM.
> - Don't rely on it to keep your data safe.
>
> Try the live ISO in a VM or on a spare machine if you are curious. Bug
> reports are welcome.

An independent Linux distribution built from scratch, with no parent distro.
It has its own package manager, bootloader, shell, Wayland compositor and
desktop, all in C: a clean traditional desktop in the spirit of Linux Mint's
Cinnamon.

See [docs/PLAN.md](docs/PLAN.md) for the full plan and milestones.

## Status

- M0 (project foundation): done.
- M1 (cross toolchain and temporary tools): done.
- M2 (chroot, base system, first boot): done.
- M3 (`flpkg`, our package manager): done. The base system is 90 packages
  built from `recipes/`; flpkg installs them into an empty root, the image
  boots from that, and inside it FreeLFS rebuilds its own packages
  byte for byte.
- M4 (`flboot`, the real disk layout): done. The image is a GPT disk (EFI
  system partition + btrfs with subvolumes) that UEFI firmware boots through
  our own boot loader; flpkg snapshots the system before every change, and
  any snapshot can be booted from the menu.
- M5 (`flsh`, our own shell): done. New users log in to flsh: POSIX syntax,
  colours as you type, grey suggestions from history, Tab menus, "did you
  mean". See [docs/flsh-screenshot.png](docs/flsh-screenshot.png).
- The FreeLFS folder layout: the system in `/usr` (FHS), also reachable as
  `/sysflfs` (a link to it), homes in `/home/NAME`, disks in `/drives`.
  Until 2026-11-13 it was the other way round (system in `/sysflfs`, homes in
  `/usr`); see docs/PLAN.md for moving an older install.
- M6 (graphics and audio base): done. Mesa, Wayland, libinput, seatd, PipeWire,
  iwd and firmware; a normal user draws on the screen with EGL on KMS
  ([docs/m6-drawtest.png](docs/m6-drawtest.png)).
- M7 (our own desktop): compositor, toolkit, panel with start menu and a live
  tray (battery, network, volume), terminal, file manager, text editor, login
  screen, keyboard choice at first boot, settings, PipeWire in the session.
  Tested headless and in QEMU with 1 GB of RAM (docs/m7/). Compositing is on
  the CPU; on a real GPU, dmabuf buffers are read back through GLES (EGL is
  loaded only when a client needs it). The first boot on a real laptop found
  bugs (FreeBWS blank on the GPU, a Settings crash, no Wi-Fi page); they are
  fixed, but the whole desktop has not been run on that laptop again. The
  kernel has USB storage built in, so a USB stick image can boot
  (`build/52-usb.sh`).
- M8 (FreeBWS, the browser on WPE WebKit): built, runs in QEMU at 1 GB; the
  web processes run in WebKit's bubblewrap sandbox (since 2026-11-12).
- M10 integration round (2026-11-11): workspaces, screenshot tool, task
  manager, accent colours and wallpapers, system sounds, editor with syntax
  colours, archive tool, image/PDF/music/video apps, Software Store (Flathub
  and FreeLFS packages), portals for Flatpak apps, update notifier, btrfs
  snapshots with "start an earlier state", Xwayland (X11 apps), gaming kernel
  options, installer. All built into one image and exercised in QEMU at 2 GB
  and 1 GB; see [docs/ROADMAP-NEXT.md](docs/ROADMAP-NEXT.md) and
  [docs/INTEGRATION-TODO.md](docs/INTEGRATION-TODO.md) for what is open.
- M9 (release), first part: a live ISO, `./build/53-iso.sh` (rootless,
  843 MB). One file that boots as a CD, written to a stick with `dd`, or
  copied to a Ventoy stick (UEFI only, Secure Boot off); user `tester`, no
  password; nothing is written to the computer's disks. The installer
  ("FreeLFS install", also an icon on the live desktop) did a whole-disk
  install onto a virtual disk in QEMU, and the installed system booted. Not
  tried on real hardware or on a real disk; "next to existing partitions",
  "Manual" and the Wi-Fi step are untested. The live session keeps
  everything in RAM, so the Store does not add Flathub there. See "The live
  ISO" in [docs/PLAN.md](docs/PLAN.md). Still to do: Secure Boot (flboot is
  not signed), a public package repository, the laptop.
- Architectures: x86_64 only. Nothing has been built for aarch64.

## Quick start

The whole build runs as your normal user. **No sudo is needed at any point.**
`./build/all.sh` runs every stage below in order; or run them one by one:

```sh
./build/00-host-check.sh        # verify the build host has the required tools
./build/01-prepare.sh           # create the target root in ~/freelfs-work/rootfs-x86_64
./build/02-fetch.sh             # download + verify all sources, link them into the target
./build/11-cross-toolchain.sh   # M1: cross binutils, gcc, glibc, libstdc++
./build/12-temp-tools.sh        # M1: temporary tools + native binutils/gcc
./build/20-chroot-tools.sh      # M2: (chroot) gettext, bison, perl, Python, texinfo, util-linux
./build/30-base-toolchain.sh    # M2: (chroot) final glibc ... gcc
./build/31-base-system.sh       # M2: (chroot) the rest of the base system, udev, sysvinit
./build/32-base-extra.sh        # M2: (chroot) btrfs-progs, dhcpcd
./build/40-system-config.sh     # M2: (chroot) strip, /etc, our rc boot scripts
./build/41-kernel.sh            # M2: (chroot) the LTS kernel
./build/50-image.sh             # M2: assemble the btrfs disk image
./build/60-packages.sh          # M3: (chroot) every recipe -> signed packages, base -> new root
./build/61-repro-check.sh       # M3: (chroot) build everything again, compare byte for byte
./build/50-image.sh             # M4: GPT disk image (ESP + btrfs subvolumes) from the packages
./build/52-usb.sh               # M7: 8 GB image for a USB stick (user tester, no password); prints the dd command
./build/51-qemu.sh              # boot it in QEMU through UEFI and flboot (--serial: no window)
./build/gentle.sh ./build/60-packages.sh squashfs-tools libburn libisofs libisoburn flinitrd
./build/gentle.sh ./build/53-iso.sh   # M9: the live ISO, ~/freelfs-work/freelfs-live.iso
./build/51-qemu.sh --iso ~/freelfs-work/freelfs-live.iso   # boot the ISO as a CD
./build/boot-test.py            # headless boot, root login, checks, power off
./build/vm-run.py CMDS          # boot, run the commands in file CMDS, power off
```

`./build/all.sh` runs everything up to the image.

## flpkg

FreeLFS's package manager is in `src/flpkg` (C, with SQLite, zstd and
OpenSSL); its design is in [docs/flpkg.md](docs/flpkg.md). Recipes live in
`recipes/<name>/recipe`. In short:

```sh
flpkg build recipes/zlib          # recipe -> zlib-1.3.2-1-x86_64.flpkg
flpkg install zlib                # from the repos, with dependencies (-S)
flpkg upgrade --sync              # (-Syu)
flpkg remove --recursive foo      # (-Rs)
flpkg verify                      # check every installed file (-Qk)
```

Its test suites run inside the chroot:

```sh
./build/chroot.sh make -C /.bootstrap/freelfs/src/flpkg check
```

Each stage script is resumable. Finished packages are stamped, so re-running
after a failure continues where it stopped. Build logs go to
`~/freelfs-work/logs/`.

Helpers:
- `./build/chroot.sh` opens a shell inside the target (or runs a command
  there).
- `./build/backup.sh save|restore NAME` snapshots the target root, e.g.
  before chapter 8.
- `./build/test-report.sh` lists the package test suites that failed.
  `FLFS_CHECK=none|core|all` picks which suites run (default: core, the
  book's critical ones).

How it stays rootless:
- The target root is a plain directory you own (LFS's `$LFS`).
- The chroot stages run in a Linux user namespace (`unshare
  --map-root-user`), which gives a "fake root" without a password. Mounts
  made there vanish with the namespace.
- Only one uid exists in that namespace, so the few files that need another
  owner (e.g. `wall`, group `tty`) are listed in `config/ownership.list`.
  The boot scripts apply that list at first boot.
- The disk image is filled from the target root by FreeLFS's own
  `mkfs.btrfs --rootdir`, also inside a user namespace, so files are stored
  as owned by root.

Override any path in `build/config.sh` through the environment, e.g.
`FLFS_JOBS=4`.

## Sources

- `sources.list` is the hand-edited list of upstream URLs.
- `sources.lock` pins each one to a SHA256 and is what the build trusts.
- After editing the list, regenerate the lock:

  ```sh
  ./build/update-lock.sh -m <upstream md5sums> -s <upstream sha256sums>
  ```

  The `-m`/`-s` reference lists are optional. They cross-check downloads against
  upstream checksums, such as the LFS book's `md5sums` and kernel.org's
  `sha256sums.asc`.

## License

FreeLFS's own code is MIT licensed (see [LICENSE](LICENSE)). Upstream packages
keep their own licenses.
