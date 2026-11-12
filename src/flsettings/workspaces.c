/* workspaces.c -- the Workspaces page of flsettings
 * SPDX-License-Identifier: MIT
 *
 * How many workspaces the desktop has, as a grid: "Across" and "Down" steppers (1 to 8 each), the
 * total with a small picture of the grid, and quick choices (1, 2, 4, 6, 8). Everything is the one key
 * workspaces=COLSxROWS in desktop.conf, written with libflui's fl_conf_set like every other setting
 * here; flcomp applies it at once (windows on workspaces that are gone move to the last one) and the
 * panel's pager follows. The pager's right-click menu in flpanel writes the same key.
 */
#include <flui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pages.h"
#include "workspaces.h"

#define ROW 56
#define MAXN 8
enum { ID_W_ACROSS = 900 /* -, + */, ID_W_DOWN = 904, ID_W_QUICK = 910 /* .. 914 */ };

struct workspaces {
	fl_app *app;
	fl_font *head, *small;
};

void ws_grid_get(int *cols, int *rows)
{
	int c = 2, r = 2; /* flcomp's default */
	const char *v = fl_conf_get("workspaces", NULL);
	int cc, rr = 1;
	if (v && sscanf(v, "%d%*1[xX]%d", &cc, &rr) >= 1) {
		c = cc < 1 ? 1 : cc > MAXN ? MAXN : cc;
		r = rr < 1 ? 1 : rr > MAXN ? MAXN : rr;
	}
	*cols = c;
	*rows = r;
}

int ws_grid_set(int cols, int rows)
{
	char v[16];
	cols = cols < 1 ? 1 : cols > MAXN ? MAXN : cols;
	rows = rows < 1 ? 1 : rows > MAXN ? MAXN : rows;
	snprintf(v, sizeof v, "%dx%d", cols, rows);
	return fl_conf_set("workspaces", v);
}

struct workspaces *ws_new(fl_app *app, fl_font *head, fl_font *small)
{
	struct workspaces *w = calloc(1, sizeof *w);
	if (!w)
		return NULL;
	w->app = app;
	w->head = head;
	w->small = small;
	return w;
}

void ws_free(struct workspaces *w)
{
	free(w);
}

/* a square button with a sign; dim and inert when it cannot go further. 1 when pressed */
static int step_button(fl_ui *ui, uint32_t id, fl_rect r, const char *sign, int enabled)
{
	if (!enabled) {
		fl_stroke_rect(&ui->cv, r, 1, fl_th.line);
		fl_text_in(&ui->cv, ui->f_med, r, FL_CENTER, 0, fl_mix(fl_th.panel, fl_th.dim, 120), sign);
		return 0;
	}
	int res = fl_widget(ui, id, r, FL_FOCUSABLE);
	fl_fill_rect(&ui->cv, r, (res & FL_DOWN) ? fl_th.select : (res & FL_HOVER) ? fl_th.raised : 0);
	fl_stroke_rect(&ui->cv, r, 1, (res & FL_FOCUS) ? fl_th.accent : (res & FL_HOVER) ? fl_th.dim : fl_th.line);
	fl_text_in(&ui->cv, ui->f_med, r, FL_CENTER, 0, fl_th.text, sign);
	if ((res & FL_FOCUS) && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_space)) {
		ui->key = 0;
		return 1;
	}
	return res & FL_CLICK;
}

/* [-] 3 [+] at the right edge of a row; returns the new value */
static int stepper(fl_ui *ui, uint32_t id, int x_right, int y, int v)
{
	char num[8];
	snprintf(num, sizeof num, "%d", v);
	fl_rect m = fl_r(x_right - 32, y + 12, 32, 32), n = fl_r(m.x - 44, m.y, 44, 32), l = fl_r(n.x - 32, m.y, 32, 32);
	int nv = v;
	if (step_button(ui, id, l, "\xe2\x88\x92", v > 1))
		nv = v - 1;
	fl_stroke_rect(&ui->cv, n, 1, fl_th.line);
	fl_text_in(&ui->cv, ui->f_med, n, FL_CENTER, 0, fl_th.strong, num);
	if (step_button(ui, id + 1, m, "+", v < MAXN))
		nv = v + 1;
	return nv;
}

/* the grid as the pager draws it: flat cells, as large as fit the box */
static void preview(fl_ui *ui, fl_rect box, int cols, int rows)
{
	int gap = 2;
	int ch = (box.h - (rows - 1) * gap) / rows;
	if (ch > 14)
		ch = 14;
	if (ch < 3)
		ch = 3;
	int cw = ch * 8 / 5;
	int gw = cols * cw + (cols - 1) * gap, gh = rows * ch + (rows - 1) * gap;
	for (int i = 0; i < cols * rows; i++) {
		fl_rect c = fl_r(box.x + box.w - gw + (i % cols) * (cw + gap), box.y + (box.h - gh) / 2 + (i / cols) * (ch + gap), cw, ch);
		fl_fill_rect(&ui->cv, c, i == 0 ? fl_th.accent : fl_th.raised);
		if (i)
			fl_stroke_rect(&ui->cv, c, 1, fl_th.line);
	}
}

