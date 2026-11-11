/* pages -- see pages.h. SPDX-License-Identifier: MIT
 *
 * freebws://start, settings, bookmarks, downloads and freebws://res/<file> are files from
 * share_dir() (start.html is filled in with the search engine). The pages talk to the browser
 * through a bridge: a user script in the script world "freebws-ui" (the pages' own scripts run
 * in the main world and cannot reach the message handler) listens for DOM events and posts the
 * message to the handler "fbui"; the reply comes back as an event. The script only does
 * anything on freebws:// pages, and only in the top frame.
 */
#include "pages.h"

#include <string.h>

#include "adblock.h"
#include "bookmarks.h"
#include "config.h"
#include "downloads.h"

#define UI_WORLD "freebws-ui"

const struct engine engines[] = {
	{ "brave", "Brave Search", "https://search.brave.com/search?q=%s", "https://search.brave.com/search", "q" },
	{ "duckduckgo", "DuckDuckGo", "https://duckduckgo.com/?q=%s", "https://duckduckgo.com/", "q" },
	{ "google", "Google", "https://www.google.com/search?q=%s", "https://www.google.com/search", "q" },
	{ "startpage", "Startpage", "https://www.startpage.com/sp/search?query=%s", "https://www.startpage.com/sp/search", "query" },
	{ "bing", "Bing", "https://www.bing.com/search?q=%s", "https://www.bing.com/search", "q" },
	{ "ecosia", "Ecosia", "https://www.ecosia.org/search?q=%s", "https://www.ecosia.org/search", "q" },
};
const int n_engines = G_N_ELEMENTS(engines);

const struct engine *search_current(void)
{
	const char *id = cfg_get("search", "brave");
	for (int i = 0; i < n_engines; i++)
		if (!strcmp(engines[i].id, id))
			return &engines[i];
	return &engines[0];
}

char *search_url(const char *text)
{
	char *q = g_uri_escape_string(text, NULL, FALSE);
	const char *fmt = search_current()->url;
	const char *pct = strstr(fmt, "%s");
	char *pre = g_strndup(fmt, (gsize)(pct - fmt));
	char *r = g_strconcat(pre, q, pct + 2, NULL);
	g_free(pre);
	g_free(q);
	return r;
}

/* ---- the files -------------------------------------------------------------------------------- */

static char *html_esc(const char *s)
{
	GString *o = g_string_new(NULL);
	for (; s && *s; s++) {
		if (*s == '&')
			g_string_append(o, "&amp;");
		else if (*s == '<')
			g_string_append(o, "&lt;");
		else if (*s == '>')
			g_string_append(o, "&gt;");
		else if (*s == '"')
			g_string_append(o, "&quot;");
		else
			g_string_append_c(o, *s);
	}
	return g_string_free(o, FALSE);
}

static char *replace_all(char *s, const char *from, const char *to)
{
	char **v = g_strsplit(s, from, 0);
	char *r = g_strjoinv(to, v);
	g_strfreev(v);
	g_free(s);
	return r;
}

static void respond(WebKitURISchemeRequest *req, const char *mime, char *data, gsize len)
{
	GInputStream *in = g_memory_input_stream_new_from_data(data, (gssize)len, g_free);
	webkit_uri_scheme_request_finish(req, in, (gint64)len, mime);
	g_object_unref(in);
}

