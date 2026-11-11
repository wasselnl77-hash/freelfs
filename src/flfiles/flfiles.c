/* flfiles -- the FreeLFS file manager
 * SPDX-License-Identifier: MIT
 *
 * One window: back, forward, up and an editable path field on top, Places
 * and Drives on the left, an icon grid, a status bar. Hidden files (a dot, or
 * a name in the folder's .hidden) stay out of sight until Ctrl-H.
 *
 * Immediate mode: draw() reads the input and queues at most one action; the
 * action runs at the end of the pass, so the entry list is never changed
 * while it is being drawn.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <flui.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
	ID_BACK = 1, ID_FWD, ID_UP, ID_PATH, ID_NEW, ID_GRID, ID_SCROLL,
	ID_SIDE = 100, ID_CELL = 1000, ID_MENU = 0x40000, ID_DLG = 0x41000,
};

enum { TOOL_H = 44, STATUS_H = 28, SIDE_W = 188, CELL_W = 108, CELL_H = 98, ICON = 40 };

enum { A_NONE, A_OPEN, A_NAV, A_BACK, A_FWD, A_UP, A_NEWDIR, A_RENAME, A_TRASH, A_COPY, A_CUT,
       A_PASTE, A_HIDDEN, A_REFRESH, A_SELALL, A_DO_MKDIR, A_DO_RENAME, A_DO_DELETE, A_FOCUS_PATH, A_EXTRACT, A_COMPRESS };

enum { OV_NONE, OV_MENU, OV_PROMPT, OV_CONFIRM };

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

struct menu_item {
	const char *label, *hint;
	int act, enabled, sep;
};

struct F {
	fl_app *app;
	fl_win *win;
	fl_font *small, *head;
	fl_timer *msg_timer;
	int inited;

	char cwd[PATH_MAX], pathbuf[PATH_MAX];
	struct entry *ents;
	int n, cap;
	char *sel; /* n flags */
	int cur, anchor;
	int scroll;
	int show_hidden;
	char **hidden_list;
	int n_hidden;

	char *hist[64];
	int hn, hpos;

	struct place *places;
	int nplaces;
	char trash_files[PATH_MAX];

	int ino_fd, ino_wd;

	char msg[300];
	int msg_err;

	char type_buf[64];
	long long type_ms;

	/* queued action */
	int act, act_idx;
	char act_path[PATH_MAX];

	/* overlays */
	int ov;
	struct menu_item menu[10];
	int nmenu, menu_x, menu_y, menu_sel;
	int prompt_act;
	char prompt_title[96], prompt_text[NAME_MAX + 1], prompt_err[160];
	char confirm_text[200];
	int ov_new;
};

static const char *const text_ext[] = {
	"txt", "md", "c", "h", "cc", "cpp", "hpp", "py", "sh", "bash", "conf", "cfg", "ini", "json", "xml",
	"html", "htm", "css", "js", "log", "toml", "yaml", "yml", "csv", "rs", "go", "patch", "diff", "mk",
	"lua", "pl", "desktop", "service", "rc", "list", "lock", "recipe", NULL
};

/* ---- messages ------------------------------------------------------------------------- */

static void msg_clear(void *ud)
{
	struct F *f = ud;
	f->msg[0] = 0;
	fl_timer_arm(f->msg_timer, 0, 0);
	fl_win_redraw(f->win);
}

static void say(struct F *f, int err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(f->msg, sizeof f->msg, fmt, ap);
	va_end(ap);
	f->msg_err = err;
	fl_timer_arm(f->msg_timer, 6000, 0);
}

/* ---- small helpers ---------------------------------------------------------------------- */

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

static int is_text_name(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!dot || dot == name) {
		static const char *const plain[] = {"Makefile", "README", "LICENSE", "COPYING", "NEWS", NULL};
		for (int i = 0; plain[i]; i++)
			if (!strcmp(name, plain[i]))
				return 1;
		return 0;
	}
	for (int i = 0; text_ext[i]; i++)
		if (!strcasecmp(dot + 1, text_ext[i]))
			return 1;
	return 0;
}

/* looks like text: no NUL byte in the first 4 KiB */
static int sniff_text(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0)
		return 0;
	char b[4096];
	ssize_t n = read(fd, b, sizeof b);
	close(fd);
	if (n < 0)
		return 0;
	return memchr(b, 0, (size_t)n) == NULL;
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

static int path_exists(const char *p)
{
	struct stat st;
	return lstat(p, &st) == 0;
}

/* a name that is free in dir: name, then "stem copy.ext", "stem copy 2.ext" ... */
static void unique_name(const char *dir, const char *name, int is_dir, char *out, size_t cap)
{
	char p[PATH_MAX];
	join(p, sizeof p, dir, name);
	if (!path_exists(p)) {
		snprintf(out, cap, "%s", name);
		return;
	}
	const char *dot = is_dir ? NULL : strrchr(name, '.');
	if (dot == name)
		dot = NULL;
	int stem = dot ? (int)(dot - name) : (int)strlen(name);
	for (int i = 1; i < 10000; i++) {
		if (i == 1)
			snprintf(out, cap, "%.*s copy%s", stem, name, dot ? dot : "");
		else
			snprintf(out, cap, "%.*s copy %d%s", stem, name, i, dot ? dot : "");
		join(p, sizeof p, dir, out);
		if (!path_exists(p))
			return;
	}
	snprintf(out, cap, "%s", name);
}

static int valid_name(const char *s, char *why, size_t cap)
{
	if (!*s) {
		snprintf(why, cap, "Type a name.");
		return 0;
	}
	if (strchr(s, '/')) {
		snprintf(why, cap, "A name cannot contain a slash.");
		return 0;
	}
	if (!strcmp(s, ".") || !strcmp(s, "..")) {
		snprintf(why, cap, "That name is not allowed.");
		return 0;
	}
	return 1;
}

/* ---- file operations ----------------------------------------------------------------------- */

static int remove_tree(const char *path)
{
	struct stat st;
	if (lstat(path, &st) < 0)
		return -1;
	if (S_ISDIR(st.st_mode)) {
		DIR *d = opendir(path);
		if (!d)
			return -1;
		struct dirent *e;
		int rc = 0;
		while ((e = readdir(d))) {
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			char sub[PATH_MAX];
			join(sub, sizeof sub, path, e->d_name);
			if (remove_tree(sub) < 0)
				rc = -1;
		}
		closedir(d);
		if (rc < 0)
			return -1;
		return rmdir(path);
	}
	return unlink(path);
}

static int copy_tree(const char *src, const char *dst, int depth)
{
	struct stat st;
	if (lstat(src, &st) < 0)
		return -1;
	if (depth > 64) {
		errno = ELOOP;
		return -1;
	}
	if (S_ISDIR(st.st_mode)) {
		if (mkdir(dst, (st.st_mode & 07777) | 0700) < 0)
			return -1;
		DIR *d = opendir(src);
		if (!d)
			return -1;
		struct dirent *e;
		int rc = 0;
		while ((e = readdir(d))) {
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			char s2[PATH_MAX], d2[PATH_MAX];
			join(s2, sizeof s2, src, e->d_name);
			join(d2, sizeof d2, dst, e->d_name);
			if (copy_tree(s2, d2, depth + 1) < 0)
				rc = -1;
		}
		closedir(d);
		chmod(dst, st.st_mode & 07777);
		return rc;
	}
	if (S_ISLNK(st.st_mode)) {
		char t[PATH_MAX];
		ssize_t n = readlink(src, t, sizeof t - 1);
		if (n < 0)
			return -1;
		t[n] = 0;
		return symlink(t, dst);
	}
	if (!S_ISREG(st.st_mode)) {
		errno = ENOTSUP;
		return -1;
	}
	int in = open(src, O_RDONLY | O_CLOEXEC);
	if (in < 0)
		return -1;
	int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st.st_mode & 07777);
	if (out < 0) {
		int e = errno;
		close(in);
		errno = e;
		return -1;
	}
	static char buf[65536];
	int rc = 0;
	for (;;) {
		ssize_t n = read(in, buf, sizeof buf);
		if (n < 0) {
			rc = -1;
			break;
		}
		if (n == 0)
			break;
		for (ssize_t off = 0; off < n;) {
			ssize_t w = write(out, buf + off, (size_t)(n - off));
			if (w < 0) {
				rc = -1;
				goto done;
			}
			off += w;
		}
	}
done:;
	int e = errno;
	close(in);
	if (close(out) < 0 && rc == 0)
		rc = -1, e = errno;
	if (rc < 0) {
		unlink(dst);
		errno = e;
	}
	return rc;
}

/* rename, or copy and remove across file systems */
static int move_path(const char *src, const char *dst)
{
	if (rename(src, dst) == 0)
		return 0;
	if (errno != EXDEV)
		return -1;
	if (copy_tree(src, dst, 0) < 0) {
		int e = errno;
		remove_tree(dst);
		errno = e;
		return -1;
	}
	return remove_tree(src);
}

