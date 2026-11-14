# 27. FreeBWS internals: the browser on WPE WebKit

Summary: FreeBWS is the FreeLFS web browser. It is a libflui window with a tab row and a toolbar, and the page is drawn by
WPE WebKit in sandboxed web processes. This chapter explains the architecture, the ad blocker, the sandbox, the portal, the
settings and the known bugs, and how to help.

## Contents

1. What FreeBWS is, and where it lives
2. Architecture: processes and windows
3. How a page reaches the screen (shm and dmabuf)
4. Tabs and discarding
5. The ad blocker (adblock, abp.c, abpconv)
6. DSSL: native styles (libdssl)
7. Pages, the internal `freebws://` pages and the bridge
8. Downloads, bookmarks, dark mode, search, errors
9. The web process sandbox
10. The portal and the screenshot path
11. Memory
12. Settings: `~/.config/freebws/config` and the environment
13. Command line
14. Known bugs and their status
15. Known gaps (not there yet)
16. How to help

---

## 1. What FreeBWS is, and where it lives

FreeBWS (`src/freebws`, recipe `freebws`, group `desktop`) is the FreeLFS web browser. It uses WPE WebKit (the `wpewebkit`
package, with `libwpe` and `wpebackend-fdo`), in a libflui window. The browser is light, so that it can run on a computer with
1 GB of RAM. Firefox (Flatpak) is the recommended browser for everyday use (`docs/PLAN-installer.md`).

The source is about 7,300 lines of C in `src/freebws`:

| File | Lines | What it holds |
|---|---|---|
| `freebws.c` | about 3,160 | the main program: window, toolbar, tabs, WebKit setup, memory, sandbox, policy, downloads, dark mode, command line |
| `adblock.c` | about 1,130 | the browser side of the ad blocker: loads the compiled rules, injects cosmetic CSS and scripts per site, the weekly list update |
| `abp.c` | about 970 | converts ABP (EasyList) filter lists to WebKit content rules, cosmetic rules and scriptlets |
| `abpconv.c` | about 60 | a command line tool around `abp.c` (`abpconv OUTDIR LIST...`) |
| `pages.c` | about 460 | the internal `freebws://` pages and their message bridge |
| `downloads.c` | about 460 | the download list and its panel |
| `bookmarks.c` | about 430 | bookmarks, folders, import of an HTML export |
| `tabstrip.c` | about 110 | the tab row |
| `popup.c` | about 70 | a small popup surface |
| `shm.c` | about 20 | copies a frame into a `wl_shm` buffer |
| `config.c` | about 150 | the settings file and the directories |
| `libdssl/` | (own directory) | native DSSL, the style language (section 6) |
| `data/` | (files) | the internal pages (`start`, `settings`, `downloads`, `bookmarks`, `error`), the test pages, and `lists/` (the filter lists) |

Build: `make` in `src/freebws`. Its Makefile uses pkg-config for `flui`, `wayland-client`, `wpe-webkit-2.0`, `wpebackend-fdo-1.0`,
`glib-2.0`, `dssl`, `libsoup-3.0`, `egl` and `gbm`. The recipe (`recipes/freebws/recipe`) also records `dssl=0.16.3` (the DSSL version it bundles).

## 2. Architecture: processes and windows

```
 FreeBWS (UI process, one per window)                       flcomp
 +-------------------------------------------------+        +----------------+
 | libflui window: tab row, toolbar, progress      |        | the screen     |
 | the page: a wl_subsurface of that window  <-----+--------+ (shm or dmabuf |
 | WebKit: one WebKitWebContext, one network       |  wl_  | buffers)       |
 | process, one WebKitWebView per tab              |  subsurface  +----------------+
 | wpebackend-fdo: a private Wayland display       |
 +-----|--------------------------------------------+
       | IPC
 +-----v-------------------------+   +--------------------------+
 | WPEWebProcess (per site or  |   | WPENetworkProcess        |
 | per tab), sandboxed by      |   | (not sandboxed by WebKit:|
 | bubblewrap, section 9       |   |  cache, cookies, downloads)|
 +-----------------------------+   +--------------------------+
```

- The **UI process** is `freebws`. It is one GLib main loop. libflui's loop runs inside it as a GSource (`fl_app_prepare` and
  `fl_app_dispatch`, chapter 22, section 13), so WebKit's signals and the toolkit's input share one thread.
