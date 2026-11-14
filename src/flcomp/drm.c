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

/* What the kernel side of one struct output (same index) needs. */
struct dout {
	uint32_t conn, crtc;		/* crtc 0: none picked yet */
	drmModeModeInfo mode;		/* the mode in use */
	drmModeModeInfo modes[64];	/* of the connector, preferred first, no duplicates of size and rate */
	int nmodes;
	drmModeCrtc *saved;		/* the CRTC as we found it, put back at the end */
	struct drm_buf buf[2];
	int front, full;		/* full: copy the whole picture the next n presents (new buffers) */
	bool flip_pending, lit;
	unsigned gen;			/* bumped by a mode change: older flip events are stale */
	int gamma;			/* size of the CRTC's gamma table, 0: none (virtio, simpledrm) */
};

struct output OUT[MAX_OUT];
int NOUT;

static struct {
	struct libseat *seat;
	bool ready;			/* the first modeset is output.c's */
	int fd, dev_id;
	struct dout o[MAX_OUT];
	struct libinput_device *devs[48];
	int ndevs;
	struct udev *udev;
	struct udev_monitor *mon;
	struct libinput *li;
	struct wl_event_source *seat_src, *drm_src, *li_src, *mon_src, *hp_timer;
} D = { .fd = -1 };

/* night light on outputs without a gamma table: factors per colour channel, applied while
 * copying to the screen (the hardware table is used where the CRTC has one) */
static struct outmode hl_start;	/* headless: the size it started with */
static int hl_outputs = 1;
static double tint[3] = { 1, 1, 1 };
static bool tint_on;
static uint8_t tint_lut[3][256];

static struct { int fd, id; } opened[64];

/* ---- libseat -------------------------------------------------------------- */

static void modeset_all(void);

static void seat_enable(struct libseat *s, void *ud)
{
	C.active = true;
	if (!D.ready)
		return;
	if (D.li)
		libinput_resume(D.li);
	modeset_all();
	damage_all();
}