static int mkdir_p(const char *path)
{
	char t[PATH_MAX];
	snprintf(t, sizeof t, "%s", path);
	for (char *p = t + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = 0;
		if (mkdir(t, 0700) < 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(t, 0700) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

static void pct_encode(const char *s, char *out, size_t cap)
{
	size_t n = 0;
	for (; *s && n + 4 < cap; s++) {
		unsigned char c = (unsigned char)*s;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		    c == '/' || c == '-' || c == '_' || c == '.' || c == '~')
			out[n++] = (char)c;
		else
			n += (size_t)snprintf(out + n, cap - n, "%%%02X", c);
	}
	out[n] = 0;
}

/* FreeDesktop trash: files in Trash/files, a .trashinfo in Trash/info */
static int trash_path(struct F *f, const char *path)
{
	const char *home = getenv("HOME");
	if (!home || !*home) {
		errno = ENOENT;
		return -1;
	}
	const char *xdg = getenv("XDG_DATA_HOME");
	char base[PATH_MAX], files[PATH_MAX], info[PATH_MAX];
	if (xdg && *xdg)
		snprintf(base, sizeof base, "%s/Trash", xdg);
	else
		snprintf(base, sizeof base, "%s/.local/share/Trash", home);
	snprintf(files, sizeof files, "%s/files", base);
	snprintf(info, sizeof info, "%s/info", base);
	if (mkdir_p(files) < 0 || mkdir_p(info) < 0)
		return -1;
	struct stat st;
	if (lstat(path, &st) < 0)
		return -1;
	char name[NAME_MAX + 1], dst[PATH_MAX], ipath[PATH_MAX];
	snprintf(name, sizeof name, "%s", base_name(path));
	/* the .trashinfo must be free too */
	for (int i = 1;; i++) {
		if (i == 1)
			snprintf(name, sizeof name, "%s", base_name(path));
		else
			snprintf(name, sizeof name, "%.200s.%d", base_name(path), i);
		join(dst, sizeof dst, files, name);
		snprintf(ipath, sizeof ipath, "%s/%s.trashinfo", info, name);
		if (!path_exists(dst) && !path_exists(ipath))
			break;
		if (i > 9999) {
			errno = EEXIST;
			return -1;
		}
	}
	char enc[PATH_MAX * 3], date[32];
	pct_encode(path, enc, sizeof enc);
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &tm);
	FILE *fp = fopen(ipath, "w");
	if (!fp)
		return -1;
	fprintf(fp, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", enc, date);
	if (fclose(fp) != 0) {
		unlink(ipath);
		return -1;
	}
	if (move_path(path, dst) < 0) {
		int e = errno;
		unlink(ipath);
		errno = e;
		return -1;
	}
	(void)f;
	return 0;
}

/* ---- directory loading ----------------------------------------------------------------------- */

static int cmp_entry(const void *a, const void *b)
{
	const struct entry *x = a, *y = b;
	if (x->dir != y->dir)
		return y->dir - x->dir;
	int c = strcasecmp(x->name, y->name);
	return c ? c : strcmp(x->name, y->name);
}

static void free_entries(struct F *f)
{
	for (int i = 0; i < f->n; i++)
		free(f->ents[i].name);
	f->n = 0;
}

static void read_hidden_list(struct F *f)
{
	for (int i = 0; i < f->n_hidden; i++)
		free(f->hidden_list[i]);
	f->n_hidden = 0;
	char p[PATH_MAX];
	join(p, sizeof p, f->cwd, ".hidden");
	FILE *fp = fopen(p, "r");
	if (!fp)
		return;
	char line[NAME_MAX + 8];
	while (fgets(line, sizeof line, fp) && f->n_hidden < 4096) {
		line[strcspn(line, "\r\n")] = 0;
		if (!line[0])
			continue;
		char **nl = realloc(f->hidden_list, (size_t)(f->n_hidden + 1) * sizeof *nl);
		if (!nl)
			break;
		f->hidden_list = nl;
		f->hidden_list[f->n_hidden++] = strdup(line);
	}
	fclose(fp);
}

static int is_hidden(struct F *f, const char *name)
{
	if (name[0] == '.')
		return 1;
	for (int i = 0; i < f->n_hidden; i++)
		if (!strcmp(f->hidden_list[i], name))
			return 1;
	return 0;
}

static int scan(struct F *f)
{
	DIR *d = opendir(f->cwd);
	if (!d)
		return -1;
	free_entries(f);
	read_hidden_list(f);
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (!f->show_hidden && is_hidden(f, e->d_name))
			continue;
		if (f->n == f->cap) {
			int nc = f->cap ? f->cap * 2 : 128;
			struct entry *ne = realloc(f->ents, (size_t)nc * sizeof *ne);
			if (!ne)
				break;
			f->ents = ne;
			f->cap = nc;
		}
		struct entry *en = &f->ents[f->n];
		memset(en, 0, sizeof *en);
		en->name = strdup(e->d_name);
		if (!en->name)
			break;
		char p[PATH_MAX];
		join(p, sizeof p, f->cwd, e->d_name);
		struct stat st;
		if (lstat(p, &st) == 0) {
			en->size = st.st_size;
			en->mtime = st.st_mtime;
			if (S_ISLNK(st.st_mode)) {
				en->link = 1;
				struct stat ts;
				if (stat(p, &ts) == 0)
					en->dir = S_ISDIR(ts.st_mode);
			} else {
				en->dir = S_ISDIR(st.st_mode);
			}
		}
		f->n++;
	}
	closedir(d);
	qsort(f->ents, (size_t)f->n, sizeof *f->ents, cmp_entry);
	return 0;
}

static void watch_cwd(struct F *f)
{
	if (f->ino_fd < 0)
		return;
	if (f->ino_wd >= 0)
		inotify_rm_watch(f->ino_fd, f->ino_wd);
	f->ino_wd = inotify_add_watch(f->ino_fd, f->cwd,
	                              IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB |
	                                  IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF);
}

static void sel_reset(struct F *f)
{
	free(f->sel);
	f->sel = calloc((size_t)(f->n ? f->n : 1), 1);
	f->cur = f->anchor = f->n ? 0 : -1;
}

static void set_title(struct F *f)
{
	fl_win_set_title(f->win, !strcmp(f->cwd, "/") ? "/" : base_name(f->cwd));
}

static void places_load(struct F *f);

/* Go to path. 0 on success; on failure the old folder stays and a message says why. */
static int load_dir(struct F *f, const char *path, int push)
{
	char rp[PATH_MAX];
	if (!realpath(path, rp)) {
		say(f, 1, "Cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	struct stat st;
	if (stat(rp, &st) < 0 || !S_ISDIR(st.st_mode)) {
		say(f, 1, "%s is not a folder", rp);
		return -1;
	}
	char old[PATH_MAX];
	snprintf(old, sizeof old, "%s", f->cwd);
	snprintf(f->cwd, sizeof f->cwd, "%s", rp);
	if (scan(f) < 0) {
		int e = errno;
		snprintf(f->cwd, sizeof f->cwd, "%s", old);
		if (old[0])
			scan(f);
		say(f, 1, "Cannot open %s: %s", rp, strerror(e));
		return -1;
	}
	sel_reset(f);
	f->scroll = 0;
	f->msg[0] = 0;
	snprintf(f->pathbuf, sizeof f->pathbuf, "%s", f->cwd);
	if (push) {
		for (int i = f->hpos + 1; i < f->hn; i++)
			free(f->hist[i]);
		f->hn = f->hpos + 1;
		if (f->hn == 64) {
			free(f->hist[0]);
			memmove(f->hist, f->hist + 1, 63 * sizeof *f->hist);
			f->hn--;
		}
		f->hist[f->hn++] = strdup(f->cwd);
		f->hpos = f->hn - 1;
	}
	watch_cwd(f);
	set_title(f);
	places_load(f);
	return 0;
}

/* re-read the folder, keeping what was selected */
static void refresh(struct F *f)
{
	char **names = NULL;
	int nn = 0;
	for (int i = 0; i < f->n; i++)
		if (f->sel && f->sel[i]) {
			char **t = realloc(names, (size_t)(nn + 1) * sizeof *t);
			if (!t)
				break;
			names = t;
			names[nn++] = strdup(f->ents[i].name);
		}
	char curname[NAME_MAX + 1] = "";
	if (f->cur >= 0 && f->cur < f->n)
		snprintf(curname, sizeof curname, "%s", f->ents[f->cur].name);
	if (scan(f) < 0) {
		/* the folder is gone: go up until one exists */
		char up[PATH_MAX];
		snprintf(up, sizeof up, "%s", f->cwd);
		for (;;) {
			char *s = strrchr(up, '/');
			if (!s)
				break;
			if (s == up)
				up[1] = 0;
			else
				*s = 0;
			if (load_dir(f, up, 1) == 0)
				break;
			if (!strcmp(up, "/"))
				break;
		}
		for (int i = 0; i < nn; i++)
			free(names[i]);
		free(names);
		return;
	}
	sel_reset(f);
	for (int i = 0; i < f->n; i++) {
		for (int j = 0; j < nn; j++)
			if (!strcmp(f->ents[i].name, names[j]))
				f->sel[i] = 1;
		if (!strcmp(f->ents[i].name, curname))
			f->cur = f->anchor = i;
	}
	for (int i = 0; i < nn; i++)
		free(names[i]);
	free(names);
	if (f->cur >= f->n)
		f->cur = f->n - 1;
}

static void select_name(struct F *f, const char *name)
{
	for (int i = 0; i < f->n; i++)
		if (!strcmp(f->ents[i].name, name)) {
			memset(f->sel, 0, (size_t)f->n);
			f->sel[i] = 1;
			f->cur = f->anchor = i;
			return;
		}
}

static void inotify_ready(void *ud, int fd, short rev)
{
	struct F *f = ud;
	char buf[4096];
	int any = 0;
	for (;;) {
		ssize_t n = read(fd, buf, sizeof buf);
		if (n <= 0)
			break;
		any = 1;
	}
	if (any) {
		refresh(f);
		fl_win_redraw(f->win);
	}
}

/* ---- places and drives --------------------------------------------------------------------------- */

static void place_add(struct F *f, const char *label, const char *path, fl_icon ic, int drive)
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

static int is_dir_path(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
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

static void places_load(struct F *f)
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
	if (f->trash_files[0])
		place_add(f, "Trash", f->trash_files, FL_ICON_TRASH, 0);

	/* drives: the system, then what is mounted under /drives */
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
			/* mount points escape spaces as \040 */
			char dec[PATH_MAX];
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

/* ---- actions ----------------------------------------------------------------------------------------- */

static void queue(struct F *f, int act)
{
	f->act = act;
}

static int count_sel(const struct F *f)
{
	int c = 0;
	for (int i = 0; i < f->n; i++)
		c += f->sel && f->sel[i];
	return c;
}

static int in_trash(const struct F *f)
{
	return f->trash_files[0] && !strncmp(f->cwd, f->trash_files, strlen(f->trash_files));
}

/* ---- archives: handed to flarchive (Archives) ----------------------------------------------------------- */

static int is_archive_name(const char *name)
{
	static const char *const sfx[] = {".zip", ".tar", ".tar.gz", ".tgz", ".tar.bz2", ".tbz2", ".tar.xz", ".txz", ".tar.zst",
	                                  ".tzst", ".7z", ".rar", ".gz", ".bz2", ".xz", ".zst", ".cab"};
	size_t n = strlen(name);
	for (size_t i = 0; i < sizeof sfx / sizeof *sfx; i++) {
		size_t l = strlen(sfx[i]);
		if (n > l && !strcasecmp(name + n - l, sfx[i]))
			return 1;
	}
	return 0;
}

/* flarchive MODE PATH...: mode "" opens an archive, --extract-here, --compress */
static void run_flarchive(struct F *f, const char *mode, char **paths, int n)
{
	char **argv = calloc((size_t)n + 3, sizeof *argv);
	if (!argv)
		return;
	int k = 0;
	argv[k++] = "flarchive";
	if (mode[0])
		argv[k++] = (char *)mode;
	for (int i = 0; i < n; i++)
		argv[k++] = paths[i];
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		signal(SIGCHLD, SIG_DFL);
		execvp("flarchive", argv);
		_exit(127);
	}
	free(argv);
	if (pid < 0)
		say(f, 1, "Cannot start Archives: %s", strerror(errno));
}

/* "Extract here" (compress == 0: the selected archives) and "Compress" (everything selected) */
static void archive_action(struct F *f, int compress)
{
	char **paths = calloc((size_t)f->n + 1, sizeof *paths);
	int n = 0;
	if (!paths)
		return;
	for (int i = 0; i < f->n; i++) {
		if (!f->sel || !f->sel[i] || (!compress && (f->ents[i].dir || !is_archive_name(f->ents[i].name))))
			continue;
		char p[PATH_MAX];
		join(p, sizeof p, f->cwd, f->ents[i].name);
		paths[n] = strdup(p);
		if (paths[n])
			n++;
	}
	if (n)
		run_flarchive(f, compress ? "--compress" : "--extract-here", paths, n);
	for (int i = 0; i < n; i++)
		free(paths[i]);
	free(paths);
}

/* ---- Open: the default app for the file's MIME type -------------------------------------------------------
 * The type comes from the shared-mime-info glob table (/usr/share/mime/globs2, longest and heaviest
 * pattern wins), text files with no known name are text/plain. The app is the first usable entry of the
 * type in mimeapps.list ([Default Applications], then [Added Associations]) from ~/.config, /etc/xdg,
 * ~/.local/share/applications and the data dirs' applications folders (we ship defaults in
 * /usr/share/applications/mimeapps.list), else the first .desktop file that lists the type. */

static void data_dirs(char dirs[8][PATH_MAX], int *n)
{
	const char *h = getenv("XDG_DATA_HOME"), *home = getenv("HOME"), *xd = getenv("XDG_DATA_DIRS");
	*n = 0;
	if (h && *h)
		snprintf(dirs[(*n)++], PATH_MAX, "%s", h);
	else if (home && *home)
		snprintf(dirs[(*n)++], PATH_MAX, "%s/.local/share", home);
	if (!xd || !*xd)
		xd = "/usr/local/share:/usr/share";
	char *c = strdup(xd);
	for (char *t = c ? strtok(c, ":") : NULL; t && *n < 8; t = strtok(NULL, ":"))
		snprintf(dirs[(*n)++], PATH_MAX, "%s", t);
	free(c);
}

static int mime_of_name(const char *name, char *out, size_t cap)
{
	FILE *g = fopen("/usr/share/mime/globs2", "r");
	if (!g)
		return -1;
	char line[512];
	int best_w = -1;
	size_t best_l = 0, nl = strlen(name);
	while (fgets(line, sizeof line, g)) {
		if (line[0] == '#')
			continue;
		char *w = line, *m = strchr(w, ':');
		if (!m)
			continue;
		*m++ = 0;
		char *p = strchr(m, ':');
		if (!p)
			continue;
		*p++ = 0;
		p[strcspn(p, ":\r\n")] = 0;
		int weight = atoi(w);
		size_t pl = strlen(p);
		int hit = 0;
		if (p[0] == '*' && p[1] == '.' && !strpbrk(p + 1, "*?[")) {
			size_t l = pl - 1; /* ".png" */
			hit = nl > l && !strcasecmp(name + nl - l, p + 1);
		} else if (!strpbrk(p, "*?[")) {
			hit = !strcmp(name, p);
		}
		if (hit && (weight > best_w || (weight == best_w && pl > best_l))) {
			best_w = weight;
			best_l = pl;
			snprintf(out, cap, "%s", m);
		}
	}
	fclose(g);
	return best_w >= 0 ? 0 : -1;
}

/* the .desktop file for an id such as flview.desktop, or "" */
static int find_desktop(const char *id, char *out, size_t cap)
{
	char d[8][PATH_MAX];
	int n;
	data_dirs(d, &n);
	for (int i = 0; i < n; i++) {
		snprintf(out, cap, "%s/applications/%s", d[i], id);
		if (access(out, R_OK) == 0)
			return 0;
	}
	return -1;
}

static int mimeapps_lookup(const char *file, const char *mime, char *desktop, size_t cap)
{
	FILE *f = fopen(file, "r");
	if (!f)
		return -1;
	char line[1024], section[64] = "";
	int found = -1;
	size_t ml = strlen(mime);
	while (found < 0 && fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '[') {
			snprintf(section, sizeof section, "%s", line);
			continue;
		}
		if (strcmp(section, "[Default Applications]") && strcmp(section, "[Added Associations]"))
			continue;
		if (strncmp(line, mime, ml) || line[ml] != '=')
			continue;
		for (char *t = strtok(line + ml + 1, ";"); t; t = strtok(NULL, ";"))
			if (*t && find_desktop(t, desktop, cap) == 0) {
				found = 0;
				break;
			}
	}
	fclose(f);
	return found;
}

static int app_for_mime(const char *mime, char *desktop, size_t cap)
{
	char d[8][PATH_MAX], p[PATH_MAX];
	int n;
	data_dirs(d, &n);
	const char *home = getenv("HOME"), *cfg = getenv("XDG_CONFIG_HOME");
	if (cfg && *cfg)
		snprintf(p, sizeof p, "%s/mimeapps.list", cfg);
	else
		snprintf(p, sizeof p, "%s/.config/mimeapps.list", home ? home : "");
	if (mimeapps_lookup(p, mime, desktop, cap) == 0 || mimeapps_lookup("/etc/xdg/mimeapps.list", mime, desktop, cap) == 0)
		return 0;
	for (int i = 0; i < n; i++) {
		snprintf(p, sizeof p, "%s/applications/mimeapps.list", d[i]);
		if (mimeapps_lookup(p, mime, desktop, cap) == 0)
			return 0;
	}
	/* no list names an app: the first desktop file that handles the type */
	size_t ml = strlen(mime);
	for (int i = 0; i < n; i++) {
		snprintf(p, sizeof p, "%s/applications", d[i]);
		DIR *dir = opendir(p);
		struct dirent *e;
		char best[PATH_MAX] = "";
		while (dir && (e = readdir(dir))) {
			size_t l = strlen(e->d_name);
			if (l < 9 || strcmp(e->d_name + l - 8, ".desktop") || !strcmp(e->d_name, "flfiles.desktop"))
				continue;
			char path[PATH_MAX + 300];
			snprintf(path, sizeof path, "%s/%s", p, e->d_name);
			FILE *f = fopen(path, "r");
			char line[2048];
			int hit = 0;
			while (f && fgets(line, sizeof line, f))
				if (!strncmp(line, "MimeType=", 9)) {
					for (char *s = line + 9; (s = strstr(s, mime)); s += ml)
						if ((s == line + 9 || s[-1] == ';') && (s[ml] == ';' || s[ml] == '\n' || s[ml] == 0))
							hit = 1;
					break;
				}
			if (f)
				fclose(f);
			if (hit && (!best[0] || strcmp(path, best) < 0))
				snprintf(best, sizeof best, "%s", path);
		}
		if (dir)
			closedir(dir);
		if (best[0]) {
			snprintf(desktop, cap, "%s", best);
			return 0;
		}
	}
	return -1;
}

/* Exec= of the desktop file, split into words, %f %F %u %U replaced by the file */
static int exec_args(const char *desktop, const char *file, char **argv, int max)
{
	FILE *f = fopen(desktop, "r");
	if (!f)
		return -1;
	char line[2048], ex[2048] = "";
	int in = 0;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '[')
			in = !strcmp(line, "[Desktop Entry]");
		else if (in && !strncmp(line, "Exec=", 5)) {
			snprintf(ex, sizeof ex, "%s", line + 5);
			break;
		}
	}
	fclose(f);
	if (!ex[0])
		return -1;
	int n = 0, used = 0;
	for (char *s = ex; *s && n < max - 2;) {
		while (*s == ' ' || *s == '\t')
			s++;
		if (!*s)
			break;
		char w[PATH_MAX + 256];
		size_t k = 0;
		int q = 0, any = 0;
		for (; *s && (q || (*s != ' ' && *s != '\t')) && k < sizeof w - PATH_MAX - 2; s++) {
			if (*s == '"') {
				q = !q;
				any = 1;
			} else if (*s == '\\' && q && s[1]) {
				w[k++] = *++s;
			} else if (*s == '%' && s[1]) {
				s++;
				if (*s == 'f' || *s == 'F' || *s == 'u' || *s == 'U') {
					k += (size_t)snprintf(w + k, sizeof w - k, "%s", file);
					used = any = 1;
				} else if (*s == '%') {
					w[k++] = '%';
				} /* %i %c %k and the rest: nothing */
			} else {
				w[k++] = *s;
			}
		}
		w[k] = 0;
		if (k || (any && w[0]))
			argv[n++] = strdup(w);
	}
	if (!used && n)
		argv[n++] = strdup(file);
	argv[n] = NULL;
	return n > 0 ? 0 : -1;
}

