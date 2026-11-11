/* tabstrip -- see tabstrip.h. SPDX-License-Identifier: MIT */
#include <math.h>
#include <string.h>

#include "tabstrip.h"

#define ID_TAB 0x1000
#define ID_TAB_CLOSE 0x2000
#define ID_TAB_NEW 0x3000
#define TAB_MAX_W 220
#define TAB_MIN_W 44
#define TAB_PAD 10
#define CLOSE_W 18
#define NEW_W 30

static void spinner(fl_ui *ui, int cx, int cy, int deg, fl_color c)
{
	/* three quarters of a circle, turning */
	fl_path *p = fl_path_new();
	const float r = 5.0f;
	for (int i = 0; i <= 14; i++) {
		float a = (float)(deg + i * 270 / 14) * 3.14159265f / 180.0f;
		float x = cx + r * cosf(a), y = cy + r * sinf(a);
		if (i == 0)
			fl_path_move(p, x, y);
		else
			fl_path_line(p, x, y);
	}
	fl_path_stroke(&ui->cv, p, 1.7f, c);
	fl_path_free(p);
}

struct ts_event ts_draw(fl_ui *ui, fl_rect r, const struct ts_item *it, int n, int active, int spin)
{
	struct ts_event ev = { -1, -1, 0 };
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, r, fl_th.bg);
	/* the line under the row; the active tab breaks it */
	fl_fill_rect(cv, fl_r(r.x, r.y + r.h - 1, r.w, 1), fl_th.line);

	int left = 6, avail = r.w - left - NEW_W - 8;
	int tw = n > 0 ? avail / n : TAB_MAX_W;
	if (tw > TAB_MAX_W)
		tw = TAB_MAX_W;
	if (tw < TAB_MIN_W)
		tw = TAB_MIN_W;
	int x = r.x + left;
	for (int i = 0; i < n; i++) {
		fl_rect t = fl_r(x, r.y + 3, tw, r.h - 3);
		int act = i == active;
		int hover = ui->mouse_in && fl_in(t, ui->mx, ui->my) && (!ui->active || (ui->active >= ID_TAB && ui->active < ID_TAB_NEW));
		fl_rect body = t, cl = fl_r(t.x + t.w - CLOSE_W - 4, t.y + (t.h - 1 - CLOSE_W) / 2 + 1, CLOSE_W, CLOSE_W);
		int show_close = act || hover;
		if (tw < 60 && !act && !hover)
			show_close = 0;
		if (show_close)
			body.w -= CLOSE_W + 6;
		/* tab background */
		if (act) {
			fl_fill_rect(cv, fl_r(t.x, t.y, t.w, t.h), fl_th.panel);
			fl_fill_rect(cv, fl_r(t.x, t.y, t.w, 2), fl_th.accent);
			fl_fill_rect(cv, fl_r(t.x, t.y + 2, 1, t.h - 2), fl_th.line);
			fl_fill_rect(cv, fl_r(t.x + t.w - 1, t.y + 2, 1, t.h - 2), fl_th.line);
		} else {
			if (hover)
				fl_fill_rect(cv, fl_r(t.x, t.y + 2, t.w, t.h - 3), fl_th.raised);
			else if (i + 1 < n && i + 1 != active)
				fl_fill_rect(cv, fl_r(t.x + t.w - 1, t.y + 9, 1, 14), fl_th.line);
		}
		/* the widgets: body selects, the close button closes, middle button closes */
		int st = fl_widget(ui, ID_TAB + i, body, 0);
		if (st & FL_CLICK)
			ev.select = i;
		if (ui->release & FL_BTN_M && ui->mouse_in && fl_in(t, ui->mx, ui->my))
			ev.close = i;
		if (show_close) {
			int cs = fl_widget(ui, ID_TAB_CLOSE + i, cl, 0);
			if (cs & (FL_HOVER | FL_DOWN))
				fl_fill_rect(cv, cl, act ? fl_th.raised : fl_th.line);
			fl_icon_draw(cv, FL_ICON_CLOSE, cl.x + (cl.w - 10) / 2, cl.y + (cl.h - 10) / 2, 10,
				     (cs & FL_HOVER) || act ? fl_th.text : fl_th.dim);
			if (cs & FL_CLICK)
				ev.close = i;
		}
		/* spinner and title */
		int tx = t.x + TAB_PAD;
		if (it[i].loading) {
			spinner(ui, tx + 6, t.y + (t.h + 2) / 2, spin, fl_th.accent);
			tx += 18;
		}
		fl_rect tr = fl_r(tx, t.y + 2, body.x + body.w - tx - 2, t.h - 3);
		if (tr.w > 8) {
			char buf[300];
			fl_font *f = act ? ui->f_med : ui->f_reg;
			fl_text_fit(f, it[i].title, tr.w, buf, sizeof buf);
			fl_color col = act || hover ? fl_th.text : it[i].discarded ? fl_mix(fl_th.dim, fl_th.bg, 90) : fl_th.dim;
			fl_text_in(cv, f, tr, FL_LEFT, 0, col, buf);
		}
		x += tw;
	}
	/* the + button */
	fl_rect nb = fl_r(x + 4, r.y + 3 + (r.h - 3 - 26) / 2, 26, 26);
	int ns = fl_widget(ui, ID_TAB_NEW, nb, 0);
	if (ns & (FL_HOVER | FL_DOWN))
		fl_fill_rect(cv, nb, fl_th.raised);
	fl_icon_draw(cv, FL_ICON_PLUS, nb.x + (nb.w - 14) / 2, nb.y + (nb.h - 14) / 2, 14,
		     ns & FL_HOVER ? fl_th.text : fl_th.dim);
	if (ns & FL_CLICK)
		ev.newtab = 1;
	return ev;
}
