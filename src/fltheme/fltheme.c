/* fltheme -- Themes: the one place to adjust the look of the desktop
 * SPDX-License-Identifier: MIT
 *
 * Every control writes desktop.conf (the keys are in README) and the running desktop follows at
 * once: libflui apps and the panel watch the file, flcomp reads it for its own colours, the X
 * window frames, the animations and where the panel sits. So the live preview is the desktop
 * itself; the miniature at the top of each page is drawn from the same values and shows what the
 * colour, font, window buttons and panel will look like together.
 *
 * Pages behind a sidebar, like Settings: Presets, Colours, Fonts, Windows, Panel, Animations.
 * Accent colour is the same key and the same code as Settings > General > Appearance (libflui's
 * fl_accents and fl_conf_set): the two places cannot disagree. Corners are square, always: there
 * is no setting for that and the Windows page says so.
 *
 *   fltheme                 the window
 *   fltheme --page NAME     presets|colours|fonts|windows|panel|animations
 *   fltheme --hints         write the GTK settings.ini hints from desktop.conf and exit
 *   fltheme --list          the presets, built in and yours
 *   fltheme --apply NAME    apply a preset (built in or yours) without a window
 *   fltheme --reset         every themed key back to the default
 *   fltheme --save NAME     save the live look as your preset NAME
 */
#include "fltheme.h"

#include <ctype.h>
#include <errno.h>
#include <flui.h>
#include <flpalette.h>
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

enum { PG_PRESETS, PG_COLOURS, PG_FONTS, PG_WINDOWS, PG_PANEL, PG_ANIM, PG_COUNT };

/* widget ids; ranges: presets 100.., user presets 200.., swatches 300.., fonts 400.., choices 500.. */
enum {
	ID_PAGE = 1, ID_SAVE_NAME = 20, ID_SAVE, ID_RESET, ID_HEX, ID_WALL, ID_SIZE, ID_PANEL_H, ID_PANEL_I, ID_SPEED, ID_SCROLL,
	ID_PRESET = 100, ID_UP_LOAD = 200, ID_UP_DEL = 240, ID_ACCENT = 300, ID_FONT = 400, ID_CH = 500,
};
#define SIDEBAR 168
#define ROW 56

struct app {
	fl_app *app;
	fl_win *win;
	fl_font *head, *small, *tiny;
	int page;
	/* presets page */
	struct user_preset user[USER_MAX];
	int nuser, user_scroll;
	char save_name[40];
	char msg[120];
	int msg_err;
	/* colours */
	char hex[16];
	/* sliders: the value while the mouse is down, applied on release */
	uint32_t sl_id;
	int sl_val;
	/* fonts that are really installed */
	const char *fonts[6];
	int nfonts;
};

static struct app A;

/* ---- helpers: the same look as Settings ------------------------------------------------------- */

static void section(fl_ui *ui, int x, int y, int w, const char *name)
{
	fl_text(&ui->cv, A.head, x, y, fl_th.dim, name, -1);
	fl_fill_rect(&ui->cv, fl_r(x, y + 20, w, 1), fl_th.line);
}

static void row_text(fl_ui *ui, int x, int y, int maxw, const char *title, const char *what)
{
	int th = fl_font_height(ui->f_med), sh = fl_font_height(A.small);
	int top = y + (ROW - th - 2 - sh) / 2;
	char fit[160];
	fl_text_fit(ui->f_med, title, maxw, fit, sizeof fit);
	fl_text(&ui->cv, ui->f_med, x, top, fl_th.text, fit, -1);
	fl_text_fit(A.small, what, maxw, fit, sizeof fit);
	fl_text(&ui->cv, A.small, x, top + th + 2, fl_th.dim, fit, -1);
}

static int choice(fl_ui *ui, uint32_t id, fl_rect r, const char *label, int on)
{
	int res = fl_widget(ui, id, r, FL_FOCUSABLE);
	fl_fill_rect(&ui->cv, r, on ? fl_th.select : (res & FL_HOVER) ? fl_th.raised : 0);
	fl_stroke_rect(&ui->cv, r, 1, on || (res & FL_FOCUS) ? fl_th.accent : fl_th.line);
	fl_text_in(&ui->cv, on ? ui->f_med : ui->f_reg, r, FL_CENTER, 0, on ? fl_th.strong : fl_th.text, label);
	if ((res & FL_FOCUS) && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_space)) {
		ui->key = 0;
		return 1;
	}
	return res & FL_CLICK;
}

/* A slider that applies when you let go (or on a key step), so a drag is one change, not twenty.
 * Returns 1 and sets *out when there is a new value. */
