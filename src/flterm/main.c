/* flterm -- the FreeLFS terminal
 * SPDX-License-Identifier: MIT
 *
 * A window from libflui around the emulator in term.c, a pty and flsh (or
 * the command after -e). Colours: the 16 ANSI ones are mapped to the
 * boot-menu palette in term.c; the ground is #16191c in both themes.
 */
#include <errno.h>
#include <flui.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "flterm.h"

#define PADX 14
#define PADY 12
#define FONT_PX 13.5f
#define LINE_H 1.55f

#define FG_DEFAULT FL_HEX(0xd8dce0)
#define BG_DEFAULT FL_HEX(0x16191c)
#define SELECT_BG FL_HEX(0x2f4a3b)

struct ft {
	fl_app *app;
	fl_win *win;
	struct term *t;
	fl_font *reg, *bold, *italic;
	int cw, ch, fh;
	int pty;
	pid_t pid;
	int view_off;
	int focused;
	int sel_on, sel_drag, ax, ay, bx, by; /* absolute lines: scrollback first */
};

/* ---- pty --------------------------------------------------------------------------------- */

static void pty_write(struct ft *f, const char *s, size_t n)
{
	while (n && f->pty >= 0) {
		ssize_t w = write(f->pty, s, n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN) {
				struct pollfd p = {f->pty, POLLOUT, 0};
				poll(&p, 1, 100);
				continue;
			}
			return;
		}
		s += w;
		n -= (size_t)w;
	}
}

static void pty_ready(void *ud, int fd, short rev)
{
	struct ft *f = ud;
	char buf[65536];
	ssize_t n = read(fd, buf, sizeof buf);
	if (n <= 0) {
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			return;
		fl_fd_remove(f->app, fd);
		f->pty = -1;
		fl_app_quit(f->app); /* the shell ended */
		return;
	}
	term_feed(f->t, buf, (size_t)n);
	if (f->t->scrolled) {
		if (f->view_off > 0)
			f->view_off += f->t->scrolled; /* stay on what you are reading */
		if (f->view_off > f->t->sb_len)
			f->view_off = f->t->sb_len;
		f->sel_on = f->sel_drag = 0;
		f->t->scrolled = 0;
	}
	fl_win_redraw(f->win);
}

static int spawn(struct ft *f, char **cmd)
{
	struct winsize ws = {(unsigned short)f->t->rows, (unsigned short)f->t->cols,
	                     (unsigned short)(f->t->cols * f->cw), (unsigned short)(f->t->rows * f->ch)};
	f->pid = forkpty(&f->pty, NULL, NULL, &ws);
	if (f->pid < 0)
		return -1;
	if (f->pid == 0) {
		setenv("TERM", "xterm-256color", 1);
		setenv("COLORTERM", "truecolor", 1);
		unsetenv("FLUI_PPM");
		unsetenv("FLUI_PPM_DELAY");
		signal(SIGCHLD, SIG_DFL);
		/* a terminal starts in the home directory when the directory it was started in cannot be used
		 * (the login chain can leave a user in root's unreadable home) */
		if (access(".", R_OK | X_OK) != 0) {
			const char *home = getenv("HOME");
			if (home && *home && chdir(home) != 0)
				(void)chdir("/");
		}
		if (cmd) {
			execvp(cmd[0], cmd);
		} else {
			const char *sh = getenv("SHELL");
			execlp("flsh", "flsh", (char *)NULL);
			if (sh && *sh)
				execl(sh, sh, (char *)NULL);
			execl("/bin/sh", "sh", (char *)NULL);
		}
		fprintf(stderr, "flterm: cannot run %s: %s\n", cmd ? cmd[0] : "flsh", strerror(errno));
		_exit(127);
	}
	f->t->out_fd = f->pty;
	return 0;
}

/* ---- window events ------------------------------------------------------------------------- */

static void on_title(void *ud, const char *title)
{
	struct ft *f = ud;
	fl_win_set_title(f->win, title[0] ? title : "Terminal");
}

