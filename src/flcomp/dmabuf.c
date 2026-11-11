/* zwp_linux_dmabuf_v1 (version 3): buffers from GPU clients.
 *
 * Single and multi-plane buffers (AMD's DCC modifiers keep compression metadata in a
 * second plane; only the GPU import can read those).
 * Every dmabuf is copied into the surface's CPU copy when it is committed, like
 * a wl_shm buffer, and released at once. Two ways to read it:
 *   - linear buffers are mmap'ed and copied row by row (the fallback: this is
 *     all there is with llvmpipe, a KMS-only device or no GPU at all);
 *   - on a real GPU, buffers with a tiled or compressed layout are imported as
 *     an EGLImage and drawn by the GPU into linear memory (gl.c).
 * Compositing stays on the CPU in both cases.
 * SPDX-License-Identifier: MIT */
#include "comp.h"

#include <drm_fourcc.h>
#include <errno.h>
#include <linux/dma-buf.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "linux-dmabuf-unstable-v1-protocol.h"

#define MOD_LINEAR DRM_FORMAT_MOD_LINEAR
#define MOD_INVALID DRM_FORMAT_MOD_INVALID

static const uint32_t formats[] = {
	DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888, DRM_FORMAT_ABGR8888, DRM_FORMAT_XBGR8888,
};
#define NFORMATS (sizeof(formats) / sizeof(*formats))

/* ---- wl_buffer ---------------------------------------------------------- */

static void buf_destroy_req(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static const struct wl_buffer_interface dbuf_impl = { .destroy = buf_destroy_req };

static void dbuf_free(struct wl_resource *r)
{
	struct dbuf *b = wl_resource_get_user_data(r);
	close(b->fd);
	for (int i = 0; i < b->nplanes - 1; i++)
		close(b->extra[i].fd);
	free(b);
}

struct dbuf *dbuf_get(struct wl_resource *r)
{
	if (!r || !wl_resource_instance_of(r, &wl_buffer_interface, &dbuf_impl))
		return NULL;
	return wl_resource_get_user_data(r);
}

static bool dbuf_alpha(const struct dbuf *b)
{
	return b->format == DRM_FORMAT_ARGB8888 || b->format == DRM_FORMAT_ABGR8888;
}

static bool dbuf_swapped(const struct dbuf *b)
{
	return b->format == DRM_FORMAT_ABGR8888 || b->format == DRM_FORMAT_XBGR8888;
}

bool dbuf_has_alpha(const struct dbuf *b) { return dbuf_alpha(b); }

/* Copy rows dmg of the buffer into dst (w * h XRGB/ARGB, as the surface keeps it). */
bool dbuf_read(struct dbuf *b, uint32_t *dst, struct rect dmg)
{
	if (b->modifier != MOD_LINEAR && b->modifier != MOD_INVALID)
		return gl_read(b, dst, dmg);
	if (b->modifier == MOD_INVALID && gl_usable() && gl_read(b, dst, dmg))
		return true;
	size_t len = (size_t)b->offset + (size_t)b->stride * b->h;
	uint8_t *m = mmap(NULL, len, PROT_READ, MAP_SHARED, b->fd, 0);
	if (m == MAP_FAILED)
		return false;
	struct dma_buf_sync sy = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
	ioctl(b->fd, DMA_BUF_IOCTL_SYNC, &sy);	/* a memfd (tests) has no such ioctl: fine */
	bool swap = dbuf_swapped(b);
	for (int y = dmg.y1; y < dmg.y2; y++) {
		const uint32_t *s = (const uint32_t *)(m + b->offset + (size_t)y * b->stride) + dmg.x1;
		uint32_t *d = dst + (size_t)y * b->w + dmg.x1;
		int n = dmg.x2 - dmg.x1;
		if (!swap)
			memcpy(d, s, (size_t)n * 4);
		else
			for (int i = 0; i < n; i++) {
				uint32_t p = s[i];
				d[i] = (p & 0xff00ff00u) | (p & 0xff) << 16 | (p >> 16 & 0xff);
			}
	}
	sy.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
	ioctl(b->fd, DMA_BUF_IOCTL_SYNC, &sy);
	munmap(m, len);
	return true;
}

/* ---- zwp_linux_buffer_params_v1 ----------------------------------------- */

struct params {
	struct wl_resource *res;
	struct { int fd; uint32_t offset, stride; bool set; } pl[4];
	uint64_t modifier;	/* of plane 0 */
	bool used;
};

static void params_close(struct params *p)
{
	for (int i = 0; i < 4; i++) {
		if (p->pl[i].fd >= 0)
			close(p->pl[i].fd);
		p->pl[i].fd = -1;
	}
}

static void params_free(struct wl_resource *r)
{
	struct params *p = wl_resource_get_user_data(r);
	params_close(p);
	free(p);
}

static void params_destroy_req(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static void params_add(struct wl_client *c, struct wl_resource *r, int32_t fd, uint32_t plane,
		       uint32_t offset, uint32_t stride, uint32_t mod_hi, uint32_t mod_lo)
{
	struct params *p = wl_resource_get_user_data(r);
	if (p->used) {
		close(fd);
		wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED, "already used");
		return;
	}
	if (plane >= 4 || p->pl[plane].set) {
		close(fd);
		wl_resource_post_error(r, plane >= 4 ? ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX
						     : ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_SET,
				       "plane %u is bad or already set", plane);
		return;
	}
	p->pl[plane].fd = fd;
	p->pl[plane].offset = offset;
	p->pl[plane].stride = stride;
	p->pl[plane].set = true;
	if (plane == 0)
		p->modifier = (uint64_t)mod_hi << 32 | mod_lo;
}

static bool format_ok(uint32_t f)
{
	for (size_t i = 0; i < NFORMATS; i++)
		if (formats[i] == f)
			return true;
	return false;
}

/* Returns the new wl_buffer, or NULL after a protocol error / failure (*fail set: send failed). */
static struct wl_resource *make_buffer(struct wl_client *c, struct wl_resource *r, uint32_t id,
				       int32_t w, int32_t h, uint32_t format, bool *fail)
{
	struct params *p = wl_resource_get_user_data(r);
	*fail = false;
	if (p->used) {
		wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_ALREADY_USED, "already used");
		return NULL;
	}
	p->used = true;
	int np = 0;
	while (np < 4 && p->pl[np].set)
		np++;
	if (np == 0) {
		wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE, "no plane");
		return NULL;
	}
	for (int i = np; i < 4; i++)
		if (p->pl[i].set) {
			wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE, "plane %d without %d", i, i - 1);
			return NULL;
		}
	if (np > 1 && p->modifier == MOD_LINEAR) {
		wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INCOMPLETE, "a linear buffer has one plane");
		return NULL;
	}
	if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
		wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_DIMENSIONS, "bad size");
		return NULL;
	}
	if (!format_ok(format)) {
		wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_FORMAT,
				       "format %#x not supported", format);
		return NULL;
	}
	off_t size = lseek(p->pl[0].fd, 0, SEEK_END);
	if (p->pl[0].stride < (uint32_t)w * 4 || size < 0 ||
	    (uint64_t)p->pl[0].offset + (uint64_t)p->pl[0].stride * h > (uint64_t)size) {
		wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_OUT_OF_BOUNDS,
				       "stride or offset outside the buffer");
		return NULL;
	}
	if (!gl_modifier_ok(format, p->modifier, np)) {
		*fail = true;
		return NULL;
	}
	struct dbuf *b = calloc(1, sizeof(*b));
	if (!b) {
		wl_client_post_no_memory(c);
		return NULL;
	}
	b->fd = p->pl[0].fd;
	b->offset = p->pl[0].offset;
	b->stride = p->pl[0].stride;
	b->nplanes = np;
	for (int i = 1; i < np; i++) {
		b->extra[i - 1].fd = p->pl[i].fd;
		b->extra[i - 1].offset = p->pl[i].offset;
		b->extra[i - 1].stride = p->pl[i].stride;
	}
	b->w = w;
	b->h = h;
	b->format = format;
	b->modifier = p->modifier;
	for (int i = 0; i < 4; i++)
		p->pl[i].fd = -1;	/* the buffer owns them now */
	struct wl_resource *buf = wl_resource_create(c, &wl_buffer_interface, 1, id);
	if (!buf) {
		close(b->fd);
		for (int i = 0; i < np - 1; i++)
			close(b->extra[i].fd);
		free(b);
		wl_client_post_no_memory(c);
		return NULL;
	}
	wl_resource_set_implementation(buf, &dbuf_impl, b, dbuf_free);
	return buf;
}

