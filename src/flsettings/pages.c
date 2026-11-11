/* pages.c -- Display, Mouse & touchpad and Keyboard pages of flsettings
 * SPDX-License-Identifier: MIT
 *
 * Every setting is a key in desktop.conf (see README); flcomp watches the file
 * and applies pointer, key repeat, Num Lock and night light at once. The
 * display mode and scale are the exception: they go to flcomp through fl_shell
 * (set_output), which tries them and goes back after 15 seconds unless this
 * program confirms; only a confirmed mode is written to desktop.conf. Brightness
 * is the backlight's sysfs file (group seat may write it, see the udev rule).
 */
#include <dirent.h>
#include <errno.h>
#include <flui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "fl-shell-client-protocol.h"
#include "pages.h"
#include "wallpaper.h"

enum {
	ID_RES = 100,	/* .. 131 */
	ID_HZ = 140,	/* .. 147 */
	ID_SCALE = 150,	/* .. 153 */
	ID_APPLY = 160, ID_RESET, ID_KEEP, ID_REVERT, ID_BRIGHT, ID_NIGHT, ID_WARM,
	ID_SPEED = 200, ID_ACCEL = 201 /* .. 202 */, ID_LEFT = 203, ID_MNAT, ID_TNAT, ID_TAP, ID_DWT,
	ID_KTILES = 300 /* .. 303 */, ID_DELAY = 310, ID_RATE, ID_NUM, ID_TRY,
};

#define MAXMODES 64
#define ROW 56

struct mode { int w, h, hz, flags; };

struct pages {
	fl_app *app;
	fl_font *head, *small;
	fl_win *win;
	struct wallpaper *wp;
	/* keyboard */
	int kbd;
	char status[200];
	char trial[128];
	/* display, from fl_shell */
	struct fl_shell *shell;
	unsigned shell_ver;
	struct mode modes[MAXMODES];
	int nmodes, batch_done;
	int scale, lw, lh;		/* now: percent, the desktop's size */
	int cur_w, cur_h, cur_hz;
	int have_state;
	int pending;			/* seconds flcomp gave us to confirm, 0: none */
	long deadline;			/* ms, local clock */
	int sel_w, sel_h, sel_hz, sel_scale;
	char msg[160];
	int msg_err;
	fl_timer *tick;
	fl_timer *bl_poll;		/* the brightness keys change it behind our back: look once a second */
	/* brightness */
	char bl_dir[300];
	int bl_max, bl_pct;
	long bl_read_at;
	char bl_err[120];
};

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ---- look: the same as the General page ------------------------------------------------------ */

void pg_section(fl_ui *ui, fl_font *head, int x, int y, int w, const char *name)
{
	fl_text(&ui->cv, head, x, y, fl_th.dim, name, -1);
	fl_fill_rect(&ui->cv, fl_r(x, y + 20, w, 1), fl_th.line);
}

/* title and a dim line under it, on the left of a row 56 px high */
void pg_row_text(fl_ui *ui, fl_font *small, int x, int y, int maxw, const char *title, const char *what)
{
	int th = fl_font_height(ui->f_med), sh = fl_font_height(small);
	int top = y + (ROW - th - 2 - sh) / 2;
	char fit[160];
	fl_text_fit(ui->f_med, title, maxw, fit, sizeof fit);
	fl_text(&ui->cv, ui->f_med, x, top, fl_th.text, fit, -1);
	fl_text_fit(small, what, maxw, fit, sizeof fit);
	fl_text(&ui->cv, small, x, top + th + 2, fl_th.dim, fit, -1);
}

int pg_choice(fl_ui *ui, uint32_t id, fl_rect r, const char *label, int on)
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

/* a choice that is not available: dim, does nothing */
static void choice_off(fl_ui *ui, fl_rect r, const char *label)
{
	fl_stroke_rect(&ui->cv, r, 1, fl_th.line);
	fl_text_in(&ui->cv, ui->f_reg, r, FL_CENTER, 0, fl_mix(fl_th.panel, fl_th.dim, 120), label);
}

