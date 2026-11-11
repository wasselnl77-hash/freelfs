/* wl.c -- Wayland client glue: connection, windows, shm buffers, input, loop
 * SPDX-License-Identifier: MIT
 *
 * Windows draw straight into wl_shm buffers (two per window). There are no
 * frame callbacks: a pass renders once per loop turn, and when both buffers
 * are still with the compositor the pass runs "dry" (no pixels) so that
 * input is never lost, and the picture follows when a buffer comes back.
 * With FLUI_PPM set there is no connection at all (see fl_app_run).
 */
#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "xdg-shell-client-protocol.h"

#define BTN_LEFT_ 0x110
#define BTN_RIGHT_ 0x111
#define BTN_MIDDLE_ 0x112

struct buf {
	struct wl_buffer *b;
	int busy;
};

struct fl_win {
	fl_app *app;
	fl_win *next;
	struct wl_surface *surf;
	struct xdg_surface *xs;
	struct xdg_toplevel *xt;
	fl_win_opts o;
	char *title, *app_id;
	int w, h;
	int pw, ph; /* size asked for by the last configure */
	int mapped, configured, dead;
	int activated, maximized, fullscreen;
	int dirty, need_present;
	/* shm */
	int fd;
	uint8_t *map;
	size_t map_size;
	struct wl_shm_pool *pool;
	struct buf buf[2];
	int bw, bh;
	fl_ui ui;
	int clicks;
	uint32_t click_t;
	int click_x, click_y;
	int index;
};

struct src {
	int fd;
	short events;
	void (*cb)(void *, int, short);
	void *ud;
	int dead;
};

struct fl_timer {
	fl_app *app;
	int fd;
	void (*cb)(void *);
	void *ud;
};

struct cursor {
	struct wl_surface *s;
	struct wl_buffer *b;
	int hx, hy;
};

struct fl_app {
	struct wl_display *dpy;
	struct wl_registry *reg;
	struct wl_compositor *comp;
	struct wl_shm *shm;
	struct xdg_wm_base *wm;
	struct wl_seat *seat;
	struct wl_data_device_manager *ddm;
	struct wl_data_device *dd;
	struct wl_output *out;
	int out_w, out_h;
	struct wl_pointer *ptr;
	struct wl_keyboard *kbd;
	fl_global_cb global_cb;
	void *global_ud;

	int ppm;
	const char *ppm_path;

	fl_win *wins;
	int nwins;
	fl_win *pfocus, *kfocus;
	uint32_t serial, ptr_serial;
	int shape;

	struct xkb_context *xctx;
	struct xkb_keymap *keymap;
	struct xkb_state *xstate;
	fl_compose compose;
	int layout_override;
	fl_timer *rep;
	uint32_t rep_key;
	int rep_delay, rep_rate;

	struct src *src;
	int nsrc, capsrc;
	int quit;

	struct cursor cur[2];

	/* clipboard */
	char *clip;
	size_t cliplen;
	struct wl_data_source *own;
	struct wl_data_offer *sel, *pending;
	int pending_text, sel_text;
	const char *sel_mime;
};

static void noop(void) {}
#define NOOP ((void *)noop)

/* ---- shm ------------------------------------------------------------------------- */

