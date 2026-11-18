# FreeLFS configuration reference

This page lists every configuration file FreeLFS reads, every key it understands, and what each one does. It was written from the source code and the shipped files in `config/` as they stood on 2026-11-17.

Most people should never edit these files by hand. Use the Settings app (`flsettings`) for desktop options, General Manager for startup apps, and the installer for disks. This page is for the times you do need to look inside.

Words used on this page:

- **System** means a file that belongs to the whole machine, such as `/etc/rc.conf`.
- **Per-user** means a file in one person's home folder, such as `~/.config/freelfs/desktop.conf`.
- **Live** means the change takes effect at once. **Restart** means the program that reads the file must be started again. **Reboot** means the change waits for the next boot. "Next login" and "next start" are spelled out where they apply.
- Items marked **not verified** could not be confirmed from the code or the shipped files.

## Contents

- **Desktop**
  1. `~/.config/freelfs/desktop.conf` (the main desktop file)
  2. Night light, outputs and brightness (keys inside `desktop.conf`)
  3. flcomp environment variables and session start
  4. Startup apps (autostart)
  5. GTK colour and font hints (`settings.ini`)
  6. Desktop portal configuration (`freelfs-portals.conf`)
- **System**
  7. `/etc/rc.conf` and the boot services
  8. `/etc/inittab` and the terminals
  9. `/etc/fstab` and the btrfs subvolume layout
  10. Snapshots (`/etc/flsnap.conf`)
  11. Time (`/etc/fltime.conf`)
  12. Shell and login files (`profile`, `inputrc`, `shells`, `issue`, `os-release`, `hostname`, `hosts`, `group`, `passwd`)
  13. Kernel settings (`sysctl`) and the kernel build config
  14. File ownership list (`ownership.list`)
- **Packages**
  15. `/etc/flpkg.conf` (repositories, keys, cache, transactions)
  16. Signing keys and the repository address
  17. Pre-transaction hooks and the package database
- **Boot**
  18. `flboot.conf` and the boot menu
  19. Live system: kernel options and `/run/live/*` mounts
  20. Persistence (`freelfs-persist.img`)
  21. `/etc/freelfs/splash.conf` (boot screen)
- **Hardware**
  22. `/etc/keyboard.conf`
  23. udev rules (`flmount`, backlight, Bluetooth, game controllers)
  24. Removable drives (`flmount`)
  25. Bluetooth
  26. PipeWire (sound)
  27. Printing (CUPS and `flsocketd`)
  28. Game settings
- **Network**
  29. Wi-Fi (`iwd`)
  30. Wired networking (`dhcpcd`, the `network` service)
  31. Name resolution (`/etc/resolv.conf`)
- **Apps**
  32. FreeBWS (`~/.config/freebws/config`, ad block lists, sandbox)
  33. Default apps (`mimeapps.list`)
  34. Environment variables that every program may read
  35. Every file under `config/` and where it goes

## Where configuration lives at a glance

| Scope | Path | Who owns it |
| --- | --- | --- |
| System | `/etc/rc.conf`, `/etc/rc.d/` | Boot scripts |
| System | `/etc/fstab` | Disks and mounts |
| System | `/etc/freelfs/splash.conf`, `/etc/fltime.conf`, `/etc/keyboard.conf` | FreeLFS tools |
| System | `/etc/flpkg.conf`, `/etc/flpkg/keys/` | flpkg |
| System | `/etc/flsnap.conf` | flsnap |
| System | `/etc/iwd/main.conf`, `/etc/dhcpcd.conf`, `/etc/cups/cupsd.conf` | Network and printing |
| System | `/boot/flboot/flboot.conf` (on the EFI partition) | flboot |
| System | `/usr/lib/sysctl.d/`, `/etc/sysctl.conf` | Kernel settings |
| System | `/usr/lib/udev/rules.d/` | udev rules from several packages |
| System | `/usr/share/applications/mimeapps.list`, `/etc/xdg/` | Default apps and system autostart |
| Per-user | `~/.config/freelfs/desktop.conf` | Desktop settings (Settings app) |
| Per-user | `~/.config/freebws/config` | FreeBWS settings |
| Per-user | `~/.config/mimeapps.list` | Default apps chosen by the person |
| Per-user | `~/.config/autostart/` | Startup apps |
| Per-user | `~/.config/gtk-3.0/settings.ini`, `~/.config/gtk-4.0/settings.ini` | Written by fltheme for GTK apps |
| Per-user | `~/.local/share/freelfs/` | Wallpapers, Flathub stamp, and similar data (not configuration) |

---

# Desktop

## 1. `~/.config/freelfs/desktop.conf`

This is the main settings file. Almost every desktop option lives here.

**Path.** The per-user file is `$XDG_CONFIG_HOME/freelfs/desktop.conf`, or `~/.config/freelfs/desktop.conf` when `XDG_CONFIG_HOME` is not set. The environment variable `FLUI_CONF` replaces the whole path (used for tests and for the login screen). There is also a system copy at `/etc/freelfs/desktop.conf`. Only the login and first-boot screen (`src/flgreet/flgreet.c`) reads it, and only when it exists. The repository does not ship that file.

**Format.** One `key=value` per line. Lines that start with `#` are comments. Spaces around the key and value are trimmed. If a key appears twice, the last line wins. Keys are case-sensitive. Values are plain text; there is no quoting.

**Who reads it.** Five groups of programs read this file:

- The shared UI library `libflui` (`src/flui/conf.c`). Every FreeLFS window uses it for theme, font, accent and panel size.
- The compositor `flcomp` (`src/flcomp/config.c`, `theme.c`, `output.c`, `perf.c`, `nightlight.c`, `seat.c`, `sound.c`, `anim.c`, `power.c`, `media.c`).
- The panel `flpanel` (`panel.c`, `quick.c`, `battery.c`, `notify.c`, `perfmode.c`, `icons.c`, `wsmenu.c`).
- Settings `flsettings` (`pages.c`, `display.c`, `wallpaper.c`, `workspaces.c`, `datetime.c`, `flsettings.c`).
- The theme app `fltheme` (`fltheme.c`, `presets.c`, `hints.c`) and the portal `flportal` (`src/flportal/main.c`).

**Writing.** Settings rewrites the whole file when it changes a value. It writes only the `key=value` lines, so **comments you added by hand are lost** after a change in Settings. `flcomp` changes one line at a time and keeps the rest of the file as it is.

**When changes apply.** `flcomp`, the panel and every `libflui` app watch the file with inotify. Most keys apply at once. The exceptions are listed in the tables below.

### 1a. Look and feel

| Key | Type and allowed values | Default | What it does | Read by |
| --- | --- | --- | --- | --- |
| `theme` | `dark` or `light` | `dark` | Colour scheme for FreeLFS windows and for `flcomp` drawing. Any value other than `light` means dark. | `flui/conf.c`, `flcomp/theme.c`, `fltheme/hints.c`, `flportal/main.c` |
| `accent` | `green`, `teal`, `blue`, `amber`, `orange`, `rose`, or a hex colour `RRGGBB` or `#RRGGBB` | `green` | Accent colour for buttons, focus, selections, the volume indicator, the Alt-Tab frame and the lock-screen caret. | `flui/conf.c`, `flcomp/theme.c`, `flcomp/lock.c`, `flcomp/switcher.c`, `flportal` (as `accent-color`) |
| `font` | Font family name | `IBM Plex Sans` (an empty value also means this) | Font for the interface. | `flui/conf.c`, `fltheme/hints.c` |
| `font_size` | Whole number from 10 to 18 (pixels) | `13` | Interface font size. Values outside the range are clamped. | `flui/conf.c`, `fltheme/hints.c` |
| `buttons` | `right` or `left` | `right` | Side of the window buttons. Applies to FreeLFS windows and to X11 window frames. | `flui/conf.c`, `flcomp/config.c` |
| `titlebar` | `normal` or `compact` | `normal` | Title bar height: 34 pixels for normal, 28 for compact. | `flui/conf.c`, `flcomp/config.c` |
| `panel_position` | `bottom` or `top` | `bottom` | Edge where the panel sits. Windows get the room the panel leaves. | `flui/conf.c`, `flcomp/config.c` |
| `panel_height` | Whole number from 36 to 64 (pixels) | `44` | Height of the panel. Clamped to the range. | `flui/conf.c`, `flcomp/config.c` |
| `panel_icon_size` | Whole number from 16 to 28 (pixels) | `20` | Size of the icons on the panel. Clamped to the range. | `flui/conf.c` |
| `animations` | `on`, `reduced` (fades only), `off` (`0` also means off) | `on` | Window and interface animations. | `flui/conf.c`, `flcomp/config.c`, `flcomp/anim.c` |
| `animation_speed` | Number from 0.1 to 4 (a factor) | `1` | `2` means twice as fast. Clamped. | `flui/conf.c`, `flcomp/config.c` |
| `wallpaper` | `accent`, one of the six accent names, `user:NAME`, or `/absolute/path.flw` | `accent` | Desktop wallpaper. `accent` is the 3D F in the accent colour. `user:NAME` is a picture imported by Settings, kept under `~/.local/share/freelfs/wallpapers/NAME.flw`. A missing file falls back to the default wallpaper. | `flcomp/theme.c`, `flcomp/render.c`, `flsettings/wallpaper.c` |
| `icon_theme` | Name of an icon theme folder (not a path; names with `/` are ignored) | `Papirus` | Icon theme used by the panel. If an icon is missing, `hicolor` is tried next. | `flpanel/icons.c` |

The six accent names and their colours are in `src/flui/flpalette.h`.

**Presets.** The theme app (`fltheme`) can apply four ready-made presets. They write these keys:

- **FreeLFS Dark** writes the default value for every look key above.
- **FreeLFS Light** sets `theme=light` and leaves the other keys alone.
- **Compact** sets `font_size=12`, `titlebar=compact`, `panel_height=38` and `panel_icon_size=18`.
- **Large** sets `font_size=15`, `titlebar=normal`, `panel_height=52` and `panel_icon_size=24`.

### 1b. Sound, notifications, panel and clock

