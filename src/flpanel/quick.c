/* quick.c -- the tray's Bluetooth and night light icons, and their flyouts
 *
 * Bluetooth: the icon is there only when an adapter is (an entry in /sys/class/bluetooth, read
 * every 5 s with the tray: a directory listing). Its state comes from bluetoothctl, run by sh in a
 * child process and read from a pipe, never waited for: when an adapter appears, after the switch
 * is thrown, and every 5 s while the flyout is open. The flyout has the switch (bluetoothctl
 * power on|off), the connected devices and "Bluetooth settings..." (flsettings --page bluetooth).
 * Night light: the night_light key in desktop.conf (1 or 0), the key flsettings writes and flcomp
 * watches; the moon icon shows it and opens the quick flyout, which has the same switch and
 * "Display settings..." (flsettings --page display). There is no fldisplay program: the key is
 * written directly.
 * Keys: Esc closes a flyout. Clicks outside close it (flyout_dismissed, panel_flyouts_close).
 * SPDX-License-Identifier: MIT
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <flui.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "flpanel.h"

#define BT_W 260
#define BT_H 180
#define QS_W 260
#define QS_H 112
#define BT_ROWS 4
#define BT_ROW_H 22
#define ICON_BOX 34 /* the tray button, as the others */
#define BUF_MAX 4096

/* ids inside the panel window: the tray has 1 to 6 (panel.c) */
enum { QID_BT = 7, QID_NIGHT = 8 };
/* ids inside the two flyouts */
enum { ID_BT_SW = 1, ID_BT_SET, ID_QS_NIGHT, ID_QS_DISP };
enum { QJ_NONE, QJ_QUERY, QJ_POWER };

static struct {
	struct panel *p;
	fl_win *bw, *qw;	/* the Bluetooth flyout, the quick flyout */
	fl_timer *kick;		/* starts the query after a power change, from a timer */
	int bt_present, bt_known, bt_on, ndev, more;
	char dev[BT_ROWS][64];
	int bt_fly, qs_fly, bt_cx, qs_cx;
	int fd, pid, job;	/* the one bluetoothctl child, its pipe */
	char buf[BUF_MAX];
	size_t blen;
	int demo;
} Q = { .fd = -1 };

static void redraw_all(void)
{
	if (Q.p && Q.p->pw)
		fl_win_redraw(Q.p->pw);
	if (Q.bt_fly && Q.bw)
		fl_win_redraw(Q.bw);
	if (Q.qs_fly && Q.qw)
		fl_win_redraw(Q.qw);
}

/* ---- the adapter and bluetoothctl ---------------------------------------------------------------- */

static int adapter_present(void)
{
	DIR *d = opendir("/sys/class/bluetooth");
	int any = 0;
	struct dirent *e;
	while (d && (e = readdir(d)))
		if (e->d_name[0] != '.') {
			any = 1;
			break;
		}
	if (d)
		closedir(d);
	return any;
}

static void job_ready(void *ud, int fd, short rev);

/* one child at a time; 0 when it did not start (busy, or no pipe) */
static int job_start(const char *cmd, int job)
{
	if (Q.job != QJ_NONE || Q.fd >= 0 || !Q.p)
		return 0;
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) < 0)
		return 0;
	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return 0;
	}
	if (pid == 0) {
		int dn = open("/dev/null", O_RDWR | O_CLOEXEC);
		if (dn >= 0) {
			dup2(dn, 0);
			dup2(dn, 2);
		}
		dup2(fds[1], 1);
		signal(SIGCHLD, SIG_DFL);
		{ const char *h_ = getenv("HOME"); if (!h_ || !*h_ || chdir(h_) != 0) (void)!chdir("/"); }
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	if (fl_fd_add(Q.p->app, fds[0], POLLIN, job_ready, NULL) < 0) {
		close(fds[0]);
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return 0;
	}
	Q.fd = fds[0];
	Q.pid = pid;
	Q.job = job;
	Q.blen = 0;
	return 1;
}

