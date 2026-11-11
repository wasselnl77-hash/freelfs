/* views.c -- drawing the Software Store
 * SPDX-License-Identifier: MIT
 *
 * Immediate mode: draw() paints the window and reports clicks as queued actions (flstore.c runs them after
 * the pass). Pages that scroll are painted at an offset inside a clip; the height seen in the last pass
 * sizes the scroll bar.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "flstore.h"

enum { TOP_H = 52, SIDE_W = 214, PAD = 28, CARD_H = 100, ROW_H = 64, BAR_H = 66, BAR_LOG_H = 200, HDR_H = 150,
       GAP = 12, SB_W = 8 };

/* ---- text ------------------------------------------------------------------------------------ */

/* Text in w px at x, y: paragraphs split at newlines, a line starting "- " is a list item. At most
 * max_lines lines (the last one ends in "..." if there was more). Returns the height used. */
static int text_block(fl_ui *ui, fl_font *f, int x, int y, int w, fl_color col, const char *s, int max_lines)
{
	fl_canvas *cv = &ui->cv;
	int lh = fl_font_height(f) + 4, y0 = y, lines = 0;
	const char *p = s;
	while (*p && lines < max_lines) {
		const char *e = strchr(p, '\n');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		if (len == 0) {
			y += lh / 2;
			p++;
			continue;
		}
		const char *t = p;
		size_t tl = len;
		int ix = 0;
		if (tl > 2 && t[0] == '-' && t[1] == ' ') {
			t += 2;
			tl -= 2;
			ix = 16;
			fl_text(cv, f, x + 3, y, col, "\xe2\x80\xa2", -1);
		}
		size_t pos = 0;
		while (pos < tl && lines < max_lines) {
			while (pos < tl && t[pos] == ' ')
				pos++;
			if (pos >= tl)
				break;
			size_t best = pos, end = pos;
			while (end < tl) {
				size_t wd = end;
				while (wd < tl && t[wd] != ' ')
					wd++;
				if (best > pos && fl_text_width(f, t + pos, (int)(wd - pos)) > w - ix)
					break;
				best = wd;
				end = wd;
				while (end < tl && t[end] == ' ')
					end++;
			}
			if (best == pos)
				best = tl;
			if (lines + 1 == max_lines && best < tl) {
				char tmp[512], fit[512];
				snprintf(tmp, sizeof tmp, "%.*s", (int)(tl - pos), t + pos);
				fl_text_fit(f, tmp, w - ix, fit, sizeof fit);
				fl_text(cv, f, x + ix, y, col, fit, -1);
			} else {
				fl_text(cv, f, x + ix, y, col, t + pos, (int)(best - pos));
			}
			y += lh;
			lines++;
			pos = best;
		}
		if (!e)
			break;
		p = e + 1;
	}
	return y - y0;
}

static void text_fit(fl_ui *ui, fl_font *f, int x, int y, int w, fl_color col, const char *s)
{
	char fit[400];
	if (w < 8)
		return;
	fl_text_fit(f, s, w, fit, sizeof fit);
	fl_text(&ui->cv, f, x, y, col, fit, -1);
}

/* ---- icons and tiles ----------------------------------------------------------------------------- */

static void draw_icon(fl_ui *ui, struct St *s, const struct rview *v, int x, int y, int px)
{
	const simg *im = icon_get(s, v, px);
	if (im) {
		simg_draw(&ui->cv, im, x + (px - im->w) / 2, y + (px - im->h) / 2);
		return;
	}
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, fl_r(x, y, px, px), fl_th.raised);
	char ch[2] = {0, 0};
	const char *n = v->name;
	ch[0] = n[0] >= 'a' && n[0] <= 'z' ? (char)(n[0] - 32) : n[0] ? n[0] : '?';
	fl_font *f = px >= 80 ? s->title : px >= 48 ? s->big : ui->f_semi;
	int tw = fl_text_width(f, ch, 1), th = fl_font_height(f);
	fl_text(cv, f, x + (px - tw) / 2, y + (px - th) / 2, v->src == SRC_PR || v->src == SRC_PI ? fl_th.accent : fl_th.dim,
	        ch, 1);
}

static const char *source_name(int src)
{
	return src == SRC_PR || src == SRC_PI ? "FreeLFS package" : "Flathub";
}

/* ---- cards ----------------------------------------------------------------------------------------- */

static void draw_card(fl_ui *ui, struct St *s, fl_rect r, struct ref ref, int id)
{
	fl_canvas *cv = &ui->cv;
	struct rview v;
	rview(s, ref, &v);
	int res = fl_widget(ui, (uint32_t)id, r, 0);
	fl_fill_rect(cv, r, (res & FL_HOVER) ? fl_th.raised : fl_th.panel);
	fl_stroke_rect(cv, r, 1, (res & FL_HOVER) ? fl_th.dim : fl_th.line);
	draw_icon(ui, s, &v, r.x + 14, r.y + (r.h - 64) / 2, 64);
	int tx = r.x + 14 + 64 + 14, tw = r.x + r.w - 14 - tx;
	int tag_w = 0;
	if (v.state != ST_AVAIL) {
		const char *t = state_label(v.state);
		tag_w = fl_text_width(s->small, t, -1) + 8;
		fl_text(cv, s->small, r.x + r.w - 14 - tag_w + 8, r.y + 13, fl_th.accent, t, -1);
	}
	fl_rect save = cv->clip;
	fl_clip(cv, fl_r(r.x + 1, r.y + 1, r.w - 2, r.h - 2));
	text_fit(ui, ui->f_semi, tx, r.y + 12, tw - tag_w, fl_th.strong, v.name);
	if (v.sum[0])
		text_block(ui, s->small, tx, r.y + 34, tw, fl_th.dim, v.sum, 2);
	char foot[200];
	if (v.src == SRC_PR || v.src == SRC_PI)
		snprintf(foot, sizeof foot, "FreeLFS package  %s", v.ver);
	else
		snprintf(foot, sizeof foot, "%s", v.app && v.app->dev[0] ? v.app->dev : source_name(v.src));
	text_fit(ui, s->tiny, tx, r.y + r.h - 24, tw, fl_th.dim, foot);
	cv->clip = save;
	if (res & FL_CLICK)
		queue(s, A_OPEN_REF, ref.src * 100000 + ref.idx);
}

static int cols_for(int w)
{
	int cols = (w + GAP) / (300 + GAP);
	return cols < 1 ? 1 : cols > 4 ? 4 : cols;
}

/* A grid of cards from y. Cards outside the viewport are not drawn (no icon is fetched for them).
 * Returns the height. */
