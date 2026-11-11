/* flpdf -- the FreeLFS document viewer ("Documents")
 * SPDX-License-Identifier: MIT
 *
 * PDF (and whatever else MuPDF opens: XPS, EPUB, CBZ) on libflui. Continuous
 * vertical scrolling, zoom, fit width / fit page, a table of contents,
 * text search with highlights and clickable links. MuPDF renders straight into
 * the window's picture buffer; only the pages on screen are drawn, from cached
 * display lists, so memory stays small (the glyph/image store is capped).
 */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mupdf/fitz.h>

#include <flui.h>

#define TOOL_H 44
#define TOC_W 270
#define GAP 14          /* between pages, screen px */
#define MARGIN 16       /* around the pages */
#define SB_W 12
#define DL_CACHE 8
#define STORE_BYTES (24u << 20)
#define MAX_HITS 256
#define PTS_TO_PX (96.0 / 72.0) /* zoom 1.0 = 100 % = 96 dpi */

struct hits {
	int n;
	fz_quad q[MAX_HITS];
};

struct dlent {
	int page; /* -1: free */
	fz_display_list *dl;
	unsigned used;
};

struct toc {
	char *title;
	int depth, page;
	float y;
};

enum { FIT_NONE, FIT_WIDTH, FIT_PAGE };

enum {
	ID_CANVAS = 1, ID_TOCBTN, ID_PREV, ID_NEXT, ID_PAGE, ID_ZOUT, ID_ZIN, ID_WIDTH, ID_FITPAGE,
	ID_SEARCH, ID_SPREV, ID_SNEXT, ID_TOC, ID_SCROLL
};

struct P {
	fl_app *app;
	fl_win *win;
	fl_font *small;
	fl_timer *msg_timer, *size_timer;
	fz_context *ctx;
	fz_document *doc;
	char path[PATH_MAX];
	char err[256];
	int npages;
	/* page sizes in points; sized == how many have been measured */
	float *pw, *ph, *px0, *py0;
	int sized;
	float def_w, def_h;
	/* layout at the current zoom */
	double zoom;
	int fit;
	double *top;      /* top[i]: y of page i in document pixels, top[npages] = total height */
	double total_w;   /* widest page, document pixels */
	int dirty_layout;
	double sx, sy;    /* top left of the viewport in document pixels */
	int cw, ch;       /* viewport size of the last layout */
	/* rendering */
	uint32_t *vbuf;
	size_t vcap;
	int vvalid, vw, vh;
	struct { double zoom, sx, sy; int w, h; unsigned gen; } vkey;
	unsigned gen;     /* changes when the picture must be redone: search, layout */
	struct dlent dl[DL_CACHE];
	unsigned tick;
	/* contents */
	struct toc *toc;
	int ntoc;
	int show_toc;
	fl_list_state toc_st;
	/* search */
	char needle[128], active_needle[128];
	struct hits **hits;
	int hit_page, hit_idx;
	/* input */
	int drag, drag_x, drag_y, drag_moved;
	char pagebuf[16];
	char msg[300];
	int msg_err;
};

/* ---- small helpers ----------------------------------------------------------------------- */

static void say(struct P *p, int err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(p->msg, sizeof p->msg, fmt, ap);
	va_end(ap);
	p->msg_err = err;
	fl_timer_arm(p->msg_timer, 4000, 0);
}

static void msg_clear(void *ud)
{
	struct P *p = ud;
	p->msg[0] = 0;
	fl_win_redraw(p->win);
}

static double page_w(const struct P *p, int i)
{
	return (i < p->sized ? p->pw[i] : p->def_w) * p->zoom * PTS_TO_PX;
}

static double page_h(const struct P *p, int i)
{
	return (i < p->sized ? p->ph[i] : p->def_h) * p->zoom * PTS_TO_PX;
}

static void relayout(struct P *p)
{
	p->total_w = 0;
	double y = MARGIN;
	for (int i = 0; i < p->npages; i++) {
		p->top[i] = y;
		y += page_h(p, i) + GAP;
		double w = page_w(p, i);
		if (w > p->total_w)
			p->total_w = w;
	}
	p->top[p->npages] = y - GAP + MARGIN;
	p->dirty_layout = 0;
}

static int page_at(const struct P *p, double y)
{
	int lo = 0, hi = p->npages - 1;
	while (lo < hi) {
		int mid = (lo + hi + 1) / 2;
		if (p->top[mid] <= y)
			lo = mid;
		else
			hi = mid - 1;
	}
	return lo;
}

/* page under the middle of the viewport */
static int cur_page(const struct P *p)
{
	if (p->npages == 0)
		return 0;
	return page_at(p, p->sy + p->ch / 2.0);
}

static void clamp_scroll(struct P *p)
{
	double maxy = p->top[p->npages] - p->ch, maxx = p->total_w + 2 * MARGIN - p->cw;
	if (maxy < 0)
		maxy = 0;
	if (maxx < 0)
		maxx = 0;
	if (p->sy > maxy)
		p->sy = maxy;
	if (p->sy < 0)
		p->sy = 0;
	if (p->sx > maxx)
		p->sx = maxx;
	if (p->sx < 0)
		p->sx = 0;
}

/* x of the left edge of page i in document pixels */
static double page_x(const struct P *p, int i)
{
	double span = p->total_w + 2 * MARGIN;
	double w = page_w(p, i);
	double base = span < p->cw ? (p->cw - span) / 2.0 : 0;
	return base + MARGIN + (p->total_w - w) / 2.0;
}

