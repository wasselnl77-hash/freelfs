/* adblock -- FreeBWS's content blocker: WebKit content filters compiled from filter
 * lists (abp.c), cosmetic filtering through injected style sheets, scriptlets as user
 * scripts, a per-site switch. Lists are data files (/usr/share/freebws/lists, newer copies
 * in ~/.local/share/freebws/adblock/lists); a child process of the browser
 * (freebws --adblock-update) converts and compiles them so the browser itself never
 * holds the compiler's memory. See docs/M8-BROWSER.md, "Ad blocker".
 * SPDX-License-Identifier: MIT */
#ifndef FB_ADBLOCK_H
#define FB_ADBLOCK_H

#include <wpe/webkit.h>

struct adb_tab {
	WebKitUserStyleSheet *sheet;	/* cosmetic rules of the page the tab shows */
	WebKitUserScript *script;	/* scriptlets of that page */
	WebKitUserScript *yt;		/* the static YouTube script (all frames) */
	char host[256];			/* the host the above was made for */
	int off;			/* the blocker is off for this tab's site */
	int filters_on;			/* network filters are in the tab's content manager */
	unsigned blocked;		/* requests blocked on this tab's page */
	char *deferred;			/* a navigation held back until the filters are ready */
};

/* the browser process */
void adb_init(void);
void adb_shutdown(void);
int adb_enabled(void);
void adb_set_enabled(int on);
int adb_ready(void);			/* the network filters are loaded (or the blocker is off) */
int adb_building(void);			/* the first filters are being made: pages wait for a while */
int adb_site_off(const char *host);
void adb_set_site_off(const char *host, int off);
void adb_update_now(void);		/* download newer lists and rebuild, in the background */
/* one line for the settings page */
char *adb_status_json(void);

void adb_tab_init(struct adb_tab *s, WebKitUserContentManager *ucm);
void adb_tab_free(struct adb_tab *s, WebKitUserContentManager *ucm);
/* put the filters in (or take them out of) the tab's content manager as the settings say */
void adb_tab_refilter(struct adb_tab *s, WebKitUserContentManager *ucm);
/* a main-frame navigation to uri is about to start. Returns 1 when it must wait (filters not
 * ready yet: s->deferred holds the address, adb_ready_cb loads it later). */
int adb_navigate(struct adb_tab *s, WebKitUserContentManager *ucm, const char *uri);

/* provided by freebws.c */
void fb_adb_changed(void);		/* filters were (re)loaded or the settings changed: refilter every tab */

/* child process entry: "update" (download, then build) or "build"; returns the exit status */
int adb_child_main(const char *mode);

#endif
