---
tags: [round]
---
# Round 19

#round

Commit `d3131d5`: Round 19: eject/cwd fix, app icons, server-side decorations, stride fix

92 files changed.

## Components touched

- [[docs/graph/components/flarchive|flarchive]]: 1 file, +1 -0
  - `src/flarchive/flarchive.desktop` (+1 -0)
- [[docs/graph/components/flcomp|flcomp]]: 13 files, +767 -14
  - `src/flcomp/Makefile` (+22 -4)
  - [[docs/graph/files/flcomp/comp.h|src/flcomp/comp.h]] (+4 -0)
  - [[docs/graph/files/flcomp/main.c|src/flcomp/main.c]] (+1 -0)
  - [[docs/graph/files/flcomp/render.c|src/flcomp/render.c]] (+3 -3)
  - [[docs/graph/files/flcomp/seat.c|src/flcomp/seat.c]] (+6 -0)
  - [[docs/graph/files/flcomp/surface.c|src/flcomp/surface.c]] (+13 -1)
  - `src/flcomp/tests/deco.sh` (+87 -0)
  - `src/flcomp/tests/shmclient.c` (+32 -2)
  - `src/flcomp/tests/stride.sh` (+21 -0)
  - [[docs/graph/files/flcomp/xdg-decoration-unstable-v1-protocol.c|src/flcomp/xdg-decoration-unstable-v1-protocol.c]] (+76 -0)
  - [[docs/graph/files/flcomp/xdg-decoration-unstable-v1-protocol.h|src/flcomp/xdg-decoration-unstable-v1-protocol.h]] (+381 -0)
  - [[docs/graph/files/flcomp/xdg.c|src/flcomp/xdg.c]] (+110 -0)
  - [[docs/graph/files/flcomp/xwdeco.c|src/flcomp/xwdeco.c]] (+11 -4)
- [[docs/graph/components/fledit|fledit]]: 2 files, +20 -1
  - [[docs/graph/files/fledit/fledit.c|src/fledit/fledit.c]] (+19 -1)
  - `src/fledit/fledit.desktop` (+1 -0)
- [[docs/graph/components/flfiles|flfiles]]: 2 files, +51 -0
  - [[docs/graph/files/flfiles/flfiles.c|src/flfiles/flfiles.c]] (+50 -0)
  - `src/flfiles/flfiles.desktop` (+1 -0)
- [[docs/graph/components/flgm|flgm]]: 9 files, +401 -70
  - `src/flgm/README` (+3 -1)
  - `src/flgm/general-manager.desktop` (+1 -0)
  - [[docs/graph/files/flgm/gm.c|src/flgm/gm.c]] (+7 -3)
  - [[docs/graph/files/flgm/gm.h|src/flgm/gm.h]] (+11 -0)
  - [[docs/graph/files/flgm/proc.c|src/flgm/proc.c]] (+87 -34)
  - [[docs/graph/files/flgm/startup.c|src/flgm/startup.c]] (+3 -0)
  - [[docs/graph/files/flgm/sys.c|src/flgm/sys.c]] (+34 -27)
  - `src/flgm/tests/check.sh` (+11 -5)
  - `src/flgm/tests/fake-test.c` (+244 -0)
- [[docs/graph/components/flinstall|flinstall]]: 1 file, +1 -0
  - `src/flinstall/flinstall.desktop` (+1 -0)
- [[docs/graph/components/flmount|flmount]]: 1 file, +21 -12
  - [[docs/graph/files/flmount/flmount.c|src/flmount/flmount.c]] (+21 -12)
- [[docs/graph/components/flmusic|flmusic]]: 1 file, +1 -0
  - `src/flmusic/flmusic.desktop` (+1 -0)
- [[docs/graph/components/flpackages|flpackages]]: 1 file, +1 -0
  - `src/flpackages/flpackages.desktop` (+1 -0)
- [[docs/graph/components/flpanel|flpanel]]: 11 files, +605 -8
  - `src/flpanel/Makefile` (+15 -4)
  - [[docs/graph/files/flpanel/deskicons.c|src/flpanel/deskicons.c]] (+1 -0)
  - [[docs/graph/files/flpanel/dialog.c|src/flpanel/dialog.c]] (+1 -0)
  - [[docs/graph/files/flpanel/fileinfo.c|src/flpanel/fileinfo.c]] (+1 -0)
  - [[docs/graph/files/flpanel/flpanel.h|src/flpanel/flpanel.h]] (+1 -1)
  - [[docs/graph/files/flpanel/icons.c|src/flpanel/icons.c]] (+372 -0)
  - [[docs/graph/files/flpanel/icons.h|src/flpanel/icons.h]] (+20 -0)
  - [[docs/graph/files/flpanel/menu.c|src/flpanel/menu.c]] (+6 -1)
  - [[docs/graph/files/flpanel/panel.c|src/flpanel/panel.c]] (+7 -2)
  - [[docs/graph/files/flpanel/quick.c|src/flpanel/quick.c]] (+1 -0)
  - `src/flpanel/tests/icons_test.c` (+180 -0)
