/* downloads -- WebKitDownload handling for FreeBWS: files go to ~/downloads (or the folder in
 * the settings), a list the toolbar panel and freebws://downloads show, cancel, retry (the WPE
 * API cannot resume a download), open and show in folder. The list (names, addresses, state)
 * is kept in downloads.json in the data directory.
 * SPDX-License-Identifier: MIT */
#ifndef FB_DOWNLOADS_H
#define FB_DOWNLOADS_H

#include <wpe/webkit.h>

enum { DL_ACTIVE, DL_DONE, DL_FAILED, DL_CANCELLED };

struct dl {
	int id;
	WebKitDownload *wk;	/* while it runs */
	char *url, *name, *path;
	guint64 total, got;	/* total 0: unknown */
	int state;
	char *error;
	gint64 started, finished;	/* g_get_real_time() / 1e6 */
};

/* changed() runs whenever the list or a progress changes (the toolbar redraws) */
void dl_init(WebKitNetworkSession *session, void (*changed)(void));
guint dl_count(void);
struct dl *dl_at(guint i);			/* newest first */
struct dl *dl_get(int id);
int dl_active_count(void);
double dl_progress(void);			/* 0..1 of all running downloads, -1 when none run */
void dl_cancel(int id);
void dl_retry(int id);				/* a new download of the same address */
void dl_remove(int id);				/* off the list (the file stays) */
void dl_clear_finished(void);
void dl_open(int id);
void dl_show_folder(int id);
void dl_start(const char *url);			/* download an address */
char *dl_dir(void);				/* the destination folder; g_free */
char *dl_json(void);
/* a download finished since the last call (for a notice), or -1 */
int dl_take_finished(void);

#endif
