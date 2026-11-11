/* The shm buffers wpebackend-fdo exports are server-side objects
 * (wl_shm_buffer, from wayland-server). wayland-server.h and wayland-client.h
 * cannot be included together, so freebws.c reaches them through these.
 * SPDX-License-Identifier: MIT */
#include <wayland-server.h>

const void *fbshm_begin(struct wl_shm_buffer *b, int *w, int *h, int *stride)
{
	*w = wl_shm_buffer_get_width(b);
	*h = wl_shm_buffer_get_height(b);
	*stride = wl_shm_buffer_get_stride(b);
	wl_shm_buffer_begin_access(b);
	return wl_shm_buffer_get_data(b);
}

void fbshm_end(struct wl_shm_buffer *b)
{
	wl_shm_buffer_end_access(b);
}
