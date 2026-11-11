/* wl_compositor, wl_surface, wl_region, wl_subcompositor.
 * Client buffers are copied into our own pixel store at commit and released
 * at once, so clients never wait for a buffer.
 * SPDX-License-Identifier: MIT */
#include "comp.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

/* ---- regions ---------------------------------------------------------- */

static void ops_push(struct regionops *o, struct rect r, bool add)
{
	struct regionop *v = realloc(o->v, (size_t)(o->n + 1) * sizeof(*v));
	if (!v)
		return;
	o->v = v;
	v[o->n].r = r;
	v[o->n].add = add;
	o->n++;
}

static bool ops_contains(const struct regionops *o, int x, int y)
{
	bool in = false;
	for (int i = 0; i < o->n; i++) {
		const struct rect *r = &o->v[i].r;
		if (x >= r->x1 && x < r->x2 && y >= r->y1 && y < r->y2)
			in = o->v[i].add;
	}
	return in;
}

static void region_destroy_req(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static void region_add(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y,
		       int32_t w, int32_t h)
{
	struct regionops *o = wl_resource_get_user_data(r);
	struct rect rc = { x, y, x + w, y + h };
	ops_push(o, rc, true);
}

static void region_sub(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y,
		       int32_t w, int32_t h)
{
	struct regionops *o = wl_resource_get_user_data(r);
	struct rect rc = { x, y, x + w, y + h };
	ops_push(o, rc, false);
}

static const struct wl_region_interface region_impl = {
	.destroy = region_destroy_req, .add = region_add, .subtract = region_sub,
};

static void region_free(struct wl_resource *r)
{
	struct regionops *o = wl_resource_get_user_data(r);
	free(o->v);
	free(o);
}

/* ---- geometry helpers ------------------------------------------------- */

void surface_origin(struct surface *s, int *x, int *y)
{
	int px = 0, py = 0;
	switch (s->role) {
	case ROLE_TOPLEVEL:
		if (s->win && s->xs) {
			px = s->win->x - s->xs->geom.x1;
			py = s->win->y - s->xs->geom.y1;
		}
		break;
	case ROLE_POPUP:
		if (s->popup && s->xs && s->popup->parent) {
			struct surface *p = s->popup->parent;
			surface_origin(p, &px, &py);
			if (p->xs) {
				px += p->xs->geom.x1;
				py += p->xs->geom.y1;
			}
			px += s->popup->x - s->xs->geom.x1;
			py += s->popup->y - s->xs->geom.y1;
		}
		break;
	case ROLE_SUB:
		if (s->sub && s->sub->parent) {
			surface_origin(s->sub->parent, &px, &py);
			px += s->sub->x;
			py += s->sub->y;
		}
		break;
	case ROLE_XWL:
		xwm_surface_origin(s, &px, &py);
		break;
	case ROLE_PANEL:
		py = C.h - C.panel_h;
		break;
	case ROLE_MENU:
		py = C.h - C.panel_h - s->h;
		break;
	case ROLE_FLYOUT:
		px = C.flyout_x - s->w / 2;
		if (px > C.w - s->w)
			px = C.w - s->w;
		if (px < 0)
			px = 0;
		py = C.h - C.panel_h - s->h;
		break;
	case ROLE_NOTIFY:
		px = C.w - s->w - 16;
		py = 16;
		break;
	case ROLE_DESKTOP:
		px = C.desktop_x;
		py = C.desktop_y;
		break;
	default:	/* ROLE_LOCK: 0, 0 */
		break;
	}
	*x = px;
	*y = py;
}

struct rect surface_rect(struct surface *s)
{
	int x, y;
	surface_origin(s, &x, &y);
	struct rect r = { x, y, x + s->w, y + s->h };
	return r;
}

struct rect tree_bounds(struct surface *s)
{
	struct rect r = surface_rect(s);
	if (s->role == ROLE_XWL && s->win && s->win->xw)	/* the frame flcomp draws around an X window */
		r = rect_union(r, xwm_deco_outer(s->win));
	struct subsurf *sub;
	wl_list_for_each(sub, &s->subs, link)
		if (sub->surf->px)
			r = rect_union(r, tree_bounds(sub->surf));
	return r;
}

void damage_tree(struct surface *s)
{
	damage_add(tree_bounds(s));
}

struct surface *root_of(struct surface *s)
{
	while (s && s->role == ROLE_SUB && s->sub && s->sub->parent)
		s = s->sub->parent;
	return s;
}

struct window *window_of_surface(struct surface *s)
{
	s = root_of(s);
	if (s && s->role == ROLE_POPUP && s->popup)
		s = root_of(s->popup->parent);
	return s ? s->win : NULL;
}

bool panel_visible(void)
{
	if (!C.panel || !C.panel->px)
		return false;
	struct window *w;
	wl_list_for_each(w, &C.windows, link) {
		if (window_visible(w))
			return !w->fullscreen;
	}
	return true;
}

bool menu_visible(void)
{
	return C.menu && C.menu->px && !C.menu_dismissed;
}

bool flyout_visible(void)
{
	return C.flyout && C.flyout->px && !C.flyout_dismissed;
}

bool popup_visible(struct popup *p)
{
	if (p->dismissed)
		return false;
	struct window *w = window_of_surface(p->surf);
	return !w || w->ws == C.ws_cur;
}

bool notify_visible(void)
{
	return C.notify && C.notify->px && !C.locked;
}

bool desktop_visible(void)
{
	return C.desktop && C.desktop->px && !C.locked;
}

/* ---- hit testing ------------------------------------------------------ */

static bool surface_has_point(struct surface *s, int lx, int ly)
{
	if (lx < 0 || ly < 0 || lx >= s->w || ly >= s->h)
		return false;
	/* the cards are a transparent surface: only what is drawn takes the click */
	if ((s->role == ROLE_NOTIFY || s->role == ROLE_DESKTOP) && s->alpha && (s->px[(size_t)ly * s->w + lx] >> 24) == 0)
		return false;
	return s->input_def || ops_contains(&s->input, lx, ly);
}

static struct surface *tree_pick(struct surface *s, int x, int y, int *lx, int *ly)
{
	if (!s->px)
		return NULL;
	struct subsurf *sub;
	wl_list_for_each_reverse(sub, &s->subs, link) {
		struct surface *r = tree_pick(sub->surf, x, y, lx, ly);
		if (r)
			return r;
	}
	int ox, oy;
	surface_origin(s, &ox, &oy);
	if (surface_has_point(s, x - ox, y - oy)) {
		*lx = x - ox;
		*ly = y - oy;
		return s;
	}
	return NULL;
}

struct surface *pick(int x, int y, int *lx, int *ly)
{
	struct surface *r;
	xwm_deco_note(NULL);	/* set again below if the pointer is on the frame of an X window */
	if (C.locked)	/* nothing but the lock screen gets the pointer */
		return C.lock_surf && C.lock_surf->px ? tree_pick(C.lock_surf, x, y, lx, ly) : NULL;
	if (notify_visible() && (r = tree_pick(C.notify, x, y, lx, ly)))
		return r;
	if (flyout_visible() && (r = tree_pick(C.flyout, x, y, lx, ly)))
		return r;
	if (menu_visible() && (r = tree_pick(C.menu, x, y, lx, ly)))
		return r;
	struct popup *p;
	wl_list_for_each_reverse(p, &C.popups, link)
		if (popup_visible(p) && (r = tree_pick(p->surf, x, y, lx, ly)))
			return r;
	struct surface *xo[64];
	int nxo = xwm_or_list(xo, 64);	/* menus and tooltips of X programs: above the windows */
	for (int i = nxo - 1; i >= 0; i--)
		if ((r = tree_pick(xo[i], x, y, lx, ly)))
			return r;
	if (panel_visible() && (r = tree_pick(C.panel, x, y, lx, ly)))
		return r;
	struct window *w;
	wl_list_for_each(w, &C.windows, link) {
		if (!window_visible(w))
			continue;
		if ((r = tree_pick(w->surf, x, y, lx, ly)))
			return r;
		if (w->xw && xwm_deco_contains(w, x, y)) {
			xwm_deco_note(w);	/* a frame: no surface, the seat handles it */
			return NULL;
		}
	}
	if (desktop_visible() && (r = tree_pick(C.desktop, x, y, lx, ly)))	/* the icons: below every window */
		return r;
	return NULL;
}

/* ---- frame callbacks -------------------------------------------------- */

static void frame_cb_destroy(struct wl_resource *res)
{
	struct frame_cb *cb = wl_resource_get_user_data(res);
	wl_list_remove(&cb->link);
	free(cb);
}

void flush_frames(uint32_t ms)
{
	struct surface *s;
	wl_list_for_each(s, &C.surfaces, link) {
		if (wl_list_empty(&s->frames))
			continue;
		if ((s->role == ROLE_TOPLEVEL && s->win && (s->win->minimized || (s->win->mapped && s->win->ws != C.ws_cur))) ||
		    (s->role == ROLE_MENU && !menu_visible()) ||
		    (s->role == ROLE_FLYOUT && !flyout_visible()))
			continue;
		struct frame_cb *cb, *tmp;
		wl_list_for_each_safe(cb, tmp, &s->frames, link) {
			wl_callback_send_done(cb->res, ms);
			wl_resource_destroy(cb->res);
		}
	}
}

/* ---- wl_surface ------------------------------------------------------- */

static void buf_destroyed(struct wl_listener *l, void *data)
{
	struct surface *s = wl_container_of(l, s, buf_destroy);
	wl_list_remove(&l->link);
	wl_list_init(&l->link);
	s->p_buf = NULL;
}

static void surf_destroy_req(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static void surf_attach(struct wl_client *c, struct wl_resource *r, struct wl_resource *buf,
			int32_t x, int32_t y)
{
	struct surface *s = wl_resource_get_user_data(r);
	wl_list_remove(&s->buf_destroy.link);
	wl_list_init(&s->buf_destroy.link);
	s->p_buf = buf;
	s->p_attached = true;
	if (buf)
		wl_resource_add_destroy_listener(buf, &s->buf_destroy);
}

static void add_damage(struct surface *s, int x, int y, int w, int h)
{
	struct rect d = { x, y, x + w, y + h };
	if (w <= 0 || h <= 0)
		return;
	s->p_dmg = s->p_dmg_set ? rect_union(s->p_dmg, d) : d;
	s->p_dmg_set = true;
}

static void surf_damage(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y,
			int32_t w, int32_t h)
{
	add_damage(wl_resource_get_user_data(r), x, y, w, h);
}

static void surf_frame(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	struct surface *s = wl_resource_get_user_data(r);
	struct frame_cb *cb = calloc(1, sizeof(*cb));
	if (!cb) {
		wl_client_post_no_memory(c);
		return;
	}
	cb->res = wl_resource_create(c, &wl_callback_interface, 1, id);
	if (!cb->res) {
		free(cb);
		wl_client_post_no_memory(c);
		return;
	}
	wl_resource_set_implementation(cb->res, NULL, cb, frame_cb_destroy);
	wl_list_insert(s->p_frames.prev, &cb->link);
}

static void surf_set_opaque(struct wl_client *c, struct wl_resource *r, struct wl_resource *reg)
{
	/* Only XRGB buffers count as opaque; a region adds nothing for us. */
}

static void surf_set_input(struct wl_client *c, struct wl_resource *r, struct wl_resource *reg)
{
	struct surface *s = wl_resource_get_user_data(r);
	free(s->p_input.v);
	s->p_input.v = NULL;
	s->p_input.n = 0;
	s->p_input_chg = true;
	s->p_input_def = reg == NULL;
	if (reg) {
		struct regionops *o = wl_resource_get_user_data(reg);
		for (int i = 0; i < o->n; i++)
			ops_push(&s->p_input, o->v[i].r, o->v[i].add);
	}
}

/* Copy the attached buffer. Returns the changed part of the surface. */
static bool apply_buffer(struct surface *s, struct rect *dmg, bool *full)
{
	struct wl_resource *b = s->p_buf;
	s->p_attached = false;
	s->p_buf = NULL;
	wl_list_remove(&s->buf_destroy.link);
	wl_list_init(&s->buf_destroy.link);
	if (!b) {
		surface_unmap(s);
		return false;
	}
	struct wl_shm_buffer *shm = wl_shm_buffer_get(b);
	struct dbuf *db = shm ? NULL : dbuf_get(b);
	if (!shm && !db) {
		wl_resource_post_error(s->res, WL_DISPLAY_ERROR_INVALID_OBJECT,
				       "only wl_shm and dmabuf buffers are supported");
		return false;
	}
	int bw = shm ? wl_shm_buffer_get_width(shm) : db->w;
	int bh = shm ? wl_shm_buffer_get_height(shm) : db->h;
	int stride = shm ? wl_shm_buffer_get_stride(shm) : 0;
	if (bw <= 0 || bh <= 0 || bw > 16384 || bh > 16384) {
		wl_resource_post_error(s->res, WL_DISPLAY_ERROR_INVALID_OBJECT, "bad buffer size");
		return false;
	}
	*full = !s->px || bw != s->w || bh != s->h || !s->p_dmg_set;
	if (!s->px || bw != s->w || bh != s->h) {
		uint32_t *np = malloc((size_t)bw * bh * 4);
		if (!np) {
			wl_client_post_no_memory(wl_resource_get_client(s->res));
			return false;
		}
		free(s->px);
		s->px = np;
		s->w = bw;
		s->h = bh;
	}
	s->alpha = shm ? wl_shm_buffer_get_format(shm) == WL_SHM_FORMAT_ARGB8888 : dbuf_has_alpha(db);
	struct rect all = { 0, 0, bw, bh };
	if (*full || !rect_intersect(s->p_dmg, all, dmg))
		*dmg = all;
	if (db) {
		if (!dbuf_read(db, s->px, *dmg))
			memset(s->px, 0, (size_t)bw * bh * 4);	/* unreadable: show nothing, not garbage */
		wl_buffer_send_release(b);
		return true;
	}
	wl_shm_buffer_begin_access(shm);
	const uint8_t *src = wl_shm_buffer_get_data(shm);
	for (int y = dmg->y1; y < dmg->y2; y++)
		memcpy(s->px + (size_t)y * bw + dmg->x1, src + (size_t)y * stride + dmg->x1 * 4,
		       (size_t)(dmg->x2 - dmg->x1) * 4);
	/* Let go of the client's pages again so they do not count against our
	 * resident size; they fault back in if the buffer is read again. */
	uintptr_t lo = ((uintptr_t)src + (size_t)dmg->y1 * stride + 4095) & ~(uintptr_t)4095;
	uintptr_t hi = ((uintptr_t)src + (size_t)dmg->y2 * stride) & ~(uintptr_t)4095;
	if (hi > lo)
		madvise((void *)lo, hi - lo, MADV_DONTNEED);
	wl_shm_buffer_end_access(shm);
	wl_buffer_send_release(b);
	return true;
}

void surface_unmap(struct surface *s)
{
	if (!s->px)
		return;
	damage_tree(s);
	free(s->px);
	s->px = NULL;
	s->w = s->h = 0;
	seat_surface_gone(s);
}

static void surf_commit(struct wl_client *c, struct wl_resource *r)
{
	struct surface *s = wl_resource_get_user_data(r);
	bool was_mapped = s->px != NULL;
	struct rect old = was_mapped ? tree_bounds(s) : (struct rect){ 0, 0, 0, 0 };
	int ox, oy;
	surface_origin(s, &ox, &oy);

	if (s->p_input_chg) {
		free(s->input.v);
		s->input = s->p_input;
		s->input_def = s->p_input_def;
		s->p_input.v = NULL;
		s->p_input.n = 0;
		s->p_input_chg = false;
	}

	struct rect dmg = { 0, 0, 0, 0 };
	bool full = false, content = false, attached = s->p_attached && s->p_buf;
	bool unmap = s->p_attached && !s->p_buf;
	if (s->p_attached)
		content = apply_buffer(s, &dmg, &full);
	s->p_dmg_set = false;

	wl_list_insert_list(&s->frames, &s->p_frames);
	wl_list_init(&s->p_frames);

	switch (s->role) {
	case ROLE_TOPLEVEL:
	case ROLE_POPUP:
		xdg_surface_commit(s, was_mapped);
		break;
	case ROLE_PANEL:
	case ROLE_MENU:
	case ROLE_FLYOUT:
	case ROLE_NOTIFY:
	case ROLE_DESKTOP:
		shell_surface_commit(s, was_mapped, attached);
		break;
	case ROLE_LOCK:
		lock_surface_committed(s);
		break;
	case ROLE_XWL:
		xwm_surface_commit(s, was_mapped);
		break;
	default:
		break;
	}

	if (!unmap && s->px) {
		int nx, ny;
		surface_origin(s, &nx, &ny);
		if (nx != ox || ny != oy || full) {
			/* moved or resized: redo the whole tree */
			damage_add(old);
			damage_tree(s);
		} else if (content) {
			struct rect d = { nx + dmg.x1, ny + dmg.y1, nx + dmg.x2, ny + dmg.y2 };
			damage_add(d);
		}
	}
	schedule_repaint();
}

static void surf_set_transform(struct wl_client *c, struct wl_resource *r, int32_t t) { }
static void surf_set_scale(struct wl_client *c, struct wl_resource *r, int32_t s) { }

static void surf_damage_buffer(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y,
			       int32_t w, int32_t h)
{
	add_damage(wl_resource_get_user_data(r), x, y, w, h);
}

static const struct wl_surface_interface surface_impl = {
	.destroy = surf_destroy_req,
	.attach = surf_attach,
	.damage = surf_damage,
	.frame = surf_frame,
	.set_opaque_region = surf_set_opaque,
	.set_input_region = surf_set_input,
	.commit = surf_commit,
	.set_buffer_transform = surf_set_transform,
	.set_buffer_scale = surf_set_scale,
	.damage_buffer = surf_damage_buffer,
};

static void free_cb_list(struct wl_list *l)
{
	struct frame_cb *cb, *tmp;
	wl_list_for_each_safe(cb, tmp, l, link) {
		wl_list_remove(&cb->link);
		wl_list_init(&cb->link);
	}
}

void window_destroy_for_surface(struct surface *s);
void popup_destroy_for_surface(struct surface *s);

static void surface_destroy(struct wl_resource *res)
{
	struct surface *s = wl_resource_get_user_data(res);
	if (s->win || s->popup)
		window_destroy_for_surface(s);
	xwm_surface_gone(s);
	if (s->sub) {
		wl_list_remove(&s->sub->link);
		s->sub->surf = NULL;
	}
	struct subsurf *sub, *tmp;
	wl_list_for_each_safe(sub, tmp, &s->subs, link) {
		wl_list_remove(&sub->link);
		wl_list_init(&sub->link);
		sub->parent = NULL;
	}
	if (s->px)
		damage_tree(s);
	struct popup *pp;
	wl_list_for_each(pp, &C.popups, link)
		if (pp->parent == s) {
			pp->parent = NULL;
			pp->dismissed = true;
		}
	if (C.panel == s) {
		C.panel = NULL;
		C.panel_h = 0;
		work_area_changed();
		damage_all();
	}
	if (C.menu == s)
		C.menu = NULL;
	if (C.flyout == s)
		C.flyout = NULL;
	if (C.notify == s)
		C.notify = NULL;
	if (C.desktop == s) {
		damage_tree(s);
		C.desktop = NULL;
	}
	lock_surface_gone(s);
	seat_surface_gone(s);
	if (s->xs)
		s->xs->surf = NULL;
	free_cb_list(&s->frames);
	free_cb_list(&s->p_frames);
	wl_list_remove(&s->buf_destroy.link);
	wl_list_remove(&s->link);
	free(s->px);
	free(s->p_input.v);
	free(s->input.v);
	free(s);
	schedule_repaint();
	update_kb_focus();
}

/* ---- wl_compositor ---------------------------------------------------- */

static void comp_create_surface(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	struct surface *s = calloc(1, sizeof(*s));
	if (!s) {
		wl_client_post_no_memory(c);
		return;
	}
	s->res = wl_resource_create(c, &wl_surface_interface, wl_resource_get_version(r), id);
	if (!s->res) {
		free(s);
		wl_client_post_no_memory(c);
		return;
	}
	s->input_def = true;
	wl_list_init(&s->p_frames);
	wl_list_init(&s->frames);
	wl_list_init(&s->subs);
	wl_list_init(&s->buf_destroy.link);
	s->buf_destroy.notify = buf_destroyed;
	wl_list_insert(&C.surfaces, &s->link);
	wl_resource_set_implementation(s->res, &surface_impl, s, surface_destroy);
}

static void comp_create_region(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	struct regionops *o = calloc(1, sizeof(*o));
	struct wl_resource *res = o ? wl_resource_create(c, &wl_region_interface, 1, id) : NULL;
	if (!res) {
		free(o);
		wl_client_post_no_memory(c);
		return;
	}
	wl_resource_set_implementation(res, &region_impl, o, region_free);
}

static const struct wl_compositor_interface compositor_impl = {
	.create_surface = comp_create_surface, .create_region = comp_create_region,
};

static void comp_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface, version, id);
	if (!r) {
		wl_client_post_no_memory(c);
		return;
	}
	wl_resource_set_implementation(r, &compositor_impl, NULL, NULL);
}

