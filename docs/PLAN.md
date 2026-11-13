# FreeLFS — Master Plan

FreeLFS is an independent Linux distribution built from source with no parent
distro. It is a desktop daily driver with its own package manager, bootloader,
shell, Wayland compositor and widget toolkit, and a clean traditional desktop
in the spirit of Linux Mint's Cinnamon.

## 1. Decisions

| Area | Decision |
|---|---|
| Goal | Desktop daily driver |
| Architectures | x86_64 only for now. Nothing has ever been built for aarch64, so the recipes say `arch=(x86_64)`: aarch64 returns once there is hardware to test on |
| libc | glibc |
| Toolchain | GCC + binutils |
| Core userland | GNU tools (coreutils, grep, sed, findutils, ...) |
| Init | sysvinit + our own rc boot scripts |
| Seat/session | seatd |
| Release model | **Rolling.** There are no numbered releases to jump between: the repository follows fixed upstream releases (the newest stable release of each series that fixes the known problems; FreeLFS backports nothing). The safety net is the btrfs snapshot flpkg takes before every transaction plus flboot's "Start an earlier state"; the fallback kernel is the LTS one (below). See docs/SECURITY.md, "Rolling release" |
| Kernel | The **newest stable kernel from kernel.org** (7.3.x as of Nov 2026; package `linux`, the config is `config/kernel/freelfs.config` carried forward with `olddefconfig`). The newest **6.18 LTS** is the package `linux-lts` (same config, `-freelfs-lts` in its version; optional, not in the live ISO), listed by flboot as "FreeLFS (LTS kernel ...)" when installed |
| Package baseline | LFS 13.1 package versions (see "Baseline" below) |
| Bootloader | **Our own** UEFI bootloader (`flboot`), x86_64 (kept portable) |
| Root filesystem | btrfs |
| Folder layout | FHS underneath, FreeLFS names on top: the system is `/usr` (`bin`, `lib`, `share`, ...), also reachable as `/sysflfs` (a link to `/usr`); `/etc` and `/var` are real directories; people's homes are `/home/NAME` (`/home/tortina/documents`, the btrfs subvolume `@home`); disks in `/drives`. File managers show `boot`, `drives`, `home` and `sysflfs` in `/`. (Until 2026-11-13: the system in a real `/sysflfs`, homes in `/usr`; see "Folder layout, second version") |
| Networking | iwd (Wi-Fi) + dhcpcd |
| Audio | PipeWire |
| Login shell | **Our own** shell (`flsh`); bash stays for scripts and builds |
| Display | **Our own** Wayland compositor, directly on DRM/KMS + libinput + EGL/GBM |
| Desktop / toolkit | **Our own** desktop and widget toolkit, in C |
| Look | Traditional desktop like Linux Mint's Cinnamon (bottom panel, start menu, window list, tray), but cleaner: modern, flat, with a dark mode. Everything written ourselves, no Cinnamon code |
| Logo | Three stacked bars, tilted 6 degrees (`docs/logo.svg`), accent green; the start button uses it |
| Installer | Graphical, running in our desktop on the live ISO |
| License (our code) | MIT |
| Build location | Rootless: a target-root directory on the host, chroot in a user namespace, disk image assembled at the end and booted in QEMU |

### Package manager (`flpkg`, working name)
- Written in C.
- Binary repository plus public recipes, similar to Arch.
- Recipes are shell scripts with variables and functions: `name=`, `version=`, `source=`, `sha256=`, `build()`, `check()`, `package()`.
- Dependency kinds: runtime, build and check.
- Version constraints: full, with `>= = <=`, `provides`, `conflicts` and `replaces`.
- Installs are atomic transactions with rollback.
- The installed-package database is SQLite.
- Packages are `.tar.zst` archives with an embedded metadata and file manifest.
- Packages and the repo are signed with minisign/signify (Ed25519).
- Config files in `/etc`: the new version always replaces the user's file. An optional setting saves a backup first. (We are that kind of OS.)
- Package groups (`base`, `desktop`, ...).
- Reproducible builds: `SOURCE_DATE_EPOCH`, sorted tar entries, fixed owners and mtimes.
- Not selected yet: split packages and install hooks. We will still need a minimal trigger for `ldconfig` and the font and icon caches. Revisit at M3.

## 2. Core strategy

1. **Bootstrap with plain scripts, then switch to `flpkg`.** The cross toolchain and temporary tools are built by numbered shell scripts, the LFS way. Once a chroot exists, we build `flpkg` inside it. From then on, every package in the final system is built from a recipe into a signed `.tar.zst` and installed by `flpkg`. The base system therefore comes out of the package manager from day one.
2. **Boot early without our bootloader.** QEMU's `-kernel` option boots the kernel directly. Booting is not blocked on `flboot`, and `flboot` gets developed and tested on its own with OVMF.
3. **Keep our own C code portable from day one.** That code is the package manager, bootloader, shell, compositor and toolkit. No x86 assumptions, so the aarch64 port is mostly a distro-build job.
4. **Use proven libraries where reinventing them adds risk and nothing users can see.** These are Mesa, libdrm, libinput, libxkbcommon, FreeType, HarfBuzz, SQLite, zstd and libcurl. "Our own" means the parts users see and touch.

## 3. Milestones

### M0: Project foundation (done)
- Repo layout, license, docs.
- `build/config.sh`: shared settings (target triplet, paths, image).
- `build/lib.sh`: shared helpers.
- `build/00-host-check.sh`: checks the host's tools and versions.
- `sources.list` and `sources.lock`: upstream URLs, pinned by SHA256. `build/update-lock.sh` regenerates the lock and cross-checks it against upstream checksum lists.
- `build/01-prepare.sh`: creates the target root directory, `~/freelfs-work/rootfs-<arch>`.
- `build/02-fetch.sh`: downloads and verifies the sources and hard-links them into the target root.

#### Rootless build
Nothing in the build needs sudo:
- **M1:** runs as the normal user in an `env -i` clean environment, writing
  into a target root the user owns.
