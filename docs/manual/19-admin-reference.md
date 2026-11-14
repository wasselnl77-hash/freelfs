# 19. Admin reference: every FreeLFS command-line tool

This chapter lists every command that FreeLFS installs, with the usage line that the program prints (copied from its source), where it is installed, and what it is for.

Status: round 17 (2026-11-14). Usage lines are copied from the sources in `src/`, `recipes/` and `config/`. Where a tool has no usage line, the chapter says what it takes instead.

## Contents

1. [How to read this chapter](#191-how-to-read-this-chapter)
2. [Packages and snapshots](#192-packages-and-snapshots)
3. [Boot, init and the live system](#193-boot-init-and-the-live-system)
4. [Accounts, security and root actions](#194-accounts-security-and-root-actions)
5. [Desktop core](#195-desktop-core)
6. [Settings and system tools](#196-settings-and-system-tools)
7. [Apps](#197-apps)
8. [Sound, Bluetooth, printing and hardware](#198-sound-bluetooth-printing-and-hardware)
9. [Network and time](#199-network-and-time)
10. [Installer](#1910-installer)
11. [Browser and documents](#1911-browser-and-documents)
12. [Shell and developer tools](#1912-shell-and-developer-tools)
13. [Scripts run by the system (not for typing)](#1913-scripts-run-by-the-system-not-for-typing)
14. [Upstream tools that FreeLFS ships](#1914-upstream-tools-that-freelfs-ships)

## 19.1 How to read this chapter

- **Usage** is the line the program prints with `--help`, a bad argument or no argument. Where a tool has more options in its header comment, the notes say so.
- **Setuid** means the program runs as root for a person in group `seat`. Such a program is always `root:seat 4750` (see [15](15-users-security.md)).
- Most programs have a `README` in their `src/` folder, and most install it as `/usr/share/doc/NAME/README`.
- `flcomp`, `flsh`, the apps and the tools with `--help` also take `-h` and `--help`.

## 19.2 Packages and snapshots

| Tool | Installed at | Usage (as printed) | What it does |
|---|---|---|---|
| `flpkg` | `/usr/bin/flpkg` | `usage: flpkg [--root DIR] [--config FILE] COMMAND [ARGS]` | the package manager; the commands are in 19.2a below |
| `flpkg install` | (subcommand) | `usage: flpkg install [--nodeps] [--unsigned] [--asdeps] [--needed] [--overwrite GLOB] FILE.flpkg...` | installs names from the repos or `.flpkg` files (`-S`) |
| `flpkg remove` | (subcommand) | `usage: flpkg remove [--cascade] [--recursive] [--nodeps] PKG...` | removes packages (`-R`) |
| `flpkg owns` | (subcommand) | `usage: flpkg owns PATH...` | which package owns a file (`-Qo`) |
| `flpkg fix-owners` | (subcommand) | `usage: flpkg fix-owners [--keep]` | applies deferred ownership; `--keep` leaves the database alone |
| `flpkg search` | (subcommand) | `usage: flpkg search TEXT...` | searches the synced indexes (`-Ss`) |
| `flpkg check-updates` | (subcommand) | `usage: flpkg check-updates [--offline] [--count]` | lists available upgrades, no root needed |
| `flpkg build` | (subcommand) | `usage: flpkg build [options] [DIR]`, options `-o/--outdir DIR`, `-s/--sources DIR`, `--nocheck`, `--keep` | builds the recipe in DIR into a package |
| `flpkg repo add` | (subcommand) | `usage: flpkg repo add [--key FILE] [--remove-old] [--valid-days N] [--serial N] DIR [FILE.flpkg...]` | adds packages to a repo and re-signs its index |
| `flpkg key generate` | (subcommand) | `usage: flpkg key generate PATH   (writes PATH.key and PATH.pub)` | creates a signing key pair |
| `flpkg key introduce` | (subcommand) | `usage: flpkg key introduce --key OLD.key --new NEW.pub [--retire-days N] DIR` | writes a key rotation statement |
| `flpkg key list` | (subcommand) | (no usage line) | lists the trusted keys and their retirement dates |
| `flpkg list` | (subcommand) | (no usage line) | installed packages; `--explicit`, `--deps` |
| `flpkg info` / `files` | (subcommands) | `usage: flpkg info PKG \| flpkg info -f FILE.flpkg` (and `files`) | details or file list of a package; `info --repo PKG` for the repos |
| `flpkg verify` | (subcommand) | (no usage line) | checks installed files against the database (`-Qk`) |
| `flpkg upgrade` | (subcommand) | (no usage line) | upgrades everything; `--sync` refreshes first (`-Su`, `-Syu`); `--yes` |
| `flpkg sync` | (subcommand) | (no usage line) | refreshes the repo indexes (`-Sy`) |
| `flpkg snapshots` | (subcommand) | (no usage line) | runs `flsnap list` |
| `flpkg history` | (subcommand) | (no usage line) | past transactions |
| `flpkg help` / `version` | (subcommands) | `flpkg help` prints the usage; `flpkg version` prints `flpkg VERSION` | |
| `flsnap` | `/usr/bin/flsnap` (setuid) | see the block below | btrfs snapshots |
| `flsnap-notice` | `/usr/bin/flsnap-notice` | (no usage line; runs at login) | the notice on an earlier state |
| `flupdate-notify` | `/usr/bin/flupdate-notify` | `flupdate-notify --once` checks now; no other options. Environment: `FLUPDATE_FIRST`, `FLUPDATE_INTERVAL`, `FLUPDATE_RETRY` | the update notice; refuses root |
| `flstore` | `/usr/bin/flstore` | `usage: flstore [--updates]` | the Software Store; `--updates` opens the Updates tab |
| `flpackages` | `/usr/bin/flpackages` | `usage: flpackages` | the Packages app (the Store replaces it in the menu) |
| `flpkg-seat` | `/usr/bin/flpkg-seat` (setuid) | `usage: flpkg-seat install\|remove NAME...` | installs or removes packages for the seat, after the password |

**flsnap** (`src/flsnap/flsnap.c`), the full usage text:

```text
usage: flsnap create [--reason TEXT] [--packages FILE] [--auto]
       flsnap list [--machine] | show N | status
       flsnap delete N... | prune [--keep K]
       flsnap rollback [N]     make snapshot N (default: the one running) the current state
Snapshots of the root file system (btrfs @snapshots); keep= in /etc/flsnap.conf.
```

`flsnap delete`, `prune` and `rollback` ask for the password (flauth). `create`, `list`, `show` and `status` do not.

**flpkg subcommands** (in the sub-table above) are the commands of the `flpkg` program (`src/flpkg/main.c`). The full list with aliases is in [13](13-packages-flpkg.md).

## 19.3 Boot, init and the live system

| Tool | Installed at | Usage (as printed) | What it does |
|---|---|---|---|
| `flboot-install` | `/usr/bin/flboot-install` | `usage: flboot-install [--esp DIR] [--update]` | puts flboot on the ESP and writes `flboot/flboot.conf` if none exists; `--update` is what the trigger runs |
| `flboot` | `/usr/lib/flboot/flboot.efi` | (no command line; the firmware starts it) | the boot menu |
| `rc` | `/etc/rc.d/rc` | `rc RUNLEVEL`, run by init (`0 1 2 3 4 5 6 S`) | enters a runlevel; stops the desktop and the services on 0 and 6 |
| `rc.sysinit` | `/etc/rc.d/rc.sysinit` | (run by init only) | the one-time boot setup |
| `rc.shutdown` | `/etc/rc.d/rc.shutdown` | (run by `rc` only) | clock, processes, unmount, halt or reboot |
| `init.d/NAME` | `/etc/rc.d/init.d/` | `start`, `stop`, `restart` or `status` (`usage: NAME {start\|stop\|restart\|status}` for the daemon scripts) | one service |
| `flinitrd` | `/init` in the live initramfs | (no command line; kernel options, see [11](11-boot-process.md)) | finds and starts the live system |
| `flpersist` | `/usr/bin/flpersist` (setuid) | `usage: flpersist create SIZE [--btrfs] \| status` | the persistence file for the live stick |
| `flsplash` | `/usr/sbin/flsplash` | `usage: flsplash [--handover \| --text \| --render MS W H FILE.ppm]` | the boot screen; `--handover` gives the screen to the login, `--text` shows the text console |
| `flsetup` | `/usr/bin/flsetup` | `usage: flsetup` and "Choose the keyboard layout and write /etc/keyboard.conf" | the first-boot keyboard choice |
| `flgreet` | `/usr/sbin/flgreet` | (no options; run by init on tty1) | the login screen (see [15](15-users-security.md)) |
| `flgreet-ui` | `/usr/libexec/flgreet-ui` | `usage: flgreet-ui` (runs inside flgreet's compositor) | the login screen itself |

**flinitrd** options on the kernel command line: `flive.timeout=SECONDS` (default 20), `flive.iso=PATTERN` (default `freelfs-live*.iso`), `flive.debug`.

**flboot.conf** keys: `timeout`, `cmdline`, `default`, `entry` (see [11](11-boot-process.md), 11.3).

## 19.4 Accounts, security and root actions

| Tool | Installed at | Usage (as printed) | What it does |
|---|---|---|---|
| `flauth` | `/usr/bin/flauth` (setuid) | `usage: flauth [--reason TEXT] [-- CMD ARGS...]` | the password prompt; with a command, runs it after the password |
| `flauth-dialog` | `/usr/libexec/flauth-dialog` | (started by flauth) | the password window |
| `flpower` | `/usr/bin/flpower` (setuid) | `usage: flpower off\|restart\|logout\|saver on\|saver off` | power off, restart, end the session, power saving (see [17](17-hardware-drivers.md)) |
| `fllock` | `/usr/bin/fllock` | `usage: fllock` and `fllock --demo [--typed N] [--wrong\|--busy\|--stuck]` (screenshots; `FLUI_PPM=FILE`) | the lock screen |
| `fllock-check` | `/usr/libexec/fllock-check` (setuid) | (reads one password line from stdin; exit 0 right, 1 wrong, 2 no password can unlock) | checks the password for the lock screen |
| `flmount` | `/usr/bin/flmount` (setuid) | `usage: flmount list \| mount DEV \| unmount DEV \| eject DEV   (--internal: also internal disks)` | removable drives; the header adds `--hotplug DEV`, `--auto`, `--gone DEV` (root, udev and rc only) |
| `fltime-set` | `/usr/bin/fltime-set` (setuid) | `usage: fltime-set zone NAME \| ntp on\|off \| sync \| time EPOCH` | time zone, NTP switch, sync, a clock set while NTP is off |
| `flinstall-helper` | `/usr/libexec/flinstall-helper` (setuid) | `usage: flinstall-helper [--dry-run] [--image FILE [--source DIR] [--kernel FILE] [--iwd DIR] [--workdir DIR]]` | the installer's privileged half (see 19.10) |
| `flcrash-test` | `/usr/bin/flcrash-test` | `usage: flcrash-test KIND`; kinds: `segv`, `abort`, `bus`, `ill`, `fpe`, `kill`, `hup`, `other`, `exit`, `startfail` | crashes your own flcomp on purpose, to see the crash screen |

**flauth** exit codes: 0 right (or the command ran), 1 wrong password three times, 2 cancelled, 3 locked out or broken, 4 no password can unlock the account.

**fllock** keys and the lock behaviour: see [15](15-users-security.md), 15.6.

## 19.5 Desktop core

| Tool | Installed at | Usage (as printed) | What it does |
|---|---|---|---|
| `flcomp` | `/usr/bin/flcomp` | `usage: flcomp [--firstboot]`, with the environment variables `FLCOMP_HEADLESS=WxH`, `FLCOMP_SCREENSHOT=F`, `FLCOMP_PANEL=CMD`, `FLCOMP_SESSION=CMD`, `FLCOMP_SETUP=CMD` | the Wayland compositor and the session |
| `flcomp` session script | `/usr/lib/flcomp/session` | (started by flcomp) | D-Bus session, pipewire, wireplumber, update notice (see [14](14-services-init.md)) |
| `desktop.sh` | `/etc/profile.d/desktop.sh` | (run by the login shell on tty1) | starts flcomp and `flcrash` on failure |
| `flpanel` | `/usr/bin/flpanel` | `usage: flpanel [--demo] [--menu] [--volume] [--workspaces] [--cards] [--center] [--battery]` | the panel; the options open a flyout or the demo, for tests |
| `flnotify` | `/usr/bin/flnotify` | `usage: flnotify [-a APP] [-i ICON] [-u low\|normal\|critical] [-t MS] [-A KEY=LABEL]...` and `[-r ID] [-T] [-w] SUMMARY [BODY]`, and `flnotify --dnd on\|off\|toggle\|status \| --clear \| --center \| --count` | a notification (`-w` waits for the answer; `--dnd` is do-not-disturb) |
| `flcrash` | `/usr/bin/flcrash` | `usage: flcrash --status N [--secs N] [--crashes N] [--prog NAME] [--log FILE]`, `flcrash --demo [CODE] [--view log] [--size WxH]`, and `flcrash --text`; codes: `SIGSEGV SIGABRT SIGBUS SIGILL SIGFPE SIGKILL SIGHUP NO-SEAT NO-DISPLAY NO-RUNTIME NO-MEMORY START-FAILED EXIT-<n> RESTART-LOOP` | the "Something went wrong" screen (`FLCRASH_PPM=FILE` writes a picture) |
| `flcrash --cant` | (same program) | `usage: flcrash --cant PATH...  (exit 0: go ahead, 1: Never mind)` | the "You can't do that." screen for deleting vital folders |
| `flsocketd` | `/usr/sbin/flsocketd` | `usage: flsocketd [-m MODE] [-g GROUP] SOCKET -- COMMAND [ARGS...]` | starts a daemon on the first connection to its socket (used for CUPS) |
| `flportal` | `/usr/libexec/flportal` | `usage: flportal [-v]` | the portal backend (started by D-Bus; `-v` logs requests) |
| `general-manager` | `/usr/bin/general-manager` | `usage: general-manager [--tab processes\|performance\|startup\|users\|details]` | the task manager (Ctrl+Shift+Esc) |
| `fl-drawtest` | `/usr/bin/fl-drawtest` | `usage: fl-drawtest [-t SECONDS] [-o FILE.ppm]` | a drawing test of the display path |
| `flui` (library) | `/usr/lib/libflui*`, `/usr/include/flui.h`, `flpalette.h` | (a library, no command) | the toolkit ([10](10-architecture.md)) |

**flcomp** keys and desktop settings are in [15](15-users-security.md) (15.5 and 15.6) and [17](17-hardware-drivers.md).

## 19.6 Settings and system tools

| Tool | Installed at | Usage (as printed) | What it does |
|---|---|---|---|
| `flsettings` | `/usr/bin/flsettings` | `usage: flsettings [--page general\|display\|wallpaper\|mouse\|keyboard\|workspaces\|datetime\|wifi\|bluetooth\|printers\|snapshots\|about]` | Settings; the page option opens one page. `--set-wallpaper FILE` sets a wallpaper (PNG or JPEG) |
| `fldisplay` | `/usr/bin/fldisplay` | `usage: fldisplay list \| set NAME [mode=WxH@HZ] [scale=S] [pos=X,Y] [enabled=0\|1] [primary=1]` and `usage: fldisplay nightlight on\|off\|auto [temp=K] [from=HH:MM] [to=HH:MM]`; also `fldisplay revert` | outputs and night light (see [17](17-hardware-drivers.md), 17.9) |
| `fltheme` | `/usr/bin/fltheme` | `usage: fltheme [--page presets\|colours\|fonts\|windows\|panel\|animations]` | Themes |
| `flreport` | `/usr/bin/flreport` | `usage: flreport [-o DIR] [-q]` (`-h` describes it) | a report of logs and hardware for a bug report; writes `~/freelfs-report-DATE.tar.gz` |
| `flsound` | `/usr/bin/flsound` | `usage: flsound [--force] NAME \| flsound --write NAME FILE.wav \| flsound --list` | plays a system sound; `--write` saves one as WAV |

**fldisplay** `revert` takes the settings back to the state before the last `set`. There is no automatic revert.

**flreport** collects logs (`/var/log`, the session and compositor logs), kernel messages, device lists from `/sys`, settings, mounts, network information and `memory.txt`. It runs as an ordinary user and leaves out what needs more rights. It keeps no passwords.

## 19.7 Apps

| Tool | Installed at | Usage (as printed) | What it does |
|---|---|---|---|
| `flfiles` | `/usr/bin/flfiles` | `usage: flfiles [FOLDER]` | the file manager ("Files"); Ctrl+H shows hidden files |
| `fledit` | `/usr/bin/fledit` | `usage: fledit [FILE...]` | the text editor; Ctrl+S saves, Ctrl+O opens, Ctrl+F finds, Ctrl+G goes to a line |
| `flterm` | `/usr/bin/flterm` | `usage: flterm [-e COMMAND [ARGS...]]` | the terminal |
| `flview` | `/usr/bin/flview` | `usage: flview [--rotate N] [--zoom PERCENT] [--no-strip] [FILE\|FOLDER]` | the image viewer ("Images") |
| `flpdf` | `/usr/bin/flpdf` | `usage: flpdf [--page N] [--zoom PERCENT] [--find TEXT] [--no-contents] FILE` | the document viewer ("Documents") |
| `flmusic` | `/usr/bin/flmusic` | `usage: flmusic [--tab albums\|artists\|songs] [--search TEXT] [FOLDER \| FILE...]`; `flmusic --ctl play-pause\|stop\|next\|previous` (from flcomp's notes) | the music player ("Music"); the `--ctl` form is what the media keys use |
| `flvideo` | `/usr/bin/flvideo` | `usage: flvideo [--start SECONDS] [--pause] [FILE]` | the video player ("Videos") |
| `flarchive` | `/usr/bin/flarchive` | `usage: flarchive [ARCHIVE]` and `flarchive --extract-here ARCHIVE...` | the archive tool |
| `snappel` | `/usr/bin/snappel` | `usage: snappel [--full \| --region \| --window] [--delay SECONDS] [--edit]` | the screenshot tool; Print and Shift+Print use it |
| `freebws` | `/usr/bin/freebws` | (no usage line in the source; options below) | the browser |
| `freebws-abpconv` | `/usr/bin/freebws-abpconv` | `usage: abpconv [-n MAXRULES] OUTDIR LIST...` (the program prints this name) | converts ad-block lists for the browser |
| `dssl2css` | `/usr/bin/dssl2css` | `usage: dssl2css [options] FILE...      (FILE - is standard input)`, options `-o FILE`, `--json`, `--tree`, `--features`, `--check`, `--banner`, `-q`, `--strict-engine`, `--version`, `--help` | DSSL to CSS, used by the browser's own pages |

**freebws** options found in the source: `--sandbox-check` (tries the web-process sandbox and reports, exit 0 when it works), `--sandbox-child` (what the probe runs inside bwrap), `--adblock-update` (updates the ad-block lists), `--adblock-build` (rebuilds the ad-block data). See [15](15-users-security.md), 15.8.

**flview** and **flmusic** are the image and music apps; their desktop shortcuts and the "Set as wallpaper" action are in [17](17-hardware-drivers.md) and [10](10-architecture.md).

**flterm -e** runs a command instead of a shell. Without `-e` it starts the user's shell.

## 19.8 Sound, Bluetooth, printing and hardware

| Tool | Installed at | Usage (as printed) | What it does |
|---|---|---|---|
| `flgaming` | `/usr/bin/flgaming` (from `recipes/gaming-config`) | `flgaming check`, `flgaming steam-install`, `flgaming steam-prepare`, `flgaming steam-library PATH` (prints the usage when called with no argument) | checks the machine for gaming, prepares Steam's Flatpak install for the person |
| `flcupsd` | `/usr/libexec/flcupsd` | (started by flsocketd; no options) | runs cupsd and avahi on demand, stops them after 60 s idle |
| `pdftopdf`, `pdftopwg` | CUPS filter directory | (run by CUPS; no options) | convert PDF for the printer |
| `bluetoothctl`, `lp`, `lpstat`, `lpadmin`, `lpoptions`, `cancel`, `lpinfo` | the bluez and CUPS packages | upstream usage | the tools Settings uses for Bluetooth and printing; see 19.14 |

**flgaming** (`recipes/gaming-config/flgaming`): `check` changes nothing; `steam-install` installs Steam from Flathub for the person; `steam-prepare` makes the runtime's 32-bit and GL extensions available; `steam-library PATH` lets Steam use a second disk for its library. It works only with the person's own Flatpak install (`flatpak --user`).

## 19.9 Network and time

| Tool | Installed at | Usage (as printed) | What it does |
|---|---|---|---|
| `fl-sntp` | `/usr/sbin/fl-sntp` | `usage: fl-sntp [-1] [-q SERVER[:PORT]] [-c CONF] [-s STATUSFILE]` | the time client (the `ntp` service runs it); `-1` once, `-q` queries one server |
| `fltime-set` | see 19.4 | | |
| `resolvconf` (iwd's stand-in) | `/usr/lib/iwd/bin/resolvconf` | `usage: resolvconf -a NAME < config \| resolvconf -d NAME` | writes `/etc/resolv.conf` from iwd's calls (see [16](16-networking.md)) |
| `iwctl` | (iwd package) | upstream usage (`iwctl --help`) | iwd's client |
| `flsettings --page wifi` | see 19.6 | | the Wi-Fi page |

## 19.10 Installer

| Tool | Installed at | Usage (as printed) | What it does |
|---|---|---|---|
| `flinstall` | `/usr/bin/flinstall` | `usage: flinstall` | the window: language, keyboard, Wi-Fi, disk, account, time zone, summary, install |
| `flinstall-helper` | `/usr/libexec/flinstall-helper` (setuid) | `usage: flinstall-helper [--dry-run] [--image FILE [--source DIR] [--kernel FILE] [--iwd DIR] [--workdir DIR]]` | the privileged half: partitions, formats, copies, writes the boot loader. `--dry-run` only prints the plan. `--image` works on a file (for tests); the test options are refused when the program runs with more privilege than the caller |
| `flboot-install` | see 19.3 | | writes the boot loader to the target ESP |

The installer's partition and format rules are in [12](12-filesystem-layout.md), 12.1. Its password rules are in [15](15-users-security.md), 15.11.

## 19.11 Browser and documents

| Tool | Installed at | Usage | What it does |
|---|---|---|---|
| `freebws` | `/usr/bin/freebws` | see 19.7 | the browser |
| `freebws-abpconv` | `/usr/bin/freebws-abpconv` | see 19.7 | ad-block list converter |
| `dssl2css` | `/usr/bin/dssl2css` | see 19.7 | DSSL to CSS |
| `flpdf` | `/usr/bin/flpdf` | see 19.7 | document viewer |
| `flportal` | `/usr/libexec/flportal` | see 19.5 | the portal backend for Flatpak apps |

## 19.12 Shell and developer tools

| Tool | Installed at | Usage (as printed) | What it does |
|---|---|---|---|
| `flsh` | `/usr/bin/flsh` | `usage: flsh [-ilsc] [-euxfnvCa] [-o OPTION] [SCRIPT [ARG...] \| -c COMMAND [NAME [ARG...]]]`, with `-c COMMAND`, `-s`, `-i`, `-l` / `--login`, `--version` | the POSIX shell and login shell. Interactive shells read `/etc/flshrc` and `~/.config/flsh/flshrc`; `help` lists the builtins |
| `fl-drawtest` | `/usr/bin/fl-drawtest` | see 19.5 | |
| `flcrash-test` | `/usr/bin/flcrash-test` | see 19.4 | |
| `libdssl` headers | `/usr/include/dssl.h` | (a header) | the DSSL library |
| `recipe.sh` | `/usr/lib/flpkg/recipe.sh` | `usage: recipe.sh dump\|run RECIPE [FUNC [OUT]]` | runs a recipe's functions; used by flpkg's build |
| `genart` and `genlockfont` | build tools, not installed | `usage: genart [-f FONT.ttf] [-o FILE] [-w WALLPAPER] [-a RRGGBB] [-W]`; `usage: genlockfont FONT.otf > lockfont.h` | build-time art and lock-screen font; they run in the source tree |

## 19.13 Scripts run by the system (not for typing)

| Script | Installed at | Notes |
|---|---|---|
| `/etc/rc.d/rc`, `rc.sysinit`, `rc.shutdown`, `functions` | `/etc/rc.d/` | see [14](14-services-init.md) |
| `/etc/rc.d/init.d/*` | `/etc/rc.d/init.d/` | `start`, `stop`, `restart`, `status`; the network script adds nothing else. `bluetooth` also takes `maybe-stop`. Its usage line: `usage: NAME {start\|stop\|restart\|status}` (the daemon scripts) |
| `/etc/profile.d/desktop.sh` | `/etc/profile.d/` | see [11](11-boot-process.md) |
| `/usr/lib/flcomp/session` | `/usr/lib/flcomp/` | session services (see [14](14-services-init.md), 14.5) |
| `/usr/libexec/flpkg/pre-transaction.d/10-flsnap` | (flsnap package) | the snapshot before each package change |
| `/usr/lib/flpkg/triggers` | (flpkg package) | the list of post-transaction commands (see [13](13-packages-flpkg.md)) |
| `/usr/bin/flsnap-notice` | (flsnap package) | runs at login; exits at once on a normal boot |
| `/usr/lib/iwd/bin/resolvconf` | (iwd package) | see 19.9 |

## 19.14 Upstream tools that FreeLFS ships

These are upstream programs with their own manuals. FreeLFS builds them and this chapter does not repeat them: `man NAME` or `NAME --help`.

| Area | Tools |
|---|---|
| Base | the GNU coreutils, bash, sed, grep, findutils, tar, gzip, util-linux (`mount`, `umount`, `fsck`, `hwclock`, `swapon`), `kmod` (`modprobe`, `lsmod`), `procps-ng` (`ps`, `free`, `pidof`), `sysvinit` (`init`, `telinit`, `halt`, `reboot`, `poweroff`), `sysklogd` (`syslogd`), `iproute2` (`ip`, `ss`), `dhcpcd`, `dbus-daemon`, `dbus-uuidgen`, `seatd` |
| Disks | `mkfs.btrfs`, `btrfs` (btrfs-progs), `mkfs.vfat`, `mtools`, `e2fsprogs`, `squashfs-tools`, `libisoburn` (`xorriso`) |
| Devices | `udevadm`, `udevd`, `udev-hwdb` (the udev package) |
| Wi-Fi and Bluetooth | `iwctl` (iwd), `bluetoothctl` (BlueZ) |
| Printing | `lp`, `lpstat`, `lpadmin`, `lpoptions`, `lpinfo`, `cancel`, `cupsd` (CUPS) |
| Audio | `pw-cat`, `pw-play` (PipeWire), `wpctl` (WirePlumber) |
| Packages (Flatpak) | `flatpak` |
| Video and graphics | `ffmpeg`, `ffprobe` (FFmpeg), `mpv` |

Any command that FreeLFS does not list here is either upstream (see the tables above) or not verified. Check `flpkg owns /usr/bin/NAME` to see which package installed it.
