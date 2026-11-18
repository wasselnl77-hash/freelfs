---
tags: [round]
---
# Round 16

#round

Commit `ebb6a28`: Round 16: laptop-test fixes, security, USB drives, persistence, desktop QoL

152 files changed.

## Components touched

- [[docs/graph/components/flarchive|flarchive]]: 1 file, +36 -10
  - [[docs/graph/files/flarchive/flarchive.c|src/flarchive/flarchive.c]] (+36 -10)
- [[docs/graph/components/flauth|flauth]]: 4 files, +484 -0
  - `src/flauth/Makefile` (+27 -0)
  - [[docs/graph/files/flauth/flauth-check.h|src/flauth/flauth-check.h]] (+39 -0)
  - [[docs/graph/files/flauth/flauth-dialog.c|src/flauth/flauth-dialog.c]] (+146 -0)
  - [[docs/graph/files/flauth/flauth.c|src/flauth/flauth.c]] (+272 -0)
- [[docs/graph/components/flcomp|flcomp]]: 14 files, +863 -25
  - `src/flcomp/Makefile` (+5 -1)
  - [[docs/graph/files/flcomp/comp.h|src/flcomp/comp.h]] (+28 -1)
  - `src/flcomp/desktop.sh` (+1 -0)
  - [[docs/graph/files/flcomp/dmabuf.c|src/flcomp/dmabuf.c]] (+9 -0)
  - [[docs/graph/files/flcomp/flshell.c|src/flcomp/flshell.c]] (+46 -1)
  - [[docs/graph/files/flcomp/lock.c|src/flcomp/lock.c]] (+10 -0)
  - [[docs/graph/files/flcomp/main.c|src/flcomp/main.c]] (+15 -2)
  - [[docs/graph/files/flcomp/popup.c|src/flcomp/popup.c]] (+382 -0)
  - `src/flcomp/protocol/fl-shell.xml` (+11 -1)
  - [[docs/graph/files/flcomp/render.c|src/flcomp/render.c]] (+4 -0)
  - [[docs/graph/files/flcomp/seat.c|src/flcomp/seat.c]] (+293 -14)
  - [[docs/graph/files/flcomp/surface.c|src/flcomp/surface.c]] (+10 -2)
  - [[docs/graph/files/flcomp/xdg.c|src/flcomp/xdg.c]] (+44 -3)
  - [[docs/graph/files/flcomp/xwsel.c|src/flcomp/xwsel.c]] (+5 -0)
- [[docs/graph/components/flfiles|flfiles]]: 2 files, +1822 -275
  - `src/flfiles/README` (+45 -13)
  - [[docs/graph/files/flfiles/flfiles.c|src/flfiles/flfiles.c]] (+1777 -262)
- [[docs/graph/components/flgreet|flgreet]]: 1 file, +8 -2
  - [[docs/graph/files/flgreet/flgreet.c|src/flgreet/flgreet.c]] (+8 -2)
- [[docs/graph/components/flinitrd|flinitrd]]: 1 file, +81 -6
  - [[docs/graph/files/flinitrd/flinitrd.c|src/flinitrd/flinitrd.c]] (+81 -6)
- [[docs/graph/components/flinstall|flinstall]]: 1 file, +24 -0
  - [[docs/graph/files/flinstall/helper.c|src/flinstall/helper.c]] (+24 -0)
- [[docs/graph/components/flmount|flmount]]: 4 files, +1157 -0
  - `src/flmount/90-flmount.rules` (+6 -0)
  - `src/flmount/Makefile` (+27 -0)
  - [[docs/graph/files/flmount/flmount.c|src/flmount/flmount.c]] (+957 -0)
  - [[docs/graph/files/flmount/flpersist.c|src/flmount/flpersist.c]] (+167 -0)
- [[docs/graph/components/flpackages|flpackages]]: 2 files, +69 -0
  - [[docs/graph/files/flpackages/flauth-check.h|src/flpackages/flauth-check.h]] (+39 -0)
  - [[docs/graph/files/flpackages/flpkg-seat.c|src/flpackages/flpkg-seat.c]] (+30 -0)