static double fit_zoom_for(const struct P *p, int i, int mode)
{
	double w = (i < p->sized ? p->pw[i] : p->def_w) * PTS_TO_PX;
	double h = (i < p->sized ? p->ph[i] : p->def_h) * PTS_TO_PX;
	if (w < 1 || h < 1)
		return 1;
	double zw = (p->cw - 2 * MARGIN - (p->npages > 1 ? SB_W : 0)) / w;
	if (mode == FIT_WIDTH)
		return zw;
	double zh = (p->ch - 2 * MARGIN) / h;
	return fmin(zw, zh);
}

/* Set the zoom keeping the document point at viewport (fx, fy) where it is. */
static void set_zoom(struct P *p, double z, double fx, double fy)
{
	if (p->npages == 0)
		return;
	if (z < 0.1)
		z = 0.1;
	if (z > 8)
		z = 8;
	/* anchor: page and the fraction of it under the point */
	double ay = p->sy + fy;
	int pg = page_at(p, ay);
	double frac = p->ph ? (ay - p->top[pg]) / fmax(page_h(p, pg), 1) : 0;
	double fxd = (p->sx + fx) / fmax(p->total_w + 2 * MARGIN, 1);
	p->zoom = z;
	relayout(p);
	p->sy = p->top[pg] + frac * page_h(p, pg) - fy;
	p->sx = fxd * (p->total_w + 2 * MARGIN) - fx;
	clamp_scroll(p);
	p->gen++;
}

static void goto_page(struct P *p, int n, double frac)
{
	if (n < 0)
		n = 0;
	if (n >= p->npages)
		n = p->npages - 1;
	if (p->npages == 0)
		return;
	p->sy = p->top[n] + frac * page_h(p, n) - (frac > 0 ? 40 : MARGIN);
	clamp_scroll(p);
	if (p->fit == FIT_PAGE)
		p->sy = p->top[n] - MARGIN, clamp_scroll(p);
}

/* ---- document ------------------------------------------------------------------------------ */

static struct dlent *dl_get(struct P *p, int page)
{
	struct dlent *free_e = NULL, *oldest = NULL;
	for (int i = 0; i < DL_CACHE; i++) {
		struct dlent *e = &p->dl[i];
		if (e->page == page && e->dl) {
			e->used = ++p->tick;
			return e;
		}
		if (!e->dl && !free_e)
			free_e = e;
		if (e->dl && (!oldest || e->used < oldest->used))
			oldest = e;
	}
	struct dlent *e = free_e ? free_e : oldest;
	if (e->dl)
		fz_drop_display_list(p->ctx, e->dl);
	e->dl = NULL;
	e->page = -1;
	fz_try(p->ctx)
		e->dl = fz_new_display_list_from_page_number(p->ctx, p->doc, page);
	fz_catch(p->ctx)
		e->dl = NULL;
	if (!e->dl)
		return NULL;
	e->page = page;
	e->used = ++p->tick;
	return e;
}

static void size_tick(void *ud)
{
	struct P *p = ud;
	int upto = p->sized + 40;
	if (upto > p->npages)
		upto = p->npages;
	int cur = cur_page(p);
	double anchor_off = p->sy - p->top[cur];
	while (p->sized < upto) {
		int i = p->sized;
		fz_page *pg = NULL;
		fz_rect b = {0, 0, 595, 842};
		fz_try(p->ctx) {
			pg = fz_load_page(p->ctx, p->doc, i);
			b = fz_bound_page(p->ctx, pg);
		}
		fz_always(p->ctx)
			fz_drop_page(p->ctx, pg);
		fz_catch(p->ctx) {}
		p->pw[i] = b.x1 - b.x0 > 1 ? b.x1 - b.x0 : p->def_w;
		p->ph[i] = b.y1 - b.y0 > 1 ? b.y1 - b.y0 : p->def_h;
		p->px0[i] = b.x0;
		p->py0[i] = b.y0;
		p->sized++;
	}
	/* keep the page on top where it was */
	relayout(p);
	p->sy = p->top[cur] + anchor_off;
	clamp_scroll(p);
	p->gen++;
	if (p->sized >= p->npages)
		fl_timer_arm(p->size_timer, 0, 0);
	fl_win_redraw(p->win);
}

static void add_outline(struct P *p, fz_outline *o, int depth, int *cap)
{
	for (; o; o = o->next) {
		if (p->ntoc == *cap) {
			*cap = *cap ? *cap * 2 : 64;
			p->toc = realloc(p->toc, (size_t)*cap * sizeof *p->toc);
			if (!p->toc)
				return;
		}
		int page = o->page.page;
		if (page < 0 && o->uri) {
			float x, y;
			fz_try(p->ctx) {
				fz_location loc = fz_resolve_link(p->ctx, p->doc, o->uri, &x, &y);
				page = fz_page_number_from_location(p->ctx, p->doc, loc);
			}
			fz_catch(p->ctx)
				page = -1;
		}
		p->toc[p->ntoc++] = (struct toc){.title = strdup(o->title ? o->title : ""), .depth = depth,
		                                 .page = page, .y = o->y};
		if (o->down)
			add_outline(p, o->down, depth + 1, cap);
	}
}

