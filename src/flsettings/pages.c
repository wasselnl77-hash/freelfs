/* pages.c -- the Mouse & touchpad and Keyboard pages of flsettings, and the parts of the Display page
 *   that are not screens (brightness, wallpaper); the look shared by all the pages
 * SPDX-License-Identifier: MIT
 *
 * Every setting is a key in desktop.conf (see README); flcomp watches the file and applies pointer,
 * key repeat and Num Lock at once. Brightness is the backlight's sysfs file (group seat may write it,
 * see the udev rule). The screens (mode, scale, arrangement, night light) are display.c's, on fldisplay.
 */
#include <dirent.h>
#include <errno.h>
#include <flui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "fl-shell-client-protocol.h"
#include "pages.h"
#include "wallpaper.h"

enum {
	ID_BRIGHT = 160,
	ID_SPEED = 200, ID_ACCEL = 201 /* .. 202 */, ID_LEFT = 203, ID_MNAT, ID_TNAT, ID_TAP, ID_DWT,
	ID_KTILES = 300 /* .. 303 */, ID_DELAY = 310, ID_RATE, ID_NUM, ID_TRY,
};

#define ROW 56

struct pages {
	fl_app *app;
	fl_font *head, *small;
	fl_win *win;
	struct wallpaper *wp;
	/* keyboard */
	int kbd;
	char status[200];
	char trial[128];
	/* the compositor's binding: the keyboard layout (version 3 also sends the output events, see below) */
	struct fl_shell *shell;
	unsigned shell_ver;
	/* brightness: the brightness keys change it behind our back, so it is looked at once a second */
	fl_timer *bl_poll;
	char bl_dir[300];
	int bl_max, bl_pct;
	long bl_read_at;
	char bl_err[120];
};

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* ---- look: the same as the General page ------------------------------------------------------ */

void pg_section(fl_ui *ui, fl_font *head, int x, int y, int w, const char *name)
{
	fl_text(&ui->cv, head, x, y, fl_th.dim, name, -1);
	fl_fill_rect(&ui->cv, fl_r(x, y + 20, w, 1), fl_th.line);
}

/* title and a dim line under it, on the left of a row 56 px high */
void pg_row_text(fl_ui *ui, fl_font *small, int x, int y, int maxw, const char *title, const char *what)
{
	int th = fl_font_height(ui->f_med), sh = fl_font_height(small);
	int top = y + (ROW - th - 2 - sh) / 2;
	char fit[160];
	fl_text_fit(ui->f_med, title, maxw, fit, sizeof fit);
	fl_text(&ui->cv, ui->f_med, x, top, fl_th.text, fit, -1);
	fl_text_fit(small, what, maxw, fit, sizeof fit);
	fl_text(&ui->cv, small, x, top + th + 2, fl_th.dim, fit, -1);
}

int pg_choice(fl_ui *ui, uint32_t id, fl_rect r, const char *label, int on)
{
	int res = fl_widget(ui, id, r, FL_FOCUSABLE);
	fl_fill_rect(&ui->cv, r, on ? fl_th.select : (res & FL_HOVER) ? fl_th.raised : 0);
	fl_stroke_rect(&ui->cv, r, 1, on || (res & FL_FOCUS) ? fl_th.accent : fl_th.line);
	fl_text_in(&ui->cv, on ? ui->f_med : ui->f_reg, r, FL_CENTER, 0, on ? fl_th.strong : fl_th.text, label);
	if ((res & FL_FOCUS) && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_space)) {
		ui->key = 0;
		return 1;
	}
	return res & FL_CLICK;
}

/* a choice that is not available: dim, does nothing */
void pg_choice_off(fl_ui *ui, fl_rect r, const char *label)
{
	fl_stroke_rect(&ui->cv, r, 1, fl_th.line);
	fl_text_in(&ui->cv, ui->f_reg, r, FL_CENTER, 0, fl_mix(fl_th.panel, fl_th.dim, 120), label);
}