- [[docs/graph/components/flpdf|flpdf]]: 1 file, +1 -0
  - `src/flpdf/flpdf.desktop` (+1 -0)
- [[docs/graph/components/flsettings|flsettings]]: 1 file, +1 -0
  - `src/flsettings/flsettings.desktop` (+1 -0)
- [[docs/graph/components/flsplash|flsplash]]: 2 files, +82 -26
  - `src/flsplash/README` (+6 -5)
  - [[docs/graph/files/flsplash/flsplash.c|src/flsplash/flsplash.c]] (+76 -21)
- [[docs/graph/components/flstore|flstore]]: 1 file, +1 -0
  - `src/flstore/flstore.desktop` (+1 -0)
- [[docs/graph/components/flterm|flterm]]: 1 file, +1 -0
  - `src/flterm/flterm.desktop` (+1 -0)
- [[docs/graph/components/fltheme|fltheme]]: 1 file, +1 -0
  - `src/fltheme/fltheme.desktop` (+1 -0)
- [[docs/graph/components/flvideo|flvideo]]: 1 file, +1 -0
  - `src/flvideo/flvideo.desktop` (+1 -0)
- [[docs/graph/components/flview|flview]]: 1 file, +1 -0
  - `src/flview/flview.desktop` (+1 -0)
- [[docs/graph/components/freebws|freebws]]: 2 files, +22 -8
  - [[docs/graph/files/freebws/freebws.c|src/freebws/freebws.c]] (+21 -8)
  - `src/freebws/freebws.desktop` (+1 -0)
- [[docs/graph/components/snappel|snappel]]: 1 file, +1 -0
  - `src/snappel/snappel.desktop` (+1 -0)

## Packages touched

- [[docs/graph/packages/flarchive|pkg/flarchive]]: `recipes/flarchive/recipe`
- [[docs/graph/packages/flcomp|pkg/flcomp]]: `recipes/flcomp/recipe`
- [[docs/graph/packages/fledit|pkg/fledit]]: `recipes/fledit/recipe`
- [[docs/graph/packages/flfiles|pkg/flfiles]]: `recipes/flfiles/recipe`
- [[docs/graph/packages/flgm|pkg/flgm]]: `recipes/flgm/recipe`
- [[docs/graph/packages/flinstall|pkg/flinstall]]: `recipes/flinstall/recipe`
- [[docs/graph/packages/flmount|pkg/flmount]]: `recipes/flmount/recipe`
- [[docs/graph/packages/flmusic|pkg/flmusic]]: `recipes/flmusic/recipe`
- [[docs/graph/packages/flpackages|pkg/flpackages]]: `recipes/flpackages/recipe`
- [[docs/graph/packages/flpanel|pkg/flpanel]]: `recipes/flpanel/recipe`
- [[docs/graph/packages/flpdf|pkg/flpdf]]: `recipes/flpdf/recipe`
- [[docs/graph/packages/flsettings|pkg/flsettings]]: `recipes/flsettings/recipe`
- [[docs/graph/packages/flsplash|pkg/flsplash]]: `recipes/flsplash/recipe`
- [[docs/graph/packages/flstore|pkg/flstore]]: `recipes/flstore/recipe`
- [[docs/graph/packages/flterm|pkg/flterm]]: `recipes/flterm/recipe`
- [[docs/graph/packages/fltheme|pkg/fltheme]]: `recipes/fltheme/recipe`
- [[docs/graph/packages/flvideo|pkg/flvideo]]: `recipes/flvideo/recipe`
- [[docs/graph/packages/flview|pkg/flview]]: `recipes/flview/recipe`
- [[docs/graph/packages/freebws|pkg/freebws]]: `recipes/freebws/recipe`
- [[docs/graph/packages/papirus-icon-theme|pkg/papirus-icon-theme]]: `recipes/papirus-icon-theme/recipe`
- [[docs/graph/packages/snappel|pkg/snappel]]: `recipes/snappel/recipe`

## Other files

- `build/live-run.py` (+38 -0)
- `build/m19-features.sh` (+43 -0)
- `build/m19/eject-launch.cmds` (+74 -0)
- `build/m19/icons.cmds` (+22 -0)
- `build/papirus-raster.py` (+169 -0)
- `docs/PLAN-quality.md` (+16 -0)
- `docs/ROADMAP-NEXT.md` (+7 -0)
- `docs/manual/07-files-drives-usb.md` (+4 -0)
- `docs/manual/23-compositor-protocols.md` (+1 -0)
- `docs/manual/25-testing.md` (+2 -0)
- `docs/manual/27-freebws-internals.md` (+32 -0)
- `site/docs.html` (+1 -1)
- `site/download.html` (+1 -1)
- `site/index.html` (+1 -1)
- `site/testers.html` (+1 -1)
- `sources.list` (+1 -0)
- `sources.lock` (+1 -0)

See also: [INDEX](../INDEX.md) · [Round 18](round-18.md) · [Round 20](round-20.md)
