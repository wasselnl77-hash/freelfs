/* menu.c -- the start menu: search, categories, programs, session buttons
 * SPDX-License-Identifier: MIT
 *
 * Programs come from the .desktop files in the applications directory of
 * every data directory of the XDG spec: $XDG_DATA_HOME (default
 * ~/.local/share), then $XDG_DATA_DIRS (default /usr/local/share:/usr/share).
 * What Flatpak exports (the person's ~/.local/share/flatpak/exports/share and
 * /var/lib/flatpak/exports/share) is looked at even when the login did not
 * put those in XDG_DATA_DIRS. /usr/share is always included. $FLPANEL_APPS
 * replaces all of it with one directory, for tests.
 * Besides the standard keys, X-FreeLFS-Glyph=X sets the character on the
 * program's tile; without it the tile shows the first letter of the name.
 */
#include <ctype.h>
#include <dirent.h>
#include <flui.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "flpanel.h"

enum { ID_SEARCH = 1, ID_CAT = 10, ID_LIST = 30, ID_LOCK = 40, ID_LOGOUT, ID_RESTART, ID_SHUTDOWN };

static const char *const cat_names[CAT_COUNT] = {"All programs", "Internet", "Office", "System", "Settings", "Recent"};

/* ---- .desktop files ------------------------------------------------------------------------- */

static unsigned cats_of(const char *list)
{
	unsigned m = 1u << CAT_ALL;
	char buf[256];
	snprintf(buf, sizeof buf, "%s", list);
	for (char *t = strtok(buf, ";"); t; t = strtok(NULL, ";")) {
		if (!strcmp(t, "Network") || !strcmp(t, "WebBrowser") || !strcmp(t, "Email"))
			m |= 1u << CAT_INTERNET;
		else if (!strcmp(t, "Office") || !strcmp(t, "TextEditor"))
			m |= 1u << CAT_OFFICE;
		else if (!strcmp(t, "System") || !strcmp(t, "Utility") || !strcmp(t, "TerminalEmulator") ||
		         !strcmp(t, "FileManager") || !strcmp(t, "PackageManager"))
			m |= 1u << CAT_SYSTEM;
		else if (!strcmp(t, "Settings"))
			m |= 1u << CAT_SETTINGS;
	}
	return m;
}

/* Is PROG (a path, or a name looked up in PATH) an executable file? */
static int runnable(const char *prog)
{
	if (!prog[0])
		return 0;
	if (strchr(prog, '/'))
		return access(prog, X_OK) == 0;
	const char *path = getenv("PATH");
	char dirs[1024], full[1100];
	snprintf(dirs, sizeof dirs, "%s", path && *path ? path : "/sysflfs/bin:/usr/bin:/bin");
	for (char *d = strtok(dirs, ":"); d; d = strtok(NULL, ":")) {
		snprintf(full, sizeof full, "%s/%s", d, prog);
		if (access(full, X_OK) == 0)
			return 1;
	}
	return 0;
}

/* The program an Exec line starts: its first word, after "env NAME=value". */
static void exec_prog(const char *exec, char *out, size_t cap)
{
	const char *s = exec;
	for (int round = 0; round < 8; round++) {
		while (*s == ' ')
			s++;
		char q = (*s == '"' || *s == '\'') ? *s++ : 0;
		size_t n = 0;
		while (s[n] && (q ? s[n] != q : s[n] != ' '))
			n++;
		snprintf(out, cap, "%.*s", (int)n, s);
		s += n + (q && s[n] ? 1 : 0);
		if (!strcmp(out, "env") || strchr(out, '=')) /* env, NAME=value: look further */
			continue;
		return;
	}
}

