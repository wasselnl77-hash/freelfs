# 28. Roadmap: what is built, what is planned, what is open

Summary: this chapter lists the plans in the repository (`docs/ROADMAP-NEXT.md`, `docs/PLAN-installer.md`, `docs/HARDWARE.md`,
`docs/PLAN.md`) and says for each item whether it is built, planned, or not started. Everything marked planned is a plan, not a
promise, and nothing planned has been built unless it says so.

## Contents

1. How to read the status
2. The feature roadmap: phases A to E
3. What is still open in the built features
4. The installer plan (round 19, planned)
5. Translations (12 languages, planned)
6. The Hardware app (planned)
7. The welcome app (planned)
8. The tester website and the community (planned)
9. Hardware support: Intel and AMD (prepared, not tested)
10. The build machine, about mid-January 2027 (planned)
11. Architectures: 32-bit is not planned; aarch64 has not started
12. Security work that is planned
13. The "Later" list and the suggested order

---

## 1. How to read the status

- **Built** means the code is in the tree and a round note says it was built and tested in QEMU or headless. It does not mean it
  was run on real hardware. Most notes say so explicitly.
- **Planned** means written down as a plan, with a decision by the project owner, but not started or not finished.
- **Not verified** means the repository does not say, and nothing here should be read as a claim that it works.

The round notes that record the status are in `docs/ROADMAP-NEXT.md` (per item and per round), `docs/INTEGRATION-TODO.md` (what is
open per round) and `docs/PLAN.md` (the milestones M0 to M10). Chapter 25 explains what the QEMU tests prove.

## 2. The feature roadmap: phases A to E

`docs/ROADMAP-NEXT.md` (planned 2026-11-10 from the user's answers) groups the features in phases. Each item has a size: S (a day of
agent work), M (a few days) or L (a week or more). The status column below is from the round notes of 2026-11-10 to 2026-11-14.

| Item | Feature | Status |
|---|---|---|
| A1 | Lock screen (`fllock`, Super+L) | built, tested in QEMU (round M9); not on hardware |
| A2 | Notifications (cards, centre, do-not-disturb) | built (`flpanel`, `flnotify`), tested in QEMU |
| A3 | Workspaces (grid, pager, overview) | built, in the image (M10); a Settings page for the grid was added on 2026-11-12 |
| A4 | Accent colour (six choices) | built, in the image (M10) |
| A5 | Wallpapers (the six accent variants, imported pictures) | built, in the image (M10) |
| A6 | Battery (percentage, power saving) | built; the battery, lid and CPU governor were **not** tested in QEMU (no battery) |
| A7 | System sounds (`flsound`) | built; "not heard by a person yet: levels and taste are untested" |
| A8 | "oh no." pages in FreeBWS (offline, DNS, TLS, crashed tab) | built in FreeBWS (`freebws/pages.c`, the error page of the README) |
| A9 | "You can't do that." (the easter egg on vital system folders) | built (`flcrash --cant`, hooks in `flfiles` and `flsh`), in the image (M10). Not yet tried on the laptop. It is a maximized window now, so the panel stays visible. |
| A10 | Time zone on a world map | built (`fltime-set`, the map widget in libflui), tested in QEMU (round M9) |
| A11 | Animations and themes (`fltheme`, light scheme) | built, in the image (M10 and later); the light scheme is in `flpalette.h` |
| B1 | Image viewer (`flview`, "Images") | built |
| B2 | Music player (`flmusic`, "Music") | built (round notes: built into the image) |
| B3 | Video player (`flvideo`, "Videos") | built on libmpv, with a file picker through the portal; hardware decoding on a real GPU not verified |
| B4 | PDF viewer (`flpdf`, "Documents") | built |
| B5 | Snappel (screenshot tool) | built; Print saves, Shift+Print region, Super+Shift+S the window; screen recording is "Later" |
| B6 | Archives (`flarchive`) | built |
| B7 | General Manager (task manager, `flgm`) | built |
| B8 | Text editor (`fledit`) | built, with tabs and syntax colours |
| C1 | Software Store (`flstore`) | built: Flathub and FreeLFS packages, installed and updated from the app |
| C2 | Bookmarks bar | built (FreeBWS) |
| C3 | Ad blocker | built (FreeBWS, section 5 of chapter 27); the YouTube remover is a scriptlet |
| C4 | Downloads panel | built (FreeBWS). Pause and resume are not possible with the WPE API |
| C5 | Force dark mode | built (FreeBWS, per site and global) |
| C6 | Search engine choice | built (FreeBWS) |
| D1 | Update notifier (`flupdate`) | built |
| D2 | Snapshots before updates (btrfs, `flsnap`, "Start an earlier state") | built; rollback tested in QEMU (round M10) |
| D3 | Online package repository | **the signed repository is built locally** (`70-repo.sh`, index with serial and expiry, `flpkg sync`). Hosting it publicly is **planned**: `docs/SECURITY.md` speaks of the repo "once it is hosted". The tree has no `config/flpkg/` directory yet. |
| E1 | Installer (`flinstall`, "FreeLFS install") | built; whole-disk install tested in QEMU (M10). **Not tested**: "next to existing partitions" and "Manual" disk modes. Dual boot, encryption and manual partitioning are in the installer plan (section 4). |
| E2 | Gaming (kernel options, udev rules, `flgaming`, performance mode, Xwayland) | built in parts: performance mode, Xwayland (`check-x`), the kernel options. **Not done**: Steam via Flatpak was not tested through the portals; gamepad support is listed as part of E2. |

