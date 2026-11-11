/* flui.h -- libflui, the FreeLFS desktop toolkit
 * SPDX-License-Identifier: MIT
 *
 * Immediate-mode widgets drawn on the CPU into wl_shm buffers (ARGB8888).
 * A window has a draw callback; every input event runs it once, and the
 * widget calls inside it both draw and report what the user did:
 *
 *	if (fl_button(ui, ID_OK, rect, "Continue", FL_BTN_ACCENT))
 *		go_on();
 *
 * Set FLUI_PPM=FILE to render the first frame to a PPM file with no
 * compositor (FLUI_PPM_DELAY=ms lets timers and fds run first).
 * See README for the whole picture.
 */
#ifndef FLUI_H
#define FLUI_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define FL_API __attribute__((visibility("default")))

struct wl_display;
struct wl_registry;
struct wl_surface;

/* ---- geometry and colour ------------------------------------------------ */

typedef uint32_t fl_color; /* 0xAARRGGBB, straight alpha */
#define FL_HEX(h) (0xff000000u | (uint32_t)(h))

typedef struct { int x, y, w, h; } fl_rect;

static inline fl_rect fl_r(int x, int y, int w, int h) { return (fl_rect){x, y, w, h}; }
static inline int fl_in(fl_rect r, int x, int y)
{
	return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}
static inline fl_rect fl_inset(fl_rect r, int dx, int dy)
{
	return (fl_rect){r.x + dx, r.y + dy, r.w - 2 * dx, r.h - 2 * dy};
}
/* Cut a slice off one side of *r and return it (the rest stays in *r). */
FL_API fl_rect fl_cut_left(fl_rect *r, int w);
FL_API fl_rect fl_cut_right(fl_rect *r, int w);
FL_API fl_rect fl_cut_top(fl_rect *r, int h);
FL_API fl_rect fl_cut_bottom(fl_rect *r, int h);
FL_API fl_rect fl_intersect(fl_rect a, fl_rect b);

/* ---- theme and settings ------------------------------------------------- */

typedef struct {
	fl_color bg, panel, raised, line, text, dim, accent, on_accent, select;
	fl_color strong;  /* headings: a step brighter than text */
	fl_color term_bg; /* terminal background, dark in both themes */
	fl_color err;
	int dark;
} fl_theme;

extern FL_API fl_theme fl_th; /* the live theme; widgets read it */

FL_API void fl_theme_set(int dark);
/* desktop.conf: $FLUI_CONF, else ~/.config/freelfs/desktop.conf */
FL_API const char *fl_conf_path(void);
FL_API void fl_conf_load(void); /* reads the file and applies theme= */
FL_API const char *fl_conf_get(const char *key, const char *def);
FL_API int fl_conf_int(const char *key, int def);
FL_API int fl_conf_set(const char *key, const char *val); /* writes the file; 0 or -1 */
/* n pairs {key, value, key, value, ...} in one write (watchers wake once); a NULL value removes the key */
FL_API int fl_conf_set_many(const char *const *pairs, int n);
FL_API int fl_conf_unset(const char *key);

/* The look that is not a colour, from desktop.conf (fl_conf_load and fl_conf_set refresh it; apps
 * that fl_app_watch_conf follow live). Keys and defaults:
 *   font=IBM Plex Sans   font_size=13 (10..18)   buttons=right|left   titlebar=normal|compact
 *   panel_position=bottom|top   panel_height=44 (36..64)   panel_icon_size=20 (16..28)
 *   animations=on|reduced|off   animation_speed=1.0 (0.1..4)
 * font_size scales every fl_font_ui font (13 is 1.0), the family replaces IBM Plex Sans in all of
 * them; the monospace font is not touched. Corners are always square, there is no setting. */
typedef struct {
	char ui_font[48];
	float font_px;       /* the UI size, 13 is the design size */
	int buttons_left;    /* window buttons at the left of the title bar */
	int title_h;         /* title bar height: 34 normal, 28 compact */
	int panel_h;         /* panel height */
	int panel_top;       /* panel at the top edge */
	int panel_icon;      /* icon size in the panel's window entries */
	int anim;            /* 0 off, 1 reduced (fades only), 2 on */
	float anim_speed;    /* factor on how fast: 2 is twice as fast */
} fl_look;
extern FL_API fl_look fl_lk;

