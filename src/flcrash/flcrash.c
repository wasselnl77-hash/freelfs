/* flcrash -- the "Something went wrong" screen
 * SPDX-License-Identifier: MIT
 *
 * Shown by /etc/profile.d/desktop.sh when flcomp ends in a way it should not
 * (killed by a signal, or a non-zero status). No compositor is left at that
 * point, so this program draws straight into a KMS dumb buffer (device access
 * through libseat, keys through libinput), using libflui's canvas, fonts and
 * paths for the picture. When there is no usable display it prints the same
 * screen as text on the console.
 *
 *   Enter  exit 0: the caller starts the desktop again
 *   R      restart the computer (flpower restart, else reboot)
 *   S      show the end of the session log (Up/Down/PgUp/PgDn, Esc or S back)
 *   Esc    exit 3: leave to the shell prompt
 *
 * flcrash --status N [--prog NAME] [--secs N] [--log FILE]
 * flcrash --demo [--status N] [--view log] [--size WxH]   made-up data;
 *         with FLCRASH_PPM=FILE the picture is written there and nothing else
 *         happens (screenshots), without it the demo is shown on the screen
 * flcrash --text     the console version even when a display is available
 *
 * flcrash --cant PATH...   the easter egg: "You can't do that." (see "the egg" below). Asked before
 *         someone deletes a vital system folder (flfiles, an interactive flsh). Exit status 0:
 *         go ahead (not a protected path, or "Continue"), 1: "Never mind".
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <flui.h>
#include <libinput.h>
#include <libseat.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <strings.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#define FIELD 0x1f3d2e /* the green field */
#define EXIT_SHELL 3
/* the big face above the headline; swap the string for another (":o", "x_x") */
#define EMOTICON ":/"
/* the headline above it, drawn narrow and stretched sideways by WIDE_X */
#define OHNO "oh no."
#define WIDE_X 2.8f

/* ---- what happened ------------------------------------------------------------------------ */

struct info {
	char prog[32];
	int status;
	long secs;	/* how long it ran, -1 unknown */
	int crashes;	/* crashes in the last minute, this one included (0: unknown) */
	char log[512];
	char code[40];     /* FL-SIGSEGV */
	char cause[40];    /* with FL-RESTART-LOOP: the code of the last crash */
	char reason[300];  /* the sentence on the screen */
	char status_txt[80];
	char os[64];
};

/* ---- the error codes ---------------------------------------------------------------------------
 * One table; docs/M7-DESIGN.md lists the same. A code is chosen from the exit status flcomp
 * left (a signal shows as 128 + number), how long it ran, how often the desktop crashed in the
 * last minute and what its log says. %d in a code or sentence is the signal or the status. */

struct def {
	const char *code;
	const char *text;	/* the plain sentence */
};

#define LOOP_MIN 3	/* this many crashes within a minute is a restart loop */
#define EARLY_SECS 5	/* a desktop that ends sooner than this did not start */

static const struct def D_SEGV = {"FL-SIGSEGV", "The display server crashed (a memory access bug, \"segmentation fault\")."};
static const struct def D_ABRT = {"FL-SIGABRT", "The display server stopped itself after an internal error."};
static const struct def D_BUS = {"FL-SIGBUS", "The display server crashed on a memory or graphics driver fault (bus error)."};
static const struct def D_ILL = {"FL-SIGILL", "The display server tried an instruction this processor cannot run."};
static const struct def D_FPE = {"FL-SIGFPE", "The display server crashed on an arithmetic error."};
static const struct def D_KILL = {"FL-SIGKILL", "The display server was killed, most likely by the kernel's out-of-memory killer because the computer ran out of memory."};
static const struct def D_HUP = {"FL-SIGHUP", "The display server lost its terminal and was told to hang up."};
static const struct def D_SIG = {"FL-SIG%d", "The display server was stopped by signal %d."};
static const struct def D_NOSEAT = {"FL-NO-SEAT", "The display server could not get access to the screen and keyboard. The seat service (seatd) is not running, or this account is not in the group seat."};
static const struct def D_NODISPLAY = {"FL-NO-DISPLAY", "No usable screen was found. The graphics driver did not start, or no display is connected."};
static const struct def D_NORUNTIME = {"FL-NO-RUNTIME", "The display server could not create its socket. The runtime folder (XDG_RUNTIME_DIR) is missing or not writable."};
static const struct def D_NOMEM = {"FL-NO-MEMORY", "The display server could not get enough memory to start."};
static const struct def D_START = {"FL-START-FAILED", "The desktop failed to start: it stopped within a few seconds. The log says why."};
static const struct def D_EXIT = {"FL-EXIT-%d", "The display server ended with an error (status %d)."};
static const struct def D_LOOP = {"FL-RESTART-LOOP", "The desktop crashed three times within a minute. Starting it again would most likely crash it again, so look at the log first or restart the computer."};

static int log_has(const char *needle);

static const struct def *pick_def(const struct info *in, int *num)
{
	int st = in->status, sig = st > 128 && st < 160 ? st - 128 : 0;
	*num = sig ? sig : st;
	if (sig) {
		switch (sig) {
		case SIGSEGV: return &D_SEGV;
		case SIGABRT: return &D_ABRT;
		case SIGBUS: return &D_BUS;
		case SIGILL: return &D_ILL;
		case SIGFPE: return &D_FPE;
		case SIGKILL: return &D_KILL;
		case SIGHUP: return &D_HUP;
		}
		return &D_SIG;
	}
	if (in->secs >= 0 && in->secs < EARLY_SECS) {
		/* it never got going: flcomp says why on stderr */
		if (log_has("cannot open a seat") || log_has("seat not enabled"))
			return &D_NOSEAT;
		if (log_has("no usable display"))
			return &D_NODISPLAY;
		if (log_has("cannot create the Wayland socket") || log_has("cannot create /"))
			return &D_NORUNTIME;
		if (log_has("out of memory"))
			return &D_NOMEM;
		return &D_START;
	}
	return &D_EXIT;
}

static void describe(struct info *in)
{
	int num;
	const struct def *d = pick_def(in, &num);
	int st = in->status;
	snprintf(in->status_txt, sizeof in->status_txt, "%d", st);
	if (st > 128 && st < 160) {
		const char *name = strsignal(st - 128);
		snprintf(in->status_txt, sizeof in->status_txt, "%d  (signal %d, %s)", st, st - 128, name ? name : "?");
	}
	snprintf(in->code, sizeof in->code, d->code, num);
	snprintf(in->reason, sizeof in->reason, d->text, num);
	in->cause[0] = 0;
	if (in->crashes >= LOOP_MIN) {
		snprintf(in->cause, sizeof in->cause, "%s", in->code);
		snprintf(in->code, sizeof in->code, "%s", D_LOOP.code);
		snprintf(in->reason, sizeof in->reason, "%s", D_LOOP.text);
	}
}

static void read_os(struct info *in)
{
	snprintf(in->os, sizeof in->os, "FreeLFS");
	FILE *f = fopen("/etc/os-release", "r");
	char line[160];
	while (f && fgets(line, sizeof line, f))
		if (!strncmp(line, "PRETTY_NAME=", 12)) {
			char *v = line + 12;
			v[strcspn(v, "\r\n")] = 0;
			if (*v == '"')
				v++;
			v[strcspn(v, "\"")] = 0;
			if (*v)
				snprintf(in->os, sizeof in->os, "%s", v);
		}
	if (f)
		fclose(f);
}

/* ---- the log ----------------------------------------------------------------------------- */

#define MAXLINES 300
static char *lines[MAXLINES];
static int nlines;

static int log_has(const char *needle)
{
	for (int i = 0; i < nlines; i++)
		if (strstr(lines[i], needle))
			return 1;
	return 0;
}

static void clean(char *s)
{
	for (; *s; s++)
		if ((unsigned char)*s < 32 && *s != '\t')
			*s = ' ';
}

