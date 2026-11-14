/* deskicons.c -- the desktop: programs that ask for an icon, and the files in ~/Desktop, on a grid.
 *
 * One raw surface covers the output (fl_shell set_desktop, version 6), below every window and above
 * the wallpaper. Its pixels are nearly transparent (alpha 1), so the pointer lands here everywhere:
 * a right click on the empty desktop opens the desktop menu, a right click on an icon its menu.
 * Icons: the programs whose .desktop file says X-FreeLFS-Desktop-Icon=true (the first grid row
 * of apps), and every file or folder in ~/Desktop. Click selects, Ctrl-click adds, a drag on
 * empty desktop rubber-band selects, dragging moves the selection and snaps it to the grid. Double
 * click opens. Positions and "show desktop icons" are kept in
 * $XDG_CONFIG_HOME/freelfs/desktop-icons.conf (the session's own config). ~/Desktop is looked at
 * every few seconds, so files made elsewhere appear.
 * The icon menu: Open, Open with (files; the apps of the file's type, fileinfo.c), Rename, Move to
 * Trash, Properties (fileinfo.c). The desktop menu: wallpaper and display settings (flsettings).
 * Not done: the drop of files from Files (flui and flcomp have no drag and drop yet: the target
 * side is desk_import_uris, the source side is Files' own drag), desktop icons dragged into Files.
 * SPDX-License-Identifier: MIT
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <flui.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "flpanel.h"

#define CELL_W 104
#define CELL_H 98
#define TILE 48
#define MARGIN 24
#define MAX_FILES 512
#define INPUT_BG 0x01000000	/* alpha 1: invisible, but it takes the pointer */

enum { IT_APP, IT_FILE, IT_DIR };
enum {
	C_NEWDIR = 1, C_NEWFILE, C_TERM, C_WALL, C_SETTINGS, C_TOGGLE, C_OPEN, C_RENAME, C_TRASH,
	C_OPENWITH, C_PROPS, C_DISPLAY,
	C_CHOICE = 100	/* C_CHOICE + i: the i-th app of Open with */
};
enum { K_NEWDIR = 1, K_NEWFILE, K_RENAME };

struct item {
	int kind, sel, placed, col, row;
	char name[256], key[300], path[512], exec[256], glyph[8];
	int terminal;
};

struct saved {
	char key[300];
	int col, row;
};

static struct desk {
	struct panel *p;
	fl_win *w;
	fl_font *f_glyph;
	struct item *it;
	int n;
	struct saved *sv;
	int nsv;
	int show, ow, oh, maxrows;
	char dir[512], conf[512];
	fl_timer *scan, *fit;
	int cw, ch;	/* the size of the surface now */
	int drag, dragging, px, py, rb, rbx, rby;
	/* the menu's subject, copied when it opens: the list may change under it */
	int menu_kind, menu_cx;
	char menu_key[300], menu_path[512];
	/* Open with: the apps offered for menu_path, picked by code C_CHOICE + i */
	struct app_choice choice[APP_MAX];
	int nchoice;
} D = { .drag = -1 };

/* ---- grid -------------------------------------------------------------------------------------- */

static int grid_x(void) { return MARGIN; }
static int grid_y(void) { return MARGIN + (fl_lk.panel_top ? fl_lk.panel_h : 0); }

static fl_rect cell_of(int col, int row)
{
	return fl_r(grid_x() + col * CELL_W, grid_y() + row * CELL_H, CELL_W, CELL_H - 8);
}

static void grid_size(void)
{
	int bottom = fl_lk.panel_top ? 0 : fl_lk.panel_h;
	D.maxrows = (D.oh - grid_y() - bottom - MARGIN) / CELL_H;
	if (D.maxrows < 1)
		D.maxrows = 1;
}

static int item_at(int x, int y)
{
	for (int i = D.n - 1; i >= 0; i--)
		if (D.it[i].placed && fl_in(cell_of(D.it[i].col, D.it[i].row), x, y))
			return i;
	return -1;
}

static int occupied(int col, int row, int except)
{
	for (int i = 0; i < D.n; i++)
		if (i != except && D.it[i].placed && D.it[i].col == col && D.it[i].row == row)
			return 1;
	return 0;
}

static int saved_find(const char *key)
{
	for (int i = 0; i < D.nsv; i++)
		if (!strcmp(D.sv[i].key, key))
			return i;
	return -1;
}

