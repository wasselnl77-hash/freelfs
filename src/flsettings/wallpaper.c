/* wallpaper.c -- Settings > Display > Wallpaper
 * SPDX-License-Identifier: MIT
 *
 * wallpaper= in desktop.conf (flcomp watches it, see flcomp/theme.c):
 *   accent       the built-in 3D F in the current accent colour (the default)
 *   green teal blue amber orange rose   one built-in variant
 *   user:NAME    an imported picture, $XDG_DATA_HOME/freelfs/wallpapers/NAME.flw
 *
 * The built-in variants are /usr/share/flcomp/wallpaper[-ID].flw (FLCOMP_SHARE moves that
 * directory), drawn by flsplash's genart. Imported PNG and JPEG files are decoded here once,
 * scaled down to fit 2560 x 1440 and written in the .flw format flcomp loads:
 *   "FLWALL01", u32 0, 0 (x, y), u32 w, h, 3 (planes), csize, then deflate data: R, G and
 *   B plane one after the other, every row delta coded against the pixel to its left.
 * The tiles show 128 x 72 thumbnails made from those files.
 */
#include <dirent.h>
#include <errno.h>
#include <flui.h>
#include <stdint.h>
#include <stdio.h>
#include <jpeglib.h>
#include <png.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "wallpaper.h"

#define TW 128
#define TH 72
#define MAXW 2560
#define MAXH 1440
#define MAX_PIXELS (64L * 1000 * 1000) /* of the source picture */
#define MAXTILES 40

enum { ID_WP_MATCH = 700, ID_WP_PATH, ID_WP_IMPORT, ID_WP_REMOVE, ID_WP_TILE = 710 /* .. 749 */ };

struct tile {
	char value[64];      /* what wallpaper= gets (accent excluded) */
	char label[40];
	char file[600];
	uint32_t *thumb;     /* TW x TH, XRGB, NULL: unreadable */
	int user;
};

struct wallpaper {
	fl_app *app;
	fl_win *win;
	fl_font *head, *small;
	struct tile tiles[MAXTILES];
	int n;
	char path[512];
	char msg[200];
	int msg_err;
};

/* ---- directories -------------------------------------------------------------------------- */

static const char *share_dir(void)
{
	const char *e = getenv("FLCOMP_SHARE");
	return e && *e ? e : "/usr/share/flcomp";
}

static void user_dir(char *out, size_t cap)
{
	const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
	if (x && *x)
		snprintf(out, cap, "%s/freelfs/wallpapers", x);
	else
		snprintf(out, cap, "%s/.local/share/freelfs/wallpapers", h ? h : "");
}

static void mkdirs(const char *dir)
{
	char t[600];
	snprintf(t, sizeof t, "%s", dir);
	for (char *p = t + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			mkdir(t, 0755);
			*p = '/';
		}
	mkdir(t, 0755);
}

