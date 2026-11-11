/* proc.c -- the process table from /proc, and the installed apps (.desktop files)
 * SPDX-License-Identifier: MIT
 */
#include "gm.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- users ----------------------------------------------------------------------------- */

const char *user_name(unsigned uid)
{
	static struct { unsigned uid; char name[32]; } cache[32];
	static int n;
	for (int i = 0; i < n; i++)
		if (cache[i].uid == uid)
			return cache[i].name;
	char nm[32];
	struct passwd *pw = getpwuid(uid);
	if (pw)
		snprintf(nm, sizeof nm, "%s", pw->pw_name);
	else
		snprintf(nm, sizeof nm, "%u", uid);
	if (n < 32) {
		cache[n].uid = uid;
		snprintf(cache[n].name, sizeof cache[n].name, "%s", nm);
		return cache[n++].name;
	}
	static char last[32];
	snprintf(last, sizeof last, "%s", nm);
	return last;
}

const char *state_name(char s)
{
	switch (s) {
	case 'R': return "Running";
	case 'S': return "Sleeping";
	case 'D': return "Waiting for disk";
	case 'Z': return "Zombie";
	case 'T': return "Stopped";
	case 't': return "Traced";
	case 'I': return "Idle";
	case 'X': return "Dead";
	default: return "Unknown";
	}
}

/* ---- installed apps -------------------------------------------------------------------- */

static struct appinfo *apps;
static int napps, appcap;

static const char *base_of(const char *p)
{
	const char *b = strrchr(p, '/');
	return b ? b + 1 : p;
}

static int generic_exec(const char *e)
{
	static const char *g[] = {"env", "sh", "bash", "dash", "zsh", "python", "python3", "perl", "ruby", "node",
	                          "flatpak", "xdg-open", "gio", "java", "wine", "sudo", NULL};
	for (int i = 0; g[i]; i++)
		if (!strcmp(e, g[i]))
			return 1;
	return 0;
}

/* The program a .desktop Exec= line starts: skips env and VAR=value, quotes and the path. */
static int exec_program(const char *line, char *out, size_t cap)
{
	const char *p = line;
	for (;;) {
		while (*p == ' ' || *p == '\t')
			p++;
		char tok[256];
		size_t n = 0;
		int q = 0;
		while (*p && (q || (*p != ' ' && *p != '\t')) && n + 1 < sizeof tok) {
			if (*p == '"') {
				q = !q;
				p++;
				continue;
			}
			if (*p == '\\' && p[1])
				p++;
			tok[n++] = *p++;
		}
		tok[n] = 0;
		if (!n)
			return -1;
		if (!strcmp(tok, "env") || (strchr(tok, '=') && tok[0] != '/' && tok[0] != '%'))
			continue;
		snprintf(out, cap, "%s", base_of(tok));
		return out[0] ? 0 : -1;
	}
}

static void app_add(const char *exec, const char *name, const char *icon)
{
	for (int i = 0; i < napps; i++)
		if (!strcmp(apps[i].exec, exec))
			return;
	if (napps == appcap) {
		appcap = appcap ? appcap * 2 : 64;
		apps = realloc(apps, (size_t)appcap * sizeof *apps);
		if (!apps) {
			napps = appcap = 0;
			return;
		}
	}
	struct appinfo *a = &apps[napps++];
	snprintf(a->exec, sizeof a->exec, "%s", exec);
	snprintf(a->name, sizeof a->name, "%s", name);
	snprintf(a->icon, sizeof a->icon, "%s", icon);
}

static void apps_scan_dir(const char *dir)
{
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		size_t l = strlen(e->d_name);
		if (l < 9 || strcmp(e->d_name + l - 8, ".desktop"))
			continue;
		char path[600];
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		FILE *f = fopen(path, "re");
		if (!f)
			continue;
		char line[512], name[64] = "", exec[64] = "", icon[32] = "";
		int in = 0, hide = 0, app = 1;
		while (fgets(line, sizeof line, f)) {
			line[strcspn(line, "\n")] = 0;
			if (line[0] == '[') {
				in = !strcmp(line, "[Desktop Entry]");
				continue;
			}
			if (!in)
				continue;
			if (!strncmp(line, "Name=", 5) && !name[0])
				snprintf(name, sizeof name, "%s", line + 5);
			else if (!strncmp(line, "Exec=", 5) && !exec[0])
				exec_program(line + 5, exec, sizeof exec);
			else if (!strncmp(line, "Icon=", 5))
				snprintf(icon, sizeof icon, "%s", line + 5);
			else if (!strcmp(line, "NoDisplay=true") || !strcmp(line, "Hidden=true"))
				hide = 1;
			else if (!strncmp(line, "Type=", 5) && strcmp(line + 5, "Application"))
				app = 0;
		}
		fclose(f);
		if (app && !hide && name[0] && exec[0] && !generic_exec(exec))
			app_add(exec, name, icon);
	}
	closedir(d);
}

