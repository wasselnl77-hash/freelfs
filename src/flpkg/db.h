/* The installed-package database.
 * SPDX-License-Identifier: MIT */
#ifndef FLPKG_DB_H
#define FLPKG_DB_H

#include "flpkg.h"

struct installed {
	struct pkginfo info;
	char *reason; /* "explicit" or "dep" */
	int64_t installdate;
};

struct pkglist {
	struct installed *v;
	size_t n, cap;
};
void pkglist_add(struct pkglist *l, const struct installed *ip);
void pkglist_free(struct pkglist *l);
void installed_free(struct installed *ip);

struct db;
char *db_dir(const char *root);
/* With CREATE false, a missing database gives NULL (nothing installed);
 * every query accepts a NULL db. */
struct db *db_open(const char *root, bool create);
void db_close(struct db *db);
void db_begin(struct db *db);
void db_commit(struct db *db);
void db_rollback(struct db *db);

char *db_meta_get(struct db *db, const char *key);
void db_meta_set(struct db *db, const char *key, const char *value);

bool db_get(struct db *db, const char *name, struct installed *out);
void db_load_all(struct db *db, struct pkglist *out);
void db_add_package(struct db *db, const struct pkginfo *p, const struct manifest *m,
                    const char *reason);
void db_remove_package(struct db *db, const char *name);
void db_set_reason(struct db *db, const char *name, const char *reason);

void db_files(struct db *db, const char *name, struct manifest *out);
char *db_file_owner(struct db *db, const char *path);
void db_dir_owners(struct db *db, const char *path, struct strlist *out);

void db_add_pending(struct db *db, const char *path, uid_t uid, gid_t gid, mode_t mode);
void db_drop_pending(struct db *db, const char *path);
void db_pending(struct db *db, struct manifest *out);

void db_history_add(struct db *db, const char *action, const char *name, const char *oldv,
                    const char *newv);
void db_history_print(struct db *db, FILE *out);

#endif
