/* The line editor: emacs-style keys, live syntax colours, grey
 * suggestions from history, a Tab completion menu, history search.
 *
 * Every change redraws the whole edit area: move to its first row, clear
 * to the end of the screen, print prompt, line, suggestion and menu, then
 * put the cursor back. Lines that wrap are handled by counting cells.
 * SPDX-License-Identifier: MIT */
#include "flsh.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

bool edit_interrupted;

enum {
	K_EOF = -2,
	K_WINCH = -3,
	K_ESC = 0x1000,
	K_UP,
	K_DOWN,
	K_LEFT,
	K_RIGHT,
	K_HOME,
	K_END,
	K_DEL,
	K_PGUP,
	K_PGDN,
	K_STAB,
	K_CLEFT,
	K_CRIGHT,
	K_PASTE,
	K_ALT = 0x10000, /* | the key */
};
#undef CTRL
#define CTRL(c) ((c) & 0x1f)

struct ed {
	struct strbuf b;
	size_t cur;
	char *prefix_lines; /* prompt lines before the last */
	char *prompt;       /* the prompt's last line */
	int pw;
	int rows_up; /* the cursor's row below the first row of the edit area */
	bool no_ghost;
	/* history */
	long hpos;
	char *hsaved;
	/* completion menu */
	bool menu;
	struct completion comp;
	long sel;
	char *morig;
	size_t mend;
	/* history search */
	bool search;
	struct strbuf sq;
	long spos;
	bool sfail;
	char *ssaved;
	char *yank;
};
static struct ed E;

static struct termios saved_tio;
static bool raw_mode;
static volatile sig_atomic_t winch;

static void on_winch(int sig)
{
	winch = 1;
}

void edit_init(void)
{
	struct sigaction sa = {0};
	sa.sa_handler = on_winch;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGWINCH, &sa, NULL);
}

