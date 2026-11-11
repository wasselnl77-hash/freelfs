/* flstore -- the FreeLFS Software Store: logic, child processes, data
 * SPDX-License-Identifier: MIT
 *
 * One window (views.c draws it): Explore (home with featured apps, categories, search), Installed and
 * Updates. Two sources: Flathub, per user through `flatpak --user` (no root, no polkit), and FreeLFS's own
 * packages through the flpkg command (reads as the person, changes through flpkg-seat, the setuid helper
 * that ships with the Packages app).
 *
 * The Flathub catalog is read from Flathub's appstream data: downloaded with curl into
 * $XDG_CACHE_HOME/flstore, turned into a compact index by a child process (`flstore --build-index`,
 * index.c), loaded here (about 1 MB). Descriptions and screenshot links of one app are read on demand;
 * icons and screenshots are downloaded one at a time as the page shows them and cached on disk.
 *
 * Everything that can wait runs as a child process and reports through fl_fd_add, so the window never
 * blocks. Immediate mode as in flfiles: draw() reads the input and queues one action which runs after the
 * pass.
 *
 * Test hooks: FLSTORE_FLPKG, FLSTORE_SEAT, FLSTORE_CONF, FLSTORE_FLATPAK, FLSTORE_CURL replace
 * /usr/bin/flpkg, flpkg-seat, /etc/flpkg.conf, flatpak, curl; FLSTORE_SELF (the program that builds the index, default itself), FLSTORE_APPSTREAM_URL and FLSTORE_ICON_URL
 * replace Flathub's; FLSTORE_PAGE=home|cat:N|pkgs|search:TEXT|app:ID|installed|updates opens there.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "flstore.h"

#define MAX_BUF (8u << 20)
#define FLATHUB_URL "https://dl.flathub.org/repo/flathub.flatpakrepo"
#define APPSTREAM_URL "https://dl.flathub.org/repo/appstream/x86_64/appstream.xml.gz"
#define ICON_URL "https://dl.flathub.org/repo/appstream/x86_64/icons"
#define STALE_SECS (24 * 3600)

static const char *const featured_ids[NFEAT] = {
	"com.valvesoftware.Steam", "org.mozilla.firefox", "org.videolan.VLC", "org.gimp.GIMP",
	"org.libreoffice.LibreOffice", "com.obsproject.Studio", "com.discordapp.Discord", "com.spotify.Client",
};

/* ---- small helpers -------------------------------------------------------------------------- */

void queue(struct St *s, int act, int arg)
{
	if (!s->act) {
		s->act = act;
		s->act_arg = arg;
	}
}

static void note_hide(void *ud)
{
	struct St *s = ud;
	fl_timer_arm(s->note_timer, 0, 0);
	if (!s->running)
		s->bar_visible = 0;
	fl_win_redraw(s->win);
}

/* A message in the bottom bar (when no change is running). */
void say(struct St *s, int err, const char *fmt, ...)
{
	if (s->running)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(s->bar_text, sizeof s->bar_text, fmt, ap);
	va_end(ap);
	s->bar_sub[0] = 0;
	s->bar_err = err;
	s->bar_visible = 1;
	fl_timer_arm(s->note_timer, err ? 15000 : 8000, 0);
	fl_win_redraw(s->win);
}

/* The same rule as flpkg-seat: [a-z0-9+._-]+, first a letter or digit, at most 64, not ending ".flpkg". */
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

static void last_line(const char *buf, size_t n, char *out, size_t cap)
{
	out[0] = 0;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
		n--;
	size_t st = n;
	while (st > 0 && buf[st - 1] != '\n')
		st--;
	size_t len = n - st;
	if (len >= cap)
		len = cap - 1;
	memcpy(out, buf + st, len);
	out[len] = 0;
}

static int mkdir_p(const char *path)
{
	char tmp[600];
	snprintf(tmp, sizeof tmp, "%s", path);
	for (char *p = tmp + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			mkdir(tmp, 0755);
			*p = '/';
		}
	return mkdir(tmp, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

static int cmp_ci(const char *a, const char *b)
{
	return strcasecmp(a, b);
}

const char *state_label(int st)
{
	return st == ST_OLDER ? "Update available" : st == ST_INST ? "Installed" : "";
}

/* ---- child processes ------------------------------------------------------------------------- */

static void job_ready(void *ud, int fd, short rev);
static void job_done(struct St *s, struct job *j);

int job_running(const struct job *j)
{
	return j->pid > 0;
}

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
		fl_fd_remove(j->s->app, j->fd[i]);
		close(j->fd[i]);
		j->fd[i] = -1;
	}
}

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

static int job_start(struct St *s, int kind, char *const argv[], int merge)
{
	struct job *j = &s->job[kind];
	job_abandon(j);
	job_free_bufs(j);
	j->s = s;
	j->kind = kind;
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
		setenv("FLATPAK_FANCY_OUTPUT", "0", 1);
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
			fl_fd_add(s->app, j->fd[i], POLLIN, job_ready, j);
		}
	return 0;
}

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
		if (r < 0 && (errno == EAGAIN || errno == EINTR))
			break;
		job_close_fd(j, i); /* EOF or error */
		break;
	}
	if (j->kind == J_CHANGE)
		fl_win_redraw(j->s->win);
	if (j->fd[0] < 0 && j->fd[1] < 0)
		job_done(j->s, j);
}

/* ---- the cache directory ----------------------------------------------------------------------- */

static void cache_init(struct St *s)
{
	const char *x = getenv("XDG_CACHE_HOME"), *h = getenv("HOME");
	if (x && *x)
		snprintf(s->cache, sizeof s->cache, "%s/flstore", x);
	else if (h && *h)
		snprintf(s->cache, sizeof s->cache, "%s/.cache/flstore", h);
	else
		snprintf(s->cache, sizeof s->cache, "/tmp/flstore-%d", (int)getuid());
	mkdir_p(s->cache);
	const char *sub[] = {"icons64", "icons128", "shots"};
	for (int i = 0; i < 3; i++) {
		char p[600];
		snprintf(p, sizeof p, "%s/%s", s->cache, sub[i]);
		mkdir(p, 0755);
	}
}

/* Keep the picture cache small: past 120 MB the oldest files go (screenshots first, they are the big ones). */
static void cache_prune(struct St *s)
{
	char dir[600];
	snprintf(dir, sizeof dir, "%s/shots", s->cache);
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct ent {
		char name[200];
		time_t t;
		off_t sz;
	} *v = NULL;
	size_t n = 0, cap = 0;
	off_t total = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char p[900];
		snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		struct stat st;
		if (stat(p, &st) < 0 || strlen(e->d_name) >= 200)
			continue;
		if (n == cap) {
			struct ent *nv = realloc(v, (cap = cap ? cap * 2 : 256) * sizeof *v);
			if (!nv)
				break;
			v = nv;
		}
		snprintf(v[n].name, sizeof v[n].name, "%s", e->d_name);
		v[n].t = st.st_mtime;
		v[n].sz = st.st_size;
		total += st.st_size;
		n++;
	}
	closedir(d);
	while (total > (120 << 20) && n) {
		size_t old = 0;
		for (size_t i = 1; i < n; i++)
			if (v[i].t < v[old].t)
				old = i;
		char p[900];
		snprintf(p, sizeof p, "%s/%s", dir, v[old].name);
		unlink(p);
		total -= v[old].sz;
		v[old] = v[--n];
	}
	free(v);
}

/* ---- images ---------------------------------------------------------------------------------- */

static void img_pump(struct St *s);

static void img_free_entry(struct ient *e)
{
	free(e->im);
	memset(e, 0, sizeof *e);
}

static struct ient *img_slot(struct St *s)
{
	struct ient *best = NULL;
	for (int i = 0; i < IMG_MAX; i++) {
		struct ient *e = &s->im[i];
		if (!e->key[0])
			return e;
		if (e->state == IM_QUEUED || e->state == IM_LOADING || e->used == s->frame)
			continue;
		if (!best || e->used < best->used)
			best = e;
	}
	if (best)
		img_free_entry(best);
	return best;
}

/* How much decoded picture memory is held (screenshots are the big ones); over 10 MB the least recently
 * used leave. */
static void img_trim(struct St *s)
{
	for (;;) {
		size_t total = 0;
		struct ient *old = NULL;
		for (int i = 0; i < IMG_MAX; i++) {
			struct ient *e = &s->im[i];
			if (e->im) {
				total += (size_t)e->im->w * e->im->h * 4;
				if (e->used != s->frame && (!old || e->used < old->used))
					old = e;
			}
		}
		if (total <= (10u << 20) || !old)
			return;
		img_free_entry(old);
	}
}

const simg *img_get(struct St *s, const char *key, const char *path, const char *url, int w, int h)
{
	struct ient *e = NULL;
	for (int i = 0; i < IMG_MAX; i++)
		if (!strcmp(s->im[i].key, key)) {
			e = &s->im[i];
			break;
		}
	if (!e) {
		e = img_slot(s);
		if (!e)
			return NULL;
		snprintf(e->key, sizeof e->key, "%s", key);
		snprintf(e->path, sizeof e->path, "%s", path);
		snprintf(e->url, sizeof e->url, "%s", url);
		e->w = w;
		e->h = h;
		e->state = IM_NEW;
	}
	e->used = s->frame;
	if (e->state == IM_NEW) {
		if (access(e->path, R_OK) == 0) {
			if (s->dec_budget <= 0) { /* a long list scrolling in: decode a few per pass */
				s->dec_more = 1;
				return NULL;
			}
			s->dec_budget -= e->w > 200 ? 3 : 1;
			e->im = simg_load(e->path, e->w, e->h);
			if (e->im) {
				e->state = IM_READY;
				img_trim(s);
			} else {
				unlink(e->path); /* damaged download: forget it */
				e->state = IM_FAILED;
			}
		} else {
			e->state = IM_QUEUED;
			img_pump(s);
		}
	}
	return e->state == IM_READY ? e->im : NULL;
}

