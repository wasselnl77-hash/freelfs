/* snapshots.h -- the Snapshots page of flsettings
 * SPDX-License-Identifier: MIT
 */
#ifndef SNAPSHOTS_H
#define SNAPSHOTS_H
#include <flui.h>

struct snapshots;

struct snapshots *snap_new(fl_app *app, fl_font *head, fl_font *small);
void snap_free(struct snapshots *s);
void snap_set_win(struct snapshots *s, fl_win *win);
/* The page became visible or hidden: visible re-reads the list from flsnap. */
void snap_show(struct snapshots *s, int visible);
/* Draws into c (offset by the caller's scrolling); returns the height it needs. */
int snap_draw(struct snapshots *s, fl_ui *ui, fl_rect c);

#endif
