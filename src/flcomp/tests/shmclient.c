/* shmclient -- a tiny Wayland test client for flcomp.
 * Draws one solid colour window (or the panel) and prints what it receives.
 *   shmclient [-w W] [-h H] [-c RRGGBB] [-a ALPHA] [-t TITLE] [-p PANELH]
 *             [-l] [-M] [-X] [-s SECONDS]
 * -p: be the panel (needs fl_shell); -l: only listen to fl_shell, no window;
 * -M: start xdg_toplevel.move on the first button press; -X maximize; -F fullscreen;
 * -a: ARGB with that alpha (0-255), premultiplied.
 * -L: be the lock screen (fl_shell lock, version 4), full output; "unlock:1" in -C unlocks;
 * -N: be the notification cards (set_notifications): left half opaque, right half transparent.
 * SPDX-License-Identifier: MIT */
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include <drm_fourcc.h>

#include "fl-screencopy-client-protocol.h"
#include "fl-shell-client-protocol.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

struct wl_display *g_dpy;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct fl_shell *shell;
static struct wl_seat *seat;
static struct wl_surface *surf;
static struct xdg_toplevel *top;
static struct xdg_surface *top_xs;
static int out_w, out_h;
static int lock_mode, notify_mode, want_unlock, unlock_at, ws_mode;
static int W = 200, H = 100, panel_h, listen_only, do_move, do_resize, maximize, menu_mode, fullscreen_req;
static const char *cmds, *clip_text;
static int clip_query;
static unsigned *cap_buf_info;
static struct wl_data_device_manager *ddm;
static struct wl_data_device *ddev;
static struct wl_data_source *dsrc;
static unsigned color = 0x3f9b63;
static int alpha = -1;
static const char *title = "shmclient";
static int mapped;
static struct zwp_linux_dmabuf_v1 *dmabuf;
static int use_dmabuf, dmabuf_swap;	/* -D: buffers via zwp_linux_dmabuf_v1 (a memfd stands in); -B: as ABGR8888 */

static void dmb_format(void *d, struct zwp_linux_dmabuf_v1 *x, uint32_t f) { }
static void dmb_modifier(void *d, struct zwp_linux_dmabuf_v1 *x, uint32_t f, uint32_t hi, uint32_t lo)
{
	printf("dmabuf_modifier %08x %x:%x\n", f, hi, lo);
}
static const struct zwp_linux_dmabuf_v1_listener dmb_l = { dmb_format, dmb_modifier };

static void draw_to(struct wl_surface *sf, int w, int h, unsigned col, int alp)
{
	int stride = w * 4;
	size_t size = (size_t)stride * h;
	int fd = memfd_create("shmclient", 0);
	if (fd < 0 || ftruncate(fd, size))
		exit(2);
	uint32_t *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	uint32_t px = col;
	if (alp >= 0) {
		unsigned r = ((col >> 16) & 255) * alp / 255, g = ((col >> 8) & 255) * alp / 255,
			 b = (col & 255) * alp / 255;
		px = ((unsigned)alp << 24) | r << 16 | g << 8 | b;
	}
	for (size_t i = 0; i < (size_t)w * h; i++)
		p[i] = px;
	struct wl_buffer *b;
	if (use_dmabuf && dmabuf) {
		if (dmabuf_swap)
			for (size_t i = 0; i < (size_t)w * h; i++)
				p[i] = (p[i] & 0xff00ff00u) | (p[i] & 0xff) << 16 | (p[i] >> 16 & 0xff);
		struct zwp_linux_buffer_params_v1 *pa = zwp_linux_dmabuf_v1_create_params(dmabuf);
		zwp_linux_buffer_params_v1_add(pa, fd, 0, 0, stride, 0, 0);
		b = zwp_linux_buffer_params_v1_create_immed(pa, w, h,
			dmabuf_swap ? (alp >= 0 ? DRM_FORMAT_ABGR8888 : DRM_FORMAT_XBGR8888)
				    : (alp >= 0 ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888), 0);
		zwp_linux_buffer_params_v1_destroy(pa);
	} else {
		struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
		b = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
			alp >= 0 ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888);
		wl_shm_pool_destroy(pool);
	}
	close(fd);
	munmap(p, size);
	wl_surface_attach(sf, b, 0, 0);
	wl_surface_damage(sf, 0, 0, w, h);
	wl_surface_commit(sf);
}