/* Start downloads for queued entries, a few at a time. */
static void img_pump(struct St *s)
{
	for (int k = 0; k < J_IMGN; k++) {
		struct job *j = &s->job[J_IMG0 + k];
		if (job_running(j))
			continue;
		struct ient *e = NULL;
		for (int i = 0; i < IMG_MAX && !e; i++) {
			if (s->im[i].state != IM_QUEUED)
				continue;
			int dup = 0; /* the same file is already on its way for another size */
			for (int m = 0; m < J_IMGN; m++)
				if (job_running(&s->job[J_IMG0 + m]) && s->job[J_IMG0 + m].tag >= 0 &&
				    !strcmp(s->im[s->job[J_IMG0 + m].tag].path, s->im[i].path))
					dup = 1;
			if (!dup)
				e = &s->im[i];
		}
		if (!e)
			return;
		char part[700];
		snprintf(part, sizeof part, "%s.part", e->path);
		char *argv[] = {(char *)s->curl, "-fsSL", "--connect-timeout", "15", "--max-time", "60", "--max-filesize",
		                "20000000", "-o", part, e->url, NULL};
		j->tag = (int)(e - s->im);
		if (job_start(s, J_IMG0 + k, argv, 0) < 0) {
			e->state = IM_FAILED;
			continue;
		}
		e->state = IM_LOADING;
	}
}

static void img_job_done(struct St *s, struct job *j)
{
	int idx = j->tag;
	j->tag = -1;
	if (idx < 0 || idx >= IMG_MAX)
		return;
	struct ient *e = &s->im[idx];
	char part[700];
	snprintf(part, sizeof part, "%s.part", e->path);
	int ok = j->status == 0 && access(part, R_OK) == 0 && rename(part, e->path) == 0;
	if (!ok)
		unlink(part);
	char path[640];
	snprintf(path, sizeof path, "%s", e->path);
	for (int i = 0; i < IMG_MAX; i++)
		if (s->im[i].key[0] && (s->im[i].state == IM_LOADING || s->im[i].state == IM_QUEUED) &&
		    !strcmp(s->im[i].path, path))
			s->im[i].state = ok ? IM_NEW : IM_FAILED;
	img_pump(s);
}

const simg *icon_get(struct St *s, const struct rview *v, int px)
{
	if ((v->src != SRC_FH && v->src != SRC_FI) || !valid_appid(v->id))
		return NULL;
	int size = px <= 64 ? 64 : 128;
	char key[200], path[640], url[460];
	snprintf(key, sizeof key, "i%d:%s:%d", size, v->id, px);
	snprintf(path, sizeof path, "%s/icons%d/%s.png", s->cache, size, v->id);
	snprintf(url, sizeof url, "%s/%dx%d/%s.png", s->icon_url, size, size, v->id);
	return img_get(s, key, path, url, px, px);
}

const simg *shot_get(struct St *s, int n, int w, int h)
{
	if (!s->det_ok || n < 0 || n >= s->det.nshot || s->cur.app.src != SRC_FH)
		return NULL;
	const struct app *a = &s->db.a[s->cur.app.idx];
	char key[200], path[640];
	snprintf(key, sizeof key, "s:%s:%d:%dx%d", a->id, n, w, h);
	snprintf(path, sizeof path, "%s/shots/%s-%d.img", s->cache, a->id, n);
	return img_get(s, key, path, s->det.shot[n], w, h);
}

/* ---- views on the data --------------------------------------------------------------------------- */

static struct pk *pk_find(const struct pk *v, size_t n, const char *name)
{
	for (size_t i = 0; i < n; i++)
		if (!strcmp(v[i].name, name))
			return (struct pk *)&v[i];
	return NULL;
}

int pk_state(const struct St *s, const char *name)
{
	const struct pk *p = pk_find(s->pkrepo, s->npkrepo, name);
	if (p)
		return p->state;
	return pk_find(s->pkinst, s->npkinst, name) ? ST_INST : ST_AVAIL;
}

static int upd_has(const struct St *s, const char *id)
{
	for (size_t i = 0; i < s->nupd; i++)
		if (!strcmp(s->upd[i], id))
			return 1;
	return 0;
}

void rview(const struct St *s, struct ref r, struct rview *v)
{
	memset(v, 0, sizeof *v);
	v->src = r.src;
	v->name = v->id = "";
	v->sum = v->ver = v->dev = "";
	switch (r.src) {
	case SRC_FH: {
		const struct app *a = &s->db.a[r.idx];
		v->app = a;
		v->id = a->id;
		v->name = a->name;
		v->sum = a->sum;
		v->ver = a->ver;
		v->dev = a->dev;
		v->state = s->st ? s->st[r.idx] : ST_AVAIL;
		if (v->state != ST_AVAIL) /* what is installed, not what the catalog has now */
			for (size_t i = 0; i < s->nfin; i++)
				if (!strcmp(s->fin[i].id, a->id) && s->fin[i].ver[0])
					v->ver = s->fin[i].ver;
		break;
	}
	case SRC_FI: {
		const struct fi *f = &s->fin[r.idx];
		v->id = f->id;
		v->name = f->name;
		v->ver = f->ver;
		v->state = upd_has(s, f->id) ? ST_OLDER : ST_INST;
		break;
	}
	case SRC_PR:
	case SRC_PI: {
		const struct pk *p = r.src == SRC_PR ? &s->pkrepo[r.idx] : &s->pkinst[r.idx];
		v->id = v->name = p->name;
		v->sum = p->sum ? p->sum : "";
		v->ver = p->ver;
		v->state = pk_state(s, p->name);
		break;
	}
	}
}

int ref_busy(const struct St *s, struct ref r)
{
	struct rview v;
	rview(s, r, &v);
	if (s->running && !strcmp(s->run.id, v.id))
		return 2;
	for (int i = 0; i < s->nq; i++)
		if (!strcmp(s->q[i].id, v.id))
			return 1;
	return 0;
}

int cat_total(const struct St *s, int cat)
{
	return s->catcount[cat];
}

const char *kv_get(const struct St *s, const char *a, const char *b)
{
	for (int i = 0; i < s->nkv; i++)
		if (!strcmp(s->kv[i].key, a) || (b && !strcmp(s->kv[i].key, b)))
			return s->kv[i].val;
	return NULL;
}

/* ---- the catalog ----------------------------------------------------------------------------------- */

static struct db *sort_db; /* for qsort */

static int cmp_ref_browse(const void *a, const void *b)
{
	const struct app *x = &sort_db->a[((const struct ref *)a)->idx], *y = &sort_db->a[((const struct ref *)b)->idx];
	int vx = x->verified * 2 + (x->nshots > 0), vy = y->verified * 2 + (y->nshots > 0);
	if (vx != vy)
		return vy - vx;
	return cmp_ci(x->name, y->name);
}

static int cmp_ref_recent(const void *a, const void *b)
{
	const struct app *x = &sort_db->a[((const struct ref *)a)->idx], *y = &sort_db->a[((const struct ref *)b)->idx];
	if (x->ts != y->ts)
		return x->ts < y->ts ? 1 : -1;
	return cmp_ci(x->name, y->name);
}

static void states_rebuild(struct St *s)
{
	if (!s->st)
		return;
	memset(s->st, 0, s->db.n);
	for (size_t i = 0; i < s->nfin; i++) {
		const struct app *a = db_find(&s->db, s->fin[i].id);
		if (a)
			s->st[a - s->db.a] = ST_INST;
	}
	for (size_t i = 0; i < s->nupd; i++) {
		const struct app *a = db_find(&s->db, s->upd[i]);
		if (a && s->st[a - s->db.a] == ST_INST)
			s->st[a - s->db.a] = ST_OLDER;
	}
}

static void view_rebuild(struct St *s);
/* bytes received on all interfaces but lo (a download's size, roughly: the one change that runs is
 * nearly all the traffic) */
unsigned long long net_rx_total(void)
{
	FILE *f = fopen("/proc/net/dev", "r");
	char line[512];
	unsigned long long sum = 0;
	while (f && fgets(line, sizeof line, f)) {
		char *c = strchr(line, ':');
		if (!c)
			continue;
		char *n = line;
		while (*n == ' ')
			n++;
		if (!strncmp(n, "lo:", 3))
			continue;
		unsigned long long rx = 0;
		if (sscanf(c + 1, "%llu", &rx) == 1)
			sum += rx;
	}
	if (f)
		fclose(f);
	return sum;
}

static long mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* free megabytes where the person's Flatpak data lives (the home folder), -1 when unknown */
long free_mb_home(void)
{
	const char *h = getenv("HOME");
	struct statvfs v;
	if (!h || statvfs(h, &v) != 0)
		return -1;
	return (long)((unsigned long long)v.f_bavail * v.f_frsize >> 20);
}