/* the flyout's slider: 4 px track, 10 x 20 thumb; Left/Right step, Home/End. 1 when *v changed */
static int slider(fl_ui *ui, uint32_t id, fl_rect r, int *v, int lo, int hi, int step)
{
	int res = fl_widget(ui, id, r, FL_FOCUSABLE), old = *v;
	int x0 = r.x + 6, x1 = r.x + r.w - 6;
	if (res & FL_DOWN) {
		int nv = lo + ((ui->mx - x0) * (hi - lo) + (x1 - x0) / 2) / (x1 - x0);
		*v = nv < lo ? lo : nv > hi ? hi : nv;
	}
	if (res & FL_FOCUS) {
		int k = ui->key;
		if (k == XKB_KEY_Left || k == XKB_KEY_Down)
			*v = *v - step < lo ? lo : *v - step;
		else if (k == XKB_KEY_Right || k == XKB_KEY_Up)
			*v = *v + step > hi ? hi : *v + step;
		else if (k == XKB_KEY_Home)
			*v = lo;
		else if (k == XKB_KEY_End)
			*v = hi;
		if (*v != old || k == XKB_KEY_Home || k == XKB_KEY_End)
			ui->key = 0;
	}
	int tx = x0 + (*v - lo) * (x1 - x0) / (hi - lo), ty = r.y + r.h / 2 - 2;
	fl_fill_rect(&ui->cv, fl_r(x0, ty, x1 - x0, 4), fl_th.line);
	fl_fill_rect(&ui->cv, fl_r(x0, ty, tx - x0, 4), fl_th.accent);
	fl_rect th = fl_r(tx - 5, r.y + r.h / 2 - 10, 10, 20);
	fl_fill_rect(&ui->cv, th, (res & FL_DOWN) ? fl_th.accent : fl_th.text);
	if (res & FL_FOCUS)
		fl_stroke_rect(&ui->cv, fl_inset(th, -3, -3), 1, fl_th.accent);
	return *v != old;
}

static void conf_int(const char *key, int v)
{
	char b[16];
	snprintf(b, sizeof b, "%d", v);
	fl_conf_set(key, b);
}

static int sw_row(fl_ui *ui, struct pages *p, uint32_t id, int x, int y, int w, const char *title, const char *what,
                  const char *key, int def)
{
	pg_row_text(ui, p->small, x, y, w - 60, title, what);
	int on = fl_conf_int(key, def);
	if (fl_switch(ui, id, fl_r(x + w - 40, y + 18, 40, 20), &on))
		fl_conf_set(key, on ? "1" : "0");
	return on;
}

/* ---- brightness ------------------------------------------------------------------------------ */

static void bl_find(struct pages *p)
{
	p->bl_dir[0] = 0;
	const char *e = getenv("FLSETTINGS_BACKLIGHT");
	if (e && *e) {
		snprintf(p->bl_dir, sizeof p->bl_dir, "%s", e);
	} else {
		DIR *d = opendir("/sys/class/backlight");
		struct dirent *de;
		while (d && (de = readdir(d)))
			if (de->d_name[0] != '.') {
				snprintf(p->bl_dir, sizeof p->bl_dir, "/sys/class/backlight/%s", de->d_name);
				break;
			}
		if (d)
			closedir(d);
	}
}

static int read_int(const char *dir, const char *name)
{
	char path[360];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	FILE *f = fopen(path, "r");
	int v = -1;
	if (f) {
		if (fscanf(f, "%d", &v) != 1)
			v = -1;
		fclose(f);
	}
	return v;
}

static void bl_refresh(struct pages *p)
{
	p->bl_read_at = now_ms();
	if (!p->bl_dir[0])
		return;
	p->bl_max = read_int(p->bl_dir, "max_brightness");
	int cur = read_int(p->bl_dir, "brightness");
	if (p->bl_max > 0 && cur >= 0)
		p->bl_pct = (cur * 100 + p->bl_max / 2) / p->bl_max;
	else
		p->bl_max = 0;
}

static void bl_set(struct pages *p, int pct)
{
	int v = p->bl_max * pct / 100;
	if (v < 1)
		v = 1;
	char path[360];
	snprintf(path, sizeof path, "%s/brightness", p->bl_dir);
	FILE *f = fopen(path, "w");
	if (f && fprintf(f, "%d\n", v) > 0 && fclose(f) == 0) {
		p->bl_err[0] = 0;
		return;
	}
	snprintf(p->bl_err, sizeof p->bl_err, "Cannot change it: %s (the account needs to be in the group seat).",
	         strerror(errno));
}

/* ---- the display, from fl_shell -------------------------------------------------------------- */

