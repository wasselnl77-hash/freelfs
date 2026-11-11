/* Performance mode: the compositor stays out of a fullscreen program's way.
 *
 * desktop.conf  performance=auto|on|off   (default auto; 1 and on are "on", 0 and off are "off")
 *   auto  performance mode is on while the window on top is fullscreen
 *   on    always on (the panel's power flyout writes this), whatever is on screen
 *   off   never: the frame cap of power saving stays, and nothing is skipped
 *
 * What it does (flcomp composites on the CPU into one shadow buffer, then copies the damaged
 * rectangles to the DRM buffer, so "unredirect" here means skipping work, not a scanout plane):
 *   - no frame cap: power saving (power.c) limits the frame rate to 30; performance mode wins,
 *     so frame callbacks and flips follow the display's refresh and nothing waits on a timer
 *     (power_frame_gap_ms)
 *   - unredirect: when the top window is fullscreen, opaque and covers the whole output, and
 *     nothing is drawn over it (no menu, flyout, popup, notification card, overview or lock
 *     screen), perf_unredirected() names it and render.c draws that window alone: no wallpaper
 *     fill, none of the windows below it, no panel. The switcher, the volume indicator and the
 *     pointer are still drawn over it. The moment anything else needs the screen (a
 *     notification, Alt-Tab) it is not eligible any more and the full path draws that rectangle
 *     again, so the picture is the same either way.
 * Not done: handing a client's dmabuf to a KMS plane (real direct scanout). That needs the
 * dmabuf kept alive until the next flip and an atomic-modeset backend: see docs/E2-GAMING.md.
 * SPDX-License-Identifier: MIT */
#include "comp.h"

#include <stdio.h>
#include <string.h>

int perf_parse(const char *v)
{
	if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "always"))
		return PERF_ON;
	if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "never"))
		return PERF_OFF;
	return PERF_AUTO;
}

/* the window on top of the desktop: first of the stack that is shown */
static struct window *top_visible(void)
{
	struct window *w;
	wl_list_for_each(w, &C.windows, link)
		if (window_visible(w))
			return w;
	return NULL;
}

static bool perf_now(void)
{
	if (CF.perf == PERF_OFF)
		return false;
	if (CF.perf == PERF_ON)
		return true;
	struct window *w = top_visible();
	return w && w->fullscreen;
}

/* asked whenever a frame is scheduled; the log line (flcomp.log) is for the person debugging a game */
bool perf_active(void)
{
	static bool last;
	bool now = perf_now();
	if (now != last) {
		last = now;
		fprintf(stderr, "flcomp: performance mode %s\n", now ? "on" : "off");
	}
	return now;
}

static struct window *unredirected(void)
{
	if (CF.perf == PERF_OFF || C.locked || overview_on())
		return NULL;
	struct window *w = top_visible();
	if (!w || !w->fullscreen)
		return NULL;
	struct surface *s = w->surf;
	if (!s || !s->px || s->alpha)
		return NULL;
	int ox, oy;
	surface_origin(s, &ox, &oy);
	if (ox > 0 || oy > 0 || ox + s->w < C.w || oy + s->h < C.h)
		return NULL;	/* a buffer smaller than the screen: the desktop shows around it */
	if (menu_visible() || flyout_visible() || notify_visible() || xwm_or_visible())
		return NULL;
	struct popup *p;
	wl_list_for_each(p, &C.popups, link)
		if (popup_visible(p))
			return NULL;
	return w;
}

/* render.c asks for every rectangle it draws; the log shows only the changes */
struct window *perf_unredirected(void)
{
	static bool last;
	struct window *w = unredirected();
	if ((w != NULL) != last) {
		last = w != NULL;
		fprintf(stderr, "flcomp: fullscreen window %s the desktop\n", w ? "replaces" : "is back on");
	}
	return w;
}
