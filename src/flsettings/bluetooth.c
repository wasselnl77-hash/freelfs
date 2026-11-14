/* bluetooth.c -- the Bluetooth page of flsettings
 * SPDX-License-Identifier: MIT
 *
 * Only bluetoothctl (BlueZ) is used: show, devices, info MAC, power on|off, --timeout 10 scan on,
 * pair, trust, connect, disconnect, remove. Each one is a helper sequence, so the window keeps drawing.
 * Refresh = show, then devices, then info for each device (paired, connected, alias, icon, battery).
 * Pair = pair, trust, connect. No adapter (show fails or bluetoothctl is missing): the page says so and
 * nothing else. Unpaired devices are listed only after a search in this visit of the page.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bluetooth.h"
#include "cmd.h"
#include "pages.h"

#define MAXDEV 32
#define ROW 56

enum {
	ID_BT_POWER = 1300, ID_BT_SEARCH = 1301, ID_BT_ACT = 1310 /* .. 1341 */, ID_BT_PAIR = 1350 /* .. 1381 */,
	ID_BT_FORGET = 1400 /* .. 1431 */, ID_BT_YES = 1440, ID_BT_NO,
};
enum { BT_LOADING, BT_NOADAPTER, BT_OK };
enum { KIND_OTHER, KIND_HEADPHONES, KIND_MOUSE, KIND_KEYBOARD, KIND_PHONE };
enum { JOB_NONE, JOB_SHOW, JOB_DEVICES, JOB_INFO, JOB_ACT, JOB_SCAN };

struct dev {
	char mac[18], name[48];
	int paired, connected, battery, kind;
};

struct bluetooth {
	fl_app *app;
	fl_font *head, *small;
	fl_win *win;
	int state, powered;
	struct dev dev[MAXDEV];
	int ndev;
	int scanned;		/* a search ran during this visit: the unpaired devices are listed */
	struct seq *job;
	int job_kind, info_i;	/* refresh: show, devices, then info for each device */
	char busy[96];
	int ask_forget;		/* the device whose Forget is being confirmed, -1 */
	char msg[320];
	int msg_err;
	fl_timer *tick;
	int phase;
};

static const char *const icon_svg[5] = {
	/* other: the Bluetooth rune */
	PG_SVG_BLUETOOTH,
	/* headphones */
	"M4 15v-3A8 8 0 0 1 20 12v3 M4 15h3v4H4z M17 15h3v4h-3z",
	/* mouse */
	"M7 2h10a4 4 0 0 1 4 4v12a4 4 0 0 1-4 4H7a4 4 0 0 1-4-4V6a4 4 0 0 1 4-4z M12 2v7",
	/* keyboard */
	"M2 6h20v12H2z M6 10h1 M11 10h2 M16 10h2 M6 14h12",
	/* phone */
	"M7 2h10v20H7z M11 18h2",
};

static int kind_of(const char *s)
{
	if (strstr(s, "headset") || strstr(s, "headphone") || strstr(s, "audio"))
		return KIND_HEADPHONES;
	if (strstr(s, "mouse"))
		return KIND_MOUSE;
	if (strstr(s, "keyboard"))
		return KIND_KEYBOARD;
	if (strstr(s, "phone"))
		return KIND_PHONE;
	return KIND_OTHER;
}

/* one bluetoothctl command; text_fail: its output saying "Failed" is a failure too */
static void mk(struct cmd_line *L, int text_fail, const char *a, const char *b, const char *c, const char *d)
{
	const char *w[5] = {"bluetoothctl", a, b, c, d};
	memset(L, 0, sizeof *L);
	L->text_fail = text_fail;
	for (int i = 0; i < 5; i++)
		if (w[i] && *w[i])
			snprintf(L->arg[L->n++], CMD_ARG_LEN, "%s", w[i]);
}

static void on_done(void *ud, int failed, const struct cmd_result *r);

static void start(struct bluetooth *b, int kind, const struct cmd_line *L, int n, int timeout, const char *busy)
{
	b->job_kind = kind;
	snprintf(b->busy, sizeof b->busy, "%s", busy ? busy : "");
	b->job = seq_start(b->app, L, n, timeout, on_done, b);
	if (!b->job) {
		b->job_kind = JOB_NONE;
		b->busy[0] = 0;
		snprintf(b->msg, sizeof b->msg, "Cannot start bluetoothctl.");
		b->msg_err = 1;
		if (kind == JOB_SHOW)
			b->state = BT_NOADAPTER;
	}
	fl_timer_arm(b->tick, 200, 200);
	if (b->win)
		fl_win_redraw(b->win);
}

static void start_refresh(struct bluetooth *b)
{
	struct cmd_line L;
	mk(&L, 0, "show", NULL, NULL, NULL);
	start(b, JOB_SHOW, &L, 1, 10000, "Looking at the Bluetooth adapter...");
}

