/* Transactions: install, upgrade and remove packages as one atomic step.
 *
 *  1. plan     Every file of every new package gets a hidden temporary name
 *              in its destination directory; every file that will be
 *              replaced or removed gets a hidden backup name there. The plan
 *              goes to the journal, which is synced before anything changes.
 *  2. stage    New files are written under their temporary names and checked
 *              against the package manifest (size, sha256).
 *  3. commit   Each replaced file is renamed to its backup name and the new
 *              one renamed into place; removed files are renamed to backups.
 *  4. record   The database is updated in one SQLite transaction; then the
 *              journal is marked committed.
 *  5. clean    Backups are deleted, emptied directories removed, triggers
 *              run, the journal deleted.
 *
 * Every step before 4 is undone by looking at which names exist, so an
 * interruption at any point (even power loss) is rolled back the next time
 * flpkg runs. After 4, recovery finishes step 5 instead.
 * SPDX-License-Identifier: MIT */
#include "txn.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum op_kind { OP_MKDIR = 'D', OP_FILE = 'F', OP_REMOVE = 'R' };

struct op {
	char kind;
	bool existed; /* OP_FILE: the destination existed when planned */
	char *path, *temp, *backup; /* relative to the root */
	mode_t mode;  /* OP_MKDIR */
	uid_t uid;
	gid_t gid;
};

struct txn_add {
	char *file;
	struct pkgfile pf;
	bool upgrade;
	struct installed old;
	struct manifest oldfiles;
	char *reason;
	size_t *opidx; /* manifest index -> op index (non-directories) */
};

struct txn_del {
	struct installed pkg;
	struct manifest files;
};

struct pending {
	char *path;
	uid_t uid;
	gid_t gid;
	mode_t mode;
};

struct txn {
	char *root;
	char *dir;     /* <root>/var/lib/flpkg */
	char *journal;
	struct db *db;
	struct txn_opts opts;
	int lockfd;
	unsigned long id;
	struct txn_add *add;
	size_t nadd;
	struct txn_del *del;
	size_t ndel;
	struct op *ops;
	size_t nops, capops;
	struct strlist newpaths; /* sorted non-directory paths the new packages install */
	struct strlist rmdirs;   /* directories that may become empty */
	struct strlist touched;  /* for triggers */
	struct pending *pending;
	size_t npending;
};

static char *abspath(const struct txn *t, const char *rel)
{
	return path_join(t->root, rel);
}

static bool exists(const char *root, const char *rel)
{
	struct stat st;
	char *p = path_join(root, rel);
	bool e = lstat(p, &st) == 0;
	free(p);
	return e;
}

/* ---- journal ----------------------------------------------------------------- */

static void put_field(struct strbuf *sb, const char *s)
{
	sb_addch(sb, ' ');
	if (!s || !*s)
		sb_addch(sb, '-');
	else
		escape_path(sb, s);
}

static void write_journal(struct txn *t)
{
	struct strbuf sb = {0};
	sb_addf(&sb, "flpkg-journal 1 %lx\n", t->id);
	for (size_t i = 0; i < t->nops; i++) {
		const struct op *o = &t->ops[i];
		sb_addf(&sb, "%c %d", o->kind, o->existed);
		put_field(&sb, o->path);
		put_field(&sb, o->temp);
		put_field(&sb, o->backup);
		sb_addch(&sb, '\n');
	}
	char *tmp = xasprintf("%s.new", t->journal);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0 || write_all(fd, sb.buf, sb.len) < 0 || fsync(fd) < 0)
		die("cannot write %s: %s", tmp, strerror(errno));
	close(fd);
	if (rename(tmp, t->journal) < 0)
		die("cannot write %s: %s", t->journal, strerror(errno));
	int dfd = open(t->dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dfd >= 0) {
		fsync(dfd);
		close(dfd);
	}
	free(tmp);
	sb_free(&sb);
}

static void mark_committed(struct txn *t)
{
	int fd = open(t->journal, O_WRONLY | O_APPEND | O_CLOEXEC);
	if (fd < 0 || write_all(fd, "committed\n", 10) < 0 || fsync(fd) < 0)
		warn("cannot mark %s committed: %s", t->journal, strerror(errno));
	if (fd >= 0)
		close(fd);
}

/* Undo one operation, deciding from which names exist on disk. */
static void undo_op(const char *root, const struct op *o)
{
	char *path = path_join(root, o->path);
	char *temp = o->temp ? path_join(root, o->temp) : NULL;
	char *backup = o->backup ? path_join(root, o->backup) : NULL;
	struct stat st;
	switch (o->kind) {
	case OP_MKDIR:
		rmdir(path); /* only if it is still empty */
		break;
	case OP_FILE: {
		bool t = temp && lstat(temp, &st) == 0;
		bool b = backup && lstat(backup, &st) == 0;
		if (t)
			unlink(temp);
		if (b)
			rename(backup, path);
		else if (!o->existed && !t)
			unlink(path);
		break;
	}
	case OP_REMOVE:
		if (backup && lstat(backup, &st) == 0)
			rename(backup, path);
		break;
	}
	free(path);
	free(temp);
	free(backup);
}

