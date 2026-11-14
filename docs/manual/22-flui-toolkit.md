# 22. The FLUI toolkit (libflui)

Summary: libflui is the FreeLFS desktop toolkit, a C17 shared library that draws immediate-mode widgets into
Wayland windows with its own CPU rendering, text, theme and config helpers; every FreeLFS desktop program is built on it.

## Contents

1. What libflui is, and where it lives
2. The model: immediate mode
3. Building and installing the library
4. The application: `fl_app`
5. Windows: `fl_win_opts` and `fl_win`
6. The canvas and drawing
7. Icons and paths
8. Text and fonts
9. Theme, look and desktop.conf (`fl_th`, `fl_lk`, `fl_conf_*`)
10. Widgets
11. Keyboard, focus and pointer
12. Timers, file descriptors and the clipboard
13. Running inside another main loop (GLib)
14. Testing without a compositor (`FLUI_PPM`)
15. Example: a "hello window" program
16. Things that are not in libflui

---

## 1. What libflui is, and where it lives

Source: `src/flui/`. Public header: `flui.h`; the palette header `flpalette.h` is installed next to it. Package:
recipe `flui` (group `desktop`). The shared library is `libflui.so.0`, with the pkg-config name `flui`.

| File | What it holds |
|---|---|
| `gfx.c` | the canvas (rectangles, anti-aliased paths, SVG path data) and the built-in icons |
| `text.c` | fontconfig, FreeType and HarfBuzz: fonts, glyph cache, fallback fonts, text layout |
| `conf.c` | theme tokens, the look settings, the accent colour, `desktop.conf` and its inotify watch |
| `ui.c` | the widgets: label, button, icon button, text field, switch, scrollbar, list, grid helper, title bar |
| `kbd.c` | the four keyboard layouts, `keyboard.conf`, the layout tiles and the key preview |
| `worldmap.c`, `worldmap_data.c` | time zones (from tzdata), the flat world map widget, the city search field |
| `wl.c` | the Wayland client: windows, `wl_shm` buffers, pointer, keyboard (xkbcommon, key repeat, dead keys), clipboard, the poll loop |
| `flpalette.h` | the colour tokens of both schemes and the accent colours, header only, shared with `flcomp` |
| `internal.h` | private declarations (not installed) |
| `tools/mkworldmap.py` | regenerates `worldmap_data.c` from Natural Earth data (public domain) |

Programs built on it (from the README): `flpanel`, `flterm`, `flsetup`, `flsettings`, `flfiles`, `fledit`,
`flgreet-ui`, and the others in `src/`.

The library is C17 with `-Wall -Wextra`, compiled with `-fvisibility=hidden`. Public functions carry `FL_API`
(`__attribute__((visibility("default")))`). Only those are exported.

## 2. The model: immediate mode

A window has one draw callback. Each input event (a key, a pointer move, a click, a timer, a Wayland event) runs the
callback once. Inside it, every widget call both draws the widget and returns what the user did with it this time:

```c
if (fl_button(ui, ID_OK, rect, "Continue", FL_BTN_ACCENT))
	go_on();
```

Consequences:

- Widgets have no objects to create or destroy. The state that has to survive between passes (focus, the text
  cursor, which button is held, a list's scroll position) lives in `fl_ui` (the window) or in a struct you own
  (for example `fl_list_state`, `fl_tzsearch`).
- A widget is identified by an ID (`uint32_t`) that you choose. Use the same ID for the same widget in every pass.
  Enumerations are the usual way to write them (`enum { ID_OK = 1, ID_CANCEL };`).
- Idle means no work. The loop blocks in `poll(2)` until something happens. A pass redraws the whole (small) window
  into a free `wl_shm` buffer. If the compositor still holds both buffers, the pass runs "dry": input is handled and
  nothing is drawn (`ui->cv.px` is NULL), and the picture follows when the buffer comes back.
- Your callback must be safe to run many times per second, and must not keep pointers to widget state across passes
  that it does not own.

## 3. Building and installing the library

From `src/flui/`:

```sh
make                 # builds libflui.so.0, libflui.so and flui.pc
make install         # PREFIX=/usr, LIBDIR, DESTDIR honoured
```

Installed files: `libflui.so.0` and its link, `flui.h`, `flpalette.h`, `flui.pc`, and the README under `share/doc/flui`.
Build dependencies: `wayland-client`, `freetype2`, `harfbuzz`, `fontconfig`, `xkbcommon`, and `wayland-protocols` (for
`xdg-shell.xml`, which the Makefile turns into C with `wayland-scanner`). The fonts come from the `ibm-plex` package
(IBM Plex Sans for the UI, Plex Mono for code).

A program that uses the library (for example `src/flview/Makefile`) builds with
`$(shell pkg-config --cflags --libs flui)` added to its own flags.

The flui recipe is `recipes/flui/recipe`. It copies `src/flui` into the build directory and runs `make` and `make DESTDIR=... install`.

## 4. The application: `fl_app`

`fl_app` is the connection to the compositor and the event loop.

| Function | Use |
|---|---|
| `fl_app *fl_app_new(fl_global_cb global_cb, void *ud)` | connects to the Wayland display. Returns NULL when there is no compositor (`wl_display_connect` fails), unless `FLUI_PPM` is set. The callback, if not NULL, sees every `wl_registry` global, to bind private protocols. Loads `desktop.conf` (`fl_conf_load`). |
| `void fl_app_run(fl_app *a)` | runs until `fl_app_quit`. |
| `void fl_app_quit(fl_app *a)` | makes `fl_app_run` return. The default close action of a window calls this. |
| `void fl_app_free(fl_app *a)` | releases it. Call after `fl_app_run` returns. |
| `int fl_app_watch_conf(fl_app *a, void (*cb)(void *ud), void *ud)` | reloads `desktop.conf` when it changes, applies the theme and the look, redraws the windows, and then calls `cb` (which may be NULL). |
| `int fl_app_mods(fl_app *a)` | the `FL_MOD_*` modifier keys held right now. `ui->mods` is only as fresh as the last key press. |
| `void fl_app_output_size(fl_app *a, int *w, int *h)` | the output size (1280 x 800 when unknown). |
| `void fl_app_set_layout(fl_app *a, const char *xkb_layout)` | use another keyboard layout name (`"de"`, `"gb"`) for this app's keys. Used by the setup program's test field. |
| `FL_API struct wl_display *fl_app_display(fl_app *a)` | the display, for code that talks to Wayland itself. |

The usual `main()`:

```c
fl_app *app = fl_app_new(NULL, NULL);
if (!app) { fprintf(stderr, "prog: cannot connect to a Wayland compositor\n"); return 1; }
/* fonts and timers can be made here, after fl_app_new */
/* windows: fl_win_new(app, &opts) */
fl_app_watch_conf(app, NULL, NULL);
fl_app_run(app);
fl_app_free(app);
return 0;
```

The same pattern is in `src/flview/flview.c` and `src/fltheme/fltheme.c`.

## 5. Windows: `fl_win_opts` and `fl_win`

`fl_win_new(app, &opts)` creates a window from a `fl_win_opts` struct:

| Field | Meaning |
|---|---|
| `title`, `app_id` | the window title, and the Wayland app id (the panel and the icon lookup use it; `fl_icon_for_app`) |
| `w`, `h` | initial size. For raw windows, the fixed size |
| `min_w`, `min_h` | minimum size |
| `decorated` | 1: libflui draws the border, title bar and buttons (client-side). 0: no decoration |
| `raw` | 1: a bare `wl_surface`, the caller gives it its role (`fl_shell` panel, desktop, notifications). See chapter 23 |
| `fullscreen` | start fullscreen |
| `resizable` | default 1 for toplevels |
| `font_px` | UI font size; default 13 |
| `draw` | the draw callback `void draw(fl_ui *ui, void *ud)` (required for normal windows) |
| `close` | called when the close button is pressed; default `fl_app_quit` |
| `key` | raw key callback; return 1 to stop the widgets seeing the key |
| `pointer` | raw pointer callback (`FL_PTR_MOVE`, `FL_PTR_DOWN`, `FL_PTR_UP`, `FL_PTR_WHEEL`) |
| `resized`, `focus` | content size changed; focus gained or lost |
| `ud` | your data pointer, given back to every callback |

Other window functions: `fl_win_destroy`, `fl_win_redraw`, `fl_win_redraw_all`, `fl_win_set_title`, `fl_win_resize`
(raw windows), `fl_win_content` (the rectangle inside the decorations, the place for your widgets),
`fl_win_surface`, `fl_win_ui`, `fl_win_active`, `fl_win_minimize`, `fl_win_maximize` (the work area below the panel,
not fullscreen), `fl_win_map` and `fl_win_hide` (raw windows only).

The `fl_ui` you get in the callback is the window's state (`fl_win_ui(w)` gives it outside the callback):

- `ui->cv`: the canvas to draw on (`fl_canvas`: `px`, `w`, `h`, `stride`, `clip`).
- `ui->win`: the window.
- `ui->mx`, `ui->my`, `ui->mouse_in`: pointer position and whether it is over the window.
- `ui->press`, `ui->release`, `ui->btn_held`, `ui->clicks`, `ui->wheel`: pointer events of this pass (`FL_BTN_L`,
  `FL_BTN_R`, `FL_BTN_M` bits). `clicks` counts double and triple clicks.
- `ui->key`, `ui->mods`, `ui->text`: the key pressed or repeated in this pass (an XKB keysym), the modifiers, and what
  the key types as UTF-8.
- `ui->focus`, `ui->active`: the focused and the pressed widget IDs.
- `ui->cursor`: set to `FL_CURSOR_IBEAM` by a text field; reset each pass.
- `ui->f_reg`, `ui->f_med`, `ui->f_semi`: the window's UI fonts (weights 400, 500, 600).

## 6. The canvas and drawing

`fl_canvas` holds ARGB8888 pixels. Colours are `fl_color` values, `0xAARRGGBB` with straight (not premultiplied)
alpha. `FL_HEX(0x1d2125)` makes an opaque colour from an RGB value.

| Function | What it does |
|---|---|
| `fl_fill_rect(cv, r, c)` | fills a rectangle. Alpha is blended. |
| `fl_stroke_rect(cv, r, thick, c)` | a border inside `r`, `thick` pixels wide |
| `fl_clip(cv, r)` | intersects the clip rectangle |
| `fl_mix(a, b, t255)` | a colour `t255/255` of the way from `a` to `b` |
| `fl_r(x, y, w, h)`, `fl_inset(r, dx, dy)`, `fl_in(r, x, y)`, `fl_intersect(a, b)` | rectangle helpers |
| `fl_cut_left/right/top/bottom(&r, n)` | takes a slice off a rectangle and returns it; the rest stays in `r`. Use these for layouts. |

Paths (anti-aliased, own rasteriser):

- `fl_path_new()`, `fl_path_free()`, `fl_path_move`, `fl_path_line`, `fl_path_quad`, `fl_path_cubic`, `fl_path_close`.
- `fl_path_svg(p, "M0 0 L10 10 Z", sx, sy, ox, oy)`: appends SVG path data (`M L H V C S Q A Z`, absolute and relative).
- `fl_path_fill(cv, p, colour)`, `fl_path_stroke(cv, p, width, colour)`.

Work with the canvas only inside the draw callback. Do not keep `ui->cv.px` across passes.

## 7. Icons and paths

`fl_icon_draw(cv, icon, x, y, size, colour)` draws a built-in icon into a square. The enumeration `fl_icon` has
these members (each with the `FL_ICON_` prefix): `MENU`, `FOLDER`, `TERMINAL`, `WIFI`, `VOLUME`, `BATTERY`, `MINIMIZE`, `MAXIMIZE`, `RESTORE`,
`CLOSE`, `BACK`, `FORWARD`, `SETTINGS`, `WINDOW`, `FILE`, `DOC`, `DRIVE`, `HOME`, `TRASH`, `MUTE`, `ETHERNET`, `UP`,
`PLUS`, `FOLDER_LG`, `FILE_LG`, `DOC_LG`, `DISPLAY`, `MOUSE`, `KEYBOARD`, `CLOCK` (the `FL_ICON_` prefix on each).
New icons go before `FL_ICON_COUNT`, as the header asks.

- `fl_icon_battery(cv, x, y, size, colour, pct)`: the battery with a fill level.
- `fl_icon_for_app(app_id)`: the icon for a Wayland app id. `flterm` (or any id containing "terminal") gets the
  terminal icon, "settings" the settings icon, "files" the folder, "edit" the document. Anything else gets `FL_ICON_WINDOW`.
- `fl_icon_button(ui, id, rect, icon, size, colour)` is a button with an icon.

There are no image files for icons. Pictures (PNG, JPEG, WebP) are decoded by the program that shows them
(`src/flview` uses libpng, libjpeg-turbo and libwebp directly). libflui does not load image files.

## 8. Text and fonts

| Function | Purpose |
|---|---|
| `fl_font_ui(weight, px)` | the UI font: `fl_lk.ui_font` (IBM Plex Sans by default), size `px * font_size / 13` |
| `fl_font_mono(weight, px)` | IBM Plex Mono, not affected by the UI font setting |
| `fl_font_open(family, weight, italic, px)` | any family fontconfig knows |
| `fl_font_family_exists(family)` | 1 when fontconfig has the family (not a substitute). For pickers. |
| `fl_font_close(f)` | releases a font |
| `fl_font_ascent`, `fl_font_height`, `fl_font_cell` | metrics. `cell` is the advance of "0" (monospace) |
| `fl_text_width(f, s, len)` | width in pixels. `len < 0` means NUL terminated. |
| `fl_text(cv, f, x, y, colour, s, len)` | draws text. `y` is the top of the line box. |
| `fl_text_ex(..., spacing64)` | the same with letter spacing in 1/64 px |
| `fl_text_in(cv, f, r, align, pad, colour, s)` | vertically centred in `r`, aligned `FL_LEFT`, `FL_CENTER` or `FL_RIGHT`, clipped to `r` |
| `fl_glyph(cv, f, x, y, colour, codepoint)` | one code point in a terminal cell |
| `fl_text_fit(f, s, w, out, cap)` | the longest prefix that fits in `w` pixels, with "..." |

Weights are 400, 500, 600 and 700. Sizes may be fractional. Text uses HarfBuzz for shaping, FreeType for glyphs, and a
glyph cache. Fallback fonts are used for characters the main font lacks.

`fl_text_set_look(family, scale)` is called by the look code when the person changes the font in Settings. Apps do not
need to call it.

## 9. Theme, look and desktop.conf

### 9.1 Colour tokens (`fl_th`)

`extern FL_API fl_theme fl_th;` is the live theme. Widgets read it on every draw, so changing it changes the next pass.
Fields: `bg`, `panel`, `raised`, `line`, `text`, `dim`, `accent`, `on_accent`, `select`, `strong`, `term_bg`, `err`, and
`dark` (an int). `fl_theme_set(dark)` applies the colour scheme. The values come from `flpalette.h`:

| Token | Dark scheme (default) | Used for |
|---|---|---|
| `bg` | `1d2125` | the desktop and the darkest surface |
| `panel` | `272c33` | window chrome, the panel, cards |
| `raised` | `343a43` | hover and pressed fills, inputs |
| `line` | `3a4048` | hairlines and borders |
| `text` / `strong` / `dim` | `d8dce0` / brighter / `8a9199` | body text, headings, secondary text |
| `accent` | green `3f9b63` by default | the user's accent colour |

The accent has six choices (`fl_accents`): green `3f9b63`, teal `35a09b`, blue `4d8fd0`, amber `d0a032`, orange
`e0793f` and rose `d4678c` (dark scheme values; the light scheme has darker ones in `flpalette.h`). Pick one with
`fl_accent_find(id)` (index or -1) and read the configured one with `fl_accent_current(dark)`.

The light scheme (`theme=light`) is defined in `flpalette.h` and its contrast is checked by `make check` in
`src/fltheme`.

Use the tokens, not literal colours, so that the schemes and the accent work. Corners are square everywhere: there is no
corner radius token. The project's look rules (flat, no gradients, no shadows, no emoji) are in `docs/AGENT-RULES.md`.

### 9.2 The look (`fl_lk`)

`extern FL_API fl_look fl_lk;` holds the settings that are not colours: `ui_font`, `font_px`, `buttons_left`,
`title_h` (34 normal, 28 compact), `panel_h`, `panel_top`, `panel_icon`, `anim` (0 off, 1 reduced, 2 on) and `anim_speed`.
Defaults are in the comment of `flui.h`: font `IBM Plex Sans`, `font_size=13`, `buttons=right`, `titlebar=normal`,
`panel_position=bottom`, `panel_height=44`, `panel_icon_size=20`, `animations=on`, `animation_speed=1.0`.

### 9.3 Config helpers (`fl_conf_*`)

The file is `$FLUI_CONF`, else `~/.config/freelfs/desktop.conf`. Format: `key=value`, one per line.

| Function | Purpose |
|---|---|
| `const char *fl_conf_path(void)` | the path in use |
| `void fl_conf_load(void)` | reads the file, applies `theme=` and the look. `fl_app_new` already calls it. |
| `const char *fl_conf_get(key, def)` | the value, or `def` when the key is absent. The pointer is valid until the next write. |
| `int fl_conf_int(key, def)` | the value as an integer |
| `int fl_conf_set(key, val)` | writes one key (and the file), returns 0 or -1 |
| `int fl_conf_set_many(pairs, n)` | writes several keys in one write: `{key, value, key, value, ...}`. A NULL value removes the key. Watchers wake once. |
| `int fl_conf_unset(key)` | removes a key |

Keys that libflui itself reads: `theme` (`dark` or `light`), `accent`, `font`, `font_size`, `buttons`, `titlebar`,
`panel_position`, `panel_height`, `panel_icon_size`, `animations`, `animation_speed`. Other programs read their own keys,
for example `power_saver` (flpanel writes it, flcomp reads it), `night_light` and `workspaces` (flcomp).
Chapter 23 lists the keys the compositor reads. Chapter 24 shows how an app uses its own keys.

Changing a key with `fl_conf_set` also updates the look at once, and every app that called `fl_app_watch_conf`
redraws with the new value.

## 10. Widgets

Every widget takes a `fl_ui *ui`, an ID and a rectangle, and returns what happened in this pass. All are declared in
`flui.h`.

| Widget | Call | Returns |
|---|---|---|
| primitive | `int fl_widget(ui, id, r, flags)` | state bits: `FL_HOVER`, `FL_DOWN`, `FL_CLICK`, `FL_FOCUS`, `FL_RCLICK`. Flags: `FL_FOCUSABLE` (takes Tab focus), `FL_NO_FOCUS_RING`. Every other widget is built on it. |
| label | `fl_label(ui, r, s, font, colour, align)` | nothing. Text in `r`, vertically centred. `font` may be NULL for `ui->f_reg`. |
| button | `int fl_button(ui, id, r, label, style)` | 1 when clicked, or when Enter or Space is pressed on the focused button. Styles: `FL_BTN_PLAIN`, `FL_BTN_OUTLINE`, `FL_BTN_ACCENT`. |
| icon button | `int fl_icon_button(ui, id, r, icon, size, colour)` | 1 when clicked |
| text field | `int fl_textfield(ui, id, r, buf, cap, placeholder, font)` | `FL_EDIT_CHANGED` (1) when the text changed, `FL_EDIT_ENTER` (2) on Enter. Edits `buf` in place (UTF-8, with a cursor and selection). |
| switch | `int fl_switch(ui, id, r, int *on)` | 1 when toggled. Changes `*on`. |
| scrollbar | `int fl_scrollbar(ui, id, r, int *off, content, view)` | nothing; `*off` is the scroll offset in pixels |
| list | `int fl_list(ui, id, r, count, item_h, fl_list_state *st, draw, ud)` | the index activated by a click or Enter, or -1. Up, Down, Home, End and Page keys move the selection. `draw` is called per visible row. |
| grid cell | `fl_rect fl_grid_cell(const fl_grid *g, int i)` | the rectangle of cell `i` of a grid |
| title bar | `int fl_titlebar(ui, r, title, focused, maximized)` | `FL_ICON_MINIMIZE`, `FL_ICON_MAXIMIZE` or `FL_ICON_CLOSE` when that button is clicked, else -1 |
| keyboard tiles | `int fl_kbd_tiles(ui, id_base, r, gap, sel)` | the index of the layout clicked, or -1 |
| key preview | `fl_kbd_preview(ui, x, y, layout)` | nothing. Draws three rows of keys. |
| dead keys | `int fl_compose_feed(fl_compose *c, sym, utf8, out, cap)` | bytes written to `out` (0 when swallowed, waiting for the second key) |
| world map | `int fl_worldmap_draw(ui, id, r, map, sel, &hover)` | the time zone clicked, or -1. Create with `fl_worldmap_new()`, free with `fl_worldmap_free()`. |
| zone search | `fl_tzsearch_field(...)`, `fl_tzsearch_popup(...)`, `fl_tzsearch_covers(...)` | a search field with a suggestion list that opens over the widgets below. Draw the field first, then the widgets, then the popup. |

Widget IDs must be unique within a window and stable between passes. The primitive `fl_widget` also sets focus:
a click on a `FL_FOCUSABLE` widget focuses it, and Tab moves focus through the focusable widgets in the order they are drawn.

Time zones (`fl_tz_*`) read zone.tab from tzdata (`$FL_ZONEINFO`, else `/usr/share/zoneinfo`). `fl_tz_local`, `fl_tz_mktime`,
`fl_tz_fmt_offset` and `fl_tz_fmt_time` format and convert times. `FL_NOW=EPOCH` fixes the time for tests and screenshots.

## 11. Keyboard, focus and pointer

- Enter, Space and Keypad Enter activate a focused button or switch (`key_activate`).
- Tab and Shift-Tab move focus. The first and last focusable widgets wrap.
- `ui->key` is set only in the pass where the key was pressed or repeated, and the widget that handles it clears it.
- Key repeat follows the compositor's `repeat_info` event (`wl.c`: 25 per second and a 400 ms delay until the
  compositor says otherwise; the compositor reads `key_repeat_rate` and `key_repeat_delay` from `desktop.conf`).
