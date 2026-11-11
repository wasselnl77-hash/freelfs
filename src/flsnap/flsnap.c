/* flsnap -- btrfs snapshots of the root file system (the @snapshots layout)
 * SPDX-License-Identifier: MIT
 *
 *   flsnap create [--reason TEXT] [--packages FILE] [--auto]
 *   flsnap list [--machine]          flsnap show N      flsnap status
 *   flsnap delete N...               flsnap prune [--keep K]
 *   flsnap rollback [N]
 *
 * Layout (build/50-image.sh): the top level of the btrfs file system holds @ (the
 * default subvolume, mounted at /), @home and @snapshots (mounted at /.snapshots).
 * A snapshot is a read-only snapshot of @ named /.snapshots/N, with its metadata in
 * /.snapshots/N.info (date, reason, installed packages). create keeps the last
 * `keep` snapshots (flsnap.conf, default 8). After every change /boot/flboot/snapshots is
 * rewritten, which is what flboot's "Start an earlier state" lists.
 *
 * rollback N: mounts the top level, moves @ to @snapshots/M (so that the state being left
 * is itself a snapshot), makes a writable copy of snapshot N as the new @ and makes it the
 * default subvolume. The next boot starts it. Without N, the snapshot the system was booted
 * from is used ("Make this the current state").
 *
 * Installed setuid root for the group seat, like fltime-set (so Settings can use it), hence
 * the care with arguments; when running with more rights than the caller, the environment
 * is discarded and the compiled-in paths are used (tests build with other ones via FLSNAP_MOCK,
 * honoured only when not setuid).
 *
 * Test backend: FLSNAP_MOCK=DIR replaces btrfs by plain directories: DIR/top/@, DIR/top/@snapshots,
 * DIR/esp, DIR/booted (the subvolume "booted" from), DIR/default. FLSNAP_FLPKG names the flpkg stand-in.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_KEEP 8
#define MAXSNAP 999999999L

static struct {
	char conf[PATH_MAX], dir[PATH_MAX], root[PATH_MAX], esp[PATH_MAX], top[PATH_MAX];
	char btrfs[PATH_MAX], mount[PATH_MAX], umount[PATH_MAX], flpkg[PATH_MAX], cp[PATH_MAX], rm[PATH_MAX];
	char device[128], mock[PATH_MAX];
	int keep;
	bool elevated;
} C;

static void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("flsnap: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	exit(1);
}

static void warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void warn(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("flsnap: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static void setpath(char *dst, const char *v)
{
	if (strlen(v) >= PATH_MAX)
		die("path too long");
	strcpy(dst, v);
}

static void pathf(char *dst, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void pathf(char *dst, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(dst, PATH_MAX, fmt, ap);
	va_end(ap);
	if (n < 0 || n >= PATH_MAX)
		die("path too long");
}

/* ---- running programs ------------------------------------------------------ */

/* Run ARGV (absolute program path); stdout goes to OUT (size cap) when given, else is
 * passed on. Returns the exit status, -1 if it could not run. */
static int run(char *const argv[], char *out, size_t cap)
{
	int fd[2] = {-1, -1};
	if (out && pipe(fd) < 0)
		return -1;
	fflush(NULL);
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (!pid) {
		if (out) {
			dup2(fd[1], 1);
			close(fd[0]);
			close(fd[1]);
		}
		execv(argv[0], argv);
		_exit(127);
	}
	if (out) {
		close(fd[1]);
		size_t n = 0;
		ssize_t r;
		while ((r = read(fd[0], out + n, cap - 1 - n)) > 0 && (n += (size_t)r) < cap - 1)
			;
		out[n] = '\0';
		close(fd[0]);
	}
	int st;
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
		;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* ---- text helpers ----------------------------------------------------------- */

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	size_t cap = 4096, n = 0;
	char *b = malloc(cap);
	size_t r;
	while (b && (r = fread(b + n, 1, cap - 1 - n, f)) > 0) {
		n += r;
		if (n == cap - 1) {
			cap *= 2;
			b = realloc(b, cap);
		}
	}
	fclose(f);
	if (!b)
		die("out of memory");
	b[n] = '\0';
	return b;
}