The "Suggested build order" of the roadmap (A2, then the named apps, then the browser, then the repository, the store and the installer)
was followed in the order the rounds show. The roadmap's own estimate was "~6–8 weeks of agent work at the current pace".

## 3. What is still open in the built features

From the round notes (`docs/ROADMAP-NEXT.md`, `docs/INTEGRATION-TODO.md`, `docs/HARDWARE.md`):

- **Shutdown** on the real laptop was reported broken in round 15 and fixed in round 16; the fix was tested in QEMU only (chapter 25,
  section 9 and 10).
- **Files** did not open images or video on the laptop (round 15 report). The QEMU tests ran on QEMU's own GPU (`-vga std` or virgl), not
  on the real GPU (`docs/bugscan-2026-11-14/tests-audit.md`, section 2.6).
- **USB sticks**: a partitioned stick (`sdX1`), a `dd`-written stick and eject on real hardware: not yet verified.
- **Night light** on a real panel and a fractional scale on real hardware: not yet verified. Its tray switch did not flip under QEMU input
  (round 17 commit notes).
- **Files to desktop drag**: not implemented (`Known gaps` in round 17).
- **FreeBWS** on the real laptop: sign-in with Google and Cloudflare's check were not verified (chapter 27, section 14).
- **Hardware round of 2026-11-12** (kernel 7.3.4 and LTS 6.18.63, firmware, microcode, Intel media driver): built, QEMU only. Nothing
  of it has run on an Intel or AMD machine yet (`docs/HARDWARE.md`).
- **Suspend and resume**: there is no suspend path in the system yet (`src/flcomp/README`, lock screen).
- **Secure Boot**: options only (`docs/SECURITY.md`, section 4); nothing implemented.

## 4. The installer plan (round 19, planned)

`docs/PLAN-installer.md` was written after the user's answers of 2026-11-14 and is planned for "round 19". Today the installer
(`src/flinstall`) has these steps: language, keyboard, Wi-Fi, disk, account, time zone, summary, install.

**Time zone, keyboard and language**

- A world map to pick the time zone, and a city search. The map exists already (A10, the same widget); the planned change is to
  show the region and the city on it. The language still suggests a zone.
- Keyboard layouts: only four are defined today (`fl_kbd_layouts[4]` in `src/flui/kbd.c`: de, us, uk, fr). Planned: all layouts
  of the installer's languages and common variants (US international, Swiss, Austrian, Belgian, Nordic and others), and a test field
  to try é, ü, @ and similar.
- Desktop language, changeable later in Settings, with full translations (section 5). The default is English where a string is missing.

**Disk**