static void resized(fl_win *w, int cw, int chh, void *ud)
{
	struct ft *f = ud;
	int cols = (cw - 2 * PADX) / f->cw, rows = (chh - 2 * PADY) / f->ch;
	if (cols < 10)
		cols = 10;
	if (rows < 3)
		rows = 3;
	term_resize(f->t, cols, rows);
	if (f->view_off > f->t->sb_len)
		f->view_off = f->t->sb_len;
	f->sel_on = f->sel_drag = 0;
	if (f->pty >= 0) {
		struct winsize ws = {(unsigned short)rows, (unsigned short)cols,
		                     (unsigned short)(cols * f->cw), (unsigned short)(rows * f->ch)};
		ioctl(f->pty, TIOCSWINSZ, &ws);
	}
}

static void focus_changed(fl_win *w, int focused, void *ud)
{
	((struct ft *)ud)->focused = focused;
}

static void paste(struct ft *f)
{
	size_t n;
	char *s = fl_clipboard_get(f->app, &n);
	if (!s)
		return;
	/* no escapes get through, so a paste cannot end bracketed mode early */
	char *o = malloc(n + 1);
	size_t m = 0;
	for (size_t i = 0; o && i < n; i++) {
		if (s[i] == 0x1b)
			continue;
		if (s[i] == '\n' && !f->t->bracketed)
			o[m++] = '\r';
		else if (s[i] == '\r' && i + 1 < n && s[i + 1] == '\n' && !f->t->bracketed)
			continue;
		else
			o[m++] = s[i];
	}
	if (o) {
		if (f->t->bracketed)
			pty_write(f, "\033[200~", 6);
		pty_write(f, o, m);
		if (f->t->bracketed)
			pty_write(f, "\033[201~", 6);
	}
	free(o);
	free(s);
	f->view_off = 0;
}

static void copy_selection(struct ft *f)
{
	if (!f->sel_on)
		return;
	char *s = term_text(f->t, f->ax, f->ay, f->bx, f->by);
	if (s && *s)
		fl_clipboard_set(f->app, s, strlen(s));
	free(s);
}

static int mod_param(int mods)
{
	return 1 + (mods & FL_MOD_SHIFT ? 1 : 0) + (mods & FL_MOD_ALT ? 2 : 0) + (mods & FL_MOD_CTRL ? 4 : 0);
}

