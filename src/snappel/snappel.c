/* snappel -- the FreeLFS screenshot tool
 * SPDX-License-Identifier: MIT
 *
 *   snappel                 the Snappel window: full screen, window, region, with an optional delay
 *   snappel --full          the whole screen, saved to ~/pictures/screenshots, with a notification
 *   snappel --region        pick an area (Print + Shift): the screen freezes under a selection
 *                           rectangle; Save, Copy, Edit or Esc
 *   snappel --window        click a window
 *     --delay N             wait N seconds first (the compositor leaves Snappel itself out)
 *     --edit                open the editor instead of saving at once
 *     --output FILE         write the PNG to FILE (no notification, no ~/pictures); exit 0 when it
 *                           is written, 1 on cancel or failure (the portal backend uses this)
 *     --demo WHAT           (tests, screenshots) start, editor, menu, region, window: a made-up
 *                           screen; with FLUI_PPM=FILE nothing needs a compositor
 *
 * The pictures come from the compositor's fl_screencopy (cap.c); the editor, the region overlay
 * and the start window are libflui windows. The editor keeps the original and a list of
 * annotations (img.c) so Undo is exact.
 */
#include <errno.h>
#include <fcntl.h>
#include <flui.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cap.h"
#include "img.h"

#define MAX_OPS 256
#define BAR_H 48

enum { M_GUI, M_FULL, M_REGION, M_WINDOW };
enum { T_SAVE, T_EDIT };
enum { R_IDLE, R_DRAG, R_DONE };
enum { TOOL_ARROW, TOOL_BOX, TOOL_TEXT, TOOL_HILITE, TOOL_BLUR, TOOL_PIXEL, TOOL_CROP, TOOL_COUNT };

static const char *const tool_name[TOOL_COUNT] = { "Arrow", "Box", "Text", "Highlight", "Blur", "Pixelate", "Crop" };
static const int tool_op[TOOL_COUNT] = { OP_ARROW, OP_BOX, OP_TEXT, OP_HILITE, OP_BLUR, OP_PIXEL, OP_CROP };
static const uint32_t palette[6] = { 0xe5484d, 0xf5c542, 0x3f9b63, 0x3e8bff, 0xffffff, 0x111111 };

enum { ID_TOOL = 10, ID_COL = 30, ID_WMINUS = 40, ID_WPLUS, ID_UNDO, ID_NEW, ID_OPEN, ID_COPY, ID_SAVE,
       ID_MODE = 60, ID_DELAY = 70, ID_CANCEL = 80, ID_MENU = 90, ID_OV = 120 };

struct app {
	fl_app *app;
	fl_win *main, *ov;
	int mode, then, delay, demo_ppm;
	const char *output;	/* --output FILE */
	int done;		/* a picture was written */
	fl_font *f_big, *f_small, *f_mono;
	/* the document */
	img_t base, cur, tmp;
	struct op ops[MAX_OPS];
	int nops;
	int tool, width;
	uint32_t color;
	int drag, texting;
	struct op pend;
	char status[300], saved[600];
	int dirty, menu_open, screen;	/* screen: 0 start window, 1 editor */
	/* countdown */
	fl_timer *timer;
	int count, pending_mode;
	/* the overlay */
	img_t frozen, dim;
	int ov_kind, rstate;		/* ov_kind: M_REGION or M_WINDOW */
	int rx1, ry1, rx2, ry2;
	struct capwin wins[48];
	int nwins;
	uint32_t hover_id;
};

static struct app A;

/* ---- small helpers ----------------------------------------------------------------------- */

static int on_path(const char *prog, char *out, size_t cap)
{
	if (strchr(prog, '/')) {
		snprintf(out, cap, "%s", prog);
		return access(prog, X_OK) == 0;
	}
	const char *path = getenv("PATH");
	while (path && *path) {
		size_t n = strcspn(path, ":");
		snprintf(out, cap, "%.*s/%s", (int)n, path, prog);
		if (access(out, X_OK) == 0)
			return 1;
		path += n + (path[n] ? 1 : 0);
	}
	return 0;
}

/* run a program detached; argv[0] is looked up on PATH */
static void spawn(char *const argv[])
{
	pid_t pid = fork();
	if (pid != 0) {
		if (pid > 0)
			waitpid(pid, NULL, 0);
		return;
	}
	setsid();
	if (fork() != 0)
		_exit(0);
	int nul = open("/dev/null", O_RDWR);
	if (nul >= 0) {
		dup2(nul, 0);
		dup2(nul, 1);
	}
	for (int i = 3; i < 256; i++)
		close(i);
	execvp(argv[0], argv);
	_exit(127);
}

static void set_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_status(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(A.status, sizeof A.status, fmt, ap);
	va_end(ap);
	if (A.main)
		fl_win_redraw(A.main);
}

