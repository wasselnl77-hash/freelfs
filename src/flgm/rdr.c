/* rdr.c -- the reads of /proc that can block, done on a worker thread with a timeout
 * SPDX-License-Identifier: MIT
 *
 * /proc/PID/cmdline and /proc/PID/smaps_rollup take the process's mm lock. A process stuck in a
 * hung page fault (NFS, FUSE, a USB disk gone away) holds that lock, so reading its cmdline or
 * smaps_rollup blocks until the fault returns, which may be never. The window must not wait for
 * that. So the reads are queued here, one worker thread does them, and the caller collects the
 * answers on a later scan. A read still running after rdr_set_timeout seconds is given up: its
 * job is marked failed and a new worker takes over the queue. The stuck thread exits by itself
 * when its read returns (it sees that its generation is over and writes nothing).
 */
#include "gm.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define JOBS 512
#define KEEP_S 10.0    /* a finished job nobody collected, or a queued one, is dropped after this */
#define MAX_STUCK 16   /* after this many hung reads the reader stops and callers use the fallbacks */

enum { J_FREE, J_QUEUED, J_RUNNING, J_DONE };

struct job {
	int state, kind, pid, ok;
	u64 start, seq;
	double t; /* submitted, started, or finished: the state says which */
	u64 a, b;
	char buf[1024];
};

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static struct job jobs[JOBS];
static u64 next_seq = 1;
static unsigned gen = 1;     /* the only worker allowed to store results */
static unsigned started_gen; /* generation of the last worker started (valid if started) */
static int started, nstuck, disabled, sync_mode;
static double timeout_s = 3.0;

void rdr_set_sync(int on)
{
	pthread_mutex_lock(&mu);
	sync_mode = on;
	pthread_mutex_unlock(&mu);
}

void rdr_set_timeout(double seconds)
{
	pthread_mutex_lock(&mu);
	timeout_s = seconds > 0 ? seconds : 3.0;
	pthread_mutex_unlock(&mu);
}

/* The read itself. Runs without the lock: this is the part that may block. */
static int do_read(int kind, int pid, char *buf, size_t cap, u64 *a, u64 *b)
{
	char path[96];
	if (kind == RDR_CMDLINE) {
		snprintf(path, sizeof path, PROC_DIR "/%d/cmdline", pid);
		int fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			return -1;
		ssize_t n = read(fd, buf, cap - 1);
		close(fd);
		if (n < 0)
			return -1;
		buf[n] = 0;
		*a = (u64)n;
		return 0;
	}
	snprintf(path, sizeof path, PROC_DIR "/%d/smaps_rollup", pid);
	FILE *f = fopen(path, "re");
	if (!f)
		return -1;
	char line[160];
	u64 pss = 0, anon = 0;
	int got = 0;
	while (fgets(line, sizeof line, f)) {
		if (!strncmp(line, "Pss:", 4)) {
			pss = strtoull(line + 4, NULL, 10);
			got |= 1;
		} else if (!strncmp(line, "Pss_Anon:", 9)) {
			anon = strtoull(line + 9, NULL, 10);
			got |= 2;
		}
	}
	fclose(f);
	if (got != 3)
		return -1; /* an old kernel without Pss_Anon: the caller falls back to RSS */
	*a = pss;
	*b = anon;
	return 0;
}

/* Starts a worker for the current generation unless one is running. Called with mu held. */
static void ensure_worker(void);

static void *worker(void *arg)
{
	unsigned my = (unsigned)(uintptr_t)arg;
	for (;;) {
		pthread_mutex_lock(&mu);
		int i = -1;
		while (my == gen) {
			for (int k = 0; k < JOBS && i < 0; k++)
				if (jobs[k].state == J_QUEUED)
					i = k;
			if (i >= 0)
				break;
			pthread_cond_wait(&cv, &mu);
		}
		if (my != gen) { /* replaced: this thread is done */
			pthread_mutex_unlock(&mu);
			return NULL;
		}
		struct job *j = &jobs[i];
		j->state = J_RUNNING;
		j->t = now_s();
		int kind = j->kind, pid = j->pid;
		u64 seq = j->seq;
		pthread_mutex_unlock(&mu);

		char buf[1024] = "";
		u64 a = 0, b = 0;
		int r = do_read(kind, pid, buf, sizeof buf, &a, &b);

		pthread_mutex_lock(&mu);
		if (my == gen && j->seq == seq && j->state == J_RUNNING) {
			j->ok = r == 0;
			j->a = a;
			j->b = b;
			memcpy(j->buf, buf, sizeof buf);
			j->state = J_DONE;
			j->t = now_s();
		}
		pthread_mutex_unlock(&mu);
	}
}

