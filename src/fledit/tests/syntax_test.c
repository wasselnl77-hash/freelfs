/* syntax_test -- checks of the line lexers (no display needed)
 * SPDX-License-Identifier: MIT
 *
 *   syntax_test            the built-in checks
 *   syntax_test FILE...    colour every file line by line, with guard bytes after each class
 *                          buffer (for AddressSanitizer runs), and print the throughput
 */
#include "../syntax.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails, checks;

/* Lex the lines of text (split at \n) and return the class of the byte at (line, col) */
static int class_at(sx_lang l, const char *text, int line, int col)
{
	struct sx_ctx cx;
	memset(&cx, 0, sizeof cx);
	sx_state st = 0;
	const char *p = text;
	for (int ln = 0;; ln++) {
		const char *nl = strchr(p, '\n');
		size_t n = nl ? (size_t)(nl - p) : strlen(p);
		uint8_t cls[512];
		st = sx_line(&cx, l, st, p, n, cls);
		if (ln == line)
			return col < (int)n ? cls[col] : -1;
		if (!nl)
			break;
		p = nl + 1;
	}
	return -1;
}

/* the word w found on the line at index `line` has class k (checks its first byte and its last) */
static void want(sx_lang l, const char *text, int line, const char *w, int k)
{
	const char *p = text;
	for (int i = 0; i < line; i++) {
		p = strchr(p, '\n');
		if (!p)
			break;
		p++;
	}
	const char *e = p ? strchr(p, '\n') : NULL;
	char buf[400];
	size_t n = p ? (e ? (size_t)(e - p) : strlen(p)) : 0;
	memcpy(buf, p, n);
	buf[n] = 0;
	const char *hit = strstr(buf, w);
	checks++;
	if (!hit) {
		printf("FAIL %s: %s not on line %d\n", sx_name(l), w, line);
		fails++;
		return;
	}
	int col = (int)(hit - buf), last = col + (int)strlen(w) - 1;
	int a = class_at(l, text, line, col), b = class_at(l, text, line, last);
	if (a != k || b != k) {
		printf("FAIL %s: \"%s\" on line %d is class %d..%d, wanted %d\n", sx_name(l), w, line, a, b, k);
		fails++;
	}
}

