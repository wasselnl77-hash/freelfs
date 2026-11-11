/* desktop.conf: the settings flcomp itself acts on (pointer, keyboard repeat,
 * Num Lock, night light, display mode and scale, brightness at start).
 * Same file and format as libflui's (key=value); Settings writes it, flcomp
 * applies changes at once through inotify. The display mode and scale are only
 * read at start: changing them live goes through fl_shell set_output.
 * SPDX-License-Identifier: MIT */
#include "comp.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

struct conf CF = {
	.pointer_speed = 0, .tap = true, .typing_disable = true, .repeat_delay = 600, .repeat_rate = 25,
	.night_warmth = 50, .brightness = -1, .scale = 100, .lock_on_lid = true,
	.ws_cols = 2, .ws_rows = 2, .anim = ANIM_ON, .anim_speed = 100, .title_h = 34, .panel_h = 44,
};

static char conf_dir[512], conf_name[512];

static void conf_path(char *path, size_t n)
{
	const char *e = getenv("FLUI_CONF"), *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
	if (e && *e)
		snprintf(path, n, "%s", e);
	else if (x && *x)
		snprintf(path, n, "%s/freelfs/desktop.conf", x);
	else
		snprintf(path, n, "%s/.config/freelfs/desktop.conf", h ? h : "");
}

static int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static void parse_line(struct conf *c, const char *k, const char *v)
{
	int n = atoi(v);
	if (!strcmp(k, "pointer_speed"))
		c->pointer_speed = clampi(n, -100, 100);
	else if (!strcmp(k, "pointer_accel"))
		c->accel_flat = !strcmp(v, "flat");
	else if (!strcmp(k, "mouse_natural"))
		c->mouse_natural = n != 0;
	else if (!strcmp(k, "touchpad_natural"))
		c->touchpad_natural = n != 0;
	else if (!strcmp(k, "tap_click"))
		c->tap = n != 0;
	else if (!strcmp(k, "typing_disable"))
		c->typing_disable = n != 0;
	else if (!strcmp(k, "left_handed"))
		c->left_handed = n != 0;
	else if (!strcmp(k, "key_repeat_delay"))
		c->repeat_delay = clampi(n, 100, 2000);
	else if (!strcmp(k, "key_repeat_rate"))
		c->repeat_rate = clampi(n, 1, 100);
	else if (!strcmp(k, "numlock"))
		c->numlock = n != 0;
	else if (!strcmp(k, "night_light"))
		c->night = n != 0;
	else if (!strcmp(k, "night_warmth"))
		c->night_warmth = clampi(n, 0, 100);
	else if (!strcmp(k, "brightness"))
		c->brightness = clampi(n, 0, 100);
	else if (!strcmp(k, "power_saver"))
		c->power_saver = n != 0;
	else if (!strcmp(k, "performance"))
		c->perf = perf_parse(v);
	else if (!strcmp(k, "animations"))	/* on (default), reduced (fades only), off */
		c->anim = !strcmp(v, "off") || !strcmp(v, "0") ? ANIM_OFF : !strcmp(v, "reduced") ? ANIM_REDUCED : ANIM_ON;
	else if (!strcmp(k, "anim_log"))
		c->anim_log = n != 0;
	else if (!strcmp(k, "animation_speed"))	/* a factor, 0.1 .. 4: 2 is twice as fast */
		c->anim_speed = clampi((int)(atof(v) * 100 + 0.5), 10, 400);
	else if (!strcmp(k, "panel_position"))
		c->panel_top = !strcmp(v, "top");
	else if (!strcmp(k, "panel_height"))
		c->panel_h = clampi(n, 36, 64);
	else if (!strcmp(k, "titlebar"))
		c->title_h = !strcmp(v, "compact") ? 28 : 34;
	else if (!strcmp(k, "buttons"))
		c->buttons_left = !strcmp(v, "left");
	else if (!strcmp(k, "lock_on_lid"))
		c->lock_on_lid = n != 0;
	else if (!strcmp(k, "workspaces")) {	/* workspaces=COLSxROWS (2x2); a bare number N is N x 1 */
		int cols, rows = 1;
		if (sscanf(v, "%d%*1[xX]%d", &cols, &rows) >= 1) {
			c->ws_cols = clampi(cols, 1, 8);
			c->ws_rows = clampi(rows, 1, 8);
		}
	} else if (!strcmp(k, "scale"))
		c->scale = clampi(n, 100, 200);
	else if (!strcmp(k, "mode")) {
		int w, h, hz = 0;
		if (sscanf(v, "%dx%d@%d", &w, &h, &hz) >= 2 && w > 0 && h > 0) {
			c->mode_w = w;
			c->mode_h = h;
			c->mode_hz = hz;
		}
	}
}

static void load(struct conf *c)
{
	char path[512];
	conf_path(path, sizeof path);
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	char line[200];
	while (fgets(line, sizeof line, f)) {
		char *s = line;
		while (*s == ' ' || *s == '\t')
			s++;
		char *eq = strchr(s, '=');
		if (*s == '#' || !eq)
			continue;
		*eq = 0;
		char *v = eq + 1;
		v[strcspn(v, "\r\n")] = 0;
		size_t kn = strlen(s);
		while (kn && (s[kn - 1] == ' ' || s[kn - 1] == '\t'))
			s[--kn] = 0;
		parse_line(c, s, v);
	}
	fclose(f);
}

/* The panel brightness at start, from brightness=; Settings writes the same
 * sysfs file (group seat, see the udev rule of flsettings). FLCOMP_BACKLIGHT
 * names another directory (tests). */
