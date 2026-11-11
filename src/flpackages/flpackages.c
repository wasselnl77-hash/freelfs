/* flpackages -- the FreeLFS package app
 * SPDX-License-Identifier: MIT
 *
 * One window like flfiles: a toolbar (Installed / Available, search), the
 * package list on the left, details with Install and Remove on the right, a
 * log strip while a change runs, a status bar.
 *
 * Everything comes from flpkg, run as a child process so the window never
 * waits: `flpkg list`, `flpkg search`, `flpkg info`. Changes go through
 * flpkg-seat, the small setuid helper (install NAME... and remove NAME...
 * only); its output shows in the log strip.
 *
 * Immediate mode, as in flfiles: draw() reads the input and queues at most
 * one action, which runs at the end of the pass.
 *
 * The Flathub tab does the same with flatpak: `flatpak remote-ls`, `list`,
 * `info` and `remote-info` read, `install`, `uninstall` and `update` change.
 * Everything is --user (per-user installs: no root, no polkit, no helper), as
 * a child process of the person's own account. Output is plain (no terminal),
 * tab-separated with --columns, so there is nothing to scrape but lines. The
 * whole Flathub catalog (about 3000 apps) is read once and searched here.
 *
 * Test hooks: FLPACKAGES_FLPKG, FLPACKAGES_SEAT, FLPACKAGES_CONF and
 * FLPACKAGES_FLATPAK replace /usr/bin/flpkg, /usr/bin/flpkg-seat,
 * /etc/flpkg.conf and /usr/bin/flatpak; FLPACKAGES_TAB=flathub opens on the
 * Flathub tab.
 */
#include <errno.h>
#include <fcntl.h>
#include <flui.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
	ID_TAB_INST = 1, ID_TAB_AVAIL, ID_SEARCH, ID_LIST, ID_INSTALL, ID_REMOVE, ID_CANCEL, ID_LOGCLOSE,
	ID_TAB_FLAT, ID_FM_BROWSE, ID_FM_INST, ID_OPEN, ID_SIDE, ID_ADD_REMOTE,
};
enum { TOOL_H = 44, STATUS_H = 28, LIST_W = 340, LOG_H = 170, ROW_H = 52, PAD = 24, STRIP_H = 40 };
enum { TAB_INST, TAB_AVAIL, TAB_FLAT };
enum { FM_BROWSE, FM_INST }; /* the Flathub tab: the whole catalog, or what is installed */
enum { A_NONE, A_TAB_INST, A_TAB_AVAIL, A_TAB_FLAT, A_SELECT, A_INSTALL, A_ASK_REMOVE, A_REMOVE, A_CANCEL,
       A_LOGCLOSE, A_FILTER, A_FM_BROWSE, A_FM_INST, A_OPEN, A_SIDE, A_ADD_REMOTE };
enum { ST_AVAIL, ST_INST, ST_OLDER }; /* installed: older = a newer version is in the repo */
enum { J_LIST, J_INFO, J_CHANGE, J_REM, J_CAT, J_FINST, J_FUPD, J_COUNT };
/* what a running change does (the log strip and what happens after it) */
enum { CK_PKG, CK_FINSTALL, CK_FREMOVE, CK_FUPDATE, CK_FUPDATE_ALL, CK_FUNUSED, CK_FREFRESH, CK_FADDREMOTE };

#define FLATHUB_URL "https://dl.flathub.org/repo/flathub.flatpakrepo"

#define MAX_BUF (8u << 20) /* a runaway child cannot eat the memory */
#define MAX_KV 24

struct pkg {
	char *name, *ver, *sum;
	char *id; /* Flatpak application ID; NULL for flpkg packages (their name is the key) */
	int state;
};

struct kv {
	char key[32], val[600];
};

struct F;

struct job {
	struct F *f;
	int kind;
	pid_t pid;
	int fd[2]; /* stdout, stderr; -1 when closed or merged */
	char *buf[2];
	size_t n[2], cap[2];
	int status, finished;
};

struct F {
	fl_app *app;
	fl_win *win;
	fl_font *small, *title, *mono, *head;
	fl_timer *msg_timer, *search_timer, *anim_timer;
	char msg[400];
	int msg_err;

	const char *flpkg, *seat, *conf, *flatpak;
	int tab, repo;           /* repo: a repo line is configured */
	char query[128];
	char last_query[128];    /* what the Available list was searched for */

	struct pkg *all;         /* what the last list or search returned */
	size_t nall;
	int *shown;              /* indexes into all, after the filter */
	int nshown;
	int loaded;              /* a list has arrived for this tab */
	char list_err[300];      /* last stderr line of the list job */
	fl_list_state ls;

	/* Flathub: the catalog and the installed apps are kept; all is a view on one of them */
	int fmode;               /* FM_BROWSE or FM_INST */
	int fl_have;             /* the flatpak program exists */
	int fl_remote;           /* -1 not asked yet, 0 no flathub remote, 1 there */
	int fl_refreshed;        /* the appstream refresh was tried in this run */
	struct pkg *cat, *fin;   /* the catalog, the installed apps */
	size_t ncat, nfin;
	int cat_loaded, fin_loaded, cat_cached;
	char cat_err[300];
	char **upd;              /* IDs with an update */
	size_t nupd;
	int shallow;             /* all holds copies of cat/fin entries: free the array only */

	char sel[80];            /* selected package name, or Flatpak ID */
	struct kv kv[MAX_KV];
	int nkv;
	int info_ok;
	char info_for[80];
	int ask_remove;          /* the Remove button asked for confirmation */

	struct job job[J_COUNT];
	char chg_what[256];      /* "Installing foo" while running, "Installed foo" after */
	int chg_visible, chg_failed;
	char chg_name[80], chg_done[100];
	int chg_kind;

	int act, act_arg;
	int inited;
};

/* ---- small helpers ------------------------------------------------------------------- */

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
	fl_timer_arm(f->msg_timer, 8000, 0);
	fl_win_redraw(f->win);
}

static void queue(struct F *f, int act, int arg)
{
	if (!f->act) {
		f->act = act;
		f->act_arg = arg;
	}
}

/* The same rule as flpkg-seat: [a-z0-9+._-]+, first a letter or digit. */
static int valid_name(const char *s)
{
	size_t n = strlen(s);
	if (n == 0 || n > 64 || !((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= '0' && s[0] <= '9')))
		return 0;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '.' || c == '_' ||
		      c == '-'))
			return 0;
	}
	return n < 6 || strcmp(s + n - 6, ".flpkg") != 0;
}

/* Last non-empty line of buf, trimmed, into out. */
static void last_line(const char *buf, size_t n, char *out, size_t cap)
{
	out[0] = 0;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
		n--;
	size_t s = n;
	while (s > 0 && buf[s - 1] != '\n')
		s--;
	size_t len = n - s;
	if (len >= cap)
		len = cap - 1;
	memcpy(out, buf + s, len);
	out[len] = 0;
}

/* A Flatpak application ID: dotted, [A-Za-z0-9_.-], at most 79 characters, no leading '.' or '-' (so it
 * can never look like an option). */
static int valid_appid(const char *s)
{
	size_t n = strlen(s);
	if (n < 3 || n > 79 || s[0] == '.' || s[0] == '-' || !strchr(s, '.'))
		return 0;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
		      c == '.' || c == '-'))
			return 0;
	}
	return 1;
}

/* What names a package: its Flatpak ID, or its flpkg name. */
static const char *pkey(const struct pkg *p)
{
	return p->id ? p->id : p->name;
}

static void pkgs_free(struct pkg *v, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		free(v[i].name);
		free(v[i].ver);
		free(v[i].sum);
		free(v[i].id);
	}
	free(v);
}

static void pkg_free_all(struct F *f)
{
	if (f->shallow)
		free(f->all);
	else
		pkgs_free(f->all, f->nall);
	f->shallow = 0;
	f->all = NULL;
	f->nall = 0;
	free(f->shown);
	f->shown = NULL;
	f->nshown = 0;
}

static int cmp_pkg(const void *a, const void *b)
{
	return strcmp(((const struct pkg *)a)->name, ((const struct pkg *)b)->name);
}

static int cmp_pkg_ci(const void *a, const void *b)
{
	int r = strcasecmp(((const struct pkg *)a)->name, ((const struct pkg *)b)->name);
	return r ? r : strcmp(((const struct pkg *)a)->id, ((const struct pkg *)b)->id);
}

/* Append a package to the array *v of *n entries. id is the Flatpak ID (NULL for flpkg). */
static void pkg_push(struct pkg **v, size_t *n, const char *name, size_t nl, const char *ver, size_t vl,
                     const char *sum, size_t sl, const char *id, size_t il, int state)
{
	if (nl == 0 || nl >= 80)
		return;
	if (*n % 256 == 0) {
		struct pkg *p = realloc(*v, (*n + 256) * sizeof *p);
		if (!p)
			return;
		*v = p;
	}
	struct pkg *p = &(*v)[(*n)++];
	p->name = strndup(name, nl);
	p->ver = strndup(ver, vl);
	p->sum = strndup(sum, sl);
	p->id = id ? strndup(id, il) : NULL;
	p->state = state;
}

