/* Output and input backends: DRM/KMS dumb buffers with page flips, libseat
 * for device access, libinput for input; or no devices at all
 * (FLCOMP_HEADLESS=WxH) for tests.
 * SPDX-License-Identifier: MIT */
#include "comp.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libinput.h>
#include <libseat.h>
#include <libudev.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <drm_mode.h>

struct drm_buf {
	uint32_t handle, fb, pitch;
	size_t size;
	uint8_t *map;
};

static struct {
	struct libseat *seat;
	bool ready;			/* modeset done once */
	int fd, dev_id;
	uint32_t crtc, conn;
	drmModeModeInfo mode;
	drmModeCrtc *saved;
	struct drm_buf buf[2];
	int front;
	bool flip_pending;
	uintptr_t gen;			/* bumped by a mode change: older flip events are stale */
	drmModeModeInfo modes[64];	/* of the connector, no duplicates of size and rate */
	int nmodes;
	struct libinput_device *devs[48];
	int ndevs;
	struct udev *udev;
	struct libinput *li;
	struct wl_event_source *seat_src, *drm_src, *li_src;
} D = { .fd = -1 };

/* night light: a table per colour channel, used when copying to the screen */
static struct outmode hl_start;	/* headless: the size it started with */
static bool night_on;
static uint8_t night_lut[3][256];

static struct { int fd, id; } opened[64];

/* ---- libseat -------------------------------------------------------------- */

static int modeset(void);

static void seat_enable(struct libseat *s, void *ud)
{
	C.active = true;
	if (!D.ready)
		return;
	if (D.li)
		libinput_resume(D.li);
	modeset();
	damage_all();
}

static void seat_disable(struct libseat *s, void *ud)
{
	C.active = false;
	D.flip_pending = false;
	if (D.li)
		libinput_suspend(D.li);
	libseat_disable_seat(s);
}

static const struct libseat_seat_listener seat_listener = {
	.enable_seat = seat_enable, .disable_seat = seat_disable,
};

static int seat_cb(int fd, uint32_t mask, void *data)
{
	if (libseat_dispatch(D.seat, 0) < 0)
		quit(1);
	return 0;
}

static int open_dev(const char *path, int *fd)
{
	int id = libseat_open_device(D.seat, path, fd);
	if (id < 0)
		return -1;
	for (size_t i = 0; i < sizeof(opened) / sizeof(*opened); i++)
		if (!opened[i].fd) {
			opened[i].fd = *fd;
			opened[i].id = id;
			break;
		}
	return id;
}

static void close_dev(int fd)
{
	for (size_t i = 0; i < sizeof(opened) / sizeof(*opened); i++)
		if (opened[i].fd == fd) {
			libseat_close_device(D.seat, opened[i].id);
			opened[i].fd = 0;
			break;
		}
	close(fd);
}

/* ---- DRM ------------------------------------------------------------------- */

static void buf_destroy(struct drm_buf *b)
{
	if (b->map)
		munmap(b->map, b->size);
	if (b->fb)
		drmModeRmFB(D.fd, b->fb);
	if (b->handle) {
		struct drm_mode_destroy_dumb d = { .handle = b->handle };
		drmIoctl(D.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
	}
	memset(b, 0, sizeof(*b));
}

static int buf_create(struct drm_buf *b, int w, int h)
{
	struct drm_mode_create_dumb cr = { .width = w, .height = h, .bpp = 32 };
	if (drmIoctl(D.fd, DRM_IOCTL_MODE_CREATE_DUMB, &cr))
		return -1;
	b->handle = cr.handle;
	b->pitch = cr.pitch;
	b->size = cr.size;
	uint32_t handles[4] = { b->handle }, pitches[4] = { b->pitch }, offsets[4] = { 0 };
	if (drmModeAddFB2(D.fd, w, h, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &b->fb, 0) &&
	    drmModeAddFB(D.fd, w, h, 24, 32, b->pitch, b->handle, &b->fb))
		goto fail;
	struct drm_mode_map_dumb mp = { .handle = b->handle };
	if (drmIoctl(D.fd, DRM_IOCTL_MODE_MAP_DUMB, &mp))
		goto fail;
	void *m = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, D.fd, mp.offset);
	if (m == MAP_FAILED)
		goto fail;
	b->map = m;
	memset(b->map, 0, b->size);
	return 0;
fail:
	buf_destroy(b);
	return -1;
}

