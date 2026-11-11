/* tabstrip -- the FreeBWS tab row, drawn with libflui.
 * One flat row above the toolbar: the active tab is the toolbar's colour with an
 * accent strip on top, the others sit on the window background. No state of its
 * own except what the caller passes in; it reports what the user did.
 * SPDX-License-Identifier: MIT */
#ifndef FB_TABSTRIP_H
#define FB_TABSTRIP_H

#include <flui.h>

#define TABS_H 34

struct ts_item {
	const char *title;	/* what to show (never empty) */
	int loading;		/* a spinner in front of the title */
	int discarded;		/* background tab without a web process: dimmer */
};

struct ts_event {
	int select;		/* tab index clicked, or -1 */
	int close;		/* tab index whose close button or middle button was used, or -1 */
	int newtab;		/* the + button */
};

/* Draw the row into r (height TABS_H). `spin` is the spinner's angle in degrees.
 * Widget ids 0x1000 .. 0x3fff are used. */
struct ts_event ts_draw(fl_ui *ui, fl_rect r, const struct ts_item *it, int n, int active, int spin);

#endif