static void scheme_cb(WebKitURISchemeRequest *req, gpointer ud)
{
	GUri *u = g_uri_parse(webkit_uri_scheme_request_get_uri(req), G_URI_FLAGS_PARSE_RELAXED, NULL);
	const char *host = u ? g_uri_get_host(u) : NULL;
	const char *path = u ? g_uri_get_path(u) : "";
	char *file = NULL;
	const char *mime = "text/html; charset=utf-8";
	if (host && !strcmp(host, "res")) {
		/* only plain file names of the package's data */
		const char *n = path[0] == '/' ? path + 1 : path;
		int ok = *n && !strchr(n, '/') && strcmp(n, "..") && !strstr(n, "..");
		for (const char *c = n; ok && *c; c++)
			if (!g_ascii_isalnum(*c) && !strchr("._-", *c))
				ok = 0;
		if (ok && (g_str_has_suffix(n, ".css") || g_str_has_suffix(n, ".js") || g_str_has_suffix(n, ".png") || g_str_has_suffix(n, ".svg"))) {
			file = g_build_filename(share_dir(), n, NULL);
			mime = g_str_has_suffix(n, ".css") ? "text/css; charset=utf-8" :
			       g_str_has_suffix(n, ".js") ? "text/javascript; charset=utf-8" :
			       g_str_has_suffix(n, ".png") ? "image/png" : "image/svg+xml";
		}
	} else if (host && (!strcmp(host, "start") || !strcmp(host, "settings") || !strcmp(host, "bookmarks") || !strcmp(host, "downloads"))) {
		char *n = g_strconcat(host, ".html", NULL);
		file = g_build_filename(share_dir(), n, NULL);
		g_free(n);
	}
	gchar *data = NULL;
	gsize len = 0;
	if (!file || !g_file_get_contents(file, &data, &len, NULL)) {
		GError *e = g_error_new(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "no such FreeBWS page");
		webkit_uri_scheme_request_finish_error(req, e);
		g_error_free(e);
	} else {
		if (host && !strcmp(host, "start")) {
			const struct engine *en = search_current();
			char *act = html_esc(en->action), *par = html_esc(en->param), *nm = html_esc(en->name);
			data = replace_all(data, "@ACTION@", act);
			data = replace_all(data, "@PARAM@", par);
			data = replace_all(data, "@NAME@", nm);
			len = strlen(data);
			g_free(act);
			g_free(par);
			g_free(nm);
		}
		respond(req, mime, data, len);
	}
	g_free(file);
	if (u)
		g_uri_unref(u);
}

void pages_init(WebKitWebContext *ctx)
{
	webkit_web_context_register_uri_scheme(ctx, "freebws", scheme_cb, NULL, NULL);
	WebKitSecurityManager *sm = webkit_web_context_get_security_manager(ctx);
	webkit_security_manager_register_uri_scheme_as_secure(sm, "freebws");
	webkit_security_manager_register_uri_scheme_as_display_isolated(sm, "freebws");
}

/* ---- the bridge ----------------------------------------------------------------------------------- */

#define BRIDGE_JS \
	"(function(){if(location.protocol!=='freebws:')return;" \
	"var h=window.webkit&&window.webkit.messageHandlers&&window.webkit.messageHandlers.fbui;if(!h)return;" \
	"document.addEventListener('fb-call',function(e){var d=JSON.parse(e.detail);" \
	"h.postMessage(d.msg).then(function(r){document.dispatchEvent(new CustomEvent('fb-reply',{detail:JSON.stringify({id:d.id,reply:r})}));}," \
	"function(){document.dispatchEvent(new CustomEvent('fb-reply',{detail:JSON.stringify({id:d.id,reply:'null'})}));});},false);})();"

static char *jstr(JSCValue *o, const char *prop)
{
	JSCValue *v = jsc_value_object_get_property(o, prop);
	char *s = v && jsc_value_is_string(v) ? jsc_value_to_string(v) : NULL;
	if (v)
		g_object_unref(v);
	return s;
}

static int jint(JSCValue *o, const char *prop, int def)
{
	JSCValue *v = jsc_value_object_get_property(o, prop);
	int r = def;
	if (v && jsc_value_is_number(v))
		r = jsc_value_to_int32(v);
	else if (v && jsc_value_is_string(v)) {
		char *s = jsc_value_to_string(v);
		r = atoi(s);
		g_free(s);
	}
	if (v)
		g_object_unref(v);
	return r;
}

static void jq(GString *o, const char *s)
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

static char *state_json(void)
{
	GString *o = g_string_new("{\"search\":");
	jq(o, search_current()->id);
	g_string_append(o, ",\"engines\":[");
	for (int i = 0; i < n_engines; i++) {
		g_string_append_printf(o, "%s{\"id\":", i ? "," : "");
		jq(o, engines[i].id);
		g_string_append(o, ",\"name\":");
		jq(o, engines[i].name);
		g_string_append_c(o, '}');
	}
	char *adb = adb_status_json(), *dir = dl_dir();
	g_string_append_printf(o, "],\"adblock\":%s,\"dark\":%d,\"dark_on\":", adb, cfg_int("dark", 0));
	jq(o, cfg_get("dark_on", ""));
	g_string_append(o, ",\"dark_off\":");
	jq(o, cfg_get("dark_off", ""));
	g_string_append(o, ",\"downloads_dir\":");
	jq(o, dir);
	g_string_append_printf(o, ",\"bookmarks_bar\":%d,\"home\":", cfg_int("bookmarks_bar", 1));
	jq(o, g_get_home_dir());
	g_string_append_c(o, '}');
	g_free(adb);
	g_free(dir);
	return g_string_free(o, FALSE);
}

