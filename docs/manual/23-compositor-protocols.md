# 23. The compositor (flcomp): protocols, internals and settings

Summary: flcomp is the FreeLFS Wayland compositor. It draws on the CPU onto DRM/KMS, speaks standard Wayland plus
its own private protocols (fl_shell, fl_screencopy), manages workspaces, outputs, night light, snapping, the lock
screen, Xwayland and the clipboard history, and reads its settings from `desktop.conf`.

## Contents

1. What flcomp is and how it is built
2. How it starts (the session)
3. Wayland globals and their versions
4. The private protocol `fl_shell` (versions 1 to 9)
5. The private protocol `fl_screencopy`
6. Trusted clients: who may use the privileged requests
7. Outputs, scale and night light
8. Windows, snapping, workspaces and the overview
9. The lock screen
10. Animations
11. Power saving and performance mode
12. X11 programs (Xwayland)
13. Clipboard and clipboard history
14. Keys, shortcuts and screenshots
15. dmabuf and GPU clients
16. `desktop.conf` keys read by flcomp
17. Debugging and headless tests
18. Source map
19. What is not implemented

---

## 1. What flcomp is and how it is built

flcomp talks to the kernel directly: DRM/KMS for the screen (dumb buffers), libseat for device access, libinput for
mice, touchpads and keyboards, and xkbcommon for layouts. It composites on the CPU, so it needs no GPU driver and about
20 MB of RAM at 1080p (README). It does not use wlroots.

- Source: `src/flcomp/`. Recipe: `recipes/flcomp/recipe`, group `desktop`, release 17 in the tree (the round notes are in
  `docs/ROADMAP-NEXT.md`).
- Build: `make` in `src/flcomp` (`CFLAGS` C17, `-Wall -Wextra -Wshadow`). It needs `wayland-server`, `libdrm`,
  `libinput`, `libseat`, `xkbcommon`, `libudev`, `libpng`, `xcb` with `xcb-xfixes`, `xcb-composite` and `xcb-res`, and
  `wayland-protocols`. EGL, GBM and GLES are used through `dlopen` only (chapter 23, section 15).
- The Makefile generates the protocol glue with `wayland-scanner` from the XML files in `protocol/` (`fl-shell.xml`,
  `fl-screencopy.xml`) and from the upstream XML of xdg-shell, linux-dmabuf and xwayland-shell.
- `make check` runs the headless suites (section 17). `make check-anim`, `check-keys`, `check-ws`, `check-lock`,
  `check-perf` and `check-x` run one each.
- The flcomp recipe copies `src/flui/flpalette.h` next to its sources (the colour tokens are shared with libflui).

## 2. How it starts (the session)

- `/etc/profile.d/desktop.sh` starts `flcomp` for ordinary users on tty1. It does not `exec`, so if flcomp fails
  you land in the shell. The desktop script sends stderr to `$XDG_RUNTIME_DIR/flcomp.log`.
- `flcomp --firstboot` runs `flsetup` (keyboard choice) and exits with its status. The login screen runs `flgreet-ui`
  this way (`FLCOMP_SETUP`).
- `flcomp` with no options is the session. It starts `flpanel` (restarted up to five times if it dies), and the session
  services of `/usr/lib/flcomp/session` (`FLCOMP_SESSION`): `dbus-daemon --session` at `$XDG_RUNTIME_DIR/bus`, then
  `pipewire`, `wireplumber` and `pipewire-pulse`, each only when installed. Their log is `$XDG_RUNTIME_DIR/session.log`.
- The same session starts `flupdate-notify`, `flsnap-notice` (says so when the system runs from an earlier snapshot)
  and the XDG autostart entries (`~/.config/autostart`, `/etc/xdg/autostart`, with `Hidden`, `OnlyShowIn`,
  `NotShowIn`, `TryExec` and `X-FreeLFS-Override` rules).
- If `flatpak` is installed, the Flathub remote is added once for the person (`flatpak --user`, retried while offline;
  stamp `~/.local/share/freelfs/flathub-added`).
- `Ctrl-Alt-Backspace` or "Log out" in the panel ends the session with status 0.
- `FLCOMP_NOSTART=1` (the desktop script) keeps you in the shell instead.

The runtime directory is `/run/user/UID` (made by the login screen). When `XDG_RUNTIME_DIR` is unset or not usable,
flcomp makes its own, `/tmp/flcomp-runtime-UID` with mode 0700 (`main.c`, `runtime_dir()`).