/* Find a connected connector and its CRTC on this device. */
static int pick_output(void)
{
	drmModeRes *res = drmModeGetResources(D.fd);
	if (!res)
		return -1;
	int ret = -1;
	for (int i = 0; i < res->count_connectors && ret < 0; i++) {
		drmModeConnector *cn = drmModeGetConnector(D.fd, res->connectors[i]);
		if (!cn)
			continue;
		if (cn->connection == DRM_MODE_CONNECTED && cn->count_modes > 0) {
			drmModeModeInfo *m = &cn->modes[0];
			for (int j = 0; j < cn->count_modes; j++)
				if (cn->modes[j].type & DRM_MODE_TYPE_PREFERRED) {
					m = &cn->modes[j];
					break;
				}
			uint32_t crtc = 0;
			drmModeEncoder *enc = cn->encoder_id ? drmModeGetEncoder(D.fd, cn->encoder_id) : NULL;
			if (enc && enc->crtc_id)
				crtc = enc->crtc_id;
			if (enc)
				drmModeFreeEncoder(enc);
			for (int e = 0; !crtc && e < cn->count_encoders; e++) {
				drmModeEncoder *en = drmModeGetEncoder(D.fd, cn->encoders[e]);
				if (!en)
					continue;
				for (int k = 0; k < res->count_crtcs; k++)
					if (en->possible_crtcs & (1u << k)) {
						crtc = res->crtcs[k];
						break;
					}
				drmModeFreeEncoder(en);
			}
			if (crtc) {
				D.conn = cn->connector_id;
				D.crtc = crtc;
				D.mode = *m;
				D.nmodes = 0;
				for (int j = 0; j < cn->count_modes && D.nmodes < 64; j++) {
					bool dup = false;
					for (int k = 0; k < D.nmodes; k++)
						if (D.modes[k].hdisplay == cn->modes[j].hdisplay &&
						    D.modes[k].vdisplay == cn->modes[j].vdisplay &&
						    D.modes[k].vrefresh == cn->modes[j].vrefresh)
							dup = true;
					if (!dup && !(cn->modes[j].flags & DRM_MODE_FLAG_INTERLACE))
						D.modes[D.nmodes++] = cn->modes[j];
				}
				ret = 0;
			}
		}
		drmModeFreeConnector(cn);
	}
	drmModeFreeResources(res);
	return ret;
}

static int modeset(void)
{
	if (drmModeSetCrtc(D.fd, D.crtc, D.buf[D.front].fb, 0, 0, &D.conn, 1, &D.mode)) {
		fprintf(stderr, "flcomp: drmModeSetCrtc: %s\n", strerror(errno));
		return -1;
	}
	return 0;
}

static void page_flip_done(int fd, unsigned seq, unsigned sec, unsigned usec, void *data)
{
	if ((uintptr_t)data != D.gen)
		return;	/* a flip from before the last mode change */
	D.front ^= 1;
	D.flip_pending = false;
	frame_done();
}

static int drm_cb(int fd, uint32_t mask, void *data)
{
	drmEventContext ctx = { .version = 2, .page_flip_handler = page_flip_done };
	drmHandleEvent(D.fd, &ctx);
	return 0;
}