static int slider(fl_ui *ui, uint32_t id, fl_rect r, int cur, int lo, int hi, int step, int *out)
{
	int res = fl_widget(ui, id, r, FL_FOCUSABLE);
	int x0 = r.x + 6, x1 = r.x + r.w - 6, v = cur, commit = 0;
	if (A.sl_id == id)
		v = A.sl_val;
	if (res & FL_DOWN) {
		int nv = lo + ((ui->mx - x0) * (hi - lo) + (x1 - x0) / 2) / (x1 - x0);
		nv = lo + (nv - lo + step / 2) / step * step;
		v = nv < lo ? lo : nv > hi ? hi : nv;
		A.sl_id = id;
		A.sl_val = v;
	} else if (A.sl_id == id) {
		A.sl_id = 0;
		if (v != cur) {
			*out = v;
			commit = 1;
		}
	}
	if (res & FL_FOCUS) {
		int k = ui->key, nv = v;
		if (k == XKB_KEY_Left || k == XKB_KEY_Down)
			nv = v - step < lo ? lo : v - step;
		else if (k == XKB_KEY_Right || k == XKB_KEY_Up)
			nv = v + step > hi ? hi : v + step;
		else if (k == XKB_KEY_Home)
			nv = lo;
		else if (k == XKB_KEY_End)
			nv = hi;
		if (nv != v || k == XKB_KEY_Home || k == XKB_KEY_End) {
			ui->key = 0;
			v = nv;
			if (v != cur) {
				*out = v;
				commit = 1;
			}
		}
	}
	int tx = x0 + (v - lo) * (x1 - x0) / (hi - lo), ty = r.y + r.h / 2 - 2;
	fl_fill_rect(&ui->cv, fl_r(x0, ty, x1 - x0, 4), fl_th.line);
	fl_fill_rect(&ui->cv, fl_r(x0, ty, tx - x0, 4), fl_th.accent);
	fl_rect th = fl_r(tx - 5, r.y + r.h / 2 - 10, 10, 20);
	fl_fill_rect(&ui->cv, th, (res & FL_DOWN) ? fl_th.accent : fl_th.text);
	if (res & FL_FOCUS)
		fl_stroke_rect(&ui->cv, fl_inset(th, -3, -3), 1, fl_th.accent);
	return commit;
}

static void set_one(const char *key, const char *val)
{
	const char *p[2] = {key, val};
	fl_conf_set_many(p, 1);
	hints_write();
}

/* a value equal to the default is removed instead of written: desktop.conf stays short */
static void set_or_default(const char *key, const char *val)
{
	const char *d = theme_default(key);
	set_one(key, d && !strcmp(d, val) ? NULL : val);
}

static void say(int err, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(int err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(A.msg, sizeof A.msg, fmt, ap);
	va_end(ap);
	A.msg_err = err;
}

/* ---- the preview: a miniature of the desktop from the live values ------------------------------ */

static void mini_window(fl_ui *ui, fl_rect w, int focused, const char *title, int body)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, w, fl_th.panel);
	fl_stroke_rect(cv, w, 1, fl_th.line);
	fl_fill_rect(cv, fl_r(w.x, w.y, w.w, focused ? 2 : 1), focused ? fl_th.accent : fl_th.line);
	int tb = fl_lk.title_h * 6 / 10 + 2;
	fl_rect bar = fl_r(w.x + 1, w.y + 2, w.w - 2, tb);
	fl_color fg = focused ? fl_th.text : fl_th.dim;
	/* the three buttons, on the side the setting says; close is the outer one */
	static const fl_icon ics[3] = {FL_ICON_MINIMIZE, FL_ICON_MAXIMIZE, FL_ICON_CLOSE};
	int bs = tb - 2 < 14 ? tb - 2 : 14;
	for (int i = 0; i < 3; i++) {
		int slot = fl_lk.buttons_left ? 2 - i : i;
		int bx = fl_lk.buttons_left ? bar.x + 4 + slot * (bs + 2) : bar.x + bar.w - 4 - (3 - slot) * (bs + 2) + 2;
		fl_icon_draw(cv, ics[i], bx + (bs - 8) / 2, bar.y + (bar.h - 8) / 2, 8, fg);
	}
	int tx = fl_lk.buttons_left ? bar.x + 4 + 3 * (bs + 2) + 6 : bar.x + 8;
	fl_text_in(cv, A.tiny, fl_r(tx, bar.y, bar.w - 3 * (bs + 2) - 22, bar.h), FL_LEFT, 0, fg, title);
	fl_fill_rect(cv, fl_r(w.x + 1, bar.y + bar.h, w.w - 2, 1), fl_th.line);
	if (!body)
		return;
	int y = bar.y + bar.h + 10, x = w.x + 12;
	fl_text(cv, ui->f_med, x, y, fl_th.strong, "Sample text", -1);
	y += fl_font_height(ui->f_med) + 2;
	fl_text(cv, ui->f_reg, x, y, fl_th.dim, "Quick brown fox 0123", -1);
	y += fl_font_height(ui->f_reg) + 10;
	fl_rect b = fl_r(x, y, 64, 24);
	if (y + 24 < w.y + w.h - 4) {
		fl_fill_rect(cv, b, fl_th.accent);
		fl_text_in(cv, ui->f_reg, b, FL_CENTER, 0, fl_th.on_accent, "Accent");
		fl_rect o = fl_r(x + 72, y, 64, 24);
		fl_stroke_rect(cv, o, 1, fl_th.line);
		fl_text_in(cv, ui->f_reg, o, FL_CENTER, 0, fl_th.text, "Plain");
		fl_rect sel = fl_r(x + 144, y, 80, 24);
		fl_fill_rect(cv, sel, fl_th.select);
		fl_text_in(cv, ui->f_reg, sel, FL_CENTER, 0, fl_th.text, "Selected");
	}
}

