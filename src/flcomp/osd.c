/* The on-screen indicator for volume, brightness, microphone and the screenshot: an icon and a bar in a
 * small flat box above the panel, centred. One of it, shared by all of them; a new
 * call replaces what is shown. It stays about a second, fades out in the next
 * 300 ms and is drawn by the compositor, so it is there with or without a panel.
 * SPDX-License-Identifier: MIT */
#include "comp.h"

#define OSD_W 240
#define OSD_H 56
#define HOLD_MS 1200
#define FADE_MS 300

/* FreeLFS palette (dark): panel, line, text, dim, accent, err */
#define C_BOX P(panel)
#define C_LINE P(line)
#define C_TEXT P(text)
#define C_DIM P(dim)
#define C_ACCENT theme_accent()	/* accent= of desktop.conf, see theme.c */
#define C_ERR P(err)

static struct {
	bool on;
	int kind, pct;
	bool muted;
	uint32_t start;
	struct wl_event_source *timer;
} O;

static struct rect osd_rect(void)
{
	int x = (C.w - OSD_W) / 2, y = C.h - (CF.panel_top ? 0 : C.panel_h) - 24 - OSD_H;
	if (y < 0)
		y = 0;
	return (struct rect){ x, y, x + OSD_W, y + OSD_H };
}

static int alpha_now(void)
{
	uint32_t age = now_ms() - O.start;
	if (age <= HOLD_MS)
		return 255;
	if (age >= HOLD_MS + FADE_MS)
		return 0;
	return (int)(255 - 255 * (age - HOLD_MS) / FADE_MS);
}

static int tick(void *data)
{
	struct rect r = osd_rect();
	uint32_t age = now_ms() - O.start;
	if (age >= HOLD_MS + FADE_MS) {
		O.on = false;
		damage_add(r);
		return 0;
	}
	if (age > HOLD_MS)
		damage_add(r);
	wl_event_source_timer_update(O.timer, age < HOLD_MS ? (int)(HOLD_MS - age) : 40);
	return 0;
}

void osd_show(int kind, int percent, bool muted)
{
	if (!C.loop)
		return;
	if (!O.timer)
		O.timer = wl_event_loop_add_timer(C.loop, tick, NULL);
	O.on = true;
	O.kind = kind;
	O.pct = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	O.muted = muted;
	O.start = now_ms();
	/* the sounds that go with it: a volume step ticks, a saved screenshot clicks, a failed one is an error */
	if (kind == OSD_VOLUME && !muted)
		sound_play("tick");
	else if (kind == OSD_SHOT)
		sound_play(muted ? "error" : "screenshot");
	damage_add(osd_rect());
	wl_event_source_timer_update(O.timer, HOLD_MS);
}

/* ---- drawing: straight into the shadow, mixed with what is below by alpha -------------- */

static void px(struct rect clip, int x, int y, uint32_t col, int a)
{
	if (x < clip.x1 || y < clip.y1 || x >= clip.x2 || y >= clip.y2 || x < 0 || y < 0 || x >= C.w || y >= C.h)
		return;
	uint32_t *d = &C.shadow[(size_t)y * C.w + x];
	if (a >= 255) {
		*d = col;
		return;
	}
	/* one channel at a time: the old packed red+blue math carried red's remainder into blue
	 * (a blue tint while the indicator faded) */
	uint32_t r = (((*d >> 16 & 255) * (255 - a) + (col >> 16 & 255) * a) / 255) & 255;
	uint32_t g = (((*d >> 8 & 255) * (255 - a) + (col >> 8 & 255) * a) / 255) & 255;
	uint32_t b = (((*d & 255) * (255 - a) + (col & 255) * a) / 255) & 255;
	*d = r << 16 | g << 8 | b;
}

static void fill(struct rect clip, int x, int y, int w, int h, uint32_t col, int a)
{
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++)
			px(clip, x + i, y + j, col, a);
}

/* icons on a 16 x 16 grid, every cell 2 x 2 pixels */
static void cell(struct rect clip, int ox, int oy, int x, int y, int w, int h, uint32_t col, int a)
{
	fill(clip, ox + x * 2, oy + y * 2, w * 2, h * 2, col, a);
}