/* ---- reading a .flw ---------------------------------------------------------------------------- */

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* a TW x TH thumbnail (cover, box filter) of a .flw file, or NULL */
static uint32_t *flw_thumb(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long fsz = ftell(f);
	rewind(f);
	uint8_t *fd = fsz > 32 && fsz < (64 << 20) ? malloc((size_t)fsz) : NULL;
	if (!fd || fread(fd, 1, (size_t)fsz, f) != (size_t)fsz || memcmp(fd, "FLWALL01", 8)) {
		free(fd);
		fclose(f);
		return NULL;
	}
	fclose(f);
	uint32_t w = le32(fd + 16), h = le32(fd + 20), np = le32(fd + 24), cs = le32(fd + 28);
	size_t plane = (size_t)w * h, n = plane * 3;
	uint8_t *raw = NULL;
	uint32_t *out = NULL;
	if (np != 3 || w < 2 || h < 2 || w > 8192 || h > 8192 || (size_t)cs + 32 > (size_t)fsz)
		goto done;
	raw = malloc(n);
	out = malloc((size_t)TW * TH * 4);
	uLongf rl = n;
	if (!raw || !out || uncompress(raw, &rl, fd + 32, cs) != Z_OK || rl != n) {
		free(out);
		out = NULL;
		goto done;
	}
	for (int c = 0; c < 3; c++)
		for (size_t y = 0; y < h; y++) {
			uint8_t *r = raw + c * plane + y * w;
			for (size_t x = 1; x < w; x++)
				r[x] = (uint8_t)(r[x] + r[x - 1]);
		}
	/* the part of the picture that covers the 16:9 tile, centred */
	double sc = (double)TW / w > (double)TH / h ? (double)TW / w : (double)TH / h;
	double cw = TW / sc, ch = TH / sc, ox = (w - cw) / 2, oy = (h - ch) / 2;
	for (int y = 0; y < TH; y++) {
		int y0 = (int)(oy + y * ch / TH), y1 = (int)(oy + (y + 1) * ch / TH);
		if (y1 <= y0)
			y1 = y0 + 1;
		if (y1 > (int)h)
			y1 = (int)h;
		for (int x = 0; x < TW; x++) {
			int x0 = (int)(ox + x * cw / TW), x1 = (int)(ox + (x + 1) * cw / TW);
			if (x1 <= x0)
				x1 = x0 + 1;
			if (x1 > (int)w)
				x1 = (int)w;
			unsigned long s[3] = {0, 0, 0};
			int cnt = 0;
			for (int yy = y0; yy < y1; yy++)
				for (int xx = x0; xx < x1; xx++, cnt++)
					for (int c = 0; c < 3; c++)
						s[c] += raw[c * plane + (size_t)yy * w + (size_t)xx];
			if (!cnt)
				cnt = 1;
			out[y * TW + x] = 0xff000000u | (uint32_t)(s[0] / cnt) << 16 | (uint32_t)(s[1] / cnt) << 8 | (uint32_t)(s[2] / cnt);
		}
	}
done:
	free(raw);
	free(fd);
	return out;
}

/* ---- writing one --------------------------------------------------------------------------------- */

