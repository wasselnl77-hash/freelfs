/* snapshots.c -- the Snapshots page of flsettings
 * SPDX-License-Identifier: MIT
 *
 * Lists the btrfs snapshots flsnap keeps (src/flsnap): date, why it was taken and how
 * many packages it holds. "Take a snapshot now", "Go back to this" (flsnap rollback N:
 * the current system is kept as a snapshot too, and the choice applies after a restart)
 * and "Delete". When the system itself runs from a snapshot, a banner offers "Make this the
 * current state" (flsnap rollback). Everything privileged is done by /usr/bin/flsnap, which is
 * setuid root for the group seat; this page only runs it. $FLSNAP names another program
 * (tests: the real flsnap on its FLSNAP_MOCK directory backend).
 */
#include <errno.h>
#include <fcntl.h>
#include <flui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pages.h"
#include "snapshots.h"

#define MAXSNAPS 64
#define ROWH 64

enum {
	ID_S_CREATE = 700, ID_S_KEEP, ID_S_YES, ID_S_NO,
	ID_S_GO = 710 /* .. 710 + 2 * MAXSNAPS */,
};

struct snap {
	long n;
	char date[32], reason[160];
	int npkg, running;
};

struct snapshots {
	fl_app *app;
	fl_font *head, *small;
	fl_win *win;
	struct snap v[MAXSNAPS];
	int n;
	int supported;
	char why[200];
	int keep;
	long booted;       /* the snapshot the system runs from, 0: the normal system */
	long confirm;      /* snapshot with an open question, 0: none */
	int confirm_kind;  /* 1: go back to it, 2: delete it */
	char msg[300];
	int msg_err;
};

/* Runs flsnap with ARGS; the first CAP-1 bytes of stdout and stderr go to OUT. Its exit status, -1 if it did not run. */
static int run_flsnap(char *const args[], char *out, size_t cap)
{
	const char *prog = getenv("FLSNAP");
	if (!prog || !*prog)
		prog = "/usr/bin/flsnap";
	int fd[2];
	if (pipe(fd) < 0)
		return -1;
	pid_t p = fork();
	if (p < 0) {
		close(fd[0]);
		close(fd[1]);
		return -1;
	}
	if (p == 0) {
		int nul = open("/dev/null", O_RDONLY);
		if (nul >= 0)
			dup2(nul, 0);
		dup2(fd[1], 1);
		dup2(fd[1], 2);
		close(fd[0]);
		close(fd[1]);
		char *argv[8] = {(char *)prog};
		for (int i = 0; args[i] && i < 6; i++)
			argv[i + 1] = args[i];
		execv(prog, argv);
		if (errno != ENOENT)
			execvp(prog, argv);
		_exit(errno == ENOENT ? 127 : 126);
	}
	close(fd[1]);
	size_t n = 0;
	ssize_t r;
	while ((r = read(fd[0], out + n, cap - 1 - n)) > 0 && (n += (size_t)r) < cap - 1)
		;
	out[n] = '\0';
	close(fd[0]);
	int st;
	if (waitpid(p, &st, 0) < 0 || !WIFEXITED(st))
		return -1;
	return WEXITSTATUS(st);
}

static void say(struct snapshots *s, int err, const char *text)
{
	snprintf(s->msg, sizeof s->msg, "%s", text);
	s->msg[strcspn(s->msg, "\n")] = '\0'; /* the first line */
	s->msg_err = err;
}

