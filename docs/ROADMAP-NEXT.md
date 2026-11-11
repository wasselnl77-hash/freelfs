# FreeLFS: the next features (plan)

Status (updated 2026-11-11): written as a plan on 2026-11-10 from the
user's answers; since then every item of phases A to E is built and, apart from
the "Later" list, built into the image and exercised in QEMU (not on the real laptop). The status lines below say
what is open per item; docs/INTEGRATION-TODO.md is the short list. Original plan text:
user's answers to the feature questionnaire. Order is by phase; inside a phase
items are independent and can be built in parallel (separate agents, separate
files). Size: S = a day of agent work, M = a few days, L = a week or more.

Ground rules for every item (same as M7/M8):
- FreeLFS look: flat, palette bg 1d2125 / panel 272c33 / raised 343a43 /
  line 3a4048 / text d8dce0 / dim 8a9199 / accent (user-selectable, default
  3f9b63), IBM Plex, sharp corners (decided: no rounded windows). No
  "AI slop" look.
- Low end first: every app idles under ~25 MB RSS where possible; the whole
  desktop must still work on 1 GB RAM.
- Own C apps on libflui where reasonable; reuse proven libraries (GStreamer,
  libarchive, mupdf, mpv) instead of writing codecs/parsers.
- Every app gets headless screenshots and a QEMU test step before it ships.

---------------------------------------------------------------------------

## Phase A: desktop basics (mostly S, all in flcomp/flpanel/flui)

| # | Feature | Decision | Notes | Size |
|---|---|---|---|---|
| A1 | Lock screen `fllock` | simple: clock + password | Super+L (shortcut already waits for /usr/bin/fllock), lock on suspend/lid; compositor-side lock surface so nothing leaks; reuse flgreet's password check | S |
| A2 | Notifications | cards + notification center with history | Implement org.freedesktop.Notifications in flpanel (session D-Bus is already running), cards top right, history flyout from the clock; "do not disturb" toggle | M |
| A3 | Workspaces | openSUSE/KDE style: as many as you like | grid of virtual desktops, pager in the panel, Super+Ctrl+arrows to switch, Super+Shift+arrows to move a window, overview on Super+Tab | M |
| A4 | Accent colour | ~6 choices in Settings > General | flui reads `accent=` from desktop.conf; flcomp, panel, apps live-reload; crash screen keeps its own green | S |
| A5 | Wallpapers | own image + 3D-F colour variants | Settings > Display > Wallpaper; flcomp already loads .flw; add PNG/JPEG import (scaled + converted once); flsplash genart renders the 3D F in each accent colour | S |
| A6 | Battery | percentage + power-saving mode | panel icon with %, flyout with time left; power-saving = lower CPU governor, dim screen, 30 fps cap in flcomp; warning notification at 10 % / 5 % (needs A2) | S |
| A7 | System sounds | yes, subtle | login, notification, error, volume-tick, screenshot click; one tiny sound player via PipeWire; off switch in Settings; sounds made from scratch (own, license-clean) | S |
| A8 | "oh no." siblings | yes | FreeBWS offline / DNS / TLS-error / crashed-tab pages in the crash-screen style (stretched "oh no.", emoticon, plain explanation, Retry key) | S |
| A9 | Easter egg | "You can't do that." | see below | S |
| A10 | Time zone with world map | click on a map to choose; change anytime | Settings > Date & time: flat world map (own, generated from public-domain Natural Earth outlines, FreeLFS colours), click a spot → nearest city/zone from tzdata's zone1970.tab coordinates, highlighted zone band, search field as alternative, 24h/12h, automatic time (NTP). Writes /etc/localtime; panel clock updates live. The same map widget is reused in the installer (E1) | M |

Not doing (decided): night-light timer, dual boot with Windows, calculator
app. Light theme: question unanswered, not planned for now.

Status 2026-11-10: **A1 lock screen, A2 notifications and A6 battery are built** and tested
headless (make check in src/flcomp, src/flpanel, src/fllock; screenshots docs/m7/lock-*.png,
notify-*.png, battery-*.png). Not tried on hardware: the lid switch, the real backlight and CPU
governor writes of power saving, `time left` from a real battery, the lock on a real display.
There is no suspend path yet, so "lock on suspend" is covered by locking on the lid only.

QEMU, 2026-11-10 (`build/m9-features.sh`, 2 GB and 1 GB, screenshots docs/m7/m9-*.png): **A1, A2 and A10 verified in the
real image**, keyboard and an absolute pointer only. `flnotify "Test" "Hello"` shows the card top right and the history
in the notification centre; Super+L locks (fllock runs, nothing else is drawn), a wrong password says so, the right one
(set for the throwaway test account only) unlocks and fllock exits; `fllock-check` answers 1 for a wrong and 0 for the
right password as the seat user; `flsettings --page datetime` opens the page; a click on Tokyo on the map runs
`fltime-set zone`, /etc/localtime and /etc/timezone change, the panel clock moves to JST at once (tz_changed); refusals
of `fltime-set` (`../../etc/passwd`, `posix/UTC`, a time while automatic time is on or out of range) exit 3 or 4;
`fl-sntp` reaches pool.ntp.org through QEMU's user networking (UDP 123 works), the ntp service steps the clock back
after `fltime-set ntp off`, a hand-set clock 2 h off and `ntp on` (offset 7200 s corrected within seconds). The setuid
entries are right in the image (root:seat 4750: flpower, fltime-set, fllock-check, flpkg-seat; fusermount3 root:root
4755). Not covered by QEMU: A6 (no battery), the lid switch, real hardware clock writes by `hwclock --systohc`.

