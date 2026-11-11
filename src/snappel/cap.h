/* cap.h -- talking to the compositor: fl_screencopy (pictures of the screen) and the clipboard
 * SPDX-License-Identifier: MIT */
#ifndef SNAPPEL_CAP_H
#define SNAPPEL_CAP_H

#include <stdint.h>

#include "img.h"

struct capwin {
	uint32_t id;
	int x, y, w, h;
	char app[48], title[96];
};

/* a second connection of its own, so a capture is a plain blocking call whatever the window
 * code is doing; 0, or -1 when there is no compositor or it has no fl_screencopy */
int cap_open(void);
void cap_close(void);
int cap_output(img_t *out);			/* the whole desktop, without Snappel's own windows; 0 or -1 */
int cap_window(uint32_t id, img_t *out);	/* one window, even when others cover it */
int cap_windows(struct capwin *w, int max);	/* the windows in view, topmost first */

/* Put the picture on the clipboard as image/png. Forks a small server that holds it until
 * something else is copied (like wl-copy), so it outlives Snappel. 0, or -1. */
int clip_copy(const img_t *im);

#endif
