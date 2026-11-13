/* The compositor's own small list popup, drawn like the Alt-Tab switcher (flat panel colours, the lock
 * screen's built-in font), for two things that need no client:
 *   - the window menu: right click on the title bar of a FreeLFS program's window (Minimize, Maximize,
 *     Always on top, Move to workspace N, Close);
 *   - the clipboard history: hold Ctrl+V (seat.c), pick an entry with the arrow keys and Enter or a click.
 * It also draws the outline that shows where a window dragged to a screen edge will land.
 * While a popup is open it takes the keyboard and the pointer; Escape or a click outside closes it.
 * SPDX-License-Identifier: MIT */
#include "comp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define ROW_H 30
#define PAD_X 14
#define MAX_ITEMS 32

struct item {
	char label[96];
	int id;
	bool disabled, check;
};

static struct {
	bool on;
	struct item it[MAX_ITEMS];
	int n, sel, kind;		/* kind: 1 window menu, 2 clipboard history */
	struct rect r;
	uint32_t wid;
} PM;

static struct rect last_r;		/* drawn last, for damage */

static void fill(struct rect clip, struct rect r, uint32_t col)
{
	struct rect o;
	if (!rect_intersect(r, clip, &o))
		return;
	for (int j = o.y1; j < o.y2; j++) {
		uint32_t *d = C.shadow + (size_t)j * C.w + o.x1;
		for (int i = o.x1; i < o.x2; i++)
			*d++ = col;
	}
}

static void frame(struct rect clip, struct rect r, int t, uint32_t col)
{
	fill(clip, (struct rect){ r.x1, r.y1, r.x2, r.y1 + t }, col);
	fill(clip, (struct rect){ r.x1, r.y2 - t, r.x2, r.y2 }, col);
	fill(clip, (struct rect){ r.x1, r.y1 + t, r.x1 + t, r.y2 - t }, col);
	fill(clip, (struct rect){ r.x2 - t, r.y1 + t, r.x2, r.y2 - t }, col);
}

/* ---- snap preview -------------------------------------------------------------------- */

static int snap_kind;		/* 0 none, -1 left half, 1 right half, 2 whole work area */
static struct rect snap_r;

static struct rect snap_rect_for(int kind)
{
	struct rect a = work_area();
	int hw = rect_w(a) / 2;
	if (kind < 0)
		return (struct rect){ a.x1, a.y1, a.x1 + hw, a.y2 };
	if (kind == 1)
		return (struct rect){ a.x1 + hw, a.y1, a.x2, a.y2 };
	return a;
}

int snap_preview_kind(void)
{
	return snap_kind;
}

void snap_preview_set(int kind)
{
	if (kind == snap_kind)
		return;
	if (snap_kind)
		damage_add(snap_r);
	snap_kind = kind;
	if (kind) {
		snap_r = snap_rect_for(kind);
		damage_add(snap_r);
	}
}

void snap_preview_draw(struct rect clip)
{
	if (snap_kind)
		frame(clip, (struct rect){ snap_r.x1 + 6, snap_r.y1 + 6, snap_r.x2 - 6, snap_r.y2 - 6 }, 3, theme_accent());
}

/* ---- the popup ------------------------------------------------------------------------ */

static void layout(int x, int y, int min_w)
{
	int w = min_w;
	for (int i = 0; i < PM.n; i++) {
		int tw = ui_text_width(PM.it[i].label) + 2 * PAD_X + (PM.it[i].check ? 18 : 0);
		if (tw > w)
			w = tw;
	}
	if (w > C.w - 16)
		w = C.w - 16;
	int h = PM.n * ROW_H + 2;
	if (x + w > C.w)
		x = C.w - w;
	if (y + h > C.h)
		y = C.h - h;
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	PM.r = (struct rect){ x, y, x + w, y + h };
}

static void shown(void)
{
	if (!rect_empty(last_r))
		damage_add(last_r);
	last_r = PM.on ? PM.r : (struct rect){ 0, 0, 0, 0 };
	if (PM.on)
		damage_add(PM.r);
}

bool pmenu_active(void)
{
	return PM.on;
}

