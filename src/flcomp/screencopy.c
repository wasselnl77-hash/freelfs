/* fl_screencopy: the desktop, a part of it or one window, copied into a client's wl_shm buffer.
 * For Snappel. The compositor composites the desktop once more (render.c composite_clean_for: no
 * pointer, no Alt-Tab row, no indicator, and none of the asking program's own windows), copies
 * the rectangle out of C.shadow and puts everything back, like Print always did (shot.c).
 * A window is drawn alone into the buffer, from its own pixels, so what covers it does not matter.
 * Nothing works while the screen is locked.
 * SPDX-License-Identifier: MIT */
#include "comp.h"
#include "fl-screencopy-protocol.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { K_OUTPUT, K_REGION, K_WINDOW };

struct frame {
	struct wl_resource *res;
	int kind;
	struct rect r;		/* desktop rectangle (output, region) */
	uint32_t win_id;
	int w, h;		/* what buffer announced */
	int pid;		/* the asking process */
	bool used, dead;
};

static int client_pid(struct wl_client *c)
{
	pid_t pid = 0;
	wl_client_get_credentials(c, &pid, NULL, NULL);
	return (int)pid;
}

/* ---- drawing a window alone ------------------------------------------------ */

static uint32_t over(uint32_t s, uint32_t d)	/* premultiplied s over d */
{
	uint32_t a = s >> 24;
	if (a == 255)
		return s;
	if (a == 0)
		return d;
	uint32_t ia = 255 - a, out = 0;
	for (int sh = 0; sh <= 24; sh += 8) {
		uint32_t v = ((s >> sh) & 255) + (((d >> sh) & 255) * ia + 127) / 255;
		out |= (v > 255 ? 255 : v) << sh;
	}
	return out;
}

/* surface s (and its subsurfaces) into dst, whose top left is the desktop point (gx, gy) */
static void put_tree(struct surface *s, uint32_t *dst, int stride, int dw, int dh, int gx, int gy)
{
	if (!s->px)
		return;
	int ox, oy;
	surface_origin(s, &ox, &oy);
	for (int y = 0; y < s->h; y++) {
		int py = oy + y - gy;
		if (py < 0 || py >= dh)
			continue;
		for (int x = 0; x < s->w; x++) {
			int px = ox + x - gx;
			if (px < 0 || px >= dw)
				continue;
			uint32_t v = s->px[(size_t)y * s->w + x];
			if (!s->alpha)
				v |= 0xff000000u;
			dst[(size_t)py * stride + px] = over(v, dst[(size_t)py * stride + px]);
		}
	}
	struct subsurf *sub;
	wl_list_for_each(sub, &s->subs, link)
		put_tree(sub->surf, dst, stride, dw, dh, gx, gy);
}

static void render_window(struct window *w, uint32_t *dst, int stride, int dw, int dh)
{
	struct rect g = window_rect(w);
	for (int y = 0; y < dh; y++)
		memset(dst + (size_t)y * stride, 0, (size_t)dw * 4);
	put_tree(w->surf, dst, stride, dw, dh, g.x1, g.y1);
	struct popup *p;
	wl_list_for_each(p, &C.popups, link)
		if (popup_visible(p) && p->surf && window_of_surface(p->surf) == w)
			put_tree(p->surf, dst, stride, dw, dh, g.x1, g.y1);
}

/* ---- frame ---------------------------------------------------------------- */