static void saved_set(const char *key, int col, int row)
{
	int i = saved_find(key);
	if (i < 0) {
		struct saved *n = realloc(D.sv, (size_t)(D.nsv + 1) * sizeof *D.sv);
		if (!n)
			return;
		D.sv = n;
		i = D.nsv++;
		snprintf(D.sv[i].key, sizeof D.sv[i].key, "%s", key);
	}
	D.sv[i].col = col;
	D.sv[i].row = row;
}

/* the first free cell in the default order: down a column, then the next one */
static void place_unplaced(void)
{
	for (int i = 0; i < D.n; i++) {
		struct item *it = &D.it[i];
		if (it->placed)
			continue;
		int s = saved_find(it->key);
		if (s >= 0 && D.sv[s].row >= 0 && D.sv[s].row < D.maxrows && !occupied(D.sv[s].col, D.sv[s].row, i)) {
			it->col = D.sv[s].col;
			it->row = D.sv[s].row;
		} else {
			for (int k = 0;; k++) {
				int c = k / D.maxrows, r = k % D.maxrows;
				if (!occupied(c, r, i)) {
					it->col = c;
					it->row = r;
					break;
				}
			}
		}
		it->placed = 1;
	}
}

/* ---- the config file --------------------------------------------------------------------------- */

static int mkdirs(const char *path)
{
	char tmp[1024];
	snprintf(tmp, sizeof tmp, "%s", path);
	for (char *s = tmp + 1; *s; s++)
		if (*s == '/') {
			*s = 0;
			mkdir(tmp, 0700);
			*s = '/';
		}
	return mkdir(tmp, 0700) == 0 || errno == EEXIST ? 0 : -1;
}

static void conf_load(void)
{
	FILE *f = fopen(D.conf, "r");
	if (!f)
		return;
	char line[600];
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		char *eq = strrchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		if (!strcmp(line, "show")) {
			D.show = atoi(eq + 1) != 0;
			continue;
		}
		int c, r;
		if (sscanf(eq + 1, "%d,%d", &c, &r) == 2 && c >= 0 && r >= 0)
			saved_set(line, c, r);
	}
	fclose(f);
}

static void conf_save(void)
{
	char tmp[600], dir[600];
	snprintf(dir, sizeof dir, "%s", D.conf);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = 0;
		mkdirs(dir);
	}
	snprintf(tmp, sizeof tmp, "%s.tmp", D.conf);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "show=%d\n", D.show);
	for (int i = 0; i < D.nsv; i++)
		fprintf(f, "%s=%d,%d\n", D.sv[i].key, D.sv[i].col, D.sv[i].row);
	if (fclose(f) == 0)
		rename(tmp, D.conf);
}

/* ---- the files in ~/Desktop -------------------------------------------------------------------- */

static int cmp_name(const void *a, const void *b)
{
	return strcmp((const char *)a, (const char *)b);
}

/* the folder's entries as they are now; the items keep their cells, new ones take a free one */
static void scan_files(void)
{
	static char names[MAX_FILES][256];
	int nn = 0;
	DIR *d = opendir(D.dir);
	if (d) {
		struct dirent *e;
		while (nn < MAX_FILES && (e = readdir(d)))
			if (e->d_name[0] != '.')
				snprintf(names[nn++], sizeof names[0], "%s", e->d_name);
		closedir(d);
	}
	qsort(names, (size_t)nn, sizeof names[0], cmp_name);

	int apps = 0, old_files = 0;
	for (int i = 0; i < D.n; i++)
		if (D.it[i].kind == IT_APP)
			apps++;
		else
			old_files++;
	int changed = old_files != nn;
	for (int k = 0; k < nn && !changed; k++) {
		int found = 0;
		for (int i = 0; i < D.n && !found; i++)
			found = D.it[i].kind != IT_APP && !strcmp(D.it[i].name, names[k]);
		changed = !found;
	}
	if (!changed)
		return;

	struct item *nw = calloc((size_t)(apps + nn + 1), sizeof *nw);
	if (!nw)
		return;
	int m = 0;
	for (int i = 0; i < D.n; i++)
		if (D.it[i].kind == IT_APP)
			nw[m++] = D.it[i];
	for (int k = 0; k < nn; k++) {
		struct item *it = &nw[m];
		int old = -1;
		for (int i = 0; i < D.n && old < 0; i++)
			if (D.it[i].kind != IT_APP && !strcmp(D.it[i].name, names[k]))
				old = i;
		if (old >= 0) {
			*it = D.it[old];
		} else {
			snprintf(it->name, sizeof it->name, "%s", names[k]);
			snprintf(it->key, sizeof it->key, "%s", names[k]);
			snprintf(it->path, sizeof it->path, "%s/%s", D.dir, names[k]);
			struct stat st;
			it->kind = stat(it->path, &st) == 0 && S_ISDIR(st.st_mode) ? IT_DIR : IT_FILE;
		}
		m++;
	}
	free(D.it);
	D.it = nw;
	D.n = m;
	place_unplaced();
	if (D.w) {
		fl_win_redraw(D.w);
	}
}

