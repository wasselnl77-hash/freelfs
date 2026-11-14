/* fileinfo.c -- the desktop icons' Open with and Properties
 *
 * Open with lists the apps for the file's type, found the way Files finds them (src/flfiles,
 * "Open with"): the type from the shared-mime-info glob table, the apps from mimeapps.list
 * (Default first, then the others), then any .desktop file that lists the type. The code is a
 * copy of Files' (the two programs are built apart), trimmed to the list.
 *
 * Properties is a decorated window like the panel's dialogs: name, type, size, modified,
 * permissions and location. A folder's size is counted in a child process (fork, no exec): the
 * window says "Counting..." until the child writes its total on a pipe. One window at a time; its
 * close button, Esc or Enter closes it, and a count still running is killed with it.
 * SPDX-License-Identifier: MIT
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <flui.h>
#include <limits.h>
#include <poll.h>
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

/* ---- Open with: the type and the apps --------------------------------------------------------- */

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

/* the MIME type of a file name from /usr/share/mime/globs2: the heaviest, then the longest pattern */
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

/* the .desktop file for an id such as flview.desktop */
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

/* an app for the list: a .desktop file that is not hidden, named by its Name= */
static void add_choice(struct app_choice *out, int *n, int max, const char *path)
{
	if (*n >= max)
		return;
	for (int i = 0; i < *n; i++)
		if (!strcmp(out[i].desk, path))
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
	if (!name[0]) {
		const char *slash = strrchr(path, '/');
		snprintf(name, sizeof name, "%s", slash ? slash + 1 : path);
	}
	snprintf(out[*n].desk, sizeof out[*n].desk, "%s", path);
	snprintf(out[*n].name, sizeof out[*n].name, "%s", name);
	(*n)++;
}

/* 1 when the .desktop file's MimeType= names mime */
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

/* the apps a mimeapps.list names for mime, in the list's order */
static void mimeapps_collect(struct app_choice *out, int *n, int max, const char *file, const char *mime)
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
				add_choice(out, n, max, desk);
		}
	}
	fclose(fp);
}

int fileinfo_apps(const char *path, struct app_choice *out, int max, char *mime, size_t mcap)
{
	const char *slash = strrchr(path, '/');
	const char *name = slash ? slash + 1 : path;
	int n = 0;
	mime[0] = 0;
	if (mime_of_name(name, mime, mcap) < 0 || !mime[0])
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
		mimeapps_collect(out, &n, max, lists[i], mime);
	/* then every .desktop file that lists the type (Files leaves out its own) */
	for (int i = 0; i < nd; i++) {
		snprintf(p, sizeof p, "%s/applications", d[i]);
		DIR *dir = opendir(p);
		struct dirent *e;
		while (dir && (e = readdir(dir))) {
			size_t l = strlen(e->d_name);
			if (l < 9 || strcmp(e->d_name + l - 8, ".desktop") || !strcmp(e->d_name, "flfiles.desktop"))
				continue;
			char path2[PATH_MAX];
			snprintf(path2, sizeof path2, "%s/%s", p, e->d_name);
			if (desk_handles(path2, mime))
				add_choice(out, &n, max, path2);
		}
		if (dir)
			closedir(dir);
	}
	return n;
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

void fileinfo_open_with(const char *desk, const char *path)
{
	char *argv[32] = {0};
	if (exec_args(desk, path, argv, 32) < 0)
		return;
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		signal(SIGCHLD, SIG_DFL);
		execvp(argv[0], argv);
		_exit(127);
	}
	for (int i = 0; argv[i]; i++)
		free(argv[i]);
}

/* ---- Properties -------------------------------------------------------------------------------- */

#define PR_W 380
#define PR_ROWS 6
#define PR_ROW_H 26
#define PR_BTN_H 32
#define PR_CH 274 /* the content: header, six rows, the button */
enum { ID_PR_CLOSE = 1 };
enum { PR_NAME, PR_TYPE, PR_SIZE, PR_MOD, PR_PERM, PR_WHERE };
static const char *const pr_label[PR_ROWS] = {"Name", "Type", "Size", "Modified", "Permissions", "Location"};

static struct {
	struct panel *p;
	fl_win *w;
	fl_timer *later;	/* made once, kept: a window is never destroyed inside its own pass */
	int act;
	char path[PATH_MAX], head[256];
	char val[PR_ROWS][512];
	int count_fd, count_pid;
	char cbuf[32];
	size_t clen;
} PR = { .count_fd = -1 };

static void props_destroy(void);

static void later_cb(void *ud)
{
	(void)ud;
	if (PR.act) {
		PR.act = 0;
		props_destroy();
	}
}