| Key | Type and allowed values | Default | What it does | Read by |
| --- | --- | --- | --- | --- |
| `sounds` | `0` or `1` | `1` | System sounds (login, volume tick, screenshot, error). `0` turns all of them off. | `flcomp/theme.c`, `flcomp/sound.c`, `flsettings/flsettings.c` |
| `notify_dnd` | `0` or `1` | `0` | Do not disturb. When `1`, low and normal notifications show no card. Critical ones are still shown (from the code comment in `notify.c`). | `flpanel/notify.c` |
| `panel_labels` | `0` or `1` | `0` | Shows program names next to the panel icons. | `flpanel/panel.c`, `flsettings/flsettings.c` |
| `clock_24h` | `1` or `0` | `1` | 24-hour clock (`0` gives 12-hour with AM/PM). | `flpanel/panel.c`, `flsettings/datetime.c` |
| `tz_changed` | A Unix time (written by Settings) | not set | Changed each time the time zone changes, so the panel and other programs re-read the zone. Do not edit by hand. | `flsettings/datetime.c` (writes), `flpanel/panel.c` (reads) |
| `power_saver` | `0` or `1` | `0` | Power saving. `flcomp` caps the frame rate and dims the backlight to 70 percent. The file's `brightness` value is not changed. | `flcomp/power.c`, `flpanel/battery.c` |
| `performance` | `auto`, `on`, `off` | `auto` | Performance mode. `on` forces it. `auto` turns it on while a fullscreen window is on top. `flcomp` also reads `1` as on and `0` as off. | `flcomp/perf.c`, `flpanel/perfmode.c` |
| `lock_on_lid` | `0` or `1` | `1` | Closing the lid locks the screen. | `flcomp/config.c` |

### 1c. Workspaces

| Key | Type and allowed values | Default | What it does | Read by |
| --- | --- | --- | --- | --- |
| `workspaces` | `COLSxROWS`, for example `2x2` or `3x2`. A bare number `N` means `Nx1`. Each side is from 1 to 8. | `2x2` | Size of the workspace grid. If a workspace disappears, its windows move to the last one. | `flcomp/config.c`, `flcomp/xdg.c`, `flpanel/wsmenu.c`, `flsettings/workspaces.c` |

Settings offers quick choices of 1, 2, 4, 6 and 8 workspaces (`1x1`, `2x1`, `2x2`, `3x2`, `4x2`).

### 1d. Mouse and touchpad

| Key | Type and allowed values | Default | What it does | Read by |
| --- | --- | --- | --- | --- |
| `pointer_speed` | Whole number from -100 to 100 | `0` | Pointer speed. Changes apply at once. | `flcomp/config.c`, `flcomp/drm.c`, `flsettings/pages.c` |
| `pointer_accel` | `adaptive` or `flat` | `adaptive` | `adaptive` speeds the pointer up with quick movements. `flat` keeps one speed. | `flcomp/config.c`, `flcomp/drm.c` |
| `left_handed` | `0` or `1` | `0` | Swaps the left and right buttons. | `flcomp/config.c`, `flcomp/drm.c` |
| `mouse_natural` | `0` or `1` | `0` | Natural scrolling for mice (content follows the wheel). | `flcomp/config.c`, `flcomp/drm.c` |
| `touchpad_natural` | `0` or `1` | `0` | Natural scrolling for touchpads (two fingers move the content). | `flcomp/config.c`, `flcomp/drm.c` |
| `tap_click` | `0` or `1` | `1` | A light tap on the touchpad is a click. | `flcomp/config.c`, `flcomp/drm.c` |
| `typing_disable` | `0` or `1` | `1` | Ignore the touchpad while keys are pressed. | `flcomp/config.c`, `flcomp/drm.c` |

### 1e. Keyboard

| Key | Type and allowed values | Default | What it does | Read by |
| --- | --- | --- | --- | --- |
| `keyboard` | `de`, `us`, `uk`, `fr` | From `/etc/keyboard.conf`, or `us` when that is missing | Keyboard layout the person picked in Settings. It overrides `LAYOUT` in `/etc/keyboard.conf` for `flcomp`. `uk` is passed on to the keyboard system as `gb`. | `flcomp/seat.c`, `flsettings/pages.c`, `flui/kbd.c` |
| `key_repeat_delay` | Milliseconds. `flcomp` clamps 100 to 2000. | `600` | How long a key must be held before it repeats. Settings offers 150 to 1000. | `flcomp/config.c`, `flcomp/seat.c`, `flsettings/pages.c` |
| `key_repeat_rate` | Keys per second. `flcomp` clamps 1 to 100. | `25` | How fast a held key repeats. Settings offers 2 to 60. | `flcomp/config.c`, `flcomp/seat.c`, `flsettings/pages.c` |
| `numlock` | `0` or `1` | `0` | Num Lock is on when the desktop starts. | `flcomp/config.c`, `flcomp/seat.c` |

The keyboard layout is applied once at the start of the desktop session, so a hand edit to `keyboard=` takes effect at the next login (**not verified** that a live edit works).

### 1f. Displays and brightness