- `ui->clicks` counts a double click when the second press comes within 400 ms (the value the test driver in
  `build/live-run.py` also assumes).
- A raw `key` or `pointer` callback in `fl_win_opts` sees input before the widgets; return 1 to keep them from it.

## 12. Timers, file descriptors and the clipboard

| Function | Purpose |
|---|---|
| `fl_timer_new(app, cb, ud)` | makes a timer. The callback runs when it fires. |
| `fl_timer_arm(t, first_ms, every_ms)` | starts it: first after `first_ms`, then every `every_ms`. `0, 0` stops it. |
| `fl_timer_free(t)` | releases it |
| `fl_fd_add(app, fd, events, cb, ud)` | calls `cb(ud, fd, revents)` when `fd` is ready for `events` (a `poll(2)` mask) |
| `fl_fd_remove(app, fd)` | stops watching `fd` |
| `fl_clipboard_set(app, text, len)` | takes the Wayland clipboard (`wl_data_device`) |
| `char *fl_clipboard_get(app, &len)` | returns malloc'ed text from the clipboard, or NULL. Free it. |

Timers and fds run in the same loop as the windows, so the callbacks may change state and then call `fl_win_redraw`.
flview uses a timer to clear its status message after 15 seconds, and a timer to load thumbnails in the background.

