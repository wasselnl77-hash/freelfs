/* fledit -- the FreeLFS text editor
 * SPDX-License-Identifier: MIT
 *
 * Tabs, syntax colours (syntax.c), line numbers, find and replace (plain or POSIX
 * regular expressions), go to line, auto-indent, bracket matching, UTF-8, a
 * clipboard, and the open files with their cursor positions remembered in
 * $XDG_STATE_HOME. Every tab is one flat buffer plus an index of line starts, so
 * a 10 MB file opens at once; only the lines in view are drawn and coloured.
 * Columns are fixed cells (IBM Plex Mono).
 *
 * Nothing runs unless something happens: no cursor blink, no timers except
 * the one that clears a status message.
 */
#include "syntax.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <flui.h>
#include <limits.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
	ID_TEXT = 1, ID_VSB, ID_FIND, ID_FNEXT, ID_FPREV, ID_FCLOSE, ID_REPL, ID_RONE, ID_RALL, ID_FCASE, ID_FREGEX,
	ID_FREPLACE, ID_NEWTAB, ID_DLG = 100, ID_TAB = 1000
};
enum { MAX_FILE = 512 * 1024 * 1024, MAX_HL = 16 * 1024 * 1024, HL_LINE = 20000, MAX_TABS = 64 };
enum { TAB_W = 4, TAB_H = 36, FIND_H = 44, STATUS_H = 28 };
enum { OV_NONE, OV_UNSAVED, OV_PATH, OV_OVERWRITE, OV_ERROR, OV_GOTO };
enum { P_NONE, P_NEW, P_OPEN, P_QUIT, P_CLOSE };
#define NO_SEL ((size_t)-1)

struct op {
	size_t pos, dlen, ilen, cur_before, cur_after;
	char *del, *ins;
};

/* one tab */
struct Doc {
	char *b;
	size_t len, cap;
	size_t *ls, nl, lcap;
	size_t cur, anchor;
	int want_col;
	char path[PATH_MAX];
	int crlf, bom, bad_utf8;
	mode_t mode;
	int untitled; /* number for the tab name when there is no path */

	/* undo */
	struct op *undo, *redo;
	size_t nundo, cundo, nredo, credo;
	long saved_top; /* nundo when saved, -1 if that state is gone */
	int no_merge;

	/* view */
	int vscroll, hscroll;

	/* syntax: the state at the start of each line is known for lines 0 to hs_n - 1 */
	sx_lang lang;
	sx_state *hs;
	size_t hs_n, hs_cap;
	struct sx_ctx sx;
	int indent_w; /* 0: tabs, else that many spaces */
	unsigned long rev; /* changes with every edit (the match count of find caches on it) */
};

struct E {
	fl_app *app;
	fl_win *win;
	fl_font *mono, *bold, *num, *small;
	fl_timer *msg_timer, *sess_timer;
	int cell, lh;
	int sess_armed, last_tab;
	struct Doc *last_doc;
	size_t last_cur;

	/* tabs: d is tabs[cur_tab] */
	struct Doc *d;
	struct Doc *tabs[MAX_TABS];
	int ntabs, cur_tab, tab_scroll, tab_reveal;
	int primary; /* started without files: this window owns the saved list of open files */

	/* view */
	int reveal, dragging, center;
	char last_title[PATH_MAX + 8];

	/* find and replace */
	int find_open, replace_open, find_focus_req, find_miss;
	int f_case, f_regex;
	char needle[256], repl[256];
	regex_t re;
	int re_ok, re_bad; /* re compiled for re_key / the pattern is not valid */
	char re_key[300];
	long count; /* matches in the document, -1 unknown; cached for (count_rev, count_key) */
	unsigned long count_rev;
	char count_key[300];

	/* overlays */
	int ov, pending, path_mode, after; /* path_mode: 1 open, 2 save as */
	char dlg_text[PATH_MAX], dlg_err[200];
	int dlg_new;

	char msg[300];
	int msg_err;
};

/* ---- messages ----------------------------------------------------------------------------- */

static void msg_clear(void *ud)
{
	struct E *e = ud;
	e->msg[0] = 0;
	fl_timer_arm(e->msg_timer, 0, 0);
	fl_win_redraw(e->win);
}

static void say(struct E *e, int err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(e->msg, sizeof e->msg, fmt, ap);
	va_end(ap);
	e->msg_err = err;
	fl_timer_arm(e->msg_timer, 6000, 0);
}

/* ---- UTF-8 ------------------------------------------------------------------------------------ */

/* Decode one code point at s (before end). Bad bytes give U+FFFD and length 1. */
static int u8dec(const char *s, const char *end, uint32_t *cp)
{
	const unsigned char *p = (const unsigned char *)s;
	size_t avail = (size_t)(end - s);
	unsigned c = p[0];
	if (c < 0x80) {
		*cp = c;
		return 1;
	}
	int n = c >= 0xf0 && c < 0xf5 ? 4 : c >= 0xe0 ? 3 : c >= 0xc2 && c < 0xe0 ? 2 : 0;
	if (c >= 0xf5)
		n = 0;
	if (!n || (size_t)n > avail)
		goto bad;
	uint32_t v = c & (0xff >> (n + 1));
	for (int i = 1; i < n; i++) {
		if ((p[i] & 0xc0) != 0x80)
			goto bad;
		v = v << 6 | (p[i] & 0x3f);
	}
	if ((n == 3 && v < 0x800) || (n == 4 && (v < 0x10000 || v > 0x10ffff)) || (v >= 0xd800 && v < 0xe000))
		goto bad;
	*cp = v;
	return n;
bad:
	*cp = 0xfffd;
	return 1;
}

static int cp_width(uint32_t cp)
{
	if (cp < 0x300)
		return 1;
	if ((cp >= 0x300 && cp <= 0x36f) || (cp >= 0x200b && cp <= 0x200f) || (cp >= 0xfe00 && cp <= 0xfe0f))
		return 0;
	if ((cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0xa4cf) || (cp >= 0xac00 && cp <= 0xd7a3) ||
	    (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xfe30 && cp <= 0xfe4f) || (cp >= 0xff00 && cp <= 0xff60) ||
	    (cp >= 0xffe0 && cp <= 0xffe6) || (cp >= 0x1f300 && cp <= 0x1faff) || (cp >= 0x20000 && cp <= 0x3fffd))
		return 2;
	return 1;
}

static int is_valid_utf8(const char *s, size_t n)
{
	const char *end = s + n;
	while (s < end) {
		uint32_t cp;
		int l = u8dec(s, end, &cp);
		if (cp == 0xfffd && l == 1 && (unsigned char)*s != 0xef)
			return 0;
		s += l;
	}
	return 1;
}

/* start of the character before pos */
static size_t prev_char(const struct E *e, size_t pos)
{
	if (!pos)
		return 0;
	size_t p = pos - 1;
	int k = 0;
	while (p > 0 && ((unsigned char)e->d->b[p] & 0xc0) == 0x80 && k < 3) {
		p--;
		k++;
	}
	uint32_t cp;
	if (p + (size_t)u8dec(e->d->b + p, e->d->b + e->d->len, &cp) == pos)
		return p;
	return pos - 1;
}

static size_t next_char(const struct E *e, size_t pos)
{
	if (pos >= e->d->len)
		return e->d->len;
	uint32_t cp;
	return pos + (size_t)u8dec(e->d->b + pos, e->d->b + e->d->len, &cp);
}

/* ---- lines ------------------------------------------------------------------------------------- */

static size_t line_of(const struct E *e, size_t pos)
{
	size_t lo = 0, hi = e->d->nl;
	while (hi - lo > 1) {
		size_t mid = (lo + hi) / 2;
		if (e->d->ls[mid] <= pos)
			lo = mid;
		else
			hi = mid;
	}
	return lo;
}

static size_t line_end(const struct E *e, size_t l)
{
	return l + 1 < e->d->nl ? e->d->ls[l + 1] - 1 : e->d->len;
}

static int index_grow(struct E *e, size_t need)
{
	if (need <= e->d->lcap)
		return 0;
	size_t nc = e->d->lcap ? e->d->lcap : 1024;
	while (nc < need)
		nc *= 2;
	size_t *n = realloc(e->d->ls, nc * sizeof *n);
	if (!n)
		return -1;
	e->d->ls = n;
	e->d->lcap = nc;
	return 0;
}

/* rebuild the line index from line l on */
static void reindex_from(struct E *e, size_t l)
{
	if (index_grow(e, l + 2) < 0)
		return;
	e->d->nl = l + 1;
	size_t p = e->d->ls[l];
	while (p < e->d->len) {
		const char *q = memchr(e->d->b + p, '\n', e->d->len - p);
		if (!q)
			break;
		p = (size_t)(q - e->d->b) + 1;
		if (index_grow(e, e->d->nl + 1) < 0)
			return;
		e->d->ls[e->d->nl++] = p;
	}
}

static int col_of(const struct E *e, size_t l, size_t off)
{
	const char *s = e->d->b + e->d->ls[l], *end = e->d->b + line_end(e, l), *stop = e->d->b + off;
	int col = 0;
	while (s < end && s < stop) {
		if (*s == '\t') {
			col = (col / TAB_W + 1) * TAB_W;
			s++;
			continue;
		}
		uint32_t cp;
		s += u8dec(s, end, &cp);
		if (cp < 0x20 || cp == 0x7f)
			continue;
		col += cp_width(cp);
	}
	return col;
}

/* the offset in line l nearest to column col */
static size_t off_of(const struct E *e, size_t l, int col)
{
	const char *s = e->d->b + e->d->ls[l], *end = e->d->b + line_end(e, l);
	int c = 0;
	while (s < end) {
		int w;
		uint32_t cp;
		int n;
		if (*s == '\t') {
			w = (c / TAB_W + 1) * TAB_W - c;
			n = 1;
		} else {
			n = u8dec(s, end, &cp);
			w = cp < 0x20 || cp == 0x7f ? 0 : cp_width(cp);
		}
		if (col <= c + (w > 0 ? (w - 1) / 2 : 0))
			break;
		c += w;
		s += n;
	}
	return (size_t)(s - e->d->b);
}

/* ---- syntax and indentation ------------------------------------------------------------------ */

/* the colour state at the start of line ln; lines are lexed forward from the last known one */
static sx_state hs_at(struct E *e, size_t ln)
{
	struct Doc *d = e->d;
	if (!d->lang)
		return 0;
	if (ln >= d->nl)
		ln = d->nl - 1;
	if (ln >= d->hs_n) {
		if (d->hs_cap < d->nl + 1) {
			size_t nc = d->hs_cap ? d->hs_cap : 1024;
			while (nc < d->nl + 1)
				nc *= 2;
			sx_state *n = realloc(d->hs, nc * sizeof *n);
			if (!n)
				return 0;
			d->hs = n;
			d->hs_cap = nc;
		}
		if (d->hs_n == 0) {
			d->hs[0] = 0;
			d->hs_n = 1;
		}
		size_t i = d->hs_n - 1;
		sx_state st = d->hs[i];
		for (; i < ln; i++) {
			st = sx_line(&d->sx, d->lang, st, d->b + d->ls[i], line_end(e, i) - d->ls[i], NULL);
			d->hs[i + 1] = st;
		}
		d->hs_n = ln + 1;
	}
	return d->hs[ln];
}

/* Colour classes of line ln into cls (room for HL_LINE bytes); NULL when the line is too long
 * or there is no language. */
static uint8_t *line_classes(struct E *e, size_t ln, uint8_t *cls)
{
	struct Doc *d = e->d;
	size_t n = line_end(e, ln) - d->ls[ln];
	if (!d->lang || n > HL_LINE)
		return NULL;
	sx_line(&d->sx, d->lang, hs_at(e, ln), d->b + d->ls[ln], n, cls);
	return cls;
}

/* Tabs or spaces: what the first lines of the file use, else the language's habit. */
static void detect_indent(struct E *e)
{
	struct Doc *d = e->d;
	int tabs = 0, spaces = 0, hist[9] = {0}, prev = 0;
	size_t lim = d->nl < 800 ? d->nl : 800;
	for (size_t l = 0; l < lim; l++) {
		const char *s = d->b + d->ls[l];
		size_t n = line_end(e, l) - d->ls[l];
		if (!n)
			continue;
		if (s[0] == '\t') {
			tabs++;
			continue;
		}
		size_t w = 0;
		while (w < n && s[w] == ' ')
			w++;
		if (w == n)
			continue;
		if (w >= 2) {
			spaces++;
			int diff = (int)w > prev ? (int)w - prev : prev - (int)w;
			if (diff >= 2 && diff <= 8)
				hist[diff]++;
		}
		prev = (int)w;
	}
	if (tabs > spaces) {
		d->indent_w = 0;
	} else if (spaces >= 3) {
		int best = 4;
		for (int u = 2; u <= 8; u++)
			if (hist[u] > hist[best] || (hist[u] == hist[best] && u == 4))
				best = u;
		d->indent_w = best;
	} else {
		switch (d->lang) {
		case L_PY: d->indent_w = 4; break;
		case L_JS: case L_HTML: case L_CSS: case L_DSSL: case L_MD: case L_CONF: d->indent_w = 2; break;
		default: d->indent_w = 0; break;
		}
	}
}

static void doc_detect(struct E *e)
{
	struct Doc *d = e->d;
	sx_lang l = d->len > MAX_HL ? L_NONE : sx_detect(d->path, d->b, line_end(e, 0));
	if (l != d->lang || !d->hs_n) {
		d->lang = l;
		d->hs_n = 0;
		memset(&d->sx, 0, sizeof d->sx);
	}
	detect_indent(e);
}

/* ---- the buffer and undo --------------------------------------------------------------------- */

static int buf_reserve(struct E *e, size_t need)
{
	if (need + 1 <= e->d->cap)
		return 0;
	size_t nc = e->d->cap ? e->d->cap : 4096;
	while (nc < need + 1)
		nc *= 2;
	char *nb = realloc(e->d->b, nc);
	if (!nb)
		return -1;
	e->d->b = nb;
	e->d->cap = nc;
	return 0;
}

static void clear_redo(struct E *e)
{
	for (size_t i = 0; i < e->d->nredo; i++) {
		free(e->d->redo[i].del);
		free(e->d->redo[i].ins);
	}
	e->d->nredo = 0;
}

static void undo_push(struct E *e, struct op *o)
{
	if (e->d->nundo == e->d->cundo) {
		size_t nc = e->d->cundo ? e->d->cundo * 2 : 256;
		struct op *n = realloc(e->d->undo, nc * sizeof *n);
		if (!n) {
			free(o->del);
			free(o->ins);
			return;
		}
		e->d->undo = n;
		e->d->cundo = nc;
	}
	e->d->undo[e->d->nundo++] = *o;
}