static int open_doc(struct P *p, const char *path)
{
	p->ctx = fz_new_context(NULL, NULL, STORE_BYTES);
	if (!p->ctx) {
		snprintf(p->err, sizeof p->err, "Out of memory");
		return -1;
	}
	fz_try(p->ctx) {
		fz_register_document_handlers(p->ctx);
		p->doc = fz_open_document(p->ctx, path);
		if (fz_needs_password(p->ctx, p->doc))
			fz_throw(p->ctx, FZ_ERROR_ARGUMENT, "this document is password protected");
		p->npages = fz_count_pages(p->ctx, p->doc);
	}
	fz_catch(p->ctx) {
		snprintf(p->err, sizeof p->err, "Cannot open %s: %s", path, fz_caught_message(p->ctx));
		if (p->doc)
			fz_drop_document(p->ctx, p->doc);
		p->doc = NULL;
		return -1;
	}
	if (p->npages < 1) {
		snprintf(p->err, sizeof p->err, "%s has no pages", path);
		return -1;
	}
	p->pw = calloc((size_t)p->npages, sizeof(float));
	p->ph = calloc((size_t)p->npages, sizeof(float));
	p->px0 = calloc((size_t)p->npages, sizeof(float));
	p->py0 = calloc((size_t)p->npages, sizeof(float));
	p->top = calloc((size_t)p->npages + 1, sizeof(double));
	p->hits = calloc((size_t)p->npages, sizeof(struct hits *));
	if (!p->pw || !p->ph || !p->px0 || !p->py0 || !p->top || !p->hits) {
		snprintf(p->err, sizeof p->err, "Out of memory");
		return -1;
	}
	p->def_w = 595;
	p->def_h = 842;
	/* the first page now, so the window opens at the right size; the rest follows on timer ticks */
	fz_page *pg = NULL;
	fz_try(p->ctx) {
		pg = fz_load_page(p->ctx, p->doc, 0);
		fz_rect b = fz_bound_page(p->ctx, pg);
		if (b.x1 - b.x0 > 1 && b.y1 - b.y0 > 1) {
			p->def_w = b.x1 - b.x0;
			p->def_h = b.y1 - b.y0;
		}
		p->px0[0] = b.x0;
		p->py0[0] = b.y0;
	}
	fz_always(p->ctx)
		fz_drop_page(p->ctx, pg);
	fz_catch(p->ctx) {}
	p->pw[0] = p->def_w;
	p->ph[0] = p->def_h;
	p->sized = 1;
	fz_outline *o = NULL;
	fz_try(p->ctx) {
		o = fz_load_outline(p->ctx, p->doc);
		int cap = 0;
		add_outline(p, o, 0, &cap);
	}
	fz_always(p->ctx)
		fz_drop_outline(p->ctx, o);
	fz_catch(p->ctx) {}
	for (int i = 0; i < DL_CACHE; i++)
		p->dl[i].page = -1;
	snprintf(p->path, sizeof p->path, "%s", path);
	return 0;
}

/* ---- search -------------------------------------------------------------------------------- */

static struct hits *hits_for(struct P *p, int page)
{
	if (!p->active_needle[0] || page < 0 || page >= p->npages)
		return NULL;
	if (!p->hits[page]) {
		struct hits *h = calloc(1, sizeof *h);
		if (!h)
			return NULL;
		fz_try(p->ctx)
			h->n = fz_search_page_number(p->ctx, p->doc, page, p->active_needle, NULL, h->q, MAX_HITS);
		fz_catch(p->ctx)
			h->n = 0;
		p->hits[page] = h;
	}
	return p->hits[page];
}

static void clear_hits(struct P *p)
{
	for (int i = 0; i < p->npages; i++) {
		free(p->hits[i]);
		p->hits[i] = NULL;
	}
	p->hit_page = -1;
	p->hit_idx = 0;
	p->gen++;
}

static void show_hit(struct P *p)
{
	struct hits *h = hits_for(p, p->hit_page);
	if (!h || p->hit_idx >= h->n)
		return;
	fz_rect r = fz_rect_from_quad(h->q[p->hit_idx]);
	double s = p->zoom * PTS_TO_PX;
	int pg = p->hit_page;
	double y0 = p->top[pg] + (r.y0 - p->py0[pg]) * s, y1 = p->top[pg] + (r.y1 - p->py0[pg]) * s;
	double x0 = page_x(p, pg) + (r.x0 - p->px0[pg]) * s, x1 = page_x(p, pg) + (r.x1 - p->px0[pg]) * s;
	if (y0 < p->sy + 30 || y1 > p->sy + p->ch - 30)
		p->sy = (y0 + y1) / 2 - p->ch / 2.0;
	if (x0 < p->sx + 30 || x1 > p->sx + p->cw - 30)
		p->sx = (x0 + x1) / 2 - p->cw / 2.0;
	clamp_scroll(p);
	p->gen++;
}

/* dir +1: next hit, -1: previous; starts from the current hit, else from the page on screen */
static void find(struct P *p, int dir)
{
	if (!p->needle[0]) {
		if (p->active_needle[0]) {
			p->active_needle[0] = 0;
			clear_hits(p);
		}
		return;
	}
	if (strcmp(p->needle, p->active_needle)) {
		snprintf(p->active_needle, sizeof p->active_needle, "%s", p->needle);
		clear_hits(p);
	}
	int start = p->hit_page >= 0 ? p->hit_page : cur_page(p);
	int idx = p->hit_page >= 0 ? p->hit_idx + dir : (dir > 0 ? 0 : -1);
	for (int step = 0; step <= p->npages; step++) {
		int pg = ((start + dir * step) % p->npages + p->npages) % p->npages;
		struct hits *h = hits_for(p, pg);
		if (h && h->n > 0) {
			int i;
			if (step == 0 && p->hit_page >= 0)
				i = idx;
			else
				i = dir > 0 ? 0 : h->n - 1;
			if (step == 0 && p->hit_page < 0 && dir < 0)
				i = h->n - 1;
			if (i >= 0 && i < h->n) {
				p->hit_page = pg;
				p->hit_idx = i;
				show_hit(p);
				say(p, 0, "Page %d, %d match%s on it", pg + 1, h->n, h->n == 1 ? "" : "es");
				return;
			}
		}
	}
	p->hit_page = -1;
	say(p, 1, "No matches for \"%s\"", p->active_needle);
}

