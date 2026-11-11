/* flyout.c -- the volume flyout: slider, percent and a mute button
 * SPDX-License-Identifier: MIT
 *
 * A small flat popup above the volume icon (fl_shell set_flyout), drawn like
 * the start menu: panel colour, accent top edge, hairlines. Everything it
 * changes goes through vol_set_pct and vol_toggle_mute in panel.c, which
 * show the new level at once and tell wpctl in the background.
 * Keys: Left/Down and Right/Up 5%, PageUp/PageDown 10%, Home/End 0/100%,
 * M mutes, Esc closes. Drag the slider or click on the bar.
 */
#include <flui.h>
#include <stdio.h>

#include "flpanel.h"

enum { ID_MUTE = 1, ID_SLIDER = 2 };

static int key_step(uint32_t k)
{
	switch (k) {
	case XKB_KEY_Left: case XKB_KEY_Down: return -5;
	case XKB_KEY_Right: case XKB_KEY_Up: return 5;
	case XKB_KEY_Page_Down: return -10;
	case XKB_KEY_Page_Up: return 10;
	}
	return 0;
}

void flyout_draw(fl_ui *ui, void *ud)
{
	struct panel *p = ud;
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, fl_r(0, 0, FLY_W, FLY_H), fl_th.panel);
	fl_fill_rect(cv, fl_r(0, 0, 1, FLY_H), fl_th.line);
	fl_fill_rect(cv, fl_r(FLY_W - 1, 0, 1, FLY_H), fl_th.line);
	fl_fill_rect(cv, fl_r(0, 0, FLY_W, 2), fl_th.accent);

	if (ui->key == XKB_KEY_Escape) {
		ui->key = 0;
		flyout_close(p);
		return;
	}
	int known = p->vol_pct >= 0;
	int pct = known ? p->vol_pct : 0;
	int dim = p->vol_muted || !known;

	/* caption and level */
	char txt[16];
	snprintf(txt, sizeof txt, "%s", !known ? "No sound" : p->vol_muted ? "Muted" : "");
	if (known && !p->vol_muted)
		snprintf(txt, sizeof txt, "%d%%", pct);
	fl_text(cv, p->f_cap, 12, 14, fl_th.dim, "Volume", -1);
	int tw = fl_text_width(ui->f_med, txt, -1);
	fl_text(cv, ui->f_med, FLY_W - 12 - tw, 11, dim ? fl_th.dim : fl_th.text, txt, -1);

	/* mute button: shows what a click does not, the state */
	fl_rect mr = fl_r(8, 36, 32, 32);
	int mres = fl_widget(ui, ID_MUTE, mr, FL_FOCUSABLE);
	if (mres & (FL_HOVER | FL_DOWN))
		fl_fill_rect(cv, mr, fl_th.raised);
	if (mres & FL_FOCUS)
		fl_stroke_rect(cv, mr, 1, fl_th.accent);
	fl_icon_draw(cv, p->vol_muted ? FL_ICON_MUTE : FL_ICON_VOLUME, mr.x + 8, mr.y + 8, 16,
	             (mres & FL_HOVER) || !dim ? fl_th.text : fl_th.dim);
	if ((mres & FL_CLICK) || ((mres & FL_FOCUS) && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_space))) {
		ui->key = 0;
		vol_toggle_mute(p);
	}

	/* slider */
	fl_rect sr = fl_r(48, 36, FLY_W - 48 - 12, 32);
	int sres = fl_widget(ui, ID_SLIDER, sr, FL_FOCUSABLE);
	int x0 = sr.x + 6, x1 = sr.x + sr.w - 6;
	if (known && (sres & FL_DOWN)) {
		int v = (ui->mx - x0) * 100 / (x1 - x0);
		vol_set_pct(p, v);
	}
	if (known && (sres & FL_FOCUS)) {
		int st = key_step(ui->key);
		if (st) {
			vol_set_pct(p, p->vol_pct + st);
			ui->key = 0;
		} else if (ui->key == XKB_KEY_Home || ui->key == XKB_KEY_End) {
			vol_set_pct(p, ui->key == XKB_KEY_Home ? 0 : 100);
			ui->key = 0;
		}
	}
	if (ui->key == XKB_KEY_m || ui->key == XKB_KEY_M) {
		ui->key = 0;
		vol_toggle_mute(p);
	}
	pct = p->vol_pct >= 0 ? p->vol_pct : 0; /* may have just changed */
	int ty = sr.y + 14, tx = x0 + pct * (x1 - x0) / 100;
	fl_fill_rect(cv, fl_r(x0, ty, x1 - x0, 4), fl_th.line);
	fl_fill_rect(cv, fl_r(x0, ty, tx - x0, 4), dim ? fl_th.dim : fl_th.accent);
	fl_rect th = fl_r(tx - 5, sr.y + 6, 10, 20);
	fl_fill_rect(cv, th, !known ? fl_th.line : (sres & FL_DOWN) ? fl_th.accent : fl_th.text);
	if (sres & FL_FOCUS)
		fl_stroke_rect(cv, fl_inset(th, -3, -3), 1, fl_th.accent);
}
