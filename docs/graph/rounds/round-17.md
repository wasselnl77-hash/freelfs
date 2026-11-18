---
tags: [round]
---
# Round 17

#round

Commit `18b0ace`: Round 17: Bluetooth, printing, display settings, night light, desktop QoL

140 files changed.

## Components touched

- [[docs/graph/components/flcomp|flcomp]]: 19 files, +1630 -355
  - `src/flcomp/Makefile` (+1 -1)
  - `src/flcomp/README` (+25 -1)
  - [[docs/graph/files/flcomp/anim.c|src/flcomp/anim.c]] (+2 -2)
  - [[docs/graph/files/flcomp/comp.h|src/flcomp/comp.h]] (+38 -6)
  - [[docs/graph/files/flcomp/config.c|src/flcomp/config.c]] (+51 -7)
  - [[docs/graph/files/flcomp/drm.c|src/flcomp/drm.c]] (+508 -235)
  - [[docs/graph/files/flcomp/fl-shell-client-protocol.c|src/flcomp/fl-shell-client-protocol.c]] (+19 -9)
  - [[docs/graph/files/flcomp/fl-shell-client-protocol.h|src/flcomp/fl-shell-client-protocol.h]] (+110 -0)
  - [[docs/graph/files/flcomp/fl-shell-protocol.c|src/flcomp/fl-shell-protocol.c]] (+19 -9)
  - [[docs/graph/files/flcomp/fl-shell-protocol.h|src/flcomp/fl-shell-protocol.h]] (+97 -0)
  - [[docs/graph/files/flcomp/flshell.c|src/flcomp/flshell.c]] (+29 -2)
  - [[docs/graph/files/flcomp/main.c|src/flcomp/main.c]] (+5 -3)
  - [[docs/graph/files/flcomp/nightlight.c|src/flcomp/nightlight.c]] (+132 -0)
  - [[docs/graph/files/flcomp/osd.c|src/flcomp/osd.c]] (+1 -1)
  - [[docs/graph/files/flcomp/output.c|src/flcomp/output.c]] (+510 -62)
  - `src/flcomp/protocol/fl-shell.xml` (+65 -1)
  - [[docs/graph/files/flcomp/seat.c|src/flcomp/seat.c]] (+2 -2)
  - [[docs/graph/files/flcomp/surface.c|src/flcomp/surface.c]] (+11 -9)
  - [[docs/graph/files/flcomp/xdg.c|src/flcomp/xdg.c]] (+5 -5)
- [[docs/graph/components/fldisplay|fldisplay]]: 5 files, +1281 -0
  - `src/fldisplay/Makefile` (+34 -0)
  - [[docs/graph/files/fldisplay/fl-shell-client-protocol.c|src/fldisplay/fl-shell-client-protocol.c]] (+97 -0)
  - [[docs/graph/files/fldisplay/fl-shell-client-protocol.h|src/fldisplay/fl-shell-client-protocol.h]] (+809 -0)
  - [[docs/graph/files/fldisplay/fldisplay.c|src/fldisplay/fldisplay.c]] (+285 -0)
  - `src/fldisplay/tests/check.sh` (+56 -0)
- [[docs/graph/components/flpanel|flpanel]]: 6 files, +1359 -12
  - `src/flpanel/Makefile` (+2 -1)
  - [[docs/graph/files/flpanel/deskicons.c|src/flpanel/deskicons.c]] (+186 -8)
  - [[docs/graph/files/flpanel/fileinfo.c|src/flpanel/fileinfo.c]] (+578 -0)
  - [[docs/graph/files/flpanel/flpanel.h|src/flpanel/flpanel.h]] (+22 -1)
  - [[docs/graph/files/flpanel/panel.c|src/flpanel/panel.c]] (+27 -2)
  - [[docs/graph/files/flpanel/quick.c|src/flpanel/quick.c]] (+544 -0)
- [[docs/graph/components/flpdf|flpdf]]: 1 file, +33 -1
  - [[docs/graph/files/flpdf/flpdf.c|src/flpdf/flpdf.c]] (+33 -1)
