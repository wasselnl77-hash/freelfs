---
tags: [round]
---
# Round 14

#round

Commit `2c2183d`: Round 14: security updates, kernel 7.3.4 (rolling), WebKit sandbox; Intel/AMD prep

121 files changed.

## Components touched

- [[docs/graph/components/flboot|flboot]]: 3 files, +34 -2
  - `src/flboot/README` (+3 -1)
  - [[docs/graph/files/flboot/conf.c|src/flboot/conf.c]] (+18 -1)
  - `src/flboot/tests/menu-test.sh` (+13 -0)
- [[docs/graph/components/flcomp|flcomp]]: 2 files, +38 -9
  - `src/flcomp/desktop.sh` (+4 -0)
  - [[docs/graph/files/flcomp/media.c|src/flcomp/media.c]] (+34 -9)
- [[docs/graph/components/flinstall|flinstall]]: 1 file, +15 -3
  - [[docs/graph/files/flinstall/helper.c|src/flinstall/helper.c]] (+15 -3)
- [[docs/graph/components/flsettings|flsettings]]: 1 file, +34 -9
  - [[docs/graph/files/flsettings/pages.c|src/flsettings/pages.c]] (+34 -9)
- [[docs/graph/components/flterm|flterm]]: 1 file, +7 -0
  - [[docs/graph/files/flterm/main.c|src/flterm/main.c]] (+7 -0)
- [[docs/graph/components/freebws|freebws]]: 3 files, +50 -8
  - `src/freebws/README` (+6 -2)
  - [[docs/graph/files/freebws/downloads.c|src/freebws/downloads.c]] (+3 -0)
  - [[docs/graph/files/freebws/freebws.c|src/freebws/freebws.c]] (+41 -6)

## Packages touched

- [[docs/graph/packages/brotli|pkg/brotli]]: `recipes/brotli/recipe`
- [[docs/graph/packages/expat|pkg/expat]]: `recipes/expat/recipe`
- [[docs/graph/packages/flboot|pkg/flboot]]: `recipes/flboot/recipe`
- [[docs/graph/packages/flcomp|pkg/flcomp]]: `recipes/flcomp/recipe`
- [[docs/graph/packages/flinstall|pkg/flinstall]]: `recipes/flinstall/recipe`
- [[docs/graph/packages/flsettings|pkg/flsettings]]: `recipes/flsettings/recipe`
- [[docs/graph/packages/flterm|pkg/flterm]]: `recipes/flterm/recipe`
- [[docs/graph/packages/freebws|pkg/freebws]]: `recipes/freebws/recipe`
- [[docs/graph/packages/glib|pkg/glib]]: `recipes/glib/recipe`
- [[docs/graph/packages/gmmlib|pkg/gmmlib]]: `recipes/gmmlib/recipe`
- [[docs/graph/packages/gzip|pkg/gzip]]: `recipes/gzip/recipe`
- [[docs/graph/packages/intel-media-driver|pkg/intel-media-driver]]: `recipes/intel-media-driver/recipe`
- [[docs/graph/packages/libclc|pkg/libclc]]: `recipes/libclc/recipe`
- [[docs/graph/packages/libsoup|pkg/libsoup]]: `recipes/libsoup/recipe`
- [[docs/graph/packages/libxml2|pkg/libxml2]]: `recipes/libxml2/recipe`
- [[docs/graph/packages/linux|pkg/linux]]: `recipes/linux/recipe`
- [[docs/graph/packages/linux-firmware|pkg/linux-firmware]]: `recipes/linux-firmware/recipe`
- [[docs/graph/packages/linux-lts|pkg/linux-lts]]: `recipes/linux-lts/recipe`
- [[docs/graph/packages/llvm|pkg/llvm]]: `recipes/llvm/recipe`
- [[docs/graph/packages/mesa|pkg/mesa]]: `recipes/mesa/recipe`
- [[docs/graph/packages/microcode|pkg/microcode]]: `recipes/microcode/recipe`
- [[docs/graph/packages/openssl|pkg/openssl]]: `recipes/openssl/recipe`
- [[docs/graph/packages/patch|pkg/patch]]: `recipes/patch/recipe`
- [[docs/graph/packages/pcre2|pkg/pcre2]]: `recipes/pcre2/recipe`
- [[docs/graph/packages/sof-firmware|pkg/sof-firmware]]: `recipes/sof-firmware/recipe`
- [[docs/graph/packages/spirv-llvm-translator|pkg/spirv-llvm-translator]]: `recipes/spirv-llvm-translator/recipe`
- [[docs/graph/packages/sqlite|pkg/sqlite]]: `recipes/sqlite/recipe`
- [[docs/graph/packages/wpewebkit|pkg/wpewebkit]]: `recipes/wpewebkit/recipe`

## Other files

- `build/53-iso.sh` (+4 -0)
- `build/cve-check.py` (+85 -9)
- `build/cve-kernel.py` (+343 -0)
- `build/cve-names.conf` (+61 -16)
- `build/m12-sandbox.cmds` (+11 -0)
- `build/m12-sandbox.sh` (+39 -0)
- `build/m12-sandbox.tty` (+32 -0)
- `config/kernel/freelfs.config` (+219 -0)
- `docs/CVE-KERNEL.md` (+175 -0)
- `docs/CVE-REPORT.md` (+51 -186)
- `docs/HARDWARE.md` (+85 -0)
- `docs/INTEGRATION-TODO.md` (+3 -1)
- `docs/PLAN.md` (+4 -3)
- `docs/ROADMAP-NEXT.md` (+4 -0)
- `docs/SECURITY.md` (+65 -7)
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
- `docs/m7/m10-snap-login-1G.png` (+- --)
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
- `docs/m7/m11-gpu-desktop-1G.png` (+- --)
- `docs/m7/m11-gpu-https-1G.png` (+- --)
- `docs/m7/m11-gpu-start-1G.png` (+- --)
- `docs/m7/m11-gpu-wiki-1G.png` (+- --)
- `docs/m7/m11-x11-desktop-1G.png` (+- --)
- `docs/m7/m11-x11-xclient-1G.png` (+- --)
- `docs/m7/m11-x11-xclient-snapped-1G.png` (+- --)
- `docs/m7/m12-sandbox-download-1G-default.png` (+- --)
- `docs/m7/m12-sandbox-download-1G-diag.png` (+- --)
- `docs/m7/m12-sandbox-https-1G-default.png` (+- --)
- `docs/m7/m12-sandbox-https-1G-diag.png` (+- --)
- `docs/m7/m12-sandbox-start-1G-default.png` (+- --)
- `sources.list` (+19 -14)
- `sources.lock` (+18 -13)

See also: [INDEX](../INDEX.md) · [Round 13](round-13.md) · [Round 15](round-15.md)
