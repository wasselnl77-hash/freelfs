/* libdssl: arena, strings, numbers, did-you-mean. SPDX-License-Identifier: MIT */
#include "internal.h"
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ arena */

struct Blk {
	Blk *next;
	size_t cap, used;
	char data[];
};

#define BLK_MIN (64 * 1024)
#define ALIGN 16

void arena_init(Arena *a, size_t limit)
{
	a->head = NULL;
	a->total = 0;
	a->limit = limit;
	a->jb = NULL;
}

void arena_free(Arena *a)
{
	Blk *b = a->head;
	while (b) {
		Blk *n = b->next;
		free(b);
		b = n;
	}
	a->head = NULL;
	a->total = 0;
}

static void oom(Arena *a)
{
	if (a->jb) longjmp(*a->jb, 1);
	abort();   /* no recovery point: a programming error in the library, not input-dependent */
}

void *a_alloc(Arena *a, size_t n)
{
	if (n == 0) n = 1;
	n = (n + ALIGN - 1) & ~(size_t)(ALIGN - 1);
	if (n > (size_t)1 << 40) oom(a);
	Blk *b = a->head;
	if (!b || b->cap - b->used < n) {
		size_t cap = n > BLK_MIN ? n : BLK_MIN;
		if (a->total + cap > a->limit) oom(a);
		Blk *nb = malloc(sizeof(Blk) + cap);
		if (!nb) oom(a);
		nb->cap = cap;
		nb->used = 0;
		a->total += cap;
		/* a big dedicated block goes behind the head so the head keeps its free space */
		if (n > BLK_MIN && b) {
			nb->next = b->next;
			b->next = nb;
			nb->used = n;
			return nb->data;
		}
		nb->next = a->head;
		a->head = nb;
		b = nb;
	}
	void *p = b->data + b->used;
	b->used += n;
	return p;
}

void *a_zalloc(Arena *a, size_t n)
{
	void *p = a_alloc(a, n);
	memset(p, 0, n);
	return p;
}

sp a_dup(Arena *a, sp s)
{
	char *p = a_alloc(a, s.n + 1);
	if (s.n) memcpy(p, s.p, s.n);
	p[s.n] = 0;
	return mk(p, s.n);
}

sp a_dups(Arena *a, const char *z) { return a_dup(a, mk(z, strlen(z))); }

sp a_fmt(Arena *a, const char *fmt, ...)
{
	va_list ap, ap2;
	va_start(ap, fmt);
	va_copy(ap2, ap);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0) n = 0;
	char *p = a_alloc(a, (size_t)n + 1);
	vsnprintf(p, (size_t)n + 1, fmt, ap2);
	va_end(ap2);
	return mk(p, (size_t)n);
}

sp a_cat2(Arena *a, sp x, sp y)
{
	char *p = a_alloc(a, x.n + y.n + 1);
	if (x.n) memcpy(p, x.p, x.n);
	if (y.n) memcpy(p + x.n, y.p, y.n);
	p[x.n + y.n] = 0;
	return mk(p, x.n + y.n);
}

sp a_cat3(Arena *a, sp x, sp y, sp z)
{
	char *p = a_alloc(a, x.n + y.n + z.n + 1);
	if (x.n) memcpy(p, x.p, x.n);
	if (y.n) memcpy(p + x.n, y.p, y.n);
	if (z.n) memcpy(p + x.n + y.n, z.p, z.n);
	p[x.n + y.n + z.n] = 0;
	return mk(p, x.n + y.n + z.n);
}

void vec_grow(Arena *a, void **v, int *cap, size_t sz)
{
	int nc = *cap ? *cap * 2 : 8;
	if ((size_t)nc > (size_t)1 << 28) oom(a);
	void *nv = a_alloc(a, (size_t)nc * sz);
	if (*v) memcpy(nv, *v, (size_t)*cap * sz);
	*v = nv;
	*cap = nc;
}

/* ------------------------------------------------------------------ string builder */

void sb_init(SB *b, Arena *a)
{
	b->a = a;
	b->p = NULL;
	b->n = b->cap = 0;
}

void sb_grow(SB *b, size_t need)
{
	size_t nc = b->cap ? b->cap : 64;
	while (nc < b->n + need + 1) nc *= 2;
	Arena *a = b->a;
	Blk *h = a->head;
	/* the buffer is the last thing in the newest block: grow it in place */
	if (b->p && h && b->p + ((b->cap + ALIGN - 1) & ~(size_t)(ALIGN - 1)) == h->data + h->used
	    && b->p >= h->data) {
		size_t oldr = (b->cap + ALIGN - 1) & ~(size_t)(ALIGN - 1);
		size_t newr = (nc + ALIGN - 1) & ~(size_t)(ALIGN - 1);
		if (h->cap - (size_t)(b->p - h->data) >= newr) {
			h->used += newr - oldr;
			b->cap = nc;
			return;
		}
	}
	char *np = a_alloc(a, nc);
	if (b->n) memcpy(np, b->p, b->n);
	b->p = np;
	b->cap = nc;
}