static int mkdirs(const char *dir)
{
	char tmp[600];
	snprintf(tmp, sizeof tmp, "%s", dir);
	for (char *p = tmp + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	return mkdir(tmp, 0755) < 0 && errno != EEXIST ? -1 : 0;
}

/* ~/pictures/screenshots/snappel-YYYY-MM-DD-HHMMSS.png ("-2", "-3" when the second is taken) */
static int make_path(char *out, size_t cap)
{
	const char *home = getenv("HOME");
	if (!home || !*home)
		return -1;
	char dir[520];
	snprintf(dir, sizeof dir, "%s/pictures/screenshots", home);
	if (mkdirs(dir) < 0)
		return -1;
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	char stamp[40];
	strftime(stamp, sizeof stamp, "%Y-%m-%d-%H%M%S", &tm);
	for (int n = 1; n < 100; n++) {
		if (n == 1)
			snprintf(out, cap, "%s/snappel-%s.png", dir, stamp);
		else
			snprintf(out, cap, "%s/snappel-%s-%d.png", dir, stamp, n);
		if (access(out, F_OK) != 0)
			return 0;
	}
	return -1;
}

/* a card in the notification centre; a click on it opens the picture (flnotify waits for that) */
static void notify(const char *summary, const char *body, const char *file)
{
	char fn[600];
	if (A.output)	/* somebody asked for this file: they tell the user */
		return;
	if (!on_path("flnotify", fn, sizeof fn))
		return;
	int fds[2];
	if (pipe(fds) < 0)
		return;
	pid_t pid = fork();
	if (pid != 0) {
		close(fds[1]);
		close(fds[0]);
		if (pid > 0)
			waitpid(pid, NULL, 0);
		return;
	}
	setsid();
	if (fork() != 0)
		_exit(0);
	close(fds[0]);
	dup2(fds[1], 1);
	for (int i = 3; i < 256; i++)
		close(i);
	/* the waiting copy of flnotify lives as long as the card (8 s) */
	if (file) {
		int pp[2];
		if (pipe(pp) == 0 && fork() == 0) {
			close(pp[0]);
			dup2(pp[1], 1);
			execlp("flnotify", "flnotify", "-a", "Snappel", "-i", "snappel", "-t", "8000", "-A", "default=Open", "-w",
			       summary, body, (char *)NULL);
			_exit(127);
		} else if (pp[1] > 0) {
			close(pp[1]);
			char buf[128] = "";
			ssize_t n = read(pp[0], buf, sizeof buf - 1);
			(void)n;
			if (strstr(buf, "action default")) {
				char vw[600];
				if (on_path("flview", vw, sizeof vw))
					execlp("flview", "flview", file, (char *)NULL);
			}
		}
		_exit(0);
	}
	execlp("flnotify", "flnotify", "-a", "Snappel", "-i", "snappel", "-t", "5000", summary, body, (char *)NULL);
	_exit(127);
}

/* ---- the document ------------------------------------------------------------------------- */

static void doc_set(img_t im)
{
	img_free(&A.base);
	img_free(&A.cur);
	img_free(&A.tmp);
	A.base = im;
	A.cur = img_copy(&A.base);
	A.nops = 0;
	A.dirty = 1;
	A.saved[0] = 0;
	A.drag = A.texting = 0;
}

static void doc_rebuild(void)
{
	img_free(&A.cur);
	A.cur = img_copy(&A.base);
	for (int i = 0; i < A.nops; i++)
		op_apply(&A.cur, &A.ops[i]);
}

static void doc_push(const struct op *o)
{
	if (A.nops >= MAX_OPS) {
		set_status("Too many annotations: undo some first");
		return;
	}
	A.ops[A.nops++] = *o;
	op_apply(&A.cur, o);
	A.dirty = 1;
	A.saved[0] = 0;
}

static void doc_undo(void)
{
	if (A.nops <= 0)
		return;
	A.nops--;
	doc_rebuild();
	A.dirty = 1;
	A.saved[0] = 0;
	set_status("Undone");
}

/* save the picture (once per change); the path is in A.saved */
static int doc_save(void)
{
	if (A.saved[0] && !A.dirty)
		return 0;
	char path[600];
	if (A.output)
		snprintf(path, sizeof path, "%s", A.output);
	if ((!A.output && make_path(path, sizeof path) < 0) || img_save_png(&A.cur, path) < 0) {
		set_status("Could not save the picture (is HOME set, is there room in ~/pictures?)");
		return -1;
	}
	snprintf(A.saved, sizeof A.saved, "%s", path);
	A.dirty = 0;
	A.done = 1;
	return 0;
}

static const char *base_name(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

static void do_save(void)
{
	if (doc_save() == 0)
		set_status("Saved: %s", A.saved);
}

static void do_copy(void)
{
	if (clip_copy(&A.cur) == 0)
		set_status("Copied to the clipboard (%d x %d)", A.cur.w, A.cur.h);
	else
		set_status("Could not copy: no clipboard in this session");
}

/* ---- opening the picture in another program --------------------------------------------- */

struct opener { const char *label, *prog; };
static const struct opener openers[] = {
	{ "Image viewer", "flview" }, { "Files (show the folder)", "flfiles" }, { "Browser", "freebws" },
};

static int open_count(struct opener *out)
{
	int n = 0;
	char b[600];
	for (size_t i = 0; i < sizeof openers / sizeof *openers; i++)
		if (getenv("SNAPPEL_ALL_OPENERS") || on_path(openers[i].prog, b, sizeof b))	/* the variable: for screenshots */
			out[n++] = openers[i];
	return n;
}

static void open_in(const struct opener *o)
{
	if (doc_save() < 0)
		return;
	char arg[700];
	if (!strcmp(o->prog, "flfiles")) {
		snprintf(arg, sizeof arg, "%s", A.saved);
		char *sl = strrchr(arg, '/');
		if (sl)
			*sl = 0;
	} else if (!strcmp(o->prog, "freebws")) {
		snprintf(arg, sizeof arg, "file://%s", A.saved);
	} else {
		snprintf(arg, sizeof arg, "%s", A.saved);
	}
	char *argv[] = { (char *)o->prog, arg, NULL };
	spawn(argv);
	set_status("Opened in %s: %s", o->label, base_name(A.saved));
}

/* ---- drawing helpers -------------------------------------------------------------------- */

static void blit(fl_canvas *cv, int dx, int dy, const img_t *im)
{
	for (int y = 0; y < im->h; y++) {
		int py = dy + y;
		if (py < cv->clip.y || py >= cv->clip.y + cv->clip.h)
			continue;
		int x0 = dx < cv->clip.x ? cv->clip.x - dx : 0;
		int x1 = dx + im->w > cv->clip.x + cv->clip.w ? cv->clip.x + cv->clip.w - dx : im->w;
		if (x1 > x0)
			memcpy(cv->px + (size_t)py * cv->stride + dx + x0, im->px + (size_t)y * im->w + x0, (size_t)(x1 - x0) * 4);
	}
}

/* the picture scaled down by zoom (<= 1) into the canvas at (dx, dy): 2 x 2 samples a pixel */
static void blit_scaled(fl_canvas *cv, int dx, int dy, const img_t *im, double z)
{
	if (z >= 0.999) {
		blit(cv, dx, dy, im);
		return;
	}
	int dw = (int)(im->w * z), dh = (int)(im->h * z);
	for (int y = 0; y < dh; y++) {
		int py = dy + y;
		if (py < cv->clip.y || py >= cv->clip.y + cv->clip.h)
			continue;
		double sy0 = (y + 0.25) / z, sy1 = (y + 0.75) / z;
		int ya = (int)sy0, yb = (int)sy1;
		if (ya >= im->h) ya = im->h - 1;
		if (yb >= im->h) yb = im->h - 1;
		for (int x = 0; x < dw; x++) {
			int px = dx + x;
			if (px < cv->clip.x || px >= cv->clip.x + cv->clip.w)
				continue;
			int xa = (int)((x + 0.25) / z), xb = (int)((x + 0.75) / z);
			if (xa >= im->w) xa = im->w - 1;
			if (xb >= im->w) xb = im->w - 1;
			uint32_t p[4] = { im->px[(size_t)ya * im->w + xa], im->px[(size_t)ya * im->w + xb],
					  im->px[(size_t)yb * im->w + xa], im->px[(size_t)yb * im->w + xb] };
			unsigned r = 0, g = 0, b = 0;
			for (int k = 0; k < 4; k++) {
				r += (p[k] >> 16) & 255;
				g += (p[k] >> 8) & 255;
				b += p[k] & 255;
			}
			cv->px[(size_t)py * cv->stride + px] = 0xff000000u | (r / 4) << 16 | (g / 4) << 8 | b / 4;
		}
	}
}

static void make_dim(void)
{
	img_free(&A.dim);
	A.dim = img_copy(&A.frozen);
	for (size_t i = 0; A.dim.px && i < (size_t)A.dim.w * A.dim.h; i++) {
		uint32_t p = A.dim.px[i];
		A.dim.px[i] = 0xff000000u | (((p >> 16) & 255) * 11 / 20) << 16 | (((p >> 8) & 255) * 11 / 20) << 8 | ((p & 255) * 11 / 20);
	}
}

static fl_rect norm_rect(int x1, int y1, int x2, int y2)
{
	int a = x1 < x2 ? x1 : x2, b = y1 < y2 ? y1 : y2;
	return fl_r(a, b, (x1 < x2 ? x2 - x1 : x1 - x2), (y1 < y2 ? y2 - y1 : y1 - y2));
}

static void draw_hint(fl_ui *ui, const char *text)
{
	fl_canvas *cv = &ui->cv;
	int tw = fl_text_width(ui->f_med, text, -1);
	fl_rect r = fl_r((cv->w - tw - 40) / 2, 20, tw + 40, 36);
	fl_fill_rect(cv, r, fl_th.panel);
	fl_stroke_rect(cv, r, 1, fl_th.line);
	fl_text_in(cv, ui->f_med, r, FL_CENTER, 0, fl_th.text, text);
}

/* ---- the capture flow -------------------------------------------------------------------- */

static void ov_close(void)
{
	if (A.ov) {
		fl_win_destroy(A.ov);
		A.ov = NULL;
	}
	img_free(&A.frozen);
	img_free(&A.dim);
	if (!A.main)
		fl_app_quit(A.app);
}

static void editor_show(void);

/* a finished picture: save and tell, or open the editor */
static void captured(img_t im, int notify_it)
{
	if (A.then == T_EDIT || A.mode == M_GUI) {
		doc_set(im);
		editor_show();
		set_status("%d x %d  -  annotate, then Save or Copy", A.cur.w, A.cur.h);
		return;
	}
	doc_set(im);
	if (doc_save() == 0) {
		fprintf(stderr, "snappel: saved %s\n", A.saved);
		if (notify_it)
			notify("Screenshot saved", base_name(A.saved), A.saved);
	} else {
		notify("Screenshot failed", "Could not write to ~/pictures/screenshots", NULL);
	}
	if (A.main)
		return;
	fl_app_quit(A.app);
}

static void open_overlay(int kind);

static void run_capture(int mode)
{
	img_t im = { 0, 0, NULL };
	if (mode == M_FULL) {
		if (cap_output(&im) < 0) {
			fprintf(stderr, "snappel: no screen capture here (is this flcomp?)\n");
			set_status("The compositor cannot take a screenshot here");
			if (!A.main)
				fl_app_quit(A.app);
			return;
		}
		captured(im, 1);
		return;
	}
	if (cap_output(&A.frozen) < 0) {
		set_status("The compositor cannot take a screenshot here");
		if (!A.main)
			fl_app_quit(A.app);
		return;
	}
	make_dim();
	A.nwins = mode == M_WINDOW ? cap_windows(A.wins, 48) : 0;
	open_overlay(mode);
}

static void count_tick(void *ud)
{
	if (--A.count > 0) {
		if (A.main)
			fl_win_redraw(A.main);
		return;
	}
	fl_timer_arm(A.timer, 0, 0);
	if (A.main)
		fl_win_redraw(A.main);
	run_capture(A.pending_mode);
}

static void start_capture(int mode)
{
	if (A.delay > 0) {
		A.pending_mode = mode;
		A.count = A.delay;
		fl_timer_arm(A.timer, 1000, 1000);
		if (A.main)
			fl_win_redraw(A.main);
		return;
	}
	run_capture(mode);
}

/* ---- the overlay: a frozen screen, a selection or a window to click ---------------------------- */

static void ov_finish_region(int action)	/* 0 save, 1 copy, 2 edit */
{
	fl_rect r = norm_rect(A.rx1, A.ry1, A.rx2, A.ry2);
	img_t im = img_crop(&A.frozen, r.x, r.y, r.w, r.h);
	if (!im.px) {
		ov_close();
		return;
	}
	ov_close();
	if (action == 2) {
		A.then = T_EDIT;
		captured(im, 0);
		return;
	}
	doc_set(im);
	if (action == 0) {
		if (doc_save() == 0) {
			fprintf(stderr, "snappel: saved %s\n", A.saved);
			notify("Screenshot saved", base_name(A.saved), A.saved);
		}
	} else if (clip_copy(&A.cur) == 0) {
		notify("Screenshot copied", "The picture is on the clipboard", NULL);
	}
	if (!A.main)
		fl_app_quit(A.app);
}

static void ov_pick_window(uint32_t id)
{
	img_t im = { 0, 0, NULL };
	if (cap_window(id, &im) < 0) {
		ov_close();
		set_status("That window is gone");
		return;
	}
	ov_close();
	captured(im, 1);
}

static void ov_draw(fl_ui *ui, void *ud)
{
	fl_canvas *cv = &ui->cv;
	if (!A.frozen.px) {
		fl_fill_rect(cv, fl_r(0, 0, cv->w, cv->h), fl_th.bg);
		return;
	}
	fl_fill_rect(cv, fl_r(0, 0, cv->w, cv->h), 0xff000000u);
	blit(cv, 0, 0, &A.dim);
	int mx = ui->mx, my = ui->my;
	if (ui->key == XKB_KEY_Escape) {
		ui->key = 0;
		ov_close();
		return;
	}
	if (A.ov_kind == M_REGION) {
		fl_rect toolbar = fl_r(0, 0, 0, 0);
		if (A.rstate == R_DONE) {
			fl_rect s = norm_rect(A.rx1, A.ry1, A.rx2, A.ry2);
			int bw = 4 * 76 + 3 * 4 + 12, bh = 44;
			int bx = s.x + s.w - bw, by = s.y + s.h + 8;
			if (by + bh > cv->h)
				by = s.y - bh - 8;
			if (by < 0)
				by = s.y + s.h - bh - 8;
			if (bx < 4)
				bx = 4;
			if (bx + bw > cv->w)
				bx = cv->w - bw - 4;
			toolbar = fl_r(bx, by, bw, bh);
		}
		int in_tb = toolbar.w && fl_in(toolbar, mx, my);
		if ((ui->press & FL_BTN_L) && !in_tb) {
			A.rstate = R_DRAG;
			A.rx1 = A.rx2 = mx;
			A.ry1 = A.ry2 = my;
		}
		if (A.rstate == R_DRAG) {
			A.rx2 = mx;
			A.ry2 = my;
			if (!(ui->btn_held & FL_BTN_L) || (ui->release & FL_BTN_L)) {
				fl_rect s = norm_rect(A.rx1, A.ry1, A.rx2, A.ry2);
				A.rstate = s.w >= 4 && s.h >= 4 ? R_DONE : R_IDLE;
			}
		}
		if (A.rstate != R_IDLE) {
			fl_rect s = norm_rect(A.rx1, A.ry1, A.rx2, A.ry2), c = fl_intersect(s, fl_r(0, 0, A.frozen.w, A.frozen.h));
			for (int y = c.y; y < c.y + c.h; y++)
				memcpy(cv->px + (size_t)y * cv->stride + c.x, A.frozen.px + (size_t)y * A.frozen.w + c.x, (size_t)c.w * 4);
			fl_stroke_rect(cv, fl_r(s.x - 1, s.y - 1, s.w + 2, s.h + 2), 1, fl_th.accent);
			char lab[40];
			snprintf(lab, sizeof lab, "%d x %d", s.w, s.h);
			int lw = fl_text_width(ui->f_med, lab, -1) + 16;
			fl_rect lr = fl_r(s.x - 1, s.y - 26 >= 0 ? s.y - 26 : s.y + 2, lw, 24);
			fl_fill_rect(cv, lr, fl_th.accent);
			fl_text_in(cv, ui->f_med, lr, FL_CENTER, 0, fl_th.on_accent, lab);
		}
		if (A.rstate == R_IDLE)
			draw_hint(ui, "Drag to select an area      Esc to cancel");
		else if (A.rstate == R_DONE) {
			fl_fill_rect(cv, toolbar, fl_th.panel);
			fl_stroke_rect(cv, toolbar, 1, fl_th.line);
			static const char *const lbl[4] = { "Save", "Copy", "Edit", "Cancel" };
			int act = -1;
			for (int i = 0; i < 4; i++)
				if (fl_button(ui, ID_OV + (uint32_t)i, fl_r(toolbar.x + 6 + i * 80, toolbar.y + 6, 76, 32), lbl[i],
					      i == 0 ? FL_BTN_ACCENT : FL_BTN_OUTLINE))
					act = i;
			if (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_KP_Enter) {
				ui->key = 0;
				act = 0;
			}
			if (ui->clicks >= 2 && (ui->press & FL_BTN_L) && fl_in(norm_rect(A.rx1, A.ry1, A.rx2, A.ry2), mx, my))
				act = 0;
			if (act == 3)
				ov_close();
			else if (act >= 0)
				ov_finish_region(act);
		}
	} else {
		/* a window: the topmost one under the pointer is lit */
		struct capwin *hit = NULL;
		for (int i = 0; i < A.nwins && !hit; i++)
			if (mx >= A.wins[i].x && my >= A.wins[i].y && mx < A.wins[i].x + A.wins[i].w && my < A.wins[i].y + A.wins[i].h)
				hit = &A.wins[i];
		if (hit) {
			fl_rect c = fl_intersect(fl_r(hit->x, hit->y, hit->w, hit->h), fl_r(0, 0, A.frozen.w, A.frozen.h));
			for (int y = c.y; y < c.y + c.h; y++)
				memcpy(cv->px + (size_t)y * cv->stride + c.x, A.frozen.px + (size_t)y * A.frozen.w + c.x, (size_t)c.w * 4);
			fl_stroke_rect(cv, fl_r(hit->x - 2, hit->y - 2, hit->w + 4, hit->h + 4), 2, fl_th.accent);
			char lab[200];
			snprintf(lab, sizeof lab, "%s  %d x %d", hit->title[0] ? hit->title : hit->app, hit->w, hit->h);
			int lw = fl_text_width(ui->f_med, lab, -1) + 20;
			fl_rect lr = fl_r(hit->x - 2, hit->y - 28 >= 0 ? hit->y - 28 : hit->y + 4, lw, 26);
			fl_fill_rect(cv, lr, fl_th.accent);
			fl_text_in(cv, ui->f_med, lr, FL_CENTER, 0, fl_th.on_accent, lab);
			if (ui->release & FL_BTN_L) {
				ov_pick_window(hit->id);
				return;
			}
		}
		draw_hint(ui, A.nwins ? "Click a window      Esc to cancel" : "No window to pick      Esc to cancel");
	}
}

static void ov_closed(fl_win *w, void *ud)
{
	ov_close();
}

static void open_overlay(int kind)
{
	A.ov_kind = kind;
	A.rstate = R_IDLE;
	fl_win_opts o = { .title = "Snappel", .app_id = "snappel-overlay", .w = A.frozen.w, .h = A.frozen.h,
			  .fullscreen = 1, .draw = ov_draw, .close = ov_closed, .ud = &A };
	A.ov = fl_win_new(A.app, &o);
	if (!A.ov) {
		img_free(&A.frozen);
		if (!A.main)
			fl_app_quit(A.app);
	}
}

/* ---- the editor and the start window -------------------------------------------------------- */

static void pt_in_image(fl_rect view, double z, int mx, int my, int *ix, int *iy)
{
	int x = (int)((mx - view.x) / z), y = (int)((my - view.y) / z);
	*ix = x < 0 ? 0 : x >= A.cur.w ? A.cur.w - 1 : x;
	*iy = y < 0 ? 0 : y >= A.cur.h ? A.cur.h - 1 : y;
}

static void pend_commit(void)
{
	struct op o = A.pend;
	A.drag = A.texting = 0;
	switch (o.kind) {
	case OP_TEXT:
		if (o.text[0])
			doc_push(&o);
		break;
	case OP_ARROW: {
		int dx = o.x2 - o.x1, dy = o.y2 - o.y1;
		if (dx * dx + dy * dy >= 36)
			doc_push(&o);
		break;
	}
	default:
		if (abs(o.x2 - o.x1) >= 3 && abs(o.y2 - o.y1) >= 3)
			doc_push(&o);
	}
	if (o.kind == OP_CROP && A.nops && A.ops[A.nops - 1].kind == OP_CROP)
		set_status("Cropped to %d x %d", A.cur.w, A.cur.h);
}

static void utf8_pop(char *s)
{
	size_t n = strlen(s);
	while (n > 0 && ((unsigned char)s[n - 1] & 0xc0) == 0x80)
		n--;
	if (n > 0)
		n--;
	s[n] = 0;
}

static void draw_canvas(fl_ui *ui, fl_rect area)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, area, fl_th.bg);
	if (!A.cur.px)
		return;
	fl_rect inner = fl_inset(area, 14, 14);
	double z = 1.0;
	if (A.cur.w > inner.w)
		z = (double)inner.w / A.cur.w;
	if (A.cur.h * z > inner.h)
		z = (double)inner.h / A.cur.h;
	int dw = (int)(A.cur.w * z), dh = (int)(A.cur.h * z);
	fl_rect view = fl_r(inner.x + (inner.w - dw) / 2, inner.y + (inner.h - dh) / 2, dw, dh);
	fl_rect old = cv->clip;
	fl_clip(cv, area);
	fl_stroke_rect(cv, fl_r(view.x - 1, view.y - 1, view.w + 2, view.h + 2), 1, fl_th.line);

	/* the pointer on the picture */
	int over = fl_in(view, ui->mx, ui->my) && !A.menu_open;
	if (over && A.tool == TOOL_TEXT)
		ui->cursor = FL_CURSOR_IBEAM;
	int ix, iy;
	pt_in_image(view, z, ui->mx, ui->my, &ix, &iy);
	if (A.texting) {
		int from = ui->text[0] && !(ui->mods & (FL_MOD_CTRL | FL_MOD_ALT)) ? 1 : 0;
		if (from && (unsigned char)ui->text[0] >= 32 && (unsigned char)ui->text[0] != 127 &&
		    strlen(A.pend.text) + strlen(ui->text) < sizeof A.pend.text)
			strcat(A.pend.text, ui->text);
		if (ui->key == XKB_KEY_BackSpace)
			utf8_pop(A.pend.text);
		else if (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_KP_Enter)
			pend_commit();
		else if (ui->key == XKB_KEY_Escape)
			A.texting = 0;
		ui->key = 0;
		ui->text[0] = 0;
	} else if (over && (ui->press & FL_BTN_L)) {
		A.pend = (struct op){ .kind = tool_op[A.tool], .x1 = ix, .y1 = iy, .x2 = ix, .y2 = iy,
				      .col = A.color, .width = A.width };
		if (A.tool == TOOL_TEXT)
			A.texting = 1;
		else
			A.drag = 1;
	}
	if (A.drag) {
		A.pend.x2 = ix;
		A.pend.y2 = iy;
		if (!(ui->btn_held & FL_BTN_L) || (ui->release & FL_BTN_L)) {
			pend_commit();
			set_status("%d x %d  -  %d annotation%s", A.cur.w, A.cur.h, A.nops, A.nops == 1 ? "" : "s");
		}
	}
	/* the picture, with what is being drawn applied to a copy */
	if ((A.drag && A.pend.kind != OP_CROP) || A.texting) {
		img_free(&A.tmp);
		A.tmp = img_copy(&A.cur);
		struct op p = A.pend;
		if (A.texting) {
			size_t n = strlen(p.text);
			if (n + 2 < sizeof p.text)
				strcat(p.text, "|");
		}
		if (A.tmp.px) {
			op_apply(&A.tmp, &p);
			blit_scaled(cv, view.x, view.y, &A.tmp, z);
		}
	} else {
		blit_scaled(cv, view.x, view.y, &A.cur, z);
	}
	if (A.drag && A.pend.kind == OP_CROP) {	/* the part that stays, the rest dimmed */
		fl_rect s = norm_rect(A.pend.x1, A.pend.y1, A.pend.x2, A.pend.y2);
		fl_rect sr = fl_r(view.x + (int)(s.x * z), view.y + (int)(s.y * z), (int)(s.w * z), (int)(s.h * z));
		fl_fill_rect(cv, fl_r(view.x, view.y, view.w, sr.y - view.y), 0xa0101418u);
		fl_fill_rect(cv, fl_r(view.x, sr.y + sr.h, view.w, view.y + view.h - sr.y - sr.h), 0xa0101418u);
		fl_fill_rect(cv, fl_r(view.x, sr.y, sr.x - view.x, sr.h), 0xa0101418u);
		fl_fill_rect(cv, fl_r(sr.x + sr.w, sr.y, view.x + view.w - sr.x - sr.w, sr.h), 0xa0101418u);
		fl_stroke_rect(cv, sr, 1, fl_th.accent);
	}
	if (A.drag && (A.pend.kind == OP_BLUR || A.pend.kind == OP_PIXEL || A.pend.kind == OP_CROP)) {
		fl_rect s = norm_rect(A.pend.x1, A.pend.y1, A.pend.x2, A.pend.y2);
		fl_stroke_rect(cv, fl_r(view.x + (int)(s.x * z), view.y + (int)(s.y * z), (int)(s.w * z), (int)(s.h * z)), 1, fl_th.accent);
	}
	cv->clip = old;
}