/* the close button of the title bar: after the pass, like the dialogs */
static void on_close(fl_win *w, void *ud)
{
	(void)w;
	(void)ud;
	PR.act = 1;
	fl_timer_arm(PR.later, 1, 0);
}

static void fmt_bytes(long long n, char *out, size_t cap)
{
	static const char *const unit[] = {"bytes", "KB", "MB", "GB", "TB"};
	if (n < 1024) {
		snprintf(out, cap, "%lld bytes", n);
		return;
	}
	double v = (double)n;
	int u = 0;
	while (v >= 1024 && u < 4) {
		v /= 1024;
		u++;
	}
	snprintf(out, cap, "%.1f %s (%lld bytes)", v, unit[u], n);
}

static void mode_str(mode_t m, char *o)
{
	static const char t[] = "rwxrwxrwx";
	for (int i = 0; i < 9; i++)
		o[i] = (m & (0400 >> i)) ? t[i] : '-';
	o[9] = 0;
}

/* bytes of a file, or of everything under a folder (symbolic links are not followed) */
static long long tree_bytes(const char *path, int depth)
{
	struct stat st;
	if (lstat(path, &st) < 0)
		return 0;
	if (!S_ISDIR(st.st_mode))
		return (long long)st.st_size;
	if (depth > 64)
		return 0;
	DIR *d = opendir(path);
	if (!d)
		return 0;
	long long total = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char sub[PATH_MAX + 300];
		snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
		total += tree_bytes(sub, depth + 1);
	}
	closedir(d);
	return total;
}

static void props_destroy(void)
{
	if (PR.count_fd >= 0) {
		fl_fd_remove(PR.p->app, PR.count_fd);
		close(PR.count_fd);
		PR.count_fd = -1;
	}
	if (PR.count_pid > 0) { /* our own child, still counting: stop it and reap it */
		kill(PR.count_pid, SIGKILL);
		waitpid(PR.count_pid, NULL, 0);
		PR.count_pid = 0;
	}
	if (PR.w) {
		fl_win_destroy(PR.w);
		PR.w = NULL;
	}
}

static void count_unknown(void)
{
	snprintf(PR.val[PR_SIZE], sizeof PR.val[PR_SIZE], "%s", "unknown");
	if (PR.w)
		fl_win_redraw(PR.w);
}

/* the child's total is in: a number of bytes, then the end of the pipe */
static void count_ready(void *ud, int fd, short rev)
{
	(void)ud;
	(void)rev;
	char buf[32];
	ssize_t n = read(fd, buf, sizeof buf);
	if (n == -1 && (errno == EINTR || errno == EAGAIN))
		return;
	if (n > 0) {
		size_t room = sizeof PR.cbuf - 1 - PR.clen;
		if (room > (size_t)n)
			room = (size_t)n;
		memcpy(PR.cbuf + PR.clen, buf, room);
		PR.clen += room;
		PR.cbuf[PR.clen] = 0;
		return;
	}
	fl_fd_remove(PR.p->app, fd);
	close(fd);
	PR.count_fd = -1;
	if (PR.count_pid > 0) {
		waitpid(PR.count_pid, NULL, 0);
		PR.count_pid = 0;
	}
	if (PR.clen == 0 || n < 0) {
		count_unknown();
		return;
	}
	fmt_bytes(strtoll(PR.cbuf, NULL, 10), PR.val[PR_SIZE], sizeof PR.val[PR_SIZE]);
	if (PR.w)
		fl_win_redraw(PR.w);
}

static void count_start(void)
{
	int fd[2];
	if (pipe2(fd, O_CLOEXEC) < 0)
		return count_unknown();
	pid_t pid = fork();
	if (pid < 0) {
		close(fd[0]);
		close(fd[1]);
		return count_unknown();
	}
	if (pid == 0) { /* the child only counts and writes the total: no exec, no panel state */
		close(fd[0]);
		char b[32];
		int l = snprintf(b, sizeof b, "%lld", tree_bytes(PR.path, 0));
		if (write(fd[1], b, (size_t)l) < 0)
			_exit(1);
		_exit(0);
	}
	close(fd[1]);
	if (fl_fd_add(PR.p->app, fd[0], POLLIN, count_ready, NULL) < 0) {
		close(fd[0]);
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return count_unknown();
	}
	PR.count_fd = fd[0];
	PR.count_pid = pid;
}