- Every tab is a `WebKitWebView` with its own view backend. All tabs share one `WebKitWebContext` and the one network process.
- The **web processes** render the pages. They run in WebKit's bubblewrap sandbox when it is available (section 9).
- The **network process** is not sandboxed by WebKit. It writes the cache, the cookies and the downloads (`~/downloads`).
- The page is drawn by WebKit into a private Wayland display that `wpebackend-fdo` runs inside `freebws`. The UI process takes
  each finished frame and hands it to flcomp (section 3).

## 3. How a page reaches the screen (shm and dmabuf)

Every finished frame of the active tab is passed on as a buffer of FreeBWS's own (`freebws.c` header):

- **shm**: the pixels are copied into a `wl_shm` buffer of the window's subsurface. This is the path with no GPU (llvmpipe).
- **dmabuf**: the GPU buffer's file descriptors go to flcomp through `zwp_linux_dmabuf_v1`, with no copy in FreeBWS. flcomp reads
  them (chapter 23, section 15).

The choice: FreeBWS picks dmabuf when flcomp announces tiled modifiers (a real GPU) and has written
`$XDG_RUNTIME_DIR/flcomp-dmabuf-planes`. Otherwise shm. Environment `FREEBWS_BUFFERS=shm` or `dmabuf` forces one, and the
config key `buffers=shm|dmabuf|auto` does the same. `FREEBWS_RENDER_NODE=/dev/dri/renderD129` picks the render node.

The dmabuf mode gives `wpebackend-fdo` an `EGLDisplay` of its own (GBM on `/dev/dri/renderD*`). Only then does the web process find
`zwp_linux_dmabuf_v1` in its private display and draw with the GPU. The README records why: an earlier version used the
"dmabuf pool" mode, in which the web process's first buffer request failed with "no memory", and a real GPU showed a blank window.

Safety net: if the GPU path delivers no frame in 12 seconds, or flcomp ends the connection with a protocol error in the first
minute, FreeBWS writes `buffers=shm` to the config and starts again on the CPU path.

Only the active tab's frames are shown. Other tabs' frames are dropped. In shm mode, the last frame of a tab is kept and shown
while its page repaints after a switch.

Tested (README): on a Radeon 610M in a headless flcomp on its render node, and in QEMU with virgl (`virtio-vga-gl`). "The bare-metal
run of the whole desktop is the user's laptop" (the README's own words).

## 4. Tabs and discarding

- Up to 24 tabs (`MAX_TABS` in `freebws.c`). At 24, the oldest background tab that is not loading is closed (it stays in the
  reopen list, Ctrl-Shift-T). If there is none, a notice says so.
- `target=_blank` and `window.open` open a tab in front, related to the opener, so `window.opener` and `postMessage` work (OAuth
  and sign-in popups). Ctrl-click and middle-click open a link in the background.
- `window.close()` closes the popup and selects the page that opened it.
- **Discarding.** A background tab that was not looked at for 30 minutes (the default of `B.idle_s` in `freebws.c`; `FREEBWS_DISCARD_IDLE` in seconds, 0 turns it
  off) is discarded. So is the oldest background tab when less than a fifth of the memory is free (at most 384 MB; `FREEBWS_DISCARD_MB`).
  Discarding destroys the web view and keeps its back and forward list. Selecting the tab loads the page again; scroll position and
  form input are lost. The opener of a popup that is still open, and a tab that is loading or playing audio, are never discarded.
- WebKit sees background tabs as hidden, so it throttles them.
- The tab row (`tabstrip.c`): a spinner for a loading tab, a dimmer title for a discarded one, an `x` on the active or hovered tab.

## 5. The ad blocker (adblock, abp.c, abpconv)

On by default. It blocks network requests, hides page elements, and runs small scripts for some sites.

**The lists.** EasyList, EasyPrivacy and the uBlock filters. A snapshot is baked into the package in `/usr/share/freebws/lists`
(`lists.conf` names them: name, URL, licence and title; the licences are in `LICENSES` there). Newer copies are downloaded weekly
into `~/.local/share/freebws/adblock/lists`. `freebws --adblock-update` does the download by hand.

**The conversion (`abp.c`).** ABP filter syntax becomes:

- network rules: WebKit content rules (JSON), compiled by WebKit's content rule list store (in the cache directory `filters`), in
  chunks;
- cosmetic rules: CSS selectors kept in `generic.txt` (for all sites) and `site.tsv` (per site), under `adblock/built/`;
- scriptlets: small JavaScript calls for specific sites, including a YouTube ad remover.

`adblock/state` records which lists the compiled rules belong to (a signature), so a changed list triggers a rebuild.

**The first start.** The first start builds the rules in a child process: about 10 seconds, and about 400 MB for a moment (README).
Pages wait for it. The navigation waits behind a "gate" for up to 120 seconds on the first start (`GATE_TIMEOUT_S`). After that,
the browser loads the compiled chunks from memory-mapped files (a map, not a compile).

**On each navigation** (`adblock.c`): the browser builds the style sheet for the page's host (the generic selectors minus the
site's exceptions, plus the site's own rules) and the script for its scriptlets, and swaps them in.

**The shield button** shows the number of blocked items and a switch for the site you are on (`adblock_off` in the config lists the
sites where the blocker is off). The count script `count.js` watches only failed sub-resources (round 16 stopped it wrapping `fetch`).

**Built-in allows.** `adblock.c` never blocks the Cloudflare challenge (`challenges.cloudflare.com`, Turnstile), whatever the lists say
(`BUILTIN_ALLOW`).

Environment for testing: `FREEBWS_ADB_SKIP=net,css,js,yt` leaves a part out (to find what breaks a site); `FREEBWS_ADB_LOG=1` logs
the blocker; `FREEBWS_ADB_UPDATE=1` updates the lists.

## 6. DSSL: native styles (libdssl)

DSSL is a style language from EHS-Studio (MIT). It is an alternative to CSS. `src/libdssl` is a small C11 library (libc and libm only)
and a command line tool `dssl2css` that follows chapters 2 to 7 and 9 to 11 of the native specification in DSSL 0.16.3.

Pages with DSSL tags (`<link rel="stylesheet/dssl">`, `<link rel="stylesheet" type="text/dssl">`, `<style type="text/dssl">`,
`<script type="text/dssl">`) are styled natively when possible: libdssl lowers the sheet to CSS in the browser process. The page then
gets `<style data-dssl-native>` elements and a `dssl-ready` event. `<html data-dssl-mode>` is `native` or `engine`.

A sheet that needs the engine (for example texture, aura, `@signal`, `seen`, see `src/libdssl/dssl.h`), a page that scripts `DSSL.`
itself, or a style-src CSP that blocks inline styles, sends the whole page to the bundled JavaScript engine (`freebws-dssl://engine/...`,
never the network). stderr gets a `freebws: dssl: ...` line per page. `FREEBWS_DSSL=engine` always uses the engine.

Test pages: `dssl-native-test.html` (native), `dssl-test.html` (needs the engine), `tabs-test.html` (links for the tab features).

## 7. Pages, the internal `freebws://` pages and the bridge

The internal pages are files in `share_dir()` (`/usr/share/freebws`): `freebws://start`, `freebws://settings`, `freebws://bookmarks`,
`freebws://downloads`, and `freebws://res/<file>` for their resources. `start.html` is filled in with the chosen search engine. The
error page is `freebws://error/`. Other pages have no access to them: internal pages cannot be opened by `window.open` from web content.

The pages talk to the browser through a bridge (`pages.c`): a user script in the script world `freebws-ui` listens for DOM events and
posts a message to the handler `fbui`. The reply comes back as an event. The bridge only does something on `freebws://` pages and only in
the top frame. The pages' own scripts run in the main world and cannot reach the handler.

## 8. Downloads, bookmarks, dark mode, search, errors

- **Downloads.** Saved to `~/downloads` (`downloads_dir` in the config). A toolbar button with a progress ring, and a panel with cancel,
  retry, open and "show in folder". `freebws://downloads` (Ctrl-J). The WPE API cannot pause or resume a download. A response becomes a
  download when its type is not supported or it carries `Content-Disposition: attachment` (the response policy, `on_policy`).
- **Bookmarks.** Ctrl-D adds or removes the current page. A bar under the address bar (Ctrl-Shift-B; `bookmarks_bar` in the config),
  folders as dropdowns, and `freebws://bookmarks` (Ctrl-Shift-O) to edit, move, delete and import an HTML export from Firefox or
  Chromium. Stored in `~/.local/share/freebws/bookmarks.json`.