static void draw_menu(fl_ui *ui, fl_rect anchor)
{
	struct opener list[8];
	int n = open_count(list);
	fl_canvas *cv = &ui->cv;
	int h = (n ? n : 1) * 34 + 8, w = 240;
	fl_rect m = fl_r(anchor.x + anchor.w - w, anchor.y - h - 6, w, h);
	fl_fill_rect(cv, m, fl_th.panel);
	fl_stroke_rect(cv, m, 1, fl_th.line);
	if (!n)
		fl_text_in(cv, ui->f_reg, fl_r(m.x, m.y + 4, m.w, 34), FL_LEFT, 12, fl_th.dim, "No program to open it in");
	for (int i = 0; i < n; i++) {
		fl_rect r = fl_r(m.x + 4, m.y + 4 + i * 34, m.w - 8, 34);
		int res = fl_widget(ui, ID_MENU + (uint32_t)i, r, 0);
		if (res & (FL_HOVER | FL_DOWN))
			fl_fill_rect(cv, r, fl_th.raised);
		fl_text_in(cv, ui->f_reg, r, FL_LEFT, 10, fl_th.text, list[i].label);
		if (res & FL_CLICK) {
			A.menu_open = 0;
			open_in(&list[i]);
		}
	}
	if ((ui->press & FL_BTN_L) && !fl_in(m, ui->mx, ui->my) && !fl_in(anchor, ui->mx, ui->my))
		A.menu_open = 0;
}