### A9 Easter egg detail
When someone tries to delete a vital system folder (`/sysflfs`, `/sysflfs/bin`,
`/sysflfs/lib`, `/sysflfs/settings`, `/boot`, and the classic hidden links):
- show the crash screen layout with the stretched "oh no.", the emoticon
  `>:(` instead of `:/`, and only the text "You can't do that."
- two keys/buttons: "Never mind" (default, cancels) and "Continue", which
  really deletes it anyway (user's wish).
- Hooks: flfiles delete/trash on those paths; flsh: a check before `rm`
  with `-r` on those paths (interactive shells only, so scripts are not
  affected). Coreutils `rm` itself stays unmodified.
- Works as a full-screen layer-shell surface when the desktop runs, falls
  back to the text version on a console.

**A4, A5, A7, A9 status (2026-11-11): built, tested headless; A4, A5, A7 and A9 were built into the image and exercised in QEMU in the M10 round (accent change repaints live, the wallpaper changes, all five sounds run without audio hardware); not on hardware.**
- A4 accent: Settings > General has six swatches (green, teal, blue, amber, orange, rose), written as `accent=`;
  libflui derives accent, text-on-accent and selection fill (dark and light shades) and every app that watches
  desktop.conf redraws live (`src/flsettings/tests/accent.py`, headless flcomp: click a swatch, Settings turns blue at
  once, the compositor's wallpaper follows). flcomp reads the key too (new `theme.c`: volume indicator, Alt-Tab
  frame, lock caret). The crash screen keeps its green. flterm still has its own green selection colour.
- A5 wallpapers: flsplash genart renders the 3D F in the other five accent colours (`-W -a RRGGBB`, green is
  byte-identical to before); Settings > Display > Wallpaper has "Match the accent colour", tiles for the six variants
  and imported pictures, a path field and `flsettings --set-wallpaper FILE` (PNG/JPEG via libpng/libjpeg, scaled once
  to fit 2560x1440, saved as .flw under ~/.local/share/freelfs/wallpapers); flcomp swaps the wallpaper live. There is
  no file chooser yet: the path is typed or pasted (the image viewer, B1, can call `--set-wallpaper`).
- A7 sounds: new `src/flsound` (recipe flsound): five sounds synthesized in the program (no audio files), played as one
  PipeWire stream; `sounds=0` and Settings > General > System sounds switch it off. Played by flcomp (login, volume
  keys, screenshot saved/failed), flpanel (notification card, volume slider), flcrash (the egg). Played once through
  the host's PipeWire (exit 0); `make check` checks the waveforms. Not heard by a person yet: levels and taste
  are untested.
- A9 egg: `flcrash --cant PATH...` (exit 1 = Never mind, 0 = go ahead), full-screen window on the desktop, text on a
  console; hooks in flfiles (Delete, Move to Trash) and an interactive flsh before `rm -r`. `src/flcrash/tests/egg.py`,
  `src/flfiles/tests/egg.py`; pictures docs/m7/egg-*.png, accent-*.png, wallpaper-*.png. The window was a fullscreen
  xdg toplevel, not a layer-shell surface (flcomp has none), and flcomp hides the panel while a fullscreen window is on
  top: the egg covered the panel. Now it is a maximized window (the work area): the panel stays visible and on top
  (round of 2026-11-11, QEMU picture docs/m7/egg-below-panel.png). Open: the egg on the laptop.

---------------------------------------------------------------------------

## Phase B: apps (each is its own src/<app>, own recipe, group desktop)

| # | App | Engine / library | Must-haves | Size |
|---|---|---|---|---|
| B1 | Image viewer | libpng, libjpeg-turbo, libwebp, gdk-pixbuf (exists) | open folder, arrow keys next/prev, zoom/fit, rotate, delete, set as wallpaper, fast thumbnails strip | S |
| B2 | Music player | GStreamer (exists) | library from ~/music, albums/artists, queue, MPRIS so media keys + panel work, small player in the panel flyout | M |
| B3 | Video player | mpv (libmpv render API into a flui window) + ffmpeg | play/pause/seek, subtitles, audio track, fullscreen, remember position; hardware decoding via VA-API where the GPU has it | M |
| B4 | PDF viewer | mupdf (light, fast) | pages, zoom, search, table of contents, print later | S |
| B5 | **Snappel** (screenshot tool) | own, uses a new flcomp screencopy protocol | full screen / window / region / timer; annotate (arrow, box, text, highlight, blur/pixelate, crop); copy to clipboard, save, "open in"; Print = full, Shift+Print = region, Super+Shift+S = Snappel window; screen recording later | M |
| B6 | Archive tool | libarchive (exists) | open/extract zip, tar.*, 7z, rar (read); create zip/tar.zst; integrates with flfiles right-click "Extract here" | S |
| B7 | **General Manager** (task manager) | /proc, own | Windows-Task-Manager-like layout: Processes (grouped by app, CPU/RAM/disk/net columns, End task), Performance (CPU/RAM/disk/net/GPU graphs, flat FreeLFS style), Startup apps, Users, Details; opens with Ctrl+Shift+Esc; must look really good | M |
| B8 | fledit upgrade | own | tabs, syntax colours (C, sh, Python, JS, HTML/CSS, DSSL, Markdown, conf), line numbers, find/replace, go-to-line, auto-indent, remember open files | M |

Status 2026-11-11 (rootfs owner round): **B1 image viewer (flview, "Images"), B4 PDF viewer (flpdf,
"Documents"), B2 music player (flmusic, "Music") and B3 video player (flvideo, "Videos") are built**
as recipes into the rootfs and tested headless (screenshots in docs/m7/view-*, pdf-*, music-*, video-*;
each src/<app>/tests/run.sh passes in the chroot). New library recipes: mupdf, libogg, libvorbis, opus
(gst-plugins-base now builds ogg, vorbis, opus), libva, fribidi, libass, libplacebo (no GPU backends),
libdisplay-info, mpv (libmpv + player, no window system code); ffmpeg release 2 adds the ffmpeg/ffprobe
tools and VA-API. Details and what is not done: the README in each src/<app>/. Open for integration:
the compositor's XF86AudioPlay/Next/Prev keys can run `flmusic --ctl play-pause|next|previous` (MPRIS);
the panel flyout can talk MPRIS to org.mpris.MediaPlayer2.flmusic; All of that was wired in the M10 round (media keys run `flmusic --ctl`, flfiles Enter opens by MIME type,
"Set as wallpaper" calls `flsettings --set-wallpaper`) and the four apps ran in the image in QEMU (flac, m4a, wav and
an mpeg4+aac mp4; mp3 and the flvideo file picker were added on 2026-11-11, see below). Open: VA-API decoding is
compiled in but untested (no GPU here), real audio output.

**B7 status (2026-11-11):** General Manager built (`src/flgm`, binary `general-manager`, recipe `recipes/flgm`): Processes (grouped, heat-tinted, sort, search, End task/Delete), Performance (CPU incl. per-core, memory, disk, network, GPU via sysfs), Startup apps (enable/disable), Users, Details. Ctrl+Shift+Esc added in `src/flcomp/seat.c`. Tested host-side: `make check` (data layer), `tests/e2e.sh` (headless flcomp, key press starts it, search + Delete ends a victim), screenshots `docs/m7/gm-*.png`. Built into the image in the M10 round: in QEMU Ctrl+Shift+Esc opens it (Processes, Performance, Startup apps), and the session starts the autostart entries it edits (the Startup tab and the session agree). Not on hardware. See src/flgm/README.

---------------------------------------------------------------------------

## Phase C: Software Store + FreeBWS features

| # | Feature | Decision | Notes | Size |
|---|---|---|---|---|
| C1 | **Software Store** | replaces the Packages app | one store for FreeLFS packages + Flathub: home page with categories and featured apps, app pages with screenshots/description (appstream data), install/update/remove, "Updates" tab (ties to D1); per-user Flatpak, flpkg via the existing seat helper | L |
| C2 | Bookmarks bar | yes | Ctrl+D, bar under the address bar, folders, import from Firefox/Chromium HTML export | S |
| C3 | Ad blocker | on by default, block EVERYTHING incl. YouTube ads | WebKit content filters (WebKitUserContentFilterStore) compiled from EasyList + EasyPrivacy + uBlock filters; cosmetic filtering; scriptlet injection for YouTube and similar sites; lists update weekly; per-site off switch. Honest note: YouTube changes its ads often, so this needs list updates to keep working | M |
| C4 | Downloads | small panel in the browser | progress, pause/resume, open/show in folder, notification when done (A2) | S |
| C5 | Force dark mode | yes | per-site toggle + global setting; prefers-color-scheme first, then a dark-reader-style filter for pages without a dark mode | S |
| C6 | Search engine | default Brave Search; choose DuckDuckGo, Google, Startpage, others | setting in FreeBWS; start page and address bar follow it | S |

---------------------------------------------------------------------------

## Phase D: system safety and updates

| # | Feature | Decision | Notes | Size |
|---|---|---|---|---|
| D1 | Update notifier | notice only, no automatic install | daily check against the online repo (needs D3) and Flathub; notification + Store "Updates" tab | S |
| D2 | Snapshots before updates | automatic | flpkg takes a btrfs snapshot of @ before every transaction (layout already has @snapshots), keeps the last N; flboot menu entry "Start an earlier state"; rollback tool in Settings | M |
| D3 | Online package repository | (from M9) | signed repo index, hosted for free (e.g. static files on a CDN/HF), `flpkg sync/upgrade` | M |

Status 2026-11-10 (D2): **built; the real btrfs run was done in the M10 round (build/m10-snapshots.sh: create/list/show, "Start an earlier state", read-only boot, rollback), see the 2026-11-11 notes below.** Original text: `src/flsnap` (create/list/show/status/delete/prune/rollback,
read-only snapshots of @ in @snapshots, `.info` with date + reason + package list, keep=8 in /etc/flsnap.conf),
flpkg runs `/usr/libexec/flpkg/pre-transaction.d/*` (hook 10-flsnap), flboot has "Start an earlier state" with a
notice, flsettings has a Snapshots page (src/flsettings/snapshots.c), `flsnap-notice` shows the desktop notice with
"Make this the current state". Verified on a mock backend (tests/test-flsnap.sh), flpkg hook tests, flboot menu
rendering headless (docs/m7/snap-*.png). Not run: the real btrfs commands, booting a snapshot, the setuid install,
clicking the Settings buttons. (Done in the M10 round: the package is built, the session starts `flsnap-notice`, the QEMU run on the btrfs image passed.)

Status 2026-11-10, D3: **built, tested on a scratch root** (src/flpkg/tests/test-repo.sh, file:// and a local http server). Signed minisign-format index and packages, `sync`, `upgrade`, `search` already existed; added `flpkg check-updates` (machine-readable, no root), curl timeouts, `build/70-repo.sh` (static repo dir in ~/freelfs-work/repo/<arch>, key in ~/freelfs-work/repo-key, public key to config/flpkg/keys), publishing notes in docs/flpkg.md. Nothing uploaded; no hosting URL chosen yet. Not covered: replay of an old signed index (no expiry in the index).
Status 2026-11-10, D1: **built** (src/flupdate, `make check` with fake flpkg/flatpak/flnotify/flstore; started from src/flcomp/session). Built into the image in the M10 round: the session starts `flupdate-notify`, flpkg `check-updates` exists and `flstore --updates` opens the Updates tab.

---------------------------------------------------------------------------

## Phase E: installer and gaming

| # | Feature | Decision | Notes | Size |
|---|---|---|---|---|
| E1 | **Installer** + "FreeLFS install" icon on the live desktop | does everything for you, but manual partitioning possible | Steps: language, keyboard (QWERTZ/QWERTY/…, moved here from first boot), Wi-Fi, disk: "Use the whole disk" (default) / "Install next to existing partitions" (use free space, no wiping) / "Manual" (partition editor), user + password (refuse names that clash with /usr links), time zone (click on the world map from A10), browser choice (FreeBWS stays; Firefox optional), summary, install, reboot. Installs the btrfs layout (@, @home at /usr, @snapshots). No Windows dual-boot support (decided) | L |
| E2 | Gaming | yes | Steam via Flatpak (Store featured), gamepad support (kernel: hid-xpad/xbox, playstation, nintendo, uinput; udev rules), Vulkan already in; gamemode-like "performance mode" in flcomp (unredirect fullscreen, no compositing delay); controller works in Big Picture | M |

**E2 status (2026-11-11):** kernel config, gaming-config recipe (udev, sysctl, `flgaming`), performance mode in flcomp (perf.c) with a panel switch in the battery flyout: written, built into the image (M10 round; /dev/uinput, /dev/uhid, /dev/ntsync exist in QEMU, `flgaming check` runs). Xwayland has been added since (see the E2 Xwayland status); Steam itself is untested. Mesa lacks anv/iris (needs clang, spirv-llvm-translator, libclc). Details and untested list: docs/E2-GAMING.md.

**E1 status (2026-11-11):** installer built: `src/flinstall` (window `flinstall`, title "FreeLFS install", + setuid helper `flinstall-helper`), recipe `recipes/flinstall`, start-menu entry that only exists on the live system (`TryExec=/run/live`). It had no desktop icon, because there was no desktop-icon layer; on 2026-11-11 flcomp got one (fl_shell 6 `set_desktop`, drawn by flpanel's deskicons.c from .desktop files with `X-FreeLFS-Desktop-Icon=true`): "FreeLFS install" is now an icon on the live desktop too (docs/m7/live-desktop-icon.png). Steps: language, keyboard, Wi-Fi (flsettings' wifi.c), disk (whole / next to existing partitions / manual editor), account (refuses the /usr link names and system names), time zone (libflui world map), summary with a check by the helper, install with progress, restart. Browser choice is not in it (FreeBWS is simply installed). The first-boot keyboard step (`flsetup`) is untouched: the installer writes /etc/keyboard.conf, so installed systems never ask. Tested host-side without privileges: helper in `--image` mode on sparse files (`tests/image-test.sh`: whole disk, free space next to two old partitions, manual ESP/root/swap/ext4, refusals; results read back with `btrfs restore` and mtools), window in headless flcomp (`tests/install.py`, dry-run helper on a fixture of fake disks), screenshots `docs/m7/install-*.png`. **Not tested (needs root):** the real-disk path (sfdisk/mkfs on `/dev/...` partition nodes, `udevadm settle`, kernel re-reading the table, `mount` in a plain mount namespace without user namespace), the setuid helper on the live ISO, booting the result, restart. Needs a change in `build/53-iso.sh` (one line, done): the ISO now carries `live/vmlinuz-<ver>`, which the installer copies to the ESP. Root gets the same password as the first user (decision, say if not wanted). See src/flinstall/README.

---------------------------------------------------------------------------

## Later (decided "later")
- Bluetooth (BlueZ + panel flyout + pairing in Settings)
- Printers (CUPS + driverless IPP Everywhere first)
- Screen recording in Snappel
- Light theme (not asked for yet)

## Suggested build order
1. Phase A (fast, visible, small) — A2 notifications first, A6 battery and
   A8/A9 build on it.
2. B5 Snappel, B7 General Manager, B8 fledit (the user's named apps), then
   B1–B4, B6.
3. C3 ad blocker + C6 search + C2/C4/C5 (FreeBWS becomes a daily browser).
4. D3 repo → D1 notifier → D2 snapshots.
5. C1 Software Store (needs D3 + Flatpak work done).
6. E1 installer, E2 gaming.

Rough total: ~6–8 weeks of agent work at the current pace; each phase ends
with a new live ISO for a stick test.

**Portals status (2026-11-11):** `src/flportal` (recipe `flportal`) is the xdg-desktop-portal backend for Flatpak apps: FileChooser (open, several files, folders, save, save several, filters, choices), AppChooser (OpenURI "Open with"), Access (Allow/Deny), Notification (-> flpanel's org.freedesktop.Notifications, actions come back), Settings (color-scheme from theme=, accent-color from accent=, live SettingChanged), Screenshot (runs `snappel --full|--region --output FILE`, response 2 when snappel is missing). `make check` / `tests/portal.sh` runs it behind the real xdg-desktop-portal 1.22.1 on a private bus, headless; pictures docs/m7/portal-*.png. Not done: PickColor, Inhibit, Background, Account, Email, Wallpaper, ScreenCast, GlobalShortcuts (no backend: those portals are simply absent); dialogs are not tied to the app's window (no xdg-foreign, parent_window is ignored). In the image since the M10 round: in QEMU the frontend, flportal and the document/permission services start by bus activation, FileChooser version 4 and the Settings portal answer, and flvideo's Open dialog (2026-11-11) goes through it. Not tried with a real Flatpak app yet.

**A3 workspaces and B5 Snappel status (2026-11-11):** built, tested headless; both ran in the image in QEMU in the M10 round (Super+Ctrl+Right switches, Super+Tab overview, Print saves a picture and shows the notice). **A3:** flcomp has a workspace grid (desktop.conf `workspaces=2x2`, 1..8 x 1..8, live), Super+Ctrl+arrows switch, Super+Shift+arrows move the focused window and follow it, Super+Tab shows the overview (tiles with window pictures, click to go); fl_shell is now v5 (events `workspaces`, `window_workspace`; requests `set_workspace`, `move_window`, `toggle_overview`; v1-v4 clients unchanged); the panel has a pager (click, wheel, right click for the overview). `make check-ws` in src/flcomp, `tests/pager.sh` in `make check` of src/flpanel; screenshots docs/m7/ws-*.png. **B5:** new fl_screencopy protocol in flcomp (screencopy.c), `src/snappel` (recipe `snappel`): full screen / window (click) / region (frozen overlay, Save, Copy, Edit) / delay 3, 5, 10 s; editor with arrow, box, text, highlight, blur, pixelate, crop, undo; copy as image/png on the clipboard, save to ~/pictures/screenshots, open in; Print = `snappel --full` (notification), Shift+Print = `--region`, Super+Shift+S = the window; without snappel Print falls back to flcomp's own shot.c; `--output FILE` for flportal. `make check` in src/snappel (tools + a headless flcomp run), screenshots docs/m7/snappel-*.png. Untested: on real hardware, a real clipboard consumer other than shmclient, HiDPI scale above 100 % (pictures are at the logical size), the notification click (flnotify -w) with the real panel.

C1 status (2026-11-11): built as src/flstore + recipes/flstore (Software Store: home with featured apps, categories, search, app pages with icon/screenshots/description/size/licence from Flathub appstream, install/update/remove with progress and queue, Installed and Updates tabs for Flatpak and flpkg, FreeLFS packages as a source). Packages app hidden and out of the desktop group, kept as provider of flpkg-seat. Tested headless against real Flathub data; built into the image in the M10 round and opened in QEMU at 2 GB and 1 GB with live Flathub data. On the live ISO it no longer touches Flathub: it says that apps can be installed after installing FreeLFS (see below). See src/flstore/README.

**B8 fledit and B6 Archives status (2026-11-11):** built, tested headless; built into the image in the M10 round, where fledit with syntax colours and Archives (extract here, the window on a zip) ran in QEMU. **B8:** `src/fledit` (release 2): tabs (dirty dot, middle-click close, Ctrl+T/W/Tab, Alt+1..9), syntax colours in the FreeLFS palette for C/C++, shell, Python, JS/TS/JSON, HTML (with script and style parts), CSS, DSSL, Markdown, conf/ini/desktop/yaml (syntax.c, one line at a time with a 2-byte state per line, so only visible lines cost anything), line numbers, find and replace with case and regex toggles (POSIX ERE, groups in the replacement, match count, replace all is one undo), go to line Ctrl+G (`line` or `line:col`), auto-indent (C-like, Python, shell, Markdown lists, tabs or spaces detected), bracket matching, open files and cursor positions in $XDG_STATE_HOME/freelfs/fledit/session. 5 MB file: about 24 MB resident, jump to end 0.2 s, lexing 5 MB 70 ms. `make check` (lexers, no display) and `tests/fledit_test.py` (real editor in headless flcomp), screenshots `docs/m7/fledit-*.png`. **B6:** new `src/flarchive` (recipe `flarchive`, title "Archives"): browse and extract zip, tar.* (gz bz2 xz zst lz4 lzma), 7z, rar (read), single .gz etc.; create zip and tar.zst; the work runs in a child process with progress and Cancel; extraction never overwrites (hidden temp folder, free name, `..` skipped), password protected archives are refused with a message; `flarchive --extract-here` and `--compress` for the file manager. flfiles got "Extract here", "Compress" and archives open in Archives (small additive change in src/flfiles/flfiles.c). `tests/flarchive_test.py`, screenshots `docs/m7/archive-*.png`. The RAR test uses a hand-made RAR5 file (no rar tool here). Not done: archive passwords, editing an existing archive, drag and drop, 7z/rar creation.

**M10 integration round (2026-11-11, rootfs owner): everything above is built into the image and was exercised in QEMU.** Packages built (one group at a time, `FLFS_JOBS=2`): flpkg (the build tool in `.bootstrap/pkg/flpkg-src` was refreshed too: its stamp `p.0-flpkg` was removed, otherwise the image database would come from the old flpkg), flui, flsound, flsh, flsnap, flboot, gaming-config, flcrash; flcomp, flpanel, flsettings, flfiles, flarchive, flgm, snappel, fledit, flview, flpdf, flmusic, flvideo, flstore, flpackages, flupdate, flportal, flinstall, flgreet, fllock, flterm, flsetup, freebws (libflui is a shared library whose header grew, so every program on it was rebuilt, releases bumped); `linux` release 6 (50 minutes at 2 jobs; xpad and the HID pad drivers are modules, uinput/uhid/ntsync built in); mpv release 2 (no menu entry). Releases bumped where the agents had not: flcomp 9, flview 2, flgreet 5, fllock 2, flterm 2, flsetup 2, flmusic 2, flpdf 2, flvideo 2, freebws 8, mpv 2. Glue fixes: flview "Set as wallpaper" runs `flsettings --set-wallpaper`; flcomp media keys (Play/Stop/Next/Prev) run `flmusic --ctl`; flfiles Enter opens by MIME type (globs2 + mimeapps.list, defaults shipped by flfiles); flstore uses `flpkg check-updates` and `--updates` opens the Updates tab; the session starts flsnap-notice and the XDG autostart entries (user dir over /etc/xdg/autostart, General Manager's Hidden/override files honoured); Ctrl+Shift+Esc and the media keys are in the flcomp README; the installer's root-password rule is one switch (`FI_ROOT_SAME_PASSWORD` in src/flinstall/common.h). **Bug found by the real install and fixed:** flinstall-helper (setuid) kept the caller's real uid, so the shell script `flboot-install` (sh drops the effective uid) failed with "Permission denied" in the root-only work directory; the helper now calls setuid(0) after its option checks.

Verified in QEMU (OVMF, KVM) at 2 GB and 1 GB unless noted: `build/boot-test.py` (2 G, 1 G) passes; `build/m9-features.sh` (2 G, 1 G) passes (ad blocker first start at 1 GB: lowest MemAvailable 427 MB, no OOM); `build/m8-freebws.sh` (2 G, 1 G, numbers in the logs); new `build/m10-features.sh` (+ `m10-samples.py`, `m10-features.cmds`, `m10-features.tty`; pictures docs/m7/m10-*-2G.png and -1G.png): login, autostart on/off, workspaces (Super+Ctrl+Right, overview Super+Tab), Snappel on Print (file saved, notice), General Manager on Ctrl+Shift+Esc (Processes, Performance, Startup apps), accent change repaints live (and the settings portal reports it), five system sounds without an audio device (no crash), fledit with syntax colours, Archives (extract-here and the window on a zip), Images + Set as wallpaper (the desktop changes), Documents (PDF), Music (flac/m4a queue, `flmusic --ctl` and the audioplay key: Stopped, Playing, Paused), Videos (mpeg4+aac mp4 plays with the sw render path, 41 MB RSS), Files opening png/pdf/m4a/txt in the right app, Software Store with live Flathub data and `flstore --updates`, portals (frontend, flportal, FileChooser v4, Settings), gaming devices and `flgaming check`, flsnap on the real btrfs. New `build/m10-snapshots.sh` (three boots of a writable image copy): snapshot create/list/show, flboot "Start an earlier state" (menu pictures), snapshot 1 boots read-only (`touch` fails with read-only file system) with the old marker file and the desktop, General Manager and the "You are in an earlier state" notice work on it, `flsnap rollback` makes it current, the default entry then shows the old state, the state left is snapshot #3. New `build/m10-install.py`: live ISO + blank 16 GiB virtual disk file, the window is clicked through (language, keyboard, Wi-Fi, disk "whole disk", account anna, Europe/Berlin, summary, install), then the installed disk boots alone: login screen with the account, desktop after the password, btrfs @ @home @snapshots, ESP with flboot + kernel + flboot.conf, snapshots = 5, root has the account's password, `flsnap create` works. The live ISO at the normal command line (838 MB, level 15) boots to the keyboard question, login and desktop at 1 GB (screenshots; its serial console is silent because `console=tty0` comes last, so `iso-test.py` ran on a twin built with `--cmdline console=ttyS0,115200` from the same root: PASS at 1 GB).

Not done / untested after that round (later rounds closed several: Xwayland came with flcomp 10/11 and the full flcomp `make check` passes in the chroot since release 11; the flvideo file picker, the mp3 sample and the egg above the panel were done on 2026-11-11, see below): Steam and a real Flatpak app through the portals, real audio output and hardware decoding, "next to existing partitions" and "Manual" disk modes of the installer on a real disk and the Wi-Fi step, the laptop, Secure Boot. Cosmetic, fixed on 2026-11-11: the snapshot made by `flsnap rollback` ("before going back to #N") listed 0 packages. `src/flpkg/tests/test-txn.sh` passes completely in the build chroot (its two host-side failures are an environment effect, not reproduced).

**E2 Xwayland status (2026-11-11):** built, tested headless in the build chroot. Recipes: xwayland 24.1.15 and the X libraries (xorgproto, xcb-proto, xtrans, libXau, libXdmcp, libxcb, libX11, libXext, libXfixes, libXrender, libXcomposite, libXdamage, libXi, libXrandr, libXcursor, libXtst, libXxf86vm, libxkbfile, libfontenc, libXfont2, libxshmfence, libxcvt, xkbcomp, pixman, xcb-util, -wm, -keysyms, -renderutil, -image); mesa release 2 (platforms wayland+x11, glx=dri: Xwayland's GLX links libGL); sources.list/lock updated. flcomp release 10: new xwm.c (Xwayland on the first X connection, ends 3 s after the last window, xwayland_shell_v1, X window manager: managed windows are normal windows, unmanaged menus, EWMH/ICCCM, focus, stacking, _NET_WM_MOVERESIZE), xwsel.c (CLIPBOARD both ways, INCR), xwdeco.c (frame for undecorated X windows), small hooks in xdg.c, surface.c, render.c, seat.c, main.c, perf.c, comp.h; `make check-x` (also in `make check`; skips without Xwayland), tests/xclient.c. flgaming check has an X11 section. Details and limits (no glamor/DRI3 until dmabuf v4, no mode-switch emulation, PRIMARY not bridged, no cursor images, no tray): docs/E2-GAMING.md, src/flcomp/README. Screenshots docs/m7/xwl-*.png. Not tried: the Steam client, Flatpak, a real GPU. Later, in the release-11 round: an X11 client ran through Xwayland in QEMU, and the `check-keys` Print failure turned out to be the chroot's own snappel and fllock (fixed in tests/ws.sh and tests/keys.sh); all five `make check` targets pass in the chroot.

**Laptop round (2026-11-11):** see docs/INTEGRATION-TODO.md. Packages rebuilt: freebws 9, flsettings 7, flpanel 10, flstore 2, flsplash 3, flcomp 10 (carries gl.c/dmabuf.c multi-plane import; shared with the XWayland work, must be built after it), initscripts 6 (iwd logs to /var/log/iwd.log), new flreport 1. Test image: ~/freelfs-work/scratch/hw-image.img (QEMU virgl run passed; real-GPU run was done headless on the Radeon 610M render node).

**Release-11 integration round (2026-11-11, rootfs owner):** flcomp is release 11, built from the current tree in one go (it has both the XWayland window manager, xwm*.c, and the multi-plane/DCC dmabuf import of gl.c/dmabuf.c; the release-10 package was built between the two agents' edits). All five flcomp suites pass in the build chroot: `make check` (run.sh, perf.sh, x.sh), `check-ws`, `check-lock`, `check-x`, `check-keys`. Three test-harness bugs fixed (no flcomp code changed): the Print steps of ws.sh and keys.sh failed in the chroot because the chroot has snappel and fllock installed (Snappel took Print, Super+L really locked the screen so Print was ignored; the tests now set FLCOMP_SNAPPEL/FLCOMP_LOCK), and `make check` ran x.sh without building tests/xclient first (30 failures on a clean tree; check now depends on it). No other recipe was stale (every recipe's version-release has a package in the build output and no src tree is newer than its package). Image rebuilt (build/50-image.sh). hw-image.img removed from the scratch directory. QEMU, all on the new image: `boot-test.py` 2 G and 1 G pass (44 steps each); `m8-freebws.sh 1G` (start page, https://example.com, bare host name, DSSL pages, 3 and 4 tabs, links, discard; no OOM); `m10-features.sh 1G` passes (pictures docs/m7/m10-*-1G.png looked at); new `build/m11-x11-gpu.sh x11|gpu` (+ m11-x11-gpu.cmds, m11-x11.tty, m11-gpu.tty; pictures docs/m7/m11-*): an X11 test client (tests/xclient) gets its Xwayland window with an flcomp frame and title, panel entry, Super+Left snaps it, `flreport` writes a 99 kB tarball with 47 entries (summary, hw, logs, config, net ...); FreeBWS on a virgl GPU (virtio-vga-gl, QEMU egl-headless on the host's Radeon 610M): flcomp says "dmabuf through the GPU (virgl ...)", freebws "dmabuf through /dev/dri/renderD128", https://example.com and https://en.wikipedia.org/wiki/Linux render (m11-gpu-https-1G.png, m11-gpu-wiki-1G.png), no OOM. build/51-qemu.sh: a `-display` in FLFS_VM_VGA now replaces the `-display none` of --serial (virgl needs it; QEMU cannot screendump a virgl scanout, so the gpu pictures are Snappel's, sent over the serial console). RAM numbers: docs/RAM-PROFILE.md (build/iso-test.py --profile). Final live ISO: ~/freelfs-work/freelfs-live.iso, 883 546 112 bytes (843 MiB), sha256 49c5df2498805c2d8701c6d4fad99a0a58d5b3fdb56bcf39c62959a741ce82ec; iso-test.py PASS at 1 GB on its serial twin (console=ttyS0 added, same root; deleted afterwards), the normal-cmdline ISO itself boots to the keyboard question, login screen and desktop at 1 GB (screenshots). Not tested: anything on real hardware (laptop GPU path, Wi-Fi, bare-metal desktop), Steam/Flatpak through Xwayland, glamor/DRI3 for Xwayland (needs dmabuf v4 feedback), a mouse-driven X11 session, the flcomp.log rotation after a crash.

**Security round (2026-11-11), status:** written and tested host-side, not in the image yet (see docs/SECURITY.md, docs/INTEGRATION-TODO.md "Security round").
- D3 follow-up, replay/expiry: the signed index now carries a serial and valid-until (default 14 days); `flpkg sync` and `check-updates` refuse an expired index or a lower serial (state in /var/lib/flpkg); key rotation with a transition period (`flpkg key introduce|list`); `src/flpkg/tests/test-trust.sh`. The repo must be re-signed at least every 10 days once hosted. Key custody rules in SECURITY.md; the key has not been generated yet.
- CVE tracking: `build/cve-check.py` (OSV for PyPI, NVD CPE matching for the rest, name map in `build/cve-names.conf`), report in docs/CVE-REPORT.md, update cadence proposal in SECURITY.md. No package was changed because of it.
- FreeBWS sandbox: wpewebkit release 2 enables ENABLE_BUBBLEWRAP_SANDBOX (a full rebuild), freebws release 10 probes bwrap, enables the sandbox, and falls back with a logged warning and a Settings note; test steps in INTEGRATION-TODO.md. Untested in a running system.
- Secure Boot: options only (SECURITY.md section 4), nothing implemented.

**Round of 2026-11-11 (rootfs owner: drift, small bugs, live RAM):** status lines of the items above corrected where INTEGRATION-TODO.md had the later truth; every recipe declares `arch=(x86_64)` only. New or changed: fl_shell version 6 (`set_desktop`: icon layer between wallpaper and windows; flpanel's deskicons.c fills it from .desktop files with `X-FreeLFS-Desktop-Icon=true`, so "FreeLFS install" is an icon on the live desktop; no icon logic in flcomp); the egg is a maximized window and stays below the panel; flvideo Open (button, Ctrl+O) through the portal's file dialog (libflui has no dialog of its own); mp3 verified with `build/mk-mp3.py` (hand-built Layer III frames, since the ffmpeg here has no encoder) in `build/m10-features.sh`; `flsnap rollback` writes the left state's package list; live session: no Flathub (session script), Software Store says apps can be installed after installing FreeLFS and offers the installer plus "Try one small app anyway"; RAM first trim 110 -> 84 MB at 1 GB (docs/RAM-PROFILE.md); flreport memory.txt. Screenshots: docs/m7/live-desktop-icon.png, live-store-message.png, flvideo-open-dialog.png, egg-below-panel.png. QEMU only.
