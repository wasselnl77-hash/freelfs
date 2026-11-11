/* syntax.c -- line lexers for C, shell, Python, JavaScript/JSON, HTML, CSS, DSSL, Markdown, conf/ini
 * SPDX-License-Identifier: MIT
 *
 * These are colourers, not parsers: they have to be fast, never fail and never
 * read outside the line. What carries over from one line to the next is a small
 * state (an open comment, string, here-document, tag, CSS block depth).
 *
 * DSSL: a CSS-like language without semicolons: `//` and block comments, blocks
 * `selector { name: value }`, the words tokens/behavior/motion/raw/fonts/use,
 * state blocks (hover { }), token paths (color.ink), colours with alpha (#fff/50).
 * See src/libdssl/README and the corpus there.
 */
#include "syntax.h"

#include <stdlib.h>
#include <string.h>

/* ---- helpers ------------------------------------------------------------------------------ */

static void paint(uint8_t *cls, size_t a, size_t b, int k)
{
	if (cls && b > a)
		memset(cls + a, k, b - a);
}

static int isid0(unsigned c) { return c == '_' || (c | 32) - 'a' < 26u || c >= 0x80; }
static int isid(unsigned c) { return isid0(c) || c - '0' < 10u; }
static int isdig(unsigned c) { return c - '0' < 10u; }
static int isws(unsigned c) { return c == ' ' || c == '\t' || c == '\r'; }
static int isalp(unsigned c) { return (c | 32) - 'a' < 26u; }
static int ishex(unsigned c) { return isdig(c) || (c | 32) - 'a' < 6u; }

static size_t skipws(const char *s, size_t i, size_t n)
{
	while (i < n && isws((unsigned char)s[i]))
		i++;
	return i;
}

/* end of a block comment that started before i (the "*" "/" included); *closed tells whether found */
static size_t block_end(const char *s, size_t i, size_t n, int *closed)
{
	for (; i + 1 < n; i++)
		if (s[i] == '*' && s[i + 1] == '/') {
			*closed = 1;
			return i + 2;
		}
	*closed = 0;
	return n;
}

/* a quoted string starting at the quote s[i]; returns the index after the closing quote, or n.
 * *closed is 0 when the line ended first. */
static size_t quoted(const char *s, size_t i, size_t n, int *closed)
{
	char q = s[i];
	size_t j = i + 1;
	while (j < n) {
		if (s[j] == '\\' && j + 1 < n) {
			j += 2;
			continue;
		}
		if (s[j] == q) {
			*closed = 1;
			return j + 1;
		}
		j++;
	}
	*closed = 0;
	return n;
}

/* a number: digits, letters, dots and underscores; a sign only right after an exponent letter */
static size_t number(const char *s, size_t i, size_t n)
{
	size_t j = i;
	int hex = j + 1 < n && s[j] == '0' && (s[j + 1] | 32) == 'x';
	while (j < n) {
		unsigned char c = (unsigned char)s[j];
		if (isid(c) || c == '.' || c == '\'')
			j++;
		else if ((c == '+' || c == '-') && j > i && !hex && ((s[j - 1] | 32) == 'e' || (s[j - 1] | 32) == 'p'))
			j++;
		else
			break;
	}
	return j;
}

/* sorted word sets, built on first use from a space separated list */
struct kws {
	const char *src;
	char *buf;
	const char **w;
	int n;
};

