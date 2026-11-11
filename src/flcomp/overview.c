/* The workspace overview (Super+Tab): every workspace as a tile in a grid like the real one, each
 * with its windows as small pictures, the one in view framed in the accent colour. Click a tile to
 * go there, click a window to go there and focus it; arrows, Tab, Enter, the digits 1-9 and Esc
 * work too. The compositor draws it (like switcher.c and osd.c), over the work area, under the
 * panel, so it needs no fonts (the tile numbers are a 5x7 pixel digit set) and the panel's pager
 * stays usable. Flat, panel colours: no shadow, no blur, no rounded corners.
 * SPDX-License-Identifier: MIT */
#include "comp.h"

#define PAD 28			/* around the grid */
#define GAP 14			/* between tiles */
#define MAX_WS 64

#define C_BACK P(back)	/* behind the tiles, a step beyond bg */
#define C_BG P(bg)
#define C_PANEL P(panel)
#define C_RAISED P(raised)
#define C_LINE P(line)
#define C_TEXT P(text)
#define C_DIM P(dim)
#define C_ACCENT theme_accent()

static bool open_;
static int sel;			/* the highlighted tile: keyboard and pointer */
static struct window *hot;	/* the window picture under the pointer, or NULL */
static bool btn_eaten;		/* a press was ours: its release is too */

bool overview_on(void)
{
	return open_ && !C.locked;
}

bool overview_has_pointer(void)
{
	return CF.panel_top ? C.py >= C.panel_h : C.py < C.h - C.panel_h;
}

void overview_changed(void)
{
	if (open_)
		damage_add(work_area());
}

/* ---- layout --------------------------------------------------------------- */

/* The tiles, row by row. Each is the size of the desktop above the panel, scaled down. */
static void layout(struct rect *t)
{
	struct rect wa = work_area();
	int cols = C.ws_cols, rows = C.ws_rows, ww = rect_w(wa), wh = rect_h(wa);
	int aw = ww - 2 * PAD - (cols - 1) * GAP, ah = wh - 2 * PAD - (rows - 1) * GAP;
	if (aw < cols * 8)
		aw = cols * 8;
	if (ah < rows * 8)
		ah = rows * 8;
	int tw = aw / cols, th = (int)((long)tw * wh / (C.w > 0 ? C.w : 1));
	if (th * rows > ah) {
		th = ah / rows;
		tw = (int)((long)th * C.w / (wh > 0 ? wh : 1));
	}
	if (tw < 8)
		tw = 8;
	if (th < 6)
		th = 6;
	int x0 = wa.x1 + (ww - (cols * tw + (cols - 1) * GAP)) / 2;
	int y0 = wa.y1 + (wh - (rows * th + (rows - 1) * GAP)) / 2;
	for (int i = 0; i < cols * rows && i < MAX_WS; i++) {
		int cx = i % cols, cy = i / cols;
		t[i].x1 = x0 + cx * (tw + GAP);
		t[i].y1 = y0 + cy * (th + GAP);
		t[i].x2 = t[i].x1 + tw;
		t[i].y2 = t[i].y1 + th;
	}
}

static int tile_at(const struct rect *t, int x, int y)
{
	for (int i = 0; i < C.ws_cols * C.ws_rows && i < MAX_WS; i++)
		if (x >= t[i].x1 && x < t[i].x2 && y >= t[i].y1 && y < t[i].y2)
			return i;
	return -1;
}

/* a screen rectangle (above the panel) inside tile t */
static struct rect to_tile(struct rect t, struct rect r)
{
	struct rect wa = work_area();
	int wh = rect_h(wa), tw = rect_w(t), th = rect_h(t), y0 = wa.y1;	/* y0: panel at the top */
	struct rect o = {
		t.x1 + (int)((long)r.x1 * tw / C.w), t.y1 + (int)((long)(r.y1 - y0) * th / wh),
		t.x1 + (int)((long)r.x2 * tw / C.w), t.y1 + (int)((long)(r.y2 - y0) * th / wh),
	};
	if (o.x2 <= o.x1)
		o.x2 = o.x1 + 1;
	if (o.y2 <= o.y1)
		o.y2 = o.y1 + 1;
	return o;
}

/* the topmost window picture of tile i at (x, y) */
static struct window *window_at(const struct rect *t, int i, int x, int y)
{
	struct window *w;
	wl_list_for_each(w, &C.windows, link)
		if (window_on_ws(w, i)) {
			struct rect r = to_tile(t[i], window_rect(w)), c;
			if (rect_intersect(r, t[i], &c) && x >= c.x1 && x < c.x2 && y >= c.y1 && y < c.y2)
				return w;
		}
	return NULL;
}

/* ---- drawing -------------------------------------------------------------- */

