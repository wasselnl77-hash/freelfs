/* flmusic -- the FreeLFS music player ("Music")
 * SPDX-License-Identifier: MIT
 *
 * A library of ~/music read by tags (albums, artists, songs), a queue, GStreamer
 * playback (playbin) and an MPRIS service on the session bus so media keys and
 * panels can drive it. Tags come from GstDiscoverer, one file per timer tick, and
 * are cached in ~/.cache/flmusic/library.tsv, so the second start shows the library
 * at once and only new or changed files are read.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>

#include <flui.h>

#include "mpris.h"

#define TOP_H 48
#define BAR_H 92
#define NAV_W 250
#define Q_W 290
#define ROW_H 30
#define NAV_ROW_H 52

enum { TAB_ALBUMS, TAB_ARTISTS, TAB_SONGS };
enum { ST_STOPPED, ST_PLAYING, ST_PAUSED };

enum {
	ID_TAB = 1, ID_SEARCH = 10, ID_QTOGGLE, ID_NAV, ID_TRACKS, ID_QUEUE, ID_PLAYALL, ID_ADDALL, ID_PREV,
	ID_PLAY, ID_NEXT, ID_SHUF, ID_REP, ID_SEEK, ID_VOL, ID_MUTE, ID_QCLEAR
};

struct track {
	char *path, *title, *artist, *album, *albumartist;
	int no, disc;
	long long dur_ms;
	long long mtime, size;
};

struct group {
	char *name, *sub; /* album: name, artist; artist: name, "" */
	int *tr;
	int n, cap;
};

struct M {
	fl_app *app;
	fl_win *win;
	fl_font *small, *big, *huge;
	fl_timer *msg_timer, *scan_timer, *pos_timer;
	char root[PATH_MAX], cache[PATH_MAX];
	/* library */
	struct track *t;
	int nt, capt;
	struct group *alb, *art;
	int nalb, nart;
	int *songs, nsongs;
	/* scan */
	struct {
		char **path;
		long long *mtime, *size;
		int n, cap, next;
	} todo;
	int scan_total, scan_done, scanning, cache_dirty;
	GstDiscoverer *disc;
	/* view */
	int tab, nav_cur;
	char search[96];
	int *shown, nshown;
	fl_list_state nav_st, trk_st, q_st;
	int show_queue;
	/* queue and playback */
	int *q, nq, qcap, qpos;
	GstElement *pipe;
	int status;
	long long pos_ms, dur_ms;
	double volume;
	int muted, shuffle, loop;
	int seeking;
	double seek_frac;
	int err_run;
	struct mpris *mpris;
	/* messages */
	char msg[300];
	int msg_err;
	unsigned rng;
};

static struct M *G;

static void say(struct M *m, int err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(m->msg, sizeof m->msg, fmt, ap);
	va_end(ap);
	m->msg_err = err;
	fl_timer_arm(m->msg_timer, 4000, 0);
}

static void msg_clear(void *ud)
{
	struct M *m = ud;
	m->msg[0] = 0;
	fl_win_redraw(m->win);
}

/* ---- strings -------------------------------------------------------------------------------- */

static char *clean(const char *s)
{
	char *d = strdup(s ? s : "");
	if (!d)
		return NULL;
	for (char *p = d; *p; p++)
		if (*p == '\t' || *p == '\n' || *p == '\r')
			*p = ' ';
	/* trim */
	size_t n = strlen(d);
	while (n && d[n - 1] == ' ')
		d[--n] = 0;
	char *b = d;
	while (*b == ' ')
		b++;
	if (b != d)
		memmove(d, b, strlen(b) + 1);
	return d;
}