static void load_log(const char *path)
{
	FILE *f = path && *path ? fopen(path, "r") : NULL;
	if (!f) {
		lines[nlines++] = strdup("(no log: nothing was written to it)");
		return;
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f), from = sz > 65536 ? sz - 65536 : 0;
	fseek(f, from, SEEK_SET);
	char buf[512];
	if (from)
		fgets(buf, sizeof buf, f); /* a cut line */
	while (fgets(buf, sizeof buf, f)) {
		buf[strcspn(buf, "\r\n")] = 0;
		clean(buf);
		if (nlines == MAXLINES) {
			free(lines[0]);
			memmove(lines, lines + 1, (MAXLINES - 1) * sizeof *lines);
			nlines--;
		}
		lines[nlines++] = strdup(buf);
	}
	fclose(f);
	if (!nlines)
		lines[nlines++] = strdup("(the log is empty)");
}

/* a copy that survives the next login: ~/.local/state/freelfs/last-crash.log */
static void save_copy(const struct info *in)
{
	const char *st = getenv("XDG_STATE_HOME"), *h = getenv("HOME");
	char dir[400], path[460];
	if (st && *st)
		snprintf(dir, sizeof dir, "%s/freelfs", st);
	else if (h && *h)
		snprintf(dir, sizeof dir, "%s/.local/state/freelfs", h);
	else
		return;
	char tmp[400];
	snprintf(tmp, sizeof tmp, "%s", dir);
	for (char *p = tmp + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			mkdir(tmp, 0755);
			*p = '/';
		}
	mkdir(tmp, 0755);
	snprintf(path, sizeof path, "%s/last-crash.log", dir);
	FILE *f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "%s  %s  status %s  %s\n\n", in->code, in->prog, in->status_txt, in->reason);
	for (int i = 0; i < nlines; i++)
		fprintf(f, "%s\n", lines[i]);
	fclose(f);
}

/* ---- the picture -------------------------------------------------------------------------- */

struct view {
	int W, H;
	uint32_t *px;
	fl_font *head, *body, *small, *mono, *monob, *key, *face, *wide;
	float s;
	int mode; /* 0 message, 1 log */
	float mul; /* 1, shrunk when the message does not fit */
	int scroll; /* log: lines up from the end */
	int focus; /* the egg: 0 Never mind, 1 Continue */
	fl_rect btn[2];
};

static int wrap(fl_font *f, const char *text, int w, char out[][160], int max)
{
	int n = 0;
	const char *p = text;
	while (*p && n < max) {
		int len = 0, last = -1;
		char line[160];
		while (p[len] && len < 150) {
			if (p[len] == ' ')
				last = len;
			int tw = fl_text_width(f, p, len + 1);
			if (tw > w && last > 0) {
				len = last;
				break;
			}
			len++;
		}
		if (!p[len])
			last = len;
		else if (p[len] != ' ')
			len = last > 0 ? last : len;
		snprintf(line, sizeof line, "%.*s", len, p);
		snprintf(out[n++], 160, "%s", line);
		p += len;
		while (*p == ' ')
			p++;
	}
	return n;
}

/* one bar of the mark: a rect in the 20 x 20 box, turned by deg around (cx, cy) */
static void bar(fl_path *p, float x, float y, float w, float h, float deg, float cx, float cy, float k,
                float ox, float oy)
{
	float a = deg * 3.14159265f / 180.0f, c = __builtin_cosf(a), s = __builtin_sinf(a);
	float pt[4][2] = {{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}};
	for (int i = 0; i < 4; i++) {
		float dx = pt[i][0] - cx, dy = pt[i][1] - cy;
		float rx = cx + dx * c - dy * s, ry = cy + dx * s + dy * c;
		if (i == 0)
			fl_path_move(p, ox + rx * k, oy + ry * k);
		else
			fl_path_line(p, ox + rx * k, oy + ry * k);
	}
	fl_path_close(p);
}

/* the mark with its middle bar knocked out of place: an outline stays where it was */
static void logo(fl_canvas *cv, int ox, int oy, float k)
{
	const fl_color white = FL_HEX(0xf0f2f4), ghost = fl_mix(FL_HEX(FIELD), white, 110);
	fl_path *p = fl_path_new();
	bar(p, 3, 3, 14, 3.4f, 6, 10, 10, k, ox, oy);
	bar(p, 3, 13.6f, 6, 3.4f, 6, 10, 10, k, ox, oy);
	fl_path_fill(cv, p, white);
	fl_path_free(p);
	/* where bar two belongs */
	p = fl_path_new();
	bar(p, 3, 8.3f, 10, 3.4f, 6, 10, 10, k, ox, oy);
	fl_path_stroke(cv, p, k * 0.28f, ghost);
	fl_path_free(p);
	/* and where it landed: pushed out to the right and tipped over */
	p = fl_path_new();
	bar(p, 14.2f, 11.4f, 10, 3.4f, 31, 19.2f, 13.1f, k, ox, oy);
	fl_path_fill(cv, p, white);
	fl_path_free(p);
}

/* "oh no." is drawn fx times taller than it is shown and then squashed vertically (a box
 * filter over fx source rows), so its width keeps the full sharpness of the glyph edges. f is
 * the font at fx times the size; the result is fl_font_height(f) / fx rows high. */
static int wide_height(fl_font *f, float fx)
{
	return (int)(fl_font_height(f) / fx);
}

static void wide_text(struct view *v, fl_font *f, int x, int y, fl_color c, const char *txt, float fx)
{
	int tw = fl_text_width(f, txt, -1) + 4, th = fl_font_height(f), oh = wide_height(f, fx);
	uint32_t *tmp = malloc((size_t)tw * th * 4);
	if (!tmp)
		return;
	for (int i = 0; i < tw * th; i++)
		tmp[i] = FL_HEX(FIELD);
	fl_canvas tc = {tmp, tw, th, tw, {0, 0, tw, th}};
	fl_text(&tc, f, 0, 0, c, txt, -1);
	for (int dy = 0; dy < oh && y + dy < v->H; dy++) {
		float s0 = dy * fx, s1 = s0 + fx;
		for (int dx = 0; dx < tw && x + dx < v->W; dx++) {
			float acc[3] = {0, 0, 0};
			for (int sy = (int)s0; sy < s1 && sy < th; sy++) {
				float wgt = (sy + 1 < s1 ? sy + 1 : s1) - (sy > s0 ? sy : s0);
				uint32_t p = tmp[sy * tw + dx];
				acc[0] += wgt * ((p >> 16) & 255);
				acc[1] += wgt * ((p >> 8) & 255);
				acc[2] += wgt * (p & 255);
			}
			v->px[(size_t)(y + dy) * v->W + x + dx] = 0xff000000u | (uint32_t)(acc[0] / fx + 0.5f) << 16 |
			                                          (uint32_t)(acc[1] / fx + 0.5f) << 8 | (uint32_t)(acc[2] / fx + 0.5f);
		}
	}
	free(tmp);
}

static void keycap(struct view *v, int x, int y, int h, const char *label, int wmin)
{
	fl_canvas cv = {v->px, v->W, v->H, v->W, {0, 0, v->W, v->H}};
	int tw = fl_text_width(v->key, label, -1), w = tw + (int)(28 * v->s);
	if (w < wmin)
		w = wmin;
	fl_rect r = fl_r(x, y, w, h);
	fl_fill_rect(&cv, r, fl_mix(FL_HEX(FIELD), FL_HEX(0xffffff), 26));
	fl_stroke_rect(&cv, r, 2, fl_mix(FL_HEX(FIELD), FL_HEX(0xffffff), 110));
	fl_text_in(&cv, v->key, r, FL_CENTER, 0, FL_HEX(0xf0f2f4), label);
}

