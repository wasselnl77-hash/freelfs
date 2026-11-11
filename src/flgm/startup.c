/* startup.c -- apps that start at login: .desktop files in the autostart directories
 * SPDX-License-Identifier: MIT
 *
 * ~/.config/autostart (XDG_CONFIG_HOME) is the user's own list; /etc/xdg/autostart
 * (XDG_CONFIG_DIRS) is the system's. Turning an app off writes Hidden=true into its user
 * file; for a system entry that is a small override file (X-FreeLFS-Override=true) that
 * is removed again when the app is turned back on.
 */
#include "gm.h"

#include <dirent.h>
#include <errno.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

const char *startup_user_dir(void)
{
	static char dir[512];
	const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
	if (x && *x)
		snprintf(dir, sizeof dir, "%s/autostart", x);
	else if (h && *h)
		snprintf(dir, sizeof dir, "%s/.config/autostart", h);
	else
		dir[0] = 0;
	return dir;
}

struct parsed {
	char name[96], comment[160], exec[256];
	int hidden, gnome_off, override, ok;
};

static void parse_file(const char *path, struct parsed *o)
{
	memset(o, 0, sizeof *o);
	FILE *f = fopen(path, "re");
	if (!f)
		return;
	char line[512];
	int in = 0;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '[') {
			in = !strcmp(line, "[Desktop Entry]");
			if (in)
				o->ok = 1;
			continue;
		}
		if (!in)
			continue;
		if (!strncmp(line, "Name=", 5) && !o->name[0])
			snprintf(o->name, sizeof o->name, "%s", line + 5);
		else if (!strncmp(line, "Comment=", 8) && !o->comment[0])
			snprintf(o->comment, sizeof o->comment, "%s", line + 8);
		else if (!strncmp(line, "Exec=", 5) && !o->exec[0])
			snprintf(o->exec, sizeof o->exec, "%s", line + 5);
		else if (!strcmp(line, "Hidden=true"))
			o->hidden = 1;
		else if (!strcmp(line, "X-GNOME-Autostart-enabled=false"))
			o->gnome_off = 1;
		else if (!strcmp(line, "X-FreeLFS-Override=true"))
			o->override = 1;
	}
	fclose(f);
}

static int add(struct startup **v, int *n, int *cap, const struct startup *s)
{
	if (*n == *cap) {
		*cap = *cap ? *cap * 2 : 16;
		struct startup *nv = realloc(*v, (size_t)*cap * sizeof **v);
		if (!nv)
			return -1;
		*v = nv;
	}
	(*v)[(*n)++] = *s;
	return 0;
}

static int scan_dir(const char *dir, int user, struct startup **v, int *n, int *cap)
{
	DIR *d = opendir(dir);
	if (!d)
		return 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		size_t l = strlen(e->d_name);
		if (l < 9 || strcmp(e->d_name + l - 8, ".desktop") || e->d_name[0] == '.')
			continue;
		struct startup s;
		memset(&s, 0, sizeof s);
		snprintf(s.file, sizeof s.file, "%s", e->d_name);
		snprintf(s.path, sizeof s.path, "%s/%s", dir, e->d_name);
		struct parsed p;
		parse_file(s.path, &p);
		if (!p.ok)
			continue;
		int off = p.hidden || p.gnome_off;
		if (user) {
			/* an override of a system entry, or an entry of the user's own */
			int hit = -1;
			for (int i = 0; i < *n; i++)
				if (!(*v)[i].user && !strcmp((*v)[i].file, s.file))
					hit = i;
			if (hit >= 0 && (p.override || !p.exec[0])) {
				(*v)[hit].enabled = !off;
				continue;
			}
			if (hit >= 0) {
				(*v)[hit].user = 1;
				snprintf((*v)[hit].path, sizeof (*v)[hit].path, "%s", s.path);
				snprintf((*v)[hit].name, sizeof (*v)[hit].name, "%s", p.name[0] ? p.name : s.file);
				snprintf((*v)[hit].comment, sizeof (*v)[hit].comment, "%s", p.comment);
				snprintf((*v)[hit].exec, sizeof (*v)[hit].exec, "%s", p.exec);
				(*v)[hit].enabled = !off;
				continue;
			}
		}
		if (!p.exec[0])
			continue;
		snprintf(s.name, sizeof s.name, "%s", p.name[0] ? p.name : e->d_name);
		snprintf(s.comment, sizeof s.comment, "%s", p.comment);
		snprintf(s.exec, sizeof s.exec, "%s", p.exec);
		s.user = user;
		s.enabled = !off;
		if (add(v, n, cap, &s) < 0)
			break;
	}
	closedir(d);
	return 0;
}