/* Accent colour: accent= in desktop.conf is an id from fl_accents or a hex colour. Apps that
 * use fl_app_watch_conf pick a change up live (the theme is re-applied, windows redraw). */
typedef struct { const char *id, *name; fl_color dark, light; } fl_accent;
#define FL_ACCENT_COUNT 6
extern FL_API const fl_accent fl_accents[FL_ACCENT_COUNT];
FL_API int fl_accent_find(const char *id);      /* index, -1 if not one of the list */
FL_API fl_color fl_accent_current(int dark);    /* the configured accent for a dark or light theme */

typedef struct fl_font fl_font;

/* ---- canvas and drawing --------------------------------------------------- */

typedef struct {
	uint32_t *px; /* NULL: a dry pass, drawing is skipped */
	int w, h, stride; /* stride in pixels */
	fl_rect clip;
} fl_canvas;

FL_API void fl_fill_rect(fl_canvas *cv, fl_rect r, fl_color c);
FL_API void fl_stroke_rect(fl_canvas *cv, fl_rect r, int thick, fl_color c); /* inside r */
FL_API void fl_clip(fl_canvas *cv, fl_rect r); /* intersect the clip */
FL_API fl_color fl_mix(fl_color a, fl_color b, int t255); /* a + (b-a)*t */

typedef struct fl_path fl_path;
FL_API fl_path *fl_path_new(void);
FL_API void fl_path_free(fl_path *p);
FL_API void fl_path_move(fl_path *p, float x, float y);
FL_API void fl_path_line(fl_path *p, float x, float y);
FL_API void fl_path_quad(fl_path *p, float cx, float cy, float x, float y);
FL_API void fl_path_cubic(fl_path *p, float c1x, float c1y, float c2x, float c2y, float x, float y);
FL_API void fl_path_close(fl_path *p);
/* Append SVG path data (M L H V C S Q A Z, absolute and relative), scaled
 * by sx, sy and moved by ox, oy. Returns 0, or -1 on a syntax error. */
FL_API int fl_path_svg(fl_path *p, const char *d, float sx, float sy, float ox, float oy);
FL_API void fl_path_fill(fl_canvas *cv, const fl_path *p, fl_color c);
FL_API void fl_path_stroke(fl_canvas *cv, const fl_path *p, float width, fl_color c);

typedef enum {
	FL_ICON_MENU, FL_ICON_FOLDER, FL_ICON_TERMINAL, FL_ICON_WIFI, FL_ICON_VOLUME,
	FL_ICON_BATTERY, FL_ICON_MINIMIZE, FL_ICON_MAXIMIZE, FL_ICON_RESTORE,
	FL_ICON_CLOSE, FL_ICON_BACK, FL_ICON_FORWARD, FL_ICON_SETTINGS, FL_ICON_WINDOW,
	/* added for the file manager, editor and tray; new icons go before COUNT */
	FL_ICON_FILE, FL_ICON_DOC, FL_ICON_DRIVE, FL_ICON_HOME, FL_ICON_TRASH, FL_ICON_MUTE,
	FL_ICON_ETHERNET, FL_ICON_UP, FL_ICON_PLUS, FL_ICON_FOLDER_LG, FL_ICON_FILE_LG, FL_ICON_DOC_LG,
	FL_ICON_DISPLAY, FL_ICON_MOUSE, FL_ICON_KEYBOARD, FL_ICON_CLOCK,
	FL_ICON_COUNT
} fl_icon;

/* Draw an icon into the square (x, y, size, size). */
FL_API void fl_icon_draw(fl_canvas *cv, fl_icon ic, int x, int y, int size, fl_color c);
/* The battery icon with a fill level, 0-100 (width size*1.25). */
FL_API void fl_icon_battery(fl_canvas *cv, int x, int y, int size, fl_color c, int pct);
/* Icon for a Wayland app_id (flterm -> terminal), FL_ICON_WINDOW if unknown. */
FL_API fl_icon fl_icon_for_app(const char *app_id);

/* ---- text ----------------------------------------------------------------- */

