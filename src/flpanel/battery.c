/* battery.c -- the battery: reading, the flyout, power saving, low battery warnings
 * SPDX-License-Identifier: MIT
 *
 * Reading: /sys/class/power_supply/BAT* (every battery added up, so two-battery laptops show
 * one level), AC from the Mains supplies. $FLPANEL_POWER_SUPPLY names another directory (tests).
 * The panel shows no battery icon at all when there is none.
 *
 * Time left comes from energy (or charge) divided by the rate, with the rate smoothed over
 * the last readings; the first one or two readings say "calculating".
 *
 * Power saving is desktop.conf power_saver=1: flcomp then dims the backlight to 70 % and
 * draws at most 30 frames a second (src/flcomp/power.c), and the panel runs
 * `flpower saver on`, which sets the CPU governor to powersave and the energy
 * preference to power (what it replaced is put back by `flpower saver off`). At the start of a session
 * with the setting on the panel applies the CPU part again, because the governor resets at boot.
 *
 * Warnings: at 10 % a notification offers "Turn on power saving"; at 5 % a critical one that stays
 * until the charger is plugged in. Each shows once per discharge (the flags reset above 15 %
 * or when charging starts).
 */
#include <dirent.h>
#include <flui.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "flpanel.h"
#include "perfmode.h"

#define BW 300
#define BH 244

enum { BS_UNKNOWN, BS_DISCHARGING, BS_CHARGING, BS_FULL, BS_NOTCHARGING };

struct battery {
	int present, pct, state, ac;
	double now, full, rate, watts, ema;
	int ema_n, secs;
	int saver, warned10, warned5;
	uint32_t low_id;
	fl_win *win;
	int open;
};

static const char *ps_dir(void)
{
	const char *e = getenv("FLPANEL_POWER_SUPPLY");
	return e && *e ? e : "/sys/class/power_supply";
}

static int read_num(const char *dir, const char *name, double *out)
{
	char path[400];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	double v;
	int ok = fscanf(f, "%lf", &v) == 1;
	fclose(f);
	if (!ok)
		return -1;
	*out = v;
	return 0;
}

static void read_str(const char *dir, const char *name, char *out, size_t cap)
{
	char path[400];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	out[0] = 0;
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	if (!fgets(out, (int)cap, f))
		out[0] = 0;
	fclose(f);
	out[strcspn(out, "\r\n")] = 0;
}

/* ---- power saving ---------------------------------------------------------------------------- */

static void run_flpower(const char *what)
{
	char cmd[64];
	snprintf(cmd, sizeof cmd, "flpower saver %s", what);
	launch(cmd, 0);
}

static void saver_set(struct panel *p, int on)
{
	struct battery *b = p->bt;
	if (!b || b->saver == on)
		return;
	b->saver = on;
	fl_conf_set("power_saver", on ? "1" : "0"); /* flcomp dims and caps the frame rate */
	if (!p->demo && !fl_app_ppm(p->app))
		run_flpower(on ? "on" : "off");
	fl_win_redraw(p->pw);
	if (b->win)
		fl_win_redraw(b->win);
}

void battery_action(struct panel *p, const char *key)
{
	if (!strcmp(key, "saver"))
		saver_set(p, 1);
}

int battery_saver(const struct panel *p)
{
	return p->bt ? p->bt->saver : 0;
}

void battery_conf_changed(struct panel *p)
{
	struct battery *b = p->bt;
	if (!b)
		return;
	if (b->win && b->open) /* performance= may have changed too */
		fl_win_redraw(b->win);
	int on = fl_conf_int("power_saver", 0) != 0;
	if (on != b->saver) { /* changed from outside (Settings): follow, CPU part included */
		b->saver = on;
		if (!p->demo && !fl_app_ppm(p->app))
			run_flpower(on ? "on" : "off");
		fl_win_redraw(p->pw);
		if (b->win)
			fl_win_redraw(b->win);
	}
}

/* ---- reading ----------------------------------------------------------------------------------- */

