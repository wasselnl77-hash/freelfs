/* xdg_shell and window management: stacking, focus, move, resize, maximize,
 * minimize, popups, Alt-Tab, the panel and menu roles of fl_shell.
 * SPDX-License-Identifier: MIT */
#include "comp.h"
#include "xdg-shell-protocol.h"

#include <stdlib.h>
#include <string.h>

#define EDGE_TOP 1
#define EDGE_BOTTOM 2
#define EDGE_LEFT 4
#define EDGE_RIGHT 8

/* ---- area and configure ----------------------------------------------- */

struct rect work_area(void)
{
	struct rect r = { 0, 0, C.w, C.h - C.panel_h };
	if (CF.panel_top) {	/* panel_position=top */
		r.y1 = C.panel_h;
		r.y2 = C.h;
	}
	return r;
}

static int geom_w(struct window *w)
{
	return w->xs && w->xs->geom.x2 > w->xs->geom.x1 ? rect_w(w->xs->geom) : (w->surf ? w->surf->w : 0);
}

static int geom_h(struct window *w)
{
	return w->xs && w->xs->geom.y2 > w->xs->geom.y1 ? rect_h(w->xs->geom) : (w->surf ? w->surf->h : 0);
}

/* the window geometry on the desktop */
struct rect window_rect(struct window *w)
{
	return (struct rect){ w->x, w->y, w->x + geom_w(w), w->y + geom_h(w) };
}

static void window_configure(struct window *w, int cw, int ch)
{
	w->cfg_w = cw;
	w->cfg_h = ch;
	if (w->xw) {		/* an X11 window: xwm.c asks the X server for the size */
		xwm_window_configure(w, cw, ch);
		return;
	}
	if (!w->xs || !w->res || !w->xs->configured)
		return;
	struct wl_array st;
	wl_array_init(&st);
	uint32_t *p;
#define STATE(v) do { p = wl_array_add(&st, sizeof(*p)); if (p) *p = (v); } while (0)
	if (w->maximized)
		STATE(XDG_TOPLEVEL_STATE_MAXIMIZED);
	if (w->fullscreen)
		STATE(XDG_TOPLEVEL_STATE_FULLSCREEN);
	if (w->resizing)
		STATE(XDG_TOPLEVEL_STATE_RESIZING);
	if (C.focused == w)
		STATE(XDG_TOPLEVEL_STATE_ACTIVATED);
#undef STATE
	xdg_toplevel_send_configure(w->res, cw, ch, &st);
	w->xs->serial = wl_display_next_serial(C.dpy);
	xdg_surface_send_configure(w->xs->res, w->xs->serial);
	wl_array_release(&st);
}

/* ---- stacking and focus ----------------------------------------------- */

struct window *window_by_id(uint32_t id)
{
	struct window *w;
	wl_list_for_each(w, &C.windows, link)
		if (w->id == id)
			return w;
	return NULL;
}

void window_raise(struct window *w)
{
	/* always-on-top windows stay above the others: a normal window goes in below the last of them */
	struct wl_list *at = &C.windows;
	if (!w->above) {
		struct window *o;
		wl_list_for_each(o, &C.windows, link) {
			if (o == w)
				continue;
			if (!o->above)
				break;
			at = &o->link;
		}
	}
	if (at->next == &w->link)
		return;
	wl_list_remove(&w->link);
	wl_list_insert(at, &w->link);
	if (w->mapped)
		damage_tree(w->surf);
}

void update_kb_focus(void)
{
	struct surface *want = NULL;
	if (C.locked)
		want = C.lock_surf && C.lock_surf->px ? C.lock_surf : NULL;
	else if (flyout_visible())
		want = C.flyout;
	else if (menu_visible())
		want = C.menu;
	else if (C.focused && C.focused->mapped && !C.focused->minimized)
		want = C.focused->surf;
	seat_set_kb_focus(want);
	xwm_focus_sync();	/* the X server's input focus follows */
}

void window_focus(struct window *w)
{
	if (w && (!w->mapped || w->minimized || w->ws != C.ws_cur))
		return;
	struct window *old = C.focused;
	if (w)
		window_raise(w);
	if (old != w) {
		C.focused = w;
		if (old) {
			window_configure(old, old->cfg_w, old->cfg_h);
			if (old->announced)
				fl_emit_changed(old);
		}
		if (w) {
			window_configure(w, w->cfg_w, w->cfg_h);
			if (w->announced)
				fl_emit_changed(w);
		}
	}
	update_kb_focus();
}

static struct window *top_window(struct window *except)
{
	struct window *w;
	wl_list_for_each(w, &C.windows, link)
		if (w != except && window_visible(w))
			return w;
	return NULL;
}

void window_minimize(struct window *w)
{
	if (w->minimized || !w->mapped)
		return;
	w->minimized = true;
	damage_tree(w->surf);
	anim_window_minimize(w);	/* shrinks towards its panel entry while it is hidden */
	xwm_window_changed(w);
	if (C.focused == w) {
		C.focused = NULL;
		window_configure(w, w->cfg_w, w->cfg_h);
		if (w->announced)
			fl_emit_changed(w);
		window_focus(top_window(w));
	} else if (w->announced) {
		fl_emit_changed(w);
	}
	update_kb_focus();
	seat_refocus();
}

void window_activate(struct window *w)
{
	if (!w->mapped)
		return;
	if (w->ws != C.ws_cur)		/* from the panel: go where it is */
		ws_switch(w->ws);
	if (w->minimized) {
		w->minimized = false;
		damage_tree(w->surf);
		anim_window_restore(w);
		if (w->announced)
			fl_emit_changed(w);
		schedule_repaint();
	}
	window_focus(w);
	seat_refocus();
}

