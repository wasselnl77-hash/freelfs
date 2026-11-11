/* fl-drawtest -- the M6 check: draw on the screen with EGL and OpenGL ES
 * through KMS and GBM, from a normal user's session, with the GPU and
 * screen handed out by seatd.
 *
 * It shows a first sketch of the FreeLFS desktop in the boot menu's
 * colours: a bottom panel with the start button, a window list and the
 * tray, and one window. All flat rectangles (scissored clears), no text:
 * fonts come with the toolkit in M7. A highlight moves along the window
 * list so page flips can be seen working.
 *
 *   fl-drawtest [-t SECONDS] [-o FILE.ppm]
 *
 * Prints the GL renderer and the frame rate; -o saves the last frame.
 * SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <gbm.h>
#include <libseat.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("fl-drawtest: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	exit(1);
}

/* ---- the seat --------------------------------------------------------------------- */
static bool seat_active;

static void seat_enable(struct libseat *seat, void *data)
{
	seat_active = true;
}

static void seat_disable(struct libseat *seat, void *data)
{
	seat_active = false;
	libseat_disable_seat(seat);
}

static struct libseat_seat_listener seat_listener = {
	.enable_seat = seat_enable,
	.disable_seat = seat_disable,
};

/* ---- the display -------------------------------------------------------------------- */
struct output {
	int fd;
	uint32_t conn, crtc;
	drmModeModeInfo mode;
	drmModeCrtc *saved;
};

/* the first card with a connected screen */
static bool find_output(struct libseat *seat, struct output *o, int *devid)
{
	drmDevicePtr devs[16];
	int n = drmGetDevices2(0, devs, 16);
	for (int i = 0; i < n; i++) {
		if (!(devs[i]->available_nodes & (1 << DRM_NODE_PRIMARY)))
			continue;
		const char *path = devs[i]->nodes[DRM_NODE_PRIMARY];
		int fd;
		int id = libseat_open_device(seat, path, &fd);
		if (id < 0)
			continue;
		drmModeRes *res = drmModeGetResources(fd);
		for (int c = 0; res && c < res->count_connectors; c++) {
			drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[c]);
			if (!conn)
				continue;
			if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes) {
				/* the preferred mode, or the first */
				o->mode = conn->modes[0];
				for (int m = 0; m < conn->count_modes; m++)
					if (conn->modes[m].type & DRM_MODE_TYPE_PREFERRED) {
						o->mode = conn->modes[m];
						break;
					}
				/* a CRTC its encoders can use */
				for (int e = 0; e < conn->count_encoders && !o->crtc; e++) {
					drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoders[e]);
					for (int k = 0; enc && k < res->count_crtcs; k++)
						if (enc->possible_crtcs & (1u << k)) {
							o->crtc = res->crtcs[k];
							break;
						}
					drmModeFreeEncoder(enc);
				}
				if (o->crtc) {
					o->fd = fd;
					o->conn = conn->connector_id;
					o->saved = drmModeGetCrtc(fd, o->crtc);
					printf("display: %s, %dx%d@%d on %s\n", "connected", o->mode.hdisplay,
					       o->mode.vdisplay, o->mode.vrefresh, path);
					drmModeFreeConnector(conn);
					drmModeFreeResources(res);
					drmFreeDevices(devs, n);
					*devid = id;
					return true;
				}
			}
			drmModeFreeConnector(conn);
		}
		drmModeFreeResources(res);
		libseat_close_device(seat, id);
	}
	drmFreeDevices(devs, n);
	return false;
}

/* ---- drawing -------------------------------------------------------------------------- */
struct rgb {
	float r, g, b;
};

static struct rgb hex(uint32_t c)
{
	return (struct rgb){((c >> 16) & 0xff) / 255.0f, ((c >> 8) & 0xff) / 255.0f, (c & 0xff) / 255.0f};
}

static int H; /* screen height: GL's y goes up, ours goes down */

static void rect(int x, int y, int w, int h, uint32_t color)
{
	if (w <= 0 || h <= 0)
		return;
	struct rgb c = hex(color);
	glScissor(x, H - y - h, w, h);
	glClearColor(c.r, c.g, c.b, 1);
	glClear(GL_COLOR_BUFFER_BIT);
}

/* The palette (flboot's) */
#define BACK 0x1d2125
#define PANEL 0x272c33
#define RAISED 0x30363e
#define LINE 0x3a4048
#define TEXT 0xd8dce0
#define DIM 0x8a9199
#define ACCENT 0x3f9b63

/* Text stand-ins: short bars the height of a line of text */
static void textbar(int x, int y, int w, int s, uint32_t color)
{
	rect(x, y + 4 * s, w, 6 * s, color);
}