/* refresh the rate once a second while a change runs; the numbers go into the sub line (views.c) */
void net_tick(struct St *s)
{
	long now = mono_ms();
	if (now - s->net_prev_ms < 1000)
		return;
	unsigned long long cur = net_rx_total();
	if (cur >= s->net_prev && s->net_prev_ms)
		s->net_rate = (double)(cur - s->net_prev) * 1000.0 / (double)(now - s->net_prev_ms);
	s->net_prev = cur;
	s->net_prev_ms = now;
}

static void app_open(struct St *s);

/* After a catalog arrived: featured apps, category counts, the recently updated. */
static void db_ready(struct St *s)
{
	free(s->st);
	s->st = calloc(s->db.n ? s->db.n : 1, 1);
	s->nfeat = s->nrecent = 0;
	for (int i = 0; i < NFEAT; i++) {
		const struct app *a = db_find(&s->db, featured_ids[i]);
		if (a)
			s->feat[s->nfeat++] = (struct ref){SRC_FH, (int)(a - s->db.a)};
	}
	memset(s->catcount, 0, sizeof s->catcount);
	struct ref *tmp = malloc(s->db.n * sizeof *tmp);
	size_t nt = 0;
	time_t now = time(NULL);
	for (size_t i = 0; i < s->db.n; i++) {
		const struct app *a = &s->db.a[i];
		for (int c = 0; c < NCATS; c++)
			if (a->cats & (1u << c))
				s->catcount[c]++;
		if (!a->cats)
			s->catcount[7]++;
		int is_feat = 0;
		for (int k = 0; k < s->nfeat; k++)
			is_feat |= s->feat[k].idx == (int)i;
		if (tmp && a->verified && a->nshots > 0 && a->ts > 0 && (time_t)a->ts <= now && !is_feat)
			tmp[nt++] = (struct ref){SRC_FH, (int)i};
	}
	if (tmp) {
		sort_db = &s->db;
		qsort(tmp, nt, sizeof *tmp, cmp_ref_recent);
		for (size_t i = 0; i < nt && s->nrecent < NRECENT; i++)
			s->recent[s->nrecent++] = tmp[i];
		free(tmp);
	}
	states_rebuild(s);
}

static void start_build(struct St *s)
{
	char gz[600];
	snprintf(gz, sizeof gz, "%s/appstream.xml.gz", s->cache);
	char *argv[] = {s->self, "--build-index", gz, s->cache, NULL};
	if (job_start(s, J_BUILD, argv, 0) < 0) {
		s->idx_state = IDX_ERR;
		snprintf(s->idx_err, sizeof s->idx_err, "Cannot start the catalog indexer");
		return;
	}
	if (s->idx_state != IDX_READY)
		s->idx_state = IDX_BUILD;
}

static void start_fetch(struct St *s)
{
	char gz[600], part[620];
	snprintf(gz, sizeof gz, "%s/appstream.xml.gz", s->cache);
	snprintf(part, sizeof part, "%s.new", gz);
	unlink(part);
	char *argv[16];
	int n = 0;
	argv[n++] = (char *)s->curl;
	argv[n++] = "-fsSL";
	argv[n++] = "--connect-timeout";
	argv[n++] = "15";
	argv[n++] = "--max-time";
	argv[n++] = "300";
	argv[n++] = "-R";
	if (access(gz, R_OK) == 0) { /* only if Flathub has something newer */
		argv[n++] = "-z";
		argv[n++] = gz;
	}
	argv[n++] = "-o";
	argv[n++] = part;
	argv[n++] = (char *)s->appstream_url;
	argv[n] = NULL;
	if (job_start(s, J_FETCH, argv, 0) < 0) {
		if (s->idx_state != IDX_READY) {
			s->idx_state = IDX_ERR;
			snprintf(s->idx_err, sizeof s->idx_err, "Cannot start curl");
		}
		return;
	}
	if (s->idx_state != IDX_READY)
		s->idx_state = IDX_FETCH;
}

/* Load the index if there is one; refresh it in the background when it is old or missing. */
static void cat_start(struct St *s)
{
	struct db db;
	char gz[600];
	snprintf(gz, sizeof gz, "%s/appstream.xml.gz", s->cache);
	struct stat st;
	int have_gz = stat(gz, &st) == 0 && st.st_size > 0;
	if (db_load(&db, s->cache) == 0) {
		s->db = db;
		s->idx_state = IDX_READY;
		db_ready(s);
		if (!have_gz || time(NULL) - st.st_mtime > STALE_SECS)
			start_fetch(s);
	} else if (have_gz) {
		start_build(s);
	} else {
		start_fetch(s);
	}
}

static void fetch_done(struct St *s, struct job *j)
{
	char gz[600], part[620];
	snprintf(gz, sizeof gz, "%s/appstream.xml.gz", s->cache);
	snprintf(part, sizeof part, "%s.new", gz);
	struct stat st;
	int got = j->status == 0 && stat(part, &st) == 0 && st.st_size > 100000;
	if (got) {
		rename(part, gz);
		start_build(s);
		return;
	}
	unlink(part);
	if (j->status == 0) { /* not modified: the old one is current again */
		utimes(gz, NULL);
		if (s->idx_state != IDX_READY && access(gz, R_OK) == 0)
			start_build(s);
		return;
	}
	if (s->idx_state != IDX_READY) {
		s->idx_state = IDX_ERR;
		last_line(j->buf[1] ? j->buf[1] : "", j->n[1], s->idx_err, sizeof s->idx_err);
		if (!s->idx_err[0])
			snprintf(s->idx_err, sizeof s->idx_err, "Could not reach Flathub (curl status %d)", j->status);
	}
}

static void build_done(struct St *s, struct job *j)
{
	struct db db;
	if (j->status != 0 || db_load(&db, s->cache) != 0) {
		if (s->idx_state != IDX_READY) {
			s->idx_state = IDX_ERR;
			last_line(j->buf[1] ? j->buf[1] : "", j->n[1], s->idx_err, sizeof s->idx_err);
			if (!s->idx_err[0])
				snprintf(s->idx_err, sizeof s->idx_err, "The catalog could not be read");
			char gz[600];
			snprintf(gz, sizeof gz, "%s/appstream.xml.gz", s->cache);
			unlink(gz); /* a broken download must not be built again and again */
		}
		return;
	}
	char keep[100] = "";
	if (s->cur.page == PG_APP && s->cur.app.src == SRC_FH)
		snprintf(keep, sizeof keep, "%s", s->db.a[s->cur.app.idx].id);
	int was_ready = s->idx_state == IDX_READY;
	detail_free(&s->det);
	s->det_ok = 0;
	db_free(&s->db);
	s->db = db;
	s->idx_state = IDX_READY;
	db_ready(s);
	if (was_ready) { /* the references into the old catalog are void */
		s->nstack = 0;
		if (keep[0]) {
			const struct app *a = db_find(&s->db, keep);
			if (a)
				s->cur.app.idx = (int)(a - s->db.a);
			else
				s->cur.page = PG_HOME;
		} else if (s->cur.page == PG_CAT || s->cur.page == PG_SEARCH || s->cur.page == PG_APP) {
			s->cur.page = PG_HOME;
		}
	}
	view_rebuild(s);
	if (s->cur.page == PG_APP)
		app_open(s);
}

/* ---- Flatpak -------------------------------------------------------------------------------------- */

#define ADD(...)                                                                                           \
	do {                                                                                               \
		const char *l_[] = {__VA_ARGS__};                                                          \
		for (size_t i_ = 0; i_ < sizeof l_ / sizeof *l_; i_++)                                     \
			argv[n++] = (char *)l_[i_];                                                        \
	} while (0)

static void list_rebuild(struct St *s);

static void fl_load_fin(struct St *s)
{
	if (!s->fl_have)
		return;
	char *argv[] = {(char *)s->flatpak, "list", "--user", "--app", "--columns=application,name,version", NULL};
	job_start(s, J_FINST, argv, 0);
}

static void fl_load_upd(struct St *s, int net)
{
	if (!s->fl_have || s->fl_remote == 0)
		return;
	s->upd_net = net;
	char *argv[12];
	int n = 0;
	argv[n++] = (char *)s->flatpak;
	ADD("remote-ls", "--user", "--app", "--updates");
	if (!net)
		ADD("--cached");
	ADD("--columns=application", "flathub");
	argv[n] = NULL;
	job_start(s, J_FUPD, argv, 0);
}

static void fl_load_remote(struct St *s)
{
	if (!s->fl_have)
		return;
	char *argv[] = {(char *)s->flatpak, "remotes", "--user", "--columns=name", NULL};
	job_start(s, J_REMOTE, argv, 0);
}

static int cmp_fi(const void *a, const void *b)
{
	const struct fi *x = a, *y = b;
	int r = cmp_ci(x->name, y->name);
	return r ? r : strcmp(x->id, y->id);
}

static char *field(const char **s, const char *end)
{
	const char *t = memchr(*s, '\t', (size_t)(end - *s));
	size_t n = t ? (size_t)(t - *s) : (size_t)(end - *s);
	char *r = strndup(*s, n);
	*s += n + (t ? 1 : 0);
	return r;
}

