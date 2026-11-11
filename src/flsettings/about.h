/* about.h -- the About page of flsettings: what this computer is, and "Save a report"
 * SPDX-License-Identifier: MIT
 */
#ifndef ABOUT_H
#define ABOUT_H
#include <flui.h>

struct about;

struct about *about_new(fl_app *app, fl_font *head, fl_font *small);
void about_free(struct about *a);
void about_set_win(struct about *a, fl_win *win);
/* The page became visible: read the system's data again. */
void about_show(struct about *a);
/* Draws into c (offset by the caller's scrolling); returns the height it needs. */
int about_draw(struct about *a, fl_ui *ui, fl_rect c);

#endif
