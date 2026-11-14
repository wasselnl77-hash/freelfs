/* display.c -- the Display page of flsettings
 * SPDX-License-Identifier: MIT
 *
 * Screens are fldisplay's (src/fldisplay), run as helper commands that never block the window:
 *	fldisplay list
 *		one line per output: NAME, connected|disconnected, WxH@HZ or -, SCALE, X,Y, enabled|disabled,
 *		primary|-, then the modes (WxH@HZ, comma separated, preferred first); tab separated
 *	fldisplay set NAME [mode=WxH@HZ] [scale=1|1.25|1.5|1.75|2] [pos=X,Y] [enabled=0|1] [primary=1]
 *	fldisplay revert
 * Apply sets every output that changed (one sequence, stops at the first failure) and then counts down
 * 15 s: Keep leaves the change as it is; otherwise, on Revert, at the end of the countdown or when
 * Settings closes, fldisplay revert runs. The arrangement is in logical pixels (physical / scale).
 *
 * Night light is desktop.conf: nightlight=off|on|auto, nightlight_temp (2700..6500 K), nightlight_from
 * and nightlight_to (HH:MM). flcomp reads night_light=0|1 and night_warmth=0..100 today, so those two
 * are written along with them (warmth derived from the temperature). Brightness and the wallpaper come
 * from pages.c, below the screens.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cmd.h"
#include "display.h"
#include "pages.h"

#define MAXOUT 8
#define MAXMODE 48
#define ROW 56
#define VIEW_H 168
#define LIST_ROW 30
#define CONFIRM_S 15
#define SNAP 40

enum {
	ID_ARR = 1000 /* .. 1007 */, ID_ENABLED = 1010 /* .. 1017 */, ID_PRIMARY = 1020 /* .. 1027 */,
	ID_DD = 1030 /* .. 1037 */, ID_LIST = 1040 /* .. 1047 */, ID_SCALE = 1100 /* .. 1163 */,
	ID_KEEP = 1200, ID_REVERT, ID_APPLY, ID_RESET, ID_NL_OFF = 1210 /* .. 1212 */, ID_NL_TEMP = 1215,
	ID_NL_FROM, ID_NL_TO,
};
enum { JOB_NONE, JOB_LIST, JOB_SET, JOB_REVERT };

struct mode { int w, h, hz; };

struct output {
	char name[40];
	int connected, enabled, primary;
	int cw, ch, chz, cscale, cx, cy;		/* as fldisplay list says them now */
	int nmodes;
	struct mode modes[MAXMODE];
	int sw, sh, shz, sscale, sx, sy, senabled, sprimary;	/* what the page shows and would apply */
	int dd_open;
	fl_list_state dd;
};

struct display {
	fl_app *app;
	fl_font *head, *small;
	fl_win *win;
	struct pages *pg;
	fl_timer *tick;
	int phase;

	struct output out[MAXOUT];
	int nout;
	int listed;			/* fldisplay list answered at least once */
	char msg[320];
	int msg_err;

	struct seq *job;		/* one helper sequence at a time */
	int job_kind;

	int pending;			/* the countdown runs: seconds to keep, 0 when nothing is to be kept */
	long deadline;			/* ms, monotonic */

	/* arrangement: the mapping stays put while a screen is held */
	int drag, moved, press_x, press_y, drag_dx, drag_dy;
	float k, ox, oy;