/* planes: 3 x w x h bytes (R, G, B); consumed in place */
static int flw_write(const char *path, uint8_t *planes, int w, int h)
{
	size_t plane = (size_t)w * h;
	for (int c = 0; c < 3; c++)
		for (int y = 0; y < h; y++) {
			uint8_t *r = planes + c * plane + (size_t)y * w;
			for (int x = w - 1; x > 0; x--)
				r[x] = (uint8_t)(r[x] - r[x - 1]);
		}
	uLongf cs = compressBound((uLong)(plane * 3));
	uint8_t *c = malloc(cs);
	if (!c || compress2(c, &cs, planes, (uLong)(plane * 3), 6) != Z_OK) {
		free(c);
		return -1;
	}
	char tmp[640];
	snprintf(tmp, sizeof tmp, "%s.tmp%d", path, (int)getpid());
	FILE *f = fopen(tmp, "wb");
	if (!f) {
		free(c);
		return -1;
	}
	uint8_t hdr[32] = "FLWALL01";
	uint32_t v[6] = {0, 0, (uint32_t)w, (uint32_t)h, 3, (uint32_t)cs};
	for (int i = 0; i < 6; i++)
		for (int b = 0; b < 4; b++)
			hdr[8 + i * 4 + b] = (uint8_t)(v[i] >> (8 * b));
	int ok = fwrite(hdr, 1, 32, f) == 32 && fwrite(c, 1, cs, f) == cs;
	ok = fclose(f) == 0 && ok;
	free(c);
	if (!ok || rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

/* ---- scaling rows down while they are read (a box filter, bounded memory) ---------------------------- */

struct scaler {
	int sw, sh, dw, dh;
	uint8_t *planes;	/* 3 x dw x dh */
	uint32_t *acc;		/* dw x 3 sums of the rows of the current destination row */
	int cur, rows;		/* destination row, source rows summed into it */
	int sy;			/* next source row */
};

static int sc_init(struct scaler *s, int sw, int sh)
{
	memset(s, 0, sizeof *s);
	s->sw = sw;
	s->sh = sh;
	double k = 1.0;
	if (sw > MAXW || sh > MAXH)
		k = (double)MAXW / sw < (double)MAXH / sh ? (double)MAXW / sw : (double)MAXH / sh;
	s->dw = (int)(sw * k + 0.5);
	s->dh = (int)(sh * k + 0.5);
	if (s->dw < 2)
		s->dw = 2;
	if (s->dh < 2)
		s->dh = 2;
	s->planes = calloc((size_t)s->dw * s->dh, 3);
	s->acc = calloc((size_t)s->dw * 3, sizeof *s->acc);
	return s->planes && s->acc ? 0 : -1;
}

static void sc_flush(struct scaler *s)
{
	if (s->cur >= s->dh || !s->rows)
		return;
	size_t plane = (size_t)s->dw * s->dh;
	for (int x = 0; x < s->dw; x++)
		for (int c = 0; c < 3; c++)
			s->planes[c * plane + (size_t)s->cur * s->dw + x] = (uint8_t)(s->acc[x * 3 + c] / (uint32_t)s->rows);
	memset(s->acc, 0, (size_t)s->dw * 3 * sizeof *s->acc);
	s->rows = 0;
	s->cur++;
}

/* one source row of RGB bytes */
static void sc_row(struct scaler *s, const uint8_t *rgb)
{
	/* which destination row does this source row belong to */
	int d = (int)((long)s->sy * s->dh / s->sh);
	if (d > s->cur)
		sc_flush(s);
	if (s->cur >= s->dh) {
		s->sy++;
		return;
	}
	for (int x = 0; x < s->dw; x++) {
		int x0 = (int)((long)x * s->sw / s->dw), x1 = (int)((long)(x + 1) * s->sw / s->dw);
		if (x1 <= x0)
			x1 = x0 + 1;
		uint32_t t[3] = {0, 0, 0};
		for (int i = x0; i < x1; i++)
			for (int c = 0; c < 3; c++)
				t[c] += rgb[i * 3 + c];
		for (int c = 0; c < 3; c++)
			s->acc[x * 3 + c] += t[c] / (uint32_t)(x1 - x0);
	}
	s->rows++;
	s->sy++;
}

static void sc_free(struct scaler *s)
{
	free(s->planes);
	free(s->acc);
}

/* de-interleave: rows were summed as RGB triples, the file wants planes; sc_flush already stores planar */

/* ---- PNG and JPEG --------------------------------------------------------------------------------- */

struct pngerr { jmp_buf jb; char *msg; size_t cap; };

static void png_fail(png_structp p, png_const_charp m)
{
	struct pngerr *e = png_get_error_ptr(p);
	snprintf(e->msg, e->cap, "That PNG file cannot be read (%s).", m);
	longjmp(e->jb, 1);
}

static void png_warn(png_structp p, png_const_charp m) {}

/* RGB or RGBA row to RGB: alpha is mixed with the desktop colour 1d2125 */
static const uint8_t *flatten(const uint8_t *src, int ch, int w, uint8_t *dst)
{
	if (ch == 3)
		return src;
	static const uint8_t bg[3] = {0x1d, 0x21, 0x25};
	for (int x = 0; x < w; x++) {
		unsigned a = src[x * 4 + 3];
		for (int c = 0; c < 3; c++)
			dst[x * 3 + c] = (uint8_t)((src[x * 4 + c] * a + bg[c] * (255 - a) + 127) / 255);
	}
	return dst;
}

static int load_png(FILE *f, struct scaler *sc, char *err, size_t ecap)
{
	struct pngerr pe = {.msg = err, .cap = ecap};
	png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &pe, png_fail, png_warn);
	png_infop info = png ? png_create_info_struct(png) : NULL;
	uint8_t *volatile row = NULL, **volatile rows = NULL, *volatile whole = NULL, *volatile flat = NULL;
	int ok = -1, inited = 0;
	if (!png || !info) {
		snprintf(err, ecap, "Out of memory.");
		goto out;
	}
	if (setjmp(pe.jb))
		goto out;
	png_init_io(png, f);
	png_read_info(png, info);
	png_uint_32 w, h;
	int depth, ctype, il;
	png_get_IHDR(png, info, &w, &h, &depth, &ctype, &il, NULL, NULL);
	if (!w || !h || (long)w * h > MAX_PIXELS || w > 30000 || h > 30000) {
		snprintf(err, ecap, "That picture is too big (over 64 megapixels).");
		goto out;
	}
	if (il != PNG_INTERLACE_NONE && (long)w * h > 24L * 1000 * 1000) {
		snprintf(err, ecap, "That interlaced PNG is too big to convert; save it without interlacing.");
		goto out;
	}
	if (ctype == PNG_COLOR_TYPE_PALETTE)
		png_set_palette_to_rgb(png);
	if (ctype == PNG_COLOR_TYPE_GRAY || ctype == PNG_COLOR_TYPE_GRAY_ALPHA) {
		if (depth < 8)
			png_set_expand_gray_1_2_4_to_8(png);
		png_set_gray_to_rgb(png);
	}
	if (png_get_valid(png, info, PNG_INFO_tRNS))
		png_set_tRNS_to_alpha(png);
	if (depth == 16)
		png_set_strip_16(png);
	/* what is left is 8 bit RGB or RGBA; alpha is flattened onto the desktop colour below */
	int passes = png_set_interlace_handling(png);
	png_read_update_info(png, info);
	if (sc_init(sc, (int)w, (int)h) < 0) {
		snprintf(err, ecap, "Out of memory.");
		goto out;
	}
	inited = 1;
	size_t rb = png_get_rowbytes(png, info);
	int ch = (int)(rb / w);
	flat = malloc((size_t)w * 3);
	if (!flat || (ch != 3 && ch != 4) || rb < (size_t)w * (size_t)ch) {
		snprintf(err, ecap, "That PNG file cannot be read (unexpected format).");
		goto out;
	}
	if (passes > 1) {
		whole = malloc(rb * h);
		rows = malloc(h * sizeof *rows);
		if (!whole || !rows) {
			snprintf(err, ecap, "Out of memory.");
			goto out;
		}
		for (png_uint_32 y = 0; y < h; y++)
			rows[y] = whole + rb * y;
		png_read_image(png, rows);
		for (png_uint_32 y = 0; y < h; y++)
			sc_row(sc, flatten(rows[y], ch, (int)w, flat));
	} else {
		row = malloc(rb);
		if (!row) {
			snprintf(err, ecap, "Out of memory.");
			goto out;
		}
		for (png_uint_32 y = 0; y < h; y++) {
			png_read_row(png, row, NULL);
			sc_row(sc, flatten(row, ch, (int)w, flat));
		}
	}
	ok = 0;
out:
	(void)inited;
	free(row);
	free(rows);
	free(whole);
	free(flat);
	png_destroy_read_struct(&png, &info, NULL);
	return ok;
}

struct jerr { struct jpeg_error_mgr pub; jmp_buf jb; };

static void jpeg_fail(j_common_ptr c)
{
	longjmp(((struct jerr *)c->err)->jb, 1);
}

static int load_jpeg(FILE *f, struct scaler *sc, char *err, size_t ecap)
{
	struct jpeg_decompress_struct ci;
	struct jerr je;
	uint8_t *volatile row = NULL;
	int ok = -1;
	ci.err = jpeg_std_error(&je.pub);
	je.pub.error_exit = jpeg_fail;
	je.pub.output_message = NULL;
	if (setjmp(je.jb)) {
		if (!err[0])
			snprintf(err, ecap, "That JPEG file cannot be read.");
		jpeg_destroy_decompress(&ci);
		free(row);
		return -1;
	}
	jpeg_create_decompress(&ci);
	jpeg_stdio_src(&ci, f);
	jpeg_read_header(&ci, TRUE);
	if ((long)ci.image_width * ci.image_height > MAX_PIXELS) {
		snprintf(err, ecap, "That picture is too big (over 64 megapixels).");
		jpeg_destroy_decompress(&ci);
		return -1;
	}
	if (ci.jpeg_color_space == JCS_CMYK || ci.jpeg_color_space == JCS_YCCK) {
		snprintf(err, ecap, "CMYK JPEG files are not supported.");
		jpeg_destroy_decompress(&ci);
		return -1;
	}
	ci.out_color_space = JCS_RGB;
	/* let the decoder scale by 1/2, 1/4, 1/8 while the result still fills the target box */
	ci.scale_num = 1;
	for (int d = 8; d >= 2; d /= 2)
		if ((int)ci.image_width / d >= MAXW && (int)ci.image_height / d >= MAXH) {
			ci.scale_denom = (unsigned)d;
			break;
		}
	if (!ci.scale_denom)
		ci.scale_denom = 1;
	jpeg_start_decompress(&ci);
	if (sc_init(sc, (int)ci.output_width, (int)ci.output_height) < 0) {
		snprintf(err, ecap, "Out of memory.");
		jpeg_destroy_decompress(&ci);
		return -1;
	}
	row = malloc((size_t)ci.output_width * 3);
	if (!row) {
		snprintf(err, ecap, "Out of memory.");
		jpeg_destroy_decompress(&ci);
		return -1;
	}
	while (ci.output_scanline < ci.output_height) {
		JSAMPROW rp = row;
		jpeg_read_scanlines(&ci, &rp, 1);
		sc_row(sc, row);
	}
	jpeg_finish_decompress(&ci);
	ok = 0;
	jpeg_destroy_decompress(&ci);
	free(row);
	return ok;
}

static int sane_name(char *out, size_t cap, const char *src)
{
	const char *b = strrchr(src, '/');
	b = b ? b + 1 : src;
	size_t n = 0;
	for (; *b && *b != '.' && n + 1 < cap && n < 36; b++)
		out[n++] = (char)((*b >= 'a' && *b <= 'z') || (*b >= 'A' && *b <= 'Z') || (*b >= '0' && *b <= '9') || *b == '-' || *b == '_' ? *b : '-');
	out[n] = 0;
	if (!n)
		snprintf(out, cap, "wallpaper");
	return 0;
}

int wp_import_file(const char *src, char *name, size_t ncap, char *err, size_t ecap)
{
	err[0] = 0;
	FILE *f = fopen(src, "rb");
	if (!f) {
		snprintf(err, ecap, "Cannot open the file: %s.", strerror(errno));
		return -1;
	}
	struct stat st;
	if (fstat(fileno(f), &st) == 0 && !S_ISREG(st.st_mode)) {
		fclose(f);
		snprintf(err, ecap, "That is not a picture file.");
		return -1;
	}
	unsigned char magic[8] = {0};
	size_t got = fread(magic, 1, 8, f);
	rewind(f);
	struct scaler sc;
	int rc;
	memset(&sc, 0, sizeof sc);
	if (got >= 8 && !png_sig_cmp(magic, 0, 8))
		rc = load_png(f, &sc, err, ecap);
	else if (got >= 3 && magic[0] == 0xff && magic[1] == 0xd8 && magic[2] == 0xff)
		rc = load_jpeg(f, &sc, err, ecap);
	else {
		fclose(f);
		snprintf(err, ecap, "Only PNG and JPEG pictures can be used.");
		return -1;
	}
	fclose(f);
	if (rc < 0) {
		if (!err[0])
			snprintf(err, ecap, "That picture cannot be read.");
		sc_free(&sc);
		return -1;
	}
	sc_flush(&sc);
	/* a source with fewer rows than expected leaves the rest black: fine */
	char dir[512], base[48], path[640];
	user_dir(dir, sizeof dir);
	mkdirs(dir);
	sane_name(base, sizeof base, src);
	snprintf(name, ncap, "%s", base);
	for (int i = 2;; i++) {
		snprintf(path, sizeof path, "%s/%s.flw", dir, name);
		struct stat s2;
		if (stat(path, &s2) < 0)
			break;
		if (i > 99)
			break;
		snprintf(name, ncap, "%s-%d", base, i);
	}
	rc = flw_write(path, sc.planes, sc.dw, sc.dh);
	sc_free(&sc);
	if (rc < 0) {
		snprintf(err, ecap, "Cannot save the wallpaper in %s: %s.", dir, strerror(errno));
		return -1;
	}
	return 0;
}

int wp_set_from_file(const char *src, char *err, size_t ecap)
{
	char name[64], val[80];
	if (wp_import_file(src, name, sizeof name, err, ecap) < 0)
		return -1;
	snprintf(val, sizeof val, "user:%s", name);
	if (fl_conf_set("wallpaper", val) < 0) {
		snprintf(err, ecap, "Cannot write desktop.conf.");
		return -1;
	}
	return 0;
}

/* ---- the section ---------------------------------------------------------------------------------- */

static void tiles_clear(struct wallpaper *w)
{
	for (int i = 0; i < w->n; i++)
		free(w->tiles[i].thumb);
	w->n = 0;
}

static void tiles_scan(struct wallpaper *w)
{
	tiles_clear(w);
	for (int i = 0; i < FL_ACCENT_COUNT; i++) {
		struct tile *t = &w->tiles[w->n++];
		memset(t, 0, sizeof *t);
		snprintf(t->value, sizeof t->value, "%s", fl_accents[i].id);
		snprintf(t->label, sizeof t->label, "%s", fl_accents[i].name);
		if (i == 0)
			snprintf(t->file, sizeof t->file, "%s/wallpaper.flw", share_dir());
		else
			snprintf(t->file, sizeof t->file, "%s/wallpaper-%s.flw", share_dir(), fl_accents[i].id);
		t->thumb = flw_thumb(t->file);
	}
	char dir[512];
	user_dir(dir, sizeof dir);
	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && (e = readdir(d)) && w->n < MAXTILES) {
		size_t n = strlen(e->d_name);
		if (n < 5 || n > 44 || strcmp(e->d_name + n - 4, ".flw"))
			continue;
		struct tile *t = &w->tiles[w->n];
		memset(t, 0, sizeof *t);
		snprintf(t->label, sizeof t->label, "%.*s", (int)n - 4, e->d_name);
		snprintf(t->value, sizeof t->value, "user:%s", t->label);
		snprintf(t->file, sizeof t->file, "%s/%s", dir, e->d_name);
		t->user = 1;
		t->thumb = flw_thumb(t->file);
		if (t->thumb)
			w->n++;
	}
	if (d)
		closedir(d);
	/* imported ones in name order */
	for (int i = FL_ACCENT_COUNT; i < w->n; i++)
		for (int j = i + 1; j < w->n; j++)
			if (strcmp(w->tiles[j].label, w->tiles[i].label) < 0) {
				struct tile t = w->tiles[i];
				w->tiles[i] = w->tiles[j];
				w->tiles[j] = t;
			}
}

