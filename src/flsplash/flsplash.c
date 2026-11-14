/* flsplash -- the FreeLFS boot screen. Runs as root from rc.sysinit.
 * SPDX-License-Identifier: MIT
 *
 * A glossy 3D logo (docs/logo.svg: three bars tilted 6 degrees, as beveled
 * slabs) on a dark floor with a soft glow, a mirror image and a light sweep,
 * then "FreeLFS" under it. The layers are rendered at build time by genart.c
 * (flsplash.dat, deflated, built into this binary, a few sizes); here only the
 * logo region is composited, in premultiplied integer math, by the clock
 * (CLOCK_MONOTONIC from the first picture, about 4 s). Straight into KMS dumb
 * buffers, flipped on the vertical blank when the driver can (two buffers),
 * otherwise only the region is copied; or /dev/fb0 when there is no DRM
 * device. No compositor.
 *
 *   flsplash             start (forks into the background; exits 0 whatever happens)
 *   flsplash --handover  wait for the animation to end, then let go of the
 *                        screen (the login screen takes it over; the last
 *                        picture stays up until the new owner has drawn)
 *   flsplash --text      switch to the boot text at once (also when flsplash
 *                        is gone: the console is put back to text mode)
 *   flsplash --render MS W H FILE.ppm
 *                        the picture at MS ms on a W x H screen, no screen needed
 *
 * When the DRM device goes away (simpledrm handing over to the real driver)
 * the card is opened again and drawing goes on at the right point. Esc shows
 * the boot text; so does SIGUSR2, which a failing boot step sends.
 * /run/flsplash.pid, .done (animation over), .released (handover done), .log.
 * Kernel option nosplash: do nothing.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <math.h>
#include <poll.h>
#include <setjmp.h>
#include <stdarg.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <zlib.h>

#include <drm/drm.h>
#include <drm/drm_mode.h>

#define PIDF "/run/flsplash.pid"
#define DONEF "/run/flsplash.done"
#define RELF "/run/flsplash.released"
#define LOGF "/run/flsplash.log"

/* the art: flsplash.dat made by genart, built in */
__asm__(".section .rodata\n.balign 16\n.global flsplash_art\nflsplash_art:\n"
        ".incbin \"" ARTFILE "\"\n.global flsplash_art_end\nflsplash_art_end:\n.byte 0\n.previous\n");
extern const uint8_t flsplash_art[], flsplash_art_end[];

/* the clock, in ms from the first picture */
#define T_GLOW 1800     /* glow swells */
#define T_LOGO0 150     /* logo fades in */
#define T_LOGO 1300
#define T_REFL0 500     /* its reflection follows */
#define T_REFL 1400
#define T_SWEEP0 1500   /* the light band crosses */
#define T_SWEEP 1500
#define T_TEXT0 2100    /* the word fades in */
#define T_TEXT 1300
#define T_END 3800
#define T_MIN 3000       /* the splash holds at least this long from the first picture (the animation is longer) */
#define FADE_US 300000   /* after the animation, or when the handover came: fade out in this time */
#define HANDOVER_WAIT_MS 6000 /* --handover waits for the fade and the animation's rest */
_Static_assert(T_END >= T_MIN, "the splash must hold at least T_MIN");
#define FRAME_US 16667
#define MIN_GAP_US 8000      /* least time between two copied frames */
#define MIN_FLIP_GAP_US 14000 /* between two flips: drivers without a vertical blank report them at once */
#define FLIP_TIMEOUT_US 250000
#define GIVE_UP_MS 4000     /* no screen by then: boot as text */
#define LINGER_MS 6000      /* after the handover: keep the last picture this long */
#define STUCK_MS 180000     /* nobody took over: show the text */
#define MAXCARD 4
#define MAXOUT 4
#define MAXSIZE 8

/* ---- the art ---------------------------------------------------------------------------- */

struct lay {
	int x, y, w, h;
	uint32_t *px;   /* premultiplied A R G B */
	int *x0, *x1;   /* per row: first and last pixel that is not empty, -1 if none */
};
struct scene {
	int S, Rw, Rh, yf;
	uint32_t *rowcol; /* Rh + 2: region rows, then screen top, screen bottom */
	uint32_t *base;   /* the room and the glow, dithered, Rw x Rh */
	uint32_t *frame;  /* the region as composed */
	struct lay logo, refl, text;
	int sx, sy, sw, sh;
	uint8_t *su, *sg;
	int *sx0, *sx1;
	uint16_t lut[256];
	int64_t frame_t;
	unsigned frame_f; /* the fade the frame was composed with */
};
static struct scene *scenes[MAXSIZE];
static int nsizes, art_S[MAXSIZE], art_Rw[MAXSIZE], art_Rh[MAXSIZE];
static uint32_t art_off[MAXSIZE];

