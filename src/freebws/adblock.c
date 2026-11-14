/* adblock -- see adblock.h. SPDX-License-Identifier: MIT
 *
 * Moving parts
 *   lists      share_dir()/lists/lists.conf names them (name TAB url TAB license TAB title); the
 *              files are share_dir()/lists/<name>.txt, or a newer copy in
 *              data_dir()/adblock/lists/<name>.txt that the weekly update downloaded.
 *   build      (child process) abp.c converts all lists; the network rules go to WebKit's
 *              content rule list store (cache_dir()/filters) in chunks, the cosmetic parts to
 *              data_dir()/adblock/built/{generic.txt,site.tsv}; adblock/state records which lists
 *              (signature) the compiled rules are for.
 *   browser    loads the compiled chunks (a memory map, not a compile) and puts them in every
 *              tab's content manager; on every main-frame navigation it makes the style sheet
 *              (generic selectors minus the site's exceptions, plus the site's own) and the
 *              script (scriptlet calls of the site) for that host and swaps them in.
 */
#include "adblock.h"

#include <errno.h>
#include <libsoup/soup.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include "abp.h"
#include "config.h"

#define CHUNK_RULES 50000
#define UPDATE_EVERY (7 * 24 * 3600)
#define RETRY_EVERY (6 * 3600)
#define GATE_TIMEOUT_S 120

struct list {
	char *name, *url, *license, *title, *path;
	gint64 size;
	time_t mtime;
};

/* never blocked, whatever the lists say: the Cloudflare challenge (Turnstile) the check pages load in a
 * third-party frame; a blocked challenge leaves "Verify you are human" waiting for ever */
#define BUILTIN_ALLOW "@@||challenges.cloudflare.com^\n@@||cloudflare.com/cdn-cgi/challenge-platform/\n"

static struct A {
	GPtrArray *lists;
	char *adb_dir, *built_dir, *lists_dir, *store_path;
	WebKitUserContentFilterStore *store;
	GPtrArray *filters;		/* WebKitUserContentFilter * in use */
	int ready;			/* filters loaded (or nothing to load) */
	int loading;
	int building;			/* a child is running */
	GPid child_pid;
	int child_is_update;
	char sig[65];			/* signature of the lists on disk now */
	char loaded_sig[65];		/* ... of the rules loaded */
	unsigned rules;
	gint64 updated, tried;
	unsigned lists_n;
	guint gate_timer, week_timer;
	/* cosmetic tables */
	char *site_map;
	gsize site_len;
	char *generic_buf;
	GPtrArray *generic;		/* const char *, into generic_buf */
	char *generic_css;		/* all generic selectors as style rules */
	char *scriptlets_lib;		/* function(calls){...} */
	char *yt_js;
	char *lastlog;
	int enabled;
	int verbose;
	const char *skip;		/* FREEBWS_ADB_SKIP=yt,js,css,net: leave a part out (finding what breaks a site) */
} A;
#define SKIP(w) (A.skip && strstr(A.skip, w))

static void logf_(const char *fmt, ...) G_GNUC_PRINTF(1, 2);
static void logf_(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char *m = g_strdup_vprintf(fmt, ap);
	va_end(ap);
	fprintf(stderr, "freebws: adblock: %s\n", m);
	g_free(m);
}

/* ---- lists and the state file -------------------------------------------------------- */

static void paths_init(void)
{
	if (A.adb_dir)
		return;
	A.adb_dir = g_build_filename(data_dir(), "adblock", NULL);
	A.built_dir = g_build_filename(A.adb_dir, "built", NULL);
	A.lists_dir = g_build_filename(A.adb_dir, "lists", NULL);
	A.store_path = g_build_filename(cache_dir(), "filters", NULL);
	g_mkdir_with_parents(A.built_dir, 0700);
	g_mkdir_with_parents(A.lists_dir, 0700);
}

static void list_free(gpointer p)
{
	struct list *l = p;
	g_free(l->name);
	g_free(l->url);
	g_free(l->license);
	g_free(l->title);
	g_free(l->path);
	g_free(l);
}

/* the lists named in lists.conf, each with the file to use: the newer of the shipped one and
 * the downloaded one */