static int cmp_name(const void *a, const void *b)
{
	return strcasecmp(((const struct startup *)a)->name, ((const struct startup *)b)->name);
}

int startup_load(struct startup **out)
{
	struct startup *v = NULL;
	int n = 0, cap = 0;
	const char *xc = getenv("XDG_CONFIG_DIRS");
	if (!xc || !*xc)
		xc = "/etc/xdg";
	char dirs[1024], p[600];
	snprintf(dirs, sizeof dirs, "%s", xc);
	for (char *s = dirs, *e; s && *s; s = e ? e + 1 : NULL) {
		e = strchr(s, ':');
		if (e)
			*e = 0;
		snprintf(p, sizeof p, "%s/autostart", s);
		scan_dir(p, 0, &v, &n, &cap);
		if (!e)
			break;
	}
	const char *u = startup_user_dir();
	if (u[0])
		scan_dir(u, 1, &v, &n, &cap);
	if (n > 1)
		qsort(v, (size_t)n, sizeof *v, cmp_name);
	*out = v;
	return n;
}

/* ---- changing one ---------------------------------------------------------------------- */

static int mkdir_p(const char *path)
{
	char t[512];
	snprintf(t, sizeof t, "%s", path);
	for (char *c = t + 1; *c; c++) {
		if (*c != '/')
			continue;
		*c = 0;
		if (mkdir(t, 0755) < 0 && errno != EEXIST)
			return -1;
		*c = '/';
	}
	if (mkdir(t, 0755) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

static int write_atomic(const char *path, const char *data, size_t len)
{
	char tmp[600];
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "we");
	if (!f)
		return -1;
	if (fwrite(data, 1, len, f) != len || fclose(f) != 0) {
		unlink(tmp);
		return -1;
	}
	if (rename(tmp, path) < 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

int startup_set(const struct startup *s, int enabled)
{
	const char *ud = startup_user_dir();
	if (!ud[0]) {
		errno = ENOENT;
		return -1;
	}
	char up[700];
	snprintf(up, sizeof up, "%s/%s", ud, s->file);

	if (!s->user) { /* a system entry: an override file in the user's directory */
		if (enabled) {
			if (unlink(up) < 0 && errno != ENOENT)
				return -1;
			return 0;
		}
		if (mkdir_p(ud) < 0)
			return -1;
		static const char ov[] = "[Desktop Entry]\nType=Application\nHidden=true\nX-FreeLFS-Override=true\n";
		return write_atomic(up, ov, sizeof ov - 1);
	}

	/* the user's own file: change Hidden= in place, keep everything else */
	FILE *f = fopen(up, "re");
	if (!f)
		return -1;
	char *out = malloc(65536);
	if (!out) {
		fclose(f);
		errno = ENOMEM;
		return -1;
	}
	size_t o = 0;
	char line[1024];
	int in = 0, wrote_head = 0, override = 0;
	while (fgets(line, sizeof line, f)) {
		char t[1024];
		snprintf(t, sizeof t, "%s", line);
		t[strcspn(t, "\r\n")] = 0;
		if (!strcmp(t, "X-FreeLFS-Override=true"))
			override = 1;
		if (t[0] == '[') {
			in = !strcmp(t, "[Desktop Entry]");
			if (in && !wrote_head) {
				wrote_head = 1;
				o += (size_t)snprintf(out + o, 65536 - o, "%s", line);
				if (!enabled)
					o += (size_t)snprintf(out + o, 65536 - o, "Hidden=true\n");
				continue;
			}
		}
		if (in && (!strncmp(t, "Hidden=", 7) || !strncmp(t, "X-GNOME-Autostart-enabled=", 26)))
			continue;
		if (o + strlen(line) + 1 < 65536)
			o += (size_t)snprintf(out + o, 65536 - o, "%s", line);
	}
	fclose(f);
	int r;
	if (enabled && override)
		r = unlink(up);
	else
		r = write_atomic(up, out, o);
	int e = errno;
	free(out);
	errno = e;
	return r;
}
