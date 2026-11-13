/* ctxmenu.c -- the right-click menus: taskbar buttons, the empty desktop, desktop icons.
 *
 * A small flat popup above the panel at a given x (fl_shell set_flyout, the same as the volume
 * flyout and the pager's menu), so no new protocol is needed. One at a time; a click outside closes
 * it through the flyout_dismissed event (panel_flyouts_close). Keys: Up/Down, Enter or Space, Esc.
 * The labels are copied: the caller may pass strings it builds on the stack.
 * SPDX-License-Identifier: MIT
 */
#include <flui.h>
#include <stdio.h>
#include <string.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "flpanel.h"

#define CTX_W 232
#define CTX_MAX 16
#define ROW_H 26
#define SEP_H 9
#define PAD 6
#define ID_BASE 100

static struct {
	struct panel *p;
	fl_win *w;
	int open, n, sel, mx, my, h;
	char lab[CTX_MAX][64];
	int on[CTX_MAX], code[CTX_MAX], sep[CTX_MAX];
	ctx_pick_cb pick;
	uintptr_t ud;
} C = { .sel = -1 };

static int row_h(int i)
{
	return C.sep[i] ? SEP_H : ROW_H;
}

static int row_y(int i)
{
	int y = PAD;
	for (int k = 0; k < i; k++)
		y += row_h(k);
	return y;
}

static int first_enabled(int from, int dir)
{
	for (int k = 0; k < C.n; k++) {
		int i = ((from + dir * k) % C.n + C.n) % C.n;
		if (C.on[i] && !C.sep[i])
			return i;
	}
	return -1;
}

void ctx_close(struct panel *p)
{
	if (!C.open)
		return;
	C.open = 0;
	if (C.w)
		panel_hide(C.w);
	if (p && p->pw)
		fl_win_redraw(p->pw);
}

static void ctx_draw(fl_ui *ui, void *ud)
{
	(void)ud;
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, fl_r(0, 0, CTX_W, C.h), fl_th.panel);
	fl_fill_rect(cv, fl_r(0, 0, 1, C.h), fl_th.line);
	fl_fill_rect(cv, fl_r(CTX_W - 1, 0, 1, C.h), fl_th.line);
	fl_fill_rect(cv, fl_r(0, 0, CTX_W, 2), fl_th.accent);

	if (ui->key == XKB_KEY_Escape) {
		ui->key = 0;
		ctx_close(C.p);
		return;
	}
	if (ui->key == XKB_KEY_Down || ui->key == XKB_KEY_Up) {
		int next = first_enabled(C.sel + (ui->key == XKB_KEY_Down ? 1 : -1), ui->key == XKB_KEY_Down ? 1 : -1);
		if (next >= 0)
			C.sel = next;
		ui->key = 0;
	}
	int go = -1;
	if (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_KP_Enter || ui->key == XKB_KEY_space) {
		ui->key = 0;
		go = C.sel;
	}
	int moved = ui->mx != C.mx || ui->my != C.my;
	for (int i = 0; i < C.n; i++) {
		int y = row_y(i);
		if (C.sep[i]) {
			fl_fill_rect(cv, fl_r(12, y + SEP_H / 2, CTX_W - 24, 1), fl_th.line);
			continue;
		}
		fl_rect rr = fl_r(1, y, CTX_W - 2, ROW_H);
		int res = fl_widget(ui, ID_BASE + (uint32_t)i, rr, 0);
		if (C.on[i] && (res & FL_HOVER) && ui->mouse_in && moved)
			C.sel = i;	/* the pointer moved here; a key press does not take the highlight back */
		int hot = C.on[i] && i == C.sel;
		if (hot) {
			fl_fill_rect(cv, rr, fl_th.raised);
			fl_fill_rect(cv, fl_r(rr.x, rr.y, 3, rr.h), fl_th.accent);
		}
		fl_color fg = !C.on[i] ? fl_mix(fl_th.panel, fl_th.dim, 140) : hot ? fl_th.strong : fl_th.text;
		fl_text_in(cv, ui->f_reg, fl_r(rr.x + 16, rr.y, rr.w - 32, rr.h), FL_LEFT, 0, fg, C.lab[i]);
		if (C.on[i] && (res & FL_CLICK))
			go = i;
	}
	C.mx = ui->mx;
	C.my = ui->my;
	if (go >= 0 && go < C.n && C.on[go] && !C.sep[go] && C.pick) {
		/* closed first: the choice may open a window of its own */
		int code = C.code[go];
		ctx_pick_cb pick = C.pick;
		uintptr_t ud2 = C.ud;
		struct panel *p = C.p;
		ctx_close(p);
		pick(p, ud2, code);
	}
}

void ctx_open(struct panel *p, int cx, const struct ctx_item *items, int n, ctx_pick_cb pick, uintptr_t ud)
{
	if (!p || !p->shell || n <= 0)
		return;
	if (!C.w) {
		fl_win_opts o = {
			.title = "menu", .app_id = "flpanel-menu", .w = CTX_W, .h = 100, .raw = 1,
			.draw = ctx_draw,
		};
		C.w = fl_win_new(p->app, &o);
		if (!C.w)
			return;
	}
	ctx_close(p);
	C.p = p;
	C.n = n > CTX_MAX ? CTX_MAX : n;
	C.h = PAD * 2;
	for (int i = 0; i < C.n; i++) {
		C.sep[i] = items[i].label == NULL;
		C.on[i] = items[i].on && !C.sep[i];
		C.code[i] = items[i].code;
		snprintf(C.lab[i], sizeof C.lab[i], "%s", items[i].label ? items[i].label : "");
		C.h += row_h(i);
	}
	C.sel = first_enabled(0, 1);
	if (C.sel < 0)
		return;
	C.pick = pick;
	C.ud = ud;
	C.open = 1;
	C.mx = C.my = -1;
	fl_win_resize(C.w, CTX_W, C.h);
	panel_open_flyout(p, FLY_CTX, C.w, cx);
	fl_win_redraw(p->pw);
}
