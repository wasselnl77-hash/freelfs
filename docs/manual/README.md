# The FreeLFS manual

FreeLFS is an independent Linux distribution built from scratch, with its own package manager, boot loader, shell, Wayland
compositor, toolkit and desktop, all in C. It is **pre-alpha**: things break, nothing is stable yet, and it must not be used for
anything you cannot lose. See the warning in the top-level [README](../../README.md).

This manual is for three kinds of reader. Part I is for people who use FreeLFS. Part II is for people who run and maintain it.
Part III is for people who build it, write programs for it and contribute to it.

Where the manual says something is planned, it is marked as planned. Where the repository does not say whether something works,
the manual says "not verified". Nothing in it is a promise.

## Contents

### Part I: User guide

| Chapter | What it covers |
|---|---|
| [01-introduction.md](01-introduction.md) | What FreeLFS is, what it is for, and its pre-alpha status |
| [02-getting-started.md](02-getting-started.md) | Trying the live ISO, the first login and the first steps on the desktop |
| [03-installing.md](03-installing.md) | Installing to a disk with the installer, and what it erases; the first-boot choices |
| [04-the-desktop.md](04-the-desktop.md) | The panel, the start menu, windows, workspaces, notifications and the lock screen |
| [05-keyboard-shortcuts.md](05-keyboard-shortcuts.md) | Every keyboard shortcut of the desktop, the browser and the terminal, in one list |
| [06-apps.md](06-apps.md) | The programs that come with FreeLFS: Files, Terminal, Settings, Images, Documents, Music, Videos, Store and the rest |
| [07-files-drives-usb.md](07-files-drives-usb.md) | Files, the `/drives` folder, USB sticks, persistence and ejecting safely |
| [08-settings-reference.md](08-settings-reference.md) | Every page of Settings and Themes, and what each setting does |
| [09-troubleshooting.md](09-troubleshooting.md) | What to do when something does not work, and how to report it with `flreport` |

### Part II: System and admin

| Chapter | What it covers |
|---|---|
| [10-architecture.md](10-architecture.md) | How the pieces fit together: kernel, boot loader, init, packages, compositor, toolkit and apps |
| [11-boot-process.md](11-boot-process.md) | From the firmware through flboot and the kernel to the login screen and the splash |
| [12-filesystem-layout.md](12-filesystem-layout.md) | The disk layout: EFI partition, btrfs subvolumes, the folders of the system, homes and snapshots |
| [13-packages-flpkg.md](13-packages-flpkg.md) | The package manager `flpkg`: commands, groups, repositories, signatures, snapshots, updates and the Store |
| [14-services-init.md](14-services-init.md) | The init system and the services: `rc.conf`, the `rc.d` scripts, udev rules and the login screen service |
| [15-users-security.md](15-users-security.md) | Users, login, the `seat` group, root actions through `flauth`, and the security model |
| [16-networking.md](16-networking.md) | Wi-Fi with iwd, wired networking with dhcpcd, DNS, time, and the Settings pages for them |
| [17-hardware-drivers.md](17-hardware-drivers.md) | GPUs, firmware, sound, Bluetooth, printing, displays and power, and what is tested on real hardware |
| [18-performance-ram.md](18-performance-ram.md) | The memory budget for 1 GB of RAM, the measurements, and how to keep the system small |
| [19-admin-reference.md](19-admin-reference.md) | Every command-line tool of FreeLFS, with its options and what it changes |

### Part III: Developer guide

| Chapter | What it covers |
|---|---|
| [20-building-from-source.md](20-building-from-source.md) | Building the whole system rootless: host requirements, the build scripts in order, the image, the ISO, locks and reproducibility |
| [21-recipes.md](21-recipes.md) | The recipe format, groups, releases and dependencies, with two full examples and the steps to add a package |
| [22-flui-toolkit.md](22-flui-toolkit.md) | libflui: the immediate-mode model, windows, drawing, text, theme and widgets, with a hello-window example |
| [23-compositor-protocols.md](23-compositor-protocols.md) | flcomp internals: the Wayland globals, the private `fl_shell` protocol (versions 1 to 9), trusted clients, outputs, night light, snapping, X11 and the clipboard history |
| [24-writing-an-app.md](24-writing-an-app.md) | A step-by-step tutorial for a new `fl*` program: Makefile, `.desktop` file, recipe, errors, settings and a headless test |
| [25-testing.md](25-testing.md) | The test system: headless checks, the package suites, the QEMU drivers, the `.cmds` language, the round suites and what QEMU cannot prove |
| [26-contributing.md](26-contributing.md) | How to report problems, apply to contribute, the code and commit style, the MIT licence, and the FreeBWS help wanted |
| [27-freebws-internals.md](27-freebws-internals.md) | The FreeBWS browser on WPE WebKit: architecture, the ad blocker, the sandbox, the portal, settings, known bugs and how to help |
| [28-roadmap.md](28-roadmap.md) | What is built, what is planned (installer, translations, the Hardware app, the tester website), and the hardware and build-machine plans |
| [29-glossary.md](29-glossary.md) | Every `fl*` name and the project terms (flcomp, flpanel, flgm, snappel, the twin ISO, the seat group and the rest) |

## Conventions in this manual

- Commands are shown as you type them. `$` is not typed. Paths beginning with `~` are your home directory.
- A name in `code` font is the real name in the source or on the system (a program, a file, a setting key, a function).
- "Built" means the code is in the tree and was tested in QEMU or headless. "Planned" means written down as a plan. "Not verified"
  means the repository does not show it.
- The chapters cite the source file or the document they are based on, for example `src/flcomp/README` or `docs/HARDWARE.md`.