void sb_put(SB *b, const char *p, size_t n)
{
	if (!n) return;
	if (b->n + n + 1 >= b->cap) sb_grow(b, n);
	memcpy(b->p + b->n, p, n);
	b->n += n;
}

void sb_fmt(SB *b, const char *fmt, ...)
{
	va_list ap, ap2;
	va_start(ap, fmt);
	va_copy(ap2, ap);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0) n = 0;
	if (b->n + (size_t)n + 1 >= b->cap) sb_grow(b, (size_t)n + 1);
	vsnprintf(b->p + b->n, (size_t)n + 1, fmt, ap2);
	va_end(ap2);
	b->n += (size_t)n;
}

sp sb_done(SB *b)
{
	if (!b->p) sb_grow(b, 1);
	b->p[b->n] = 0;
	return mk(b->p, b->n);
}

/* ------------------------------------------------------------------ JS numbers */

void sb_num(SB *b, double v)
{
	if (isnan(v)) { sb_puts(b, "NaN"); return; }
	if (isinf(v)) { sb_puts(b, v < 0 ? "-Infinity" : "Infinity"); return; }
	if (v == 0) { sb_putc(b, '0'); return; }
	char buf[48];
	int p;
	for (p = 1; p <= 17; p++) {
		snprintf(buf, sizeof buf, "%.*e", p - 1, v);
		if (strtod(buf, NULL) == v) break;
	}
	if (p > 17) snprintf(buf, sizeof buf, "%.16e", v);
	/* buf: [-]d[.ddd]e[+-]xx */
	char dig[32];
	int k = 0, neg = 0, e = 0;
	const char *q = buf;
	if (*q == '-') { neg = 1; q++; }
	for (; *q && *q != 'e'; q++) if (*q != '.') dig[k++] = *q;
	if (*q == 'e') e = atoi(q + 1);
	while (k > 1 && dig[k - 1] == '0') k--;
	int n = e + 1;
	if (neg) sb_putc(b, '-');
	if (k <= n && n <= 21) {
		sb_put(b, dig, (size_t)k);
		for (int i = 0; i < n - k; i++) sb_putc(b, '0');
	} else if (0 < n && n <= 21) {
		sb_put(b, dig, (size_t)n);
		sb_putc(b, '.');
		sb_put(b, dig + n, (size_t)(k - n));
	} else if (-6 < n && n <= 0) {
		sb_puts(b, "0.");
		for (int i = 0; i < -n; i++) sb_putc(b, '0');
		sb_put(b, dig, (size_t)k);
	} else {
		int ee = n - 1;
		sb_putc(b, dig[0]);
		if (k > 1) { sb_putc(b, '.'); sb_put(b, dig + 1, (size_t)(k - 1)); }
		sb_fmt(b, "e%c%d", ee < 0 ? '-' : '+', ee < 0 ? -ee : ee);
	}
}

double js_parse_float(sp s)
{
	size_t i = 0, n = s.n;
	while (i < n && is_ws(s.p[i])) i++;
	size_t st = i;
	if (i < n && (s.p[i] == '+' || s.p[i] == '-')) i++;
	size_t ds = i;
	while (i < n && is_digit(s.p[i])) i++;
	size_t nd = i - ds;
	if (i < n && s.p[i] == '.') {
		size_t j = i + 1;
		while (j < n && is_digit(s.p[j])) j++;
		nd += j - i - 1;
		if (nd) i = j;
	}
	if (!nd) {
		if (n - ds >= 8 && memcmp(s.p + ds, "Infinity", 8) == 0) return s.p[st] == '-' ? -INFINITY : INFINITY;
		return NAN;
	}
	if (i < n && (s.p[i] == 'e' || s.p[i] == 'E')) {
		size_t j = i + 1;
		if (j < n && (s.p[j] == '+' || s.p[j] == '-')) j++;
		size_t es = j;
		while (j < n && is_digit(s.p[j])) j++;
		if (j > es) i = j;
	}
	char buf[80];
	size_t len = i - st;
	if (len >= sizeof buf) len = sizeof buf - 1;   /* absurdly long digit runs lose precision, not safety */
	memcpy(buf, s.p + st, len);
	buf[len] = 0;
	return strtod(buf, NULL);
}

