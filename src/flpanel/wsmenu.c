/* wsmenu.c -- the pager's right-click menu: overview, add or remove a workspace, workspace settings
 * SPDX-License-Identifier: MIT
 *
 * A small flat popup above the pager (fl_shell set_flyout, like the volume flyout). "Add workspace"
 * and "Remove workspace" change workspaces=COLSxROWS in desktop.conf through libflui's fl_conf_set,
 * the same call Settings uses; flcomp applies the file at once and answers with its workspaces
 * event, which is what redraws the pager. Add makes one more column (a row once there are 8
 * columns), remove one column fewer (a row fewer once there is only one column), never below 1x1
 * and never above 8x8. "Workspace settings" opens flsettings on its Workspaces page.
 * Keys: Up/Down, Enter or Space, Esc.
 */
#include <flui.h>
#include <stdio.h>
#include <string.h>

#include "flpanel.h"

enum { WI_OVERVIEW, WI_ADD, WI_REMOVE, WI_SETTINGS, WI_COUNT };
enum { ID_BASE = 10 };

#define ROW_H 30
#define SEP_H 9
#define PAD_T 6
#define PAD_B 4

/* y of each item; WSM_H in flpanel.h is the sum */
static int item_y(int i)
{
	return 2 + PAD_T + i * ROW_H + (i >= WI_ADD ? SEP_H : 0) + (i >= WI_SETTINGS ? SEP_H : 0);
}

/* The grid as desktop.conf has it (what the panel last asked for), else what flcomp last said.
 * The same parse as flcomp: COLSxROWS, a bare N is N x 1. */
static void grid_now(const struct panel *p, int *cols, int *rows)
{
	int c = p->ws_cols > 0 ? p->ws_cols : 1, r = p->ws_rows > 0 ? p->ws_rows : 1;
	const char *v = fl_conf_get("workspaces", NULL);
	int cc, rr = 1;
	if (v && sscanf(v, "%d%*1[xX]%d", &cc, &rr) >= 1) {
		c = cc < 1 ? 1 : cc > 8 ? 8 : cc;
		r = rr < 1 ? 1 : rr > 8 ? 8 : rr;
	}
	*cols = c;
	*rows = r;
}

/* one workspace more (dir 1) or fewer (dir -1): 1 and the new grid, or 0 when there is no room */
static int grid_step(const struct panel *p, int dir, int *cols, int *rows)
{
	grid_now(p, cols, rows);
	if (dir > 0) {
		if (*cols < 8)
			(*cols)++;
		else if (*rows < 8)
			(*rows)++;
		else
			return 0;
	} else {
		if (*cols > 1)
			(*cols)--;
		else if (*rows > 1)
			(*rows)--;
		else
			return 0;
	}
	return 1;
}

static void resize(struct panel *p, int dir)
{
	int c, r;
	if (!grid_step(p, dir, &c, &r))
		return;
	char v[16];
	snprintf(v, sizeof v, "%dx%d", c, r);
	fl_conf_set("workspaces", v);
	/* the pager follows with the compositor's answer; until then it shows the old grid */
}

static void act(struct panel *p, int item)
{
	wsmenu_close(p);
	switch (item) {
	case WI_OVERVIEW:
		if (p->shell && p->shell_ver >= 5)
			fl_shell_toggle_overview(p->shell);
		break;
	case WI_ADD:
		resize(p, 1);
		break;
	case WI_REMOVE:
		resize(p, -1);
		break;
	case WI_SETTINGS:
		launch("flsettings --page workspaces", 0);
		break;
	}
}

static int enabled(const struct panel *p, int item)
{
	int c, r;
	switch (item) {
	case WI_OVERVIEW:
		return p->demo || (p->shell && p->shell_ver >= 5);
	case WI_ADD:
		return grid_step(p, 1, &c, &r);
	case WI_REMOVE:
		return grid_step(p, -1, &c, &r);
	}
	return 1;
}

static void move_sel(struct panel *p, int dir)
{
	for (int k = 1; k <= WI_COUNT; k++) {
		int i = ((p->ws_sel + dir * k) % WI_COUNT + WI_COUNT) % WI_COUNT;
		if (enabled(p, i)) {
			p->ws_sel = i;
			return;
		}
	}
}