static void fl_parse_fin(struct St *s, const char *buf)
{
	for (size_t i = 0; i < s->nfin; i++) {
		free(s->fin[i].id);
		free(s->fin[i].name);
		free(s->fin[i].ver);
	}
	free(s->fin);
	s->fin = NULL;
	s->nfin = 0;
	for (const char *p = buf; *p;) {
		const char *e = strchr(p, '\n');
		const char *end = e ? e : p + strlen(p);
		const char *q = p;
		char *id = field(&q, end), *name = field(&q, end), *ver = field(&q, end);
		if (valid_appid(id)) {
			struct fi *nv = realloc(s->fin, (s->nfin + 1) * sizeof *nv);
			if (nv) {
				if (!name[0]) {
					free(name);
					name = strdup(id);
				}
				s->fin = nv;
				s->fin[s->nfin++] = (struct fi){id, name, ver};
				p = e ? e + 1 : end;
				continue;
			}
		}
		free(id);
		free(name);
		free(ver);
		p = e ? e + 1 : end;
	}
	if (s->nfin > 1)
		qsort(s->fin, s->nfin, sizeof *s->fin, cmp_fi);
}

static void fl_parse_upd(struct St *s, const char *buf)
{
	for (size_t i = 0; i < s->nupd; i++)
		free(s->upd[i]);
	free(s->upd);
	s->upd = NULL;
	s->nupd = 0;
	for (const char *p = buf; *p;) {
		const char *e = strchr(p, '\n');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		if (len > 2 && len < 80) {
			char id[96];
			snprintf(id, sizeof id, "%.*s", (int)len, p);
			if (valid_appid(id)) {
				char **nv = realloc(s->upd, (s->nupd + 1) * sizeof *nv);
				if (nv) {
					s->upd = nv;
					s->upd[s->nupd++] = strdup(id);
				}
			}
		}
		p += len + (e ? 1 : 0);
	}
}

/* ---- flpkg ------------------------------------------------------------------------------------------ */

static int repo_configured(const char *path)
{
	FILE *fp = fopen(path, "r");
	if (!fp)
		return 0;
	char line[512];
	int found = 0;
	while (!found && fgets(line, sizeof line, fp)) {
		const char *p = line + strspn(line, " \t");
		if (!strncmp(p, "repo", 4) && (p[4] == ' ' || p[4] == '\t'))
			found = 1;
	}
	fclose(fp);
	return found;
}

static int run_flpkg(struct St *s, int kind, const char *a1, const char *a2, const char *a3)
{
	char *argv[10];
	int n = 0;
	argv[n++] = (char *)s->flpkg;
	if (getenv("FLSTORE_CONF")) {
		argv[n++] = "--config";
		argv[n++] = (char *)s->conf;
	}
	argv[n++] = (char *)a1;
	if (a2)
		argv[n++] = (char *)a2;
	if (a3)
		argv[n++] = (char *)a3;
	argv[n] = NULL;
	return job_start(s, kind, argv, 0);
}

static void pk_free(struct pk **v, size_t *n)
{
	for (size_t i = 0; i < *n; i++) {
		free((*v)[i].name);
		free((*v)[i].ver);
		free((*v)[i].sum);
	}
	free(*v);
	*v = NULL;
	*n = 0;
}

static void pk_push(struct pk **v, size_t *n, const char *name, size_t nl, const char *ver, size_t vl,
                    const char *sum, size_t sl, int state)
{
	if (nl == 0 || nl > 64)
		return;
	struct pk *nv = realloc(*v, (*n + 1) * sizeof *nv);
	if (!nv)
		return;
	*v = nv;
	(*v)[(*n)++] = (struct pk){strndup(name, nl), strndup(ver, vl), strndup(sum, sl), state};
}

static int cmp_pk(const void *a, const void *b)
{
	return strcmp(((const struct pk *)a)->name, ((const struct pk *)b)->name);
}

/* flpkg list: "name version" */
static void pk_parse_list(struct St *s, const char *buf)
{
	pk_free(&s->pkinst, &s->npkinst);
	for (const char *p = buf; *p;) {
		const char *e = strchr(p, '\n');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		const char *sp = memchr(p, ' ', len);
		if (sp)
			pk_push(&s->pkinst, &s->npkinst, p, (size_t)(sp - p), sp + 1, len - (size_t)(sp - p) - 1, "", 0, ST_INST);
		p += len + (e ? 1 : 0);
	}
	if (s->npkinst > 1)
		qsort(s->pkinst, s->npkinst, sizeof *s->pkinst, cmp_pk);
}

/* flpkg search: "repo/name version [installed]" then the summary indented on the next line */
static void pk_parse_search(struct St *s, const char *buf)
{
	pk_free(&s->pkrepo, &s->npkrepo);
	s->npkupd = 0;
	for (const char *p = buf; *p;) {
		const char *e = strchr(p, '\n');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		if (p[0] != ' ' && p[0] != '\t') {
			const char *slash = memchr(p, '/', len), *sp = memchr(p, ' ', len);
			if (slash && sp && slash < sp) {
				const char *name = slash + 1, *ver = sp + 1;
				const char *vend = memchr(ver, ' ', len - (size_t)(ver - p));
				size_t vl = vend ? (size_t)(vend - ver) : len - (size_t)(ver - p);
				int state = ST_AVAIL;
				if (vend && strstr(vend, "[installed: older]"))
					state = ST_OLDER;
				else if (vend && strstr(vend, "[installed]"))
					state = ST_INST;
				const char *next = e ? e + 1 : NULL, *sum = "";
				size_t sl = 0;
				if (next && (next[0] == ' ' || next[0] == '\t')) {
					sum = next + strspn(next, " \t");
					const char *ne = strchr(sum, '\n');
					sl = ne ? (size_t)(ne - sum) : strlen(sum);
				}
				if (state == ST_OLDER)
					s->npkupd++;
				pk_push(&s->pkrepo, &s->npkrepo, name, (size_t)(sp - name), ver, vl, sum, sl, state);
			}
		}
		p += len + (e ? 1 : 0);
	}
	if (s->npkrepo > 1)
		qsort(s->pkrepo, s->npkrepo, sizeof *s->pkrepo, cmp_pk);
}

/* flpkg check-updates (no root: it reads the repo indexes into a private temp dir):
 * "NAME INSTALLED AVAILABLE BYTES REPO", tab-separated. INSTALLED "-" is a new dependency, not an update. */
static void pk_parse_upd(struct St *s, const char *buf)
{
	pk_free(&s->pkup, &s->npkup);
	for (const char *p = buf; *p;) {
		const char *e = strchr(p, '\n');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		const char *t1 = memchr(p, '\t', len);
		const char *t2 = t1 ? memchr(t1 + 1, '\t', len - (size_t)(t1 + 1 - p)) : NULL;
		if (t1 && t2) {
			const char *t3 = memchr(t2 + 1, '\t', len - (size_t)(t2 + 1 - p));
			size_t vl = (t3 ? (size_t)(t3 - (t2 + 1)) : len - (size_t)(t2 + 1 - p));
			if (!(t1[1] == '-' && t2 == t1 + 2))
				pk_push(&s->pkup, &s->npkup, p, (size_t)(t1 - p), t2 + 1, vl, "", 0, ST_OLDER);
		}
		p += len + (e ? 1 : 0);
	}
}

/* what check-updates found, over what the search said (the search only knows a synced index) */
static void pk_apply_upd(struct St *s)
{
	int added = 0;
	for (size_t i = 0; i < s->npkup; i++) {
		struct pk *r = pk_find(s->pkrepo, s->npkrepo, s->pkup[i].name);
		if (r) {
			if (r->state != ST_OLDER) {
				free(r->ver);
				r->ver = strdup(s->pkup[i].ver);
				r->state = ST_OLDER;
			}
		} else {
			pk_push(&s->pkrepo, &s->npkrepo, s->pkup[i].name, strlen(s->pkup[i].name), s->pkup[i].ver,
			        strlen(s->pkup[i].ver), "", 0, ST_OLDER);
			added = 1;
		}
	}
	if (added && s->npkrepo > 1)
		qsort(s->pkrepo, s->npkrepo, sizeof *s->pkrepo, cmp_pk);
	s->npkupd = 0;
	for (size_t i = 0; i < s->npkrepo; i++)
		if (s->pkrepo[i].state == ST_OLDER)
			s->npkupd++;
}

static void pk_check_updates(struct St *s)
{
	run_flpkg(s, J_PKUPD, "check-updates", NULL, NULL);
}

static void pk_load(struct St *s)
{
	run_flpkg(s, J_PKLIST, "list", NULL, NULL);
	s->repo_cfg = repo_configured(s->conf);
	if (s->repo_cfg) {
		run_flpkg(s, J_PKREPO, "search", "", NULL);
		pk_check_updates(s);
	} else {
		pk_free(&s->pkrepo, &s->npkrepo);
		s->npkupd = 0;
		s->pkrepo_loaded = 1;
	}
}

/* ---- lists ------------------------------------------------------------------------------------------- */

static int find_ci(const char *h, const char *w)
{
	return h && h[0] && strcasestr(h, w) != NULL;
}

static int word_score(const struct app *a, const char *w)
{
	const char *p = strcasestr(a->name, w);
	if (p) {
		if (p == a->name)
			return 100;
		if (!isalnum((unsigned char)p[-1]))
			return 80;
		return 60;
	}
	if (find_ci(a->id, w))
		return 40;
	if (find_ci(a->kw, w))
		return 30;
	if (find_ci(a->sum, w))
		return 20;
	if (find_ci(a->dev, w))
		return 10;
	return 0;
}