static void lists_load(void)
{
	if (A.lists)
		g_ptr_array_free(A.lists, TRUE);
	A.lists = g_ptr_array_new_with_free_func(list_free);
	char *conf = g_build_filename(share_dir(), "lists", "lists.conf", NULL), *text = NULL;
	if (g_file_get_contents(conf, &text, NULL, NULL)) {
		char **lines = g_strsplit(text, "\n", 0);
		for (int i = 0; lines[i]; i++) {
			if (!*lines[i] || *lines[i] == '#')
				continue;
			char **f = g_strsplit(lines[i], "\t", 4);
			if (f[0] && f[1] && f[2]) {
				struct list *l = g_new0(struct list, 1);
				l->name = g_strdup(f[0]);
				l->url = g_strdup(f[1]);
				l->license = g_strdup(f[2]);
				l->title = g_strdup(f[3] ? f[3] : f[0]);
				char *ship = g_strdup_printf("%s/lists/%s.txt", share_dir(), l->name);
				char *user = g_strdup_printf("%s/%s.txt", A.lists_dir, l->name);
				struct stat a, b;
				int has_a = !stat(ship, &a), has_b = !stat(user, &b);
				if (has_b && (!has_a || b.st_mtime > a.st_mtime)) {
					l->path = user;
					l->size = b.st_size;
					l->mtime = b.st_mtime;
					g_free(ship);
				} else if (has_a) {
					l->path = ship;
					l->size = a.st_size;
					l->mtime = a.st_mtime;
					g_free(user);
				} else {
					g_free(ship);
					g_free(user);
				}
				if (l->path)
					g_ptr_array_add(A.lists, l);
				else
					list_free(l);
			}
			g_strfreev(f);
		}
		g_strfreev(lines);
		g_free(text);
	}
	g_free(conf);
	GChecksum *cs = g_checksum_new(G_CHECKSUM_SHA256);
	char b[128];
	snprintf(b, sizeof b, "abp%d\n%s", ABP_CONVERTER_VERSION, BUILTIN_ALLOW);
	g_checksum_update(cs, (guchar *)b, -1);
	for (guint i = 0; i < A.lists->len; i++) {
		struct list *l = g_ptr_array_index(A.lists, i);
		snprintf(b, sizeof b, "%s:%lld:%lld\n", l->name, (long long)l->size, (long long)l->mtime);
		g_checksum_update(cs, (guchar *)b, -1);
	}
	g_strlcpy(A.sig, g_checksum_get_string(cs), sizeof A.sig);
	g_checksum_free(cs);
	A.lists_n = A.lists->len;
}

static char *state_path(void) { return g_build_filename(A.adb_dir, "state", NULL); }

struct state {
	char sig[65];
	int chunks;
	unsigned rules;
	gint64 updated, tried;
};

static void state_read(struct state *s)
{
	memset(s, 0, sizeof *s);
	char *p = state_path(), *text = NULL;
	if (g_file_get_contents(p, &text, NULL, NULL)) {
		char **l = g_strsplit(text, "\n", 0);
		for (int i = 0; l[i]; i++) {
			if (!strncmp(l[i], "sig=", 4))
				g_strlcpy(s->sig, l[i] + 4, sizeof s->sig);
			else if (!strncmp(l[i], "chunks=", 7))
				s->chunks = atoi(l[i] + 7);
			else if (!strncmp(l[i], "rules=", 6))
				s->rules = (unsigned)atoi(l[i] + 6);
			else if (!strncmp(l[i], "updated=", 8))
				s->updated = g_ascii_strtoll(l[i] + 8, NULL, 10);
			else if (!strncmp(l[i], "tried=", 6))
				s->tried = g_ascii_strtoll(l[i] + 6, NULL, 10);
		}
		g_strfreev(l);
		g_free(text);
	}
	g_free(p);
}

static void state_write(const struct state *s)
{
	char *t = g_strdup_printf("sig=%s\nchunks=%d\nrules=%u\nupdated=%lld\ntried=%lld\n", s->sig, s->chunks, s->rules,
				  (long long)s->updated, (long long)s->tried);
	char *p = state_path();
	write_file_atomic(p, t, strlen(t));
	g_free(p);
	g_free(t);
}

static char *chunk_id(const char *sig, int i) { return g_strdup_printf("fb-%.10s-%d", sig, i); }

/* ---- the child: download lists, convert, compile ------------------------------------------ */

static GMainLoop *c_loop;
static int c_rc;
static int c_chunks, c_next;
static abp *c_abp;
static unsigned c_rules;
static WebKitUserContentFilterStore *c_store;
static struct state c_state;

static void c_save_next(void);

static void c_finish(void)
{
	c_state.chunks = c_chunks;
	c_state.rules = c_rules;
	state_write(&c_state);
	/* the old compiled rules are not needed any more */
	GFileEnumerator *en = NULL;
	GFile *d = g_file_new_for_path(A.store_path);
	en = g_file_enumerate_children(d, "standard::name", 0, NULL, NULL);
	GFileInfo *fi;
	char *keep = g_strdup_printf("ContentRuleList-fb-%.10s-", c_state.sig);
	while (en && (fi = g_file_enumerator_next_file(en, NULL, NULL))) {
		const char *n = g_file_info_get_name(fi);
		if (g_str_has_prefix(n, "ContentRuleList-fb-") && !g_str_has_prefix(n, keep)) {
			char *p = g_build_filename(A.store_path, n, NULL);
			unlink(p);
			g_free(p);
		}
		g_object_unref(fi);
	}
	g_free(keep);
	if (en)
		g_object_unref(en);
	g_object_unref(d);
	g_main_loop_quit(c_loop);
}