static int open_drm(void)
{
	DIR *d = opendir("/dev/dri");
	if (!d)
		return -1;
	struct dirent *e;
	int ret = -1;
	while (ret < 0 && (e = readdir(d))) {
		if (strncmp(e->d_name, "card", 4))
			continue;
		char path[300];
		snprintf(path, sizeof(path), "/dev/dri/%s", e->d_name);
		int fd, id = open_dev(path, &fd);
		if (id < 0)
			continue;
		D.fd = fd;
		D.dev_id = id;
		if (pick_output() == 0)
			ret = 0;
		else {
			close_dev(fd);
			D.fd = -1;
		}
	}
	closedir(d);
	return ret;
}

static int drm_init(void)
{
	/* wait until the seat is enabled */
	struct pollfd pf = { .fd = libseat_get_fd(D.seat), .events = POLLIN };
	while (!C.active) {
		if (libseat_dispatch(D.seat, 0) < 0 || C.active)
			break;
		if (poll(&pf, 1, 3000) <= 0)
			break;
	}
	if (!C.active) {
		fprintf(stderr, "flcomp: seat not enabled (is seatd running, are you in its group?)\n");
		return -1;
	}
	if (open_drm() < 0) {
		fprintf(stderr, "flcomp: no usable display found in /dev/dri\n");
		return -1;
	}
	C.pw = C.w = D.mode.hdisplay;
	C.ph = C.h = D.mode.vdisplay;
	C.refresh = D.mode.vrefresh * 1000;
	for (int i = 0; i < 2; i++)
		if (buf_create(&D.buf[i], C.pw, C.ph)) {
			fprintf(stderr, "flcomp: cannot create %dx%d dumb buffer: %s\n", C.pw, C.ph,
				strerror(errno));
			return -1;
		}
	D.saved = drmModeGetCrtc(D.fd, D.crtc);
	D.front = 0;
	if (modeset())
		return -1;
	D.drm_src = wl_event_loop_add_fd(C.loop, D.fd, WL_EVENT_READABLE, drm_cb, NULL);
	return 0;
}

/* ---- libinput -------------------------------------------------------------- */

static int li_open(const char *path, int flags, void *ud)
{
	int fd;
	if (open_dev(path, &fd) < 0)
		return -errno;
	return fd;
}

static void li_close(int fd, void *ud)
{
	close_dev(fd);
}

static const struct libinput_interface li_iface = { .open_restricted = li_open,
						    .close_restricted = li_close };

/* The person's pointer settings (desktop.conf, see config.c) on one device. A
 * touchpad is what has tap fingers; the other pointers are mice. Settings the
 * device does not have are skipped. */
static void li_configure(struct libinput_device *dev)
{
	if (libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_POINTER)) {
		bool pad = libinput_device_config_tap_get_finger_count(dev) > 0;
		if (libinput_device_config_accel_is_available(dev)) {
			double sp = CF.pointer_speed / 100.0;
			libinput_device_config_accel_set_speed(dev, sp < -1 ? -1 : sp > 1 ? 1 : sp);
			uint32_t prof = libinput_device_config_accel_get_profiles(dev);
			enum libinput_config_accel_profile want = CF.accel_flat ? LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT
										: LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
			if (prof & want)
				libinput_device_config_accel_set_profile(dev, want);
		}
		if (libinput_device_config_scroll_has_natural_scroll(dev))
			libinput_device_config_scroll_set_natural_scroll_enabled(
				dev, pad ? CF.touchpad_natural : CF.mouse_natural);
		if (libinput_device_config_left_handed_is_available(dev))
			libinput_device_config_left_handed_set(dev, CF.left_handed);
		if (pad)
			libinput_device_config_tap_set_enabled(dev, CF.tap ? LIBINPUT_CONFIG_TAP_ENABLED
									   : LIBINPUT_CONFIG_TAP_DISABLED);
		if (libinput_device_config_dwt_is_available(dev))
			libinput_device_config_dwt_set_enabled(dev, CF.typing_disable ? LIBINPUT_CONFIG_DWT_ENABLED
										      : LIBINPUT_CONFIG_DWT_DISABLED);
	}
}