static int cmpw(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static void kws_init(struct kws *k)
{
	size_t len = strlen(k->src);
	k->buf = malloc(len + 1);
	if (!k->buf)
		return;
	memcpy(k->buf, k->src, len + 1);
	int cnt = 0;
	for (size_t i = 0; i < len; i++)
		if (k->buf[i] != ' ' && (i == 0 || k->buf[i - 1] == ' '))
			cnt++;
	k->w = malloc((size_t)(cnt ? cnt : 1) * sizeof *k->w);
	if (!k->w)
		return;
	k->n = 0;
	for (char *p = k->buf; *p;) {
		while (*p == ' ')
			*p++ = 0;
		if (!*p)
			break;
		k->w[k->n++] = p;
		while (*p && *p != ' ')
			p++;
	}
	/* a trailing space in the list leaves p on a NUL, which ends the loop */
	qsort(k->w, (size_t)k->n, sizeof *k->w, cmpw);
}

static int kws_has(struct kws *k, const char *s, size_t n)
{
	if (!k->w) {
		if (k->buf)
			return 0;
		kws_init(k);
		if (!k->w)
			return 0;
	}
	if (n == 0 || n > 40)
		return 0;
	int lo = 0, hi = k->n - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		int c = strncmp(k->w[mid], s, n);
		if (c == 0 && k->w[mid][n])
			c = 1; /* the word is longer than s */
		if (c == 0)
			return 1;
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
	return 0;
}

#define KWS(name, list) static struct kws name = {list, NULL, NULL, 0}

/* ---- C and C++ ------------------------------------------------------------------------------ */

KWS(c_kw, "auto break case const continue default do else enum extern for goto if inline register restrict return "
          "sizeof static struct switch typedef union volatile while _Alignas _Alignof _Atomic _Generic _Noreturn "
          "_Static_assert _Thread_local alignas alignof asm class catch constexpr consteval constinit decltype "
          "delete explicit export false final friend mutable namespace new noexcept nullptr operator override "
          "private protected public static_assert static_cast dynamic_cast reinterpret_cast const_cast template "
          "this throw true try typeid typename using virtual NULL true false import package interface "
          "implements extends instanceof synchronized throws abstract");
KWS(c_ty, "bool char double float int long short signed unsigned void _Bool _Complex wchar_t char8_t char16_t "
          "char32_t size_t ssize_t ptrdiff_t intptr_t uintptr_t off_t pid_t uid_t gid_t mode_t time_t FILE DIR "
          "va_list string vector map set unique_ptr shared_ptr byte boolean String Object Integer Long");

enum { C_NORM, C_BLOCK, C_PPCONT };

static sx_state lex_c(sx_state st, const char *s, size_t n, uint8_t *cls)
{
	size_t i = 0;
	int pp = st == C_PPCONT, first = st != C_PPCONT, tagnext = 0;
	if (st == C_BLOCK) {
		int closed;
		size_t j = block_end(s, 0, n, &closed);
		paint(cls, 0, j, K_COMMENT);
		if (!closed)
			return C_BLOCK;
		i = j;
	}
	while (i < n) {
		unsigned char c = (unsigned char)s[i];
		if (isws(c)) {
			i++;
			continue;
		}
		if (c == '/' && i + 1 < n && s[i + 1] == '*') {
			int closed;
			size_t j = block_end(s, i + 2, n, &closed);
			paint(cls, i, j, K_COMMENT);
			if (!closed)
				return C_BLOCK;
			i = j;
			continue;
		}
		if (c == '/' && i + 1 < n && s[i + 1] == '/') {
			paint(cls, i, n, K_COMMENT);
			i = n;
			break;
		}
		if (first && c == '#') {
			size_t j = skipws(s, i + 1, n), w = j;
			while (j < n && isid((unsigned char)s[j]))
				j++;
			paint(cls, i, j, K_PREPROC);
			int inc = j - w == 7 && !strncmp(s + w, "include", 7);
			first = 0;
			pp = 1;
			i = j;
			if (inc) {
				j = skipws(s, i, n);
				if (j < n && s[j] == '<') {
					size_t k = j;
					while (k < n && s[k] != '>')
						k++;
					if (k < n)
						k++;
					paint(cls, j, k, K_STRING);
					i = k;
				}
			}
			continue;
		}
		first = 0;
		if (c == '"' || c == '\'') {
			int closed;
			size_t j = quoted(s, i, n, &closed);
			paint(cls, i, j, K_STRING);
			i = j;
			tagnext = 0;
			continue;
		}
		if (isdig(c) || (c == '.' && i + 1 < n && isdig((unsigned char)s[i + 1]))) {
			size_t j = number(s, i, n);
			paint(cls, i, j, K_NUMBER);
			i = j;
			tagnext = 0;
			continue;
		}
		if (isid0(c)) {
			size_t j = i;
			while (j < n && isid((unsigned char)s[j]))
				j++;
			size_t len = j - i;
			int k;
			if (tagnext)
				k = K_TYPE;
			else if (kws_has(&c_kw, s + i, len))
				k = K_KEYWORD;
			else if (kws_has(&c_ty, s + i, len) || (len > 2 && s[j - 2] == '_' && s[j - 1] == 't'))
				k = K_TYPE;
			else {
				size_t q = skipws(s, j, n);
				int caps = len >= 2, upper = 0;
				for (size_t z = i; z < j; z++) {
					unsigned char ch = (unsigned char)s[z];
					if (ch >= 'A' && ch <= 'Z')
						upper = 1;
					else if (!isdig(ch) && ch != '_')
						caps = 0;
				}
				if (q < n && s[q] == '(')
					k = K_FUNC;
				else if (caps && upper)
					k = K_PREPROC;
				else
					k = K_TEXT;
			}
			tagnext = k == K_KEYWORD && ((len == 6 && !strncmp(s + i, "struct", 6)) ||
			                             (len == 5 && (!strncmp(s + i, "union", 5) || !strncmp(s + i, "class", 5))) ||
			                             (len == 4 && !strncmp(s + i, "enum", 4)));
			paint(cls, i, j, k);
			i = j;
			continue;
		}
		tagnext = 0;
		if (strchr("{}()[];,.:?+-*/%=<>!&|^~", c))
			paint(cls, i, i + 1, K_PUNCT);
		i++;
	}
	if (pp) {
		size_t e = n;
		while (e > 0 && isws((unsigned char)s[e - 1]))
			e--;
		if (e > 0 && s[e - 1] == '\\')
			return C_PPCONT;
	}
	return C_NORM;
}

/* ---- shell ---------------------------------------------------------------------------------------- */

KWS(sh_kw, "if then else elif fi for while until do done case esac in function select time coproc "
           "return exit break continue local export readonly declare typeset unset shift set source alias "
           "eval exec trap let");

enum { SH_NORM, SH_SQ, SH_DQ, SH_HD };

static int sh_hd_id(struct sx_ctx *cx, int dash, const char *w, size_t len)
{
	if (!cx || len == 0 || len >= SX_HD_LEN - 1)
		return -1;
	char key[SX_HD_LEN];
	key[0] = dash ? '-' : '+';
	memcpy(key + 1, w, len);
	key[len + 1] = 0;
	for (int i = 0; i < cx->nhd; i++)
		if (!strcmp(cx->hd[i], key))
			return i;
	if (cx->nhd >= SX_HD_MAX)
		return -1;
	memcpy(cx->hd[cx->nhd], key, len + 2);
	return cx->nhd++;
}

/* inside "...": ends at the closing quote; $name and ${...} inside get K_PROP */
static size_t sh_dq(const char *s, size_t i, size_t n, uint8_t *cls, int *closed)
{
	size_t j = i;
	size_t run = i;
	while (j < n) {
		if (s[j] == '\\' && j + 1 < n) {
			j += 2;
			continue;
		}
		if (s[j] == '"') {
			paint(cls, run, j + 1, K_STRING);
			*closed = 1;
			return j + 1;
		}
		if (s[j] == '$' && j + 1 < n && (isid0((unsigned char)s[j + 1]) || s[j + 1] == '{')) {
			paint(cls, run, j, K_STRING);
			size_t k = j + 1;
			if (s[k] == '{') {
				while (k < n && s[k] != '}')
					k++;
				if (k < n)
					k++;
			} else {
				while (k < n && isid((unsigned char)s[k]))
					k++;
			}
			paint(cls, j, k, K_PROP);
			j = run = k;
			continue;
		}
		j++;
	}
	paint(cls, run, n, K_STRING);
	*closed = 0;
	return n;
}

static sx_state lex_sh(struct sx_ctx *cx, sx_state st, const char *s, size_t n, uint8_t *cls)
{
	size_t i = 0;
	int mode = st & 0xff, cmdpos = 1, valword = 0, pending = -1;
	if (mode == SH_HD) {
		int id = st >> 8;
		if (cx && id < cx->nhd) {
			const char *w = cx->hd[id] + 1;
			size_t a = 0, e = n;
			if (cx->hd[id][0] == '-')
				while (a < e && s[a] == '\t')
					a++;
			while (e > a && s[e - 1] == '\r')
				e--;
			if (strlen(w) == e - a && !memcmp(s + a, w, e - a)) {
				paint(cls, 0, n, K_PREPROC);
				return SH_NORM;
			}
		} else {
			return SH_NORM;
		}
		paint(cls, 0, n, K_STRING);
		return st;
	}
	if (mode == SH_SQ) {
		const char *q = memchr(s, '\'', n);
		if (!q) {
			paint(cls, 0, n, K_STRING);
			return SH_SQ;
		}
		i = (size_t)(q - s) + 1;
		paint(cls, 0, i, K_STRING);
	} else if (mode == SH_DQ) {
		int closed;
		i = sh_dq(s, 0, n, cls, &closed);
		if (!closed)
			return SH_DQ;
	}
	while (i < n) {
		unsigned char c = (unsigned char)s[i];
		if (isws(c)) {
			i++;
			continue;
		}
		if (c == '#' && (i == 0 || strchr(" \t;|&(", s[i - 1]))) {
			paint(cls, i, n, i == 0 && i + 1 < n && s[i + 1] == '!' ? K_PREPROC : K_COMMENT);
			break;
		}
		if (c == '\\') {
			i += 2;
			continue;
		}
		if (c == '\'') {
			const char *q = i + 1 < n ? memchr(s + i + 1, '\'', n - i - 1) : NULL;
			if (!q) {
				paint(cls, i, n, K_STRING);
				return SH_SQ;
			}
			size_t j = (size_t)(q - s) + 1;
			paint(cls, i, j, K_STRING);
			i = j;
			cmdpos = 0;
			continue;
		}
		if (c == '"') {
			int closed;
			size_t j = sh_dq(s, i + 1, n, cls, &closed);
			paint(cls, i, i + 1, K_STRING);
			i = j;
			if (!closed)
				return SH_DQ;
			cmdpos = 0;
			continue;
		}
		if (c == '$') {
			size_t j = i + 1;
			if (j < n && s[j] == '(') {
				j++;
				if (j < n && s[j] == '(')
					j++;
				paint(cls, i, j, K_PUNCT);
				cmdpos = 1;
				i = j;
				continue;
			}
			if (j < n && s[j] == '{') {
				while (j < n && s[j] != '}')
					j++;
				if (j < n)
					j++;
			} else if (j < n && isid0((unsigned char)s[j])) {
				while (j < n && isid((unsigned char)s[j]))
					j++;
			} else if (j < n && (isdig((unsigned char)s[j]) || strchr("?#@*$!-", s[j]))) {
				j++;
			}
			paint(cls, i, j, K_PROP);
			i = j;
			cmdpos = 0;
			continue;
		}
		if (c == '<' && i + 1 < n && s[i + 1] == '<' && !(i + 2 < n && s[i + 2] == '<')) {
			size_t j = i + 2;
			int dash = j < n && s[j] == '-';
			if (dash)
				j++;
			j = skipws(s, j, n);
			char q = j < n && (s[j] == '\'' || s[j] == '"') ? s[j] : 0;
			if (q || (j < n && s[j] == '\\'))
				j++;
			size_t w = j;
			while (j < n && (isid((unsigned char)s[j]) || s[j] == '.' || s[j] == '-'))
				j++;
			paint(cls, i, w, K_PUNCT);
			paint(cls, w, j, K_STRING);
			if (j > w && pending < 0)
				pending = sh_hd_id(cx, dash, s + w, j - w);
			if (q && j < n && s[j] == q)
				j++;
			i = j;
			continue;
		}
		if (strchr(";|&()<>`", c)) {
			paint(cls, i, i + 1, K_PUNCT);
			if (c != '<' && c != '>' && c != ')')
				cmdpos = 1;
			i++;
			continue;
		}
		/* a word */
		size_t j = i;
		while (j < n && !strchr(" \t\r;|&()<>'\"`$\\=", s[j]) && !(s[j] == '#' && j == i))
			j++;
		if (j == i) { /* '=' or a stray byte */
			paint(cls, i, i + 1, K_PUNCT);
			i++;
			continue;
		}
		size_t len = j - i;
		if (j < n && s[j] == '=' && isid0((unsigned char)s[i]) && (cmdpos || valword)) {
			int ok = 1;
			for (size_t z = i; z < j; z++)
				if (!isid((unsigned char)s[z]))
					ok = 0;
			if (ok) {
				paint(cls, i, j, K_PROP);
				paint(cls, j, j + 1, K_PUNCT);
				i = j + 1;
				/* the value is not a command; the command may follow it */
				size_t v = i;
				if (v < n && s[v] != '"' && s[v] != '\'' && s[v] != '$' && !isws((unsigned char)s[v])) {
					while (v < n && !strchr(" \t\r;|&()<>'\"`$\\", s[v]))
						v++;
					if (isdig((unsigned char)s[i]))
						paint(cls, i, v, K_NUMBER);
					i = v;
				}
				continue;
			}
		}
		int k = K_TEXT;
		if (len == 1 && (s[i] == '{' || s[i] == '}' || s[i] == '[' || s[i] == ']' || s[i] == '!')) {
			k = K_PUNCT;
			cmdpos = s[i] == '{' || s[i] == '!';
		} else if (len == 2 && (!strncmp(s + i, "[[", 2) || !strncmp(s + i, "]]", 2))) {
			k = K_PUNCT;
			cmdpos = 0;
		} else if (kws_has(&sh_kw, s + i, len)) {
			k = K_KEYWORD;
			cmdpos = (len == 2 && (!strncmp(s + i, "if", 2) || !strncmp(s + i, "do", 2))) ||
			         (len == 4 && (!strncmp(s + i, "then", 4) || !strncmp(s + i, "else", 4) || !strncmp(s + i, "elif", 4))) ||
			         (len == 5 && (!strncmp(s + i, "while", 5) || !strncmp(s + i, "until", 5)));
			valword = (len == 5 && !strncmp(s + i, "local", 5)) || (len == 6 && !strncmp(s + i, "export", 6));
		} else if (cmdpos) {
			k = K_FUNC;
			cmdpos = 0;
		} else if (s[i] == '-' && len > 1) {
			size_t o = i + 1 + (s[i + 1] == '-' && len > 2);
			if (isid0((unsigned char)s[o]))
				k = K_TYPE;
		} else {
			int alld = 1;
			for (size_t z = i; z < j; z++)
				if (!isdig((unsigned char)s[z]))
					alld = 0;
			if (alld)
				k = K_NUMBER;
		}
		paint(cls, i, j, k);
		i = j;
	}
	if (pending >= 0)
		return (sx_state)(SH_HD | (pending << 8));
	return SH_NORM;
}

/* ---- Python ------------------------------------------------------------------------------------ */

KWS(py_kw, "and as assert async await break class continue def del elif else except finally for from global if "
           "import in is lambda nonlocal not or pass raise return try while with yield match case True False None");
KWS(py_ty, "int float str bool list dict set tuple bytes bytearray object type frozenset complex range Exception "
           "ValueError TypeError KeyError OSError self cls");

enum { PY_NORM, PY_TQ1, PY_TQ2 };

/* inside a triple quoted string from index i; q is the quote byte */
static size_t py_tq(const char *s, size_t i, size_t n, char q, int *closed)
{
	for (size_t j = i; j < n; j++) {
		if (s[j] == '\\') {
			j++;
			continue;
		}
		if (j + 3 <= n && s[j] == q && s[j + 1] == q && s[j + 2] == q) {
			*closed = 1;
			return j + 3;
		}
	}
	*closed = 0;
	return n;
}

static sx_state lex_py(sx_state st, const char *s, size_t n, uint8_t *cls)
{
	size_t i = 0;
	int expect = 0; /* 1: next name is a function, 2: a class */
	if (st == PY_TQ1 || st == PY_TQ2) {
		int closed;
		size_t j = py_tq(s, 0, n, st == PY_TQ1 ? '\'' : '"', &closed);
		paint(cls, 0, j, K_STRING);
		if (!closed)
			return st;
		i = j;
	}
	int first = 1;
	while (i < n) {
		unsigned char c = (unsigned char)s[i];
		if (isws(c)) {
			i++;
			continue;
		}
		int was_first = first;
		first = 0;
		if (c == '#') {
			paint(cls, i, n, K_COMMENT);
			break;
		}
		if (c == '@' && was_first) {
			size_t j = i + 1;
			while (j < n && (isid((unsigned char)s[j]) || s[j] == '.'))
				j++;
			paint(cls, i, j, K_PREPROC);
			i = j;
			continue;
		}
		size_t qs = i, pre = 0; /* a string, maybe with a prefix such as r or f */
		int isstr = 0;
		if (c == '"' || c == '\'') {
			isstr = 1;
		} else if (isid0(c)) {
			size_t j = i;
			while (j < n && isid((unsigned char)s[j]) && j - i < 3)
				j++;
			if (j < n && (s[j] == '"' || s[j] == '\'') && j - i <= 3) {
				int ok = 1;
				for (size_t z = i; z < j; z++)
					if (!strchr("rRbBfFuU", s[z]))
						ok = 0;
				if (ok) {
					isstr = 1;
					qs = j;
					pre = j - i;
				}
			}
		}
		if (isstr) {
			char q = s[qs];
			if (qs + 2 < n && s[qs + 1] == q && s[qs + 2] == q) {
				int closed;
				size_t j = py_tq(s, qs + 3, n, q, &closed);
				paint(cls, i, j, K_STRING);
				if (!closed)
					return q == '\'' ? PY_TQ1 : PY_TQ2;
				i = j;
			} else {
				int closed;
				size_t j = quoted(s, qs, n, &closed);
				paint(cls, i, j, K_STRING);
				i = j;
			}
			(void)pre;
			continue;
		}
		if (isdig(c) || (c == '.' && i + 1 < n && isdig((unsigned char)s[i + 1]))) {
			size_t j = number(s, i, n);
			paint(cls, i, j, K_NUMBER);
			i = j;
			continue;
		}
		if (isid0(c)) {
			size_t j = i;
			while (j < n && isid((unsigned char)s[j]))
				j++;
			size_t len = j - i;
			int k = K_TEXT;
			if (expect) {
				k = expect == 1 ? K_FUNC : K_TYPE;
				expect = 0;
			} else if (kws_has(&py_kw, s + i, len)) {
				k = K_KEYWORD;
				if (len == 3 && !strncmp(s + i, "def", 3))
					expect = 1;
				else if (len == 5 && !strncmp(s + i, "class", 5))
					expect = 2;
			} else if (kws_has(&py_ty, s + i, len)) {
				k = K_TYPE;
			} else {
				size_t q = skipws(s, j, n);
				if (q < n && s[q] == '(')
					k = K_FUNC;
			}
			paint(cls, i, j, k);
			i = j;
			continue;
		}
		if (strchr("{}()[]:,.;+-*/%=<>!&|^~@", c))
			paint(cls, i, i + 1, K_PUNCT);
		i++;
	}
	return PY_NORM;
}

/* ---- JavaScript, TypeScript and JSON ---------------------------------------------------------------- */

KWS(js_kw, "break case catch class const continue debugger default delete do else export extends finally for "
           "function if import in instanceof let new of return super switch this throw try typeof var void while "
           "with yield async await static get set from as interface type enum implements public private protected "
           "readonly abstract declare namespace module keyof true false null undefined NaN Infinity");

enum { JS_NORM, JS_BLOCK, JS_TPL };

static sx_state lex_js(sx_state st, const char *s, size_t n, uint8_t *cls);

/* a template literal body from i: ${ ... } on the same line is code */
static size_t js_tpl(const char *s, size_t i, size_t n, uint8_t *cls, int *closed)
{
	size_t j = i, run = i;
	while (j < n) {
		if (s[j] == '\\' && j + 1 < n) {
			j += 2;
			continue;
		}
		if (s[j] == '`') {
			paint(cls, run, j + 1, K_STRING);
			*closed = 1;
			return j + 1;
		}
		if (s[j] == '$' && j + 1 < n && s[j + 1] == '{') {
			size_t k = j + 2;
			int depth = 1;
			while (k < n && depth) {
				if (s[k] == '{')
					depth++;
				else if (s[k] == '}')
					depth--;
				k++;
			}
			if (depth == 0) {
				paint(cls, run, j, K_STRING);
				paint(cls, j, j + 2, K_PUNCT);
				lex_js(JS_NORM, s + j + 2, k - 1 - (j + 2), cls ? cls + j + 2 : NULL);
				paint(cls, k - 1, k, K_PUNCT);
				j = run = k;
				continue;
			}
		}
		j++;
	}
	paint(cls, run, n, K_STRING);
	*closed = 0;
	return n;
}

static sx_state lex_js(sx_state st, const char *s, size_t n, uint8_t *cls)
{
	size_t i = 0;
	int last = 0, last_kw = 0; /* the previous significant byte, and whether it ended a keyword like return */
	if (st == JS_BLOCK) {
		int closed;
		size_t j = block_end(s, 0, n, &closed);
		paint(cls, 0, j, K_COMMENT);
		if (!closed)
			return JS_BLOCK;
		i = j;
	} else if (st == JS_TPL) {
		int closed;
		size_t j = js_tpl(s, 0, n, cls, &closed);
		if (!closed)
			return JS_TPL;
		i = j;
		last = '`';
	}
	while (i < n) {
		unsigned char c = (unsigned char)s[i];
		if (isws(c)) {
			i++;
			continue;
		}
		if (c == '/' && i + 1 < n && s[i + 1] == '*') {
			int closed;
			size_t j = block_end(s, i + 2, n, &closed);
			paint(cls, i, j, K_COMMENT);
			if (!closed)
				return JS_BLOCK;
			i = j;
			continue;
		}
		if (c == '/' && i + 1 < n && s[i + 1] == '/') {
			paint(cls, i, n, K_COMMENT);
			break;
		}
		if (c == '/' && (last == 0 || last_kw || strchr("(,=:[!&|?{};+-*%<>~^", last))) {
			size_t j = i + 1;
			int cls_open = 0, ok = 0;
			while (j < n) {
				if (s[j] == '\\' && j + 1 < n) {
					j += 2;
					continue;
				}
				if (s[j] == '[')
					cls_open = 1;
				else if (s[j] == ']')
					cls_open = 0;
				else if (s[j] == '/' && !cls_open) {
					ok = 1;
					break;
				}
				j++;
			}
			if (ok) {
				j++;
				while (j < n && isid((unsigned char)s[j]))
					j++;
				paint(cls, i, j, K_PREPROC);
				i = j;
				last = ')';
				last_kw = 0;
				continue;
			}
		}
		if (c == '"' || c == '\'') {
			int closed;
			size_t j = quoted(s, i, n, &closed);
			size_t q = skipws(s, j, n);
			paint(cls, i, j, q < n && s[q] == ':' ? K_PROP : K_STRING);
			i = j;
			last = ')';
			last_kw = 0;
			continue;
		}
		if (c == '`') {
			int closed;
			size_t j = js_tpl(s, i + 1, n, cls, &closed);
			paint(cls, i, i + 1, K_STRING);
			if (!closed)
				return JS_TPL;
			i = j;
			last = '`';
			last_kw = 0;
			continue;
		}
		if (isdig(c) || (c == '.' && i + 1 < n && isdig((unsigned char)s[i + 1]))) {
			size_t j = number(s, i, n);
			paint(cls, i, j, K_NUMBER);
			i = j;
			last = ')';
			last_kw = 0;
			continue;
		}
		if (isid0(c) || c == '$') {
			size_t j = i;
			while (j < n && (isid((unsigned char)s[j]) || s[j] == '$'))
				j++;
			size_t len = j - i;
			int k = K_TEXT, kw = 0;
			int after_dot = i > 0 && s[i - 1] == '.';
			size_t q = skipws(s, j, n);
			if (!after_dot && kws_has(&js_kw, s + i, len)) {
				k = K_KEYWORD;
				kw = len == 6 ? !strncmp(s + i, "return", 6) || !strncmp(s + i, "typeof", 6) : len == 4 && !strncmp(s + i, "case", 4);
				kw = kw || (len == 2 && (!strncmp(s + i, "in", 2) || !strncmp(s + i, "of", 2)));
			} else if (q < n && s[q] == '(') {
				k = K_FUNC;
			} else if (q < n && s[q] == ':' && !after_dot && (last == '{' || last == ',')) {
				k = K_PROP;
			} else if (!after_dot && s[i] >= 'A' && s[i] <= 'Z' && len > 1) {
				k = K_TYPE;
			}
			paint(cls, i, j, k);
			i = j;
			last = ')';
			last_kw = kw;
			continue;
		}
		if (strchr("{}()[];,.:?+-*/%=<>!&|^~", c))
			paint(cls, i, i + 1, K_PUNCT);
		last = c;
		last_kw = 0;
		i++;
	}
	return JS_NORM;
}

/* ---- CSS -------------------------------------------------------------------------------------------- */

/* state: bit 0 in a comment, bit 1 in a value, bits 4-7 block depth (0 to 15) */
enum { CS_COMMENT = 1, CS_VALUE = 2 };
#define CS_DEPTH(st) (((st) >> 4) & 15)

KWS(dssl_state, "hover focus focus-visible active disabled checked empty target required invalid valid open closed "
                "placeholder-shown read-only read-write enabled indeterminate first-child last-child only-child "
                "first-of-type last-of-type link visited seen before after placeholder selection marker dark light");

/* A selector in s[a, b): .class #id tag :pseudo [attr] and combinators */
static void css_selector(const char *s, size_t a, size_t b, uint8_t *cls)
{
	size_t i = a;
	while (i < b) {
		unsigned char c = (unsigned char)s[i];
		if (c == '.' || c == '#') {
			size_t j = i + 1;
			while (j < b && (isid((unsigned char)s[j]) || s[j] == '-'))
				j++;
			paint(cls, i, j, c == '.' ? K_TYPE : K_FUNC);
			i = j;
		} else if (c == ':') {
			size_t j = i + 1;
			while (j < b && (s[j] == ':' || isid((unsigned char)s[j]) || s[j] == '-'))
				j++;
			paint(cls, i, j, K_PREPROC);
			i = j;
		} else if (c == '[') {
			size_t j = i;
			while (j < b && s[j] != ']')
				j++;
			if (j < b)
				j++;
			paint(cls, i, j, K_PROP);
			i = j;
		} else if (c == '@') {
			size_t j = i + 1;
			while (j < b && (isid((unsigned char)s[j]) || s[j] == '-'))
				j++;
			paint(cls, i, j, K_PREPROC);
			i = j;
		} else if (isid0(c)) {
			size_t j = i;
			while (j < b && (isid((unsigned char)s[j]) || s[j] == '-'))
				j++;
			paint(cls, i, j, K_KEYWORD);
			i = j;
		} else if (isdig(c)) {
			size_t j = number(s, i, b);
			paint(cls, i, j, K_NUMBER);
			i = j;
		} else {
			if (strchr("{}()>+~*,&=", c))
				paint(cls, i, i + 1, K_PUNCT);
			i++;
		}
	}
}

/* A value in s[a, b): numbers with units, colours, strings, functions, variables */
static void css_value(const char *s, size_t a, size_t b, uint8_t *cls)
{
	size_t i = a;
	while (i < b) {
		unsigned char c = (unsigned char)s[i];
		if (c == '"' || c == '\'') {
			int closed;
			size_t j = quoted(s, i, b, &closed);
			paint(cls, i, j, K_STRING);
			i = j;
		} else if (c == '#' && i + 1 < b && ishex((unsigned char)s[i + 1])) {
			size_t j = i + 1;
			while (j < b && ishex((unsigned char)s[j]))
				j++;
			paint(cls, i, j, K_NUMBER);
			i = j;
		} else if (isdig(c) || ((c == '.' || c == '-' || c == '+') && i + 1 < b && (isdig((unsigned char)s[i + 1]) || s[i + 1] == '.') && (i == a || !isid((unsigned char)s[i - 1])))) {
			size_t j = i + 1;
			while (j < b && (isdig((unsigned char)s[j]) || s[j] == '.'))
				j++;
			while (j < b && (isalp((unsigned char)s[j]) || s[j] == '%'))
				j++;
			paint(cls, i, j, K_NUMBER);
			i = j;
		} else if (c == '-' && i + 1 < b && s[i + 1] == '-') {
			size_t j = i + 2;
			while (j < b && (isid((unsigned char)s[j]) || s[j] == '-'))
				j++;
			paint(cls, i, j, K_PROP);
			i = j;
		} else if (c == '!') {
			size_t j = i + 1;
			while (j < b && isid((unsigned char)s[j]))
				j++;
			paint(cls, i, j, K_KEYWORD);
			i = j;
		} else if (isid0(c)) {
			size_t j = i;
			while (j < b && (isid((unsigned char)s[j]) || s[j] == '-'))
				j++;
			if (j < b && s[j] == '(')
				paint(cls, i, j, K_FUNC);
			i = j;
		} else {
			if (strchr("{}()/,;:", c))
				paint(cls, i, i + 1, K_PUNCT);
			i++;
		}
	}
}

/* from i on, does the line show a declaration (name: value) rather than a selector? */
static int css_is_decl(const char *s, size_t i, size_t n)
{
	size_t j = i;
	if (j < n && s[j] == '-')
		j++;
	while (j < n && (isid((unsigned char)s[j]) || s[j] == '-'))
		j++;
	size_t q = skipws(s, j, n);
	if (j == i || q >= n || s[q] != ':' || (q + 1 < n && s[q + 1] == ':'))
		return 0;
	size_t e = n;
	while (e > q && isws((unsigned char)s[e - 1]))
		e--;
	if (e > 0 && s[e - 1] == ',')
		return 0;
	for (size_t z = q + 1; z < n; z++) {
		if (s[z] == '{')
			return 0;
		if (s[z] == ';' || s[z] == '}')
			return 1;
	}
	return 1;
}

static sx_state lex_css(sx_state st, const char *s, size_t n, uint8_t *cls)
{
	size_t i = 0;
	int comment = st & CS_COMMENT, value = (st & CS_VALUE) != 0, depth = (int)CS_DEPTH(st);
	if (comment) {
		int closed;
		size_t j = block_end(s, 0, n, &closed);
		paint(cls, 0, j, K_COMMENT);
		if (!closed)
			return st;
		comment = 0;
		i = j;
	}
	size_t seg = i; /* start of the piece not yet painted as selector or value */
	while (i < n) {
		unsigned char c = (unsigned char)s[i];
		if (c == '/' && i + 1 < n && s[i + 1] == '*') {
			if (value)
				css_value(s, seg, i, cls);
			else
				css_selector(s, seg, i, cls);
			int closed;
			size_t j = block_end(s, i + 2, n, &closed);
			paint(cls, i, j, K_COMMENT);
			if (!closed)
				return (sx_state)(CS_COMMENT | (value ? CS_VALUE : 0) | depth << 4);
			i = seg = j;
			continue;
		}
		if (c == '"' || c == '\'') {
			int closed;
			i = quoted(s, i, n, &closed);
			continue;
		}
		if (c == '(' && value) { /* url(...) may hold // and other oddities */
			i++;
			continue;
		}
		if (!value && depth > 0 && !isws(c) && c != '}' && c != '{' && c != ';' && skipws(s, seg, n) == i && css_is_decl(s, i, n)) {
			size_t j = i;
			while (j < n && s[j] != ':')
				j++;
			size_t e = j;
			while (e > i && isws((unsigned char)s[e - 1]))
				e--;
			paint(cls, i, e, K_PROP);
			paint(cls, j, j + 1, K_PUNCT);
			i = seg = j + 1;
			value = 1;
			continue;
		}
		if (c == '{') {
			css_selector(s, seg, i, cls);
			paint(cls, i, i + 1, K_PUNCT);
			if (depth < 15)
				depth++;
			value = 0;
			i = seg = i + 1;
			continue;
		}
		if (c == '}') {
			if (value)
				css_value(s, seg, i, cls);
			else
				css_selector(s, seg, i, cls);
			paint(cls, i, i + 1, K_PUNCT);
			if (depth > 0)
				depth--;
			value = 0;
			i = seg = i + 1;
			continue;
		}
		if (c == ';') {
			if (value)
				css_value(s, seg, i, cls);
			else
				css_selector(s, seg, i, cls);
			paint(cls, i, i + 1, K_PUNCT);
			value = 0;
			i = seg = i + 1;
			continue;
		}
		i++;
	}
	if (value)
		css_value(s, seg, n, cls);
	else
		css_selector(s, seg, n, cls);
	return (sx_state)((value ? CS_VALUE : 0) | depth << 4);
}

/* ---- HTML and XML ------------------------------------------------------------------------------------ */

/* low byte: mode 0-2 plus flags; bits 8-15: the state of the embedded lexer */
enum { H_TEXT, H_TAG, H_COMMENT, H_SCRIPT, H_STYLE, H_DQ, H_SQ, H_MODE = 7, H_ISSCRIPT = 8, H_ISSTYLE = 16 };

static size_t ci_find(const char *s, size_t i, size_t n, const char *w)
{
	size_t wl = strlen(w);
	for (; i + wl <= n; i++) {
		size_t k = 0;
		while (k < wl && (s[i + k] | 32) == w[k])
			k++;
		if (k == wl)
			return i;
	}
	return n;
}

static sx_state lex_html(sx_state st, const char *s, size_t n, uint8_t *cls)
{
	size_t i = 0;
	int mode = st & H_MODE, flags = st & (H_ISSCRIPT | H_ISSTYLE), sub = st >> 8;
	while (i < n) {
		if (mode == H_COMMENT) {
			size_t j = i;
			int found = 0;
			while (j + 3 <= n) {
				if (!memcmp(s + j, "-->", 3)) {
					found = 1;
					break;
				}
				j++;
			}
			if (found) {
				paint(cls, i, j + 3, K_COMMENT);
				i = j + 3;
				mode = H_TEXT;
			} else {
				paint(cls, i, n, K_COMMENT);
				i = n;
			}
		} else if (mode == H_SCRIPT || mode == H_STYLE) {
			size_t j = ci_find(s, i, n, mode == H_SCRIPT ? "</script" : "</style");
			sx_state r = mode == H_SCRIPT ? lex_js((sx_state)sub, s + i, j - i, cls ? cls + i : NULL)
			                              : lex_css((sx_state)sub, s + i, j - i, cls ? cls + i : NULL);
			if (j < n) {
				mode = H_TEXT;
				sub = 0;
				i = j;
			} else {
				sub = r;
				i = n;
			}
		} else if (mode == H_TAG || mode == H_DQ || mode == H_SQ) {
			unsigned char c = (unsigned char)s[i];
			if (mode == H_DQ || mode == H_SQ) {
				char q = mode == H_DQ ? '"' : '\'';
				size_t j = i;
				while (j < n && s[j] != q)
					j++;
				if (j < n) {
					j++;
					mode = H_TAG;
				}
				paint(cls, i, j, K_STRING);
				i = j;
			} else if (isws(c)) {
				i++;
			} else if (c == '"' || c == '\'') {
				mode = c == '"' ? H_DQ : H_SQ;
				paint(cls, i, i + 1, K_STRING);
				i++;
			} else if (c == '>' || (c == '/' && i + 1 < n && s[i + 1] == '>') || (c == '?' && i + 1 < n && s[i + 1] == '>')) {
				size_t e = i + (c == '>' ? 1 : 2);
				paint(cls, i, e, K_PUNCT);
				i = e;
				if (flags & H_ISSCRIPT)
					mode = H_SCRIPT;
				else if (flags & H_ISSTYLE)
					mode = H_STYLE;
				else
					mode = H_TEXT;
				if (c == '/')
					mode = H_TEXT;
				flags = 0;
				sub = 0;
			} else if (c == '=') {
				paint(cls, i, i + 1, K_PUNCT);
				i++;
				size_t j = skipws(s, i, n);
				if (j < n && s[j] != '"' && s[j] != '\'' && s[j] != '>') {
					size_t k = j;
					while (k < n && !isws((unsigned char)s[k]) && s[k] != '>')
						k++;
					paint(cls, j, k, K_STRING);
					i = k;
				}
			} else {
				size_t j = i;
				while (j < n && !isws((unsigned char)s[j]) && !strchr("=>/\"'", s[j]))
					j++;
				if (j == i)
					j = i + 1;
				paint(cls, i, j, K_PROP);
				i = j;
			}
		} else { /* text */
			size_t j = i;
			while (j < n && s[j] != '<' && s[j] != '&')
				j++;
			if (j > i) {
				i = j;
				continue;
			}
			if (s[i] == '&') {
				size_t k = i + 1;
				while (k < n && k - i < 12 && (isid((unsigned char)s[k]) || s[k] == '#'))
					k++;
				if (k < n && s[k] == ';') {
					paint(cls, i, k + 1, K_PREPROC);
					i = k + 1;
				} else {
					i++;
				}
				continue;
			}
			/* '<' */
			if (i + 3 < n && !strncmp(s + i, "<!--", 4)) {
				paint(cls, i, i + 4, K_COMMENT);
				mode = H_COMMENT;
				i += 4;
				continue;
			}
			if (i + 1 < n && (s[i + 1] == '!' || s[i + 1] == '?')) {
				size_t k = i + 2;
				while (k < n && !isws((unsigned char)s[k]) && s[k] != '>')
					k++;
				paint(cls, i, k, K_PREPROC);
				i = k;
				mode = H_TAG;
				flags = 0;
				continue;
			}
			size_t k = i + 1;
			int closing = k < n && s[k] == '/';
			if (closing)
				k++;
			size_t nm = k;
			while (k < n && (isid((unsigned char)s[k]) || s[k] == '-' || s[k] == ':' || s[k] == '.'))
				k++;
			if (k == nm) { /* a lone < */
				i++;
				continue;
			}
			paint(cls, i, nm, K_PUNCT);
			paint(cls, nm, k, K_TAG);
			flags = 0;
			if (!closing) {
				if (k - nm == 6 && ci_find(s, nm, k, "script") == nm)
					flags = H_ISSCRIPT;
				else if (k - nm == 5 && ci_find(s, nm, k, "style") == nm)
					flags = H_ISSTYLE;
			}
			mode = H_TAG;
			i = k;
		}
	}
	return (sx_state)(mode | flags | (sub << 8));
}

/* ---- Markdown ---------------------------------------------------------------------------------------- */

enum { M_NORM, M_FENCE };

static void md_inline(const char *s, size_t a, size_t n, uint8_t *cls)
{
	size_t i = a;
	while (i < n) {
		unsigned char c = (unsigned char)s[i];
		if (c == '\\' && i + 1 < n) {
			i += 2;
		} else if (c == '`') {
			size_t j = i + 1;
			while (j < n && s[j] != '`')
				j++;
			if (j < n) {
				paint(cls, i, j + 1, K_STRING);
				i = j + 1;
			} else {
				i++;
			}
		} else if ((c == '*' || c == '_') && i + 1 < n && !isws((unsigned char)s[i + 1]) &&
		           (i == a || !isid((unsigned char)s[i - 1]) || c == '*')) {
			size_t m = 1;
			while (i + m < n && s[i + m] == (char)c && m < 3)
				m++;
			size_t j = i + m;
			while (j + m <= n && !(!memcmp(s + j, s + i, m) && j > i + m && !isws((unsigned char)s[j - 1])))
				j++;
			if (j + m <= n && j > i + m) {
				paint(cls, i, j + m, K_EMPH);
				i = j + m;
			} else {
				i += m;
			}
		} else if (c == '[') {
			size_t j = i + 1;
			while (j < n && s[j] != ']')
				j++;
			if (j + 1 < n && s[j + 1] == '(') {
				size_t k = j + 2;
				while (k < n && s[k] != ')')
					k++;
				if (k < n) {
					paint(cls, i, j + 1, K_LINK);
					paint(cls, j + 1, k + 1, K_COMMENT);
					i = k + 1;
					continue;
				}
			}
			i++;
		} else if (c == '<') {
			size_t j = i + 1;
			while (j < n && s[j] != '>')
				j++;
			if (j < n && j > i + 1 && (isid0((unsigned char)s[i + 1]) || s[i + 1] == '/' || s[i + 1] == '!')) {
				paint(cls, i, j + 1, K_TAG);
				i = j + 1;
			} else {
				i++;
			}
		} else if (c == 'h' && n - i > 8 && (!strncmp(s + i, "http://", 7) || !strncmp(s + i, "https://", 8))) {
			size_t j = i;
			while (j < n && !isws((unsigned char)s[j]) && s[j] != ')' && s[j] != '>')
				j++;
			paint(cls, i, j, K_LINK);
			i = j;
		} else {
			i++;
		}
	}
}

static sx_state lex_md(sx_state st, const char *s, size_t n, uint8_t *cls)
{
	size_t i = skipws(s, 0, n);
	if (i > 3 && st == M_NORM)
		i = 0; /* indented code or a continuation: no block syntax here */
	int fence = i + 2 < n && ((s[i] == '`' && s[i + 1] == '`' && s[i + 2] == '`') || (s[i] == '~' && s[i + 1] == '~' && s[i + 2] == '~'));
	if (st == M_FENCE) {
		if (fence) {
			paint(cls, 0, n, K_PUNCT);
			return M_NORM;
		}
		paint(cls, 0, n, K_STRING);
		return M_FENCE;
	}
	if (fence) {
		size_t j = i;
		while (j < n && s[j] == s[i])
			j++;
		paint(cls, i, j, K_PUNCT);
		paint(cls, j, n, K_TYPE);
		return M_FENCE;
	}
	if (i < n && s[i] == '#') {
		size_t j = i;
		while (j < n && s[j] == '#')
			j++;
		if (j - i <= 6 && (j == n || isws((unsigned char)s[j]))) {
			paint(cls, i, n, K_HEAD);
			return M_NORM;
		}
	}
	/* a line of === or --- (setext heading underline, rule) or *** */
	{
		size_t j = i, cnt = 0;
		char m = i < n ? s[i] : 0;
		if (m == '=' || m == '-' || m == '*' || m == '_') {
			while (j < n && (s[j] == m || isws((unsigned char)s[j]))) {
				if (s[j] == m)
					cnt++;
				j++;
			}
			if (j == n && cnt >= 3) {
				paint(cls, i, n, m == '=' ? K_HEAD : K_PUNCT);
				return M_NORM;
			}
		}
	}
	size_t a = i;
	if (a < n && s[a] == '>') {
		while (a < n && (s[a] == '>' || isws((unsigned char)s[a])))
			a++;
		paint(cls, i, a, K_PUNCT);
	} else if (a < n && (s[a] == '-' || s[a] == '*' || s[a] == '+') && a + 1 < n && isws((unsigned char)s[a + 1])) {
		paint(cls, a, a + 1, K_KEYWORD);
		a += 2;
		if (a + 2 < n && s[a] == '[' && s[a + 2] == ']') {
			paint(cls, a, a + 3, K_TYPE);
			a += 3;
		}
	} else if (a < n && isdig((unsigned char)s[a])) {
		size_t j = a;
		while (j < n && isdig((unsigned char)s[j]))
			j++;
		if (j + 1 < n && (s[j] == '.' || s[j] == ')') && isws((unsigned char)s[j + 1])) {
			paint(cls, a, j + 1, K_KEYWORD);
			a = j + 1;
		}
	} else if (a < n && s[a] == '|') {
		/* a table row: pipes are punctuation; a separator row is all punctuation */
		int sep = 1;
		for (size_t j = a; j < n; j++)
			if (!strchr("|-: \t", s[j]))
				sep = 0;
		if (sep) {
			paint(cls, a, n, K_PUNCT);
			return M_NORM;
		}
		md_inline(s, a, n, cls);
		for (size_t j = a; j < n; j++)
			if (s[j] == '|' && (!cls || cls[j] == K_TEXT))
				paint(cls, j, j + 1, K_PUNCT);
		return M_NORM;
	}
	md_inline(s, a, n, cls);
	return M_NORM;
}

/* ---- conf, ini, desktop files, yaml, toml ------------------------------------------------------------ */

KWS(conf_kw, "true false yes no on off null none enabled disabled True False");

static void conf_value(const char *s, size_t a, size_t n, uint8_t *cls)
{
	size_t i = a;
	while (i < n) {
		unsigned char c = (unsigned char)s[i];
		if (c == '"' || c == '\'') {
			int closed;
			size_t j = quoted(s, i, n, &closed);
			paint(cls, i, j, K_STRING);
			i = j;
		} else if (c == '#' && i > a && isws((unsigned char)s[i - 1])) {
			paint(cls, i, n, K_COMMENT);
			break;
		} else if (c == '$' || c == '%') {
			size_t j = i + 1;
			if (j < n && s[j] == '{') {
				while (j < n && s[j] != '}')
					j++;
				if (j < n)
					j++;
			} else {
				while (j < n && isid((unsigned char)s[j]))
					j++;
				if (j == i + 1 && c == '%' && j < n)
					j++;
			}
			paint(cls, i, j, K_PREPROC);
			i = j;
		} else if (isdig(c) && (i == a || !isid((unsigned char)s[i - 1]))) {
			size_t j = number(s, i, n);
			paint(cls, i, j, K_NUMBER);
			i = j;
		} else if (isid0(c)) {
			size_t j = i;
			while (j < n && (isid((unsigned char)s[j]) || s[j] == '-'))
				j++;
			if (kws_has(&conf_kw, s + i, j - i))
				paint(cls, i, j, K_KEYWORD);
			i = j;
		} else {
			if (c == ',' || c == '[' || c == ']' || c == '{' || c == '}')
				paint(cls, i, i + 1, K_PUNCT);
			i++;
		}
	}
}

static sx_state lex_conf(const char *s, size_t n, uint8_t *cls)
{
	size_t i = skipws(s, 0, n);
	if (i >= n)
		return 0;
	if (s[i] == '#' || s[i] == ';') {
		paint(cls, i, n, K_COMMENT);
		return 0;
	}
	if (s[i] == '[') {
		size_t j = i;
		while (j < n && s[j] != ']')
			j++;
		if (j < n)
			j++;
		paint(cls, i, j, K_SECTION);
		if (j < n && s[j] == '#')
			paint(cls, j, n, K_COMMENT);
		return 0;
	}
	size_t k = i;
	if (s[k] == '-' && k + 1 < n && isws((unsigned char)s[k + 1])) { /* yaml list item */
		paint(cls, k, k + 1, K_PUNCT);
		k = skipws(s, k + 1, n);
	}
	size_t j = k;
	if (j < n && (s[j] == '"' || s[j] == '\'')) {
		int closed;
		j = quoted(s, j, n, &closed);
	} else {
		while (j < n && !isws((unsigned char)s[j]) && s[j] != '=' && !(s[j] == ':' && (j + 1 == n || isws((unsigned char)s[j + 1]))))
			j++;
	}
	size_t e = skipws(s, j, n);
	int has_eq = e < n && (s[e] == '=' || (s[e] == ':' && (e + 1 == n || isws((unsigned char)s[e + 1]))));
	int bare = !has_eq && e < n && j > k && s[k] != '-' && !isdig((unsigned char)s[k]);
	if (has_eq) {
		paint(cls, k, j, K_PROP);
		paint(cls, e, e + 1, K_PUNCT);
		conf_value(s, e + 1, n, cls);
	} else if (bare) { /* "Key value" style files */
		paint(cls, k, j, K_PROP);
		conf_value(s, e, n, cls);
	} else {
		conf_value(s, k, n, cls);
	}
	return 0;
}

/* ---- DSSL ----------------------------------------------------------------------------------------------- */

KWS(dssl_blockkw, "tokens behavior motion raw fonts");

enum { D_NORM, D_BLOCK, D_RAW };

/* a DSSL value: numbers with units, #colours with /alpha, token paths, functions, strings, -> */
static void dssl_value(const char *s, size_t a, size_t n, uint8_t *cls)
{
	size_t i = a;
	while (i < n) {
		unsigned char c = (unsigned char)s[i];
		if (c == '"' || c == '\'') {
			int closed;
			size_t j = quoted(s, i, n, &closed);
			paint(cls, i, j, K_STRING);
			i = j;
		} else if (c == '#' && i + 1 < n && ishex((unsigned char)s[i + 1])) {
			size_t j = i + 1;
			while (j < n && ishex((unsigned char)s[j]))
				j++;
			paint(cls, i, j, K_NUMBER);
			i = j;
		} else if (c == '/' && i + 1 < n && (isdig((unsigned char)s[i + 1]) || s[i + 1] == '.')) {
			size_t j = i + 1;
			while (j < n && (isdig((unsigned char)s[j]) || s[j] == '.' || s[j] == '%'))
				j++;
			paint(cls, i, j, K_NUMBER);
			i = j;
		} else if (isdig(c) || ((c == '.' || c == '-') && i + 1 < n && (isdig((unsigned char)s[i + 1]) || s[i + 1] == '.') &&
		                         (i == a || !isid((unsigned char)s[i - 1])))) {
			size_t j = i + 1;
			while (j < n && (isdig((unsigned char)s[j]) || s[j] == '.'))
				j++;
			while (j < n && (isalp((unsigned char)s[j]) || s[j] == '%'))
				j++;
			paint(cls, i, j, K_NUMBER);
			i = j;
		} else if (c == '-' && i + 1 < n && s[i + 1] == '>') {
			paint(cls, i, i + 2, K_PUNCT);
			i += 2;
		} else if (isid0(c)) {
			size_t j = i;
			int dotted = 0;
			while (j < n && (isid((unsigned char)s[j]) || s[j] == '-' || (s[j] == '.' && j + 1 < n && isid0((unsigned char)s[j + 1])))) {
				if (s[j] == '.')
					dotted = 1;
				j++;
			}
			if (j < n && s[j] == '(')
				paint(cls, i, j, K_FUNC);
			else if (dotted)
				paint(cls, i, j, K_TYPE); /* a token path: color.ink */
			i = j;
		} else {
			if (strchr("(),", c))
				paint(cls, i, i + 1, K_PUNCT);
			i++;
		}
	}
}

/* The text of a header (before the "{"): a block word, an at-rule, a state word or a selector */
static void dssl_header(const char *s, size_t a, size_t b, uint8_t *cls)
{
	size_t i = skipws(s, a, b);
	size_t e = b;
	while (e > i && isws((unsigned char)s[e - 1]))
		e--;
	if (i >= e)
		return;
	size_t w = i;
	while (w < e && (isid((unsigned char)s[w]) || s[w] == '-' || s[w] == '@'))
		w++;
	if (s[i] == '@') {
		paint(cls, i, w, K_PREPROC);
		/* the condition: (min-width: 768), dark, 480..960 */
		for (size_t k = w; k < e; k++) {
			unsigned char c = (unsigned char)s[k];
			if (isdig(c)) {
				size_t j = number(s, k, e);
				paint(cls, k, j, K_NUMBER);
				k = j - 1;
			} else if (isid0(c)) {
				size_t j = k;
				while (j < e && (isid((unsigned char)s[j]) || s[j] == '-'))
					j++;
				paint(cls, k, j, j < e && s[skipws(s, j, e)] == ':' ? K_PROP : K_KEYWORD);
				k = j - 1;
			} else if (strchr("(),<>=.", c)) {
				paint(cls, k, k + 1, K_PUNCT);
			}
		}
		return;
	}
	if (kws_has(&dssl_blockkw, s + i, w - i)) {
		paint(cls, i, w, K_KEYWORD);
		size_t k = skipws(s, w, e), j = k;
		while (j < e && (isid((unsigned char)s[j]) || s[j] == '-'))
			j++;
		paint(cls, k, j, K_TYPE); /* the name of a behavior or motion */
		return;
	}
	if (w == e && kws_has(&dssl_state, s + i, w - i)) {
		paint(cls, i, e, K_PREPROC);
		return;
	}
	css_selector(s, i, e, cls);
}

/* the "}" that closes the "{" before from, within s[from, e), or e */
static size_t dssl_close(const char *s, size_t from, size_t e)
{
	int depth = 1;
	for (size_t k = from; k < e; k++) {
		if (s[k] == '"' || s[k] == '\'') {
			int closed;
			k = quoted(s, k, e, &closed) - 1;
		} else if (s[k] == '{') {
			depth++;
		} else if (s[k] == '}' && --depth == 0) {
			return k;
		}
	}
	return e;
}

/* Colour the code s[a, e) of one line: blocks (also one-line ones, nested), declarations, bare
 * words and "use name". *raw gets the state to continue in when a raw { block stays open. */
static void dssl_stmt(const char *s, size_t a, size_t e, uint8_t *cls, sx_state *raw)
{
	while (a < e) {
		a = skipws(s, a, e);
		if (a >= e)
			return;
		if (s[a] == '}' || s[a] == ';') {
			paint(cls, a, a + 1, K_PUNCT);
			a++;
			continue;
		}
		/* the first "{" outside strings and parentheses */
		size_t brace = e;
		int depth = 0;
		for (size_t k = a; k < e; k++) {
			unsigned char c = (unsigned char)s[k];
			if (c == '"' || c == '\'') {
				int closed;
				k = quoted(s, k, e, &closed) - 1;
			} else if (c == '(') {
				depth++;
			} else if (c == ')' && depth) {
				depth--;
			} else if (c == '{' && depth == 0) {
				brace = k;
				break;
			}
		}
		if (brace < e) {
			dssl_header(s, a, brace, cls);
			paint(cls, brace, brace + 1, K_PUNCT);
			size_t w = a, we;
			we = w;
			while (we < brace && isid((unsigned char)s[we]))
				we++;
			int is_raw = we - w == 3 && !strncmp(s + w, "raw", 3) && skipws(s, we, brace) == brace;
			size_t close = dssl_close(s, brace + 1, e);
			if (is_raw) {
				sx_state r = lex_css((sx_state)(1 << 4), s + brace + 1, close - brace - 1, cls ? cls + brace + 1 : NULL);
				if (close >= e && ((r >> 4) & 15) != 0 && raw)
					*raw = (sx_state)(D_RAW | (r << 8));
			} else {
				dssl_stmt(s, brace + 1, close, cls, raw);
			}
			if (close < e)
				paint(cls, close, close + 1, K_PUNCT);
			a = close < e ? close + 1 : e;
			continue;
		}
		/* a declaration, a bare word or "use name" up to the next ";" or the end */
		size_t end = a;
		while (end < e && s[end] != ';' && s[end] != '}')
			end++;
		size_t k = a;
		while (k < end && (isid((unsigned char)s[k]) || s[k] == '-' || s[k] == '.' || s[k] == '@'))
			k++;
		size_t kc = skipws(s, k, end);
		if (k > a && kc < end && s[kc] == ':') {
			paint(cls, a, k, isdig((unsigned char)s[a]) ? K_NUMBER : K_PROP);
			paint(cls, kc, kc + 1, K_PUNCT);
			dssl_value(s, kc + 1, end, cls);
		} else if (k - a == 3 && !strncmp(s + a, "use", 3)) {
			paint(cls, a, k, K_KEYWORD);
			size_t j = skipws(s, k, end), je = j;
			while (je < end && (isid((unsigned char)s[je]) || s[je] == '-'))
				je++;
			paint(cls, j, je, K_TYPE);
		} else if (k > a) {
			paint(cls, a, k, K_PROP);
			dssl_value(s, k, end, cls);
		} else {
			dssl_value(s, a, end, cls);
		}
		a = end;
	}
}

static sx_state lex_dssl(sx_state st, const char *s, size_t n, uint8_t *cls)
{
	size_t i = 0;
	int mode = st & 0xff, cs = st >> 8;
	if (mode == D_BLOCK) {
		int closed;
		size_t j = block_end(s, 0, n, &closed);
		paint(cls, 0, j, K_COMMENT);
		if (!closed)
			return D_BLOCK;
		i = j;
		mode = D_NORM;
	}
	if (mode == D_RAW) {
		sx_state r = lex_css((sx_state)cs, s, n, cls);
		if (((r >> 4) & 15) == 0 && !(r & CS_COMMENT))
			return D_NORM;
		return (sx_state)(D_RAW | (r << 8));
	}
	/* where the code on this line ends: at a // comment or at the start of a block comment
	 * that does not close on this line (one that does is skipped over; it is painted below) */
	size_t code_end = n;
	int block_open = 0;
	for (size_t k = i; k < n; k++) {
		unsigned char c = (unsigned char)s[k];
		if (c == '"' || c == '\'') {
			int closed;
			k = quoted(s, k, n, &closed) - 1;
		} else if (c == '/' && k + 1 < n && s[k + 1] == '/' && !(k > i && s[k - 1] == ':')) {
			code_end = k;
			break;
		} else if (c == '/' && k + 1 < n && s[k + 1] == '*') {
			int closed;
			size_t j = block_end(s, k + 2, n, &closed);
			if (!closed) {
				code_end = k;
				block_open = 1;
				break;
			}
			k = j - 1;
		}
	}
	sx_state ret = D_NORM;
	dssl_stmt(s, i, code_end, cls, &ret);
	/* comments last, over whatever was painted */
	for (size_t k = i; k < n; k++) {
		unsigned char c = (unsigned char)s[k];
		if (c == '"' || c == '\'') {
			int closed;
			k = quoted(s, k, n, &closed) - 1;
		} else if (k == code_end && !block_open) {
			paint(cls, k, n, K_COMMENT);
			break;
		} else if (c == '/' && k + 1 < n && s[k + 1] == '*') {
			int closed;
			size_t j = block_end(s, k + 2, n, &closed);
			paint(cls, k, j, K_COMMENT);
			if (!closed)
				return D_BLOCK;
			k = j - 1;
		}
	}
	return ret;
}

/* ---- dispatch and detection ------------------------------------------------------------------------------ */

sx_state sx_line(struct sx_ctx *cx, sx_lang l, sx_state in, const char *s, size_t n, uint8_t *cls)
{
	if (cls)
		memset(cls, K_TEXT, n);
	switch (l) {
	case L_C: return lex_c(in, s, n, cls);
	case L_SH: return lex_sh(cx, in, s, n, cls);
	case L_PY: return lex_py(in, s, n, cls);
	case L_JS: return lex_js(in, s, n, cls);
	case L_HTML: return lex_html(in, s, n, cls);
	case L_CSS: return lex_css(in, s, n, cls);
	case L_DSSL: return lex_dssl(in, s, n, cls);
	case L_MD: return lex_md(in, s, n, cls);
	case L_CONF: return lex_conf(s, n, cls);
	default: return 0;
	}
}

const char *sx_name(sx_lang l)
{
	static const char *const names[L_COUNT] = {"Plain text", "C", "Shell", "Python", "JavaScript", "HTML", "CSS",
	                                            "DSSL", "Markdown", "Config"};
	return (unsigned)l < L_COUNT ? names[l] : names[0];
}

static int ends_with(const char *s, const char *suf)
{
	size_t a = strlen(s), b = strlen(suf);
	if (b > a)
		return 0;
	for (size_t i = 0; i < b; i++)
		if ((s[a - b + i] | 32) != (suf[i] | 32))
			return 0;
	return 1;
}

sx_lang sx_detect(const char *path, const char *first, size_t n)
{
	static const struct {
		const char *suf;
		sx_lang l;
	} ext[] = {
		{".c", L_C}, {".h", L_C}, {".cc", L_C}, {".cpp", L_C}, {".cxx", L_C}, {".hpp", L_C}, {".hh", L_C},
		{".y", L_C}, {".l", L_C}, {".java", L_C}, {".cs", L_C},
		{".sh", L_SH}, {".bash", L_SH}, {".zsh", L_SH}, {".ksh", L_SH}, {".bats", L_SH}, {".install", L_SH},
		{".py", L_PY}, {".pyw", L_PY}, {".pyi", L_PY},
		{".js", L_JS}, {".mjs", L_JS}, {".cjs", L_JS}, {".jsx", L_JS}, {".ts", L_JS}, {".tsx", L_JS},
		{".json", L_JS}, {".jsonc", L_JS},
		{".html", L_HTML}, {".htm", L_HTML}, {".xhtml", L_HTML}, {".xml", L_HTML}, {".svg", L_HTML},
		{".ui", L_HTML}, {".xsl", L_HTML}, {".xsd", L_HTML}, {".plist", L_HTML},
		{".css", L_CSS},
		{".dssl", L_DSSL},
		{".md", L_MD}, {".markdown", L_MD}, {".mkd", L_MD},
		{".conf", L_CONF}, {".ini", L_CONF}, {".cfg", L_CONF}, {".desktop", L_CONF}, {".service", L_CONF},
		{".socket", L_CONF}, {".timer", L_CONF}, {".mount", L_CONF}, {".toml", L_CONF}, {".yml", L_CONF},
		{".yaml", L_CONF}, {".properties", L_CONF}, {".rules", L_CONF}, {".repo", L_CONF}, {".pc", L_CONF},
		{".gitignore", L_CONF}, {".editorconfig", L_CONF},
	};
	static const struct {
		const char *name;
		sx_lang l;
	} names[] = {
		{"Makefile", L_SH}, {"makefile", L_SH}, {"GNUmakefile", L_SH}, {"recipe", L_SH}, {"PKGBUILD", L_SH},
		{".bashrc", L_SH}, {".profile", L_SH}, {".bash_profile", L_SH}, {".zshrc", L_SH}, {"configure", L_SH},
		{"Dockerfile", L_SH}, {"fstab", L_CONF}, {"crontab", L_CONF}, {"hosts", L_CONF}, {"os-release", L_CONF},
		{"README", L_NONE},
	};
	if (path && *path) {
		const char *base = strrchr(path, '/');
		base = base ? base + 1 : path;
		for (size_t i = 0; i < sizeof names / sizeof *names; i++)
			if (!strcmp(base, names[i].name))
				return names[i].l;
		for (size_t i = 0; i < sizeof ext / sizeof *ext; i++)
			if (ends_with(base, ext[i].suf) && strlen(base) > strlen(ext[i].suf))
				return ext[i].l;
		if (strlen(base) > 5 && ends_with(base, ".conf.d"))
			return L_CONF;
	}
	if (first && n > 2 && first[0] == '#' && first[1] == '!') {
		char buf[120];
		size_t m = n < sizeof buf - 1 ? n : sizeof buf - 1;
		memcpy(buf, first, m);
		buf[m] = 0;
		if (strstr(buf, "python"))
			return L_PY;
		if (strstr(buf, "node") || strstr(buf, "deno") || strstr(buf, "bun"))
			return L_JS;
		if (strstr(buf, "sh") || strstr(buf, "bash") || strstr(buf, "zsh") || strstr(buf, "dash"))
			return L_SH;
	}
	if (first && n > 5 && (!strncmp(first, "<?xml", 5) || !strncmp(first, "<!DOC", 5) || !strncmp(first, "<html", 5)))
		return L_HTML;
	return L_NONE;
}
