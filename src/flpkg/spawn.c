/* Running other programs: build functions, tar, strip, triggers.
 * SPDX-License-Identifier: MIT */
#include "flpkg.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int wait_child(pid_t pid)
{
	int status;
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	return 128 + WTERMSIG(status);
}

static _Noreturn void exec_child(char *const argv[], char *const envp[], const char *cwd)
{
	if (cwd && chdir(cwd) < 0) {
		fprintf(stderr, "%s: cannot enter %s\n", progname, cwd);
		_exit(127);
	}
	if (envp)
		execvpe(argv[0], argv, envp);
	else
		execvp(argv[0], argv);
	fprintf(stderr, "%s: cannot run %s: %s\n", progname, argv[0], strerror(errno));
	_exit(127);
}

int run_argv(char *const argv[], char *const envp[], const char *cwd)
{
	fflush(NULL);
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0)
		exec_child(argv, envp, cwd);
	return wait_child(pid);
}

int run_capture(char *const argv[], char *const envp[], const char *cwd, struct strbuf *out)
{
	int fds[2];
	if (pipe(fds) < 0)
		return -1;
	fflush(NULL);
	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return -1;
	}
	if (pid == 0) {
		close(fds[0]);
		if (dup2(fds[1], STDOUT_FILENO) < 0)
			_exit(127);
		close(fds[1]);
		exec_child(argv, envp, cwd);
	}
	close(fds[1]);
	char buf[8192];
	ssize_t n;
	while ((n = read(fds[0], buf, sizeof(buf))) != 0) {
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		sb_add(out, buf, (size_t)n);
	}
	close(fds[0]);
	return wait_child(pid);
}

bool have_program(const char *name)
{
	const char *path = getenv("PATH");
	if (!path)
		path = "/usr/bin:/usr/sbin";
	char *copy = xstrdup(path), *save = NULL;
	bool found = false;
	for (char *dir = strtok_r(copy, ":", &save); dir && !found; dir = strtok_r(NULL, ":", &save)) {
		char *p = path_join(dir, name);
		found = access(p, X_OK) == 0;
		free(p);
	}
	free(copy);
	return found;
}