static int load_file(const char *path, struct entry *e)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	memset(e, 0, sizeof(*e));
	char line[512], catl[256] = "", tryexec[256] = "";
	int in = 0, hidden = 0, app = 1;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '[') {
			in = !strcmp(line, "[Desktop Entry]");
			continue;
		}
		if (!in)
			continue;
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		const char *k = line, *v = eq + 1;
		if (!strcmp(k, "Name") && !e->name[0])
			snprintf(e->name, sizeof e->name, "%s", v);
		else if (!strcmp(k, "Comment") && !e->what[0])
			snprintf(e->what, sizeof e->what, "%s", v);
		else if (!strcmp(k, "Exec"))
			snprintf(e->exec, sizeof e->exec, "%s", v);
		else if (!strcmp(k, "Categories"))
			snprintf(catl, sizeof catl, "%s", v);
		else if (!strcmp(k, "TryExec"))
			snprintf(tryexec, sizeof tryexec, "%s", v);
		else if (!strcmp(k, "Terminal"))
			e->terminal = !strcmp(v, "true");
		else if (!strcmp(k, "NoDisplay") || !strcmp(k, "Hidden"))
			hidden |= !strcmp(v, "true");
		else if (!strcmp(k, "Type"))
			app = !strcmp(v, "Application");
		else if (!strcmp(k, "X-FreeLFS-Desktop-Icon"))
			e->desktop = !strcmp(v, "true");
		else if (!strcmp(k, "X-FreeLFS-Glyph"))
			snprintf(e->glyph, sizeof e->glyph, "%s", v);
	}
	fclose(f);
	if (hidden || !app || !e->name[0] || !e->exec[0])
		return -1;
	/* programs that are not installed here (GVim without its X11 build) stay out */
	char prog[256];
	exec_prog(e->exec, prog, sizeof prog);
	if ((tryexec[0] && !runnable(tryexec)) || !runnable(prog))
		return -1;
	e->cats = cats_of(catl);
	if (!e->glyph[0]) {
		unsigned char c = (unsigned char)e->name[0];
		int n = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
		memcpy(e->glyph, e->name, (size_t)n);
		if (n == 1)
			e->glyph[0] = (char)toupper(c);
	}
	return 0;
}

static int cmp_entry(const void *a, const void *b)
{
	return strcasecmp(((const struct entry *)a)->name, ((const struct entry *)b)->name);
}

static void scan_dir(struct panel *p, const char *dir, int *cap)
{
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *de;
	while ((de = readdir(d))) {
		size_t n = strlen(de->d_name);
		if (n < 9 || strcmp(de->d_name + n - 8, ".desktop"))
			continue;
		char path[600];
		snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
		struct entry e;
		if (load_file(path, &e) < 0)
			continue;
		int dup = 0;
		for (int i = 0; i < p->napps; i++)
			if (!strcmp(p->apps[i].name, e.name))
				dup = 1;
		if (dup)
			continue;
		if (p->napps == *cap) {
			*cap = *cap ? *cap * 2 : 32;
			p->apps = realloc(p->apps, (size_t)*cap * sizeof(*p->apps));
			if (!p->apps) {
				p->napps = 0;
				closedir(d);
				return;
			}
		}
		p->apps[p->napps++] = e;
	}
	closedir(d);
}

/* ---- recently used files: the freedesktop list (~/.local/share/recently-used.xbel), read only when it
 * exists. The newest RECENT_N that still exist become entries of the Recent category: name, path,
 * and an Exec that opens them with gio. */
#define RECENT_N 8

static void percent_decode(char *s)
{
	char *o = s;
	for (char *i = s; *i; i++) {
		if (i[0] == '%' && isxdigit((unsigned char)i[1]) && isxdigit((unsigned char)i[2])) {
			char h[3] = {i[1], i[2], 0};
			*o++ = (char)strtol(h, NULL, 16);
			i += 2;
		} else {
			*o++ = *i;
		}
	}
	*o = 0;
}

/* the value of attribute NAME in an XML start tag line, or 0 */
static int xml_attr(const char *line, const char *name, char *out, size_t cap)
{
	char key[40];
	snprintf(key, sizeof key, " %s=\"", name);
	const char *v = strstr(line, key);
	if (!v)
		return 0;
	v += strlen(key);
	const char *end = strchr(v, '"');
	if (!end)
		return 0;
	size_t n = (size_t)(end - v);
	if (n >= cap)
		n = cap - 1;
	memcpy(out, v, n);
	out[n] = 0;
	return 1;
}

