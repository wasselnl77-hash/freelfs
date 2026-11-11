/* sound.h -- play a system sound: runs `flsound NAME` detached (it checks sounds=0 itself)
 * SPDX-License-Identifier: MIT */
#ifndef PANEL_SOUND_H
#define PANEL_SOUND_H
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static inline void panel_sound(const char *name)
{
	if (getenv("FLPANEL_NO_SOUND"))
		return;
	pid_t pid = fork();
	if (pid < 0)
		return;
	if (pid == 0) {
		pid_t g = fork();	/* the panel does not reap children: the grandchild is adopted */
		if (g == 0) {
			setsid();
			execlp("flsound", "flsound", name, (char *)NULL);
		}
		_exit(0);
	}
	waitpid(pid, NULL, 0);
}
#endif
