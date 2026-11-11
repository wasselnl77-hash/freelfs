/* The Alt-Tab switcher: a row of small window pictures, centred over the work area, with the
 * window that Alt-Tab has picked outlined in the accent colour. The compositor draws it (like
 * osd.c), so it needs no panel and no fonts: the pictures are the windows themselves, scaled
 * down; the panel's entry for the picked window lights up at the same time because Alt-Tab
 * focuses the windows live (xdg.c). Shown from the first Alt-Tab to the release of Alt.
 * Flat, panel colours: no shadow, no blur, no rounded corners.
 * SPDX-License-Identifier: MIT */
#include "comp.h"

#define TILE_W 176		/* one window: frame included */
#define TILE_H 120
#define FRAME 8			/* between the frame and the picture */
#define GAP 8
#define PAD 12			/* around the row */

/* FreeLFS palette (dark): panel, raised, bg, line, accent */
#define C_BOX 0x272c33u
#define C_BG 0x1d2125u
#define C_LINE 0x3a4048u
#define C_ACCENT theme_accent()	/* accent= of desktop.conf, see theme.c */

static struct rect last;	/* where it was drawn last: empty when not shown */

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

static void frame(struct rect clip, int x, int y, int w, int h, int t, uint32_t col)
{
	fill(clip, x, y, w, t, col);
	fill(clip, x, y + h - t, w, t, col);
	fill(clip, x, y + t, t, h - 2 * t, col);
	fill(clip, x + w - t, y + t, t, h - 2 * t, col);
}

/* the windows to show, in Alt-Tab order, and the picked one's place in that list; at most as many as fit */
static int visible(struct window **out, int max, int *sel_out, int *first_out)
{
	int n, sel;
	struct window **l = cycle_windows(&n, &sel);
	struct window *all[64];
	int cnt = 0, selv = -1;
	for (int i = 0; i < n && cnt < 64; i++)
		if (l[i]) {
			if (i == sel)
				selv = cnt;
			all[cnt++] = l[i];
		}
	int first = 0;
	if (cnt > max) {		/* more than fit: a stretch around the picked one */
		first = selv > max / 2 ? selv - max / 2 : 0;
		if (first > cnt - max)
			first = cnt - max;
	}
	int shown = cnt > max ? max : cnt;
	for (int i = 0; i < shown; i++)
		out[i] = all[first + i];
	*sel_out = selv - first;
	*first_out = first;
	return shown;
}

static int max_tiles(void)
{
	int m = (C.w - 2 * PAD - 32 + GAP) / (TILE_W + GAP);
	return m < 1 ? 1 : m > 64 ? 64 : m;
}

static struct rect box_rect(int n)
{
	struct rect wa = work_area();
	int w = n * TILE_W + (n - 1) * GAP + 2 * PAD, h = TILE_H + 2 * PAD;
	int x = wa.x1 + (rect_w(wa) - w) / 2, y = wa.y1 + (rect_h(wa) - h) / 2;
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	return (struct rect){ x, y, x + w, y + h };
}

/* Called whenever the list or the picked window changed, and when cycling ended. */
void switcher_changed(void)
{
	if (!rect_empty(last))
		damage_add(last);
	last = (struct rect){ 0, 0, 0, 0 };
	if (!cycling())
		return;
	struct window *v[64];
	int sel, first, n = visible(v, max_tiles(), &sel, &first);
	if (n < 1)
		return;
	last = box_rect(n);
	damage_add(last);
}

static uint32_t over(uint32_t s, uint32_t d)	/* premultiplied s over d */
{
	uint32_t a = s >> 24;
	if (a == 255)
		return s;
	uint32_t ia = 255 - a;
	uint32_t rb = ((d & 0xff00ffu) * ia / 255) & 0xff00ffu, g = ((d & 0x00ff00u) * ia / 255) & 0x00ff00u;
	return 0xff000000u | ((s & 0xffffffu) + rb + g);
}