static void scan_tick(void *ud)
{
	(void)ud;
	if (!D.w || D.drag >= 0 || D.rb)
		return;
	scan_files();
}

/* ---- actions ----------------------------------------------------------------------------------- */

static void open_item(const struct item *it)
{
	char q[600];
	if (it->kind == IT_APP) {
		launch(it->exec, it->terminal);
	} else if (fl_shq(q, sizeof q, it->path)) {
		char cmd[700];
		snprintf(cmd, sizeof cmd, "%s %s", it->kind == IT_DIR ? "flfiles" : "gio open", q);
		launch(cmd, 0);
	}
}

static int percent_enc(const char *in, char *out, size_t cap)
{
	size_t n = 0;
	for (const unsigned char *s = (const unsigned char *)in; *s; s++) {
		int keep = (*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
		           strchr("-._~/", *s) != NULL;
		if (n + 4 >= cap)
			return 0;
		if (keep)
			out[n++] = (char)*s;
		else
			n += (size_t)snprintf(out + n, cap - n, "%%%02X", *s);
	}
	out[n] = 0;
	return 1;
}

/* freedesktop trash: files/NAME and info/NAME.trashinfo, the info written first (O_EXCL picks a free name) */
static int trash_file(const char *path, char *err, size_t errcap)
{
	const char *base = strrchr(path, '/');
	base = base ? base + 1 : path;
	const char *xd = getenv("XDG_DATA_HOME");
	char root[480], files[512], info[512];
	if (xd && *xd)
		snprintf(root, sizeof root, "%s/Trash", xd);
	else
		snprintf(root, sizeof root, "%s/.local/share/Trash", getenv("HOME") ? getenv("HOME") : "");
	snprintf(files, sizeof files, "%s/files", root);
	snprintf(info, sizeof info, "%s/info", root);
	if (mkdirs(files) < 0 || mkdirs(info) < 0) {
		snprintf(err, errcap, "%s", strerror(errno));
		return -1;
	}
	for (int n = 1; n < 1000; n++) {
		char name[256], ip[800], dst[800], enc[1536], when[32], body[2048];
		if (n == 1)
			snprintf(name, sizeof name, "%s", base);
		else
			snprintf(name, sizeof name, "%s %d", base, n);
		snprintf(ip, sizeof ip, "%s/%s.trashinfo", info, name);
		int fd = open(ip, O_WRONLY | O_CREAT | O_EXCL, 0600);
		if (fd < 0) {
			if (errno == EEXIST)
				continue;
			snprintf(err, errcap, "%s", strerror(errno));
			return -1;
		}
		time_t t = time(NULL);
		struct tm tm;
		localtime_r(&t, &tm);
		strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", &tm);
		percent_enc(path, enc, sizeof enc);
		int bl = snprintf(body, sizeof body, "[Trash Info]\nPath=%s\nDeletionDate=%s\n", enc, when);
		if (write(fd, body, (size_t)bl) != bl) {
			close(fd);
			unlink(ip);
			snprintf(err, errcap, "%s", "could not write the trash record");
			return -1;
		}
		close(fd);
		snprintf(dst, sizeof dst, "%s/%s", files, name);
		if (rename(path, dst) < 0) {
			int e = errno;
			unlink(ip);
			snprintf(err, errcap, "%s", strerror(e));
			return -1;
		}
		return 0;
	}
	snprintf(err, errcap, "%s", "too many files with that name in the trash");
	return -1;
}

/* a name for a new item: BASE, then BASE 2, BASE 3 ... (ext is put back after the number) */
static void unique_name(const char *dir, const char *base, const char *ext, char *out, size_t cap)
{
	for (int n = 1; n < 1000; n++) {
		char path[700];
		if (n == 1)
			snprintf(out, cap, "%s%s", base, ext);
		else
			snprintf(out, cap, "%s %d%s", base, n, ext);
		snprintf(path, sizeof path, "%s/%s", dir, out);
		struct stat st;
		if (stat(path, &st) != 0)
			return;
	}
}

static void name_done(struct panel *p, int kind, const char *ctx, const char *name)
{
	char err[200] = "", dst[800];
	if (!strcmp(name, ".") || !strcmp(name, "..") || strchr(name, '/')) {
		dlg_error(p, "Name not allowed", "A name cannot contain a slash, and not be . or ..");
		return;
	}
	if (kind == K_NEWDIR) {
		snprintf(dst, sizeof dst, "%s/%s", ctx, name);
		if (mkdir(dst, 0755) < 0)
			snprintf(err, sizeof err, "%s", strerror(errno));
	} else if (kind == K_NEWFILE) {
		snprintf(dst, sizeof dst, "%s/%s", ctx, name);
		int fd = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0644);
		if (fd < 0)
			snprintf(err, sizeof err, "%s", strerror(errno));
		else
			close(fd);
	} else if (kind == K_RENAME) {
		const char *slash = strrchr(ctx, '/');
		size_t dl = slash ? (size_t)(slash - ctx) : 0;
		snprintf(dst, sizeof dst, "%.*s/%s", (int)dl, ctx, name);
		if (!strcmp(dst, ctx))
			return;
		if (rename(ctx, dst) < 0)
			snprintf(err, sizeof err, "%s", strerror(errno));
	}
	if (err[0]) {
		dlg_error(p, "Could not do that", err);
		return;
	}
	scan_files();
}