static void props_fill(const char *path, int is_dir)
{
	struct stat st;
	char dir[PATH_MAX];
	const char *slash = strrchr(path, '/');
	const char *base = slash ? slash + 1 : path;
	snprintf(dir, sizeof dir, "%s", path);
	char *cut = strrchr(dir, '/');
	if (cut == dir)
		dir[1] = 0;
	else if (cut)
		*cut = 0;
	memset(PR.val, 0, sizeof PR.val);
	snprintf(PR.head, sizeof PR.head, "%s", base);
	snprintf(PR.val[PR_NAME], sizeof PR.val[PR_NAME], "%s", base);
	if (stat(path, &st) < 0) {
		snprintf(PR.val[PR_TYPE], sizeof PR.val[PR_TYPE], "%s", strerror(errno));
		snprintf(PR.val[PR_WHERE], sizeof PR.val[PR_WHERE], "%s", dir);
		return;
	}
	if (is_dir) {
		snprintf(PR.val[PR_TYPE], sizeof PR.val[PR_TYPE], "%s", "Folder");
		snprintf(PR.val[PR_SIZE], sizeof PR.val[PR_SIZE], "%s", "Counting...");
	} else {
		char mime[128];
		if (mime_of_name(base, mime, sizeof mime) == 0)
			snprintf(PR.val[PR_TYPE], sizeof PR.val[PR_TYPE], "%s", mime);
		else
			snprintf(PR.val[PR_TYPE], sizeof PR.val[PR_TYPE], "%s", "Unknown type");
		fmt_bytes((long long)st.st_size, PR.val[PR_SIZE], sizeof PR.val[PR_SIZE]);
	}
	struct tm tm;
	localtime_r(&st.st_mtime, &tm);
	strftime(PR.val[PR_MOD], sizeof PR.val[PR_MOD], "%-d %b %Y  %H:%M", &tm);
	char rwx[10];
	mode_str(st.st_mode, rwx);
	snprintf(PR.val[PR_PERM], sizeof PR.val[PR_PERM], "%s (%04o)", rwx, (unsigned)(st.st_mode & 07777));
	snprintf(PR.val[PR_WHERE], sizeof PR.val[PR_WHERE], "%s", dir);
}

static void props_draw(fl_ui *ui, void *ud)
{
	(void)ud;
	fl_canvas *cv = &ui->cv;
	fl_rect cr = fl_win_content(ui->win); /* the drawing starts under the title bar */
	int ox = cr.x, oy = cr.y;
	int esc = ui->key == XKB_KEY_Escape;
	int enter = ui->key == XKB_KEY_Return || ui->key == XKB_KEY_KP_Enter;
	if (esc || enter)
		ui->key = 0;
	fl_fill_rect(cv, fl_r(0, 0, cv->w, cv->h), fl_th.panel);
	fl_fill_rect(cv, fl_r(ox, oy, PR_W, 2), fl_th.accent);
	fl_text_in(cv, ui->f_semi, fl_r(ox + 20, oy + 16, PR_W - 40, 26), FL_LEFT, 0, fl_th.strong, PR.head);
	int y = oy + 56;
	for (int i = 0; i < PR_ROWS; i++, y += PR_ROW_H) {
		fl_text_in(cv, ui->f_reg, fl_r(ox + 20, y, 96, PR_ROW_H), FL_LEFT, 0, fl_th.dim, pr_label[i]);
		char fit[600];
		fl_text_fit(ui->f_reg, PR.val[i], PR_W - 40 - 100, fit, sizeof fit);
		fl_text_in(cv, ui->f_reg, fl_r(ox + 120, y, PR_W - 140, PR_ROW_H), FL_LEFT, 0, fl_th.text, fit);
	}
	int by = oy + 56 + PR_ROWS * PR_ROW_H + 14;
	if (fl_button(ui, ID_PR_CLOSE, fl_r(ox + PR_W - 20 - 96, by, 96, PR_BTN_H), "Close", FL_BTN_ACCENT) || esc || enter) {
		PR.act = 1;
		fl_timer_arm(PR.later, 1, 0);
	}
}

void props_open(struct panel *p, const char *path, int is_dir)
{
	if (!p || !p->app)
		return;
	if (!PR.later)
		PR.later = fl_timer_new(p->app, later_cb, NULL);
	if (!PR.later)
		return;
	props_destroy();
	PR.p = p;
	snprintf(PR.path, sizeof PR.path, "%s", path);
	props_fill(path, is_dir);
	fl_win_opts o = {
		.title = "Properties", .app_id = "flpanel-properties", .w = PR_W + 2, .h = PR_CH + fl_lk.title_h + 3,
		.decorated = 1, .resizable = 0, /* the size is the whole window, as in dialog.c */
		.draw = props_draw, .close = on_close, .ud = NULL,
	};
	PR.w = fl_win_new(p->app, &o);
	if (!PR.w)
		return;
	fl_win_redraw(PR.w);
	if (is_dir)
		count_start();
}
