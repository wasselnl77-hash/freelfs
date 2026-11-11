# M7 design

Goal: a desktop that looks like the mockups, runs in 1 GB of RAM and needs no
usable GPU. Everything in C, MIT, written by us. Palette and layout: the
mockups (`fl-*` colours below). No gradients, blur, shadows, rounded corners.

| Program | Source | What |
|---|---|---|
| `flcomp` | src/flcomp | Wayland compositor |
| `libflui` | src/flui | Toolkit: drawing, text, widgets, Wayland client glue |
| `flpanel` | src/flpanel | Bottom panel + start menu |
| `flterm` | src/flterm | Terminal (runs flsh) |
| `flsetup` | src/flsetup | First boot keyboard choice |
| `flsettings` | src/flsettings | Settings: General, Display, Mouse & touchpad, Keyboard, Wi-Fi (iwd over D-Bus) |
| `flcrash` | src/flcrash | "Something went wrong" screen when the desktop session dies; `flcrash-test` triggers it |
| `flfiles` | src/flfiles | File manager: places, drives, icon grid, Trash |
| `fledit` | src/fledit | Text editor: UTF-8, undo, find, line numbers |
| `fllock` | src/fllock | Lock screen: clock, date, password; the compositor holds the lock (fl_shell lock); `fllock-check` is its setuid password checker |
| `flgreet` | src/flgreet | Login screen on tty1 (launcher as root + `flgreet-ui`) |
| `flpackages` | src/flpackages | Package app: lists, search, details, Install and Remove; `flpkg-seat` is its setuid helper (mode 4750 root:seat). The Flathub tab browses and installs Flatpak apps per user through `flatpak` (no helper) |



## Rules for all of it
- Memory budget (RSS): flcomp < 30 MB, flpanel < 15 MB, flterm < 20 MB.
- Rendering is CPU: clients draw into wl_shm buffers. Flat design = mostly
  opaque fills, so this is cheap. Only redraw damage. No busy loops; idle = 0 CPU.
- C17, `-Wall -Wextra`, tabs, comments like src/flsh and src/flpkg, SPDX line.
  Each source dir: Makefile with `all install clean`, PREFIX=/usr, DESTDIR, and
  a README. Recipe in recipes/NAME/recipe like recipes/flsh/recipe,
  groups=(desktop).

## flcomp
Libraries: wayland-server, libdrm, libinput, libseat, libxkbcommon, libudev.
No wlroots. Own DRM/KMS output via dumb buffers (double buffered, page flip),
CPU compositing with damage tracking. A GLES path for dmabuf clients comes in M8.

Globals: wl_compositor, wl_subcompositor, wl_shm, wl_output, wl_seat (pointer,
keyboard), wl_data_device_manager (clipboard), xdg_wm_base, fl_shell.
Windows: focus on click, move by title bar drag (xdg_toplevel.move), resize,
maximize, minimize. Server draws no decorations: flui draws them (client side).
Focused window gets the 2px accent top edge from flui via xdg_toplevel activated state.
Keys: see "Keyboard shortcuts" below.
`flcomp --firstboot` runs flsetup alone and exits with it. Otherwise it starts
`flpanel`. Keyboard layout from /etc/keyboard.conf (`LAYOUT=de`, default us).
Backend for tests: `FLCOMP_SCREENSHOT=FILE.ppm` dumps a frame on SIGUSR1.