sp js_tofixed3(Arena *a, double v)
{
	SB b;
	sb_init(&b, a);
	if (isnan(v) || isinf(v) || fabs(v) >= 1e21) { sb_num(&b, v); return sb_done(&b); }
	double m = fabs(v);
	char big[160];
	snprintf(big, sizeof big, "%.60f", m);
	char *dot = strchr(big, '.');
	if (dot && dot[4] == '5') {
		int tie = 1;
		for (char *c = dot + 5; *c; c++) if (*c != '0') { tie = 0; break; }
		if (tie) m = nextafter(m, INFINITY);   /* toFixed rounds an exact half up */
	}
	char buf[64];
	snprintf(buf, sizeof buf, "%.3f", m);
	double r = strtod(buf, NULL);
	sb_num(&b, v < 0 ? -r : r);
	return sb_done(&b);
}

/* ------------------------------------------------------------------ strings */

sp sp_ltrim(sp s) { while (s.n && is_ws((unsigned char)s.p[0])) { s.p++; s.n--; } return s; }
sp sp_rtrim(sp s) { while (s.n && is_ws((unsigned char)s.p[s.n - 1])) s.n--; return s; }
sp sp_trim(sp s) { return sp_rtrim(sp_ltrim(s)); }

sp sp_lower(Arena *a, sp s)
{
	char *p = a_alloc(a, s.n + 1);
	for (size_t i = 0; i < s.n; i++) p[i] = (char)lower((unsigned char)s.p[i]);
	p[s.n] = 0;
	return mk(p, s.n);
}

int sp_ieq(sp a, const char *z)
{
	size_t n = strlen(z);
	if (a.n != n) return 0;
	for (size_t i = 0; i < n; i++) if (lower((unsigned char)a.p[i]) != lower((unsigned char)z[i])) return 0;
	return 1;
}

int sp_istarts(sp a, const char *z)
{
	size_t n = strlen(z);
	if (a.n < n) return 0;
	for (size_t i = 0; i < n; i++) if (lower((unsigned char)a.p[i]) != lower((unsigned char)z[i])) return 0;
	return 1;
}

long sp_find(sp hay, const char *z)
{
	size_t n = strlen(z);
	if (!n) return 0;
	if (hay.n < n) return -1;
	for (size_t i = 0; i + n <= hay.n; i++)
		if (hay.p[i] == z[0] && memcmp(hay.p + i, z, n) == 0) return (long)i;
	return -1;
}
int sp_has(sp hay, const char *z) { return sp_find(hay, z) >= 0; }
long sp_findc(sp hay, char c)
{
	for (size_t i = 0; i < hay.n; i++) if (hay.p[i] == c) return (long)i;
	return -1;
}

void split_ws(Arena *a, sp s, int keep_empty, SpVec *out)
{
	size_t i = 0, n = s.n;
	if (keep_empty) {
		/* s.split(/\s+/): a leading or trailing run gives an empty piece */
		size_t st = 0;
		while (i < n) {
			if (is_ws((unsigned char)s.p[i])) {
				vpush(a, out, mk(s.p + st, i - st));
				while (i < n && is_ws((unsigned char)s.p[i])) i++;
				st = i;
			} else i++;
		}
		vpush(a, out, mk(s.p + st, n - st));
		return;
	}
	while (i < n) {
		while (i < n && is_ws((unsigned char)s.p[i])) i++;
		size_t st = i;
		while (i < n && !is_ws((unsigned char)s.p[i])) i++;
		if (i > st) vpush(a, out, mk(s.p + st, i - st));
	}
}

void split_char(Arena *a, sp s, char c, SpVec *out)
{
	size_t st = 0;
	for (size_t i = 0; i < s.n; i++)
		if (s.p[i] == c) { vpush(a, out, mk(s.p + st, i - st)); st = i + 1; }
	vpush(a, out, mk(s.p + st, s.n - st));
}

sp utf16_prefix(sp s, size_t units)
{
	size_t i = 0, u = 0;
	while (i < s.n) {
		unsigned char c = (unsigned char)s.p[i];
		size_t len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
		size_t w = len == 4 ? 2 : 1;
		if (u + w > units || i + len > s.n) break;
		i += len;
		u += w;
	}
	return mk(s.p, i);
}

int is_ident_plain(sp s)
{
	if (!s.n || !is_alpha((unsigned char)s.p[0])) return 0;
	for (size_t i = 1; i < s.n; i++) {
		int c = (unsigned char)s.p[i];
		if (!(is_word(c) || c == '-')) return 0;
	}
	return 1;
}

