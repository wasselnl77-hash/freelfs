/* fake-test.c -- sys.c and proc.c against a fake /proc and /sys (built with PROC_DIR and SYS_DIR
 * pointing into a scratch tree, see check.sh). Many processes, long and non-UTF-8 names, vanished
 * entries, reused pids, missing files, counters that go backwards. Nothing outside the tree is read.
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../gm.h"

static int fails;

#define CHECK(cond, ...)                 \
	do {                                 \
		if (cond)                        \
			printf("ok   " __VA_ARGS__); \
		else {                           \
			printf("FAIL " __VA_ARGS__); \
			fails++;                     \
		}                                \
		printf("\n");                    \
	} while (0)

static void wr(const char *path, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void wr(const char *path, const char *fmt, ...)
{
	FILE *f = fopen(path, "wb");
	if (!f) {
		printf("cannot write %s: %s\n", path, strerror(errno));
		exit(2);
	}
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fclose(f);
}

static void dir(const char *path)
{
	mkdir(path, 0755);
}

static void proc_entry_pp(int pid, int ppid, const char *comm, unsigned long long start, unsigned long long ticks,
                          const char *cmdline, int with_io)
{
	char p[512];
	snprintf(p, sizeof p, PROC_DIR "/%d", pid);
	dir(p);
	snprintf(p, sizeof p, PROC_DIR "/%d/stat", pid);
	/* 52 fields: the ones flgm reads, the rest zero */
	wr(p, "%d (%s) S %d %d %d 0 -1 4194304 100 0 0 0 %llu 0 0 0 20 0 1 0 %llu 1000000 300 18446744073709551615\n",
	   pid, comm, ppid, pid, pid, ticks, start);
	snprintf(p, sizeof p, PROC_DIR "/%d/statm", pid);
	wr(p, "1000 500 100 1 0 300 0\n");
	snprintf(p, sizeof p, PROC_DIR "/%d/cmdline", pid);
	wr(p, "%s", cmdline);
	if (with_io) {
		snprintf(p, sizeof p, PROC_DIR "/%d/io", pid);
		wr(p, "rchar: 1\nwchar: 2\nread_bytes: 4096\nwrite_bytes: 8192\n");
	}
}

static void proc_entry(int pid, const char *comm, unsigned long long start, unsigned long long ticks,
                       const char *cmdline, int with_io)
{
	proc_entry_pp(pid, 1, comm, start, ticks, cmdline, with_io);
}

