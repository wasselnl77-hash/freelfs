/* flpkg-seat -- install or remove packages for the person at the machine.
 * Installed setuid root, runnable only by the group seat (mode 4750), so the
 * Packages app can change the system without a password. It does exactly two
 * things:
 *
 *	flpkg-seat install NAME...
 *	flpkg-seat remove NAME...
 *
 * and runs /usr/bin/flpkg with -y for them. Nothing else gets through: every
 * NAME must be a plain package name, the environment is cleared, and no
 * option or file name can reach flpkg (a name never starts with "-" or ends
 * in ".flpkg", which flpkg would take for a file).
 * SPDX-License-Identifier: MIT */
#include "flauth-check.h"

#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef FLPKG_BIN
#define FLPKG_BIN "/usr/bin/flpkg"
#endif
#define MAX_NAMES 32
#define MAX_LEN 64

/* [a-z0-9+._-]+, starting with a letter or digit, at most MAX_LEN, and not
 * something flpkg would read as a package file. */
static int valid_name(const char *s)
{
	size_t n = strlen(s);
	if (n == 0 || n > MAX_LEN)
		return 0;
	if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= '0' && s[0] <= '9')))
		return 0;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '.' ||
		      c == '_' || c == '-'))
			return 0;
	}
	if (n >= 6 && !strcmp(s + n - 6, ".flpkg"))
		return 0;
	return 1;
}

int main(int argc, char **argv)
{
	if (argc < 3 || argc > 2 + MAX_NAMES ||
	    (strcmp(argv[1], "install") && strcmp(argv[1], "remove"))) {
		fputs("usage: flpkg-seat install|remove NAME...\n", stderr);
		return 2;
	}
	for (int i = 2; i < argc; i++) {
		if (!valid_name(argv[i])) {
			fputs("flpkg-seat: not a package name\n", stderr);
			return 2;
		}
	}
#ifndef FLPKG_SEAT_TEST
	if (geteuid() != 0) {
		fputs("flpkg-seat: not installed setuid root\n", stderr);
		return 1;
	}
	/* Changing the system asks for the person's password (flauth; no question on the live system's
	 * passwordless tester). Without a fresh stamp this program starts itself again through flauth, as
	 * the person, which finds the stamp the second time; FLAUTH_TRIED stops a loop. */
	if (!flauth_ok(getuid())) {
		uid_t u = getuid();
		gid_t g = getgid();
		if (getenv("FLAUTH_TRIED") || setresgid(g, g, g) < 0 || setresuid(u, u, u) < 0) {
			fputs("flpkg-seat: your password is needed (flauth)\n", stderr);
			return 77;
		}
		setenv("FLAUTH_TRIED", "1", 1);
		char reason[256];
		snprintf(reason, sizeof reason, "%s the package%s %s%s.", !strcmp(argv[1], "install") ? "Install" : "Remove",
			 argc > 3 ? "s" : "", argv[2], argc > 3 ? " and more" : "");
		char *args[MAX_NAMES + 10];
		int k = 0;
		args[k++] = "flauth";
		args[k++] = "--reason";
		args[k++] = reason;
		args[k++] = "--";
		args[k++] = "/usr/bin/flpkg-seat";
		for (int i = 1; i < argc; i++)
			args[k++] = argv[i];
		args[k] = NULL;
		execv("/usr/bin/flauth", args);
		fputs("flpkg-seat: your password is needed, and flauth is not installed\n", stderr);
		return 77;
	}
	/* real ids to root too, and none of the caller's groups */
	if (setgroups(0, NULL) < 0 || setgid(0) < 0 || setuid(0) < 0) {
		perror("flpkg-seat");
		return 1;
	}
#endif
	/* a known state: umask, directory, signals, descriptors, environment */
	umask(022);
	if (chdir("/") < 0)
		return 1;
	sigset_t none;
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	for (int s = 1; s < 32; s++)
		if (s != SIGKILL && s != SIGSTOP)
			signal(s, SIG_DFL);
	int in = open("/dev/null", O_RDONLY);
	if (in < 0)
		return 1;
	dup2(in, 0); /* flpkg never waits for a keyboard */
	for (int fd = 1; fd < 3; fd++)
		if (fcntl(fd, F_GETFD) < 0) {
			int nul = open("/dev/null", O_WRONLY);
			if (nul != fd)
				dup2(nul, fd);
		}
	for (int fd = 3; fd < 1024; fd++)
		close(fd);
	if (clearenv() != 0 || setenv("PATH", "/usr/sbin:/usr/bin", 1) != 0)
		return 1;

	char *args[MAX_NAMES + 6];
	int n = 0;
	args[n++] = "flpkg";
	args[n++] = argv[1];
	args[n++] = "-y";
	args[n++] = "--";
	for (int i = 2; i < argc; i++)
		args[n++] = argv[i];
	args[n] = NULL;
	execv(FLPKG_BIN, args);
	perror(FLPKG_BIN);
	return 1;
}
