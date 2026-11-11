/* libdssl conformance runner. SPDX-License-Identifier: MIT
 *
 *   conform [--quiet] CORPUS EXPECTED [GOLDEN]   levels 1 and 2 (and the CSS lane) against the committed expected output
 *   conform --fuzz CORPUS [N]           every input cut at N random points, plus mutations: must not crash
 *   conform --bench CORPUS              parse and lower 100 kB of DSSL, print the time
 *
 * Layout (see README): CORPUS/NAME.dssl, CORPUS/multi/NAME/NN.dssl; EXPECTED/NAME.l1.json (syntax tree,
 * level 1), NAME.json (resolved document, level 2), NAME.css (CSS lane, only where nothing needs the
 * engine), multi-NAME.json, and KNOWN-FAILURES (lines "LEVEL name reason": expected to fail, listed). */
#define _POSIX_C_SOURCE 200809L
#include "../dssl.h"
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static int quiet;

typedef struct { char **v; int n, cap; } Names;
static void names_add(Names *l, const char *s)
{
	if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 64; l->v = realloc(l->v, (size_t)l->cap * sizeof *l->v); if (!l->v) abort(); }
	l->v[l->n++] = strdup(s);
}
static void names_free(Names *l)
{
	for (int i = 0; i < l->n; i++) free(l->v[i]);
	free(l->v);
	l->v = NULL;
	l->n = l->cap = 0;
}
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static Names list_dir(const char *dir)
{
	Names l = { 0 };
	DIR *d = opendir(dir);
	if (!d) return l;
	struct dirent *e;
	while ((e = readdir(d))) if (e->d_name[0] != '.') names_add(&l, e->d_name);
	closedir(d);
	qsort(l.v, (size_t)l.n, sizeof *l.v, cmp_str);
	return l;
}

static char *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *b = malloc((size_t)n + 1);
	if (!b) abort();
	size_t r = fread(b, 1, (size_t)n, f);
	fclose(f);
	b[r] = 0;
	if (len) *len = r;
	return b;
}

static int ends_with(const char *s, const char *suf)
{
	size_t a = strlen(s), b = strlen(suf);
	return a >= b && strcmp(s + a - b, suf) == 0;
}

/* ------------------------------------------------------------------ known failures */

