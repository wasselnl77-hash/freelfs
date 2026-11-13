/* .PKGINFO (package metadata) and .FILES (the manifest).
 * SPDX-License-Identifier: MIT */
#include "flpkg.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

void pkginfo_free(struct pkginfo *p)
{
	free(p->name);
	free(p->base);
	free(p->version);
	free(p->summary);
	free(p->url);
	free(p->arch);
	free(p->filename);
	free(p->sha256);
	sl_free(&p->license);
	sl_free(&p->depend);
	sl_free(&p->provide);
	sl_free(&p->conflict);
	sl_free(&p->replace);
	sl_free(&p->group);
	sl_free(&p->sysuser);
	sl_free(&p->sysgroup);
	memset(p, 0, sizeof(*p));
}

static bool parse_u64(const char *s, uint64_t *out)
{
	char *end;
	errno = 0;
	unsigned long long v = strtoull(s, &end, 10);
	if (errno || end == s || *end || *s == '-')
		return false;
	*out = v;
	return true;
}

static void set_once(char **field, const char *v)
{
	free(*field);
	*field = xstrdup(v);
}

int pkginfo_parse(const char *text, struct pkginfo *p, char **error)
{
	char *copy = xstrdup(text), *save = NULL;
	int lineno = 0;
	memset(p, 0, sizeof(*p));
	for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		lineno++;
		char *s = trim(line);
		if (!*s || *s == '#')
			continue;
		char *eq = strstr(s, " = ");
		if (!eq) {
			*error = xasprintf("line %d: expected 'key = value'", lineno);
			goto bad;
		}
		*eq = '\0';
		const char *k = s, *v = eq + 3;
		uint64_t n;
		if (!strcmp(k, "name"))
			set_once(&p->name, v);
		else if (!strcmp(k, "base"))
			set_once(&p->base, v);
		else if (!strcmp(k, "version"))
			set_once(&p->version, v);
		else if (!strcmp(k, "summary"))
			set_once(&p->summary, v);
		else if (!strcmp(k, "url"))
			set_once(&p->url, v);
		else if (!strcmp(k, "arch"))
			set_once(&p->arch, v);
		else if (!strcmp(k, "filename"))
			set_once(&p->filename, v);
		else if (!strcmp(k, "sha256"))
			set_once(&p->sha256, v);
		else if (!strcmp(k, "license"))
			sl_add(&p->license, v);
		else if (!strcmp(k, "depend"))
			sl_add(&p->depend, v);
		else if (!strcmp(k, "provide"))
			sl_add(&p->provide, v);
		else if (!strcmp(k, "conflict"))
			sl_add(&p->conflict, v);
		else if (!strcmp(k, "replace"))
			sl_add(&p->replace, v);
		else if (!strcmp(k, "group"))
			sl_add(&p->group, v);
		else if (!strcmp(k, "sysuser"))
			sl_add(&p->sysuser, v);
		else if (!strcmp(k, "sysgroup"))
			sl_add(&p->sysgroup, v);
		else if (!strcmp(k, "builddate") || !strcmp(k, "size") || !strcmp(k, "csize")) {
			if (!parse_u64(v, &n)) {
				*error = xasprintf("line %d: bad number for %s", lineno, k);
				goto bad;
			}
			if (!strcmp(k, "builddate"))
				p->builddate = (int64_t)n;
			else if (!strcmp(k, "size"))
				p->size = n;
			else
				p->csize = n;
		}
		/* unknown keys are ignored, so newer packages stay readable */
	}
	free(copy);
	if (!p->name || !p->version || !p->arch) {
		*error = xstrdup("name, version and arch are required");
		pkginfo_free(p);
		return -1;
	}
	if (!p->base)
		p->base = xstrdup(p->name);
	return 0;
bad:
	free(copy);
	pkginfo_free(p);
	return -1;
}

static void put_kv(struct strbuf *out, const char *k, const char *v)
{
	if (v && *v)
		sb_addf(out, "%s = %s\n", k, v);
}

