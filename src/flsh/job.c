/* Jobs: waiting for processes and, in interactive shells, job control
 * (process groups, the terminal's foreground group, stop and continue).
 * SPDX-License-Identifier: MIT */
#include "flsh.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

pid_t shell_pgid;
int tty_fd = -1;
static struct termios shell_tmodes;
static bool have_tmodes;

struct jobx {
	struct job j;
	struct termios tmodes;
	bool have_tmodes;
};

static struct jobx **jobs;
static size_t njobs, capjobs;
static int cur_id, prev_id;

void set_tty_pgrp(pid_t pgid)
{
	if (tty_fd < 0)
		return;
	sigset_t set, old;
	sigemptyset(&set);
	sigaddset(&set, SIGTTOU);
	sigprocmask(SIG_BLOCK, &set, &old);
	tcsetpgrp(tty_fd, pgid);
	sigprocmask(SIG_SETMASK, &old, NULL);
}

void jobs_init(void)
{
	int fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return;
	tty_fd = move_fd_high(fd);
	if (tty_fd < 0)
		return;
	/* wait until we are in the foreground */
	for (;;) {
		pid_t fg = tcgetpgrp(tty_fd);
		if (fg < 0) {
			close(tty_fd);
			tty_fd = -1;
			return;
		}
		if (fg == getpgrp())
			break;
		kill(-getpgrp(), SIGTTIN);
	}
	sh.o_monitor = true;
	interactive_signals();
	if (getpid() != getsid(0))
		setpgid(0, 0);
	shell_pgid = getpgrp();
	set_tty_pgrp(shell_pgid);
	have_tmodes = tcgetattr(tty_fd, &shell_tmodes) == 0;
}

static int new_id(void)
{
	for (int id = 1;; id++) {
		bool used = false;
		for (size_t i = 0; i < njobs; i++)
			if (jobs[i]->j.id == id)
				used = true;
		if (!used)
			return id;
	}
}

struct job *job_new(pid_t pgid, bool bg, const char *cmd)
{
	struct jobx *x = xcalloc(1, sizeof(*x));
	x->j.pgid = pgid;
	x->j.bg = bg;
	x->j.cmd = cmd ? xstrdup(cmd) : NULL;
	if (bg) {
		x->j.id = new_id();
		prev_id = cur_id;
		cur_id = x->j.id;
	}
	if (njobs == capjobs) {
		capjobs = capjobs ? capjobs * 2 : 8;
		jobs = xrealloc(jobs, capjobs * sizeof(*jobs));
	}
	jobs[njobs++] = x;
	return &x->j;
}

void job_add_proc(struct job *j, pid_t pid, const char *cmd)
{
	j->procs = xrealloc(j->procs, (j->nprocs + 1) * sizeof(struct proc));
	struct proc *p = &j->procs[j->nprocs++];
	memset(p, 0, sizeof(*p));
	p->pid = pid;
	p->cmd = xstrdup(cmd ? cmd : "");
}

static void job_remove(struct job *j)
{
	for (size_t i = 0; i < njobs; i++)
		if (&jobs[i]->j == j) {
			for (size_t k = 0; k < j->nprocs; k++)
				free(j->procs[k].cmd);
			free(j->procs);
			free(j->cmd);
			if (cur_id == j->id && j->id) {
				cur_id = prev_id;
				prev_id = 0;
			} else if (prev_id == j->id)
				prev_id = 0;
			free(jobs[i]);
			memmove(jobs + i, jobs + i + 1, (njobs - i - 1) * sizeof(*jobs));
			njobs--;
			return;
		}
}

void jobs_forget(void)
{
	while (njobs)
		job_remove(&jobs[njobs - 1]->j);
	cur_id = prev_id = 0;
}

static bool record(pid_t pid, int status)
{
	for (size_t i = 0; i < njobs; i++) {
		struct job *j = &jobs[i]->j;
		for (size_t k = 0; k < j->nprocs; k++) {
			struct proc *p = &j->procs[k];
			if (p->pid != pid)
				continue;
			if (WIFSTOPPED(status)) {
				p->stopped = true;
			} else if (WIFCONTINUED(status)) {
				p->stopped = false;
			} else {
				p->done = true;
				p->stopped = false;
				p->status = status;
			}
			j->changed = true;
			j->notified = false;
			return true;
		}
	}
	return false;
}

void job_reap(bool block)
{
	for (;;) {
		int status;
		pid_t pid = waitpid(-1, &status, (block ? 0 : WNOHANG) | WUNTRACED | WCONTINUED);
		if (pid <= 0)
			return;
		record(pid, status);
	}
}

bool job_running(struct job *j)
{
	for (size_t k = 0; k < j->nprocs; k++)
		if (!j->procs[k].done && !j->procs[k].stopped)
			return true;
	return false;
}

static bool job_done(struct job *j)
{
	for (size_t k = 0; k < j->nprocs; k++)
		if (!j->procs[k].done)
			return false;
	return true;
}

static bool job_stopped(struct job *j)
{
	return !job_done(j) && !job_running(j);
}