- **M2+ (chroot):** runs in `unshare --map-root-user --mount --pid`. That is a
  single-uid user namespace, so everything is "root" inside it. Mounting
  `/proc` and bind-mounting `/dev` both work there (tested). Two limits:
  - Files cannot be given other uids or gids. The few base-system exceptions
    (e.g. `wall` in group `tty`, dhcpcd's state directory) go into
    `config/ownership.list`. A build step that needs one fails unless it is
    listed, and `rc.sysinit` applies the list at first boot. `flpkg` takes
    this over in M3.
  - Installing the `uidmap` package would lift this, but that needs sudo once.
- **Disk image:** assembled without root (tested). The target root is laid
  out as bind mounts in a throwaway mount namespace (leaving out `sources/`
  and the bootstrap state), and FreeLFS's own `mkfs.btrfs --rootdir`
  (btrfs-progs 7.3, run through the target's loader) fills the file system,
  storing files as owned by root. btrfs-progs 7.x can also create subvolumes
  this way (`--subvol`), so the `@`/`@home`/`@snapshots` layout in M4 stays
  rootless. The ESP will be filled with `mtools`.

#### Baseline
The LFS book stopped publishing a sysvinit edition after 12.4. The 13.x books
are systemd-only. FreeLFS therefore follows:
- **LFS 13.1** for package versions and the toolchain and core build steps.
- **LFS 12.4 (sysv)** as the reference for the sysvinit-specific parts: building
  udev on its own from the systemd source, sysklogd and sysvinit. Our rc
  scripts are our own.
- The newest stable kernel (rolling release; the 6.18 LTS as the fallback package `linux-lts`) instead of the kernel LFS names.

### M1: Cross toolchain and temporary tools (x86_64) (done)
- `build/11-cross-toolchain.sh`: binutils pass 1, gcc pass 1, Linux API headers, glibc (+ the book's toolchain sanity check), libstdc++.
- `build/12-temp-tools.sh`: cross-compile the temporary tools (m4, ncurses, bash, coreutils, diffutils, file, findutils, gawk, grep, gzip, make, patch, sed, tar, xz), then binutils and gcc pass 2, then a smoke test that runs target binaries through the target's own loader.
- Shared harness in `build/lib.sh`: clean environment, per-package logs, resumable stamps.

### M2: Chroot and the first boot (done)
- `build/chroot.sh`: enter the target in a user + mount + pid namespace.
- `build/20-chroot-tools.sh` (ch. 7): directories, essential files, then gettext, bison, perl, zlib, mpdecimal, Python, texinfo, util-linux.
- `build/30-base-toolchain.sh` and `31-base-system.sh` (ch. 8): the final base system with scripts first. udev is built on its own from the systemd source, plus sysklogd and sysvinit (the LFS 12.4 sysv way); GRUB is left out.
- `build/32-base-extra.sh`: what FreeLFS needs beyond LFS (BLFS versions): btrfs-progs with lzo, and dhcpcd.
- `build/40-system-config.sh`: strip, clean up, and install `config/etc`: our inittab, `/etc/rc.conf` (an old-school `SERVICES=(...)` list), and the rc scripts (`rc.sysinit`, `rc`, `rc.shutdown`, `init.d/`).
- `build/41-kernel.sh`: the LTS kernel from defconfig plus `config/kernel/freelfs.config` (root fs drivers built in, no initramfs). M6 widens it for real hardware (GPUs, Wi-Fi, sound).
- `build/50-image.sh`: the first disk image, assembled rootless. For M2 it is one btrfs file system used as QEMU's whole disk; M4 adds GPT, the ESP and subvolumes.
- `build/51-qemu.sh` boots it with `-kernel`; `build/boot-test.py` checks the boot headless.
- Development images have no root password; the installer sets real ones (M9).
- **Done when** FreeLFS boots in QEMU to a login prompt.
- Result: boots to login in about 4 s; `build/boot-test.py` passes (root login, btrfs root read-write, syslog, first-boot ownership, DHCP lease, clean power off). Test suites: 6 of ~6,000 glibc tests and 2 of ~5,970 binutils tests fail. 4 of the glibc failures and 1 of the binutils failures are explained by the single-uid namespace or the chroot; the other 3 have plausible but unconfirmed causes (timing, memory, a container-sensitive profiler test). GMP 199/199 and MPFR 198/198 pass.

### M3: `flpkg` v1 (done)
- `src/flpkg`, about 6,600 lines of C using SQLite, zstd and OpenSSL's libcrypto, with our own tar code. Design: [flpkg.md](flpkg.md).
- `flpkg build`: recipe → reproducible `.flpkg` packages (split packages, `build_style=autotools|meson|python`, owner exceptions, strip, path checks).
- `install`/`remove`/`upgrade`: atomic transactions with a journal; an error or a crash at any point rolls back or finishes on the next run (tested by killing flpkg at each stage).
- Dependency resolution with versions, provides, conflicts, replaces and groups; signed repos (minisign-compatible Ed25519, checked against an independent verifier); declarative system users; system triggers.
- `recipes/`: 89 recipes, 90 packages: every M2 package plus `base-files`, `initscripts`, `linux`, `linux-api-headers`, `tzdata` and `flpkg`.
- `build/60-packages.sh` builds them all in the chroot, signs them into a repo and installs group `base` into an empty root; the image from that root boots and passes the boot test. `build/61-repro-check.sh` builds everything a second time and compares.
- Self-hosting: inside the booted image, flpkg rebuilds zlib and flpkg byte-identical to the chroot builds, and installs them.
- Result: all 90 packages are byte-identical across two independent builds. Getting there took: pinned `KBUILD_BUILD_*` for the kernel, a pinned configure date for perl, Python without profile-guided optimisation or its test suite, and a fix for one real bug the check found (flpkg exported `$srcdir`, so man-pages installed nothing; flpkg now also refuses empty packages).
- **Done when** the system is self-hosting, meaning FreeLFS rebuilds itself with `flpkg`.

### M4: `flboot` and a real disk layout (done)
- `src/flboot`: our UEFI boot loader in C, with no gnu-efi or EDK2: our own UEFI headers, built with the normal Linux toolchain (position-independent ELF linked at 0 with our own linker script, converted to PE by `objcopy`, self-relocating at start).
- A clean graphical menu on the firmware framebuffer (Terminus font from kbd, scaled on large screens), with a text fallback, a countdown, Up/Down and Enter.
- Lists every `vmlinuz-*` on the ESP, extra entries from `flboot/flboot.conf`, and btrfs snapshots; starts kernels through their EFI stub with the command line as load options.
- Disk layout: GPT with an EFI system partition (FAT32, mounted at `/boot`, where the kernels live) and btrfs with subvolumes `@` (default, `/`), `@home` and `@snapshots`. `build/50-image.sh` assembles it rootless with FreeLFS's own `sfdisk`, `mkfs.fat`, mtools and `mkfs.btrfs --subvol`.
- New packages: `flboot` (with `flboot-install`, rerun by a trigger), `dosfstools`, `mtools`.
- Snapshots: before each transaction flpkg snapshots `/` into `@snapshots` (keeps the last 5) and lists them for flboot. Tested: a snapshot taken before removing a package boots, with the package still installed.
- `build/51-qemu.sh` boots through OVMF; `build/vm-run.py` runs commands in the booted image (and can press keys in the boot menu).
- **Done when** the image boots in QEMU with OVMF through `flboot`, with no `-kernel`.

### M5: `flsh`, our own shell — done
- `src/flsh`, about 10,000 lines of C, no libraries beyond libc: a POSIX shell (parser, word expansion, here-docs, redirections, functions, traps, `set -eu`, `getopts`, `read`, job control with `fg`/`bg`/Ctrl-Z) plus the bash extensions scripts use most (`${v/pat/rep}`, `${v:off:len}`, `${v^^}`, `$'...'`, `local`, `source`, `set -o pipefail`).
- Its own line editor: syntax colours as you type (unknown commands red), grey suggestions from history (Right takes them), Tab completion with a menu (commands, files, `$vars`, per-command rules in `/usr/share/flsh/completions`, flpkg's among them), prefix history search on Up/Down, Ctrl-R, bracketed paste, "Did you mean grep?".
- The prompt: user, short folder, git branch, a red mark with the status after failures; slow commands report their time. `PS1` or a `flsh_prompt` function replace it.
- Tests: 41 scripts compared against dash and bash (`tests/run.sh`), 30 checks of the editor through a pseudo terminal (`tests/interactive.py`); clean under AddressSanitizer and UBSan. A tight loop runs faster than bash, about half dash's speed.
- flsh is the shell of every new user (`useradd` default, `/etc/shells`); root keeps bash, so a broken flsh can never lock out the admin. bash stays for scripts.
- The console uses the FreeLFS colours (flboot's palette) from the first boot message on, through `vt.default_red/grn/blu` in flboot's kernel command line.
- The boot test now creates a user, logs in over the console and checks flsh's hint, Tab completion and loops on the real system; `build/vm-run.py` can type on the VGA screen and take screenshots ([docs/flsh-screenshot.png](flsh-screenshot.png)).
- **Done when** a new user logs in to flsh and the checks above pass in the image.

### Between M5 and M6: the FreeLFS folder layout — done
- base-files makes `/sysflfs/{bin,sbin,lib,libexec,include,share,src,local,opt,srv,root,settings,data}`, `/usr` for homes (with links `bin`, `lib`, `share`, ... pointing into `/sysflfs`), `/drives`, and links `/etc`, `/var`, `/bin`, `/sbin`, `/lib`, `/home`, `/mnt`, `/media`, `/opt`, `/srv`, `/root`. `/lib64` stays a real directory (the dynamic linker path is fixed by the x86-64 ABI). `.hidden` files list what file managers leave out.
- Packages are still built with `--prefix=/usr` and install through the links; nothing was rebuilt except base-files, shadow and flpkg.
- flpkg: when a package brings a link where a directory is, or where other packages of the same transaction put files, it moves the contents to the link's target and places the link first. `verify` accepts links standing for directories. New transaction tests cover both.
- The `@home` subvolume is mounted at `/usr` and carries the same links; new users get `/usr/NAME` with `desktop documents downloads music pictures videos` and a matching `user-dirs.dirs`; `PATH` is `/sysflfs/bin`.
- To do (installer, M9): refuse user names that collide with the links in `/usr` (`bin`, `lib`, `share`, ...).
- Replaced on 2026-11-13 by the second version below.

### Folder layout, second version (2026-11-13, base-files release 4) — done
- The decision: "rename usr to home and keep sysflfs, but under the hood it means both usr and sysflfs". So `/usr` is the real system directory (the normal FHS way), `/sysflfs` is a relative link to it (every old `/sysflfs/bin/...` path keeps working), `/bin`, `/lib`, `/sbin` are links into `/usr` (merged /usr), `/etc`, `/var`, `/root`, `/opt`, `/srv` are real directories, homes are `/home/NAME` on the `@home` subvolume (mounted at `/home`, holding only homes now; it used to carry the `/usr` links). `/drives` with `/mnt` and `/media` as links stays.
- Packages always installed to `usr/...` paths (`--prefix=/usr`, flpkg followed the links), so flipping the link direction in base-files was the whole package-side change: no package had to be rebuilt for its prefix. Rebuilt for the layout: base-files 4, shadow 2 (`/etc/default/useradd` HOME=/home), initscripts 8 (`/usr/sbin` paths), flinstall 3 (fstab `/home` line, `/home/NAME`, reserved names from `/home`, the live `/home` left out of `@`).
- Gone: `/sysflfs/settings` and `/sysflfs/data` (they are `/etc` and `/var` again), the `.hidden` link farm in `/usr`, the installer's refusal of names like `bin` or `share`.
- **Older installs (base-files 1 to 3) cannot move by `flpkg upgrade`.** flpkg would have to turn the real `/sysflfs` into a link while `/usr` is a separate mount (`@home`) full of links pointing back into it; it stops with "cannot move /sysflfs/... into the new layout" before changing anything (not tested on an old install). Pre-alpha answer: reinstall from a new ISO, after copying the homes (`/usr/NAME`) somewhere safe. By hand it would be: boot a snapshot or the live ISO, in `@` move `sysflfs/*` to `usr/` (replacing the links), `settings` to `etc`, `data` to `var`, `root` to `/root`, replace `sysflfs` with a link `usr`; in `@home` delete the links (`bin include lib libexec local sbin share src`, `.hidden`); in fstab mount `@home` at `/home`; in `/etc/passwd` change the homes to `/home/NAME`.

### M6: Graphics and audio foundation — done
- Kernel: amdgpu (the development laptop's Mendocino), i915, bochs and virtio-gpu for QEMU, I2C-HID touchpads, HD and USB audio, Wi-Fi (rtw89 for the RTL8852BE, iwlwifi) and the crypto iwd needs. The console uses the FreeLFS colours from flboot's command line.
- 38 new packages in the `desktop` group (BLFS 13.1 versions): libdrm, wayland, wayland-protocols, libxkbcommon, xkeyboard-config, libevdev, mtdev, libinput, seatd, libpng, freetype, harfbuzz, fontconfig, DejaVu fonts, cmake, LLVM 22 (core, AMDGPU + host), SPIR-V, glslang, Vulkan headers and loader, Mesa 26.1 (radeonsi, radv, llvmpipe, lavapipe, softpipe, virgl, zink; Wayland only, no X11), D-Bus, alsa-lib, lua, glib, PipeWire, WirePlumber, iwd, linux-firmware (GPU, Wi-Fi, Bluetooth, Ethernet families), wireless-regdb. The image now installs `base desktop`: 128 packages.
- Boot services: dbus, seatd (group `seat`), iwd. New users join seat, audio, video and render.
- `fl-drawtest` (src/drawtest): EGL + GLES2 on GBM/KMS, the display opened through libseat. In the image, as a normal user: llvmpipe, 1280x800, 234 fps. It draws a first sketch of the desktop ([docs/m6-drawtest.png](m6-drawtest.png)).
- Build system: recipes build in dependency order (tsort); desktop packages are installed into the build root as they are built (with `--nodeps`, since the build root's base is not in flpkg's database); `build_style=cmake`; `build/gentle.sh` runs long builds at idle priority with a memory cap, and jobs default to one per GB of RAM beyond 4 GB (the laptop's integrated GPU shares RAM).
- **Done when** a test client draws with EGL on KMS from a user session through seatd: passes in the boot test.
- Not yet tried: Wi-Fi and radeonsi on the real laptop (the USB image of M7 is the way to try it), Intel's iris/anv (need libclc and SPIRV-LLVM-Translator).
- Later: build each package in a clean root with only its dependencies, instead of the shared build root.

### M7: Our compositor, toolkit and desktop — in progress
- Works in QEMU with 1 GB of RAM (see docs/M7-DESIGN.md, docs/m7/): `flcomp` (own compositor, CPU rendering on KMS), `libflui`, `flpanel` with the start menu and a live tray, `flterm`, `flsetup` (keyboard at first boot), `flsettings`, and since the second slice `flfiles` (file manager), `fledit` (text editor) and `flgreet` (login screen). Boot to the login screen, log in, desktop with PipeWire running: about 100 MB used in total with a file manager and an editor open (QEMU, `build/m7-desktop.tty`). Each app stays under 20 MB RSS (flfiles 12.7, flpanel 12.2, flgreet-ui below 20, fledit below 20 with a small file; a 10 MB file takes about 25 MB).
- Login: inittab runs `flgreet` on tty1; it runs a private `flcomp` with `flgreet-ui` (accounts with uid 1000-59999, password checked with crypt_r, empty hash logs in, locked refuses, 1 s delay on failure), then `login -f NAME`. If flcomp cannot start it falls back to agetty. Ctrl-Alt-F2..F6 still give console logins.
- Boot screen: `flsplash` (src/flsplash) draws the logo animation on KMS from rc.sysinit, kernel `quiet`; Esc or a failed rc step shows the text; flgreet waits for the animation to end and takes the screen over. See docs/M7-DESIGN.md, screendumps docs/m7/splash-*.png (build/splash-shots.py).
- Session: flcomp starts `/usr/lib/flcomp/session` after the panel: session D-Bus, PipeWire, WirePlumber, pipewire-pulse, with `XDG_RUNTIME_DIR=/run/user/UID` made by flgreet.
- Tray: battery (hidden when there is none), network (Wi-Fi or wired, slashed without link), volume through wpctl (scroll changes it, click mutes); one 5 s timer, wpctl is never waited for.
- Tests: `src/flcomp/tests/apps.py` drives flfiles, fledit, flgreet-ui, flpackages and the start menu headless (screenshots in docs/m7/); `build/boot-test.py` now checks that the login screen runs on tty1; `build/m7-desktop.tty` logs in through the login screen in QEMU. A bug found this way and fixed in libflui: a key whose release arrives after the focus moved kept repeating into the next window (it pressed Enter in a freshly opened start menu).
- USB boot: the kernel has xHCI (PCI), EHCI, USB storage, UAS and USB HID built in, with SCSI disk support (FreeLFS has no initramfs, so the whole path to the root must be built in); flboot's command line has `rootwait` (build/50-image.sh and `flboot-install`), because a stick shows up late. `build/52-usb.sh` runs 50-image.sh with an 8 GB default size and writes `~/freelfs-work/freelfs-usb.img`: a user `tester` with an empty password (home `/usr/tester`, groups seat, audio, video, render; its ownership is set at the first boot by `flpkg fix-owners`, like every other file the rootless build cannot chown), and no `/etc/keyboard.conf`, so the first boot asks. It never writes to a device; it prints `dd` with a placeholder device. Checked in QEMU with 1 GB: the image booted as a USB mass storage disk behind an emulated xHCI controller (kernel found it, waited for it, mounted the root, flsetup asked for the keyboard), and `build/boot-test.py` (with `FLFS_BOOT_USB=1`) passes on a copy. Not tried on the laptop: the real xHCI and the stick itself. Secure Boot must be off (flboot is not signed). The root account has no password in this image either (development image).
- Packages: `flpackages` (src/flpackages, start menu: System) on libflui, looking like flfiles: Installed and Available tabs, search, list, details (version, licences, depends, sizes), Install, Update and Remove (asks first), a log strip with flpkg's output and a progress bar while a change runs, errors in red in the window and the status bar. Reads run `flpkg list`, `flpkg search` and `flpkg info` as the user, as child processes (the window never waits). Changes go through `flpkg-seat`, setuid root, mode 4750 root:seat (recipe `owner=`): it accepts only `install NAME...` or `remove NAME...` (names `[a-z0-9+._-]+`, first character a letter or digit, at most 64 characters, not ending in `.flpkg`; at most 32), refuses everything else, clears the environment (PATH only), resets signals, umask and file descriptors, goes to `/`, and runs `/usr/bin/flpkg install|remove -y -- NAMES`; removal never uses `--cascade`. Without a `repo` line in /etc/flpkg.conf the Available tab says so plainly and Install is off (a fresh image has no repo, so today only Installed and Remove work against real data). The index is not refreshed by the app: `flpkg sync` stays a root command.
- Tests: `src/flpackages/tests/seat.sh` (the helper with a stand-in flpkg: good names, 20 refused inputs, cleared environment; no root needed) and `apps.py flpackages` (headless flcomp with a stand-in flpkg and flpkg-seat: list, filter, search, details, install, confirmed remove, a failing install, no-repo state; screenshots docs/m7/flpackages-*.png); `build/boot-test.py` checks flpkg-seat's mode and that, as a seat member, `flpkg-seat remove nosuchpkg` reaches flpkg as root and bad names are refused. A libflui bug found by the app: clicking a text field never gave it the focus when the previously focused widget came later in the draw order (`run_pass` dropped the focus); fixed in `fl_widget`.
- Third slice, checked in QEMU at 2 GB and 1 GB (`build/m9-features.sh`, docs/ROADMAP-NEXT.md has the list): notifications (card, centre), lock screen (Super+L, wrong and right password), Settings > Date & time with the world map (a click on Tokyo changes the zone and the panel clock), `fltime-set` and the setuid helpers' modes, `fl-sntp` against a real NTP pool, FreeBWS ad blocker first start at 1 GB (no OOM, 434 MB MemAvailable at the lowest) and its error page. Found by the tests: only test-side problems (the QEMU monitor's `mouse_move` is relative: `build/vm-run.py` has a `click X Y` step through QMP's absolute axes now) and two small FreeBWS fixes (test-page link, a status text). `build/boot-test.py` runs on the real image, not a copy: its Wi-Fi step now removes the line it adds, waits until iwd lists both fake radios and restarts iwd once if it does not (iwd sometimes left the first radio alone: the step failed about half the runs before), and a step may take up to 90 s. The live ISO (790 MB, 1 GB RAM) passes `build/iso-test.py` with the new packages.
- Not yet done: GPU compositing (since flcomp 3 a real GPU's dmabuf buffers are read back through GLES, but the screen is still composed on the CPU), the whole desktop on the real laptop again after its first boot found bugs (Wi-Fi, battery and the volume keys are untested on real hardware: QEMU has no battery, no Wi-Fi and no sound card), copying very large files in flfiles runs in the window's own loop (the window waits), the login screen has no per-user theme and no session choice, fl_clipboard_get gives other apps' clipboard only up to 1 MB.
- Compositor: DRM/KMS modesetting, GBM/EGL rendering, libinput, xdg-shell, layer-shell, multiple outputs, clipboard and DnD.
- Toolkit: C widget library with our own rendering, plus FreeType/HarfBuzz for text, with a clean flat theme and a dark mode.
- Desktop, laid out like Cinnamon: a bottom panel with start menu, window list, system tray and clock; window decorations; desktop icons; terminal, file manager, a central settings app (display, Wi-Fi via iwd, audio via PipeWire, appearance), text editor.
- Low-end hardware is a goal: the desktop must run well on about 1 GB of RAM and without a usable GPU (software rendering). Memory is budgeted per component and tested in QEMU with 1 GB.
- Keyboard layouts: the first boot asks for the layout (QWERTZ, QWERTY, others), one setting drives both the console and the compositor, and it can be changed later in settings.
- **Done when** FreeLFS is usable day to day for basic work in its own desktop, in 1 GB of RAM.

### M8: App compatibility
- XWayland support in the compositor: done (flcomp 10/11, Xwayland 24.1.15; an X11 client runs in QEMU; no glamor/DRI3 yet; docs/E2-GAMING.md).
- GTK and Qt runtimes so third-party apps run (themed as close to our look as possible).
- Default browser: our own, on WPE WebKit (docs/M8-BROWSER.md). Status of the first three steps of its order:
  - **Packages** (all `groups=(desktop)`, BLFS 13.1 versions where BLFS has them): `libgpg-error` 1.61, `libgcrypt` 1.12.2, `libtasn1` 4.21.0, `icu` 78.3, `libxml2` 2.15.3 (no git here: `prepare` cuts the `git describe` call out of its meson.build; no history/readline), `libxslt` 1.1.45, `nasm` 3.02, `libjpeg-turbo` 3.2.0, `libwebp` 1.6.0, `libepoxy` 1.5.10 (EGL only), `libpsl` 0.23.3 (ICU for IDNA), `nghttp2` 1.70.0 (library only), `brotli` 1.2.0, `lcms2` 2.19.1, `woff2` 1.0.2 (`-include cstdint`: GCC 16), `unifdef` 2.12 (`CC="cc -std=gnu17"`: C23 made `constexpr` a keyword), `libyaml` 0.2.5, `ruby` 4.0.6 (WebKit's JavaScriptCore needs it at build time), `libsoup` 3.6.6 (+ BLFS upstream_fixes patch, no introspection or docs), `glib-networking` 2.80.1 (OpenSSL backend, no GnuTLS/libproxy), `orc` 0.4.44, `gstreamer`, `gst-plugins-base` (playback, audio and video basics, GL through EGL/GLES2 on Wayland/GBM, no desktop GL: Mesa builds no libGL), `gst-plugins-good` (matroska, isomp4, audio parsers, avi, flv, rtp, png/jpeg, ...), `gst-plugins-bad` (only what WebKit links: the libraries gstmpegts and gstcodecparsers, plus the video parsers and the MPEG-TS demuxer), `gst-libav` 1.28.6 and `ffmpeg` 9.0.1 (LGPL build: libraries only, no programs, no GPL or non-free parts, `--disable-autodetect`), `libwpe` 1.16.3 (default backend wpebackend-fdo), `wpebackend-fdo` 1.16.1, `wpewebkit` 2.52.5 (the webkitgtk version of BLFS 13.1). `harfbuzz` went to release 2 with ICU on (WebKit needs libharfbuzz-icu). WPE WebKit is configured with the legacy WPE API (libwpe + wpebackend-fdo), Skia, GStreamer video and Web Audio, MSE; off: GTK, X11, introspection, docs, journald, speech, WebDriver, gamepad, WebRTC, media stream and recorder, GPU process (one process less), JPEG XL, AVIF, hyphenation, libbacktrace, sysprof, ATK, and the bubblewrap sandbox (no bubblewrap, libseccomp or xdg-dbus-proxy yet; a gap to close before FreeBWS ships). CMake 4.4 needs one sed in WebKit's `WebKitMacros.cmake` (unquoted empty variable in an `if`). Everything up to `gst-libav` and `wpebackend-fdo` built; `wpewebkit` compiles for many hours with two jobs (about 9300 ninja steps; the recipe keeps ninja's build directory in `/.bootstrap/pkg/wpewebkit-resume` when ninja stops, so a restart of `./build/gentle.sh ./build/60-packages.sh wpewebkit` goes on where it was). **flpkg's `jobs` comes from `flpkg.conf`, written once by the stamped step `p.1-setup`; it said 6 and WebKit started 6 compilers (swap full). It is 2 now (`FLFS_JOBS=2` while the stamp was removed); remove the stamp again to take the default.**
  - **flcomp** (release 3): `zwp_linux_dmabuf_v1` version 3 (`dmabuf.c`), single-plane ARGB8888, XRGB8888, ABGR8888, XBGR8888. A dmabuf is read into the surface's CPU copy at commit and released, like an shm buffer; compositing stays on the CPU, shm clients are untouched (`make check` passes, the new tests send ARGB, XRGB and ABGR buffers through a memfd that stands in for a dmabuf; flcomp's own RSS and memory use are unchanged because nothing new runs unless a client binds the dmabuf global). Linear buffers are mmap'ed and copied (this is the only path with llvmpipe, a KMS-only device or no GPU: only the LINEAR modifier is announced). On a real GPU (`gl.c`), started lazily by the first client that binds the global: EGL on the DRM device through GBM, every modifier the GPU can sample is announced, and a tiled buffer is imported as an EGLImage, drawn by GLES2 into a linear texture and read back into the surface copy. `FLCOMP_NO_GL=1` switches it off. **Not tested on any GPU** (no GPU access here; it compiles and the protocol path is tested), and not done: GPU compositing of the whole screen (the scanout stays a CPU-composited dumb buffer), `wl_drm`, dmabuf feedback (version 4). The test script (`tests/run.sh`) now sets `FLCOMP_WALLPAPER=` (an installed wallpaper made four screenshot checks fail).
  - **FreeBWS** (`src/freebws`, recipe `freebws`, libflui got `fl_app_prepare` and `fl_app_dispatch` so that its loop runs inside GLib's): one window, toolbar (back, forward, reload, address bar that also searches, load progress), the page as a desynchronised `wl_subsurface`; frames arrive as shm (copied) or dmabuf (passed on to flcomp) depending on what flcomp announces. Written; only the part outside the WebKit API was compile-checked (against the real libwpe, wpebackend-fdo and libflui headers, WebKit stubbed); built and run in QEMU at 2 GB and 1 GB (see docs/M8-BROWSER.md): start page, https (TLS with ca-certificates), address bar like other browsers, DSSL pages; shared-mime-info added to the image (without it file:// pages were shown as text/plain). `build/m8-finish.sh` (running unattended, log `~/freelfs-work/logs/m8-finish.log`) waits for WebKit, builds flui, flcomp and freebws, makes the image, runs `build/m8-freebws.sh` (2 GB and 1 GB, screenshots to `docs/m7/freebws-*.png`, RSS of every WebKit process in `logs/m8-freebws-*.log`) and `boot-test.py` at 2 GB and 1 GB (for the hwsim Wi-Fi step that fails at 1 GB with flsplash), then the ISO.
  - **FreeBWS, second pass** (docs/M8-BROWSER.md has the details and numbers): tabs (tab row `src/freebws/tabstrip.c`, Ctrl-T, Ctrl-W, Ctrl-Tab, `target=_blank`, `window.open`, Ctrl-click and middle click in the background, discarding of idle background tabs); memory kept flat across origins (cache model DOCUMENT_VIEWER, `ProcessSwapOnCrossSiteNavigation` off, Skia CPU rendering without a GPU, tabs in one web process under 3 GB); native DSSL: `libdssl` is in the desktop group and FreeBWS lowers DSSL to CSS itself, the JS engine only for sheets with texture, aura, `@signal` and the other hard features. `build/m8-freebws.sh` now covers all of it (and runs on a throwaway copy of the image).
- Firefox as the fallback in the repo, which pulls in the Rust toolchain, nodejs and others. This is the largest build item.
- **Decision: Flatpak with Flathub is the app catalog** (instead of packaging every app ourselves). Per-user installs only (`flatpak --user`, apps in `~/.local/share/flatpak`), so Flatpak is built with `system_helper=disabled` and **no polkit** (which would pull in mozjs/duktape), and with no systemd (D-Bus activates `flatpak-portal`, the session helper and xdg-desktop-portal from the session bus). Recipes (all `desktop`): flatpak 1.18.4, ostree 2026.6, bubblewrap 0.13.1, xdg-dbus-proxy, xdg-desktop-portal 1.22.1 (frontend only), appstream 1.2.2 (+ libxmlb, libfyaml), gdk-pixbuf, json-glib, libarchive, libseccomp, fuse3, curl 8.23 (+ ca-certificates from Mozilla's bundle via certifi: the system had no CA store), gnupg 2.4.9 + gpgme + libassuan + libksba + npth (gpgme needs `gpg` to verify Flathub's signed commits), pyparsing (build only). The session adds the Flathub remote once at the first login that has a network (src/flcomp/session); `/etc/profile.d/10-flatpak.sh` puts the export dirs in XDG_DATA_DIRS and the start menu scans them; the Packages app has a Flathub tab (docs/m7/packages-flathub-*.png). Kernel: `CONFIG_FUSE_FS=y` added (linux release 5); user namespaces and seccomp were already on. **Known gaps:** (1) xdg-desktop-portal has no backend (the GTK/GNOME/KDE ones need toolkits we lack), so file chooser, screenshot and similar portal calls fail until a libflui backend exists; (2) homes are in `/usr/NAME` and Flatpak refuses to export anything inside `/usr` into a sandbox, so `--filesystem=home` and the per-app data dir `~/.var/app/ID` were not bound (apps ran; their data was not persistent): flatpak release 2 carries `recipes/flatpak/flatpak-1.18.4-home-under-usr.patch`, which lets the user's own `$HOME` and below through (and nothing else under `/usr`; the mount point is an empty directory made in the user-owned runtime checkout just before it becomes the sandbox's `/usr`, `$HOME` is a tmpfs in the sandbox unless exported). Written from the source, only syntax-checked (`flatpak-exports.c` with `-Wall`), never built or run: first test is `flatpak --user run --filesystem=home org.some.App` and `ls ~/.var/app/ID` after a run; (3) the whole chain is untested: it has not been built or run yet (`build/flatpak-finish.sh`).

### M9: Release

#### The live ISO (`build/53-iso.sh`)
- **Layout.** ISO 9660 (Rock Ridge, Joliet) with `live/root.sfs` (squashfs, zstd level 15, 256 KB blocks, about 640 MB: the installed root of `base desktop` plus the changes below, without `/boot` and the flpkg cache), `EFI/BOOT/BOOTX64.EFI`, and a FAT16 ESP image (flboot as `EFI/BOOT/BOOTX64.EFI`, `vmlinuz-6.18.59-freelfs`, `initrd.img`, `flboot/flboot.conf`: a normal entry and one with `flive.debug`). The ESP image is the El Torito UEFI boot image and, appended with `-appended_part_as_gpt`, GPT partition 2, so the one file boots as a CD, written to a stick with `dd` (protective MBR + GPT), and as a file on a Ventoy stick. It must stay under 32 MB (the script checks): OVMF did not boot a 40 MB FAT32 boot image (boot catalog size 0) but booted a 20 MB FAT16 one.
- **Tools.** New recipes in the new group `live` (not in the images; built and installed into the build root like the desktop group): `squashfs-tools` 4.7.5, `libburn`, `libisofs`, `libisoburn` (xorriso) 1.5.8, `flinitrd`. The script makes the squashfs and the ISO in the chroot; the root is overlaid there with overlayfs in the user namespace, so the real root is never touched. Build with `./build/gentle.sh ./build/60-packages.sh linux squashfs-tools libburn libisofs libisoburn flinitrd flboot initscripts`, then `./build/gentle.sh ./build/53-iso.sh`. The whole ISO step takes about 1.5 minutes; `--cmdline "console=ttyS0,115200"` makes a test build that reads on the serial port.
- **flboot and the initramfs.** No flboot change was needed for the initrd: the EFI stub loads `initrd=\initrd.img` from the volume flboot read the kernel from (flboot already sets the kernel's device handle). One flboot change: if the firmware shows no file system on the device flboot was loaded from, it uses the first volume with `flboot/flboot.conf`. The command line has no `root=` and ends in `console=tty0` (so the prompt below works on a laptop screen).
- **`src/flinitrd`**, the initramfs's `/init`: one static C program (no busybox), initramfs is gzip, 340 KB. It mounts proc, sys, devtmpfs and waits up to 20 s (`flive.timeout=`) for disks, scanning `/sys/block` (disks, partitions, dm-N; not loop, ram, zram) every 250 ms. Case 1: a device that is ISO 9660 with `/live/root.sfs` (CD, dd'd stick, a dm device). Case 2: a partition (or a disk without partitions) mounted as exfat, vfat, ext4 (`noload`) or iso9660, with a file `freelfs-live*.iso` (case-insensitive; `flive.iso=` changes the pattern) at the top or up to 3 directories down (at most 20,000 entries per file system). Then loop (read-only) over the ISO, mount it, loop over `root.sfs`, mount the squashfs, tmpfs overlay (`/run/live/rw`), move /dev /proc /sys /run, switch_root to `/sbin/init`. **Read-only by construction:** every device is set read-only in the kernel (`BLKROSET`) before it is mounted, all mounts are `MS_RDONLY` (also nosuid, nodev, noexec), loops are read-only. If nothing is found it says what it looked for, lists the disks and offers a prompt: `r` look again, `l` list, `p` power off, `b` reboot (and retries by itself every few seconds when there is no keyboard). A bug found by the first test: `LO_FLAGS_AUTOCLEAR` cleared the loop device when flinitrd closed it, before the mount held it (EIO on the first read).
- **Kernel** (`config/kernel/freelfs.config`, `linux` release 3): built in `EXFAT_FS`, `ISO9660_FS`, `SQUASHFS` + `SQUASHFS_ZSTD`/`ZLIB`/`XZ`, `OVERLAY_FS`, `BLK_DEV_LOOP`, `BLK_DEV_DM`, `BLK_DEV_INITRD` + `RD_GZIP`/`RD_ZSTD` (the recipe's check confirmed every setting was applied); VFAT, ext4, SR (CD-ROM), USB storage and SATA/NVMe were already in.
- **The live system.** User `tester`, empty password, home `/home/tester` (until 2026-11-13 `/usr/tester`), made in the squashfs (ownership applied at boot by `flpkg fix-owners`, as for the USB image); no `/etc/keyboard.conf`, so the first boot asks; login screen on tty1. `/etc/fstab` is replaced by a live one (proc, sys, devpts, /run, /dev, shm, /tmp, cgroup2): **no `@home`, `@snapshots` or ESP lines**, because a disk of the host with the same label (an installed FreeLFS!) would be mounted and written; no fsck, no swap. flpkg's `snapshots = 0`. rc.sysinit and rc.shutdown remount `/` read-write or read-only through the new `remount_root`, which does nothing on an overlay (the remount failed with "No changes allowed in reconfigure" and showed a `[FAIL]` in the first test; fixed, initscripts release 2). Nothing else of the host is touched: no automounter, nothing mounts the internal disks.
- **Memory.** Tested at 1 GB in QEMU (KVM): boot to the login screen in about 18 s, 962 MB seen by the kernel, after login about 90 MB used and 700 MB free (flcomp 10 MB, flpanel 10 MB RSS). Everything the live system writes (logs, `/home/tester`, installed packages) lives in a tmpfs on top, so a long session or a big package install eats RAM; 1 GB is for looking around and basic work, 2 GB or more is comfortable. The squashfs is read through two loop devices (ISO file, then root.sfs), so the page cache holds a little twice.
- **Wi-Fi page in Settings (after the first real-laptop boot, where the RTL8852BE worked but Settings had no Wi-Fi):** `flsettings --page wifi` (the panel's network icon), iwd over D-Bus, password agent, seat/netdev D-Bus policy, iwd's DHCP + a resolvconf stand-in, `denyinterfaces wl*` for dhcpcd, group netdev; kernel `CONFIG_MAC80211_HWSIM=m` for tests. Details in docs/M7-DESIGN.md ("Wi-Fi"). Tested with a fake iwd on a private bus and with iwd on mac80211_hwsim in QEMU; not yet with the real chip.
- **Tests** (`build/iso-test.py`, QEMU + OVMF with fresh firmware settings, 1 GB; `build/iso-test-disk.py` makes test disks; `build/51-qemu.sh --iso FILE` boots a CD): as a CD (`/dev/sr0`, case 1); as a virtio disk, i.e. a dd'd stick (`/dev/vda`, case 1); the ISO as a file in an exFAT partition (a directory down) and in a FAT32 partition of a second disk, booted from a small ESP (case 2: `/isos/freelfs-live.iso on /dev/vda2 (exfat)`; the exFAT image comes from a small writer in iso-test-disk.py, a single contiguous file as Ventoy lays them out); **real Ventoy 1.1.18** (release tarball from GitHub, its own EFI partition image and MBR, in QEMU/OVMF): its UEFI menu lists the ISO, "Boot in normal mode" boots flboot, kernel, flinitrd found the ISO in Ventoy's exFAT partition, to the keyboard question. Each run checks: no `[FAIL]` in rc.sysinit, `/` is an overlay on a squashfs, user tester, no `/boot` content, no btrfs in fstab, and that the only block device mounted is the ISO's own, read-only, under `/run/live`. The data test disks' modification times did not change over the runs (no writes). Screenshots: `docs/m7/live-*.png`.
- **Not tested:** the real laptop (xHCI stick, real timing, Wi-Fi, GPU); Ventoy on a real stick (only Ventoy's own images in QEMU, with my exFAT file system instead of the one Ventoy2Disk makes, and an MBR stick; Ventoy's GPT mode and its other boot modes, grub2 and memdisk, are untested: use "Boot in normal mode"); Secure Boot (must be off); a slow or flaky stick that appears later than 20 s (raise `flive.timeout=` in the flboot menu); more than one ISO.

- **M10 integration round (2026-11-11):** the features of eleven parallel agents (workspaces, Snappel, General Manager, accent/wallpaper/sounds, fledit and Archives, Images/Documents/Music/Videos, Software Store, portals, update notifier, snapshots, gaming kernel, installer) are built into one image and one live ISO and were exercised in QEMU at 2 GB and 1 GB: `build/m10-features.sh`, `build/m10-snapshots.sh` (the first real btrfs run: snapshot, flboot "Start an earlier state", read-only boot, rollback) and `build/m10-install.py` (a real whole-disk install onto a virtual disk file from the live ISO, then booting the result). One real bug came out of it (the installer helper's real uid versus a shell script) and is fixed. Details, what was not tested and the final ISO: docs/ROADMAP-NEXT.md ("M10 integration round"), docs/INTEGRATION-TODO.md.
- **Live ISO (`build/53-iso.sh`):** `~/freelfs-work/freelfs-live.iso`, 843 MB, rootless, with the installer on it (an icon on the live desktop and a start menu entry). Details below in "The live ISO". On the live system the Store does not add Flathub (RAM: see docs/RAM-PROFILE.md). Still to do for the release: Secure Boot (flboot is not signed), the laptop, a real-disk install.
- Graphical installer (`src/flinstall`): partitioning, btrfs subvolumes, `flpkg` base and desktop install, `flboot` setup, user creation. Built; a whole-disk install onto a virtual disk in QEMU booted. Not tried on a real disk.
- Public binary repo and signing keys.
- CI that rebuilds changed recipes and checks reproducibility.

#### Live persistence and USB drives (round 16)

- **Persistent home on the stick.** On a Ventoy stick (the ISO is a file on the data partition), put a file
  `freelfs-persist.img` (an ext4 or btrfs image) at the top of that partition, or next to the ISO. `flinitrd` sees it,
  makes the stick writable, loop-mounts the image read-write and bind-mounts it over the live `/home`; `rc.sysinit`
  copies the live user's skeleton home into an empty image and the ownership step gives it to her. Without the file,
  everything stays in RAM as before. `flive.nopersist` on the kernel command line turns it off. Only `/home` is kept
  (Flatpak apps the person installs live in `~/.local/share/flatpak`, so they are kept too); the system itself is still
  the read-only ISO. Make the image with `flpersist create SIZE [--btrfs]` (setuid for the group seat; for example
  `flpersist create 4G`: it makes the stick writable, allocates the file and runs mkfs); `flpersist status` says
  none, ready or active. Settings > About runs `flpersist create 4G` in a live session. A stick written with dd has
  no file system to hold the image: not supported.
- **USB drives.** `flmount` (setuid, group seat) is the one helper: `list`, `mount`, `unmount`, `eject` (see its header
  for the line format). A plugged-in removable partition is mounted by a udev rule (`90-flmount.rules`) at
  `/drives/LABEL`, nosuid,nodev,noexec, with the uid and gid of the session's user on vfat/exFAT, and the session sees
  the notification "USB stick 'LABEL' connected" (flnotify, with an Open button that starts Files there). The live boot
  stick is made writable and bound to `/drives/LABEL`; `flinitrd` clears the kernel read-only flag it set while probing
  on every device before the live system starts. NTFS is not supported (the kernel has no NTFS driver built in).
- **Root actions ask for a password.** `flauth` (setuid, group seat, PAM-free: crypt() against /etc/shadow, rate limited,
  a flui dialog run as the person) gates `flpkg-seat install|remove` and `flsnap rollback|delete|prune`: without a
  fresh stamp (`/run/flauth/ok-UID`, 5 minutes) they start themselves again through flauth. The live session's tester
  has no password and is not asked.

### M10: aarch64 port (not started)
aarch64 returns once there is hardware to test on (the recipes declare `arch=(x86_64)` until then).

- Cross toolchain for `aarch64-freelfs-linux-gnu`, recipes adapted where needed.
- `flboot` for AArch64 UEFI.
- Test on QEMU `virt` with AAVMF, then real hardware.

## 4. Repo layout (target)

```
freelfs/
  build/          bootstrap stage scripts (00-host-check.sh, 10-..., 20-...)
  sources.lock    pinned upstream sources (version, url, sha256)
  recipes/        <pkg>/recipe + patches, from M3 on
  config/         kernel configs, /etc skeleton, rc scripts
  src/
    flpkg/        package manager (C)
    flboot/       UEFI bootloader (C)
    flsh/         shell (C)
    compositor/   Wayland compositor (C)
    toolkit/      widget toolkit (C)
    desktop/      panel, apps, settings, installer (C)
  image/          disk image and ISO tooling
  docs/
```

## 5. Risks and mitigations

- **Build time.** gcc, LLVM, Mesa and Firefox each take hours. Mitigations: `-j$(nproc)`, ccache for our own code, and binary packages so each package is built once.
- **Host contamination.** Mitigations: a separate `$FLFS_TGT` triplet, a clean `env -i` build user, and the chroot.
- **Scope.** We are writing our own bootloader, shell, compositor and toolkit. Each is a real project. Milestones are ordered so the distro is usable before every custom piece is done; for example, `-kernel` boot comes before `flboot`, and bash comes before `flsh`.
- **Without systemd.** Some desktop software assumes logind or systemd. Mitigations: seatd, plus patches or shims per package when needed.
- **Security upkeep.** Rolling release on the newest stable kernel with an LTS fallback, pinned sources, `build/cve-check.py` against the NVD, and cheap rebuilds through `flpkg`.

## 6. Open items
- The exact look (colours, panel, menu, window decorations): decide from mockups before M7. Direction: Cinnamon-like layout, cleaner.
- Final names for `flpkg`, `flboot`, `flsh`, the compositor and the toolkit (these are working names).
- Decided in M3: split packages (Arch-style), system triggers only (no package scripts), subcommands plus pacman-style flags, the name flpkg.
- Package dependencies are written by hand; a check that compares them with the shared libraries each package links against comes with M6.
