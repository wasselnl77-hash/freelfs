/* flgreet-ui -- the login screen you see. Runs as root under flgreet's flcomp.
 * SPDX-License-Identifier: MIT
 *
 * Shows the accounts (uid 1000-59999), a password field and Log in / Shut
 * down. The password is checked here against /etc/shadow with crypt_r; the
 * user name goes to flgreet over the pipe in $FLGREET_FD (stdout without
 * it, for tests) and the program exits 0. The password never leaves this
 * process, is never printed or logged, and its buffer is wiped after use.
 *
 *   empty hash in /etc/shadow   logs in without a password
 *   hash starting ! or *        locked: refused
 *   anything else               must match; a wrong one waits one second
 */
#include <crypt.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <flui.h>
#include <math.h>
#include <pwd.h>
#include <shadow.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { ID_USERS = 10, ID_PW = 11, ID_LOGIN = 12, ID_SHUT = 13 };
enum { MIN_UID = 1000, MAX_UID = 59999, MAX_USERS = 64, PW_CAP = 256 };

struct user {
	char name[48];
	char label[48]; /* the comment field up to the first comma, else the name */
};

struct G {
	fl_app *app;
	fl_font *title, *small, *big;
	fl_timer *shut_timer;
	struct user users[MAX_USERS];
	int nusers;
	int sel, first; /* selected account, first visible row */
	char pw[PW_CAP];
	char err[96];
	int shut_armed;
	int shut_fd; /* stderr of `flpower off` while it runs, else -1 */
	pid_t shut_pid;
	char shut_msg[96];
	int done;
	int out_fd;
	/* the take-over from flsplash (see "the wordmark" below) */
	int anim;		/* 0 none, 1 running, 2 frozen at FLGREET_ANIM_T (tests) */
	long anim_ms;		/* where a frozen animation stands */
	struct timespec anim_t0;
	int anim_started;
	fl_timer *anim_timer;
	fl_font *wm_font;
	float wm_px;
	float s_mid, s_base, s_em, s_pen; /* flsplash's wordmark: ink middle, baseline, em, pen start */
};

static void load_users(struct G *g)
{
	setpwent();
	struct passwd *p;
	while (g->nusers < MAX_USERS && (p = getpwent())) {
		if (p->pw_uid < MIN_UID || p->pw_uid > MAX_UID)
			continue;
		struct user *u = &g->users[g->nusers++];
		snprintf(u->name, sizeof u->name, "%s", p->pw_name);
		snprintf(u->label, sizeof u->label, "%s", p->pw_name);
		if (p->pw_gecos && p->pw_gecos[0] && p->pw_gecos[0] != ',') {
			snprintf(u->label, sizeof u->label, "%.*s", (int)strcspn(p->pw_gecos, ","), p->pw_gecos);
		}
	}
	endpwent();
}

/* ---- the password check ------------------------------------------------------------------------- */

static int ct_equal(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b);
	unsigned char d = la != lb;
	for (size_t i = 0; i < la; i++)
		d |= (unsigned char)a[i] ^ (unsigned char)b[i % (lb ? lb : 1)];
	return d == 0;
}

/* 0 ok, else a message for the screen */
static const char *check_password(const char *user, const char *pw)
{
	struct spwd *sp = getspnam(user);
	if (!sp)
		return "This account cannot log in (no shadow entry).";
	const char *hash = sp->sp_pwdp;
	if (sp->sp_expire >= 0 && (long)(time(NULL) / 86400) > sp->sp_expire)
		return "This account has expired.";
	if (hash[0] == 0)
		return NULL;
	if (hash[0] == '!' || hash[0] == '*')
		return "This account is locked.";
	struct crypt_data *cd = calloc(1, sizeof *cd);
	if (!cd)
		return "Out of memory.";
	char *r = crypt_r(pw, hash, cd);
	int good = r && r[0] != '*' && ct_equal(r, hash);
	explicit_bzero(cd, sizeof *cd);
	free(cd);
	return good ? NULL : "Wrong password.";
}

