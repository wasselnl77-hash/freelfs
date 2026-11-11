/* The installed-package database: /var/lib/flpkg/db.sqlite.
 * SPDX-License-Identifier: MIT */
#include "flpkg.h"
#include "db.h"

#include <errno.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SCHEMA_VERSION 1

struct db {
	sqlite3 *h;
	char *path;
};

static const char schema[] =
	"CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT);"
	"CREATE TABLE IF NOT EXISTS packages("
	"  id INTEGER PRIMARY KEY, name TEXT UNIQUE NOT NULL, version TEXT NOT NULL,"
	"  installdate INTEGER NOT NULL, reason TEXT NOT NULL, pkginfo TEXT NOT NULL);"
	"CREATE TABLE IF NOT EXISTS relations("
	"  pkg INTEGER NOT NULL REFERENCES packages(id) ON DELETE CASCADE,"
	"  kind TEXT NOT NULL, value TEXT NOT NULL);"
	"CREATE INDEX IF NOT EXISTS relations_pkg ON relations(pkg);"
	"CREATE TABLE IF NOT EXISTS files("
	"  path TEXT PRIMARY KEY, pkg INTEGER NOT NULL REFERENCES packages(id) ON DELETE CASCADE,"
	"  type TEXT NOT NULL, mode INTEGER NOT NULL, uid INTEGER NOT NULL, gid INTEGER NOT NULL,"
	"  size INTEGER, sha256 TEXT, target TEXT);"
	"CREATE INDEX IF NOT EXISTS files_pkg ON files(pkg);"
	"CREATE TABLE IF NOT EXISTS dirs("
	"  path TEXT NOT NULL, pkg INTEGER NOT NULL REFERENCES packages(id) ON DELETE CASCADE,"
	"  mode INTEGER NOT NULL, uid INTEGER NOT NULL, gid INTEGER NOT NULL,"
	"  PRIMARY KEY(path, pkg));"
	"CREATE INDEX IF NOT EXISTS dirs_pkg ON dirs(pkg);"
	"CREATE TABLE IF NOT EXISTS pending_owners("
	"  path TEXT PRIMARY KEY, uid INTEGER NOT NULL, gid INTEGER NOT NULL, mode INTEGER NOT NULL);"
	"CREATE TABLE IF NOT EXISTS history("
	"  id INTEGER PRIMARY KEY, time INTEGER NOT NULL, action TEXT NOT NULL, name TEXT NOT NULL,"
	"  old_version TEXT, new_version TEXT);";

static _Noreturn void db_die(struct db *db, const char *what)
{
	die("database %s: %s: %s", db->path, what, sqlite3_errmsg(db->h));
}

static void exec(struct db *db, const char *sql)
{
	char *err = NULL;
	if (sqlite3_exec(db->h, sql, NULL, NULL, &err) != SQLITE_OK)
		die("database %s: %s", db->path, err ? err : "error");
}

static sqlite3_stmt *prep(struct db *db, const char *sql)
{
	sqlite3_stmt *st;
	if (sqlite3_prepare_v2(db->h, sql, -1, &st, NULL) != SQLITE_OK)
		db_die(db, sql);
	return st;
}

static void step_done(struct db *db, sqlite3_stmt *st)
{
	if (sqlite3_step(st) != SQLITE_DONE)
		db_die(db, sqlite3_sql(st));
	sqlite3_finalize(st);
}

char *db_dir(const char *root)
{
	return path_join(root, "var/lib/flpkg");
}

struct db *db_open(const char *root, bool create)
{
	struct db *db = xcalloc(1, sizeof(*db));
	char *dir = db_dir(root);
	db->path = path_join(dir, "db.sqlite");
	if (create && mkdirs(dir, 0755) < 0)
		die("cannot create %s: %s", dir, strerror(errno));
	free(dir);
	int flags = create ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE : SQLITE_OPEN_READONLY;
	if (sqlite3_open_v2(db->path, &db->h, flags, NULL) != SQLITE_OK) {
		if (!create) {
			/* no database yet: nothing is installed */
			sqlite3_close(db->h);
			free(db->path);
			free(db);
			return NULL;
		}
		db_die(db, "open");
	}
	sqlite3_busy_timeout(db->h, 10000);
	exec(db, "PRAGMA foreign_keys = ON;");
	if (create) {
		exec(db, "PRAGMA synchronous = FULL;");
		exec(db, schema);
		char *v = xasprintf("INSERT OR IGNORE INTO meta VALUES('schema', '%d');", SCHEMA_VERSION);
		exec(db, v);
		free(v);
	}
	return db;
}