static void fill(struct rect clip, struct rect r, uint32_t col)
{
	struct rect o;
	if (!rect_intersect(r, clip, &o))
		return;
	for (int y = o.y1; y < o.y2; y++) {
		uint32_t *d = C.shadow + (size_t)y * C.w + o.x1;
		for (int x = o.x1; x < o.x2; x++)
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

/* pixels sw x sh scaled into the rectangle dst, 2 x 2 samples per pixel, only inside clip */
static void scaled(struct rect clip, const uint32_t *px, int sw, int sh, bool alpha, struct rect dst)
{
	int dw = rect_w(dst), dh = rect_h(dst);
	struct rect o;
	if (!px || sw < 1 || sh < 1 || dw < 1 || dh < 1 || !rect_intersect(dst, clip, &o))
		return;
	for (int y = o.y1; y < o.y2; y++) {
		long ya = ((long)(y - dst.y1) * 4 + 1) * sh / (dh * 4), yb = ((long)(y - dst.y1) * 4 + 3) * sh / (dh * 4);
		uint32_t *d = C.shadow + (size_t)y * C.w + o.x1;
		for (int x = o.x1; x < o.x2; x++, d++) {
			long xa = ((long)(x - dst.x1) * 4 + 1) * sw / (dw * 4), xb = ((long)(x - dst.x1) * 4 + 3) * sw / (dw * 4);
			uint32_t p[4] = { px[ya * sw + xa], px[ya * sw + xb], px[yb * sw + xa], px[yb * sw + xb] };
			uint32_t a = 0, rr = 0, gg = 0, bb = 0;
			for (int k = 0; k < 4; k++) {
				uint32_t q = alpha ? p[k] : (p[k] | 0xff000000u);
				a += q >> 24;
				rr += (q >> 16) & 255;
				gg += (q >> 8) & 255;
				bb += q & 255;
			}
			a /= 4;	/* premultiplied over the tile */
			uint32_t ia = 255 - a, dp = *d;
			uint32_t r2 = rr / 4 + (((dp >> 16) & 255) * ia + 127) / 255;
			uint32_t g2 = gg / 4 + (((dp >> 8) & 255) * ia + 127) / 255;
			uint32_t b2 = bb / 4 + ((dp & 255) * ia + 127) / 255;
			*d = (r2 > 255 ? 255 : r2) << 16 | (g2 > 255 ? 255 : g2) << 8 | (b2 > 255 ? 255 : b2);
		}
	}
}

/* 5 x 7 digits, drawn at 2x */
static const char *const DIGITS[10][7] = {
	{ "01110", "10001", "10011", "10101", "11001", "10001", "01110" },
	{ "00100", "01100", "00100", "00100", "00100", "00100", "01110" },
	{ "01110", "10001", "00001", "00010", "00100", "01000", "11111" },
	{ "11110", "00001", "00001", "01110", "00001", "00001", "11110" },
	{ "00010", "00110", "01010", "10010", "11111", "00010", "00010" },
	{ "11111", "10000", "11110", "00001", "00001", "10001", "01110" },
	{ "00110", "01000", "10000", "11110", "10001", "10001", "01110" },
	{ "11111", "00001", "00010", "00100", "01000", "01000", "01000" },
	{ "01110", "10001", "10001", "01110", "10001", "10001", "01110" },
	{ "01110", "10001", "10001", "01111", "00001", "00010", "01100" },
};

/* the tile number in a small box at the tile's top left corner */
static void label(struct rect clip, struct rect t, int num, bool cur)
{
	char s[4];
	int n = 0;
	if (num >= 10)
		s[n++] = (char)('0' + num / 10 % 10);
	s[n++] = (char)('0' + num % 10);
	int bw = n * 12 + 6 + 4, bh = 14 + 8;
	struct rect b = { t.x1, t.y1, t.x1 + bw, t.y1 + bh };
	fill(clip, b, cur ? C_ACCENT : C_PANEL);
	uint32_t fg = cur ? P(on_accent) : C_TEXT;
	for (int i = 0; i < n; i++)
		for (int y = 0; y < 7; y++)
			for (int x = 0; x < 5; x++)
				if (DIGITS[s[i] - '0'][y][x] == '1')
					fill(clip, (struct rect){ b.x1 + 5 + i * 12 + x * 2, b.y1 + 4 + y * 2,
								  b.x1 + 5 + i * 12 + x * 2 + 2, b.y1 + 4 + y * 2 + 2 }, fg);
}

static bool snapshot_draw;	/* anim.c draws the overview off screen while it zooms, open or not */

void overview_draw_snapshot(struct rect clip)
{
	snapshot_draw = true;
	overview_draw(clip);
	snapshot_draw = false;
}

void overview_draw(struct rect clip)
{
	if (!overview_on() && !snapshot_draw)
		return;
	struct rect wa = work_area(), c;
	if (!rect_intersect(wa, clip, &c))
		return;
	fill(c, wa, C_BACK);
	struct rect t[MAX_WS];
	layout(t);
	int n = C.ws_cols * C.ws_rows;
	const uint32_t *wp = wall_pixels();
	for (int i = 0; i < n && i < MAX_WS; i++) {
		struct rect tc;
		if (!rect_intersect(t[i], c, &tc))
			continue;
		fill(c, t[i], C_BG);
		if (wp)	/* the wallpaper covers the whole output; the tile is the part above the panel */
			scaled(c, wp + (size_t)wa.y1 * C.w, C.w, rect_h(wa), false, t[i]);
		struct window *w;
		wl_list_for_each_reverse(w, &C.windows, link)	/* bottom to top */
			if (window_on_ws(w, i) && w->surf->px) {
				struct rect g = to_tile(t[i], window_rect(w)), clipw;
				/* the whole surface, not only the geometry: the picture is what is drawn */
				int ox, oy;
				surface_origin(w->surf, &ox, &oy);
				struct rect full = to_tile(t[i], (struct rect){ ox, oy, ox + w->surf->w, oy + w->surf->h });
				if (rect_intersect(tc, full, &clipw))
					scaled(clipw, w->surf->px, w->surf->w, w->surf->h, w->surf->alpha, full);
				if (rect_intersect(tc, g, &clipw))
					frame(clipw, g, 1, w == hot ? C_ACCENT : C_LINE);
			}
		bool cur = i == C.ws_cur;
		label(c, t[i], i + 1, cur);
		if (i == sel)
			frame(c, t[i], 2, C_ACCENT);
		else if (cur)
			frame(c, t[i], 1, C_ACCENT);
		else
			frame(c, t[i], 1, C_LINE);
	}
}

/* ---- opening, closing, input ----------------------------------------------- */

void overview_toggle(void)
{
	if (open_) {
		overview_close();
		return;
	}
	if (C.locked || ws_count() < 1)
		return;
	if (cycling())
		cycle_end();
	menu_dismiss();
	flyout_dismiss();
	popups_dismiss();
	open_ = true;
	sel = C.ws_cur;
	hot = NULL;
	if (overview_has_pointer())
		seat_set_ptr_focus(NULL, 0, 0);
	anim_overview(true);
	damage_add(work_area());
}

void overview_close(void)
{
	if (!open_)
		return;
	anim_overview(false);	/* the picture of it, fading, before it is gone */
	damage_add(work_area());
	open_ = false;
	hot = NULL;
	seat_refocus();
}

static void go(int idx, struct window *w)
{
	overview_close();
	ws_switch(idx);
	if (w)
		window_activate(w);
}

void overview_motion(void)
{
	if (!overview_on())
		return;
	struct rect t[MAX_WS];
	layout(t);
	int i = tile_at(t, (int)C.px, (int)C.py);
	struct window *w = i >= 0 ? window_at(t, i, (int)C.px, (int)C.py) : NULL;
	int nsel = i >= 0 ? i : sel;
	if (nsel != sel || w != hot) {
		sel = nsel;
		hot = w;
		damage_add(work_area());
	}
}

bool overview_button(bool pressed)
{
	if (!overview_on())
		return false;
	if (!pressed) {
		bool e = btn_eaten;
		btn_eaten = false;
		return e;
	}
	if (!overview_has_pointer())
		return false;
	btn_eaten = true;
	struct rect t[MAX_WS];
	layout(t);
	int i = tile_at(t, (int)C.px, (int)C.py);
	if (i < 0)
		overview_close();	/* a click beside the tiles puts it away */
	else
		go(i, window_at(t, i, (int)C.px, (int)C.py));
	return true;
}

static void select_to(int idx)
{
	int n = C.ws_cols * C.ws_rows;
	if (idx < 0 || idx >= n || idx == sel)
		return;
	sel = idx;
	hot = NULL;
	damage_add(work_area());
}

bool overview_key(uint32_t sym, bool pressed)
{
	if (!overview_on())
		return false;
	if (!pressed)
		return true;
	int n = C.ws_cols * C.ws_rows, x = sel % C.ws_cols, y = sel / C.ws_cols;
	switch (sym) {
	case 0xff1b:	/* Escape */
		overview_close();
		break;
	case 0xff0d: case 0xff8d: case 0x20:	/* Return, KP_Enter, space */
		go(sel, NULL);
		break;
	case 0xff51: if (x > 0) select_to(sel - 1); break;			/* Left */
	case 0xff53: if (x < C.ws_cols - 1) select_to(sel + 1); break;	/* Right */
	case 0xff52: if (y > 0) select_to(sel - C.ws_cols); break;		/* Up */
	case 0xff54: if (y < C.ws_rows - 1) select_to(sel + C.ws_cols); break;	/* Down */
	case 0xff09: select_to((sel + 1) % n); break;			/* Tab: the next, and round */
	case 0xfe20: select_to((sel + n - 1) % n); break;			/* ISO_Left_Tab */
	default:
		if (sym >= '1' && sym <= '9' && (int)(sym - '1') < n)
			go((int)(sym - '1'), NULL);
	}
	return true;
}