static void new_here(int kind)
{
	char name[256];
	if (kind == K_NEWDIR) {
		unique_name(D.dir, "New folder", "", name, sizeof name);
		dlg_name(D.p, "New folder", name, K_NEWDIR, D.dir, name_done);
	} else {
		unique_name(D.dir, "New text file", ".txt", name, sizeof name);
		dlg_name(D.p, "New text file", name, K_NEWFILE, D.dir, name_done);
	}
}

/* ---- files dropped on the desktop --------------------------------------------------------------
 * text/uri-list (RFC 2483): one file:// URI per line, lines starting with # are comments, and
 * the lines end in CRLF or LF. The drop (the Wayland side is not wired yet) hands the list here.
 * The destination names are picked here: a taken name gets " (2)", " (3)" in front of the
 * extension. One sh runs all the moves or copies (mv, or cp -R for a copy), in a child process
 * that the panel does not wait for: a double fork, so the worker is reaped by init and the panel
 * needs no SIGCHLD handling. The desktop's scan shows the new files within 4 s. */

#define DROP_MAX 64

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* the local paths of a text/uri-list, percent-decoded; file://host/... is left out */
static int uri_paths(const char *list, char (*out)[PATH_MAX], int max)
{
	int n = 0;
	const char *s = list;
	while (*s && n < max) {
		const char *e = strpbrk(s, "\r\n");
		size_t len = e ? (size_t)(e - s) : strlen(s);
		if (len > 8 && s[0] != '#' && !strncmp(s, "file:///", 8)) {
			char path[PATH_MAX];
			size_t k = 0;
			for (size_t i = 7; i < len && k + 1 < sizeof path; i++) {
				if (s[i] == '%' && i + 2 < len && hexval((unsigned char)s[i + 1]) >= 0 &&
				    hexval((unsigned char)s[i + 2]) >= 0) {
					path[k++] = (char)(hexval((unsigned char)s[i + 1]) * 16 + hexval((unsigned char)s[i + 2]));
					i += 2;
				} else {
					path[k++] = s[i];
				}
			}
			path[k] = 0;
			if (path[0] == '/' && k > 1) {
				snprintf(out[n], PATH_MAX, "%s", path);
				n++;
			}
		}
		if (!e)
			break;
		s = e + 1;
	}
	return n;
}

/* NAME in dir, or NAME (2), NAME (3) ... before the extension, whichever is free */
static void dest_name(const char *dir, const char *name, char *out, size_t cap)
{
	const char *dot = strrchr(name, '.');
	if (dot == name)
		dot = NULL; /* a dot file has no extension */
	size_t base = dot ? (size_t)(dot - name) : strlen(name);
	for (int n = 1; n < 1000; n++) {
		if (n == 1)
			snprintf(out, cap, "%s", name);
		else
			snprintf(out, cap, "%.*s (%d)%s", (int)base, name, n, dot ? dot : "");
		char path[PATH_MAX + 300];
		snprintf(path, sizeof path, "%s/%s", dir, out);
		struct stat st;
		if (lstat(path, &st) != 0)
			return;
	}
}

