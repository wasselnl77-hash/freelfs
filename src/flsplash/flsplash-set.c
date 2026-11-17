/* flsplash-set -- the boot screen's style, for the Themes app. Installed setuid root, group seat
 * (mode 4750), like flpkg-seat. It does exactly one thing:
 *
 *	flsplash-set flat | legacy
 *
 * and writes /etc/freelfs/splash.conf (splash=flat or splash=legacy) after the person's password
 * (flauth). Every check is here:
 *	- exactly one argument, and it is "flat" or "legacy";
 *	- the caller must be allowed (flauth_ok: root, an empty password, or a fresh flauth stamp);
 *	  without it the program starts itself again through flauth, as the person, once;
 *	- /etc/freelfs is a root-owned directory, not writable by others (made 0755 when missing);
 *	- the file is written as a new file (O_EXCL, no symlink followed) and renamed over the old one;
 *	  a symlink or other non-regular file at the place is refused.
 * Exit: 0 written; 2 bad argument; 1 failed; 77 the password is needed and flauth is missing;
 * otherwise flauth's own code (1 wrong, 2 cancelled, 3 locked out, 4 no password).
 * FLSPLASH_SET_TEST (tests only): no root and no password, the paths come from
 * FLSPLASH_SET_DIR and FLSPLASH_SET_CONF.
 * SPDX-License-Identifier: MIT */
#include "flauth-check.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DIR_DEFAULT "/etc/freelfs"
#define CONF_DEFAULT "/etc/freelfs/splash.conf"

static const char *conf_dir(void)
{
#ifdef FLSPLASH_SET_TEST
	return getenv("FLSPLASH_SET_DIR");
#else
	return DIR_DEFAULT;
#endif
}

static const char *conf_file(void)
{
#ifdef FLSPLASH_SET_TEST
	return getenv("FLSPLASH_SET_CONF");
#else
	return CONF_DEFAULT;
#endif
}

static int write_conf(const char *style)
{
	const char *dir = conf_dir(), *file = conf_file();
	char tmp[600];
	struct stat st;
	if (!dir || !file || strlen(dir) > 200 || strlen(file) > 400) {
		fputs("flsplash-set: no configuration path\n", stderr);
		return 1;
	}
	if (lstat(dir, &st) < 0) {
		if (errno != ENOENT || mkdir(dir, 0755) < 0) {
			perror("flsplash-set: /etc/freelfs");
			return 1;
		}
		if (lstat(dir, &st) < 0)
			return 1;
	}
#ifndef FLSPLASH_SET_TEST
	if (!S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 022)) {
		fputs("flsplash-set: /etc/freelfs is not a root directory only root can write\n", stderr);
		return 1;
	}
#endif
	if (lstat(file, &st) == 0 && !S_ISREG(st.st_mode)) {
		fputs("flsplash-set: the splash file is not a regular file; not touched\n", stderr);
		return 1;
	}
	snprintf(tmp, sizeof tmp, "%s/.splash.conf.new", dir);
	unlink(tmp);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
	if (fd < 0) {
		perror("flsplash-set: write");
		return 1;
	}
	char body[160];
	int n = snprintf(body, sizeof body,
	                 "# The boot screen (flsplash): flat (the default) or legacy. Written by flsplash-set.\n"
	                 "splash=%s\n",
	                 style);
	int ok = write(fd, body, (size_t)n) == n && fsync(fd) == 0;
	if (close(fd) < 0)
		ok = 0;
	if (!ok || rename(tmp, file) < 0) {
		perror("flsplash-set: write");
		unlink(tmp);
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 2 || (strcmp(argv[1], "flat") && strcmp(argv[1], "legacy"))) {
		fputs("usage: flsplash-set flat|legacy\n", stderr);
		return 2;
	}
#ifndef FLSPLASH_SET_TEST
	if (geteuid() != 0) {
		fputs("flsplash-set: not installed setuid root\n", stderr);
		return 1;
	}
	/* Changing the boot screen asks for the person's password (flauth). Without a fresh stamp this
	 * program starts itself again through flauth, as the person; FLAUTH_TRIED stops a loop. */
	if (!flauth_ok(getuid())) {
		uid_t u = getuid();
		gid_t g = getgid();
		if (getenv("FLAUTH_TRIED") || setresgid(g, g, g) < 0 || setresuid(u, u, u) < 0) {
			fputs("flsplash-set: your password is needed (flauth)\n", stderr);
			return 77;
		}
		setenv("FLAUTH_TRIED", "1", 1);
		char reason[128];
		snprintf(reason, sizeof reason, "Change the boot screen to %s.", argv[1]);
		char *args[] = {"flauth", "--reason", reason, "--", "/usr/bin/flsplash-set", argv[1], NULL};
		execv("/usr/bin/flauth", args);
		fputs("flsplash-set: your password is needed, and flauth is not installed\n", stderr);
		return 77;
	}
	/* real ids to root too, and none of the caller's groups */
	if (setgroups(0, NULL) < 0 || setgid(0) < 0 || setuid(0) < 0) {
		perror("flsplash-set");
		return 1;
	}
	if (clearenv() != 0 || setenv("PATH", "/usr/sbin:/usr/bin", 1) != 0)
		return 1;
	umask(022);
#endif
	return write_conf(argv[1]);
}
