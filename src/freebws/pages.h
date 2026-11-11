/* pages -- the pages FreeBWS serves itself under freebws:// (start, settings, bookmarks,
 * downloads), the bridge those pages use to talk to the browser, the search engines, and the
 * error pages (offline, no such server, TLS, blocked, crashed tab) in the crash screen's style.
 * SPDX-License-Identifier: MIT */
#ifndef FB_PAGES_H
#define FB_PAGES_H

#include <wpe/webkit.h>

struct engine {
	const char *id, *name;
	const char *url;	/* the search address, %s is the escaped query */
	const char *action, *param;	/* the start page's form: GET action?param=query */
};

extern const struct engine engines[];
extern const int n_engines;
const struct engine *search_current(void);
/* the address a search for text goes to (g_free) */
char *search_url(const char *text);

/* ---- the scheme and the bridge -------------------------------------------------------------- */
void pages_init(WebKitWebContext *ctx);
/* one line for the Settings page: is the web process sandbox on (set by freebws.c) */
extern const char *pages_sandbox_status;
/* per tab: the message handler of the pages (world "freebws-ui") and the bridge script */
void pages_setup_view(WebKitUserContentManager *ucm, void *tab);
void pages_teardown_view(WebKitUserContentManager *ucm, void *tab);

/* ---- error pages ------------------------------------------------------------------------------ */
enum { ERR_OTHER, ERR_OFFLINE, ERR_DNS, ERR_REFUSED, ERR_TLS, ERR_BLOCKED, ERR_CRASH, ERR_TIMEOUT };
/* what kind of failure a load error is (message and the network state decide) */
int pages_classify(const GError *err);
/* show the error page in view; the address bar keeps showing uri. detail: technical line */
void pages_show_error(WebKitWebView *view, int kind, const char *uri, const char *detail, int can_proceed);

/* ---- provided by freebws.c ---------------------------------------------------------------------- */
void fb_open_url(void *tab, const char *url, int new_tab);
void fb_tab_retry(void *tab);
void fb_tab_tls_proceed(void *tab);	/* continue to the page whose certificate failed */
void fb_tab_blocked_proceed(void *tab);	/* open the page the blocker stopped, with the blocker off for it */
void fb_setting_changed(const char *key);

#endif