void window_close(struct window *w)
{
	if (w->xw) {
		xwm_window_close(w);
		return;
	}
	if (w->res)
		xdg_toplevel_send_close(w->res);
}

/* ---- maximize and fullscreen ------------------------------------------ */

static void window_refit(struct window *w)
{
	struct rect a = w->fullscreen ? (struct rect){ 0, 0, C.w, C.h } : work_area();
	struct xinset in = xwm_insets(w);	/* the frame flcomp draws around an X window */
	a.x1 += in.l;
	a.y1 += in.t;
	a.x2 -= in.r;
	a.y2 -= in.b;
	if (w->mapped)
		damage_tree(w->surf);
	w->x = a.x1;
	w->y = a.y1;
	window_configure(w, rect_w(a), rect_h(a));
	if (w->mapped)
		damage_tree(w->surf);
}

static void window_set_state(struct window *w, bool max, bool full)
{
	struct rect before = window_rect(w);	/* for the animation of the change */
	bool was = w->maximized || w->fullscreen, will = max || full;
	if (!was && will && w->mapped && !w->snap) {	/* a snapped window already holds its old place */
		w->restore = (struct rect){ w->x, w->y, w->x + geom_w(w), w->y + geom_h(w) };
		w->have_restore = true;
	}
	if (will)
		w->snap = 0;
	w->maximized = max;
	w->fullscreen = full;
	if (will) {
		window_refit(w);
	} else if (was && w->have_restore) {
		if (w->mapped)
			damage_tree(w->surf);
		w->x = w->restore.x1;
		w->y = w->restore.y1;
		window_configure(w, rect_w(w->restore), rect_h(w->restore));
		if (w->mapped)
			damage_tree(w->surf);
	} else {
		window_configure(w, 0, 0);
	}
	if (w->announced)
		fl_emit_changed(w);
	xwm_window_changed(w);
	if (w->mapped)
		anim_window_geometry(w, before);
}

/* Super+Left / Super+Right: one half of the work area. The place from before stays in restore. */
static void window_snap_to(struct window *w, int side)
{
	struct rect a = work_area();
	int hw = rect_w(a) / 2, nw = side < 0 ? hw : rect_w(a) - hw;
	struct xinset in = xwm_insets(w);
	struct rect before = window_rect(w);
	if (w->mapped)
		damage_tree(w->surf);
	w->x = (side < 0 ? a.x1 : a.x1 + hw) + in.l;
	w->y = a.y1 + in.t;
	w->snap = side;
	window_configure(w, nw - in.l - in.r, rect_h(a) - in.t - in.b);
	if (w->mapped) {
		damage_tree(w->surf);
		anim_window_geometry(w, before);
	}
}

/* back to where the window was before it was snapped */
static void window_unsnap(struct window *w)
{
	if (!w->snap)
		return;
	w->snap = 0;
	if (!w->have_restore)
		return;
	struct rect before = window_rect(w);
	if (w->mapped)
		damage_tree(w->surf);
	w->x = w->restore.x1;
	w->y = w->restore.y1;
	window_configure(w, rect_w(w->restore), rect_h(w->restore));
	if (w->mapped) {
		damage_tree(w->surf);
		anim_window_geometry(w, before);
	}
}

/* the keyboard shortcuts for the focused window (seat.c) */
void window_set_above(struct window *w, bool on)
{
	w->above = on;
	window_raise(w);
	if (w->mapped)
		damage_tree(w->surf);
}

void window_action(int act)
{
	window_action_on(C.focused, act);
}

int window_geom_h_pub(struct window *w)
{
	return geom_h(w);
}

void window_action_on(struct window *w, int act)
{
	if (act == WA_TOGGLE_MAXIMIZE)
		act = w && w->maximized ? WA_RESTORE_OR_MINIMIZE : WA_MAXIMIZE;
	if (!w || !w->mapped || w->minimized || w->fullscreen)
		return;
	switch (act) {
	case WA_SNAP_LEFT:
	case WA_SNAP_RIGHT: {
		int side = act == WA_SNAP_LEFT ? -1 : 1;
		if (w->snap == -side) {	/* from the other half: back to the old place */
			window_unsnap(w);
			return;
		}
		if (w->snap == side)
			return;
		if (w->maximized)	/* the old place is still in restore */
			w->maximized = false;
		else if (!w->snap) {
			w->restore = (struct rect){ w->x, w->y, w->x + geom_w(w), w->y + geom_h(w) };
			w->have_restore = true;
		}
		window_snap_to(w, side);
		break;
	}
	case WA_MAXIMIZE:
		if (!w->maximized)
			window_set_state(w, true, false);
		break;
	case WA_RESTORE_OR_MINIMIZE:
		if (w->maximized)
			window_set_state(w, false, false);
		else if (w->snap)
			window_unsnap(w);
		else
			window_minimize(w);
		break;
	}
}

/* the output got smaller: windows that are now off screen come back */
void windows_fit_output(void)
{
	struct window *w;
	wl_list_for_each(w, &C.windows, link) {
		if (!w->mapped || w->maximized || w->fullscreen || w->snap)
			continue;
		int gw = geom_w(w), gh = geom_h(w), nx = w->x, ny = w->y;
		if (nx + gw > C.w)
			nx = C.w - gw;
		struct rect wa = work_area();
		if (ny + gh > wa.y2)
			ny = wa.y2 - gh;
		struct xinset in = xwm_insets(w);
		if (nx < 0)
			nx = 0;
		if (ny < wa.y1 + in.t)
			ny = wa.y1 + in.t;
		if (nx != w->x || ny != w->y) {
			w->x = nx;
			w->y = ny;
		}
	}
}

