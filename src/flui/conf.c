/* conf.c -- theme tokens and desktop.conf
 * SPDX-License-Identifier: MIT
 */
#include "internal.h"
#include "flpalette.h"

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

/* the look settings that are not colours: see flui.h (fl_look) */
fl_look fl_lk = {
	.ui_font = "IBM Plex Sans", .font_px = 13, .buttons_left = 0, .title_h = 34, .panel_h = 44,
	.panel_top = 0, .panel_icon = 20, .anim = 2, .anim_speed = 1.0f,
};

/* ---- accent colour ---------------------------------------------------------------------
 * accent= in desktop.conf is one of the ids below or a hex colour (RRGGBB, #RRGGBB). Every
 * accent has a dark-theme and a light-theme shade; a hex colour is used as is on dark and
 * darkened on light. on_accent (text on an accent fill) is picked for contrast and the
 * selection fill is the accent thinly mixed into the panel colour. */

/* the table lives in flpalette.h, shared with the compositor */
const fl_accent fl_accents[FL_ACCENT_COUNT] = {
#define A(i) {flpal_accents[i].id, flpal_accents[i].name, FL_HEX(flpal_accents[i].dark), FL_HEX(flpal_accents[i].light)}
	A(0), A(1), A(2), A(3), A(4), A(5),
#undef A
};

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
	return flpal_accent(fl_conf_get("accent", "green"), dark);
}

static void pal_to_theme(const struct flpal *p)
{
	fl_th = (fl_theme){
		.bg = p->bg, .panel = p->panel, .raised = p->raised, .line = p->line, .text = p->text,
		.dim = p->dim, .accent = p->accent, .on_accent = p->on_accent, .select = p->select,
		.strong = p->strong, .term_bg = p->term_bg, .err = p->err, .dark = p->dark,
	};
}

void fl_theme_set(int dark)
{
	struct flpal p;
	flpal_make(&p, dark, fl_conf_get("accent", NULL));
	pal_to_theme(&p);
}

/* ---- the file ----------------------------------------------------------------- */

/* every key and value is kept whole, on the heap: nothing is cut or dropped when the file is written */
struct kvp {
	char *key, *val;
};
static struct kvp *kv;
static int nkv, capkv;
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

static void kv_clear(void)
{
	for (int i = 0; i < nkv; i++) {
		free(kv[i].key);
		free(kv[i].val);
	}
	nkv = 0;
}

static void kv_set(const char *k, const char *v)
{
	for (int i = 0; i < nkv; i++)
		if (!strcmp(kv[i].key, k)) {
			char *nv = strdup(v);
			if (nv) {
				free(kv[i].val);
				kv[i].val = nv;
			}
			return;
		}
	if (nkv == capkv) {
		int nc = capkv ? capkv * 2 : 32;
		struct kvp *n = realloc(kv, (size_t)nc * sizeof *n);
		if (!n)
			return;
		kv = n;
		capkv = nc;
	}
	char *kk = strdup(k), *vv = strdup(v);
	if (!kk || !vv) {
		free(kk);
		free(vv);
		return;
	}
	kv[nkv].key = kk;
	kv[nkv].val = vv;
	nkv++;
}

static void read_file(void)
{
	kv_clear();
	FILE *f = fopen(fl_conf_path(), "r");
	if (!f)
		return;
	char *line = NULL;
	size_t cap = 0;
	while (getline(&line, &cap, f) >= 0) { /* any line length */
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
		while (*v == ' ' || *v == '\t') /* theme = light reads as light */
			v++;
		size_t n = strlen(v);
		while (n && (v[n - 1] == '\n' || v[n - 1] == ' ' || v[n - 1] == '\t' || v[n - 1] == '\r'))
			v[--n] = 0;
		char *k = s;
		size_t kn = strlen(k);
		while (kn && (k[kn - 1] == ' ' || k[kn - 1] == '\t'))
			k[--kn] = 0;
		kv_set(k, v);
	}
	free(line);
	fclose(f);
}

/* the non-colour look settings (desktop.conf: font, font_size, buttons, titlebar, panel_*,
 * animations, animation_speed); fonts follow at once through fl_text_set_look */
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void look_apply(void)
{
	fl_look l = {.font_px = 13, .title_h = 34, .panel_h = 44, .panel_icon = 20, .anim = 2, .anim_speed = 1.0f};
	const char *v = fl_conf_get("font", "");
	snprintf(l.ui_font, sizeof l.ui_font, "%s", *v ? v : "IBM Plex Sans");
	l.font_px = (float)clampi(fl_conf_int("font_size", 13), 10, 18);
	l.buttons_left = !strcmp(fl_conf_get("buttons", "right"), "left");
	l.title_h = !strcmp(fl_conf_get("titlebar", "normal"), "compact") ? 28 : 34;
	l.panel_h = clampi(fl_conf_int("panel_height", 44), 36, 64);
	l.panel_top = !strcmp(fl_conf_get("panel_position", "bottom"), "top");
	l.panel_icon = clampi(fl_conf_int("panel_icon_size", 20), 16, 28);
	v = fl_conf_get("animations", "on");
	l.anim = !strcmp(v, "off") || !strcmp(v, "0") ? 0 : !strcmp(v, "reduced") ? 1 : 2;
	float sp = (float)atof(fl_conf_get("animation_speed", "1"));
	l.anim_speed = sp < 0.1f ? 0.1f : sp > 4.0f ? 4.0f : sp;
	fl_lk = l;
	fl_text_set_look(l.ui_font, l.font_px / 13.0f);
}

void fl_conf_load(void)
{
	read_file();
	fl_theme_set(strcmp(fl_conf_get("theme", "dark"), "light") != 0);
	look_apply();
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

static void kv_del(const char *k)
{
	for (int i = 0; i < nkv; i++)
		if (!strcmp(kv[i].key, k)) {
			free(kv[i].key);
			free(kv[i].val);
			kv[i] = kv[--nkv];
			return;
		}
}

/* pairs[0], pairs[1] = key, value; a NULL value removes the key. One write, so watchers wake once. */
int fl_conf_set_many(const char *const *pairs, int n)
{
	read_file(); /* keep what others wrote */
	for (int i = 0; i < n; i++) {
		if (pairs[2 * i + 1])
			kv_set(pairs[2 * i], pairs[2 * i + 1]);
		else
			kv_del(pairs[2 * i]);
	}
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
	look_apply();
	return 0;
}

int fl_conf_set(const char *key, const char *val)
{
	const char *p[2] = {key, val};
	return fl_conf_set_many(p, 1);
}

int fl_conf_unset(const char *key)
{
	const char *p[2] = {key, NULL};
	return fl_conf_set_many(p, 1);
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
	int old_title = fl_lk.title_h;
	fl_conf_load();
	if (fl_lk.title_h != old_title)
		fl_wins_relayout(w->app);
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
