/* data-test.c -- sys.c, proc.c and startup.c against the host's /proc, /sys and a scratch
 * autostart directory. XDG_CONFIG_HOME and XDG_CONFIG_DIRS must point into a scratch dir
 * (check.sh does that). No window, no libflui.
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../gm.h"

static int fails;

#define CHECK(cond, ...) \
	do { \
		if (cond) \
			printf("ok   " __VA_ARGS__); \
		else { \
			printf("FAIL " __VA_ARGS__); \
			fails++; \
		} \
		printf("\n"); \
	} while (0)

static int file_has(const char *path, const char *needle)
{
	char buf[4096];
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	size_t n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = 0;
	return strstr(buf, needle) != NULL;
}

static struct startup *find(struct startup *v, int n, const char *file)
{
	for (int i = 0; i < n; i++)
		if (!strcmp(v[i].file, file))
			return &v[i];
	return NULL;
}

int main(void)
{
	/* formatting */
	CHECK(!strcmp(fmt_pct(0), "0%") && !strcmp(fmt_pct(3.24), "3.2%") && !strcmp(fmt_pct(42.4), "42%"), "fmt_pct");
	CHECK(!strcmp(fmt_mem(250 * 1024), "250.0 MB") && !strcmp(fmt_mem(2048 * 1024), "2.00 GB"), "fmt_mem");
	CHECK(!strcmp(fmt_rate(0), "0 MB/s") && !strcmp(fmt_rate(3 * 1048576.0), "3.0 MB/s"), "fmt_rate");
	CHECK(!strcmp(fmt_bits(12.5e6), "12.5 Mbps") && !strcmp(fmt_uptime(90061), "1:01:01:01"), "fmt_bits, fmt_uptime");
	CHECK(nice_ceil(0.3) == 0.5 && nice_ceil(7) == 10 && nice_ceil(120) == 200, "nice_ceil");

	/* machine numbers */
	static struct sys s;
	sys_init(&s);
	sys_sample(&s);
	usleep(400000);
	sys_sample(&s);
	CHECK(s.ncpu >= 1 && s.ci.model[0], "cpu: %d logical, %s", s.ncpu, s.ci.model);
	CHECK(s.cpu_total >= 0 && s.cpu_total <= 100 && s.h_cpu.n >= 1, "cpu utilization %.1f%%", s.cpu_total);
	CHECK(s.mem_total > 0 && s.mem_avail > 0 && s.mem_avail <= s.mem_total, "memory %lu / %lu kB", (unsigned long)s.mem_avail,
	      (unsigned long)s.mem_total);
	CHECK(s.uptime > 0 && s.nproc > 0 && s.nthreads >= s.nproc, "uptime %.0f s, %d processes, %d threads", s.uptime, s.nproc,
	      s.nthreads);
	printf("note %d disks, %d adapters, %d gpus\n", s.ndisk, s.nnet, s.ngpu);
	for (int i = 0; i < s.ndisk; i++)
		CHECK(s.disk[i].active >= 0 && s.disk[i].active <= 100, "disk %s active %.1f%%", s.disk[i].name, s.disk[i].active);
	for (int i = 0; i < s.nnet; i++)
		CHECK(s.net[i].name[0] && strcmp(s.net[i].name, "lo"), "adapter %s", s.net[i].name);

	/* processes */
	static struct procs ps;
	procs_init(&ps, s.ncpu);
	procs_scan(&ps);
	usleep(300000);
	procs_scan(&ps);
	struct proc *me = procs_find(&ps, getpid());
	CHECK(ps.n > 3 && me, "process table: %d processes, self found", ps.n);
	if (me) {
		CHECK(me->name[0] && me->cmd && me->uid == getuid() && me->priv_kb > 0, "self: %s, uid %u, %lu kB", me->name, me->uid,
		      (unsigned long)me->priv_kb);
		CHECK(me->io_ok, "own /proc/PID/io readable");
	}
	struct proc *init = procs_find(&ps, 1);
	CHECK(init && init->ppid == 0, "pid 1 present");
	struct proc *kt = procs_find(&ps, 2);
	CHECK(!kt || kt->kernel, "pid 2 is a kernel thread (if visible)");
	int sorted = 1;
	for (int i = 1; i < ps.n; i++)
		if (ps.p[i - 1].pid >= ps.p[i].pid)
			sorted = 0;
	CHECK(sorted, "table sorted by pid");
	CHECK(user_name(0)[0] && !strcmp(state_name('R'), "Running"), "user_name, state_name");

	/* startup apps: scratch dirs from the environment */
	const char *ud = startup_user_dir();
	CHECK(ud[0] && strstr(ud, "autostart"), "user dir %s", ud);
	struct startup *v = NULL;
	int n = startup_load(&v);
	struct startup *a = find(v, n, "alpha.desktop"), *b = find(v, n, "beta.desktop"), *c = find(v, n, "sys-one.desktop");
	CHECK(n == 3 && a && b && c, "three startup entries found (%d)", n);
	if (a && b && c) {
		CHECK(a->user && a->enabled && !strcmp(a->name, "Alpha") && !strcmp(a->exec, "alpha --serve"), "user entry read");
		CHECK(b->user && !b->enabled, "Hidden=true reads as disabled");
		CHECK(!c->user && c->enabled, "system entry read");
		CHECK(startup_set(a, 0) == 0 && file_has(a->path, "Hidden=true") && file_has(a->path, "Exec=alpha --serve"),
		      "disable user entry keeps the file and adds Hidden=true");
		CHECK(startup_set(b, 1) == 0 && !file_has(b->path, "Hidden"), "enable removes Hidden=true");
		CHECK(startup_set(c, 0) == 0, "disable system entry writes an override");
	}
	free(v);
	n = startup_load(&v);
	a = find(v, n, "alpha.desktop");
	b = find(v, n, "beta.desktop");
	c = find(v, n, "sys-one.desktop");
	CHECK(a && !a->enabled && b && b->enabled && c && !c->enabled && !c->user && n == 3, "state after reload");
	if (c) {
		char p[600];
		snprintf(p, sizeof p, "%s/sys-one.desktop", ud);
		CHECK(file_has(p, "X-FreeLFS-Override=true"), "override file marked");
		CHECK(startup_set(c, 1) == 0 && access(p, F_OK) != 0, "enable removes the override again");
	}
	free(v);
	n = startup_load(&v);
	c = find(v, n, "sys-one.desktop");
	CHECK(c && c->enabled && n == 3, "system entry enabled again");
	free(v);

	printf("%s\n", fails ? "FAILED" : "all passed");
	return fails != 0;
}
