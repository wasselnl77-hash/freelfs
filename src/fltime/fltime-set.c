/* fltime-set -- the few privileged things Settings does about the time.
 * SPDX-License-Identifier: MIT
 *
 * Installed setuid root and runnable only by the group seat (mode 4750), like
 * flpower, so the Settings app can do this without a password. It accepts
 * exactly four commands and checks every argument:
 *
 *   fltime-set zone NAME      /etc/localtime -> /usr/share/zoneinfo/NAME and
 *                             /etc/timezone, for a name that is a real TZif
 *                             file inside the zoneinfo directory
 *   fltime-set ntp on|off     NTP= in /etc/fltime.conf, then starts or stops the
 *                             ntp service (/etc/rc.d/init.d/ntp)
 *   fltime-set sync           asks the ntp service to synchronise now
 *   fltime-set time EPOCH     sets the clock, only while ntp is off
 *
 * The environment is thrown away; all paths are fixed at build time (the tests
 * build a copy with other paths). Exit status: 0 done, 2 usage, 3 refused
 * argument, 4 refused (ntp is on), 1 failed.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef ZONEINFO
#define ZONEINFO "/usr/share/zoneinfo"
#endif
#ifndef ETCDIR
#define ETCDIR "/etc"
#endif
#ifndef INITD
#define INITD "/etc/rc.d/init.d"
#endif
#ifndef HWCLOCK
#define HWCLOCK "/usr/sbin/hwclock"
#endif

#define MAXNAME 48

static int fail(int code, const char *msg)
{
	fprintf(stderr, "fltime-set: %s\n", msg);
	return code;
}

/* Europe/Berlin, America/Argentina/Buenos_Aires, Etc/GMT+5, UTC: no dots at the
 * start of a part, no empty parts, nothing but these characters */
static int name_ok(const char *s)
{
	size_t n = strlen(s);
	if (n == 0 || n > MAXNAME)
		return 0;
	int part = 0;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '/') {
			if (part == 0)
				return 0;
			part = 0;
			continue;
		}
		if (!(isalnum(c) || c == '_' || c == '+' || c == '-' || (c == '.' && part > 0)))
			return 0;
		part++;
	}
	if (part == 0)
		return 0;
	/* parts that are not zones */
	if (!strncmp(s, "posix/", 6) || !strncmp(s, "right/", 6) || !strcmp(s, "posixrules") ||
	    !strcmp(s, "localtime") || !strcmp(s, "Factory"))
		return 0;
	return 1;
}

/* a regular file inside the zoneinfo directory that starts with "TZif" */
static int zone_exists(const char *name)
{
	char path[PATH_MAX], real[PATH_MAX], root[PATH_MAX];
	snprintf(path, sizeof path, "%s/%s", ZONEINFO, name);
	if (!realpath(ZONEINFO, root) || !realpath(path, real))
		return 0;
	size_t rl = strlen(root);
	if (strncmp(real, root, rl) || real[rl] != '/')
		return 0;
	int fd = open(real, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return 0;
	struct stat st;
	char magic[4];
	int ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && read(fd, magic, 4) == 4 && !memcmp(magic, "TZif", 4);
	close(fd);
	return ok;
}

static int write_atomic(const char *path, const char *data)
{
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof tmp, "%s.fltime%d", path, (int)getpid());
	int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
	if (fd < 0)
		return -1;
	size_t n = strlen(data);
	if (write(fd, data, n) != (ssize_t)n || fchmod(fd, 0644) < 0 || fsync(fd) < 0) {
		close(fd);
		unlink(tmp);
		return -1;
	}
	if (close(fd) < 0 || rename(tmp, path) < 0) {
		int e = errno;
		unlink(tmp);
		errno = e;
		return -1;
	}
	return 0;
}

static int cmd_zone(const char *name)
{
	if (!name_ok(name))
		return fail(3, "not a zone name");
	if (!zone_exists(name))
		return fail(3, "no such zone");
	char link[PATH_MAX], tmp[PATH_MAX], target[PATH_MAX], line[MAXNAME + 2];
	snprintf(target, sizeof target, "%s/%s", ZONEINFO, name);
	snprintf(link, sizeof link, "%s/localtime", ETCDIR);
	snprintf(tmp, sizeof tmp, "%s/localtime.fltime%d", ETCDIR, (int)getpid());
	unlink(tmp);
	if (symlink(target, tmp) < 0 || rename(tmp, link) < 0) {
		int e = errno;
		unlink(tmp);
		fprintf(stderr, "fltime-set: %s: %s\n", link, strerror(e));
		return 1;
	}
	char tzf[PATH_MAX];
	snprintf(tzf, sizeof tzf, "%s/timezone", ETCDIR);
	snprintf(line, sizeof line, "%s\n", name);
	if (write_atomic(tzf, line) < 0) {
		fprintf(stderr, "fltime-set: %s: %s\n", tzf, strerror(errno));
		return 1;
	}
	return 0;
}

