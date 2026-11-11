/* datetime.h -- the Date & time page of flsettings
 * SPDX-License-Identifier: MIT
 */
#ifndef DATETIME_H
#define DATETIME_H
#include <flui.h>

struct datetime;

struct datetime *dt_new(fl_app *app, fl_font *head, fl_font *small);
void dt_free(struct datetime *d);
void dt_set_win(struct datetime *d, fl_win *win);
/* The page became visible or hidden: reads what may have changed, starts or stops the minute timer. */
void dt_show(struct datetime *d, int visible);
/* Draws into c (offset by the caller's scrolling); returns the height it needs. */
int dt_draw(struct datetime *d, fl_ui *ui, fl_rect c);

#endif