static int draw_grid(fl_ui *ui, struct St *s, fl_rect view, int x, int y, int w, const struct ref *refs, int n,
                     int idbase)
{
	int cols = cols_for(w), cw = (w - (cols - 1) * GAP) / cols;
	int rows = (n + cols - 1) / cols;
	for (int i = 0; i < n; i++) {
		fl_rect r = fl_r(x + (i % cols) * (cw + GAP), y + (i / cols) * (CARD_H + GAP), cw, CARD_H);
		if (r.y + r.h < view.y || r.y > view.y + view.h)
			continue;
		draw_card(ui, s, r, refs[i], idbase + i);
	}
	return rows ? rows * CARD_H + (rows - 1) * GAP : 0;
}

/* ---- scrolling -------------------------------------------------------------------------------------- */

struct sc {
	fl_rect body, save;
	int x, y, w;
};

static void scroll_begin(fl_ui *ui, struct St *s, fl_rect body, struct sc *c)
{
	int maxs = s->content_h - body.h;
	if (maxs < 0)
		maxs = 0;
	if (ui->mouse_in && fl_in(body, ui->mx, ui->my) && ui->wheel) {
		s->scroll += ui->wheel * 64;
		ui->wheel = 0;
	}
	if (ui->focus != ID_SEARCH && ui->key) {
		int page = body.h - 60;
		switch (ui->key) {
		case XKB_KEY_Page_Down: s->scroll += page; ui->key = 0; break;
		case XKB_KEY_Page_Up: s->scroll -= page; ui->key = 0; break;
		case XKB_KEY_Down: s->scroll += 48; ui->key = 0; break;
		case XKB_KEY_Up: s->scroll -= 48; ui->key = 0; break;
		case XKB_KEY_Home: s->scroll = 0; ui->key = 0; break;
		case XKB_KEY_End: s->scroll = maxs; ui->key = 0; break;
		}
	}
	if (s->scroll > maxs)
		s->scroll = maxs;
	if (s->scroll < 0)
		s->scroll = 0;
	fl_fill_rect(&ui->cv, body, fl_th.bg);
	c->body = body;
	c->save = ui->cv.clip;
	fl_clip(&ui->cv, body);
	c->x = body.x + PAD;
	c->w = body.w - 2 * PAD;
	c->y = body.y - s->scroll;
}

static void scroll_end(fl_ui *ui, struct St *s, struct sc *c, int y_end)
{
	ui->cv.clip = c->save;
	s->content_h = y_end - (c->body.y - s->scroll) + PAD;
	fl_scrollbar(ui, ID_SCROLL, fl_r(c->body.x + c->body.w - SB_W, c->body.y, SB_W, c->body.h), &s->scroll,
	             s->content_h, c->body.h);
}

/* ---- notices ---------------------------------------------------------------------------------------- */

static int notice(fl_ui *ui, struct St *s, int x, int y, int w, const char *head, const char *body)
{
	if (head && head[0])
		y += text_block(ui, ui->f_semi, x, y, w, fl_th.strong, head, 3) + 6;
	if (body && body[0])
		y += text_block(ui, s->small, x, y, w, fl_th.dim, body, 8);
	return y;
}

static void bar_indeterminate(fl_ui *ui, fl_rect bar)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	long ms = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
	int seg = bar.w / 4 > 40 ? bar.w / 4 : 40;
	int pos = (int)((ms / 4) % (bar.w + seg)) - seg;
	fl_fill_rect(&ui->cv, fl_intersect(fl_r(bar.x + pos, bar.y, seg, bar.h), bar), fl_th.accent);
}

/* ---- Explore pages ------------------------------------------------------------------------------------ */

static void catalog_notice(fl_ui *ui, struct St *s, fl_rect body)
{
	fl_fill_rect(&ui->cv, body, fl_th.bg);
	int x = body.x + PAD, w = body.w - 2 * PAD, y = body.y + PAD;
	if (s->idx_state == IDX_ERR) {
		y = notice(ui, s, x, y, w, "The Flathub catalog could not be loaded.", s->idx_err);
		y += 8;
		y += text_block(ui, s->small, x, y, w, fl_th.dim,
		                "Check the network connection. Installed apps and FreeLFS packages still work.", 3);
		if (fl_button(ui, ID_RETRY, fl_r(x, y + 14, 110, 32), "Try again", FL_BTN_ACCENT))
			queue(s, A_RETRY, 0);
		return;
	}
	const char *head = s->idx_state == IDX_FETCH ? "Downloading the Flathub catalog..." : "Preparing the catalog...";
	const char *sub = s->idx_state == IDX_FETCH ? "The first time this takes a minute (about 11 MB); after that it is kept up to date in the background." :
	                                              "Reading the app list. This happens once after each update of the catalog.";
	y = notice(ui, s, x, y, w, head, sub);
	bar_indeterminate(ui, fl_r(x, y + 16, w > 420 ? 420 : w, 4));
}

static void heading(fl_ui *ui, struct St *s, int x, int y, const char *t)
{
	fl_text(&ui->cv, s->big, x, y, fl_th.strong, t, -1);
}

static void draw_home(fl_ui *ui, struct St *s, fl_rect body)
{
	if (s->idx_state != IDX_READY) {
		catalog_notice(ui, s, body);
		return;
	}
	struct sc c;
	scroll_begin(ui, s, body, &c);
	int y = c.y + PAD - 6;
	heading(ui, s, c.x, y, "Featured");
	y += fl_font_height(s->big) + 14;
	y += draw_grid(ui, s, body, c.x, y, c.w - SB_W - 4, s->feat, s->nfeat, ID_CARD);
	y += 34;
	if (s->nrecent) {
		heading(ui, s, c.x, y, "Recently updated");
		y += fl_font_height(s->big) + 14;
		y += draw_grid(ui, s, body, c.x, y, c.w - SB_W - 4, s->recent, s->nrecent, ID_CARD + NFEAT);
	}
	scroll_end(ui, s, &c, y);
}

static void pager(fl_ui *ui, struct St *s, int x, int y, int w, int pages)
{
	if (pages <= 1)
		return;
	char t[48];
	snprintf(t, sizeof t, "Page %d of %d", s->cur.pg + 1, pages);
	int tw = fl_text_width(ui->f_reg, t, -1);
	int mid = x + w / 2;
	if (s->cur.pg > 0 && fl_button(ui, ID_PG_PREV, fl_r(mid - tw / 2 - 16 - 104, y, 104, 32), "Previous", FL_BTN_OUTLINE))
		queue(s, A_PAGE, -1);
	fl_text(&ui->cv, ui->f_reg, mid - tw / 2, y + 7, fl_th.dim, t, -1);
	if (s->cur.pg < pages - 1 && fl_button(ui, ID_PG_NEXT, fl_r(mid + tw / 2 + 16, y, 104, 32), "Next", FL_BTN_OUTLINE))
		queue(s, A_PAGE, 1);
}