/* ---- rendering ----------------------------------------------------------------------------- */

static void render(struct P *p, int cw, int ch)
{
	if (p->vcap < (size_t)cw * (size_t)ch) {
		free(p->vbuf);
		p->vbuf = malloc((size_t)cw * (size_t)ch * 4);
		p->vcap = p->vbuf ? (size_t)cw * (size_t)ch : 0;
		if (!p->vbuf)
			return;
	}
	p->vw = cw;
	p->vh = ch;
	uint32_t bg = fl_th.bg;
	for (size_t i = 0; i < (size_t)cw * (size_t)ch; i++)
		p->vbuf[i] = bg;
	fz_pixmap *pix = NULL;
	fz_try(p->ctx)
		pix = fz_new_pixmap_with_data(p->ctx, fz_device_bgr(p->ctx), cw, ch, NULL, 1, cw * 4,
		                              (unsigned char *)p->vbuf);
	fz_catch(p->ctx)
		pix = NULL;
	if (!pix)
		return;
	double s = p->zoom * PTS_TO_PX;
	int first = page_at(p, p->sy), last = page_at(p, p->sy + ch);
	for (int i = first; i <= last && i < p->npages; i++) {
		double x = page_x(p, i) - p->sx, y = p->top[i] - p->sy;
		int w = (int)ceil(page_w(p, i)), h = (int)ceil(page_h(p, i));
		fz_irect clip = {(int)fmax(x, 0), (int)fmax(y, 0), (int)fmin(x + w, cw), (int)fmin(y + h, ch)};
		if (clip.x1 <= clip.x0 || clip.y1 <= clip.y0)
			continue;
		for (int yy = clip.y0; yy < clip.y1; yy++)
			for (int xx = clip.x0; xx < clip.x1; xx++)
				p->vbuf[(size_t)yy * (size_t)cw + (size_t)xx] = 0xffffffffu;
		if (i >= p->sized)
			continue; /* size not known yet: a blank page */
		struct dlent *e = dl_get(p, i);
		if (!e)
			continue; /* a page MuPDF cannot read stays blank */
		fz_device *dev = NULL;
		fz_try(p->ctx) {
			fz_matrix ctm = fz_concat(fz_scale((float)s, (float)s),
			                          fz_translate((float)(x - p->px0[i] * s), (float)(y - p->py0[i] * s)));
			dev = fz_new_draw_device_with_bbox(p->ctx, fz_identity, pix, &clip);
			fz_run_display_list(p->ctx, e->dl, dev, ctm, fz_make_rect((float)clip.x0, (float)clip.y0, (float)clip.x1, (float)clip.y1), NULL);
			fz_close_device(p->ctx, dev);
		}
		fz_always(p->ctx)
			fz_drop_device(p->ctx, dev);
		fz_catch(p->ctx) {}
		/* search hits */
		struct hits *hs = p->active_needle[0] ? hits_for(p, i) : NULL;
		for (int k = 0; hs && k < hs->n; k++) {
			fz_rect r = fz_rect_from_quad(hs->q[k]);
			int rx0 = (int)(x + (r.x0 - p->px0[i]) * s), rx1 = (int)ceil(x + (r.x1 - p->px0[i]) * s);
			int ry0 = (int)(y + (r.y0 - p->py0[i]) * s), ry1 = (int)ceil(y + (r.y1 - p->py0[i]) * s);
			int here = i == p->hit_page && k == p->hit_idx;
			uint32_t col = here ? (fl_th.accent & 0x00ffffffu) : 0x00ffd24a;
			unsigned alpha = here ? 110 : 90;
			for (int yy = ry0 < clip.y0 ? clip.y0 : ry0; yy < ry1 && yy < clip.y1; yy++)
				for (int xx = rx0 < clip.x0 ? clip.x0 : rx0; xx < rx1 && xx < clip.x1; xx++) {
					uint32_t *d = &p->vbuf[(size_t)yy * (size_t)cw + (size_t)xx];
					uint32_t o = *d;
					uint32_t rr = (((o >> 16) & 255) * (255 - alpha) + ((col >> 16) & 255) * alpha) / 255;
					uint32_t gg = (((o >> 8) & 255) * (255 - alpha) + ((col >> 8) & 255) * alpha) / 255;
					uint32_t bb = ((o & 255) * (255 - alpha) + (col & 255) * alpha) / 255;
					*d = 0xff000000u | rr << 16 | gg << 8 | bb;
				}
			if (here) {
				fl_canvas tmp = {.px = p->vbuf, .w = cw, .h = ch, .stride = cw, .clip = fl_r(0, 0, cw, ch)};
				fl_stroke_rect(&tmp, fl_r(rx0, ry0, rx1 - rx0, ry1 - ry0), 2, fl_th.accent);
			}
		}
	}
	fz_drop_pixmap(p->ctx, pix);
	p->vkey.zoom = p->zoom;
	p->vkey.sx = p->sx;
	p->vkey.sy = p->sy;
	p->vkey.w = cw;
	p->vkey.h = ch;
	p->vkey.gen = p->gen;
	p->vvalid = 1;
}