void work_area_changed(void)
{
	windows_fit_output();	/* a panel that moved or grew: windows that now lie under it come out */
	struct window *w;
	wl_list_for_each(w, &C.windows, link) {
		if (w->maximized || w->fullscreen)
			window_refit(w);
		else if (w->snap && w->mapped)
			window_snap_to(w, w->snap);
	}
}

/* ---- workspaces ---------------------------------------------------------- */

bool window_visible(struct window *w)
{
	return w->mapped && !w->minimized && w->ws == C.ws_cur;
}

bool window_on_ws(struct window *w, int idx)
{
	return w->mapped && !w->minimized && w->ws == idx;
}

int ws_count(void)
{
	return C.ws_cols * C.ws_rows;
}

/* Another workspace comes into view: what was being done to a window of the old one ends, the
 * newest window of the new one gets the focus (the stacking order is the order of last use). */
void ws_switch(int idx)
{
	if (idx < 0 || idx >= ws_count() || idx == C.ws_cur)
		return;
	if (grab_active())
		grab_end();
	if (cycling())
		cycle_end();
	popups_dismiss();
	struct window *old = C.focused;
	int from_ws = C.ws_cur;
	C.ws_cur = idx;
	anim_ws_switch(from_ws, idx);	/* the windows of both slide; they are drawn by anim.c until it ends */
	if (old && old->ws != idx) {
		C.focused = NULL;
		window_configure(old, old->cfg_w, old->cfg_h);
		if (old->announced)
			fl_emit_changed(old);
	}
	window_focus(top_window(NULL));
	damage_all();
	overview_changed();
	update_kb_focus();
	seat_refocus();
	fl_emit_workspaces();
}

void ws_step(int dx, int dy)
{
	int x = C.ws_cur % C.ws_cols + dx, y = C.ws_cur / C.ws_cols + dy;
	if (x < 0 || y < 0 || x >= C.ws_cols || y >= C.ws_rows)
		return;
	ws_switch(y * C.ws_cols + x);
}

/* the window and the dialogs that belong to it */
static void ws_put(struct window *w, int idx)
{
	struct window *x;
	wl_list_for_each(x, &C.windows, link)
		if (x == w || x->parent == w) {
			if (x->mapped)
				damage_tree(x->surf);
			x->ws = idx;
			if (x->announced)
				fl_emit_window_ws(x);
		}
}

void ws_move_window(struct window *w, int idx, bool follow)
{
	if (idx < 0 || idx >= ws_count() || w->ws == idx)
		return;
	struct window *p = w;
	while (p->parent)
		p = p->parent;		/* a dialog goes with the window it belongs to */
	ws_put(p, idx);
	if (follow) {
		ws_switch(idx);
		if (p->mapped && !p->minimized)
			window_focus(p);
	} else if (C.focused && C.focused->ws != C.ws_cur) {	/* it took the focus with it */
		struct window *f = C.focused;
		C.focused = NULL;
		window_configure(f, f->cfg_w, f->cfg_h);
		if (f->announced)
			fl_emit_changed(f);
		window_focus(top_window(NULL));
	}
	overview_changed();
	update_kb_focus();
	seat_refocus();
	damage_all();
}

void ws_move_focused(int dx, int dy)
{
	struct window *w = C.focused;
	if (!w || !w->mapped || w->minimized)
		return;
	int x = w->ws % C.ws_cols + dx, y = w->ws / C.ws_cols + dy;
	if (x < 0 || y < 0 || x >= C.ws_cols || y >= C.ws_rows)
		return;
	ws_move_window(w, y * C.ws_cols + x, true);
}

/* desktop.conf: the grid. Windows on workspaces that are gone move to the last one. */
void ws_configure(int cols, int rows)
{
	if (cols < 1 || rows < 1)
		return;
	bool first = C.ws_cols == 0;
	if (!first && cols == C.ws_cols && rows == C.ws_rows)
		return;
	overview_close();
	C.ws_cols = cols;
	C.ws_rows = rows;
	int n = cols * rows;
	struct window *w;
	wl_list_for_each(w, &C.windows, link)
		if (w->ws >= n) {
			w->ws = n - 1;
			if (w->announced)
				fl_emit_window_ws(w);
		}
	if (C.ws_cur >= n) {
		C.ws_cur = n - 1;
		window_focus(top_window(NULL));
		update_kb_focus();
	}
	damage_all();
	seat_refocus();
	fl_emit_workspaces();
}

/* ---- move and resize grabs -------------------------------------------- */

static struct {
	int mode;			/* 0 none, 1 move, 2 resize */
	struct window *w;
	double sx, sy;
	int wx, wy, ww, wh;
	unsigned edges;
} G;

bool grab_active(void)
{
	return G.mode != 0;
}

static void grab_start_move(struct window *w)
{
	if (!C.btn_down || w->fullscreen || G.mode)
		return;
	if (w->maximized || w->snap) {
		int gw = geom_w(w) > 0 ? geom_w(w) : 1;
		double ratio = (C.px - w->x) / gw;
		int rw = w->have_restore ? rect_w(w->restore) : gw;
		if (w->maximized)
			window_set_state(w, false, false);
		else
			window_unsnap(w);
		struct xinset in = xwm_insets(w);
		w->x = (int)(C.px - ratio * rw);
		w->y = (int)C.py - 14 + in.t;
		if (w->y < in.t)
			w->y = in.t;
	}
	G.mode = 1;
	G.w = w;
	G.sx = C.px;
	G.sy = C.py;
	G.wx = w->x;
	G.wy = w->y;
	seat_set_ptr_focus(NULL, 0, 0);
}