void apps_load(void)
{
	napps = 0;
	const char *home = getenv("HOME");
	const char *xh = getenv("XDG_DATA_HOME");
	char p[600];
	if (xh && *xh)
		snprintf(p, sizeof p, "%s/applications", xh);
	else if (home && *home)
		snprintf(p, sizeof p, "%s/.local/share/applications", home);
	else
		p[0] = 0;
	if (p[0])
		apps_scan_dir(p);
	const char *xd = getenv("XDG_DATA_DIRS");
	if (!xd || !*xd)
		xd = "/usr/local/share:/usr/share";
	char dirs[1024];
	snprintf(dirs, sizeof dirs, "%s", xd);
	for (char *s = dirs, *e; s && *s; s = e ? e + 1 : NULL) {
		e = strchr(s, ':');
		if (e)
			*e = 0;
		snprintf(p, sizeof p, "%s/applications", s);
		apps_scan_dir(p);
		if (!e)
			break;
	}
}

int apps_count(void) { return napps; }
const struct appinfo *apps_get(int i) { return i >= 0 && i < napps ? &apps[i] : NULL; }

static int app_match(const char *name, const char *comm)
{
	for (int i = 0; i < napps; i++) {
		const char *x = apps[i].exec;
		if (!strcmp(x, name) || !strcmp(x, comm))
			return i;
		if (strlen(comm) == 15 && !strncmp(x, comm, 15))
			return i;
	}
	return -1;
}

/* ---- the scan -------------------------------------------------------------------------- */

static int slurp(const char *path, char *buf, size_t cap)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ssize_t n = read(fd, buf, cap - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = 0;
	return (int)n;
}

static int cmp_int(const void *a, const void *b)
{
	return *(const int *)a - *(const int *)b;
}

static int is_interp(const char *c)
{
	static const char *k[] = {"python", "perl", "ruby", "node", "sh", "bash", "dash", "zsh", "lua", NULL};
	for (int i = 0; k[i]; i++)
		if (!strncmp(c, k[i], strlen(k[i])))
			return 1;
	return 0;
}

/* The name shown for a process: the program, or the script an interpreter runs. */
static void derive_name(struct proc *p)
{
	snprintf(p->name, sizeof p->name, "%s", p->comm);
	if (!p->cmd || p->kernel || !p->cmd[0] || p->cmd[0] == '[')
		return;
	char a0[256];
	size_t n = strcspn(p->cmd, " ");
	snprintf(a0, sizeof a0, "%.*s", (int)(n < sizeof a0 ? n : sizeof a0 - 1), p->cmd);
	const char *b = base_of(a0);
	size_t cl = strlen(p->comm);
	if (cl && !strncmp(b, p->comm, cl) && !strchr(b, ':') && strlen(b) < sizeof p->name)
		snprintf(p->name, sizeof p->name, "%s", b);
	if (is_interp(p->comm) && p->cmd[n]) {
		const char *s = p->cmd + n;
		while (*s == ' ')
			s++;
		if (*s && *s != '-') {
			char tok[256];
			snprintf(tok, sizeof tok, "%.*s", (int)strcspn(s, " "), s);
			const char *sb = base_of(tok);
			if (sb[0] && strlen(sb) < sizeof p->name)
				snprintf(p->name, sizeof p->name, "%s", sb);
		}
	}
}

void procs_init(struct procs *ps, int ncpu)
{
	memset(ps, 0, sizeof *ps);
	ps->ncpu = ncpu > 0 ? ncpu : 1;
	ps->hz = sysconf(_SC_CLK_TCK);
	if (ps->hz <= 0)
		ps->hz = 100;
}

struct proc *procs_find(struct procs *ps, int pid)
{
	int lo = 0, hi = ps->n - 1;
	while (lo <= hi) {
		int m = (lo + hi) / 2;
		if (ps->p[m].pid == pid)
			return &ps->p[m];
		if (ps->p[m].pid < pid)
			lo = m + 1;
		else
			hi = m - 1;
	}
	return NULL;
}