static const char *base_of(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

static int is_audio(const char *name)
{
	static const char *ext[] = {".mp3", ".flac", ".ogg", ".oga", ".opus", ".m4a", ".aac", ".wav", ".mka", NULL};
	const char *e = strrchr(name, '.');
	if (!e)
		return 0;
	for (int i = 0; ext[i]; i++)
		if (!strcasecmp(e, ext[i]))
			return 1;
	return 0;
}

static void fmt_time(long long ms, char *out, size_t cap)
{
	if (ms < 0)
		ms = 0;
	long long s = ms / 1000;
	if (s >= 3600)
		snprintf(out, cap, "%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
	else
		snprintf(out, cap, "%lld:%02lld", s / 60, s % 60);
}

/* ---- the library ------------------------------------------------------------------------- */

static int add_track(struct M *m, struct track *t)
{
	if (m->nt == m->capt) {
		m->capt = m->capt ? m->capt * 2 : 256;
		struct track *n = realloc(m->t, (size_t)m->capt * sizeof *n);
		if (!n)
			return -1;
		m->t = n;
	}
	m->t[m->nt++] = *t;
	return m->nt - 1;
}

/* read the tags of one file; 0 when it is a playable audio file */
static int probe(struct M *m, const char *path, long long mtime, long long size, struct track *t)
{
	memset(t, 0, sizeof *t);
	if (!m->disc)
		return -1;
	char *uri = gst_filename_to_uri(path, NULL);
	if (!uri)
		return -1;
	GError *err = NULL;
	GstDiscovererInfo *info = gst_discoverer_discover_uri(m->disc, uri, &err);
	g_free(uri);
	if (err)
		g_error_free(err);
	if (!info)
		return -1;
	int ok = gst_discoverer_info_get_result(info) == GST_DISCOVERER_OK;
	GList *as = gst_discoverer_info_get_audio_streams(info);
	if (!as)
		ok = 0;
	g_list_free_full(as, g_object_unref);
	if (!ok) {
		g_object_unref(info);
		return -1;
	}
	const GstTagList *tl = gst_discoverer_info_get_tags(info);
	gchar *s = NULL;
	guint u = 0;
	t->path = strdup(path);
	if (tl && gst_tag_list_get_string(tl, GST_TAG_TITLE, &s) && s && *s)
		t->title = clean(s);
	g_free(s);
	s = NULL;
	if (tl && gst_tag_list_get_string(tl, GST_TAG_ARTIST, &s) && s && *s)
		t->artist = clean(s);
	g_free(s);
	s = NULL;
	if (tl && gst_tag_list_get_string(tl, GST_TAG_ALBUM, &s) && s && *s)
		t->album = clean(s);
	g_free(s);
	s = NULL;
	if (tl && gst_tag_list_get_string(tl, GST_TAG_ALBUM_ARTIST, &s) && s && *s)
		t->albumartist = clean(s);
	g_free(s);
	if (tl && gst_tag_list_get_uint(tl, GST_TAG_TRACK_NUMBER, &u))
		t->no = (int)u;
	if (tl && gst_tag_list_get_uint(tl, GST_TAG_ALBUM_VOLUME_NUMBER, &u))
		t->disc = (int)u;
	t->dur_ms = (long long)(gst_discoverer_info_get_duration(info) / GST_MSECOND);
	t->mtime = mtime;
	t->size = size;
	g_object_unref(info);
	if (!t->title) {
		/* the file name without its extension */
		char *b = clean(base_of(path)), *dot = strrchr(b, '.');
		if (dot)
			*dot = 0;
		t->title = b;
	}
	if (!t->artist)
		t->artist = strdup("Unknown artist");
	if (!t->album) {
		/* the folder it is in, unless that is the library itself */
		char *dir = strdup(path), *sl = strrchr(dir, '/');
		if (sl)
			*sl = 0;
		const char *folder = base_of(dir);
		const char *root = base_of(m->root);
		t->album = clean(*folder && strcmp(folder, root) ? folder : "Unknown album");
		free(dir);
	}
	if (!t->albumartist)
		t->albumartist = strdup(t->artist);
	return 0;
}

static int matches(const struct M *m, const struct track *t)
{
	if (!m->search[0])
		return 1;
	return strcasestr(t->title, m->search) || strcasestr(t->artist, m->search) || strcasestr(t->album, m->search);
}

static int cmp_ci(const char *a, const char *b)
{
	int c = strcasecmp(a, b);
	return c ? c : strcmp(a, b);
}

static int cmp_album_order(const void *a, const void *b)
{
	const struct track *x = &G->t[*(const int *)a], *y = &G->t[*(const int *)b];
	int c = cmp_ci(x->albumartist, y->albumartist);
	if (!c)
		c = cmp_ci(x->album, y->album);
	if (!c)
		c = x->disc - y->disc;
	if (!c)
		c = x->no - y->no;
	if (!c)
		c = cmp_ci(x->title, y->title);
	return c;
}

static int cmp_artist_order(const void *a, const void *b)
{
	const struct track *x = &G->t[*(const int *)a], *y = &G->t[*(const int *)b];
	int c = cmp_ci(x->artist, y->artist);
	if (!c)
		c = cmp_ci(x->album, y->album);
	if (!c)
		c = x->disc - y->disc;
	if (!c)
		c = x->no - y->no;
	if (!c)
		c = cmp_ci(x->title, y->title);
	return c;
}

static void group_add(struct group *g, int ti)
{
	if (g->n == g->cap) {
		g->cap = g->cap ? g->cap * 2 : 8;
		g->tr = realloc(g->tr, (size_t)g->cap * sizeof(int));
	}
	g->tr[g->n++] = ti;
}

static void groups_free(struct group **g, int *n)
{
	for (int i = 0; i < *n; i++) {
		free((*g)[i].name);
		free((*g)[i].sub);
		free((*g)[i].tr);
	}
	free(*g);
	*g = NULL;
	*n = 0;
}

static void rebuild_shown(struct M *m);

/* Albums, artists and the song list from the tracks that match the search. */
static void build_index(struct M *m)
{
	groups_free(&m->alb, &m->nalb);
	groups_free(&m->art, &m->nart);
	free(m->songs);
	m->songs = NULL;
	m->nsongs = 0;
	int *idx = malloc((size_t)(m->nt ? m->nt : 1) * sizeof(int)), n = 0;
	if (!idx)
		return;
	for (int i = 0; i < m->nt; i++)
		if (matches(m, &m->t[i]))
			idx[n++] = i;
	G = m;
	/* albums */
	qsort(idx, (size_t)n, sizeof(int), cmp_album_order);
	int acap = 0;
	for (int i = 0; i < n; i++) {
		struct track *t = &m->t[idx[i]];
		struct group *g = m->nalb ? &m->alb[m->nalb - 1] : NULL;
		if (!g || strcmp(g->name, t->album) || strcmp(g->sub, t->albumartist)) {
			if (m->nalb == acap) {
				acap = acap ? acap * 2 : 32;
				m->alb = realloc(m->alb, (size_t)acap * sizeof *m->alb);
			}
			g = &m->alb[m->nalb++];
			*g = (struct group){.name = strdup(t->album), .sub = strdup(t->albumartist)};
		}
		group_add(g, idx[i]);
	}
	/* artists, and the song list in the same order */
	qsort(idx, (size_t)n, sizeof(int), cmp_artist_order);
	acap = 0;
	for (int i = 0; i < n; i++) {
		struct track *t = &m->t[idx[i]];
		struct group *g = m->nart ? &m->art[m->nart - 1] : NULL;
		if (!g || strcmp(g->name, t->artist)) {
			if (m->nart == acap) {
				acap = acap ? acap * 2 : 32;
				m->art = realloc(m->art, (size_t)acap * sizeof *m->art);
			}
			g = &m->art[m->nart++];
			*g = (struct group){.name = strdup(t->artist), .sub = strdup("")};
		}
		group_add(g, idx[i]);
	}
	m->songs = idx;
	m->nsongs = n;
	rebuild_shown(m);
}

static void rebuild_shown(struct M *m)
{
	free(m->shown);
	m->shown = NULL;
	m->nshown = 0;
	int n = m->tab == TAB_ALBUMS ? m->nalb : m->tab == TAB_ARTISTS ? m->nart : 0;
	if (m->nav_cur >= n)
		m->nav_cur = n - 1;
	if (m->nav_cur < 0)
		m->nav_cur = 0;
	const int *src = NULL;
	int cnt = 0;
	if (m->tab == TAB_SONGS) {
		src = m->songs;
		cnt = m->nsongs;
	} else if (m->tab == TAB_ALBUMS && m->nalb) {
		src = m->alb[m->nav_cur].tr;
		cnt = m->alb[m->nav_cur].n;
	} else if (m->tab == TAB_ARTISTS && m->nart) {
		src = m->art[m->nav_cur].tr;
		cnt = m->art[m->nav_cur].n;
	}
	if (cnt) {
		m->shown = malloc((size_t)cnt * sizeof(int));
		if (m->shown) {
			memcpy(m->shown, src, (size_t)cnt * sizeof(int));
			m->nshown = cnt;
		}
	}
	m->trk_st.scroll = 0;
}

/* ---- cache and scan ------------------------------------------------------------------------ */

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
	return mkdir(t, 0700) < 0 && errno != EEXIST ? -1 : 0;
}

static void cache_save(struct M *m)
{
	char dir[PATH_MAX], tmp[PATH_MAX + 8];
	snprintf(dir, sizeof dir, "%s", m->cache);
	char *sl = strrchr(dir, '/');
	if (sl)
		*sl = 0;
	if (mkdir_p(dir) < 0)
		return;
	snprintf(tmp, sizeof tmp, "%s.tmp", m->cache);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	for (int i = 0; i < m->nt; i++) {
		struct track *t = &m->t[i];
		fprintf(f, "%s\t%lld\t%lld\t%lld\t%d\t%d\t%s\t%s\t%s\t%s\n", t->path, t->mtime, t->size, t->dur_ms,
		        t->no, t->disc, t->title, t->artist, t->album, t->albumartist);
	}
	if (fclose(f) == 0)
		rename(tmp, m->cache);
	else
		unlink(tmp);
}

struct cent {
	char *line;
	char *path;
};

static int cmp_cent(const void *a, const void *b)
{
	return strcmp(((const struct cent *)a)->path, ((const struct cent *)b)->path);
}

static struct cent *centries;
static int ncent;

static void cache_load(struct M *m)
{
	FILE *f = fopen(m->cache, "r");
	if (!f)
		return;
	char *line = NULL;
	size_t cap = 0;
	int ccap = 0;
	while (getline(&line, &cap, f) > 0) {
		line[strcspn(line, "\n")] = 0;
		char *path = line, *tab = strchr(line, '\t');
		if (!tab)
			continue;
		if (ncent == ccap) {
			ccap = ccap ? ccap * 2 : 256;
			centries = realloc(centries, (size_t)ccap * sizeof *centries);
		}
		*tab = 0;
		centries[ncent].line = strdup(tab + 1);
		centries[ncent].path = strdup(path);
		ncent++;
	}
	free(line);
	fclose(f);
	qsort(centries, (size_t)ncent, sizeof *centries, cmp_cent);
}

/* a cached track for this file when it has not changed, else -1 */
static int from_cache(struct M *m, const char *path, long long mtime, long long size)
{
	struct cent key = {.path = (char *)path};
	struct cent *e = bsearch(&key, centries, (size_t)ncent, sizeof *centries, cmp_cent);
	if (!e || !e->line)
		return -1;
	long long mt, sz, dur;
	int no, disc, off = 0;
	if (sscanf(e->line, "%lld\t%lld\t%lld\t%d\t%d\t%n", &mt, &sz, &dur, &no, &disc, &off) < 5 || mt != mtime || sz != size)
		return -1;
	char *f[4];
	char *rest = strdup(e->line + off), *p = rest;
	for (int i = 0; i < 4; i++) {
		f[i] = p;
		char *tab = p ? strchr(p, '\t') : NULL;
		if (i < 3 && !tab) {
			free(rest);
			return -1;
		}
		if (tab) {
			*tab = 0;
			p = tab + 1;
		}
	}
	struct track t = {.path = strdup(path), .title = strdup(f[0]), .artist = strdup(f[1]), .album = strdup(f[2]),
	                  .albumartist = strdup(f[3]), .no = no, .disc = disc, .dur_ms = dur, .mtime = mt, .size = sz};
	free(rest);
	return add_track(m, &t);
}

static void todo_add(struct M *m, const char *path, long long mtime, long long size)
{
	if (m->todo.n == m->todo.cap) {
		m->todo.cap = m->todo.cap ? m->todo.cap * 2 : 128;
		m->todo.path = realloc(m->todo.path, (size_t)m->todo.cap * sizeof(char *));
		m->todo.mtime = realloc(m->todo.mtime, (size_t)m->todo.cap * sizeof(long long));
		m->todo.size = realloc(m->todo.size, (size_t)m->todo.cap * sizeof(long long));
	}
	m->todo.path[m->todo.n] = strdup(path);
	m->todo.mtime[m->todo.n] = mtime;
	m->todo.size[m->todo.n] = size;
	m->todo.n++;
}

static void walk(struct M *m, const char *dir, int depth)
{
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char p[PATH_MAX];
		snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
		struct stat st;
		if (stat(p, &st) < 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			if (depth < 8)
				walk(m, p, depth + 1);
		} else if (S_ISREG(st.st_mode) && is_audio(e->d_name)) {
			m->scan_total++;
			if (from_cache(m, p, (long long)st.st_mtime, (long long)st.st_size) < 0)
				todo_add(m, p, (long long)st.st_mtime, (long long)st.st_size);
		}
	}
	closedir(d);
}

