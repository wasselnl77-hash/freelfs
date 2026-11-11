# Browser draft (FreeBWS, working name)

Decision: the default browser is our own, on WPE WebKit, drawn with libflui.
Firefox goes in the repo as the fallback for DRM, video calls and extensions.

## Parts
- Engine: WPE WebKit (wpewebkit + libwpe + wpebackend-fdo, or WPE's newer
  platform API, whichever the current release uses). No GTK, no Qt.
- Media: GStreamer with base, good and libav plugins, for video and audio
  through PipeWire.
- UI, `src/freebws`: libflui window with the page as a subsurface. The legacy
  WPE API is used: WebKit's web process draws into a Wayland display that
  wpebackend-fdo runs inside freebws, and freebws hands each frame to flcomp,
  as shm (copy, llvmpipe and no GPU) or as dmabuf (passed on, a GPU). flcomp
  therefore has zwp_linux_dmabuf_v1 (see its README). The newer WPEPlatform
  API is not built (ENABLE_WPE_PLATFORM=OFF): it makes the view a toplevel of
  its own, not a subsurface.

## Version 1
Tabs (one row, flat, accent strip on the active tab; done, see "Tabs"), back, forward, reload,
address bar that also searches, find in page, downloads to ~/downloads, zoom,
bookmarks, history, private window, built-in ad blocker (WebKit content
filters, EasyList converted at build time), Safari user agent by default with
per-site overrides. Search default: DuckDuckGo.

Later: saved passwords, sync, reader mode, Web Push.

## DSSL
Built-in support for DSSL (huggingface.co/EHS-Studio/dssl, MIT), a style
language alongside CSS. Its engine is JavaScript, so no WebKit patches. Two
paths, decided per page: native first (libdssl lowers DSSL to CSS inside
FreeBWS), the bundled JS engine only for what CSS cannot express.
Engine (v0.16.3, pinned to commit 0d7ad1f, sha256 in recipes/freebws):
- The engine's browser modules are installed in /usr/share/freebws/dssl (with
  its LICENSE) and served to pages through the URI scheme `freebws-dssl://engine/`
  (secure and CORS-enabled; never the network).
- Test page: /usr/share/freebws/dssl-test.html (no CSS at all; tokens, layout
  words, hover, texture, fluid type), screenshot docs/m7/freebws-dssl-<mem>.png. Its
  starter token file has `@signal` rules and the page uses `texture: wood`, so it
  always takes the engine path.
Limits of the engine path: pages with a strict `script-src` CSP do not get the
engine (the import is subject to it); `.dssl` files next to a `file://` page cannot
be fetched (WebKit blocks it, as Chrome does), so use inline blocks there; the engine
styles after it loads, so a page can flash unstyled first.

### Native DSSL (src/libdssl), status
DSSL 0.16.3 added a written spec for native implementations (native/spec).
Step 1: src/libdssl, a dependency-free C11 library and `dssl2css`, recipe
recipes/libdssl. It parses DSSL and resolves it as chapters 2 to 7 say, then lowers the part
that has a CSS equivalent (tokens, layers, selectors and nesting, states,
`@media`/`@container`, semantic properties, gradients, motions as `@keyframes`,
fonts, raw) to CSS with `:where()` and `@layer tokens, base, components,
utilities, pin`. Measured against the JS engine on 703 inputs (the project's own
stylesheets, guide examples and test fixtures plus our own cases): syntax tree 700/700,
resolved document 636/636 (64 skipped: aura, slider), CSS byte-equal to the engine's
`dssl build` 552/552, ASan/UBSan and valgrind clean, every input cut at random points
without a crash; 4.8 ms to parse and resolve 100 kB. The 0.16.3 checkout has no
committed corpus (spec chapter 10 describes one), so ours is derived by running the
engine's own files; src/libdssl/README says exactly what that does and does not prove.
- What stays on the JS engine, reported by `dssl_needs_engine()` with line and
  column: `@signal`, `seen`, `animate` with `jitter`, `stagger`, `texture`, `aura`,
  `bend`/`fold`/`stretch`/`bend-around`, `tilt`, `slider`, `overlap`, `gradient-flow`.
  Decided per page (all sheets of a page, in load order, go through one `dssl_ctx`).