static void on_mode(void *ud, struct fl_shell *sh, uint32_t w, uint32_t h, uint32_t hz, uint32_t flags)
{
	struct pages *p = ud;
	if (p->batch_done) {
		p->nmodes = 0;
		p->batch_done = 0;
	}
	if (p->nmodes < MAXMODES)
		p->modes[p->nmodes++] = (struct mode){(int)w, (int)h, (int)hz, (int)flags};
	if (flags & 1) {
		p->cur_w = (int)w;
		p->cur_h = (int)h;
		p->cur_hz = (int)hz;
	}
}

static void tick_cb(void *ud);

static void on_state(void *ud, struct fl_shell *sh, uint32_t scale, uint32_t w, uint32_t h, uint32_t pending)
{
	struct pages *p = ud;
	int was_pending = p->pending;
	p->batch_done = 1;
	p->scale = (int)scale;
	p->lw = (int)w;
	p->lh = (int)h;
	p->pending = (int)pending;
	if (pending) {
		p->deadline = now_ms() + (long)pending * 1000;
		if (p->tick)
			fl_timer_arm(p->tick, 250, 250);
	} else if (p->tick) {
		fl_timer_arm(p->tick, 0, 0);
	}
	if (!p->have_state || !pending) {
		p->sel_w = p->cur_w;
		p->sel_h = p->cur_h;
		p->sel_hz = p->cur_hz;
		p->sel_scale = p->scale;
	}
	p->have_state = 1;
	if (was_pending && !pending && p->msg[0] == 0)
		snprintf(p->msg, sizeof p->msg, "Back to the previous display settings.");
	if (p->win)
		fl_win_redraw(p->win);
}

static void on_failed(void *ud, struct fl_shell *sh)
{
	struct pages *p = ud;
	snprintf(p->msg, sizeof p->msg, "The display did not accept that. Nothing was changed.");
	p->msg_err = 1;
	if (p->win)
		fl_win_redraw(p->win);
}

static void noop0(void *ud, struct fl_shell *sh) {}
static void noop_added(void *ud, struct fl_shell *sh, uint32_t id, const char *a, const char *t) {}
static void noop_changed(void *ud, struct fl_shell *sh, uint32_t id, const char *t, uint32_t f, uint32_t m) {}
static void noop_id(void *ud, struct fl_shell *sh, uint32_t id) {}
static void noop_str(void *ud, struct fl_shell *sh, const char *s) {}

static const struct fl_shell_listener shell_listener = {
	.window_added = noop_added, .window_changed = noop_changed, .window_removed = noop_id,
	.menu_dismissed = noop0, .menu_toggle = noop0, .keyboard_layout = noop_str,
	.flyout_dismissed = noop0, .volume_key = noop_id,
	.output_mode = on_mode, .output_state = on_state, .output_failed = on_failed,
};

static void tick_cb(void *ud)
{
	struct pages *p = ud;
	if (p->win)
		fl_win_redraw(p->win);
	if (p->pending && now_ms() > p->deadline + 3000)
		fl_timer_arm(p->tick, 0, 0);
}

void pages_set_shell(struct pages *p, struct fl_shell *sh, unsigned version)
{
	p->shell = sh;
	p->shell_ver = version;
	if (sh && version >= 3)
		fl_shell_add_listener(sh, &shell_listener, p);
}

static void send_set(struct pages *p, int w, int h, int hz, int scale)
{
	fl_shell_set_output(p->shell, (uint32_t)w, (uint32_t)h, (uint32_t)hz, (uint32_t)scale);
	wl_display_flush(fl_app_display(p->app));
	p->msg[0] = 0;
	p->msg_err = 0;
}

static int cmp_res(const void *a, const void *b)
{
	const struct mode *x = a, *y = b;
	long ax = (long)x->w * x->h, ay = (long)y->w * y->h;
	return ay > ax ? 1 : ay < ax ? -1 : y->w - x->w;
}

static int scale_ok(int w, int h, int scale)
{
	return w * 100 / scale >= 640 && h * 100 / scale >= 400;
}