static uint32_t rd32(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static int art_init(void)
{
	size_t n = (size_t)(flsplash_art_end - flsplash_art);
	if (n < 12 || memcmp(flsplash_art, "FLSPART1", 8))
		return -1;
	nsizes = (int)rd32(flsplash_art + 8);
	if (nsizes < 1 || nsizes > MAXSIZE || n < 12 + (size_t)nsizes * 16)
		return -1;
	for (int i = 0; i < nsizes; i++) {
		const uint8_t *e = flsplash_art + 12 + i * 16;
		art_S[i] = (int)rd32(e);
		art_Rw[i] = (int)rd32(e + 4);
		art_Rh[i] = (int)rd32(e + 8);
		art_off[i] = rd32(e + 12);
	}
	return 0;
}

/* inflate one layer; returns the planes (one after the other, rows undone) */
static uint8_t *get_layer(const uint8_t **pp, int *x, int *y, int *w, int *h, int *np)
{
	const uint8_t *p = *pp;
	*x = (int)rd32(p);
	*y = (int)rd32(p + 4);
	*w = (int)rd32(p + 8);
	*h = (int)rd32(p + 12);
	*np = (int)rd32(p + 16);
	uint32_t cs = rd32(p + 20);
	*pp = p + 24 + cs;
	size_t plane = (size_t)*w * (size_t)*h, n = plane * (size_t)*np;
	uint8_t *d = malloc(n ? n : 1);
	if (!d)
		return NULL;
	uLongf dl = (uLongf)n;
	if (n && uncompress(d, &dl, p + 24, cs) != Z_OK) {
		free(d);
		return NULL;
	}
	for (size_t r = 0; *w > 0 && r < (size_t)*h * (size_t)*np; r++) {
		uint8_t *row = d + r * (size_t)*w;
		for (int i = 1; i < *w; i++)
			row[i] = (uint8_t)(row[i] + row[i - 1]);
	}
	return d;
}

static int *spans(const uint32_t *px, int w, int h, int last)
{
	int *s = malloc((size_t)h * sizeof *s);
	if (!s)
		return NULL;
	for (int y = 0; y < h; y++) {
		int a = -1, b = -1;
		for (int x = 0; x < w; x++)
			if (px[(size_t)y * (size_t)w + (size_t)x] >> 24) {
				if (a < 0)
					a = x;
				b = x;
			}
		s[y] = last ? b : a;
	}
	return s;
}

static int lay_load(struct lay *l, const uint8_t **pp)
{
	int np, w, h;
	uint8_t *d = get_layer(pp, &l->x, &l->y, &w, &h, &np);
	if (!d || np != 4)
		return -1;
	l->w = w;
	l->h = h;
	size_t plane = (size_t)w * (size_t)h;
	l->px = malloc((plane ? plane : 1) * 4);
	if (!l->px)
		return -1;
	for (size_t i = 0; i < plane; i++)
		l->px[i] = (uint32_t)d[i] << 24 | (uint32_t)d[plane + i] << 16 | (uint32_t)d[2 * plane + i] << 8 | d[3 * plane + i];
	free(d);
	l->x0 = spans(l->px, w, h, 0);
	l->x1 = spans(l->px, w, h, 1);
	return l->x0 && l->x1 ? 0 : -1;
}

static int bayer8(int x, int y)
{
	int v = 0, xx = (x ^ y) & 7, yy = y & 7;
	for (int b = 0; b < 3; b++)
		v = v * 4 + ((((yy >> b) & 1) << 1) | ((xx >> b) & 1));
	return v; /* 0..63 */
}

static struct scene *load_scene(int idx)
{
	if (idx < 0 || idx >= nsizes)
		return NULL;
	if (scenes[idx])
		return scenes[idx];
	struct scene *s = calloc(1, sizeof *s);
	if (!s)
		return NULL;
	const uint8_t *p = flsplash_art + art_off[idx];
	int x, y, w, h, np;
	s->S = art_S[idx];
	s->Rw = art_Rw[idx];
	s->Rh = art_Rh[idx];
	uint8_t *d = get_layer(&p, &x, &y, &w, &h, &np);
	if (!d || w != 1 || h != s->Rh + 2 || np != 3)
		return NULL;
	s->rowcol = malloc((size_t)h * 4);
	for (int i = 0; i < h; i++)
		s->rowcol[i] = (uint32_t)d[i] << 16 | (uint32_t)d[h + i] << 8 | d[2 * h + i];
	free(d);
	/* the glow, half resolution: bilinear up, dithered, on the room */
	int gw, gh;
	uint8_t *g = get_layer(&p, &x, &y, &gw, &gh, &np);
	s->base = malloc((size_t)s->Rw * (size_t)s->Rh * 4);
	s->frame = malloc((size_t)s->Rw * (size_t)s->Rh * 4);
	uint16_t *row = malloc((size_t)gw * 3 * sizeof *row);
	if (!g || np != 3 || !s->base || !s->frame || !row)
		return NULL;
	size_t gp = (size_t)gw * (size_t)gh;
	for (int yy = 0; yy < s->Rh; yy++) {
		int fy = (yy * 256 + 128) / 2 - 128; /* 8.8, source row */
		int y0 = fy >> 8, fr = fy & 255, y1 = y0 + 1;
		y0 = y0 < 0 ? 0 : y0 >= gh ? gh - 1 : y0;
		y1 = y1 < 0 ? 0 : y1 >= gh ? gh - 1 : y1;
		for (int c = 0; c < 3; c++)
			for (int xx = 0; xx < gw; xx++)
				row[xx * 3 + c] = (uint16_t)(g[c * gp + (size_t)y0 * (size_t)gw + (size_t)xx] * (256 - fr) +
				                             g[c * gp + (size_t)y1 * (size_t)gw + (size_t)xx] * fr);
		uint32_t rc = s->rowcol[yy];
		for (int xx = 0; xx < s->Rw; xx++) {
			int fx = (xx * 256 + 128) / 2 - 128;
			int x0 = fx >> 8, fq = fx & 255, x1 = x0 + 1;
			x0 = x0 < 0 ? 0 : x0 >= gw ? gw - 1 : x0;
			x1 = x1 < 0 ? 0 : x1 >= gw ? gw - 1 : x1;
			unsigned dith = (unsigned)bayer8(xx, yy) * 4 + 2; /* 2..254 */
			uint32_t o = 0;
			for (int c = 0; c < 3; c++) {
				unsigned v = ((unsigned)row[x0 * 3 + c] * (256 - (unsigned)fq) + (unsigned)row[x1 * 3 + c] * (unsigned)fq) >> 8; /* 8.8 */
				unsigned gv = (v + dith) >> 8;
				unsigned sh = (unsigned)(16 - 8 * c);
				unsigned b = (rc >> sh & 255) + gv;
				o |= (b > 255 ? 255 : b) << sh;
			}
			s->base[(size_t)yy * (size_t)s->Rw + (size_t)xx] = o;
		}
	}
	free(row);
	free(g);
	if (lay_load(&s->logo, &p) < 0 || lay_load(&s->refl, &p) < 0)
		return NULL;
	s->yf = s->refl.y - 1;
	uint8_t *sw = get_layer(&p, &s->sx, &s->sy, &s->sw, &s->sh, &np);
	if (!sw || np != 2)
		return NULL;
	s->su = sw;
	s->sg = sw + (size_t)s->sw * (size_t)s->sh;
	s->sx0 = malloc((size_t)s->sh * sizeof(int));
	s->sx1 = malloc((size_t)s->sh * sizeof(int));
	for (int yy = 0; yy < s->sh; yy++) {
		int a = -1, b = -1;
		for (int xx = 0; xx < s->sw; xx++)
			if (s->sg[(size_t)yy * (size_t)s->sw + (size_t)xx]) {
				if (a < 0)
					a = xx;
				b = xx;
			}
		s->sx0[yy] = a;
		s->sx1[yy] = b;
	}
	if (lay_load(&s->text, &p) < 0)
		return NULL;
	for (int i = 0; i < 256; i++) { /* the band: a soft wide glow with a narrower streak in it */
		float dx = ((float)i - 128.0f) / 128.0f;
		float broad = fabsf(dx) < 1 ? cosf(dx * 1.5707963f) * cosf(dx * 1.5707963f) * 0.30f : 0;
		float narrow = expf(-dx * dx / (2 * 0.12f * 0.12f)) * 0.42f;
		s->lut[i] = (uint16_t)lroundf((broad + narrow) * 256.0f);
	}
	s->frame_t = -1;
	s->frame_f = ~0u;
	scenes[idx] = s;
	return s;
}

/* ---- compositing: premultiplied, integer, two channels at a time ------------------------ */

static inline uint32_t over(uint32_t s, uint32_t d, uint32_t k) /* s over d, s scaled by k/256 */
{
	uint32_t a = (s >> 24) * k >> 8, inv = 256 - a;
	uint32_t srb = ((s & 0xff00ff) * k >> 8) & 0xff00ff, sg = ((s >> 8) & 0xff) * k >> 8;
	uint32_t drb = ((d & 0xff00ff) * inv >> 8) & 0xff00ff, dg = ((d >> 8) & 0xff) * inv >> 8;
	return (srb + drb) | ((sg + dg) << 8);
}

static inline uint32_t screen_mint(uint32_t d, uint32_t k) /* toward a pale mint white by k/256 */
{
	uint32_t rb = d & 0xff00ff, g = (d >> 8) & 0xff, kb = k * 176 >> 8;
	rb += ((0xff00ff - rb) * kb >> 8) & 0xff00ff;
	g += (255 - g) * k >> 8;
	return rb | g << 8;
}

static inline uint32_t lerp_up(uint32_t a, uint32_t b, uint32_t k) /* a + (b - a) * k/256, b >= a per channel */
{
	uint32_t df = b - a;
	uint32_t rb = ((df & 0xff00ff) * k >> 8) & 0xff00ff, g = ((df >> 8) & 0xff) * k >> 8;
	return a + (rb | g << 8);
}

static float ease(float x)
{
	x = x < 0 ? 0 : x > 1 ? 1 : x;
	return x * x * (3 - 2 * x);
}

static unsigned k256(float v)
{
	return (unsigned)(v * 256.0f + 0.5f);
}

static void over_row(uint32_t *d, const uint32_t *s, int a, int b, uint32_t k)
{
	for (int x = a; x <= b; x++) {
		uint32_t p = s[x];
		if (p >> 24)
			d[x] = over(p, d[x], k);
	}
}

static unsigned g_fade; /* 0..256: how far the picture has faded to the background */

static inline uint32_t fade_to(uint32_t d, uint32_t c, unsigned k) /* d toward c by k/256, per channel */
{
	uint32_t out = d & 0xff000000u;
	for (int sh = 0; sh <= 16; sh += 8) {
		int a = (int)((d >> sh) & 255), b = (int)((c >> sh) & 255);
		out |= (uint32_t)(a + (b - a) * (int)k / 256) << sh;
	}
	return out;
}

static void compose(struct scene *s, int64_t t)
{
	if (s->frame_t == t && s->frame_f == g_fade)
		return;
	s->frame_t = t;
	s->frame_f = g_fade;
	unsigned ga = k256(ease((float)t / T_GLOW)), la = k256(ease((float)(t - T_LOGO0) / T_LOGO)),
	         ra = k256(ease((float)(t - T_REFL0) / T_REFL)), ta = k256(ease((float)(t - T_TEXT0) / T_TEXT));
	int sweep = t > T_SWEEP0 && t < T_SWEEP0 + T_SWEEP, cpos = 0;
	if (sweep) {
		float p = (float)(t - T_SWEEP0) / T_SWEEP;
		p = 0.5f - 0.5f * cosf(p * 3.14159265f); /* eases in and out */
		cpos = (int)(-150.0f + 545.0f * p);
	}
	for (int y = 0; y < s->Rh; y++) {
		uint32_t *d = s->frame + (size_t)y * (size_t)s->Rw;
		const uint32_t *b = s->base + (size_t)y * (size_t)s->Rw;
		if (ga >= 256) {
			memcpy(d, b, (size_t)s->Rw * 4);
		} else {
			uint32_t rc = s->rowcol[y];
			for (int x = 0; x < s->Rw; x++)
				d[x] = lerp_up(rc, b[x], ga);
		}
		int ry = y - s->refl.y;
		if (ra && ry >= 0 && ry < s->refl.h && s->refl.x0[ry] >= 0)
			over_row(d + s->refl.x, s->refl.px + (size_t)ry * (size_t)s->refl.w, s->refl.x0[ry], s->refl.x1[ry], ra);
		int ly = y - s->logo.y;
		if (la && ly >= 0 && ly < s->logo.h && s->logo.x0[ly] >= 0)
			over_row(d + s->logo.x, s->logo.px + (size_t)ly * (size_t)s->logo.w, s->logo.x0[ly], s->logo.x1[ly], la);
		int sy = y - s->sy;
		if (sweep && sy >= 0 && sy < s->sh && s->sx0[sy] >= 0) {
			unsigned sc = y > s->yf ? ra : la;
			const uint8_t *u = s->su + (size_t)sy * (size_t)s->sw, *g = s->sg + (size_t)sy * (size_t)s->sw;
			for (int x = s->sx0[sy]; x <= s->sx1[sy]; x++) {
				int dd = u[x] - cpos + 128;
				if (!g[x] || (unsigned)dd >= 256)
					continue;
				uint32_t k = (g[x] * (uint32_t)s->lut[dd]) >> 8;
				k = k * sc >> 8;
				if (k)
					d[s->sx + x] = screen_mint(d[s->sx + x], k);
			}
		}
		int ty = y - s->text.y;
		if (ta && ty >= 0 && ty < s->text.h && s->text.x0[ty] >= 0)
			over_row(d + s->text.x, s->text.px + (size_t)ty * (size_t)s->text.w, s->text.x0[ty], s->text.x1[ty], ta);
		if (g_fade)
			for (int x = 0; x < s->Rw; x++)
				d[x] = fade_to(d[x], s->rowcol[y], g_fade);
	}
}

/* ---- screens ---------------------------------------------------------------------------- */

struct out {
	uint32_t w, h, pitch, crtc, conn;
	size_t size;
	uint32_t fb[2], handle[2];
	uint8_t *map[2];
	int nbuf, front, flipto, flip_ok, pending, final_sent;
	int64_t pend_us, next_us;
	struct drm_mode_modeinfo mode;
	int rs, gs, bs; /* channel shifts of a 32 bit pixel */
	struct scene *sc;
	int rx, ry;     /* where the region sits */
};
struct card {
	int fd, fbdev, nout, nopoll;
	char path[300];
	struct out o[MAXOUT];
};

static struct card cards[MAXCARD];
static int ncards;
static int ttyfd = -1, ifd = -1, wd_dev = -1, wd_dri = -1;
static struct termios tty_saved;
static int tty_raw, kd_graphics;
static int64_t t0_us = -1; /* monotonic us of the first picture */
static int anim_done, released, card_gone, want_release;
static int64_t fade_us = -1; /* when the fade-out started, -1 none */
static int64_t st_frames, st_us, st_gap, st_last; /* what the frames cost: for the log */
static int64_t start_us, rescan_at = -1, retry_until = -1, release_us, done_us;

static int64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
	va_list ap;
	char b[160];
	int n = snprintf(b, sizeof b, "%6lld ms ", (long long)((now_us() - start_us) / 1000));
	va_start(ap, fmt);
	n += vsnprintf(b + n, sizeof b - (size_t)n - 1, fmt, ap);
	va_end(ap);
	b[n++] = '\n';
	if (write(2, b, (size_t)n) < 0) {
		/* nothing to do about it */
	}
}

