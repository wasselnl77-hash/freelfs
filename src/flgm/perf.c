/* perf.c -- the Performance tab: resource list on the left, graph and numbers on the right
 * SPDX-License-Identifier: MIT
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "gmui.h"

#define LIST_W 244
#define ENTRY_H 74
#define PAD 24
#define ID_PERF 2000
#define ID_CORES 2100

/* ---- graphs ------------------------------------------------------------------------------- */

static fl_color tint(fl_color c, int t)
{
	return fl_mix(fl_th.bg, c, t);
}

static fl_color series2(void)
{
	return fl_mix(fl_th.accent, fl_th.text, 120);
}

/* One series, newest at the right edge. */
static void draw_series(fl_canvas *cv, fl_rect r, const struct hist *h, float max, fl_color col, float lw, int fill)
{
	if (!cv->px || h->n < 2 || r.w < 4 || r.h < 4)
		return;
	if (max <= 0)
		max = 1;
	fl_rect save = cv->clip;
	fl_clip(cv, r);
	fl_path *line = fl_path_new(), *area = fill ? fl_path_new() : NULL;
	if (!line) {
		cv->clip = save;
		return;
	}
	float bot = (float)(r.y + r.h - 1), span = (float)(r.h - 2);
	int first = HIST - h->n;
	for (int i = first; i < HIST; i++) {
		float x = (float)r.x + (float)(r.w - 1) * (float)i / (float)(HIST - 1);
		float v = h->v[i] / max;
		v = v < 0 ? 0 : v > 1 ? 1 : v;
		float y = bot - v * span;
		if (i == first) {
			fl_path_move(line, x, y);
			if (area) {
				fl_path_move(area, x, bot + 1);
				fl_path_line(area, x, y);
			}
		} else {
			fl_path_line(line, x, y);
			if (area)
				fl_path_line(area, x, y);
		}
	}
	if (area) {
		fl_path_line(area, (float)(r.x + r.w - 1), bot + 1);
		fl_path_close(area);
		fl_path_fill(cv, area, gm_alpha(col, 58));
		fl_path_free(area);
	}
	fl_path_stroke(cv, line, lw, col);
	fl_path_free(line);
	cv->clip = save;
}

static void graph_frame(fl_ui *ui, fl_rect r, int grid)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, r, tint(fl_th.accent, 14));
	if (grid && r.h > 40) {
		fl_color gl = gm_alpha(fl_th.accent, 36);
		int cell = r.h / 4;
		if (cell < 24)
			cell = 24;
		for (int x = r.x + r.w - cell; x > r.x; x -= cell)
			fl_fill_rect(cv, fl_r(x, r.y + 1, 1, r.h - 2), gl);
		for (int i = 1; i < 4; i++)
			fl_fill_rect(cv, fl_r(r.x + 1, r.y + r.h * i / 4, r.w - 2, 1), gl);
	}
	fl_stroke_rect(cv, r, 1, tint(fl_th.accent, 110));
}

/* A graph with a caption row above and "60 seconds" below. h is the height of the graph itself.
 * Returns the y below the block. */
static int graph_block(fl_ui *ui, struct gm *g, int x, int y, int w, int h, const char *cap_l, const char *cap_r,
                       const struct hist *a, const struct hist *b, float max, int legend)
{
	fl_canvas *cv = &ui->cv;
	gm_text(ui, g->f.sm, fl_r(x, y, w / 2, 18), FL_LEFT, fl_th.dim, cap_l);
	if (cap_r)
		gm_text(ui, g->f.sm, fl_r(x + w / 2, y, w / 2, 18), FL_RIGHT, fl_th.dim, cap_r);
	if (legend) { /* what the two lines are: 1 read / write, 2 receive / send */
		const char *n1 = legend == 1 ? "Read" : "Receive", *n2 = legend == 1 ? "Write" : "Send";
		int cw = cap_r ? fl_text_width(g->f.sm, cap_r, -1) + 24 : 0;
		int w2 = fl_text_width(g->f.sm, n2, -1), w1 = fl_text_width(g->f.sm, n1, -1);
		int rx = x + w - cw - w2 - 14;
		fl_fill_rect(cv, fl_r(rx, y + 5, 8, 8), series2());
		gm_text(ui, g->f.sm, fl_r(rx + 14, y, w2 + 4, 18), FL_LEFT, fl_th.dim, n2);
		int lx = rx - 24 - w1 - 14;
		fl_fill_rect(cv, fl_r(lx, y + 5, 8, 8), fl_th.accent);
		gm_text(ui, g->f.sm, fl_r(lx + 14, y, w1 + 4, 18), FL_LEFT, fl_th.dim, n1);
	}
	fl_rect gr = fl_r(x, y + 22, w, h);
	graph_frame(ui, gr, 1);
	if (b)
		draw_series(cv, gr, b, max, series2(), 1.5f, 1);
	if (a)
		draw_series(cv, gr, a, max, fl_th.accent, 1.7f, 1);
	gm_text(ui, g->f.tiny, fl_r(x, y + 22 + h + 3, w / 2, 16), FL_LEFT, fl_th.dim, "60 seconds");
	gm_text(ui, g->f.tiny, fl_r(x + w / 2, y + 22 + h + 3, w / 2, 16), FL_RIGHT, fl_th.dim, "0");
	return y + 22 + h + 22;
}

