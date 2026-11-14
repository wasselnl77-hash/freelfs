/* fldisplay -- list and change the display outputs through flcomp (fl_shell version 9)
 *
 *   fldisplay list
 *   fldisplay set NAME [mode=WxH@HZ] [scale=1|1.25|1.5|1.75|2] [pos=X,Y] [enabled=0|1] [primary=1]
 *   fldisplay revert
 *   fldisplay nightlight on|off|auto [temp=2700..6500] [from=HH:MM] [to=HH:MM]
 *
 * list prints one line per output, tab separated:
 *   NAME  connected|disconnected  WxH@HZ|-  SCALE  X,Y  enabled|disabled  primary|-  MODES
 * MODES is a comma separated list of WxH@HZ, preferred first. X,Y are logical pixels.
 * flcomp applies the change at once and saves it in desktop.conf (output.NAME=); there is no
 * automatic revert: ask "keep these settings?" and call `fldisplay revert` if the answer is no.
 * nightlight writes nightlight= (and the older night_light=) and the other keys to desktop.conf,
 * flcomp fades to the new state within two seconds. Exit 0 on success, errors on stderr.
 * SPDX-License-Identifier: MIT */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

#include "fl-shell-client-protocol.h"

struct out {
	char name[32], modes[1024];
	unsigned flags, w, h, hz, scale;
	int x, y;
};
static struct out O[16];
static int NO;
static bool failed, got_state;
static struct fl_shell *shell;

static void on_info(void *d, struct fl_shell *s, const char *name, uint32_t flags, int32_t x, int32_t y, uint32_t w,
		    uint32_t h, uint32_t hz, uint32_t scale)
{
	if (NO >= 16)
		return;
	struct out *o = &O[NO++];
	memset(o, 0, sizeof *o);
	snprintf(o->name, sizeof o->name, "%s", name);
	o->flags = flags;
	o->x = x;
	o->y = y;
	o->w = w;
	o->h = h;
	o->hz = hz;
	o->scale = scale;
}

static void on_mode_info(void *d, struct fl_shell *s, uint32_t w, uint32_t h, uint32_t hz, uint32_t flags)
{
	if (!NO)
		return;
	struct out *o = &O[NO - 1];
	size_t n = strlen(o->modes);
	snprintf(o->modes + n, sizeof o->modes - n, "%s%ux%u@%u", n ? "," : "", w, h, hz);
}

static void on_state(void *d, struct fl_shell *s, uint32_t sc, uint32_t w, uint32_t h, uint32_t p) { got_state = true; }
static void on_failed(void *d, struct fl_shell *s) { failed = true; }
static void nop() {}

static const struct fl_shell_listener lis = {
	.window_added = (void *)nop, .window_changed = (void *)nop, .window_removed = (void *)nop,
	.menu_dismissed = (void *)nop, .menu_toggle = (void *)nop, .keyboard_layout = (void *)nop,
	.flyout_dismissed = (void *)nop, .volume_key = (void *)nop, .output_mode = (void *)nop,
	.output_state = on_state, .output_failed = on_failed, .lock_state = (void *)nop,
	.workspaces = (void *)nop, .window_workspace = (void *)nop,
	.output_info = on_info, .output_mode_info = on_mode_info,
};

static void reg(void *d, struct wl_registry *r, uint32_t id, const char *iface, uint32_t ver)
{
	if (!strcmp(iface, "fl_shell") && ver >= 9) {
		shell = wl_registry_bind(r, id, &fl_shell_interface, 9);
		fl_shell_add_listener(shell, &lis, NULL);
	}
}
static void reg_gone(void *d, struct wl_registry *r, uint32_t id) {}
static const struct wl_registry_listener rl = { reg, reg_gone };

/* ---- desktop.conf (the nightlight keys; flcomp watches the file) ------------------------------ */