Step 2, done: libdssl is in the desktop group (release 2) and FreeBWS depends on it.
- The user script at the end of every top-level document (one per tab, in the
  tab's own WebKitUserContentManager) looks for `<link rel="stylesheet/dssl">`,
  `<link rel="stylesheet" type="text/dssl">`, `<style type="text/dssl">` and
  `<script type="text/dssl">`. Other pages pay one querySelector; a page that ships its
  own engine (`script[src*=dssl]`) or has an inline script that uses `DSSL.` is left to
  the engine path.
- It fetches the linked sheets in the page (`fetch()`: cross-origin and `file://` behave
  as they did) and posts every source, in the engine's load order (links first, then
  inline blocks), to the script message handler `dssl` (registered with reply, so
  `postMessage` returns a promise). Native parses them through one `dssl_ctx` and answers
  `["native 0.16.3", css1, css2, ...]` or `["engine"]`.
- Native: the script inserts one `<style data-dssl-native>` after each original tag, sets
  `<html data-dssl-mode="native">` and `window.__freebwsDssl = "native"`, fires `dssl-ready`
  (`detail.version`, plus `detail.native = true`) and adds the `dssl-ready` class, as the engine
  does. The engine is not imported: no request for `freebws-dssl://engine/boot.js`, no
  `window.DSSL`.
- Engine: any sheet with a hard feature (all sheets are still parsed so the log names every
  one), an out-of-memory or oversize input, or a `style-src` CSP violation while inserting
  the styles (the script then removes them and boots the engine) sends the whole page to the
  engine exactly as before (`data-dssl-mode="engine"`, links of type `text/dssl` rewritten to
  `stylesheet/dssl`). The decision is per page, not per sheet: the engine writes inline styles,
  which beat any stylesheet, while lowered CSS lives in cascade layers; mixing the two would
  flip who wins between DSSL rules of different sheets.
- stderr (and so `/tmp/freebws.err` in the test) gets one line per page:
  `freebws: dssl: native, 2 sheet(s), 2140 bytes of DSSL -> 5214 bytes of CSS`, or
  `freebws: dssl: <sheet> needs the JS engine (signal at 100:1)` (one such line per sheet, naming
  every hard feature) followed by `page goes to the JS engine`, and `freebws: dssl: JS engine loaded`
  when the engine's entry module is served. Parse errors of a sheet are logged as
  `file:line:col: message`. FREEBWS_DSSL=engine forces the engine path (for comparisons).
- Memory, native against engine (one tab, fresh browser each; build/m8-freebws.sh part 5; rss / pss / used):

| step | 1 GB rss / pss / used | 2 GB rss / pss / used |
|---|--:|--:|
| one tab, start page | 426 / 265 / 208 | 427 / 265 / 224 |
| dssl-native-test.html, native | 439 / 272 / 209 | 440 / 272 / 223 |
| dssl-test.html, engine | 461 / 292 / 217 | 460 / 292 / 233 |

  The native page costs 13 MB rss (7 pss, 1 used) over the bare start page; the engine page 35 (27 pss, 9 used).
  Not the same pages: the engine page also draws the wood texture (an image the engine generates) and loads the
  starter token file, so read it as the size of the difference between the two paths for a page like this one, not as
  the price of the engine alone. The engine is about 530 kB of JavaScript that WebKit loads, compiles and keeps; the
  native path keeps nothing of DSSL in the web process (libdssl runs in the browser process and frees its arena when the page's sheets are done; its README measures a
  peak of about 4 MB while parsing a 66 kB sheet).
- Test pages: /usr/share/freebws/dssl-native-test.html (tokens, layout words, states, a
  gradient, `animate`, `@media`, fluid type; its pill and a line of text say which path styled
  it) must be styled natively; dssl-test.html (texture, `@signal`) must still work through the
  engine. The QEMU test (build/m8-freebws.sh, part 5) loads each in a fresh browser.
