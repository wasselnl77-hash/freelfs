/* img.c -- pictures, PNG output and the annotation tools: arrow, box, text, highlight, blur,
 * pixelate, crop. The tools draw with libflui's canvas calls into the picture itself.
 * SPDX-License-Identifier: MIT */
#include "img.h"

#include <flui.h>
#include <math.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

img_t img_new(int w, int h)
{
	img_t im = { 0, 0, NULL };
	if (w < 1 || h < 1 || (size_t)w * h > (size_t)1 << 28)
		return im;
	im.px = calloc((size_t)w * h, 4);
	if (im.px) {
		im.w = w;
		im.h = h;
	}
	return im;
}

void img_free(img_t *im)
{
	free(im->px);
	im->px = NULL;
	im->w = im->h = 0;
}

img_t img_copy(const img_t *im)
{
	img_t c = img_new(im->w, im->h);
	if (c.px)
		memcpy(c.px, im->px, (size_t)im->w * im->h * 4);
	return c;
}

img_t img_crop(const img_t *im, int x, int y, int w, int h)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > im->w) w = im->w - x;
	if (y + h > im->h) h = im->h - y;
	img_t c = img_new(w, h);
	for (int j = 0; c.px && j < c.h; j++)
		memcpy(c.px + (size_t)j * c.w, im->px + (size_t)(y + j) * im->w + x, (size_t)c.w * 4);
	return c;
}