static void draw(int w, int h)
{
	draw_to(surf, w, h, color, alpha);
	mapped = 1;
	printf("mapped %dx%d\n", w, h);
	fflush(stdout);
}

/* left half opaque colour, right half fully transparent (the cards' input test) */
static void draw_half(int w, int h)
{
	int stride = w * 4;
	size_t size = (size_t)stride * h;
	int fd = memfd_create("shmclient", 0);
	if (fd < 0 || ftruncate(fd, size))
		exit(2);
	uint32_t *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			p[y * w + x] = x < w / 2 ? 0xff000000u | color : 0;
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	munmap(p, size);
	wl_surface_attach(surf, b, 0, 0);
	wl_surface_damage(surf, 0, 0, w, h);
	wl_surface_commit(surf);
	mapped = 1;
	printf("mapped %dx%d\n", w, h);
	fflush(stdout);
}

/* ---- popup (-U): opened on the first button press ---- */
static int do_popup, popup_mapped;
static struct wl_surface *psurf;
static struct xdg_surface *pxs;
static struct xdg_popup *ppop;

static void pxs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
	xdg_surface_ack_configure(xs, serial);
	if (!popup_mapped) {
		popup_mapped = 1;
		draw_to(psurf, 50, 30, 0xff00ff, -1);
		printf("popup mapped\n");
		fflush(stdout);
	}
}
static const struct xdg_surface_listener pxs_l = { pxs_configure };
static void pp_configure(void *d, struct xdg_popup *p, int32_t x, int32_t y, int32_t w, int32_t h)
{ printf("popup_configure %d %d %d %d\n", x, y, w, h); fflush(stdout); }
static void pp_done(void *d, struct xdg_popup *p)
{
	printf("popup_done\n");
	fflush(stdout);
	xdg_popup_destroy(ppop);
	xdg_surface_destroy(pxs);
	wl_surface_destroy(psurf);
	ppop = NULL;
}
static void pp_repositioned(void *d, struct xdg_popup *p, uint32_t t) { }
static const struct xdg_popup_listener pp_l = { pp_configure, pp_done, pp_repositioned };

/* ---- xdg ---- */
static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t s) { xdg_wm_base_pong(b, s); }
static const struct xdg_wm_base_listener wm_l = { wm_ping };

static void xs_configure(void *d, struct xdg_surface *xs, uint32_t serial)
{
	xdg_surface_ack_configure(xs, serial);
	if (!mapped)
		draw(W, H);
}
static const struct xdg_surface_listener xs_l = { xs_configure };

static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st)
{
	uint32_t *s;
	printf("configure %dx%d states:", w, h);
	wl_array_for_each(s, st) printf(" %u", *s);
	printf("\n");
	fflush(stdout);
	if (w > 0 && h > 0) {
		W = w;
		H = h;
		if (mapped)
			draw(W, H);
	}
}
static void tl_close(void *d, struct xdg_toplevel *t) { printf("close\n"); exit(0); }
static const struct xdg_toplevel_listener tl_l = { tl_configure, tl_close };