## 3. Wayland globals and their versions

These are the globals flcomp creates (`wl_global_create` calls in `src/flcomp`):

| Global | Version | Source | Notes |
|---|---|---|---|
| `wl_compositor` | 4 | `surface.c` | surfaces |
| `wl_subcompositor` | 1 | `surface.c` | subsurfaces (FreeBWS uses them for the page) |
| `wl_shm` | libwayland's | `surface.c` (`wl_display_init_shm`) | shared-memory buffers |
| `wl_output` | 3 | `main.c` | the primary output's logical size |
| `wl_seat` | 5 | `seat.c` | pointer, keyboard, touch not offered |
| `wl_data_device_manager` | 3 | `seat.c` | clipboard and drag offers (drag and drop not implemented) |
| `xdg_wm_base` | 3 | `xdg.c` | toplevels, popups |
| `zxdg_decoration_manager_v1` | 1 | `xdg.c` | server-side decorations (round 19). A client that binds it and does not ask for client-side gets `server_side`: flcomp draws the same title bar and frame it draws for X windows (`xwdeco.c`: minimize, maximize, close, move, resize, right click opens the window menu, middle click closes). A client that sets `client_side` keeps its own. Clients that never bind it (GTK 3, Firefox, FreeLFS's own flui programs) are not touched. A window with a server-drawn frame is not animated on open and close. Test: `src/flcomp/tests/deco.sh` |
| `zwp_linux_dmabuf_v1` | 3 | `dmabuf.c` | GPU buffers (section 15) |
| `fl_shell` | 9 | `flshell.c` | the private desktop protocol (section 4) |
| `fl_screencopy_manager` | 1 | `screencopy.c` | the private screenshot protocol (section 5) |
| `xwayland_shell_v1` | 1 | `xwm.c` | for Xwayland only: bound by its own connection, hidden from every other client |

Not offered: `wl_drm`, `wp_viewporter`, `wp_fractional_scale`, `zwlr_layer_shell` (no layer shell), primary
selection, touch, server-side decorations. Clients that need one of these fall back or do without it (chapter 23,
section 19).

Protocol errors and the close of a client follow the libwayland rules. A request that comes from a client which is not
trusted is ignored, not answered with an error (section 6).

## 4. The private protocol `fl_shell` (versions 1 to 9)

The XML is `src/flcomp/protocol/fl-shell.xml` and is installed to `/usr/share/flcomp`. Its header says it is "private
protocol between flcomp and the desktop programs (flpanel, flsettings)". Several clients may bind it; every event goes to
every binding. Only one client at a time can own the panel, and the lock (a second `lock` gets the `locked` error).

Each version adds requests and events. A client that binds version N gets what is in versions up to N.

| Version | Added | Requests | Events |
|---|---|---|---|
| 1 | the base | `destroy`, `set_panel(surface, height)`, `set_menu(surface)`, `activate(id)`, `minimize(id)`, `close(id)`, `set_keyboard_layout(name)`, `logout` | `window_added(id, app_id, title)`, `window_changed(id, title, focused, minimized)`, `window_removed(id)`, `menu_dismissed`, `menu_toggle` (Super tapped), `keyboard_layout(name)` |
| 2 | flyouts and volume keys | `set_flyout(surface, x)` | `flyout_dismissed`, `volume_key(action)` (0 raise, 1 lower, 2 mute) |
| 3 | outputs and the on-screen indicator | `set_output(w, h, refresh, scale)`, `confirm_output`, `revert_output`, `show_osd(kind, percent, flags)` | `output_mode(w, h, refresh, flags)`, `output_state(scale, w, h, pending)`, `output_failed` |
| 4 | notifications and the lock | `set_notifications(surface)`, `lock(surface)`, `unlock` | `lock_state(locked)` |
| 5 | workspaces | `set_workspace(index)`, `move_window(id, index)`, `toggle_overview` | `workspaces(cols, rows, current)`, `window_workspace(id, index)` |
| 6 | desktop icons | `set_desktop(surface, x, y)` | none |
| 7 | panel entry position | `set_window_entry(id, x, y, w, h)` | none |
| 8 | maximize | `maximize(id)` | none |
| 9 | several outputs | `output_configure(name, flags, w, h, refresh, scale, x, y, enabled, primary)`, `output_revert` | `output_info(name, flags, x, y, w, h, refresh, scale)`, `output_mode_info(w, h, refresh, flags)` |

Details that matter when you write a client:

- `set_panel`: the surface sits at the bottom edge (the top with `panel_position=top`), always above ordinary windows,
  and the compositor keeps the height free when it maximizes windows. It gets pointer input but never keyboard focus.
- `set_menu` and `set_flyout`: shown while the surface has a buffer. A click elsewhere hides it and sends
  `menu_dismissed` or `flyout_dismissed`. It shows again at the next commit that attaches a buffer.
- `set_output` (v3): tries a mode and a scale. If `confirm_output` does not arrive within 15 seconds (or the client goes
  away), the old mode comes back. A refused mode sends `output_failed`.
- `output_configure` (v9): applies one output at once and saves it in `desktop.conf` (section 7). It has no automatic
  revert: the caller asks the person to confirm and then calls `output_revert` if not.
- `set_notifications`: the cards are shown at the top right, 16 px from the edges, above windows, panel, menu and flyout,
  but not above the lock screen. Transparent pixels take no pointer input.
- `set_desktop` (v6): a small surface drawn above the wallpaper and below every window, at `x, y`, on every workspace.
  Only what is not transparent takes the pointer.
- `set_window_entry` (v7): where the panel shows a window, used by minimize and restore animations.
- `lock` (v4): see section 9.
- `show_osd` (v3): one indicator (icon and bar) above the panel for about 1.5 seconds. `kind` 0 volume, 1 brightness,
  2 microphone; flag 1 means muted.
- The compositor swallows `XF86AudioRaiseVolume`, `XF86AudioLowerVolume` and `XF86AudioMute` and sends `volume_key` to
  every binding of version 2 or later. The panel then changes the volume and calls `show_osd`.

An example of the order of events on bind: `output_info` and `output_mode_info` for each connector, then `output_state`
(v9); `workspaces` and `window_workspace` for each window (v5); `keyboard_layout` (v1); `lock_state` when locked (v4).

## 5. The private protocol `fl_screencopy`

`src/flcomp/protocol/fl-screencopy.xml`, global `fl_screencopy_manager` version 1. Snappel (`src/snappel`) is the client.

- Manager requests: `capture_output`, `capture_region`, `capture_window`, `list_windows`. The events `window` and
  `windows_done` answer `list_windows`.
- Frame requests: `copy(buffer)` into a `wl_shm` buffer. Events: `buffer` (the format and size to use), `ready` (the
  pixels are there) and `failed` (no picture: locked, window gone, or nothing to capture).
- The picture is the desktop as flcomp draws it: no pointer, no Alt-Tab row, no indicator, and without the asking
  process's own windows (found by pid). `capture_window` gives one window from its own pixels.
- Everything fails while the screen is locked.

## 6. Trusted clients: who may use the privileged requests

Some `fl_shell` requests change the system: ending the session, the lock, the output mode, the desktop layer. flcomp
accepts them only from programs it knows. `flshell.c`, function `trusted_client()`, identifies the client by the
`/proc/PID/exe` link of its process (a process cannot change that link). The program's base name must be one of:

`flpanel`, `fllock`, `flgreet`, `flgreet-ui`, `flsettings`, `fldisplay`

and it must live in `/usr/bin/`, `/usr/sbin/` or `/usr/libexec/`. Any other client gets its request ignored:

- `logout`, `set_output`, `output_configure`, `output_revert`, `set_desktop`, `lock`.
- `unlock` is only honoured from the client that holds the lock (its surface is still the lock surface).

Setting `FLCOMP_TRUST_ANY=1` lifts the check for tests. The flcomp Makefile exports it, so the headless test clients
(`tests/shmclient`, `tests/xclient`) are trusted there. Whoever starts flcomp sets its environment, so an ordinary user
cannot set this for the real session, and `flgreet` clears it.

The check was added in round 16, after the bug scan of 2026-11-13 (`docs/bugscan-2026-11-13/A-compositor-session.md`).
`fldisplay` joined the list in round 17.

## 7. Outputs, scale and night light

Every connector the backend knows is an output (for example `HDMI-A-1`, `eDP-1`, `Virtual-1`). The enabled outputs form one
desktop:

- The desktop's logical size is the bounding box of the enabled outputs, each divided by its scale. Scale is a percentage
  from 100 to 200 per output. Clients always draw at scale 1, so `wp_fractional_scale` is not needed. The compositor
  enlarges the picture for each output (pixel doubling at 200 %, bilinear in between).
- The primary output carries the panel, the start menu, notifications, and maximized, fullscreen and snapped windows.
  `wl_output` and the panel see only the primary's logical size. The other outputs are extra space for windows and
  the pointer.
- Settings are saved in `desktop.conf` by flcomp itself, after `output_configure` (from `fldisplay set`):

  ```
  output.HDMI-A-1=mode=1920x1080@60 scale=1.25 pos=0,0 enabled=1 primary=1
  ```

  They are applied at start and after a hotplug (a udev `drm` change, with a 300 ms settle). The old keys `mode=` and
  `scale=` still apply to the primary output until it has an `output.NAME` entry.
- `set_output` (v3) keeps the previous mode for `revert_output`. `output_configure` (v9) keeps the previous state of all
  outputs for `output_revert`.
- Limits written in the README: the panel, the overview, the lock text and the desktop icon layer assume the primary
  output is at the top left; the pointer can enter empty corners between differently sized outputs; snapping at the
  screen edge uses the whole desktop's edges.
- For tests: `FLCOMP_HEADLESS_OUTPUTS=N` (1 to 4) makes N fake outputs (`src/fldisplay/tests/check.sh`).

**Night light.** Keys (from `config.c` and `nightlight.c`):

| Key | Values | Meaning |
|---|---|---|
| `night_light` | `0`, `1`, `auto` | off, on, or on between `night_light_from` and `night_light_to` |
| `nightlight` | `off`, `on`, `auto` | the newer spelling; wins over `night_light` when both are set |
| `night_light_temp` (or `nightlight_temp`) | 2700 to 6500, default 4000 | colour temperature (6500 is neutral) |
| `night_light_from`, `night_light_to` | `HH:MM`, default 20:00 and 07:00 | the schedule; it may cross midnight |

A change fades over 2 seconds. `auto` is checked every 30 seconds. The colour factors go to the CRTC's gamma table. An
output whose CRTC has no gamma table (virtio-gpu, simpledrm) gets a per-pixel table in the copy to the screen instead.
The log says which one ("night light on (3000K, ...)"). The panel's quick switch runs `fldisplay nightlight on|off`, which writes the keys (`quick.c`, `fldisplay`).

## 8. Windows, snapping, workspaces and the overview

**Windows.** flcomp handles stacking, click to focus, move and resize (`xdg_toplevel.move` and `resize`), maximize,
fullscreen, minimize, popups, and the clipboard. Windows draw their own title bars (client-side decoration). The
window menu is the compositor's own: a right click on the title bar of a FreeLFS program opens Minimize, Maximize,
Always on top, Move to workspace N, and Close. A middle click on a title bar closes the window.

**Snapping.** Drag a window to the left or right screen edge to fill that half; to the top edge to maximize. An outline
shows where it will land (`popup.c`, `snap_preview`). `Super+Left` and `Super+Right` snap to a half, `Super+Up` maximizes,
`Super+Down` restores or minimizes. The other half key undoes a snap (the README's wording: "the other key undoes it").

**Workspaces.** A grid of virtual desktops (`desktop.conf` `workspaces=COLSxROWS`, default `2x2`, each from 1 to 8).
A window is on exactly one workspace. New windows open on the workspace in view; dialogs open on their parent's
workspace. Changing the grid applies live; windows on removed workspaces move to the last one. Switching focuses the
newest window of the new workspace. `Super+Ctrl+arrows` switch along the grid (no wrap). `Super+Shift+arrows` move the
focused window and its dialogs to the next workspace, and the view follows.

**Overview.** `Super+Tab` (or `toggle_overview`) shows every workspace as a tile in the grid's shape, with its windows as
pictures, over the work area and under the panel. Click a tile to go there, a window to focus it there, or a click beside
the tiles, or Esc, to close it.

**Alt-Tab.** `Alt-Tab` and `Alt-Shift-Tab` cycle windows forwards and backwards while Alt is held. A row of window pictures
shows the choice, in the accent colour for the picked one (`switcher.c`).

## 9. The lock screen

The lock is part of `fl_shell` version 4: `lock(surface)` and `unlock`. The lock is the compositor's, not the lock
program's.

- While locked, only the lock client's surface is drawn (at 0,0, covering the output). Only it gets pointer and keyboard
  input. There is no panel, window, notification, menu, Alt-Tab, Print or Ctrl-Alt-Backspace. Key bindings do nothing
  except Ctrl-Alt-F1 to F12 (virtual terminals) and the media keys. The cursor is still drawn.
- `Super+L` locks at once and starts `fllock` (the lock program, `FLCOMP_LOCK`, default `fllock`), but only where
  `/usr/bin/fllock` exists. The lid switch locks the same way unless `lock_on_lid=0`.
- Only the client that sent `lock` can unlock. A second `lock` from another client is the `locked` protocol error.
- Every binding of version 4 gets `lock_state(1)` and `lock_state(0)`.
- If the lock program dies, the screen stays locked and shows a solid colour. flcomp starts the lock program again after
  300 ms. If it dies three times quickly (5 seconds or less each), flcomp stops restarting it. If nothing shows the lock
  within about two seconds, a hint is drawn with flcomp's built-in font (`lockfont.h`, made by `tools/genlockfont.c` from
  IBM Plex Sans): "Screen locked. The lock program is not running. Press Enter to start it again." Enter starts it
  again; Ctrl-Alt-F2 opens a console.
- There is no suspend path in the system yet (flpower knows off and restart). Whatever later suspends the machine should
  run `fllock` first, or rely on the lid.
- `make check-lock` tests all of this headless.

## 10. Animations

`desktop.conf`: `animations=on|reduced|off` (default on) and `animation_speed=FACTOR` (0.1 to 4; 2 is twice as fast),
`anim_log=1` writes a log.

- Motion: short, flat, eased, 100 to 200 ms, no wobble, no bounce. Window open (a fade from 96 % to 100 %), close (a picture
  of the last frame fades out), minimize and restore (towards and from the panel entry), maximize, snap and restore, the
  workspace slide, the overview zoom (from 108 %), and the start menu, flyouts and notification cards.
- `reduced` allows fades only. `off` moves nothing.
- Cost: flcomp composites on the CPU, so animations are drawn the same way. Nothing runs at idle. An animation exists from its
  start to its last frame, and each page flip advances it. There are no timers of its own.
- Not animated: X11 windows, anything while performance mode draws a fullscreen window alone, and the lock screen.
- `make check-anim` (`tests/anim.sh`) tests each motion and the pacing. `FLCOMP_ANIM_LOG=1` logs the frames.

## 11. Power saving and performance mode

**Power saving** (`power.c`, `media.c`, `render.c`; `power_saver=1`). The panel's battery flyout writes the key. flcomp then
draws at most 30 frames a second (repaints at least 33 ms apart) and sets the backlight to 70 % of its value. Brightness
set by hand stays as it is in `brightness=`. Turning it off restores the old value, unless the brightness was changed by
hand in the meantime. The CPU governor is set by flpower (flpanel's part).

**Performance mode** (`perf.c`; `performance=auto|on|off`). `auto` (the default) turns it on while the window on top is
fullscreen; `on` always; `off` never. In this mode there is no frame cap, and when the fullscreen window is opaque and
covers the whole output with nothing drawn over it, only that window is drawn (no wallpaper, no panel, no windows below).
Alt-Tab, the volume indicator and the pointer still appear over it. A notification or popup brings normal drawing back.
This saves work on the CPU path. It is not a KMS scanout plane: a game's dmabuf is still read into the shadow buffer.
`make check-perf` tests it.

## 12. X11 programs (Xwayland)

flcomp is also the window manager of Xwayland, so X11 programs such as Steam run.

- flcomp listens on `/tmp/.X11-unix/X<N>` (the first free N, mode 0700: only this user; there is no X authority file)
  from the start, and exports `DISPLAY=:N` to every program of the session.
- Xwayland is not running until the first X client connects. The listening socket is handed to it (`-listenfd`), it runs
  rootless (`-rootless`), it talks to flcomp over a socketpair as an ordinary Wayland client (the only one that may bind
  `xwayland_shell_v1`), and it gets flcomp as its window manager over a second socketpair (`-wm`).
- Three seconds after the last X window is gone (and no X client without a window is left) flcomp ends Xwayland. The next X
  client starts it again. On an idle desktop there is no Xwayland and no X memory.
- Env: `FLCOMP_NO_XWAYLAND=1` switches it all off. `FLCOMP_XWAYLAND=/path` names another binary, and also switches X on in
  headless runs. `FLCOMP_XDEBUG=1` logs what the window manager sees. Xwayland's own messages go to
  `$XDG_RUNTIME_DIR/xwayland.log`.
- X windows are matched to their Wayland surface by `WL_SURFACE_SERIAL` and `xwayland_surface_v1.set_serial`. A managed X
  window becomes an ordinary window: it takes part in workspaces, Alt-Tab, the overview, snapping, Super arrows, the panel
  list (app id = the lower-case `WM_CLASS`), General Manager and performance mode. Override-redirect windows, menus,
  tooltips, docks and notifications are unmanaged: drawn above the managed windows where the client put them.
- Window manager: `_NET_WM_STATE` (fullscreen, maximized, hidden, focused, skip taskbar), `_NET_ACTIVE_WINDOW`,
  `_NET_CLOSE_WINDOW`, `WM_DELETE_WINDOW` (else the client is killed), `WM_TAKE_FOCUS`, `_NET_WM_MOVERESIZE`, the normal
  hints for size limits, `_NET_FRAME_EXTENTS`, `_NET_WM_WINDOW_TYPE`, `_MOTIF_WM_HINTS` (no decoration), `WM_STATE`,
  `_NET_CLIENT_LIST`, `_NET_CURRENT_DESKTOP`, `_NET_WM_DESKTOP`.
- Frames (`xwdeco.c`): an X window without its own title bar gets one from flcomp, with minimize, maximize and close, a
  3 px border and a 4 px grip outside it.
- Clipboard (`xwsel.c`): CLIPBOARD is bridged both ways (text as `UTF8_STRING`, `STRING`, `TEXT` and `text/plain`; any
  mime-type target; INCR for large data, 64 MB at most). PRIMARY (select and middle click) stays among the X clients.

Not done: X cursor images (flcomp draws its own arrow), RandR mode changes for games, glamor and DRI3 acceleration inside
Xwayland (until `zwp_linux_dmabuf_v1` version 4 with feedback exists; Xwayland then draws in software and logs "falling
back to sw"), a system tray, XSettings, and several X screens. See `docs/E2-GAMING.md`.

## 13. Clipboard and clipboard history

- The Wayland clipboard is `wl_data_device`. Text and the mime types the client offers go through.
- Clipboard history (`seat.c`): the last 20 text entries, kept in RAM only. Texts longer than 32 KB are not kept. Hold
  Ctrl+V for 450 ms (the press is held back for that long) to open the list. Arrow keys and Enter, or a click, pick an
  entry, and the pick is pasted. A quick press of Ctrl+V pastes when the key is released; a press held longer opens the list. `FLCOMP_NO_CLIPHIST=1` turns the history off, and then Ctrl+V
  is never delayed.
- The history popup is the compositor's own (`popup.c`), drawn like the Alt-Tab row.

## 14. Keys, shortcuts and screenshots

From the README (and `seat.c`):

| Key | Action |
|---|---|
| Super (tapped) | toggles the start menu (`menu_toggle` to the panel) |
| Super+Left, Super+Right | snap to a half |
| Super+Up, Super+Down | maximize; restore or minimize |
| Super+E | `flfiles` |
| Super+T, Ctrl+Alt+T | `flterm` |
| Super+L | lock the screen (only where `fllock` exists) |
| Super+Ctrl+arrows | switch workspace |
| Super+Shift+arrows | move the focused window to the next workspace |
| Super+Tab | the workspace overview |
| Ctrl+Shift+Esc | General Manager (task manager, `flgm`) |
| Ctrl+Alt+Backspace | end the session |
| Ctrl+Alt+F1 to F12 | virtual terminals |
| Alt+Tab, Alt+Shift+Tab | cycle windows |
| Print | the whole screen: `snappel --full` when Snappel is installed, else flcomp saves it |
| Shift+Print | `snappel --region` |
| Super+Shift+S | the Snappel window (`FLCOMP_SNAPPEL` names another program) |
| XF86 volume keys | volume, sent to the panel, with the on-screen indicator |
| XF86 brightness keys | backlight in 5 % steps, at least 5 % (`media.c`); the indicator shows it |
| XF86AudioMicMute | toggles the microphone |
| XF86AudioPlay, Stop, Next, Prev | `flmusic --ctl play-pause`, `stop`, `next`, `previous` |

Without Snappel, Print saves the whole screen to `~/pictures/screenshot-YYYY-MM-DD-HHMMSS.png` (`shot.c`, libpng). The
picture is taken from a frozen copy in a forked child, so the compositor keeps drawing while the file is written. The
on-screen indicator then shows saved or failed.

## 15. dmabuf and GPU clients

`zwp_linux_dmabuf_v1` version 3 (`dmabuf.c`, `gl.c`): single-plane ARGB8888, XRGB8888, ABGR8888 and XBGR8888 formats.
A dmabuf is read when the client commits, and released at once, like a `wl_shm` buffer. Compositing stays on the CPU.

- On llvmpipe or a KMS-only device, only the LINEAR modifier is announced, and linear buffers are mapped and copied.
- On a real GPU, the first client that binds the global makes flcomp start EGL on the DRM device through GBM. Then every
  modifier the GPU can sample is announced. A tiled or compressed buffer is imported as an EGLImage, drawn with GLES2 into
  a linear texture, and read back into the surface's copy.
- A session of `shm` clients never loads EGL. `gl.c` reaches EGL, GBM and GLES through `dlopen`, because linking them pulled
  libgallium, libLLVM and SPIRV-Tools into every session (33 MB to 12 MB of PSS on the idle live desktop, according to the
  README).
- `FLCOMP_NO_GL=1` turns the GPU path off. `FLCOMP_RENDER_NODE` picks a render node.
- `wl_drm` (old EGL clients without dmabuf) and a GPU compositing path are not implemented.

FreeBWS (chapter 27) is the main GPU client. Its `freebws` README describes when it chooses dmabuf or shm.

## 16. `desktop.conf` keys read by flcomp

The file is `~/.config/freelfs/desktop.conf` (or `$FLUI_CONF`). flcomp watches it with inotify, so most changes apply at once.
Keys read by `config.c` (values as the code accepts them):

| Key | Values (default) | Meaning |
|---|---|---|
| `pointer_speed` | number | pointer speed |
| `pointer_accel` | `flat` or default | flat acceleration |
| `mouse_natural`, `touchpad_natural` | 0/1 | natural scrolling |
| `tap_click` | 0/1 | tap to click |
| `typing_disable` | 0/1 | disable touchpad while typing |
| `left_handed` | 0/1 | swap buttons |
| `key_repeat_delay`, `key_repeat_rate` | ms, per second | key repeat |
| `numlock` | 0/1 | Num Lock at start |
| `brightness` | percent | backlight, set at start |
| `power_saver` | 0/1 | power saving (section 11) |
| `performance` | `auto`, `on`, `off` | performance mode (section 11) |
| `animations` | `on` (default), `reduced`, `off` | section 10 |
| `animation_speed` | 0.1 to 4 (1) | section 10 |
| `anim_log` | 0/1 | animation log |
| `panel_position` | `bottom` (default), `top` | the panel edge |
| `panel_height` | number | the panel's height |
| `titlebar` | `normal` (34 px), `compact` (28 px) | title bar height |
| `buttons` | `right` (default), `left` | window button side |
| `lock_on_lid` | 0 disables | lid switch locks |
| `workspaces` | `COLSxROWS`, default `2x2` | the workspace grid (or a bare number N for N x 1) |
| `scale`, `mode` | scale percent, `WxH@HZ` | the primary output (until `output.NAME` exists) |
| `output.NAME` | `mode=... scale=... pos=X,Y enabled=0/1 primary=0/1` | per-output settings |
| `night_light`, `nightlight`, `night_light_temp`, `night_light_from`, `night_light_to` | section 7 | night light |
| `theme`, `accent`, `wallpaper`, `sounds` | read by `theme.c` and `sound.c` | colour scheme, accent, wallpaper; `sounds=0` silences the system sounds |

## 17. Debugging and headless tests

Environment variables (from `flcomp --help` and the README):

| Variable | Use |
|---|---|
| `FLCOMP_HEADLESS=WxH` | no display and no input devices (for tests). Headless has a made-up list of modes, so `set_output` can be tried without a display. |
| `FLCOMP_SCREENSHOT=FILE` | writes a PPM of the screen when flcomp gets `SIGUSR1` (also works on real hardware) |
| `FLCOMP_TEST_FIFO=FILE` | a FIFO of fake input. Lines: `move X Y`, `button CODE 0` or `1`, `key CODE 0` or `1`, `axis STEPS`, `lid 0` or `1` |
| `FLCOMP_TRUST_ANY=1` | lifts the trusted-client check (tests only) |
| `FLCOMP_PANEL=CMD` | the panel program (default `flpanel`; empty for none) |
| `FLCOMP_SESSION=CMD` | the session services (default `/usr/lib/flcomp/session`; empty for none) |
| `FLCOMP_SETUP=CMD` | the program for `--firstboot` (default `flsetup`) |
| `FLCOMP_LOCK=CMD` | the lock program (default `fllock`) |
| `FLCOMP_SNAPPEL=CMD` | the screenshot program for Super+Shift+S |
| `FLCOMP_KEYBOARD_CONF=FILE` | another `keyboard.conf` (default `/etc/keyboard.conf`; layout from `LAYOUT=`, `VARIANT=`, `OPTIONS=`) |
| `FLCOMP_BACKLIGHT=DIR` | another backlight directory (tests) |
| `FLCOMP_NO_SOUND`, `FLCOMP_NO_CLIPHIST`, `FLCOMP_NO_XWAYLAND`, `FLCOMP_XWAYLAND`, `FLCOMP_XDEBUG`, `FLCOMP_ANIM_LOG`, `FLCOMP_NO_GL`, `FLCOMP_RENDER_NODE`, `FLCOMP_HEADLESS_OUTPUTS` | the switches described in the sections above (sound, history, X11, animation log, GPU path, render node for a headless run on a real GPU, fake outputs) |
| `FLCOMP_WALLPAPER`, `FLCOMP_SHARE`, `FLCOMP_SEL`, `FLCOMP_XFER` | also read by the source; their meaning is not written down in this chapter (not verified) |
| `SIGUSR2` | exits with status 3 (used to test the crash screen, `flcrash`) |

Logs: `flcomp.log` in the runtime directory (what flcomp says about outputs, night light and performance mode),
`session.log` for the session services, `xwayland.log` for Xwayland.

Headless test targets in `src/flcomp/Makefile`: `make check` (`tests/run.sh`, `perf.sh`, `anim.sh`, `x.sh`), `check-anim`,
`check-keys`, `check-ws`, `check-lock`, `check-perf`, `check-x` (Xwayland, run it in the build chroot), and `tests/apps.py`
(headless tests of flfiles, fledit, flgreet-ui and flpackages). `tests/run.sh` starts flcomp with `FLCOMP_HEADLESS=640x400`,
connects the test client `tests/shmclient`, sends the fake input through the FIFO and checks the screenshot pixels.

## 18. Source map

| File | What it does |
|---|---|
| `main.c` | startup, `wl_output`, children, signals, the runtime directory |
| `drm.c` | DRM/KMS, libseat, libinput, the headless backend, gamma tables |
| `render.c` | damage lists, compositing, frame callbacks, screenshot |
| `surface.c` | `wl_surface`, regions, subsurfaces, hit testing, `wl_compositor` |
| `xdg.c` | `xdg_shell` and window management, snapping, workspaces, resizing |
| `seat.c` | pointer, keyboard, key bindings, layouts, repeat, Num Lock, clipboard, history, cursor |
| `config.c` | `desktop.conf` |
| `output.c` | modes, scale, the confirm timer, `output.NAME` |
| `nightlight.c` | night light |
| `lock.c`, `lockfont.h` | the lock screen; the hint's glyphs (made by `tools/genlockfont.c`) |
| `power.c`, `perf.c` | power saving and performance mode |
| `anim.c` | animations |
| `overview.c`, `switcher.c`, `popup.c`, `osd.c` | overview, Alt-Tab row, popups (window menu, history, snap outline), on-screen indicator |
| `media.c` | volume, brightness and microphone keys |
| `shot.c`, `screencopy.c` | Print and the screenshot protocol |
| `theme.c` | `theme=`, `accent=` and `wallpaper=` |
| `sound.c` | runs `flsound` for the system sounds |
| `flshell.c` | `fl_shell` |
| `dmabuf.c`, `gl.c` | the dmabuf global; EGL through `dlopen` |
| `xwm.c`, `xwm-int.h`, `xwm.h` | Xwayland: the X socket, starting and ending it, the X window manager |
| `xwsel.c`, `xwdeco.c` | the clipboard between X and Wayland; the frame around X windows |

## 19. What is not implemented

From the README and the tree: drag and drop, touch input, server-side decorations, `wl_drm`, GPU compositing (EGL is used
to import GPU buffers only), layer-shell (so there is no `zwlr_layer_shell`; the desktop uses `fl_shell` for panels,
notifications and the desktop layer), `wp_viewporter`, fractional scale, Wayland primary selection, X cursor images,
RandR, and XSettings. "Screen recording in Snappel" is listed as later in `docs/ROADMAP-NEXT.md`.

Read `src/flcomp/README` (353 lines) for the full design notes. This chapter follows it and the headers of the source files.