static void add_recent(struct panel *p, int *cap)
{
	const char *xd = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
	char path[512];
	if (xd && *xd)
		snprintf(path, sizeof path, "%s/recently-used.xbel", xd);
	else
		snprintf(path, sizeof path, "%s/.local/share/recently-used.xbel", h ? h : "");
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	struct {
		char file[512], when[40];
	} top[RECENT_N];
	int nt = 0;
	char line[2048];
	while (fgets(line, sizeof line, f)) {
		if (!strstr(line, "<bookmark"))
			continue;
		char href[1024] = "", when[40] = "", file[512];
		if (!xml_attr(line, "href", href, sizeof href) || strncmp(href, "file://", 7))
			continue;
		if (!xml_attr(line, "modified", when, sizeof when))
			when[0] = 0;
		snprintf(file, sizeof file, "%s", href + 7);
		percent_decode(file);
		if (file[0] != '/' || access(file, F_OK) != 0)
			continue;
		/* newest first: where it goes in the list of the best RECENT_N so far */
		int at = 0;
		while (at < nt && strcmp(top[at].when, when) >= 0)
			at++;
		if (at >= RECENT_N)
			continue;
		int last = nt < RECENT_N ? nt : RECENT_N - 1;
		for (int k = last; k > at; k--)
			top[k] = top[k - 1];
		snprintf(top[at].file, sizeof top[at].file, "%s", file);
		snprintf(top[at].when, sizeof top[at].when, "%s", when);
		if (nt < RECENT_N)
			nt++;
	}
	fclose(f);
	for (int k = 0; k < nt; k++) {
		char q[600];
		if (!fl_shq(q, sizeof q, top[k].file) || strlen(q) + 10 >= 256)
			continue;
		if (p->napps == *cap) {
			*cap = *cap ? *cap * 2 : 32;
			struct entry *n = realloc(p->apps, (size_t)*cap * sizeof(*p->apps));
			if (!n)
				return;
			p->apps = n;
		}
		struct entry e;
		memset(&e, 0, sizeof e);
		const char *base = strrchr(top[k].file, '/');
		base = base && base[1] ? base + 1 : top[k].file;
		snprintf(e.name, sizeof e.name, "%s", base);
		snprintf(e.what, sizeof e.what, "%s", top[k].file);
		snprintf(e.exec, sizeof e.exec, "gio open %s", q);
		e.glyph[0] = (char)toupper((unsigned char)e.name[0]);
		e.glyph[1] = 0;
		e.cats = 1u << CAT_RECENT;
		p->apps[p->napps++] = e;
	}
}

/* Scan DATA/applications once: DATA is a data directory (a trailing slash is ignored). */
static void scan_data(struct panel *p, const char *data, int *cap, char seen[][400], int *nseen)
{
	char dir[400];
	size_t n = strlen(data);
	while (n > 1 && data[n - 1] == '/')
		n--;
	if (!n || n > 300)
		return;
	snprintf(dir, sizeof dir, "%.*s/applications", (int)n, data);
	for (int i = 0; i < *nseen; i++)
		if (!strcmp(seen[i], dir))
			return;
	if (*nseen < 32)
		snprintf(seen[(*nseen)++], 400, "%s", dir);
	scan_dir(p, dir, cap);
}

static void scan_all(struct panel *p, int *cap)
{
	static char seen[32][400];
	int nseen = 0;
	const char *h = getenv("HOME"), *xh = getenv("XDG_DATA_HOME"), *xd = getenv("XDG_DATA_DIRS");
	char own[400] = "";
	if (xh && *xh)
		snprintf(own, sizeof own, "%s", xh);
	else if (h && *h)
		snprintf(own, sizeof own, "%s/.local/share", h);
	if (own[0]) {
		char fl[420];
		scan_data(p, own, cap, seen, &nseen);
		snprintf(fl, sizeof fl, "%s/flatpak/exports/share", own);
		scan_data(p, fl, cap, seen, &nseen);
	}
	scan_data(p, "/var/lib/flatpak/exports/share", cap, seen, &nseen);
	char dirs[2048];
	snprintf(dirs, sizeof dirs, "%s", xd && *xd ? xd : "/usr/local/share:/usr/share");
	for (char *d = strtok(dirs, ":"); d; d = strtok(NULL, ":"))
		if (d[0] == '/')
			scan_data(p, d, cap, seen, &nseen);
	scan_data(p, "/usr/share", cap, seen, &nseen);
}