/* NTP= in fltime.conf: 1 unless it says 0 */
static int ntp_enabled(void)
{
	char path[PATH_MAX], line[256];
	snprintf(path, sizeof path, "%s/fltime.conf", ETCDIR);
	FILE *f = fopen(path, "r");
	int on = 1;
	while (f && fgets(line, sizeof line, f))
		if (!strncmp(line, "NTP=", 4))
			on = atoi(line + 4) != 0;
	if (f)
		fclose(f);
	return on;
}

static int run(const char *prog, const char *a1, const char *a2)
{
	pid_t p = fork();
	if (p < 0)
		return -1;
	if (p == 0) {
		clearenv();
		setenv("PATH", "/usr/sbin:/usr/bin", 1);
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) {
			dup2(nul, 0);
			dup2(nul, 1);
			dup2(nul, 2);
		}
		execl(prog, prog, a1, a2, (char *)NULL);
		_exit(127);
	}
	int st;
	if (waitpid(p, &st, 0) < 0)
		return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int service(const char *action)
{
	char prog[PATH_MAX];
	snprintf(prog, sizeof prog, "%s/ntp", INITD);
	return run(prog, action, NULL);
}

static int cmd_ntp(const char *arg)
{
	int on;
	if (!strcmp(arg, "on"))
		on = 1;
	else if (!strcmp(arg, "off"))
		on = 0;
	else
		return fail(3, "ntp takes on or off");
	char path[PATH_MAX], line[256], out[2048] = "";
	snprintf(path, sizeof path, "%s/fltime.conf", ETCDIR);
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof line, f))
		if (strncmp(line, "NTP=", 4) && strlen(out) + strlen(line) < sizeof out - 16)
			strcat(out, line);
	if (f)
		fclose(f);
	size_t l = strlen(out);
	if (l && out[l - 1] != '\n' && l + 1 < sizeof out)
		strcat(out, "\n");
	strcat(out, on ? "NTP=1\n" : "NTP=0\n");
	if (write_atomic(path, out) < 0) {
		fprintf(stderr, "fltime-set: %s: %s\n", path, strerror(errno));
		return 1;
	}
	service(on ? "restart" : "stop");
	return 0;
}

static int cmd_time(const char *arg)
{
	if (!*arg || strlen(arg) > 11)
		return fail(3, "not a time");
	for (const char *p = arg; *p; p++)
		if (!isdigit((unsigned char)*p))
			return fail(3, "not a time");
	long long t = atoll(arg);
	if (t < 1700000000LL || t > 4102444800LL)
		return fail(3, "time out of range");
	if (ntp_enabled())
		return fail(4, "automatic time is on");
#ifndef NO_CLOCK
	struct timespec ts = {.tv_sec = (time_t)t, .tv_nsec = 0};
	if (clock_settime(CLOCK_REALTIME, &ts) < 0) {
		fprintf(stderr, "fltime-set: clock_settime: %s\n", strerror(errno));
		return 1;
	}
#endif
	/* the hardware clock follows, in what rc.conf says it keeps */
	char path[PATH_MAX], line[256];
	const char *mode = "--utc";
	snprintf(path, sizeof path, "%s/rc.conf", ETCDIR);
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof line, f))
		if (!strncmp(line, "HARDWARECLOCK=", 14) && strstr(line + 14, "localtime"))
			mode = "--localtime";
	if (f)
		fclose(f);
	run(HWCLOCK, "--systohc", mode);
	return 0;
}

int main(int argc, char **argv)
{
	umask(022);
	if (argc != 2 && argc != 3) {
		fputs("usage: fltime-set zone NAME | ntp on|off | sync | time EPOCH\n", stderr);
		return 2;
	}
#ifndef FLTIME_TEST
	if (setuid(0) < 0) {
		perror("fltime-set");
		return 1;
	}
#endif
	clearenv();
	setenv("PATH", "/usr/sbin:/usr/bin", 1);
	if (argc == 3 && !strcmp(argv[1], "zone"))
		return cmd_zone(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "ntp"))
		return cmd_ntp(argv[2]);
	if (argc == 3 && !strcmp(argv[1], "time"))
		return cmd_time(argv[2]);
	if (argc == 2 && !strcmp(argv[1], "sync"))
		return ntp_enabled() ? (service("restart") == 0 ? 0 : 1) : fail(4, "automatic time is off");
	fputs("usage: fltime-set zone NAME | ntp on|off | sync | time EPOCH\n", stderr);
	return 2;
}
