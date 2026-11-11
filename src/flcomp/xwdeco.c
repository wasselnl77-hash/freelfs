/* xwdeco.c -- the frame flcomp draws around X windows that have none of their own.
 *
 * Wayland windows draw their own title bars; X11 programs expect the window manager to. A frame
 * is a title bar (title in the built-in Plex font of lockfont.h, minimize, maximize and close),
 * a thin border on three sides, and a few pixels of grip outside it for resizing. Windows that
 * ask for no decoration (_MOTIF_WM_HINTS, as Steam does), splash screens, menus and fullscreen
 * windows get none. Drawn and hit-tested by flcomp, outside the client's surface: pick() notes
 * the window when the pointer is on its frame (surface.c), seat.c sends presses here.
 * Flat, FreeLFS palette: panel and raised greys, accent line on the focused window.
 * SPDX-License-Identifier: MIT */
#include "xwm-int.h"
#include "lockfont.h"

#include <string.h>

#define GRIP 4			/* grab zone outside the border */
#define BTN_W 46

#define C_PANEL P(panel)
#define C_RAISED P(raised)
#define C_LINE P(line)
#define C_TEXT P(text)
#define C_DIM P(dim)
#define C_CLOSE P(err)

enum { DP_NONE, DP_TITLE, DP_MIN, DP_MAX, DP_CLOSE, DP_EDGE };

static struct window *noted, *hover_w, *pressed_w;
static int hover_p, pressed_p;
static unsigned pressed_edges;

struct xinset xwm_insets(struct window *w)
{
	struct xinset z = { 0, 0, 0, 0 };
	if (!w || !w->xw || !w->xw->deco || w->fullscreen)
		return z;
	if (w->maximized) {
		z.t = XD_TITLE;
		return z;
	}
	z.l = z.r = z.b = XD_BORDER;
	z.t = XD_TITLE;
	return z;
}

static struct rect outer_of(struct window *w)
{
	struct xinset in = xwm_insets(w);
	struct rect c = window_rect(w);
	return (struct rect){ c.x1 - in.l, c.y1 - in.t, c.x2 + in.r, c.y2 + in.b };
}

struct rect xwm_deco_outer(struct window *w)
{
	if (!w->xw || !w->xw->deco || w->fullscreen)
		return (struct rect){ 0, 0, 0, 0 };
	return outer_of(w);
}

/* titlebar= or buttons= changed: the frames are laid out again on the next paint */
void xwm_frames_changed(void)
{
	work_area_changed();
	damage_all();
}

void xwm_deco_damage(struct window *w)
{
	struct rect r = xwm_deco_outer(w);
	if (!rect_empty(r) && w->mapped)
		damage_add(r);
}

void xwm_deco_forget(struct window *w)
{
	if (noted == w)
		noted = NULL;
	if (hover_w == w)
		hover_w = NULL;
	if (pressed_w == w) {
		pressed_w = NULL;
		pressed_p = DP_NONE;
	}
}

/* ---- drawing ------------------------------------------------------------------------------------- */

static void fill(struct rect clip, int x, int y, int w, int h, uint32_t col)
{
	struct rect r = { x, y, x + w, y + h }, o;
	if (!rect_intersect(r, clip, &o))
		return;
	for (int j = o.y1; j < o.y2; j++) {
		uint32_t *d = C.shadow + (size_t)j * C.w + o.x1;
		for (int i = o.x1; i < o.x2; i++)
			*d++ = col;
	}
}

static void put(struct rect clip, int x, int y, uint32_t col, int a)
{
	if (x < clip.x1 || y < clip.y1 || x >= clip.x2 || y >= clip.y2)
		return;
	uint32_t *d = &C.shadow[(size_t)y * C.w + x];
	uint32_t r = (((*d >> 16 & 255) * (255 - a) + (col >> 16 & 255) * a) / 255) & 255;
	uint32_t g = (((*d >> 8 & 255) * (255 - a) + (col >> 8 & 255) * a) / 255) & 255;
	uint32_t b = (((*d & 255) * (255 - a) + (col & 255) * a) / 255) & 255;
	*d = r << 16 | g << 8 | b;
}

static int text_width(const char *s)
{
	int w = 0;
	for (; *s; s++)
		if (*s >= 32 && *s < 127)
			w += LF_BODY_ADV[*s - 32];
	return w;
}