static void li_device_added(struct libinput_device *dev)
{
	if (libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_POINTER)) {
		C.has_pointer = true;
		cursor_damage();
	}
	if (D.ndevs < (int)(sizeof(D.devs) / sizeof(*D.devs)))
		D.devs[D.ndevs++] = libinput_device_ref(dev);
	li_configure(dev);
	if (libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_KEYBOARD))
		seat_apply_config();	/* its LEDs */
}

static void li_device_removed(struct libinput_device *dev)
{
	for (int i = 0; i < D.ndevs; i++)
		if (D.devs[i] == dev) {
			libinput_device_unref(dev);
			D.devs[i] = D.devs[--D.ndevs];
			break;
		}
}

/* settings changed: every device we know, now */
void backend_input_apply(void)
{
	for (int i = 0; i < D.ndevs; i++)
		li_configure(D.devs[i]);
}

void backend_set_leds(int leds)
{
	for (int i = 0; i < D.ndevs; i++)
		if (libinput_device_has_capability(D.devs[i], LIBINPUT_DEVICE_CAP_KEYBOARD))
			libinput_device_led_update(D.devs[i], (enum libinput_led)leds);
}

static void li_scroll(struct libinput_event_pointer *pe, bool wheel)
{
	for (int axis = 0; axis < 2; axis++) {
		enum libinput_pointer_axis ax = axis ? LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL
						     : LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL;
		if (!libinput_event_pointer_has_axis(pe, ax))
			continue;
		if (wheel) {
			double v = libinput_event_pointer_get_scroll_value_v120(pe, ax);
			int notches = (int)(v / 120.0 + (v < 0 ? -0.5 : 0.5));
			if (!notches && v)
				notches = v < 0 ? -1 : 1;
			seat_axis(now_ms(), axis, v / 120.0 * 15.0, notches, 0);
		} else {
			double v = libinput_event_pointer_get_scroll_value(pe, ax);
			seat_axis(now_ms(), axis, v, 0, 1);
		}
	}
}

static int li_cb(int fd, uint32_t mask, void *data)
{
	libinput_dispatch(D.li);
	struct libinput_event *ev;
	while ((ev = libinput_get_event(D.li))) {
		enum libinput_event_type et = libinput_event_get_type(ev);
		struct libinput_event_pointer *pe =
			et >= LIBINPUT_EVENT_POINTER_MOTION && et <= LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS
				? libinput_event_get_pointer_event(ev) : NULL;
		switch (libinput_event_get_type(ev)) {
		case LIBINPUT_EVENT_DEVICE_ADDED:
			li_device_added(libinput_event_get_device(ev));
			break;
		case LIBINPUT_EVENT_DEVICE_REMOVED:
			li_device_removed(libinput_event_get_device(ev));
			break;
		case LIBINPUT_EVENT_POINTER_MOTION: {
			/* one logical pixel is scale/100 screen pixels: keep the pointer's feel */
			double k = C.scale > 100 ? 100.0 / C.scale : 1.0;
			seat_motion(now_ms(), libinput_event_pointer_get_dx(pe) * k,
				    libinput_event_pointer_get_dy(pe) * k);
			break;
		}
		case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
			seat_motion_abs(now_ms(), libinput_event_pointer_get_absolute_x_transformed(pe, C.w),
					libinput_event_pointer_get_absolute_y_transformed(pe, C.h));
			break;
		case LIBINPUT_EVENT_POINTER_BUTTON:
			seat_button(now_ms(), libinput_event_pointer_get_button(pe),
				    libinput_event_pointer_get_button_state(pe) ==
					    LIBINPUT_BUTTON_STATE_PRESSED);
			break;
		case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
			li_scroll(pe, true);
			break;
		case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
		case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS:
			li_scroll(pe, false);
			break;
		case LIBINPUT_EVENT_KEYBOARD_KEY: {
			struct libinput_event_keyboard *ke = libinput_event_get_keyboard_event(ev);
			seat_key(now_ms(), libinput_event_keyboard_get_key(ke),
				 libinput_event_keyboard_get_key_state(ke) == LIBINPUT_KEY_STATE_PRESSED);
			break;
		}
		case LIBINPUT_EVENT_SWITCH_TOGGLE: {
			struct libinput_event_switch *se = libinput_event_get_switch_event(ev);
			if (libinput_event_switch_get_switch(se) == LIBINPUT_SWITCH_LID)
				lock_lid(libinput_event_switch_get_switch_state(se) == LIBINPUT_SWITCH_STATE_ON);
			break;
		}
		default:
			break;
		}
		libinput_event_destroy(ev);
	}
	return 0;
}

