/* flpower -- what the person at the machine may ask of the power: shut down, restart, and
 * power saving. Installed setuid root, runnable only by the group seat (mode 4750), so the
 * panel can do this without a password. It takes no input beyond these words:
 *
 *   flpower off | restart       poweroff, reboot (exit 0 once init has been told, else a message
 *                               on stderr and a non-zero status)
 *   flpower logout              ends the caller's own desktop session (SIGTERM to the caller's flcomp;
 *                               needs no privilege)
 *   flpower saver on            every CPU: governor powersave, energy preference power
 *                               (what was there is kept in /run/flpower-saver)
 *   flpower saver off           puts that back
 *
 * Power saving only writes two fixed sysfs files per CPU (cpufreq/scaling_governor and
 * cpufreq/energy_performance_preference) with values taken from a fixed list, and only if
 * the kernel says it supports them. Nothing the caller passes ends up in a path or a value.
 * Tests: when not running as root, FLPOWER_CPU=DIR replaces /sys/devices/system/cpu and
 * FLPOWER_STATE=FILE replaces /run/flpower-saver; a privileged copy ignores both.
 * SPDX-License-Identifier: MIT */
#include <ctype.h>
#include <dirent.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *cpu_dir(void)
{
	const char *e = geteuid() != 0 ? getenv("FLPOWER_CPU") : NULL;
	return e && *e ? e : "/sys/devices/system/cpu";
}

static const char *state_file(void)
{
	const char *e = geteuid() != 0 ? getenv("FLPOWER_STATE") : NULL;
	return e && *e ? e : "/run/flpower-saver";
}

static int read_line(const char *path, char *out, size_t cap)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(out, (int)cap, f))
		out[0] = 0;
	fclose(f);
	out[strcspn(out, "\r\n")] = 0;
	return 0;
}

static int write_line(const char *path, const char *val)
{
	FILE *f = fopen(path, "w");
	if (!f)
		return -1;
	int r = fprintf(f, "%s\n", val) < 0;
	if (fclose(f))
		r = 1;
	return r ? -1 : 0;
}

/* is word one of the space separated words of list? */
static int has_word(const char *list, const char *word)
{
	size_t n = strlen(word);
	for (const char *s = list; *s;) {
		while (*s == ' ')
			s++;
		const char *e = s;
		while (*e && *e != ' ')
			e++;
		if ((size_t)(e - s) == n && !strncmp(s, word, n))
			return 1;
		s = e;
	}
	return 0;
}

/* the value to restore must be a plain word: letters, digits, '_' and '-' */
static int plain(const char *s)
{
	if (!*s || strlen(s) > 40)
		return 0;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '_' || *s == '-'))
			return 0;
	return 1;
}

static int saver_on(void)
{
	if (access(state_file(), F_OK) == 0)
		return 0; /* already on: keep what was saved the first time */
	DIR *d = opendir(cpu_dir());
	if (!d) {
		perror("flpower: cpu");
		return 1;
	}
	FILE *st = fopen(state_file(), "w");
	if (!st) {
		perror("flpower: state");
		closedir(d);
		return 1;
	}
	struct dirent *e;
	int changed = 0;
	while ((e = readdir(d))) {
		int n;
		char tail;
		if (sscanf(e->d_name, "cpu%d%c", &n, &tail) != 1)
			continue;
		char gov[320], epp[320], buf[400], avail[400] = "", cur[64] = "", curepp[64] = "";
		snprintf(gov, sizeof gov, "%s/%s/cpufreq/scaling_governor", cpu_dir(), e->d_name);
		snprintf(epp, sizeof epp, "%s/%s/cpufreq/energy_performance_preference", cpu_dir(), e->d_name);
		read_line(gov, cur, sizeof cur);
		read_line(epp, curepp, sizeof curepp);
		fprintf(st, "%d %s %s\n", n, plain(cur) ? cur : "-", plain(curepp) ? curepp : "-");
		snprintf(buf, sizeof buf, "%s/%s/cpufreq/scaling_available_governors", cpu_dir(), e->d_name);
		read_line(buf, avail, sizeof avail);
		if (plain(cur) && has_word(avail, "powersave") && strcmp(cur, "powersave") && !write_line(gov, "powersave"))
			changed++;
		snprintf(buf, sizeof buf, "%s/%s/cpufreq/energy_performance_available_preferences", cpu_dir(), e->d_name);
		avail[0] = 0;
		read_line(buf, avail, sizeof avail);
		if (plain(curepp) && has_word(avail, "power") && strcmp(curepp, "power") && !write_line(epp, "power"))
			changed++;
	}
	closedir(d);
	if (fclose(st))
		return 1;
	(void)changed;
	return 0;
}