/* A category, the search results or the FreeLFS packages: a title and pages of cards. */
static void draw_list_page(fl_ui *ui, struct St *s, fl_rect body)
{
	int page = s->cur.page;
	if (page != PG_PKGS && s->idx_state != IDX_READY) {
		catalog_notice(ui, s, body);
		return;
	}
	struct sc c;
	scroll_begin(ui, s, body, &c);
	int y = c.y + PAD - 6, w = c.w - SB_W - 4;
	char title[160], sub[200] = "";
	int nfh = 0, npk = 0;
	for (int i = 0; i < s->nlist; i++)
		if (s->list[i].src == SRC_FH)
			nfh++;
		else
			npk++;
	if (page == PG_CAT) {
		snprintf(title, sizeof title, "%s", cat_names[s->cur.cat]);
		snprintf(sub, sizeof sub, "%d apps from Flathub. Verified apps and apps with screenshots come first.", nfh);
	} else if (page == PG_PKGS) {
		snprintf(title, sizeof title, "FreeLFS packages");
		snprintf(sub, sizeof sub, "%d packages in the FreeLFS repository%s", npk,
		         s->npkupd ? ", some can be updated" : "");
	} else {
		snprintf(title, sizeof title, "Results for \"%s\"", s->cur.query);
		if (npk)
			snprintf(sub, sizeof sub, "%d apps on Flathub, %d FreeLFS packages", nfh, npk);
		else
			snprintf(sub, sizeof sub, "%d apps on Flathub", nfh);
	}
	fl_text(&ui->cv, s->title, c.x, y, fl_th.strong, title, -1);
	y += fl_font_height(s->title) + 6;
	fl_text(&ui->cv, s->small, c.x, y, fl_th.dim, sub, -1);
	y += fl_font_height(s->small) + 22;
	if (s->nlist == 0) {
		if (page == PG_PKGS && !s->repo_cfg)
			notice(ui, s, c.x, y, w, "No package repository is configured.",
			       "FreeLFS has nowhere to get its own packages from yet. Add a line \"repo NAME URL\" to "
			       "/etc/flpkg.conf and sync the index (flpkg sync, as root). Apps from Flathub work without it.");
		else if (page == PG_PKGS && !s->pkrepo_loaded)
			notice(ui, s, c.x, y, w, "Loading...", NULL);
		else if (page == PG_PKGS)
			notice(ui, s, c.x, y, w, "The repository has no packages.", "Sync the index with flpkg sync (as root).");
		else
			notice(ui, s, c.x, y, w, page == PG_SEARCH ? "Nothing found." : "No apps in this category.",
			       page == PG_SEARCH ? "Try fewer or different words." : NULL);
		scroll_end(ui, s, &c, y + 80);
		return;
	}
	int pages = (s->nlist + PAGE_SIZE - 1) / PAGE_SIZE;
	int first = s->cur.pg * PAGE_SIZE, n = s->nlist - first;
	if (n > PAGE_SIZE)
		n = PAGE_SIZE;
	y += draw_grid(ui, s, body, c.x, y, w, s->list + first, n, ID_CARD);
	y += 28;
	pager(ui, s, c.x, y, w, pages);
	scroll_end(ui, s, &c, y + (pages > 1 ? 32 : 0));
}

/* ---- the app page ---------------------------------------------------------------------------------------- */

static void pretty_license(const char *in, char *out, size_t cap)
{
	snprintf(out, cap, "%s", in);
	char *eq = strchr(out, '=');
	if (eq)
		*eq = 0;
	if (!strncmp(out, "LicenseRef-proprietary", 22))
		snprintf(out, cap, "Proprietary");
	else if (!strncmp(out, "LicenseRef-", 11))
		memmove(out, out + 11, strlen(out + 11) + 1);
}

static void info_row(fl_ui *ui, struct St *s, int x, int *y, int w, const char *key, const char *val)
{
	if (!val || !val[0])
		return;
	fl_text_ex(&ui->cv, s->tiny, x, *y + 3, fl_th.dim, key, -1, 0);
	int h = text_block(ui, ui->f_reg, x + 150, *y, w - 150, fl_th.text, val, 4);
	*y += (h > 20 ? h : 20) + 8;
}

/* 0..1000 for what the running change did, or -1 when unknown. */
static int progress_of(struct St *s)
{
	struct job *j = &s->job[J_CHANGE];
	if (!s->running || !j->buf[0])
		return -1;
	int pct = pct_of(j->buf[0], j->n[0]);
	int st = 0, tot = 0;
	if (steps_of(j->buf[0], j->n[0], &st, &tot)) {
		int base = 1000 * (st - 1) / tot;
		return pct >= 0 ? base + 1000 * pct / 100 / tot : base + 1000 / tot / 4;
	}
	return pct >= 0 ? pct * 10 : -1;
}

static void fmt_bytes(double b, char *out, size_t cap)
{
	if (b >= 1024.0 * 1024.0 * 1024.0)
		snprintf(out, cap, "%.1f GB", b / (1024.0 * 1024.0 * 1024.0));
	else if (b >= 1024.0 * 1024.0)
		snprintf(out, cap, "%.0f MB", b / (1024.0 * 1024.0));
	else
		snprintf(out, cap, "%.0f kB", b / 1024.0);
}

/* seconds since the running change started */
static long mono_secs_running(const struct St *s)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec - s->net_t0_ms / 1000;
}