static int input_init(void)
{
	D.udev = udev_new();
	if (D.udev) {
		D.li = libinput_udev_create_context(&li_iface, NULL, D.udev);
		if (D.li && libinput_udev_assign_seat(D.li, libseat_seat_name(D.seat))) {
			libinput_unref(D.li);
			D.li = NULL;
		}
		if (D.li) {
			libinput_dispatch(D.li);
			if (libinput_next_event_type(D.li) == LIBINPUT_EVENT_NONE) {
				/* udev knows no input devices (no udevd?): scan by hand */
				libinput_unref(D.li);
				D.li = NULL;
			}
		}
	}
	if (!D.li) {
		D.li = libinput_path_create_context(&li_iface, NULL);
		for (int i = 0; D.li && i < 32; i++) {
			char path[64];
			snprintf(path, sizeof(path), "/dev/input/event%d", i);
			libinput_path_add_device(D.li, path);
		}
	}
	if (!D.li) {
		fprintf(stderr, "flcomp: no input\n");
		return -1;
	}
	D.li_src = wl_event_loop_add_fd(C.loop, libinput_get_fd(D.li), WL_EVENT_READABLE, li_cb, NULL);
	return 0;
}

/* ---- test input (headless only) -------------------------------------------- */

/* FLCOMP_TEST_FIFO=path: lines "move X Y", "button CODE 0|1", "key CODE 0|1",
 * "axis STEPS" (wheel, + is down), "lid 0|1". */
static int test_cb(int fd, uint32_t mask, void *data)
{
	static char buf[512];
	static size_t len;
	ssize_t n = read(fd, buf + len, sizeof(buf) - len - 1);
	if (n <= 0)
		return 0;
	len += (size_t)n;
	buf[len] = 0;
	char *line = buf, *nl;
	while ((nl = strchr(line, '\n'))) {
		*nl = 0;
		int a, b;
		if (sscanf(line, "move %d %d", &a, &b) == 2) {
			C.has_pointer = true;
			seat_motion_abs(now_ms(), a, b);
		} else if (sscanf(line, "button %d %d", &a, &b) == 2) {
			seat_button(now_ms(), a, b);
		} else if (sscanf(line, "key %d %d", &a, &b) == 2) {
			seat_key(now_ms(), a, b);
		} else if (sscanf(line, "axis %d", &a) == 1) {
			seat_axis(now_ms(), 0, a * 10.0, a, 0);
		} else if (sscanf(line, "lid %d", &a) == 1) {
			lock_lid(a != 0);
		}
		line = nl + 1;
	}
	len = strlen(line);
	memmove(buf, line, len);
	return 0;
}

/* ---- backend interface ---------------------------------------------------------- */