- [[docs/graph/components/flpanel|flpanel]]: 11 files, +1630 -91
  - `src/flpanel/Makefile` (+1 -1)
  - `src/flpanel/README` (+19 -1)
  - [[docs/graph/files/flpanel/battery.c|src/flpanel/battery.c]] (+15 -7)
  - [[docs/graph/files/flpanel/ctxmenu.c|src/flpanel/ctxmenu.c]] (+160 -0)
  - [[docs/graph/files/flpanel/deskicons.c|src/flpanel/deskicons.c]] (+759 -68)
  - [[docs/graph/files/flpanel/dialog.c|src/flpanel/dialog.c]] (+367 -0)
  - [[docs/graph/files/flpanel/flpanel.h|src/flpanel/flpanel.h]] (+45 -2)
  - [[docs/graph/files/flpanel/flpower.c|src/flpanel/flpower.c]] (+84 -7)
  - [[docs/graph/files/flpanel/menu.c|src/flpanel/menu.c]] (+122 -5)
  - [[docs/graph/files/flpanel/notify.c|src/flpanel/notify.c]] (+4 -0)
  - [[docs/graph/files/flpanel/panel.c|src/flpanel/panel.c]] (+54 -0)
- [[docs/graph/components/flpkg|flpkg]]: 10 files, +93 -14
  - [[docs/graph/files/flpkg/cmd_pkg.c|src/flpkg/cmd_pkg.c]] (+11 -3)
  - `src/flpkg/flpkg` (+- --)
  - [[docs/graph/files/flpkg/pkginfo.c|src/flpkg/pkginfo.c]] (+49 -0)
  - [[docs/graph/files/flpkg/repo.c|src/flpkg/repo.c]] (+20 -0)
  - [[docs/graph/files/flpkg/repo.h|src/flpkg/repo.h]] (+2 -0)
  - [[docs/graph/files/flpkg/tar.c|src/flpkg/tar.c]] (+1 -1)
  - `src/flpkg/tests/recipes/layout/recipe` (+4 -4)
  - `src/flpkg/tests/test-build.sh` (+1 -1)
  - `src/flpkg/tests/test-hook.sh` (+1 -1)
  - `src/flpkg/tests/test-txn.sh` (+4 -4)
- [[docs/graph/components/flportal|flportal]]: 1 file, +34 -1
  - [[docs/graph/files/flportal/main.c|src/flportal/main.c]] (+34 -1)
- [[docs/graph/components/flsettings|flsettings]]: 3 files, +124 -30
  - [[docs/graph/files/flsettings/about.c|src/flsettings/about.c]] (+88 -14)
  - [[docs/graph/files/flsettings/flsettings.c|src/flsettings/flsettings.c]] (+19 -14)
  - [[docs/graph/files/flsettings/wifi.c|src/flsettings/wifi.c]] (+17 -2)
- [[docs/graph/components/flsnap|flsnap]]: 2 files, +68 -0
  - [[docs/graph/files/flsnap/flauth-check.h|src/flsnap/flauth-check.h]] (+39 -0)
  - [[docs/graph/files/flsnap/flsnap.c|src/flsnap/flsnap.c]] (+29 -0)
- [[docs/graph/components/flui|flui]]: 3 files, +87 -25
  - [[docs/graph/files/flui/conf.c|src/flui/conf.c]] (+46 -14)
  - [[docs/graph/files/flui/text.c|src/flui/text.c]] (+15 -6)
  - [[docs/graph/files/flui/ui.c|src/flui/ui.c]] (+26 -5)
- [[docs/graph/components/flview|flview]]: 1 file, +8 -4
  - [[docs/graph/files/flview/flview.c|src/flview/flview.c]] (+8 -4)
- [[docs/graph/components/freebws|freebws]]: 3 files, +169 -22
  - `src/freebws/README` (+30 -5)
  - `src/freebws/data/count.js` (+4 -8)
  - [[docs/graph/files/freebws/freebws.c|src/freebws/freebws.c]] (+135 -9)

