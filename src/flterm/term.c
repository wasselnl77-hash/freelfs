/* term.c -- VT100/xterm subset: enough for flsh, vim, less, top
 * SPDX-License-Identifier: MIT
 *
 * Handles: UTF-8, colours (16, 256, 24-bit), cursor movement, scroll
 * regions, insert/delete, tabs, save/restore cursor, the alternate screen,
 * bracketed paste mode, DEC line drawing, title (OSC 0/2) and the replies
 * programs ask for (cursor position, device attributes). Not handled:
 * mouse reporting, sixel, double-height lines.
 */
#include "flterm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { S_GROUND, S_ESC, S_ESC_INTER, S_CSI, S_CSI_IGN, S_OSC, S_OSC_ESC, S_STR, S_STR_ESC };

/* ---- rows ----------------------------------------------------------------------------- */

static struct row row_new(int cols)
{
	struct row r = {calloc((size_t)cols, sizeof(struct cell)), cols};
	return r;
}

static void row_clear(struct row *r, uint32_t bg)
{
	for (int i = 0; i < r->cols; i++)
		r->c[i] = (struct cell){0, 0, bg, 0};
}

static struct row *screen_new(int cols, int rows)
{
	struct row *s = calloc((size_t)rows, sizeof(*s));
	for (int i = 0; i < rows; i++)
		s[i] = row_new(cols);
	return s;
}

static void screen_free(struct row *s, int rows)
{
	if (!s)
		return;
	for (int i = 0; i < rows; i++)
		free(s[i].c);
	free(s);
}

struct term *term_new(int cols, int rows)
{
	struct term *t = calloc(1, sizeof(*t));
	t->cols = cols;
	t->rows = rows;
	t->scr = screen_new(cols, rows);
	t->sb = calloc(SCROLLBACK, sizeof(struct row));
	t->tabs = calloc((size_t)cols + 1, 1);
	for (int i = 8; i < cols; i += 8)
		t->tabs[i] = 1;
	t->top = 0;
	t->bottom = rows - 1;
	t->autowrap = 1;
	t->cursor_visible = 1;
	t->out_fd = -1;
	return t;
}

void term_free(struct term *t)
{
	if (!t)
		return;
	screen_free(t->scr, t->rows);
	if (t->alt)
		screen_free(t->main_scr, t->rows);
	for (int i = 0; i < SCROLLBACK; i++)
		free(t->sb[i].c);
	free(t->sb);
	free(t->tabs);
	free(t);
}

static struct row *sb_at(const struct term *t, int i) /* 0 = oldest */
{
	return &t->sb[(t->sb_head + i) % SCROLLBACK];
}

static void sb_push(struct term *t, struct row r)
{
	if (t->sb_len == SCROLLBACK) {
		free(t->sb[t->sb_head].c);
		t->sb[t->sb_head] = r;
		t->sb_head = (t->sb_head + 1) % SCROLLBACK;
	} else {
		t->sb[(t->sb_head + t->sb_len) % SCROLLBACK] = r;
		t->sb_len++;
	}
	t->scrolled++;
}

const struct row *term_view_row(const struct term *t, int off, int v)
{
	if (off > t->sb_len)
		off = t->sb_len;
	if (v < off)
		return sb_at(t, t->sb_len - off + v);
	return &t->scr[v - off];
}

/* ---- resize ---------------------------------------------------------------------------- */

static void resize_rows(struct row *s, int rows, int cols)
{
	for (int i = 0; i < rows; i++) {
		if (s[i].cols == cols)
			continue;
		struct cell *n = calloc((size_t)cols, sizeof(struct cell));
		int keep = s[i].cols < cols ? s[i].cols : cols;
		memcpy(n, s[i].c, (size_t)keep * sizeof(struct cell));
		free(s[i].c);
		s[i].c = n;
		s[i].cols = cols;
	}
}

/* Give screen s (old rows) a new height. Shrinking drops blank-ish lines under the
 * cursor first, then pushes the top ones into the scrollback; growing pulls
 * lines back out of it. *cy follows the cursor's row. */
