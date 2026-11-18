---
tags: [round]
---
# Round 18

#round

Commit `1e1c899`: Round 18: shutdown hardening, visible launch errors, honest tests, manual, website

152 files changed.

## Components touched

- [[docs/graph/components/flcomp|flcomp]]: 1 file, +0 -0
  - `src/flcomp/tests/xclient` (+- --)
- [[docs/graph/components/flfiles|flfiles]]: 1 file, +32 -2
  - [[docs/graph/files/flfiles/flfiles.c|src/flfiles/flfiles.c]] (+32 -2)
- [[docs/graph/components/flgreet|flgreet]]: 1 file, +76 -5
  - [[docs/graph/files/flgreet/flgreet-ui.c|src/flgreet/flgreet-ui.c]] (+76 -5)
- [[docs/graph/components/flmusic|flmusic]]: 1 file, +29 -4
  - [[docs/graph/files/flmusic/flmusic.c|src/flmusic/flmusic.c]] (+29 -4)
- [[docs/graph/components/flpanel|flpanel]]: 4 files, +86 -17
  - [[docs/graph/files/flpanel/deskicons.c|src/flpanel/deskicons.c]] (+1 -1)
  - [[docs/graph/files/flpanel/fileinfo.c|src/flpanel/fileinfo.c]] (+34 -2)
  - [[docs/graph/files/flpanel/flpanel.h|src/flpanel/flpanel.h]] (+1 -1)
  - [[docs/graph/files/flpanel/flpower.c|src/flpanel/flpower.c]] (+50 -13)
- [[docs/graph/components/flpkg|flpkg]]: 1 file, +0 -0
  - `src/flpkg/tests/test-core` (+- --)
- [[docs/graph/components/flsnap|flsnap]]: 1 file, +0 -0
  - `src/flsnap/flsnap` (+- --)
- [[docs/graph/components/flsplash|flsplash]]: 1 file, +19 -5
  - [[docs/graph/files/flsplash/flsplash.c|src/flsplash/flsplash.c]] (+19 -5)
- [[docs/graph/components/fltheme|fltheme]]: 2 files, +0 -0
  - `src/fltheme/fltheme` (+- --)
  - `src/fltheme/tests/contrast` (+- --)
- [[docs/graph/components/flvideo|flvideo]]: 2 files, +44 -11
  - `src/flvideo/README` (+4 -4)
  - [[docs/graph/files/flvideo/flvideo.c|src/flvideo/flvideo.c]] (+40 -7)
- [[docs/graph/components/flview|flview]]: 1 file, +10 -8
  - [[docs/graph/files/flview/flview.c|src/flview/flview.c]] (+10 -8)
- [[docs/graph/components/freebws|freebws]]: 3 files, +23 -12
  - `src/freebws/README` (+6 -4)
  - [[docs/graph/files/freebws/adblock.c|src/freebws/adblock.c]] (+6 -1)
  - [[docs/graph/files/freebws/freebws.c|src/freebws/freebws.c]] (+11 -7)

## Packages touched

- [[docs/graph/packages/flfiles|pkg/flfiles]]: `recipes/flfiles/recipe`
- [[docs/graph/packages/flgreet|pkg/flgreet]]: `recipes/flgreet/recipe`
- [[docs/graph/packages/flmusic|pkg/flmusic]]: `recipes/flmusic/recipe`
- [[docs/graph/packages/flpanel|pkg/flpanel]]: `recipes/flpanel/recipe`
- [[docs/graph/packages/flsplash|pkg/flsplash]]: `recipes/flsplash/recipe`
- [[docs/graph/packages/flvideo|pkg/flvideo]]: `recipes/flvideo/recipe`
- [[docs/graph/packages/flview|pkg/flview]]: `recipes/flview/recipe`
- [[docs/graph/packages/freebws|pkg/freebws]]: `recipes/freebws/recipe`
- [[docs/graph/packages/initscripts|pkg/initscripts]]: `recipes/initscripts/recipe`

## Other files

