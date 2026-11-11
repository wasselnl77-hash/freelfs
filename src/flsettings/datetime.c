/* datetime.c -- Date & time page: time zone on a world map, 24 h / 12 h, automatic time
 * SPDX-License-Identifier: MIT
 *
 * The map and the search come from libflui (worldmap.c). Choosing a zone runs
 * fltime-set zone NAME (a setuid helper for the group seat, src/fltime) which
 * points /etc/localtime at it and writes /etc/timezone. The panel and the other
 * programs notice through the tz_changed key in desktop.conf (the panel also calls
 * tzset() every time it draws its clock). Automatic time is the ntp service; the
 * switch runs fltime-set ntp on|off, the state is read from /etc/fltime.conf and
 * /run/fltime.status. A time set by hand goes through fltime-set time EPOCH.
 *
 * Environment for tests: FLTIME_SET (helper), FLTIME_CONF, FLTIME_STATUS,
 * FL_TZ_ETC (where /etc/localtime and /etc/timezone are read), FL_ZONEINFO, FL_NOW,
 * and for screenshots FLSETTINGS_TEST_MOUSE=x,y and FLSETTINGS_TEST_SEARCH=text.
 */
#include <errno.h>
#include <fcntl.h>
#include <flui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "datetime.h"
#include "pages.h"

enum {
	ID_SEARCH = 400, ID_POP = 410 /* .. 417 */, ID_MAP = 420, ID_H24 = 430, ID_H12, ID_AUTO, ID_SYNC, ID_DATE,
	ID_TIME, ID_SETTIME,
};
#define ROW 56

struct datetime {
	fl_app *app;
	fl_win *win;
	fl_font *head, *small, *big;
	fl_worldmap *map;
	fl_tzsearch search;
	char query[64];
	int zone;			/* the system's zone, -1 unknown */
	char msg[200];
	int msg_err;
	int ntp;			/* automatic time on */
	char ntp_line[160];
	char date[16], time[8];
	int fields_for;			/* minute the fields were last filled for */
	char tmsg[200];
	int tmsg_err;
	fl_timer *tick;
	int test_done;
};

static const char *conf_path(void)
{
	const char *e = getenv("FLTIME_CONF");
	return e && *e ? e : "/etc/fltime.conf";
}

static const char *status_path(void)
{
	const char *e = getenv("FLTIME_STATUS");
	return e && *e ? e : "/run/fltime.status";
}

static int ntp_enabled(void)
{
	FILE *f = fopen(conf_path(), "r");
	char line[256];
	int on = 1;
	while (f && fgets(line, sizeof line, f))
		if (!strncmp(line, "NTP=", 4))
			on = atoi(line + 4) != 0;
	if (f)
		fclose(f);
	return on;
}

static void ntp_refresh(struct datetime *d)
{
	d->ntp = ntp_enabled();
	d->ntp_line[0] = 0;
	if (!d->ntp) {
		snprintf(d->ntp_line, sizeof d->ntp_line, "Off: set the date and time yourself below");
		return;
	}
	FILE *f = fopen(status_path(), "r");
	char line[200], state[16] = "", server[96] = "";
	long long last = 0;
	while (f && fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "state=", 6))
			snprintf(state, sizeof state, "%.15s", line + 6);
		else if (!strncmp(line, "last=", 5))
			last = atoll(line + 5);
		else if (!strncmp(line, "server=", 7))
			snprintf(server, sizeof server, "%.95s", line + 7);
	}
	if (f)
		fclose(f);
	if (!strcmp(state, "synced")) {
		long ago = (long)(time(NULL) - (time_t)last);
		char when[48];
		if (ago < 90)
			snprintf(when, sizeof when, "just now");
		else if (ago < 5400)
			snprintf(when, sizeof when, "%ld minutes ago", (ago + 30) / 60);
		else if (ago < 129600)
			snprintf(when, sizeof when, "%ld hours ago", (ago + 1800) / 3600);
		else
			snprintf(when, sizeof when, "%ld days ago", ago / 86400);
		snprintf(d->ntp_line, sizeof d->ntp_line, "Synchronised %s with %s", when, server);
	} else if (!strcmp(state, "waiting")) {
		snprintf(d->ntp_line, sizeof d->ntp_line, "Waiting for the network to ask a time server");
	} else {
		snprintf(d->ntp_line, sizeof d->ntp_line, "On, but the time service is not running (yet)");
	}
}

