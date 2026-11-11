/* The lock screen, enforced by the compositor (fl_shell lock / unlock).
 *
 * While C.locked nothing but the lock client's surface is drawn, picked or given the
 * keyboard (render.c composite_rect, surface.c pick, xdg.c update_kb_focus, seat.c
 * locked_binding): windows, panel, menus and notifications are simply not part of the
 * picture, so there is nothing to leak and nothing to click through to.
 *
 * The lock holds when the lock program dies. lock_surface_gone() then starts it again
 * (up to three quick failures in a row); while nothing runs the screen is a solid
 * colour and, after a moment, a hint says that Enter starts the lock program again.
 * The text is drawn here from a small built-in font (lockfont.h, tools/genlockfont.c)
 * because the compositor has no fonts and must not depend on the program that failed.
 * Ctrl-Alt-F1..F12 still switch the virtual terminal (seat.c).
 *
 * lock_start() is Super+L and the lid switch: lock at once, then start $FLCOMP_LOCK
 * (default fllock), which sends fl_shell lock with its surface. The panel's Lock
 * button just starts fllock, which sends the same request.
 * SPDX-License-Identifier: MIT */
#include "comp.h"
#include "lockfont.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BG 0x1d2125u
#define TEXT 0xd8dce0u
#define DIM 0x8a9199u
#define ACCENT theme_accent()	/* accent= of desktop.conf, see theme.c */
#define QUICK_MS 5000	/* a lock program that dies within this counts as failing */
#define HINT_MS 1500	/* nothing shows the lock for this long: say so */

static bool hint;
static int fails;
static uint32_t surf_since;
static struct wl_event_source *hint_timer, *respawn_timer;

static const char *lock_cmd(void)
{
	const char *e = getenv("FLCOMP_LOCK");
	return e && *e ? e : "fllock";
}

bool lock_available(void)
{
	if (C.nolock) /* the login screen and first-boot setup have nothing to lock */
		return false;
	const char *e = getenv("FLCOMP_LOCK");
	if (e)
		return *e != 0;
	return access("/usr/bin/fllock", X_OK) == 0;
}

static void lock_run(void)
{
	run_program(lock_cmd());
}

static void timers_stop(void)
{
	if (hint_timer)
		wl_event_source_timer_update(hint_timer, 0);
	if (respawn_timer)
		wl_event_source_timer_update(respawn_timer, 0);
}

static int hint_cb(void *data)
{
	if (C.locked && !C.lock_surf) {
		hint = true;
		damage_all();
	}
	return 0;
}

static int respawn_cb(void *data)
{
	if (C.locked && !C.lock_surf)
		lock_run();
	return 0;
}

static void arm_hint(int ms)
{
	if (!hint_timer && C.loop)
		hint_timer = wl_event_loop_add_timer(C.loop, hint_cb, NULL);
	if (hint_timer)
		wl_event_source_timer_update(hint_timer, ms);
}

/* the locked state, with or without a lock client yet */
static void enter(void)
{
	if (C.locked)
		return;
	C.locked = true;
	hint = false;
	menu_dismiss();
	flyout_dismiss();
	popups_dismiss();
	if (grab_active())
		grab_end();
	cycle_end();
	seat_set_ptr_focus(NULL, 0, 0);
	update_kb_focus();
	seat_refocus();
	damage_all();
	fl_emit_lock_state();
}

void lock_start(void)
{
	if (C.locked)
		return;
	enter();
	arm_hint(HINT_MS + 500);
	lock_run();
}

void lock_begin(struct surface *s, struct wl_client *c)
{
	enter();
	s->role = ROLE_LOCK;
	C.lock_surf = s;
	C.lock_client = c;
	surf_since = now_ms();
	hint = false;
	timers_stop();
	update_kb_focus();
	seat_refocus();
	damage_all();
}

/* the lock client's surface got (or lost) its buffer */
void lock_surface_committed(struct surface *s)
{
	if (!C.locked || s != C.lock_surf)
		return;
	update_kb_focus();
	seat_refocus();
	damage_all();
}

void lock_end(struct wl_client *c)
{
	if (!C.locked || !C.lock_surf || c != C.lock_client)
		return;
	C.locked = false;
	C.lock_surf = NULL;
	C.lock_client = NULL;
	hint = false;
	fails = 0;
	timers_stop();
	update_kb_focus();
	seat_refocus();
	damage_all();
	fl_emit_lock_state();
}

void lock_surface_gone(struct surface *s)
{
	if (!C.locked || s != C.lock_surf)
		return;
	C.lock_surf = NULL;
	C.lock_client = NULL;
	update_kb_focus();
	seat_refocus();
	damage_all();
	if (now_ms() - surf_since < QUICK_MS)
		fails++;
	else
		fails = 0;
	if (fails < 3 && lock_available()) {
		if (!respawn_timer && C.loop)
			respawn_timer = wl_event_loop_add_timer(C.loop, respawn_cb, NULL);
		if (respawn_timer)
			wl_event_source_timer_update(respawn_timer, 300 * (fails + 1));
		arm_hint(HINT_MS + 300 * (fails + 1));
	} else {
		hint = true;	/* it keeps failing: stop and ask */
	}
}

