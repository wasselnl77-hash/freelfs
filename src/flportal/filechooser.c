/* flportal -- the file chooser dialog
 * SPDX-License-Identifier: MIT
 *
 * org.freedesktop.impl.portal.FileChooser: OpenFile (one or several files, or folders),
 * SaveFile (a name in a folder) and SaveFiles (a folder for several files that the app named).
 * It looks like flfiles (same toolbar, Places and Drives, colours) but shows a list with
 * Name / Size / Modified, which is what picking wants. Places and the XDG user-dirs lookup
 * follow src/flfiles/flfiles.c on purpose.
 *
 * Immediate mode like the rest of flui: draw() reads the input and queues at most one action,
 * which runs at the end of the pass, so the entry list never changes while it is drawn.
 */
#include "flportal.h"

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum {
	ID_BACK = 1, ID_FWD, ID_UP, ID_PATH, ID_NEW, ID_LIST, ID_SCROLL, ID_NAME, ID_OK, ID_CANCEL,
	ID_FILTER, ID_HIDDEN, ID_SIDE = 100, ID_CHOICE = 200, ID_ROW = 1000, ID_POP = 0x40000,
	ID_DLG = 0x41000,
};
enum { TOOL_H = 44, SIDE_W = 176, ROW_H = 28, HEAD_H = 26, BAR_H = 58, INFO_H = 34, NAME_H = 54 };
enum { A_NONE, A_NAV, A_BACK, A_FWD, A_UP, A_ACCEPT, A_REFRESH, A_NEWDIR, A_DO_MKDIR, A_DO_ACCEPT,
       A_ENTER_ROW };
enum { OV_NONE, OV_POPUP, OV_CONFIRM, OV_PROMPT };
enum { POP_FILTER, POP_CHOICE };

struct entry {
	char *name;
	off_t size;
	time_t mtime;
	unsigned char dir, link;
};

struct place {
	char label[40], path[PATH_MAX];
	fl_icon ic;
	int drive;
};

struct filter {
	char *name;
	GVariant *pats; /* a(us), owned */
};

struct choice {
	char *id, *label;
	int n;           /* number of options; 0: a yes/no box */
	char **oid, **olabel;
	int sel;         /* option index */
	int on;          /* yes/no */
};

struct fc {
	struct dlg d;
	int kind, multiple, dirs_only;
	char accept[64];
	fl_font *small, *head;
	int inited;

	char cwd[PATH_MAX], pathbuf[PATH_MAX];
	char name[NAME_MAX + 1]; /* save: the file name */
	struct entry *ents;
	int n, cap;
	char *sel;
	int cur, anchor, scroll;
	int show_hidden;
	char *hist[32];
	int hn, hpos;
	struct place *places;
	int nplaces;

	struct filter *filters;
	int nfilters, curfilter;
	struct choice *choices;
	int nchoices;
	char **files; /* SaveFiles: the names the app wants to save */
	int nfiles;

	char msg[200];
	long long msg_ms;
	char type_buf[64];
	long long type_ms;

	int act;
	char act_path[PATH_MAX];
	int act_idx;

	int ov;
	int pop_kind, pop_which, pop_n, pop_sel;
	fl_rect pop_anchor;
	char prompt_text[NAME_MAX + 1], prompt_err[160];
	int ov_new;
	char confirm_path[PATH_MAX];
};