static void scan_tick(void *ud)
{
	struct M *m = ud;
	if (m->todo.next >= m->todo.n) {
		fl_timer_arm(m->scan_timer, 0, 0);
		m->scanning = 0;
		if (m->cache_dirty || ncent != m->nt)
			cache_save(m);
		m->cache_dirty = 0;
		build_index(m);
		fl_win_redraw(m->win);
		return;
	}
	int i = m->todo.next++;
	struct track t;
	if (probe(m, m->todo.path[i], m->todo.mtime[i], m->todo.size[i], &t) == 0) {
		add_track(m, &t);
		m->cache_dirty = 1;
	} else {
		m->scan_total--;
	}
	m->scan_done++;
	if (m->scan_done % 20 == 0 || m->todo.next >= m->todo.n) {
		build_index(m);
		fl_win_redraw(m->win);
	}
}

/* ---- playback ------------------------------------------------------------------------------ */

static void notify(struct M *m)
{
	mpris_changed(m->mpris);
	fl_win_redraw(m->win);
}

static const struct track *cur_track(const struct M *m)
{
	if (m->qpos < 0 || m->qpos >= m->nq)
		return NULL;
	return &m->t[m->q[m->qpos]];
}

static void set_status(struct M *m, int st)
{
	m->status = st;
	fl_timer_arm(m->pos_timer, st == ST_PLAYING ? 300 : 0, st == ST_PLAYING ? 300 : 0);
}

static void apply_volume(struct M *m)
{
	if (!m->pipe)
		return;
	g_object_set(m->pipe, "volume", m->volume, "mute", m->muted ? TRUE : FALSE, NULL);
}

static void stop(struct M *m)
{
	if (m->pipe)
		gst_element_set_state(m->pipe, GST_STATE_NULL);
	set_status(m, ST_STOPPED);
	m->pos_ms = 0;
	notify(m);
}

static void play_pos(struct M *m, int qpos)
{
	if (qpos < 0 || qpos >= m->nq || !m->pipe)
		return;
	m->qpos = qpos;
	const struct track *t = cur_track(m);
	char *uri = gst_filename_to_uri(t->path, NULL);
	gst_element_set_state(m->pipe, GST_STATE_NULL);
	g_object_set(m->pipe, "uri", uri, NULL);
	g_free(uri);
	apply_volume(m);
	gst_element_set_state(m->pipe, GST_STATE_PLAYING);
	m->pos_ms = 0;
	m->dur_ms = t->dur_ms;
	set_status(m, ST_PLAYING);
	char title[400];
	snprintf(title, sizeof title, "%s - %s", t->title, t->artist);
	fl_win_set_title(m->win, title);
	notify(m);
}

static int rnd(struct M *m, int n)
{
	m->rng = m->rng * 1664525u + 1013904223u;
	return (int)((m->rng >> 8) % (unsigned)n);
}

/* auto: the track ended by itself */
static void go_next(struct M *m, int autoplay)
{
	if (m->nq == 0)
		return;
	if (autoplay && m->loop == LOOP_TRACK) {
		play_pos(m, m->qpos);
		return;
	}
	int np;
	if (m->shuffle && m->nq > 1) {
		np = rnd(m, m->nq - 1);
		if (np >= m->qpos)
			np++;
	} else {
		np = m->qpos + 1;
		if (np >= m->nq) {
			if (m->loop == LOOP_LIST || !autoplay)
				np = m->loop == LOOP_LIST ? 0 : m->nq - 1;
			else {
				stop(m);
				return;
			}
		}
	}
	play_pos(m, np);
}