- Dual boot with Windows or Linux: detect other systems, shrink a partition (the NTFS tool's licence and size are to be checked), and add
  flboot menu entries. The riskiest item; test only with disk images in QEMU.
- Disk encryption: a LUKS2 checkbox and a passphrase, unlocked in `flinitrd`. Needs cryptsetup and a prompt in the splash and init.
- Manual partitioning: an advanced screen with partitions, file systems and mount points.
- Swap choice: zram (the default on PCs with 2 GB or less), a swap file, or none. The installer's swap step also sets swappiness (a slider,
  about 100 for zram); both can be changed later in the Hardware app.

**Setup**

- App picks: checkboxes for Firefox, LibreOffice, VLC and others, installed through the Store (Flatpak) after install, or during it when online.
- Automatic login and a hostname field.

**Around the install**

- A hardware check first: warn about little disk space, no internet, a low battery, an unsupported GPU (for example an old Radeon without
  r600) or little RAM.
- An install log and a report: on failure show the log, with a button that saves an flreport to a USB stick.
- Update during install when online, so the first boot is current.
- A welcome app (section 7).

**Order** (from the plan): (1) translation system and keyboard layouts, since everything else uses them; (2) the map, the test field,
hostname and auto-login, swap; (3) the welcome app, the hardware check, the log and report, app picks, update during install;
(4) encryption, manual partitioning, dual boot.

## 5. Translations (12 languages, planned)

`docs/PLAN-installer.md`: full translations of all `fl*` apps, the panel, Settings, Files, the Store, the installer, the greeter and the
others, into the 12 languages the installer already lists: en_US, en_GB, de, fr, es, it, pt_BR, nl, pl, cs, sv and tr. The plan expects
the translations to be prepared with agents, and a small gettext-like catalogue in libflui, without the glibc gettext dependency if that
is lighter. The default is English. **No translation catalogue exists in the source tree yet** (chapter 22, section 16).

## 6. The Hardware app (planned)

`docs/PLAN-installer.md` ("Added 2026-11-14"): a universal FreeLFS app, `src/flhw`, with five pages. It is not in the tree yet.

- **Device overview**: CPU, GPU and the driver in use, RAM, disks, Wi-Fi and Bluetooth chips, battery health. A "Copy info" button for bug
  reports, also included in `flreport`.
- **Drivers and firmware**: missing firmware, or a GPU that runs without acceleration (an old Radeon without r600, an Intel GPU before clang),
  and how to fix it.
- **Power and CPU**: power profiles (saver, balanced, performance, using `flpower saver`), the CPU governor, lid and low-battery actions.
- **Sensors and disk health**: temperatures and fans (hwmon), SMART health of SSDs and HDDs.
- **Memory**: zram size and algorithm, swap file, swappiness.

Privileged writes go through a small setuid helper limited to the `seat` group, with `flauth` asking for the password (as `flmount` and
`flpower` do; the users and security chapter, 15, covers the groups).

## 7. The welcome app (planned)

First-login app, which can be opened again from the menu: "New? Click here" (a short tour of the Store, Settings, updates and shortcuts),
"Settings", "We need testers! (click here to apply)" (the target is still to be chosen: GitHub issues or discussions), and later "Support".
Not in the tree.

## 8. The tester website and the community (planned, partly built)

- The public pages are in the repository: `site/index.html`, `site/download.html`, `site/testers.html` and `site/docs.html`. The tester
  page says what to test and how to report (run `flreport`, open a GitHub issue, use the report template).
- **Planned**: publish the site on GitHub Pages, and later on its own domain (`docs/PLAN-installer.md`). Whether the pages are published
  today: not verified.
- **Planned**: "help wanted" and "good first issue" labels on FreeBWS issues, and `docs/CONTRIBUTING-freebws.md` (chapter 26, section 11).
- FreeBWS stays a community project inside the main repository. Firefox (Flatpak) is the recommended browser.
- App names: the real name big in the menu, with the generic name small under it (`GenericName=`, planned for every `.desktop` file;
  chapter 24, section 5).

## 9. Hardware support: Intel and AMD (prepared, not tested)

`docs/HARDWARE.md` (the status table):