/* Enter on the hint screen: try the lock program once more */
bool lock_retry(void)
{
	if (!C.locked || C.lock_surf || !hint || !lock_available())
		return false;
	fails = 0;
	hint = false;
	damage_all();
	arm_hint(HINT_MS + 500);
	lock_run();
	return true;
}

/* ---- drawing ----------------------------------------------------------------- */

static inline void put(struct rect clip, int x, int y, uint32_t col, int a)
{
	if (x < clip.x1 || y < clip.y1 || x >= clip.x2 || y >= clip.y2)
		return;
	uint32_t *d = &C.shadow[(size_t)y * C.w + x];
	uint32_t r = (((*d >> 16 & 255) * (255 - a) + (col >> 16 & 255) * a) / 255) & 255;
	uint32_t g = (((*d >> 8 & 255) * (255 - a) + (col >> 8 & 255) * a) / 255) & 255;
	uint32_t b = (((*d & 255) * (255 - a) + (col & 255) * a) / 255) & 255;
	*d = r << 16 | g << 8 | b;
}

static void glyph(struct rect clip, int x, int y, const unsigned char *g, int cw, int ch, uint32_t col)
{
	for (int j = 0; j < ch; j++)
		for (int i = 0; i < cw; i++) {
			int idx = j * cw + i;
			int q = (g[idx >> 2] >> (6 - 2 * (idx & 3))) & 3;
			if (q)
				put(clip, x + i, y + j, col, q * 85);
		}
}

static int body_width(const char *s)
{
	int w = 0;
	for (; *s; s++)
		if (*s >= 32 && *s < 127)
			w += LF_BODY_ADV[*s - 32];
	return w;
}

static void body_text(struct rect clip, int x, int y, const char *s, uint32_t col)
{
	for (; *s; s++) {
		if (*s < 32 || *s > 126)
			continue;
		glyph(clip, x, y, LF_BODY_DATA[*s - 32], LF_BODY_W, LF_BODY_H, col);
		x += LF_BODY_ADV[*s - 32];
	}
}

static int head_width(const char *s)
{
	int w = 0;
	for (; *s; s++) {
		const char *p = strchr(LF_HEAD_SET, *s);
		if (p)
			w += LF_HEAD_ADV[p - LF_HEAD_SET];
	}
	return w;
}

static void head_text(struct rect clip, int x, int y, const char *s, uint32_t col)
{
	for (; *s; s++) {
		const char *p = strchr(LF_HEAD_SET, *s);
		if (!p)
			continue;
		glyph(clip, x, y, LF_HEAD_DATA[p - LF_HEAD_SET], LF_HEAD_W, LF_HEAD_H, col);
		x += LF_HEAD_ADV[p - LF_HEAD_SET];
	}
}

void lock_draw_base(struct rect clip)
{
	for (int y = clip.y1; y < clip.y2; y++) {
		uint32_t *p = C.shadow + (size_t)y * C.w;
		for (int x = clip.x1; x < clip.x2; x++)
			p[x] = BG;
	}
	if (!hint || C.lock_surf)
		return;
	static const char head[] = "Screen locked";
	static const char *const lines[] = {
		"The lock program is not running.",
		"Press Enter to start it again.",
	};
	static const char foot[] = "Ctrl+Alt+F2 opens a text console.";
	int hw = head_width(head), lh = LF_BODY_H + 6;
	int block = LF_HEAD_H + 16 + 4 + 18 + 2 * lh + 18 + lh;
	int y = (C.h - block) / 2;
	if (y < 8)
		y = 8;
	int x = (C.w - hw) / 2;
	head_text(clip, x, y, head, TEXT);
	y += LF_HEAD_H + 16;
	for (int j = 0; j < 4; j++)	/* a short accent bar under the heading */
		for (int i = 0; i < 48; i++)
			put(clip, (C.w - 48) / 2 + i, y + j, ACCENT, 255);
	y += 4 + 18;
	for (int k = 0; k < 2; k++, y += lh)
		body_text(clip, (C.w - body_width(lines[k])) / 2, y, lines[k], TEXT);
	y += 18;
	body_text(clip, (C.w - body_width(foot)) / 2, y, foot, DIM);
}

bool lock_hint_shown(void)
{
	return C.locked && !C.lock_surf && hint;
}

/* the lid was closed: lock, if there is something to lock with and the setting allows it */
void lock_lid(bool closed)
{
	if (closed && CF.lock_on_lid && lock_available())
		lock_start();
}
