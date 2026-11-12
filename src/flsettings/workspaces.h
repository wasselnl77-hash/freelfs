/* workspaces.h -- the Workspaces page of flsettings: how many, as a grid
 * SPDX-License-Identifier: MIT
 */
#ifndef WORKSPACES_H
#define WORKSPACES_H
#include <flui.h>

struct workspaces;

struct workspaces *ws_new(fl_app *app, fl_font *head, fl_font *small);
void ws_free(struct workspaces *w);
/* Draws into c (offset by the caller's scrolling); returns the height it needs. */
int ws_draw(struct workspaces *w, fl_ui *ui, fl_rect c);

/* workspaces= in desktop.conf as flcomp reads it: COLSxROWS, a bare N is N x 1, each 1..8; 2x2 when
 * the key is missing. ws_grid_set writes COLSxROWS (clamped) with fl_conf_set, flcomp applies it at
 * once. */
void ws_grid_get(int *cols, int *rows);
int ws_grid_set(int cols, int rows);

#endif