static void params_create(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h,
			  uint32_t format, uint32_t flags)
{
	bool fail;
	struct wl_resource *buf = make_buffer(c, r, 0, w, h, format, &fail);
	if (buf)
		zwp_linux_buffer_params_v1_send_created(r, buf);
	else if (fail)
		zwp_linux_buffer_params_v1_send_failed(r);
}

static void params_create_immed(struct wl_client *c, struct wl_resource *r, uint32_t id, int32_t w,
				int32_t h, uint32_t format, uint32_t flags)
{
	bool fail;
	struct wl_resource *buf = make_buffer(c, r, id, w, h, format, &fail);
	if (!buf && fail)
		wl_resource_post_error(r, ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
				       "the buffer cannot be imported");
}

static const struct zwp_linux_buffer_params_v1_interface params_impl = {
	.destroy = params_destroy_req,
	.add = params_add,
	.create = params_create,
	.create_immed = params_create_immed,
};

/* ---- zwp_linux_dmabuf_v1 ------------------------------------------------- */

static void dmabuf_destroy_req(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static void dmabuf_create_params(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	struct params *p = calloc(1, sizeof(*p));
	if (!p) {
		wl_client_post_no_memory(c);
		return;
	}
	for (int i = 0; i < 4; i++)
		p->pl[i].fd = -1;
	p->res = wl_resource_create(c, &zwp_linux_buffer_params_v1_interface,
				    wl_resource_get_version(r), id);
	if (!p->res) {
		free(p);
		wl_client_post_no_memory(c);
		return;
	}
	wl_resource_set_implementation(p->res, &params_impl, p, params_free);
}

static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
	.destroy = dmabuf_destroy_req,
	.create_params = dmabuf_create_params,
};

static void dmabuf_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &zwp_linux_dmabuf_v1_interface, version, id);
	if (!r) {
		wl_client_post_no_memory(c);
		return;
	}
	wl_resource_set_implementation(r, &dmabuf_impl, NULL, NULL);
	gl_start();	/* first client that wants dmabuf: now find out what the GPU can read */
	for (size_t i = 0; i < NFORMATS; i++) {
		if (version < 3) {
			zwp_linux_dmabuf_v1_send_format(r, formats[i]);
			continue;
		}
		uint64_t mods[64];
		int n = gl_modifiers(formats[i], mods, 64);
		for (int m = 0; m < n; m++)
			zwp_linux_dmabuf_v1_send_modifier(r, formats[i], mods[m] >> 32,
							  mods[m] & 0xffffffffu);
	}
}

void dmabuf_init(void)
{
	wl_global_create(C.dpy, &zwp_linux_dmabuf_v1_interface, 3, NULL, dmabuf_bind);
}