/* runs the helper; 0 when it did it, else a message in err */
static int helper(const char *a1, const char *a2, char *err, size_t cap)
{
	const char *prog = getenv("FLTIME_SET");
	if (!prog || !*prog)
		prog = "/usr/bin/fltime-set";
	pid_t p = fork();
	if (p < 0) {
		snprintf(err, cap, "Cannot start the helper: %s", strerror(errno));
		return -1;
	}
	if (p == 0) {
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) {
			dup2(nul, 0);
			dup2(nul, 1);
			dup2(nul, 2);
		}
		execl(prog, prog, a1, a2, (char *)NULL);
		_exit(errno == ENOENT ? 127 : 126);
	}
	int st = 0;
	if (waitpid(p, &st, 0) < 0 || !WIFEXITED(st)) {
		snprintf(err, cap, "The helper did not finish.");
		return -1;
	}
	switch (WEXITSTATUS(st)) {
	case 0: return 0;
	case 126: snprintf(err, cap, "Needs administrator rights: the account has to be in the group seat."); break;
	case 127: snprintf(err, cap, "The helper fltime-set is not installed."); break;
	case 4: snprintf(err, cap, "Turn off automatic time first."); break;
	case 3: snprintf(err, cap, "That was not accepted."); break;
	default: snprintf(err, cap, "It did not work (the helper says no)."); break;
	}
	return -1;
}

/* tell the panel and the others: desktop.conf is watched */
static void nudge(const char *key)
{
	char b[32];
	snprintf(b, sizeof b, "%lld", (long long)time(NULL));
	fl_conf_set(key, b);
}

static void pick_zone(struct datetime *d, int idx)
{
	const fl_tzone *z = fl_tz_get(idx);
	if (!z || idx == d->zone)
		return;
	char err[160];
	if (helper("zone", z->name, err, sizeof err) == 0) {
		d->zone = idx;
		nudge("tz_changed");
		snprintf(d->msg, sizeof d->msg, "Time zone set to %s. Every program follows it.", z->city);
		d->msg_err = 0;
	} else {
		snprintf(d->msg, sizeof d->msg, "%s", err);
		d->msg_err = 1;
	}
}

static void fill_fields(struct datetime *d)
{
	struct tm tm;
	int idx = d->zone >= 0 ? d->zone : fl_tz_find("UTC");
	if (fl_tz_local(idx, fl_tz_time(), &tm, NULL, NULL, 0) < 0)
		return;
	strftime(d->date, sizeof d->date, "%Y-%m-%d", &tm);
	strftime(d->time, sizeof d->time, "%H:%M", &tm);
	d->fields_for = (int)(fl_tz_time() / 60);
}

static void minute_arm(struct datetime *d)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long ms = (60 - ts.tv_sec % 60) * 1000L - ts.tv_nsec / 1000000L + 40;
	fl_timer_arm(d->tick, (int)ms, 0);
}

static void on_tick(void *ud)
{
	struct datetime *d = ud;
	minute_arm(d);
	ntp_refresh(d);
	if (d->win)
		fl_win_redraw(d->win);
}

struct datetime *dt_new(fl_app *app, fl_font *head, fl_font *small)
{
	struct datetime *d = calloc(1, sizeof *d);
	if (!d)
		return NULL;
	d->app = app;
	d->head = head;
	d->small = small;
	d->big = fl_font_ui(500, 30);
	d->map = fl_worldmap_new();
	d->tick = fl_timer_new(app, on_tick, d);
	d->zone = fl_tz_system();
	ntp_refresh(d);
	return d;
}

void dt_free(struct datetime *d)
{
	if (!d)
		return;
	if (d->tick)
		fl_timer_free(d->tick);
	fl_worldmap_free(d->map);
	free(d);
}

void dt_set_win(struct datetime *d, fl_win *win)
{
	d->win = win;
}

void dt_show(struct datetime *d, int visible)
{
	if (visible) {
		d->zone = fl_tz_system();
		ntp_refresh(d);
		d->fields_for = 0;
		minute_arm(d);
	} else {
		fl_timer_arm(d->tick, 0, 0);
	}
}

/* ---- drawing ------------------------------------------------------------------------------- */

static int sw_row_text(fl_ui *ui, struct datetime *d, int x, int y, int maxw, const char *title, const char *what)
{
	pg_row_text(ui, d->small, x, y, maxw, title, what);
	return 0;
}

