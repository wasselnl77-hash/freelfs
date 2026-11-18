---
tags: [round]
---
# Round 22

#round

Commit `b469866`: Round 22: flpkg one command for every source, signed online repo, fixes, new docs

119 files changed.

## Components touched

- [[docs/graph/components/flcomp|flcomp]]: 2 files, +56 -1
  - `src/flcomp/session` (+11 -0)
  - [[docs/graph/files/flcomp/xdg.c|src/flcomp/xdg.c]] (+45 -1)
- [[docs/graph/components/flfiles|flfiles]]: 1 file, +92 -4
  - [[docs/graph/files/flfiles/flfiles.c|src/flfiles/flfiles.c]] (+92 -4)
- [[docs/graph/components/flgm|flgm]]: 7 files, +203 -39
  - [[docs/graph/files/flgm/gm.c|src/flgm/gm.c]] (+2 -0)
  - [[docs/graph/files/flgm/gm.h|src/flgm/gm.h]] (+10 -2)
  - [[docs/graph/files/flgm/gmui.h|src/flgm/gmui.h]] (+2 -0)
  - [[docs/graph/files/flgm/perf.c|src/flgm/perf.c]] (+46 -16)
  - [[docs/graph/files/flgm/sys.c|src/flgm/sys.c]] (+82 -17)
  - `src/flgm/tests/check.sh` (+1 -1)
  - `src/flgm/tests/fake-test.c` (+60 -3)
- [[docs/graph/components/flpackages|flpackages]]: 2 files, +17 -6
  - [[docs/graph/files/flpackages/flpkg-seat.c|src/flpackages/flpkg-seat.c]] (+14 -6)
  - `src/flpackages/tests/seat.sh` (+3 -0)
- [[docs/graph/components/flpanel|flpanel]]: 1 file, +20 -7
  - [[docs/graph/files/flpanel/panel.c|src/flpanel/panel.c]] (+20 -7)
- [[docs/graph/components/flpkg|flpkg]]: 3 files, +489 -2
  - `src/flpkg/Makefile` (+1 -0)
  - [[docs/graph/files/flpkg/main.c|src/flpkg/main.c]] (+52 -2)
  - `src/flpkg/sources.sh` (+436 -0)
- [[docs/graph/components/flportal|flportal]]: 5 files, +131 -3
  - `src/flportal/README` (+12 -0)
  - [[docs/graph/files/flportal/filechooser.c|src/flportal/filechooser.c]] (+5 -0)
  - [[docs/graph/files/flportal/flportal.h|src/flportal/flportal.h]] (+2 -0)
  - [[docs/graph/files/flportal/main.c|src/flportal/main.c]] (+107 -2)
  - `src/flportal/tests/portal.sh` (+5 -1)
- [[docs/graph/components/flreport|flreport]]: 1 file, +7 -0
  - `src/flreport/flreport` (+7 -0)
- [[docs/graph/components/flstore|flstore]]: 3 files, +149 -12
  - [[docs/graph/files/flstore/flstore.c|src/flstore/flstore.c]] (+92 -10)
  - [[docs/graph/files/flstore/flstore.h|src/flstore/flstore.h]] (+7 -2)
  - [[docs/graph/files/flstore/views.c|src/flstore/views.c]] (+50 -0)
- [[docs/graph/components/snappel|snappel]]: 1 file, +8 -0
  - [[docs/graph/files/snappel/img.c|src/snappel/img.c]] (+8 -0)

## Packages touched

- [[docs/graph/packages/distrobox|pkg/distrobox]]: `recipes/distrobox/recipe`
- [[docs/graph/packages/flcomp|pkg/flcomp]]: `recipes/flcomp/recipe`
- [[docs/graph/packages/flfiles|pkg/flfiles]]: `recipes/flfiles/recipe`
- [[docs/graph/packages/flgm|pkg/flgm]]: `recipes/flgm/recipe`
- [[docs/graph/packages/flpackages|pkg/flpackages]]: `recipes/flpackages/recipe`
- [[docs/graph/packages/flpanel|pkg/flpanel]]: `recipes/flpanel/recipe`
- [[docs/graph/packages/flpkg|pkg/flpkg]]: `recipes/flpkg/recipe`
- [[docs/graph/packages/flportal|pkg/flportal]]: `recipes/flportal/recipe`
- [[docs/graph/packages/flreport|pkg/flreport]]: `recipes/flreport/recipe`
- [[docs/graph/packages/flstore|pkg/flstore]]: `recipes/flstore/recipe`
- [[docs/graph/packages/initscripts|pkg/initscripts]]: `recipes/initscripts/recipe`
- [[docs/graph/packages/linux|pkg/linux]]: `recipes/linux/recipe`
- [[docs/graph/packages/linux-lts|pkg/linux-lts]]: `recipes/linux-lts/recipe`
- [[docs/graph/packages/snappel|pkg/snappel]]: `recipes/snappel/recipe`

## Other files

- `.gitignore` (+7 -0)
- `build/70-publish-repo.sh` (+118 -0)
- `build/m21/vm/g.py` (+10 -0)
- `build/m21/vm/vars.fd` (+- --)
- `build/m21/vm/vm.log` (+0 -0)
- `build/m22-all.sh` (+91 -0)
- `build/m22/flpkg-sources.sh` (+168 -0)
- `build/m22/live.cmds` (+69 -0)
- `build/m22/portal-log.cmds` (+31 -0)
- `build/m22/repo-update.sh` (+76 -0)
- `build/m22/ui.cmds` (+68 -0)
- `config/etc/inittab` (+2 -1)
- `config/etc/rc.d/serial-getty` (+15 -0)
- `config/flpkg/keys/freelfs-repo.pub` (+2 -0)
- `config/flpkg/repo-url` (+1 -0)
- `config/kernel/freelfs.config` (+27 -0)
- `docs/COMMANDS.md` (+1652 -0)
- `docs/CONFIG.md` (+1268 -0)
- `docs/ROADMAP-NEXT.md` (+8 -0)
- `docs/TROUBLESHOOTING.md` (+1280 -0)
- `docs/flpkg.md` (+30 -1)
- `docs/m7/m10-accent-blue-settings-1G.png` (+- --)
- `docs/m7/m10-archive-1G.png` (+- --)
- `docs/m7/m10-desktop-1G.png` (+- --)
- `docs/m7/m10-desktop-wallpaper-1G.png` (+- --)
- `docs/m7/m10-end-1G.png` (+- --)
- `docs/m7/m10-files-open-png-1G.png` (+- --)
- `docs/m7/m10-files-open-txt-1G.png` (+- --)
- `docs/m7/m10-flboot-ask-1G.png` (+- --)
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
- `docs/manual/13-packages-flpkg.md` (+4 -4)
- `recipes-wip/nix/README` (+17 -0)
- `recipes-wip/podman/README` (+14 -0)
- `sources.list` (+3 -0)
- `sources.lock` (+1 -0)

See also: [INDEX](../INDEX.md) · [Round 21](round-21.md)