static void build_tree(void)
{
	dir(PROC_DIR);
	dir(PROC_DIR "/net");
	for (int i = 0; i < 300; i++) {
		char cmd[64];
		snprintf(cmd, sizeof cmd, "/usr/bin/app%d --flag", i);
		int pid = 100 + i;
		proc_entry(pid, i % 50 == 7 ? "weird\xff\xfe name" : i % 50 == 8 ? "tab\tname" : "app", 1000 + i, 500,
		           cmd, i % 3 != 0);
	}
	/* a process with a 4 KB command line and a name that is UTF-8 */
	char *big = malloc(4200);
	memset(big, 'x', 4199);
	for (int k = 0; k < 4199; k += 40)
		big[k] = ' ';
	big[4199] = 0;
	proc_entry(900, "longone", 5000, 10, big, 1);
	free(big);
	/* a kernel thread: no cmdline */
	proc_entry(901, "kworker/0:1", 5001, 0, "", 0);
	/* the stat of a comm with ')' in it */
	proc_entry(902, "a)b", 5002, 3, "/bin/a)b", 1);
	/* a half written stat file */
	char p[256];
	snprintf(p, sizeof p, PROC_DIR "/903");
	dir(p);
	snprintf(p, sizeof p, PROC_DIR "/903/stat");
	wr(p, "903 (");
	/* a non-numeric entry in /proc */
	dir(PROC_DIR "/self");
	/* a cpu-less machine file set */
	wr(PROC_DIR "/stat", "cpu  1 0 1 100 0 0 0 0 0 0\ncpu0 1 0 1 100 0 0 0 0 0 0\nintr 12\n");
	wr(PROC_DIR "/meminfo", "MemTotal: 1000000 kB\nMemFree: 300000 kB\nMemAvailable: 500000 kB\nCached: 100000 kB\n"
	                        "Buffers: 1000 kB\nSwapTotal: 0 kB\nSwapFree: 0 kB\nShmem: 40000 kB\nSlab: 60000 kB\n"
	                        "SUnreclaim: 30000 kB\nPageTables: 5000 kB\nKernelStack: 2000 kB\n");
	wr(PROC_DIR "/loadavg", "0.10 0.20 0.30 1/300 999\n");
	wr(PROC_DIR "/uptime", "1234.5 100.0\n");
	wr(PROC_DIR "/diskstats",
	   "   8       0 sda 100 0 800 50 200 0 1600 90 0 100 140\n"
	   "   8      16 sdb 10 0 80 5 20 0 160 9 0 10 14\n"
	   "   8      17 sdb1 10 0 80 5 20 0 160 9 0 10 14\n"
	   "   7       0 loop0 1 0 8 0 0 0 0 0 0 0 0\n");
	wr(PROC_DIR "/net/dev", "Inter-|\n face |bytes\n    lo: 1 1 0 0 0 0 0 0 1 1 0 0 0 0 0 0\n"
	                        "  eth0: 5000 1 0 0 0 0 0 0 9000 1 0 0 0 0 0 0\n");
	wr(PROC_DIR "/cpuinfo", "processor\t: 0\nmodel name\t:  Fake  CPU  9000\n");
	/* sysfs: sda complete, sdb without model or queue, a Wi-Fi card with no speed file, a GPU with no
	 * hwmon and no vram files */
	dir(SYS_DIR);
	dir(SYS_DIR "/block");
	dir(SYS_DIR "/block/sda");
	dir(SYS_DIR "/block/sda/device");
	dir(SYS_DIR "/block/sda/queue");
	wr(SYS_DIR "/block/sda/size", "2000000\n");
	wr(SYS_DIR "/block/sda/device/model", "FAKE DISK    \n");
	wr(SYS_DIR "/block/sda/queue/rotational", "0\n");
	dir(SYS_DIR "/block/sdb");
	wr(SYS_DIR "/block/sdb/size", "1000\n");
	dir(SYS_DIR "/class");
	dir(SYS_DIR "/class/net");
	dir(SYS_DIR "/class/net/eth0");
	wr(SYS_DIR "/class/net/eth0/operstate", "up\n");
	dir(SYS_DIR "/class/net/wlan0");
	dir(SYS_DIR "/class/net/wlan0/phy80211");
	wr(SYS_DIR "/class/net/wlan0/operstate", "down\n");
	dir(SYS_DIR "/class/drm");
	dir(SYS_DIR "/class/drm/card0");
	dir(SYS_DIR "/class/drm/card0/device");
	wr(SYS_DIR "/class/drm/card0/device/vendor", "0x1002\n");
	wr(SYS_DIR "/class/drm/card0/device/device", "0x164e\n");
	dir(SYS_DIR "/class/drm/card0-eDP-1");
	dir(SYS_DIR "/devices");
	dir(SYS_DIR "/devices/system");
	dir(SYS_DIR "/devices/system/cpu");
	dir(SYS_DIR "/devices/system/cpu/cpu0");
	dir(SYS_DIR "/devices/system/cpu/cpu0/cpufreq");
	wr(SYS_DIR "/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "1200000\n");
}

/* smaps_rollup (PSS) for three processes, and a Flatpak app: its desktop file says
 * "flatpak run ... org.mozilla.firefox", and its launcher, bwrap, the program and a content
 * process are the processes below. Pid 102 has no Pss line (RSS stands in). */
static void build_extra(void)
{
	char p[512], xdg[300];
	snprintf(p, sizeof p, PROC_DIR "/100/smaps_rollup");
	wr(p, "Rss: 2000 kB\nPss: 1500 kB\nPss_Anon: 900 kB\nPss_File: 600 kB\n");
	snprintf(p, sizeof p, PROC_DIR "/101/smaps_rollup");
	wr(p, "Pss: 700 kB\nPss_Anon: 100 kB\n");
	snprintf(p, sizeof p, PROC_DIR "/102/smaps_rollup");
	wr(p, "Rss: 1 kB\n");

	snprintf(xdg, sizeof xdg, PROC_DIR "/../xdg");
	dir(xdg);
	snprintf(p, sizeof p, "%s/applications", xdg);
	dir(p);
	snprintf(p, sizeof p, "%s/applications/firefox.desktop", xdg);
	wr(p, "[Desktop Entry]\nType=Application\nName=Firefox\n"
	      "Exec=flatpak run --branch=stable --arch=x86_64 --command=firefox org.mozilla.firefox @@u %%u @@\n"
	      "Icon=org.mozilla.firefox\n");
	setenv("XDG_DATA_HOME", xdg, 1);
	setenv("XDG_DATA_DIRS", PROC_DIR "/../no-such-dir", 1);
	apps_load();

	proc_entry_pp(2000, 1, "flatpak", 6000, 5, "flatpak run --branch=stable --arch=x86_64 --command=firefox org.mozilla.firefox", 0);
	proc_entry_pp(2001, 2000, "bwrap", 6001, 5, "bwrap --args 40 /app/bin/firefox", 0);
	proc_entry_pp(2002, 2001, "firefox", 6002, 900, "/app/lib/firefox/firefox -profile x", 1);
	snprintf(p, sizeof p, PROC_DIR "/2002/smaps_rollup");
	wr(p, "Pss: 5000 kB\nPss_Anon: 3000 kB\n");
	proc_entry_pp(2003, 2002, "Web Content", 6003, 40, "/app/lib/firefox/firefox -contentproc -childID 1", 0);
}

static int all_printable_utf8(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;
	while (*p) {
		if (*p < 0x20 && *p != 0)
			return 0;
		if (*p >= 0x80)
			return 0;
		p++;
	}
	return 1;
}

int main(void)
{
	rdr_set_sync(1); /* the hung-read test below switches to the worker thread */
	/* the sanitizer */
	char a[] = "a\xff" "b\xe2\x82", b[] = "\xc3\xa9t\xc3\xa9\x01", c[] = "ok";
	gm_clean_utf8(a);
	gm_clean_utf8(b);
	gm_clean_utf8(c);
	CHECK(!strcmp(a, "a?b??") && !strcmp(b, "\xc3\xa9t\xc3\xa9?") && !strcmp(c, "ok"), "gm_clean_utf8");

	build_tree();
	build_extra();

	/* the machine numbers against the fake tree */
	static struct sys s;
	sys_init(&s);
	CHECK(s.ncpu == 1 && !strcmp(s.ci.model, "Fake CPU 9000"), "cpu from fake cpuinfo: %d, '%s'", s.ncpu, s.ci.model);
	sys_sample(&s);
	/* counters that go backwards: a re-probed disk, a reset adapter */
	wr(PROC_DIR "/diskstats", "   8       0 sda 1 0 1 1 1 0 1 1 0 1 1\n   8      16 sdb 1 0 1 1 1 0 1 1 0 1 1\n");
	wr(PROC_DIR "/net/dev", "  eth0: 1 1 0 0 0 0 0 0 1 1 0 0 0 0 0 0\n");
	usleep(60000);
	sys_sample(&s);
	int disks_ok = 1;
	for (int i = 0; i < s.ndisk; i++) {
		struct disk *d = &s.disk[i];
		if (!(d->active >= 0 && d->active <= 100 && d->rd_bps >= 0 && d->wr_bps >= 0 && d->resp_ms >= 0 &&
		      d->rd_bps < 1e12 && d->wr_bps < 1e12))
			disks_ok = 0;
	}
	CHECK(disks_ok, "disk rates stay sane after the counters went backwards (%d disks)", s.ndisk);
	CHECK(s.ndisk >= 1 && s.disk[0].model[0] && !strcmp(s.disk[0].model, "FAKE DISK"), "disk model trimmed: '%s'",
	      s.ndisk ? s.disk[0].model : "");
	CHECK(s.nnet == 1 && s.net[0].rx_bps >= 0 && s.net[0].rx_bps < 1e12, "adapter rate after a counter reset");
	CHECK(s.ngpu == 1 && s.gpu[0].vram_total == 0 && s.gpu[0].temp_c == -1, "gpu without hwmon or vram files");
	CHECK(s.mem_total == 1000000 && s.mem_avail == 500000, "meminfo");
	CHECK(s.mem_shmem == 40000 && s.mem_unrec == 30000 && s.mem_pt == 5000 && s.mem_kstack == 2000, "meminfo: shmem and kernel parts");
	CHECK(!s.live, "no live mark in the fake tree");

	/* what "in use" is made of: the parts add up to it, whatever the numbers */
	struct memsplit m;
	u64 sum;
	mem_split(&m, &s, 300000);
	sum = m.apps + m.system + m.files + m.graphics + m.other;
	CHECK(m.used == 500000 && m.apps == 300000 && m.system == 37000 && m.files == 40000 && m.graphics == 0 &&
	          m.other == 123000 && sum == m.used,
	      "split: apps %lu, system %lu, files %lu, other %lu kB", (unsigned long)m.apps, (unsigned long)m.system,
	      (unsigned long)m.files, (unsigned long)m.other);
	s.gpu[0].gtt_used = 20480000; /* 20000 kB of GPU memory kept in system RAM */
	mem_split(&m, &s, 300000);
	sum = m.apps + m.system + m.files + m.graphics + m.other;
	CHECK(m.graphics == 20000 && m.files == 20000 && m.other == 123000 && sum == m.used,
	      "GPU memory counted once: graphics %lu, files %lu", (unsigned long)m.graphics, (unsigned long)m.files);
	mem_split(&m, &s, 900000); /* more than used: the parts are capped, the sum still holds */
	sum = m.apps + m.system + m.files + m.graphics + m.other;
	CHECK(m.apps == 500000 && m.other == 0 && sum == m.used, "capped split adds up to used");
	s.gpu[0].gtt_used = 0;
	/* a live system: the mark file makes sys_init report it */
	wr(LIVE_MARK, "live\n");
	static struct sys live;
	sys_init(&live);
	CHECK(live.live, "live mark seen by sys_init");
	unlink(LIVE_MARK);

	/* the process table: 300 processes plus the odd ones */
	static struct procs ps;
	procs_init(&ps, s.ncpu);
	procs_scan(&ps);
	CHECK(ps.n >= 300, "scan: %d processes", ps.n);
	CHECK(!ps.failed, "scan reports success");
	int sorted = 1, names_ok = 1, cmd_ok = 1, cpu_ok = 1;
	for (int i = 0; i < ps.n; i++) {
		if (i && ps.p[i - 1].pid >= ps.p[i].pid)
			sorted = 0;
		if (!all_printable_utf8(ps.p[i].name) || !all_printable_utf8(ps.p[i].comm))
			names_ok = 0;
		if (!ps.p[i].cmd || strlen(ps.p[i].cmd) >= 1024)
			cmd_ok = 0;
		if (!(ps.p[i].cpu >= 0 && ps.p[i].cpu < 1e6))
			cpu_ok = 0;
	}
	CHECK(sorted, "table sorted by pid");
	CHECK(names_ok, "names are valid UTF-8, no control bytes");
	CHECK(cmd_ok, "command lines present and bounded");
	CHECK(cpu_ok, "cpu values finite and sane");
	CHECK(procs_find(&ps, 100) != NULL && procs_find(&ps, 999999) == NULL, "procs_find");
	struct proc *w = procs_find(&ps, 900);
	CHECK(w && strlen(w->cmd) < 1024, "4 KB command line bounded");
	CHECK(procs_find(&ps, 903) == NULL, "half written stat is skipped");
	struct proc *ap = procs_find(&ps, 100);
	CHECK(ap && !ap->io_ok && ap->disk_bps == 0, "missing /proc/PID/io gives no disk numbers");
	CHECK(procs_find(&ps, 101) && procs_find(&ps, 101)->io_ok, "io readable");

	/* PSS from smaps_rollup where it reads; RSS where it does not */
	struct proc *q100 = procs_find(&ps, 100), *q102 = procs_find(&ps, 102);
	CHECK(q100 && q100->pss_ok && q100->mem_kb == 1500 && q100->anon_kb == 900,
	      "PSS 1500 kB, anon 900 kB from smaps_rollup (%lu, %lu)", q100 ? (unsigned long)q100->mem_kb : 0UL,
	      q100 ? (unsigned long)q100->anon_kb : 0UL);
	CHECK(q102 && !q102->pss_ok && q102->mem_kb == q102->rss_kb && q102->rss_kb == 2000 && q102->anon_kb == q102->priv_kb,
	      "no Pss line: RSS 2000 kB stands in");

	/* the flatpak app: launcher, bwrap, program and content process form one group */
	int ff = -1;
	for (int i = 0; i < apps_count(); i++)
		if (apps_get(i) && !strcmp(apps_get(i)->exec, "flatpak:org.mozilla.firefox"))
			ff = i;
	struct proc *f0 = procs_find(&ps, 2000), *f1 = procs_find(&ps, 2001), *f2 = procs_find(&ps, 2002),
	            *f3 = procs_find(&ps, 2003);
	CHECK(ff >= 0 && !strcmp(apps_get(ff)->name, "Firefox"), "flatpak desktop entry is an app (index %d)", ff);
	CHECK(f0 && f1 && f2 && f3 && f0->app == ff && f1->app == ff && f2->app == ff && f3->app == ff,
	      "flatpak launcher, bwrap, program and content process grouped under it");
	CHECK(procs_app_mem(&ps, ff) == 11000, "group total %lu kB: PSS 5000 plus three RSS of 2000",
	      (unsigned long)procs_app_mem(&ps, ff));

	/* a process vanishes, another pid is reused with a new start time */
	char p[256];
	snprintf(p, sizeof p, PROC_DIR "/150/stat");
	unlink(p);
	proc_entry(101, "reborn", 777777, 1, "/bin/reborn", 1);
	usleep(100000);
	procs_scan(&ps);
	CHECK(procs_find(&ps, 150) == NULL, "vanished entry dropped");
	struct proc *rb = procs_find(&ps, 101);
	CHECK(rb && !strcmp(rb->comm, "reborn") && rb->cpu < 100 * ps.ncpu + 1.0, "reused pid renamed, no cpu spike (%.2f)",
	      rb ? rb->cpu : -1.0);
	CHECK(ps.n >= 305 && ps.n <= 307, "second scan: %d processes", ps.n);

	/* a second, quiet scan of the same tree */
	procs_scan(&ps);
	CHECK(ps.n >= 298, "third scan");

	/* a read that hangs: a FIFO with no writer stands in for smaps_rollup of a process stuck in a
	 * page fault. The scan must not wait for it; after the timeout the read is given up and the
	 * reader goes on with the other processes. */
	rdr_set_sync(0);
	rdr_set_timeout(0.5);
	proc_entry_pp(2100, 1, "hung", 7000, 1, "/bin/hung", 0);
	char hf[256];
	snprintf(hf, sizeof hf, PROC_DIR "/2100/smaps_rollup");
	CHECK(mkfifo(hf, 0644) == 0, "fifo in place of a hung smaps_rollup");
	double t0 = now_s();
	procs_scan(&ps);
	double scan_s = now_s() - t0;
	CHECK(scan_s < 0.4, "scan returns while the read hangs (%.3f s)", scan_s);
	struct proc *hung = procs_find(&ps, 2100), *p100 = procs_find(&ps, 100);
	if (p100)
		p100->pss_t = 0; /* due now: it must get past the hung read */
	int done = 0;
	t0 = now_s();
	while (!done && now_s() - t0 < 4.0) {
		usleep(50000);
		procs_scan(&ps);
		hung = procs_find(&ps, 2100);
		p100 = procs_find(&ps, 100);
		done = hung && !hung->pss_busy && hung->pss_t > 0 && p100 && !p100->pss_busy && p100->pss_t > 0;
	}
	CHECK(done, "hung read given up after the timeout, next read done (%.2f s)", now_s() - t0);
	CHECK(hung && !hung->pss_ok && hung->mem_kb == hung->rss_kb, "hung process shows RSS, not PSS");
	CHECK(p100 && p100->pss_ok && p100->mem_kb == 1500, "the reader went on past the hung one (PSS %lu kB)",
	      p100 ? (unsigned long)p100->mem_kb : 0UL);
	/* release the stuck thread: its read ends, and its result is thrown away */
	int wfd = open(hf, O_WRONLY | O_NONBLOCK);
	if (wfd >= 0)
		close(wfd);
	unlink(hf);

	printf("%s, %d failed\n", fails ? "FAILED" : "all ok", fails);
	return fails ? 1 : 0;
}
