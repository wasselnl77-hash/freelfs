/* conf.c -- theme tokens and desktop.conf
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

fl_theme fl_th = {
	.bg = FL_HEX(0x1d2125), .panel = FL_HEX(0x272c33), .raised = FL_HEX(0x343a43),
	.line = FL_HEX(0x3a4048), .text = FL_HEX(0xd8dce0), .dim = FL_HEX(0x8a9199),
	.accent = FL_HEX(0x3f9b63), .on_accent = FL_HEX(0x0e1a13), .select = FL_HEX(0x2f4a3b),
	.strong = FL_HEX(0xf0f2f4), .term_bg = FL_HEX(0x16191c), .err = FL_HEX(0xe06c6c), .dark = 1,
};

/* ---- accent colour ---------------------------------------------------------------------
 * accent= in desktop.conf is one of the ids below or a hex colour (RRGGBB, #RRGGBB). Every
 * accent has a dark-theme and a light-theme shade; a hex colour is used as is on dark and
 * darkened on light. on_accent (text on an accent fill) is picked for contrast and the
 * selection fill is the accent thinly mixed into the panel colour. */

const fl_accent fl_accents[FL_ACCENT_COUNT] = {
	{"green",  "Green",  FL_HEX(0x3f9b63), FL_HEX(0x2c7a4b)},
	{"teal",   "Teal",   FL_HEX(0x35a09b), FL_HEX(0x217c78)},
	{"blue",   "Blue",   FL_HEX(0x4d8fd0), FL_HEX(0x2f6cae)},
	{"amber",  "Amber",  FL_HEX(0xd0a032), FL_HEX(0x9a7210)},
	{"orange", "Orange", FL_HEX(0xe0793f), FL_HEX(0xb35318)},
	{"rose",   "Rose",   FL_HEX(0xd4678c), FL_HEX(0xab4569)},
};

static int parse_hex(const char *s, fl_color *out)
{
	if (*s == '#')
		s++;
	if (strlen(s) != 6)
		return -1;
	for (int i = 0; i < 6; i++)
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F')))
			return -1;
	*out = FL_HEX(strtoul(s, NULL, 16));
	return 0;
}

int fl_accent_find(const char *id)
{
	for (int i = 0; id && i < FL_ACCENT_COUNT; i++)
		if (!strcmp(id, fl_accents[i].id))
			return i;
	return -1;
}

/* The accent in desktop.conf, as shown on a dark or light theme. */
fl_color fl_accent_current(int dark)
{
	const char *v = fl_conf_get("accent", "green");
	int i = fl_accent_find(v);
	if (i >= 0)
		return dark ? fl_accents[i].dark : fl_accents[i].light;
	fl_color c;
	if (parse_hex(v, &c) == 0)
		return dark ? c : fl_mix(c, FL_HEX(0x000000), 56);
	return dark ? fl_accents[0].dark : fl_accents[0].light;
}

static unsigned chan_lin(unsigned c8)
{
	/* sRGB to linear, scaled to 0..1000 */
	float c = (float)c8 / 255.0f;
	c = c <= 0.04045f ? c / 12.92f : __builtin_powf((c + 0.055f) / 1.055f, 2.4f);
	return (unsigned)(c * 1000.0f);
}

static unsigned lum(fl_color c)
{
	return (2126 * chan_lin(c >> 16 & 255) + 7152 * chan_lin(c >> 8 & 255) + 722 * chan_lin(c & 255)) / 10000;
}

/* the text colour with the better contrast on a fill */
static fl_color on_fill(fl_color fill, fl_color dark_text, fl_color light_text)
{
	unsigned l = lum(fill) + 50, d = lum(dark_text) + 50, w = lum(light_text) + 50;
	unsigned cd = l > d ? l * 100 / d : d * 100 / l, cw = l > w ? l * 100 / w : w * 100 / l;
	return cd >= cw ? dark_text : light_text;
}

static void theme_base(int dark)
{
	if (dark) {
		fl_th = (fl_theme){
			.bg = FL_HEX(0x1d2125), .panel = FL_HEX(0x272c33), .raised = FL_HEX(0x343a43),
			.line = FL_HEX(0x3a4048), .text = FL_HEX(0xd8dce0), .dim = FL_HEX(0x8a9199),
			.accent = FL_HEX(0x3f9b63), .on_accent = FL_HEX(0x0e1a13), .select = FL_HEX(0x2f4a3b),
			.strong = FL_HEX(0xf0f2f4), .term_bg = FL_HEX(0x16191c), .err = FL_HEX(0xe06c6c), .dark = 1,
		};
	} else {
		fl_th = (fl_theme){
			.bg = FL_HEX(0xdfe3e7), .panel = FL_HEX(0xf4f5f7), .raised = FL_HEX(0xffffff),
			.line = FL_HEX(0xc6ccd3), .text = FL_HEX(0x1d2125), .dim = FL_HEX(0x565e67),
			.accent = FL_HEX(0x2c7a4b), .on_accent = FL_HEX(0xffffff), .select = FL_HEX(0xcfe6d8),
			.strong = FL_HEX(0x1d2125), .term_bg = FL_HEX(0x16191c), .err = FL_HEX(0xb83b3b), .dark = 0,
		};
	}
}

void fl_theme_set(int dark)
{
	theme_base(dark);
	/* the user's accent (the default green leaves the tokens above exactly as they were) */
	const char *v = fl_conf_get("accent", NULL);
	if (!v || !*v || !strcmp(v, "green"))
		return;
	fl_color a = fl_accent_current(dark);
	fl_th.accent = a;
	fl_th.on_accent = dark ? on_fill(a, FL_HEX(0x0e1418), FL_HEX(0xf0f2f4))
	                       : on_fill(a, FL_HEX(0x0e1418), FL_HEX(0xffffff));
	fl_th.select = fl_mix(fl_th.panel, a, dark ? 70 : 56);
}