static void draw_editor(fl_ui *ui, fl_rect c)
{
	fl_canvas *cv = &ui->cv;
	fl_rect top = fl_r(c.x, c.y, c.w, BAR_H), bot = fl_r(c.x, c.y + c.h - BAR_H, c.w, BAR_H);
	fl_rect area = fl_r(c.x, c.y + BAR_H, c.w, c.h - 2 * BAR_H);
	/* keyboard first, so a shortcut and the picture see the same state */
	if (!A.texting && (ui->mods & FL_MOD_CTRL)) {
		if (ui->key == 'z' || ui->key == 'Z') { ui->key = 0; doc_undo(); }
		else if (ui->key == 'c' || ui->key == 'C') { ui->key = 0; do_copy(); }
		else if (ui->key == 's' || ui->key == 'S') { ui->key = 0; do_save(); }
	}
	draw_canvas(ui, area);
	fl_fill_rect(cv, top, fl_th.panel);
	fl_fill_rect(cv, fl_r(top.x, top.y + top.h - 1, top.w, 1), fl_th.line);
	fl_fill_rect(cv, bot, fl_th.panel);
	fl_fill_rect(cv, fl_r(bot.x, bot.y, bot.w, 1), fl_th.line);

	int x = top.x + 8;
	static const int tw[TOOL_COUNT] = { 66, 52, 54, 86, 54, 78, 56 };
	for (int i = 0; i < TOOL_COUNT; i++) {
		if (fl_button(ui, ID_TOOL + (uint32_t)i, fl_r(x, top.y + 8, tw[i], 32), tool_name[i],
			      i == A.tool ? FL_BTN_ACCENT : FL_BTN_PLAIN)) {
			if (A.texting)
				pend_commit();
			A.tool = i;
		}
		x += tw[i] + 4;
	}
	x += 12;
	fl_fill_rect(cv, fl_r(x - 8, top.y + 12, 1, 24), fl_th.line);
	for (int i = 0; i < 6; i++) {
		fl_rect r = fl_r(x, top.y + 12, 24, 24);
		int res = fl_widget(ui, ID_COL + (uint32_t)i, r, 0);
		fl_fill_rect(cv, r, FL_HEX(palette[i]));
		fl_stroke_rect(cv, r, A.color == palette[i] ? 2 : 1, A.color == palette[i] ? fl_th.accent : (res & FL_HOVER ? fl_th.text : fl_th.line));
		if (A.color == palette[i])
			fl_stroke_rect(cv, fl_inset(r, 2, 2), 1, fl_th.bg);
		if (res & FL_CLICK) {
			A.color = palette[i];
			if (A.texting)
				A.pend.col = A.color;
		}
		x += 28;
	}
	x += 8;
	fl_fill_rect(cv, fl_r(x - 8 + 4, top.y + 12, 1, 24), fl_th.line);
	x += 8;
	if (fl_button(ui, ID_WMINUS, fl_r(x, top.y + 8, 30, 32), "-", FL_BTN_PLAIN) && A.width > 1)
		A.width--;
	char wv[8];
	snprintf(wv, sizeof wv, "%d", A.width);
	fl_text_in(cv, ui->f_med, fl_r(x + 30, top.y + 8, 26, 32), FL_CENTER, 0, fl_th.text, wv);
	if (fl_button(ui, ID_WPLUS, fl_r(x + 56, top.y + 8, 30, 32), "+", FL_BTN_PLAIN) && A.width < 6)
		A.width++;
	if (A.texting)
		A.pend.width = A.width;
	x += 96;
	if (fl_button(ui, ID_UNDO, fl_r(top.x + top.w - 76, top.y + 8, 68, 32), "Undo", A.nops ? FL_BTN_OUTLINE : FL_BTN_PLAIN))
		doc_undo();

	/* the bottom bar: what happened, and what to do with the picture */
	fl_rect st = fl_r(bot.x + 14, bot.y, bot.w - 14 - 360, bot.h);
	char fit[320];
	fl_text_fit(ui->f_reg, A.status, st.w, fit, sizeof fit);
	fl_text_in(cv, ui->f_reg, st, FL_LEFT, 0, fl_th.dim, fit);
	fl_rect b_new = fl_r(bot.x + bot.w - 8 - 72, bot.y + 8, 72, 32), b_save = fl_r(b_new.x - 4 - 80, bot.y + 8, 80, 32),
		b_copy = fl_r(b_save.x - 4 - 72, bot.y + 8, 72, 32), b_open = fl_r(b_copy.x - 4 - 104, bot.y + 8, 104, 32);
	if (fl_button(ui, ID_NEW, b_new, "New", FL_BTN_OUTLINE)) {
		if (A.texting)
			A.texting = 0;
		A.screen = 0;
		A.menu_open = 0;
		A.status[0] = 0;
	}
	if (fl_button(ui, ID_SAVE, b_save, "Save", FL_BTN_ACCENT))
		do_save();
	if (fl_button(ui, ID_COPY, b_copy, "Copy", FL_BTN_OUTLINE))
		do_copy();
	if (fl_button(ui, ID_OPEN, b_open, "Open in", FL_BTN_OUTLINE))
		A.menu_open = !A.menu_open;
	if (A.menu_open)
		draw_menu(ui, b_open);
}