int backend_init(void)
{
	const char *hl = getenv("FLCOMP_HEADLESS");
	if (hl && *hl) {
		int w = 0, h = 0;
		if (sscanf(hl, "%dx%d", &w, &h) != 2 || w < 16 || h < 16 || w > 8192 || h > 8192) {
			w = 1024;
			h = 768;
		}
		C.headless = true;
		C.active = true;
		C.pw = C.w = w;
		C.ph = C.h = h;
		C.refresh = 60000;
		hl_start = (struct outmode){ w, h, 60, true };
		const char *fifo = getenv("FLCOMP_TEST_FIFO");
		if (fifo) {
			int fd = open(fifo, O_RDWR | O_NONBLOCK | O_CLOEXEC);
			if (fd >= 0)
				wl_event_loop_add_fd(C.loop, fd, WL_EVENT_READABLE, test_cb, NULL);
		}
		return 0;
	}
	D.seat = libseat_open_seat(&seat_listener, NULL);
	if (!D.seat) {
		fprintf(stderr, "flcomp: cannot open a seat: %s\n", strerror(errno));
		return -1;
	}
	D.seat_src = wl_event_loop_add_fd(C.loop, libseat_get_fd(D.seat), WL_EVENT_READABLE, seat_cb, NULL);
	if (drm_init() < 0)
		return -1;
	if (input_init() < 0)
		fprintf(stderr, "flcomp: continuing without input\n");
	D.ready = true;
	return 0;
}

/* Called once everything else is set up: handle the devices already found. */
void backend_start(void)
{
	if (D.li)
		li_cb(0, 0, NULL);
}

/* ---- night light ------------------------------------------------------------------------ */

/* warmth 0..100: green and blue go down, red stays (about 6500 K down to 2700 K) */
void backend_night_changed(void)
{
	night_on = CF.night && CF.night_warmth > 0;
	if (night_on)
		for (int i = 0; i < 256; i++) {
			night_lut[0][i] = (uint8_t)i;
			night_lut[1][i] = (uint8_t)(i * (1000 - 2.7 * CF.night_warmth) / 1000);
			night_lut[2][i] = (uint8_t)(i * (1000 - 5.5 * CF.night_warmth) / 1000);
		}
	damage_all();
}

static inline uint32_t night_px(uint32_t p)
{
	return (p & 0xff000000u) | (uint32_t)night_lut[0][(p >> 16) & 255] << 16 |
	       (uint32_t)night_lut[1][(p >> 8) & 255] << 8 | night_lut[2][p & 255];
}

/* ---- from the shadow buffer to the screen ------------------------------------------------- */

static void copy_row(uint8_t *dst, const uint32_t *src, int n)
{
	if (!night_on) {
		memcpy(dst, src, (size_t)n * 4);
		return;
	}
	uint32_t *o = (uint32_t *)dst;
	for (int i = 0; i < n; i++)
		o[i] = night_px(src[i]);
}

static inline uint32_t lerp(uint32_t a, uint32_t b, unsigned t)	/* t 0..256 */
{
	uint32_t rb = (((a & 0x00ff00ffu) * (256 - t) + (b & 0x00ff00ffu) * t) >> 8) & 0x00ff00ffu;
	uint32_t g = (((a & 0x0000ff00u) * (256 - t) + (b & 0x0000ff00u) * t) >> 8) & 0x0000ff00u;
	return rb | g;
}

/* The shadow is the desktop at its logical size; with a scale above 100 % the
 * screen gets it enlarged: pixel doubling at 200 %, bilinear in between. r is in
 * logical pixels. */