/* ---- numbers ------------------------------------------------------------------------------ */

struct kv {
	const char *label;
	char value[72];
};

static int stats_height(int nbig, int nmid, int nkv, int stacked)
{
	int left = (nbig ? 54 : 0) + (nmid ? (nbig ? 62 : 48) : 0);
	int right = nkv * 23;
	return stacked ? left + (nkv ? right + 14 : 0) : (left > right ? left : right);
}

static int pane_bottom; /* where the last page ended, for scrolling a window that is too short */

static void stats_draw(fl_ui *ui, struct gm *g, int x, int y, int w, const struct kv *big, int nbig, const struct kv *mid,
                       int nmid, const struct kv *kv, int nkv, int stacked)
{
	pane_bottom = y + stats_height(nbig, nmid, nkv, stacked);
	int cy = y;
	for (int i = 0; i < nbig; i++) {
		gm_text(ui, g->f.sm, fl_r(x + i * 190, cy, 190, 16), FL_LEFT, fl_th.dim, big[i].label);
		gm_text(ui, g->f.big, fl_r(x + i * 190, cy + 16, 190, 36), FL_LEFT, fl_th.strong, big[i].value);
	}
	if (nbig)
		cy += 62;
	int ms = nmid <= 2 ? 190 : 150;
	for (int i = 0; i < nmid; i++) {
		gm_text(ui, g->f.sm, fl_r(x + i * ms, cy, ms, 16), FL_LEFT, fl_th.dim, mid[i].label);
		gm_text(ui, g->f.mid, fl_r(x + i * ms, cy + 16, ms, 26), FL_LEFT, fl_th.text, mid[i].value);
	}
	if (nmid)
		cy += 48;
	int kx = x + (nbig > 2 || nmid > 2 ? 3 * 150 + 40 : 2 * 190 + 40), ky = y;
	if (stacked) {
		kx = x;
		ky = cy + 14;
	}
	int lw = 148;
	for (int i = 0; i < nkv; i++) {
		gm_text(ui, g->f.sm, fl_r(kx, ky + i * 23, lw, 22), FL_LEFT, fl_th.dim, kv[i].label);
		char fit[96];
		fl_text_fit(ui->f_reg, kv[i].value, x + w - (kx + lw) - 4 > 40 ? x + w - (kx + lw) - 4 : 40, fit, sizeof fit);
		gm_text(ui, ui->f_reg, fl_r(kx + lw, ky + i * 23, x + w - kx - lw, 22), FL_LEFT, fl_th.text, fit);
	}
}