/* 1: started an app for the file */
static int open_by_mime(struct F *f, const char *path, const char *name)
{
	char mime[128] = "", dt[PATH_MAX];
	if (mime_of_name(name, mime, sizeof mime) < 0)
		snprintf(mime, sizeof mime, "%s", is_text_name(name) || sniff_text(path) ? "text/plain" : "");
	if (!mime[0])
		return 0;
	if (app_for_mime(mime, dt, sizeof dt) < 0) {
		if (strncmp(mime, "text/", 5) || app_for_mime("text/plain", dt, sizeof dt) < 0)
			return 0;
	}
	char *argv[32] = {0};
	if (exec_args(dt, path, argv, 32) < 0)
		return 0;
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		signal(SIGCHLD, SIG_DFL);
		execvp(argv[0], argv);
		_exit(127);
	}
	for (int i = 0; argv[i]; i++)
		free(argv[i]);
	if (pid < 0)
		say(f, 1, "Cannot start the app for %s: %s", name, strerror(errno));
	return 1;
}

static void open_entry(struct F *f, int i)
{
	if (i < 0 || i >= f->n)
		return;
	char p[PATH_MAX];
	join(p, sizeof p, f->cwd, f->ents[i].name);
	if (f->ents[i].dir) {
		load_dir(f, p, 1);
		return;
	}
	if (is_archive_name(f->ents[i].name)) {
		char *one = p;
		run_flarchive(f, "", &one, 1);
		return;
	}
	if (open_by_mime(f, p, f->ents[i].name))
		return;
	if (!is_text_name(f->ents[i].name) && !sniff_text(p)) {
		say(f, 0, "No app to open %s", f->ents[i].name);
		return;
	}
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		signal(SIGCHLD, SIG_DFL);
		execlp("fledit", "fledit", p, (char *)NULL);
		_exit(127);
	}
	if (pid < 0)
		say(f, 1, "Cannot start the editor: %s", strerror(errno));
}