/* ---- fl_shell ---- */
static void sh_added(void *d, struct fl_shell *s, uint32_t id, const char *app, const char *t)
{
	printf("window_added id=%u app_id=%s title=%s\n", id, app, t);
	fflush(stdout);
}
static void sh_changed(void *d, struct fl_shell *s, uint32_t id, const char *t, uint32_t f, uint32_t m)
{
	printf("window_changed id=%u title=%s focused=%u minimized=%u\n", id, t, f, m);
	fflush(stdout);
}
static void sh_removed(void *d, struct fl_shell *s, uint32_t id)
{
	printf("window_removed id=%u\n", id);
	fflush(stdout);
}
static void sh_dismissed(void *d, struct fl_shell *s)
{
	printf("menu_dismissed\n");
	fflush(stdout);
	if (menu_mode) {
		wl_surface_attach(surf, NULL, 0, 0);
		wl_surface_commit(surf);
	}
}
static void sh_toggle(void *d, struct fl_shell *s) { printf("menu_toggle\n"); fflush(stdout); }
static void sh_layout(void *d, struct fl_shell *s, const char *n)
{
	printf("keyboard_layout %s\n", n);
	fflush(stdout);
}
static void sh_fly_dismissed(void *d, struct fl_shell *s) { }
static void sh_volume_key(void *d, struct fl_shell *s, uint32_t a) { }
static void sh_output_mode(void *d, struct fl_shell *s, uint32_t w, uint32_t h, uint32_t hz, uint32_t f) { }
static void sh_output_state(void *d, struct fl_shell *s, uint32_t sc, uint32_t w, uint32_t h, uint32_t p) { }
static void sh_output_failed(void *d, struct fl_shell *s) { }
static void sh_lock_state(void *d, struct fl_shell *s, uint32_t locked)
{
	printf("lock_state %u\n", locked);
	fflush(stdout);
}
static void sh_workspaces(void *d, struct fl_shell *s, uint32_t cols, uint32_t rows, uint32_t cur)
{
	printf("workspaces %ux%u current=%u\n", cols, rows, cur);
	fflush(stdout);
}
static void sh_window_ws(void *d, struct fl_shell *s, uint32_t id, uint32_t idx)
{
	printf("window_workspace id=%u ws=%u\n", id, idx);
	fflush(stdout);
}
static const struct fl_shell_listener sh_l = { sh_added, sh_changed, sh_removed, sh_dismissed,
					       sh_toggle, sh_layout, sh_fly_dismissed, sh_volume_key,
					       sh_output_mode, sh_output_state, sh_output_failed, sh_lock_state,
					       sh_workspaces, sh_window_ws };

/* ---- fl_screencopy (-S) ---- */
static struct fl_screencopy_manager *scm;
static const char *cap_spec, *cap_file;	/* -S out | region:X,Y,W,H | window:ID | list ; -O file.ppm */
static struct fl_screencopy_frame *cap_frame;

static void fr_buffer(void *d, struct fl_screencopy_frame *f, uint32_t fmt, uint32_t w, uint32_t h, uint32_t stride)
{
	printf("capture_buffer fmt=%u %ux%u\n", fmt, w, h);
	fflush(stdout);
	size_t size = (size_t)stride * h;
	int fd = memfd_create("shmclient-cap", 0);
	if (fd < 0 || ftruncate(fd, size))
		exit(2);
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, w, h, stride, fmt);
	wl_shm_pool_destroy(pool);
	fl_screencopy_frame_copy(f, b);
	wl_display_flush(g_dpy);
	/* keep the memfd: ready finds it through the user data */
	static int fds[1];
	fds[0] = fd;
	wl_proxy_set_user_data((struct wl_proxy *)f, fds);
	static unsigned wh[3];
	wh[0] = w; wh[1] = h; wh[2] = stride;
	cap_buf_info = wh;
}
static void fr_ready(void *d, struct fl_screencopy_frame *f)
{
	int *fds = wl_proxy_get_user_data((struct wl_proxy *)f);
	unsigned w = cap_buf_info[0], h = cap_buf_info[1], stride = cap_buf_info[2];
	uint32_t *p = mmap(NULL, (size_t)stride * h, PROT_READ, MAP_SHARED, fds[0], 0);
	printf("capture_ready %ux%u first=%08x\n", w, h, p ? p[0] : 0);
	fflush(stdout);
	FILE *o = cap_file && p != MAP_FAILED ? fopen(cap_file, "wb") : NULL;
	if (o) {
		fprintf(o, "P6\n%u %u\n255\n", w, h);
		for (unsigned y = 0; y < h; y++)
			for (unsigned x = 0; x < w; x++) {
				uint32_t v = p[(size_t)y * stride / 4 + x];
				fputc(v >> 16 & 255, o);
				fputc(v >> 8 & 255, o);
				fputc(v & 255, o);
			}
		fclose(o);
	}
}
static void fr_failed(void *d, struct fl_screencopy_frame *f) { printf("capture_failed\n"); fflush(stdout); }
static const struct fl_screencopy_frame_listener fr_l = { fr_buffer, fr_ready, fr_failed };
static void scm_window(void *d, struct fl_screencopy_manager *m, uint32_t id, int32_t x, int32_t y, uint32_t w,
		       uint32_t h, const char *app, const char *title)
{
	printf("sc_window id=%u %d,%d %ux%u app=%s title=%s\n", id, x, y, w, h, app, title);
	fflush(stdout);
}
static void scm_done(void *d, struct fl_screencopy_manager *m) { printf("sc_windows_done\n"); fflush(stdout); }
static const struct fl_screencopy_manager_listener scm_l = { scm_window, scm_done };