/* Replace dlen bytes at pos with ins. The one place the text changes. */
static int do_replace(struct E *e, size_t pos, size_t dlen, const char *ins, size_t ilen, size_t cur_after,
                      int record)
{
	if (pos > e->d->len)
		pos = e->d->len;
	if (dlen > e->d->len - pos)
		dlen = e->d->len - pos;
	if (e->d->len - dlen + ilen > MAX_FILE) {
		say(e, 1, "The document is too large");
		return -1;
	}
	char *del = NULL;
	if (record && dlen) {
		del = malloc(dlen);
		if (!del)
			return -1;
		memcpy(del, e->d->b + pos, dlen);
	}
	char *dup = NULL;
	if (record && ilen) {
		dup = malloc(ilen);
		if (!dup) {
			free(del);
			return -1;
		}
		memcpy(dup, ins, ilen);
	}
	if (buf_reserve(e, e->d->len - dlen + ilen) < 0) {
		free(del);
		free(dup);
		return -1;
	}
	int del_nl = dlen && memchr(e->d->b + pos, '\n', dlen) != NULL;
	int ins_nl = ilen && memchr(ins, '\n', ilen) != NULL;
	size_t L = line_of(e, pos);
	e->d->rev++;
	if (e->d->hs_n > L + 1)
		e->d->hs_n = L + 1; /* the colour state of the lines from L on is stale */
	memmove(e->d->b + pos + ilen, e->d->b + pos + dlen, e->d->len - pos - dlen);
	if (ilen)
		memcpy(e->d->b + pos, ins, ilen);
	e->d->len = e->d->len - dlen + ilen;
	e->d->b[e->d->len] = 0;
	if (!del_nl && !ins_nl) {
		for (size_t i = L + 1; i < e->d->nl; i++)
			e->d->ls[i] = e->d->ls[i] - dlen + ilen;
	} else {
		reindex_from(e, L);
	}
	if (record) {
		clear_redo(e);
		if ((long)e->d->nundo < e->d->saved_top)
			e->d->saved_top = -1; /* the saved state can no longer be reached */
		struct op o = {pos, dlen, ilen, e->d->cur, cur_after, del, dup};
		struct op *prev = e->d->nundo ? &e->d->undo[e->d->nundo - 1] : NULL;
		int merged = 0;
		if (prev && !e->d->no_merge) {
			if (!prev->dlen && !dlen && ilen && prev->pos + prev->ilen == pos && ilen < 8 &&
			    !memchr(ins, '\n', ilen) && prev->ilen < 400) {
				char *n = realloc(prev->ins, prev->ilen + ilen);
				if (n) {
					memcpy(n + prev->ilen, ins, ilen);
					prev->ins = n;
					prev->ilen += ilen;
					prev->cur_after = cur_after;
					merged = 1;
				}
			} else if (!prev->ilen && !ilen && dlen && dlen < 8 && prev->dlen < 400) {
				if (prev->pos == pos + dlen) { /* backspacing */
					char *n = malloc(prev->dlen + dlen);
					if (n) {
						memcpy(n, del, dlen);
						memcpy(n + dlen, prev->del, prev->dlen);
						free(prev->del);
						prev->del = n;
						prev->dlen += dlen;
						prev->pos = pos;
						prev->cur_after = cur_after;
						merged = 1;
					}
				} else if (prev->pos == pos) { /* Delete key */
					char *n = realloc(prev->del, prev->dlen + dlen);
					if (n) {
						memcpy(n + prev->dlen, del, dlen);
						prev->del = n;
						prev->dlen += dlen;
						prev->cur_after = cur_after;
						merged = 1;
					}
				}
			}
		}
		if (merged) {
			free(del);
			free(dup);
		} else {
			undo_push(e, &o);
		}
		e->d->no_merge = 0;
	}
	e->d->cur = cur_after;
	e->d->anchor = NO_SEL;
	e->d->want_col = -1;
	e->reveal = 1;
	e->find_miss = 0;
	return 0;
}

static int modified_doc(const struct Doc *d)
{
	return d->saved_top < 0 || (long)d->nundo != d->saved_top;
}

static int modified(const struct E *e)
{
	return modified_doc(e->d);
}

static void do_undo(struct E *e)
{
	if (!e->d->nundo) {
		return;
	}
	struct op o = e->d->undo[--e->d->nundo];
	size_t save_cur = e->d->cur;
	(void)save_cur;
	do_replace(e, o.pos, o.ilen, o.del, o.dlen, o.cur_before, 0);
	if (e->d->nredo == e->d->credo) {
		size_t nc = e->d->credo ? e->d->credo * 2 : 64;
		struct op *n = realloc(e->d->redo, nc * sizeof *n);
		if (!n) {
			free(o.del);
			free(o.ins);
			return;
		}
		e->d->redo = n;
		e->d->credo = nc;
	}
	e->d->redo[e->d->nredo++] = o;
	e->d->no_merge = 1;
}

static void do_redo(struct E *e)
{
	if (!e->d->nredo)
		return;
	struct op o = e->d->redo[--e->d->nredo];
	do_replace(e, o.pos, o.dlen, o.ins, o.ilen, o.cur_after, 0);
	undo_push(e, &o);
	e->d->no_merge = 1;
}

static void free_undo(struct E *e)
{
	for (size_t i = 0; i < e->d->nundo; i++) {
		free(e->d->undo[i].del);
		free(e->d->undo[i].ins);
	}
	e->d->nundo = 0;
	clear_redo(e);
}

/* ---- selection and editing commands --------------------------------------------------------------- */

static int has_sel(const struct E *e)
{
	return e->d->anchor != NO_SEL && e->d->anchor != e->d->cur;
}

static void sel_range(const struct E *e, size_t *a, size_t *b)
{
	*a = e->d->anchor < e->d->cur ? e->d->anchor : e->d->cur;
	*b = e->d->anchor < e->d->cur ? e->d->cur : e->d->anchor;
}

static void insert_text(struct E *e, const char *t, size_t n)
{
	size_t a = e->d->cur, b = e->d->cur;
	if (has_sel(e))
		sel_range(e, &a, &b);
	do_replace(e, a, b - a, t, n, a + n, 1);
}

static void delete_sel(struct E *e)
{
	size_t a, b;
	sel_range(e, &a, &b);
	do_replace(e, a, b - a, "", 0, a, 1);
}

static void move_to(struct E *e, size_t pos, int extend, int keep_col)
{
	if (pos > e->d->len)
		pos = e->d->len;
	if (extend) {
		if (e->d->anchor == NO_SEL)
			e->d->anchor = e->d->cur;
	} else {
		e->d->anchor = NO_SEL;
	}
	e->d->cur = pos;
	if (!keep_col)
		e->d->want_col = -1;
	e->reveal = 1;
	e->d->no_merge = 1;
}