/* "\tPowered: yes" and "Device AA:BB:CC:DD:EE:FF Name" lines, from the query */
static void parse_query(void)
{
	Q.bt_known = 0;
	Q.ndev = 0;
	Q.more = 0;
	char *ln = Q.buf;
	while (ln && *ln) {
		char *nl = strchr(ln, '\n');
		if (nl)
			*nl = 0;
		char *s = ln;
		while (*s == ' ' || *s == '\t')
			s++;
		if (!strncmp(s, "Powered:", 8)) {
			Q.bt_known = 1;
			Q.bt_on = strstr(s, "yes") != NULL;
		} else if (!strncmp(s, "Device ", 7)) {
			if (Q.ndev < BT_ROWS) {
				const char *nm = strchr(s + 7, ' ');
				snprintf(Q.dev[Q.ndev], sizeof Q.dev[0], "%s", nm ? nm + 1 : s + 7);
				Q.ndev++;
			} else {
				Q.more++;
			}
		}
		ln = nl ? nl + 1 : NULL;
	}
}

static void job_ready(void *ud, int fd, short rev)
{
	(void)ud;
	(void)rev;
	char tmp[512];
	ssize_t n = read(fd, tmp, sizeof tmp);
	if (n == -1 && (errno == EINTR || errno == EAGAIN))
		return;
	if (n > 0) {
		size_t room = sizeof Q.buf - 1 - Q.blen;
		if (room > (size_t)n)
			room = (size_t)n;
		memcpy(Q.buf + Q.blen, tmp, room);
		Q.blen += room;
		Q.buf[Q.blen] = 0;
		return;
	}
	/* end of the output: the child is gone; reap it (a no-op when SIGCHLD is ignored) */
	fl_fd_remove(Q.p->app, fd);
	close(fd);
	Q.fd = -1;
	if (Q.pid > 0) {
		waitpid(Q.pid, NULL, 0);
		Q.pid = 0;
	}
	int job = Q.job;
	Q.job = QJ_NONE;
	if (job == QJ_QUERY) {
		Q.buf[Q.blen] = 0;
		parse_query();
	} else if (job == QJ_POWER) {
		fl_timer_arm(Q.kick, 1, 0); /* ask again once the change has had time: from the timer, not here */
	}
	redraw_all();
}

static void query(void)
{
	job_start("{ bluetoothctl show | grep Powered; bluetoothctl devices Connected; } 2>/dev/null", QJ_QUERY);
}

static void kick_cb(void *ud)
{
	(void)ud;
	if (Q.bt_present)
		query();
}

static void power(int on)
{
	int was = Q.bt_on;
	Q.bt_on = on;	/* shown at once; the query after the change corrects it */
	if (!job_start(on ? "bluetoothctl power on >/dev/null 2>&1" : "bluetoothctl power off >/dev/null 2>&1", QJ_POWER))
		Q.bt_on = was;
	redraw_all();
}

/* ---- flyouts ------------------------------------------------------------------------------------- */

static void bt_close(void)
{
	if (!Q.bt_fly)
		return;
	Q.bt_fly = 0;
	if (Q.bw)
		panel_hide(Q.bw);
	if (Q.p && Q.p->pw)
		fl_win_redraw(Q.p->pw);
}

static void qs_close(void)
{
	if (!Q.qs_fly)
		return;
	Q.qs_fly = 0;
	if (Q.qw)
		panel_hide(Q.qw);
	if (Q.p && Q.p->pw)
		fl_win_redraw(Q.p->pw);
}

static void bt_draw(fl_ui *ui, void *ud);
static void qs_draw(fl_ui *ui, void *ud);

/* the flyouts' windows are made on first use: an idle panel holds no buffers for them */
static void ensure_bt(void)
{
	if (Q.bw || !Q.p)
		return;
	fl_win_opts bo = {
		.title = "bluetooth", .app_id = "flpanel-bluetooth", .w = BT_W, .h = BT_H, .raw = 1,
		.draw = bt_draw, .ud = NULL,
	};
	Q.bw = fl_win_new(Q.p->app, &bo);
}

static void ensure_qs(void)
{
	if (Q.qw || !Q.p)
		return;
	fl_win_opts qo = {
		.title = "quick settings", .app_id = "flpanel-quick", .w = QS_W, .h = QS_H, .raw = 1,
		.draw = qs_draw, .ud = NULL,
	};
	Q.qw = fl_win_new(Q.p->app, &qo);
}

