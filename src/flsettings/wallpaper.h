/* wallpaper.h -- Settings > Display > Wallpaper
 * SPDX-License-Identifier: MIT
 */
#ifndef WALLPAPER_H
#define WALLPAPER_H
#include <flui.h>
#include <stddef.h>

struct wallpaper;

struct wallpaper *wp_new(fl_app *app, fl_font *head, fl_font *small);
void wp_free(struct wallpaper *w);
void wp_set_win(struct wallpaper *w, fl_win *win);
/* The page became visible: look for imported pictures again. */
void wp_show(struct wallpaper *w);
/* Draws the Wallpaper section at x, y, width w; returns the y below it. */
int wp_draw(struct wallpaper *w, fl_ui *ui, int x, int y, int ww);

/* Convert a PNG or JPEG into ~/.local/share/freelfs/wallpapers/NAME.flw (scaled to fit 2560 x 1440,
 * never enlarged). 0: ok and name holds NAME; -1: err says why. */
int wp_import_file(const char *src, char *name, size_t ncap, char *err, size_t ecap);
/* Import and make it the wallpaper (wallpaper=user:NAME): `flsettings --set-wallpaper FILE`. */
int wp_set_from_file(const char *src, char *err, size_t ecap);

#endif