/* After the database commit: only the backups are left to delete. */
static void finish_op(const char *root, const struct op *o)
{
	if (o->kind == OP_MKDIR || !o->backup)
		return;
	char *backup = path_join(root, o->backup);
	unlink(backup);
	free(backup);
	if (o->temp) { /* normally renamed away already */
		char *temp = path_join(root, o->temp);
		unlink(temp);
		free(temp);
	}
}

static char *journal_field(char **save)
{
	char *f = strtok_r(NULL, " ", save);
	if (!f)
		return NULL;
	return strcmp(f, "-") ? unescape_path(f) : xstrdup("");
}

void txn_recover(const char *root)
{
	char *dir = db_dir(root), *journal = path_join(dir, "journal");
	size_t len;
	char *text = read_file(journal, &len);
	if (!text) {
		free(dir);
		free(journal);
		return;
	}
	struct op *ops = NULL;
	size_t n = 0;
	bool committed = false;
	char *save = NULL, *id = NULL;
	for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		if (!strcmp(line, "committed")) {
			committed = true;
			continue;
		}
		if (has_prefix(line, "flpkg-journal")) {
			const char *sp = strrchr(line, ' ');
			free(id);
			id = sp ? xstrdup(sp + 1) : NULL;
			continue;
		}
		char *fsave = NULL, *kind = strtok_r(line, " ", &fsave);
		char *existed = strtok_r(NULL, " ", &fsave);
		if (!kind || !existed)
			continue;
		struct op o = {.kind = kind[0], .existed = existed[0] == '1'};
		o.path = journal_field(&fsave);
		o.temp = journal_field(&fsave);
		o.backup = journal_field(&fsave);
		if (!o.path || !*o.path) {
			free(o.path), free(o.temp), free(o.backup);
			continue;
		}
		if (o.temp && !*o.temp)
			free(o.temp), o.temp = NULL;
		if (o.backup && !*o.backup)
			free(o.backup), o.backup = NULL;
		ops = xrealloc(ops, (n + 1) * sizeof(*ops));
		ops[n++] = o;
	}
	/* The database records the id of the last transaction it committed, in
	 * the same SQLite transaction as the package changes: that decides
	 * whether a journal without the "committed" mark got that far. */
	if (!committed && id) {
		struct db *db = db_open(root, false);
		char *last = db_meta_get(db, "last_txn");
		committed = last && !strcmp(last, id);
		free(last);
		db_close(db);
	}
	free(id);
	if (committed) {
		warn("finishing an interrupted transaction");
		for (size_t i = 0; i < n; i++)
			finish_op(root, &ops[i]);
	} else {
		warn("rolling back an interrupted transaction");
		for (size_t i = n; i-- > 0;)
			undo_op(root, &ops[i]);
	}
	for (size_t i = 0; i < n; i++) {
		free(ops[i].path);
		free(ops[i].temp);
		free(ops[i].backup);
	}
	free(ops);
	free(text);
	if (unlink(journal) < 0)
		die("cannot remove %s: %s", journal, strerror(errno));
	free(journal);
	free(dir);
}

/* Before reading the database: finish or undo an interrupted transaction,
 * if this process may (otherwise say so and carry on). */
void txn_recover_pending(const char *root)
{
	char *dir = db_dir(root), *journal = path_join(dir, "journal"), *lock = path_join(dir, "lock");
	if (access(journal, F_OK) == 0) {
		int fd = open(lock, O_RDWR | O_CLOEXEC);
		if (fd < 0 || access(dir, W_OK) < 0)
			warn("an interrupted transaction needs recovery; run flpkg as root");
		else if (flock(fd, LOCK_EX | LOCK_NB) == 0)
			txn_recover(root);
		if (fd >= 0)
			close(fd);
	}
	free(journal);
	free(lock);
	free(dir);
}

/* ---- setting up ---------------------------------------------------------------- */

