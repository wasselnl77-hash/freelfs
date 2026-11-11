/* img.c -- decode PNG, JPEG and WebP for the store, scaled down at once
 * SPDX-License-Identifier: MIT
 *
 * Icons and screenshots come from Flathub as PNG (some as JPEG or WebP). They are decoded, box-filtered
 * down to the size they are shown at and kept premultiplied, so a screenshot costs w*h*4 bytes of the
 * shown size, not of the original, and drawing is a plain blend.
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>

#include "store.h"

#define MAX_PIXELS (40u << 20) /* a picture bigger than this is refused (memory) */

struct raw {
	int w, h;
	uint8_t *rgba; /* straight alpha, w*h*4 */
};

static int read_file(const char *path, uint8_t **data, size_t *len)
{
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return -1;
	fseek(fp, 0, SEEK_END);
	long n = ftell(fp);
	rewind(fp);
	if (n < 16 || n > (64L << 20)) {
		fclose(fp);
		return -1;
	}
	*data = malloc((size_t)n);
	if (!*data || fread(*data, 1, (size_t)n, fp) != (size_t)n) {
		free(*data);
		fclose(fp);
		return -1;
	}
	fclose(fp);
	*len = (size_t)n;
	return 0;
}

/* ---- PNG ---------------------------------------------------------------------------------- */

struct memrd {
	const uint8_t *p;
	size_t n, pos;
};

static void png_mem_read(png_structp png, png_bytep out, size_t n)
{
	struct memrd *m = png_get_io_ptr(png);
	if (m->pos + n > m->n)
		png_error(png, "short");
	memcpy(out, m->p + m->pos, n);
	m->pos += n;
}

static int dec_png(const uint8_t *d, size_t n, struct raw *r)
{
	png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	if (!png)
		return -1;
	png_infop info = png_create_info_struct(png);
	uint8_t *buf = NULL;
	png_bytep *rows = NULL;
	if (!info || setjmp(png_jmpbuf(png))) {
		png_destroy_read_struct(&png, &info, NULL);
		free(buf);
		free(rows);
		return -1;
	}
	struct memrd m = {d, n, 0};
	png_set_read_fn(png, &m, png_mem_read);
	png_read_info(png, info);
	png_uint_32 w = png_get_image_width(png, info), h = png_get_image_height(png, info);
	if (!w || !h || (uint64_t)w * h > MAX_PIXELS)
		png_error(png, "size");
	int ct = png_get_color_type(png, info), bd = png_get_bit_depth(png, info);
	if (bd == 16)
		png_set_strip_16(png);
	if (ct == PNG_COLOR_TYPE_PALETTE)
		png_set_palette_to_rgb(png);
	if (ct == PNG_COLOR_TYPE_GRAY && bd < 8)
		png_set_expand_gray_1_2_4_to_8(png);
	if (png_get_valid(png, info, PNG_INFO_tRNS))
		png_set_tRNS_to_alpha(png);
	if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA)
		png_set_gray_to_rgb(png);
	png_set_filler(png, 0xff, PNG_FILLER_AFTER);
	png_set_interlace_handling(png);
	png_read_update_info(png, info);
	buf = malloc((size_t)w * h * 4);
	rows = malloc(h * sizeof *rows);
	if (!buf || !rows)
		png_error(png, "memory");
	for (png_uint_32 y = 0; y < h; y++)
		rows[y] = buf + (size_t)y * w * 4;
	png_read_image(png, rows);
	png_destroy_read_struct(&png, &info, NULL);
	free(rows);
	r->w = (int)w;
	r->h = (int)h;
	r->rgba = buf;
	return 0;
}

/* ---- JPEG ---------------------------------------------------------------------------------- */

struct jerr {
	struct jpeg_error_mgr pub;
	jmp_buf jb;
};

static void jerr_exit(j_common_ptr c)
{
	longjmp(((struct jerr *)c->err)->jb, 1);
}

static int dec_jpeg(const uint8_t *d, size_t n, int want_w, int want_h, struct raw *r)
{
	struct jpeg_decompress_struct cinfo;
	struct jerr je;
	uint8_t *buf = NULL;
	cinfo.err = jpeg_std_error(&je.pub);
	je.pub.error_exit = jerr_exit;
	if (setjmp(je.jb)) {
		jpeg_destroy_decompress(&cinfo);
		free(buf);
		return -1;
	}
	jpeg_create_decompress(&cinfo);
	jpeg_mem_src(&cinfo, d, n);
	jpeg_read_header(&cinfo, TRUE);
	/* let the decoder shrink by 2, 4 or 8 while it is at it, as long as the result is still big enough */
	cinfo.scale_num = 1;
	cinfo.scale_denom = 1;
	for (int s = 8; s >= 2; s /= 2)
		if ((int)cinfo.image_width / s >= want_w && (int)cinfo.image_height / s >= want_h) {
			cinfo.scale_denom = (unsigned)s;
			break;
		}
	cinfo.out_color_space = JCS_RGB;
	jpeg_start_decompress(&cinfo);
	int w = (int)cinfo.output_width, h = (int)cinfo.output_height;
	if (w <= 0 || h <= 0 || (uint64_t)w * (uint64_t)h > MAX_PIXELS)
		longjmp(je.jb, 1);
	buf = malloc((size_t)w * h * 4);
	uint8_t *row = malloc((size_t)w * 3);
	if (!buf || !row) {
		free(row);
		longjmp(je.jb, 1);
	}
	while ((int)cinfo.output_scanline < h) {
		uint8_t *line = row;
		jpeg_read_scanlines(&cinfo, &line, 1);
		uint8_t *o = buf + (size_t)(cinfo.output_scanline - 1) * w * 4;
		for (int x = 0; x < w; x++) {
			o[x * 4] = row[x * 3];
			o[x * 4 + 1] = row[x * 3 + 1];
			o[x * 4 + 2] = row[x * 3 + 2];
			o[x * 4 + 3] = 0xff;
		}
	}
	free(row);
	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	r->w = w;
	r->h = h;
	r->rgba = buf;
	return 0;
}