static void kvset(struct kv *k, const char *label, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void kvset(struct kv *k, const char *label, const char *fmt, ...)
{
	va_list ap;
	k->label = label;
	va_start(ap, fmt);
	vsnprintf(k->value, sizeof k->value, fmt, ap);
	va_end(ap);
}

/* ---- the entry list ----------------------------------------------------------------------- */

static int build_entries(struct gm *g, struct perf_entry *e, int max)
{
	const struct sys *s = &g->sys;
	int n = 0;
	struct perf_entry *p = &e[n++];
	memset(p, 0, sizeof *p);
	p->kind = PERF_CPU;
	snprintf(p->title, sizeof p->title, "CPU");
	snprintf(p->line1, sizeof p->line1, "%s  %s", fmt_pct(s->cpu_total), s->ghz > 0 ? fmt_ghz(s->ghz) : "");
	p->h = &s->h_cpu;
	p->max = 100;

	p = &e[n++];
	memset(p, 0, sizeof *p);
	p->kind = PERF_MEM;
	snprintf(p->title, sizeof p->title, "Memory");
	double used = (double)(s->mem_total - s->mem_avail) * 1024, tot = (double)s->mem_total * 1024;
	snprintf(p->line1, sizeof p->line1, "%s / %s", fmt_bytes(used), fmt_bytes(tot));
	snprintf(p->line2, sizeof p->line2, "%s", fmt_pct(tot > 0 ? 100.0 * used / tot : 0));
	p->h = &s->h_mem;
	p->max = 100;

	for (int i = 0; i < s->ndisk && n < max; i++) {
		p = &e[n++];
		memset(p, 0, sizeof *p);
		p->kind = PERF_DISK;
		p->index = i;
		snprintf(p->title, sizeof p->title, "Disk %d (%s)", i, s->disk[i].name);
		snprintf(p->line1, sizeof p->line1, "%s", s->disk[i].type);
		snprintf(p->line2, sizeof p->line2, "%s", fmt_pct(s->disk[i].active));
		p->h = &s->disk[i].h_active;
		p->max = 100;
	}
	for (int i = 0; i < s->nnet && n < max; i++) {
		p = &e[n++];
		memset(p, 0, sizeof *p);
		p->kind = PERF_NET;
		p->index = i;
		snprintf(p->title, sizeof p->title, "%s (%s)", s->net[i].label, s->net[i].name);
		snprintf(p->line1, sizeof p->line1, "S: %s", fmt_bits(s->net[i].tx_bps));
		snprintf(p->line2, sizeof p->line2, "R: %s", fmt_bits(s->net[i].rx_bps));
		p->h = &s->net[i].h_rx;
		p->h2 = &s->net[i].h_tx;
	}
	for (int i = 0; i < s->ngpu && n < max; i++) {
		p = &e[n++];
		memset(p, 0, sizeof *p);
		p->kind = PERF_GPU;
		p->index = i;
		snprintf(p->title, sizeof p->title, "GPU %d", i);
		snprintf(p->line1, sizeof p->line1, "%s", s->gpu[i].name);
		if (s->gpu[i].has_busy)
			snprintf(p->line2, sizeof p->line2, "%s", fmt_pct(s->gpu[i].busy));
		else
			snprintf(p->line2, sizeof p->line2, "usage not reported");
		p->h = &s->gpu[i].h_busy;
		p->max = 100;
	}
	return n;
}

static void spark(fl_ui *ui, fl_rect r, const struct perf_entry *e, int selected)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, r, tint(fl_th.accent, selected ? 24 : 14));
	float max = e->max;
	if (max <= 0) {
		max = hist_max(e->h);
		if (e->h2 && hist_max(e->h2) > max)
			max = hist_max(e->h2);
		max = (float)nice_ceil(max < 1e5f ? 1e5f : max);
	}
	if (e->h2)
		draw_series(cv, r, e->h2, max, series2(), 1.2f, 1);
	draw_series(cv, r, e->h, max, fl_th.accent, 1.3f, 1);
	fl_stroke_rect(cv, r, 1, tint(fl_th.accent, 110));
}

static void draw_list(fl_ui *ui, struct gm *g, fl_rect r, const struct perf_entry *e, int n)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, r, fl_th.panel);
	fl_fill_rect(cv, fl_r(r.x + r.w - 1, r.y, 1, r.h), fl_th.line);
	fl_rect save = cv->clip;
	fl_clip(cv, r);
	int content = n * ENTRY_H + 16, max = content > r.h ? content - r.h : 0;
	if (fl_in(r, ui->mx, ui->my) && ui->wheel) {
		g->perf_scroll += ui->wheel * 48;
		ui->wheel = 0;
	}
	if (g->perf_sel * ENTRY_H + 8 < g->perf_scroll)
		g->perf_scroll = g->perf_sel * ENTRY_H;
	if ((g->perf_sel + 1) * ENTRY_H + 8 > g->perf_scroll + r.h)
		g->perf_scroll = (g->perf_sel + 1) * ENTRY_H + 16 - r.h;
	g->perf_scroll = g->perf_scroll < 0 ? 0 : g->perf_scroll > max ? max : g->perf_scroll;
	for (int i = 0; i < n; i++) {
		fl_rect er = fl_r(r.x, r.y + 8 + i * ENTRY_H - g->perf_scroll, r.w - 1, ENTRY_H);
		int res = fl_widget(ui, ID_PERF + i, er, FL_FOCUSABLE | FL_NO_FOCUS_RING);
		if ((res & FL_CLICK) && g->perf_sel != i) {
			g->perf_sel = i;
			g->pane_scroll = 0;
		}
		int sel = i == g->perf_sel;
		if (sel)
			fl_fill_rect(cv, er, fl_th.raised);
		else if (res & FL_HOVER)
			fl_fill_rect(cv, er, gm_alpha(fl_th.raised, 120));
		if (sel)
			fl_fill_rect(cv, fl_r(er.x, er.y, 3, er.h), fl_th.accent);
		fl_rect sp = fl_r(er.x + 16, er.y + (er.h - 46) / 2, 66, 46);
		spark(ui, sp, &e[i], sel);
		int tx = sp.x + sp.w + 14, tw = er.x + er.w - tx - 10;
		char fit[96];
		fl_text_fit(ui->f_med, e[i].title, tw, fit, sizeof fit);
		gm_text(ui, ui->f_med, fl_r(tx, er.y + 11, tw, 18), FL_LEFT, sel ? fl_th.strong : fl_th.text, fit);
		fl_text_fit(g->f.sm, e[i].line1, tw, fit, sizeof fit);
		gm_text(ui, g->f.sm, fl_r(tx, er.y + 30, tw, 16), FL_LEFT, fl_th.dim, fit);
		if (e[i].line2[0]) {
			fl_text_fit(g->f.sm, e[i].line2, tw, fit, sizeof fit);
			gm_text(ui, g->f.sm, fl_r(tx, er.y + 46, tw, 16), FL_LEFT, fl_th.dim, fit);
		}
	}
	cv->clip = save;
	/* Up and Down move through the list */
	if (ui->key == XKB_KEY_Down && g->perf_sel + 1 < n) {
		g->perf_sel++;
		ui->key = 0;
	} else if (ui->key == XKB_KEY_Up && g->perf_sel > 0) {
		g->perf_sel--;
		ui->key = 0;
	}
}

