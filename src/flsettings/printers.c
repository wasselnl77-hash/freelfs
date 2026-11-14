/* printers.c -- the Printers page of flsettings
 * SPDX-License-Identifier: MIT
 *
 * CUPS through its command-line tools only, each one a helper sequence (the window keeps drawing):
 *	lpstat -p -d                                 printers, their state and the default
 *	lpinfo --include-schemes dnssd,ipp,ipps,usb -v   what can be added (about 10 s)
 *	lpadmin -p NAME -E -v URI -m everywhere      add (NAME: [A-Za-z0-9_-] only)
 *	lpoptions -d NAME                            set as default
 *	lp -d NAME FILE                              test page (cups' testprint, else a small generated file)
 *	cancel -a NAME                               cancel all jobs
 *	lpadmin -x NAME                              remove (asked first)
 * Without lpstat (ENOENT) the page says printing is not installed and nothing else.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cmd.h"
#include "pages.h"
#include "printers.h"

#define MAXPR 16
#define MAXFOUND 16
#define ROW 56
#define TESTPRINT "/usr/share/cups/data/testprint"

enum {
	ID_PR_SEARCH = 1500, ID_PR_ADD = 1510 /* .. 1525 */, ID_PR_DEF = 1530 /* .. 1545 */,
	ID_PR_TEST = 1550 /* .. 1565 */, ID_PR_CANCEL = 1570 /* .. 1585 */, ID_PR_REMOVE = 1590 /* .. 1605 */,
	ID_PR_YES = 1610, ID_PR_NO,
};
enum { PR_LOADING, PR_NOTINSTALLED, PR_OK };
enum { JOB_NONE, JOB_LIST, JOB_FIND, JOB_ACT };

struct printer {
	char name[48], status[16];
	int isdefault, disabled;
};

struct found {
	char uri[CMD_ARG_LEN];
	char cls[16], name[96];
};

struct printers {
	fl_app *app;
	fl_font *head, *small;
	fl_win *win;
	int state;
	struct printer pr[MAXPR];
	int npr;
	char defname[48];
	struct found fd[MAXFOUND];
	int nfd, searched;
	struct seq *job;
	int job_kind;
	char busy[96];
	int ask_remove;		/* the printer whose removal is being confirmed, -1 */
	char tmp[256];		/* the generated test page, removed when its job ends */
	char msg[320];
	int msg_err;
	fl_timer *tick;
	int phase;
};

static void on_done(void *ud, int failed, const struct cmd_result *r);

/* a NULL-terminated argv */
static void mkv(struct cmd_line *L, int text_fail, const char *const *w)
{
	memset(L, 0, sizeof *L);
	L->text_fail = text_fail;
	for (int i = 0; w[i] && L->n < CMD_ARGS; i++)
		snprintf(L->arg[L->n++], CMD_ARG_LEN, "%s", w[i]);
}

static void start(struct printers *p, int kind, const struct cmd_line *L, int n, int timeout, const char *busy)
{
	p->job_kind = kind;
	snprintf(p->busy, sizeof p->busy, "%s", busy ? busy : "");
	p->job = seq_start(p->app, L, n, timeout, on_done, p);
	if (!p->job) {
		p->job_kind = JOB_NONE;
		p->busy[0] = 0;
		snprintf(p->msg, sizeof p->msg, "Cannot start the printing tools.");
		p->msg_err = 1;
		if (kind == JOB_LIST)
			p->state = PR_NOTINSTALLED;
	}
	fl_timer_arm(p->tick, 200, 200);
	if (p->win)
		fl_win_redraw(p->win);
}

static void start_list(struct printers *p)
{
	struct cmd_line L;
	const char *const w[] = {"lpstat", "-p", "-d", NULL};
	mkv(&L, 0, w);
	start(p, JOB_LIST, &L, 1, 10000, "Looking at the printers...");
}

/* "printer NAME is idle.", "printer NAME disabled since ...", "system default destination: NAME" */
static void parse_lpstat(struct printers *p, char *text)
{
	char *s = text, *e;
	p->npr = 0;
	p->defname[0] = 0;
	while (s && *s) {
		e = strchr(s, '\n');
		if (e)
			*e = 0;
		if (!strncmp(s, "printer ", 8) && p->npr < MAXPR) {
			struct printer *q = &p->pr[p->npr];
			const char *sp;
			memset(q, 0, sizeof *q);
			sp = strchr(s + 8, ' ');
			snprintf(q->name, sizeof q->name, "%.*s", (int)(sp ? sp - (s + 8) : (long)strlen(s + 8)), s + 8);
			q->disabled = strstr(s, "disabled") != NULL;
			snprintf(q->status, sizeof q->status, "%s",
			         q->disabled ? "Disabled" : strstr(s, "printing") ? "Printing" : "Idle");
			p->npr++;
		} else if (!strncmp(s, "system default destination: ", 28)) {
			snprintf(p->defname, sizeof p->defname, "%s", s + 28);
		}
		if (!e)
			break;
		s = e + 1;
	}
	for (int i = 0; i < p->npr; i++)
		p->pr[i].isdefault = !strcmp(p->pr[i].name, p->defname);
}