static void pkg_add(struct F *f, const char *name, size_t nl, const char *ver, size_t vl, const char *sum,
                    size_t sl, int state)
{
	pkg_push(&f->all, &f->nall, name, nl, ver, vl, sum, sl, NULL, 0, state);
}

/* ---- the repo setting ------------------------------------------------------------------- */

/* Is a "repo NAME URL" line active in flpkg.conf? */
static int repo_configured(const char *path)
{
	FILE *fp = fopen(path, "r");
	if (!fp)
		return 0;
	char line[512];
	int found = 0;
	while (!found && fgets(line, sizeof line, fp)) {
		const char *s = line + strspn(line, " \t");
		if (!strncmp(s, "repo", 4) && (s[4] == ' ' || s[4] == '\t'))
			found = 1;
	}
	fclose(fp);
	return found;
}

/* ---- child processes ---------------------------------------------------------------------- */

static void job_ready(void *ud, int fd, short rev);

static void job_free_bufs(struct job *j)
{
	for (int i = 0; i < 2; i++) {
		free(j->buf[i]);
		j->buf[i] = NULL;
		j->n[i] = j->cap[i] = 0;
	}
}

static void job_close_fd(struct job *j, int i)
{
	if (j->fd[i] >= 0) {
		fl_fd_remove(j->f->app, j->fd[i]);
		close(j->fd[i]);
		j->fd[i] = -1;
	}
}

static int job_running(const struct job *j)
{
	return j->pid > 0;
}

/* Stop a reading job whose answer is no longer wanted. Never used for changes. */
static void job_abandon(struct job *j)
{
	if (!job_running(j))
		return;
	kill(j->pid, SIGTERM);
	job_close_fd(j, 0);
	job_close_fd(j, 1);
	waitpid(j->pid, NULL, 0);
	j->pid = 0;
}

static int job_start(struct F *f, struct job *j, int kind, char *const argv[], int merge)
{
	job_abandon(j);
	job_free_bufs(j);
	j->f = f;
	j->kind = kind;
	j->finished = 0;
	j->status = 0;
	int p0[2], p1[2] = {-1, -1};
	if (pipe2(p0, O_CLOEXEC) < 0)
		return -1;
	if (!merge && pipe2(p1, O_CLOEXEC) < 0) {
		close(p0[0]);
		close(p0[1]);
		return -1;
	}
	pid_t pid = fork();
	if (pid < 0) {
		close(p0[0]);
		close(p0[1]);
		if (!merge) {
			close(p1[0]);
			close(p1[1]);
		}
		return -1;
	}
	if (pid == 0) {
		int nul = open("/dev/null", O_RDONLY);
		if (nul >= 0)
			dup2(nul, 0);
		dup2(p0[1], 1);
		dup2(merge ? p0[1] : p1[1], 2);
		setenv("FLATPAK_FANCY_OUTPUT", "0", 1); /* plain lines, whatever the environment says */
		execv(argv[0], argv);
		dprintf(2, "cannot run %s: %s%s\n", argv[0], strerror(errno),
		        errno == EACCES && kind == J_CHANGE ? " (your account needs to be in the group seat)" : "");
		_exit(127);
	}
	close(p0[1]);
	if (!merge)
		close(p1[1]);
	j->pid = pid;
	j->fd[0] = p0[0];
	j->fd[1] = merge ? -1 : p1[0];
	for (int i = 0; i < 2; i++)
		if (j->fd[i] >= 0) {
			fcntl(j->fd[i], F_SETFL, fcntl(j->fd[i], F_GETFL) | O_NONBLOCK);
			fl_fd_add(f->app, j->fd[i], POLLIN, job_ready, j);
		}
	return 0;
}

static void job_done(struct F *f, struct job *j);

static void job_ready(void *ud, int fd, short rev)
{
	struct job *j = ud;
	int i = fd == j->fd[0] ? 0 : 1;
	char tmp[4096];
	for (;;) {
		ssize_t r = read(fd, tmp, sizeof tmp);
		if (r > 0) {
			if (j->n[i] + (size_t)r + 1 > j->cap[i]) {
				size_t nc = j->cap[i] ? j->cap[i] * 2 : 8192;
				while (nc < j->n[i] + (size_t)r + 1)
					nc *= 2;
				char *nb = nc <= MAX_BUF ? realloc(j->buf[i], nc) : NULL;
				if (!nb)
					continue; /* over the cap: drop it, keep draining */
				j->buf[i] = nb;
				j->cap[i] = nc;
			}
			memcpy(j->buf[i] + j->n[i], tmp, (size_t)r);
			j->n[i] += (size_t)r;
			j->buf[i][j->n[i]] = 0;
			continue;
		}
		if (r < 0 && (errno == EAGAIN || errno == EINTR)) {
			if (!(rev & (POLLHUP | POLLERR)))
				break;
			break;
		}
		job_close_fd(j, i); /* EOF or error */
		break;
	}
	if (j->kind == J_CHANGE)
		fl_win_redraw(j->f->win);
	if (j->fd[0] < 0 && j->fd[1] < 0)
		job_done(j->f, j);
}

/* ---- flpkg ---------------------------------------------------------------------------------- */

/* Argument vector for a read: flpkg [--config FILE] ARGS... */
static int run_flpkg(struct F *f, int kind, const char *a1, const char *a2, const char *a3)
{
	char *argv[10];
	int n = 0;
	argv[n++] = (char *)f->flpkg;
	if (getenv("FLPACKAGES_CONF")) {
		argv[n++] = "--config";
		argv[n++] = (char *)f->conf;
	}
	argv[n++] = (char *)a1;
	if (a2)
		argv[n++] = (char *)a2;
	if (a3)
		argv[n++] = (char *)a3;
	argv[n] = NULL;
	return job_start(f, &f->job[kind], kind, argv, 0);
}

static int find_pkg(struct F *f, const char *name)
{
	for (size_t i = 0; i < f->nall; i++)
		if (!strcmp(pkey(&f->all[i]), name))
			return (int)i;
	return -1;
}

static const struct pkg *selected(struct F *f)
{
	int i = f->sel[0] ? find_pkg(f, f->sel) : -1;
	return i >= 0 ? &f->all[i] : NULL;
}

static void info_load(struct F *f)
{
	const struct pkg *p = selected(f);
	job_abandon(&f->job[J_INFO]);
	f->nkv = 0;
	f->info_ok = 0;
	f->info_for[0] = 0;
	if (!p || !valid_name(p->name))
		return;
	snprintf(f->info_for, sizeof f->info_for, "%s", p->name);
	if (p->state == ST_AVAIL)
		run_flpkg(f, J_INFO, "info", "--repo", p->name);
	else
		run_flpkg(f, J_INFO, "info", p->name, NULL);
}

/* Details of a Flatpak app: `flatpak info` when it is installed, `remote-info` from the cache when not. */
static void flat_info_load(struct F *f)
{
	const struct pkg *p = selected(f);
	job_abandon(&f->job[J_INFO]);
	f->nkv = 0;
	f->info_ok = 0;
	f->info_for[0] = 0;
	if (!p || !p->id || !valid_appid(p->id))
		return;
	snprintf(f->info_for, sizeof f->info_for, "%s", p->id);
	char *argv[10];
	int n = 0;
	argv[n++] = (char *)f->flatpak;
	if (p->state == ST_AVAIL) {
		argv[n++] = "remote-info";
		argv[n++] = "--user";
		argv[n++] = "--cached";
		argv[n++] = "flathub";
	} else {
		argv[n++] = "info";
		argv[n++] = "--user";
	}
	argv[n++] = f->info_for;
	argv[n] = NULL;
	job_start(f, &f->job[J_INFO], J_INFO, argv, 0);
}

static void apply_filter(struct F *f)
{
	free(f->shown);
	f->nshown = 0;
	f->shown = f->nall ? malloc(f->nall * sizeof(int)) : NULL;
	char q[128];
	snprintf(q, sizeof q, "%s", f->query);
	char *words[8];
	int nw = 0;
	if (f->tab != TAB_AVAIL) {
		for (char *w = strtok(q, " \t"); w && nw < 8; w = strtok(NULL, " \t"))
			words[nw++] = w;
	}
	for (size_t i = 0; f->shown && i < f->nall; i++) {
		const struct pkg *p = &f->all[i];
		int ok = 1;
		for (int w = 0; w < nw && ok; w++) {
			ok = strcasestr(p->name, words[w]) != NULL;
			if (!ok && f->tab == TAB_FLAT)
				ok = strcasestr(p->id, words[w]) != NULL || strcasestr(p->sum, words[w]) != NULL;
		}
		if (ok)
			f->shown[f->nshown++] = (int)i;
	}
	f->ls.sel = 0;
	f->ls.scroll = 0;
}

/* ---- Flathub ----------------------------------------------------------------------------------- */

static void start_flat_change(struct F *f, int kind);
static void flat_start(struct F *f);

