/* fl_shell: the private protocol between flcomp and flpanel/flsettings.
 * SPDX-License-Identifier: MIT */
#include "comp.h"
#include "fl-shell-protocol.h"

#include <stdlib.h>

static void for_shells(void (*fn)(struct wl_resource *, void *), void *arg)
{
	struct reslink *l;
	wl_list_for_each(l, &C.shells, link)
		fn(l->res, arg);
}

static void ev_added(struct wl_resource *r, void *a)
{
	struct window *w = a;
	fl_shell_send_window_added(r, w->id, w->app_id, w->title);
}

static void ev_changed(struct wl_resource *r, void *a)
{
	struct window *w = a;
	fl_shell_send_window_changed(r, w->id, w->title, C.focused == w, w->minimized);
}

static void ev_removed(struct wl_resource *r, void *a)
{
	fl_shell_send_window_removed(r, ((struct window *)a)->id);
}

static void ev_dismissed(struct wl_resource *r, void *a)
{
	fl_shell_send_menu_dismissed(r);
}

static void ev_fly_dismissed(struct wl_resource *r, void *a)
{
	if (wl_resource_get_version(r) >= 2)
		fl_shell_send_flyout_dismissed(r);
}

static void ev_volume_key(struct wl_resource *r, void *a)
{
	if (wl_resource_get_version(r) >= 2)
		fl_shell_send_volume_key(r, *(uint32_t *)a);
}

static void ev_toggle(struct wl_resource *r, void *a)
{
	fl_shell_send_menu_toggle(r);
}

static void ev_layout(struct wl_resource *r, void *a)
{
	fl_shell_send_keyboard_layout(r, seat_layout());
}

static void ev_output(struct wl_resource *r, void *a)
{
	if (wl_resource_get_version(r) >= 3)
		output_send_modes(r);
}

static void ev_lock_state(struct wl_resource *r, void *a)
{
	if (wl_resource_get_version(r) >= 4)
		fl_shell_send_lock_state(r, C.locked);
}

static void ev_workspaces(struct wl_resource *r, void *a)
{
	if (wl_resource_get_version(r) >= 5)
		fl_shell_send_workspaces(r, (uint32_t)C.ws_cols, (uint32_t)C.ws_rows, (uint32_t)C.ws_cur);
}

static void ev_window_ws(struct wl_resource *r, void *a)
{
	struct window *w = a;
	if (wl_resource_get_version(r) >= 5)
		fl_shell_send_window_workspace(r, w->id, (uint32_t)w->ws);
}

void fl_emit_workspaces(void) { for_shells(ev_workspaces, NULL); }
void fl_emit_window_ws(struct window *w) { for_shells(ev_window_ws, w); }
void fl_emit_lock_state(void) { for_shells(ev_lock_state, NULL); }
void fl_emit_output(void) { for_shells(ev_output, NULL); }
void fl_emit_added(struct window *w) { for_shells(ev_added, w); }
void fl_emit_changed(struct window *w) { for_shells(ev_changed, w); }
void fl_emit_removed(struct window *w) { for_shells(ev_removed, w); }
void fl_emit_menu_dismissed(void) { for_shells(ev_dismissed, NULL); }
void fl_emit_flyout_dismissed(void) { for_shells(ev_fly_dismissed, NULL); }
void fl_emit_volume_key(uint32_t action) { for_shells(ev_volume_key, &action); }
void fl_emit_menu_toggle(void) { for_shells(ev_toggle, NULL); }
void fl_emit_layout(void) { for_shells(ev_layout, NULL); }

/* ---- requests ------------------------------------------------------------ */