static struct row *fit_rows(struct term *t, struct row *s, int old, int rows, int *cy, int hist)
{
	if (rows == old)
		return s;
	struct row *n = calloc((size_t)rows, sizeof(*n));
	if (rows < old) {
		int drop = old - rows, below = old - 1 - *cy;
		int from_bottom = drop < below ? drop : below, from_top = drop - from_bottom;
		for (int i = 0; i < old; i++) {
			if (i < from_top) {
				if (hist)
					sb_push(t, s[i]);
				else
					free(s[i].c);
			} else if (i - from_top < rows) {
				n[i - from_top] = s[i];
			} else {
				free(s[i].c);
			}
		}
		*cy -= from_top;
	} else {
		int pull = 0;
		if (hist) {
			pull = rows - old < t->sb_len ? rows - old : t->sb_len;
			for (int i = 0; i < pull; i++) {
				struct row *src = sb_at(t, t->sb_len - 1 - i);
				n[pull - 1 - i] = *src;
				src->c = NULL;
			}
			t->sb_len -= pull;
			*cy += pull;
		}
		for (int i = 0; i < old; i++)
			n[pull + i] = s[i];
		for (int i = pull + old; i < rows; i++)
			n[i] = row_new(t->cols);
	}
	free(s);
	return n;
}

void term_resize(struct term *t, int cols, int rows)
{
	if (cols < 2)
		cols = 2;
	if (rows < 1)
		rows = 1;
	if (cols == t->cols && rows == t->rows)
		return;
	int old = t->rows, dummy = 0;
	if (t->alt) {
		t->main_scr = fit_rows(t, t->main_scr, old, rows, &dummy, 1);
		t->scr = fit_rows(t, t->scr, old, rows, &t->cy, 0);
		resize_rows(t->main_scr, rows, cols);
	} else {
		t->scr = fit_rows(t, t->scr, old, rows, &t->cy, 1);
	}
	resize_rows(t->scr, rows, cols);
	t->cols = cols;
	t->rows = rows;
	free(t->tabs);
	t->tabs = calloc((size_t)cols + 1, 1);
	for (int i = 8; i < cols; i += 8)
		t->tabs[i] = 1;
	t->top = 0;
	t->bottom = rows - 1;
	if (t->cx >= cols)
		t->cx = cols - 1;
	if (t->cy >= rows)
		t->cy = rows - 1;
	if (t->cy < 0)
		t->cy = 0;
	t->wrap_pending = 0;
}

/* ---- scrolling ---------------------------------------------------------------------------- */

static void scroll_up(struct term *t, int n)
{
	int h = t->bottom - t->top + 1;
	if (n > h)
		n = h;
	for (int k = 0; k < n; k++) {
		struct row gone = t->scr[t->top];
		memmove(&t->scr[t->top], &t->scr[t->top + 1], (size_t)(t->bottom - t->top) * sizeof(struct row));
		if (t->top == 0 && !t->alt) {
			sb_push(t, gone);
			t->scr[t->bottom] = row_new(t->cols);
			if (t->bg)
				row_clear(&t->scr[t->bottom], t->bg);
		} else {
			t->scr[t->bottom] = gone;
			row_clear(&t->scr[t->bottom], t->bg);
		}
	}
}

static void scroll_down(struct term *t, int n)
{
	int h = t->bottom - t->top + 1;
	if (n > h)
		n = h;
	for (int k = 0; k < n; k++) {
		struct row gone = t->scr[t->bottom];
		memmove(&t->scr[t->top + 1], &t->scr[t->top], (size_t)(t->bottom - t->top) * sizeof(struct row));
		t->scr[t->top] = gone;
		row_clear(&t->scr[t->top], t->bg);
	}
}

static void linefeed(struct term *t)
{
	if (t->cy == t->bottom)
		scroll_up(t, 1);
	else if (t->cy < t->rows - 1)
		t->cy++;
}

/* ---- characters ----------------------------------------------------------------------------- */