static void draw_banner(fl_ui *ui, struct pages *p, int x, int y, int w)
{
	fl_rect r = fl_r(x, y, w, ROW);
	fl_fill_rect(&ui->cv, r, fl_th.select);
	fl_fill_rect(&ui->cv, fl_r(r.x, r.y, 3, r.h), fl_th.accent);
	long left = (p->deadline - now_ms() + 999) / 1000;
	if (left < 0)
		left = 0;
	if (left > p->pending)
		left = p->pending;
	char sub[96];
	snprintf(sub, sizeof sub, "Going back to the old settings in %ld s", left);
	pg_row_text(ui, p->small, r.x + 16, r.y, w - 220, "Keep these display settings?", sub);
	if (fl_button(ui, ID_KEEP, fl_r(r.x + w - 190, r.y + 14, 84, 28), "Keep", FL_BTN_ACCENT)) {
		fl_shell_confirm_output(p->shell);
		wl_display_flush(fl_app_display(p->app));
		char b[48];
		snprintf(b, sizeof b, "%dx%d@%d", p->cur_w, p->cur_h, p->cur_hz);
		fl_conf_set("mode", b);
		conf_int("scale", p->scale);
		snprintf(p->msg, sizeof p->msg, "Saved. These settings are used at every start.");
		p->msg_err = 0;
	}
	if (fl_button(ui, ID_REVERT, fl_r(r.x + w - 98, r.y + 14, 84, 28), "Revert", FL_BTN_OUTLINE)) {
		fl_shell_revert_output(p->shell);
		wl_display_flush(fl_app_display(p->app));
	}
}