/* family as fontconfig knows it; weight 400, 500, 600, 700; px may be fractional */
FL_API fl_font *fl_font_open(const char *family, int weight, int italic, float px);
FL_API fl_font *fl_font_ui(int weight, float px);   /* the UI font (fl_lk.ui_font, IBM Plex Sans) at px * font_size / 13 */
/* The look changed (conf.c calls this): every UI font takes the new family and scale at once. */
FL_API void fl_text_set_look(const char *family, float scale);
/* 1 when fontconfig has this family (not a substitute), for pickers */
FL_API int fl_font_family_exists(const char *family);
FL_API fl_font *fl_font_mono(int weight, float px); /* IBM Plex Mono */
FL_API void fl_font_close(fl_font *f);
FL_API int fl_font_ascent(const fl_font *f);
FL_API int fl_font_height(const fl_font *f); /* ascent + descent */
FL_API int fl_font_cell(const fl_font *f);   /* advance of "0", for monospace */

/* y is the top of the line box. len < 0: NUL terminated. */
FL_API int fl_text_width(fl_font *f, const char *s, int len);
FL_API int fl_text(fl_canvas *cv, fl_font *f, int x, int y, fl_color c, const char *s, int len);
/* With extra letter spacing in 1/64 px. */
FL_API int fl_text_ex(fl_canvas *cv, fl_font *f, int x, int y, fl_color c, const char *s,
                      int len, int spacing64);
/* Vertically centred in r, left aligned at r.x + pad, clipped to r. */
FL_API void fl_text_in(fl_canvas *cv, fl_font *f, fl_rect r, int align, int pad, fl_color c,
                       const char *s);
/* One code point in a terminal cell at x, y (top left). Returns advance. */
FL_API int fl_glyph(fl_canvas *cv, fl_font *f, int x, int y, fl_color c, uint32_t cp);
/* Byte length of the longest prefix of s that fits in w px, ending with "..." */
FL_API int fl_text_fit(fl_font *f, const char *s, int w, char *out, size_t cap);

enum { FL_LEFT = 0, FL_CENTER = 1, FL_RIGHT = 2 };

/* ---- application and windows ---------------------------------------------- */

typedef struct fl_app fl_app;
typedef struct fl_win fl_win;
typedef struct fl_timer fl_timer;

enum { FL_MOD_SHIFT = 1, FL_MOD_CTRL = 2, FL_MOD_ALT = 4, FL_MOD_SUPER = 8 };
enum { FL_BTN_L = 1, FL_BTN_R = 2, FL_BTN_M = 4 };
enum { FL_CURSOR_ARROW, FL_CURSOR_IBEAM };

typedef struct fl_ui {
	fl_canvas cv;
	fl_win *win;
	int mx, my, mouse_in;
	int btn_held;              /* FL_BTN_* currently down */
	int press, release;        /* FL_BTN_* events in this pass */
	int clicks;                /* click count of this press: 1, 2, ... */
	int wheel;                 /* wheel steps this pass, + is down */
	uint32_t key;              /* keysym pressed (or repeated) in this pass, else 0 */
	int mods;
	char text[16];             /* what the key types, UTF-8 */
	/* widget state */
	uint32_t active, focus;
	int cursor;                /* FL_CURSOR_*, set by widgets, reset each pass */
	fl_font *f_reg, *f_med, *f_semi; /* the window's UI fonts: 400, 500, 600 */
	/* private */
	uint32_t _first_focus, _last_focus, _prev_focus, _edit_id;
	int _focus_next, _focus_prev, _repass, _cur, _sel, _scroll, _focus_seen, _press_taken;
	uint32_t _wheel_taken;
} fl_ui;

