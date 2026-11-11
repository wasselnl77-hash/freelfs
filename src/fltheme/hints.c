/* hints.c -- tell GTK (and so Flatpak apps) about the colour scheme and the UI font
 * SPDX-License-Identifier: MIT
 *
 * GTK 3 and 4 read $XDG_CONFIG_HOME/gtk-3.0/settings.ini and gtk-4.0/settings.ini; Flatpak shows
 * both read-only to every app. Two keys are written, in [Settings], and nothing else of an
 * existing file is touched:
 *   gtk-application-prefer-dark-theme=true|false     from theme=
 *   gtk-font-name=IBM Plex Sans 10                   from font= and font_size= (px to pt at 96 dpi)
 * The other half, for apps that ask the desktop instead of reading files, is flportal: its Settings
 * portal answers org.freedesktop.appearance color-scheme (and accent-color) from the same
 * desktop.conf and emits SettingChanged, so Flatpak apps that follow the portal switch live.
 * Qt is not covered.
 */
#include "fltheme.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void mkdirs(char *path)
{
	for (char *p = path + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			mkdir(path, 0755);
			*p = '/';
		}
}

/* key=val inside [section], keeping every other line; 0, or -1 when the file cannot be written */
static int ini_set(const char *path, const char *section, const char *key, const char *val)
{
	char *old = NULL;
	size_t n = 0;
	FILE *f = fopen(path, "r");
	if (f) {
		old = calloc(1, 65537);
		if (old)
			n = fread(old, 1, 65536, f);
		fclose(f);
	}
	size_t cap = n + strlen(section) + strlen(key) + strlen(val) + 64;
	char *out = calloc(1, cap);
	if (!out) {
		free(old);
		return -1;
	}
	char head[96];
	snprintf(head, sizeof head, "[%s]", section);
	int in_sec = 0, done = 0, seen = 0;
	size_t o = 0;
	char *line = old ? strtok(old, "\n") : NULL;
	/* strtok skips empty lines: fine for an ini file we rewrite */
	while (line) {
		char *s = line;
		while (*s == ' ' || *s == '\t')
			s++;
		if (*s == '[') {
			if (in_sec && !done) { /* leaving the section without having seen the key: add it here */
				o += (size_t)snprintf(out + o, cap - o, "%s=%s\n", key, val);
				done = 1;
			}
			in_sec = !strncmp(s, head, strlen(head));
			if (in_sec)
				seen = 1;
		} else if (in_sec && !strncmp(s, key, strlen(key)) && (s[strlen(key)] == '=' || s[strlen(key)] == ' ')) {
			o += (size_t)snprintf(out + o, cap - o, "%s=%s\n", key, val);
			done = 1;
			line = strtok(NULL, "\n");
			continue;
		}
		o += (size_t)snprintf(out + o, cap - o, "%s\n", line);
		line = strtok(NULL, "\n");
	}
	if (!seen)
		o += (size_t)snprintf(out + o, cap - o, "%s\n", head);
	if (!done)
		o += (size_t)snprintf(out + o, cap - o, "%s=%s\n", key, val);
	free(old);
	char dir[600], tmp[640];
	snprintf(dir, sizeof dir, "%s", path);
	char *sl = strrchr(dir, '/');
	if (sl) {
		*sl = 0;
		mkdirs(dir);
		mkdir(dir, 0755);
	}
	snprintf(tmp, sizeof tmp, "%s.%d", path, (int)getpid());
	f = fopen(tmp, "w");
	if (!f) {
		free(out);
		return -1;
	}
	int bad = fwrite(out, 1, o, f) != o;
	free(out);
	if (fclose(f) || bad || rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

static const char *config_home(char *buf, size_t cap)
{
	const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
	if (x && *x)
		snprintf(buf, cap, "%s", x);
	else
		snprintf(buf, cap, "%s/.config", h ? h : "");
	return buf;
}

int hints_write(void)
{
	char base[512], path[600], font[96];
	config_home(base, sizeof base);
	const char *family = fl_conf_get("font", "");
	int px = fl_conf_int("font_size", 13);
	px = px < 10 ? 10 : px > 18 ? 18 : px;
	snprintf(font, sizeof font, "%s %d", *family ? family : "IBM Plex Sans", (px * 72 + 48) / 96);
	const char *dark = strcmp(fl_conf_get("theme", "dark"), "light") ? "true" : "false";
	int bad = 0;
	static const char *const dirs[] = {"gtk-3.0", "gtk-4.0"};
	for (int i = 0; i < 2; i++) {
		snprintf(path, sizeof path, "%s/%s/settings.ini", base, dirs[i]);
		bad |= ini_set(path, "Settings", "gtk-application-prefer-dark-theme", dark);
		bad |= ini_set(path, "Settings", "gtk-font-name", font);
	}
	return bad ? -1 : 0;
}