static void report_user(struct G *g, const char *name)
{
	char line[64];
	int n = snprintf(line, sizeof line, "%s\n", name);
	const char *p = line;
	while (n > 0) {
		ssize_t w = write(g->out_fd, p, (size_t)n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		p += w;
		n -= (int)w;
	}
}

static void try_login(struct G *g)
{
	if (g->nusers == 0)
		return;
	const char *name = g->users[g->sel].name;
	const char *why = check_password(name, g->pw);
	explicit_bzero(g->pw, sizeof g->pw);
	if (!why) {
		report_user(g, name);
		g->done = 1;
		fl_app_quit(g->app);
		return;
	}
	struct timespec d = {1, 0};
	nanosleep(&d, NULL);
	snprintf(g->err, sizeof g->err, "%s", why);
	fl_win_redraw_all(g->app); /* this pass drew the old state: show the message */
}

/* ---- the wordmark: flsplash's "FreeLFS" flies into the card --------------------------------------
 *
 * flsplash ends on a big, widely spaced, pale "FreeLFS" (IBM Plex Sans Medium, em = 0.205 of the logo
 * size S, 0.2 em extra between the letters, a vertical gradient from near white to grey-green) under
 * the logo, and that picture stays on screen until we draw. We start by drawing the same word at the
 * same size, place, weight and colours on a dark room like flsplash's, then move it to the card's
 * title while its size, spacing and colour settle, the room turns into the desktop colour and the
 * card fades in. The numbers below are flsplash's own (flsplash.c layout() and pick_size(),
 * genart.c render() and make_text()); the logo and its glow cannot be redrawn here and still go at
 * once. The card's title is this same wordmark at rest, so there is no hand-over from one drawing
 * to another. Only done when flsplash really handed over just now (/run/flsplash.released).
 */
#define WM_TEXT "FreeLFS"
#define WM_MS 640		/* the whole take-over */
#define WM_REST_PX 28		/* the title in the card */
#define WM_ASC 0.73f		/* height of the tallest letter, in em */
#define WM_BASE 0.715f		/* baseline below the top of the layer flsplash puts it in, in em */
#define WM_PEN_FIX -0.02f	/* left side bearing of the F against the ink box, in em (tuned on a screenshot) */

static const int splash_sizes[] = {128, 176, 240, 320, 432, 576};

static int even_up(float v) { return ((int)lroundf(v) + 1) & ~1; }

/* the wordmark as flsplash leaves it on a w x h screen: pen start x, baseline, em; 0 if no art fits */
static int splash_wordmark(int w, int h, float *x_ink_mid, float *base, float *em)
{
	int best = -1;
	for (size_t i = 0; i < sizeof splash_sizes / sizeof *splash_sizes; i++) {
		int S = splash_sizes[i];
		int ly = (int)lroundf(0.60f * (float)S), yf = ly + (int)lroundf(0.925f * (float)S);
		int tytop = yf + (int)lroundf(0.36f * (float)S);
		int Rw = even_up(2.30f * (float)S), Rh = even_up((float)tytop + 0.42f * (float)S);
		if (Rw <= w && Rh <= h && (best < 0 || S * 100 <= h * 40))
			best = (int)i;
	}
	if (best < 0)
		return 0;
	int S = splash_sizes[best];
	int ly = (int)lroundf(0.60f * (float)S), yf = ly + (int)lroundf(0.925f * (float)S);
	int tytop = yf + (int)lroundf(0.36f * (float)S);
	int Rw = even_up(2.30f * (float)S), Rh = even_up((float)tytop + 0.42f * (float)S);
	int ry = h / 2 - h / 40 - S * 139 / 100;
	if (ry + Rh > h)
		ry = h - Rh;
	if (ry < 0)
		ry = 0;
	*em = (float)lroundf(0.205f * (float)S);
	*x_ink_mid = (float)((w - Rw) / 2) + (float)Rw / 2.0f;
	*base = (float)(ry + tytop) + WM_BASE * *em;
	return 1;
}

static float smooth01(float x)
{
	x = x < 0 ? 0 : x > 1 ? 1 : x;
	return x * x * (3 - 2 * x);
}

static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

static long anim_elapsed(struct G *g)
{
	if (g->anim == 2)
		return g->anim_ms;
	struct timespec n;
	clock_gettime(CLOCK_MONOTONIC, &n);
	return (n.tv_sec - g->anim_t0.tv_sec) * 1000 + (n.tv_nsec - g->anim_t0.tv_nsec) / 1000000;
}

static void anim_tick(void *ud)
{
	struct G *g = ud;
	fl_win_redraw_all(g->app);
}

/* flsplash's room without the logo: top to the floor line (61% of the height), then the dark floor */
static fl_color room_color(int y, int h)
{
	static const struct { float at; int rgb[3]; } st[] = {
		{0.00f, {0x0d, 0x10, 0x12}}, {0.60f, {0x21, 0x27, 0x2c}}, {0.62f, {0x1b, 0x22, 0x25}},
		{0.65f, {0x16, 0x1b, 0x1d}}, {0.74f, {0x10, 0x13, 0x15}}, {0.83f, {0x0a, 0x0d, 0x0f}},
		{1.00f, {0x06, 0x07, 0x08}},
	};
	float f = (float)y / (float)(h > 1 ? h - 1 : 1);
	size_t i = 1;
	while (i < sizeof st / sizeof *st - 1 && f > st[i].at)
		i++;
	float t = (f - st[i - 1].at) / (st[i].at - st[i - 1].at);
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	int c[3];
	for (int k = 0; k < 3; k++)
		c[k] = (int)lroundf(lerpf((float)st[i - 1].rgb[k], (float)st[i].rgb[k], t));
	return FL_HEX(c[0] << 16 | c[1] << 8 | c[2]);
}

/* The word at pen start x, baseline base, size em px and sp em of extra space after each letter.
 * toward_strong 0..255 moves the colour from flsplash's gradient to the theme's heading colour.
 * The gradient is done in bands of two rows (the glyphs come from the cache, so this is cheap). */
static void wordmark(fl_canvas *cv, struct G *g, float x, float base, float em, float sp, int toward_strong)
{
	if (!cv->px)
		return;
	if (!g->wm_font || fabsf(g->wm_px - em) > 0.2f) {
		fl_font_close(g->wm_font);
		g->wm_font = fl_font_ui(500, em);
		g->wm_px = em;
	}
	fl_font *f = g->wm_font;
	int sp64 = (int)lroundf(sp * em * 64.0f);
	if (sp64 < 1)
		sp64 = 1; /* not 0: that would round every advance to whole pixels and shift the letters */
	int b = (int)lroundf(base), top = b - fl_font_ascent(f), px = (int)lroundf(x);
	if (toward_strong >= 255) {
		fl_text_ex(cv, f, px, top, fl_th.strong, WM_TEXT, -1, sp64);
		return;
	}
	int ink_h = (int)lroundf(WM_ASC * em) + 2, inkt = b - (int)lroundf(WM_ASC * em);
	fl_rect save = cv->clip;
	for (int y = inkt - 2; y < b + (int)(0.1f * em) + 2; y += 2) {
		float t = smooth01((float)(y + 1 - inkt) / (float)(ink_h - 1));
		int r = (int)lroundf(255 * lerpf(0.98f, 0.70f, t)), gr = (int)lroundf(255 * lerpf(1.0f, 0.78f, t)),
		    bl = (int)lroundf(255 * lerpf(0.985f, 0.74f, t));
		fl_color c = fl_mix(FL_HEX(r << 16 | gr << 8 | bl), fl_th.strong, toward_strong);
		cv->clip = save;
		fl_clip(cv, fl_r(0, y, cv->w, 2));
		fl_text_ex(cv, f, px, top, c, WM_TEXT, -1, sp64);
	}
	cv->clip = save;
}

/* ---- widgets ------------------------------------------------------------------------------------------ */

/* A password field: squares for dots, no clipboard, no selection. 1 on Enter. */
static int pw_field(fl_ui *ui, struct G *g, fl_rect r)
{
	int res = fl_widget(ui, ID_PW, r, FL_FOCUSABLE | FL_NO_FOCUS_RING);
	int focused = res & FL_FOCUS, enter = 0;
	size_t len = strlen(g->pw);
	if (focused) {
		if (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_KP_Enter) {
			enter = 1;
			ui->key = 0;
		} else if (ui->key == XKB_KEY_BackSpace) {
			while (len > 0 && ((unsigned char)g->pw[len - 1] & 0xc0) == 0x80)
				g->pw[--len] = 0;
			if (len > 0)
				g->pw[--len] = 0;
			ui->key = 0;
			g->err[0] = 0;
		} else if (ui->key == XKB_KEY_Escape || ((ui->mods & FL_MOD_CTRL) && (ui->key == 'u' || ui->key == 'U'))) {
			explicit_bzero(g->pw, sizeof g->pw);
			len = 0;
			ui->key = 0;
		}
		size_t n = strlen(ui->text);
		if (n && len + n < sizeof g->pw && !(ui->mods & FL_MOD_CTRL)) {
			memcpy(g->pw + len, ui->text, n + 1);
			len += n;
			g->err[0] = 0;
		}
		explicit_bzero(ui->text, sizeof ui->text);
	}
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, r, fl_th.bg);
	fl_stroke_rect(cv, r, 1, focused ? fl_th.accent : fl_th.line);
	int x = r.x + 14, cy = r.y + r.h / 2;
	if (len) {
		int dots = 0;
		for (size_t i = 0; i < len; i++)
			if (((unsigned char)g->pw[i] & 0xc0) != 0x80)
				dots++;
		int maxd = (r.w - 28) / 14;
		int first = dots > maxd ? dots - maxd : 0;
		for (int i = first; i < dots; i++)
			fl_fill_rect(cv, fl_r(x + (i - first) * 14, cy - 3, 7, 7), fl_th.text);
		x += (dots - first) * 14;
	}
	if (focused) {
		/* the caret: a bar after the last dot */
		int cx = len ? x : r.x + 14;
		fl_fill_rect(cv, fl_r(cx, cy - 9, 1, 18), fl_th.text);
	}
	return enter;
}

