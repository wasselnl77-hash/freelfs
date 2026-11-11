/* presets.c -- named looks: three built in, the user's own as small files
 * SPDX-License-Identifier: MIT
 *
 * A theme is a set of desktop.conf keys (theme_keys below). Applying one writes them to
 * desktop.conf in a single write; a value of NULL removes the key, which is the default.
 * Built-in presets: "FreeLFS Dark" (every key back to the default: this is also Reset),
 * "FreeLFS Light", "Compact" and "Large" (those two only touch sizes: they keep your colours,
 * accent, font family and animation settings).
 * The user's presets live in <config>/freelfs/themes/NAME.theme next to desktop.conf, the same
 * key=value lines. Loading one sets the keys it has and resets the ones it lacks, and refuses
 * anything else in it: only the keys of theme_keys, with short printable values.
 */
#include "fltheme.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

const char *const theme_keys[] = {
	"theme", "accent", "font", "font_size", "buttons", "titlebar", "panel_position", "panel_height",
	"panel_icon_size", "animations", "animation_speed", NULL,
};

static const char *const defaults[][2] = {
	{"theme", "dark"}, {"accent", "green"}, {"font", "IBM Plex Sans"}, {"font_size", "13"},
	{"buttons", "right"}, {"titlebar", "normal"}, {"panel_position", "bottom"}, {"panel_height", "44"},
	{"panel_icon_size", "20"}, {"animations", "on"}, {"animation_speed", "1.0"},
};

const char *theme_default(const char *key)
{
	for (size_t i = 0; i < sizeof defaults / sizeof *defaults; i++)
		if (!strcmp(defaults[i][0], key))
			return defaults[i][1];
	return NULL;
}

/* every key back to the default */
#define ALL_DEFAULT \
	.n = 11, .kv = { {"theme", NULL}, {"accent", NULL}, {"font", NULL}, {"font_size", NULL}, {"buttons", NULL}, \
	                 {"titlebar", NULL}, {"panel_position", NULL}, {"panel_height", NULL}, \
	                 {"panel_icon_size", NULL}, {"animations", NULL}, {"animation_speed", NULL} }

const struct preset builtin_presets[] = {
	{ .name = "FreeLFS Dark", .blurb = "The default: dark, green, square", ALL_DEFAULT },
	{ .name = "FreeLFS Light", .blurb = "Light colours, everything else as it was",
	  .n = 11, .kv = { {"theme", "light"}, {"accent", NULL}, {"font", NULL}, {"font_size", NULL}, {"buttons", NULL},
	                   {"titlebar", NULL}, {"panel_position", NULL}, {"panel_height", NULL},
	                   {"panel_icon_size", NULL}, {"animations", NULL}, {"animation_speed", NULL} } },
	{ .name = "Compact", .blurb = "Smaller text, bars and icons; keeps your colours",
	  .n = 4, .kv = { {"font_size", "12"}, {"titlebar", "compact"}, {"panel_height", "38"}, {"panel_icon_size", "18"} } },
	{ .name = "Large", .blurb = "Bigger text, bars and icons; keeps your colours",
	  .n = 4, .kv = { {"font_size", "15"}, {"titlebar", "normal"}, {"panel_height", "52"}, {"panel_icon_size", "24"} } },
};
const int n_builtin = (int)(sizeof builtin_presets / sizeof *builtin_presets);

static int same_value(const char *key, const char *cur, const char *want)
{
	/* an unset key means the default */
	const char *c = cur ? cur : theme_default(key), *w = want ? want : theme_default(key);
	if (!c || !w)
		return c == w;
	if (!strcmp(key, "animation_speed"))
		return (int)(atof(c) * 100 + 0.5) == (int)(atof(w) * 100 + 0.5);
	return !strcmp(c, w);
}

int preset_matches(const struct preset *p)
{
	for (int i = 0; i < p->n; i++)
		if (!same_value(p->kv[i][0], fl_conf_get(p->kv[i][0], NULL), p->kv[i][1]))
			return 0;
	return 1;
}

int preset_apply(const struct preset *p)
{
	const char *pairs[PRESET_MAX_KEYS * 2];
	for (int i = 0; i < p->n; i++) {
		pairs[2 * i] = p->kv[i][0];
		pairs[2 * i + 1] = p->kv[i][1];
	}
	int r = fl_conf_set_many(pairs, p->n);
	hints_write();
	return r;
}

int theme_reset(void)
{
	return preset_apply(&builtin_presets[0]);
}