static void start_info(struct bluetooth *b)
{
	struct cmd_line L;
	char busy[96];
	mk(&L, 0, "info", b->dev[b->info_i].mac, NULL, NULL);
	snprintf(busy, sizeof busy, "Reading %s...", b->dev[b->info_i].name);
	start(b, JOB_INFO, &L, 1, 10000, busy);
}

/* "Powered: yes" and the like */
static int has_yes(const char *text, const char *key)
{
	const char *p = strstr(text, key), *v;
	if (!p || !(v = strchr(p, ':')))
		return 0;
	v++;
	while (*v == ' ')
		v++;
	return !strncmp(v, "yes", 3);
}

/* "Device AA:BB:CC:DD:EE:FF Name": the list is rebuilt, what was known about a device is kept */
static void parse_devices(struct bluetooth *b, char *text)
{
	struct dev old[MAXDEV];
	int nold = b->ndev;
	char *p = text, *e;

	memcpy(old, b->dev, sizeof old);
	b->ndev = 0;
	while (p && *p && b->ndev < MAXDEV) {
		e = strchr(p, '\n');
		if (e)
			*e = 0;
		if (!strncmp(p, "Device ", 7) && strlen(p) >= 24 && p[9] == ':') {
			struct dev *d = &b->dev[b->ndev];
			memset(d, 0, sizeof *d);
			memcpy(d->mac, p + 7, 17);
			d->mac[17] = 0;
			snprintf(d->name, sizeof d->name, "%s", p[24] == ' ' ? p + 25 : d->mac);
			for (int i = 0; i < nold; i++)
				if (!strcmp(old[i].mac, d->mac)) {
					d->paired = old[i].paired;
					d->connected = old[i].connected;
					d->battery = old[i].battery;
					d->kind = old[i].kind;
				}
			b->ndev++;
		}
		if (!e)
			break;
		p = e + 1;
	}
}

/* the lines of "bluetoothctl info": Name, Alias, Paired, Connected, Icon, Battery Percentage */
static void parse_info(struct dev *d, char *text)
{
	char *p = text, *e;
	char alias[48] = "", icon[48] = "";
	d->paired = d->connected = 0;
	d->battery = -1;
	while (p && *p) {
		e = strchr(p, '\n');
		if (e)
			*e = 0;
		while (*p == ' ' || *p == '\t')
			p++;
		if (!strncmp(p, "Name: ", 6))
			snprintf(d->name, sizeof d->name, "%s", p + 6);
		else if (!strncmp(p, "Alias: ", 7))
			snprintf(alias, sizeof alias, "%s", p + 7);
		else if (!strncmp(p, "Paired: ", 8))
			d->paired = !strcmp(p + 8, "yes");
		else if (!strncmp(p, "Connected: ", 11))
			d->connected = !strcmp(p + 11, "yes");
		else if (!strncmp(p, "Icon: ", 6))
			snprintf(icon, sizeof icon, "%s", p + 6);
		else if (!strncmp(p, "Battery Percentage: ", 20) && strchr(p, '('))
			d->battery = atoi(strchr(p, '(') + 1);
		if (!e)
			break;
		p = e + 1;
	}
	if (alias[0])
		snprintf(d->name, sizeof d->name, "%s", alias);
	if (!d->name[0])
		memcpy(d->name, d->mac, sizeof d->mac);
	d->kind = kind_of(icon);
}

static void on_done(void *ud, int failed, const struct cmd_result *r)
{
	struct bluetooth *b = ud;
	struct cmd_line L;
	char m[160], *copy;
	int kind = b->job_kind;

	b->job = NULL;
	b->job_kind = JOB_NONE;
	switch (kind) {
	case JOB_SHOW:
		if (failed >= 0 || !strstr(r->out, "Controller")) {
			b->state = BT_NOADAPTER;
			b->ndev = 0;
			b->powered = 0;
			break;
		}
		b->state = BT_OK;
		b->powered = has_yes(r->out, "Powered");
		mk(&L, 0, "devices", NULL, NULL, NULL);
		start(b, JOB_DEVICES, &L, 1, 10000, "Looking for known devices...");
		return;
	case JOB_DEVICES:
		copy = strdup(r->out);
		if (copy && failed < 0)
			parse_devices(b, copy);
		free(copy);
		b->info_i = 0;
		if (b->ndev > 0) {
			start_info(b);
			return;
		}
		break;
	case JOB_INFO:
		copy = strdup(r->out);
		if (copy && failed < 0)
			parse_info(&b->dev[b->info_i], copy);
		free(copy);
		b->info_i++;
		if (b->info_i < b->ndev) {
			start_info(b);
			return;
		}
		break;
	case JOB_ACT:
		if (failed >= 0) {
			cmd_message(r, m, sizeof m);
			snprintf(b->msg, sizeof b->msg, "%s", m[0] ? m : "bluetoothctl failed.");
			b->msg_err = 1;
		} else {
			b->msg[0] = 0;
		}
		start_refresh(b);
		return;
	case JOB_SCAN:
		b->scanned = 1;
		if (failed >= 0) {
			cmd_message(r, m, sizeof m);
			snprintf(b->msg, sizeof b->msg, "The search did not work: %s", m);
			b->msg_err = 1;
		}
		start_refresh(b);
		return;
	}
	b->busy[0] = 0;
	if (b->win)
		fl_win_redraw(b->win);
}