static int read_one(struct procs *ps, struct proc *p, int fresh, double dt)
{
	char path[64], buf[2048];
	snprintf(path, sizeof path, "/proc/%d/stat", p->pid);
	int n = slurp(path, buf, sizeof buf);
	if (n <= 0)
		return -1;
	char *rp = strrchr(buf, ')');
	char *lp = strchr(buf, '(');
	if (!rp || !lp)
		return -1;
	if (fresh) {
		size_t cl = (size_t)(rp - lp - 1);
		if (cl >= sizeof p->comm)
			cl = sizeof p->comm - 1;
		memcpy(p->comm, lp + 1, cl);
		p->comm[cl] = 0;
	}
	char state;
	int ppid;
	unsigned long long ut = 0, st = 0;
	long threads = 1;
	/* after ") ": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime
	 * cutime cstime priority nice num_threads */
	if (sscanf(rp + 2, "%c %d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu %*d %*d %*d %*d %ld", &state, &ppid,
	           &ut, &st, &threads) < 5)
		return -1;
	p->state = state;
	p->ppid = ppid;
	p->threads = (int)threads;
	p->p_ticks = p->ticks;
	p->ticks = ut + st;
	if (fresh)
		p->p_ticks = p->ticks;
	p->cpu = 0;
	if (!fresh && dt > 0.05) {
		double d = (double)(p->ticks - p->p_ticks) / (double)ps->hz / dt * 100.0 / ps->ncpu;
		p->cpu = d < 0 ? 0 : d;
	}
	snprintf(path, sizeof path, "/proc/%d/statm", p->pid);
	if (slurp(path, buf, sizeof buf) > 0) {
		unsigned long long size, res, shr;
		if (sscanf(buf, "%llu %llu %llu", &size, &res, &shr) == 3) {
			long pg = sysconf(_SC_PAGESIZE) / 1024;
			p->priv_kb = (res > shr ? res - shr : 0) * (u64)pg;
		}
	}
	if (p->io_ok || fresh) {
		snprintf(path, sizeof path, "/proc/%d/io", p->pid);
		int m = (p->uid == getuid() || geteuid() == 0) ? slurp(path, buf, sizeof buf) : -1;
		p->p_io = p->io;
		if (m > 0) {
			unsigned long long rd = 0, wr = 0;
			char *a = strstr(buf, "read_bytes:"), *b = strstr(buf, "write_bytes:");
			if (a)
				rd = strtoull(a + 11, NULL, 10);
			if (b)
				wr = strtoull(b + 12, NULL, 10);
			p->io = rd + wr;
			if (fresh)
				p->p_io = p->io;
			p->io_ok = 1;
			p->disk_bps = !fresh && dt > 0.05 && p->io >= p->p_io ? (double)(p->io - p->p_io) / dt : 0;
		} else {
			p->io_ok = 0;
			p->disk_bps = 0;
		}
	}
	return 0;
}

void procs_scan(struct procs *ps)
{
	double t = now_s();
	double dt = ps->t_prev > 0 && t - ps->t_prev < 6 ? t - ps->t_prev : 0;
	ps->t_prev = t;

	int *pids = NULL, np = 0, pcap = 0;
	DIR *d = opendir("/proc");
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] < '1' || e->d_name[0] > '9')
			continue;
		if (np == pcap) {
			pcap = pcap ? pcap * 2 : 512;
			pids = realloc(pids, (size_t)pcap * sizeof *pids);
			if (!pids) {
				closedir(d);
				return;
			}
		}
		pids[np++] = atoi(e->d_name);
	}
	closedir(d);
	qsort(pids, (size_t)np, sizeof *pids, cmp_int);

	struct proc *nw = calloc((size_t)np + 1, sizeof *nw);
	if (!nw) {
		free(pids);
		return;
	}
	int nn = 0, oi = 0;
	for (int i = 0; i < np; i++) {
		while (oi < ps->n && ps->p[oi].pid < pids[i])
			oi++;
		struct proc *p = &nw[nn];
		int fresh = !(oi < ps->n && ps->p[oi].pid == pids[i]);
		if (!fresh) {
			*p = ps->p[oi];
			ps->p[oi].cmd = NULL; /* moved */
		} else {
			memset(p, 0, sizeof *p);
			p->pid = pids[i];
			p->own_app = p->app = -1;
			char path[64];
			struct stat sb;
			snprintf(path, sizeof path, "/proc/%d", p->pid);
			if (stat(path, &sb) == 0)
				p->uid = sb.st_uid;
		}
		if (read_one(ps, p, fresh, dt) < 0) {
			free(p->cmd);
			continue;
		}
		if (fresh) {
			char path[64], cb[1024];
			snprintf(path, sizeof path, "/proc/%d/cmdline", p->pid);
			int n = slurp(path, cb, sizeof cb);
			if (n > 0) {
				for (int k = 0; k < n; k++)
					if (!cb[k])
						cb[k] = ' ';
				while (n > 0 && cb[n - 1] == ' ')
					cb[--n] = 0;
			}
			p->kernel = n <= 0 && (p->ppid == 2 || p->pid == 2);
			if (n > 0)
				p->cmd = strdup(cb);
			else {
				char br[40];
				snprintf(br, sizeof br, p->kernel ? "[%s]" : "(%s)", p->comm);
				p->cmd = strdup(br);
			}
			derive_name(p);
			p->own_app = app_match(p->name, p->comm);
		}
		nn++;
	}
	for (int i = 0; i < ps->n; i++)
		free(ps->p[i].cmd);
	free(ps->p);
	free(pids);
	ps->p = nw;
	ps->n = nn;
	ps->cap = np + 1;

	/* which app each process belongs to: the nearest ancestor (or itself) that is one */
	for (int i = 0; i < ps->n; i++) {
		struct proc *p = &ps->p[i];
		p->root_pid = 0;
		p->app = -1;
		struct proc *q = p;
		for (int depth = 0; q && depth < 40; depth++) {
			if (q->own_app >= 0) {
				p->root_pid = q->pid;
				p->app = q->own_app;
				break;
			}
			if (q->ppid <= 1)
				break;
			q = procs_find(ps, q->ppid);
		}
	}
}
