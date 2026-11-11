/* The output: display mode, scale, and the 15 second "keep these settings?".
 *
 * The screen has a mode (pixels). The desktop is laid out at the mode's size
 * divided by the scale (100 to 200 %), in C.w x C.h, and drm.c enlarges it
 * for the screen. wl_output tells clients the desktop's size, so nothing else
 * has to know about the scale. Mode and scale at start come from desktop.conf
 * (mode=1920x1080@60, scale=125); at run time Settings uses fl_shell set_output.
 * SPDX-License-Identifier: MIT */
#include "comp.h"
#include "fl-shell-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIRM_SECS 15

static struct {
	bool pending;
	int w, h, hz, scale;			/* what to go back to */
	struct wl_event_source *timer;
	int deadline_ms;			/* now_ms() + the wait */
	struct wl_client *owner;
	struct wl_listener gone;
} P;

/* the desktop must stay usable: 640 x 400 logical at least */
bool output_scale_ok(int pw, int ph, int scale)
{
	return scale >= 100 && scale <= 200 && pw * 100 / scale >= 640 && ph * 100 / scale >= 400;
}

/* the desktop's size for a mode and scale; sets C.w, C.h and everything that depends on them */
static bool resize(int scale)
{
	int w = C.pw * 100 / scale, h = C.ph * 100 / scale;
	uint32_t *sh = calloc((size_t)w * h, 4);
	if (!sh)
		return false;
	free(C.shadow);
	C.shadow = sh;
	C.w = w;
	C.h = h;
	C.scale = scale;
	C.dmg.n = C.prev.n = 0;
	wall_reload();
	if (C.px > C.w - 1)
		C.px = C.w - 1;
	if (C.py > C.h - 1)
		C.py = C.h - 1;
	return true;
}

static void announce(void)
{
	outputs_send_mode();
	fl_emit_output();
}

/* the part after the screen changed: windows, panel, picture */
static void after_change(void)
{
	windows_fit_output();
	work_area_changed();
	output_restart_panel();	/* it sizes itself to the output once, when it starts */
	damage_all();
	announce();
}

bool output_set(int w, int h, int hz, int scale)
{
	int ow = C.pw, oh = C.ph, ohz = C.refresh / 1000, os = C.scale;
	if (!w || !h) {
		w = ow;
		h = oh;
		hz = ohz;
	}
	if (!output_scale_ok(w, h, scale))
		return false;
	if (w == ow && h == oh && hz == ohz && scale == os)
		return true;
	if ((w != ow || h != oh || hz != ohz) && !backend_set_mode(w, h, hz))
		return false;
	if (!resize(scale)) {
		if (w != ow || h != oh || hz != ohz)
			backend_set_mode(ow, oh, ohz);
		return false;
	}
	if (!P.pending) {
		P.pending = true;
		P.w = ow;
		P.h = oh;
		P.hz = ohz;
		P.scale = os;
	}
	P.deadline_ms = (int)now_ms() + CONFIRM_SECS * 1000;
	if (P.timer)
		wl_event_source_timer_update(P.timer, CONFIRM_SECS * 1000);
	after_change();
	return true;
}

static void settle(void)
{
	P.pending = false;
	if (P.timer)
		wl_event_source_timer_update(P.timer, 0);
	if (P.owner) {
		wl_list_remove(&P.gone.link);
		P.owner = NULL;
	}
}

void output_confirm(void)
{
	if (!P.pending)
		return;
	settle();
	announce();
}

void output_revert(void)
{
	if (!P.pending)
		return;
	int w = P.w, h = P.h, hz = P.hz, scale = P.scale;
	settle();
	if (!backend_set_mode(w, h, hz))
		fprintf(stderr, "flcomp: cannot go back to %dx%d@%d\n", w, h, hz);
	if (resize(scale))
		after_change();
	else
		announce();
}

static int timer_cb(void *data)
{
	output_revert();
	return 0;
}

static void owner_gone(struct wl_listener *l, void *data)
{
	P.owner = NULL;	/* the listener is unlinked by libwayland */
	output_revert();
}

/* a client asked for the change: if it dies before confirming, go back */
void output_watch_client(struct wl_client *c)
{
	if (!P.pending)
		return;
	if (P.owner)
		wl_list_remove(&P.gone.link);
	P.owner = c;
	P.gone.notify = owner_gone;
	wl_client_add_destroy_listener(c, &P.gone);
}

int output_pending_secs(void)
{
	if (!P.pending)
		return 0;
	int left = (P.deadline_ms - (int)now_ms() + 999) / 1000;
	return left < 1 ? 1 : left > CONFIRM_SECS ? CONFIRM_SECS : left;
}

void output_send_modes(struct wl_resource *r)
{
	struct outmode m[64];
	int n = backend_modes(m, 64);
	for (int i = 0; i < n; i++) {
		uint32_t flags = (m[i].w == C.pw && m[i].h == C.ph && m[i].hz == C.refresh / 1000 ? 1 : 0) |
				 (m[i].preferred ? 2 : 0);
		fl_shell_send_output_mode(r, m[i].w, m[i].h, m[i].hz, flags);
	}
	fl_shell_send_output_state(r, C.scale, C.w, C.h, output_pending_secs());
}

/* At start: the mode and scale of desktop.conf, if the display has them. */
void output_init(void)
{
	P.timer = wl_event_loop_add_timer(C.loop, timer_cb, NULL);
	C.scale = 100;
	if (CF.mode_w && !(CF.mode_w == C.pw && CF.mode_h == C.ph && (!CF.mode_hz || CF.mode_hz == C.refresh / 1000))) {
		int hz = CF.mode_hz;
		if (!hz) {
			struct outmode m[64];
			int n = backend_modes(m, 64);
			for (int i = 0; i < n; i++)
				if (m[i].w == CF.mode_w && m[i].h == CF.mode_h && m[i].hz > hz)
					hz = m[i].hz;
		}
		if (!backend_set_mode(CF.mode_w, CF.mode_h, hz))
			fprintf(stderr, "flcomp: mode %dx%d@%d from desktop.conf not usable, keeping the display's\n",
				CF.mode_w, CF.mode_h, hz);
	}
	int scale = CF.scale;
	if (!output_scale_ok(C.pw, C.ph, scale))
		scale = 100;
	C.w = C.pw * 100 / scale;
	C.h = C.ph * 100 / scale;
	C.scale = scale;
	CF.scale = scale;
}