static void builtin(void)
{
	/* detection */
	struct {
		const char *path, *first;
		sx_lang l;
	} det[] = {
		{"/a/b/main.c", "", L_C}, {"x.PY", "", L_PY}, {"recipes/fledit/recipe", "", L_SH}, {"/etc/fstab", "", L_CONF},
		{"style.css", "", L_CSS}, {"site.dssl", "", L_DSSL}, {"README.md", "", L_MD}, {"a.desktop", "", L_CONF},
		{"x.json", "", L_JS}, {"index.html", "", L_HTML}, {"script", "#!/usr/bin/env python3", L_PY},
		{"tool", "#!/bin/sh", L_SH}, {"notes.txt", "", L_NONE}, {".bashrc", "", L_SH},
	};
	for (size_t i = 0; i < sizeof det / sizeof *det; i++) {
		checks++;
		if (sx_detect(det[i].path, det[i].first, strlen(det[i].first)) != det[i].l) {
			printf("FAIL detect %s\n", det[i].path);
			fails++;
		}
	}

	/* C */
	const char *c = "#include <stdio.h>\n#define MAX(a, b) ((a) > (b) ? (a) : (b)) \\\n  + 1\n"
	                "/* a comment\n   goes on */ int x = 0x1F; // end\nstruct foo *p = \"str\\\"ing\"; size_t n;\n"
	                "static void run(void) { return NULL; }\n";
	want(L_C, c, 0, "#include", K_PREPROC);
	want(L_C, c, 0, "<stdio.h>", K_STRING);
	want(L_C, c, 1, "#define", K_PREPROC);
	want(L_C, c, 2, "+", K_PUNCT); /* the continued line is still a directive but its tokens are lexed */
	want(L_C, c, 3, "/* a comment", K_COMMENT);
	want(L_C, c, 4, "goes on */", K_COMMENT);
	want(L_C, c, 4, "int", K_TYPE);
	want(L_C, c, 4, "0x1F", K_NUMBER);
	want(L_C, c, 4, "// end", K_COMMENT);
	want(L_C, c, 5, "struct", K_KEYWORD);
	want(L_C, c, 5, "foo", K_TYPE);
	want(L_C, c, 5, "\"str\\\"ing\"", K_STRING);
	want(L_C, c, 5, "size_t", K_TYPE);
	want(L_C, c, 6, "static", K_KEYWORD);
	want(L_C, c, 6, "run", K_FUNC);
	want(L_C, c, 6, "NULL", K_KEYWORD);

	/* shell with a here-document */
	const char *sh = "#!/bin/sh\n# comment\nFOO=1 make -j4 \"$HOME/x\" 'it'\ncat <<'EOF'\nit's a \"quote\n$notcode\nEOF\n"
	                 "if [ -f x ]; then echo $? ; fi\nx=$(date +%s)\n";
	want(L_SH, sh, 0, "#!/bin/sh", K_PREPROC);
	want(L_SH, sh, 1, "# comment", K_COMMENT);
	want(L_SH, sh, 2, "FOO", K_PROP);
	want(L_SH, sh, 2, "make", K_FUNC);
	want(L_SH, sh, 2, "-j4", K_TYPE);
	want(L_SH, sh, 2, "$HOME", K_PROP);
	want(L_SH, sh, 2, "'it'", K_STRING);
	want(L_SH, sh, 3, "cat", K_FUNC);
	want(L_SH, sh, 4, "it's a \"quote", K_STRING);
	want(L_SH, sh, 5, "$notcode", K_STRING);
	want(L_SH, sh, 6, "EOF", K_PREPROC);
	want(L_SH, sh, 7, "if", K_KEYWORD);
	want(L_SH, sh, 7, "echo", K_FUNC);
	want(L_SH, sh, 7, "then", K_KEYWORD);
	want(L_SH, sh, 7, "$?", K_PROP);
	want(L_SH, sh, 8, "date", K_FUNC);

	/* Python */
	const char *py = "@dec.orator\ndef fun(x: int = 3):\n    \"\"\"doc\n    string\"\"\" # c\n    s = f'a{b}' + r\"\\d\"\nclass Foo(Base): pass\n";
	want(L_PY, py, 0, "@dec.orator", K_PREPROC);
	want(L_PY, py, 1, "def", K_KEYWORD);
	want(L_PY, py, 1, "fun", K_FUNC);
	want(L_PY, py, 1, "int", K_TYPE);
	want(L_PY, py, 1, "3", K_NUMBER);
	want(L_PY, py, 2, "\"\"\"doc", K_STRING);
	want(L_PY, py, 3, "string\"\"\"", K_STRING);
	want(L_PY, py, 3, "# c", K_COMMENT);
	want(L_PY, py, 4, "f'a{b}'", K_STRING);
	want(L_PY, py, 4, "r\"\\d\"", K_STRING);
	want(L_PY, py, 5, "Foo", K_TYPE);

	/* JavaScript / JSON */
	const char *js = "const re = /ab+c/gi; let t = `x ${a + 1} y`;\n/* open\nstill */ x = a / b / c;\n{\"key\": [1, 2.5e3, true, null]}\nconsole.log(\"hi\");\n";
	want(L_JS, js, 0, "const", K_KEYWORD);
	want(L_JS, js, 0, "/ab+c/gi", K_PREPROC);
	want(L_JS, js, 0, "`x ", K_STRING);
	want(L_JS, js, 0, "1", K_NUMBER);
	want(L_JS, js, 0, " y`", K_STRING);
	want(L_JS, js, 1, "/* open", K_COMMENT);
	want(L_JS, js, 2, "still */", K_COMMENT);
	want(L_JS, js, 2, "/ b /", K_PUNCT); /* division, not a regular expression */
	want(L_JS, js, 3, "\"key\"", K_PROP);
	want(L_JS, js, 3, "2.5e3", K_NUMBER);
	want(L_JS, js, 3, "true", K_KEYWORD);
	want(L_JS, js, 4, "log", K_FUNC);

	/* CSS */
	const char *css = "/* c */\n.a:hover, #b > p {\n  color: #fff;\n  margin: 0 auto !important;\n  background: url(x.png) 10px;\n}\n@media (min-width: 600px) {\n  .c { --x: 1; }\n}\n";
	want(L_CSS, css, 0, "/* c */", K_COMMENT);
	want(L_CSS, css, 1, ".a", K_TYPE);
	want(L_CSS, css, 1, ":hover", K_PREPROC);
	want(L_CSS, css, 1, "#b", K_FUNC);
	want(L_CSS, css, 1, "p", K_KEYWORD);
	want(L_CSS, css, 2, "color", K_PROP);
	want(L_CSS, css, 2, "#fff", K_NUMBER);
	want(L_CSS, css, 3, "margin", K_PROP);
	want(L_CSS, css, 3, "!important", K_KEYWORD);
	want(L_CSS, css, 4, "url", K_FUNC);
	want(L_CSS, css, 4, "10px", K_NUMBER);
	want(L_CSS, css, 6, "@media", K_PREPROC);
	want(L_CSS, css, 7, ".c", K_TYPE);
	want(L_CSS, css, 7, "--x", K_PROP);

	/* HTML with embedded script and style */
	const char *html = "<!DOCTYPE html>\n<!-- c\n--><a href=\"x\" class=y>t &amp; u</a>\n<style>\n.k { color: red }\n</style>\n<script>\nvar a = 1; // c\n</script>\n<p\n id='z'>\n";
	want(L_HTML, html, 0, "<!DOCTYPE", K_PREPROC);
	want(L_HTML, html, 1, "<!-- c", K_COMMENT);
	want(L_HTML, html, 2, "-->", K_COMMENT);
	want(L_HTML, html, 2, "a", K_TAG);
	want(L_HTML, html, 2, "href", K_PROP);
	want(L_HTML, html, 2, "\"x\"", K_STRING);
	want(L_HTML, html, 2, "y", K_STRING);
	want(L_HTML, html, 2, "&amp;", K_PREPROC);
	want(L_HTML, html, 4, ".k", K_TYPE);
	want(L_HTML, html, 4, "color", K_PROP);
	want(L_HTML, html, 5, "style", K_TAG);
	want(L_HTML, html, 7, "var", K_KEYWORD);
	want(L_HTML, html, 7, "// c", K_COMMENT);
	want(L_HTML, html, 10, "'z'", K_STRING);
	want(L_HTML, html, 10, "id", K_PROP);

	/* DSSL */
	const char *ds = "// a comment\ntokens {\n  color {\n    ink: #16161d\n    accent: oklch(62% 0.19 35)\n  }\n  space { 1: 4 }\n}\n"
	                 ".card {\n  background: color.ink/20\n  padding: space.4 8\n  hover {\n    radius: 6\n  }\n  use fire\n  truncate\n}\n"
	                "@media 768 { .a { size: 10 } }\nraw {\n  .x { color: red; }\n}\n/* x\ny */\nbehavior fire {\n  aura-glow: #ff7a00 36 -> 1\n}\n";
	want(L_DSSL, ds, 0, "// a comment", K_COMMENT);
	want(L_DSSL, ds, 1, "tokens", K_KEYWORD);
	want(L_DSSL, ds, 3, "ink", K_PROP);
	want(L_DSSL, ds, 3, "#16161d", K_NUMBER);
	want(L_DSSL, ds, 4, "oklch", K_FUNC);
	want(L_DSSL, ds, 4, "62%", K_NUMBER);
	want(L_DSSL, ds, 6, "1", K_NUMBER);
	want(L_DSSL, ds, 8, ".card", K_TYPE);
	want(L_DSSL, ds, 9, "background", K_PROP);
	want(L_DSSL, ds, 9, "color.ink", K_TYPE);
	want(L_DSSL, ds, 9, "/20", K_NUMBER);
	want(L_DSSL, ds, 11, "hover", K_PREPROC);
	want(L_DSSL, ds, 12, "radius", K_PROP);
	want(L_DSSL, ds, 14, "use", K_KEYWORD);
	want(L_DSSL, ds, 14, "fire", K_TYPE);
	want(L_DSSL, ds, 15, "truncate", K_PROP);
	want(L_DSSL, ds, 17, "@media", K_PREPROC);
	want(L_DSSL, ds, 17, "768", K_NUMBER);
	want(L_DSSL, ds, 17, "size", K_PROP);
	want(L_DSSL, ds, 18, "raw", K_KEYWORD);
	want(L_DSSL, ds, 19, ".x", K_TYPE);
	want(L_DSSL, ds, 19, "color", K_PROP);
	want(L_DSSL, ds, 21, "/* x", K_COMMENT);
	want(L_DSSL, ds, 22, "y */", K_COMMENT);
	want(L_DSSL, ds, 23, "behavior", K_KEYWORD);
	want(L_DSSL, ds, 23, "fire", K_TYPE);
	want(L_DSSL, ds, 24, "aura-glow", K_PROP);
	want(L_DSSL, ds, 24, "->", K_PUNCT);

	/* Markdown */
	const char *md = "# Title\ntext **bold** and `code` and [a link](http://x.y)\n- item\n```c\nint x;\n```\nafter\n> quote\n| a | b |\n|---|---|\n";
	want(L_MD, md, 0, "# Title", K_HEAD);
	want(L_MD, md, 1, "**bold**", K_EMPH);
	want(L_MD, md, 1, "`code`", K_STRING);
	want(L_MD, md, 1, "[a link]", K_LINK);
	want(L_MD, md, 1, "(http://x.y)", K_COMMENT);
	want(L_MD, md, 2, "-", K_KEYWORD);
	want(L_MD, md, 3, "```", K_PUNCT);
	want(L_MD, md, 4, "int x;", K_STRING);
	want(L_MD, md, 5, "```", K_PUNCT);
	want(L_MD, md, 6, "after", K_TEXT);
	want(L_MD, md, 7, ">", K_PUNCT);
	want(L_MD, md, 8, "|", K_PUNCT);
	want(L_MD, md, 9, "|---|---|", K_PUNCT);

	/* conf */
	const char *cf = "# c\n[Desktop Entry]\nName=Text Editor\nExec=fledit %f\nTerminal=false\nKey = 12 # tail\nbare value\n";
	want(L_CONF, cf, 0, "# c", K_COMMENT);
	want(L_CONF, cf, 1, "[Desktop Entry]", K_SECTION);
	want(L_CONF, cf, 2, "Name", K_PROP);
	want(L_CONF, cf, 3, "%f", K_PREPROC);
	want(L_CONF, cf, 4, "false", K_KEYWORD);
	want(L_CONF, cf, 5, "12", K_NUMBER);
	want(L_CONF, cf, 5, "# tail", K_COMMENT);
	want(L_CONF, cf, 6, "bare", K_PROP);
}

