---
tags: [round]
---
# Round 20

#round

Commit `27f10f9`: Round 20: Wi-Fi drivers, NVIDIA phase 1, flat splash, FreeBWS watchdogs, fixes

149 files changed.

## Components touched

- [[docs/graph/components/flboot|flboot]]: 3 files, +27 -0
  - `src/flboot/README` (+4 -0)
  - [[docs/graph/files/flboot/conf.c|src/flboot/conf.c]] (+17 -0)
  - `src/flboot/tests/menu-test.sh` (+6 -0)
- [[docs/graph/components/flcomp|flcomp]]: 2 files, +106 -21
  - [[docs/graph/files/flcomp/drm.c|src/flcomp/drm.c]] (+95 -21)
  - [[docs/graph/files/flcomp/xdg.c|src/flcomp/xdg.c]] (+11 -0)
- [[docs/graph/components/flfiles|flfiles]]: 1 file, +26 -2
  - [[docs/graph/files/flfiles/flfiles.c|src/flfiles/flfiles.c]] (+26 -2)
- [[docs/graph/components/flgm|flgm]]: 12 files, +781 -93
  - `src/flgm/Makefile` (+2 -2)
  - `src/flgm/README` (+13 -3)
  - [[docs/graph/files/flgm/gm.c|src/flgm/gm.c]] (+75 -44)
  - [[docs/graph/files/flgm/gm.h|src/flgm/gm.h]] (+41 -1)
  - [[docs/graph/files/flgm/gmui.h|src/flgm/gmui.h]] (+2 -1)
  - [[docs/graph/files/flgm/perf.c|src/flgm/perf.c]] (+57 -5)
  - [[docs/graph/files/flgm/proc.c|src/flgm/proc.c]] (+151 -25)
  - [[docs/graph/files/flgm/rdr.c|src/flgm/rdr.c]] (+263 -0)
  - [[docs/graph/files/flgm/sys.c|src/flgm/sys.c]] (+37 -2)
  - `src/flgm/tests/check.sh` (+4 -4)
  - `src/flgm/tests/data-test.c` (+4 -0)
  - `src/flgm/tests/fake-test.c` (+132 -6)
- [[docs/graph/components/flgreet|flgreet]]: 1 file, +37 -3
  - [[docs/graph/files/flgreet/flgreet-ui.c|src/flgreet/flgreet-ui.c]] (+37 -3)
- [[docs/graph/components/flmount|flmount]]: 1 file, +133 -9
  - [[docs/graph/files/flmount/flmount.c|src/flmount/flmount.c]] (+133 -9)
- [[docs/graph/components/flpanel|flpanel]]: 3 files, +41 -3
  - [[docs/graph/files/flpanel/deskicons.c|src/flpanel/deskicons.c]] (+10 -1)
  - [[docs/graph/files/flpanel/menu.c|src/flpanel/menu.c]] (+27 -1)
  - [[docs/graph/files/flpanel/panel.c|src/flpanel/panel.c]] (+4 -1)
- [[docs/graph/components/flreport|flreport]]: 1 file, +29 -0
  - `src/flreport/flreport` (+29 -0)
- [[docs/graph/components/flsplash|flsplash]]: 6 files, +606 -57
  - `src/flsplash/Makefile` (+15 -3)
  - `src/flsplash/README` (+45 -13)
  - [[docs/graph/files/flsplash/flauth-check.h|src/flsplash/flauth-check.h]] (+39 -0)
  - [[docs/graph/files/flsplash/flsplash-set.c|src/flsplash/flsplash-set.c]] (+139 -0)
  - [[docs/graph/files/flsplash/flsplash.c|src/flsplash/flsplash.c]] (+319 -41)
  - `src/flsplash/tests/check.sh` (+49 -0)
- [[docs/graph/components/fltheme|fltheme]]: 6 files, +336 -14
  - `src/fltheme/Makefile` (+7 -2)
  - `src/fltheme/README` (+4 -0)
  - [[docs/graph/files/fltheme/fltheme.c|src/fltheme/fltheme.c]] (+180 -11)
  - [[docs/graph/files/fltheme/fltheme.h|src/fltheme/fltheme.h]] (+3 -0)
  - [[docs/graph/files/fltheme/presets.c|src/fltheme/presets.c]] (+17 -1)
  - `src/fltheme/tests/accent.c` (+125 -0)