int img_save_png(const img_t *im, const char *path)
{
	char tmp[1100];
	snprintf(tmp, sizeof tmp, "%s.part", path);
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return -1;
	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_infop info = png ? png_create_info_struct(png) : NULL;
	unsigned char *row = malloc((size_t)im->w * 3);
	if (!png || !info || !row || setjmp(png_jmpbuf(png))) {
		png_destroy_write_struct(&png, &info);
		free(row);
		fclose(f);
		unlink(tmp);
		return -1;
	}
	png_init_io(png, f);
	png_set_compression_level(png, 4);
	png_set_IHDR(png, info, (png_uint_32)im->w, (png_uint_32)im->h, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
		     PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png, info);
	for (int y = 0; y < im->h; y++) {
		const uint32_t *p = im->px + (size_t)y * im->w;
		for (int x = 0; x < im->w; x++) {
			row[x * 3] = (p[x] >> 16) & 0xff;
			row[x * 3 + 1] = (p[x] >> 8) & 0xff;
			row[x * 3 + 2] = p[x] & 0xff;
		}
		png_write_row(png, row);
	}
	png_write_end(png, info);
	png_destroy_write_struct(&png, &info);
	free(row);
	if (fclose(f) != 0 || rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

/* ---- the tools ---------------------------------------------------------------------------- */

static fl_canvas canvas(img_t *im)
{
	fl_canvas cv = { im->px, im->w, im->h, im->w, { 0, 0, im->w, im->h } };
	return cv;
}

static fl_color rgb(uint32_t c, unsigned a)
{
	return (a << 24) | (c & 0xffffff);
}

static void norm(const struct op *o, int *x1, int *y1, int *x2, int *y2)
{
	*x1 = o->x1 < o->x2 ? o->x1 : o->x2;
	*x2 = o->x1 < o->x2 ? o->x2 : o->x1;
	*y1 = o->y1 < o->y2 ? o->y1 : o->y2;
	*y2 = o->y1 < o->y2 ? o->y2 : o->y1;
}

static void clip_rect(const img_t *im, int *x1, int *y1, int *x2, int *y2)
{
	if (*x1 < 0) *x1 = 0;
	if (*y1 < 0) *y1 = 0;
	if (*x2 > im->w) *x2 = im->w;
	if (*y2 > im->h) *y2 = im->h;
}

static void arrow(img_t *im, const struct op *o)
{
	fl_canvas cv = canvas(im);
	float x1 = (float)o->x1, y1 = (float)o->y1, x2 = (float)o->x2, y2 = (float)o->y2;
	float dx = x2 - x1, dy = y2 - y1, len = sqrtf(dx * dx + dy * dy);
	if (len < 2.0f)
		return;
	float ux = dx / len, uy = dy / len, w = (float)o->width;
	float head = 5.0f * w + 10.0f;
	if (head > len * 0.8f)
		head = len * 0.8f;
	float half = head * 0.5f;
	float bx = x2 - ux * head, by = y2 - uy * head;	/* the base of the head */
	fl_path *sh = fl_path_new(), *hd = fl_path_new();
	if (sh) {
		fl_path_move(sh, x1, y1);
		fl_path_line(sh, bx + ux * 1.5f, by + uy * 1.5f);
		fl_path_stroke(&cv, sh, w + 1.0f, rgb(o->col, 255));
		fl_path_free(sh);
	}
	if (hd) {
		fl_path_move(hd, x2, y2);
		fl_path_line(hd, bx - uy * half, by + ux * half);
		fl_path_line(hd, bx + uy * half, by - ux * half);
		fl_path_close(hd);
		fl_path_fill(&cv, hd, rgb(o->col, 255));
		fl_path_free(hd);
	}
}

static void box(img_t *im, const struct op *o)
{
	fl_canvas cv = canvas(im);
	int x1, y1, x2, y2;
	norm(o, &x1, &y1, &x2, &y2);
	int t = o->width + 1;
	if (x2 - x1 < 2 * t) x2 = x1 + 2 * t;
	if (y2 - y1 < 2 * t) y2 = y1 + 2 * t;
	fl_stroke_rect(&cv, fl_r(x1, y1, x2 - x1, y2 - y1), t, rgb(o->col, 255));
}

static void hilite(img_t *im, const struct op *o)
{
	fl_canvas cv = canvas(im);
	int x1, y1, x2, y2;
	norm(o, &x1, &y1, &x2, &y2);
	fl_fill_rect(&cv, fl_r(x1, y1, x2 - x1, y2 - y1), rgb(o->col, 0x78));
}

int op_text_px(int width)
{
	return 12 + 4 * (width < 1 ? 1 : width > 6 ? 6 : width);
}

static fl_font *font_for(int px)
{
	static fl_font *cache[64];
	static int sizes[64];
	for (int i = 0; i < 64; i++)
		if (cache[i] && sizes[i] == px)
			return cache[i];
	for (int i = 0; i < 64; i++)
		if (!cache[i]) {
			cache[i] = fl_font_ui(600, (float)px);
			sizes[i] = px;
			return cache[i];
		}
	return NULL;
}

void op_text_size(const struct op *o, int *w, int *h)
{
	fl_font *f = font_for(op_text_px(o->width));
	*w = f ? fl_text_width(f, o->text, -1) : 0;
	*h = f ? fl_font_height(f) : 0;
}

static void text(img_t *im, const struct op *o)
{
	fl_canvas cv = canvas(im);
	fl_font *f = font_for(op_text_px(o->width));
	if (!f || !o->text[0])
		return;
	/* a thin dark edge keeps the text readable on any picture */
	uint32_t edge = (o->col & 0xffffff) == 0x111111 ? 0xffffff : 0x111111;
	fl_text(&cv, f, o->x1 + 1, o->y1 + 1, rgb(edge, 0xa0), o->text, -1);
	fl_text(&cv, f, o->x1, o->y1, rgb(o->col, 255), o->text, -1);
}

/* box blur over a rectangle, 3 passes of a horizontal and a vertical average */
static void blur(img_t *im, const struct op *o)
{
	int x1, y1, x2, y2;
	norm(o, &x1, &y1, &x2, &y2);
	clip_rect(im, &x1, &y1, &x2, &y2);
	int w = x2 - x1, h = y2 - y1, r = 3 + 3 * o->width;
	if (w < 1 || h < 1)
		return;
	uint32_t *tmp = malloc((size_t)(w > h ? w : h) * 4);
	if (!tmp)
		return;
	for (int pass = 0; pass < 3; pass++) {
		for (int dir = 0; dir < 2; dir++) {
			int n = dir ? h : w, lines = dir ? w : h;
			for (int l = 0; l < lines; l++) {
				uint32_t *base = im->px + (dir ? (size_t)y1 * im->w + x1 + l : (size_t)(y1 + l) * im->w + x1);
				size_t step = dir ? (size_t)im->w : 1;
				long sr = 0, sg = 0, sb = 0;
				int cnt = 0;
				for (int i = 0; i < n && i <= r; i++) {	/* window [i - r, i + r] clipped, edges repeat */
					uint32_t p = base[i * step];
					sr += (p >> 16) & 255; sg += (p >> 8) & 255; sb += p & 255;
					cnt++;
				}
				for (int i = 0; i < n; i++) {
					tmp[i] = 0xff000000u | (uint32_t)(sr / cnt) << 16 | (uint32_t)(sg / cnt) << 8 | (uint32_t)(sb / cnt);
					int add = i + r + 1, del = i - r;
					if (add < n) {
						uint32_t p = base[add * step];
						sr += (p >> 16) & 255; sg += (p >> 8) & 255; sb += p & 255;
						cnt++;
					}
					if (del >= 0) {
						uint32_t p = base[del * step];
						sr -= (p >> 16) & 255; sg -= (p >> 8) & 255; sb -= p & 255;
						cnt--;
					}
				}
				for (int i = 0; i < n; i++)
					base[i * step] = tmp[i];
			}
		}
	}
	free(tmp);
}

static void pixelate(img_t *im, const struct op *o)
{
	int x1, y1, x2, y2;
	norm(o, &x1, &y1, &x2, &y2);
	clip_rect(im, &x1, &y1, &x2, &y2);
	int blk = 4 + 4 * o->width;
	for (int by = y1; by < y2; by += blk)
		for (int bx = x1; bx < x2; bx += blk) {
			int ex = bx + blk < x2 ? bx + blk : x2, ey = by + blk < y2 ? by + blk : y2;
			unsigned long r = 0, g = 0, b = 0, n = 0;
			for (int y = by; y < ey; y++)
				for (int x = bx; x < ex; x++) {
					uint32_t p = im->px[(size_t)y * im->w + x];
					r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255;
					n++;
				}
			uint32_t c = 0xff000000u | (uint32_t)(r / n) << 16 | (uint32_t)(g / n) << 8 | (uint32_t)(b / n);
			for (int y = by; y < ey; y++)
				for (int x = bx; x < ex; x++)
					im->px[(size_t)y * im->w + x] = c;
		}
}

void op_apply(img_t *im, const struct op *o)
{
	switch (o->kind) {
	case OP_ARROW: arrow(im, o); break;
	case OP_BOX: box(im, o); break;
	case OP_TEXT: text(im, o); break;
	case OP_HILITE: hilite(im, o); break;
	case OP_BLUR: blur(im, o); break;
	case OP_PIXEL: pixelate(im, o); break;
	case OP_CROP: {
		int x1, y1, x2, y2;
		norm(o, &x1, &y1, &x2, &y2);
		clip_rect(im, &x1, &y1, &x2, &y2);
		if (x2 - x1 < 1 || y2 - y1 < 1)
			break;
		img_t c = img_crop(im, x1, y1, x2 - x1, y2 - y1);
		if (c.px) {
			img_free(im);
			*im = c;
		}
		break;
	}
	}
}