void desk_import_uris(struct panel *p, const char *list, int move)
{
	static char src[DROP_MAX][PATH_MAX], dst[DROP_MAX][PATH_MAX];
	if (!D.w || !list)
		return;
	(void)p;
	int n = uri_paths(list, src, DROP_MAX), k = 0;
	char *argv[4 + 2 * DROP_MAX + 1];
	int a = 0;
	const char *script = move
		? "while [ $# -ge 2 ]; do mv -- \"$1\" \"$2\" || true; shift 2; done"
		: "while [ $# -ge 2 ]; do cp -R -- \"$1\" \"$2\" || true; shift 2; done";
	argv[a++] = "sh";
	argv[a++] = "-c";
	argv[a++] = (char *)script;
	argv[a++] = "sh";
	for (int i = 0; i < n; i++) {
		const char *slash = strrchr(src[i], '/');
		const char *name = slash ? slash + 1 : src[i];
		if (!name[0])
			continue;
		char here[PATH_MAX];
		snprintf(here, sizeof here, "%s", src[i]);
		char *cut = strrchr(here, '/');
		if (cut)
			*cut = 0;
		if (move && !strcmp(here, D.dir))
			continue; /* already on the desktop */
		char picked[256];
		dest_name(D.dir, name, picked, sizeof picked);
		snprintf(dst[k], sizeof dst[k], "%s/%s", D.dir, picked);
		argv[a++] = src[i];
		argv[a++] = dst[k];
		k++;
	}
	argv[a] = NULL;
	if (k == 0)
		return;
	pid_t mid = fork();
	if (mid < 0)
		return;
	if (mid == 0) {
		pid_t worker = fork();
		if (worker == 0) {
			signal(SIGCHLD, SIG_DFL);
			execv("/bin/sh", argv);
			_exit(127);
		}
		_exit(worker < 0 ? 1 : 0);
	}
	waitpid(mid, NULL, 0); /* the middle one exits at once */
}

static void desk_pick(struct panel *p, uintptr_t ud, int code);

/* Open with: the apps for the file, as a second menu at the same place */
static void open_with_menu(struct panel *p)
{
	char mime[128];
	struct ctx_item it[APP_MAX];
	int n = fileinfo_apps(D.menu_path, D.choice, APP_MAX, mime, sizeof mime);
	D.nchoice = n;
	if (n == 0) {
		static const struct ctx_item none[] = {{"No app opens this file", 0, 0}};
		ctx_open(p, D.menu_cx, none, 1, desk_pick, 0);
		return;
	}
	for (int i = 0; i < n; i++)
		it[i] = (struct ctx_item){D.choice[i].name, 1, C_CHOICE + i};
	ctx_open(p, D.menu_cx, it, n, desk_pick, 0);
}

static void desk_pick(struct panel *p, uintptr_t ud, int code)
{
	(void)ud;
	char err[200];
	if (code >= C_CHOICE) {
		int i = code - C_CHOICE;
		if (i < D.nchoice)
			fileinfo_open_with(D.choice[i].desk, D.menu_path);
		return;
	}
	switch (code) {
	case C_NEWDIR:
		new_here(K_NEWDIR);
		break;
	case C_NEWFILE:
		new_here(K_NEWFILE);
		break;
	case C_TERM: {
		char q[600], cmd[700];
		if (fl_shq(q, sizeof q, D.dir)) {
			snprintf(cmd, sizeof cmd, "cd %s && exec flterm", q);
			launch(cmd, 0);
		}
		break;
	}
	case C_WALL:
		launch("flsettings --page wallpaper", 0);
		break;
	case C_DISPLAY:
		launch("flsettings --page display", 0);
		break;
	case C_OPENWITH:
		open_with_menu(p);
		break;
	case C_PROPS:
		props_open(p, D.menu_path, D.menu_kind == IT_DIR);
		break;
	case C_SETTINGS:
		launch("flsettings", 0);
		break;
	case C_TOGGLE:
		D.show = !D.show;
		conf_save();
		if (D.w)
			fl_win_redraw(D.w);
		break;
	case C_OPEN:
		for (int i = 0; i < D.n; i++)
			if (!strcmp(D.it[i].key, D.menu_key))
				open_item(&D.it[i]);
		break;
	case C_RENAME: {
		const char *slash = strrchr(D.menu_path, '/');
		dlg_name(p, "Rename", slash ? slash + 1 : D.menu_path, K_RENAME, D.menu_path, name_done);
		break;
	}
	case C_TRASH:
		if (trash_file(D.menu_path, err, sizeof err) < 0)
			dlg_error(p, "Could not move it to the trash", err);
		else
			scan_files();
		break;
	}
}