- `.gitignore` (+5 -0)
- `build/boot-test.py` (+10 -4)
- `build/iso-test.py` (+31 -12)
- `build/live-run.py` (+126 -11)
- `build/m16-features.sh` (+14 -2)
- `build/m16/files-dclick.cmds` (+1 -1)
- `build/m16/persist-1-create.cmds` (+3 -5)
- `build/m16/persist-2-write.cmds` (+3 -3)
- `build/m16/persist-3-check.cmds` (+2 -2)
- `build/m16/poweroff-menu.cmds` (+2 -2)
- `build/m16/usb-notify.cmds` (+3 -7)
- `build/m16/usb-plug.cmds` (+19 -25)
- `build/m17-features.sh` (+1 -1)
- `build/m18-honest.sh` (+31 -0)
- `build/m18-realiso.py` (+165 -0)
- `build/m18/files-open.cmds` (+35 -0)
- `build/m18/forced-shutdown.cmds` (+9 -0)
- `build/m18/greeter-fresh.cmds` (+14 -0)
- `build/m18/greeter-shutdown.cmds` (+20 -0)
- `build/m18/panel-shutdown.cmds` (+16 -0)
- `config/etc/rc.d/functions` (+52 -0)
- `config/etc/rc.d/rc` (+6 -1)
- `config/etc/rc.d/rc.shutdown` (+26 -13)
- `docs/PLAN-installer.md` (+64 -0)
- `docs/ROADMAP-NEXT.md` (+6 -0)
- `docs/bugscan-2026-11-14/tests-audit.md` (+396 -0)
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
- `docs/m7/m10-install-installed-1-login.png` (+- --)
- `docs/m7/m10-install-installed-2-desktop.png` (+- --)
- `docs/m7/m10-install-installed-3-end.png` (+- --)
- `docs/m7/m10-install-live-1-login.png` (+- --)
- `docs/m7/m10-install-live-10-installing.png` (+- --)
- `docs/m7/m10-install-live-11-done.png` (+- --)
- `docs/m7/m10-install-live-2-menu.png` (+- --)
- `docs/m7/m10-install-live-3-language.png` (+- --)
- `docs/m7/m10-install-live-4-keyboard.png` (+- --)
- `docs/m7/m10-install-live-5-wifi.png` (+- --)
- `docs/m7/m10-install-live-6-disk.png` (+- --)
- `docs/m7/m10-install-live-7-account.png` (+- --)
- `docs/m7/m10-install-live-8-timezone.png` (+- --)
- `docs/m7/m10-install-live-9-summary.png` (+- --)
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
- `docs/manual/01-introduction.md` (+185 -0)
- `docs/manual/02-getting-started.md` (+255 -0)
- `docs/manual/03-installing.md` (+263 -0)
- `docs/manual/04-the-desktop.md` (+351 -0)
- `docs/manual/05-keyboard-shortcuts.md` (+399 -0)
- `docs/manual/06-apps.md` (+409 -0)
- `docs/manual/07-files-drives-usb.md` (+236 -0)
- `docs/manual/08-settings-reference.md` (+328 -0)
- `docs/manual/09-troubleshooting.md` (+287 -0)
- `docs/manual/10-architecture.md` (+165 -0)
- `docs/manual/11-boot-process.md` (+247 -0)
- `docs/manual/12-filesystem-layout.md` (+230 -0)
- `docs/manual/13-packages-flpkg.md` (+279 -0)
- `docs/manual/14-services-init.md` (+184 -0)
- `docs/manual/15-users-security.md` (+195 -0)
- `docs/manual/16-networking.md` (+163 -0)
- `docs/manual/17-hardware-drivers.md` (+201 -0)
- `docs/manual/18-performance-ram.md` (+156 -0)
- `docs/manual/19-admin-reference.md` (+251 -0)
- `docs/manual/20-building-from-source.md` (+374 -0)
- `docs/manual/21-recipes.md` (+342 -0)
- `docs/manual/22-flui-toolkit.md` (+458 -0)
- `docs/manual/23-compositor-protocols.md` (+463 -0)
- `docs/manual/24-writing-an-app.md` (+545 -0)
- `docs/manual/25-testing.md` (+288 -0)
- `docs/manual/26-contributing.md` (+210 -0)
- `docs/manual/27-freebws-internals.md` (+338 -0)
- `docs/manual/28-roadmap.md` (+234 -0)
- `docs/manual/29-glossary.md` (+192 -0)
- `docs/manual/README.md` (+65 -0)
- `site/docs.html` (+70 -0)
- `site/download.html` (+78 -0)
- `site/img/desktop.png` (+- --)
- `site/img/files.png` (+- --)
- `site/img/installer-disk.png` (+- --)
- `site/img/overview.png` (+- --)
- `site/img/settings.png` (+- --)
- `site/img/store.png` (+- --)
- `site/img/terminal.png` (+- --)
- `site/img/themes-dark.png` (+- --)
- `site/index.html` (+99 -0)
- `site/logo.svg` (+5 -0)
- `site/style.css` (+38 -0)
- `site/testers.html` (+73 -0)

See also: [INDEX](../INDEX.md) · [Round 17](round-17.md) · [Round 19](round-19.md)
