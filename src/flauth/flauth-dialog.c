/* flauth-dialog -- the password window of flauth. A plain flui window run as the person (never as root): it prints
 * the password as one line on stdout, exit 0, or exits 1 when it is cancelled. flauth reads the line, checks it and
 * wipes it. Only flauth starts this program.
 *
 *   flauth-dialog [--reason TEXT] [--error TEXT] [--user NAME]
 * SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <flui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

enum { ID_PW = 11, ID_OK, ID_CANCEL };

struct D {
	fl_app *app;
	fl_win *win;
	fl_font *f_h, *f_reg;
	char pw[256];
	char reason[160], error[96], user[64];
	int done, ok;
};

/* squares for dots, no clipboard */
static int pw_field(fl_ui *ui, struct D *d, fl_rect r)
{
	int res = fl_widget(ui, ID_PW, r, FL_FOCUSABLE | FL_NO_FOCUS_RING);
	int focused = res & FL_FOCUS, enter = 0;
	size_t len = strlen(d->pw);
	if (focused) {
		if (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_KP_Enter) {
			enter = 1;
			ui->key = 0;
		} else if (ui->key == XKB_KEY_BackSpace) {
			while (len > 0 && ((unsigned char)d->pw[len - 1] & 0xc0) == 0x80)
				d->pw[--len] = 0;
			if (len > 0)
				d->pw[--len] = 0;
			ui->key = 0;
		} else if (ui->key == XKB_KEY_Escape) {
			d->done = 1;
			ui->key = 0;
		}
		size_t n = strlen(ui->text);
		if (n && len + n < sizeof d->pw && !(ui->mods & FL_MOD_CTRL)) {
			memcpy(d->pw + len, ui->text, n + 1);
			len += n;
		}
	}
	explicit_bzero(ui->text, sizeof ui->text);
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, r, fl_th.panel);
	fl_stroke_rect(cv, r, 1, focused ? fl_th.accent : fl_th.line);
	int x = r.x + 14, cy = r.y + r.h / 2, dots = 0;
	for (size_t i = 0; i < len; i++)
		if (((unsigned char)d->pw[i] & 0xc0) != 0x80)
			dots++;
	int maxd = (r.w - 28) / 14, first = dots > maxd ? dots - maxd : 0;
	for (int i = first; i < dots; i++)
		fl_fill_rect(cv, fl_r(x + (i - first) * 14, cy - 3, 7, 7), fl_th.text);
	x += (dots - first) * 14;
	if (!dots)
		fl_text_in(cv, d->f_reg, fl_r(r.x + 22, r.y, r.w - 30, r.h), FL_LEFT, 0, fl_th.dim, "Password");
	if (focused)
		fl_fill_rect(cv, fl_r(dots ? x : r.x + 14, cy - 9, 1, 18), fl_th.text);
	return enter;
}

static void draw(fl_ui *ui, void *ud)
{
	struct D *d = ud;
	fl_canvas *cv = &ui->cv;
	fl_rect c = fl_win_content(ui->win);
	fl_fill_rect(cv, c, fl_th.bg);
	if (!ui->focus)
		ui->focus = ID_PW;
	int x = c.x + 24, w = c.w - 48, y = c.y + 22;
	fl_text_in(cv, d->f_h, fl_r(x, y, w, 26), FL_LEFT, 0, fl_th.strong, "Enter your password");
	y += 34;
	fl_text_in(cv, d->f_reg, fl_r(x, y, w, 40), FL_LEFT, 0, fl_th.dim, d->reason);
	y += 48;
	int enter = pw_field(ui, d, fl_r(x, y, w, 40));
	y += 46;
	if (d->error[0])
		fl_text_in(cv, d->f_reg, fl_r(x, y, w, 20), FL_LEFT, 0, fl_th.err, d->error);
	y = c.y + c.h - 24 - 38;
	int cancel = fl_button(ui, ID_CANCEL, fl_r(x, y, 110, 38), "Cancel", FL_BTN_OUTLINE);
	int ok = fl_button(ui, ID_OK, fl_r(x + w - 140, y, 140, 38), "Authenticate", FL_BTN_ACCENT);
	if (cancel)
		d->done = 1;
	if ((ok || enter) && d->pw[0]) {
		d->ok = 1;
		d->done = 1;
	}
	if (d->done)
		fl_app_quit(d->app);
}

static void closed(fl_win *w, void *ud)
{
	struct D *d = ud;
	d->done = 1;
	fl_app_quit(d->app);
}

int main(int argc, char **argv)
{
	struct D d = {0};
	snprintf(d.reason, sizeof d.reason, "This needs your permission.");
	for (int i = 1; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "--reason"))
			snprintf(d.reason, sizeof d.reason, "%s", argv[i + 1]);
		else if (!strcmp(argv[i], "--error"))
			snprintf(d.error, sizeof d.error, "%s", argv[i + 1]);
		else if (!strcmp(argv[i], "--user"))
			snprintf(d.user, sizeof d.user, "%s", argv[i + 1]);
	}
	d.app = fl_app_new(NULL, NULL);
	if (!d.app) {
		fputs("flauth-dialog: no compositor\n", stderr);
		return 2;
	}
	d.f_h = fl_font_ui(600, 18);
	d.f_reg = fl_font_ui(400, 13);
	fl_win_opts o = {
		.title = "Authentication required", .app_id = "flauth", .w = 440, .h = 270, .decorated = 1,
		.resizable = 0, .draw = draw, .close = closed, .ud = &d,
	};
	d.win = fl_win_new(d.app, &o);
	if (!d.win || !d.f_h || !d.f_reg)
		return 2;
	fl_win_ui(d.win)->focus = ID_PW;
	fl_app_run(d.app);
	fl_app_free(d.app);
	int ok = d.ok;
	if (ok) {
		size_t n = strlen(d.pw);
		d.pw[n] = '\n';
		ssize_t r = write(1, d.pw, n + 1);
		(void)r;
	}
	explicit_bzero(d.pw, sizeof d.pw);
	return ok ? 0 : 1;
}