## 13. Running inside another main loop (GLib)

Programs that already run a GLib main loop (FreeBWS, through WebKit) cannot call `fl_app_run`. Use the split API:

- `int fl_app_prepare(app, struct pollfd *pf, int max)`: draws the dirty windows, dispatches pending events, and fills
  `pf` with the display's fd and every fd added with `fl_fd_add`. Returns how many. -1 is an error, and -2 means `max`
  was too small (call it again with a bigger array).
- Wait on the descriptors (GLib does this).
- `int fl_app_dispatch(app, pf, n, r)`: handles the events. `r` is the result of the wait (0 when nothing was ready).
  -1 means the Wayland connection is gone.
- `fl_app_quitting(app)` tells the loop to stop.

FreeBWS (`src/freebws/freebws.c`) uses this so that WebKit's signals and libflui's input share one thread.

## 14. Testing without a compositor (`FLUI_PPM`)

Set environment variables before the program starts:

| Variable | Effect |
|---|---|
| `FLUI_PPM=FILE` | no compositor: `fl_app_new` does not connect, and `fl_app_run` renders the first frame of each window to `FILE` (`FILE.1`, `FILE.2` for more windows) as a PPM picture, then returns. |
| `FLUI_PPM_DELAY=MS` | lets fds and timers run for MS milliseconds before the picture is taken |
| `FLUI_CONF=FILE` | another `desktop.conf` path |
| `FL_KEYBOARD_CONF=FILE` | another `keyboard.conf` (default `/etc/keyboard.conf`) |
| `FL_ZONEINFO`, `FL_TZ_ETC`, `FL_NOW` | the time zone test hooks (section 10) |