/* Run `flatpak ARGS...` (the NULL ends the list) as the job KIND. */
static int flat_run(struct F *f, int kind, int merge, ...)
{
	char *argv[18];
	int n = 0;
	argv[n++] = (char *)f->flatpak;
	va_list ap;
	va_start(ap, merge);
	const char *a;
	while (n < 17 && (a = va_arg(ap, const char *)))
		argv[n++] = (char *)a;
	va_end(ap);
	argv[n] = NULL;
	return job_start(f, &f->job[kind], kind, argv, merge);
}

static int flat_state(const struct F *f, const char *id)
{
	int st = ST_AVAIL;
	for (size_t i = 0; i < f->nfin; i++)
		if (!strcmp(f->fin[i].id, id)) {
			st = ST_INST;
			break;
		}
	if (st == ST_INST)
		for (size_t i = 0; i < f->nupd; i++)
			if (!strcmp(f->upd[i], id))
				return ST_OLDER;
	return st;
}

/* The list on screen is a view on the catalog or on the installed apps, with the state worked out. */
static void flat_rebuild(struct F *f)
{
	if (f->tab != TAB_FLAT) /* the list on screen belongs to another tab: the data waits */
		return;
	char keep[80];
	snprintf(keep, sizeof keep, "%s", f->sel);
	fl_list_state was = f->ls;
	pkg_free_all(f);
	const struct pkg *src = f->fmode == FM_BROWSE ? f->cat : f->fin;
	size_t n = f->fmode == FM_BROWSE ? f->ncat : f->nfin;
	f->all = n ? malloc(n * sizeof *f->all) : NULL;
	f->shallow = 1;
	for (size_t i = 0; f->all && i < n; i++) {
		f->all[f->nall] = src[i];
		f->all[f->nall++].state = flat_state(f, src[i].id);
	}
	f->loaded = f->fmode == FM_BROWSE ? f->cat_loaded : f->fin_loaded;
	apply_filter(f);
	if (keep[0] && find_pkg(f, keep) < 0)
		f->sel[0] = 0;
	for (int i = 0; i < f->nshown && f->sel[0]; i++)
		if (!strcmp(pkey(&f->all[f->shown[i]]), f->sel)) {
			f->ls.sel = i;
			break;
		}
	if (f->nshown) /* new data arriving must not throw the list back to the top */
		f->ls.scroll = was.scroll;
	if (f->sel[0] && strcmp(f->info_for, f->sel))
		flat_info_load(f);
	if (!f->sel[0]) {
		f->nkv = f->info_ok = 0;
		f->info_for[0] = 0;
	}
	fl_win_redraw(f->win);
}

/* The list on screen holds copies of the catalog's and the installed apps' entries (not their strings):
 * it goes before they do. */
static void flat_drop_view(struct F *f)
{
	if (f->shallow)
		pkg_free_all(f);
}

static void flat_clear_cat(struct F *f)
{
	flat_drop_view(f);
	pkgs_free(f->cat, f->ncat);
	f->cat = NULL;
	f->ncat = 0;
}

static void flat_load_cat(struct F *f, int cached)
{
	f->cat_cached = cached;
	f->cat_err[0] = 0;
	if (cached)
		flat_run(f, J_CAT, 0, "remote-ls", "--user", "--app", "--cached",
		         "--columns=application,name,version,description", "flathub", NULL);
	else
		flat_run(f, J_CAT, 0, "remote-ls", "--user", "--app",
		         "--columns=application,name,version,description", "flathub", NULL);
}

static void flat_load_fin(struct F *f)
{
	flat_run(f, J_FINST, 0, "list", "--user", "--app", "--columns=application,name,version,description", NULL);
}

static void flat_load_upd(struct F *f)
{
	flat_run(f, J_FUPD, 0, "remote-ls", "--user", "--app", "--updates", "--cached", "--columns=application",
	         "flathub", NULL);
}

/* Has `flatpak update --appstream` ever run for flathub? Without it names and descriptions are empty. */
static int flat_has_appstream(void)
{
	const char *xdg = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
	char path[512];
	if (xdg && *xdg)
		snprintf(path, sizeof path, "%s/flatpak/appstream/flathub", xdg);
	else if (h && *h)
		snprintf(path, sizeof path, "%s/.local/share/flatpak/appstream/flathub", h);
	else
		return 1;
	return access(path, F_OK) == 0;
}

static void flat_after_remote(struct F *f)
{
	flat_load_fin(f);
	if (!f->cat_loaded) {
		int refreshing = job_running(&f->job[J_CHANGE]) && f->chg_kind == CK_FREFRESH;
		if (refreshing || job_running(&f->job[J_CAT])) {
			/* on its way */
		} else if (!f->fl_refreshed && !flat_has_appstream() && !job_running(&f->job[J_CHANGE])) {
			f->fl_refreshed = 1;
			start_flat_change(f, CK_FREFRESH);
		} else {
			flat_load_cat(f, 1);
		}
	} else {
		flat_load_upd(f);
	}
}

/* Entering the tab, or coming back to the window: what is installed is read again, the catalog only once. */
static void flat_start(struct F *f)
{
	f->fl_have = access(f->flatpak, X_OK) == 0;
	if (!f->fl_have) {
		f->loaded = 1;
		return;
	}
	f->loaded = 0;
	if (f->cat_loaded || f->fin_loaded)
		flat_rebuild(f);
	if (f->fl_remote == 1)
		flat_after_remote(f);
	else
		flat_run(f, J_REM, 0, "remotes", "--user", "--columns=name", NULL);
}

/* Tab-separated lines: ID, name, version, description (the last ones may be empty). */
static void parse_flat(struct pkg **v, size_t *n, const char *buf)
{
	for (const char *s = buf; *s;) {
		const char *e = strchr(s, '\n');
		size_t len = e ? (size_t)(e - s) : strlen(s);
		const char *fld[4] = {s, "", "", ""};
		size_t fl[4] = {len, 0, 0, 0};
		int nf = 1;
		for (size_t i = 0, st = 0; i < len && nf < 4; i++)
			if (s[i] == '\t') {
				fl[nf - 1] = i - st;
				st = i + 1;
				fld[nf] = s + st;
				fl[nf] = len - st;
				nf++;
			}
		char id[96];
		snprintf(id, sizeof id, "%.*s", (int)(fl[0] < 95 ? fl[0] : 95), fld[0]);
		if (valid_appid(id)) {
			const char *name = fl[1] ? fld[1] : fld[0];
			size_t nl = fl[1] ? fl[1] : fl[0];
			pkg_push(v, n, name, nl, fld[2], fl[2], fld[3], fl[3], fld[0], fl[0], ST_AVAIL);
		}
		s += len + (e ? 1 : 0);
	}
	if (*n > 1)
		qsort(*v, *n, sizeof **v, cmp_pkg_ci);
}

static void parse_updates(struct F *f, const char *buf)
{
	for (size_t i = 0; i < f->nupd; i++)
		free(f->upd[i]);
	free(f->upd);
	f->upd = NULL;
	f->nupd = 0;
	for (const char *s = buf; *s;) {
		const char *e = strchr(s, '\n');
		size_t len = e ? (size_t)(e - s) : strlen(s);
		if (len > 2 && len < 80) {
			char id[96];
			snprintf(id, sizeof id, "%.*s", (int)len, s);
			if (valid_appid(id)) {
				char **nv = realloc(f->upd, (f->nupd + 1) * sizeof *nv);
				if (nv) {
					f->upd = nv;
					f->upd[f->nupd++] = strdup(id);
				}
			}
		}
		s += len + (e ? 1 : 0);
	}
}

/* `flatpak info` and `remote-info`: right-aligned "Key: value" lines after a title line. */
static void parse_finfo(struct F *f, const char *buf)
{
	static const char *const keep[] = {"ID", "Version", "License", "Branch", "Origin", "Download", "Installed",
	                                   "Runtime", "Date", NULL};
	f->nkv = 0;
	for (const char *s = buf; *s && f->nkv < MAX_KV;) {
		const char *e = strchr(s, '\n');
		size_t len = e ? (size_t)(e - s) : strlen(s);
		const char *k = s;
		while (k < s + len && *k == ' ')
			k++;
		const char *colon = memchr(k, ':', len - (size_t)(k - s));
		if (colon && colon[1] == ' ' && colon - k > 0 && colon - k < 20) {
			char key[24];
			snprintf(key, sizeof key, "%.*s", (int)(colon - k), k);
			for (int i = 0; keep[i]; i++)
				if (!strcmp(key, keep[i]) && f->nkv < MAX_KV) {
					struct kv *kv = &f->kv[f->nkv++];
					snprintf(kv->key, sizeof kv->key, "%s", !strcmp(key, "Installed") ? "On disk" : key);
					const char *v = colon + 2;
					size_t vl = len - (size_t)(v - s);
					if (vl >= sizeof kv->val)
						vl = sizeof kv->val - 1;
					memcpy(kv->val, v, vl);
					kv->val[vl] = 0;
				}
		}
		s += len + (e ? 1 : 0);
	}
}