int term_cwidth(uint32_t c)
{
	if (c < 0x300)
		return 1;
	if ((c >= 0x300 && c < 0x370) || (c >= 0x200b && c <= 0x200f) || (c >= 0xfe00 && c <= 0xfe0f) ||
	    c == 0x20e3 || (c >= 0x1ab0 && c < 0x1b00) || (c >= 0x20d0 && c < 0x2100 && c < 0x20f1))
		return 0;
	if ((c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0xa4cf && c != 0x303f) ||
	    (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe30 && c <= 0xfe6f) ||
	    (c >= 0xff00 && c <= 0xff60) || (c >= 0xffe0 && c <= 0xffe6) || (c >= 0x1f300 && c <= 0x1f64f) ||
	    (c >= 0x1f900 && c <= 0x1f9ff) || (c >= 0x20000 && c <= 0x3fffd))
		return 2;
	return 1;
}

static const uint32_t special[32] = {
	0xa0, 0x25c6, 0x2592, 0x2409, 0x240c, 0x240d, 0x240a, 0xb0, 0xb1, 0x2424, 0x240b, 0x2518, 0x2510,
	0x250c, 0x2514, 0x253c, 0x23ba, 0x23bb, 0x2500, 0x23bc, 0x23bd, 0x251c, 0x2524, 0x2534, 0x252c,
	0x2502, 0x2264, 0x2265, 0x3c0, 0x2260, 0xa3, 0xb7,
};

static void put_char(struct term *t, uint32_t cp)
{
	if (t->special && cp >= 0x5f && cp <= 0x7e)
		cp = special[cp - 0x5f];
	int w = term_cwidth(cp);
	if (w == 0)
		return;
	t->last_cp = cp;
	if (t->wrap_pending && t->autowrap) {
		t->cx = 0;
		linefeed(t);
		t->wrap_pending = 0;
	}
	if (w == 2 && t->cx == t->cols - 1) {
		if (t->autowrap) {
			t->scr[t->cy].c[t->cx] = (struct cell){0, 0, t->bg, 0};
			t->cx = 0;
			linefeed(t);
		} else {
			return;
		}
	}
	struct row *r = &t->scr[t->cy];
	if (t->insert) {
		memmove(&r->c[t->cx + w], &r->c[t->cx], (size_t)(t->cols - t->cx - w) * sizeof(struct cell));
	}
	r->c[t->cx] = (struct cell){cp, t->fg, t->bg, (uint8_t)(t->attr | (w == 2 ? A_WIDE : 0))};
	if (w == 2)
		r->c[t->cx + 1] = (struct cell){0, t->fg, t->bg, A_CONT};
	t->cx += w;
	if (t->cx >= t->cols) {
		t->cx = t->cols - 1;
		t->wrap_pending = 1;
	}
}

/* ---- escape sequences ------------------------------------------------------------------------ */

static void reply(struct term *t, const char *s)
{
	if (t->out_fd >= 0)
		(void)!write(t->out_fd, s, strlen(s));
}

static void set_cursor(struct term *t, int x, int y)
{
	int top = t->origin ? t->top : 0, bot = t->origin ? t->bottom : t->rows - 1;
	if (t->origin)
		y += t->top;
	t->cx = x < 0 ? 0 : x >= t->cols ? t->cols - 1 : x;
	t->cy = y < top ? top : y > bot ? bot : y;
	t->wrap_pending = 0;
}

static void erase(struct term *t, int y, int x0, int x1) /* [x0, x1) */
{
	struct row *r = &t->scr[y];
	if (x1 > r->cols)
		x1 = r->cols;
	for (int x = x0; x < x1; x++)
		r->c[x] = (struct cell){0, 0, t->bg, 0};
}

static uint32_t palette16[16] = {
	0x2e343b, 0xe06c6c, 0x5fbf83, 0xd8b56a, 0x6fa8dc, 0xc58bd9, 0x56b6c2, 0xd8dce0,
	0x8a9199, 0xf08a8a, 0x7fd69d, 0xe6c88a, 0x8fbfee, 0xd6a5e6, 0x7acdd8, 0xf0f2f4,
};