static void do_capture(void)
{
	if (!scm || !cap_spec)
		return;
	if (!strcmp(cap_spec, "list")) {
		fl_screencopy_manager_list_windows(scm);
		return;
	}
	if (!strcmp(cap_spec, "out"))
		cap_frame = fl_screencopy_manager_capture_output(scm);
	else if (!strncmp(cap_spec, "window:", 7))
		cap_frame = fl_screencopy_manager_capture_window(scm, (uint32_t)atoi(cap_spec + 7));
	else {
		int x = 0, y = 0, w = 0, h = 0;
		sscanf(cap_spec, "region:%d,%d,%d,%d", &x, &y, &w, &h);
		cap_frame = fl_screencopy_manager_capture_region(scm, x, y, w, h);
	}
	fl_screencopy_frame_add_listener(cap_frame, &fr_l, NULL);
}

/* ---- seat ---- */
static void p_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y)
{ printf("pointer_enter %d %d\n", wl_fixed_to_int(x), wl_fixed_to_int(y)); fflush(stdout); }
static void p_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf)
{ printf("pointer_leave\n"); fflush(stdout); }
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{ printf("pointer_motion %d %d\n", wl_fixed_to_int(x), wl_fixed_to_int(y)); fflush(stdout); }
static void p_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b, uint32_t st)
{
	printf("pointer_button %u %u\n", b, st);
	fflush(stdout);
	if (do_move && st == 1 && top)
		xdg_toplevel_move(top, seat, s);
	if (do_popup && st == 1 && top && !ppop) {
		struct xdg_positioner *pos = xdg_wm_base_create_positioner(wm);
		xdg_positioner_set_size(pos, 50, 30);
		xdg_positioner_set_anchor_rect(pos, 20, 20, 10, 10);
		xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT);
		xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
		psurf = wl_compositor_create_surface(comp);
		pxs = xdg_wm_base_get_xdg_surface(wm, psurf);
		xdg_surface_add_listener(pxs, &pxs_l, NULL);
		struct xdg_surface *parent = top_xs;
		ppop = xdg_surface_get_popup(pxs, parent, pos);
		xdg_popup_add_listener(ppop, &pp_l, NULL);
		xdg_popup_grab(ppop, seat, s);
		xdg_positioner_destroy(pos);
		wl_surface_commit(psurf);
		popup_mapped = 0;
	}
	if (do_resize && st == 1 && top)
		xdg_toplevel_resize(top, seat, s, XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT);
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) { }
static void p_frame(void *d, struct wl_pointer *p) { }
static void p_axis_source(void *d, struct wl_pointer *p, uint32_t s) { }
static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { }
static void p_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { }
static const struct wl_pointer_listener p_l = { p_enter, p_leave, p_motion, p_button, p_axis, p_frame,
	p_axis_source, p_axis_stop, p_axis_discrete };