static void draw_start(fl_ui *ui, fl_rect c)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, c, fl_th.bg);
	int cx = c.x + c.w / 2, y = c.y + c.h / 2 - 170;
	fl_text_in(cv, A.f_big, fl_r(c.x, y, c.w, 44), FL_CENTER, 0, fl_th.strong, "Snappel");
	fl_text_in(cv, ui->f_reg, fl_r(c.x, y + 42, c.w, 24), FL_CENTER, 0, fl_th.dim, "Take a screenshot");
	static const char *const nm[3] = { "Full screen", "Window", "Region" };
	static const char *const cap[3] = { "The whole desktop", "Click a window", "Drag an area" };
	static const int md[3] = { M_FULL, M_WINDOW, M_REGION };
	int bw = 200, bh = 104, gap = 16, x0 = cx - (3 * bw + 2 * gap) / 2;
	int busy = A.count > 0;
	for (int i = 0; i < 3; i++) {
		fl_rect r = fl_r(x0 + i * (bw + gap), y + 96, bw, bh);
		int res = busy ? 0 : fl_widget(ui, ID_MODE + (uint32_t)i, r, FL_FOCUSABLE);
		fl_fill_rect(cv, r, (res & (FL_HOVER | FL_DOWN)) ? fl_th.raised : fl_th.panel);
		fl_stroke_rect(cv, r, 1, (res & FL_FOCUS) ? fl_th.accent : fl_th.line);
		/* a small picture of what it takes: the screen, a window on it, a dashed selection */
		fl_rect ic = fl_r(r.x + bw / 2 - 22, r.y + 14, 44, 30);
		fl_stroke_rect(cv, ic, 2, (res & FL_HOVER) ? fl_th.text : fl_th.dim);
		if (i == 1)
			fl_stroke_rect(cv, fl_r(ic.x + 9, ic.y + 7, 20, 14), 2, fl_th.accent);
		else if (i == 2)
			fl_fill_rect(cv, fl_r(ic.x + 10, ic.y + 8, 26, 14), fl_mix(fl_th.panel, fl_th.accent, 120));
		if (i == 2)
			fl_stroke_rect(cv, fl_r(ic.x + 10, ic.y + 8, 26, 14), 1, fl_th.accent);
		fl_text_in(cv, ui->f_semi, fl_r(r.x, r.y + 54, r.w, 22), FL_CENTER, 0, fl_th.text, nm[i]);
		fl_text_in(cv, ui->f_reg, fl_r(r.x, r.y + 74, r.w, 20), FL_CENTER, 0, fl_th.dim, cap[i]);
		if (res & FL_CLICK)
			start_capture(md[i]);
	}
	/* the delay */
	fl_text_in(cv, ui->f_reg, fl_r(x0, y + 224, 60, 32), FL_LEFT, 0, fl_th.dim, "Delay");
	static const char *const dl[4] = { "None", "3 s", "5 s", "10 s" };
	static const int dv[4] = { 0, 3, 5, 10 };
	for (int i = 0; i < 4; i++)
		if (fl_button(ui, ID_DELAY + (uint32_t)i, fl_r(x0 + 64 + i * 66, y + 224, 62, 32), dl[i],
			      A.delay == dv[i] ? FL_BTN_ACCENT : FL_BTN_OUTLINE) && !busy)
			A.delay = dv[i];
	if (busy) {
		char t[80];
		snprintf(t, sizeof t, "Capturing in %d ...", A.count);
		fl_text_in(cv, A.f_big, fl_r(c.x, y + 270, c.w, 44), FL_CENTER, 0, fl_th.accent, t);
		if (fl_button(ui, ID_CANCEL, fl_r(cx - 40, y + 322, 80, 32), "Cancel", FL_BTN_OUTLINE)) {
			A.count = 0;
			fl_timer_arm(A.timer, 0, 0);
		}
	} else if (A.status[0]) {
		fl_text_in(cv, ui->f_reg, fl_r(c.x, y + 276, c.w, 24), FL_CENTER, 0, fl_th.dim, A.status);
	}
	fl_text_in(cv, ui->f_reg, fl_r(c.x, c.y + c.h - 44, c.w, 28), FL_CENTER, 0, fl_th.dim,
		   "Print: whole screen      Shift+Print: region      Super+Shift+S: this window");
}