static int on_key(fl_win *w, uint32_t code, uint32_t sym, const char *utf8, int mods, int pressed, void *ud)
{
	struct ft *f = ud;
	if (!pressed)
		return 1;
	char buf[32];
	int n = 0;
	const int ctrl_shift = (mods & (FL_MOD_CTRL | FL_MOD_SHIFT)) == (FL_MOD_CTRL | FL_MOD_SHIFT);

	if (ctrl_shift && (sym == 'C' || sym == 'c')) {
		copy_selection(f);
		return 1;
	}
	if ((ctrl_shift && (sym == 'V' || sym == 'v')) || (sym == XKB_KEY_Insert && (mods & FL_MOD_SHIFT))) {
		paste(f);
		fl_win_redraw(w);
		return 1;
	}
	if (sym == XKB_KEY_Insert && (mods & FL_MOD_CTRL)) {
		copy_selection(f);
		return 1;
	}
	if ((mods & FL_MOD_SHIFT) && (sym == XKB_KEY_Page_Up || sym == XKB_KEY_Page_Down)) {
		int page = f->t->rows - 1;
		f->view_off += sym == XKB_KEY_Page_Up ? page : -page;
		if (f->view_off < 0)
			f->view_off = 0;
		if (f->view_off > f->t->sb_len)
			f->view_off = f->t->sb_len;
		fl_win_redraw(w);
		return 1;
	}
	if (sym == XKB_KEY_Shift_L || sym == XKB_KEY_Shift_R || sym == XKB_KEY_Control_L ||
	    sym == XKB_KEY_Control_R || sym == XKB_KEY_Alt_L || sym == XKB_KEY_Alt_R ||
	    sym == XKB_KEY_Super_L || sym == XKB_KEY_Super_R || sym == XKB_KEY_Caps_Lock ||
	    sym == XKB_KEY_ISO_Level3_Shift || sym == XKB_KEY_Num_Lock || sym == XKB_KEY_Meta_L ||
	    sym == XKB_KEY_Meta_R)
		return 1;

	const char *csi_final = NULL;
	int tilde = 0;
	switch (sym) {
	case XKB_KEY_Up: csi_final = "A"; break;
	case XKB_KEY_Down: csi_final = "B"; break;
	case XKB_KEY_Right: csi_final = "C"; break;
	case XKB_KEY_Left: csi_final = "D"; break;
	case XKB_KEY_Home: csi_final = "H"; break;
	case XKB_KEY_End: csi_final = "F"; break;
	case XKB_KEY_Insert: tilde = 2; break;
	case XKB_KEY_Delete: tilde = 3; break;
	case XKB_KEY_Page_Up: tilde = 5; break;
	case XKB_KEY_Page_Down: tilde = 6; break;
	case XKB_KEY_F5: tilde = 15; break;
	case XKB_KEY_F6: tilde = 17; break;
	case XKB_KEY_F7: tilde = 18; break;
	case XKB_KEY_F8: tilde = 19; break;
	case XKB_KEY_F9: tilde = 20; break;
	case XKB_KEY_F10: tilde = 21; break;
	case XKB_KEY_F11: tilde = 23; break;
	case XKB_KEY_F12: tilde = 24; break;
	case XKB_KEY_F1: csi_final = "P"; break;
	case XKB_KEY_F2: csi_final = "Q"; break;
	case XKB_KEY_F3: csi_final = "R"; break;
	case XKB_KEY_F4: csi_final = "S"; break;
	default: break;
	}
	int is_f14 = sym >= XKB_KEY_F1 && sym <= XKB_KEY_F4;
	if (csi_final) {
		int mp = mod_param(mods);
		if (mp > 1)
			n = snprintf(buf, sizeof buf, "\033[1;%d%s", mp, csi_final);
		else if (is_f14 || (f->t->appcursor && sym != XKB_KEY_Home && sym != XKB_KEY_End) ||
		         (f->t->appcursor && (sym == XKB_KEY_Home || sym == XKB_KEY_End)))
			n = snprintf(buf, sizeof buf, "\033O%s", csi_final);
		else
			n = snprintf(buf, sizeof buf, "\033[%s", csi_final);
	} else if (tilde) {
		int mp = mod_param(mods);
		if (mp > 1)
			n = snprintf(buf, sizeof buf, "\033[%d;%d~", tilde, mp);
		else
			n = snprintf(buf, sizeof buf, "\033[%d~", tilde);
	} else if (sym == XKB_KEY_ISO_Left_Tab) {
		n = snprintf(buf, sizeof buf, "\033[Z");
	} else if (sym == XKB_KEY_BackSpace) {
		buf[n++] = (mods & FL_MOD_CTRL) ? 0x08 : 0x7f;
		if (mods & FL_MOD_ALT) {
			memmove(buf + 1, buf, 1);
			buf[0] = 0x1b;
			n = 2;
		}
	} else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
		if (mods & FL_MOD_ALT)
			buf[n++] = 0x1b;
		buf[n++] = '\r';
	} else if (sym == XKB_KEY_Escape) {
		buf[n++] = 0x1b;
	} else if (sym == XKB_KEY_Tab) {
		if (mods & FL_MOD_ALT)
			buf[n++] = 0x1b;
		buf[n++] = '\t';
	} else if (sym == XKB_KEY_space && (mods & FL_MOD_CTRL)) {
		buf[n++] = 0;
	} else if (utf8[0]) {
		if (mods & FL_MOD_ALT)
			buf[n++] = 0x1b;
		size_t l = strlen(utf8);
		if (l > sizeof buf - 2)
			l = sizeof buf - 2;
		memcpy(buf + n, utf8, l);
		n += (int)l;
	}
	if (n) {
		pty_write(f, buf, (size_t)n);
		if (f->view_off || f->sel_on) {
			f->view_off = 0;
			f->sel_on = f->sel_drag = 0;
			fl_win_redraw(w);
		}
	}
	return 1;
}