static void seek_ms(struct M *m, long long ms)
{
	if (!m->pipe || m->status == ST_STOPPED)
		return;
	if (ms < 0)
		ms = 0;
	if (m->dur_ms > 0 && ms > m->dur_ms)
		ms = m->dur_ms;
	gst_element_seek_simple(m->pipe, GST_FORMAT_TIME, (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
	                        (gint64)ms * GST_MSECOND);
	m->pos_ms = ms;
	mpris_seeked(m->mpris, ms * 1000);
	fl_win_redraw(m->win);
}

static void go_prev(struct M *m)
{
	if (m->nq == 0)
		return;
	if (m->pos_ms > 3000 || m->qpos <= 0) {
		if (m->qpos <= 0 && m->pos_ms <= 3000 && m->loop == LOOP_LIST && m->nq > 1)
			play_pos(m, m->nq - 1);
		else if (m->status == ST_STOPPED)
			play_pos(m, m->qpos < 0 ? 0 : m->qpos);
		else
			seek_ms(m, 0);
		return;
	}
	play_pos(m, m->qpos - 1);
}

static void do_pause(struct M *m)
{
	if (m->status != ST_PLAYING)
		return;
	gst_element_set_state(m->pipe, GST_STATE_PAUSED);
	set_status(m, ST_PAUSED);
	notify(m);
}

static void do_play(struct M *m)
{
	if (m->status == ST_PAUSED) {
		gst_element_set_state(m->pipe, GST_STATE_PLAYING);
		set_status(m, ST_PLAYING);
		notify(m);
	} else if (m->status == ST_STOPPED && m->nq) {
		play_pos(m, m->qpos >= 0 ? m->qpos : 0);
	}
}

static void toggle(struct M *m)
{
	if (m->status == ST_PLAYING)
		do_pause(m);
	else
		do_play(m);
}

static void queue_add(struct M *m, int ti)
{
	if (m->nq == m->qcap) {
		m->qcap = m->qcap ? m->qcap * 2 : 64;
		m->q = realloc(m->q, (size_t)m->qcap * sizeof(int));
	}
	m->q[m->nq++] = ti;
}

/* replace the queue by tracks and start at index first */
static void queue_play(struct M *m, const int *tr, int n, int first)
{
	if (n <= 0)
		return;
	m->nq = 0;
	for (int i = 0; i < n; i++)
		queue_add(m, tr[i]);
	m->q_st.scroll = 0;
	play_pos(m, first);
}

static void bus_ready(void *ud, int fd, short rev)
{
	struct M *m = ud;
	GstBus *bus = gst_element_get_bus(m->pipe);
	GstMessage *msg;
	while ((msg = gst_bus_pop(bus))) {
		switch (GST_MESSAGE_TYPE(msg)) {
		case GST_MESSAGE_EOS:
			m->err_run = 0;
			go_next(m, 1);
			break;
		case GST_MESSAGE_ERROR: {
			GError *e = NULL;
			gst_message_parse_error(msg, &e, NULL);
			const struct track *t = cur_track(m);
			say(m, 1, "Cannot play %s: %s", t ? t->title : "this file", e ? e->message : "unknown error");
			if (e)
				g_error_free(e);
			/* skip it, but never loop through a queue of broken files forever */
			if (++m->err_run < m->nq && m->qpos + 1 < m->nq)
				go_next(m, 1);
			else
				stop(m);
			break;
		}
		case GST_MESSAGE_DURATION_CHANGED:
			if (m->pipe) {
				gint64 d;
				if (gst_element_query_duration(m->pipe, GST_FORMAT_TIME, &d) && d > 0)
					m->dur_ms = d / GST_MSECOND;
				notify(m);
			}
			break;
		default: break;
		}
		gst_message_unref(msg);
	}
	gst_object_unref(bus);
	(void)fd;
	(void)rev;
}

static void pos_tick(void *ud)
{
	struct M *m = ud;
	gint64 p;
	if (m->pipe && !m->seeking && gst_element_query_position(m->pipe, GST_FORMAT_TIME, &p))
		m->pos_ms = p / GST_MSECOND;
	if (m->dur_ms <= 0) {
		gint64 d;
		if (m->pipe && gst_element_query_duration(m->pipe, GST_FORMAT_TIME, &d) && d > 0)
			m->dur_ms = d / GST_MSECOND;
	}
	fl_win_redraw(m->win);
}

static int make_pipeline(struct M *m)
{
	m->pipe = gst_element_factory_make("playbin", "player");
	if (!m->pipe)
		return -1;
	GstElement *sink = NULL;
	const char *want = getenv("FLMUSIC_SINK");
	if (want && *want) {
		sink = gst_element_factory_make(want, NULL);
		if (sink && g_object_class_find_property(G_OBJECT_GET_CLASS(sink), "sync"))
			g_object_set(sink, "sync", TRUE, NULL);
	} else {
		GstElementFactory *f = gst_element_factory_find("pipewiresink");
		if (f) {
			gst_object_unref(f);
			sink = gst_element_factory_make("pipewiresink", NULL);
			/* the plugin can be there with no PipeWire daemon: READY opens the connection, so check it now
			 * and fall back to autoaudiosink (ALSA, PulseAudio) rather than fail on the first song */
			if (sink && gst_element_set_state(sink, GST_STATE_READY) == GST_STATE_CHANGE_FAILURE) {
				gst_element_set_state(sink, GST_STATE_NULL);
				gst_object_unref(sink);
				sink = NULL;
			}
		}
		if (!sink)
			sink = gst_element_factory_make("autoaudiosink", NULL);
	}
	/* audio and soft volume only: no video, no subtitles */
	g_object_set(m->pipe, "flags", 0x02 | 0x10, NULL);
	if (sink)
		g_object_set(m->pipe, "audio-sink", sink, NULL);
	GstBus *bus = gst_element_get_bus(m->pipe);
	GPollFD pfd;
	gst_bus_get_pollfd(bus, &pfd);
	fl_fd_add(m->app, pfd.fd, POLLIN, bus_ready, m);
	gst_object_unref(bus);
	return 0;
}

/* ---- MPRIS glue ---------------------------------------------------------------------------- */

static void mp_play(void *ud) { do_play(ud); }
static void mp_pause(void *ud) { do_pause(ud); }
static void mp_playpause(void *ud) { toggle(ud); }
static void mp_stop(void *ud) { stop(ud); }
static void mp_next(void *ud) { go_next(ud, 0); }
static void mp_prev(void *ud) { go_prev(ud); }
static void mp_seek(void *ud, int64_t off)
{
	struct M *m = ud;
	seek_ms(m, m->pos_ms + off / 1000);
}
static void mp_setpos(void *ud, int64_t pos) { seek_ms(ud, pos / 1000); }
static void mp_setvol(void *ud, double v)
{
	struct M *m = ud;
	m->volume = v;
	apply_volume(m);
	notify(m);
}
static void mp_shuffle(void *ud, int on)
{
	struct M *m = ud;
	m->shuffle = on;
	notify(m);
}
static void mp_loop(void *ud, int l)
{
	struct M *m = ud;
	m->loop = l;
	notify(m);
}
static void mp_open(void *ud, const char *uri)
{
	struct M *m = ud;
	if (strncmp(uri, "file://", 7))
		return;
	char *path = g_filename_from_uri(uri, NULL, NULL);
	if (!path)
		return;
	struct stat st;
	if (stat(path, &st) == 0) {
		struct track t;
		if (probe(m, path, (long long)st.st_mtime, (long long)st.st_size, &t) == 0) {
			int ti = add_track(m, &t);
			if (ti >= 0)
				queue_play(m, &ti, 1, 0);
		}
	}
	g_free(path);
}
static void mp_quit(void *ud) { fl_app_quit(((struct M *)ud)->app); }

static void mp_get(void *ud, struct mpris_state *s)
{
	struct M *m = ud;
	const struct track *t = cur_track(m);
	s->status = m->status == ST_PLAYING ? "Playing" : m->status == ST_PAUSED ? "Paused" : "Stopped";
	s->loop = m->loop;
	s->shuffle = m->shuffle;
	s->volume = m->volume;
	s->position_us = m->pos_ms * 1000;
	s->can_play = m->nq > 0;
	s->can_next = m->nq > 1 && (m->shuffle || m->qpos + 1 < m->nq || m->loop == LOOP_LIST);
	s->can_prev = m->nq > 0;
	if (t) {
		s->title = t->title;
		s->artist = t->artist;
		s->album = t->album;
		static char uri[PATH_MAX * 3];
		char *u = g_filename_to_uri(t->path, NULL, NULL);
		snprintf(uri, sizeof uri, "%s", u ? u : "");
		g_free(u);
		s->uri = uri;
		s->length_us = (m->dur_ms > 0 ? m->dur_ms : t->dur_ms) * 1000;
		s->track_no = m->qpos;
	}
}

/* ---- drawing -------------------------------------------------------------------------------- */

static fl_color tile_color(const char *s)
{
	static const unsigned pal[] = {0x2c7a4b, 0x217c78, 0x2f6cae, 0x9a7210, 0xb35318, 0xab4569, 0x4d5a6b};
	unsigned h = 5381;
	for (; *s; s++)
		h = h * 33 + (unsigned char)*s;
	return FL_HEX(pal[h % (sizeof pal / sizeof *pal)]);
}

/* flat album tile: a colour from the name and its first letter */
static void draw_tile(fl_ui *ui, struct M *m, fl_rect r, const char *name)
{
	fl_fill_rect(&ui->cv, r, tile_color(name));
	char ch[8] = "?";
	const unsigned char *p = (const unsigned char *)name;
	if (p[0]) {
		size_t n = 1;
		if (p[0] >= 0xc0)
			while (p[n] && (p[n] & 0xc0) == 0x80 && n < 4)
				n++;
		memcpy(ch, p, n);
		ch[n] = 0;
		if (n == 1)
			ch[0] = (char)toupper(ch[0]);
	}
	fl_text_in(&ui->cv, r.h >= 44 ? m->huge : m->big, r, FL_CENTER, 0, FL_HEX(0xe9efe9), ch);
}

static void glyph_play(fl_canvas *cv, fl_rect r, fl_color c)
{
	fl_path *p = fl_path_new();
	float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f, s = r.w * 0.22f;
	fl_path_move(p, cx - s * 0.8f, cy - s * 1.2f);
	fl_path_line(p, cx + s * 1.3f, cy);
	fl_path_line(p, cx - s * 0.8f, cy + s * 1.2f);
	fl_path_close(p);
	fl_path_fill(cv, p, c);
	fl_path_free(p);
}

static void glyph_pause(fl_canvas *cv, fl_rect r, fl_color c)
{
	int cx = r.x + r.w / 2, cy = r.y + r.h / 2, s = r.w / 5;
	fl_fill_rect(cv, fl_r(cx - s - s / 2, cy - s * 5 / 4, s, s * 5 / 2), c);
	fl_fill_rect(cv, fl_r(cx + s / 2, cy - s * 5 / 4, s, s * 5 / 2), c);
}

static void glyph_skip(fl_canvas *cv, fl_rect r, fl_color c, int back)
{
	fl_path *p = fl_path_new();
	float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f, s = r.w * 0.2f;
	if (back) {
		fl_fill_rect(cv, fl_r((int)(cx - s * 1.3f), (int)(cy - s), 2, (int)(s * 2)), c);
		fl_path_move(p, cx + s * 1.1f, cy - s);
		fl_path_line(p, cx - s * 0.9f, cy);
		fl_path_line(p, cx + s * 1.1f, cy + s);
	} else {
		fl_fill_rect(cv, fl_r((int)(cx + s * 1.3f) - 2, (int)(cy - s), 2, (int)(s * 2)), c);
		fl_path_move(p, cx - s * 1.1f, cy - s);
		fl_path_line(p, cx + s * 0.9f, cy);
		fl_path_line(p, cx - s * 1.1f, cy + s);
	}
	fl_path_close(p);
	fl_path_fill(cv, p, c);
	fl_path_free(p);
}

static int round_button(fl_ui *ui, uint32_t id, fl_rect r, int kind, int big)
{
	int res = fl_widget(ui, id, r, FL_FOCUSABLE | FL_NO_FOCUS_RING);
	int hot = res & (FL_HOVER | FL_DOWN);
	if (big) {
		fl_color bg = fl_th.accent;
		if (res & FL_DOWN)
			bg = fl_mix(bg, 0xff000000, 40);
		else if (hot)
			bg = fl_mix(bg, 0xffffffff, 28);
		fl_fill_rect(&ui->cv, r, bg);
		if (res & FL_FOCUS)
			fl_stroke_rect(&ui->cv, r, 1, fl_th.strong);
		if (kind == 1)
			glyph_pause(&ui->cv, r, fl_th.on_accent);
		else
			glyph_play(&ui->cv, r, fl_th.on_accent);
	} else {
		if (hot)
			fl_fill_rect(&ui->cv, r, fl_th.raised);
		if (res & FL_FOCUS)
			fl_stroke_rect(&ui->cv, r, 1, fl_th.accent);
		glyph_skip(&ui->cv, r, hot ? fl_th.strong : fl_th.text, kind == 2);
	}
	return res & FL_CLICK;
}

/* a horizontal slider; returns 1 while the user drags it, *frac 0..1 */
static int slider(fl_ui *ui, uint32_t id, fl_rect r, double value, double *frac, int *released)
{
	int res = fl_widget(ui, id, r, 0);
	int track_y = r.y + r.h / 2 - 2;
	fl_fill_rect(&ui->cv, fl_r(r.x, track_y, r.w, 4), fl_th.line);
	int dragging = (res & FL_DOWN) != 0;
	if (dragging || (res & FL_CLICK)) {
		double f = (double)(ui->mx - r.x) / (r.w > 1 ? r.w : 1);
		*frac = f < 0 ? 0 : f > 1 ? 1 : f;
		value = *frac;
	}
	*released = (res & FL_CLICK) ? 1 : 0;
	int fw = (int)(value * r.w);
	fl_fill_rect(&ui->cv, fl_r(r.x, track_y, fw, 4), fl_th.accent);
	if (res & (FL_HOVER | FL_DOWN))
		fl_fill_rect(&ui->cv, fl_r(r.x + fw - 5, r.y + r.h / 2 - 7, 10, 14), fl_th.strong);
	return dragging;
}

struct trk_ctx {
	struct M *m;
};

static void track_row(fl_ui *ui, fl_rect r, int i, int hover, int selected, void *ud)
{
	struct M *m = ud;
	fl_canvas *cv = &ui->cv;
	const struct track *t = &m->t[m->shown[i]];
	const struct track *cur = cur_track(m);
	int playing = cur && cur == t;
	if (selected || hover)
		fl_fill_rect(cv, r, selected ? fl_th.select : fl_th.raised);
	fl_color fg = playing ? fl_th.accent : fl_th.text;
	char num[16] = "";
	if (m->tab != TAB_SONGS && t->no > 0)
		snprintf(num, sizeof num, "%d", t->no);
	if (playing)
		glyph_play(cv, fl_r(r.x + 6, r.y + 4, 22, r.h - 8), fl_th.accent);
	else
		fl_text_in(cv, m->small, fl_r(r.x, r.y, 36, r.h), FL_RIGHT, 8, fl_th.dim, num);
	char tm[24];
	fmt_time(t->dur_ms, tm, sizeof tm);
	int tw = 64;
	fl_text_in(cv, m->small, fl_r(r.x + r.w - tw, r.y, tw, r.h), FL_RIGHT, 12, fl_th.dim, tm);
	char sec[600] = "";
	if (m->tab == TAB_ARTISTS)
		snprintf(sec, sizeof sec, "%s", t->album);
	else if (m->tab == TAB_SONGS)
		snprintf(sec, sizeof sec, "%s  ·  %s", t->artist, t->album);
	int avail = r.w - 44 - tw;
	int secw = sec[0] ? avail * 42 / 100 : 0;
	char fit[600];
	fl_text_fit(ui->f_reg, t->title, avail - secw - 8, fit, sizeof fit);
	fl_text_in(cv, playing ? ui->f_med : ui->f_reg, fl_r(r.x + 44, r.y, avail - secw, r.h), FL_LEFT, 0, fg, fit);
	if (secw) {
		fl_text_fit(m->small, sec, secw - 8, fit, sizeof fit);
		fl_text_in(cv, m->small, fl_r(r.x + 44 + avail - secw, r.y, secw, r.h), FL_LEFT, 0, fl_th.dim, fit);
	}
}

static void nav_row(fl_ui *ui, fl_rect r, int i, int hover, int selected, void *ud)
{
	struct M *m = ud;
	fl_canvas *cv = &ui->cv;
	const struct group *g = m->tab == TAB_ALBUMS ? &m->alb[i] : &m->art[i];
	int cur = i == m->nav_cur;
	if (cur)
		fl_fill_rect(cv, r, fl_th.select);
	else if (hover)
		fl_fill_rect(cv, r, fl_th.raised);
	fl_rect tile = fl_r(r.x + 10, r.y + 6, r.h - 12, r.h - 12);
	draw_tile(ui, m, tile, g->name);
	int x = tile.x + tile.w + 10, w = r.x + r.w - x - 8;
	char fit[300], line[400];
	fl_text_fit(ui->f_med, g->name, w, fit, sizeof fit);
	if (m->tab == TAB_ALBUMS) {
		fl_text_in(cv, ui->f_med, fl_r(x, r.y + 7, w, 20), FL_LEFT, 0, cur ? fl_th.strong : fl_th.text, fit);
		fl_text_fit(m->small, g->sub, w, fit, sizeof fit);
		fl_text_in(cv, m->small, fl_r(x, r.y + 27, w, 18), FL_LEFT, 0, fl_th.dim, fit);
	} else {
		fl_text_in(cv, ui->f_med, fl_r(x, r.y + 7, w, 20), FL_LEFT, 0, cur ? fl_th.strong : fl_th.text, fit);
		snprintf(line, sizeof line, "%d track%s", g->n, g->n == 1 ? "" : "s");
		fl_text_in(cv, m->small, fl_r(x, r.y + 27, w, 18), FL_LEFT, 0, fl_th.dim, line);
	}
}

static void queue_row(fl_ui *ui, fl_rect r, int i, int hover, int selected, void *ud)
{
	struct M *m = ud;
	fl_canvas *cv = &ui->cv;
	const struct track *t = &m->t[m->q[i]];
	int cur = i == m->qpos;
	if (selected || hover)
		fl_fill_rect(cv, r, selected ? fl_th.select : fl_th.raised);
	if (cur)
		fl_fill_rect(cv, fl_r(r.x, r.y + 4, 3, r.h - 8), fl_th.accent);
	char fit[300];
	fl_text_fit(ui->f_reg, t->title, r.w - 20, fit, sizeof fit);
	fl_text_in(cv, cur ? ui->f_med : ui->f_reg, fl_r(r.x + 12, r.y + 4, r.w - 20, 20), FL_LEFT, 0,
	           cur ? fl_th.accent : fl_th.text, fit);
	fl_text_fit(m->small, t->artist, r.w - 20, fit, sizeof fit);
	fl_text_in(cv, m->small, fl_r(r.x + 12, r.y + 24, r.w - 20, 18), FL_LEFT, 0, fl_th.dim, fit);
}

static void set_tab(struct M *m, int tab)
{
	if (m->tab == tab)
		return;
	m->tab = tab;
	m->nav_cur = 0;
	m->nav_st.scroll = 0;
	rebuild_shown(m);
}

static void keys(fl_ui *ui, struct M *m)
{
	uint32_t k = ui->key;
	if (!k || ui->focus == ID_SEARCH)
		return;
	int mods = fl_app_mods(m->app), ctrl = mods & FL_MOD_CTRL;
	int handled = 1;
	switch (k) {
	case XKB_KEY_space: toggle(m); break;
	case XKB_KEY_Right:
		if (mods & FL_MOD_ALT)
			go_next(m, 0);
		else
			seek_ms(m, m->pos_ms + 5000);
		break;
	case XKB_KEY_Left:
		if (mods & FL_MOD_ALT)
			go_prev(m);
		else
			seek_ms(m, m->pos_ms - 5000);
		break;
	case XKB_KEY_n: go_next(m, 0); break;
	case XKB_KEY_p: go_prev(m); break;
	case XKB_KEY_m:
		m->muted = !m->muted;
		apply_volume(m);
		break;
	case XKB_KEY_s:
		m->shuffle = !m->shuffle;
		notify(m);
		break;
	case XKB_KEY_r:
		m->loop = (m->loop + 1) % 3;
		notify(m);
		break;
	case XKB_KEY_q:
		if (ctrl)
			fl_app_quit(m->app);
		else
			m->show_queue = !m->show_queue;
		break;
	case XKB_KEY_a:
		if (m->nshown && m->trk_st.sel >= 0 && m->trk_st.sel < m->nshown) {
			queue_add(m, m->shown[m->trk_st.sel]);
			say(m, 0, "Added to the queue");
		}
		break;
	case XKB_KEY_f:
		if (ctrl)
			ui->focus = ID_SEARCH;
		else
			handled = 0;
		break;
	case XKB_KEY_slash: ui->focus = ID_SEARCH; break;
	case XKB_KEY_1: set_tab(m, TAB_ALBUMS); break;
	case XKB_KEY_2: set_tab(m, TAB_ARTISTS); break;
	case XKB_KEY_3: set_tab(m, TAB_SONGS); break;
	case XKB_KEY_Escape:
		if (m->search[0]) {
			m->search[0] = 0;
			build_index(m);
		} else
			handled = 0;
		break;
	default: handled = 0;
	}
	if (handled)
		ui->key = 0;
}

static void draw_bar(fl_ui *ui, struct M *m, fl_rect bar)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, bar, fl_th.panel);
	fl_fill_rect(cv, fl_r(bar.x, bar.y, bar.w, 1), fl_th.line);
	const struct track *t = cur_track(m);
	/* left: what is playing */
	fl_rect inner = fl_inset(bar, 14, 12);
	fl_rect tile = fl_cut_left(&inner, inner.h);
	if (t)
		draw_tile(ui, m, tile, t->album);
	else
		fl_fill_rect(cv, tile, fl_th.raised);
	inner.x += 12;
	inner.w -= 12;
	int info_w = bar.w < 900 ? 190 : 260;
	fl_rect info = fl_cut_left(&inner, info_w);
	char fit[400];
	if (t) {
		fl_text_fit(ui->f_semi, t->title, info.w, fit, sizeof fit);
		fl_text_in(cv, ui->f_semi, fl_r(info.x, info.y + 8, info.w, 22), FL_LEFT, 0, fl_th.strong, fit);
		fl_text_fit(ui->f_reg, t->artist, info.w, fit, sizeof fit);
		fl_text_in(cv, ui->f_reg, fl_r(info.x, info.y + 31, info.w, 20), FL_LEFT, 0, fl_th.text, fit);
		fl_text_fit(m->small, t->album, info.w, fit, sizeof fit);
		fl_text_in(cv, m->small, fl_r(info.x, info.y + 51, info.w, 18), FL_LEFT, 0, fl_th.dim, fit);
	} else {
		fl_text_in(cv, ui->f_reg, fl_r(info.x, info.y, info.w, info.h), FL_LEFT, 0, fl_th.dim, "Nothing playing");
	}
	/* right: volume, shuffle, repeat */
	int vol_w = bar.w < 900 ? 90 : 120;
	fl_rect right = fl_cut_right(&inner, vol_w + 40 + 150);
	fl_rect rr = right;
	fl_rect vol = fl_cut_right(&rr, vol_w);
	fl_rect mute = fl_cut_right(&rr, 34);
	if (fl_icon_button(ui, ID_MUTE, fl_r(mute.x, bar.y + (bar.h - 34) / 2, 34, 34), m->muted ? FL_ICON_MUTE : FL_ICON_VOLUME,
	                   18, fl_th.text)) {
		m->muted = !m->muted;
		apply_volume(m);
	}
	double vf = m->volume, vfr = m->volume;
	int rel;
	if (slider(ui, ID_VOL, fl_r(vol.x, bar.y + (bar.h - 24) / 2, vol.w, 24), m->muted ? 0 : vf, &vfr, &rel)) {
		m->volume = vfr;
		m->muted = 0;
		apply_volume(m);
		mpris_changed(m->mpris);
	}
	rr.w -= 8;
	fl_rect rep = fl_cut_right(&rr, 76);
	fl_rect shuf = fl_cut_right(&rr, 66);
	static const char *rep_label[] = {"Repeat", "Repeat 1", "Repeat all"};
	if (fl_button(ui, ID_REP, fl_r(rep.x, bar.y + (bar.h - 32) / 2, rep.w + 10, 32), rep_label[m->loop], FL_BTN_PLAIN)) {
		m->loop = (m->loop + 1) % 3;
		notify(m);
	}
	if (m->loop)
		fl_fill_rect(cv, fl_r(rep.x + 10, bar.y + (bar.h + 32) / 2 - 3, rep.w - 10, 2), fl_th.accent);
	if (fl_button(ui, ID_SHUF, fl_r(shuf.x, bar.y + (bar.h - 32) / 2, shuf.w, 32), "Shuffle", FL_BTN_PLAIN)) {
		m->shuffle = !m->shuffle;
		notify(m);
	}
	if (m->shuffle)
		fl_fill_rect(cv, fl_r(shuf.x + 10, bar.y + (bar.h + 32) / 2 - 3, shuf.w - 20, 2), fl_th.accent);
	/* centre: transport and the seek bar */
	fl_rect mid = fl_inset(inner, 16, 0);
	int bw = 38, by = bar.y + 10;
	int cx = mid.x + mid.w / 2;
	if (round_button(ui, ID_PREV, fl_r(cx - 22 - bw - 14, by, bw, 38), 2, 0))
		go_prev(m);
	if (round_button(ui, ID_PLAY, fl_r(cx - 22, by - 2, 44, 42), m->status == ST_PLAYING, 1))
		toggle(m);
	if (round_button(ui, ID_NEXT, fl_r(cx + 22 + 14, by, bw, 38), 3, 0))
		go_next(m, 0);
	long long dur = m->dur_ms > 0 ? m->dur_ms : (t ? t->dur_ms : 0);
	char a[24], b[24];
	double frac = dur > 0 ? (double)m->pos_ms / (double)dur : 0;
	if (frac > 1)
		frac = 1;
	fl_rect sk = fl_r(mid.x + 52, bar.y + 58, mid.w - 104, 22);
	double sf = frac;
	int released;
	int drag = slider(ui, ID_SEEK, sk, frac, &sf, &released);
	if (drag) {
		m->seeking = 1;
		m->pos_ms = (long long)(sf * (double)dur);
	}
	if (released && dur > 0) {
		m->seeking = 0;
		seek_ms(m, (long long)(sf * (double)dur));
	}
	if (!drag && !released)
		m->seeking = 0;
	fmt_time(m->pos_ms, a, sizeof a);
	fmt_time(dur, b, sizeof b);
	fl_text_in(cv, m->small, fl_r(mid.x, bar.y + 58, 48, 22), FL_RIGHT, 6, fl_th.dim, a);
	fl_text_in(cv, m->small, fl_r(mid.x + mid.w - 48, bar.y + 58, 48, 22), FL_LEFT, 6, fl_th.dim, b);
}