static void draw_app_header(fl_ui *ui, struct St *s, fl_rect hdr)
{
	fl_canvas *cv = &ui->cv;
	struct rview v;
	rview(s, s->cur.app, &v);
	fl_fill_rect(cv, hdr, fl_th.bg);
	fl_fill_rect(cv, fl_r(hdr.x, hdr.y + hdr.h - 1, hdr.w, 1), fl_th.line);
	int x = hdr.x + PAD, y = hdr.y + 22;
	draw_icon(ui, s, &v, x, y, 96);
	int tx = x + 96 + 22, tw = hdr.w - PAD - (tx - hdr.x) - 10;
	fl_rect save = cv->clip;
	fl_clip(cv, fl_intersect(save, hdr));
	text_fit(ui, s->title, tx, y - 4, tw, fl_th.strong, v.name);
	char meta[300];
	const char *ver = v.ver[0] ? v.ver : kv_get(s, "Version", NULL) ? kv_get(s, "Version", NULL) : "";
	const char *who = v.app && v.app->dev[0] ? v.app->dev : v.src == SRC_PR || v.src == SRC_PI ? "FreeLFS" : "";
	snprintf(meta, sizeof meta, "%s%s%s%s%s", who, who[0] && ver[0] ? "   " : "", ver[0] ? "Version " : "", ver,
	         v.state != ST_AVAIL ? (who[0] || ver[0] ? "   " : "") : "");
	int mw = fl_text_width(s->small, meta, -1);
	fl_text(cv, s->small, tx, y + 32, fl_th.dim, meta, -1);
	if (v.state != ST_AVAIL)
		fl_text(cv, s->small, tx + mw, y + 32, fl_th.accent, state_label(v.state), -1);
	const char *sum = v.sum[0] ? v.sum : "";
	if (sum[0])
		text_fit(ui, ui->f_reg, tx, y + 54, tw, fl_th.text, sum);

	/* the buttons, or the progress of what is happening to this app */
	int by = y + 84, busy = ref_busy(s, s->cur.app);
	int flat = v.src == SRC_FH || v.src == SRC_FI;
	if (busy) {
		int fr = busy == 2 ? progress_of(s) : -1;
		char t[80];
		snprintf(t, sizeof t, busy == 2 ? (s->run.kind == CK_FREMOVE || s->run.kind == CK_PKREMOVE ? "Removing..." :
		                                   s->run.kind == CK_FUPDATE ? "Updating..." : "Installing...") :
		                                  "Waiting for the other changes...");
		fl_text_in(cv, ui->f_med, fl_r(tx, by, 190, 34), FL_LEFT, 0, fl_th.text, t);
		fl_rect bar = fl_r(tx + 200, by + 15, tw - 200 > 260 ? 260 : tw - 200, 4);
		fl_fill_rect(cv, bar, fl_th.line);
		if (busy == 2 && fr >= 0)
			fl_fill_rect(cv, fl_r(bar.x, bar.y, bar.w * fr / 1000, bar.h), fl_th.accent);
		else if (busy == 2)
			bar_indeterminate(ui, bar);
	} else if (flat && !s->fl_have) {
		fl_text_in(cv, ui->f_reg, fl_r(tx, by, tw, 34), FL_LEFT, 0, fl_th.dim,
		           "Flatpak is not installed, so Flathub apps cannot be installed here.");
	} else if (flat && s->fl_remote == 0 && v.state == ST_AVAIL) {
		if (fl_button(ui, ID_ADD_REMOTE, fl_r(tx, by, 150, 34), "Add Flathub first", FL_BTN_ACCENT))
			queue(s, A_ADD_REMOTE, 0);
	} else if (s->ask_remove) {
		char q[200];
		snprintf(q, sizeof q, "Remove %s?", v.name);
		fl_text_in(cv, ui->f_med, fl_r(tx, by, 300, 34), FL_LEFT, 0, fl_th.text, q);
		int qw = fl_text_width(ui->f_med, q, -1) + 18;
		if (fl_button(ui, ID_CONFIRM, fl_r(tx + qw, by, 100, 34), "Remove", FL_BTN_ACCENT))
			queue(s, A_REMOVE, 0);
		if (fl_button(ui, ID_CANCEL, fl_r(tx + qw + 108, by, 100, 34), "Cancel", FL_BTN_OUTLINE))
			queue(s, A_CANCEL, 0);
	} else {
		int bx = tx;
		if (v.state == ST_AVAIL) {
			int unlisted = flat && !s->fl_have;
			if (!unlisted && fl_button(ui, ID_PRIMARY, fl_r(bx, by, 128, 34), "Install", FL_BTN_ACCENT))
				queue(s, A_INSTALL, 0);
			bx += 136;
			if (flat && !unlisted) {
				/* what an install costs: the runtimes come along (often 1 to 2 GB the first time) */
				char hint[300], fr[40] = "";
				if (s->free_mb >= 0)
					snprintf(fr, sizeof fr, "%.1f GB free. ", (double)s->free_mb / 1024.0);
				snprintf(hint, sizeof hint, "%sRuntimes come along: 1 to 2 GB the first time.", fr);
				text_fit(ui, s->small, bx + 4, by + (s->live ? 0 : 9), tx + tw - bx - 4,
				         s->free_mb >= 0 && s->free_mb < 1536 ? fl_th.err : fl_th.dim, hint);
				if (s->live)
					text_fit(ui, s->small, bx + 4, by + 17, tx + tw - bx - 4, fl_th.dim,
					         "Live system: apps stay in memory, gone at shutdown.");
			}
		} else {
			if (v.state == ST_OLDER) {
				if (fl_button(ui, ID_PRIMARY, fl_r(bx, by, 128, 34), "Update", FL_BTN_ACCENT))
					queue(s, A_UPDATE, 0);
				bx += 136;
			}
			if (flat) {
				if (fl_button(ui, ID_SECOND, fl_r(bx, by, 110, 34), "Open", v.state == ST_INST ? FL_BTN_ACCENT : FL_BTN_OUTLINE))
					queue(s, A_OPEN_APP, 0);
				bx += 118;
			}
			if (fl_button(ui, ID_THIRD, fl_r(bx, by, 110, 34), "Remove", FL_BTN_OUTLINE))
				queue(s, A_ASK_REMOVE, 0);
		}
	}
	cv->clip = save;
}