void db_close(struct db *db)
{
	if (!db)
		return;
	sqlite3_close(db->h);
	free(db->path);
	free(db);
}

void db_begin(struct db *db)
{
	exec(db, "BEGIN IMMEDIATE;");
}

void db_commit(struct db *db)
{
	exec(db, "COMMIT;");
}

void db_rollback(struct db *db)
{
	sqlite3_exec(db->h, "ROLLBACK;", NULL, NULL, NULL);
}

/* ---- packages ------------------------------------------------------------------ */

static void load_row(sqlite3_stmt *st, struct installed *ip)
{
	char *err = NULL;
	const char *text = (const char *)sqlite3_column_text(st, 0);
	if (pkginfo_parse(text, &ip->info, &err) < 0)
		die("database: bad package record: %s", err);
	ip->reason = xstrdup((const char *)sqlite3_column_text(st, 1));
	ip->installdate = sqlite3_column_int64(st, 2);
}

bool db_get(struct db *db, const char *name, struct installed *out)
{
	memset(out, 0, sizeof(*out));
	if (!db)
		return false;
	sqlite3_stmt *st = prep(db, "SELECT pkginfo, reason, installdate FROM packages WHERE name = ?");
	sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
	bool found = sqlite3_step(st) == SQLITE_ROW;
	if (found)
		load_row(st, out);
	sqlite3_finalize(st);
	return found;
}

void db_load_all(struct db *db, struct pkglist *out)
{
	memset(out, 0, sizeof(*out));
	if (!db)
		return;
	sqlite3_stmt *st = prep(db, "SELECT pkginfo, reason, installdate FROM packages ORDER BY name");
	while (sqlite3_step(st) == SQLITE_ROW) {
		struct installed ip = {0};
		load_row(st, &ip);
		pkglist_add(out, &ip);
	}
	sqlite3_finalize(st);
}

void installed_free(struct installed *ip)
{
	pkginfo_free(&ip->info);
	free(ip->reason);
	memset(ip, 0, sizeof(*ip));
}

void pkglist_add(struct pkglist *l, const struct installed *ip)
{
	if (l->n == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 64;
		l->v = xrealloc(l->v, l->cap * sizeof(*l->v));
	}
	l->v[l->n++] = *ip;
}

void pkglist_free(struct pkglist *l)
{
	for (size_t i = 0; i < l->n; i++)
		installed_free(&l->v[i]);
	free(l->v);
	memset(l, 0, sizeof(*l));
}

char *db_meta_get(struct db *db, const char *key)
{
	if (!db)
		return NULL;
	sqlite3_stmt *st = prep(db, "SELECT value FROM meta WHERE key = ?");
	sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
	char *v = sqlite3_step(st) == SQLITE_ROW ? xstrdup((const char *)sqlite3_column_text(st, 0))
	                                         : NULL;
	sqlite3_finalize(st);
	return v;
}

void db_meta_set(struct db *db, const char *key, const char *value)
{
	sqlite3_stmt *st = prep(db, "INSERT OR REPLACE INTO meta VALUES(?, ?)");
	sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 2, value, -1, SQLITE_STATIC);
	step_done(db, st);
}

static void add_relations(struct db *db, int64_t id, const char *kind, const struct strlist *l)
{
	for (size_t i = 0; i < l->n; i++) {
		sqlite3_stmt *st = prep(db, "INSERT INTO relations VALUES(?, ?, ?)");
		sqlite3_bind_int64(st, 1, id);
		sqlite3_bind_text(st, 2, kind, -1, SQLITE_STATIC);
		sqlite3_bind_text(st, 3, l->v[i], -1, SQLITE_STATIC);
		step_done(db, st);
	}
}

