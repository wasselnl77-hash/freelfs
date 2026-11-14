/* cmd.c -- helper programs for the Settings pages, run without blocking the window
 * SPDX-License-Identifier: MIT
 *
 * seq_start runs argv lists one step after another. A step is a child whose stdout and stderr are
 * pipes read by the event loop (fl_fd_add); a timer reaps it once both pipes are closed. A step that
 * cannot start (no such program: ENOENT, reported through a close-on-exec pipe) or that runs past its
 * timeout is killed and counts as failed. SIGCHLD keeps its default disposition.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cmd.h"

#define TICK_MS 200
#define DEFAULT_TIMEOUT_MS 15000

struct seq {
	fl_app *app;
	struct cmd_line lines[CMD_MAX_STEPS];
	int n, step, timeout_ms, cancelled;
	seq_done_cb cb;
	void *ud;
	fl_timer *t;
	/* the running step */
	pid_t pid;
	int fd_out, fd_err, open, reaped, status, spawn_errno, timed_out;
	char *out, *err;
	size_t nout, nerr;
	long deadline;
};

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* appends to a NUL terminated buffer, never past CMD_OUT_MAX; the rest of a long output is read and dropped */
static void append(char **buf, size_t *len, const char *d, size_t n)
{
	if (*len >= CMD_OUT_MAX)
		return;
	if (n > CMD_OUT_MAX - *len)
		n = CMD_OUT_MAX - *len;
	char *nb = realloc(*buf, *len + n + 1);
	if (!nb)
		return;
	memcpy(nb + *len, d, n);
	*len += n;
	nb[*len] = 0;
	*buf = nb;
}

/* ESC [ ... letter sequences (colours, cursor moves) and carriage returns, removed in place */
static void strip_ansi(char *s)
{
	char *w = s;
	if (!s)
		return;
	for (char *r = s; *r; r++) {
		if (*r == 27 && r[1] == '[') {
			r += 2;
			while (*r && !((*r >= 0x40 && *r <= 0x7e)))
				r++;
			if (!*r)
				break;
		} else if (*r != '\r') {
			*w++ = *r;
		}
	}
	*w = 0;
}

static void free_out(struct seq *s)
{
	free(s->out);
	free(s->err);
	s->out = s->err = NULL;
	s->nout = s->nerr = 0;
}

static void close_fd(struct seq *s, int *fd)
{
	if (*fd < 0)
		return;
	fl_fd_remove(s->app, *fd);
	close(*fd);
	*fd = -1;
	s->open--;
}

static void step_done(struct seq *s);

static void on_pipe(void *ud, int fd, short rev)
{
	struct seq *s = ud;
	char buf[4096];
	ssize_t n = read(fd, buf, sizeof buf);
	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (n > 0) {
		if (fd == s->fd_out)
			append(&s->out, &s->nout, buf, (size_t)n);
		else
			append(&s->err, &s->nerr, buf, (size_t)n);
		return;
	}
	close_fd(s, fd == s->fd_out ? &s->fd_out : &s->fd_err);
	if (!s->open)
		fl_timer_arm(s->t, 5, 5);
}

static void tick(void *ud)
{
	struct seq *s = ud;
	if (s->open) {
		if (s->timed_out || !s->timeout_ms || now_ms() < s->deadline)
			return;
		s->timed_out = 1;
		kill(s->pid, SIGKILL);
		close_fd(s, &s->fd_out);
		close_fd(s, &s->fd_err);
	}
	if (!s->reaped) {
		int st = 0;
		pid_t r = waitpid(s->pid, &st, WNOHANG);
		if (r == 0 || (r < 0 && errno == EINTR)) {
			fl_timer_arm(s->t, 5, 5);
			return;
		}
		s->reaped = 1;
		s->status = r == s->pid ? st : -1;
	}
	step_done(s);
}