static void ensure_worker(void)
{
	if (started && started_gen == gen)
		return;
	pthread_t th;
	pthread_attr_t at;
	if (pthread_attr_init(&at) != 0)
		goto off;
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	int rc = pthread_create(&th, &at, worker, (void *)(uintptr_t)gen);
	pthread_attr_destroy(&at);
	if (rc != 0)
		goto off;
	started = 1;
	started_gen = gen;
	return;
off:
	disabled = 1; /* no threads: the callers keep the RSS and the command names */
}

int rdr_submit(int kind, int pid, u64 start)
{
	pthread_mutex_lock(&mu);
	int rc = -1;
	if (!disabled) {
		int k = 0;
		while (k < JOBS && jobs[k].state != J_FREE)
			k++;
		if (k < JOBS) {
			struct job *j = &jobs[k];
			j->kind = kind;
			j->pid = pid;
			j->start = start;
			j->seq = next_seq++;
			j->ok = 0;
			j->a = j->b = 0;
			j->t = now_s();
			if (sync_mode) {
				char buf[1024] = "";
				u64 a = 0, b = 0;
				j->ok = do_read(kind, pid, buf, sizeof buf, &a, &b) == 0;
				j->a = a;
				j->b = b;
				memcpy(j->buf, buf, sizeof buf);
				j->state = J_DONE;
			} else {
				j->state = J_QUEUED;
				ensure_worker();
				pthread_cond_signal(&cv);
			}
			rc = 0;
		}
	}
	pthread_mutex_unlock(&mu);
	return rc;
}

int rdr_take(int kind, int pid, u64 start, char *buf, size_t cap, u64 *a, u64 *b)
{
	int rc = -1;
	pthread_mutex_lock(&mu);
	for (int k = 0; k < JOBS; k++) {
		struct job *j = &jobs[k];
		if (j->state == J_FREE || j->kind != kind || j->pid != pid || j->start != start)
			continue;
		if (j->state != J_DONE) {
			rc = 0;
			break;
		}
		rc = j->ok ? 1 : -1;
		if (j->ok) {
			if (buf && cap) { /* cmdline has NUL separators: copy by length, not as a string */
				size_t n = j->kind == RDR_CMDLINE && j->a < cap - 1 ? (size_t)j->a : cap - 1;
				memcpy(buf, j->buf, n);
				buf[n] = 0;
			}
			if (a)
				*a = j->a;
			if (b)
				*b = j->b;
		}
		j->state = J_FREE;
		break;
	}
	pthread_mutex_unlock(&mu);
	return rc;
}

void rdr_pump(void)
{
	double t = now_s();
	pthread_mutex_lock(&mu);
	int stuck = 0;
	for (int k = 0; k < JOBS; k++) {
		struct job *j = &jobs[k];
		if (j->state == J_RUNNING && t - j->t > timeout_s) {
			j->state = J_DONE; /* the caller sees a failure */
			j->ok = 0;
			j->t = t;
			stuck = 1;
		} else if (j->state == J_QUEUED && t - j->t > KEEP_S) {
			j->state = J_DONE;
			j->ok = 0;
			j->t = t;
		} else if (j->state == J_DONE && t - j->t > KEEP_S) {
			j->state = J_FREE; /* nobody came back for it */
		}
	}
	if (stuck) {
		gen++; /* the stuck worker's result will be ignored when its read returns */
		nstuck++;
		if (nstuck > MAX_STUCK) {
			disabled = 1;
		} else {
			ensure_worker();
		}
		pthread_cond_broadcast(&cv);
	}
	pthread_mutex_unlock(&mu);
}