static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void say(struct fc *f, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void say(struct fc *f, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(f->msg, sizeof f->msg, fmt, ap);
	va_end(ap);
	f->msg_ms = now_ms();
}

static void join(char *out, size_t cap, const char *dir, const char *name)
{
	if (!strcmp(dir, "/"))
		snprintf(out, cap, "/%s", name);
	else
		snprintf(out, cap, "%s/%s", dir, name);
}

static const char *base_name(const char *p)
{
	const char *s = strrchr(p, '/');
	return s && s[1] ? s + 1 : p;
}

static int is_dir_path(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void fmt_size(off_t sz, char *out, size_t cap)
{
	if (sz < 1000)
		snprintf(out, cap, "%d B", (int)sz);
	else if (sz < 1000000)
		snprintf(out, cap, "%.1f kB", (double)sz / 1e3);
	else if (sz < 1000000000)
		snprintf(out, cap, "%.1f MB", (double)sz / 1e6);
	else
		snprintf(out, cap, "%.1f GB", (double)sz / 1e9);
}

/* ---- places (the same ones flfiles shows) ------------------------------------------------ */

static void place_add(struct fc *f, const char *label, const char *path, fl_icon ic, int drive)
{
	struct place *np = realloc(f->places, (size_t)(f->nplaces + 1) * sizeof *np);
	if (!np)
		return;
	f->places = np;
	struct place *p = &np[f->nplaces++];
	snprintf(p->label, sizeof p->label, "%s", label);
	snprintf(p->path, sizeof p->path, "%s", path);
	p->ic = ic;
	p->drive = drive;
}

/* XDG_xxx_DIR="$HOME/name" from user-dirs.dirs */
static int user_dir(const char *key, char *out, size_t cap)
{
	const char *home = getenv("HOME");
	if (!home)
		return 0;
	const char *cfg = getenv("XDG_CONFIG_HOME");
	char p[PATH_MAX];
	if (cfg && *cfg)
		snprintf(p, sizeof p, "%s/user-dirs.dirs", cfg);
	else
		snprintf(p, sizeof p, "%s/.config/user-dirs.dirs", home);
	FILE *fp = fopen(p, "r");
	if (!fp)
		return 0;
	char line[PATH_MAX + 64];
	size_t kl = strlen(key);
	int found = 0;
	while (fgets(line, sizeof line, fp)) {
		char *s = line;
		while (*s == ' ' || *s == '\t')
			s++;
		if (strncmp(s, key, kl) || s[kl] != '=')
			continue;
		s += kl + 1;
		if (*s == '"')
			s++;
		char *e = s + strcspn(s, "\"\r\n");
		*e = 0;
		if (!strncmp(s, "$HOME", 5))
			snprintf(out, cap, "%s%s", home, s + 5);
		else
			snprintf(out, cap, "%s", s);
		found = 1;
	}
	fclose(fp);
	return found;
}

static void places_load(struct fc *f)
{
	f->nplaces = 0;
	const char *home = getenv("HOME");
	if (home && *home && is_dir_path(home))
		place_add(f, "Home", home, FL_ICON_HOME, 0);
	static const struct { const char *key, *label, *dflt; } ud[] = {
		{"XDG_DESKTOP_DIR", "Desktop", "desktop"}, {"XDG_DOCUMENTS_DIR", "Documents", "documents"},
		{"XDG_DOWNLOAD_DIR", "Downloads", "downloads"}, {"XDG_MUSIC_DIR", "Music", "music"},
		{"XDG_PICTURES_DIR", "Pictures", "pictures"}, {"XDG_VIDEOS_DIR", "Videos", "videos"},
	};
	for (size_t i = 0; i < sizeof ud / sizeof *ud && home; i++) {
		char p[PATH_MAX];
		if (!user_dir(ud[i].key, p, sizeof p))
			snprintf(p, sizeof p, "%s/%s", home, ud[i].dflt);
		if (!is_dir_path(p) || !strcmp(p, home))
			continue;
		place_add(f, ud[i].label, p, FL_ICON_FOLDER, 0);
	}
	if (is_dir_path("/sysflfs"))
		place_add(f, "System", "/sysflfs", FL_ICON_DRIVE, 1);
	else
		place_add(f, "System", "/", FL_ICON_DRIVE, 1);
	FILE *fp = fopen("/proc/mounts", "r");
	if (fp) {
		char line[1024];
		while (fgets(line, sizeof line, fp)) {
			char dev[256], mp[PATH_MAX];
			if (sscanf(line, "%255s %4095s", dev, mp) != 2)
				continue;
			char dec[PATH_MAX]; /* mount points escape spaces as \040 */
			size_t o = 0;
			for (size_t i = 0; mp[i] && o + 1 < sizeof dec; i++) {
				if (mp[i] == '\\' && mp[i + 1] >= '0' && mp[i + 1] <= '3' && strlen(mp + i) >= 4) {
					dec[o++] = (char)(((mp[i + 1] - '0') << 6) | ((mp[i + 2] - '0') << 3) | (mp[i + 3] - '0'));
					i += 3;
				} else {
					dec[o++] = mp[i];
				}
			}
			dec[o] = 0;
			if (strncmp(dec, "/drives/", 8) || !dec[8])
				continue;
			place_add(f, base_name(dec), dec, FL_ICON_DRIVE, 1);
		}
		fclose(fp);
	}
}

/* ---- the folder ---------------------------------------------------------------------- */

static void free_entries(struct fc *f)
{
	for (int i = 0; i < f->n; i++)
		free(f->ents[i].name);
	f->n = 0;
}

static int cmp_entry(const void *a, const void *b)
{
	const struct entry *x = a, *y = b;
	if (x->dir != y->dir)
		return y->dir - x->dir;
	int c = strcasecmp(x->name, y->name);
	return c ? c : strcmp(x->name, y->name);
}

/* does the name pass the chosen filter? (folders always do) */
static int filter_match(const struct fc *f, const char *name)
{
	if (f->nfilters == 0 || f->curfilter < 0)
		return 1;
	GVariant *pats = f->filters[f->curfilter].pats;
	gsize n = g_variant_n_children(pats);
	if (n == 0)
		return 1;
	char *ctype = NULL;
	int ok = 0;
	for (gsize i = 0; i < n && !ok; i++) {
		guint32 type;
		const char *pat;
		g_variant_get_child(pats, i, "(u&s)", &type, &pat);
		if (type == 0) {
			ok = fnmatch(pat, name, FNM_CASEFOLD) == 0;
		} else {
			if (!ctype) {
				gboolean unsure = FALSE;
				ctype = g_content_type_guess(name, NULL, 0, &unsure);
			}
			ok = g_content_type_is_a(ctype, pat);
		}
	}
	g_free(ctype);
	return ok;
}

static int scan(struct fc *f)
{
	DIR *d = opendir(f->cwd);
	if (!d)
		return -1;
	free_entries(f);
	struct dirent *de;
	while ((de = readdir(d))) {
		const char *nm = de->d_name;
		if (!strcmp(nm, ".") || !strcmp(nm, ".."))
			continue;
		if (nm[0] == '.' && !f->show_hidden)
			continue;
		char p[PATH_MAX];
		join(p, sizeof p, f->cwd, nm);
		struct stat st, lst;
		if (lstat(p, &lst) != 0)
			continue;
		int link = S_ISLNK(lst.st_mode);
		if (stat(p, &st) != 0)
			continue; /* a dangling link */
		int dir = S_ISDIR(st.st_mode);
		if (!dir && (f->dirs_only || !S_ISREG(st.st_mode) || !filter_match(f, nm)))
			continue;
		if (f->n == f->cap) {
			int nc = f->cap ? f->cap * 2 : 128;
			struct entry *ne = realloc(f->ents, (size_t)nc * sizeof *ne);
			if (!ne)
				break;
			f->ents = ne;
			f->cap = nc;
		}
		struct entry *e = &f->ents[f->n++];
		e->name = strdup(nm);
		e->size = st.st_size;
		e->mtime = st.st_mtime;
		e->dir = (unsigned char)dir;
		e->link = (unsigned char)link;
	}
	closedir(d);
	qsort(f->ents, (size_t)f->n, sizeof *f->ents, cmp_entry);
	free(f->sel);
	f->sel = calloc((size_t)(f->n ? f->n : 1), 1);
	f->cur = f->anchor = -1;
	f->scroll = 0;
	return 0;
}

static int load_dir(struct fc *f, const char *path, int push)
{
	char real[PATH_MAX];
	if (!realpath(path, real)) {
		say(f, "Cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	char old[PATH_MAX];
	snprintf(old, sizeof old, "%s", f->cwd);
	snprintf(f->cwd, sizeof f->cwd, "%s", real);
	if (scan(f) < 0) {
		int e = errno;
		snprintf(f->cwd, sizeof f->cwd, "%s", old);
		say(f, "Cannot open %s: %s", base_name(real), strerror(e));
		return -1;
	}
	if (push) {
		for (int i = f->hpos + 1; i < f->hn; i++)
			free(f->hist[i]);
		f->hn = f->hpos + 1;
		if (f->hn == (int)(sizeof f->hist / sizeof *f->hist)) {
			free(f->hist[0]);
			memmove(f->hist, f->hist + 1, sizeof(f->hist[0]) * (size_t)(f->hn - 1));
			f->hn--;
		}
		f->hist[f->hn++] = strdup(real);
		f->hpos = f->hn - 1;
	}
	return 0;
}

static void select_name(struct fc *f, const char *name)
{
	for (int i = 0; i < f->n; i++)
		if (!strcmp(f->ents[i].name, name)) {
			memset(f->sel, 0, (size_t)f->n);
			f->sel[i] = 1;
			f->cur = f->anchor = i;
			return;
		}
}

/* ---- selection ------------------------------------------------------------------------- */

static int count_sel(const struct fc *f)
{
	int c = 0;
	for (int i = 0; i < f->n; i++)
		c += f->sel[i];
	return c;
}

static void click_select(struct fc *f, int i, int mods)
{
	if (f->multiple && (mods & FL_MOD_SHIFT)) {
		int a = f->anchor < 0 ? i : f->anchor;
		if (!(mods & FL_MOD_CTRL))
			memset(f->sel, 0, (size_t)f->n);
		for (int k = a < i ? a : i; k <= (a < i ? i : a); k++)
			f->sel[k] = 1;
	} else if (f->multiple && (mods & FL_MOD_CTRL)) {
		f->sel[i] = !f->sel[i];
		f->anchor = i;
	} else {
		memset(f->sel, 0, (size_t)f->n);
		f->sel[i] = 1;
		f->anchor = i;
	}
	f->cur = i;
	if (f->kind == FC_SAVE && !f->ents[i].dir) /* a click on a file names the file */
		snprintf(f->name, sizeof f->name, "%s", f->ents[i].name);
}

static void move_cursor(struct fc *f, int to, int shift)
{
	if (f->n == 0)
		return;
	if (to < 0)
		to = 0;
	if (to >= f->n)
		to = f->n - 1;
	if (f->multiple && shift) {
		int a = f->anchor < 0 ? to : f->anchor;
		memset(f->sel, 0, (size_t)f->n);
		for (int k = a < to ? a : to; k <= (a < to ? to : a); k++)
			f->sel[k] = 1;
	} else {
		memset(f->sel, 0, (size_t)f->n);
		f->sel[to] = 1;
		f->anchor = to;
	}
	f->cur = to;
	if (f->kind == FC_SAVE && !f->ents[to].dir)
		snprintf(f->name, sizeof f->name, "%s", f->ents[to].name);
}

static void ensure_visible(struct fc *f, int view_h)
{
	if (f->cur < 0)
		return;
	int top = f->cur * ROW_H, bot = top + ROW_H;
	if (top < f->scroll)
		f->scroll = top;
	if (bot > f->scroll + view_h)
		f->scroll = bot - view_h;
	if (f->scroll < 0)
		f->scroll = 0;
}

static void typeahead(struct fc *f, const char *t)
{
	long long now = now_ms();
	size_t l = strlen(f->type_buf);
	if (now - f->type_ms > 1000)
		l = 0;
	f->type_ms = now;
	size_t tl = strlen(t);
	if (l + tl >= sizeof f->type_buf)
		return;
	memcpy(f->type_buf + l, t, tl + 1);
	l += tl;
	for (int i = 0; i < f->n; i++)
		if (!strncasecmp(f->ents[i].name, f->type_buf, l)) {
			move_cursor(f, i, 0);
			return;
		}
}

/* ---- answering ------------------------------------------------------------------------- */

/* a name that is free in dir ("name (2).ext") for SaveFiles */
static void unique_name(const char *dir, const char *name, char *out, size_t cap)
{
	char p[PATH_MAX];
	join(p, sizeof p, dir, name);
	if (access(p, F_OK) != 0) {
		snprintf(out, cap, "%s", p);
		return;
	}
	const char *dot = strrchr(name, '.');
	int stem = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
	for (int i = 2; i < 1000; i++) {
		char nn[NAME_MAX + 32];
		snprintf(nn, sizeof nn, "%.*s (%d)%s", stem, name, i, name + stem);
		join(p, sizeof p, dir, nn);
		if (access(p, F_OK) != 0)
			break;
	}
	snprintf(out, cap, "%s", p);
}

static void finish(struct fc *f, char **paths, int np)
{
	GVariantBuilder rb, ub;
	g_variant_builder_init(&rb, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_init(&ub, G_VARIANT_TYPE("as"));
	for (int i = 0; i < np; i++) {
		char *uri = g_filename_to_uri(paths[i], NULL, NULL);
		if (uri) {
			g_variant_builder_add(&ub, "s", uri);
			g_free(uri);
		}
	}
	g_variant_builder_add(&rb, "{sv}", "uris", g_variant_builder_end(&ub));
	if (f->nchoices) {
		GVariantBuilder cb;
		g_variant_builder_init(&cb, G_VARIANT_TYPE("a(ss)"));
		for (int i = 0; i < f->nchoices; i++) {
			struct choice *c = &f->choices[i];
			g_variant_builder_add(&cb, "(ss)", c->id,
			                      c->n ? c->oid[c->sel] : c->on ? "true" : "false");
		}
		g_variant_builder_add(&rb, "{sv}", "choices", g_variant_builder_end(&cb));
	}
	if (f->nfilters && f->curfilter >= 0) {
		struct filter *fl = &f->filters[f->curfilter];
		g_variant_builder_add(&rb, "{sv}", "current_filter",
		                      g_variant_new("(s@a(us))", fl->name, g_variant_ref(fl->pats)));
	}
	portal_reply(f->d.req, 0, g_variant_builder_end(&rb));
}

/* Build the answer from what is chosen. Returns 0 when it was sent, -1 with a message. */
static int do_accept(struct fc *f, int confirmed)
{
	char path[PATH_MAX];
	if (f->kind == FC_OPEN && !f->dirs_only) {
		int c = count_sel(f);
		if (!c) {
			say(f, "Select a file first.");
			return -1;
		}
		char **paths = calloc((size_t)c, sizeof *paths);
		int k = 0;
		for (int i = 0; i < f->n && paths; i++)
			if (f->sel[i] && !f->ents[i].dir) {
				join(path, sizeof path, f->cwd, f->ents[i].name);
				paths[k++] = strdup(path);
			}
		if (!k) {
			free(paths);
			say(f, "Select a file first.");
			return -1;
		}
		finish(f, paths, k);
		for (int i = 0; i < k; i++)
			free(paths[i]);
		free(paths);
		return 0;
	}
	if (f->kind == FC_SAVEFILES) { /* the app's files go into this folder */
		char **paths = calloc((size_t)(f->nfiles ? f->nfiles : 1), sizeof *paths);
		int k = 0;
		for (int i = 0; i < f->nfiles && paths; i++) {
			unique_name(f->cwd, f->files[i], path, sizeof path);
			paths[k++] = strdup(path);
		}
		finish(f, paths, k);
		for (int i = 0; i < k; i++)
			free(paths[i]);
		free(paths);
		return 0;
	}
	if (f->kind == FC_OPEN) { /* folders: the selected ones, else the one that is open */
		int c = count_sel(f);
		char **paths = calloc((size_t)(c ? c : 1), sizeof *paths);
		int k = 0;
		for (int i = 0; i < f->n && paths; i++)
			if (f->sel[i] && (f->multiple || k == 0)) {
				join(path, sizeof path, f->cwd, f->ents[i].name);
				paths[k++] = strdup(path);
			}
		if (paths && !k)
			paths[k++] = strdup(f->cwd);
		finish(f, paths, k);
		for (int i = 0; i < k; i++)
			free(paths[i]);
		free(paths);
		return 0;
	}
	/* save: cwd/name */
	char nm[NAME_MAX + 1];
	snprintf(nm, sizeof nm, "%s", f->name);
	size_t l = strlen(nm);
	while (l && (nm[l - 1] == ' ' || nm[l - 1] == '\t'))
		nm[--l] = 0;
	char *s = nm;
	while (*s == ' ')
		s++;
	if (!*s) {
		say(f, "Type a name for the file.");
		return -1;
	}
	if (strchr(s, '/')) {
		say(f, "A file name cannot contain a /.");
		return -1;
	}
	if (!strcmp(s, ".") || !strcmp(s, "..")) {
		say(f, "That is not a file name.");
		return -1;
	}
	join(path, sizeof path, f->cwd, s);
	struct stat st;
	if (stat(path, &st) == 0) {
		if (S_ISDIR(st.st_mode)) {
			if (load_dir(f, path, 1) == 0)
				f->name[0] = 0;
			return -1;
		}
		if (!confirmed) {
			snprintf(f->confirm_path, sizeof f->confirm_path, "%s", path);
			f->ov = OV_CONFIRM;
			return -1;
		}
	}
	if (access(f->cwd, W_OK) != 0) {
		say(f, "This folder is read-only.");
		return -1;
	}
	char *p = strdup(path);
	finish(f, &p, 1);
	free(p);
	return 0;
}

/* ---- overlays: popup menu, new folder, replace? -------------------------------------------- */

static void popup_open(struct fc *f, int kind, int which, fl_rect anchor, int n, int sel)
{
	f->ov = OV_POPUP;
	f->pop_kind = kind;
	f->pop_which = which;
	f->pop_n = n;
	f->pop_sel = sel;
	f->pop_anchor = anchor;
}

static const char *pop_label(struct fc *f, int i)
{
	return f->pop_kind == POP_FILTER ? f->filters[i].name : f->choices[f->pop_which].olabel[i];
}

#define POP_ITEM_H 30

static void draw_popup(fl_ui *ui, struct fc *f)
{
	fl_canvas *cv = &ui->cv;
	int w = f->pop_anchor.w > 220 ? f->pop_anchor.w : 220;
	for (int i = 0; i < f->pop_n; i++) {
		int tw = fl_text_width(ui->f_reg, pop_label(f, i), -1) + 28;
		if (tw > w)
			w = tw;
	}
	if (w > cv->w - 16)
		w = cv->w - 16;
	int h = f->pop_n * POP_ITEM_H + 8;
	int x = f->pop_anchor.x + f->pop_anchor.w - w, y = f->pop_anchor.y - h - 2;
	if (x < 8)
		x = 8;
	if (y < 4)
		y = f->pop_anchor.y + f->pop_anchor.h + 2;
	fl_rect box = fl_r(x, y, w, h);
	fl_fill_rect(cv, box, fl_th.raised);
	fl_stroke_rect(cv, box, 1, fl_th.line);
	if (ui->key == XKB_KEY_Escape) {
		ui->key = 0;
		f->ov = OV_NONE;
		return;
	}
	if (ui->key == XKB_KEY_Down || ui->key == XKB_KEY_Up) {
		f->pop_sel = (f->pop_sel + (ui->key == XKB_KEY_Down ? 1 : -1) + f->pop_n) % f->pop_n;
		ui->key = 0;
	}
	int pick = -1;
	if ((ui->key == XKB_KEY_Return || ui->key == XKB_KEY_KP_Enter) && f->pop_sel >= 0) {
		ui->key = 0;
		pick = f->pop_sel;
	}
	for (int i = 0; i < f->pop_n; i++) {
		fl_rect r = fl_r(x + 4, y + 4 + i * POP_ITEM_H, w - 8, POP_ITEM_H);
		int res = fl_widget(ui, ID_POP + (uint32_t)i, r, 0);
		if ((res & FL_HOVER) && (ui->mx || ui->my))
			f->pop_sel = i;
		if (i == f->pop_sel)
			fl_fill_rect(cv, r, fl_th.select);
		int cur = f->pop_kind == POP_FILTER ? f->curfilter == i : f->choices[f->pop_which].sel == i;
		fl_text_in(cv, cur ? ui->f_med : ui->f_reg, r, FL_LEFT, 10, cur ? fl_th.strong : fl_th.text, pop_label(f, i));
		if (res & FL_CLICK)
			pick = i;
	}
	if ((ui->press & (FL_BTN_L | FL_BTN_R)) && !fl_in(box, ui->mx, ui->my)) {
		f->ov = OV_NONE;
		ui->press = 0;
	}
	if (pick >= 0) {
		f->ov = OV_NONE;
		if (f->pop_kind == POP_FILTER) {
			f->curfilter = pick;
			f->act = A_REFRESH;
		} else {
			f->choices[f->pop_which].sel = pick;
		}
	}
}

static void draw_prompt(fl_ui *ui, struct fc *f)
{
	fl_canvas *cv = &ui->cv;
	fl_rect box;
	ui_modal_frame(ui, 400, 168, &box);
	if (f->ov_new) {
		ui->focus = ID_DLG;
		f->ov_new = 0;
	}
	int x = box.x + 24, w = box.w - 48, y = box.y + 22;
	fl_text(cv, ui->f_semi, x, y, fl_th.strong, "New folder", -1);
	y += 28;
	int ch = fl_textfield(ui, ID_DLG, fl_r(x, y, w, 36), f->prompt_text, sizeof f->prompt_text, NULL, ui->f_reg);
	if (ch & FL_EDIT_CHANGED)
		f->prompt_err[0] = 0;
	int enter = ch & FL_EDIT_ENTER;
	y += 42;
	if (f->prompt_err[0])
		fl_text_in(cv, f->small, fl_r(x, y, w, 18), FL_LEFT, 0, fl_th.err, f->prompt_err);
	int by = box.y + box.h - 24 - 34;
	int ok = fl_button(ui, ID_DLG + 1, fl_r(x + w - 96, by, 96, 34), "Create", FL_BTN_ACCENT);
	int cancel = fl_button(ui, ID_DLG + 2, fl_r(x + w - 200, by, 96, 34), "Cancel", FL_BTN_OUTLINE);
	if (ui->key == XKB_KEY_Escape) {
		ui->key = 0;
		cancel = 1;
	}
	if (ok || enter) {
		f->ov = OV_NONE;
		f->act = A_DO_MKDIR;
	} else if (cancel) {
		f->ov = OV_NONE;
	}
}

static void draw_confirm(fl_ui *ui, struct fc *f)
{
	fl_canvas *cv = &ui->cv;
	fl_rect box;
	ui_modal_frame(ui, 460, 160, &box);
	int x = box.x + 24, w = box.w - 48, y = box.y + 22;
	char s[NAME_MAX + 64];
	snprintf(s, sizeof s, "Replace \"%s\"?", base_name(f->confirm_path));
	char fit[NAME_MAX + 80];
	fl_text_fit(ui->f_semi, s, w, fit, sizeof fit);
	fl_text(cv, ui->f_semi, x, y, fl_th.strong, fit, -1);
	y += 28;
	fl_text_in(cv, ui->f_reg, fl_r(x, y, w, 20), FL_LEFT, 0, fl_th.text,
	           "A file with this name is already in the folder.");
	int by = box.y + box.h - 24 - 34;
	int ok = fl_button(ui, ID_DLG + 1, fl_r(x + w - 96, by, 96, 34), "Replace", FL_BTN_ACCENT);
	int cancel = fl_button(ui, ID_DLG + 2, fl_r(x + w - 200, by, 96, 34), "Cancel", FL_BTN_OUTLINE);
	if (ui->key == XKB_KEY_Escape) {
		ui->key = 0;
		cancel = 1;
	}
	if (ok) {
		f->ov = OV_NONE;
		f->act = A_DO_ACCEPT;
	} else if (cancel) {
		f->ov = OV_NONE;
	}
}

/* ---- actions ---------------------------------------------------------------------------------- */

static void go_history(struct fc *f, int d)
{
	int np = f->hpos + d;
	if (np < 0 || np >= f->hn)
		return;
	char p[PATH_MAX];
	snprintf(p, sizeof p, "%s", f->hist[np]);
	if (load_dir(f, p, 0) == 0)
		f->hpos = np;
}

static void go_up(struct fc *f)
{
	if (!strcmp(f->cwd, "/"))
		return;
	char dir[PATH_MAX], child[NAME_MAX + 1];
	snprintf(dir, sizeof dir, "%s", f->cwd);
	char *s = strrchr(dir, '/');
	snprintf(child, sizeof child, "%s", s + 1);
	if (s == dir)
		dir[1] = 0;
	else
		*s = 0;
	if (load_dir(f, dir, 1) == 0)
		select_name(f, child);
}

static void run_action(struct fc *f)
{
	int a = f->act;
	f->act = A_NONE;
	switch (a) {
	case A_NAV:
		load_dir(f, f->act_path, 1);
		break;
	case A_BACK:
		go_history(f, -1);
		break;
	case A_FWD:
		go_history(f, 1);
		break;
	case A_UP:
		go_up(f);
		break;
	case A_REFRESH: {
		char keep[NAME_MAX + 1] = "";
		if (f->cur >= 0 && f->cur < f->n)
			snprintf(keep, sizeof keep, "%s", f->ents[f->cur].name);
		int sc = f->scroll;
		scan(f);
		if (keep[0])
			select_name(f, keep);
		f->scroll = sc;
		break;
	}
	case A_ACCEPT:
		do_accept(f, 0);
		break;
	case A_DO_ACCEPT:
		do_accept(f, 1);
		break;
	case A_NEWDIR:
		f->ov = OV_PROMPT;
		f->ov_new = 1;
		f->prompt_text[0] = 0;
		f->prompt_err[0] = 0;
		break;
	case A_DO_MKDIR: {
		const char *nm = f->prompt_text;
		if (!*nm || strchr(nm, '/') || !strcmp(nm, ".") || !strcmp(nm, "..")) {
			say(f, "That is not a folder name.");
			break;
		}
		char p[PATH_MAX];
		join(p, sizeof p, f->cwd, nm);
		if (mkdir(p, 0777) != 0) {
			say(f, "Cannot make \"%s\": %s", nm, strerror(errno));
			break;
		}
		scan(f);
		select_name(f, nm);
		break;
	}
	case A_ENTER_ROW: {
		int i = f->act_idx;
		if (i < 0 || i >= f->n)
			break;
		if (f->ents[i].dir) {
			char p[PATH_MAX];
			join(p, sizeof p, f->cwd, f->ents[i].name);
			load_dir(f, p, 1);
		} else {
			if (f->kind == FC_SAVE)
				snprintf(f->name, sizeof f->name, "%s", f->ents[i].name);
			memset(f->sel, 0, (size_t)f->n);
			f->sel[i] = 1;
			do_accept(f, 0);
		}
		break;
	}
	}
}

/* ---- drawing ---------------------------------------------------------------------------------- */

static void draw_toolbar(fl_ui *ui, struct fc *f, fl_rect tb)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, tb, fl_th.panel);
	fl_fill_rect(cv, fl_r(tb.x, tb.y + tb.h - 1, tb.w, 1), fl_th.line);
	int x = tb.x + 8, y = tb.y + (tb.h - 32) / 2;
	struct { uint32_t id; fl_icon ic; int can; int act; } b[3] = {
		{ID_BACK, FL_ICON_BACK, f->hpos > 0, A_BACK}, {ID_FWD, FL_ICON_FORWARD, f->hpos + 1 < f->hn, A_FWD},
		{ID_UP, FL_ICON_UP, strcmp(f->cwd, "/") != 0, A_UP},
	};
	for (int i = 0; i < 3; i++) {
		fl_rect r = fl_r(x, y, 32, 32);
		if (b[i].can) {
			if (fl_icon_button(ui, b[i].id, r, b[i].ic, 14, fl_th.text))
				f->act = b[i].act;
		} else {
			fl_icon_draw(cv, b[i].ic, r.x + 9, r.y + 9, 14, fl_th.line);
		}
		x += 34;
	}
	x += 6;
	int can_new = f->kind != FC_OPEN || f->dirs_only;
	fl_rect pr = fl_r(x, y, tb.x + tb.w - 8 - x - (can_new ? 40 : 0), 32);
	if (ui->focus != ID_PATH)
		snprintf(f->pathbuf, sizeof f->pathbuf, "%s", f->cwd);
	int r = fl_textfield(ui, ID_PATH, pr, f->pathbuf, sizeof f->pathbuf, "Path", ui->f_reg);
	if (r & FL_EDIT_ENTER) {
		char p[PATH_MAX];
		const char *t = f->pathbuf;
		if (t[0] == '~' && (t[1] == '/' || !t[1]) && getenv("HOME"))
			snprintf(p, sizeof p, "%s%s", getenv("HOME"), t + 1);
		else if (t[0] == '/')
			snprintf(p, sizeof p, "%s", t);
		else
			join(p, sizeof p, f->cwd, t);
		struct stat st;
		if (stat(p, &st) == 0 && !S_ISDIR(st.st_mode)) {
			char dir[PATH_MAX];
			snprintf(dir, sizeof dir, "%s", p);
			char *s = strrchr(dir, '/');
			if (s == dir)
				dir[1] = 0;
			else if (s)
				*s = 0;
			if (load_dir(f, dir, 1) == 0)
				select_name(f, base_name(p));
		} else {
			snprintf(f->act_path, sizeof f->act_path, "%s", p);
			f->act = A_NAV;
		}
		ui->focus = ID_LIST;
	}
	if (can_new && fl_icon_button(ui, ID_NEW, fl_r(tb.x + tb.w - 8 - 32, y, 32, 32), FL_ICON_PLUS, 14, fl_th.text))
		f->act = A_NEWDIR;
}

static void draw_sidebar(fl_ui *ui, struct fc *f, fl_rect sb)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, sb, fl_th.panel);
	fl_fill_rect(cv, fl_r(sb.x + sb.w - 1, sb.y, 1, sb.h), fl_th.line);
	int y = sb.y + 14;
	for (int section = 0; section < 2; section++) {
		fl_text_ex(cv, f->head, sb.x + 16, y, fl_th.dim, section ? "DRIVES" : "PLACES", -1, 56);
		y += fl_font_height(f->head) + 8;
		for (int i = 0; i < f->nplaces; i++) {
			struct place *p = &f->places[i];
			if (p->drive != section)
				continue;
			fl_rect r = fl_r(sb.x, y, sb.w - 1, 30);
			int res = fl_widget(ui, ID_SIDE + (uint32_t)i, r, 0);
			int active = !strcmp(p->path, f->cwd);
			if (active)
				fl_fill_rect(cv, r, fl_th.select);
			else if (res & (FL_HOVER | FL_DOWN))
				fl_fill_rect(cv, r, fl_th.raised);
			if (active)
				fl_fill_rect(cv, fl_r(r.x, r.y, 3, r.h), fl_th.accent);
			fl_icon_draw(cv, p->ic, r.x + 16, r.y + 7, 16, active ? fl_th.accent : fl_th.dim);
			fl_text_in(cv, active ? ui->f_med : ui->f_reg, fl_r(r.x + 42, r.y, r.w - 48, r.h), FL_LEFT, 0,
			           active ? fl_th.strong : fl_th.text, p->label);
			if (res & FL_CLICK) {
				snprintf(f->act_path, sizeof f->act_path, "%s", p->path);
				f->act = A_NAV;
				ui->focus = ID_LIST;
			}
			y += 30;
		}
		y += 14;
	}
}

static void draw_row(fl_ui *ui, struct fc *f, fl_rect r, int i, int hover, int size_x, int date_x)
{
	fl_canvas *cv = &ui->cv;
	struct entry *e = &f->ents[i];
	int sel = f->sel[i], cur = i == f->cur && ui->focus == ID_LIST;
	if (sel)
		fl_fill_rect(cv, r, fl_th.select);
	else if (hover)
		fl_fill_rect(cv, r, fl_th.raised);
	if (cur)
		fl_stroke_rect(cv, r, 1, fl_th.accent);
	fl_icon_draw(cv, e->dir ? FL_ICON_FOLDER : FL_ICON_FILE, r.x + 12, r.y + 6, 16, e->dir ? fl_th.accent : fl_th.dim);
	fl_color fg = sel ? fl_th.strong : fl_th.text;
	char fit[NAME_MAX + 8];
	int nw = size_x - (r.x + 38) - 8;
	fl_text_fit(ui->f_reg, e->name, nw, fit, sizeof fit);
	fl_text_in(cv, ui->f_reg, fl_r(r.x + 38, r.y, nw, r.h), FL_LEFT, 0, fg, fit);
	if (!e->dir) {
		char sz[32];
		fmt_size(e->size, sz, sizeof sz);
		int w = fl_text_width(f->small, sz, -1);
		fl_text_in(cv, f->small, fl_r(date_x - 16 - w, r.y, w + 4, r.h), FL_LEFT, 0, fl_th.dim, sz);
	}
	if (date_x < r.x + r.w - 60) {
		char when[48];
		struct tm tm;
		localtime_r(&e->mtime, &tm);
		strftime(when, sizeof when, "%-d %b %Y  %H:%M", &tm);
		fl_text_in(cv, f->small, fl_r(date_x, r.y, r.x + r.w - date_x - 6, r.h), FL_LEFT, 0, fl_th.dim, when);
	}
}

static void draw_list(fl_ui *ui, struct fc *f, fl_rect g)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, g, fl_th.bg);
	fl_rect head = fl_cut_top(&g, HEAD_H);
	fl_fill_rect(cv, head, fl_th.panel);
	fl_fill_rect(cv, fl_r(head.x, head.y + head.h - 1, head.w, 1), fl_th.line);
	int date_x = g.x + g.w - 168, size_x;
	if (g.w < 520)
		date_x = g.x + g.w; /* narrow: no date column */
	size_x = date_x - 96;
	fl_text_ex(cv, f->head, head.x + 38, head.y + 7, fl_th.dim, "NAME", -1, 40);
	{
		int w = fl_text_width(f->head, "SIZE", -1) + 4 * 40 / 64;
		fl_text_ex(cv, f->head, date_x - 16 - w, head.y + 7, fl_th.dim, "SIZE", -1, 40);
	}
	if (date_x < g.x + g.w)
		fl_text_ex(cv, f->head, date_x, head.y + 7, fl_th.dim, "MODIFIED", -1, 40);

	int res = fl_widget(ui, ID_LIST, g, FL_FOCUSABLE | FL_NO_FOCUS_RING);
	int content = f->n * ROW_H;
	int mods = fl_app_mods(portal_app_get());
	if ((res & FL_HOVER) && ui->wheel) {
		f->scroll += ui->wheel * 56;
		ui->wheel = 0;
	}
	if (ui->focus == ID_LIST && ui->key) {
		uint32_t k = ui->key;
		int shift = ui->mods & FL_MOD_SHIFT, ctrl = ui->mods & FL_MOD_CTRL, alt = ui->mods & FL_MOD_ALT;
		int page = g.h / ROW_H;
		int handled = 1;
		if (ctrl) {
			switch (k) {
			case 'a': case 'A':
				if (f->multiple) {
					for (int i = 0; i < f->n; i++)
						f->sel[i] = f->dirs_only || !f->ents[i].dir;
				}
				break;
			case 'h': case 'H': f->show_hidden = !f->show_hidden; f->act = A_REFRESH; break;
			case 'l': case 'L': ui->focus = ID_PATH; break;
			case 'n': case 'N': if (shift && (f->kind != FC_OPEN || f->dirs_only)) f->act = A_NEWDIR; else handled = 0; break;
			default: handled = 0; break;
			}
		} else if (alt) {
			switch (k) {
			case XKB_KEY_Left: f->act = A_BACK; break;
			case XKB_KEY_Right: f->act = A_FWD; break;
			case XKB_KEY_Up: f->act = A_UP; break;
			default: handled = 0; break;
			}
		} else {
			switch (k) {
			case XKB_KEY_Up: move_cursor(f, f->cur < 0 ? 0 : f->cur - 1, shift); break;
			case XKB_KEY_Down: move_cursor(f, f->cur < 0 ? 0 : f->cur + 1, shift); break;
			case XKB_KEY_Page_Up: move_cursor(f, f->cur - page, shift); break;
			case XKB_KEY_Page_Down: move_cursor(f, f->cur + page, shift); break;
			case XKB_KEY_Home: move_cursor(f, 0, shift); break;
			case XKB_KEY_End: move_cursor(f, f->n - 1, shift); break;
			case XKB_KEY_Return:
			case XKB_KEY_KP_Enter:
				if (f->cur >= 0 && f->sel[f->cur]) {
					f->act_idx = f->cur;
					f->act = A_ENTER_ROW;
				} else if (f->kind != FC_OPEN || f->dirs_only) {
					f->act = A_ACCEPT;
				}
				break;
			case XKB_KEY_BackSpace: f->act = A_UP; break;
			case XKB_KEY_F5: f->act = A_REFRESH; break;
			default: handled = 0; break;
			}
			if (!handled && ui->text[0] && ui->text[0] != ' ' && f->kind != FC_SAVE) {
				typeahead(f, ui->text);
				handled = 1;
			}
			if (handled)
				ensure_visible(f, g.h);
		}
		if (handled) {
			ui->key = 0;
			ui->text[0] = 0;
		}
	}
	int max = content > g.h ? content - g.h : 0;
	if (f->scroll > max)
		f->scroll = max;
	if (f->scroll < 0)
		f->scroll = 0;

	fl_rect save = cv->clip;
	fl_clip(cv, g);
	int hit = -1;
	int first = f->scroll / ROW_H, last = (f->scroll + g.h) / ROW_H + 1;
	for (int i = first; i < last && i < f->n; i++) {
		fl_rect r = fl_r(g.x, g.y + i * ROW_H - f->scroll, g.w - (max ? 8 : 0), ROW_H);
		int cr = fl_widget(ui, ID_ROW + (uint32_t)i, r, 0);
		draw_row(ui, f, r, i, cr & FL_HOVER, size_x, date_x);
		if (cr & FL_HOVER) {
			hit = i;
			if (ui->press & FL_BTN_L) {
				click_select(f, i, mods);
				ui->focus = ID_LIST;
				if (ui->clicks >= 2 && !(mods & (FL_MOD_CTRL | FL_MOD_SHIFT))) {
					f->act_idx = i;
					f->act = A_ENTER_ROW;
				}
			}
		}
	}
	cv->clip = save;
	if (hit < 0 && (res & FL_HOVER) && (ui->press & FL_BTN_L) && !(mods & (FL_MOD_CTRL | FL_MOD_SHIFT))) {
		memset(f->sel, 0, (size_t)f->n);
		f->cur = -1;
	}
	if (max)
		fl_scrollbar(ui, ID_SCROLL, fl_r(g.x + g.w - 6, g.y + 2, 4, g.h - 4), &f->scroll, content, g.h);
	if (f->n == 0)
		fl_text_in(cv, ui->f_reg, g, FL_CENTER, 0, fl_th.dim,
		           f->nfilters && f->kind != FC_SAVEFILES ? "Nothing here matches the filter" : "This folder is empty");
}