static void go_history(struct F *f, int d)
{
	int np = f->hpos + d;
	if (np < 0 || np >= f->hn)
		return;
	char p[PATH_MAX];
	snprintf(p, sizeof p, "%s", f->hist[np]);
	if (load_dir(f, p, 0) == 0)
		f->hpos = np;
}

static void go_up(struct F *f)
{
	if (!strcmp(f->cwd, "/"))
		return;
	char up[PATH_MAX], child[NAME_MAX + 1];
	snprintf(up, sizeof up, "%s", f->cwd);
	snprintf(child, sizeof child, "%s", base_name(f->cwd));
	char *s = strrchr(up, '/');
	if (s == up)
		up[1] = 0;
	else
		*s = 0;
	if (load_dir(f, up, 1) == 0)
		select_name(f, child);
}

static void do_copy(struct F *f, int cut)
{
	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	if (!buf)
		return;
	len = (size_t)snprintf(buf, cap, "%s\n", cut ? "x-flfiles/cut" : "x-flfiles/copy");
	int c = 0;
	for (int i = 0; i < f->n; i++) {
		if (!f->sel[i])
			continue;
		char p[PATH_MAX];
		join(p, sizeof p, f->cwd, f->ents[i].name);
		size_t pl = strlen(p) + 1;
		if (len + pl + 1 > cap) {
			cap = (len + pl + 1) * 2;
			char *nb = realloc(buf, cap);
			if (!nb) {
				free(buf);
				return;
			}
			buf = nb;
		}
		len += (size_t)snprintf(buf + len, cap - len, "%s\n", p);
		c++;
	}
	if (c) {
		fl_clipboard_set(f->app, buf, len);
		say(f, 0, "%s %d item%s", cut ? "Cut" : "Copied", c, c == 1 ? "" : "s");
	}
	free(buf);
}

static void do_paste(struct F *f)
{
	size_t n = 0;
	char *clip = fl_clipboard_get(f->app, &n);
	if (!clip) {
		say(f, 0, "Nothing to paste");
		return;
	}
	char *text = malloc(n + 1);
	if (!text) {
		free(clip);
		return;
	}
	memcpy(text, clip, n);
	text[n] = 0;
	free(clip);
	int cut = 0;
	char *line = text, *save = NULL;
	line = strtok_r(text, "\n", &save);
	if (!line || (strcmp(line, "x-flfiles/copy") && strcmp(line, "x-flfiles/cut"))) {
		say(f, 0, "Nothing to paste");
		free(text);
		return;
	}
	cut = !strcmp(line, "x-flfiles/cut");
	int done = 0, failed = 0;
	char last[NAME_MAX + 1] = "";
	while ((line = strtok_r(NULL, "\n", &save))) {
		if (line[0] != '/')
			continue;
		struct stat st;
		if (lstat(line, &st) < 0) {
			failed++;
			continue;
		}
		char dname[PATH_MAX];
		snprintf(dname, sizeof dname, "%s", line);
		char *slash = strrchr(dname, '/');
		const char *name = base_name(line);
		if (slash && slash != dname)
			*slash = 0;
		else
			snprintf(dname, sizeof dname, "/");
		int same_dir = !strcmp(dname, f->cwd);
		if (cut && same_dir) {
			done++;
			snprintf(last, sizeof last, "%s", name);
			continue;
		}
		size_t sl = strlen(line);
		if (S_ISDIR(st.st_mode) && !strncmp(f->cwd, line, sl) && (f->cwd[sl] == '/' || f->cwd[sl] == 0)) {
			say(f, 1, "Cannot put a folder inside itself");
			failed++;
			continue;
		}
		char nn[NAME_MAX + 1], dst[PATH_MAX];
		unique_name(f->cwd, name, S_ISDIR(st.st_mode), nn, sizeof nn);
		join(dst, sizeof dst, f->cwd, nn);
		int rc = cut ? move_path(line, dst) : copy_tree(line, dst, 0);
		if (rc < 0) {
			say(f, 1, "Could not %s %s: %s", cut ? "move" : "copy", name, strerror(errno));
			failed++;
		} else {
			done++;
			snprintf(last, sizeof last, "%s", nn);
		}
	}
	free(text);
	if (cut && done && !failed)
		fl_clipboard_set(f->app, "", 0);
	refresh(f);
	if (last[0] && done == 1)
		select_name(f, last);
	if (!failed)
		say(f, 0, "%s %d item%s", cut ? "Moved" : "Pasted", done, done == 1 ? "" : "s");
}

/* The easter egg: `flcrash --cant PATH...` shows "You can't do that." when one of the paths is a
 * vital system folder (/sysflfs, /boot, ...) and exits 1 on "Never mind". Anything else (not one of
 * those, "Continue", flcrash not installed, no way to ask) lets the operation go on. */
