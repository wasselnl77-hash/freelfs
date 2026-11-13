# Bug scan B: FreeBWS, libdssl, flportal, flstore (2026-11-13)

Read-only scan. Nothing built, run, edited or committed. Line numbers are from the tree at HEAD `c9604b6`.

## Coverage

- **Read in full:** `src/freebws/freebws.c` (3028 lines), `pages.c`, `downloads.c`, `config.c`, `config.h`, `popup.c/h`, `shm.c`, `tabstrip.c`, `abpconv.c`, `pages.h`, `adblock.h`.
- **Read in part:** `adblock.c` (lines 1-140, 640-1128), `bookmarks.c` (1-120, 340-430), `flportal/main.c` (60-240, 575-800), `flportal/filechooser.c` (the accept, path and open code), `flportal/flportal.h`, `libdssl/main.c`, `libdssl/util.c` (arena), `libdssl/resolve.c` (`dssl_parse`, public API), `libdssl/dssl.h` (1-80), `flstore/flstore.c` (195-230, 370-503), `flstore/index.c` (32-44, 450-470).
- **Not read:** `abp.c` (974 lines), `adblock.c` 140-640, `bookmarks.c` 120-340, `abp.h`, `flportal/appchooser.c`, `flportal/ui.c`, most of `filechooser.c`, most of `flportal/main.c`, `libdssl` apart from the above (about 8000 lines), `flstore/flstore.c` (rest), `index.c` (rest), `views.c`, `img.c`, `tests/`. **The brief asked for every file; this scan did not reach that.** The unread files should get a second pass.
- **Brief mismatch:** libdssl has no network, TLS or certificate code (`grep -i 'ssl|tls|socket|curl'` finds nothing but the file header). It is a DSSL parser and CSS lowering library, so the TLS check does not apply there. TLS in the browser lives in WebKit's network process and in freebws only as the error page at `freebws.c:1968-1997`.

## Summary for the login hang (top 10 lines)

1. Most likely: Google/OAuth popups are blocked or lose their opener. Nothing sets `javascript-can-open-windows-automatically` (H1), and `on_create` returns NULL silently (H1).
2. The opener tab is backgrounded when the popup opens and is discarded after 10 minutes idle or on low memory (H2). A slow 2FA step kills the login round trip.
3. No user agent is set (H3). Google treats embedded WebKit UAs as untrusted; this would explain a Google-specific failure.
4. The ad blocker's first-start gate can replay a stale URL over a newer navigation (H6).
5. The sandbox "on" verdict comes from a bwrap namespace probe, not from a web process (H4). Missing binds would break fonts or timezone, not login.
6. `count.js` replaces `window.fetch` on every page (M1). Bot checks (Cloudflare) can see that.
7. A real use-after-free is in flportal's Screenshot path (C1). Not the login bug, but it is the worst item here.
8. flportal's impl interfaces accept calls from any session-bus client with no sender check (H7).
9. The best A/B test on the laptop: `FREEBWS_SANDBOX=off`, `FREEBWS_ADB_SKIP=yt`, `FREEBWS_DISCARD_IDLE=0 FREEBWS_DISCARD_MB=0`, and `FREEBWS_EVAL=navigator.userAgent` (logs to stderr, freebws.c:1901-1911).
10. Two items are uncertain (see the last section): WebKit's default for popups, and COOP severing the opener.

---

## Critical

**C1. Use-after-free in the flportal Screenshot path.**
`src/flportal/main.c:598-607` (`shot_done`), `:644` (`g_subprocess_wait_async(r->proc, NULL, shot_done, r)`), `:89-107` (`req_free`), `:126-133` (`req_close`).
`req_free` frees `r` and force-exits `r->proc`, but the wait was started with a NULL cancellable, so `shot_done` still runs. It then reads `r->handle` at `:606` and `r->proc` after that, both freed memory.
Trigger: any session-bus client calls `org.freedesktop.impl.portal.Screenshot.Screenshot` with handle H, then `org.freedesktop.impl.portal.Request.Close` on H before snappel exits. Requires no privilege. Verified in code, not run.

## High

**H1. Popups from `window.open` are blocked silently, and the opener waits forever.**
- `src/freebws/freebws.c:2346-2366` (`settings_apply`) never calls `webkit_settings_set_javascript_can_open_windows_automatically`. Only the feature list and the page-cache env override are set. WebKit's default for this setting is FALSE (see the uncertain list).
- Consequence: a `window.open` that is not in a user gesture (after an `await`, or some seconds after the click) is dropped in WebKit before the `create` signal. FreeBWS shows nothing, and the page waits for a popup that never opens.
- Second cause: `src/freebws/freebws.c:2000-2007` (`on_create`) returns NULL when `tab_create` fails. `tab_create` at `:612-615` fails silently at `MAX_TABS` (24 tabs, `:64`). The popup is dropped, with no message.
- Trigger: click "Continue with Google" where claude.ai calls `window.open` after a `fetch`. Also: have 24 tabs open and click any popup login.