static void draw_preview(fl_ui *ui, fl_rect r)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, r, fl_th.bg);
	fl_stroke_rect(cv, r, 1, fl_th.line);
	int ph = fl_lk.panel_h * 6 / 10;
	fl_rect pan = fl_lk.panel_top ? fl_r(r.x + 1, r.y + 1, r.w - 2, ph) : fl_r(r.x + 1, r.y + r.h - 1 - ph, r.w - 2, ph);
	fl_rect area = fl_lk.panel_top ? fl_r(r.x + 1, r.y + 1 + ph, r.w - 2, r.h - 2 - ph) : fl_r(r.x + 1, r.y + 1, r.w - 2, r.h - 2 - ph);
	fl_clip(cv, r);
	fl_rect back = fl_r(area.x + area.w - 270, area.y + 12, 240, area.h - 34);
	mini_window(ui, back, 0, "Files", 0);
	fl_rect front = fl_r(area.x + 26, area.y + 20, 330, area.h - 34);
	if (front.h < 90)
		front.h = 90;
	mini_window(ui, front, 1, "Themes", 1);
	/* the panel */
	fl_fill_rect(cv, pan, fl_th.panel);
	fl_fill_rect(cv, fl_r(pan.x, fl_lk.panel_top ? pan.y + pan.h - 1 : pan.y, pan.w, 1), fl_th.line);
	fl_rect st = fl_r(pan.x, pan.y, ph + 10, pan.h);
	fl_fill_rect(cv, st, fl_th.accent);
	int is = fl_lk.panel_icon * 6 / 10 + 2;
	fl_icon_draw(cv, FL_ICON_MENU, st.x + (st.w - is) / 2, st.y + (st.h - is) / 2, is, fl_th.on_accent);
	static const fl_icon ents[3] = {FL_ICON_FOLDER, FL_ICON_TERMINAL, FL_ICON_SETTINGS};
	int ex = st.x + st.w + 6, ew = is + 18;
	for (int i = 0; i < 3; i++) {
		fl_rect e = fl_r(ex + i * (ew + 2), pan.y + (fl_lk.panel_top ? 0 : 1), ew, pan.h - 1);
		if (i == 0) {
			fl_fill_rect(cv, e, fl_th.raised);
			fl_fill_rect(cv, fl_r(e.x, e.y + e.h - 2, e.w, 2), fl_th.accent);
		}
		fl_icon_draw(cv, ents[i], e.x + (e.w - is) / 2, e.y + (e.h - 2 - is) / 2, is, i == 0 ? fl_th.text : fl_th.dim);
	}
	fl_text_in(cv, A.tiny, fl_r(pan.x + pan.w - 60, pan.y, 52, pan.h), FL_RIGHT, 0, fl_th.text, "12:34");
	ui->cv.clip = fl_r(0, 0, ui->cv.w, ui->cv.h);
}

#define PREVIEW_H 176

/* ---- pages -------------------------------------------------------------------------------------- */

static void refresh_user(void)
{
	A.nuser = user_list(A.user, USER_MAX);
	if (A.user_scroll > A.nuser - 3)
		A.user_scroll = A.nuser > 3 ? A.nuser - 3 : 0;
}

/* a small picture of what a preset looks like: its colours and bars */
static void preset_thumb(fl_ui *ui, fl_rect r, const struct preset *p)
{
	int light = !strcmp(fl_conf_get("theme", "dark"), "light");
	for (int i = 0; i < p->n; i++)
		if (!strcmp(p->kv[i][0], "theme"))
			light = p->kv[i][1] && !strcmp(p->kv[i][1], "light");
	struct flpal pal;
	flpal_make(&pal, !light, fl_conf_get("accent", NULL));
	int compact = 0, big = 0;
	for (int i = 0; i < p->n; i++) {
		if (!strcmp(p->kv[i][0], "titlebar"))
			compact = p->kv[i][1] && !strcmp(p->kv[i][1], "compact");
		if (!strcmp(p->kv[i][0], "font_size"))
			big = p->kv[i][1] && atoi(p->kv[i][1]) > 13;
	}
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, r, pal.bg);
	fl_rect w = fl_r(r.x + 6, r.y + 6, r.w - 20, r.h - 12 - 6);
	fl_fill_rect(cv, w, pal.panel);
	fl_stroke_rect(cv, w, 1, pal.line);
	fl_fill_rect(cv, fl_r(w.x, w.y, w.w, 2), pal.accent);
	fl_fill_rect(cv, fl_r(w.x + 6, w.y + 6 + (compact ? 0 : 2), w.w / 3, 3 + big), pal.text);
	fl_fill_rect(cv, fl_r(w.x + 6, w.y + 14 + (compact ? 0 : 2) + big * 2, w.w / 2, 2), pal.dim);
	fl_fill_rect(cv, fl_r(r.x, r.y + r.h - (compact ? 5 : big ? 9 : 7), r.w, compact ? 5 : big ? 9 : 7), pal.panel);
	fl_fill_rect(cv, fl_r(r.x, r.y + r.h - (compact ? 5 : big ? 9 : 7), 10, compact ? 5 : big ? 9 : 7), pal.accent);
}