static void draw_info(fl_ui *ui, struct fc *f, fl_rect r)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, r, fl_th.bg);
	fl_fill_rect(cv, fl_r(r.x, r.y, r.w, 1), fl_th.line);
	char s[PATH_MAX + 128];
	int c = count_sel(f);
	if (f->msg[0] && now_ms() - f->msg_ms < 6000) {
		fl_text_in(cv, f->small, fl_inset(r, 14, 0), FL_LEFT, 0, fl_th.err, f->msg);
		return;
	}
	if (f->kind == FC_SAVEFILES) {
		snprintf(s, sizeof s, "%d file%s will be saved in %s", f->nfiles, f->nfiles == 1 ? "" : "s", base_name(f->cwd));
	} else if (c == 1 && f->cur >= 0 && f->sel[f->cur]) {
		struct entry *e = &f->ents[f->cur];
		char sz[32] = "";
		if (!e->dir)
			fmt_size(e->size, sz, sizeof sz);
		snprintf(s, sizeof s, "%s%s%s", e->name, sz[0] ? "   " : "", sz);
	} else if (c > 1) {
		snprintf(s, sizeof s, "%d of %d items selected", c, f->n);
	} else if (f->dirs_only && f->kind == FC_OPEN) {
		snprintf(s, sizeof s, "Select a folder, or use this one: %s", f->cwd);
	} else {
		snprintf(s, sizeof s, "%d item%s%s", f->n, f->n == 1 ? "" : "s", f->show_hidden ? "  (hidden files shown)" : "");
	}
	char fit[PATH_MAX + 130];
	fl_text_fit(f->small, s, r.w - 28, fit, sizeof fit);
	fl_text_in(cv, f->small, fl_inset(r, 14, 0), FL_LEFT, 0, fl_th.dim, fit);
}

