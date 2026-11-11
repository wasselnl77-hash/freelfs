# E2: gaming

Status: written, host-compiled and headless-tested where possible. Nothing here has run on
hardware; the X server side of Steam exists now (see "X11: Xwayland"), the Steam client itself was not tried.

## X11: Xwayland (was the blocker)
The Steam client is an X11 program (Chromium Embedded Framework, no Wayland backend), and so
are most Proton games' launchers. The Flathub package asks for `--socket=x11`. flcomp now is
the window manager of Xwayland (src/flcomp/xwm.c, xwsel.c, xwdeco.c; recipe `xwayland` and the X
libraries: xorgproto, xcb-proto, libxcb, libX11 and the extension libraries, xcb-util*, libXfont2,
libxkbfile, xkbcomp, libxshmfence, libxcvt, pixman; Mesa release 2 got the X11 platform, DRI3 and
GLX because Xwayland's GLX links libGL). How it works, in short:
- flcomp listens on `/tmp/.X11-unix/X<N>` (mode 0700) from its start and exports `DISPLAY=:N` to
  the whole session, so the panel, the menu and `flatpak run` pass it on. Flatpak's
  `--socket=x11` bind-mounts exactly that socket file into the sandbox; there is no X authority
  file (the server only accepts the same user).
- Xwayland does not run until the first X client connects, and flcomp ends it about 3 seconds
  after the last X window is gone and no client without a window is left (XRes). Idle: no X
  server, no X memory. Running idle: Xwayland about 46 MB resident (mostly shared libraries),
  flcomp itself about 28 MB. Its messages (and xkbcomp's) are in `$XDG_RUNTIME_DIR/xwayland.log`.
- X windows are normal windows for the rest of the desktop: panel entry (app id from WM_CLASS),
  workspaces, Alt-Tab, overview, snap/maximize, fullscreen with performance mode, General Manager
  (they are ordinary processes there; Xwayland itself shows up as a program called Xwayland).
  Windows without a title bar of their own get one from flcomp; Steam asks for none
  (`_MOTIF_WM_HINTS`) and keeps its own. Menus and tooltips (override-redirect) are drawn where
  the program puts them. CLIPBOARD is bridged both ways; PRIMARY stays among X programs.
- `flgaming check` has an "X11 (Xwayland)" section: Xwayland and xkbcomp installed, DISPLAY set,
  the X socket yours and writable, whether Xwayland draws in software, and whether Steam's
  permissions list the x11 socket.
Tested headless in the build chroot (`make check-x` in src/flcomp, a small xcb client,
screenshots docs/m7/xwl-*.png): lazy start/stop/restart, frames, titles, focus, move, resize,
maximize, fullscreen, menus, dialogs, no-decoration windows, workspaces, Alt-Tab, overview,
clipboard both ways. **Not tested: the Steam client itself, a Flatpak app, a real GPU, QEMU
with the new image, the laptop.**

Limits that matter for games:
- No glamor/DRI3 yet. Xwayland enables it only on a real GPU, and only if the compositor offers
  `zwp_linux_dmabuf_v1` version 4 (default feedback) or `wl_drm`; flcomp offers version 3. Until
  that is added (dmabuf.c, needs a GPU to test) Xwayland draws in software (xwayland.log:
  "falling back to sw"): the Steam client's window and X programs work, GL inside X is slow, and
  Vulkan games cannot present through X11 (they need DRI3). It also refuses glamor on llvmpipe, so
  QEMU always runs the software path.
- Games that change the video mode (XRandR, old SDL) are not emulated: that needs
  `wp_viewporter` in flcomp. They get a window of the size they ask for.
- X cursor images are not used (flcomp draws its arrow for every client), no X<->Wayland drag
  and drop, no system tray (Steam's tray icon), no XSettings (DPI follows 96).
- Fonts: only the built-in `fixed` and `cursor` core fonts and fontconfig's TTFs (DejaVu, Plex)
  for Xft programs; old programs that ask for other core bitmap fonts get a fallback or fail.
  No font-misc recipe yet.
Big Picture ("controller works in Big Picture") still needs the client to be tried.

## Kernel (config/kernel/freelfs.config, linux release 6)
Checked with `make defconfig`, `merge_config.sh` and `olddefconfig` on the 6.18.59 tree and the
recipe's "not applied" loop: every line applies. The kernel was not built.
- Xbox pads: `CONFIG_JOYSTICK_XPAD` (the name "hid-xpad" does not exist in mainline) with FF and LEDs, module.
- DualShock 4 / DualSense: `HID_PLAYSTATION` + `PLAYSTATION_FF` (+ `LEDS_CLASS_MULTICOLOR`), module.
- Switch pads: `HID_NINTENDO` + `NINTENDO_FF`, module (USB; Bluetooth waits for BlueZ).
- Steam Controller / Deck: `HID_STEAM` + `STEAM_FF`, module. The client talks to hidraw itself;
  if hid-steam gets in its way, blacklist it.
- `INPUT_FF_MEMLESS`, `INPUT_JOYDEV` (module), `HIDRAW`, `HID_BATTERY_STRENGTH`.
- `/dev/uinput` (was `=m`, now `=y`), `/dev/uhid` and `/dev/ntsync` are built in: rc.sysinit loads no
  modules, and these have no hardware to trigger a load. `NTSYNC` needs kernel 6.14 or later (6.18 has it).
- Already on: `USER_NS`, `FUTEX` (futex_waitv/fsync), `IA32_EMULATION`, `MODIFY_LDT_SYSCALL`, seccomp, FUSE.
- Known overlap: `HID_SONY=y` (defconfig) also lists DualShock 4; if a DS4 shows up as a Sony
  device without touchpad/gyro, make `HID_SONY` a module.

## udev, sysctl, helper (recipes/gaming-config)
- `71-freelfs-gaming.rules`: no logind means no `uaccess`, so controller event/js nodes, hidraw of
  Valve/Sony/Nintendo/Microsoft/8BitDo and other pad makers, raw USB for Steam's dongle, `/dev/uinput`,
  `/dev/uhid`, `/dev/ntsync` go to group `seat`, 0660 (new users are in seat, audio, video, render, not input).
  The PlayStation touchpad gets `LIBINPUT_IGNORE_DEVICE` so it does not move the desktop pointer.
  Syntax checked with `udevadm verify` (group `seat` substituted, it does not exist on the host).
- `70-freelfs-gaming.conf` sysctl: `vm.max_map_count=1048576` (many Proton games crash at start otherwise).
- `flgaming check | steam-install | steam-prepare | steam-library PATH`: per-user only, refuses root.
  Tested against a stub `flatpak` and a fake /dev and /proc.

## Steam via Flatpak, gotchas
- Per-user install (`flatpak --user`), Flathub remote is added by the session at first login.
- Needs besides the app: the runtime of the Steam branch, `org.freedesktop.Platform.GL.default`,
  `GL32.default` and `Compat.i386` of the *same* branch. `flgaming steam-prepare` installs any missing one.
  Mesa for games (radv, anv) comes from these extensions, not from the host's Mesa.
- Sandbox needs `--device=all` (already in Steam's manifest): pads, hidraw, uinput, GPU.
- Home is `/usr/NAME`: games and `~/.var/app/com.valvesoftware.Steam` depend on recipes/flatpak's
  home-under-usr patch, which is itself unbuilt and untested.
- A second disk: `flgaming steam-library /drives/NAME`. The system directories cannot be shared.
- Steam's pressure-vessel nests a sandbox through the `org.freedesktop.portal.Flatpak` D-Bus service;
  the service file exists in the rootfs and the session bus finds it, untested.
- Disk: Steam plus runtimes is several GB before the first game.
- xdg-desktop-portal has no backend yet (file chooser, screenshots from inside the sandbox fail).

## Vulkan / GL
Mesa has radv (AMD) and lavapipe in the image; anv/iris are not (need clang, spirv-llvm-translator,
libclc, recipe comment in recipes/mesa has the steps). No change made to the build.

## Performance mode (src/flcomp/perf.c)
desktop.conf `performance=auto|on|off`. Auto: on while the top window is fullscreen. Effects: the
power saving frame cap is lifted, and an opaque, screen-covering fullscreen window with nothing over
it is drawn alone (no wallpaper, windows below or panel). Panel: switch in the battery flyout
(on, or auto when turned off); screenshots docs/m7/battery-flyout-performance-*.png.
Limits:
- The flyout exists only where there is a battery. A desktop PC has no manual switch; edit
  desktop.conf until Settings gets one.
- This is a CPU-path saving. A GPU client's dmabuf is still read back into the shadow buffer
  (gl.c) and copied to the dumb buffer, which will cap heavy games. Real direct scanout needs the
  dmabuf imported as a KMS framebuffer (`drmModeAddFB2WithModifiers`), kept alive until the flip,
  an atomic backend with a cursor plane, and a fallback; it touches drm.c and dmabuf.c and needs a GPU to test.
- No CPU governor change (gamemode does that); `flpower` only knows power saving.
