---
tags: [component, system]
---
# flcomp

#component #system

flcomp is the display server of the FreeLFS desktop. It talks to the kernel directly (DRM/KMS dumb buffers, libseat for device access, libinput for mice and keyboards) and composites on the CPU, so it needs no GPU driver and about 20 MB...

## Source files

- [[docs/graph/files/flcomp/anim.c|src/flcomp/anim.c]] -- 942 lines -- 56 fn
- [[docs/graph/files/flcomp/comp.h|src/flcomp/comp.h]] -- 510 lines -- 3 fn
- [[docs/graph/files/flcomp/config.c|src/flcomp/config.c]] -- 337 lines -- 11 fn
- [[docs/graph/files/flcomp/dmabuf.c|src/flcomp/dmabuf.c]] -- 338 lines -- 19 fn
- [[docs/graph/files/flcomp/drm.c|src/flcomp/drm.c]] -- 1192 lines -- 50 fn
- [[docs/graph/files/flcomp/fl-screencopy-client-protocol.c|src/flcomp/fl-screencopy-client-protocol.c]] -- 78 lines (generated)
- [[docs/graph/files/flcomp/fl-screencopy-client-protocol.h|src/flcomp/fl-screencopy-client-protocol.h]] -- 366 lines (generated)
- [[docs/graph/files/flcomp/fl-screencopy-protocol.c|src/flcomp/fl-screencopy-protocol.c]] -- 78 lines (generated)
- [[docs/graph/files/flcomp/fl-screencopy-protocol.h|src/flcomp/fl-screencopy-protocol.h]] -- 312 lines (generated)
- [[docs/graph/files/flcomp/fl-shell-client-protocol.c|src/flcomp/fl-shell-client-protocol.c]] -- 97 lines (generated)
- [[docs/graph/files/flcomp/fl-shell-client-protocol.h|src/flcomp/fl-shell-client-protocol.h]] -- 809 lines (generated)
- [[docs/graph/files/flcomp/fl-shell-protocol.c|src/flcomp/fl-shell-protocol.c]] -- 97 lines (generated)
- [[docs/graph/files/flcomp/fl-shell-protocol.h|src/flcomp/fl-shell-protocol.h]] -- 760 lines (generated)
- [[docs/graph/files/flcomp/flshell.c|src/flcomp/flshell.c]] -- 407 lines -- 53 fn
- [[docs/graph/files/flcomp/gl.c|src/flcomp/gl.c]] -- 455 lines -- 9 fn
- [[docs/graph/files/flcomp/linux-dmabuf-unstable-v1-client-protocol.c|src/flcomp/linux-dmabuf-unstable-v1-client-protocol.c]] -- 120 lines (generated)
- [[docs/graph/files/flcomp/linux-dmabuf-unstable-v1-client-protocol.h|src/flcomp/linux-dmabuf-unstable-v1-client-protocol.h]] -- 1084 lines (generated)
- [[docs/graph/files/flcomp/linux-dmabuf-unstable-v1-protocol.c|src/flcomp/linux-dmabuf-unstable-v1-protocol.c]] -- 120 lines (generated)
- [[docs/graph/files/flcomp/linux-dmabuf-unstable-v1-protocol.h|src/flcomp/linux-dmabuf-unstable-v1-protocol.h]] -- 926 lines (generated)
- [[docs/graph/files/flcomp/lock.c|src/flcomp/lock.c]] -- 316 lines -- 25 fn
- [[docs/graph/files/flcomp/lockfont.h|src/flcomp/lockfont.h]] -- 124 lines
- [[docs/graph/files/flcomp/main.c|src/flcomp/main.c]] -- 330 lines -- 19 fn
- [[docs/graph/files/flcomp/media.c|src/flcomp/media.c]] -- 329 lines -- 13 fn
- [[docs/graph/files/flcomp/nightlight.c|src/flcomp/nightlight.c]] -- 132 lines -- 9 fn
- [[docs/graph/files/flcomp/osd.c|src/flcomp/osd.c]] -- 184 lines -- 9 fn
- [[docs/graph/files/flcomp/output.c|src/flcomp/output.c]] -- 653 lines -- 33 fn
- [[docs/graph/files/flcomp/overview.c|src/flcomp/overview.c]] -- 368 lines -- 20 fn
- [[docs/graph/files/flcomp/perf.c|src/flcomp/perf.c]] -- 102 lines -- 6 fn
- [[docs/graph/files/flcomp/popup.c|src/flcomp/popup.c]] -- 382 lines -- 22 fn
- [[docs/graph/files/flcomp/power.c|src/flcomp/power.c]] -- 17 lines -- 2 fn
- [[docs/graph/files/flcomp/render.c|src/flcomp/render.c]] -- 435 lines -- 25 fn
- [[docs/graph/files/flcomp/screencopy.c|src/flcomp/screencopy.c]] -- 276 lines -- 16 fn
- [[docs/graph/files/flcomp/seat.c|src/flcomp/seat.c]] -- 1413 lines -- 81 fn
- [[docs/graph/files/flcomp/shot.c|src/flcomp/shot.c]] -- 187 lines -- 6 fn
- [[docs/graph/files/flcomp/sound.c|src/flcomp/sound.c]] -- 39 lines -- 1 fn
- [[docs/graph/files/flcomp/surface.c|src/flcomp/surface.c]] -- 823 lines -- 54 fn
- [[docs/graph/files/flcomp/switcher.c|src/flcomp/switcher.c]] -- 182 lines -- 9 fn
- [[docs/graph/files/flcomp/theme.c|src/flcomp/theme.c]] -- 171 lines -- 9 fn
- [[docs/graph/files/flcomp/tools/genlockfont.c|src/flcomp/tools/genlockfont.c]] -- 74 lines -- 2 fn
- [[docs/graph/files/flcomp/xdg-decoration-unstable-v1-protocol.c|src/flcomp/xdg-decoration-unstable-v1-protocol.c]] -- 76 lines (generated)
- [[docs/graph/files/flcomp/xdg-decoration-unstable-v1-protocol.h|src/flcomp/xdg-decoration-unstable-v1-protocol.h]] -- 381 lines (generated)
- [[docs/graph/files/flcomp/xdg-shell-client-protocol.c|src/flcomp/xdg-shell-client-protocol.c]] -- 184 lines (generated)
- [[docs/graph/files/flcomp/xdg-shell-client-protocol.h|src/flcomp/xdg-shell-client-protocol.h]] -- 2392 lines (generated)
- [[docs/graph/files/flcomp/xdg-shell-protocol.c|src/flcomp/xdg-shell-protocol.c]] -- 184 lines (generated)
- [[docs/graph/files/flcomp/xdg-shell-protocol.h|src/flcomp/xdg-shell-protocol.h]] -- 2392 lines (generated)
- [[docs/graph/files/flcomp/xdg.c|src/flcomp/xdg.c]] -- 1642 lines -- 112 fn
- [[docs/graph/files/flcomp/xwayland-shell-protocol.c|src/flcomp/xwayland-shell-protocol.c]] -- 72 lines (generated)
- [[docs/graph/files/flcomp/xwayland-shell-protocol.h|src/flcomp/xwayland-shell-protocol.h]] -- 333 lines (generated)
- [[docs/graph/files/flcomp/xwdeco.c|src/flcomp/xwdeco.c]] -- 400 lines -- 23 fn
- [[docs/graph/files/flcomp/xwm-int.h|src/flcomp/xwm-int.h]] -- 96 lines
- [[docs/graph/files/flcomp/xwm.c|src/flcomp/xwm.c]] -- 1634 lines -- 75 fn
- [[docs/graph/files/flcomp/xwm.h|src/flcomp/xwm.h]] -- 75 lines
- [[docs/graph/files/flcomp/xwsel.c|src/flcomp/xwsel.c]] -- 637 lines -- 23 fn