static int is_word(unsigned char c)
{
	return c >= 0x80 || c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static size_t word_left(const struct E *e, size_t p)
{
	while (p > 0 && !is_word((unsigned char)e->d->b[p - 1]) && e->d->b[p - 1] != '\n')
		p--;
	while (p > 0 && is_word((unsigned char)e->d->b[p - 1]))
		p--;
	if (p > 0 && p == e->d->cur && e->d->b[p - 1] == '\n')
		p--;
	return p;
}

static size_t word_right(const struct E *e, size_t p)
{
	while (p < e->d->len && !is_word((unsigned char)e->d->b[p]) && e->d->b[p] != '\n')
		p++;
	while (p < e->d->len && is_word((unsigned char)e->d->b[p]))
		p++;
	if (p < e->d->len && p == e->d->cur && e->d->b[p] == '\n')
		p++;
	return p;
}

static void move_vert(struct E *e, int dl, int extend)
{
	size_t l = line_of(e, e->d->cur);
	int col = e->d->want_col >= 0 ? e->d->want_col : col_of(e, l, e->d->cur);
	long nl = (long)l + dl;
	if (nl < 0) {
		move_to(e, 0, extend, 0);
		return;
	}
	if ((size_t)nl >= e->d->nl) {
		move_to(e, e->d->len, extend, 0);
		return;
	}
	move_to(e, off_of(e, (size_t)nl, col), extend, 1);
	e->d->want_col = col;
}

/* one level of indentation after the whitespace that ends at column col */
static size_t indent_unit(const struct E *e, char *out, int col)
{
	if (!e->d->indent_w) {
		out[0] = '\t';
		return 1;
	}
	int w = e->d->indent_w;
	int n = w - col % w;
	memset(out, ' ', (size_t)n);
	return (size_t)n;
}

static int is_blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* does the line s[0, n) end in the shell word w? */
static int ends_word(const char *s, size_t n, const char *w)
{
	size_t wl = strlen(w);
	if (n < wl || memcmp(s + n - wl, w, wl))
		return 0;
	return n == wl || !(s[n - wl - 1] == '_' || ((s[n - wl - 1] | 32) >= 'a' && (s[n - wl - 1] | 32) <= 'z'));
}

/* Markdown: the list marker (or "> ") of the line starting at ls, copied to out with a number
 * stepped on; returns its length, 0 for none. *empty tells that nothing follows the marker. */
static size_t md_marker(const struct E *e, size_t ls, size_t le, size_t upto, char *out, size_t cap, int *empty)
{
	const char *s = e->d->b + ls;
	size_t n = le - ls, i = 0;
	while (i < n && is_blank(s[i]))
		i++;
	size_t m = i, len = 0;
	if (i < n && s[i] == '>') {
		while (i < n && (s[i] == '>' || s[i] == ' '))
			i++;
		len = i - m;
	} else if (i + 1 < n && (s[i] == '-' || s[i] == '*' || s[i] == '+') && s[i + 1] == ' ') {
		i += 2;
		if (i + 2 < n && s[i] == '[' && s[i + 2] == ']' && (s[i + 1] == ' ' || s[i + 1] == 'x')) {
			i += 3;
			if (i < n && s[i] == ' ')
				i++;
		}
		len = i - m;
	} else if (i < n && s[i] >= '0' && s[i] <= '9') {
		size_t j = i;
		while (j < n && s[j] >= '0' && s[j] <= '9')
			j++;
		if (j + 1 < n && (s[j] == '.' || s[j] == ')') && s[j + 1] == ' ') {
			len = j + 2 - m;
			i = j + 2;
		}
	}
	if (!len || len + 12 >= cap)
		return 0;
	if (upto < ls + i)
		return 0; /* the cursor is inside the marker */
	size_t k = i;
	while (k < n && is_blank(s[k]))
		k++;
	*empty = k == n;
	if (s[m] >= '0' && s[m] <= '9') {
		long v = strtol(s + m, NULL, 10) + 1;
		int w = snprintf(out, cap, "%ld%c ", v, s[i - 2]);
		return (size_t)w;
	}
	size_t o = 0;
	for (size_t z = m; z < i && o < cap - 1; z++)
		out[o++] = s[z] == 'x' ? ' ' : s[z];
	return o;
}

static void newline(struct E *e)
{
	struct Doc *d = e->d;
	size_t a = d->cur, b = a;
	if (has_sel(e))
		sel_range(e, &a, &b);
	size_t l = line_of(e, a), ls = d->ls[l], le = line_end(e, l);
	char ins[512];
	size_t n = 0;
	ins[n++] = '\n';
	size_t k = ls;
	while (k < a && is_blank(d->b[k]) && n < 200)
		ins[n++] = d->b[k++];
	size_t ws_end = n;
	size_t p = a; /* the last byte of the line before the cursor that is not blank */
	while (p > ls && is_blank(d->b[p - 1]))
		p--;
	char last = p > ls ? d->b[p - 1] : 0;
	size_t bl = line_of(e, b), ble = line_end(e, bl), q = b;
	while (q < ble && is_blank(d->b[q]))
		q++;
	char next = q < ble ? d->b[q] : 0;

	if (d->lang == L_MD) {
		char mk[64];
		int empty = 0;
		size_t ml = md_marker(e, ls, le, a, mk, sizeof mk, &empty);
		if (ml && empty && a == le) { /* Enter on an empty item ends the list */
			do_replace(e, ls, a - ls, "", 0, ls, 1);
			return;
		}
		if (ml) {
			memcpy(ins + ws_end, mk, ml);
			n = ws_end + ml;
			do_replace(e, a, b - a, ins, n, a + n, 1);
			d->no_merge = 1;
			return;
		}
	}
	int extra = 0;
	switch (d->lang) {
	case L_C: case L_JS: case L_CSS: case L_DSSL:
		extra = last == '{' || last == '(' || last == '[';
		break;
	case L_PY:
		extra = last == ':';
		break;
	case L_SH:
		extra = last == '{' || last == '(' || ends_word(d->b + ls, p - ls, "then") || ends_word(d->b + ls, p - ls, "do") ||
		        ends_word(d->b + ls, p - ls, "else");
		break;
	case L_HTML:
		extra = last == '>' && p - ls >= 2 && d->b[p - 2] != '/' && d->b[p - 2] != '-' && d->b[p - 2] != '?' && next == '<' &&
		        q + 1 < ble && d->b[q + 1] == '/';
		break;
	default: break;
	}
	size_t cur_off = 0;
	if (extra) {
		/* the column after the copied whitespace */
		size_t c2 = 0;
		for (size_t z = 1; z < ws_end; z++)
			c2 = ins[z] == '\t' ? (c2 / TAB_W + 1) * TAB_W : c2 + 1;
		n += indent_unit(e, ins + n, (int)c2);
		cur_off = n;
		int pair = (last == '{' && next == '}') || (last == '(' && next == ')') || (last == '[' && next == ']') ||
		           (d->lang == L_HTML && next == '<');
		if (pair && n + ws_end < sizeof ins - 1) {
			ins[n++] = '\n';
			memcpy(ins + n, ins + 1, ws_end - 1);
			n += ws_end - 1;
			/* what is left of the line after the cursor stays where it is: after the closer's new line */
			size_t drop = q - b; /* blanks between the cursor and the closer go */
			do_replace(e, a, (b - a) + drop, ins, n, a + cur_off, 1);
			d->no_merge = 1;
			return;
		}
	}
	do_replace(e, a, b - a, ins, n, a + n, 1);
	d->no_merge = 1;
}

/* the text typed: a closing bracket on a line that holds only blanks steps back one level */
static void type_text(struct E *e, const char *t)
{
	struct Doc *d = e->d;
	size_t n = strlen(t);
	if (n == 1 && (t[0] == '}' || t[0] == ')' || t[0] == ']') && !has_sel(e) &&
	    (d->lang == L_C || d->lang == L_JS || d->lang == L_CSS || d->lang == L_DSSL)) {
		size_t ls = d->ls[line_of(e, d->cur)], k = ls;
		while (k < d->cur && is_blank(d->b[k]))
			k++;
		if (k == d->cur && k > ls) {
			size_t rem = 0;
			if (d->indent_w == 0) {
				rem = d->b[k - 1] == '\t';
			} else {
				while (rem < (size_t)d->indent_w && k - rem > ls && d->b[k - rem - 1] == ' ')
					rem++;
			}
			if (rem) {
				do_replace(e, d->cur - rem, rem, t, 1, d->cur - rem + 1, 1);
				return;
			}
		}
	}
	insert_text(e, t, n);
}

/* Indent (dir 1) or dedent (dir -1) the lines the selection touches, or the cursor's line. */
static void indent_lines(struct E *e, int dir)
{
	struct Doc *d = e->d;
	size_t a = d->cur, b = d->cur;
	int sel = has_sel(e);
	if (sel)
		sel_range(e, &a, &b);
	size_t l0 = line_of(e, a), l1 = line_of(e, b);
	if (sel && l1 > l0 && b == d->ls[l1])
		l1--;
	size_t start = d->ls[l0], end = line_end(e, l1);
	size_t w = d->indent_w ? (size_t)d->indent_w : 1;
	size_t cap = (end - start) + (l1 - l0 + 1) * w + 1;
	char *out = malloc(cap);
	if (!out)
		return;
	size_t o = 0;
	long cur_delta = 0;
	size_t cur_line = line_of(e, d->cur);
	for (size_t l = l0; l <= l1; l++) {
		const char *s = d->b + d->ls[l];
		size_t n = line_end(e, l) - d->ls[l], skip = 0;
		long delta = 0;
		if (dir > 0) {
			if (n) {
				if (d->indent_w) {
					memset(out + o, ' ', w);
				} else {
					out[o] = '\t';
				}
				o += w;
				delta = (long)w;
			}
		} else if (n && s[0] == '\t') {
			skip = 1;
			delta = -1;
		} else {
			size_t sp = 0, lim = d->indent_w ? w : TAB_W;
			while (sp < n && sp < lim && s[sp] == ' ')
				sp++;
			skip = sp;
			delta = -(long)sp;
		}
		memcpy(out + o, s + skip, n - skip);
		o += n - skip;
		if (l < l1)
			out[o++] = '\n';
		if (l == cur_line)
			cur_delta = delta;
	}
	if (dir < 0 && o == end - start && !memcmp(out, d->b + start, o)) {
		free(out);
		return;
	}
	size_t cur_after = d->cur;
	if (!sel) {
		long c = (long)d->cur + cur_delta;
		if (c < (long)start)
			c = (long)start;
		cur_after = (size_t)c;
	}
	do_replace(e, start, end - start, out, o, sel ? start + o : cur_after, 1);
	if (sel) {
		d->anchor = start;
		d->cur = start + o;
	}
	free(out);
	d->no_merge = 1;
}

static void copy_sel(struct E *e, int cut)
{
	if (!has_sel(e))
		return;
	size_t a, b;
	sel_range(e, &a, &b);
	fl_clipboard_set(e->app, e->d->b + a, b - a);
	if (cut)
		delete_sel(e);
}

static void paste(struct E *e)
{
	size_t n;
	char *clip = fl_clipboard_get(e->app, &n);
	if (!clip)
		return;
	/* the document is LF inside: drop CR of CRLF */
	size_t o = 0;
	for (size_t i = 0; i < n; i++) {
		if (clip[i] == '\r' && i + 1 < n && clip[i + 1] == '\n')
			continue;
		clip[o++] = clip[i];
	}
	if (o) {
		insert_text(e, clip, o);
		e->d->no_merge = 1;
	}
	free(clip);
}

/* ---- find and replace --------------------------------------------------------------------------- */

/* Compile the pattern when regular expressions are on and it changed. REG_NEWLINE makes ^ and $ mean
 * the line, so a line is matched on its own with the document's bytes around it as context. */
static void re_prepare(struct E *e)
{
	if (!e->f_regex)
		return;
	char key[300];
	snprintf(key, sizeof key, "%c%s", e->f_case ? 'c' : 'i', e->needle);
	if (!strcmp(key, e->re_key) && (e->re_ok || e->re_bad))
		return;
	if (e->re_ok) {
		regfree(&e->re);
		e->re_ok = 0;
	}
	snprintf(e->re_key, sizeof e->re_key, "%s", key);
	e->re_bad = 0;
	if (!e->needle[0])
		return;
	if (regcomp(&e->re, e->needle, REG_EXTENDED | REG_NEWLINE | (e->f_case ? 0 : REG_ICASE)) == 0)
		e->re_ok = 1;
	else
		e->re_bad = 1;
}

static int match_at(const struct E *e, size_t p, size_t le, const char *nd, size_t nl)
{
	if (p + nl > le)
		return 0;
	const char *b = e->d->b + p;
	if (e->f_case)
		return !memcmp(b, nd, nl);
	for (size_t i = 0; i < nl; i++) {
		unsigned char a = (unsigned char)b[i], c = (unsigned char)nd[i];
		if (a >= 'A' && a <= 'Z')
			a += 32;
		if (c >= 'A' && c <= 'Z')
			c += 32;
		if (a != c)
			return 0;
	}
	return 1;
}

/* The first match in line l that starts at or after position from (absolute); fills ms, me and, for
 * regular expressions, the groups in pm (absolute offsets). Empty matches are skipped. */
static int match_in_line(struct E *e, size_t l, size_t from, size_t *ms, size_t *me, regmatch_t *pm)
{
	struct Doc *d = e->d;
	size_t ls = d->ls[l], le = line_end(e, l);
	if (from < ls)
		from = ls;
	if (!e->needle[0])
		return 0;
	if (!e->f_regex) {
		size_t nl = strlen(e->needle);
		unsigned char c0 = (unsigned char)e->needle[0];
		int lo = c0 >= 'A' && c0 <= 'Z' ? c0 + 32 : c0;
		for (size_t p = from; p + nl <= le; p++) {
			unsigned char c = (unsigned char)d->b[p];
			if (!e->f_case && c >= 'A' && c <= 'Z')
				c += 32;
			if (c != (e->f_case ? c0 : (unsigned char)lo))
				continue;
			if (match_at(e, p, le, e->needle, nl)) {
				*ms = p;
				*me = p + nl;
				return 1;
			}
		}
		return 0;
	}
	if (!e->re_ok)
		return 0;
	for (size_t p = from; p <= le; p++) {
		regmatch_t m[10];
		m[0].rm_so = (regoff_t)p;
		m[0].rm_eo = (regoff_t)le;
		if (regexec(&e->re, d->b, 10, m, REG_STARTEND) != 0)
			return 0;
		if (m[0].rm_eo > m[0].rm_so) {
			*ms = (size_t)m[0].rm_so;
			*me = (size_t)m[0].rm_eo;
			if (pm)
				memcpy(pm, m, sizeof m);
			return 1;
		}
		p = (size_t)m[0].rm_so; /* an empty match: try one byte further */
	}
	return 0;
}

/* the last match in line l that starts before limit */
static int match_last_in_line(struct E *e, size_t l, size_t limit, size_t *ms, size_t *me)
{
	size_t p = e->d->ls[l], s, f;
	int found = 0;
	while (match_in_line(e, l, p, &s, &f, NULL) && s < limit) {
		*ms = s;
		*me = f;
		found = 1;
		p = s + 1;
	}
	return found;
}

/* The next match from position from (forward: at or after it, backward: starting before it);
 * wraps around the document. 0 when there is none. */
static int find_from(struct E *e, size_t from, int dir, size_t *ms, size_t *me)
{
	struct Doc *d = e->d;
	if (!e->needle[0] || !d->len)
		return 0;
	re_prepare(e);
	if (e->f_regex && !e->re_ok)
		return 0;
	size_t l0 = line_of(e, from);
	if (dir > 0) {
		for (size_t k = 0; k <= d->nl; k++) {
			size_t l = (l0 + k) % d->nl;
			if (match_in_line(e, l, k == 0 ? from : 0, ms, me, NULL))
				return 1;
		}
	} else {
		for (size_t k = 0; k <= d->nl; k++) {
			size_t l = (l0 + d->nl - k % d->nl) % d->nl;
			size_t lim = k == 0 ? from : (size_t)-1;
			if (match_last_in_line(e, l, lim, ms, me))
				return 1;
		}
	}
	return 0;
}

static void find_step(struct E *e, int dir)
{
	struct Doc *d = e->d;
	if (!e->needle[0])
		return;
	size_t a = d->cur, b = d->cur, ms, me;
	if (has_sel(e))
		sel_range(e, &a, &b);
	if (!find_from(e, dir > 0 ? (has_sel(e) ? a + 1 : d->cur) : a, dir, &ms, &me)) {
		e->find_miss = 1;
		return;
	}
	e->find_miss = 0;
	d->anchor = ms;
	d->cur = me;
	d->want_col = -1;
	e->reveal = 1;
}

/* how many matches there are (cached until the text, the pattern or the options change) */
static long count_matches(struct E *e)
{
	struct Doc *d = e->d;
	char key[300];
	snprintf(key, sizeof key, "%d%d%s", e->f_case, e->f_regex, e->needle);
	if (e->count >= 0 && e->count_rev == d->rev && !strcmp(key, e->count_key))
		return e->count;
	e->count_rev = d->rev;
	snprintf(e->count_key, sizeof e->count_key, "%s", key);
	e->count = 0;
	re_prepare(e);
	if (!e->needle[0] || (e->f_regex && !e->re_ok))
		return 0;
	for (size_t l = 0; l < d->nl && e->count < 100000; l++) {
		size_t p = d->ls[l], s, f;
		while (match_in_line(e, l, p, &s, &f, NULL) && e->count < 100000) {
			e->count++;
			p = f;
		}
	}
	return e->count;
}

struct sbuf {
	char *p;
	size_t n, cap;
};

static int sb_add(struct sbuf *sb, const char *s, size_t n)
{
	if (sb->n + n + 1 > sb->cap) {
		size_t nc = sb->cap ? sb->cap : 256;
		while (nc < sb->n + n + 1)
			nc *= 2;
		char *np = realloc(sb->p, nc);
		if (!np)
			return -1;
		sb->p = np;
		sb->cap = nc;
	}
	memcpy(sb->p + sb->n, s, n);
	sb->n += n;
	return 0;
}

/* the replacement text for a match: with regular expressions \1 to \9, \0, \n, \t and \\ work */
static int expand_repl(struct E *e, struct sbuf *sb, const regmatch_t *pm)
{
	const char *r = e->repl;
	if (!e->f_regex)
		return sb_add(sb, r, strlen(r));
	for (; *r; r++) {
		if (*r == '\\' && r[1]) {
			r++;
			if (*r >= '0' && *r <= '9') {
				const regmatch_t *g = &pm[*r - '0'];
				if (g->rm_so >= 0 && sb_add(sb, e->d->b + g->rm_so, (size_t)(g->rm_eo - g->rm_so)) < 0)
					return -1;
			} else {
				char c = *r == 'n' ? '\n' : *r == 't' ? '\t' : *r;
				if (sb_add(sb, &c, 1) < 0)
					return -1;
			}
		} else if (sb_add(sb, r, 1) < 0) {
			return -1;
		}
	}
	return 0;
}

static void replace_one(struct E *e)
{
	struct Doc *d = e->d;
	re_prepare(e);
	if (has_sel(e)) {
		size_t a, b, ms, me;
		regmatch_t pm[10];
		sel_range(e, &a, &b);
		size_t l = line_of(e, a);
		if (b <= line_end(e, l) && match_in_line(e, l, a, &ms, &me, pm) && ms == a && me == b) {
			struct sbuf sb = {0};
			if (expand_repl(e, &sb, pm) == 0)
				do_replace(e, a, b - a, sb.p ? sb.p : "", sb.n, a + sb.n, 1);
			free(sb.p);
			d->no_merge = 1;
		}
	}
	find_step(e, 1);
}

static void replace_all(struct E *e)
{
	struct Doc *d = e->d;
	re_prepare(e);
	if (!e->needle[0] || (e->f_regex && !e->re_ok))
		return;
	struct sbuf sb = {0};
	size_t first = 0, copied = 0, n = 0;
	for (size_t l = 0; l < d->nl; l++) {
		size_t p = d->ls[l], ms, me;
		regmatch_t pm[10];
		while (match_in_line(e, l, p, &ms, &me, pm)) {
			if (!n)
				first = copied = ms;
			if (sb_add(&sb, d->b + copied, ms - copied) < 0 || expand_repl(e, &sb, pm) < 0) {
				free(sb.p);
				say(e, 1, "Out of memory");
				return;
			}
			copied = p = me;
			n++;
		}
	}
	if (!n) {
		e->find_miss = 1;
		free(sb.p);
		return;
	}
	do_replace(e, first, copied - first, sb.p ? sb.p : "", sb.n, first + sb.n, 1);
	d->no_merge = 1;
	free(sb.p);
	say(e, 0, "Replaced %zu match%s", n, n == 1 ? "" : "es");
}

/* ---- load and save -------------------------------------------------------------------------------- */

static void doc_reset(struct E *e)
{
	e->d->len = 0;
	if (e->d->b)
		e->d->b[0] = 0;
	if (index_grow(e, 2) == 0) {
		e->d->ls[0] = 0;
		e->d->nl = 1;
	}
	e->d->cur = 0;
	e->d->anchor = NO_SEL;
	e->d->want_col = -1;
	e->d->vscroll = e->d->hscroll = 0;
	e->d->crlf = e->d->bom = e->d->bad_utf8 = 0;
	e->d->mode = 0;
	e->d->path[0] = 0;
	free_undo(e);
	e->d->saved_top = 0;
	e->d->no_merge = 1;
	e->d->lang = L_NONE;
	e->d->hs_n = 0;
	memset(&e->d->sx, 0, sizeof e->d->sx);
	e->d->indent_w = 0;
	e->d->rev++;
	e->find_miss = 0;
}

static int load_file(struct E *e, const char *path, char *err, size_t ecap)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		snprintf(err, ecap, "Cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	struct stat st;
	if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
		snprintf(err, ecap, "%s is not a regular file", path);
		close(fd);
		return -1;
	}
	if (st.st_size > MAX_FILE) {
		snprintf(err, ecap, "%s is too large (%lld MB)", path, (long long)st.st_size >> 20);
		close(fd);
		return -1;
	}
	size_t size = (size_t)st.st_size;
	char *nb = malloc(size + 2);
	if (!nb) {
		snprintf(err, ecap, "Out of memory");
		close(fd);
		return -1;
	}
	size_t got = 0;
	while (got < size) {
		ssize_t n = read(fd, nb + got, size - got);
		if (n < 0) {
			snprintf(err, ecap, "Cannot read %s: %s", path, strerror(errno));
			free(nb);
			close(fd);
			return -1;
		}
		if (n == 0)
			break;
		got += (size_t)n;
	}
	close(fd);
	size = got;
	if (memchr(nb, 0, size)) {
		snprintf(err, ecap, "%s looks like a binary file", path);
		free(nb);
		return -1;
	}
	doc_reset(e);
	free(e->d->b);
	e->d->b = nb;
	e->d->cap = size + 2;
	size_t off = 0;
	if (size >= 3 && !memcmp(nb, "\xef\xbb\xbf", 3)) {
		e->d->bom = 1;
		off = 3;
	}
	if (off) {
		memmove(nb, nb + off, size - off);
		size -= off;
	}
	/* CRLF only when every line ends that way */
	size_t nlf = 0, ncrlf = 0;
	for (size_t i = 0; i < size; i++)
		if (nb[i] == '\n') {
			nlf++;
			if (i && nb[i - 1] == '\r')
				ncrlf++;
		}
	if (nlf && nlf == ncrlf) {
		e->d->crlf = 1;
		size_t o = 0;
		for (size_t i = 0; i < size; i++) {
			if (nb[i] == '\r' && i + 1 < size && nb[i + 1] == '\n')
				continue;
			nb[o++] = nb[i];
		}
		size = o;
	}
	nb[size] = 0;
	e->d->len = size;
	e->d->ls[0] = 0;
	e->d->nl = 1;
	reindex_from(e, 0);
	e->d->bad_utf8 = !is_valid_utf8(nb, size);
	e->d->mode = st.st_mode & 07777;
	snprintf(e->d->path, sizeof e->d->path, "%s", path);
	doc_detect(e);
	return 0;
}