## Packages touched

- [[docs/graph/packages/flarchive|pkg/flarchive]]: `recipes/flarchive/recipe`
- [[docs/graph/packages/flauth|pkg/flauth]]: `recipes/flauth/recipe`
- [[docs/graph/packages/flcomp|pkg/flcomp]]: `recipes/flcomp/recipe`
- [[docs/graph/packages/flfiles|pkg/flfiles]]: `recipes/flfiles/recipe`
- [[docs/graph/packages/flgreet|pkg/flgreet]]: `recipes/flgreet/recipe`
- [[docs/graph/packages/flinitrd|pkg/flinitrd]]: `recipes/flinitrd/recipe`
- [[docs/graph/packages/flinstall|pkg/flinstall]]: `recipes/flinstall/recipe`
- [[docs/graph/packages/flmount|pkg/flmount]]: `recipes/flmount/recipe`
- [[docs/graph/packages/flpackages|pkg/flpackages]]: `recipes/flpackages/recipe`
- [[docs/graph/packages/flpanel|pkg/flpanel]]: `recipes/flpanel/recipe`
- [[docs/graph/packages/flpkg|pkg/flpkg]]: `recipes/flpkg/recipe`
- [[docs/graph/packages/flportal|pkg/flportal]]: `recipes/flportal/recipe`
- [[docs/graph/packages/flsettings|pkg/flsettings]]: `recipes/flsettings/recipe`
- [[docs/graph/packages/flsnap|pkg/flsnap]]: `recipes/flsnap/recipe`
- [[docs/graph/packages/flui|pkg/flui]]: `recipes/flui/recipe`
- [[docs/graph/packages/flview|pkg/flview]]: `recipes/flview/recipe`
- [[docs/graph/packages/freebws|pkg/freebws]]: `recipes/freebws/recipe`
- [[docs/graph/packages/initscripts|pkg/initscripts]]: `recipes/initscripts/recipe`

## Other files

- `.gitignore` (+1 -0)
- `build/60-packages.sh` (+1 -1)
- `build/iso-test-disk.py` (+1 -1)
- `build/live-run.py` (+190 -0)
- `build/m16-features.sh` (+58 -0)
- `build/m16/child.html` (+3 -0)
- `build/m16/clipboard.cmds` (+45 -0)
- `build/m16/files-dclick.cmds` (+9 -0)
- `build/m16/flauth.cmds` (+3 -0)
- `build/m16/flauth.tty` (+13 -0)
- `build/m16/freebws-popup.cmds` (+21 -0)
- `build/m16/persist-1-create.cmds` (+8 -0)
- `build/m16/persist-2-write.cmds` (+8 -0)
- `build/m16/persist-3-check.cmds` (+3 -0)
- `build/m16/pop.html` (+4 -0)
- `build/m16/poweroff-menu.cmds` (+10 -0)
- `build/m16/usb-notify.cmds` (+16 -0)
- `build/m16/usb-plug.cmds` (+37 -0)
- `build/m16/window-qol.cmds` (+26 -0)
- `config/etc/rc.d/init.d/network` (+2 -2)
- `config/etc/rc.d/rc` (+10 -0)
- `config/etc/rc.d/rc.shutdown` (+5 -5)
- `config/etc/rc.d/rc.sysinit` (+18 -0)
- `docs/PLAN.md` (+23 -0)
- `docs/ROADMAP-NEXT.md` (+8 -0)
- `docs/bugscan-2026-11-13/A-compositor-session.md` (+180 -0)
- `docs/bugscan-2026-11-13/B-browser-portal.md` (+133 -0)
- `docs/bugscan-2026-11-13/C-toolkit-panel-settings.md` (+123 -0)
- `docs/bugscan-2026-11-13/D-shell-pkg-boot-install.md` (+152 -0)
- `docs/bugscan-2026-11-13/E-apps-files-usb.md` (+243 -0)
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

See also: [INDEX](../INDEX.md) · [Round 15](round-15.md) · [Round 17](round-17.md)