static void draw(int W, int Hh, double t)
{
	H = Hh;
	int s = Hh >= 1400 ? 2 : 1; /* scale */
	glEnable(GL_SCISSOR_TEST);
	rect(0, 0, W, Hh, BACK);

	/* a window */
	int wx = W / 6, wy = Hh / 8, ww = W * 7 / 12, wh = Hh * 5 / 8;
	rect(wx - 1, wy - 1, ww + 2, wh + 2, LINE);
	rect(wx, wy, ww, wh, PANEL);
	rect(wx, wy, ww, 34 * s, RAISED);              /* title bar */
	textbar(wx + 14 * s, wy + 10 * s, 160 * s, s, TEXT);
	for (int i = 0; i < 3; i++)                    /* window buttons */
		rect(wx + ww - (34 + i * 28) * s, wy + 11 * s, 12 * s, 12 * s, i == 0 ? DIM : LINE);
	/* sidebar and content */
	rect(wx, wy + 34 * s, 190 * s, wh - 34 * s, BACK);
	for (int i = 0; i < 6; i++) {
		int y = wy + (52 + i * 30) * s;
		if (i == 1)
			rect(wx, y - 6 * s, 3 * s, 28 * s, ACCENT);
		textbar(wx + 20 * s, y, (90 + (i * 37) % 60) * s, s, i == 1 ? TEXT : DIM);
	}
	for (int i = 0; i < 7; i++) {
		int y = wy + (58 + i * 34) * s;
		rect(wx + 214 * s, y, 22 * s, 22 * s, i % 3 ? LINE : ACCENT);
		textbar(wx + 250 * s, y + 2 * s, (180 + (i * 53) % 140) * s, s, TEXT);
		textbar(wx + ww - 120 * s, y + 2 * s, 70 * s, s, DIM);
	}

	/* the panel at the bottom */
	int ph = 44 * s, py = Hh - ph;
	rect(0, py, W, ph, PANEL);
	rect(0, py, W, 1, LINE);
	rect(0, py, 52 * s, ph, ACCENT);               /* start button */
	rect(18 * s, py + 14 * s, 16 * s, 16 * s, PANEL);
	/* window list: four entries, the highlight walks along them */
	int ex = 64 * s, ew = 170 * s, active = (int)(t * 1.5) % 4;
	for (int i = 0; i < 4; i++) {
		int x = ex + i * (ew + 6 * s);
		rect(x, py + 6 * s, ew, ph - 12 * s, i == active ? RAISED : PANEL);
		if (i == active)
			rect(x, py + ph - 9 * s, ew, 3 * s, ACCENT);
		rect(x + 10 * s, py + 14 * s, 16 * s, 16 * s, i == active ? TEXT : DIM);
		textbar(x + 36 * s, py + 13 * s, (80 + i * 17) * s, s, i == active ? TEXT : DIM);
	}
	/* tray and clock */
	for (int i = 0; i < 3; i++)
		rect(W - (150 + i * 28) * s, py + 15 * s, 14 * s, 14 * s, DIM);
	textbar(W - 92 * s, py + 13 * s, 76 * s, s, TEXT);
}

static void save_ppm(const char *path, int w, int h)
{
	unsigned char *px = malloc((size_t)w * h * 4);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
	FILE *f = fopen(path, "wb");
	if (!f)
		die("%s: %s", path, strerror(errno));
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int y = h - 1; y >= 0; y--)
		for (int x = 0; x < w; x++)
			fwrite(px + ((size_t)y * w + x) * 4, 1, 3, f);
	fclose(f);
	free(px);
}

/* ---- buffers and flips ------------------------------------------------------------------ */
static void fb_destroy(struct gbm_bo *bo, void *data)
{
	uint32_t id = (uint32_t)(uintptr_t)data;
	int fd = gbm_device_get_fd(gbm_bo_get_device(bo));
	drmModeRmFB(fd, id);
}

static uint32_t fb_for(int fd, struct gbm_bo *bo)
{
	void *data = gbm_bo_get_user_data(bo);
	if (data)
		return (uint32_t)(uintptr_t)data;
	uint32_t handles[4] = {gbm_bo_get_handle(bo).u32}, strides[4] = {gbm_bo_get_stride(bo)},
	         offsets[4] = {0}, id;
	if (drmModeAddFB2(fd, gbm_bo_get_width(bo), gbm_bo_get_height(bo), gbm_bo_get_format(bo),
	                  handles, strides, offsets, &id, 0))
		die("cannot add a framebuffer: %s", strerror(errno));
	gbm_bo_set_user_data(bo, (void *)(uintptr_t)id, fb_destroy);
	return id;
}