static void page_presets(fl_ui *ui, fl_rect c)
{
	int x = c.x + 28, w = c.w - 56, y = c.y + 20;
	draw_preview(ui, fl_r(x, y, w, PREVIEW_H));
	y += PREVIEW_H + 18;
	section(ui, x, y, w, "Presets");
	y += 30;
	int tw = (w - 10) / 2, th = 68;
	for (int i = 0; i < n_builtin; i++) {
		const struct preset *p = &builtin_presets[i];
		fl_rect r = fl_r(x + (i % 2) * (tw + 10), y + (i / 2) * (th + 8), tw, th);
		int on = preset_matches(p);
		int res = fl_widget(ui, ID_PRESET + (uint32_t)i, r, FL_FOCUSABLE);
		fl_fill_rect(&ui->cv, r, on ? fl_th.select : (res & FL_HOVER) ? fl_th.raised : 0);
		fl_stroke_rect(&ui->cv, r, 1, on || (res & FL_FOCUS) ? fl_th.accent : fl_th.line);
		preset_thumb(ui, fl_r(r.x + 1, r.y + 1, 96, r.h - 2), p);
		char fit[96];
		fl_text_fit(on ? ui->f_med : ui->f_reg, p->name, r.w - 96 - 22, fit, sizeof fit);
		fl_text(&ui->cv, on ? ui->f_med : ui->f_reg, r.x + 108, r.y + 10, on ? fl_th.strong : fl_th.text, fit, -1);
		/* the blurb on two lines at most */
		const char *b = p->blurb;
		int lw = r.w - 96 - 22, ly = r.y + 10 + fl_font_height(ui->f_med) + 2;
		for (int line = 0; line < 2 && *b; line++) {
			int n = (int)strlen(b), fitn = n;
			while (fitn > 0 && fl_text_width(A.small, b, fitn) > lw)
				fitn--;
			if (fitn < n) {	/* break at the last space that fits */
				int k = fitn;
				while (k > 0 && b[k] != ' ')
					k--;
				if (k > 0)
					fitn = k;
			}
			fl_text(&ui->cv, A.small, r.x + 108, ly, fl_th.dim, b, fitn);
			ly += fl_font_height(A.small) + 1;
			b += fitn;
			while (*b == ' ')
				b++;
		}
		int pick = res & FL_CLICK;
		if ((res & FL_FOCUS) && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_space)) {
			ui->key = 0;
			pick = 1;
		}
		if (pick) {
			preset_apply(p);
			say(0, "Applied %s", p->name);
		}
	}
	y += ((n_builtin + 1) / 2) * (th + 8) + 8;

	section(ui, x, y, w, "My presets");
	y += 30;
	int sw = 84, bw = 84;
	fl_rect nf = fl_r(x, y, w - (sw + 20) - bw - 20, 32);
	int e = fl_textfield(ui, ID_SAVE_NAME, nf, A.save_name, sizeof A.save_name, "Name for the current look", ui->f_reg);
	fl_rect sv = fl_r(nf.x + nf.w + 10, y, sw + 20, 32);
	int ok = name_ok(A.save_name);
	int saved = 0;
	if ((fl_button(ui, ID_SAVE, sv, "Save current", ok ? FL_BTN_ACCENT : FL_BTN_OUTLINE) || (e & FL_EDIT_ENTER)) && ok)
		saved = 1;
	fl_rect rs = fl_r(sv.x + sv.w + 10, y, bw, 32);
	if (fl_button(ui, ID_RESET, rs, "Reset", FL_BTN_OUTLINE)) {
		theme_reset();
		say(0, "Back to the defaults");
	}
	if (saved) {
		if (user_save(A.save_name) == 0) {
			say(0, "Saved %s", A.save_name);
			A.save_name[0] = 0;
			refresh_user();
		} else {
			say(1, "Could not save: %s", strerror(errno));
		}
	}
	y += 40;
	if (A.msg[0])
		fl_text(&ui->cv, A.small, x, y, A.msg_err ? fl_th.err : fl_th.dim, A.msg, -1);
	else if (!ok && A.save_name[0])
		fl_text(&ui->cv, A.small, x, y, fl_th.dim, "Letters, digits, space, - and _ (up to 32)", -1);
	y += 20;
	/* the list: three rows, the wheel scrolls it */
	fl_rect lr = fl_r(x, y, w, 3 * 34);
	if (!A.nuser) {
		fl_text(&ui->cv, A.small, x, y + 6, fl_th.dim, "Nothing saved yet. Presets are small files in ~/.config/freelfs/themes.", -1);
	} else {
		if (ui->wheel && fl_in(lr, ui->mx, ui->my)) {
			A.user_scroll += ui->wheel > 0 ? 1 : -1;
			ui->wheel = 0;
			if (A.user_scroll < 0)
				A.user_scroll = 0;
			if (A.user_scroll > A.nuser - 3)
				A.user_scroll = A.nuser > 3 ? A.nuser - 3 : 0;
		}
		for (int i = 0; i < 3 && A.user_scroll + i < A.nuser; i++) {
			int k = A.user_scroll + i;
			fl_rect r = fl_r(x, y + i * 34, w - (A.nuser > 3 ? 14 : 0), 32);
			fl_fill_rect(&ui->cv, r, fl_th.bg);
			fl_stroke_rect(&ui->cv, r, 1, fl_th.line);
			fl_text_in(&ui->cv, ui->f_reg, fl_r(r.x + 12, r.y, r.w - 200, r.h), FL_LEFT, 0, fl_th.text, A.user[k].name);
			if (fl_button(ui, ID_UP_LOAD + (uint32_t)k, fl_r(r.x + r.w - 160, r.y + 3, 72, 26), "Apply", FL_BTN_OUTLINE)) {
				if (user_load(A.user[k].name) == 0)
					say(0, "Applied %s", A.user[k].name);
				else
					say(1, "%s is not a theme file", A.user[k].name);
			}
			if (fl_button(ui, ID_UP_DEL + (uint32_t)k, fl_r(r.x + r.w - 80, r.y + 3, 72, 26), "Delete", FL_BTN_OUTLINE)) {
				user_delete(A.user[k].name);
				say(0, "Deleted %s", A.user[k].name);
				refresh_user();
				break;
			}
		}
		if (A.nuser > 3) {
			int off = A.user_scroll * 34;
			fl_scrollbar(ui, ID_SCROLL, fl_r(x + w - 10, y, 10, 3 * 34 - 2), &off, A.nuser * 34, 3 * 34);
			A.user_scroll = off / 34;
		}
	}
}