/* the flyout's slider: 4 px track, 10 x 20 thumb; Left/Right step, Home/End. 1 when *v changed */
int pg_slider(fl_ui *ui, uint32_t id, fl_rect r, int *v, int lo, int hi, int step)
{
	int res = fl_widget(ui, id, r, FL_FOCUSABLE), old = *v;
	int x0 = r.x + 6, x1 = r.x + r.w - 6;
	if (res & FL_DOWN) {
		int nv = lo + ((ui->mx - x0) * (hi - lo) + (x1 - x0) / 2) / (x1 - x0);
		*v = nv < lo ? lo : nv > hi ? hi : nv;
	}
	if (res & FL_FOCUS) {
		int k = ui->key;
		if (k == XKB_KEY_Left || k == XKB_KEY_Down)
			*v = *v - step < lo ? lo : *v - step;
		else if (k == XKB_KEY_Right || k == XKB_KEY_Up)
			*v = *v + step > hi ? hi : *v + step;
		else if (k == XKB_KEY_Home)
			*v = lo;
		else if (k == XKB_KEY_End)
			*v = hi;
		if (*v != old || k == XKB_KEY_Home || k == XKB_KEY_End)
			ui->key = 0;
	}
	int tx = x0 + (*v - lo) * (x1 - x0) / (hi - lo), ty = r.y + r.h / 2 - 2;
	fl_fill_rect(&ui->cv, fl_r(x0, ty, x1 - x0, 4), fl_th.line);
	fl_fill_rect(&ui->cv, fl_r(x0, ty, tx - x0, 4), fl_th.accent);
	fl_rect th = fl_r(tx - 5, r.y + r.h / 2 - 10, 10, 20);
	fl_fill_rect(&ui->cv, th, (res & FL_DOWN) ? fl_th.accent : fl_th.text);
	if (res & FL_FOCUS)
		fl_stroke_rect(&ui->cv, fl_inset(th, -3, -3), 1, fl_th.accent);
	return *v != old;
}

static void conf_int(const char *key, int v)
{
	char b[16];
	snprintf(b, sizeof b, "%d", v);
	fl_conf_set(key, b);
}

static int sw_row(fl_ui *ui, struct pages *p, uint32_t id, int x, int y, int w, const char *title, const char *what,
                  const char *key, int def)
{
	pg_row_text(ui, p->small, x, y, w - 60, title, what);
	int on = fl_conf_int(key, def);
	if (fl_switch(ui, id, fl_r(x + w - 40, y + 18, 40, 20), &on))
		fl_conf_set(key, on ? "1" : "0");
	return on;
}

/* a dropdown: the chosen label and a flat chevron (up while open). 1 when clicked; the caller opens or shuts */
int pg_dropdown(fl_ui *ui, uint32_t id, fl_rect r, const char *label, int open)
{
	int res = fl_widget(ui, id, r, FL_FOCUSABLE);
	int cx = r.x + r.w - 16, cy = r.y + r.h / 2;
	fl_path *p = fl_path_new();
	fl_fill_rect(&ui->cv, r, (res & FL_HOVER) ? fl_th.raised : 0);
	fl_stroke_rect(&ui->cv, r, 1, (open || (res & FL_FOCUS)) ? fl_th.accent : fl_th.line);
	fl_text_in(&ui->cv, ui->f_reg, fl_r(r.x, r.y, r.w - 32, r.h), FL_LEFT, 12, fl_th.text, label);
	if (p) {
		int dy = open ? 3 : -3;
		fl_path_move(p, cx - 5, cy - dy);
		fl_path_line(p, cx + 5, cy - dy);
		fl_path_line(p, cx, cy + dy);
		fl_path_close(p);
		fl_path_fill(&ui->cv, p, fl_th.dim);
		fl_path_free(p);
	}
	if ((res & FL_FOCUS) && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_space)) {
		ui->key = 0;
		return 1;
	}
	return res & FL_CLICK;
}

