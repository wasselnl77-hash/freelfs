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
#include <ctype.h>
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
	ID_SIDE = 100, ID_DRIVE = 200, ID_EJECT = 300, ID_CELL = 1000, ID_MENU = 0x40000, ID_DLG = 0x41000,
	ID_JOB = 0x42000, ID_OW = 0x43000,
};

#define DRV_MAX 24 /* drives shown in the sidebar */

enum { TOOL_H = 44, STATUS_H = 28, SIDE_W = 188, CELL_W = 108, CELL_H = 98, ICON = 40 };

enum { A_NONE, A_OPEN, A_NAV, A_BACK, A_FWD, A_UP, A_NEWDIR, A_RENAME, A_TRASH, A_COPY, A_CUT,
       A_PASTE, A_HIDDEN, A_REFRESH, A_SELALL, A_DO_MKDIR, A_DO_RENAME, A_DO_DELETE, A_FOCUS_PATH, A_EXTRACT, A_COMPRESS,
       A_NEWFILE, A_DO_NEWFILE, A_PERMDEL, A_PROPS, A_OPENWITH, A_TERMINAL, A_RESTORE, A_EMPTY, A_DO_EMPTY,
       A_DROP, A_JOB_START, A_MOUNT, A_EJECT };

enum { OV_NONE, OV_MENU, OV_PROMPT, OV_CONFIRM, OV_CLASH, OV_OPENWITH, OV_PROPS, OV_MSG };

struct entry {
	char *name;
	off_t size;
	time_t mtime;
	unsigned char dir, link;
};

struct place {
	char label[40], path[PATH_MAX];
	fl_icon ic;
};

/* a removable or boot partition from flmount, or a mount under /drives or /run/media; System is first */
struct drive {
	char dev[64], label[64], fstype[24], path[PATH_MAX];
	unsigned long long size;
	int mounted, removable, system;
};

/* a file job's item: paths are heap strings; action is set by the clash dialog */
struct jitem {
	char *src, *dst, *info; /* info: the .trashinfo to remove with it, or NULL */
	char name[NAME_MAX + 1];
	int action, clash;
};

struct job {
	int op, n;
	struct jitem *it;
	pid_t pid;
	int rfd;
	char buf[1024];
	size_t blen;
	long long total, done;
	int ok, bad, cancelled, cancel_req, clear_clip;
	char cur[NAME_MAX + 1], err[200], title[96];
};

struct app_choice {
	char desk[PATH_MAX], name[96];
};

struct F;

/* one flmount call: stdout and stderr in buf[0] and buf[1], exit status from the status pipe */
struct hlp {
	struct F *f;
	int kind; /* HK_* */
	pid_t pid;
	int fd[3], open_n;
	char *buf[2];
	size_t len[2], cap[2];
	int status;
	char label[64], dev[64];
};

enum { HK_LIST, HK_MOUNT, HK_EJECT };

struct menu_item {
	const char *label, *hint;
	int act, enabled, sep;
};

struct F {
	fl_app *app;
	fl_win *win;
	fl_font *small, *head;
	fl_timer *msg_timer, *tick;
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
	char trash_files[PATH_MAX], trash_base[PATH_MAX];

	/* drives: the list from flmount, and the last one built (kept to see a change) */
	struct drive drv[DRV_MAX], drv2[DRV_MAX];
	int ndrv, ndrv2, flm_ok;
	struct hlp *lst, *opr;
	int mi_fd;

	int ino_fd, ino_wd;

	char msg[300];
	int msg_err;

	char type_buf[64];
	long long type_ms;

	/* queued action */
	int act, act_idx;
	char act_path[PATH_MAX];
	int ctx_item, dr_copy;

	/* drag and drop inside the window: 1 pressed, 2 dragging */
	int dr_state, dr_x, dr_y;
	char dr_target[PATH_MAX];
	fl_rect dr_rect;

	/* copy, move, trash, delete and restore: the running job, and the one waiting for clash answers */
	struct job *job;
	struct jitem *pend;
	int npend, pend_op, pend_clip, clash_idx, clash_all;