/* text clipped to maxw pixels: "..." replaces what does not fit */
static void text(struct rect clip, int x, int y, const char *s, uint32_t col, int maxw)
{
	int total = text_width(s);
	bool cut = total > maxw;
	int dots = LF_BODY_ADV['.' - 32] * 3;
	int limit = cut ? maxw - dots : maxw, used = 0;
	for (; *s; s++) {
		if (*s < 32 || *s > 126)
			continue;
		int adv = LF_BODY_ADV[*s - 32];
		if (used + adv > limit)
			break;
		const unsigned char *g = LF_BODY_DATA[*s - 32];
		for (int j = 0; j < LF_BODY_H; j++)
			for (int i = 0; i < LF_BODY_W; i++) {
				int idx = j * LF_BODY_W + i;
				int q = (g[idx >> 2] >> (6 - 2 * (idx & 3))) & 3;
				if (q)
					put(clip, x + used + i, y + j, col, q * 85);
			}
		used += adv;
	}
	if (cut)
		for (int k = 0; k < 3; k++) {
			const unsigned char *g = LF_BODY_DATA['.' - 32];
			for (int j = 0; j < LF_BODY_H; j++)
				for (int i = 0; i < LF_BODY_W; i++) {
					int idx = j * LF_BODY_W + i;
					int q = (g[idx >> 2] >> (6 - 2 * (idx & 3))) & 3;
					if (q)
						put(clip, x + used + i, y + j, col, q * 85);
				}
			used += LF_BODY_ADV['.' - 32];
		}
}

static void line(struct rect clip, int x0, int y0, int x1, int y1, uint32_t col)
{
	int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y1 - y0 : y0 - y1;
	int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx - dy;
	for (;;) {
		put(clip, x0, y0, col, 255);
		if (x0 == x1 && y0 == y1)
			break;
		int e2 = 2 * err;
		if (e2 > -dy) { err -= dy; x0 += sx; }
		if (e2 < dx) { err += dx; y0 += sy; }
	}
}

static void box(struct rect clip, int x, int y, int w, int h, uint32_t col)
{
	fill(clip, x, y, w, 1, col);
	fill(clip, x, y + h - 1, w, 1, col);
	fill(clip, x, y, 1, h, col);
	fill(clip, x + w - 1, y, 1, h, col);
}

static struct rect button_rect(struct rect o, int part)
{
	int idx = part == DP_CLOSE ? 0 : part == DP_MAX ? 1 : 2;
	if (CF.buttons_left)	/* buttons=left: close is the outer one at the left edge too */
		return (struct rect){ o.x1 + BTN_W * idx, o.y1, o.x1 + BTN_W * (idx + 1), o.y1 + XD_TITLE };
	return (struct rect){ o.x2 - BTN_W * (idx + 1), o.y1, o.x2 - BTN_W * idx, o.y1 + XD_TITLE };
}

void xwm_deco_draw(struct window *w, struct rect clip)
{
	if (!w->xw || !w->xw->deco || w->fullscreen)
		return;
	struct xinset in = xwm_insets(w);
	struct rect c = window_rect(w), o = { c.x1 - in.l, c.y1 - in.t, c.x2 + in.r, c.y2 + in.b }, t;
	if (!rect_intersect(o, clip, &t))
		return;
	bool foc = C.focused == w;
	uint32_t bg = foc ? C_RAISED : C_PANEL;
	int ow = rect_w(o);
	fill(clip, o.x1, o.y1, ow, in.t, bg);						/* title bar */
	fill(clip, o.x1, c.y1, in.l, rect_h(c), bg);					/* sides and bottom */
	fill(clip, c.x2, c.y1, in.r, rect_h(c), bg);
	fill(clip, o.x1, c.y2, ow, in.b, bg);
	if (in.l || in.r || in.b) {							/* the thin outer line */
		fill(clip, o.x1, o.y1, 1, rect_h(o), C_LINE);
		fill(clip, o.x2 - 1, o.y1, 1, rect_h(o), C_LINE);
		fill(clip, o.x1, o.y2 - 1, ow, 1, C_LINE);
	}
	fill(clip, o.x1, o.y1, ow, 1, foc ? theme_accent() : C_LINE);
	if (foc)
		fill(clip, o.x1, o.y1 + 1, ow, 1, theme_accent());
	fill(clip, o.x1, c.y1 - 1, ow, 1, C_LINE);					/* between title bar and client */
	/* the title */
	const char *title = w->title && *w->title ? w->title : w->app_id && *w->app_id ? w->app_id : "";
	int tx = o.x1 + (CF.buttons_left ? 3 * BTN_W + 10 : 12), tw = ow - 12 - 3 * BTN_W - 8;
	if (tw > 20)
		text(clip, tx, o.y1 + (XD_TITLE - LF_BODY_H) / 2 + 1, title, foc ? C_TEXT : C_DIM, tw);
	/* buttons */
	uint32_t ink = foc ? C_TEXT : C_DIM;
	for (int part = DP_MIN; part <= DP_CLOSE; part++) {
		struct rect b = button_rect(o, part);
		bool hot = (hover_w == w && hover_p == part) || (pressed_w == w && pressed_p == part);
		uint32_t col = ink;
		if (hot) {
			fill(clip, b.x1, b.y1 + 1, BTN_W, XD_TITLE - 1, part == DP_CLOSE ? C_CLOSE : C_LINE);
			col = C_TEXT;
		}
		int cx = (b.x1 + b.x2) / 2, cy = (b.y1 + b.y2) / 2;
		if (part == DP_MIN) {
			fill(clip, cx - 5, cy + 3, 11, 1, col);
		} else if (part == DP_MAX) {
			if (w->maximized) {
				box(clip, cx - 4, cy - 2, 8, 8, col);
				fill(clip, cx - 2, cy - 4, 8, 1, col);
				fill(clip, cx + 5, cy - 4, 1, 6, col);
			} else {
				box(clip, cx - 5, cy - 5, 11, 11, col);
				fill(clip, cx - 4, cy - 4, 9, 1, col);	/* a heavier top edge */
			}
		} else {
			line(clip, cx - 5, cy - 5, cx + 5, cy + 5, col);
			line(clip, cx - 5, cy + 5, cx + 5, cy - 5, col);
			line(clip, cx - 4, cy - 5, cx + 5, cy + 4, col);
			line(clip, cx - 4, cy + 5, cx + 5, cy - 4, col);
		}
	}
}