/* a busy indicator: eight flat squares on a ring, one lit and the others fading behind it */
void pg_spinner(fl_ui *ui, int cx, int cy, int phase)
{
	static const int ox[8] = {0, 5, 7, 5, 0, -5, -7, -5}, oy[8] = {-7, -5, 0, 5, 7, 5, 0, -5};
	for (int i = 0; i < 8; i++) {
		int k = ((i - phase) % 8 + 8) % 8;
		fl_fill_rect(&ui->cv, fl_r(cx + ox[i] - 2, cy + oy[i] - 2, 4, 4),
		             fl_mix(fl_th.panel, fl_th.accent, 255 - k * 30));
	}
}

/* a line icon from SVG path data on a 24 x 24 grid, stroked 1.6 px at size */
void pg_glyph(fl_ui *ui, const char *d, int x, int y, int size, fl_color c)
{
	fl_path *p = fl_path_new();
	float sc = size / 24.0f;
	if (!p)
		return;
	if (fl_path_svg(p, d, sc, sc, (float)x, (float)y) == 0)
		fl_path_stroke(&ui->cv, p, 1.6f, c);
	fl_path_free(p);
}

/* ---- brightness ------------------------------------------------------------------------------ */

/* Which backlight: a laptop can have several (acpi_video0, intel_backlight, amdgpu_bl1, nvidia_0).
 * Prefer the kernel's "type": firmware, then platform, then raw; ties go to the first by name. */
static void bl_pick(char *out, size_t n)
{
	static const char *const rank[] = { "firmware", "platform", "raw" };
	int best = 99;
	DIR *d = opendir("/sys/class/backlight");
	struct dirent *de;
	out[0] = 0;
	while (d && (de = readdir(d))) {
		char p[300], t[32] = "";
		if (de->d_name[0] == '.')
			continue;
		snprintf(p, sizeof p, "/sys/class/backlight/%s/type", de->d_name);
		FILE *f = fopen(p, "r");
		if (f) {
			if (fgets(t, sizeof t, f))
				t[strcspn(t, "\n")] = 0;
			fclose(f);
		}
		int r = 3;
		for (int i = 0; i < 3; i++)
			if (!strcmp(t, rank[i]))
				r = i;
		if (r < best || (r == best && strcmp(de->d_name, out + 21) < 0)) {
			best = r;
			snprintf(out, n, "/sys/class/backlight/%s", de->d_name);
		}
	}
	if (d)
		closedir(d);
}

static void bl_find(struct pages *p)
{
	p->bl_dir[0] = 0;
	const char *e = getenv("FLSETTINGS_BACKLIGHT");
	if (e && *e) {
		snprintf(p->bl_dir, sizeof p->bl_dir, "%s", e);
	} else {
		bl_pick(p->bl_dir, sizeof p->bl_dir);
	}
}

static int read_int(const char *dir, const char *name)
{
	char path[360];
	snprintf(path, sizeof path, "%s/%s", dir, name);
	FILE *f = fopen(path, "r");
	int v = -1;
	if (f) {
		if (fscanf(f, "%d", &v) != 1)
			v = -1;
		fclose(f);
	}
	return v;
}

static void bl_refresh(struct pages *p)
{
	p->bl_read_at = now_ms();
	if (!p->bl_dir[0])
		return;
	p->bl_max = read_int(p->bl_dir, "max_brightness");
	int cur = read_int(p->bl_dir, "brightness");
	if (p->bl_max > 0 && cur >= 0)
		p->bl_pct = (cur * 100 + p->bl_max / 2) / p->bl_max;
	else
		p->bl_max = 0;
}

static void bl_set(struct pages *p, int pct)
{
	int v = p->bl_max * pct / 100;
	if (v < 1)
		v = 1;
	char path[360];
	snprintf(path, sizeof path, "%s/brightness", p->bl_dir);
	FILE *f = fopen(path, "w");
	if (f && fprintf(f, "%d\n", v) > 0 && fclose(f) == 0) {
		p->bl_err[0] = 0;
		return;
	}
	snprintf(p->bl_err, sizeof p->bl_err, "Cannot change it: %s (the account needs to be in the group seat).",
	         strerror(errno));
}

