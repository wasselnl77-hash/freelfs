/* deskicons.c -- icons on the desktop (fl_shell version 6, set_desktop).
 *
 * A small transparent surface at the top left of the screen, below every window and above the
 * wallpaper. It shows the programs whose .desktop file says X-FreeLFS-Desktop-Icon=true (and
 * whose TryExec exists, so "FreeLFS install" is there on the live system only). Click selects,
 * double click starts the program. Without version 6 of the protocol (an older flcomp) there are
 * no icons. Nothing runs and nothing is allocated when no entry asks for an icon.
 * SPDX-License-Identifier: MIT */
#include "flpanel.h"

#include <stdlib.h>
#include <string.h>

#define CELL_W 104
#define CELL_H 98
#define TILE 48
#define MARGIN 24
#define MAX_ICONS 8

struct desk {
	struct panel *p;
	fl_win *w;
	fl_font *f_glyph;
	struct entry e[MAX_ICONS];
	int n, sel;
};

static void desk_draw(fl_ui *ui, void *ud)
{
	struct desk *d = ud;
	fl_canvas *cv = &ui->cv;
	if (cv->px) /* transparent: clear what the buffer held before */
		for (int y = 0; y < cv->h; y++)
			memset(cv->px + (size_t)y * cv->stride, 0, (size_t)cv->w * 4);
	for (int i = 0; i < d->n; i++) {
		struct entry *e = &d->e[i];
		fl_rect cell = fl_r(0, i * CELL_H, CELL_W, CELL_H);
		fl_rect tile = fl_r((CELL_W - TILE) / 2, cell.y + 8, TILE, TILE);
		fl_rect lab = fl_r(2, tile.y + TILE + 6, CELL_W - 4, 20);
		int res = fl_widget(ui, 200 + (uint32_t)i, fl_r(0, cell.y, CELL_W, CELL_H - 8), 0);
		if (res & FL_CLICK) {
			d->sel = i;
			if (ui->clicks >= 2)
				launch(e->exec, e->terminal);
		}
		int hot = (res & (FL_HOVER | FL_DOWN)) != 0, sel = d->sel == i;
		fl_fill_rect(cv, tile, hot ? fl_th.select : fl_th.raised);
		fl_stroke_rect(cv, tile, 1, sel ? fl_th.accent : fl_th.line);
		if (sel)
			fl_fill_rect(cv, fl_r(tile.x, tile.y, TILE, 3), fl_th.accent);
		fl_text_in(cv, d->f_glyph, tile, FL_CENTER, 0, fl_th.text, e->glyph);
		/* the name sits on a solid flat box, so it reads on any wallpaper */
		char fit[96];
		int tw = fl_text_width(d->p->f_small, e->name, -1);
		if (tw > lab.w - 10)
			tw = lab.w - 10;
		fl_text_fit(d->p->f_small, e->name, lab.w - 10, fit, sizeof fit);
		fl_rect box = fl_r(lab.x + (lab.w - tw - 10) / 2, lab.y, tw + 10, lab.h);
		fl_fill_rect(cv, box, fl_th.panel);
		fl_stroke_rect(cv, box, 1, sel ? fl_th.accent : fl_th.line);
		fl_text_in(cv, d->p->f_small, box, FL_CENTER, 0, fl_th.text, fit);
	}
	/* a click on nothing of ours does not reach this surface (the compositor hit-tests alpha) */
}

static struct desk *the_desk;

static int desk_y(void)
{
	return MARGIN + (fl_lk.panel_top ? fl_lk.panel_h : 0);	/* below a panel at the top */
}

void desk_reposition(struct panel *p)
{
	if (the_desk && p->shell)
		fl_shell_set_desktop(p->shell, fl_win_surface(the_desk->w), MARGIN, desk_y());
}

void desk_init(struct panel *p)
{
	if (!p->shell || p->shell_ver < 6 || p->demo || fl_app_ppm(p->app))
		return;
	menu_init(p); /* loads the .desktop entries (it reloads them every time the menu opens) */
	struct desk *d = calloc(1, sizeof *d);
	if (!d)
		return;
	d->p = p;
	d->sel = -1;
	for (int i = 0; i < p->napps && d->n < MAX_ICONS; i++)
		if (p->apps[i].desktop)
			d->e[d->n++] = p->apps[i];
	/* the menu list is loaded again when the menu opens: do not keep it (about 100 KB of RSS) */
	free(p->apps);
	free(p->shown);
	p->apps = NULL;
	p->shown = NULL;
	p->napps = p->nshown = 0;
	if (!d->n) {
		free(d);
		return;
	}
	d->f_glyph = fl_font_mono(500, 26);
	fl_win_opts o = {
		.title = "desktop icons", .app_id = "flpanel-desktop", .w = CELL_W, .h = d->n * CELL_H, .raw = 1,
		.draw = desk_draw, .ud = d,
	};
	d->w = fl_win_new(p->app, &o);
	if (!d->w) {
		free(d);
		return;
	}
	the_desk = d;
	fl_shell_set_desktop(p->shell, fl_win_surface(d->w), MARGIN, desk_y());
	fl_win_map(d->w);
}