/* returns 1 when the technical lines would run into the key hints: render() shrinks and retries */
static int draw_message(struct view *v, const struct info *in)
{
	fl_canvas cv = {v->px, v->W, v->H, v->W, {0, 0, v->W, v->H}};
	const fl_color white = FL_HEX(0xf0f2f4), soft = fl_mix(FL_HEX(FIELD), white, 205),
	               dim = fl_mix(FL_HEX(FIELD), white, 150);
	float s = v->s;
	fl_fill_rect(&cv, fl_r(0, 0, v->W, v->H), FL_HEX(FIELD));
	int mx = v->W * 9 / 100;
	if (mx < 48)
		mx = 48;
	int mt = (int)(v->H * 0.07f);
	wide_text(v, v->wide, mx - (int)(4 * s), mt, white, OHNO, WIDE_X);
	int y = mt + wide_height(v->wide, WIDE_X) + (int)(4 * s);
	fl_text(&cv, v->face, mx - (int)(4 * s), y, white, EMOTICON, -1);
	y += fl_font_height(v->face) + (int)(6 * s);

	fl_text(&cv, v->head, mx, y, white, "Something went wrong", -1);
	y += fl_font_height(v->head) + (int)(14 * s);

	char ln[6][160];
	int maxw = v->W * 62 / 100;
	char para[500];
	snprintf(para, sizeof para, "%s %s", in->reason,
	         in->secs >= EARLY_SECS ? "Programs that were open have been closed and unsaved work in them may be lost. Your files are safe."
	                                : "Your files are safe.");
	int n = wrap(v->body, para, maxw, ln, 6);
	for (int i = 0; i < n; i++) {
		fl_text(&cv, v->body, mx, y, soft, ln[i], -1);
		y += fl_font_height(v->body) + (int)(5 * s);
	}
	y += (int)(22 * s);

	struct hint {
		const char *key, *what;
	};
	static const struct hint normal[4] = {
		{"Enter", "Restart the desktop"}, {"R", "Restart the computer"},
		{"S", "Show what the system wrote down"}, {"Esc", "Go to the text console"},
	};
	/* a restart loop: the way out comes first, trying again last */
	static const struct hint loop[4] = {
		{"S", "Show what the system wrote down"}, {"R", "Restart the computer"},
		{"Esc", "Go to the text console"}, {"Enter", "Try the desktop once more"},
	};
	const struct hint *hints = in->cause[0] ? loop : normal;
	int kh = (int)(34 * s), kw = (int)(78 * s), gap = (int)(10 * s);
	for (int i = 0; i < 4; i++) {
		keycap(v, mx, y, kh, hints[i].key, kw);
		fl_text_in(&cv, v->body, fl_r(mx + kw + (int)(22 * s), y, v->W / 2, kh), FL_LEFT, 0, white, hints[i].what);
		y += kh + gap;
	}

	/* the technical lines, at the bottom */
	char dur[40] = "";
	if (in->secs >= 0) {
		long m = in->secs / 60;
		if (m >= 60)
			snprintf(dur, sizeof dur, "%ld h %ld min", m / 60, m % 60);
		else if (m >= 1)
			snprintf(dur, sizeof dur, "%ld min", m);
		else
			snprintf(dur, sizeof dur, "%ld s", in->secs);
	}
	const char *rows[6][2];
	int cnt = 0;
	rows[cnt][0] = "Code", rows[cnt++][1] = in->code;
	if (in->cause[0])
		rows[cnt][0] = "Last crash", rows[cnt++][1] = in->cause;
	rows[cnt][0] = "Program", rows[cnt++][1] = in->prog;
	rows[cnt][0] = "Status", rows[cnt++][1] = in->status_txt;
	if (dur[0])
		rows[cnt][0] = "Ran for", rows[cnt++][1] = dur;
	rows[cnt][0] = "Log", rows[cnt++][1] = in->log;
	int lh = fl_font_height(v->mono) + (int)(4 * s);
	int by = v->H - (int)(v->H * 0.08f) - cnt * lh;
	int vx = mx + (int)(130 * s);
	if (y > by - (int)(6 * s) && (v->mul ? v->mul : 1.0f) > 0.5f)
		return 1;
	for (int i = 0; i < cnt; i++) {
		fl_text(&cv, v->mono, mx, by + i * lh, dim, rows[i][0], -1);
		char fit[200];
		fl_text_fit(v->mono, rows[i][1], v->W - vx - mx, fit, sizeof fit);
		fl_text(&cv, i == 0 ? v->monob : v->mono, vx, by + i * lh, i == 0 ? white : soft, fit, -1);
	}
	/* the mark with its middle bar out of place, and the system's name, bottom right */
	int ow = fl_text_width(v->mono, in->os, -1), oy = v->H - (int)(v->H * 0.08f) - lh;
	float k = 4.2f * s;
	logo(&cv, v->W - mx - ow - (int)(2 * k), oy - (int)(22 * k) - (int)(10 * s), k);
	fl_text(&cv, v->mono, v->W - mx - ow, oy, dim, in->os, -1);
	return 0;
}

static void draw_log(struct view *v, const struct info *in)
{
	fl_canvas cv = {v->px, v->W, v->H, v->W, {0, 0, v->W, v->H}};
	float s = v->s;
	fl_fill_rect(&cv, fl_r(0, 0, v->W, v->H), fl_th.bg);
	int mx = (int)(40 * s), y = (int)(30 * s);
	fl_text(&cv, v->body, mx, y, fl_th.strong, "What the system wrote down", -1);
	char sub[600];
	snprintf(sub, sizeof sub, "%s  %s", in->code, in->log);
	fl_text(&cv, v->small, mx, y + fl_font_height(v->body) + 4, fl_th.dim, sub, -1);
	int top = y + fl_font_height(v->body) + fl_font_height(v->small) + (int)(22 * s);
	fl_fill_rect(&cv, fl_r(mx, top - (int)(10 * s), v->W - 2 * mx, 1), fl_th.line);
	int lh = fl_font_height(v->mono) + 2, foot = (int)(44 * s);
	int rows = (v->H - top - foot) / lh;
	if (rows < 1)
		rows = 1;
	int maxs = nlines > rows ? nlines - rows : 0;
	if (v->scroll > maxs)
		v->scroll = maxs;
	if (v->scroll < 0)
		v->scroll = 0;
	int first = nlines - rows - v->scroll;
	if (first < 0)
		first = 0;
	for (int i = 0; i < rows && first + i < nlines; i++) {
		char fit[300];
		fl_text_fit(v->mono, lines[first + i], v->W - 2 * mx, fit, sizeof fit);
		fl_text(&cv, v->mono, mx, top + i * lh, fl_th.text, fit, -1);
	}
	fl_fill_rect(&cv, fl_r(0, v->H - foot, v->W, foot), fl_th.panel);
	fl_text_in(&cv, v->small, fl_r(mx, v->H - foot, v->W - 2 * mx, foot), FL_LEFT, 0, fl_th.dim,
	           "Up, Down, Page Up, Page Down scroll     Esc or S  back");
	if (maxs) {
		char pos[40];
		snprintf(pos, sizeof pos, "lines %d-%d of %d", first + 1, first + rows > nlines ? nlines : first + rows, nlines);
		fl_text_in(&cv, v->small, fl_r(mx, v->H - foot, v->W - 2 * mx, foot), FL_RIGHT, 0, fl_th.dim, pos);
	}
}

static int open_fonts(struct view *v)
{
	float s = (float)v->H / 1000.0f;
	if (s < 0.62f)
		s = 0.62f;
	if (s > 1.8f)
		s = 1.8f;
	s *= v->mul ? v->mul : 1.0f;
	v->s = s;
	fl_font *old[8] = {v->head, v->body, v->small, v->mono, v->monob, v->key, v->face, v->wide};
	for (int i = 0; i < 8; i++)
		if (old[i])
			fl_font_close(old[i]);
	v->head = fl_font_ui(600, 56 * s);
	v->body = fl_font_ui(400, 22 * s);
	v->small = fl_font_ui(400, 14 * s);
	v->mono = fl_font_mono(400, 16 * s);
	v->monob = fl_font_mono(500, 16 * s);
	v->key = fl_font_ui(500, 18 * s);
	v->face = fl_font_ui(400, 112 * s);
	v->wide = fl_font_ui(600, 92 * s * WIDE_X); /* squashed again by wide_text */
	return v->wide && v->face && v->head && v->body && v->small && v->mono && v->monob && v->key ? 0 : -1;
}