static int egg_allows(char **paths, int n)
{
	for (int from = 0; from < n; from += 500) {
		int cnt = n - from < 500 ? n - from : 500;
		char **argv = calloc((size_t)cnt + 3, sizeof *argv);
		if (!argv)
			return 1;
		argv[0] = "flcrash";
		argv[1] = "--cant";
		for (int i = 0; i < cnt; i++)
			argv[2 + i] = paths[from + i];
		/* main() ignores SIGCHLD (launched programs are not waited for): here the exit status matters */
		void (*old)(int) = signal(SIGCHLD, SIG_DFL);
		pid_t p = fork();
		if (p == 0) {
			execvp("flcrash", argv);
			_exit(0);
		}
		int st = 0, rc = p < 0 ? -1 : 0;
		while (p > 0 && waitpid(p, &st, 0) < 0 && errno == EINTR)
			;
		signal(SIGCHLD, old);
		free(argv);
		if (rc == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 1)
			return 0;
	}
	return 1;
}

/* the selected entries as full paths; free with free_paths */
static char **selected_paths(struct F *f, int *n)
{
	char **v = calloc((size_t)f->n + 1, sizeof *v);
	*n = 0;
	for (int i = 0; v && i < f->n; i++)
		if (f->sel[i]) {
			char p[PATH_MAX];
			join(p, sizeof p, f->cwd, f->ents[i].name);
			v[(*n)++] = strdup(p);
		}
	return v;
}

static void free_paths(char **v, int n)
{
	for (int i = 0; v && i < n; i++)
		free(v[i]);
	free(v);
}

static int egg_ok_selection(struct F *f)
{
	int n;
	char **v = selected_paths(f, &n);
	int ok = !v || egg_allows(v, n);
	free_paths(v, n);
	return ok;
}

static void do_trash(struct F *f)
{
	if (!egg_ok_selection(f)) {
		say(f, 0, "Left it where it is");
		return;
	}
	int done = 0, failed = 0;
	for (int i = 0; i < f->n; i++) {
		if (!f->sel[i])
			continue;
		char p[PATH_MAX];
		join(p, sizeof p, f->cwd, f->ents[i].name);
		if (trash_path(f, p) < 0) {
			say(f, 1, "Could not move %s to the Trash: %s", f->ents[i].name, strerror(errno));
			failed++;
		} else {
			done++;
		}
	}
	int keep = f->cur;
	refresh(f);
	if (f->n) {
		f->cur = f->anchor = keep < f->n ? keep : f->n - 1;
	}
	if (done && !failed)
		say(f, 0, "Moved %d item%s to the Trash", done, done == 1 ? "" : "s");
}

static void do_delete(struct F *f)
{
	if (!egg_ok_selection(f)) {
		say(f, 0, "Left it where it is");
		return;
	}
	int done = 0, failed = 0;
	char info[PATH_MAX];
	for (int i = 0; i < f->n; i++) {
		if (!f->sel[i])
			continue;
		char p[PATH_MAX];
		join(p, sizeof p, f->cwd, f->ents[i].name);
		if (remove_tree(p) < 0) {
			say(f, 1, "Could not delete %s: %s", f->ents[i].name, strerror(errno));
			failed++;
			continue;
		}
		/* its .trashinfo, next to the files folder */
		snprintf(info, sizeof info, "%.*s/info/%s.trashinfo", (int)(strlen(f->trash_files) - 6),
		         f->trash_files, f->ents[i].name);
		if (!strcmp(f->cwd, f->trash_files))
			unlink(info);
		done++;
	}
	refresh(f);
	if (done && !failed)
		say(f, 0, "Deleted %d item%s", done, done == 1 ? "" : "s");
}

static void open_prompt(struct F *f, int act, const char *title, const char *text)
{
	f->ov = OV_PROMPT;
	f->prompt_act = act;
	snprintf(f->prompt_title, sizeof f->prompt_title, "%s", title);
	snprintf(f->prompt_text, sizeof f->prompt_text, "%s", text);
	f->prompt_err[0] = 0;
	f->ov_new = 1;
}

static void run_action(struct F *f)
{
	int act = f->act;
	f->act = A_NONE;
	switch (act) {
	case A_OPEN:
		open_entry(f, f->act_idx);
		break;
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
	case A_NEWDIR:
		if (in_trash(f))
			say(f, 0, "Cannot create folders in the Trash");
		else {
			char nn[NAME_MAX + 1];
			unique_name(f->cwd, "New folder", 1, nn, sizeof nn);
			open_prompt(f, A_DO_MKDIR, "New folder", nn);
		}
		break;
	case A_RENAME:
		if (count_sel(f) == 1 && f->cur >= 0 && f->sel[f->cur])
			open_prompt(f, A_DO_RENAME, "Rename", f->ents[f->cur].name);
		break;
	case A_TRASH:
		if (!count_sel(f))
			break;
		if (in_trash(f)) {
			f->ov = OV_CONFIRM;
			int c = count_sel(f);
			if (c == 1)
				snprintf(f->confirm_text, sizeof f->confirm_text, "Delete \"%s\" for good? This cannot be undone.",
				         f->ents[f->cur].name);
			else
				snprintf(f->confirm_text, sizeof f->confirm_text, "Delete %d items for good? This cannot be undone.", c);
		} else {
			do_trash(f);
		}
		break;
	case A_COPY:
		do_copy(f, 0);
		break;
	case A_CUT:
		do_copy(f, 1);
		break;
	case A_PASTE:
		do_paste(f);
		break;
	case A_HIDDEN:
		f->show_hidden = !f->show_hidden;
		refresh(f);
		break;
	case A_REFRESH:
		refresh(f);
		places_load(f);
		break;
	case A_SELALL:
		memset(f->sel, 1, (size_t)f->n);
		break;
	case A_DO_MKDIR: {
		char why[160], p[PATH_MAX];
		if (!valid_name(f->prompt_text, why, sizeof why)) {
			snprintf(f->prompt_err, sizeof f->prompt_err, "%s", why);
			f->ov = OV_PROMPT;
			break;
		}
		join(p, sizeof p, f->cwd, f->prompt_text);
		if (mkdir(p, 0777) < 0) {
			snprintf(f->prompt_err, sizeof f->prompt_err, "%s", strerror(errno));
			f->ov = OV_PROMPT;
			break;
		}
		refresh(f);
		select_name(f, f->prompt_text);
		say(f, 0, "Created %s", f->prompt_text);
		break;
	}
	case A_DO_RENAME: {
		char why[160], from[PATH_MAX], to[PATH_MAX];
		if (f->cur < 0 || f->cur >= f->n)
			break;
		if (!valid_name(f->prompt_text, why, sizeof why)) {
			snprintf(f->prompt_err, sizeof f->prompt_err, "%s", why);
			f->ov = OV_PROMPT;
			break;
		}
		if (!strcmp(f->prompt_text, f->ents[f->cur].name))
			break;
		join(from, sizeof from, f->cwd, f->ents[f->cur].name);
		join(to, sizeof to, f->cwd, f->prompt_text);
		if (path_exists(to)) {
			snprintf(f->prompt_err, sizeof f->prompt_err, "A file with that name already exists.");
			f->ov = OV_PROMPT;
			break;
		}
		if (rename(from, to) < 0) {
			snprintf(f->prompt_err, sizeof f->prompt_err, "%s", strerror(errno));
			f->ov = OV_PROMPT;
			break;
		}
		refresh(f);
		select_name(f, f->prompt_text);
		break;
	}
	case A_DO_DELETE:
		do_delete(f);
		break;
	case A_FOCUS_PATH:
		break;
	case A_EXTRACT:
	case A_COMPRESS:
		archive_action(f, act == A_COMPRESS);
		break;
	}
	fl_win_redraw(f->win);
}

/* ---- context menu --------------------------------------------------------------------------------------- */

static void menu_open(struct F *f, int x, int y, int on_item)
{
	int k = 0;
	int one = count_sel(f) == 1;
	if (on_item) {
		f->menu[k++] = (struct menu_item){"Open", "Enter", A_OPEN, one, 1};
		f->menu[k++] = (struct menu_item){"Cut", "Ctrl+X", A_CUT, 1, 0};
		f->menu[k++] = (struct menu_item){"Copy", "Ctrl+C", A_COPY, 1, 0};
		f->menu[k++] = (struct menu_item){"Paste", "Ctrl+V", A_PASTE, 1, 1};
		f->menu[k++] = (struct menu_item){"Rename", "F2", A_RENAME, one, 0};
		int ai = -1; /* the one selected entry, for "Extract here" */
		for (int i = 0; one && i < f->n; i++)
			if (f->sel && f->sel[i])
				ai = i;
		f->menu[k++] = (struct menu_item){"Extract here", "", A_EXTRACT, ai >= 0 && !f->ents[ai].dir && is_archive_name(f->ents[ai].name), 0};
		f->menu[k++] = (struct menu_item){"Compress", "", A_COMPRESS, 1, 0};
		f->menu[k++] = (struct menu_item){in_trash(f) ? "Delete for good" : "Move to Trash", "Del", A_TRASH, 1, 0};
	} else {
		f->menu[k++] = (struct menu_item){"New folder", "Ctrl+Shift+N", A_NEWDIR, 1, 0};
		f->menu[k++] = (struct menu_item){"Paste", "Ctrl+V", A_PASTE, 1, 1};
		f->menu[k++] = (struct menu_item){"Select all", "Ctrl+A", A_SELALL, f->n > 0, 0};
		f->menu[k++] = (struct menu_item){f->show_hidden ? "Hide hidden files" : "Show hidden files", "Ctrl+H",
		                                  A_HIDDEN, 1, 0};
		f->menu[k++] = (struct menu_item){"Refresh", "F5", A_REFRESH, 1, 0};
	}
	f->nmenu = k;
	f->menu_x = x;
	f->menu_y = y;
	f->menu_sel = -1;
	f->ov = OV_MENU;
	f->act_idx = f->cur;
}