void db_add_package(struct db *db, const struct pkginfo *p, const struct manifest *m,
                    const char *reason)
{
	struct strbuf text = {0};
	pkginfo_write(p, &text, false);
	sqlite3_stmt *st = prep(db, "INSERT INTO packages(name, version, installdate, reason, pkginfo)"
	                            " VALUES(?, ?, ?, ?, ?)");
	sqlite3_bind_text(st, 1, p->name, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 2, p->version, -1, SQLITE_STATIC);
	sqlite3_bind_int64(st, 3, (int64_t)time(NULL));
	sqlite3_bind_text(st, 4, reason, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 5, text.buf, -1, SQLITE_STATIC);
	step_done(db, st);
	sb_free(&text);
	int64_t id = sqlite3_last_insert_rowid(db->h);

	add_relations(db, id, "depend", &p->depend);
	add_relations(db, id, "provide", &p->provide);
	add_relations(db, id, "conflict", &p->conflict);
	add_relations(db, id, "replace", &p->replace);
	add_relations(db, id, "group", &p->group);

	sqlite3_stmt *fst = prep(db, "INSERT INTO files VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)");
	sqlite3_stmt *dst = prep(db, "INSERT INTO dirs VALUES(?, ?, ?, ?, ?)");
	for (size_t i = 0; i < m->n; i++) {
		const struct manifest_entry *e = &m->v[i];
		sqlite3_stmt *s = e->type == 'd' ? dst : fst;
		sqlite3_reset(s);
		sqlite3_bind_text(s, 1, e->path, -1, SQLITE_STATIC);
		sqlite3_bind_int64(s, 2, id);
		if (e->type == 'd') {
			sqlite3_bind_int(s, 3, (int)e->mode);
			sqlite3_bind_int64(s, 4, e->uid);
			sqlite3_bind_int64(s, 5, e->gid);
		} else {
			char type[2] = {e->type, 0};
			sqlite3_bind_text(s, 3, type, -1, SQLITE_TRANSIENT);
			sqlite3_bind_int(s, 4, (int)e->mode);
			sqlite3_bind_int64(s, 5, e->uid);
			sqlite3_bind_int64(s, 6, e->gid);
			if (e->type == 'f') {
				sqlite3_bind_int64(s, 7, (int64_t)e->size);
				sqlite3_bind_text(s, 8, e->sha256, -1, SQLITE_STATIC);
				sqlite3_bind_null(s, 9);
			} else {
				sqlite3_bind_null(s, 7);
				sqlite3_bind_null(s, 8);
				sqlite3_bind_text(s, 9, e->target, -1, SQLITE_STATIC);
			}
		}
		if (sqlite3_step(s) != SQLITE_DONE)
			db_die(db, e->path);
	}
	sqlite3_finalize(fst);
	sqlite3_finalize(dst);
}

void db_remove_package(struct db *db, const char *name)
{
	sqlite3_stmt *st = prep(db, "DELETE FROM packages WHERE name = ?");
	sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
	step_done(db, st);
}

void db_set_reason(struct db *db, const char *name, const char *reason)
{
	sqlite3_stmt *st = prep(db, "UPDATE packages SET reason = ? WHERE name = ?");
	sqlite3_bind_text(st, 1, reason, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 2, name, -1, SQLITE_STATIC);
	step_done(db, st);
}

/* ---- files ------------------------------------------------------------------- */

static void read_entry(sqlite3_stmt *st, struct manifest_entry *e, bool dir)
{
	memset(e, 0, sizeof(*e));
	e->path = xstrdup((const char *)sqlite3_column_text(st, 0));
	if (dir) {
		e->type = 'd';
		e->mode = (mode_t)sqlite3_column_int(st, 1);
		e->uid = (uid_t)sqlite3_column_int64(st, 2);
		e->gid = (gid_t)sqlite3_column_int64(st, 3);
		return;
	}
	e->type = ((const char *)sqlite3_column_text(st, 1))[0];
	e->mode = (mode_t)sqlite3_column_int(st, 2);
	e->uid = (uid_t)sqlite3_column_int64(st, 3);
	e->gid = (gid_t)sqlite3_column_int64(st, 4);
	e->size = (uint64_t)sqlite3_column_int64(st, 5);
	if (sqlite3_column_type(st, 6) != SQLITE_NULL)
		snprintf(e->sha256, sizeof(e->sha256), "%s", sqlite3_column_text(st, 6));
	if (sqlite3_column_type(st, 7) != SQLITE_NULL)
		e->target = xstrdup((const char *)sqlite3_column_text(st, 7));
}

void db_files(struct db *db, const char *name, struct manifest *out)
{
	memset(out, 0, sizeof(*out));
	if (!db)
		return;
	sqlite3_stmt *st = prep(db, "SELECT d.path, d.mode, d.uid, d.gid FROM dirs d"
	                            " JOIN packages p ON p.id = d.pkg WHERE p.name = ?");
	sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
	struct manifest_entry e;
	while (sqlite3_step(st) == SQLITE_ROW) {
		read_entry(st, &e, true);
		manifest_add(out, &e);
	}
	sqlite3_finalize(st);
	st = prep(db, "SELECT f.path, f.type, f.mode, f.uid, f.gid, f.size, f.sha256, f.target"
	              " FROM files f JOIN packages p ON p.id = f.pkg WHERE p.name = ?");
	sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
	while (sqlite3_step(st) == SQLITE_ROW) {
		read_entry(st, &e, false);
		manifest_add(out, &e);
	}
	sqlite3_finalize(st);
	manifest_sort(out);
}