/* ---- the user's own ------------------------------------------------------------------------ */

const char *themes_dir(void)
{
	static char dir[560];
	snprintf(dir, sizeof dir, "%s", fl_conf_path());
	char *sl = strrchr(dir, '/');
	if (sl)
		*sl = 0;
	size_t n = strlen(dir);
	snprintf(dir + n, sizeof dir - n, "/themes");
	return dir;
}

int name_ok(const char *name)
{
	size_t n = strlen(name);
	if (n < 1 || n > 32 || name[0] == ' ' || name[0] == '.' || name[n - 1] == ' ')
		return 0;
	for (size_t i = 0; i < n; i++) {
		char c = name[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_'))
			return 0;
	}
	return 1;
}

static void path_of(const char *name, char *out, size_t cap)
{
	snprintf(out, cap, "%s/%s.theme", themes_dir(), name);
}

static int cmp_name(const void *a, const void *b)
{
	return strcasecmp(((const struct user_preset *)a)->name, ((const struct user_preset *)b)->name);
}

int user_list(struct user_preset *out, int max)
{
	DIR *d = opendir(themes_dir());
	struct dirent *e;
	int n = 0;
	while (d && (e = readdir(d)) && n < max) {
		size_t l = strlen(e->d_name);
		if (l < 7 || strcmp(e->d_name + l - 6, ".theme"))
			continue;
		char name[sizeof out->name];
		if (l - 6 >= sizeof name)
			continue;
		memcpy(name, e->d_name, l - 6);
		name[l - 6] = 0;
		if (!name_ok(name))
			continue;
		snprintf(out[n++].name, sizeof out->name, "%s", name);
	}
	if (d)
		closedir(d);
	qsort(out, (size_t)n, sizeof *out, cmp_name);
	return n;
}

int user_save(const char *name)
{
	if (!name_ok(name))
		return -1;
	char dir[600], path[700], tmp[760];
	snprintf(dir, sizeof dir, "%s", themes_dir());
	for (char *p = dir + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			mkdir(dir, 0755);
			*p = '/';
		}
	mkdir(dir, 0755);
	path_of(name, path, sizeof path);
	snprintf(tmp, sizeof tmp, "%s.%d", path, (int)getpid());
	FILE *f = fopen(tmp, "w");
	if (!f)
		return -1;
	fprintf(f, "# FreeLFS theme: desktop.conf keys, loaded by Themes (fltheme)\n");
	for (int i = 0; theme_keys[i]; i++) {
		const char *v = fl_conf_get(theme_keys[i], NULL);
		if (v && *v)
			fprintf(f, "%s=%s\n", theme_keys[i], v);
	}
	if (fclose(f) || rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

static int known_key(const char *k)
{
	for (int i = 0; theme_keys[i]; i++)
		if (!strcmp(theme_keys[i], k))
			return 1;
	return 0;
}

int user_load(const char *name)
{
	if (!name_ok(name))
		return -1;
	char path[700];
	path_of(name, path, sizeof path);
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	static char vals[PRESET_MAX_KEYS][64];
	const char *got[PRESET_MAX_KEYS] = {0};
	char line[200];
	int found = 0;
	while (fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq)
			continue;
		*eq = 0;
		char *v = eq + 1;
		v[strcspn(v, "\r\n")] = 0;
		if (!known_key(line) || !*v || strlen(v) > 47)
			continue;
		int ok = 1;
		for (char *c = v; *c; c++)
			if ((unsigned char)*c < 0x20)
				ok = 0;
		if (!ok)
			continue;
		for (int i = 0; theme_keys[i]; i++)
			if (!strcmp(theme_keys[i], line)) {
				snprintf(vals[i], sizeof vals[i], "%s", v);
				got[i] = vals[i];
				found++;
			}
	}
	fclose(f);
	if (!found)
		return -1; /* not a theme file */
	const char *pairs[PRESET_MAX_KEYS * 2];
	int n = 0;
	for (int i = 0; theme_keys[i]; i++) {
		pairs[2 * n] = theme_keys[i];
		pairs[2 * n + 1] = got[i]; /* NULL: the theme does not have it, the default applies */
		n++;
	}
	int r = fl_conf_set_many(pairs, n);
	hints_write();
	return r;
}

int user_delete(const char *name)
{
	if (!name_ok(name))
		return -1;
	char path[700];
	path_of(name, path, sizeof path);
	return unlink(path);
}