static void bt_open(void)
{
	struct panel *p = Q.p;
	ensure_bt();
	if (Q.bt_fly || !Q.bw || !p || !p->shell || p->shell_ver < 2)
		return;
	Q.bt_fly = 1;
	panel_open_flyout(p, FLY_BT, Q.bw, Q.bt_cx);
	query(); /* what it shows is the state now */
	redraw_all();
}

static void qs_open(void)
{
	struct panel *p = Q.p;
	ensure_qs();
	if (Q.qs_fly || !Q.qw || !p || !p->shell || p->shell_ver < 2)
		return;
	Q.qs_fly = 1;
	panel_open_flyout(p, FLY_QS, Q.qw, Q.qs_cx);
	redraw_all();
}

void quick_flyouts_close(struct panel *p, int except)
{
	(void)p;
	if (except != FLY_BT)
		bt_close();
	if (except != FLY_QS)
		qs_close();
}

static void run_settings(const char *cmd)
{
	struct panel *p = Q.p;
	bt_close();
	qs_close();
	if (p)
		launch(cmd, 0);
}

static int night_on(void)
{
	/* nightlight= (written by fldisplay and Settings) wins over the older night_light=; auto is on
	 * only inside its span, which flcomp knows: the toggle shows the stored choice */
	const char *n = fl_conf_get("nightlight", "");
	if (*n)
		return strcmp(n, "off") != 0 && strcmp(n, "0") != 0;
	return fl_conf_int("night_light", 0) != 0;
}

static void night_set(int on)
{
	/* fldisplay writes nightlight= and night_light=; flcomp watches desktop.conf and fades */
	if (!system(on ? "fldisplay nightlight on" : "fldisplay nightlight off"))
		;
	redraw_all();
}

/* ---- drawing -------------------------------------------------------------------------------------- */

static void flyout_frame(fl_canvas *cv, int w, int h)
{
	fl_fill_rect(cv, fl_r(0, 0, w, h), fl_th.panel);
	fl_fill_rect(cv, fl_r(0, 0, 1, h), fl_th.line);
	fl_fill_rect(cv, fl_r(w - 1, 0, 1, h), fl_th.line);
	fl_fill_rect(cv, fl_r(0, 0, w, 2), fl_th.accent);
}

static void bt_draw(fl_ui *ui, void *ud)
{
	(void)ud;
	fl_canvas *cv = &ui->cv;
	flyout_frame(cv, BT_W, BT_H);
	if (ui->key == XKB_KEY_Escape) {
		ui->key = 0;
		bt_close();
		return;
	}
	fl_text(cv, Q.p->f_cap, 12, 14, fl_th.dim, "Bluetooth", -1);
	int on = Q.bt_on;
	if (fl_switch(ui, ID_BT_SW, fl_r(BT_W - 12 - 40, 12, 40, 20), &on))
		power(on);
	fl_fill_rect(cv, fl_r(12, 44, BT_W - 24, 1), fl_th.line);

	/* the devices: what the state is, or the connected ones */
	int y = 52;
	if (!Q.bt_known) {
		fl_text_in(cv, ui->f_reg, fl_r(12, y, BT_W - 24, BT_ROW_H), FL_LEFT, 0, fl_th.dim,
		           Q.fd >= 0 ? "Checking..." : "No answer from Bluetooth");
	} else if (!Q.bt_on) {
		fl_text_in(cv, ui->f_reg, fl_r(12, y, BT_W - 24, BT_ROW_H), FL_LEFT, 0, fl_th.dim, "Bluetooth is off");
	} else if (Q.ndev == 0) {
		fl_text_in(cv, ui->f_reg, fl_r(12, y, BT_W - 24, BT_ROW_H), FL_LEFT, 0, fl_th.dim, "No devices connected");
	} else {
		for (int i = 0; i < Q.ndev; i++, y += BT_ROW_H) {
			char fit[120];
			fl_text_fit(ui->f_reg, Q.dev[i], BT_W - 120, fit, sizeof fit);
			fl_text_in(cv, ui->f_reg, fl_r(12, y, BT_W - 24, BT_ROW_H), FL_LEFT, 0, fl_th.text, fit);
			fl_text_in(cv, Q.p->f_small, fl_r(12, y, BT_W - 24, BT_ROW_H), FL_RIGHT, 0, fl_th.dim, "connected");
		}
		if (Q.more)
			fl_text_in(cv, Q.p->f_small, fl_r(12, y, BT_W - 24, BT_ROW_H), FL_LEFT, 0, fl_th.dim, "and more");
	}
	fl_fill_rect(cv, fl_r(12, 140, BT_W - 24, 1), fl_th.line);

	fl_rect sr = fl_r(1, 144, BT_W - 2, 32);
	int res = fl_widget(ui, ID_BT_SET, sr, FL_FOCUSABLE);
	if (res & (FL_HOVER | FL_DOWN))
		fl_fill_rect(cv, sr, fl_th.raised);
	fl_text_in(cv, ui->f_reg, fl_r(sr.x + 16, sr.y, sr.w - 32, sr.h), FL_LEFT, 0, fl_th.text, "Bluetooth settings\xe2\x80\xa6");
	if ((res & FL_CLICK) || ((res & FL_FOCUS) && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_space))) {
		ui->key = 0;
		run_settings("flsettings --page bluetooth");
	}
}

