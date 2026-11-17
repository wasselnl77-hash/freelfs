/* csdclient -- a Wayland test client that behaves like a GTK3/GTK4 program with client-side decorations (Firefox):
 * it draws its own shadow margin around the window, says where the window really is with
 * xdg_surface.set_window_geometry, drops the margin when maximized or tiled, and asks the compositor to maximize,
 * fullscreen, move and resize. Prints what it receives so a script can check it.
 *
 *   csdclient [-g WxH] [-m MARGIN] [-k] [-c RRGGBB] [-S SCRIPT]
 *
 * -g: window geometry (default 300x200); -m: shadow margin in px (default 24); -k: keep drawing the margin even
 * when maximized (a client that does not follow the state, the compositor must still place the geometry);
 * SCRIPT: steps separated by ';' run in order: sleep MS, max, unmax, full, unfull, min, move, resize EDGE
 * (top|bottom|left|right|tl|tr|bl|br). move and resize wait for the first pointer button press and use its serial.
 * Output: "configure WxH states: N N ..." per xdg_toplevel.configure, "commit geom WxH margin M buffer WxH" per
 * drawn frame, "pointer-button serial S", "request NAME".
 * SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"

static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct xdg_wm_base *wm;
static struct wl_seat *seat;
static struct wl_surface *surf;
static struct xdg_toplevel *top;
static struct xdg_surface *xs;
static int gw = 300, gh = 200, margin0 = 24, margin = 24, keep, mapped;
static unsigned color = 0x5577aa;
static uint32_t last_serial;
static int have_serial;
static char *script, *cur;
static double wake;
static int pending_move, pending_resize = -1;

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void draw(void)
{
	int bw = gw + 2 * margin, bh = gh + 2 * margin;
	size_t size = (size_t)bw * bh * 4;
	int fd = memfd_create("csd", 0);
	if (fd < 0 || ftruncate(fd, size))
		exit(2);
	uint32_t *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	for (int y = 0; y < bh; y++)
		for (int x = 0; x < bw; x++) {
			int in = x >= margin && x < margin + gw && y >= margin && y < margin + gh;
			p[y * bw + x] = in ? 0xff000000u | color : 0xff202020u; /* the "shadow": opaque here, so the test can see it */
		}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, bw, bh, bw * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	munmap(p, size);
	xdg_surface_set_window_geometry(xs, margin, margin, gw, gh);
	wl_surface_attach(surf, b, 0, 0);
	wl_surface_damage_buffer(surf, 0, 0, bw, bh);
	wl_surface_commit(surf);
	printf("commit geom %dx%d margin %d buffer %dx%d\n", gw, gh, margin, bw, bh);
	fflush(stdout);
	mapped = 1;
}

static void xs_configure(void *d, struct xdg_surface *x, uint32_t serial)
{
	xdg_surface_ack_configure(x, serial);
	draw();
}
static const struct xdg_surface_listener xs_l = { xs_configure };

static void tl_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st)
{
	uint32_t *s;
	int no_margin = 0;
	printf("configure %dx%d states:", w, h);
	wl_array_for_each(s, st) {
		printf(" %u", *s);
		if (*s == XDG_TOPLEVEL_STATE_MAXIMIZED || *s == XDG_TOPLEVEL_STATE_FULLSCREEN ||
		    *s == XDG_TOPLEVEL_STATE_TILED_LEFT || *s == XDG_TOPLEVEL_STATE_TILED_RIGHT)
			no_margin = 1;
	}
	printf("\n");
	fflush(stdout);
	if (w > 0 && h > 0) {
		gw = w;
		gh = h;
	}
	margin = no_margin && !keep ? 0 : margin0;
}
static void tl_close(void *d, struct xdg_toplevel *t) { exit(0); }
static const struct xdg_toplevel_listener tl_l = { tl_configure, tl_close };

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t s) { xdg_wm_base_pong(b, s); }
static const struct xdg_wm_base_listener wm_l = { wm_ping };