static void draw_name(fl_ui *ui, struct fc *f, fl_rect r)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, r, fl_th.panel);
	fl_fill_rect(cv, fl_r(r.x, r.y, r.w, 1), fl_th.line);
	fl_text_in(cv, ui->f_med, fl_r(r.x + 14, r.y, 56, r.h), FL_LEFT, 0, fl_th.dim, "Name");
	int res = fl_textfield(ui, ID_NAME, fl_r(r.x + 70, r.y + 9, r.w - 84, 36), f->name, sizeof f->name, "File name", ui->f_reg);
	if (res & FL_EDIT_ENTER)
		f->act = A_ACCEPT;
}

static void draw_bar(fl_ui *ui, struct fc *f, fl_rect bar)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, bar, fl_th.panel);
	fl_fill_rect(cv, fl_r(bar.x, bar.y, bar.w, 1), fl_th.line);
	int y = bar.y + (bar.h - 34) / 2;
	int x = bar.x + bar.w - 14;
	/* accept, cancel */
	int aw = 112;
	const char *accept = f->accept[0] ? f->accept : f->kind == FC_SAVE || f->kind == FC_SAVEFILES ? "Save" : f->dirs_only ? "Select" : "Open";
	x -= aw;
	if (fl_button(ui, ID_OK, fl_r(x, y, aw, 34), accept, FL_BTN_ACCENT))
		f->act = A_ACCEPT;
	x -= 8 + 96;
	if (fl_button(ui, ID_CANCEL, fl_r(x, y, 96, 34), "Cancel", FL_BTN_OUTLINE))
		portal_reply(f->d.req, 1, NULL);
	/* filter */
	int right_edge = x - 12;
	if (f->nfilters && f->kind != FC_SAVEFILES) {
		int fw = 0;
		for (int i = 0; i < f->nfilters; i++) {
			int w = fl_text_width(ui->f_reg, f->filters[i].name, -1) + 40;
			if (w > fw)
				fw = w;
		}
		if (fw > 260)
			fw = 260;
		if (fw < 120)
			fw = 120;
		fl_rect fr = fl_r(right_edge - fw, y, fw, 34);
		int res = fl_widget(ui, ID_FILTER, fr, FL_FOCUSABLE);
		fl_fill_rect(cv, fr, (res & (FL_HOVER | FL_DOWN)) ? fl_th.raised : fl_th.bg);
		fl_stroke_rect(cv, fr, 1, (res & FL_FOCUS) ? fl_th.accent : fl_th.line);
		char fit[200];
		fl_text_fit(ui->f_reg, f->filters[f->curfilter].name, fw - 40, fit, sizeof fit);
		fl_text_in(cv, ui->f_reg, fl_r(fr.x, fr.y, fr.w - 26, fr.h), FL_LEFT, 12, fl_th.text, fit);
		fl_icon_draw(cv, FL_ICON_FORWARD, fr.x + fr.w - 22, fr.y + 10, 14, fl_th.dim);
		if (res & FL_CLICK)
			popup_open(f, POP_FILTER, 0, fr, f->nfilters, f->curfilter);
		right_edge = fr.x - 12;
	}
	/* choices and the hidden-files switch on the left */
	int lx = bar.x + 14;
	for (int i = 0; i < f->nchoices; i++) {
		struct choice *c = &f->choices[i];
		int lw = fl_text_width(ui->f_reg, c->label, -1);
		if (c->n == 0) {
			if (lx + 40 + 8 + lw > right_edge)
				break;
			fl_rect sr = fl_r(lx, y + 5, 40, 24);
			fl_switch(ui, ID_CHOICE + (uint32_t)i, sr, &c->on);
			fl_text_in(cv, ui->f_reg, fl_r(sr.x + 48, y, lw + 4, 34), FL_LEFT, 0, fl_th.text, c->label);
			lx += 40 + 8 + lw + 24;
		} else {
			int bw = 0;
			for (int k = 0; k < c->n; k++) {
				int w = fl_text_width(ui->f_reg, c->olabel[k], -1) + 40;
				if (w > bw)
					bw = w;
			}
			if (bw < 100)
				bw = 100;
			if (lx + lw + 8 + bw > right_edge)
				break;
			fl_text_in(cv, ui->f_reg, fl_r(lx, y, lw + 4, 34), FL_LEFT, 0, fl_th.dim, c->label);
			fl_rect br = fl_r(lx + lw + 10, y, bw, 34);
			int res = fl_widget(ui, ID_CHOICE + (uint32_t)i, br, FL_FOCUSABLE);
			fl_fill_rect(cv, br, (res & (FL_HOVER | FL_DOWN)) ? fl_th.raised : fl_th.bg);
			fl_stroke_rect(cv, br, 1, (res & FL_FOCUS) ? fl_th.accent : fl_th.line);
			fl_text_in(cv, ui->f_reg, fl_r(br.x, br.y, br.w - 26, br.h), FL_LEFT, 12, fl_th.text, c->olabel[c->sel]);
			fl_icon_draw(cv, FL_ICON_FORWARD, br.x + br.w - 22, br.y + 10, 14, fl_th.dim);
			if (res & FL_CLICK)
				popup_open(f, POP_CHOICE, i, br, c->n, c->sel);
			lx += lw + 10 + bw + 20;
		}
	}
	{
		const char *lab = "Hidden files";
		int lw = fl_text_width(ui->f_reg, lab, -1);
		if (lx + 40 + 8 + lw <= right_edge) {
			fl_rect sr = fl_r(lx, y + 5, 40, 24);
			if (fl_switch(ui, ID_HIDDEN, sr, &f->show_hidden))
				f->act = A_REFRESH;
			fl_text_in(cv, ui->f_reg, fl_r(sr.x + 48, y, lw + 4, 34), FL_LEFT, 0, fl_th.dim, lab);
		}
	}
}

