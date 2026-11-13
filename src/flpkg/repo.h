/* Repositories and dependency resolution.
 * SPDX-License-Identifier: MIT */
#ifndef FLPKG_REPO_H
#define FLPKG_REPO_H

#include "db.h"

struct repopkg {
	struct pkginfo info; /* with filename, csize, sha256 */
	const char *repo;    /* name, from cfg.repos */
	const char *url;
};

struct reposet {
	struct repopkg *v;
	size_t n, cap;
};

/* Load the synced indexes of every configured repo, in config order. */
void repos_load(struct reposet *rs);
void reposet_free(struct reposet *rs);
/* The package named NAME in the first repo that has it. */
const struct repopkg *repo_find(const struct reposet *rs, const char *name);
/* Copy a package into the cache and check its sha256 and signature;
 * returns the cached path. */
char *repo_fetch(const struct repopkg *rp);
/* Verify FILE.sig for a local package file (dies on failure; leaves the files alone). */
void repo_verify_local(const char *file);

/* solve.c */
struct choice {
	const struct repopkg *pkg;
	char *reason; /* "explicit" or "dep" */
};

struct solution {
	struct choice *v;
	size_t n, cap;
	struct strlist remove; /* installed packages that chosen ones replace */
};
/* Resolve TARGETS (names, "name>=1.2" or group names) and everything they
 * need. With NEEDED, targets already installed at the repo version are
 * skipped. Dies with a clear message if something cannot be satisfied. */
void solve_install(const struct reposet *rs, const struct pkglist *installed,
                   char *const *targets, size_t n, bool needed, struct solution *out);
/* Newer versions of installed packages, packages that replace installed
 * ones, and whatever they need. */
void solve_upgrade(const struct reposet *rs, const struct pkglist *installed,
                   struct solution *out);
void solution_free(struct solution *s);

#endif
