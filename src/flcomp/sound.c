/* sound.c -- system sounds: runs `flsound NAME` (src/flsound), detached.
 * Off when sounds=0 in desktop.conf (theme.c reads it). "tick" is limited here too, so a held
 * volume key does not start a process per key repeat; flsound limits it again.
 * Headless runs (tests) are silent.
 * SPDX-License-Identifier: MIT */
#include "comp.h"

#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

void sound_play(const char *name)
{
	if (C.headless || !theme_sounds() || getenv("FLCOMP_NO_SOUND"))
		return;
	if (!strcmp(name, "tick")) {
		static struct timespec last;
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		long ms = (now.tv_sec - last.tv_sec) * 1000L + (now.tv_nsec - last.tv_nsec) / 1000000L;
		if (last.tv_sec && ms < 80)
			return;
		last = now;
	}
	/* detached: main.c's SIGCHLD handler reaps it */
	pid_t pid = fork();
	if (pid != 0)
		return;
	sigset_t none;
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	signal(SIGPIPE, SIG_DFL);
	setsid();
	execlp("flsound", "flsound", name, (char *)NULL);
	_exit(127);
}