static void desk_menu(int cx)
{
	static const struct ctx_item it[] = {
		{"New folder", 1, C_NEWDIR},
		{"New text file", 1, C_NEWFILE},
		{"Open terminal here", 1, C_TERM},
		{NULL, 0, 0},
		{"Change wallpaper\xe2\x80\xa6", 1, C_WALL},
		{"Display settings\xe2\x80\xa6", 1, C_DISPLAY},
		{"Settings", 1, C_SETTINGS},
		{NULL, 0, 0},
	};
	struct ctx_item all[12];
	int n = 0;
	for (size_t i = 0; i < sizeof it / sizeof it[0]; i++)
		all[n++] = it[i];
	all[n++] = (struct ctx_item){D.show ? "Hide desktop icons" : "Show desktop icons", 1, C_TOGGLE};
	ctx_open(D.p, cx, all, n, desk_pick, 0);
}

static void item_menu(const struct item *it, int cx)
{
	D.menu_kind = it->kind;
	D.menu_cx = cx;
	snprintf(D.menu_key, sizeof D.menu_key, "%s", it->key);
	snprintf(D.menu_path, sizeof D.menu_path, "%s", it->path);
	struct ctx_item all[8];
	int n = 0;
	all[n++] = (struct ctx_item){"Open", 1, C_OPEN};
	if (it->kind == IT_FILE)
		all[n++] = (struct ctx_item){"Open with\xe2\x80\xa6", 1, C_OPENWITH};
	if (it->kind != IT_APP) {
		all[n++] = (struct ctx_item){NULL, 0, 0};
		all[n++] = (struct ctx_item){"Rename\xe2\x80\xa6", 1, C_RENAME};
		all[n++] = (struct ctx_item){"Move to Trash", 1, C_TRASH};
		all[n++] = (struct ctx_item){NULL, 0, 0};
		all[n++] = (struct ctx_item){"Properties", 1, C_PROPS};
	}
	ctx_open(D.p, cx, all, n, desk_pick, 0);
}

/* ---- drawing and pointer ----------------------------------------------------------------------- */

static void select_only(int i)
{
	for (int k = 0; k < D.n; k++)
		D.it[k].sel = 0;
	if (i >= 0)
		D.it[i].sel = 1;
}

/* the selected icons moved by (dx, dy) pixels: each goes to the nearest free cell, or stays */
static void finish_drag(int dx, int dy)
{
	int moved = 0;
	for (int i = 0; i < D.n; i++) {
		struct item *it = &D.it[i];
		if (!it->sel || !it->placed)
			continue;
		int dc = (dx + (dx >= 0 ? CELL_W / 2 : -CELL_W / 2)) / CELL_W;
		int dr = (dy + (dy >= 0 ? CELL_H / 2 : -CELL_H / 2)) / CELL_H;
		int nc = it->col + dc, nr = it->row + dr;
		if (nc < 0)
			nc = 0;
		if (nr < 0)
			nr = 0;
		if (nr >= D.maxrows)
			nr = D.maxrows - 1;
		if ((nc != it->col || nr != it->row) && !occupied(nc, nr, i)) {
			it->col = nc;
			it->row = nr;
			saved_set(it->key, nc, nr);
			moved = 1;
		}
	}
	if (moved)
		conf_save();
}

static int in_rect(fl_rect a, int x0, int y0, int x1, int y1)
{
	int ax1 = a.x + a.w, ay1 = a.y + a.h;
	int lx = x0 < x1 ? x0 : x1, hx = x0 < x1 ? x1 : x0, ly = y0 < y1 ? y0 : y1, hy = y0 < y1 ? y1 : y0;
	return a.x < hx && ax1 > lx && a.y < hy && ay1 > ly;
}

static void want_size(int *w, int *h);