/* "network dnssd://HP%20LaserJet._ipp._tcp.local/?uuid=..." : the class, then the URI */
static void parse_lpinfo(struct printers *p, char *text)
{
	char *s = text, *e;
	p->nfd = 0;
	while (s && *s && p->nfd < MAXFOUND) {
		char *sp;
		e = strchr(s, '\n');
		if (e)
			*e = 0;
		sp = strchr(s, ' ');
		if (sp && strstr(sp + 1, "://")) {
			struct found *f = &p->fd[p->nfd];
			snprintf(f->cls, sizeof f->cls, "%.*s", (int)(sp - s), s);
			snprintf(f->uri, sizeof f->uri, "%s", sp + 1);
			p->nfd++;
		}
		if (!e)
			break;
		s = e + 1;
	}
}

/* a printer name from a URI: the host or model part, [A-Za-z0-9_-] only, never empty */
static void name_from_uri(const char *uri, char *out, size_t n)
{
	const char *s = strstr(uri, "://");
	size_t j = 0;
	s = s ? s + 3 : uri;
	for (; *s && *s != '/' && *s != '?' && !(s[0] == '.' && s[1] == '_') && j < 40 && j + 1 < n; s++) {
		char c = *s;
		if (c == '%' && s[1] && s[2]) {
			s += 2;
			c = '_';
		}
		out[j++] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'
		               ? c : '_';
	}
	out[j] = 0;
	if (!j)
		snprintf(out, n, "Printer");
}

/* the name is not taken by a printer already: NAME, NAME-2, ... */
static void unique_name(const struct printers *p, char *name, size_t n)
{
	char base[41], cand[64];
	snprintf(base, sizeof base, "%.40s", name);
	for (int k = 1; k < 10; k++) {
		int taken = 0;
		if (k == 1)
			snprintf(cand, sizeof cand, "%s", base);
		else
			snprintf(cand, sizeof cand, "%s-%d", base, k);
		for (int i = 0; i < p->npr; i++)
			taken |= !strcmp(p->pr[i].name, cand);
		if (!taken) {
			snprintf(name, n, "%s", cand);
			return;
		}
	}
}

/* the test page: CUPS' own when it is there, else a short text file that is removed afterwards */
static void start_test(struct printers *p, const char *name)
{
	struct cmd_line L;
	const char *file = TESTPRINT;
	const char *dir;
	char line[160];
	int fd;

	if (access(file, R_OK) != 0) {
		dir = getenv("XDG_RUNTIME_DIR");
		if (!dir || !*dir)
			dir = getenv("TMPDIR");
		if (!dir || !*dir)
			dir = "/tmp";
		snprintf(p->tmp, sizeof p->tmp, "%s/flprint-XXXXXX", dir);
		fd = mkstemp(p->tmp);
		if (fd < 0) {
			p->tmp[0] = 0;
			snprintf(p->msg, sizeof p->msg, "Cannot make the test page: %s", strerror(errno));
			p->msg_err = 1;
			return;
		}
		snprintf(line, sizeof line, "FreeLFS test page\n\nPrinter: %s\n\nIf you can read this, printing works.\n", name);
		if (write(fd, line, strlen(line)) < 0)
			p->tmp[0] = 0;
		close(fd);
		file = p->tmp[0] ? p->tmp : TESTPRINT;
	}
	{
		const char *const w[] = {"lp", "-d", name, file, NULL};
		mkv(&L, 0, w);
	}
	start(p, JOB_ACT, &L, 1, 20000, "Sending the test page...");
}