typedef struct {
	const char *title, *app_id;
	int w, h;             /* initial size; raw windows: the fixed size */
	int min_w, min_h;
	int decorated;        /* flui draws border, title bar and buttons */
	int raw;              /* a bare wl_surface: the caller assigns its role (fl_shell) */
	int fullscreen;
	int resizable;        /* default 1 for toplevels */
	float font_px;        /* UI font size, default 13 */
	void (*draw)(fl_ui *ui, void *ud);
	void (*close)(fl_win *w, void *ud);   /* default: fl_app_quit */
	/* raw input, optional; return 1 to stop the widgets seeing it */
	int (*key)(fl_win *w, uint32_t keycode, uint32_t sym, const char *utf8, int mods, int pressed,
	           void *ud);
	int (*pointer)(fl_win *w, int type, int x, int y, int button, void *ud);
	void (*resized)(fl_win *w, int w_, int h_, void *ud); /* content size changed */
	void (*focus)(fl_win *w, int focused, void *ud);
	void *ud;
} fl_win_opts;

enum { FL_PTR_MOVE, FL_PTR_DOWN, FL_PTR_UP, FL_PTR_WHEEL /* button = steps */ };

/* global_cb sees every wl_registry global (to bind private protocols). */
typedef void (*fl_global_cb)(void *ud, struct wl_registry *reg, uint32_t name, const char *iface,
                             uint32_t version);
FL_API fl_app *fl_app_new(fl_global_cb global_cb, void *ud); /* NULL: no compositor */
FL_API void fl_app_free(fl_app *a);
FL_API void fl_app_run(fl_app *a);
FL_API void fl_app_quit(fl_app *a);
/* For a program with another main loop (GLib): fl_app_run is prepare, poll,
 * dispatch. Prepare draws dirty windows, readies the display and fills pf with
 * the display fd and then every fl_fd_add fd; it returns how many (-1: error, -2:
 * max was too small because drawing or an event handler added an fd: call it again
 * with a bigger pf). Poll them (or wait on them), then call dispatch with the same pf, n and the
 * poll result r (0 when nothing was ready); -1 means the connection is gone. */
struct pollfd;
FL_API int fl_app_prepare(fl_app *a, struct pollfd *pf, int max);
FL_API int fl_app_dispatch(fl_app *a, const struct pollfd *pf, int n, int r);
FL_API int fl_app_quitting(const fl_app *a);
FL_API struct wl_display *fl_app_display(fl_app *a);
FL_API int fl_app_ppm(fl_app *a); /* 1 when rendering to FLUI_PPM */
/* FL_MOD_* held right now (ui->mods is only as fresh as the last key press). */
FL_API int fl_app_mods(fl_app *a);
FL_API void fl_app_output_size(fl_app *a, int *w, int *h); /* 1280x800 if unknown */
/* Reload desktop.conf when it changes; cb runs after the theme is applied. */
/* Use this xkb layout name ("de", "gb") for this app's keys instead of the compositor's
 * keymap. For flsetup's try-typing field. */
FL_API void fl_app_set_layout(fl_app *a, const char *xkb_layout);
FL_API int fl_app_watch_conf(fl_app *a, void (*cb)(void *ud), void *ud);

FL_API fl_win *fl_win_new(fl_app *a, const fl_win_opts *o);
FL_API void fl_win_destroy(fl_win *w);
FL_API void fl_win_map(fl_win *w);  /* raw windows: start (or resume) drawing */
FL_API void fl_win_hide(fl_win *w); /* raw windows: unmap, keep the surface */
FL_API void fl_win_redraw(fl_win *w);
FL_API void fl_win_redraw_all(fl_app *a);
FL_API void fl_win_set_title(fl_win *w, const char *title);
FL_API void fl_win_resize(fl_win *w, int width, int height); /* raw windows */
FL_API struct wl_surface *fl_win_surface(fl_win *w);
FL_API fl_ui *fl_win_ui(fl_win *w);
FL_API fl_rect fl_win_content(const fl_win *w); /* inside the decorations */
FL_API int fl_win_active(const fl_win *w);
FL_API void fl_win_minimize(fl_win *w);
FL_API void fl_win_maximize(fl_win *w); /* ask for the whole work area (below the panel), not fullscreen */

/* fds and timers; the callback gets the revents of poll(2). */
FL_API int fl_fd_add(fl_app *a, int fd, short events, void (*cb)(void *ud, int fd, short rev),
                     void *ud);
FL_API void fl_fd_remove(fl_app *a, int fd);
FL_API fl_timer *fl_timer_new(fl_app *a, void (*cb)(void *ud), void *ud);
FL_API void fl_timer_arm(fl_timer *t, int first_ms, int every_ms); /* 0, 0 stops it */
FL_API void fl_timer_free(fl_timer *t);