static gboolean on_message(WebKitUserContentManager *ucm, JSCValue *msg, WebKitScriptMessageReply *reply, gpointer tab)
{
	char *out = NULL;
	char *cmd = jsc_value_is_object(msg) ? jstr(msg, "cmd") : NULL;
	if (!cmd) {
		out = g_strdup("null");
		goto reply;
	}
	if (!strcmp(cmd, "state")) {
		out = state_json();
	} else if (!strcmp(cmd, "set")) {
		char *k = jstr(msg, "key"), *v = jstr(msg, "value");
		if (k && v) {
			if (!strcmp(k, "adblock")) {
				adb_set_enabled(atoi(v));
			} else if (!strcmp(k, "search")) {
				for (int i = 0; i < n_engines; i++)
					if (!strcmp(engines[i].id, v))
						cfg_set("search", v);
			} else if (!strcmp(k, "dark") || !strcmp(k, "bookmarks_bar")) {
				cfg_set(k, atoi(v) ? "1" : "0");
			} else if (!strcmp(k, "downloads_dir")) {
				cfg_set(k, v);
			}
			fb_setting_changed(k);
		}
		g_free(k);
		g_free(v);
		out = state_json();
	} else if (!strcmp(cmd, "adb")) {
		char *op = jstr(msg, "op"), *host = jstr(msg, "host");
		if (op && !strcmp(op, "update"))
			adb_update_now();
		else if (op && !strcmp(op, "site") && host)
			adb_set_site_off(host, jint(msg, "off", 0));
		g_free(op);
		g_free(host);
		out = state_json();
	} else if (!strcmp(cmd, "dark-site")) {
		char *host = jstr(msg, "host");
		if (host) {
			cfg_list_set("dark_on", host, FALSE);
			cfg_list_set("dark_off", host, FALSE);
			int mode = jint(msg, "mode", 0);	/* 0: follow the global setting, 1: on, 2: off */
			if (mode == 1)
				cfg_list_set("dark_on", host, TRUE);
			else if (mode == 2)
				cfg_list_set("dark_off", host, TRUE);
			fb_setting_changed("dark");
		}
		g_free(host);
		out = state_json();
	} else if (!strcmp(cmd, "bm.list")) {
		out = bm_json();
	} else if (!strcmp(cmd, "bm.add")) {
		char *t = jstr(msg, "title"), *u = jstr(msg, "url");
		int id = u && *u ? bm_add(t, u, jint(msg, "parent", 0)) : 0;
		g_free(t);
		g_free(u);
		out = g_strdup_printf("{\"id\":%d}", id);
		fb_setting_changed("bookmarks");
	} else if (!strcmp(cmd, "bm.folder")) {
		char *t = jstr(msg, "title");
		int id = bm_add_folder(t && *t ? t : "New folder", jint(msg, "parent", 0));
		g_free(t);
		out = g_strdup_printf("{\"id\":%d}", id);
		fb_setting_changed("bookmarks");
	} else if (!strcmp(cmd, "bm.edit")) {
		char *t = jstr(msg, "title"), *u = jstr(msg, "url");
		bm_update(jint(msg, "id", 0), t, u);
		g_free(t);
		g_free(u);
		out = g_strdup("{}");
		fb_setting_changed("bookmarks");
	} else if (!strcmp(cmd, "bm.del")) {
		bm_remove(jint(msg, "id", 0));
		out = g_strdup("{}");
		fb_setting_changed("bookmarks");
	} else if (!strcmp(cmd, "bm.move")) {
		bm_move(jint(msg, "id", 0), jint(msg, "parent", 0));
		out = g_strdup("{}");
		fb_setting_changed("bookmarks");
	} else if (!strcmp(cmd, "bm.import")) {
		char *path = jstr(msg, "path"), *text = NULL;
		gsize n = 0;
		int cnt = -2;
		if (path && *path) {
			char *p = path;
			char *full = p[0] == '~' ? g_build_filename(g_get_home_dir(), p[1] == '/' ? p + 2 : p + 1, NULL) : g_strdup(p);
			if (g_file_get_contents(full, &text, &n, NULL))
				cnt = bm_import_html(text, n);
			g_free(full);
		}
		g_free(text);
		g_free(path);
		out = g_strdup_printf("{\"count\":%d}", cnt);	/* -2: cannot read it, -1: not a bookmark file */
		fb_setting_changed("bookmarks");
	} else if (!strcmp(cmd, "dl.list")) {
		out = dl_json();
	} else if (!strcmp(cmd, "dl.cancel")) {
		dl_cancel(jint(msg, "id", 0));
		out = g_strdup("{}");
	} else if (!strcmp(cmd, "dl.retry")) {
		dl_retry(jint(msg, "id", 0));
		out = g_strdup("{}");
	} else if (!strcmp(cmd, "dl.remove")) {
		dl_remove(jint(msg, "id", 0));
		out = g_strdup("{}");
	} else if (!strcmp(cmd, "dl.open")) {
		dl_open(jint(msg, "id", 0));
		out = g_strdup("{}");
	} else if (!strcmp(cmd, "dl.show")) {
		dl_show_folder(jint(msg, "id", 0));
		out = g_strdup("{}");
	} else if (!strcmp(cmd, "dl.clear")) {
		dl_clear_finished();
		out = g_strdup("{}");
	} else if (!strcmp(cmd, "retry")) {
		fb_tab_retry(tab);
		out = g_strdup("{}");
	} else if (!strcmp(cmd, "tls-proceed")) {
		fb_tab_tls_proceed(tab);
		out = g_strdup("{}");
	} else if (!strcmp(cmd, "blocked-proceed")) {
		fb_tab_blocked_proceed(tab);
		out = g_strdup("{}");
	} else if (!strcmp(cmd, "open")) {
		char *u = jstr(msg, "url");
		if (u)
			fb_open_url(tab, u, jint(msg, "newtab", 0));
		g_free(u);
		out = g_strdup("{}");
	}
	if (!out)
		out = g_strdup("null");
reply:;
	JSCContext *jc = jsc_value_get_context(msg);
	JSCValue *rv = jsc_value_new_string(jc, out);
	webkit_script_message_reply_return_value(reply, rv);
	g_object_unref(rv);
	g_free(out);
	g_free(cmd);
	return TRUE;
}

