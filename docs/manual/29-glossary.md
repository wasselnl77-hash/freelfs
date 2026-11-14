# 29. Glossary

Summary: every `fl*` name in the tree, the other programs and libraries of FreeLFS, and the terms used in the manual and the
source, each in one or two lines, with the chapter that explains it in more detail.

## Contents

1. Programs and libraries named `fl*` (and the desktop's own names)
2. Other FreeLFS programs, helpers and files
3. Build and package terms
4. Desktop, Wayland and graphics terms
5. Security and system terms
6. Project and process terms

---

## 1. Programs and libraries named `fl*` (and the desktop's own names)

Names are the program or library as it is installed. Where the start menu shows a different name, it is in brackets.

| Name | What it is | Chapter |
|---|---|---|
| `fl-drawtest` | a check of EGL and OpenGL ES on KMS through GBM, run as a normal user (M6) | 23 |
| `fl-sntp` | the SNTP client daemon of the `ntp` service (`src/fltime`) | 16, 28 |
| `fl-screencopy` | the private screenshot protocol of flcomp (`protocol/fl-screencopy.xml`) | 23 |
| `fl-shell` | the private desktop protocol between flcomp and flpanel, flsettings and others (`protocol/fl-shell.xml`, versions 1 to 9) | 23 |
| `flarchive` (Archives) | the archive tool: open and extract zip, tar and others; create zip and tar.zst | 06 |
| `flauth` | the setuid password dialog: asks for the password before a root action, rate-limited, checked against `/etc/shadow` | 15 |
| `flauth-dialog` | the window `flauth` shows (in `libexec`) | 15 |
| `flboot` | the UEFI boot loader: the graphical menu, boot entries, starts the kernel. `flboot-install` is its install helper (its details are not verified in this pass). | 11 |
| `flcomp` | the Wayland compositor: draws the screen, windows, workspaces, lock, Xwayland | 23 |
| `flcrash` | the "Something went wrong" crash screen; `--cant` shows the "You can't do that." egg; `flcrash-test` tests it | 14, 24 |
| `fldisplay` | a command line tool for the screens (`list`, `set`, `revert`, `nightlight`), through fl_shell version 9 | 23 |
| `fledit` (Text editor) | the text editor, with tabs and syntax colours | 06 |
| `flfiles` (Files) | the file manager: folders, drives, Open with, trash, the desktop icon menu | 04, 07 |
| `flgm` (General Manager, binary `general-manager`) | the task manager: processes, performance, startup apps | 04 |
| `flgreet` | the login screen's start program, in `sbin` | 14 |
| `flgreet-ui` | the login screen itself, a libflui program in `libexec`; started by `flcomp --firstboot` | 14 |
| `flinitrd` | the init program of the live ISO's initramfs (`/usr/lib/flinitrd/init`); finds the ISO on a stick | 20 |
| `flinstall` (FreeLFS install) | the installer: language, keyboard, Wi-Fi, disk, account, time zone, summary, install | 03 |
| `flinstall-helper` | the setuid helper the installer uses for the privileged steps (group `seat`) | 03, 28 |
| `fllock` | the lock screen program (started by flcomp on Super+L, the lid and restarts) | 04 |
| `fllock-check` | checks the password of the seat user for `fllock` (setuid, group `seat`) | 15 |
| `flmount` | mount, unmount and eject removable drives (setuid, group `seat`); the automount rule and the notice of a connected stick | 07 |
| `flmusic` (Music) | the music player; `flmusic --ctl play-pause` answers the media keys (MPRIS) | 06 |
| `flnotify` | sends a notification to the panel's notification server (`flnotify "Title" "Text"`) | 04 |
| `flpackages` (Packages) | the package app, kept as the provider of `flpkg-seat`; out of the desktop group | 13 |
| `flpanel` | the bottom panel and the start menu: tray, clock, battery, network, volume, workspaces, notifications | 04 |
| `flpdf` (Documents) | the document viewer (PDF) | 06 |
| `flpersist` | handles the persistent image of a stick (`freelfs-persist.img`), which becomes `/home` (`src/flmount/flpersist.c`) | 07 |
| `flpkg` | the package manager: install, remove, upgrade, build, repository, signatures, snapshots' hook | 13 |
| `flpkg-seat` | the setuid helper of the Packages and Store apps (group `seat`), using `flauth` | 13 |
| `flportal` | the backend of xdg-desktop-portal for Flatpak apps: file chooser, notifications, settings, screenshots | 27 |
| `flpower` | power actions: off, restart, logout, power saver, CPU governor (setuid, group `seat`) | 14 |
| `flreport` | collects a report of the machine for bug reports | 26 |
| `flsettings` (Settings) | the settings app: Display, Wi-Fi, sound, keyboard, date and time, workspaces, Bluetooth, printers, wallpaper | 08 |
| `flsetup` | the first boot program: keyboard choice (`flcomp --firstboot`) | 03 |
| `flsh` | the FreeLFS shell: POSIX syntax, colours as you type, suggestions from history, Tab menus | 06 |
| `flsnap` | btrfs snapshots of the system: create, list, show, prune, rollback, delete; flboot starts an earlier state from them | 13, 19 |
| `flsnap-notice` | says so when the system runs from an earlier snapshot | 14 |
| `flsocketd` | starts a program when a client connects to its socket (used for CUPS on demand) | 16 |
| `flsound` | the system sounds, synthesised in the program and played through PipeWire | 04 |
| `flsplash` | the boot screen (animated logo); `genart` renders the 3D F in the accent colours | 11 |
| `flstore` (Software Store) | the store: Flathub and FreeLFS packages, Installed and Updates tabs | 13 |
| `flterm` (Terminal) | the terminal emulator | 06 |
| `fltheme` (Themes) | colour scheme, accent, fonts, buttons, panel, animations and presets | 08 |
| `fltime` | the time package: `fl-sntp` and `fltime-set` (the setuid helper for the time zone and the clock) | 16 |
| `fltime-set` | the setuid helper Settings uses for the time zone, automatic time and a hand-set clock | 16 |
| `flupdate` | the update notifier: checks daily, notifies, never installs (`flupdate-notify` is the session's start) | 13 |
| `flview` (Images) | the image viewer (PNG, JPEG, WebP) | 06 |
| `flvideo` (Videos) | the video player, on libmpv, with resume and subtitles | 06 |
| `fl_` (prefix) | the functions of libflui (`fl_button`, `fl_win_new`, `fl_conf_get`), see chapter 22 | 22 |
| `flui` (libflui) | the FreeLFS desktop toolkit: immediate-mode widgets on Wayland, text, theme, `desktop.conf` helpers | 22 |
| `flpalette.h` | the colour tokens of both schemes, shared by libflui and flcomp | 22 |
| `flshell.c` | the source file of the `fl_shell` protocol inside flcomp | 23 |

## 2. Other FreeLFS programs, helpers and files

| Name | What it is | Chapter |
|---|---|---|
| `abpconv` (`freebws-abpconv`) | converts ABP filter lists to the browser's rules | 27 |
| `Snappel` (`snappel`) | the screenshot tool: full screen, window, region, timer, annotate, clipboard | 06 |
| `freebws` | FreeBWS, the web browser on WPE WebKit | 27 |
| `libdssl`, `dssl2css` | the native DSSL style library (C) and its command line tool | 27 |
| `hook-pre-transaction` | the `flpkg` pre-transaction hook (`10-flsnap`) that makes a snapshot before an install | 13 |
| `session` (`/usr/lib/flcomp/session`) | the session services started by flcomp: D-Bus, PipeWire, WirePlumber | 23 |
| `desktop.sh` (`/etc/profile.d/`) | starts flcomp for ordinary users on tty1 | 23 |
| `flcrash` | see section 1 | 14 |
| `gaming-config` (`flgaming`) | the game controller udev rules, Proton sysctl values and the `flgaming` helper | 17 |
| `recipe.sh` | the bash helper `flpkg` uses to read a recipe (`dump`, `run`) | 21 |
| `WPE WebKit` | the WebKit port used by FreeBWS (`wpewebkit`, `libwpe`, `wpebackend-fdo`) | 27 |

## 3. Build and package terms

| Term | Meaning | Chapter |
|---|---|---|
| Bootstrap | the first stages of the build (`00` to `41`) that make the target root | 20 |
| Chroot (rootless) | `build/chroot.sh`: a user namespace where the target root is `/`; only one user ID exists in it | 20 |
| Build root | the root in which the packages are built and installed (the `desktop` and `live` groups go into it) | 20 |
| Deferred (group) | recipes kept for later that cannot build yet (`libclc`, `spirv-llvm-translator`); built only when named | 21 |
| Ownership list | `config/ownership.list`: file owners the rootless build cannot set | 20 |
| `flpkg` transaction | an install, remove or upgrade that either finishes or rolls back, with a journal | 13 |
| Group (recipe) | a set of packages: `base`, `desktop`, `live`, `lts`, `deferred` | 21 |
| `index.zst` | the repository's compressed index of the newest version of every package | 13 |
| Live ISO | the CD image that boots the system without installing it (`freelfs-live.iso`) | 03, 20 |
| `freelfs-usb.img` | the disk image for a USB stick (8 GB by default) | 20 |
| minisign | the signature format of packages and the repository index (Ed25519) | 13 |
| `pax` | the archive format inside a `.flpkg` file (`NAME-VERSION-RELEASE-ARCH.flpkg`) | 13, 21 |
| `PARTUUID` | a fixed partition ID; the image uses fixed ones so that it is reproducible | 20 |
| Recipe | `recipes/NAME/recipe`: the bash file that says how to build a package | 21 |
| Release | the package revision number, raised when a recipe changes but the upstream version does not | 21 |
| Repository | a static, signed directory of packages and an index | 13, 20 |
| `rootfs owner` | the coordination role of the agent that owns the target root in the project's notes (`docs/AGENT-RULES.md`); not a software term | 26 |
| Sources lock | `sources.lock`: the pinned SHA256 of every upstream source | 20 |
| Stamp | an empty file that marks a build step as done, so that a stage can resume | 20 |
| `sources.list` | the list of upstream URLs; `update-lock.sh` turns it into `sources.lock` | 20 |
| Split recipe | a recipe that makes two or more packages (`gcc` and `gcc-libs`) | 21 |
| Twin ISO (serial twin) | the live ISO with `console=ttyS0,115200`, so the serial console can log in; used by the `live-run.py` suites | 25 |
| `tsort` | the sort that puts recipes in build order from their dependencies | 21 |

## 4. Desktop, Wayland and graphics terms

| Term | Meaning | Chapter |
|---|---|---|
| Accent | the user's colour for focus and buttons; six choices (green, teal, blue, amber, orange, rose) | 08, 22 |
| Alt-Tab row | the row of window pictures shown while Alt-Tab is held | 23 |
| Client-side decoration | the program draws its own title bar (libflui with `decorated=1`) | 22 |
| `desktop.conf` | the shared settings file, `~/.config/freelfs/desktop.conf` | 22, 23 |
| dmabuf | a GPU buffer shared between processes by a file descriptor (`zwp_linux_dmabuf_v1`) | 23, 27 |
| DRM/KMS | the Linux kernel's display interface, used by flcomp to drive the screen | 23 |
| Dry pass | a libflui pass that handles input but draws nothing, because the compositor still holds the buffers | 22 |
| Flyout | a small popup above the panel (the volume slider, the battery panel) | 04, 23 |
| Immediate mode | the model of libflui: the draw callback runs on each event, and widgets draw and report in one call | 22 |
| `keyboard.conf` | the keyboard layout file, `/etc/keyboard.conf` (`LAYOUT=`, `VARIANT=`, `OPTIONS=`) | 22, 23 |
| Layer shell | a Wayland protocol for panels and overlays; **not implemented** in flcomp (fl_shell does that job) | 23 |
| libinput | the input library of flcomp (mice, touchpads, keyboards) | 23 |
| libseat | the library that gives a compositor access to devices without root | 23 |
| Night light | the warmer picture in the evening, made by the gamma tables of the screen | 23 |
| Output | a connector and the picture on it (`HDMI-A-1`, `eDP-1`) | 23 |
| Primary output | the output that carries the panel, the menus and the maximized windows | 23 |
| Performance mode | flcomp draws a fullscreen window alone, with no frame cap | 23 |
| Power saver | flcomp draws at most 30 frames a second and dims the backlight | 23 |
| Shadow buffer | flcomp's CPU copy of the desktop, from which the screen is filled | 23 |
| Shm | shared memory buffers (`wl_shm`), the CPU path for pictures | 23, 27 |
| Snapping | dragging or pressing a key to put a window on half the screen or maximize it | 23 |
| Subsurface | a surface drawn inside another one (`wl_subsurface`): FreeBWS draws the page this way | 27 |
| Wayland global | a service the compositor offers to clients (`wl_compositor`, `xdg_wm_base` and so on) | 23 |
| Wallpaper | the picture behind the windows; `.flw` files are FreeLFS's own format | 08 |
| Workspace | one of the virtual desktops of the grid (default 2 by 2) | 23 |
| Xwayland | the X11 server that runs inside the Wayland session for old X11 programs | 23 |
| X11 | the older display protocol; supported through Xwayland | 23 |

## 5. Security and system terms

| Term | Meaning | Chapter |
|---|---|---|
| bubblewrap (`bwrap`) | the tool WebKit uses to sandbox its web processes (namespaces, a seccomp filter) | 27 |
| btrfs | the file system of the installed system, with subvolumes | 12 |
| Crash screen | the "Something went wrong" program (`flcrash`), shown after a crash | 14 |
| CVE | a published vulnerability; `build/cve-check.py` checks the recipes against them | 26, 28 |
| EFI system partition (ESP) | the FAT partition with the boot loader and the kernels | 11, 20 |
| Egg | the "You can't do that." message for deleting a vital system folder | 24 |
| Flathub | the online catalogue of Flatpak apps | 13 |
| Flatpak | a way to install apps with their own libraries, per user here (`flatpak --user`) | 13 |
| `flreport` | see section 1 | 26 |
| Lid switch | a closed lid locks the screen (unless `lock_on_lid=0`) | 23 |
| `seat` group | group 98, the group of the programs that use `libseat` and the setuid helpers (`flpower`, `flmount`, `flauth`, `fltime-set`) | 15 |
| setuid | a program that runs with the owner's rights (root) when a user starts it; FreeLFS limits it to group `seat` where possible | 15 |
| Snapshot | a read-only copy of the root subvolume, made before an update (flsnap) | 13, 19 |
| `@`, `@home`, `@snapshots` | the btrfs subvolumes: the system, the homes, the snapshots | 12, 20 |
| Trusted client | a program allowed to use the privileged `fl_shell` requests (`flpanel`, `fllock`, `flgreet`, `flsettings`, `fldisplay`) | 23 |
| Secure Boot | the UEFI feature that starts only signed boot code; **options only** in FreeLFS | 28 |
| zram | compressed memory used as swap | 18, 28 |
| swappiness | how eagerly the kernel swaps; a slider in the planned Hardware app | 18, 28 |

## 6. Project and process terms

| Term | Meaning | Chapter |
|---|---|---|
| AGENT-RULES | `docs/AGENT-RULES.md`: the rules for the agents that work on the tree (no sudo, rootless, no commits by agents) | 25, 26 |
| Audit | a read-only review of the code or the tests, such as the tests audit of 2026-11-14 | 25 |
| Bug scan | the reviews of 2026-11-13 by area (compositor, browser, toolkit, shell, apps) in `docs/bugscan-2026-11-13/` | 27 |
| Contributor application | the GitHub issue with that title, the way to apply to contribute | 26 |
| Gentle | `build/gentle.sh`: runs a build with low priority and a memory limit | 20 |
| Milestone | a step of the plan, M0 to M10 (`docs/PLAN.md`) | 28 |
| Pre-alpha | the stage of the project: things break, nothing is stable, do not rely on it | 01 |
| QEMU | the virtual machine used for the tests; OVMF is its UEFI firmware, KVM its accelerator | 25 |
| Round | a numbered batch of work (round 17 is the latest in the notes); the commit subjects start with `Round N:` | 26 |
| Stage | a build script of `build/` (`00-host-check.sh` to `70-repo.sh`) | 20 |
| Suite | a test script (m8 to m18, boot-test, iso-test) | 25 |
| Tester | a person who runs the live ISO on a machine and reports what happens | 26 |
| Twin | see section 3 | 25 |
