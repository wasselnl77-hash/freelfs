/* libdssl: value resolution (spec chapter 3, plus token map). SPDX-License-Identifier: MIT
 * Port of resolveValue() of resolver.js 0.16.3 with its regexes written out by hand. */
#include "internal.h"
#include <stdlib.h>

/* ---------------------------------------------------------------- token map */

static uint32_t hash_sp(sp s)
{
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < s.n; i++) { h ^= (unsigned char)s.p[i]; h *= 16777619u; }
	return h;
}

void tm_init(TokMap *m) { memset(m, 0, sizeof *m); }

static void tm_reindex(Arena *a, TokMap *m)
{
	int cap = 64;
	while (cap < m->cap * 2) cap *= 2;
	m->idx = a_alloc(a, (size_t)cap * sizeof(int));
	m->idx_cap = cap;
	for (int i = 0; i < cap; i++) m->idx[i] = -1;
	for (int i = 0; i < m->n; i++) {
		uint32_t h = m->e[i].hash & (uint32_t)(cap - 1);
		while (m->idx[h] >= 0) h = (h + 1) & (uint32_t)(cap - 1);
		m->idx[h] = i;
	}
}

TokEnt *tm_find(TokMap *m, sp key)
{
	if (!m->n || !m->idx) return NULL;
	uint32_t hv = hash_sp(key);
	uint32_t h = hv & (uint32_t)(m->idx_cap - 1);
	while (m->idx[h] >= 0) {
		TokEnt *e = &m->e[m->idx[h]];
		if (e->hash == hv && sp_eq(e->key, key)) return e;
		h = (h + 1) & (uint32_t)(m->idx_cap - 1);
	}
	return NULL;
}

void tm_set(Arena *a, TokMap *m, sp key, sp value, int line, int col, int own)
{
	TokEnt *e = tm_find(m, key);
	if (e) {
		e->value = value;
		if (own) { e->line = line; e->col = col; e->own = 1; }
		return;
	}
	if (m->n == m->cap) {
		vec_grow(a, (void **)&m->e, &m->cap, sizeof *m->e);
		tm_reindex(a, m);
	}
	TokEnt ne;
	ne.key = key; ne.value = value; ne.hash = hash_sp(key); ne.line = own ? line : 0; ne.col = own ? col : 0; ne.own = own;
	m->e[m->n] = ne;
	uint32_t h = ne.hash & (uint32_t)(m->idx_cap - 1);
	while (m->idx[h] >= 0) h = (h + 1) & (uint32_t)(m->idx_cap - 1);
	m->idx[h] = m->n++;
}

void tm_copy(Arena *a, TokMap *dst, const TokMap *src)
{
	tm_init(dst);
	for (int i = 0; i < src->n; i++) tm_set(a, dst, a_dup(a, src->e[i].key), a_dup(a, src->e[i].value), src->e[i].line, src->e[i].col, src->e[i].own);
}

void tm_clone(Arena *a, TokMap *dst, const TokMap *src)
{
	tm_init(dst);
	for (int i = 0; i < src->n; i++) tm_set(a, dst, src->e[i].key, src->e[i].value, 0, 0, 0);
}

/* ---------------------------------------------------------------- diagnostics */

void diag_push(Arena *a, DiagVec *v, int line, int col, sp msg, const char *hint, sp hint_sp, int has_hint)
{
	Diag d;
	d.line = line; d.col = col; d.msg = msg;
	if (hint) { d.hint = mk(hint, strlen(hint)); d.has_hint = 1; }
	else { d.hint = hint_sp; d.has_hint = has_hint; }
	vpush(a, v, d);
}

static void verr_sp(VC *vc, sp msg, const char *hint, sp hint_sp, int has_hint)
{
	diag_push(vc->r->A, vc->errors, vc->line, vc->col, msg, hint, hint_sp, has_hint);
}

/* ---------------------------------------------------------------- Number() */