static int proc_status(const struct proc *p)
{
	if (WIFEXITED(p->status))
		return WEXITSTATUS(p->status);
	if (WIFSIGNALED(p->status))
		return 128 + WTERMSIG(p->status);
	return 0;
}

int job_status(struct job *j)
{
	if (!j->nprocs)
		return 0;
	if (sh.o_pipefail)
		for (size_t k = j->nprocs; k-- > 0;)
			if (proc_status(&j->procs[k]))
				return proc_status(&j->procs[k]);
	return proc_status(&j->procs[j->nprocs - 1]);
}

static const char *state_text(struct job *j, char *buf, size_t n)
{
	if (job_stopped(j))
		return "Stopped";
	if (!job_done(j))
		return "Running";
	const struct proc *p = &j->procs[j->nprocs - 1];
	if (WIFSIGNALED(p->status)) {
		snprintf(buf, n, "%s%s", strsignal(WTERMSIG(p->status)),
		         WCOREDUMP(p->status) ? " (core dumped)" : "");
		return buf;
	}
	int st = proc_status(p);
	if (!st)
		return "Done";
	snprintf(buf, n, "Exit %d", st);
	return buf;
}

static void print_job(struct job *j, bool long_fmt)
{
	char buf[128];
	char mark = j->id == cur_id ? '+' : j->id == prev_id ? '-' : ' ';
	if (long_fmt)
		printf("[%d]%c %ld %-22s %s\n", j->id, mark, (long)j->pgid, state_text(j, buf, sizeof(buf)),
		       j->cmd ? j->cmd : "");
	else
		printf("[%d]%c  %-22s %s\n", j->id, mark, state_text(j, buf, sizeof(buf)),
		       j->cmd ? j->cmd : "");
	fflush(stdout);
}

/* Wait until every process of a foreground job is done or stopped. */
int job_wait_fg(struct job *j)
{
	bool jc = sh.o_monitor && !sh.subshell && j->pgid;
	while (!job_done(j) && job_running(j)) {
		int status;
		pid_t pid = waitpid(-1, &status, WUNTRACED);
		if (pid < 0) {
			if (errno == EINTR)
				continue;
			for (size_t k = 0; k < j->nprocs; k++)
				j->procs[k].done = true; /* ECHILD: nothing left to wait for */
			break;
		}
		record(pid, status);
	}
	struct jobx *x = (struct jobx *)j;
	if (jc) {
		set_tty_pgrp(shell_pgid);
		if (job_stopped(j)) {
			x->have_tmodes = tcgetattr(tty_fd, &x->tmodes) == 0;
		}
	}
	if (job_stopped(j)) {
		if (!j->id)
			j->id = new_id();
		prev_id = cur_id;
		cur_id = j->id;
		j->bg = true;
		j->notified = true;
		if (jc && have_tmodes)
			tcsetattr(tty_fd, TCSADRAIN, &shell_tmodes);
		fputc('\n', stderr);
		fflush(stderr);
		print_job(j, false);
		int sig = SIGTSTP;
		for (size_t k = 0; k < j->nprocs; k++)
			if (j->procs[k].stopped && WIFSTOPPED(j->procs[k].status))
				sig = WSTOPSIG(j->procs[k].status);
		return 128 + sig;
	}
	int st = job_status(j);
	const struct proc *last = j->nprocs ? &j->procs[j->nprocs - 1] : NULL;
	bool interrupted = false;
	if (last && WIFSIGNALED(last->status)) {
		int sig = WTERMSIG(last->status);
		if (jc && have_tmodes)
			tcsetattr(tty_fd, TCSADRAIN, &shell_tmodes);
		if (sig == SIGINT)
			interrupted = true;
		else if (sig != SIGPIPE && sh.hints) {
			fprintf(stderr, "%s%s\n", strsignal(sig), WCOREDUMP(last->status) ? " (core dumped)" : "");
		}
	}
	job_remove(j);
	if (interrupted && sh.interactive && !sh.subshell) {
		fputc('\n', stderr);
		int_pending = 1; /* stop the rest of the command line */
	}
	return st;
}

void job_notify(bool all)
{
	job_reap(false);
	for (size_t i = 0; i < njobs;) {
		struct job *j = &jobs[i]->j;
		if (!j->bg || (!j->changed && !all)) {
			i++;
			continue;
		}
		if (job_done(j)) {
			print_job(j, false);
			job_remove(j);
			continue;
		}
		if (j->changed && !j->notified && job_stopped(j))
			print_job(j, false);
		j->changed = false;
		j->notified = true;
		i++;
	}
}

void job_free_done(void)
{
	for (size_t i = 0; i < njobs;) {
		struct job *j = &jobs[i]->j;
		if (j->bg && job_done(j))
			job_remove(j);
		else
			i++;
	}
}

