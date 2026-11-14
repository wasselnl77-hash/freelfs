/* display.h -- the Display page of flsettings: screens (fldisplay), night light, brightness, wallpaper
 * SPDX-License-Identifier: MIT
 */
#ifndef DISPLAY_H
#define DISPLAY_H
#include <flui.h>

struct pages;
struct display;

struct display *disp_new(fl_app *app, fl_font *head, fl_font *small, struct pages *pg);
void disp_free(struct display *d);
void disp_set_win(struct display *d, fl_win *win);
/* The page became visible (or hidden): fldisplay list runs again, the night light fields are read. */
void disp_show(struct display *d, int visible);
/* Draws into c (offset by the caller's scrolling); returns the height it needs. */
int disp_draw(struct display *d, fl_ui *ui, fl_rect c);

#endif