static void seat_disable(struct libseat *s, void *ud)
{
	C.active = false;
	for (int i = 0; i < MAX_OUT; i++)
		D.o[i].flip_pending = false;
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

static const char *conn_type(uint32_t t)
{
	switch (t) {
	case DRM_MODE_CONNECTOR_VGA: return "VGA";
	case DRM_MODE_CONNECTOR_DVII: return "DVI-I";
	case DRM_MODE_CONNECTOR_DVID: return "DVI-D";
	case DRM_MODE_CONNECTOR_DVIA: return "DVI-A";
	case DRM_MODE_CONNECTOR_Composite: return "Composite";
	case DRM_MODE_CONNECTOR_SVIDEO: return "S-Video";
	case DRM_MODE_CONNECTOR_LVDS: return "LVDS";
	case DRM_MODE_CONNECTOR_Component: return "Component";
	case DRM_MODE_CONNECTOR_9PinDIN: return "DIN";
	case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
	case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
	case DRM_MODE_CONNECTOR_HDMIB: return "HDMI-B";
	case DRM_MODE_CONNECTOR_TV: return "TV";
	case DRM_MODE_CONNECTOR_eDP: return "eDP";
	case DRM_MODE_CONNECTOR_VIRTUAL: return "Virtual";
	case DRM_MODE_CONNECTOR_DSI: return "DSI";
	case DRM_MODE_CONNECTOR_DPI: return "DPI";
	case DRM_MODE_CONNECTOR_WRITEBACK: return "Writeback";
	default: return "Unknown";
	}
}

/* The connectors of this device into OUT/D.o. An entry stays when its connector goes away
 * (connected = false), so the list and the saved settings keep their order. */
static bool probe_drm(void)
{
	drmModeRes *res = drmModeGetResources(D.fd);
	if (!res)
		return false;
	bool changed = false;
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *cn = drmModeGetConnector(D.fd, res->connectors[i]);
		if (!cn)
			continue;
		int idx = -1;
		for (int k = 0; k < NOUT; k++)
			if (D.o[k].conn == cn->connector_id)
				idx = k;
		if (idx < 0) {
			if (NOUT >= MAX_OUT) {
				drmModeFreeConnector(cn);
				continue;
			}
			idx = NOUT++;
			memset(&OUT[idx], 0, sizeof OUT[idx]);
			D.o[idx].conn = cn->connector_id;
			snprintf(OUT[idx].name, sizeof OUT[idx].name, "%s-%u", conn_type(cn->connector_type),
				 cn->connector_type_id);
			OUT[idx].scale = 100;
			changed = true;
		}
		struct output *o = &OUT[idx];
		struct dout *d = &D.o[idx];
		bool now = cn->connection == DRM_MODE_CONNECTED && cn->count_modes > 0;
		if (now != o->connected)
			changed = true;
		o->connected = now;
		if (!now) {
			drmModeFreeConnector(cn);
			continue;
		}
		/* modes: the preferred one first, then as the kernel lists them (best first) */
		int n = 0;
		struct outmode om[64];
		drmModeModeInfo dm[64];
		for (int pass = 0; pass < 2; pass++)
			for (int j = 0; j < cn->count_modes && n < 64; j++) {
				bool pref = cn->modes[j].type & DRM_MODE_TYPE_PREFERRED;
				if (pref != (pass == 0) || (cn->modes[j].flags & DRM_MODE_FLAG_INTERLACE))
					continue;
				bool dup = false;
				for (int k = 0; k < n; k++)
					if (om[k].w == cn->modes[j].hdisplay && om[k].h == cn->modes[j].vdisplay &&
					    om[k].hz == (int)cn->modes[j].vrefresh)
						dup = true;
				if (dup)
					continue;
				om[n] = (struct outmode){ cn->modes[j].hdisplay, cn->modes[j].vdisplay,
							  (int)cn->modes[j].vrefresh, pref };
				dm[n++] = cn->modes[j];
			}
		if (n != o->nmodes || (n && memcmp(&om[0], &o->modes[0], sizeof om[0])))
			changed = true;
		memcpy(o->modes, om, sizeof(om[0]) * (size_t)n);
		memcpy(d->modes, dm, sizeof(dm[0]) * (size_t)n);
		o->nmodes = d->nmodes = n;
		if (!d->saved && !d->crtc && cn->encoder_id) {
			drmModeEncoder *enc = drmModeGetEncoder(D.fd, cn->encoder_id);
			if (enc && enc->crtc_id)
				d->saved = drmModeGetCrtc(D.fd, enc->crtc_id);
			if (enc)
				drmModeFreeEncoder(enc);
		}
		drmModeFreeConnector(cn);
	}
	drmModeFreeResources(res);
	return changed;
}

/* a CRTC this connector can use that no other lit output has */
static uint32_t find_crtc(int idx)
{
	drmModeRes *res = drmModeGetResources(D.fd);
	drmModeConnector *cn = res ? drmModeGetConnector(D.fd, D.o[idx].conn) : NULL;
	uint32_t pick = 0;
	if (!cn)
		goto out;
	bool used[32] = { 0 };
	for (int k = 0; k < NOUT; k++)
		if (k != idx && D.o[k].lit)
			for (int c = 0; c < res->count_crtcs && c < 32; c++)
				if (res->crtcs[c] == D.o[k].crtc)
					used[c] = true;
	if (D.o[idx].crtc)	/* keep ours while it is still free */
		for (int c = 0; c < res->count_crtcs && c < 32; c++)
			if (res->crtcs[c] == D.o[idx].crtc && !used[c])
				pick = D.o[idx].crtc;
	for (int e = 0; !pick && e < cn->count_encoders; e++) {
		drmModeEncoder *en = drmModeGetEncoder(D.fd, cn->encoders[e]);
		if (!en)
			continue;
		if (en->crtc_id && cn->encoder_id == en->encoder_id)	/* the one lit by the firmware first */
			for (int c = 0; c < res->count_crtcs && c < 32; c++)
				if (res->crtcs[c] == en->crtc_id && !used[c])
					pick = en->crtc_id;
		drmModeFreeEncoder(en);
	}
	for (int e = 0; !pick && e < cn->count_encoders; e++) {
		drmModeEncoder *en = drmModeGetEncoder(D.fd, cn->encoders[e]);
		if (!en)
			continue;
		for (int c = 0; !pick && c < res->count_crtcs && c < 32; c++)
			if ((en->possible_crtcs & (1u << c)) && !used[c])
				pick = res->crtcs[c];
		drmModeFreeEncoder(en);
	}
out:
	if (cn)
		drmModeFreeConnector(cn);
	if (res)
		drmModeFreeResources(res);
	return pick;
}