static void draw(fl_ui *ui, void *ud)
{
	struct fc *f = ud;
	fl_canvas *cv = &ui->cv;
	if (!f->d.req)
		return; /* answered; the window goes in a moment */
	fl_rect c = fl_win_content(ui->win);
	if (!f->inited) {
		ui->focus = f->kind == FC_SAVE ? ID_NAME : ID_LIST;
		f->inited = 1;
	}
	struct inmask im = {0};
	int modal = f->ov != OV_NONE;
	if (modal)
		ui_mask(ui, &im);

	fl_rect area = c;
	fl_rect tb = fl_cut_top(&area, TOOL_H);
	fl_rect bar = fl_cut_bottom(&area, BAR_H);
	fl_rect name = {0}, info = {0};
	if (f->kind == FC_SAVE)
		name = fl_cut_bottom(&area, NAME_H);
	else
		info = fl_cut_bottom(&area, INFO_H);
	fl_rect sb = fl_cut_left(&area, SIDE_W);
	fl_fill_rect(cv, c, fl_th.bg);
	draw_list(ui, f, area);
	draw_sidebar(ui, f, sb);
	draw_toolbar(ui, f, tb);
	if (f->kind == FC_SAVE)
		draw_name(ui, f, name);
	else
		draw_info(ui, f, info);
	draw_bar(ui, f, bar);
	if (f->kind == FC_SAVE && f->msg[0] && now_ms() - f->msg_ms < 6000) {
		fl_rect m = fl_r(name.x + 70, name.y - 20, name.w - 84, 20);
		fl_fill_rect(cv, m, fl_th.bg);
		fl_text_in(cv, f->small, m, FL_LEFT, 0, fl_th.err, f->msg);
	}

	/* Escape cancels, unless a text field or an overlay wants it */
	if (!modal && ui->key == XKB_KEY_Escape) {
		ui->key = 0;
		portal_reply(f->d.req, 1, NULL);
		return;
	}

	if (modal) {
		ui_unmask(ui, &im);
		if (f->ov == OV_POPUP)
			draw_popup(ui, f);
		else if (f->ov == OV_CONFIRM)
			draw_confirm(ui, f);
		else if (f->ov == OV_PROMPT)
			draw_prompt(ui, f);
		ui->key = 0;
		ui->text[0] = 0;
		ui->press = ui->release = 0;
	}
	if (f->act && f->d.req)
		run_action(f);
	if (f->d.req && (((f->ov != OV_NONE) != modal) || f->msg[0]))
		fl_win_redraw(ui->win);
}

