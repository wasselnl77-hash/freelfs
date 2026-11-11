/* ops_test -- the annotation tools on in-memory pictures; no display, no compositor.
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../img.h"

static int fails;
#define CHECK(cond, msg) do { if (cond) printf("ok   %s\n", msg); else { printf("FAIL %s\n", msg); fails++; } } while (0)

static img_t flat(int w, int h, uint32_t c)
{
	img_t im = img_new(w, h);
	for (int i = 0; i < w * h; i++)
		im.px[i] = 0xff000000u | c;
	return im;
}

static int count(const img_t *im, uint32_t c)
{
	int n = 0;
	for (int i = 0; i < im->w * im->h; i++)
		n += (im->px[i] & 0xffffff) == c;
	return n;
}

int main(void)
{
	img_t im = flat(200, 120, 0x202020);

	struct op box = { .kind = OP_BOX, .x1 = 20, .y1 = 20, .x2 = 100, .y2 = 80, .col = 0xe5484d, .width = 2 };
	op_apply(&im, &box);
	CHECK((im.px[20 * 200 + 50] & 0xffffff) == 0xe5484d, "box: the top edge is in its colour");
	CHECK((im.px[50 * 200 + 50] & 0xffffff) == 0x202020, "box: the inside is untouched");
	CHECK(count(&im, 0xe5484d) > 400 && count(&im, 0xe5484d) < 1200, "box: a frame, not a fill");
	struct op boxr = box;
	boxr.x1 = 100; boxr.y1 = 80; boxr.x2 = 20; boxr.y2 = 20;
	img_t im2 = flat(200, 120, 0x202020);
	op_apply(&im2, &boxr);
	CHECK(memcmp(im.px, im2.px, (size_t)200 * 120 * 4) == 0, "box: dragged backwards gives the same box");
	img_free(&im2);

	img_t a = flat(200, 120, 0x202020);
	struct op arrow = { .kind = OP_ARROW, .x1 = 20, .y1 = 100, .x2 = 150, .y2 = 30, .col = 0x3e8bff, .width = 3 };
	op_apply(&a, &arrow);
	int n = count(&a, 0x3e8bff);
	CHECK(n > 300, "arrow: line and head are drawn");
	int tip = 0;
	for (int y = 26; y <= 34; y++)
		for (int x = 144; x <= 154; x++)
			tip += (a.px[y * 200 + x] & 0xff) > 0x60;
	CHECK(tip > 4, "arrow: the tip is where the drag ended");
	CHECK((a.px[110 * 200 + 5] & 0xffffff) == 0x202020, "arrow: far away is untouched");
	img_free(&a);

	img_t h = flat(100, 50, 0x000000);
	struct op hi = { .kind = OP_HILITE, .x1 = 10, .y1 = 10, .x2 = 60, .y2 = 30, .col = 0xf5c542, .width = 3 };
	op_apply(&h, &hi);
	uint32_t p = h.px[20 * 100 + 30];
	CHECK(((p >> 16) & 255) > 60 && ((p >> 16) & 255) < 200, "highlight: blends, does not cover");
	CHECK((h.px[5 * 100 + 5] & 0xffffff) == 0, "highlight: outside is untouched");
	img_free(&h);

	/* a checkerboard: blur averages it, pixelate makes blocks */
	img_t c = img_new(64, 64);
	for (int y = 0; y < 64; y++)
		for (int x = 0; x < 64; x++)
			c.px[y * 64 + x] = 0xff000000u | (((x + y) & 1) ? 0xffffff : 0x000000);
	img_t d = img_copy(&c);
	struct op bl = { .kind = OP_BLUR, .x1 = 8, .y1 = 8, .x2 = 56, .y2 = 56, .col = 0, .width = 1 };
	op_apply(&c, &bl);
	uint32_t m = c.px[32 * 64 + 32] & 255;
	CHECK(m > 90 && m < 170, "blur: the checkerboard turns grey");
	CHECK((c.px[2 * 64 + 2] & 0xffffff) == 0x000000 && (c.px[2 * 64 + 3] & 0xffffff) == 0xffffff, "blur: outside the area it stays sharp");
	struct op px = { .kind = OP_PIXEL, .x1 = 8, .y1 = 8, .x2 = 56, .y2 = 56, .col = 0, .width = 1 };
	op_apply(&d, &px);
	CHECK(d.px[10 * 64 + 10] == d.px[11 * 64 + 11] && d.px[10 * 64 + 10] == d.px[12 * 64 + 9], "pixelate: a block is one colour");
	CHECK((d.px[2 * 64 + 3] & 0xffffff) == 0xffffff, "pixelate: outside the area it stays sharp");
	img_free(&c);
	img_free(&d);

	struct op tx = { .kind = OP_TEXT, .x1 = 10, .y1 = 10, .col = 0xffffff, .width = 3 };
	snprintf(tx.text, sizeof tx.text, "Hello");
	int tw, th;
	op_text_size(&tx, &tw, &th);
	CHECK(tw > 20 && th > 12, "text: it has a size");
	img_t t = flat(200, 60, 0x202020);
	op_apply(&t, &tx);
	CHECK(count(&t, 0xffffff) > 30, "text: pixels in the text colour");
	img_free(&t);

	struct op cr = { .kind = OP_CROP, .x1 = 100, .y1 = 80, .x2 = 40, .y2 = 30, .col = 0, .width = 1 };
	op_apply(&im, &cr);
	CHECK(im.w == 60 && im.h == 50, "crop: the selection is the new size, whichever way it was dragged");
	struct op cr2 = { .kind = OP_CROP, .x1 = -50, .y1 = -50, .x2 = 20, .y2 = 20, .col = 0, .width = 1 };
	op_apply(&im, &cr2);
	CHECK(im.w == 20 && im.h == 20, "crop: the part outside the picture is cut off");

	char path[128];
	snprintf(path, sizeof path, "/tmp/snappel-ops-%d.png", (int)getpid());
	CHECK(img_save_png(&im, path) == 0, "png: written");
	FILE *f = fopen(path, "rb");
	unsigned char magic[8] = { 0 };
	if (f) {
		if (fread(magic, 1, 8, f) != 8)
			magic[0] = 0;
		fclose(f);
	}
	CHECK(!memcmp(magic, "\x89PNG\r\n\x1a\n", 8), "png: has the signature");
	unlink(path);
	img_free(&im);

	printf(fails ? "%d test(s) failed\n" : "all tests passed\n", fails);
	return fails != 0;
}
