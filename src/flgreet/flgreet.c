/* flgreet -- the login screen's launcher. Runs as root on tty1, from inittab.
 * SPDX-License-Identifier: MIT
 *
 * It takes tty1 as its controlling terminal, runs a private flcomp whose only
 * client is flgreet-ui, and waits for the user name that comes back over a
 * pipe. Then it checks the name against /etc/passwd and becomes
 * `login -f NAME`: the password was already checked by flgreet-ui, and login
 * does the rest (groups, utmp, the shell). When login ends, init starts
 * flgreet again.
 *
 * If flcomp cannot start (no usable display, a crash, no flgreet-ui), the
 * console gets a normal agetty, so a broken desktop never locks you out.
 *
 * This program runs as root, so it stays small: it parses nothing but one
 * short line, never uses a shell, and starts login with a clean environment.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TTY "/dev/tty1"
#define UI "/usr/libexec/flgreet-ui"
#define MIN_UID 1000
#define MAX_UID 59999

static void say(const char *what)
{
	fprintf(stderr, "flgreet: %s\n", what);
}

/* flsplash, the boot screen, if there is one: --handover waits for its
 * animation to end and lets go of the screen, --text shows the boot text */
static void splash(const char *arg)
{
	if (access("/usr/sbin/flsplash", X_OK) < 0)
		return;
	pid_t pid = fork();
	if (pid == 0) {
		execl("/usr/sbin/flsplash", "flsplash", arg, (char *)NULL);
		_exit(127);
	}
	int st;
	while (pid > 0 && waitpid(pid, &st, 0) < 0 && errno == EINTR)
		;
}

/* the plain console login, used when the graphical one cannot run */
static void fallback(void)
{
	splash("--text");
	say("no graphical login, starting agetty");
	execl("/sbin/agetty", "agetty", "--noclear", "tty1", "38400", "linux", (char *)NULL);
	perror("flgreet: /sbin/agetty");
	sleep(5);
	_exit(1);
}

static void take_tty(void)
{
	setsid(); /* init made us a leader already: then this fails, which is fine */
	int fd = open(TTY, O_RDWR | O_NOCTTY);
	if (fd < 0) {
		perror("flgreet: " TTY);
		return;
	}
	ioctl(fd, TIOCSCTTY, 1);
	dup2(fd, 0);
	dup2(fd, 1);
	dup2(fd, 2);
	if (fd > 2)
		close(fd);
}

/* a name we are willing to hand to login: what useradd allows, no more */
static int name_ok(const char *s)
{
	size_t n = strlen(s);
	if (n < 1 || n > 32 || s[0] == '-')
		return 0;
	for (; *s; s++) {
		char c = *s;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
		      c == '-' || c == '.'))
			return 0;
	}
	return 1;
}

/* /run/user/UID, owned by the user, 0700 (the session's XDG_RUNTIME_DIR) */
static void runtime_dir(const struct passwd *pw)
{
	char path[64];
	mkdir("/run/user", 0755);
	snprintf(path, sizeof path, "/run/user/%u", (unsigned)pw->pw_uid);
	if (mkdir(path, 0700) < 0 && errno != EEXIST) {
		perror("flgreet: /run/user");
		return;
	}
	int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (fd < 0)
		return;
	fchown(fd, pw->pw_uid, pw->pw_gid);
	fchmod(fd, 0700);
	close(fd);
}

/* One run of the greeter. Returns 1 with the name in out, 0 for "show it again",
 * -1 when the graphical login does not work. */
static int run_greeter(char *out, size_t cap)
{
	int fds[2];
	if (access(UI, X_OK) < 0 || access("/usr/bin/flcomp", X_OK) < 0) {
		say("flcomp or flgreet-ui is missing");
		return -1;
	}
	if (pipe(fds) < 0)
		return -1;
	mkdir("/run/flgreet", 0700);
	chmod("/run/flgreet", 0700);
	time_t t0 = time(NULL);
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		close(fds[0]);
		char fdstr[16];
		snprintf(fdstr, sizeof fdstr, "%d", fds[1]);
		clearenv();
		setenv("PATH", "/usr/bin:/usr/sbin", 1);
		setenv("HOME", "/root", 1);
		setenv("TERM", "linux", 1);
		setenv("LANG", "C.UTF-8", 1);
		setenv("XDG_RUNTIME_DIR", "/run/flgreet", 1);
		setenv("FLCOMP_SETUP", UI, 1);
		setenv("FLCOMP_SESSION", "", 1);
		setenv("FLGREET_FD", fdstr, 1);
		if (access("/etc/freelfs/desktop.conf", R_OK) == 0)
			setenv("FLUI_CONF", "/etc/freelfs/desktop.conf", 1);
		execl("/usr/bin/flcomp", "flcomp", "--firstboot", (char *)NULL);
		perror("flgreet: flcomp");
		_exit(127);
	}
	close(fds[1]);
	/* at most one short line; read until every writer is gone */
	char buf[96];
	size_t n = 0;
	for (;;) {
		ssize_t r = read(fds[0], buf + n, sizeof buf - 1 - n);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		n += (size_t)r;
		if (n >= sizeof buf - 1)
			break; /* too long to be a name */
	}
	close(fds[0]);
	buf[n] = 0;
	int st = 0;
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
		;
	char *nl = strchr(buf, '\n');
	if (nl)
		*nl = 0;
	if (n && name_ok(buf) && n < cap) {
		snprintf(out, cap, "%s", buf);
		return 1;
	}
	if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
		say("the greeter failed");
		return -1;
	}
	/* a clean exit with no name: show the screen again. Three of them within
	 * two seconds each mean something is wrong, and the console gets a getty. */
	static int quick;
	if (time(NULL) - t0 < 2) {
		if (++quick >= 3) {
			say("the greeter keeps ending at once");
			return -1;
		}
		sleep(1);
	} else {
		quick = 0;
	}
	return 0;
}

int main(void)
{
	if (getuid() != 0) {
		say("must run as root (it is started by init)");
		return 1;
	}
	take_tty();
	splash("--handover");
	for (int tries = 0; tries < 20; tries++) {
		char name[64];
		int r = run_greeter(name, sizeof name);
		if (r < 0)
			fallback();
		if (r == 0)
			continue;
		struct passwd *pw = getpwnam(name);
		if (!pw || strcmp(pw->pw_name, name) || pw->pw_uid < MIN_UID || pw->pw_uid > MAX_UID) {
			say("refusing that user name");
			continue;
		}
		runtime_dir(pw);
		clearenv();
		setenv("TERM", "linux", 1);
		setenv("PATH", "/usr/bin:/usr/sbin", 1);
		execl("/usr/bin/login", "login", "-f", name, (char *)NULL);
		perror("flgreet: /usr/bin/login");
		fallback();
	}
	fallback();
	return 1;
}