static void k_keymap(void *d, struct wl_keyboard *k, uint32_t f, int32_t fd, uint32_t sz)
{ printf("keymap format=%u size=%u\n", f, sz); fflush(stdout); close(fd); }
static void ds_target(void *d, struct wl_data_source *s, const char *m) { }
static void ds_send(void *d, struct wl_data_source *s, const char *mime, int32_t fd)
{
	if (write(fd, clip_text, strlen(clip_text)) < 0)
		perror("write");
	close(fd);
}
static void ds_cancelled(void *d, struct wl_data_source *s) { printf("clip_cancelled\n"); fflush(stdout); }
static const struct wl_data_source_listener ds_l = { ds_target, ds_send, ds_cancelled };

static void k_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, struct wl_array *keys)
{
	printf("kb_enter\n");
	fflush(stdout);
	if (clip_text && ddm && ddev && !dsrc) {
		dsrc = wl_data_device_manager_create_data_source(ddm);
		wl_data_source_add_listener(dsrc, &ds_l, NULL);
		wl_data_source_offer(dsrc, "text/plain");
		wl_data_device_set_selection(ddev, dsrc, s);
	}
}
static void k_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf)
{ printf("kb_leave\n"); fflush(stdout); }
static void k_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t key, uint32_t st)
{ printf("key %u %u\n", key, st); fflush(stdout); }
static void k_mods(void *d, struct wl_keyboard *k, uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t g) { }
static void k_repeat(void *d, struct wl_keyboard *k, int32_t r, int32_t dl) { }
static const struct wl_keyboard_listener k_l = { k_keymap, k_enter, k_leave, k_key, k_mods, k_repeat };

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
	if (caps & WL_SEAT_CAPABILITY_POINTER)
		wl_pointer_add_listener(wl_seat_get_pointer(s), &p_l, NULL);
	if (caps & WL_SEAT_CAPABILITY_KEYBOARD)
		wl_keyboard_add_listener(wl_seat_get_keyboard(s), &k_l, NULL);
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { }
static const struct wl_seat_listener seat_l = { seat_caps, seat_name };

static struct wl_data_offer *cur_offer;
static void do_offer(void *d, struct wl_data_offer *o, const char *m) { }
static const struct wl_data_offer_listener offer_l = { do_offer };
static void dd_data_offer(void *d, struct wl_data_device *dd, struct wl_data_offer *o)
{
	wl_data_offer_add_listener(o, &offer_l, NULL);
}
static void dd_enter(void *d, struct wl_data_device *dd, uint32_t s, struct wl_surface *sf,
		     wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *o) { }
static void dd_leave(void *d, struct wl_data_device *dd) { }
static void dd_motion(void *d, struct wl_data_device *dd, uint32_t t, wl_fixed_t x, wl_fixed_t y) { }
static void dd_drop(void *d, struct wl_data_device *dd) { }
static void dd_selection(void *d, struct wl_data_device *dd, struct wl_data_offer *o)
{
	cur_offer = o;
	if (!o) {
		printf("clip: none\n");
		fflush(stdout);
		return;
	}
	if (!clip_query)
		return;
	int fds[2];
	if (pipe(fds))
		return;
	const char *mime = getenv("SHM_CLIP_MIME");	/* default text/plain; image/png prints the size and magic */
	wl_data_offer_receive(o, mime ? mime : "text/plain", fds[1]);
	close(fds[1]);
	extern struct wl_display *g_dpy;
	wl_display_roundtrip(g_dpy);
	static unsigned char big[1 << 21];
	size_t got = 0;
	ssize_t n;
	while (got < sizeof big - 1 && (n = read(fds[0], big + got, sizeof big - 1 - got)) > 0)
		got += (size_t)n;
	close(fds[0]);
	if (mime && strcmp(mime, "text/plain")) {
		printf("clip: %zu bytes magic=%02x%02x%02x%02x\n", got, big[0], big[1], big[2], big[3]);
	} else {
		big[got] = 0;
		printf("clip: %.255s\n", big);
	}
	fflush(stdout);
}
static const struct wl_data_device_listener dd_l = { dd_data_offer, dd_enter, dd_leave, dd_motion,
						     dd_drop, dd_selection };

/* ---- output / registry ---- */
static void o_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
		       int32_t sp, const char *mk, const char *md, int32_t t) { }