#define MENU_W 236
#define MENU_ITEM_H 30

static void draw_menu(fl_ui *ui, struct F *f)
{
	fl_canvas *cv = &ui->cv;
	int h = f->nmenu * MENU_ITEM_H + 8;
	int x = f->menu_x, y = f->menu_y;
	if (x + MENU_W > cv->w)
		x = cv->w - MENU_W - 2;
	if (y + h > cv->h)
		y = cv->h - h - 2;
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	fl_rect box = fl_r(x, y, MENU_W, h);
	fl_fill_rect(cv, box, fl_th.raised);
	fl_stroke_rect(cv, box, 1, fl_th.line);
	int pick = -1;
	if (ui->key == XKB_KEY_Escape) {
		ui->key = 0;
		f->ov = OV_NONE;
		return;
	}
	if (ui->key == XKB_KEY_Down || ui->key == XKB_KEY_Up) {
		int d = ui->key == XKB_KEY_Down ? 1 : -1;
		ui->key = 0;
		for (int t = 0, i = f->menu_sel; t < f->nmenu; t++) {
			i = (i + d + f->nmenu * 2) % f->nmenu;
			if (f->menu[i].enabled) {
				f->menu_sel = i;
				break;
			}
		}
	}
	if ((ui->key == XKB_KEY_Return || ui->key == XKB_KEY_KP_Enter) && f->menu_sel >= 0) {
		ui->key = 0;
		pick = f->menu_sel;
	}
	int inside = fl_in(box, ui->mx, ui->my);
	for (int i = 0; i < f->nmenu; i++) {
		struct menu_item *m = &f->menu[i];
		fl_rect r = fl_r(x + 4, y + 4 + i * MENU_ITEM_H, MENU_W - 8, MENU_ITEM_H);
		int res = fl_widget(ui, ID_MENU + (uint32_t)i, r, 0);
		if (m->enabled && (res & FL_HOVER) && (ui->mx != 0 || ui->my != 0))
			f->menu_sel = i;
		if (m->enabled && i == f->menu_sel)
			fl_fill_rect(cv, r, fl_th.select);
		fl_color fg = m->enabled ? fl_th.text : fl_th.dim;
		fl_text_in(cv, ui->f_reg, fl_inset(r, 0, 0), FL_LEFT, 10, fg, m->label);
		int hw = fl_text_width(f->small, m->hint, -1);
		fl_text_in(cv, f->small, fl_r(r.x + r.w - hw - 10, r.y, hw + 10, r.h), FL_LEFT, 0, fl_th.dim, m->hint);
		if (m->sep && i + 1 < f->nmenu)
			fl_fill_rect(cv, fl_r(r.x + 6, r.y + r.h - 1, r.w - 12, 1), fl_th.line);
		if ((res & FL_CLICK) && m->enabled)
			pick = i;
	}
	if ((ui->press & (FL_BTN_L | FL_BTN_R)) && !inside) {
		f->ov = OV_NONE;
		ui->press = 0;
	}
	if (pick >= 0) {
		f->ov = OV_NONE;
		queue(f, f->menu[pick].act);
		if (f->menu[pick].act == A_OPEN)
			f->act_idx = f->cur;
	}
}

/* ---- dialogs ------------------------------------------------------------------------------------------------ */

static void dialog_frame(fl_ui *ui, struct F *f, int w, int h, fl_rect *box)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, fl_r(0, 0, cv->w, cv->h), fl_th.dark ? 0x99000000 : 0x66000000);
	*box = fl_r((cv->w - w) / 2, (cv->h - h) / 2 - 20, w, h);
	fl_fill_rect(cv, *box, fl_th.panel);
	fl_stroke_rect(cv, *box, 1, fl_th.line);
	fl_fill_rect(cv, fl_r(box->x, box->y, w, 3), fl_th.accent);
}

static void draw_prompt(fl_ui *ui, struct F *f)
{
	fl_canvas *cv = &ui->cv;
	fl_rect box;
	dialog_frame(ui, f, 400, 168, &box);
	if (f->ov_new) {
		ui->focus = ID_DLG;
		f->ov_new = 0;
	}
	int x = box.x + 24, w = box.w - 48, y = box.y + 22;
	fl_text(cv, ui->f_semi, x, y, fl_th.strong, f->prompt_title, -1);
	y += 28;
	int ch = fl_textfield(ui, ID_DLG, fl_r(x, y, w, 36), f->prompt_text, sizeof f->prompt_text, NULL, ui->f_reg);
	if (ch & FL_EDIT_CHANGED)
		f->prompt_err[0] = 0;
	int enter = ch & FL_EDIT_ENTER;
	y += 36 + 6;
	if (f->prompt_err[0])
		fl_text_in(cv, f->small, fl_r(x, y, w, 18), FL_LEFT, 0, fl_th.err, f->prompt_err);
	int by = box.y + box.h - 24 - 34;
	int ok = fl_button(ui, ID_DLG + 1, fl_r(x + w - 96, by, 96, 34), "OK", FL_BTN_ACCENT);
	int cancel = fl_button(ui, ID_DLG + 2, fl_r(x + w - 96 - 8 - 96, by, 96, 34), "Cancel", FL_BTN_OUTLINE);
	if (ui->key == XKB_KEY_Escape) {
		ui->key = 0;
		cancel = 1;
	}
	if (ok || enter) {
		f->ov = OV_NONE;
		ui->focus = ID_GRID;
		queue(f, f->prompt_act);
	} else if (cancel) {
		f->ov = OV_NONE;
		ui->focus = ID_GRID;
	}
}

static void draw_confirm(fl_ui *ui, struct F *f)
{
	fl_canvas *cv = &ui->cv;
	fl_rect box;
	dialog_frame(ui, f, 440, 150, &box);
	int x = box.x + 24, w = box.w - 48, y = box.y + 22;
	fl_text(cv, ui->f_semi, x, y, fl_th.strong, "Delete for good", -1);
	y += 28;
	fl_text_in(cv, ui->f_reg, fl_r(x, y, w, 20), FL_LEFT, 0, fl_th.text, f->confirm_text);
	int by = box.y + box.h - 24 - 34;
	int ok = fl_button(ui, ID_DLG + 1, fl_r(x + w - 96, by, 96, 34), "Delete", FL_BTN_ACCENT);
	int cancel = fl_button(ui, ID_DLG + 2, fl_r(x + w - 96 - 8 - 96, by, 96, 34), "Cancel", FL_BTN_OUTLINE);
	if (ui->key == XKB_KEY_Escape) {
		ui->key = 0;
		cancel = 1;
	}
	if (ok) {
		f->ov = OV_NONE;
		queue(f, A_DO_DELETE);
	} else if (cancel) {
		f->ov = OV_NONE;
	}
}

/* ---- the main view ------------------------------------------------------------------------------------------ */

/* longest prefix of s that fits in w px, on a UTF-8 boundary */
static int fit_prefix(fl_font *fnt, const char *s, int w)
{
	int len = (int)strlen(s), best = 0;
	for (int i = 1; i <= len; i++) {
		if (i < len && ((unsigned char)s[i] & 0xc0) == 0x80)
			continue;
		if (fl_text_width(fnt, s, i) > w)
			break;
		best = i;
	}
	return best ? best : len > 0 ? 1 : 0;
}

static void draw_cell(fl_ui *ui, struct F *f, fl_rect r, int i, int hover)
{
	fl_canvas *cv = &ui->cv;
	struct entry *e = &f->ents[i];
	int sel = f->sel[i], cur = i == f->cur && ui->focus == ID_GRID;
	fl_rect cell = fl_inset(r, 3, 3);
	if (sel)
		fl_fill_rect(cv, cell, fl_th.select);
	else if (hover)
		fl_fill_rect(cv, cell, fl_th.raised);
	if (cur || sel)
		fl_stroke_rect(cv, cell, 1, cur ? fl_th.accent : fl_th.select);
	int ix = r.x + (r.w - ICON) / 2, iy = r.y + 12;
	if (e->dir) {
		fl_icon_draw(cv, FL_ICON_FOLDER_LG, ix, iy, ICON, fl_th.accent);
	} else if (is_text_name(e->name)) {
		fl_icon_draw(cv, FL_ICON_DOC_LG, ix, iy, ICON, fl_th.dim);
	} else {
		fl_icon_draw(cv, FL_ICON_FILE_LG, ix, iy, ICON, fl_th.dim);
	}
	if (e->link) { /* a small arrow tab on the icon's corner */
		fl_fill_rect(cv, fl_r(ix + ICON - 12, iy + ICON - 12, 12, 12), sel ? fl_th.select : fl_th.bg);
		fl_icon_draw(cv, FL_ICON_FORWARD, ix + ICON - 11, iy + ICON - 11, 10, fl_th.text);
	}
	int tw = r.w - 14;
	int n1 = fit_prefix(f->small, e->name, tw);
	int lh = fl_font_height(f->small);
	int ty = iy + ICON + 8;
	fl_color fg = sel ? fl_th.strong : fl_th.text;
	fl_rect save = cv->clip;
	fl_clip(cv, fl_r(r.x + 2, r.y, r.w - 4, r.h));
	int w1 = fl_text_width(f->small, e->name, n1);
	fl_text(cv, f->small, r.x + (r.w - w1) / 2, ty, fg, e->name, n1);
	if (e->name[n1]) {
		char fit[NAME_MAX + 8];
		fl_text_fit(f->small, e->name + n1, tw, fit, sizeof fit);
		int w2 = fl_text_width(f->small, fit, -1);
		fl_text(cv, f->small, r.x + (r.w - w2) / 2, ty + lh + 1, fg, fit, -1);
	}
	cv->clip = save;
}