static void draw_info(fl_ui *ui, struct datetime *d, int x, int y, int w, int h24)
{
	const fl_tzone *z = fl_tz_get(d->zone);
	if (!z) {
		fl_text(&ui->cv, d->small, x, y + 8, fl_th.dim, "The time zone of this system is not one of the listed places.", -1);
		return;
	}
	struct tm tm;
	int off;
	char abbr[16], t[24], o[24], line[260], fit[260];
	fl_tz_local(d->zone, fl_tz_time(), &tm, &off, abbr, sizeof abbr);
	fl_tz_fmt_time(&tm, h24, t, sizeof t);
	fl_tz_fmt_offset(off, o, sizeof o);
	int tw = fl_text_width(d->big, t, -1);
	fl_text(&ui->cv, d->big, x, y + 2, fl_th.strong, t, -1);
	int tx = x + tw + 22, avail = w - tw - 22;
	if (z->country[0])
		snprintf(line, sizeof line, "%s, %s", z->city, z->country);
	else
		snprintf(line, sizeof line, "%s", z->city);
	fl_text_fit(ui->f_med, line, avail, fit, sizeof fit);
	int lh = fl_font_height(ui->f_med), sh = fl_font_height(d->small);
	int top = y + 2 + (fl_font_height(d->big) - lh - 3 - sh) / 2;
	fl_text(&ui->cv, ui->f_med, tx, top, fl_th.text, fit, -1);
	char day[40];
	strftime(day, sizeof day, "%A %-d %B", &tm);
	/* "Monday 9 November  .  CET  .  UTC+1  .  Europe/Berlin", without what repeats */
	int n = snprintf(line, sizeof line, "%s", day);
	if (strcmp(abbr, o))
		n += snprintf(line + n, sizeof line - (size_t)n, "  \xc2\xb7  %s", abbr);
	n += snprintf(line + n, sizeof line - (size_t)n, "  \xc2\xb7  %s", o);
	if (strcmp(z->name, z->city))
		snprintf(line + n, sizeof line - (size_t)n, "  \xc2\xb7  %s", z->name);
	fl_text_fit(d->small, line, avail, fit, sizeof fit);
	fl_text(&ui->cv, d->small, tx, top + lh + 3, fl_th.dim, fit, -1);
}

static int parse_fields(const char *date, const char *tm_s, struct tm *out)
{
	int Y, M, D, h, m;
	char junk;
	if (sscanf(date, "%d-%d-%d%c", &Y, &M, &D, &junk) != 3 || sscanf(tm_s, "%d:%d%c", &h, &m, &junk) != 2)
		return -1;
	if (Y < 2024 || Y > 2099 || M < 1 || M > 12 || D < 1 || D > 31 || h < 0 || h > 23 || m < 0 || m > 59)
		return -1;
	memset(out, 0, sizeof *out);
	out->tm_year = Y - 1900;
	out->tm_mon = M - 1;
	out->tm_mday = D;
	out->tm_hour = h;
	out->tm_min = m;
	return 0;
}

static void set_time(struct datetime *d)
{
	struct tm tm;
	d->tmsg_err = 1;
	if (parse_fields(d->date, d->time, &tm) < 0) {
		snprintf(d->tmsg, sizeof d->tmsg, "Write the date like 2026-11-10 and the time like 14:32.");
		return;
	}
	int copy = tm.tm_mday;
	int idx = d->zone >= 0 ? d->zone : fl_tz_find("UTC");
	time_t t = fl_tz_mktime(idx, &tm);
	if (!t || tm.tm_mday != copy) {
		snprintf(d->tmsg, sizeof d->tmsg, "That day does not exist.");
		return;
	}
	char arg[24], err[160];
	snprintf(arg, sizeof arg, "%lld", (long long)t);
	if (helper("time", arg, err, sizeof err) == 0) {
		d->tmsg_err = 0;
		snprintf(d->tmsg, sizeof d->tmsg, "The clock is set.");
		nudge("tz_changed");
		d->fields_for = 0;
	} else {
		snprintf(d->tmsg, sizeof d->tmsg, "%s", err);
	}
}