static void page_colours(fl_ui *ui, fl_rect c)
{
	int x = c.x + 28, w = c.w - 56, y = c.y + 20;
	draw_preview(ui, fl_r(x, y, w, PREVIEW_H));
	y += PREVIEW_H + 18;
	section(ui, x, y, w, "Colour scheme");
	y += 30;
	row_text(ui, x, y, w - 190, "Scheme", "Dark is the FreeLFS look; Light is flat and sober");
	int dark = fl_th.dark;
	fl_rect d = fl_r(x + w - 176, y + 12, 86, 32), l = fl_r(x + w - 86, y + 12, 86, 32);
	if (choice(ui, ID_CH + 0, d, "Dark", dark) && !dark)
		set_or_default("theme", "dark");
	if (choice(ui, ID_CH + 1, l, "Light", !dark) && dark)
		set_or_default("theme", "light");
	y += ROW + 8;

	/* the same swatches and the same key as Settings > General > Appearance */
	const char *cur = fl_conf_get("accent", "green");
	int sel = fl_accent_find(cur), sz = 28, gap = 10;
	char sub[96];
	snprintf(sub, sizeof sub, "Buttons, selections and highlights: %s", sel >= 0 ? fl_accents[sel].name : "custom");
	row_text(ui, x, y, w - (FL_ACCENT_COUNT * (sz + gap) + 20), "Accent colour", sub);
	for (int i = 0; i < FL_ACCENT_COUNT; i++) {
		fl_rect r = fl_r(x + w - (FL_ACCENT_COUNT - i) * (sz + gap) + gap, y + (ROW - sz) / 2, sz, sz);
		int res = fl_widget(ui, ID_ACCENT + (uint32_t)i, r, FL_FOCUSABLE | FL_NO_FOCUS_RING);
		fl_fill_rect(&ui->cv, r, fl_th.dark ? fl_accents[i].dark : fl_accents[i].light);
		if (i == sel)
			fl_stroke_rect(&ui->cv, fl_inset(r, -4, -4), 2, fl_th.strong);
		else if (res & (FL_HOVER | FL_FOCUS))
			fl_stroke_rect(&ui->cv, fl_inset(r, -3, -3), 1, (res & FL_FOCUS) ? fl_th.strong : fl_th.dim);
		int pick = res & FL_CLICK;
		if ((res & FL_FOCUS) && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_space)) {
			ui->key = 0;
			pick = 1;
		}
		if (pick && i != sel)
			set_or_default("accent", fl_accents[i].id);
	}
	y += ROW + 8;
	row_text(ui, x, y, w - 250, "Custom accent", "Six hex digits, then Enter (for example 8a5fd0)");
	fl_rect hf = fl_r(x + w - 230, y + 12, 130, 32);
	if (!A.hex[0] && sel < 0)
		snprintf(A.hex, sizeof A.hex, "%s", cur);
	int e = fl_textfield(ui, ID_HEX, hf, A.hex, sizeof A.hex, "RRGGBB", ui->f_reg);
	fl_color probe;
	uint32_t pv;
	int valid = flpal_parse_hex(A.hex, &pv) == 0;
	probe = valid ? flpal_accent(A.hex, fl_th.dark) : fl_th.line;
	fl_fill_rect(&ui->cv, fl_r(hf.x + hf.w + 8, hf.y, 32, 32), probe);
	if (fl_button(ui, ID_HEX + 1, fl_r(hf.x + hf.w + 48, hf.y, 52, 32), "Set", valid ? FL_BTN_ACCENT : FL_BTN_OUTLINE) || (e & FL_EDIT_ENTER)) {
		if (valid) {
			char norm[16];
			snprintf(norm, sizeof norm, "%06x", pv & 0xffffff);
			set_one("accent", norm);
		}
	}
	y += ROW + 8;
	row_text(ui, x, y, w - 190, "Wallpaper", "Pictures and the 3D F in your accent colour");
	if (fl_button(ui, ID_WALL, fl_r(x + w - 176, y + 12, 176, 32), "Change in Settings", FL_BTN_OUTLINE)) {
		pid_t p = fork();
		if (p == 0) {
			setsid();
			execlp("flsettings", "flsettings", "--page", "display", (char *)NULL);
			_exit(127);
		}
	}
}