static void c_saved(GObject *o, GAsyncResult *r, gpointer d)
{
	GError *e = NULL;
	WebKitUserContentFilter *f = webkit_user_content_filter_store_save_from_file_finish(c_store, r, &e);
	if (!f) {
		fprintf(stderr, "freebws: adblock: compiling chunk %d failed: %s\n", c_next, e ? e->message : "?");
		g_clear_error(&e);
		c_rc = 1;
		g_main_loop_quit(c_loop);
		return;
	}
	webkit_user_content_filter_unref(f);
	{
		char *path = g_strdup_printf("%s/net-%d.json", A.built_dir, c_next);
		unlink(path);
		g_free(path);
	}
	c_next++;
	c_save_next();
}

static void c_save_next(void)
{
	if (c_next >= c_chunks) {
		c_finish();
		return;
	}
	char *id = chunk_id(c_state.sig, c_next);
	char *path = g_strdup_printf("%s/net-%d.json", A.built_dir, c_next);
	GFile *f = g_file_new_for_path(path);
	fprintf(stderr, "freebws: adblock: compiling %s\n", id);
	webkit_user_content_filter_store_save_from_file(c_store, id, f, NULL, c_saved, NULL);
	g_object_unref(f);
	g_free(path);
	g_free(id);
}

static int build_lists(void)
{
	c_abp = abp_new();
	for (guint i = 0; i < A.lists->len; i++) {
		struct list *l = g_ptr_array_index(A.lists, i);
		gchar *t;
		gsize n;
		if (!g_file_get_contents(l->path, &t, &n, NULL))
			continue;
		abp_add_list(c_abp, t, n);
		g_free(t);
	}
	abp_add_list(c_abp, BUILTIN_ALLOW, strlen(BUILTIN_ALLOW));
	abp_finish(c_abp);
	const struct abp_stats *st = abp_stats(c_abp);
	fprintf(stderr, "freebws: adblock: %u lines: network block %u, exception %u, important %u, skipped %u; "
		"cosmetic generic %u, site %u; scriptlets %u\n", st->lines, st->net_block, st->net_except, st->net_important,
		st->net_skipped, st->cos_generic, st->cos_site, st->scriptlets);
	char *g = abp_generic(c_abp), *s = abp_site_table(c_abp);
	char *pg = g_build_filename(A.built_dir, "generic.txt", NULL), *ps = g_build_filename(A.built_dir, "site.tsv", NULL);
	int rc = write_file_atomic(pg, g, strlen(g)) || write_file_atomic(ps, s, strlen(s));
	g_free(pg);
	g_free(ps);
	g_free(g);
	g_free(s);
	if (rc)
		return -1;
	/* the rules go to files first so that the converter's memory is gone before the compiler runs */
	c_chunks = abp_chunks(c_abp, CHUNK_RULES);
	for (int i = 0; i < c_chunks; i++) {
		unsigned n = 0;
		char *json = abp_chunk_json(c_abp, CHUNK_RULES, i, &n);
		char *path = g_strdup_printf("%s/net-%d.json", A.built_dir, i);
		int wr = write_file_atomic(path, json, strlen(json));
		c_rules += n;
		g_free(path);
		g_free(json);
		if (wr)
			return -1;
	}
	abp_free(c_abp);
	c_abp = NULL;
	return 0;
}

/* download one list if the server has a newer one; 1 = replaced, 0 = same, -1 = failed */
static int fetch_list(SoupSession *ss, struct list *l)
{
	char *dst = g_strdup_printf("%s/%s.txt", A.lists_dir, l->name);
	char *etag_p = g_strdup_printf("%s/%s.etag", A.lists_dir, l->name);
	char *etag = NULL;
	struct stat st;
	if (!stat(dst, &st) || !strcmp(l->path, dst))
		g_file_get_contents(etag_p, &etag, NULL, NULL);
	SoupMessage *m = soup_message_new("GET", l->url);
	if (!m) {
		g_free(dst);
		g_free(etag_p);
		g_free(etag);
		return -1;
	}
	if (etag && *etag)
		soup_message_headers_append(soup_message_get_request_headers(m), "If-None-Match", g_strstrip(etag));
	GError *e = NULL;
	GBytes *b = soup_session_send_and_read(ss, m, NULL, &e);
	int rc = -1;
	guint status = soup_message_get_status(m);
	if (b && status == 304) {
		rc = 0;
	} else if (b && status == 200) {
		gsize n;
		const char *d = g_bytes_get_data(b, &n);
		/* a list, not an error page; and not much smaller than the one we have */
		int looks = n > 2000 && (memchr(d, '!', n < 4096 ? n : 4096) != NULL);
		if (looks && (l->size == 0 || (gint64)n * 10 > l->size * 5)) {
			if (!write_file_atomic(dst, d, n)) {
				const char *et = soup_message_headers_get_one(soup_message_get_response_headers(m), "ETag");
				if (et)
					write_file_atomic(etag_p, et, strlen(et));
				rc = 1;
			}
		} else {
			fprintf(stderr, "freebws: adblock: %s: download does not look like the list (%zu bytes), ignored\n", l->name, n);
		}
	} else {
		fprintf(stderr, "freebws: adblock: %s: %s\n", l->name, e ? e->message : "HTTP error");
	}
	g_clear_error(&e);
	if (b)
		g_bytes_unref(b);
	g_object_unref(m);
	g_free(dst);
	g_free(etag_p);
	g_free(etag);
	return rc;
}