static void frame_destroy_req(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static void frame_fail(struct frame *f)
{
	f->dead = true;
	fl_screencopy_frame_send_failed(f->res);
}

static void frame_copy(struct wl_client *c, struct wl_resource *r, struct wl_resource *bres)
{
	struct frame *f = wl_resource_get_user_data(r);
	if (f->used) {
		wl_resource_post_error(r, FL_SCREENCOPY_FRAME_ERROR_ALREADY_USED, "copy was sent already");
		return;
	}
	f->used = true;
	if (f->dead)
		return;
	struct wl_shm_buffer *sb = wl_shm_buffer_get(bres);
	uint32_t fmt = sb ? wl_shm_buffer_get_format(sb) : 0xffffffffu;
	if (!sb || (fmt != 0 && fmt != 1) || wl_shm_buffer_get_width(sb) != f->w ||
	    wl_shm_buffer_get_height(sb) != f->h || wl_shm_buffer_get_stride(sb) < f->w * 4) {
		wl_resource_post_error(r, FL_SCREENCOPY_FRAME_ERROR_INVALID_BUFFER, "the buffer does not fit");
		return;
	}
	if (C.locked || !C.shadow) {
		frame_fail(f);
		return;
	}
	int stride = wl_shm_buffer_get_stride(sb) / 4;
	bool argb = fmt == 0;
	if (f->kind == K_WINDOW) {
		struct window *w = window_by_id(f->win_id);
		struct rect g = w && w->mapped ? window_rect(w) : (struct rect){ 0, 0, 0, 0 };
		if (rect_empty(g) || rect_w(g) != f->w || rect_h(g) != f->h) {
			frame_fail(f);
			return;
		}
		wl_shm_buffer_begin_access(sb);
		uint32_t *dst = wl_shm_buffer_get_data(sb);
		render_window(w, dst, stride, f->w, f->h);
		if (!argb)	/* XRGB: the transparent part is black */
			for (int y = 0; y < f->h; y++)
				for (int x = 0; x < f->w; x++) {
					uint32_t *p = &dst[(size_t)y * stride + x];
					*p |= 0xff000000u;
				}
		wl_shm_buffer_end_access(sb);
	} else {
		composite_clean_for(f->pid);
		wl_shm_buffer_begin_access(sb);
		uint32_t *dst = wl_shm_buffer_get_data(sb);
		for (int y = 0; y < f->h; y++) {
			const uint32_t *src = C.shadow + (size_t)(f->r.y1 + y) * C.w + f->r.x1;
			uint32_t *d = dst + (size_t)y * stride;
			for (int x = 0; x < f->w; x++)
				d[x] = src[x] | 0xff000000u;
		}
		wl_shm_buffer_end_access(sb);
		composite_restore();
	}
	fl_screencopy_frame_send_ready(r);
}

static const struct fl_screencopy_frame_interface frame_impl = {
	.copy = frame_copy, .destroy = frame_destroy_req,
};

static void frame_free(struct wl_resource *r)
{
	free(wl_resource_get_user_data(r));
}

/* A new frame: announce the buffer, or fail at once (the client waits for one of the two). */
static void frame_new(struct wl_client *c, struct wl_resource *mgr, uint32_t id, int kind, struct rect r,
		      uint32_t win_id)
{
	struct frame *f = calloc(1, sizeof(*f));
	struct wl_resource *res = f ? wl_resource_create(c, &fl_screencopy_frame_interface, 1, id) : NULL;
	if (!res) {
		free(f);
		wl_client_post_no_memory(c);
		return;
	}
	f->res = res;
	f->kind = kind;
	f->pid = client_pid(c);
	f->win_id = win_id;
	wl_resource_set_implementation(res, &frame_impl, f, frame_free);
	bool ok = !C.locked && C.shadow;
	uint32_t format = 1;	/* xrgb8888 */
	if (ok && kind == K_WINDOW) {
		struct window *w = window_by_id(win_id);
		struct rect g = w && w->mapped ? window_rect(w) : (struct rect){ 0, 0, 0, 0 };
		ok = !rect_empty(g);
		r = (struct rect){ 0, 0, rect_w(g), rect_h(g) };
		format = 0;	/* argb8888: transparent where the window is */
	} else if (ok && kind == K_REGION) {
		struct rect scr = { 0, 0, C.w, C.h };
		ok = rect_intersect(r, scr, &r);
	} else if (ok) {
		r = (struct rect){ 0, 0, C.w, C.h };
	}
	if (!ok) {
		f->dead = true;
		fl_screencopy_frame_send_failed(res);
		return;
	}
	f->r = r;
	f->w = rect_w(r);
	f->h = rect_h(r);
	fl_screencopy_frame_send_buffer(res, format, (uint32_t)f->w, (uint32_t)f->h, (uint32_t)f->w * 4);
}

/* ---- manager -------------------------------------------------------------------- */

static void mgr_destroy(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static void mgr_output(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	frame_new(c, r, id, K_OUTPUT, (struct rect){ 0, 0, 0, 0 }, 0);
}

static void mgr_region(struct wl_client *c, struct wl_resource *r, uint32_t id, int32_t x, int32_t y,
		       int32_t w, int32_t h)
{
	if (w < 1 || h < 1 || w > 32768 || h > 32768) {
		frame_new(c, r, id, K_REGION, (struct rect){ 0, 0, 0, 0 }, 0);
		return;
	}
	frame_new(c, r, id, K_REGION, (struct rect){ x, y, x + w, y + h }, 0);
}

static void mgr_window(struct wl_client *c, struct wl_resource *r, uint32_t id, uint32_t win)
{
	frame_new(c, r, id, K_WINDOW, (struct rect){ 0, 0, 0, 0 }, win);
}

static void mgr_list(struct wl_client *c, struct wl_resource *r)
{
	int me = client_pid(c);
	if (!C.locked) {
		struct window *w;
		wl_list_for_each(w, &C.windows, link) {	/* topmost first */
			if (!window_visible(w) || w->parent || !w->announced)
				continue;
			pid_t pid = 0;
			wl_client_get_credentials(wl_resource_get_client(w->surf->res), &pid, NULL, NULL);
			if ((int)pid == me)
				continue;
			struct rect g = window_rect(w);
			fl_screencopy_manager_send_window(r, w->id, g.x1, g.y1, (uint32_t)rect_w(g), (uint32_t)rect_h(g),
							  w->app_id ? w->app_id : "", w->title ? w->title : "");
		}
	}
	fl_screencopy_manager_send_windows_done(r);
}

static const struct fl_screencopy_manager_interface mgr_impl = {
	.destroy = mgr_destroy,
	.capture_output = mgr_output,
	.capture_region = mgr_region,
	.capture_window = mgr_window,
	.list_windows = mgr_list,
};

static void mgr_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &fl_screencopy_manager_interface, 1, id);
	if (!r) {
		wl_client_post_no_memory(c);
		return;
	}
	wl_resource_set_implementation(r, &mgr_impl, NULL, NULL);
}

void screencopy_init(void)
{
	wl_global_create(C.dpy, &fl_screencopy_manager_interface, 1, NULL, mgr_bind);
}
