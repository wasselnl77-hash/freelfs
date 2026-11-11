/* config -- FreeBWS settings (key=value file) and the directories it uses.
 * SPDX-License-Identifier: MIT */
#ifndef FB_CONFIG_H
#define FB_CONFIG_H

#include <glib.h>

/* ~/.config/freebws (config), ~/.local/share/freebws (bookmarks, ad block lists, downloads
 * list), ~/.cache/freebws (compiled filters). FREEBWS_HOME=DIR puts all three under DIR
 * (tests). Created on first use. */
const char *cfg_dir(void);
const char *data_dir(void);
const char *cache_dir(void);
/* The package's data files: /usr/share/freebws, or $FREEBWS_DATADIR. */
const char *share_dir(void);

void cfg_load(void);
const char *cfg_get(const char *key, const char *def);
int cfg_int(const char *key, int def);
void cfg_set(const char *key, const char *val);	/* writes the file at once */
void cfg_set_int(const char *key, int val);

/* "a.com,b.org" lists kept in one key */
gboolean cfg_list_has(const char *key, const char *item);
void cfg_list_set(const char *key, const char *item, gboolean on);

/* write a file atomically (temp file, rename); 0 or -1 */
int write_file_atomic(const char *path, const char *data, gsize len);

#endif