static void on_done(void *ud, int failed, const struct cmd_result *r)
{
	struct printers *p = ud;
	char m[160];
	int kind = p->job_kind;
	char *copy;

	p->job = NULL;
	p->job_kind = JOB_NONE;
	if (kind == JOB_LIST) {
		if (failed >= 0 && r->spawn_errno == ENOENT) {
			p->state = PR_NOTINSTALLED;
		} else if (failed >= 0 && !strstr(r->err, "No destinations") && !strstr(r->out, "No destinations")) {
			cmd_message(r, m, sizeof m);
			snprintf(p->msg, sizeof p->msg, "lpstat: %s", m);
			p->msg_err = 1;
			p->state = PR_OK;
		} else {
			copy = strdup(r->out);
			if (copy)
				parse_lpstat(p, copy);
			free(copy);
			p->state = PR_OK;
		}
	} else if (kind == JOB_FIND) {
		p->searched = 1;
		if (failed >= 0) {
			cmd_message(r, m, sizeof m);
			snprintf(p->msg, sizeof p->msg, "Looking for printers failed: %s", m);
			p->msg_err = 1;
			p->nfd = 0;
		} else {
			copy = strdup(r->out);
			if (copy)
				parse_lpinfo(p, copy);
			free(copy);
			for (int i = 0; i < p->nfd; i++) {
				char nm[96];
				name_from_uri(p->fd[i].uri, nm, sizeof nm);
				snprintf(p->fd[i].name, sizeof p->fd[i].name, "%s", nm);
			}
		}
	} else if (kind == JOB_ACT) {
		if (p->tmp[0]) {
			unlink(p->tmp);
			p->tmp[0] = 0;
		}
		if (failed >= 0) {
			cmd_message(r, m, sizeof m);
			snprintf(p->msg, sizeof p->msg, "%s", m[0] ? m : "The printing tools failed.");
			p->msg_err = 1;
		} else {
			p->msg[0] = 0;
		}
		start_list(p);
		return;
	}
	p->busy[0] = 0;
	if (p->win)
		fl_win_redraw(p->win);
}

static void tick_cb(void *ud)
{
	struct printers *p = ud;
	p->phase++;
	if (p->win)
		fl_win_redraw(p->win);
	if (!p->job)
		fl_timer_arm(p->tick, 0, 0);
}

/* ---- drawing ------------------------------------------------------------------------------------ */