static int64_t clock_t_now(void)
{
	int64_t t = t0_us < 0 ? 0 : (now_us() - t0_us) / 1000;
	return t > T_END ? T_END : t;
}

static uint32_t pack(const struct out *o, uint32_t rgb)
{
	return ((rgb >> 16 & 255) << o->rs) | ((rgb >> 8 & 255) << o->gs) | ((rgb & 255) << o->bs);
}

static uint32_t mixrgb(uint32_t a, uint32_t b, float t)
{
	uint32_t r = 0;
	for (int s = 0; s <= 16; s += 8) {
		float ca = (float)((a >> s) & 255), cb = (float)((b >> s) & 255);
		r |= (uint32_t)(ca + (cb - ca) * t + 0.5f) << s;
	}
	return r;
}

/* the colour of a row outside (and inside) the region: the room is the same all the way across */
static uint32_t row_color(const struct out *o, int y)
{
	const struct scene *s = o->sc;
	int r = y - o->ry;
	if (r >= 0 && r < s->Rh)
		return s->rowcol[r];
	if (r < 0)
		return mixrgb(s->rowcol[s->Rh], s->rowcol[0], ease((float)y / (float)(o->ry ? o->ry : 1)));
	int below = (int)o->h - (o->ry + s->Rh);
	return mixrgb(s->rowcol[s->Rh - 1], s->rowcol[s->Rh + 1], ease((float)(r - s->Rh) / (float)(below ? below : 1)));
}