Programs use these in their own tests. For example, flview's `tests/run.sh` decodes a generated folder, drives the viewer and
checks the PPM. `fltheme` reads `FLTHEME_SIZE` and `FLTHEME_TEST_SAVE` only when `FLUI_PPM` is set.

## 15. Example: a "hello window" program

This program uses only functions declared in `flui.h`. It shows a title line, a button that counts clicks, a switch, and a
Close button. It is written to the real API; it has not been compiled in this pass (builds were not allowed for this
documentation task). Build it with the flags of any program in `src/` (for example `cc -std=c17 -D_GNU_SOURCE $(pkg-config --cflags flui) hello.c $(pkg-config --libs flui) -o hello`).

```c
/* hello.c -- a window with a title, a counting button, a switch and Close */
#include <flui.h>
#include <stdio.h>

enum { ID_COUNT = 1, ID_SWITCH, ID_CLOSE };

struct hello {
	fl_app *app;
	fl_font *title;
	int clicks;
	int on;
};

static void draw(fl_ui *ui, void *ud)
{
	struct hello *h = ud;
	char buf[64];
	fl_rect c = fl_win_content(ui->win);

	fl_fill_rect(&ui->cv, c, fl_th.bg);

	fl_rect row = fl_r(c.x + 24, c.y + 20, c.w - 48, 32);
	fl_label(ui, row, "Hello from libflui", h->title, fl_th.strong, FL_LEFT);

	fl_rect b = fl_r(c.x + 24, c.y + 70, 150, 32);
	snprintf(buf, sizeof buf, "Clicked %d", h->clicks);
	if (fl_button(ui, ID_COUNT, b, buf, FL_BTN_ACCENT))
		h->clicks++;

	fl_rect s = fl_r(c.x + 24, c.y + 120, 200, 24);
	fl_switch(ui, ID_SWITCH, s, &h->on);
	fl_label(ui, fl_r(c.x + 76, c.y + 120, 150, 24),
	         h->on ? "Details on" : "Details off", NULL, fl_th.dim, FL_LEFT);

	fl_rect q = fl_r(c.x + c.w - 124, c.y + c.h - 52, 100, 32);
	if (fl_button(ui, ID_CLOSE, q, "Close", FL_BTN_OUTLINE))
		fl_app_quit(h->app);
}

int main(void)
{
	struct hello h = {0};

	h.app = fl_app_new(NULL, NULL);
	if (!h.app) {
		fprintf(stderr, "hello: cannot connect to a Wayland compositor\n");
		return 1;
	}
	h.title = fl_font_ui(500, 16);

	fl_win_opts o = {
		.title = "Hello", .app_id = "hello", .w = 360, .h = 220,
		.min_w = 300, .min_h = 200, .decorated = 1, .resizable = 1,
		.draw = draw, .ud = &h,
	};
	if (!fl_win_new(h.app, &o)) {
		fprintf(stderr, "hello: cannot open the window\n");
		return 1;
	}

	fl_app_watch_conf(h.app, NULL, NULL);
	fl_app_run(h.app);
	fl_font_close(h.title);
	fl_app_free(h.app);
	return 0;
}
```

Notes on the example:

- `fl_win_content` gives the area inside the decorations, so widgets are placed relative to it.
- `fl_label` with a NULL font uses `ui->f_reg`. The title uses a font made after `fl_app_new`.
- Both buttons call their actions only on the pass in which they were clicked (or Enter was pressed on them).
- The switch updates `h.on` itself. Its return value is 1 only in the pass it changed.
- `fl_app_watch_conf` makes the window follow a theme or accent change made in Settings.
- For a headless picture of this program: `FLUI_PPM=/tmp/hello.ppm ./hello` (in your own scratch directory, not `/tmp`
  for a real session).

## 16. Things that are not in libflui

- **Drag and drop**: not yet. flui and flcomp have none (`src/flpanel/deskicons.c`).
- **Translations**: no message catalogue. The planned translation system is in `docs/PLAN-installer.md` (chapter 28).
- **Image loading**: not in the library (section 7).
- **Scrollable text view or rich text**: not a widget. Apps build what they need from `fl_text_in`, `fl_list` and `fl_scrollbar`.
- **Server-side decorations**: the client draws the title bar when `decorated=1`.
- **Accessibility tree**: none.

Read `src/flui/README` for the design notes, and `flui.h` for the exact signatures. This chapter follows those two files.