static void refilter(struct panel *p)
{
	free(p->shown);
	p->shown = malloc((size_t)(p->napps + 1) * sizeof(int));
	p->nshown = 0;
	char q[96];
	size_t i = 0;
	for (; p->query[i] && i + 1 < sizeof q; i++)
		q[i] = (char)tolower((unsigned char)p->query[i]);
	q[i] = 0;
	for (int a = 0; a < p->napps && p->shown; a++) {
		struct entry *e = &p->apps[a];
		if (!(e->cats & (1u << p->cat)))
			continue;
		if (q[0]) {
			char hay[512];
			snprintf(hay, sizeof hay, "%s %s %s", e->name, e->what, e->exec);
			for (char *c = hay; *c; c++)
				*c = (char)tolower((unsigned char)*c);
			if (!strstr(hay, q))
				continue;
		}
		p->shown[p->nshown++] = a;
	}
	if (p->list.sel >= p->nshown)
		p->list.sel = p->nshown ? p->nshown - 1 : 0;
}

/* A fresh menu: apps re-read (they may have been installed), search empty. */
void menu_init(struct panel *p)
{
	free(p->apps);
	p->apps = NULL;
	p->napps = 0;
	int cap = 0;
	const char *only = getenv("FLPANEL_APPS");
	if (only) {
		scan_dir(p, only, &cap);
	} else {
		scan_all(p, &cap);
	}
	add_recent(p, &cap);
	if (p->napps)
		qsort(p->apps, (size_t)p->napps, sizeof(*p->apps), cmp_entry);
	p->cat = CAT_ALL;
	p->query[0] = 0;
	p->list = (fl_list_state){0};
	if (!p->user[0]) {
		struct passwd *pw = getpwuid(getuid());
		snprintf(p->user, sizeof p->user, "%s", pw ? pw->pw_name : "user");
	}
	refilter(p);
	fl_ui *ui = fl_win_ui(p->mw);
	ui->focus = ID_SEARCH;
	ui->active = 0;
}

/* ---- drawing -------------------------------------------------------------------------------- */

static void draw_item(fl_ui *ui, fl_rect r, int i, int hover, int selected, void *ud)
{
	struct panel *p = ud;
	struct entry *e = &p->apps[p->shown[i]];
	fl_canvas *cv = &ui->cv;
	fl_rect row = fl_r(r.x, r.y, r.w, 52);
	int hot = hover || selected;
	if (hot)
		fl_fill_rect(cv, row, fl_th.raised);
	fl_rect tile = fl_r(row.x + 10, row.y + 10, 32, 32);
	fl_fill_rect(cv, tile, hot ? fl_th.select : fl_th.raised);
	fl_text_in(cv, p->f_mono, tile, FL_CENTER, 0, fl_th.text, e->glyph);
	int nh = fl_font_height(ui->f_med), wh = fl_font_height(p->f_what);
	int tx = row.x + 10 + 32 + 12, ty = row.y + (52 - nh - 1 - wh) / 2;
	fl_rect clip = fl_r(tx, row.y, row.x + row.w - tx - 8, 52);
	fl_rect save = cv->clip;
	fl_clip(cv, clip);
	fl_text(cv, ui->f_med, tx, ty, fl_th.text, e->name, -1);
	fl_text(cv, p->f_what, tx, ty + nh + 1, fl_th.dim, e->what, -1);
	cv->clip = save;
}