- [[docs/graph/components/flsettings|flsettings]]: 13 files, +2430 -315
  - `src/flsettings/Makefile` (+2 -1)
  - `src/flsettings/README` (+18 -8)
  - [[docs/graph/files/flsettings/bluetooth.c|src/flsettings/bluetooth.c]] (+477 -0)
  - [[docs/graph/files/flsettings/bluetooth.h|src/flsettings/bluetooth.h]] (+18 -0)
  - [[docs/graph/files/flsettings/cmd.c|src/flsettings/cmd.c]] (+342 -0)
  - [[docs/graph/files/flsettings/cmd.h|src/flsettings/cmd.h]] (+42 -0)
  - [[docs/graph/files/flsettings/display.c|src/flsettings/display.c]] (+862 -0)
  - [[docs/graph/files/flsettings/display.h|src/flsettings/display.h]] (+19 -0)
  - [[docs/graph/files/flsettings/flsettings.c|src/flsettings/flsettings.c]] (+62 -21)
  - [[docs/graph/files/flsettings/pages.c|src/flsettings/pages.c]] (+72 -280)
  - [[docs/graph/files/flsettings/pages.h|src/flsettings/pages.h]] (+20 -5)
  - [[docs/graph/files/flsettings/printers.c|src/flsettings/printers.c]] (+478 -0)
  - [[docs/graph/files/flsettings/printers.h|src/flsettings/printers.h]] (+18 -0)
- [[docs/graph/components/flsocketd|flsocketd]]: 3 files, +185 -0
  - `src/flsocketd/Makefile` (+25 -0)
  - [[docs/graph/files/flsocketd/flsocketd.c|src/flsocketd/flsocketd.c]] (+134 -0)
  - `src/flsocketd/tests/check.sh` (+26 -0)
- [[docs/graph/components/flview|flview]]: 1 file, +33 -0
  - [[docs/graph/files/flview/flview.c|src/flview/flview.c]] (+33 -0)

## Packages touched

- [[docs/graph/packages/avahi|pkg/avahi]]: `recipes/avahi/recipe`
- [[docs/graph/packages/bluez|pkg/bluez]]: `recipes/bluez/recipe`
- [[docs/graph/packages/cups|pkg/cups]]: `recipes/cups/recipe`
- [[docs/graph/packages/flcomp|pkg/flcomp]]: `recipes/flcomp/recipe`
- [[docs/graph/packages/fldisplay|pkg/fldisplay]]: `recipes/fldisplay/recipe`
- [[docs/graph/packages/flpanel|pkg/flpanel]]: `recipes/flpanel/recipe`
- [[docs/graph/packages/flpdf|pkg/flpdf]]: `recipes/flpdf/recipe`
- [[docs/graph/packages/flsettings|pkg/flsettings]]: `recipes/flsettings/recipe`
- [[docs/graph/packages/flsocketd|pkg/flsocketd]]: `recipes/flsocketd/recipe`
- [[docs/graph/packages/flview|pkg/flview]]: `recipes/flview/recipe`
- [[docs/graph/packages/initscripts|pkg/initscripts]]: `recipes/initscripts/recipe`
- [[docs/graph/packages/libdaemon|pkg/libdaemon]]: `recipes/libdaemon/recipe`
- [[docs/graph/packages/libusb|pkg/libusb]]: `recipes/libusb/recipe`
- [[docs/graph/packages/pipewire|pkg/pipewire]]: `recipes/pipewire/recipe`
- [[docs/graph/packages/sbc|pkg/sbc]]: `recipes/sbc/recipe`

## Other files

- `.gitignore` (+1 -0)
- `build/m17-features.sh` (+60 -0)
- `build/m17/bluetooth.cmds` (+12 -0)
- `build/m17/display.cmds` (+26 -0)
- `build/m17/gui1.cmds` (+4 -0)
- `build/m17/gui2.cmds` (+39 -0)
- `build/m17/gui3.cmds` (+5 -0)
- `build/m17/pages.cmds` (+30 -0)
- `build/m17/printing.cmds` (+36 -0)
- `config/bluez/90-flbluetooth.rules` (+3 -0)
- `config/bluez/seat-dbus.conf` (+20 -0)
- `config/cups/flcupsd` (+28 -0)
- `config/cups/freelfs.convs` (+3 -0)
- `config/cups/freelfs.types` (+2 -0)
- `config/cups/pdftopdf` (+6 -0)
- `config/cups/pdftopwg` (+11 -0)
- `config/etc/rc.conf` (+1 -1)
- `config/etc/rc.d/init.d/bluetooth` (+47 -0)
- `config/etc/rc.d/init.d/cups` (+30 -0)
- `docs/AGENT-RULES.md` (+7 -0)
- `docs/ROADMAP-NEXT.md` (+2 -0)
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
- `sources.list` (+7 -0)
- `sources.lock` (+6 -0)

See also: [INDEX](../INDEX.md) · [Round 16](round-16.md) · [Round 18](round-18.md)