static int dec_webp(const uint8_t *d, size_t n, struct raw *r)
{
	int w, h;
	if (!WebPGetInfo(d, n, &w, &h) || w <= 0 || h <= 0 || (uint64_t)w * (uint64_t)h > MAX_PIXELS)
		return -1;
	uint8_t *px = WebPDecodeRGBA(d, n, &w, &h);
	if (!px)
		return -1;
	r->rgba = malloc((size_t)w * h * 4);
	if (!r->rgba) {
		WebPFree(px);
		return -1;
	}
	memcpy(r->rgba, px, (size_t)w * h * 4);
	WebPFree(px);
	r->w = w;
	r->h = h;
	return 0;
}

/* ---- scaling ------------------------------------------------------------------------------- */

/* Box filter: every target pixel is the average of the source area it covers (alpha-weighted). */
static simg *scale_to(const struct raw *r, int tw, int th)
{
	simg *im = malloc(sizeof *im + (size_t)tw * th * 4);
	if (!im)
		return NULL;
	im->w = tw;
	im->h = th;
	for (int y = 0; y < th; y++) {
		int y0 = (int)((int64_t)y * r->h / th), y1 = (int)((int64_t)(y + 1) * r->h / th);
		if (y1 <= y0)
			y1 = y0 + 1;
		for (int x = 0; x < tw; x++) {
			int x0 = (int)((int64_t)x * r->w / tw), x1 = (int)((int64_t)(x + 1) * r->w / tw);
			if (x1 <= x0)
				x1 = x0 + 1;
			uint64_t sr = 0, sg = 0, sb = 0, sa = 0, cnt = 0;
			for (int yy = y0; yy < y1 && yy < r->h; yy++) {
				const uint8_t *p = r->rgba + ((size_t)yy * r->w + x0) * 4;
				for (int xx = x0; xx < x1 && xx < r->w; xx++, p += 4) {
					uint32_t a = p[3];
					sr += p[0] * a;
					sg += p[1] * a;
					sb += p[2] * a;
					sa += a;
					cnt++;
				}
			}
			uint32_t a = cnt ? (uint32_t)(sa / cnt) : 0;
			uint32_t rr = sa ? (uint32_t)(sr / (cnt * 255)) : 0;
			uint32_t gg = sa ? (uint32_t)(sg / (cnt * 255)) : 0;
			uint32_t bb = sa ? (uint32_t)(sb / (cnt * 255)) : 0;
			im->px[(size_t)y * tw + x] = a << 24 | rr << 16 | gg << 8 | bb;
		}
	}
	return im;
}

simg *simg_load(const char *path, int max_w, int max_h)
{
	uint8_t *d;
	size_t n;
	if (max_w < 1 || max_h < 1 || read_file(path, &d, &n) < 0)
		return NULL;
	struct raw r = {0};
	int rc = -1;
	if (n > 8 && !memcmp(d, "\x89PNG", 4))
		rc = dec_png(d, n, &r);
	else if (d[0] == 0xff && d[1] == 0xd8)
		rc = dec_jpeg(d, n, max_w, max_h, &r);
	else if (n > 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4))
		rc = dec_webp(d, n, &r);
	free(d);
	if (rc < 0)
		return NULL;
	int tw = r.w, th = r.h;
	if (tw > max_w || th > max_h) {
		if ((int64_t)max_w * th <= (int64_t)max_h * tw) {
			th = (int)((int64_t)th * max_w / tw);
			tw = max_w;
		} else {
			tw = (int)((int64_t)tw * max_h / th);
			th = max_h;
		}
		if (tw < 1)
			tw = 1;
		if (th < 1)
			th = 1;
	}
	simg *im;
	if (tw == r.w && th == r.h) {
		im = malloc(sizeof *im + (size_t)tw * th * 4);
		if (im) {
			im->w = tw;
			im->h = th;
			for (size_t i = 0; i < (size_t)tw * th; i++) {
				const uint8_t *p = r.rgba + i * 4;
				uint32_t a = p[3];
				im->px[i] = a << 24 | (uint32_t)(p[0] * a / 255) << 16 | (uint32_t)(p[1] * a / 255) << 8 |
				            (uint32_t)(p[2] * a / 255);
			}
		}
	} else {
		im = scale_to(&r, tw, th);
	}
	free(r.rgba);
	return im;
}

void simg_draw(fl_canvas *cv, const simg *im, int x, int y)
{
	if (!cv->px || !im)
		return;
	fl_rect vis = fl_intersect(fl_r(x, y, im->w, im->h), cv->clip);
	for (int yy = vis.y; yy < vis.y + vis.h; yy++) {
		const uint32_t *s = im->px + (size_t)(yy - y) * im->w + (vis.x - x);
		uint32_t *o = cv->px + (size_t)yy * cv->stride + vis.x;
		for (int xx = 0; xx < vis.w; xx++, s++, o++) {
			uint32_t p = *s, a = p >> 24;
			if (a == 0xff) {
				*o = p;
			} else if (a) {
				uint32_t d = *o, ia = 255 - a;
				uint32_t r = ((p >> 16) & 0xff) + (((d >> 16) & 0xff) * ia + 127) / 255;
				uint32_t g = ((p >> 8) & 0xff) + (((d >> 8) & 0xff) * ia + 127) / 255;
				uint32_t b = (p & 0xff) + ((d & 0xff) * ia + 127) / 255;
				*o = 0xff000000u | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
			}
		}
	}
}