static void present_scaled(struct drm_buf *b, struct rect r)
{
	static int xi[8192];
	static uint8_t xf[8192];
	const int S = C.scale;
	r.x1 = r.x1 > 0 ? r.x1 - 1 : 0;
	r.y1 = r.y1 > 0 ? r.y1 - 1 : 0;
	r.x2 = r.x2 < C.w ? r.x2 + 1 : C.w;
	r.y2 = r.y2 < C.h ? r.y2 + 1 : C.h;
	int px1 = r.x1 * S / 100, px2 = (r.x2 * S + 99) / 100, py1 = r.y1 * S / 100, py2 = (r.y2 * S + 99) / 100;
	if (px2 > C.pw)
		px2 = C.pw;
	if (py2 > C.ph)
		py2 = C.ph;
	if (px2 > 8192)
		px2 = 8192;
	bool exact = S == 200;
	for (int x = px1; x < px2; x++) {
		if (exact) {
			xi[x] = x / 2;
			xf[x] = 0;
			continue;
		}
		int64_t sx = ((int64_t)(2 * x + 1) * 100 * 65536) / (2 * S) - 32768;
		if (sx < 0)
			sx = 0;
		xi[x] = (int)(sx >> 16);
		xf[x] = (uint8_t)((sx >> 8) & 255);
	}
	uint32_t line[8192];
	for (int y = py1; y < py2; y++) {
		int y0, y1;
		unsigned fy = 0;
		if (exact) {
			y0 = y1 = y / 2;
		} else {
			int64_t sy = ((int64_t)(2 * y + 1) * 100 * 65536) / (2 * S) - 32768;
			if (sy < 0)
				sy = 0;
			y0 = (int)(sy >> 16);
			fy = (unsigned)((sy >> 8) & 255);
			y1 = y0 + 1 < C.h ? y0 + 1 : y0;
		}
		if (y0 >= C.h)
			y0 = y1 = C.h - 1;
		const uint32_t *r0 = C.shadow + (size_t)y0 * C.w, *r1 = C.shadow + (size_t)y1 * C.w;
		for (int x = px1; x < px2; x++) {
			int x0 = xi[x] < C.w ? xi[x] : C.w - 1, x1 = x0 + 1 < C.w ? x0 + 1 : x0;
			if (exact) {
				line[x] = r0[x0];
			} else {
				unsigned fx = xf[x];
				line[x] = lerp(lerp(r0[x0], r0[x1], fx), lerp(r1[x0], r1[x1], fx), fy);
			}
		}
		copy_row(b->map + (size_t)y * b->pitch + (size_t)px1 * 4, line + px1, px2 - px1);
	}
}

void backend_present(const struct damage *d)
{
	if (C.headless) {
		frame_done();
		return;
	}
	struct drm_buf *b = &D.buf[D.front ^ 1];
	for (int i = 0; i < d->n; i++) {
		struct rect r = d->r[i];
		if (C.scale > 100) {
			present_scaled(b, r);
			continue;
		}
		for (int y = r.y1; y < r.y2; y++)
			copy_row(b->map + (size_t)y * b->pitch + (size_t)r.x1 * 4,
				 C.shadow + (size_t)y * C.w + r.x1, r.x2 - r.x1);
	}
	if (drmModePageFlip(D.fd, D.crtc, b->fb, DRM_MODE_PAGE_FLIP_EVENT, (void *)D.gen) == 0) {
		D.flip_pending = true;
		return;
	}
	/* flip refused: show the buffer by a full modeset instead */
	D.front ^= 1;
	modeset();
	frame_done();
}

/* ---- modes ----------------------------------------------------------------------------------- */

/* headless: made-up modes, so the whole path can be tested; hl_start is FLCOMP_HEADLESS=WxH */
static const struct outmode fake_modes[] = {
	{ 2560, 1440, 60, false }, { 1920, 1080, 60, false }, { 1600, 900, 60, false },
	{ 1366, 768, 60, false }, { 1280, 1024, 60, false }, { 1280, 800, 60, false },
	{ 1280, 720, 60, false }, { 1024, 768, 75, false }, { 1024, 768, 60, false },
};