static int shm_file(size_t size)
{
	int fd = memfd_create("flui", MFD_CLOEXEC);
	if (fd < 0) {
		char name[] = "/dev/shm/flui-XXXXXX";
		fd = mkstemp(name);
		if (fd < 0)
			return -1;
		unlink(name);
	}
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void buffer_release(void *data, struct wl_buffer *b)
{
	fl_win *w = data;
	for (int i = 0; i < 2; i++)
		if (w->buf[i].b == b)
			w->buf[i].busy = 0;
	if (w->need_present && !w->dead) {
		w->dirty = 1;
		w->need_present = 0;
	}
}

static const struct wl_buffer_listener buffer_listener = {.release = buffer_release};

static void free_buffers(fl_win *w)
{
	for (int i = 0; i < 2; i++) {
		if (w->buf[i].b)
			wl_buffer_destroy(w->buf[i].b);
		w->buf[i].b = NULL;
		w->buf[i].busy = 0;
	}
	if (w->pool)
		wl_shm_pool_destroy(w->pool);
	w->pool = NULL;
	if (w->map)
		munmap(w->map, w->map_size);
	w->map = NULL;
	if (w->fd >= 0)
		close(w->fd);
	w->fd = -1;
	w->bw = w->bh = 0;
}

static int ensure_buffers(fl_win *w)
{
	if (w->bw == w->w && w->bh == w->h && w->map)
		return 0;
	free_buffers(w);
	size_t one = (size_t)w->w * w->h * 4;
	w->map_size = one * 2;
	w->fd = shm_file(w->map_size);
	if (w->fd < 0)
		return -1;
	w->map = mmap(NULL, w->map_size, PROT_READ | PROT_WRITE, MAP_SHARED, w->fd, 0);
	if (w->map == MAP_FAILED) {
		w->map = NULL;
		free_buffers(w);
		return -1;
	}
	w->pool = wl_shm_create_pool(w->app->shm, w->fd, (int)w->map_size);
	for (int i = 0; i < 2; i++) {
		w->buf[i].b = wl_shm_pool_create_buffer(w->pool, (int)(one * i), w->w, w->h, w->w * 4,
		                                        WL_SHM_FORMAT_ARGB8888);
		wl_buffer_add_listener(w->buf[i].b, &buffer_listener, w);
	}
	w->bw = w->w;
	w->bh = w->h;
	return 0;
}

/* ---- cursors ------------------------------------------------------------------------ */

static void make_cursor(fl_app *a, int shape)
{
	struct cursor *c = &a->cur[shape];
	if (c->s)
		return;
	int sz = 24;
	int fd = shm_file((size_t)sz * sz * 4);
	if (fd < 0)
		return;
	uint32_t *px = mmap(NULL, (size_t)sz * sz * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (px == MAP_FAILED) {
		close(fd);
		return;
	}
	memset(px, 0, (size_t)sz * sz * 4);
	fl_canvas cv = {px, sz, sz, sz, {0, 0, sz, sz}};
	fl_path *p = fl_path_new();
	if (shape == FL_CURSOR_ARROW) {
		fl_path_svg(p, "M1.5 1.5v15.5l3.8-3.6 2.8 6.4 2.4-1-2.8-6.2h5.2z", 1, 1, 0, 0);
		fl_path_stroke(&cv, p, 2.4f, FL_HEX(0x16191c));
		fl_path_fill(&cv, p, FL_HEX(0xf0f2f4));
		c->hx = 1;
		c->hy = 1;
	} else {
		fl_path_svg(p, "M5 3h5M7.5 3v15M5 18h5", 1, 1, 0, 0);
		fl_path_stroke(&cv, p, 3.4f, FL_HEX(0x16191c));
		fl_path_stroke(&cv, p, 1.2f, FL_HEX(0xf0f2f4));
		c->hx = 8;
		c->hy = 10;
	}
	fl_path_free(p);
	struct wl_shm_pool *pool = wl_shm_create_pool(a->shm, fd, sz * sz * 4);
	c->b = wl_shm_pool_create_buffer(pool, 0, sz, sz, sz * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	munmap(px, (size_t)sz * sz * 4);
	close(fd);
	c->s = wl_compositor_create_surface(a->comp);
	wl_surface_attach(c->s, c->b, 0, 0);
	wl_surface_damage(c->s, 0, 0, sz, sz);
	wl_surface_commit(c->s);
}

static void set_cursor(fl_app *a, int shape)
{
	if (!a->ptr || !a->pfocus)
		return;
	make_cursor(a, shape);
	struct cursor *c = &a->cur[shape];
	wl_pointer_set_cursor(a->ptr, a->ptr_serial, c->s, c->hx, c->hy);
	a->shape = shape;
}

/* ---- decorations --------------------------------------------------------------------- */

#define EDGE 5

fl_rect fl_win_content(const fl_win *w)
{
	if (!w->o.decorated)
		return (fl_rect){0, 0, w->w, w->h};
	int b = (w->maximized || w->fullscreen) ? 0 : 1, top = fl_lk.title_h + 2; /* 36 normal, 30 compact */
	return (fl_rect){b, top, w->w - 2 * b, w->h - top - b};
}

static void decorate(fl_win *w)
{
	fl_ui *ui = &w->ui;
	fl_canvas *cv = &ui->cv;
	int full = w->maximized || w->fullscreen;
	int b = full ? 0 : 1;
	fl_fill_rect(cv, (fl_rect){0, 0, w->w, w->h}, fl_th.panel);
	if (!full)
		fl_stroke_rect(cv, (fl_rect){0, 0, w->w, w->h}, 1, fl_th.line);
	int top = w->activated ? 2 : 1;
	if (full && !w->activated)
		top = 0;
	if (top)
		fl_fill_rect(cv, (fl_rect){0, 0, w->w, top}, w->activated ? fl_th.accent : fl_th.line);

	/* grab an edge to resize */
	if (!full && w->o.resizable && w->xt && (ui->press & FL_BTN_L) && ui->mouse_in) {
		int e = 0, x = ui->mx, y = ui->my;
		int l = x < EDGE, r = x >= w->w - EDGE, t = y < 3, bo = y >= w->h - EDGE;
		if (t && l) e = XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
		else if (t && r) e = XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
		else if (bo && l) e = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
		else if (bo && r) e = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
		else if (t) e = XDG_TOPLEVEL_RESIZE_EDGE_TOP;
		else if (bo) e = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
		else if (l) e = XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
		else if (r) e = XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
		if (e) {
			xdg_toplevel_resize(w->xt, w->app->seat, w->app->serial, e);
			ui->press = 0;
			ui->btn_held = 0;
			return;
		}
	}

	/* the bar ends one line above the content: at 35 for the normal height */
	fl_rect tb = {b, top, w->w - 2 * b, fl_lk.title_h + 1 - top};
	if (top == 0)
		tb.h = fl_lk.title_h;
	int hit = fl_titlebar(ui, tb, w->title ? w->title : "", w->activated, w->maximized);
	fl_fill_rect(cv, (fl_rect){b, tb.y + tb.h, tb.w, 1}, fl_th.line);
	if (hit == FL_ICON_CLOSE) {
		if (w->o.close)
			w->o.close(w, w->o.ud);
		else
			fl_app_quit(w->app);
	} else if (hit == FL_ICON_MINIMIZE) {
		fl_win_minimize(w);
	} else if (hit == FL_ICON_MAXIMIZE) {
		if (w->xt) {
			if (w->maximized)
				xdg_toplevel_unset_maximized(w->xt);
			else
				xdg_toplevel_set_maximized(w->xt);
		}
	} else if ((ui->press & FL_BTN_L) && !ui->active && fl_in(tb, ui->mx, ui->my) && w->xt) {
		if (ui->clicks >= 2) {
			if (w->maximized)
				xdg_toplevel_unset_maximized(w->xt);
			else
				xdg_toplevel_set_maximized(w->xt);
		} else {
			xdg_toplevel_move(w->xt, w->app->seat, w->app->serial);
		}
		ui->press = 0;
		ui->btn_held = 0;
	}
}

/* ---- the pass ------------------------------------------------------------------------- */

static void clear_events(fl_ui *ui)
{
	ui->press = ui->release = 0;
	ui->wheel = 0;
	ui->key = 0;
	ui->text[0] = 0;
	ui->_wheel_taken = 0;
}

static void run_pass(fl_win *w, uint32_t *px)
{
	fl_ui *ui = &w->ui;
	for (int pass = 0; pass < 3; pass++) {
		ui->cv = (fl_canvas){px, w->w, w->h, w->w, {0, 0, w->w, w->h}};
		ui->cursor = FL_CURSOR_ARROW;
		ui->_repass = 0;
		ui->_last_focus = ui->_first_focus = ui->_prev_focus = 0;
		ui->_focus_seen = 0;
		ui->_press_taken = 0;
		if (w->o.decorated)
			decorate(w);
		if (w->o.decorated)
			fl_clip(&ui->cv, fl_win_content(w));
		if (w->o.draw)
			w->o.draw(ui, w->o.ud);
		ui->cv.clip = (fl_rect){0, 0, w->w, w->h};
		/* Tab moved the focus past the last or before the first widget */
		if (ui->_focus_next) {
			ui->focus = ui->_first_focus;
			ui->_focus_next = 0;
			ui->_repass = 1;
		}
		if (ui->_focus_prev) {
			ui->focus = ui->_last_focus;
			ui->_focus_prev = 0;
			ui->_repass = 1;
		}
		if (ui->focus && !ui->_focus_seen)
			ui->focus = 0;
		if ((ui->press & FL_BTN_L) && !ui->_press_taken && ui->focus) {
			ui->focus = 0; /* a click on nothing drops the focus */
			ui->_repass = 1;
		}
		if ((ui->key == XKB_KEY_Tab || ui->key == XKB_KEY_ISO_Left_Tab) && !ui->focus &&
		    ui->_first_focus) {
			ui->focus = (ui->key == XKB_KEY_Tab || !ui->_last_focus) ? ui->_first_focus : ui->_last_focus;
			ui->_repass = 1;
		}
		if (ui->release & FL_BTN_L)
			ui->active = 0;
		clear_events(ui);
		if (!ui->_repass)
			break;
	}
	if (px && w->app->ptr && w->app->pfocus == w && ui->cursor != w->app->shape)
		set_cursor(w->app, ui->cursor);
}

static void render(fl_win *w)
{
	fl_app *a = w->app;
	if (w->dead || !w->mapped || !w->configured)
		return;
	w->dirty = 0;
	if (a->ppm)
		return;
	if (w->w <= 0 || w->h <= 0)
		return;
	if (ensure_buffers(w) < 0)
		return;
	struct buf *b = NULL;
	for (int i = 0; i < 2; i++)
		if (!w->buf[i].busy) {
			b = &w->buf[i];
			break;
		}
	if (!b) {
		w->need_present = 1;
		run_pass(w, NULL);
		return;
	}
	size_t one = (size_t)w->w * w->h * 4;
	uint32_t *px = (uint32_t *)(w->map + (b == &w->buf[0] ? 0 : one));
	run_pass(w, px);
	if (w->dead || !w->mapped)
		return; /* the pass itself hid or closed the window */
	wl_surface_attach(w->surf, b->b, 0, 0);
	wl_surface_damage_buffer(w->surf, 0, 0, w->w, w->h);
	wl_surface_commit(w->surf);
	b->busy = 1;
}

/* The title bar height changed (look): the content moved, tell the apps the new size. */
HIDDEN void fl_wins_relayout(fl_app *a)
{
	for (fl_win *w = a->wins; w; w = w->next)
		if (w->o.decorated && w->o.resized && w->configured && !w->dead) {
			fl_rect c = fl_win_content(w);
			w->o.resized(w, c.w, c.h, w->o.ud);
		}
}

void fl_win_redraw(fl_win *w)
{
	if (w && !w->dead)
		w->dirty = 1;
}

void fl_win_redraw_all(fl_app *a)
{
	for (fl_win *w = a->wins; w; w = w->next)
		fl_win_redraw(w);
}

fl_ui *fl_win_ui(fl_win *w) { return &w->ui; }
fl_app *fl_win_app(fl_win *w) { return w->app; }
struct wl_surface *fl_win_surface(fl_win *w) { return w->surf; }
int fl_win_active(const fl_win *w) { return w->activated; }

/* ---- windows ----------------------------------------------------------------------------- */

static void xs_configure(void *data, struct xdg_surface *xs, uint32_t serial)
{
	fl_win *w = data;
	xdg_surface_ack_configure(xs, serial);
	int nw = w->pw > 0 ? w->pw : (w->w > 0 ? w->w : w->o.w);
	int nh = w->ph > 0 ? w->ph : (w->h > 0 ? w->h : w->o.h);
	if (w->o.min_w && nw < w->o.min_w)
		nw = w->o.min_w;
	if (w->o.min_h && nh < w->o.min_h)
		nh = w->o.min_h;
	int changed = nw != w->w || nh != w->h || !w->configured;
	w->w = nw;
	w->h = nh;
	w->configured = 1;
	if (changed && w->o.resized) {
		fl_rect c = fl_win_content(w);
		w->o.resized(w, c.w, c.h, w->o.ud);
	}
	render(w);
}

static void xt_configure(void *data, struct xdg_toplevel *xt, int32_t width, int32_t height,
                         struct wl_array *states)
{
	fl_win *w = data;
	w->pw = width;
	w->ph = height;
	w->activated = w->maximized = w->fullscreen = 0;
	uint32_t *s;
	wl_array_for_each(s, states)
	{
		if (*s == XDG_TOPLEVEL_STATE_ACTIVATED)
			w->activated = 1;
		else if (*s == XDG_TOPLEVEL_STATE_MAXIMIZED)
			w->maximized = 1;
		else if (*s == XDG_TOPLEVEL_STATE_FULLSCREEN)
			w->fullscreen = 1;
	}
	w->dirty = 1;
}

static void xt_close(void *data, struct xdg_toplevel *xt)
{
	fl_win *w = data;
	if (w->o.close)
		w->o.close(w, w->o.ud);
	else
		fl_app_quit(w->app);
}

static const struct xdg_surface_listener xs_listener = {.configure = xs_configure};
static const struct xdg_toplevel_listener xt_listener = {
	.configure = xt_configure, .close = xt_close,
	.configure_bounds = NOOP, .wm_capabilities = NOOP,
};

fl_win *fl_win_new(fl_app *a, const fl_win_opts *o)
{
	fl_win *w = calloc(1, sizeof(*w));
	if (!w)
		return NULL;
	w->app = a;
	w->o = *o;
	w->fd = -1;
	w->title = strdup(o->title ? o->title : "");
	w->app_id = o->app_id ? strdup(o->app_id) : NULL;
	w->w = o->w > 0 ? o->w : 640;
	w->h = o->h > 0 ? o->h : 480;
	if (!o->raw && !o->resizable && !o->decorated)
		w->o.resizable = 0;
	else if (!o->raw && !o->resizable)
		w->o.resizable = 1;
	w->index = a->nwins++;
	w->ui.win = w;
	w->ui.clicks = 1;
	{
		float px = o->font_px > 0 ? o->font_px : 13;
		w->ui.f_reg = fl_font_ui(400, px);
		w->ui.f_med = fl_font_ui(500, px);
		w->ui.f_semi = fl_font_ui(600, px);
	}
	w->next = NULL;
	fl_win **pp = &a->wins;
	while (*pp)
		pp = &(*pp)->next;
	*pp = w;
	if (a->ppm) {
		w->mapped = w->configured = 1;
		w->activated = 1;
		if (w->o.resized) {
			fl_rect c = fl_win_content(w);
			w->o.resized(w, c.w, c.h, w->o.ud);
		}
		return w;
	}
	w->surf = wl_compositor_create_surface(a->comp);
	if (o->raw) {
		w->configured = 1;
		return w; /* mapped by fl_win_map, after the caller gave it a role */
	}
	w->mapped = 1;
	w->xs = xdg_wm_base_get_xdg_surface(a->wm, w->surf);
	xdg_surface_add_listener(w->xs, &xs_listener, w);
	w->xt = xdg_surface_get_toplevel(w->xs);
	xdg_toplevel_add_listener(w->xt, &xt_listener, w);
	xdg_toplevel_set_title(w->xt, w->title);
	if (w->app_id)
		xdg_toplevel_set_app_id(w->xt, w->app_id);
	if (o->min_w || o->min_h)
		xdg_toplevel_set_min_size(w->xt, o->min_w, o->min_h);
	if (o->fullscreen)
		xdg_toplevel_set_fullscreen(w->xt, NULL);
	if (w->o.resized) {
		fl_rect c = fl_win_content(w);
		w->o.resized(w, c.w, c.h, w->o.ud);
	}
	wl_surface_commit(w->surf);
	return w;
}

void fl_win_map(fl_win *w)
{
	if (w->dead)
		return;
	w->mapped = 1;
	w->dirty = 1;
	if (w->o.resized) {
		fl_rect c = fl_win_content(w);
		w->o.resized(w, c.w, c.h, w->o.ud);
	}
}

void fl_win_hide(fl_win *w)
{
	if (w->dead || !w->mapped)
		return;
	w->mapped = 0;
	w->dirty = 0;
	if (w->app->kfocus == w) {
		w->app->rep_key = 0; /* no repeat into a window that is gone */
		if (w->app->rep)
			fl_timer_arm(w->app->rep, 0, 0);
	}
	if (w->surf) {
		wl_surface_attach(w->surf, NULL, 0, 0);
		wl_surface_commit(w->surf);
	}
}

void fl_win_resize(fl_win *w, int width, int height)
{
	if (w->w == width && w->h == height)
		return;
	w->w = width;
	w->h = height;
	w->dirty = 1;
}

void fl_win_set_title(fl_win *w, const char *title)
{
	free(w->title);
	w->title = strdup(title);
	if (w->xt)
		xdg_toplevel_set_title(w->xt, title);
	w->dirty = 1;
}

void fl_win_minimize(fl_win *w)
{
	if (w->xt)
		xdg_toplevel_set_minimized(w->xt);
}

void fl_win_maximize(fl_win *w)
{
	if (w && w->xt)
		xdg_toplevel_set_maximized(w->xt);
}

void fl_win_destroy(fl_win *w)
{
	if (!w || w->dead)
		return;
	fl_app *a = w->app;
	w->dead = 1;
	if (a->pfocus == w)
		a->pfocus = NULL;
	if (a->kfocus == w) {
		a->kfocus = NULL;
		if (a->rep)
			fl_timer_arm(a->rep, 0, 0);
	}
	free_buffers(w);
	fl_font_close(w->ui.f_reg);
	fl_font_close(w->ui.f_med);
	fl_font_close(w->ui.f_semi);
	w->ui.f_reg = w->ui.f_med = w->ui.f_semi = NULL;
	if (w->xt)
		xdg_toplevel_destroy(w->xt);
	if (w->xs)
		xdg_surface_destroy(w->xs);
	if (w->surf)
		wl_surface_destroy(w->surf);
	w->xt = NULL;
	w->xs = NULL;
	w->surf = NULL;
}

static void reap(fl_app *a)
{
	for (fl_win **pp = &a->wins; *pp;) {
		fl_win *w = *pp;
		if (w->dead) {
			*pp = w->next;
			free(w->title);
			free(w->app_id);
			free(w);
		} else {
			pp = &w->next;
		}
	}
}

static fl_win *win_of(fl_app *a, struct wl_surface *s)
{
	for (fl_win *w = a->wins; w; w = w->next)
		if (w->surf == s && !w->dead)
			return w;
	return NULL;
}

/* ---- input ------------------------------------------------------------------------------- */

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
                          wl_fixed_t x, wl_fixed_t y)
{
	fl_app *a = data;
	fl_win *w = win_of(a, s);
	a->ptr_serial = serial;
	a->pfocus = w;
	if (!w)
		return;
	w->ui.mouse_in = 1;
	w->ui.mx = wl_fixed_to_int(x);
	w->ui.my = wl_fixed_to_int(y);
	a->shape = -1;
	set_cursor(a, FL_CURSOR_ARROW);
	w->dirty = 1;
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
	fl_app *a = data;
	fl_win *w = win_of(a, s);
	if (w) {
		w->ui.mouse_in = 0;
		w->dirty = 1;
	}
	if (a->pfocus == w)
		a->pfocus = NULL;
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	fl_app *a = data;
	fl_win *w = a->pfocus;
	if (!w)
		return;
	w->ui.mx = wl_fixed_to_int(x);
	w->ui.my = wl_fixed_to_int(y);
	w->ui.mouse_in = 1;
	if (w->o.pointer)
		w->o.pointer(w, FL_PTR_MOVE, w->ui.mx, w->ui.my, 0, w->o.ud);
	w->dirty = 1;
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
                           uint32_t button, uint32_t state)
{
	fl_app *a = data;
	fl_win *w = a->pfocus;
	a->serial = serial;
	if (!w)
		return;
	fl_ui *ui = &w->ui;
	int bit = button == BTN_LEFT_ ? FL_BTN_L : button == BTN_RIGHT_ ? FL_BTN_R : button == BTN_MIDDLE_ ? FL_BTN_M : 0;
	if (!bit)
		return;
	int down = state == WL_POINTER_BUTTON_STATE_PRESSED;
	if (down) {
		ui->btn_held |= bit;
		ui->press = bit;
		if (time - w->click_t < 400 && abs(ui->mx - w->click_x) < 5 && abs(ui->my - w->click_y) < 5)
			w->clicks++;
		else
			w->clicks = 1;
		w->click_t = time;
		w->click_x = ui->mx;
		w->click_y = ui->my;
		ui->clicks = w->clicks;
	} else {
		ui->btn_held &= ~bit;
		ui->release = bit;
	}
	if (w->o.pointer)
		w->o.pointer(w, down ? FL_PTR_DOWN : FL_PTR_UP, ui->mx, ui->my, bit, w->o.ud);
	render(w);
}

static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t v)
{
	fl_app *a = data;
	fl_win *w = a->pfocus;
	if (!w || axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
		return;
	double d = wl_fixed_to_double(v) / 10.0;
	int steps = (int)(d < 0 ? d - 0.5 : d + 0.5);
	if (!steps)
		steps = d < 0 ? -1 : 1;
	w->ui.wheel += steps;
	if (w->o.pointer)
		w->o.pointer(w, FL_PTR_WHEEL, w->ui.mx, w->ui.my, steps, w->o.ud);
	render(w);
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion,
	.button = pointer_button, .axis = pointer_axis, .frame = NOOP, .axis_source = NOOP,
	.axis_stop = NOOP, .axis_discrete = NOOP, .axis_value120 = NOOP,
	.axis_relative_direction = NOOP,
};

static void set_default_keymap(fl_app *a)
{
	if (a->xstate)
		xkb_state_unref(a->xstate);
	if (a->keymap)
		xkb_keymap_unref(a->keymap);
	struct xkb_rule_names names = {0};
	a->keymap = xkb_keymap_new_from_names(a->xctx, &names, 0);
	a->xstate = a->keymap ? xkb_state_new(a->keymap) : NULL;
}

static void kbd_keymap(void *data, struct wl_keyboard *k, uint32_t format, int32_t fd, uint32_t size)
{
	fl_app *a = data;
	if (a->layout_override) {
		close(fd);
		return;
	}
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (map == MAP_FAILED)
		return;
	char *copy = malloc(size + 1);
	if (copy) {
		memcpy(copy, map, size);
		copy[size] = 0;
		struct xkb_keymap *km = xkb_keymap_new_from_string(a->xctx, copy, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
		if (km) {
			struct xkb_state *st = xkb_state_new(km);
			if (a->xstate)
				xkb_state_unref(a->xstate);
			if (a->keymap)
				xkb_keymap_unref(a->keymap);
			a->keymap = km;
			a->xstate = st;
		}
		free(copy);
	}
	munmap(map, size);
}

void fl_app_set_layout(fl_app *a, const char *layout)
{
	if (a->ppm || !a->xctx)
		return;
	struct xkb_rule_names names = {.layout = layout};
	struct xkb_keymap *km = xkb_keymap_new_from_names(a->xctx, &names, 0);
	if (!km)
		return;
	if (a->xstate)
		xkb_state_unref(a->xstate);
	if (a->keymap)
		xkb_keymap_unref(a->keymap);
	a->keymap = km;
	a->xstate = xkb_state_new(km);
	a->layout_override = 1;
	a->compose.dead = 0;
}

static void kbd_enter(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s,
                      struct wl_array *keys)
{
	fl_app *a = data;
	a->serial = serial;
	/* a key held when focus last left may have lost its release: never let its
	 * repeat carry over into a new focus (it would press Enter in a new menu) */
	a->rep_key = 0;
	if (a->rep)
		fl_timer_arm(a->rep, 0, 0);
	a->kfocus = win_of(a, s);
	if (a->kfocus) {
		a->kfocus->dirty = 1;
		if (a->kfocus->o.focus)
			a->kfocus->o.focus(a->kfocus, 1, a->kfocus->o.ud);
	}
}

static void kbd_leave(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *s)
{
	fl_app *a = data;
	fl_win *w = win_of(a, s);
	a->rep_key = 0;
	if (a->rep)
		fl_timer_arm(a->rep, 0, 0);
	if (w) {
		w->dirty = 1;
		if (w->o.focus)
			w->o.focus(w, 0, w->o.ud);
	}
	if (a->kfocus == w)
		a->kfocus = NULL;
}

static int cur_mods(fl_app *a)
{
	int m = 0;
	if (!a->xstate)
		return 0;
	if (xkb_state_mod_name_is_active(a->xstate, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0)
		m |= FL_MOD_SHIFT;
	if (xkb_state_mod_name_is_active(a->xstate, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0)
		m |= FL_MOD_CTRL;
	if (xkb_state_mod_name_is_active(a->xstate, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0)
		m |= FL_MOD_ALT;
	if (xkb_state_mod_name_is_active(a->xstate, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0)
		m |= FL_MOD_SUPER;
	return m;
}

int fl_app_mods(fl_app *a)
{
	return cur_mods(a);
}

/* Deliver one key press to the focused window. */
static void deliver_key(fl_app *a, uint32_t keycode)
{
	fl_win *w = a->kfocus;
	if (!w || !a->xstate)
		return;
	if (w->dead || !w->mapped) {
		/* a hidden window (the start menu) must not collect keys: the one that hid
		 * it may still be held, and its repeat would act on the menu's next opening */
		a->rep_key = 0;
		if (a->rep)
			fl_timer_arm(a->rep, 0, 0);
		return;
	}
	xkb_keycode_t kc = keycode + 8;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(a->xstate, kc);
	char utf8[16] = "";
	xkb_state_key_get_utf8(a->xstate, kc, utf8, sizeof utf8);
	int mods = cur_mods(a);
	char out[16];
	int n;
	if (mods & (FL_MOD_CTRL | FL_MOD_ALT)) {
		snprintf(out, sizeof out, "%s", utf8);
		n = (int)strlen(out);
	} else {
		n = fl_compose_feed(&a->compose, sym, utf8, out, sizeof out);
		if (n == 0 && sym >= XKB_KEY_dead_grave && sym <= XKB_KEY_dead_greek)
			return; /* a dead key: wait for the next one */
	}
	out[n] = 0;
	if (w->o.key && w->o.key(w, keycode, sym, out, mods, 1, w->o.ud))
		return;
	fl_ui *ui = &w->ui;
	ui->key = sym ? sym : 1;
	ui->mods = mods;
	ui->text[0] = 0;
	/* only printable text goes to widgets as text */
	if (!(mods & (FL_MOD_CTRL | FL_MOD_ALT)) && n && (unsigned char)out[0] >= 0x20 && out[0] != 0x7f)
		snprintf(ui->text, sizeof ui->text, "%s", out);
	render(w);
}

static void rep_fire(void *ud)
{
	fl_app *a = ud;
	if (a->rep && a->rep_key && a->kfocus)
		deliver_key(a, a->rep_key - 0);
	else if (a->rep) {
		a->rep_key = 0; /* nobody has the keyboard: no one to repeat to */
		fl_timer_arm(a->rep, 0, 0);
	}
}

static void kbd_key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t key,
                    uint32_t state)
{
	fl_app *a = data;
	a->serial = serial;
	if (!a->kfocus || !a->xstate)
		return;
	if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		deliver_key(a, key);
		/* the key may have closed or hidden the window; the focus is then already gone */
		if (a->kfocus && a->rep && a->rep_rate > 0 &&
		    xkb_keymap_key_repeats(a->keymap, key + 8)) {
			a->rep_key = key;
			fl_timer_arm(a->rep, a->rep_delay, 1000 / a->rep_rate);
		}
	} else {
		if (a->rep_key == key) {
			a->rep_key = 0;
			if (a->rep)
				fl_timer_arm(a->rep, 0, 0);
		}
		fl_win *w = a->kfocus;
		if (w && w->o.key)
			w->o.key(w, key, 0, "", cur_mods(a), 0, w->o.ud);
	}
}

static void kbd_modifiers(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t dep,
                          uint32_t lat, uint32_t lock, uint32_t group)
{
	fl_app *a = data;
	if (a->xstate)
		xkb_state_update_mask(a->xstate, dep, lat, lock, 0, 0, group);
}

static void kbd_repeat(void *data, struct wl_keyboard *k, int32_t rate, int32_t delay)
{
	fl_app *a = data;
	a->rep_rate = rate;
	a->rep_delay = delay;
}

static const struct wl_keyboard_listener kbd_listener = {
	.keymap = kbd_keymap, .enter = kbd_enter, .leave = kbd_leave, .key = kbd_key,
	.modifiers = kbd_modifiers, .repeat_info = kbd_repeat,
};

static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps)
{
	fl_app *a = data;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !a->ptr) {
		a->ptr = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(a->ptr, &pointer_listener, a);
	}
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !a->kbd) {
		a->kbd = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(a->kbd, &kbd_listener, a);
	}
}

static const struct wl_seat_listener seat_listener = {.capabilities = seat_caps, .name = NOOP};

/* ---- clipboard --------------------------------------------------------------------------- */

static int is_text_mime(const char *m)
{
	return !strcmp(m, "text/plain;charset=utf-8") || !strcmp(m, "text/plain") ||
	       !strcmp(m, "UTF8_STRING");
}

static void offer_offer(void *data, struct wl_data_offer *o, const char *mime)
{
	fl_app *a = data;
	if (o == a->pending && is_text_mime(mime)) {
		/* prefer utf-8 */
		if (!a->pending_text || !strcmp(mime, "text/plain;charset=utf-8"))
			a->sel_mime = !strcmp(mime, "text/plain;charset=utf-8") ? "text/plain;charset=utf-8"
			            : !strcmp(mime, "UTF8_STRING") ? "UTF8_STRING" : "text/plain";
		a->pending_text = 1;
	}
}

static const struct wl_data_offer_listener offer_listener = {
	.offer = offer_offer, .source_actions = NOOP, .action = NOOP,
};

static void dd_data_offer(void *data, struct wl_data_device *d, struct wl_data_offer *o)
{
	fl_app *a = data;
	if (a->pending && a->pending != a->sel)
		wl_data_offer_destroy(a->pending);
	a->pending = o;
	a->pending_text = 0;
	wl_data_offer_add_listener(o, &offer_listener, a);
}

static void dd_selection(void *data, struct wl_data_device *d, struct wl_data_offer *o)
{
	fl_app *a = data;
	if (a->sel && a->sel != o)
		wl_data_offer_destroy(a->sel);
	a->sel = o;
	a->sel_text = o && o == a->pending ? a->pending_text : 0;
	if (o && o == a->pending)
		a->pending = NULL;
}

static const struct wl_data_device_listener dd_listener = {
	.data_offer = dd_data_offer, .enter = NOOP, .leave = NOOP, .motion = NOOP, .drop = NOOP,
	.selection = dd_selection,
};

static void src_send(void *data, struct wl_data_source *s, const char *mime, int32_t fd)
{
	fl_app *a = data;
	size_t off = 0;
	while (a->clip && off < a->cliplen) {
		ssize_t n = write(fd, a->clip + off, a->cliplen - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		off += (size_t)n;
	}
	close(fd);
}

static void src_cancelled(void *data, struct wl_data_source *s)
{
	fl_app *a = data;
	if (a->own == s) {
		a->own = NULL;
		free(a->clip);
		a->clip = NULL;
		a->cliplen = 0;
	}
	wl_data_source_destroy(s);
}

static const struct wl_data_source_listener src_listener = {
	.target = NOOP, .send = src_send, .cancelled = src_cancelled, .dnd_drop_performed = NOOP,
	.dnd_finished = NOOP, .action = NOOP,
};

void fl_clipboard_set(fl_app *a, const char *text, size_t len)
{
	if (a->ppm || !a->ddm || !a->dd)
		return;
	char *copy = malloc(len + 1);
	if (!copy)
		return;
	memcpy(copy, text, len);
	copy[len] = 0;
	struct wl_data_source *old = a->own;
	free(a->clip);
	a->clip = copy;
	a->cliplen = len;
	a->own = wl_data_device_manager_create_data_source(a->ddm);
	wl_data_source_add_listener(a->own, &src_listener, a);
	wl_data_source_offer(a->own, "text/plain;charset=utf-8");
	wl_data_source_offer(a->own, "text/plain");
	wl_data_source_offer(a->own, "UTF8_STRING");
	wl_data_device_set_selection(a->dd, a->own, a->serial);
	if (old) {
		/* its cancelled event frees the text, so keep ours */
		a->clip = copy;
		wl_data_source_destroy(old);
	}
	wl_display_flush(a->dpy);
}

char *fl_clipboard_get(fl_app *a, size_t *len)
{
	*len = 0;
	if (a->ppm)
		return NULL;
	if (a->own && a->clip) {
		char *c = malloc(a->cliplen + 1);
		if (!c)
			return NULL;
		memcpy(c, a->clip, a->cliplen + 1);
		*len = a->cliplen;
		return c;
	}
	if (!a->sel || !a->sel_text || !a->sel_mime)
		return NULL;
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) < 0)
		return NULL;
	wl_data_offer_receive(a->sel, a->sel_mime, fds[1]);
	close(fds[1]);
	wl_display_flush(a->dpy);
	size_t cap = 4096, n = 0;
	char *buf = malloc(cap);
	for (;;) {
		struct pollfd p = {fds[0], POLLIN, 0};
		if (poll(&p, 1, 700) <= 0)
			break;
		if (n + 4096 > cap) {
			if (cap >= (1u << 20))
				break;
			cap *= 2;
			buf = realloc(buf, cap);
			if (!buf)
				break;
		}
		ssize_t r = read(fds[0], buf + n, cap - n - 1);
		if (r <= 0)
			break;
		n += (size_t)r;
	}
	close(fds[0]);
	if (!buf)
		return NULL;
	buf[n] = 0;
	*len = n;
	return buf;
}

/* ---- registry and connection ------------------------------------------------------------- */

static void out_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t r)
{
	fl_app *a = data;
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		a->out_w = w;
		a->out_h = h;
	}
}