static uint32_t color256(int n)
{
	if (n < 16)
		return 0xff000000u | palette16[n];
	if (n < 232) {
		static const int lv[6] = {0, 95, 135, 175, 215, 255};
		n -= 16;
		return 0xff000000u | lv[n / 36] << 16 | lv[n / 6 % 6] << 8 | lv[n % 6];
	}
	int g = 8 + (n - 232) * 10;
	return 0xff000000u | g << 16 | g << 8 | g;
}

static void sgr(struct term *t)
{
	if (t->nparams == 0) {
		t->params[0] = 0;
		t->nparams = 1;
	}
	for (int i = 0; i < t->nparams; i++) {
		int p = t->params[i];
		if (p == 0) {
			t->fg = t->bg = 0;
			t->attr = 0;
		} else if (p == 1) t->attr |= A_BOLD;
		else if (p == 2) t->attr |= A_DIM;
		else if (p == 3) t->attr |= A_ITALIC;
		else if (p == 4 || p == 21) t->attr |= A_UNDER;
		else if (p == 7) t->attr |= A_REVERSE;
		else if (p == 9) t->attr |= A_STRIKE;
		else if (p == 22) t->attr &= (uint8_t)~(A_BOLD | A_DIM);
		else if (p == 23) t->attr &= (uint8_t)~A_ITALIC;
		else if (p == 24) t->attr &= (uint8_t)~A_UNDER;
		else if (p == 27) t->attr &= (uint8_t)~A_REVERSE;
		else if (p == 29) t->attr &= (uint8_t)~A_STRIKE;
		else if (p >= 30 && p <= 37) t->fg = color256(p - 30);
		else if (p == 39) t->fg = 0;
		else if (p >= 40 && p <= 47) t->bg = color256(p - 40);
		else if (p == 49) t->bg = 0;
		else if (p >= 90 && p <= 97) t->fg = color256(p - 90 + 8);
		else if (p >= 100 && p <= 107) t->bg = color256(p - 100 + 8);
		else if (p == 38 || p == 48) {
			uint32_t c = 0;
			if (i + 2 < t->nparams && t->params[i + 1] == 5) {
				c = color256(t->params[i + 2] & 255);
				i += 2;
			} else if (i + 4 < t->nparams && t->params[i + 1] == 2) {
				c = 0xff000000u | (uint32_t)(t->params[i + 2] & 255) << 16 |
				    (uint32_t)(t->params[i + 3] & 255) << 8 | (uint32_t)(t->params[i + 4] & 255);
				i += 4;
			} else {
				break;
			}
			if (p == 38)
				t->fg = c;
			else
				t->bg = c;
		}
	}
}

static void alt_screen(struct term *t, int on, int clear_and_save)
{
	if (on == t->alt)
		return;
	if (on) {
		if (clear_and_save) {
			t->sx = t->cx;
			t->sy = t->cy;
		}
		t->main_scr = t->scr;
		t->scr = screen_new(t->cols, t->rows);
		t->alt = 1;
	} else {
		screen_free(t->scr, t->rows);
		t->scr = t->main_scr;
		t->main_scr = NULL;
		t->alt = 0;
		if (clear_and_save) {
			t->cx = t->sx;
			t->cy = t->sy;
			if (t->cx >= t->cols)
				t->cx = t->cols - 1;
			if (t->cy >= t->rows)
				t->cy = t->rows - 1;
		}
	}
	t->top = 0;
	t->bottom = t->rows - 1;
	t->wrap_pending = 0;
}

static void set_mode(struct term *t, int priv, int p, int on)
{
	if (priv) {
		switch (p) {
		case 1: t->appcursor = on; break;
		case 6: t->origin = on; set_cursor(t, 0, 0); break;
		case 7: t->autowrap = on; break;
		case 25: t->cursor_visible = on; break;
		case 47:
		case 1047: alt_screen(t, on, 0); break;
		case 1049: alt_screen(t, on, 1); break;
		case 2004: t->bracketed = on; break;
		default: break;
		}
	} else if (p == 4) {
		t->insert = on;
	} else if (p == 20) {
		t->lnm = on;
	}
}