/* ---- the compositor's events: screens are display.c's (fldisplay), so these are only read and dropped ---- */

static void noop0(void *ud, struct fl_shell *sh) {}
static void noop_added(void *ud, struct fl_shell *sh, uint32_t id, const char *a, const char *t) {}
static void noop_changed(void *ud, struct fl_shell *sh, uint32_t id, const char *t, uint32_t f, uint32_t m) {}
static void noop_id(void *ud, struct fl_shell *sh, uint32_t id) {}
static void noop_str(void *ud, struct fl_shell *sh, const char *s) {}
static void noop_mode(void *ud, struct fl_shell *sh, uint32_t w, uint32_t h, uint32_t hz, uint32_t flags) {}
static void noop_state(void *ud, struct fl_shell *sh, uint32_t scale, uint32_t w, uint32_t h, uint32_t pending) {}

static const struct fl_shell_listener shell_listener = {
	.window_added = noop_added, .window_changed = noop_changed, .window_removed = noop_id,
	.menu_dismissed = noop0, .menu_toggle = noop0, .keyboard_layout = noop_str,
	.flyout_dismissed = noop0, .volume_key = noop_id,
	.output_mode = noop_mode, .output_state = noop_state, .output_failed = noop0,
};

void pages_set_shell(struct pages *p, struct fl_shell *sh, unsigned version)
{
	p->shell = sh;
	p->shell_ver = version;
	if (sh && version >= 3)
		fl_shell_add_listener(sh, &shell_listener, p);
}

/* ---- the Display page's lower part: brightness and wallpaper. Returns the y below them. ------------ */

int pages_draw_brightness(struct pages *p, fl_ui *ui, int x, int y, int w)
{
	char buf[32];

	pg_section(ui, p->head, x, y, w, "Brightness");
	y += 28;
	if (p->bl_max > 0) {
		if (now_ms() - p->bl_read_at > 1000 && !(ui->btn_held & FL_BTN_L))
			bl_refresh(p);
		snprintf(buf, sizeof buf, "%d%%", p->bl_pct);
		pg_row_text(ui, p->small, x, y, w - 280, "Brightness", "Backlight of this screen");
		fl_text_in(&ui->cv, ui->f_reg, fl_r(x + w - 44, y, 44, ROW), FL_RIGHT, 0, fl_th.text, buf);
		int v = p->bl_pct;
		if (pg_slider(ui, ID_BRIGHT, fl_r(x + w - 280, y + 12, 224, 32), &v, 1, 100, 5)) {
			p->bl_pct = v;
			bl_set(p, v);
			conf_int("brightness", v);
		}
	} else {
		pg_row_text(ui, p->small, x, y, w, "Brightness", "This screen has no backlight to adjust");
	}
	y += ROW;
	if (p->bl_err[0]) {
		fl_text_in(&ui->cv, p->small, fl_r(x, y - 8, w, 18), FL_LEFT, 0, fl_th.err, p->bl_err);
		y += 14;
	}
	y += 24;
	if (p->wp)
		y = wp_draw(p->wp, ui, x, y, w);
	return y;
}

/* ---- mouse and touchpad ---------------------------------------------------------------------- */

static int have_device(const char *needle)
{
	FILE *f = fopen("/proc/bus/input/devices", "r");
	char line[256];
	int found = 0;
	while (f && fgets(line, sizeof line, f))
		if (line[0] == 'N' && strstr(line, needle))
			found = 1;
	if (f)
		fclose(f);
	return found;
}

static void section_note(fl_ui *ui, struct pages *p, int x, int y, int w, const char *note)
{
	int tw = fl_text_width(p->small, note, -1);
	fl_rect cover = fl_r(x + w - tw - 8, y - 1, tw + 8, 20);
	fl_fill_rect(&ui->cv, cover, fl_th.panel);
	fl_text(&ui->cv, p->small, x + w - tw, y + 1, fl_th.dim, note, -1);
}