/* write TEXT to PATH atomically (a .new file, renamed) */
static int put(const char *path, const char *text)
{
	char tmp[PATH_MAX];
	pathf(tmp, "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return -1;
	int ok = fputs(text, f) >= 0;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

/* single line, printable, at most MAX bytes: what goes into the .info file */
static void clean(char *dst, size_t max, const char *src)
{
	size_t n = 0;
	for (; *src && n + 1 < max; src++)
		dst[n++] = (unsigned char)*src < 32 || *src == 127 ? ' ' : *src;
	while (n && dst[n - 1] == ' ')
		n--;
	dst[n] = '\0';
}

static void conf_load(void)
{
	C.keep = DEFAULT_KEEP;
	setpath(C.device, "LABEL=freelfs");
	char *t = slurp(C.conf);
	if (!t)
		return;
	char *save = NULL;
	for (char *l = strtok_r(t, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
		while (isspace((unsigned char)*l))
			l++;
		if (*l == '#' || !*l)
			continue;
		char *eq = strchr(l, '=');
		if (!eq)
			continue;
		*eq++ = '\0';
		char *k = l, *v = eq;
		for (char *e = k + strlen(k); e > k && isspace((unsigned char)e[-1]); *--e = '\0')
			;
		while (isspace((unsigned char)*v))
			v++;
		char *hash = strchr(v, '#');
		if (hash)
			*hash = '\0';
		for (char *e = v + strlen(v); e > v && isspace((unsigned char)e[-1]); *--e = '\0')
			;
		if (!strcmp(k, "keep")) {
			char *end;
			long x = strtol(v, &end, 10);
			if (*v && !*end && x >= 1 && x <= 99)
				C.keep = (int)x;
			else
				warn("%s: keep must be 1..99, using %d", C.conf, C.keep);
		} else if (!strcmp(k, "device") && *v && strlen(v) < sizeof C.device && !strchr(v, ' ')) {
			strcpy(C.device, v);
		}
	}
	free(t);
}

static void setup(void)
{
	C.elevated = getuid() != geteuid();
	setpath(C.conf, "/etc/flsnap.conf");
	setpath(C.dir, "/.snapshots");
	setpath(C.root, "/");
	setpath(C.esp, "/boot");
	setpath(C.btrfs, "/usr/bin/btrfs");
	setpath(C.mount, "/usr/bin/mount");
	setpath(C.umount, "/usr/bin/umount");
	setpath(C.flpkg, "/usr/bin/flpkg");
	setpath(C.cp, "/usr/bin/cp");
	setpath(C.rm, "/usr/bin/rm");
	if (C.elevated) {
		/* the caller is not trusted: nothing from the environment, and root for the children */
		clearenv();
		setenv("PATH", "/usr/bin:/usr/sbin:/bin:/sbin", 1);
		if (setuid(0) < 0)
			die("cannot become root");
	} else {
		const char *e;
		if ((e = getenv("FLSNAP_CONF")))
			setpath(C.conf, e);
		if ((e = getenv("FLSNAP_FLPKG")))
			setpath(C.flpkg, e);
		if ((e = getenv("FLSNAP_MOCK")) && *e) {
			setpath(C.mock, e);
			pathf(C.top, "%s/top", e);
			pathf(C.root, "%s/top/@", e);
			pathf(C.dir, "%s/top/@snapshots", e);
			pathf(C.esp, "%s/esp", e);
			setpath(C.cp, "/usr/bin/cp");
			if (access(C.cp, X_OK))
				setpath(C.cp, "/bin/cp");
			if (access(C.rm, X_OK))
				setpath(C.rm, "/bin/rm");
		}
	}
	conf_load();
}

/* ---- the btrfs layer (or the mock) ---------------------------------------------- */

/* mountinfo: the root (path inside the file system) and source of the mount at DIR, and its type */
static bool mountinfo(const char *dir, char *root, char *source, char *type, size_t cap)
{
	char *t = slurp("/proc/self/mountinfo");
	bool found = false;
	if (!t)
		return false;
	char *save = NULL;
	for (char *l = strtok_r(t, "\n", &save); l && !found; l = strtok_r(NULL, "\n", &save)) {
		char mp[512], rt[512], ty[64], src[512];
		int id, parent, n = 0;
		unsigned maj, min;
		if (sscanf(l, "%d %d %u:%u %511s %511s %*s%n", &id, &parent, &maj, &min, rt, mp, &n) < 6)
			continue;
		char *dash = strstr(l + n, " - ");
		if (!dash || sscanf(dash + 3, "%63s %511s", ty, src) != 2 || strcmp(mp, dir))
			continue;
		/* the last mount at a path wins: keep looking */
		snprintf(root, cap, "%s", rt);
		snprintf(source, cap, "%s", src);
		snprintf(type, 64, "%s", ty);
		found = true;
		(void)id; (void)parent; (void)maj; (void)min;
	}
	free(t);
	return found;
}

/* Does this system have the layout? REASON gets a sentence when not. */
static bool supported(char *why, size_t cap)
{
	if (*C.mock)
		return true;
	char rt[512], src[512], ty[64];
	if (!mountinfo("/", rt, src, ty, sizeof rt) || strcmp(ty, "btrfs")) {
		snprintf(why, cap, "The system is not on a btrfs file system.");
		return false;
	}
	if (!mountinfo(C.dir, rt, src, ty, sizeof rt) || strcmp(ty, "btrfs") || strcmp(rt, "/@snapshots")) {
		snprintf(why, cap, "%.200s is not the @snapshots subvolume.", C.dir);
		return false;
	}
	if (access(C.btrfs, X_OK) < 0) {
		snprintf(why, cap, "btrfs-progs is not installed.");
		return false;
	}
	return true;
}

/* Number of the snapshot the system runs from, 0 if it runs from @ (or a mock "@"), -1 if
 * it runs from some other subvolume. */
static long booted_snapshot(void)
{
	char root[512] = "", src[512], ty[64];
	if (*C.mock) {
		char p[PATH_MAX];
		pathf(p, "%s/booted", C.mock);
		char *t = slurp(p);
		if (t) {
			t[strcspn(t, "\n")] = '\0';
			snprintf(root, sizeof root, "/%s", t);
			free(t);
		} else {
			return 0;
		}
	} else if (!mountinfo("/", root, src, ty, sizeof root)) {
		return -1;
	}
	if (!strcmp(root, "/@"))
		return 0;
	if (!strncmp(root, "/@snapshots/", 12) && root[12] >= '1' && root[12] <= '9') {
		char *end;
		long n = strtol(root + 12, &end, 10);
		if (!*end)
			return n;
	}
	return -1;
}

static int bt_snapshot(const char *src, const char *dst, bool ro)
{
	if (*C.mock) {
		char *a[] = {C.cp, "-a", (char *)src, (char *)dst, NULL};
		if (run(a, NULL, 0))
			return -1;
		char m[PATH_MAX];
		pathf(m, "%s/.mock-ro", dst);
		if (ro)
			return put(m, "ro\n");
		unlink(m);
		return 0;
	}
	char *ra[] = {C.btrfs, "-q", "subvolume", "snapshot", "-r", (char *)src, (char *)dst, NULL};
	char *wa[] = {C.btrfs, "-q", "subvolume", "snapshot", (char *)src, (char *)dst, NULL};
	return run(ro ? ra : wa, NULL, 0) ? -1 : 0;
}

static int bt_delete(const char *path)
{
	if (*C.mock) {
		char *a[] = {C.rm, "-rf", (char *)path, NULL};
		return run(a, NULL, 0) ? -1 : 0;
	}
	char *a[] = {C.btrfs, "-q", "subvolume", "delete", (char *)path, NULL};
	return run(a, NULL, 0) ? -1 : 0;
}

/* the top level of the file system: where @ and @snapshots are siblings */
static char toptmp[PATH_MAX];

static const char *top_open(void)
{
	if (*C.mock)
		return C.top;
	char rt[512], src[512], ty[64], dev[512];
	snprintf(dev, sizeof dev, "%s", C.device);
	/* the device /.snapshots was mounted from, unless the kernel called it /dev/root */
	if (mountinfo(C.dir, rt, src, ty, sizeof rt) && !strncmp(src, "/dev/", 5) && strcmp(src, "/dev/root"))
		snprintf(dev, sizeof dev, "%s", src);
	const char *run_dir = access("/run", W_OK) == 0 ? "/run" : "/tmp";
	pathf(toptmp, "%s/flsnap.XXXXXX", run_dir);
	if (!mkdtemp(toptmp))
		die("cannot make a mount point: %s", strerror(errno));
	char *a[] = {C.mount, "-t", "btrfs", "-o", "subvolid=5,noatime", dev, toptmp, NULL};
	if (run(a, NULL, 0)) {
		rmdir(toptmp);
		toptmp[0] = '\0';
		die("cannot mount the top level of the file system (%s)", dev);
	}
	return toptmp;
}

static void top_close(void)
{
	if (!toptmp[0])
		return;
	char *a[] = {C.umount, toptmp, NULL};
	if (run(a, NULL, 0))
		warn("could not unmount %s", toptmp);
	else
		rmdir(toptmp);
	toptmp[0] = '\0';
}

static int bt_set_default(const char *top, const char *sub)
{
	char path[PATH_MAX];
	pathf(path, "%s/%s", top, sub);
	if (*C.mock) {
		char f[PATH_MAX];
		pathf(f, "%s/default", C.mock);
		char t[300];
		snprintf(t, sizeof t, "%s\n", sub);
		return put(f, t);
	}
	char out[4096];
	char *show[] = {C.btrfs, "subvolume", "show", path, NULL};
	if (run(show, out, sizeof out))
		return -1;
	char *p = strstr(out, "Subvolume ID:");
	long id = p ? strtol(p + 13, NULL, 10) : 0;
	if (id < 256)
		return -1;
	char ids[24];
	snprintf(ids, sizeof ids, "%ld", id);
	char *a[] = {C.btrfs, "-q", "subvolume", "set-default", ids, (char *)top, NULL};
	return run(a, NULL, 0) ? -1 : 0;
}

/* ---- snapshots on disk ---------------------------------------------------------- */

struct snap {
	long n;
	time_t epoch;
	char date[32], reason[160];
	char *packages; /* "name version\n" lines, or NULL */
	int npkg;
};

static int cmp_n(const void *a, const void *b)
{
	long x = ((const struct snap *)a)->n, y = ((const struct snap *)b)->n;
	return x < y ? -1 : x > y;
}

static void snap_info(const char *dir, struct snap *s)
{
	char p[PATH_MAX];
	pathf(p, "%s/%ld.info", dir, s->n);
	char *t = slurp(p);
	snprintf(s->date, sizeof s->date, "unknown date");
	s->reason[0] = '\0';
	s->packages = NULL;
	s->npkg = 0;
	s->epoch = 0;
	if (!t)
		return;
	bool inpk = false;
	char *pk = malloc(strlen(t) + 1);
	size_t pn = 0;
	char *save = NULL;
	for (char *l = strtok_r(t, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
		if (!strcmp(l, "[packages]")) {
			inpk = true;
		} else if (inpk) {
			size_t n = strlen(l);
			memcpy(pk + pn, l, n);
			pn += n;
			pk[pn++] = '\n';
			s->npkg++;
		} else if (!strncmp(l, "epoch=", 6)) {
			s->epoch = (time_t)atoll(l + 6);
		} else if (!strncmp(l, "date=", 5)) {
			clean(s->date, sizeof s->date, l + 5);
		} else if (!strncmp(l, "reason=", 7)) {
			clean(s->reason, sizeof s->reason, l + 7);
		}
	}
	pk[pn] = '\0';
	s->packages = pk;
	free(t);
}

static void snap_free(struct snap *v, size_t n)
{
	for (size_t i = 0; i < n; i++)
		free(v[i].packages);
	free(v);
}

/* all snapshots in DIR, oldest first */
static struct snap *snap_list(const char *dir, size_t *count)
{
	DIR *d = opendir(dir);
	struct snap *v = NULL;
	size_t n = 0, cap = 0;
	struct dirent *de;
	while (d && (de = readdir(d))) {
		char *end;
		if (de->d_name[0] < '1' || de->d_name[0] > '9')
			continue;
		long x = strtol(de->d_name, &end, 10);
		if (*end || x > MAXSNAP)
			continue;
		char p[PATH_MAX];
		struct stat st;
		pathf(p, "%s/%s", dir, de->d_name);
		if (lstat(p, &st) < 0 || !S_ISDIR(st.st_mode))
			continue;
		if (n == cap)
			v = realloc(v, (cap = cap ? cap * 2 : 16) * sizeof *v);
		if (!v)
			die("out of memory");
		v[n].n = x;
		snap_info(dir, &v[n]);
		n++;
	}
	if (d)
		closedir(d);
	if (n > 1)
		qsort(v, n, sizeof *v, cmp_n);
	*count = n;
	return v;
}

static long next_number(const char *dir)
{
	size_t n;
	struct snap *v = snap_list(dir, &n);
	long last = n ? v[n - 1].n : 0;
	snap_free(v, n);
	char p[PATH_MAX];
	pathf(p, "%s/.last", dir);
	char *t = slurp(p);
	if (t) {
		long x = atol(t);
		if (x > last)
			last = x;
		free(t);
	}
	if (last >= MAXSNAP)
		die("too many snapshots");
	return last + 1;
}

static int write_info(const char *dir, long n, const char *reason, const char *packages)
{
	char p[PATH_MAX], date[32], r[160];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(date, sizeof date, "%Y-%m-%d %H:%M", &tm);
	clean(r, sizeof r, reason);
	pathf(p, "%s/%ld.info", dir, n);
	size_t cap = 256 + strlen(packages) + strlen(r);
	char *b = malloc(cap);
	if (!b)
		die("out of memory");
	snprintf(b, cap, "epoch=%lld\ndate=%s\nreason=%s\n[packages]\n%s", (long long)now, date, r, packages);
	int rc = put(p, b);
	free(b);
	if (rc < 0)
		return -1;
	char last[PATH_MAX], num[24];
	pathf(last, "%s/.last", dir);
	snprintf(num, sizeof num, "%ld\n", n);
	put(last, num);
	return 0;
}

/* what flboot lists: "@snapshots/N DATE  REASON", newest first */
static void write_boot_list(void)
{
	char d[PATH_MAX], p[PATH_MAX];
	pathf(d, "%s/flboot", C.esp);
	if (access(d, W_OK) < 0)
		return;
	pathf(p, "%s/flboot/snapshots", C.esp);
	size_t n;
	struct snap *v = snap_list(C.dir, &n);
	size_t cap = 256 + n * 240;
	char *b = malloc(cap), *o = b;
	if (!b)
		die("out of memory");
	o += snprintf(o, cap, "# Written by flsnap: the states flboot can start (newest first)\n");
	for (size_t i = n; i-- > 0;)
		o += snprintf(o, cap - (size_t)(o - b), "@snapshots/%ld %s  %s\n", v[i].n, v[i].date, v[i].reason);
	if (put(p, b) < 0)
		warn("cannot update %s: %s", p, strerror(errno));
	free(b);
	snap_free(v, n);
}

/* "name version" lines of what is installed, from FILE or flpkg */
static char *package_list(const char *file)
{
	if (file) {
		char *t = slurp(file);
		return t ? t : strdup("");
	}
	size_t cap = 1 << 20;
	char *out = malloc(cap);
	if (!out)
		die("out of memory");
	char *a[] = {C.flpkg, "list", NULL};
	if (access(C.flpkg, X_OK) < 0 || run(a, out, cap))
		out[0] = '\0';
	return out;
}

/* ---- commands ----------------------------------------------------------------- */

static void need_supported(void)
{
	char why[256];
	if (!supported(why, sizeof why))
		die("%s", why);
}

static void prune(int keep)
{
	size_t n;
	struct snap *v = snap_list(C.dir, &n);
	long booted = booted_snapshot();
	size_t drop = n > (size_t)keep ? n - (size_t)keep : 0;
	for (size_t i = 0; i < n && drop; i++) {
		if (v[i].n == booted)
			continue;
		char p[PATH_MAX], info[PATH_MAX];
		pathf(p, "%s/%ld", C.dir, v[i].n);
		pathf(info, "%s/%ld.info", C.dir, v[i].n);
		if (bt_delete(p) == 0) {
			unlink(info);
			printf("removed snapshot #%ld (%s)\n", v[i].n, v[i].date);
		} else {
			warn("cannot delete snapshot #%ld", v[i].n);
		}
		drop--;
	}
	snap_free(v, n);
}

static int cmd_create(int argc, char **argv)
{
	const char *reason = "manual", *pkgfile = NULL;
	bool autom = false;
	for (int i = 0; i < argc; i++) {
		if (!strcmp(argv[i], "--reason") && i + 1 < argc)
			reason = argv[++i];
		else if (!strcmp(argv[i], "--packages") && i + 1 < argc)
			pkgfile = argv[++i];
		else if (!strcmp(argv[i], "--auto"))
			autom = true;
		else
			die("usage: flsnap create [--reason TEXT] [--packages FILE] [--auto]");
	}
	char why[256];
	if (!supported(why, sizeof why)) {
		if (autom)
			return 0; /* a hook on a system without the layout: nothing to do */
		die("%s", why);
	}
	if (booted_snapshot() != 0)
		die("this system runs from an earlier state; make it the current one (flsnap rollback) first");
	long n = next_number(C.dir);
	char p[PATH_MAX];
	pathf(p, "%s/%ld", C.dir, n);
	if (bt_snapshot(C.root, p, true) < 0) {
		if (autom) {
			warn("could not take a snapshot (going on without one)");
			return 1;
		}
		die("could not take a snapshot");
	}
	/* a caller with more rights than its own must not make us read files for it */
	char *pk = package_list(C.elevated ? NULL : pkgfile);
	if (write_info(C.dir, n, reason, pk) < 0)
		warn("cannot write the information file of snapshot #%ld", n);
	free(pk);
	prune(C.keep);
	write_boot_list();
	printf("snapshot #%ld taken\n", n);
	return 0;
}

static int cmd_list(int argc, char **argv)
{
	bool machine = argc > 0 && !strcmp(argv[0], "--machine");
	if (argc > 1 || (argc == 1 && !machine))
		die("usage: flsnap list [--machine]");
	size_t n;
	struct snap *v = snap_list(C.dir, &n);
	long booted = booted_snapshot();
	if (!n && !machine)
		puts("no snapshots yet");
	for (size_t i = n; i-- > 0;) {
		if (machine) /* N, epoch, date, reason, packages, booted: tab separated */
			printf("%ld\t%lld\t%s\t%s\t%d\t%d\n", v[i].n, (long long)v[i].epoch, v[i].date, v[i].reason,
			       v[i].npkg, v[i].n == booted);
		else
			printf("#%-4ld %s  %-40s %4d packages%s\n", v[i].n, v[i].date, v[i].reason, v[i].npkg,
			       v[i].n == booted ? "  (running now)" : "");
	}
	snap_free(v, n);
	return 0;
}

static long parse_n(const char *s)
{
	char *end;
	if (*s == '#')
		s++;
	long n = strtol(s, &end, 10);
	if (!*s || *end || n < 1 || n > MAXSNAP || *s == '0' || *s == '+' || *s == '-')
		die("not a snapshot number: %s", s);
	return n;
}

static int cmd_show(int argc, char **argv)
{
	if (argc != 1)
		die("usage: flsnap show N");
	struct snap s = {.n = parse_n(argv[0])};
	char p[PATH_MAX];
	struct stat st;
	pathf(p, "%s/%ld", C.dir, s.n);
	if (lstat(p, &st) < 0)
		die("no snapshot #%ld", s.n);
	snap_info(C.dir, &s);
	printf("snapshot #%ld\ndate:    %s\nreason:  %s\npackages (%d):\n%s", s.n, s.date, s.reason, s.npkg,
	       s.packages ? s.packages : "");
	free(s.packages);
	return 0;
}

static int cmd_status(void)
{
	char why[256] = "";
	bool ok = supported(why, sizeof why);
	size_t n;
	struct snap *v = snap_list(C.dir, &n);
	snap_free(v, n);
	long b = booted_snapshot();
	printf("supported=%d\nwhy=%s\nkeep=%d\ncount=%zu\nbooted=%ld\n", ok, why, C.keep, n, b > 0 ? b : 0);
	return 0;
}

static int cmd_delete(int argc, char **argv)
{
	if (argc < 1)
		die("usage: flsnap delete N...");
	need_supported();
	long booted = booted_snapshot();
	for (int i = 0; i < argc; i++)
		if (parse_n(argv[i]) == booted)
			die("snapshot #%ld is the state running now", booted);
	int rc = 0;
	for (int i = 0; i < argc; i++) {
		long n = parse_n(argv[i]);
		char p[PATH_MAX], info[PATH_MAX];
		struct stat st;
		pathf(p, "%s/%ld", C.dir, n);
		pathf(info, "%s/%ld.info", C.dir, n);
		if (lstat(p, &st) < 0) {
			warn("no snapshot #%ld", n);
			rc = 1;
		} else if (bt_delete(p) < 0) {
			warn("cannot delete snapshot #%ld", n);
			rc = 1;
		} else {
			unlink(info);
			printf("snapshot #%ld deleted\n", n);
		}
	}
	write_boot_list();
	return rc;
}

static int cmd_prune(int argc, char **argv)
{
	int keep = C.keep;
	if (argc == 2 && !strcmp(argv[0], "--keep")) {
		char *end;
		long k = strtol(argv[1], &end, 10);
		if (!*argv[1] || *end || k < 1 || k > 99)
			die("--keep takes 1..99");
		keep = (int)k;
	} else if (argc) {
		die("usage: flsnap prune [--keep K]");
	}
	need_supported();
	prune(keep);
	write_boot_list();
	return 0;
}

static int cmd_rollback(int argc, char **argv)
{
	if (argc > 1)
		die("usage: flsnap rollback [N]");
	need_supported();
	long booted = booted_snapshot();
	long n = argc ? parse_n(argv[0]) : booted;
	if (n <= 0)
		die("name the snapshot to go back to (flsnap list); the system is not running from one");
	char src[PATH_MAX];
	struct stat st;
	pathf(src, "%s/%ld", C.dir, n);
	if (lstat(src, &st) < 0)
		die("no snapshot #%ld", n);

	/* the packages of the state that is left, when it is the running system */
	char *pk = booted == 0 ? package_list(NULL) : strdup("");
	const char *top = top_open();
	char cur[PATH_MAX], from[PATH_MAX], keepdir[PATH_MAX], sdir[PATH_MAX];
	pathf(cur, "%s/@", top);
	pathf(sdir, "%s/@snapshots", top);
	pathf(from, "%s/@snapshots/%ld", top, n);
	long m = next_number(sdir);
	pathf(keepdir, "%s/@snapshots/%ld", top, m);
	char reason[64];
	snprintf(reason, sizeof reason, "before going back to #%ld", n);

	int rc = 1;
	bool moved = false, copied = false;
	if (rename(cur, keepdir) < 0) {
		warn("cannot move @ aside: %s", strerror(errno));
		goto out;
	}
	moved = true;
	if (bt_snapshot(from, cur, false) < 0) {
		warn("cannot make the new @ from snapshot #%ld", n);
		goto out;
	}
	copied = true;
	if (bt_set_default(top, "@") < 0) {
		warn("cannot make the new @ the default subvolume");
		goto out;
	}
	rc = 0;
out:
	if (rc) { /* undo, so that the next boot is what it was */
		if (copied)
			bt_delete(cur);
		if (moved && rename(keepdir, cur) < 0)
			warn("could not put @ back; it is %s", keepdir);
	} else if (write_info(sdir, m, reason, pk) < 0) {
		warn("cannot write the information file of snapshot #%ld", m);
	}
	free(pk);
	top_close();
	if (rc)
		return rc;
	write_boot_list();
	prune(C.keep);
	write_boot_list();
	printf("Snapshot #%ld is the current state now; the state left is snapshot #%ld.\n"
	       "Restart the computer to start it.\n", n, m);
	return 0;
}

static void usage(FILE *f)
{
	fputs("usage: flsnap create [--reason TEXT] [--packages FILE] [--auto]\n"
	      "       flsnap list [--machine] | show N | status\n"
	      "       flsnap delete N... | prune [--keep K]\n"
	      "       flsnap rollback [N]     make snapshot N (default: the one running) the current state\n"
	      "Snapshots of the root file system (btrfs @snapshots); keep= in /etc/flsnap.conf.\n", f);
}

int main(int argc, char **argv)
{
	if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
		usage(argc < 2 ? stderr : stdout);
		return argc < 2 ? 2 : 0;
	}
	setup();
	const char *c = argv[1];
	int n = argc - 2;
	char **a = argv + 2;
	if (!strcmp(c, "create"))
		return cmd_create(n, a);
	if (!strcmp(c, "list") || !strcmp(c, "ls"))
		return cmd_list(n, a);
	if (!strcmp(c, "show"))
		return cmd_show(n, a);
	if (!strcmp(c, "status"))
		return cmd_status();
	if (!strcmp(c, "delete") || !strcmp(c, "rm"))
		return cmd_delete(n, a);
	if (!strcmp(c, "prune"))
		return cmd_prune(n, a);
	if (!strcmp(c, "rollback"))
		return cmd_rollback(n, a);
	usage(stderr);
	return 2;
}