- **Dark mode.** `dark=1` in the config (or a per-site choice with `dark_on` and `dark_off` lists). Pages that are light are turned dark
  with a filter. Pages that are dark already are left alone. Web pages only.
- **Search.** Brave Search by default. DuckDuckGo, Google, Startpage, Bing and Ecosia are also offered, chosen at `freebws://settings`
  (`search` in the config). The address bar and the start page follow the choice.
- **Address bar.** An address without a scheme gets `https://` when it looks like a host (a dot and no spaces); IP addresses and
  `localhost` get `http://`. Anything else is a search. Ctrl-L, F6 and Alt-D select the bar. Escape puts the page's address back.
- **Errors.** Offline, no such server, connection refused, TLS (continue anyway), blocked by the ad blocker (open anyway), and a crashed
  tab. The error pages use the "oh no." style of the crash screen. Enter retries.

## 9. The web process sandbox

WPE WebKit is built with `ENABLE_BUBBLEWRAP_SANDBOX` (wpewebkit release 2 and later, `docs/SECURITY.md`). Every web process runs in
bubblewrap: its own user, mount, pid and uts namespaces (not ipc: WebKit shares it for WPE), a seccomp filter, a private `/tmp` and
runtime directory. `/` is not visible. Only `/usr`, `/etc`, `/lib*`, `/dev/dri` and `/dev/udmabuf` (for the GPU), the fonts, the
GStreamer data, and the paths added with `webkit_web_context_add_path_to_sandbox` (here `/usr/share/freebws` and `$FREEBWS_SANDBOX_RO`)
are visible. The session bus is reached only through `xdg-dbus-proxy`. The Wayland link is a socket that is handed over the IPC
connection, so dmabuf and shm buffers need nothing but the render node.

WebKit aborts the browser when bwrap cannot run, so FreeBWS runs a probe first (`sandbox_probe`, `freebws.c`):

| `sandbox=` (or `FREEBWS_SANDBOX`) | When bwrap is usable | When it is not usable |
|---|---|---|
| `auto` (default) | sandbox on | the web processes run **without** a sandbox, with a warning on stderr and a note on the Settings page |
| `on` | sandbox on | FreeBWS exits, with the reason |
| `off` | sandbox off (warned about) | sandbox off (warned about) |

With `off`, FreeBWS sets `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1`. The Settings page and the stderr output say which mode is in use.
`freebws --sandbox-check` prints the verdict (exit 0 when usable).

The probe binds what WebKit binds: `/usr/lib`, `/usr/bin`, `/lib`, `/lib64`, `/bin` one by one, never `/usr` as a whole, plus the
directory of the program's real executable. Until base-files release 3, `/usr` on FreeLFS was the homes directory, so
`/proc/self/exe` was under `/sysflfs`; the probe did not bind that, and the sandbox failed (release 10). Now `/usr` is the real
system directory and `/sysflfs` a link to it (chapter 12, the folder layout).

**Open issue from the bug scan (not verified fixed):** the probe runs `freebws --sandbox-child`, which exits at once. It does not start
a web process, so an "on" verdict does not prove that pages get fonts, the time zone or shared memory inside the sandbox
(`docs/bugscan-2026-11-13/B-browser-portal.md`, H4). Test with `FREEBWS_SANDBOX=off` and the same page to compare.

## 10. The portal and the screenshot path

`flportal` (`src/flportal`) is the backend of xdg-desktop-portal for Flatpak apps and the desktop: file chooser, "open with", the
allow or deny question, notifications, settings (colour scheme and accent) and screenshots. The screenshot method runs `snappel`
when it is installed (chapter 23, section 14). The use-after-free in its screenshot path (bug C1 of the 2026-11-13 scan) and the
impl interfaces that accepted calls from any session-bus client (H7) were fixed in round 16: the impl interfaces now answer only
xdg-desktop-portal. Check `docs/bugscan-2026-11-13/B-browser-portal.md` before changing the portal.

## 11. Memory