static void blit(fl_canvas *cv, fl_rect r, const uint32_t *src, int sw)
{
	if (!cv->px)
		return;
	fl_rect c = fl_intersect(r, cv->clip);
	for (int y = c.y; y < c.y + c.h; y++)
		memcpy(cv->px + (size_t)y * (size_t)cv->stride + (size_t)c.x,
		       src + (size_t)(y - r.y) * (size_t)sw + (size_t)(c.x - r.x), (size_t)c.w * 4);
}

/* ---- links --------------------------------------------------------------------------------- */

/* a click at viewport (mx, my) */
static void click_at(struct P *p, int mx, int my)
{
	double dy = p->sy + my, dx = p->sx + mx;
	int pg = page_at(p, dy);
	if (pg >= p->sized || dy > p->top[pg] + page_h(p, pg) || dy < p->top[pg])
		return;
	double s = p->zoom * PTS_TO_PX;
	float fx = (float)((dx - page_x(p, pg)) / s + p->px0[pg]);
	float fy = (float)((dy - p->top[pg]) / s + p->py0[pg]);
	fz_page *page = NULL;
	fz_link *links = NULL;
	char uri[512] = "";
	fz_try(p->ctx) {
		page = fz_load_page(p->ctx, p->doc, pg);
		links = fz_load_links(p->ctx, page);
		for (fz_link *l = links; l; l = l->next)
			if (fx >= l->rect.x0 && fx <= l->rect.x1 && fy >= l->rect.y0 && fy <= l->rect.y1) {
				snprintf(uri, sizeof uri, "%s", l->uri ? l->uri : "");
				break;
			}
	}
	fz_always(p->ctx) {
		fz_drop_link(p->ctx, links);
		fz_drop_page(p->ctx, page);
	}
	fz_catch(p->ctx) {}
	if (!uri[0])
		return;
	if (fz_is_external_link(p->ctx, uri)) {
		fl_clipboard_set(p->app, uri, strlen(uri));
		say(p, 0, "Link copied: %s", uri);
		return;
	}
	float x = 0, y = 0;
	int target = -1;
	fz_try(p->ctx) {
		fz_location loc = fz_resolve_link(p->ctx, p->doc, uri, &x, &y);
		target = fz_page_number_from_location(p->ctx, p->doc, loc);
	}
	fz_catch(p->ctx)
		target = -1;
	if (target >= 0) {
		if (target < p->sized && p->ph[target] > 1)
			goto_page(p, target, (y - p->py0[target]) / p->ph[target]);
		else
			goto_page(p, target, 0);
		p->fit = p->fit == FIT_PAGE ? FIT_PAGE : FIT_NONE;
	}
}

/* ---- drawing ------------------------------------------------------------------------------- */

static void toc_item(fl_ui *ui, fl_rect r, int i, int hover, int selected, void *ud)
{
	struct P *p = ud;
	fl_canvas *cv = &ui->cv;
	if (selected || hover)
		fl_fill_rect(cv, r, selected ? fl_th.select : fl_th.raised);
	int cur = cur_page(p);
	int active = p->toc[i].page == cur;
	char fit[300];
	int x = r.x + 10 + p->toc[i].depth * 12;
	fl_text_fit(ui->f_reg, p->toc[i].title, r.x + r.w - x - 8, fit, sizeof fit);
	fl_text_in(cv, ui->f_reg, fl_r(x, r.y, r.x + r.w - x, r.h), FL_LEFT, 0, active ? fl_th.strong : fl_th.text, fit);
}

/* A chevron button (the icon set has no down arrow). */
static int chevron(fl_ui *ui, uint32_t id, fl_rect r, int up)
{
	int res = fl_widget(ui, id, r, FL_FOCUSABLE);
	if (res & (FL_HOVER | FL_DOWN))
		fl_fill_rect(&ui->cv, r, fl_th.raised);
	if (res & FL_FOCUS)
		fl_stroke_rect(&ui->cv, r, 1, fl_th.accent);
	fl_path *pa = fl_path_new();
	float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f;
	float dy = up ? 2.5f : -2.5f;
	fl_path_move(pa, cx - 5, cy + dy);
	fl_path_line(pa, cx, cy - dy);
	fl_path_line(pa, cx + 5, cy + dy);
	fl_path_stroke(&ui->cv, pa, 1.8f, fl_th.text);
	fl_path_free(pa);
	return res & FL_CLICK;
}

