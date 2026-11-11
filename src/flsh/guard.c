/* guard.c -- the easter egg: an interactive `rm -r /sysflfs` asks first
 * SPDX-License-Identifier: MIT
 *
 * Only interactive shells, only the external rm, only with -r, -R or --recursive. The paths are
 * handed to `flcrash --cant`, which knows the vital ones (/sysflfs, /sysflfs/bin, /boot, the
 * classic links ...), shows "You can't do that." (full screen on the desktop, text on a
 * console) and exits 1 on "Never mind". Scripts and `sh -c` are never asked, and coreutils' rm
 * itself is untouched: `command rm` from a script, or /usr/bin/rm called by another program,
 * does what it always did.
 */
#include "flsh.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* true: run rm; false: the person said "Never mind" (status 1, nothing was run) */
bool guard_rm_allows(const char *cmdpath, char **argv)
{
	if (!sh.interactive)
		return true;
	const char *base = strrchr(cmdpath, '/');
	base = base ? base + 1 : cmdpath;
	if (strcmp(base, "rm"))
		return true;
	size_t argc = 0;
	while (argv[argc])
		argc++;
	char **ask = calloc(argc + 3, sizeof *ask);
	if (!ask)
		return true;
	size_t n = 0;
	ask[n++] = "flcrash";
	ask[n++] = "--cant";
	bool recursive = false, opts = true;
	for (size_t i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (opts && !strcmp(a, "--")) {
			opts = false;
		} else if (opts && a[0] == '-' && a[1] == '-') {
			if (!strcmp(a, "--recursive"))
				recursive = true;
		} else if (opts && a[0] == '-' && a[1]) {
			if (strchr(a + 1, 'r') || strchr(a + 1, 'R'))
				recursive = true;
		} else {
			ask[n++] = argv[i];
		}
	}
	bool ok = true;
	if (recursive && n > 2) {
		pid_t p = fork();
		if (p == 0) {
			/* a path that looks like an option must not be taken for one */
			execvp("flcrash", ask);
			_exit(0); /* not installed: no questions asked */
		}
		int st = 0;
		if (p > 0) {
			while (waitpid(p, &st, 0) < 0 && errno == EINTR)
				;
			ok = !(WIFEXITED(st) && WEXITSTATUS(st) == 1);
		}
	}
	free(ask);
	return ok;
}