## Uses

- component [[docs/graph/components/flui|flui]] (hdr:flpalette.h)
- libraries: `--libs`, `-lm`, `-lz`, `egl`, `gbm`, `glesv2`, `libdrm`, `libinput`, `libpng`, `libseat`, `libudev`, `pkg-config`, `wayland-client`, `wayland-server`, `xcb`, `xcb-composite`, `xcb-res`, `xcb-xfixes`, `xkbcommon`

## Talks to

- CLI [[docs/graph/components/flsound|flsound]]: `flsound`
- CLI [[docs/graph/components/snappel|snappel]]: `snappel`
- Wayland: `fl_screencopy_frame`, `fl_screencopy_manager`, `fl_shell`, `wl_compositor`, `wl_data_device_manager`, `wl_output`, `wl_seat`, `wl_shm`, `wl_subcompositor`, `xdg_popup`, `xdg_positioner`, `xdg_surface`, `xdg_toplevel`, `xdg_wm_base`, `xwayland_shell_v1`, `xwayland_surface_v1`, `zwp_linux_buffer_params_v1`, `zwp_linux_dmabuf_v1`, `zxdg_decoration_manager_v1`, `zxdg_toplevel_decoration_v1`

## Used by

- [[docs/graph/components/flgreet|flgreet]] (cli)
- [[docs/graph/components/freebws|freebws]] (hdr)