/* ---- the right side ----------------------------------------------------------------------- */

static int header(fl_ui *ui, struct gm *g, fl_rect r, const char *title, const char *sub)
{
	int x = r.x, y = r.y;
	gm_text(ui, g->f.title, fl_r(x, y, r.w, 34), FL_LEFT, fl_th.strong, title);
	if (sub && sub[0]) {
		int tw = fl_text_width(g->f.title, title, -1) + 24;
		char fit[128];
		fl_text_fit(g->f.h2, sub, r.w - tw > 40 ? r.w - tw : 40, fit, sizeof fit);
		gm_text(ui, g->f.h2, fl_r(x + tw, y + 1, r.w - tw, 34), FL_RIGHT, fl_th.dim, fit);
	}
	return y + 44;
}

/* Overall / Logical processors switch, drawn at the right end of a caption row. */
static void core_switch(fl_ui *ui, struct gm *g, int x, int y, int w)
{
	fl_canvas *cv = &ui->cv;
	static const char *const lab[2] = {"Overall", "Logical processors"};
	int bw[2] = {80, 150}, cx = x + w - bw[0] - bw[1];
	for (int i = 0; i < 2; i++) {
		fl_rect b = fl_r(cx, y - 4, bw[i], 24);
		int res = fl_widget(ui, ID_CORES + i, b, 0);
		if (res & FL_CLICK)
			g->cores_view = i;
		int on = g->cores_view == i;
		if (on)
			fl_fill_rect(cv, b, fl_th.raised);
		else if (res & FL_HOVER)
			fl_fill_rect(cv, b, gm_alpha(fl_th.raised, 110));
		fl_stroke_rect(cv, b, 1, fl_th.line);
		gm_text(ui, g->f.sm, b, FL_CENTER, on ? fl_th.strong : fl_th.dim, lab[i]);
		if (on)
			fl_fill_rect(cv, fl_r(b.x, b.y + b.h - 2, b.w, 2), fl_th.accent);
		cx += bw[i];
	}
}

static void cores_grid(fl_ui *ui, struct gm *g, fl_rect r)
{
	const struct sys *s = &g->sys;
	int n = s->ncpu;
	int cols = n <= 2 ? n : n <= 4 ? 2 : n <= 9 ? 3 : n <= 16 ? 4 : n <= 30 ? 6 : 8;
	if (cols < 1)
		cols = 1;
	int rows = (n + cols - 1) / cols, gap = 8;
	int cw = (r.w - gap * (cols - 1)) / cols, ch = (r.h - gap * (rows - 1)) / rows;
	if (ch < 36)
		ch = 36;
	for (int i = 0; i < n; i++) {
		fl_rect c = fl_r(r.x + (i % cols) * (cw + gap), r.y + (i / cols) * (ch + gap), cw, ch);
		graph_frame(ui, c, 0);
		draw_series(&ui->cv, c, &s->h_core[i], 100, fl_th.accent, 1.4f, 1);
		char b[32];
		snprintf(b, sizeof b, "CPU %d", i);
		gm_text(ui, g->f.tiny, fl_r(c.x + 8, c.y + 4, c.w - 16, 16), FL_LEFT, fl_th.dim, b);
		gm_text(ui, g->f.tiny, fl_r(c.x + 8, c.y + 4, c.w - 16, 16), FL_RIGHT, fl_th.text, fmt_pct(s->core[i]));
	}
}