static void flat_job_done(struct F *f, struct job *j)
{
	const char *out = j->buf[0] ? j->buf[0] : "";
	switch (j->kind) {
	case J_REM: {
		f->fl_remote = 0;
		for (const char *s = out; *s;) {
			const char *e = strchr(s, '\n');
			size_t len = e ? (size_t)(e - s) : strlen(s);
			if (len == 7 && !strncmp(s, "flathub", 7))
				f->fl_remote = 1;
			s += len + (e ? 1 : 0);
		}
		if (j->status != 0)
			f->fl_remote = 0;
		if (f->fl_remote == 1) {
			flat_after_remote(f);
		} else {
			if (f->tab == TAB_FLAT)
				f->loaded = 1;
			f->cat_loaded = 0;
			flat_rebuild(f);
		}
		break;
	}
	case J_CAT:
		if (j->status == 0 && strchr(out, '\n')) {
			flat_clear_cat(f);
			parse_flat(&f->cat, &f->ncat, out);
		}
		if (f->ncat == 0 && f->cat_cached) {
			flat_load_cat(f, 0); /* nothing cached yet: ask the network */
			break;
		}
		if (f->ncat == 0) {
			last_line(j->buf[1] ? j->buf[1] : "", j->n[1], f->cat_err, sizeof f->cat_err);
			if (!f->cat_err[0])
				snprintf(f->cat_err, sizeof f->cat_err, "flatpak stopped with status %d", j->status);
		}
		f->cat_loaded = 1;
		flat_load_upd(f);
		flat_rebuild(f);
		break;
	case J_FINST:
		flat_drop_view(f);
		pkgs_free(f->fin, f->nfin);
		f->fin = NULL;
		f->nfin = 0;
		if (j->status == 0)
			parse_flat(&f->fin, &f->nfin, out);
		f->fin_loaded = 1;
		flat_rebuild(f);
		break;
	case J_FUPD:
		if (j->status == 0) {
			parse_updates(f, out);
			flat_rebuild(f);
		}
		break;
	}
}

static void reload(struct F *f)
{
	if (f->tab == TAB_FLAT) {
		flat_start(f);
		return;
	}
	f->repo = repo_configured(f->conf);
	job_abandon(&f->job[J_LIST]);
	f->loaded = 0;
	f->list_err[0] = 0;
	if (f->tab == TAB_AVAIL) {
		if (!f->repo) {
			pkg_free_all(f);
			f->loaded = 1;
			fl_win_redraw(f->win);
			return;
		}
		snprintf(f->last_query, sizeof f->last_query, "%s", f->query);
		char q[128];
		snprintf(q, sizeof q, "%s", f->query);
		/* words are AND-ed by flpkg; an empty search lists everything */
		char *words[8];
		int nw = 0;
		for (char *w = strtok(q, " \t"); w && nw < 8; w = strtok(NULL, " \t"))
			words[nw++] = w;
		char *argv[16];
		int n = 0;
		argv[n++] = (char *)f->flpkg;
		if (getenv("FLPACKAGES_CONF")) {
			argv[n++] = "--config";
			argv[n++] = (char *)f->conf;
		}
		argv[n++] = "search";
		if (!nw)
			argv[n++] = "";
		for (int i = 0; i < nw; i++)
			argv[n++] = words[i];
		argv[n] = NULL;
		job_start(f, &f->job[J_LIST], J_LIST, argv, 0);
	} else {
		run_flpkg(f, J_LIST, "list", NULL, NULL);
	}
	fl_win_redraw(f->win);
}

static void parse_list(struct F *f, const char *buf)
{
	for (const char *s = buf; *s;) {
		const char *e = strchr(s, '\n');
		size_t len = e ? (size_t)(e - s) : strlen(s);
		const char *sp = memchr(s, ' ', len);
		if (sp)
			pkg_add(f, s, (size_t)(sp - s), sp + 1, len - (size_t)(sp - s) - 1, "", 0, ST_INST);
		s += len + (e ? 1 : 0);
	}
}

static void parse_search(struct F *f, const char *buf)
{
	for (const char *s = buf; *s;) {
		const char *e = strchr(s, '\n');
		size_t len = e ? (size_t)(e - s) : strlen(s);
		if (s[0] != ' ' && s[0] != '\t') {
			const char *slash = memchr(s, '/', len), *sp = memchr(s, ' ', len);
			if (slash && sp && slash < sp) {
				const char *name = slash + 1, *ver = sp + 1;
				const char *vend = memchr(ver, ' ', len - (size_t)(ver - s));
				size_t vl = vend ? (size_t)(vend - ver) : len - (size_t)(ver - s);
				int state = ST_AVAIL;
				if (vend && strstr(vend, "[installed: older]"))
					state = ST_OLDER;
				else if (vend && strstr(vend, "[installed]"))
					state = ST_INST;
				/* the summary is on the next line, indented */
				const char *next = e ? e + 1 : NULL, *sum = "";
				size_t sl = 0;
				if (next && (next[0] == ' ' || next[0] == '\t')) {
					sum = next + strspn(next, " \t");
					const char *ne = strchr(sum, '\n');
					sl = ne ? (size_t)(ne - sum) : strlen(sum);
				}
				pkg_add(f, name, (size_t)(sp - name), ver, vl, sum, sl, state);
			}
		}
		s += len + (e ? 1 : 0);
	}
}

static void parse_info(struct F *f, const char *buf)
{
	f->nkv = 0;
	for (const char *s = buf; *s && f->nkv < MAX_KV;) {
		const char *e = strchr(s, '\n');
		size_t len = e ? (size_t)(e - s) : strlen(s);
		const char *colon = memchr(s, ':', len);
		if (colon && colon > s && colon[1] == ' ') {
			struct kv *kv = &f->kv[f->nkv++];
			size_t kl = (size_t)(colon - s);
			while (kl && s[kl - 1] == ' ')
				kl--;
			if (kl >= sizeof kv->key)
				kl = sizeof kv->key - 1;
			memcpy(kv->key, s, kl);
			kv->key[kl] = 0;
			const char *v = colon + 2;
			size_t vl = len - (size_t)(v - s);
			if (vl >= sizeof kv->val)
				vl = sizeof kv->val - 1;
			memcpy(kv->val, v, vl);
			kv->val[vl] = 0;
		}
		s += len + (e ? 1 : 0);
	}
}

static void job_done(struct F *f, struct job *j)
{
	int st = 0;
	if (j->pid > 0)
		waitpid(j->pid, &st, 0);
	j->pid = 0;
	j->status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
	j->finished = 1;
	const char *out = j->buf[0] ? j->buf[0] : "";
	if (j->kind >= J_REM) {
		flat_job_done(f, j);
		fl_win_redraw(f->win);
		return;
	}
	if (j->kind == J_LIST) {
		pkg_free_all(f);
		f->list_err[0] = 0;
		if (j->status == 0) {
			if (f->tab == TAB_AVAIL)
				parse_search(f, out);
			else
				parse_list(f, out);
			if (f->nall > 1)
				qsort(f->all, f->nall, sizeof *f->all, cmp_pkg);
		}
		if (j->buf[1])
			last_line(j->buf[1], j->n[1], f->list_err, sizeof f->list_err);
		if (j->status && !f->list_err[0])
			snprintf(f->list_err, sizeof f->list_err, "flpkg stopped with status %d", j->status);
		f->loaded = 1;
		apply_filter(f);
		if (f->sel[0] && find_pkg(f, f->sel) < 0)
			f->sel[0] = 0;
		/* keep the selection, or follow it: show the right package in the list */
		for (int i = 0; i < f->nshown && f->sel[0]; i++)
			if (!strcmp(f->all[f->shown[i]].name, f->sel)) {
				f->ls.sel = i;
				break;
			}
		info_load(f);
	} else if (j->kind == J_INFO) {
		if (!strcmp(f->info_for, f->sel) && j->status == 0) {
			if (f->tab == TAB_FLAT)
				parse_finfo(f, out);
			else
				parse_info(f, out);
			f->info_ok = 1;
		}
	} else if (j->kind == J_CHANGE) {
		f->chg_failed = j->status != 0;
		if (f->chg_failed) {
			char was[160];
			snprintf(was, sizeof was, "%s", f->chg_what);
			snprintf(f->chg_what, sizeof f->chg_what, "%s failed (status %d)", was, j->status);
		} else {
			snprintf(f->chg_what, sizeof f->chg_what, "%s", f->chg_done);
		}
		fl_timer_arm(f->anim_timer, 0, 0);
		if (f->chg_failed) {
			char line[300];
			last_line(j->buf[0] ? j->buf[0] : "", j->n[0], line, sizeof line);
			say(f, 1, "%s", line[0] ? line : "The change failed");
		} else {
			say(f, 0, "%s", f->chg_done);
		}
		if (!f->chg_failed && (f->chg_kind == CK_FREFRESH || f->chg_kind == CK_FADDREMOTE))
			f->chg_visible = 0; /* housekeeping that worked: nothing to read */
		switch (f->chg_kind) {
		case CK_PKG:
			reload(f);
			break;
		case CK_FREFRESH: /* the catalog is read again: from the network when the refresh worked */
			f->cat_loaded = 0;
			if (f->tab == TAB_FLAT && f->fl_have)
				flat_load_cat(f, f->chg_failed);
			break;
		case CK_FADDREMOTE:
			f->fl_remote = -1;
			if (f->tab == TAB_FLAT)
				flat_start(f);
			break;
		default: /* an app came, went or was updated: the catalog does not change */
			if (f->tab == TAB_FLAT) {
				flat_load_fin(f);
				flat_load_upd(f);
				if (f->sel[0])
					flat_info_load(f);
			}
			break;
		}
	}
	fl_win_redraw(f->win);
}

