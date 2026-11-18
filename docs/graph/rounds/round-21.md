---
tags: [round]
---
# Round 21

#round

Commit `e62d37c`: Round 21: boot stick ownership, Load into RAM, FreeBWS GPU freeze fix, flboot entries

129 files changed.

## Components touched

- [[docs/graph/components/flboot|flboot]]: 10 files, +292 -7
  - `src/flboot/Makefile` (+1 -1)
  - `src/flboot/README` (+8 -0)
  - [[docs/graph/files/flboot/boot.c|src/flboot/boot.c]] (+2 -0)
  - [[docs/graph/files/flboot/efi.h|src/flboot/efi.h]] (+10 -1)
  - [[docs/graph/files/flboot/flboot.h|src/flboot/flboot.h]] (+10 -0)
  - [[docs/graph/files/flboot/main.c|src/flboot/main.c]] (+1 -0)
  - [[docs/graph/files/flboot/menu.c|src/flboot/menu.c]] (+39 -4)
  - [[docs/graph/files/flboot/other.c|src/flboot/other.c]] (+196 -0)
  - `src/flboot/tests/hostmenu.c` (+11 -1)
  - `src/flboot/tests/menu-test.sh` (+14 -0)
- [[docs/graph/components/flcomp|flcomp]]: 5 files, +383 -1
  - `src/flcomp/Makefile` (+9 -1)
  - `src/flcomp/README` (+7 -0)
  - `src/flcomp/tests/csd.sh` (+117 -0)
  - `src/flcomp/tests/csdclient.c` (+242 -0)
  - [[docs/graph/files/flcomp/xdg.c|src/flcomp/xdg.c]] (+8 -0)
- [[docs/graph/components/flfiles|flfiles]]: 1 file, +27 -7
  - [[docs/graph/files/flfiles/flfiles.c|src/flfiles/flfiles.c]] (+27 -7)
- [[docs/graph/components/flgm|flgm]]: 4 files, +171 -5
  - `src/flgm/README` (+1 -1)
  - [[docs/graph/files/flgm/gm.h|src/flgm/gm.h]] (+3 -1)
  - [[docs/graph/files/flgm/proc.c|src/flgm/proc.c]] (+120 -2)
  - `src/flgm/tests/fake-test.c` (+47 -1)
- [[docs/graph/components/flinitrd|flinitrd]]: 2 files, +197 -19
  - `src/flinitrd/README` (+8 -0)
  - [[docs/graph/files/flinitrd/flinitrd.c|src/flinitrd/flinitrd.c]] (+189 -19)
- [[docs/graph/components/flmount|flmount]]: 1 file, +17 -10
  - [[docs/graph/files/flmount/flmount.c|src/flmount/flmount.c]] (+17 -10)
- [[docs/graph/components/flpanel|flpanel]]: 2 files, +22 -0
  - `src/flpanel/README` (+4 -0)
  - [[docs/graph/files/flpanel/panel.c|src/flpanel/panel.c]] (+18 -0)
- [[docs/graph/components/flportal|flportal]]: 2 files, +39 -5
  - `src/flportal/README` (+10 -0)
  - [[docs/graph/files/flportal/filechooser.c|src/flportal/filechooser.c]] (+29 -5)
- [[docs/graph/components/freebws|freebws]]: 1 file, +23 -4
  - [[docs/graph/files/freebws/freebws.c|src/freebws/freebws.c]] (+23 -4)

## Packages touched

- [[docs/graph/packages/efibootmgr|pkg/efibootmgr]]: `recipes/efibootmgr/recipe`
- [[docs/graph/packages/efivar|pkg/efivar]]: `recipes/efivar/recipe`
- [[docs/graph/packages/flboot|pkg/flboot]]: `recipes/flboot/recipe`
- [[docs/graph/packages/flcomp|pkg/flcomp]]: `recipes/flcomp/recipe`
- [[docs/graph/packages/flfiles|pkg/flfiles]]: `recipes/flfiles/recipe`
- [[docs/graph/packages/flgm|pkg/flgm]]: `recipes/flgm/recipe`
- [[docs/graph/packages/flinitrd|pkg/flinitrd]]: `recipes/flinitrd/recipe`
- [[docs/graph/packages/flmount|pkg/flmount]]: `recipes/flmount/recipe`
- [[docs/graph/packages/flpanel|pkg/flpanel]]: `recipes/flpanel/recipe`
- [[docs/graph/packages/flportal|pkg/flportal]]: `recipes/flportal/recipe`
- [[docs/graph/packages/freebws|pkg/freebws]]: `recipes/freebws/recipe`
- [[docs/graph/packages/wpebackend-fdo|pkg/wpebackend-fdo]]: `recipes/wpebackend-fdo/recipe`, `recipes/wpebackend-fdo/wpebackend-fdo-1.16.1-surface-feedback.patch`

## Other files

- `build/51-qemu.sh` (+9 -2)
- `build/53-iso.sh` (+1 -0)
- `build/live-run.py` (+26 -1)
- `build/m21-all.sh` (+59 -0)
- `build/m21-features.sh` (+87 -0)
- `build/m21-flboot.sh` (+3 -0)
- `build/m21-freebws.sh` (+59 -0)
- `build/m21/flboot-fw.py` (+226 -0)
- `build/m21/mkstick.py` (+177 -0)
- `build/m21/oidc-testsite.py` (+218 -0)
- `build/m21/portal-client.c` (+65 -0)
- `build/m21/portal-folder.cmds` (+58 -0)
- `build/m21/stick-boot.cmds` (+59 -0)
- `build/m21/taskbar-tip.cmds` (+17 -0)
- `build/m21/toram-menu.cmds` (+9 -0)
- `build/m21/toram-off.cmds` (+5 -0)
- `build/m21/toram.cmds` (+53 -0)
- `build/m21/vm/boot.sh` (+12 -0)
- `build/m21/vm/closetab.sh` (+18 -0)
- `build/m21/vm/findbtn.py` (+33 -0)
- `build/m21/vm/flow.sh` (+8 -0)
- `build/m21/vm/g.py` (+85 -0)
- `build/m21/vm/hidden.sh` (+18 -0)
- `build/m21/vm/loop.sh` (+10 -0)
- `build/m21/vm/shot.sh` (+4 -0)
- `build/m21/vm/startvm.sh` (+9 -0)
- `build/m21/vm/verdict.py` (+19 -0)
- `docs/PLAN-installer.md` (+16 -0)
- `docs/ROADMAP-NEXT.md` (+11 -0)
- `docs/m7/m10-accent-blue-settings-1G.png` (+- --)
- `docs/m7/m10-archive-1G.png` (+- --)
- `docs/m7/m10-desktop-1G.png` (+- --)
- `docs/m7/m10-desktop-wallpaper-1G.png` (+- --)
- `docs/m7/m10-end-1G.png` (+- --)
- `docs/m7/m10-files-open-png-1G.png` (+- --)
- `docs/m7/m10-files-open-txt-1G.png` (+- --)
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
- `docs/manual/02-getting-started.md` (+40 -0)
- `docs/manual/07-files-drives-usb.md` (+15 -8)
- `docs/manual/11-boot-process.md` (+4 -2)
- `docs/manual/25-testing.md` (+16 -0)
- `docs/manual/27-freebws-internals.md` (+34 -0)
- `sources.list` (+3 -0)
- `sources.lock` (+2 -0)

See also: [INDEX](../INDEX.md) · [Round 20](round-20.md) · [Round 22](round-22.md)