struct wallpaper *wp_new(fl_app *app, fl_font *head, fl_font *small)
{
	struct wallpaper *w = calloc(1, sizeof *w);
	if (!w)
		return NULL;
	w->app = app;
	w->head = head;
	w->small = small;
	return w;	/* the tiles are read when the page is shown */
}

void wp_free(struct wallpaper *w)
{
	if (!w)
		return;
	tiles_clear(w);
	free(w);
}

void wp_set_win(struct wallpaper *w, fl_win *win)
{
	w->win = win;
}

void wp_show(struct wallpaper *w)
{
	tiles_scan(w);
}

static void blit(fl_canvas *cv, int x, int y, const uint32_t *px, int w, int h)
{
	if (!cv->px)
		return;
	if (!px) {	/* a picture that could not be read (an accent's file missing from the install): an empty tile */
		fl_fill_rect(cv, fl_r(x, y, w, h), fl_th.raised);
		return;
	}
	for (int j = 0; j < h; j++) {
		int yy = y + j;
		if (yy < cv->clip.y || yy >= cv->clip.y + cv->clip.h || yy < 0 || yy >= cv->h)
			continue;
		for (int i = 0; i < w; i++) {
			int xx = x + i;
			if (xx < cv->clip.x || xx >= cv->clip.x + cv->clip.w || xx < 0 || xx >= cv->w)
				continue;
			cv->px[(size_t)yy * cv->stride + xx] = px[j * w + i];
		}
	}
}