static const struct wl_output_listener out_listener = {
	.geometry = NOOP, .mode = out_mode, .done = NOOP, .scale = NOOP, .name = NOOP, .description = NOOP,
};

static void wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{
	xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_listener = {.ping = wm_ping};

static uint32_t minu(uint32_t a, uint32_t b) { return a < b ? a : b; }

static void reg_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver)
{
	fl_app *a = data;
	if (!strcmp(iface, "wl_compositor")) {
		a->comp = wl_registry_bind(reg, name, &wl_compositor_interface, minu(ver, 4));
	} else if (!strcmp(iface, "wl_shm")) {
		a->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, "xdg_wm_base")) {
		a->wm = wl_registry_bind(reg, name, &xdg_wm_base_interface, minu(ver, 2));
		xdg_wm_base_add_listener(a->wm, &wm_listener, a);
	} else if (!strcmp(iface, "wl_seat") && !a->seat) {
		a->seat = wl_registry_bind(reg, name, &wl_seat_interface, minu(ver, 5));
		wl_seat_add_listener(a->seat, &seat_listener, a);
	} else if (!strcmp(iface, "wl_data_device_manager")) {
		a->ddm = wl_registry_bind(reg, name, &wl_data_device_manager_interface, minu(ver, 3));
	} else if (!strcmp(iface, "wl_output") && !a->out) {
		a->out = wl_registry_bind(reg, name, &wl_output_interface, minu(ver, 2));
		wl_output_add_listener(a->out, &out_listener, a);
	}
	if (a->global_cb)
		a->global_cb(a->global_ud, reg, name, iface, ver);
}