static void keys(fl_ui *ui, struct P *p, fl_rect view)
{
	uint32_t k = ui->key;
	if (!k || ui->focus == ID_SEARCH || ui->focus == ID_PAGE)
		return;
	int mods = fl_app_mods(p->app), ctrl = mods & FL_MOD_CTRL, shift = mods & FL_MOD_SHIFT;
	int handled = 1;
	int cur = cur_page(p);
	double step = 60;
	switch (k) {
	case XKB_KEY_Down: p->sy += step; break;
	case XKB_KEY_Up: p->sy -= step; break;
	case XKB_KEY_Left: p->sx -= step; break;
	case XKB_KEY_Right: p->sx += step; break;
	case XKB_KEY_Page_Down:
	case XKB_KEY_space:
		if (shift && k == XKB_KEY_space)
			p->sy -= view.h * 0.9;
		else
			p->sy += view.h * 0.9;
		break;
	case XKB_KEY_Page_Up: p->sy -= view.h * 0.9; break;
	case XKB_KEY_Home: p->sy = 0; break;
	case XKB_KEY_End: p->sy = p->top[p->npages]; break;
	case XKB_KEY_n:
	case XKB_KEY_N: goto_page(p, cur + 1, 0); break;
	case XKB_KEY_p:
	case XKB_KEY_P: goto_page(p, cur - 1, 0); break;
	case XKB_KEY_plus:
	case XKB_KEY_equal:
	case XKB_KEY_KP_Add:
		p->fit = FIT_NONE;
		set_zoom(p, p->zoom * 1.2, view.w / 2.0, view.h / 2.0);
		break;
	case XKB_KEY_minus:
	case XKB_KEY_KP_Subtract:
		p->fit = FIT_NONE;
		set_zoom(p, p->zoom / 1.2, view.w / 2.0, view.h / 2.0);
		break;
	case XKB_KEY_0:
	case XKB_KEY_KP_0:
		p->fit = FIT_WIDTH;
		break;
	case XKB_KEY_9: p->fit = FIT_PAGE; break;
	case XKB_KEY_1:
		p->fit = FIT_NONE;
		set_zoom(p, 1.0, view.w / 2.0, view.h / 2.0);
		break;
	case XKB_KEY_f:
	case XKB_KEY_F:
		if (ctrl) {
			ui->focus = ID_SEARCH;
			break;
		}
		handled = 0;
		break;
	case XKB_KEY_slash: ui->focus = ID_SEARCH; break;
	case XKB_KEY_F3: find(p, shift ? -1 : 1); break;
	case XKB_KEY_g:
	case XKB_KEY_G:
		if (ctrl)
			find(p, shift ? -1 : 1);
		else
			handled = 0;
		break;
	case XKB_KEY_F9:
	case XKB_KEY_c:
		if (p->ntoc)
			p->show_toc = !p->show_toc;
		break;
	case XKB_KEY_q:
	case XKB_KEY_Escape:
		if (k == XKB_KEY_Escape && p->active_needle[0]) {
			p->needle[0] = 0;
			find(p, 1);
		} else
			fl_app_quit(p->app);
		break;
	default: handled = 0;
	}
	if (handled)
		ui->key = 0;
	clamp_scroll(p);
}