/* The package owning a file or symlink at PATH (relative), or NULL. */
char *db_file_owner(struct db *db, const char *path)
{
	if (!db)
		return NULL;
	sqlite3_stmt *st = prep(db, "SELECT p.name FROM files f JOIN packages p ON p.id = f.pkg"
	                            " WHERE f.path = ?");
	sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
	char *owner = sqlite3_step(st) == SQLITE_ROW
	                  ? xstrdup((const char *)sqlite3_column_text(st, 0))
	                  : NULL;
	sqlite3_finalize(st);
	return owner;
}

/* Packages that own directory PATH. */
void db_dir_owners(struct db *db, const char *path, struct strlist *out)
{
	memset(out, 0, sizeof(*out));
	if (!db)
		return;
	sqlite3_stmt *st = prep(db, "SELECT p.name FROM dirs d JOIN packages p ON p.id = d.pkg"
	                            " WHERE d.path = ?");
	sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
	while (sqlite3_step(st) == SQLITE_ROW)
		sl_add(out, (const char *)sqlite3_column_text(st, 0));
	sqlite3_finalize(st);
}

/* ---- pending ownership --------------------------------------------------------- */

void db_add_pending(struct db *db, const char *path, uid_t uid, gid_t gid, mode_t mode)
{
	sqlite3_stmt *st = prep(db, "INSERT OR REPLACE INTO pending_owners VALUES(?, ?, ?, ?)");
	sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
	sqlite3_bind_int64(st, 2, uid);
	sqlite3_bind_int64(st, 3, gid);
	sqlite3_bind_int(st, 4, (int)mode);
	step_done(db, st);
}

void db_drop_pending(struct db *db, const char *path)
{
	sqlite3_stmt *st = prep(db, "DELETE FROM pending_owners WHERE path = ?");
	sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
	step_done(db, st);
}

void db_pending(struct db *db, struct manifest *out)
{
	memset(out, 0, sizeof(*out));
	if (!db)
		return;
	sqlite3_stmt *st = prep(db, "SELECT path, uid, gid, mode FROM pending_owners ORDER BY path");
	while (sqlite3_step(st) == SQLITE_ROW) {
		struct manifest_entry e = {.type = 'f'};
		e.path = xstrdup((const char *)sqlite3_column_text(st, 0));
		e.uid = (uid_t)sqlite3_column_int64(st, 1);
		e.gid = (gid_t)sqlite3_column_int64(st, 2);
		e.mode = (mode_t)sqlite3_column_int(st, 3);
		manifest_add(out, &e);
	}
	sqlite3_finalize(st);
}

/* ---- history -------------------------------------------------------------------- */

void db_history_add(struct db *db, const char *action, const char *name, const char *oldv,
                    const char *newv)
{
	sqlite3_stmt *st = prep(db, "INSERT INTO history(time, action, name, old_version, new_version)"
	                            " VALUES(?, ?, ?, ?, ?)");
	sqlite3_bind_int64(st, 1, (int64_t)time(NULL));
	sqlite3_bind_text(st, 2, action, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 3, name, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 4, oldv, -1, SQLITE_STATIC);
	sqlite3_bind_text(st, 5, newv, -1, SQLITE_STATIC);
	step_done(db, st);
}

void db_history_print(struct db *db, FILE *out)
{
	if (!db)
		return;
	sqlite3_stmt *st = prep(db, "SELECT time, action, name, old_version, new_version"
	                            " FROM history ORDER BY id");
	while (sqlite3_step(st) == SQLITE_ROW) {
		time_t t = (time_t)sqlite3_column_int64(st, 0);
		char when[32];
		strftime(when, sizeof(when), "%Y-%m-%d %H:%M", localtime(&t));
		const char *action = (const char *)sqlite3_column_text(st, 1);
		const char *name = (const char *)sqlite3_column_text(st, 2);
		const char *oldv = (const char *)sqlite3_column_text(st, 3);
		const char *newv = (const char *)sqlite3_column_text(st, 4);
		fprintf(out, "%s  %-9s %s", when, action, name);
		if (oldv && newv)
			fprintf(out, " (%s -> %s)", oldv, newv);
		else if (newv || oldv)
			fprintf(out, " (%s)", newv ? newv : oldv);
		fputc('\n', out);
	}
	sqlite3_finalize(st);
}