static Names known;
static int is_known(const char *level, const char *name)
{
	char key[512];
	snprintf(key, sizeof key, "%s %s", level, name);
	for (int i = 0; i < known.n; i++) {
		size_t kl = strlen(key);
		if (strncmp(known.v[i], key, kl) == 0 && (known.v[i][kl] == ' ' || known.v[i][kl] == 0)) return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------ comparison */

typedef struct { int pass, fail, skip, xfail, xpass; } Tally;
static Tally T[5];   /* 0 level 1, 1 level 2, 2 CSS lane, 3 multi-sheet, 4 golden */
static const char *const LEVEL[5] = { "L1", "L2", "CSS", "MULTI", "GOLD" };

static void show_diff(const char *got, const char *want)
{
	size_t i = 0;
	while (got[i] && want[i] && got[i] == want[i]) i++;
	size_t from = i > 60 ? i - 60 : 0;
	fprintf(stderr, "      first difference at byte %zu\n      got : ...%.140s\n      want: ...%.140s\n", i, got + from, want + from);
}

static void report(int level, const char *name, int ok, const char *got, const char *want)
{
	Tally *t = &T[level];
	int kn = is_known(LEVEL[level], name);
	if (ok) {
		if (kn) { t->xpass++; printf("XPASS %s %s (listed in KNOWN-FAILURES, now passes)\n", LEVEL[level], name); }
		else { t->pass++; if (!quiet) printf("PASS  %s %s\n", LEVEL[level], name); }
		return;
	}
	if (kn) { t->xfail++; if (!quiet) printf("XFAIL %s %s\n", LEVEL[level], name); return; }
	t->fail++;
	printf("FAIL  %s %s\n", LEVEL[level], name);
	if (got && want) show_diff(got, want);
}

static void skip(int level, const char *name, const char *why)
{
	T[level].skip++;
	if (!quiet) printf("SKIP  %s %s (%s)\n", LEVEL[level], name, why);
}

static char *trim_nl(char *s)
{
	size_t n = strlen(s);
	while (n && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = 0;
	return s;
}

static int engine_only(const dssl_sheet *s)
{
	/* texture, aura and slider values are not computed here (the JS engine owns them) */
	return (dssl_features(s) & (DSSL_FEAT_TEXTURE | DSSL_FEAT_AURA | DSSL_FEAT_SLIDER)) != 0;
}

static void run_case(const char *corpus, const char *expected, const char *file)
{
	char name[256];
	snprintf(name, sizeof name, "%.*s", (int)(strlen(file) - 5), file);
	char path[1024];
	snprintf(path, sizeof path, "%s/%s", corpus, file);
	size_t len;
	char *src = slurp(path, &len);
	if (!src) { printf("FAIL  IO %s\n", path); T[0].fail++; return; }
	dssl_sheet *s = dssl_parse(src, len, file, NULL);
	if (!s) { report(0, name, 0, "(NULL)", ""); free(src); return; }

	char epath[1024];
	/* level 1 */
	snprintf(epath, sizeof epath, "%s/%s.l1.json", expected, name);
	char *want = slurp(epath, NULL);
	if (want) {
		char *got = dssl_tree_json(s);
		report(0, name, got && strcmp(got, trim_nl(want)) == 0, got, want);
		free(got);
		free(want);
	} else skip(0, name, "no expected file");

	/* level 2 */
	snprintf(epath, sizeof epath, "%s/%s.json", expected, name);
	want = slurp(epath, NULL);
	if (want) {
		if (engine_only(s)) {
			/* texture, aura and slider values belong to the JS engine; where only texture is involved the
			 * expected document has "<engine>" for those values and the rest is compared */
			char npath[1024];
			snprintf(npath, sizeof npath, "%s/%s.norm.json", expected, name);
			char *nw = slurp(npath, NULL);
			if (nw) {
				char *got = dssl_to_json(s);
				report(1, name, got && strcmp(got, trim_nl(nw)) == 0, got, nw);
				free(got);
				free(nw);
			} else skip(1, name, "aura or slider: the JS engine owns the value");
		} else {
			char *got = dssl_to_json(s);
			report(1, name, got && strcmp(got, trim_nl(want)) == 0, got, want);
			free(got);
		}
		free(want);
	} else skip(1, name, "no expected file");

	/* the CSS lane, where the reference lowered the whole sheet */
	snprintf(epath, sizeof epath, "%s/%s.css", expected, name);
	want = slurp(epath, NULL);
	if (want) {
		if (engine_only(s)) skip(2, name, "the engine's static lane bakes textures into images; libdssl leaves them to the engine");
		else {
			char *got = dssl_to_css(s, 0);
			report(2, name, got && strcmp(got, want) == 0, got, want);
			free(got);
		}
		free(want);
	}
	dssl_free(s);
	free(src);
}

static void run_multi(const char *corpus, const char *expected, const char *dir)
{
	char mdir[1024];
	snprintf(mdir, sizeof mdir, "%s/multi/%s", corpus, dir);
	Names files = list_dir(mdir);
	dssl_ctx *ctx = dssl_ctx_new();
	size_t cap = 1 << 16, n = 0;
	char *out = malloc(cap);
	out[n++] = '[';
	int ok_all = 1;
	for (int i = 0; i < files.n; i++) {
		char p[1200], url[512];
		snprintf(p, sizeof p, "%s/%s", mdir, files.v[i]);
		snprintf(url, sizeof url, "%s-%s", dir, files.v[i]);
		size_t len;
		char *src = slurp(p, &len);
		dssl_sheet *s = dssl_parse(src, len, url, ctx);
		char *js = s ? dssl_to_json(s) : NULL;
		if (!js) { ok_all = 0; js = strdup("null"); }
		size_t jl = strlen(js);
		while (n + jl + 2 > cap) { cap *= 2; out = realloc(out, cap); }
		if (i) out[n++] = ',';
		memcpy(out + n, js, jl);
		n += jl;
		free(js);
		dssl_free(s);
		free(src);
	}
	out[n++] = ']';
	out[n] = 0;
	char epath[1024], name[300];
	snprintf(epath, sizeof epath, "%s/multi-%s.json", expected, dir);
	snprintf(name, sizeof name, "multi-%s", dir);
	char *want = slurp(epath, NULL);
	if (want) report(3, name, ok_all && strcmp(out, trim_nl(want)) == 0, out, want);
	else skip(3, name, "no expected file");
	free(want);
	free(out);
	dssl_ctx_free(ctx);
	for (int i = 0; i < files.n; i++) free(files.v[i]);
	free(files.v);
}

/* tests/golden: output reviewed by hand, for the parts the reference lane has no word for: lowering of animate:,
 * stack: and seen, fonts and raw, the CSS of several sheets in a row, and the feature list (NAME.feat) */
static void run_golden(const char *dir)
{
	Names files = list_dir(dir);
	for (int i = 0; i < files.n; i++) {
		const char *f = files.v[i];
		char path[2200];
		if (ends_with(f, ".dssl")) {
			char name[300];
			snprintf(name, sizeof name, "golden-%.*s", (int)(strlen(f) - 5), f);
			snprintf(path, sizeof path, "%s/%s", dir, f);
			size_t len;
			char *src = slurp(path, &len);
			dssl_sheet *s = src ? dssl_parse(src, len, f, NULL) : NULL;
			char gp[1024];
			snprintf(gp, sizeof gp, "%s/%.*s.css", dir, (int)(strlen(f) - 5), f);
			char *want = slurp(gp, NULL);
			char *got = s ? dssl_to_css(s, 0) : NULL;
			report(4, name, got && want && strcmp(got, want) == 0, got, want);
			free(got);
			free(want);
			/* the feature list: one "name line:col" per occurrence */
			snprintf(gp, sizeof gp, "%s/%.*s.feat", dir, (int)(strlen(f) - 5), f);
			want = slurp(gp, NULL);
			if (want && s) {
				char buf[16384];
				size_t o = 0;
				dssl_feature ft;
				for (size_t k = 0; dssl_feature_at(s, k, &ft) && o + 200 < sizeof buf; k++)
					o += (size_t)snprintf(buf + o, sizeof buf - o, "%s:%d:%d: %s%s\n", f, ft.line, ft.col, ft.name, (ft.kind & DSSL_FEAT_HARD_MASK) ? "" : " (lowered approximately)");
				if (!dssl_feature_count(s)) o += (size_t)snprintf(buf + o, sizeof buf - o, "%s: nothing needs the engine\n", f);
				char fname[300];
				snprintf(fname, sizeof fname, "golden-%.*s-features", (int)(strlen(f) - 5), f);
				report(4, fname, strcmp(buf, want) == 0, buf, want);
			}
			free(want);
			dssl_free(s);
			free(src);
		} else if (strncmp(f, "multi-", 6) == 0 && !ends_with(f, ".css")) {
			char mdir[1024];
			snprintf(mdir, sizeof mdir, "%s/%s", dir, f);
			Names sheets = list_dir(mdir);
			dssl_ctx *ctx = dssl_ctx_new();
			size_t cap = 1 << 16, n = 0;
			char *out = malloc(cap);
			out[0] = 0;
			for (int k = 0; k < sheets.n; k++) {
				snprintf(path, sizeof path, "%s/%s", mdir, sheets.v[k]);
				size_t len;
				char *src = slurp(path, &len);
				dssl_sheet *s = dssl_parse(src, len, sheets.v[k], ctx);
				char *css = s ? dssl_to_css(s, 0) : NULL;
				size_t cl = css ? strlen(css) : 0;
				while (n + cl + 1 > cap) { cap *= 2; out = realloc(out, cap); }
				if (css) memcpy(out + n, css, cl);
				n += cl;
				out[n] = 0;
				free(css);
				dssl_free(s);
				free(src);
			}
			char gp[1024], name[300];
			snprintf(gp, sizeof gp, "%s/%s.css", dir, f);
			snprintf(name, sizeof name, "golden-%s", f);
			char *want = slurp(gp, NULL);
			report(4, name, want && strcmp(out, want) == 0, out, want);
			free(want);
			free(out);
			dssl_ctx_free(ctx);
			names_free(&sheets);
		}
	}
	names_free(&files);
}

static int conform(const char *corpus, const char *expected, const char *golden)
{
	char kp[1024];
	snprintf(kp, sizeof kp, "%s/KNOWN-FAILURES", expected);
	char *kt = slurp(kp, NULL);
	if (kt) {
		for (char *line = strtok(kt, "\n"); line; line = strtok(NULL, "\n"))
			if (line[0] && line[0] != '#') names_add(&known, line);
	}
	Names files = list_dir(corpus);
	for (int i = 0; i < files.n; i++) if (ends_with(files.v[i], ".dssl")) run_case(corpus, expected, files.v[i]);
	char md[1024];
	snprintf(md, sizeof md, "%s/multi", corpus);
	Names multi = list_dir(md);
	for (int i = 0; i < multi.n; i++) run_multi(corpus, expected, multi.v[i]);

	names_free(&files);
	names_free(&multi);
	if (golden) run_golden(golden);
	names_free(&known);
	free(kt);
	int bad = 0;
	printf("\n");
	static const char *const title[5] = { "level 1 (syntax tree)", "level 2 (resolved document)", "CSS lane (vs the engine's static lane)", "multi-sheet (registries flow)", "golden (reviewed lowering output)" };
	for (int l = 0; l < 5; l++) {
		if (l == 4 && !golden) break;
		int total = T[l].pass + T[l].fail + T[l].xfail + T[l].xpass;
		printf("%-40s %4d / %-4d passed", title[l], T[l].pass + T[l].xpass, total);
		if (T[l].fail) printf(", %d FAILED", T[l].fail);
		if (T[l].xfail) printf(", %d known failures", T[l].xfail);
		if (T[l].skip) printf(", %d skipped", T[l].skip);
		printf("\n");
		bad += T[l].fail;
	}
	return bad ? 1 : 0;
}

/* ------------------------------------------------------------------ fuzz */

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
	rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
	return (uint32_t)(rng_state >> 11);
}

static long runs;

static void exercise(const char *buf, size_t len, dssl_ctx *ctx)
{
	dssl_sheet *s = dssl_parse(buf, len, "fuzz.dssl", ctx);
	runs++;
	if (!s) return;
	char *j = dssl_to_json(s);
	free(j);
	j = dssl_tree_json(s);
	free(j);
	j = dssl_to_css(s, DSSL_CSS_BANNER);
	free(j);
	dssl_diag d;
	for (size_t i = 0; dssl_error_at(s, i, &d); i++) if (d.line < 0 || !d.message) abort();
	for (size_t i = 0; dssl_warning_at(s, i, &d); i++) if (d.line < 0 || !d.message) abort();
	dssl_feature f;
	for (size_t i = 0; dssl_feature_at(s, i, &f); i++) if (!f.name) abort();
	(void)dssl_needs_engine(s);
	dssl_free(s);
}

static void synthetic(void)
{
	size_t big = 400000;
	char *b = malloc(big + 64);
	if (!b) abort();
	/* deep nesting, in several shapes */
	memset(b, '{', big); exercise(b, big, NULL);
	memset(b, '(', big); exercise(b, big, NULL);
	memset(b, '}', big); exercise(b, big, NULL);
	memset(b, '"', big); exercise(b, big, NULL);
	for (size_t i = 0; i < big; i += 2) { b[i] = '.'; b[i + 1] = i % 4 ? '{' : 'a'; } exercise(b, big, NULL);
	const char *nest = ".a{";
	size_t k = 0;
	for (int i = 0; i < 20000 && k + 8 < big; i++) { memcpy(b + k, nest, 3); k += 3; }
	exercise(b, k, NULL);
	/* a very long value with calls nested inside each other */
	k = 0;
	memcpy(b, ".a{width:", 9); k = 9;
	for (int i = 0; i < 50000 && k + 8 < big; i++) { memcpy(b + k, "calc(", 5); k += 5; }
	exercise(b, k, NULL);
	/* behaviors that use each other twice: the number of expansions doubles per level */
	{
		char *p = malloc(200000);
		size_t o = 0;
		for (int i = 0; i < 40; i++) o += (size_t)sprintf(p + o, "behavior b%d { use b%d\nuse b%d }\n", i, i + 1, i + 1);
		o += (size_t)sprintf(p + o, "behavior b40 { color: red }\n.a { use b0 }\n");
		exercise(p, o, NULL);
		free(p);
	}
	/* selector lists that multiply */
	{
		char *p = malloc(200000);
		size_t o = 0;
		for (int i = 0; i < 12; i++) o += (size_t)sprintf(p + o, ".a%d, .b%d, .c%d, .d%d, .e%d, .f%d {\n", i, i, i, i, i, i);
		o += (size_t)sprintf(p + o, "color: red\n");
		for (int i = 0; i < 12; i++) o += (size_t)sprintf(p + o, "}\n");
		exercise(p, o, NULL);
		free(p);
	}
	/* arbitrary bytes, NULs included */
	for (int round = 0; round < 40; round++) {
		size_t n = 1 + rnd() % 20000;
		for (size_t i = 0; i < n; i++) b[i] = (char)rnd();
		exercise(b, n, NULL);
	}
	/* printable noise made of the language's own punctuation */
	for (int round = 0; round < 40; round++) {
		static const char alpha[] = "{}()[];:,.#&@\"'/*\\-+>~ \n\tabc01%";
		size_t n = 1 + rnd() % 20000;
		for (size_t i = 0; i < n; i++) b[i] = alpha[rnd() % (sizeof alpha - 1)];
		exercise(b, n, NULL);
	}
	exercise("", 0, NULL);
	exercise(NULL, 0, NULL);
	free(b);
}

static int fuzz(const char *corpus, int rounds)
{
	Names files = list_dir(corpus);
	dssl_ctx *shared = dssl_ctx_new();
	for (int i = 0; i < files.n; i++) {
		if (!ends_with(files.v[i], ".dssl")) continue;
		char path[1024];
		snprintf(path, sizeof path, "%s/%s", corpus, files.v[i]);
		size_t len;
		char *src = slurp(path, &len);
		if (!src) continue;
		exercise(src, len, NULL);
		char *m = malloc(len + 16);
		for (int r = 0; r < rounds; r++) {
			size_t cut = len ? rnd() % (len + 1) : 0;
			exercise(src, cut, (r & 3) == 3 ? shared : NULL);        /* cut at a random point */
			memcpy(m, src, len);
			size_t flips = 1 + rnd() % 4;
			for (size_t f = 0; f < flips && len; f++) {
				static const char punct[] = "{}()[];:,\"'/*\\-&@.# \n";
				m[rnd() % len] = (rnd() & 1) ? punct[rnd() % (sizeof punct - 1)] : (char)rnd();
			}
			exercise(m, len ? len - (rnd() % (len / 8 + 1)) : 0, NULL);   /* mutated, a little shorter */
		}
		free(m);
		free(src);
	}
	dssl_ctx_free(shared);
	names_free(&files);
	synthetic();
	printf("fuzz: %ld parses of truncated and mutated inputs, no crash\n", runs);
	return 0;
}

/* ------------------------------------------------------------------ bench */

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int bench(const char *corpus)
{
	Names files = list_dir(corpus);
	size_t cap = 1 << 20, n = 0;
	char *buf = malloc(cap);
	/* real stylesheets first, then guide examples, until there are 100 kB */
	const char *prefixes[] = { "s-", "g-", "f-" };
	for (int pi = 0; pi < 3 && n < 100000; pi++)
		for (int i = 0; i < files.n && n < 100000; i++) {
			if (strncmp(files.v[i], prefixes[pi], 2) != 0 || !ends_with(files.v[i], ".dssl")) continue;
			char path[1024];
			snprintf(path, sizeof path, "%s/%s", corpus, files.v[i]);
			size_t len;
			char *src = slurp(path, &len);
			if (!src) continue;
			if (n + len + 2 < cap) { memcpy(buf + n, src, len); n += len; buf[n++] = '\n'; }
			free(src);
		}
	printf("input: %zu bytes\n", n);
	double best_parse = 1e9, best_css = 1e9, sum = 0;
	int iters = 40;
	for (int i = 0; i < iters; i++) {
		double t0 = now_ms();
		dssl_sheet *s = dssl_parse(buf, n, "bench.dssl", NULL);
		double t1 = now_ms();
		char *css = dssl_to_css(s, 0);
		double t2 = now_ms();
		free(css);
		dssl_free(s);
		if (t1 - t0 < best_parse) best_parse = t1 - t0;
		if (t2 - t1 < best_css) best_css = t2 - t1;
		sum += t1 - t0;
	}
	printf("parse + resolve: best %.2f ms, mean %.2f ms (%d runs)\n", best_parse, sum / iters, iters);
	printf("lowering to CSS: best %.2f ms\n", best_css);
	printf("per 100 kB: %.2f ms\n", best_parse * 100000.0 / (double)n);
	names_free(&files);
	free(buf);
	return 0;
}

int main(int argc, char **argv)
{
	int i = 1;
	if (i < argc && strcmp(argv[i], "--quiet") == 0) { quiet = 1; i++; }
	if (i < argc && strcmp(argv[i], "--fuzz") == 0 && i + 1 < argc) return fuzz(argv[i + 1], i + 2 < argc ? atoi(argv[i + 2]) : 8);
	if (i < argc && strcmp(argv[i], "--bench") == 0 && i + 1 < argc) return bench(argv[i + 1]);
	if (i + 1 < argc) return conform(argv[i], argv[i + 1], i + 2 < argc ? argv[i + 2] : NULL);
	fprintf(stderr, "usage: conform [--quiet] CORPUS EXPECTED [GOLDEN] | --fuzz CORPUS [N] | --bench CORPUS\n");
	return 2;
}