static void user_row(fl_ui *ui, fl_rect r, int i, int hover, int selected, void *ud)
{
	struct G *g = ud;
	fl_canvas *cv = &ui->cv;
	if (selected)
		fl_fill_rect(cv, r, fl_th.select);
	else if (hover)
		fl_fill_rect(cv, r, fl_th.raised);
	if (selected)
		fl_fill_rect(cv, fl_r(r.x, r.y, 3, r.h), fl_th.accent);
	fl_rect av = fl_r(r.x + 14, r.y + (r.h - 34) / 2, 34, 34);
	fl_fill_rect(cv, av, selected ? fl_th.accent : fl_th.line);
	char ini[8] = {0};
	unsigned char c = (unsigned char)g->users[i].label[0];
	int n = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
	memcpy(ini, g->users[i].label, (size_t)n);
	if (n == 1 && c >= 'a' && c <= 'z')
		ini[0] = (char)(c - 32);
	fl_text_in(cv, ui->f_semi, av, FL_CENTER, 0, selected ? fl_th.on_accent : fl_th.text, ini);
	fl_text_in(cv, selected ? ui->f_med : ui->f_reg, fl_r(r.x + 62, r.y, r.w - 70, r.h), FL_LEFT, 0,
	           selected ? fl_th.strong : fl_th.text, g->users[i].label);
}