static const struct wl_registry_listener reg_listener = {.global = reg_global, .global_remove = NOOP};

fl_app *fl_app_new(fl_global_cb cb, void *ud)
{
	fl_app *a = calloc(1, sizeof(*a));
	if (!a)
		return NULL;
	a->global_cb = cb;
	a->global_ud = ud;
	a->out_w = 1280;
	a->out_h = 800;
	a->shape = -1;
	a->rep_rate = 25;
	a->rep_delay = 400;
	fl_conf_load();
	const char *ppm = getenv("FLUI_PPM");
	if (ppm && *ppm) {
		a->ppm = 1;
		a->ppm_path = ppm;
		return a;
	}
	a->dpy = wl_display_connect(NULL);
	if (!a->dpy) {
		free(a);
		return NULL;
	}
	a->xctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	set_default_keymap(a);
	a->reg = wl_display_get_registry(a->dpy);
	wl_registry_add_listener(a->reg, &reg_listener, a);
	wl_display_roundtrip(a->dpy);
	if (!a->comp || !a->shm || !a->wm) {
		fprintf(stderr, "flui: the compositor lacks wl_compositor, wl_shm or xdg_wm_base\n");
		fl_app_free(a);
		return NULL;
	}
	if (a->ddm && a->seat) {
		a->dd = wl_data_device_manager_get_data_device(a->ddm, a->seat);
		wl_data_device_add_listener(a->dd, &dd_listener, a);
	}
	wl_display_roundtrip(a->dpy);
	wl_display_roundtrip(a->dpy); /* seat capabilities, keymap, private globals' first events */
	a->rep = fl_timer_new(a, rep_fire, a);
	return a;
}