- Different from the engine, to know: lowered DSSL sits in cascade layers, so a page's own
  unlayered CSS beats it, where the engine's inline styles beat the page's CSS (`yield:` and
  DSSL-versus-CSS conflicts change meaning); `raw { }` is emitted unlayered and beats every DSSL
  rule except `pin`; DSSL tags added to the DOM after the load are not seen (the engine's
  automatic boot reads them once, too); a page script that reads computed styles before the
  round trip to native has finished sees unstyled values (the engine has the same window
  while its modules load). SPEC-NOTES.md in src/libdssl has 28 notes (spec versus engine, gaps,
  engine bugs) to send to the DSSL developers. No bug of libdssl showed up in the integration; src/libdssl is
  unchanged. One thing the lowering exposed in the old test page: `box-shadow: lift` (a bare
  token name) is an error for libdssl ("unknown value"), where the engine passes it through to
  CSS, which drops it; `shadow.lift` is the form that works (the native test page uses it).

## Tabs
Done (src/freebws/freebws.c, the tab row in src/freebws/tabstrip.c, drawn with libflui in the toolbar's
flat style: window background `#1d2125`, active tab and toolbar `#272c33` with a 2 px accent (`#3f9b63`)
strip on top, hover `#343a43`, lines `#3a4048`, IBM Plex through libflui; screenshot of the row's states,
rendered with FLUI_PPM from the real code: docs/m7/freebws-tabstrip.png).
- One tab = one WebKitWebView with its own wpebackend-fdo view backend; all share one WebKitWebContext
  and the single network process. Only the active tab's frames reach flcomp (shm: the last frame of a tab
  is kept to show while its page repaints after a switch; dmabuf: dropped, not tested without a GPU).
  Background tabs lose the `visible` activity state, so WebKit throttles them.
- Ctrl-T new tab (start page, address bar ready), Ctrl-W close (the last one closes the window),
  Ctrl-Shift-T reopen the last closed (8 are remembered), Ctrl-Tab / Ctrl-Shift-Tab, Ctrl-PageDown / Ctrl-PageUp,
  Alt-1..9 (Ctrl-1..9) select. Click selects, the x on the active or hovered tab and a middle click close, +
  opens a new tab, the title is the page title (the address without scheme until there is one, "New tab" for the
  start page), a spinner shows loading, a dim title a discarded tab. New tabs open right of the active one.
- `target=_blank` and `window.open` open a tab in front, as a related view of the opener (`window.opener`
  works); Ctrl-click or Ctrl-Enter on a link and a middle click open it in the background (policy decision
  on the navigation action: button 2 or Ctrl, and the original navigation is ignored); `window.close()` closes the tab.
  Tested in QEMU with the keyboard (Tab, Enter, Ctrl-Enter on /usr/share/freebws/tabs-test.html); QEMU's
  tablet did not reach flcomp (no pointer events, tried with `usb-tablet` and absolute moves), so clicks,
  hover and the middle button are untested in a running browser: the row's drawing is checked with FLUI_PPM
  and the widgets are libflui's own `fl_widget`.
- Discarding: every 10 s a timer looks at the background tabs. One that was not selected for 10 minutes
  (FREEBWS_DISCARD_IDLE, seconds), or the oldest one when MemAvailable is under a fifth of the memory (at most 384 MB;
  FREEBWS_DISCARD_MB), is discarded: the back/forward list is saved (`webkit_web_view_get_session_state`), the web view
  and its backend are destroyed, the tab keeps its address and title. Selecting it makes a new view, restores
  the list and goes to the current item (the page loads again; scroll position and form input are lost). Tabs
  that load or play audio are not discarded. A web process that dies reloads the visible tab at once and the others when selected.