static int arg(struct term *t, int i, int def)
{
	if (i >= t->nparams || t->params[i] == 0)
		return def;
	return t->params[i];
}

static void csi(struct term *t, int f)
{
	int n = arg(t, 0, 1);
	struct row *r = &t->scr[t->cy];
	if (t->priv && f != 'h' && f != 'l' && f != 'n' && f != 'c')
		return; /* private sequences we do not know: ignore, above all not SGR */
	switch (f) {
	case 'A': set_cursor(t, t->cx, (t->origin ? t->cy - t->top : t->cy) - n); break;
	case 'B':
	case 'e': set_cursor(t, t->cx, (t->origin ? t->cy - t->top : t->cy) + n); break;
	case 'C':
	case 'a': set_cursor(t, t->cx + n, t->origin ? t->cy - t->top : t->cy); break;
	case 'D': set_cursor(t, t->cx - n, t->origin ? t->cy - t->top : t->cy); break;
	case 'E': set_cursor(t, 0, (t->origin ? t->cy - t->top : t->cy) + n); break;
	case 'F': set_cursor(t, 0, (t->origin ? t->cy - t->top : t->cy) - n); break;
	case 'G':
	case '`': set_cursor(t, n - 1, t->origin ? t->cy - t->top : t->cy); break;
	case 'd': set_cursor(t, t->cx, n - 1); break;
	case 'H':
	case 'f': set_cursor(t, arg(t, 1, 1) - 1, arg(t, 0, 1) - 1); break;
	case 'I':
		for (int k = 0; k < n; k++) {
			do t->cx++; while (t->cx < t->cols - 1 && !t->tabs[t->cx]);
		}
		if (t->cx >= t->cols)
			t->cx = t->cols - 1;
		t->wrap_pending = 0;
		break;
	case 'Z':
		for (int k = 0; k < n; k++) {
			do t->cx--; while (t->cx > 0 && !t->tabs[t->cx]);
		}
		if (t->cx < 0)
			t->cx = 0;
		t->wrap_pending = 0;
		break;
	case 'J': {
		int m = t->nparams ? t->params[0] : 0;
		if (m == 0) {
			erase(t, t->cy, t->cx, t->cols);
			for (int y = t->cy + 1; y < t->rows; y++)
				erase(t, y, 0, t->cols);
		} else if (m == 1) {
			for (int y = 0; y < t->cy; y++)
				erase(t, y, 0, t->cols);
			erase(t, t->cy, 0, t->cx + 1);
		} else if (m == 2) {
			for (int y = 0; y < t->rows; y++)
				erase(t, y, 0, t->cols);
		} else if (m == 3 && !t->alt) {
			for (int i = 0; i < t->sb_len; i++)
				free(sb_at(t, i)->c), sb_at(t, i)->c = NULL;
			t->sb_len = 0;
			t->sb_head = 0;
		}
		break;
	}
	case 'K': {
		int m = t->nparams ? t->params[0] : 0;
		if (m == 0)
			erase(t, t->cy, t->cx, t->cols);
		else if (m == 1)
			erase(t, t->cy, 0, t->cx + 1);
		else if (m == 2)
			erase(t, t->cy, 0, t->cols);
		break;
	}
	case 'X': erase(t, t->cy, t->cx, t->cx + n); break;
	case '@':
		if (n > t->cols - t->cx)
			n = t->cols - t->cx;
		memmove(&r->c[t->cx + n], &r->c[t->cx], (size_t)(t->cols - t->cx - n) * sizeof(struct cell));
		erase(t, t->cy, t->cx, t->cx + n);
		break;
	case 'P':
		if (n > t->cols - t->cx)
			n = t->cols - t->cx;
		memmove(&r->c[t->cx], &r->c[t->cx + n], (size_t)(t->cols - t->cx - n) * sizeof(struct cell));
		erase(t, t->cy, t->cols - n, t->cols);
		break;
	case 'L':
	case 'M':
		if (t->cy >= t->top && t->cy <= t->bottom) {
			int save = t->top;
			t->top = t->cy;
			if (f == 'L')
				scroll_down(t, n);
			else
				scroll_up(t, n);
			t->top = save;
			t->cx = 0;
			t->wrap_pending = 0;
		}
		break;
	case 'S': scroll_up(t, n); break;
	case 'T': scroll_down(t, n); break;
	case 'b':
		for (int k = 0; k < n && k < 1000; k++)
			put_char(t, t->last_cp ? t->last_cp : ' ');
		break;
	case 'g':
		if (t->nparams == 0 || t->params[0] == 0)
			t->tabs[t->cx] = 0;
		else if (t->params[0] == 3)
			memset(t->tabs, 0, (size_t)t->cols + 1);
		break;
	case 'h':
	case 'l':
		for (int i = 0; i < (t->nparams ? t->nparams : 1); i++)
			set_mode(t, t->priv == '?', t->nparams ? t->params[i] : 0, f == 'h');
		break;
	case 'm':
		if (!t->priv && !t->inter)
			sgr(t);
		break;
	case 'n': {
		int m = t->nparams ? t->params[0] : 0;
		char buf[32];
		if (m == 5) {
			reply(t, "\033[0n");
		} else if (m == 6) {
			snprintf(buf, sizeof buf, "\033[%d;%dR", (t->origin ? t->cy - t->top : t->cy) + 1, t->cx + 1);
			reply(t, buf);
		}
		break;
	}
	case 'c':
		if (t->priv == '>')
			reply(t, "\033[>1;10;0c");
		else if (!t->priv)
			reply(t, "\033[?62;c");
		break;
	case 'r': {
		int top = arg(t, 0, 1) - 1, bot = arg(t, 1, t->rows) - 1;
		if (bot >= t->rows)
			bot = t->rows - 1;
		if (top < bot) {
			t->top = top;
			t->bottom = bot;
		}
		set_cursor(t, 0, 0);
		break;
	}
	case 's':
		t->sx = t->cx;
		t->sy = t->cy;
		t->sfg = t->fg;
		t->sbg = t->bg;
		t->sattr = t->attr;
		break;
	case 'u':
		t->cx = t->sx < t->cols ? t->sx : t->cols - 1;
		t->cy = t->sy < t->rows ? t->sy : t->rows - 1;
		t->wrap_pending = 0;
		break;
	default:
		break;
	}
}