static void draw_shots(fl_ui *ui, struct St *s, int x, int *y, int w, fl_rect view)
{
	fl_canvas *cv = &ui->cv;
	int n = s->det.nshot;
	if (!s->det_ok || n == 0)
		return;
	int bw = w > 760 ? 760 : w, bh = bw * 9 / 16;
	if (bh > 400)
		bh = 400;
	fl_rect box = fl_r(x, *y, bw, bh);
	if (box.y + box.h >= view.y && box.y <= view.y + view.h) {
		fl_fill_rect(cv, box, fl_th.panel);
		fl_stroke_rect(cv, box, 1, fl_th.line);
		if (s->cur.shot >= n)
			s->cur.shot = 0;
		const simg *im = shot_get(s, s->cur.shot, bw - 2, bh - 2);
		if (im) {
			simg_draw(cv, im, box.x + (box.w - im->w) / 2, box.y + (box.h - im->h) / 2);
		} else {
			int failed = 0; /* the image never arrived: say so instead of waiting for ever */
			char key[200];
			snprintf(key, sizeof key, "s:%s:%d:%dx%d", s->db.a[s->cur.app.idx].id, s->cur.shot, bw - 2, bh - 2);
			for (int i = 0; i < IMG_MAX; i++)
				if (!strcmp(s->im[i].key, key) && s->im[i].state == IM_FAILED)
					failed = 1;
			fl_text_in(cv, ui->f_reg, box, FL_CENTER, 0, fl_th.dim, failed ? "This screenshot is not available." : "Loading screenshot...");
		}
		if (n > 1) {
			int aw = 36;
			fl_rect lr = fl_r(box.x + 10, box.y + (box.h - aw) / 2, aw, aw), rr = fl_r(box.x + box.w - 10 - aw, lr.y, aw, aw);
			int rl = fl_widget(ui, ID_SHOT_PREV, lr, 0), rrres = fl_widget(ui, ID_SHOT_NEXT, rr, 0);
			fl_fill_rect(cv, lr, (rl & FL_HOVER) ? fl_th.raised : fl_th.panel);
			fl_stroke_rect(cv, lr, 1, fl_th.line);
			fl_icon_draw(cv, FL_ICON_BACK, lr.x + 8, lr.y + 8, 20, fl_th.text);
			fl_fill_rect(cv, rr, (rrres & FL_HOVER) ? fl_th.raised : fl_th.panel);
			fl_stroke_rect(cv, rr, 1, fl_th.line);
			fl_icon_draw(cv, FL_ICON_FORWARD, rr.x + 8, rr.y + 8, 20, fl_th.text);
			if (rl & FL_CLICK)
				queue(s, A_SHOT, -1);
			if (rrres & FL_CLICK)
				queue(s, A_SHOT, 1);
			char t[48];
			snprintf(t, sizeof t, "%d / %d", s->cur.shot + 1, n);
			int tw = fl_text_width(s->small, t, -1);
			fl_fill_rect(cv, fl_r(box.x + (box.w - tw) / 2 - 8, box.y + box.h - 26, tw + 16, 20), fl_th.panel);
			fl_text(cv, s->small, box.x + (box.w - tw) / 2, box.y + box.h - 24, fl_th.dim, t, -1);
		}
	}
	*y += bh + 28;
}

static void draw_app(fl_ui *ui, struct St *s, fl_rect area)
{
	fl_rect hdr = fl_cut_top(&area, HDR_H);
	struct rview v;
	rview(s, s->cur.app, &v);
	if (ui->focus != ID_SEARCH && s->det.nshot > 1) {
		if (ui->key == XKB_KEY_Left) {
			queue(s, A_SHOT, -1);
			ui->key = 0;
		} else if (ui->key == XKB_KEY_Right) {
			queue(s, A_SHOT, 1);
			ui->key = 0;
		}
	}
	struct sc c;
	scroll_begin(ui, s, area, &c);
	int x = c.x, w = c.w - SB_W - 6, y = c.y + 26;
	draw_shots(ui, s, x, &y, w, area);
	int tw = w > 720 ? 720 : w;
	heading(ui, s, x, y, "About");
	y += fl_font_height(s->big) + 10;
	const char *desc = s->det_ok && s->det.desc && s->det.desc[0] ? s->det.desc : NULL;
	if (desc)
		y += text_block(ui, ui->f_reg, x, y, tw, fl_th.text, desc, 60);
	else
		y += text_block(ui, ui->f_reg, x, y, tw, fl_th.text, v.sum[0] ? v.sum : "No description available.", 3);
	y += 30;
	heading(ui, s, x, y, "Details");
	y += fl_font_height(s->big) + 14;
	int flat = v.src == SRC_FH || v.src == SRC_FI;
	const char *ver = v.ver[0] ? v.ver : kv_get(s, "Version", NULL);
	info_row(ui, s, x, &y, tw, "VERSION", ver);
	char lic[200] = "";
	if (v.app && v.app->lic[0])
		pretty_license(v.app->lic, lic, sizeof lic);
	else if (kv_get(s, "License", "Licenses"))
		pretty_license(kv_get(s, "License", "Licenses"), lic, sizeof lic);
	info_row(ui, s, x, &y, tw, "LICENSE", lic);
	const char *dl = kv_get(s, "Download", NULL), *inst = kv_get(s, "Installed", NULL);
	if (flat || dl || inst) {
		info_row(ui, s, x, &y, tw, "DOWNLOAD SIZE", dl ? dl : s->info_ok ? NULL : "Loading...");
		info_row(ui, s, x, &y, tw, v.state == ST_AVAIL ? "INSTALLED SIZE" : "SIZE ON DISK",
		         inst ? inst : s->info_ok ? NULL : "Loading...");
	}
	info_row(ui, s, x, &y, tw, "SOURCE", flat ? "Flathub (installed for your account only)" : "FreeLFS repository");
	if (v.app && v.app->dev[0])
		info_row(ui, s, x, &y, tw, "DEVELOPER", v.app->dev);
	if (s->det_ok && s->det.home)
		info_row(ui, s, x, &y, tw, "WEBSITE", s->det.home);
	else if (kv_get(s, "URL", NULL))
		info_row(ui, s, x, &y, tw, "WEBSITE", kv_get(s, "URL", NULL));
	if (v.app && v.app->cats) {
		char cats[200] = "";
		for (int i = 0; i < NCATS; i++)
			if (v.app->cats & (1u << i))
				snprintf(cats + strlen(cats), sizeof cats - strlen(cats), "%s%s", cats[0] ? ", " : "", cat_names[i]);
		info_row(ui, s, x, &y, tw, "CATEGORIES", cats);
	}
	if (!flat) {
		info_row(ui, s, x, &y, tw, "DEPENDS ON", kv_get(s, "Depends on", NULL));
		info_row(ui, s, x, &y, tw, "GROUPS", kv_get(s, "Groups", NULL));
	}
	info_row(ui, s, x, &y, tw, flat ? "APPLICATION ID" : "PACKAGE", v.id);
	scroll_end(ui, s, &c, y);
	draw_app_header(ui, s, hdr);
}

/* ---- Installed and Updates ------------------------------------------------------------------------------------ */

struct rowctx {
	struct St *s;
	const struct ref *refs;
	int updates;
	uint32_t id;
};