static int stacked_for(int w) { return w < 700; }

static void page_cpu(fl_ui *ui, struct gm *g, fl_rect r)
{
	const struct sys *s = &g->sys;
	int x = r.x, w = r.w, y = header(ui, g, r, "CPU", s->ci.model);
	int stacked = stacked_for(w);
	struct kv big[2], mid[3], kv[8];
	kvset(&big[0], "Utilization", "%s", fmt_pct(s->cpu_total));
	kvset(&big[1], "Speed", "%s", fmt_ghz(s->ghz));
	kvset(&mid[0], "Processes", "%d", s->nproc);
	kvset(&mid[1], "Threads", "%d", s->nthreads);
	kvset(&mid[2], "Up time", "%s", fmt_uptime(s->uptime));
	int nkv = 0;
	if (s->ci.max_ghz > 0)
		kvset(&kv[nkv++], "Maximum speed", "%s", fmt_ghz(s->ci.max_ghz));
	kvset(&kv[nkv++], "Sockets", "%d", s->ci.sockets);
	kvset(&kv[nkv++], "Cores", "%d", s->ci.cores);
	kvset(&kv[nkv++], "Logical processors", "%d", s->ci.logical);
	if (s->ci.l1[0])
		kvset(&kv[nkv++], "L1 cache", "%s", s->ci.l1);
	if (s->ci.l2[0])
		kvset(&kv[nkv++], "L2 cache", "%s", s->ci.l2);
	if (s->ci.l3[0])
		kvset(&kv[nkv++], "L3 cache", "%s", s->ci.l3);
	kvset(&kv[nkv++], "Load average", "%.2f  %.2f  %.2f", s->load[0], s->load[1], s->load[2]);
	int sh = stats_height(2, 3, nkv, stacked);
	int gh = r.y + r.h - y - 22 - 22 - 18 - sh - 8;
	if (gh < 120)
		gh = 120;
	gm_text(ui, g->f.sm, fl_r(x, y, w / 2, 18), FL_LEFT, fl_th.dim, "% Utilization over 60 seconds");
	if (g->cores_view) {
		core_switch(ui, g, x, y, w);
		cores_grid(ui, g, fl_r(x, y + 22, w, gh));
		y += 22 + gh + 22;
	} else {
		core_switch(ui, g, x, y, w);
		fl_rect gr = fl_r(x, y + 22, w, gh);
		graph_frame(ui, gr, 1);
		draw_series(&ui->cv, gr, &s->h_cpu, 100, fl_th.accent, 1.7f, 1);
		gm_text(ui, g->f.tiny, fl_r(x, y + 22 + gh + 3, w / 2, 16), FL_LEFT, fl_th.dim, "60 seconds");
		gm_text(ui, g->f.tiny, fl_r(x + w / 2, y + 22 + gh + 3, w / 2, 16), FL_RIGHT, fl_th.dim, "0");
		gm_text(ui, g->f.tiny, fl_r(x + 6, y + 26, 80, 16), FL_LEFT, fl_th.dim, "100%");
		y += 22 + gh + 22;
	}
	stats_draw(ui, g, x, y + 8, w, big, 2, mid, 3, kv, nkv, stacked);
}

