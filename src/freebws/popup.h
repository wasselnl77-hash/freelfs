/* popup -- a small libflui window that sits above the page as a subsurface of the browser
 * window (menus and panels have to be drawn above the page, which is a subsurface itself).
 * SPDX-License-Identifier: MIT */
#ifndef FB_POPUP_H
#define FB_POPUP_H

#include <flui.h>

struct wl_subcompositor;
struct wl_surface;
struct popup;

/* parent: the browser window's surface; above: the page's surface (the popup is placed over it) */
struct popup *popup_new(fl_app *app, struct wl_subcompositor *subc, struct wl_surface *parent, struct wl_surface *above,
			void (*draw)(fl_ui *ui, void *ud), void *ud);
/* x, y: where the top left goes in the parent's coordinates */
void popup_show(struct popup *p, int x, int y, int w, int h);
void popup_hide(struct popup *p);
int popup_visible(const struct popup *p);
fl_rect popup_rect(const struct popup *p);
void popup_redraw(struct popup *p);
fl_win *popup_win(struct popup *p);

#endif