The design goal (`docs/M8-BROWSER.md`): under 500 MB with three normal tabs on a 1 GB machine. The measurements of the M8 round were
in QEMU at 1 GB and 2 GB (`build/m8-freebws.sh`, logs in `~/freelfs-work/logs/m8-freebws-<mem>.log`). The same three tabs measured
568 MB as the sum of the resident sizes, which the document calls over the goal. The M8 notes also describe discarding of
background tabs and, on machines under 3 GB, one web process for all tabs (`docs/M8-BROWSER.md`; not re-measured here).

Settings that affect memory (README): the cache model `DOCUMENT_VIEWER` (no page cache, so back reloads the page), a process that
keeps its web process across origins (`ProcessSwapOnCrossSiteNavigation` off), Skia on the CPU unless a GPU is announced, discarding of
background tabs, and the memory-pressure settings `FREEBWS_MP=limit_mb,conservative,strict,kill,poll_s`. The first start of the ad blocker
uses about 400 MB for a moment (section 5).

## 12. Settings: `~/.config/freebws/config` and the environment

The config file is `~/.config/freebws/config` (`XDG_CONFIG_HOME` is honoured). It is `key=value`. The keys that the code reads:

| Key | Values (default) | Meaning |
|---|---|---|
| `search` | an engine name (Brave Search is the default) | the search engine for the address bar and the start page |
| `downloads_dir` | path (`~/downloads`) | where downloads go |
| `adblock` | 0/1 (1) | the ad blocker on or off |
| `adblock_off` | comma-separated hosts | sites where the blocker is off |
| `bookmarks_bar` | 0/1 (1) | the bookmarks bar |
| `dark` | 0/1 (0) | dark mode for all web pages |
| `dark_on`, `dark_off` | comma-separated hosts | per-site dark mode |
| `buffers` | `auto` (default), `shm`, `dmabuf` | frame transfer (section 3) |
| `sandbox` | `auto` (default), `on`, `off` | the web process sandbox (section 9) |
| `cookies` | `always` (default), `no-third-party` | the WebKit cookie accept policy |
| `useragent` | a string | replaces the default user agent (section 14) |

Environment variables (for testing and for the person, as the code reads them):

- Sandbox and buffers: `FREEBWS_SANDBOX`, `FREEBWS_SANDBOX_RO`, `FREEBWS_BUFFERS`, `FREEBWS_RENDER_NODE`.
- Cookies: `FREEBWS_COOKIES`.
- Memory: `FREEBWS_MP`, `FREEBWS_CACHE` (`viewer`, `document` or `browser`), `FREEBWS_PAGECACHE` (a page cache override, see `freebws.c`), `FREEBWS_SHARE=1`
  (all tabs in one web process), `FREEBWS_DISCARD_IDLE`, `FREEBWS_DISCARD_MB`.
- WebKit features: `FREEBWS_FEATURES=Name=0|1,...` replaces the default feature list.
- Ad blocker: `FREEBWS_ADB_SKIP`, `FREEBWS_ADB_LOG`, `FREEBWS_ADB_UPDATE`.
- DSSL: `FREEBWS_DSSL=engine`.
- Logging: `FREEBWS_POLICY_LOG` (the response policy), `FREEBWS_EVAL=js` (runs a script in each page after 3 seconds and logs the
  result; for example `FREEBWS_EVAL=navigator.userAgent` logs the user agent of every page).
- Directories: `FREEBWS_HOME=DIR` puts the config, data and cache under DIR; `FREEBWS_DATADIR=DIR` replaces `/usr/share/freebws`.

The data directory is `~/.local/share/freebws` (bookmarks, the adblock lists and build); the cache is `~/.cache/freebws` (`XDG_CACHE_HOME`).

## 13. Command line

```
freebws [URL]                  start (default: freebws://start)
freebws --adblock-update       download the newer filter lists now
freebws --adblock-build        build the rules (a child process, normally started by the browser)
freebws --sandbox-check        print whether the web process sandbox works here (exit 0: yes)
freebws --sandbox-child        what the sandbox probe runs inside bwrap (not for people)
```

Keys in the window (from the README): Ctrl-L, F6 or Alt-D for the address bar; F5 or Ctrl-R reload; Alt-Left and Alt-Right back and
forward; Ctrl-T new tab; Ctrl-W or Ctrl-F4 close; Ctrl-Shift-T reopen; Ctrl-Tab and Ctrl-PageDown next tab; Alt-1 to Alt-8 that tab;
Alt-9 the last tab; Ctrl-D bookmark; Ctrl-Shift-B the bookmarks bar; Ctrl-Shift-O the bookmarks page; Ctrl-J downloads.

