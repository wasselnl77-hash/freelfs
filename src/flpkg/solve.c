/* Dependency resolution against the repos.
 *
 * Chosen packages are pulled in depth first. A dependency counts as met by
 * a package already chosen, or by an installed one that is neither being
 * replaced nor upgraded in this transaction. Otherwise the provider is: a
 * package of that exact name that meets the version constraint (first repo
 * in config order), else the first package whose provides meet it.
 * SPDX-License-Identifier: MIT */
#include "repo.h"

#include <stdlib.h>
#include <string.h>

static const struct choice *find_choice(const struct solution *s, const char *name)
{
	for (size_t i = 0; i < s->n; i++)
		if (!strcmp(s->v[i].pkg->info.name, name))
			return &s->v[i];
	return NULL;
}

static const struct installed *find_installed(const struct pkglist *inst, const char *name)
{
	for (size_t i = 0; i < inst->n; i++)
		if (!strcmp(inst->v[i].info.name, name))
			return &inst->v[i];
	return NULL;
}

static bool met(const struct solution *s, const struct pkglist *inst, const struct dep *d)
{
	for (size_t i = 0; i < s->n; i++) {
		const struct pkginfo *p = &s->v[i].pkg->info;
		if (dep_satisfied_by(d, p->name, p->version, &p->provide))
			return true;
	}
	for (size_t i = 0; i < inst->n; i++) {
		const struct pkginfo *p = &inst->v[i].info;
		if (find_choice(s, p->name) || sl_has(&s->remove, p->name))
			continue;
		if (dep_satisfied_by(d, p->name, p->version, &p->provide))
			return true;
	}
	return false;
}

static const struct repopkg *by_name(const struct reposet *rs, const struct dep *d)
{
	for (size_t i = 0; i < rs->n; i++)
		if (!strcmp(rs->v[i].info.name, d->name) && dep_version_ok(d, rs->v[i].info.version))
			return &rs->v[i];
	return NULL;
}

static const struct repopkg *provider(const struct reposet *rs, const struct dep *d)
{
	const struct repopkg *rp = by_name(rs, d);
	for (size_t i = 0; i < rs->n && !rp; i++) {
		const struct pkginfo *p = &rs->v[i].info;
		if (dep_satisfied_by(d, p->name, p->version, &p->provide))
			rp = &rs->v[i];
	}
	return rp;
}

static void pull(const struct reposet *rs, const struct pkglist *inst, struct solution *s,
                 const struct repopkg *rp, const char *reason)
{
	for (size_t i = 0; i < s->n; i++) {
		if (strcmp(s->v[i].pkg->info.name, rp->info.name))
			continue;
		if (!strcmp(reason, "explicit")) {
			free(s->v[i].reason);
			s->v[i].reason = xstrdup(reason);
		}
		return;
	}
	if (s->n == s->cap) {
		s->cap = s->cap ? s->cap * 2 : 32;
		s->v = xrealloc(s->v, s->cap * sizeof(*s->v));
	}
	s->v[s->n++] = (struct choice){rp, xstrdup(reason)};

	for (size_t i = 0; i < rp->info.replace.n; i++) {
		struct dep r;
		if (dep_parse(rp->info.replace.v[i], &r) < 0)
			continue;
		for (size_t j = 0; j < inst->n; j++) {
			const struct pkginfo *p = &inst->v[j].info;
			if (strcmp(p->name, rp->info.name) && !sl_has(&s->remove, p->name) &&
			    dep_satisfied_by(&r, p->name, p->version, &p->provide))
				sl_add(&s->remove, p->name);
		}
		dep_free(&r);
	}

	for (size_t i = 0; i < rp->info.depend.n; i++) {
		struct dep d;
		if (dep_parse(rp->info.depend.v[i], &d) < 0)
			die("%s: bad dependency '%s'", rp->info.name, rp->info.depend.v[i]);
		if (!met(s, inst, &d)) {
			const struct repopkg *p = provider(rs, &d);
			if (!p)
				die("cannot find %s, which %s needs", rp->info.depend.v[i], rp->info.name);
			pull(rs, inst, s, p, "dep");
		}
		dep_free(&d);
	}
}

static bool up_to_date(const struct pkglist *inst, const struct repopkg *rp)
{
	const struct installed *ip = find_installed(inst, rp->info.name);
	return ip && !strcmp(ip->info.version, rp->info.version);
}

void solve_install(const struct reposet *rs, const struct pkglist *installed,
                   char *const *targets, size_t n, bool needed, struct solution *out)
{
	memset(out, 0, sizeof(*out));
	for (size_t i = 0; i < n; i++) {
		struct dep d;
		if (dep_parse(targets[i], &d) < 0)
			die("bad package name '%s'", targets[i]);
		const struct repopkg *rp = by_name(rs, &d);
		bool group = false;
		if (!rp && d.op == DEP_ANY) {
			for (size_t j = 0; j < rs->n; j++) {
				if (!sl_has(&rs->v[j].info.group, d.name))
					continue;
				group = true;
				/* the first repo wins for a name found twice */
				if (repo_find(rs, rs->v[j].info.name) != &rs->v[j])
					continue;
				if (needed && up_to_date(installed, &rs->v[j]))
					continue;
				pull(rs, installed, out, &rs->v[j], "explicit");
			}
		}
		if (!rp && !group)
			rp = provider(rs, &d);
		if (!rp && !group)
			die("%s is not in any repo (try 'flpkg sync')", targets[i]);
		if (rp) {
			if (needed && up_to_date(installed, rp))
				msg("%s %s is up to date", rp->info.name, rp->info.version);
			else
				pull(rs, installed, out, rp, "explicit");
		}
		dep_free(&d);
	}
}

void solve_upgrade(const struct reposet *rs, const struct pkglist *installed,
                   struct solution *out)
{
	memset(out, 0, sizeof(*out));
	for (size_t i = 0; i < installed->n; i++) {
		const struct installed *ip = &installed->v[i];
		const struct repopkg *rp = repo_find(rs, ip->info.name);
		if (rp && vercmp(rp->info.version, ip->info.version) > 0)
			pull(rs, installed, out, rp, ip->reason);
	}
	/* packages that replace installed ones (renames, merges) */
	for (size_t i = 0; i < rs->n; i++) {
		const struct repopkg *rp = &rs->v[i];
		if (find_installed(installed, rp->info.name) || repo_find(rs, rp->info.name) != rp)
			continue;
		for (size_t j = 0; j < rp->info.replace.n; j++) {
			struct dep r;
			if (dep_parse(rp->info.replace.v[j], &r) < 0)
				continue;
			for (size_t k = 0; k < installed->n; k++) {
				const struct pkginfo *p = &installed->v[k].info;
				if (dep_satisfied_by(&r, p->name, p->version, &p->provide))
					pull(rs, installed, out, rp, installed->v[k].reason);
			}
			dep_free(&r);
		}
	}
}

void solution_free(struct solution *s)
{
	for (size_t i = 0; i < s->n; i++)
		free(s->v[i].reason);
	free(s->v);
	sl_free(&s->remove);
	memset(s, 0, sizeof(*s));
}