static int *score_buf;

static int cmp_ref_score(const void *a, const void *b)
{
	const struct ref *x = a, *y = b;
	int sx = x->src == SRC_FH ? score_buf[x->idx] : 0, sy = y->src == SRC_FH ? score_buf[y->idx] : 0;
	if (x->src != y->src)
		return x->src == SRC_FH ? -1 : 1; /* Flathub first, FreeLFS packages after */
	if (sx != sy)
		return sy - sx;
	if (x->src == SRC_FH)
		return cmp_ci(sort_db->a[x->idx].name, sort_db->a[y->idx].name);
	return x->idx - y->idx;
}

static void list_free(struct St *s)
{
	free(s->list);
	s->list = NULL;
	s->nlist = 0;
}

static void build_cat(struct St *s, int cat)
{
	list_free(s);
	s->list = malloc((s->db.n + 1) * sizeof *s->list);
	if (!s->list)
		return;
	for (size_t i = 0; i < s->db.n; i++) {
		const struct app *a = &s->db.a[i];
		if ((a->cats & (1u << cat)) || (cat == 7 && !a->cats))
			s->list[s->nlist++] = (struct ref){SRC_FH, (int)i};
	}
	sort_db = &s->db;
	qsort(s->list, (size_t)s->nlist, sizeof *s->list, cmp_ref_browse);
}

static void build_pkgs(struct St *s)
{
	list_free(s);
	s->list = malloc((s->npkrepo + 1) * sizeof *s->list);
	if (!s->list)
		return;
	for (size_t i = 0; i < s->npkrepo; i++)
		s->list[s->nlist++] = (struct ref){SRC_PR, (int)i};
}

static void build_search(struct St *s, const char *query)
{
	list_free(s);
	char q[128];
	snprintf(q, sizeof q, "%s", query);
	char *words[6];
	int nw = 0;
	char *save = NULL;
	for (char *w = strtok_r(q, " \t", &save); w && nw < 6; w = strtok_r(NULL, " \t", &save))
		words[nw++] = w;
	if (!nw)
		return;
	s->list = malloc((s->db.n + s->npkrepo + 1) * sizeof *s->list);
	int *scores = calloc(s->db.n + 1, sizeof *scores);
	if (!s->list || !scores) {
		free(scores);
		list_free(s);
		return;
	}
	for (size_t i = 0; i < s->db.n; i++) {
		const struct app *a = &s->db.a[i];
		int total = 0, ok = 1;
		for (int w = 0; w < nw && ok; w++) {
			int sc = word_score(a, words[w]);
			if (!sc)
				ok = 0;
			total += sc;
		}
		if (ok) {
			scores[i] = total + (a->verified ? 5 : 0);
			s->list[s->nlist++] = (struct ref){SRC_FH, (int)i};
		}
	}
	for (size_t i = 0; i < s->npkrepo; i++) {
		int ok = 1;
		for (int w = 0; w < nw && ok; w++)
			ok = find_ci(s->pkrepo[i].name, words[w]) || find_ci(s->pkrepo[i].sum, words[w]);
		if (ok)
			s->list[s->nlist++] = (struct ref){SRC_PR, (int)i};
	}
	score_buf = scores;
	sort_db = &s->db;
	qsort(s->list, (size_t)s->nlist, sizeof *s->list, cmp_ref_score);
	score_buf = NULL;
	free(scores);
}

static void view_rebuild(struct St *s)
{
	switch (s->cur.page) {
	case PG_CAT: build_cat(s, s->cur.cat); break;
	case PG_PKGS: build_pkgs(s); break;
	case PG_SEARCH: build_search(s, s->cur.query); break;
	default: list_free(s); break;
	}
	int pages = (s->nlist + PAGE_SIZE - 1) / PAGE_SIZE;
	if (s->cur.pg >= pages)
		s->cur.pg = pages > 0 ? pages - 1 : 0;
}

/* Installed and Updates: rebuilt whenever what is installed or outdated changes. */
static void list_rebuild(struct St *s)
{
	free(s->ilist);
	free(s->ulist);
	s->ilist = s->ulist = NULL;
	s->nilist = s->nulist = s->nipk = s->niapp = 0;
	s->niapp = (int)s->nfin;
	s->nipk = (int)s->npkinst;
	int cap = (int)(s->nfin + s->npkinst + s->npkrepo) + 1;
	s->ilist = malloc((size_t)cap * sizeof *s->ilist);
	s->ulist = malloc((size_t)cap * sizeof *s->ulist);
	if (!s->ilist || !s->ulist)
		return;
	char flt[128];
	snprintf(flt, sizeof flt, "%s", s->tab == TAB_INST ? s->query : "");
	char *words[6];
	int nw = 0;
	char *save = NULL;
	for (char *w = strtok_r(flt, " \t", &save); w && nw < 6; w = strtok_r(NULL, " \t", &save))
		words[nw++] = w;
	if (s->cur.inst_mode == 0) {
		for (size_t i = 0; i < s->nfin; i++) {
			const struct app *a = db_find(&s->db, s->fin[i].id);
			struct ref r = a ? (struct ref){SRC_FH, (int)(a - s->db.a)} : (struct ref){SRC_FI, (int)i};
			struct rview v;
			rview(s, r, &v);
			int ok = 1;
			for (int w = 0; w < nw && ok; w++)
				ok = find_ci(v.name, words[w]) || find_ci(v.id, words[w]) || find_ci(v.sum, words[w]);
			if (ok)
				s->ilist[s->nilist++] = r;
		}
	} else {
		for (size_t i = 0; i < s->npkinst; i++) {
			int ok = 1;
			for (int w = 0; w < nw && ok; w++)
				ok = find_ci(s->pkinst[i].name, words[w]);
			if (ok)
				s->ilist[s->nilist++] = (struct ref){SRC_PI, (int)i};
		}
	}
	for (size_t i = 0; i < s->nfin; i++) {
		if (!upd_has(s, s->fin[i].id))
			continue;
		const struct app *a = db_find(&s->db, s->fin[i].id);
		s->ulist[s->nulist++] = a ? (struct ref){SRC_FH, (int)(a - s->db.a)} : (struct ref){SRC_FI, (int)i};
	}
	for (size_t i = 0; i < s->npkrepo; i++)
		if (s->pkrepo[i].state == ST_OLDER)
			s->ulist[s->nulist++] = (struct ref){SRC_PR, (int)i};
	if (s->ils.sel >= s->nilist)
		s->ils.sel = 0;
	if (s->uls.sel >= s->nulist)
		s->uls.sel = 0;
}

static void nav_reset(struct St *s, int page);
static void app_open(struct St *s);
static int app_resolve(struct St *s, struct nav *n);

/* Everything that depends on the loaded data, after any of it changed. */
static void data_changed(struct St *s)
{
	states_rebuild(s);
	list_rebuild(s);
	if (s->cur.page == PG_SEARCH || s->cur.page == PG_PKGS)
		view_rebuild(s);
	for (int i = 0; i < s->nstack; i++)
		if (s->stack[i].page == PG_APP && !app_resolve(s, &s->stack[i]))
			s->stack[i].page = PG_HOME;
	if (s->cur.page == PG_APP && !app_resolve(s, &s->cur)) {
		if (s->nstack > 0) {
			s->cur = s->stack[--s->nstack];
			s->scroll = s->cur.scroll;
		} else {
			nav_reset(s, PG_HOME);
		}
		view_rebuild(s);
		if (s->cur.page == PG_APP)
			app_open(s);
	}
}

/* ---- the app page ------------------------------------------------------------------------------------- */

static void info_parse(struct St *s, const char *buf, int flatpak)
{
	static const char *const keep_fp[] = {"ID", "Version", "License", "Download Size", "Installed Size", "Download",
	                                      "Installed", "Runtime", "Branch", NULL};
	static const char *const keep_pk[] = {"Version", "URL", "Licenses", "Installed size", "Download size", "Depends on",
	                                      "Groups", "Summary", NULL};
	const char *const *keep = flatpak ? keep_fp : keep_pk;
	s->nkv = 0;
	for (const char *p = buf; *p && s->nkv < MAX_KV;) {
		const char *e = strchr(p, '\n');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		const char *k = p;
		while (k < p + len && *k == ' ')
			k++;
		const char *colon = memchr(k, ':', len - (size_t)(k - p));
		if (colon && colon[1] == ' ' && colon - k > 0 && colon - k < 22) {
			char key[24];
			snprintf(key, sizeof key, "%.*s", (int)(colon - k), k);
			while (key[0] && key[strlen(key) - 1] == ' ')
				key[strlen(key) - 1] = 0;
			for (int i = 0; keep[i]; i++)
				if (!strcmp(key, keep[i])) {
					struct kv *kv = &s->kv[s->nkv++];
					const char *name = key;
					if (!strcmp(key, "Download Size"))
						name = "Download";
					else if (!strcmp(key, "Installed Size") || !strcmp(key, "Installed size"))
						name = "Installed";
					else if (!strcmp(key, "Download size"))
						name = "Download";
					snprintf(kv->key, sizeof kv->key, "%s", name);
					const char *v = colon + 2;
					size_t vl = len - (size_t)(v - p);
					if (vl >= sizeof kv->val)
						vl = sizeof kv->val - 1;
					memcpy(kv->val, v, vl);
					kv->val[vl] = 0;
					break;
				}
		}
		p += len + (e ? 1 : 0);
	}
}

