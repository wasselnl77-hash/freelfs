/* bookmarks -- FreeBWS bookmarks: a tree of folders and links, kept as JSON in the data
 * directory (bookmarks.json), imported from Netscape HTML bookmark files (what Firefox and
 * Chromium export). Items directly under the root (parent 0) are the ones on the bar.
 * SPDX-License-Identifier: MIT */
#ifndef FB_BOOKMARKS_H
#define FB_BOOKMARKS_H

#include <glib.h>

struct bm {
	int id;
	int parent;		/* 0: the root (the bookmarks bar) */
	int folder;
	char *title;
	char *url;		/* NULL for a folder */
};

void bm_load(void);
guint bm_count(void);
const struct bm *bm_at(guint i);			/* in file order */
const struct bm *bm_get(int id);
const struct bm *bm_find_url(const char *url);		/* a bookmark of exactly this address */
int bm_add(const char *title, const char *url, int parent);	/* the new id */
int bm_add_folder(const char *title, int parent);
void bm_remove(int id);					/* a folder with everything in it */
void bm_update(int id, const char *title, const char *url);	/* url NULL: keep */
void bm_move(int id, int parent);			/* to the end of that folder; no loops */
/* children of a folder in order; free the array only (not the items) */
GPtrArray *bm_children(int parent);
/* Netscape bookmark file text: number of links added (-1: not such a file) */
int bm_import_html(const char *html, gsize len);
/* the whole tree as JSON for the manager page: [{id,parent,folder,title,url}, ...] */
char *bm_json(void);
/* bumped on every change, so the bar can notice */
extern unsigned bm_serial;

#endif
