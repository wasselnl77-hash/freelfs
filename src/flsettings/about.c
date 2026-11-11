/* about.c -- the About page of flsettings
 * SPDX-License-Identifier: MIT
 *
 * The system in a few lines (release, kernel, processor, memory, machine, graphics) and one button,
 * "Save a report": it runs `flreport -q` (src/flreport) in the background, which collects logs,
 * kernel messages and device lists into ~/freelfs-report-DATE.tar.gz. $FLREPORT names another
 * program (tests).
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <flui.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "about.h"
#include "pages.h"

#define ROW 56
#define MAXINFO 10
enum { ID_A_REPORT = 800 };

struct about {
	fl_app *app;
	fl_font *head, *small;
	fl_win *win;
	char label[MAXINFO][24], value[MAXINFO][160];
	int n;
	int live;
	/* the report job */
	pid_t pid;
	int fd;
	char out[600];
	size_t len;
	char msg[700];
	int msg_err;
};

static int read_line(const char *path, char *out, size_t cap)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(out, (int)cap, f))
		out[0] = 0;
	fclose(f);
	out[strcspn(out, "\r\n")] = 0;
	return 0;
}

static void add(struct about *a, const char *label, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void add(struct about *a, const char *label, const char *fmt, ...)
{
	if (a->n >= MAXINFO)
		return;
	va_list ap;
	va_start(ap, fmt);
	snprintf(a->label[a->n], sizeof a->label[0], "%s", label);
	vsnprintf(a->value[a->n], sizeof a->value[0], fmt, ap);
	va_end(ap);
	a->n++;
}

/* KEY="value" from /etc/os-release */
static int os_release(const char *key, char *out, size_t cap)
{
	FILE *f = fopen("/etc/os-release", "r");
	char line[256];
	size_t kl = strlen(key);
	int found = 0;
	while (f && fgets(line, sizeof line, f))
		if (!strncmp(line, key, kl) && line[kl] == '=') {
			char *v = line + kl + 1;
			v[strcspn(v, "\r\n")] = 0;
			if (*v == '"') {
				v++;
				char *q = strrchr(v, '"');
				if (q)
					*q = 0;
			}
			snprintf(out, cap, "%s", v);
			found = 1;
			break;
		}
	if (f)
		fclose(f);
	return found ? 0 : -1;
}

static void collect(struct about *a)
{
	char b[200], c[200];
	a->n = 0;
	a->live = access("/run/live", F_OK) == 0;
	if (os_release("PRETTY_NAME", b, sizeof b) < 0 && os_release("NAME", b, sizeof b) < 0)
		snprintf(b, sizeof b, "FreeLFS");
	add(a, "System", "%s%s", b, a->live ? " (live, nothing is kept after shutdown)" : "");
	struct utsname u;
	if (uname(&u) == 0)
		add(a, "Kernel", "%s %s", u.sysname, u.release);
	FILE *f = fopen("/proc/cpuinfo", "r");
	char line[300];
	int cpus = 0;
	b[0] = 0;
	while (f && fgets(line, sizeof line, f)) {
		if (!strncmp(line, "model name", 10) && !b[0]) {
			char *p = strchr(line, ':');
			if (p) {
				p++;
				while (*p == ' ')
					p++;
				p[strcspn(p, "\r\n")] = 0;
				snprintf(b, sizeof b, "%s", p);
			}
		} else if (!strncmp(line, "processor", 9)) {
			cpus++;
		}
	}
	if (f)
		fclose(f);
	if (b[0])
		add(a, "Processor", "%s (%d threads)", b, cpus);
	f = fopen("/proc/meminfo", "r");
	unsigned long kb = 0;
	while (f && fgets(line, sizeof line, f))
		if (sscanf(line, "MemTotal: %lu kB", &kb) == 1)
			break;
	if (f)
		fclose(f);
	if (kb)
		add(a, "Memory", "%.1f GB", (double)kb / 1048576.0);
	b[0] = c[0] = 0;
	read_line("/sys/class/dmi/id/sys_vendor", b, sizeof b);
	read_line("/sys/class/dmi/id/product_name", c, sizeof c);
	if (b[0] || c[0])
		add(a, "Computer", "%s %s", b, c);
	/* graphics: the kernel driver and PCI id of each DRM card */
	DIR *d = opendir("/sys/class/drm");
	struct dirent *de;
	while (d && (de = readdir(d))) {
		if (strncmp(de->d_name, "card", 4) || strchr(de->d_name + 4, '-'))
			continue;
		char p[300], drv[160] = "", ven[32] = "", dev[32] = "";
		snprintf(p, sizeof p, "/sys/class/drm/%s/device/driver", de->d_name);
		ssize_t n = readlink(p, drv, sizeof drv - 1);
		if (n > 0) {
			drv[n] = 0;
			memmove(drv, strrchr(drv, '/') ? strrchr(drv, '/') + 1 : drv, strlen(strrchr(drv, '/') ? strrchr(drv, '/') + 1 : drv) + 1);
		}
		snprintf(p, sizeof p, "/sys/class/drm/%s/device/vendor", de->d_name);
		read_line(p, ven, sizeof ven);
		snprintf(p, sizeof p, "/sys/class/drm/%s/device/device", de->d_name);
		read_line(p, dev, sizeof dev);
		add(a, "Graphics", "%s (%s:%s)", drv[0] ? drv : "no driver", ven[0] ? ven + 2 : "?", dev[0] ? dev + 2 : "?");
		break;
	}
	if (d)
		closedir(d);
}

struct about *about_new(fl_app *app, fl_font *head, fl_font *small)
{
	struct about *a = calloc(1, sizeof *a);
	if (!a)
		return NULL;
	a->app = app;
	a->head = head;
	a->small = small;
	a->fd = -1;
	return a;
}

static void job_end(struct about *a)
{
	if (a->fd >= 0) {
		fl_fd_remove(a->app, a->fd);
		close(a->fd);
		a->fd = -1;
	}
	if (a->pid > 0) {
		int st = 0;
		waitpid(a->pid, &st, 0);
		a->pid = 0;
	}
}

void about_free(struct about *a)
{
	if (!a)
		return;
	if (a->pid > 0)
		kill(a->pid, SIGTERM);
	job_end(a);
	free(a);
}

void about_set_win(struct about *a, fl_win *win)
{
	a->win = win;
}

void about_show(struct about *a)
{
	collect(a);
}

static void report_ready(void *ud, int fd, short rev)
{
	struct about *a = ud;
	char tmp[256];
	for (;;) {
		ssize_t r = read(fd, tmp, sizeof tmp);
		if (r > 0) {
			size_t room = sizeof a->out - 1 - a->len;
			size_t n = (size_t)r < room ? (size_t)r : room;
			memcpy(a->out + a->len, tmp, n);
			a->len += n;
			continue;
		}
		if (r < 0 && (errno == EAGAIN || errno == EINTR))
			return; /* more to come */
		break;
	}
	a->out[a->len] = 0;
	/* the path is the last line */
	char *nl = a->out + strlen(a->out);
	while (nl > a->out && (nl[-1] == '\n' || nl[-1] == '\r'))
		*--nl = 0;
	char *last = strrchr(a->out, '\n');
	last = last ? last + 1 : a->out;
	int st = 0;
	pid_t pid = a->pid;
	job_end(a);
	(void)pid;
	(void)st;
	if (last[0] == '/' && access(last, R_OK) == 0) {
		snprintf(a->msg, sizeof a->msg, "Saved %s", last);
		a->msg_err = 0;
	} else {
		snprintf(a->msg, sizeof a->msg, "The report could not be made%s%s", last[0] ? ": " : ".", last);
		a->msg_err = 1;
	}
	if (a->win)
		fl_win_redraw(a->win);
}

static void report_start(struct about *a)
{
	const char *prog = getenv("FLREPORT");
	if (!prog || !*prog)
		prog = "flreport";
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) < 0) {
		snprintf(a->msg, sizeof a->msg, "Cannot start: %s", strerror(errno));
		a->msg_err = 1;
		return;
	}
	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		snprintf(a->msg, sizeof a->msg, "Cannot start: %s", strerror(errno));
		a->msg_err = 1;
		return;
	}
	if (pid == 0) {
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) {
			dup2(nul, 0);
			dup2(nul, 2);
		}
		dup2(fds[1], 1);
		signal(SIGCHLD, SIG_DFL);
		execlp(prog, prog, "-q", (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	a->fd = fds[0];
	a->pid = pid;
	a->len = 0;
	a->out[0] = 0;
	a->msg[0] = 0;
	fl_fd_add(a->app, a->fd, POLLIN, report_ready, a);
	if (a->win)
		fl_win_redraw(a->win);
}

int about_draw(struct about *a, fl_ui *ui, fl_rect c)
{
	int x = c.x + 28, w = c.w - 56, y = c.y + 24;

	pg_section(ui, a->head, x, y, w, "This computer");
	y += 34;
	for (int i = 0; i < a->n; i++) {
		char fit[160];
		fl_text(&ui->cv, ui->f_reg, x, y, fl_th.dim, a->label[i], -1);
		fl_text_fit(ui->f_reg, a->value[i], w - 130, fit, sizeof fit);
		fl_text(&ui->cv, ui->f_reg, x + 130, y, fl_th.text, fit, -1);
		y += 26;
	}
	y += 24;

	pg_section(ui, a->head, x, y, w, "Something does not work?");
	y += 28;
	pg_row_text(ui, a->small, x, y, w - 160, "Save a report",
	            "Logs and device info in one file in your home folder.");
	fl_rect b = fl_r(x + w - 140, y + 12, 140, 32);
	if (a->pid > 0) {
		fl_stroke_rect(&ui->cv, b, 1, fl_th.line);
		fl_text_in(&ui->cv, ui->f_reg, b, FL_CENTER, 0, fl_th.dim, "Collecting...");
	} else if (fl_button(ui, ID_A_REPORT, b, "Save a report", FL_BTN_ACCENT)) {
		report_start(a);
	}
	y += ROW;
	if (a->msg[0]) {
		char fit[300];
		fl_text_fit(a->small, a->msg, w, fit, sizeof fit);
		fl_text(&ui->cv, a->small, x, y, a->msg_err ? fl_th.err : fl_th.text, fit, -1);
		y += 20;
		if (!a->msg_err) {
			fl_text_fit(a->small, a->live ? "The home folder of the live system is in memory: copy the file to a USB stick "
			                                 "with Files before you shut down." : "Copy it to a USB stick with Files, or send it with the problem.",
			            w, fit, sizeof fit);
			fl_text(&ui->cv, a->small, x, y, fl_th.dim, fit, -1);
			y += 20;
		}
	} else if (a->pid > 0) {
		fl_text(&ui->cv, a->small, x, y, fl_th.dim, "This takes about ten seconds.", -1);
		y += 20;
	}
	return y + 24 - c.y;
}
