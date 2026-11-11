/* genart -- renders the layers of the FreeLFS boot logo for flsplash, offline.
 * SPDX-License-Identifier: MIT
 *
 * The three bars of docs/logo.svg (tilted 6 degrees) as beveled glossy slabs
 * with an extruded side, a soft glow, a faded mirror image on a dark floor, a
 * light-sweep mask and the word "FreeLFS", at a few sizes. Bevel normals,
 * diffuse and specular light, a fake studio environment and the gloss band
 * are computed here, per sample, in floating point with 6 x 6 (or 4 x 4)
 * supersampling; flsplash only composites the finished layers at boot.
 *
 *   genart [-f FONT.ttf] [-o flsplash.dat]
 *
 * File format (little endian), see flsplash.c for the reader:
 *   "FLSPART1", u32 nsizes, nsizes x { u32 S, Rw, Rh, offset },
 *   per size 6 layers { u32 x, y, w, h, planes, csize; deflate data }.
 *   The data is planar (one plane after the other), each plane with every row
 *   delta coded against the pixel to its left. Layers: 0 row colours (1 x
 *   Rh+2: region rows, then screen top, screen bottom), 1 glow (half
 *   resolution, RGB), 2 logo (A R G B, premultiplied), 3 reflection (A R G B,
 *   premultiplied), 4 sweep (position, gain), 5 the word (A R G B, premult.).
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include <ft2build.h>
#include FT_FREETYPE_H

typedef struct { float r, g, b; } v3;

static const int SIZES[] = {128, 176, 240, 320, 432, 576};
#define NSIZES ((int)(sizeof SIZES / sizeof *SIZES))
#define WORD "FreeLFS"

static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static float mixf(float a, float b, float t) { return a + (b - a) * t; }
static float sstep(float a, float b, float x)
{
	float t = clampf((x - a) / (b - a), 0, 1);
	return t * t * (3 - 2 * t);
}
static v3 V(float r, float g, float b) { return (v3){r, g, b}; }
static v3 vmix(v3 a, v3 b, float t) { return V(mixf(a.r, b.r, t), mixf(a.g, b.g, t), mixf(a.b, b.b, t)); }
static v3 vadd(v3 a, v3 b, float k) { return V(a.r + b.r * k, a.g + b.g * k, a.b + b.b * k); }
static v3 vmul(v3 a, float k) { return V(a.r * k, a.g * k, a.b * k); }

/* ---- other colours: -a RRGGBB turns the green material into another hue ----------------
 * Hue is replaced in HSV, saturation and brightness stay, so the white highlights stay
 * white and the shading is the one of the green original. TH < 0: no change (green). */
static float TH = -1;

static float hue_of(v3 c)
{
	float mx = fmaxf(c.r, fmaxf(c.g, c.b)), mn = fminf(c.r, fminf(c.g, c.b)), d = mx - mn;
	if (d <= 0)
		return 0;
	float h = mx == c.r ? fmodf((c.g - c.b) / d, 6.0f) : mx == c.g ? (c.b - c.r) / d + 2 : (c.r - c.g) / d + 4;
	h *= 60;
	return h < 0 ? h + 360 : h;
}

static v3 tint(v3 c)
{
	if (TH < 0)
		return c;
	float mx = fmaxf(c.r, fmaxf(c.g, c.b)), mn = fminf(c.r, fminf(c.g, c.b));
	float s = mx > 0 ? (mx - mn) / mx : 0, h = TH / 60.0f;
	float hf = fmodf(h, 2.0f) - 1.0f, x = 1.0f - (hf < 0 ? -hf : hf);
	float r, g, b;
	switch ((int)h % 6) {
	case 0: r = 1; g = x; b = 0; break;
	case 1: r = x; g = 1; b = 0; break;
	case 2: r = 0; g = 1; b = x; break;
	case 3: r = 0; g = x; b = 1; break;
	case 4: r = x; g = 0; b = 1; break;
	default: r = 1; g = 0; b = x; break;
	}
	return V(mx * (1 - s + s * r), mx * (1 - s + s * g), mx * (1 - s + s * b));
}

/* ---- the slabs -------------------------------------------------------------------------- */

/* docs/logo.svg before the turn: u0, v0, u1, v1 in a 20 x 20 box */
static const float BAR[3][4] = {{3, 3, 17, 6.4f}, {3, 8.3f, 13, 11.7f}, {3, 13.6f, 9, 17}};
static float CT, ST;       /* cos and sin of the 6 degree turn */
static float WDU, WDV;     /* extrusion vector in the bars' own frame */
static float LX, LY, LZ;   /* light direction */
static float HX, HY, HZ_;  /* half vector */

#define BEVEL 0.62f
#define BEVEL_H 0.70f

static void norm3(float *x, float *y, float *z)
{
	float l = sqrtf(*x * *x + *y * *y + *z * *z);
	*x /= l;
	*y /= l;
	*z /= l;
}

static void setup_math(void)
{
	float a = 6.0f * (float)M_PI / 180.0f;
	CT = cosf(a);
	ST = sinf(a);
	float dx = 0.30f, dy = 0.92f; /* extrusion, screen space, box units */
	WDU = dx * CT + dy * ST;
	WDV = -dx * ST + dy * CT;
	LX = -0.45f;
	LY = -0.62f;
	LZ = 0.64f;
	norm3(&LX, &LY, &LZ);
	HX = LX;
	HY = LY;
	HZ_ = LZ + 1;
	norm3(&HX, &HY, &HZ_);
}