/* Clipboard (wl_data_device). get returns malloc'ed text or NULL. */
FL_API void fl_clipboard_set(fl_app *a, const char *text, size_t len);
FL_API char *fl_clipboard_get(fl_app *a, size_t *len);

/* ---- widgets ---------------------------------------------------------------- */

enum { FL_HOVER = 1, FL_DOWN = 2, FL_CLICK = 4, FL_FOCUS = 8, FL_RCLICK = 16 };
enum { FL_FOCUSABLE = 1, FL_NO_FOCUS_RING = 2 };

/* The primitive every widget is built on: hit test, press, click, focus. */
FL_API int fl_widget(fl_ui *ui, uint32_t id, fl_rect r, int flags);

enum { FL_BTN_PLAIN, FL_BTN_OUTLINE, FL_BTN_ACCENT };
FL_API void fl_label(fl_ui *ui, fl_rect r, const char *s, fl_font *f, fl_color c, int align);
FL_API int fl_button(fl_ui *ui, uint32_t id, fl_rect r, const char *label, int style);
FL_API int fl_icon_button(fl_ui *ui, uint32_t id, fl_rect r, fl_icon ic, int isize, fl_color c);

enum { FL_EDIT_CHANGED = 1, FL_EDIT_ENTER = 2 };
FL_API int fl_textfield(fl_ui *ui, uint32_t id, fl_rect r, char *buf, size_t cap,
                        const char *placeholder, fl_font *f);

FL_API int fl_switch(fl_ui *ui, uint32_t id, fl_rect r, int *on); /* 1 when toggled */
/* Vertical scrollbar in r. *off in px, content and view heights in px. */
FL_API int fl_scrollbar(fl_ui *ui, uint32_t id, fl_rect r, int *off, int content, int view);

typedef struct { int sel, scroll, lx, ly; } fl_list_state;
typedef void (*fl_list_draw)(fl_ui *ui, fl_rect r, int index, int hover, int selected, void *ud);
/* Returns the index activated by click or Enter, else -1. Up/Down/Home/End/Page move. */
FL_API int fl_list(fl_ui *ui, uint32_t id, fl_rect r, int count, int item_h, fl_list_state *st,
                   fl_list_draw draw, void *ud);

typedef struct { int x, y, cols, cell_w, cell_h, gap; } fl_grid;
FL_API fl_rect fl_grid_cell(const fl_grid *g, int i);

/* Window title bar: returns FL_ICON_MINIMIZE/MAXIMIZE/CLOSE when clicked, else -1. */
FL_API int fl_titlebar(fl_ui *ui, fl_rect r, const char *title, int focused, int maximized);

/* ---- keyboard layouts (flsetup, flsettings) ------------------------------- */

typedef struct {
	const char *id;    /* "de", written to keyboard.conf */
	const char *xkb;   /* layout name for xkb ("gb" for uk) */
	const char *name;  /* "QWERTZ" */
	const char *where; /* "German" */
	const char *rows[3];
	int hi;            /* highlighted keys in the top row */
} fl_kbd_layout;

extern FL_API const fl_kbd_layout fl_kbd_layouts[4];
FL_API int fl_kbd_find(const char *id); /* index, -1 if unknown */
/* keyboard.conf path: $FL_KEYBOARD_CONF, else /etc/keyboard.conf */
FL_API const char *fl_kbd_conf_path(void);
FL_API int fl_kbd_conf_read(void);          /* index of LAYOUT=, or -1 */
FL_API int fl_kbd_conf_write(const char *id); /* 0, or -1 with errno */
/* Draw the layout tiles in a row: returns the index clicked or -1. */
FL_API int fl_kbd_tiles(fl_ui *ui, uint32_t id_base, fl_rect r, int gap, int sel);
/* Draw the three-row key preview at x, y. */
FL_API void fl_kbd_preview(fl_ui *ui, int x, int y, int layout);