void fl_app_free(fl_app *a)
{
	if (!a)
		return;
	for (fl_win *w = a->wins; w; w = w->next)
		fl_win_destroy(w);
	reap(a);
	if (a->rep)
		fl_timer_free(a->rep);
	if (a->xstate)
		xkb_state_unref(a->xstate);
	if (a->keymap)
		xkb_keymap_unref(a->keymap);
	if (a->xctx)
		xkb_context_unref(a->xctx);
	if (a->dpy) {
		wl_display_flush(a->dpy);
		wl_display_disconnect(a->dpy);
	}
	free(a->clip);
	free(a->src);
	free(a);
}

struct wl_display *fl_app_display(fl_app *a) { return a->dpy; }
int fl_app_ppm(fl_app *a) { return a->ppm; }
void fl_app_quit(fl_app *a) { a->quit = 1; }

void fl_app_output_size(fl_app *a, int *w, int *h)
{
	*w = a->out_w;
	*h = a->out_h;
}

/* ---- fds, timers, the loop -------------------------------------------------------------------- */

int fl_fd_add(fl_app *a, int fd, short events, void (*cb)(void *, int, short), void *ud)
{
	if (a->nsrc == a->capsrc) {
		int nc = a->capsrc ? a->capsrc * 2 : 8;
		struct src *n = realloc(a->src, (size_t)nc * sizeof(*n));
		if (!n)
			return -1;
		a->src = n;
		a->capsrc = nc;
	}
	a->src[a->nsrc++] = (struct src){fd, events, cb, ud, 0};
	return 0;
}

