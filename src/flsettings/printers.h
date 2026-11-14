/* printers.h -- the Printers page of flsettings (CUPS command-line tools)
 * SPDX-License-Identifier: MIT
 */
#ifndef PRINTERS_H
#define PRINTERS_H
#include <flui.h>

struct printers;

struct printers *pr_new(fl_app *app, fl_font *head, fl_font *small);
void pr_free(struct printers *p);
void pr_set_win(struct printers *p, fl_win *win);
/* The page became visible (the queues are read again) or hidden (the search results go). */
void pr_show(struct printers *p, int visible);
/* Draws into c (offset by the caller's scrolling); returns the height it needs. */
int pr_draw(struct printers *p, fl_ui *ui, fl_rect c);

#endif