static void tick_cb(void *ud)
{
	struct bluetooth *b = ud;
	b->phase++;
	if (b->win)
		fl_win_redraw(b->win);
	if (!b->job)
		fl_timer_arm(b->tick, 0, 0);
}

/* ---- drawing ------------------------------------------------------------------------------------ */

static const char *dev_state(const struct dev *d, char *buf, size_t n)
{
	if (d->battery >= 0)
		snprintf(buf, n, "%s, battery %d%%", d->connected ? "Connected" : "Not connected", d->battery);
	else
		snprintf(buf, n, "%s", d->connected ? "Connected" : "Not connected");
	return buf;
}

int bt_draw(struct bluetooth *b, fl_ui *ui, fl_rect c)
{
	int x = c.x + 28, w = c.w - 56, y = c.y + 24, on;
	char buf[96];

	if (b->state == BT_NOADAPTER) {
		fl_text_in(&ui->cv, ui->f_med, fl_r(x, y, w, ROW), FL_LEFT, 0, fl_th.text, "No Bluetooth adapter found");
		return y + ROW - c.y;
	}
	if (b->state == BT_LOADING) {
		pg_spinner(ui, x + 8, y + 20, b->phase);
		fl_text_in(&ui->cv, ui->f_reg, fl_r(x + 32, y + 8, w - 32, 32), FL_LEFT, 0, fl_th.dim,
		           "Looking for the Bluetooth adapter...");
		return y + 40 - c.y;
	}

	pg_section(ui, b->head, x, y, w, "Bluetooth");
	y += 28;
	pg_row_text(ui, b->small, x, y, w - 60, "Bluetooth", b->powered ? "On: other devices can find this computer" : "Off");
	on = b->powered;
	if (fl_switch(ui, ID_BT_POWER, fl_r(x + w - 40, y + 18, 40, 20), &on) && !b->job) {
		struct cmd_line L;
		mk(&L, 1, "power", on ? "on" : "off", NULL, NULL);
		start(b, JOB_ACT, &L, 1, 15000, on ? "Turning Bluetooth on..." : "Turning Bluetooth off...");
	}
	y += ROW + 24;
	if (!b->powered) {
		fl_text_in(&ui->cv, ui->f_reg, fl_r(x, y, w, 32), FL_LEFT, 0, fl_th.dim,
		           "Turn Bluetooth on to pair and connect devices.");
		y += 32 + 12;
	} else {
		int any = 0;

		pg_section(ui, b->head, x, y, w, "Paired devices");
		y += 28;
		for (int i = 0; i < b->ndev; i++) {
			struct dev *d = &b->dev[i];
			char what[96];
			if (!d->paired)
				continue;
			any = 1;
			pg_glyph(ui, icon_svg[d->kind], x + 4, y + 16, 24, fl_th.dim);
			if (b->ask_forget == i) {
				snprintf(what, sizeof what, "Remove the pairing with this computer?");
			} else {
				dev_state(d, what, sizeof what);
			}
			pg_row_text(ui, b->small, x + 44, y, w - 44 - 260, d->name, what);
			if (b->ask_forget == i) {
				if (fl_button(ui, ID_BT_YES, fl_r(x + w - 232, y + 12, 116, 32), "Forget", FL_BTN_ACCENT) && !b->job) {
					struct cmd_line L;
					snprintf(buf, sizeof buf, "Forgetting %s...", d->name);
					mk(&L, 1, "remove", d->mac, NULL, NULL);
					b->ask_forget = -1;
					start(b, JOB_ACT, &L, 1, 15000, buf);
				}
				if (fl_button(ui, ID_BT_NO, fl_r(x + w - 106, y + 12, 106, 32), "Cancel", FL_BTN_OUTLINE))
					b->ask_forget = -1;
			} else {
				if (fl_button(ui, ID_BT_ACT + (uint32_t)i, fl_r(x + w - 232, y + 12, 116, 32),
				              d->connected ? "Disconnect" : "Connect", FL_BTN_OUTLINE) && !b->job) {
					struct cmd_line L;
					snprintf(buf, sizeof buf, "%s %s...", d->connected ? "Disconnecting from" : "Connecting to", d->name);
					mk(&L, 1, d->connected ? "disconnect" : "connect", d->mac, NULL, NULL);
					start(b, JOB_ACT, &L, 1, 30000, buf);
				}
				if (fl_button(ui, ID_BT_FORGET + (uint32_t)i, fl_r(x + w - 106, y + 12, 106, 32), "Forget", FL_BTN_OUTLINE))
					b->ask_forget = i;
			}
			y += ROW;
		}
		if (!any) {
			fl_text_in(&ui->cv, ui->f_reg, fl_r(x, y, w, 32), FL_LEFT, 0, fl_th.dim, "No paired devices yet.");
			y += 32;
		}
		y += 24;

		pg_section(ui, b->head, x, y, w, "Nearby devices");
		y += 28;
		pg_row_text(ui, b->small, x, y, w - 200, "Search",
		            b->scanned ? "Devices that are not paired yet" : "Look for devices for 10 seconds");
		if (b->job_kind == JOB_SCAN)
			pg_spinner(ui, x + w - 150, y + 28, b->phase);
		if (fl_button(ui, ID_BT_SEARCH, fl_r(x + w - 120, y + 12, 120, 32), "Search", FL_BTN_OUTLINE) && !b->job) {
			struct cmd_line L;
			mk(&L, 0, "--timeout", "10", "scan", "on");
			start(b, JOB_SCAN, &L, 1, 20000, "Searching for 10 seconds...");
		}
		y += ROW;
		if (b->scanned) {
			int nany = 0;
			for (int i = 0; i < b->ndev; i++) {
				struct dev *d = &b->dev[i];
				if (d->paired)
					continue;
				nany = 1;
				pg_glyph(ui, icon_svg[d->kind], x + 4, y + 16, 24, fl_th.dim);
				pg_row_text(ui, b->small, x + 44, y, w - 44 - 140, d->name, d->mac);
				if (fl_button(ui, ID_BT_PAIR + (uint32_t)i, fl_r(x + w - 106, y + 12, 106, 32), "Pair", FL_BTN_ACCENT) && !b->job) {
					struct cmd_line L[3];
					snprintf(buf, sizeof buf, "Pairing with %s...", d->name);
					mk(&L[0], 1, "pair", d->mac, NULL, NULL);
					mk(&L[1], 1, "trust", d->mac, NULL, NULL);
					mk(&L[2], 1, "connect", d->mac, NULL, NULL);
					start(b, JOB_ACT, L, 3, 60000, buf);
				}
				y += ROW;
			}
			if (!nany) {
				fl_text_in(&ui->cv, ui->f_reg, fl_r(x, y, w, 32), FL_LEFT, 0, fl_th.dim,
				           "No devices found. Put the device in pairing mode and search again.");
				y += 32;
			}
		}
	}

	y += 16;
	if (b->job) {
		pg_spinner(ui, x + 8, y + 12, b->phase);
		fl_text_in(&ui->cv, b->small, fl_r(x + 32, y, w - 32, 24), FL_LEFT, 0, fl_th.dim, b->busy);
		y += 32;
	} else if (b->msg[0]) {
		fl_text_in(&ui->cv, b->small, fl_r(x, y, w, 24), FL_LEFT, 0, b->msg_err ? fl_th.err : fl_th.dim, b->msg);
		y += 32;
	}
	return y + 12 - c.y;
}

/* ---- the page's life ----------------------------------------------------------------------------- */

struct bluetooth *bt_new(fl_app *app, fl_font *head, fl_font *small)
{
	struct bluetooth *b = calloc(1, sizeof *b);
	if (!b)
		return NULL;
	b->app = app;
	b->head = head;
	b->small = small;
	b->ask_forget = -1;
	b->state = BT_LOADING;
	b->tick = fl_timer_new(app, tick_cb, b);
	if (!b->tick) {
		free(b);
		return NULL;
	}
	return b;
}

void bt_free(struct bluetooth *b)
{
	if (!b)
		return;
	seq_cancel(b->job);
	fl_timer_free(b->tick);
	free(b);
}

void bt_set_win(struct bluetooth *b, fl_win *win)
{
	b->win = win;
}

void bt_show(struct bluetooth *b, int visible)
{
	if (!b)
		return;
	if (!visible) {
		/* search results are not kept after the page is left */
		int j = 0;
		for (int i = 0; i < b->ndev; i++)
			if (b->dev[i].paired)
				b->dev[j++] = b->dev[i];
		b->ndev = j;
		b->scanned = 0;
		b->ask_forget = -1;
		return;
	}
	if (!b->job)
		start_refresh(b);
}
