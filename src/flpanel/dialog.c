/* dialog.c -- the panel's small windows: the power confirmation, an error with the command's
 * stderr, and a name (new folder, new file, rename). Decorated windows, one at a time.
 *
 * Power: "Shut down in 30 s" (Restart and Log out alike) counts down; "Shut down now" runs at
 * once; Cancel, Esc or the window's close button stops it. The command is `flpower off`,
 * `flpower restart` or `flpower logout`, run by sh with stderr on a pipe. The panel ignores
 * SIGCHLD (main), so waitpid cannot tell how it ended: the wrapper writes its status to a second
 * pipe. A failure shows the stderr text in an error window.
 * Buttons run from a timer, never inside the window's own draw pass (the window may go).
 * Keys: Enter does the main button, Esc cancels.
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <flui.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "flpanel.h"

#define DLG_W 400
#define POW_H 184
#define ERR_H 176
#define NAME_H 132
#define COUNT_S 30
#define TEXT_MAX 700
#define BTN_H 32

enum { DLG_NONE, DLG_POWER, DLG_ERROR, DLG_NAME };
enum { ACT_NONE, ACT_CLOSE, ACT_GO };
enum { ID_CANCEL = 1, ID_GO, ID_FIELD };

static struct dlg {
	struct panel *p;
	fl_win *w;
	fl_timer *tick, *later;	/* made once, kept: a timer is never freed inside its own callback */
	int kind, what, left, running, act;
	char title[64], text[TEXT_MAX], field[256], ctx[512];
	dlg_name_cb done;
	int err_fd, st_fd;
} D = { .err_fd = -1, .st_fd = -1 };

static const struct {
	const char *cmd, *verb, *now, *head;
} power[POW_LOGOUT + 1] = {
	{"flpower off", "Shut down", "Shut down now", "Shut down in %d s"},
	{"flpower restart", "Restart", "Restart now", "Restart in %d s"},
	{"flpower logout", "Log out", "Log out now", "Log out in %d s"},
};

static void dlg_destroy(void)
{
	if (D.tick)
		fl_timer_arm(D.tick, 0, 0);
	if (D.later)
		fl_timer_arm(D.later, 0, 0);
	D.act = ACT_NONE;
	if (D.err_fd >= 0) {
		fl_fd_remove(D.p->app, D.err_fd);
		close(D.err_fd);
		D.err_fd = -1;
	}
	if (D.st_fd >= 0) {
		close(D.st_fd);
		D.st_fd = -1;
	}
	D.running = 0;
	D.kind = DLG_NONE;
	if (D.w) {
		fl_win_destroy(D.w);
		D.w = NULL;
	}
}

static void dlg_draw(fl_ui *ui, void *ud);
static void power_run(void);

/* the window's close button: after the pass; a running command is not stopped by it */
static void on_close(fl_win *w, void *ud)
{
	(void)w;
	(void)ud;
	if (D.running)
		return;
	D.act = ACT_CLOSE;
	fl_timer_arm(D.later, 1, 0);
}

static void go(void)
{
	if (D.kind == DLG_POWER) {
		if (!D.running)
			power_run();
	} else if (D.kind == DLG_NAME) {
		char name[256], ctx[512];
		snprintf(name, sizeof name, "%s", D.field);
		snprintf(ctx, sizeof ctx, "%s", D.ctx);
		struct panel *p = D.p;
		dlg_name_cb done = D.done;
		int kind = D.what;
		dlg_destroy();
		if (done && name[0])
			done(p, kind, ctx, name);
	} else {
		dlg_destroy();
	}
}

/* runs the button or close that a draw pass asked for */
static void on_later(void *ud)
{
	(void)ud;
	int act = D.act;
	D.act = ACT_NONE;
	if (act == ACT_CLOSE)
		dlg_destroy();
	else if (act == ACT_GO)
		go();
}

static void on_tick(void *ud)
{
	(void)ud;
	if (!D.w || D.running || D.kind != DLG_POWER)
		return;
	if (--D.left <= 0) {
		power_run();
		return;
	}
	fl_win_redraw(D.w);
}