/* ---- hit testing --------------------------------------------------------------------------------------- */

static int part_at(struct window *w, int x, int y, unsigned *edges)
{
	*edges = 0;
	if (!w->xw || !w->xw->deco || w->fullscreen)
		return DP_NONE;
	struct xinset in = xwm_insets(w);
	struct rect c = window_rect(w), o = { c.x1 - in.l, c.y1 - in.t, c.x2 + in.r, c.y2 + in.b };
	bool resizable = !w->maximized;
	struct rect hit = o;
	if (resizable) {
		hit.x1 -= GRIP;
		hit.x2 += GRIP;
		hit.y2 += GRIP;
	}
	if (x < hit.x1 || x >= hit.x2 || y < hit.y1 || y >= hit.y2)
		return DP_NONE;
	if (x >= c.x1 && x < c.x2 && y >= c.y1 && y < c.y2)
		return DP_NONE;		/* the client's own pixels */
	if (resizable) {
		unsigned e = 0;
		if (x < c.x1) e |= 4;			/* left */
		if (x >= c.x2) e |= 8;			/* right */
		if (y >= c.y2) e |= 2;			/* bottom */
		if (y < o.y1 + 4) e |= 1;		/* top */
		/* the left and right strips beside the title bar also grab, the corners too */
		if (e) {
			*edges = e;
			return DP_EDGE;
		}
	}
	if (y < c.y1 && x >= o.x1 && x < o.x2) {
		for (int part = DP_MIN; part <= DP_CLOSE; part++) {
			struct rect b = button_rect(o, part);
			if (x >= b.x1 && x < b.x2 && y >= b.y1 && y < b.y2)
				return part;
		}
		return DP_TITLE;
	}
	return DP_NONE;
}

bool xwm_deco_contains(struct window *w, int x, int y)
{
	unsigned e;
	return part_at(w, x, y, &e) != DP_NONE;
}

void xwm_deco_note(struct window *w)
{
	noted = w;
}

struct window *xwm_deco_target(void)
{
	return noted;
}

void xwm_deco_hover(void)
{
	struct window *nw = noted;
	unsigned e;
	int np = nw ? part_at(nw, (int)C.px, (int)C.py, &e) : DP_NONE;
	if (hover_w == nw && hover_p == np)
		return;
	struct window *old = hover_w;
	hover_w = nw;
	hover_p = np;
	if (old)
		xwm_deco_damage(old);
	if (nw && nw != old)
		xwm_deco_damage(nw);
	else if (nw)
		xwm_deco_damage(nw);
}

bool xwm_deco_active(void)
{
	return pressed_w != NULL;
}

void xwm_deco_press(struct window *w)
{
	static struct window *last_w;
	static uint32_t last_t;
	static int last_x, last_y;
	unsigned edges;
	int part = part_at(w, (int)C.px, (int)C.py, &edges);
	pressed_w = w;
	pressed_p = part;
	pressed_edges = edges;
	switch (part) {
	case DP_TITLE: {
		uint32_t now = now_ms();
		int dx = (int)C.px - last_x, dy = (int)C.py - last_y;
		if (last_w == w && now - last_t < 400 && dx * dx + dy * dy < 64) {
			last_w = NULL;
			pressed_w = NULL;
			pressed_p = DP_NONE;
			window_request_state(w, !w->maximized, -1);	/* double click: maximize or restore */
			return;
		}
		last_w = w;
		last_t = now;
		last_x = (int)C.px;
		last_y = (int)C.py;
		window_move_begin(w);
		break;
	}
	case DP_EDGE:
		window_resize_begin(w, edges);
		break;
	default:
		xwm_deco_damage(w);
		break;
	}
}

void xwm_deco_release(void)
{
	struct window *w = pressed_w;
	int p = pressed_p;
	if (!w)
		return;
	pressed_w = NULL;
	pressed_p = DP_NONE;
	xwm_deco_damage(w);
	if (grab_active())
		return;
	unsigned e;
	if (part_at(w, (int)C.px, (int)C.py, &e) != p)
		return;
	switch (p) {
	case DP_MIN:
		window_minimize(w);
		break;
	case DP_MAX:
		window_request_state(w, !w->maximized, -1);
		break;
	case DP_CLOSE:
		window_close(w);
		break;
	default:
		break;
	}
}