void menu_draw(fl_ui *ui, void *ud)
{
	struct panel *p = ud;
	fl_canvas *cv = &ui->cv;
	const int W = 500, H = 520;
	fl_fill_rect(cv, fl_r(0, 0, W, H), fl_th.panel);
	fl_fill_rect(cv, fl_r(W - 1, 0, 1, H), fl_th.line);
	fl_fill_rect(cv, fl_r(0, H - 1, W, 1), fl_th.line);
	fl_fill_rect(cv, fl_r(0, 0, W, 2), fl_th.accent);

	if (ui->key == XKB_KEY_Escape) {
		ui->key = 0;
		menu_close(p);
		return;
	}

	/* search */
	fl_text(cv, p->f_cap, 12, 14, fl_th.dim, "Search programs", -1);
	int er = fl_textfield(ui, ID_SEARCH, fl_r(12, 34, W - 1 - 24, 34), p->query, sizeof p->query,
	                      "Type to search", NULL);
	if (er & FL_EDIT_CHANGED) {
		p->list.sel = 0;
		p->list.scroll = 0;
		refilter(p);
	}
	if ((er & FL_EDIT_ENTER) && p->nshown) {
		struct entry *e = &p->apps[p->shown[p->list.sel]];
		launch(e->exec, e->terminal);
		menu_close(p);
		return;
	}
	fl_fill_rect(cv, fl_r(0, 80, W - 1, 1), fl_th.line);

	/* categories */
	fl_rect nav = fl_r(0, 81, 150, 471 - 81);
	fl_fill_rect(cv, nav, fl_th.bg);
	fl_fill_rect(cv, fl_r(149, nav.y, 1, nav.h), fl_th.line);
	for (int i = 0; i < CAT_COUNT; i++) {
		fl_rect r = fl_r(0, nav.y + 8 + i * 34, 149, 34);
		int res = fl_widget(ui, ID_CAT + (uint32_t)i, r, 0);
		int on = i == p->cat;
		if (!on && (res & FL_HOVER))
			fl_fill_rect(cv, r, fl_th.panel);
		if (on)
			fl_fill_rect(cv, fl_r(r.x, r.y, 3, r.h), fl_th.accent);
		fl_text_in(cv, ui->f_reg, fl_r(r.x + 12, r.y, r.w - 24, r.h), FL_LEFT, 0,
		           on || (res & FL_HOVER) ? fl_th.text : fl_th.dim, cat_names[i]);
		if ((res & FL_CLICK) && !on) {
			p->cat = i;
			p->list.sel = 0;
			p->list.scroll = 0;
			refilter(p);
		}
	}

	/* programs */
	fl_rect lr = fl_r(156, 87, W - 1 - 150 - 12, 378);
	if (p->nshown) {
		int act = fl_list(ui, ID_LIST, lr, p->nshown, 54, &p->list, draw_item, p);
		if (act >= 0) {
			struct entry *e = &p->apps[p->shown[act]];
			launch(e->exec, e->terminal);
			menu_close(p);
			return;
		}
	} else {
		fl_text_in(cv, ui->f_reg, fl_r(lr.x, lr.y, lr.w, 52), FL_LEFT, 10, fl_th.dim,
		           p->query[0] ? "No program matches" : "No programs here");
	}

	/* footer */
	fl_fill_rect(cv, fl_r(0, 471, W - 1, 1), fl_th.line);
	fl_text_in(cv, ui->f_med, fl_r(12, 472, 200, 47), FL_LEFT, 0, fl_th.text, p->user);
	int x = W - 1 - 8, y = 472 + (47 - 32) / 2;
	{
		/* each asks first (dialog.c): a countdown, then flpower does it */
		int w = fl_text_width(ui->f_reg, "Shut down", -1) + 22;
		x -= w;
		if (fl_button(ui, ID_SHUTDOWN, fl_r(x, y, w, 32), "Shut down", FL_BTN_OUTLINE)) {
			menu_close(p);
			dlg_power(p, POW_OFF);
			return;
		}
		x -= 4;
		w = fl_text_width(ui->f_reg, "Restart", -1) + 20;
		x -= w;
		if (fl_button(ui, ID_RESTART, fl_r(x, y, w, 32), "Restart", FL_BTN_PLAIN)) {
			menu_close(p);
			dlg_power(p, POW_RESTART);
			return;
		}
		x -= 4;
		w = fl_text_width(ui->f_reg, "Log out", -1) + 20;
		x -= w;
		if (fl_button(ui, ID_LOGOUT, fl_r(x, y, w, 32), "Log out", FL_BTN_PLAIN)) {
			menu_close(p);
			dlg_power(p, POW_LOGOUT);
			return;
		}
		x -= 4;
		w = fl_text_width(ui->f_reg, "Lock", -1) + 20;
		x -= w;
		if (access("/usr/bin/fllock", X_OK) == 0) {
			if (fl_button(ui, ID_LOCK, fl_r(x, y, w, 32), "Lock", FL_BTN_PLAIN)) {
				menu_close(p);
				launch("fllock", 0);
				return;
			}
		} else { /* nothing to lock with yet */
			fl_text_in(cv, ui->f_reg, fl_r(x, y, w, 32), FL_CENTER, 0, fl_mix(fl_th.dim, fl_th.panel, 130), "Lock");
		}
	}
}