static int draw_display(fl_ui *ui, struct pages *p, fl_rect c)
{
	int x = c.x + 28, w = c.w - 56, y = c.y + 24;
	char buf[96];

	if (p->pending && p->shell_ver >= 3) {
		draw_banner(ui, p, x, y, w);
		y += ROW + 16;
	}
	pg_section(ui, p->head, x, y, w, "Screen");
	y += 28;
	if (p->shell_ver < 3 || !p->have_state) {
		pg_row_text(ui, p->small, x, y, w, "Resolution and scale",
		            p->shell ? "The compositor is too old for this (needs fl_shell 3)." : "Only available inside the FreeLFS desktop.");
		y += ROW + 24;
	} else {
		/* distinct sizes, biggest first */
		struct mode res[MAXMODES];
		int nres = 0;
		for (int i = 0; i < p->nmodes; i++) {
			int dup = 0;
			for (int j = 0; j < nres; j++)
				dup |= res[j].w == p->modes[i].w && res[j].h == p->modes[i].h;
			if (!dup && nres < 16)
				res[nres++] = p->modes[i];
		}
		qsort(res, (size_t)nres, sizeof *res, cmp_res);
		const struct mode *pref = NULL;
		for (int i = 0; i < p->nmodes; i++)
			if (p->modes[i].flags & 2)
				pref = &p->modes[i];
		snprintf(buf, sizeof buf, pref ? "Recommended: %d \xc3\x97 %d" : "The display's own size is the sharpest",
		         pref ? pref->w : 0, pref ? pref->h : 0);
		pg_row_text(ui, p->small, x, y, w, "Resolution", buf);
		y += ROW - 6;
		int cols = 4, gap = 8, tw = (w - (cols - 1) * gap) / cols;
		for (int i = 0; i < nres; i++) {
			fl_rect t = fl_r(x + (i % cols) * (tw + gap), y + (i / cols) * (32 + gap), tw, 32);
			snprintf(buf, sizeof buf, "%d \xc3\x97 %d", res[i].w, res[i].h);
			if (pg_choice(ui, ID_RES + (uint32_t)i, t, buf, res[i].w == p->sel_w && res[i].h == p->sel_h)) {
				p->sel_w = res[i].w;
				p->sel_h = res[i].h;
				/* the best rate it has, or the one in use when it is the same size */
				int best = 0;
				for (int k = 0; k < p->nmodes; k++)
					if (p->modes[k].w == res[i].w && p->modes[k].h == res[i].h && p->modes[k].hz > best)
						best = p->modes[k].hz;
				p->sel_hz = res[i].w == p->cur_w && res[i].h == p->cur_h ? p->cur_hz : best;
				if (!scale_ok(p->sel_w, p->sel_h, p->sel_scale))
					p->sel_scale = 100;
			}
		}
		y += (nres + cols - 1) / cols * (32 + gap) + 4;

		/* refresh rates of the chosen size */
		int hzs[8], nhz = 0;
		for (int i = 0; i < p->nmodes; i++)
			if (p->modes[i].w == p->sel_w && p->modes[i].h == p->sel_h) {
				int dup = 0;
				for (int j = 0; j < nhz; j++)
					dup |= hzs[j] == p->modes[i].hz;
				if (!dup && nhz < 8)
					hzs[nhz++] = p->modes[i].hz;
			}
		for (int i = 0; i < nhz; i++)
			for (int j = i + 1; j < nhz; j++)
				if (hzs[j] > hzs[i]) {
					int t = hzs[i];
					hzs[i] = hzs[j];
					hzs[j] = t;
				}
		pg_row_text(ui, p->small, x, y, w - 4 * 80, "Refresh rate", "How often the screen updates");
		for (int i = 0; i < nhz && i < 4; i++) {
			snprintf(buf, sizeof buf, "%d Hz", hzs[i]);
			fl_rect t = fl_r(x + w - ((nhz < 4 ? nhz : 4) * 80 - 8) + i * 80, y + 12, 72, 32);
			if (pg_choice(ui, ID_HZ + (uint32_t)i, t, buf, hzs[i] == p->sel_hz))
				p->sel_hz = hzs[i];
		}
		y += ROW;

		pg_row_text(ui, p->small, x, y, w - 4 * 72, "Scale", "Size of text and windows");
		static const int scales[4] = {100, 125, 150, 200};
		for (int i = 0; i < 4; i++) {
			snprintf(buf, sizeof buf, "%d%%", scales[i]);
			fl_rect t = fl_r(x + w - (4 * 72 - 8) + i * 72, y + 12, 64, 32);
			if (!scale_ok(p->sel_w, p->sel_h, scales[i]))
				choice_off(ui, t, buf);
			else if (pg_choice(ui, ID_SCALE + (uint32_t)i, t, buf, scales[i] == p->sel_scale))
				p->sel_scale = scales[i];
		}
		y += ROW;

		int differs = p->sel_w != p->cur_w || p->sel_h != p->cur_h || p->sel_hz != p->cur_hz || p->sel_scale != p->scale;
		if (differs && !p->pending) {
			fl_text_in(&ui->cv, p->small, fl_r(x, y, w - 190, 32), FL_LEFT, 0, fl_th.dim,
			           "You get 15 seconds to keep the change.");
			if (fl_button(ui, ID_APPLY, fl_r(x + w - 184, y, 84, 32), "Apply", FL_BTN_ACCENT))
				send_set(p, p->sel_w, p->sel_h, p->sel_hz, p->sel_scale);
			if (fl_button(ui, ID_RESET, fl_r(x + w - 92, y, 92, 32), "Reset", FL_BTN_OUTLINE)) {
				p->sel_w = p->cur_w;
				p->sel_h = p->cur_h;
				p->sel_hz = p->cur_hz;
				p->sel_scale = p->scale;
			}
		} else if (p->msg[0]) {
			fl_text_in(&ui->cv, p->small, fl_r(x, y, w, 32), FL_LEFT, 0, p->msg_err ? fl_th.err : fl_th.dim, p->msg);
		} else {
			snprintf(buf, sizeof buf, "The desktop is %d \xc3\x97 %d. 100%% and 200%% are the sharpest.", p->lw, p->lh);
			fl_text_in(&ui->cv, p->small, fl_r(x, y, w, 32), FL_LEFT, 0, fl_th.dim, buf);
		}
		y += 28 + 18;
	}

	pg_section(ui, p->head, x, y, w, "Brightness and colour");
	y += 28;
	if (p->bl_max > 0) {
		if (now_ms() - p->bl_read_at > 1000 && !(ui->btn_held & FL_BTN_L))
			bl_refresh(p);
		snprintf(buf, sizeof buf, "%d%%", p->bl_pct);
		pg_row_text(ui, p->small, x, y, w - 280, "Brightness", "Backlight of this screen");
		fl_text_in(&ui->cv, ui->f_reg, fl_r(x + w - 44, y, 44, ROW), FL_RIGHT, 0, fl_th.text, buf);
		int v = p->bl_pct;
		if (slider(ui, ID_BRIGHT, fl_r(x + w - 280, y + 12, 224, 32), &v, 1, 100, 5)) {
			p->bl_pct = v;
			bl_set(p, v);
			conf_int("brightness", v);
		}
	} else {
		pg_row_text(ui, p->small, x, y, w, "Brightness", "This screen has no backlight to adjust");
	}
	y += ROW;
	if (p->bl_err[0]) {
		fl_text_in(&ui->cv, p->small, fl_r(x, y - 8, w, 18), FL_LEFT, 0, fl_th.err, p->bl_err);
		y += 14;
	}
	int night = sw_row(ui, p, ID_NIGHT, x, y, w, "Night light", "Warmer colours, easier on the eyes in the evening",
	                   "night_light", 0);
	y += ROW;
	if (night) {
		int warm = fl_conf_int("night_warmth", 50);
		snprintf(buf, sizeof buf, "%d%%", warm);
		pg_row_text(ui, p->small, x, y, w - 280, "Warmth", "How orange the screen gets");
		fl_text_in(&ui->cv, ui->f_reg, fl_r(x + w - 44, y, 44, ROW), FL_RIGHT, 0, fl_th.text, buf);
		if (slider(ui, ID_WARM, fl_r(x + w - 280, y + 12, 224, 32), &warm, 5, 100, 5))
			conf_int("night_warmth", warm);
		y += ROW;
	}
	y += 24;
	if (p->wp)
		y = wp_draw(p->wp, ui, x, y, w);
	return y - c.y;
}