/* ---- selection with the mouse ------------------------------------------------------------------ */

static void cell_at(struct ft *f, int mx, int my, int *col, int *abs_line)
{
	fl_rect c = fl_win_content(f->win);
	int x = (mx - c.x - PADX) / f->cw, v = (my - c.y - PADY) / f->ch;
	if (mx - c.x - PADX < 0)
		x = 0;
	if (my - c.y - PADY < 0)
		v = 0;
	x = x < 0 ? 0 : x >= f->t->cols ? f->t->cols - 1 : x;
	v = v < 0 ? 0 : v >= f->t->rows ? f->t->rows - 1 : v;
	*col = x;
	*abs_line = f->t->sb_len - f->view_off + v;
}

static int is_word(uint32_t cp)
{
	return cp > ' ' && cp != '"' && cp != '\'' && cp != '(' && cp != ')' && cp != '<' && cp != '>' &&
	       cp != '|' && cp != ';' && cp != '`';
}

static int on_pointer(fl_win *w, int type, int x, int y, int button, void *ud)
{
	struct ft *f = ud;
	fl_rect c = fl_win_content(w);
	if (type == FL_PTR_WHEEL) {
		if (!fl_in(c, x, y))
			return 0;
		if (f->t->alt) {
			const char *k = f->t->appcursor ? (button > 0 ? "\033OB" : "\033OA") : (button > 0 ? "\033[B" : "\033[A");
			for (int i = 0; i < 3 * (button < 0 ? -button : button); i++)
				pty_write(f, k, 3);
		} else {
			f->view_off -= button * 3;
			if (f->view_off < 0)
				f->view_off = 0;
			if (f->view_off > f->t->sb_len)
				f->view_off = f->t->sb_len;
		}
		fl_win_redraw(w);
		return 1;
	}
	if (type == FL_PTR_DOWN && fl_in(c, x, y)) {
		if (button == FL_BTN_M) {
			paste(f);
			return 1;
		}
		if (button != FL_BTN_L)
			return 0;
		int col, line;
		cell_at(f, x, y, &col, &line);
		int clicks = fl_win_ui(w)->clicks;
		if (clicks >= 3) { /* a whole line */
			f->ax = 0;
			f->bx = f->t->cols - 1;
			f->ay = f->by = line;
			f->sel_on = 1;
			f->sel_drag = 0;
		} else if (clicks == 2) { /* a word */
			int v = line - (f->t->sb_len - f->view_off);
			const struct row *row = term_view_row(f->t, f->view_off, v);
			int a = col, b = col;
			while (a > 0 && a - 1 < row->cols && is_word(row->c[a - 1].cp))
				a--;
			while (b + 1 < row->cols && is_word(row->c[b + 1].cp))
				b++;
			f->ax = a;
			f->bx = b;
			f->ay = f->by = line;
			f->sel_on = is_word(col < row->cols ? row->c[col].cp : 0);
			f->sel_drag = 0;
		} else {
			f->ax = f->bx = col;
			f->ay = f->by = line;
			f->sel_on = 0;
			f->sel_drag = 1;
		}
		fl_win_redraw(w);
		return 1;
	}
	if (type == FL_PTR_MOVE && f->sel_drag) {
		int col, line;
		cell_at(f, x, y, &col, &line);
		f->bx = col;
		f->by = line;
		f->sel_on = f->bx != f->ax || f->by != f->ay;
		fl_win_redraw(w);
		return 1;
	}
	if (type == FL_PTR_UP && f->sel_drag) {
		f->sel_drag = 0;
		return 1;
	}
	return 0;
}