int adb_child_main(const char *mode)
{
	paths_init();
	if (nice(10) < 0) { }
	/* if memory runs out, the kernel should take this process, not the browser */
	FILE *oom = fopen("/proc/self/oom_score_adj", "w");
	if (oom) {
		fputs("1000\n", oom);
		fclose(oom);
	}
	lists_load();
	struct state st;
	state_read(&st);
	c_state = st;
	time_t now = time(NULL);
	int changed = 0;
	if (!strcmp(mode, "update")) {
		SoupSession *ss = soup_session_new_with_options("timeout", 25u, "user-agent", "FreeBWS-lists/0.2", NULL);
		int ok = 0, fail = 0;
		for (guint i = 0; i < A.lists->len; i++) {
			int r = fetch_list(ss, g_ptr_array_index(A.lists, i));
			if (r > 0)
				changed++, ok++;
			else if (r == 0)
				ok++;
			else
				fail++;
		}
		g_object_unref(ss);
		c_state.tried = now;
		if (ok && !fail)
			c_state.updated = now;
		fprintf(stderr, "freebws: adblock: update: %d list(s) newer, %d unchanged or failed\n", changed, (int)A.lists->len - changed);
		if (changed)
			lists_load();	/* the downloaded files are now the newer ones */
		if (!changed) {
			state_write(&c_state);
			return 10;
		}
	}
	if (!strcmp(c_state.sig, A.sig) && strcmp(mode, "force")) {
		state_write(&c_state);
		return 10;
	}
	g_strlcpy(c_state.sig, A.sig, sizeof c_state.sig);
	if (build_lists())
		return 1;
	g_mkdir_with_parents(A.store_path, 0700);
	c_store = webkit_user_content_filter_store_new(A.store_path);
	c_loop = g_main_loop_new(NULL, FALSE);
	c_next = 0;
	c_save_next();
	g_main_loop_run(c_loop);
	return c_rc;
}

/* ---- the browser: tables ---------------------------------------------------------------- */

static char *read_share(const char *name)
{
	char *p = g_build_filename(share_dir(), name, NULL), *t = NULL;
	if (!g_file_get_contents(p, &t, NULL, NULL))
		t = NULL;
	g_free(p);
	return t;
}

static void tables_free(void)
{
	if (A.site_map)
		munmap(A.site_map, A.site_len);
	A.site_map = NULL;
	A.site_len = 0;
	g_free(A.generic_buf);
	A.generic_buf = NULL;
	if (A.generic)
		g_ptr_array_free(A.generic, TRUE);
	A.generic = NULL;
	g_free(A.generic_css);
	A.generic_css = NULL;
}

#define GROUP 16

static void css_group(GString *o, GPtrArray *sels, GHashTable *skip)
{
	int n = 0;
	for (guint i = 0; i < sels->len; i++) {
		const char *s = g_ptr_array_index(sels, i);
		if (skip && g_hash_table_contains(skip, s))
			continue;
		if (n == GROUP) {
			g_string_append(o, "{display:none!important}\n");
			n = 0;
		}
		g_string_append_printf(o, "%s%s", n ? "," : "", s);
		n++;
	}
	if (n)
		g_string_append(o, "{display:none!important}\n");
}

static void tables_load(void)
{
	tables_free();
	char *p = g_build_filename(A.built_dir, "site.tsv", NULL);
	int fd = open(p, O_RDONLY | O_CLOEXEC);
	g_free(p);
	if (fd >= 0) {
		struct stat st;
		if (!fstat(fd, &st) && st.st_size > 0) {
			void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
			if (m != MAP_FAILED) {
				A.site_map = m;
				A.site_len = (gsize)st.st_size;
			}
		}
		close(fd);
	}
	p = g_build_filename(A.built_dir, "generic.txt", NULL);
	gsize n = 0;
	if (g_file_get_contents(p, &A.generic_buf, &n, NULL)) {
		A.generic = g_ptr_array_new();
		char *s = A.generic_buf;
		while (s < A.generic_buf + n) {
			char *nl = memchr(s, '\n', (gsize)(A.generic_buf + n - s));
			if (!nl)
				break;
			*nl = 0;
			if (*s)
				g_ptr_array_add(A.generic, s);
			s = nl + 1;
		}
		GString *o = g_string_new(NULL);
		css_group(o, A.generic, NULL);
		A.generic_css = g_string_free(o, FALSE);
	}
	g_free(p);
	if (A.verbose)
		logf_("tables: site %zu bytes, %u generic selectors", A.site_len, A.generic ? A.generic->len : 0);
}

struct host_info {
	int nofilter, elemhide, generichide, specifichide, nojs;
	GPtrArray *hide;	/* selectors to hide (char *, owned) */
	GHashTable *unhide;	/* selectors not to hide */
	GPtrArray *css;		/* style rules as they are */
	GPtrArray *js;		/* "name\targ\targ" */
};