static void draw_row(fl_ui *ui, fl_rect r, int i, int hover, int selected, void *ud)
{
	struct rowctx *rc = ud;
	struct St *s = rc->s;
	fl_canvas *cv = &ui->cv;
	struct rview v;
	rview(s, rc->refs[i], &v);
	fl_rect row = fl_r(r.x, r.y, r.w, ROW_H);
	if (hover || (selected && ui->focus == rc->id))
		fl_fill_rect(cv, row, fl_th.raised);
	fl_fill_rect(cv, fl_r(row.x + PAD, row.y + row.h - 1, row.w - 2 * PAD, 1), fl_th.line);
	draw_icon(ui, s, &v, row.x + PAD, row.y + (row.h - 40) / 2, 40);
	int tx = row.x + PAD + 40 + 16;
	char tag[48] = "";
	int tagw = 0;
	if (rc->updates || v.state == ST_OLDER) {
		snprintf(tag, sizeof tag, "Update available");
		tagw = fl_text_width(s->small, tag, -1);
		fl_text(cv, s->small, row.x + row.w - PAD - SB_W - 8 - tagw, row.y + 12, fl_th.accent, tag, -1);
	}
	fl_rect save = cv->clip;
	fl_clip(cv, fl_r(row.x, row.y, row.w - SB_W, row.h));
	text_fit(ui, ui->f_semi, tx, row.y + 11, row.w - (tx - row.x) - PAD - tagw - 24, fl_th.strong, v.name);
	char sub[300];
	const struct pk *old = NULL;
	if (rc->updates && v.src == SRC_PR)
		for (size_t k = 0; k < s->npkinst; k++)
			if (!strcmp(s->pkinst[k].name, v.name))
				old = &s->pkinst[k];
	if (old)
		snprintf(sub, sizeof sub, "%s to %s   %s", old->ver, v.ver, source_name(v.src));
	else
		snprintf(sub, sizeof sub, "%s%s%s", v.ver, v.ver[0] ? "   " : "", source_name(v.src));
	if (v.sum[0])
		snprintf(sub + strlen(sub), sizeof sub - strlen(sub), "   %s", v.sum);
	text_fit(ui, s->small, tx, row.y + 34, row.w - (tx - row.x) - PAD - 10, fl_th.dim, sub);
	cv->clip = save;
}

static void list_view(fl_ui *ui, struct St *s, fl_rect r, uint32_t id, struct ref *refs, int n, fl_list_state *ls,
                      int updates)
{
	struct rowctx rc = {s, refs, updates, id};
	fl_rect lr = fl_r(r.x, r.y, r.w, r.h);
	int act = fl_list(ui, id, lr, n, ROW_H, ls, draw_row, &rc);
	if (act >= 0 && act < n)
		queue(s, A_OPEN_REF, refs[act].src * 100000 + refs[act].idx);
}

static void chip(fl_ui *ui, struct St *s, fl_rect r, uint32_t id, const char *label, int on, int mode)
{
	fl_fill_rect(&ui->cv, r, on ? fl_th.select : fl_th.panel);
	int res = fl_widget(ui, id, r, 0);
	if (res & FL_HOVER)
		fl_fill_rect(&ui->cv, r, on ? fl_th.select : fl_th.raised);
	fl_stroke_rect(&ui->cv, r, 1, on ? fl_th.accent : fl_th.line);
	fl_text_in(&ui->cv, ui->f_med, r, FL_CENTER, 0, on ? fl_th.strong : fl_th.text, label);
	if (res & FL_CLICK)
		queue(s, A_CHIP, mode);
}

static void draw_installed(fl_ui *ui, struct St *s, fl_rect body)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, body, fl_th.bg);
	fl_rect top = fl_cut_top(&body, 64);
	int x = top.x + PAD, y = top.y + 16;
	char a[48], b[48];
	snprintf(a, sizeof a, "Apps  %d", s->niapp);
	snprintf(b, sizeof b, "System packages  %d", s->nipk);
	chip(ui, s, fl_r(x, y, fl_text_width(ui->f_med, a, -1) + 32, 32), ID_CHIP0, a, s->cur.inst_mode == 0, 0);
	x += fl_text_width(ui->f_med, a, -1) + 32 + 8;
	chip(ui, s, fl_r(x, y, fl_text_width(ui->f_med, b, -1) + 32, 32), ID_CHIP1, b, s->cur.inst_mode == 1, 1);
	if (s->cur.inst_mode == 0 && s->fl_have && !s->running &&
	    fl_button(ui, ID_UNUSED, fl_r(top.x + top.w - PAD - 190, y, 190, 32), "Remove unused runtimes", FL_BTN_OUTLINE))
		queue(s, A_UNUSED, 0);
	fl_fill_rect(cv, fl_r(top.x, top.y + top.h - 1, top.w, 1), fl_th.line);
	if (s->nilist == 0) {
		int x2 = body.x + PAD, w = body.w - 2 * PAD;
		if (s->cur.inst_mode == 0 && !s->fl_have)
			notice(ui, s, x2, body.y + 28, w, "Flatpak is not installed.",
			       "Flathub apps run through Flatpak. Install the flatpak package first.");
		else if (!(s->cur.inst_mode == 0 ? s->fin_loaded : s->pkinst_loaded))
			notice(ui, s, x2, body.y + 28, w, "Loading...", NULL);
		else if (s->query[0])
			notice(ui, s, x2, body.y + 28, w, "Nothing installed matches.", NULL);
		else if (s->cur.inst_mode == 0)
			notice(ui, s, x2, body.y + 28, w, "No apps installed yet.",
			       "Find one in Explore and press Install. Apps from Flathub are installed for your account only.");
		else
			notice(ui, s, x2, body.y + 28, w, "No packages are installed.", NULL);
		return;
	}
	list_view(ui, s, body, ID_ILIST, s->ilist, s->nilist, &s->ils, 0);
}

static void draw_updates(fl_ui *ui, struct St *s, fl_rect body)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, body, fl_th.bg);
	fl_rect top = fl_cut_top(&body, 84);
	int checking = job_running(&s->job[J_FUPD]) || job_running(&s->job[J_SYNC]) || job_running(&s->job[J_PKREPO]) ||
		       job_running(&s->job[J_PKUPD]);
	char t[120], sub[160] = "";
	if (checking)
		snprintf(t, sizeof t, "Checking for updates...");
	else if (s->nulist)
		snprintf(t, sizeof t, "%d update%s available", s->nulist, s->nulist == 1 ? "" : "s");
	else
		snprintf(t, sizeof t, "Everything is up to date");
	if (!checking && s->checked) {
		struct tm tm;
		localtime_r(&s->checked, &tm);
		char hm[16];
		strftime(hm, sizeof hm, "%H:%M", &tm);
		snprintf(sub, sizeof sub, "Last checked at %s. Flatpak apps and FreeLFS packages. Nothing is installed until you say so.", hm);
	}
	fl_text(cv, s->big, top.x + PAD, top.y + 16, fl_th.strong, t, -1);
	fl_text(cv, s->small, top.x + PAD, top.y + 16 + fl_font_height(s->big) + 6, fl_th.dim, sub, -1);
	int bx = top.x + top.w - PAD - 110;
	if (s->nulist && !s->running && fl_button(ui, ID_UPD_ALL, fl_r(bx - 124, top.y + 16, 116, 34), "Update all", FL_BTN_ACCENT))
		queue(s, A_UPDATE_ALL, 0);
	if (!checking && fl_button(ui, ID_CHECK, fl_r(bx, top.y + 16, 110, 34), "Check now", FL_BTN_OUTLINE))
		queue(s, A_CHECK, 0);
	fl_fill_rect(cv, fl_r(top.x, top.y + top.h - 1, top.w, 1), fl_th.line);
	if (checking)
		bar_indeterminate(ui, fl_r(top.x, top.y + top.h - 3, top.w, 2));
	if (s->nulist == 0)
		return;
	list_view(ui, s, body, ID_ULIST, s->ulist, s->nulist, &s->uls, 1);
}

