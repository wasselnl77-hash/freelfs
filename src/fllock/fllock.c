/* fllock -- the FreeLFS lock screen
 * SPDX-License-Identifier: MIT
 *
 * A big clock, the date and a password field on a plain screen. The locking itself
 * is the compositor's (fl_shell lock): while this program holds the lock nothing else
 * is shown or gets input, and if it dies the screen stays locked and flcomp starts it
 * again. It only asks for the lock, draws, checks the password and says unlock.
 *
 * The password is checked by fllock-check (setuid root, group seat), which compares
 * it with the caller's own shadow entry: this program never runs as root and never
 * sees /etc/shadow. The password goes to the helper through a pipe, the buffer is
 * wiped after use, and nothing is logged.
 *
 *   fllock               lock and wait for the password
 *   fllock --demo [--typed N] [--wrong|--busy|--stuck]
 *                        no lock request, a fixed clock and the given state, for
 *                        screenshots (FLUI_PPM=FILE writes the picture)
 * Environment (tests): FLLOCK_CHECK=PATH uses another password checker.
 */
#include <errno.h>
#include <fcntl.h>
#include <flui.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "fl-shell-client-protocol.h"

#define CHECK_PATH "/usr/libexec/fllock-check"
#define PW_CAP 256

enum { ID_PW = 1 };

struct L {
	fl_app *app;
	fl_win *win;
	struct fl_shell *shell;
	int shell_ver;
	fl_font *f_clock, *f_date, *f_name, *f_small;
	fl_timer *clock;
	char user[64], label[64];
	char pw[PW_CAP];
	char msg[160], msg2[160];
	int msg_err;
	int demo, typed, busy;
	long locked_at;
	/* the checker */
	pid_t pid;
	int rfd;
	int quit_code;
};

/* ---- the password check ------------------------------------------------------------ */

static void check_done(struct L *l, int status)
{
	l->busy = 0;
	l->msg[0] = l->msg2[0] = 0;
	l->msg_err = 1;
	int code = WIFEXITED(status) ? WEXITSTATUS(status) : 255;
	if (code == 0) {
		if (l->shell)
			fl_shell_unlock(l->shell);
		if (l->shell && fl_app_display(l->app))
			wl_display_roundtrip(fl_app_display(l->app));
		fl_app_quit(l->app);
		return;
	}
	if (code == 1) {
		snprintf(l->msg, sizeof l->msg, "Wrong password.");
	} else if (code == 2) {
		snprintf(l->msg, sizeof l->msg, "This account has no password that can unlock the screen.");
		snprintf(l->msg2, sizeof l->msg2, "Press Ctrl+Alt+F2, log in there and run: pkill flcomp");
	} else {
		snprintf(l->msg, sizeof l->msg, "The password could not be checked (fllock-check).");
		snprintf(l->msg2, sizeof l->msg2, "Press Ctrl+Alt+F2, log in there and run: pkill flcomp");
	}
	fl_win_redraw(l->win);
}

static void check_ready(void *ud, int fd, short rev)
{
	struct L *l = ud;
	char tmp[16];
	ssize_t r = read(fd, tmp, sizeof tmp);
	if (r > 0 || (r < 0 && (errno == EAGAIN || errno == EINTR)))
		return; /* the helper says nothing; wait for the end */
	fl_fd_remove(l->app, fd);
	close(fd);
	l->rfd = -1;
	int st = 0;
	if (l->pid > 0)
		waitpid(l->pid, &st, 0);
	l->pid = 0;
	check_done(l, st);
}

/* Starts the helper with the password on its stdin; the answer arrives through its stdout
 * closing (check_ready). The password buffer is wiped here in any case. */
