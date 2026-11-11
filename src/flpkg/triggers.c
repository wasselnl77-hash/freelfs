/* System triggers: commands flpkg runs after a transaction that touched
 * matching paths. They are defined by flpkg itself in LIBDIR/triggers, one
 * "GLOB COMMAND" per line; each distinct command runs once, with sh -c,
 * inside the root.
 *
 * With --root, a trigger runs chrooted into that root in a mount namespace
 * of its own, with /proc, /sys and /dev mounted there (tools like udev-hwdb
 * fail without /proc). The mounts vanish when the trigger exits.
 * SPDX-License-Identifier: MIT */
#include "txn.h"

#include <errno.h>
#include <fnmatch.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>

static void mount_in(const char *root, const char *dir, const char *src, const char *type,
                     unsigned long flags)
{
	char *target = path_join(root, dir);
	if (access(target, F_OK) == 0 && mount(src, target, type, flags, NULL) < 0 && verbose)
		warn("cannot mount %s in %s: %s", dir, root, strerror(errno));
	free(target);
}

static int run_in_root(const char *root, const char *cmd)
{
	fflush(NULL);
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		if (unshare(CLONE_NEWNS) == 0) {
			/* Keep the mounts below from reaching other namespaces. In a
			 * chroot "/" is no mount point and this fails (EINVAL); the
			 * namespace set up for the chroot is private already. */
			mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
			char *proc = path_join(root, "proc");
			/* a new proc needs our own pid namespace; else bind the current one */
			if (mount("proc", proc, "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL) < 0)
				mount_in(root, "proc", "/proc", NULL, MS_BIND | MS_REC);
			free(proc);
			mount_in(root, "sys", "/sys", NULL, MS_BIND | MS_REC);
			mount_in(root, "dev", "/dev", NULL, MS_BIND | MS_REC);
		} else if (verbose) {
			warn("no mount namespace for triggers (%s); running without /proc",
			     strerror(errno));
		}
		if (chroot(root) < 0 || chdir("/") < 0) {
			fprintf(stderr, "%s: cannot chroot to %s: %s\n", progname, root, strerror(errno));
			_exit(127);
		}
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	int status;
	while (waitpid(pid, &status, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

void run_triggers(const char *root, const struct strlist *touched)
{
	char *file = path_join(cfg.libdir, "triggers");
	char *text = read_file(file, NULL);
	if (!text) {
		if (verbose)
			warn("no triggers file %s", file);
		free(file);
		return;
	}
	struct strlist cmds = {0};
	char *save = NULL;
	for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *s = trim(line);
		if (!*s || *s == '#')
			continue;
		char *cmd = s + strcspn(s, " \t");
		if (!*cmd)
			continue;
		*cmd++ = '\0';
		cmd = trim(cmd);
		if (sl_has(&cmds, cmd))
			continue;
		for (size_t i = 0; i < touched->n; i++) {
			if (fnmatch(s, touched->v[i], 0) == 0) {
				sl_add(&cmds, cmd);
				break;
			}
		}
	}
	bool other_root = strcmp(root, "/") != 0;
	for (size_t i = 0; i < cmds.n; i++) {
		if (verbose)
			msg("trigger: %s", cmds.v[i]);
		char *argv[] = {"/bin/sh", "-c", cmds.v[i], NULL};
		int rc = other_root ? run_in_root(root, cmds.v[i]) : run_argv(argv, NULL, NULL);
		if (rc != 0)
			warn("trigger failed (exit %d): %s", rc, cmds.v[i]);
	}
	sl_free(&cmds);
	free(text);
	free(file);
}