/* ---- changes ---------------------------------------------------------------------------------- */

static void start_change(struct F *f, const char *verb)
{
	const struct pkg *p = selected(f);
	if (!p || job_running(&f->job[J_CHANGE]) || !valid_name(p->name))
		return;
	snprintf(f->chg_name, sizeof f->chg_name, "%s", p->name);
	char *argv[4] = {(char *)f->seat, (char *)verb, f->chg_name, NULL};
	/* the log shows what runs, then flpkg's own words */
	if (job_start(f, &f->job[J_CHANGE], J_CHANGE, argv, 1) < 0) {
		say(f, 1, "Cannot start %s: %s", f->seat, strerror(errno));
		return;
	}
	snprintf(f->chg_what, sizeof f->chg_what, "%s %s", !strcmp(verb, "install") ? "Installing" : "Removing",
	         f->chg_name);
	f->chg_kind = CK_PKG;
	snprintf(f->chg_done, sizeof f->chg_done, "Done: %s", f->chg_name);
	f->chg_visible = 1;
	f->chg_failed = 0;
	f->ask_remove = 0;
	fl_timer_arm(f->anim_timer, 60, 60);
}

/* A Flatpak change, as the person: no helper, no root. The ID goes to flatpak as one argument, and only
 * after valid_appid() (which also rules out anything that looks like an option). */
#define ADD(...)                                                                                           \
	do {                                                                                               \
		const char *l_[] = {__VA_ARGS__};                                                          \
		for (size_t i_ = 0; i_ < sizeof l_ / sizeof *l_; i_++)                                     \
			argv[n++] = (char *)l_[i_];                                                        \
	} while (0)

static void start_flat_change(struct F *f, int kind)
{
	const struct pkg *p = kind <= CK_FUPDATE ? selected(f) : NULL;
	if (job_running(&f->job[J_CHANGE]) || !f->fl_have)
		return;
	if (kind <= CK_FUPDATE && (!p || !p->id || !valid_appid(p->id)))
		return;
	char *argv[12];
	int n = 0;
	const char *what = "";
	argv[n++] = (char *)f->flatpak;
	switch (kind) {
	case CK_FINSTALL:
		ADD("install", "--user", "-y", "--noninteractive", "flathub");
		argv[n++] = p->id;
		what = "Installing";
		break;
	case CK_FREMOVE:
		ADD("uninstall", "--user", "-y", "--noninteractive");
		argv[n++] = p->id;
		what = "Removing";
		break;
	case CK_FUPDATE:
		ADD("update", "--user", "-y", "--noninteractive");
		argv[n++] = p->id;
		what = "Updating";
		break;
	case CK_FUPDATE_ALL:
		ADD("update", "--user", "-y", "--noninteractive");
		what = "Updating all apps";
		break;
	case CK_FUNUSED:
		ADD("uninstall", "--user", "--unused", "-y", "--noninteractive");
		what = "Removing unused runtimes";
		break;
	case CK_FREFRESH:
		ADD("update", "--user", "--appstream", "--noninteractive", "flathub");
		what = "Refreshing the Flathub catalog";
		break;
	case CK_FADDREMOTE:
		ADD("remote-add", "--user", "--if-not-exists", "flathub", FLATHUB_URL);
		what = "Adding Flathub";
		break;
	default:
		return;
	}
	argv[n] = NULL;
	if (job_start(f, &f->job[J_CHANGE], J_CHANGE, argv, 1) < 0) {
		say(f, 1, "Cannot start %s: %s", f->flatpak, strerror(errno));
		return;
	}
	f->chg_kind = kind;
	if (p) {
		snprintf(f->chg_name, sizeof f->chg_name, "%s", p->name);
		snprintf(f->chg_what, sizeof f->chg_what, "%s %s", what, p->name);
		snprintf(f->chg_done, sizeof f->chg_done, "Done: %s", p->name);
	} else {
		static const char *const done[] = {[CK_FUPDATE_ALL] = "All apps are up to date",
		                                   [CK_FUNUSED] = "Unused runtimes removed",
		                                   [CK_FREFRESH] = "Flathub catalog updated",
		                                   [CK_FADDREMOTE] = "Flathub added"};
		snprintf(f->chg_name, sizeof f->chg_name, "%s", what);
		snprintf(f->chg_what, sizeof f->chg_what, "%s", what);
		snprintf(f->chg_done, sizeof f->chg_done, "%s", done[kind]);
	}
	f->chg_visible = 1;
	f->chg_failed = 0;
	f->ask_remove = 0;
	fl_timer_arm(f->anim_timer, 60, 60);
}
#undef ADD

/* Start an installed app, detached (a double fork: nothing is left to wait for). */
static void flat_open(struct F *f)
{
	const struct pkg *p = selected(f);
	if (!p || !p->id || !valid_appid(p->id) || !f->fl_have)
		return;
	pid_t pid = fork();
	if (pid < 0)
		return;
	if (pid == 0) {
		if (fork() != 0)
			_exit(0);
		setsid();
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) {
			dup2(nul, 0);
			dup2(nul, 1);
			dup2(nul, 2);
		}
		execl(f->flatpak, f->flatpak, "run", p->id, (char *)NULL);
		_exit(127);
	}
	waitpid(pid, NULL, 0);
	say(f, 0, "Starting %s", p->name);
}

static void anim_tick(void *ud)
{
	fl_win_redraw(((struct F *)ud)->win);
}

static void search_fire(void *ud)
{
	struct F *f = ud;
	fl_timer_arm(f->search_timer, 0, 0);
	if (f->tab == TAB_AVAIL && strcmp(f->query, f->last_query))
		reload(f);
}

static void select_index(struct F *f, int shown_i)
{
	if (shown_i < 0 || shown_i >= f->nshown)
		return;
	const struct pkg *p = &f->all[f->shown[shown_i]];
	snprintf(f->sel, sizeof f->sel, "%s", pkey(p));
	f->ask_remove = 0;
	if (f->tab == TAB_FLAT)
		flat_info_load(f);
	else
		info_load(f);
}

static void run_action(struct F *f)
{
	int act = f->act, arg = f->act_arg;
	f->act = 0;
	switch (act) {
	case A_TAB_INST:
	case A_TAB_AVAIL:
	case A_TAB_FLAT: {
		int t = act == A_TAB_INST ? TAB_INST : act == A_TAB_AVAIL ? TAB_AVAIL : TAB_FLAT;
		if (t != f->tab) {
			f->tab = t;
			f->sel[0] = 0;
			f->nkv = f->info_ok = 0;
			f->ask_remove = 0;
			pkg_free_all(f);
			reload(f);
		}
		break;
	}
	case A_FILTER:
		if (f->tab != TAB_AVAIL)
			apply_filter(f);
		else
			fl_timer_arm(f->search_timer, 300, 0);
		break;
	case A_SELECT: select_index(f, arg); break;
	case A_INSTALL:
		if (f->tab == TAB_FLAT) {
			const struct pkg *p = selected(f);
			start_flat_change(f, p && p->state == ST_OLDER ? CK_FUPDATE : CK_FINSTALL);
		} else {
			start_change(f, "install");
		}
		break;
	case A_ASK_REMOVE: f->ask_remove = 1; break;
	case A_CANCEL: f->ask_remove = 0; break;
	case A_REMOVE:
		if (f->tab == TAB_FLAT)
			start_flat_change(f, CK_FREMOVE);
		else
			start_change(f, "remove");
		break;
	case A_OPEN: flat_open(f); break;
	case A_FM_BROWSE:
	case A_FM_INST: {
		int m = act == A_FM_BROWSE ? FM_BROWSE : FM_INST;
		if (m != f->fmode) {
			f->fmode = m;
			f->sel[0] = 0;
			f->nkv = f->info_ok = 0;
			f->ask_remove = 0;
			f->ls = (fl_list_state){0};
			flat_rebuild(f);
		}
		break;
	}
	case A_SIDE: /* the strip's button: refresh the catalog, update everything, or remove unused runtimes */
		if (f->fmode == FM_BROWSE)
			start_flat_change(f, CK_FREFRESH);
		else
			start_flat_change(f, f->nupd ? CK_FUPDATE_ALL : CK_FUNUSED);
		break;
	case A_ADD_REMOTE: start_flat_change(f, CK_FADDREMOTE); break;
	case A_LOGCLOSE:
		if (!job_running(&f->job[J_CHANGE]))
			f->chg_visible = 0;
		break;
	}
	fl_win_redraw(f->win);
}

/* ---- drawing ------------------------------------------------------------------------------------ */