/* ---- drawing -------------------------------------------------------------------------------------- */

static int selected(const struct ft *f, int line, int col)
{
	if (!f->sel_on)
		return 0;
	int ax = f->ax, ay = f->ay, bx = f->bx, by = f->by;
	if (by < ay || (by == ay && bx < ax)) {
		int tx = ax, ty = ay;
		ax = bx, ay = by, bx = tx, by = ty;
	}
	if (line < ay || line > by)
		return 0;
	if (line == ay && col < ax)
		return 0;
	if (line == by && col > bx)
		return 0;
	return 1;
}

static void draw(fl_ui *ui, void *ud)
{
	struct ft *f = ud;
	struct term *t = f->t;
	fl_canvas *cv = &ui->cv;
	fl_rect c = fl_win_content(f->win);
	fl_fill_rect(cv, c, BG_DEFAULT);
	if (ui->mouse_in && fl_in(c, ui->mx, ui->my))
		ui->cursor = FL_CURSOR_IBEAM;
	int ox = c.x + PADX, oy = c.y + PADY;
	int ty_off = (f->ch - f->fh) / 2;

	for (int v = 0; v < t->rows; v++) {
		const struct row *r = term_view_row(t, f->view_off, v);
		int y = oy + v * f->ch;
		int line = t->sb_len - f->view_off + v;
		int cols = r->cols < t->cols ? r->cols : t->cols;
		/* backgrounds, in runs */
		for (int x = 0; x < cols;) {
			const struct cell *k = &r->c[x];
			fl_color bg = k->bg ? k->bg : BG_DEFAULT, fg = k->fg ? k->fg : FG_DEFAULT;
			if (k->attr & A_REVERSE)
				bg = fg;
			if (selected(f, line, x))
				bg = SELECT_BG;
			int run = x + 1;
			while (run < cols) {
				const struct cell *k2 = &r->c[run];
				fl_color b2 = k2->bg ? k2->bg : BG_DEFAULT, f2 = k2->fg ? k2->fg : FG_DEFAULT;
				if (k2->attr & A_REVERSE)
					b2 = f2;
				if (selected(f, line, run))
					b2 = SELECT_BG;
				if (b2 != bg)
					break;
				run++;
			}
			if (bg != BG_DEFAULT)
				fl_fill_rect(cv, fl_r(ox + x * f->cw, y, (run - x) * f->cw, f->ch), bg);
			x = run;
		}
		/* glyphs */
		for (int x = 0; x < cols; x++) {
			const struct cell *k = &r->c[x];
			if (k->attr & A_CONT)
				continue;
			fl_color fg = k->fg ? k->fg : FG_DEFAULT, bg = k->bg ? k->bg : BG_DEFAULT;
			if (k->attr & A_REVERSE) {
				fl_color tmp = fg;
				fg = k->bg ? k->bg : BG_DEFAULT;
				bg = tmp;
			}
			if (selected(f, line, x) && !(k->attr & A_REVERSE))
				bg = SELECT_BG;
			if (k->attr & A_DIM)
				fg = fl_mix(fg, bg, 110);
			int px = ox + x * f->cw;
			if (k->cp > ' ') {
				fl_font *font = (k->attr & A_BOLD) ? f->bold : (k->attr & A_ITALIC) ? f->italic : f->reg;
				fl_glyph(cv, font, px, y + ty_off, fg, k->cp);
			}
			if (k->attr & A_UNDER)
				fl_fill_rect(cv, fl_r(px, y + f->ch - 4, f->cw * (k->attr & A_WIDE ? 2 : 1), 1), fg);
			if (k->attr & A_STRIKE)
				fl_fill_rect(cv, fl_r(px, y + f->ch / 2, f->cw * (k->attr & A_WIDE ? 2 : 1), 1), fg);
		}
	}

	if (f->view_off == 0 && t->cursor_visible && t->cy < t->rows) {
		int x = ox + t->cx * f->cw, y = oy + t->cy * f->ch;
		const struct cell *k = &t->scr[t->cy].c[t->cx < t->scr[t->cy].cols ? t->cx : 0];
		int w = f->cw * (k->attr & A_WIDE ? 2 : 1);
		if (f->focused || fl_app_ppm(f->app)) {
			fl_fill_rect(cv, fl_r(x, y, w, f->ch), FG_DEFAULT);
			if (k->cp > ' ')
				fl_glyph(cv, (k->attr & A_BOLD) ? f->bold : f->reg, x, y + ty_off, BG_DEFAULT, k->cp);
		} else {
			fl_stroke_rect(cv, fl_r(x, y, w, f->ch), 1, FG_DEFAULT);
		}
	}

	if (f->view_off > 0) { /* where you are in the history */
		int total = t->sb_len + t->rows;
		int bh = c.h * t->rows / total;
		if (bh < 20)
			bh = 20;
		int by = c.y + (c.h - bh) * (t->sb_len - f->view_off) / (t->sb_len ? t->sb_len : 1);
		fl_fill_rect(cv, fl_r(c.x + c.w - 5, by, 3, bh), FL_HEX(0x3a4048));
	}
}