- [[docs/graph/components/flui|flui]]: 6 files, +316 -27
  - `src/flui/Makefile` (+9 -2)
  - [[docs/graph/files/flui/flui.h|src/flui/flui.h]] (+8 -0)
  - [[docs/graph/files/flui/internal.h|src/flui/internal.h]] (+4 -0)
  - [[docs/graph/files/flui/test-scroll.c|src/flui/test-scroll.c]] (+200 -0)
  - [[docs/graph/files/flui/ui.c|src/flui/ui.c]] (+81 -10)
  - [[docs/graph/files/flui/wl.c|src/flui/wl.c]] (+14 -15)
- [[docs/graph/components/freebws|freebws]]: 2 files, +253 -42
  - [[docs/graph/files/freebws/adblock.c|src/freebws/adblock.c]] (+84 -25)
  - [[docs/graph/files/freebws/freebws.c|src/freebws/freebws.c]] (+169 -17)

## Packages touched

- [[docs/graph/packages/base-files|pkg/base-files]]: `recipes/base-files/recipe`
- [[docs/graph/packages/flarchive|pkg/flarchive]]: `recipes/flarchive/recipe`
- [[docs/graph/packages/flauth|pkg/flauth]]: `recipes/flauth/recipe`
- [[docs/graph/packages/flboot|pkg/flboot]]: `recipes/flboot/recipe`
- [[docs/graph/packages/flcomp|pkg/flcomp]]: `recipes/flcomp/recipe`
- [[docs/graph/packages/flcrash|pkg/flcrash]]: `recipes/flcrash/recipe`
- [[docs/graph/packages/fledit|pkg/fledit]]: `recipes/fledit/recipe`
- [[docs/graph/packages/flfiles|pkg/flfiles]]: `recipes/flfiles/recipe`
- [[docs/graph/packages/flgm|pkg/flgm]]: `recipes/flgm/recipe`
- [[docs/graph/packages/flgreet|pkg/flgreet]]: `recipes/flgreet/recipe`
- [[docs/graph/packages/flinstall|pkg/flinstall]]: `recipes/flinstall/recipe`
- [[docs/graph/packages/fllock|pkg/fllock]]: `recipes/fllock/recipe`
- [[docs/graph/packages/flmount|pkg/flmount]]: `recipes/flmount/recipe`
- [[docs/graph/packages/flmusic|pkg/flmusic]]: `recipes/flmusic/recipe`
- [[docs/graph/packages/flpackages|pkg/flpackages]]: `recipes/flpackages/recipe`
- [[docs/graph/packages/flpanel|pkg/flpanel]]: `recipes/flpanel/recipe`
- [[docs/graph/packages/flpdf|pkg/flpdf]]: `recipes/flpdf/recipe`
- [[docs/graph/packages/flportal|pkg/flportal]]: `recipes/flportal/recipe`
- [[docs/graph/packages/flreport|pkg/flreport]]: `recipes/flreport/recipe`
- [[docs/graph/packages/flsettings|pkg/flsettings]]: `recipes/flsettings/recipe`
- [[docs/graph/packages/flsetup|pkg/flsetup]]: `recipes/flsetup/recipe`
- [[docs/graph/packages/flsplash|pkg/flsplash]]: `recipes/flsplash/recipe`
- [[docs/graph/packages/flstore|pkg/flstore]]: `recipes/flstore/recipe`
- [[docs/graph/packages/flterm|pkg/flterm]]: `recipes/flterm/recipe`
- [[docs/graph/packages/fltheme|pkg/fltheme]]: `recipes/fltheme/recipe`
- [[docs/graph/packages/flui|pkg/flui]]: `recipes/flui/recipe`
- [[docs/graph/packages/flvideo|pkg/flvideo]]: `recipes/flvideo/recipe`
- [[docs/graph/packages/flview|pkg/flview]]: `recipes/flview/recipe`
- [[docs/graph/packages/freebws|pkg/freebws]]: `recipes/freebws/recipe`
- [[docs/graph/packages/initscripts|pkg/initscripts]]: `recipes/initscripts/recipe`
- [[docs/graph/packages/linux|pkg/linux]]: `recipes/linux/recipe`
- [[docs/graph/packages/linux-firmware|pkg/linux-firmware]]: `recipes/linux-firmware/recipe`
- [[docs/graph/packages/linux-lts|pkg/linux-lts]]: `recipes/linux-lts/recipe`
- [[docs/graph/packages/mesa|pkg/mesa]]: `recipes/mesa/recipe`
- [[docs/graph/packages/pciutils|pkg/pciutils]]: `recipes/pciutils/recipe`
- [[docs/graph/packages/snappel|pkg/snappel]]: `recipes/snappel/recipe`
- [[docs/graph/packages/usbutils|pkg/usbutils]]: `recipes/usbutils/recipe`

## Other files