static void page_fonts(fl_ui *ui, fl_rect c)
{
	int x = c.x + 28, w = c.w - 56, y = c.y + 20;
	draw_preview(ui, fl_r(x, y, w, PREVIEW_H));
	y += PREVIEW_H + 18;
	section(ui, x, y, w, "Interface font");
	y += 30;
	row_text(ui, x, y, w, "Family", "Used by every FreeLFS program; the terminal keeps its own monospace font");
	y += ROW + 2;
	const char *cur = fl_conf_get("font", "IBM Plex Sans");
	int cw = (w - 3 * 8) / 4;
	for (int i = 0; i < A.nfonts; i++) {
		fl_rect r = fl_r(x + (i % 4) * (cw + 8), y + (i / 4) * 40, cw, 32);
		if (choice(ui, ID_FONT + (uint32_t)i, r, A.fonts[i], !strcasecmp(cur, A.fonts[i])))
			set_or_default("font", A.fonts[i]);
	}
	y += ((A.nfonts + 3) / 4) * 40 + 12;
	int size = (int)fl_lk.font_px;
	char what[64];
	snprintf(what, sizeof what, "%d px%s; 13 is the design size", size, size == 13 ? " (default)" : "");
	row_text(ui, x, y, w - 300, "Text size", what);
	int nv;
	if (slider(ui, ID_SIZE, fl_r(x + w - 280, y + 8, 280, 40), size, 11, 16, 1, &nv))
		set_or_default("font_size", (snprintf(what, sizeof what, "%d", nv), what));
}

static void page_windows(fl_ui *ui, fl_rect c)
{
	int x = c.x + 28, w = c.w - 56, y = c.y + 20;
	draw_preview(ui, fl_r(x, y, w, PREVIEW_H));
	y += PREVIEW_H + 18;
	section(ui, x, y, w, "Window controls");
	y += 30;
	row_text(ui, x, y, w - 190, "Buttons", "Minimise, maximise and close at the right or the left of the title bar");
	int left = fl_lk.buttons_left;
	if (choice(ui, ID_CH + 10, fl_r(x + w - 176, y + 12, 86, 32), "Right", !left) && left)
		set_or_default("buttons", "right");
	if (choice(ui, ID_CH + 11, fl_r(x + w - 86, y + 12, 86, 32), "Left", left) && !left)
		set_or_default("buttons", "left");
	y += ROW + 8;
	row_text(ui, x, y, w - 190, "Title bar", "Normal is 34 px high, compact 28 px");
	int comp = fl_lk.title_h < 34;
	if (choice(ui, ID_CH + 12, fl_r(x + w - 176, y + 12, 86, 32), "Normal", !comp) && comp)
		set_or_default("titlebar", "normal");
	if (choice(ui, ID_CH + 13, fl_r(x + w - 86, y + 12, 86, 32), "Compact", comp) && !comp)
		set_or_default("titlebar", "compact");
	y += ROW + 8;
	row_text(ui, x, y, w - 120, "Corners", "Square. It is the FreeLFS look, so there is nothing to set");
	fl_rect sq = fl_r(x + w - 34, y + (ROW - 22) / 2, 22, 22);
	fl_stroke_rect(&ui->cv, sq, 2, fl_th.dim);
}