struct job *job_find(const char *spec, bool quiet)
{
	int id = -1;
	if (!spec || !strcmp(spec, "%") || !strcmp(spec, "%%") || !strcmp(spec, "%+"))
		id = cur_id;
	else if (!strcmp(spec, "%-"))
		id = prev_id;
	else {
		const char *s = spec[0] == '%' ? spec + 1 : spec;
		long n;
		if (str_to_int(s, &n) == 0) {
			if (spec[0] == '%')
				id = (int)n;
			else
				for (size_t i = 0; i < njobs; i++) /* a process id */
					for (size_t k = 0; k < jobs[i]->j.nprocs; k++)
						if (jobs[i]->j.procs[k].pid == n)
							return &jobs[i]->j;
		} else {
			bool contains = s[0] == '?';
			if (contains)
				s++;
			struct job *found = NULL;
			for (size_t i = 0; i < njobs; i++) {
				struct job *j = &jobs[i]->j;
				if (!j->bg || !j->cmd)
					continue;
				if (contains ? strstr(j->cmd, s) != NULL : has_prefix(j->cmd, s)) {
					if (found) {
						if (!quiet)
							sh_warn("%s: more than one job matches", spec);
						return NULL;
					}
					found = j;
				}
			}
			if (found)
				return found;
		}
	}
	for (size_t i = 0; i < njobs; i++)
		if (jobs[i]->j.id == id && jobs[i]->j.bg)
			return &jobs[i]->j;
	if (!quiet)
		sh_warn("%s: no such job", spec ? spec : "current");
	return NULL;
}

void job_list(bool long_fmt, bool pids_only)
{
	job_reap(false);
	for (size_t i = 0; i < njobs;) {
		struct job *j = &jobs[i]->j;
		if (!j->bg) {
			i++;
			continue;
		}
		if (pids_only)
			printf("%ld\n", (long)(j->pgid ? j->pgid : j->procs[0].pid));
		else
			print_job(j, long_fmt);
		if (job_done(j)) {
			job_remove(j);
			continue;
		}
		j->changed = false;
		j->notified = true;
		i++;
	}
}

static void continue_job(struct job *j)
{
	for (size_t k = 0; k < j->nprocs; k++)
		j->procs[k].stopped = false;
	if (j->pgid)
		kill(-j->pgid, SIGCONT);
	else
		for (size_t k = 0; k < j->nprocs; k++)
			if (!j->procs[k].done)
				kill(j->procs[k].pid, SIGCONT);
}

int job_fg(struct job *j)
{
	printf("%s\n", j->cmd ? j->cmd : "");
	fflush(stdout);
	struct jobx *x = (struct jobx *)j;
	if (sh.o_monitor && j->pgid) {
		set_tty_pgrp(j->pgid);
		if (x->have_tmodes)
			tcsetattr(tty_fd, TCSADRAIN, &x->tmodes);
	}
	j->bg = false;
	if (cur_id == j->id) {
		cur_id = prev_id;
		prev_id = 0;
	}
	continue_job(j);
	return job_wait_fg(j);
}

int job_bg(struct job *j)
{
	j->bg = true;
	prev_id = cur_id == j->id ? prev_id : cur_id;
	cur_id = j->id;
	printf("[%d]+ %s &\n", j->id, j->cmd ? j->cmd : "");
	fflush(stdout);
	continue_job(j);
	return 0;
}

/* Wait for one process: a job's (wait PID) or a plain child ($(...)) */
int job_wait_pid(pid_t pid)
{
	for (size_t i = 0; i < njobs; i++) {
		struct job *j = &jobs[i]->j;
		for (size_t k = 0; k < j->nprocs; k++) {
			if (j->procs[k].pid != pid)
				continue;
			while (!j->procs[k].done) {
				int status;
				pid_t w = waitpid(-1, &status, 0);
				if (w < 0) {
					if (errno == EINTR) {
						check_traps();
						return 128 + last_signal;
					}
					j->procs[k].done = true;
					break;
				}
				record(w, status);
			}
			int st = proc_status(&j->procs[k]);
			if (job_done(j))
				job_remove(j);
			return st;
		}
	}
	int status;
	for (;;) {
		pid_t w = waitpid(pid, &status, 0);
		if (w == pid)
			break;
		if (w < 0 && errno != EINTR)
			return 127;
	}
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return 0;
}

int job_wait_all(void)
{
	for (;;) {
		bool any = false;
		for (size_t i = 0; i < njobs; i++)
			if (jobs[i]->j.bg && job_running(&jobs[i]->j))
				any = true;
		if (!any)
			break;
		int status;
		pid_t w = waitpid(-1, &status, 0);
		if (w < 0) {
			if (errno == EINTR) {
				check_traps();
				return 128 + last_signal;
			}
			break;
		}
		record(w, status);
	}
	job_free_done();
	return 0;
}

size_t job_count(bool stopped_only)
{
	size_t n = 0;
	for (size_t i = 0; i < njobs; i++)
		if (jobs[i]->j.bg && (!stopped_only || job_stopped(&jobs[i]->j)))
			n++;
	return n;
}

/* On exit: stopped jobs get SIGHUP and SIGCONT, so they do not linger */
void jobs_hangup(void)
{
	for (size_t i = 0; i < njobs; i++) {
		struct job *j = &jobs[i]->j;
		if (j->pgid && job_stopped(j)) {
			kill(-j->pgid, SIGHUP);
			kill(-j->pgid, SIGCONT);
		}
	}
}