static void grab_start_resize(struct window *w, unsigned edges)
{
	if (!C.btn_down || w->fullscreen || w->maximized || !edges || G.mode)
		return;
	G.mode = 2;
	G.w = w;
	w->snap = 0;		/* a hand-sized window is no longer a half */
	G.sx = C.px;
	G.sy = C.py;
	G.ww = geom_w(w);
	G.wh = geom_h(w);
	G.edges = edges;
	w->edges = edges;
	w->anchor_r = w->x + G.ww;
	w->anchor_b = w->y + G.wh;
	w->resizing = true;
	window_configure(w, G.ww, G.wh);
	seat_set_ptr_focus(NULL, 0, 0);
}

void grab_motion(void)
{
	struct window *w = G.w;
	if (!w)
		return;
	int dx = (int)(C.px - G.sx), dy = (int)(C.py - G.sy);
	if (G.mode == 1) {
		/* at the left or right screen edge: half the screen; at the top edge: maximize (the outline shows it) */
		snap_preview_set(w->fullscreen ? 0 : C.px < 4 ? -1 : C.px >= C.w - 4 ? 1 : C.py < 4 ? 2 : 0);
		int nx = G.wx + dx, ny = G.wy + dy;
		int top = xwm_insets(w).t;	/* the title bar of an X window must stay on screen */
		if (ny < top)
			ny = top;
		if (nx == w->x && ny == w->y)
			return;
		damage_tree(w->surf);
		w->x = nx;
		w->y = ny;
		damage_tree(w->surf);
	} else if (G.mode == 2) {
		int nw = G.ww, nh = G.wh;
		if (G.edges & EDGE_RIGHT)
			nw += dx;
		if (G.edges & EDGE_LEFT)
			nw -= dx;
		if (G.edges & EDGE_BOTTOM)
			nh += dy;
		if (G.edges & EDGE_TOP)
			nh -= dy;
		int minw = w->min_w > 0 ? w->min_w : 120, minh = w->min_h > 0 ? w->min_h : 80;
		if (nw < minw)
			nw = minw;
		if (nh < minh)
			nh = minh;
		if (w->max_w > 0 && nw > w->max_w)
			nw = w->max_w;
		if (w->max_h > 0 && nh > w->max_h)
			nh = w->max_h;
		if (nw != w->cfg_w || nh != w->cfg_h)
			window_configure(w, nw, nh);
	}
}

void grab_end(void)
{
	struct window *w = G.w;
	int snapk = snap_preview_kind();
	snap_preview_set(0);
	if (G.mode == 1 && w && snapk) {
		G.mode = 0;
		G.w = NULL;
		window_action_on(w, snapk < 0 ? WA_SNAP_LEFT : snapk == 1 ? WA_SNAP_RIGHT : WA_MAXIMIZE);
		return;
	}
	if (G.mode == 2 && w) {
		w->resizing = false;
		window_configure(w, w->cfg_w, w->cfg_h);
		w->release_serial = w->xs ? w->xs->serial : 0;
	}
	G.mode = 0;
	G.w = NULL;
}

/* ---- Alt-Tab ----------------------------------------------------------- */

static struct window *cyc[64];
static int cyc_n, cyc_i;
static bool cyc_on;

bool cycling(void)
{
	return cyc_on;
}

struct window **cycle_windows(int *n, int *sel)
{
	*n = cyc_n;
	*sel = cyc_i;
	return cyc;
}

/* d = 1: the next window down the stack, d = -1: the one before (Alt-Shift-Tab) */
static void cycle_step(int d)
{
	if (!cyc_on) {
		cyc_n = 0;
		struct window *w;
		wl_list_for_each(w, &C.windows, link)
			if (window_visible(w) && cyc_n < 64)
				cyc[cyc_n++] = w;
		if (cyc_n < 2)
			return;
		cyc_on = true;
		cyc_i = 0;
	}
	for (int tries = 0; tries < cyc_n; tries++) {
		cyc_i = (cyc_i + d + cyc_n) % cyc_n;
		if (cyc[cyc_i]) {
			window_focus(cyc[cyc_i]);
			break;
		}
	}
	switcher_changed();
}

void cycle_next(void) { cycle_step(1); }
void cycle_prev(void) { cycle_step(-1); }

/* Selected window on top, the others keep their old relative order. */
void cycle_end(void)
{
	if (!cyc_on)
		return;
	cyc_on = false;
	switcher_changed();	/* it is gone from here on */
	struct window *sel = cyc[cyc_i];
	for (int i = cyc_n - 1; i >= 0; i--)
		if (cyc[i] && cyc[i] != sel) {
			wl_list_remove(&cyc[i]->link);
			wl_list_insert(&C.windows, &cyc[i]->link);
			damage_tree(cyc[i]->surf);
		}
	if (sel) {
		wl_list_remove(&sel->link);
		wl_list_insert(&C.windows, &sel->link);
		damage_tree(sel->surf);
	}
}

/* ---- window lifetime ---------------------------------------------------- */

static void window_destroy(struct window *w)
{
	anim_window_closing(w);		/* a copy of the last picture fades out; the struct itself goes now */
	anim_window_gone(w);
	if (G.w == w) {
		G.mode = 0;
		G.w = NULL;
	}
	for (int i = 0; i < cyc_n; i++)
		if (cyc[i] == w)
			cyc[i] = NULL;
	if (cyc_on)
		switcher_changed();
	struct window *x;
	wl_list_for_each(x, &C.windows, link)	/* children lose their parent */
		if (x->parent == w)
			x->parent = NULL;
	if (w->announced)
		fl_emit_removed(w);
	bool wasfoc = C.focused == w;
	wl_list_remove(&w->link);
	if (wasfoc)
		C.focused = NULL;
	if (w->res)
		wl_resource_set_user_data(w->res, NULL);
	if (w->xs)
		w->xs->win = NULL;
	if (w->surf) {
		if (w->mapped)
			damage_tree(w->surf);
		w->surf->win = NULL;
		w->mapped = false;
		surface_unmap(w->surf);
	}
	xwm_window_gone(w);
	free(w->title);
	free(w->app_id);
	free(w);
	overview_changed();
	if (wasfoc)
		window_focus(top_window(NULL));
	update_kb_focus();
	seat_refocus();
}