static int write_all(int fd, const char *p, size_t n)
{
	while (n) {
		ssize_t w = write(fd, p, n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

static int write_doc(struct E *e, int fd)
{
	if (e->d->bom && write_all(fd, "\xef\xbb\xbf", 3) < 0)
		return -1;
	if (!e->d->crlf)
		return write_all(fd, e->d->b, e->d->len);
	size_t s = 0;
	while (s < e->d->len) {
		const char *q = memchr(e->d->b + s, '\n', e->d->len - s);
		size_t n = q ? (size_t)(q - e->d->b) - s : e->d->len - s;
		if (write_all(fd, e->d->b + s, n) < 0)
			return -1;
		if (!q)
			break;
		if (write_all(fd, "\r\n", 2) < 0)
			return -1;
		s += n + 1;
	}
	return 0;
}

static int save_to(struct E *e, const char *path, char *err, size_t ecap)
{
	char target[PATH_MAX];
	if (!realpath(path, target)) /* a new file: keep the name as given */
		snprintf(target, sizeof target, "%s", path);
	char dir[PATH_MAX], tmp[PATH_MAX + 32];
	snprintf(dir, sizeof dir, "%s", target);
	char *sl = strrchr(dir, '/');
	if (sl && sl != dir)
		*sl = 0;
	else if (sl)
		dir[1] = 0;
	else
		snprintf(dir, sizeof dir, ".");
	mode_t mode = e->d->mode;
	if (!mode) {
		mode_t um = umask(0);
		umask(um);
		mode = 0666 & ~um;
	}
	struct stat st;
	if (stat(target, &st) == 0)
		mode = st.st_mode & 07777;
	/* write a temporary file next to the target, then rename it over: a crash
	 * or a full disk never leaves half a file behind */
	snprintf(tmp, sizeof tmp, "%s/.fledit-XXXXXX", dir);
	int fd = mkstemp(tmp);
	if (fd >= 0) {
		fchmod(fd, mode);
		if (write_doc(e, fd) < 0 || fsync(fd) < 0) {
			snprintf(err, ecap, "Cannot write %s: %s", path, strerror(errno));
			close(fd);
			unlink(tmp);
			return -1;
		}
		if (close(fd) < 0 || rename(tmp, target) < 0) {
			snprintf(err, ecap, "Cannot write %s: %s", path, strerror(errno));
			unlink(tmp);
			return -1;
		}
	} else {
		/* no room for a temporary file (a read-only folder): write in place */
		fd = open(target, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
		if (fd < 0) {
			snprintf(err, ecap, "Cannot save %s: %s", path, strerror(errno));
			return -1;
		}
		if (write_doc(e, fd) < 0 || close(fd) < 0) {
			snprintf(err, ecap, "Cannot write %s: %s", path, strerror(errno));
			return -1;
		}
	}
	snprintf(e->d->path, sizeof e->d->path, "%s", target);
	e->d->mode = mode;
	e->d->saved_top = (long)e->d->nundo;
	e->d->no_merge = 1;
	if (sx_detect(target, e->d->b, line_end(e, 0)) != e->d->lang || !e->d->hs_n)
		doc_detect(e); /* a new name can mean a new language */
	return 0;
}

/* ---- actions and dialogs ------------------------------------------------------------------------------ */

static void open_path_dialog(struct E *e, int mode, int after)
{
	e->ov = OV_PATH;
	e->path_mode = mode;
	e->after = after;
	e->dlg_err[0] = 0;
	e->dlg_new = 1;
	if (e->d->path[0] && mode == 2) {
		snprintf(e->dlg_text, sizeof e->dlg_text, "%s", e->d->path);
	} else {
		char cwd[PATH_MAX];
		const char *dir = NULL;
		if (e->d->path[0]) {
			snprintf(cwd, sizeof cwd, "%s", e->d->path);
			char *s = strrchr(cwd, '/');
			if (s)
				s[1] = 0;
			dir = cwd;
		} else if (getcwd(cwd, sizeof cwd) && access(cwd, W_OK | X_OK) == 0) {
			size_t l = strlen(cwd);
			if (l && cwd[l - 1] != '/' && l + 1 < sizeof cwd)
				strcat(cwd, "/");
			dir = cwd;
		} else {	/* the folder is gone, not writable (an ejected drive, "/"): Home */
			const char *home = getenv("HOME");
			if (home && *home) {
				snprintf(cwd, sizeof cwd, "%s/", home);
				dir = cwd;
			}
		}
		snprintf(e->dlg_text, sizeof e->dlg_text, "%s%s", dir ? dir : "", mode == 2 ? "untitled.txt" : "");
	}
}

/* ---- tabs ------------------------------------------------------------------------------------------ */

static void expand_home(const char *in, char *out, size_t cap);

static struct Doc *doc_alloc(struct E *e)
{
	static int untitled;
	struct Doc *d = calloc(1, sizeof *d);
	if (!d)
		return NULL;
	struct Doc *sv = e->d;
	e->d = d;
	doc_reset(e);
	e->d = sv;
	d->untitled = ++untitled;
	return d;
}

static void doc_free(struct E *e, struct Doc *d)
{
	struct Doc *sv = e->d;
	e->d = d;
	free_undo(e);
	e->d = sv;
	free(d->undo);
	free(d->redo);
	free(d->b);
	free(d->ls);
	free(d->hs);
	free(d);
}

/* where the cursor and the scroll position of files were left (most recent first) */
struct pos {
	char *path;
	size_t line, off, top;
};
static struct pos *pos_tab;
static int npos;
enum { MAX_POS = 100 };

static char **sess_paths; /* the files that were open, from the session file */
static int nsess, sess_active;

static void pos_note(struct E *e, struct Doc *d)
{
	if (!d->path[0])
		return;
	struct Doc *sv = e->d;
	e->d = d;
	size_t line = line_of(e, d->cur);
	size_t off = d->cur - d->ls[line];
	size_t top = e->lh ? (size_t)d->vscroll / (size_t)e->lh : 0;
	e->d = sv;
	int k = -1;
	for (int i = 0; i < npos; i++)
		if (!strcmp(pos_tab[i].path, d->path)) {
			k = i;
			break;
		}
	struct pos p = {NULL, line, off, top};
	if (k >= 0) {
		p.path = pos_tab[k].path;
		memmove(pos_tab + 1, pos_tab, (size_t)k * sizeof *pos_tab);
	} else {
		p.path = strdup(d->path);
		if (!p.path)
			return;
		struct pos *np = realloc(pos_tab, (size_t)(npos + 1) * sizeof *np);
		if (!np) {
			free(p.path);
			return;
		}
		pos_tab = np;
		memmove(pos_tab + 1, pos_tab, (size_t)npos * sizeof *pos_tab);
		npos++;
	}
	pos_tab[0] = p;
	while (npos > MAX_POS)
		free(pos_tab[--npos].path);
}

static void pos_restore(struct E *e)
{
	struct Doc *d = e->d;
	for (int i = 0; i < npos; i++) {
		if (strcmp(pos_tab[i].path, d->path))
			continue;
		size_t line = pos_tab[i].line < d->nl ? pos_tab[i].line : d->nl - 1;
		size_t le = line_end(e, line), at = d->ls[line] + pos_tab[i].off;
		d->cur = at < le ? at : le;
		while (d->cur > d->ls[line] && ((unsigned char)d->b[d->cur] & 0xc0) == 0x80)
			d->cur--; /* not in the middle of a character */
		d->anchor = NO_SEL;
		d->vscroll = (int)(pos_tab[i].top * (size_t)e->lh);
		return;
	}
}

/* $XDG_STATE_HOME/freelfs/fledit/session, else ~/.local/state/... */
static int session_path(char *out, size_t cap, int mk)
{
	const char *x = getenv("XDG_STATE_HOME"), *h = getenv("HOME");
	char base[PATH_MAX];
	if (x && x[0] == '/')
		snprintf(base, sizeof base, "%s", x);
	else if (h && h[0] == '/')
		snprintf(base, sizeof base, "%s/.local/state", h);
	else
		return -1;
	char dir[PATH_MAX + 32];
	snprintf(dir, sizeof dir, "%s/freelfs/fledit", base);
	if (mk) {
		char tmp[PATH_MAX + 32];
		snprintf(tmp, sizeof tmp, "%s", dir);
		for (char *p = tmp + 1; *p; p++)
			if (*p == '/') {
				*p = 0;
				mkdir(tmp, 0700);
				*p = '/';
			}
		mkdir(tmp, 0700);
	}
	snprintf(out, cap, "%s/session", dir);
	return 0;
}

static void session_load(void)
{
	char path[PATH_MAX + 64];
	if (session_path(path, sizeof path, 0) < 0)
		return;
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	char *line = NULL;
	size_t cap = 0;
	ssize_t n;
	while ((n = getline(&line, &cap, f)) > 0) {
		if (line[n - 1] == '\n')
			line[--n] = 0;
		if (!strncmp(line, "active ", 7)) {
			sess_active = atoi(line + 7);
		} else if (!strncmp(line, "tab ", 4) && line[4] == '/') {
			char **np = realloc(sess_paths, (size_t)(nsess + 1) * sizeof *np);
			if (np) {
				sess_paths = np;
				sess_paths[nsess] = strdup(line + 4);
				if (sess_paths[nsess])
					nsess++;
			}
		} else if (!strncmp(line, "pos ", 4) && npos < MAX_POS) {
			char *end;
			unsigned long ln = strtoul(line + 4, &end, 10), off = strtoul(end, &end, 10), top = strtoul(end, &end, 10);
			if (*end == ' ' && end[1] == '/') {
				struct pos *np = realloc(pos_tab, (size_t)(npos + 1) * sizeof *np);
				if (np) {
					pos_tab = np;
					pos_tab[npos] = (struct pos){strdup(end + 1), ln, off, top};
					if (pos_tab[npos].path)
						npos++;
				}
			}
		}
	}
	free(line);
	fclose(f);
}

/* Write the list of open files (when this window owns it) and the remembered positions. */
static void session_save(struct E *e)
{
	char path[PATH_MAX + 64], tmp[PATH_MAX + 80];
	if (session_path(path, sizeof path, 1) < 0)
		return;
	for (int i = e->ntabs - 1; i >= 0; i--)
		pos_note(e, e->tabs[i]);
	snprintf(tmp, sizeof tmp, "%s.%d", path, (int)getpid());
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "# fledit session\n");
	if (e->primary) {
		int active = 0, k = 0;
		for (int i = 0; i < e->ntabs; i++)
			if (e->tabs[i]->path[0] && !strchr(e->tabs[i]->path, '\n')) {
				if (i <= e->cur_tab)
					active = k;
				k++;
			}
		fprintf(f, "active %d\n", active);
		for (int i = 0; i < e->ntabs; i++)
			if (e->tabs[i]->path[0] && !strchr(e->tabs[i]->path, '\n'))
				fprintf(f, "tab %s\n", e->tabs[i]->path);
	} else {
		fprintf(f, "active %d\n", sess_active);
		for (int i = 0; i < nsess; i++)
			fprintf(f, "tab %s\n", sess_paths[i]);
	}
	for (int i = 0; i < npos; i++)
		if (!strchr(pos_tab[i].path, '\n'))
			fprintf(f, "pos %zu %zu %zu %s\n", pos_tab[i].line, pos_tab[i].off, pos_tab[i].top, pos_tab[i].path);
	if (fclose(f) == 0)
		rename(tmp, path);
	else
		unlink(tmp);
}

/* Moving the cursor or switching tabs writes the session two seconds later (once, not on every key). */
static void sess_fire(void *ud)
{
	struct E *e = ud;
	fl_timer_arm(e->sess_timer, 0, 0);
	e->sess_armed = 0;
	session_save(e);
}

static void sess_note(struct E *e)
{
	if (e->d == e->last_doc && e->d->cur == e->last_cur && e->cur_tab == e->last_tab)
		return;
	e->last_doc = e->d;
	e->last_cur = e->d->cur;
	e->last_tab = e->cur_tab;
	if (!e->sess_armed && e->sess_timer && e->d->path[0]) {
		e->sess_armed = 1;
		fl_timer_arm(e->sess_timer, 2000, 0);
	}
}

static void tab_select(struct E *e, int i)
{
	if (i < 0 || i >= e->ntabs)
		return;
	e->cur_tab = i;
	e->d = e->tabs[i];
	e->reveal = 1;
	e->tab_reveal = 1;
	e->dragging = 0;
	e->find_miss = 0;
	e->count = -1;
}

/* Put d after the current tab (or in its place when that one is an untouched blank) and show it. */
static int tab_insert(struct E *e, struct Doc *d)
{
	struct Doc *c = e->ntabs ? e->tabs[e->cur_tab] : NULL;
	if (c && !c->path[0] && c->len == 0 && !c->nundo && !c->nredo && e->ntabs > 0) {
		doc_free(e, c);
		e->tabs[e->cur_tab] = d;
		tab_select(e, e->cur_tab);
		return 0;
	}
	if (e->ntabs >= MAX_TABS)
		return -1;
	int at = e->ntabs ? e->cur_tab + 1 : 0;
	memmove(e->tabs + at + 1, e->tabs + at, (size_t)(e->ntabs - at) * sizeof *e->tabs);
	e->tabs[at] = d;
	e->ntabs++;
	tab_select(e, at);
	return 0;
}

static void tab_new(struct E *e)
{
	struct Doc *d = doc_alloc(e);
	if (!d)
		return;
	if (e->ntabs >= MAX_TABS) {
		say(e, 1, "Too many tabs");
		doc_free(e, d);
		return;
	}
	/* a new blank tab is always a tab of its own, not a replacement of a blank one */
	int at = e->ntabs ? e->cur_tab + 1 : 0;
	memmove(e->tabs + at + 1, e->tabs + at, (size_t)(e->ntabs - at) * sizeof *e->tabs);
	e->tabs[at] = d;
	e->ntabs++;
	tab_select(e, at);
}

/* Open a file in a tab of its own (or show the tab that has it). A name that does not exist
 * yet gives an empty tab with that name when create is set. */
static int tab_open(struct E *e, const char *given, int create, char *err, size_t ecap)
{
	char full[PATH_MAX], real[PATH_MAX];
	expand_home(given, full, sizeof full);
	const char *path = realpath(full, real) ? real : full;
	for (int i = 0; i < e->ntabs; i++)
		if (!strcmp(e->tabs[i]->path, path)) {
			tab_select(e, i);
			return 0;
		}
	struct stat st;
	struct Doc *cur = e->ntabs ? e->tabs[e->cur_tab] : NULL;
	int blank = cur && !cur->path[0] && cur->len == 0 && !cur->nundo && !cur->nredo;
	if (e->ntabs >= MAX_TABS && !blank) {
		snprintf(err, ecap, "Too many tabs (%d)", MAX_TABS);
		return -1;
	}
	struct Doc *d = doc_alloc(e);
	if (!d) {
		snprintf(err, ecap, "Out of memory");
		return -1;
	}
	struct Doc *sv = e->d;
	e->d = d;
	int rc;
	if (create && stat(path, &st) < 0 && errno == ENOENT) {
		snprintf(d->path, sizeof d->path, "%s", path);
		doc_detect(e);
		rc = 0;
	} else {
		rc = load_file(e, path, err, ecap);
	}
	e->d = sv;
	if (rc < 0) {
		doc_free(e, d);
		return -1;
	}
	tab_insert(e, d);
	pos_restore(e);
	return 0;
}

static void tab_close(struct E *e, int i)
{
	if (i < 0 || i >= e->ntabs)
		return;
	pos_note(e, e->tabs[i]);
	doc_free(e, e->tabs[i]);
	memmove(e->tabs + i, e->tabs + i + 1, (size_t)(e->ntabs - i - 1) * sizeof *e->tabs);
	e->ntabs--;
	if (!e->ntabs) {
		e->d = NULL;
		tab_new(e);
		return;
	}
	int to = e->cur_tab;
	if (i < to || to >= e->ntabs)
		to--;
	if (to < 0)
		to = 0;
	e->d = NULL;
	tab_select(e, to);
}

static int tab_first_modified(const struct E *e)
{
	for (int i = 0; i < e->ntabs; i++)
		if (modified_doc(e->tabs[i]))
			return i;
	return -1;
}

static void perform(struct E *e, int act)
{
	switch (act) {
	case P_NEW:
		tab_new(e);
		e->msg[0] = 0;
		break;
	case P_OPEN:
		open_path_dialog(e, 1, P_NONE);
		break;
	case P_CLOSE:
		tab_close(e, e->cur_tab);
		session_save(e);
		break;
	case P_QUIT: {
		int m = tab_first_modified(e);
		if (m < 0) {
			session_save(e);
			fl_app_quit(e->app);
		} else {
			tab_select(e, m);
			e->ov = OV_UNSAVED;
			e->pending = P_QUIT;
		}
		break;
	}
	}
}

static void request(struct E *e, int act)
{
	if (act == P_QUIT) {
		perform(e, act);
	} else if (act == P_CLOSE && modified(e)) {
		e->ov = OV_UNSAVED;
		e->pending = act;
	} else {
		perform(e, act);
	}
	fl_win_redraw(e->win);
}

static void save_now(struct E *e, int after)
{
	if (!e->d->path[0]) {
		open_path_dialog(e, 2, after);
		return;
	}
	char err[300];
	if (save_to(e, e->d->path, err, sizeof err) < 0) {
		snprintf(e->dlg_err, sizeof e->dlg_err, "%s", err);
		e->ov = OV_ERROR;
		return;
	}
	say(e, 0, "Saved %s", e->d->path);
	if (after != P_NONE)
		perform(e, after);
}

static void expand_home(const char *in, char *out, size_t cap)
{
	const char *home = getenv("HOME");
	if (in[0] == '~' && (in[1] == '/' || !in[1]) && home)
		snprintf(out, cap, "%s%s", home, in + 1);
	else if (in[0] != '/') {
		char cwd[PATH_MAX];
		if (getcwd(cwd, sizeof cwd))
			snprintf(out, cap, "%s/%s", cwd, in);
		else
			snprintf(out, cap, "%s", in);
	} else {
		snprintf(out, cap, "%s", in);
	}
}

/* Tab in the path field: complete the last name */
static void complete_path(struct E *e)
{
	char full[PATH_MAX];
	expand_home(e->dlg_text, full, sizeof full);
	char *slash = strrchr(full, '/');
	if (!slash)
		return;
	char dir[PATH_MAX];
	size_t dl = (size_t)(slash - full) + 1;
	memcpy(dir, full, dl);
	dir[dl] = 0;
	const char *part = slash + 1;
	size_t pl = strlen(part);
	DIR *d = opendir(dir);
	if (!d)
		return;
	char best[NAME_MAX + 2] = "";
	int n = 0, best_dir = 0;
	struct dirent *de;
	while ((de = readdir(d))) {
		if (strncmp(de->d_name, part, pl) || !strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (de->d_name[0] == '.' && part[0] != '.')
			continue;
		char p[PATH_MAX];
		snprintf(p, sizeof p, "%s%s", dir, de->d_name);
		struct stat st;
		int isdir = stat(p, &st) == 0 && S_ISDIR(st.st_mode);
		if (n == 0) {
			snprintf(best, sizeof best, "%s", de->d_name);
			best_dir = isdir;
		} else {
			size_t k = 0;
			while (best[k] && best[k] == de->d_name[k])
				k++;
			best[k] = 0;
			best_dir = 0;
		}
		n++;
	}
	closedir(d);
	if (!n)
		return;
	/* keep the user's own prefix (~) when it was typed */
	const char *raw = e->dlg_text;
	const char *rs = strrchr(raw, '/');
	size_t keep = rs ? (size_t)(rs - raw) + 1 : 0;
	char out[PATH_MAX];
	snprintf(out, sizeof out, "%.*s%s%s", (int)keep, raw, best, n == 1 && best_dir ? "/" : "");
	snprintf(e->dlg_text, sizeof e->dlg_text, "%s", out);
}

static void path_confirm(struct E *e)
{
	char full[PATH_MAX];
	if (!e->dlg_text[0]) {
		snprintf(e->dlg_err, sizeof e->dlg_err, "Type a file name.");
		return;
	}
	expand_home(e->dlg_text, full, sizeof full);
	struct stat st;
	int exists = stat(full, &st) == 0;
	if (exists && S_ISDIR(st.st_mode)) {
		snprintf(e->dlg_err, sizeof e->dlg_err, "That is a folder.");
		return;
	}
	if (e->path_mode == 1) {
		char err[300];
		if (tab_open(e, full, 0, err, sizeof err) < 0) {
			snprintf(e->dlg_err, sizeof e->dlg_err, "%s", err);
			return;
		}
		e->ov = OV_NONE;
		{
			const char *nm = strrchr(e->d->path, '/');
			say(e, 0, "Opened %s", nm ? nm + 1 : e->d->path);
		}
		session_save(e);
		return;
	}
	if (exists && strcmp(full, e->d->path)) {
		snprintf(e->dlg_text, sizeof e->dlg_text, "%s", full);
		e->ov = OV_OVERWRITE;
		return;
	}
	char err[300];
	if (save_to(e, full, err, sizeof err) < 0) {
		snprintf(e->dlg_err, sizeof e->dlg_err, "%s", err);
		/* not allowed / gone / read-only (an ejected drive, "/"): offer the same name in Home */
		const char *home = getenv("HOME");
		if (home && *home && (strstr(err, "Permission denied") || strstr(err, "Read-only") ||
				      strstr(err, "No such file") || strstr(err, "Input/output") || strstr(err, "No such device"))) {
			const char *bn = strrchr(full, '/');
			char alt[PATH_MAX];
			snprintf(alt, sizeof alt, "%s/%s", home, bn ? bn + 1 : full);
			if (strcmp(alt, full)) {
				snprintf(e->dlg_text, sizeof e->dlg_text, "%s", alt);
				snprintf(e->dlg_err, sizeof e->dlg_err, "%s - the name now points to your Home folder, press Save again", err);
			}
		}
		return;
	}
	e->ov = OV_NONE;
	say(e, 0, "Saved %s", full);
	if (e->after != P_NONE)
		perform(e, e->after);
}

/* "42" or "42:7": line and column (1-based). 0 when done, -1 when it is not a number. */
static int goto_line(struct E *e, const char *text)
{
	struct Doc *d = e->d;
	char *end;
	while (*text == ' ')
		text++;
	long ln = strtol(text, &end, 10);
	if (end == text || ln < 1)
		return -1;
	long col = 1;
	if (*end == ':') {
		col = strtol(end + 1, &end, 10);
		if (col < 1)
			col = 1;
	}
	while (*end == ' ')
		end++;
	if (*end)
		return -1;
	size_t l = (size_t)ln > d->nl ? d->nl - 1 : (size_t)ln - 1;
	d->cur = off_of(e, l, (int)col - 1);
	d->anchor = NO_SEL;
	d->want_col = -1;
	d->no_merge = 1;
	e->reveal = 1;
	e->center = 1;
	return 0;
}

static void dialog_frame(fl_ui *ui, int w, int h, fl_rect *box)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, fl_r(0, 0, cv->w, cv->h), fl_th.dark ? 0x99000000 : 0x66000000);
	*box = fl_r((cv->w - w) / 2, (cv->h - h) / 2 - 20, w, h);
	fl_fill_rect(cv, *box, fl_th.panel);
	fl_stroke_rect(cv, *box, 1, fl_th.line);
	fl_fill_rect(cv, fl_r(box->x, box->y, w, 3), fl_th.accent);
}

static void draw_overlay(fl_ui *ui, struct E *e)
{
	fl_canvas *cv = &ui->cv;
	fl_rect box;
	int esc = 0;
	if (ui->key == XKB_KEY_Escape) {
		esc = 1;
		ui->key = 0;
	}
	if (e->ov == OV_UNSAVED) {
		dialog_frame(ui, 460, 160, &box);
		int x = box.x + 24, w = box.w - 48, y = box.y + 22;
		fl_text(cv, ui->f_semi, x, y, fl_th.strong, "Save changes?", -1);
		y += 28;
		char s[PATH_MAX + 64], fit[PATH_MAX + 64];
		const char *nm = strrchr(e->d->path, '/');
		snprintf(s, sizeof s, "%s has changes that are not saved.", e->d->path[0] ? (nm ? nm + 1 : e->d->path) : "This document");
		fl_text_fit(ui->f_reg, s, w, fit, sizeof fit);
		fl_text(cv, ui->f_reg, x, y, fl_th.text, fit, -1);
		int by = box.y + box.h - 24 - 34;
		int save = fl_button(ui, ID_DLG + 1, fl_r(x + w - 96, by, 96, 34), "Save", FL_BTN_ACCENT);
		int drop = fl_button(ui, ID_DLG + 2, fl_r(x + w - 96 - 8 - 130, by, 130, 34), "Don't save", FL_BTN_OUTLINE);
		int cancel = fl_button(ui, ID_DLG + 3, fl_r(x, by, 96, 34), "Cancel", FL_BTN_OUTLINE);
		if (e->dlg_new) {
			ui->focus = ID_DLG + 1;
			e->dlg_new = 0;
		}
		if (save) {
			int act = e->pending;
			e->ov = OV_NONE;
			save_now(e, act);
		} else if (drop) {
			int act = e->pending;
			e->ov = OV_NONE;
			e->d->saved_top = (long)e->d->nundo; /* so the quit path sees it as clean */
			perform(e, act);
		} else if (cancel || esc) {
			e->ov = OV_NONE;
		}
	} else if (e->ov == OV_PATH) {
		dialog_frame(ui, 560, 176, &box);
		int x = box.x + 24, w = box.w - 48, y = box.y + 22;
		fl_text(cv, ui->f_semi, x, y, fl_th.strong, e->path_mode == 1 ? "Open file" : "Save as", -1);
		y += 28;
		if (e->dlg_new) {
			ui->focus = ID_DLG;
			e->dlg_new = 0;
		}
		uint32_t k = ui->key;
		if (k == XKB_KEY_Tab) {
			complete_path(e);
			ui->key = 0;
			ui->focus = ID_DLG;
		}
		int ch = fl_textfield(ui, ID_DLG, fl_r(x, y, w, 36), e->dlg_text, sizeof e->dlg_text, "Path of the file", ui->f_reg);
		if (ch & FL_EDIT_CHANGED)
			e->dlg_err[0] = 0;
		y += 36 + 6;
		if (e->dlg_err[0]) {
			char fit[220];
			fl_text_fit(e->small, e->dlg_err, w, fit, sizeof fit);
			fl_text(cv, e->small, x, y, fl_th.err, fit, -1);
		} else {
			fl_text(cv, e->small, x, y, fl_th.dim, "Tab completes the name.", -1);
		}
		int by = box.y + box.h - 24 - 34;
		int ok = fl_button(ui, ID_DLG + 1, fl_r(x + w - 96, by, 96, 34), e->path_mode == 1 ? "Open" : "Save", FL_BTN_ACCENT);
		int cancel = fl_button(ui, ID_DLG + 2, fl_r(x + w - 96 - 8 - 96, by, 96, 34), "Cancel", FL_BTN_OUTLINE);
		if (ok || (ch & FL_EDIT_ENTER)) {
			path_confirm(e);
		} else if (cancel || esc) {
			e->ov = OV_NONE;
		}
	} else if (e->ov == OV_OVERWRITE) {
		dialog_frame(ui, 460, 150, &box);
		int x = box.x + 24, w = box.w - 48, y = box.y + 22;
		fl_text(cv, ui->f_semi, x, y, fl_th.strong, "Replace file?", -1);
		y += 28;
		char fit[PATH_MAX];
		fl_text_fit(ui->f_reg, e->dlg_text, w, fit, sizeof fit);
		fl_text(cv, ui->f_reg, x, y, fl_th.text, fit, -1);
		y += 22;
		fl_text(cv, e->small, x, y, fl_th.dim, "A file with this name already exists.", -1);
		int by = box.y + box.h - 24 - 34;
		int ok = fl_button(ui, ID_DLG + 1, fl_r(x + w - 110, by, 110, 34), "Replace", FL_BTN_ACCENT);
		int cancel = fl_button(ui, ID_DLG + 2, fl_r(x + w - 110 - 8 - 96, by, 96, 34), "Cancel", FL_BTN_OUTLINE);
		if (ok) {
			char err[300];
			if (save_to(e, e->dlg_text, err, sizeof err) < 0) {
				snprintf(e->dlg_err, sizeof e->dlg_err, "%s", err);
				e->ov = OV_ERROR;
			} else {
				e->ov = OV_NONE;
				say(e, 0, "Saved %s", e->d->path);
				if (e->after != P_NONE)
					perform(e, e->after);
			}
		} else if (cancel || esc) {
			e->ov = OV_PATH;
		}
	} else if (e->ov == OV_GOTO) {
		dialog_frame(ui, 360, 176, &box);
		int x = box.x + 24, w = box.w - 48, y = box.y + 22;
		fl_text(cv, ui->f_semi, x, y, fl_th.strong, "Go to line", -1);
		y += 28;
		if (e->dlg_new) {
			ui->focus = ID_DLG;
			e->dlg_new = 0;
		}
		int ch = fl_textfield(ui, ID_DLG, fl_r(x, y, w, 36), e->dlg_text, 32, "Line number", ui->f_reg);
		if (ch & FL_EDIT_CHANGED)
			e->dlg_err[0] = 0;
		y += 36 + 6;
		if (e->dlg_err[0]) {
			fl_text(cv, e->small, x, y, fl_th.err, e->dlg_err, -1);
		} else {
			char hint[96];
			snprintf(hint, sizeof hint, "1 to %zu. Line:column works too.", e->d->nl);
			fl_text(cv, e->small, x, y, fl_th.dim, hint, -1);
		}
		int by = box.y + box.h - 24 - 34;
		int ok = fl_button(ui, ID_DLG + 1, fl_r(x + w - 96, by, 96, 34), "Go", FL_BTN_ACCENT);
		int cancel = fl_button(ui, ID_DLG + 2, fl_r(x + w - 96 - 8 - 96, by, 96, 34), "Cancel", FL_BTN_OUTLINE);
		if (ok || (ch & FL_EDIT_ENTER)) {
			if (goto_line(e, e->dlg_text) == 0)
				e->ov = OV_NONE;
			else
				snprintf(e->dlg_err, sizeof e->dlg_err, "Type a line number from 1 to %zu.", e->d->nl);
		} else if (cancel || esc) {
			e->ov = OV_NONE;
		}
	} else if (e->ov == OV_ERROR) {
		dialog_frame(ui, 520, 150, &box);
		int x = box.x + 24, w = box.w - 48, y = box.y + 22;
		fl_text(cv, ui->f_semi, x, y, fl_th.strong, "That did not work", -1);
		y += 28;
		char fit[300];
		fl_text_fit(ui->f_reg, e->dlg_err, w, fit, sizeof fit);
		fl_text(cv, ui->f_reg, x, y, fl_th.err, fit, -1);
		int by = box.y + box.h - 24 - 34;
		if (fl_button(ui, ID_DLG + 1, fl_r(x + w - 96, by, 96, 34), "OK", FL_BTN_ACCENT) || esc)
			e->ov = OV_NONE;
	}
	ui->key = 0;
	ui->text[0] = 0;
	ui->press = ui->release = 0;
}

/* ---- the view ------------------------------------------------------------------------------------------- */

struct layout {
	fl_rect tabs, find, gutter, text, vsb, status;
	int view_h, view_w;
};

static int digits_of(size_t n)
{
	int d = 1;
	while (n >= 10) {
		n /= 10;
		d++;
	}
	return d;
}

static void make_layout(struct E *e, fl_rect c, struct layout *l)
{
	fl_rect area = c;
	l->status = fl_cut_bottom(&area, STATUS_H);
	l->tabs = fl_cut_top(&area, TAB_H);
	l->find = e->find_open ? fl_cut_top(&area, e->replace_open ? 2 * FIND_H : FIND_H) : fl_r(area.x, area.y, 0, 0);
	int d = digits_of(e->d->nl);
	if (d < 3)
		d = 3;
	l->gutter = fl_cut_left(&area, d * fl_font_cell(e->num) + 26);
	l->vsb = fl_cut_right(&area, 10);
	fl_cut_left(&area, 10); /* a little air between the gutter and the text */
	l->text = area;
	l->view_h = area.h;
	l->view_w = area.w;
}

static void clamp_scroll(struct E *e, const struct layout *l)
{
	long content = (long)e->d->nl * e->lh;
	long max = content - l->view_h;
	if (max < 0)
		max = 0;
	if (e->d->vscroll > max)
		e->d->vscroll = (int)max;
	if (e->d->vscroll < 0)
		e->d->vscroll = 0;
	if (e->d->hscroll < 0)
		e->d->hscroll = 0;
}

static void reveal_cursor(struct E *e, const struct layout *l)
{
	size_t ln = line_of(e, e->d->cur);
	long top = (long)ln * e->lh;
	if (top < e->d->vscroll)
		e->d->vscroll = (int)top;
	if (top + e->lh > e->d->vscroll + l->view_h)
		e->d->vscroll = (int)(top + e->lh - l->view_h);
	if (e->center) { /* a jump: put the line in the middle of the view */
		e->d->vscroll = (int)(top - (l->view_h - e->lh) / 2);
		e->center = 0;
	}
	int x = col_of(e, ln, e->d->cur) * e->cell;
	if (x < e->d->hscroll)
		e->d->hscroll = x > 3 * e->cell ? x - 3 * e->cell : 0;
	else if (x + e->cell > e->d->hscroll + l->view_w - 12)
		e->d->hscroll = x + e->cell - l->view_w + 12 + 3 * e->cell;
	clamp_scroll(e, l);
}

static size_t hit_test(struct E *e, const struct layout *l, int mx, int my)
{
	long ln = (long)(my - l->text.y + e->d->vscroll);
	ln = ln < 0 ? 0 : ln / e->lh;
	if ((size_t)ln >= e->d->nl)
		ln = (long)e->d->nl - 1;
	int col = (mx - l->text.x + e->d->hscroll + e->cell / 2) / e->cell;
	if (col < 0)
		col = 0;
	return off_of(e, (size_t)ln, col);
}

/* the colours of the classes in syntax.h, in the FreeLFS palette: green is the accent, the rest
 * is kept quiet; no purple */
static fl_color pal[K_COUNT];

static void make_palette(void)
{
	static const uint32_t dk[K_COUNT] = {
		0, 0x5cc487, 0x4db6ac, 0xd9a45b, 0xe8836a, 0x6c7680, 0x7db4ea, 0xb7c96e,
		0xa9b1b9, 0xe0cd8a, 0x5cc487, 0x5cc487, 0xe0cd8a, 0x7db4ea, 0x4db6ac,
	};
	static const uint32_t lt[K_COUNT] = {
		0, 0x1f7a45, 0x12766d, 0x8c5f0b, 0xb0481d, 0x6b747d, 0x2863a8, 0x5d7a1c,
		0x4a525a, 0x7a6200, 0x1f7a45, 0x1f7a45, 0x7a6200, 0x2863a8, 0x12766d,
	};
	for (int i = 1; i < K_COUNT; i++)
		pal[i] = FL_HEX(fl_th.dark ? dk[i] : lt[i]);
	pal[K_TEXT] = fl_th.text;
}

static void flush_run(fl_ui *ui, struct E *e, const struct layout *l, int y, char *run, int *rn, int run_col, int k)
{
	if (!*rn)
		return;
	fl_text(&ui->cv, k == K_HEAD || k == K_SECTION ? e->bold : e->mono, l->text.x + run_col * e->cell - e->d->hscroll, y,
	        pal[k], run, *rn);
	*rn = 0;
}

static void draw_line(fl_ui *ui, struct E *e, const struct layout *l, size_t ln, int y, const uint8_t *cls)
{
	const char *base = e->d->b + e->d->ls[ln], *s = base, *end = e->d->b + line_end(e, ln);
	int first_col = e->d->hscroll / e->cell - 1, last_col = (e->d->hscroll + l->view_w) / e->cell + 1;
	int col = 0, rn = 0, run_col = 0, rk = K_TEXT;
	char run[256];
	int ty = y + (e->lh - fl_font_height(e->mono)) / 2;
#define FLUSH() flush_run(ui, e, l, ty, run, &rn, run_col, rk)
	while (s < end && col <= last_col) {
		int k = cls ? cls[s - base] : K_TEXT;
		if (*s == '\t') {
			FLUSH();
			col = (col / TAB_W + 1) * TAB_W;
			s++;
			continue;
		}
		uint32_t cp;
		int n = u8dec(s, end, &cp);
		s += n;
		if (cp < 0x20 || cp == 0x7f)
			continue;
		int w = cp_width(cp);
		if (col + w < first_col) {
			FLUSH();
			col += w;
			continue;
		}
		if (cp < 0x80 && cp != 0x20) {
			if (rn && (col != run_col + rn || k != rk))
				FLUSH();
			if (!rn) {
				run_col = col;
				rk = k;
			}
			run[rn++] = (char)cp;
			if (rn == (int)sizeof run)
				FLUSH();
		} else if (cp != 0x20) {
			FLUSH();
			int gc = w == 0 && col > 0 ? col - 1 : col;
			fl_glyph(&ui->cv, e->mono, l->text.x + gc * e->cell - e->d->hscroll, ty, pal[k], cp);
		} else {
			FLUSH();
		}
		col += w;
	}
	FLUSH();
#undef FLUSH
}

/* The partner of the bracket at pos, skipping brackets in strings and comments. Returns 1 and
 * sets *mp when found, 0 when there is none, -1 when pos is not a bracket. */
static int bracket_match(struct E *e, size_t pos, size_t *mp)
{
	static uint8_t cls[HL_LINE];
	struct Doc *d = e->d;
	const char *open = "([{", *close = ")]}";
	char c = d->b[pos];
	const char *p = c ? strchr(open, c) : NULL;
	int dir = 1;
	char other;
	if (p) {
		other = close[p - open];
	} else if (c && (p = strchr(close, c))) {
		dir = -1;
		other = open[p - close];
	} else {
		return -1;
	}
	size_t l = line_of(e, pos);
	uint8_t *kc = line_classes(e, l, cls);
	int skip = kc && kc[pos - d->ls[l]] != K_STRING && kc[pos - d->ls[l]] != K_COMMENT;
	long depth = 0;
	size_t from = pos;
	for (int lines = 0; lines < 3000; lines++) {
		size_t ls = d->ls[l], le = line_end(e, l);
		kc = skip ? line_classes(e, l, cls) : NULL;
		if (dir > 0) {
			for (size_t i = lines ? ls : from + 1; i < le; i++) {
				if (kc && (kc[i - ls] == K_STRING || kc[i - ls] == K_COMMENT))
					continue;
				if (d->b[i] == c)
					depth++;
				else if (d->b[i] == other && depth-- == 0) {
					*mp = i;
					return 1;
				}
			}
			if (++l >= d->nl)
				return 0;
		} else {
			for (size_t i = lines ? le : from; i > ls;) {
				i--;
				if (kc && (kc[i - ls] == K_STRING || kc[i - ls] == K_COMMENT))
					continue;
				if (d->b[i] == c)
					depth++;
				else if (d->b[i] == other && depth-- == 0) {
					*mp = i;
					return 1;
				}
			}
			if (l == 0)
				return 0;
			l--;
		}
	}
	return 0;
}

static void handle_key(fl_ui *ui, struct E *e, const struct layout *l)
{
	uint32_t k = ui->key;
	int shift = ui->mods & FL_MOD_SHIFT, ctrl = ui->mods & FL_MOD_CTRL;
	int lines = l->view_h / e->lh > 1 ? l->view_h / e->lh - 1 : 1;
	if (k && ctrl) {
		int used = 1;
		switch (k) {
		case 'z': case 'Z':
			if (shift)
				do_redo(e);
			else
				do_undo(e);
			break;
		case 'y': case 'Y': do_redo(e); break;
		case 'a': case 'A':
			e->d->anchor = 0;
			e->d->cur = e->d->len;
			e->reveal = 1;
			break;
		case 'c': case 'C': copy_sel(e, 0); break;
		case 'x': case 'X': copy_sel(e, 1); break;
		case 'v': case 'V': paste(e); break;
		case XKB_KEY_Home: move_to(e, 0, shift, 0); break;
		case XKB_KEY_End: move_to(e, e->d->len, shift, 0); break;
		case XKB_KEY_Left: move_to(e, word_left(e, e->d->cur), shift, 0); break;
		case XKB_KEY_Right: move_to(e, word_right(e, e->d->cur), shift, 0); break;
		case XKB_KEY_BackSpace:
			if (has_sel(e))
				delete_sel(e);
			else if (e->d->cur)
				do_replace(e, word_left(e, e->d->cur), e->d->cur - word_left(e, e->d->cur), "", 0, word_left(e, e->d->cur), 1);
			break;
		case XKB_KEY_Delete:
			if (has_sel(e))
				delete_sel(e);
			else if (e->d->cur < e->d->len) {
				size_t to = word_right(e, e->d->cur);
				do_replace(e, e->d->cur, to - e->d->cur, "", 0, e->d->cur, 1);
			}
			break;
		default: used = 0; break;
		}
		if (used)
			ui->key = 0;
		return;
	}
	if (!k)
		return;
	int used = 1;
	switch (k) {
	case XKB_KEY_Left:
		if (has_sel(e) && !shift) {
			size_t a, b;
			sel_range(e, &a, &b);
			move_to(e, a, 0, 0);
		} else {
			move_to(e, prev_char(e, e->d->cur), shift, 0);
		}
		break;
	case XKB_KEY_Right:
		if (has_sel(e) && !shift) {
			size_t a, b;
			sel_range(e, &a, &b);
			move_to(e, b, 0, 0);
		} else {
			move_to(e, next_char(e, e->d->cur), shift, 0);
		}
		break;
	case XKB_KEY_Up: move_vert(e, -1, shift); break;
	case XKB_KEY_Down: move_vert(e, 1, shift); break;
	case XKB_KEY_Page_Up: move_vert(e, -lines, shift); break;
	case XKB_KEY_Page_Down: move_vert(e, lines, shift); break;
	case XKB_KEY_Home: {
		size_t ln = line_of(e, e->d->cur), s = e->d->ls[ln], f = s;
		while (f < line_end(e, ln) && (e->d->b[f] == ' ' || e->d->b[f] == '\t'))
			f++;
		move_to(e, e->d->cur == f ? s : f, shift, 0); /* first press: first character; again: column 0 */
		break;
	}
	case XKB_KEY_End: move_to(e, line_end(e, line_of(e, e->d->cur)), shift, 0); break;
	case XKB_KEY_BackSpace:
		if (has_sel(e)) {
			delete_sel(e);
		} else if (e->d->cur) {
			size_t p = prev_char(e, e->d->cur);
			size_t ls = e->d->ls[line_of(e, e->d->cur)];
			if (e->d->indent_w && e->d->cur > ls) { /* in the indentation: back one level */
				size_t k = ls;
				while (k < e->d->cur && e->d->b[k] == ' ')
					k++;
				if (k == e->d->cur) {
					size_t cols = e->d->cur - ls, rem = cols % (size_t)e->d->indent_w;
					p = e->d->cur - (rem ? rem : (size_t)e->d->indent_w);
				}
			}
			do_replace(e, p, e->d->cur - p, "", 0, p, 1);
		}
		break;
	case XKB_KEY_Delete:
		if (has_sel(e))
			delete_sel(e);
		else if (e->d->cur < e->d->len)
			do_replace(e, e->d->cur, next_char(e, e->d->cur) - e->d->cur, "", 0, e->d->cur, 1);
		break;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		newline(e);
		break;
	case XKB_KEY_Tab:
		if (has_sel(e) && line_of(e, e->d->anchor) != line_of(e, e->d->cur)) {
			indent_lines(e, 1);
		} else {
			size_t a = e->d->cur, b = a;
			if (has_sel(e))
				sel_range(e, &a, &b);
			char u[16];
			size_t n = indent_unit(e, u, col_of(e, line_of(e, a), a));
			insert_text(e, u, n);
		}
		break;
	case XKB_KEY_ISO_Left_Tab:
		indent_lines(e, -1);
		break;
	case XKB_KEY_Escape:
		e->d->anchor = NO_SEL;
		break;
	default: used = 0; break;
	}
	if (used) {
		ui->key = 0;
		ui->text[0] = 0;
	} else if (ui->text[0]) {
		type_text(e, ui->text);
		ui->text[0] = 0;
		ui->key = 0;
	}
}

static void handle_mouse(fl_ui *ui, struct E *e, const struct layout *l, int active)
{
	int res = fl_widget(ui, ID_TEXT, l->text, 0);
	int mods = fl_app_mods(e->app);
	if (res & FL_HOVER)
		ui->cursor = FL_CURSOR_IBEAM;
	if ((res & FL_HOVER) && ui->wheel) {
		if (mods & FL_MOD_SHIFT)
			e->d->hscroll += ui->wheel * 3 * e->cell * 4;
		else
			e->d->vscroll += ui->wheel * 3 * e->lh;
		ui->wheel = 0;
	}
	if (!active)
		return;
	if ((res & FL_HOVER) && (ui->press & FL_BTN_L)) {
		size_t pos = hit_test(e, l, ui->mx, ui->my);
		ui->focus = 0;
		e->dragging = 1;
		e->d->no_merge = 1;
		if (ui->clicks >= 3) {
			size_t ln = line_of(e, pos);
			e->d->anchor = e->d->ls[ln];
			e->d->cur = ln + 1 < e->d->nl ? e->d->ls[ln + 1] : e->d->len;
		} else if (ui->clicks == 2) {
			size_t a = pos, b = pos;
			if (a < e->d->len && is_word((unsigned char)e->d->b[a])) {
				while (a > 0 && is_word((unsigned char)e->d->b[a - 1]))
					a--;
				while (b < e->d->len && is_word((unsigned char)e->d->b[b]))
					b++;
			} else if (b < e->d->len && e->d->b[b] != '\n') {
				b = next_char(e, b);
			}
			e->d->anchor = a;
			e->d->cur = b;
		} else if (mods & FL_MOD_SHIFT) {
			if (e->d->anchor == NO_SEL)
				e->d->anchor = e->d->cur;
			e->d->cur = pos;
		} else {
			e->d->cur = pos;
			e->d->anchor = pos;
		}
		e->d->want_col = -1;
	} else if (e->dragging && (ui->btn_held & FL_BTN_L)) {
		if (ui->my < l->text.y)
			e->d->vscroll -= e->lh / 2;
		else if (ui->my > l->text.y + l->view_h)
			e->d->vscroll += e->lh / 2;
		size_t pos = hit_test(e, l, ui->mx, ui->my);
		if (pos != e->d->cur) {
			e->d->cur = pos;
			e->d->want_col = -1;
		}
	}
	if (!(ui->btn_held & FL_BTN_L))
		e->dragging = 0;
}

static void open_find(struct E *e, int replace)
{
	int was_open = e->find_open;
	e->find_focus_req = replace && was_open && e->replace_open ? 2 : 1; /* again: the replace field */
	e->find_open = 1;
	if (replace)
		e->replace_open = 1;
	struct Doc *d = e->d;
	if (!was_open && has_sel(e)) { /* the selection becomes the search text (not when the bar is already up) */
		size_t a, b;
		sel_range(e, &a, &b);
		if (b - a < sizeof e->needle && !memchr(d->b + a, '\n', b - a)) {
			memcpy(e->needle, d->b + a, b - a);
			e->needle[b - a] = 0;
			e->count = -1;
		}
	}
}

/* the pattern or an option changed: look again from the start of the current match */
static void find_refresh(struct E *e)
{
	struct Doc *d = e->d;
	size_t a = d->cur, b, ms, me;
	e->find_miss = 0;
	e->count = -1;
	e->re_key[0] = 0;
	if (!e->needle[0])
		return;
	if (has_sel(e)) {
		sel_range(e, &a, &b);
	}
	if (find_from(e, a, 1, &ms, &me)) {
		d->anchor = ms;
		d->cur = me;
		e->reveal = 1;
	} else {
		e->find_miss = 1;
	}
}

static void draw_find(fl_ui *ui, struct E *e, fl_rect fr)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, fr, fl_th.panel);
	fl_fill_rect(cv, fl_r(fr.x, fr.y + fr.h - 1, fr.w, 1), fl_th.line);
	if (e->find_focus_req) {
		ui->focus = e->find_focus_req == 2 && e->replace_open ? ID_REPL : ID_FIND;
		e->find_focus_req = 0;
	}
	if (e->replace_open && (ui->key == XKB_KEY_Tab || ui->key == XKB_KEY_ISO_Left_Tab) && (ui->focus == ID_FIND || ui->focus == ID_REPL)) {
		ui->focus = ui->focus == ID_FIND ? ID_REPL : ID_FIND; /* Tab goes between the two fields */
		ui->key = 0;
		fl_win_redraw(ui->win);
	}
	int y = fr.y + (FIND_H - 32) / 2, x = fr.x + 12;
	int fw = fr.w - 12 - 384 - 90;
	fw = fw < 120 ? 120 : fw > 300 ? 300 : fw;
	int ch = fl_textfield(ui, ID_FIND, fl_r(x, y, fw, 32), e->needle, sizeof e->needle, "Find", ui->f_reg);
	if (ch & FL_EDIT_CHANGED)
		find_refresh(e);
	if (ch & FL_EDIT_ENTER)
		find_step(e, (ui->mods & FL_MOD_SHIFT) ? -1 : 1);
	x += fw + 8;
	if (fl_button(ui, ID_FPREV, fl_r(x, y, 84, 32), "Previous", FL_BTN_OUTLINE)) {
		find_step(e, -1);
		ui->focus = 0;
	}
	x += 90;
	if (fl_button(ui, ID_FNEXT, fl_r(x, y, 64, 32), "Next", FL_BTN_OUTLINE)) {
		find_step(e, 1);
		ui->focus = 0;
	}
	x += 74;
	if (fl_button(ui, ID_FCASE, fl_r(x, y, 34, 32), "Aa", e->f_case ? FL_BTN_ACCENT : FL_BTN_OUTLINE)) {
		e->f_case = !e->f_case;
		find_refresh(e);
	}
	x += 38;
	if (fl_button(ui, ID_FREGEX, fl_r(x, y, 34, 32), ".*", e->f_regex ? FL_BTN_ACCENT : FL_BTN_OUTLINE)) {
		e->f_regex = !e->f_regex;
		find_refresh(e);
	}
	x += 44;
	/* what the search found */
	int rx = fr.x + fr.w - 12 - 32 - 8 - 104;
	if (e->needle[0] && rx - x > 40) {
		re_prepare(e);
		char info[64];
		int err = 0;
		if (e->f_regex && e->re_bad) {
			snprintf(info, sizeof info, "Not a valid pattern");
			err = 1;
		} else if (e->find_miss) {
			snprintf(info, sizeof info, "No match");
			err = 1;
		} else {
			long n = count_matches(e);
			if (n >= 100000)
				snprintf(info, sizeof info, "100000+ matches");
			else
				snprintf(info, sizeof info, "%ld match%s", n, n == 1 ? "" : "es");
			if (n == 0)
				err = 1;
		}
		fl_text_in(cv, ui->f_reg, fl_r(x, fr.y, rx - x, FIND_H), FL_LEFT, 0, err ? fl_th.err : fl_th.dim, info);
	}
	if (fl_button(ui, ID_FREPLACE, fl_r(rx, y, 104, 32), e->replace_open ? "Hide replace" : "Replace", FL_BTN_OUTLINE)) {
		e->replace_open = !e->replace_open;
		ui->focus = 0;
		fl_win_redraw(ui->win);
	}
	int closed = fl_icon_button(ui, ID_FCLOSE, fl_r(fr.x + fr.w - 12 - 32, y, 32, 32), FL_ICON_CLOSE, 12, fl_th.dim);
	if (e->replace_open) {
		int y2 = y + FIND_H, x2 = fr.x + 12;
		int all_key = 0;
		if (ui->focus == ID_REPL && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_KP_Enter) && (ui->mods & FL_MOD_CTRL)) {
			all_key = 1; /* Ctrl+Enter: replace all (the text field itself ignores Enter with Ctrl) */
			ui->key = 0;
		}
		int rc = fl_textfield(ui, ID_REPL, fl_r(x2, y2, fw, 32), e->repl, sizeof e->repl, "Replace with", ui->f_reg);
		x2 += fw + 8;
		int one = rc & FL_EDIT_ENTER;
		if (fl_button(ui, ID_RONE, fl_r(x2, y2, 84, 32), "Replace", FL_BTN_OUTLINE) || one) {
			replace_one(e);
			e->reveal = 1;
			if (!one)
				ui->focus = 0;
		}
		x2 += 90;
		if (fl_button(ui, ID_RALL, fl_r(x2, y2, 64, 32), "All", FL_BTN_OUTLINE) || all_key) {
			replace_all(e);
			if (!all_key)
				ui->focus = 0;
		}
	}
	if (closed || ((ui->focus == ID_FIND || ui->focus == ID_REPL) && ui->key == XKB_KEY_Escape)) {
		e->find_open = 0;
		ui->focus = 0;
		ui->key = 0;
	}
}