static void put_list(struct strbuf *out, const char *k, const struct strlist *l)
{
	for (size_t i = 0; i < l->n; i++)
		put_kv(out, k, l->v[i]);
}

void pkginfo_write(const struct pkginfo *p, struct strbuf *out, bool repo_fields)
{
	put_kv(out, "name", p->name);
	put_kv(out, "base", p->base);
	put_kv(out, "version", p->version);
	put_kv(out, "summary", p->summary);
	put_kv(out, "url", p->url);
	put_kv(out, "arch", p->arch);
	put_list(out, "license", &p->license);
	sb_addf(out, "builddate = %lld\n", (long long)p->builddate);
	sb_addf(out, "size = %llu\n", (unsigned long long)p->size);
	put_list(out, "depend", &p->depend);
	put_list(out, "provide", &p->provide);
	put_list(out, "conflict", &p->conflict);
	put_list(out, "replace", &p->replace);
	put_list(out, "group", &p->group);
	put_list(out, "sysgroup", &p->sysgroup);
	put_list(out, "sysuser", &p->sysuser);
	if (repo_fields) {
		put_kv(out, "filename", p->filename);
		sb_addf(out, "csize = %llu\n", (unsigned long long)p->csize);
		put_kv(out, "sha256", p->sha256);
	}
}

/* ---- .FILES ---------------------------------------------------------------
 * One line per entry, seven space-separated fields:
 *   TYPE MODE UID GID SIZE SHA256 PATH      (type f)
 *   TYPE MODE UID GID - TARGET PATH         (types l and h)
 *   TYPE MODE UID GID - - PATH              (type d)
 * Backslash, space and newline in TARGET and PATH are written as \\, \040
 * and \012, as in mtree. */

void escape_path(struct strbuf *out, const char *s)
{
	for (; *s; s++) {
		if (*s == '\\')
			sb_addstr(out, "\\\\");
		else if (*s == ' ')
			sb_addstr(out, "\\040");
		else if (*s == '\n')
			sb_addstr(out, "\\012");
		else
			sb_addch(out, *s);
	}
}

char *unescape_path(const char *s)
{
	struct strbuf sb = {0};
	for (; *s; s++) {
		if (*s != '\\') {
			sb_addch(&sb, *s);
		} else if (s[1] == '\\') {
			sb_addch(&sb, '\\');
			s++;
		} else if (s[1] >= '0' && s[1] <= '3' && s[2] >= '0' && s[2] <= '7' &&
		           s[3] >= '0' && s[3] <= '7') {
			sb_addch(&sb, (char)((s[1] - '0') * 64 + (s[2] - '0') * 8 + (s[3] - '0')));
			s += 3;
		} else {
			sb_free(&sb);
			return NULL;
		}
	}
	return sb_steal(&sb);
}

void manifest_add(struct manifest *m, const struct manifest_entry *e)
{
	if (m->n == m->cap) {
		m->cap = m->cap ? m->cap * 2 : 64;
		m->v = xrealloc(m->v, m->cap * sizeof(*m->v));
	}
	m->v[m->n++] = *e;
}

static int cmp_entry(const void *a, const void *b)
{
	return strcmp(((const struct manifest_entry *)a)->path,
	              ((const struct manifest_entry *)b)->path);
}

void manifest_sort(struct manifest *m)
{
	if (m->n > 1)
		qsort(m->v, m->n, sizeof(*m->v), cmp_entry);
}

const struct manifest_entry *manifest_find(const struct manifest *m, const char *path)
{
	struct manifest_entry key = {.path = (char *)path};
	return m->n ? bsearch(&key, m->v, m->n, sizeof(*m->v), cmp_entry) : NULL;
}

void manifest_free(struct manifest *m)
{
	for (size_t i = 0; i < m->n; i++) {
		free(m->v[i].target);
		free(m->v[i].path);
	}
	free(m->v);
	memset(m, 0, sizeof(*m));
}