/* the command's stderr: read until it ends, then its status comes from the other pipe */
static void on_out(void *ud, int fd, short rev)
{
	(void)ud;
	(void)rev;
	char buf[256];
	ssize_t n = read(fd, buf, sizeof buf);
	if (n == -1 && (errno == EINTR || errno == EAGAIN))
		return;
	if (n > 0) {
		size_t have = strlen(D.text), room = sizeof D.text - 1 - have;
		if (room > (size_t)n)
			room = (size_t)n;
		memcpy(D.text + have, buf, room);
		D.text[have + room] = 0;
		return;
	}
	fl_fd_remove(D.p->app, fd);
	close(fd);
	D.err_fd = -1;
	char st[16] = "";
	ssize_t m = D.st_fd >= 0 ? read(D.st_fd, st, sizeof st - 1) : 0;
	if (D.st_fd >= 0)
		close(D.st_fd);
	D.st_fd = -1;
	int code = m > 0 ? atoi(st) : -1;
	if (code == 0) { /* done: the machine goes down or the session ends, and this goes with it */
		D.act = ACT_CLOSE;
		fl_timer_arm(D.later, 1, 0);
		return;
	}
	char msg[TEXT_MAX], head[80];
	if (D.text[0])
		snprintf(msg, sizeof msg, "%s", D.text);
	else if (code > 0)
		snprintf(msg, sizeof msg, "The command failed with status %d.", code);
	else
		snprintf(msg, sizeof msg, "The command did not finish.");
	snprintf(head, sizeof head, "%s did not work", power[D.what].verb);
	dlg_error(D.p, head, msg);
}

static void power_run(void)
{
	int e[2], s[2], what = D.what;
	D.text[0] = 0;
	if (pipe2(e, O_CLOEXEC) < 0)
		return dlg_error(D.p, "Could not run it", "No pipe for the command.");
	if (pipe2(s, O_CLOEXEC) < 0) {
		close(e[0]);
		close(e[1]);
		return dlg_error(D.p, "Could not run it", "No pipe for the command.");
	}
	pid_t pid = fork();
	if (pid < 0) {
		close(e[0]);
		close(e[1]);
		close(s[0]);
		close(s[1]);
		return dlg_error(D.p, "Could not run it", "fork failed.");
	}
	if (pid == 0) {
		int dn = open("/dev/null", O_RDWR);
		signal(SIGCHLD, SIG_DFL);
		if (dn >= 0) {
			dup2(dn, 0);
			dup2(dn, 1);
		}
		dup2(e[1], 2);
		dup2(s[1], 3);
		{ const char *h_ = getenv("HOME"); if (!h_ || !*h_ || chdir(h_) != 0) (void)!chdir("/"); }
		execl("/bin/sh", "sh", "-c", "eval \"$1\"; r=$?; printf '%d' \"$r\" >&3; exit \"$r\"", "sh",
		      power[what].cmd, (char *)NULL);
		_exit(127);
	}
	close(e[1]);
	close(s[1]);
	D.err_fd = e[0];
	D.st_fd = s[0];
	if (fl_fd_add(D.p->app, D.err_fd, POLLIN, on_out, NULL) < 0) {
		close(D.err_fd);
		close(D.st_fd);
		D.err_fd = D.st_fd = -1;
		return dlg_error(D.p, "Could not run it", "The panel cannot watch the command.");
	}
	D.running = 1;
	fl_timer_arm(D.tick, 0, 0);
	if (D.w)
		fl_win_redraw(D.w);
}

static void open_window(const char *title, int h, int kind)
{
	fl_win_opts o = {
		.title = title, .app_id = "flpanel-dialog", .w = DLG_W + 2, .h = h + fl_lk.title_h + 3, .decorated = 1, .resizable = 0,	/* the size is the whole window: title bar and border come on top of the content */
		.draw = dlg_draw, .close = on_close, .ud = NULL,
	};
	snprintf(D.title, sizeof D.title, "%s", title);
	D.kind = kind;
	D.w = fl_win_new(D.p->app, &o);
	if (!D.w)
		dlg_destroy();
}

static void ensure(struct panel *p)
{
	dlg_destroy();
	D.p = p;
	if (!D.tick)
		D.tick = fl_timer_new(p->app, on_tick, NULL);
	if (!D.later)
		D.later = fl_timer_new(p->app, on_later, NULL);
}

void dlg_power(struct panel *p, int what)
{
	if (what < 0 || what > POW_LOGOUT)
		return;
	ensure(p);
	D.what = what;
	D.left = COUNT_S;
	open_window(power[what].verb, POW_H, DLG_POWER);
	if (!D.w)
		return;
	fl_timer_arm(D.tick, 1000, 1000);
	fl_win_redraw(D.w);
}

void dlg_error(struct panel *p, const char *title, const char *text)
{
	char keep[TEXT_MAX], head[64];
	snprintf(keep, sizeof keep, "%s", text ? text : "");
	snprintf(head, sizeof head, "%s", title ? title : "Error");
	ensure(p);
	snprintf(D.text, sizeof D.text, "%s", keep);
	D.what = -1;
	open_window(head, ERR_H, DLG_ERROR);
	if (D.w)
		fl_win_redraw(D.w);
}