static void main_draw(fl_ui *ui, void *ud)
{
	fl_rect c = fl_win_content(ui->win);
	if (A.screen == 1 && A.cur.px)
		draw_editor(ui, c);
	else
		draw_start(ui, c);
}

static void main_closed(fl_win *w, void *ud)
{
	fl_app_quit(A.app);
}

static void editor_show(void)
{
	A.screen = 1;
	A.menu_open = 0;
	if (!A.main) {
		fl_win_opts o = { .title = "Snappel", .app_id = "snappel", .w = 1040, .h = 700, .min_w = 900, .min_h = 420,
				  .decorated = 1, .resizable = 1, .draw = main_draw, .close = main_closed, .ud = &A };
		A.main = fl_win_new(A.app, &o);
	}
	if (A.main)
		fl_win_redraw(A.main);
}

static void start_show(void)
{
	A.screen = 0;
	fl_win_opts o = { .title = "Snappel", .app_id = "snappel", .w = 1040, .h = 700, .min_w = 900, .min_h = 420,
			  .decorated = 1, .resizable = 1, .draw = main_draw, .close = main_closed, .ud = &A };
	A.main = fl_win_new(A.app, &o);
}

/* ---- a made-up screen, for the screenshots and the tests -------------------------------------- */

static void demo_screen(img_t *im, int w, int h)
{
	*im = img_new(w, h);
	fl_canvas cv = { im->px, w, h, w, { 0, 0, w, h } };
	fl_fill_rect(&cv, fl_r(0, 0, w, h), FL_HEX(0x1d2125));
	fl_fill_rect(&cv, fl_r(0, h - 44, w, 44), FL_HEX(0x272c33));
	fl_fill_rect(&cv, fl_r(0, h - 44, 52, 44), FL_HEX(0x3f9b63));
	fl_font *f = fl_font_ui(500, 13), *fm = fl_font_mono(400, 13);
	struct { int x, y, w, h; const char *title; } win[2] = { { w / 10, h / 10, w * 5 / 10, h * 6 / 10, "Files" },
								 { w * 4 / 10, h * 22 / 100, w * 5 / 10, h * 55 / 100, "Terminal" } };
	for (int i = 0; i < 2; i++) {
		fl_fill_rect(&cv, fl_r(win[i].x, win[i].y, win[i].w, win[i].h), FL_HEX(i ? 0x14181c : 0x22272d));
		fl_fill_rect(&cv, fl_r(win[i].x, win[i].y, win[i].w, 32), FL_HEX(0x272c33));
		fl_stroke_rect(&cv, fl_r(win[i].x, win[i].y, win[i].w, win[i].h), 1, FL_HEX(0x3a4048));
		fl_text(&cv, f, win[i].x + 12, win[i].y + 8, FL_HEX(0xd8dce0), win[i].title, -1);
	}
	const char *lines[] = { "$ ls ~/documents", "notes.txt  budget.ods  passwords.txt", "$ cat passwords.txt",
				"mail: hunter2    bank: correct-horse", "$ snappel --region", "saved ~/pictures/screenshots/" };
	for (int i = 0; i < 6; i++)
		fl_text(&cv, fm, win[1].x + 16, win[1].y + 48 + i * 20, FL_HEX(i % 2 ? 0x8a9199 : 0xd8dce0), lines[i], -1);
	for (int i = 0; i < 7; i++) {
		fl_fill_rect(&cv, fl_r(win[0].x + 16, win[0].y + 48 + i * 30, 18, 14), FL_HEX(0x3f9b63));
		fl_text(&cv, f, win[0].x + 44, win[0].y + 46 + i * 30, FL_HEX(0xd8dce0),
			(const char *[]){ "documents", "music", "pictures", "downloads", "notes.txt", "budget.ods", "passwords.txt" }[i], -1);
	}
}