static void o_mode(void *d, struct wl_output *o, uint32_t fl, int32_t w, int32_t h, int32_t r)
{ out_w = w; out_h = h; }
static void o_done(void *d, struct wl_output *o) { }
static void o_scale(void *d, struct wl_output *o, int32_t f) { }
static const struct wl_output_listener o_l = { o_geometry, o_mode, o_done, o_scale };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
	if (!strcmp(iface, "wl_compositor"))
		comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, "wl_shm"))
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, "xdg_wm_base"))
		wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 2);
	else if (!strcmp(iface, "fl_shell"))
	{
		shell = wl_registry_bind(r, name, &fl_shell_interface, ws_mode ? (v < 5 ? v : 5) :
					 lock_mode || notify_mode || want_unlock ? (v < 4 ? v : 4) : 1);
		fl_shell_add_listener(shell, &sh_l, NULL);
	}
	else if (!strcmp(iface, "fl_screencopy_manager"))
	{
		scm = wl_registry_bind(r, name, &fl_screencopy_manager_interface, 1);
		fl_screencopy_manager_add_listener(scm, &scm_l, NULL);
	}
	else if (!strcmp(iface, "wl_seat"))
	{
		seat = wl_registry_bind(r, name, &wl_seat_interface, 5);
		wl_seat_add_listener(seat, &seat_l, NULL);
	}
	else if (!strcmp(iface, "wl_data_device_manager"))
		ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, 3);
	else if (!strcmp(iface, "zwp_linux_dmabuf_v1")) {
		dmabuf = wl_registry_bind(r, name, &zwp_linux_dmabuf_v1_interface, v < 3 ? v : 3);
		zwp_linux_dmabuf_v1_add_listener(dmabuf, &dmb_l, NULL);
	}
	else if (!strcmp(iface, "wl_output"))
		wl_output_add_listener(wl_registry_bind(r, name, &wl_output_interface, 3), &o_l, NULL);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