struct txn *txn_new(const char *root, const struct txn_opts *opts)
{
	struct txn *t = xcalloc(1, sizeof(*t));
	t->root = xstrdup(root);
	t->dir = db_dir(root);
	t->journal = path_join(t->dir, "journal");
	if (opts)
		t->opts = *opts;
	if (mkdirs(t->dir, 0755) < 0)
		die("cannot create %s: %s", t->dir, strerror(errno));
	char *lock = path_join(t->dir, "lock");
	t->lockfd = open(lock, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (t->lockfd < 0)
		die("cannot open %s: %s", lock, strerror(errno));
	if (flock(t->lockfd, LOCK_EX | LOCK_NB) < 0)
		die("another flpkg is working on %s (%s is locked)", root, lock);
	free(lock);
	txn_recover(root);
	t->db = db_open(root, true);
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	t->id = (unsigned long)getpid() ^ (unsigned long)ts.tv_nsec;
	return t;
}

struct db *txn_db(struct txn *t)
{
	return t->db;
}

void txn_add_file(struct txn *t, const char *path, const char *reason)
{
	struct txn_add a = {.file = xstrdup(path)};
	char *err = NULL;
	if (pkgfile_open(path, &a.pf, &err) < 0)
		die("%s", err);
	for (size_t i = 0; i < t->nadd; i++)
		if (!strcmp(t->add[i].pf.info.name, a.pf.info.name))
			die("%s is given twice", a.pf.info.name);
	a.upgrade = db_get(t->db, a.pf.info.name, &a.old);
	if (a.upgrade) {
		db_files(t->db, a.pf.info.name, &a.oldfiles);
		if (!strcmp(a.old.reason, "explicit"))
			reason = "explicit";
	}
	a.reason = xstrdup(reason);
	t->add = xrealloc(t->add, (t->nadd + 1) * sizeof(*t->add));
	t->add[t->nadd++] = a;
}

void txn_remove(struct txn *t, const char *name)
{
	struct txn_del d = {0};
	if (!db_get(t->db, name, &d.pkg))
		die("%s is not installed", name);
	for (size_t i = 0; i < t->ndel; i++)
		if (!strcmp(t->del[i].pkg.info.name, name)) {
			installed_free(&d.pkg);
			return;
		}
	db_files(t->db, name, &d.files);
	t->del = xrealloc(t->del, (t->ndel + 1) * sizeof(*t->del));
	t->del[t->ndel++] = d;
}

bool txn_empty(const struct txn *t)
{
	return !t->nadd && !t->ndel;
}

void txn_print_plan(const struct txn *t, FILE *out)
{
	uint64_t size = 0;
	for (size_t i = 0; i < t->nadd; i++) {
		const struct txn_add *a = &t->add[i];
		if (a->upgrade)
			fprintf(out, "   upgrade  %s  %s -> %s\n", a->pf.info.name,
			        a->old.info.version, a->pf.info.version);
		else
			fprintf(out, "   install  %s  %s\n", a->pf.info.name, a->pf.info.version);
		size += a->pf.info.size;
	}
	for (size_t i = 0; i < t->ndel; i++)
		fprintf(out, "   remove   %s  %s\n", t->del[i].pkg.info.name,
		        t->del[i].pkg.info.version);
	if (t->nadd)
		fprintf(out, "   (%.1f MiB to install)\n", (double)size / (1 << 20));
}

/* ---- checks --------------------------------------------------------------------- */

static bool is_removed(const struct txn *t, const char *name)
{
	for (size_t i = 0; i < t->ndel; i++)
		if (!strcmp(t->del[i].pkg.info.name, name))
			return true;
	return false;
}

static const struct txn_add *find_add(const struct txn *t, const char *name)
{
	for (size_t i = 0; i < t->nadd; i++)
		if (!strcmp(t->add[i].pf.info.name, name))
			return &t->add[i];
	return NULL;
}

/* The packages installed after the transaction. */
static void final_set(struct txn *t, struct pkglist *installed, const struct pkginfo ***out,
                      size_t *n)
{
	db_load_all(t->db, installed);
	*out = xcalloc(installed->n + t->nadd + 1, sizeof(**out));
	*n = 0;
	for (size_t i = 0; i < installed->n; i++) {
		const char *name = installed->v[i].info.name;
		if (!is_removed(t, name) && !find_add(t, name))
			(*out)[(*n)++] = &installed->v[i].info;
	}
	for (size_t i = 0; i < t->nadd; i++)
		(*out)[(*n)++] = &t->add[i].pf.info;
}

static bool satisfied(const struct dep *d, const struct pkginfo **set, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (dep_satisfied_by(d, set[i]->name, set[i]->version, &set[i]->provide))
			return true;
	return false;
}

static int check_deps(struct txn *t)
{
	struct pkglist installed;
	const struct pkginfo **set;
	size_t n;
	int problems = 0;
	final_set(t, &installed, &set, &n);

	for (size_t i = 0; i < n; i++) {
		const struct pkginfo *p = set[i];
		bool is_new = find_add(t, p->name) != NULL;
		for (size_t j = 0; j < p->depend.n; j++) {
			struct dep d;
			if (dep_parse(p->depend.v[j], &d) < 0)
				continue;
			if (!satisfied(&d, set, n)) {
				/* Only complain about an old package when this
				 * transaction is what breaks it. */
				bool before = false;
				if (!is_new)
					for (size_t k = 0; k < installed.n && !before; k++)
						before = dep_satisfied_by(&d, installed.v[k].info.name,
						                          installed.v[k].info.version,
						                          &installed.v[k].info.provide);
				if (is_new || before) {
					fprintf(stderr, "   %s %s requires %s\n", p->name, p->version,
					        p->depend.v[j]);
					problems++;
				}
			}
			dep_free(&d);
		}
		for (size_t j = 0; j < p->conflict.n; j++) {
			struct dep d;
			if (dep_parse(p->conflict.v[j], &d) < 0)
				continue;
			for (size_t k = 0; k < n; k++) {
				if (set[k] == p || (!is_new && !find_add(t, set[k]->name)))
					continue;
				if (dep_satisfied_by(&d, set[k]->name, set[k]->version, &set[k]->provide)) {
					fprintf(stderr, "   %s conflicts with %s\n", p->name, set[k]->name);
					problems++;
				}
			}
			dep_free(&d);
		}
	}
	free(set);
	pkglist_free(&installed);
	return problems;
}

static bool overwrite_ok(const struct txn *t, const char *rel)
{
	char *abs = xasprintf("/%s", rel);
	bool ok = false;
	for (size_t i = 0; i < t->opts.overwrite.n && !ok; i++)
		ok = fnmatch(t->opts.overwrite.v[i], abs, 0) == 0;
	free(abs);
	return ok;
}

static bool is_dir_or_link_to_dir(const char *abs)
{
	struct stat st;
	return stat(abs, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Move everything in directory src into dst (created if needed), merging
 * subdirectories that exist in both. */
static void merge_dir(const char *src, const char *dst, const char *rel)
{
	if (mkdirs(dst, 0755) < 0)
		die("cannot create %s: %s", dst, strerror(errno));
	DIR *d = opendir(src);
	if (!d)
		die("cannot read /%s: %s", rel, strerror(errno));
	struct dirent *de;
	while ((de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		char *from = xasprintf("%s/%s", src, de->d_name), *to = xasprintf("%s/%s", dst, de->d_name);
		char *sub = xasprintf("%s/%s", rel, de->d_name);
		struct stat a, b;
		if (lstat(to, &b) < 0) {
			if (rename(from, to) < 0)
				die("cannot move /%s: %s", sub, strerror(errno));
		} else if (lstat(from, &a) == 0 && S_ISDIR(a.st_mode) && S_ISDIR(b.st_mode)) {
			merge_dir(from, to, sub);
			if (rmdir(from) < 0)
				die("cannot remove /%s: %s", sub, strerror(errno));
		} else
			die("cannot move /%s into the new layout: %s exists", sub, to);
		free(from);
		free(to);
		free(sub);
	}
	closedir(d);
}

/* A package brings a link where a directory is (a layout change, like
 * /var -> sysflfs/data), or where other packages of the transaction put
 * files: move the directory's contents to the link's target and put the
 * link in place now, so nothing is missing or misplaced while the
 * transaction runs. The result is the same tree, so this needs no undo. */
static void migrate_to_link(struct txn *t, const struct manifest_entry *e)
{
	char *abs = abspath(t, e->path);
	char *target;
	if (e->target[0] == '/')
		target = abspath(t, e->target + 1);
	else {
		char *dir = xstrdup(e->path);
		char *slash = strrchr(dir, '/');
		if (slash)
			*slash = '\0';
		target = slash ? xasprintf("%s/%s/%s", t->root, dir, e->target)
		               : xasprintf("%s/%s", t->root, e->target);
		free(dir);
	}
	struct stat st;
	if (lstat(abs, &st) == 0) {
		msg("moving /%s to /%s (the new file system layout)", e->path,
		    target + strlen(t->root) + (t->root[strlen(t->root) - 1] == '/' ? 0 : 1));
		merge_dir(abs, target, e->path);
		if (rmdir(abs) < 0)
			die("cannot remove /%s: %s", e->path, strerror(errno));
	} else if (mkdirs(target, 0755) < 0)
		die("cannot create %s: %s", target, strerror(errno));
	if (symlink(e->target, abs) < 0)
		die("cannot replace /%s with a link: %s", e->path, strerror(errno));
	free(abs);
	free(target);
}

/* Is path a directory in some package of this transaction? */
static bool dir_in_transaction(const struct txn *t, const char *path)
{
	for (size_t i = 0; i < t->nadd; i++) {
		const struct manifest_entry *m = manifest_find(&t->add[i].pf.files, path);
		if (m && m->type == 'd')
			return true;
	}
	return false;
}

static int check_files(struct txn *t)
{
	int problems = 0;
	struct strlist seen = {0}; /* "path\tpkg" for duplicates within the transaction */
	for (size_t i = 0; i < t->nadd; i++) {
		const struct txn_add *a = &t->add[i];
		for (size_t j = 0; j < a->pf.files.n; j++) {
			const struct manifest_entry *e = &a->pf.files.v[j];
			char *abs = abspath(t, e->path);
			struct stat st;
			bool there = lstat(abs, &st) == 0;
			if (e->type == 'd') {
				if (there && !is_dir_or_link_to_dir(abs)) {
					fprintf(stderr, "   %s: /%s is a directory in the package but not on disk\n",
					        a->pf.info.name, e->path);
					problems++;
				}
				free(abs);
				continue;
			}
			/* a link standing for a directory (usr/bin -> ../sysflfs/bin):
			 * in place before anything is put "in" it */
			if (e->type == 'l' &&
			    (there ? S_ISDIR(st.st_mode) : dir_in_transaction(t, e->path))) {
				migrate_to_link(t, e);
				there = lstat(abs, &st) == 0;
			}
			sl_add_owned(&seen, xasprintf("%s\t%s", e->path, a->pf.info.name));
			char *owner = db_file_owner(t->db, e->path);
			if (owner) {
				const struct txn_add *oa = find_add(t, owner);
				bool ok = !strcmp(owner, a->pf.info.name) || is_removed(t, owner) ||
				          (oa && !manifest_find(&oa->pf.files, e->path));
				if (!ok) {
					fprintf(stderr, "   %s: /%s is owned by %s\n", a->pf.info.name, e->path,
					        owner);
					problems++;
				}
				free(owner);
			} else if (there) {
				char lt[4096];
				ssize_t ln = S_ISLNK(st.st_mode) ? readlink(abs, lt, sizeof(lt) - 1) : -1;
				if (ln >= 0) /* the same link is already there */
					lt[ln] = '\0';
				if (e->type == 'l' && ln >= 0 && !strcmp(lt, e->target)) {
					; /* fine: replaced by an identical link */
				} else if (S_ISDIR(st.st_mode)) {
					fprintf(stderr, "   %s: /%s is a directory on disk\n", a->pf.info.name,
					        e->path);
					problems++;
				} else if (!overwrite_ok(t, e->path)) {
					fprintf(stderr, "   %s: /%s exists in the file system\n",
					        a->pf.info.name, e->path);
					problems++;
				}
			}
			free(abs);
		}
	}
	sl_sort(&seen);
	for (size_t i = 1; i < seen.n; i++) {
		size_t len = strcspn(seen.v[i], "\t");
		if (!strncmp(seen.v[i], seen.v[i - 1], len) && seen.v[i - 1][len] == '\t') {
			fprintf(stderr, "   /%.*s is in both %s and %s\n", (int)len, seen.v[i],
			        seen.v[i - 1] + len + 1, seen.v[i] + len + 1);
			problems++;
		}
	}
	/* Remember every new non-directory path, for skipping removals. */
	for (size_t i = 0; i < seen.n; i++)
		seen.v[i][strcspn(seen.v[i], "\t")] = '\0';
	t->newpaths = seen;
	return problems;
}

/* ---- planning --------------------------------------------------------------------- */

static size_t add_op(struct txn *t, const struct op *o)
{
	if (t->nops == t->capops) {
		t->capops = t->capops ? t->capops * 2 : 256;
		t->ops = xrealloc(t->ops, t->capops * sizeof(*t->ops));
	}
	t->ops[t->nops] = *o;
	return t->nops++;
}

static char *hidden_name(struct txn *t, const char *rel, const char *what)
{
	static unsigned long counter;
	const char *slash = strrchr(rel, '/');
	if (slash)
		return xasprintf("%.*s/.flpkg.%lx.%lu.%s", (int)(slash - rel), rel, t->id, ++counter, what);
	return xasprintf(".flpkg.%lx.%lu.%s", t->id, ++counter, what);
}

static int cmp_strp(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static bool in_newpaths(const struct txn *t, const char *path)
{
	return t->newpaths.n &&
	       bsearch(&path, t->newpaths.v, t->newpaths.n, sizeof(char *), cmp_strp) != NULL;
}

static void plan_remove(struct txn *t, const struct manifest *files,
                        const struct manifest *keep)
{
	for (size_t i = 0; i < files->n; i++) {
		const struct manifest_entry *e = &files->v[i];
		if (keep && manifest_find(keep, e->path))
			continue;
		if (e->type == 'd') {
			sl_add(&t->rmdirs, e->path);
			continue;
		}
		if (in_newpaths(t, e->path) || !exists(t->root, e->path))
			continue;
		struct op o = {.kind = OP_REMOVE, .existed = true, .path = xstrdup(e->path)};
		o.backup = hidden_name(t, e->path, "old");
		add_op(t, &o);
		sl_add(&t->touched, e->path);
	}
}

static void plan(struct txn *t)
{
	for (size_t i = 0; i < t->nadd; i++) {
		struct txn_add *a = &t->add[i];
		a->opidx = xcalloc(a->pf.files.n + 1, sizeof(size_t));
		for (size_t j = 0; j < a->pf.files.n; j++) {
			const struct manifest_entry *e = &a->pf.files.v[j];
			struct op o = {.path = xstrdup(e->path), .mode = e->mode, .uid = e->uid, .gid = e->gid};
			if (e->type == 'd') {
				if (exists(t->root, e->path)) {
					free(o.path);
					continue;
				}
				o.kind = OP_MKDIR;
				add_op(t, &o);
				continue;
			}
			o.kind = OP_FILE;
			o.existed = exists(t->root, e->path);
			o.temp = hidden_name(t, e->path, "new");
			if (o.existed)
				o.backup = hidden_name(t, e->path, "old");
			a->opidx[j] = add_op(t, &o);
			sl_add(&t->touched, e->path);
		}
	}
	for (size_t i = 0; i < t->nadd; i++)
		if (t->add[i].upgrade)
			plan_remove(t, &t->add[i].oldfiles, &t->add[i].pf.files);
	for (size_t i = 0; i < t->ndel; i++)
		plan_remove(t, &t->del[i].files, NULL);
}

/* ---- staging ----------------------------------------------------------------------- */

static void note_owner(struct txn *t, const char *path, uid_t uid, gid_t gid, mode_t mode)
{
	t->pending = xrealloc(t->pending, (t->npending + 1) * sizeof(*t->pending));
	t->pending[t->npending++] = (struct pending){xstrdup(path), uid, gid, mode};
}

/* chown, or remember it for 'flpkg fix-owners' when this process may not
 * (a user namespace with one uid, or not root). */
static int set_owner(struct txn *t, int fd, const char *abs, const char *rel, uid_t uid,
                     gid_t gid, mode_t mode, bool link)
{
	int rc = link ? lchown(abs, uid, gid) : fchown(fd, uid, gid);
	if (rc < 0) {
		if (errno != EPERM && errno != EINVAL)
			return -1;
		note_owner(t, rel, uid, gid, mode);
	}
	return 0;
}

static void stage_mkdir(struct txn *t, const struct op *o)
{
	char *abs = abspath(t, o->path);
	if (mkdir(abs, o->mode & 07777) < 0) {
		if (errno == EEXIST && is_dir_or_link_to_dir(abs)) {
			free(abs);
			return;
		}
		die("cannot create /%s: %s", o->path, strerror(errno));
	}
	int fd = open(abs, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0 || set_owner(t, fd, abs, o->path, o->uid, o->gid, o->mode, false) < 0 ||
	    fchmod(fd, o->mode & 07777) < 0)
		die("cannot set up /%s: %s", o->path, strerror(errno));
	close(fd);
	free(abs);
}

static void stage_file(struct txn *t, struct txn_add *a, const struct manifest_entry *e,
                       const struct op *o)
{
	char *abs = abspath(t, o->temp);
	int fd = open(abs, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0)
		die("cannot create /%s: %s", o->temp, strerror(errno));
	struct hasher *h = hasher_new("SHA256");
	char buf[65536];
	uint64_t total = 0;
	ssize_t n;
	while ((n = tar_read(&a->pf.tr, buf, sizeof(buf))) > 0) {
		if (write_all(fd, buf, (size_t)n) < 0)
			die("cannot write /%s: %s", o->path, strerror(errno));
		hasher_update(h, buf, (size_t)n);
		total += (uint64_t)n;
	}
	unsigned char md[64];
	char hex[SHA256_HEX];
	hasher_final(h, md);
	hex_encode(md, SHA256_LEN, hex);
	if (n < 0)
		die("%s: %s", a->file, a->pf.tr.error);
	if (total != e->size || strcmp(hex, e->sha256))
		die("%s: /%s does not match the package manifest (corrupt package?)", a->file, e->path);
	struct timespec times[2] = {{cfg.source_date_epoch, 0}, {cfg.source_date_epoch, 0}};
	if (set_owner(t, fd, abs, o->path, e->uid, e->gid, e->mode, false) < 0 ||
	    fchmod(fd, e->mode & 07777) < 0 || futimens(fd, times) < 0 || close(fd) < 0)
		die("cannot set up /%s: %s", o->path, strerror(errno));
	free(abs);
}

static void stage_package(struct txn *t, struct txn_add *a)
{
	struct tar_entry te = {0};
	const struct manifest *m = &a->pf.files;
	for (size_t j = 0; j < m->n; j++) {
		const struct manifest_entry *e = &m->v[j];
		int rc = tar_next(&a->pf.tr, &te);
		char want = e->type == 'd' ? TAR_DIR : e->type == 'l' ? TAR_SYMLINK
		          : e->type == 'h' ? TAR_HARDLINK : TAR_FILE;
		if (rc != 1 || strcmp(te.path, e->path) || te.type != want)
			die("%s: archive does not match its manifest at /%s%s%s", a->file, e->path,
			    rc < 0 ? ": " : "", rc < 0 ? a->pf.tr.error : "");
		if (e->type == 'd')
			continue; /* created from the plan */
		const struct op *o = &t->ops[a->opidx[j]];
		char *temp = abspath(t, o->temp);
		if (e->type == 'f') {
			stage_file(t, a, e, o);
		} else if (e->type == 'l') {
			if (symlink(e->target, temp) < 0 ||
			    set_owner(t, -1, temp, o->path, e->uid, e->gid, e->mode, true) < 0)
				die("cannot create /%s: %s", o->temp, strerror(errno));
		} else {
			/* a hard link: link to the target's staged copy */
			const struct manifest_entry *target = manifest_find(m, e->target);
			if (!target || target->type != 'f')
				die("%s: hard link /%s points at a missing file", a->file, e->path);
			const struct op *to = &t->ops[a->opidx[target - m->v]];
			char *ttemp = abspath(t, to->temp);
			if (link(ttemp, temp) < 0)
				die("cannot link /%s: %s", o->path, strerror(errno));
			free(ttemp);
		}
		free(temp);
	}
	if (tar_next(&a->pf.tr, &te) != 0)
		die("%s: archive has entries its manifest does not list", a->file);
	tar_entry_clear(&te);
}

/* Save a changed /etc file before the package's version replaces it. */
static void save_config(struct txn *t, const struct txn_add *a, const struct manifest_entry *e)
{
	if (!cfg.backup_config || !has_prefix(e->path, "etc/") || e->type != 'f')
		return;
	char *abs = abspath(t, e->path);
	char hex[SHA256_HEX];
	struct stat st;
	if (lstat(abs, &st) < 0 || !S_ISREG(st.st_mode) || sha256_file(abs, hex) < 0) {
		free(abs);
		return;
	}
	const struct manifest_entry *old = a->upgrade ? manifest_find(&a->oldfiles, e->path) : NULL;
	if ((!old || strcmp(old->sha256, hex)) && strcmp(e->sha256, hex)) {
		char *save = xasprintf("%s.flpkg-save", abs);
		size_t len;
		char *data = read_file(abs, &len);
		if (!data || write_file(save, data, len, st.st_mode & 07777) < 0)
			die("cannot save /%s: %s", e->path, strerror(errno));
		warn("/%s was changed; your version is saved as /%s.flpkg-save", e->path, e->path);
		free(data);
		free(save);
	}
	free(abs);
}

/* ---- commit ---------------------------------------------------------------------- */

static void rollback(struct txn *t)
{
	for (size_t i = t->nops; i-- > 0;)
		undo_op(t->root, &t->ops[i]);
	unlink(t->journal);
}

/* While files are being staged or swapped, an error rolls back at once. */
static struct txn *active;

static void rollback_active(void)
{
	if (active) {
		rollback(active);
		fprintf(stderr, "%s: everything was rolled back\n", progname);
	}
}

static int apply_ops(struct txn *t)
{
	for (size_t i = 0; i < t->nops; i++) {
		const struct op *o = &t->ops[i];
		if (o->kind == OP_MKDIR)
			continue;
		char *path = abspath(t, o->path);
		char *backup = o->backup ? abspath(t, o->backup) : NULL;
		char *temp = o->temp ? abspath(t, o->temp) : NULL;
		int rc = 0;
		if (o->kind == OP_FILE) {
			if (backup && rename(path, backup) < 0 && errno != ENOENT)
				rc = -1;
			if (!rc && rename(temp, path) < 0)
				rc = -1;
		} else if (rename(path, backup) < 0 && errno != ENOENT) {
			rc = -1;
		}
		if (rc)
			warn("cannot replace /%s: %s", o->path, strerror(errno));
		free(path);
		free(backup);
		free(temp);
		if (rc)
			return -1;
	}
	return 0;
}

/* For the test suite: FLPKG_TEST_CRASH=staged|applied|recorded stops the
 * process dead at that point, as a power loss would. */
static void crash_point(const char *where)
{
	const char *c = getenv("FLPKG_TEST_CRASH");
	if (c && !strcmp(c, where))
		_exit(99);
}

static int cmp_desc(const void *a, const void *b)
{
	return strcmp(*(char *const *)b, *(char *const *)a);
}

static void remove_empty_dirs(struct txn *t)
{
	if (t->rmdirs.n > 1)
		qsort(t->rmdirs.v, t->rmdirs.n, sizeof(char *), cmp_desc); /* children first */
	for (size_t i = 0; i < t->rmdirs.n; i++) {
		struct strlist owners;
		db_dir_owners(t->db, t->rmdirs.v[i], &owners);
		if (!owners.n) {
			char *abs = abspath(t, t->rmdirs.v[i]);
			rmdir(abs); /* fails, harmlessly, if something is still in it */
			free(abs);
		}
		sl_free(&owners);
	}
}

static void record(struct txn *t)
{
	db_begin(t->db);
	for (size_t i = 0; i < t->ndel; i++) {
		db_remove_package(t->db, t->del[i].pkg.info.name);
		db_history_add(t->db, "remove", t->del[i].pkg.info.name, t->del[i].pkg.info.version, NULL);
	}
	for (size_t i = 0; i < t->nadd; i++) {
		struct txn_add *a = &t->add[i];
		if (a->upgrade)
			db_remove_package(t->db, a->pf.info.name);
		db_add_package(t->db, &a->pf.info, &a->pf.files, a->reason);
		int c = a->upgrade ? vercmp(a->pf.info.version, a->old.info.version) : 1;
		db_history_add(t->db, !a->upgrade ? "install" : c > 0 ? "upgrade"
		                     : c < 0 ? "downgrade" : "reinstall",
		               a->pf.info.name, a->upgrade ? a->old.info.version : NULL,
		               a->pf.info.version);
	}
	for (size_t i = 0; i < t->npending; i++)
		db_add_pending(t->db, t->pending[i].path, t->pending[i].uid, t->pending[i].gid,
		               t->pending[i].mode);
	char *id = xasprintf("%lx", t->id);
	db_meta_set(t->db, "last_txn", id);
	free(id);
	db_commit(t->db);
}

void txn_commit(struct txn *t)
{
	if (!t->opts.nodeps && check_deps(t))
		die("dependency problems; nothing was changed");
	if (check_files(t))
		die("file conflicts; nothing was changed");

	/* a btrfs snapshot to boot into if this goes wrong after all */
	struct strbuf what = {0};
	for (size_t i = 0; i < t->nadd; i++)
		sb_addf(&what, "%s%s %s", what.len ? ", " : "", t->add[i].upgrade ? "upgrade" : "install",
		        t->add[i].pf.info.name);
	for (size_t i = 0; i < t->ndel; i++)
		sb_addf(&what, "%sremove %s", what.len ? ", " : "", t->del[i].pkg.info.name);
	snapshot_before(what.buf ? what.buf : "");
	sb_free(&what);

	for (size_t i = 0; i < t->nadd; i++) {
		ensure_sysgroups(t->root, &t->add[i].pf.info.sysgroup);
		ensure_sysusers(t->root, &t->add[i].pf.info.sysuser);
	}

	plan(t);
	write_journal(t);
	active = t;
	die_hook = rollback_active;

	for (size_t i = 0; i < t->nops; i++)
		if (t->ops[i].kind == OP_MKDIR)
			stage_mkdir(t, &t->ops[i]);
	for (size_t i = 0; i < t->nadd; i++) {
		msg("unpacking %s %s", t->add[i].pf.info.name, t->add[i].pf.info.version);
		stage_package(t, &t->add[i]);
	}
	for (size_t i = 0; i < t->nadd; i++)
		for (size_t j = 0; j < t->add[i].pf.files.n; j++)
			save_config(t, &t->add[i], &t->add[i].pf.files.v[j]);

	crash_point("staged");
	if (apply_ops(t) < 0)
		die("could not put the new files in place");
	crash_point("applied");
	record(t);
	crash_point("recorded");
	die_hook = NULL; /* the database has the new state: no going back now */
	active = NULL;
	mark_committed(t);

	for (size_t i = 0; i < t->nops; i++)
		finish_op(t->root, &t->ops[i]);
	remove_empty_dirs(t);
	if (unlink(t->journal) < 0)
		warn("cannot remove %s: %s", t->journal, strerror(errno));
	run_triggers(t->root, &t->touched);
	if (t->npending)
		msg("%zu files need an owner this process cannot set; 'flpkg fix-owners' applies them",
		    t->npending);
}

void txn_free(struct txn *t)
{
	for (size_t i = 0; i < t->nadd; i++) {
		struct txn_add *a = &t->add[i];
		free(a->file);
		pkgfile_close(&a->pf);
		if (a->upgrade)
			installed_free(&a->old);
		manifest_free(&a->oldfiles);
		free(a->reason);
		free(a->opidx);
	}
	free(t->add);
	for (size_t i = 0; i < t->ndel; i++) {
		installed_free(&t->del[i].pkg);
		manifest_free(&t->del[i].files);
	}
	free(t->del);
	for (size_t i = 0; i < t->nops; i++) {
		free(t->ops[i].path);
		free(t->ops[i].temp);
		free(t->ops[i].backup);
	}
	free(t->ops);
	for (size_t i = 0; i < t->npending; i++)
		free(t->pending[i].path);
	free(t->pending);
	sl_free(&t->newpaths);
	sl_free(&t->rmdirs);
	sl_free(&t->touched);
	db_close(t->db);
	close(t->lockfd);
	free(t->root);
	free(t->dir);
	free(t->journal);
	free(t);
}
