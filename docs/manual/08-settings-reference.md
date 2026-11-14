# 8. Settings reference

> Every page of **Settings** and **Themes**, what each option does, and the keys
> that the settings write to `~/.config/freelfs/desktop.conf`. You do not have to
> edit that file by hand, but the keys are listed in case you want to.

## Contents

- [Opening Settings](#opening-settings)
- [General](#general)
- [Display](#display)
- [Mouse & touchpad](#mouse--touchpad)
- [Keyboard](#keyboard)
- [Workspaces](#workspaces)
- [Date & time](#date--time)
- [Wi-Fi](#wi-fi)
- [Bluetooth](#bluetooth)
- [Printers](#printers)
- [Snapshots](#snapshots)
- [About](#about)
- [Themes](#themes)
- [The desktop.conf keys](#the-desktopconf-keys)
- [FreeBWS settings](#freebws-settings)

## Opening Settings

- Start menu, Settings. Or the **Settings** item in the desktop's right-click menu.
- From a terminal, open a page directly:

```sh
flsettings --page general      # or: display, wallpaper (opens Display), mouse, keyboard,
                               #    workspaces, datetime, wifi, bluetooth, printers,
                               #    snapshots, about
flsettings --set-wallpaper FILE.png     # set a picture as wallpaper, without a window
```

If Settings is already open, the page is shown in that window.

Settings are written to `~/.config/freelfs/desktop.conf`. The desktop follows the
change at once. Most pages have no Apply button.

## General

| Section | Option | What it does |
|---|---|---|
| Appearance | **Theme** | Dark or light. Colours of the panel, windows and menus. Also updates the GTK settings of Flatpak apps |
| Appearance | **Accent colour** | Six swatches: green, teal, blue, amber, orange, rose. Selection, highlights, the pager's current workspace, the lock screen, and the volume indicator |
| Appearance | **More in Themes** | Opens the Themes app |
| Panel | **Show names** | Puts the window's name next to its icon in the panel |
| Sounds | **System sounds** | Quiet sounds for sign-in, alerts, volume and screenshots. Off turns them all off |
| Sounds | **Test** | Plays a notification sound, even when sounds are off |

## Display

| Section | Option | What it does |
|---|---|---|
| Screens | **Arrangement** | Drag the screens to where they sit. They snap edge to edge |
| Screens | **Resolution** and **Refresh** | The mode the screen runs at. Lists the modes the screen reports |
| Screens | **Scale** | 100 to 200 percent. Bigger means things look larger |
| Screens | **Enabled** | Turns a screen on or off |
| Screens | **Primary** | Marks the primary screen |
| Screens | **Apply** | Applies the changes, then asks **Keep these settings?** with a 15 second countdown. Keep leaves them. Otherwise, or when the countdown ends, they go back |
| Night light | **Off, On, Auto** | Warmer colours. Auto uses the times below |
| Night light | **Warmth** | The colour temperature, 2700 K (very warm) to 6500 K (normal) |
| Night light | **From, To** | The times for Auto, as HH:MM. From is when it warms up, To when it goes back to normal |
| Brightness | **Brightness** | The screen's backlight, when the screen has one. Without a backlight the page says so |
| Wallpaper | **Accent (match the accent colour)** | The default wallpaper is the 3D F in the accent colour |
| Wallpaper | **Colours** | One of the six built-in wallpapers |
| Wallpaper | **Your picture** | Type or paste the path of a PNG or JPEG and press **Use picture**. It is scaled to fit 2560 by 1440 and saved in your data folder. **Remove** deletes an imported picture |

Notes:

- The brightness keys on the keyboard change the same setting, in 5 percent steps.
- Power saving lowers the brightness to 70 percent until you turn it off.

## Mouse & touchpad

| Section | Option | What it does |
|---|---|---|
| Pointer | **Pointer speed** | How far the pointer moves for a given movement (-100 to 100) |
| Pointer | **Acceleration** | **Adaptive** speeds up with quick movements. **Flat** moves the pointer the same distance for the same movement |
| Pointer | **Left-handed** | Swaps the left and right buttons |
| Mouse | **Natural scrolling** | The content moves with the wheel, as on a phone |
| Touchpad | **Natural scrolling** | Two fingers move the content, not the scrollbar |
| Touchpad | **Tap to click** | A light tap on the pad is a click (on by default) |
| Touchpad | **Disable while typing** | Ignores the pad while keys are pressed (on by default) |

## Keyboard

| Section | Option | What it does |
|---|---|---|
| Layout | **de, us, uk, fr** | The keyboard layout. The choice is written to `/etc/keyboard.conf` (for everyone) when you are allowed to write it. If not, it is saved for you only and the window says so |
| Typing | **Repeat delay** | How long a key is held before it repeats (default 600 ms) |
| Typing | **Repeat rate** | How fast it repeats, in keys per second (default 25) |
| Typing | **Num Lock at start** | The number pad types digits when the desktop starts |
| Try it | Field | Type here to try the layout and the repeat |
| Shortcuts | List | A read-only list of the desktop's shortcuts |

The shortcut list is not complete. See [05-keyboard-shortcuts.md](05-keyboard-shortcuts.md)
for every key, and its last section for the differences.

## Workspaces

| Option | What it does |
|---|---|
| **Across** and **Down** | Number of workspaces across and down, 1 to 8 each |
| Total and picture | How many workspaces there are, and a small picture of the grid |
| Quick choices | 1, 2, 4, 6 and 8 (1 by 1, 2 by 1, 2 by 2, 3 by 2, 4 by 2) |

Changes apply at once. Windows on a workspace that is removed move to the last one that
is left. The page also reminds you of the keys: Super+Ctrl+arrows to switch,
Super+Shift+arrows to move a window, Super+Tab for the overview.

The default is 2 by 2.

## Date & time

| Option | What it does |
|---|---|
| **Time zone map** | Click a place. The nearest zone is chosen. Hover shows the local time |
| **Search** | Type a city, country or zone name, such as "berlin" or "new york" |
| **Clock: 24-hour or 12-hour** | Format of the clock in the panel |
| **Set time automatically** | Turns the automatic time service (NTP) on or off. The line below says when it last synchronised. **Sync now** asks again |
| **Date and time** (when the automatic time is off) | Type a date (YYYY-MM-DD) and a time (HH:MM), then **Set** |

The time zone is written to `/etc/localtime`, which needs the right to write it.
When that is not allowed, the page says so in red and nothing changes.

## Wi-Fi

| Option | What it does |
|---|---|
| **Wi-Fi switch** | Turns Wi-Fi on or off |
| **Networks** | The networks around you, with signal bars and a lock for the secured ones |
| **Scan** | Looks for networks again |
| **Connect** | Asks for the password the first time, then connects |
| **Disconnect** | Leaves the current network |
| **Forget** | Removes the saved password of a network |
| **Wired** | A status line for the cable connection |

Notes:

- The Wi-Fi service is iwd. The page needs it running. "Looking for the Wi-Fi service..."
  means it is starting. "No Wi-Fi hardware found" means the computer has no Wi-Fi
  adapter that the kernel knows, or its firmware did not load.
- A password is wiped from memory after it is sent, and is never written to a log.
- Only the Wi-Fi chips that the kernel drives can be used. The tested one is a Realtek
  rtw89 chip on the laptop. See `docs/HARDWARE.md`.

## Bluetooth

| Option | What it does |
|---|---|
| **Adapter** and **Power** | Turns the Bluetooth adapter on or off |
| **Paired devices** | Connect, disconnect, forget, and the battery level when the device reports it |
| **Search** | Looks for nearby devices for 10 seconds |
| **Pair** | Pairs, trusts and connects a nearby device |

Without an adapter the page says "No Bluetooth adapter found". The panel's Bluetooth
icon only appears when an adapter is there.

## Printers

| Option | What it does |
|---|---|
| **Printers** | The printers found on the network or over USB |
| **Set as default** | Makes a printer the default |
| **Test page** | Prints a test page |
| **Cancel jobs** | Stops the printing jobs of a printer |
| **Remove** | Removes a printer. It asks first |
| **Add a printer** | Adds a printer that was found, without a driver package (IPP Everywhere) |

The printing system is CUPS. It starts when it is first needed, and stops after a
minute without use. Without the printing tools, the page says "Printing is not
installed".

## Snapshots

Snapshots are copies of the system that FreeLFS takes before each package change.

| Option | What it does |
|---|---|
| **Take a snapshot** | Makes one now, with the date and the reason |
| **Go back to this** | Makes this snapshot the current state. It asks first. The state you leave is kept as a snapshot, and you restart to use it |
| **Delete** | Removes a snapshot |
| **Make this the current state** | A banner that appears when you run from an old snapshot |

The number of snapshots kept is 8 by default (`keep = 8` in `/etc/flsnap.conf`).
The snapshot the system runs from is never removed.

To start an earlier state, use the boot menu's "Start an earlier state". See [09-troubleshooting.md](09-troubleshooting.md).

## About

| Section | What it shows or does |
|---|---|
| **This computer** | System (FreeLFS, and "live" on the live system), kernel, processor, memory, computer name, graphics |
| **Save a report** | Runs `flreport` (about 10 seconds) and tells you the file's name. The file goes to your home folder |
| **Live system: Keep my files on this USB stick** | Visible on the live system only. Creates a 4 GB persistence file on the stick (see [02-getting-started.md](02-getting-started.md)) |

## Themes

Themes (`fltheme`) is the place for the look of the desktop. It has its own sidebar.

| Page | Options |
|---|---|
| **Presets** | FreeLFS Dark (default), FreeLFS Light, Compact, Large. **My presets**: save the current look under a name, apply or delete. **Reset** puts every themed setting back to its default |
| **Colours** | Dark or light. Accent (the six swatches or six hex digits). A link to the wallpaper in Settings > Display |
| **Fonts** | Interface font: IBM Plex Sans, IBM Plex Mono, DejaVu Sans, DejaVu Sans Mono (the ones installed). Text size from 11 to 16 pixels, 13 by design |
| **Windows** | Buttons on the right or the left. Title bar normal (34 px) or compact (28 px). Corners are square and fixed |
| **Panel** | Bottom or top edge. Height from 36 to 60 pixels. Icon size from 16 to 28 pixels |
| **Animations** | On, reduced (fades only) or off. Speed from 0.5 times to 2 times |

Command line:

```sh
fltheme --page presets        # or: colours, fonts, windows, panel, animations
fltheme --list                # the presets
fltheme --apply NAME          # apply a preset (yours or a built-in)
fltheme --save NAME           # save the current look as a preset
fltheme --reset
```

My presets are small files in `~/.config/freelfs/themes/NAME.theme`, with
`key=value` lines for the keys of this chapter. A preset cannot change the wallpaper.

## The desktop.conf keys

The file is `~/.config/freelfs/desktop.conf`, one `key=value` per line. The programs
read it again when it changes. A value equal to its default is removed from the
file. The defaults in brackets are the ones the code or the project's documents
state. Where no default is given, it was not stated in the sources read for this
chapter.

**Appearance**

| Key | Values [default] | Used by |
|---|---|---|
| `theme` | `dark`, `light` [dark] | every app, the compositor |
| `accent` | `green`, `teal`, `blue`, `amber`, `orange`, `rose`, or six hex digits [green] | every app, the compositor |
| `font` | a font family name [IBM Plex Sans] | every app |
| `font_size` | 10 to 18 [13] | every app |
| `titlebar` | `normal`, `compact` | the compositor, apps |
| `buttons` | `right`, `left` | the compositor, apps |
| `wallpaper` | `accent` [accent], a built-in colour name, `user:NAME`, or a path to a picture | the compositor, Settings |
| `animations` | `on`, `reduced`, `off` | the compositor |
| `animation_speed` | 0.1 to 4 [1.0] | the compositor |
| `anim_log` | `1` writes animation timing to the log | the compositor (for tests) |

**Panel, clock and sound**

| Key | Values [default] | Used by |
|---|---|---|
| `panel_position` | `bottom`, `top` [bottom] | the panel, the compositor |
| `panel_height` | 36 to 64 [44] | the panel, the compositor |
| `panel_icon_size` | 16 to 28 [20] | the panel |
| `panel_labels` | `0`, `1` | the panel |
| `clock_24h` | `1`, `0` [1] | the panel |
| `tz_changed` | written by Settings after a time zone change | the panel |
| `sounds` | `0`, `1` [1] | the sound program, the compositor |
| `notify_dnd` | `0`, `1` | the panel (Do not disturb) |

**Display and night light**

| Key | Values [default] | Used by |
|---|---|---|
| `mode` | `WIDTHxHEIGHT@HZ`, for example `1920x1080@60` | the compositor |
| `scale` | 100 to 200 [100] | the compositor |
| `brightness` | a percentage | the compositor, Settings |
| `nightlight` | `off`, `on`, `auto` | Settings, the panel |
| `nightlight_temp` | 2700 to 6500 (kelvin) | Settings |
| `nightlight_from`, `nightlight_to` | times as `HH:MM` | Settings |
| `night_light` | `0`, `1` [0] | the compositor, the panel (kept for older programs) |
| `night_warmth` | 0 to 100 | the compositor (kept for older programs) |
| `power_saver` | `0`, `1` | the compositor, the panel |
| `performance` | `auto`, `on`, `off` [auto] | the compositor |
| `lock_on_lid` | `0`, `1` [1] | the compositor |

**Workspaces**

| Key | Values [default] | Used by |
|---|---|---|
| `workspaces` | `COLSxROWS`, 1 to 8 each [2x2] | the compositor, the panel, Settings |

**Mouse, touchpad and keyboard**

| Key | Values [default] | Used by |
|---|---|---|
| `pointer_speed` | -100 to 100 | the compositor |
| `pointer_accel` | `adaptive`, `flat` | the compositor |
| `left_handed` | `0`, `1` | the compositor |
| `mouse_natural` | `0`, `1` | the compositor |
| `touchpad_natural` | `0`, `1` | the compositor |
| `tap_click` | `0`, `1` [1] | the compositor |
| `typing_disable` | `0`, `1` [1] | the compositor |
| `key_repeat_delay` | milliseconds [600] | the compositor |
| `key_repeat_rate` | keys per second [25] | the compositor |
| `numlock` | `0`, `1` | the compositor |
| `keyboard` | `de`, `us`, `uk`, `fr` | Settings |

The keyboard layout itself is in `/etc/keyboard.conf`, as `LAYOUT=de` (and optionally
`VARIANT=` and `OPTIONS=`). Without that file the first boot asks.

## FreeBWS settings

FreeBWS has its own file, `~/.config/freebws/config`. Its settings page
(`freebws://settings`) writes the same keys.

| Key | Values (default) | What it does |
|---|---|---|
| `search` | `brave`, `duckduckgo`, `google`, `startpage`, `bing`, `ecosia` (`brave`) | The search engine for the address bar |
| `adblock` | `1` or `0` (`1`) | The ad blocker |
| `adblock_off` | a comma list of sites | Sites where the ad blocker is off (the shield's switch writes this) |
| `dark` | `1` or `0` | Dark mode for light pages, by default |
| `dark_on`, `dark_off` | comma lists of sites | Dark mode forced on or off for those sites |
| `downloads_dir` | a folder (`~/downloads`) | Where downloads go |
| `bookmarks_bar` | `1` or `0` (`1`) | Shows the bookmarks bar |
| `cookies` | `always` or `no-third-party` (`always`) | Which cookies are kept |
| `useragent` | a text (a Safari-like string) | Replaces the browser's identity string |
| `buffers` | `auto`, `shm` or `dmabuf` (`auto`) | How pages are drawn. `shm` is the CPU path, `dmabuf` uses the graphics card |
| `sandbox` | `auto`, `on` or `off` (`auto`) | The sandbox for the web pages. `on` refuses to start without it |

Environment variables for testing are listed in `src/freebws/README`. Normal users
do not need them.

Next chapter: [09-troubleshooting.md](09-troubleshooting.md). Back to
[07-files-drives-usb.md](07-files-drives-usb.md) or the table of contents in
[README.md](README.md).