/* `flpower off` (we are root: it tries poweroff, then init, then the kernel, each with a time limit). Its
 * words and its status come back here: when it fails the login screen says so instead of doing nothing. */
static void shut_out(void *ud, int fd, short rev)
{
	struct G *g = ud;
	(void)rev;
	char buf[96];
	ssize_t n = read(fd, buf, sizeof buf - 1);
	if (n == -1 && (errno == EINTR || errno == EAGAIN))
		return;
	size_t have = strlen(g->shut_msg);
	if (n > 0) {
		if (have + (size_t)n < sizeof g->shut_msg) {
			memcpy(g->shut_msg + have, buf, (size_t)n);
			g->shut_msg[have + (size_t)n] = 0;
		}
		return;
	}
	fl_fd_remove(g->app, fd);
	close(fd);
	g->shut_fd = -1;
	int st = 0;
	while (g->shut_pid > 0 && waitpid(g->shut_pid, &st, 0) < 0 && errno == EINTR)
		;
	g->shut_pid = 0;
	if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
		return; /* init has been told: the screen goes away by itself */
	for (char *c = g->shut_msg; *c; c++)
		if (*c == '\n')
			*c = ' ';
	snprintf(g->err, sizeof g->err, "Shut down failed: %s", g->shut_msg[0] ? g->shut_msg : "no answer");
	g->shut_armed = 0;
	g->shut_msg[0] = 0;
}