static int draw_input(fl_ui *ui, struct pages *p, fl_rect c)
{
	int x = c.x + 28, w = c.w - 56, y = c.y + 24;
	char buf[32];

	pg_section(ui, p->head, x, y, w, "Pointer");
	y += 28;
	int sp = fl_conf_int("pointer_speed", 0);
	pg_row_text(ui, p->small, x, y, w - 280, "Pointer speed", "How far the pointer moves");
	if (pg_slider(ui, ID_SPEED, fl_r(x + w - 224, y + 12, 224, 32), &sp, -100, 100, 10))
		conf_int("pointer_speed", sp);
	y += ROW;
	pg_row_text(ui, p->small, x, y, w - 190, "Acceleration", "Adaptive speeds up with quick movements");
	int flat = !strcmp(fl_conf_get("pointer_accel", "adaptive"), "flat");
	if (pg_choice(ui, ID_ACCEL, fl_r(x + w - 176, y + 12, 86, 32), "Adaptive", !flat) && flat)
		fl_conf_set("pointer_accel", "adaptive");
	if (pg_choice(ui, ID_ACCEL + 1, fl_r(x + w - 86, y + 12, 86, 32), "Flat", flat) && !flat)
		fl_conf_set("pointer_accel", "flat");
	y += ROW;
	sw_row(ui, p, ID_LEFT, x, y, w, "Left-handed", "Swap the left and right buttons", "left_handed", 0);
	y += ROW + 24;

	pg_section(ui, p->head, x, y, w, "Mouse");
	if (!have_device("ouse"))
		section_note(ui, p, x, y, w, "none detected");
	y += 28;
	sw_row(ui, p, ID_MNAT, x, y, w, "Natural scrolling", "Content follows the wheel, like on a phone", "mouse_natural", 0);
	y += ROW + 24;

	pg_section(ui, p->head, x, y, w, "Touchpad");
	if (!have_device("ouchpad"))
		section_note(ui, p, x, y, w, "none detected");
	y += 28;
	sw_row(ui, p, ID_TNAT, x, y, w, "Natural scrolling", "Two fingers move the content, not the scrollbar",
	       "touchpad_natural", 0);
	y += ROW;
	sw_row(ui, p, ID_TAP, x, y, w, "Tap to click", "A light tap on the pad is a click", "tap_click", 1);
	y += ROW;
	sw_row(ui, p, ID_DWT, x, y, w, "Disable while typing", "Ignore the pad while keys are pressed", "typing_disable", 1);
	y += ROW;
	(void)buf;
	return y + 12 - c.y;
}

/* ---- keyboard -------------------------------------------------------------------------------- */

static void pick_layout(struct pages *p, int i)
{
	const fl_kbd_layout *l = &fl_kbd_layouts[i];
	p->kbd = i;
	fl_conf_set("keyboard", l->id);
	if (p->shell) {
		fl_shell_set_keyboard_layout(p->shell, l->xkb);
		wl_display_flush(fl_app_display(p->app));
	}
	if (fl_kbd_conf_write(l->id) == 0)
		snprintf(p->status, sizeof p->status, "Saved for every user, from the next start.");
	else
		snprintf(p->status, sizeof p->status, "Saved for you. For all users it needs administrator rights (%s).",
		         strerror(errno));
}

/* ---- the shortcut list (read only): what flcomp does with these keys, src/flcomp/seat.c --------- */

