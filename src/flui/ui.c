/* ui.c -- widgets
 * SPDX-License-Identifier: MIT
 *
 * Immediate mode: each call draws the widget and reports what happened to it
 * in this pass. State that has to survive (focus, the text cursor, the
 * button being pressed) lives in the fl_ui of the window.
 */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

#define KEY_SHIFT_TAB XKB_KEY_ISO_Left_Tab

/* ---- the primitive ---------------------------------------------------------------- */

int fl_widget(fl_ui *ui, uint32_t id, fl_rect r, int flags)
{
	int res = 0;
	fl_rect vis = fl_intersect(r, ui->cv.clip);
	int hover = ui->mouse_in && fl_in(vis, ui->mx, ui->my) && (!ui->active || ui->active == id);

	if (flags & FL_FOCUSABLE) {
		if (!ui->_first_focus)
			ui->_first_focus = id;
		if (ui->_focus_next) { /* Tab on the widget before this one */
			ui->focus = id;
			ui->_focus_next = 0;
		}
		if (ui->focus == id) {
			ui->_focus_seen = 1;
			if (ui->key == XKB_KEY_Tab) {
				ui->_focus_next = 1;
				ui->key = 0;
				ui->_repass = 1;
			} else if (ui->key == KEY_SHIFT_TAB) {
				ui->key = 0;
				ui->_repass = 1;
				if (ui->_prev_focus)
					ui->focus = ui->_prev_focus;
				else
					ui->_focus_prev = 1; /* wraps to the last one */
			}
		}
		ui->_prev_focus = id;
		ui->_last_focus = id;
		if (ui->focus == id)
			res |= FL_FOCUS;
	}
	if (hover)
		res |= FL_HOVER;
	if (hover && (ui->press & FL_BTN_L)) {
		ui->active = id;
		if (flags & FL_FOCUSABLE) {
			ui->focus = id;
			ui->_focus_seen = 1; /* or run_pass drops a focus given to a widget the pass has not met yet */
			ui->_press_taken = 1;
			res |= FL_FOCUS;
		}
	}
	if (hover && (ui->press & FL_BTN_R))
		res |= FL_RCLICK;
	if (ui->active == id) {
		if (ui->btn_held & FL_BTN_L)
			res |= FL_DOWN;
		if (ui->release & FL_BTN_L) {
			if (hover)
				res |= FL_CLICK;
			ui->active = 0;
		}
	}
	return res;
}

static int key_activate(fl_ui *ui, int res)
{
	if ((res & FL_FOCUS) && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_space ||
	                         ui->key == XKB_KEY_KP_Enter)) {
		ui->key = 0;
		return 1;
	}
	return 0;
}

/* ---- label and buttons ------------------------------------------------------------------ */

void fl_label(fl_ui *ui, fl_rect r, const char *s, fl_font *f, fl_color c, int align)
{
	fl_text_in(&ui->cv, f ? f : ui->f_reg, r, align, 0, c, s);
}

int fl_button(fl_ui *ui, uint32_t id, fl_rect r, const char *label, int style)
{
	int res = fl_widget(ui, id, r, FL_FOCUSABLE);
	int hot = res & (FL_HOVER | FL_DOWN);
	fl_color bg = 0, fg = fl_th.text, border = 0;
	fl_font *f = ui->f_reg;
	switch (style) {
	case FL_BTN_ACCENT:
		bg = fl_th.accent;
		if (res & FL_DOWN)
			bg = fl_mix(bg, 0xff000000, 40);
		else if (hot)
			bg = fl_mix(bg, 0xffffffff, 28);
		fg = fl_th.on_accent;
		f = ui->f_semi;
		break;
	case FL_BTN_OUTLINE:
		border = fl_th.line;
		fg = fl_th.text;
		if (hot)
			bg = fl_th.raised;
		break;
	default:
		fg = hot ? fl_th.text : fl_th.dim;
		if (hot)
			bg = fl_th.raised;
		break;
	}
	if (bg)
		fl_fill_rect(&ui->cv, r, bg);
	if (border)
		fl_stroke_rect(&ui->cv, r, 1, border);
	if (res & FL_FOCUS)
		fl_stroke_rect(&ui->cv, r, 1, style == FL_BTN_ACCENT ? fl_th.strong : fl_th.accent);
	fl_text_in(&ui->cv, f, r, FL_CENTER, 0, fg, label);
	return (res & FL_CLICK) || key_activate(ui, res);
}