static void page_panel(fl_ui *ui, fl_rect c)
{
	int x = c.x + 28, w = c.w - 56, y = c.y + 20;
	draw_preview(ui, fl_r(x, y, w, PREVIEW_H));
	y += PREVIEW_H + 18;
	section(ui, x, y, w, "Panel");
	y += 30;
	row_text(ui, x, y, w - 190, "Position", "The panel and the start menu at the bottom or the top edge");
	int top = fl_lk.panel_top;
	if (choice(ui, ID_CH + 20, fl_r(x + w - 176, y + 12, 86, 32), "Bottom", !top) && top)
		set_or_default("panel_position", "bottom");
	if (choice(ui, ID_CH + 21, fl_r(x + w - 86, y + 12, 86, 32), "Top", top) && !top)
		set_or_default("panel_position", "top");
	y += ROW + 8;
	char what[64];
	snprintf(what, sizeof what, "%d px%s", fl_lk.panel_h, fl_lk.panel_h == 44 ? " (default)" : "");
	row_text(ui, x, y, w - 300, "Height", what);
	int nv;
	if (slider(ui, ID_PANEL_H, fl_r(x + w - 280, y + 8, 280, 40), fl_lk.panel_h, 36, 60, 2, &nv))
		set_or_default("panel_height", (snprintf(what, sizeof what, "%d", nv), what));
	y += ROW + 8;
	snprintf(what, sizeof what, "%d px%s", fl_lk.panel_icon, fl_lk.panel_icon == 20 ? " (default)" : "");
	row_text(ui, x, y, w - 300, "Icon size", what);
	if (slider(ui, ID_PANEL_I, fl_r(x + w - 280, y + 8, 280, 40), fl_lk.panel_icon, 16, 28, 2, &nv))
		set_or_default("panel_icon_size", (snprintf(what, sizeof what, "%d", nv), what));
}

static void page_anim(fl_ui *ui, fl_rect c)
{
	int x = c.x + 28, w = c.w - 56, y = c.y + 20;
	draw_preview(ui, fl_r(x, y, w, PREVIEW_H));
	y += PREVIEW_H + 18;
	section(ui, x, y, w, "Animations");
	y += 30;
	row_text(ui, x, y, w - 300, "Motion", "Reduced uses fades only; Off changes everything at once");
	int m = fl_lk.anim;
	int cw = 90;
	if (choice(ui, ID_CH + 30, fl_r(x + w - 3 * cw - 10, y + 12, cw, 32), "On", m == 2) && m != 2)
		set_or_default("animations", "on");
	if (choice(ui, ID_CH + 31, fl_r(x + w - 2 * cw - 5, y + 12, cw, 32), "Reduced", m == 1) && m != 1)
		set_or_default("animations", "reduced");
	if (choice(ui, ID_CH + 32, fl_r(x + w - cw, y + 12, cw, 32), "Off", m == 0) && m != 0)
		set_or_default("animations", "off");
	y += ROW + 8;
	int pct = (int)(fl_lk.anim_speed * 100 + 0.5);
	char what[64];
	snprintf(what, sizeof what, "%d.%02dx%s", pct / 100, pct % 100, pct == 100 ? " (default)" : "");
	row_text(ui, x, y, w - 300, "Speed", m ? what : "Animations are off");
	int nv;
	if (m && slider(ui, ID_SPEED, fl_r(x + w - 280, y + 8, 280, 40), pct, 50, 200, 10, &nv)) {
		snprintf(what, sizeof what, "%d.%d", nv / 100, nv % 100 / 10);
		if (nv % 10)
			snprintf(what, sizeof what, "%d.%02d", nv / 100, nv % 100);
		set_or_default("animation_speed", what);
	}
	y += ROW + 16;
	fl_text(&ui->cv, A.small, x, y, fl_th.dim, "Windows open, close, minimise and snap; workspaces slide; the overview zooms; menus fade.", -1);
	fl_text(&ui->cv, A.small, x, y + 18, fl_th.dim, "Nothing runs while the desktop is idle.", -1);
}

/* ---- window ------------------------------------------------------------------------------------ */

static int sidebar_item(fl_ui *ui, uint32_t id, fl_rect r, fl_icon ic, const char *label, int on)
{
	int res = fl_widget(ui, id, r, FL_FOCUSABLE);
	fl_fill_rect(&ui->cv, r, on ? fl_th.select : (res & FL_HOVER) ? fl_th.raised : 0);
	if (on)
		fl_fill_rect(&ui->cv, fl_r(r.x, r.y, 3, r.h), fl_th.accent);
	if (res & FL_FOCUS)
		fl_stroke_rect(&ui->cv, r, 1, fl_th.accent);
	fl_icon_draw(&ui->cv, ic, r.x + 16, r.y + (r.h - 16) / 2, 16, on ? fl_th.accent : fl_th.dim);
	fl_text_in(&ui->cv, on ? ui->f_med : ui->f_reg, fl_r(r.x + 44, r.y, r.w - 44, r.h), FL_LEFT, 0,
	           on ? fl_th.strong : fl_th.text, label);
	if ((res & FL_FOCUS) && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_space)) {
		ui->key = 0;
		return 1;
	}
	return res & FL_CLICK;
}