static void warn(struct panel *p, struct battery *b)
{
	if (b->state != BS_DISCHARGING) {
		if (b->state == BS_CHARGING || b->state == BS_FULL || b->ac) {
			if (b->low_id) /* the charger is in: the warning is not true any more */
				notify_close(p, b->low_id, 3);
			b->low_id = 0;
			b->warned10 = b->warned5 = 0;
		}
		return;
	}
	if (b->pct > 15) {
		b->warned10 = b->warned5 = 0;
		return;
	}
	char body[160];
	if (b->pct <= 5 && !b->warned5) {
		b->warned5 = b->warned10 = 1;
		snprintf(body, sizeof body, "Battery is at %d%%. Plug in the charger now, or the computer will switch off soon.", b->pct);
		struct notify_req r = {.app = "Battery", .summary = "Battery critically low", .body = body, .urgency = 2,
		                       .timeout_ms = 0, .replaces = b->low_id, .internal = 1};
		b->low_id = notify_post(p, &r);
	} else if (b->pct <= 10 && !b->warned10) {
		b->warned10 = 1;
		snprintf(body, sizeof body, "Battery is at %d%%. Plug in the charger, or turn on power saving to make it last longer.", b->pct);
		struct notify_req r = {.app = "Battery", .summary = "Battery is low", .body = body, .urgency = 1,
		                       .timeout_ms = 15000, .internal = 1, .n_actions = 1,
		                       .akey = {"saver"}, .alabel = {"Turn on power saving"}};
		if (b->saver)
			r.n_actions = 0;
		b->low_id = notify_post(p, &r);
	}
}

int battery_read(struct panel *p)
{
	struct battery *b = p->bt;
	if (!b)
		return 0;
	int old_pct = p->bat_pct, old_chg = p->bat_charging, old_state = b->state, old_ac = b->ac, old_secs = b->secs;
	double e_now = 0, e_full = 0, rate = 0, volts_w = 0;
	int n = 0, cap_sum = 0, discharging = 0, charging = 0, full = 0, other = 0, have_energy = 0, ac = 0;
	DIR *d = opendir(ps_dir());
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char dir[400], type[32];
		snprintf(dir, sizeof dir, "%s/%s", ps_dir(), e->d_name);
		read_str(dir, "type", type, sizeof type);
		if (!strcmp(type, "Mains") || !strncmp(e->d_name, "AC", 2) || !strncmp(e->d_name, "ADP", 3)) {
			double on;
			if (read_num(dir, "online", &on) == 0 && on > 0)
				ac = 1;
			continue;
		}
		if (strncmp(e->d_name, "BAT", 3))
			continue;
		double cap;
		if (read_num(dir, "capacity", &cap) < 0)
			continue;
		double now, full_v, r1, volt;
		int ok = read_num(dir, "energy_now", &now) == 0 && read_num(dir, "energy_full", &full_v) == 0;
		if (ok) {
			if (read_num(dir, "power_now", &r1) < 0)
				r1 = 0;
			volts_w += r1 / 1e6;
		} else if (read_num(dir, "charge_now", &now) == 0 && read_num(dir, "charge_full", &full_v) == 0) {
			ok = 1;
			if (read_num(dir, "current_now", &r1) < 0)
				r1 = 0;
			if (read_num(dir, "voltage_now", &volt) == 0)
				volts_w += r1 * volt / 1e12;
		}
		if (ok && full_v > 0) {
			e_now += now;
			e_full += full_v;
			rate += r1 < 0 ? -r1 : r1;
			have_energy = 1;
		}
		cap_sum += (int)cap;
		n++;
		char st[32];
		read_str(dir, "status", st, sizeof st);
		if (!strcmp(st, "Discharging"))
			discharging++;
		else if (!strcmp(st, "Charging"))
			charging++;
		else if (!strcmp(st, "Full"))
			full++;
		else
			other++;
	}
	if (d)
		closedir(d);
	if (getenv("FLPANEL_NOBAT")) /* tests: the panel looks the same on every machine */
		n = 0;
	b->present = n > 0;
	if (!n) {
		p->bat_pct = -1;
		p->bat_charging = 0;
		b->state = BS_UNKNOWN;
		b->secs = -1;
		return old_pct != -1;
	}
	int pct = have_energy ? (int)lround(100.0 * e_now / e_full) : cap_sum / n;
	pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
	int st = discharging ? BS_DISCHARGING : charging ? BS_CHARGING : full == n ? BS_FULL : BS_NOTCHARGING;
	if (!discharging && !charging && !full && other)
		st = BS_UNKNOWN;
	if (st != b->state) {
		b->ema_n = 0;
		b->ema = 0;
	}
	b->state = st;
	b->ac = ac;
	b->pct = pct;
	b->now = e_now;
	b->full = e_full;
	b->watts = volts_w;
	if (rate > 0) {
		b->ema = b->ema_n ? b->ema * 0.7 + rate * 0.3 : rate;
		if (b->ema_n < 100)
			b->ema_n++;
	}
	b->secs = -1;
	if (have_energy && b->ema > 0 && b->ema_n >= 2) {
		if (st == BS_DISCHARGING)
			b->secs = (int)(e_now / b->ema * 3600.0);
		else if (st == BS_CHARGING)
			b->secs = (int)((e_full - e_now) / b->ema * 3600.0);
	}
	p->bat_pct = pct;
	p->bat_charging = st == BS_CHARGING;
	warn(p, b);
	int changed = old_pct != pct || old_chg != p->bat_charging || old_state != st || old_ac != ac ||
	              (b->secs / 60 != (old_secs < 0 ? -1 : old_secs / 60));
	if (changed && b->open && b->win)
		fl_win_redraw(b->win);
	return old_pct != pct || old_chg != p->bat_charging;
}

