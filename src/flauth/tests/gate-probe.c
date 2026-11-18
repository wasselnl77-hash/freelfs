/* gate-probe -- asks the password policy (flauth-check.h) whether this user may do the root action of CLASS
 * right now. Test program for policy.sh; it is not installed.
 *   gate-probe check CLASS   the answer for this process (its parent is the shell that started it)
 *   gate-probe spawn CLASS   the answer for a child of this program (its parent is this program's executable)
 *   gate-probe gate CLASS    what a helper does: allowed (prints 1), or starts flauth (-DFLAUTH_BIN=...) which
 *                            runs this program again with the same arguments; the second run prints 1
 * Prints 1 (allowed) or 0 (refused). SPDX-License-Identifier: MIT */
#include "../flauth-check.h"

#include <sys/wait.h>

int main(int argc, char **argv)
{
	if (argc != 3) {
		fputs("usage: gate-probe check|spawn|gate CLASS\n", stderr);
		return 2;
	}
	if (!strcmp(argv[1], "check")) {
		printf("%d\n", flauth_allowed(getuid(), argv[2]));
		return 0;
	}
	if (!strcmp(argv[1], "gate")) {
		int tried = getenv("FLAUTH_TRIED") != NULL;
		printf("%d\n", flauth_gate(tried, argv[2], "Test.", argv[0], argc, argv) ? 1 : 0);
		return 0;
	}
	if (!strcmp(argv[1], "spawn")) {
		fflush(stdout);
		pid_t p = fork();
		if (p == 0) {
			printf("%d\n", flauth_allowed(getuid(), argv[2]));
			fflush(stdout);
			_exit(0);
		}
		int st = 0;
		waitpid(p, &st, 0);
		return 0;
	}
	return 2;
}