static void draw(fl_ui *ui, void *ud)
{
	(void)ud;
	fl_rect c = fl_win_content(ui->win);
	fl_fill_rect(&ui->cv, c, fl_th.panel);
	fl_rect side = fl_cut_left(&c, SIDEBAR);
	fl_fill_rect(&ui->cv, side, fl_th.bg);
	fl_fill_rect(&ui->cv, fl_r(side.x + side.w - 1, side.y, 1, side.h), fl_th.line);
	static const struct { fl_icon icon; const char *name; } items[PG_COUNT] = {
		{FL_ICON_SETTINGS, "Presets"}, {FL_ICON_DISPLAY, "Colours"}, {FL_ICON_DOC, "Fonts"},
		{FL_ICON_WINDOW, "Windows"}, {FL_ICON_MENU, "Panel"}, {FL_ICON_CLOCK, "Animations"},
	};
	for (int i = 0; i < PG_COUNT; i++)
		if (sidebar_item(ui, ID_PAGE + (uint32_t)i, fl_r(side.x, side.y + 12 + i * 40, side.w - 1, 40), items[i].icon,
		                 items[i].name, A.page == i)) {
			A.page = i;
			A.msg[0] = 0;
		}
	switch (A.page) {
	case PG_COLOURS: page_colours(ui, c); break;
	case PG_FONTS: page_fonts(ui, c); break;
	case PG_WINDOWS: page_windows(ui, c); break;
	case PG_PANEL: page_panel(ui, c); break;
	case PG_ANIM: page_anim(ui, c); break;
	default: page_presets(ui, c);
	}
}

static const char *const font_candidates[] = {"IBM Plex Sans", "IBM Plex Mono", "DejaVu Sans", "DejaVu Sans Mono", NULL};

static int page_by_name(const char *n)
{
	static const char *const names[PG_COUNT] = {"presets", "colours", "fonts", "windows", "panel", "animations"};
	for (int i = 0; i < PG_COUNT; i++)
		if (!strcasecmp(n, names[i]))
			return i;
	if (!strcasecmp(n, "colors") || !strcasecmp(n, "accent") || !strcasecmp(n, "wallpaper"))
		return PG_COLOURS;
	if (!strcasecmp(n, "animation"))
		return PG_ANIM;
	return PG_PRESETS;
}

static int cli_apply(const char *name)
{
	for (int i = 0; i < n_builtin; i++)
		if (!strcasecmp(name, builtin_presets[i].name))
			return preset_apply(&builtin_presets[i]) == 0 ? 0 : 1;
	if (user_load(name) == 0)
		return 0;
	fprintf(stderr, "fltheme: no preset called %s (see fltheme --list)\n", name);
	return 1;
}

int main(int argc, char **argv)
{
	int page = PG_PRESETS;
	fl_conf_load();
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--page") && i + 1 < argc) {
			page = page_by_name(argv[++i]);
		} else if (!strcmp(argv[i], "--hints")) {
			return hints_write() == 0 ? 0 : 1;
		} else if (!strcmp(argv[i], "--list")) {
			for (int k = 0; k < n_builtin; k++)
				printf("%s%s\n", builtin_presets[k].name, preset_matches(&builtin_presets[k]) ? " *" : "");
			struct user_preset up[USER_MAX];
			int n = user_list(up, USER_MAX);
			for (int k = 0; k < n; k++)
				printf("%s (yours)\n", up[k].name);
			return 0;
		} else if (!strcmp(argv[i], "--apply") && i + 1 < argc) {
			return cli_apply(argv[i + 1]);
		} else if (!strcmp(argv[i], "--reset")) {
			return theme_reset() == 0 ? 0 : 1;
		} else if (!strcmp(argv[i], "--save") && i + 1 < argc) {
			if (user_save(argv[i + 1]) < 0) {
				fprintf(stderr, "fltheme: cannot save %s\n", argv[i + 1]);
				return 1;
			}
			return 0;
		} else {
			fprintf(stderr, "usage: fltheme [--page presets|colours|fonts|windows|panel|animations]\n"
			                "       fltheme --hints | --list | --reset | --apply NAME | --save NAME\n"
			                "Colour scheme, accent, fonts, window buttons, panel, animations, presets\n");
			return !strcmp(argv[i], "-h") || !strcmp(argv[i], "--help") ? 0 : 2;
		}
	}
	A.page = page;
	A.app = fl_app_new(NULL, NULL);
	if (!A.app) {
		fprintf(stderr, "fltheme: cannot connect to a Wayland compositor\n");
		return 1;
	}
	A.head = fl_font_ui(500, 11);
	A.small = fl_font_ui(400, 12);
	A.tiny = fl_font_ui(500, 10);
	for (int i = 0; font_candidates[i]; i++)
		if (fl_font_family_exists(font_candidates[i]))
			A.fonts[A.nfonts++] = font_candidates[i];
	refresh_user();
	fl_win_opts o = {
		.title = "Themes", .app_id = "fltheme", .w = 800, .h = 690, .min_w = 700, .min_h = 600,
		.decorated = 1, .resizable = 1, .draw = draw,
	};
	const char *sz = getenv("FLTHEME_SIZE"); /* WxH, for screenshots */
	if (sz && getenv("FLUI_PPM"))
		sscanf(sz, "%dx%d", &o.w, &o.h);
	const char *pre = getenv("FLTHEME_TEST_SAVE"); /* a name typed into the field, for screenshots */
	if (pre && getenv("FLUI_PPM"))
		snprintf(A.save_name, sizeof A.save_name, "%s", pre);
	A.win = fl_win_new(A.app, &o);
	if (!A.win)
		return 1;
	fl_app_watch_conf(A.app, NULL, NULL);
	fl_app_run(A.app);
	fl_app_free(A.app);
	return 0;
}