static void draw(fl_ui *ui, void *ud)
{
	struct P *p = ud;
	fl_canvas *cv = &ui->cv;
	fl_rect c = fl_win_content(ui->win);
	fl_fill_rect(cv, c, fl_th.bg);
	fl_rect area = c;
	fl_rect tb = fl_cut_top(&area, TOOL_H);
	if (!p->doc) {
		fl_text_in(cv, ui->f_reg, area, FL_CENTER, 0, fl_th.err, p->err[0] ? p->err : "No document");
		return;
	}
	fl_rect side = {0};
	if (p->show_toc && p->ntoc)
		side = fl_cut_left(&area, TOC_W);
	fl_rect canvas = area;
	int vw_changed = p->cw != canvas.w || p->ch != canvas.h;
	p->cw = canvas.w;
	p->ch = canvas.h;
	if (p->fit != FIT_NONE) {
		int cur = cur_page(p);
		double z = fit_zoom_for(p, cur, p->fit);
		if (fabs(z - p->zoom) > 1e-6 || vw_changed) {
			if (p->zoom < 1e-6) {
				p->zoom = z;
				relayout(p);
			} else {
				set_zoom(p, z, 0, 0);
			}
			if (p->fit == FIT_PAGE)
				goto_page(p, cur, 0);
		}
	}
	if (p->dirty_layout)
		relayout(p);

	keys(ui, p, fl_r(0, 0, canvas.w, canvas.h));

	/* the document: wheel scrolls (Ctrl: zoom at the pointer), drag pans, click follows a link */
	int res = fl_widget(ui, ID_CANVAS, canvas, FL_FOCUSABLE | FL_NO_FOCUS_RING);
	int mx = ui->mx - canvas.x, my = ui->my - canvas.y;
	if ((res & FL_HOVER) && ui->wheel) {
		if (fl_app_mods(p->app) & FL_MOD_CTRL) {
			p->fit = FIT_NONE;
			set_zoom(p, p->zoom * pow(1.15, -ui->wheel), mx, my);
		} else {
			p->sy += ui->wheel * 70;
		}
		ui->wheel = 0;
	}
	if ((ui->press & FL_BTN_L) && fl_in(canvas, ui->mx, ui->my) && ui->mx < canvas.x + canvas.w - SB_W) {
		p->drag = 1;
		p->drag_moved = 0;
		p->drag_x = ui->mx;
		p->drag_y = ui->my;
	}
	if (p->drag && (ui->btn_held & FL_BTN_L)) {
		int dx = ui->mx - p->drag_x, dy = ui->my - p->drag_y;
		if (dx || dy) {
			p->sx -= dx;
			p->sy -= dy;
			p->drag_moved += abs(dx) + abs(dy);
			p->drag_x = ui->mx;
			p->drag_y = ui->my;
		}
	} else if (p->drag) {
		if (p->drag_moved < 5)
			click_at(p, mx, my);
		p->drag = 0;
	}
	clamp_scroll(p);

	int stale = !p->vvalid || p->vkey.zoom != p->zoom || p->vkey.sx != p->sx || p->vkey.sy != p->sy ||
	            p->vkey.w != canvas.w || p->vkey.h != canvas.h || p->vkey.gen != p->gen;
	if (stale && cv->px && canvas.w > 0 && canvas.h > 0)
		render(p, canvas.w, canvas.h);
	if (p->vvalid && p->vw == canvas.w && p->vh == canvas.h)
		blit(cv, canvas, p->vbuf, p->vw);

	/* scroll bar */
	int off = (int)p->sy, content = (int)p->top[p->npages];
	if (content > canvas.h) {
		fl_rect sb = fl_r(canvas.x + canvas.w - SB_W, canvas.y, SB_W, canvas.h);
		if (fl_scrollbar(ui, ID_SCROLL, sb, &off, content, canvas.h)) {
			p->sy = off;
			clamp_scroll(p);
		}
	}

	/* contents */
	if (side.w) {
		fl_fill_rect(cv, side, fl_th.panel);
		fl_fill_rect(cv, fl_r(side.x + side.w - 1, side.y, 1, side.h), fl_th.line);
		fl_rect lr = fl_r(side.x, side.y + 6, side.w - 1, side.h - 6);
		int pick = fl_list(ui, ID_TOC, lr, p->ntoc, 28, &p->toc_st, toc_item, p);
		if (pick >= 0 && p->toc[pick].page >= 0) {
			int pg = p->toc[pick].page;
			p->fit = p->fit == FIT_PAGE ? FIT_PAGE : FIT_NONE;
			if (pg < p->sized && p->ph[pg] > 1 && p->toc[pick].y > 0)
				goto_page(p, pg, (p->toc[pick].y - p->py0[pg]) / p->ph[pg]);
			else
				goto_page(p, pg, 0);
		}
	}

	/* toolbar */
	fl_fill_rect(cv, tb, fl_th.panel);
	fl_fill_rect(cv, fl_r(tb.x, tb.y + tb.h - 1, tb.w, 1), fl_th.line);
	fl_rect t = fl_inset(tb, 8, 6);
	int cur = cur_page(p);
	if (p->ntoc) {
		fl_rect b = fl_cut_left(&t, 82);
		if (fl_button(ui, ID_TOCBTN, b, "Contents", FL_BTN_PLAIN))
			p->show_toc = !p->show_toc;
		if (p->show_toc)
			fl_fill_rect(cv, fl_r(b.x + 10, b.y + b.h - 3, b.w - 20, 2), fl_th.accent);
		t.x += 6;
		t.w -= 6;
	}
	if (chevron(ui, ID_PREV, fl_cut_left(&t, 32), 1))
		goto_page(p, cur - 1, 0);
	t.x += 2;
	t.w -= 2;
	if (chevron(ui, ID_NEXT, fl_cut_left(&t, 32), 0))
		goto_page(p, cur + 1, 0);
	t.x += 6;
	t.w -= 6;
	if (ui->focus != ID_PAGE)
		snprintf(p->pagebuf, sizeof p->pagebuf, "%d", cur + 1);
	int pe = fl_textfield(ui, ID_PAGE, fl_cut_left(&t, 52), p->pagebuf, sizeof p->pagebuf, NULL, ui->f_reg);
	if (pe & FL_EDIT_ENTER) {
		int n = atoi(p->pagebuf);
		if (n >= 1 && n <= p->npages)
			goto_page(p, n - 1, 0);
		ui->focus = ID_CANVAS;
	}
	char of[32];
	snprintf(of, sizeof of, "of %d", p->npages);
	fl_text_in(cv, p->small, fl_cut_left(&t, 64), FL_CENTER, 0, fl_th.dim, of);
	/* the right side: search */
	int wide = tb.w > 900;
	fl_rect sn = fl_cut_right(&t, 30);
	t.w -= 2;
	fl_rect sp = fl_cut_right(&t, 30);
	t.w -= 8;
	fl_rect sf = fl_cut_right(&t, wide ? 230 : 150);
	t.w -= 12;
	if (chevron(ui, ID_SNEXT, sn, 0))
		find(p, 1);
	if (chevron(ui, ID_SPREV, sp, 1))
		find(p, -1);
	int se = fl_textfield(ui, ID_SEARCH, sf, p->needle, sizeof p->needle, "Find", ui->f_reg);
	if (se & FL_EDIT_ENTER)
		find(p, (fl_app_mods(p->app) & FL_MOD_SHIFT) ? -1 : 1);
	if ((se & FL_EDIT_CHANGED) && !p->needle[0] && p->active_needle[0]) {
		p->active_needle[0] = 0;
		clear_hits(p);
	}
	/* zoom */
	int pct = (int)(p->zoom * 100 + 0.5);
	char zs[16];
	snprintf(zs, sizeof zs, "%d%%", pct);
	static const struct { uint32_t id; const char *l; int w; } zb[] = {
		{ID_FITPAGE, "Page", 50}, {ID_WIDTH, "Width", 56}, {ID_ZIN, "+", 30}, {ID_ZOUT, "-", 30},
	};
	int zused = 0;
	fl_rect zr = t;
	if (t.w > 330) {
		fl_rect zl = fl_cut_right(&zr, 52);
		fl_text_in(cv, p->small, zl, FL_CENTER, 0, fl_th.dim, zs);
		for (int i = 0; i < 4; i++) {
			if (zr.w < zb[i].w + 8)
				break;
			fl_rect b = fl_cut_right(&zr, zb[i].w);
			zr.w -= 2;
			int hit = fl_button(ui, zb[i].id, b, zb[i].l, FL_BTN_PLAIN);
			if (hit) {
				switch (zb[i].id) {
				case ID_FITPAGE: p->fit = FIT_PAGE; break;
				case ID_WIDTH: p->fit = FIT_WIDTH; break;
				case ID_ZIN:
					p->fit = FIT_NONE;
					set_zoom(p, p->zoom * 1.2, canvas.w / 2.0, canvas.h / 2.0);
					break;
				case ID_ZOUT:
					p->fit = FIT_NONE;
					set_zoom(p, p->zoom / 1.2, canvas.w / 2.0, canvas.h / 2.0);
					break;
				}
			}
			if ((zb[i].id == ID_WIDTH && p->fit == FIT_WIDTH) || (zb[i].id == ID_FITPAGE && p->fit == FIT_PAGE))
				fl_fill_rect(cv, fl_r(b.x + 8, b.y + b.h - 3, b.w - 16, 2), fl_th.accent);
			zused++;
		}
	}
	(void)zused;
	/* the file name in what is left */
	if (zr.w > 80) {
		const char *base = strrchr(p->path, '/');
		base = base ? base + 1 : p->path;
		char fit[PATH_MAX];
		fl_text_fit(ui->f_med, base, zr.w - 12, fit, sizeof fit);
		fl_text_in(cv, ui->f_med, fl_r(t.x + 4, t.y, zr.w, t.h), FL_LEFT, 0, fl_th.strong, fit);
	}

	if (p->msg[0]) {
		int mw = fl_text_width(ui->f_reg, p->msg, -1) + 32;
		if (mw > canvas.w - 40)
			mw = canvas.w - 40;
		fl_rect m = fl_r(canvas.x + (canvas.w - mw) / 2, canvas.y + canvas.h - 52, mw, 34);
		fl_fill_rect(cv, m, fl_th.raised);
		fl_stroke_rect(cv, m, 1, p->msg_err ? fl_th.err : fl_th.line);
		fl_text_in(cv, ui->f_reg, m, FL_CENTER, 0, p->msg_err ? fl_th.err : fl_th.text, p->msg);
	}
}