static void save_cursor(struct term *t)
{
	t->sx = t->cx;
	t->sy = t->cy;
	t->sfg = t->fg;
	t->sbg = t->bg;
	t->sattr = t->attr;
	t->s_origin = t->origin;
	t->s_special = t->special;
	t->s_wrap_pending = t->wrap_pending;
}

static void restore_cursor(struct term *t)
{
	t->cx = t->sx < t->cols ? t->sx : t->cols - 1;
	t->cy = t->sy < t->rows ? t->sy : t->rows - 1;
	t->fg = t->sfg;
	t->bg = t->sbg;
	t->attr = t->sattr;
	t->origin = t->s_origin;
	t->special = t->s_special;
	t->wrap_pending = t->s_wrap_pending;
}

static void reset(struct term *t)
{
	for (int y = 0; y < t->rows; y++)
		erase(t, y, 0, t->cols);
	t->fg = t->bg = 0;
	t->attr = 0;
	t->top = 0;
	t->bottom = t->rows - 1;
	t->cx = t->cy = 0;
	t->autowrap = 1;
	t->origin = t->insert = t->appcursor = t->bracketed = t->lnm = t->special = 0;
	t->cursor_visible = 1;
	t->wrap_pending = 0;
	memset(t->tabs, 0, (size_t)t->cols + 1);
	for (int i = 8; i < t->cols; i += 8)
		t->tabs[i] = 1;
}

