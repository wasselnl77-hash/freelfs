/* abpconv -- convert ABP filter lists (see abp.h) for FreeBWS.
 *   abpconv [-n MAXRULES] OUTDIR LIST...
 * writes OUTDIR/net-N.json, generic.txt, site.tsv and prints the counts.
 * SPDX-License-Identifier: MIT */
#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "abp.h"

int main(int argc, char **argv)
{
	int max = 45000, i = 1;
	if (argc > 2 && !strcmp(argv[1], "-n")) {
		max = atoi(argv[2]);
		i = 3;
	}
	if (argc - i < 2) {
		fprintf(stderr, "usage: abpconv [-n MAXRULES] OUTDIR LIST...\n");
		return 2;
	}
	const char *out = argv[i++];
	abp *a = abp_new();
	for (; i < argc; i++) {
		gchar *t;
		gsize n;
		GError *e = NULL;
		if (!g_file_get_contents(argv[i], &t, &n, &e)) {
			fprintf(stderr, "abpconv: %s\n", e->message);
			return 1;
		}
		abp_add_list(a, t, n);
		g_free(t);
	}
	abp_finish(a);
	g_mkdir_with_parents(out, 0755);
	int nc = abp_chunks(a, max);
	for (int c = 0; c < nc; c++) {
		unsigned n;
		char *j = abp_chunk_json(a, max, c, &n);
		char *p = g_strdup_printf("%s/net-%d.json", out, c);
		g_file_set_contents(p, j, -1, NULL);
		printf("net-%d.json: %u rules\n", c, n);
		g_free(p);
		g_free(j);
	}
	char *g = abp_generic(a), *s = abp_site_table(a);
	char *p = g_strdup_printf("%s/generic.txt", out);
	g_file_set_contents(p, g, -1, NULL);
	g_free(p);
	p = g_strdup_printf("%s/site.tsv", out);
	g_file_set_contents(p, s, -1, NULL);
	g_free(p);
	const struct abp_stats *st = abp_stats(a);
	printf("lines %u; network: block %u, exception %u, important %u, badfilter %u, skipped %u\n", st->lines,
	       st->net_block, st->net_except, st->net_important, st->net_badfilter, st->net_skipped);
	printf("cosmetic: generic %u, site %u, exceptions %u, skipped (procedural) %u; scriptlets %u + %u generic; "
	       "site switches %u\n", st->cos_generic, st->cos_site, st->cos_exception, st->cos_procedural, st->scriptlets,
	       st->scriptlets_generic, st->allow_hosts);
	printf("generic.txt %zu bytes, site.tsv %zu bytes\n", strlen(g), strlen(s));
	return 0;
}