void pmenu_close(void)
{
	if (!PM.on)
		return;
	PM.on = false;
	shown();
	seat_refocus();
}

static void add(const char *label, int id, bool disabled, bool check)
{
	if (PM.n >= MAX_ITEMS)
		return;
	struct item *it = &PM.it[PM.n++];
	snprintf(it->label, sizeof it->label, "%s", label);
	it->id = id;
	it->disabled = disabled;
	it->check = check;
}

/* the window menu's ids */
enum { M_MIN = 1, M_MAX, M_TOP, M_CLOSE, M_WS = 100 };

static void pick_window(int id)
{
	struct window *w = window_by_id(PM.wid);
	if (!w || !w->mapped)
		return;
	switch (id) {
	case M_MIN: window_minimize(w); break;
	case M_MAX: window_action_on(w, WA_TOGGLE_MAXIMIZE); window_focus(w); break;
	case M_TOP: window_set_above(w, !w->above); break;
	case M_CLOSE: window_close(w); break;
	default:
		if (id >= M_WS)
			ws_move_window(w, id - M_WS, false);
	}
}

static void pm_pick(int i)
{
	if (i < 0 || i >= PM.n || PM.it[i].disabled)
		return;
	int kind = PM.kind, id = PM.it[i].id;
	pmenu_close();
	if (kind == 1)
		pick_window(id);
	else if (kind == 2)
		cliphist_paste(id);
}

void window_menu_open(struct window *w, int x, int y)
{
	PM.n = 0;
	PM.kind = 1;
	PM.wid = w->id;
	add("Minimize", M_MIN, false, false);
	add(w->maximized ? "Restore" : "Maximize", M_MAX, w->fullscreen, false);
	add("Always on top", M_TOP, false, w->above);
	int n = C.ws_cols * C.ws_rows;
	for (int i = 0; i < n && i < 9; i++) {
		if (i == w->ws)
			continue;
		char b[40];
		snprintf(b, sizeof b, "Move to workspace %d", i + 1);
		add(b, M_WS + i, false, false);
	}
	add("Close", M_CLOSE, false, false);
	PM.sel = -1;
	layout(x, y, 200);
	PM.on = true;
	shown();
	seat_set_ptr_focus(NULL, 0, 0);
}

void cliphist_menu_open(void)
{
	int n = cliphist_count();
	if (n <= 0 || PM.on)
		return;
	PM.n = 0;
	PM.kind = 2;
	for (int i = 0; i < n; i++) {
		char b[96];
		const char *t = cliphist_get(i);
		size_t k = 0;
		while (*t && k < 58) {
			unsigned char c = (unsigned char)*t++;
			b[k++] = c < 32 ? ' ' : c > 126 ? '?' : (char)c;
		}
		if (*t)
			strcpy(b + k, "...");
		else
			b[k] = 0;
		add(b, i, false, false);
	}
	PM.sel = 0;
	struct rect a = work_area();
	layout(a.x1 + (rect_w(a) - 420) / 2, a.y1 + rect_h(a) / 4, 420);
	PM.on = true;
	shown();
	seat_set_ptr_focus(NULL, 0, 0);
}

static int row_at(void)
{
	int x = (int)C.px, y = (int)C.py;
	if (x < PM.r.x1 || x >= PM.r.x2 || y < PM.r.y1 + 1 || y >= PM.r.y2 - 1)
		return -1;
	return (y - PM.r.y1 - 1) / ROW_H;
}

void pmenu_motion(void)
{
	int r = row_at();
	if (r >= 0 && r != PM.sel && !PM.it[r].disabled) {
		PM.sel = r;
		damage_add(PM.r);
	}
}

bool pmenu_button(uint32_t button, bool pressed)
{
	if (!PM.on)
		return false;
	if (!pressed)
		return true;
	int r = row_at();
	if (r < 0) {
		pmenu_close();
		return true;
	}
	pm_pick(r);
	return true;
}

