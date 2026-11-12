/* pages.h -- the Display, Mouse & touchpad and Keyboard pages of flsettings
 * SPDX-License-Identifier: MIT
 */
#ifndef PAGES_H
#define PAGES_H
#include <flui.h>

struct fl_shell;
struct pages;

enum { PG_DISPLAY, PG_INPUT, PG_KEYBOARD };

struct pages *pages_new(void);
void pages_start(struct pages *p, fl_app *app, fl_font *head, fl_font *small);
void pages_free(struct pages *p);
void pages_set_win(struct pages *p, fl_win *win);
/* The compositor's fl_shell binding (version 3 has the display requests). */
void pages_set_shell(struct pages *p, struct fl_shell *sh, unsigned version);
/* A page became visible: read what may have changed behind our back. */
void pages_show(struct pages *p, int page);
/* Draws a page into c (offset by the caller's scrolling) and returns the height
 * it needs. */
int pages_draw(struct pages *p, int page, fl_ui *ui, fl_rect c);

/* shared look: a heading with a rule, and a row's title with its dim line */
void pg_section(fl_ui *ui, fl_font *head, int x, int y, int w, const char *name);
void pg_row_text(fl_ui *ui, fl_font *small, int x, int y, int maxw, const char *title, const char *what);
int pg_choice(fl_ui *ui, uint32_t id, fl_rect r, const char *label, int on);
/* "Super+Left / Super+Right" as flat key chips (a + joins the keys of a combination, a / separates
 * alternatives); returns the end x. draw 0 only measures. */
int pg_key_chips(fl_ui *ui, fl_font *f, int x, int y, int h, const char *combo, int draw);

#endif