/* In place: "a//b/./c" -> "a/b/c", "sysflfs/x" -> "usr/x". False for "..", or an empty name. */
static bool canon_manifest_path(char **pp)
{
	const char *in = *pp;
	char *out = malloc(strlen(in) + 8);
	if (!out)
		return false;
	size_t n = 0;
	while (*in) {
		while (*in == '/')
			in++;
		const char *e = in;
		while (*e && *e != '/')
			e++;
		size_t len = (size_t)(e - in);
		if (len == 2 && in[0] == '.' && in[1] == '.') {
			free(out);
			return false;
		}
		if (len && !(len == 1 && in[0] == '.')) {
			const char *word = in;
			if (n == 0 && len == 7 && !memcmp(in, "sysflfs", 7) && e[strspn(e, "/")]) {	/* the link itself stays */
				word = "usr";
				len = 3;
			}
			if (n)
				out[n++] = '/';
			memcpy(out + n, word, len);
			n += len;
		}
		in = e;
	}
	out[n] = 0;
	if (!n) {
		free(out);
		return false;
	}
	free(*pp);
	*pp = out;
	return true;
}

int manifest_parse(const char *text, struct manifest *m, char **error)
{
	char *copy = xstrdup(text), *save = NULL;
	int lineno = 0;
	memset(m, 0, sizeof(*m));
	for (char *line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		lineno++;
		char *f[7];
		char *s = line;
		for (int i = 0; i < 6; i++) {
			f[i] = s;
			s = strchr(s, ' ');
			if (!s)
				goto bad;
			*s++ = '\0';
		}
		f[6] = s;
		if (strchr(f[6], ' '))
			goto bad;
		struct manifest_entry e = {0};
		char *end;
		e.type = f[0][0];
		if (strlen(f[0]) != 1 || !strchr("dflh", e.type) || !*f[6])
			goto bad;
		e.mode = (mode_t)strtoul(f[1], &end, 8);
		if (*end)
			goto bad;
		e.uid = (uid_t)strtoul(f[2], &end, 10);
		if (*end)
			goto bad;
		e.gid = (gid_t)strtoul(f[3], &end, 10);
		if (*end)
			goto bad;
		if (e.type == 'f') {
			if (!parse_u64(f[4], &e.size) || strlen(f[5]) != SHA256_HEX - 1)
				goto bad;
			memcpy(e.sha256, f[5], SHA256_HEX);
		} else if (e.type == 'l' || e.type == 'h') {
			if (!(e.target = unescape_path(f[5])))
				goto bad;
		}
		if (!(e.path = unescape_path(f[6]))) {
			free(e.target);
			goto bad;
		}
		/* one name per file: no "..", "." or empty parts, no leading "/", and the old /sysflfs
		 * alias spelled usr/ (it is a link to usr now), so ownership checks compare like with like */
		if (!canon_manifest_path(&e.path)) {
			free(e.target);
			free(e.path);
			goto bad;
		}
		manifest_add(m, &e);
	}
	free(copy);
	return 0;
bad:
	*error = xasprintf(".FILES line %d is malformed", lineno);
	free(copy);
	manifest_free(m);
	return -1;
}

void manifest_write(const struct manifest *m, struct strbuf *out)
{
	for (size_t i = 0; i < m->n; i++) {
		const struct manifest_entry *e = &m->v[i];
		sb_addf(out, "%c %04o %u %u ", e->type, (unsigned)e->mode, (unsigned)e->uid,
		        (unsigned)e->gid);
		if (e->type == 'f') {
			sb_addf(out, "%llu %s", (unsigned long long)e->size, e->sha256);
		} else if (e->type == 'l' || e->type == 'h') {
			sb_addstr(out, "- ");
			escape_path(out, e->target);
		} else {
			sb_addstr(out, "- -");
		}
		sb_addch(out, ' ');
		escape_path(out, e->path);
		sb_addch(out, '\n');
	}
}
