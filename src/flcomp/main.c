/* flcomp -- the FreeLFS Wayland compositor: startup, output global, children.
 * SPDX-License-Identifier: MIT */
#include "comp.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct comp C;

void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("flcomp: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	exit(1);
}

void quit(int code)
{
	C.exit_code = code;
	wl_display_terminate(C.dpy);
}

struct reslink *reslink_add(struct wl_list *l, struct wl_resource *res)
{
	struct reslink *rl = calloc(1, sizeof(*rl));
	if (!rl)
		return NULL;
	rl->res = res;
	wl_list_insert(l->prev, &rl->link);
	return rl;
}

void reslink_destroy(struct wl_resource *res)
{
	struct reslink *rl = wl_resource_get_user_data(res);
	wl_list_remove(&rl->link);
	free(rl);
}

/* ---- wl_output ------------------------------------------------------------- */

static void output_release(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static const struct wl_output_interface output_impl = { .release = output_release };

static void output_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &wl_output_interface, version, id);
	struct reslink *l = r ? reslink_add(&C.outputs, r) : NULL;
	if (!l) {
		wl_client_post_no_memory(c);
		return;
	}
	wl_resource_set_implementation(r, &output_impl, l, reslink_destroy);
	wl_output_send_geometry(r, 0, 0, 0, 0, WL_OUTPUT_SUBPIXEL_UNKNOWN, "FreeLFS", "flcomp",
				WL_OUTPUT_TRANSFORM_NORMAL);
	wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, C.prim.x2 - C.prim.x1, C.prim.y2 - C.prim.y1, C.refresh);
	if (version >= WL_OUTPUT_SCALE_SINCE_VERSION)
		wl_output_send_scale(r, 1);
	if (version >= WL_OUTPUT_DONE_SINCE_VERSION)
		wl_output_send_done(r);
}

/* ---- children ----------------------------------------------------------------- */

static pid_t panel_pid, setup_pid, session_pid;
static int panel_restarts;
static bool panel_respawn;	/* killed on purpose (the output changed): start it again */
static bool firstboot;

static pid_t spawn_keep(const char *prog, int keep)
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGPIPE, SIG_DFL);
		{ const char *h_ = getenv("HOME"); if (!h_ || !*h_ || chdir(h_) != 0) (void)!chdir("/"); }	/* programs start in $HOME, not in a folder that may go away */
		for (int fd = 3; fd < 1024; fd++)	/* no inherited descriptors, except the greeter's pipe for flgreet-ui */
			if (fd != keep)
				close(fd);
		execlp(prog, prog, (char *)NULL);
		fprintf(stderr, "flcomp: cannot run %s: %s\n", prog, strerror(errno));
		_exit(127);
	}
	return pid;
}

static pid_t spawn(const char *prog)
{
	return spawn_keep(prog, -1);
}

/* flfiles, flterm, fllock for the keyboard shortcuts (seat.c); it is reaped like any child */
void run_program(const char *prog)
{
	if (C.nolock)	/* login screen / first boot runs as root: launch nothing */
		return;
	spawn(prog);
}

static void start_panel(void)
{
	const char *cmd = getenv("FLCOMP_PANEL");
	if (!cmd)
		cmd = "flpanel";
	if (*cmd)
		panel_pid = spawn(cmd);
}

/* Services of the desktop session (D-Bus, PipeWire): /usr/lib/flcomp/session, or
 * $FLCOMP_SESSION; set it to the empty string for none. */
static void start_session(void)
{
	const char *cmd = getenv("FLCOMP_SESSION");
	if (!cmd)
		cmd = "/usr/lib/flcomp/session";
	if (*cmd && access(cmd, X_OK) == 0)
		session_pid = spawn(cmd);
}

static int sigchld_cb(int sig, void *data)
{
	int st;
	pid_t pid;
	while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
		int code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
		if (xwm_child_exited(pid, st)) {
			continue;	/* Xwayland (xwm.c) */
		} else if (pid == setup_pid) {
			setup_pid = 0;
			quit(code);
		} else if (pid == session_pid) {
			session_pid = 0; /* it only starts daemons; nothing to restart */
		} else if (pid == panel_pid) {
			panel_pid = 0;
			if (panel_respawn) {
				panel_respawn = false;
				start_panel();
			} else if (code != 127 && panel_restarts++ < 5) {
				start_panel();
			}
		}
	}
	return 0;
}

/* The panel is as wide as the output and reads that once at start. */
void output_restart_panel(void)
{
	if (panel_pid > 0 && !firstboot) {
		panel_respawn = true;
		kill(panel_pid, SIGTERM);
	}
}

/* wl_output: the desktop's size changed */
void outputs_send_mode(void)
{
	struct reslink *l;
	wl_list_for_each(l, &C.outputs, link) {
		wl_output_send_mode(l->res, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, C.prim.x2 - C.prim.x1, C.prim.y2 - C.prim.y1, C.refresh);
		if (wl_resource_get_version(l->res) >= WL_OUTPUT_DONE_SINCE_VERSION)
			wl_output_send_done(l->res);
	}
}

static int term_cb(int sig, void *data)
{
	quit(0);
	return 0;
}

/* test hook for flcrash-test: end with status 3, as a failing flcomp would */
static int usr2_cb(int sig, void *data)
{
	fprintf(stderr, "flcomp: SIGUSR2, exiting with status 3 (flcrash-test)\n");
	quit(3);
	return 0;
}

