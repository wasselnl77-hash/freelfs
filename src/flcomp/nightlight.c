/* Night light: a warmer picture in the evening. desktop.conf keys (config.c):
 *   night_light=0|1|auto  (default 0)    night_light_temp=2700..6500 (default 4000)
 *   night_light_from=HH:MM (20:00)       night_light_to=HH:MM (07:00)
 * "auto" is on from `from` until `to` (the span may cross midnight), checked every 30 s.
 * Every change fades over 2 s. The colour factors go to the CRTCs' gamma tables
 * (backend_tint, drm.c); an output whose CRTC has none (virtio-gpu, simpledrm) gets the same
 * factors in the copy to the screen instead. flcomp.log says which, once per fade.
 * SPDX-License-Identifier: MIT */
#include "comp.h"

#include <math.h>
#include <stdio.h>
#include <time.h>

#define FADE_MS 2000
#define STEP_MS 50

static double cur[3] = { 1, 1, 1 }, from[3], to[3];
static int target_temp = 6500;
static uint32_t fade_start;
static int tail;			/* extra ticks after the fade: both screen buffers get the final picture */
static struct wl_event_source *tick, *autot;

/* colour factors for a temperature: Tanner Helland's blackbody fit, scaled so 6500 K is neutral */
static void temp_rgb(int k, double out[3])
{
	double t = k / 100.0;
	double r = t <= 66 ? 255 : 329.698727446 * pow(t - 60, -0.1332047592);
	double g = t <= 66 ? 99.4708025861 * log(t) - 161.1195681661 : 288.1221695283 * pow(t - 60, -0.0755148492);
	double b = t >= 66 ? 255 : t <= 19 ? 0 : 138.5177312231 * log(t - 10) - 305.0447927307;
	double n[3] = { r, g, b };
	double ref[3];
	t = 65;
	ref[0] = 255;
	ref[1] = 99.4708025861 * log(t) - 161.1195681661;
	ref[2] = 138.5177312231 * log(t - 10) - 305.0447927307;
	for (int i = 0; i < 3; i++) {
		double v = n[i] / ref[i];
		out[i] = v < 0 ? 0 : v > 1 ? 1 : v;
	}
}

static bool want_on(void)
{
	int mode = CF.nl_mode >= 0 ? CF.nl_mode : CF.nl_old >= 0 ? CF.nl_old : 0;
	if (mode != 2)
		return mode == 1;
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	int now = tm.tm_hour * 60 + tm.tm_min, f = CF.nl_from, e = CF.nl_to;
	if (f == e)
		return false;
	return f < e ? now >= f && now < e : now >= f || now < e;
}

static int tick_cb(void *data)
{
	uint32_t el = now_ms() - fade_start;
	double k = el >= FADE_MS ? 1.0 : el / (double)FADE_MS;
	for (int i = 0; i < 3; i++)
		cur[i] = from[i] + (to[i] - from[i]) * k;
	backend_tint(cur);
	if (el < FADE_MS) {
		wl_event_source_timer_update(tick, STEP_MS);
		return 0;
	}
	if (tail++ == 0)
		fprintf(stderr, "flcomp: night light %s (%dK, factors %.3f %.3f %.3f)\n",
			target_temp < 6500 ? "on" : "off", target_temp, cur[0], cur[1], cur[2]);
	if (tail <= 2) {
		damage_all();	/* the second screen buffer too (soft path) */
		wl_event_source_timer_update(tick, 120);
	}
	return 0;
}

static void go(int temp)
{
	double t[3];
	if (temp >= 6500) {
		t[0] = t[1] = t[2] = 1;
	} else {
		temp_rgb(temp, t);
	}
	if (t[0] == to[0] && t[1] == to[1] && t[2] == to[2] && fade_start)
		return;
	for (int i = 0; i < 3; i++) {
		from[i] = cur[i];
		to[i] = t[i];
	}
	target_temp = temp;
	fade_start = now_ms();
	tail = 0;
	if (tick)
		wl_event_source_timer_update(tick, 1);
}

static void evaluate(void)
{
	go(want_on() ? (CF.nl_temp ? CF.nl_temp : 4000) : 6500);
}

static int auto_cb(void *data)
{
	evaluate();
	wl_event_source_timer_update(autot, 30000);
	return 0;
}

void night_init(void)
{
	to[0] = to[1] = to[2] = 1;
	tick = wl_event_loop_add_timer(C.loop, tick_cb, NULL);
	autot = wl_event_loop_add_timer(C.loop, auto_cb, NULL);
}

/* desktop.conf changed (or the start): fade to the new state */
void night_conf_changed(void)
{
	if (!tick)
		night_init();
	evaluate();
	wl_event_source_timer_update(autot, 30000);
}

/* an output was lit or changed mode: its CRTC starts neutral */
void night_outputs_changed(void)
{
	if (cur[0] < 1 || cur[1] < 1 || cur[2] < 1)
		backend_tint(cur);
}