static void info_load(struct St *s, int cached)
{
	struct rview v;
	rview(s, s->cur.app, &v);
	job_abandon(&s->job[J_INFO]);
	if (cached)
		s->info_tried = 0;
	s->nkv = 0;
	s->info_ok = 0;
	snprintf(s->info_for, sizeof s->info_for, "%s", v.id);
	if (v.src == SRC_FH || v.src == SRC_FI) {
		if (!s->fl_have || !valid_appid(v.id))
			return;
		char *argv[10];
		int n = 0;
		argv[n++] = (char *)s->flatpak;
		if (v.state == ST_AVAIL) {
			if (s->fl_remote == 0)
				return;
			ADD("remote-info", "--user");
			if (cached)
				ADD("--cached");
			ADD("flathub");
		} else {
			ADD("info", "--user");
		}
		argv[n++] = (char *)v.id;
		argv[n] = NULL;
		job_start(s, J_INFO, argv, 0);
	} else if (valid_name(v.id)) {
		if (v.state == ST_AVAIL)
			run_flpkg(s, J_INFO, "info", "--repo", v.id);
		else
			run_flpkg(s, J_INFO, "info", v.id, NULL);
	}
}

/* The app page's reference after the data behind it was read again; the stored ID is what counts. */
static int app_resolve(struct St *s, struct nav *n)
{
	if (!n->app_pkg) {
		const struct app *a = db_find(&s->db, n->app_id);
		if (a) {
			n->app = (struct ref){SRC_FH, (int)(a - s->db.a)};
			return 1;
		}
		for (size_t i = 0; i < s->nfin; i++)
			if (!strcmp(s->fin[i].id, n->app_id)) {
				n->app = (struct ref){SRC_FI, (int)i};
				return 1;
			}
		return 0;
	}
	for (size_t i = 0; i < s->npkrepo; i++)
		if (!strcmp(s->pkrepo[i].name, n->app_id)) {
			n->app = (struct ref){SRC_PR, (int)i};
			return 1;
		}
	for (size_t i = 0; i < s->npkinst; i++)
		if (!strcmp(s->pkinst[i].name, n->app_id)) {
			n->app = (struct ref){SRC_PI, (int)i};
			return 1;
		}
	return 0;
}

static void app_open(struct St *s)
{
	detail_free(&s->det);
	s->det_ok = 0;
	if (s->cur.app.src == SRC_FH && s->cur.app.idx < (int)s->db.n)
		s->det_ok = db_detail(&s->db, &s->db.a[s->cur.app.idx], &s->det) == 0;
	s->ask_remove = 0;
	s->free_mb = free_mb_home();
	info_load(s, 1);
}

/* ---- changes -------------------------------------------------------------------------------------------- */

static void chg_pump(struct St *s);

static void chg_add(struct St *s, int kind, const char *id, const char *name)
{
	for (int i = 0; i < s->nq; i++)
		if (s->q[i].kind == kind && !strcmp(s->q[i].id, id))
			return;
	if (s->running && s->run.kind == kind && !strcmp(s->run.id, id))
		return;
	if (s->nq >= MAX_Q)
		return;
	struct chg *c = &s->q[s->nq++];
	c->kind = kind;
	snprintf(c->id, sizeof c->id, "%s", id);
	snprintf(c->name, sizeof c->name, "%s", name);
	chg_pump(s);
}

static int chg_is_flatpak(int kind)
{
	return kind != CK_PKINSTALL && kind != CK_PKREMOVE;
}

static void chg_pump(struct St *s)
{
	if (s->running || s->nq == 0)
		return;
	struct chg c = s->q[0];
	memmove(&s->q[0], &s->q[1], (size_t)(s->nq - 1) * sizeof c);
	s->nq--;
	char *argv[14];
	int n = 0;
	const char *what = "";
	if (chg_is_flatpak(c.kind)) {
		if (!s->fl_have) {
			say(s, 1, "Flatpak is not installed");
			return;
		}
		argv[n++] = (char *)s->flatpak;
	} else {
		argv[n++] = (char *)s->seat;
	}
	switch (c.kind) {
	case CK_FINSTALL:
		ADD("install", "--user", "-y", "--noninteractive", "flathub");
		argv[n++] = c.id;
		what = "Installing";
		break;
	case CK_FUPDATE:
		ADD("update", "--user", "-y", "--noninteractive");
		argv[n++] = c.id;
		what = "Updating";
		break;
	case CK_FREMOVE:
		ADD("uninstall", "--user", "-y", "--noninteractive");
		argv[n++] = c.id;
		what = "Removing";
		break;
	case CK_FUPDATE_ALL:
		ADD("update", "--user", "-y", "--noninteractive");
		what = "Updating all apps";
		break;
	case CK_FUNUSED:
		ADD("uninstall", "--user", "--unused", "-y", "--noninteractive");
		what = "Cleaning up unused runtimes";
		break;
	case CK_ADDREMOTE:
		ADD("remote-add", "--user", "--if-not-exists", "flathub", FLATHUB_URL);
		what = "Adding Flathub";
		break;
	case CK_PKINSTALL:
		ADD("install");
		argv[n++] = c.id;
		what = "Installing";
		break;
	case CK_PKREMOVE:
		ADD("remove");
		argv[n++] = c.id;
		what = "Removing";
		break;
	default: return;
	}
	argv[n] = NULL;
	if (job_start(s, J_CHANGE, argv, 1) < 0) {
		say(s, 1, "Cannot start %s: %s", argv[0], strerror(errno));
		chg_pump(s);
		return;
	}
	s->run = c;
	s->running = 1;
	if (c.kind == CK_FUPDATE_ALL || c.kind == CK_FUNUSED || c.kind == CK_ADDREMOTE)
		snprintf(s->bar_text, sizeof s->bar_text, "%s", what);
	else
		snprintf(s->bar_text, sizeof s->bar_text, "%s %s", what, c.name);
	s->bar_sub[0] = 0;
	s->bar_visible = 1;
	s->bar_err = 0;
	s->ask_remove = 0;
	s->net0 = s->net_prev = net_rx_total();
	s->net_t0_ms = s->net_prev_ms = mono_ms();
	s->net_rate = 0;
	fl_timer_arm(s->note_timer, 0, 0);
	fl_timer_arm(s->anim_timer, 60, 60);
}

static void chg_done(struct St *s, struct job *j)
{
	struct chg c = s->run;
	s->running = 0;
	int failed = j->status != 0;
	char line[300];
	last_line(j->buf[0] ? j->buf[0] : "", j->n[0], line, sizeof line);
	if (failed) {
		snprintf(s->bar_text, sizeof s->bar_text, "%s", line[0] ? line : "The change failed");
		snprintf(s->bar_sub, sizeof s->bar_sub, "Status %d", j->status);
		s->bar_err = 1;
		s->nq = 0; /* what was queued behind a failure is not run */
	} else {
		static const char *const done[] = {[CK_FINSTALL] = "Installed", [CK_FUPDATE] = "Updated",
		                                   [CK_FREMOVE] = "Removed", [CK_PKINSTALL] = "Installed",
		                                   [CK_PKREMOVE] = "Removed"};
		if (c.kind == CK_FUPDATE_ALL)
			snprintf(s->bar_text, sizeof s->bar_text, "Flatpak apps are up to date");
		else if (c.kind == CK_FUNUSED)
			snprintf(s->bar_text, sizeof s->bar_text, "Unused runtimes removed");
		else if (c.kind == CK_ADDREMOTE)
			snprintf(s->bar_text, sizeof s->bar_text, "Flathub added");
		else
			snprintf(s->bar_text, sizeof s->bar_text, "%s %s", done[c.kind], c.name);
		s->bar_sub[0] = 0;
		s->bar_err = 0;
	}
	fl_timer_arm(s->anim_timer, 0, 0);
	s->bar_visible = 1;
	if (!failed && c.kind == CK_FREMOVE)
		chg_add(s, CK_FUNUSED, "unused", "unused runtimes");
	/* what changed is read again */
	if (chg_is_flatpak(c.kind)) {
		if (c.kind == CK_ADDREMOTE) {
			s->fl_remote = -1;
			fl_load_remote(s);
		}
		fl_load_fin(s);
		fl_load_upd(s, 0);
	} else {
		pk_load(s);
	}
	if (s->cur.page == PG_APP)
		info_load(s, 1);
	chg_pump(s);
	if (!s->running)
		fl_timer_arm(s->note_timer, failed ? 20000 : 8000, 0);
}

/* ---- finished jobs ------------------------------------------------------------------------------------------- */