| Key | Type and allowed values | Default | What it does | Read by |
| --- | --- | --- | --- | --- |
| `output.NAME` | `mode=WIDTHxHEIGHT@HZ scale=S pos=X,Y enabled=0\|1 primary=0\|1` (one line) | not set | Saved state of one display. `NAME` is the connector name the kernel uses. The value is written by `flcomp` after a change in Settings. | `flcomp/output.c` |
| `mode` | `WIDTHxHEIGHT` or `WIDTHxHEIGHT@HZ` | not set (the screen's preferred mode) | Old way of setting the main display's mode. Used only while the main display has no `output.NAME` line. | `flcomp/config.c`, `flcomp/output.c` |
| `scale` | Percent, from 100 to 200 | `100` | Old way of setting the main display's scale. Used only while the main display has no `output.NAME` line. Settings offers 100, 125, 150 and 200. | `flcomp/config.c`, `flcomp/output.c` |
| `brightness` | Percent, from 0 to 100 | not set | Screen brightness. `flcomp` sets it on the backlight at start. The brightness keys and Settings update it. | `flcomp/config.c`, `flcomp/media.c`, `flsettings/pages.c` |

Changes to `output.NAME` are read at start and when a display is plugged in or removed. `mode` and `scale` are read only at start. To change a display while the desktop runs, use Settings, which talks to `flcomp` directly (the `fldisplay` tool). Hand edits to these lines apply at the next start.

### 1g. Night light

| Key | Type and allowed values | Default | What it does | Read by |
| --- | --- | --- | --- | --- |
| `nightlight` | `off`, `on`, `auto` | not set (falls back to `night_light`) | Night light. Written by Settings. If set, it wins over `night_light`. The panel reads it for its night light switch. | `flcomp/config.c`, `flpanel/quick.c`, `flsettings/display.c` |
| `night_light` | `0`, `1`, or `auto` | `0` | Older switch, kept for compatibility. `auto` means the same as `nightlight=auto`. | `flcomp/config.c`, `flsettings/display.c` |
| `nightlight_temp` (also `night_light_temp`) | Kelvin, from 2700 to 6500 | `4000` | Colour temperature of the warm screen. Lower is warmer. | `flcomp/config.c`, `flsettings/display.c` |
| `night_warmth` | Number from 0 to 100 | `50` | Strength of the warm filter. Written by Settings from the temperature. | `flcomp/config.c`, `flcomp/nightlight.c` |
| `nightlight_from` (also `night_light_from`) | `HH:MM`, 24-hour | `20:00` | Start of the warm period in `auto` mode. | `flcomp/config.c`, `flsettings/display.c` |
| `nightlight_to` (also `night_light_to`) | `HH:MM`, 24-hour | `07:00` | End of the warm period in `auto` mode. | `flcomp/config.c`, `flsettings/display.c` |

### 1h. Other keys

| Key | Type and allowed values | Default | What it does | Read by |
| --- | --- | --- | --- | --- |
| `anim_log` | `0` or `1` | `0` | Logs animation timing to stderr (for debugging). | `flcomp/config.c`, `flcomp/anim.c` |

**Example** (a complete small file):

```
# ~/.config/freelfs/desktop.conf
theme=dark
accent=teal
font_size=14
panel_position=top
workspaces=3x2
night_light=auto
nightlight_from=21:00
nightlight_to=06:30
output.HDMI-A-1=mode=1920x1080@60 scale=1 pos=0,0 enabled=1 primary=1
```

### Summary of when `desktop.conf` changes apply

- **At once (live):** theme, accent, font and font size, buttons, titlebar, panel position and height, animations, pointer and touchpad settings, key repeat, Num Lock, night light, power saving, performance mode, workspaces, sounds, notification do-not-disturb, panel labels, clock format, wallpaper, icon theme (panel icons on the next draw).
- **At the next start of the program:** `mode` and `scale`, `output.NAME` (until a display is plugged in or removed), `keyboard` (at login), `brightness` (set by flcomp at start).
- **At the next start of FreeBWS:** every FreeBWS key. FreeBWS does not watch this file; its own `~/.config/freebws/config` is a separate file (see Apps).

---

## 2. Night light, outputs and brightness

These keys are inside `desktop.conf`. They are described in section 1f and 1g. The main points:

- Night light has two switches. `nightlight` wins if it is set. Settings writes both, so a hand edit to one should be checked against the other.
- `output.NAME` holds the full state of one display. The old `mode=` and `scale=` keys apply to the main display only until it has its own `output.NAME` line.
- `brightness` is written to the backlight's sysfs file at start. The group `seat` may write that file through the rule in `src/flsettings/90-flsettings-backlight.rules`.

---

## 3. flcomp environment variables and session start

`flcomp` is the compositor. It reads `desktop.conf` (section 1) and these environment variables. Most are for testing; the ones marked **normal use** matter for everyday users too. The full list of variables read by `flcomp` is in `src/flcomp/README` and the source (`getenv` calls).

| Variable | Default | What it does | Source |
| --- | --- | --- | --- |
| `FLUI_CONF` | `~/.config/freelfs/desktop.conf` | Replaces the path of `desktop.conf`. Read by `flcomp` and the UI library. | `flcomp/config.c`, `flcomp/theme.c` |
| `FLCOMP_KEYBOARD_CONF` | `/etc/keyboard.conf` | Keyboard file to use instead. | `flcomp/seat.c` |
| `FLCOMP_BACKLIGHT` | `/sys/class/backlight/*` | Backlight directory for `brightness=` at start (tests). | `flcomp/config.c` |
| `FLCOMP_SHARE` | `/usr/share/flcomp` | Folder with the wallpapers and other shipped art. | `flcomp/theme.c` |
| `FLCOMP_WALLPAPER` | not set | Overrides the wallpaper file. | `flcomp/render.c` |
| `FLCOMP_NO_SOUND` | not set | Set to any value to silence system sounds. | `flcomp/sound.c` |
| `FLCOMP_NO_GL` | not set | Set to any value to turn off the GPU path. | `flcomp/gl.c` |
| `FLCOMP_RENDER_NODE` | not set | Render node to use for the GPU path (for example `/dev/dri/renderD128`). | `flcomp/gl.c` |
| `FLCOMP_NO_CLIPHIST` | not set | Set to `1` to turn off clipboard history. | `flcomp/seat.c` |
| `FLCOMP_SNAPPEL` | not verified | Screenshot program for Super+Shift+S. | `flcomp/shot.c` |
| `FLCOMP_LOCK` | `fllock` | Lock program for Super+L, the lid and restarts. | `flcomp` README |
| `FLCOMP_PANEL` | not verified | Panel program. Empty for none. | `flcomp` README |
| `FLCOMP_SETUP` | `flsetup` | Program for `--firstboot`. The login screen sets this to its own UI. | `flcomp` README |
| `FLCOMP_SESSION` | `/usr/lib/flcomp/session` | Session services script. Empty for none. | `flcomp` README |
| `FLCOMP_ANIM_LOG` | not set | Logs animation frames (same as `anim_log=1`). | `flcomp/anim.c` |
| `FLCOMP_WINLOG` | on | Window log lines (`0` turns them off). | `flcomp/xdg.c` |
| `FLCOMP_XDEBUG` | not set | Logs the X11 bridge (XWayland). | `flcomp/xwm.c` |
| `FLCOMP_XWAYLAND_LOG_STDERR` | not set | Sends XWayland's own log to stderr. | `flcomp/xwm.c` |
| `FLCOMP_NO_XWAYLAND` | not set | Turns off the X11 bridge. | `flcomp` README |
| `FLCOMP_XWAYLAND` | not verified | Path of another XWayland program. | `flcomp` README |
| `FLCOMP_SCREENSHOT` | not set | Path for a PPM screenshot written on `SIGUSR1`. | `flcomp` README |
| `FLCOMP_HEADLESS` | not set | Runs with no display and no input devices, `WxH` (tests). | `flcomp/main.c` |
| `FLCOMP_HEADLESS_OUTPUTS` | not set | Number of fake outputs in headless mode (tests). | `flcomp` README |
| `FLCOMP_TEST_FIFO` | not set | Feeds fake input events (tests). | `flcomp` README |
| `FLCOMP_TRUST_ANY` | not set | Lifts the check on which clients may change the display (tests). | `flcomp/flshell.c` |

**Session start.** After the panel starts, `flcomp` runs `/usr/lib/flcomp/session` as the person. That script starts, in order, the session bus (`dbus-daemon --session`), `pipewire`, `wireplumber`, `pipewire-pulse`, `flupdate-notify` and `flsnap-notice`. It also adds the Flathub remote for Flatpak apps (once, per person, not on the live system) and runs the startup apps (section 4). Its log is `$XDG_RUNTIME_DIR/session.log`. Each daemon is optional: a missing one is skipped.

The script sets `XDG_CURRENT_DESKTOP=FreeLFS`, which the portal uses to pick FreeLFS's portal backend (section 6).

Note: `src/flcomp/README` says `/etc/profile.d/desktop.sh` exports `XDG_RUNTIME_DIR`. **Not verified:** no such file exists in `config/` or the recipes. The login screen sets `XDG_RUNTIME_DIR` to `/run/user/UID`.

---

## 4. Startup apps (autostart)

**Path.** Per-user: `$XDG_CONFIG_HOME/autostart/*.desktop` (or `~/.config/autostart/`). System: `/etc/xdg/autostart/*.desktop` (or every folder in `XDG_CONFIG_DIRS`). A file with the same name in the per-user folder wins over the system file.

**Format.** Standard XDG `.desktop` files. Only the `[Desktop Entry]` section is read.

**Who reads it.** The session script (`src/flcomp/session`), three seconds after login. General Manager (`src/flgm/startup.c`) edits the same files from its Startup apps tab.

**Keys the session script understands:**

| Key | Effect |
| --- | --- |
| `Exec` | The command to run. Field codes such as `%U` are removed. |
| `TryExec` | If set, the program must exist, or the entry is skipped. |
| `Type` | Only `Application` entries run. |
| `Hidden=true` | Skips the entry. |
| `X-GNOME-Autostart-enabled=false` | Skips the entry. |
| `X-FreeLFS-Override=true` | Skips the entry (General Manager writes this to turn off a system entry). |
| `OnlyShowIn=` | Skips the entry unless the list includes `FreeLFS`. |
| `NotShowIn=` | Skips the entry if the list includes `FreeLFS`. |

**When it applies.** At the next login. Nothing is started until the session script runs.

**Example.** To stop a system entry from starting, add this per-user file:

```
# ~/.config/autostart/some-app.desktop
[Desktop Entry]
Type=Application
Name=Some app
Hidden=true
```

---

## 5. GTK colour and font hints (`settings.ini`)

GTK 3 and GTK 4 apps (and Flatpak apps that read these files) get the colour scheme and font from two files that `fltheme` writes.

**Path.** `~/.config/gtk-3.0/settings.ini` and `~/.config/gtk-4.0/settings.ini`.

**Keys written** (in the `[Settings]` section only; other lines are not touched):

| Key | Value | Taken from |
| --- | --- | --- |
| `gtk-application-prefer-dark-theme` | `true` or `false` | `theme=` in `desktop.conf` |
| `gtk-font-name` | Font name and size in points, for example `IBM Plex Sans 10` | `font=` and `font_size=` (pixels converted to points at 96 dpi) |

The writer is `src/fltheme/hints.c` (`hints_write`). It is called by `fltheme` and its presets. Qt apps are not covered.

Flatpak apps can also ask the portal for the colour scheme; see section 6.

---

## 6. Desktop portal configuration

xdg-desktop-portal lets sandboxed apps (Flatpak) ask the desktop for a file, a notification, a screenshot or the colour scheme. FreeLFS provides its own backend, `flportal`.

**Files shipped** (system-wide, from `src/flportal/`):

- `/usr/share/xdg-desktop-portal/freelfs-portals.conf`, which picks `flportal` for everything it implements. A copy is also installed as `portals.conf`, so sessions that do not set `XDG_CURRENT_DESKTOP` still work.
- `/usr/share/xdg-desktop-portal/portals/freelfs.portal`, which lists the interfaces that `flportal` provides: FileChooser, AppChooser, Access, Notification, Settings and Screenshot.
- `/usr/share/dbus-1/services/org.freedesktop.impl.portal.desktop.freelfs.service`, which starts `flportal` on first use.

**Keys in `freelfs-portals.conf`:**

| Section and key | Value | Meaning |
| --- | --- | --- |
| `[preferred]` `default` | `freelfs` | Use the FreeLFS backend for everything it implements. |

**Keys in `freelfs.portal`:** `DBusName`, `Interfaces` (a semicolon list) and `UseIn=FreeLFS`. These are fixed; do not edit them.

**What `flportal` reads from `desktop.conf`:**

- `theme` and `accent` for the `org.freedesktop.appearance` setting (colour scheme 1 for dark, 2 for light; accent colour from `accent=`).
- `desktop.conf` itself is watched, so a change in Settings sends `SettingChanged`.

**Environment variables:** `FLPORTAL_VERBOSE` (log requests), `FLPORTAL_SNAPPEL` (screenshot program), `FLPORTAL_ANY_SENDER` (accept calls from any sender, for tests), `FLPORTAL_SHOT`, `FLPORTAL_TEST_ACCEPT`, `FLPORTAL_TEST_SELECT`, `FLPORTAL_TEST_NAME`, `FLPORTAL_TEST_OVERLAY` (dialog tests only).

**Per-user override.** The standard xdg-desktop-portal also reads per-user `portals.conf` files in `~/.config/xdg-desktop-portal/`. **Not verified** against the FreeLFS build; the repository does not ship such a file.

**When it applies.** `xdg-desktop-portal` reads these files when it starts. **Not verified** whether a change needs a session restart; a log-out and log-in is the safe choice.

---

# System

## 7. `/etc/rc.conf` and the boot services

**Path.** `/etc/rc.conf` (system only). Installed from `config/etc/rc.conf` by `build/40-system-config.sh`.

**Who reads it.** The boot script `/etc/rc.d/rc.sysinit`, the runlevel script `/etc/rc.d/rc`, the shutdown script `/etc/rc.d/rc.shutdown`, the network service (`/etc/rc.d/init.d/network`), the NTP service (reads `/etc/fltime.conf`), and `/etc/rc.d/functions`.

**Format.** A POSIX shell file. Each setting is a shell variable. Arrays use `NAME=( ... )`.

**Keys:**

| Key | Type and allowed values | Default in the repository | What it does |
| --- | --- | --- | --- |
| `HARDWARECLOCK` | `UTC` or `localtime` | `UTC` | Whether the hardware clock keeps UTC or local time. Used at boot and at shutdown. |
| `KEYMAP` | A keymap name (for example `de`), or empty | empty | Console keymap, loaded with `loadkeys`. Empty means: use `LAYOUT` from `/etc/keyboard.conf`. |
| `CONSOLEFONT` | A console font name, or empty | empty | Font for the text terminals (`setfont`). Empty keeps the kernel font. |
| `MODULES` | Array of kernel module names | `()` (none) | Modules loaded at boot, in order (`modprobe`). |
| `SERVICES` | Array of service names | `(syslog dbus network seatd iwd bluetooth cups ntp)` | Services started at boot, in this order, and stopped in reverse order at shutdown. A name prefixed with `!` is disabled. |
| `NETWORK` | Array of strings | `()` (empty) | Interfaces to configure. Each entry is `"IFACE dhcp"` or `"IFACE static ADDRESS/PREFIX GATEWAY"`. Empty means DHCP on every interface. |

**Enabling and disabling a service.** A service is a script in `/etc/rc.d/init.d/`. To turn one on, add its name to `SERVICES`. To turn one off, either remove it or prefix it with `!`:

```
SERVICES=(syslog dbus network seatd iwd !bluetooth cups ntp)
```

Here `bluetooth` is listed but disabled. The rc script skips it at start and at stop (`enabled_services` in `functions`).

**The services shipped**, all in `config/etc/rc.d/init.d/`:

| Service | Purpose | Notes |
| --- | --- | --- |
| `syslog` | System logger (`syslogd`) | |
| `dbus` | D-Bus system bus | |
| `network` | Wired networking | Reads `NETWORK` from `rc.conf`. |
| `seatd` | Seat manager, which gives the screen and input devices to the person at the machine | Group `seat`. |
| `iwd` | Wi-Fi | Logs to `/var/log/iwd.log`. |
| `bluetooth` | `bluetoothd`, only while an adapter exists | Started by udev when an adapter appears. |
| `cups` | Printing, on demand | Runs `flsocketd` (section 27). |
| `ntp` | Clock sync (`fl-sntp`) | Reads `/etc/fltime.conf` (section 11). |

Each service accepts `start`, `stop`, `restart` and `status` (the `daemon_service` helper). Bluetooth also accepts `maybe-stop`, which udev calls.

**Optional local script.** `/etc/rc.local` is run at the end of the start-up runlevel if it exists and is executable. The repository does not ship it.

**When it applies.** The boot scripts read `rc.conf` at boot and at shutdown. A change to `SERVICES` or `NETWORK` takes effect at the next boot.

**Example.** A laptop with no Bluetooth and a fixed address on the wired port:

```
SERVICES=(syslog dbus network seatd iwd !bluetooth cups ntp)
NETWORK=(
	"eth0 static 192.168.1.10/24 192.168.1.1"
)
```

---

## 8. `/etc/inittab` and the terminals

**Path.** `/etc/inittab` (system only). From `config/etc/inittab`.

**Runlevels.** `0` halt, `1` single user, `3` multi-user with consoles (the default, set by `id:3:initdefault:`), `5` graphical (later), `6` reboot.

**Terminals.** Terminal `tty1` starts the graphical login (`flgreet`), and falls back to `agetty` (through `flsplash --text`) when `flgreet` is not installed. `tty2` to `tty6` run `agetty`. The serial port entry `S0` runs `/etc/rc.d/serial-getty`, which only starts a getty when a real serial port exists.

**Keys.** The file uses the standard inittab format: `id:runlevels:action:process`. To change the number of text consoles, add or remove `agetty` lines.

**When it applies.** At the next boot. `init` reads the file when it starts.

---

## 9. `/etc/fstab` and the btrfs subvolume layout

**Path.** `/etc/fstab` (system only). From `config/etc/fstab`.

**Format.** The standard six-column fstab: device, mount point, type, options, dump, pass.

**The btrfs layout.** The root file system is one btrfs partition, labelled `freelfs`, with three subvolumes:

| Subvolume | Mounted at | Notes |
| --- | --- | --- |
| `@` | `/` | The default subvolume. Snapshots are copies of this. |
| `@home` | `/home` | People's home folders. Mounted with `nofail`. |
| `@snapshots` | `/.snapshots` | Read-only snapshots of `/`. Mounted with `nofail`. |

The EFI system partition (FAT, labelled `ESP`) is mounted at `/boot`. It holds the kernels and the boot loader (section 18).

**The entries in the shipped file:**

| Device | Mount point | Type | Options (short) |
| --- | --- | --- | --- |
| `LABEL=freelfs` | `/` | `btrfs` | `defaults,noatime,compress=zstd:1` |
| `LABEL=freelfs` | `/home` | `btrfs` | `defaults,noatime,compress=zstd:1,subvol=@home,nofail` |
| `LABEL=freelfs` | `/.snapshots` | `btrfs` | `defaults,noatime,subvol=@snapshots,nofail` |
| `LABEL=ESP` | `/boot` | `vfat` | `defaults,umask=0022,quiet,nofail` |
| `proc`, `sysfs`, `devpts`, `devtmpfs`, `tmpfs` (`/run`, `/dev/shm`, `/tmp`), `cgroup2` | system | | The kernel file systems. The boot script also mounts them itself. |

**Keys.** There are no FreeLFS-specific keys; these are standard fstab options. The `subvol=` option picks the subvolume. `compress=zstd:1` is the compression level used for new data.

**When it applies.** At the next boot (the boot script runs `mount -a` for local file systems). A new line is not used until then, or until it is mounted by hand.

**Example.** To keep a data disk mounted at `/srv/data`:

```
LABEL=data  /srv/data  ext4  defaults,noatime,nofail  0  2
```

---

## 10. Snapshots (`/etc/flsnap.conf`)

**Path.** `/etc/flsnap.conf` (system only). From `src/flsnap/flsnap.conf`.

**Format.** `key = value`, with `#` comments.

**Keys:**

| Key | Type and allowed values | Default | What it does |
| --- | --- | --- | --- |
| `keep` | Whole number from 1 to 99 | `8` | How many snapshots to keep. The oldest go first. |
| `device` | Any value without spaces, such as `LABEL=freelfs` | not set | The file system holding the snapshots, when `/.snapshots` is not a mount of it. |

**Who reads it.** `flsnap` (`src/flsnap/flsnap.c`) and its pre-transaction hook (section 17), which `flpkg` runs before every change. The flpkg setting `snapshots` (section 15) decides whether that hook runs at all.

**When it applies.** `flsnap` reads the file each time it runs, so the next snapshot uses the new value.

**Example.** Keep the last 20 snapshots:

```
keep = 20
```

---

## 11. Time (`/etc/fltime.conf`)

**Path.** `/etc/fltime.conf` (system only). Written by `fltime-set` when the person changes the setting in Settings.

**Format.** Shell variable lines.

**Keys:**

| Key | Type and allowed values | Default | What it does |
| --- | --- | --- | --- |
| `NTP` | `0` or `1` | `1` (on when the line is missing) | Set the clock from a time server at boot. `0` turns it off. |
| `SERVERS` | Quoted, space-separated host names | `pool.ntp.org` | Time servers to ask, in order. |

**Who reads it.** The `ntp` service script (`config/etc/rc.d/init.d/ntp`) and the `fl-sntp` program (`src/fltime/fl-sntp.c`).

**When it applies.** The `ntp` service reads it when it starts. Settings restarts or stops the service at once when the switch changes. A change to `SERVERS` by hand takes effect when the service next starts.

**Example:**

```
NTP=1
SERVERS="time.example.org 0.pool.ntp.org"
```

---

## 12. Shell and login files

These files are shipped as they are. Nothing in FreeLFS reads them as settings except where noted.

| File | What it is | Notes |
| --- | --- | --- |
| `/etc/profile` | Login shell setup for every user | Sets `PATH`, the language, history size, `EDITOR=vim`, `PAGER=less`, `umask 022`, and the prompt for bash. Runs every file in `/etc/profile.d/*.sh`. |
| `/etc/profile.d/*.sh` | Drop-in scripts run by `/etc/profile` | Shipped by recipes, for example the Flatpak one. Add your own here for every user. |
| `/etc/inputrc` | Readline settings (line editing) | Sets the bell to none, enables 8-bit input, and maps Home, End, PageUp and PageDown keys. Read when a shell starts. |
| `/etc/shells` | Valid login shells | Lists `/bin/sh`, `/bin/bash`, `/usr/bin/bash`, `/bin/flsh` and `/usr/bin/flsh`. |
| `/etc/issue` | Text shown before the login prompt | The text in the repository is `FreeLFS 0.1-dev (\l)`, and a development build adds a line about the root login. |
| `/etc/os-release` | Name and version of the system | `ID=freelfs`, `PRETTY_NAME="FreeLFS 0.1-dev"`. Read by tools that show the system name. |
| `/etc/hostname` | Machine name | One line. Set at boot. |
| `/etc/hosts` | Names of the local machine and a few fixed names | Standard format. |
| `/etc/group`, `/etc/passwd` | Groups and users | The shipped files define `root`, the system users and groups, `wheel` (97) and `users` (999). The group `seat`, which desktop users belong to, is not in the shipped `group` file (**not verified** where it is created). |

**When these apply.** `/etc/hostname` at boot. `/etc/profile` and `/etc/inputrc` at each new shell or login. `/etc/shells` when a login is checked against it. `/etc/os-release` and `/etc/issue` when they are read.

---

## 13. Kernel settings (`sysctl`) and the kernel build config

### 13a. `sysctl` files

`rc.sysinit` runs `sysctl --system` at boot. That reads the standard `sysctl.d` folders (among them `/usr/lib/sysctl.d/*.conf` and `/etc/sysctl.d/*.conf`), and then `/etc/sysctl.conf`. A file with the same name in `/etc/sysctl.d` replaces the one in `/usr/lib/sysctl.d`.

- `/etc/sysctl.conf` is created empty by the build (`build/40-system-config.sh`). Add your own settings here.
- `/usr/lib/sysctl.d/70-freelfs-gaming.conf` (shipped by the `gaming-config` package) sets `vm.max_map_count = 1048576`. Many Windows games need it. See section 28.

**Format.** `key = value`, one per line, `#` for comments.

**When it applies.** At boot. To apply by hand, run `sysctl --system` as root.

### 13b. The kernel build config

`config/kernel/freelfs.config` (491 lines) is the kernel configuration used to build the FreeLFS kernel. It is not read at run time. Notable values:

| Option | Value | Why it matters |
| --- | --- | --- |
| `CONFIG_EFI_STUB` | `y` | Lets `flboot` start the kernel directly from the EFI partition. |
| `CONFIG_DEVTMPFS_MOUNT` | `y` | The kernel mounts `/dev` itself. |
| `CONFIG_BTRFS_FS` | `y` | Root file system support. |
| `CONFIG_SQUASHFS`, `CONFIG_OVERLAY_FS` | `y` | The live system's read-only image and its RAM overlay. |
| `CONFIG_NTSYNC` | `y` | Wine and Proton synchronisation (used by games). |
| `CONFIG_LOCALVERSION` | `"-freelfs"` | Suffix on the kernel version. |

**Not verified:** the exact build step that reads this file (expected in `build/41-kernel.sh`), and the value of `CONFIG_HZ`, which the file does not set in the form searched.

---

## 14. File ownership list (`ownership.list`)

**Path.** Installed as `/usr/share/freelfs/ownership.list` from `config/ownership.list`.

**Why it exists.** Some files need an owner other than root, but the build (a rootless chroot) cannot change them. The list records those exceptions. The boot script (`rc.sysinit`, function `fix_owners`) applies them once and again when the list changes. On a live system, `flpkg fix-owners --keep` does the same from the package database.

**Format.** `path uid gid mode`, with numeric ids. Lines starting with `#` are comments.

**Entries in the repository:**

| Path | Owner (uid:gid) | Mode | Reason |
| --- | --- | --- | --- |
| `/var/log/lastlog` | 0:13 | `0664` | Login records, group `utmp`. |
| `/usr/bin/wall` | 0:5 | `2755` | Setgid to group `tty`. |
| `/usr/bin/write` | 0:5 | `2755` | Setgid to group `tty`. |
| `/var/lib/dhcpcd` | 52:52 | `0700` | The privilege-separation user of `dhcpcd`. |

---

# Packages

## 15. `/etc/flpkg.conf` (repositories, keys, cache, transactions)

**Path.** `/etc/flpkg.conf` (system only). The default is written by the `flpkg` recipe (`recipes/flpkg/recipe`). The repository line is filled from `config/flpkg/repo-url`. A different file can be named with `flpkg --config FILE`.

**Format.** Two kinds of line:

- `key = value`, for settings.
- `repo NAME URL`, for each repository. Repositories are searched in the order they appear. The URL can be a path, a `file://` address or an `https://` address.

`#` starts a comment anywhere on a line.

**Keys:**

| Key | Type and allowed values | Default | What it does |
| --- | --- | --- | --- |
| `cache` | Path | `/var/cache/flpkg` | Downloaded package files. |
| `builddir` | Path | `/var/tmp/flpkg` | Scratch space for building packages. |
| `keys` | Path | `/etc/flpkg/keys` | Folder of trusted `*.pub` signing keys. |
| `signkey` | Path | not set | Key used to sign a repository you build yourself. |
| `jobs` | Whole number | number of CPU cores | Parallel jobs for building. |
| `snapshots` | Whole number | `0` when the line is missing; the shipped file sets `1` | `0` turns off the btrfs snapshot taken before every change. Any other value turns it on; how many are kept is set in `/etc/flsnap.conf`. |
| `compression_level` | Whole number | `19` | zstd compression level for packages you build. |
| `backup_config` | `yes` or `no` | `no` | When a package replaces a file under `/etc` that you changed, keep your copy as `FILE.flpkg-save`. |
| `cflags`, `cxxflags`, `ldflags` | Text | not set | Compiler and linker flags for building packages. |
| `signatures` | `required`, `optional` or `never` | `required` | How strictly packages must be signed. `never` skips the check. |
| `source_date_epoch` | Whole number (a Unix time) | `1767225600` (2026-01-01) | Fixed build time, for reproducible packages. The environment variable `SOURCE_DATE_EPOCH` overrides it. |
| `repo NAME URL` | Name and address | the shipped repository | Adds a repository. Repeat the line for more. |

Anything else on a `key =` line produces a warning, not an error.

**Example:**

```
# /etc/flpkg.conf
repo freelfs https://github.com/wasselnl77-hash/freelfs/releases/download/repo-x86_64
keys = /etc/flpkg/keys
signatures = required
backup_config = yes
snapshots = 1
```

**When it applies.** `flpkg` reads the file at the start of each command. A change applies to the next command. There is no daemon.

---

## 16. Signing keys and the repository address

**Keys.** The shipped key is `config/flpkg/keys/freelfs-repo.pub`, installed as `/etc/flpkg/keys/freelfs-repo.pub`. Every `*.pub` file in the keys folder is trusted. Keys learned from a rotation are kept under `/var/lib/flpkg/keys/`.

**Repository address.** `config/flpkg/repo-url` holds the address the shipped `flpkg.conf` uses (a GitHub release). The `flpkg` recipe copies it into `repo freelfs URL`.

**Format of the key file.** The standard minisign public key format (one comment line and one key line).

**When it applies.** Keys are read at each command that checks a signature. The repository address applies at the next `flpkg sync`.

---

## 17. Pre-transaction hooks and the package database

**Hooks.** Before `flpkg` changes anything, it runs every executable in `/usr/libexec/flpkg/pre-transaction.d/`, sorted by name. The FreeLFS hook `10-flsnap` (from the `flsnap` package) takes a btrfs snapshot of `/`. A hook that fails is only a warning.

The hook receives these environment variables: `FLPKG_ROOT`, `FLPKG_SUMMARY` (for example `upgrade foo, remove bar`), `FLPKG_PACKAGES` (a file of `name version` lines) and `FLPKG_SNAPSHOTS` (the value of `snapshots =`).

**Database and journal.** The installed-package database is `/var/lib/flpkg/db.sqlite`. A transaction writes a journal at `/var/lib/flpkg/journal` and takes a lock at `/var/lib/flpkg/lock`. An interrupted transaction is finished or undone the next time `flpkg` runs.

**Environment variables for flpkg:**

| Variable | What it does |
| --- | --- |
| `FLPKG_LIBDIR` | Library folder (default `/usr/lib/flpkg`). |
| `FLPKG_SOURCES` | Program that prints the package sources. |
| `FLPKG_NO_SOURCES` | Skips the sources step. |
| `FLPKG_NATIVE` | Use the native code path. |
| `FLPKG_HOOKDIR` | Hook folder for a `--root` other than `/` (tests). |
| `FLPKG_ASSUME_TTY` | Treat output as a terminal (tests). |
| `FLPKG_LIVE_DIR` | Live folder (default `/run/live`). |
| `FLPKG_TEST_CRASH` | Crash point for tests. |
| `SOURCE_DATE_EPOCH` | Overrides `source_date_epoch`. |

**Command-line options that matter for config:** `--config FILE` (another config file), `--root DIR` (another root), `-v` (verbose).

---

# Boot

## 18. `flboot.conf` and the boot menu

`flboot` is the UEFI boot loader. The firmware starts `EFI/BOOT/BOOTX64.EFI` on the EFI partition (`BOOTAA64.EFI` on arm64). The installer writes `flboot.conf`. The file is on the EFI partition at `\flboot\flboot.conf`, which is `/boot/flboot/flboot.conf` on the running system.

**Format.** One setting per line, `#` starts a comment, spaces and tabs are trimmed.

**Keys:**

| Key | Type and allowed values | Default | What it does |
| --- | --- | --- | --- |
| `timeout` | Seconds. `0` starts the default at once. `-1` waits for a key. | `5` | Countdown before the default entry starts. Any key stops it. |
| `cmdline` | Kernel options, separated by spaces | not set | Kernel command line for every kernel found on the EFI partition. |
| `default` | A kernel version or an entry title | not set | Entry selected when the menu opens. |
| `entry` | `TITLE\|PATH\|OPTIONS` | not set | An extra entry. `PATH` is on the EFI partition, with `\` separators. Repeat for more entries. |

**The installer's starting file** (written only when none exists):

```
# flboot.conf -- see /usr/share/doc/flboot/README
timeout 5
cmdline root=PARTUUID=... rootwait ro quiet panic=30 console=tty0 ...
```

The `root=` option must name the root partition directly (`PARTUUID=`), because the installed system has no initramfs.

**What the menu lists** (from `src/flboot/README`):

- Every kernel on the EFI partition (`/boot/vmlinuz-*`), newest first. The LTS kernel comes after the others as its own entry.
- The entries in `flboot.conf`.
- A "Safe graphics" entry for each kernel: the same command line plus `nomodeset nouveau.modeset=0`. It is left out when an entry already has `nomodeset`.
- "Start an earlier state" (only when snapshots exist), listing the snapshots in `\flboot\snapshots`. That file is written by `flsnap` and is not a setting.
- "Firmware setup (BIOS)" when the firmware supports it.
- "Other drives and systems", listing the firmware boot options and other EFI programs such as Windows' boot manager.

**Keys in the optional `snapshots` file** are not settings. Each line is `SUBVOLUME TITLE`, written by `flsnap`.

**When it applies.** `flboot` reads the file each time it starts, so the next boot. Use the Settings app or `flsnap` for snapshot rollback.

**Example.** A menu that waits for a key, with an extra entry for another Linux install on a second disk:

```
timeout -1
cmdline root=PARTUUID=0a1b2c3d-01 rootflags=subvol=@ rootwait ro quiet
default 6.18
entry Other Linux|\EFI\other\grubx64.efi|
```

---

## 19. Live system: kernel options and `/run/live/*` mounts

The live system (the installer image, on a DVD or a USB stick) is started by `flboot` with `initrd=\initrd.img` on the kernel command line. Its init program `flinitrd` reads these options from the kernel command line:

| Option | Values | Default | What it does |
| --- | --- | --- | --- |
| `flive.timeout=N` | Seconds | `20` | How long to wait for disks. |
| `flive.iso=PATTERN` | File name pattern | `freelfs-live*.iso` | Which ISO file to use on a stick written by Ventoy. |
| `flive.toram=0` or `1` | `1` forces loading into RAM, `0` forbids it | automatic | Copies the system image into RAM and releases the stick. Automatic when RAM is at least the image size plus 4 GiB and the stick has no persistence image. |
| `flive.debug` | (flag) | off | Says what is tried, on the console. |
| `flive.nopersist` | (flag) | not set | Present in the source (`src/flinitrd/flinitrd.c`). **Not verified** what it does beyond disabling persistence. |

Live boot is read-only by construction: each device is set read-only in the kernel before it is mounted.

**The `/run/live/*` mounts.** When the live system runs, these exist:

| Path | What it is |
| --- | --- |
| `/run/live` | The marker directory. Tools test for it to know they run on the live system. |
| `/run/live/src` | The medium the system started from (the stick or disk), mounted read-only. Used by `flmount` and `flpersist`. |
| `/run/live/iso` | The ISO file, loop-mounted (for a Ventoy stick). |
| `/run/live/rom` | The `root.sfs` squashfs image that holds the whole system (read-only). The installer copies from here. |
| `/run/live/rw/up` | The overlay's upper layer. Everything written on the live system goes here, in RAM (a tmpfs). |
| `/run/live/persist` | The persistence image, mounted as `/home` when it exists (see section 20). |
| `/run/live/toram` | Exists only when the system was loaded into RAM. |

**Persistence and what is kept.** Without a persistence image, everything the live user writes stays in RAM and is lost at power-off. The image is covered in section 20.

**Live-only behaviour.** The live system skips the `flathub` setup and does not download the app catalogue (see section 3 and `src/flstore/README`). The installer (`flinstall`) copies from `/run/live/rom`.

---

## 20. Persistence (`freelfs-persist.img`)

**What it is.** A file named `freelfs-persist.img` on the stick's data partition, next to the ISO. At boot, `flinitrd` mounts it as `/home`, so the live user's files survive a reboot. The boot script (`rc.sysinit`, `live_home`) then copies the skeleton home into a new persistence image the first time.

**Creating it.** `flpersist create SIZE [--btrfs]` makes the image on the stick, with an ext4 file system (or btrfs). `SIZE` is written like `512M` or `4G`. `flpersist status` prints `active`, `ready` or `none`.

**Limits.** Only works for a live session started from an ISO file on a stick (Ventoy). A stick written with `dd` has no file system to hold the image.

**When it applies.** The image is used at the next boot. The Software Store reads whether the persistence mount is present (`src/flstore/flstore.c`).

---

## 21. `/etc/freelfs/splash.conf` (boot screen)

**Path.** `/etc/freelfs/splash.conf` (system only). The directory is root-owned. The setting is written by `flsplash-set`, after the person types their password (through `flauth`). The program refuses to write if `/etc/freelfs` is not root-owned.

**Format.** One line per setting.

**Keys:**

| Line | Meaning |
| --- | --- |
| `splash=flat` | The flat boot screen (the default). |
| `splash=legacy` | The older boot screen. |

If the file is missing or has no `splash=` line, the flat screen is used. If there are several `splash=` lines, the last one wins. Other lines are ignored.

**Who reads it.** `flsplash` (`src/flsplash/flsplash.c`), at start. The environment variables `FLSPLASH_CONF` (another file) and `FLSPLASH_SET_DIR` / `FLSPLASH_SET_CONF` (tests of the setter) replace the path.

**When it applies.** At the next boot.

**Example:**

```
splash=legacy
```

---

# Hardware

## 22. `/etc/keyboard.conf`

**Path.** `/etc/keyboard.conf` (system only). Written by the first-boot screen (`flsetup`) and by Settings when the file is writable. If the first-boot screen cannot run, the file is written with `LAYOUT=us`.

**Format.** `NAME=value` lines. Quotes are optional. `#` starts a comment.

**Keys:**

| Key | Type and allowed values | Default | What it does |
| --- | --- | --- | --- |
| `LAYOUT` | A layout name such as `us`, `de`, `fr`, `uk` | `us` | Keyboard layout. `uk` is passed on as `gb`. A comma-separated list keeps the first name only. |
| `VARIANT` | A variant name such as `nodeadkeys` | empty | Layout variant, added after a colon. |
| `OPTIONS` | Keyboard options such as `ctrl:nocaps` | empty | Extra options for the keyboard system. |

**Who reads it.** `flcomp` (`src/flcomp/seat.c`), `libflui` (`src/flui/kbd.c`), and the boot script (`rc.sysinit`). At boot, if `KEYMAP` in `rc.conf` is empty, the boot script uses `LAYOUT` for the console keymap.

**Order of precedence.** The `keyboard=` line in the person's `desktop.conf` (section 1e) wins over `LAYOUT` for the desktop. `KEYMAP` in `rc.conf` wins for the console.

**Environment variables:** `FLCOMP_KEYBOARD_CONF` and `FL_KEYBOARD_CONF` replace the path.

**When it applies.** The desktop reads it at its start. The console reads it at boot. Settings applies a layout change at once.

**Example:**

```
LAYOUT=de
VARIANT=nodeadkeys
OPTIONS=ctrl:nocaps
```

---

## 23. udev rules (`flmount`, backlight, Bluetooth, game controllers)

udev rules decide what happens when a device appears or goes away. The four rule files in the repository are:

| File | Source | Purpose |
| --- | --- | --- |
| `90-flmount.rules` | `src/flmount/` | Mounts a removable partition or disk when it appears (`flmount --hotplug`), and cleans up when it is pulled (`flmount --gone`). Internal disks are left alone. |
| `90-flsettings-backlight.rules` | `src/flsettings/` | Gives group `seat` write access to the backlight brightness file, so Settings can change the brightness. |
| `90-flbluetooth.rules` | `config/bluez/` | Starts `bluetoothd` when a Bluetooth adapter (`hci*`) appears, and stops it when the adapter goes away. Installed by the `bluez` recipe. |
| `71-freelfs-gaming.rules` | `recipes/gaming-config/` | Gives group `seat` access to game controllers, the Steam virtual device nodes (`uinput`, `uhid`), and the `ntsync` device used by Proton. |

**Who controls the access.** FreeLFS has no logind, so there are no per-user ACLs. Desktop users are in group `seat`, and the rules give that group access. The group list for a new user is `seat`, `audio`, `video` and `render`.

**When they apply.** The rules run when a device appears. A change to a rule file takes effect at the next device event or after udev reloads its rules (**not verified** that a reload is done without a reboot).

**Editing.** These files are under the system's udev rules folder. Put your own rule files in `/etc/udev/rules.d/` (standard udev behaviour).

---

## 24. Removable drives (`flmount`)

`flmount` mounts, unmounts and ejects removable drives for the person at the machine. It runs setuid root for group `seat`.

**Config file.** None. Its behaviour comes from the command line and the rules above.

**Mount points.** Drives are mounted under `/drives/LABEL` (the label is made safe for a file name). The live boot stick is bound to `/drives/LABEL` and called "FreeLFS boot stick" in the list.

**Mount options.** Mounts are `nosuid,nodev,noexec`. FAT, exFAT and NTFS are given the caller's user and group. On ext4 and btrfs the top folder goes to the caller when root owns it.

**Commands:**

| Command | What it does |
| --- | --- |
| `flmount list [--internal]` | One line per partition: device, label, file system, size, mount point, class. |
| `flmount mount DEV` | Mounts a partition. Prints the mount point. |
| `flmount unmount DEV` | Unmounts. The boot stick is refused. |
| `flmount remount DEV` | Unmounts and mounts again (lazily). |
| `flmount eject DEV` | Unmounts every partition on the disk and powers it off. |
| `flmount --hotplug DEV`, `--auto`, `--gone DEV` | Root only, used by udev and the boot script. |

**When it applies.** On the command.

---

## 25. Bluetooth

**Service.** `/etc/rc.d/init.d/bluetooth`, listed in `SERVICES` in `rc.conf`. At boot it starts `bluetoothd` only when an adapter is present. udev starts or stops it when an adapter appears or goes away (section 23).

**Pairing agent.** The service starts a default agent (`NoInputNoOutput`), so headphones, speakers and mice pair without a PIN prompt.

**Storage.** Paired devices are stored under `/var/lib/bluetooth`. Log: `/var/log/bluetooth.log`. Both are standard bluez locations.

**D-Bus policy.** `config/bluez/seat-dbus.conf`, installed by the `bluez` recipe as `/usr/share/dbus-1/system.d/bluetooth-seat.conf`. It lets every user in group `seat` talk to `org.bluez`, so Settings works without a password. It also lets bluez and the audio server call back into the sender.

**bluez's own configuration** (`/etc/bluetooth/main.conf`) is not shipped in the repository. **Not verified** whether the recipe installs one; the defaults of bluez apply.

---

## 26. PipeWire (sound)

**No configuration file is shipped.** The `pipewire` and `wireplumber` recipes build with stock settings (`recipes/pipewire/recipe`, `recipes/wireplumber/recipe`). The build turns off codecs that need extra libraries; SBC is on, so headsets work.

**How it starts.** The session script (`src/flcomp/session`) starts `pipewire`, then `wireplumber` (the policy manager) and `pipewire-pulse` (for apps that speak PulseAudio). Each one is optional.

**Where to change it.** PipeWire and WirePlumber come with their own stock configuration from the upstream packages. This repository adds no override files. Per-user overrides in `~/.config/pipewire/` and `~/.config/wireplumber/` are the usual place for these programs (**not verified** against the built packages).

**The FreeLFS setting that touches sound** is `sounds=` in `desktop.conf` (section 1b). It turns off system sounds (login, tick, screenshot, error). It does not affect PipeWire.

---

## 27. Printing (CUPS and `flsocketd`)

Printing starts on demand. The `cups` service (`config/etc/rc.d/init.d/cups`) runs `flsocketd`, which owns `/run/cups/cups.sock`. The first connection starts `cupsd` (through `flcupsd`, which also starts `avahi-daemon` for printer discovery). When nothing has printed or connected for 60 seconds, `cupsd` exits.

**`flsocketd`.** Its usage is `flsocketd [-m MODE] [-g GROUP] SOCKET -- COMMAND [ARGS...]`. The service starts it with `-m 0666` (anyone may connect) and the socket path `/run/cups/cups.sock`. Printing clients such as `lp`, `lpstat`, `lpadmin` and Settings connect to that socket.

**`/etc/cups/cupsd.conf`.** Not in `config/`. The `cups` recipe edits the stock file:

| Directive | Value | Why |
| --- | --- | --- |
| `IdleExitTimeout` | `60` | Exit after 60 seconds without jobs. |
| `Browsing` | `No` | No printer broadcasting on the network. |

**Converters.** `config/cups/freelfs.types` and `config/cups/freelfs.convs` (installed under `/usr/share/cups/mime/`) say how PDF is turned into what a printer takes:

- `pdftopdf` (`config/cups/pdftopdf`) sends a PDF unchanged to printers that take PDF (IPP Everywhere, AirPrint). It does no page fitting.
- `pdftopwg` (`config/cups/pdftopwg`) draws a PDF with `mutool` at 300 dpi for printers that only take PWG raster.

**When it applies.** `flsocketd` and `cupsd` read their settings when they start. Because `cupsd` starts on demand, a change to `cupsd.conf` applies the next time it starts (**not verified** exactly when that is).

---

## 28. Game settings

Two files help games. Both are shipped by the `gaming-config` recipe:

- `/usr/lib/sysctl.d/70-freelfs-gaming.conf`: `vm.max_map_count = 1048576`. Many Windows games (and Wine) map more memory areas than the kernel's default of 65530 allows. This setting matches Fedora, Arch and SteamOS. Applied at boot by `sysctl --system`.
- `/usr/lib/udev/rules.d/71-freelfs-gaming.rules`: game controllers, Steam Input virtual devices and `ntsync` go to group `seat`. See section 23.

The `flgaming` helper checks that the sysctl value is set and reports when it is not.

---

# Network

## 29. Wi-Fi (`iwd`)

**Main settings file.** `/etc/iwd/main.conf` (system only). From `config/iwd/main.conf`, installed by the `iwd` recipe.

**Format.** Standard iwd INI format with `[Section]` headers.

**Keys:**

| Section and key | Value in the repository | Meaning |
| --- | --- | --- |
| `[General]` `EnableNetworkConfiguration` | `true` | iwd runs its own DHCP client on Wi-Fi interfaces. |
| `[Network]` `NameResolvingService` | `resolvconf` | iwd calls `resolvconf` to set the name servers (see section 31). |

**Files around it:**

| File | What it does |
| --- | --- |
| `/usr/lib/iwd/bin/resolvconf` | Stand-in for `resolvconf`. It writes `/etc/resolv.conf` from the calls iwd makes. The service puts this folder first in its path, so `dhcpcd` never sees it. |
| `/etc/dhcpcd.conf` | Ends with `denyinterfaces wl*`, so `dhcpcd` leaves Wi-Fi interfaces alone. |
| `/usr/share/dbus-1/system.d/iwd-seat.conf` | D-Bus policy: group `seat` may talk to `net.connman.iwd` (from `config/iwd/seat-dbus.conf`). |
| `/var/lib/iwd/` | Known networks and their saved passwords. The installer copies these from an earlier install. |
| `/var/log/iwd.log` | Log of association, DHCP and DNS. |

**When it applies.** iwd reads `main.conf` when it starts. A change applies after the `iwd` service restarts (**not verified** whether iwd reloads on a signal).

**Example:**

```
[General]
EnableNetworkConfiguration=true

[Network]
NameResolvingService=resolvconf
```

---

## 30. Wired networking (`dhcpcd`, the `network` service)

**Service.** `/etc/rc.d/init.d/network`, listed in `SERVICES`. The settings are the `NETWORK` array in `/etc/rc.conf` (section 7).

**With `NETWORK` empty**, `dhcpcd` gets an address on every interface.

**With entries**, each one is applied in order:

- `"IFACE dhcp"` runs `dhcpcd` on that interface.
- `"IFACE static ADDRESS/PREFIX GATEWAY"` sets the address and an optional default route with `ip`.

**`/etc/dhcpcd.conf`.** Shipped by the `dhcpcd` recipe with the `denyinterfaces wl*` line (Wi-Fi is left to iwd). Other `dhcpcd` options use the stock defaults.

**When it applies.** At the next boot, or when the `network` service is restarted.

---

## 31. Name resolution (`/etc/resolv.conf`)

`/etc/resolv.conf` is written by:

- `dhcpcd` for wired interfaces.
- The `resolvconf` stand-in (section 29) for Wi-Fi. Its output starts with the line `# generated by iwd (FreeLFS resolvconf helper)`, so the stand-in only removes a file it wrote.

**Hosts.** `/etc/hosts` (shipped with the system, section 12) holds fixed host names.

**Not configurable in the repository.** `/etc/nsswitch.conf` is named by the build scripts. **Not verified** whether a file is shipped or what it contains.

---

# Apps

## 32. FreeBWS (`~/.config/freebws/config`, ad block lists, sandbox)

FreeBWS is the web browser. It has its own settings file, separate from `desktop.conf`.

### 32a. The settings file

**Path.** `$XDG_CONFIG_HOME/freebws/config`, or `~/.config/freebws/config`. The folder is made with mode `0700`.

**Format.** `key=value`, one per line, `#` for comments. Settings made in the browser rewrite the file with its keys sorted and a header line `# FreeBWS settings (written by the browser)`, so hand comments are lost after a change in the browser.

**When it applies.** FreeBWS reads the file when it starts. **Changes need a restart of the browser.** Settings changed inside the browser are saved at once and used at once.

**Keys:**

| Key | Type and allowed values | Default | What it does |
| --- | --- | --- | --- |
| `search` | `brave`, `duckduckgo`, `google`, `startpage`, `bing`, `ecosia` | `brave` | Search engine for the address bar and the start page. |
| `downloads_dir` | Path (`~` is expanded) | `~/downloads` | Where downloads are saved. |
| `adblock` | `0` or `1` | `1` | Ad blocker on or off. |
| `adblock_off` | Comma-separated host names | empty | Sites where the ad blocker is off (set by the Shield button). |
| `bookmarks_bar` | `0` or `1` | `1` | Shows the bookmarks bar. |
| `dark` | `0` or `1` | `0` | Dark mode for web pages (the global switch in Settings). |
| `dark_on` | Comma-separated host names | empty | Sites that always get dark mode. |
| `dark_off` | Comma-separated host names | empty | Sites that never get dark mode. |
| `cookies` | `always` or `no-third-party` | `always` | Cookie policy. `no-third-party` blocks cookies set by other sites (breaks some sign-in flows). `FREEBWS_COOKIES` overrides it. |
| `buffers` | `auto`, `shm`, `dmabuf` | `auto` | How pages are drawn. `auto` uses the GPU when one is announced, else the CPU. `FREEBWS_BUFFERS` overrides it. |
| `sandbox` | `auto`, `on`, `off` | `auto` | Web process sandbox (see 32c). `FREEBWS_SANDBOX` overrides it. |
| `useragent` | Text | the Safari form of the user agent | Replaces the browser's user agent string. |
| `watchdog_test` | `0` or `1` | `0` | Test switch for the watchdog. Not for normal use. |

**Example:**

```
# ~/.config/freebws/config
search=duckduckgo
downloads_dir=~/Downloads
adblock=1
cookies=no-third-party
buffers=auto
sandbox=auto
```

### 32b. Ad block lists

**Lists shipped:** `/usr/share/freebws/lists/lists.conf` names the lists. Each line is tab-separated: name, URL, license, title. Lines starting with `#` are comments. The list files themselves are shipped next to it, with their licences in `/usr/share/freebws/lists/LICENSES`.

**Newer lists:** the weekly update (or `freebws --adblock-update`) downloads newer lists into `~/.local/share/freebws/adblock/lists/`. The browser uses whichever copy is newer.

**Built files:** the converted rules are kept in `~/.local/share/freebws/adblock/built/` (`generic.txt` and `site.tsv`). The first start builds them, which takes about ten seconds.

**Environment variables:**

| Variable | What it does |
| --- | --- |
| `FREEBWS_ADB_SKIP` | Leave out parts of the blocker: `net`, `css`, `js`, `yt` (a comma list). |
| `FREEBWS_ADB_LOG` | Log what the blocker does. |
| `FREEBWS_ADB_UPDATE` | Update the lists now. |

### 32c. The web process sandbox

The web process (the part that runs a page) can run inside bubblewrap, a sandbox with its own user, mount, process and host name namespaces. The `sandbox` key chooses the mode:

| Value | What happens |
| --- | --- |
| `auto` (default) | The sandbox is on when it works. When it does not work, the pages run without a sandbox, with a warning on stderr and on the Settings page. |
| `on` | FreeBWS refuses to start when the sandbox does not work. |
| `off` | The sandbox is off on purpose. A warning is shown. |

`freebws --sandbox-check` prints whether the sandbox can run here (exit 0 means it can).

**Environment variables:** `FREEBWS_SANDBOX` (overrides `sandbox`), `FREEBWS_SANDBOX_RO` (extra read-only folders for the sandbox).

### 32d. Buffers, memory and other test switches

These are environment variables. They are for testing and tuning; the common ones are listed.

| Variable | What it does |
| --- | --- |
| `FREEBWS_BUFFERS` | `shm`, `dmabuf` or `auto`. Overrides `buffers`. |
| `FREEBWS_RENDER_NODE` | GPU render node to use, for example `/dev/dri/renderD129`. |
| `FREEBWS_CACHE` | `viewer`, `document` or `browser`: the cache model. |
| `FREEBWS_FEATURES` | WebKit feature list, `Name=0\|1,...`. Replaces the default list. |
| `FREEBWS_MP` | Memory pressure settings: `limit_mb,conservative,strict,kill,poll_s`. |
| `FREEBWS_DISCARD_IDLE` | Seconds before a background tab is discarded (`0` means never). |
| `FREEBWS_DISCARD_MB` | Free memory, in MB, that triggers discarding. |
| `FREEBWS_SHARE` | `1` puts all tabs in one web process. |
| `FREEBWS_DSSL` | `engine` forces the bundled engine for DSSL pages. |
| `FREEBWS_HOME` | Puts config, data and cache under one folder (tests). |
| `FREEBWS_DATADIR` | Replaces `/usr/share/freebws` (tests). |
| `FREEBWS_EVAL` | Runs a script in each page after three seconds, and logs it (tests). |
| `FREEBWS_DEBUG`, `FREEBWS_POLICY_LOG`, `FREEBWS_HANGKILL`, `FREEBWS_PAGECACHE`, `FREEBWS_POPUP_UNRELATE`, `FREEBWS_ADB_UPDATE` | Diagnostics and experiments (names found in `src/freebws/*.c`; behaviour **not verified** in detail). |

**Where things are stored:**

| Data | Path |
| --- | --- |
| Settings | `~/.config/freebws/config` |
| Bookmarks | `~/.local/share/freebws/bookmarks.json` |
| Downloads | `~/downloads` (or `downloads_dir`) |
| Ad block lists and rules | `~/.local/share/freebws/adblock/` |

---

## 33. Default apps (`mimeapps.list`)

Standard XDG file that says which program opens which file type.

**Paths** (the first one found for each type wins; a later list adds more programs):

| Path | Who wins | Notes |
| --- | --- | --- |
| `~/.config/mimeapps.list` | First | The person's own choices. |
| `/etc/xdg/mimeapps.list` | Second | Administrator choices. |
| `$XDG_DATA_DIRS/applications/mimeapps.list` (for example `/usr/share/applications/mimeapps.list`) | Last | Defaults shipped with FreeLFS. |

**Format.** `[Default Applications]` and `[Added Associations]` sections, with `mime/type=app.desktop;` lines.

**Shipped defaults** (`src/flfiles/mimeapps.list`, installed as `/usr/share/applications/mimeapps.list`):

| Kind | Program |
| --- | --- |
| Images (PNG, JPEG, WebP) | `flview` |
| PDF, EPUB, comic archives, XPS | `flpdf` |
| Audio (MP3, FLAC, Ogg, Opus, AAC, WAV, MP4 audio) | `flmusic` |
| Video (MP4, Matroska, WebM, AVI, QuickTime, MPEG, Ogg) | `flvideo` |
| Text, Markdown, JSON, shell scripts | `fledit` |
| Archives (zip, tar, 7z, RAR, gzip, bzip2, xz, zstd) | `flarchive` |
| Web pages and the web address schemes (`http`, `https`) | `freebws` |

**Who reads it.** `flfiles` (its file-type lookup) and `flpanel` (the "Open with" list). Files are looked up each time an app is chosen.

**When it applies.** At the next time an app is picked for a file. A change to the person's own file takes effect at once.

**Example** (send PDFs to another viewer):

```
# ~/.config/mimeapps.list
[Default Applications]
application/pdf=org.example.Viewer.desktop;
```

---

## 34. Environment variables that every program may read

These are read by many programs. The list below covers the ones that change what a user sees or where files go. Test-only variables are listed in the sections above.

| Variable | Default | Used for | Source |
| --- | --- | --- | --- |
| `HOME` | from the account | Home folder, the fallback for every per-user path. | All |
| `XDG_CONFIG_HOME` | `~/.config` | Per-user configuration (`desktop.conf`, `mimeapps.list`, `autostart`, `freebws/config`). | Many |
| `XDG_DATA_HOME` | `~/.local/share` | Per-user data (wallpapers, bookmarks, ad block lists). | Many |
| `XDG_RUNTIME_DIR` | `/run/user/UID` (set by the login screen) | Sockets and the session log. | `flcomp/session`, `flportal` |
| `XDG_CURRENT_DESKTOP` | `FreeLFS` (set by the session) | Lets the portal pick FreeLFS's backend. | `flcomp/session` |
| `XDG_CONFIG_DIRS` | `/etc/xdg` | System config folders (autostart, mimeapps). | `flcomp/session`, `flfiles` |
| `XDG_DATA_DIRS` | `/usr/share` | System data folders (applications, mime). | `flfiles` |
| `WAYLAND_DISPLAY` | set by the compositor | Wayland socket name. | Clients |
| `DBUS_SESSION_BUS_ADDRESS` | `unix:path=$XDG_RUNTIME_DIR/bus` | Session bus address. | `flcomp/session` |
| `TZ` | not set | Time zone override for the world map. | `flui/worldmap.c` |
| `FL_ZONEINFO`, `FL_TZ_ETC` | `/usr/share/zoneinfo`, `/etc` | Time zone data and the files Settings writes (tests). | `flsettings/datetime.c` |
| `TERM` | set by the terminal | Terminal type. The package builder reads it. | `flpkg/build.c` |
| `TMPDIR` | `/tmp` | Scratch space. | `flpkg/repo.c`, `flsettings/printers.c` |
| `PATH` | set by `/etc/profile` | Program search path. | Several |
| `FLUI_CONF` | `~/.config/freelfs/desktop.conf` | Path of `desktop.conf`. | `libflui`, `flcomp` |
| `FLUI_PPM` | not set | Writes one frame of each window to this PPM file, with no compositor (screenshots and tests). | `libflui` |
| `FLUI_PPM_DELAY` | not set | Milliseconds to wait before `FLUI_PPM` renders (tests). | `libflui` |
| `FL_KEYBOARD_CONF` | `/etc/keyboard.conf` | Keyboard file. | `libflui` |
| `FLPANEL_ROOT`, `FLPANEL_APPS`, `FLPANEL_NOBAT`, `FLPANEL_NO_SOUND`, `FLPANEL_POWER_SUPPLY` | not set | Panel test and override switches (folders, battery, sound). | `flpanel` (`icons.c`, `battery.c`, `notify.c`) |
| `FLSETTINGS_BACKLIGHT` | `/sys/class/backlight/*` | Backlight folder (tests). | `flsettings` |
| `FLSETTINGS_SIZE` | not set | Window size for screenshots (tests). | `flsettings` |
| `FLPORTAL_VERBOSE` | not set | Logs portal requests. | `flportal` |
| `FLSPLASH_CONF` | `/etc/freelfs/splash.conf` | Boot screen setting file. | `flsplash` |
| `FLTIME_CONF`, `FLTIME_SET`, `FLTIME_STATUS` | `/etc/fltime.conf` and tool paths | Time settings (tests). | `flsettings/datetime.c`, `fltime` |
| `FLSNAP_CONF`, `FLSNAP`, `FLSNAP_FLPKG` | `/etc/flsnap.conf` and tool paths | Snapshot settings (tests). | `flsnap/flsnap.c` |
| `FLBOOT_TEXT`, `FLBOOT_FAKE_OTHER` | not set | Boot menu test switches. | `flboot/tests/hostmenu.c` (tests only) |
| `FLSTORE_CONF`, `FLSTORE_LIVE_DIR`, `FLSTORE_SELF`, `FLSTORE_APPSTREAM_URL`, `FLSTORE_ICON_URL` | `/etc/flpkg.conf`, `/run/live`, and the shipped addresses | Software Store settings (tests and mirrors). | `flstore` |
| `FLPERSIST` | not set | Read by the About page to show the persistence state (**meaning not verified** in detail). | `flsettings/about.c` |
| `FLGREET_FD` | set by the login screen | Pipe from the login screen to the compositor. | `flgreet` |
| `GM_INTERVAL_MS` | not set | General Manager refresh interval (tests). | `flgm/gm.c` |
| `FLREPORT` | not set | Report program override. | `flsettings/about.c` |
| `FLAUTH_TRIED` | not set | Internal: marks a password try. | `flsplash-set.c`, `flsnap.c`, `flpkg-seat.c` |
| `FLPOWER_STATE`, `FLPOWER_CPU` | not set | Power state and CPU file overrides (tests). | `flpanel/flpower.c` |
| `SOURCE_DATE_EPOCH` | not set | Fixed build time for packages (see section 15). | `flpkg` |
| `FLPKG_*` | see section 17 | Package manager settings. | `flpkg` |
| `FREEBWS_*` | see section 32 | Browser settings. | `freebws` |
| `FLCOMP_*` | see section 3 | Compositor settings. | `flcomp` |

---

## 35. Every file under `config/` and where it goes

The `config/` folder holds the source of the system's configuration files. The build installs them (section "Where configuration lives at a glance"). This table covers every file in the folder.

| File in `config/` | Installed as | Section of this page |
| --- | --- | --- |
| `bluez/90-flbluetooth.rules` | `/usr/lib/udev/rules.d/90-flbluetooth.rules` | 23, 25 |
| `bluez/seat-dbus.conf` | `/usr/share/dbus-1/system.d/bluetooth-seat.conf` | 25 |
| `cups/flcupsd` | `/usr/libexec/flcupsd` | 27 |
| `cups/freelfs.convs` | `/usr/share/cups/mime/freelfs.convs` | 27 |
| `cups/freelfs.types` | `/usr/share/cups/mime/freelfs.types` | 27 |
| `cups/pdftopdf` | Installed by the `cups` recipe (install path **not verified**) | 27 |
| `cups/pdftopwg` | `/usr/lib/cups/filter/pdftopwg` | 27 |
| `etc/fstab` | `/etc/fstab` | 9 |
| `etc/group` | `/etc/group` | 12 |
| `etc/hostname` | `/etc/hostname` | 12 |
| `etc/hosts` | `/etc/hosts` | 12 |
| `etc/inittab` | `/etc/inittab` | 8 |
| `etc/inputrc` | `/etc/inputrc` | 12 |
| `etc/issue` | `/etc/issue` | 12 |
| `etc/os-release` | `/etc/os-release` | 12 |
| `etc/passwd` | `/etc/passwd` | 12 |
| `etc/profile` | `/etc/profile` | 12 |
| `etc/rc.conf` | `/etc/rc.conf` | 7 |
| `etc/rc.d/functions` | `/etc/rc.d/functions` | 7 |
| `etc/rc.d/rc` | `/etc/rc.d/rc` | 7 |
| `etc/rc.d/rc.shutdown` | `/etc/rc.d/rc.shutdown` | 7 |
| `etc/rc.d/rc.sysinit` | `/etc/rc.d/rc.sysinit` | 7, 13, 22 |
| `etc/rc.d/serial-getty` | `/etc/rc.d/serial-getty` | 8 |
| `etc/rc.d/init.d/bluetooth` | `/etc/rc.d/init.d/bluetooth` | 7, 25 |
| `etc/rc.d/init.d/cups` | `/etc/rc.d/init.d/cups` | 7, 27 |
| `etc/rc.d/init.d/dbus` | `/etc/rc.d/init.d/dbus` | 7 |
| `etc/rc.d/init.d/iwd` | `/etc/rc.d/init.d/iwd` | 7, 29 |
| `etc/rc.d/init.d/network` | `/etc/rc.d/init.d/network` | 7, 30 |
| `etc/rc.d/init.d/ntp` | `/etc/rc.d/init.d/ntp` | 7, 11 |
| `etc/rc.d/init.d/seatd` | `/etc/rc.d/init.d/seatd` | 7 |
| `etc/rc.d/init.d/syslog` | `/etc/rc.d/init.d/syslog` | 7 |
| `etc/shells` | `/etc/shells` | 12 |
| `flpkg/keys/freelfs-repo.pub` | `/etc/flpkg/keys/freelfs-repo.pub` | 16 |
| `flpkg/repo-url` | Copied into the `repo freelfs` line of `/etc/flpkg.conf` | 15, 16 |
| `iwd/main.conf` | `/etc/iwd/main.conf` | 29 |
| `iwd/resolvconf` | `/usr/lib/iwd/bin/resolvconf` | 29, 31 |
| `iwd/seat-dbus.conf` | `/usr/share/dbus-1/system.d/iwd-seat.conf` | 29 |
| `kernel/freelfs.config` | Kernel build input (not installed) | 13b |
| `ownership.list` | `/usr/share/freelfs/ownership.list` | 14 |

**Not under `config/` but part of the same story:**

- Udev rules kept with their programs: `src/flmount/90-flmount.rules` and `src/flsettings/90-flsettings-backlight.rules` (section 23).
- Other files kept with their programs: `src/flcomp/session` (section 3), `src/flsplash/` (section 21), `src/flboot/` (section 18), `src/freebws/` (section 32), `src/fltheme/hints.c` (section 5), `src/flfiles/mimeapps.list` (section 33), `src/flportal/freelfs-portals.conf` and `src/flportal/freelfs.portal` (section 6), and `src/flsnap/flsnap.conf` (section 10).
- `recipes/gaming-config/70-freelfs-gaming.conf` and `71-freelfs-gaming.rules` (section 28).
- `sources.list` and `sources.lock` at the top of the repository are inputs to the build (which package versions to fetch). They are not configuration for the installed system and are not covered here.

---

## Known gaps

These were checked and could not be confirmed from the code or the shipped files:

- **`/etc/profile.d/desktop.sh`**: `src/flcomp/README` says it exports `XDG_RUNTIME_DIR`. No such file exists in the repository.
- **`flinitrd` option `flive.nopersist`**: found in the source, but its exact effect was not traced.
- **Live udev rule reloads**: whether a udev rule change takes effect without a reboot.
- **iwd reloads**: whether iwd re-reads `main.conf` on a signal.
- **Keyboard layout hand edits**: whether a change to `keyboard=` in `desktop.conf` applies without restarting the session.
- **`xdg-desktop-portal` reloads**: whether a change to the portal files needs a session restart.
- **`bluez` main configuration**: the repository ships no `/etc/bluetooth/main.conf`. The `bluez` recipe was not read in full.
- **PipeWire user overrides**: the `~/.config/pipewire` override path is standard behaviour, not confirmed against the built package.
- **`/etc/nsswitch.conf`**: named by the build scripts, not shipped under `config/`.
- **`CONFIG_HZ`** in the kernel config and the build step that reads it.
- **`watchdog_test`** and the other FreeBWS diagnostic variables: named in the source; meanings were not traced.
- **`/etc/xdg` per-user portal overrides**: the xdg standard path, not checked against a FreeLFS build.
- **Install path of `pdftopdf`** (CUPS filter) and of the gaming udev rules file: the recipe was not read to the install line.
- **`FLCOMP_PANEL`, `FLCOMP_SNAPPEL` and `FLCOMP_XWAYLAND` defaults**: the code and README do not state them plainly.