int term_width(void)
{
	struct winsize ws;
	if (ioctl(2, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
		return ws.ws_col;
	const char *c = var_get("COLUMNS");
	long n;
	if (c && str_to_int(c, &n) == 0 && n > 0)
		return (int)n;
	return 80;
}

static int term_height(void)
{
	struct winsize ws;
	if (ioctl(2, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
		return ws.ws_row;
	return 24;
}

static void out(const char *s)
{
	write_all(2, s, strlen(s));
}

static bool raw_on(void)
{
	if (tcgetattr(0, &saved_tio) < 0)
		return false;
	struct termios t = saved_tio;
	t.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	t.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	if (tcsetattr(0, TCSADRAIN, &t) < 0)
		return false;
	raw_mode = true;
	out("\033[?2004h"); /* bracketed paste */
	return true;
}

static void raw_off(void)
{
	if (!raw_mode)
		return;
	out("\033[?2004l");
	tcsetattr(0, TCSADRAIN, &saved_tio);
	raw_mode = false;
}

void edit_reset_term(void)
{
	raw_off();
}

/* ---- keys ------------------------------------------------------------------------------------ */
static int read_byte(int timeout_ms)
{
	if (timeout_ms >= 0) {
		struct pollfd p = {.fd = 0, .events = POLLIN};
		if (poll(&p, 1, timeout_ms) <= 0)
			return -1;
	}
	for (;;) {
		unsigned char c;
		ssize_t r = read(0, &c, 1);
		if (r == 1)
			return c;
		if (r < 0 && errno == EINTR) {
			if (winch)
				return K_WINCH;
			continue;
		}
		return K_EOF;
	}
}

static int read_key(void)
{
	int c = read_byte(-1);
	if (c != 033)
		return c;
	int d = read_byte(50);
	if (d < 0)
		return K_ESC;
	if (d != '[' && d != 'O')
		return d == K_EOF ? K_ESC : K_ALT | d;
	char seq[16];
	int n = 0;
	for (;;) {
		int e = read_byte(50);
		if (e < 0)
			return K_ESC;
		if (n < (int)sizeof(seq) - 1)
			seq[n++] = (char)e;
		if (e >= 0x40 && e <= 0x7e)
			break;
	}
	seq[n] = '\0';
	char final = seq[n - 1];
	if (n == 1 || d == 'O') {
		switch (final) {
		case 'A': return K_UP;
		case 'B': return K_DOWN;
		case 'C': return K_RIGHT;
		case 'D': return K_LEFT;
		case 'H': return K_HOME;
		case 'F': return K_END;
		case 'Z': return K_STAB;
		}
		return -1;
	}
	if (final == '~') {
		int num = atoi(seq);
		switch (num) {
		case 1:
		case 7: return K_HOME;
		case 4:
		case 8: return K_END;
		case 3: return K_DEL;
		case 5: return K_PGUP;
		case 6: return K_PGDN;
		case 200: return K_PASTE;
		}
		return -1;
	}
	/* modified arrows: 1;5C (ctrl) 1;3C (alt) */
	if (strchr(seq, ';')) {
		if (final == 'C')
			return K_CRIGHT;
		if (final == 'D')
			return K_CLEFT;
		if (final == 'H')
			return K_HOME;
		if (final == 'F')
			return K_END;
	}
	return -1;
}

/* ---- buffer helpers --------------------------------------------------------------------------- */
static void ins(size_t pos, const char *s, size_t n)
{
	sb_add(&E.b, "", 0);
	char *tail = xstrndup(E.b.buf + pos, E.b.len - pos);
	size_t tl = E.b.len - pos;
	E.b.len = pos;
	sb_add(&E.b, s, n);
	sb_add(&E.b, tail, tl);
	free(tail);
	if (E.cur >= pos)
		E.cur += n;
}

static void del(size_t from, size_t to)
{
	if (to <= from)
		return;
	memmove(E.b.buf + from, E.b.buf + to, E.b.len - to + 1);
	E.b.len -= to - from;
	if (E.cur >= to)
		E.cur -= to - from;
	else if (E.cur > from)
		E.cur = from;
}

static void set_line(const char *s)
{
	E.b.len = 0;
	sb_add(&E.b, s, strlen(s));
	E.cur = E.b.len;
}

static void cut(size_t from, size_t to)
{
	if (to <= from)
		return;
	free(E.yank);
	E.yank = xstrndup(E.b.buf + from, to - from);
	del(from, to);
}

static size_t prev_char(size_t i)
{
	if (!i)
		return 0;
	i--;
	while (i > 0 && ((unsigned char)E.b.buf[i] & 0xc0) == 0x80)
		i--;
	return i;
}

static size_t next_char(size_t i)
{
	if (i >= E.b.len)
		return E.b.len;
	return i + (size_t)utf8_len((unsigned char)E.b.buf[i]) > E.b.len
	           ? E.b.len
	           : i + (size_t)utf8_len((unsigned char)E.b.buf[i]);
}

static bool is_space(char c)
{
	return c == ' ' || c == '\t' || c == '\n';
}

static bool is_wordch(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
	       c == '_' || (unsigned char)c >= 0x80;
}

static size_t word_left(size_t i, bool alnum)
{
	while (i > 0 && (alnum ? !is_wordch(E.b.buf[i - 1]) : is_space(E.b.buf[i - 1])))
		i--;
	while (i > 0 && (alnum ? is_wordch(E.b.buf[i - 1]) : !is_space(E.b.buf[i - 1])))
		i--;
	return i;
}

static size_t word_right(size_t i, bool alnum)
{
	while (i < E.b.len && (alnum ? !is_wordch(E.b.buf[i]) : is_space(E.b.buf[i])))
		i++;
	while (i < E.b.len && (alnum ? is_wordch(E.b.buf[i]) : !is_space(E.b.buf[i])))
		i++;
	return i;
}

/* ---- drawing ------------------------------------------------------------------------------------ */
/* visible width, skipping escape sequences */
static int vis_width(const char *s, size_t n)
{
	int w = 0;
	for (size_t i = 0; i < n;) {
		unsigned char c = (unsigned char)s[i];
		if (c == 033) {
			i++;
			if (i < n && s[i] == '[') {
				i++;
				while (i < n && !((unsigned char)s[i] >= 0x40 && (unsigned char)s[i] <= 0x7e))
					i++;
				i++;
			} else if (i < n && s[i] == ']') {
				while (i < n && s[i] != '\a' && !(s[i] == 033 && i + 1 < n && s[i + 1] == '\\'))
					i++;
				i += i < n && s[i] == 033 ? 2 : 1;
			} else
				i++;
			continue;
		}
		if (c == 1 || c == 2) {
			i++;
			continue;
		}
		size_t len;
		w += c == '\n' ? 1 : utf8_width(s + i, n - i, &len);
		if (c == '\n')
			len = 1;
		i += len;
	}
	return w;
}

static const char *ghost(void)
{
	if (E.no_ghost || E.menu || E.search || E.cur != E.b.len || !E.b.len)
		return NULL;
	const char *h = hist_suggest(E.b.buf);
	return h ? h + E.b.len : NULL;
}

static int menu_cols(int cols, int *colw_out)
{
	int maxw = 1;
	for (size_t i = 0; i < E.comp.shown.n; i++) {
		int w = vis_width(E.comp.shown.v[i], strlen(E.comp.shown.v[i]));
		if (w > maxw)
			maxw = w;
	}
	int colw = maxw + 2;
	if (colw > cols - 1)
		colw = cols - 1;
	if (colw_out)
		*colw_out = colw;
	int ncols = (cols - 1) / (colw > 0 ? colw : 1);
	return ncols < 1 ? 1 : ncols;
}

/* Truncate s to at most w cells */
static void add_fit(struct strbuf *o, const char *s, int w, int *used)
{
	int u = 0;
	for (size_t i = 0; s[i];) {
		size_t len;
		int cw = utf8_width(s + i, strlen(s + i), &len);
		if (u + cw > w)
			break;
		sb_add(o, s + i, len);
		u += cw;
		i += len;
	}
	*used = u;
}

static int draw_menu(struct strbuf *o, int cols, bool at_row_start)
{
	int colw, ncols = menu_cols(cols, &colw);
	long n = (long)E.comp.shown.n;
	long nrows = (n + ncols - 1) / ncols;
	long maxrows = term_height() - 4;
	if (maxrows > 10)
		maxrows = 10;
	if (maxrows < 1)
		maxrows = 1;
	long first = 0;
	if (E.sel >= 0 && E.sel / ncols >= maxrows)
		first = E.sel / ncols - maxrows + 1;
	long shown = nrows - first < maxrows ? nrows - first : maxrows;
	int rows = 0;
	for (long r = first; r < first + shown; r++) {
		if (rows || !at_row_start)
			sb_addc(o, '\n');
		rows++;
		sb_addstr(o, "  ");
		for (int c = 0; c < ncols; c++) {
			long idx = r * ncols + c;
			if (idx >= n)
				break;
			const char *s = E.comp.shown.v[idx];
			size_t sl = strlen(s);
			bool dir = sl && s[sl - 1] == '/';
			if (use_color)
				sb_addstr(o, idx == E.sel ? "\033[97;42m" : dir ? "\033[34m" : "");
			else if (idx == E.sel)
				sb_addstr(o, "\033[7m");
			int used;
			add_fit(o, s, colw - 2 > 0 ? colw - 2 : 1, &used);
			sb_addstr(o, "\033[0m");
			if (c + 1 < ncols)
				for (int k = used; k < colw; k++)
					sb_addc(o, ' ');
		}
	}
	/* the footer */
	sb_addc(o, '\n');
	rows++;
	sb_addstr(o, "\033[90m  ");
	char foot[128];
	if (nrows > shown)
		snprintf(foot, sizeof(foot), "%ld of %ld   Tab: next   Enter: pick   Esc: cancel",
		         E.sel >= 0 ? E.sel + 1 : 0, n);
	else
		snprintf(foot, sizeof(foot), "Tab: next   Enter: pick   Esc: cancel");
	int used;
	add_fit(o, foot, cols - 3, &used);
	sb_addstr(o, "\033[0m");
	return rows;
}

static void refresh(void)
{
	int cols = term_width();
	struct strbuf o = {0};
	if (E.rows_up > 0)
		sb_addf(&o, "\033[%dA", E.rows_up);
	sb_addstr(&o, "\r\033[J");
	sb_addstr(&o, E.prompt);
	const char *buf = E.b.buf ? E.b.buf : "";
	if (use_color)
		hl_render(buf, E.b.len, E.cur, &o, NULL);
	else
		for (size_t i = 0; i < E.b.len; i++) {
			unsigned char c = (unsigned char)buf[i];
			if (c == '\n')
				sb_addc(&o, '~');
			else if (c < 0x20 || c == 0x7f) {
				sb_addc(&o, '^');
				sb_addc(&o, c == 0x7f ? '?' : (char)(c + '@'));
			} else
				sb_addc(&o, (char)c);
		}
	const char *g = ghost();
	if (g) {
		sb_addstr(&o, use_color ? "\033[90m" : "");
		sb_addstr(&o, g);
		sb_addstr(&o, use_color ? "\033[0m" : "");
	}
	int pos_end = E.pw + vis_width(buf, E.b.len) + (g ? vis_width(g, strlen(g)) : 0);
	int pos_cur = E.pw + vis_width(buf, E.cur);
	bool at_row_start = false;
	if (pos_end > 0 && pos_end % cols == 0) {
		sb_addc(&o, '\n'); /* past a full row: put the cursor on the next */
		at_row_start = true;
	}
	int last_row = pos_end / cols;
	if (E.menu)
		last_row += draw_menu(&o, cols, at_row_start) - (at_row_start ? 1 : 0);
	if (E.search) {
		sb_addstr(&o, at_row_start ? "" : "\n");
		sb_addstr(&o, "\033[90msearch history:\033[0m ");
		int used;
		add_fit(&o, E.sq.buf ? E.sq.buf : "", cols - 30, &used);
		if (E.sfail)
			sb_addstr(&o, use_color ? "  \033[31m(no match)\033[0m" : "  (no match)");
		last_row += at_row_start ? 0 : 1;
	}
	int cur_row = pos_cur / cols, cur_col = pos_cur % cols;
	if (last_row > cur_row)
		sb_addf(&o, "\033[%dA", last_row - cur_row);
	sb_addc(&o, '\r');
	if (cur_col)
		sb_addf(&o, "\033[%dC", cur_col);
	E.rows_up = cur_row;
	write_all(2, o.buf, o.len);
	sb_free(&o);
}

/* ---- history navigation and search ------------------------------------------------------------------ */
static void hist_reset(void)
{
	E.hpos = -1;
	free(E.hsaved);
	E.hsaved = NULL;
}

static void hist_move(int dir)
{
	long n = (long)hist_count();
	if (E.hpos < 0) {
		if (dir > 0)
			return;
		E.hsaved = xstrdup(E.b.buf ? E.b.buf : "");
		E.hpos = n;
	}
	const char *prefix = E.hsaved;
	size_t plen = strlen(prefix);
	const char *curline = E.b.buf ? E.b.buf : "";
	for (long i = E.hpos + dir; i >= 0 && i < n; i += dir) {
		const char *h = hist_at((size_t)i);
		if (!strncmp(h, prefix, plen) && strcmp(h, curline)) {
			E.hpos = i;
			set_line(h);
			return;
		}
	}
	if (dir > 0) {
		set_line(E.hsaved);
		hist_reset();
	}
}

static void search_run(long from)
{
	const char *q = E.sq.buf ? E.sq.buf : "";
	E.sfail = false;
	if (!*q)
		return;
	for (long i = from; i >= 0; i--) {
		const char *h = hist_at((size_t)i);
		const char *m = h ? strstr(h, q) : NULL;
		if (m) {
			E.spos = i;
			set_line(h);
			E.cur = (size_t)(m - h);
			return;
		}
	}
	E.sfail = true;
}

static void search_end(bool keep)
{
	if (!keep && E.ssaved)
		set_line(E.ssaved);
	free(E.ssaved);
	E.ssaved = NULL;
	E.search = false;
	sb_free(&E.sq);
}

/* ---- completion ------------------------------------------------------------------------------------ */
static void menu_end(void)
{
	E.menu = false;
	completion_free(&E.comp);
	free(E.morig);
	E.morig = NULL;
}

static void replace_region(size_t start, size_t end, const char *s)
{
	del(start, end);
	E.cur = start;
	ins(start, s, strlen(s));
}

static void menu_select(long sel)
{
	long n = (long)E.comp.words.n;
	if (!n)
		return;
	sel = ((sel % n) + n) % n;
	E.sel = sel;
	replace_region(E.comp.start, E.mend, E.comp.words.v[sel]);
	E.mend = E.comp.start + strlen(E.comp.words.v[sel]);
	E.cur = E.mend;
}

static void do_complete(void)
{
	struct completion c;
	complete_line(E.b.buf ? E.b.buf : "", E.cur, &c);
	size_t n = c.words.n;
	if (!n) {
		completion_free(&c);
		return;
	}
	if (n == 1) {
		replace_region(c.start, c.end, c.words.v[0]);
		if (!c.nospace && (E.cur >= E.b.len || E.b.buf[E.cur] != ' '))
			ins(E.cur, " ", 1);
		completion_free(&c);
		return;
	}
	/* the common prefix of all candidates */
	size_t plen = strlen(c.words.v[0]);
	for (size_t i = 1; i < n; i++) {
		size_t k = 0;
		while (k < plen && c.words.v[i][k] == c.words.v[0][k])
			k++;
		plen = k;
	}
	while (plen && ((unsigned char)c.words.v[0][plen] & 0xc0) == 0x80)
		plen--; /* not in the middle of a character */
	if (plen > c.end - c.start) {
		char *p = xstrndup(c.words.v[0], plen);
		replace_region(c.start, c.end, p);
		free(p);
		completion_free(&c);
		return;
	}
	E.comp = c;
	E.menu = true;
	E.sel = -1;
	E.morig = xstrndup(E.b.buf + c.start, c.end - c.start);
	E.mend = c.end;
}

/* Keys while the menu is open; false: close it and handle the key normally */
static bool menu_key(int k, bool *done)
{
	int colw, ncols = menu_cols(term_width(), &colw);
	long n = (long)E.comp.words.n;
	switch (k) {
	case '\t':
	case K_RIGHT:
		menu_select(E.sel + 1);
		return true;
	case K_STAB:
	case K_LEFT:
		menu_select(E.sel <= 0 ? n - 1 : E.sel - 1);
		return true;
	case K_DOWN:
		menu_select(E.sel < 0 ? 0 : E.sel + ncols < n ? E.sel + ncols : E.sel);
		return true;
	case K_UP:
		if (E.sel - ncols >= 0)
			menu_select(E.sel - ncols);
		return true;
	case '\r':
	case '\n':
		if (E.sel >= 0) {
			const char *w = E.comp.words.v[E.sel];
			size_t l = strlen(w);
			if (l && w[l - 1] != '/' && (E.cur >= E.b.len || E.b.buf[E.cur] != ' '))
				ins(E.cur, " ", 1);
		}
		menu_end();
		*done = false;
		return true;
	case K_ESC:
	case CTRL('g'):
		replace_region(E.comp.start, E.mend, E.morig);
		menu_end();
		return true;
	}
	menu_end();
	return false;
}

/* ---- the editor -------------------------------------------------------------------------------------- */
static char *plain_read(const char *prompt)
{
	if (prompt && sh.interactive) {
		out(prompt);
	}
	struct strbuf sb = {0};
	for (;;) {
		char c;
		ssize_t r = read(0, &c, 1);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0) {
			if (!sb.len) {
				sb_free(&sb);
				return NULL;
			}
			break;
		}
		if (c == '\n')
			break;
		sb_addc(&sb, c);
	}
	return sb_steal(&sb);
}

char *read_line_plain(void)
{
	return plain_read(NULL);
}

static void strip_markers(char *s)
{
	char *o = s;
	for (; *s; s++)
		if (*s != 1 && *s != 2)
			*o++ = *s;
	*o = '\0';
}

static void read_paste(void)
{
	struct strbuf p = {0};
	for (;;) {
		int c = read_byte(-1);
		if (c < 0)
			break;
		sb_addc(&p, (char)c);
		if (p.len >= 6 && !memcmp(p.buf + p.len - 6, "\033[201~", 6)) {
			p.len -= 6;
			break;
		}
	}
	for (size_t i = 0; i < p.len; i++)
		if (p.buf[i] == '\r')
			p.buf[i] = '\n';
	/* a trailing newline from copying a whole line: leave it out */
	while (p.len && p.buf[p.len - 1] == '\n')
		p.len--;
	if (p.len)
		ins(E.cur, p.buf, p.len);
	sb_free(&p);
}

static char *finish(bool keep)
{
	E.no_ghost = true;
	E.cur = E.b.len;
	refresh();
	out("\n");
	raw_off();
	E.no_ghost = false;
	char *line = keep ? xstrdup(E.b.buf ? E.b.buf : "") : NULL;
	return line;
}

static void last_arg(void)
{
	size_t n = hist_count();
	if (!n)
		return;
	const char *h = hist_at(n - 1);
	size_t len = strlen(h);
	while (len && is_space(h[len - 1]))
		len--;
	size_t start = len;
	while (start && !is_space(h[start - 1]))
		start--;
	ins(E.cur, h + start, len - start);
}

char *edit_line(const char *prompt, bool continuation)
{
	edit_interrupted = false;
	if (!isatty(0) || !isatty(2))
		return plain_read(prompt);
	free(E.prompt);
	free(E.prefix_lines);
	const char *nl = strrchr(prompt, '\n');
	E.prefix_lines = nl ? xstrndup(prompt, (size_t)(nl - prompt + 1)) : xstrdup("");
	E.prompt = xstrdup(nl ? nl + 1 : prompt);
	strip_markers(E.prefix_lines);
	strip_markers(E.prompt);
	E.pw = vis_width(E.prompt, strlen(E.prompt));
	E.b.len = 0;
	sb_add(&E.b, "", 0);
	E.cur = 0;
	E.rows_up = 0;
	E.hpos = -1;
	E.menu = E.search = false;
	(void)continuation;
	if (!raw_on())
		return plain_read(prompt);
	if (*E.prefix_lines) {
		/* "\n" needs \r\n in raw mode output? OPOST is still on */
		out(E.prefix_lines);
	}
	refresh();
	for (;;) {
		int k = read_key();
		if (k == K_WINCH) {
			winch = 0;
			refresh();
			continue;
		}
		if (k == K_EOF) {
			char *l = finish(E.b.len > 0);
			return l;
		}
		if (k < 0)
			continue;
		if (E.menu) {
			bool done = true;
			if (menu_key(k, &done)) {
				refresh();
				continue;
			}
		}
		if (E.search) {
			if (k == CTRL('r')) {
				search_run(E.spos - 1);
				refresh();
				continue;
			}
			if (k == K_ESC || k == CTRL('g') || k == CTRL('c')) {
				search_end(false);
				refresh();
				continue;
			}
			if (k == 127 || k == CTRL('h')) {
				if (E.sq.len) {
					size_t l = E.sq.len - 1;
					while (l && ((unsigned char)E.sq.buf[l] & 0xc0) == 0x80)
						l--;
					E.sq.len = l;
					E.sq.buf[l] = '\0';
				}
				search_run((long)hist_count() - 1);
				refresh();
				continue;
			}
			if (k >= 0x20 && k < 0x100 && k != 127) {
				sb_addc(&E.sq, (char)k);
				search_run(E.spos >= 0 ? E.spos : (long)hist_count() - 1);
				refresh();
				continue;
			}
			search_end(true); /* any other key: keep the match, then the key */
		}
		if (k != K_UP && k != K_DOWN && k != CTRL('p') && k != CTRL('n'))
			hist_reset();
		switch (k) {
		case '\r':
		case '\n':
			return finish(true);
		case CTRL('c'): {
			E.no_ghost = true;
			E.cur = E.b.len;
			refresh();
			out(use_color ? "\033[90m^C\033[0m\n" : "^C\n");
			raw_off();
			E.no_ghost = false;
			edit_interrupted = true;
			return xstrdup("");
		}
		case CTRL('d'):
			if (!E.b.len) {
				finish(false);
				return NULL;
			}
			del(E.cur, next_char(E.cur));
			break;
		case 127:
		case CTRL('h'):
			del(prev_char(E.cur), E.cur);
			break;
		case K_DEL:
			del(E.cur, next_char(E.cur));
			break;
		case K_LEFT:
		case CTRL('b'):
			E.cur = prev_char(E.cur);
			break;
		case K_RIGHT:
		case CTRL('f'):
		case K_END:
		case CTRL('e'): {
			const char *g = ghost();
			if (g && (k == K_RIGHT || k == CTRL('f') || k == K_END || k == CTRL('e')))
				ins(E.b.len, g, strlen(g)); /* take the suggestion */
			else if (k == K_END || k == CTRL('e'))
				E.cur = E.b.len;
			else
				E.cur = next_char(E.cur);
			break;
		}
		case K_HOME:
		case CTRL('a'):
			E.cur = 0;
			break;
		case K_ALT | 'b':
		case K_CLEFT:
			E.cur = word_left(E.cur, k == (K_ALT | 'b'));
			break;
		case K_ALT | 'f':
		case K_CRIGHT: {
			const char *g = ghost();
			if (g) { /* take one word of the suggestion */
				size_t i = 0, gl = strlen(g);
				while (i < gl && is_space(g[i]))
					i++;
				while (i < gl && !is_space(g[i]))
					i++;
				ins(E.b.len, g, i);
			} else
				E.cur = word_right(E.cur, k == (K_ALT | 'f'));
			break;
		}
		case K_UP:
		case CTRL('p'):
			hist_move(-1);
			break;
		case K_DOWN:
		case CTRL('n'):
			hist_move(1);
			break;
		case K_PGUP:
			if (hist_count()) {
				hist_reset();
				set_line(hist_at(0));
			}
			break;
		case K_PGDN:
			hist_reset();
			break;
		case CTRL('k'):
			cut(E.cur, E.b.len);
			break;
		case CTRL('u'):
			cut(0, E.cur);
			break;
		case CTRL('w'):
			cut(word_left(E.cur, false), E.cur);
			break;
		case K_ALT | 127:
		case K_ALT | CTRL('h'):
			cut(word_left(E.cur, true), E.cur);
			break;
		case K_ALT | 'd':
			cut(E.cur, word_right(E.cur, true));
			break;
		case CTRL('y'):
			if (E.yank)
				ins(E.cur, E.yank, strlen(E.yank));
			break;
		case CTRL('t'):
			if (E.cur > 0 && E.b.len > 1) {
				if (E.cur == E.b.len)
					E.cur = prev_char(E.cur);
				size_t a = prev_char(E.cur), b = next_char(E.cur);
				char *first = xstrndup(E.b.buf + a, E.cur - a);
				char *second = xstrndup(E.b.buf + E.cur, b - E.cur);
				del(a, b);
				E.cur = a;
				ins(a, second, strlen(second));
				ins(E.cur, first, strlen(first));
				free(first);
				free(second);
			}
			break;
		case CTRL('l'):
			out("\033[H\033[2J");
			out(E.prefix_lines);
			E.rows_up = 0;
			break;
		case CTRL('r'):
			E.search = true;
			E.ssaved = xstrdup(E.b.buf ? E.b.buf : "");
			E.spos = (long)hist_count();
			E.sfail = false;
			break;
		case K_ALT | '.':
			last_arg();
			break;
		case '\t':
			do_complete();
			break;
		case K_PASTE:
			read_paste();
			break;
		case K_ESC:
			break;
		default:
			if (k >= 0x20 && k < 0x100 && k != 127) {
				char c[4] = {(char)k};
				size_t len = (size_t)utf8_len((unsigned char)k);
				for (size_t i = 1; i < len && i < 4; i++) {
					int b = read_byte(50);
					if (b < 0)
						break;
					c[i] = (char)b;
				}
				ins(E.cur, c, len);
			}
		}
		refresh();
	}
}
