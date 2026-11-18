---
tags: [round]
---
# Round 15

#round

Commit `c9604b6`: Round 15: folder layout (/usr real, /sysflfs alias, homes in /home); hardware build

98 files changed.

## Components touched

- [[docs/graph/components/flcomp|flcomp]]: 1 file, +1 -1
  - `src/flcomp/desktop.sh` (+1 -1)
- [[docs/graph/components/flcrash|flcrash]]: 1 file, +1 -1
  - [[docs/graph/files/flcrash/flcrash.c|src/flcrash/flcrash.c]] (+1 -1)
- [[docs/graph/components/flinstall|flinstall]]: 5 files, +38 -64
  - `src/flinstall/README` (+4 -6)
  - [[docs/graph/files/flinstall/common.h|src/flinstall/common.h]] (+0 -9)
  - [[docs/graph/files/flinstall/flinstall.c|src/flinstall/flinstall.c]] (+1 -1)
  - [[docs/graph/files/flinstall/helper.c|src/flinstall/helper.c]] (+11 -28)
  - `src/flinstall/tests/image-test.sh` (+22 -20)
- [[docs/graph/components/freebws|freebws]]: 2 files, +12 -10
  - `src/freebws/README` (+4 -3)
  - [[docs/graph/files/freebws/freebws.c|src/freebws/freebws.c]] (+8 -7)

## Packages touched

- [[docs/graph/packages/base-files|pkg/base-files]]: `recipes/base-files/recipe`
- [[docs/graph/packages/flatpak|pkg/flatpak]]: `recipes/flatpak/recipe`
- [[docs/graph/packages/gaming-config|pkg/gaming-config]]: `recipes/gaming-config/flgaming`
- [[docs/graph/packages/initscripts|pkg/initscripts]]: `recipes/initscripts/recipe`
- [[docs/graph/packages/libclc|pkg/libclc]]: `recipes/libclc/recipe`
- [[docs/graph/packages/llvm|pkg/llvm]]: `recipes/llvm/recipe`
- [[docs/graph/packages/mesa|pkg/mesa]]: `recipes/mesa/recipe`
- [[docs/graph/packages/shadow|pkg/shadow]]: `recipes/shadow/recipe`
- [[docs/graph/packages/spirv-llvm-translator|pkg/spirv-llvm-translator]]: `recipes/spirv-llvm-translator/recipe`

## Other files

- `README.md` (+10 -5)
- `build/50-image.sh` (+7 -5)
- `build/52-usb.sh` (+3 -3)
- `build/53-iso.sh` (+7 -7)
- `build/60-packages.sh` (+4 -1)
- `build/boot-test.py` (+11 -10)
- `build/iso-test.py` (+3 -3)
- `build/m10-features.cmds` (+4 -4)
- `build/m10-features.tty` (+22 -12)
- `build/m10-install.py` (+8 -3)
- `build/m10-snap1.cmds` (+1 -1)
- `build/m11-gpu.tty` (+2 -2)
- `build/m11-x11-gpu.cmds` (+1 -1)
- `build/m11-x11.tty` (+2 -2)
- `build/m12-sandbox.cmds` (+2 -2)
- `build/m12-sandbox.sh` (+1 -1)
- `build/m12-sandbox.tty` (+4 -4)
- `build/m9-features.tty` (+3 -3)
- `config/etc/fstab` (+3 -4)
- `config/etc/profile` (+2 -2)
- `config/etc/rc.d/functions` (+1 -1)
- `config/etc/rc.d/rc.sysinit` (+2 -2)
- `docs/HARDWARE.md` (+19 -8)
- `docs/PLAN.md` (+10 -3)
- `docs/ROADMAP-NEXT.md` (+2 -0)
- `docs/SECURITY.md` (+2 -2)
- `docs/m7/m10-accent-blue-settings-1G.png` (+- --)
- `docs/m7/m10-archive-1G.png` (+- --)
- `docs/m7/m10-desktop-1G.png` (+- --)
- `docs/m7/m10-desktop-wallpaper-1G.png` (+- --)
- `docs/m7/m10-end-1G.png` (+- --)
- `docs/m7/m10-files-open-png-1G.png` (+- --)
- `docs/m7/m10-files-open-txt-1G.png` (+- --)
- `docs/m7/m10-flboot-ask-2G.png` (+- --)
- `docs/m7/m10-flboot-list-2G.png` (+- --)
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
- `docs/m7/m10-snap-desktop-2G.png` (+- --)
- `docs/m7/m10-snap-gm-2G.png` (+- --)
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

See also: [INDEX](../INDEX.md) · [Round 14](round-14.md) · [Round 16](round-16.md)