- Machines under 3 GB put all tabs in one web process (the views are related to each other, and WebKit
  runs related views in one process; `FREEBWS_SHARE=0` gives each tab its own). A tab without a process of its own
  saves that process's base, 65 MB rss and about 20 MB pss. The price: one tab's hang or crash affects all
  (they reload), related views share window names (`window.open('', name)` can find another tab's window), and a
  discarded tab frees only its own page, not a process.
  In QEMU, 3 tabs in one process: 568 MB rss at 1 GB; without sharing, the same three tabs were 885 MB rss
  and 455 MB pss in a development run (and after two of them were discarded 456 / 298: a discarded tab that had
  its own process frees the whole process).

Screenshots of the run (docs/m7, `<mem>` is 1G or 2G): `freebws-start-`, `-dssl-`, `-test-`, `-https-`,
`-bare-`, `-escape-`, `-back-` (one tab through the origins), `-tabs3-`, `-tabs-prev-`, `-tabs4-`, `-tabs-closed-` (three and four
tabs, Ctrl-Shift-Tab, Ctrl-W), `-link-blank-`, `-link-bg-`, `-link-open-`, `-link-closed-`, `-link-reopen-` (target=_blank,
Ctrl-Enter, window.open, Ctrl-W, Ctrl-Shift-T), `-discard-`, `-discard-back-` (dim titles: discarded tabs; selecting one),
`-native-` (the natively styled page); `freebws-tabstrip.png` is the row's states.

## Memory
Target: under 500 MB with three normal tabs. Tabs unused for 10 minutes get
discarded, and on low memory the oldest background tab goes first. Measured in QEMU at 1 GB and
2 GB with `build/m8-freebws.sh` (logs `~/freelfs-work/logs/m8-freebws-<mem>.log`, screenshots
`docs/m7/freebws-*-<mem>.png`). Every number is MB (kB / 1000) summed over freebws, flcomp,
the network process and every web process of the browser: **rss** counts a shared library once per
process (as the earlier measurements did, so it overstates), **pss** splits shared pages between
the processes that map them (what the browser really holds), **used** is the `free -m` used column of
the whole system, desktop included (page cache not in it). The table cells are rss / pss / used.

### Across origins: one tab, five origins (goal: flat)
Before, with WebKit's defaults on the same sequence (start page, dssl-test.html, test.html, example.com, example.org, text
typed in the bar): 1 GB 444 / 548 / 591 / 936 / 1266 MB rss, 2 GB 487 / 585 / 624 / 978 / 1312 MB, five live web
processes at the end: old ones never exit (every origin got its own process, the old page stayed in the page cache, and
WebKit kept a process cache and prewarmed spares of 65 MB each). After:

| step | 1 GB rss / pss / used | 2 GB rss / pss / used |
|---|--:|--:|
| start page | 426 / 265 / 208 | 427 / 265 / 224 |
| dssl-test.html (engine) | 467 / 298 / 220 | 467 / 298 / 236 |
| test.html | 521 / 351 / 263 | 528 / 357 / 287 |
| example.com | 532 / 353 / 260 | 538 / 359 / 283 |
| example.org | 532 / 353 / 262 | 539 / 360 / 285 |
| idle 10 s, text typed in the bar | 532 / 353 / 261 | 539 / 359 / 284 |
| Alt-Left (reload) | 533 / 355 / 262 | 539 / 360 / 284 |

Flat from the second origin on: 1266 MB became 532 MB rss (353 pss) at 1 GB, 1312 became 539 at 2 GB, with one web process
the whole time; Alt-Left goes back (reloading the page). Each change on its own, measured on a development
harness (1 GB, one tab, seven origins in a row: start page, dssl-test.html, example.com, example.org, www.iana.org,
example.net, then 60 s idle and Alt-Left; rss / pss; the start-page row differs a little from the table above because
that harness had another flcomp):

