/* downloads -- see downloads.h. SPDX-License-Identifier: MIT */
#include "downloads.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"

#define KEEP 60

static GPtrArray *list;		/* struct dl *, newest first */
static int next_id = 1;
static void (*changed_cb)(void);
static int finished_id = -1;
static WebKitNetworkSession *session;

static void dl_free(gpointer p)
{
	struct dl *d = p;
	if (d->wk) {
		g_signal_handlers_disconnect_by_data(d->wk, d);
		g_object_unref(d->wk);
	}
	g_free(d->url);
	g_free(d->name);
	g_free(d->path);
	g_free(d->error);
	g_free(d);
}

static void changed(void)
{
	if (changed_cb)
		changed_cb();
}

char *dl_dir(void)
{
	const char *c = cfg_get("downloads_dir", "");
	char *d;
	if (c && *c)
		d = g_strdup(c);
	else
		d = g_build_filename(g_get_home_dir(), "downloads", NULL);
	if (d[0] == '~' && (d[1] == '/' || !d[1])) {
		char *e = g_build_filename(g_get_home_dir(), d + (d[1] ? 2 : 1), NULL);
		g_free(d);
		d = e;
	}
	g_mkdir_with_parents(d, 0755);
	return d;
}

/* ---- the list on disk ---------------------------------------------------------------------- */

