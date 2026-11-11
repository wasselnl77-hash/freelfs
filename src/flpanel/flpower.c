/* flpower -- what the person at the machine may ask of the power: shut down, restart, and
 * power saving. Installed setuid root, runnable only by the group seat (mode 4750), so the
 * panel can do this without a password. It takes no input beyond these words:
 *
 *   flpower off | restart       poweroff, reboot
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
#include <dirent.h>
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

int main(int argc, char **argv)
{
	const char *cmd = NULL;
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
		fputs("usage: flpower off|restart|saver on|saver off\n", stderr);
		return 2;
	}
	if (setuid(0) < 0) {
		perror("flpower");
		return 1;
	}
	clearenv();
	setenv("PATH", "/usr/sbin:/usr/bin", 1);
	execl(cmd, cmd, (char *)NULL);
	perror(cmd);
	return 1;
}