static int key_cmp(const char *line, const char *key, size_t kl)
{
	size_t ll = strcspn(line, "\t\n");
	size_t m = ll < kl ? ll : kl;
	int c = memcmp(line, key, m);
	if (c)
		return c;
	return ll < kl ? -1 : ll > kl ? 1 : 0;
}

static void site_collect(const char *key, struct host_info *hi)
{
	if (!A.site_map)
		return;
	size_t kl = strlen(key), lo = 0, hi_ = A.site_len;
	const char *base = A.site_map, *end = base + A.site_len;
	while (lo < hi_) {
		size_t mid = lo + (hi_ - lo) / 2;
		const char *ls = base + mid;
		while (ls > base + lo && ls[-1] != '\n')
			ls--;
		const char *le = memchr(ls, '\n', (size_t)(end - ls));
		le = le ? le : end;
		if (key_cmp(ls, key, kl) < 0)
			lo = (size_t)(le + 1 - base);
		else
			hi_ = (size_t)(ls - base);
	}
	const char *p = base + lo;
	while (p < end && key_cmp(p, key, kl) == 0) {
		const char *le = memchr(p, '\n', (size_t)(end - p));
		le = le ? le : end;
		const char *v = p + kl + 1;
		char *val = g_strndup(v, (gsize)(le - v));
		if (!strcmp(val, "@nofilter"))
			hi->nofilter = 1;
		else if (!strcmp(val, "@elemhide"))
			hi->elemhide = 1;
		else if (!strcmp(val, "@generichide"))
			hi->generichide = 1;
		else if (!strcmp(val, "@specifichide"))
			hi->specifichide = 1;
		else if (!strcmp(val, "-js"))
			hi->nojs = 1;
		else if (!strncmp(val, "+js\t", 4))
			g_ptr_array_add(hi->js, g_strdup(val + 4));
		else if (val[0] == '$')
			g_ptr_array_add(hi->css, g_strdup(val + 1));
		else if (val[0] == '!')
			g_hash_table_add(hi->unhide, g_strdup(val + 1));
		else
			g_ptr_array_add(hi->hide, g_strdup(val));
		g_free(val);
		p = le + 1;
	}
}

/* all keys that apply to a host: it and its parents, entities (name.*), and "*" */
static void host_collect(const char *host, struct host_info *hi)
{
	char **lab = g_strsplit(host, ".", 0);
	int n = (int)g_strv_length(lab);
	for (int i = 0; i < n; i++) {
		char *k = g_strjoinv(".", lab + i);
		site_collect(k, hi);
		g_free(k);
		for (int r = 1; r <= 2; r++)
			if (n - r > i) {
				char save = 0;
				GString *e = g_string_new(NULL);
				for (int j = i; j < n - r; j++)
					g_string_append_printf(e, "%s%s", j > i ? "." : "", lab[j]);
				g_string_append(e, ".*");
				(void)save;
				site_collect(e->str, hi);
				g_string_free(e, TRUE);
			}
	}
	site_collect("*", hi);
	g_strfreev(lab);
}

static void json_q(GString *o, const char *s)
{
	g_string_append_c(o, '"');
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\')
			g_string_append_printf(o, "\\%c", c);
		else if (c < 0x20)
			g_string_append_printf(o, "\\u%04x", c);
		else
			g_string_append_c(o, (char)c);
	}
	g_string_append_c(o, '"');
}

/* ---- the browser: tabs --------------------------------------------------------------------- */

static void filters_unload(void)
{
	if (A.filters)
		g_ptr_array_free(A.filters, TRUE);
	A.filters = NULL;
}

static gboolean gate_timeout(gpointer d)
{
	A.gate_timer = 0;
	if (!A.ready) {
		logf_("the filters were not ready after %d s: pages load without them for now", GATE_TIMEOUT_S);
		A.ready = 1;
		fb_adb_changed();
	}
	return G_SOURCE_REMOVE;
}

struct load_job {
	struct state st;
	int pending, failed;
	GPtrArray *got;
};

static void load_done(struct load_job *j)
{
	filters_unload();
	if (j->failed) {
		logf_("a compiled filter is missing, rebuilding");
		g_ptr_array_free(j->got, TRUE);
		A.loading = 0;
		g_free(j);
		A.loaded_sig[0] = 0;
		adb_update_now();
		return;
	}
	A.filters = j->got;
	g_strlcpy(A.loaded_sig, j->st.sig, sizeof A.loaded_sig);
	A.rules = j->st.rules;
	A.ready = 1;
	A.loading = 0;
	tables_load();
	if (A.gate_timer) {
		g_source_remove(A.gate_timer);
		A.gate_timer = 0;
	}
	logf_("%u filter chunk(s) loaded (%u rules)", A.filters->len, j->st.rules);
	g_free(j);
	fb_adb_changed();
}

static void load_one(GObject *o, GAsyncResult *r, gpointer d)
{
	struct load_job *j = d;
	GError *e = NULL;
	WebKitUserContentFilter *f = webkit_user_content_filter_store_load_finish(A.store, r, &e);
	if (f)
		g_ptr_array_add(j->got, f);
	else
		j->failed = 1;
	g_clear_error(&e);
	if (--j->pending == 0)
		load_done(j);
}