/* Wrap s into r.w at words, up to max_lines; returns the height used. */
static int wrap(fl_ui *ui, fl_font *font, fl_rect r, fl_color c, const char *s, int max_lines)
{
	int lh = fl_font_height(font) + 3, lines = 0;
	size_t len = strlen(s), start = 0;
	while (start < len && lines < max_lines) {
		while (start < len && s[start] == ' ')
			start++;
		size_t end = start, best = start;
		while (end < len) {
			size_t w = end;
			while (w < len && s[w] != ' ')
				w++;
			if (best > start && fl_text_width(font, s + start, (int)(w - start)) > r.w)
				break;
			best = w;
			end = w;
			while (end < len && s[end] == ' ')
				end++;
		}
		if (best == start)
			best = len; /* one word wider than the line: the clip cuts it */
		fl_rect save = ui->cv.clip;
		fl_clip(&ui->cv, fl_r(r.x, r.y + lines * lh, r.w, lh));
		fl_text(&ui->cv, font, r.x, r.y + lines * lh, c, s + start, (int)(best - start));
		ui->cv.clip = save;
		lines++;
		start = best;
	}
	return lines * lh;
}

static const char *state_text(int st)
{
	return st == ST_OLDER ? "update available" : st == ST_INST ? "installed" : "";
}

static void draw_row(fl_ui *ui, fl_rect r, int i, int hover, int selected_row, void *ud)
{
	struct F *f = ud;
	const struct pkg *p = &f->all[f->shown[i]];
	fl_canvas *cv = &ui->cv;
	int cur = !strcmp(pkey(p), f->sel);
	fl_rect row = fl_r(r.x, r.y, r.w, ROW_H);
	if (cur)
		fl_fill_rect(cv, row, fl_th.select);
	else if (hover || selected_row)
		fl_fill_rect(cv, row, fl_th.raised);
	if (cur)
		fl_fill_rect(cv, fl_r(row.x, row.y, 3, row.h), fl_th.accent);
	int vw = fl_text_width(f->small, p->ver, -1);
	int nh = fl_font_height(ui->f_med), sh = fl_font_height(f->small);
	const char *second = p->sum[0] ? p->sum : "";
	char tag[32] = "";
	/* Available and the Flathub catalog mark what is installed; the installed apps only an update */
	if (p->state != ST_AVAIL && (f->tab == TAB_AVAIL || (f->tab == TAB_FLAT &&
	    (f->fmode == FM_BROWSE || p->state == ST_OLDER))))
		snprintf(tag, sizeof tag, "%s", state_text(p->state));
	int two = second[0] || tag[0];
	int ty = row.y + (two ? (ROW_H - nh - 2 - sh) / 2 : (ROW_H - nh) / 2);
	fl_rect save = cv->clip;
	fl_clip(cv, fl_r(row.x, row.y, row.w, row.h));
	fl_text(cv, f->small, row.x + row.w - 14 - vw, ty + 2, fl_th.dim, p->ver, -1);
	fl_rect nr = fl_r(row.x + 16, ty, row.w - 16 - vw - 28, nh + 2);
	fl_text_in(cv, ui->f_med, nr, FL_LEFT, 0, fl_th.strong, p->name);
	if (two) {
		int tw = tag[0] ? fl_text_width(f->small, tag, -1) : 0;
		if (tag[0])
			fl_text(cv, f->small, row.x + row.w - 14 - tw, ty + nh + 2, fl_th.accent, tag, -1);
		if (second[0]) {
			char fit[300];
			fl_text_fit(f->small, second, row.w - 32 - (tw ? tw + 12 : 0), fit, sizeof fit);
			fl_text(cv, f->small, row.x + 16, ty + nh + 2, fl_th.dim, fit, -1);
		}
	}
	cv->clip = save;
}

static void draw_toolbar(fl_ui *ui, struct F *f, fl_rect tb)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, tb, fl_th.panel);
	fl_fill_rect(cv, fl_r(tb.x, tb.y + tb.h - 1, tb.w, 1), fl_th.line);
	int x = tb.x + 8, y = tb.y + (tb.h - 32) / 2;
	if (fl_button(ui, ID_TAB_INST, fl_r(x, y, 96, 32), "Installed",
	              f->tab == TAB_INST ? FL_BTN_ACCENT : FL_BTN_OUTLINE))
		queue(f, A_TAB_INST, 0);
	x += 100;
	if (fl_button(ui, ID_TAB_AVAIL, fl_r(x, y, 96, 32), "Available",
	              f->tab == TAB_AVAIL ? FL_BTN_ACCENT : FL_BTN_OUTLINE))
		queue(f, A_TAB_AVAIL, 0);
	x += 100;
	if (fl_button(ui, ID_TAB_FLAT, fl_r(x, y, 96, 32), "Flathub",
	              f->tab == TAB_FLAT ? FL_BTN_ACCENT : FL_BTN_OUTLINE))
		queue(f, A_TAB_FLAT, 0);
	x += 100 + 8;
	fl_rect sr = fl_r(x, y, tb.x + tb.w - 8 - x, 32);
	const char *hint = f->tab == TAB_INST ? "Search installed packages" :
	                   f->tab == TAB_AVAIL ? "Search the repository" :
	                   f->fmode == FM_BROWSE ? "Search Flathub apps" : "Search installed apps";
	int r = fl_textfield(ui, ID_SEARCH, sr, f->query, sizeof f->query, hint, ui->f_reg);
	if (r & FL_EDIT_CHANGED)
		queue(f, A_FILTER, 0);
	if (r & FL_EDIT_ENTER) {
		if (f->tab == TAB_AVAIL && strcmp(f->query, f->last_query))
			reload(f);
		ui->focus = ID_LIST;
	}
}

static int draw_notice(fl_ui *ui, struct F *f, fl_rect r, const char *head, const char *body)
{
	int y = r.y + 28;
	if (head && head[0])
		y += wrap(ui, ui->f_semi, fl_r(r.x + 20, y, r.w - 40, 0), fl_th.strong, head, 3) + 6;
	if (body && body[0])
		y += wrap(ui, f->small, fl_r(r.x + 20, y, r.w - 40, 0), fl_th.dim, body, 8);
	return y;
}

static void draw_list(fl_ui *ui, struct F *f, fl_rect lr)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, lr, fl_th.bg);
	fl_fill_rect(cv, fl_r(lr.x + lr.w - 1, lr.y, 1, lr.h), fl_th.line);
	fl_rect inner = fl_r(lr.x, lr.y, lr.w - 1, lr.h);
	if (f->tab == TAB_FLAT && !f->fl_have) {
		draw_notice(ui, f, inner, "Flatpak is not installed.",
		            "Flathub apps run through Flatpak. Install the \"flatpak\" package from the Available tab, "
		            "then come back here.");
		return;
	}
	if (f->tab == TAB_FLAT && f->fl_remote == 0) {
		int y = draw_notice(ui, f, inner, "Flathub is not set up yet.",
		                    "Flathub is the app store behind Flatpak. It is added for your account when you "
		                    "log in with a network connection; you can add it now.");
		if (!job_running(&f->job[J_CHANGE]) &&
		    fl_button(ui, ID_ADD_REMOTE, fl_r(inner.x + 20, y + 14, 132, 32), "Add Flathub", FL_BTN_ACCENT))
			queue(f, A_ADD_REMOTE, 0);
		return;
	}
	if (f->tab == TAB_AVAIL && !f->repo) {
		draw_notice(ui, f, inner, "No package repository is configured.",
		            "FreeLFS has nowhere to get packages from yet. Add a line \"repo NAME URL\" to "
		            "/etc/flpkg.conf, then run \"flpkg sync\" as root. The Installed tab works without one.");
		return;
	}
	if (!f->loaded) {
		if (f->tab == TAB_FLAT && f->fmode == FM_BROWSE)
			draw_notice(ui, f, inner, "Loading Flathub...",
			            job_running(&f->job[J_CHANGE]) && f->chg_kind == CK_FREFRESH ?
			            "The catalog is fetched the first time: a few megabytes." : NULL);
		else
			draw_notice(ui, f, inner, "Loading...", NULL);
		return;
	}
	if (f->tab == TAB_FLAT && f->nshown == 0) {
		if (f->fmode == FM_BROWSE && f->cat_err[0])
			draw_notice(ui, f, inner, "Could not read Flathub.", f->cat_err);
		else if (f->query[0])
			draw_notice(ui, f, inner, f->fmode == FM_BROWSE ? "No app on Flathub matches." :
			            "No installed app matches.", NULL);
		else if (f->fmode == FM_INST)
			draw_notice(ui, f, inner, "No Flatpak apps are installed.",
			            "Find one in Browse and press Install. Apps are installed for your account only.");
		else
			draw_notice(ui, f, inner, "Flathub has no apps in the catalog.", NULL);
		return;
	}
	if (f->nshown == 0) {
		if (f->list_err[0])
			draw_notice(ui, f, inner, "Could not read the package list.", f->list_err);
		else if (f->tab == TAB_INST)
			draw_notice(ui, f, inner, f->query[0] ? "No installed package matches." : "Nothing is installed.",
			            NULL);
		else
			draw_notice(ui, f, inner, f->query[0] ? "No package in the repository matches." :
			            "The repository has no packages.", f->query[0] ? NULL :
			            "Run \"flpkg sync\" as root to fetch its index.");
		return;
	}
	int act = fl_list(ui, ID_LIST, inner, f->nshown, ROW_H, &f->ls, draw_row, f);
	if (act >= 0)
		queue(f, A_SELECT, act);
}