/* ---- bars --------------------------------------------------------------------------------------------------------- */

static int bar_height(const struct St *s)
{
	return s->bar_visible ? (s->bar_log ? BAR_LOG_H : BAR_H) : 0;
}

static void draw_bar(fl_ui *ui, struct St *s, fl_rect b)
{
	fl_canvas *cv = &ui->cv;
	struct job *j = &s->job[J_CHANGE];
	fl_fill_rect(cv, b, fl_th.panel);
	fl_fill_rect(cv, fl_r(b.x, b.y, b.w, 1), fl_th.line);
	int bw = 84, rx = b.x + b.w - 14;
	if (!s->running && fl_button(ui, ID_BAR_CLOSE, fl_r(rx - bw, b.y + 10, bw, 30), "Close", FL_BTN_OUTLINE))
		queue(s, A_BAR_CLOSE, 0);
	if (s->running && s->run.kind != CK_PKINSTALL && s->run.kind != CK_PKREMOVE &&
	    fl_button(ui, ID_BAR_CANCEL, fl_r(rx - bw, b.y + 10, bw, 30), "Cancel", FL_BTN_OUTLINE))
		queue(s, A_BAR_CANCEL, 0);
	int dw = 90;
	if (fl_button(ui, ID_BAR_DETAILS, fl_r(rx - bw - 8 - dw, b.y + 10, dw, 30), s->bar_log ? "Hide log" : "Show log",
	              FL_BTN_OUTLINE))
		queue(s, A_BAR_LOG, 0);
	int lw = b.w - 28 - bw - dw - 24;
	text_fit(ui, ui->f_semi, b.x + 16, b.y + 15, lw, s->bar_err ? fl_th.err : fl_th.strong, s->bar_text);
	fl_rect bar = fl_r(b.x + 16, b.y + 44, b.w - 32, 4);
	fl_fill_rect(cv, bar, fl_th.line);
	char sub[200] = "";
	if (s->running) {
		int fr = progress_of(s);
		int st = 0, tot = 0;
		if (j->buf[0] && steps_of(j->buf[0], j->n[0], &st, &tot))
			snprintf(sub, sizeof sub, "Step %d of %d", st, tot);
		if (s->run.kind != CK_PKINSTALL && s->run.kind != CK_PKREMOVE && s->run.kind != CK_FREMOVE &&
		    s->run.kind != CK_FUNUSED) {
			/* flatpak says nothing while it downloads: show what the network brings in */
			net_tick(s);
			double got = (double)(s->net_prev - s->net0);
			char b[40];
			size_t l = strlen(sub);
			if (got >= 1024.0 * 1024.0) {
				fmt_bytes(got, b, sizeof b);
				snprintf(sub + l, sizeof sub - l, "%s%s received", l ? "   " : "", b);
				l = strlen(sub);
				if (s->net_rate > 1024.0) {
					fmt_bytes(s->net_rate, b, sizeof b);
					snprintf(sub + l, sizeof sub - l, "   %s/s", b);
				}
			} else if (mono_secs_running(s) > 6) {
				snprintf(sub + l, sizeof sub - l, "%swaiting for the network", l ? "   " : "");
			}
		}
		if (s->nq)
			snprintf(sub + strlen(sub), sizeof sub - strlen(sub), "%s%d more waiting", sub[0] ? "   " : "", s->nq);
		if (fr >= 0)
			fl_fill_rect(cv, fl_r(bar.x, bar.y, bar.w * fr / 1000, bar.h), fl_th.accent);
		else
			bar_indeterminate(ui, bar);
	} else {
		fl_fill_rect(cv, bar, s->bar_err ? fl_th.err : fl_th.accent);
		snprintf(sub, sizeof sub, "%s", s->bar_sub);
	}
	/* time stays on the right of the sub line for a running change */
	if (sub[0])
		fl_text(cv, s->small, b.x + 16, b.y + 50, fl_th.dim, sub, -1);
	if (s->bar_log) {
		fl_rect body = fl_r(b.x + 16, b.y + 72, b.w - 32, b.h - 72 - 8);
		int lh = fl_font_height(s->mono) + 2, rows = body.h / lh;
		const char *buf = j->buf[0] ? j->buf[0] : "";
		size_t n = j->n[0];
		while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
			n--;
		size_t st = n;
		int lines = 0;
		while (st > 0 && lines < rows) {
			st--;
			if (buf[st] == '\n' && ++lines == rows) {
				st++;
				break;
			}
		}
		fl_rect save = cv->clip;
		fl_clip(cv, body);
		int y = body.y;
		for (size_t i = st; i < n;) {
			const char *e = memchr(buf + i, '\n', n - i);
			size_t len = e ? (size_t)(e - (buf + i)) : n - i;
			fl_text(cv, s->mono, body.x, y, fl_th.text, buf + i, (int)len);
			y += lh;
			i += len + 1;
		}
		if (n == 0)
			fl_text(cv, s->mono, body.x, y, fl_th.dim, s->running ? "Waiting for output..." : "No output.", -1);
		cv->clip = save;
	}
}