void fl_fd_remove(fl_app *a, int fd)
{
	for (int i = 0; i < a->nsrc; i++)
		if (a->src[i].fd == fd)
			a->src[i].dead = 1;
}

static void timer_ready(void *ud, int fd, short rev)
{
	fl_timer *t = ud;
	uint64_t n;
	if (read(fd, &n, sizeof n) == sizeof n && t->cb)
		t->cb(t->ud);
}

fl_timer *fl_timer_new(fl_app *a, void (*cb)(void *), void *ud)
{
	fl_timer *t = calloc(1, sizeof(*t));
	if (!t)
		return NULL;
	t->fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (t->fd < 0) {
		free(t);
		return NULL;
	}
	t->app = a;
	t->cb = cb;
	t->ud = ud;
	fl_fd_add(a, t->fd, POLLIN, timer_ready, t);
	return t;
}

void fl_timer_arm(fl_timer *t, int first_ms, int every_ms)
{
	struct itimerspec its = {
		.it_value = {first_ms / 1000, (first_ms % 1000) * 1000000L},
		.it_interval = {every_ms / 1000, (every_ms % 1000) * 1000000L},
	};
	timerfd_settime(t->fd, 0, &its, NULL);
}

void fl_timer_free(fl_timer *t)
{
	if (!t)
		return;
	fl_fd_remove(t->app, t->fd);
	/* the fd is closed when the dead source is swept */
	t->cb = NULL;
	struct itimerspec off = {0};
	timerfd_settime(t->fd, 0, &off, NULL);
	close(t->fd);
	free(t);
}