static void apply_gamma(struct dout *d)
{
	if (!d->lit || d->gamma < 2)
		return;
	uint16_t t[3][1024];
	int n = d->gamma > 1024 ? 1024 : d->gamma;
	for (int i = 0; i < n; i++)
		for (int c = 0; c < 3; c++)
			t[c][i] = (uint16_t)(i * 65535.0 / (n - 1) * tint[c] + 0.5);
	if (n == d->gamma)
		drmModeCrtcSetGamma(D.fd, d->crtc, (uint32_t)n, t[0], t[1], t[2]);
}

static int modeset_one(struct dout *d)
{
	if (drmModeSetCrtc(D.fd, d->crtc, d->buf[d->front].fb, 0, 0, &d->conn, 1, &d->mode)) {
		fprintf(stderr, "flcomp: drmModeSetCrtc: %s\n", strerror(errno));
		return -1;
	}
	apply_gamma(d);
	return 0;
}

static void modeset_all(void)
{
	for (int i = 0; i < NOUT; i++)
		if (D.o[i].lit)
			modeset_one(&D.o[i]);
}

static void out_dark(int idx)	/* switch the CRTC off and free its buffers */
{
	struct dout *d = &D.o[idx];
	if (d->lit && C.active)
		drmModeSetCrtc(D.fd, d->crtc, 0, 0, 0, NULL, 0, NULL);
	buf_destroy(&d->buf[0]);
	buf_destroy(&d->buf[1]);
	d->lit = false;
	d->flip_pending = false;
	d->gen++;
}

/* Light the output with a mode (or darken it): new dumb buffers, the modeset; when the display
 * refuses, the old state stays. */
bool backend_out_set(struct output *o, bool enable, int w, int h, int hz)
{
	int idx = (int)(o - OUT);
	if (C.headless) {
		if (enable) {
			bool ok = false;
			for (int i = 0; i < o->nmodes; i++)
				ok |= o->modes[i].w == w && o->modes[i].h == h && o->modes[i].hz == hz;
			if (!ok)
				return false;
			o->w = w;
			o->h = h;
			o->hz = hz;
		}
		o->enabled = enable;
		return true;
	}
	if (idx < 0 || idx >= NOUT || !C.active)
		return false;
	struct dout *d = &D.o[idx];
	if (!enable) {
		out_dark(idx);
		o->enabled = false;
		return true;
	}
	if (!o->connected)
		return false;
	drmModeModeInfo *m = NULL;
	for (int i = 0; i < d->nmodes; i++)
		if (d->modes[i].hdisplay == w && d->modes[i].vdisplay == h && (int)d->modes[i].vrefresh == hz)
			m = &d->modes[i];
	if (!m)
		return false;
	uint32_t crtc = find_crtc(idx);
	if (!crtc) {
		fprintf(stderr, "flcomp: no free CRTC for %s\n", o->name);
		return false;
	}
	struct drm_buf nb[2] = { 0 };
	for (int i = 0; i < 2; i++)
		if (buf_create(&nb[i], w, h)) {
			buf_destroy(&nb[0]);
			buf_destroy(&nb[1]);
			return false;
		}
	if (!d->saved)
		d->saved = drmModeGetCrtc(D.fd, crtc);
	uint32_t oldcrtc = d->crtc;
	drmModeCrtc *cur = d->lit ? NULL : drmModeGetCrtc(D.fd, crtc);
	d->crtc = crtc;
	if (drmModeSetCrtc(D.fd, crtc, nb[0].fb, 0, 0, &d->conn, 1, m)) {
		fprintf(stderr, "flcomp: %s: mode %dx%d@%d refused: %s\n", o->name, w, h, hz, strerror(errno));
		buf_destroy(&nb[0]);
		buf_destroy(&nb[1]);
		d->crtc = oldcrtc;
		if (d->lit)
			drmModeSetCrtc(D.fd, d->crtc, d->buf[d->front].fb, 0, 0, &d->conn, 1, &d->mode);
		if (cur)
			drmModeFreeCrtc(cur);
		return false;
	}
	if (cur)
		drmModeFreeCrtc(cur);
	buf_destroy(&d->buf[0]);
	buf_destroy(&d->buf[1]);
	d->buf[0] = nb[0];
	d->buf[1] = nb[1];
	d->mode = *m;
	d->front = 0;
	d->gen++;
	d->flip_pending = false;
	d->lit = true;
	d->full = 2;
	drmModeCrtc *cc = drmModeGetCrtc(D.fd, crtc);
	d->gamma = cc ? cc->gamma_size : 0;
	if (cc)
		drmModeFreeCrtc(cc);
	apply_gamma(d);
	o->enabled = true;
	o->w = w;
	o->h = h;
	o->hz = hz;
	return true;
}