static void icon(struct rect clip, int ox, int oy, int kind, bool muted, int a)
{
	uint32_t c = C_TEXT;
	switch (kind) {
	case OSD_VOLUME:
		cell(clip, ox, oy, 1, 6, 4, 4, c, a);			/* the box */
		for (int i = 0; i < 5; i++)				/* the cone */
			cell(clip, ox, oy, 5 + i, 6 - i, 1, 4 + 2 * i, c, a);
		if (muted) {
			for (int i = 0; i < 5; i++) {
				cell(clip, ox, oy, 11 + i, 6 + i, 1, 1, C_ERR, a);
				cell(clip, ox, oy, 11 + i, 10 - i, 1, 1, C_ERR, a);
			}
		} else {
			cell(clip, ox, oy, 11, 6, 1, 4, c, a);
			cell(clip, ox, oy, 13, 4, 1, 8, c, a);
		}
		break;
	case OSD_BRIGHTNESS:
		cell(clip, ox, oy, 5, 6, 6, 4, c, a);			/* the disc */
		cell(clip, ox, oy, 6, 5, 4, 6, c, a);
		cell(clip, ox, oy, 7, 1, 2, 2, c, a);			/* rays */
		cell(clip, ox, oy, 7, 13, 2, 2, c, a);
		cell(clip, ox, oy, 1, 7, 2, 2, c, a);
		cell(clip, ox, oy, 13, 7, 2, 2, c, a);
		cell(clip, ox, oy, 3, 3, 2, 2, c, a);
		cell(clip, ox, oy, 11, 3, 2, 2, c, a);
		cell(clip, ox, oy, 3, 11, 2, 2, c, a);
		cell(clip, ox, oy, 11, 11, 2, 2, c, a);
		break;
	case OSD_SHOT:
		/* the viewfinder: four corners round a dot (red when the picture could not be saved) */
		for (int k = 0; k < 4; k++) {
			int x = k & 1 ? 12 : 1, y = k & 2 ? 12 : 1, hx = k & 1 ? 12 : 1, vy = k & 2 ? 12 : 1;
			cell(clip, ox, oy, hx, y, 3, 1, c, a);
			cell(clip, ox, oy, x, vy, 1, 3, c, a);
		}
		cell(clip, ox, oy, 6, 6, 4, 4, muted ? C_ERR : C_ACCENT, a);
		break;
	default:
		cell(clip, ox, oy, 6, 1, 4, 8, c, a);			/* the capsule */
		cell(clip, ox, oy, 5, 2, 6, 6, c, a);
		cell(clip, ox, oy, 3, 7, 1, 3, c, a);			/* the cradle */
		cell(clip, ox, oy, 12, 7, 1, 3, c, a);
		cell(clip, ox, oy, 4, 10, 8, 1, c, a);
		cell(clip, ox, oy, 7, 11, 2, 2, c, a);
		cell(clip, ox, oy, 5, 13, 6, 1, c, a);
		if (muted)
			for (int i = 0; i < 14; i++)
				cell(clip, ox, oy, 1 + i, 14 - i, 1, 1, C_ERR, a);
	}
}

void osd_draw(struct rect clip)
{
	if (!O.on)
		return;
	int a = alpha_now();
	if (!a)
		return;
	struct rect r = osd_rect(), o;
	if (!rect_intersect(r, clip, &o))
		return;
	fill(o, r.x1, r.y1, OSD_W, OSD_H, C_LINE, a);			/* 1 px line around the box */
	fill(o, r.x1 + 1, r.y1 + 1, OSD_W - 2, OSD_H - 2, C_BOX, a);
	icon(o, r.x1 + 16, r.y1 + 12, O.kind, O.muted, a);
	int bx = r.x1 + 64, bw = OSD_W - 64 - 20, by = r.y1 + OSD_H / 2 - 3;
	int level = O.kind == OSD_SHOT ? 100 : O.kind == OSD_MIC ? (O.muted ? 0 : 100) : O.muted ? 0 : O.pct;
	fill(o, bx, by, bw, 6, C_LINE, a);
	fill(o, bx, by, bw * level / 100, 6, O.kind == OSD_SHOT && O.muted ? C_ERR : O.muted ? C_DIM : C_ACCENT, a);
}