void wsmenu_draw(fl_ui *ui, void *ud)
{
	struct panel *p = ud;
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, fl_r(0, 0, WSM_W, WSM_H), fl_th.panel);
	fl_fill_rect(cv, fl_r(0, 0, 1, WSM_H), fl_th.line);
	fl_fill_rect(cv, fl_r(WSM_W - 1, 0, 1, WSM_H), fl_th.line);
	fl_fill_rect(cv, fl_r(0, 0, WSM_W, 2), fl_th.accent);

	if (ui->key == XKB_KEY_Escape) {
		ui->key = 0;
		wsmenu_close(p);
		return;
	}
	if (ui->key == XKB_KEY_Down || ui->key == XKB_KEY_Up) {
		move_sel(p, ui->key == XKB_KEY_Down ? 1 : -1);
		ui->key = 0;
	}
	int go = -1;
	int moved = ui->mx != p->ws_mx || ui->my != p->ws_my;
	if (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_KP_Enter || ui->key == XKB_KEY_space) {
		ui->key = 0;
		go = p->ws_sel;
	}

	int c, r;
	for (int i = 0; i < WI_COUNT; i++) {
		static const char *const names[WI_COUNT] = {
			"Workspace overview", "Add workspace", "Remove workspace", "Workspace settings\xe2\x80\xa6",
		};
		char hint[16] = "";
		int on = enabled(p, i);
		if (i == WI_OVERVIEW)
			snprintf(hint, sizeof hint, "Super+Tab");
		else if (i == WI_ADD && grid_step(p, 1, &c, &r))
			snprintf(hint, sizeof hint, "%dx%d", c, r);
		else if (i == WI_REMOVE && grid_step(p, -1, &c, &r))
			snprintf(hint, sizeof hint, "%dx%d", c, r);
		fl_rect rr = fl_r(1, item_y(i), WSM_W - 2, ROW_H);
		int res = fl_widget(ui, ID_BASE + (uint32_t)i, rr, 0);
		if (on && (res & FL_HOVER) && ui->mouse_in && moved)
			p->ws_sel = i; /* the pointer moved here (a key press does not take the highlight back) */
		if (i == p->ws_sel && on) {
			fl_fill_rect(cv, rr, fl_th.raised);
			fl_fill_rect(cv, fl_r(rr.x, rr.y, 3, rr.h), fl_th.accent);
		}
		fl_color fg = !on ? fl_mix(fl_th.panel, fl_th.dim, 140) : i == p->ws_sel ? fl_th.strong : fl_th.text;
		fl_text_in(cv, ui->f_reg, fl_r(rr.x + 16, rr.y, rr.w - 32, rr.h), FL_LEFT, 0, fg, names[i]);
		if (hint[0]) {
			int tw = fl_text_width(p->f_small, hint, -1);
			fl_text(cv, p->f_small, rr.x + rr.w - 14 - tw, rr.y + (rr.h - fl_font_height(p->f_small)) / 2,
			        on ? fl_th.dim : fl_mix(fl_th.panel, fl_th.dim, 140), hint, -1);
		}
		if (on && (res & FL_CLICK))
			go = i;
		if (i == WI_OVERVIEW || i == WI_REMOVE)
			fl_fill_rect(cv, fl_r(12, item_y(i) + ROW_H + SEP_H / 2, WSM_W - 24, 1), fl_th.line);
	}
	p->ws_mx = ui->mx;
	p->ws_my = ui->my;
	if (go >= 0 && enabled(p, go))
		act(p, go);
}

void wsmenu_init(struct panel *p)
{
	fl_win_opts o = {
		.title = "workspaces", .app_id = "flpanel-workspaces", .w = WSM_W, .h = WSM_H, .raw = 1,
		.draw = wsmenu_draw, .ud = p,
	};
	p->wsw = fl_win_new(p->app, &o);
}

int wsmenu_is_open(const struct panel *p)
{
	return p->ws_open;
}

void wsmenu_open(struct panel *p)
{
	if (p->ws_open || !p->wsw || (!p->demo && (!p->shell || p->shell_ver < 5)))
		return;
	p->ws_open = 1;
	p->ws_sel = -1;
	move_sel(p, 1);
	panel_open_flyout(p, FLY_WS, p->wsw, p->ws_cx);
	fl_win_redraw(p->pw);
}

void wsmenu_close(struct panel *p)
{
	if (!p->ws_open)
		return;
	p->ws_open = 0;
	if (p->wsw)
		panel_hide(p->wsw);
	fl_win_redraw(p->pw);
}

void wsmenu_toggle(struct panel *p)
{
	if (p->ws_open)
		wsmenu_close(p);
	else
		wsmenu_open(p);
}