static void bgfill(struct out *o, uint8_t *map)
{
	for (uint32_t y = 0; y < o->h; y++) {
		uint32_t *row = (uint32_t *)(map + (size_t)y * o->pitch), c = pack(o, row_color(o, (int)y));
		for (uint32_t x = 0; x < o->w; x++)
			row[x] = c;
	}
}

static void blit(struct out *o, uint8_t *map)
{
	struct scene *s = o->sc;
	int native = o->rs == 16 && o->gs == 8 && o->bs == 0;
	for (int y = 0; y < s->Rh; y++) {
		int yy = o->ry + y;
		if (yy < 0 || yy >= (int)o->h)
			continue;
		uint32_t *dst = (uint32_t *)(map + (size_t)yy * o->pitch) + o->rx;
		const uint32_t *src = s->frame + (size_t)y * (size_t)s->Rw;
		if (native) {
			memcpy(dst, src, (size_t)s->Rw * 4);
		} else {
			for (int x = 0; x < s->Rw; x++)
				dst[x] = pack(o, src[x]);
		}
	}
}

static void dirty(struct card *c, struct out *o, int full)
{
	if (c->fbdev || c->fd < 0 || !o->fb[0])
		return;
	struct drm_clip_rect cl = {0, 0, (unsigned short)o->w, (unsigned short)o->h};
	if (!full)
		cl = (struct drm_clip_rect){(unsigned short)o->rx, (unsigned short)o->ry, (unsigned short)(o->rx + o->sc->Rw),
		                            (unsigned short)(o->ry + o->sc->Rh)};
	struct drm_mode_fb_dirty_cmd d = {.fb_id = o->fb[o->front], .num_clips = 1, .clips_ptr = (uintptr_t)&cl};
	ioctl(c->fd, DRM_IOCTL_MODE_DIRTYFB, &d); /* only drivers with a shadow buffer need it */
}

/* everything: room and region, into every buffer, no flipping */
static void present_full(struct card *c, struct out *o, int64_t t)
{
	compose(o->sc, t);
	for (int i = 0; i < o->nbuf; i++) {
		bgfill(o, o->map[i]);
		blit(o, o->map[i]);
	}
	dirty(c, o, 1);
}

/* one frame of the region: flipped on the vertical blank if the driver can, else copied */
static void present(struct card *c, struct out *o, int64_t t, int idx)
{
	compose(o->sc, t);
	int back = o->nbuf > 1 && o->flip_ok ? 1 - o->front : o->front;
	blit(o, o->map[back]);
	if (back != o->front) {
		struct drm_mode_crtc_page_flip pf = {.crtc_id = o->crtc, .fb_id = o->fb[back],
		                                     .flags = DRM_MODE_PAGE_FLIP_EVENT, .user_data = (uint64_t)idx + 1};
		if (ioctl(c->fd, DRM_IOCTL_MODE_PAGE_FLIP, &pf) == 0) {
			o->pending = 1;
			o->flipto = back;
			o->pend_us = now_us();
			return;
		}
		if (errno == ENODEV) {
			card_gone = 1;
			return;
		}
		o->flip_ok = 0; /* no flips here: copy the region into what is on screen */
		blit(o, o->map[o->front]);
	}
	dirty(c, o, 0);
}

static void read_events(struct card *c)
{
	char b[1024];
	ssize_t n = read(c->fd, b, sizeof b);
	for (ssize_t p = 0; n > 0 && p + (ssize_t)sizeof(struct drm_event) <= n;) {
		struct drm_event *e = (struct drm_event *)(b + p);
		if (e->length < sizeof *e)
			break;
		if (e->type == DRM_EVENT_FLIP_COMPLETE && p + (ssize_t)sizeof(struct drm_event_vblank) <= n) {
			struct drm_event_vblank *v = (struct drm_event_vblank *)e;
			int i = (int)v->user_data - 1;
			if (i >= 0 && i < c->nout && c->o[i].pending) {
				c->o[i].pending = 0;
				c->o[i].front = c->o[i].flipto;
			}
		}
		p += e->length;
	}
}

/* a card that vanishes under us (simpledrm handing over) turns its mappings
 * into SIGBUS: drawing is guarded */
static sigjmp_buf jb;
static volatile sig_atomic_t in_draw;
static void on_bus(int sig)
{
	if (in_draw)
		siglongjmp(jb, 1);
	signal(SIGBUS, SIG_DFL);
	raise(SIGBUS);
}

static int draw_card_full(struct card *c, int64_t t)
{
	if (sigsetjmp(jb, 1)) {
		in_draw = 0;
		return -1;
	}
	in_draw = 1;
	for (int i = 0; i < c->nout; i++) {
		struct out *o = &c->o[i];
		present_full(c, o, t);
		o->pending = 0;
		o->final_sent = t >= T_END && fade_us < 0;
		o->next_us = 0;
	}
	in_draw = 0;
	return 0;
}