	char nl_from[8], nl_to[8];
};

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* the window closed during a countdown: nothing was kept, so fldisplay revert runs (not waited for) */
static void revert_at_exit(void)
{
	pid_t p = fork();
	if (p == 0) {
		if (fork() == 0) {
			int nul = open("/dev/null", O_RDWR);
			if (nul >= 0) {
				dup2(nul, 0);
				dup2(nul, 1);
				dup2(nul, 2);
				if (nul > 2)
					close(nul);
			}
			execlp("fldisplay", "fldisplay", "revert", (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	if (p > 0)
		waitpid(p, NULL, 0);
}

/* ---- text and numbers ------------------------------------------------------------------------- */

static int round_i(float v)
{
	return (int)(v < 0 ? v - 0.5f : v + 0.5f);
}

static void mode_label(char *buf, size_t n, int w, int h, int hz)
{
	if (!w || !h)
		snprintf(buf, n, "no mode");
	else if (hz)
		snprintf(buf, n, "%d \xc3\x97 %d, %d Hz", w, h, hz);
	else
		snprintf(buf, n, "%d \xc3\x97 %d", w, h);
}

/* the fldisplay word for a scale in per cent: 100 -> 1, 125 -> 1.25 */
static void scale_arg(char *buf, size_t n, int pct)
{
	snprintf(buf, n, "%g", pct / 100.0);
}

/* the scales that leave at least 640 x 400 of the desktop (as pages.c did) */
static int scale_ok(int w, int h, int scale)
{
	return w * 100 / scale >= 640 && h * 100 / scale >= 400;
}

static int hhmm_ok(const char *s)
{
	if (strlen(s) != 5 || s[2] != ':')
		return 0;
	for (int i = 0; i < 5; i++)
		if (i != 2 && (s[i] < '0' || s[i] > '9'))
			return 0;
	return (s[0] - '0') * 10 + (s[1] - '0') < 24 && (s[3] - '0') * 10 + (s[4] - '0') < 60;
}

/* ---- parsing fldisplay list ---------------------------------------------------------------------- */

static int split_tabs(char *s, char **f, int max)
{
	int n = 0;
	f[n++] = s;
	for (char *p = s; *p && n < max; p++)
		if (*p == '\t') {
			*p = 0;
			f[n++] = p + 1;
		}
	return n;
}

/* the page's pending values start at what the screen has */
static void take_current(struct output *o)
{
	if (o->cw && o->ch) {
		o->sw = o->cw;
		o->sh = o->ch;
		o->shz = o->chz;
	} else if (o->nmodes) {
		o->sw = o->modes[0].w;
		o->sh = o->modes[0].h;
		o->shz = o->modes[0].hz;
	} else {
		o->sw = o->sh = o->shz = 0;
	}
	o->sscale = o->cscale ? o->cscale : 100;
	o->sx = o->cx;
	o->sy = o->cy;
	o->senabled = o->enabled;
	o->sprimary = o->primary;
}

static void parse_list(struct display *d, const char *text)
{
	char *copy = strdup(text ? text : ""), *p, *f[8], *e;
	if (!copy)
		return;
	d->nout = 0;
	p = copy;
	while (p && *p && d->nout < MAXOUT) {
		int nf;
		e = strchr(p, '\n');
		if (e)
			*e = 0;
		nf = p[0] ? split_tabs(p, f, 8) : 0;
		if (nf >= 6 && f[0][0]) {
			struct output *o = &d->out[d->nout];
			float sc = 1, hz = 0;
			memset(o, 0, sizeof *o);
			snprintf(o->name, sizeof o->name, "%s", f[0]);
			o->connected = !strcmp(f[1], "connected");
			if (sscanf(f[2], "%dx%d@%f", &o->cw, &o->ch, &hz) < 2)
				o->cw = o->ch = 0;
			o->chz = o->cw ? round_i(hz) : 0;
			if (sscanf(f[3], "%f", &sc) != 1 || sc <= 0)
				sc = 1;
			o->cscale = round_i(sc * 100);
			if (sscanf(f[4], "%d,%d", &o->cx, &o->cy) != 2)
				o->cx = o->cy = 0;
			o->enabled = !strcmp(f[5], "enabled");
			o->primary = nf > 6 && !strcmp(f[6], "primary");
			if (nf > 7) {
				char *q = f[7];
				while (*q && o->nmodes < MAXMODE) {
					int w, h;
					float hz = 0;
					if (sscanf(q, "%dx%d@%f", &w, &h, &hz) >= 2)
						o->modes[o->nmodes++] = (struct mode){w, h, round_i(hz)};
					q = strchr(q, ',');
					if (!q)
						break;
					q++;
				}
			}
			take_current(o);
			d->nout++;
		}
		if (!e)
			break;
		p = e + 1;
	}
	free(copy);
}

/* ---- the arrangement ----------------------------------------------------------------------------- */

/* logical size of an output as the page would apply it; 0 when it is not in the arrangement */
static int lay_size(const struct output *o, int *w, int *h)
{
	if (!o->connected || !o->senabled || !o->sw || !o->sh)
		return 0;
	*w = o->sw * 100 / o->sscale;
	*h = o->sh * 100 / o->sscale;
	return 1;
}

static int is_changed(const struct output *o)
{
	if (!o->connected)
		return 0;
	if (o->senabled != o->enabled || o->sprimary != o->primary)
		return 1;
	if (!o->senabled)
		return 0;
	return o->sw != o->cw || o->sh != o->ch || o->shz != o->chz || o->sscale != o->cscale ||
	       o->sx != o->cx || o->sy != o->cy;
}

/* fit every arrangeable output into v, centred */
static void layout_map(struct display *d, fl_rect v)
{
	int minx = 1 << 30, miny = 1 << 30, maxx = -(1 << 30), maxy = -(1 << 30), any = 0;
	float k, ky;
	int bw, bh;
	for (int i = 0; i < d->nout; i++) {
		const struct output *o = &d->out[i];
		int w, h;
		if (!lay_size(o, &w, &h))
			continue;
		any = 1;
		if (o->sx < minx)
			minx = o->sx;
		if (o->sy < miny)
			miny = o->sy;
		if (o->sx + w > maxx)
			maxx = o->sx + w;
		if (o->sy + h > maxy)
			maxy = o->sy + h;
	}
	if (!any) {
		d->k = 0.2f;
		d->ox = v.x + v.w / 2.0f;
		d->oy = v.y + v.h / 2.0f;
		return;
	}
	bw = maxx - minx < 1 ? 1 : maxx - minx;
	bh = maxy - miny < 1 ? 1 : maxy - miny;
	k = (v.w - 48) / (float)bw;
	ky = (v.h - 48) / (float)bh;
	if (ky < k)
		k = ky;
	if (k > 0.5f)
		k = 0.5f;
	if (k < 0.02f)
		k = 0.02f;
	d->k = k;
	d->ox = v.x + (v.w - bw * k) / 2 - minx * k;
	d->oy = v.y + (v.h - bh * k) / 2 - miny * k;
}

static fl_rect rect_of(const struct display *d, const struct output *o, int w, int h)
{
	return fl_r(round_i(d->ox + o->sx * d->k), round_i(d->oy + o->sy * d->k), round_i(w * d->k),
	            round_i(h * d->k));
}

/* Released screen: it snaps edge to edge with a screen it touches (within SNAP), on the axis where it
 * is near. Only the pending position changes; Apply sends it. */
static void snap_out(struct display *d, int i)
{
	struct output *o = &d->out[i];
	int w, h, x = o->sx, y = o->sy, bdx = SNAP + 1, bdy = SNAP + 1, bx = x, by = y;
	if (!lay_size(o, &w, &h))
		return;
	for (int j = 0; j < d->nout; j++) {
		const struct output *q = &d->out[j];
		int qw, qh, cand[2];
		if (j == i || !lay_size(q, &qw, &qh))
			continue;
		if (y < q->sy + qh + SNAP && q->sy < y + h + SNAP) {
			cand[0] = q->sx + qw;
			cand[1] = q->sx - w;
			for (int c = 0; c < 2; c++)
				if (abs(x - cand[c]) < bdx) {
					bdx = abs(x - cand[c]);
					bx = cand[c];
				}
		}
		if (x < q->sx + qw + SNAP && q->sx < x + w + SNAP) {
			cand[0] = q->sy + qh;
			cand[1] = q->sy - h;
			for (int c = 0; c < 2; c++)
				if (abs(y - cand[c]) < bdy) {
					bdy = abs(y - cand[c]);
					by = cand[c];
				}
		}
	}
	if (bdx <= SNAP)
		o->sx = bx;
	if (bdy <= SNAP)
		o->sy = by;
}

/* ---- helper jobs ----------------------------------------------------------------------------------- */

static void job_done(void *ud, int failed, const struct cmd_result *r);

static void add(struct cmd_line *c, const char *s)
{
	if (c->n < CMD_ARGS)
		snprintf(c->arg[c->n++], CMD_ARG_LEN, "%s", s);
}

/* fldisplay VERB [NAME] */
static void fl_line(struct cmd_line *L, const char *verb, const char *name)
{
	memset(L, 0, sizeof *L);
	add(L, "fldisplay");
	add(L, verb);
	if (name)
		add(L, name);
}

static void job_start(struct display *d, int kind, const struct cmd_line *lines, int n, int timeout)
{
	d->job_kind = kind;
	d->job = seq_start(d->app, lines, n, timeout, job_done, d);
	if (!d->job) {
		d->job_kind = JOB_NONE;
		snprintf(d->msg, sizeof d->msg, "Cannot start fldisplay.");
		d->msg_err = 1;
	}
	fl_timer_arm(d->tick, 250, 250);
	if (d->win)
		fl_win_redraw(d->win);
}

static void job_done(void *ud, int failed, const struct cmd_result *r)
{
	struct display *d = ud;
	int kind = d->job_kind;
	char m[160];
	struct cmd_line L;

	d->job = NULL;
	d->job_kind = JOB_NONE;
	if (kind == JOB_LIST) {
		if (failed >= 0) {
			d->listed = 0;
			d->nout = 0;
			cmd_message(r, m, sizeof m);
			if (r->spawn_errno == ENOENT)
				snprintf(d->msg, sizeof d->msg, "fldisplay is not installed.");
			else
				snprintf(d->msg, sizeof d->msg, "fldisplay list: %s", m);
			d->msg_err = 1;
		} else {
			parse_list(d, r->out);
			d->listed = 1;
		}
	} else if (kind == JOB_SET) {
		if (failed >= 0) {
			cmd_message(r, m, sizeof m);
			snprintf(d->msg, sizeof d->msg, "The display did not accept that: %s. Going back.", m);
			d->msg_err = 1;
			fl_line(&L, "revert", NULL);
			job_start(d, JOB_REVERT, &L, 1, 15000);
			return;
		}
		d->pending = CONFIRM_S;
		d->deadline = now_ms() + CONFIRM_S * 1000L;
		fl_line(&L, "list", NULL);
		job_start(d, JOB_LIST, &L, 1, 10000);
		return;
	} else if (kind == JOB_REVERT) {
		if (failed >= 0) {
			cmd_message(r, m, sizeof m);
			snprintf(d->msg, sizeof d->msg, "Revert failed: %s", m);
			d->msg_err = 1;
		}
		fl_line(&L, "list", NULL);
		job_start(d, JOB_LIST, &L, 1, 10000);
		return;
	}
	if (d->win)
		fl_win_redraw(d->win);
}

static void tick_cb(void *ud)
{
	struct display *d = ud;
	struct cmd_line L;
	d->phase++;
	if (d->pending && d->deadline - now_ms() <= 0 && !d->job) {
		d->pending = 0;
		snprintf(d->msg, sizeof d->msg, "Back to the previous settings.");
		d->msg_err = 0;
		fl_line(&L, "revert", NULL);
		job_start(d, JOB_REVERT, &L, 1, 15000);
		return;
	}
	if (d->win)
		fl_win_redraw(d->win);
	if (!d->pending && !d->job)
		fl_timer_arm(d->tick, 0, 0);
}

/* one fldisplay set per output that changed, in one sequence */
static void apply(struct display *d)
{
	struct cmd_line L[MAXOUT];
	int n = 0;
	char b[64], s[16];
	for (int i = 0; i < d->nout; i++) {
		struct output *o = &d->out[i];
		struct cmd_line *c = &L[n];
		if (!is_changed(o))
			continue;
		memset(c, 0, sizeof *c);
		add(c, "fldisplay");
		add(c, "set");
		add(c, o->name);
		if (o->senabled != o->enabled) {
			snprintf(b, sizeof b, "enabled=%d", o->senabled ? 1 : 0);
			add(c, b);
		}
		if (o->senabled) {
			if (o->sw != o->cw || o->sh != o->ch || o->shz != o->chz) {
				if (o->shz)
					snprintf(b, sizeof b, "mode=%dx%d@%d", o->sw, o->sh, o->shz);
				else
					snprintf(b, sizeof b, "mode=%dx%d", o->sw, o->sh);
				add(c, b);
			}
			if (o->sscale != o->cscale) {
				scale_arg(s, sizeof s, o->sscale);
				snprintf(b, sizeof b, "scale=%s", s);
				add(c, b);
			}
			if (o->sx != o->cx || o->sy != o->cy) {
				snprintf(b, sizeof b, "pos=%d,%d", o->sx, o->sy);
				add(c, b);
			}
		}
		if (o->sprimary && !o->primary) {
			add(c, "primary=1");
		}
		if (c->n > 3)
			n++;
	}
	if (!n) {
		snprintf(d->msg, sizeof d->msg, "Nothing to apply.");
		d->msg_err = 0;
		return;
	}
	job_start(d, JOB_SET, L, n, 20000);
}

/* ---- drawing ---------------------------------------------------------------------------------------- */

static void draw_mode_row(fl_ui *ui, fl_rect r, int index, int hover, int selected, void *ud)
{
	const struct output *o = ud;
	const struct mode *m = &o->modes[index];
	char buf[64];
	int cur = m->w == o->cw && m->h == o->ch && m->hz == o->chz;
	fl_fill_rect(&ui->cv, r, selected ? fl_th.select : hover ? fl_th.raised : fl_th.panel);
	mode_label(buf, sizeof buf, m->w, m->h, m->hz);
	fl_text_in(&ui->cv, ui->f_reg, r, FL_LEFT, 12, fl_th.text, buf);
	if (cur)
		fl_text_in(&ui->cv, ui->f_reg, r, FL_RIGHT, 12, fl_th.dim, "current");
}

static int mode_index(const struct output *o)
{
	for (int i = 0; i < o->nmodes; i++)
		if (o->modes[i].w == o->sw && o->modes[i].h == o->sh && o->modes[i].hz == o->shz)
			return i;
	return 0;
}

static void draw_arrangement(struct display *d, fl_ui *ui, int x, int y, int w)
{
	fl_rect v = fl_r(x, y, w, VIEW_H), old = ui->cv.clip;

	fl_fill_rect(&ui->cv, v, fl_th.bg);
	fl_stroke_rect(&ui->cv, v, 1, fl_th.line);
	if (d->drag < 0)
		layout_map(d, v);

	/* a held screen follows the pointer once it has moved; released, it snaps */
	if (d->drag >= 0) {
		struct output *o = &d->out[d->drag];
		if (ui->btn_held & FL_BTN_L) {
			if (!d->moved && (abs(ui->mx - d->press_x) + abs(ui->my - d->press_y) > 3))
				d->moved = 1;
			if (d->moved) {
				o->sx = round_i(((float)(ui->mx - d->drag_dx) - d->ox) / d->k);
				o->sy = round_i(((float)(ui->my - d->drag_dy) - d->oy) / d->k);
			}
		} else {
			if (d->moved)
				snap_out(d, d->drag);
			d->drag = -1;
			d->moved = 0;
		}
	}

	fl_clip(&ui->cv, v);
	for (int i = 0; i < d->nout; i++) {
		struct output *o = &d->out[i];
		char buf[48];
		int lw, lh, res;
		fl_rect r;
		if (!lay_size(o, &lw, &lh))
			continue;
		r = rect_of(d, o, lw, lh);
		res = fl_widget(ui, ID_ARR + (uint32_t)i, r, 0);
		if ((res & FL_DOWN) && d->drag < 0) {
			d->drag = i;
			d->moved = 0;
			d->press_x = ui->mx;
			d->press_y = ui->my;
			d->drag_dx = ui->mx - r.x;
			d->drag_dy = ui->my - r.y;
		}
		fl_fill_rect(&ui->cv, r, o->sprimary ? fl_th.raised : fl_th.panel);
		fl_stroke_rect(&ui->cv, r, d->drag == i ? 2 : 1,
		               (d->drag == i || o->sprimary) ? fl_th.accent : fl_th.line);
		fl_text_in(&ui->cv, ui->f_med, fl_r(r.x, r.y + r.h / 2 - 22, r.w, 20), FL_CENTER, 0, fl_th.strong, o->name);
		mode_label(buf, sizeof buf, o->sw, o->sh, 0);
		fl_text_in(&ui->cv, ui->f_reg, fl_r(r.x, r.y + r.h / 2 + 2, r.w, 18), FL_CENTER, 0, fl_th.dim, buf);
	}
	ui->cv.clip = old;
	fl_text_in(&ui->cv, d->small, fl_r(x, y + VIEW_H + 6, w, 18), FL_LEFT, 0, fl_th.dim,
	           "Drag a screen to move it. Screens snap edge to edge.");
}

/* one screen: name and Enabled; with it on, mode, scale and primary */
static int draw_output(struct display *d, fl_ui *ui, int i, int x, int w, int y)
{
	struct output *o = &d->out[i];
	char what[96], buf[64];
	int on;

	if (!o->connected) {
		pg_row_text(ui, d->small, x, y, w, o->name, "Not connected");
		return y + ROW + 12;
	}
	if (o->cw) {
		mode_label(buf, sizeof buf, o->cw, o->ch, o->chz);
		snprintf(what, sizeof what, "Connected, %s", buf);
	} else {
		snprintf(what, sizeof what, "Connected, switched off");
	}
	pg_row_text(ui, d->small, x, y, w - 200, o->name, what);
	fl_text_in(&ui->cv, ui->f_reg, fl_r(x + w - 150, y, 100, ROW), FL_RIGHT, 0, fl_th.dim, "Enabled");
	on = o->senabled;
	if (fl_switch(ui, ID_ENABLED + (uint32_t)i, fl_r(x + w - 40, y + 18, 40, 20), &on))
		o->senabled = on;
	y += ROW;
	if (!o->senabled)
		return y + 12;

	/* resolution and refresh rate: one dropdown of every mode */
	pg_row_text(ui, d->small, x, y, w - 280, "Resolution and refresh", "Screen size and how often it updates");
	mode_label(buf, sizeof buf, o->sw, o->sh, o->shz);
	if (pg_dropdown(ui, ID_DD + (uint32_t)i, fl_r(x + w - 260, y + 12, 260, 32), buf, o->dd_open) && o->nmodes) {
		o->dd_open = !o->dd_open;
		o->dd.sel = mode_index(o);
	}
	y += ROW;
	if (o->dd_open && o->nmodes) {
		int h = (o->nmodes < 6 ? o->nmodes : 6) * LIST_ROW;
		int pick = fl_list(ui, ID_LIST + (uint32_t)i, fl_r(x + w - 260, y, 260, h), o->nmodes, LIST_ROW,
		                   &o->dd, draw_mode_row, o);
		if (pick >= 0 && pick < o->nmodes) {
			o->sw = o->modes[pick].w;
			o->sh = o->modes[pick].h;
			o->shz = o->modes[pick].hz;
			o->dd_open = 0;
		}
		y += h + 8;
	}

	/* scale */
	{
		static const int scales[5] = {100, 125, 150, 175, 200};
		pg_row_text(ui, d->small, x, y, w - 360, "Scale", "Size of text and windows");
		for (int k = 0; k < 5; k++) {
			fl_rect t = fl_r(x + w - 344 + k * 70, y + 12, 64, 32);
			snprintf(buf, sizeof buf, "%d%%", scales[k]);
			if (!scale_ok(o->sw, o->sh, scales[k]))
				pg_choice_off(ui, t, buf);
			else if (pg_choice(ui, ID_SCALE + (uint32_t)(i * 8 + k), t, buf, o->sscale == scales[k]))
				o->sscale = scales[k];
		}
		y += ROW;
	}

	/* primary: the one that carries the panel; turning it on takes it from the others */
	pg_row_text(ui, d->small, x, y, w - 60, "Primary", "The main screen, where the panel and the start menu go");
	on = o->sprimary;
	if (fl_switch(ui, ID_PRIMARY + (uint32_t)i, fl_r(x + w - 40, y + 18, 40, 20), &on) && on) {
		for (int j = 0; j < d->nout; j++)
			d->out[j].sprimary = j == i;
	}
	y += ROW;
	return y + 12;
}

static void draw_banner(struct display *d, fl_ui *ui, int x, int y, int w)
{
	fl_rect r = fl_r(x, y, w, ROW);
	char sub[96];
	long left = (d->deadline - now_ms() + 999) / 1000;
	struct cmd_line L;

	if (left < 0)
		left = 0;
	if (left > CONFIRM_S)
		left = CONFIRM_S;
	fl_fill_rect(&ui->cv, r, fl_th.select);
	fl_fill_rect(&ui->cv, fl_r(r.x, r.y, 3, r.h), fl_th.accent);
	snprintf(sub, sizeof sub, "Going back to the old settings in %ld s", left);
	pg_row_text(ui, d->small, r.x + 16, r.y, w - 220, "Keep these settings?", sub);
	if (fl_button(ui, ID_KEEP, fl_r(r.x + w - 190, r.y + 14, 84, 28), "Keep", FL_BTN_ACCENT)) {
		d->pending = 0;
		snprintf(d->msg, sizeof d->msg, "Kept. The screens stay as set.");
		d->msg_err = 0;
	}
	if (fl_button(ui, ID_REVERT, fl_r(r.x + w - 98, r.y + 14, 84, 28), "Revert", FL_BTN_OUTLINE) && !d->job) {
		d->pending = 0;
		snprintf(d->msg, sizeof d->msg, "Back to the previous settings.");
		d->msg_err = 0;
		fl_line(&L, "revert", NULL);
		job_start(d, JOB_REVERT, &L, 1, 15000);
	}
}

static int draw_apply(struct display *d, fl_ui *ui, int x, int y, int w)
{
	int changed = 0;
	for (int i = 0; i < d->nout; i++)
		changed |= is_changed(&d->out[i]);
	if (changed && !d->pending) {
		fl_text_in(&ui->cv, d->small, fl_r(x, y, w - 200, 32), FL_LEFT, 0, fl_th.dim,
		           "You get 15 seconds to keep the change.");
		if (fl_button(ui, ID_APPLY, fl_r(x + w - 184, y, 84, 32), "Apply", FL_BTN_ACCENT) && !d->job) {
			d->msg[0] = 0;
			apply(d);
		}
		if (fl_button(ui, ID_RESET, fl_r(x + w - 92, y, 92, 32), "Reset", FL_BTN_OUTLINE))
			for (int i = 0; i < d->nout; i++)
				take_current(&d->out[i]);
	} else if (d->msg[0]) {
		fl_text_in(&ui->cv, d->small, fl_r(x, y, w, 32), FL_LEFT, 0, d->msg_err ? fl_th.err : fl_th.dim, d->msg);
	}
	return y + 32 + 18;
}

/* the night light fields: the mode, the warmth and (for auto) the two times */
static void night_write(const char *mode, int temp)
{
	char t[16], warm[16];
	int tv = temp < 2700 ? 2700 : temp > 6500 ? 6500 : temp;
	const char *pairs[8];
	snprintf(t, sizeof t, "%d", tv);
	snprintf(warm, sizeof warm, "%d", (6500 - tv) * 100 / 3800);
	pairs[0] = "nightlight";
	pairs[1] = mode;
	pairs[2] = "nightlight_temp";
	pairs[3] = t;
	pairs[4] = "night_light";
	pairs[5] = !strcmp(mode, "on") ? "1" : "0";
	pairs[6] = "night_warmth";
	pairs[7] = warm;
	fl_conf_set_many((const char *const *)pairs, 4);
}

static int draw_night(struct display *d, fl_ui *ui, int x, int y, int w)
{
	static const char *const names[3] = {"Off", "On", "Auto"};
	static const char *const keys[3] = {"off", "on", "auto"};
	const char *mode = fl_conf_get("nightlight", NULL);
	int cur, temp, v;
	char buf[24];

	if (!mode)
		mode = fl_conf_int("night_light", 0) ? "on" : "off";
	cur = !strcmp(mode, "on") ? 1 : !strcmp(mode, "auto") ? 2 : 0;
	temp = fl_conf_int("nightlight_temp", 4000);
	temp = temp < 2700 ? 2700 : temp > 6500 ? 6500 : temp;

	pg_section(ui, d->head, x, y, w, "Night light");
	y += 28;
	pg_row_text(ui, d->small, x, y, w - 260, "Night light", "Warmer colours in the evening, easier on the eyes");
	for (int i = 0; i < 3; i++)
		if (pg_choice(ui, ID_NL_OFF + (uint32_t)i, fl_r(x + w - 244 + i * 84, y + 12, 76, 32), names[i], cur == i))
			night_write(keys[i], temp);
	y += ROW;
	if (cur) {
		v = 6500 - temp;	/* the slider runs from the coolest (left) to the warmest (right) */
		snprintf(buf, sizeof buf, "%d K", temp);
		pg_row_text(ui, d->small, x, y, w - 320, "Warmth", "How orange the screen gets: drag right for warmer");
		fl_text_in(&ui->cv, ui->f_reg, fl_r(x + w - 64, y, 64, ROW), FL_RIGHT, 0, fl_th.text, buf);
		if (pg_slider(ui, ID_NL_TEMP, fl_r(x + w - 300, y + 12, 224, 32), &v, 0, 3800, 50))
			night_write(keys[cur], 6500 - v);
		y += ROW;
	}
	if (cur == 2) {
		int e;
		pg_row_text(ui, d->small, x, y, w - 200, "From", "Warm from this time, HH:MM");
		e = fl_textfield(ui, ID_NL_FROM, fl_r(x + w - 100, y + 10, 84, 36), d->nl_from, sizeof d->nl_from,
		                 "20:00", ui->f_reg);
		if ((e & FL_EDIT_CHANGED) && hhmm_ok(d->nl_from))
			fl_conf_set("nightlight_from", d->nl_from);
		y += ROW;
		pg_row_text(ui, d->small, x, y, w - 200, "To", "Back to normal at this time, HH:MM");
		e = fl_textfield(ui, ID_NL_TO, fl_r(x + w - 100, y + 10, 84, 36), d->nl_to, sizeof d->nl_to, "07:00",
		                 ui->f_reg);
		if ((e & FL_EDIT_CHANGED) && hhmm_ok(d->nl_to))
			fl_conf_set("nightlight_to", d->nl_to);
		y += ROW;
	}
	return y;
}

int disp_draw(struct display *d, fl_ui *ui, fl_rect c)
{
	int x = c.x + 28, w = c.w - 56, y = c.y + 24;

	if (d->pending) {
		draw_banner(d, ui, x, y, w);
		y += ROW + 16;
	}
	pg_section(ui, d->head, x, y, w, "Screens");
	if (d->job)
		pg_spinner(ui, x + w - 8, y + 8, d->phase);
	y += 28;
	if (!d->listed) {
		if (d->msg_err) {
			fl_text_in(&ui->cv, d->small, fl_r(x, y, w, 32), FL_LEFT, 0, fl_th.err, d->msg);
		} else {
			fl_text_in(&ui->cv, d->small, fl_r(x, y, w, 32), FL_LEFT, 0, fl_th.dim, "Looking at the screens...");
		}
		y += 32 + 18;
	} else {
		draw_arrangement(d, ui, x, y, w);
		y += VIEW_H + 36;
		for (int i = 0; i < d->nout; i++)
			y = draw_output(d, ui, i, x, w, y);
		y = draw_apply(d, ui, x, y, w);
	}
	y += 12;
	y = draw_night(d, ui, x, y, w);
	y += 24;
	y = pages_draw_brightness(d->pg, ui, x, y, w);
	return y - c.y;
}

/* ---- the page's life ----------------------------------------------------------------------------- */

struct display *disp_new(fl_app *app, fl_font *head, fl_font *small, struct pages *pg)
{
	struct display *d = calloc(1, sizeof *d);
	if (!d)
		return NULL;
	d->app = app;
	d->head = head;
	d->small = small;
	d->pg = pg;
	d->drag = -1;
	snprintf(d->nl_from, sizeof d->nl_from, "%s", "20:00");
	snprintf(d->nl_to, sizeof d->nl_to, "%s", "07:00");
	d->tick = fl_timer_new(app, tick_cb, d);
	if (!d->tick) {
		free(d);
		return NULL;
	}
	return d;
}

void disp_free(struct display *d)
{
	if (!d)
		return;
	if (d->pending)
		revert_at_exit();
	seq_cancel(d->job);
	fl_timer_free(d->tick);
	free(d);
}

void disp_set_win(struct display *d, fl_win *win)
{
	d->win = win;
}

void disp_show(struct display *d, int visible)
{
	struct cmd_line L;
	if (!d || !visible)
		return;
	snprintf(d->nl_from, sizeof d->nl_from, "%s", fl_conf_get("nightlight_from", "20:00"));
	snprintf(d->nl_to, sizeof d->nl_to, "%s", fl_conf_get("nightlight_to", "07:00"));
	if (!d->job && !d->pending) {
		fl_line(&L, "list", NULL);
		job_start(d, JOB_LIST, &L, 1, 10000);
	}
}
