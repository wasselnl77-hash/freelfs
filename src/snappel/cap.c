/* cap.c -- fl_screencopy capture and the clipboard server. Plain libwayland-client, no toolkit.
 * SPDX-License-Identifier: MIT */
#include "cap.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wayland-client.h>

#include "fl-screencopy-client-protocol.h"

#define BACK 0x1d2125u	/* behind a window's transparent corners */

static struct wl_display *dpy;
static struct wl_registry *reg;
static struct wl_shm *shm;
static struct fl_screencopy_manager *mgr;
static struct wl_seat *seat;
static struct wl_data_device_manager *ddm;

static void on_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t v)
{
	if (!strcmp(iface, "wl_shm"))
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, "fl_screencopy_manager"))
		mgr = wl_registry_bind(r, name, &fl_screencopy_manager_interface, 1);
	else if (!strcmp(iface, "wl_seat"))
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
	else if (!strcmp(iface, "wl_data_device_manager"))
		ddm = wl_registry_bind(r, name, &wl_data_device_manager_interface, 1);
}
static void on_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_l = { on_global, on_remove };

static int connect_all(void)
{
	dpy = wl_display_connect(NULL);
	if (!dpy)
		return -1;
	reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_l, NULL);
	wl_display_roundtrip(dpy);
	return 0;
}

int cap_open(void)
{
	if (dpy)
		return mgr && shm ? 0 : -1;
	if (connect_all() < 0)
		return -1;
	return mgr && shm ? 0 : -1;
}

void cap_close(void)
{
	if (dpy)
		wl_display_disconnect(dpy);
	dpy = NULL;
	mgr = NULL;
	shm = NULL;
}

/* ---- one frame ---------------------------------------------------------------------------- */

struct cap {
	int state;	/* 0 waiting, 1 ready, -1 failed */
	uint32_t fmt, w, h, stride;
	int fd;
	uint32_t *map;
	size_t size;
};

static void fr_buffer(void *d, struct fl_screencopy_frame *f, uint32_t fmt, uint32_t w, uint32_t h, uint32_t stride)
{
	struct cap *c = d;
	c->fmt = fmt;
	c->w = w;
	c->h = h;
	c->stride = stride;
	c->size = (size_t)stride * h;
	c->fd = memfd_create("snappel-cap", MFD_CLOEXEC);
	if (c->fd < 0 || ftruncate(c->fd, (off_t)c->size) < 0) {
		c->state = -1;
		return;
	}
	c->map = mmap(NULL, c->size, PROT_READ, MAP_SHARED, c->fd, 0);
	if (c->map == MAP_FAILED) {
		c->map = NULL;
		c->state = -1;
		return;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, c->fd, (int32_t)c->size);
	struct wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, (int32_t)w, (int32_t)h, (int32_t)stride, fmt);
	wl_shm_pool_destroy(pool);
	fl_screencopy_frame_copy(f, b);
}
static void fr_ready(void *d, struct fl_screencopy_frame *f) { ((struct cap *)d)->state = 1; }
static void fr_failed(void *d, struct fl_screencopy_frame *f) { ((struct cap *)d)->state = -1; }
static const struct fl_screencopy_frame_listener fr_l = { fr_buffer, fr_ready, fr_failed };

static int run_frame(struct fl_screencopy_frame *f, img_t *out)
{
	struct cap c = { .fd = -1 };
	fl_screencopy_frame_add_listener(f, &fr_l, &c);
	while (c.state == 0 && wl_display_dispatch(dpy) >= 0)
		;
	int rc = -1;
	if (c.state == 1 && c.map) {
		img_t im = img_new((int)c.w, (int)c.h);
		if (im.px) {
			for (uint32_t y = 0; y < c.h; y++)
				for (uint32_t x = 0; x < c.w; x++) {
					uint32_t p = c.map[(size_t)y * c.stride / 4 + x];
					uint32_t a = p >> 24;
					if (c.fmt == 0 && a != 255) {	/* premultiplied: over the backdrop */
						uint32_t ia = 255 - a, o = 0xff000000u;
						for (int sh = 0; sh <= 16; sh += 8) {
							uint32_t v = ((p >> sh) & 255) + (((BACK >> sh) & 255) * ia + 127) / 255;
							o |= (v > 255 ? 255 : v) << sh;
						}
						p = o;
					}
					im.px[(size_t)y * c.w + x] = p | 0xff000000u;
				}
			*out = im;
			rc = 0;
		}
	}
	if (c.map)
		munmap(c.map, c.size);
	if (c.fd >= 0)
		close(c.fd);
	fl_screencopy_frame_destroy(f);
	return rc;
}