static void draw_details(fl_ui *ui, struct F *f, fl_rect dr)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, dr, fl_th.bg);
	const struct pkg *p = selected(f);
	if (!p) {
		fl_text_in(cv, ui->f_reg, dr, FL_CENTER, 0, fl_th.dim,
		           f->nshown ? "Select a package" : "");
		return;
	}
	int x = dr.x + PAD, w = dr.w - 2 * PAD, y = dr.y + PAD;
	if (w < 120)
		return;
	fl_rect save = cv->clip;
	fl_clip(cv, dr);
	fl_text_in(cv, f->title, fl_r(x, y, w, fl_font_height(f->title) + 4), FL_LEFT, 0, fl_th.strong, p->name);
	y += fl_font_height(f->title) + 8;
	char sub[160];
	snprintf(sub, sizeof sub, "%s%s%s", p->ver, p->state ? "   " : "", state_text(p->state));
	fl_text(cv, f->small, x, y, p->state ? fl_th.accent : fl_th.dim, sub, -1);
	y += fl_font_height(f->small) + 14;

	/* the buttons */
	int busy = job_running(&f->job[J_CHANGE]);
	if (busy) {
		fl_text_in(cv, ui->f_reg, fl_r(x, y, w, 32), FL_LEFT, 0, fl_th.dim, "Working...");
	} else if (f->ask_remove) {
		char q[160];
		snprintf(q, sizeof q, "Remove %s?", p->name);
		fl_text_in(cv, ui->f_med, fl_r(x, y, w, 32), FL_LEFT, 0, fl_th.text, q);
		int tw = fl_text_width(ui->f_med, q, -1) + 14;
		if (fl_button(ui, ID_REMOVE, fl_r(x + tw, y, 96, 32), "Remove", FL_BTN_ACCENT))
			queue(f, A_REMOVE, 0);
		if (fl_button(ui, ID_CANCEL, fl_r(x + tw + 104, y, 96, 32), "Cancel", FL_BTN_OUTLINE))
			queue(f, A_CANCEL, 0);
	} else {
		int bx = x;
		int flat = f->tab == TAB_FLAT;
		if (p->state != ST_INST) {
			if (flat ? f->fl_remote != 1 : !f->repo) {
				fl_text_in(cv, ui->f_reg, fl_r(bx, y, w, 32), FL_LEFT, 0, fl_th.dim,
				           flat ? "Flathub is not set up" : "No repository configured");
			} else if (fl_button(ui, ID_INSTALL, fl_r(bx, y, 104, 32), p->state == ST_OLDER ? "Update" : "Install",
			                     FL_BTN_ACCENT)) {
				queue(f, A_INSTALL, 0);
			}
			bx += 112;
		}
		if (flat && p->state != ST_AVAIL) {
			if (fl_button(ui, ID_OPEN, fl_r(bx, y, 104, 32), "Open", p->state == ST_INST ? FL_BTN_ACCENT : FL_BTN_OUTLINE))
				queue(f, A_OPEN, 0);
			bx += 112;
		}
		if (p->state != ST_AVAIL &&
		    fl_button(ui, ID_REMOVE, fl_r(bx, y, 104, 32), "Remove", FL_BTN_OUTLINE))
			queue(f, A_ASK_REMOVE, 0);
	}
	y += 32 + 14;
	fl_fill_rect(cv, fl_r(x, y, w, 1), fl_th.line);
	y += 14;

	if (!f->info_ok) {
		fl_text(cv, f->small, x, y, fl_th.dim, job_running(&f->job[J_INFO]) ? "Loading..." : "No details available", -1);
		cv->clip = save;
		return;
	}
	const char *summary = f->tab == TAB_FLAT ? p->sum : "";
	for (int i = 0; i < f->nkv; i++)
		if (!strcmp(f->kv[i].key, "Summary"))
			summary = f->kv[i].val;
	if (summary[0])
		y += wrap(ui, ui->f_reg, fl_r(x, y, w, 0), fl_th.text, summary, 3) + 12;
	int kw = 112;
	for (int i = 0; i < f->nkv && y < dr.y + dr.h; i++) {
		const struct kv *kv = &f->kv[i];
		if (!strcmp(kv->key, "Name") || !strcmp(kv->key, "Version") || !strcmp(kv->key, "Summary") ||
		    !strcmp(kv->key, "Base") || !strcmp(kv->val, "None") || !kv->val[0])
			continue;
		fl_text_ex(cv, f->head, x, y + 2, fl_th.dim, kv->key, -1, 0);
		int h = wrap(ui, f->small, fl_r(x + kw, y, w - kw, 0), fl_th.text, kv->val, 3);
		y += (h > fl_font_height(f->small) + 3 ? h : fl_font_height(f->small) + 3) + 6;
	}
	cv->clip = save;
}

/* flatpak's plain output says "Installing 2/4..." for each step: find the last "N/M" in the output. */
static int steps_of(const char *buf, size_t n, int *cur, int *total)
{
	for (size_t i = n, stop = n > 600 ? n - 600 : 0; i-- > stop + 1;) {
		if (buf[i] != '/' || buf[i - 1] < '0' || buf[i - 1] > '9' || i + 1 >= n || buf[i + 1] < '0' || buf[i + 1] > '9')
			continue;
		size_t a = i;
		while (a > 0 && buf[a - 1] >= '0' && buf[a - 1] <= '9')
			a--;
		size_t b = i + 1;
		while (b < n && buf[b] >= '0' && buf[b] <= '9')
			b++;
		if (i - a > 3 || b - i - 1 > 3)
			continue;
		*cur = atoi(buf + a);
		*total = atoi(buf + i + 1);
		return *total > 0 && *cur > 0 && *cur <= *total;
	}
	return 0;
}

static void draw_log(fl_ui *ui, struct F *f, fl_rect lg)
{
	fl_canvas *cv = &ui->cv;
	struct job *j = &f->job[J_CHANGE];
	int run = job_running(j);
	fl_fill_rect(cv, lg, fl_th.panel);
	fl_fill_rect(cv, fl_r(lg.x, lg.y, lg.w, 1), fl_th.line);
	fl_rect head = fl_r(lg.x, lg.y + 1, lg.w, 34);
	int bw = 76;
	int step = 0, steps = 0;
	if (f->chg_kind != CK_PKG && run && j->buf[0])
		steps_of(j->buf[0], j->n[0], &step, &steps);
	if (steps && run) {
		char st[32];
		snprintf(st, sizeof st, "step %d of %d", step, steps);
		int sw = fl_text_width(f->small, st, -1);
		fl_text(cv, f->small, head.x + head.w - 16 - sw, head.y + (head.h - fl_font_height(f->small)) / 2, fl_th.dim,
		        st, -1);
	}
	fl_text_in(cv, ui->f_semi, fl_r(head.x, head.y, head.w - bw - 16 - (steps && run ? 90 : 0), head.h), FL_LEFT, 16,
	           f->chg_failed ? fl_th.err : fl_th.strong, f->chg_what);
	if (!run && fl_button(ui, ID_LOGCLOSE, fl_r(head.x + head.w - bw - 8, head.y + 3, bw, 28), "Close",
	                      FL_BTN_OUTLINE))
		queue(f, A_LOGCLOSE, 0);
	/* progress: a segment running along while it works, a full bar after */
	fl_rect bar = fl_r(lg.x, head.y + head.h, lg.w, 3);
	fl_fill_rect(cv, bar, fl_th.line);
	if (run && steps) { /* a bar that fills with the steps flatpak reports */
		int done = bar.w * (step - 1) / steps, part = bar.w / steps;
		fl_fill_rect(cv, fl_intersect(fl_r(bar.x, bar.y, done + part / 3, bar.h), bar), fl_th.accent);
	} else if (run) {
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		long ms = ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
		int seg = bar.w / 4 > 40 ? bar.w / 4 : 40;
		int pos = (int)((ms / 4) % (bar.w + seg)) - seg;
		fl_rect s = fl_intersect(fl_r(bar.x + pos, bar.y, seg, bar.h), bar);
		fl_fill_rect(cv, s, fl_th.accent);
	} else {
		fl_fill_rect(cv, bar, f->chg_failed ? fl_th.err : fl_th.accent);
	}
	/* the last lines of output */
	fl_rect body = fl_r(lg.x + 16, bar.y + bar.h + 6, lg.w - 32, lg.y + lg.h - bar.y - bar.h - 10);
	int lh = fl_font_height(f->mono) + 2, rows = body.h / lh;
	const char *buf = j->buf[0] ? j->buf[0] : "";
	size_t n = j->n[0];
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		n--;
	/* find the start of the last `rows` lines */
	size_t s = n;
	int lines = 0;
	while (s > 0 && lines < rows) {
		s--;
		if (buf[s] == '\n') {
			if (++lines == rows) {
				s++;
				break;
			}
		}
	}
	if (rows < 1)
		return;
	fl_rect save = cv->clip;
	fl_clip(cv, body);
	int y = body.y;
	for (size_t i = s; i < n && y + lh <= body.y + body.h + 2;) {
		const char *e = memchr(buf + i, '\n', n - i);
		size_t len = e ? (size_t)(e - (buf + i)) : n - i;
		fl_text(cv, f->mono, body.x, y, fl_th.text, buf + i, (int)len);
		y += lh;
		i += len + 1;
	}
	if (n == 0 && run)
		fl_text(cv, f->mono, body.x, y, fl_th.dim,
		        f->chg_kind == CK_PKG ? "Waiting for flpkg..." : "Waiting for flatpak...", -1);
	cv->clip = save;
}