static void filters_load(void)
{
	if (A.loading)
		return;
	struct state st;
	state_read(&st);
	if (!st.sig[0] || st.chunks <= 0) {
		A.loaded_sig[0] = 0;
		return;
	}
	if (!A.store)
		A.store = webkit_user_content_filter_store_new(A.store_path);
	A.loading = 1;
	struct load_job *j = g_new0(struct load_job, 1);
	j->st = st;
	j->pending = st.chunks;
	j->got = g_ptr_array_new_with_free_func((GDestroyNotify)webkit_user_content_filter_unref);
	for (int i = 0; i < st.chunks; i++) {
		char *id = chunk_id(st.sig, i);
		webkit_user_content_filter_store_load(A.store, id, NULL, load_one, j);
		g_free(id);
	}
}

static void child_exit(GPid pid, gint status, gpointer d)
{
	g_spawn_close_pid(pid);
	A.building = 0;
	int rc = g_spawn_check_wait_status(status, NULL) ? 0 : (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	logf_("the %s process ended (%d)", A.child_is_update ? "update" : "build", rc);
	if (rc == 0 || rc == 10) {
		lists_load();
		struct state st;
		state_read(&st);
		A.updated = st.updated;
		A.tried = st.tried;
		if (strcmp(st.sig, A.loaded_sig))
			filters_load();
		else if (!A.ready) {
			A.ready = 1;
			fb_adb_changed();
		}
	} else if (!A.ready) {
		A.ready = 1;	/* no filters: do not hold the pages for ever */
		fb_adb_changed();
	}
}

static unsigned long mem_avail_mb(void)
{
	FILE *f = fopen("/proc/meminfo", "r");
	char line[128];
	unsigned long kb = 0;
	while (f && fgets(line, sizeof line, f))
		if (sscanf(line, "MemAvailable: %lu kB", &kb) == 1)
			break;
	if (f)
		fclose(f);
	return kb / 1024;
}

/* background rebuilds (list updates) wait for memory: the compiler needs about 400 MB for a while */
#define BUILD_NEEDS_MB 650

static void spawn_child(const char *mode)
{
	if (A.building)
		return;
	if (A.ready && A.filters && mem_avail_mb() < BUILD_NEEDS_MB) {
		logf_("not enough free memory to rebuild the filters now, later");
		return;
	}
	char *self = g_file_read_link("/proc/self/exe", NULL);
	if (!self)
		return;
	const char *flag = !strcmp(mode, "update") ? "--adblock-update" : "--adblock-build";
	char *argv[] = { self, (char *)flag, NULL };
	GError *e = NULL;
	if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, &A.child_pid, &e)) {
		logf_("cannot start the %s process: %s", mode, e->message);
		g_clear_error(&e);
		g_free(self);
		return;
	}
	g_free(self);
	A.building = 1;
	A.child_is_update = !strcmp(mode, "update");
	g_child_watch_add(A.child_pid, child_exit, NULL);
	logf_("%s process started", mode);
}

void adb_update_now(void)
{
	spawn_child("update");
}

static gboolean week_tick(gpointer d)
{
	if (!A.enabled)
		return G_SOURCE_CONTINUE;
	time_t now = time(NULL);
	if (!A.building && now - A.updated > UPDATE_EVERY && now - A.tried > RETRY_EVERY)
		spawn_child("update");
	return G_SOURCE_CONTINUE;
}

static gboolean update_cb(gpointer d)
{
	adb_update_now();
	return G_SOURCE_REMOVE;
}

static gboolean first_check(gpointer d)
{
	week_tick(NULL);
	return G_SOURCE_REMOVE;
}

void adb_init(void)
{
	paths_init();
	A.enabled = cfg_int("adblock", 1);
	A.verbose = getenv("FREEBWS_ADB_LOG") != NULL;
	A.skip = getenv("FREEBWS_ADB_SKIP");
	A.scriptlets_lib = read_share("scriptlets.js");
	{	/* the YouTube script and the block counter: one static script in every frame */
		char *yt = read_share("yt-ads.js"), *cnt = read_share("count.js");
		A.yt_js = yt || cnt ? g_strconcat(yt ? yt : "", "\n", cnt ? cnt : "", NULL) : NULL;
		g_free(yt);
		g_free(cnt);
	}
	lists_load();
	struct state st;
	state_read(&st);
	A.updated = st.updated;
	A.tried = st.tried;
	if (!A.enabled) {
		A.ready = 1;
		return;
	}
	if (!A.lists_n) {
		logf_("no filter lists found in %s/lists", share_dir());
		A.ready = 1;
		return;
	}
	if (st.sig[0] && st.chunks > 0) {
		filters_load();	/* what we have protects at once, even if the lists are newer */
		if (strcmp(st.sig, A.sig))
			spawn_child("build");
	} else {
		spawn_child("build");	/* first start: pages wait for it (with a limit) */
		A.gate_timer = g_timeout_add_seconds(GATE_TIMEOUT_S, gate_timeout, NULL);
	}
	if (!A.week_timer)
		A.week_timer = g_timeout_add_seconds(1800, week_tick, NULL);
	if (getenv("FREEBWS_ADB_UPDATE") && atoi(getenv("FREEBWS_ADB_UPDATE")))
		g_timeout_add_seconds(5, update_cb, NULL);
	else
		g_timeout_add_seconds(60, first_check, NULL);
}