int dt_draw(struct datetime *d, fl_ui *ui, fl_rect c)
{
	int x = c.x + 28, w = c.w - 56, y = c.y + 24;
	int h24 = fl_conf_int("clock_24h", 1);

	/* screenshots: a pointer and a search text without a compositor */
	if (fl_app_ppm(d->app) && !d->test_done) {
		d->test_done = 1;
		const char *s = getenv("FLSETTINGS_TEST_SEARCH");
		if (s && *s) {
			snprintf(d->query, sizeof d->query, "%s", s);
			ui->focus = ID_SEARCH;
		}
	}
	if (fl_app_ppm(d->app) && getenv("FLSETTINGS_TEST_MOUSE")) {
		int tx, ty;
		if (sscanf(getenv("FLSETTINGS_TEST_MOUSE"), "%d,%d", &tx, &ty) == 2) {
			ui->mx = tx;
			ui->my = ty;
			ui->mouse_in = 1;
		}
	}

	pg_section(ui, d->head, x, y, w, "Time zone");
	y += 28;
	int picked = fl_tzsearch_field(ui, ID_SEARCH, fl_r(x, y, w, 36), &d->search, d->query, sizeof d->query);
	y += 36 + 10;

	int mw = w > 960 ? 960 : w; /* a map any bigger only costs time to draw */
	int mh = fl_worldmap_height(mw);
	fl_rect mr = fl_r(x + (w - mw) / 2, y, mw, mh);
	int mx = ui->mx, my = ui->my;
	if (fl_tzsearch_covers(&d->search, ui->mx, ui->my))
		ui->mx = ui->my = -10000; /* the list lies over the map */
	if (!fl_in(ui->cv.clip, ui->mx, ui->my))
		ui->mx = ui->my = -10000; /* scrolled out of the window */
	fl_worldmap_set_24h(d->map, h24);
	int hover = -1;
	int clicked = fl_worldmap_draw(ui, ID_MAP, mr, d->map, d->zone, &hover);
	ui->mx = mx;
	ui->my = my;
	int pop = fl_tzsearch_popup(ui, ID_POP, &d->search, d->query);
	if (pop >= 0)
		picked = pop;
	if (picked < 0)
		picked = clicked;
	if (picked >= 0)
		pick_zone(d, picked);
	y += mh + 14;

	draw_info(ui, d, x, y, w, h24);
	y += fl_font_height(d->big) + 12;
	if (d->msg[0])
		fl_text_in(&ui->cv, d->small, fl_r(x, y, w, 20), FL_LEFT, 0, d->msg_err ? fl_th.err : fl_th.dim, d->msg);
	else
		fl_text_in(&ui->cv, d->small, fl_r(x, y, w, 20), FL_LEFT, 0, fl_th.dim,
		           hover >= 0 ? "Click to use this place's time zone." : "Click the map or search for a city.");
	y += 20 + 24;

	pg_section(ui, d->head, x, y, w, "Clock");
	y += 28;
	sw_row_text(ui, d, x, y, w - 190, "Clock format", "How the time is written in the panel");
	if (pg_choice(ui, ID_H24, fl_r(x + w - 176, y + 12, 86, 32), "24-hour", h24) && !h24)
		fl_conf_set("clock_24h", "1");
	if (pg_choice(ui, ID_H12, fl_r(x + w - 86, y + 12, 86, 32), "12-hour", !h24) && h24)
		fl_conf_set("clock_24h", "0");
	y += ROW + 24;

	pg_section(ui, d->head, x, y, w, "Date and time");
	y += 28;
	pg_row_text(ui, d->small, x, y, w - 160, "Set time automatically", d->ntp_line);
	int on = d->ntp;
	if (d->ntp && fl_button(ui, ID_SYNC, fl_r(x + w - 140, y + 14, 88, 28), "Sync now", FL_BTN_OUTLINE)) {
		char err[160];
		if (helper("sync", NULL, err, sizeof err) == 0) {
			snprintf(d->tmsg, sizeof d->tmsg, "Asking a time server ...");
			d->tmsg_err = 0;
		} else {
			snprintf(d->tmsg, sizeof d->tmsg, "%s", err);
			d->tmsg_err = 1;
		}
	}
	if (fl_switch(ui, ID_AUTO, fl_r(x + w - 40, y + 18, 40, 20), &on)) {
		char err[160];
		if (helper("ntp", on ? "on" : "off", err, sizeof err) == 0) {
			ntp_refresh(d);
			d->tmsg[0] = 0;
			d->fields_for = 0;
		} else {
			snprintf(d->tmsg, sizeof d->tmsg, "%s", err);
			d->tmsg_err = 1;
		}
	}
	y += ROW;
	if (!d->ntp) {
		if (d->fields_for != (int)(fl_tz_time() / 60) && ui->focus != ID_DATE && ui->focus != ID_TIME)
			fill_fields(d);
		pg_row_text(ui, d->small, x, y, w - 310, "Date and time", "In the time zone chosen above");
		int e1 = fl_textfield(ui, ID_DATE, fl_r(x + w - 292, y + 10, 118, 36), d->date, sizeof d->date, "YYYY-MM-DD", ui->f_reg);
		int e2 = fl_textfield(ui, ID_TIME, fl_r(x + w - 166, y + 10, 68, 36), d->time, sizeof d->time, "HH:MM", ui->f_reg);
		int set = fl_button(ui, ID_SETTIME, fl_r(x + w - 90, y + 14, 90, 28), "Set", FL_BTN_ACCENT);
		if ((e1 & FL_EDIT_ENTER) || (e2 & FL_EDIT_ENTER) || set)
			set_time(d);
		y += ROW;
	}
	if (d->tmsg[0]) {
		fl_text_in(&ui->cv, d->small, fl_r(x, y, w, 20), FL_LEFT, 0, d->tmsg_err ? fl_th.err : fl_th.dim, d->tmsg);
		y += 24;
	}
	return y + 12 - c.y;
}