static void start_shutdown(struct G *g)
{
	int e[2];
	if (g->shut_fd >= 0)
		return; /* already running */
	if (pipe2(e, O_CLOEXEC) < 0) {
		snprintf(g->err, sizeof g->err, "Shut down failed: no pipe");
		return;
	}
	pid_t p = fork();
	if (p < 0) {
		close(e[0]);
		close(e[1]);
		snprintf(g->err, sizeof g->err, "Shut down failed: fork");
		return;
	}
	if (p == 0) {
		dup2(e[1], 2);
		execl("/usr/bin/flpower", "flpower", "off", (char *)NULL);
		fprintf(stderr, "flpower: %s", strerror(errno));
		_exit(127);
	}
	close(e[1]);
	g->shut_pid = p;
	g->shut_fd = e[0];
	g->shut_msg[0] = 0;
	g->err[0] = 0;
	snprintf(g->err, sizeof g->err, "Shutting down...");
	if (fl_fd_add(g->app, e[0], POLLIN, shut_out, g) < 0) {
		close(e[0]);
		g->shut_fd = -1;
	}
}

static void shut_disarm(void *ud)
{
	struct G *g = ud;
	g->shut_armed = 0;
	fl_timer_arm(g->shut_timer, 0, 0);
	fl_win_redraw_all(g->app);
}