static void popup_destroy(struct popup *p)
{
	wl_list_remove(&p->link);
	if (p->res)
		wl_resource_set_user_data(p->res, NULL);
	if (p->xs)
		p->xs->popup = NULL;
	if (p->surf) {
		if (p->surf->px)
			damage_tree(p->surf);
		p->surf->popup = NULL;
		surface_unmap(p->surf);
	}
	free(p);
	seat_refocus();
}

/* Called when a wl_surface goes away before its xdg role objects. */
void window_destroy_for_surface(struct surface *s)
{
	if (s->win)
		window_destroy(s->win);
	if (s->popup)
		popup_destroy(s->popup);
}

/* ---- commit hooks ------------------------------------------------------- */

static void window_place(struct window *w)
{
	struct rect a = work_area();
	int gw = geom_w(w), gh = geom_h(w);
	struct xinset in = xwm_insets(w);
	if (w->fullscreen || w->maximized) {
		struct rect f = { 0, 0, C.w, C.h };
		w->x = w->fullscreen ? f.x1 : a.x1;
		w->y = w->fullscreen ? f.y1 : a.y1 + in.t;
		return;
	}
	if (w->xw && (gw + in.l + in.r > rect_w(a) || gh + in.t + in.b > rect_h(a))) {
		/* an X window bigger than the screen (it cannot know): ask for what fits */
		int nw = gw + in.l + in.r > rect_w(a) ? rect_w(a) - in.l - in.r : gw;
		int nh = gh + in.t + in.b > rect_h(a) ? rect_h(a) - in.t - in.b : gh;
		window_configure(w, nw, nh);
		gw = nw;
		gh = nh;
	}
	if (w->parent && w->parent->mapped) {
		w->x = w->parent->x + (geom_w(w->parent) - gw) / 2;
		w->y = w->parent->y + (geom_h(w->parent) - gh) / 2;
	} else {
		static int cascade;
		int off = (cascade++ % 6) * 24;
		w->x = a.x1 + (rect_w(a) - gw) / 2 + off - 60;
		w->y = a.y1 + (rect_h(a) - gh) / 2 + off - 60;
	}
	if (w->x + gw + in.r > a.x2)
		w->x = a.x2 - gw - in.r;
	if (w->y + gh + in.b > a.y2)
		w->y = a.y2 - gh - in.b;
	if (w->x - in.l < a.x1)
		w->x = a.x1 + in.l;
	if (w->y - in.t < a.y1)
		w->y = a.y1 + in.t;
}

static void window_commit(struct window *w, bool was_mapped)
{
	bool now = w->surf->px != NULL;
	w->mapped = now;
	if (now && !was_mapped) {
		window_place(w);
		w->ws = w->parent ? w->parent->ws : C.ws_cur;
		anim_window_open(w);
		if (!w->parent && !w->announced && !window_skip_panel(w)) {
			w->announced = true;
			fl_emit_added(w);
			fl_emit_changed(w);
			fl_emit_window_ws(w);
		}
		w->minimized = false;
		window_focus(w);
		overview_changed();
		seat_refocus();
	} else if (!now && was_mapped) {
		if (w->announced) {
			w->announced = false;
			fl_emit_removed(w);
		}
		if (C.focused == w) {
			C.focused = NULL;
			window_focus(top_window(w));
		}
		overview_changed();
		update_kb_focus();
		seat_refocus();
	} else if (now && w->edges && !w->maximized && !w->fullscreen) {
		/* resizing by the left or top edge: the opposite edge stays put */
		if (w->edges & EDGE_LEFT)
			w->x = w->anchor_r - geom_w(w);
		if (w->edges & EDGE_TOP)
			w->y = w->anchor_b - geom_h(w);
		if (!w->resizing && (!w->xs || w->xs->acked >= w->release_serial))
			w->edges = 0;
	}
}

/* xwm.c: the surface of an X window committed */
void window_commit_xw(struct window *w, bool was_mapped)
{
	window_commit(w, was_mapped);
}

void window_request_state(struct window *w, int max, int full)
{
	if (!w->mapped)
		return;
	window_set_state(w, max < 0 ? w->maximized : max, full < 0 ? w->fullscreen : full);
}

void window_move_begin(struct window *w)
{
	grab_start_move(w);
}

void window_resize_begin(struct window *w, unsigned edges)
{
	grab_start_resize(w, edges);
}

static void popup_send_configure(struct popup *p)
{
	xdg_popup_send_configure(p->res, p->x, p->y, p->w, p->h);
	p->xs->serial = wl_display_next_serial(C.dpy);
	xdg_surface_send_configure(p->xs->res, p->xs->serial);
}

void xdg_surface_commit(struct surface *s, bool was_mapped)
{
	struct xdg_surf *xs = s->xs;
	if (!xs)
		return;
	if (xs->p_geom_set) {
		xs->geom = xs->p_geom;
		xs->geom_set = true;
		xs->p_geom_set = false;
	}
	if (!xs->geom_set) {
		xs->geom = (struct rect){ 0, 0, s->w, s->h };
	}
	if (!xs->configured) {
		xs->configured = true;
		if (s->win)
			window_configure(s->win, s->win->cfg_w, s->win->cfg_h);
		else if (s->popup)
			popup_send_configure(s->popup);
		return;
	}
	if (s->win)
		window_commit(s->win, was_mapped);
}

