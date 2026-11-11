/* abp -- Adblock Plus / uBlock Origin filter lists to WebKit content blocker JSON.
 *
 * Own converter (no code taken from other converters). It reads filter lists in
 * ABP syntax and produces
 *   - network rules as content blocker JSON, split into chunks of at most N rules
 *     (WebKit compiles each chunk as one content rule list)
 *   - generic cosmetic selectors (hidden on every site), one per line
 *   - a sorted per-site table: cosmetic selectors, unhide exceptions, style
 *     rules, scriptlet calls, and the per-site switches of the lists
 *     (@@...$elemhide, $generichide, $document)
 * What WebKit's matcher cannot express is skipped and counted in abp_stats.
 * SPDX-License-Identifier: MIT */
#ifndef FB_ABP_H
#define FB_ABP_H

#include <glib.h>

#define ABP_CONVERTER_VERSION 2

typedef struct abp abp;

struct abp_stats {
	unsigned lines;
	unsigned net_block, net_except, net_important;	/* rules emitted (deduplicated) */
	unsigned net_badfilter;
	unsigned net_skipped;				/* options or syntax WebKit cannot express */
	unsigned cos_generic, cos_site, cos_exception, cos_procedural;
	unsigned scriptlets, scriptlets_generic;
	unsigned allow_hosts;				/* @@...$document / $elemhide / $generichide hosts */
};

abp *abp_new(void);
void abp_free(abp *a);
/* One list. text need not be NUL terminated. */
void abp_add_list(abp *a, const char *text, gsize len);
/* After the last list: apply $badfilter, order the rules. */
void abp_finish(abp *a);
const struct abp_stats *abp_stats(const abp *a);

/* Network rules as JSON: how many chunks of at most max_rules (blocks, then every
 * exception, then the $important blocks of that chunk), and chunk i. Free with g_free. */
int abp_chunks(const abp *a, int max_rules);
char *abp_chunk_json(const abp *a, int max_rules, int i, unsigned *nrules);
/* Generic hide selectors, one per line. */
char *abp_generic(const abp *a);
/* The sorted per-site table (see abp.c). */
char *abp_site_table(const abp *a);

#endif