static void draw(fl_ui *ui, void *ud)
{
	struct M *m = ud;
	fl_canvas *cv = &ui->cv;
	fl_rect c = fl_win_content(ui->win);
	fl_fill_rect(cv, c, fl_th.bg);
	fl_rect area = c;
	fl_rect tb = fl_cut_top(&area, TOP_H);
	fl_rect bar = fl_cut_bottom(&area, BAR_H);

	/* top bar: views, search, queue */
	fl_fill_rect(cv, tb, fl_th.panel);
	fl_fill_rect(cv, fl_r(tb.x, tb.y + tb.h - 1, tb.w, 1), fl_th.line);
	fl_rect t = fl_inset(tb, 10, 7);
	static const char *tabs[] = {"Albums", "Artists", "Songs"};
	static const int tabw[] = {72, 72, 66};
	for (int i = 0; i < 3; i++) {
		fl_rect b = fl_cut_left(&t, tabw[i]);
		if (fl_button(ui, ID_TAB + (uint32_t)i, b, tabs[i], FL_BTN_PLAIN))
			set_tab(m, i);
		if (m->tab == i)
			fl_fill_rect(cv, fl_r(b.x + 10, b.y + b.h - 3, b.w - 20, 2), fl_th.accent);
		t.x += 2;
		t.w -= 2;
	}
	fl_rect qb = fl_cut_right(&t, 70);
	if (fl_button(ui, ID_QTOGGLE, qb, "Queue", FL_BTN_PLAIN))
		m->show_queue = !m->show_queue;
	if (m->show_queue)
		fl_fill_rect(cv, fl_r(qb.x + 10, qb.y + qb.h - 3, qb.w - 20, 2), fl_th.accent);
	t.w -= 10;
	int sw = t.w < 330 ? t.w : 260;
	fl_rect sf = fl_cut_right(&t, sw);
	int se = fl_textfield(ui, ID_SEARCH, sf, m->search, sizeof m->search, "Search", ui->f_reg);
	if (se & FL_EDIT_CHANGED)
		build_index(m);
	if (m->scanning) {
		char s[64];
		snprintf(s, sizeof s, "Reading tags  %d / %d", m->scan_done, m->todo.n);
		fl_text_in(cv, m->small, fl_r(t.x + 16, t.y, t.w - 16, t.h), FL_LEFT, 0, fl_th.dim, s);
	}

	keys(ui, m);

	fl_rect qside = {0}, nav = {0};
	if (m->show_queue && area.w >= 760)
		qside = fl_cut_right(&area, Q_W);
	if (m->tab != TAB_SONGS)
		nav = fl_cut_left(&area, NAV_W);
	fl_rect main = area;

	if (nav.w) {
		fl_fill_rect(cv, nav, fl_th.panel);
		fl_fill_rect(cv, fl_r(nav.x + nav.w - 1, nav.y, 1, nav.h), fl_th.line);
		int n = m->tab == TAB_ALBUMS ? m->nalb : m->nart;
		int pick = fl_list(ui, ID_NAV, fl_r(nav.x, nav.y + 4, nav.w - 1, nav.h - 4), n, NAV_ROW_H, &m->nav_st, nav_row, m);
		if (pick >= 0 && pick != m->nav_cur) {
			m->nav_cur = pick;
			rebuild_shown(m);
		}
	}

	/* the track list */
	fl_rect head = fl_cut_top(&main, 52);
	if (m->nshown || m->nt) {
		char title[400] = "All songs", sub[200] = "";
		if (m->tab == TAB_ALBUMS && m->nalb) {
			snprintf(title, sizeof title, "%s", m->alb[m->nav_cur].name);
			snprintf(sub, sizeof sub, "%s", m->alb[m->nav_cur].sub);
		} else if (m->tab == TAB_ARTISTS && m->nart) {
			snprintf(title, sizeof title, "%s", m->art[m->nav_cur].name);
			snprintf(sub, sizeof sub, "%d albums", 0);
			int na = 0;
			for (int i = 0; i < m->nalb; i++)
				if (!strcmp(m->alb[i].sub, m->art[m->nav_cur].name))
					na++;
			snprintf(sub, sizeof sub, "%d track%s", m->art[m->nav_cur].n, m->art[m->nav_cur].n == 1 ? "" : "s");
			(void)na;
		} else if (m->tab == TAB_SONGS) {
			snprintf(sub, sizeof sub, "%d track%s", m->nsongs, m->nsongs == 1 ? "" : "s");
		}
		fl_rect h = fl_inset(head, 14, 6);
		fl_rect btns = fl_cut_right(&h, 230);
		char fit[400];
		fl_text_fit(ui->f_semi, title, h.w, fit, sizeof fit);
		fl_text_in(cv, ui->f_semi, fl_r(h.x, h.y, h.w, 22), FL_LEFT, 0, fl_th.strong, fit);
		fl_text_in(cv, m->small, fl_r(h.x, h.y + 22, h.w, 18), FL_LEFT, 0, fl_th.dim, sub);
		fl_rect pa = fl_cut_left(&btns, 80);
		btns.x += 8;
		btns.w -= 8;
		if (fl_button(ui, ID_PLAYALL, fl_r(pa.x, pa.y + 3, pa.w, 34), "Play", FL_BTN_ACCENT) && m->nshown)
			queue_play(m, m->shown, m->nshown, m->shuffle ? rnd(m, m->nshown) : 0);
		if (fl_button(ui, ID_ADDALL, fl_r(btns.x, btns.y + 3, btns.w, 34), "Add to queue", FL_BTN_OUTLINE) && m->nshown) {
			for (int i = 0; i < m->nshown; i++)
				queue_add(m, m->shown[i]);
			if (m->status == ST_STOPPED && m->qpos < 0)
				m->qpos = 0;
			say(m, 0, "Added %d track%s to the queue", m->nshown, m->nshown == 1 ? "" : "s");
		}
	}
	fl_fill_rect(cv, fl_r(main.x, main.y, main.w, 1), fl_th.line);
	if (m->nshown) {
		int enter = ui->key == XKB_KEY_Return && ui->focus != ID_SEARCH;
		int pick = fl_list(ui, ID_TRACKS, fl_r(main.x, main.y + 1, main.w, main.h - 1), m->nshown, ROW_H, &m->trk_st,
		                   track_row, m);
		if (pick >= 0 && (ui->clicks >= 2 || enter))
			queue_play(m, m->shown, m->nshown, pick);
	} else {
		const char *s;
		char buf[PATH_MAX + 200];
		if (m->scanning && m->nt == 0)
			s = "Reading your music...";
		else if (m->nt == 0) {
			snprintf(buf, sizeof buf, "No music found in %s\nPut MP3, FLAC, Ogg, Opus, M4A or WAV files there.", m->root);
			s = buf;
		} else
			s = "Nothing matches the search";
		/* two lines */
		const char *nl = strchr(s, '\n');
		if (nl) {
			char l1[PATH_MAX + 200];
			snprintf(l1, sizeof l1, "%.*s", (int)(nl - s), s);
			fl_text_in(cv, ui->f_reg, fl_r(main.x, main.y + main.h / 2 - 24, main.w, 24), FL_CENTER, 0, fl_th.text, l1);
			fl_text_in(cv, m->small, fl_r(main.x, main.y + main.h / 2, main.w, 24), FL_CENTER, 0, fl_th.dim, nl + 1);
		} else
			fl_text_in(cv, ui->f_reg, main, FL_CENTER, 0, fl_th.dim, s);
	}

	/* the queue */
	if (qside.w) {
		fl_fill_rect(cv, qside, fl_th.panel);
		fl_fill_rect(cv, fl_r(qside.x, qside.y, 1, qside.h), fl_th.line);
		fl_rect qh = fl_cut_top(&qside, 48);
		fl_text_in(cv, ui->f_semi, fl_r(qh.x + 14, qh.y, 140, qh.h), FL_LEFT, 0, fl_th.strong, "Up next");
		if (m->nq && fl_button(ui, ID_QCLEAR, fl_r(qh.x + qh.w - 70, qh.y + 8, 58, 32), "Clear", FL_BTN_PLAIN)) {
			m->nq = 0;
			m->qpos = -1;
			stop(m);
		}
		if (m->nq) {
			int pick = fl_list(ui, ID_QUEUE, fl_r(qside.x + 1, qside.y, qside.w - 1, qside.h), m->nq, 46, &m->q_st,
			                   queue_row, m);
			if (pick >= 0)
				play_pos(m, pick);
		} else {
			fl_text_in(cv, m->small, fl_r(qside.x, qside.y + 20, qside.w, 40), FL_CENTER, 0, fl_th.dim, "The queue is empty");
			fl_text_in(cv, m->small, fl_r(qside.x, qside.y + 40, qside.w, 40), FL_CENTER, 0, fl_th.dim,
			           "Double click a song to play");
		}
	}

	draw_bar(ui, m, bar);

	if (m->msg[0]) {
		int mw = fl_text_width(ui->f_reg, m->msg, -1) + 32;
		if (mw > c.w - 40)
			mw = c.w - 40;
		fl_rect b = fl_r(c.x + (c.w - mw) / 2, bar.y - 50, mw, 34);
		fl_fill_rect(cv, b, fl_th.raised);
		fl_stroke_rect(cv, b, 1, m->msg_err ? fl_th.err : fl_th.line);
		fl_text_in(cv, ui->f_reg, b, FL_CENTER, 0, m->msg_err ? fl_th.err : fl_th.text, m->msg);
	}
}

