/* anim.c -- short, flat, eased motion: window open and close, minimize and restore towards the
 * panel entry, maximize and snap, the workspace slide, the overview's zoom, and the fade of the
 * start menu, flyouts and notifications.
 *
 * Rules (the FreeLFS look): no wobble, no bounce, no overshoot. 120 to 200 ms (before the speed
 * factor), an ease-out curve for what appears and moves, ease-in for what leaves, one smooth
 * in-out for the slides. Scaling is slight (96 %), except where something travels (minimize).
 *
 * Settings (desktop.conf, read by config.c, applied live):
 *   animations=on|reduced|off   reduced: fades only (no scale, no slide, no zoom, no geometry
 *                               animation); off: nothing moves, the code below never runs
 *   animation_speed=1.0         factor on how fast (0.1 .. 4), 2 means half the time
 *
 * How it costs (flcomp composites on the CPU into one shadow buffer, there is no GL compositing
 * path: GLES is only used to import GPU clients' dmabufs):
 *   - nothing is scheduled while idle. An animation exists from its start until its last frame;
 *     frame_done() (the page flip, or a 16 ms timer in the headless backend) calls anim_frame(),
 *     which advances every animation by the clock, damages exactly what moved, and so asks for
 *     the next frame. No timers of its own, no wakeups after the last frame.
 *   - only the rectangles that move are repainted. A window that is not scaled is copied with
 *     the same row loops as ever (a slide or a pure fade never samples), a scaled one is
 *     sampled bilinearly. A window bigger than 1.2 megapixels is faded without scaling.
 *   - pictures (the last frame of a closing window, a hiding menu) are copies of the surface,
 *     freed with the animation; the overview is drawn once off screen and then only blitted. They
 *     live for at most about 200 ms; at idle there is nothing.
 *   - time based: a slow frame skips ahead instead of slowing the motion down.
 * Not animated: X11 windows (their frame is drawn separately by xwdeco.c), a window going or
 * coming while a fullscreen window is drawn alone (performance mode), and anything while the
 * screen is locked.
 *
 * FLCOMP_ANIM_LOG=1 prints, after each burst of motion, how many frames it took, the interval
 * between frames and the time spent drawing (tests/anim.sh reads that).
 * SPDX-License-Identifier: MIT */
#include "comp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { K_WIN, K_WIN_GHOST, K_SURF, K_SURF_GHOST, K_WS, K_OVERVIEW };
enum { E_OUT, E_IN, E_INOUT };

#define MAX_ANIM 24
#define BIG_WINDOW (1200 * 1000)	/* pixels: above this a window fades without scaling */

struct anim {
	bool used;
	int kind;
	struct window *win;
	struct surface *surf;
	uint32_t *px;			/* ghosts: a copy of the tree, premultiplied ARGB */
	int pw, ph;
	struct rect pbounds, pgeom;	/* where the copy was: its bounds, and (window) the geometry */
	bool palpha;
	struct rect from, to;		/* window: geometry; surface and overview: bounds */
	int a0, a1;			/* opacity 0..255 */
	uint32_t t0, dur;
	int ease;
	bool hidden;			/* drawn although window_visible() is false (minimize) */
	/* state of the last step */
	struct rect cur;
	int cur_a;
	struct rect dmg;		/* what this animation covered when last drawn */
	/* workspace slide */
	int from_ws, to_ws, dirx, diry, off_x, off_y;
	bool fade_only;
};

static struct anim A[MAX_ANIM];
static int nactive;
static uint32_t *ov_buf;	/* the overview, drawn once while it zooms */

/* ---- statistics (FLCOMP_ANIM_LOG) ------------------------------------------------------------------ */

static bool logging;
static uint32_t st_t0;
static int st_frames, st_missed;
static double st_int_sum, st_int_max, st_draw_sum, st_draw_max;

static double mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static double draw_t0;

void anim_draw_begin(void)
{
	if (logging && nactive)
		draw_t0 = mono_ms();
}