static void desk_draw(fl_ui *ui, void *ud)
{
	(void)ud;
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, fl_r(0, 0, cv->w, cv->h), INPUT_BG);
	int mx = ui->mx, my = ui->my;
	int ctrl = (ui->mods & FL_MOD_CTRL) != 0;

	if (!D.show) {	/* hidden: only the menu (right click) is left */
		if (ui->press & FL_BTN_R)
			desk_menu(mx);
		return;
	}
	int hit = item_at(mx, my);

	if (ui->press & FL_BTN_R) {
		D.drag = -1;
		D.rb = 0;
		if (hit >= 0) {
			if (!D.it[hit].sel)
				select_only(hit);
			item_menu(&D.it[hit], mx);
		} else {
			desk_menu(mx);
		}
	}
	if (ui->press & FL_BTN_L) {
		if (hit >= 0 && ui->clicks >= 2 && !ctrl) {
			open_item(&D.it[hit]);
			D.drag = -1;
		} else if (hit >= 0) {
			if (ctrl)
				D.it[hit].sel = !D.it[hit].sel;
			else if (!D.it[hit].sel)
				select_only(hit);
			D.drag = hit;
			D.px = mx;
			D.py = my;
			D.dragging = 0;
		} else {
			if (!ctrl)
				select_only(-1);
			D.rb = 1;
			D.rbx = mx;
			D.rby = my;
			D.drag = -1;
		}
	}
	if (ui->btn_held & FL_BTN_L) {
		if (D.drag >= 0 && (abs(mx - D.px) > 4 || abs(my - D.py) > 4))
			D.dragging = 1;
		if (D.rb)
			for (int i = 0; i < D.n; i++)
				D.it[i].sel = D.it[i].placed && in_rect(cell_of(D.it[i].col, D.it[i].row), D.rbx, D.rby, mx, my);
	}
	if (ui->release & FL_BTN_L) {
		if (D.dragging)
			finish_drag(mx - D.px, my - D.py);
		D.drag = -1;
		D.dragging = 0;
		D.rb = 0;
	}

	int ddx = D.dragging ? mx - D.px : 0, ddy = D.dragging ? my - D.py : 0;
	for (int i = 0; i < D.n; i++) {
		struct item *it = &D.it[i];
		if (!it->placed)
			continue;
		int off_x = it->sel ? ddx : 0, off_y = it->sel ? ddy : 0;
		fl_rect cell = cell_of(it->col, it->row);
		cell.x += off_x;
		cell.y += off_y;
		fl_rect tile = fl_r(cell.x + (CELL_W - TILE) / 2, cell.y + 8, TILE, TILE);
		fl_rect lab = fl_r(cell.x + 2, tile.y + TILE + 6, CELL_W - 4, 20);
		int hot = i == hit;
		fl_fill_rect(cv, tile, hot ? fl_th.select : fl_th.raised);
		fl_stroke_rect(cv, tile, 1, it->sel ? fl_th.accent : fl_th.line);
		if (it->sel)
			fl_fill_rect(cv, fl_r(tile.x, tile.y, TILE, 3), fl_th.accent);
		fl_text_in(cv, D.f_glyph, tile, FL_CENTER, 0, fl_th.text, it->glyph);
		/* the name sits on a solid flat box, so it reads on any wallpaper */
		char fit[120];
		fl_text_fit(D.p->f_small, it->name, lab.w - 10, fit, sizeof fit);
		int tw = fl_text_width(D.p->f_small, fit, -1) + 10;
		fl_rect box = fl_r(lab.x + (lab.w - tw) / 2, lab.y, tw, lab.h);
		fl_fill_rect(cv, box, fl_th.panel);
		fl_stroke_rect(cv, box, 1, it->sel ? fl_th.accent : fl_th.line);
		fl_text_in(cv, D.p->f_small, box, FL_CENTER, 0, fl_th.text, fit);
	}
	if (D.rb) {
		fl_rect r = fl_r(D.rbx < mx ? D.rbx : mx, D.rby < my ? D.rby : my, abs(mx - D.rbx), abs(my - D.rby));
		fl_stroke_rect(cv, r, 1, fl_th.accent);
	}
	if (D.fit) {	/* the icons moved or a drag began/ended: the surface follows (not inside this pass) */
		int w, h;
		want_size(&w, &h);
		if (w != D.cw || h != D.ch)
			fl_timer_arm(D.fit, 20, 0);
	}
}

/* ---- set up ------------------------------------------------------------------------------------ */

static void apps_from_menu(struct panel *p)
{
	for (int i = 0; i < p->napps; i++) {
		struct entry *e = &p->apps[i];
		if (!e->desktop)
			continue;
		struct item *n = realloc(D.it, (size_t)(D.n + 1) * sizeof *D.it);
		if (!n)
			break;
		D.it = n;
		struct item *it = &D.it[D.n++];
		memset(it, 0, sizeof *it);
		it->kind = IT_APP;
		snprintf(it->name, sizeof it->name, "%s", e->name);
		snprintf(it->key, sizeof it->key, "app:%s", e->name);
		snprintf(it->exec, sizeof it->exec, "%s", e->exec);
		snprintf(it->glyph, sizeof it->glyph, "%s", e->glyph);
		it->terminal = e->terminal;
	}
}