static void render(struct view *v, const struct info *in)
{
	if (v->mode) {
		draw_log(v, in);
		return;
	}
	for (int i = 0; i < 8 && draw_message(v, in); i++) {
		v->mul = (v->mul ? v->mul : 1.0f) * 0.9f;
		open_fonts(v);
	}
}

static int write_ppm(const struct view *v, const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	fprintf(f, "P6\n%d %d\n255\n", v->W, v->H);
	for (int i = 0; i < v->W * v->H; i++) {
		unsigned char rgb[3] = {(v->px[i] >> 16) & 255, (v->px[i] >> 8) & 255, v->px[i] & 255};
		fwrite(rgb, 1, 3, f);
	}
	return fclose(f);
}

/* ---- what a key does ------------------------------------------------------------------------ */

enum { ACT_NONE, ACT_RESTART, ACT_REBOOT, ACT_SHELL, ACT_REDRAW };

static int on_key(struct view *v, int key)
{
	if (v->mode) {
		switch (key) {
		case KEY_ESC:
		case KEY_S:
		case KEY_ENTER:
			v->mode = 0;
			return ACT_REDRAW;
		case KEY_UP: v->scroll++; return ACT_REDRAW;
		case KEY_DOWN: v->scroll--; return ACT_REDRAW;
		case KEY_PAGEUP: v->scroll += 15; return ACT_REDRAW;
		case KEY_PAGEDOWN: v->scroll -= 15; return ACT_REDRAW;
		case KEY_HOME: v->scroll = 100000; return ACT_REDRAW;
		case KEY_END: v->scroll = 0; return ACT_REDRAW;
		}
		return ACT_NONE;
	}
	switch (key) {
	case KEY_ENTER:
	case KEY_KPENTER: return ACT_RESTART;
	case KEY_R: return ACT_REBOOT;
	case KEY_S: v->mode = 1; v->scroll = 0; return ACT_REDRAW;
	case KEY_ESC: return ACT_SHELL;
	}
	return ACT_NONE;
}

static int do_reboot(void)
{
	execl("/usr/bin/flpower", "flpower", "restart", (char *)NULL);
	execl("/usr/sbin/reboot", "reboot", (char *)NULL);
	return ACT_NONE; /* neither exists: stay on the screen */
}

static int finish(int act)
{
	switch (act) {
	case ACT_RESTART: return 0;
	case ACT_SHELL: return EXIT_SHELL;
	}
	return -1;
}

/* ---- KMS and libinput ------------------------------------------------------------------------ */

struct drm {
	struct libseat *seat;
	int active;
	int fd, dev_id, direct;
	uint32_t crtc, conn;
	drmModeModeInfo mode;
	drmModeCrtc *saved;
	uint32_t handle, fb, pitch;
	size_t size;
	uint8_t *map;
	struct libinput *li;
	struct { int fd, id; } opened[32];
	int ctrl, alt, vt_pending;
} D = {.fd = -1};

static int open_dev(const char *path, int *fd)
{
	if (D.seat) {
		int id = libseat_open_device(D.seat, path, fd);
		if (id < 0)
			return -1;
		for (size_t i = 0; i < sizeof D.opened / sizeof *D.opened; i++)
			if (!D.opened[i].fd) {
				D.opened[i].fd = *fd;
				D.opened[i].id = id;
				break;
			}
		return id;
	}
	*fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
	return *fd < 0 ? -1 : 0;
}

static void close_dev(int fd)
{
	if (D.seat)
		for (size_t i = 0; i < sizeof D.opened / sizeof *D.opened; i++)
			if (D.opened[i].fd == fd) {
				libseat_close_device(D.seat, D.opened[i].id);
				D.opened[i].fd = 0;
				break;
			}
	close(fd);
}

static void seat_enable(struct libseat *s, void *ud)
{
	D.active = 1;
}

static void seat_disable(struct libseat *s, void *ud)
{
	D.active = 0;
	if (D.li)
		libinput_suspend(D.li);
	libseat_disable_seat(s);
}

static const struct libseat_seat_listener seat_listener = {seat_enable, seat_disable};

static int pick_output(void)
{
	drmModeRes *res = drmModeGetResources(D.fd);
	if (!res)
		return -1;
	int ret = -1;
	for (int i = 0; i < res->count_connectors && ret < 0; i++) {
		drmModeConnector *cn = drmModeGetConnector(D.fd, res->connectors[i]);
		if (!cn)
			continue;
		if (cn->connection == DRM_MODE_CONNECTED && cn->count_modes > 0) {
			drmModeModeInfo *m = &cn->modes[0];
			for (int j = 0; j < cn->count_modes; j++)
				if (cn->modes[j].type & DRM_MODE_TYPE_PREFERRED) {
					m = &cn->modes[j];
					break;
				}
			uint32_t crtc = 0;
			drmModeEncoder *enc = cn->encoder_id ? drmModeGetEncoder(D.fd, cn->encoder_id) : NULL;
			if (enc && enc->crtc_id)
				crtc = enc->crtc_id;
			if (enc)
				drmModeFreeEncoder(enc);
			for (int e = 0; !crtc && e < cn->count_encoders; e++) {
				drmModeEncoder *en = drmModeGetEncoder(D.fd, cn->encoders[e]);
				if (!en)
					continue;
				for (int k = 0; k < res->count_crtcs; k++)
					if (en->possible_crtcs & (1u << k)) {
						crtc = res->crtcs[k];
						break;
					}
				drmModeFreeEncoder(en);
			}
			if (crtc) {
				D.conn = cn->connector_id;
				D.crtc = crtc;
				D.mode = *m;
				ret = 0;
			}
		}
		drmModeFreeConnector(cn);
	}
	drmModeFreeResources(res);
	return ret;
}

static int make_buffer(int w, int h)
{
	struct drm_mode_create_dumb cr = {.width = (uint32_t)w, .height = (uint32_t)h, .bpp = 32};
	if (drmIoctl(D.fd, DRM_IOCTL_MODE_CREATE_DUMB, &cr))
		return -1;
	D.handle = cr.handle;
	D.pitch = cr.pitch;
	D.size = cr.size;
	uint32_t handles[4] = {D.handle}, pitches[4] = {D.pitch}, offsets[4] = {0};
	if (drmModeAddFB2(D.fd, (uint32_t)w, (uint32_t)h, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &D.fb, 0) &&
	    drmModeAddFB(D.fd, (uint32_t)w, (uint32_t)h, 24, 32, D.pitch, D.handle, &D.fb))
		return -1;
	struct drm_mode_map_dumb mp = {.handle = D.handle};
	if (drmIoctl(D.fd, DRM_IOCTL_MODE_MAP_DUMB, &mp))
		return -1;
	void *m = mmap(NULL, D.size, PROT_READ | PROT_WRITE, MAP_SHARED, D.fd, (off_t)mp.offset);
	if (m == MAP_FAILED)
		return -1;
	D.map = m;
	memset(D.map, 0, D.size);
	return 0;
}

/* 0 when the screen is ours: seat (or the device itself), connector, buffer, mode set */
static int drm_open(struct view *v)
{
	D.seat = libseat_open_seat(&seat_listener, NULL);
	if (D.seat) {
		struct pollfd pf = {.fd = libseat_get_fd(D.seat), .events = POLLIN};
		while (!D.active) {
			if (libseat_dispatch(D.seat, 0) < 0 || D.active)
				break;
			if (poll(&pf, 1, 2000) <= 0)
				break;
		}
		if (!D.active) {
			libseat_close_seat(D.seat);
			D.seat = NULL;
		}
	}
	if (!D.seat)
		D.active = 1; /* no seat daemon: try the devices directly (root, or group video and input) */
	for (int i = 0; i < 8 && D.fd < 0; i++) {
		char path[40];
		snprintf(path, sizeof path, "/dev/dri/card%d", i);
		int fd;
		if (open_dev(path, &fd) < 0)
			continue;
		D.fd = fd;
		if (pick_output() == 0)
			break;
		close_dev(fd);
		D.fd = -1;
	}
	if (D.fd < 0)
		return -1;
	v->W = D.mode.hdisplay;
	v->H = D.mode.vdisplay;
	if (make_buffer(v->W, v->H))
		return -1;
	D.saved = drmModeGetCrtc(D.fd, D.crtc);
	if (drmModeSetCrtc(D.fd, D.crtc, D.fb, 0, 0, &D.conn, 1, &D.mode))
		return -1;
	return 0;
}