static float powf_(float x, float e) { return x <= 0 ? 0 : powf(x, e); }

/* one point of a front face: u, v in the bar's frame; returns the colour */
static v3 face(const float *B, float u, float v, float *gloss)
{
	float d[4] = {u - B[0], B[2] - u, v - B[1], B[3] - v};
	static const float g[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
	float dmin = d[0];
	for (int i = 1; i < 4; i++)
		dmin = d[i] < dmin ? d[i] : dmin;
	const float ks = 0.07f;
	float sw = 0, gx = 0, gy = 0;
	for (int i = 0; i < 4; i++) {
		float w = expf(-(d[i] - dmin) / ks);
		sw += w;
		gx += w * g[i][0];
		gy += w * g[i][1];
	}
	float dd = dmin - ks * logf(sw);
	if (dd < 0)
		dd = 0;
	float gl = sqrtf(gx * gx + gy * gy);
	gx /= gl;
	gy /= gl;
	float q = clampf(dd / BEVEL, 0, 1);
	float s = BEVEL_H * ((float)M_PI / 2) / BEVEL * cosf(q * (float)M_PI / 2);
	float sx = gx * CT - gy * ST, sy = gx * ST + gy * CT;
	float nx = -s * sx, ny = -s * sy, nz = 1;
	norm3(&nx, &ny, &nz);

	float a = (u - B[0]) / (B[2] - B[0]), b = (v - B[1]) / (B[3] - B[1]);
	/* the green: lighter at the top, deep at the bottom */
	v3 top = V(0.34f, 0.98f, 0.58f), mid = V(0.09f, 0.70f, 0.33f), bot = V(0.02f, 0.30f, 0.14f);
	v3 c = b < 0.5f ? vmix(top, mid, sstep(0.0f, 0.5f, b)) : vmix(mid, bot, sstep(0.5f, 1.0f, b));
	float diff = fmaxf(0, nx * LX + ny * LY + nz * LZ);
	c = vmul(c, 0.50f + 0.62f * diff);
	/* light coming up from the floor into the lower part */
	c = vadd(c, V(0.05f, 0.42f, 0.20f), 0.75f * sstep(0.72f, 1.0f, b) * (1 - q * 0.4f));
	c = vmul(c, 1.0f - 0.18f * sstep(0.50f, 0.66f, b) * (1 - sstep(0.66f, 0.86f, b)));
	/* the bevel catches the light: a hard specular line and a soft one */
	float nh = fmaxf(0, nx * HX + ny * HY + nz * HZ_);
	float spec = powf_(nh, 70) * 1.15f + powf_(nh, 14) * 0.22f;
	c = vadd(c, V(0.95f, 1.0f, 0.96f), spec);
	/* the environment: bright sky above, dark green floor below, mostly at grazing angles */
	float ry = 2 * nz * ny;
	float sky = clampf(-ry * 1.4f, 0, 1), flo = clampf(ry * 1.4f, 0, 1);
	float fres = 0.04f + 0.96f * powf_(1 - nz, 2.0f);
	v3 env = vadd(vmul(V(0.80f, 1.0f, 0.88f), sky), V(0.04f, 0.26f, 0.13f), flo);
	c = vmix(c, env, fres * 0.80f);
	/* the gel highlight: the upper half of the flat face, hard-edged underneath */
	float wf = sstep(0.30f * BEVEL, 1.15f * BEVEL, dd);
	float be = 0.54f - 0.12f * a + 0.05f * sinf(a * (float)M_PI);
	float hl = 0;
	if (b < be)
		hl = 0.60f * (1.0f - 0.78f * powf_(b / be, 0.9f));
	hl *= wf * sstep(0.0f, 0.03f, be - b + 0.01f);
	c = vmix(c, V(0.93f, 1.0f, 0.95f), hl);
	/* the lower edge of the highlight is darker, like glass over a deep body */
	if (b >= be && b < be + 0.16f)
		c = vmul(c, 1.0f - 0.22f * (1 - (b - be) / 0.16f) * wf);
	*gloss = 0.40f + 0.60f * wf;
	return c;
}

/* one point of the extruded side (the part the viewer sees under and right of the faces) */
static int wall(float u, float v, v3 *col, float *gloss)
{
	float best = 2;
	int side = 0;
	for (int i = 0; i < 3; i++) {
		const float *B = BAR[i];
		float tul = (u - B[2]) / WDU, tuh = (u - B[0]) / WDU;
		float tvl = (v - B[3]) / WDV, tvh = (v - B[1]) / WDV;
		float te = fmaxf(fmaxf(tul, tvl), 0), tx = fminf(fminf(tuh, tvh), 1);
		if (te < tx && te < best) {
			best = te;
			side = tul > tvl; /* 1: right wall, 0: bottom wall */
		}
	}
	if (best > 1)
		return 0;
	float t = best;
	v3 base = vmix(V(0.12f, 0.56f, 0.32f), V(0.025f, 0.17f, 0.085f), powf_(t, 0.75f));
	float lit = side ? 0.62f : 0.80f;
	v3 c = vmul(base, lit);
	c = vadd(c, V(0.02f, 0.14f, 0.07f), (1 - t) * (side ? 0.4f : 0.9f));
	float rim = 1 - sstep(0.0f, 0.10f, t);
	c = vadd(c, V(0.55f, 0.95f, 0.66f), rim * (side ? 0.18f : 0.30f));
	*col = c;
	*gloss = 0.14f;
	return 1;
}

static int sample(float x, float y, v3 *col, float *gloss)
{
	float dx = x - 10, dy = y - 10;
	float u = 10 + dx * CT + dy * ST, v = 10 - dx * ST + dy * CT;
	for (int i = 0; i < 3; i++) {
		const float *B = BAR[i];
		if (u > B[0] && u < B[2] && v > B[1] && v < B[3]) {
			*col = tint(face(B, u, v, gloss));
			return 1;
		}
	}
	int hit = wall(u, v, col, gloss);
	if (hit)
		*col = tint(*col);
	return hit;
}

/* ---- image helpers ---------------------------------------------------------------------- */

static void blur_h(const float *src, float *dst, int w, int h, int n, float sigma)
{
	int r = (int)ceilf(sigma * 3);
	float *k = malloc((size_t)(2 * r + 1) * sizeof *k), sum = 0;
	for (int i = -r; i <= r; i++)
		sum += k[i + r] = expf(-(float)(i * i) / (2 * sigma * sigma));
	for (int i = 0; i <= 2 * r; i++)
		k[i] /= sum;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			for (int c = 0; c < n; c++) {
				float s = 0;
				for (int i = -r; i <= r; i++) {
					int xx = x + i;
					if (xx >= 0 && xx < w)
						s += k[i + r] * src[((size_t)y * (size_t)w + (size_t)xx) * (size_t)n + (size_t)c];
				}
				dst[((size_t)y * (size_t)w + (size_t)x) * (size_t)n + (size_t)c] = s;
			}
	free(k);
}

static void blur_v(const float *src, float *dst, int w, int h, int n, float sigma)
{
	int r = (int)ceilf(sigma * 3);
	float *k = malloc((size_t)(2 * r + 1) * sizeof *k), sum = 0;
	for (int i = -r; i <= r; i++)
		sum += k[i + r] = expf(-(float)(i * i) / (2 * sigma * sigma));
	for (int i = 0; i <= 2 * r; i++)
		k[i] /= sum;
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			for (int c = 0; c < n; c++) {
				float s = 0;
				for (int i = -r; i <= r; i++) {
					int yy = y + i;
					if (yy >= 0 && yy < h)
						s += k[i + r] * src[((size_t)yy * (size_t)w + (size_t)x) * (size_t)n + (size_t)c];
				}
				dst[((size_t)y * (size_t)w + (size_t)x) * (size_t)n + (size_t)c] = s;
			}
	free(k);
}

static void blur(float *img, int w, int h, int n, float sigma)
{
	float *t = malloc((size_t)w * (size_t)h * (size_t)n * sizeof *t);
	blur_h(img, t, w, h, n, sigma);
	blur_v(t, img, w, h, n, sigma);
	free(t);
}

static int bayer(int x, int y) /* 8 x 8 ordered dither, 0..63 */
{
	static int m[8][8];
	static int init;
	if (!init) {
		init = 1;
		for (int j = 0; j < 8; j++)
			for (int i = 0; i < 8; i++) {
				int v = 0, xx = i ^ j, yy = j;
				for (int b = 0; b < 3; b++)
					v = v * 4 + ((((yy >> b) & 1) << 1) | ((xx >> b) & 1));
				m[j][i] = v;
			}
	}
	return m[y & 7][x & 7];
}

static uint8_t q8(float v, int x, int y)
{
	float d = ((float)bayer(x, y) + 0.5f) / 64.0f - 0.5f;
	int i = (int)floorf(v * 255.0f + 0.5f + d * 0.9f);
	return (uint8_t)(i < 0 ? 0 : i > 255 ? 255 : i);
}

/* ---- the output file -------------------------------------------------------------------- */

struct buf {
	uint8_t *p;
	size_t n, cap;
};

static void put(struct buf *b, const void *d, size_t n)
{
	if (b->n + n > b->cap) {
		b->cap = (b->n + n) * 2;
		b->p = realloc(b->p, b->cap);
	}
	memcpy(b->p + b->n, d, n);
	b->n += n;
}

static void put32(struct buf *b, uint32_t v)
{
	uint8_t x[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
	put(b, x, 4);
}

/* one layer: planes[i] are w x h bytes each */
static void layer(struct buf *b, int x, int y, int w, int h, int np, uint8_t **planes)
{
	size_t plane = (size_t)w * (size_t)h, n = plane * (size_t)np;
	uint8_t *raw = malloc(n ? n : 1);
	for (int p = 0; p < np; p++)
		for (int r = 0; r < h; r++) {
			const uint8_t *s = planes[p] + (size_t)r * (size_t)w;
			uint8_t *d = raw + (size_t)p * plane + (size_t)r * (size_t)w;
			for (int i = 0; i < w; i++)
				d[i] = (uint8_t)(s[i] - (i ? s[i - 1] : 0));
		}
	uLongf cs = compressBound((uLong)n);
	uint8_t *c = malloc(cs);
	if (compress2(c, &cs, raw, (uLong)n, 9) != Z_OK) {
		fprintf(stderr, "genart: compress failed\n");
		exit(1);
	}
	put32(b, (uint32_t)x);
	put32(b, (uint32_t)y);
	put32(b, (uint32_t)w);
	put32(b, (uint32_t)h);
	put32(b, (uint32_t)np);
	put32(b, (uint32_t)cs);
	put(b, c, cs);
	free(raw);
	free(c);
}

/* ---- the word --------------------------------------------------------------------------- */

static int make_text(const char *font, int S, uint8_t *pl[4], int *tw, int *th)
{
	static const char *const fonts[] = {
		"/usr/share/fonts/ibm-plex/IBMPlexSans-Medium.ttf",
		"/sysflfs/share/fonts/ibm-plex/IBMPlexSans-Medium.ttf",
		"/usr/share/fonts/ibm-plex/IBMPlexSans-Regular.ttf",
		"/sysflfs/share/fonts/ibm-plex/IBMPlexSans-Regular.ttf",
	};
	FT_Library lib;
	FT_Face face = NULL;
	if (FT_Init_FreeType(&lib))
		return -1;
	if (font && FT_New_Face(lib, font, 0, &face))
		face = NULL;
	for (size_t i = 0; i < sizeof fonts / sizeof *fonts && !face; i++)
		if (FT_New_Face(lib, fonts[i], 0, &face))
			face = NULL;
	int em = (int)lroundf(0.205f * (float)S), big = em * 4;
	if (!face || FT_Set_Pixel_Sizes(face, 0, (unsigned)big)) {
		FT_Done_FreeType(lib);
		return -1;
	}
	int W = big * 9, H = big * 2, base = big * 3 / 2;
	uint8_t *m = calloc((size_t)W * (size_t)H, 1);
	int pen = big / 2;
	for (const char *c = WORD; *c; c++) {
		if (FT_Load_Char(face, (unsigned char)*c, FT_LOAD_RENDER | FT_LOAD_NO_HINTING))
			continue;
		FT_GlyphSlot g = face->glyph;
		for (unsigned r = 0; r < g->bitmap.rows; r++)
			for (unsigned q = 0; q < g->bitmap.width; q++) {
				int x = pen + g->bitmap_left + (int)q, y = base - g->bitmap_top + (int)r;
				if (x < 0 || x >= W || y < 0 || y >= H)
					continue;
				unsigned v = m[y * W + x] + g->bitmap.buffer[r * (unsigned)g->bitmap.pitch + q];
				m[y * W + x] = (uint8_t)(v > 255 ? 255 : v);
			}
		pen += (int)(g->linearHoriAdvance >> 16) + (int)lroundf(0.20f * (float)big); /* spaced */
	}
	FT_Done_Face(face);
	FT_Done_FreeType(lib);
	int x0 = W, x1 = -1, y0 = H, y1 = -1;
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++)
			if (m[y * W + x]) {
				x0 = x < x0 ? x : x0;
				x1 = x > x1 ? x : x1;
				y0 = y < y0 ? y : y0;
				y1 = y > y1 ? y : y1;
			}
	if (x1 < 0) {
		free(m);
		return -1;
	}
	x0 &= ~3;
	y0 &= ~3;
	int w = (x1 - x0) / 4 + 2, h = (y1 - y0) / 4 + 2;
	for (int i = 0; i < 4; i++)
		pl[i] = calloc((size_t)w * (size_t)h, 1);
	for (int y = 0; y < h; y++) {
		float t = (float)y / (float)(h - 1);
		v3 col = vmix(V(0.98f, 1.0f, 0.985f), V(0.70f, 0.78f, 0.74f), sstep(0.0f, 1.0f, t));
		for (int x = 0; x < w; x++) {
			unsigned sum = 0;
			for (int j = 0; j < 4; j++)
				for (int i = 0; i < 4; i++) {
					int xx = x0 + x * 4 + i, yy = y0 + y * 4 + j;
					if (xx < W && yy < H)
						sum += m[yy * W + xx];
				}
			float cov = (float)sum / (16.0f * 255.0f);
			float a = powf_(cov, 0.82f); /* light on dark looks thin: a little more body */
			size_t o = (size_t)y * (size_t)w + (size_t)x;
			pl[0][o] = (uint8_t)lroundf(a * 255);
			pl[1][o] = (uint8_t)lroundf(col.r * a * 255);
			pl[2][o] = (uint8_t)lroundf(col.g * a * 255);
			pl[3][o] = (uint8_t)lroundf(col.b * a * 255);
		}
	}
	free(m);
	*tw = w;
	*th = h;
	return 0;
}

/* ---- one size --------------------------------------------------------------------------- */

/* ---- the desktop wallpaper: the logo without the word, 1920 x 1080 ---------------------- */

#define WPS 432 /* logo size used */
#define WPW 1920
#define WPH 1080
static struct buf wpbuf;
static int wp_on;

static void wallpaper(int S, int Rw, int Rh, int lx, int ly, int yf, int sy0, int sy1, uint8_t **rc,
                      uint8_t **glp, int gw, int gh, int lx0, int ly0, int lw, int lh, uint8_t **lp,
                      int rh, uint8_t **rp)
{
	int ox = (WPW - Rw) / 2, oy = (WPH - (sy1 - sy0)) / 2 - sy0;
	uint8_t *pl[3];
	for (int c = 0; c < 3; c++)
		pl[c] = malloc((size_t)WPW * WPH);
	size_t gp = (size_t)gw * (size_t)gh;
	for (int y = 0; y < WPH; y++) {
		int ry = y - oy, cr = ry < 0 ? 0 : ry >= Rh ? Rh - 1 : ry;
		for (int x = 0; x < WPW; x++) {
			int rx = x - ox;
			float d[3];
			for (int c = 0; c < 3; c++)
				d[c] = rc[c][cr];
			if (rx >= 0 && rx < Rw && ry >= 0 && ry < Rh) {
				float fx = ((float)rx + 0.5f) / 2 - 0.5f, fy = ((float)ry + 0.5f) / 2 - 0.5f;
				int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
				float ax = fx - (float)x0, ay = fy - (float)y0;
				int x1 = x0 + 1 < gw ? x0 + 1 : gw - 1, y1 = y0 + 1 < gh ? y0 + 1 : gh - 1;
				x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0;
				for (int c = 0; c < 3; c++) {
					const uint8_t *g = glp[c];
					d[c] += mixf(mixf(g[(size_t)y0 * (size_t)gw + (size_t)x0], g[(size_t)y0 * (size_t)gw + (size_t)x1], ax),
					             mixf(g[(size_t)y1 * (size_t)gw + (size_t)x0], g[(size_t)y1 * (size_t)gw + (size_t)x1], ax), ay);
				}
				(void)gp;
				int qx = rx - lx0, qy = ry - ly0;
				if (qx >= 0 && qx < lw && qy >= 0 && qy < lh) {
					size_t o = (size_t)qy * (size_t)lw + (size_t)qx;
					float ia = 1.0f - (float)lp[0][o] / 255.0f;
					for (int c = 0; c < 3; c++)
						d[c] = (float)lp[1 + c][o] + ia * d[c];
				}
				qx = rx - lx; qy = ry - (yf + 1);
				if (qx >= 0 && qx < S && qy >= 0 && qy < rh) {
					size_t o = (size_t)qy * (size_t)S + (size_t)qx;
					float ia = 1.0f - (float)rp[0][o] / 255.0f;
					for (int c = 0; c < 3; c++)
						d[c] = (float)rp[1 + c][o] + ia * d[c];
				}
			}
			float dz = ((float)bayer(x, y) + 0.5f) / 64.0f - 0.5f;
			for (int c = 0; c < 3; c++) {
				int i = (int)floorf(d[c] + 0.5f + dz * 0.9f);
				pl[c][(size_t)y * WPW + (size_t)x] = (uint8_t)(i < 0 ? 0 : i > 255 ? 255 : i);
			}
		}
	}
	(void)ly;
	put(&wpbuf, "FLWALL01", 8);
	layer(&wpbuf, 0, 0, WPW, WPH, 3, pl);
	for (int c = 0; c < 3; c++)
		free(pl[c]);
}

static int even(float v) { return ((int)lroundf(v) + 1) & ~1; }

static void render(struct buf *out, int S, const char *font)
{
	int Rw = even(2.30f * (float)S), ly = (int)lroundf(0.60f * (float)S), lx = (Rw - S) / 2;
	int yf = ly + (int)lroundf(0.925f * (float)S);        /* the floor line */
	int tytop = yf + (int)lroundf(0.36f * (float)S);      /* the word */
	int Rh = even((float)tytop + 0.42f * (float)S);
	float k = (float)S / 20.0f;
	int SS = S <= 240 ? 6 : 4;

	/* the logo, premultiplied, floating point, in its S x S box */
	float *LG = calloc((size_t)S * (size_t)S * 4, sizeof *LG);
	float *GL = calloc((size_t)S * (size_t)S, sizeof *GL);
	for (int py = 0; py < S; py++)
		for (int px = 0; px < S; px++) {
			float acc[3] = {0, 0, 0}, gl = 0;
			int hit = 0;
			for (int sy = 0; sy < SS; sy++)
				for (int sx = 0; sx < SS; sx++) {
					float x = ((float)px + ((float)sx + 0.5f) / (float)SS) / k;
					float y = ((float)py + ((float)sy + 0.5f) / (float)SS) / k;
					v3 c;
					float g;
					if (!sample(x, y, &c, &g))
						continue;
					acc[0] += clampf(c.r, 0, 1);
					acc[1] += clampf(c.g, 0, 1);
					acc[2] += clampf(c.b, 0, 1);
					gl += g;
					hit++;
				}
			size_t o = (size_t)py * (size_t)S + (size_t)px;
			float n = (float)(SS * SS);
			LG[o * 4] = (float)hit / n;
			for (int c = 0; c < 3; c++)
				LG[o * 4 + 1 + (size_t)c] = acc[c] / n;
			GL[o] = hit ? gl / (float)hit : 0;
		}

	/* 8 bit planes of the logo, trimmed to what is not empty */
	int bx0 = S, bx1 = -1, by0 = S, by1 = -1;
	for (int y = 0; y < S; y++)
		for (int x = 0; x < S; x++)
			if (LG[((size_t)y * (size_t)S + (size_t)x) * 4] > 0) {
				bx0 = x < bx0 ? x : bx0;
				bx1 = x > bx1 ? x : bx1;
				by0 = y < by0 ? y : by0;
				by1 = y > by1 ? y : by1;
			}
	int lw = bx1 - bx0 + 1, lh = by1 - by0 + 1;
	uint8_t *lp[4];
	for (int i = 0; i < 4; i++)
		lp[i] = calloc((size_t)lw * (size_t)lh, 1);
	for (int y = 0; y < lh; y++)
		for (int x = 0; x < lw; x++) {
			const float *s = LG + ((size_t)(by0 + y) * (size_t)S + (size_t)(bx0 + x)) * 4;
			size_t o = (size_t)y * (size_t)lw + (size_t)x;
			uint8_t a = (uint8_t)lroundf(s[0] * 255);
			lp[0][o] = a;
			for (int c = 0; c < 3; c++) {
				uint8_t v = q8(s[1 + c], x, y);
				lp[1 + c][o] = v > a ? a : v;
			}
		}

	/* the mirror image on the floor: blurrier and fainter with distance */
	int dmax = (int)lroundf(0.60f * (float)S);
	float *RS = calloc((size_t)S * (size_t)dmax * 4, sizeof *RS), *RF = calloc((size_t)S * (size_t)dmax * 4, sizeof *RF);
	float *RG = calloc((size_t)S * (size_t)dmax, sizeof *RG);
	for (int j = 0; j < dmax; j++) {
		float ys = (float)(yf - ly) - (float)(j + 1) + 0.5f; /* row in the box */
		int y0 = (int)floorf(ys - 0.5f);
		float f = ys - 0.5f - (float)y0;
		for (int x = 0; x < S; x++)
			for (int c = 0; c < 4; c++) {
				float a = y0 >= 0 && y0 < S ? LG[((size_t)y0 * (size_t)S + (size_t)x) * 4 + (size_t)c] : 0;
				float b = y0 + 1 >= 0 && y0 + 1 < S ? LG[((size_t)(y0 + 1) * (size_t)S + (size_t)x) * 4 + (size_t)c] : 0;
				RS[((size_t)j * (size_t)S + (size_t)x) * 4 + (size_t)c] = mixf(a, b, f);
			}
		for (int x = 0; x < S; x++) {
			int yy = y0 >= 0 && y0 < S ? y0 : -1;
			RG[(size_t)j * (size_t)S + (size_t)x] = yy >= 0 ? GL[(size_t)yy * (size_t)S + (size_t)x] : 0;
		}
	}
	memcpy(RF, RS, (size_t)S * (size_t)dmax * 4 * sizeof *RF);
	blur(RS, S, dmax, 4, 0.0045f * (float)S + 0.4f);
	blur(RF, S, dmax, 4, 0.020f * (float)S);
	float *RM = calloc((size_t)S * (size_t)dmax * 4, sizeof *RM);
	for (int j = 0; j < dmax; j++) {
		float s = (float)j / (float)dmax;
		float fade = 0.64f * powf_(1 - s, 1.6f);
		float soft = sstep(0.0f, 0.85f, s);
		for (int x = 0; x < S; x++)
			for (int c = 0; c < 4; c++) {
				size_t o = ((size_t)j * (size_t)S + (size_t)x) * 4 + (size_t)c;
				float v = mixf(RS[o], RF[o], soft) * fade;
				if (c)
					v *= (c == 1 ? 0.80f : c == 2 ? 0.95f : 0.86f); /* a little darker, greener */
				RM[o] = v;
			}
	}
	int rh = dmax;
	uint8_t *rp[4];
	for (int i = 0; i < 4; i++)
		rp[i] = calloc((size_t)S * (size_t)rh, 1);
	for (int y = 0; y < rh; y++)
		for (int x = 0; x < S; x++) {
			const float *s = RM + ((size_t)y * (size_t)S + (size_t)x) * 4;
			size_t o = (size_t)y * (size_t)S + (size_t)x;
			uint8_t a = (uint8_t)lroundf(s[0] * 255);
			rp[0][o] = a;
			for (int c = 0; c < 3; c++) {
				uint8_t v = q8(s[1 + c], x, y);
				rp[1 + c][o] = v > a ? a : v;
			}
		}

	/* the sweep: where each pixel lies along the band's axis, and how much it shines */
	const float phi = 24.0f * (float)M_PI / 180.0f;
	float half = 0.5f * (float)S * (cosf(phi) + sinf(phi));
	int sy0 = ly + by0, sy1 = yf + rh; /* rows (region) */
	int sh = sy1 - sy0;
	uint8_t *su = calloc((size_t)S * (size_t)sh, 1), *sg = calloc((size_t)S * (size_t)sh, 1);
	for (int y = 0; y < sh; y++)
		for (int x = 0; x < S; x++) {
			int ry = sy0 + y;
			size_t o = (size_t)y * (size_t)S + (size_t)x;
			float cy = ry <= yf ? (float)(ry - ly) : (float)(2 * yf - ly - ry);
			float prj = ((float)x - 0.5f * (float)S) * cosf(phi) + (cy - 0.5f * (float)S) * sinf(phi);
			su[o] = (uint8_t)lroundf(clampf(127.5f + 127.5f * prj / half, 0, 255));
			float g = 0;
			if (ry <= yf) {
				int yy = ry - ly;
				if (yy >= 0 && yy < S)
					g = LG[((size_t)yy * (size_t)S + (size_t)x) * 4] * GL[(size_t)yy * (size_t)S + (size_t)x];
			} else {
				int j = ry - yf - 1;
				if (j >= 0 && j < rh) {
					float s = (float)j / (float)dmax;
					g = RM[((size_t)j * (size_t)S + (size_t)x) * 4] * 1.0f / 0.64f * 0.55f *
					    RG[(size_t)j * (size_t)S + (size_t)x] * powf_(1 - s, 1.2f);
				}
			}
			sg[o] = (uint8_t)lroundf(clampf(g, 0, 1) * 255);
		}

	/* the glow, half resolution, additive */
	int gw = Rw / 2, gh = Rh / 2;
	float *gl = calloc((size_t)gw * (size_t)gh * 3, sizeof *gl);
	float *al = calloc((size_t)gw * (size_t)gh, sizeof *al);
	for (int y = 0; y < S; y++)
		for (int x = 0; x < S; x++) {
			int gx = (lx + x) / 2, gy = (ly + y) / 2;
			if (gx < gw && gy < gh)
				al[(size_t)gy * (size_t)gw + (size_t)gx] += LG[((size_t)y * (size_t)S + (size_t)x) * 4] * 0.25f;
		}
	float *bl1 = malloc((size_t)gw * (size_t)gh * sizeof *bl1), *bl2 = malloc((size_t)gw * (size_t)gh * sizeof *bl2);
	memcpy(bl1, al, (size_t)gw * (size_t)gh * sizeof *bl1);
	memcpy(bl2, al, (size_t)gw * (size_t)gh * sizeof *bl2);
	blur(bl1, gw, gh, 1, 0.045f * (float)S / 2);
	blur(bl2, gw, gh, 1, 0.15f * (float)S / 2);
	float cx = (float)Rw / 2, cy = (float)ly + 0.5f * (float)S * 0.97f;
	for (int y = 0; y < gh; y++)
		for (int x = 0; x < gw; x++) {
			float px = ((float)x + 0.5f) * 2, py = ((float)y + 0.5f) * 2;
			/* broad halo behind the logo, windowed to nothing at the region's edge */
			float rx = (px - cx) / (1.10f * (float)S), ry = (py - cy) / (1.02f * (float)S);
			float r2 = rx * rx + ry * ry;
			float halo = expf(-3.2f * r2) * (1 - sstep(0.55f, 1.0f, sqrtf(r2)));
			/* the pool of light on the floor */
			float prx = (px - cx) / (1.02f * (float)S), pry = (py - (float)yf - 0.06f * (float)S) / (0.52f * (float)S);
			float pr2 = prx * prx + pry * pry;
			float pool = expf(-3.0f * pr2) * (1 - sstep(0.5f, 1.0f, sqrtf(pr2)));
			if (py < (float)yf)
				pool *= sstep((float)yf - 0.20f * (float)S, (float)yf, py);
			float sh1 = bl1[(size_t)y * (size_t)gw + (size_t)x], sh2 = bl2[(size_t)y * (size_t)gw + (size_t)x];
			float e1 = halo * 0.50f + sh1 * 0.22f + sh2 * 0.50f;
			float e2 = pool * 0.38f;
			float *o = gl + ((size_t)y * (size_t)gw + (size_t)x) * 3;
			float e = e1 + e2;
			v3 g1 = tint(V(0.095f, 0.50f, 0.25f)), g2 = tint(V(0.05f, 0.10f, 0.06f));
			o[0] = e * g1.r + e * e * g2.r;
			o[1] = e * g1.g + e * e * g2.g;
			o[2] = e * g1.b + e * e * g2.b;
		}
	uint8_t *glp[3];
	for (int i = 0; i < 3; i++)
		glp[i] = calloc((size_t)gw * (size_t)gh, 1);
	for (int i = 0; i < gw * gh; i++)
		for (int c = 0; c < 3; c++)
			glp[c][i] = (uint8_t)lroundf(clampf(gl[i * 3 + c], 0, 1) * 255);

	/* the room: top and floor, and the horizon line between them */
	uint8_t *rc[3];
	for (int i = 0; i < 3; i++)
		rc[i] = calloc((size_t)Rh + 2, 1);
	v3 HZc = V(0.130f, 0.155f, 0.172f), TOPR = V(0.050f, 0.062f, 0.070f), FL0 = V(0.085f, 0.105f, 0.115f),
	   FL1 = V(0.030f, 0.038f, 0.043f), TOP = V(0.030f, 0.038f, 0.043f), BOT = V(0.022f, 0.028f, 0.031f);
	float sig = fmaxf(0.8f, (float)S / 220.0f);
	for (int y = 0; y < Rh + 2; y++) {
		v3 c;
		if (y == Rh)
			c = TOP;
		else if (y == Rh + 1)
			c = BOT;
		else if (y <= yf) {
			float h = ((float)yf - (float)y) / (float)S;
			c = vmix(HZc, TOPR, sstep(0.0f, 1.55f, h));
		} else {
			float d = ((float)y - (float)yf) / (float)S;
			c = vmix(FL0, FL1, powf_(sstep(0.0f, 0.82f, d), 0.8f));
		}
		if (y < Rh) {
			float e = ((float)y - (float)yf) / sig;
			c = vadd(c, V(0.07f, 0.09f, 0.10f), expf(-0.5f * e * e));
		}
		rc[0][y] = (uint8_t)lroundf(clampf(c.r, 0, 1) * 255);
		rc[1][y] = (uint8_t)lroundf(clampf(c.g, 0, 1) * 255);
		rc[2][y] = (uint8_t)lroundf(clampf(c.b, 0, 1) * 255);
	}

	uint8_t *tp[4] = {0};
	int tw = 0, th = 0;
	int have_text = make_text(font, S, tp, &tw, &th) == 0;

	if (wp_on && S == WPS)
		wallpaper(S, Rw, Rh, lx, ly, yf, ly + by0, yf + rh, rc, glp, gw, gh, lx + bx0, ly + by0, lw, lh, lp, rh, rp);
	/* the block for this size */
	layer(out, 0, 0, 1, Rh + 2, 3, rc);
	layer(out, 0, 0, gw, gh, 3, glp);
	layer(out, lx + bx0, ly + by0, lw, lh, 4, lp);
	layer(out, lx, yf + 1, S, rh, 4, rp);
	uint8_t *sp[2] = {su, sg};
	layer(out, lx, sy0, S, sh, 2, sp);
	if (!have_text) {
		fprintf(stderr, "genart: no IBM Plex Sans font (use -f FONT.ttf)\n");
		exit(1);
	}
	layer(out, (Rw - tw) / 2, tytop, tw, th, 4, tp);
	fprintf(stderr, "genart: S=%d region %dx%d, %zu bytes so far\n", S, Rw, Rh, out->n);
	free(LG); free(GL); free(RS); free(RF); free(RG); free(RM); free(gl); free(al); free(bl1); free(bl2);
	for (int i = 0; i < 4; i++) { free(lp[i]); free(rp[i]); free(tp[i]); }
	for (int i = 0; i < 3; i++) { free(glp[i]); free(rc[i]); }
	free(su); free(sg);
}

int main(int argc, char **argv)
{
	const char *font = NULL, *outf = "flsplash.dat", *wpf = NULL;
	int wp_only = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-W")) {
			wp_only = 1;
			memmove(argv + i, argv + i + 1, (size_t)(argc - i) * sizeof *argv);
			argc--;
			i--;
		}
	}
	for (int i = 1; i + 1 < argc; i += 2) {
		if (!strcmp(argv[i], "-f"))
			font = argv[i + 1];
		else if (!strcmp(argv[i], "-o"))
			outf = argv[i + 1];
		else if (!strcmp(argv[i], "-w"))
			wpf = argv[i + 1], wp_on = 1;
		else if (!strcmp(argv[i], "-a")) {
			const char *h = argv[i + 1] + (argv[i + 1][0] == '#');
			unsigned long v = strtoul(h, NULL, 16);
			TH = hue_of(V((float)(v >> 16 & 255) / 255, (float)(v >> 8 & 255) / 255, (float)(v & 255) / 255));
		} else {
			fprintf(stderr, "usage: genart [-f FONT.ttf] [-o FILE] [-w WALLPAPER] [-a RRGGBB] [-W]\n"
			                "  -a  render the wallpaper's 3D F in this hue instead of green\n"
			                "  -W  wallpaper only (-w FILE): skip the boot logo sizes, write no -o file\n");
			return 2;
		}
	}
	setup_math();
	struct buf blocks = {0}, offs = {0};
	struct buf hdr = {0};
	size_t dirsz = 8 + 4 + (size_t)NSIZES * 16;
	for (int i = 0; i < NSIZES; i++) {
		int S = SIZES[i];
		if (wp_only && S != WPS)
			continue;
		size_t start = blocks.n;
		render(&blocks, S, font);
		int Rw = even(2.30f * (float)S), ly = (int)lroundf(0.60f * (float)S);
		int yf = ly + (int)lroundf(0.925f * (float)S), Rh = even((float)(yf + (int)lroundf(0.36f * (float)S)) + 0.42f * (float)S);
		put32(&offs, (uint32_t)S);
		put32(&offs, (uint32_t)Rw);
		put32(&offs, (uint32_t)Rh);
		put32(&offs, (uint32_t)(dirsz + start));
	}
	if (wp_only) {
		FILE *w = wpf ? fopen(wpf, "wb") : NULL;
		if (!w || fwrite(wpbuf.p, 1, wpbuf.n, w) != wpbuf.n || fclose(w)) {
			perror(wpf ? wpf : "genart: -W needs -w FILE");
			return 1;
		}
		fprintf(stderr, "genart: %s, %zu bytes\n", wpf, wpbuf.n);
		return 0;
	}
	put(&hdr, "FLSPART1", 8);
	put32(&hdr, NSIZES);
	put(&hdr, offs.p, offs.n);
	FILE *f = fopen(outf, "wb");
	if (!f || fwrite(hdr.p, 1, hdr.n, f) != hdr.n || fwrite(blocks.p, 1, blocks.n, f) != blocks.n || fclose(f)) {
		perror(outf);
		return 1;
	}
	if (wpf) {
		FILE *w = fopen(wpf, "wb");
		if (!w || fwrite(wpbuf.p, 1, wpbuf.n, w) != wpbuf.n || fclose(w)) {
			perror(wpf);
			return 1;
		}
	}
	fprintf(stderr, "genart: %s, %zu bytes\n", outf, hdr.n + blocks.n);
	return 0;
}