/* ---- the file ----------------------------------------------------------------- */

#define MAXKEYS 64
static struct {
	char key[32], val[96];
} kv[MAXKEYS];
static int nkv;
static char path_buf[512];

const char *fl_conf_path(void)
{
	const char *e = getenv("FLUI_CONF"), *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
	if (e && *e)
		snprintf(path_buf, sizeof path_buf, "%s", e);
	else if (x && *x)
		snprintf(path_buf, sizeof path_buf, "%s/freelfs/desktop.conf", x);
	else
		snprintf(path_buf, sizeof path_buf, "%s/.config/freelfs/desktop.conf", h ? h : "");
	return path_buf;
}

static void kv_set(const char *k, const char *v)
{
	for (int i = 0; i < nkv; i++)
		if (!strcmp(kv[i].key, k)) {
			snprintf(kv[i].val, sizeof kv[i].val, "%s", v);
			return;
		}
	if (nkv < MAXKEYS) {
		snprintf(kv[nkv].key, sizeof kv[nkv].key, "%s", k);
		snprintf(kv[nkv].val, sizeof kv[nkv].val, "%s", v);
		nkv++;
	}
}

static void read_file(void)
{
	nkv = 0;
	FILE *f = fopen(fl_conf_path(), "r");
	if (!f)
		return;
	char line[160];
	while (fgets(line, sizeof line, f)) {
		char *s = line;
		while (*s == ' ' || *s == '\t')
			s++;
		if (*s == '#' || *s == '\n' || !*s)
			continue;
		char *eq = strchr(s, '=');
		if (!eq)
			continue;
		*eq = 0;
		char *v = eq + 1;
		size_t n = strlen(v);
		while (n && (v[n - 1] == '\n' || v[n - 1] == ' ' || v[n - 1] == '\r'))
			v[--n] = 0;
		char *k = s;
		size_t kn = strlen(k);
		while (kn && (k[kn - 1] == ' ' || k[kn - 1] == '\t'))
			k[--kn] = 0;
		kv_set(k, v);
	}
	fclose(f);
}

void fl_conf_load(void)
{
	read_file();
	fl_theme_set(strcmp(fl_conf_get("theme", "dark"), "light") != 0);
}

const char *fl_conf_get(const char *key, const char *def)
{
	for (int i = 0; i < nkv; i++)
		if (!strcmp(kv[i].key, key))
			return kv[i].val;
	return def;
}

int fl_conf_int(const char *key, int def)
{
	const char *v = fl_conf_get(key, NULL);
	return v ? atoi(v) : def;
}

int fl_conf_set(const char *key, const char *val)
{
	read_file(); /* keep what others wrote */
	kv_set(key, val);
	const char *path = fl_conf_path();
	char dir[512], tmp[560];
	snprintf(dir, sizeof dir, "%s", path);
	char *sl = strrchr(dir, '/');
	if (sl) {
		*sl = 0;
		for (char *p = dir + 1; *p; p++)
			if (*p == '/') {
				*p = 0;
				mkdir(dir, 0755);
				*p = '/';
			}
		mkdir(dir, 0755);
	}
	snprintf(tmp, sizeof tmp, "%s.%d", path, (int)getpid());
	FILE *f = fopen(tmp, "w");
	if (!f)
		return -1;
	for (int i = 0; i < nkv; i++)
		fprintf(f, "%s=%s\n", kv[i].key, kv[i].val);
	if (fclose(f) || rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	fl_theme_set(strcmp(fl_conf_get("theme", "dark"), "light") != 0);
	return 0;
}

/* ---- watching it ------------------------------------------------------------------ */

struct watch {
	fl_app *app;
	int fd;
	void (*cb)(void *);
	void *ud;
	char name[512];
};

static void watch_ready(void *ud, int fd, short rev)
{
	struct watch *w = ud;
	char buf[4096] __attribute__((aligned(8)));
	ssize_t n = read(fd, buf, sizeof buf);
	int hit = 0;
	for (char *p = buf; n > 0 && p < buf + n;) {
		struct inotify_event *e = (struct inotify_event *)p;
		if (e->len && !strcmp(e->name, w->name))
			hit = 1;
		p += sizeof(*e) + e->len;
	}
	if (!hit)
		return;
	fl_conf_load();
	fl_win_redraw_all(w->app);
	if (w->cb)
		w->cb(w->ud);
}

int fl_app_watch_conf(fl_app *a, void (*cb)(void *), void *ud)
{
	struct watch *w = calloc(1, sizeof(*w));
	if (!w)
		return -1;
	char dir[512];
	snprintf(dir, sizeof dir, "%s", fl_conf_path());
	char *sl = strrchr(dir, '/');
	snprintf(w->name, sizeof w->name, "%s", sl ? sl + 1 : dir);
	if (sl) {
		*sl = 0;
		for (char *p = dir + 1; *p; p++)
			if (*p == '/') {
				*p = 0;
				mkdir(dir, 0755);
				*p = '/';
			}
		mkdir(dir, 0755);
	}
	w->fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
	if (w->fd < 0 || inotify_add_watch(w->fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE) < 0) {
		if (w->fd >= 0)
			close(w->fd);
		free(w);
		return -1;
	}
	w->app = a;
	w->cb = cb;
	w->ud = ud;
	return fl_fd_add(a, w->fd, 1 /* POLLIN */, watch_ready, w);
}