int main(int argc, char **argv)
{
	int secs = 3, opt;
	while ((opt = getopt(argc, argv, "w:h:c:a:t:p:lMZXFmUs:C:P:QDBLNWS:O:")) != -1) {
		switch (opt) {
		case 'w': W = atoi(optarg); break;
		case 'h': H = atoi(optarg); break;
		case 'c': color = strtoul(optarg, NULL, 16); break;
		case 'a': alpha = atoi(optarg); break;
		case 't': title = optarg; break;
		case 'p': panel_h = atoi(optarg); break;
		case 'l': listen_only = 1; break;
		case 'M': do_move = 1; break;
		case 'X': maximize = 1; break;
		case 'F': fullscreen_req = 1; break;
		case 'Z': do_resize = 1; break;
		case 'm': menu_mode = 1; break;
		case 'U': do_popup = 1; break;
		case 'P': clip_text = optarg; break;
		case 'Q': clip_query = 1; break;
		case 'L': lock_mode = 1; break;
		case 'N': notify_mode = 1; break;
		case 'W': ws_mode = 1; break;		/* bind fl_shell version 5: print workspaces and window_workspace */
		case 'S': cap_spec = optarg; break;	/* capture once, 1 s in: out | region:X,Y,W,H | window:ID | list */
		case 'O': cap_file = optarg; break;	/* write the capture here as PPM */
		case 'D': use_dmabuf = 1; break;
		case 'B': use_dmabuf = dmabuf_swap = 1; break;
		case 'C': cmds = optarg; want_unlock = strstr(optarg, "unlock") != NULL; break;
		case 's': secs = atoi(optarg); break;
		default: return 2;
		}
	}
	struct wl_display *dpy = g_dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "shmclient: cannot connect\n");
		return 1;
	}
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_l, NULL);
	wl_display_roundtrip(dpy);
	wl_display_roundtrip(dpy);
	if (!comp || !shm || !wm) {
		fprintf(stderr, "shmclient: missing globals\n");
		return 1;
	}
	xdg_wm_base_add_listener(wm, &wm_l, NULL);
	if (ddm && seat && (clip_text || clip_query)) {
		ddev = wl_data_device_manager_get_data_device(ddm, seat);
		wl_data_device_add_listener(ddev, &dd_l, NULL);
	}
	printf("output %dx%d\n", out_w, out_h);
	fflush(stdout);

	if (listen_only) {
		/* nothing to draw */
	} else if (menu_mode) {
		if (!shell)
			return 1;
		surf = wl_compositor_create_surface(comp);
		fl_shell_set_menu(shell, surf);
		W = 150;
		H = 120;
		draw(W, H);
	} else if (lock_mode) {
		if (!shell)
			return 1;
		surf = wl_compositor_create_surface(comp);
		fl_shell_lock(shell, surf);
		draw(out_w, out_h);
	} else if (notify_mode) {
		if (!shell)
			return 1;
		surf = wl_compositor_create_surface(comp);
		fl_shell_set_notifications(shell, surf);
		draw_half(W, H);
	} else if (panel_h) {
		if (!shell)
			return 1;
		surf = wl_compositor_create_surface(comp);
		fl_shell_set_panel(shell, surf, panel_h);
		draw(out_w, panel_h);
	} else {
		surf = wl_compositor_create_surface(comp);
		struct xdg_surface *xs = top_xs = xdg_wm_base_get_xdg_surface(wm, surf);
		xdg_surface_add_listener(xs, &xs_l, NULL);
		top = xdg_surface_get_toplevel(xs);
		xdg_toplevel_add_listener(top, &tl_l, NULL);
		xdg_toplevel_set_title(top, title);
		xdg_toplevel_set_app_id(top, "shmclient");
		if (maximize)
			xdg_toplevel_set_maximized(top);
		if (fullscreen_req)
			xdg_toplevel_set_fullscreen(top, NULL);
		wl_surface_commit(surf);
	}
	wl_display_flush(dpy);

	struct pollfd pf = { .fd = wl_display_get_fd(dpy), .events = POLLIN };
	for (int ms = secs * 1000; ms > 0;) {
		while (wl_display_prepare_read(dpy) != 0)
			wl_display_dispatch_pending(dpy);
		wl_display_flush(dpy);
		int n = poll(&pf, 1, 100);
		if (n > 0)
			wl_display_read_events(dpy);
		else
			wl_display_cancel_read(dpy);
		wl_display_dispatch_pending(dpy);
		ms -= 100;
		if (unlock_at && secs * 1000 - ms >= unlock_at && shell) {
			fl_shell_unlock(shell);	/* "unlock:SECONDS" in -C */
			unlock_at = 0;
			wl_display_flush(dpy);
		}
		if (cap_spec && ms == secs * 1000 - 1000)
			do_capture();
		if (cmds && ms == secs * 1000 - 1000) {
			/* "activate:1 minimize:2 layout:de close:3" */
			char *c = strdup(cmds), *save, *tok;
			for (tok = strtok_r(c, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
				char *colon = strchr(tok, ':');
				if (!colon || !shell)
					continue;
				*colon++ = 0;
				if (!strcmp(tok, "activate"))
					fl_shell_activate(shell, atoi(colon));
				else if (!strcmp(tok, "minimize"))
					fl_shell_minimize(shell, atoi(colon));
				else if (!strcmp(tok, "close"))
					fl_shell_close(shell, atoi(colon));
				else if (!strcmp(tok, "layout"))
					fl_shell_set_keyboard_layout(shell, colon);
				else if (!strcmp(tok, "unlock"))
					unlock_at = atoi(colon) * 1000;
				else if (!strcmp(tok, "ws"))
					fl_shell_set_workspace(shell, atoi(colon));
				else if (!strcmp(tok, "wsmove")) {	/* wsmove:ID,N */
					int id = 0, n = 0;
					sscanf(colon, "%d,%d", &id, &n);
					fl_shell_move_window(shell, id, n);
				} else if (!strcmp(tok, "overview"))
					fl_shell_toggle_overview(shell);
			}
			free(c);
			wl_display_flush(dpy);
		}
	}
	return 0;
}
