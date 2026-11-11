/* dssl2css: DSSL to CSS with libdssl. SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "dssl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *f)
{
	fprintf(f,
	    "usage: dssl2css [options] FILE...      (FILE - is standard input)\n"
	    "  -o FILE         write the output to FILE (default: standard output)\n"
	    "  --json          the resolved document (spec chapter 10) as canonical JSON, one line per file\n"
	    "  --tree          the syntax tree and parser errors (spec chapter 2) as JSON\n"
	    "  --features      list what needs the engine, one line per occurrence; no CSS\n"
	    "  --check         diagnostics only\n"
	    "  --banner        start the CSS with a comment naming the file\n"
	    "  -q              no diagnostics on standard error\n"
	    "  --strict-engine exit with status 3 when a file needs the JavaScript engine\n"
	    "  --version, --help\n"
	    "Files are one cascade in the order given: tokens and motions flow from file to file.\n"
	    "Exit status: 0 ok, 1 errors in the DSSL (output is still written), 2 usage or I/O error,\n"
	    "3 needs the engine (with --strict-engine).\n");
}

static char *slurp(const char *path, size_t *len)
{
	FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
	if (!f) return NULL;
	size_t cap = 1 << 16, n = 0;
	char *buf = malloc(cap);
	if (!buf) { if (f != stdin) fclose(f); return NULL; }
	for (;;) {
		size_t r = fread(buf + n, 1, cap - n, f);
		n += r;
		if (n < cap) break;
		cap *= 2;
		char *nb = realloc(buf, cap);
		if (!nb) { free(buf); if (f != stdin) fclose(f); return NULL; }
		buf = nb;
	}
	int bad = ferror(f);
	if (f != stdin) fclose(f);
	if (bad) { free(buf); return NULL; }
	*len = n;
	return buf;
}

int main(int argc, char **argv)
{
	const char *out_path = NULL;
	int mode = 0;   /* 0 css, 1 json, 2 tree, 3 features, 4 check */
	unsigned flags = 0;
	int quiet = 0, strict_engine = 0;
	char **files = calloc((size_t)argc + 1, sizeof *files);
	int nfiles = 0;
	if (!files) return 2;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (strcmp(a, "-o") == 0 && i + 1 < argc) out_path = argv[++i];
		else if (strcmp(a, "--json") == 0) mode = 1;
		else if (strcmp(a, "--tree") == 0) mode = 2;
		else if (strcmp(a, "--features") == 0) mode = 3;
		else if (strcmp(a, "--check") == 0) mode = 4;
		else if (strcmp(a, "--banner") == 0) flags |= DSSL_CSS_BANNER;
		else if (strcmp(a, "-q") == 0) quiet = 1;
		else if (strcmp(a, "--strict-engine") == 0) strict_engine = 1;
		else if (strcmp(a, "--version") == 0) { printf("dssl2css %s (DSSL spec %s)\n", DSSL_VERSION, DSSL_SPEC_VERSION); return 0; }
		else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) { usage(stdout); return 0; }
		else if (a[0] == '-' && a[1] && strcmp(a, "-") != 0) { fprintf(stderr, "dssl2css: unknown option %s\n", a); usage(stderr); return 2; }
		else files[nfiles++] = argv[i];
	}
	if (!nfiles) { usage(stderr); return 2; }
	FILE *out = stdout;
	if (out_path && !(out = fopen(out_path, "wb"))) { perror(out_path); return 2; }
	dssl_ctx *ctx = dssl_ctx_new();
	if (!ctx) { fprintf(stderr, "dssl2css: out of memory\n"); return 2; }
	int status = 0;
	for (int i = 0; i < nfiles; i++) {
		size_t len;
		char *src = slurp(files[i], &len);
		if (!src) { fprintf(stderr, "dssl2css: cannot read %s\n", files[i]); status = 2; continue; }
		dssl_sheet *s = dssl_parse(src, len, strcmp(files[i], "-") == 0 ? "stdin" : files[i], ctx);
		free(src);
		if (!s) { fprintf(stderr, "dssl2css: %s: out of memory or too large\n", files[i]); status = 2; continue; }
		if (!quiet) {
			dssl_diag d;
			for (size_t k = 0; dssl_error_at(s, k, &d); k++) {
				fprintf(stderr, "%s:%d:%d: error: %s\n", d.file, d.line, d.col, d.message);
				if (d.hint) fprintf(stderr, "    %s\n", d.hint);
			}
			for (size_t k = 0; dssl_warning_at(s, k, &d); k++) {
				fprintf(stderr, "%s:%d:%d: warning: %s\n", d.file, d.line, d.col, d.message);
				if (d.hint) fprintf(stderr, "    %s\n", d.hint);
			}
		}
		if (dssl_error_count(s) && status < 1) status = 1;
		if (strict_engine && dssl_needs_engine(s) && status < 3) status = 3;
		char *text = NULL;
		if (mode == 0) text = dssl_to_css(s, flags);
		else if (mode == 1) text = dssl_to_json(s);
		else if (mode == 2) text = dssl_tree_json(s);
		else if (mode == 3) {
			dssl_feature f;
			for (size_t k = 0; dssl_feature_at(s, k, &f); k++)
				fprintf(out, "%s:%d:%d: %s%s\n", files[i], f.line, f.col, f.name, (f.kind & DSSL_FEAT_HARD_MASK) ? "" : " (lowered approximately)");
			if (!dssl_feature_count(s)) fprintf(out, "%s: nothing needs the engine\n", files[i]);
		}
		if (text) {
			fputs(text, out);
			if (mode == 1 || mode == 2) fputc('\n', out);
			free(text);
		} else if (mode <= 2) { fprintf(stderr, "dssl2css: out of memory\n"); status = 2; }
		dssl_free(s);
	}
	dssl_ctx_free(ctx);
	free(files);
	if (out != stdout) fclose(out);
	return status;
}