static void draw_status(fl_ui *ui, struct F *f, fl_rect st)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, st, fl_th.panel);
	fl_fill_rect(cv, fl_r(st.x, st.y, st.w, 1), fl_th.line);
	char s[160] = "";
	if (f->tab == TAB_FLAT && !f->fl_have)
		snprintf(s, sizeof s, "Flatpak is not installed");
	else if (f->tab == TAB_FLAT && f->fl_remote == 0)
		snprintf(s, sizeof s, "Flathub is not set up");
	else if (f->tab == TAB_FLAT && f->loaded && f->query[0])
		snprintf(s, sizeof s, "%d of %zu apps", f->nshown, f->nall);
	else if (f->tab == TAB_FLAT && f->loaded)
		snprintf(s, sizeof s, "%d app%s%s", f->nshown, f->nshown == 1 ? "" : "s",
		         f->fmode == FM_INST ? " installed" : " on Flathub");
	else if (f->tab == TAB_AVAIL && !f->repo)
		snprintf(s, sizeof s, "No repository configured");
	else if (!f->loaded)
		snprintf(s, sizeof s, "Loading...");
	else if (f->tab == TAB_INST && f->query[0])
		snprintf(s, sizeof s, "%d of %zu installed packages", f->nshown, f->nall);
	else
		snprintf(s, sizeof s, "%d package%s%s", f->nshown, f->nshown == 1 ? "" : "s",
		         f->tab == TAB_INST ? " installed" : "");
	int mw = f->msg[0] ? fl_text_width(f->small, f->msg, -1) : 0;
	int avail = st.w - 28 - (mw ? mw + 24 : 0);
	if (mw && avail < 120) {
		mw = avail = st.w - 28;
		s[0] = 0;
	}
	char fit[200];
	fl_text_fit(f->small, s, avail, fit, sizeof fit);
	fl_text_in(cv, f->small, fl_r(st.x + 14, st.y + 1, avail, st.h - 1), FL_LEFT, 0, fl_th.dim, fit);
	if (mw) {
		char mf[420];
		fl_text_fit(f->small, f->msg, st.w - 28 - (s[0] ? 140 : 0), mf, sizeof mf);
		int w = fl_text_width(f->small, mf, -1);
		fl_text_in(cv, f->small, fl_r(st.x + st.w - 14 - w, st.y + 1, w + 14, st.h - 1), FL_LEFT, 0,
		           f->msg_err ? fl_th.err : fl_th.text, mf);
	}
}

/* The Flathub tab's strip above the list: Browse the catalog or look at what is installed. */
static void draw_strip(fl_ui *ui, struct F *f, fl_rect st)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, st, fl_th.panel);
	fl_fill_rect(cv, fl_r(st.x, st.y + st.h - 1, st.w, 1), fl_th.line);
	fl_fill_rect(cv, fl_r(st.x + st.w - 1, st.y, 1, st.h), fl_th.line);
	int y = st.y + (st.h - 28) / 2, x = st.x + 8;
	if (fl_button(ui, ID_FM_BROWSE, fl_r(x, y, 80, 28), "Browse", f->fmode == FM_BROWSE ? FL_BTN_ACCENT : FL_BTN_OUTLINE))
		queue(f, A_FM_BROWSE, 0);
	x += 84;
	if (fl_button(ui, ID_FM_INST, fl_r(x, y, 92, 28), "Installed", f->fmode == FM_INST ? FL_BTN_ACCENT : FL_BTN_OUTLINE))
		queue(f, A_FM_INST, 0);
	const char *side = f->fmode == FM_BROWSE ? "Refresh" : f->nupd ? "Update all" : "Remove unused";
	int sw = f->fmode == FM_INST && !f->nupd ? 116 : 96;
	if (fl_button(ui, ID_SIDE, fl_r(st.x + st.w - 1 - 8 - sw, y, sw, 28), side, FL_BTN_OUTLINE))
		queue(f, A_SIDE, 0);
}

static void draw(fl_ui *ui, void *ud)
{
	struct F *f = ud;
	fl_canvas *cv = &ui->cv;
	fl_rect c = fl_win_content(ui->win);
	if (!f->inited) {
		ui->focus = ID_SEARCH;
		f->inited = 1;
	}
	if (ui->key == XKB_KEY_f && (ui->mods & FL_MOD_CTRL)) {
		ui->focus = ID_SEARCH;
		ui->key = 0;
	}
	fl_rect area = c;
	fl_rect tb = fl_cut_top(&area, TOOL_H);
	fl_rect st = fl_cut_bottom(&area, STATUS_H);
	fl_rect lg = {0};
	if (f->chg_visible)
		lg = fl_cut_bottom(&area, LOG_H);
	fl_rect lr = fl_cut_left(&area, LIST_W);
	fl_rect strip = {0};
	int has_strip = f->tab == TAB_FLAT && f->fl_have && f->fl_remote != 0;
	if (has_strip)
		strip = fl_cut_top(&lr, STRIP_H);
	fl_fill_rect(cv, c, fl_th.bg);
	draw_list(ui, f, lr);
	if (has_strip)
		draw_strip(ui, f, strip);
	draw_details(ui, f, area);
	if (f->chg_visible)
		draw_log(ui, f, lg);
	draw_toolbar(ui, f, tb);
	draw_status(ui, f, st);
	if (f->act)
		run_action(f);
}

static void on_focus(fl_win *w, int focused, void *ud)
{
	struct F *f = ud;
	/* the lists may have changed (a flpkg in a terminal): read again, unless the person is typing a search */
	if (focused && f->inited && f->loaded && !job_running(&f->job[J_CHANGE]) &&
	    (f->tab != TAB_AVAIL || !f->query[0]))
		reload(f);
}

static void on_close(fl_win *w, void *ud)
{
	struct F *f = ud;
	if (job_running(&f->job[J_CHANGE])) {
		say(f, 1, "Wait until the change is finished");
		return;
	}
	fl_app_quit(f->app);
}

int main(int argc, char **argv)
{
	if (argc > 1) {
		fprintf(stderr, "usage: flpackages\nInstall and remove packages. Reads with flpkg, changes through flpkg-seat.\n");
		return !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") ? 0 : 2;
	}
	signal(SIGPIPE, SIG_IGN);
	struct F *f = calloc(1, sizeof *f);
	if (!f)
		return 1;
	f->flpkg = getenv("FLPACKAGES_FLPKG") ? getenv("FLPACKAGES_FLPKG") : "/usr/bin/flpkg";
	f->seat = getenv("FLPACKAGES_SEAT") ? getenv("FLPACKAGES_SEAT") : "/usr/bin/flpkg-seat";
	f->conf = getenv("FLPACKAGES_CONF") ? getenv("FLPACKAGES_CONF") : "/etc/flpkg.conf";
	f->flatpak = getenv("FLPACKAGES_FLATPAK") ? getenv("FLPACKAGES_FLATPAK") : "/usr/bin/flatpak";
	f->fl_remote = -1;
	for (int i = 0; i < J_COUNT; i++)
		f->job[i].fd[0] = f->job[i].fd[1] = -1;
	f->app = fl_app_new(NULL, NULL);
	if (!f->app) {
		fprintf(stderr, "flpackages: cannot connect to a Wayland compositor\n");
		return 1;
	}
	f->small = fl_font_ui(400, 12);
	f->head = fl_font_ui(600, 11);
	f->title = fl_font_ui(600, 20);
	f->mono = fl_font_mono(400, 12);
	f->msg_timer = fl_timer_new(f->app, msg_clear, f);
	f->search_timer = fl_timer_new(f->app, search_fire, f);
	f->anim_timer = fl_timer_new(f->app, anim_tick, f);
	fl_win_opts o = {
		.title = "Packages", .app_id = "flpackages", .w = 900, .h = 600, .min_w = 640, .min_h = 420,
		.decorated = 1, .resizable = 1, .draw = draw, .focus = on_focus, .close = on_close, .ud = f,
	};
	f->win = fl_win_new(f->app, &o);
	if (!f->win)
		return 1;
	f->tab = getenv("FLPACKAGES_TAB") && !strcmp(getenv("FLPACKAGES_TAB"), "flathub") ? TAB_FLAT : TAB_INST;
	reload(f);
	fl_app_watch_conf(f->app, NULL, NULL);
	fl_app_run(f->app);
	fl_app_free(f->app);
	return 0;
}