/* ---- main ------------------------------------------------------------------------------- */

static void usage(void)
{
	fprintf(stderr, "usage: flmusic [--tab albums|artists|songs] [--search TEXT] [FOLDER | FILE...]\n"
	                "       flmusic --ctl play|pause|play-pause|stop|next|previous|status\n"
	                "A library of ~/music (or FOLDER). Space plays and pauses. The --ctl form talks to a running\n"
	                "player over MPRIS (for media keys).\n");
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "--ctl"))
		return mpris_ctl(argv[2]);
	if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
		usage();
		return 0;
	}
	struct M *m = calloc(1, sizeof *m);
	if (!m)
		return 1;
	m->volume = 0.8;
	m->qpos = -1;
	m->rng = (unsigned)time(NULL) ^ (unsigned)getpid();
	const char *home = getenv("HOME");
	const char *env = getenv("FLMUSIC_DIR");
	char files[64][PATH_MAX];
	int nfiles = 0;
	if (env && *env)
		snprintf(m->root, sizeof m->root, "%s", env);
	else if (home) {
		snprintf(m->root, sizeof m->root, "%s/music", home);
		struct stat st;
		if (stat(m->root, &st) < 0 || !S_ISDIR(st.st_mode)) {
			char alt[PATH_MAX];
			snprintf(alt, sizeof alt, "%s/Music", home);
			if (stat(alt, &st) == 0 && S_ISDIR(st.st_mode))
				snprintf(m->root, sizeof m->root, "%s", alt);
		}
	}
	int start_tab = TAB_ALBUMS;
	for (int i = 1; i < argc; i++) {
		struct stat st;
		char real[PATH_MAX];
		if (!strcmp(argv[i], "--tab") && i + 1 < argc) {
			start_tab = !strcmp(argv[i + 1], "artists") ? TAB_ARTISTS : !strcmp(argv[i + 1], "songs") ? TAB_SONGS : TAB_ALBUMS;
			i++;
			continue;
		}
		if (!strcmp(argv[i], "--search") && i + 1 < argc) {
			snprintf(m->search, sizeof m->search, "%s", argv[++i]);
			continue;
		}
		if (argv[i][0] == '-' || !realpath(argv[i], real) || stat(real, &st) < 0) {
			usage();
			return 2;
		}
		if (S_ISDIR(st.st_mode))
			snprintf(m->root, sizeof m->root, "%s", real);
		else if (nfiles < 64)
			snprintf(files[nfiles++], PATH_MAX, "%s", real);
	}
	const char *xc = getenv("XDG_CACHE_HOME");
	if (xc && *xc)
		snprintf(m->cache, sizeof m->cache, "%s/flmusic/library.tsv", xc);
	else
		snprintf(m->cache, sizeof m->cache, "%s/.cache/flmusic/library.tsv", home ? home : "/tmp");

	gst_init(NULL, NULL);
	m->app = fl_app_new(NULL, NULL);
	if (!m->app) {
		fprintf(stderr, "flmusic: cannot connect to a Wayland compositor\n");
		return 1;
	}
	m->small = fl_font_ui(400, 12);
	m->big = fl_font_ui(600, 16);
	m->huge = fl_font_ui(600, 26);
	m->msg_timer = fl_timer_new(m->app, msg_clear, m);
	m->scan_timer = fl_timer_new(m->app, scan_tick, m);
	m->pos_timer = fl_timer_new(m->app, pos_tick, m);
	m->show_queue = 1;
	GError *err = NULL;
	/* no discoverer: the window still opens and says why (probe() refuses without one) */
	char start_err[300] = "";
	m->disc = gst_discoverer_new(5 * GST_SECOND, &err);
	if (!m->disc) {
		snprintf(start_err, sizeof start_err, "Cannot read audio files: %s",
		         err ? err->message : "GStreamer has no discoverer");
		fprintf(stderr, "flmusic: %s\n", start_err);
		if (err)
			g_error_free(err);
		err = NULL;
	}
	if (make_pipeline(m) < 0)
		say(m, 1, "GStreamer's playbin is missing: nothing can be played");

	fl_win_opts o = {
		.title = "Music", .app_id = "flmusic", .w = 1060, .h = 640, .min_w = 520, .min_h = 420,
		.decorated = 1, .resizable = 1, .draw = draw, .ud = m,
	};
	m->win = fl_win_new(m->app, &o);
	if (!m->win)
		return 1;

	struct mpris_ops ops = {
		.ud = m, .play = mp_play, .pause = mp_pause, .playpause = mp_playpause, .stop = mp_stop, .next = mp_next,
		.prev = mp_prev, .seek = mp_seek, .set_position = mp_setpos, .set_volume = mp_setvol,
		.set_shuffle = mp_shuffle, .set_loop = mp_loop, .open_uri = mp_open, .quit = mp_quit, .get = mp_get,
	};
	m->mpris = mpris_start(m->app, &ops);

	m->tab = start_tab;
	if (start_err[0])
		say(m, 1, "%s", start_err);
	cache_load(m);
	if (m->root[0])
		walk(m, m->root, 0);
	m->scanning = m->todo.n > 0;
	build_index(m);
	if (m->scanning)
		fl_timer_arm(m->scan_timer, 1, 1);
	else if (ncent != m->nt)
		cache_save(m);

	if (nfiles) {
		int first = -1, bad = 0;
		char badname[300] = "";
		int *tr = malloc((size_t)nfiles * sizeof(int)), n = 0;
		for (int i = 0; i < nfiles; i++) {
			struct stat st;
			struct track t;
			if (stat(files[i], &st) < 0 || probe(m, files[i], (long long)st.st_mtime, (long long)st.st_size, &t) < 0) {
				/* said below: a file the command line named must not vanish without a word */
				if (!bad)
					snprintf(badname, sizeof badname, "%s", strrchr(files[i], '/') ? strrchr(files[i], '/') + 1 : files[i]);
				bad++;
				continue;
			}
			int ti = add_track(m, &t);
			if (ti >= 0)
				tr[n++] = ti;
		}
		if (n)
			first = 0;
		if (first >= 0)
			queue_play(m, tr, n, 0);
		free(tr);
		build_index(m);
		if (bad)
			say(m, 1, bad == 1 ? "Cannot play %s" : "Cannot play %s and %d other file(s)", badname, bad - 1);
	}
	fl_app_watch_conf(m->app, NULL, NULL);
	fl_app_run(m->app);
	if (m->pipe)
		gst_element_set_state(m->pipe, GST_STATE_NULL);
	mpris_stop(m->mpris);
	fl_app_free(m->app);
	return 0;
}
