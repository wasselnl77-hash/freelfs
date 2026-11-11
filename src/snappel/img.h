/* img.h -- Snappel's pictures and the annotations on them
 * SPDX-License-Identifier: MIT */
#ifndef SNAPPEL_IMG_H
#define SNAPPEL_IMG_H

#include <stdint.h>

/* opaque XRGB pixels (the alpha byte is 0xff) */
typedef struct { int w, h; uint32_t *px; } img_t;

img_t img_new(int w, int h);
void img_free(img_t *im);
img_t img_copy(const img_t *im);
img_t img_crop(const img_t *im, int x, int y, int w, int h);
int img_save_png(const img_t *im, const char *path); /* 0, or -1 */

enum { OP_ARROW, OP_BOX, OP_TEXT, OP_HILITE, OP_BLUR, OP_PIXEL, OP_CROP };

struct op {
	int kind;
	int x1, y1, x2, y2; /* picture pixels; text: x1, y1 is the top left of the text; crop: the kept part */
	uint32_t col;       /* 0xRRGGBB */
	int width;          /* 1..6: line width, text size, blur strength */
	char text[160];
};

/* Draw one annotation into the picture. A crop returns the smaller picture in *im (the old one
 * is freed); every other kind draws in place. */
void op_apply(img_t *im, const struct op *o);
/* The text size in pixels for a width setting */
int op_text_px(int width);
/* The width and height the text takes (for a caret and a box in the editor) */
void op_text_size(const struct op *o, int *w, int *h);

#endif
