---
tags: [round]
---
# Round 13

#round

Commit `b0715e7`: Round 13: compositor animations, Themes app (fltheme), light scheme

128 files changed.

## Components touched

- [[docs/graph/components/flcomp|flcomp]]: 24 files, +1885 -60
  - `src/flcomp/Makefile` (+9 -3)
  - `src/flcomp/README` (+29 -4)
  - [[docs/graph/files/flcomp/anim.c|src/flcomp/anim.c]] (+942 -0)
  - [[docs/graph/files/flcomp/comp.h|src/flcomp/comp.h]] (+13 -5)
  - [[docs/graph/files/flcomp/config.c|src/flcomp/config.c]] (+24 -2)
  - [[docs/graph/files/flcomp/fl-shell-client-protocol.c|src/flcomp/fl-shell-client-protocol.c]] (+10 -8)
  - [[docs/graph/files/flcomp/fl-shell-client-protocol.h|src/flcomp/fl-shell-client-protocol.h]] (+21 -0)
  - [[docs/graph/files/flcomp/fl-shell-protocol.c|src/flcomp/fl-shell-protocol.c]] (+10 -8)
  - [[docs/graph/files/flcomp/fl-shell-protocol.h|src/flcomp/fl-shell-protocol.h]] (+23 -0)
  - [[docs/graph/files/flcomp/flshell.c|src/flcomp/flshell.c]] (+12 -1)
  - [[docs/graph/files/flcomp/main.c|src/flcomp/main.c]] (+1 -0)
  - [[docs/graph/files/flcomp/osd.c|src/flcomp/osd.c]] (+1 -1)
  - [[docs/graph/files/flcomp/overview.c|src/flcomp/overview.c]] (+18 -6)
  - [[docs/graph/files/flcomp/render.c|src/flcomp/render.c]] (+26 -10)
  - [[docs/graph/files/flcomp/surface.c|src/flcomp/surface.c]] (+9 -4)
  - `src/flcomp/tests/anim.sh` (+242 -0)
  - `src/flcomp/tests/pss.sh` (+39 -0)
  - `src/flcomp/tests/xclient` (+- --)
  - [[docs/graph/files/flcomp/xdg.c|src/flcomp/xdg.c]] (+39 -6)
  - [[docs/graph/files/flcomp/xwayland-shell-protocol.c|src/flcomp/xwayland-shell-protocol.c]] (+72 -0)
  - [[docs/graph/files/flcomp/xwayland-shell-protocol.h|src/flcomp/xwayland-shell-protocol.h]] (+333 -0)
  - [[docs/graph/files/flcomp/xwdeco.c|src/flcomp/xwdeco.c]] (+10 -1)
  - [[docs/graph/files/flcomp/xwm-int.h|src/flcomp/xwm-int.h]] (+1 -1)
  - [[docs/graph/files/flcomp/xwm.h|src/flcomp/xwm.h]] (+1 -0)
- [[docs/graph/components/flpanel|flpanel]]: 4 files, +54 -14
  - `src/flpanel/README` (+2 -1)
  - [[docs/graph/files/flpanel/deskicons.c|src/flpanel/deskicons.c]] (+15 -1)
  - [[docs/graph/files/flpanel/flpanel.h|src/flpanel/flpanel.h]] (+3 -0)
  - [[docs/graph/files/flpanel/panel.c|src/flpanel/panel.c]] (+34 -12)
- [[docs/graph/components/flpkg|flpkg]]: 2 files, +0 -0
  - `src/flpkg/flpkg` (+- --)
  - `src/flpkg/tests/test-core` (+- --)
- [[docs/graph/components/flsettings|flsettings]]: 2 files, +27 -3
  - `src/flsettings/README` (+3 -0)
  - [[docs/graph/files/flsettings/flsettings.c|src/flsettings/flsettings.c]] (+24 -3)
- [[docs/graph/components/fltheme|fltheme]]: 14 files, +1702 -0
  - `src/fltheme/Makefile` (+50 -0)
  - `src/fltheme/README` (+49 -0)
  - `src/fltheme/fltheme` (+- --)
  - [[docs/graph/files/fltheme/fltheme.c|src/fltheme/fltheme.c]] (+732 -0)
  - `src/fltheme/fltheme.desktop` (+7 -0)
  - [[docs/graph/files/fltheme/fltheme.h|src/fltheme/fltheme.h]] (+39 -0)
  - [[docs/graph/files/fltheme/hints.c|src/fltheme/hints.c]] (+133 -0)
  - [[docs/graph/files/fltheme/presets.c|src/fltheme/presets.c]] (+255 -0)
  - `src/fltheme/tests/contrast` (+- --)
  - `src/fltheme/tests/contrast.c` (+87 -0)
  - `src/fltheme/tests/frames.py` (+65 -0)
  - `src/fltheme/tests/presets.sh` (+69 -0)
  - `src/fltheme/tests/shot.sh` (+14 -0)
  - `src/fltheme/tests/switch.py` (+202 -0)
- [[docs/graph/components/flui|flui]]: 1 file, +7 -2
  - `src/flui/README` (+7 -2)