static int shot_cb(int sig, void *data)
{
	const char *p = C.screenshot ? C.screenshot : "flcomp-screenshot.ppm";
	if (write_screenshot(p))
		fprintf(stderr, "flcomp: screenshot %s: %s\n", p, strerror(errno));
	return 0;
}

/* XDG_RUNTIME_DIR is where the socket goes; make one if the login did not. */
static void runtime_dir(void)
{
	const char *d = getenv("XDG_RUNTIME_DIR");
	if (d && *d && access(d, W_OK | X_OK) == 0)
		return;
	char path[128];
	snprintf(path, sizeof(path), "/tmp/flcomp-runtime-%u", (unsigned)getuid());
	if (mkdir(path, 0700) && errno != EEXIST)
		die("cannot create %s: %s", path, strerror(errno));
	chmod(path, 0700);
	setenv("XDG_RUNTIME_DIR", path, 1);
}

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--firstboot")) {
			firstboot = true;
		} else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			puts("usage: flcomp [--firstboot]\n"
			     "  FLCOMP_HEADLESS=WxH   no display or input, for tests\n"
			     "  FLCOMP_SCREENSHOT=F   write a PPM of the screen to F on SIGUSR1\n"
			     "  FLCOMP_PANEL=CMD      panel program (default flpanel, empty for none)\n"
			     "  FLCOMP_SESSION=CMD    session services (default /usr/lib/flcomp/session, empty for none)\n"
			     "  FLCOMP_SETUP=CMD      program run by --firstboot (default flsetup)");
			return 0;
		} else {
			die("unknown option %s", argv[i]);
		}
	}
	signal(SIGPIPE, SIG_IGN);
	C.nolock = firstboot;
	runtime_dir();
	if (!firstboot && !getenv("DBUS_SESSION_BUS_ADDRESS")) {
		/* the session script starts the bus at this fixed address; clients find it
		 * from the environment they inherit from here */
		char addr[256];
		snprintf(addr, sizeof addr, "unix:path=%s/bus", getenv("XDG_RUNTIME_DIR"));
		setenv("DBUS_SESSION_BUS_ADDRESS", addr, 1);
	}

	wl_list_init(&C.surfaces);
	wl_list_init(&C.windows);
	wl_list_init(&C.popups);
	wl_list_init(&C.outputs);
	wl_list_init(&C.shells);
	C.screenshot = getenv("FLCOMP_SCREENSHOT");

	C.dpy = wl_display_create();
	if (!C.dpy)
		die("cannot create the display");
	C.loop = wl_display_get_event_loop(C.dpy);

	config_init();
	anim_init();
	if (backend_init() < 0) {
		backend_fini();
		return 1;
	}
	output_init();
	if (!C.shadow)
		C.shadow = calloc((size_t)C.w * C.h, 4);
	if (!C.shadow)
		die("out of memory");
	night_init();
	wall_init();

	surface_init();
	dmabuf_init();
	xdg_init();
	seat_init();
	flshell_init();
	screencopy_init();
	config_apply_start();
	wl_global_create(C.dpy, &wl_output_interface, 3, NULL, output_bind);

	const char *sock = wl_display_add_socket_auto(C.dpy);
	if (!sock)
		die("cannot create the Wayland socket in %s", getenv("XDG_RUNTIME_DIR"));
	setenv("WAYLAND_DISPLAY", sock, 1);
	setenv("XDG_SESSION_TYPE", "wayland", 1);
	setenv("XDG_CURRENT_DESKTOP", "FreeLFS", 1);
	if (!firstboot)
		xwm_init();	/* DISPLAY for X11 programs; Xwayland itself starts with the first one */
	fprintf(stderr, "flcomp: %dx%d%s on %s\n", C.w, C.h, C.headless ? " (headless)" : "", sock);
	if (C.scale != 100)
		fprintf(stderr, "flcomp: screen %dx%d, scale %d%%\n", C.pw, C.ph, C.scale);

	wl_event_loop_add_signal(C.loop, SIGINT, term_cb, NULL);
	wl_event_loop_add_signal(C.loop, SIGTERM, term_cb, NULL);
	wl_event_loop_add_signal(C.loop, SIGCHLD, sigchld_cb, NULL);
	wl_event_loop_add_signal(C.loop, SIGUSR1, shot_cb, NULL);
	wl_event_loop_add_signal(C.loop, SIGUSR2, usr2_cb, NULL);

	if (!firstboot)
		sound_play("login");	/* the session starts (sounds=0 or a headless run: silent) */
	if (firstboot) {
		const char *cmd = getenv("FLCOMP_SETUP");
		{
			const char *fdv = getenv("FLGREET_FD");
			setup_pid = spawn_keep(cmd && *cmd ? cmd : "flsetup", fdv ? atoi(fdv) : -1);
		}
		if (setup_pid < 0)
			die("cannot run flsetup");
	} else {
		start_panel();
		start_session();
	}

	backend_start();
	damage_all();
	wl_display_run(C.dpy);

	if (session_pid > 0)
		kill(session_pid, SIGTERM);
	if (panel_pid > 0)
		kill(panel_pid, SIGTERM);
	if (setup_pid > 0)
		kill(setup_pid, SIGTERM);
	xwm_fini();
	backend_fini();
	return C.exit_code;
}