static void page_mem(fl_ui *ui, struct gm *g, fl_rect r)
{
	const struct sys *s = &g->sys;
	fl_canvas *cv = &ui->cv;
	int x = r.x, w = r.w;
	double tot = (double)s->mem_total * 1024;
	int y = header(ui, g, r, "Memory", fmt_bytes(tot));
	int stacked = stacked_for(w);
	double used = (double)(s->mem_total - s->mem_avail) * 1024;
	double cached = (double)(s->mem_cached + s->mem_buffers) * 1024;
	double avail = (double)s->mem_avail * 1024;
	struct kv big[2], mid[2], kv[7];
	kvset(&big[0], "In use", "%s", fmt_bytes(used));
	kvset(&big[1], "Available", "%s", fmt_bytes(avail));
	kvset(&mid[0], "Cached", "%s", fmt_bytes(cached));
	kvset(&mid[1], "Swap in use", "%s of %s", fmt_bytes((double)(s->swap_total - s->swap_free) * 1024), fmt_bytes((double)s->swap_total * 1024));
	int nkv = 0;
	kvset(&kv[nkv++], "Committed", "%s of %s", fmt_bytes((double)s->commit * 1024), fmt_bytes((double)s->commit_limit * 1024));
	kvset(&kv[nkv++], "Free", "%s", fmt_bytes((double)s->mem_free * 1024));
	kvset(&kv[nkv++], "Buffers", "%s", fmt_bytes((double)s->mem_buffers * 1024));
	kvset(&kv[nkv++], "Shared", "%s", fmt_bytes((double)s->mem_shmem * 1024));
	kvset(&kv[nkv++], "Kernel slab", "%s", fmt_bytes((double)s->mem_slab * 1024));
	int sh = stats_height(2, 2, nkv, stacked);
	int comp_h = 22 + 26 + 26;
	int gh = r.y + r.h - y - 22 - 22 - comp_h - 18 - sh - 8;
	if (gh < 100)
		gh = 100;
	char cap[64];
	snprintf(cap, sizeof cap, "%s", fmt_bytes(tot));
	y = graph_block(ui, g, x, y, w, gh, "Memory usage", cap, &s->h_mem, NULL, 100, 0);

	/* composition: in use | cached | free */
	gm_text(ui, g->f.sm, fl_r(x, y, w, 18), FL_LEFT, fl_th.dim, "Memory composition");
	fl_rect bar = fl_r(x, y + 22, w, 22);
	fl_fill_rect(cv, bar, tint(fl_th.accent, 14));
	double inuse_w = tot > 0 ? (used / tot) * (bar.w - 2) : 0;
	double cache_w = tot > 0 ? (cached / tot) * (bar.w - 2) : 0;
	if (inuse_w + cache_w > bar.w - 2)
		cache_w = bar.w - 2 - inuse_w;
	fl_fill_rect(cv, fl_r(bar.x + 1, bar.y + 1, (int)inuse_w, bar.h - 2), fl_th.accent);
	fl_fill_rect(cv, fl_r(bar.x + 1 + (int)inuse_w, bar.y + 1, (int)cache_w, bar.h - 2), gm_alpha(fl_th.accent, 100));
	fl_stroke_rect(cv, bar, 1, tint(fl_th.accent, 110));
	int lx = x;
	const struct { const char *name; fl_color c; double v; } leg[3] = {
		{"In use", fl_th.accent, used}, {"Cached", gm_alpha(fl_th.accent, 100), cached},
		{"Free", tint(fl_th.accent, 30), (double)s->mem_free * 1024}};
	for (int i = 0; i < 3; i++) {
		fl_fill_rect(cv, fl_r(lx, y + 52, 10, 10), leg[i].c);
		fl_stroke_rect(cv, fl_r(lx, y + 52, 10, 10), 1, tint(fl_th.accent, 110));
		char t[64];
		snprintf(t, sizeof t, "%s  %s", leg[i].name, fmt_bytes(leg[i].v));
		int tw = fl_text_width(g->f.sm, t, -1);
		gm_text(ui, g->f.sm, fl_r(lx + 16, y + 48, tw + 4, 18), FL_LEFT, fl_th.dim, t);
		lx += tw + 44;
	}
	y += comp_h + 10;
	stats_draw(ui, g, x, y, w, big, 2, mid, 2, kv, nkv, stacked);
}

static void page_disk(fl_ui *ui, struct gm *g, fl_rect r, int di)
{
	const struct sys *s = &g->sys;
	if (di >= s->ndisk)
		return;
	const struct disk *d = &s->disk[di];
	int x = r.x, w = r.w;
	char title[48];
	snprintf(title, sizeof title, "Disk %d (%s)", di, d->name);
	int y = header(ui, g, r, title, d->model[0] ? d->model : d->type);
	int stacked = stacked_for(w);
	struct kv big[2], mid[2], kv[5];
	kvset(&big[0], "Active time", "%s", fmt_pct(d->active));
	kvset(&big[1], "Average response time", "%.1f ms", d->resp_ms);
	kvset(&mid[0], "Read speed", "%s", fmt_rate(d->rd_bps));
	kvset(&mid[1], "Write speed", "%s", fmt_rate(d->wr_bps));
	int nkv = 0;
	kvset(&kv[nkv++], "Capacity", "%s", d->bytes ? fmt_bytes((double)d->bytes) : "\xe2\x80\x94");
	kvset(&kv[nkv++], "Type", "%s", d->type);
	if (d->model[0])
		kvset(&kv[nkv++], "Model", "%s", d->model);
	kvset(&kv[nkv++], "Device", "/dev/%s", d->name);
	int sh = stats_height(2, 2, nkv, stacked);
	int avail = r.y + r.h - y - 2 * (22 + 22) - sh - 8;
	int g1 = avail * 55 / 100, g2 = avail - g1;
	if (g1 < 80)
		g1 = 80;
	if (g2 < 60)
		g2 = 60;
	y = graph_block(ui, g, x, y, w, g1, "Active time", "100%", &d->h_active, NULL, 100, 0);
	float m = hist_max(&d->h_rd) > hist_max(&d->h_wr) ? hist_max(&d->h_rd) : hist_max(&d->h_wr);
	float scale = (float)(nice_ceil(m < 1048576.f ? 1.0 : m / 1048576.0) * 1048576.0);
	y = graph_block(ui, g, x, y, w, g2, "Disk transfer rate (read and write)", fmt_rate(scale), &d->h_rd, &d->h_wr, scale, 1);
	stats_draw(ui, g, x, y + 4, w, big, 2, mid, 2, kv, nkv, stacked);
}