static void check_start(struct L *l)
{
	const char *helper = getenv("FLLOCK_CHECK");
	if (!helper || !*helper)
		helper = CHECK_PATH;
	int in[2], out[2];
	if (pipe2(in, O_CLOEXEC) < 0)
		goto fail;
	if (pipe2(out, O_CLOEXEC) < 0) {
		close(in[0]);
		close(in[1]);
		goto fail;
	}
	pid_t pid = fork();
	if (pid < 0) {
		close(in[0]);
		close(in[1]);
		close(out[0]);
		close(out[1]);
		goto fail;
	}
	if (pid == 0) {
		dup2(in[0], 0);
		dup2(out[1], 1);
		int nul = open("/dev/null", O_WRONLY);
		if (nul >= 0)
			dup2(nul, 2);
		signal(SIGPIPE, SIG_DFL);
		execl(helper, helper, (char *)NULL);
		_exit(127);
	}
	close(in[0]);
	close(out[1]);
	char line[PW_CAP + 2];
	size_t n = strlen(l->pw);
	memcpy(line, l->pw, n);
	line[n++] = '\n';
	signal(SIGPIPE, SIG_IGN);
	ssize_t w = write(in[1], line, n);
	(void)w;
	close(in[1]);
	explicit_bzero(line, sizeof line);
	explicit_bzero(l->pw, sizeof l->pw);
	fcntl(out[0], F_SETFL, O_NONBLOCK);
	l->pid = pid;
	l->rfd = out[0];
	l->busy = 1;
	l->msg[0] = l->msg2[0] = 0;
	fl_fd_add(l->app, out[0], POLLIN, check_ready, l);
	return;
fail:
	explicit_bzero(l->pw, sizeof l->pw);
	snprintf(l->msg, sizeof l->msg, "The password could not be checked.");
	l->msg_err = 1;
}

/* ---- the clock --------------------------------------------------------------------- */

static void clock_arm(struct L *l)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long ms = (60 - ts.tv_sec % 60) * 1000L - ts.tv_nsec / 1000000L + 30;
	fl_timer_arm(l->clock, (int)ms, 0);
}

static void clock_tick(void *ud)
{
	struct L *l = ud;
	clock_arm(l);
	fl_win_redraw(l->win);
}

/* ---- drawing ----------------------------------------------------------------------- */

/* A password field: squares for dots, no clipboard, no selection. 1 on Enter. */
static int pw_field(fl_ui *ui, struct L *l, fl_rect r)
{
	int res = fl_widget(ui, ID_PW, r, FL_FOCUSABLE | FL_NO_FOCUS_RING);
	int focused = (res & FL_FOCUS) && !l->busy, enter = 0;
	size_t len = strlen(l->pw);
	if (focused && !l->demo) {
		if (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_KP_Enter) {
			enter = 1;
			ui->key = 0;
		} else if (ui->key == XKB_KEY_BackSpace) {
			while (len > 0 && ((unsigned char)l->pw[len - 1] & 0xc0) == 0x80)
				l->pw[--len] = 0;
			if (len > 0)
				l->pw[--len] = 0;
			ui->key = 0;
			l->msg[0] = l->msg2[0] = 0;
		} else if (ui->key == XKB_KEY_Escape || ((ui->mods & FL_MOD_CTRL) && (ui->key == 'u' || ui->key == 'U'))) {
			explicit_bzero(l->pw, sizeof l->pw);
			len = 0;
			ui->key = 0;
		}
		size_t n = strlen(ui->text);
		if (n && len + n < sizeof l->pw && !(ui->mods & FL_MOD_CTRL)) {
			memcpy(l->pw + len, ui->text, n + 1);
			len += n;
			l->msg[0] = l->msg2[0] = 0;
		}
	}
	explicit_bzero(ui->text, sizeof ui->text);
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, r, fl_th.panel);
	fl_stroke_rect(cv, r, 1, focused ? fl_th.accent : fl_th.line);
	int x = r.x + 16, cy = r.y + r.h / 2;
	int dots = 0;
	for (size_t i = 0; i < len; i++)
		if (((unsigned char)l->pw[i] & 0xc0) != 0x80)
			dots++;
	if (l->demo)
		dots = l->typed;
	if (dots) {
		int maxd = (r.w - 32) / 14;
		int first = dots > maxd ? dots - maxd : 0;
		for (int i = first; i < dots; i++)
			fl_fill_rect(cv, fl_r(x + (i - first) * 14, cy - 3, 7, 7), l->busy ? fl_th.dim : fl_th.text);
		x += (dots - first) * 14;
	} else if (!l->busy) {
		fl_text_in(cv, l->f_small, fl_r(r.x + 24, r.y, r.w - 40, r.h), FL_LEFT, 0, fl_th.dim, "Password");
	}
	if (focused || (l->demo && !l->busy)) {
		int cx = dots ? x : r.x + 16;
		fl_fill_rect(cv, fl_r(cx, cy - 9, 1, 18), fl_th.text);
	}
	return enter;
}

