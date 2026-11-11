/* Damage tracking and CPU compositing into the shadow buffer.
 * SPDX-License-Identifier: MIT */
#include "comp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>

#define BG P(bg)		/* fl bg: the desktop colour */

/* ---- the wallpaper: scaled once to the output size, one cached buffer -------------------- */

static uint32_t *wall;	/* C.w x C.h XRGB, or NULL: flat BG */

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void wall_init(void)
{
	theme_init();
	const char *path = getenv("FLCOMP_WALLPAPER");
	if (!path)
		path = theme_wallpaper();	/* wallpaper= in desktop.conf (theme.c) */
	if (!*path)
		return;
	FILE *f = fopen(path, "rb");
	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	long fsz = ftell(f);
	rewind(f);
	uint8_t *fd = fsz > 32 && fsz < (64 << 20) ? malloc((size_t)fsz) : NULL;
	if (!fd || fread(fd, 1, (size_t)fsz, f) != (size_t)fsz || memcmp(fd, "FLWALL01", 8)) {
		free(fd);
		fclose(f);
		return;
	}
	fclose(f);
	uint32_t w = le32(fd + 16), h = le32(fd + 20), np = le32(fd + 24), cs = le32(fd + 28);
	size_t plane = (size_t)w * h, n = plane * 3;
	uint8_t *raw = NULL;
	uint32_t *out = NULL;
	if (np != 3 || w < 2 || h < 2 || w > 8192 || h > 8192 || (size_t)cs + 32 > (size_t)fsz)
		goto done;
	raw = malloc(n);
	out = malloc((size_t)C.w * C.h * 4);
	uLongf rl = n;
	if (!raw || !out || uncompress(raw, &rl, fd + 32, cs) != Z_OK || rl != n)
		goto fail;
	for (int c = 0; c < 3; c++)
		for (size_t y = 0; y < h; y++) {
			uint8_t *r = raw + c * plane + y * w;
			for (size_t x = 1; x < w; x++)
				r[x] = (uint8_t)(r[x] + r[x - 1]);
		}
	/* cover the output, centred, bilinear, 16.16 */
	double sc = (double)C.w / w > (double)C.h / h ? (double)C.w / w : (double)C.h / h;
	double offx = ((double)w - C.w / sc) / 2, offy = ((double)h - C.h / sc) / 2;
	for (int y = 0; y < C.h; y++) {
		double sy = ((y + 0.5) / sc + offy) - 0.5;
		int y0 = (int)sy;
		unsigned fy = (unsigned)((sy - y0) * 256);
		if (sy < 0) { y0 = 0; fy = 0; }
		int y1 = y0 + 1 < (int)h ? y0 + 1 : y0;
		for (int x = 0; x < C.w; x++) {
			double sx = ((x + 0.5) / sc + offx) - 0.5;
			int x0 = (int)sx;
			unsigned fx = (unsigned)((sx - x0) * 256);
			if (sx < 0) { x0 = 0; fx = 0; }
			int x1 = x0 + 1 < (int)w ? x0 + 1 : x0;
			uint32_t px = 0;
			for (int c = 0; c < 3; c++) {
				const uint8_t *p = raw + c * plane;
				unsigned a = p[(size_t)y0 * w + x0] * (256 - fx) + p[(size_t)y0 * w + x1] * fx;
				unsigned b = p[(size_t)y1 * w + x0] * (256 - fx) + p[(size_t)y1 * w + x1] * fx;
				px |= (((a * (256 - fy) + b * fy) + 0x8000) >> 16) << (16 - 8 * c);
			}
			out[(size_t)y * C.w + x] = px;
		}
	}
	wall = out;
	out = NULL;
fail:
	free(out);
done:
	free(raw);
	free(fd);
}

/* the output changed size: scale the picture again */
void wall_reload(void)
{
	free(wall);
	wall = NULL;
	wall_init();
}

bool rect_intersect(struct rect a, struct rect b, struct rect *o)
{
	o->x1 = a.x1 > b.x1 ? a.x1 : b.x1;
	o->y1 = a.y1 > b.y1 ? a.y1 : b.y1;
	o->x2 = a.x2 < b.x2 ? a.x2 : b.x2;
	o->y2 = a.y2 < b.y2 ? a.y2 : b.y2;
	return !rect_empty(*o);
}

struct rect rect_union(struct rect a, struct rect b)
{
	if (rect_empty(a))
		return b;
	if (rect_empty(b))
		return a;
	struct rect r = {
		a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1,
		a.x2 > b.x2 ? a.x2 : b.x2, a.y2 > b.y2 ? a.y2 : b.y2,
	};
	return r;
}