/* ---- wl_subcompositor ------------------------------------------------- */

static void sub_destroy_req(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static void sub_set_position(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y)
{
	struct subsurf *ss = wl_resource_get_user_data(r);
	if (!ss || !ss->surf)
		return;
	if (ss->parent && ss->parent->px)
		damage_tree(ss->parent);
	ss->x = x;
	ss->y = y;
	if (ss->parent && ss->parent->px)
		damage_tree(ss->parent);
}

static void sub_reorder(struct subsurf *ss, struct wl_resource *sibling_res, bool above)
{
	if (!ss || !ss->parent)
		return;
	struct surface *sib = wl_resource_get_user_data(sibling_res);
	struct wl_list *pos;
	wl_list_remove(&ss->link);
	if (sib == ss->parent)
		pos = above ? &ss->parent->subs : ss->parent->subs.prev;
	else if (sib->sub && sib->sub->parent == ss->parent)
		pos = above ? &sib->sub->link : sib->sub->link.prev;
	else
		pos = ss->parent->subs.prev;
	if (sib == ss->parent && !above)
		pos = &ss->parent->subs;
	wl_list_insert(pos, &ss->link);
	damage_tree(ss->parent);
}

static void sub_place_above(struct wl_client *c, struct wl_resource *r, struct wl_resource *sib)
{
	sub_reorder(wl_resource_get_user_data(r), sib, true);
}

static void sub_place_below(struct wl_client *c, struct wl_resource *r, struct wl_resource *sib)
{
	sub_reorder(wl_resource_get_user_data(r), sib, false);
}

/* Subsurfaces always behave as desynchronized: a commit shows at once. */
static void sub_set_sync(struct wl_client *c, struct wl_resource *r) { }
static void sub_set_desync(struct wl_client *c, struct wl_resource *r) { }

static const struct wl_subsurface_interface subsurface_impl = {
	.destroy = sub_destroy_req, .set_position = sub_set_position,
	.place_above = sub_place_above, .place_below = sub_place_below,
	.set_sync = sub_set_sync, .set_desync = sub_set_desync,
};

static void subsurf_destroy(struct wl_resource *res)
{
	struct subsurf *ss = wl_resource_get_user_data(res);
	if (ss->surf) {
		if (ss->parent && ss->parent->px)
			damage_tree(ss->parent);
		wl_list_remove(&ss->link);
		ss->surf->sub = NULL;
		ss->surf->role = ROLE_NONE;
		surface_unmap(ss->surf);
		if (ss->parent && ss->parent->px)
			damage_tree(ss->parent);
	}
	free(ss);
}

static void subc_destroy(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static void subc_get_subsurface(struct wl_client *c, struct wl_resource *r, uint32_t id,
				struct wl_resource *surf_res, struct wl_resource *parent_res)
{
	struct surface *s = wl_resource_get_user_data(surf_res);
	struct surface *p = wl_resource_get_user_data(parent_res);
	if (s->role != ROLE_NONE || s == p) {
		wl_resource_post_error(r, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
				       "surface already has a role");
		return;
	}
	struct subsurf *ss = calloc(1, sizeof(*ss));
	if (!ss) {
		wl_client_post_no_memory(c);
		return;
	}
	ss->res = wl_resource_create(c, &wl_subsurface_interface, wl_resource_get_version(r), id);
	if (!ss->res) {
		free(ss);
		wl_client_post_no_memory(c);
		return;
	}
	ss->surf = s;
	ss->parent = p;
	s->role = ROLE_SUB;
	s->sub = ss;
	wl_list_insert(p->subs.prev, &ss->link);
	wl_resource_set_implementation(ss->res, &subsurface_impl, ss, subsurf_destroy);
}

static const struct wl_subcompositor_interface subcompositor_impl = {
	.destroy = subc_destroy, .get_subsurface = subc_get_subsurface,
};

static void subc_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &wl_subcompositor_interface, version, id);
	if (!r) {
		wl_client_post_no_memory(c);
		return;
	}
	wl_resource_set_implementation(r, &subcompositor_impl, NULL, NULL);
}

void surface_init(void)
{
	wl_global_create(C.dpy, &wl_compositor_interface, 4, NULL, comp_bind);
	wl_global_create(C.dpy, &wl_subcompositor_interface, 1, NULL, subc_bind);
	wl_display_init_shm(C.dpy);
}