static void reload(struct snapshots *s)
{
	char out[1 << 16];
	s->n = 0;
	s->supported = 0;
	s->booted = 0;
	snprintf(s->why, sizeof s->why, "The snapshot tool flsnap is not installed.");
	s->keep = 8;
	char *st[] = {"status", NULL};
	int rc = run_flsnap(st, out, sizeof out);
	if (rc == 126) {
		snprintf(s->why, sizeof s->why, "Needs administrator rights: the account has to be in the group seat.");
		return;
	}
	if (rc != 0)
		return;
	for (char *l = strtok(out, "\n"); l; l = strtok(NULL, "\n")) {
		if (!strncmp(l, "supported=", 10))
			s->supported = atoi(l + 10);
		else if (!strncmp(l, "why=", 4))
			snprintf(s->why, sizeof s->why, "%s", l + 4);
		else if (!strncmp(l, "keep=", 5))
			s->keep = atoi(l + 5);
		else if (!strncmp(l, "booted=", 7))
			s->booted = atol(l + 7);
	}
	if (!s->supported)
		return;
	char *ls[] = {"list", "--machine", NULL};
	if (run_flsnap(ls, out, sizeof out) != 0)
		return;
	for (char *l = strtok(out, "\n"); l && s->n < MAXSNAPS; l = strtok(NULL, "\n")) {
		/* N, epoch, date, reason, packages, running: tab separated (empty reason: fields stay) */
		char *f[6] = {0};
		char *p = l;
		for (int i = 0; i < 6; i++) {
			f[i] = p;
			char *t = strchr(p, '\t');
			if (!t) {
				if (i < 5)
					f[0] = NULL;
				break;
			}
			*t = '\0';
			p = t + 1;
		}
		if (!f[0] || !f[5])
			continue;
		struct snap *e = &s->v[s->n++];
		e->n = atol(f[0]);
		snprintf(e->date, sizeof e->date, "%s", f[2]);
		snprintf(e->reason, sizeof e->reason, "%s", f[3]);
		e->npkg = atoi(f[4]);
		e->running = atoi(f[5]);
	}
}

struct snapshots *snap_new(fl_app *app, fl_font *head, fl_font *small)
{
	struct snapshots *s = calloc(1, sizeof *s);
	if (!s)
		return NULL;
	s->app = app;
	s->head = head;
	s->small = small;
	return s;
}

void snap_free(struct snapshots *s)
{
	free(s);
}

void snap_set_win(struct snapshots *s, fl_win *win)
{
	s->win = win;
}

void snap_show(struct snapshots *s, int visible)
{
	if (!visible)
		return;
	s->confirm = 0;
	s->msg[0] = '\0';
	reload(s);
}

static void do_flsnap(struct snapshots *s, char *const args[], const char *done)
{
	char out[2048];
	int rc = run_flsnap(args, out, sizeof out);
	if (rc == 0) {
		/* flsnap's own sentence when it has one worth showing (rollback), else ours */
		char *r = strstr(out, "Restart");
		say(s, 0, r ? r : done);
	} else if (rc == 126) {
		say(s, 1, "Needs administrator rights: the account has to be in the group seat.");
	} else if (rc == 127) {
		say(s, 1, "The snapshot tool flsnap is not installed.");
	} else {
		char *m = strstr(out, "flsnap: ");
		say(s, 1, m ? m + 8 : "It did not work.");
	}
	reload(s);
	if (s->win)
		fl_win_redraw(s->win);
}

static void line_text(fl_ui *ui, fl_font *f, int x, int y, int maxw, fl_color c, const char *t)
{
	char fit[200];
	fl_text_fit(f, t, maxw, fit, sizeof fit);
	fl_text(&ui->cv, f, x, y, c, fit, -1);
}