static bool overlap(struct rect a, struct rect b)
{
	return a.x1 < b.x2 && b.x1 < a.x2 && a.y1 < b.y2 && b.y1 < a.y2;
}

/* A short list of rectangles; overlapping ones are merged, and when the list
 * is full it collapses into its bounding box. */
void dmg_add(struct damage *d, struct rect r)
{
	struct rect scr = { 0, 0, C.w, C.h };
	if (!rect_intersect(r, scr, &r))
		return;
	for (int i = 0; i < d->n; i++) {
		if (overlap(r, d->r[i])) {
			r = rect_union(r, d->r[i]);
			d->r[i] = d->r[--d->n];
			i = -1;
		}
	}
	if (d->n == DMG_MAX) {
		for (int i = 0; i < d->n; i++)
			r = rect_union(r, d->r[i]);
		d->n = 0;
	}
	d->r[d->n++] = r;
}

void damage_add(struct rect r)
{
	dmg_add(&C.dmg, r);
	if (C.dmg.n)
		schedule_repaint();
}

void damage_all(void)
{
	struct rect all = { 0, 0, C.w, C.h };
	C.dmg.n = C.prev.n = 0;
	dmg_add(&C.prev, all);
	damage_add(all);
}

/* ---- compositing ---------------------------------------------------- */