void dlg_name(struct panel *p, const char *title, const char *initial, int kind, const char *ctx, dlg_name_cb done)
{
	ensure(p);
	D.done = done;
	D.ctx[0] = 0;
	if (ctx)
		snprintf(D.ctx, sizeof D.ctx, "%s", ctx);
	snprintf(D.field, sizeof D.field, "%s", initial ? initial : "");
	D.what = kind;
	open_window(title ? title : "Name", NAME_H, DLG_NAME);
	if (!D.w)
		return;
	fl_win_ui(D.w)->focus = ID_FIELD;
	fl_win_redraw(D.w);
}

static int OX, OY;	/* the content origin of the window being drawn (below the title bar) */
static fl_rect R(int x, int y, int w, int h)
{
	return fl_r(x + OX, y + OY, w, h);
}

static void dlg_draw(fl_ui *ui, void *ud)
{
	(void)ud;
	fl_canvas *cv = &ui->cv;
	fl_rect cr = fl_win_content(ui->win);	/* the drawing starts under the title bar */
	OX = cr.x;
	OY = cr.y;
	int h = D.kind == DLG_POWER ? POW_H : D.kind == DLG_ERROR ? ERR_H : NAME_H;
	fl_fill_rect(cv, fl_r(0, 0, cv->w, cv->h), fl_th.panel);
	fl_fill_rect(cv, R(0, 0, DLG_W, 2), fl_th.accent);
	int by = h - 20 - BTN_H;
	int esc = ui->key == XKB_KEY_Escape;
	int enter = ui->key == XKB_KEY_Return || ui->key == XKB_KEY_KP_Enter;
	if (esc || enter)
		ui->key = 0;

	if (D.kind == DLG_POWER) {
		char head[80];
		if (D.running)
			snprintf(head, sizeof head, "Running %s", power[D.what].verb);
		else
			snprintf(head, sizeof head, power[D.what].head, D.left);
		fl_text_in(cv, ui->f_semi, R(20, 22, DLG_W - 40, 26), FL_LEFT, 0, fl_th.text, head);
		fl_text_in(cv, ui->f_reg, R(20, 58, DLG_W - 40, 40), FL_LEFT, 0, fl_th.dim,
		           D.running ? "Waiting for the system to answer." : "Unsaved work in open windows is lost.");
		if (D.running)
			return;
		int aw = fl_text_width(ui->f_reg, power[D.what].now, -1) + 28;
		if (fl_button(ui, ID_CANCEL, R(DLG_W - 20 - aw - 8 - 104, by, 104, BTN_H), "Cancel", FL_BTN_PLAIN) || esc) {
			D.act = ACT_CLOSE;
			fl_timer_arm(D.later, 1, 0);
			return;
		}
		if (fl_button(ui, ID_GO, R(DLG_W - 20 - aw, by, aw, BTN_H), power[D.what].now, FL_BTN_ACCENT) || enter) {
			D.act = ACT_GO;
			fl_timer_arm(D.later, 1, 0);
		}
	} else if (D.kind == DLG_ERROR) {
		fl_text_in(cv, ui->f_semi, R(20, 18, DLG_W - 40, 24), FL_LEFT, 0, fl_th.err, D.title);
		/* up to four lines, each cut to the width */
		char rest[TEXT_MAX], *line = rest;
		snprintf(rest, sizeof rest, "%s", D.text);
		for (int i = 0; i < 4 && line && *line; i++) {
			char *nl = strchr(line, '\n');
			if (nl)
				*nl = 0;
			char fit[200];
			fl_text_fit(ui->f_reg, line, DLG_W - 40, fit, sizeof fit);
			fl_text_in(cv, ui->f_reg, R(20, 52 + i * 20, DLG_W - 40, 20), FL_LEFT, 0, fl_th.text, fit);
			line = nl ? nl + 1 : NULL;
		}
		if (fl_button(ui, ID_GO, R(DLG_W - 20 - 96, by, 96, BTN_H), "OK", FL_BTN_ACCENT) || esc || enter) {
			D.act = ACT_CLOSE;
			fl_timer_arm(D.later, 1, 0);
		}
	} else if (D.kind == DLG_NAME) {
		int ev = fl_textfield(ui, ID_FIELD, R(20, 20, DLG_W - 40, 34), D.field, sizeof D.field, NULL, ui->f_reg);
		if (fl_button(ui, ID_CANCEL, R(DLG_W - 20 - 96 - 8 - 96, by, 96, BTN_H), "Cancel", FL_BTN_PLAIN) || esc) {
			D.act = ACT_CLOSE;
			fl_timer_arm(D.later, 1, 0);
			return;
		}
		if (fl_button(ui, ID_GO, R(DLG_W - 20 - 96, by, 96, BTN_H), "OK", FL_BTN_ACCENT) || enter || (ev & FL_EDIT_ENTER)) {
			D.act = ACT_GO;
			fl_timer_arm(D.later, 1, 0);
		}
	}
}