| change | start | dssl-test.html | example.com | after 7 origins | back |
|---|--:|--:|--:|--:|--:|
| WebKit defaults | 444 / 300 | 546 / 401 | 917 / 566 | 1358 / 687 | 1408 / 737 |
| cache model DOCUMENT_VIEWER | 443 / 300 | 548 / 403 | 552 / 341 | 751 / 376 | 891 / 458 |
| viewer + `enable_page_cache` off | 445 / 300 | 549 / 404 | 552 / 344 | 751 / 375 | 888 / 455 |
| viewer + memory pressure settings (network session call) | 444 / 300 | 554 / 409 | 553 / 345 | 753 / 377 | 891 / 457 |
| viewer + CPU rendering + memory pressure settings on the web context (200 MB, 0.4/0.6, 5 s) | 398 / 254 | 431 / 286 | 512 / 305 | 713 / 336 | 815 / 383 |
| viewer + `UsesSingleWebProcess` (set after the view exists) | 444 / 299 | 553 / 407 | 552 / 343 | 752 / 376 | 889 / 456 |
| viewer + `ProcessSwapOnCrossSiteNavigation` off (after the view exists) | 444 / 299 | 543 / 397 | 602 / 445 | 491 / 336 | 563 / 407 |
| viewer + Skia CPU rendering | 398 / 254 | 438 / 292 | 515 / 306 | 714 / 338 | 819 / 386 |
| viewer + process swap off set at construction + CPU rendering | 399 / 254 | 438 / 293 | 513 / 305 | 713 / 336 | 820 / 388 |
| viewer + process swap off (after the view exists) + CPU rendering = what ships | 397 / 254 | 437 / 292 | 505 / 351 | 451 / 297 | 490 / 336 |
| shipped + `WEBKIT_SKIA_CPU_PAINTING_THREADS=1` | 398 / 254 | 438 / 293 | 506 / 352 | 451 / 298 | 491 / 335 |
| shipped + `MALLOC_ARENA_MAX=1` | 398 / 255 | 437 / 292 | 502 / 347 | 451 / 296 | 493 / 337 |
| shipped + `JSC_useJIT=false` | 395 / 252 | 424 / 279 | 494 / 340 | 452 / 297 | 484 / 329 |

What these show:
- **Cache model DOCUMENT_VIEWER** (`webkit_web_context_set_cache_model`): the first big step. No page cache, no
  back/forward cache, the processes of old pages go; Alt-Left reloads. The page cache setting on top of it
  changes nothing (the model already turns it off).
- It is not enough: WebKit still starts a web process per origin (process swap on cross-site navigation) and
  keeps spares (65 MB rss each, about 18 pss). **`ProcessSwapOnCrossSiteNavigation` off** keeps a tab in its process
  across origins: flat. It is a WebKit feature flag (`webkit_settings_set_feature_enabled`; the list is
  `webkit_settings_get_all_features()`, 457 entries in 2.52.5; there is no `webkit_web_context_set_process_model` or
  process limit in the WPE 2.52 headers). It only takes effect when set on the finished view: set at construction (the `settings`
  property) the process pool does not hear of it (row "set at construction": the growth is back).
- **Skia CPU rendering** (`WEBKIT_SKIA_ENABLE_CPU_RENDERING=1`, set by FreeBWS unless flcomp announces a GPU) saves 45 MB
  at the start and 110 MB on the DSSL page: without it every web process builds a GL context on llvmpipe (Mesa logs "ZINK: failed
  to choose pdev", falls back), and the shader compiler is a good part of the web process.
- **WebKitMemoryPressureSettings**: `webkit_network_session_set_memory_pressure_settings()` reaches only the network process and
  changed nothing we could measure. The web processes take theirs from the `memory-pressure-settings` property of the
  `WebKitWebContext` (construct-only: FreeBWS makes its context with `g_object_new`). One tab at rest does not change (nothing to
  give back); a web process that held several tabs gives memory back after background tabs were discarded: 22 MB rss
  (545 to 523) against 9 without (measured on the development harness: 565 to 556). Shipped under 3 GB: limit 300 MB, cleanup from 150 MB, strict from 210 MB, polled every 10 s,
  no kill threshold. CPU while idle stays far under 1 % of a CPU (m8cpu in the log).
- **`UsesSingleWebProcess`** (an "embedder" feature) did nothing when set on the finished view, like the process swap at
  construction; one process for all tabs is done with related views (see Tabs).
- No effect: `WEBKIT_SKIA_CPU_PAINTING_THREADS=1`, `MALLOC_ARENA_MAX=1`. `JSC_useJIT=false` saves 14 MB on the DSSL page and
  nothing at rest, and makes scripts slow: not used.
- The bare start page's web process is still 190 MB rss (143 pss); freebws takes 77 MB rss, the network process 62 and
  flcomp 69. That is the floor of this WebKit build here.
- Machines with 3 GB or more keep WebKit's defaults (back/forward cache, a process per site, per-tab processes, no
  memory pressure settings): the savings cost back/forward speed and isolation.