## fl_shell (protocol/fl-shell.xml in src/flcomp, installed to /usr/share/flcomp)
Private protocol for flpanel:
- requests: `set_panel(surface, height)` bottom panel, reserves its height;
  `set_menu(surface)` popup above the panel's left edge, closed by click outside;
  `activate(id)`, `minimize(id)`, `close(id)`; `set_keyboard_layout(name)`;
  since v2 `set_flyout(surface, x)`: popup above the panel centred on x (the volume slider);
  since v3 `set_output(w, h, hz, scale)`, `confirm_output`, `revert_output` (Settings' Display page).
- events: `window_added(id, app_id, title)`, `window_changed(id, title, focused,
  minimized)`, `window_removed(id)`, `menu_dismissed()`, `keyboard_layout(name)`;
  since v2 `flyout_dismissed()` and `volume_key(action)` (flcomp swallows the XF86Audio
  volume and mute keys and repeats raise/lower while held); since v3 `output_mode(w, h, hz, flags)`
  for every mode of the display (flag 1 in use, 2 preferred), `output_state(scale, w, h, pending)`
  and `output_failed()`. Older requests and events are unchanged.
  since v4 `set_notifications(surface)` (the notification cards, top right, above everything but the
  lock screen; pixels with alpha 0 take no input), `lock(surface)` and `unlock()` (the lock screen, see
  "Lock screen" below) and the error enum (`role`, `locked`); event `lock_state(locked)` to every
  binding of version 4. flpanel binds v4, flsettings binds at most v3 (no event of v4 reaches it). flpanel binds v3 (it needs `show_osd`) and has empty handlers for the
  three output events: libwayland aborts a client that gets an event nobody listens to, which is what the panel did
  at start until this was added.

## flui
Pixel format ARGB8888. API: window create, event loop (fd based, timers),
fill rect, line, anti-aliased path fill/stroke (own scanline rasterizer, for
icons), text via FreeType + HarfBuzz with a glyph cache. Widgets: label, button,
icon button, text field, list, grid, scrollbar, switch, title bar with
minimize, maximize, close. Keyboard focus with Tab. Icons: built-in vector paths
as in the mockups (folder, terminal, wifi, volume, battery, ...).
Theme: dark and light tokens.

dark:  bg 1d2125 panel 272c33 raised 343a43 line 3a4048 text d8dce0 dim 8a9199
       accent 3f9b63 on-accent 0e1a13 select 2f4a3b
light: bg dfe3e7 panel f4f5f7 raised ffffff line c6ccd3 text 1d2125 dim 565e67
       accent 2c7a4b on-accent ffffff select cfe6d8

Fonts: IBM Plex Sans (UI, 13 px), IBM Plex Mono (terminal, 13.5 px), package
`ibm-plex`. `FLUI_PPM=FILE` renders the first frame to a file with no compositor,
for tests and screenshots.

`fl_app_run` is `fl_app_prepare`, poll, `fl_app_dispatch`; prepare draws and dispatches, and either may add an
fd (the panel starts a wpctl job from the volume slider's draw), so it returns -2 when the caller's pollfd array
became too small and `fl_app_run` simply asks again with more room. Before, the loop ended instead and the panel
quit on the first slider drag.

Settings file `~/.config/freelfs/desktop.conf`, `key=value`: `theme=dark|light`,
`panel_labels=0|1` (default 0: panel shows icons only, 1: icon and name).
Programs reload it on change (inotify). flcomp reads it too (src/flcomp/config.c) for the
input and display keys of the Settings pages below.

## flpanel
Layout of mockup Panel.dc.html: 44 px, start button (accent), window list,
keyboard indicator, Wi-Fi, volume, battery, clock. Window entries: 44 px icon
with accent underline when focused, or icon + name when panel_labels=1. Start
menu as in Menu.dc.html: search, categories, programs from .desktop files in
/usr/share/applications, Lock, Log out, Shut down. Clicking the keyboard
indicator cycles layouts.

## flterm
VT100/xterm subset enough for flsh, vim, less, top: colours (16 + 256), cursor
moves, scroll regions, alternate screen, bracketed paste, resize. Scrollback
2000 lines. Copy and paste via wl_data_device. Uses the flsh ANSI colours mapped
to the flboot palette.

## Session
inittab runs /usr/sbin/flgreet on tty1 (agetty if it is missing). flgreet
starts a private `flcomp --firstboot` whose one client, flgreet-ui, shows the
accounts (uid 1000-59999) and checks the password; flgreet then creates
/run/user/UID and runs `login -f NAME`. /etc/profile.d/desktop.sh exports
XDG_RUNTIME_DIR and starts `flcomp` for non-root users on tty1. flcomp runs
/usr/lib/flcomp/session after the panel: session D-Bus (fixed address
$XDG_RUNTIME_DIR/bus), pipewire, wireplumber, pipewire-pulse. If flcomp cannot
start for the login screen, flgreet falls back to agetty on tty1.
rc.sysinit: if /etc/keyboard.conf is missing, run `flcomp --firstboot`
(flsetup); console keymap comes from the same file.

## Notifications, battery, lock screen (docs/ROADMAP-NEXT.md A2, A6, A1)
- Notifications: flpanel owns org.freedesktop.Notifications (spec 1.2) on the session bus, draws the
  cards (top right, a transparent fl_shell surface, at most four) and the notification centre (a
  flyout above the clock, history of 50, Do not disturb, Clear all); `flnotify` sends from scripts.
  Details in src/flpanel/README.
- Battery: icon with the percentage, a flyout (state, time left, watts, Power saving switch);
  power saving = desktop.conf power_saver=1: flcomp dims to 70 % and caps at 30 fps, `flpower saver
  on` sets the CPU governor to powersave and the energy preference to power; warnings at 10 % and 5 %
  through the notifications.
- Lock screen: enforced by flcomp (src/flcomp/lock.c). `fllock` asks with fl_shell `lock`; while locked
  only its surface is shown and gets input, only Ctrl-Alt-F1..F12 and the media keys work, and if
  fllock dies the screen stays locked (solid colour, restart after 300 ms, then a hint and Enter). Super+L
  locks at once and starts it; so does the lid (libinput switch, lock_on_lid=1). The password is checked
  by `fllock-check` (setuid root, group seat 4750) against the caller's own shadow entry; it takes
  turns and holds a lock file one second after a wrong try. No suspend path exists yet (flpower: off, restart).

## Tray
flpanel shows battery (/sys/class/power_supply, hidden without one), network
(/sys/class/net, /proc/net/wireless) and volume (wpctl: scroll changes it,
click mutes). One 5 s timer for the files; wpctl runs in a child process and is
read through a pipe.

## Date & time (flsettings, flui worldmap, fltime)
Settings > Date & time (A10): a world map drawn by libflui (`worldmap.c`, Natural Earth
projection, land and borders from public-domain Natural Earth 1:110m data converted at
development time by `src/flui/tools/mkworldmap.py` into `worldmap_data.c`, 70 kB). Places are
tzdata's `zone.tab`, read at run time; a click picks the nearest place, the map tints each
place's region (land nearer to it than to any other) and all regions with the same time now.
The zone is written by `fltime-set` (setuid root, group seat, mode 4750 like `flpower`): it
accepts only `zone NAME` (a real TZif file inside zoneinfo), `ntp on|off`, `sync` and `time
EPOCH`. Automatic time is the `ntp` rc service running `fl-sntp` (own SNTP client, no busybox
or chrony in the system); `/etc/fltime.conf` holds `NTP=` and `SERVERS=`, the state is
`/run/fltime.status`. The panel calls `tzset()` whenever it draws its clock and honours
`clock_24h` in desktop.conf. The same map and search widgets are meant for the installer (E1).
Tests: `src/fltime/tests/test.sh`, `src/flsettings/tests/datetime.py` (headless flcomp),
screenshots `docs/m7/settings-datetime*.png`.

## Wi-Fi (flsettings, iwd)
Settings has a sidebar with two pages, General and Wi-Fi. `flsettings --page wifi`
opens the Wi-Fi page; the panel's network icon runs exactly that (it also
raises a Settings window that is already open, and a second `flsettings` only
tells the first one, through `$XDG_RUNTIME_DIR/flsettings.sock`, which page to
show). The page shows a Wired line (/sys/class/net), the Wi-Fi switch and state,
and the networks sorted by signal with signal bars, a lock for secured ones and
Connect, Disconnect, Forget buttons; Scan asks for a new scan (one is also asked
when the page opens, and a visible page refreshes every 5 s; hidden = no timer).
Messages: no D-Bus, iwd not running, access denied, no Wi-Fi hardware, Wi-Fi off.
- `wifi.c` talks to net.connman.iwd with libdbus on a private system-bus
  connection (fd in flui's loop). Snapshot = GetManagedObjects + Station.
  GetOrderedNetworks; any iwd signal re-takes it after 150 ms. Connect, Disconnect,
  Scan, Forget and Device.Powered are plain calls.
- Passwords: the page exports net.connman.iwd.Agent and registers it. iwd's
  RequestPassphrase is answered when the user submits the masked field (8 to 63
  characters, Show button, Ctrl-V); Cancel/Escape answers
  `net.connman.iwd.Agent.Error.Canceled`. The one buffer is wiped with
  explicit_bzero after sending or cancelling; nothing is logged or stored (libdbus
  does not wipe its copy of the message). Enterprise (802.1X) networks that iwd
  has not been provisioned for are refused with a message; hidden networks are
  not offered yet.
- Permissions: iwd-dbus.conf allows root, wheel and netdev; the iwd package adds
  iwd-seat.conf for group seat (every desktop user), and base-files declares group
  netdev (gid 86), so dbus-daemon no longer warns about an unknown group.
- IP address: /etc/iwd/main.conf sets EnableNetworkConfiguration=true (iwd's
  own DHCP client) and NameResolvingService=resolvconf. There is no resolvconf
  package: iwd's service puts /usr/lib/iwd/bin first in PATH, where a small shell
  script implements `-a NAME` and `-d NAME` and writes /etc/resolv.conf (header
  "generated by iwd"; removed again only when it still carries that header).
  dhcpcd (the `network` service) has `denyinterfaces wl*` in /etc/dhcpcd.conf.
- Hardware path (RTL8852BE): kernel rtw89_8852be (module; udev loads it by PCI alias
  through 80-drivers.rules, depmod pulls in rtw89_core, rtw89_pci, rtw89_8852b*,
  mac80211, cfg80211, libarc4), firmware /lib/firmware/rtw89/rtw8852b_fw*.bin
  (linux-firmware) and /lib/firmware/regulatory.db(.p7s) (wireless-regdb; cfg80211
  checks the signature with the keys built into the kernel). build/boot-test.py
  checks these files and the module dependencies.
- Tests: src/flsettings/tests/fakeiwd.c is a stand-in iwd; `apps.py flsettings` runs
  it on a private bus with headless flcomp (list, scan, open network, wrong and right
  password, Escape, Disconnect, Forget, page hand-over, iwd gone, no hardware;
  screenshots docs/m7/flsettings-wifi-*.png). The kernel has
  CONFIG_MAC80211_HWSIM=m; boot-test.py loads it with two radios, runs an access
  point with iwd on one and joins it from the other (passphrase, iwd's DHCP).
  Real Wi-Fi hardware is not testable in QEMU.

## Settings pages: Display, Mouse & touchpad, Keyboard
Same look as General: sections with a rule, 56 px rows (title, dim line, control on the right),
choice tiles, the flyout's slider (4 px track, 10 x 20 thumb), switches. A page taller than the
window scrolls (wheel or bar). Every setting is a key in desktop.conf that flcomp applies at
once (inotify); only the display mode and scale go through fl_shell, because they can leave the
screen unusable.
- Display: resolutions (tiles, biggest first) and refresh rates from the connector's modes, scale
  100/125/150/200 %, brightness (backlight sysfs, slider), night light (switch, warmth slider).
  Apply sends `set_output`; flcomp switches (new dumb buffers, modeset; when the display refuses,
  the old mode stays and `output_failed` comes back), lays the desktop out at mode / scale and
  restarts the panel; a banner "Keep these display settings? Going back in N s" with Keep and
  Revert counts down, and flcomp itself goes back after 15 s unless `confirm_output` arrives or the
  asking client dies. Only a kept setting is written (`mode=1920x1080@60`, `scale=125`) and used
  at the next start; a mode that fails at start falls back to the preferred one. Scale is done
  by the compositor: the desktop is smaller (mode / scale, at least 640 x 400) and enlarged when it is
  copied to the screen (pixel doubling at 200 %, bilinear between: slightly soft), so no
  program needs to know. Brightness writes `/sys/class/backlight/*/brightness`; the udev rule
  `90-flsettings-backlight.rules` (flsettings package) makes it group seat writable, and
  `brightness=` is applied at start by flcomp. Night light: green and blue tables applied in the
  copy to the screen (`night_light=`, `night_warmth=`).
- Mouse & touchpad: `pointer_speed` (-100..100), `pointer_accel` (adaptive|flat), `left_handed`,
  `mouse_natural`, `touchpad_natural`, `tap_click`, `typing_disable`. flcomp keeps every libinput
  device (present and hot-plugged) and configures it; a touchpad is a pointer with tap fingers.
- Keyboard: the four layouts (as flsetup and /etc/keyboard.conf), `key_repeat_delay` (150-1000 ms),
  `key_repeat_rate` (2-60 per second; wl_keyboard.repeat_info to every client at once),
  `numlock` (on at start; the LEDs follow), and a field to try it.

## Keyboard shortcuts
All of them are handled by flcomp (src/flcomp/seat.c `handle_binding`), whichever program has focus; the key
that completes a shortcut (and its release) is not passed on. The same list is on Settings' Keyboard page
(read only, `draw_shortcuts` in src/flsettings/pages.c).

| Keys | What |
|---|---|
| Super (tapped alone) | toggles the start menu (`fl_shell menu_toggle`); a Super that was used with another key does not |
| Alt+Tab, Alt+Shift+Tab | next / previous window, hold Alt: focus follows live, a row of window pictures (switcher.c) shows all windows (minimized ones excluded) with the picked one outlined in the accent colour; releasing Alt ends it and puts the picked window on top. The pictures are the windows scaled down, no titles: the compositor has no fonts, the panel's entry for the picked window lights up at the same time |
| Super+Left, Super+Right | snap the focused window to the left or right half of the work area (above the panel); the other direction from a snapped window puts it back; dragging a snapped window by its title bar restores its size, like a maximized one; a half is kept when the panel changes size |
| Super+Up | maximize |
| Super+Down | restore (from maximized or snapped), otherwise minimize |
| Super+E | `flfiles` |
| Super+T, Ctrl+Alt+T | `flterm` |
| Super+L | `fllock`, only if /usr/bin/fllock exists (the panel's Lock button has the same rule); there is no lock screen yet, so today the key does nothing and passes through |
| Print | screenshot of the whole screen: ~/pictures/screenshot-YYYY-MM-DD-HHMMSS.png (the pictures folder is created when missing; -2, -3 ... if the same second already has one; no pointer, no indicator, no switcher in it). shot.c composites the desktop once more without those, forks, and the child writes the PNG with libpng (level 3) and a `.part` file that is renamed at the end, so the compositor never waits. When the child reports, the indicator shows the viewfinder icon with a green bar (saved) or a red one (failed). At a scale above 100 % the picture is the desktop at its logical size |
| XF86 volume, mute, brightness, microphone keys | see below |
| Ctrl+Alt+Backspace | quit the session |
| Ctrl+Alt+F1 ... F12 | switch virtual terminal |

Tests: `make check-keys` in src/flcomp (tests/keys.sh, headless; HOME and PATH point into a scratch directory):
snap left/right and back, maximize, restore, minimize, no menu toggle from combinations, Super alone still toggles,
the switcher (pixels of the box, the picked tile, Alt-Shift-Tab, gone after Alt is released), Super+E, Super+T,
Ctrl+Alt+T, Print (the PNG is decoded: size, panel pixel, no pointer, no indicator, no .part left; the indicator
appears afterwards). docs/m7/shortcuts-flcomp.png (switcher, snap, screenshot with its indicator),
docs/m7/flsettings-keyboard-shortcuts.png.

## Media keys and the indicator
flcomp swallows these keys (src/flcomp/media.c), everywhere, whichever program has focus:
XF86AudioRaiseVolume, XF86AudioLowerVolume, XF86AudioMute (the panel does the volume through
wpctl in 5 % steps, as before; without a panel flcomp runs wpctl itself, so a bare session has
them too), XF86MonBrightnessUp, XF86MonBrightnessDown (flcomp writes the backlight's sysfs file in
5 % steps, never below 5 %, and `brightness=` in desktop.conf, the same file and key as the
Display page; the Settings slider re-reads the backlight once a second while it is visible) and
XF86AudioMicMute (wpctl on the default source). Raise and lower repeat while held. Without a
backlight, or without write access (group seat), the brightness keys do nothing and say so once
in the log.
The one on-screen indicator (osd.c): a 240 x 56 box above the panel, centred, with the icon (speaker,
speaker crossed out when muted, sun, microphone) and a bar in the accent colour, flat, panel colours,
shown for 1.2 s and faded out in 300 ms; a new key replaces it. The compositor draws it, so it
works with or without a panel; the panel asks for it after its own volume handling with
`fl_shell.show_osd(kind, percent, flags)` (since version 3; kind 0 volume, 1 brightness,
2 microphone, flags 1 muted). docs/m7/osd-volume.png, osd-brightness.png.
Every volume change of the panel now shows it, not only the keys: the wheel on the tray icon and the
flyout's slider and mute button (`vol_set_pct`, `vol_toggle_mute` in panel.c ask for it, a drag sends one per
step and each replaces the last), docs/m7/osd-volume-panel.png. A fifth look, kind 3 (the viewfinder icon),
is for the screenshot and is only used inside flcomp (`fl_shell.show_osd` still accepts kinds 0 to 2).

## Crash screen (flcrash)
When flcomp ends with a non-zero status (a signal shows as 128 + number), /etc/profile.d/desktop.sh
runs `flcrash --prog flcomp --status N --secs S --crashes C --log FILE`. flcomp's stderr (and the
panel's and every program's) is in `$XDG_RUNTIME_DIR/flcomp.log`. flcrash draws on the KMS device
(libseat, libinput; the console as a fallback), solid green field (#1f3d2e): a wide "oh no." (the text
drawn tall and squashed vertically, so its edges stay sharp), a big ":/" (`EMOTICON` and `OHNO` defines),
"Something went wrong", one sentence, key hints (Enter restart the desktop, R restart the computer, S the
log, Esc the text console) and technical lines (Code, Program, Status, Ran for, Log). The FreeLFS mark
with its middle bar knocked out of place sits bottom right. Enter exits 0 and the script starts flcomp
again; Esc exits 3 and leaves a shell. A copy of the log's tail is kept in
`~/.local/state/freelfs/last-crash.log`. `flcrash --demo CODE` with `FLCRASH_PPM=FILE` writes the picture
without a display (docs/m7/crash-*.png).

Error codes (one table in flcrash.c, chosen from the exit status, the run time, the crashes of the last
minute and what the log says):

| Code | When | Test (`flcrash-test KIND` crashes the real flcomp of your session) |
|---|---|---|
| FL-SIGSEGV | killed by SIGSEGV: a memory access bug | `flcrash-test segv` |
| FL-SIGABRT | SIGABRT: flcomp stopped itself after an internal error | `flcrash-test abort` |
| FL-SIGBUS | SIGBUS: memory or graphics driver fault | `flcrash-test bus` |
| FL-SIGILL | SIGILL: an instruction the processor cannot run | `flcrash-test ill` |
| FL-SIGFPE | SIGFPE: arithmetic error | `flcrash-test fpe` |
| FL-SIGKILL | SIGKILL: most likely the kernel's out-of-memory killer, the sentence says so | `flcrash-test kill` |
| FL-SIGHUP | SIGHUP: lost its terminal | `flcrash-test hup` |
| FL-SIG<n> | any other signal | `flcrash-test other` (SIGXCPU, shown as FL-SIG24) |
| FL-NO-SEAT | ended within 5 s and the log has "cannot open a seat" or "seat not enabled": seatd not running or no group seat | `flcrash-test noseat` |
| FL-NO-DISPLAY | within 5 s and "no usable display": no DRM device or no display connected | `flcrash-test nodisplay` |
| FL-NO-RUNTIME | within 5 s and "cannot create ...": XDG_RUNTIME_DIR missing or not writable | (only a real failure) |
| FL-NO-MEMORY | within 5 s and "out of memory" | (only a real failure) |
| FL-START-FAILED | ended within 5 s with another message | `flcrash-test startfail` |
| FL-EXIT-<n> | ended with status n after 5 s or more | `flcrash-test exit` (status 3) |
| FL-RESTART-LOOP | the third crash within 60 s: the sentence says starting again would crash again; the hints put S and R first and "Try the desktop once more" last; the code of the last crash is shown | `flcrash-test loop` |

`flcrash-test` sends the signal to your own flcomp (no root; flcomp's SIGUSR2 is a test hook that exits
with status 3). The start failures and the loop are pretended by a note in
`$XDG_RUNTIME_DIR/flcrash-simulate` that the session script reads after flcomp ended, so the real path
(status, timing, log, counting) is what runs. Not detected: no input devices (flcomp goes on without
input, it does not exit).

Kernel panic: the console already has the FreeLFS colours (`vt.default_*` on the command line); flboot.conf,
the ISO and flboot-install now also pass `panic=30`, so a panicked machine restarts itself after 30 s
instead of hanging. No kernel changes.

## Login screen take-over (flgreet-ui)
The boot screen ends on a big, widely spaced, pale "FreeLFS" (IBM Plex Sans Medium, em = 0.205 S where S is the
logo size picked by `pick_size`, 0.2 em between the letters, a gradient from near white to grey-green, see
genart.c `make_text`). flgreet-ui used to cut to its own card with a small, tight, semibold title, which looked
like the word shrinking and getting slimmer. Now the card's title is the wordmark itself (Medium, 28 px, heading
colour), and when flsplash has just handed the screen over (`/run/flsplash.released` exists; flsplash removes it
when it ends, so after a log out there is no animation) the first frames draw the word exactly where flsplash left
it (same S, same place, same size, spacing, weight and gradient: `splash_wordmark` repeats flsplash's layout; checked
against `flsplash --render` at 1920x1080, 1366x768, 1280x800 and 1024x768, within 1-2 px) on a flat copy of flsplash's
dark room, then for 640 ms ease it up into the card while its size, spacing and colour settle, the room turns into
the desktop colour and the card fades in (the light theme turns the ground faster, so the pale word never sits on
a mid grey). The logo and its glow are in flsplash's compiled-in art and are not redrawn: they still go at once.
`FLGREET_ANIM_T=MS` shows the take-over frozen at MS ms (tests, screenshots); input works from the first frame.
docs/m7/flgreet-takeover-before.png (flsplash's last picture and the old card), flgreet-takeover-after.png (0, 160,
320, 480, 640 ms at 1280x800).

## Boot screen (flsplash)
The one place that breaks the flat rule, on purpose: a PS3-era glossy 3D logo,
overdone and proud. The three tilted bars as beveled slabs with a glossy green
surface and top highlight, rendered ahead of time at build time; a soft light
band sweeps across as it appears; faded mirror reflection on a dark glossy
floor and a soft glow behind; "FreeLFS" fades in below, spaced, drawn once at
one size. About 4 s, smooth; at boot only the logo region is redrawn, so it
stays cheap on old machines. Holds until the login screen takes over.
Esc shows the boot text; a failed step switches to text. Survives the
simpledrm to GPU driver handover (reopen the device, continue by the clock).