int fl_icon_button(fl_ui *ui, uint32_t id, fl_rect r, fl_icon ic, int isize, fl_color c)
{
	int res = fl_widget(ui, id, r, FL_FOCUSABLE);
	if (res & (FL_HOVER | FL_DOWN))
		fl_fill_rect(&ui->cv, r, fl_th.raised);
	if (res & FL_FOCUS)
		fl_stroke_rect(&ui->cv, r, 1, fl_th.accent);
	fl_icon_draw(&ui->cv, ic, r.x + (r.w - isize) / 2, r.y + (r.h - isize) / 2, isize, c);
	return (res & FL_CLICK) || key_activate(ui, res);
}

/* ---- text field --------------------------------------------------------------------------- */

static int u8_prev(const char *s, int i)
{
	if (i <= 0)
		return 0;
	i--;
	while (i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80)
		i--;
	return i;
}

static int u8_next(const char *s, int i, int len)
{
	if (i >= len)
		return len;
	i++;
	while (i < len && ((unsigned char)s[i] & 0xc0) == 0x80)
		i++;
	return i;
}

/* byte index in s whose left edge is nearest to x px from the start */
static int index_at(fl_font *f, const char *s, int len, int x)
{
	int i = 0;
	while (i < len) {
		int n = u8_next(s, i, len);
		int w0 = fl_text_width(f, s, i), w1 = fl_text_width(f, s, n);
		if (x < (w0 + w1) / 2)
			return i;
		i = n;
	}
	return len;
}

static void del_range(char *buf, int *len, int a, int b)
{
	memmove(buf + a, buf + b, (size_t)(*len - b) + 1);
	*len -= b - a;
}

