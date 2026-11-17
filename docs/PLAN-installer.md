# Installer plan (planned for round 19, after round 18)

The user chose these on 2026-11-14. The installer is `src/flinstall`; today it has these steps: language, keyboard, Wi-Fi, disk, account, time zone, summary, install.

## Time zone, keyboard, language
- **World map time zone picker:**
  - click a region on a flat map, or search for a city
  - the region and the city show on the map
  - the language still suggests a zone (`LANGS[].zone`)
  - the map is drawn by flui from a small vector or 1-bit asset; keep RAM low
- **Keyboard layouts:** only 4 today (de us uk fr in `fl_kbd_layouts`).
  - Add all layouts for the installer's languages, plus common variants (us intl, ch, at, be, nordic and others).
  - Add a **keyboard test field** to try é ü @ and similar before continuing.
- **Desktop language** (changeable later in Settings), with full translations of all fl* apps:
  - the panel, Settings, Files, the store, the installer, the greeter and the other apps
  - the same 12 languages the installer lists: en_US, en_GB, de, fr, es, it, pt_BR, nl, pl, cs, sv, tr
  - Haiku agents translate
  - a small gettext-like catalog in flui, with no glibc gettext dependency needed if it's lighter
  - the default is English when a string is missing

## Disk
- **Dual-boot with Windows/Linux:** detect other systems, shrink a partition (NTFS needs ntfsresize: check the licence and size), and add flboot menu entries.
- **Disk encryption:** a LUKS2 checkbox and a passphrase, unlocked in flinitrd. Needs cryptsetup, and a prompt in flsplash/flinitrd.
- **Manual partitioning:** an advanced screen with partitions, filesystem and mount points.
- **Swap choice:** zram (the default on PCs with 2 GB or less), a swap file, or none.

## Setup
- **App picks:** checkboxes for Firefox, LibreOffice, VLC and others, installed through the Store/Flatpak after install, or during it when online.
- **Auto-login, plus a hostname field.**

## Around the install
- **Hardware check first:** warn about low disk space, no internet, a low battery, an unsupported GPU (for example old Radeon without r600) or low RAM.
- **Install log and report:** on failure show the log, plus a button that saves an flreport to a USB stick.
- **Update during install:** when online, install the newest packages so the first boot is already up to date.
- **Welcome app** (first login, can be reopened from the menu):
  - "New? Click here" (short tour: Store, Settings, updates, shortcuts)
  - "Settings"
  - "We need testers! (click here to apply)": the link target is TBD, for example GitHub issues or discussions
  - later, not now: "Support / buy me a coffee"

## Order
1. Translation system and keyboard layouts (everything else uses them)
2. Map, test field, hostname/auto-login, swap
3. Welcome app, hardware check, log/report, app picks, update during install
4. Encryption, manual partitioning, dual-boot (the riskiest; test with disk images in QEMU, never real disks)

## Added 2026-11-14 (second set of answers)
- **zram and swappiness:** the installer's swap step also sets swappiness (e.g. a slider, default about 100 for zram). You can change both later in the Hardware app.
- **New app "Hardware"** (universal FreeLFS app, `src/flhw`), with pages:
  - **Device overview:** CPU, GPU and the driver in use, RAM, disks, Wi-Fi and Bluetooth chips, battery health. A "Copy info" button for bug reports (and include it in flreport).
  - **Drivers and firmware:** shows missing firmware, or a GPU running without acceleration (e.g. old Radeon without r600, Intel before clang), and how to fix it.
  - **Power and CPU:** power profiles (saver/balanced/performance, using `flpower saver`), CPU governor, lid and low-battery actions.
  - **Sensors and disk health:** temperatures and fans (hwmon), SSD/HDD SMART health.
  - **Memory:** zram size and algorithm, swap file, swappiness.
  - Privileged writes go through a small setuid helper limited to the group seat, with flauth asking for the password, like flmount/flpower.
- **Tester website:** start on GitHub Pages, move to its own domain later. Content: what FreeLFS is, ISO download, how to test, how to report (GitHub Issues with a tester template). The welcome app's "We need testers!" links there.
- **FreeBWS as a community project, staying in the main repo:**
  - "help wanted" and "good first issue" labels on FreeBWS issues
  - docs/CONTRIBUTING-freebws.md (build, architecture, known bugs: the Cloudflare Turnstile check, Google login)
  - Firefox (Flatpak) is the recommended browser; FreeBWS is the light one for 1 GB PCs
- **App names: the real name, with what it is in small text under it:**
  - **Start menu and search:** the real name big ("FreeBWS"), "Web browser" small under it; search matches both.
  - **Window titles and taskbar:** the real name ("FreeBWS", "General Manager").
  - Needs `GenericName=` in every .desktop file, read by flpanel's menu and search.

## Branding (2026-11-16)
- **Tagline:** "Scratch, from scratch."
- **Welcome app greeting:** "Hello! Your free RAM is calling for you!" Below it, "5 things to do with the RAM you just saved" (draft list in the chat, final wording TBD), plus a live counter "FreeLFS is using N MB right now".
- **Welcome list, draft:**
  1. 🦊 Open 47 more browser tabs and still feel nothing
  2. 🎮 Give it to your games. They deserve it more than Windows Update does
  3. 🧠 Let your laptop from 2012 breathe for the first time in years
  4. 🐧 Brag on the internet that your whole desktop uses less than one Chrome tab
  5. 💤 Absolutely nothing. Free RAM is happy RAM (don't listen to those pirates who say differently!)

## Taskbar previews (requested 2026-11-17)
Hovering a taskbar entry shows a small window above the taskbar with:
- a live thumbnail of the window (flcomp already holds a CPU copy of every window; add an fl_shell request so flpanel can get a scaled thumbnail)
- the window title, and a small × in the corner to close that window
- one row per window when an app has several
- for apps playing media (MPRIS over D-Bus: Firefox, VLC, Spotify, flmusic, flvideo): ⏮ ⏯ ⏭ buttons plus the track title

Round 21 only adds a plain title tooltip; the previews come in round 22.

## Camera (requested 2026-11-17, round 22)
- Kernel: UVC (uvcvideo) plus V4L2 for laptop front cameras, as modules in both kernels. For newer Intel laptops, IPU6/IPU7 MIPI cameras need libcamera (harder; check later).
- PipeWire camera support (libcamera/v4l2 SPA plugin) and the xdg-desktop-portal Camera interface in flportal, so Firefox, Flatpak apps and video calls (Discord or Meet in the browser) can use it with a permission prompt.
- A small FreeLFS camera app, "flcam": live view, take a photo, record video (GStreamer, already used by flmusic), saved to ~/Pictures and ~/Videos.
- Privacy: a tray indicator while the camera is in use, and an off switch in Settings > Privacy.
- Tests: QEMU with an emulated USB webcam if available (otherwise v4l2loopback as a fake camera).