/* ---- setup ------------------------------------------------------------------------------------ */

static void fc_destroy(struct dlg *d)
{
	struct fc *f = (struct fc *)d;
	free_entries(f);
	free(f->ents);
	free(f->sel);
	free(f->places);
	for (int i = 0; i < f->hn; i++)
		free(f->hist[i]);
	for (int i = 0; i < f->nfilters; i++) {
		g_free(f->filters[i].name);
		g_variant_unref(f->filters[i].pats);
	}
	g_free(f->filters);
	for (int i = 0; i < f->nchoices; i++) {
		struct choice *c = &f->choices[i];
		g_free(c->id);
		g_free(c->label);
		for (int k = 0; k < c->n; k++) {
			g_free(c->oid[k]);
			g_free(c->olabel[k]);
		}
		g_free(c->oid);
		g_free(c->olabel);
	}
	g_free(f->choices);
	for (int i = 0; i < f->nfiles; i++)
		g_free(f->files[i]);
	g_free(f->files);
	if (f->small)
		fl_font_close(f->small);
	if (f->head)
		fl_font_close(f->head);
	g_free(f);
}

/* FLPORTAL_SHOT: the same code path as the Open/Save button, for the headless test */
static void fc_test(struct dlg *d, int accept)
{
	struct fc *f = (struct fc *)d;
	if (!accept)
		return;
	if (do_accept(f, 1) != 0)
		fprintf(stderr, "flportal: test accept failed: %s\n", f->msg);
}