static void draw(fl_ui *ui, void *ud)
{
	struct G *g = ud;
	fl_canvas *cv = &ui->cv;
	int W = cv->w, H = cv->h;
	float p = 1;
	if (g->anim) {
		if (!g->anim_started) {
			g->anim_started = 1;
			clock_gettime(CLOCK_MONOTONIC, &g->anim_t0);
			if (g->anim == 1)
				fl_timer_arm(g->anim_timer, 16, 16);
		}
		p = (float)anim_elapsed(g) / WM_MS;
		if (p >= 1) {
			p = 1;
			g->anim = 0;
			fl_timer_arm(g->anim_timer, 0, 0);
		}
	}
	/* how far the room has become the desktop colour; the light theme gets there sooner, so that the
	 * pale word is never on a mid-grey ground */
	int bgk = (int)lroundf(255 * smooth01(p / (fl_th.dark ? 0.8f : 0.45f)));
	if (p < 1) {
		for (int y = 0; y < H; y++)
			fl_fill_rect(cv, fl_r(0, y, W, 1), fl_mix(room_color(y, H), fl_th.bg, bgk));
	} else {
		fl_fill_rect(cv, fl_r(0, 0, W, H), fl_th.bg);
	}
	if (!ui->focus)
		ui->focus = ID_PW; /* whatever you type goes to the password */

	const int CW = 440, PX = 40;
	int rows = g->nusers > 4 ? 4 : g->nusers ? g->nusers : 1;
	int list_h = rows * 52;
	int CH = 36 + 36 + 4 + 18 + 28 + list_h + 24 + 16 + 6 + 44 + 8 + 20 + 14 + 40 + 36;
	fl_rect card = fl_r((W - CW) / 2, (H - CH) / 2, CW, CH);
	fl_fill_rect(cv, card, fl_th.panel);
	fl_fill_rect(cv, fl_r(card.x, card.y, CW, 4), fl_th.accent);
	int x = card.x + PX, w = CW - 2 * PX, y = card.y + 36;
	/* the title is the wordmark at rest (drawn last, over the fading card, see the end of draw) */
	float wm_x1 = (float)x, wm_b1 = (float)(y + fl_font_ascent(g->title));
	y += 36 + 4;
	fl_text(cv, ui->f_reg, x, y, fl_th.dim, g->nusers ? "Log in" : "No accounts yet", -1);
	y += 18 + 28;

	/* the password field and the buttons first: Enter belongs to the field, not the list */
	int pw_y = y + list_h + 24 + 16 + 6;
	int enter = pw_field(ui, g, fl_r(x, pw_y, w, 44));

	if (g->nusers) {
		fl_rect lr = fl_r(x, y, w, list_h);
		fl_fill_rect(cv, lr, fl_th.bg);
		fl_stroke_rect(cv, lr, 1, fl_th.line);
		/* no fl_list here: its highlight follows the mouse, and a pointer parked in the
		 * middle of the screen would pick an account by itself. Here only a click or
		 * Up/Down picks. */
		int before = g->sel;
		if (ui->key == XKB_KEY_Up && g->sel > 0) {
			g->sel--;
			ui->key = 0;
		} else if (ui->key == XKB_KEY_Down && g->sel + 1 < g->nusers) {
			g->sel++;
			ui->key = 0;
		}
		fl_rect in = fl_inset(lr, 1, 1);
		int res = fl_widget(ui, ID_USERS, in, 0);
		if ((res & FL_HOVER) && ui->wheel) {
			g->first += ui->wheel;
			ui->wheel = 0;
		}
		if (g->sel < g->first)
			g->first = g->sel;
		if (g->sel >= g->first + rows)
			g->first = g->sel - rows + 1;
		if (g->first > g->nusers - rows)
			g->first = g->nusers - rows;
		if (g->first < 0)
			g->first = 0;
		fl_rect save = cv->clip;
		fl_clip(cv, in);
		for (int i = g->first; i < g->nusers && i < g->first + rows; i++) {
			fl_rect rr = fl_r(in.x, in.y + (i - g->first) * 52 - (i - g->first) * 0, in.w - (g->nusers > rows ? 8 : 0), 52);
			int rres = fl_widget(ui, ID_USERS + 100 + (uint32_t)i, rr, 0);
			if (rres & FL_CLICK)
				g->sel = i;
			user_row(ui, rr, i, rres & FL_HOVER, i == g->sel, g);
		}
		cv->clip = save;
		if (g->nusers > rows) {
			int sy = in.y + in.h * g->first / g->nusers, sh = in.h * rows / g->nusers;
			fl_fill_rect(cv, fl_r(in.x + in.w - 5, sy, 4, sh), fl_th.line);
		}
		if (g->sel != before) {
			explicit_bzero(g->pw, sizeof g->pw);
			g->err[0] = 0;
		}
	} else {
		fl_text_in(cv, ui->f_reg, fl_r(x, y, w, 52), FL_LEFT, 0, fl_th.dim,
		           "Log in as root on another console to add one.");
	}
	y += list_h + 24;
	fl_text(cv, g->small, x, y, fl_th.dim, "Password", -1);

	y = pw_y + 44 + 8;
	if (g->err[0])
		fl_text_in(cv, ui->f_reg, fl_r(x, y, w, 20), FL_LEFT, 0, fl_th.err, g->err);
	y += 20 + 14;

	int shut = fl_button(ui, ID_SHUT, fl_r(x, y, 150, 40), g->shut_armed ? "Click again" : "Shut down", FL_BTN_OUTLINE);
	int login = fl_button(ui, ID_LOGIN, fl_r(x + w - 108, y, 108, 40), "Log in", FL_BTN_ACCENT);
	if (shut) {
		if (!g->shut_armed) {
			g->shut_armed = 1;
			fl_timer_arm(g->shut_timer, 4000, 0);
		} else {
			start_shutdown(g);
		}
	}
	if ((login || enter) && g->nusers)
		try_login(g);
	fl_color foot = fl_th.dim;
	if (p < 1) {
		/* the card comes up out of the room while the word is still on its way */
		int ac = (int)lroundf(255 * smooth01((p - 0.45f) / 0.55f));
		/* run_pass(w, NULL) calls draw without pixels (layout only): nothing to blend then */
		for (int yy = card.y; cv->px && yy < card.y + CH && yy < H; yy++) {
			fl_color row = fl_mix(room_color(yy, H), fl_th.bg, bgk);
			for (int xx = card.x; xx < card.x + CW && xx < W; xx++) {
				uint32_t *px = &cv->px[(size_t)yy * (size_t)cv->stride + (size_t)xx];
				*px = fl_mix(row, *px, ac);
			}
		}
		foot = fl_mix(fl_mix(room_color(H - 20, H), fl_th.bg, bgk), fl_th.dim, ac);
		float e = smooth01(p);
		float em = lerpf(g->s_em, (float)WM_REST_PX, e);
		float sp = lerpf(0.20f, 0.0f, e);
		if (g->s_pen < 0) { /* centre the ink like flsplash: the advance holds a space after the last letter */
			fl_canvas dry = {0};
			if (!g->wm_font || fabsf(g->wm_px - g->s_em) > 0.2f) {
				fl_font_close(g->wm_font);
				g->wm_font = fl_font_ui(500, g->s_em);
				g->wm_px = g->s_em;
			}
			float adv = (float)fl_text_ex(&dry, g->wm_font, 0, 0, 0, WM_TEXT, -1, (int)lroundf(0.20f * g->s_em * 64.0f));
			g->s_pen = g->s_mid - (adv - 0.20f * g->s_em) / 2 + WM_PEN_FIX * g->s_em;
		}
		float c0 = fl_th.dark ? 0.15f : 0.45f;
		wordmark(cv, g, lerpf(g->s_pen, wm_x1, e), lerpf(g->s_base, wm_b1, e), em, sp,
		         (int)lroundf(255 * smooth01((p - c0) / (1 - c0))));
	} else {
		wordmark(cv, g, wm_x1, wm_b1, (float)WM_REST_PX, 0.0f, 255);
	}
	if (!g->done)
		fl_text_in(cv, g->small, fl_r(W - 216, H - 12 - 16, 200, 16), FL_RIGHT, 0, foot, "FreeLFS 0.1");
}