static void draw_top(fl_ui *ui, struct St *s, fl_rect top)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, top, fl_th.panel);
	fl_fill_rect(cv, fl_r(top.x, top.y + top.h - 1, top.w, 1), fl_th.line);
	int x = top.x + 10;
	if (s->nstack > 0) {
		if (fl_icon_button(ui, ID_BACK, fl_r(x, top.y + 8, 36, 36), FL_ICON_BACK, 20, fl_th.text))
			queue(s, A_BACK, 0);
	} else {
		fl_icon_draw(cv, FL_ICON_BACK, x + 8, top.y + 16, 20, fl_th.line); /* nowhere to go back to */
	}
	x += 44;
	char updl[40];
	int nupd = (int)s->nupd + (int)s->npkupd;
	struct { const char *label; int id, tab; } tabs[3] = {{"Explore", ID_TAB_EXPLORE, TAB_EXPLORE},
	                                                      {"Installed", ID_TAB_INST, TAB_INST},
	                                                      {"Updates", ID_TAB_UPD, TAB_UPD}};
	for (int i = 0; i < 3; i++) {
		const char *label = tabs[i].label;
		int tw = fl_text_width(ui->f_semi, label, -1);
		int badge = i == 2 && nupd > 0;
		snprintf(updl, sizeof updl, "%d", nupd);
		int bw = badge ? fl_text_width(s->tiny, updl, -1) + 12 : 0;
		fl_rect r = fl_r(x, top.y, tw + 32 + (badge ? bw + 6 : 0), top.h - 1);
		int res = fl_widget(ui, (uint32_t)tabs[i].id, r, 0);
		int on = s->tab == tabs[i].tab;
		if (res & FL_HOVER && !on)
			fl_fill_rect(cv, r, fl_th.raised);
		fl_text(cv, ui->f_semi, r.x + 16, top.y + (top.h - fl_font_height(ui->f_semi)) / 2, on ? fl_th.strong : fl_th.dim,
		        label, -1);
		if (badge) {
			fl_rect br = fl_r(r.x + 16 + tw + 6, top.y + (top.h - 18) / 2, bw, 18);
			fl_fill_rect(cv, br, fl_th.accent);
			fl_text_in(cv, s->tiny, br, FL_CENTER, 0, fl_th.on_accent, updl);
		}
		if (on)
			fl_fill_rect(cv, fl_r(r.x, top.y + top.h - 3, r.w, 3), fl_th.accent);
		if (res & FL_CLICK)
			queue(s, A_TAB, tabs[i].tab);
		x += r.w + 2;
	}
	int sw = top.w / 3 > 320 ? 320 : top.w / 3;
	if (sw < 180)
		sw = 180;
	fl_rect sr = fl_r(top.x + top.w - 12 - sw, top.y + 10, sw, 32);
	if (s->tab != TAB_UPD) {
		const char *hint = s->tab == TAB_INST ? "Filter installed" : "Search apps and packages";
		int r = fl_textfield(ui, ID_SEARCH, sr, s->query, sizeof s->query, hint, ui->f_reg);
		if (r & FL_EDIT_CHANGED) {
			if (s->tab == TAB_INST)
				queue(s, A_SEARCH, 0);
			else
				fl_timer_arm(s->search_timer, 250, 0);
		}
		if (r & FL_EDIT_ENTER) {
			queue(s, A_SEARCH, 0);
			ui->focus = 0;
		}
	}
}

static void draw_side(fl_ui *ui, struct St *s, fl_rect side)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, side, fl_th.panel);
	fl_fill_rect(cv, fl_r(side.x + side.w - 1, side.y, 1, side.h), fl_th.line);
	int y = side.y + 10;
	enum { RH = 34 };
	int cur_row = s->cur.page == PG_HOME ? 0 : s->cur.page == PG_CAT ? s->cur.cat + 1 : s->cur.page == PG_PKGS ? NCATS + 1 : -1;
	for (int i = 0; i < NCATS + 2; i++) {
		const char *label = i == 0 ? "Home" : i == NCATS + 1 ? "FreeLFS packages" : cat_names[i - 1];
		if (i == 1) {
			fl_text_ex(cv, s->tiny, side.x + 18, y + 12, fl_th.dim, "CATEGORIES", -1, 64 * 1);
			y += 34;
		}
		if (i == NCATS + 1) {
			fl_fill_rect(cv, fl_r(side.x + 14, y + 6, side.w - 29, 1), fl_th.line);
			y += 14;
		}
		fl_rect r = fl_r(side.x, y, side.w - 1, RH);
		int res = fl_widget(ui, (uint32_t)(ID_SIDE + i), r, 0);
		if (cur_row == i)
			fl_fill_rect(cv, r, fl_th.select);
		else if (res & FL_HOVER)
			fl_fill_rect(cv, r, fl_th.raised);
		if (cur_row == i)
			fl_fill_rect(cv, fl_r(r.x, r.y, 3, r.h), fl_th.accent);
		char cnt[16] = "";
		if (i >= 1 && i <= NCATS && s->idx_state == IDX_READY)
			snprintf(cnt, sizeof cnt, "%d", cat_total(s, i - 1));
		else if (i == NCATS + 1 && s->repo_cfg && s->pkrepo_loaded)
			snprintf(cnt, sizeof cnt, "%zu", s->npkrepo);
		int cw = cnt[0] ? fl_text_width(s->small, cnt, -1) : 0;
		fl_text_in(cv, ui->f_reg, fl_r(r.x, r.y, r.w - cw - 22, r.h), FL_LEFT, 18, cur_row == i ? fl_th.strong : fl_th.text,
		           label);
		if (cw)
			fl_text(cv, s->small, r.x + r.w - 14 - cw, r.y + (r.h - fl_font_height(s->small)) / 2, fl_th.dim, cnt, -1);
		if (res & FL_CLICK) {
			if (i == 0)
				queue(s, A_HOME, 0);
			else if (i == NCATS + 1)
				queue(s, A_PKGS, 0);
			else
				queue(s, A_CAT, i - 1);
		}
		y += RH;
	}
}

void draw(fl_ui *ui, void *ud)
{
	struct St *s = ud;
	fl_canvas *cv = &ui->cv;
	frame_begin(s);
	fl_rect c = fl_win_content(ui->win);
	if (!s->inited) {
		s->inited = 1;
		ui->focus = 0;
	}
	if (ui->key == XKB_KEY_f && (ui->mods & FL_MOD_CTRL) && s->tab != TAB_UPD) {
		ui->focus = ID_SEARCH;
		ui->key = 0;
	}
	if (ui->key == XKB_KEY_Escape) {
		if (ui->focus == ID_SEARCH && s->query[0]) {
			s->query[0] = 0;
			queue(s, A_SEARCH, 0);
		} else if (s->nstack > 0) {
			queue(s, A_BACK, 0);
		}
		ui->key = 0;
	}
	fl_fill_rect(cv, c, fl_th.bg);
	fl_rect area = c;
	fl_rect top = fl_cut_top(&area, TOP_H);
	fl_rect bar = fl_cut_bottom(&area, bar_height(s));
	fl_rect side = s->tab == TAB_EXPLORE ? fl_cut_left(&area, SIDE_W) : (fl_rect){0};
	switch (s->cur.page) {
	case PG_HOME: draw_home(ui, s, area); break;
	case PG_CAT:
	case PG_SEARCH:
	case PG_PKGS: draw_list_page(ui, s, area); break;
	case PG_APP: draw_app(ui, s, area); break;
	case PG_INST: draw_installed(ui, s, area); break;
	case PG_UPD: draw_updates(ui, s, area); break;
	}
	if (s->tab == TAB_EXPLORE)
		draw_side(ui, s, side);
	if (s->bar_visible)
		draw_bar(ui, s, bar);
	draw_top(ui, s, top);
	frame_end(s);
}