static void ensure_visible(struct F *f, int cols, int view_h)
{
	if (f->cur < 0)
		return;
	int row = f->cur / cols;
	int top = 8 + row * CELL_H, bot = top + CELL_H;
	if (top < f->scroll)
		f->scroll = top - 8;
	if (bot > f->scroll + view_h)
		f->scroll = bot - view_h + 8;
	if (f->scroll < 0)
		f->scroll = 0;
}

static void click_select(struct F *f, int i, int mods)
{
	if (mods & FL_MOD_SHIFT) {
		int a = f->anchor < 0 ? i : f->anchor;
		if (!(mods & FL_MOD_CTRL))
			memset(f->sel, 0, (size_t)f->n);
		for (int k = a < i ? a : i; k <= (a < i ? i : a); k++)
			f->sel[k] = 1;
	} else if (mods & FL_MOD_CTRL) {
		f->sel[i] = !f->sel[i];
		f->anchor = i;
	} else {
		memset(f->sel, 0, (size_t)f->n);
		f->sel[i] = 1;
		f->anchor = i;
	}
	f->cur = i;
}

static void move_cursor(struct F *f, int to, int shift)
{
	if (f->n == 0)
		return;
	if (to < 0)
		to = 0;
	if (to >= f->n)
		to = f->n - 1;
	if (shift) {
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
}

struct inmask {
	int mouse_in, press, release, wheel, btn_held;
	uint32_t key;
	char text[16];
};

static void mask_input(fl_ui *ui, struct inmask *m)
{
	m->mouse_in = ui->mouse_in;
	m->press = ui->press;
	m->release = ui->release;
	m->wheel = ui->wheel;
	m->btn_held = ui->btn_held;
	m->key = ui->key;
	memcpy(m->text, ui->text, sizeof m->text);
	ui->mouse_in = ui->press = ui->release = ui->wheel = ui->btn_held = 0;
	ui->key = 0;
	ui->text[0] = 0;
}

static void unmask_input(fl_ui *ui, const struct inmask *m)
{
	ui->mouse_in = m->mouse_in;
	ui->press = m->press;
	ui->release = m->release;
	ui->wheel = m->wheel;
	ui->btn_held = m->btn_held;
	ui->key = m->key;
	memcpy(ui->text, m->text, sizeof ui->text);
}

static void typeahead(struct F *f, const char *t)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	long long now = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
	size_t l = strlen(f->type_buf);
	if (now - f->type_ms > 1000)
		l = 0; /* a pause starts a new search */
	f->type_ms = now;
	size_t tl = strlen(t);
	if (l + tl < sizeof f->type_buf) {
		memcpy(f->type_buf + l, t, tl + 1);
		l += tl;
	} else {
		return;
	}
	for (int i = 0; i < f->n; i++)
		if (!strncasecmp(f->ents[i].name, f->type_buf, l)) {
			move_cursor(f, i, 0);
			return;
		}
}

static void draw_toolbar(fl_ui *ui, struct F *f, fl_rect tb)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, tb, fl_th.panel);
	fl_fill_rect(cv, fl_r(tb.x, tb.y + tb.h - 1, tb.w, 1), fl_th.line);
	int x = tb.x + 8, y = tb.y + (tb.h - 32) / 2;
	int can_back = f->hpos > 0, can_fwd = f->hpos + 1 < f->hn, can_up = strcmp(f->cwd, "/") != 0;
	struct { uint32_t id; fl_icon ic; int can; int act; } b[3] = {
		{ID_BACK, FL_ICON_BACK, can_back, A_BACK}, {ID_FWD, FL_ICON_FORWARD, can_fwd, A_FWD},
		{ID_UP, FL_ICON_UP, can_up, A_UP},
	};
	for (int i = 0; i < 3; i++) {
		fl_rect r = fl_r(x, y, 32, 32);
		if (b[i].can) {
			if (fl_icon_button(ui, b[i].id, r, b[i].ic, 14, fl_th.text)) {
				queue(f, b[i].act);
				ui->focus = ID_GRID;
			}
		} else {
			fl_icon_draw(cv, b[i].ic, r.x + 9, r.y + 9, 14, fl_th.line);
		}
		x += 34;
	}
	x += 6;
	fl_rect nr = fl_r(tb.x + tb.w - 8 - 32, y, 32, 32);
	fl_rect pr = fl_r(x, y, nr.x - 8 - x, 32);
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
			/* a file: show its folder, selected */
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
			queue(f, A_NAV);
		}
		ui->focus = ID_GRID;
	}
	if (fl_icon_button(ui, ID_NEW, nr, FL_ICON_PLUS, 14, fl_th.text)) {
		queue(f, A_NEWDIR);
		ui->focus = ID_GRID;
	}
}

static void draw_sidebar(fl_ui *ui, struct F *f, fl_rect sb)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, sb, fl_th.panel);
	fl_fill_rect(cv, fl_r(sb.x + sb.w - 1, sb.y, 1, sb.h), fl_th.line);
	int y = sb.y + 14;
	for (int section = 0; section < 2; section++) {
		fl_text_ex(cv, f->head, sb.x + 16, y, fl_th.dim, section ? "DRIVES" : "PLACES", -1, 56);
		y += fl_font_height(f->head) + 8;
		int any = 0;
		for (int i = 0; i < f->nplaces; i++) {
			struct place *p = &f->places[i];
			if (p->drive != section)
				continue;
			any = 1;
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
				queue(f, A_NAV);
				ui->focus = ID_GRID;
			}
			y += 30;
		}
		if (!any && section)
			fl_text(cv, f->small, sb.x + 16, y + 6, fl_th.dim, "No other drives", -1);
		y += (any ? 0 : 30) + 14;
	}
}

static void draw_status(fl_ui *ui, struct F *f, fl_rect st)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, st, fl_th.panel);
	fl_fill_rect(cv, fl_r(st.x, st.y, st.w, 1), fl_th.line);
	char s[PATH_MAX + 128];
	int c = count_sel(f);
	if (c == 1 && f->cur >= 0 && f->sel[f->cur]) {
		struct entry *e = &f->ents[f->cur];
		char sz[32] = "", when[48];
		struct tm tm;
		localtime_r(&e->mtime, &tm);
		strftime(when, sizeof when, "%-d %b %Y  %H:%M", &tm);
		if (!e->dir)
			fmt_size(e->size, sz, sizeof sz);
		snprintf(s, sizeof s, "%s%s%s   %s", e->name, sz[0] ? "   " : "", sz, when);
	} else if (c > 1) {
		snprintf(s, sizeof s, "%d of %d items selected", c, f->n);
	} else {
		snprintf(s, sizeof s, "%d item%s%s", f->n, f->n == 1 ? "" : "s", f->show_hidden ? "  (hidden files shown)" : "");
	}
	int mw = f->msg[0] ? fl_text_width(f->small, f->msg, -1) : 0;
	int avail = st.w - 28 - (mw ? mw + 24 : 0);
	char fit[PATH_MAX + 130];
	fl_text_fit(f->small, s, avail, fit, sizeof fit);
	fl_text_in(cv, f->small, fl_r(st.x + 14, st.y + 1, avail, st.h - 1), FL_LEFT, 0, fl_th.dim, fit);
	if (mw)
		fl_text_in(cv, f->small, fl_r(st.x + st.w - 14 - mw, st.y + 1, mw + 14, st.h - 1), FL_LEFT, 0,
		           f->msg_err ? fl_th.err : fl_th.text, f->msg);
}

