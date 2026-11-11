/* theme.c -- the accent colour and the wallpaper from desktop.conf, live.
 *
 * accent=ID|RRGGBB    colour of the volume indicator, the Alt-Tab frame, the lock screen's
 *                     caret (the toolkit's apps read the same key through libflui)
 * sounds=0|1         system sounds on (default) or off (read here, played by flsound, see sound.c)
 * wallpaper=NAME      accent (default): the 3D F in the accent colour; green, teal, blue,
 *                     amber, orange, rose: that variant; user:NAME: a picture imported by Settings;
 *                     /abs/path.flw: any .flw file
 *
 * Its own inotify watch on desktop.conf, so config.c stays as it is. A wallpaper change
 * reloads and repaints at once; an accent change shows the next time something is drawn.
 * SPDX-License-Identifier: MIT */
#include "comp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#define DEFAULT_WALL "wallpaper.flw"

/* the ids of the wallpaper variants (flsplash genart) are the accent ids of flpalette.h */
struct flpal PAL;

static char accent_id[24] = "green", wall_val[512] = "accent", wall_path[600];
static bool light;
static char conf_dir[512], conf_name[512];
static bool inited;
static bool sounds_on = true;

static void __attribute__((constructor)) pal_default(void)
{
	flpal_base(&PAL, 1);
}

static const char *share_dir(void)
{
	const char *e = getenv("FLCOMP_SHARE");
	return e && *e ? e : "/usr/share/flcomp";
}

static void conf_file(char *path, size_t n)
{
	const char *e = getenv("FLUI_CONF"), *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
	if (e && *e)
		snprintf(path, n, "%s", e);
	else if (x && *x)
		snprintf(path, n, "%s/freelfs/desktop.conf", x);
	else
		snprintf(path, n, "%s/.config/freelfs/desktop.conf", h ? h : "");
}

static void read_conf(void)
{
	char path[512], line[700];
	snprintf(accent_id, sizeof accent_id, "green");
	snprintf(wall_val, sizeof wall_val, "accent");
	sounds_on = true;
	light = false;
	conf_file(path, sizeof path);
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq)
			continue;
		*eq = 0;
		char *v = eq + 1;
		v[strcspn(v, "\r\n")] = 0;
		if (!strcmp(line, "accent") && *v)
			snprintf(accent_id, sizeof accent_id, "%s", v);
		else if (!strcmp(line, "theme"))
			light = !strcmp(v, "light");
		else if (!strcmp(line, "sounds"))
			sounds_on = atoi(v) != 0;
		else if (!strcmp(line, "wallpaper") && *v)
			snprintf(wall_val, sizeof wall_val, "%s", v);
	}
	if (f)
		fclose(f);
	flpal_make(&PAL, !light, accent_id);
}

/* the file to show now: the value of wallpaper= with fallbacks down to the green default */
const char *theme_wallpaper(void)
{
	char def[600];
	snprintf(def, sizeof def, "%s/" DEFAULT_WALL, share_dir());
	const char *name = wall_val;
	if (!strcmp(name, "accent"))
		name = accent_id;
	if (name[0] == '/') {
		snprintf(wall_path, sizeof wall_path, "%s", name);
	} else if (!strncmp(name, "user:", 5) && !strchr(name + 5, '/') && name[5] != '.') {
		/* an imported picture: Settings keeps them in $XDG_DATA_HOME/freelfs/wallpapers */
		const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
		if (x && *x)
			snprintf(wall_path, sizeof wall_path, "%s/freelfs/wallpapers/%s.flw", x, name + 5);
		else
			snprintf(wall_path, sizeof wall_path, "%s/.local/share/freelfs/wallpapers/%s.flw", h ? h : "", name + 5);
	} else {
		snprintf(wall_path, sizeof wall_path, "%s", def);
		for (size_t i = 1; i < FLPAL_ACCENT_COUNT; i++)
			if (!strcmp(name, flpal_accents[i].id))
				snprintf(wall_path, sizeof wall_path, "%s/wallpaper-%s.flw", share_dir(), name);
	}
	if (access(wall_path, R_OK) != 0)
		snprintf(wall_path, sizeof wall_path, "%s", def);
	return wall_path;
}

bool theme_sounds(void)
{
	return sounds_on;
}

uint32_t theme_accent(void)
{
	return PAL.accent & 0xffffffu;
}

static int theme_cb(int fd, uint32_t mask, void *data)
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
	char old[600];
	snprintf(old, sizeof old, "%s", wall_path);
	struct flpal was = PAL;
	read_conf();
	if (memcmp(&was, &PAL, sizeof PAL))
		damage_all();	/* a new scheme or accent: the indicator, overview, lock and X frames follow */
	if (strcmp(old, theme_wallpaper()) && !getenv("FLCOMP_WALLPAPER")) {
		wall_reload();
		damage_all();
	}
	return 0;
}

void theme_init(void)
{
	if (inited)
		return;
	inited = true;
	read_conf();
	theme_wallpaper();
	char path[512];
	conf_file(path, sizeof path);
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
	wl_event_loop_add_fd(C.loop, fd, WL_EVENT_READABLE, theme_cb, NULL);
}
