/* Print: Snappel (the screenshot tool) when it is installed: Print takes the whole screen, Shift+Print asks
 * for a region, Super+Shift+S opens the Snappel window (screenshot_key; $FLCOMP_SNAPPEL names another program).
 * Without Snappel Print saves the whole screen itself, as described next, and the other two keys do nothing.
 *
 * Save the whole screen as ~/pictures/screenshot-YYYY-MM-DD-HHMMSS.png.
 *
 * The windows are composited once more without the pointer and the on-screen
 * indicator (render.c: composite_clean), then flcomp forks: the child owns a frozen copy of
 * that picture and does the slow part (deflate, write the file) while the compositor goes
 * on drawing. The child reports on a pipe; when it says "saved" (or "failed") the
 * indicator appears (osd.c, the viewfinder icon with a green or red bar). Nothing is
 * kept in memory afterwards. At scales above 100 % the picture is the desktop at its
 * logical size, as the programs drew it.
 * SPDX-License-Identifier: MIT */
#include "comp.h"

#include <errno.h>
#include <png.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* in render.c: the desktop in C.shadow without pointer and indicator, then everything back */
void composite_clean(void);
void composite_restore(void);

static struct wl_event_source *pipe_src;

/* ~/pictures/screenshot-DATE.png, and the pictures directory if it is not there yet; the name
 * gets -2, -3 ... when the same second already has one. 0 or -1. */
static int make_path(char *out, size_t cap)
{
	const char *home = getenv("HOME");
	if (!home || !*home)
		return -1;
	char dir[512];
	snprintf(dir, sizeof dir, "%s/pictures", home);
	if (mkdir(dir, 0755) < 0 && errno != EEXIST)
		return -1;
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	char stamp[40];
	strftime(stamp, sizeof stamp, "%Y-%m-%d-%H%M%S", &tm);
	for (int n = 1; n < 100; n++) {
		if (n == 1)
			snprintf(out, cap, "%s/screenshot-%s.png", dir, stamp);
		else
			snprintf(out, cap, "%s/screenshot-%s-%d.png", dir, stamp, n);
		if (access(out, F_OK) != 0)
			return 0;
	}
	return -1;
}

/* the child: write the PNG from C.shadow (XRGB, w x h); exit status 0 when it worked */
static int write_png(const char *path)
{
	char tmp[600];
	snprintf(tmp, sizeof tmp, "%s.part", path);
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return -1;
	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_infop info = png ? png_create_info_struct(png) : NULL;
	unsigned char *row = malloc((size_t)C.w * 3);
	if (!png || !info || !row || setjmp(png_jmpbuf(png))) {
		png_destroy_write_struct(&png, &info);
		free(row);
		fclose(f);
		unlink(tmp);
		return -1;
	}
	png_init_io(png, f);
	png_set_compression_level(png, 3);	/* fast: a screen is mostly flat colour */
	png_set_IHDR(png, info, (png_uint_32)C.w, (png_uint_32)C.h, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
		     PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png, info);
	for (int y = 0; y < C.h; y++) {
		const uint32_t *p = C.shadow + (size_t)y * C.w;
		for (int x = 0; x < C.w; x++) {
			row[x * 3] = (p[x] >> 16) & 0xff;
			row[x * 3 + 1] = (p[x] >> 8) & 0xff;
			row[x * 3 + 2] = p[x] & 0xff;
		}
		png_write_row(png, row);
	}
	png_write_end(png, info);
	png_destroy_write_struct(&png, &info);
	free(row);
	if (fclose(f) != 0 || rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

static int result_cb(int fd, uint32_t mask, void *data)
{
	char b = 0;
	ssize_t n = read(fd, &b, 1);
	if (n < 0 && errno == EAGAIN)
		return 0;
	osd_show(OSD_SHOT, 100, n != 1 || b != 'k');
	wl_event_source_remove(pipe_src);
	pipe_src = NULL;
	close(fd);
	return 0;
}

/* the program on $PATH (or the path itself), or NULL */
static const char *find_exe(const char *prog, char *buf, size_t cap)
{
	if (strchr(prog, '/'))
		return access(prog, X_OK) == 0 ? prog : NULL;
	const char *path = getenv("PATH");
	while (path && *path) {
		size_t n = strcspn(path, ":");
		snprintf(buf, cap, "%.*s/%s", (int)n, path, prog);
		if (access(buf, X_OK) == 0)
			return buf;
		path += n + (path[n] ? 1 : 0);
	}
	return NULL;
}

void screenshot_key(int mode)
{
	const char *prog = getenv("FLCOMP_SNAPPEL");
	char buf[600];
	if (!prog || !*prog)
		prog = "snappel";
	const char *exe = find_exe(prog, buf, sizeof buf);
	if (!exe) {
		if (mode == SHOT_FULL)
			screenshot_take();
		return;
	}
	static const char *const arg[3] = { "--full", "--region", NULL };
	pid_t pid = fork();
	if (pid != 0)
		return;
	sigset_t none;
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	signal(SIGPIPE, SIG_DFL);
	execl(exe, "snappel", arg[mode], (char *)NULL);
	_exit(127);
}

void screenshot_take(void)
{
	if (pipe_src || !C.shadow)	/* one at a time (the key repeats) */
		return;
	char path[600];
	int fds[2];
	if (make_path(path, sizeof path) < 0 || pipe(fds) < 0) {
		fprintf(stderr, "flcomp: screenshot: no place to save it (HOME=%s)\n", getenv("HOME") ? getenv("HOME") : "");
		osd_show(OSD_SHOT, 100, true);
		return;
	}
	composite_clean();
	pid_t pid = fork();
	if (pid == 0) {
		close(fds[0]);
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		int ok = write_png(path) == 0;
		if (write(fds[1], ok ? "k" : "x", 1) < 0)
			_exit(2);
		_exit(ok ? 0 : 1);
	}
	composite_restore();
	close(fds[1]);
	if (pid < 0) {
		close(fds[0]);
		osd_show(OSD_SHOT, 100, true);
		return;
	}
	fprintf(stderr, "flcomp: screenshot %s\n", path);
	pipe_src = wl_event_loop_add_fd(C.loop, fds[0], WL_EVENT_READABLE, result_cb, NULL);
}