int fl_textfield(fl_ui *ui, uint32_t id, fl_rect r, char *buf, size_t cap, const char *ph, fl_font *f)
{
	if (!f)
		f = ui->f_reg;
	int res = fl_widget(ui, id, r, FL_FOCUSABLE | FL_NO_FOCUS_RING);
	int focused = res & FL_FOCUS, ret = 0;
	int len = (int)strlen(buf);
	const int pad = 10;
	if (res & FL_HOVER)
		ui->cursor = FL_CURSOR_IBEAM;

	if (focused && ui->_edit_id != id) {
		ui->_edit_id = id;
		ui->_cur = len;
		ui->_sel = -1;
		ui->_scroll = 0;
	}
	if (!focused && ui->_edit_id == id)
		ui->_edit_id = 0;

	if (focused && ui->_edit_id == id) {
		if (ui->_cur > len)
			ui->_cur = len;
		/* the mouse places the caret; dragging selects */
		if ((res & FL_HOVER) && (ui->press & FL_BTN_L)) {
			ui->_cur = index_at(f, buf, len, ui->mx - (r.x + pad) + ui->_scroll);
			ui->_sel = ui->_cur;
		} else if ((res & FL_DOWN) && !(ui->press & FL_BTN_L)) {
			ui->_cur = index_at(f, buf, len, ui->mx - (r.x + pad) + ui->_scroll);
		}
		if (ui->_sel == ui->_cur && !(res & FL_DOWN))
			ui->_sel = -1;

		int sh = ui->mods & FL_MOD_SHIFT, ctrl = ui->mods & FL_MOD_CTRL;
		int lo = ui->_sel >= 0 && ui->_sel != ui->_cur ? (ui->_sel < ui->_cur ? ui->_sel : ui->_cur) : -1;
		int hi = lo >= 0 ? (ui->_sel < ui->_cur ? ui->_cur : ui->_sel) : -1;
		uint32_t k = ui->key;
		int moved = 0;
		if (k && ctrl && (k == 'a' || k == 'A')) {
			ui->_sel = 0;
			ui->_cur = len;
			k = 0;
		} else if (k && ctrl && (k == 'c' || k == 'C' || k == 'x' || k == 'X')) {
			if (lo >= 0) {
				fl_clipboard_set(fl_win_app(ui->win), buf + lo, (size_t)(hi - lo));
				if (k == 'x' || k == 'X') {
					del_range(buf, &len, lo, hi);
					ui->_cur = lo;
					ui->_sel = -1;
					ret |= FL_EDIT_CHANGED;
				}
			}
			k = 0;
		} else if (k && ctrl && (k == 'v' || k == 'V')) {
			size_t n;
			char *clip = fl_clipboard_get(fl_win_app(ui->win), &n);
			if (clip) {
				if (lo >= 0) {
					del_range(buf, &len, lo, hi);
					ui->_cur = lo;
				}
				for (size_t i = 0; i < n && len + 1 < (int)cap; i++) {
					char ch = clip[i] == '\n' || clip[i] == '\r' || clip[i] == '\t' ? ' ' : clip[i];
					memmove(buf + ui->_cur + 1, buf + ui->_cur, (size_t)(len - ui->_cur) + 1);
					buf[ui->_cur++] = ch;
					len++;
				}
				ui->_sel = -1;
				ret |= FL_EDIT_CHANGED;
				free(clip);
			}
			k = 0;
		}
		if (k && !ctrl) {
			switch (k) {
			case XKB_KEY_Left:
			case XKB_KEY_Right:
			case XKB_KEY_Home:
			case XKB_KEY_End: {
				int to = ui->_cur;
				if (!sh && lo >= 0 && (k == XKB_KEY_Left || k == XKB_KEY_Right)) {
					to = k == XKB_KEY_Left ? lo : hi;
				} else if (k == XKB_KEY_Left) {
					to = u8_prev(buf, ui->_cur);
				} else if (k == XKB_KEY_Right) {
					to = u8_next(buf, ui->_cur, len);
				} else if (k == XKB_KEY_Home) {
					to = 0;
				} else {
					to = len;
				}
				if (sh && ui->_sel < 0)
					ui->_sel = ui->_cur;
				if (!sh)
					ui->_sel = -1;
				ui->_cur = to;
				moved = 1;
				break;
			}
			case XKB_KEY_BackSpace:
				if (lo >= 0) {
					del_range(buf, &len, lo, hi);
					ui->_cur = lo;
				} else if (ui->_cur > 0) {
					int p = u8_prev(buf, ui->_cur);
					del_range(buf, &len, p, ui->_cur);
					ui->_cur = p;
				} else {
					break;
				}
				ui->_sel = -1;
				ret |= FL_EDIT_CHANGED;
				break;
			case XKB_KEY_Delete:
				if (lo >= 0) {
					del_range(buf, &len, lo, hi);
					ui->_cur = lo;
				} else if (ui->_cur < len) {
					del_range(buf, &len, ui->_cur, u8_next(buf, ui->_cur, len));
				} else {
					break;
				}
				ui->_sel = -1;
				ret |= FL_EDIT_CHANGED;
				break;
			case XKB_KEY_Return:
			case XKB_KEY_KP_Enter:
				ret |= FL_EDIT_ENTER;
				ui->key = 0;
				break;
			default:
				break;
			}
			(void)moved;
		}
		if (ui->text[0] && !ctrl) {
			size_t n = strlen(ui->text);
			if (lo >= 0) {
				del_range(buf, &len, lo, hi);
				ui->_cur = lo;
				ui->_sel = -1;
			}
			if ((size_t)len + n < cap) {
				memmove(buf + ui->_cur + n, buf + ui->_cur, (size_t)(len - ui->_cur) + 1);
				memcpy(buf + ui->_cur, ui->text, n);
				ui->_cur += (int)n;
				len += (int)n;
				ret |= FL_EDIT_CHANGED;
			}
			ui->text[0] = 0;
		}
	}

	/* draw */
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, r, fl_th.bg);
	fl_stroke_rect(cv, r, 1, focused ? fl_th.accent : fl_th.line);
	fl_rect in = {r.x + pad, r.y + 1, r.w - 2 * pad, r.h - 2};
	fl_rect save = cv->clip;
	fl_clip(cv, in);
	int fh = fl_font_height(f), ty = r.y + (r.h - fh) / 2;
	if (focused && ui->_edit_id == id) {
		int cw = fl_text_width(f, buf, ui->_cur);
		if (cw - ui->_scroll > in.w - 2)
			ui->_scroll = cw - in.w + 2;
		if (cw < ui->_scroll)
			ui->_scroll = cw;
		if (ui->_scroll < 0)
			ui->_scroll = 0;
	}
	int tx = in.x - (focused ? ui->_scroll : 0);
	if (focused && ui->_sel >= 0 && ui->_sel != ui->_cur) {
		int a = ui->_sel < ui->_cur ? ui->_sel : ui->_cur, b = ui->_sel < ui->_cur ? ui->_cur : ui->_sel;
		int x0 = tx + fl_text_width(f, buf, a), x1 = tx + fl_text_width(f, buf, b);
		fl_fill_rect(cv, (fl_rect){x0, ty - 1, x1 - x0, fh + 2}, fl_th.select);
	}
	if (len)
		fl_text(cv, f, tx, ty, fl_th.text, buf, len);
	else if (ph)
		fl_text(cv, f, in.x, ty, fl_th.dim, ph, -1);
	if (focused)
		fl_fill_rect(cv, (fl_rect){tx + fl_text_width(f, buf, ui->_cur), ty - 1, 1, fh + 2}, fl_th.text);
	cv->clip = save;
	return ret;
}