/* ---- the flyout --------------------------------------------------------------------------------------- */

static void fmt_time(int secs, char *out, size_t cap)
{
	int m = (secs + 30) / 60;
	if (m < 1)
		snprintf(out, cap, "less than a minute");
	else if (m < 60)
		snprintf(out, cap, "%d min", m);
	else if (m % 60)
		snprintf(out, cap, "%d h %d min", m / 60, m % 60);
	else
		snprintf(out, cap, "%d h", m / 60);
}

static void bat_flyout_draw(fl_ui *ui, void *ud)
{
	struct panel *p = ud;
	struct battery *b = p->bt;
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, fl_r(0, 0, BW, BH), fl_th.panel);
	fl_fill_rect(cv, fl_r(0, 0, 1, BH), fl_th.line);
	fl_fill_rect(cv, fl_r(BW - 1, 0, 1, BH), fl_th.line);
	fl_fill_rect(cv, fl_r(0, 0, BW, 2), fl_th.accent);
	if (ui->key == XKB_KEY_Escape) {
		ui->key = 0;
		battery_flyout_close(p);
		return;
	}
	fl_text(cv, p->f_cap, 16, 16, fl_th.dim, "Battery", -1);
	char big[16];
	snprintf(big, sizeof big, "%d%%", b->pct);
	fl_color c = b->state == BS_CHARGING ? fl_th.accent : b->pct <= 15 && b->state == BS_DISCHARGING ? fl_th.err : fl_th.strong;
	fl_text(cv, p->f_big, 16, 32, c, big, -1);
	const char *state = b->state == BS_DISCHARGING ? "On battery"
	                    : b->state == BS_CHARGING  ? "Charging"
	                    : b->state == BS_FULL      ? "Fully charged"
	                    : b->state == BS_NOTCHARGING ? "Plugged in, not charging"
	                                                 : "Status unknown";
	int sw = fl_text_width(ui->f_reg, state, -1);
	fl_text(cv, ui->f_reg, BW - 16 - sw, 46, fl_th.text, state, -1);

	/* the level bar */
	fl_rect br = fl_r(16, 78, BW - 32, 6);
	fl_fill_rect(cv, br, fl_th.line);
	fl_fill_rect(cv, fl_r(br.x, br.y, br.w * b->pct / 100, br.h), c == fl_th.strong ? fl_th.accent : c);
	/* what it means */
	char line[96] = "", tm[32];
	if (b->state == BS_DISCHARGING) {
		if (b->secs >= 0) {
			fmt_time(b->secs, tm, sizeof tm);
			snprintf(line, sizeof line, "%s left", tm);
		} else {
			snprintf(line, sizeof line, "Calculating time left...");
		}
	} else if (b->state == BS_CHARGING) {
		if (b->secs >= 0) {
			fmt_time(b->secs, tm, sizeof tm);
			snprintf(line, sizeof line, "%s until full", tm);
		} else {
			snprintf(line, sizeof line, "Calculating...");
		}
	} else if (b->ac) {
		snprintf(line, sizeof line, "Connected to power");
	}
	if (b->watts > 0.05 && (b->state == BS_DISCHARGING || b->state == BS_CHARGING)) {
		size_t l = strlen(line);
		snprintf(line + l, sizeof line - l, "%s%.1f W", l ? "  \xc2\xb7  " : "", b->watts);
	}
	fl_text(cv, ui->f_reg, 16, 94, fl_th.dim, line, -1);

	fl_fill_rect(cv, fl_r(0, 128, BW, 1), fl_th.line);
	fl_text(cv, ui->f_reg, 16, 142, fl_th.text, "Power saving", -1);
	fl_text(cv, p->f_small, 16, 142 + 19, fl_th.dim, "Slower CPU, dimmer screen, 30 frames a second", -1);
	int on = b->saver;
	if (fl_switch(ui, 2, fl_r(BW - 16 - 40, 138, 40, 24), &on))
		saver_set(p, on);
	perfmode_row(ui, p, 176, BW); /* performance mode (perfmode.c) */
}