static int saver_off(void)
{
	FILE *st = fopen(state_file(), "r");
	if (!st)
		return 0; /* never turned on */
	char line[200];
	while (fgets(line, sizeof line, st)) {
		int n;
		char g[64], p[64];
		if (sscanf(line, "%d %63s %63s", &n, g, p) != 3 || n < 0 || n > 4096)
			continue;
		char path[320];
		if (strcmp(g, "-") && plain(g)) {
			snprintf(path, sizeof path, "%s/cpu%d/cpufreq/scaling_governor", cpu_dir(), n);
			write_line(path, g);
		}
		if (strcmp(p, "-") && plain(p)) {
			snprintf(path, sizeof path, "%s/cpu%d/cpufreq/energy_performance_preference", cpu_dir(), n);
			write_line(path, p);
		}
	}
	fclose(st);
	unlink(state_file());
	return 0;
}

/* the caller's own compositor: /proc/PID with our real uid and the name flcomp */
static int logout_session(void)
{
	uid_t me = getuid();
	if (geteuid() == 0 && setresuid(me, me, me) < 0) {
		perror("flpower: cannot drop privileges");
		return 1;
	}
	DIR *d = opendir("/proc");
	if (!d) {
		perror("flpower: /proc");
		return 1;
	}
	struct dirent *e;
	int hit = 0;
	while ((e = readdir(d))) {
		if (!isdigit((unsigned char)e->d_name[0]))
			continue;
		char path[300], comm[64] = "";
		struct stat st;
		snprintf(path, sizeof path, "/proc/%s", e->d_name);
		if (stat(path, &st) < 0 || st.st_uid != me)
			continue;
		snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
		read_line(path, comm, sizeof comm);
		if (!strcmp(comm, "flcomp") && kill((pid_t)atoi(e->d_name), SIGTERM) == 0)
			hit++;
	}
	closedir(d);
	if (!hit) {
		fputs("flpower: no desktop session of yours is running\n", stderr);
		return 1;
	}
	return 0;
}

/* run poweroff or reboot; its own words and our status go to stderr/exit code */
static int run_power(const char *cmd)
{
	if (access(cmd, X_OK) < 0) {
		fprintf(stderr, "flpower: %s: %s\n", cmd, strerror(errno));
		return 1;
	}
	pid_t pid = fork();
	if (pid < 0) {
		perror("flpower: fork");
		return 1;
	}
	if (pid == 0) {
		clearenv();
		setenv("PATH", "/usr/sbin:/usr/bin", 1);
		execl(cmd, cmd, (char *)NULL);
		fprintf(stderr, "flpower: %s: %s\n", cmd, strerror(errno));
		_exit(127);
	}
	int st = 0;
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
		;
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
		fprintf(stderr, "flpower: %s failed (status %d)\n", cmd, WIFEXITED(st) ? WEXITSTATUS(st) : -1);
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	/* nothing inherited: the panel's status pipe on fd 3 must not stay open in anything we start */
	for (int fd = 3; fd < 256; fd++)
		close(fd);
	const char *cmd = NULL;
	if (argc == 2 && !strcmp(argv[1], "logout"))
		return logout_session();
	if (argc == 2 && !strcmp(argv[1], "off"))
		cmd = "/usr/sbin/poweroff";
	else if (argc == 2 && !strcmp(argv[1], "restart"))
		cmd = "/usr/sbin/reboot";
	else if (argc == 3 && !strcmp(argv[1], "saver") && (!strcmp(argv[2], "on") || !strcmp(argv[2], "off"))) {
		if (geteuid() == 0 && setuid(0) < 0) {
			perror("flpower");
			return 1;
		}
		umask(077);
		return !strcmp(argv[2], "on") ? saver_on() : saver_off();
	}
	if (!cmd) {
		fputs("usage: flpower off|restart|logout|saver on|saver off\n", stderr);
		return 2;
	}
	if (geteuid() != 0) {
		fputs("flpower: not installed setuid root (the system's file ownership was not applied)\n", stderr);
		return 1;
	}
	if (setuid(0) < 0) {
		perror("flpower");
		return 1;
	}
	return run_power(cmd);
}