**H2. The OAuth opener is backgrounded, then discarded, and loses its `window.opener` round trip.**
- `src/freebws/freebws.c:556-569` (`tab_activate`): the popup becomes `B.cur`, and the opener loses `visible` and `focused` (`:566-567`). WebKit throttles the opener and its JS state is no longer driven.
- `:2336` `B.idle_s = 600` by default. `:2390-2412` (`mem_tick`) discards a background tab idle longer than that (`:2398-2401`). `:2341-2342` also discards the oldest background tab when MemAvailable is below `min(MemTotal/5, 384)` MB, which is about 200 MB on a 1 GB laptop (`:2407-2409`).
- `:669-683` (`tab_discard`) calls `tab_unrealize`, which destroys the view and its web process state. The opener's `window.opener` target and any pending `postMessage` are gone.
- Trigger: a Google sign-in that stays open more than 10 minutes (2FA, passkey, account chooser), or any sign-in on a 1 GB machine under memory pressure. Test: `FREEBWS_DISCARD_IDLE=0 FREEBWS_DISCARD_MB=0`.

**H3. No user agent is set.**
- `grep -rn -i 'user.agent\|useragent' src/freebws` finds only `adblock.c:408` (the list downloader's UA). `settings_apply` (`freebws.c:2346-2366`) leaves WebKit's default UA, which does not look like Chrome or Firefox.
- Google's sign-in and Cloudflare's challenge both use the UA to decide whether to refuse or degrade an embedded browser.
- Trigger: sign in to Google in a FreeBWS tab. Test: `FREEBWS_EVAL=navigator.userAgent` logs the UA on each page load. Whether Google refuses this exact UA is not verified here (uncertain list).

**H4. The sandbox "on" verdict does not test the web process.**
- `src/freebws/freebws.c:2177-2238` (`sandbox_probe`) runs `bwrap` with only `/usr/lib`, `/usr/bin`, `/lib`, `/lib64`, `/bin`, `--dev /dev`, `--proc /proc`, `--tmpfs /tmp` (`:2213-2218`), then runs `freebws --sandbox-child`, which exits at once (`:2934-2935`).
- `:2255-2258` then sets `pages_sandbox_status` to "Web process sandbox: on". No web process was started, so the claim is unverified.
- The two comments contradict each other: `:2151` says WebKit binds `/usr`, `/etc`, fonts and GStreamer data; `:2197-2199` says WebKit binds only `/usr/lib`, `/usr/bin`, `/lib`, `/lib64`, `/bin`, one by one. Neither the probe nor the comments mention `/etc/fonts`, `/usr/share/fonts`, `/etc/localtime` or `/dev/shm`.
- Trigger: any page that needs fonts, timezone or shared memory in the web process. Test: `FREEBWS_SANDBOX=off` and the same page. Login-specific effect unknown.

**H6. The ad blocker's first-start gate can load a stale URL over a newer navigation.**
- `src/freebws/freebws.c:1010-1027` (`navigate_hook`): when `t->gating` is already 1 it returns 0 at `:1012` and the new load starts at once, without filters.
- `src/freebws/freebws.c:2725-2741` (`fb_adb_changed`) then loads `t->adb.deferred` with `webkit_web_view_load_uri` at `:2736`. That is the URL the tab asked for before the new navigation, so it overwrites the page the user is now on.
- `adblock.c:1098-1103` sets `deferred` every time `adb_navigate` runs while not ready. The gate is armed for up to 120 s on first start (`adblock.c:856`, `GATE_TIMEOUT_S` defined at `:36`). A filter rebuild after a list update takes the same path.
- Trigger: first start (or the first start after a list update) and a link click or redirect while the first navigation is held. The old URL, perhaps an OAuth step, loads after the new one.

**H7. flportal impl interfaces accept calls from any session-bus client.**
- `src/flportal/main.c:700-704` (`method_call`) logs `sender` and never checks it. The `org.freedesktop.impl.portal.*` names are on the session bus, so any process can call them. The frontend's permission store is never consulted.
- Consequences: the FileChooser branch (`:705-716`) lets any client open a dialog with a caller-chosen title and receive the chosen URIs. The Screenshot branch (near `:771`, `screenshot_call` at `:622-646`) runs `snappel --full` with no confirmation and returns the saved file's URI. The Notification branch (`:741-752`) and the Settings branch (after `:752`) work the same way.
- Trigger: `gdbus call --session -d org.freedesktop.impl.portal.desktop.freelfs -o /org/freedesktop/portal/desktop -m org.freedesktop.impl.portal.Screenshot.Screenshot ...` (bus name from `main.c:38`, path from `:39`). Severity depends on whether sandboxed apps can reach the session bus directly. This is the bypass of the consent model that matters.

## Medium

**M1. `count.js` replaces `window.fetch` on every page, including cross-origin frames.**
- `src/freebws/data/count.js:25-26`: `var of = window.fetch; window.fetch = function (inp) {...}`. The wrapper calls `p.catch` on every fetch (`:27-30`).
- It is injected on every tab with the blocker on. `src/freebws/adblock.c:979-988` (`adb_tab_refilter`) adds the yt/count script whenever `A.enabled && !s->off`, even when no filter list is loaded, and `INJECT_ALL_FRAMES` makes it run in every frame.
- Effect: `fetch.toString()` is no longer native code. Bot checks (Cloudflare, some Google flows) look for exactly that. Test: `FREEBWS_ADB_SKIP=yt` turns the script off (`adblock.c:979`).

**M2. A popup closed with `window.close()` activates the wrong tab.**
- `src/freebws/freebws.c:2009-2012` (`on_close_req`) queues `idle_close`. `tab_close` (`:637-665`) picks `B.tabs[i + 1]` (`:650-652`), the tab to the right of the popup, not the opener.
- Consequence: the opener stays backgrounded (see H2) and the user lands on an unrelated tab. Looks like "the popup did not return".

**M3. A redirect during gating is stopped and replayed as GET.**
- `src/freebws/freebws.c:1926-1934` (`on_load`, `LOAD_REDIRECTED`): when `navigate_hook` returns 1, the redirect is stopped with `webkit_web_view_stop_loading`. `fb_adb_changed` then reloads the target with `webkit_web_view_load_uri` (`:2736`).
- A POST-based redirect (a form_post or SAML step) comes back as GET. Only while filters are not ready (H6 window).

**M4. A pointer release outside the page is dropped, so WebKit keeps the button down.**
- `src/freebws/freebws.c:358-362` (`p_leave`) clears `B.in_page`. `:373-386` (`p_button`) returns before forwarding anything when `!B.in_page`, so a release outside the page is never sent.
- Trigger: press in the page, drag out, release outside. WebKit's next click becomes a drag, and links and buttons stop firing.

**M5. Frames are dropped for good when all four shm buffers stay busy.**
- `src/freebws/freebws.c:195-213` (`sbuf_get`): when every slot is `busy` and the size matches, it returns NULL and `show_pixels` (`:243-259`) drops the frame. `busy` is cleared only by `sb_release` (`:188-192`), i.e. by `wl_buffer.release` from flcomp.
- Trigger: a compositor that does not send `release` for an attached buffer (flcomp bug or a resize race). The page keeps loading but stops painting.

**M6. Out of memory in the dmabuf path leaves the tab frozen.**
- `src/freebws/freebws.c:287-316` (`export_dmabuf`): the `calloc` failure at `:303-306` returns without `dispatch_release_buffer` and without `frame_complete`, so WebKit waits for a frame that never comes. Also `pa` (`:295`) and the result of `create_immed` (`:300-301`) are used without a NULL check.
- Trigger: OOM at that instant. Low chance, but it leaves the page stuck with no error.

**M7. Finished downloads are opened with `gio open` (any type).**
- `src/freebws/downloads.c:435-449` (`dl_open`): a text file goes to `fledit`, anything else goes to `gio open <path>`. A downloaded `.desktop` file or other launchable type runs its handler.
- Trigger: a web page offers a `.desktop` or an app-typed file, the user clicks it in the downloads panel. User-initiated, but the file is untrusted.

**M8. flportal request handles are caller-chosen and can be closed by any client.**
- `src/flportal/main.c:139-147` (`req_new`) registers the object at the handle the caller gives. Nothing checks it is under the caller's own `/org/freedesktop/portal/desktop/request/<sender>/` path.
- `:126-133` (`req_close`) answers `Request.Close` from any client. So one client can cancel another client's pending dialog or screenshot by knowing its handle.

## Low

- `src/freebws/freebws.c:510-528` (`tab_unrealize`) never resets `t->gating`. `adb_tab_free` frees `deferred` (`adblock.c:1011-1012`), so a discarded tab stays at `gating == 1` and `navigate_hook` skips the blocker for it for the rest of its life. (`adblock.c:1012`, `freebws.c:1012`.)
- `src/freebws/freebws.c:500-501`: `t->view = g_object_new(...)` is not checked for NULL before `settings_apply` and `tab_signals` use it.
- `src/freebws/downloads.c:143`: `atoi(strchr(s, ':') + 1)` dereferences NULL if `downloads.json` has `"next"` with no `:` after it. Only on a corrupt file.
- `src/freebws/downloads.c:222-226` (`unique_path`): after 999 name collisions the last tried name is used even though it exists, so a finished download can overwrite an older one.
- `src/freebws/pages.c:312-322` (`bm.import`): the settings page can make the browser read any local file path and parse it as bookmarks. Only a count is returned. Limited because only the fixed `freebws://settings` page can post to the bridge.
- `src/libdssl/main.c:110`: `fputs(text, out)` and the final `fclose(out)` (`:118`) are not checked, so a full disk or a broken pipe still exits 0 with truncated output.

## Checked and found clean

- `src/flportal/filechooser.c`: SaveFile rejects names with `/`, `.` and `..` (about `:584-612`). SaveFiles uses `g_path_get_basename` on the caller's names (`:1483`) and `unique_name` (`:556-572`). `load_dir` uses `realpath` (`:358`). `finish` only sends paths built from `cwd` and readdir names. Reply after close is guarded: `draw` returns early when `!f->d.req` (`:1270`), and `win_closed` replies once.
- `src/flportal/main.c`: `req_free` clears `dlg->req`, so the dialog cannot reply to a freed request.
- `src/flstore/index.c:32-44` (`valid_appid`) rejects `/`, leading `.`, leading `-`, and needs a dot. Icon and screenshot paths (`flstore.c:484-502`) use the validated id. Icon and screenshot URLs must be `https://` (`index.c:457`). curl runs with argv, not a shell.
- `src/libdssl/util.c` arena: `setjmp` recovery is reset on every exit path of `dssl_parse` (`resolve.c:1700-1727`). No double-free or stale jump buffer found in the parts read.
- `src/freebws/freebws.c:2016-2059` (`on_policy`) only turns responses into downloads when the MIME type is unsupported or `Content-Disposition: attachment`. The ad-blocker lists were checked with `xz -dc`: they have no login-domain blocks (only `gen_204` and `accounts.google.com/gsi/log` in EasyPrivacy, `easylist.txt.xz` lines 61285-61293, `easyprivacy.txt.xz` lines 3181-3183, 10665, 11961-11981).

## Uncertain (not verified in code or not reproduced)

- **WebKit default for `javascript-can-open-windows-automatically`** (H1). Believed FALSE in WebKit's settings defaults. Check at runtime with `webkit_settings_get_javascript_can_open_windows_automatically` on startup, or add a log line.
- **Whether Google refuses WPE's default UA** (H3). Needs the exact UA string; `FREEBWS_EVAL=navigator.userAgent` gives it.
- **COOP severing the popup's opener.** Google's login pages set `Cross-Origin-Opener-Policy`. If the WPE version enforces COOP, `window.opener` is cut when the popup navigates cross-origin, and `ProcessSwapOnCrossSiteNavigation=0` (`freebws.c:2329`, for under 3 GB) does not change that. Depends on the WPE version.
- **Cookie policy and ITP.** No cookie accept policy or ITP setting is set (`grep -i cookie` finds nothing in `src/freebws` except comments). WebKit's defaults decide whether third-party iframe cookies (Google's `gsi` frame, Cloudflare's frame) are kept. Not verified.
- **WebGL and CPU rendering.** `WEBKIT_SKIA_ENABLE_CPU_RENDERING=1` (`freebws.c:2982-2983`) may disable accelerated WebGL. Challenge pages that need WebGL could hang. Not verified.
- **bwrap `--dev` and `/dev/shm`** in the web process sandbox (H4). Not verified whether `--dev` provides `/dev/shm`.
- **HTTP/2 and websockets.** Nothing in FreeBWS sets network options, so these are WebKit/libsoup defaults. Not verified.
- **Subframe downloads.** `on_policy` (`freebws.c:2021-2031`) also runs for subframe responses. A Google or Cloudflare iframe with an unusual MIME type would become a download and its load would stop. Not checked against real responses.
- **Screenshot UI.** Whether `snappel --full` shows any UI is outside this scan. If it is silent, C1's trigger and H7's screenshot path are as described.
- **Whether another process already owns `org.freedesktop.impl.portal.desktop.freelfs`** (H7's trigger). flportal requests it at `main.c:945`; a live bus was not checked.
