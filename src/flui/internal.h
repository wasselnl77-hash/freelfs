/* internal.h -- libflui internals
 * SPDX-License-Identifier: MIT
 */
#ifndef FLUI_INTERNAL_H
#define FLUI_INTERNAL_H

#include "flui.h"
#include <stdint.h>

#define HIDDEN __attribute__((visibility("hidden")))

/* Blend straight-alpha colour c (alpha scaled by cov, 0-255) onto a pixel. */
static inline uint32_t fl_over(uint32_t d, fl_color c, unsigned cov)
{
	unsigned a = (c >> 24) * cov / 255;
	if (a == 255)
		return c | 0xff000000u;
	if (a == 0)
		return d;
	unsigned ia = 255 - a;
	unsigned r = ((c >> 16 & 255) * a + (d >> 16 & 255) * ia) / 255;
	unsigned g = ((c >> 8 & 255) * a + (d >> 8 & 255) * ia) / 255;
	unsigned b = ((c & 255) * a + (d & 255) * ia) / 255;
	unsigned al = a + (d >> 24) * ia / 255;
	return al << 24 | r << 16 | g << 8 | b;
}

HIDDEN void fl_blend_px(fl_canvas *cv, int x, int y, fl_color c, unsigned cov);

/* Opens the shared fontconfig/freetype state and the UI fonts. */
HIDDEN int fl_text_init(void);
HIDDEN fl_app *fl_win_app(fl_win *w);

#endif