static void drm_show(const struct view *v)
{
	for (int y = 0; y < v->H; y++)
		memcpy(D.map + (size_t)y * D.pitch, v->px + (size_t)y * v->W, (size_t)v->W * 4);
}

static void drm_close(void)
{
	if (D.li)
		libinput_unref(D.li);
	D.li = NULL;
	if (D.fd >= 0 && D.active && D.saved && D.saved->mode_valid)
		drmModeSetCrtc(D.fd, D.saved->crtc_id, D.saved->buffer_id, D.saved->x, D.saved->y, &D.conn, 1,
		               &D.saved->mode);
	if (D.saved)
		drmModeFreeCrtc(D.saved);
	if (D.map)
		munmap(D.map, D.size);
	if (D.fb)
		drmModeRmFB(D.fd, D.fb);
	if (D.handle) {
		struct drm_mode_destroy_dumb d = {.handle = D.handle};
		drmIoctl(D.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
	}
	if (D.fd >= 0)
		close_dev(D.fd);
	if (D.seat)
		libseat_close_seat(D.seat);
}

static int li_open(const char *path, int flags, void *ud)
{
	int fd;
	if (open_dev(path, &fd) < 0)
		return -errno;
	return fd;
}

static void li_close(int fd, void *ud)
{
	close_dev(fd);
}

static const struct libinput_interface li_iface = {li_open, li_close};

static int input_open(void)
{
	D.li = libinput_path_create_context(&li_iface, NULL);
	int n = 0;
	for (int i = 0; D.li && i < 32; i++) {
		char path[64];
		snprintf(path, sizeof path, "/dev/input/event%d", i);
		struct libinput_device *d = libinput_path_add_device(D.li, path);
		if (d && libinput_device_has_capability(d, LIBINPUT_DEVICE_CAP_KEYBOARD))
			n++;
	}
	return n;
}

/* returns an exit code, or -1 to keep going */
static int drm_loop(struct view *v, const struct info *in)
{
	if (!input_open())
		fprintf(stderr, "flcrash: no keyboard found\n");
	render(v, in);
	drm_show(v);
	for (;;) {
		struct pollfd pf[2] = {{.fd = D.li ? libinput_get_fd(D.li) : -1, .events = POLLIN},
		                       {.fd = D.seat ? libseat_get_fd(D.seat) : -1, .events = POLLIN}};
		if (poll(pf, 2, -1) < 0 && errno != EINTR)
			return EXIT_SHELL;
		if (D.seat && (pf[1].revents & POLLIN)) {
			int was = D.active;
			libseat_dispatch(D.seat, 0);
			if (!was && D.active) {
				if (D.li)
					libinput_resume(D.li);
				drmModeSetCrtc(D.fd, D.crtc, D.fb, 0, 0, &D.conn, 1, &D.mode);
				drm_show(v);
			}
		}
		if (!D.li)
			continue;
		libinput_dispatch(D.li);
		struct libinput_event *ev;
		while ((ev = libinput_get_event(D.li))) {
			if (libinput_event_get_type(ev) == LIBINPUT_EVENT_KEYBOARD_KEY) {
				struct libinput_event_keyboard *ke = libinput_event_get_keyboard_event(ev);
				int key = (int)libinput_event_keyboard_get_key(ke),
				    down = libinput_event_keyboard_get_key_state(ke) == LIBINPUT_KEY_STATE_PRESSED;
				if (key == KEY_LEFTCTRL || key == KEY_RIGHTCTRL)
					D.ctrl = down;
				else if (key == KEY_LEFTALT || key == KEY_RIGHTALT)
					D.alt = down;
				else if (down && D.ctrl && D.alt && key >= KEY_F1 && key <= KEY_F10 && D.seat)
					libseat_switch_session(D.seat, key - KEY_F1 + 1);
				else if (down && !D.ctrl && !D.alt) {
					int act = on_key(v, key);
					if (act == ACT_REBOOT) {
						libinput_event_destroy(ev);
						drm_close();
						do_reboot();
						return EXIT_SHELL;
					}
					int code = finish(act);
					if (code >= 0) {
						libinput_event_destroy(ev);
						return code;
					}
					if (act == ACT_REDRAW) {
						render(v, in);
						if (D.active)
							drm_show(v);
					}
				}
			}
			libinput_event_destroy(ev);
		}
	}
}

/* ---- the console version ----------------------------------------------------------------------- */

static void text_screen(const struct info *in, int cols, int rows)
{
	const char *bg = "\033[48;2;31;61;46m", *fg = "\033[38;2;240;242;244m", *dim = "\033[38;2;170;200;182m";
	printf("\033[0m%s%s\033[2J\033[H", bg, fg);
	int top = rows > 24 ? 3 : 1, x = cols > 70 ? 6 : 2;
	char para[500];
	snprintf(para, sizeof para, "%s %s", in->reason,
	         in->secs >= EARLY_SECS ? "Programs that were open have been closed and unsaved work in them may be lost. Your files are safe."
	                                : "Your files are safe.");
	printf("\033[%d;%dH\033[1m%s", top, x, "oh no.   Something went wrong");
	printf("\033[0m%s%s", bg, fg);
	/* plain word wrap */
	int w = cols - 2 * x, row = top + 2, col = 0;
	printf("\033[%d;%dH", row, x);
	for (const char *p = para; *p;) {
		size_t n = strcspn(p, " ");
		if (col && col + 1 + (int)n > w) {
			row++;
			col = 0;
			printf("\033[%d;%dH", row, x);
		}
		printf("%s%.*s", col ? " " : "", (int)n, p);
		col += (col ? 1 : 0) + (int)n;
		p += n;
		while (*p == ' ')
			p++;
	}
	row += 2;
	static const char *normal[] = {"Enter  Restart the desktop", "R      Restart the computer",
	                               "S      Show what the system wrote down", "Esc    Go to the text console"};
	static const char *loop[] = {"S      Show what the system wrote down", "R      Restart the computer",
	                             "Esc    Go to the text console", "Enter  Try the desktop once more"};
	const char **hints = in->cause[0] ? loop : normal;
	for (int i = 0; i < 4; i++)
		printf("\033[%d;%dH%s", row + i, x, hints[i]);
	row += 5;
	printf("\033[%d;%dH%s", row, x, dim);
	printf("Code     %s", in->code);
	if (in->cause[0])
		printf("\033[%d;%dHLast     %s", ++row, x, in->cause);
	printf("\033[%d;%dHProgram  %s", row + 1, x, in->prog);
	printf("\033[%d;%dHStatus   %s", row + 2, x, in->status_txt);
	printf("\033[%d;%dHLog      %s", row + 3, x, in->log);
	printf("\033[0m");
	fflush(stdout);
}

static void text_log(int cols, int rows, int scroll)
{
	printf("\033[0m\033[2J\033[H\033[1mWhat the system wrote down\033[0m   (Up/Down scroll, Esc or S back)\n\n");
	int n = rows - 4;
	if (n < 1)
		n = 1;
	int first = nlines - n - scroll;
	if (first < 0)
		first = 0;
	for (int i = 0; i < n && first + i < nlines; i++)
		printf("%.*s\n", cols - 1, lines[first + i]);
	fflush(stdout);
}

static int text_loop(const struct info *in)
{
	struct termios old, raw;
	int tty = isatty(0);
	if (tty) {
		tcgetattr(0, &old);
		raw = old;
		cfmakeraw(&raw);
		raw.c_cc[VMIN] = 1;
		raw.c_cc[VTIME] = 0;
		tcsetattr(0, TCSANOW, &raw);
	}
	struct winsize ws = {0};
	ioctl(1, TIOCGWINSZ, &ws);
	int cols = ws.ws_col ? ws.ws_col : 80, rows = ws.ws_row ? ws.ws_row : 25;
	int mode = 0, scroll = 0, code = -1;
	text_screen(in, cols, rows);
	while (code < 0) {
		unsigned char c;
		if (read(0, &c, 1) <= 0) {
			code = EXIT_SHELL;
			break;
		}
		int key = 0;
		if (c == '\r' || c == '\n')
			key = KEY_ENTER;
		else if (c == 'r' || c == 'R')
			key = KEY_R;
		else if (c == 's' || c == 'S')
			key = KEY_S;
		else if (c == 3)
			key = KEY_ESC;
		else if (c == 27) {
			unsigned char seq[2];
			struct pollfd pf = {.fd = 0, .events = POLLIN};
			if (poll(&pf, 1, 40) > 0 && read(0, &seq[0], 1) == 1 && seq[0] == '[' && read(0, &seq[1], 1) == 1)
				key = seq[1] == 'A' ? KEY_UP : seq[1] == 'B' ? KEY_DOWN : seq[1] == '5' ? KEY_PAGEUP
				      : seq[1] == '6' ? KEY_PAGEDOWN : 0;
			else
				key = KEY_ESC;
			if (key == KEY_PAGEUP || key == KEY_PAGEDOWN) {
				unsigned char tilde;
				if (read(0, &tilde, 1) < 0)
					break;
			}
		}
		struct view v = {.mode = mode, .scroll = scroll};
		int act = on_key(&v, key);
		mode = v.mode;
		scroll = v.scroll;
		if (act == ACT_REBOOT) {
			if (tty)
				tcsetattr(0, TCSANOW, &old);
			printf("\033[0m\033[2J\033[H");
			fflush(stdout);
			do_reboot();
			code = EXIT_SHELL;
		} else if (finish(act) >= 0) {
			code = finish(act);
		} else if (act == ACT_REDRAW) {
			if (mode)
				text_log(cols, rows, scroll);
			else
				text_screen(in, cols, rows);
		}
	}
	if (tty)
		tcsetattr(0, TCSANOW, &old);
	printf("\033[0m\033[2J\033[H");
	fflush(stdout);
	return code;
}


/* ---- the egg: "You can't do that." ----------------------------------------------------------------
 * Someone tries to delete /sysflfs, /boot and the like. The crash screen's layout and green, the
 * stretched "oh no.", an angry face instead of ":/", and only one sentence. Two buttons: "Never
 * mind" (the default: Enter, Esc, N) and "Continue" (C, or Tab and Enter, or a click), which lets
 * the caller really delete it. On the desktop it is one full-screen window; on a console the same
 * thing in text; a program without either gets "Never mind". */

#define EGG_FACE ">:("
#define EGG_TEXT "You can't do that."

static const char *const PROTECTED[] = {
	"/", "/boot", "/sysflfs", "/sysflfs/bin", "/sysflfs/sbin", "/sysflfs/lib", "/sysflfs/libexec",
	"/sysflfs/share", "/sysflfs/settings", "/sysflfs/data",
	/* the classic names, hidden links into /sysflfs (and /usr, where the homes live) */
	"/usr", "/usr/bin", "/usr/sbin", "/usr/lib", "/usr/libexec", "/usr/share", "/bin", "/sbin", "/lib", "/lib64",
	"/etc", "/var", "/home", "/root", "/drives", "/mnt", "/media", "/opt", "/srv",
};

/* a path made absolute with "." and ".." and doubled or trailing slashes folded away (no links resolved) */
static void fold_path(const char *in, char *out, size_t cap)
{
	char tmp[PATH_MAX * 2];
	if (in[0] != '/') {
		if (!getcwd(tmp, sizeof tmp))
			tmp[0] = 0;
		strncat(tmp, "/", sizeof tmp - strlen(tmp) - 1);
		strncat(tmp, in, sizeof tmp - strlen(tmp) - 1);
	} else {
		snprintf(tmp, sizeof tmp, "%s", in);
	}
	size_t n = 0;
	out[0] = 0;
	for (char *p = tmp; *p;) {
		while (*p == '/')
			p++;
		char *e = strchr(p, '/');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		if (len == 0)
			break;
		if (len == 1 && p[0] == '.') {
		} else if (len == 2 && p[0] == '.' && p[1] == '.') {
			char *sl = strrchr(out, '/');
			n = sl ? (size_t)(sl - out) : 0;
			out[n] = 0;
		} else if (n + len + 2 < cap) {
			out[n++] = '/';
			memcpy(out + n, p, len);
			n += len;
			out[n] = 0;
		}
		p += len;
	}
	if (!out[0])
		snprintf(out, cap, "/");
}

static int is_protected_one(const char *p)
{
	for (size_t i = 0; i < sizeof PROTECTED / sizeof *PROTECTED; i++)
		if (!strcmp(p, PROTECTED[i]))
			return 1;
	/* FLCRASH_PROTECT=/a:/b adds paths (tests; nothing else needs it) */
	const char *extra = getenv("FLCRASH_PROTECT");
	while (extra && *extra) {
		size_t n = strcspn(extra, ":");
		if (n && n < PATH_MAX && !strncmp(p, extra, n) && !p[n])
			return 1;
		extra += n + (extra[n] ? 1 : 0);
	}
	return 0;
}

/* the name as typed, folded, or what it points to */
static int cant_protected(const char *path)
{
	char f[PATH_MAX], r[PATH_MAX];
	fold_path(path, f, sizeof f);
	if (is_protected_one(f))
		return 1;
	if (realpath(path, r) && is_protected_one(r))
		return 1;
	return 0;
}

static int draw_cant(struct view *v)
{
	fl_canvas cv = {v->px, v->W, v->H, v->W, {0, 0, v->W, v->H}};
	const fl_color white = FL_HEX(0xf0f2f4);
	float s = v->s;
	fl_fill_rect(&cv, fl_r(0, 0, v->W, v->H), FL_HEX(FIELD));
	int mx = v->W * 9 / 100;
	if (mx < 48)
		mx = 48;
	int mt = (int)(v->H * 0.07f);
	wide_text(v, v->wide, mx - (int)(4 * s), mt, white, OHNO, WIDE_X);
	int y = mt + wide_height(v->wide, WIDE_X) + (int)(4 * s);
	fl_text(&cv, v->face, mx - (int)(4 * s), y, white, EGG_FACE, -1);
	y += fl_font_height(v->face) + (int)(6 * s);
	fl_text(&cv, v->head, mx, y, white, EGG_TEXT, -1);
	y += fl_font_height(v->head) + (int)(48 * s);

	static const char *const label[2] = {"Never mind", "Continue"};
	int bh = (int)(54 * s), bw = (int)(190 * s), gap = (int)(18 * s);
	for (int i = 0; i < 2; i++) {
		fl_rect r = fl_r(mx + i * (bw + gap), y, bw, bh);
		v->btn[i] = r;
		int on = v->focus == i;
		if (on) {
			fl_fill_rect(&cv, r, white);
			fl_text_in(&cv, v->key, r, FL_CENTER, 0, FL_HEX(FIELD), label[i]);
			fl_stroke_rect(&cv, fl_inset(r, -(int)(5 * s), -(int)(5 * s)), 2, fl_mix(FL_HEX(FIELD), white, 150));
		} else {
			fl_fill_rect(&cv, r, fl_mix(FL_HEX(FIELD), white, 26));
			fl_stroke_rect(&cv, r, 2, fl_mix(FL_HEX(FIELD), white, 110));
			fl_text_in(&cv, v->key, r, FL_CENTER, 0, white, label[i]);
		}
	}
	return 0;
}

/* the same words on a console, with the colours the Linux console can show */
static void cant_text_screen(int focus, int cols, int rows)
{
	const char *bg = "\033[48;2;31;61;46m", *fg = "\033[38;2;240;242;244m";
	printf("\033[0m%s%s\033[2J\033[H", bg, fg);
	int top = rows > 20 ? 4 : 2, x = cols > 60 ? 6 : 2;
	printf("\033[%d;%dH\033[1m%s   %s", top, x, "oh no.", EGG_FACE);
	printf("\033[%d;%dH%s", top + 2, x, EGG_TEXT);
	printf("\033[0m%s%s\033[%d;%dH", bg, fg, top + 5, x);
	printf(focus == 0 ? "\033[7m[ Never mind ]\033[0m%s%s" : "[ Never mind ]", bg, fg);
	printf("   ");
	printf(focus == 1 ? "\033[7m[ Continue ]\033[0m%s%s" : "[ Continue ]", bg, fg);
	printf("\033[%d;%dH\033[2mEnter picks, Tab or the arrows switch, Esc cancels\033[0m", top + 7, x);
	fflush(stdout);
}

/* 0: Continue, 1: Never mind (also when nobody can answer: no keys, end of input) */
static int cant_text(void)
{
	struct termios old, raw;
	int tty = isatty(0);
	if (!tty)
		return 1;
	tcgetattr(0, &old);
	raw = old;
	cfmakeraw(&raw);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	tcsetattr(0, TCSANOW, &raw);
	struct winsize ws = {0};
	ioctl(1, TIOCGWINSZ, &ws);
	int cols = ws.ws_col ? ws.ws_col : 80, rows = ws.ws_row ? ws.ws_row : 25;
	int focus = 0, res = -1;
	cant_text_screen(focus, cols, rows);
	while (res < 0) {
		unsigned char c;
		if (read(0, &c, 1) <= 0) {
			res = 1;
			break;
		}
		if (c == '\r' || c == '\n')
			res = focus ? 0 : 1;
		else if (c == 'n' || c == 'N' || c == 3)
			res = 1;
		else if (c == 'c' || c == 'C')
			res = 0;
		else if (c == '\t')
			focus ^= 1;
		else if (c == 27) {
			unsigned char seq[2];
			struct pollfd pf = {.fd = 0, .events = POLLIN};
			if (poll(&pf, 1, 40) > 0 && read(0, &seq[0], 1) == 1 && seq[0] == '[' && read(0, &seq[1], 1) == 1) {
				if (seq[1] == 'C' || seq[1] == 'D')
					focus ^= 1;
			} else {
				res = 1;
			}
		}
		if (res < 0)
			cant_text_screen(focus, cols, rows);
	}
	tcsetattr(0, TCSANOW, &old);
	printf("\033[0m\033[2J\033[H");
	fflush(stdout);
	return res;
}

/* the error sound, once; nobody waits for it */
static void cant_sound(void)
{
	if (getenv("FLCRASH_NO_SOUND"))
		return;
	pid_t p = fork();
	if (p < 0)
		return;
	if (p == 0) {
		pid_t g = fork();
		if (g == 0) {
			setsid();
			execlp("flsound", "flsound", "error", (char *)NULL);
		}
		_exit(0);
	}
	waitpid(p, NULL, 0);
}

struct egg {
	struct view v;
	fl_app *app;
	int W, H;
	int res; /* -1 open, 0 continue, 1 never mind */
};

static void egg_draw(fl_ui *ui, void *ud)
{
	struct egg *e = ud;
	int W = ui->cv.w, H = ui->cv.h;
	if (!e->v.px || W != e->W || H != e->H) {
		free(e->v.px);
		e->W = e->v.W = W;
		e->H = e->v.H = H;
		e->v.px = calloc((size_t)W * H, 4);
		e->v.mul = 0;
		if (!e->v.px || open_fonts(&e->v) < 0)
			return;
	}
	draw_cant(&e->v);
	if (ui->cv.px)
		for (int y = 0; y < H; y++)
			memcpy(ui->cv.px + (size_t)y * ui->cv.stride, e->v.px + (size_t)y * W, (size_t)W * 4);
}

static void egg_done(struct egg *e, int res)
{
	e->res = res;
	fl_app_quit(e->app);
}

static int egg_key(fl_win *w, uint32_t keycode, uint32_t sym, const char *utf8, int mods, int pressed, void *ud)
{
	struct egg *e = ud;
	if (!pressed)
		return 1;
	switch (sym) {
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		egg_done(e, e->v.focus ? 0 : 1);
		break;
	case XKB_KEY_Escape:
	case XKB_KEY_n:
	case XKB_KEY_N:
		egg_done(e, 1);
		break;
	case XKB_KEY_c:
	case XKB_KEY_C:
		egg_done(e, 0);
		break;
	case XKB_KEY_Tab:
	case XKB_KEY_ISO_Left_Tab:
	case XKB_KEY_Left:
	case XKB_KEY_Right:
		e->v.focus ^= 1;
		fl_win_redraw(w);
		break;
	}
	return 1;
}

static int egg_pointer(fl_win *w, int type, int x, int y, int button, void *ud)
{
	struct egg *e = ud;
	for (int i = 0; i < 2; i++)
		if (fl_in(e->v.btn[i], x, y)) {
			if (e->v.focus != i && type == FL_PTR_MOVE) {
				e->v.focus = i;
				fl_win_redraw(w);
			}
			if (type == FL_PTR_UP)
				egg_done(e, i ? 0 : 1);
		}
	return 1;
}

static void egg_close(fl_win *w, void *ud)
{
	egg_done(ud, 1);
}

/* 0: Continue, 1: Never mind, -1: no desktop to show it on */
static int cant_gui(int focus)
{
	static struct egg e;
	e.res = -1;
	e.v.focus = focus;
	e.app = fl_app_new(NULL, NULL);
	if (!e.app)
		return -1;
	fl_win_opts o = {
		.title = "FreeLFS", .app_id = "flcrash", .w = 1280, .h = 800, .decorated = 0, .fullscreen = 0,
		.resizable = 1, .draw = egg_draw, .key = egg_key, .pointer = egg_pointer, .close = egg_close, .ud = &e,
	};
	fl_app_output_size(e.app, &o.w, &o.h);
	fl_win *w = fl_win_new(e.app, &o);
	if (!w) {
		fl_app_free(e.app);
		return -1;
	}
	/* The work area, not the whole screen: a fullscreen window hides the panel (flcomp does that for
	 * games), but the egg is a question on the desktop, so it stacks below the panel. */
	fl_win_maximize(w);
	cant_sound();
	fl_app_run(e.app);
	fl_app_free(e.app);
	return e.res < 0 ? 1 : e.res;
}

static int cant_main(int argc, char **argv)
{
	int W = 1920, H = 1080, text = 0, focus = 0, any = 0;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--cant"))
			continue;
		if (!strcmp(a, "--text"))
			text = 1;
		else if (!strcmp(a, "--focus") && i + 1 < argc)
			focus = !strcmp(argv[++i], "continue");
		else if (!strcmp(a, "--size") && i + 1 < argc)
			sscanf(argv[++i], "%dx%d", &W, &H);
		else if (!strcmp(a, "--always"))
			any = 1;	/* ask whatever the path is (screenshots, tests) */
		else if (a[0] == '-' && a[1] == '-') {
			fprintf(stderr, "usage: flcrash --cant PATH...  (exit 0: go ahead, 1: Never mind)\n");
			return 2;
		}
	}
	int hit = any;
	for (int i = 1; i < argc && !hit; i++) {
		const char *a = argv[i];
		if (a[0] == '-' && a[1] == '-') {
			if ((!strcmp(a, "--focus") || !strcmp(a, "--size")) && i + 1 < argc)
				i++;
			continue;
		}
		if (cant_protected(a))
			hit = 1;
	}
	if (!hit)
		return 0;
	signal(SIGPIPE, SIG_IGN);
	const char *ppm = getenv("FLCRASH_PPM");
	if (ppm && *ppm) {
		struct view v = {.W = W, .H = H, .focus = focus};
		v.px = calloc((size_t)W * H, 4);
		if (!v.px || open_fonts(&v) < 0) {
			fprintf(stderr, "flcrash: cannot load the IBM Plex fonts\n");
			return 1;
		}
		draw_cant(&v);
		write_ppm(&v, ppm);
		return 1;
	}
	int r = -1;
	if (!text && getenv("WAYLAND_DISPLAY"))
		r = cant_gui(focus);
	if (r < 0)
		r = cant_text();
	return r;
}