static void draw(fl_ui *ui, void *ud)
{
	struct L *l = ud;
	fl_canvas *cv = &ui->cv;
	int W = cv->w, H = cv->h;
	fl_fill_rect(cv, fl_r(0, 0, W, H), fl_th.bg);
	if (!ui->focus)
		ui->focus = ID_PW; /* whatever is typed goes to the password */

	char hm[16], date[64];
	time_t t = time(NULL);
	struct tm tm;
	if (l->demo) {
		memset(&tm, 0, sizeof tm);
		tm.tm_year = 126;
		tm.tm_mon = 10;
		tm.tm_mday = 10;
		tm.tm_hour = 14;
		tm.tm_min = 32;
		tm.tm_wday = 2;
		tm.tm_yday = 313;
	} else {
		localtime_r(&t, &tm);
	}
	strftime(hm, sizeof hm, "%H:%M", &tm);
	strftime(date, sizeof date, "%A %-d %B", &tm);

	/* one column, a little above the middle */
	int fh = fl_font_height(l->f_clock), dh = fl_font_height(l->f_date);
	int FW = 360, FH = 48;
	int block = fh + 4 + dh + 36 + 4 + 40 + 22 + FH + 14 + 20 + 20;
	int y = (H - block) * 2 / 5;
	if (y < 12)
		y = 12;
	int cw = fl_text_width(l->f_clock, hm, -1);
	fl_text(cv, l->f_clock, (W - cw) / 2, y, fl_th.strong, hm, -1);
	y += fh + 4;
	int dw = fl_text_width(l->f_date, date, -1);
	fl_text(cv, l->f_date, (W - dw) / 2, y, fl_th.dim, date, -1);
	y += dh + 36;
	fl_fill_rect(cv, fl_r((W - 48) / 2, y, 48, 4), fl_th.accent);
	y += 4 + 40;

	char who[160];
	snprintf(who, sizeof who, "%s", l->label);
	int ww = fl_text_width(l->f_name, who, -1);
	fl_text(cv, l->f_name, (W - ww) / 2, y, fl_th.text, who, -1);
	y += fl_font_height(l->f_name) + 22 - 4;

	int enter = pw_field(ui, l, fl_r((W - FW) / 2, y, FW, FH));
	y += FH + 14;
	const char *line1 = l->busy ? "Checking..." : l->msg[0] ? l->msg : "Enter unlocks";
	fl_color c1 = l->busy ? fl_th.dim : l->msg[0] ? fl_th.err : fl_th.dim;
	int w1 = fl_text_width(l->f_small, line1, -1);
	fl_text(cv, l->f_small, (W - w1) / 2, y, c1, line1, -1);
	if (l->msg2[0]) {
		int w2 = fl_text_width(l->f_small, l->msg2, -1);
		fl_text(cv, l->f_small, (W - w2) / 2, y + 20, fl_th.dim, l->msg2, -1);
	}

	/* the corners: since when, and the system */
	char since[64];
	if (l->demo) {
		snprintf(since, sizeof since, "Locked at 14:20");
	} else {
		struct tm lt;
		time_t la = l->locked_at;
		localtime_r(&la, &lt);
		strftime(since, sizeof since, "Locked at %H:%M", &lt);
	}
	fl_text(cv, l->f_small, 24, H - 24 - fl_font_height(l->f_small), fl_th.dim, since, -1);
	const char *os = "FreeLFS 0.1";
	fl_text(cv, l->f_small, W - 24 - fl_text_width(l->f_small, os, -1), H - 24 - fl_font_height(l->f_small),
	        fl_th.dim, os, -1);

	if (enter && !l->busy && !l->demo)
		check_start(l);
}