void adb_shutdown(void)
{
	filters_unload();
	tables_free();
}

int adb_enabled(void) { return A.enabled; }
int adb_ready(void) { return A.ready || !A.enabled; }
int adb_building(void) { return A.building && !A.ready; }

void adb_set_enabled(int on)
{
	on = on != 0;
	if (on == A.enabled)
		return;
	A.enabled = on;
	cfg_set_int("adblock", on);
	if (on) {
		A.ready = 0;
		if (!A.filters) {
			struct state st;
			state_read(&st);
			if (st.sig[0] && st.chunks > 0)
				filters_load();
			else
				spawn_child("build");
		} else {
			A.ready = 1;
		}
		if (!A.week_timer)
			A.week_timer = g_timeout_add_seconds(1800, week_tick, NULL);
	}
	fb_adb_changed();
}

static int host_is_or_under(const char *host, const char *dom)
{
	size_t hl = strlen(host), dl = strlen(dom);
	if (hl < dl)
		return 0;
	if (strcmp(host + hl - dl, dom))
		return 0;
	return hl == dl || host[hl - dl - 1] == '.';
}

int adb_site_off(const char *host)
{
	if (!host || !*host)
		return 0;
	char **v = g_strsplit(cfg_get("adblock_off", ""), ",", 0);
	int off = 0;
	for (int i = 0; v[i]; i++)
		if (*v[i] && host_is_or_under(host, v[i]))
			off = 1;
	g_strfreev(v);
	return off;
}

void adb_set_site_off(const char *host, int off)
{
	if (!host || !*host)
		return;
	cfg_list_set("adblock_off", host, off);
	fb_adb_changed();
}

char *adb_status_json(void)
{
	GString *o = g_string_new("{");
	g_string_append_printf(o, "\"enabled\":%d,\"ready\":%d,\"building\":%d,\"rules\":%u,\"chunks\":%u,\"lists\":[",
			       A.enabled, A.ready, A.building, A.rules, A.filters ? A.filters->len : 0);
	for (guint i = 0; i < A.lists_n && A.lists && i < A.lists->len; i++) {
		struct list *l = g_ptr_array_index(A.lists, i);
		g_string_append_printf(o, "%s{\"name\":", i ? "," : "");
		json_q(o, l->title);
		g_string_append(o, ",\"license\":");
		json_q(o, l->license);
		g_string_append_printf(o, ",\"kb\":%lld}", (long long)(l->size / 1024));
	}
	g_string_append_printf(o, "],\"updated\":%lld,\"tried\":%lld,\"off\":", (long long)A.updated, (long long)A.tried);
	json_q(o, cfg_get("adblock_off", ""));
	g_string_append_c(o, '}');
	return g_string_free(o, FALSE);
}

static void tab_clear_content(struct adb_tab *s, WebKitUserContentManager *ucm)
{
	if (s->sheet) {
		webkit_user_content_manager_remove_style_sheet(ucm, s->sheet);
		webkit_user_style_sheet_unref(s->sheet);
		s->sheet = NULL;
	}
	if (s->script) {
		webkit_user_content_manager_remove_script(ucm, s->script);
		webkit_user_script_unref(s->script);
		s->script = NULL;
	}
}

