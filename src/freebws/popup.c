/* popup -- see popup.h. SPDX-License-Identifier: MIT */
#include "popup.h"

#include <stdlib.h>
#include <wayland-client.h>

struct popup {
	fl_win *win;
	struct wl_subsurface *sub;
	int visible, x, y, w, h;
};

struct popup *popup_new(fl_app *app, struct wl_subcompositor *subc, struct wl_surface *parent, struct wl_surface *above,
			void (*draw)(fl_ui *ui, void *ud), void *ud)
{
	struct popup *p = calloc(1, sizeof(*p));
	if (!p)
		return NULL;
	fl_win_opts o = { .title = "", .app_id = "freebws", .w = 200, .h = 100, .raw = 1, .draw = draw, .ud = ud };
	p->win = fl_win_new(app, &o);
	if (!p->win) {
		free(p);
		return NULL;
	}
	struct wl_surface *s = fl_win_surface(p->win);
	if (s && subc && parent) {	/* no surface with FLUI_PPM: nothing to place */
		p->sub = wl_subcompositor_get_subsurface(subc, s, parent);
		wl_subsurface_set_desync(p->sub);
		if (above)
			wl_subsurface_place_above(p->sub, above);
	}
	return p;
}

void popup_show(struct popup *p, int x, int y, int w, int h)
{
	if (!p)
		return;
	p->x = x;
	p->y = y;
	p->w = w;
	p->h = h;
	if (p->sub)
		wl_subsurface_set_position(p->sub, x, y);
	fl_win_resize(p->win, w, h);
	if (!p->visible) {
		fl_win_map(p->win);
		p->visible = 1;
	}
	fl_win_redraw(p->win);
}

void popup_hide(struct popup *p)
{
	if (!p || !p->visible)
		return;
	p->visible = 0;
	fl_win_hide(p->win);
}

int popup_visible(const struct popup *p) { return p && p->visible; }
fl_rect popup_rect(const struct popup *p) { return fl_r(p->x, p->y, p->w, p->h); }
void popup_redraw(struct popup *p)
{
	if (p && p->visible)
		fl_win_redraw(p->win);
}
fl_win *popup_win(struct popup *p) { return p->win; }