static void closed(fl_win *w, void *ud)
{
	struct ft *f = ud;
	if (f->pid > 0)
		kill(f->pid, SIGHUP);
	fl_app_quit(f->app);
}

/* ---- main ----------------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	char **cmd = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-e") && i + 1 < argc) {
			cmd = &argv[i + 1];
			break;
		}
		fprintf(stderr, "usage: flterm [-e COMMAND [ARGS...]]\n");
		return !strcmp(argv[i], "-h") || !strcmp(argv[i], "--help") ? 0 : 2;
	}
	signal(SIGPIPE, SIG_IGN);
	struct ft f = {0};
	f.pty = -1;
	f.app = fl_app_new(NULL, NULL);
	if (!f.app) {
		fprintf(stderr, "flterm: cannot connect to a Wayland compositor\n");
		return 1;
	}
	f.reg = fl_font_mono(400, FONT_PX);
	f.bold = fl_font_mono(600, FONT_PX);
	f.italic = fl_font_open("IBM Plex Mono", 400, 1, FONT_PX);
	if (!f.reg || !f.bold || !f.italic) {
		fprintf(stderr, "flterm: no monospace font (is ibm-plex installed?)\n");
		return 1;
	}
	f.cw = fl_font_cell(f.reg);
	if (f.cw < 1)
		f.cw = 8;
	f.ch = (int)(FONT_PX * LINE_H + 0.5f);
	f.fh = fl_font_height(f.reg);
	f.t = term_new(80, 24);
	f.t->on_title = on_title;
	f.t->ud = &f;

	int w = 2 + 2 * PADX + 80 * f.cw, h = 37 + 2 * PADY + 24 * f.ch;
	fl_win_opts o = {
		.title = "Terminal", .app_id = "flterm", .w = w, .h = h, .min_w = 200, .min_h = 120,
		.decorated = 1, .resizable = 1, .draw = draw, .close = closed, .key = on_key,
		.pointer = on_pointer, .resized = resized, .focus = focus_changed, .ud = &f,
	};
	f.win = fl_win_new(f.app, &o);
	if (!f.win)
		return 1;
	if (spawn(&f, cmd) < 0) {
		fprintf(stderr, "flterm: cannot open a pty: %s\n", strerror(errno));
		return 1;
	}
	fl_fd_add(f.app, f.pty, POLLIN, pty_ready, &f);
	fl_app_watch_conf(f.app, NULL, NULL);
	fl_app_run(f.app);
	if (f.pid > 0) {
		kill(f.pid, SIGHUP);
		waitpid(f.pid, NULL, WNOHANG);
	}
	fl_app_free(f.app);
	term_free(f.t);
	return 0;
}
