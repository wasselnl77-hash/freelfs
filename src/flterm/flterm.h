/* flterm.h -- the terminal emulator core (no UI in here)
 * SPDX-License-Identifier: MIT
 */
#ifndef FLTERM_H
#define FLTERM_H

#include <stddef.h>
#include <stdint.h>

enum {
	A_BOLD = 1, A_DIM = 2, A_ITALIC = 4, A_UNDER = 8, A_REVERSE = 16, A_STRIKE = 32,
	A_WIDE = 64, /* first half of a double-width character */
	A_CONT = 128 /* second half: draws nothing */
};

struct cell {
	uint32_t cp;
	uint32_t fg, bg; /* 0 = the default colour, else 0xff000000 | rgb */
	uint8_t attr;
};

struct row {
	struct cell *c;
	int cols;
};

#define SCROLLBACK 2000

struct term {
	int cols, rows;
	struct row *scr;       /* the visible screen: rows entries */
	struct row *main_scr;  /* parked while the alternate screen is up */
	int alt;
	struct row *sb;        /* scrollback ring */
	int sb_head, sb_len;
	int cx, cy, wrap_pending;
	uint32_t fg, bg;
	uint8_t attr;
	int top, bottom;       /* scroll region, inclusive */
	/* saved cursor */
	int sx, sy;
	uint32_t sfg, sbg;
	uint8_t sattr;
	int s_origin, s_special, s_wrap_pending;
	/* modes */
	int autowrap, origin, insert, appcursor, bracketed, cursor_visible, lnm;
	int special; /* G0 is DEC special graphics */
	unsigned char *tabs;
	uint32_t last_cp;
	/* parser */
	int st;
	int params[16], nparams;
	int sub_colon;
	char priv, inter;
	uint32_t utf, utf_left;
	char osc[256];
	int osc_len;
	/* results for the owner */
	int out_fd;            /* answers to queries go here */
	void (*on_title)(void *ud, const char *title);
	void *ud;
	int scrolled;          /* lines moved into scrollback since the owner last looked */
};

struct term *term_new(int cols, int rows);
void term_free(struct term *t);
void term_resize(struct term *t, int cols, int rows);
void term_feed(struct term *t, const char *buf, size_t n);
/* A line for drawing: v is 0 at the top of the view, off lines back in the scrollback. */
const struct row *term_view_row(const struct term *t, int off, int v);
/* The text of the cells between two points (line numbers count scrollback + screen). */
char *term_text(const struct term *t, int x0, int y0, int x1, int y1);
int term_cwidth(uint32_t cp);

#endif