int backend_modes(struct outmode *out, int max)
{
	int n = 0;
	if (C.headless) {
		bool have = false;
		for (size_t i = 0; i < sizeof(fake_modes) / sizeof(*fake_modes) && n < max; i++) {
			out[n] = fake_modes[i];
			out[n].preferred = fake_modes[i].w == hl_start.w && fake_modes[i].h == hl_start.h &&
					   fake_modes[i].hz == hl_start.hz;
			if (out[n].w == hl_start.w && out[n].h == hl_start.h && out[n].hz == hl_start.hz)
				have = true;
			n++;
		}
		if (!have && n < max)	/* the size it started with stays a mode, as the display's own */
			out[n++] = hl_start;
		return n;
	}
	for (int i = 0; i < D.nmodes && n < max; i++) {
		out[n].w = D.modes[i].hdisplay;
		out[n].h = D.modes[i].vdisplay;
		out[n].hz = (int)D.modes[i].vrefresh;
		out[n].preferred = D.modes[i].type & DRM_MODE_TYPE_PREFERRED;
		n++;
	}
	return n;
}

/* New dumb buffers for the mode, then the modeset; when the display refuses,
 * the old mode and buffers stay. */
bool backend_set_mode(int w, int h, int hz)
{
	if (C.headless) {
		struct outmode m[16];
		int n = backend_modes(m, 16);
		for (int i = 0; i < n; i++)
			if (m[i].w == w && m[i].h == h && m[i].hz == hz) {
				C.pw = w;
				C.ph = h;
				C.refresh = hz * 1000;
				return true;
			}
		return false;
	}
	if (!C.active)
		return false;
	drmModeModeInfo *m = NULL;
	for (int i = 0; i < D.nmodes; i++)
		if (D.modes[i].hdisplay == w && D.modes[i].vdisplay == h && (int)D.modes[i].vrefresh == hz)
			m = &D.modes[i];
	if (!m)
		return false;
	drmModeModeInfo old = D.mode;
	int oldfront = D.front;
	struct drm_buf nb[2] = { 0 };
	for (int i = 0; i < 2; i++)
		if (buf_create(&nb[i], w, h)) {
			buf_destroy(&nb[0]);
			buf_destroy(&nb[1]);
			return false;
		}
	if (drmModeSetCrtc(D.fd, D.crtc, nb[0].fb, 0, 0, &D.conn, 1, m)) {
		fprintf(stderr, "flcomp: mode %dx%d@%d refused: %s\n", w, h, hz, strerror(errno));
		buf_destroy(&nb[0]);
		buf_destroy(&nb[1]);
		drmModeSetCrtc(D.fd, D.crtc, D.buf[oldfront].fb, 0, 0, &D.conn, 1, &old);
		return false;
	}
	D.mode = *m;
	buf_destroy(&D.buf[0]);
	buf_destroy(&D.buf[1]);
	D.buf[0] = nb[0];
	D.buf[1] = nb[1];
	D.front = 0;
	D.gen++;
	D.flip_pending = false;
	C.pw = w;
	C.ph = h;
	C.refresh = hz * 1000;
	return true;
}

bool backend_busy(void)
{
	return D.flip_pending;
}

void backend_switch_vt(int n)
{
	if (D.seat)
		libseat_switch_session(D.seat, n);
}

void backend_fini(void)
{
	if (C.headless || !D.seat)
		return;
	for (int i = 0; i < D.ndevs; i++)
		libinput_device_unref(D.devs[i]);
	D.ndevs = 0;
	if (D.li) {
		libinput_unref(D.li);
		D.li = NULL;
	}
	if (D.fd >= 0 && C.active && D.saved && D.saved->mode_valid)
		drmModeSetCrtc(D.fd, D.saved->crtc_id, D.saved->buffer_id, D.saved->x, D.saved->y,
			       &D.conn, 1, &D.saved->mode);
	if (D.saved)
		drmModeFreeCrtc(D.saved);
	if (D.fd >= 0) {
		buf_destroy(&D.buf[0]);
		buf_destroy(&D.buf[1]);
		close_dev(D.fd);
	}
	libseat_close_seat(D.seat);
	if (D.udev)
		udev_unref(D.udev);
}

int backend_drm_fd(void)
{
	return C.headless ? -1 : D.fd;
}