/* ---- mouse and touchpad ---------------------------------------------------------------------- */

static int have_device(const char *needle)
{
	FILE *f = fopen("/proc/bus/input/devices", "r");
	char line[256];
	int found = 0;
	while (f && fgets(line, sizeof line, f))
		if (line[0] == 'N' && strstr(line, needle))
			found = 1;
	if (f)
		fclose(f);
	return found;
}

static void section_note(fl_ui *ui, struct pages *p, int x, int y, int w, const char *note)
{
	int tw = fl_text_width(p->small, note, -1);
	fl_rect cover = fl_r(x + w - tw - 8, y - 1, tw + 8, 20);
	fl_fill_rect(&ui->cv, cover, fl_th.panel);
	fl_text(&ui->cv, p->small, x + w - tw, y + 1, fl_th.dim, note, -1);
}

static int draw_input(fl_ui *ui, struct pages *p, fl_rect c)
{
	int x = c.x + 28, w = c.w - 56, y = c.y + 24;
	char buf[32];

	pg_section(ui, p->head, x, y, w, "Pointer");
	y += 28;
	int sp = fl_conf_int("pointer_speed", 0);
	pg_row_text(ui, p->small, x, y, w - 280, "Pointer speed", "How far the pointer moves");
	if (slider(ui, ID_SPEED, fl_r(x + w - 224, y + 12, 224, 32), &sp, -100, 100, 10))
		conf_int("pointer_speed", sp);
	y += ROW;
	pg_row_text(ui, p->small, x, y, w - 190, "Acceleration", "Adaptive speeds up with quick movements");
	int flat = !strcmp(fl_conf_get("pointer_accel", "adaptive"), "flat");
	if (pg_choice(ui, ID_ACCEL, fl_r(x + w - 176, y + 12, 86, 32), "Adaptive", !flat) && flat)
		fl_conf_set("pointer_accel", "adaptive");
	if (pg_choice(ui, ID_ACCEL + 1, fl_r(x + w - 86, y + 12, 86, 32), "Flat", flat) && !flat)
		fl_conf_set("pointer_accel", "flat");
	y += ROW;
	sw_row(ui, p, ID_LEFT, x, y, w, "Left-handed", "Swap the left and right buttons", "left_handed", 0);
	y += ROW + 24;

	pg_section(ui, p->head, x, y, w, "Mouse");
	if (!have_device("ouse"))
		section_note(ui, p, x, y, w, "none detected");
	y += 28;
	sw_row(ui, p, ID_MNAT, x, y, w, "Natural scrolling", "Content follows the wheel, like on a phone", "mouse_natural", 0);
	y += ROW + 24;

	pg_section(ui, p->head, x, y, w, "Touchpad");
	if (!have_device("ouchpad"))
		section_note(ui, p, x, y, w, "none detected");
	y += 28;
	sw_row(ui, p, ID_TNAT, x, y, w, "Natural scrolling", "Two fingers move the content, not the scrollbar",
	       "touchpad_natural", 0);
	y += ROW;
	sw_row(ui, p, ID_TAP, x, y, w, "Tap to click", "A light tap on the pad is a click", "tap_click", 1);
	y += ROW;
	sw_row(ui, p, ID_DWT, x, y, w, "Disable while typing", "Ignore the pad while keys are pressed", "typing_disable", 1);
	y += ROW;
	(void)buf;
	return y + 12 - c.y;
}

/* ---- keyboard -------------------------------------------------------------------------------- */