/* ---- main -------------------------------------------------------------------------- */

static void on_global(void *ud, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver)
{
	struct L *l = ud;
	if (!strcmp(iface, "fl_shell") && ver >= 4) {
		l->shell_ver = 4;
		l->shell = wl_registry_bind(reg, name, &fl_shell_interface, 4);
	}
}

static void usage(void)
{
	fputs("usage: fllock\n"
	      "       fllock --demo [--typed N] [--wrong|--busy|--stuck]   (screenshots; FLUI_PPM=FILE)\n",
	      stderr);
}

int main(int argc, char **argv)
{
	struct L l = {0};
	l.rfd = -1;
	const char *state = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--demo"))
			l.demo = 1;
		else if (!strcmp(argv[i], "--typed") && i + 1 < argc)
			l.typed = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--wrong") || !strcmp(argv[i], "--busy") || !strcmp(argv[i], "--stuck"))
			state = argv[i];
		else {
			usage();
			return !strcmp(argv[i], "-h") || !strcmp(argv[i], "--help") ? 0 : 2;
		}
	}
	signal(SIGPIPE, SIG_IGN);
	l.locked_at = (long)time(NULL);

	struct passwd *pw = getpwuid(getuid());
	snprintf(l.user, sizeof l.user, "%s", pw ? pw->pw_name : "?");
	snprintf(l.label, sizeof l.label, "%s", l.user);
	if (pw && pw->pw_gecos && pw->pw_gecos[0] && pw->pw_gecos[0] != ',')
		snprintf(l.label, sizeof l.label, "%.*s", (int)strcspn(pw->pw_gecos, ","), pw->pw_gecos);
	if (l.demo)
		snprintf(l.label, sizeof l.label, "Alex Example");

	l.app = fl_app_new(on_global, &l);
	if (!l.app) {
		fprintf(stderr, "fllock: cannot connect to a Wayland compositor\n");
		return 1;
	}
	if (!l.shell && !fl_app_ppm(l.app)) {
		fprintf(stderr, "fllock: the compositor has no fl_shell 4 (run it under flcomp)\n");
		return 1;
	}
	int ow, oh;
	fl_app_output_size(l.app, &ow, &oh);
	float k = (float)oh / 1080.0f;
	if (k < 0.5f)
		k = 0.5f;
	if (k > 1.5f)
		k = 1.5f;
	l.f_clock = fl_font_ui(300, 168.0f * k);
	if (!l.f_clock)
		l.f_clock = fl_font_ui(400, 168.0f * k);
	l.f_date = fl_font_ui(400, 24.0f * k > 18 ? 24.0f * k : 18);
	l.f_name = fl_font_ui(500, 18);
	l.f_small = fl_font_ui(400, 13);

	fl_win_opts o = {
		.title = "fllock", .app_id = "fllock", .w = ow, .h = oh, .raw = 1, .font_px = 14,
		.draw = draw, .ud = &l,
	};
	l.win = fl_win_new(l.app, &o);
	if (!l.win || !l.f_clock || !l.f_date || !l.f_name || !l.f_small)
		return 1;
	if (l.shell)
		fl_shell_lock(l.shell, fl_win_surface(l.win)); /* the screen is locked from here on */
	fl_win_ui(l.win)->focus = ID_PW;
	fl_win_map(l.win);
	if (l.demo && state) {
		if (!strcmp(state, "--wrong")) {
			snprintf(l.msg, sizeof l.msg, "Wrong password.");
		} else if (!strcmp(state, "--busy")) {
			l.busy = 1;
		} else {
			snprintf(l.msg, sizeof l.msg, "This account has no password that can unlock the screen.");
			snprintf(l.msg2, sizeof l.msg2, "Press Ctrl+Alt+F2, log in there and run: pkill flcomp");
		}
	}
	l.clock = fl_timer_new(l.app, clock_tick, &l);
	if (!l.demo)
		clock_arm(&l);
	fl_app_watch_conf(l.app, NULL, NULL);
	fl_app_run(l.app);
	fl_app_free(l.app);
	explicit_bzero(l.pw, sizeof l.pw);
	return 0;
}