void pages_setup_view(WebKitUserContentManager *ucm, void *tab)
{
	WebKitUserScript *us = webkit_user_script_new_for_world(BRIDGE_JS, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
								WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, UI_WORLD, NULL, NULL);
	webkit_user_content_manager_add_script(ucm, us);
	webkit_user_script_unref(us);
	webkit_user_content_manager_register_script_message_handler_with_reply(ucm, "fbui", UI_WORLD);
	g_signal_connect(ucm, "script-message-with-reply-received::fbui", G_CALLBACK(on_message), tab);
}

void pages_teardown_view(WebKitUserContentManager *ucm, void *tab)
{
	g_signal_handlers_disconnect_by_data(ucm, tab);
}

/* ---- error pages ----------------------------------------------------------------------------------- */

int pages_classify(const GError *err)
{
	const char *m = err && err->message ? err->message : "";
	if (strstr(m, "content blocker"))
		return ERR_BLOCKED;
	if (strstr(m, "resolve") || strstr(m, "Name or service") || strstr(m, "name resolution") || strstr(m, "No address") ||
	    strstr(m, "hostname"))
		return g_network_monitor_get_network_available(g_network_monitor_get_default()) ? ERR_DNS : ERR_OFFLINE;
	if (strstr(m, "unreachable") || strstr(m, "No route") || strstr(m, "Network is down"))
		return ERR_OFFLINE;
	if (strstr(m, "refused"))
		return ERR_REFUSED;
	if (strstr(m, "timed out") || strstr(m, "Timeout") || strstr(m, "timeout"))
		return ERR_TIMEOUT;
	if (strstr(m, "TLS") || strstr(m, "certificate") || strstr(m, "SSL"))
		return ERR_TLS;
	if (!g_network_monitor_get_network_available(g_network_monitor_get_default()))
		return ERR_OFFLINE;
	return ERR_OTHER;
}