static uintptr_t flip_tag(int idx, unsigned gen)
{
	return (uintptr_t)idx | ((uintptr_t)gen << 8);
}

static void page_flip_done(int fd, unsigned seq, unsigned sec, unsigned usec, void *data)
{
	int idx = (int)((uintptr_t)data & 255);
	unsigned gen = (unsigned)((uintptr_t)data >> 8);
	if (idx >= MAX_OUT)
		return;
	struct dout *d = &D.o[idx];
	if (gen != d->gen || !d->flip_pending)
		return;	/* a flip from before the last mode change */
	d->front ^= 1;
	d->flip_pending = false;
	if (!backend_busy())
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
		NOUT = 0;
		probe_drm();
		bool any = false;
		for (int i = 0; i < NOUT; i++)
			any |= OUT[i].connected;
		if (any)
			ret = 0;
		else {
			close_dev(fd);
			D.fd = -1;
		}
	}
	closedir(d);
	return ret;
}

/* ---- hotplug: the kernel says "change" on the card when a monitor is plugged in or out ------ */

static int hp_timer_cb(void *data)
{
	if (C.active && backend_probe())
		output_hotplug();
	return 0;
}

static int mon_cb(int fd, uint32_t mask, void *data)
{
	struct udev_device *dev;
	bool hit = false;
	while ((dev = udev_monitor_receive_device(D.mon))) {
		const char *hp = udev_device_get_property_value(dev, "HOTPLUG");
		if (hp && !strcmp(hp, "1"))
			hit = true;
		udev_device_unref(dev);
	}
	if (hit && D.hp_timer)
		wl_event_source_timer_update(D.hp_timer, 300);	/* settle: several events per plug */
	return 0;
}

static void hotplug_init(void)
{
	if (!D.udev)
		D.udev = udev_new();
	if (!D.udev)
		return;
	D.mon = udev_monitor_new_from_netlink(D.udev, "udev");
	if (!D.mon)
		return;
	udev_monitor_filter_add_match_subsystem_devtype(D.mon, "drm", NULL);
	if (udev_monitor_enable_receiving(D.mon) < 0) {
		udev_monitor_unref(D.mon);
		D.mon = NULL;
		return;
	}
	D.hp_timer = wl_event_loop_add_timer(C.loop, hp_timer_cb, NULL);
	D.mon_src = wl_event_loop_add_fd(C.loop, udev_monitor_get_fd(D.mon), WL_EVENT_READABLE, mon_cb, NULL);
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
			/* one logical pixel is scale/100 screen pixels: keep the pointer's feel (the primary's scale) */
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

/* headless: made-up modes, so the whole path can be tested; hl_start is FLCOMP_HEADLESS=WxH,
 * FLCOMP_HEADLESS_OUTPUTS=N gives N outputs (HEADLESS-1 ..) */
static const struct outmode fake_modes[] = {
	{ 2560, 1440, 60, false }, { 1920, 1080, 60, false }, { 1600, 900, 60, false },
	{ 1366, 768, 60, false }, { 1280, 1024, 60, false }, { 1280, 800, 60, false },
	{ 1280, 720, 60, false }, { 1024, 768, 75, false }, { 1024, 768, 60, false },
};

static void probe_headless(void)
{
	for (int i = 0; i < hl_outputs && i < MAX_OUT; i++) {
		struct output *o = &OUT[i];
		memset(o, 0, sizeof *o);
		snprintf(o->name, sizeof o->name, "HEADLESS-%d", i + 1);
		o->connected = true;
		o->scale = 100;
		int n = 0;
		bool have = false;
		/* the size it started with is the display's own (preferred) mode, first in the list */
		o->modes[n++] = (struct outmode){ hl_start.w, hl_start.h, hl_start.hz, true };
		for (size_t k = 0; k < sizeof(fake_modes) / sizeof(*fake_modes) && n < 64; k++) {
			if (fake_modes[k].w == hl_start.w && fake_modes[k].h == hl_start.h && fake_modes[k].hz == hl_start.hz)
				have = true;
			else
				o->modes[n++] = fake_modes[k];
		}
		(void)have;
		o->nmodes = n;
		NOUT = i + 1;
	}
}

bool backend_probe(void)
{
	if (C.headless)
		return false;
	return probe_drm();
}

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
		hl_start = (struct outmode){ w, h, 60, true };
		const char *no = getenv("FLCOMP_HEADLESS_OUTPUTS");
		if (no && atoi(no) > 1)
			hl_outputs = atoi(no) > 4 ? 4 : atoi(no);
		probe_headless();
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
	hotplug_init();
	D.ready = true;
	return 0;
}