static void start_step(struct seq *s)
{
	const struct cmd_line *L = &s->lines[s->step];
	char *argv[CMD_ARGS + 1];
	int po[2] = {-1, -1}, pe[2] = {-1, -1}, px[2] = {-1, -1};
	int e = 0, n = 0;
	pid_t p;

	free_out(s);
	s->pid = 0;
	s->fd_out = s->fd_err = -1;
	s->open = s->reaped = s->status = s->spawn_errno = s->timed_out = 0;
	for (int i = 0; i < L->n && i < CMD_ARGS; i++)
		argv[n++] = (char *)L->arg[i];
	argv[n] = NULL;
	if (n < 1) {
		e = EINVAL;
		goto fail;
	}
	if (pipe2(po, O_CLOEXEC) < 0 || pipe2(pe, O_CLOEXEC) < 0 || pipe2(px, O_CLOEXEC) < 0) {
		e = errno;
		goto fail;
	}
	p = fork();
	if (p < 0) {
		e = errno;
		goto fail;
	}
	if (p == 0) {
		int nul = open("/dev/null", O_RDONLY | O_CLOEXEC);
		if (nul >= 0)
			dup2(nul, 0);
		dup2(po[1], 1);
		dup2(pe[1], 2);
		execvp(argv[0], argv);
		int ec = errno;
		ssize_t w = write(px[1], &ec, sizeof ec);
		(void)w;
		_exit(127);
	}
	close(po[1]);
	close(pe[1]);
	close(px[1]);
	/* the close-on-exec pipe: EOF after a successful exec, the errno when exec failed */
	int got = -1;
	ssize_t rn;
	do
		rn = read(px[0], &got, sizeof got);
	while (rn < 0 && errno == EINTR);
	if (rn == (ssize_t)sizeof got)
		s->spawn_errno = got;
	close(px[0]);
	fcntl(po[0], F_SETFL, O_NONBLOCK);
	fcntl(pe[0], F_SETFL, O_NONBLOCK);
	s->pid = p;
	s->fd_out = po[0];
	s->fd_err = pe[0];
	s->open = 2;
	s->deadline = now_ms() + s->timeout_ms;
	fl_fd_add(s->app, s->fd_out, POLLIN, on_pipe, s);
	fl_fd_add(s->app, s->fd_err, POLLIN, on_pipe, s);
	fl_timer_arm(s->t, TICK_MS, TICK_MS);
	return;

fail:
	close(po[0]);
	close(po[1]);
	close(pe[0]);
	close(pe[1]);
	close(px[0]);
	close(px[1]);
	s->spawn_errno = e ? e : EIO;
	s->reaped = 1;
	/* report from the loop, never from inside seq_start */
	fl_timer_arm(s->t, 1, 0);
}

static int has_fail(const char *t)
{
	return t && (strstr(t, "Failed") || strstr(t, "failed"));
}

static void step_done(struct seq *s)
{
	struct cmd_result r;
	int bad, failed;
	seq_done_cb cb;
	void *ud;

	if (s->cancelled) {
		fl_timer_free(s->t);
		free_out(s);
		free(s);
		return;
	}
	strip_ansi(s->out);
	strip_ansi(s->err);
	r.spawn_errno = s->spawn_errno;
	r.timed_out = s->timed_out;
	r.code = s->spawn_errno ? 127 : s->timed_out ? -1 : WIFEXITED(s->status) ? WEXITSTATUS(s->status) : -1;
	r.out = s->out ? s->out : "";
	r.err = s->err ? s->err : "";
	bad = s->spawn_errno || s->timed_out || r.code != 0 ||
	      (s->lines[s->step].text_fail && (has_fail(s->out) || has_fail(s->err)));
	if (!bad && s->step + 1 < s->n) {
		s->step++;
		start_step(s);
		return;
	}
	cb = s->cb;
	ud = s->ud;
	failed = bad ? s->step : -1;
	s->cb = NULL;
	fl_timer_free(s->t);
	s->t = NULL;
	if (cb)
		cb(ud, failed, &r);
	free_out(s);
	free(s);
}

struct seq *seq_start(fl_app *app, const struct cmd_line *lines, int n, int timeout_ms, seq_done_cb cb, void *ud)
{
	struct seq *s;
	if (!app || !lines || n < 1 || n > CMD_MAX_STEPS)
		return NULL;
	s = calloc(1, sizeof *s);
	if (!s)
		return NULL;
	s->t = fl_timer_new(app, tick, s);
	if (!s->t) {
		free(s);
		return NULL;
	}
	s->app = app;
	memcpy(s->lines, lines, (size_t)n * sizeof *lines);
	s->n = n;
	s->timeout_ms = timeout_ms > 0 ? timeout_ms : DEFAULT_TIMEOUT_MS;
	s->cb = cb;
	s->ud = ud;
	s->fd_out = s->fd_err = -1;
	start_step(s);
	return s;
}

void seq_cancel(struct seq *s)
{
	if (!s)
		return;
	s->cb = NULL;
	s->cancelled = 1;
	if (!s->reaped && s->pid > 0)
		kill(s->pid, SIGKILL);
	close_fd(s, &s->fd_out);
	close_fd(s, &s->fd_err);
	/* the timer reaps the child and frees the sequence */
	if (!s->reaped)
		fl_timer_arm(s->t, 5, 5);
	else
		fl_timer_arm(s->t, 1, 0);
}

void cmd_message(const struct cmd_result *r, char *buf, size_t n)
{
	const char *t[2] = {r->err, r->out};
	if (n < 1)
		return;
	buf[0] = 0;
	if (r->spawn_errno) {
		snprintf(buf, n, "%s", strerror(r->spawn_errno));
		return;
	}
	if (r->timed_out) {
		snprintf(buf, n, "it took too long");
		return;
	}
	for (int k = 0; k < 2; k++) {
		const char *p = t[k];
		while (p && *p) {
			const char *e = strchr(p, '\n');
			const char *a = p;
			if (!e)
				e = p + strlen(p);
			while (a < e && (*a == ' ' || *a == '\t'))
				a++;
			if (e > a) {
				size_t m = (size_t)(e - a);
				if (m > n - 1)
					m = n - 1;
				memcpy(buf, a, m);
				buf[m] = 0;
				return;
			}
			p = *e ? e + 1 : e;
		}
	}
}