## Packages touched

- [[docs/graph/packages/flcomp|pkg/flcomp]]: `recipes/flcomp/recipe`
- [[docs/graph/packages/flpanel|pkg/flpanel]]: `recipes/flpanel/recipe`
- [[docs/graph/packages/flpkg|pkg/flpkg]]: `recipes/flpkg/recipe`
- [[docs/graph/packages/flsettings|pkg/flsettings]]: `recipes/flsettings/recipe`
- [[docs/graph/packages/fltheme|pkg/fltheme]]: `recipes/fltheme/recipe`
- [[docs/graph/packages/flui|pkg/flui]]: `recipes/flui/recipe`

## Other files

- `build/m10-features.tty` (+21 -0)
- `docs/INTEGRATION-TODO.md` (+8 -0)
- `docs/RAM-PROFILE.md` (+8 -0)
- `docs/ROADMAP-NEXT.md` (+16 -0)
- `docs/m7/fltheme-anim-close-mid.png` (+- --)
- `docs/m7/fltheme-anim-maximize-mid.png` (+- --)
- `docs/m7/fltheme-anim-minimize-mid.png` (+- --)
- `docs/m7/fltheme-anim-open-mid.png` (+- --)
- `docs/m7/fltheme-anim-overview-mid.png` (+- --)
- `docs/m7/fltheme-anim-workspace-slide-mid.png` (+- --)
- `docs/m7/fltheme-animations-light.png` (+- --)
- `docs/m7/fltheme-colours-dark.png` (+- --)
- `docs/m7/fltheme-colours-light.png` (+- --)
- `docs/m7/fltheme-dark-again.png` (+- --)
- `docs/m7/fltheme-dark.png` (+- --)
- `docs/m7/fltheme-fonts-large-plex-mono.png` (+- --)
- `docs/m7/fltheme-fonts-light.png` (+- --)
- `docs/m7/fltheme-light.png` (+- --)
- `docs/m7/fltheme-panel-light.png` (+- --)
- `docs/m7/fltheme-panel-top-compact.png` (+- --)
- `docs/m7/fltheme-presets-dark.png` (+- --)
- `docs/m7/fltheme-presets-light-typing.png` (+- --)
- `docs/m7/fltheme-presets-light.png` (+- --)
- `docs/m7/fltheme-ram-profile-1G.txt` (+110 -0)
- `docs/m7/fltheme-reset.png` (+- --)
- `docs/m7/fltheme-top-compact.png` (+- --)
- `docs/m7/fltheme-windows-light.png` (+- --)
- `docs/m7/freebws-back-1G.png` (+- --)
- `docs/m7/freebws-bare-1G.png` (+- --)
- `docs/m7/freebws-discard-1G.png` (+- --)
- `docs/m7/freebws-discard-back-1G.png` (+- --)
- `docs/m7/freebws-dssl-1G.png` (+- --)
- `docs/m7/freebws-escape-1G.png` (+- --)
- `docs/m7/freebws-https-1G.png` (+- --)
- `docs/m7/freebws-link-bg-1G.png` (+- --)
- `docs/m7/freebws-link-blank-1G.png` (+- --)
- `docs/m7/freebws-link-closed-1G.png` (+- --)
- `docs/m7/freebws-link-open-1G.png` (+- --)
- `docs/m7/freebws-link-reopen-1G.png` (+- --)
- `docs/m7/freebws-native-1G.png` (+- --)
- `docs/m7/freebws-start-1G.png` (+- --)
- `docs/m7/freebws-tabs-closed-1G.png` (+- --)
- `docs/m7/freebws-tabs-prev-1G.png` (+- --)
- `docs/m7/freebws-tabs3-1G.png` (+- --)
- `docs/m7/freebws-tabs4-1G.png` (+- --)
- `docs/m7/freebws-test-1G.png` (+- --)
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
- `docs/m7/m10-music-1G.png` (+- --)
- `docs/m7/m10-music-mp3-1G.png` (+- --)
- `docs/m7/m10-overview-1G.png` (+- --)
- `docs/m7/m10-pdf-1G.png` (+- --)
- `docs/m7/m10-print-1G.png` (+- --)
- `docs/m7/m10-store-1G.png` (+- --)
- `docs/m7/m10-store-updates-1G.png` (+- --)
- `docs/m7/m10-themes-dark-1G.png` (+- --)
- `docs/m7/m10-themes-light-1G.png` (+- --)
- `docs/m7/m10-themes-reset-1G.png` (+- --)
- `docs/m7/m10-themes-slide-mid-1G.png` (+- --)
- `docs/m7/m10-themes-top-compact-1G.png` (+- --)
- `docs/m7/m10-video-1G.png` (+- --)
- `docs/m7/m10-ws1-back-1G.png` (+- --)
- `docs/m7/m10-ws1-files-1G.png` (+- --)
- `docs/m7/m10-ws2-empty-1G.png` (+- --)

See also: [INDEX](../INDEX.md) · [Round 12](round-12.md) · [Round 14](round-14.md)