static void pick_layout(struct pages *p, int i)
{
	const fl_kbd_layout *l = &fl_kbd_layouts[i];
	p->kbd = i;
	fl_conf_set("keyboard", l->id);
	if (p->shell) {
		fl_shell_set_keyboard_layout(p->shell, l->xkb);
		wl_display_flush(fl_app_display(p->app));
	}
	if (fl_kbd_conf_write(l->id) == 0)
		snprintf(p->status, sizeof p->status, "Saved for every user, from the next start.");
	else
		snprintf(p->status, sizeof p->status, "Saved for you. For all users it needs administrator rights (%s).",
		         strerror(errno));
}

/* ---- the shortcut list (read only): what flcomp does with these keys, src/flcomp/seat.c --------- */

static const struct { const char *keys, *what; } shortcuts[] = {
	{"Super", "Start menu"},
	{"Super+E", "Files"},
	{"Super+T / Ctrl+Alt+T", "Terminal"},
	{"Alt+Tab / Alt+Shift+Tab", "Switch windows (hold Alt)"},
	{"Ctrl+Shift+Esc", "General Manager (task manager)"},
	{"Super+Left / Super+Right", "Snap to the left or right half"},
	{"Super+Up", "Maximize"},
	{"Super+Down", "Restore, or minimize"},
	{"Print", "Screenshot to the pictures folder"},
	{"Media keys", "Volume, brightness, microphone"},
	{"Ctrl+Alt+Backspace", "Log out at once"},
	{"Ctrl+Alt+F1 ... F6", "Text consoles"},
};

/* "Super+Left / Super+Right": every key a flat chip (raised, 1 px line), a dim + between the keys of a
 * combination and a dim / between alternatives. Returns the end x; draw 0 only measures. */
static int key_chips(fl_ui *ui, fl_font *f, int x, int y, int h, const char *combo, int draw)
{
	fl_canvas *cv = &ui->cv;
	const char *q = combo;
	while (*q) {
		const char *e = q;
		while (*e && *e != '+' && *e != '/')
			e++;
		int n = (int)(e - q);
		while (n > 0 && q[n - 1] == ' ')
			n--;
		if (n > 0 && *q != ' ') {
			char part[48];
			snprintf(part, sizeof part, "%.*s", n, q);
			int w = fl_text_width(f, part, -1) + 16;
			fl_rect r = fl_r(x, y, w, h);
			if (draw) {
				fl_fill_rect(cv, r, fl_th.raised);
				fl_stroke_rect(cv, r, 1, fl_th.line);
				fl_text_in(cv, f, r, FL_CENTER, 0, fl_th.text, part);
			}
			x += w;
		}
		if (*e == '+' || *e == '/') {	/* between two keys of one combination, or two combinations */
			int gap = *e == '+' ? 14 : 26;
			if (draw)
				fl_text_in(cv, f, fl_r(x, y, gap, h), FL_CENTER, 0, fl_th.dim, *e == '+' ? "+" : "/");
			x += gap;
		}
		q = *e ? e + 1 : e;
		while (*q == ' ')
			q++;
	}
	return x;
}

static int draw_shortcuts(fl_ui *ui, struct pages *p, int x, int y, int w)
{
	pg_section(ui, p->head, x, y, w, "Shortcuts");
	y += 36;
	int kw = 0;
	for (size_t i = 0; i < sizeof shortcuts / sizeof *shortcuts; i++) {
		int e = key_chips(ui, p->small, 0, 0, 22, shortcuts[i].keys, 0);
		kw = e > kw ? e : kw;
	}
	kw += 24;
	for (size_t i = 0; i < sizeof shortcuts / sizeof *shortcuts; i++) {
		char fit[160];
		key_chips(ui, p->small, x, y + 4, 22, shortcuts[i].keys, 1);
		fl_text_fit(ui->f_reg, shortcuts[i].what, w - kw, fit, sizeof fit);
		fl_text_in(&ui->cv, ui->f_reg, fl_r(x + kw, y, w - kw, 30), FL_LEFT, 0, fl_th.dim, fit);
		y += 30;
	}
	return y;
}