static void conf_path(char *p, size_t n)
{
	const char *e = getenv("FLUI_CONF"), *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
	if (e && *e)
		snprintf(p, n, "%s", e);
	else if (x && *x)
		snprintf(p, n, "%s/freelfs/desktop.conf", x);
	else
		snprintf(p, n, "%s/.config/freelfs/desktop.conf", h ? h : "");
}

/* replace or add KEY=VAL lines, the rest of the file as it was, renamed into place */
static int conf_set(const char *const *kv, int nkv)
{
	char path[512], tmp[560], line[400], dir[512];
	conf_path(path, sizeof path);
	snprintf(dir, sizeof dir, "%s", path);
	char *sl = strrchr(dir, '/');
	if (sl) {
		*sl = 0;
		char cmd[600];
		snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
		if (system(cmd) != 0)
			return -1;
	}
	snprintf(tmp, sizeof tmp, "%s.fldisplay", path);
	FILE *out = fopen(tmp, "w");
	if (!out)
		return -1;
	FILE *in = fopen(path, "r");
	bool done[16] = { 0 };
	while (in && fgets(line, sizeof line, in)) {
		bool replaced = false;
		for (int i = 0; i < nkv; i += 2) {
			size_t kn = strlen(kv[i]);
			if (!strncmp(line, kv[i], kn) && line[kn] == '=') {
				if (!done[i / 2])
					fprintf(out, "%s=%s\n", kv[i], kv[i + 1]);
				done[i / 2] = replaced = true;
			}
		}
		if (!replaced)
			fputs(line, out);
	}
	if (in)
		fclose(in);
	for (int i = 0; i < nkv; i += 2)
		if (!done[i / 2])
			fprintf(out, "%s=%s\n", kv[i], kv[i + 1]);
	if (fclose(out) || rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

static int cmd_nightlight(int argc, char **argv)
{
	if (argc < 1 || (strcmp(argv[0], "on") && strcmp(argv[0], "off") && strcmp(argv[0], "auto"))) {
		fprintf(stderr, "usage: fldisplay nightlight on|off|auto [temp=K] [from=HH:MM] [to=HH:MM]\n");
		return 2;
	}
	const char *kv[16];
	int n = 0;
	kv[n++] = "nightlight";
	kv[n++] = argv[0];
	kv[n++] = "night_light";	/* the older key: 0, 1, auto */
	kv[n++] = !strcmp(argv[0], "on") ? "1" : !strcmp(argv[0], "off") ? "0" : "auto";
	char temp[16], from[8], to[8];
	for (int i = 1; i < argc; i++) {
		int a, b;
		if (sscanf(argv[i], "temp=%d", &a) == 1 && a >= 2700 && a <= 6500) {
			snprintf(temp, sizeof temp, "%d", a);
			kv[n++] = "nightlight_temp";
			kv[n++] = temp;
		} else if (sscanf(argv[i], "from=%d:%d", &a, &b) == 2 && a >= 0 && a < 24 && b >= 0 && b < 60) {
			snprintf(from, sizeof from, "%02d:%02d", a, b);
			kv[n++] = "nightlight_from";
			kv[n++] = from;
		} else if (sscanf(argv[i], "to=%d:%d", &a, &b) == 2 && a >= 0 && a < 24 && b >= 0 && b < 60) {
			snprintf(to, sizeof to, "%02d:%02d", a, b);
			kv[n++] = "nightlight_to";
			kv[n++] = to;
		} else {
			fprintf(stderr, "fldisplay: bad argument %s\n", argv[i]);
			return 2;
		}
	}
	if (conf_set(kv, n) < 0) {
		fprintf(stderr, "fldisplay: cannot write desktop.conf: %s\n", strerror(errno));
		return 1;
	}
	return 0;
}

static struct wl_display *connect_shell(void)
{
	struct wl_display *dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "fldisplay: cannot connect to the compositor (WAYLAND_DISPLAY)\n");
		return NULL;
	}
	struct wl_registry *r = wl_display_get_registry(dpy);
	wl_registry_add_listener(r, &rl, NULL);
	wl_display_roundtrip(dpy);
	if (!shell) {
		fprintf(stderr, "fldisplay: the compositor has no fl_shell version 9\n");
		return NULL;
	}
	wl_display_roundtrip(dpy);
	return dpy;
}