static void demo_ops(int ox, int oy)	/* ox, oy: where the picture was cut out of the made-up screen */
{
	int w = 1280, h = 800;
	int tx = w * 4 / 10 - ox, ty = h * 22 / 100 - oy, fx = w / 10 - ox, fy = h / 10 - oy;
	struct op o = { 0 };
	o = (struct op){ .kind = OP_HILITE, .x1 = tx + 10, .y1 = ty + 86, .x2 = tx + 330, .y2 = ty + 106, .col = 0xf5c542, .width = 3 };
	doc_push(&o);
	o = (struct op){ .kind = OP_BLUR, .x1 = tx + 12, .y1 = ty + 108, .x2 = tx + 330, .y2 = ty + 128, .col = 0, .width = 3 };
	doc_push(&o);
	o = (struct op){ .kind = OP_BOX, .x1 = fx + 34, .y1 = fy + 222, .x2 = fx + 170, .y2 = fy + 250, .col = 0xe5484d, .width = 2 };
	doc_push(&o);
	o = (struct op){ .kind = OP_ARROW, .x1 = fx + 330, .y1 = fy + 360, .x2 = fx + 176, .y2 = fy + 254, .col = 0xe5484d, .width = 3 };
	doc_push(&o);
	o = (struct op){ .kind = OP_TEXT, .x1 = fx + 300, .y1 = fy + 366, .col = 0xe5484d, .width = 3 };
	snprintf(o.text, sizeof o.text, "this one");
	doc_push(&o);
}