### Three tabs against the goal (< 500 MB)
Start page, example.com and dssl-test.html (the JS engine; the heavy kind of page), after a 30 s wait:

| step | 1 GB rss / pss / used | 2 GB rss / pss / used |
|---|--:|--:|
| 1 tab: start page | 419 / 259 / 204 | 420 / 259 / 219 |
| 2 tabs: + example.com | 504 / 331 / 257 | 504 / 330 / 275 |
| 3 tabs: + dssl-test.html (engine) | 568 / 385 / 296 | 568 / 384 / 315 |
| 3 tabs, 30 s later | 568 / 385 / 296 | 568 / 384 / 312 |
| 4 tabs: + dssl-native-test.html (native) | 607 / 421 / 329 | 607 / 420 / 345 |
| 4th closed (Ctrl-W) | 582 / 399 / 306 | 582 / 397 / 324 |

Honest reading: three tabs hold **385 MB pss** and the system uses **296 MB** (`used`, desktop included), both under
500 MB, at 1 GB and 2 GB; the rss sum the earlier measurements used is **568 MB** for the same three tabs, over the goal, because it
counts library pages again in flcomp, freebws, the network process and the web process, and flcomp (the compositor: 37 to 95 MB rss
between runs of the same software, 94 in these) is part of the sum. Without flcomp the browser alone (freebws, network process
and web process) is **473 MB rss** (323 pss) for the three tabs at both memory sizes, and 332 MB at the start page. The third tab as native DSSL page
instead of the engine's costs less (below); a fourth live tab (the native page) costs 39 MB rss.

### Discarding (idle 45 s here, 10 minutes by default)
| step | 1 GB rss / pss / used | 2 GB rss / pss / used |
|---|--:|--:|
| 3 tabs (start page, example.com, dssl-test.html) | 545 / 364 / 276 | 547 / 363 / 292 |
| 75 s later, two background tabs discarded | 523 / 345 / 256 | 525 / 344 / 272 |
| example.com selected again | 546 / 366 / 276 | 548 / 364 / 293 |

Discarding two background tabs gave back 22 MB rss (they shared the one web process); selecting a
discarded tab loads it again (+23 MB). The log says `freebws: discarding tab "..." (unused)`.

## Browser features (C2 to C6, A8)
Screenshots: docs/m7/freebws-newtab, -adblock, -shield, -menu, -downloads, -bookmarks, -bookmark-bar-folder, -settings, -dark,
-error-dns, -error-offline, -error-tls, -error-blocked, -error-crash (.png). Rendered in a rootfs bwrap rig on the host (real
WPE WebKit and flcomp, headless), not in QEMU.

### Ad blocker (C3)
- Lists (data files, not code): EasyList, EasyPrivacy (GPL-3.0-or-later or CC BY-SA 3.0, we use the GPL option), uBlock
  filters, Quick fixes, Unbreak, Privacy (GPL-3.0-or-later). Snapshot of 2026-11-10 in src/freebws/data/lists (xz, sha256 in
  SHA256SUMS, `make check-lists`; the upstream addresses are rolling, so they cannot be pinned as recipe sources); lists.conf has
  URLs and licenses, LICENSES the license note (data files next to the MIT program, nothing links them in; GPL text included).