	/* overlays */
	int ov;
	struct menu_item menu[20];
	int nmenu, menu_x, menu_y, menu_sel;
	int prompt_act;
	char prompt_title[96], prompt_text[NAME_MAX + 1], prompt_err[160];
	char confirm_title[64], confirm_text[200], confirm_btn[16];
	int confirm_act;
	int ov_new;
	char dlg_title[96], dlg_text[600];
	struct app_choice ac[12];
	int nac;
	char ac_file[PATH_MAX], ac_mime[128];
	char pr[6][PATH_MAX + 64];
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

/* "base", then "base 2", "base 3" ... the first name free in dir */
static void next_free(const char *dir, const char *base, char *out, size_t cap)
{
	char p[PATH_MAX];
	for (int i = 1; i < 10000; i++) {
		if (i == 1)
			snprintf(out, cap, "%s", base);
		else
			snprintf(out, cap, "%s %d", base, i);
		join(p, sizeof p, dir, out);
		if (!path_exists(p))
			return;
	}
	snprintf(out, cap, "%s", base);
}

/* %XX escapes (trashinfo Path=) back to bytes */
static void pct_decode(const char *s, char *out, size_t cap)
{
	size_t o = 0;
	for (; *s && o + 1 < cap; s++) {
		if (s[0] == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
			char hx[3] = {s[1], s[2], 0};
			out[o++] = (char)strtol(hx, NULL, 16);
			s += 2;
		} else {
			out[o++] = *s;
		}
	}
	out[o] = 0;
}

/* \040 style escapes of /proc/mounts back to bytes */
static void mount_unescape(const char *in, char *out, size_t cap)
{
	size_t o = 0;
	for (size_t i = 0; in[i] && o + 1 < cap; i++) {
		if (in[i] == '\\' && in[i + 1] >= '0' && in[i + 1] <= '3' && in[i + 2] >= '0' && in[i + 2] <= '7' &&
		    in[i + 3] >= '0' && in[i + 3] <= '7') {
			out[o++] = (char)(((in[i + 1] - '0') << 6) | ((in[i + 2] - '0') << 3) | (in[i + 3] - '0'));
			i += 3;
		} else {
			out[o++] = in[i];
		}
	}
	out[o] = 0;
}

/* ---- jobs: the child side. The child reads job_cancel and sends progress lines on job_wfd. */

static volatile sig_atomic_t job_cancel;
static int job_wfd = -1;
static long long job_bytes, job_reported;

static void job_on_usr1(int sig)
{
	(void)sig;
	job_cancel = 1;
}

/* one line to the window: "T total", "I name", "P done", "E message", "D ok bad cancelled" */
static void job_send(const char *fmt, ...)
{
	char line[600];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	if (job_wfd < 0 || n <= 0)
		return;
	if (n >= (int)sizeof line)
		n = (int)sizeof line - 1;
	for (int off = 0; off < n;) {
		ssize_t w = write(job_wfd, line + off, (size_t)(n - off));
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return;
		off += (int)w;
	}
}

static void job_progress(size_t n)
{
	job_bytes += (long long)n;
	if (job_bytes - job_reported >= 256 * 1024) {
		job_reported = job_bytes;
		job_send("P %lld\n", job_bytes);
	}
}

/* ---- file operations ----------------------------------------------------------------------- */

static int remove_tree(const char *path)
{
	struct stat st;
	if (job_cancel) {
		errno = EINTR;
		return -1;
	}
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
	if (job_cancel) {
		errno = EINTR;
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
		if (job_cancel) {
			rc = -1;
			errno = EINTR;
			break;
		}
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
			job_progress((size_t)w);
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
static int trash_path(const char *path)
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

static void place_add(struct F *f, const char *label, const char *path, fl_icon ic)
{
	struct place *np = realloc(f->places, (size_t)(f->nplaces + 1) * sizeof *np);
	if (!np)
		return;
	f->places = np;
	struct place *p = &np[f->nplaces++];
	snprintf(p->label, sizeof p->label, "%s", label);
	snprintf(p->path, sizeof p->path, "%s", path);
	p->ic = ic;
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

/* the PLACES list; drives are drives_rebuild's */
static void places_load(struct F *f)
{
	f->nplaces = 0;
	const char *home = getenv("HOME");
	if (home && *home && is_dir_path(home))
		place_add(f, "Home", home, FL_ICON_HOME);
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
		place_add(f, ud[i].label, p, FL_ICON_FOLDER);
	}
	if (f->trash_files[0])
		place_add(f, "Trash", f->trash_files, FL_ICON_TRASH);
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

/* start the app of a desktop file on one file; 0 if the file has no usable Exec= */
static int spawn_desktop(struct F *f, const char *desktop, const char *path, const char *name)
{
	char *argv[32] = {0};
	if (exec_args(desktop, path, argv, 32) < 0)
		return 0;
	char prog[256];
	snprintf(prog, sizeof prog, "%s", argv[0]);
	/* a pipe with close-on-exec: it stays empty when exec works, and carries errno when it fails */
	int ep[2];
	if (pipe2(ep, O_CLOEXEC) < 0) {
		for (int i = 0; argv[i]; i++)
			free(argv[i]);
		say(f, 1, "Cannot start %s: %s", prog, strerror(errno));
		return 1;
	}
	pid_t pid = fork();
	if (pid == 0) {
		close(ep[0]);
		setsid();
		signal(SIGCHLD, SIG_DFL);
		/* the app gets no descriptors of Files (sockets, job pipes, mount info) */
		for (int fd = 3; fd < 1024; fd++)
			if (fd != ep[1])
				close(fd);
		execvp(argv[0], argv);
		int e = errno;
		ssize_t wr = write(ep[1], &e, sizeof e);
		(void)wr;
		_exit(127);
	}
	close(ep[1]);
	for (int i = 0; argv[i]; i++)
		free(argv[i]);
	if (pid < 0) {
		close(ep[0]);
		say(f, 1, "Cannot start %s: %s", prog, strerror(errno));
		return 1;
	}
	int e = 0;
	ssize_t r;
	do
		r = read(ep[0], &e, sizeof e);
	while (r < 0 && errno == EINTR);
	close(ep[0]);
	if (r == (ssize_t)sizeof e)
		say(f, 1, "Cannot start %s for %s: %s", prog, name, strerror(e));
	return 1;
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
	return spawn_desktop(f, dt, path, name);
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

/* ---- jobs: copy, move, trash, delete and restore ----------------------------------------------------------
 * A job runs in a forked child that does the file work. The child reports on a pipe ("T total", "I name",
 * "P done", "E message", "D ok bad cancelled") and the window reads that pipe from its loop, so a big copy
 * never holds the screen. Cancel sends SIGUSR1: the child stops after the file it is on. SIGCHLD stays
 * ignored in the window, so a job ends at the pipe's EOF; nothing is waited for. Name clashes are asked
 * for in the window before the child starts (Replace, Keep both, Skip). */

enum { JOB_COPY, JOB_MOVE, JOB_TRASH, JOB_DELETE, JOB_RESTORE };
enum { JA_PROCEED, JA_REPLACE, JA_SKIP };

static void job_clean(char *out, const char *in)
{
	size_t n = 0;
	for (; in[0] && n < NAME_MAX; in++, n++)
		out[n] = (in[0] == '\n' || in[0] == '\r') ? ' ' : in[0];
	out[n] = 0;
}

static void mkdir_parent(const char *path)
{
	char t[PATH_MAX];
	snprintf(t, sizeof t, "%s", path);
	char *s = strrchr(t, '/');
	if (s && s != t) {
		*s = 0;
		mkdir_p(t);
	}
}

/* the bytes under path: what copying or deleting it moves */
static long long tree_bytes(const char *path, int depth)
{
	struct stat st;
	if (job_cancel || depth > 64 || lstat(path, &st) < 0)
		return 0;
	if (!S_ISDIR(st.st_mode))
		return S_ISREG(st.st_mode) ? (long long)st.st_size : 0;
	DIR *d = opendir(path);
	if (!d)
		return 0;
	long long t = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char sub[PATH_MAX];
		join(sub, sizeof sub, path, e->d_name);
		t += tree_bytes(sub, depth + 1);
	}
	closedir(d);
	return t;
}

/* the child: never returns */
static void job_child(struct job *j, int wfd)
{
	job_wfd = wfd;
	job_bytes = job_reported = 0;
	signal(SIGUSR1, job_on_usr1);
	signal(SIGCHLD, SIG_DFL);
	signal(SIGPIPE, SIG_DFL);
	long long *sz = calloc((size_t)j->n + 1, sizeof *sz);
	if (!sz)
		_exit(1);
	long long total = 0;
	for (int i = 0; i < j->n; i++)
		if (j->it[i].action != JA_SKIP) {
			sz[i] = tree_bytes(j->it[i].src, 0);
			total += sz[i];
		}
	job_send("T %lld\n", total);
	int ok = 0, bad = 0;
	long long acc = 0;
	for (int i = 0; i < j->n && !job_cancel; i++) {
		struct jitem *it = &j->it[i];
		if (it->action == JA_SKIP)
			continue;
		char nm[NAME_MAX + 1];
		job_clean(nm, it->name);
		job_send("I %s\n", nm);
		job_bytes = acc;
		int rc = 0;
		switch (j->op) {
		case JOB_COPY:
		case JOB_MOVE:
			if (it->action == JA_REPLACE)
				remove_tree(it->dst);
			if (path_exists(it->dst)) {
				errno = EEXIST;
				rc = -1;
			} else {
				rc = j->op == JOB_COPY ? copy_tree(it->src, it->dst, 0) : move_path(it->src, it->dst);
				if (rc == 0 && j->op == JOB_MOVE && it->info)
					unlink(it->info);
			}
			break;
		case JOB_TRASH:
			rc = trash_path(it->src);
			break;
		case JOB_DELETE:
			rc = remove_tree(it->src);
			if (rc == 0 && it->info)
				unlink(it->info);
			break;
		case JOB_RESTORE:
			if (it->action == JA_REPLACE)
				remove_tree(it->dst);
			if (path_exists(it->dst)) {
				errno = EEXIST;
				rc = -1;
			} else {
				mkdir_parent(it->dst);
				rc = move_path(it->src, it->dst);
				if (rc == 0 && it->info)
					unlink(it->info);
			}
			break;
		}
		if (rc < 0 && job_cancel)
			break;
		if (rc < 0) {
			bad++;
			job_send("E %s: %s\n", nm, strerror(errno));
		} else {
			ok++;
		}
		acc += sz[i];
		job_bytes = acc;
		job_reported = acc;
		job_send("P %lld\n", acc);
	}
	job_send("D %d %d %d\n", ok, bad, job_cancel ? 1 : 0);
	_exit(0);
}

static void job_free_items(struct jitem *it, int n)
{
	for (int i = 0; it && i < n; i++) {
		free(it[i].src);
		free(it[i].dst);
		free(it[i].info);
	}
	free(it);
}

static void job_free(struct job *j)
{
	job_free_items(j->it, j->n);
	free(j);
}

static const char *job_verb(int op)
{
	switch (op) {
	case JOB_COPY: return "Copied";
	case JOB_MOVE: return "Moved";
	case JOB_TRASH: return "Trashed";
	case JOB_RESTORE: return "Restored";
	default: return "Deleted";
	}
}

static void job_line(struct job *j, const char *ln)
{
	const char *arg = ln[0] && ln[1] ? ln + 2 : "";
	switch (ln[0]) {
	case 'T': j->total = atoll(ln + 1); break;
	case 'I': snprintf(j->cur, sizeof j->cur, "%s", arg); break;
	case 'P': j->done = atoll(ln + 1); break;
	case 'E': snprintf(j->err, sizeof j->err, "%s", arg); break;
	case 'D': sscanf(ln + 1, "%d %d %d", &j->ok, &j->bad, &j->cancelled); break;
	}
}

static void job_end(struct F *f)
{
	struct job *j = f->job;
	f->job = NULL;
	fl_fd_remove(f->app, j->rfd);
	close(j->rfd);
	int stopped = j->cancelled || j->cancel_req;
	if (stopped)
		say(f, 0, "Stopped. %d item%s done", j->ok, j->ok == 1 ? "" : "s");
	else if (j->bad)
		say(f, 1, "%d item%s failed: %s", j->bad, j->bad == 1 ? "" : "s", j->err[0] ? j->err : "error");
	else
		say(f, 0, "%s %d item%s", job_verb(j->op), j->ok, j->ok == 1 ? "" : "s");
	if (j->clear_clip && !stopped && !j->bad)
		fl_clipboard_set(f->app, "", 0);
	refresh(f);
	job_free(j);
	fl_win_redraw(f->win);
}

static void job_ready(void *ud, int fd, short rev)
{
	struct F *f = ud;
	struct job *j = f->job;
	(void)rev;
	if (!j || fd != j->rfd)
		return;
	for (;;) {
		ssize_t n = read(fd, j->buf + j->blen, sizeof j->buf - 1 - j->blen);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
			break;
		if (n <= 0) {
			job_end(f);
			return;
		}
		j->blen += (size_t)n;
		char *s = j->buf, *nl;
		while ((nl = memchr(s, '\n', j->blen - (size_t)(s - j->buf)))) {
			*nl = 0;
			job_line(j, s);
			s = nl + 1;
		}
		size_t used = (size_t)(s - j->buf);
		memmove(j->buf, s, j->blen - used);
		j->blen -= used;
		if (j->blen == sizeof j->buf - 1)
			j->blen = 0; /* a line longer than the buffer: dropped */
	}
	fl_win_redraw(f->win);
}

static void job_start(struct F *f)
{
	struct jitem *it = f->pend;
	int n = f->npend, op = f->pend_op;
	f->pend = NULL;
	f->npend = 0;
	if (f->job) {
		job_free_items(it, n);
		say(f, 1, "Wait for the current job to finish");
		return;
	}
	struct job *j = calloc(1, sizeof *j);
	int fds[2];
	if (!j || pipe2(fds, O_CLOEXEC) < 0) {
		free(j);
		job_free_items(it, n);
		say(f, 1, "Cannot start: %s", strerror(errno));
		return;
	}
	j->op = op;
	j->n = n;
	j->it = it;
	j->rfd = fds[0];
	j->clear_clip = f->pend_clip;
	const char *what = op == JOB_COPY ? "Copying" : op == JOB_MOVE ? "Moving" : op == JOB_TRASH ? "Moving to the Trash"
	                   : op == JOB_RESTORE ? "Restoring" : "Deleting";
	snprintf(j->title, sizeof j->title, "%s %d item%s", what, n, n == 1 ? "" : "s");
	j->pid = fork();
	if (j->pid == 0) {
		close(fds[0]);
		job_child(j, fds[1]);
	}
	close(fds[1]);
	if (j->pid < 0) {
		close(fds[0]);
		say(f, 1, "Cannot start: %s", strerror(errno));
		job_free(j);
		return;
	}
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	fl_fd_add(f->app, fds[0], POLLIN, job_ready, f);
	f->job = j;
	fl_win_redraw(f->win);
}

/* the window's Stop button */
static void job_stop(struct F *f)
{
	if (f->job && !f->job->cancel_req) {
		f->job->cancel_req = 1;
		kill(f->job->pid, SIGUSR1);
	}
	fl_win_redraw(f->win);
}

/* a free name next to the item's destination: "name copy", "name copy 2" ... */
static void keep_both(struct jitem *x)
{
	char dir[PATH_MAX], nn[NAME_MAX + 1], p[PATH_MAX];
	snprintf(dir, sizeof dir, "%s", x->dst);
	char *s = strrchr(dir, '/');
	if (s == dir)
		dir[1] = 0;
	else if (s)
		*s = 0;
	struct stat st;
	int isdir = lstat(x->src, &st) == 0 && S_ISDIR(st.st_mode);
	unique_name(dir, base_name(x->dst), isdir, nn, sizeof nn);
	join(p, sizeof p, dir, nn);
	char *np = strdup(p);
	if (np) {
		free(x->dst);
		x->dst = np;
	}
	x->action = JA_PROCEED;
}

static int inside_dir(const char *dir, const char *src)
{
	size_t l = strlen(src);
	return !strncmp(dir, src, l) && (dir[l] == '/' || dir[l] == 0);
}

/* Queue a job. Takes it[] over: freed here, or by the job. dir is the destination folder, or NULL. */
static void request_job(struct F *f, int op, const char *dir, struct jitem *it, int n, int clear_clip)
{
	if (f->job || f->npend) {
		job_free_items(it, n);
		say(f, 1, "Wait for the current job to finish");
		return;
	}
	int first = -1;
	for (int i = 0; i < n; i++) {
		struct jitem *x = &it[i];
		struct stat st;
		int isdir = lstat(x->src, &st) == 0 && S_ISDIR(st.st_mode);
		if ((op == JOB_COPY || op == JOB_MOVE) && dir && isdir && inside_dir(dir, x->src)) {
			say(f, 1, "Cannot put a folder inside itself");
			job_free_items(it, n);
			return;
		}
		if (op == JOB_MOVE && x->dst && !strcmp(x->src, x->dst))
			x->action = JA_SKIP;
		else if (op == JOB_COPY && x->dst && !strcmp(x->src, x->dst))
			keep_both(x);
		if (x->action != JA_SKIP && x->dst && (op == JOB_COPY || op == JOB_MOVE || op == JOB_RESTORE) &&
		    path_exists(x->dst)) {
			x->clash = 1;
			if (first < 0)
				first = i;
		}
	}
	f->pend = it;
	f->npend = n;
	f->pend_op = op;
	f->pend_clip = clear_clip;
	if (first >= 0) {
		f->clash_idx = first;
		f->clash_all = 0;
		f->ov = OV_CLASH;
		fl_win_redraw(f->win);
	} else {
		queue(f, A_JOB_START);
	}
}

/* the answer to the name clash shown: act for it, and for the rest when "same for the rest" is on */
static void clash_answer(struct F *f, int act)
{
	for (int i = f->clash_idx; i < f->npend; i++) {
		struct jitem *x = &f->pend[i];
		if (!x->clash || (i != f->clash_idx && !f->clash_all))
			continue;
		x->clash = 0;
		if (act == JA_PROCEED)
			keep_both(x);
		else
			x->action = act;
	}
	for (int i = 0; i < f->npend; i++)
		if (f->pend[i].clash) {
			f->clash_idx = i;
			fl_win_redraw(f->win);
			return;
		}
	f->ov = OV_NONE;
	queue(f, A_JOB_START);
}

static void clash_cancel(struct F *f)
{
	job_free_items(f->pend, f->npend);
	f->pend = NULL;
	f->npend = 0;
	f->ov = OV_NONE;
	say(f, 0, "Cancelled. Nothing was changed.");
}

/* the selected entries as jitems; dir (or NULL) gives each one's destination */
static struct jitem *sel_items(struct F *f, const char *dir, int *n)
{
	struct jitem *it = calloc((size_t)count_sel(f) + 1, sizeof *it);
	*n = 0;
	for (int i = 0; it && i < f->n; i++) {
		if (!f->sel[i])
			continue;
		struct jitem *x = &it[*n];
		char p[PATH_MAX];
		join(p, sizeof p, f->cwd, f->ents[i].name);
		x->src = strdup(p);
		if (dir) {
			join(p, sizeof p, dir, f->ents[i].name);
			x->dst = strdup(p);
		}
		if (in_trash(f)) {
			snprintf(p, sizeof p, "%s/info/%s.trashinfo", f->trash_base, f->ents[i].name);
			x->info = strdup(p);
		}
		snprintf(x->name, sizeof x->name, "%s", f->ents[i].name);
		(*n)++;
	}
	return it;
}

/* the selected trash entries, back to where their .trashinfo says they came from */
static struct jitem *restore_items(struct F *f, int *n)
{
	struct jitem *it = sel_items(f, NULL, n);
	for (int i = 0; it && i < *n; i++) {
		struct jitem *x = &it[i];
		char ip[PATH_MAX], line[PATH_MAX + 64], dst[PATH_MAX] = "";
		snprintf(ip, sizeof ip, "%s/info/%s.trashinfo", f->trash_base, x->name);
		FILE *fp = fopen(ip, "r");
		if (fp) {
			while (fgets(line, sizeof line, fp))
				if (!strncmp(line, "Path=", 5)) {
					line[strcspn(line, "\r\n")] = 0;
					pct_decode(line + 5, dst, sizeof dst);
					break;
				}
			fclose(fp);
		}
		if (dst[0] != '/') {
			const char *home = getenv("HOME");
			snprintf(dst, sizeof dst, "%s/%s", home ? home : "/", x->name);
		}
		x->dst = strdup(dst);
	}
	return it;
}

/* every entry of the Trash, for Empty Trash */
static struct jitem *trash_all_items(struct F *f, int *n)
{
	*n = 0;
	DIR *d = opendir(f->trash_files);
	if (!d)
		return NULL;
	int cnt = 0;
	while (readdir(d))
		cnt++;
	rewinddir(d);
	struct jitem *it = calloc((size_t)cnt + 1, sizeof *it);
	struct dirent *e;
	while (it && (e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char p[PATH_MAX], ip[PATH_MAX];
		join(p, sizeof p, f->trash_files, e->d_name);
		snprintf(ip, sizeof ip, "%s/info/%s.trashinfo", f->trash_base, e->d_name);
		it[*n].src = strdup(p);
		it[*n].info = strdup(ip);
		snprintf(it[*n].name, sizeof it[*n].name, "%s", e->d_name);
		(*n)++;
	}
	closedir(d);
	return it;
}

/* Delete permanently: asks first, then A_DO_DELETE */
static void ask_permanent(struct F *f)
{
	int c = count_sel(f);
	if (!c)
		return;
	if (!egg_ok_selection(f)) {
		say(f, 0, "Left it where it is");
		return;
	}
	f->ov = OV_CONFIRM;
	snprintf(f->confirm_title, sizeof f->confirm_title, "Delete permanently");
	if (c == 1 && f->cur >= 0 && f->sel[f->cur])
		snprintf(f->confirm_text, sizeof f->confirm_text, "Delete \"%s\" for good? This cannot be undone.",
		         f->ents[f->cur].name);
	else
		snprintf(f->confirm_text, sizeof f->confirm_text, "Delete %d items for good? This cannot be undone.", c);
	snprintf(f->confirm_btn, sizeof f->confirm_btn, "Delete");
	f->confirm_act = A_DO_DELETE;
}

/* a drop on a folder (or the Trash) in the window: move, or copy with Ctrl */
static void drop_selection(struct F *f, const char *dir, int copy)
{
	int n = 0;
	if (!strcmp(dir, f->trash_files)) {
		if (!egg_ok_selection(f)) {
			say(f, 0, "Left it where it is");
			return;
		}
		struct jitem *it = sel_items(f, NULL, &n);
		if (!n) {
			free(it);
			return;
		}
		request_job(f, JOB_TRASH, NULL, it, n, 0);
		return;
	}
	struct jitem *it = sel_items(f, dir, &n);
	if (!n) {
		free(it);
		return;
	}
	request_job(f, copy ? JOB_COPY : JOB_MOVE, dir, it, n, 0);
}

/* ---- drives: flmount does the mounting ---------------------------------------------------------------------
 * flmount list | mount DEV | eject DEV (its CLI is in the README). Each call runs as a child whose stdout,
 * stderr and exit status come back on three pipes that the window's loop reads, so nothing waits. The list
 * is asked for on start, on a change of /proc/self/mountinfo (POLLPRI) and every 3 s while the window is
 * open. If flmount is missing, the removable rows stay hidden and only the timer and mount changes are
 * skipped. */

static void hlp_add(struct hlp *h, int i, const char *s, size_t n)
{
	if (h->len[i] + n + 1 > 65536)
		return;
	if (h->len[i] + n + 1 > h->cap[i]) {
		size_t nc = (h->len[i] + n + 1) * 2 + 256;
		char *nb = realloc(h->buf[i], nc);
		if (!nb)
			return;
		h->buf[i] = nb;
		h->cap[i] = nc;
	}
	memcpy(h->buf[i] + h->len[i], s, n);
	h->len[i] += n;
	h->buf[i][h->len[i]] = 0;
}

static void hlp_free(struct hlp *h)
{
	free(h->buf[0]);
	free(h->buf[1]);
	free(h);
}

static void show_msg(struct F *f, const char *title, const char *text)
{
	snprintf(f->dlg_title, sizeof f->dlg_title, "%s", title);
	snprintf(f->dlg_text, sizeof f->dlg_text, "%s", text);
	f->ov = OV_MSG;
	fl_win_redraw(f->win);
}

/* split tab fields in place; returns the count (at most max) */
static int split_tabs(char *line, char **fld, int max)
{
	int n = 0;
	char *t = line;
	for (;;) {
		fld[n++] = t;
		char *tab = strchr(t, '\t');
		if (!tab || n == max)
			break;
		*tab = 0;
		t = tab + 1;
	}
	return n;
}

/* the drive list from flmount list (NULL: flmount is not there), plus the mounts under /drives and /run/media */
static void drives_rebuild(struct F *f, const char *list)
{
	memset(f->drv2, 0, sizeof f->drv2);
	int n = 0;
	struct drive *s = &f->drv2[n++];
	s->system = 1;
	s->mounted = 1;
	snprintf(s->label, sizeof s->label, "System");
	snprintf(s->path, sizeof s->path, "%s", is_dir_path("/sysflfs") ? "/sysflfs" : "/");
	if (list) {
		char *copy = strdup(list);
		char *p = copy;
		while (copy && p && *p && n < DRV_MAX) {
			char *e = strchr(p, '\n');
			char *next = e ? e + 1 : NULL;
			if (e)
				*e = 0;
			size_t l = strlen(p);
			if (l && p[l - 1] == '\r')
				p[l - 1] = 0;
			char *fld[6] = {0};
			int nf = split_tabs(p, fld, 6);
			if (nf == 6 && fld[0][0] && (!strcmp(fld[5], "removable") || !strcmp(fld[5], "boot"))) {
				struct drive *d = &f->drv2[n++];
				snprintf(d->dev, sizeof d->dev, "%s", fld[0]);
				snprintf(d->label, sizeof d->label, "%s", fld[1][0] ? fld[1] : base_name(fld[0]));
				snprintf(d->fstype, sizeof d->fstype, "%s", fld[2]);
				d->size = strtoull(fld[3], NULL, 10);
				d->mounted = strcmp(fld[4], "-") != 0;
				if (d->mounted)
					snprintf(d->path, sizeof d->path, "%s", fld[4]);
				d->removable = 1;
			}
			p = next;
		}
		free(copy);
	}
	FILE *fp = fopen("/proc/mounts", "r");
	if (fp) {
		char line[1024];
		while (n < DRV_MAX && fgets(line, sizeof line, fp)) {
			char dev[256], mp[PATH_MAX], dec[PATH_MAX];
			if (sscanf(line, "%255s %4095s", dev, mp) != 2)
				continue;
			mount_unescape(mp, dec, sizeof dec);
			int under = (!strncmp(dec, "/drives/", 8) && dec[8]) || (!strncmp(dec, "/run/media/", 11) && dec[11]);
			if (!under)
				continue;
			int seen = 0;
			for (int i = 0; i < n; i++)
				if (!strcmp(f->drv2[i].path, dec))
					seen = 1;
			if (seen)
				continue;
			struct drive *d = &f->drv2[n++];
			snprintf(d->dev, sizeof d->dev, "%s", dev);
			snprintf(d->label, sizeof d->label, "%s", base_name(dec));
			snprintf(d->path, sizeof d->path, "%s", dec);
			d->mounted = 1;
			d->removable = 1;
		}
		fclose(fp);
	}
	f->ndrv2 = n;
	if (f->ndrv != f->ndrv2 || memcmp(f->drv, f->drv2, sizeof f->drv)) {
		memcpy(f->drv, f->drv2, sizeof f->drv);
		f->ndrv = f->ndrv2;
		fl_win_redraw(f->win);
	}
}

static void drive_refresh(struct F *f);

static void hlp_done(struct hlp *h)
{
	struct F *f = h->f;
	char out[65536], err[4096];
	snprintf(out, sizeof out, "%s", h->buf[0] ? h->buf[0] : "");
	snprintf(err, sizeof err, "%s", h->buf[1] ? h->buf[1] : "");
	int ok = h->status == 0;
	switch (h->kind) {
	case HK_LIST:
		if (f->lst == h)
			f->lst = NULL;
		f->flm_ok = ok;
		drives_rebuild(f, ok ? out : NULL);
		break;
	case HK_MOUNT: {
		if (f->opr == h)
			f->opr = NULL;
		if (ok) {
			char mp[PATH_MAX];
			snprintf(mp, sizeof mp, "%s", out);
			mp[strcspn(mp, "\r\n")] = 0;
			drive_refresh(f);
			if (mp[0] == '/')
				load_dir(f, mp, 1);
		} else {
			char text[700];
			snprintf(text, sizeof text, "%s", err[0] ? err : "flmount did not finish.");
			char title[120];
			snprintf(title, sizeof title, "Could not mount %s", h->label);
			show_msg(f, title, text);
		}
		break;
	}
	case HK_EJECT: {
		if (f->opr == h)
			f->opr = NULL;
		if (ok) {
			say(f, 0, "Ejected %s. It is safe to remove.", h->label);
		} else {
			char text[700];
			snprintf(text, sizeof text, "%s", err[0] ? err : "flmount did not finish.");
			char title[120];
			snprintf(title, sizeof title, "Could not eject %s", h->label);
			show_msg(f, title, text);
		}
		drive_refresh(f);
		break;
	}
	}
	hlp_free(h);
}

static void hlp_ready(void *ud, int fd, short rev)
{
	struct hlp *h = ud;
	int i = fd == h->fd[0] ? 0 : fd == h->fd[1] ? 1 : 2;
	char b[1024];
	(void)rev;
	for (;;) {
		ssize_t n = read(fd, b, sizeof b);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno == EAGAIN)
			return;
		if (n > 0) {
			if (i == 2) {
				char t[16];
				size_t m = (size_t)n < sizeof t - 1 ? (size_t)n : sizeof t - 1;
				memcpy(t, b, m);
				t[m] = 0;
				h->status = atoi(t);
			} else {
				hlp_add(h, i, b, (size_t)n);
			}
			continue;
		}
		fl_fd_remove(h->f->app, fd);
		close(fd);
		h->fd[i] = -1;
		if (--h->open_n == 0)
			hlp_done(h);
		return;
	}
}

/* run flmount VERB [DEV] in a child; NULL when that fails */
static struct hlp *hlp_spawn(struct F *f, int kind, const char *verb, const char *dev)
{
	struct hlp *h = calloc(1, sizeof *h);
	if (!h)
		return NULL;
	h->f = f;
	h->kind = kind;
	h->status = -1;
	int p[3][2] = {{-1, -1}, {-1, -1}, {-1, -1}};
	for (int i = 0; i < 3; i++)
		if (pipe2(p[i], O_CLOEXEC) < 0)
			goto fail;
	const char *script = "flmount \"$@\"; printf '%d' \"$?\" >&3";
	pid_t pid = fork();
	if (pid < 0)
		goto fail;
	if (pid == 0) {
		int dn = open("/dev/null", O_RDONLY);
		if (dn >= 0)
			dup2(dn, 0);
		dup2(p[0][1], 1);
		dup2(p[1][1], 2);
		dup2(p[2][1], 3);
		fcntl(3, F_SETFD, 0);
		signal(SIGCHLD, SIG_DFL);
		if (dev)
			execl("/bin/sh", "sh", "-c", script, "flmount", verb, dev, (char *)NULL);
		else
			execl("/bin/sh", "sh", "-c", script, "flmount", verb, (char *)NULL);
		_exit(127);
	}
	h->pid = pid;
	for (int i = 0; i < 3; i++) {
		close(p[i][1]);
		p[i][1] = -1;
		h->fd[i] = p[i][0];
		fcntl(p[i][0], F_SETFL, O_NONBLOCK);
		fl_fd_add(f->app, p[i][0], POLLIN, hlp_ready, h);
	}
	h->open_n = 3;
	return h;
fail:
	for (int i = 0; i < 3; i++) {
		if (p[i][0] >= 0)
			close(p[i][0]);
		if (p[i][1] >= 0)
			close(p[i][1]);
	}
	free(h);
	return NULL;
}

/* ask flmount for the drive list, unless a list is already on its way */
static void drive_refresh(struct F *f)
{
	if (f->lst || !f->win)
		return;
	f->lst = hlp_spawn(f, HK_LIST, "list", NULL);
	if (!f->lst)
		drives_rebuild(f, NULL);
}

/* the mount table changed */
static void mi_ready(void *ud, int fd, short rev)
{
	struct F *f = ud;
	char b[512];
	(void)rev;
	lseek(fd, 0, SEEK_SET);
	while (read(fd, b, sizeof b) > 0)
		;
	drive_refresh(f);
}

static void tick(void *ud)
{
	struct F *f = ud;
	if (f->flm_ok)
		drive_refresh(f);
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
	int lines = 1;
	for (char *c = text; *c; c++)
		lines += *c == '\n';
	char *save = NULL;
	char *line = strtok_r(text, "\n", &save);
	if (!line || (strcmp(line, "x-flfiles/copy") && strcmp(line, "x-flfiles/cut"))) {
		say(f, 0, "Nothing to paste");
		free(text);
		return;
	}
	int cut = !strcmp(line, "x-flfiles/cut");
	struct jitem *it = calloc((size_t)lines, sizeof *it);
	int k = 0;
	while (it && (line = strtok_r(NULL, "\n", &save))) {
		if (line[0] != '/' || !path_exists(line))
			continue;
		char dst[PATH_MAX];
		snprintf(it[k].name, sizeof it[k].name, "%s", base_name(line));
		it[k].src = strdup(line);
		join(dst, sizeof dst, f->cwd, it[k].name);
		it[k].dst = strdup(dst);
		k++;
	}
	free(text);
	if (!k) {
		free(it);
		say(f, 0, "Nothing to paste");
		return;
	}
	request_job(f, cut ? JOB_MOVE : JOB_COPY, f->cwd, it, k, cut);
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

/* a terminal (flterm) in dir */
static void open_terminal(struct F *f, const char *dir)
{
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		signal(SIGCHLD, SIG_DFL);
		if (chdir(dir) != 0)
			_exit(1);
		execlp("flterm", "flterm", (char *)NULL);
		_exit(127);
	}
	if (pid < 0)
		say(f, 1, "Cannot start a terminal: %s", strerror(errno));
}

/* an app for Open with: a .desktop file that is not hidden */
static void add_choice(struct F *f, const char *path)
{
	if (f->nac >= (int)(sizeof f->ac / sizeof *f->ac))
		return;
	for (int i = 0; i < f->nac; i++)
		if (!strcmp(f->ac[i].desk, path))
			return;
	FILE *fp = fopen(path, "r");
	if (!fp)
		return;
	char name[96] = "", line[512];
	int in = 0, hidden = 0;
	while (fgets(line, sizeof line, fp)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '[') {
			in = !strcmp(line, "[Desktop Entry]");
			continue;
		}
		if (!in)
			continue;
		if (!strncmp(line, "Name=", 5) && !name[0])
			snprintf(name, sizeof name, "%s", line + 5);
		else if (!strcmp(line, "NoDisplay=true") || !strcmp(line, "Hidden=true"))
			hidden = 1;
	}
	fclose(fp);
	if (hidden)
		return;
	if (!name[0])
		snprintf(name, sizeof name, "%s", base_name(path));
	snprintf(f->ac[f->nac].desk, sizeof f->ac[f->nac].desk, "%s", path);
	snprintf(f->ac[f->nac].name, sizeof f->ac[f->nac].name, "%s", name);
	f->nac++;
}

static int desk_handles(const char *path, const char *mime)
{
	FILE *fp = fopen(path, "r");
	if (!fp)
		return 0;
	char line[2048];
	size_t ml = strlen(mime);
	int hit = 0;
	while (!hit && fgets(line, sizeof line, fp))
		if (!strncmp(line, "MimeType=", 9)) {
			for (char *s = line + 9; (s = strstr(s, mime)); s += ml)
				if ((s == line + 9 || s[-1] == ';') && (s[ml] == ';' || s[ml] == '\n' || s[ml] == 0)) {
					hit = 1;
					break;
				}
			break;
		}
	fclose(fp);
	return hit;
}

/* the apps named for mime in one mimeapps.list, Default first */
static void mimeapps_collect(struct F *f, const char *file, const char *mime)
{
	FILE *fp = fopen(file, "r");
	if (!fp)
		return;
	char line[1024], section[64] = "";
	size_t ml = strlen(mime);
	while (fgets(line, sizeof line, fp)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == '[') {
			snprintf(section, sizeof section, "%s", line);
			continue;
		}
		if (strcmp(section, "[Default Applications]") && strcmp(section, "[Added Associations]"))
			continue;
		if (strncmp(line, mime, ml) || line[ml] != '=')
			continue;
		for (char *t = strtok(line + ml + 1, ";"); t; t = strtok(NULL, ";")) {
			char desk[PATH_MAX];
			if (*t && find_desktop(t, desk, sizeof desk) == 0)
				add_choice(f, desk);
		}
	}
	fclose(fp);
}

/* the apps that open the file: the lists' choices first, then any .desktop that lists the type */
static int apps_for_file(struct F *f, const char *name, const char *path)
{
	char mime[128] = "";
	if (mime_of_name(name, mime, sizeof mime) < 0)
		snprintf(mime, sizeof mime, "%s", is_text_name(name) || sniff_text(path) ? "text/plain" : "");
	snprintf(f->ac_mime, sizeof f->ac_mime, "%s", mime);
	f->nac = 0;
	if (!mime[0])
		return 0;
	char d[8][PATH_MAX], p[PATH_MAX], lists[10][PATH_MAX];
	int nd, nl = 0;
	data_dirs(d, &nd);
	const char *home = getenv("HOME"), *cfg = getenv("XDG_CONFIG_HOME");
	if (cfg && *cfg)
		snprintf(lists[nl++], PATH_MAX, "%s/mimeapps.list", cfg);
	else
		snprintf(lists[nl++], PATH_MAX, "%s/.config/mimeapps.list", home ? home : "");
	snprintf(lists[nl++], PATH_MAX, "/etc/xdg/mimeapps.list");
	for (int i = 0; i < nd && nl < 10; i++)
		snprintf(lists[nl++], PATH_MAX, "%s/applications/mimeapps.list", d[i]);
	for (int i = 0; i < nl; i++)
		mimeapps_collect(f, lists[i], mime);
	for (int i = 0; i < nd; i++) {
		snprintf(p, sizeof p, "%s/applications", d[i]);
		DIR *dir = opendir(p);
		struct dirent *e;
		while (dir && (e = readdir(dir))) {
			size_t l = strlen(e->d_name);
			if (l < 9 || strcmp(e->d_name + l - 8, ".desktop") || !strcmp(e->d_name, "flfiles.desktop"))
				continue;
			char path2[PATH_MAX + 300];
			snprintf(path2, sizeof path2, "%s/%s", p, e->d_name);
			if (desk_handles(path2, mime))
				add_choice(f, path2);
		}
		if (dir)
			closedir(dir);
	}
	return f->nac;
}

static void mode_str(mode_t m, char *o)
{
	static const char t[] = "rwxrwxrwx";
	for (int i = 0; i < 9; i++)
		o[i] = (m & (0400 >> i)) ? t[i] : '-';
	o[9] = 0;
}

/* the six lines of the Properties dialog for entry i */
static void props_fill(struct F *f, int i)
{
	struct entry *e = &f->ents[i];
	char p[PATH_MAX], sz[32];
	struct stat st;
	join(p, sizeof p, f->cwd, e->name);
	memset(f->pr, 0, sizeof f->pr);
	snprintf(f->pr[0], sizeof f->pr[0], "%s", e->name);
	if (e->dir) {
		snprintf(f->pr[1], sizeof f->pr[1], "Folder");
		snprintf(f->pr[2], sizeof f->pr[2], "-");
	} else {
		char m[128];
		if (mime_of_name(e->name, m, sizeof m) == 0)
			snprintf(f->pr[1], sizeof f->pr[1], "%s", m);
		else
			snprintf(f->pr[1], sizeof f->pr[1], "%s", is_text_name(e->name) ? "text/plain" : "Unknown type");
		fmt_size(e->size, sz, sizeof sz);
		snprintf(f->pr[2], sizeof f->pr[2], "%s (%lld bytes)", sz, (long long)e->size);
	}
	struct tm tm;
	localtime_r(&e->mtime, &tm);
	strftime(f->pr[3], sizeof f->pr[3], "%-d %b %Y  %H:%M", &tm);
	if (lstat(p, &st) == 0) {
		char rwx[10];
		mode_str(st.st_mode, rwx);
		snprintf(f->pr[4], sizeof f->pr[4], "%s (%04o)", rwx, (unsigned)(st.st_mode & 07777));
	}
	snprintf(f->pr[5], sizeof f->pr[5], "%s", f->cwd);
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
			next_free(f->cwd, "New folder", nn, sizeof nn);
			open_prompt(f, A_DO_MKDIR, "New folder", nn);
		}
		break;
	case A_NEWFILE:
		if (in_trash(f))
			say(f, 0, "Cannot create files in the Trash");
		else {
			char nn[NAME_MAX + 1];
			next_free(f->cwd, "New file", nn, sizeof nn);
			open_prompt(f, A_DO_NEWFILE, "New empty file", nn);
		}
		break;
	case A_RENAME:
		if (count_sel(f) == 1 && f->cur >= 0 && f->sel[f->cur])
			open_prompt(f, A_DO_RENAME, "Rename", f->ents[f->cur].name);
		break;
	case A_TRASH: {
		if (!count_sel(f))
			break;
		if (in_trash(f)) {
			ask_permanent(f);
			break;
		}
		if (!egg_ok_selection(f)) {
			say(f, 0, "Left it where it is");
			break;
		}
		int n = 0;
		struct jitem *it = sel_items(f, NULL, &n);
		if (n)
			request_job(f, JOB_TRASH, NULL, it, n, 0);
		else
			free(it);
		break;
	}
	case A_PERMDEL:
		ask_permanent(f);
		break;
	case A_DO_DELETE: {
		int n = 0;
		struct jitem *it = sel_items(f, NULL, &n);
		if (n)
			request_job(f, JOB_DELETE, NULL, it, n, 0);
		else
			free(it);
		break;
	}
	case A_EMPTY:
		f->ov = OV_CONFIRM;
		snprintf(f->confirm_title, sizeof f->confirm_title, "Empty Trash");
		snprintf(f->confirm_text, sizeof f->confirm_text, "Everything in the Trash is deleted for good. This cannot be undone.");
		snprintf(f->confirm_btn, sizeof f->confirm_btn, "Empty");
		f->confirm_act = A_DO_EMPTY;
		break;
	case A_DO_EMPTY: {
		int n = 0;
		struct jitem *it = trash_all_items(f, &n);
		if (n)
			request_job(f, JOB_DELETE, NULL, it, n, 0);
		else {
			free(it);
			say(f, 0, "The Trash is empty");
		}
		break;
	}
	case A_RESTORE: {
		int n = 0;
		struct jitem *it = restore_items(f, &n);
		if (n)
			request_job(f, JOB_RESTORE, NULL, it, n, 0);
		else {
			free(it);
			say(f, 0, "Nothing selected");
		}
		break;
	}
	case A_COPY:
		do_copy(f, 0);
		break;
	case A_CUT:
		do_copy(f, 1);
		break;
	case A_PASTE:
		do_paste(f);
		break;
	case A_DROP:
		drop_selection(f, f->act_path, f->dr_copy);
		break;
	case A_JOB_START:
		job_start(f);
		break;
	case A_PROPS:
		if (f->act_idx >= 0 && f->act_idx < f->n) {
			props_fill(f, f->act_idx);
			f->ov = OV_PROPS;
		}
		break;
	case A_OPENWITH: {
		int i = f->act_idx;
		if (i < 0 || i >= f->n || f->ents[i].dir)
			break;
		join(f->ac_file, sizeof f->ac_file, f->cwd, f->ents[i].name);
		if (!apps_for_file(f, f->ents[i].name, f->ac_file)) {
			say(f, 0, "No app is set up to open %s", f->ents[i].name);
			break;
		}
		f->ov = OV_OPENWITH;
		break;
	}
	case A_TERMINAL: {
		char dir[PATH_MAX];
		int i = f->act_idx;
		if (f->ctx_item && i >= 0 && i < f->n && f->ents[i].dir)
			join(dir, sizeof dir, f->cwd, f->ents[i].name);
		else
			snprintf(dir, sizeof dir, "%s", f->cwd);
		open_terminal(f, dir);
		break;
	}
	case A_HIDDEN:
		f->show_hidden = !f->show_hidden;
		refresh(f);
		break;
	case A_REFRESH:
		refresh(f);
		places_load(f);
		drive_refresh(f);
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
	case A_DO_NEWFILE: {
		char why[160], p[PATH_MAX];
		if (!valid_name(f->prompt_text, why, sizeof why)) {
			snprintf(f->prompt_err, sizeof f->prompt_err, "%s", why);
			f->ov = OV_PROMPT;
			break;
		}
		join(p, sizeof p, f->cwd, f->prompt_text);
		int fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
		if (fd < 0) {
			snprintf(f->prompt_err, sizeof f->prompt_err, "%s", strerror(errno));
			f->ov = OV_PROMPT;
			break;
		}
		close(fd);
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
	case A_FOCUS_PATH:
		break;
	case A_EXTRACT:
	case A_COMPRESS:
		archive_action(f, act == A_COMPRESS);
		break;
	case A_MOUNT:
	case A_EJECT: {
		if (f->act_idx < 0 || f->act_idx >= f->ndrv || !f->drv[f->act_idx].dev[0])
			break;
		if (f->opr) {
			say(f, 0, "flmount is busy, try again in a moment");
			break;
		}
		struct drive *d = &f->drv[f->act_idx];
		int mount = act == A_MOUNT;
		say(f, 0, "%s %s...", mount ? "Mounting" : "Ejecting", d->label);
		f->opr = hlp_spawn(f, mount ? HK_MOUNT : HK_EJECT, mount ? "mount" : "eject", d->dev);
		if (f->opr)
			snprintf(f->opr->label, sizeof f->opr->label, "%s", d->label);
		else
			say(f, 1, "Cannot start flmount");
		break;
	}
	}
	fl_win_redraw(f->win);
}

/* ---- context menu --------------------------------------------------------------------------------------- */

static void menu_open(struct F *f, int x, int y, int on_item)
{
	int k = 0;
	int c = count_sel(f), trash = in_trash(f), one = c == 1;
	int ai = -1; /* the last selected entry */
	for (int i = 0; i < f->n; i++)
		if (f->sel && f->sel[i])
			ai = i;
	int file1 = one && ai >= 0 && !f->ents[ai].dir;
	f->ctx_item = on_item;
	if (on_item) {
		if (trash) {
			f->menu[k++] = (struct menu_item){"Restore", "", A_RESTORE, c > 0, 1};
			f->menu[k++] = (struct menu_item){"Copy", "Ctrl+C", A_COPY, 1, 0};
			f->menu[k++] = (struct menu_item){"Delete for good", "Del", A_PERMDEL, 1, 0};
		} else {
			f->menu[k++] = (struct menu_item){"Open", "Enter", A_OPEN, one, 0};
			f->menu[k++] = (struct menu_item){"Open with...", "", A_OPENWITH, file1, 1};
			f->menu[k++] = (struct menu_item){"Cut", "Ctrl+X", A_CUT, 1, 0};
			f->menu[k++] = (struct menu_item){"Copy", "Ctrl+C", A_COPY, 1, 0};
			f->menu[k++] = (struct menu_item){"Paste", "Ctrl+V", A_PASTE, 1, 1};
			f->menu[k++] = (struct menu_item){"Rename", "F2", A_RENAME, one, 0};
			f->menu[k++] = (struct menu_item){"Move to Trash", "Del", A_TRASH, 1, 0};
			f->menu[k++] = (struct menu_item){"Delete permanently", "Shift+Del", A_PERMDEL, 1, 1};
			f->menu[k++] = (struct menu_item){"Extract here", "", A_EXTRACT,
			                                  file1 && is_archive_name(f->ents[ai].name), 0};
			f->menu[k++] = (struct menu_item){"Compress", "", A_COMPRESS, 1, 1};
		}
		f->menu[k++] = (struct menu_item){"Properties", "", A_PROPS, one, 0};
		f->menu[k++] = (struct menu_item){"Open terminal here", "", A_TERMINAL, 1, 0};
	} else {
		f->menu[k++] = (struct menu_item){"New folder", "Ctrl+Shift+N", A_NEWDIR, !trash, 0};
		f->menu[k++] = (struct menu_item){"New empty file", "", A_NEWFILE, !trash, 0};
		f->menu[k++] = (struct menu_item){"Paste", "Ctrl+V", A_PASTE, 1, 1};
		f->menu[k++] = (struct menu_item){"Open terminal here", "", A_TERMINAL, 1, 1};
		f->menu[k++] = (struct menu_item){f->show_hidden ? "Hide hidden files" : "Show hidden files", "Ctrl+H",
		                                  A_HIDDEN, 1, 0};
		if (trash)
			f->menu[k++] = (struct menu_item){"Empty Trash", "", A_EMPTY, 1, 0};
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
	fl_text(cv, ui->f_semi, x, y, fl_th.strong, f->confirm_title, -1);
	y += 28;
	fl_text_in(cv, ui->f_reg, fl_r(x, y, w, 20), FL_LEFT, 0, fl_th.text, f->confirm_text);
	int by = box.y + box.h - 24 - 34;
	int ok = fl_button(ui, ID_DLG + 1, fl_r(x + w - 96, by, 96, 34), f->confirm_btn, FL_BTN_ACCENT);
	int cancel = fl_button(ui, ID_DLG + 2, fl_r(x + w - 96 - 8 - 96, by, 96, 34), "Cancel", FL_BTN_OUTLINE);
	if (ui->key == XKB_KEY_Escape) {
		ui->key = 0;
		cancel = 1;
	}
	if (ok) {
		f->ov = OV_NONE;
		queue(f, f->confirm_act);
	} else if (cancel) {
		f->ov = OV_NONE;
	}
}

/* a name clash: Replace, Keep both or Skip, for this item or for all the rest */
static void draw_clash(fl_ui *ui, struct F *f)
{
	fl_canvas *cv = &ui->cv;
	fl_rect box;
	dialog_frame(ui, f, 460, 196, &box);
	int x = box.x + 24, w = box.w - 48, y = box.y + 22;
	struct jitem *it = &f->pend[f->clash_idx];
	char dir[PATH_MAX], line[PATH_MAX + 160], fit[PATH_MAX + 170];
	snprintf(dir, sizeof dir, "%s", it->dst);
	char *s = strrchr(dir, '/');
	if (s == dir)
		dir[1] = 0;
	else if (s)
		*s = 0;
	fl_text(cv, ui->f_semi, x, y, fl_th.strong, "Name clash", -1);
	y += 28;
	snprintf(line, sizeof line, "\"%s\" is already in \"%s\".", it->name, dir);
	fl_text_fit(ui->f_reg, line, w, fit, sizeof fit);
	fl_text_in(cv, ui->f_reg, fl_r(x, y, w, 20), FL_LEFT, 0, fl_th.text, fit);
	y += 26;
	fl_text_in(cv, f->small, fl_r(x, y, w, 18), FL_LEFT, 0, fl_th.dim, "Replace it, keep both, or skip it.");
	y += 30;
	fl_switch(ui, ID_DLG + 5, fl_r(x, y, 40, 22), &f->clash_all);
	fl_text_in(cv, f->small, fl_r(x + 50, y, w - 50, 22), FL_LEFT, 0, fl_th.text, "Do the same for the rest");
	int by = box.y + box.h - 24 - 34;
	int rep = fl_button(ui, ID_DLG + 1, fl_r(x, by, 96, 34), "Replace", FL_BTN_ACCENT);
	int keep = fl_button(ui, ID_DLG + 2, fl_r(x + 102, by, 96, 34), "Keep both", FL_BTN_OUTLINE);
	int skip = fl_button(ui, ID_DLG + 3, fl_r(x + 204, by, 96, 34), "Skip", FL_BTN_OUTLINE);
	int stop = fl_button(ui, ID_DLG + 4, fl_r(x + 306, by, 96, 34), "Cancel", FL_BTN_PLAIN);
	if (ui->key == XKB_KEY_Escape) {
		ui->key = 0;
		stop = 1;
	}
	if (rep)
		clash_answer(f, JA_REPLACE);
	else if (keep)
		clash_answer(f, JA_PROCEED);
	else if (skip)
		clash_answer(f, JA_SKIP);
	else if (stop)
		clash_cancel(f);
}

/* Open with: the apps for the file's type, one button each */
static void draw_openwith(fl_ui *ui, struct F *f)
{
	fl_canvas *cv = &ui->cv;
	fl_rect box;
	int rows = f->nac ? f->nac : 1;
	dialog_frame(ui, f, 420, 150 + 38 * rows, &box);
	int x = box.x + 24, w = box.w - 48, y = box.y + 22;
	fl_text(cv, ui->f_semi, x, y, fl_th.strong, "Open with", -1);
	y += 28;
	char line[PATH_MAX + 80], fit[PATH_MAX + 90];
	snprintf(line, sizeof line, "Choose an app for \"%s\" (%s).", base_name(f->ac_file), f->ac_mime);
	fl_text_fit(ui->f_reg, line, w, fit, sizeof fit);
	fl_text_in(cv, ui->f_reg, fl_r(x, y, w, 20), FL_LEFT, 0, fl_th.text, fit);
	y += 26;
	int picked = -1;
	for (int i = 0; i < f->nac; i++) {
		if (fl_button(ui, ID_OW + (uint32_t)i, fl_r(x, y, w, 32), f->ac[i].name, i == 0 ? FL_BTN_ACCENT : FL_BTN_OUTLINE))
			picked = i;
		y += 38;
	}
	int by = box.y + box.h - 24 - 34;
	int cancel = fl_button(ui, ID_DLG + 2, fl_r(x + w - 96, by, 96, 34), "Cancel", FL_BTN_OUTLINE);
	if (ui->key == XKB_KEY_Escape) {
		ui->key = 0;
		cancel = 1;
	}
	if (picked >= 0) {
		f->ov = OV_NONE;
		spawn_desktop(f, f->ac[picked].desk, f->ac_file, base_name(f->ac_file));
	} else if (cancel) {
		f->ov = OV_NONE;
	}
}

static void draw_props(fl_ui *ui, struct F *f)
{
	fl_canvas *cv = &ui->cv;
	fl_rect box;
	dialog_frame(ui, f, 440, 262, &box);
	int x = box.x + 24, w = box.w - 48, y = box.y + 22;
	fl_text(cv, ui->f_semi, x, y, fl_th.strong, "Properties", -1);
	y += 30;
	static const char *const lab[6] = {"Name", "Type", "Size", "Modified", "Permissions", "Location"};
	for (int i = 0; i < 6; i++) {
		char fit[PATH_MAX + 80];
		fl_text(cv, f->small, x, y + 3, fl_th.dim, lab[i], -1);
		fl_text_fit(ui->f_reg, f->pr[i], w - 104, fit, sizeof fit);
		fl_text_in(cv, ui->f_reg, fl_r(x + 104, y, w - 104, 20), FL_LEFT, 0, fl_th.text, fit);
		y += 24;
	}
	int by = box.y + box.h - 24 - 34;
	int close = fl_button(ui, ID_DLG + 1, fl_r(x + w - 96, by, 96, 34), "Close", FL_BTN_OUTLINE);
	if (ui->key == XKB_KEY_Escape) {
		ui->key = 0;
		close = 1;
	}
	if (close)
		f->ov = OV_NONE;
}

/* a message: a title and up to four lines */
static void draw_msg(fl_ui *ui, struct F *f)
{
	fl_canvas *cv = &ui->cv;
	fl_rect box;
	dialog_frame(ui, f, 440, 210, &box);
	int x = box.x + 24, w = box.w - 48, y = box.y + 22;
	char fit[sizeof f->dlg_text + 16], buf[sizeof f->dlg_text];
	fl_text_fit(ui->f_semi, f->dlg_title, w, fit, sizeof fit);
	fl_text_in(cv, ui->f_semi, fl_r(x, y, w, 24), FL_LEFT, 0, fl_th.strong, fit);
	y += 32;
	snprintf(buf, sizeof buf, "%s", f->dlg_text);
	char *s = buf;
	for (int ln = 0; ln < 4 && s && *s; ln++) {
		char *e = strchr(s, '\n');
		if (e)
			*e = 0;
		fl_text_fit(ui->f_reg, s, w, fit, sizeof fit);
		fl_text_in(cv, ui->f_reg, fl_r(x, y, w, 20), FL_LEFT, 0, fl_th.text, fit);
		y += 22;
		s = e ? e + 1 : NULL;
	}
	int by = box.y + box.h - 24 - 34;
	int ok = fl_button(ui, ID_DLG + 1, fl_r(x + w - 96, by, 96, 34), "OK", FL_BTN_ACCENT);
	if (ui->key == XKB_KEY_Escape || ui->key == XKB_KEY_Return || ui->key == XKB_KEY_KP_Enter) {
		ui->key = 0;
		ok = 1;
	}
	if (ok)
		f->ov = OV_NONE;
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

/* a row under the pointer while a drag is on: it becomes the drop target */
static void sidebar_target(struct F *f, const char *path, int res, fl_rect r)
{
	if (f->dr_state == 2 && (res & FL_HOVER) && is_dir_path(path)) {
		snprintf(f->dr_target, sizeof f->dr_target, "%s", path);
		f->dr_rect = r;
	}
}

/* the eject symbol: a triangle over a bar */
static void draw_eject(fl_canvas *cv, fl_rect r, fl_color c)
{
	fl_path *p = fl_path_new();
	if (p) {
		float x = (float)r.x, y = (float)r.y, w = (float)r.w, h = (float)r.h;
		fl_path_move(p, x, y + h * 0.6f);
		fl_path_line(p, x + w / 2, y + h * 0.1f);
		fl_path_line(p, x + w, y + h * 0.6f);
		fl_path_close(p);
		fl_path_fill(cv, p, c);
		fl_path_free(p);
	}
	fl_fill_rect(cv, fl_r(r.x, r.y + r.h - 3, r.w, 3), c);
}

static void draw_sidebar(fl_ui *ui, struct F *f, fl_rect sb)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, sb, fl_th.panel);
	fl_fill_rect(cv, fl_r(sb.x + sb.w - 1, sb.y, 1, sb.h), fl_th.line);
	int y = sb.y + 14;
	fl_text_ex(cv, f->head, sb.x + 16, y, fl_th.dim, "PLACES", -1, 56);
	y += fl_font_height(f->head) + 8;
	for (int i = 0; i < f->nplaces; i++) {
		struct place *p = &f->places[i];
		fl_rect r = fl_r(sb.x, y, sb.w - 1, 30);
		int res = fl_widget(ui, ID_SIDE + (uint32_t)i, r, 0);
		int active = !strcmp(p->path, f->cwd);
		sidebar_target(f, p->path, res, r);
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
	y += 14;
	fl_text_ex(cv, f->head, sb.x + 16, y, fl_th.dim, "DRIVES", -1, 56);
	y += fl_font_height(f->head) + 8;
	for (int i = 0; i < f->ndrv; i++) {
		struct drive *d = &f->drv[i];
		int eject = d->removable && d->dev[0];
		fl_rect r = fl_r(sb.x, y, sb.w - 1 - (eject ? 30 : 0), 40);
		int res = fl_widget(ui, ID_DRIVE + (uint32_t)i, r, 0);
		int active = d->mounted && !strcmp(d->path, f->cwd);
		if (d->mounted)
			sidebar_target(f, d->path, res, r);
		if (active)
			fl_fill_rect(cv, r, fl_th.select);
		else if (res & (FL_HOVER | FL_DOWN))
			fl_fill_rect(cv, r, fl_th.raised);
		if (active)
			fl_fill_rect(cv, fl_r(r.x, r.y, 3, r.h), fl_th.accent);
		fl_icon_draw(cv, FL_ICON_DRIVE, r.x + 16, r.y + 12, 16, active ? fl_th.accent : fl_th.dim);
		fl_text_in(cv, active ? ui->f_med : ui->f_reg, fl_r(r.x + 42, r.y + 4, r.w - 48, 18), FL_LEFT, 0,
		           active ? fl_th.strong : d->mounted ? fl_th.text : fl_th.dim, d->label);
		char sub[80] = "", sz[32] = "";
		if (d->size) {
			fmt_size((off_t)d->size, sz, sizeof sz);
			snprintf(sub, sizeof sub, "%s%s%s", sz, d->fstype[0] ? "  " : "", d->fstype);
		} else if (d->system) {
			snprintf(sub, sizeof sub, "%s", d->path);
		} else if (!d->mounted) {
			snprintf(sub, sizeof sub, "click to mount");
		}
		fl_text_in(cv, f->small, fl_r(r.x + 42, r.y + 21, r.w - 48, 14), FL_LEFT, 0, fl_th.dim, sub);
		if (res & FL_CLICK) {
			if (d->mounted) {
				snprintf(f->act_path, sizeof f->act_path, "%s", d->path);
				queue(f, A_NAV);
			} else {
				f->act_idx = i;
				queue(f, A_MOUNT);
			}
			ui->focus = ID_GRID;
		}
		if (eject) {
			fl_rect er = fl_r(sb.x + sb.w - 1 - 28, y + 9, 22, 22);
			int eres = fl_widget(ui, ID_EJECT + (uint32_t)i, er, 0);
			if (eres & FL_HOVER)
				fl_fill_rect(cv, er, fl_th.raised);
			draw_eject(cv, fl_inset(er, 5, 5), (eres & FL_HOVER) ? fl_th.text : fl_th.dim);
			if (eres & FL_CLICK) {
				f->act_idx = i;
				queue(f, A_EJECT);
				ui->focus = ID_GRID;
			}
		}
		y += 40;
	}
}

/* the Stop card for the running job, at the bottom right of the grid; the folder stays usable */
static fl_rect job_box(fl_rect area)
{
	return fl_r(area.x + area.w - 336, area.y + area.h - 116, 320, 100);
}

static void draw_job(fl_ui *ui, struct F *f, fl_rect area)
{
	struct job *j = f->job;
	if (!j)
		return;
	fl_canvas *cv = &ui->cv;
	fl_rect box = job_box(area);
	fl_fill_rect(cv, box, fl_th.raised);
	fl_stroke_rect(cv, box, 1, fl_th.line);
	fl_text_in(cv, ui->f_med, fl_r(box.x + 14, box.y + 8, box.w - 28, 20), FL_LEFT, 0, fl_th.strong, j->title);
	char fit[NAME_MAX + 64];
	fl_text_fit(f->small, j->cur, box.w - 28, fit, sizeof fit);
	fl_text_in(cv, f->small, fl_r(box.x + 14, box.y + 30, box.w - 28, 16), FL_LEFT, 0, fl_th.dim, fit);
	fl_rect track = fl_r(box.x + 14, box.y + 52, box.w - 28, 6);
	fl_fill_rect(cv, track, fl_th.line);
	if (j->total > 0) {
		long long d = j->done > j->total ? j->total : j->done;
		int fw = (int)(track.w * d / j->total);
		fl_fill_rect(cv, fl_r(track.x, track.y, fw, track.h), fl_th.accent);
	}
	if (fl_button(ui, ID_JOB, fl_r(box.x + box.w - 14 - 88, box.y + 66, 88, 28),
	              j->cancel_req ? "Stopping" : "Stop", FL_BTN_OUTLINE))
		job_stop(f);
}

/* the drag in progress: a label at the pointer, the target outlined, the drop on release */
static void drag_update(fl_ui *ui, struct F *f)
{
	fl_canvas *cv = &ui->cv;
	if (f->dr_state == 2 && (ui->release & FL_BTN_L)) {
		if (f->dr_target[0]) {
			snprintf(f->act_path, sizeof f->act_path, "%s", f->dr_target);
			f->dr_copy = (fl_app_mods(f->app) & FL_MOD_CTRL) != 0;
			queue(f, A_DROP);
		}
		f->dr_state = 0;
		return;
	}
	if (f->dr_state == 1 && (ui->release & FL_BTN_L))
		f->dr_state = 0;
	if (f->dr_state == 1) {
		int dx = ui->mx - f->dr_x, dy = ui->my - f->dr_y;
		if (dx * dx + dy * dy > 36)
			f->dr_state = 2;
	}
	if (f->dr_state != 2)
		return;
	if (f->dr_target[0])
		fl_stroke_rect(cv, f->dr_rect, 2, fl_th.accent);
	int c = count_sel(f);
	char lab[64];
	snprintf(lab, sizeof lab, "%s %d item%s", (fl_app_mods(f->app) & FL_MOD_CTRL) ? "Copy" : "Move", c, c == 1 ? "" : "s");
	fl_rect g = fl_r(ui->mx + 14, ui->my + 14, fl_text_width(f->small, lab, -1) + 20, 24);
	fl_fill_rect(cv, g, fl_th.raised);
	fl_stroke_rect(cv, g, 1, fl_th.line);
	fl_text_in(cv, f->small, g, FL_LEFT, 10, fl_th.text, lab);
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
			case XKB_KEY_Delete: queue(f, shift ? A_PERMDEL : A_TRASH); break;
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
	fl_rect hit_r = {0};
	/* the grid's own widget took the press (active) before the cells were met: give it back to the cells for
	 * this loop (a cell takes it, else the grid keeps it), or no cell would ever see a click */
	int grid_active = ui->active == ID_GRID;
	if (grid_active)
		ui->active = 0;
	int first = f->scroll / CELL_H * cols;
	int last = (f->scroll + g.h) / CELL_H * cols + cols;
	for (int i = first; i < last && i < f->n; i++) {
		int col = i % cols, row = i / cols;
		fl_rect r = fl_r(x0 + col * CELL_W, g.y + 8 + row * CELL_H - f->scroll, CELL_W, CELL_H);
		int cr = fl_widget(ui, ID_CELL + (uint32_t)i, r, 0);
		draw_cell(ui, f, r, i, cr & FL_HOVER);
		if (cr & FL_HOVER) {
			hit = i;
			hit_r = r;
			if (ui->press & FL_BTN_L) {
				click_select(f, i, mods);
				f->dr_state = 1;
				f->dr_x = ui->mx;
				f->dr_y = ui->my;
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
	if (grid_active && !ui->active)
		ui->active = ID_GRID;
	if (f->dr_state == 2 && hit >= 0 && f->ents[hit].dir && !f->sel[hit]) {
		join(f->dr_target, sizeof f->dr_target, f->cwd, f->ents[hit].name);
		f->dr_rect = hit_r;
	}
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
	f->dr_target[0] = 0;
	/* the job card is on top of the grid: the grid must not see the pointer there */
	int saved_in = ui->mouse_in;
	if (f->job && fl_in(job_box(area), ui->mx, ui->my))
		ui->mouse_in = 0;
	draw_grid(ui, f, area);
	ui->mouse_in = saved_in;
	draw_sidebar(ui, f, sb);
	draw_toolbar(ui, f, tb);
	draw_status(ui, f, st);
	draw_job(ui, f, area);
	drag_update(ui, f);

	if (modal) {
		unmask_input(ui, &im);
		if (f->ov == OV_MENU)
			draw_menu(ui, f);
		else if (f->ov == OV_PROMPT)
			draw_prompt(ui, f);
		else if (f->ov == OV_CONFIRM)
			draw_confirm(ui, f);
		else if (f->ov == OV_CLASH && f->npend)
			draw_clash(ui, f);
		else if (f->ov == OV_OPENWITH)
			draw_openwith(ui, f);
		else if (f->ov == OV_PROPS)
			draw_props(ui, f);
		else if (f->ov == OV_MSG)
			draw_msg(ui, f);
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
		drive_refresh(f);
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
		/* the freedesktop Trash: files/ and info/ side by side */
		snprintf(f->trash_base, sizeof f->trash_base, "%.*s", (int)(strlen(f->trash_files) - 6), f->trash_files);
		char info[PATH_MAX];
		snprintf(info, sizeof info, "%s/info", f->trash_base);
		mkdir_p(f->trash_files);
		mkdir_p(info);
	}
	f->ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	f->ino_wd = -1;
	f->mi_fd = open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
	f->msg_timer = fl_timer_new(f->app, msg_clear, f);
	f->tick = fl_timer_new(f->app, tick, f);
	fl_timer_arm(f->tick, 3000, 3000);

	fl_win_opts o = {
		.title = "Files", .app_id = "flfiles", .w = 860, .h = 560, .min_w = 520, .min_h = 320,
		.decorated = 1, .resizable = 1, .draw = draw, .focus = on_focus, .ud = f,
	};
	f->win = fl_win_new(f->app, &o);
	if (!f->win)
		return 1;
	if (f->ino_fd >= 0)
		fl_fd_add(f->app, f->ino_fd, POLLIN, inotify_ready, f);
	if (f->mi_fd >= 0)
		fl_fd_add(f->app, f->mi_fd, POLLPRI, mi_ready, f);

	const char *start = argc == 2 ? argv[1] : home && *home ? home : "/";
	if (load_dir(f, start, 1) < 0) {
		char first[300];
		snprintf(first, sizeof first, "%s", f->msg);
		if (load_dir(f, "/", 1) == 0)
			say(f, 1, "%s", first);
	}
	drives_rebuild(f, NULL); /* System and mounts now; flmount's list follows */
	drive_refresh(f);
	fl_app_watch_conf(f->app, NULL, NULL);
	fl_app_run(f->app);
	fl_app_free(f->app);
	return 0;
}