/* the due frames of one card; returns -1 when the card is gone, else sets *next to when it wants to be called */
static int draw_card(struct card *c, int64_t t, int64_t nowu, int64_t *next)
{
	if (sigsetjmp(jb, 1)) {
		in_draw = 0;
		return -1;
	}
	in_draw = 1;
	for (int i = 0; i < c->nout; i++) {
		struct out *o = &c->o[i];
		if (o->pending) {
			int64_t to = o->pend_us + FLIP_TIMEOUT_US;
			if (nowu < to) {
				if (to < *next)
					*next = to;
				continue;
			}
			o->pending = 0; /* the event never came: take the flip as done, copy from now on */
			o->front = o->flipto;
			o->flip_ok = 0;
		}
		if (o->final_sent)
			continue;
		if (nowu < o->next_us) {
			if (o->next_us < *next)
				*next = o->next_us;
			continue;
		}
		int64_t p0 = now_us();
		present(c, o, t, i);
		st_frames++;
		st_us += now_us() - p0;
		if (st_last && p0 - st_last > st_gap)
			st_gap = p0 - st_last;
		st_last = p0;
		if (t >= T_END && fade_us < 0)
			o->final_sent = 1;
		if (o->pending) {
			o->next_us = nowu + MIN_FLIP_GAP_US;
			if (o->pend_us + FLIP_TIMEOUT_US < *next)
				*next = o->pend_us + FLIP_TIMEOUT_US;
		} else {
			o->next_us += FRAME_US; /* dropped frames are not made up: the next one shows the clock's time */
			if (o->next_us < nowu + MIN_GAP_US)
				o->next_us = nowu + MIN_GAP_US;
			if (!o->final_sent && o->next_us < *next)
				*next = o->next_us;
		}
	}
	in_draw = 0;
	return 0;
}

static int all_final(void)
{
	for (int i = 0; i < ncards; i++)
		for (int j = 0; j < cards[i].nout; j++)
			if (!cards[i].o[j].final_sent || cards[i].o[j].pending)
				return 0;
	return 1;
}

static void close_card(struct card *c)
{
	for (int i = 0; i < c->nout; i++)
		for (int b = 0; b < 2; b++)
			if (c->o[i].map[b])
				munmap(c->o[i].map[b], c->o[i].size);
	if (c->fd >= 0)
		close(c->fd);
	memset(c, 0, sizeof *c);
	c->fd = -1;
}

static void drop_card(int i)
{
	close_card(&cards[i]);
	cards[i] = cards[--ncards];
}

/* the biggest size that sits well on the screen (about 40% of its height at most) */
static int pick_size(int w, int h)
{
	int best = -1;
	for (int i = 0; i < nsizes; i++)
		if (art_Rw[i] <= w && art_Rh[i] <= h && (best < 0 || art_S[i] * 100 <= h * 40))
			best = i;
	return best;
}

static int layout(struct out *o)
{
	o->sc = load_scene(pick_size((int)o->w, (int)o->h));
	if (!o->sc)
		return -1;
	o->rx = ((int)o->w - o->sc->Rw) / 2;
	o->ry = (int)o->h / 2 - (int)o->h / 40 - o->sc->S * 139 / 100;
	if (o->ry + o->sc->Rh > (int)o->h)
		o->ry = (int)o->h - o->sc->Rh;
	if (o->ry < 0)
		o->ry = 0;
	return 0;
}

static int map_buf(struct card *c, struct out *o, int b)
{
	struct drm_mode_create_dumb cd = {.width = o->w, .height = o->h, .bpp = 32};
	if (ioctl(c->fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd) < 0)
		return -1;
	o->handle[b] = cd.handle;
	o->pitch = cd.pitch;
	o->size = cd.size;
	struct drm_mode_fb_cmd2 f2 = {.width = o->w, .height = o->h,
	                              .pixel_format = 'X' | 'R' << 8 | '2' << 16 | (uint32_t)'4' << 24,
	                              .handles = {o->handle[b]}, .pitches = {o->pitch}};
	if (ioctl(c->fd, DRM_IOCTL_MODE_ADDFB2, &f2) < 0) {
		struct drm_mode_fb_cmd f1 = {.width = o->w, .height = o->h, .pitch = o->pitch, .bpp = 32,
		                             .depth = 24, .handle = o->handle[b]};
		if (ioctl(c->fd, DRM_IOCTL_MODE_ADDFB, &f1) < 0)
			return -1;
		o->fb[b] = f1.fb_id;
	} else {
		o->fb[b] = f2.fb_id;
	}
	struct drm_mode_map_dumb md = {.handle = o->handle[b]};
	if (ioctl(c->fd, DRM_IOCTL_MODE_MAP_DUMB, &md) < 0)
		return -1;
	o->map[b] = mmap(NULL, o->size, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, (off_t)md.offset);
	if (o->map[b] == MAP_FAILED) {
		o->map[b] = NULL;
		return -1;
	}
	return 0;
}