int js_number(sp s, double *out)
{
	s = sp_trim(s);
	if (!s.n) { *out = 0; return 1; }
	size_t i = 0;
	int neg = 0;
	if (s.n > 2 && s.p[0] == '0' && (s.p[1] == 'x' || s.p[1] == 'X' || s.p[1] == 'o' || s.p[1] == 'O' || s.p[1] == 'b' || s.p[1] == 'B')) {
		int base = (s.p[1] | 32) == 'x' ? 16 : (s.p[1] | 32) == 'o' ? 8 : 2;
		double v = 0;
		for (i = 2; i < s.n; i++) {
			int c = lower((unsigned char)s.p[i]);
			int d = is_digit(c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 99;
			if (d >= base) return 0;
			v = v * base + d;
		}
		*out = v;
		return 1;
	}
	if (s.p[i] == '+' || s.p[i] == '-') { neg = s.p[i] == '-'; i++; }
	if (s.n - i == 8 && memcmp(s.p + i, "Infinity", 8) == 0) { *out = neg ? -1.0 / 0.0 : 1.0 / 0.0; return 1; }
	size_t st = i, nd = 0;
	while (i < s.n && is_digit((unsigned char)s.p[i])) { i++; nd++; }
	if (i < s.n && s.p[i] == '.') { i++; while (i < s.n && is_digit((unsigned char)s.p[i])) { i++; nd++; } }
	if (!nd) return 0;
	if (i < s.n && (s.p[i] == 'e' || s.p[i] == 'E')) {
		size_t j = i + 1;
		if (j < s.n && (s.p[j] == '+' || s.p[j] == '-')) j++;
		size_t es = j;
		while (j < s.n && is_digit((unsigned char)s.p[j])) j++;
		if (j == es) return 0;
		i = j;
	}
	if (i != s.n) return 0;
	*out = js_parse_float(sub(s, 0, s.n));
	(void)st;
	return 1;
}

/* ---------------------------------------------------------------- value scan */

/* length of an identifier at raw[i] (IDENT_RE), 0 when there is none */
static size_t ident_len(sp raw, size_t i)
{
	size_t n = raw.n, k = i;
	int c0 = (unsigned char)raw.p[i];
	if (is_alpha(c0) || c0 == '_') k = i + 1;
	else if (c0 == '-') {
		int c1 = i + 1 < n ? (unsigned char)raw.p[i + 1] : 0;
		int c2 = i + 2 < n ? (unsigned char)raw.p[i + 2] : 0;
		int c3 = i + 3 < n ? (unsigned char)raw.p[i + 3] : 0;
		if (is_alpha(c1) || c1 == '_') k = i + 2;
		else if (c1 == '-' && c2 == '-' && (is_alpha(c3) || c3 == '_')) k = i + 4;
		else if (c1 == '-' && (is_alpha(c2) || c2 == '_')) k = i + 3;
		else return 0;
	} else return 0;
	while (k < n) {
		int c = (unsigned char)raw.p[k];
		if (is_alpha(c) || is_digit(c) || c == '_' || c == '.' || c == '\\' || c == '-') k++; else break;
	}
	return k - i;
}

/* ^\s*\/\s*([0-9.]+)\s*%?   returns the consumed length (0: no match); *num is the number text */
static size_t alpha_suffix(sp rest, sp *num)
{
	size_t i = 0, n = rest.n;
	while (i < n && is_ws((unsigned char)rest.p[i])) i++;
	if (i >= n || rest.p[i] != '/') return 0;
	i++;
	while (i < n && is_ws((unsigned char)rest.p[i])) i++;
	size_t st = i;
	while (i < n && (is_digit((unsigned char)rest.p[i]) || rest.p[i] == '.')) i++;
	if (i == st) return 0;
	*num = mk(rest.p + st, i - st);
	while (i < n && is_ws((unsigned char)rest.p[i])) i++;
	if (i < n && rest.p[i] == '%') i++;
	return i;
}

static int is_time_prop(sp prop) { return vset_has(&V_TIME_PROP, prop.p, prop.n); }

#define MAX_VALUE_DEPTH 64

sp resolve_value(VC *vc0, sp raw)
{
	VC vc = *vc0;
	R *r = vc.r;
	Arena *A = r->A;
	SB out;
	sb_init(&out, A);
	size_t i = 0, n = raw.n;
	const char *s = raw.p;

	while (i < n) {
		char c = s[i];

		if (is_ws((unsigned char)c)) { sb_putc(&out, c); i++; continue; }

		if (c == '"' || c == '\'') {
			size_t j = i + 1;
			while (j < n && s[j] != c) j += s[j] == '\\' ? 2 : 1;
			size_t end = j + 1 > n ? n : j + 1;
			sb_put(&out, s + i, end - i);
			i = j + 1;
			continue;
		}

		if (is_digit((unsigned char)c) || (c == '.' && i + 1 < n && is_digit((unsigned char)s[i + 1]))
		    || ((c == '-' || c == '+') && i + 1 < n && (is_digit((unsigned char)s[i + 1]) || s[i + 1] == '.'))) {
			size_t j = i + 1;
			while (j < n && (is_digit((unsigned char)s[j]) || s[j] == '.')) j++;
			sb_put(&out, s + i, j - i);
			if (j < n && (is_alpha((unsigned char)s[j]) || s[j] == '%')) {
				size_t k = j;
				while (k < n && (is_alpha((unsigned char)s[k]) || s[k] == '%')) { sb_putc(&out, (char)lower((unsigned char)s[k])); k++; }
				j = k;
			} else if (!vc.unitless) sb_puts(&out, is_time_prop(vc.prop) ? "ms" : "px");
			i = j;
			continue;
		}

		size_t il = ident_len(raw, i);
		if (il) {
			sp ident = mk(s + i, il);
			i += il;

			if (il >= 2 && ident.p[0] == '-' && ident.p[1] == '-') { sb_putsp(&out, ident); continue; }

			if (i < n && s[i] == '(') {
				int depth = 1;
				size_t j = i + 1;
				while (j < n && depth > 0) {
					if (s[j] == '(') depth++;
					if (s[j] == ')') depth--;
					if (depth > 0 && (s[j] == '"' || s[j] == '\'')) {
						char q = s[j];
						j++;
						while (j < n && s[j] != q) j += s[j] == '\\' ? 2 : 1;
					}
					j++;
				}
				size_t iend = j - 1 > n ? n : j - 1;
				sp inner = iend > i + 1 ? mk(s + i + 1, iend - (i + 1)) : mk("", 0);
				sp fname = sp_lower(A, ident);
				if (sp_eqs(fname, "url")) {
					sb_puts(&out, "url(");
					sb_putsp(&out, sp_trim(inner));
					sb_putc(&out, ')');
					i = j;
					continue;
				}
				if (sp_eqs(fname, "repeat")) {
					size_t k = 0;
					while (k < inner.n && is_ws((unsigned char)inner.p[k])) k++;
					size_t ds = k;
					while (k < inner.n && is_digit((unsigned char)inner.p[k])) k++;
					if (k > ds) {
						if (k + 1 < inner.n && inner.p[k] == '.' && is_digit((unsigned char)inner.p[k + 1])) {
							k++;
							while (k < inner.n && is_digit((unsigned char)inner.p[k])) k++;
						}
						size_t de = k;
						while (k < inner.n && is_ws((unsigned char)inner.p[k])) k++;
						if (k < inner.n && inner.p[k] == ',') {
							sb_putsp(&out, ident);
							sb_putc(&out, '(');
							sb_put(&out, inner.p + ds, de - ds);
							sb_putc(&out, ',');
							VC v2 = vc;
							v2.depth++;
							if (v2.depth > MAX_VALUE_DEPTH) sb_put(&out, inner.p + k + 1, inner.n - k - 1);
							else sb_putsp(&out, resolve_value(&v2, sub(inner, k + 1, inner.n)));
							sb_putc(&out, ')');
							i = j;
							continue;
						}
					}
				}
				int inner_unitless = vset_has(&V_UNITLESS_FN, fname.p, fname.n) ? 1 : vset_has(&V_LENGTH_FN, fname.p, fname.n) ? 0 : vc.unitless;
				int name_fn = sp_eqs(fname, "counter") || sp_eqs(fname, "counters") || sp_eqs(fname, "attr") || sp_eqs(fname, "env")
				    || sp_eqs(fname, "format") || sp_eqs(fname, "local") || sp_eqs(fname, "paint") || sp_eqs(fname, "element");
				sb_putsp(&out, ident);
				sb_putc(&out, '(');
				VC v2 = vc;
				v2.unitless = inner_unitless;
				v2.bare = name_fn ? 1 : vc.bare;
				v2.depth++;
				if (v2.depth > MAX_VALUE_DEPTH) sb_putsp(&out, inner);   /* absurd nesting: copied, not resolved */
				else sb_putsp(&out, resolve_value(&v2, inner));
				sb_putc(&out, ')');
				i = j;
				continue;
			}

			TokEnt *te = tm_find(&r->tokens, ident);
			if (te) {
				SB v;
				sb_init(&v, A);
				sb_puts(&v, "var(--dssl-");
				for (size_t k = 0; k < te->key.n; k++) sb_putc(&v, te->key.p[k] == '.' ? '-' : te->key.p[k]);
				sb_putc(&v, ')');
				sp varref = sb_done(&v);
				sp num;
				size_t al = alpha_suffix(mk(s + i, n - i), &num);
				if (al) {
					sb_puts(&out, "color-mix(in oklab, ");
					sb_putsp(&out, varref);
					sb_putc(&out, ' ');
					sb_putsp(&out, num);
					sb_puts(&out, "%, transparent)");
					i += al;
					continue;
				}
				sb_putsp(&out, varref);
				continue;
			}

			sp low = sp_lower(A, ident);
			if (vset_has(&V_KEYWORD, ident.p, ident.n) || vset_has(&V_KEYWORD, low.p, low.n)) {
				if (vset_has(&V_NAMED_COLOR, low.p, low.n) || sp_eqs(low, "currentcolor")) {
					sp num;
					size_t al = alpha_suffix(mk(s + i, n - i), &num);
					if (al) {
						sb_puts(&out, "color-mix(in oklab, ");
						sb_putsp(&out, ident);
						sb_putc(&out, ' ');
						sb_putsp(&out, num);
						sb_puts(&out, "%, transparent)");
						i += al;
						continue;
					}
				}
				sb_putsp(&out, ident);
				continue;
			}

			if (vc.bare && is_ident_plain(ident)) { sb_putsp(&out, ident); continue; }

			{
				Dym d;
				dym_init(&d, A, ident);
				for (int k = 0; k < r->tokens.n; k++) dym_sp(&d, r->tokens.e[k].key);
				dym_set(&d, &V_KEYWORD);
				sp h;
				int has = dym_done(&d, &h);
				verr_sp(&vc, a_fmt(A, "unknown value \"%.*s\" for property \"%.*s\"", PF(ident), PF(vc.prop)), NULL, has ? h : mk(NULL, 0), has);
			}
			sb_putsp(&out, ident);
			continue;
		}

		if (c == '#') {
			size_t j = i + 1;
			while (j < n && is_hex((unsigned char)s[j])) j++;
			sp hex = mk(s + i, j - i);
			i = j;
			sp num;
			size_t al = alpha_suffix(mk(s + i, n - i), &num);
			if (al) {
				sb_puts(&out, "color-mix(in oklab, ");
				sb_putsp(&out, hex);
				sb_putc(&out, ' ');
				sb_putsp(&out, num);
				sb_puts(&out, "%, transparent)");
				i += al;
			} else sb_putsp(&out, hex);
			continue;
		}
		sb_putc(&out, c);
		i++;
	}
	return sb_done(&out);
}
