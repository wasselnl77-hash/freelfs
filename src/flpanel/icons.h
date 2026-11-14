/* icons.h -- app icons from the freedesktop icon themes (icons.c)
 * SPDX-License-Identifier: MIT
 */
#ifndef FLPANEL_ICONS_H
#define FLPANEL_ICONS_H

#include <flui.h>

/* The icon of a Wayland app_id: its .desktop file's Icon=, looked up in the theme. Draws it at
 * (x, y), size x size px (16..48), and returns 1. Returns 0 when there is none; the caller then
 * draws its built-in glyph. */
int icon_draw_app(fl_canvas *cv, const char *app_id, int x, int y, int size);

/* An Icon= value (a theme icon name, or an absolute path to a PNG): same return value. */
int icon_draw_name(fl_canvas *cv, const char *name, int x, int y, int size);

/* Forgets the cached lookups and decoded pixels. The start menu calls it when it reloads. */
void icons_flush(void);

#endif