static void page_net(fl_ui *ui, struct gm *g, fl_rect r, int ni)
{
	const struct sys *s = &g->sys;
	if (ni >= s->nnet)
		return;
	const struct net *n = &s->net[ni];
	int x = r.x, w = r.w;
	int y = header(ui, g, r, n->label, n->name);
	int stacked = stacked_for(w);
	struct kv big[2], kv[6];
	kvset(&big[0], "Send", "%s", fmt_bits(n->tx_bps));
	kvset(&big[1], "Receive", "%s", fmt_bits(n->rx_bps));
	int nkv = 0;
	kvset(&kv[nkv++], "Adapter name", "%s", n->name);
	kvset(&kv[nkv++], "State", "%s", n->state[0] ? n->state : "unknown");
	kvset(&kv[nkv++], "IPv4 address", "%s", n->ip4[0] ? n->ip4 : "\xe2\x80\x94");
	if (n->speed_mbps > 0)
		kvset(&kv[nkv++], "Link speed", "%ld Mbps", n->speed_mbps);
	kvset(&kv[nkv++], "Sent since start", "%s", fmt_bytes((double)n->tx));
	kvset(&kv[nkv++], "Received since start", "%s", fmt_bytes((double)n->rx));
	int sh = stats_height(2, 0, nkv, stacked);
	int gh = r.y + r.h - y - 22 - 22 - sh - 8;
	if (gh < 120)
		gh = 120;
	float m = hist_max(&n->h_rx) > hist_max(&n->h_tx) ? hist_max(&n->h_rx) : hist_max(&n->h_tx);
	float scale = (float)nice_ceil(m < 1e5f ? 1e5f : m);
	y = graph_block(ui, g, x, y, w, gh, "Throughput (receive and send)", fmt_bits(scale), &n->h_rx, &n->h_tx, scale, 2);
	stats_draw(ui, g, x, y + 4, w, big, 2, NULL, 0, kv, nkv, stacked);
}

static void page_gpu(fl_ui *ui, struct gm *g, fl_rect r, int gi)
{
	const struct sys *s = &g->sys;
	if (gi >= s->ngpu)
		return;
	const struct gpu *p = &s->gpu[gi];
	char title[24];
	snprintf(title, sizeof title, "GPU %d", gi);
	int x = r.x, w = r.w;
	int y = header(ui, g, r, title, p->name);
	int stacked = stacked_for(w);
	struct kv big[2], mid[2], kv[8];
	const char *dash = "\xe2\x80\x94";
	kvset(&big[0], "Utilization", "%s", p->has_busy ? fmt_pct(p->busy) : dash);
	if (p->has_vram)
		kvset(&big[1], "Dedicated memory", "%s", fmt_bytes((double)p->vram_used));
	else
		kvset(&big[1], "Dedicated memory", "%s", dash);
	int nmid = 0, nkv = 0;
	if (p->gtt_total)
		kvset(&mid[nmid++], "Shared memory", "%s", fmt_bytes((double)p->gtt_used));
	if (p->temp_c >= 0)
		kvset(&mid[nmid++], "Temperature", "%d \xc2\xb0""C", p->temp_c);
	if (p->has_vram)
		kvset(&kv[nkv++], "Dedicated total", "%s", fmt_bytes((double)p->vram_total));
	if (p->gtt_total)
		kvset(&kv[nkv++], "Shared total", "%s", fmt_bytes((double)p->gtt_total));
	if (p->power_w >= 0)
		kvset(&kv[nkv++], "Power", "%.1f W", p->power_w);
	kvset(&kv[nkv++], "Driver", "%s", p->driver[0] ? p->driver : dash);
	kvset(&kv[nkv++], "Vendor", "%s", p->vendor[0] ? p->vendor : dash);
	kvset(&kv[nkv++], "PCI ID", "%s", p->pci);
	kvset(&kv[nkv++], "Device", "%s", p->path);
	int sh = stats_height(2, nmid, nkv, stacked);
	int blocks = p->has_vram ? 2 : 1;
	int avail = r.y + r.h - y - blocks * (22 + 22) - sh - 8;
	int g1 = blocks == 2 ? avail * 58 / 100 : avail, g2 = avail - g1;
	if (g1 < 90)
		g1 = 90;
	if (g2 < 60)
		g2 = 60;
	if (p->has_busy) {
		y = graph_block(ui, g, x, y, w, g1, "% Utilization over 60 seconds", "100%", &p->h_busy, NULL, 100, 0);
	} else {
		y = graph_block(ui, g, x, y, w, g1, "% Utilization over 60 seconds", "100%", NULL, NULL, 100, 0);
		gm_text(ui, ui->f_reg, fl_r(x, y - 22 - g1 - 22 + 22, w, g1), FL_CENTER, fl_th.dim,
		        "This driver does not report how busy the GPU is.");
	}
	if (p->has_vram) {
		y = graph_block(ui, g, x, y, w, g2, "Dedicated memory use", fmt_bytes((double)p->vram_total), &p->h_vram, NULL, 100, 0);
	}
	stats_draw(ui, g, x, y + 4, w, big, 2, mid, nmid, kv, nkv, stacked);
}