void adb_tab_refilter(struct adb_tab *s, WebKitUserContentManager *ucm)
{
	if (!ucm)
		return;
	int want = A.enabled && !s->off && A.filters && A.filters->len && !SKIP("net");
	if (s->filters_on) {
		webkit_user_content_manager_remove_all_filters(ucm);
		s->filters_on = 0;
	}
	if (want) {
		for (guint i = 0; i < A.filters->len; i++)
			webkit_user_content_manager_add_filter(ucm, g_ptr_array_index(A.filters, i));
		s->filters_on = 1;
	}
	int yt = A.enabled && !s->off && A.yt_js && !SKIP("yt");
	if (yt && !s->yt) {
		s->yt = webkit_user_script_new(A.yt_js, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
					       WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
		webkit_user_content_manager_add_script(ucm, s->yt);
	} else if (!yt && s->yt) {
		webkit_user_content_manager_remove_script(ucm, s->yt);
		webkit_user_script_unref(s->yt);
		s->yt = NULL;
	}
	if (!A.enabled || s->off) {
		tab_clear_content(s, ucm);
		s->host[0] = 0;
	} else {
		s->host[0] = 0;	/* make the page's sheet and script again at the next navigation */
	}
}

void adb_tab_init(struct adb_tab *s, WebKitUserContentManager *ucm)
{
	memset(s, 0, sizeof *s);
	adb_tab_refilter(s, ucm);
}

void adb_tab_free(struct adb_tab *s, WebKitUserContentManager *ucm)
{
	if (ucm) {
		tab_clear_content(s, ucm);
		if (s->yt)
			webkit_user_content_manager_remove_script(ucm, s->yt);
	}
	if (s->yt)
		webkit_user_script_unref(s->yt);
	g_free(s->deferred);
	memset(s, 0, sizeof *s);
}

/* the style sheet and the scriptlet script for a host */
static void make_page_content(struct adb_tab *s, WebKitUserContentManager *ucm, const char *host)
{
	struct host_info hi = { 0 };
	hi.hide = g_ptr_array_new_with_free_func(g_free);
	hi.unhide = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	hi.css = g_ptr_array_new_with_free_func(g_free);
	hi.js = g_ptr_array_new_with_free_func(g_free);
	host_collect(host, &hi);
	if (hi.nofilter) {	/* the lists whitelist this site as a whole */
		webkit_user_content_manager_remove_all_filters(ucm);
		s->filters_on = 0;
	} else if (!s->filters_on && A.filters && A.filters->len && !SKIP("net")) {
		for (guint i = 0; i < A.filters->len; i++)
			webkit_user_content_manager_add_filter(ucm, g_ptr_array_index(A.filters, i));
		s->filters_on = 1;
	}
	GString *css = g_string_new(NULL);
	if (!hi.nofilter && !hi.elemhide && !SKIP("css")) {
		if (!hi.generichide && A.generic) {
			if (g_hash_table_size(hi.unhide))
				css_group(css, A.generic, hi.unhide);
			else if (A.generic_css)
				g_string_append(css, A.generic_css);
		}
		if (!hi.specifichide) {
			css_group(css, hi.hide, hi.unhide);
			for (guint i = 0; i < hi.css->len; i++)
				g_string_append_printf(css, "%s\n", (char *)g_ptr_array_index(hi.css, i));
		}
	}
	if (css->len) {
		s->sheet = webkit_user_style_sheet_new(css->str, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
						       WEBKIT_USER_STYLE_LEVEL_USER, NULL, NULL);
		webkit_user_content_manager_add_style_sheet(ucm, s->sheet);
	}
	if (!hi.nofilter && !hi.nojs && hi.js->len && A.scriptlets_lib && !SKIP("js")) {
		GString *calls = g_string_new("[");
		for (guint i = 0; i < hi.js->len; i++) {
			char **f = g_strsplit(g_ptr_array_index(hi.js, i), "\t", 0);
			g_string_append_printf(calls, "%s[", i ? "," : "");
			for (int k = 0; f[k]; k++) {
				if (k)
					g_string_append_c(calls, ',');
				json_q(calls, f[k]);
			}
			g_string_append(calls, "]");
			g_strfreev(f);
		}
		g_string_append_c(calls, ']');
		char *src = g_strdup_printf("(%s)(%s);", A.scriptlets_lib, calls->str);
		s->script = webkit_user_script_new(src, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
						   WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
		webkit_user_content_manager_add_script(ucm, s->script);
		g_free(src);
		g_string_free(calls, TRUE);
	}
	if (A.verbose)
		logf_("%s: %zu bytes of CSS (%u own, %u unhide), %u scriptlet(s)%s%s%s%s", host, css->len, hi.hide->len,
		      g_hash_table_size(hi.unhide), hi.js->len, hi.nofilter ? ", nofilter" : "", hi.elemhide ? ", elemhide" : "",
		      hi.generichide ? ", generichide" : "", hi.specifichide ? ", specifichide" : "");
	g_string_free(css, TRUE);
	g_ptr_array_free(hi.hide, TRUE);
	g_hash_table_destroy(hi.unhide);
	g_ptr_array_free(hi.css, TRUE);
	g_ptr_array_free(hi.js, TRUE);
}

int adb_navigate(struct adb_tab *s, WebKitUserContentManager *ucm, const char *uri)
{
	if (!uri || !ucm)
		return 0;
	int web = g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://");
	if (!web) {	/* local and internal pages: no blocking, nothing injected */
		if (s->sheet || s->script || s->host[0]) {
			tab_clear_content(s, ucm);
			s->host[0] = 0;
		}
		return 0;
	}
	if (!A.enabled)
		return 0;
	if (!A.ready) {
		g_free(s->deferred);
		s->deferred = g_strdup(uri);
		return 1;
	}
	GUri *u = g_uri_parse(uri, G_URI_FLAGS_PARSE_RELAXED, NULL);
	const char *h = u ? g_uri_get_host(u) : NULL;
	if (!h) {
		if (u)
			g_uri_unref(u);
		return 0;
	}
	char *host = g_ascii_strdown(h, -1);
	if (u)
		g_uri_unref(u);
	int off = adb_site_off(host);
	if (off != s->off) {
		s->off = off;
		adb_tab_refilter(s, ucm);
	}
	if (!off && strcmp(host, s->host)) {
		tab_clear_content(s, ucm);
		make_page_content(s, ucm, host);
		g_strlcpy(s->host, host, sizeof s->host);
	} else if (off) {
		tab_clear_content(s, ucm);
		g_strlcpy(s->host, host, sizeof s->host);
	}
	g_free(host);
	return 0;
}