static void sh_destroy(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static void sh_set_panel(struct wl_client *c, struct wl_resource *r, struct wl_resource *sr,
			 uint32_t height)
{
	struct surface *s = wl_resource_get_user_data(sr);
	if (s->role != ROLE_NONE && s->role != ROLE_PANEL) {
		wl_resource_post_error(r, FL_SHELL_ERROR_ROLE, "surface already has a role");
		return;
	}
	s->role = ROLE_PANEL;
	C.panel = s;
	C.panel_h = height > (uint32_t)C.h / 2 ? C.h / 2 : (int)height;
	work_area_changed();
	damage_all();
}

static void sh_set_menu(struct wl_client *c, struct wl_resource *r, struct wl_resource *sr)
{
	struct surface *s = wl_resource_get_user_data(sr);
	if (s->role != ROLE_NONE && s->role != ROLE_MENU) {
		wl_resource_post_error(r, FL_SHELL_ERROR_ROLE, "surface already has a role");
		return;
	}
	s->role = ROLE_MENU;
	C.menu = s;
	C.menu_dismissed = false;
}

static void sh_set_flyout(struct wl_client *c, struct wl_resource *r, struct wl_resource *sr, int32_t x)
{
	struct surface *s = wl_resource_get_user_data(sr);
	if (s->role != ROLE_NONE && s->role != ROLE_FLYOUT) {
		wl_resource_post_error(r, FL_SHELL_ERROR_ROLE, "surface already has a role");
		return;
	}
	s->role = ROLE_FLYOUT;
	if (C.flyout == s && C.flyout->px && !C.flyout_dismissed)
		damage_tree(s); /* the old place */
	C.flyout = s;
	C.flyout_x = x;
	if (s->px && !C.flyout_dismissed)
		damage_tree(s);
}

static void sh_activate(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	struct window *w = window_by_id(id);
	if (w)
		window_activate(w);
}

static void sh_minimize(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	struct window *w = window_by_id(id);
	if (w)
		window_minimize(w);
}

static void sh_close(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	struct window *w = window_by_id(id);
	if (w)
		window_close(w);
}

static void sh_set_layout(struct wl_client *c, struct wl_resource *r, const char *name)
{
	seat_set_layout(name);
}

static void sh_logout(struct wl_client *c, struct wl_resource *r)
{
	quit(0);
}

static void sh_set_output(struct wl_client *c, struct wl_resource *r, uint32_t w, uint32_t h,
			  uint32_t hz, uint32_t scale)
{
	if (w <= 16384 && h <= 16384 && hz <= 1000 && scale <= 1000 && output_set((int)w, (int)h, (int)hz, (int)scale)) {
		output_watch_client(c);
	} else {
		struct reslink *l;
		wl_list_for_each(l, &C.shells, link)
			if (wl_resource_get_version(l->res) >= 3)
				fl_shell_send_output_failed(l->res);
	}
}

static void sh_confirm_output(struct wl_client *c, struct wl_resource *r)
{
	output_confirm();
}

static void sh_revert_output(struct wl_client *c, struct wl_resource *r)
{
	output_revert();
}

static void sh_show_osd(struct wl_client *c, struct wl_resource *r, uint32_t kind, uint32_t pct, uint32_t flags)
{
	if (kind <= OSD_MIC)
		osd_show((int)kind, pct > 100 ? 100 : (int)pct, flags & 1);
}

static void sh_set_notifications(struct wl_client *c, struct wl_resource *r, struct wl_resource *sr)
{
	struct surface *s = wl_resource_get_user_data(sr);
	if (s->role != ROLE_NONE && s->role != ROLE_NOTIFY) {
		wl_resource_post_error(r, FL_SHELL_ERROR_ROLE, "surface already has a role");
		return;
	}
	s->role = ROLE_NOTIFY;
	C.notify = s;
}

static void sh_lock(struct wl_client *c, struct wl_resource *r, struct wl_resource *sr)
{
	struct surface *s = wl_resource_get_user_data(sr);
	if (s->role != ROLE_NONE && s->role != ROLE_LOCK) {
		wl_resource_post_error(r, FL_SHELL_ERROR_ROLE, "surface already has a role");
		return;
	}
	if (C.locked && C.lock_surf && C.lock_surf != s) {
		wl_resource_post_error(r, FL_SHELL_ERROR_LOCKED, "the screen is already locked");
		return;
	}
	lock_begin(s, c);
}

static void sh_unlock(struct wl_client *c, struct wl_resource *r)
{
	lock_end(c);
}

static void sh_set_workspace(struct wl_client *c, struct wl_resource *r, uint32_t idx)
{
	if (idx < (uint32_t)ws_count())
		ws_switch((int)idx);
}

static void sh_move_window(struct wl_client *c, struct wl_resource *r, uint32_t id, uint32_t idx)
{
	struct window *w = window_by_id(id);
	if (w && idx < (uint32_t)ws_count())
		ws_move_window(w, (int)idx, false);
}

static void sh_toggle_overview(struct wl_client *c, struct wl_resource *r)
{
	overview_toggle();
}

static const struct fl_shell_interface shell_impl = {
	.set_workspace = sh_set_workspace,
	.move_window = sh_move_window,
	.toggle_overview = sh_toggle_overview,
	.set_notifications = sh_set_notifications,
	.lock = sh_lock,
	.unlock = sh_unlock,
	.show_osd = sh_show_osd,
	.set_output = sh_set_output,
	.confirm_output = sh_confirm_output,
	.revert_output = sh_revert_output,
	.destroy = sh_destroy,
	.set_panel = sh_set_panel,
	.set_menu = sh_set_menu,
	.set_flyout = sh_set_flyout,
	.activate = sh_activate,
	.minimize = sh_minimize,
	.close = sh_close,
	.set_keyboard_layout = sh_set_layout,
	.logout = sh_logout,
};

static void shell_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &fl_shell_interface, version, id);
	struct reslink *l = r ? reslink_add(&C.shells, r) : NULL;
	if (!l) {
		wl_client_post_no_memory(c);
		return;
	}
	wl_resource_set_implementation(r, &shell_impl, l, reslink_destroy);
	/* tell the newcomer what is already there */
	struct window *w;
	wl_list_for_each_reverse(w, &C.windows, link)
		if (w->announced) {
			ev_added(r, w);
			ev_changed(r, w);
			ev_window_ws(r, w);
		}
	ev_workspaces(r, NULL);
	ev_layout(r, NULL);
	ev_output(r, NULL);
	if (C.locked && version >= 4)
		fl_shell_send_lock_state(r, 1);
}

void flshell_init(void)
{
	wl_global_create(C.dpy, &fl_shell_interface, 5, NULL, shell_bind);
}