## 14. Known bugs and their status

The bug scan of 2026-11-13 (`docs/bugscan-2026-11-13/B-browser-portal.md`) listed the browser's problems, and the round notes say which
were fixed. The statuses below are read from the code and the notes. "Not verified" means no one has run the case on a real site or a
real laptop since.

| Item | Scan finding | Status |
|---|---|---|
| H1 | `window.open` after an `await` was dropped; the opener waited forever | fixed in round 16: `javascript-can-open-windows-automatically` is on, so the popup is not dropped (README "Popups") |
| H2 | the OAuth opener was backgrounded and discarded during a sign-in | round 16: "OAuth popups keep their opener", no discard during logins. Still open in theory for a sign-in that stays idle 10 minutes on a 1 GB machine (discard): not verified |
| H3 | no user agent; Google and Cloudflare judge embedded browsers by it | round 16 set a Chrome-style string; **round 17 replaced it** with the Safari form, because a Chrome string on a WebKit engine is what Cloudflare Turnstile catches (README). Not verified whether Google accepts this form. |
| H4 | the sandbox "on" verdict did not start a web process | open (see section 9); not verified fixed |
| H6 | the ad blocker's first-start gate could load a stale URL over a newer navigation | "ad-block gate race fixed" in round 16 (commit notes) |
| H7 | flportal's impl interfaces accepted any caller | fixed in round 16 (the interfaces answer only the portal front end) |
| C1 | use-after-free in flportal's screenshot path | fixed in round 16 |
| Turnstile | Cloudflare's challenge page blocks or fails in the embedded browser | the challenge host is never blocked by the ad blocker (`BUILTIN_ALLOW`), and the user agent changed in round 17. Whether the check now passes: **not verified** |
| Google login | sign-in may refuse an embedded browser (user agent), or lose its opener (COOP); cookies (`cookies=always` default); popups; the discard | **open**. The popup and opener parts were changed in round 16. Whether Google accepts the sign-in: **not verified**. The planned `docs/CONTRIBUTING-freebws.md` will list it. |

Two other items the scan marked uncertain: whether WebKit's default cookie and tracking settings keep third-party frames (Google's and
Cloudflare's), and whether a subframe response with an unusual type becomes a download and stops the load. Neither was checked against
real sites.

The `cookies=no-third-party` choice (WebKit's policy) was not made the default, because sign-in frames and popups that set cookies for
the page that opened them broke with it in an earlier test. It has not been checked against a real sign-in (README).

## 15. Known gaps (not there yet)

From the README and `docs/M8-BROWSER.md`:

- Find in page, history, zoom, a private window.
- Tabs: no drag to reorder, no pinned tabs, no tab list for many tabs (they shrink, to 44 px), no audio indicator.
- No site isolation and no back-forward cache on machines under 3 GB (a memory choice).
- No Widevine (Netflix, Disney+, Prime Video, Spotify web), no WebUSB, Web Serial or Web Bluetooth, no extensions. WebRTC is to be
  proven. Firefox covers these.
- The dmabuf path with several tabs is not tested without a GPU.

## 16. How to help

- **Reproduce a bug** on a real site. Write the address, what you did, what you expected, and what happened. Run `FREEBWS_EVAL=navigator.userAgent`
  or the sandbox test (`FREEBWS_SANDBOX=off`) to tell the cause apart. Attach `flreport` (chapter 26).
- **Test the test pages**: `/usr/share/freebws/tabs-test.html`, `dssl-test.html`, `adblock-test.html`. The QEMU suites `build/m8-freebws.sh`
  and `build/m12-sandbox.sh` run them (chapter 25, section 7).
- **Look at the list of "not there yet"** (section 15). Each item is a self-contained piece of work in `freebws.c` or `pages.c`.
- **Filter lists**: a site that breaks because of the blocker is found with `FREEBWS_ADB_SKIP=net,css,js,yt` and reported with the list
  name and the rule, if it can be seen.
- **Sign-in tests** are the most useful: they show whether the user agent, the opener and the cookies are right for each provider.
- The project's `help wanted` and `good first issue` labels for FreeBWS are planned (chapter 26, section 11).