static inline uint32_t blend(uint32_t src, uint32_t dst)
{
	uint32_t a = src >> 24;
	if (a == 255)
		return src;
	if (a == 0)
		return dst;
	uint32_t ia = 255 - a;
	uint32_t rb = (dst & 0x00ff00ffu) * ia;
	rb = ((rb + 0x00800080u + ((rb >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
	uint32_t g = (dst & 0x0000ff00u) * ia;
	g = ((g + 0x00008000u + ((g >> 8) & 0x0000ff00u)) >> 8) & 0x0000ff00u;
	return 0xff000000u | ((src & 0x00ffffffu) + rb + g);
}

static void blit(struct surface *s, struct rect clip)
{
	int ox, oy;
	surface_origin(s, &ox, &oy);
	struct rect sr = { ox, oy, ox + s->w, oy + s->h }, r;
	if (!rect_intersect(sr, clip, &r))
		return;
	for (int y = r.y1; y < r.y2; y++) {
		uint32_t *dst = C.shadow + (size_t)y * C.w + r.x1;
		const uint32_t *src = s->px + (size_t)(y - oy) * s->w + (r.x1 - ox);
		int n = r.x2 - r.x1;
		if (!s->alpha) {
			memcpy(dst, src, (size_t)n * 4);
		} else {
			for (int i = 0; i < n; i++)
				dst[i] = blend(src[i], dst[i]);
		}
	}
}

void draw_tree(struct surface *s, struct rect clip)
{
	if (!s->px)
		return;
	blit(s, clip);
	struct subsurf *sub;
	wl_list_for_each(sub, &s->subs, link)
		draw_tree(sub->surf, clip);
}

static bool clean;	/* composite_clean: no switcher, indicator or pointer, for a screenshot */
static int skip_pid;	/* composite_clean_for: the windows of this process are left out */

static int surface_pid(struct surface *s)
{
	pid_t pid = 0;
	wl_client_get_credentials(wl_resource_get_client(s->res), &pid, NULL, NULL);
	return (int)pid;
}

const uint32_t *wall_pixels(void)
{
	return wall;
}

static void composite_rect(struct rect clip)
{
	if (C.locked) {
		/* the lock screen alone: a solid colour, the lock client's surface, the pointer */
		lock_draw_base(clip);
		if (C.lock_surf && C.lock_surf->px)
			draw_tree(C.lock_surf, clip);
		if (!clean)
			cursor_draw(clip);
		return;
	}
	/* performance mode (perf.c): a fullscreen window covers the screen, draw only that */
	struct window *fs = skip_pid ? NULL : perf_unredirected();
	if (fs) {
		draw_tree(fs->surf, clip);
		if (!clean) {
			switcher_draw(clip);
			osd_draw(clip);
			cursor_draw(clip);
		}
		return;
	}
	for (int y = clip.y1; y < clip.y2; y++) {
		uint32_t *p = C.shadow + (size_t)y * C.w;
		if (wall)
			memcpy(p + clip.x1, wall + (size_t)y * C.w + clip.x1, (size_t)(clip.x2 - clip.x1) * 4);
		else
			for (int x = clip.x1; x < clip.x2; x++)
				p[x] = BG;
	}
	if (desktop_visible())	/* the icons: above the wallpaper, below the windows */
		draw_tree(C.desktop, clip);
	struct window *w;
	wl_list_for_each_reverse(w, &C.windows, link)
		if (window_visible(w) && !(skip_pid && surface_pid(w->surf) == skip_pid)) {
			draw_tree(w->surf, clip);
			if (w->xw)
				xwm_deco_draw(w, clip);	/* the frame flcomp gives an X window */
		}
	overview_draw(clip);
	if (panel_visible())
		draw_tree(C.panel, clip);
	struct surface *xo[64];		/* menus and tooltips of X programs */
	int nxo = xwm_or_list(xo, 64);
	for (int i = 0; i < nxo; i++)
		draw_tree(xo[i], clip);
	struct popup *p;
	wl_list_for_each(p, &C.popups, link)
		if (popup_visible(p))
			draw_tree(p->surf, clip);
	if (menu_visible())
		draw_tree(C.menu, clip);
	if (flyout_visible())
		draw_tree(C.flyout, clip);
	if (notify_visible())
		draw_tree(C.notify, clip);
	if (clean)
		return;
	switcher_draw(clip);
	osd_draw(clip);
	cursor_draw(clip);
}

void composite_all(void)
{
	struct rect all = { 0, 0, C.w, C.h };
	composite_rect(all);
}

/* the desktop alone, in C.shadow (shot.c takes its copy), and then everything as it was */
void composite_clean(void)
{
	clean = true;
	composite_all();
	clean = false;
}

void composite_clean_for(int pid)
{
	skip_pid = pid;
	composite_clean();
	skip_pid = 0;
}

void composite_restore(void)
{
	composite_all();
}

/* ---- repaint loop ---------------------------------------------------- */

static bool repaint_pending;

uint32_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* The frame reached the screen: tell clients to draw the next one. */
void frame_done(void)
{
	flush_frames(now_ms());
	if (C.dmg.n)
		schedule_repaint();
}

static uint32_t last_repaint;
static struct wl_event_source *cap_timer;

static void repaint(void *data)
{
	(void)data;
	repaint_pending = false;
	last_repaint = now_ms();
	if (!C.active || backend_busy())
		return;
	if (C.dmg.n == 0) {
		frame_done();
		return;
	}
	xwm_sync();	/* X windows: positions, stacking */
	for (int i = 0; i < C.dmg.n; i++)
		composite_rect(C.dmg.r[i]);
	/* The buffer we draw into is two frames old: it needs this frame's
	 * damage and the previous one's. */
	struct damage cp = C.dmg;
	for (int i = 0; i < C.prev.n; i++)
		dmg_add(&cp, C.prev.r[i]);
	C.prev = C.dmg;
	C.dmg.n = 0;
	backend_present(&cp);
}


static int cap_cb(void *data)
{
	repaint(NULL);
	return 0;
}

void schedule_repaint(void)
{
	if (repaint_pending || !C.active || backend_busy())
		return;
	repaint_pending = true;
	/* power saving: at most one frame every power_frame_gap_ms() */
	int gap = power_frame_gap_ms();
	uint32_t since = now_ms() - last_repaint;
	if (gap > 0 && since < (uint32_t)gap) {
		if (!cap_timer)
			cap_timer = wl_event_loop_add_timer(C.loop, cap_cb, NULL);
		if (cap_timer) {
			wl_event_source_timer_update(cap_timer, gap - (int)since);
			return;
		}
	}
	wl_event_loop_add_idle(C.loop, repaint, NULL);
}

/* ---- screenshot ------------------------------------------------------ */

int write_screenshot(const char *path)
{
	composite_all();
	char tmp[4096];
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return -1;
	fprintf(f, "P6\n%d %d\n255\n", C.w, C.h);
	unsigned char *row = malloc((size_t)C.w * 3);
	if (!row) {
		fclose(f);
		return -1;
	}
	for (int y = 0; y < C.h; y++) {
		const uint32_t *p = C.shadow + (size_t)y * C.w;
		for (int x = 0; x < C.w; x++) {
			row[x * 3] = (p[x] >> 16) & 0xff;
			row[x * 3 + 1] = (p[x] >> 8) & 0xff;
			row[x * 3 + 2] = p[x] & 0xff;
		}
		fwrite(row, 3, (size_t)C.w, f);
	}
	free(row);
	if (fclose(f))
		return -1;
	return rename(tmp, path);
}