/* ---- switch, scrollbar ---------------------------------------------------------------------- */

int fl_switch(fl_ui *ui, uint32_t id, fl_rect r, int *on)
{
	int res = fl_widget(ui, id, r, FL_FOCUSABLE);
	int toggled = (res & FL_CLICK) || key_activate(ui, res);
	if (toggled)
		*on = !*on;
	fl_rect t = {r.x, r.y + (r.h - 20) / 2, 40, 20};
	if (*on) {
		fl_fill_rect(&ui->cv, t, fl_th.accent);
		fl_fill_rect(&ui->cv, (fl_rect){t.x + 24, t.y + 4, 12, 12}, fl_th.on_accent);
	} else {
		fl_fill_rect(&ui->cv, t, (res & FL_HOVER) ? fl_th.raised : fl_th.bg);
		fl_stroke_rect(&ui->cv, t, 1, fl_th.line);
		fl_fill_rect(&ui->cv, (fl_rect){t.x + 4, t.y + 4, 12, 12}, fl_th.dim);
	}
	if (res & FL_FOCUS)
		fl_stroke_rect(&ui->cv, (fl_rect){t.x - 2, t.y - 2, t.w + 4, t.h + 4}, 1, fl_th.accent);
	return toggled;
}

int fl_scrollbar(fl_ui *ui, uint32_t id, fl_rect r, int *off, int content, int view)
{
	int max = content - view;
	if (max <= 0 || view <= 0) {
		*off = 0;
		return 0;
	}
	int th = r.h * view / content;
	if (th < 24)
		th = 24;
	if (th > r.h)
		th = r.h;
	int span = r.h - th;
	fl_rect thumb = {r.x, r.y + (span ? span * *off / max : 0), r.w, th};
	int res = fl_widget(ui, id, r, 0);
	int changed = 0;
	if (res & FL_DOWN) {
		if (span > 0) {
			int o = (ui->my - r.y - th / 2) * max / span;
			o = o < 0 ? 0 : o > max ? max : o;
			if (o != *off) {
				*off = o;
				changed = 1;
			}
		}
		thumb.y = r.y + (span ? span * *off / max : 0);
	}
	fl_fill_rect(&ui->cv, thumb, (res & (FL_HOVER | FL_DOWN)) ? fl_th.dim : fl_th.line);
	return changed;
}

/* ---- list ------------------------------------------------------------------------------------ */