- Converter src/freebws/abp.c (own code, also `freebws-abpconv`): network rules to content blocker JSON (block, exceptions with
  ignore-previous-rules, $important after the exceptions, $badfilter, type, third-party, domain=, regex rules WebKit accepts),
  split into chunks of 50,000 rules with every exception repeated in each chunk (ignore-previous-rules only works inside one
  list); host-only rules fold the frame case into one rule. Skipped (3k of 170k lines): $redirect, $csp, $removeparam,
  $replace, $denyallow, $to, entity-only domain= (skipping is safe, dropping the condition would make a rule global: this
  broke YouTube once), procedural cosmetic filters (:has-text, :xpath, ...).
  Result: 121,600 block + 4,300 exception + 240 important rules = 134,800 rules in 3 compiled lists (70 MB on disk, mapped by
  WebKit); cosmetic: 13,700 generic selectors, 23,600 site selectors (sorted table, binary search in a memory map),
  38,000 site/scriptlet pairs.
- Build: a child process (`freebws --adblock-build`, oom_score_adj 1000) converts and compiles: 11 s and 397 MB peak at first start
  on a fast host, so the browser itself never holds the compiler's memory; navigations wait for it (notice page). Later
  starts load the compiled lists (about 1.2 s after start, in the background). A weekly update (`--adblock-update`, libsoup,
  ETag, sanity checks, only with 650 MB free) downloads newer lists, rebuilds and swaps the filters in live; tested.
- Cosmetic: on each main-frame navigation the page's host gets one user style sheet (generic selectors minus the site's
  exceptions, plus its own) and one user script (its scriptlets), swapped in before the load (`load_uri` and
  load-changed STARTED/REDIRECTED; decide-policy cannot tell frames apart in WPE 2.52). Scriptlets: data/scriptlets.js (own
  implementation of set, aopr, aopw, acs, aost, nowoif, nostif/nosiif, nano-stb/sib, aeld, rmnt, rpnt, ra, rc, json-prune
  (+ fetch, xhr), trusted-replace-xhr/fetch-response, no-fetch/xhr-if; others ignored). YouTube: data/yt-ads.js (every frame):
  ad fields removed from the player data and the /youtubei/ responses, ad skipping, hidden slots, the "ad blockers" box closed.
  Checked on a video with a pre-roll ad: blocker off, a 20 s ad plays first; on, the 213 s video starts, no ad slots.
- Per-site off: shield popup (config adblock_off); the lists' own @@$document/$elemhide/$generichide switches are honoured.
- Counter: WebKit does not report blocked requests, so data/count.js counts third-party sub-resources that failed without a
  resource timing entry (an estimate). Blocked main pages show the error page with "open anyway".
- Cost (this host, small-memory profile of tune_webkit, browser processes without flcomp, rss / pss in MB): blocker off 375/262
  (start page), 467/342 (+ example.com), 624/494 (+ Wikipedia); on 384/270, 489/358, 668/526. About +10 MB for the filters in
  the browser and web process, +10 to 20 MB per page for the style sheet (13,700 generic rules); a page with
  @generichide (Wikipedia: none). On pages with ads the blocker saves more than it costs (CNN: 1067 to 997 MB rss).
  The page sheet is the part to cut if 1 GB machines need it. Startup: the window and page do not wait for the filters
  except the first navigation (about 1 s).
### Search (C6), downloads (C4), bookmarks (C2), dark mode (C5), error pages (A8), settings
Done as in the README ("Features"). Internal pages are served under freebws:// (start, settings, bookmarks, downloads,
res/); they talk to the browser through a bridge in a separate script world (pages.c). Popups (menu, shield, downloads,
bookmark folders) are libflui raw windows placed as subsurfaces above the page (popup.c). Error pages are loaded as
alternate HTML for the failing address (pages.c, data/error.html). Dark mode is a document-start script (DARK_JS) that
inverts light pages and removes itself when the page is dark; no WPE 2.52 setting for prefers-color-scheme was found, so a page's
own dark style is not asked for, only recognised.
Not possible or not done: pause/resume of downloads (no API), the blocked count is an estimate, file chooser for the
bookmark import is a typed path, ads inside iframes of other sites only get the network and YouTube rules.