void shell_surface_commit(struct surface *s, bool was_mapped, bool attached)
{
	if (s->role == ROLE_MENU) {
		bool was_shown = was_mapped && !C.menu_dismissed;
		if (attached)
			C.menu_dismissed = false;
		if (attached && !was_shown)
			anim_surface_show(s);
		update_kb_focus();
	} else if (s->role == ROLE_FLYOUT) {
		bool was_shown = was_mapped && !C.flyout_dismissed;
		if (attached)
			C.flyout_dismissed = false;
		if (attached && !was_shown)
			anim_surface_show(s);
		update_kb_focus();
	} else if (s->role == ROLE_NOTIFY) {
		if (attached && !was_mapped)
			anim_surface_show(s);
	}
}

/* ---- dismissing menu and popups ---------------------------------------- */

void menu_dismiss(void)
{
	if (!menu_visible())
		return;
	anim_surface_hide(C.menu);
	C.menu_dismissed = true;
	damage_tree(C.menu);
	fl_emit_menu_dismissed();
	update_kb_focus();
	seat_refocus();
}

void flyout_dismiss(void)
{
	if (!flyout_visible())
		return;
	anim_surface_hide(C.flyout);
	C.flyout_dismissed = true;
	damage_tree(C.flyout);
	fl_emit_flyout_dismissed();
	update_kb_focus();
	seat_refocus();
}

void popups_dismiss(void)
{
	struct popup *p;
	bool any = false;
	wl_list_for_each_reverse(p, &C.popups, link) {
		if (p->grab && !p->dismissed) {
			p->dismissed = true;
			any = true;
			if (p->res)
				xdg_popup_send_popup_done(p->res);
			if (p->surf)
				damage_tree(p->surf);
		}
	}
	if (any)
		seat_refocus();
}

/* ---- xdg_positioner ---------------------------------------------------- */

struct positioner {
	int w, h, ox, oy;
	struct rect ar;
	uint32_t anchor, gravity;
};

static void pos_destroy_req(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static void pos_set_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h)
{
	struct positioner *p = wl_resource_get_user_data(r);
	p->w = w;
	p->h = h;
}

static void pos_set_anchor_rect(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y,
				int32_t w, int32_t h)
{
	struct positioner *p = wl_resource_get_user_data(r);
	p->ar = (struct rect){ x, y, x + w, y + h };
}

static void pos_set_anchor(struct wl_client *c, struct wl_resource *r, uint32_t a)
{
	((struct positioner *)wl_resource_get_user_data(r))->anchor = a;
}

static void pos_set_gravity(struct wl_client *c, struct wl_resource *r, uint32_t g)
{
	((struct positioner *)wl_resource_get_user_data(r))->gravity = g;
}

static void pos_set_adjust(struct wl_client *c, struct wl_resource *r, uint32_t a) { }

static void pos_set_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y)
{
	struct positioner *p = wl_resource_get_user_data(r);
	p->ox = x;
	p->oy = y;
}

static void pos_nop_reactive(struct wl_client *c, struct wl_resource *r) { }
static void pos_nop_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h) { }
static void pos_nop_serial(struct wl_client *c, struct wl_resource *r, uint32_t s) { }

static const struct xdg_positioner_interface positioner_impl = {
	.destroy = pos_destroy_req,
	.set_size = pos_set_size,
	.set_anchor_rect = pos_set_anchor_rect,
	.set_anchor = pos_set_anchor,
	.set_gravity = pos_set_gravity,
	.set_constraint_adjustment = pos_set_adjust,
	.set_offset = pos_set_offset,
	.set_reactive = pos_nop_reactive,
	.set_parent_size = pos_nop_size,
	.set_parent_configure = pos_nop_serial,
};

static void positioner_free(struct wl_resource *r)
{
	free(wl_resource_get_user_data(r));
}

/* Where the popup goes, relative to the parent's window geometry, slid back
 * onto the screen when it would leave it. */
static void popup_compute(struct popup *p, const struct positioner *ps)
{
	struct rect a = ps->ar;
	int ax, ay, x, y;
	switch (ps->anchor) {
	case XDG_POSITIONER_ANCHOR_LEFT: case XDG_POSITIONER_ANCHOR_TOP_LEFT:
	case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT:
		ax = a.x1; break;
	case XDG_POSITIONER_ANCHOR_RIGHT: case XDG_POSITIONER_ANCHOR_TOP_RIGHT:
	case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT:
		ax = a.x2; break;
	default:
		ax = (a.x1 + a.x2) / 2;
	}
	switch (ps->anchor) {
	case XDG_POSITIONER_ANCHOR_TOP: case XDG_POSITIONER_ANCHOR_TOP_LEFT:
	case XDG_POSITIONER_ANCHOR_TOP_RIGHT:
		ay = a.y1; break;
	case XDG_POSITIONER_ANCHOR_BOTTOM: case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT:
	case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT:
		ay = a.y2; break;
	default:
		ay = (a.y1 + a.y2) / 2;
	}
	switch (ps->gravity) {
	case XDG_POSITIONER_GRAVITY_LEFT: case XDG_POSITIONER_GRAVITY_TOP_LEFT:
	case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:
		x = ax - ps->w; break;
	case XDG_POSITIONER_GRAVITY_RIGHT: case XDG_POSITIONER_GRAVITY_TOP_RIGHT:
	case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT:
		x = ax; break;
	default:
		x = ax - ps->w / 2;
	}
	switch (ps->gravity) {
	case XDG_POSITIONER_GRAVITY_TOP: case XDG_POSITIONER_GRAVITY_TOP_LEFT:
	case XDG_POSITIONER_GRAVITY_TOP_RIGHT:
		y = ay - ps->h; break;
	case XDG_POSITIONER_GRAVITY_BOTTOM: case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:
	case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT:
		y = ay; break;
	default:
		y = ay - ps->h / 2;
	}
	x += ps->ox;
	y += ps->oy;
	int gx, gy;
	surface_origin(p->parent, &gx, &gy);
	if (p->parent->xs) {
		gx += p->parent->xs->geom.x1;
		gy += p->parent->xs->geom.y1;
	}
	struct rect wa = work_area();
	if (gx + x + ps->w > wa.x2)
		x = wa.x2 - ps->w - gx;
	if (gy + y + ps->h > wa.y2)
		y = wa.y2 - ps->h - gy;
	if (gx + x < wa.x1)
		x = wa.x1 - gx;
	if (gy + y < wa.y1)
		y = wa.y1 - gy;
	p->x = x;
	p->y = y;
	p->w = ps->w;
	p->h = ps->h;
}