/* ---- main ------------------------------------------------------------------------------------ */

static void usage(void)
{
	fprintf(stderr, "usage: snappel [--full | --region | --window] [--delay SECONDS] [--edit]\n"
			"  no option  the Snappel window\n"
			"  --full     the whole screen, saved to ~/pictures/screenshots with a notification\n"
			"  --region   select an area on the frozen screen\n"
			"  --window   click a window\n"
			"  --edit     open the editor with the picture instead of saving it at once\n");
}

static void sigchld_ignore(void) {}

int main(int argc, char **argv)
{
	const char *demo = NULL;
	A.mode = M_GUI;
	A.tool = TOOL_ARROW;
	A.color = palette[0];
	A.width = 3;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--full"))
			A.mode = M_FULL;
		else if (!strcmp(argv[i], "--region"))
			A.mode = M_REGION;
		else if (!strcmp(argv[i], "--window"))
			A.mode = M_WINDOW;
		else if (!strcmp(argv[i], "--edit"))
			A.then = T_EDIT;
		else if (!strcmp(argv[i], "--output") && i + 1 < argc)
			A.output = argv[++i];
		else if (!strcmp(argv[i], "--delay") && i + 1 < argc)
			A.delay = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--demo") && i + 1 < argc)
			demo = argv[++i];
		else {
			usage();
			return !strcmp(argv[i], "-h") || !strcmp(argv[i], "--help") ? 0 : 2;
		}
	}
	(void)sigchld_ignore;
	if (A.delay < 0 || A.delay > 60)
		A.delay = 0;

	/* the plain screenshot needs no window at all */
	if (A.output && A.mode == M_GUI) {
		usage();
		return 2;
	}
	if (A.mode == M_FULL && !A.then && !demo) {
		if (A.delay)
			sleep((unsigned)A.delay);
		img_t im;
		if (cap_output(&im) < 0) {
			fprintf(stderr, "snappel: no screen capture here (run it under flcomp)\n");
			return 1;
		}
		doc_set(im);
		if (doc_save() < 0) {
			notify("Screenshot failed", "Could not write to ~/pictures/screenshots", NULL);
			return 1;
		}
		fprintf(stderr, "snappel: saved %s\n", A.saved);
		notify("Screenshot saved", base_name(A.saved), A.saved);
		return 0;
	}

	A.app = fl_app_new(NULL, NULL);
	if (!A.app) {
		fprintf(stderr, "snappel: cannot connect to a Wayland compositor\n");
		return 1;
	}
	A.f_big = fl_font_ui(600, 30);
	A.f_small = fl_font_ui(500, 11);
	A.timer = fl_timer_new(A.app, count_tick, NULL);
	fl_app_watch_conf(A.app, NULL, NULL);

	if (demo) {
		int w = 1280, h = 800;
		img_t im;
		demo_screen(&im, w, h);
		if (!strcmp(demo, "start")) {
			start_show();
		} else if (!strcmp(demo, "editor") || !strcmp(demo, "menu")) {
			img_t part = img_crop(&im, 80, 60, 960, 600);
			img_free(&im);
			doc_set(part);
			demo_ops(80, 60);
			A.tool = TOOL_BOX;
			editor_show();
			A.menu_open = !strcmp(demo, "menu");
			set_status("%d x %d  -  %d annotations", A.cur.w, A.cur.h, A.nops);
		} else if (!strcmp(demo, "region") || !strcmp(demo, "window")) {
			A.frozen = im;
			make_dim();
			A.rx1 = 300; A.ry1 = 200; A.rx2 = 900; A.ry2 = 560;
			if (!strcmp(demo, "window")) {
				A.nwins = 2;
				A.wins[0] = (struct capwin){ 2, w * 4 / 10, h * 22 / 100, w * 5 / 10, h * 55 / 100, "flterm", "Terminal" };
				A.wins[1] = (struct capwin){ 1, w / 10, h / 10, w * 5 / 10, h * 6 / 10, "flfiles", "Files" };
			}
			open_overlay(!strcmp(demo, "window") ? M_WINDOW : M_REGION);
			A.rstate = R_DONE;
		} else {
			usage();
			return 2;
		}
		fl_app_run(A.app);
		fl_app_free(A.app);
		return 0;
	}
	if (A.mode == M_GUI) {
		start_show();
	} else {
		if (A.delay)
			sleep((unsigned)A.delay);
		run_capture(A.mode);
		if (!A.ov && !A.main) {
			fl_app_free(A.app);
			return A.output && !A.done ? 1 : 0;
		}
	}
	fl_app_run(A.app);
	fl_app_free(A.app);
	return A.output && !A.done ? 1 : 0;
}