static void act(void)
{
	if (pending_move) {
		pending_move = 0;
		printf("request move\n");
		fflush(stdout);
		xdg_toplevel_move(top, seat, last_serial);
	}
	if (pending_resize >= 0) {
		printf("request resize %d\n", pending_resize);
		fflush(stdout);
		xdg_toplevel_resize(top, seat, last_serial, (uint32_t)pending_resize);
		pending_resize = -1;
	}
}

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *su, wl_fixed_t x, wl_fixed_t y) { }
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *su) { }
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) { }
static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t b, uint32_t state)
{
	if (state != WL_POINTER_BUTTON_STATE_PRESSED)
		return;
	last_serial = serial;
	have_serial = 1;
	printf("pointer-button serial %u\n", serial);
	fflush(stdout);
	act();
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) { }
static void ptr_frame(void *d, struct wl_pointer *p) { }
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s) { }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { }
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { }
static const struct wl_pointer_listener ptr_l = { ptr_enter, ptr_leave, ptr_motion, ptr_button, ptr_axis, ptr_frame,
						  ptr_axis_source, ptr_axis_stop, ptr_axis_discrete };

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
	if (caps & WL_SEAT_CAPABILITY_POINTER)
		wl_pointer_add_listener(wl_seat_get_pointer(s), &ptr_l, NULL);
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { }
static const struct wl_seat_listener seat_l = { seat_caps, seat_name };

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
	if (!strcmp(iface, "wl_compositor"))
		comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, "wl_shm"))
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, "xdg_wm_base")) {
		wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 2);
		xdg_wm_base_add_listener(wm, &wm_l, NULL);
	} else if (!strcmp(iface, "wl_seat")) {
		seat = wl_registry_bind(r, name, &wl_seat_interface, 5);
		wl_seat_add_listener(seat, &seat_l, NULL);
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { }
static const struct wl_registry_listener reg_l = { reg_global, reg_remove };

/* run the next script step(s) that are due */
static void steps(void)
{
	while (cur && *cur && now() >= wake) {
		char *e = strchr(cur, ';');
		if (e)
			*e = 0;
		char *s = cur;
		while (*s == ' ')
			s++;
		cur = e ? e + 1 : NULL;
		if (!strncmp(s, "sleep ", 6))
			wake = now() + atoi(s + 6) / 1000.0;
		else if (!strcmp(s, "max")) { printf("request max\n"); fflush(stdout); xdg_toplevel_set_maximized(top); }
		else if (!strcmp(s, "unmax")) { printf("request unmax\n"); fflush(stdout); xdg_toplevel_unset_maximized(top); }
		else if (!strcmp(s, "full")) { printf("request full\n"); fflush(stdout); xdg_toplevel_set_fullscreen(top, NULL); }
		else if (!strcmp(s, "unfull")) { printf("request unfull\n"); fflush(stdout); xdg_toplevel_unset_fullscreen(top); }
		else if (!strcmp(s, "min")) { printf("request min\n"); fflush(stdout); xdg_toplevel_set_minimized(top); }
		else if (!strcmp(s, "move")) pending_move = 1;
		else if (!strncmp(s, "resize ", 7)) {
			static const char *const names[] = {"top", "bottom", "left", "tl", "bl", "right", "tr", "br"};
			for (int i = 0; i < 8; i++)
				if (!strcmp(s + 7, names[i]))
					pending_resize = i + 1;
		}
		;
	}
}

int main(int argc, char **argv)
{
	int opt;
	while ((opt = getopt(argc, argv, "g:m:kc:S:")) != -1) {
		switch (opt) {
		case 'g': sscanf(optarg, "%dx%d", &gw, &gh); break;
		case 'm': margin0 = margin = atoi(optarg); break;
		case 'k': keep = 1; break;
		case 'c': color = strtoul(optarg, NULL, 16); break;
		case 'S': script = strdup(optarg); cur = script; break;
		}
	}
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "no display\n");
		return 1;
	}
	wl_registry_add_listener(wl_display_get_registry(dpy), &reg_l, NULL);
	wl_display_roundtrip(dpy);
	if (!comp || !shm || !wm) {
		fprintf(stderr, "missing globals\n");
		return 1;
	}
	surf = wl_compositor_create_surface(comp);
	xs = xdg_wm_base_get_xdg_surface(wm, surf);
	xdg_surface_add_listener(xs, &xs_l, NULL);
	top = xdg_surface_get_toplevel(xs);
	xdg_toplevel_add_listener(top, &tl_l, NULL);
	xdg_toplevel_set_title(top, "csdclient");
	xdg_toplevel_set_app_id(top, "csdclient");
	wl_surface_commit(surf);
	wake = now() + 1.0;
	for (;;) {
		wl_display_flush(dpy);
		struct pollfd pf = { wl_display_get_fd(dpy), POLLIN, 0 };
		if (poll(&pf, 1, 50) > 0 && wl_display_dispatch(dpy) < 0)
			return 0;
		if (mapped)
			steps();
		if (mapped && script && (!cur || !*cur) && now() >= wake && !pending_move && pending_resize < 0)
			return 0;	/* the script has run to its end */
	}
}