/* Dead keys: feed keysyms, get composed text. Returns bytes written to out (0: swallowed). */
typedef struct { uint32_t dead; } fl_compose;
FL_API int fl_compose_feed(fl_compose *c, uint32_t sym, const char *utf8, char *out, size_t cap);

/* ---- time zones and the world map (worldmap.c) ----------------------------------- */

/* The zones come from tzdata's zone.tab (zone1970.tab if that is missing) and
 * iso3166.tab in $FL_ZONEINFO, else /usr/share/zoneinfo; only zones that have a
 * file there are listed. Everything loads on first use. FL_NOW=EPOCH makes
 * fl_tz_time() return a fixed time (tests, screenshots). */
typedef struct {
	char name[44];    /* "Europe/Berlin", what /etc/timezone holds */
	char city[32];    /* "Berlin" */
	char country[40]; /* "Germany" */
	float lat, lon;   /* degrees; lat > 90: no place on the map ("UTC") */
	int off_now;      /* UTC offset in seconds right now, refreshed by fl_tz_refresh() */
} fl_tzone;

FL_API int fl_tz_count(void);
FL_API const fl_tzone *fl_tz_get(int i);
FL_API int fl_tz_find(const char *name);              /* index or -1 */
FL_API int fl_tz_nearest(float lat, float lon);       /* index of the closest place on the map */
/* Zones matching what was typed (city, country or zone name, any case): fills
 * out[] best first, returns how many. */
FL_API int fl_tz_search(const char *q, int *out, int max);
FL_API int fl_tz_system(void);                        /* zone in force (/etc/timezone, else /etc/localtime), or -1; $FL_TZ_ETC replaces /etc */
FL_API const char *fl_tz_dir(void);
FL_API time_t fl_tz_time(void);
FL_API void fl_tz_refresh(void);                      /* off_now of every zone for the current time */
/* Local time in a zone: tm, UTC offset in seconds and abbreviation (may be NULL). */
FL_API int fl_tz_local(int idx, time_t t, struct tm *tm, int *off, char *abbr, size_t cap);
/* Epoch seconds of a wall-clock time (year, month, ... in tm) in the zone; 0 on failure. */
FL_API time_t fl_tz_mktime(int idx, struct tm *tm);
/* "UTC+2", "UTC+5:30", "UTC-3", "UTC" */
FL_API void fl_tz_fmt_offset(int off_s, char *out, size_t cap);
/* "14:32" or, with h24 == 0, "2:32 PM" */
FL_API void fl_tz_fmt_time(const struct tm *tm, int h24, char *out, size_t cap);

/* A flat world map (Natural Earth projection, land and borders from Natural
 * Earth's public domain 1:110m data, FreeLFS colours) with a dot for every zone.
 * Clicking picks the zone whose place is nearest (the map shows each zone's
 * region by nearest place); regions that have the same time as sel are tinted. */
typedef struct fl_worldmap fl_worldmap;
FL_API fl_worldmap *fl_worldmap_new(void);
FL_API void fl_worldmap_free(fl_worldmap *m);
FL_API void fl_worldmap_set_24h(fl_worldmap *m, int h24);
/* Height that fits width w. */
FL_API int fl_worldmap_height(int w);
/* Draws the map in r (the picture is centred and keeps its proportions).
 * sel: selected zone or -1. *hover gets the zone nearest to the pointer, or -1.
 * Returns the zone clicked, else -1. */
FL_API int fl_worldmap_draw(fl_ui *ui, uint32_t id, fl_rect r, fl_worldmap *m, int sel, int *hover);

/* Search field with a list of suggestions that opens over whatever is below.
 * Zero-initialise the state. Call _field first, draw the widgets that lie below
 * the list (skip the pointer for them while fl_tzsearch_covers), then _popup. */
typedef struct {
	int n, row, res[8];
	char last[64];
	fl_rect field;
	int shown;
} fl_tzsearch;
FL_API int fl_tzsearch_field(fl_ui *ui, uint32_t id, fl_rect r, fl_tzsearch *s, char *buf, size_t cap);
FL_API int fl_tzsearch_popup(fl_ui *ui, uint32_t id_base, fl_tzsearch *s, char *buf);
FL_API int fl_tzsearch_covers(const fl_tzsearch *s, int x, int y);

#endif
