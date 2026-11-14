/* flsocketd -- start a daemon when someone connects to its Unix socket.
 *
 *   flsocketd [-m MODE] [-g GROUP] SOCKET -- COMMAND [ARGS...]
 *
 * Binds and listens on SOCKET and does nothing else (about 0.2 MB of RAM) until a client
 * connects. Then it starts COMMAND with the listening socket as fd 3 and in the environment
 * UPSTART_EVENTS=socket and UPSTART_FDS=3 (the on-demand protocol CUPS knows), and waits for it.
 * The connection stays in the socket's queue and is accepted by the daemon itself, so peer
 * credentials (SO_PEERCRED) are the client's own. When the daemon exits (CUPS does after
 * IdleExitTimeout), flsocketd listens again. SIGTERM removes the socket, ends the daemon and exits.
 * SPDX-License-Identifier: MIT */
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t stop;
static void on_term(int sig) { (void)sig; stop = 1; }

static void usage(void)
{
	fprintf(stderr, "usage: flsocketd [-m MODE] [-g GROUP] SOCKET -- COMMAND [ARGS...]\n");
	exit(2);
}

int main(int argc, char **argv)
{
	mode_t mode = 0666;
	const char *group = NULL;
	int opt;
	while ((opt = getopt(argc, argv, "+m:g:")) != -1) {
		if (opt == 'm')
			mode = (mode_t)strtol(optarg, NULL, 8);
		else if (opt == 'g')
			group = optarg;
		else
			usage();
	}
	if (optind >= argc)
		usage();
	const char *path = argv[optind++];
	if (optind < argc && !strcmp(argv[optind], "--"))
		optind++;
	if (optind >= argc)
		usage();
	char **cmd = argv + optind;

	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	if (strlen(path) >= sizeof sa.sun_path) {
		fprintf(stderr, "flsocketd: path too long\n");
		return 2;
	}
	strcpy(sa.sun_path, path);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		perror("flsocketd: socket");
		return 1;
	}
	unlink(path);
	umask(0777);
	if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 16) < 0) {
		fprintf(stderr, "flsocketd: %s: %s\n", path, strerror(errno));
		return 1;
	}
	if (group) {
		struct group *g = getgrnam(group);
		if (g && chown(path, (uid_t)-1, g->gr_gid) < 0)
			perror("flsocketd: chown");
	}
	chmod(path, mode);

	struct sigaction act = { .sa_handler = on_term };
	sigaction(SIGTERM, &act, NULL);
	sigaction(SIGINT, &act, NULL);
	signal(SIGPIPE, SIG_IGN);

	pid_t child = 0;
	while (!stop) {
		if (!child) {
			struct pollfd pf = { .fd = fd, .events = POLLIN };
			int r = poll(&pf, 1, 1000);
			if (r <= 0 || !(pf.revents & POLLIN))
				continue;
			child = fork();
			if (child < 0) {
				child = 0;
				sleep(1);
				continue;
			}
			if (!child) {
				if (fd != 3) {
					dup2(fd, 3);
					close(fd);
				} else {
					fcntl(3, F_SETFD, 0);
				}
				setenv("UPSTART_EVENTS", "socket", 1);
				setenv("UPSTART_FDS", "3", 1);
				signal(SIGTERM, SIG_DFL);
				signal(SIGINT, SIG_DFL);
				signal(SIGPIPE, SIG_DFL);
				execvp(cmd[0], cmd);
				fprintf(stderr, "flsocketd: %s: %s\n", cmd[0], strerror(errno));
				_exit(127);
			}
			continue;
		}
		int st;
		pid_t w = waitpid(child, &st, WNOHANG);
		if (w == child) {
			child = 0;
			/* a daemon that dies at once must not be restarted in a tight loop */
			sleep(1);
		} else {
			poll(NULL, 0, 500);
		}
	}
	if (child) {
		kill(child, SIGTERM);
		for (int i = 0; i < 50 && waitpid(child, NULL, WNOHANG) == 0; i++)
			poll(NULL, 0, 100);
	}
	unlink(path);
	return 0;
}
