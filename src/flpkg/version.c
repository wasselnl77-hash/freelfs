/* Version comparison and dependency constraints.
 *
 * Versions are [epoch:]version[-release]. The version and release parts are
 * compared segment by segment, rpm style: a segment is a run of digits or a
 * run of letters, and anything else separates segments. Digit runs compare
 * numerically and beat letter runs; letter runs compare as strings. When one
 * side runs out, more digits make a version newer (1.0 < 1.0.1) and more
 * letters make it older (1.0rc1 < 1.0).
 * SPDX-License-Identifier: MIT */
#include "flpkg.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

struct seg {
	const char *s;
	size_t len;
	bool digits;
};

/* Next segment of *p, or false at the end. */
static bool next_seg(const char **p, struct seg *seg)
{
	const char *s = *p;
	while (*s && !isalnum((unsigned char)*s))
		s++;
	if (!*s) {
		*p = s;
		return false;
	}
	seg->s = s;
	seg->digits = isdigit((unsigned char)*s);
	if (seg->digits)
		while (isdigit((unsigned char)*s))
			s++;
	else
		while (isalpha((unsigned char)*s))
			s++;
	seg->len = (size_t)(s - seg->s);
	*p = s;
	return true;
}

static int cmp_digits(struct seg a, struct seg b)
{
	while (a.len > 1 && *a.s == '0')
		a.s++, a.len--;
	while (b.len > 1 && *b.s == '0')
		b.s++, b.len--;
	if (a.len != b.len)
		return a.len < b.len ? -1 : 1;
	int c = memcmp(a.s, b.s, a.len);
	return c < 0 ? -1 : c > 0;
}

static int cmp_letters(struct seg a, struct seg b)
{
	size_t n = a.len < b.len ? a.len : b.len;
	int c = memcmp(a.s, b.s, n);
	if (c)
		return c < 0 ? -1 : 1;
	return a.len == b.len ? 0 : a.len < b.len ? -1 : 1;
}

static int cmp_part(const char *a, const char *b)
{
	struct seg sa, sb;
	for (;;) {
		bool ha = next_seg(&a, &sa), hb = next_seg(&b, &sb);
		if (!ha && !hb)
			return 0;
		if (!ha)
			return sb.digits ? -1 : 1;
		if (!hb)
			return sa.digits ? 1 : -1;
		if (sa.digits != sb.digits)
			return sa.digits ? 1 : -1;
		int c = sa.digits ? cmp_digits(sa, sb) : cmp_letters(sa, sb);
		if (c)
			return c;
	}
}

/* Split "[epoch:]version[-release]" (copies into buf). */
static void split(const char *v, char *buf, long *epoch, char **ver, char **rel)
{
	strcpy(buf, v);
	char *s = buf, *colon = strchr(s, ':');
	*epoch = 0;
	if (colon) {
		bool numeric = colon > s;
		for (char *c = s; c < colon; c++)
			numeric = numeric && isdigit((unsigned char)*c);
		if (numeric) {
			*epoch = strtol(s, NULL, 10);
			s = colon + 1;
		}
	}
	char *dash = strrchr(s, '-');
	*rel = NULL;
	if (dash) {
		*dash = '\0';
		*rel = dash + 1;
	}
	*ver = s;
}

int vercmp(const char *a, const char *b)
{
	if (strcmp(a, b) == 0)
		return 0;
	char *ba = xmalloc(strlen(a) + 1), *bb = xmalloc(strlen(b) + 1);
	long ea, eb;
	char *va, *vb, *ra, *rb;
	split(a, ba, &ea, &va, &ra);
	split(b, bb, &eb, &vb, &rb);
	int c = ea < eb ? -1 : ea > eb;
	if (!c)
		c = cmp_part(va, vb);
	/* A release only counts when both sides have one. */
	if (!c && ra && rb)
		c = cmp_part(ra, rb);
	free(ba);
	free(bb);
	return c;
}

/* ---- dependency constraints ----------------------------------------------- */

static const struct {
	const char *s;
	enum dep_op op;
} ops[] = {{">=", DEP_GE}, {"<=", DEP_LE}, {"=", DEP_EQ}, {">", DEP_GT}, {"<", DEP_LT}};

int dep_parse(const char *s, struct dep *d)
{
	size_t n = strcspn(s, "<>=");
	d->name = xstrndup(s, n);
	d->op = DEP_ANY;
	d->version = NULL;
	if (!n) {
		dep_free(d);
		return -1;
	}
	if (!s[n])
		return 0;
	for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
		if (has_prefix(s + n, ops[i].s)) {
			const char *v = s + n + strlen(ops[i].s);
			if (!*v)
				break;
			d->op = ops[i].op;
			d->version = xstrdup(v);
			return 0;
		}
	}
	dep_free(d);
	return -1;
}

void dep_free(struct dep *d)
{
	free(d->name);
	free(d->version);
	d->name = d->version = NULL;
}

/* "1.2" in a constraint matches any release of 1.2. */
bool dep_version_ok(const struct dep *d, const char *version)
{
	if (d->op == DEP_ANY)
		return true;
	if (!version)
		return false;
	char *cand = NULL;
	if (!strchr(d->version, '-')) {
		const char *dash = strrchr(version, '-');
		if (dash)
			version = cand = xstrndup(version, (size_t)(dash - version));
	}
	int c = vercmp(version, d->version);
	free(cand);
	switch (d->op) {
	case DEP_EQ: return c == 0;
	case DEP_GE: return c >= 0;
	case DEP_LE: return c <= 0;
	case DEP_GT: return c > 0;
	case DEP_LT: return c < 0;
	default: return true;
	}
}

bool dep_satisfied_by(const struct dep *d, const char *name, const char *version,
                      const struct strlist *provides)
{
	if (strcmp(d->name, name) == 0 && dep_version_ok(d, version))
		return true;
	for (size_t i = 0; provides && i < provides->n; i++) {
		struct dep p;
		if (dep_parse(provides->v[i], &p) < 0)
			continue;
		/* An unversioned provide only satisfies unversioned deps. */
		bool ok = strcmp(p.name, d->name) == 0 &&
		          (d->op == DEP_ANY || (p.op == DEP_EQ && dep_version_ok(d, p.version)));
		dep_free(&p);
		if (ok)
			return true;
	}
	return false;
}

char *dep_format(const struct dep *d)
{
	for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
		if (ops[i].op == d->op)
			return xasprintf("%s%s%s", d->name, ops[i].s, d->version);
	return xstrdup(d->name);
}