static bool flip_pending;

static void on_flip(int fd, unsigned seq, unsigned sec, unsigned usec, void *data)
{
	flip_pending = false;
}

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	double seconds = 5;
	const char *shot = NULL;
	int opt;
	while ((opt = getopt(argc, argv, "t:o:")) != -1) {
		if (opt == 't')
			seconds = atof(optarg);
		else if (opt == 'o')
			shot = optarg;
		else {
			fprintf(stderr, "usage: fl-drawtest [-t SECONDS] [-o FILE.ppm]\n");
			return 2;
		}
	}

	struct libseat *seat = libseat_open_seat(&seat_listener, NULL);
	if (!seat)
		die("cannot open the seat (is seatd running, and are you in its group?)");
	while (!seat_active)
		if (libseat_dispatch(seat, -1) < 0)
			die("seat: %s", strerror(errno));
	printf("seat: %s\n", libseat_seat_name(seat));

	struct output o = {0};
	int devid;
	if (!find_output(seat, &o, &devid))
		die("no connected display");

	struct gbm_device *gbm = gbm_create_device(o.fd);
	int W = o.mode.hdisplay, Hh = o.mode.vdisplay;
	struct gbm_surface *gs = gbm_surface_create(gbm, W, Hh, GBM_FORMAT_XRGB8888,
	                                            GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	if (!gbm || !gs)
		die("cannot set up GBM");

	PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
	    (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay dpy = get_display(EGL_PLATFORM_GBM_KHR, gbm, NULL);
	EGLint major, minor;
	if (!eglInitialize(dpy, &major, &minor))
		die("cannot initialise EGL");
	eglBindAPI(EGL_OPENGL_ES_API);
	EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
	                  EGL_BLUE_SIZE, 8, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_NONE};
	EGLConfig cfgs[64], cfg = NULL;
	EGLint ncfg;
	eglChooseConfig(dpy, attrs, cfgs, 64, &ncfg);
	for (int i = 0; i < ncfg && !cfg; i++) { /* the one matching the GBM format */
		EGLint id;
		eglGetConfigAttrib(dpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &id);
		if ((uint32_t)id == GBM_FORMAT_XRGB8888)
			cfg = cfgs[i];
	}
	if (!cfg)
		die("no EGL config for XRGB8888");
	EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
	EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
	EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)gs, NULL);
	if (ctx == EGL_NO_CONTEXT || surf == EGL_NO_SURFACE || !eglMakeCurrent(dpy, surf, surf, ctx))
		die("cannot create the EGL context");
	printf("EGL %d.%d, GL renderer: %s\n", major, minor, glGetString(GL_RENDERER));

	drmEventContext ev = {.version = 2, .page_flip_handler = on_flip};
	struct gbm_bo *shown = NULL;
	double start = now(), t = 0;
	int frames = 0;
	while (t < seconds) {
		t = now() - start;
		glViewport(0, 0, W, Hh);
		draw(W, Hh, t);
		if (shot && t + 0.05 >= seconds)
			save_ppm(shot, W, Hh);
		eglSwapBuffers(dpy, surf);
		struct gbm_bo *bo = gbm_surface_lock_front_buffer(gs);
		uint32_t fb = fb_for(o.fd, bo);
		if (!shown) {
			if (drmModeSetCrtc(o.fd, o.crtc, fb, 0, 0, &o.conn, 1, &o.mode))
				die("cannot set the mode: %s", strerror(errno));
		} else {
			flip_pending = true;
			if (drmModePageFlip(o.fd, o.crtc, fb, DRM_MODE_PAGE_FLIP_EVENT, NULL))
				die("page flip failed: %s", strerror(errno));
			while (flip_pending) {
				struct pollfd p[2] = {{.fd = o.fd, .events = POLLIN},
				                      {.fd = libseat_get_fd(seat), .events = POLLIN}};
				poll(p, 2, -1);
				if (p[0].revents)
					drmHandleEvent(o.fd, &ev);
				if (p[1].revents)
					libseat_dispatch(seat, 0);
			}
			gbm_surface_release_buffer(gs, shown);
		}
		shown = bo;
		frames++;
	}
	printf("%d frames in %.1f s: %.0f fps\n", frames, t, frames / t);

	if (o.saved) /* give the console its screen back */
		drmModeSetCrtc(o.fd, o.saved->crtc_id, o.saved->buffer_id, o.saved->x, o.saved->y,
		               &o.conn, 1, &o.saved->mode);
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglTerminate(dpy);
	gbm_surface_destroy(gs);
	gbm_device_destroy(gbm);
	libseat_close_device(seat, devid);
	libseat_close_seat(seat);
	return 0;
}