static void remove_user(struct wallpaper *w, const struct tile *t)
{
	unlink(t->file);
	if (!strcmp(fl_conf_get("wallpaper", "accent"), t->value))
		fl_conf_set("wallpaper", "accent");
	tiles_scan(w);
}

int wp_draw(struct wallpaper *w, fl_ui *ui, int x, int y, int ww)
{
	enum { ROWH = 56 };
	fl_text(&ui->cv, w->head, x, y, fl_th.dim, "Wallpaper", -1);
	fl_fill_rect(&ui->cv, fl_r(x, y + 20, ww, 1), fl_th.line);
	y += 28;

	const char *cur = fl_conf_get("wallpaper", "accent");
	int match = !strcmp(cur, "accent");
	/* the row: follow the accent */
	{
		int th = fl_font_height(ui->f_med), sh = fl_font_height(w->small);
		int top = y + (ROWH - th - 2 - sh) / 2;
		fl_text(&ui->cv, ui->f_med, x, top, fl_th.text, "Match the accent colour", -1);
		fl_text(&ui->cv, w->small, x, top + th + 2, fl_th.dim, "The wallpaper's F changes colour with the accent", -1);
		int on = match;
		if (fl_switch(ui, ID_WP_MATCH, fl_r(x + ww - 40, y + 18, 40, 20), &on))
			fl_conf_set("wallpaper", on ? "accent" : fl_conf_get("accent", "green"));
		y += ROWH;
	}
	/* which tile is in use */
	const char *acc = fl_conf_get("accent", "green");
	int active = -1;
	for (int i = 0; i < w->n; i++)
		if (!strcmp(w->tiles[i].value, match ? acc : cur))
			active = i;
	if (active < 0 && match)
		active = 0;

	int gap = 12, cols = (ww + gap) / (TW + gap);
	if (cols < 1)
		cols = 1;
	int cell_h = TH + 28;
	for (int i = 0; i < w->n; i++) {
		struct tile *t = &w->tiles[i];
		fl_rect r = fl_r(x + (i % cols) * (TW + gap), y + (i / cols) * (cell_h + 4), TW, TH);
		int res = fl_widget(ui, ID_WP_TILE + (uint32_t)i, r, FL_FOCUSABLE | FL_NO_FOCUS_RING);
		blit(&ui->cv, r.x, r.y, t->thumb, TW, TH);
		int sel = i == active;
		if (sel)
			fl_stroke_rect(&ui->cv, fl_inset(r, -3, -3), 2, fl_th.accent);
		else if (res & (FL_HOVER | FL_FOCUS))
			fl_stroke_rect(&ui->cv, fl_inset(r, -3, -3), 1, (res & FL_FOCUS) ? fl_th.accent : fl_th.dim);
		char fit[48];
		fl_text_fit(w->small, t->label, TW, fit, sizeof fit);
		fl_text(&ui->cv, w->small, r.x, r.y + TH + 6, sel ? fl_th.strong : fl_th.dim, fit, -1);
		int pick = res & FL_CLICK;
		if ((res & FL_FOCUS) && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_space)) {
			ui->key = 0;
			pick = 1;
		}
		if (pick)
			fl_conf_set("wallpaper", t->value);
	}
	y += ((w->n - 1) / cols + 1) * (cell_h + 4) + 8;

	/* import */
	fl_text(&ui->cv, w->small, x, y, fl_th.dim, "Use your own picture (PNG or JPEG): type or paste its path", -1);
	y += 22;
	fl_rect field = fl_r(x, y, ww - 100 - 8 - (active >= FL_ACCENT_COUNT ? 92 : 0), 32);
	int r = fl_textfield(ui, ID_WP_PATH, field, w->path, sizeof w->path, "/usr/you/pictures/photo.jpg", NULL);
	int go = fl_button(ui, ID_WP_IMPORT, fl_r(field.x + field.w + 8, y, 100, 32), "Use picture", FL_BTN_ACCENT);
	if (active >= FL_ACCENT_COUNT &&
	    fl_button(ui, ID_WP_REMOVE, fl_r(field.x + field.w + 8 + 100 + 8 - 0, y, 84, 32), "Remove", FL_BTN_OUTLINE) && !match) {
		/* the picture in use goes */
		remove_user(w, &w->tiles[active]);
		snprintf(w->msg, sizeof w->msg, "Removed.");
		w->msg_err = 0;
		fl_win_redraw(w->win);
	}
	if (go || (r & FL_EDIT_ENTER)) {
		char p[512];
		snprintf(p, sizeof p, "%s", w->path);
		size_t n = strlen(p);
		while (n && (p[n - 1] == ' ' || p[n - 1] == '\n'))
			p[--n] = 0;
		char *s = p;
		while (*s == ' ')
			s++;
		if (!*s) {
			snprintf(w->msg, sizeof w->msg, "Type the path of a picture first.");
			w->msg_err = 1;
		} else if (s[0] == '~' && s[1] == '/' && getenv("HOME")) {
			char q[1100];
			snprintf(q, sizeof q, "%s/%s", getenv("HOME"), s + 2);
			snprintf(p, sizeof p, "%s", q);
			s = p;
		}
		if (*s) {
			char err[200] = "";
			if (wp_set_from_file(s, err, sizeof err) == 0) {
				snprintf(w->msg, sizeof w->msg, "The wallpaper is set.");
				w->msg_err = 0;
				w->path[0] = 0;
				tiles_scan(w);
			} else {
				snprintf(w->msg, sizeof w->msg, "%s", err);
				w->msg_err = 1;
			}
		}
	}
	y += 32 + 8;
	if (w->msg[0]) {
		fl_text_in(&ui->cv, w->small, fl_r(x, y, ww, 20), FL_LEFT, 0, w->msg_err ? fl_th.err : fl_th.dim, w->msg);
		y += 22;
	}
	return y + 12;
}