| Hardware | Status |
|---|---|
| AMD Ryzen 5 7520U laptop (Radeon 610M, rtw89 Wi-Fi) | tested partly, in earlier rounds; the M10 run on the laptop failed and was fixed; not retested after the kernel change |
| Other AMD | prepared, untested on real hardware |
| Intel | prepared, untested on real hardware; **needs testers** |
| NVIDIA | nouveau and NVK planned; the proprietary driver later. Today the machine runs on the firmware framebuffer (simpledrm) or on its integrated GPU. |
| QEMU and virtio-gpu | tested in every round |

The 2026-11-12 hardware round (kernel 7.3.4 release 2, linux-lts 6.18.63 release 2, linux-firmware, sof-firmware, microcode, gmmlib,
intel-media-driver, Mesa release 3 without the Intel 3D drivers, backlight preference) was built on 2026-11-13 and run in QEMU only.

**Deferred to a 16 GB build machine: Mesa's Intel 3D drivers.** iris, crocus, anv and hasvk need clang, spirv-llvm-translator and libclc
at build time, and linking clang needs about 8 GB of RAM. The recipes `llvm` (release 2, with clang, described in its header),
`spirv-llvm-translator` and `libclc` (group `deferred`) are ready but not built. Until then an Intel GPU gets kernel modesetting (i915 or
xe), software OpenGL (llvmpipe) and lavapipe, and hardware video decoding through `intel-media-driver`.

The wanted testers (`docs/HARDWARE.md`): an Intel laptop with VMD or RST NVMe, one with SOF audio, one with Wi-Fi 7 (BE200), an AMD laptop
with MT7922 or MT7925 Wi-Fi, and a Ryzen 7040 or 8040 (Phoenix) machine such as the planned NucBox M6 Ultra.

## 10. The build machine, about mid-January 2027 (planned)

The plan is a new build machine with 16 GB of RAM, a NucBox M6 Ultra, expected around mid-January 2027. `docs/HARDWARE.md`
gives the date as "about January 2027" and the 16 GB, and names the NucBox M6 Ultra as a planned Phoenix-class machine for
testers. The owner's plan is to use it as the build machine. It is planned, not bought or set up in the tree.

When it arrives, the job limits of the build (`FLFS_JOBS`, chapter 20, section 3) are to be raised, and the Intel 3D drivers
(section 9) can be built there, because linking clang needs about 8 GB.

## 11. Architectures: 32-bit is not planned; aarch64 has not started

- **32-bit systems are not planned.** The download page says so (`site/download.html`: "32-bit systems are not planned").
- **aarch64**: `docs/PLAN.md` (milestone M10) says "aarch64 port (not started)" and "x86_64 only for now. Nothing has ever been built for
  aarch64". The C code is meant to stay portable, and the plan calls the port mostly distribution work (recipes and the boot loader).

## 12. Security work that is planned

From `docs/SECURITY.md` and the round notes:

- The signed repository: publishing it, and re-signing at least every 10 days once it is hosted (the index expires after 14 days by default,
  `FLFS_REPO_VALID_DAYS`).
- A second signing key, introduced with a transition period: the mechanism exists (`flpkg key introduce`), and its use is planned.
- A CVE cadence: `build/cve-check.py` weekly, and before each release; WebKit security releases within 14 days, or 7 days for an exploited
  advisory (proposal in `docs/SECURITY.md`; not yet a written process of the project).
- Secure Boot: options only.
- Suspend and resume, and a lock on suspend (chapter 23, section 9).

## 13. The "Later" list and the suggested order

`docs/ROADMAP-NEXT.md` "Later (decided later)":

- Bluetooth (BlueZ, the panel flyout, pairing in Settings): **built in round 17**. Pairing uses a default agent for one-shot pairing.
- Printers (CUPS, driverless IPP Everywhere first): **built in round 17**, started on demand through `flsocketd`.
- Screen recording in Snappel: planned.
- Light theme: the roadmap said "not asked for yet"; a light scheme has been built since round 13 (`fltheme`, `flpalette.h`).

The suggested order in the roadmap was: A2 notifications, the named apps (Snappel, General Manager, editor), the image viewer, PDF, video and
archives, the ad blocker and the browser features, the repository, the notifier and the snapshots, the Software Store, then the installer
and gaming. The rounds followed it roughly. The next open items are the ones in section 3 and the installer plan (section 4).