static void fmt_scale(char *b, size_t n, unsigned pct)
{
	snprintf(b, n, "%g", pct / 100.0);
}

int main(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[1], "nightlight"))
		return cmd_nightlight(argc - 2, argv + 2);
	if (argc < 2 || (strcmp(argv[1], "list") && strcmp(argv[1], "set") && strcmp(argv[1], "revert"))) {
		fprintf(stderr, "usage: fldisplay list | set NAME [mode=WxH@HZ] [scale=S] [pos=X,Y] [enabled=0|1] [primary=1]\n"
				"       fldisplay revert | nightlight on|off|auto [temp=K] [from=HH:MM] [to=HH:MM]\n");
		return 2;
	}
	struct wl_display *dpy = connect_shell();
	if (!dpy)
		return 1;
	if (!strcmp(argv[1], "list")) {
		for (int i = 0; i < NO; i++) {
			struct out *o = &O[i];
			char cur[40] = "-", sc[16];
			bool en = o->flags & 2;
			if (en && o->w)
				snprintf(cur, sizeof cur, "%ux%u@%u", o->w, o->h, o->hz);
			fmt_scale(sc, sizeof sc, o->scale);
			printf("%s\t%s\t%s\t%s\t%d,%d\t%s\t%s\t%s\n", o->name, o->flags & 1 ? "connected" : "disconnected", cur, sc,
			       o->x, o->y, en ? "enabled" : "disabled", o->flags & 4 ? "primary" : "-", o->modes);
		}
		return 0;
	}
	if (!strcmp(argv[1], "revert")) {
		fl_shell_output_revert(shell);
		wl_display_roundtrip(dpy);
		if (failed) {
			fprintf(stderr, "fldisplay: nothing to revert\n");
			return 1;
		}
		return 0;
	}
	if (argc < 4) {
		fprintf(stderr, "fldisplay: set NAME key=value ...\n");
		return 2;
	}
	unsigned flags = 0, w = 0, h = 0, hz = 0, scale = 0, en = 0, prim = 0;
	int x = 0, y = 0;
	for (int i = 3; i < argc; i++) {
		int a, b, c;
		double sc;
		if (sscanf(argv[i], "mode=%dx%d@%d", &a, &b, &c) == 3 && a > 0 && b > 0 && c > 0) {
			flags |= 1;
			w = a;
			h = b;
			hz = c;
		} else if (sscanf(argv[i], "scale=%lf", &sc) == 1 && sc >= 1 && sc <= 2) {
			flags |= 2;
			scale = (unsigned)(sc * 100 + 0.5);
		} else if (sscanf(argv[i], "pos=%d,%d", &a, &b) == 2) {
			flags |= 4;
			x = a;
			y = b;
		} else if (sscanf(argv[i], "enabled=%d", &a) == 1) {
			flags |= 8;
			en = a != 0;
		} else if (sscanf(argv[i], "primary=%d", &a) == 1 && a == 1) {
			flags |= 16;
			prim = 1;
		} else {
			fprintf(stderr, "fldisplay: bad argument %s\n", argv[i]);
			return 2;
		}
	}
	bool known = false;
	for (int i = 0; i < NO; i++)
		known |= !strcmp(O[i].name, argv[2]);
	if (!known) {
		fprintf(stderr, "fldisplay: no output %s\n", argv[2]);
		return 1;
	}
	fl_shell_output_configure(shell, argv[2], flags, w, h, hz, scale, x, y, en, prim);
	wl_display_roundtrip(dpy);
	if (failed) {
		fprintf(stderr, "fldisplay: flcomp refused the change (output not connected, mode or scale not usable, or the last output)\n");
		return 1;
	}
	return 0;
}