void battery_flyout_close(struct panel *p)
{
	struct battery *b = p->bt;
	if (!b || !b->open)
		return;
	b->open = 0;
	if (b->win)
		panel_hide(b->win);
	fl_win_redraw(p->pw);
}

void battery_flyout_toggle(struct panel *p)
{
	struct battery *b = p->bt;
	if (!b || !b->win || !b->present || p->shell_ver < 2)
		return;
	if (b->open) {
		battery_flyout_close(p);
		return;
	}
	b->open = 1;
	panel_open_flyout(p, FLY_BAT, b->win, p->bat_cx);
	fl_win_ui(b->win)->focus = 2;
	fl_win_redraw(p->pw);
}

int battery_flyout_is_open(const struct panel *p)
{
	return p->bt && p->bt->open;
}

void battery_init(struct panel *p)
{
	struct battery *b = calloc(1, sizeof *b);
	if (!b)
		return;
	p->bt = b;
	b->secs = -1;
	b->saver = fl_conf_int("power_saver", 0) != 0;
	fl_win_opts o = {
		.title = "battery", .app_id = "flpanel-battery", .w = BW, .h = BH, .raw = 1,
		.draw = bat_flyout_draw, .ud = p,
	};
	b->win = fl_win_new(p->app, &o);
	if (b->saver && !p->demo && !fl_app_ppm(p->app))
		run_flpower("on"); /* the governor was reset at boot */
}

void battery_demo(struct panel *p, int open)
{
	struct battery *b = p->bt;
	if (!b)
		return;
	if (open < 0) { /* the plain demo: no battery window in the picture files */
		fl_win_destroy(b->win);
		b->win = NULL;
		return;
	}
	b->present = 1;
	b->pct = 72;
	b->state = BS_DISCHARGING;
	b->secs = 83 * 60;
	b->watts = 8.4;
	p->bat_pct = 72;
	if (open) {
		b->open = 1;
		fl_win_map(b->win);
	} else {
		fl_win_destroy(b->win);
		b->win = NULL;
	}
}