static void job_done(struct St *s, struct job *j)
{
	int st = 0;
	if (j->pid > 0)
		waitpid(j->pid, &st, 0);
	j->pid = 0;
	j->status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
	const char *out = j->buf[0] ? j->buf[0] : "";
	if (j->kind >= J_IMG0) {
		img_job_done(s, j);
		fl_win_redraw(s->win);
		return;
	}
	switch (j->kind) {
	case J_FETCH: fetch_done(s, j); break;
	case J_BUILD: build_done(s, j); break;
	case J_REMOTE: {
		s->fl_remote = 0;
		for (const char *p = out; *p;) {
			const char *e = strchr(p, '\n');
			size_t len = e ? (size_t)(e - p) : strlen(p);
			if (len == 7 && !strncmp(p, "flathub", 7))
				s->fl_remote = 1;
			p += len + (e ? 1 : 0);
		}
		if (j->status)
			s->fl_remote = 0;
		if (s->fl_remote == 1 && !s->upd_loaded && !job_running(&s->job[J_FUPD]))
			fl_load_upd(s, 1);
		if (s->cur.page == PG_APP && !s->info_ok)
			info_load(s, 1);
		break;
	}
	case J_FINST:
		if (j->status == 0)
			fl_parse_fin(s, out);
		s->fin_loaded = 1;
		data_changed(s);
		break;
	case J_FUPD:
		if (j->status == 0) {
			fl_parse_upd(s, out);
			s->upd_loaded = 1;
			s->checked = time(NULL);
			data_changed(s);
		} else if (s->upd_net) { /* offline: what the last check saw */
			fl_load_upd(s, 0);
		} else {
			s->upd_loaded = 1;
			data_changed(s);
		}
		break;
	case J_INFO:
		if (j->status == 0 && s->cur.page == PG_APP && !strcmp(s->info_for, s->cur.app_id)) {
			info_parse(s, out, !s->cur.app_pkg);
			s->info_ok = 1;
		} else if (j->status != 0 && !s->cur.app_pkg && !s->info_tried) {
			s->info_tried = 1; /* not in the cached summary yet: ask the network once */
			info_load(s, 0);
		}
		break;
	case J_PKLIST:
		if (j->status == 0)
			pk_parse_list(s, out);
		s->pkinst_loaded = 1;
		data_changed(s);
		break;
	case J_PKREPO:
		if (j->status == 0)
			pk_parse_search(s, out);
		else {
			pk_free(&s->pkrepo, &s->npkrepo);
			s->npkupd = 0;
		}
		s->pkrepo_loaded = 1;
		pk_apply_upd(s);
		s->checked = s->checked ? s->checked : time(NULL);
		data_changed(s);
		break;
	case J_PKUPD:
		if (j->status == 0) {
			pk_parse_upd(s, out);
			s->checked = time(NULL);
		} else {
			pk_free(&s->pkup, &s->npkup);
		}
		if (s->pkrepo_loaded)
			pk_apply_upd(s);
		data_changed(s);
		break;
	case J_SYNC:
		/* `flpkg sync` is a root command today; whether it works or not, read the index again */
		run_flpkg(s, J_PKREPO, "search", "", NULL);
		pk_check_updates(s);
		break;
	case J_CHANGE: chg_done(s, j); break;
	}
	fl_win_redraw(s->win);
}

/* ---- starting an installed app ----------------------------------------------------------------------------------- */

static void app_run(struct St *s, const char *id)
{
	if (!s->fl_have || !valid_appid(id))
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
		execl(s->flatpak, s->flatpak, "run", id, (char *)NULL);
		_exit(127);
	}
	waitpid(pid, NULL, 0);
}

/* ---- navigation and actions ------------------------------------------------------------------------------------- */

static void nav_push(struct St *s)
{
	s->cur.scroll = s->scroll;
	if (s->nstack == MAX_STACK) {
		memmove(&s->stack[0], &s->stack[1], (MAX_STACK - 1) * sizeof s->stack[0]);
		s->nstack--;
	}
	s->stack[s->nstack++] = s->cur;
}

static void nav_reset(struct St *s, int page)
{
	s->nstack = 0;
	memset(&s->cur, 0, sizeof s->cur);
	s->cur.page = page;
	s->scroll = 0;
}

static void go_tab(struct St *s, int tab)
{
	s->tab = tab;
	s->query[0] = 0;
	s->last_query[0] = 0;
	nav_reset(s, tab == TAB_EXPLORE ? PG_HOME : tab == TAB_INST ? PG_INST : PG_UPD);
	view_rebuild(s);
	list_rebuild(s);
}

static void search_run(struct St *s)
{
	if (s->tab == TAB_INST) {
		list_rebuild(s);
		return;
	}
	if (s->tab != TAB_EXPLORE)
		return;
	snprintf(s->last_query, sizeof s->last_query, "%s", s->query);
	if (!s->query[0]) {
		if (s->cur.page == PG_SEARCH) {
			if (s->nstack > 0) {
				s->cur = s->stack[--s->nstack];
				s->scroll = s->cur.scroll;
				if (s->cur.page == PG_APP && app_resolve(s, &s->cur))
					app_open(s);
				else if (s->cur.page == PG_APP)
					s->cur.page = PG_HOME;
			} else {
				nav_reset(s, PG_HOME);
			}
			view_rebuild(s);
		}
		return;
	}
	if (s->cur.page != PG_SEARCH) {
		nav_push(s);
		memset(&s->cur, 0, sizeof s->cur);
		s->cur.page = PG_SEARCH;
	}
	snprintf(s->cur.query, sizeof s->cur.query, "%s", s->query);
	s->cur.pg = 0;
	s->scroll = 0;
	view_rebuild(s);
}

static void run_action(struct St *s)
{
	int act = s->act, arg = s->act_arg;
	s->act = 0;
	switch (act) {
	case A_TAB:
		if (arg != s->tab || s->cur.page != (arg == TAB_EXPLORE ? PG_HOME : arg == TAB_INST ? PG_INST : PG_UPD))
			go_tab(s, arg);
		break;
	case A_HOME:
		s->query[0] = s->last_query[0] = 0;
		nav_reset(s, PG_HOME);
		view_rebuild(s);
		break;
	case A_CAT:
		s->query[0] = s->last_query[0] = 0;
		nav_reset(s, PG_CAT);
		s->cur.cat = arg;
		view_rebuild(s);
		break;
	case A_PKGS:
		s->query[0] = s->last_query[0] = 0;
		nav_reset(s, PG_PKGS);
		view_rebuild(s);
		break;
	case A_OPEN_REF:
		nav_push(s);
		s->cur.page = PG_APP;
		s->cur.app = (struct ref){arg / 100000, arg % 100000};
		{
			struct rview v;
			rview(s, s->cur.app, &v);
			snprintf(s->cur.app_id, sizeof s->cur.app_id, "%s", v.id);
			s->cur.app_pkg = v.src == SRC_PR || v.src == SRC_PI;
		}
		s->cur.shot = 0;
		s->scroll = 0;
		app_open(s);
		break;
	case A_BACK:
		if (s->nstack > 0) {
			s->cur = s->stack[--s->nstack];
			s->scroll = s->cur.scroll;
			if (s->cur.page == PG_SEARCH)
				snprintf(s->query, sizeof s->query, "%s", s->cur.query);
			else if (s->tab == TAB_EXPLORE)
				s->query[0] = s->last_query[0] = 0;
			view_rebuild(s);
			if (s->cur.page == PG_APP && app_resolve(s, &s->cur))
				app_open(s);
			else if (s->cur.page == PG_APP)
				s->cur.page = PG_HOME;
		}
		break;
	case A_SEARCH: search_run(s); break;
	case A_PAGE: {
		int pages = (s->nlist + PAGE_SIZE - 1) / PAGE_SIZE;
		s->cur.pg += arg;
		if (s->cur.pg < 0)
			s->cur.pg = 0;
		if (s->cur.pg >= pages)
			s->cur.pg = pages > 0 ? pages - 1 : 0;
		s->scroll = 0;
		break;
	}
	case A_INSTALL:
	case A_UPDATE: {
		struct rview v;
		rview(s, s->cur.app, &v);
		if (v.src == SRC_FH || v.src == SRC_FI)
			chg_add(s, act == A_UPDATE || v.state == ST_OLDER ? CK_FUPDATE : CK_FINSTALL, v.id, v.name);
		else if (valid_name(v.id))
			chg_add(s, CK_PKINSTALL, v.id, v.name);
		break;
	}
	case A_ASK_REMOVE: s->ask_remove = 1; break;
	case A_CANCEL: s->ask_remove = 0; break;
	case A_REMOVE: {
		struct rview v;
		rview(s, s->cur.app, &v);
		s->ask_remove = 0;
		if (v.src == SRC_FH || v.src == SRC_FI)
			chg_add(s, CK_FREMOVE, v.id, v.name);
		else if (valid_name(v.id))
			chg_add(s, CK_PKREMOVE, v.id, v.name);
		break;
	}
	case A_OPEN_APP: {
		struct rview v;
		rview(s, s->cur.app, &v);
		app_run(s, v.id);
		say(s, 0, "Starting %s", v.name);
		break;
	}
	case A_SHOT:
		if (s->det.nshot > 0)
			s->cur.shot = (s->cur.shot + arg + s->det.nshot) % s->det.nshot;
		break;
	case A_CHIP:
		if (s->cur.inst_mode != arg) {
			s->cur.inst_mode = arg;
			s->ils = (fl_list_state){0};
			list_rebuild(s);
		}
		break;
	case A_UPDATE_ALL:
		if (s->nupd)
			chg_add(s, CK_FUPDATE_ALL, "all", "all apps");
		for (size_t i = 0; i < s->npkrepo; i++)
			if (s->pkrepo[i].state == ST_OLDER && valid_name(s->pkrepo[i].name))
				chg_add(s, CK_PKINSTALL, s->pkrepo[i].name, s->pkrepo[i].name);
		break;
	case A_CHECK:
		fl_load_upd(s, 1);
		fl_load_fin(s);
		if (s->repo_cfg) {
			char *argv[] = {(char *)s->flpkg, "sync", NULL};
			char *argv2[] = {(char *)s->flpkg, "--config", (char *)s->conf, "sync", NULL};
			if (job_start(s, J_SYNC, getenv("FLSTORE_CONF") ? argv2 : argv, 0) < 0) {
				run_flpkg(s, J_PKREPO, "search", "", NULL);
				pk_check_updates(s);
			}
		} else {
			pk_load(s);
		}
		if (s->idx_state != IDX_BUILD && (!s->live || s->live_try))
			start_fetch(s);
		break;
	case A_LIVE_TRY:
		if (s->live && !s->live_try) {
			s->live_try = 1;
			s->idx_state = IDX_NONE;
			cat_start(s);
		}
		break;
	case A_LIVE_INSTALL: {
		pid_t pid = fork();
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
			execlp("flinstall", "flinstall", (char *)NULL);
			_exit(127);
		}
		if (pid > 0)
			waitpid(pid, NULL, 0);
		break;
	}
	case A_RETRY:
		if (s->idx_state == IDX_ERR) {
			s->idx_state = IDX_NONE;
			cat_start(s);
		}
		break;
	case A_ADD_REMOTE: chg_add(s, CK_ADDREMOTE, "flathub", "Flathub"); break;
	case A_BAR_CLOSE:
		if (!s->running)
			s->bar_visible = 0;
		break;
	case A_BAR_LOG: s->bar_log = !s->bar_log; break;
	case A_BAR_CANCEL:
		if (s->running && chg_is_flatpak(s->run.kind) && s->job[J_CHANGE].pid > 0)
			kill(s->job[J_CHANGE].pid, SIGINT);
		s->nq = 0;
		break;
	case A_UNUSED: chg_add(s, CK_FUNUSED, "unused", "unused runtimes"); break;
	}
	fl_win_redraw(s->win);
}