/* a bytestring option (a path, NUL terminated) */
static char *opt_path(GVariant *o, const char *key)
{
	GVariant *v = g_variant_lookup_value(o, key, G_VARIANT_TYPE_BYTESTRING);
	if (!v)
		return NULL;
	char *s = g_strdup(g_variant_get_bytestring(v));
	g_variant_unref(v);
	return s;
}

static void parse_filters(struct fc *f, GVariant *options)
{
	GVariant *fl = g_variant_lookup_value(options, "filters", G_VARIANT_TYPE("a(sa(us))"));
	if (fl) {
		gsize n = g_variant_n_children(fl);
		f->filters = g_new0(struct filter, n ? n : 1);
		for (gsize i = 0; i < n; i++) {
			const char *name;
			GVariant *pats;
			g_variant_get_child(fl, i, "(&s@a(us))", &name, &pats);
			f->filters[f->nfilters].name = g_strdup(name);
			f->filters[f->nfilters].pats = pats;
			f->nfilters++;
		}
		g_variant_unref(fl);
	}
	f->curfilter = f->nfilters ? 0 : -1;
	GVariant *cur = g_variant_lookup_value(options, "current_filter", G_VARIANT_TYPE("(sa(us))"));
	if (cur) {
		const char *name;
		GVariant *pats;
		g_variant_get(cur, "(&s@a(us))", &name, &pats);
		int found = -1;
		for (int i = 0; i < f->nfilters; i++)
			if (g_variant_equal(f->filters[i].pats, pats) && !strcmp(f->filters[i].name, name))
				found = i;
		if (found < 0) { /* the app's current filter is not in its list: add it */
			f->filters = g_renew(struct filter, f->filters, (gsize)f->nfilters + 1);
			f->filters[f->nfilters].name = g_strdup(name);
			f->filters[f->nfilters].pats = g_variant_ref(pats);
			found = f->nfilters++;
		}
		f->curfilter = found;
		g_variant_unref(pats);
		g_variant_unref(cur);
	}
}