static const struct { const char *keys, *what; } shortcuts[] = {
	{"Super", "Start menu"},
	{"Super+E", "Files"},
	{"Super+T / Ctrl+Alt+T", "Terminal"},
	{"Alt+Tab / Alt+Shift+Tab", "Switch windows (hold Alt)"},
	{"Ctrl+Shift+Esc", "General Manager (task manager)"},
	{"Super+Left / Super+Right", "Snap to the left or right half"},
	{"Super+Up", "Maximize"},
	{"Super+Down", "Restore, or minimize"},
	{"Super+Ctrl+Arrows", "Switch workspace"},
	{"Super+Shift+Arrows", "Move the window to another workspace"},
	{"Super+Tab", "Overview of the workspaces"},
	{"Print", "Screenshot to the pictures folder"},
	{"Media keys", "Volume, brightness, microphone"},
	{"Ctrl+Alt+Backspace", "Log out at once"},
	{"Ctrl+Alt+F1 ... F6", "Text consoles"},
};

/* "Super+Left / Super+Right": every key a flat chip (raised, 1 px line), a dim + between the keys of a
 * combination and a dim / between alternatives. Returns the end x; draw 0 only measures. */
int pg_key_chips(fl_ui *ui, fl_font *f, int x, int y, int h, const char *combo, int draw)
{
	fl_canvas *cv = &ui->cv;
	const char *q = combo;
	while (*q) {
		const char *e = q;
		while (*e && *e != '+' && *e != '/')
			e++;
		int n = (int)(e - q);
		while (n > 0 && q[n - 1] == ' ')
			n--;
		if (n > 0 && *q != ' ') {
			char part[48];
			snprintf(part, sizeof part, "%.*s", n, q);
			int w = fl_text_width(f, part, -1) + 16;
			fl_rect r = fl_r(x, y, w, h);
			if (draw) {
				fl_fill_rect(cv, r, fl_th.raised);
				fl_stroke_rect(cv, r, 1, fl_th.line);
				fl_text_in(cv, f, r, FL_CENTER, 0, fl_th.text, part);
			}
			x += w;
		}
		if (*e == '+' || *e == '/') {	/* between two keys of one combination, or two combinations */
			int gap = *e == '+' ? 14 : 26;
			if (draw)
				fl_text_in(cv, f, fl_r(x, y, gap, h), FL_CENTER, 0, fl_th.dim, *e == '+' ? "+" : "/");
			x += gap;
		}
		q = *e ? e + 1 : e;
		while (*q == ' ')
			q++;
	}
	return x;
}

static int draw_shortcuts(fl_ui *ui, struct pages *p, int x, int y, int w)
{
	pg_section(ui, p->head, x, y, w, "Shortcuts");
	y += 36;
	int kw = 0;
	for (size_t i = 0; i < sizeof shortcuts / sizeof *shortcuts; i++) {
		int e = pg_key_chips(ui, p->small, 0, 0, 22, shortcuts[i].keys, 0);
		kw = e > kw ? e : kw;
	}
	kw += 24;
	for (size_t i = 0; i < sizeof shortcuts / sizeof *shortcuts; i++) {
		char fit[160];
		pg_key_chips(ui, p->small, x, y + 4, 22, shortcuts[i].keys, 1);
		fl_text_fit(ui->f_reg, shortcuts[i].what, w - kw, fit, sizeof fit);
		fl_text_in(&ui->cv, ui->f_reg, fl_r(x + kw, y, w - kw, 30), FL_LEFT, 0, fl_th.dim, fit);
		y += 30;
	}
	return y;
}