/* true: the key is the popup's (modifier keys are left to the normal path) */
bool pmenu_key(uint32_t sym, bool pressed)
{
	if (!PM.on)
		return false;
	if ((sym >= XKB_KEY_Shift_L && sym <= XKB_KEY_Hyper_R) || sym == XKB_KEY_ISO_Level3_Shift)
		return false;
	if (!pressed)
		return true;
	switch (sym) {
	case XKB_KEY_Escape:
		pmenu_close();
		break;
	case XKB_KEY_Up:
	case XKB_KEY_Down: {
		int d = sym == XKB_KEY_Up ? -1 : 1, i = PM.sel;
		for (int k = 0; k < PM.n; k++) {
			i = ((i < 0 ? (d > 0 ? -1 : 0) : i) + d + PM.n) % PM.n;
			if (!PM.it[i].disabled)
				break;
		}
		PM.sel = i;
		damage_add(PM.r);
		break;
	}
	case XKB_KEY_Home:
		PM.sel = 0;
		damage_add(PM.r);
		break;
	case XKB_KEY_End:
		PM.sel = PM.n - 1;
		damage_add(PM.r);
		break;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		pm_pick(PM.sel);
		break;
	}
	return true;
}

void pmenu_draw(struct rect clip)
{
	if (!PM.on)
		return;
	struct rect o;
	if (!rect_intersect(PM.r, clip, &o))
		return;
	fill(o, PM.r, P(line));
	fill(o, (struct rect){ PM.r.x1 + 1, PM.r.y1 + 1, PM.r.x2 - 1, PM.r.y2 - 1 }, P(panel));
	for (int i = 0; i < PM.n; i++) {
		struct rect row = { PM.r.x1 + 1, PM.r.y1 + 1 + i * ROW_H, PM.r.x2 - 1, PM.r.y1 + 1 + (i + 1) * ROW_H };
		if (i == PM.sel) {
			fill(o, row, P(raised));
			fill(o, (struct rect){ row.x1, row.y1, row.x1 + 3, row.y2 }, theme_accent());
		}
		int x = row.x1 + PAD_X;
		if (PM.it[i].check) {
			ui_text(o, x, row.y1 + 2, "x", theme_accent());
			x += 18;
		}
		ui_text(o, x, row.y1 + 2, PM.it[i].label, PM.it[i].disabled ? P(dim) : P(text));
	}
}

/* ---- the title bar: right click opens the window menu, middle click closes ---------------- */

/* Only for programs of ours (their title bar is the one flui draws; a browser or a GTK program has its own
 * right and middle clicks there): the program's /proc/PID/exe starts with "fl" or is one of a few. */
static bool ours(struct window *w)
{
	if (w->deco_ours)
		return w->deco_ours == 1;
	w->deco_ours = 2;
	if (!w->surf || !w->surf->res)
		return false;
	pid_t pid = 0;
	wl_client_get_credentials(wl_resource_get_client(w->surf->res), &pid, NULL, NULL);
	char link[40], exe[256];
	snprintf(link, sizeof link, "/proc/%d/exe", (int)pid);
	ssize_t n = readlink(link, exe, sizeof exe - 1);
	if (n <= 0)
		return false;
	exe[n] = 0;
	const char *b = strrchr(exe, '/');
	b = b ? b + 1 : exe;
	if (!strncmp(b, "fl", 2) || !strcmp(b, "freebws") || !strcmp(b, "snappel") || !strcmp(b, "general-manager") ||
	    !strncmp(b, "ld-linux", 8))	/* started through the loader (tests) */
		w->deco_ours = 1;
	return w->deco_ours == 1;
}

bool title_click(struct window *w, uint32_t button, int x, int y)
{
	if (!w || w->xw || w->fullscreen || !w->mapped || w->minimized)
		return false;
	struct rect r = window_rect(w);
	if (x < r.x1 || x >= r.x2 || y < r.y1 || y > r.y1 + CF.title_h)
		return false;
	if (!ours(w))
		return false;
	if (button == 274) {		/* BTN_MIDDLE */
		window_close(w);
		return true;
	}
	if (button == 273) {		/* BTN_RIGHT */
		window_focus(w);
		window_menu_open(w, x, y);
		return true;
	}
	return false;
}