/* ---- main ------------------------------------------------------------------------------- */

static void usage(void)
{
	fprintf(stderr, "usage: flpdf [--page N] [--zoom PERCENT] [--find TEXT] [--no-contents] FILE\n"
	                "View PDF, XPS, EPUB and CBZ files. Ctrl+F finds, Ctrl+wheel zooms, Q quits.\n");
}

int main(int argc, char **argv)
{
	struct P *p = calloc(1, sizeof *p);
	if (!p)
		return 1;
	const char *file = NULL, *find_text = NULL;
	int start_page = 0, zoom_pct = 0, want_toc = 1;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage();
			return 0;
		} else if (!strcmp(argv[i], "--page") && i + 1 < argc) {
			start_page = atoi(argv[++i]) - 1;
		} else if (!strcmp(argv[i], "--zoom") && i + 1 < argc) {
			zoom_pct = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--find") && i + 1 < argc) {
			find_text = argv[++i];
		} else if (!strcmp(argv[i], "--no-contents")) {
			want_toc = 0;
		} else if (argv[i][0] == '-' && argv[i][1]) {
			usage();
			return 2;
		} else if (!file) {
			file = argv[i];
		} else {
			usage();
			return 2;
		}
	}
	if (!file) {
		usage();
		return 2;
	}
	p->app = fl_app_new(NULL, NULL);
	if (!p->app) {
		fprintf(stderr, "flpdf: cannot connect to a Wayland compositor\n");
		return 1;
	}
	p->small = fl_font_ui(400, 12);
	p->msg_timer = fl_timer_new(p->app, msg_clear, p);
	p->size_timer = fl_timer_new(p->app, size_tick, p);
	p->hit_page = -1;
	int rc = open_doc(p, file);
	if (rc < 0)
		fprintf(stderr, "flpdf: %s\n", p->err);
	p->show_toc = want_toc && p->ntoc > 0;
	p->fit = FIT_WIDTH;
	int ww = 980, wh = 700;
	if (rc == 0) {
		/* a window that fits the first page, but not bigger than the screen */
		int ow, oh;
		fl_app_output_size(p->app, &ow, &oh);
		wh = oh - 120 < 700 ? oh - 120 : 700;
		if (wh < 360)
			wh = 360;
		ww = ow - 100 < 1000 ? ow - 100 : 1000;
		if (ww < 560)
			ww = 560;
	}
	fl_win_opts o = {
		.title = "Documents", .app_id = "flpdf", .w = ww, .h = wh, .min_w = 520, .min_h = 320,
		.decorated = 1, .resizable = 1, .draw = draw, .ud = p,
	};
	p->win = fl_win_new(p->app, &o);
	if (!p->win)
		return 1;
	if (rc == 0) {
		const char *base = strrchr(file, '/');
		fl_win_set_title(p->win, base ? base + 1 : file);
		/* the first frame needs a layout: fit-to-width runs in draw, which also lays out */
		p->cw = ww - (p->show_toc ? TOC_W : 0);
		p->ch = wh - TOOL_H - 30;
		p->zoom = fit_zoom_for(p, 0, FIT_WIDTH);
		relayout(p);
		if (zoom_pct) {
			p->fit = FIT_NONE;
			p->zoom = zoom_pct / 100.0;
			relayout(p);
		}
		if (start_page > 0 && start_page < p->npages)
			goto_page(p, start_page, 0);
		if (find_text) {
			snprintf(p->needle, sizeof p->needle, "%s", find_text);
			find(p, 1);
		}
		if (p->npages > 1)
			fl_timer_arm(p->size_timer, 1, 1);
	}
	fl_app_watch_conf(p->app, NULL, NULL);
	fl_app_run(p->app);
	fl_app_free(p->app);
	return 0;
}