/* Called once everything else is set up: handle the devices already found. */
void backend_start(void)
{
	if (D.li)
		li_cb(0, 0, NULL);
}

/* ---- night light, the part without a gamma table ----------------------------------------- */

static inline uint32_t tint_px(uint32_t p)
{
	return (p & 0xff000000u) | (uint32_t)tint_lut[0][(p >> 16) & 255] << 16 |
	       (uint32_t)tint_lut[1][(p >> 8) & 255] << 8 | tint_lut[2][p & 255];
}

/* the factors for every output: the CRTC's gamma table where it has one, else the copy loop's
 * table (that costs a table lookup per pixel while the tint is not neutral) */
void backend_tint(const double rgb[3])
{
	bool was = tint_on;
	bool soft = false;
	for (int c = 0; c < 3; c++)
		tint[c] = rgb[c] < 0 ? 0 : rgb[c] > 1 ? 1 : rgb[c];
	tint_on = tint[0] < 0.999 || tint[1] < 0.999 || tint[2] < 0.999;
	for (int i = 0; i < 256; i++)
		for (int c = 0; c < 3; c++)
			tint_lut[c][i] = (uint8_t)(i * tint[c] + 0.5);
	if (C.headless)
		return;
	for (int i = 0; i < NOUT; i++) {
		struct dout *d = &D.o[i];
		if (!d->lit)
			continue;
		if (d->gamma > 1)
			apply_gamma(d);
		else {
			soft = true;
			d->full = 2;
		}
	}
	if (soft && (tint_on || was))
		damage_all();
}

/* ---- from the shadow buffer to the screen ------------------------------------------------- */

static void copy_row(uint8_t *dst, const uint32_t *src, int n, bool soft)
{
	if (!soft || !tint_on) {
		memcpy(dst, src, (size_t)n * 4);
		return;
	}
	uint32_t *o = (uint32_t *)dst;
	for (int i = 0; i < n; i++)
		o[i] = tint_px(src[i]);
}

static inline uint32_t lerp(uint32_t a, uint32_t b, unsigned t)	/* t 0..256 */
{
	uint32_t rb = (((a & 0x00ff00ffu) * (256 - t) + (b & 0x00ff00ffu) * t) >> 8) & 0x00ff00ffu;
	uint32_t g = (((a & 0x0000ff00u) * (256 - t) + (b & 0x0000ff00u) * t) >> 8) & 0x0000ff00u;
	return rb | g;
}

/* One output's part of the shadow to its buffer. r is in desktop (logical) pixels and inside the
 * output. The output's picture is the desktop at its logical size; with a scale above 100 % the
 * screen gets it enlarged: pixel doubling at 200 %, bilinear in between. */