void anim_draw_end(void)
{
	if (!logging || !nactive)
		return;
	double now = mono_ms(), d = now - draw_t0;
	static double last;
	if (st_frames > 0) {
		double iv = now - last;
		st_int_sum += iv;
		if (iv > st_int_max)
			st_int_max = iv;
		double nominal = C.refresh > 0 ? 1000000.0 / C.refresh : 16.7;
		int gap = anim_gap_ms();
		if (gap > nominal)
			nominal = gap;
		if (iv > nominal * 1.5 + 1.0)
			st_missed++;
	}
	last = now;
	st_frames++;
	st_draw_sum += d;
	if (d > st_draw_max)
		st_draw_max = d;
}

static void stats_flush(const char *why)
{
	if (logging && st_frames > 1)
		fprintf(stderr, "anim: %s: %d frames in %u ms, interval avg %.1f max %.1f ms, drawing avg %.2f max %.2f ms, missed %d\n",
			why, st_frames, now_ms() - st_t0, st_int_sum / (st_frames - 1), st_int_max,
			st_draw_sum / st_frames, st_draw_max, st_missed);
	st_frames = st_missed = 0;
	st_int_sum = st_int_max = st_draw_sum = st_draw_max = 0;
}

/* ---- settings ---------------------------------------------------------------------------------------- */

static bool enabled(void)
{
	return CF.anim != ANIM_OFF && !C.locked && !perf_unredirected();
}

static bool fades_only(void)
{
	return CF.anim == ANIM_REDUCED;
}

static uint32_t scaled_ms(uint32_t base)
{
	uint32_t d = base * 100 / (uint32_t)(CF.anim_speed > 0 ? CF.anim_speed : 100);
	return d < 16 ? 16 : d;
}

void anim_init(void)
{
	logging = getenv("FLCOMP_ANIM_LOG") != NULL || CF.anim_log;
}

bool anim_active(void)
{
	return nactive > 0;
}

int anim_gap_ms(void)
{
	if (!nactive)
		return 0;
	/* the headless backend has no vblank: pace it like a 60 Hz display. A real flip waits for the
	 * vblank by itself, but a virtual GPU (QEMU) completes it at once: without a floor the motion
	 * would redraw 250 times a second. One millisecond under the refresh interval, so no vblank is skipped. */
	if (C.headless)
		return 16;
	int ms = C.refresh > 0 ? (int)(1000000 / C.refresh) : 16;
	return ms > 3 ? ms - 1 : 0;
}

/* ---- geometry helpers -------------------------------------------------------------------------------- */

static int lerpi(int a, int b, int e1024)
{
	return a + (int)((long)(b - a) * e1024 / 1024);
}

static struct rect lerp_rect(struct rect a, struct rect b, int e)
{
	return (struct rect){ lerpi(a.x1, b.x1, e), lerpi(a.y1, b.y1, e), lerpi(a.x2, b.x2, e), lerpi(a.y2, b.y2, e) };
}

static int ease(int kind, uint32_t t0, uint32_t dur, uint32_t now, bool *done)
{
	double t = dur ? (double)(now - t0) / dur : 1.0;
	if (t < 0)
		t = 0;
	*done = t >= 1.0;
	if (t > 1.0)
		t = 1.0;
	double e;
	switch (kind) {
	case E_IN:
		e = t * t;
		break;
	case E_INOUT:
		e = t * t * (3 - 2 * t);
		break;
	default:
		e = 1 - (1 - t) * (1 - t) * (1 - t);
	}
	return (int)(e * 1024 + 0.5);
}

/* the point r.x maps to when geometry g becomes gp */
static struct rect map_rect(struct rect r, struct rect g, struct rect gp)
{
	long gw = rect_w(g) > 0 ? rect_w(g) : 1, gh = rect_h(g) > 0 ? rect_h(g) : 1;
	long pw = rect_w(gp), ph = rect_h(gp);
	struct rect o = {
		gp.x1 + (int)(((long)(r.x1 - g.x1) * pw + (pw >= 0 ? gw / 2 : 0)) / gw),
		gp.y1 + (int)(((long)(r.y1 - g.y1) * ph + gh / 2) / gh),
		gp.x1 + (int)(((long)(r.x2 - g.x1) * pw + gw / 2) / gw),
		gp.y1 + (int)(((long)(r.y2 - g.y1) * ph + gh / 2) / gh),
	};
	return o;
}

static struct rect shrunk(struct rect g, int pct)
{
	int w = rect_w(g), h = rect_h(g), nw = w * pct / 100, nh = h * pct / 100;
	int cx = (g.x1 + g.x2) / 2, cy = (g.y1 + g.y2) / 2;
	return (struct rect){ cx - nw / 2, cy - nh / 2, cx - nw / 2 + nw, cy - nh / 2 + nh };
}