static void qs_draw(fl_ui *ui, void *ud)
{
	(void)ud;
	fl_canvas *cv = &ui->cv;
	flyout_frame(cv, QS_W, QS_H);
	if (ui->key == XKB_KEY_Escape) {
		ui->key = 0;
		qs_close();
		return;
	}
	fl_text(cv, Q.p->f_cap, 12, 14, fl_th.dim, "Quick settings", -1);
	fl_text_in(cv, ui->f_reg, fl_r(12, 36, QS_W - 80, 32), FL_LEFT, 0, fl_th.text, "Night light");
	int on = night_on();
	if (fl_switch(ui, ID_QS_NIGHT, fl_r(QS_W - 12 - 40, 42, 40, 20), &on))
		night_set(on);
	fl_fill_rect(cv, fl_r(12, 76, QS_W - 24, 1), fl_th.line);

	fl_rect dr = fl_r(1, 80, QS_W - 2, 32);
	int res = fl_widget(ui, ID_QS_DISP, dr, FL_FOCUSABLE);
	if (res & (FL_HOVER | FL_DOWN))
		fl_fill_rect(cv, dr, fl_th.raised);
	fl_text_in(cv, ui->f_reg, fl_r(dr.x + 16, dr.y, dr.w - 32, dr.h), FL_LEFT, 0, fl_th.text, "Display settings\xe2\x80\xa6");
	if ((res & FL_CLICK) || ((res & FL_FOCUS) && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_space))) {
		ui->key = 0;
		run_settings("flsettings --page display");
	}
}

/* the Bluetooth symbol and a crescent moon, 24-unit paths scaled into the box */
static void draw_bt_icon(fl_canvas *cv, int x, int y, int size, fl_color c)
{
	fl_path *pth = fl_path_new();
	if (!pth)
		return;
	float s = (float)size / 24.0f;
	if (fl_path_svg(pth, "M6.5 6.5L17.5 17.5L12 23V1L17.5 6.5L6.5 17.5", s, s, (float)x, (float)y) == 0)
		fl_path_stroke(cv, pth, 1.5f, c);
	fl_path_free(pth);
}

static void draw_moon_icon(fl_canvas *cv, int x, int y, int size, fl_color c)
{
	fl_path *pth = fl_path_new();
	if (!pth)
		return;
	float s = (float)size / 24.0f;
	if (fl_path_svg(pth, "M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z", s, s, (float)x, (float)y) == 0)
		fl_path_fill(cv, pth, c);
	fl_path_free(pth);
}

/* ---- the tray: called by the panel's draw pass, right to left, *x moves left ---------------------- */