/* a dim paragraph, wrapped at word boundaries to w; returns the y below it */
static int paragraph(fl_ui *ui, fl_font *f, int x, int y, int w, const char *text)
{
	int lh = fl_font_height(f) + 4;
	char line[160];
	const char *p = text;
	while (*p) {
		int n = 0, last = 0;
		while (p[n] && n < (int)sizeof line - 1) {
			if (p[n] == ' ')
				last = n;
			if (fl_text_width(f, p, n + 1) > w) {
				if (last)
					n = last;
				break;
			}
			n++;
		}
		if (!p[n] || n >= (int)sizeof line - 1) /* the rest fits */
			n = (int)strlen(p) < (int)sizeof line - 1 ? (int)strlen(p) : (int)sizeof line - 1;
		if (n < 1)
			n = 1; /* a word wider than the page: never stand still */
		snprintf(line, sizeof line, "%.*s", n, p);
		fl_text(&ui->cv, f, x, y, fl_th.dim, line, -1);
		y += lh;
		p += n;
		while (*p == ' ')
			p++;
	}
	return y;
}

static const struct { int cols, rows; } quick[] = {{1, 1}, {2, 1}, {2, 2}, {3, 2}, {4, 2}};

int ws_draw(struct workspaces *w, fl_ui *ui, fl_rect c)
{
	int x = c.x + 28, cw = c.w - 56, y = c.y + 24;
	int cols, rows;
	ws_grid_get(&cols, &rows);

	pg_section(ui, w->head, x, y, cw, "Workspaces");
	y += 28;

	pg_row_text(ui, w->small, x, y, cw - 240, "Across", "How many workspaces side by side, 1 to 8");
	int nc = stepper(ui, ID_W_ACROSS, x + cw, y, cols);
	y += ROW;
	pg_row_text(ui, w->small, x, y, cw - 240, "Down", "How many rows of workspaces, 1 to 8");
	int nr = stepper(ui, ID_W_DOWN, x + cw, y, rows);
	y += ROW;

	char total[64], what[64];
	snprintf(total, sizeof total, "%d workspace%s", cols * rows, cols * rows == 1 ? "" : "s");
	snprintf(what, sizeof what, "%d across, %d down", cols, rows);
	pg_row_text(ui, w->small, x, y, cw - 240, total, what);
	preview(ui, fl_r(x + cw - 200, y + 6, 200, ROW - 12), cols, rows);
	y += ROW;

	pg_row_text(ui, w->small, x, y, cw - 260, "Quick choice", "Total number of workspaces");
	for (int i = 0; i < 5; i++) {
		char lab[8];
		snprintf(lab, sizeof lab, "%d", quick[i].cols * quick[i].rows);
		fl_rect r = fl_r(x + cw - 5 * 44 - 4 * 4 + i * 48, y + 12, 44, 32);
		if (pg_choice(ui, ID_W_QUICK + (uint32_t)i, r, lab, cols == quick[i].cols && rows == quick[i].rows)) {
			nc = quick[i].cols;
			nr = quick[i].rows;
		}
	}
	y += ROW;
	if (nc != cols || nr != rows)
		ws_grid_set(nc, nr);

	y += 4;
	y = paragraph(ui, w->small, x, y, cw,
	              "Windows on workspaces that are taken away move to the last workspace that is left.");
	y += 24;

	pg_section(ui, w->head, x, y, cw, "Shortcuts");
	y += 36;
	static const struct { const char *keys, *what; } keys[] = {
		{"Super+Ctrl+Arrows", "Switch to the workspace next to this one"},
		{"Super+Shift+Arrows", "Move the window to the workspace next to this one"},
		{"Super+Tab", "Overview of all workspaces"},
	};
	int kw = 0;
	for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
		int e = pg_key_chips(ui, w->small, 0, 0, 22, keys[i].keys, 0);
		kw = e > kw ? e : kw;
	}
	kw += 24;
	for (size_t i = 0; i < sizeof keys / sizeof *keys; i++) {
		char fit[160];
		pg_key_chips(ui, w->small, x, y + 4, 22, keys[i].keys, 1);
		fl_text_fit(ui->f_reg, keys[i].what, cw - kw, fit, sizeof fit);
		fl_text_in(&ui->cv, ui->f_reg, fl_r(x + kw, y, cw - kw, 30), FL_LEFT, 0, fl_th.dim, fit);
		y += 30;
	}
	y += 8;
	y = paragraph(ui, w->small, x, y, cw,
	              "In the panel, click a square of the workspace picture to go there, scroll over it to step through them, "
	              "or right-click it to add or remove a workspace.");
	return y + 24 - c.y;
}