static void run_page(fl_ui *ui, struct gm *g, const struct perf_entry *e, fl_rect pg)
{
	switch (e->kind) {
	case PERF_CPU: page_cpu(ui, g, pg); break;
	case PERF_MEM: page_mem(ui, g, pg); break;
	case PERF_DISK: page_disk(ui, g, pg, e->index); break;
	case PERF_NET: page_net(ui, g, pg, e->index); break;
	case PERF_GPU: page_gpu(ui, g, pg, e->index); break;
	}
}

void perf_draw(fl_ui *ui, struct gm *g, fl_rect r)
{
	struct perf_entry ent[1 + 1 + MAXDISK + MAXNET + MAXGPU];
	int n = build_entries(g, ent, (int)(sizeof ent / sizeof *ent));
	if (g->perf_sel >= n)
		g->perf_sel = n - 1;
	if (g->perf_sel < 0)
		g->perf_sel = 0;
	/* --perf kind from the command line arrives as a kind; map it to the first such entry */
	static int mapped;
	if (!mapped) {
		mapped = 1;
		for (int i = 0; i < n; i++)
			if (ent[i].kind == g->perf_sel) {
				g->perf_sel = i;
				break;
			}
	}
	fl_rect list = fl_r(r.x, r.y, LIST_W, r.h);
	draw_list(ui, g, list, ent, n);
	fl_rect pane = fl_r(r.x + LIST_W + PAD, r.y + 18, r.w - LIST_W - 2 * PAD, r.h - 18);
	fl_rect area = fl_r(r.x + LIST_W, r.y, r.w - LIST_W, r.h);
	/* a window too short for the page scrolls it */
	int max = g->pane_need > pane.h ? g->pane_need - pane.h : 0;
	if (fl_in(area, ui->mx, ui->my) && ui->wheel && max) {
		g->pane_scroll += ui->wheel * 48;
		ui->wheel = 0;
	}
	g->pane_scroll = g->pane_scroll < 0 ? 0 : g->pane_scroll > max ? max : g->pane_scroll;
	fl_rect pg = pane;
	pg.y -= g->pane_scroll;
	const struct perf_entry *e = &ent[g->perf_sel];
	fl_rect save = ui->cv.clip;
	fl_clip(&ui->cv, area);
	if (!g->pane_need) { /* first frame: measure with a dry pass so the scrollbar is there from the start */
		uint32_t *px = ui->cv.px;
		ui->cv.px = NULL;
		run_page(ui, g, e, pg);
		ui->cv.px = px;
		g->pane_need = pane_bottom - pg.y + 12;
		max = g->pane_need > pane.h ? g->pane_need - pane.h : 0;
	}
	pane_bottom = 0;
	run_page(ui, g, e, pg);
	ui->cv.clip = save;
	if (pane_bottom)
		g->pane_need = pane_bottom - pg.y + 12;
	if (max) {
		fl_rect sb = fl_r(area.x + area.w - 8, area.y + 2, 6, area.h - 4);
		fl_scrollbar(ui, ID_PERF + 90, sb, &g->pane_scroll, g->pane_need + 18, pane.h + 18);
	}
}
