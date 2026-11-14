/* pages.h -- the Mouse & touchpad and Keyboard pages, the non-screen parts of Display, and the look
 * shared by the Settings pages
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
/* The compositor's fl_shell binding (the keyboard layout; version 3 is the one that sends events). */
void pages_set_shell(struct pages *p, struct fl_shell *sh, unsigned version);
/* A page became visible: read what may have changed behind our back (PG_DISPLAY: the brightness). */
void pages_show(struct pages *p, int page);
/* PG_INPUT or PG_KEYBOARD: draws into c (offset by the caller's scrolling) and returns the height it needs. */
int pages_draw(struct pages *p, int page, fl_ui *ui, fl_rect c);
/* The Brightness and Wallpaper parts of the Display page, drawn by display.c below the screens. Returns y. */
int pages_draw_brightness(struct pages *p, fl_ui *ui, int x, int y, int w);

/* shared look: a heading with a rule, and a row's title with its dim line */
void pg_section(fl_ui *ui, fl_font *head, int x, int y, int w, const char *name);
void pg_row_text(fl_ui *ui, fl_font *small, int x, int y, int maxw, const char *title, const char *what);
int pg_choice(fl_ui *ui, uint32_t id, fl_rect r, const char *label, int on);
/* a choice that is not available: dim, does nothing */
void pg_choice_off(fl_ui *ui, fl_rect r, const char *label);
/* the flyout's slider: 1 when *v changed (Left/Right step, Home/End) */
int pg_slider(fl_ui *ui, uint32_t id, fl_rect r, int *v, int lo, int hi, int step);
/* a dropdown head with a chevron; 1 when clicked (the caller opens or shuts its list) */
int pg_dropdown(fl_ui *ui, uint32_t id, fl_rect r, const char *label, int open);
/* a busy indicator centred on cx, cy; phase advances each frame */
void pg_spinner(fl_ui *ui, int cx, int cy, int phase);
/* a line icon from SVG path data (24 x 24 grid, see PG_SVG_* in pages.c) at x, y, size px */
void pg_glyph(fl_ui *ui, const char *d, int x, int y, int size, fl_color c);
/* "Super+Left / Super+Right" as flat key chips (a + joins the keys of a combination, a / separates
 * alternatives); returns the end x. draw 0 only measures. */
int pg_key_chips(fl_ui *ui, fl_font *f, int x, int y, int h, const char *combo, int draw);

#define PG_SVG_BLUETOOTH "M7 7l10 10-5 5V2l5 5L7 17"
#define PG_SVG_PRINTER "M7 9V3h10v6 M7 17H4v-7h16v7h-3 M7 13h10v8H7z"

#endif