static void esc_final(struct term *t, int c)
{
	switch (c) {
	case '7': save_cursor(t); break;
	case '8': restore_cursor(t); break;
	case 'D': linefeed(t); break;
	case 'E':
		t->cx = 0;
		linefeed(t);
		t->wrap_pending = 0;
		break;
	case 'M':
		if (t->cy == t->top)
			scroll_down(t, 1);
		else if (t->cy > 0)
			t->cy--;
		break;
	case 'H': t->tabs[t->cx] = 1; break;
	case 'c': reset(t); break;
	default: break;
	}
}

static void osc_end(struct term *t)
{
	t->osc[t->osc_len] = 0;
	int code = atoi(t->osc);
	char *sep = strchr(t->osc, ';');
	if ((code == 0 || code == 2) && sep && t->on_title)
		t->on_title(t->ud, sep + 1);
}

static void control(struct term *t, unsigned char c)
{
	switch (c) {
	case '\a': break;
	case '\b':
		if (t->cx > 0)
			t->cx--;
		t->wrap_pending = 0;
		break;
	case '\t':
		do t->cx++; while (t->cx < t->cols - 1 && !t->tabs[t->cx]);
		if (t->cx >= t->cols)
			t->cx = t->cols - 1;
		t->wrap_pending = 0;
		break;
	case '\n':
	case '\v':
	case '\f':
		linefeed(t);
		if (t->lnm)
			t->cx = 0;
		t->wrap_pending = 0;
		break;
	case '\r':
		t->cx = 0;
		t->wrap_pending = 0;
		break;
	case 0x0e: t->special = 1; break; /* SO: G1, which we treat as line drawing */
	case 0x0f: t->special = 0; break;
	default: break;
	}
}

static void feed_byte(struct term *t, unsigned char c)
{
	switch (t->st) {
	case S_GROUND:
		if (t->utf_left) {
			if ((c & 0xc0) == 0x80) {
				t->utf = t->utf << 6 | (c & 0x3f);
				if (--t->utf_left == 0)
					put_char(t, t->utf);
				return;
			}
			t->utf_left = 0;
			put_char(t, 0xfffd);
		}
		if (c >= 0x20 && c < 0x7f) {
			put_char(t, c);
		} else if (c >= 0xc2 && c < 0xe0) {
			t->utf = c & 0x1f;
			t->utf_left = 1;
		} else if (c >= 0xe0 && c < 0xf0) {
			t->utf = c & 0x0f;
			t->utf_left = 2;
		} else if (c >= 0xf0 && c < 0xf5) {
			t->utf = c & 0x07;
			t->utf_left = 3;
		} else if (c == 0x1b) {
			t->st = S_ESC;
		} else if (c < 0x20) {
			control(t, c);
		}
		return;
	case S_ESC:
		if (c == '[') {
			t->st = S_CSI;
			t->nparams = 0;
			memset(t->params, 0, sizeof t->params);
			t->priv = t->inter = 0;
			t->sub_colon = 0;
		} else if (c == ']') {
			t->st = S_OSC;
			t->osc_len = 0;
		} else if (c == 'P' || c == '_' || c == '^' || c == 'X') {
			t->st = S_STR;
		} else if (c >= 0x20 && c <= 0x2f) {
			t->inter = (char)c;
			t->st = S_ESC_INTER;
		} else if (c == 0x1b) {
			t->st = S_ESC;
		} else if (c < 0x20) {
			control(t, c);
		} else {
			esc_final(t, c);
			t->st = S_GROUND;
		}
		return;
	case S_ESC_INTER:
		if (c >= 0x20 && c <= 0x2f)
			return;
		if (t->inter == '(')
			t->special = c == '0';
		t->st = S_GROUND;
		return;
	case S_CSI:
		if (c >= '0' && c <= '9') {
			if (t->nparams == 0)
				t->nparams = 1;
			if (t->nparams <= 16) {
				int *p = &t->params[t->nparams - 1];
				if (*p < 100000)
					*p = *p * 10 + (c - '0');
			}
		} else if (c == ';' || c == ':') {
			if (t->nparams == 0)
				t->nparams = 1;
			if (t->nparams < 16)
				t->nparams++;
		} else if (c >= '<' && c <= '?') {
			t->priv = (char)c;
		} else if (c >= 0x20 && c <= 0x2f) {
			t->inter = (char)c;
		} else if (c >= 0x40 && c <= 0x7e) {
			if (!t->inter || c == 'm')
				csi(t, c);
			t->st = S_GROUND;
		} else if (c == 0x1b) {
			t->st = S_ESC;
		} else if (c < 0x20) {
			control(t, c);
		} else {
			t->st = S_CSI_IGN;
		}
		return;
	case S_CSI_IGN:
		if (c >= 0x40 && c <= 0x7e)
			t->st = S_GROUND;
		return;
	case S_OSC:
		if (c == 0x07) {
			osc_end(t);
			t->st = S_GROUND;
		} else if (c == 0x1b) {
			t->st = S_OSC_ESC;
		} else if (t->osc_len < (int)sizeof t->osc - 1 && c >= 0x20) {
			t->osc[t->osc_len++] = (char)c;
		}
		return;
	case S_OSC_ESC:
		osc_end(t);
		t->st = c == '\\' ? S_GROUND : S_ESC;
		if (c != '\\')
			feed_byte(t, c);
		return;
	case S_STR:
		if (c == 0x07)
			t->st = S_GROUND;
		else if (c == 0x1b)
			t->st = S_STR_ESC;
		return;
	case S_STR_ESC:
		t->st = c == '\\' ? S_GROUND : S_STR;
		return;
	}
}