/* ---- drawing support: progress ------------------------------------------------------------------------------------ */

/* flatpak's plain output says "Installing 2/4..." for each step: find the last "N/M". */
int steps_of(const char *buf, size_t n, int *cur, int *total)
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

/* The last "NN%" in the last 200 bytes, or -1. */
int pct_of(const char *buf, size_t n)
{
	for (size_t i = n, stop = n > 200 ? n - 200 : 0; i-- > stop;) {
		if (buf[i] != '%' || i == 0 || buf[i - 1] < '0' || buf[i - 1] > '9')
			continue;
		size_t a = i;
		while (a > 0 && buf[a - 1] >= '0' && buf[a - 1] <= '9')
			a--;
		if (i - a > 3)
			continue;
		int v = atoi(buf + a);
		return v >= 0 && v <= 100 ? v : -1;
	}
	return -1;
}

/* ---- window events ------------------------------------------------------------------------------------------------ */

static void anim_tick(void *ud)
{
	fl_win_redraw(((struct St *)ud)->win);
}

static void search_fire(void *ud)
{
	struct St *s = ud;
	fl_timer_arm(s->search_timer, 0, 0);
	if (strcmp(s->query, s->last_query))
		queue(s, A_SEARCH, 0);
	fl_win_redraw(s->win);
}

static void pump_fire(void *ud)
{
	struct St *s = ud;
	fl_timer_arm(s->pump_timer, 0, 0);
	fl_win_redraw(s->win);
}

static void on_focus(fl_win *w, int focused, void *ud)
{
	struct St *s = ud;
	/* what is installed may have changed outside (flpkg or flatpak in a terminal) */
	if (focused && s->inited && !s->running && !job_running(&s->job[J_FINST])) {
		fl_load_fin(s);
		pk_load(s);
	}
}

static void on_close(fl_win *w, void *ud)
{
	struct St *s = ud;
	if (s->running) {
		say(s, 1, "Wait until the change is finished");
		return;
	}
	fl_app_quit(s->app);
}

/* Called by views.c at the start of each pass. */
void frame_begin(struct St *s)
{
	s->frame++;
	s->dec_budget = 4;
	s->dec_more = 0;
}

void frame_end(struct St *s)
{
	if (s->dec_more)
		fl_timer_arm(s->pump_timer, 12, 0);
	if (s->act)
		run_action(s);
}

static void open_hook(struct St *s)
{
	const char *p = getenv("FLSTORE_PAGE");
	if (!p || !*p)
		return;
	if (!strcmp(p, "installed"))
		go_tab(s, TAB_INST);
	else if (!strcmp(p, "updates"))
		go_tab(s, TAB_UPD);
	else if (!strcmp(p, "pkgs")) {
		s->act = A_PKGS;
		run_action(s);
	} else if (!strncmp(p, "cat:", 4)) {
		s->act = A_CAT;
		s->act_arg = atoi(p + 4);
		run_action(s);
	} else if (!strncmp(p, "search:", 7)) {
		snprintf(s->query, sizeof s->query, "%s", p + 7);
		search_run(s);
	} else if (!strncmp(p, "app:", 4) && s->idx_state == IDX_READY) {
		const struct app *a = db_find(&s->db, p + 4);
		if (a) {
			s->act = A_OPEN_REF;
			s->act_arg = SRC_FH * 100000 + (int)(a - s->db.a);
			run_action(s);
		}
	}
	if (getenv("FLSTORE_SHOT"))
		s->cur.shot = atoi(getenv("FLSTORE_SHOT"));
}

int main(int argc, char **argv)
{
	if (argc >= 4 && !strcmp(argv[1], "--build-index"))
		return index_build(argv[2], argv[3]);
	if (argc == 2 && !strcmp(argv[1], "--updates")) /* flupdate-notify's button: open on the Updates tab */
		setenv("FLSTORE_PAGE", "updates", 1);
	else if (argc > 1) {
		fprintf(stderr, "usage: flstore [--updates]\nThe Software Store: Flathub apps and FreeLFS packages.\n");
		return !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") ? 0 : 2;
	}
	signal(SIGPIPE, SIG_IGN);
	struct St *s = calloc(1, sizeof *s);
	if (!s)
		return 1;
	const char *e;
	s->flpkg = (e = getenv("FLSTORE_FLPKG")) ? e : "/usr/bin/flpkg";
	s->seat = (e = getenv("FLSTORE_SEAT")) ? e : "/usr/bin/flpkg-seat";
	s->conf = (e = getenv("FLSTORE_CONF")) ? e : "/etc/flpkg.conf";
	s->flatpak = (e = getenv("FLSTORE_FLATPAK")) ? e : "/usr/bin/flatpak";
	s->curl = (e = getenv("FLSTORE_CURL")) ? e : "/usr/bin/curl";
	s->appstream_url = (e = getenv("FLSTORE_APPSTREAM_URL")) ? e : APPSTREAM_URL;
	s->icon_url = (e = getenv("FLSTORE_ICON_URL")) ? e : ICON_URL;
	s->fl_remote = -1;
	ssize_t n = readlink("/proc/self/exe", s->self, sizeof s->self - 1);
	if (n > 0)
		s->self[n] = 0;
	else
		snprintf(s->self, sizeof s->self, "%s", argv[0]);
	if ((e = getenv("FLSTORE_SELF")))
		snprintf(s->self, sizeof s->self, "%s", e);
	for (int i = 0; i < J_COUNT; i++) {
		s->job[i].fd[0] = s->job[i].fd[1] = -1;
		s->job[i].tag = -1;
	}
	cache_init(s);
	cache_prune(s);
	s->app = fl_app_new(NULL, NULL);
	if (!s->app) {
		fprintf(stderr, "flstore: cannot connect to a Wayland compositor\n");
		return 1;
	}
	s->small = fl_font_ui(400, 12);
	s->tiny = fl_font_ui(600, 11);
	s->big = fl_font_ui(600, 18);
	s->title = fl_font_ui(600, 24);
	s->mono = fl_font_mono(400, 12);
	s->anim_timer = fl_timer_new(s->app, anim_tick, s);
	s->search_timer = fl_timer_new(s->app, search_fire, s);
	s->note_timer = fl_timer_new(s->app, note_hide, s);
	s->pump_timer = fl_timer_new(s->app, pump_fire, s);
	fl_win_opts o = {
		.title = "Software Store", .app_id = "flstore", .w = 980, .h = 640, .min_w = 760, .min_h = 480,
		.decorated = 1, .resizable = 1, .draw = draw, .focus = on_focus, .close = on_close, .ud = s,
	};
	s->win = fl_win_new(s->app, &o);
	if (!s->win)
		return 1;
	s->fl_have = access(s->flatpak, X_OK) == 0;
	/* The live system keeps everything in memory: no catalog download (11 MB packed, 100 MB or more
	 * unpacked and indexed) and no Flathub unless the person asks for it on the notice (A_LIVE_TRY). */
	s->live = access(getenv("FLSTORE_LIVE_DIR") ? getenv("FLSTORE_LIVE_DIR") : "/run/live", F_OK) == 0;
	if (!s->live)
		cat_start(s);
	fl_load_remote(s);
	fl_load_fin(s);
	pk_load(s);
	open_hook(s);
	fl_app_watch_conf(s->app, NULL, NULL);
	fl_app_run(s->app);
	fl_app_free(s->app);
	return 0;
}