int main(int argc, char **argv)
{
	if (argc > 1) {
		fprintf(stderr, "usage: flgreet-ui\nRuns under flgreet's compositor; prints the user name to $FLGREET_FD.\n");
		return !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") ? 0 : 2;
	}
	struct G *g = calloc(1, sizeof *g);
	if (!g)
		return 1;
	g->out_fd = 1;
	const char *fd = getenv("FLGREET_FD");
	if (fd && *fd)
		g->out_fd = atoi(fd);
	g->app = fl_app_new(NULL, NULL);
	if (!g->app) {
		fprintf(stderr, "flgreet-ui: cannot connect to a Wayland compositor\n");
		return 1;
	}
	g->title = fl_font_ui(500, WM_REST_PX);
	g->small = fl_font_ui(400, 12);
	g->shut_fd = -1;
	g->shut_timer = fl_timer_new(g->app, shut_disarm, g);
	load_users(g);

	int ow, oh;
	fl_app_output_size(g->app, &ow, &oh);
	/* flsplash just handed the screen over (it removes its file when it ends): take over seamlessly.
	 * FLGREET_ANIM_T=MS (tests) shows the take-over frozen at that many ms. */
	const char *at = getenv("FLGREET_ANIM_T");
	if ((at && *at) || access("/run/flsplash.released", F_OK) == 0) {
		float mid;
		if (splash_wordmark(ow, oh, &mid, &g->s_base, &g->s_em)) {
			g->s_mid = mid;
			g->s_pen = -1;
			g->anim = at && *at ? 2 : 1;
			g->anim_ms = at ? atol(at) : 0;
			g->anim_timer = fl_timer_new(g->app, anim_tick, g);
		}
	}
	fl_win_opts o = {
		.title = "Log in", .app_id = "flgreet", .w = ow, .h = oh, .fullscreen = 1, .font_px = 14,
		.draw = draw, .close = NULL, .ud = g,
	};
	fl_win *w = fl_win_new(g->app, &o);
	if (!w)
		return 1;
	fl_win_ui(w)->focus = ID_PW;
	fl_app_watch_conf(g->app, NULL, NULL);
	fl_app_run(g->app);
	fl_app_free(g->app);
	explicit_bzero(g->pw, sizeof g->pw);
	return g->done ? 0 : 1;
}