static int draw_keyboard(fl_ui *ui, struct pages *p, fl_rect c)
{
	int x = c.x + 28, w = c.w - 56, y = c.y + 24;
	char buf[48];

	pg_section(ui, p->head, x, y, w, "Layout");
	y += 36;
	int pick = fl_kbd_tiles(ui, ID_KTILES, fl_r(x, y, w, 64), 8, p->kbd);
	if (pick >= 0 && pick != p->kbd)
		pick_layout(p, pick);
	y += 64 + 10;
	fl_text_in(&ui->cv, p->small, fl_r(x, y, w, 18), FL_LEFT, 0, fl_th.dim,
	           p->status[0] ? p->status : "Applies at once in every program.");
	y += 18 + 22;

	pg_section(ui, p->head, x, y, w, "Typing");
	y += 28;
	int delay = fl_conf_int("key_repeat_delay", 600);
	snprintf(buf, sizeof buf, "%d ms", delay);
	pg_row_text(ui, p->small, x, y, w - 300, "Repeat delay", "How long a key is held before it repeats");
	fl_text_in(&ui->cv, ui->f_reg, fl_r(x + w - 64, y, 64, ROW), FL_RIGHT, 0, fl_th.text, buf);
	if (slider(ui, ID_DELAY, fl_r(x + w - 300, y + 12, 224, 32), &delay, 150, 1000, 50))
		conf_int("key_repeat_delay", delay);
	y += ROW;
	int rate = fl_conf_int("key_repeat_rate", 25);
	snprintf(buf, sizeof buf, "%d/s", rate);
	pg_row_text(ui, p->small, x, y, w - 300, "Repeat rate", "How fast it repeats, keys per second");
	fl_text_in(&ui->cv, ui->f_reg, fl_r(x + w - 64, y, 64, ROW), FL_RIGHT, 0, fl_th.text, buf);
	if (slider(ui, ID_RATE, fl_r(x + w - 300, y + 12, 224, 32), &rate, 2, 60, 2))
		conf_int("key_repeat_rate", rate);
	y += ROW;
	sw_row(ui, p, ID_NUM, x, y, w, "Num Lock at start", "Number pad types digits when the desktop starts",
	       "numlock", 0);
	y += ROW + 24;

	pg_section(ui, p->head, x, y, w, "Try it");
	y += 36;
	fl_textfield(ui, ID_TRY, fl_r(x, y, w, 36), p->trial, sizeof p->trial,
	             "Click here and type to try the layout and the repeat", ui->f_reg);
	y += 36 + 24;
	y = draw_shortcuts(ui, p, x, y, w);
	return y + 12 - c.y;
}

/* ---- the lot --------------------------------------------------------------------------------- */

/* Two steps: the object exists before the Wayland connection (fl_shell's first events come
 * while the globals are bound), the rest is set up once there is an app. */
static void bl_poll_cb(void *ud);

struct pages *pages_new(void)
{
	struct pages *p = calloc(1, sizeof *p);
	if (p)
		p->scale = 100;
	return p;
}

void pages_start(struct pages *p, fl_app *app, fl_font *head, fl_font *small)
{
	p->app = app;
	p->head = head;
	p->small = small;
	p->wp = wp_new(app, head, small);
	p->tick = fl_timer_new(app, tick_cb, p);
	p->bl_poll = fl_timer_new(app, bl_poll_cb, p);
	int k = fl_kbd_find(fl_conf_get("keyboard", ""));
	if (k < 0)
		k = fl_kbd_conf_read();
	p->kbd = k >= 0 ? k : 1;
	bl_find(p);
	bl_refresh(p);
}

void pages_free(struct pages *p)
{
	if (!p)
		return;
	wp_free(p->wp);
	if (p->tick)
		fl_timer_free(p->tick);
	if (p->bl_poll)
		fl_timer_free(p->bl_poll);
	free(p);
}

void pages_set_win(struct pages *p, fl_win *win)
{
	p->win = win;
	if (p->wp)
		wp_set_win(p->wp, win);
}

static void bl_poll_cb(void *ud)
{
	struct pages *p = ud;
	int old = p->bl_pct;
	bl_refresh(p);
	if (p->bl_pct != old && p->win)
		fl_win_redraw(p->win);
}

/* page: PG_*, or -1 for a page that is not one of these */
void pages_show(struct pages *p, int page)
{
	if (page == PG_DISPLAY) {
		if (p->wp)
			wp_show(p->wp);
		bl_refresh(p);
		if (p->bl_poll && p->bl_max > 0)
			fl_timer_arm(p->bl_poll, 1000, 1000);
	} else if (p->bl_poll) {
		fl_timer_arm(p->bl_poll, 0, 0);
	}
}

int pages_draw(struct pages *p, int page, fl_ui *ui, fl_rect c)
{
	switch (page) {
	case PG_DISPLAY: return draw_display(ui, p, c);
	case PG_INPUT: return draw_input(ui, p, c);
	default: return draw_keyboard(ui, p, c);
	}
}