void quick_tray(struct panel *p, fl_ui *ui, int *x)
{
	fl_canvas *cv = &ui->cv;
	int H = fl_lk.panel_h;
	if (Q.bt_present) {
		*x -= ICON_BOX;
		fl_rect r = fl_r(*x, 1 + (H - 1 - ICON_BOX) / 2, ICON_BOX, ICON_BOX);
		int res = fl_widget(ui, QID_BT, r, FL_FOCUSABLE);
		Q.bt_cx = r.x + r.w / 2;
		int open = Q.bt_fly;
		if ((res & (FL_HOVER | FL_DOWN)) || open)
			fl_fill_rect(cv, r, fl_th.raised);
		if (res & FL_FOCUS)
			fl_stroke_rect(cv, r, 1, fl_th.accent);
		fl_color c = (res & FL_HOVER) || open ? fl_th.text : Q.bt_known && Q.bt_on ? fl_th.text : fl_th.dim;
		draw_bt_icon(cv, r.x + (r.w - 16) / 2, r.y + 9, 16, c);
		if ((res & FL_CLICK) || ((res & FL_FOCUS) && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_space))) {
			ui->key = 0;
			if (Q.bt_fly)
				bt_close();
			else
				bt_open();
		}
		*x -= 4;
	}
	{
		*x -= ICON_BOX;
		fl_rect r = fl_r(*x, 1 + (H - 1 - ICON_BOX) / 2, ICON_BOX, ICON_BOX);
		int res = fl_widget(ui, QID_NIGHT, r, FL_FOCUSABLE);
		Q.qs_cx = r.x + r.w / 2;
		int open = Q.qs_fly;
		if ((res & (FL_HOVER | FL_DOWN)) || open)
			fl_fill_rect(cv, r, fl_th.raised);
		if (res & FL_FOCUS)
			fl_stroke_rect(cv, r, 1, fl_th.accent);
		/* accent while the night light is on: the same mark as the battery when charging */
		fl_color c = night_on() ? fl_th.accent : (res & FL_HOVER) || open ? fl_th.text : fl_th.dim;
		draw_moon_icon(cv, r.x + (r.w - 16) / 2, r.y + 9, 16, c);
		if ((res & FL_CLICK) || ((res & FL_FOCUS) && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_space))) {
			ui->key = 0;
			if (Q.qs_fly)
				qs_close();
			else
				qs_open();
		}
		*x -= 4;
	}
	(void)p;
}

/* ---- set up and the tray's timer ------------------------------------------------------------------ */

void quick_init(struct panel *p)
{
	Q.p = p;
	Q.kick = fl_timer_new(p->app, kick_cb, NULL);
	if (!p->demo && !fl_app_ppm(p->app)) {
		Q.bt_present = adapter_present();
		if (Q.bt_present)
			query();
	}
}

/* Called every 5 s with the tray. 1 when the Bluetooth icon appeared or went. While the flyout is open
 * the state is asked for at the same rate; otherwise only a new adapter causes a question. */
int quick_tick(struct panel *p)
{
	if (Q.demo || !Q.p)
		return 0;
	int present = adapter_present();
	int changed = present != Q.bt_present;
	Q.bt_present = present;
	if (!present) {
		Q.bt_known = 0;
		Q.ndev = 0;
		bt_close();
		return changed;
	}
	(void)p;
	if (changed || Q.bt_fly)
		query();
	return changed;
}

/* The demo (FLUI_PPM screenshots): a Bluetooth adapter with two devices, the flyouts the options ask for. */
void quick_demo(struct panel *p, int bt, int qs)
{
	Q.p = p;
	Q.demo = 1;
	Q.bt_present = 1;
	Q.bt_known = 1;
	Q.bt_on = 1;
	Q.ndev = 2;
	Q.more = 0;
	snprintf(Q.dev[0], sizeof Q.dev[0], "%s", "WH-1000XM4");
	snprintf(Q.dev[1], sizeof Q.dev[1], "%s", "Keyboard K380");
	Q.bt_cx = p->out_w - 232;
	Q.qs_cx = p->out_w - 110;
	if (bt) {
		ensure_bt();
		Q.bt_fly = 1;
		if (Q.bw)
			fl_win_map(Q.bw);
	}
	if (qs) {
		ensure_qs();
		Q.qs_fly = 1;
		if (Q.qw)
			fl_win_map(Q.qw);
	}
}