int fl_list(fl_ui *ui, uint32_t id, fl_rect r, int count, int item_h, fl_list_state *st,
            fl_list_draw draw, void *ud)
{
	int res = fl_widget(ui, id, r, FL_FOCUSABLE | FL_NO_FOCUS_RING);
	int content = count * item_h, ret = -1;
	int max = content > r.h ? content - r.h : 0;
	if (count <= 0) {
		st->sel = 0;
		st->scroll = 0;
		return -1;
	}
	if (st->sel >= count)
		st->sel = count - 1;
	if (st->sel < 0)
		st->sel = 0;

	if ((res & FL_HOVER) && ui->wheel) {
		st->scroll += ui->wheel * 40;
		ui->wheel = 0;
	}
	int hover_i = -1;
	if ((res & FL_HOVER) && !(ui->active && ui->active != id))
		hover_i = (ui->my - r.y + st->scroll) / item_h;
	if (hover_i >= count)
		hover_i = -1;
	if (hover_i >= 0 && (ui->mx != st->lx || ui->my != st->ly))
		st->sel = hover_i; /* the highlight follows the mouse, and the keys carry on from there */
	st->lx = ui->mx;
	st->ly = ui->my;
	if (res & FL_CLICK && hover_i >= 0) {
		st->sel = hover_i;
		ret = hover_i;
	}

	/* Up/Down work whether or not the list holds the focus, unless a widget took the key */
	if (ui->key && !(ui->mods & FL_MOD_CTRL)) {
		int page = r.h / item_h > 1 ? r.h / item_h - 1 : 1, moved = 1;
		switch (ui->key) {
		case XKB_KEY_Down: st->sel++; break;
		case XKB_KEY_Up: st->sel--; break;
		case XKB_KEY_Page_Down: st->sel += page; break;
		case XKB_KEY_Page_Up: st->sel -= page; break;
		case XKB_KEY_Home: st->sel = 0; break;
		case XKB_KEY_End: st->sel = count - 1; break;
		case XKB_KEY_Return:
		case XKB_KEY_KP_Enter:
			ret = st->sel;
			moved = 0;
			ui->key = 0;
			break;
		default: moved = 0; break;
		}
		if (moved) {
			ui->key = 0;
			if (st->sel < 0)
				st->sel = 0;
			if (st->sel >= count)
				st->sel = count - 1;
			if (st->sel * item_h < st->scroll)
				st->scroll = st->sel * item_h;
			if ((st->sel + 1) * item_h > st->scroll + r.h)
				st->scroll = (st->sel + 1) * item_h - r.h;
			hover_i = -1;
		}
	}
	if (st->scroll > max)
		st->scroll = max;
	if (st->scroll < 0)
		st->scroll = 0;

	fl_rect save = ui->cv.clip;
	fl_clip(&ui->cv, r);
	int first = st->scroll / item_h, last = (st->scroll + r.h) / item_h;
	for (int i = first; i <= last && i < count; i++) {
		fl_rect ir = {r.x, r.y + i * item_h - st->scroll, r.w, item_h};
		if (max)
			ir.w -= 8;
		draw(ui, ir, i, i == hover_i, i == st->sel, ud);
	}
	ui->cv.clip = save;
	if (max) {
		fl_rect sb = {r.x + r.w - 5, r.y, 4, r.h};
		fl_scrollbar(ui, id + 0x10000, sb, &st->scroll, content, r.h);
	}
	return ret;
}

fl_rect fl_grid_cell(const fl_grid *g, int i)
{
	int col = i % g->cols, row = i / g->cols;
	return (fl_rect){g->x + col * (g->cell_w + g->gap), g->y + row * (g->cell_h + g->gap), g->cell_w,
	                 g->cell_h};
}

/* ---- title bar ---------------------------------------------------------------------------------- */

int fl_titlebar(fl_ui *ui, fl_rect r, const char *title, int focused, int maximized)
{
	fl_color fg = focused ? fl_th.text : fl_th.dim;
	fl_fill_rect(&ui->cv, r, fl_th.panel);
	int hit = -1;
	int bh = r.h - 8 < 26 ? r.h - 8 : 26, left = fl_lk.buttons_left; /* compact bars get smaller buttons */
	int bx = left ? r.x + 6 : r.x + r.w - 6 - 90;
	fl_rect btns = {bx, r.y + (r.h - bh) / 2, 90, bh};
	/* buttons at the right: minimise, maximise, close (close is the outer one); at the left the
	 * order mirrors, so close stays at the window's edge */
	static const fl_icon ics[3] = {FL_ICON_MINIMIZE, FL_ICON_MAXIMIZE, FL_ICON_CLOSE};
	for (int i = 0; i < 3; i++) {
		int slot = left ? 2 - i : i;
		fl_rect b = {btns.x + slot * 30, btns.y, 30, bh};
		int res = fl_widget(ui, 0xf1000000u + i, b, 0);
		if (res & (FL_HOVER | FL_DOWN))
			fl_fill_rect(&ui->cv, b, fl_th.raised);
		fl_icon ic = ics[i];
		if (i == 1 && maximized)
			ic = FL_ICON_RESTORE;
		fl_icon_draw(&ui->cv, ic, b.x + 9, b.y + (bh - 12) / 2, 12, fg);
		if (res & FL_CLICK)
			hit = ics[i];
	}
	fl_rect tr = left ? (fl_rect){r.x + 6 + 90 + 8, r.y, r.w - 6 - 90 - 8 - 14, r.h}
	                  : (fl_rect){r.x + 14, r.y, r.w - 14 - 96, r.h};
	char fit[256];
	fl_font *f = focused ? ui->f_med : ui->f_reg;
	fl_text_fit(f, title, tr.w, fit, sizeof fit);
	fl_text_in(&ui->cv, f, tr, FL_LEFT, 0, fg, fit);
	return hit;
}