int is_int_str(sp s)
{
	if (!s.n) return 0;
	for (size_t i = 0; i < s.n; i++) if (!is_digit((unsigned char)s.p[i])) return 0;
	return 1;
}

int is_number_str(sp s, int allow_sign)
{
	size_t i = 0;
	if (allow_sign && i < s.n && s.p[i] == '-') i++;
	size_t d = i;
	while (i < s.n && is_digit((unsigned char)s.p[i])) i++;
	if (i == d) return 0;
	if (i < s.n && s.p[i] == '.') {
		i++;
		size_t f = i;
		while (i < s.n && is_digit((unsigned char)s.p[i])) i++;
		if (i == f) return 0;
	}
	return i == s.n;
}

/* ------------------------------------------------------------------ UTF-8 */

size_t utf8_sanitize(const char *src, size_t n, char *dst)
{
	size_t i = 0, o = 0;
	while (i < n) {
		unsigned char c = (unsigned char)src[i];
		size_t len = 0;
		if (c < 0x80) len = 1;
		else if (c >= 0xC2 && c <= 0xDF) len = 2;
		else if (c >= 0xE0 && c <= 0xEF) len = 3;
		else if (c >= 0xF0 && c <= 0xF4) len = 4;
		int ok = len != 0 && i + len <= n;
		if (ok && len > 1) {
			for (size_t k = 1; k < len; k++) if (((unsigned char)src[i + k] & 0xC0) != 0x80) { ok = 0; break; }
			if (ok && len == 3) {
				unsigned char c1 = (unsigned char)src[i + 1];
				if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 >= 0xA0)) ok = 0;
			}
			if (ok && len == 4) {
				unsigned char c1 = (unsigned char)src[i + 1];
				if ((c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 >= 0x90)) ok = 0;
			}
		}
		if (ok) {
			memcpy(dst + o, src + i, len);
			o += len;
			i += len;
		} else {
			dst[o++] = (char)0xEF; dst[o++] = (char)0xBF; dst[o++] = (char)0xBD;
			i++;
		}
	}
	return o;
}

/* ------------------------------------------------------------------ did-you-mean (DIA-8) */

void dym_init(Dym *d, Arena *a, sp word)
{
	d->a = a;
	d->wn = word.n;
	d->lw = a_alloc(a, word.n + 1);
	for (size_t i = 0; i < word.n; i++) d->lw[i] = (char)lower((unsigned char)word.p[i]);
	d->tol = (int)(word.n / 3);
	if (d->tol < 2) d->tol = 2;
	d->best = d->tol + 1;     /* only a distance up to the tolerance can win */
	d->best_s = mk(NULL, 0);
	d->found = 0;
	d->rows = NULL;
}

void dym_one(Dym *d, const char *p, size_t n)
{
	if (!d->wn || !n) return;
	size_t diff = n > d->wn ? n - d->wn : d->wn - n;
	if ((int)(diff > 1000000 ? 1000000 : diff) >= d->best) return;   /* the distance is at least the length difference */
	if (!d->rows) d->rows = a_alloc(d->a, 2 * (d->wn + 1) * sizeof(int));
	int *prev = d->rows, *cur = d->rows + d->wn + 1;
	/* Levenshtein over the lower-cased candidate; rows run along the candidate (n is small) */
	size_t m = d->wn;
	for (size_t j = 0; j <= m; j++) prev[j] = (int)j;
	for (size_t i = 1; i <= n; i++) {
		int ci = lower((unsigned char)p[i - 1]);
		cur[0] = (int)i;
		for (size_t j = 1; j <= m; j++) {
			int cost = ci == (unsigned char)d->lw[j - 1] ? 0 : 1;
			int v = prev[j] + 1;
			if (cur[j - 1] + 1 < v) v = cur[j - 1] + 1;
			if (prev[j - 1] + cost < v) v = prev[j - 1] + cost;
			cur[j] = v;
		}
		int *t = prev; prev = cur; cur = t;
	}
	int dist = prev[m];
	if (dist < d->best) {
		d->best = dist;
		d->best_s = mk(p, n);
		d->found = 1;
	}
}

void dym_set(Dym *d, const VSet *s)
{
	for (unsigned i = 0; i < s->n; i++) dym_one(d, s->words[i], strlen(s->words[i]));
}
void dym_list(Dym *d, const char *const *list, int n)
{
	for (int i = 0; i < n; i++) dym_one(d, list[i], strlen(list[i]));
}
void dym_sp(Dym *d, sp s) { dym_one(d, s.p, s.n); }

int dym_done(Dym *d, sp *out)
{
	if (!d->found || d->best > d->tol) return 0;
	*out = a_dup(d->a, d->best_s);
	return 1;
}