/* ---- xdg_popup ---------------------------------------------------------- */

static void popup_destroy_req(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static void popup_grab(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat,
		       uint32_t serial)
{
	struct popup *p = wl_resource_get_user_data(r);
	if (p)
		p->grab = true;
}

static void popup_reposition(struct wl_client *c, struct wl_resource *r, struct wl_resource *pos,
			     uint32_t token)
{
	struct popup *p = wl_resource_get_user_data(r);
	if (!p)
		return;
	if (p->surf && p->surf->px)
		damage_tree(p->surf);
	popup_compute(p, wl_resource_get_user_data(pos));
	xdg_popup_send_repositioned(r, token);
	popup_send_configure(p);
}

static const struct xdg_popup_interface popup_impl = {
	.destroy = popup_destroy_req, .grab = popup_grab, .reposition = popup_reposition,
};

static void popup_res_destroy(struct wl_resource *r)
{
	struct popup *p = wl_resource_get_user_data(r);
	if (p) {
		p->res = NULL;
		popup_destroy(p);
	}
}

/* ---- xdg_toplevel ------------------------------------------------------- */

#define WIN() struct window *w = wl_resource_get_user_data(r); if (!w) return

static void tl_destroy_req(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static void tl_set_parent(struct wl_client *c, struct wl_resource *r, struct wl_resource *par)
{
	WIN();
	w->parent = par ? wl_resource_get_user_data(par) : NULL;
}

static void tl_set_title(struct wl_client *c, struct wl_resource *r, const char *t)
{
	WIN();
	char *n = strdup(t);
	if (!n)
		return;
	free(w->title);
	w->title = n;
	if (w->announced)
		fl_emit_changed(w);
}

static void tl_set_app_id(struct wl_client *c, struct wl_resource *r, const char *t)
{
	WIN();
	char *n = strdup(t);
	if (!n)
		return;
	free(w->app_id);
	w->app_id = n;
}

static void tl_show_window_menu(struct wl_client *c, struct wl_resource *r, struct wl_resource *s,
				uint32_t serial, int32_t x, int32_t y) { }

static void tl_move(struct wl_client *c, struct wl_resource *r, struct wl_resource *s,
		    uint32_t serial)
{
	WIN();
	grab_start_move(w);
}

static void tl_resize(struct wl_client *c, struct wl_resource *r, struct wl_resource *s,
		      uint32_t serial, uint32_t edges)
{
	WIN();
	grab_start_resize(w, edges);
}

static void tl_set_max_size(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y)
{
	WIN();
	w->max_w = x;
	w->max_h = y;
}

static void tl_set_min_size(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y)
{
	WIN();
	w->min_w = x;
	w->min_h = y;
}

static void tl_set_maximized(struct wl_client *c, struct wl_resource *r)
{
	WIN();
	window_set_state(w, true, w->fullscreen);
}

static void tl_unset_maximized(struct wl_client *c, struct wl_resource *r)
{
	WIN();
	window_set_state(w, false, w->fullscreen);
}

static void tl_set_fullscreen(struct wl_client *c, struct wl_resource *r, struct wl_resource *o)
{
	WIN();
	window_set_state(w, w->maximized, true);
}

static void tl_unset_fullscreen(struct wl_client *c, struct wl_resource *r)
{
	WIN();
	window_set_state(w, w->maximized, false);
}

static void tl_set_minimized(struct wl_client *c, struct wl_resource *r)
{
	WIN();
	window_minimize(w);
}

static const struct xdg_toplevel_interface toplevel_impl = {
	.destroy = tl_destroy_req,
	.set_parent = tl_set_parent,
	.set_title = tl_set_title,
	.set_app_id = tl_set_app_id,
	.show_window_menu = tl_show_window_menu,
	.move = tl_move,
	.resize = tl_resize,
	.set_max_size = tl_set_max_size,
	.set_min_size = tl_set_min_size,
	.set_maximized = tl_set_maximized,
	.unset_maximized = tl_unset_maximized,
	.set_fullscreen = tl_set_fullscreen,
	.unset_fullscreen = tl_unset_fullscreen,
	.set_minimized = tl_set_minimized,
};

static void tl_res_destroy(struct wl_resource *r)
{
	struct window *w = wl_resource_get_user_data(r);
	if (w) {
		w->res = NULL;
		window_destroy(w);
	}
}

/* ---- xdg_surface -------------------------------------------------------- */

static void xs_destroy_req(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static void xs_get_toplevel(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	struct xdg_surf *xs = wl_resource_get_user_data(r);
	if (!xs->surf || xs->win || xs->popup) {
		wl_resource_post_error(r, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED, "bad xdg_surface");
		return;
	}
	struct surface *s = xs->surf;
	if (s->role != ROLE_NONE && s->role != ROLE_TOPLEVEL) {
		wl_resource_post_error(r, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED, "surface has another role");
		return;
	}
	struct window *w = calloc(1, sizeof(*w));
	if (!w) {
		wl_client_post_no_memory(c);
		return;
	}
	w->res = wl_resource_create(c, &xdg_toplevel_interface, wl_resource_get_version(r), id);
	if (!w->res) {
		free(w);
		wl_client_post_no_memory(c);
		return;
	}
	w->id = ++C.next_id;
	w->surf = s;
	w->xs = xs;
	w->title = strdup("");
	w->app_id = strdup("");
	s->role = ROLE_TOPLEVEL;
	s->win = w;
	xs->win = w;
	wl_list_insert(&C.windows, &w->link);
	wl_resource_set_implementation(w->res, &toplevel_impl, w, tl_res_destroy);
}

static void xs_get_popup(struct wl_client *c, struct wl_resource *r, uint32_t id,
			 struct wl_resource *parent_res, struct wl_resource *pos_res)
{
	struct xdg_surf *xs = wl_resource_get_user_data(r);
	struct xdg_surf *pxs = parent_res ? wl_resource_get_user_data(parent_res) : NULL;
	if (!xs->surf || xs->win || xs->popup || !pxs || !pxs->surf) {
		wl_resource_post_error(r, XDG_SURFACE_ERROR_NOT_CONSTRUCTED, "bad popup");
		return;
	}
	struct surface *s = xs->surf;
	if (s->role != ROLE_NONE && s->role != ROLE_POPUP) {
		wl_resource_post_error(r, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED, "surface has another role");
		return;
	}
	struct popup *p = calloc(1, sizeof(*p));
	if (!p) {
		wl_client_post_no_memory(c);
		return;
	}
	p->res = wl_resource_create(c, &xdg_popup_interface, wl_resource_get_version(r), id);
	if (!p->res) {
		free(p);
		wl_client_post_no_memory(c);
		return;
	}
	p->xs = xs;
	p->surf = s;
	p->parent = pxs->surf;
	s->role = ROLE_POPUP;
	s->popup = p;
	xs->popup = p;
	popup_compute(p, wl_resource_get_user_data(pos_res));
	wl_list_insert(C.popups.prev, &p->link);
	wl_resource_set_implementation(p->res, &popup_impl, p, popup_res_destroy);
}

static void xs_set_geometry(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y,
			    int32_t w, int32_t h)
{
	struct xdg_surf *xs = wl_resource_get_user_data(r);
	if (w <= 0 || h <= 0)
		return;
	xs->p_geom = (struct rect){ x, y, x + w, y + h };
	xs->p_geom_set = true;
}

static void xs_ack_configure(struct wl_client *c, struct wl_resource *r, uint32_t serial)
{
	struct xdg_surf *xs = wl_resource_get_user_data(r);
	xs->acked = serial;
}

static const struct xdg_surface_interface xdg_surface_impl = {
	.destroy = xs_destroy_req,
	.get_toplevel = xs_get_toplevel,
	.get_popup = xs_get_popup,
	.set_window_geometry = xs_set_geometry,
	.ack_configure = xs_ack_configure,
};

static void xs_res_destroy(struct wl_resource *r)
{
	struct xdg_surf *xs = wl_resource_get_user_data(r);
	if (xs->win)
		window_destroy(xs->win);
	if (xs->popup)
		popup_destroy(xs->popup);
	if (xs->surf)
		xs->surf->xs = NULL;
	free(xs);
}

/* ---- xdg_wm_base -------------------------------------------------------- */

static void wm_destroy_req(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static void wm_create_positioner(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	struct positioner *p = calloc(1, sizeof(*p));
	struct wl_resource *res = p ? wl_resource_create(c, &xdg_positioner_interface,
							 wl_resource_get_version(r), id) : NULL;
	if (!res) {
		free(p);
		wl_client_post_no_memory(c);
		return;
	}
	wl_resource_set_implementation(res, &positioner_impl, p, positioner_free);
}

static void wm_get_xdg_surface(struct wl_client *c, struct wl_resource *r, uint32_t id,
			       struct wl_resource *surf_res)
{
	struct surface *s = wl_resource_get_user_data(surf_res);
	if (s->xs) {
		wl_resource_post_error(r, XDG_WM_BASE_ERROR_ROLE, "surface already has an xdg_surface");
		return;
	}
	struct xdg_surf *xs = calloc(1, sizeof(*xs));
	if (!xs) {
		wl_client_post_no_memory(c);
		return;
	}
	xs->res = wl_resource_create(c, &xdg_surface_interface, wl_resource_get_version(r), id);
	if (!xs->res) {
		free(xs);
		wl_client_post_no_memory(c);
		return;
	}
	xs->surf = s;
	s->xs = xs;
	wl_resource_set_implementation(xs->res, &xdg_surface_impl, xs, xs_res_destroy);
}

static void wm_pong(struct wl_client *c, struct wl_resource *r, uint32_t serial) { }

static const struct xdg_wm_base_interface wm_base_impl = {
	.destroy = wm_destroy_req,
	.create_positioner = wm_create_positioner,
	.get_xdg_surface = wm_get_xdg_surface,
	.pong = wm_pong,
};

static void wm_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &xdg_wm_base_interface, version, id);
	if (!r) {
		wl_client_post_no_memory(c);
		return;
	}
	wl_resource_set_implementation(r, &wm_base_impl, NULL, NULL);
}

void xdg_init(void)
{
	wl_global_create(C.dpy, &xdg_wm_base_interface, 3, NULL, wm_bind);
}