int pr_draw(struct printers *p, fl_ui *ui, fl_rect c)
{
	int x = c.x + 28, w = c.w - 56, y = c.y + 24;
	char buf[160];

	if (p->state == PR_NOTINSTALLED) {
		fl_text_in(&ui->cv, ui->f_med, fl_r(x, y, w, ROW), FL_LEFT, 0, fl_th.text, "Printing is not installed");
		return y + ROW - c.y;
	}
	if (p->state == PR_LOADING) {
		pg_spinner(ui, x + 8, y + 20, p->phase);
		fl_text_in(&ui->cv, ui->f_reg, fl_r(x + 32, y + 8, w - 32, 32), FL_LEFT, 0, fl_th.dim,
		           "Looking at the printers...");
		return y + 40 - c.y;
	}

	pg_section(ui, p->head, x, y, w, "Printers");
	y += 28;
	if (!p->npr) {
		fl_text_in(&ui->cv, ui->f_reg, fl_r(x, y, w, 32), FL_LEFT, 0, fl_th.dim,
		           "No printers are set up yet. Add one below.");
		y += 32 + 12;
	}
	for (int i = 0; i < p->npr; i++) {
		const struct printer *q = &p->pr[i];
		pg_glyph(ui, PG_SVG_PRINTER, x + 4, y + 14, 24, q->disabled ? fl_th.dim : fl_th.text);
		snprintf(buf, sizeof buf, "%s%s", q->isdefault ? "Default, " : "", q->status);
		pg_row_text(ui, p->small, x + 44, y, w - 44, q->name, buf);
		y += ROW - 8;
		if (p->ask_remove == i) {
			fl_text_in(&ui->cv, p->small, fl_r(x, y + 8, w - 240, 32), FL_LEFT, 0, fl_th.dim,
			           "Remove this printer? Its queue and settings go.");
			if (fl_button(ui, ID_PR_YES, fl_r(x + w - 232, y + 8, 116, 32), "Remove", FL_BTN_ACCENT) && !p->job) {
				const char *const wv[] = {"lpadmin", "-x", q->name, NULL};
				struct cmd_line L;
				mkv(&L, 0, wv);
				p->ask_remove = -1;
				snprintf(buf, sizeof buf, "Removing %s...", q->name);
				start(p, JOB_ACT, &L, 1, 20000, buf);
			}
			if (fl_button(ui, ID_PR_NO, fl_r(x + w - 106, y + 8, 106, 32), "Cancel", FL_BTN_OUTLINE))
				p->ask_remove = -1;
		} else {
			int bx = x;
			if (!q->isdefault) {
				if (fl_button(ui, ID_PR_DEF + (uint32_t)i, fl_r(bx, y + 8, 96, 32), "Default", FL_BTN_OUTLINE) && !p->job) {
					const char *const wv[] = {"lpoptions", "-d", q->name, NULL};
					struct cmd_line L;
					mkv(&L, 0, wv);
					start(p, JOB_ACT, &L, 1, 10000, "Setting the default printer...");
				}
				bx += 104;
			}
			if (fl_button(ui, ID_PR_TEST + (uint32_t)i, fl_r(bx, y + 8, 92, 32), "Test page", FL_BTN_OUTLINE) && !p->job)
				start_test(p, q->name);
			bx += 100;
			if (fl_button(ui, ID_PR_CANCEL + (uint32_t)i, fl_r(bx, y + 8, 108, 32), "Cancel jobs", FL_BTN_OUTLINE) && !p->job) {
				const char *const wv[] = {"cancel", "-a", q->name, NULL};
				struct cmd_line L;
				mkv(&L, 0, wv);
				start(p, JOB_ACT, &L, 1, 10000, "Cancelling the jobs...");
			}
			bx += 116;
			if (fl_button(ui, ID_PR_REMOVE + (uint32_t)i, fl_r(bx, y + 8, 84, 32), "Remove", FL_BTN_OUTLINE) && !p->job)
				p->ask_remove = i;
		}
		y += 44 + 12;
	}
	y += 12;

	pg_section(ui, p->head, x, y, w, "Add a printer");
	y += 28;
	pg_row_text(ui, p->small, x, y, w - 200, "Find printers",
	            p->searched ? "On the network and on USB" : "Looks on the network and on USB, about 10 seconds");
	if (p->job_kind == JOB_FIND)
		pg_spinner(ui, x + w - 150, y + 28, p->phase);
	if (fl_button(ui, ID_PR_SEARCH, fl_r(x + w - 120, y + 12, 120, 32), "Search", FL_BTN_OUTLINE) && !p->job) {
		const char *const wv[] = {"lpinfo", "--include-schemes", "dnssd,ipp,ipps,usb", "-v", NULL};
		struct cmd_line L;
		mkv(&L, 0, wv);
		start(p, JOB_FIND, &L, 1, 25000, "Looking for printers, about 10 seconds...");
	}
	y += ROW;
	for (int i = 0; i < p->nfd; i++) {
		const struct found *f = &p->fd[i];
		pg_glyph(ui, PG_SVG_PRINTER, x + 4, y + 14, 24, fl_th.dim);
		pg_row_text(ui, p->small, x + 44, y, w - 44 - 120, f->name, f->uri);
		if (fl_button(ui, ID_PR_ADD + (uint32_t)i, fl_r(x + w - 106, y + 12, 106, 32), "Add", FL_BTN_ACCENT) && !p->job) {
			char nm[96];
			struct cmd_line L;
			name_from_uri(f->uri, nm, sizeof nm);
			unique_name(p, nm, sizeof nm);
			{
				const char *const wv[] = {"lpadmin", "-p", nm, "-E", "-v", f->uri, "-m", "everywhere", NULL};
				mkv(&L, 0, wv);
			}
			snprintf(buf, sizeof buf, "Adding %s...", nm);
			start(p, JOB_ACT, &L, 1, 20000, buf);
		}
		y += ROW;
	}
	if (p->searched && !p->nfd) {
		fl_text_in(&ui->cv, ui->f_reg, fl_r(x, y, w, 32), FL_LEFT, 0, fl_th.dim,
		           "No printers found. Check that it is on and connected.");
		y += 32;
	}

	y += 16;
	if (p->job) {
		pg_spinner(ui, x + 8, y + 12, p->phase);
		fl_text_in(&ui->cv, p->small, fl_r(x + 32, y, w - 32, 24), FL_LEFT, 0, fl_th.dim, p->busy);
		y += 32;
	} else if (p->msg[0]) {
		fl_text_in(&ui->cv, p->small, fl_r(x, y, w, 24), FL_LEFT, 0, p->msg_err ? fl_th.err : fl_th.dim, p->msg);
		y += 32;
	}
	return y + 12 - c.y;
}

/* ---- the page's life ----------------------------------------------------------------------------- */

struct printers *pr_new(fl_app *app, fl_font *head, fl_font *small)
{
	struct printers *p = calloc(1, sizeof *p);
	if (!p)
		return NULL;
	p->app = app;
	p->head = head;
	p->small = small;
	p->ask_remove = -1;
	p->state = PR_LOADING;
	p->tick = fl_timer_new(app, tick_cb, p);
	if (!p->tick) {
		free(p);
		return NULL;
	}
	return p;
}

void pr_free(struct printers *p)
{
	if (!p)
		return;
	seq_cancel(p->job);
	if (p->tmp[0])
		unlink(p->tmp);
	fl_timer_free(p->tick);
	free(p);
}

void pr_set_win(struct printers *p, fl_win *win)
{
	p->win = win;
}

void pr_show(struct printers *p, int visible)
{
	if (!p)
		return;
	if (!visible) {
		/* the search results are not kept after the page is left */
		p->nfd = 0;
		p->searched = 0;
		p->ask_remove = -1;
		return;
	}
	if (!p->job)
		start_list(p);
}