static int draw_keyboard(fl_ui *ui, struct pages *p, fl_rect c)
{
	int x = c.x + 28, w = c.w - 56, y = c.y + 24;
	char buf[48];

	pg_section(ui, p->head, x, y, w, "Layout");
	y += 36;
	int pick = fl_kbd_tiles(ui, ID_KTILES, fl_r(x, y, w, 64), 8, p->kbd);
	if (pick >= 0 && pick != p->kbd)
		pick_layout(p, pick);
	y += 64 + 10;
	fl_text_in(&ui->cv, p->small, fl_r(x, y, w, 18), FL_LEFT, 0, fl_th.dim,
	           p->status[0] ? p->status : "Applies at once in every program.");
	y += 18 + 22;

	pg_section(ui, p->head, x, y, w, "Typing");
	y += 28;
	int delay = fl_conf_int("key_repeat_delay", 600);
	snprintf(buf, sizeof buf, "%d ms", delay);
	pg_row_text(ui, p->small, x, y, w - 300, "Repeat delay", "How long a key is held before it repeats");
	fl_text_in(&ui->cv, ui->f_reg, fl_r(x + w - 64, y, 64, ROW), FL_RIGHT, 0, fl_th.text, buf);
	if (pg_slider(ui, ID_DELAY, fl_r(x + w - 300, y + 12, 224, 32), &delay, 150, 1000, 50))
		conf_int("key_repeat_delay", delay);
	y += ROW;
	int rate = fl_conf_int("key_repeat_rate", 25);
	snprintf(buf, sizeof buf, "%d/s", rate);
	pg_row_text(ui, p->small, x, y, w - 300, "Repeat rate", "How fast it repeats, keys per second");
	fl_text_in(&ui->cv, ui->f_reg, fl_r(x + w - 64, y, 64, ROW), FL_RIGHT, 0, fl_th.text, buf);
	if (pg_slider(ui, ID_RATE, fl_r(x + w - 300, y + 12, 224, 32), &rate, 2, 60, 2))
		conf_int("key_repeat_rate", rate);
	y += ROW;
	sw_row(ui, p, ID_NUM, x, y, w, "Num Lock at start", "Number pad types digits when the desktop starts",
	       "numlock", 0);
	y += ROW + 24;

	pg_section(ui, p->head, x, y, w, "Try it");
	y += 36;
	fl_textfield(ui, ID_TRY, fl_r(x, y, w, 36), p->trial, sizeof p->trial,
	             "Click here and type to try the layout and the repeat", ui->f_reg);
	y += 36 + 24;
	y = draw_shortcuts(ui, p, x, y, w);
	return y + 12 - c.y;
}

/* ---- the lot --------------------------------------------------------------------------------- */

/* Two steps: the object exists before the Wayland connection (fl_shell's first events come
 * while the globals are bound), the rest is set up once there is an app. */
static void bl_poll_cb(void *ud);

struct pages *pages_new(void)
{
	struct pages *p = calloc(1, sizeof *p);
	return p;
}

void pages_start(struct pages *p, fl_app *app, fl_font *head, fl_font *small)
{
	p->app = app;
	p->head = head;
	p->small = small;
	p->wp = wp_new(app, head, small);
	p->bl_poll = fl_timer_new(app, bl_poll_cb, p);
	int k = fl_kbd_find(fl_conf_get("keyboard", ""));
	if (k < 0)
		k = fl_kbd_conf_read();
	p->kbd = k >= 0 ? k : 1;
	bl_find(p);
	bl_refresh(p);
}

void pages_free(struct pages *p)
{
	if (!p)
		return;
	wp_free(p->wp);
	if (p->bl_poll)
		fl_timer_free(p->bl_poll);
	free(p);
}

void pages_set_win(struct pages *p, fl_win *win)
{
	p->win = win;
	if (p->wp)
		wp_set_win(p->wp, win);
}

static void bl_poll_cb(void *ud)
{
	struct pages *p = ud;
	int old = p->bl_pct;
	bl_refresh(p);
	if (p->bl_pct != old && p->win)
		fl_win_redraw(p->win);
}

/* page: PG_*, or -1 for a page that is not one of these */
void pages_show(struct pages *p, int page)
{
	if (page == PG_DISPLAY) {
		if (p->wp)
			wp_show(p->wp);
		bl_refresh(p);
		if (p->bl_poll && p->bl_max > 0)
			fl_timer_arm(p->bl_poll, 1000, 1000);
	} else if (p->bl_poll) {
		fl_timer_arm(p->bl_poll, 0, 0);
	}
}

int pages_draw(struct pages *p, int page, fl_ui *ui, fl_rect c)
{
	switch (page) {
	case PG_INPUT: return draw_input(ui, p, c);
	default: return draw_keyboard(ui, p, c);
	}
}