void pages_show_error(WebKitWebView *view, int kind, const char *uri, const char *detail, int can_proceed)
{
	static const struct { const char *face, *title, *text, *code; } k[] = {
		[ERR_OTHER] = { ":/", "This page did not load", "Something went wrong while loading the page. Trying again often helps.", "FB-LOAD" },
		[ERR_OFFLINE] = { ":/", "You are offline", "The computer has no network connection right now. Check the network icon in the panel or the cable, then try again.", "FB-OFFLINE" },
		[ERR_DNS] = { ":/", "Can't find that server", "The address could not be looked up. Check it for typing mistakes, or try again later: the site may be down.", "FB-DNS" },
		[ERR_REFUSED] = { ":/", "The server did not answer", "The computer was reached, but nothing there accepts the connection. The site may be down.", "FB-REFUSED" },
		[ERR_TLS] = { ":o", "The connection is not safe", "The site's security certificate could not be trusted, so nothing was sent. Someone may be listening in, or the site's certificate is wrong or out of date.", "FB-TLS" },
		[ERR_BLOCKED] = { ":|", "Blocked by the ad blocker", "This address is on a list of ad, tracking or harmful sites, so the page was not opened.", "FB-BLOCKED" },
		[ERR_CRASH] = { ":(", "This tab crashed", "The page's process stopped (a bug or not enough memory). Your other tabs are fine. Reloading usually works.", "FB-CRASH" },
		[ERR_TIMEOUT] = { ":/", "The server took too long", "The connection was made but the site did not answer in time. Try again in a moment.", "FB-TIMEOUT" },
	};
	if (kind < 0 || kind > ERR_TIMEOUT)
		kind = ERR_OTHER;
	char *tpl = NULL, *p = g_build_filename(share_dir(), "error.html", NULL);
	if (!g_file_get_contents(p, &tpl, NULL, NULL))
		tpl = g_strdup("<!doctype html><title>Error</title><h1>@TITLE@</h1><p>@TEXT@</p>");
	g_free(p);
	char *face = html_esc(k[kind].face), *title = html_esc(k[kind].title), *text = html_esc(k[kind].text),
	     *code = html_esc(k[kind].code), *u = html_esc(uri ? uri : ""), *det = html_esc(detail ? detail : "");
	GString *btn = g_string_new(NULL);
	const char *retry = kind == ERR_BLOCKED ? "" : "<a class=\"key main\" id=\"retry\" href=\"#\" data-act=\"retry\"><kbd>Enter</kbd><span>Try again</span></a>";
	g_string_append(btn, retry);
	if (kind == ERR_TLS && can_proceed)
		g_string_append(btn, "<a class=\"key\" href=\"#\" data-act=\"tls-proceed\"><kbd>C</kbd><span>Continue anyway (not safe)</span></a>");
	if (kind == ERR_BLOCKED)
		g_string_append(btn, "<a class=\"key main\" href=\"#\" data-act=\"blocked-proceed\"><kbd>O</kbd><span>Open it anyway, with the blocker off for this site</span></a>");
	g_string_append(btn, "<a class=\"key\" href=\"#\" data-act=\"home\"><kbd>Esc</kbd><span>Go to the start page</span></a>");
	char *html = tpl;
	html = replace_all(html, "@FACE@", face);
	html = replace_all(html, "@TITLE@", title);
	html = replace_all(html, "@TEXT@", text);
	html = replace_all(html, "@CODE@", code);
	html = replace_all(html, "@URL@", u);
	html = replace_all(html, "@DETAIL@", det);
	html = replace_all(html, "@BUTTONS@", btn->str);
	webkit_web_view_load_alternate_html(view, html, uri && *uri ? uri : "about:blank", "freebws://error/");
	g_free(html);
	g_string_free(btn, TRUE);
	g_free(face);
	g_free(title);
	g_free(text);
	g_free(code);
	g_free(u);
	g_free(det);
}