static void fmt_size(size_t n, char *out, size_t cap)
{
	if (n < 1000)
		snprintf(out, cap, "%zu B", n);
	else if (n < 1000000)
		snprintf(out, cap, "%.1f kB", (double)n / 1e3);
	else
		snprintf(out, cap, "%.1f MB", (double)n / 1e6);
}

static void draw_status(fl_ui *ui, struct E *e, fl_rect st)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, st, fl_th.panel);
	fl_fill_rect(cv, fl_r(st.x, st.y, st.w, 1), fl_th.line);
	size_t ln = line_of(e, e->d->cur);
	char left[160], right[160], sz[32];
	if (has_sel(e)) {
		size_t a, b;
		sel_range(e, &a, &b);
		snprintf(left, sizeof left, "Ln %zu, Col %d   (%zu selected)", ln + 1, col_of(e, ln, e->d->cur) + 1, b - a);
	} else {
		snprintf(left, sizeof left, "Ln %zu, Col %d", ln + 1, col_of(e, ln, e->d->cur) + 1);
	}
	fmt_size(e->d->len, sz, sizeof sz);
	char ind[24];
	if (e->d->indent_w)
		snprintf(ind, sizeof ind, "%d spaces", e->d->indent_w);
	else
		snprintf(ind, sizeof ind, "Tabs");
	snprintf(right, sizeof right, "%s   %s   %s%s   %s   %s   %zu line%s", e->d->lang ? sx_name(e->d->lang) : (e->d->len > MAX_HL ? "Plain (large file)" : "Plain text"),
	         ind, e->d->bad_utf8 ? "UTF-8 (bad bytes kept)" : "UTF-8", e->d->bom ? " BOM" : "", e->d->crlf ? "CRLF" : "LF", sz, e->d->nl, e->d->nl == 1 ? "" : "s");
	int rw = fl_text_width(e->small, right, -1);
	fl_text_in(cv, e->small, fl_r(st.x + st.w - 14 - rw, st.y + 1, rw + 14, st.h - 1), FL_LEFT, 0, fl_th.dim, right);
	fl_text_in(cv, e->small, fl_r(st.x + 14, st.y + 1, 190, st.h - 1), FL_LEFT, 0, fl_th.dim, left);
	if (e->msg[0]) {
		int mx = st.x + 14 + 200, mw = st.w - 14 - rw - 28 - 200;
		if (mw > 40) {
			char fit[320];
			fl_text_fit(e->small, e->msg, mw, fit, sizeof fit);
			fl_text_in(cv, e->small, fl_r(mx, st.y + 1, mw, st.h - 1), FL_LEFT, 0, e->msg_err ? fl_th.err : fl_th.text, fit);
		}
	}
}