## QEMU verification of the third pass (ad blocker, error pages, start page)
`build/m9-features.sh` (2 GB and 1 GB, throwaway image copy) and `build/m8-freebws.sh`, 2026-11-10, release 6 of the package:
- The start page is `freebws://start` (the log says `loaded freebws://start`; `build/m8-freebws.tty` counts it and counts
  zero loads of `file:///usr/share/freebws/start.html`). A web page (file:// test page)
  cannot open `freebws://start` with `window.open` (the tab stays blank: internal pages are not reachable from page content;
  intended), so `tabs-test.html` opens `adblock-test.html` instead of the old `start.html` link.
- **Ad blocker first start at 1 GB** (962 MB visible, nothing cached): the child `--adblock-build` compiles the three
  chunks (134,867 rules) within 25 s of the start (still running at 5 s); the sampler (`m9sample`, every 0.5 s) saw MemAvailable never below 434 MB, the
  child's rss peaked at 352 MB, `used` was 511 MB 5 s after the start (browser, child, WebKit processes, desktop) and 215 MB
  once the child was gone. No OOM kill, no swap thrash, the page and the first navigation were not held noticeably. At 2 GB
  the same run peaks at 498 MB `used`. Compiled lists: 70 MB in `~/.cache/freebws/filters`. The weekly update runs right
  after the first start when the network is up (6 lists were newer than the package snapshot) and rebuilds once more; it waits for
  650 MB of free memory when filters are already loaded. So precompiling the WebKit filter store at package build time was
  not necessary and is not done (the store belongs to one WebKit build and one user's cache directory).
- Error page: `http://nonexistent.invalid` shows the "oh no." DNS page ("Can't find that server", code FB-DNS, Enter retries,
  Esc goes to the start page) in both memory sizes. `freebws://settings` shows the blocker's status and the lists.
- One text fix from this run: while an update rebuild runs with the filters loaded, Settings said "Preparing the filters
  (one time...)"; it now says the rules are active and newer lists are being prepared.
- Memory (m8mem, rss / pss in MB, desktop included): start page 410 / 262; three tabs 570 / 392; same at 1 GB and 2 GB.

## Known gaps
Tabs: no drag to reorder, no pinned tabs, no tab list for many tabs (they shrink to 44 px, at most 24), no tab
audio indicator; the mouse paths of the tab row are checked only by rendering (see "Tabs"). No site isolation or back/forward
cache on machines under 3 GB (a memory choice). The dmabuf path with several tabs is not tested without a GPU.
No Widevine (Netflix, Disney+, Prime, Spotify web), no WebUSB, Web Serial or
Web Bluetooth, no extensions, WebRTC to be proven. Firefox covers these.

## Order
1. GStreamer, WPE WebKit packages; build in gentle mode (hours). Done except the WebKit compile (still running, see PLAN.md M8).
2. flcomp: linux-dmabuf and EGL client buffers. Also needed for any GL app. Done: dmabuf read into the CPU copy, GPU detiling through EGL/GLES2 (untested on a GPU); GPU compositing of the screen, wl_drm and dmabuf feedback are not done.
3. FreeBWS minimal: one window, one page, address bar. Built and run in QEMU (2 GB and 1 GB, build/m8-freebws.sh; screenshots docs/m7/freebws-*.png). Address bar like other browsers: click, Ctrl-L, F6 or Alt-D select all, Enter loads, Escape restores the address; host-like input gets https://, other input is a DuckDuckGo search; the window title is the page title. shared-mime-info is in the image (without it file:// pages came out as text/plain).
4. Tabs (done: see "Tabs"; memory flat across origins, see "Memory"; native DSSL), then downloads, bookmarks, ad blocker.
5. Firefox package (GTK3, Rust, nodejs) for the repo.

## Choosing a browser
FreeBWS ships in the image and always stays installed. A first-boot step offers
FreeBWS or Firefox; Firefox is downloaded from the repo when picked, queued and
retried if offline, never blocking setup, changeable in the Packages app.
Machines under 4 GB of RAM get FreeBWS recommended. Needs the online repo.