static struct rect shifted(struct rect r, int dx, int dy)
{
	return (struct rect){ r.x1 + dx, r.y1 + dy, r.x2 + dx, r.y2 + dy };
}

/* ---- the sampler ------------------------------------------------------------------------------------- */

static inline uint32_t lerp_px(uint32_t a, uint32_t b, unsigned t)	/* t 0..256 */
{
	uint32_t rb = (((a & 0x00ff00ffu) * (256 - t) + (b & 0x00ff00ffu) * t) >> 8) & 0x00ff00ffu;
	uint32_t ag = (((a >> 8 & 0x00ff00ffu) * (256 - t) + (b >> 8 & 0x00ff00ffu) * t)) & 0xff00ff00u;
	return rb | ag;
}

/* premultiplied (or opaque) pixel s with extra opacity g (0..256) over an opaque destination */
static inline uint32_t over(uint32_t s, unsigned g, uint32_t d)
{
	if (g < 256) {
		uint32_t rb = ((s & 0x00ff00ffu) * g >> 8) & 0x00ff00ffu;
		uint32_t ag = ((s >> 8 & 0x00ff00ffu) * g) & 0xff00ff00u;
		s = rb | ag;
	}
	unsigned ia = 255 - (s >> 24);
	if (ia == 255)
		return d;
	uint32_t rb = (d & 0x00ff00ffu) * ia;
	rb = ((rb + 0x00800080u + ((rb >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
	uint32_t gg = (d & 0x0000ff00u) * ia;
	gg = ((gg + 0x00008000u + ((gg >> 8) & 0x0000ff00u)) >> 8) & 0x0000ff00u;
	return 0xff000000u | ((s & 0x00ffffffu) + rb + gg);
}

/* The picture px (sw x sh, premultiplied when alpha, else opaque) drawn into dst with opacity
 * g (0..255), clipped to clip. Same size: a straight copy; otherwise bilinear. */
static void blit_xf(const uint32_t *px, int sw, int sh, bool alpha, struct rect dst, int g, struct rect clip)
{
	struct rect r;
	int dw = rect_w(dst), dh = rect_h(dst);
	if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0 || !rect_intersect(dst, clip, &r) || g <= 0)
		return;
	unsigned gg = (unsigned)g + (g >> 7);	/* 0..256 */
	if (dw == sw && dh == sh) {
		for (int y = r.y1; y < r.y2; y++) {
			uint32_t *d = C.shadow + (size_t)y * C.w + r.x1;
			const uint32_t *s = px + (size_t)(y - dst.y1) * sw + (r.x1 - dst.x1);
			int n = r.x2 - r.x1;
			if (!alpha && g >= 255) {
				memcpy(d, s, (size_t)n * 4);
			} else if (!alpha) {
				for (int i = 0; i < n; i++)
					d[i] = lerp_px(d[i], s[i], gg) | 0xff000000u;
			} else {
				for (int i = 0; i < n; i++)
					d[i] = over(s[i], gg, d[i]);
			}
		}
		return;
	}
	/* bilinear, 16.16, pixel centres */
	int nx = r.x2 - r.x1;
	if (nx > 8192)
		return;
	int xi0[8192], xi1[8192];
	uint8_t xfr[8192];
	for (int i = 0; i < nx; i++) {
		long sx = (((long)(r.x1 + i - dst.x1) * 2 + 1) * sw * 32768L) / dw - 32768L;
		if (sx < 0)
			sx = 0;
		int x0 = (int)(sx >> 16);
		if (x0 >= sw - 1) {
			x0 = sw - 1;
			xi1[i] = x0;
			xfr[i] = 0;
		} else {
			xi1[i] = x0 + 1;
			xfr[i] = (uint8_t)((sx >> 8) & 255);
		}
		xi0[i] = x0;
	}
	for (int y = r.y1; y < r.y2; y++) {
		long sy = (((long)(y - dst.y1) * 2 + 1) * sh * 32768L) / dh - 32768L;
		if (sy < 0)
			sy = 0;
		int y0 = (int)(sy >> 16), y1;
		unsigned fy;
		if (y0 >= sh - 1) {
			y0 = y1 = sh - 1;
			fy = 0;
		} else {
			y1 = y0 + 1;
			fy = (unsigned)((sy >> 8) & 255);
		}
		const uint32_t *r0 = px + (size_t)y0 * sw, *r1 = px + (size_t)y1 * sw;
		uint32_t *d = C.shadow + (size_t)y * C.w + r.x1;
		for (int i = 0; i < nx; i++) {
			unsigned fx = xfr[i];
			uint32_t top = lerp_px(r0[xi0[i]], r0[xi1[i]], fx), bot = lerp_px(r1[xi0[i]], r1[xi1[i]], fx);
			uint32_t s = fy ? lerp_px(top, bot, fy) : top;
			if (!alpha)
				d[i] = (g >= 255 ? s : lerp_px(d[i], s, gg)) | 0xff000000u;
			else
				d[i] = over(s, gg, d[i]);
		}
	}
}

/* a surface tree (root and subsurfaces) drawn with geometry g shown as gp */
static void draw_tree_xf(struct surface *s, struct rect g, struct rect gp, int a, struct rect clip)
{
	if (!s->px)
		return;
	struct rect sr = surface_rect(s);
	blit_xf(s->px, s->w, s->h, s->alpha, map_rect(sr, g, gp), a, clip);
	struct subsurf *sub;
	wl_list_for_each(sub, &s->subs, link)
		draw_tree_xf(sub->surf, g, gp, a, clip);
}

/* ---- pictures of surface trees ----------------------------------------------------------------------- */

static void flatten_into(uint32_t *buf, struct rect b, struct surface *s)
{
	if (!s->px)
		return;
	struct rect sr = surface_rect(s), r;
	if (rect_intersect(sr, b, &r)) {
		int bw = rect_w(b);
		for (int y = r.y1; y < r.y2; y++) {
			uint32_t *d = buf + (size_t)(y - b.y1) * bw + (r.x1 - b.x1);
			const uint32_t *p = s->px + (size_t)(y - sr.y1) * s->w + (r.x1 - sr.x1);
			for (int i = 0; i < r.x2 - r.x1; i++) {
				uint32_t v = s->alpha ? p[i] : p[i] | 0xff000000u;
				unsigned ia = 255 - (v >> 24);
				if (ia == 255) {
					continue;
				} else if (ia == 0) {
					d[i] = v;
				} else {
					uint32_t rb = ((d[i] & 0x00ff00ffu) * ia + 0x00800080u) >> 8 & 0x00ff00ffu;
					uint32_t ag = (((d[i] >> 8) & 0x00ff00ffu) * ia + 0x00800080u) & 0xff00ff00u;
					d[i] = v + (rb | ag);
				}
			}
		}
	}
	struct subsurf *sub;
	wl_list_for_each(sub, &s->subs, link)
		flatten_into(buf, b, sub->surf);
}

static bool snapshot(struct anim *a, struct surface *s)
{
	struct rect b = tree_bounds(s);
	int w = rect_w(b), h = rect_h(b);
	if (w <= 0 || h <= 0 || (long)w * h > 3000L * 2000L)
		return false;
	a->px = calloc((size_t)w * h, 4);
	if (!a->px)
		return false;
	flatten_into(a->px, b, s);
	a->pw = w;
	a->ph = h;
	a->pbounds = b;
	a->palpha = true;
	return true;
}

/* ---- the list ---------------------------------------------------------------------------------------- */

static struct anim *new_anim(int kind)
{
	for (int i = 0; i < MAX_ANIM; i++)
		if (!A[i].used) {
			memset(&A[i], 0, sizeof A[i]);
			A[i].used = true;
			A[i].kind = kind;
			if (!nactive) {
				st_t0 = now_ms();
				st_frames = st_missed = 0;
			}
			nactive++;
			return &A[i];
		}
	return NULL;
}

static void drop(struct anim *a)
{
	if (!a->used)
		return;
	free(a->px);
	if (a->kind == K_OVERVIEW) {
		free(ov_buf);
		ov_buf = NULL;
	}
	a->used = false;
	if (--nactive == 0)
		stats_flush("done");
}

/* the rectangle in desktop coordinates this animation draws into now */
static struct rect area_of(struct anim *a)
{
	switch (a->kind) {
	case K_WIN:
		if (a->win && a->win->surf && a->win->surf->px)
			return map_rect(tree_bounds(a->win->surf), window_rect(a->win), a->cur);
		return a->cur;
	case K_WIN_GHOST:
		return map_rect(a->pbounds, a->pgeom, a->cur);
	default:
		return a->cur;
	}
}

static void step(struct anim *a, uint32_t now)
{
	bool done;
	int e = ease(a->ease, a->t0, a->dur, now, &done);
	struct rect before = a->dmg;
	switch (a->kind) {
	case K_WS: {
		/* outgoing windows leave along the move, incoming ones arrive from the other side */
		struct rect wa = work_area();
		int ox = a->dirx * (rect_w(wa) * e / 1024), oy = a->diry * (rect_h(wa) * e / 1024);
		int px = a->off_x, py = a->off_y;
		a->off_x = fades_only() ? 0 : ox;
		a->off_y = fades_only() ? 0 : oy;
		a->cur_a = e;
		struct window *w;
		wl_list_for_each(w, &C.windows, link) {
			if (!w->mapped || w->minimized || w->xw || !w->surf || !w->surf->px)
				continue;
			struct rect b = tree_bounds(w->surf);
			if (w->ws == a->from_ws) {
				damage_add(shifted(b, -px, -py));
				damage_add(shifted(b, -a->off_x, -a->off_y));
			} else if (w->ws == a->to_ws) {
				damage_add(shifted(b, a->dirx * rect_w(wa) - px, a->diry * rect_h(wa) - py));
				damage_add(shifted(b, a->dirx * rect_w(wa) - a->off_x, a->diry * rect_h(wa) - a->off_y));
				damage_add(b);
			}
		}
		if (done)
			damage_add(wa);
		break;
	}
	case K_OVERVIEW:
		a->cur = lerp_rect(a->from, a->to, e);
		a->cur_a = lerpi(a->a0, a->a1, e);
		damage_add(work_area());	/* the picture is clipped to the work area, and zooms past it */
		break;
	default: {
		a->cur = lerp_rect(a->from, a->to, e);
		a->cur_a = lerpi(a->a0, a->a1, e);
		struct rect now_area = area_of(a);
		if (before.x2 > before.x1)
			damage_add(before);
		damage_add(now_area);
		a->dmg = now_area;
		if (done) {
			a->cur = a->to;
			damage_add(area_of(a));
		}
	}
	}
	if (done)
		drop(a);
}

void anim_frame(void)
{
	if (!nactive)
		return;
	uint32_t now = now_ms();
	for (int i = 0; i < MAX_ANIM; i++)
		if (A[i].used)
			step(&A[i], now);
	if (nactive)
		schedule_repaint();
}

static void kick(struct anim *a)
{
	/* the first frame shows the start state: step at t0 */
	step(a, a->t0);
	schedule_repaint();
}

void anim_conf_changed(void)
{
	logging = getenv("FLCOMP_ANIM_LOG") != NULL || CF.anim_log;
	if (CF.anim != ANIM_OFF)
		return;
	if (nactive) {
		for (int i = 0; i < MAX_ANIM; i++)
			if (A[i].used)
				drop(&A[i]);
		damage_all();
	}
}

static struct anim *find_win(struct window *w)
{
	for (int i = 0; i < MAX_ANIM; i++)
		if (A[i].used && A[i].kind == K_WIN && A[i].win == w)
			return &A[i];
	return NULL;
}

/* ---- windows ----------------------------------------------------------------------------------------- */

static bool window_ok(struct window *w)
{
	return enabled() && w && !w->xw && w->surf && w->surf->px && rect_w(window_rect(w)) > 0 && rect_h(window_rect(w)) > 0;
}

static struct anim *win_anim(struct window *w, struct rect from, int a0, struct rect to, int a1, uint32_t base_ms, int e,
			     bool hidden)
{
	struct anim *old = find_win(w);
	if (old) {	/* take over from where it is */
		from = old->cur;
		a0 = old->cur_a;
		drop(old);
	}
	struct anim *a = new_anim(K_WIN);
	if (!a)
		return NULL;
	a->win = w;
	a->from = from;
	a->to = to;
	a->a0 = a0;
	a->a1 = a1;
	a->t0 = now_ms();
	a->dur = scaled_ms(base_ms);
	a->ease = e;
	a->hidden = hidden;
	a->cur = from;
	a->cur_a = a0;
	kick(a);
	return a;
}

static bool big(struct rect g)
{
	return (long)rect_w(g) * rect_h(g) > BIG_WINDOW;
}

void anim_window_open(struct window *w)
{
	if (!window_ok(w) || w->ws != C.ws_cur)
		return;
	struct rect g = window_rect(w);
	win_anim(w, fades_only() || big(g) ? g : shrunk(g, 96), 0, g, 255, 160, E_OUT, false);
}

void anim_window_closing(struct window *w)
{
	if (!window_ok(w) || w->minimized || !window_visible(w))
		return;
	struct anim *old = find_win(w);
	struct rect g = old ? old->cur : window_rect(w);
	int a0 = old ? old->cur_a : 255;
	if (old)
		drop(old);
	struct anim *a = new_anim(K_WIN_GHOST);
	if (!a)
		return;
	if (!snapshot(a, w->surf)) {
		drop(a);
		return;
	}
	a->pgeom = window_rect(w);
	a->from = g;
	a->to = fades_only() || big(g) ? g : shrunk(g, 96);
	a->a0 = a0;
	a->a1 = 0;
	a->t0 = now_ms();
	a->dur = scaled_ms(120);
	a->ease = E_IN;
	a->cur = g;
	a->cur_a = a0;
	a->dmg = (struct rect){ 0, 0, 0, 0 };
	kick(a);
}

void anim_window_gone(struct window *w)
{
	for (int i = 0; i < MAX_ANIM; i++)
		if (A[i].used && A[i].win == w && A[i].kind == K_WIN) {
			if (A[i].dmg.x2 > A[i].dmg.x1)
				damage_add(A[i].dmg);
			drop(&A[i]);
		}
}

/* where the panel shows the window, in desktop coordinates; without a hint: a spot at the panel's middle */
static struct rect entry_rect(struct window *w)
{
	int px = 0, py = 0;
	if (C.panel)
		surface_origin(C.panel, &px, &py);
	else
		py = CF.panel_top ? 0 : C.h - C.panel_h;
	if (w->has_entry)
		return shifted(w->entry, px, py);
	return (struct rect){ C.w / 2 - 24, py + 4, C.w / 2 + 24, py + (C.panel_h > 8 ? C.panel_h - 4 : 4) };
}

void anim_window_minimize(struct window *w)
{
	if (!window_ok(w) || w->ws != C.ws_cur)
		return;
	struct rect g = window_rect(w), t = entry_rect(w);
	if (fades_only())
		t = g;
	win_anim(w, g, 255, t, 0, 180, E_IN, true);
}

void anim_window_restore(struct window *w)
{
	if (!window_ok(w))
		return;
	struct rect g = window_rect(w), t = entry_rect(w);
	if (fades_only())
		t = g;
	win_anim(w, t, 0, g, 255, 180, E_OUT, false);
}

void anim_window_geometry(struct window *w, struct rect from)
{
	if (!window_ok(w) || fades_only() || w->fullscreen || w->ws != C.ws_cur || w->minimized)
		return;
	/* the new geometry: the size asked for, at the new place (the client follows in a moment) */
	struct rect g = window_rect(w);
	struct rect to = g;
	if (w->cfg_w > 0 && w->cfg_h > 0)
		to = (struct rect){ w->x, w->y, w->x + w->cfg_w, w->y + w->cfg_h };
	if (!memcmp(&from, &to, sizeof from) || rect_w(from) <= 0 || big(to))
		return;
	win_anim(w, from, 255, to, 255, 180, E_OUT, false);
}

bool anim_window_xf(struct window *w, struct anim_xf *xf)
{
	if (w->xw)
		return false;
	struct anim *a = find_win(w);
	if (a) {
		xf->dst = a->cur;
		xf->alpha = a->cur_a;
		return true;
	}
	for (int i = 0; i < MAX_ANIM; i++)
		if (A[i].used && A[i].kind == K_WS && (w->ws == A[i].from_ws || w->ws == A[i].to_ws)) {
			struct rect g = window_rect(w);
			int sw = rect_w(work_area());
			int sh = rect_h(work_area());
			if (w->ws == A[i].from_ws) {
				xf->dst = shifted(g, -A[i].off_x, -A[i].off_y);
				xf->alpha = fades_only() ? 255 - A[i].cur_a * 255 / 1024 : 255;
			} else {
				xf->dst = shifted(g, A[i].dirx * sw - A[i].off_x, A[i].diry * sh - A[i].off_y);
				xf->alpha = fades_only() ? A[i].cur_a * 255 / 1024 : 255;
			}
			return true;
		}
	return false;
}

bool anim_window_drawn_hidden(struct window *w)
{
	if (w->xw || !w->mapped)
		return false;
	struct anim *a = find_win(w);
	if (a && a->hidden)
		return true;
	for (int i = 0; i < MAX_ANIM; i++)
		if (A[i].used && A[i].kind == K_WS && w->ws == A[i].from_ws && !w->minimized)
			return true;
	return false;
}

/* draws the window (all its subsurfaces) through the animation, true when it did */
bool anim_draw_window(struct window *w, struct rect clip)
{
	struct anim_xf xf;
	if (!anim_window_xf(w, &xf))
		return false;
	draw_tree_xf(w->surf, window_rect(w), xf.dst, xf.alpha, clip);
	return true;
}

void anim_draw_win_ghosts(struct rect clip)
{
	for (int i = 0; i < MAX_ANIM; i++)
		if (A[i].used && A[i].kind == K_WIN_GHOST && A[i].px)
			blit_xf(A[i].px, A[i].pw, A[i].ph, true, map_rect(A[i].pbounds, A[i].pgeom, A[i].cur), A[i].cur_a, clip);
}

/* ---- workspaces ------------------------------------------------------------------------------------- */

bool anim_ws_active(void)
{
	for (int i = 0; i < MAX_ANIM; i++)
		if (A[i].used && A[i].kind == K_WS)
			return true;
	return false;
}

void anim_ws_switch(int from, int to)
{
	if (!enabled() || overview_on() || anim_overview_busy() || from == to)
		return;
	/* one slide at a time: the old one ends where it is */
	for (int i = 0; i < MAX_ANIM; i++)
		if (A[i].used && A[i].kind == K_WS) {
			drop(&A[i]);
			damage_add(work_area());
		}
	bool any = false;
	struct window *w;
	wl_list_for_each(w, &C.windows, link)
		if (w->mapped && !w->minimized && !w->xw && (w->ws == from || w->ws == to))
			any = true;
	if (!any)
		return;
	struct anim *a = new_anim(K_WS);
	if (!a)
		return;
	int dx = (to % C.ws_cols) - (from % C.ws_cols), dy = (to / C.ws_cols) - (from / C.ws_cols);
	a->dirx = dx > 0 ? 1 : dx < 0 ? -1 : 0;
	a->diry = dy > 0 ? 1 : dy < 0 ? -1 : 0;
	a->from_ws = from;
	a->to_ws = to;
	a->t0 = now_ms();
	a->dur = scaled_ms(200);
	a->ease = E_INOUT;
	kick(a);
}

/* ---- the overview ----------------------------------------------------------------------------------- */

bool anim_overview_busy(void)
{
	for (int i = 0; i < MAX_ANIM; i++)
		if (A[i].used && A[i].kind == K_OVERVIEW)
			return true;
	return false;
}

void anim_overview(bool opening)
{
	if (!enabled())
		return;
	for (int i = 0; i < MAX_ANIM; i++)
		if (A[i].used && A[i].kind == K_OVERVIEW)
			drop(&A[i]);
	struct rect wa = work_area();
	size_t n = (size_t)C.w * rect_h(wa);
	ov_buf = malloc(n * 4);
	if (!ov_buf)
		return;
	/* one picture of the overview; C.shadow points there while it is drawn (rows from wa.y1) */
	uint32_t *keep = C.shadow;
	C.shadow = ov_buf - (size_t)wa.y1 * C.w;
	overview_draw_snapshot(wa);
	C.shadow = keep;
	struct anim *a = new_anim(K_OVERVIEW);
	if (!a) {
		free(ov_buf);
		ov_buf = NULL;
		return;
	}
	struct rect z = fades_only() ? wa : shrunk(wa, 108);
	a->from = opening ? z : wa;
	a->to = opening ? wa : z;
	a->a0 = opening ? 0 : 255;
	a->a1 = opening ? 255 : 0;
	a->t0 = now_ms();
	a->dur = scaled_ms(opening ? 180 : 150);
	a->ease = opening ? E_OUT : E_IN;
	a->cur = a->from;
	a->cur_a = a->a0;
	a->hidden = opening;
	/* the zoom is clipped to the work area: the picture is bigger than it */
	kick(a);
}

bool anim_draw_overview(struct rect clip)
{
	for (int i = 0; i < MAX_ANIM; i++)
		if (A[i].used && A[i].kind == K_OVERVIEW && ov_buf) {
			struct rect wa = work_area(), c;
			if (rect_intersect(clip, wa, &c))
				blit_xf(ov_buf, C.w, rect_h(wa), false, A[i].cur, A[i].cur_a, c);
			return true;
		}
	return false;
}

/* ---- the shell's surfaces: menu, flyout, notifications ------------------------------------------------ */

static struct anim *find_surf(struct surface *s, int kind)
{
	for (int i = 0; i < MAX_ANIM; i++)
		if (A[i].used && A[i].surf == s && A[i].kind == kind)
			return &A[i];
	return NULL;
}

/* how far a surface travels: the menu and flyout come from the panel's edge, notifications from the right */
static void surf_offset(struct surface *s, int *dx, int *dy, bool leaving)
{
	int d = leaving ? 6 : 12;
	*dx = *dy = 0;
	if (fades_only())
		return;
	if (s->role == ROLE_NOTIFY)
		*dx = leaving ? 10 : 24;
	else
		*dy = CF.panel_top ? -d : d;
}

void anim_surface_show(struct surface *s)
{
	if (!enabled() || !s->px)
		return;
	struct anim *g = find_surf(s, K_SURF_GHOST);
	if (g) {
		damage_add(g->cur);
		drop(g);
	}
	struct anim *old = find_surf(s, K_SURF);
	if (old)
		drop(old);
	struct anim *a = new_anim(K_SURF);
	if (!a)
		return;
	int dx, dy;
	surf_offset(s, &dx, &dy, false);
	struct rect b = tree_bounds(s);
	a->surf = s;
	a->from = shifted(b, dx, dy);
	a->to = b;
	a->a0 = 0;
	a->a1 = 255;
	a->t0 = now_ms();
	a->dur = scaled_ms(s->role == ROLE_NOTIFY ? 180 : 130);
	a->ease = E_OUT;
	a->cur = a->from;
	a->cur_a = 0;
	kick(a);
}

void anim_surface_hide(struct surface *s)
{
	if (!enabled() || !s->px || C.locked)
		return;
	struct anim *old = find_surf(s, K_SURF);
	if (old) {
		damage_add(old->cur);
		drop(old);
	}
	struct anim *g = find_surf(s, K_SURF_GHOST);
	if (g)
		drop(g);
	struct anim *a = new_anim(K_SURF_GHOST);
	if (!a)
		return;
	if (!snapshot(a, s)) {
		drop(a);
		return;
	}
	int dx, dy;
	surf_offset(s, &dx, &dy, true);
	a->surf = s;
	a->from = a->pbounds;
	a->to = shifted(a->pbounds, dx, dy);
	a->a0 = 255;
	a->a1 = 0;
	a->t0 = now_ms();
	a->dur = scaled_ms(100);
	a->ease = E_IN;
	a->cur = a->from;
	a->cur_a = 255;
	kick(a);
}

void anim_surface_gone(struct surface *s)
{
	for (int i = 0; i < MAX_ANIM; i++)
		if (A[i].used && A[i].surf == s && A[i].kind == K_SURF) {
			damage_add(A[i].cur);
			drop(&A[i]);
		}
}

/* the surface is animating (shown): draw it moved and faded, true when it did */
bool anim_draw_surface(struct surface *s, struct rect clip)
{
	struct anim *a = find_surf(s, K_SURF);
	if (!a || !s->px)
		return false;
	draw_tree_xf(s, tree_bounds(s), a->cur, a->cur_a, clip);
	return true;
}

void anim_draw_surf_ghosts(struct rect clip)
{
	for (int i = 0; i < MAX_ANIM; i++)
		if (A[i].used && A[i].kind == K_SURF_GHOST && A[i].px)
			blit_xf(A[i].px, A[i].pw, A[i].ph, true, A[i].cur, A[i].cur_a, clip);
}
