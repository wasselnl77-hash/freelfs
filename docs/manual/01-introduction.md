# 1. Introduction

> FreeLFS is an independent Linux distribution built from scratch, with its own
> package manager, boot loader, shell, Wayland compositor and desktop. It is
> **pre-alpha**: useful for trying things out, not for anything you care about.

## Contents

- [What FreeLFS is](#what-freelfs-is)
- [Pre-alpha: read this first](#pre-alpha-read-this-first)
- [Goals](#goals)
- [How much memory it needs](#how-much-memory-it-needs)
- [What is included](#what-is-included)
- [How it differs from other distributions](#how-it-differs-from-other-distributions)
- [Where to go next](#where-to-go-next)

## What FreeLFS is

FreeLFS is a Linux distribution. Linux is the kernel; a distribution is
everything around it: the programs, the desktop, the installer and the way
software is installed and updated.

Most distributions are built on top of another one. FreeLFS is not. It starts
from the source code of the upstream programs (the kernel, C libraries, compilers
and so on) and builds the whole system itself, with recipes in the `recipes/`
folder of the project. Its own parts are written in C:

| Part | Name | What it does |
|---|---|---|
| Package manager | `flpkg` | Builds, installs, upgrades and removes packages in one transaction |
| Boot loader | `flboot` | Starts the kernel from a menu on the EFI system partition |
| Shell | `flsh` | The command line for the terminal (POSIX syntax, with colours and suggestions while you type) |
| Compositor | `flcomp` | The Wayland display server that draws the desktop |
| Panel and start menu | `flpanel` | The bar at the bottom, the start menu, the tray and notifications |
| Toolkit | `libflui` | The library all the FreeLFS windows are drawn with |
| Apps | `flfiles`, `flsettings`, `fledit`, ... | Files, Settings, text editor and the other programs |

The desktop is a classic one, in the spirit of Linux Mint's Cinnamon: a panel
with a start menu, windows you can move, snap and minimize, and a tray with the
usual icons. The look is flat and simple: dark or light colours, one accent colour
you choose, sharp corners.

The project is an experiment in progress. It is developed in the open in a
GitHub repository. Its status is written down in
[`README.md`](../../README.md) and in the plan files under `docs/`.

## Pre-alpha: read this first

FreeLFS is **pre-alpha**. What that means in practice:

- **Things will break.** Expect crashes, missing features and changes that are
  not backwards compatible.
- **Nothing is stable yet.** Not the package format, not the repository, not the
  configuration files.
- **It has been tested mostly in virtual machines** (QEMU). It has been run on one
  real laptop, an AMD Ryzen 5 7520U with a Radeon 610M graphics chip and a Realtek
  Wi-Fi chip (rtw89). That run found serious bugs. Shutdown and opening media from
  the Files app failed there in round 17 and are being fixed in round 18. Other
  hardware (Intel, most AMD machines) has not been tested at all. See
  [09-troubleshooting.md](09-troubleshooting.md) and `docs/HARDWARE.md`.
- **The installer erases the disk you pick.** Only try it on a spare computer or
  inside a virtual machine. Read [03-installing.md](03-installing.md) first.
- **No security support.** Known vulnerabilities are tracked in `docs/SECURITY.md`.
  The browser's web processes run in a bubblewrap sandbox, but there is no
  security team, no advisories and no guaranteed update schedule.
- **Do not rely on it to keep your data safe.** Back up anything you care about.

If you are curious, try the live system in a virtual machine or on a spare
computer, and send bug reports. That is what the project needs most right now.

## Goals

These are the aims the project works towards, as written in its plan:

- **Independent.** No parent distribution. Every package comes from a recipe the
  project keeps.
- **Clean and traditional.** A desktop that works the way people expect from a
  classic Linux desktop, with no surprises.
- **Light.** It must run on a computer with **1 GB of RAM**. Every app should
  idle at around 25 MB or less where possible.
- **Rolling release.** The kernel and the packages move forward. The plan is a
  rolling release with the newest 7.x kernel stable, and the 6.18 LTS kernel
  kept as a fallback in the boot menu.
- **Recoverable.** Btrfs snapshots are taken before every change to the system,
  and an older state can be started from the boot menu. See
  [09-troubleshooting.md](09-troubleshooting.md).
- **Plain C.** The FreeLFS parts are C programs with few dependencies. The project
  reuses proven libraries (Mesa, WebKit, mpv, MuPDF, libarchive and others) instead
  of writing codecs or parsers itself.

## How much memory it needs

The 1 GB target is measured in QEMU, a virtual machine, so real hardware will
differ a little. These are the most recent numbers in the project's notes:

| What | Number | Where it is measured |
|---|---|---|
| Live ISO, idle desktop after login | about 85 MB used (MemTotal minus MemAvailable) | QEMU, 1 GB, 2026-11-11 and round 15 |
| Boot to the login screen | about 18 s | QEMU, 1 GB |
| Compositor `flcomp` | about 12 MB (proportional memory) | live desktop, QEMU |
| Panel `flpanel` | about 10 MB | live desktop, QEMU |
| Web browser `FreeBWS`, one tab over five sites | about 530 MB resident, about 350 MB proportional. The goal is under 500 MB for three tabs, not yet reached | QEMU, 1 GB, `docs/M8-BROWSER.md` |
| Music player with a library | about 60 MB | source notes of `flmusic` |
| Task manager `General Manager` | about 17 MB | source notes of `flgm` |

The live session keeps everything in RAM. Files you save on the live system,
and packages you install into it, use memory too. One gigabyte is enough to look
around and do light work. Two gigabytes or more is comfortable. The details are in
`docs/RAM-PROFILE.md`.

Memory numbers on a real laptop are not known yet. The `flreport` tool (see
[09-troubleshooting.md](09-troubleshooting.md)) writes the memory figures to its
report, so a tester can send them.

## What is included

The base system is built from about 270 recipes in the `recipes/` folder. The
desktop group adds the parts a person sees. This is the list as it stands:

**Desktop and system**

- Compositor `flcomp` (Wayland, drawn on the processor) with Xwayland, so X11 programs run
- Panel `flpanel` with start menu, workspace pager, Wi-Fi, volume, battery, Bluetooth, night light, keyboard layout and clock
- Login screen `flgreet`, lock screen `fllock`, "Something went wrong" screen `flcrash`
- Boot screen `flsplash` and boot loader `flboot` with a menu of kernels and earlier states
- Settings, Themes, General Manager (task manager), Software Store, Snappel (screenshots)
- Notifications, system sounds, automatic time (NTP) and time zone
- Audio with PipeWire and WirePlumber, Bluetooth with BlueZ, printing with CUPS, Wi-Fi with iwd
- Btrfs snapshots (`flsnap`), update notifier `flupdate`, problem report `flreport`

**Apps**

- Files (`flfiles`), Terminal (`flterm`) with the shell `flsh`, Text Editor (`fledit`)
- Images (`flview`), Videos (`flvideo`), Music (`flmusic`), Documents (`flpdf`, PDF and e-books)
- Archives (`flarchive`), FreeBWS (the web browser, on WPE WebKit)
- FreeLFS install (the graphical installer)

**Software from outside**

- The Software Store installs Flathub apps (per user, through Flatpak) and FreeLFS packages. The FreeLFS package repository is not public yet, so only Flathub is usable today. See [06-apps.md](06-apps.md).

**Not in the box (yet)**

- A Firefox build. Firefox is available as a Flatpak from Flathub in the Store, on an installed system.
- Office suites, games (Steam runs through Flatpak, with helper `flgaming`, not tested on the laptop), and a Hardware app (planned).

## How it differs from other distributions

| Topic | FreeLFS | Typical distribution |
|---|---|---|
| Base | Built from its own recipes, no parent distribution | Derived from another distribution or from packages it maintains |
| Package manager | `flpkg`, its own format and signing | apt, dnf, pacman or similar |
| Boot loader | `flboot`, its own, reads the kernel from the EFI partition | GRUB or systemd-boot |
| Shell | `flsh`, its own (POSIX syntax) | bash, zsh or fish |
| Desktop | Own compositor and toolkit, drawn on the processor, no GPU driver needed for the desktop itself | GNOME, KDE, Cinnamon, ... on Mesa and a compositor library |
| Apps | Mostly own C apps, plus Flathub | Mostly the desktop's own app suite |
| Init | sysvinit with FreeLFS's own shell-script `rc` scripts, not systemd | Usually systemd |
| Snapshots | Btrfs, taken before each package change, bootable from the menu | Often optional |
| Secure Boot | Not supported yet (the boot loader and kernel are not signed) | Usually supported |
| Hardware | Tested on very little real hardware | Tested on very many machines |

A few things are not decided or not done in FreeLFS yet: ARM (aarch64) support,
Secure Boot, disk encryption, dual boot with Windows, and the BIOS (non-UEFI)
boot path. The installer's README lists these as "not supported" (see
[03-installing.md](03-installing.md)).

## Where to go next

| If you want to | Read |
|---|---|
| Try the live system | [02-getting-started.md](02-getting-started.md) |
| Install it on a disk | [03-installing.md](03-installing.md) |
| Learn the desktop | [04-the-desktop.md](04-the-desktop.md) |
| Look up a key combination | [05-keyboard-shortcuts.md](05-keyboard-shortcuts.md) |
| Learn each app | [06-apps.md](06-apps.md) |
| Use USB sticks and files | [07-files-drives-usb.md](07-files-drives-usb.md) |
| Look up every setting | [08-settings-reference.md](08-settings-reference.md) |
| Fix a problem or report a bug | [09-troubleshooting.md](09-troubleshooting.md) |
| The system and administration guide | Chapters 10 to 19 (another author) |
| Build FreeLFS and write code for it | Chapters 20 to 29 (another author) |
| The full table of contents | [README.md](README.md) |

*Status of this chapter: written from the project's source code, README files
and plan documents as of 2026-11-14. Where a number comes from a test rather
than from a plan, the text says so.*