/* The surface only has to cover the icons (and two cells of room around them): the compositor hands the
 * desktop layer the pointer all over the output where no window is, whatever the size of its buffer, so a
 * full-output buffer (8 MB at 1080p, twice with the compositor's copy) is not needed. During a drag or a
 * rubber band it is the whole output, so the icon can be carried anywhere. */
static void want_size(int *w, int *h)
{
	if (D.dragging || D.rb) {
		*w = D.ow;
		*h = D.oh;
		return;
	}
	int mc = 0, mr = 0;
	for (int i = 0; i < D.n; i++)
		if (D.it[i].placed) {
			if (D.it[i].col > mc)
				mc = D.it[i].col;
			if (D.it[i].row > mr)
				mr = D.it[i].row;
		}
	*w = grid_x() + (mc + 3) * CELL_W;
	*h = grid_y() + (mr + 3) * CELL_H;
	if (*w > D.ow)
		*w = D.ow;
	if (*h > D.oh)
		*h = D.oh;
}

static void desk_fit_surface(void)
{
	if (!D.w || !D.p->shell)
		return;
	int w, h;
	want_size(&w, &h);
	D.cw = w;
	D.ch = h;
	fl_win_resize(D.w, w, h);
	fl_shell_set_desktop(D.p->shell, fl_win_surface(D.w), 0, 0);
	fl_win_redraw(D.w);
}

static void fit_tick(void *ud)
{
	int w, h;
	want_size(&w, &h);
	if (w != D.cw || h != D.ch)
		desk_fit_surface();
}

void desk_reposition(struct panel *p)
{
	if (!D.w || !p->shell)
		return;
	int ow, oh;
	fl_app_output_size(p->app, &ow, &oh);
	if (ow != D.ow || oh != D.oh) {
		D.ow = ow;
		D.oh = oh;
		desk_fit_surface();
	}
	grid_size();
	for (int i = 0; i < D.n; i++)
		if (D.it[i].placed && (D.it[i].row >= D.maxrows))
			D.it[i].row = D.maxrows - 1;
	fl_win_redraw(D.w);
}

void desk_init(struct panel *p)
{
	if (!p->shell || p->shell_ver < 6 || p->demo || fl_app_ppm(p->app))
		return;
	menu_init(p);	/* loads the .desktop entries (it reloads them every time the menu opens) */
	D.p = p;
	D.drag = -1;
	const char *home = getenv("HOME") ? getenv("HOME") : "";
	snprintf(D.dir, sizeof D.dir, "%s/Desktop", home);
	const char *xc = getenv("XDG_CONFIG_HOME");
	if (xc && *xc)
		snprintf(D.conf, sizeof D.conf, "%s/freelfs/desktop-icons.conf", xc);
	else
		snprintf(D.conf, sizeof D.conf, "%s/.config/freelfs/desktop-icons.conf", home);
	D.show = 1;
	conf_load();
	apps_from_menu(p);
	/* the menu list is loaded again when the menu opens: do not keep it (about 100 KB of RSS) */
	free(p->apps);
	free(p->shown);
	p->apps = NULL;
	p->shown = NULL;
	p->napps = p->nshown = 0;
	mkdirs(D.dir);
	fl_app_output_size(p->app, &D.ow, &D.oh);
	grid_size();
	D.f_glyph = fl_font_mono(500, 26);
	scan_files();
	place_unplaced();
	want_size(&D.cw, &D.ch);
	fl_win_opts o = {
		.title = "desktop", .app_id = "flpanel-desktop", .w = D.cw, .h = D.ch, .raw = 1,
		.draw = desk_draw, .ud = NULL,
	};
	D.w = fl_win_new(p->app, &o);
	if (!D.w) {
		D.p = NULL;
		return;
	}
	D.fit = fl_timer_new(p->app, fit_tick, NULL);
	D.scan = fl_timer_new(p->app, scan_tick, NULL);
	fl_timer_arm(D.scan, 4000, 4000);
	fl_shell_set_desktop(p->shell, fl_win_surface(D.w), 0, 0);
	fl_win_map(D.w);
}