int cap_output(img_t *out)
{
	if (cap_open() < 0)
		return -1;
	return run_frame(fl_screencopy_manager_capture_output(mgr), out);
}

int cap_window(uint32_t id, img_t *out)
{
	if (cap_open() < 0)
		return -1;
	return run_frame(fl_screencopy_manager_capture_window(mgr, id), out);
}

/* ---- the window list ---------------------------------------------------------------------- */

struct wl_list_state {
	struct capwin *w;
	int n, max;
	int done;
};

static struct wl_list_state *cur_ls;
static int ls_hooked;

static void ls_window(void *d, struct fl_screencopy_manager *m, uint32_t id, int32_t x, int32_t y, uint32_t w,
		      uint32_t h, const char *app, const char *title)
{
	struct wl_list_state *s = cur_ls;
	if (!s)
		return;
	if (s->n >= s->max)
		return;
	struct capwin *c = &s->w[s->n++];
	c->id = id;
	c->x = x;
	c->y = y;
	c->w = (int)w;
	c->h = (int)h;
	snprintf(c->app, sizeof c->app, "%s", app);
	snprintf(c->title, sizeof c->title, "%s", title);
}
static void ls_done(void *d, struct fl_screencopy_manager *m) { if (cur_ls) cur_ls->done = 1; }
static const struct fl_screencopy_manager_listener ls_l = { ls_window, ls_done };

int cap_windows(struct capwin *w, int max)
{
	if (cap_open() < 0)
		return 0;
	struct wl_list_state s = { w, 0, max, 0 };
	if (!ls_hooked) {
		fl_screencopy_manager_add_listener(mgr, &ls_l, NULL);
		ls_hooked = 1;
	}
	cur_ls = &s;
	fl_screencopy_manager_list_windows(mgr);
	while (!s.done && wl_display_dispatch(dpy) >= 0)
		;
	cur_ls = NULL;
	return s.n;
}

/* ---- the clipboard server ---------------------------------------------------------------------- */

static int clip_fd = -1;
static int clip_done;

static void ds_target(void *d, struct wl_data_source *s, const char *m) {}
static void ds_send(void *d, struct wl_data_source *s, const char *mime, int32_t fd)
{
	if (!strcmp(mime, "image/png")) {
		char buf[16384];
		off_t off = 0;
		ssize_t n;
		while ((n = pread(clip_fd, buf, sizeof buf, off)) > 0) {
			ssize_t done = 0;
			while (done < n) {
				ssize_t k = write(fd, buf + done, (size_t)(n - done));
				if (k < 0 && errno == EINTR)
					continue;
				if (k <= 0)
					goto out;
				done += k;
			}
			off += n;
		}
	}
out:
	close(fd);
}
static void ds_cancelled(void *d, struct wl_data_source *s) { clip_done = 1; }
static const struct wl_data_source_listener ds_l = { ds_target, ds_send, ds_cancelled };

/* the child: its own connection, the picture from an unlinked temporary file */
static void clip_child(int fd)
{
	clip_fd = fd;
	signal(SIGPIPE, SIG_IGN);
	for (int i = 3; i < 256; i++)
		if (i != fd)
			close(i);
	dpy = NULL;
	mgr = NULL;
	shm = NULL;
	if (connect_all() < 0 || !seat || !ddm)
		_exit(1);
	struct wl_data_device *dev = wl_data_device_manager_get_data_device(ddm, seat);
	struct wl_data_source *src = wl_data_device_manager_create_data_source(ddm);
	wl_data_source_add_listener(src, &ds_l, NULL);
	wl_data_source_offer(src, "image/png");
	wl_data_device_set_selection(dev, src, 0);
	wl_display_flush(dpy);
	while (!clip_done && wl_display_dispatch(dpy) >= 0)
		;
	_exit(0);
}

int clip_copy(const img_t *im)
{
	const char *rt = getenv("XDG_RUNTIME_DIR");
	char path[600];
	snprintf(path, sizeof path, "%s/snappel-clip-%d.png", rt && *rt ? rt : "/tmp", (int)getpid());
	if (img_save_png(im, path) < 0)
		return -1;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	unlink(path);
	if (fd < 0)
		return -1;
	pid_t pid = fork();
	if (pid < 0) {
		close(fd);
		return -1;
	}
	if (pid == 0) {
		setsid();
		pid_t pid2 = fork();	/* not our child, so nobody has to wait for it */
		if (pid2 != 0)
			_exit(pid2 < 0 ? 1 : 0);
		clip_child(fd);
	}
	close(fd);
	int st;
	waitpid(pid, &st, 0);
	return 0;
}