static void present_out(const struct output *o, struct dout *d, struct drm_buf *b, struct rect r)
{
	static int xi[8192];
	static uint8_t xf[8192];
	const int S = o->scale, lw = o->lw, lh = o->lh;
	const bool soft = d->gamma < 2;
	const uint32_t *base = C.shadow + (size_t)o->dy * C.w + o->dx;
	r.x1 -= o->dx;
	r.x2 -= o->dx;
	r.y1 -= o->dy;
	r.y2 -= o->dy;
	if (S == 100) {
		for (int y = r.y1; y < r.y2; y++)
			copy_row(b->map + (size_t)y * b->pitch + (size_t)r.x1 * 4, base + (size_t)y * C.w + r.x1,
				 r.x2 - r.x1, soft);
		return;
	}
	r.x1 = r.x1 > 0 ? r.x1 - 1 : 0;
	r.y1 = r.y1 > 0 ? r.y1 - 1 : 0;
	r.x2 = r.x2 < lw ? r.x2 + 1 : lw;
	r.y2 = r.y2 < lh ? r.y2 + 1 : lh;
	int px1 = r.x1 * S / 100, px2 = (r.x2 * S + 99) / 100, py1 = r.y1 * S / 100, py2 = (r.y2 * S + 99) / 100;
	if (px2 > o->w)
		px2 = o->w;
	if (py2 > o->h)
		py2 = o->h;
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
			y1 = y0 + 1 < lh ? y0 + 1 : y0;
		}
		if (y0 >= lh)
			y0 = y1 = lh - 1;
		const uint32_t *r0 = base + (size_t)y0 * C.w, *r1 = base + (size_t)y1 * C.w;
		for (int x = px1; x < px2; x++) {
			int x0 = xi[x] < lw ? xi[x] : lw - 1, x1 = x0 + 1 < lw ? x0 + 1 : x0;
			if (exact) {
				line[x] = r0[x0];
			} else {
				unsigned fx = xf[x];
				line[x] = lerp(lerp(r0[x0], r0[x1], fx), lerp(r1[x0], r1[x1], fx), fy);
			}
		}
		copy_row(b->map + (size_t)y * b->pitch + (size_t)px1 * 4, line + px1, px2 - px1, soft);
	}
}

void backend_present(const struct damage *dm)
{
	if (C.headless) {
		frame_done();
		return;
	}
	bool any = false;
	for (int i = 0; i < NOUT; i++) {
		struct output *o = &OUT[i];
		struct dout *d = &D.o[i];
		if (!o->enabled || !d->lit)
			continue;
		struct drm_buf *b = &d->buf[d->front ^ 1];
		struct rect orc = { o->dx, o->dy, o->dx + o->lw, o->dy + o->lh };
		if (d->full) {
			present_out(o, d, b, orc);
			d->full--;
		} else {
			for (int k = 0; k < dm->n; k++) {
				struct rect r = dm->r[k];
				if (r.x1 < orc.x1) r.x1 = orc.x1;
				if (r.y1 < orc.y1) r.y1 = orc.y1;
				if (r.x2 > orc.x2) r.x2 = orc.x2;
				if (r.y2 > orc.y2) r.y2 = orc.y2;
				if (r.x1 < r.x2 && r.y1 < r.y2)
					present_out(o, d, b, r);
			}
		}
		if (drmModePageFlip(D.fd, d->crtc, b->fb, DRM_MODE_PAGE_FLIP_EVENT, (void *)flip_tag(i, d->gen)) == 0) {
			d->flip_pending = true;
			any = true;
			continue;
		}
		/* flip refused: show the buffer by a full modeset instead */
		d->front ^= 1;
		modeset_one(d);
	}
	if (!any)
		frame_done();
}

bool backend_busy(void)
{
	for (int i = 0; i < NOUT; i++)
		if (D.o[i].flip_pending)
			return true;
	return false;
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
	for (int i = 0; i < NOUT; i++) {
		struct dout *d = &D.o[i];
		if (D.fd >= 0 && C.active && d->lit && d->saved && d->saved->mode_valid)
			drmModeSetCrtc(D.fd, d->saved->crtc_id, d->saved->buffer_id, d->saved->x, d->saved->y,
				       &d->conn, 1, &d->saved->mode);
		else if (D.fd >= 0 && C.active && d->lit)
			drmModeSetCrtc(D.fd, d->crtc, 0, 0, 0, NULL, 0, NULL);
		if (d->saved)
			drmModeFreeCrtc(d->saved);
		d->saved = NULL;
		if (D.fd >= 0) {
			buf_destroy(&d->buf[0]);
			buf_destroy(&d->buf[1]);
		}
	}
	if (D.fd >= 0)
		close_dev(D.fd);
	libseat_close_seat(D.seat);
	if (D.mon)
		udev_monitor_unref(D.mon);
	if (D.udev)
		udev_unref(D.udev);
}

int backend_drm_fd(void)
{
	return C.headless ? -1 : D.fd;
}