static int drm_setup(struct card *c)
{
	uint32_t crtcs[32] = {0}, conns[32] = {0};
	struct drm_mode_card_res res = {0};
	if (ioctl(c->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
		return -1;
	if (res.count_crtcs > 32 || res.count_connectors > 32 || !res.count_crtcs)
		return -1;
	res.count_encoders = 0;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	res.connector_id_ptr = (uintptr_t)conns;
	if (ioctl(c->fd, DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
		return -1;
	uint32_t used = 0; /* bit per crtc index */
	for (uint32_t ci = 0; ci < res.count_connectors && c->nout < MAXOUT; ci++) {
		struct drm_mode_get_connector gc = {.connector_id = conns[ci]};
		if (ioctl(c->fd, DRM_IOCTL_MODE_GETCONNECTOR, &gc) < 0 || gc.connection != 1 || !gc.count_modes)
			continue;
		uint32_t nm = gc.count_modes, ne = gc.count_encoders > 32 ? 32 : gc.count_encoders, encs[32] = {0};
		struct drm_mode_modeinfo *modes = calloc(nm, sizeof *modes);
		if (!modes)
			continue;
		struct drm_mode_get_connector g2 = {.connector_id = conns[ci], .count_modes = nm,
		                                    .modes_ptr = (uintptr_t)modes, .count_encoders = ne,
		                                    .encoders_ptr = (uintptr_t)encs};
		if (ioctl(c->fd, DRM_IOCTL_MODE_GETCONNECTOR, &g2) < 0 || g2.count_modes > nm || !g2.count_modes) {
			free(modes);
			continue;
		}
		struct drm_mode_modeinfo mode = modes[0];
		for (uint32_t i = 0; i < g2.count_modes; i++)
			if (modes[i].type & DRM_MODE_TYPE_PREFERRED) {
				mode = modes[i];
				break;
			}
		free(modes);
		int pick = -1;
		struct drm_mode_get_encoder ge = {.encoder_id = g2.encoder_id};
		if (g2.encoder_id && ioctl(c->fd, DRM_IOCTL_MODE_GETENCODER, &ge) == 0)
			for (uint32_t i = 0; i < res.count_crtcs; i++)
				if (crtcs[i] == ge.crtc_id && !(used >> i & 1))
					pick = (int)i;
		for (uint32_t e = 0; pick < 0 && e < g2.count_encoders && e < ne; e++) {
			struct drm_mode_get_encoder g3 = {.encoder_id = encs[e]};
			if (ioctl(c->fd, DRM_IOCTL_MODE_GETENCODER, &g3) < 0)
				continue;
			for (uint32_t i = 0; i < res.count_crtcs; i++)
				if ((g3.possible_crtcs >> i & 1) && !(used >> i & 1)) {
					pick = (int)i;
					break;
				}
		}
		if (pick < 0)
			continue;
		struct out *o = &c->o[c->nout];
		memset(o, 0, sizeof *o);
		o->w = mode.hdisplay;
		o->h = mode.vdisplay;
		o->mode = mode;
		o->crtc = crtcs[pick];
		o->conn = conns[ci];
		o->rs = 16;
		o->gs = 8;
		if (layout(o) < 0 || map_buf(c, o, 0) < 0)
			continue;
		o->nbuf = 1;
		if (map_buf(c, o, 1) == 0) {
			o->nbuf = 2;
			o->flip_ok = 1;
		}
		c->nout++; /* counted now: present_full and draw_card use it */
		present_full(c, o, clock_t_now());
		struct drm_mode_crtc sc = {.crtc_id = o->crtc, .fb_id = o->fb[0], .set_connectors_ptr = (uintptr_t)&o->conn,
		                           .count_connectors = 1, .mode_valid = 1, .mode = o->mode};
		if (ioctl(c->fd, DRM_IOCTL_MODE_SETCRTC, &sc) < 0) {
			for (int b = 0; b < 2; b++)
				if (o->map[b])
					munmap(o->map[b], o->size);
			c->nout--;
			continue;
		}
		used |= 1u << pick;
		logf_("%s: %ux%u on connector %u, logo %d, %s", c->path, o->w, o->h, o->conn, o->sc->S,
		      o->nbuf > 1 ? "flipping" : "copying");
	}
	return c->nout ? 0 : -1;
}

static int fb_setup(struct card *c)
{
	struct fb_var_screeninfo v;
	struct fb_fix_screeninfo f;
	if (ioctl(c->fd, FBIOGET_VSCREENINFO, &v) < 0 || ioctl(c->fd, FBIOGET_FSCREENINFO, &f) < 0 ||
	    v.bits_per_pixel != 32)
		return -1;
	struct out *o = &c->o[0];
	memset(o, 0, sizeof *o);
	o->w = v.xres;
	o->h = v.yres;
	o->pitch = f.line_length;
	o->size = f.smem_len;
	o->rs = (int)v.red.offset;
	o->gs = (int)v.green.offset;
	o->bs = (int)v.blue.offset;
	o->nbuf = 1;
	if (layout(o) < 0)
		return -1;
	o->map[0] = mmap(NULL, o->size, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, 0);
	if (o->map[0] == MAP_FAILED) {
		o->map[0] = NULL;
		return -1;
	}
	c->nout = 1;
	logf_("%s: %ux%u (fbdev), logo %d", c->path, o->w, o->h, o->sc->S);
	return 0;
}

static void set_graphics(void)
{
	if (ttyfd < 0 || kd_graphics)
		return;
	if (ioctl(ttyfd, KDSETMODE, KD_GRAPHICS) == 0)
		kd_graphics = 1;
}

static int have_card(const char *path)
{
	for (int i = 0; i < ncards; i++)
		if (!strcmp(cards[i].path, path))
			return 1;
	return 0;
}

static int card_alive(struct card *c)
{
	if (c->fbdev) {
		struct stat st;
		return stat(c->path, &st) == 0;
	}
	struct drm_mode_card_res r = {0};
	return !(ioctl(c->fd, DRM_IOCTL_MODE_GETRESOURCES, &r) < 0 && errno == ENODEV);
}

static void watch(void)
{
	if (ifd < 0)
		return;
	if (wd_dev < 0)
		wd_dev = inotify_add_watch(ifd, "/dev", IN_CREATE | IN_DELETE | IN_MOVED_TO);
	wd_dri = inotify_add_watch(ifd, "/dev/dri", IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_ATTRIB);
}

/* Look at the devices again: drop what vanished, open what is new. Returns
 * whether something should be tried again soon. */
static int rescan(void)
{
	int again = 0, added = 0;
	for (int i = ncards - 1; i >= 0; i--)
		if (!card_alive(&cards[i])) {
			logf_("%s is gone", cards[i].path);
			drop_card(i);
		}
	watch();
	DIR *d = opendir("/dev/dri");
	struct dirent *de;
	int drm_nodes = 0;
	while (d && (de = readdir(d))) {
		if (strncmp(de->d_name, "card", 4) || ncards >= MAXCARD)
			continue;
		char path[300];
		snprintf(path, sizeof path, "/dev/dri/%s", de->d_name);
		drm_nodes++;
		if (have_card(path))
			continue;
		struct card *c = &cards[ncards];
		memset(c, 0, sizeof *c);
		snprintf(c->path, sizeof c->path, "%s", path);
		c->fd = open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
		if (c->fd < 0) {
			again = 1;
			continue;
		}
		ioctl(c->fd, DRM_IOCTL_SET_MASTER, 0); /* the first opener is master anyway */
		int rc = drm_setup(c);
		if (rc < 0) {
			close_card(c);
			again = 1; /* connectors may need a moment */
			continue;
		}
		ncards++;
		added++;
	}
	if (d)
		closedir(d);
	if (!ncards && !drm_nodes && access("/dev/fb0", R_OK | W_OK) == 0 && !have_card("/dev/fb0")) {
		struct card *c = &cards[0];
		memset(c, 0, sizeof *c);
		snprintf(c->path, sizeof c->path, "/dev/fb0");
		c->fbdev = 1;
		c->fd = open(c->path, O_RDWR | O_CLOEXEC);
		if (c->fd >= 0 && fb_setup(c) == 0) {
			ncards = 1;
			added++;
		} else {
			close_card(c);
		}
	}
	if (added) {
		if (t0_us < 0)
			t0_us = now_us();
		set_graphics();
		int64_t t = clock_t_now();
		for (int i = ncards - 1; i >= 0; i--)
			if (draw_card_full(&cards[i], t) < 0)
				drop_card(i);
	}
	return again;
}

/* ---- the terminal ----------------------------------------------------------------------- */

static void tty_open(void)
{
	ttyfd = open("/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (ttyfd < 0)
		return;
	if (tcgetattr(ttyfd, &tty_saved) == 0) {
		struct termios t = tty_saved;
		cfmakeraw(&t);
		t.c_cc[VMIN] = 0;
		t.c_cc[VTIME] = 0;
		if (tcsetattr(ttyfd, TCSANOW, &t) == 0)
			tty_raw = 1;
	}
}

static void tty_release(int to_text)
{
	if (ttyfd < 0)
		return;
	if (to_text && kd_graphics)
		ioctl(ttyfd, KDSETMODE, KD_TEXT);
	if (tty_raw) {
		tcflush(ttyfd, TCIFLUSH);
		tcsetattr(ttyfd, TCSANOW, &tty_saved);
	}
	close(ttyfd);
	ttyfd = -1;
}

/* console text on again, whoever switched it off */
static void kd_text_now(void)
{
	int fd = open("/dev/tty0", O_RDWR | O_NOCTTY | O_CLOEXEC);
	if (fd < 0)
		return;
	ioctl(fd, KDSETMODE, KD_TEXT);
	close(fd);
}

static void cleanup_files(void)
{
	unlink(PIDF);
	unlink(DONEF);
	unlink(RELF);
}

static void finish_text(void)
{
	logf_("text");
	tty_release(1);
	for (int i = ncards - 1; i >= 0; i--)
		close_card(&cards[i]); /* the last close gives the screen back to the console */
	cleanup_files();
	exit(0);
}

static void touch(const char *p)
{
	int fd = open(p, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
	if (fd >= 0)
		close(fd);
}

/* let go of the screen but keep the last picture in the scanout */
static void do_release(void)
{
	if (released)
		return;
	released = 1;
	release_us = now_us();
	for (int i = 0; i < ncards; i++)
		if (!cards[i].fbdev)
			ioctl(cards[i].fd, DRM_IOCTL_DROP_MASTER, 0);
	tty_release(0);
	if (ifd >= 0)
		close(ifd);
	ifd = -1;
	touch(RELF);
	logf_("released");
}

/* the fade-out: the picture goes to the background colour over FADE_US, then the screen is let go */
__attribute__((unused)) static void fade_start(int64_t now)
{
	fade_us = now;
	g_fade = 0;
	for (int i = 0; i < ncards; i++)
		for (int j = 0; j < cards[i].nout; j++) {
			cards[i].o[j].final_sent = 0;
			cards[i].o[j].next_us = 0;
		}
}

static void fade_finish(void)
{
	g_fade = 256;
	int64_t t = clock_t_now();
	for (int i = ncards - 1; i >= 0; i--)
		for (int j = 0; j < cards[i].nout; j++) {
			cards[i].o[j].next_us = 0;
			cards[i].o[j].pending = 0;
		}
	for (int i = ncards - 1; i >= 0; i--) {
		int64_t next = INT64_MAX;
		draw_card(&cards[i], t, now_us(), &next); /* the last picture, fully faded */
	}
	do_release();
	logf_("faded out, screen released");
}

static void daemon_main(int ready)
{
	start_us = now_us();
	logf_("start");
	if (art_init() < 0) {
		logf_("no art");
		cleanup_files();
		close(ready);
		exit(0);
	}
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	sigaddset(&set, SIGUSR2);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGHUP);
	sigprocmask(SIG_BLOCK, &set, NULL);
	int sfd = signalfd(-1, &set, SFD_CLOEXEC);
	struct sigaction sa = {.sa_handler = on_bus, .sa_flags = SA_NODEFER};
	sigaction(SIGBUS, &sa, NULL);
	ifd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
	tty_open();
	int again = rescan();
	if (write(ready, "x", 1) < 0) {
		/* nothing to do about it */
	}
	close(ready);
	if (again)
		retry_until = now_us() + 8000000;
	for (;;) {
		int64_t now = now_us(), wake = INT64_MAX;
#define WAKE(at) do { if ((at) < wake) wake = (at); } while (0)
		if (!released && ncards && (!anim_done || fade_us >= 0)) {
			int64_t next = INT64_MAX;
			int lost = 0;
			int64_t t = clock_t_now();
			if (fade_us >= 0) {
				int64_t e = (now - fade_us) * 256 / FADE_US;
				g_fade = e >= 256 ? 256u : (unsigned)e;
			}
			for (int i = ncards - 1; i >= 0; i--)
				if (draw_card(&cards[i], t, now, &next) < 0) {
					logf_("%s vanished while drawing", cards[i].path);
					drop_card(i);
					lost = 1;
				}
			if ((lost || card_gone) && rescan_at < 0)
				rescan_at = now + 100000;
			card_gone = 0;
			if (fade_us >= 0) {
				WAKE(next);
			} else if (ncards && all_final()) {
				anim_done = 1;
				done_us = now;
				touch(DONEF);
				logf_("done: %lld frames, %lld us each, longest gap %lld ms", (long long)st_frames,
				      (long long)(st_us / (st_frames ? st_frames : 1)), (long long)(st_gap / 1000));
				if (want_release)
					do_release(); /* no fade: flgreet's first frames continue from the wordmark left on screen */
			} else {
				WAKE(next);
			}
		}
		if (fade_us >= 0 && !released && now >= fade_us + (int64_t)FADE_US) {
			fade_finish();
			continue;
		}
		if (fade_us >= 0 && !released)
			WAKE(fade_us + (int64_t)FADE_US);
		if (rescan_at >= 0)
			WAKE(rescan_at);
		if (!ncards && !released)
			WAKE(start_us + (int64_t)GIVE_UP_MS * 1000);
		if (released)
			WAKE(release_us + (int64_t)LINGER_MS * 1000);
		else if (anim_done)
			WAKE(done_us + (int64_t)STUCK_MS * 1000);
		struct pollfd pf[3 + MAXCARD] = {{sfd, POLLIN, 0}, {ifd, POLLIN, 0}, {ttyfd, POLLIN, 0}};
		for (int i = 0; i < MAXCARD; i++)
			pf[3 + i] = (struct pollfd){i < ncards && !cards[i].nopoll && !released ? cards[i].fd : -1, POLLIN, 0};
		int64_t wait = wake - now_us();
		struct timespec to = {wait / 1000000, (wait % 1000000) * 1000};
		if (wait < 0)
			to = (struct timespec){0, 0};
		ppoll(pf, 3 + MAXCARD, wake == INT64_MAX ? NULL : &to, NULL);
		now = now_us();
		for (int i = 0; i < ncards; i++) {
			short re = pf[3 + i].revents;
			if (re & POLLIN)
				read_events(&cards[i]);
			if (re & (POLLERR | POLLHUP | POLLNVAL)) {
				cards[i].nopoll = 1;
				if (rescan_at < 0)
					rescan_at = now + 100000;
			}
		}
		if (pf[0].revents & POLLIN) {
			struct signalfd_siginfo si;
			if (read(sfd, &si, sizeof si) == sizeof si) {
				if (si.ssi_signo == SIGUSR1) {
					if (anim_done || !ncards || t0_us < 0)
						do_release();
					else { /* the system is up: the animation runs to its end, then the screen is let go with the wordmark in place */
						want_release = 1;
					}
				} else if (released) {
					if (si.ssi_signo == SIGUSR2) /* the login screen did not come up */
						kd_text_now();
					cleanup_files();
					exit(0);
				} else {
					finish_text();
				}
			}
		}
		if (pf[1].revents & POLLIN) {
			char buf[2048];
			ssize_t n;
			int hit = 0;
			while (!released && (n = read(ifd, buf, sizeof buf)) > 0)
				for (char *p = buf; p < buf + n;) {
					struct inotify_event *ev = (struct inotify_event *)p;
					if (ev->wd == wd_dri || !strcmp(ev->len ? ev->name : "", "dri") ||
					    !strcmp(ev->len ? ev->name : "", "fb0"))
						hit = 1;
					p += sizeof *ev + ev->len;
				}
			if (hit && rescan_at < 0)
				rescan_at = now + 150000;
		}
		if (ttyfd >= 0 && (pf[2].revents & (POLLIN | POLLHUP | POLLERR)) && !released) {
			char k[16];
			ssize_t n = read(ttyfd, k, sizeof k);
			if (n == 1 && k[0] == 0x1b)
				finish_text();
			if (n <= 0 && (pf[2].revents & (POLLHUP | POLLERR)))
				tty_release(0);
		}
		if (released) {
			if (now >= release_us + (int64_t)LINGER_MS * 1000) {
				cleanup_files();
				exit(0);
			}
			continue;
		}
		if (rescan_at >= 0 && now >= rescan_at) {
			rescan_at = -1;
			if (rescan())
				retry_until = retry_until > now ? retry_until : now + 8000000;
			if (retry_until > now)
				rescan_at = now + 300000;
			else
				retry_until = -1;
		}
		if (!ncards && now >= start_us + (int64_t)GIVE_UP_MS * 1000) {
			logf_("no screen, boot as text");
			tty_release(1);
			cleanup_files();
			exit(0);
		}
		if (anim_done && !released && now >= done_us + (int64_t)STUCK_MS * 1000)
			finish_text();
	}
}

/* ---- command line ----------------------------------------------------------------------- */

static pid_t read_pid(void)
{
	char b[24] = "";
	int fd = open(PIDF, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	ssize_t n = read(fd, b, sizeof b - 1);
	close(fd);
	pid_t p = n > 0 ? (pid_t)atoi(b) : 0;
	return p > 1 && kill(p, 0) == 0 ? p : 0;
}

static int wait_file(const char *f, pid_t p, int ms)
{
	for (int i = 0; i < ms / 50; i++) {
		if (access(f, F_OK) == 0)
			return 1;
		if (kill(p, 0) < 0)
			return 0;
		nanosleep(&(struct timespec){0, 50 * 1000000}, NULL);
	}
	return 0;
}

static int in_cmdline(const char *word)
{
	char b[4096] = "";
	int fd = open("/proc/cmdline", O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	ssize_t n = read(fd, b, sizeof b - 2);
	close(fd);
	if (n <= 0)
		return 0;
	b[n] = 0;
	for (char *t = strtok(b, " \n"); t; t = strtok(NULL, " \n"))
		if (!strcmp(t, word))
			return 1;
	return 0;
}

/* --render: the picture at some time, into a PPM, for looking at it without a screen */
static int render_ppm(int ms, int w, int h, const char *file)
{
	if (art_init() < 0 || w < 64 || h < 64 || w > 8192 || h > 8192)
		return 1;
	struct out o = {.w = (uint32_t)w, .h = (uint32_t)h, .pitch = (uint32_t)w * 4, .rs = 16, .gs = 8, .nbuf = 1};
	o.size = (size_t)o.pitch * (size_t)h;
	o.map[0] = calloc(1, o.size);
	if (!o.map[0] || layout(&o) < 0)
		return 1;
	struct card c = {.fd = -1};
	present_full(&c, &o, ms);
	FILE *f = fopen(file, "wb");
	if (!f)
		return 1;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int y = 0; y < h; y++) {
		const uint32_t *row = (const uint32_t *)(o.map[0] + (size_t)y * o.pitch);
		for (int x = 0; x < w; x++) {
			uint8_t px[3] = {(uint8_t)(row[x] >> 16), (uint8_t)(row[x] >> 8), (uint8_t)row[x]};
			fwrite(px, 1, 3, f);
		}
	}
	return fclose(f) ? 1 : 0;
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "--handover")) {
		pid_t p = read_pid();
		if (!p)
			return 0;
		kill(p, SIGUSR1); /* the daemon runs the animation to its end, then the screen is let go */
		wait_file(RELF, p, HANDOVER_WAIT_MS);
		return 0;
	}
	if (argc > 1 && !strcmp(argv[1], "--text")) {
		pid_t p = read_pid();
		if (!p) {
			kd_text_now();
			return 0;
		}
		kill(p, SIGUSR2);
		for (int i = 0; i < 20 && kill(p, 0) == 0; i++)
			nanosleep(&(struct timespec){0, 50 * 1000000}, NULL);
		return 0;
	}
	if (argc == 6 && !strcmp(argv[1], "--render"))
		return render_ppm(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]), argv[5]);
	if (argc > 1) {
		fprintf(stderr, "usage: flsplash [--handover | --text | --render MS W H FILE.ppm]\n");
		return 2;
	}
	if (getuid() != 0 || read_pid() || in_cmdline("nosplash"))
		return 0;
	int pp[2];
	if (pipe(pp) < 0)
		return 0;
	pid_t pid = fork();
	if (pid < 0)
		return 0;
	if (pid > 0) { /* wait until the first picture is up (or it failed), then boot on */
		close(pp[1]);
		struct pollfd pf = {pp[0], POLLIN, 0};
		poll(&pf, 1, 1500);
		return 0;
	}
	close(pp[0]);
	setsid();
	chdir("/");
	int fd = open("/dev/null", O_RDWR);
	dup2(fd, 0);
	dup2(fd, 1);
	int lg = open(LOGF, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	dup2(lg >= 0 ? lg : fd, 2);
	char b[24];
	int n = snprintf(b, sizeof b, "%d\n", (int)getpid());
	int pf = open(PIDF, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (pf >= 0 && write(pf, b, (size_t)n) < 0) {
		/* no pid file: --handover finds nothing */
	}
	if (pf >= 0)
		close(pf);
	daemon_main(pp[1]);
	return 0;
}