static void update_title(struct E *e)
{
	char t[PATH_MAX + 16];
	const char *name = e->d->path[0] ? strrchr(e->d->path, '/') : NULL;
	name = name ? name + 1 : e->d->path[0] ? e->d->path : "Untitled";
	snprintf(t, sizeof t, "%s%s", modified(e) ? "\xe2\x80\xa2 " : "", name);
	if (strcmp(t, e->last_title)) {
		snprintf(e->last_title, sizeof e->last_title, "%s", t);
		fl_win_set_title(e->win, t);
	}
}

/* ---- the tab strip ------------------------------------------------------------------------------ */

/* the name a tab shows; when another tab has the same file name, the folder too */
static void tab_label(const struct E *e, int i, char *name, size_t ncap, char *hint, size_t hcap)
{
	const struct Doc *d = e->tabs[i];
	hint[0] = 0;
	if (!d->path[0]) {
		if (d->untitled > 1 && e->ntabs > 1)
			snprintf(name, ncap, "Untitled %d", d->untitled);
		else
			snprintf(name, ncap, "Untitled");
		return;
	}
	const char *sl = strrchr(d->path, '/');
	snprintf(name, ncap, "%s", sl ? sl + 1 : d->path);
	for (int j = 0; j < e->ntabs; j++) {
		const struct Doc *o = e->tabs[j];
		const char *os = strrchr(o->path, '/');
		if (j != i && o->path[0] && !strcmp(os ? os + 1 : o->path, name)) {
			/* the last folder of the path */
			char dir[PATH_MAX];
			snprintf(dir, sizeof dir, "%s", d->path);
			char *s1 = strrchr(dir, '/');
			if (s1 && s1 != dir) {
				*s1 = 0;
				char *s2 = strrchr(dir, '/');
				snprintf(hint, hcap, "%s", s2 ? s2 + 1 : dir);
			}
			break;
		}
	}
}