- `build/live-run.py` (+6 -0)
- `build/m20-features.sh` (+66 -0)
- `build/m20/desktop-rubber.cmds` (+27 -0)
- `build/m20/dmlinear.c` (+70 -0)
- `build/m20/freebws-watchdog.cmds` (+38 -0)
- `build/m20/nomodeset.cmds` (+30 -0)
- `build/m20/ppmcheck.py` (+18 -0)
- `build/m20/splash-style.cmds` (+14 -0)
- `build/m20/stall-server.py` (+19 -0)
- `build/m20/ventoy-eject.cmds` (+41 -0)
- `build/m20/ventoy-replug.cmds` (+61 -0)
- `build/m20/wifi-modules.cmds` (+17 -0)
- `config/etc/profile` (+1 -1)
- `config/etc/rc.d/functions` (+4 -3)
- `config/etc/rc.d/rc` (+1 -0)
- `config/kernel/freelfs.config` (+34 -0)
- `docs/HARDWARE.md` (+83 -3)
- `docs/PLAN-installer.md` (+10 -0)
- `docs/PLAN-quality.md` (+14 -0)
- `docs/ROADMAP-NEXT.md` (+10 -0)
- `docs/m7/m10-accent-blue-settings-1G.png` (+- --)
- `docs/m7/m10-archive-1G.png` (+- --)
- `docs/m7/m10-desktop-1G.png` (+- --)
- `docs/m7/m10-desktop-wallpaper-1G.png` (+- --)
- `docs/m7/m10-end-1G.png` (+- --)
- `docs/m7/m10-files-open-png-1G.png` (+- --)
- `docs/m7/m10-files-open-txt-1G.png` (+- --)
- `docs/m7/m10-flboot-ask-1G.png` (+- --)
- `docs/m7/m10-flboot-list-1G.png` (+- --)
- `docs/m7/m10-fledit-1G.png` (+- --)
- `docs/m7/m10-gm-performance-1G.png` (+- --)
- `docs/m7/m10-gm-processes-1G.png` (+- --)
- `docs/m7/m10-gm-startup-1G.png` (+- --)
- `docs/m7/m10-image-1G.png` (+- --)
- `docs/m7/m10-image-wallpaper-1G.png` (+- --)
- `docs/m7/m10-music-1G.png` (+- --)
- `docs/m7/m10-music-mp3-1G.png` (+- --)
- `docs/m7/m10-overview-1G.png` (+- --)
- `docs/m7/m10-pdf-1G.png` (+- --)
- `docs/m7/m10-print-1G.png` (+- --)
- `docs/m7/m10-snap-desktop-1G.png` (+- --)
- `docs/m7/m10-snap-gm-1G.png` (+- --)
- `docs/m7/m10-store-1G.png` (+- --)
- `docs/m7/m10-store-updates-1G.png` (+- --)
- `docs/m7/m10-terminal-home-1G.png` (+- --)
- `docs/m7/m10-themes-dark-1G.png` (+- --)
- `docs/m7/m10-themes-light-1G.png` (+- --)
- `docs/m7/m10-themes-reset-1G.png` (+- --)
- `docs/m7/m10-themes-slide-mid-1G.png` (+- --)
- `docs/m7/m10-themes-top-compact-1G.png` (+- --)
- `docs/m7/m10-video-1G.png` (+- --)
- `docs/m7/m10-ws1-back-1G.png` (+- --)
- `docs/m7/m10-ws1-files-1G.png` (+- --)
- `docs/m7/m10-ws2-empty-1G.png` (+- --)
- `docs/m7/m11-x11-desktop-1G.png` (+- --)
- `docs/m7/m11-x11-xclient-1G.png` (+- --)
- `docs/m7/m11-x11-xclient-snapped-1G.png` (+- --)
- `docs/m7/m12-sandbox-download-1G-default.png` (+- --)
- `docs/m7/m12-sandbox-https-1G-default.png` (+- --)
- `docs/m7/m12-sandbox-start-1G-default.png` (+- --)
- `docs/m7/m20-greeter-flat-takeover.png` (+- --)
- `docs/m7/m20-rubber-r19-middrag.png` (+- --)
- `docs/manual/07-files-drives-usb.md` (+7 -0)
- `docs/manual/23-compositor-protocols.md` (+11 -0)
- `docs/manual/25-testing.md` (+18 -2)
- `docs/manual/27-freebws-internals.md` (+24 -0)
- `sources.list` (+3 -0)
- `sources.lock` (+2 -0)

See also: [INDEX](../INDEX.md) · [Round 19](round-19.md) · [Round 21](round-21.md)