static void parse_choices(struct fc *f, GVariant *options)
{
	GVariant *cl = g_variant_lookup_value(options, "choices", G_VARIANT_TYPE("a(ssa(ss)s)"));
	if (!cl)
		return;
	gsize n = g_variant_n_children(cl);
	f->choices = g_new0(struct choice, n ? n : 1);
	for (gsize i = 0; i < n; i++) {
		const char *id, *label, *initial;
		GVariant *opts;
		g_variant_get_child(cl, i, "(&s&s@a(ss)&s)", &id, &label, &opts, &initial);
		struct choice *c = &f->choices[f->nchoices++];
		c->id = g_strdup(id);
		c->label = g_strdup(label);
		c->n = (int)g_variant_n_children(opts);
		c->oid = g_new0(char *, c->n ? (gsize)c->n : 1);
		c->olabel = g_new0(char *, c->n ? (gsize)c->n : 1);
		for (int k = 0; k < c->n; k++) {
			const char *oid, *ol;
			g_variant_get_child(opts, (gsize)k, "(&s&s)", &oid, &ol);
			c->oid[k] = g_strdup(oid);
			c->olabel[k] = g_strdup(ol);
			if (!strcmp(oid, initial))
				c->sel = k;
		}
		if (c->n == 0)
			c->on = !strcmp(initial, "true");
		g_variant_unref(opts);
	}
	g_variant_unref(cl);
}

void fc_open(struct req *r, int kind, const char *title, GVariant *options)
{
	struct fc *f = g_new0(struct fc, 1);
	f->d.destroy = fc_destroy;
	f->d.test = fc_test;
	f->kind = kind;
	f->hpos = -1;
	f->cur = f->anchor = -1;
	gboolean b = FALSE;
	if (kind == FC_OPEN) {
		if (g_variant_lookup(options, "multiple", "b", &b))
			f->multiple = b;
		b = FALSE;
		if (g_variant_lookup(options, "directory", "b", &b))
			f->dirs_only = b;
	} else if (kind == FC_SAVEFILES) {
		f->dirs_only = 1;
		GVariant *fv = g_variant_lookup_value(options, "files", G_VARIANT_TYPE_BYTESTRING_ARRAY);
		if (fv) {
			gsize n;
			const char **names = g_variant_get_bytestring_array(fv, &n);
			f->files = g_new0(char *, n ? n : 1);
			for (gsize i = 0; i < n; i++)
				f->files[f->nfiles++] = g_path_get_basename(names[i]);
			g_free(names);
			g_variant_unref(fv);
		}
	}
	const char *al = NULL;
	if (g_variant_lookup(options, "accept_label", "&s", &al) && al)
		snprintf(f->accept, sizeof f->accept, "%s", al);
	/* GTK style labels carry a mnemonic underscore */
	for (char *s = f->accept, *d = s;; s++) {
		if (*s != '_')
			*d++ = *s;
		if (!*s)
			break;
	}
	parse_filters(f, options);
	parse_choices(f, options);

	/* where to start: current_file (save), current_folder, else the home folder */
	char *folder = opt_path(options, "current_folder");
	char *cfile = kind == FC_SAVE ? opt_path(options, "current_file") : NULL;
	const char *cname = NULL;
	if (kind == FC_SAVE || kind == FC_SAVEFILES)
		g_variant_lookup(options, "current_name", "&s", &cname);
	if (cfile && *cfile) {
		char *dir = g_path_get_dirname(cfile);
		g_free(folder);
		folder = dir;
		snprintf(f->name, sizeof f->name, "%s", base_name(cfile));
	} else if (cname && kind == FC_SAVE) {
		snprintf(f->name, sizeof f->name, "%s", cname);
	}
	g_free(cfile);
	const char *home = getenv("HOME");
	f->small = fl_font_ui(400, 12);
	f->head = fl_font_ui(600, 10.5f);
	places_load(f);
	int ok = -1;
	if (folder && *folder)
		ok = load_dir(f, folder, 1);
	if (ok < 0 && home && *home)
		ok = load_dir(f, home, 1);
	if (ok < 0)
		ok = load_dir(f, "/", 1);
	g_free(folder);
	f->msg[0] = 0;
	if (ok < 0) {
		portal_reply(r, 2, NULL);
		fc_destroy(&f->d);
		return;
	}
	/* headless test: FLPORTAL_TEST_SELECT=a,b picks entries (and shows them in the picture) */
	const char *ts = portal_test_mode() ? getenv("FLPORTAL_TEST_SELECT") : NULL;
	if (ts && *ts) {
		char *dup = g_strdup(ts);
		for (char *tok = strtok(dup, ","); tok; tok = strtok(NULL, ",")) {
			for (int i = 0; i < f->n; i++)
				if (!strcmp(f->ents[i].name, tok)) {
					f->sel[i] = 1;
					f->cur = i;
					if (f->kind == FC_SAVE && !f->ents[i].dir)
						snprintf(f->name, sizeof f->name, "%s", tok);
				}
		}
		g_free(dup);
	}
	if (portal_test_mode() && getenv("FLPORTAL_TEST_NAME"))
		snprintf(f->name, sizeof f->name, "%s", getenv("FLPORTAL_TEST_NAME"));
	/* ... and FLPORTAL_TEST_OVERLAY=confirm|popup|prompt shows an overlay in the picture */
	const char *tov = portal_test_mode() ? getenv("FLPORTAL_TEST_OVERLAY") : NULL;
	if (tov && !strcmp(tov, "confirm")) {
		join(f->confirm_path, sizeof f->confirm_path, f->cwd, f->name);
		f->ov = OV_CONFIRM;
	} else if (tov && !strcmp(tov, "popup") && f->nfilters) {
		popup_open(f, POP_FILTER, 0, fl_r(480, 520, 240, 34), f->nfilters, f->curfilter);
	} else if (tov && !strcmp(tov, "prompt")) {
		f->ov = OV_PROMPT;
		f->ov_new = 1;
		snprintf(f->prompt_text, sizeof f->prompt_text, "Invoices");
	}

	fl_win_opts o = {
		.title = title && *title ? title : kind == FC_SAVE ? "Save" : "Open", .app_id = "flportal",
		.w = 860, .h = 580, .min_w = 560, .min_h = 380, .decorated = 1, .resizable = 1, .draw = draw,
	};
	portal_dialog_open(r, &f->d, &o);
}