int snap_draw(struct snapshots *s, fl_ui *ui, fl_rect c)
{
	int x = c.x + 28, w = c.w - 56, y = c.y + 24;
	char buf[256];

	pg_section(ui, s->head, x, y, w, "Snapshots");
	y += 28;
	if (!s->supported) {
		pg_row_text(ui, s->small, x, y, w, "Snapshots are not available", s->why);
		return y + ROWH + 24 - c.y;
	}

	if (s->booted > 0) {
		/* the system runs from a snapshot: say so, and offer to keep it */
		fl_rect b = fl_r(x, y, w, 100);
		fl_fill_rect(&ui->cv, b, fl_th.raised);
		fl_fill_rect(&ui->cv, fl_r(b.x, b.y, 3, b.h), fl_th.accent);
		snprintf(buf, sizeof buf, "You are in an earlier state (snapshot #%ld)", s->booted);
		fl_text(&ui->cv, ui->f_med, b.x + 18, b.y + 12, fl_th.strong, buf, -1);
		line_text(ui, s->small, b.x + 18, b.y + 34, w - 36, fl_th.dim,
		          "It is read-only: nothing you change here is kept. Restart to go back to normal,");
		line_text(ui, s->small, b.x + 18, b.y + 52, w - 36, fl_th.dim, "or make this state the current one.");
		if (fl_button(ui, ID_S_KEEP, fl_r(b.x + b.w - 18 - 220, b.y + b.h - 40, 220, 30), "Make this the current state",
		              FL_BTN_ACCENT)) {
			char *a[] = {"rollback", NULL};
			do_flsnap(s, a, "Done.");
		}
		y += 100 + 16;
	}

	snprintf(buf, sizeof buf, s->booted > 0 ? "Not while an earlier state is running."
	                                       : "Also taken before every update. Keeps the last %d.", s->keep);
	pg_row_text(ui, s->small, x, y, w - 190, "Take a snapshot now", buf);
	if (s->booted > 0) {
		/* flsnap refuses while an old state runs */
	} else if (fl_button(ui, ID_S_CREATE, fl_r(x + w - 170, y + 12, 170, 32), "Take a snapshot", FL_BTN_OUTLINE)) {
		char *a[] = {"create", "--reason", "taken in Settings", NULL};
		do_flsnap(s, a, "Snapshot taken.");
	}
	y += ROWH;
	if (s->msg[0]) {
		line_text(ui, s->small, x, y, w, s->msg_err ? fl_th.err : fl_th.accent, s->msg);
		y += 22;
	}
	y += 16;

	pg_section(ui, s->head, x, y, w, "Saved states");
	y += 28;
	if (!s->n) {
		pg_row_text(ui, s->small, x, y, w, "No snapshots yet", "The first one is taken before the next software change.");
		y += ROWH;
	}
	for (int i = 0; i < s->n; i++, y += ROWH) {
		struct snap *e = &s->v[i];
		int bw = 124;
		snprintf(buf, sizeof buf, "#%ld   %s%s", e->n, e->date, e->running ? "   (running now)" : "");
		char what[260];
		snprintf(what, sizeof what, "%s%s%d packages", e->reason, e->reason[0] ? "  -  " : "", e->npkg);
		fl_fill_rect(&ui->cv, fl_r(x, y + ROWH - 1, w, 1), fl_th.line);
		if (s->confirm == e->n) {
			int th = fl_font_height(ui->f_med);
			line_text(ui, ui->f_med, x, y + (ROWH - th) / 2 - 10, w - 2 * bw - 24, fl_th.text,
			          s->confirm_kind == 1 ? "Go back to this state?" : "Delete this snapshot?");
			line_text(ui, s->small, x, y + (ROWH - th) / 2 + 10, w - 2 * bw - 24, fl_th.dim,
			          s->confirm_kind == 1 ? "The current system is kept as a snapshot. Restart to start it."
			                               : "It cannot be brought back.");
			if (fl_button(ui, ID_S_YES, fl_r(x + w - 2 * bw - 8, y + 15, bw, 32),
			              s->confirm_kind == 1 ? "Go back" : "Delete", FL_BTN_ACCENT)) {
				char num[16];
				snprintf(num, sizeof num, "%ld", e->n);
				char *a[] = {s->confirm_kind == 1 ? "rollback" : "delete", num, NULL};
				long n = e->n;
				s->confirm = 0;
				snprintf(buf, sizeof buf, s->confirm_kind == 1 ? "Snapshot #%ld is the current state after a restart." : "Snapshot #%ld deleted.", n);
				do_flsnap(s, a, buf);
				return y + ROWH - c.y + 24; /* the list changed under us: draw it again */
			}
			if (fl_button(ui, ID_S_NO, fl_r(x + w - bw, y + 15, bw, 32), "Cancel", FL_BTN_OUTLINE))
				s->confirm = 0;
			continue;
		}
		pg_row_text(ui, s->small, x, y, w - 2 * bw - 24, buf, what);
		if (e->running)
			continue; /* the state running now can be made current (banner), not deleted */
		if (fl_button(ui, ID_S_GO + 2 * (uint32_t)i, fl_r(x + w - 2 * bw - 8, y + 15, bw, 32), "Go back to this",
		              FL_BTN_OUTLINE)) {
			s->confirm = e->n;
			s->confirm_kind = 1;
		}
		if (fl_button(ui, ID_S_GO + 2 * (uint32_t)i + 1, fl_r(x + w - bw, y + 15, bw, 32), "Delete", FL_BTN_OUTLINE)) {
			s->confirm = e->n;
			s->confirm_kind = 2;
		}
	}
	return y + 24 - c.y;
}