/* A filled circle: the "not saved" dot. */
static void draw_dot(fl_canvas *cv, float cx, float cy, float r, fl_color c)
{
	const float k = 0.5523f * r;
	fl_path *p = fl_path_new();
	if (!p)
		return;
	fl_path_move(p, cx + r, cy);
	fl_path_cubic(p, cx + r, cy + k, cx + k, cy + r, cx, cy + r);
	fl_path_cubic(p, cx - k, cy + r, cx - r, cy + k, cx - r, cy);
	fl_path_cubic(p, cx - r, cy - k, cx - k, cy - r, cx, cy - r);
	fl_path_cubic(p, cx + k, cy - r, cx + r, cy - k, cx + r, cy);
	fl_path_close(p);
	fl_path_fill(cv, p, c);
	fl_path_free(p);
}

/* Draws the strip; sets *sel to the tab to show and *close to the tab to close, else -1. */
static void draw_tabs(fl_ui *ui, struct E *e, fl_rect r, int *sel, int *close, int *add)
{
	fl_canvas *cv = &ui->cv;
	*sel = *close = -1;
	*add = 0;
	fl_fill_rect(cv, r, fl_th.panel);
	fl_fill_rect(cv, fl_r(r.x, r.y + r.h - 1, r.w, 1), fl_th.line);
	int plus_w = 38, avail = r.w - plus_w;
	int tw = e->ntabs ? avail / e->ntabs : avail;
	if (tw > 220)
		tw = 220;
	if (tw < 120)
		tw = 120;
	int total = tw * e->ntabs;
	if (total <= avail) {
		e->tab_scroll = 0;
	} else {
		fl_rect over = fl_r(r.x, r.y, avail, r.h);
		if (fl_in(over, ui->mx, ui->my) && ui->wheel) {
			e->tab_scroll += ui->wheel * 60;
			ui->wheel = 0;
		}
		if (e->tab_reveal) {
			int x0 = e->cur_tab * tw;
			if (x0 < e->tab_scroll)
				e->tab_scroll = x0;
			if (x0 + tw > e->tab_scroll + avail)
				e->tab_scroll = x0 + tw - avail;
		}
		if (e->tab_scroll > total - avail)
			e->tab_scroll = total - avail;
		if (e->tab_scroll < 0)
			e->tab_scroll = 0;
	}
	e->tab_reveal = 0;
	fl_rect save = cv->clip;
	fl_clip(cv, fl_r(r.x, r.y, avail, r.h));
	for (int i = 0; i < e->ntabs; i++) {
		fl_rect t = fl_r(r.x + i * tw - e->tab_scroll, r.y, tw, r.h);
		if (t.x + t.w < r.x || t.x > r.x + avail)
			continue;
		int active = i == e->cur_tab, dirty = modified_doc(e->tabs[i]);
		fl_rect cr = fl_r(t.x + tw - 26, r.y + (r.h - 20) / 2, 20, 20);
		int over_tab = fl_in(fl_r(t.x, t.y, t.w, t.h), ui->mx, ui->my) && fl_in(fl_r(r.x, r.y, avail, r.h), ui->mx, ui->my);
		int over_x = over_tab && fl_in(cr, ui->mx, ui->my);
		int cres = fl_widget(ui, ID_TAB + (uint32_t)i * 2 + 1, over_tab ? cr : fl_r(0, 0, 0, 0), 0);
		int res = fl_widget(ui, ID_TAB + (uint32_t)i * 2, t, 0);
		int hover = (res & FL_HOVER) != 0;
		fl_fill_rect(cv, t, active ? fl_th.bg : hover ? fl_th.raised : fl_th.panel);
		fl_fill_rect(cv, fl_r(t.x + t.w - 1, t.y, 1, t.h - 1), fl_th.line);
		if (active)
			fl_fill_rect(cv, fl_r(t.x, t.y, t.w - 1, 2), fl_th.accent);
		char name[PATH_MAX], hint[NAME_MAX + 1], fit[PATH_MAX];
		tab_label(e, i, name, sizeof name, hint, sizeof hint);
		int room = tw - 14 - 30;
		int nw = fl_text_width(ui->f_reg, name, -1);
		fl_color tc = active ? fl_th.strong : fl_th.dim;
		int ty = r.y + (r.h - fl_font_height(ui->f_reg)) / 2 + (active ? 1 : 0);
		fl_rect tclip = cv->clip;
		fl_clip(cv, fl_r(t.x, t.y, tw - 28, t.h));
		if (nw <= room) {
			fl_text(cv, ui->f_reg, t.x + 14, ty, tc, name, -1);
			if (hint[0] && room - nw > 40) {
				char hf[NAME_MAX + 1];
				fl_text_fit(e->small, hint, room - nw - 10, hf, sizeof hf);
				fl_text(cv, e->small, t.x + 14 + nw + 8, r.y + (r.h - fl_font_height(e->small)) / 2 + 1, fl_th.dim, hf, -1);
			}
		} else {
			fl_text_fit(ui->f_reg, name, room, fit, sizeof fit);
			fl_text(cv, ui->f_reg, t.x + 14, ty, tc, fit, -1);
		}
		cv->clip = tclip;
		/* the dot when there are unsaved changes, the close cross when hovering or on the active tab */
		if (dirty && !over_tab) {
			draw_dot(cv, (float)cr.x + 10.0f, (float)cr.y + 10.0f, 4.0f, fl_th.accent);
		} else if (over_tab || active) {
			if (over_x)
				fl_fill_rect(cv, cr, fl_th.raised);
			fl_icon_draw(cv, FL_ICON_CLOSE, cr.x + 5, cr.y + 5, 10, over_x ? fl_th.strong : fl_th.dim);
		}
		if (over_tab && (ui->press & FL_BTN_M))
			*close = i;
		else if (over_x && (cres & FL_CLICK))
			*close = i;
		else if ((res & FL_CLICK) && !over_x)
			*sel = i;
	}
	cv->clip = save;
	if (fl_icon_button(ui, ID_NEWTAB, fl_r(r.x + r.w - plus_w + 2, r.y + 2, plus_w - 4, r.h - 5), FL_ICON_PLUS, 14, fl_th.dim))
		*add = 1;
}