static void restore_brightness(void)
{
	if (C.headless || CF.brightness < 0)
		return;
	char dir[300] = "";
	const char *e = getenv("FLCOMP_BACKLIGHT");
	if (e && *e) {
		snprintf(dir, sizeof dir, "%s", e);
	} else {
		DIR *d = opendir("/sys/class/backlight");
		struct dirent *de;
		while (d && (de = readdir(d)))
			if (de->d_name[0] != '.') {
				snprintf(dir, sizeof dir, "/sys/class/backlight/%s", de->d_name);
				break;
			}
		if (d)
			closedir(d);
	}
	if (!dir[0])
		return;
	char p[340];
	snprintf(p, sizeof p, "%s/max_brightness", dir);
	FILE *f = fopen(p, "r");
	int max = 0;
	if (f) {
		if (fscanf(f, "%d", &max) != 1)
			max = 0;
		fclose(f);
	}
	if (max <= 0)
		return;
	int v = max * CF.brightness / 100;
	if (v < 1)
		v = 1;
	snprintf(p, sizeof p, "%s/brightness", dir);
	f = fopen(p, "w");
	if (f) {
		fprintf(f, "%d\n", v);
		fclose(f);
	}
}

static void apply(const struct conf *old, bool first)
{
	if (first || old->pointer_speed != CF.pointer_speed || old->accel_flat != CF.accel_flat ||
	    old->mouse_natural != CF.mouse_natural || old->touchpad_natural != CF.touchpad_natural ||
	    old->tap != CF.tap || old->typing_disable != CF.typing_disable || old->left_handed != CF.left_handed)
		backend_input_apply();
	if (first || old->repeat_delay != CF.repeat_delay || old->repeat_rate != CF.repeat_rate ||
	    old->numlock != CF.numlock)
		seat_apply_config();
	if ((first && CF.power_saver) || old->power_saver != CF.power_saver)
		power_apply(first);
	if (first || old->ws_cols != CF.ws_cols || old->ws_rows != CF.ws_rows)
		ws_configure(CF.ws_cols, CF.ws_rows);
	if (first || old->anim != CF.anim || old->anim_speed != CF.anim_speed || old->anim_log != CF.anim_log)
		anim_conf_changed();
	if (!first && (old->panel_top != CF.panel_top || old->panel_h != CF.panel_h)) {
		work_area_changed();	/* the panel moves itself; where windows may go changes at once */
		damage_all();
	}
	if (!first && (old->title_h != CF.title_h || old->buttons_left != CF.buttons_left))
		xwm_frames_changed();
	if (old->night != CF.night || old->night_warmth != CF.night_warmth || (first && CF.night))
		backend_night_changed();
}

static int conf_cb(int fd, uint32_t mask, void *data)
{
	char buf[4096] __attribute__((aligned(8)));
	ssize_t n = read(fd, buf, sizeof buf);
	bool hit = false;
	for (char *p = buf; n > 0 && p < buf + n;) {
		struct inotify_event *e = (struct inotify_event *)p;
		if (e->len && !strcmp(e->name, conf_name))
			hit = true;
		p += sizeof(*e) + e->len;
	}
	if (!hit)
		return 0;
	struct conf old = CF, fresh = {
		.tap = true, .typing_disable = true, .repeat_delay = 600, .repeat_rate = 25,
		.night_warmth = 50, .brightness = -1, .scale = 100, .lock_on_lid = true,
		.ws_cols = 2, .ws_rows = 2, .anim = ANIM_ON, .anim_speed = 100, .title_h = 34, .panel_h = 44,
	};
	load(&fresh);
	/* the display is only changed through fl_shell: keep what runs */
	fresh.mode_w = old.mode_w;
	fresh.mode_h = old.mode_h;
	fresh.mode_hz = old.mode_hz;
	fresh.scale = old.scale;
	CF = fresh;
	apply(&old, false);
	return 0;
}

void config_init(void)
{
	load(&CF);
	restore_brightness();
	char path[512];
	conf_path(path, sizeof path);
	snprintf(conf_dir, sizeof conf_dir, "%s", path);
	char *sl = strrchr(conf_dir, '/');
	snprintf(conf_name, sizeof conf_name, "%s", sl ? sl + 1 : path);
	if (!sl)
		return;
	*sl = 0;
	int fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
	if (fd < 0)
		return;
	if (inotify_add_watch(fd, conf_dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE) < 0) {
		close(fd);
		return;
	}
	wl_event_loop_add_fd(C.loop, fd, WL_EVENT_READABLE, conf_cb, NULL);
	/* the settings that need a running seat and devices are applied when
	 * those exist (backend_start, seat_init); night light needs the screen */
}

/* once the output is up: the settings that act on it */
void config_apply_start(void)
{
	apply(&(struct conf){ .night_warmth = CF.night_warmth }, true);
}

/* One key of desktop.conf changed by flcomp (brightness from the keys): the file is
 * rewritten as it is, with that line replaced or added, and renamed into place. */
int config_set(const char *key, const char *val)
{
	char path[512], tmp[560], line[300];
	conf_path(path, sizeof path);
	snprintf(tmp, sizeof tmp, "%s.flcomp", path);
	FILE *out = fopen(tmp, "w");
	if (!out)
		return -1;
	FILE *in = fopen(path, "r");
	size_t kn = strlen(key);
	int done = 0;
	while (in && fgets(line, sizeof line, in)) {
		if (!strncmp(line, key, kn) && line[kn] == '=') {
			fprintf(out, "%s=%s\n", key, val);
			done = 1;
		} else {
			fputs(line, out);
		}
	}
	if (in)
		fclose(in);
	if (!done)
		fprintf(out, "%s=%s\n", key, val);
	if (fclose(out) || rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}