void term_feed(struct term *t, const char *buf, size_t n)
{
	for (size_t i = 0; i < n; i++)
		feed_byte(t, (unsigned char)buf[i]);
}

/* ---- text extraction --------------------------------------------------------------------------- */

static const struct row *abs_row(const struct term *t, int y)
{
	if (y < t->sb_len)
		return sb_at(t, y);
	if (y - t->sb_len < t->rows)
		return &t->scr[y - t->sb_len];
	return NULL;
}

char *term_text(const struct term *t, int x0, int y0, int x1, int y1)
{
	if (y1 < y0 || (y1 == y0 && x1 < x0)) {
		int a = x0, b = y0;
		x0 = x1, y0 = y1, x1 = a, y1 = b;
	}
	size_t cap = 256, n = 0;
	char *out = malloc(cap);
	for (int y = y0; y <= y1; y++) {
		const struct row *r = abs_row(t, y);
		if (!r)
			break;
		int a = y == y0 ? x0 : 0, b = y == y1 ? x1 : r->cols - 1;
		if (b >= r->cols)
			b = r->cols - 1;
		int last = b;
		while (last >= a && (r->c[last].cp == 0 || r->c[last].cp == ' ') && y != y1)
			last--;
		for (int x = a; x <= last; x++) {
			if (r->c[x].attr & A_CONT)
				continue;
			uint32_t cp = r->c[x].cp ? r->c[x].cp : ' ';
			if (n + 8 > cap) {
				cap *= 2;
				out = realloc(out, cap);
			}
			if (cp < 0x80)
				out[n++] = (char)cp;
			else if (cp < 0x800) {
				out[n++] = (char)(0xc0 | cp >> 6);
				out[n++] = (char)(0x80 | (cp & 0x3f));
			} else if (cp < 0x10000) {
				out[n++] = (char)(0xe0 | cp >> 12);
				out[n++] = (char)(0x80 | (cp >> 6 & 0x3f));
				out[n++] = (char)(0x80 | (cp & 0x3f));
			} else {
				out[n++] = (char)(0xf0 | cp >> 18);
				out[n++] = (char)(0x80 | (cp >> 12 & 0x3f));
				out[n++] = (char)(0x80 | (cp >> 6 & 0x3f));
				out[n++] = (char)(0x80 | (cp & 0x3f));
			}
		}
		if (y != y1) {
			if (n + 2 > cap)
				out = realloc(out, cap *= 2);
			out[n++] = '\n';
		}
	}
	out[n] = 0;
	return out;
}