static void json_q(GString *o, const char *s)
{
	g_string_append_c(o, '"');
	for (; s && *s; s++) {
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

char *dl_json(void)
{
	GString *o = g_string_new("[");
	for (guint i = 0; list && i < list->len; i++) {
		struct dl *d = g_ptr_array_index(list, i);
		g_string_append_printf(o, "%s{\"id\":%d,\"state\":%d,\"total\":%llu,\"got\":%llu,\"started\":%lld,\"name\":", i ? "," : "",
				       d->id, d->state, (unsigned long long)d->total, (unsigned long long)d->got, (long long)d->started);
		json_q(o, d->name);
		g_string_append(o, ",\"url\":");
		json_q(o, d->url);
		g_string_append(o, ",\"path\":");
		json_q(o, d->path);
		g_string_append(o, ",\"error\":");
		json_q(o, d->error ? d->error : "");
		g_string_append_c(o, '}');
	}
	g_string_append_c(o, ']');
	return g_string_free(o, FALSE);
}

static void save(void)
{
	char *j = dl_json(), *p = g_build_filename(data_dir(), "downloads.json", NULL);
	GString *o = g_string_new(NULL);
	g_string_append_printf(o, "{\"next\":%d,\"items\":%s}\n", next_id, j);
	write_file_atomic(p, o->str, o->len);
	g_string_free(o, TRUE);
	g_free(j);
	g_free(p);
}

static char *rd(const char **pp)
{
	const char *p = *pp;
	if (*p != '"')
		return NULL;
	p++;
	GString *o = g_string_new(NULL);
	while (*p && *p != '"') {
		if (*p == '\\' && p[1]) {
			p++;
			if (*p == 'u' && p[1] && p[2] && p[3] && p[4]) {
				char hex[5] = { p[1], p[2], p[3], p[4], 0 };
				g_string_append_unichar(o, (gunichar)strtoul(hex, NULL, 16));
				p += 4;
			} else {
				g_string_append_c(o, *p);
			}
			p++;
		} else {
			g_string_append_c(o, *p++);
		}
	}
	if (*p == '"')
		p++;
	*pp = p;
	return g_string_free(o, FALSE);
}

static void load(void)
{
	char *p = g_build_filename(data_dir(), "downloads.json", NULL), *text = NULL;
	if (!g_file_get_contents(p, &text, NULL, NULL)) {
		g_free(p);
		return;
	}
	g_free(p);
	const char *s = strstr(text, "\"next\"");
	if (s)
		next_id = atoi(strchr(s, ':') + 1);
	s = strstr(text, "\"items\"");
	s = s ? strchr(s, '[') : NULL;
	/* items come newest first, keep that order */
	while (s && (s = strchr(s, '{'))) {
		struct dl *d = g_new0(struct dl, 1);
		const char *e = s + 1;
		while (*e && *e != '}') {
			while (*e == ' ' || *e == ',')
				e++;
			char *k = rd(&e);
			if (!k)
				break;
			if (*e == ':')
				e++;
			if (*e == '"') {
				char *v = rd(&e);
				if (!strcmp(k, "name"))
					d->name = v;
				else if (!strcmp(k, "url"))
					d->url = v;
				else if (!strcmp(k, "path"))
					d->path = v;
				else if (!strcmp(k, "error"))
					d->error = *v ? v : (g_free(v), NULL);
				else
					g_free(v);
			} else {
				long long n = atoll(e);
				if (!strcmp(k, "id"))
					d->id = (int)n;
				else if (!strcmp(k, "state"))
					d->state = (int)n;
				else if (!strcmp(k, "total"))
					d->total = (guint64)n;
				else if (!strcmp(k, "got"))
					d->got = (guint64)n;
				else if (!strcmp(k, "started"))
					d->started = n;
				while (*e && *e != ',' && *e != '}')
					e++;
			}
			g_free(k);
		}
		if (d->id > 0 && d->name && d->url) {
			if (d->state == DL_ACTIVE) {	/* the browser ended while it ran */
				d->state = DL_FAILED;
				d->error = g_strdup("interrupted");
			}
			if (d->id >= next_id)
				next_id = d->id + 1;
			g_ptr_array_add(list, d);
		} else {
			dl_free(d);
		}
		s = *e ? e + 1 : e;
	}
	g_free(text);
}

/* ---- WebKit ---------------------------------------------------------------------------------- */

static char *unique_path(const char *dir, const char *name)
{
	char *base = g_path_get_basename(name);
	if (!*base || !strcmp(base, ".") || !strcmp(base, ".."))
		base = g_strdup("download");
	for (char *p = base; *p; p++)
		if (*p == '/' || (unsigned char)*p < 0x20)
			*p = '_';
	char *path = g_build_filename(dir, base, NULL);
	if (access(path, F_OK)) {
		g_free(base);
		return path;
	}
	const char *dot = strrchr(base, '.');
	if (dot == base)
		dot = NULL;
	char *stem = dot ? g_strndup(base, (gsize)(dot - base)) : g_strdup(base);
	for (int i = 1; i < 1000; i++) {
		char *n = g_strdup_printf("%s (%d)%s", stem, i, dot ? dot : "");
		g_free(path);
		path = g_build_filename(dir, n, NULL);
		g_free(n);
		if (access(path, F_OK))
			break;
	}
	g_free(stem);
	g_free(base);
	return path;
}

static void trim_list(void)
{
	while (list->len > KEEP) {
		struct dl *d = g_ptr_array_index(list, list->len - 1);
		if (d->state == DL_ACTIVE)
			break;
		g_ptr_array_remove_index(list, list->len - 1);
	}
}

static gboolean on_dest(WebKitDownload *wk, const char *suggested, struct dl *d)
{
	char *dir = dl_dir();
	char *path = unique_path(dir, suggested && *suggested ? suggested : "download");
	g_free(d->path);
	d->path = path;
	g_free(d->name);
	d->name = g_path_get_basename(path);
	webkit_download_set_destination(wk, path);
	fprintf(stderr, "freebws: download %d: %s -> %s\n", d->id, d->url ? d->url : "?", path);
	g_free(dir);
	WebKitURIResponse *r = webkit_download_get_response(wk);
	if (r)
		d->total = (guint64)webkit_uri_response_get_content_length(r);
	save();
	changed();
	return TRUE;
}

static void on_received(WebKitDownload *wk, guint64 n, struct dl *d)
{
	d->got = webkit_download_get_received_data_length(wk);
	if (!d->total) {
		WebKitURIResponse *r = webkit_download_get_response(wk);
		if (r)
			d->total = (guint64)webkit_uri_response_get_content_length(r);
	}
	changed();
}

static void on_finished(WebKitDownload *wk, struct dl *d)
{
	if (d->state == DL_ACTIVE) {
		d->state = DL_DONE;
		d->got = webkit_download_get_received_data_length(wk);
		d->finished = g_get_real_time() / 1000000;
		finished_id = d->id;
	}
	g_signal_handlers_disconnect_by_data(wk, d);
	g_object_unref(d->wk);
	d->wk = NULL;
	save();
	changed();
}

static void on_failed(WebKitDownload *wk, GError *e, struct dl *d)
{
	fprintf(stderr, "freebws: download %d failed: %s\n", d->id, e && e->message ? e->message : "?");
	d->state = e && e->code == WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER ? DL_CANCELLED : DL_FAILED;
	g_free(d->error);
	d->error = e && d->state == DL_FAILED ? g_strdup(e->message) : NULL;
	d->finished = g_get_real_time() / 1000000;
	if (d->path)
		unlink(d->path);	/* a half file is no use */
	/* on_finished may follow; it keeps the state */
}

static void on_started(WebKitNetworkSession *s, WebKitDownload *wk, gpointer data)
{
	struct dl *d = g_new0(struct dl, 1);
	d->id = next_id++;
	d->wk = g_object_ref(wk);
	WebKitURIRequest *rq = webkit_download_get_request(wk);
	d->url = g_strdup(rq ? webkit_uri_request_get_uri(rq) : "");
	d->name = g_strdup("download");
	d->started = g_get_real_time() / 1000000;
	d->state = DL_ACTIVE;
	fprintf(stderr, "freebws: download %d started: %s\n", d->id, d->url);
	g_signal_connect(wk, "decide-destination", G_CALLBACK(on_dest), d);
	g_signal_connect(wk, "received-data", G_CALLBACK(on_received), d);
	g_signal_connect(wk, "finished", G_CALLBACK(on_finished), d);
	g_signal_connect(wk, "failed", G_CALLBACK(on_failed), d);
	g_ptr_array_insert(list, 0, d);
	trim_list();
	changed();
}

void dl_init(WebKitNetworkSession *s, void (*cb)(void))
{
	changed_cb = cb;
	session = s;
	list = g_ptr_array_new_with_free_func(dl_free);
	load();
	g_signal_connect(s, "download-started", G_CALLBACK(on_started), NULL);
}

guint dl_count(void) { return list ? list->len : 0; }
struct dl *dl_at(guint i) { return list && i < list->len ? g_ptr_array_index(list, i) : NULL; }

struct dl *dl_get(int id)
{
	for (guint i = 0; list && i < list->len; i++)
		if (((struct dl *)g_ptr_array_index(list, i))->id == id)
			return g_ptr_array_index(list, i);
	return NULL;
}

int dl_active_count(void)
{
	int n = 0;
	for (guint i = 0; list && i < list->len; i++)
		n += ((struct dl *)g_ptr_array_index(list, i))->state == DL_ACTIVE;
	return n;
}

double dl_progress(void)
{
	guint64 tot = 0, got = 0;
	int n = 0, unknown = 0;
	for (guint i = 0; list && i < list->len; i++) {
		struct dl *d = g_ptr_array_index(list, i);
		if (d->state != DL_ACTIVE)
			continue;
		n++;
		if (d->total) {
			tot += d->total;
			got += d->got < d->total ? d->got : d->total;
		} else {
			unknown++;
		}
	}
	if (!n)
		return -1;
	if (!tot)
		return 0;	/* sizes not known yet: an empty ring, still turning */
	return (double)got / (double)tot + (unknown ? 0 : 0);
}

void dl_cancel(int id)
{
	struct dl *d = dl_get(id);
	if (d && d->wk && d->state == DL_ACTIVE)
		webkit_download_cancel(d->wk);
}

void dl_start(const char *url)
{
	if (session && url && *url)
		webkit_network_session_download_uri(session, url);
}

void dl_retry(int id)
{
	struct dl *d = dl_get(id);
	if (d && d->state != DL_ACTIVE && d->url)
		dl_start(d->url);
}

void dl_remove(int id)
{
	struct dl *d = dl_get(id);
	if (!d)
		return;
	if (d->state == DL_ACTIVE)
		dl_cancel(id);
	g_ptr_array_remove(list, d);
	save();
	changed();
}

void dl_clear_finished(void)
{
	for (guint i = 0; i < list->len;) {
		struct dl *d = g_ptr_array_index(list, i);
		if (d->state != DL_ACTIVE)
			g_ptr_array_remove_index(list, i);
		else
			i++;
	}
	save();
	changed();
}

static void run(const char *a, const char *b, const char *c)
{
	char *argv[] = { (char *)a, (char *)b, (char *)c, NULL };
	if (!c)
		argv[2] = NULL;
	g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
		      NULL, NULL);
}

void dl_show_folder(int id)
{
	struct dl *d = dl_get(id);
	char *dir = d && d->path ? g_path_get_dirname(d->path) : dl_dir();
	run("flfiles", dir, NULL);
	g_free(dir);
}

void dl_open(int id)
{
	struct dl *d = dl_get(id);
	if (!d || d->state != DL_DONE || !d->path || access(d->path, R_OK)) {
		dl_show_folder(id);
		return;
	}
	char *ct = g_content_type_guess(d->path, NULL, 0, NULL);
	if (ct && g_str_has_prefix(ct, "text/"))
		run("fledit", d->path, NULL);
	else if (g_find_program_in_path("gio"))
		run("gio", "open", d->path);
	else
		dl_show_folder(id);
	g_free(ct);
}

int dl_take_finished(void)
{
	int r = finished_id;
	finished_id = -1;
	return r;
}