/* colour whole files; every class buffer has guard bytes behind it */
static int run_files(int argc, char **argv)
{
	for (int a = 1; a < argc; a++) {
		FILE *f = fopen(argv[a], "rb");
		if (!f) {
			perror(argv[a]);
			return 1;
		}
		fseek(f, 0, SEEK_END);
		long sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		char *buf = malloc((size_t)sz + 1);
		if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
			fclose(f);
			return 1;
		}
		fclose(f);
		buf[sz] = 0;
		sx_lang l = sx_detect(argv[a], buf, (size_t)sz);
		for (int pass = 0; pass < 2; pass++) { /* pass 1: every language on every file */
			for (sx_lang lg = pass ? L_C : l; lg < (pass ? L_COUNT : l + 1); lg++) {
				struct sx_ctx *cx = calloc(1, sizeof *cx);
				sx_state st = 0;
				struct timespec t0, t1;
				clock_gettime(CLOCK_MONOTONIC, &t0);
				size_t lines = 0;
				for (long p = 0; p <= sz;) {
					char *nl = memchr(buf + p, '\n', (size_t)(sz - p));
					size_t n = nl ? (size_t)(nl - (buf + p)) : (size_t)(sz - p);
					uint8_t *cls = malloc(n + 8);
					memset(cls + n, 0xA5, 8);
					st = sx_line(cx, lg, st, buf + p, n, cls);
					for (int g = 0; g < 8; g++)
						if (cls[n + g] != 0xA5) {
							printf("FAIL %s: wrote past the line (%s)\n", argv[a], sx_name(lg));
							return 1;
						}
					free(cls);
					uint8_t dummy = 0;
					(void)dummy;
					lines++;
					if (!nl)
						break;
					p = (long)(nl - buf) + 1;
				}
				clock_gettime(CLOCK_MONOTONIC, &t1);
				double ms = (double)(t1.tv_sec - t0.tv_sec) * 1e3 + (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
				if (!pass)
					printf("%s: %s, %zu lines, %.1f ms\n", argv[a], sx_name(lg), lines, ms);
				free(cx);
			}
		}
		free(buf);
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc > 1)
		return run_files(argc, argv);
	builtin();
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