## Installed by

- [[docs/graph/packages/flcomp|pkg/flcomp]] (recipe `recipes/flcomp/recipe`)
- also copied from src/flcomp by [[docs/graph/packages/fldisplay|pkg/fldisplay]]
- also copied from src/flcomp by [[docs/graph/packages/fllock|pkg/fllock]]
- also copied from src/flcomp by [[docs/graph/packages/flpanel|pkg/flpanel]]
- also copied from src/flcomp by [[docs/graph/packages/flsettings|pkg/flsettings]]
- also copied from src/flcomp by [[docs/graph/packages/snappel|pkg/snappel]]

## Docs

- [[docs/manual/01-introduction|01-introduction]]
- [[docs/manual/04-the-desktop|04-the-desktop]]
- [[docs/manual/05-keyboard-shortcuts|05-keyboard-shortcuts]]
- [[docs/manual/09-troubleshooting|09-troubleshooting]]
- [[docs/manual/10-architecture|10-architecture]]
- [[docs/manual/11-boot-process|11-boot-process]]
- [[docs/manual/12-filesystem-layout|12-filesystem-layout]]
- [[docs/manual/13-packages-flpkg|13-packages-flpkg]]
- [[docs/manual/14-services-init|14-services-init]]
- [[docs/manual/15-users-security|15-users-security]]
- [[docs/manual/17-hardware-drivers|17-hardware-drivers]]
- [[docs/manual/18-performance-ram|18-performance-ram]]
- [[docs/manual/19-admin-reference|19-admin-reference]]
- [[docs/manual/20-building-from-source|20-building-from-source]]
- [[docs/manual/21-recipes|21-recipes]]
- [[docs/manual/22-flui-toolkit|22-flui-toolkit]]
- [[docs/manual/23-compositor-protocols|23-compositor-protocols]]
- [[docs/manual/25-testing|25-testing]]
- [[docs/manual/26-contributing|26-contributing]]
- [[docs/manual/27-freebws-internals|27-freebws-internals]]
- [[docs/manual/28-roadmap|28-roadmap]]
- [[docs/manual/29-glossary|29-glossary]]

## Recent changes

- `b469866` Round 22: flpkg one command for every source, signed online repo, fixes, new docs
- `e62d37c` Round 21: boot stick ownership, Load into RAM, FreeBWS GPU freeze fix, flboot entries
- `27f10f9` Round 20: Wi-Fi drivers, NVIDIA phase 1, flat splash, FreeBWS watchdogs, fixes
- `d3131d5` Round 19: eject/cwd fix, app icons, server-side decorations, stride fix
- `1e1c899` Round 18: shutdown hardening, visible launch errors, honest tests, manual, website

See also: [src/flcomp/README](../../../src/flcomp/README) · [01-introduction](../../manual/01-introduction.md) · [04-the-desktop](../../manual/04-the-desktop.md) · [05-keyboard-shortcuts](../../manual/05-keyboard-shortcuts.md) · [AGENT-MAP](../AGENT-MAP.md) · [INDEX](../INDEX.md)