static void draw_grid(fl_ui *ui, struct F *f, fl_rect g)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, g, fl_th.bg);
	int res = fl_widget(ui, ID_GRID, g, FL_FOCUSABLE | FL_NO_FOCUS_RING);
	int cols = (g.w - 16) / CELL_W;
	if (cols < 1)
		cols = 1;
	int rows = (f->n + cols - 1) / cols;
	int content = rows * CELL_H + 16;
	int mods = fl_app_mods(f->app);
	int x0 = g.x + (g.w - cols * CELL_W) / 2;
	if (x0 < g.x + 8)
		x0 = g.x + 8;

	if ((res & FL_HOVER) && ui->wheel) {
		f->scroll += ui->wheel * 48;
		ui->wheel = 0;
	}
	/* keys, unless the path field has them */
	if (ui->focus != ID_PATH && ui->key) {
		uint32_t k = ui->key;
		int shift = mods & FL_MOD_SHIFT, ctrl = mods & FL_MOD_CTRL, alt = mods & FL_MOD_ALT;
		int page = (g.h / CELL_H) * cols;
		int handled = 1;
		if (ctrl) {
			switch (k) {
			case 'a': case 'A': queue(f, A_SELALL); break;
			case 'c': case 'C': queue(f, A_COPY); break;
			case 'x': case 'X': queue(f, A_CUT); break;
			case 'v': case 'V': queue(f, A_PASTE); break;
			case 'h': case 'H': queue(f, A_HIDDEN); break;
			case 'l': case 'L': ui->focus = ID_PATH; break;
			case 'n': case 'N': if (shift) queue(f, A_NEWDIR); else handled = 0; break;
			default: handled = 0; break;
			}
		} else if (alt) {
			switch (k) {
			case XKB_KEY_Left: queue(f, A_BACK); break;
			case XKB_KEY_Right: queue(f, A_FWD); break;
			case XKB_KEY_Up: queue(f, A_UP); break;
			default: handled = 0; break;
			}
		} else {
			switch (k) {
			case XKB_KEY_Left: move_cursor(f, f->cur < 0 ? 0 : f->cur - 1, shift); break;
			case XKB_KEY_Right: move_cursor(f, f->cur < 0 ? 0 : f->cur + 1, shift); break;
			case XKB_KEY_Up: move_cursor(f, f->cur < 0 ? 0 : f->cur - cols, shift); break;
			case XKB_KEY_Down: move_cursor(f, f->cur < 0 ? 0 : f->cur + cols, shift); break;
			case XKB_KEY_Page_Up: move_cursor(f, f->cur - page, shift); break;
			case XKB_KEY_Page_Down: move_cursor(f, f->cur + page, shift); break;
			case XKB_KEY_Home: move_cursor(f, 0, shift); break;
			case XKB_KEY_End: move_cursor(f, f->n - 1, shift); break;
			case XKB_KEY_Return:
			case XKB_KEY_KP_Enter:
				if (f->cur >= 0 && f->sel[f->cur]) {
					f->act_idx = f->cur;
					queue(f, A_OPEN);
				}
				break;
			case XKB_KEY_BackSpace: queue(f, A_UP); break;
			case XKB_KEY_Delete: queue(f, A_TRASH); break;
			case XKB_KEY_F2: queue(f, A_RENAME); break;
			case XKB_KEY_F5: queue(f, A_REFRESH); break;
			case XKB_KEY_Menu:
				menu_open(f, g.x + 40, g.y + 40, count_sel(f) > 0);
				break;
			default: handled = 0; break;
			}
			if (!handled && ui->text[0] && ui->text[0] != ' ' ) {
				typeahead(f, ui->text);
				handled = 1;
			}
			if (handled && k >= XKB_KEY_Left && k <= XKB_KEY_Down)
				ensure_visible(f, cols, g.h);
			if (handled && (k == XKB_KEY_Page_Up || k == XKB_KEY_Page_Down || k == XKB_KEY_Home || k == XKB_KEY_End))
				ensure_visible(f, cols, g.h);
		}
		if (handled) {
			ui->key = 0;
			ui->text[0] = 0;
		}
	}
	if (ui->text[0] && ui->focus == ID_GRID && !(mods & (FL_MOD_CTRL | FL_MOD_ALT))) {
		typeahead(f, ui->text);
		ensure_visible(f, cols, g.h);
		ui->text[0] = 0;
	}

	int max = content > g.h ? content - g.h : 0;
	if (f->scroll > max)
		f->scroll = max;
	if (f->scroll < 0)
		f->scroll = 0;

	fl_rect save = cv->clip;
	fl_clip(cv, g);
	int rclick = -1, hit = -1;
	int first = f->scroll / CELL_H * cols;
	int last = (f->scroll + g.h) / CELL_H * cols + cols;
	for (int i = first; i < last && i < f->n; i++) {
		int col = i % cols, row = i / cols;
		fl_rect r = fl_r(x0 + col * CELL_W, g.y + 8 + row * CELL_H - f->scroll, CELL_W, CELL_H);
		int cr = fl_widget(ui, ID_CELL + (uint32_t)i, r, 0);
		draw_cell(ui, f, r, i, cr & FL_HOVER);
		if (cr & FL_HOVER) {
			hit = i;
			if (ui->press & FL_BTN_L) {
				click_select(f, i, mods);
				if (ui->clicks >= 2 && !(mods & (FL_MOD_CTRL | FL_MOD_SHIFT))) {
					f->act_idx = i;
					queue(f, A_OPEN);
				}
			}
			if (cr & FL_RCLICK)
				rclick = i;
		}
	}
	cv->clip = save;
	if (hit < 0 && (res & FL_HOVER) && (ui->press & FL_BTN_L) && !(mods & (FL_MOD_CTRL | FL_MOD_SHIFT))) {
		memset(f->sel, 0, (size_t)f->n);
		f->cur = -1;
	}
	if (rclick >= 0) {
		if (!f->sel[rclick])
			click_select(f, rclick, 0);
		menu_open(f, ui->mx, ui->my, 1);
	} else if ((res & FL_RCLICK) && hit < 0) {
		menu_open(f, ui->mx, ui->my, 0);
	}
	if (res & FL_HOVER)
		ui->cursor = FL_CURSOR_ARROW;
	if (max) {
		int sbh = g.h - 4;
		fl_scrollbar(ui, ID_SCROLL, fl_r(g.x + g.w - 6, g.y + 2, 4, sbh), &f->scroll, content, g.h);
	}
	if (f->n == 0)
		fl_text_in(cv, ui->f_reg, g, FL_CENTER, 0, fl_th.dim, "This folder is empty");
}

static void draw(fl_ui *ui, void *ud)
{
	struct F *f = ud;
	fl_canvas *cv = &ui->cv;
	fl_rect c = fl_win_content(ui->win);
	if (!f->inited) {
		ui->focus = ID_GRID;
		f->inited = 1;
	}
	struct inmask im = {0};
	int modal = f->ov != OV_NONE;
	if (modal)
		mask_input(ui, &im);

	fl_rect area = c;
	fl_rect tb = fl_cut_top(&area, TOOL_H);
	fl_rect st = fl_cut_bottom(&area, STATUS_H);
	fl_rect sb = fl_cut_left(&area, SIDE_W);
	fl_fill_rect(cv, c, fl_th.bg);
	draw_grid(ui, f, area);
	draw_sidebar(ui, f, sb);
	draw_toolbar(ui, f, tb);
	draw_status(ui, f, st);

	if (modal) {
		unmask_input(ui, &im);
		if (f->ov == OV_MENU)
			draw_menu(ui, f);
		else if (f->ov == OV_PROMPT)
			draw_prompt(ui, f);
		else if (f->ov == OV_CONFIRM)
			draw_confirm(ui, f);
		if (f->ov == OV_NONE && ui->focus != ID_GRID && !f->act)
			ui->focus = ID_GRID;
		ui->key = 0;
		ui->text[0] = 0;
		ui->press = ui->release = 0;
	}
	if (f->act)
		run_action(f);
	else if ((f->ov != OV_NONE) != modal)
		fl_win_redraw(ui->win);
}

static void on_focus(fl_win *w, int focused, void *ud)
{
	struct F *f = ud;
	if (focused) {
		places_load(f);
		fl_win_redraw(w);
	}
}

int main(int argc, char **argv)
{
	if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
		fprintf(stderr, "usage: flfiles [FOLDER]\nBrowse files. Ctrl-H shows hidden files.\n");
		return argc == 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) ? 0 : 2;
	}
	signal(SIGCHLD, SIG_IGN);
	struct F *f = calloc(1, sizeof *f);
	if (!f)
		return 1;
	f->app = fl_app_new(NULL, NULL);
	if (!f->app) {
		fprintf(stderr, "flfiles: cannot connect to a Wayland compositor\n");
		return 1;
	}
	f->small = fl_font_ui(400, 12);
	f->head = fl_font_ui(600, 10.5f);
	f->hpos = -1;
	f->cur = f->anchor = -1;
	const char *home = getenv("HOME");
	if (home && *home) {
		const char *xdg = getenv("XDG_DATA_HOME");
		if (xdg && *xdg)
			snprintf(f->trash_files, sizeof f->trash_files, "%s/Trash/files", xdg);
		else
			snprintf(f->trash_files, sizeof f->trash_files, "%s/.local/share/Trash/files", home);
	}
	f->ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	f->ino_wd = -1;
	f->msg_timer = fl_timer_new(f->app, msg_clear, f);

	fl_win_opts o = {
		.title = "Files", .app_id = "flfiles", .w = 860, .h = 560, .min_w = 520, .min_h = 320,
		.decorated = 1, .resizable = 1, .draw = draw, .focus = on_focus, .ud = f,
	};
	f->win = fl_win_new(f->app, &o);
	if (!f->win)
		return 1;
	if (f->ino_fd >= 0)
		fl_fd_add(f->app, f->ino_fd, POLLIN, inotify_ready, f);

	const char *start = argc == 2 ? argv[1] : home && *home ? home : "/";
	if (load_dir(f, start, 1) < 0) {
		char first[300];
		snprintf(first, sizeof first, "%s", f->msg);
		if (load_dir(f, "/", 1) == 0)
			say(f, 1, "%s", first);
	}
	fl_app_watch_conf(f->app, NULL, NULL);
	fl_app_run(f->app);
	fl_app_free(f->app);
	return 0;
}