/* Keys that work whatever has the focus: files, tabs, find, go to line. */
static void global_keys(fl_ui *ui, struct E *e)
{
	uint32_t k = ui->key;
	int shift = ui->mods & FL_MOD_SHIFT, ctrl = ui->mods & FL_MOD_CTRL, alt = ui->mods & FL_MOD_ALT;
	if (!k)
		return;
	int used = 1;
	if (ctrl && !alt) {
		switch (k) {
		case 's': case 'S':
			if (shift)
				open_path_dialog(e, 2, P_NONE);
			else
				save_now(e, P_NONE);
			break;
		case 'o': case 'O': perform(e, P_OPEN); break;
		case 'n': case 'N': case 't': case 'T': perform(e, P_NEW); break;
		case 'w': case 'W': request(e, P_CLOSE); break;
		case 'q': case 'Q': request(e, P_QUIT); break;
		case 'f': case 'F': open_find(e, 0); break;
		case 'h': case 'H': open_find(e, 1); break;
		case 'g': case 'G':
			e->ov = OV_GOTO;
			e->dlg_new = 1;
			e->dlg_err[0] = 0;
			e->dlg_text[0] = 0;
			break;
		case XKB_KEY_Tab: tab_select(e, (e->cur_tab + (shift ? e->ntabs - 1 : 1)) % e->ntabs); break;
		case XKB_KEY_ISO_Left_Tab: tab_select(e, (e->cur_tab + e->ntabs - 1) % e->ntabs); break;
		case XKB_KEY_Page_Down: tab_select(e, (e->cur_tab + 1) % e->ntabs); break;
		case XKB_KEY_Page_Up: tab_select(e, (e->cur_tab + e->ntabs - 1) % e->ntabs); break;
		default: used = 0; break;
		}
	} else if (alt && !ctrl && k >= '1' && k <= '9') {
		if ((int)(k - '1') < e->ntabs)
			tab_select(e, (int)(k - '1'));
	} else if (k == XKB_KEY_F3) {
		find_step(e, shift ? -1 : 1);
	} else {
		used = 0;
	}
	if (used) {
		ui->key = 0;
		ui->text[0] = 0;
		fl_win_redraw(ui->win);
	}
}

static void draw(fl_ui *ui, void *ud)
{
	struct E *e = ud;
	fl_canvas *cv = &ui->cv;
	fl_rect c = fl_win_content(ui->win);
	int modal = e->ov != OV_NONE;
	int sv_mouse = ui->mouse_in, sv_press = ui->press, sv_rel = ui->release, sv_wheel = ui->wheel, sv_held = ui->btn_held;
	uint32_t sv_key = ui->key;
	char sv_text[16];
	memcpy(sv_text, ui->text, sizeof sv_text);
	if (modal) {
		ui->mouse_in = ui->press = ui->release = ui->wheel = ui->btn_held = 0;
		ui->key = 0;
		ui->text[0] = 0;
	} else {
		global_keys(ui, e);
	}
	make_palette();

	struct layout l;
	make_layout(e, c, &l);
	int active = !modal && (ui->focus == 0 || ui->focus == ID_TEXT);

	handle_mouse(ui, e, &l, active);
	if (active)
		handle_key(ui, e, &l);
	/* the document may have changed: lay out again */
	make_layout(e, c, &l);
	if (e->reveal) {
		reveal_cursor(e, &l);
		e->reveal = 0;
	}
	clamp_scroll(e, &l);

	struct Doc *d = e->d;
	fl_fill_rect(cv, c, fl_th.bg);
	/* text */
	fl_rect save = cv->clip;
	fl_clip(cv, l.text);
	size_t first = (size_t)(d->vscroll / e->lh);
	size_t cur_line = line_of(e, d->cur);
	size_t sa = 0, sb = 0;
	int sel = has_sel(e);
	if (sel)
		sel_range(e, &sa, &sb);
	int finding = e->find_open && e->needle[0];
	if (finding)
		re_prepare(e);
	/* the bracket next to the cursor and its partner */
	size_t br1 = NO_SEL, br2 = NO_SEL;
	int br_bad = 0;
	if (!sel && d->len) {
		size_t cand[2] = {d->cur, d->cur ? d->cur - 1 : NO_SEL};
		for (int i = 0; i < 2 && br1 == NO_SEL; i++) {
			if (cand[i] == NO_SEL || cand[i] >= d->len)
				continue;
			size_t m;
			int r = bracket_match(e, cand[i], &m);
			if (r >= 0) {
				br1 = cand[i];
				if (r == 1)
					br2 = m;
				else
					br_bad = 1;
			}
		}
	}
	static uint8_t cls_buf[HL_LINE];
	for (size_t ln = first; ln < d->nl; ln++) {
		int y = l.text.y + (int)((long)ln * e->lh - d->vscroll);
		if (y >= l.text.y + l.text.h)
			break;
		fl_rect row = fl_r(l.text.x, y, l.text.w, e->lh);
		if (ln == cur_line && !sel)
			fl_fill_rect(cv, row, fl_th.panel);
		size_t ls = d->ls[ln], le = line_end(e, ln);
		if (sel && sb > ls && sa <= le) {
			size_t a = sa > ls ? sa : ls, b = sb < le ? sb : le;
			int x0 = l.text.x + col_of(e, ln, a) * e->cell - d->hscroll;
			int x1 = l.text.x + col_of(e, ln, b) * e->cell - d->hscroll;
			if (sb > le && ln + 1 < d->nl)
				x1 += e->cell / 2 + 2; /* the line break is selected too */
			fl_fill_rect(cv, fl_r(x0, y, x1 - x0, e->lh), fl_th.select);
		}
		if (finding && le - ls < 4000 && !(e->f_regex && !e->re_ok)) {
			size_t p = ls, ms, me;
			while (match_in_line(e, ln, p, &ms, &me, NULL)) {
				p = me;
				if (sel && ms >= sa && me <= sb)
					continue;
				int x0 = l.text.x + col_of(e, ln, ms) * e->cell - d->hscroll;
				int x1 = l.text.x + col_of(e, ln, me) * e->cell - d->hscroll;
				fl_fill_rect(cv, fl_r(x0, y + 1, x1 - x0, e->lh - 2), fl_mix(fl_th.bg, fl_th.accent, 90));
			}
		}
		for (int b = 0; b < 2; b++) { /* bracket pair */
			size_t bp = b ? br2 : br1;
			if (bp == NO_SEL || bp < ls || bp >= le + (le == d->len))
				continue;
			if (bp >= le)
				continue;
			int x0 = l.text.x + col_of(e, ln, bp) * e->cell - d->hscroll;
			fl_rect br = fl_r(x0, y + 2, e->cell, e->lh - 4);
			fl_fill_rect(cv, br, br_bad ? fl_mix(fl_th.bg, fl_th.err, 70) : fl_mix(fl_th.bg, fl_th.accent, 110));
			fl_stroke_rect(cv, br, 1, br_bad ? fl_th.err : fl_th.accent);
		}
		draw_line(ui, e, &l, ln, y, line_classes(e, ln, cls_buf));
	}
	/* the cursor: a 2 px accent bar */
	{
		int x = l.text.x + col_of(e, cur_line, d->cur) * e->cell - d->hscroll;
		int y = l.text.y + (int)((long)cur_line * e->lh - d->vscroll);
		fl_fill_rect(cv, fl_r(x, y + 1, 2, e->lh - 2), fl_th.accent);
	}
	cv->clip = save;

	/* gutter */
	fl_fill_rect(cv, l.gutter, fl_th.panel);
	fl_fill_rect(cv, fl_r(l.gutter.x + l.gutter.w - 1, l.gutter.y, 1, l.gutter.h), fl_th.line);
	save = cv->clip;
	fl_clip(cv, l.gutter);
	for (size_t ln = first; ln < d->nl; ln++) {
		int y = l.text.y + (int)((long)ln * e->lh - d->vscroll);
		if (y >= l.gutter.y + l.gutter.h)
			break;
		char num[24];
		snprintf(num, sizeof num, "%zu", ln + 1);
		int w = fl_text_width(e->num, num, -1);
		int ty = y + (e->lh - fl_font_height(e->num)) / 2;
		fl_text(cv, e->num, l.gutter.x + l.gutter.w - 12 - w, ty, ln == cur_line ? fl_th.text : fl_th.dim, num, -1);
	}
	cv->clip = save;

	/* vertical scrollbar */
	fl_scrollbar(ui, ID_VSB, fl_r(l.vsb.x + 3, l.vsb.y + 2, 4, l.vsb.h - 4), &d->vscroll, (int)(d->nl * e->lh), l.view_h);
	if (e->find_open)
		draw_find(ui, e, l.find);
	draw_status(ui, e, l.status);
	int tsel, tclose, tadd;
	draw_tabs(ui, e, l.tabs, &tsel, &tclose, &tadd);
	update_title(e);
	sess_note(e);

	if (modal) {
		ui->mouse_in = sv_mouse;
		ui->press = sv_press;
		ui->release = sv_rel;
		ui->wheel = sv_wheel;
		ui->btn_held = sv_held;
		ui->key = sv_key;
		memcpy(ui->text, sv_text, sizeof sv_text);
		draw_overlay(ui, e);
		if (e->ov == OV_NONE) {
			ui->focus = 0;
			fl_win_redraw(ui->win);
		}
	} else if (e->ov != OV_NONE) {
		fl_win_redraw(ui->win);
	}
	/* what the tab strip asked for happens after the drawing, since it changes e->d */
	if (!modal) {
		if (tsel >= 0 && tsel != e->cur_tab) {
			tab_select(e, tsel);
			session_save(e);
		} else if (tclose >= 0) {
			tab_select(e, tclose);
			request(e, P_CLOSE);
		} else if (tadd) {
			perform(e, P_NEW);
		}
		if (tsel >= 0 || tclose >= 0 || tadd)
			fl_win_redraw(ui->win);
	}
	if (e->reveal)
		fl_win_redraw(ui->win);
}

static void closed(fl_win *w, void *ud)
{
	request(ud, P_QUIT);
}

static void focus_changed(fl_win *w, int focused, void *ud)
{
	if (!focused)
		session_save(ud); /* the cursor positions survive a crash or a power cut */
}

int main(int argc, char **argv)
{
	if (argc >= 2 && argv[1][0] == '-' && argv[1][1]) {
		fprintf(stderr, "usage: fledit [FILE...]\nA small text editor. Ctrl-S saves, Ctrl-O opens, Ctrl-F finds, Ctrl-G goes to a line.\n");
		return !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") ? 0 : 2;
	}
	signal(SIGCHLD, SIG_IGN);
	struct E *e = calloc(1, sizeof *e);
	if (!e)
		return 1;
	e->app = fl_app_new(NULL, NULL);
	if (!e->app) {
		fprintf(stderr, "fledit: cannot connect to a Wayland compositor\n");
		return 1;
	}
	e->mono = fl_font_mono(400, 13.5f);
	e->bold = fl_font_mono(600, 13.5f);
	e->num = fl_font_mono(400, 12);
	e->small = fl_font_ui(400, 12);
	e->cell = fl_font_cell(e->mono);
	e->lh = fl_font_height(e->mono) + 6;
	e->msg_timer = fl_timer_new(e->app, msg_clear, e);
	e->sess_timer = fl_timer_new(e->app, sess_fire, e);
	e->count = -1;
	e->primary = argc < 2;

	fl_win_opts o = {
		.title = "Untitled", .app_id = "fledit", .w = 960, .h = 620, .min_w = 640, .min_h = 300,
		.decorated = 1, .resizable = 1, .draw = draw, .close = closed, .focus = focus_changed, .ud = e,
	};
	e->win = fl_win_new(e->app, &o);
	if (!e->win)
		return 1;

	session_load();
	char err[300];
	if (e->primary) {
		/* no files given: the ones that were open last time, each at its last cursor position */
		int active = -1;
		for (int i = 0; i < nsess; i++) {
			struct stat st;
			if (stat(sess_paths[i], &st) < 0 || !S_ISREG(st.st_mode))
				continue;
			if (tab_open(e, sess_paths[i], 0, err, sizeof err) == 0 && i == sess_active)
				active = e->cur_tab;
		}
		if (active >= 0)
			tab_select(e, active);
	} else {
		for (int i = 1; i < argc; i++)
			if (tab_open(e, argv[i], 1, err, sizeof err) < 0 && e->ov == OV_NONE) {
				snprintf(e->dlg_err, sizeof e->dlg_err, "%s", err);
				e->ov = OV_ERROR;
			}
	}
	if (!e->ntabs)
		tab_new(e);
	update_title(e);
	fl_app_watch_conf(e->app, NULL, NULL);
	fl_app_run(e->app);
	fl_app_free(e->app);
	return 0;
}