/* ---- main ---------------------------------------------------------------------------------------- */

/* --demo CODE: what the real path would have seen for that code (status, time, log). Matches
 * FL-NAME, NAME or a prefix of it; EXIT-3 and so on take the number. */
static void add_line(const char *l)
{
	if (nlines < MAXLINES)
		lines[nlines++] = strdup(l);
}

static int demo_setup(struct info *in, const char *code)
{
	static const struct {
		const char *name;
		int status;
		long secs;
		int crashes;
		const char *log;	/* what flcomp wrote last, "" none */
	} demos[] = {
		{"SIGSEGV", 139, 8040, 0, "flcomp: fatal: SIGSEGV in blit (render.c)"},
		{"SIGABRT", 134, 1260, 0, "flcomp: assertion failed: surface->px != NULL (surface.c)"},
		{"SIGBUS", 135, 3600, 0, "flcomp: mmap of a client buffer: the file was truncated"},
		{"SIGILL", 132, 95, 0, ""},
		{"SIGFPE", 136, 420, 0, ""},
		{"SIGKILL", 137, 17000, 0, ""},
		{"SIGHUP", 129, 200, 0, ""},
		{"SIG24", 152, 5400, 0, ""},
		{"NO-SEAT", 1, 1, 0, "flcomp: cannot open a seat: Permission denied"},
		{"NO-DISPLAY", 1, 2, 0, "flcomp: no usable display found in /dev/dri"},
		{"NO-RUNTIME", 1, 0, 0, "flcomp: cannot create /run/user/1000: Permission denied"},
		{"NO-MEMORY", 1, 1, 0, "flcomp: out of memory"},
		{"START-FAILED", 1, 2, 0, "flcomp: cannot create 1920x1080 dumb buffer: Invalid argument"},
		{"RESTART-LOOP", 139, 14, 3, "flcomp: fatal: SIGSEGV in blit (render.c)"},
	};
	const char *last = demos[0].log;
	in->status = demos[0].status;
	in->secs = demos[0].secs;
	in->crashes = 0;
	if (code) {
		char want[40];
		snprintf(want, sizeof want, "%s", !strncasecmp(code, "FL-", 3) ? code + 3 : code);
		for (char *p = want; *p; p++)
			*p = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
		int found = 0;
		if (!strncmp(want, "EXIT-", 5) && atoi(want + 5) > 0 && atoi(want + 5) < 128) {
			in->status = atoi(want + 5);
			in->secs = 640;
			last = "flcomp: fatal: the compositor lost its output";
			found = 1;
		}
		for (size_t i = 0; !found && i < sizeof demos / sizeof *demos; i++)
			if (!strncmp(demos[i].name, want, strlen(want)) && want[0]) {
				in->status = demos[i].status;
				in->secs = demos[i].secs;
				in->crashes = demos[i].crashes;
				last = demos[i].log;
				found = 1;
			}
		if (!found)
			return 0;
	}
	static const char *fake[] = {
		"session: start 09:12:44 as anna", "flcomp: 1920x1080 on wayland-0",
		"flpanel: connected, 1920 px wide", "flterm: new window 1", "flfiles: opened /home/anna",
		"flcomp: dmabuf: LINEAR only (no GPU path)", "freebws: page load https://example.org/",
		"freebws: process 4121 for https://example.org/ started",
		"flcomp: surface 0x5608a1 committed a buffer with the wrong stride",
		"flcomp: xdg_surface 0x5608d4 acked serial 117, expected 118",
	};
	if (in->secs >= EARLY_SECS)
		for (size_t i = 0; i < sizeof fake / sizeof *fake; i++)
			add_line(fake[i]);
	if (*last)
		add_line(last);
	if (!nlines)
		add_line("(the log is empty)");
	return 1;
}

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "--cant"))
			return cant_main(argc, argv);
	struct info in = {.status = 1, .secs = -1};
	snprintf(in.prog, sizeof in.prog, "flcomp");
	int demo = 0, text = 0, view_log = 0, W = 1920, H = 1080;
	const char *demo_code = NULL;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		int more = i + 1 < argc;
		if (!strcmp(a, "--status") && more)
			in.status = atoi(argv[++i]);
		else if (!strcmp(a, "--prog") && more)
			snprintf(in.prog, sizeof in.prog, "%s", argv[++i]);
		else if (!strcmp(a, "--secs") && more)
			in.secs = atol(argv[++i]);
		else if (!strcmp(a, "--log") && more)
			snprintf(in.log, sizeof in.log, "%s", argv[++i]);
		else if (!strcmp(a, "--crashes") && more)
			in.crashes = atoi(argv[++i]);
		else if (!strcmp(a, "--demo")) {
			demo = 1;
			if (more && argv[i + 1][0] != '-')
				demo_code = argv[++i];
		}
		else if (!strcmp(a, "--text"))
			text = 1;
		else if (!strcmp(a, "--view") && more)
			view_log = !strcmp(argv[++i], "log");
		else if (!strcmp(a, "--size") && more)
			sscanf(argv[++i], "%dx%d", &W, &H);
		else {
			fprintf(stderr, "usage: flcrash --status N [--secs N] [--crashes N] [--prog NAME] [--log FILE]\n"
			                "       flcrash --demo [CODE] [--view log] [--size WxH]  (FLCRASH_PPM=FILE: write a picture)\n"
			                "         CODE: SIGSEGV SIGABRT SIGBUS SIGILL SIGFPE SIGKILL SIGHUP NO-SEAT NO-DISPLAY\n"
			                "               NO-RUNTIME NO-MEMORY START-FAILED EXIT-<n> RESTART-LOOP\n"
			                "       flcrash --text\n");
			return !strcmp(a, "-h") || !strcmp(a, "--help") ? 0 : 2;
		}
	}
	signal(SIGPIPE, SIG_IGN);
	if (!demo)
		read_os(&in);
	else
		snprintf(in.os, sizeof in.os, "FreeLFS 0.1");
	if (demo) {
		if (!demo_setup(&in, demo_code)) {
			fprintf(stderr, "flcrash: no such code: %s\n", demo_code);
			return 2;
		}
		if (!in.log[0])
			snprintf(in.log, sizeof in.log, "/run/user/1000/flcomp.log");
	} else {
		if (!in.log[0]) {
			const char *rt = getenv("XDG_RUNTIME_DIR");
			if (rt && *rt)
				snprintf(in.log, sizeof in.log, "%s/flcomp.log", rt);
		}
		load_log(in.log);
	}
	describe(&in);
	if (!demo)
		save_copy(&in);

	const char *ppm = getenv("FLCRASH_PPM");
	struct view v = {.W = W, .H = H, .mode = view_log};
	if (demo && ppm && *ppm) {
		if (W < 320 || H < 240 || W > 8192 || H > 8192)
			return 2;
		v.px = calloc((size_t)W * H, 4);
		if (!v.px || open_fonts(&v) < 0) {
			fprintf(stderr, "flcrash: cannot load the IBM Plex fonts\n");
			return 1;
		}
		render(&v, &in);
		return write_ppm(&v, ppm) ? 1 : 0;
	}
	if (!text && !(demo && !isatty(0)) && drm_open(&v) == 0) {
		v.px = calloc((size_t)v.W * v.H, 4);
		if (v.px && open_fonts(&v) == 0) {
			int code = drm_loop(&v, &in);
			drm_close();
			return code;
		}
		drm_close();
	}
	return text_loop(&in);
}