static void sweep(fl_app *a)
{
	int j = 0;
	for (int i = 0; i < a->nsrc; i++)
		if (!a->src[i].dead)
			a->src[j++] = a->src[i];
	a->nsrc = j;
}

static void flush_dirty(fl_app *a)
{
	for (fl_win *w = a->wins; w; w = w->next)
		if (w->dirty && !w->dead)
			render(w);
	reap(a);
}

static void ppm_write(fl_win *w, uint32_t *px)
{
	const char *base = w->app->ppm_path;
	char path[600];
	if (w->index == 0)
		snprintf(path, sizeof path, "%s", base);
	else
		snprintf(path, sizeof path, "%s.%d", base, w->index);
	FILE *f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "flui: cannot write %s\n", path);
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", w->w, w->h);
	for (int i = 0; i < w->w * w->h; i++) {
		unsigned char rgb[3] = {px[i] >> 16, px[i] >> 8, px[i]};
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

static void ppm_run(fl_app *a)
{
	const char *d = getenv("FLUI_PPM_DELAY");
	int delay = d ? atoi(d) : 0;
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (!a->quit) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		long left = delay - ((now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000);
		if (left <= 0)
			break;
		struct pollfd *pf = calloc((size_t)a->nsrc + 1, sizeof(*pf));
		for (int i = 0; i < a->nsrc; i++) {
			pf[i].fd = a->src[i].fd;
			pf[i].events = a->src[i].events;
		}
		int n = poll(pf, (nfds_t)a->nsrc, (int)left);
		if (n > 0) {
			int ns = a->nsrc;
			for (int i = 0; i < ns; i++)
				if (pf[i].revents && !a->src[i].dead)
					a->src[i].cb(a->src[i].ud, a->src[i].fd, pf[i].revents);
		}
		free(pf);
		sweep(a);
	}
	for (fl_win *w = a->wins; w; w = w->next) {
		if (w->dead)
			continue;
		uint32_t *px = calloc((size_t)w->w * w->h, 4);
		if (!px)
			continue;
		run_pass(w, px);
		ppm_write(w, px);
		free(px);
	}
}

int fl_app_quitting(const fl_app *a) { return a->quit; }

int fl_app_prepare(fl_app *a, struct pollfd *pf, int max)
{
	flush_dirty(a);
	while (wl_display_prepare_read(a->dpy) != 0)
		wl_display_dispatch_pending(a->dpy);
	int need_out = 0;
	if (wl_display_flush(a->dpy) < 0) {
		if (errno == EAGAIN)
			need_out = 1;
		else {
			wl_display_cancel_read(a->dpy);
			return -1;
		}
	}
	if (a->nsrc + 1 > max) { /* a draw or an event handler added an fd after the caller counted */
		wl_display_cancel_read(a->dpy);
		return -2;
	}
	pf[0].fd = wl_display_get_fd(a->dpy);
	pf[0].events = POLLIN | (need_out ? POLLOUT : 0);
	pf[0].revents = 0;
	for (int i = 0; i < a->nsrc; i++) {
		pf[i + 1].fd = a->src[i].fd;
		pf[i + 1].events = a->src[i].events;
		pf[i + 1].revents = 0;
	}
	return a->nsrc + 1;
}

int fl_app_dispatch(fl_app *a, const struct pollfd *pf, int n, int r)
{
	if (r < 0 && errno != EINTR) {
		wl_display_cancel_read(a->dpy);
		return -1;
	}
	if (r > 0 && (pf[0].revents & POLLIN)) {
		if (wl_display_read_events(a->dpy) < 0)
			return -1;
	} else {
		wl_display_cancel_read(a->dpy);
	}
	if (r > 0 && (pf[0].revents & (POLLERR | POLLHUP)) && !(pf[0].revents & POLLIN))
		return -1;
	if (wl_display_dispatch_pending(a->dpy) < 0)
		return -1;
	for (int i = 0; i < n - 1 && r > 0; i++)
		if (pf[i + 1].revents && !a->src[i].dead)
			a->src[i].cb(a->src[i].ud, a->src[i].fd, pf[i + 1].revents);
	sweep(a);
	return 0;
}

void fl_app_run(fl_app *a)
{
	if (a->ppm) {
		ppm_run(a);
		return;
	}
	while (!a->quit) {
		/* prepare draws the dirty windows and dispatches pending events, and either may add an fd
		 * (the panel starts a wpctl from the volume slider's draw): -2 means "more room, please" */
		int cap = a->nsrc + 1, n;
		struct pollfd *pf = NULL;
		do {
			free(pf);
			pf = calloc((size_t)cap, sizeof(*pf));
			if (!pf)
				break;
			n = fl_app_prepare(a, pf, cap);
			cap = a->nsrc + 1;
		} while (n == -2);
		if (!pf || n < 0) {
			free(pf);
			break;
		}
		int r = poll(pf, (nfds_t)n, -1);
		int bad = fl_app_dispatch(a, pf, n, r);
		free(pf);
		if (bad)
			break;
	}
	wl_display_flush(a->dpy);
}