/* The window s scaled down into the box (x, y, w, h), 2 x 2 samples per pixel, only inside clip. */
static void thumb(struct rect clip, struct surface *s, int bx, int by, int bw, int bh)
{
	if (!s || !s->px || s->w < 1 || s->h < 1)
		return;
	/* fit, never enlarge */
	long fx = (long)bw * 65536 / s->w, fy = (long)bh * 65536 / s->h;
	long sc = fx < fy ? fx : fy;
	if (sc > 65536)
		sc = 65536;
	int dw = (int)((long)s->w * sc / 65536), dh = (int)((long)s->h * sc / 65536);
	if (dw < 1 || dh < 1)
		return;
	int ox = bx + (bw - dw) / 2, oy = by + (bh - dh) / 2;
	long inv = 65536L * 65536L / sc;	/* source pixels per destination pixel, 16.16 */
	struct rect r = { ox, oy, ox + dw, oy + dh }, o;
	if (!rect_intersect(r, clip, &o))
		return;
	for (int y = o.y1; y < o.y2; y++) {
		long sy0 = (long)(y - oy) * inv + inv / 4, sy1 = (long)(y - oy) * inv + inv * 3 / 4;
		int ya = (int)(sy0 >> 16), yb = (int)(sy1 >> 16);
		ya = ya >= s->h ? s->h - 1 : ya;
		yb = yb >= s->h ? s->h - 1 : yb;
		uint32_t *d = C.shadow + (size_t)y * C.w + o.x1;
		for (int x = o.x1; x < o.x2; x++, d++) {
			long sx0 = (long)(x - ox) * inv + inv / 4, sx1 = (long)(x - ox) * inv + inv * 3 / 4;
			int xa = (int)(sx0 >> 16), xb = (int)(sx1 >> 16);
			xa = xa >= s->w ? s->w - 1 : xa;
			xb = xb >= s->w ? s->w - 1 : xb;
			uint32_t p[4] = { s->px[(size_t)ya * s->w + xa], s->px[(size_t)ya * s->w + xb],
					  s->px[(size_t)yb * s->w + xa], s->px[(size_t)yb * s->w + xb] };
			uint32_t a = 0, rr = 0, gg = 0, bb = 0;
			for (int k = 0; k < 4; k++) {
				uint32_t q = s->alpha ? p[k] : (p[k] | 0xff000000u);
				a += q >> 24;
				rr += (q >> 16) & 255;
				gg += (q >> 8) & 255;
				bb += q & 255;
			}
			*d = over((a / 4) << 24 | (rr / 4) << 16 | (gg / 4) << 8 | bb / 4, *d);
		}
	}
}

void switcher_draw(struct rect clip)
{
	if (rect_empty(last) || !cycling())
		return;
	struct rect o;
	if (!rect_intersect(last, clip, &o))
		return;
	struct window *v[64];
	int sel, first, n = visible(v, max_tiles(), &sel, &first);
	if (n < 1)
		return;
	struct rect b = box_rect(n);
	fill(o, b.x1, b.y1, rect_w(b), rect_h(b), C_LINE);		/* 1 px line around the box */
	fill(o, b.x1 + 1, b.y1 + 1, rect_w(b) - 2, rect_h(b) - 2, C_BOX);
	for (int i = 0; i < n; i++) {
		int tx = b.x1 + PAD + i * (TILE_W + GAP), ty = b.y1 + PAD;
		fill(o, tx, ty, TILE_W, TILE_H, C_BG);
		thumb(o, v[i]->surf, tx + FRAME, ty + FRAME, TILE_W - 2 * FRAME, TILE_H - 2 * FRAME);
		if (i == sel)
			frame(o, tx, ty, TILE_W, TILE_H, 2, C_ACCENT);
		else
			frame(o, tx, ty, TILE_W, TILE_H, 1, C_LINE);
	}
}
