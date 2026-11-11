/* Package commands: install, remove, and the queries on installed packages.
 * SPDX-License-Identifier: MIT */
#include "repo.h"
#include "txn.h"

#include <errno.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>



static bool confirm(bool yes)
{
	if (yes || !isatty(STDIN_FILENO))
		return true;
	fputs(":: Proceed? [Y/n] ", stdout);
	fflush(stdout);
	char buf[16];
	if (!fgets(buf, sizeof(buf), stdin))
		return false;
	return buf[0] == '\n' || buf[0] == 'y' || buf[0] == 'Y';
}

/* ---- install ---------------------------------------------------------------- */

int cmd_install(int argc, char **argv)
{
	static const struct option longopts[] = {
		{"nodeps", no_argument, NULL, 'd'},   {"yes", no_argument, NULL, 'y'},
		{"asdeps", no_argument, NULL, 'a'},   {"overwrite", required_argument, NULL, 'o'},
		{"needed", no_argument, NULL, 'n'},   {0},
	};
	struct txn_opts opts = {0};
	bool yes = false, asdeps = false, needed = false;
	int c;
	optind = 1;
	while ((c = getopt_long(argc, argv, "y", longopts, NULL)) != -1) {
		switch (c) {
		case 'd': opts.nodeps = true; break;
		case 'y': yes = true; break;
		case 'a': asdeps = true; break;
		case 'o': sl_add(&opts.overwrite, optarg); break;
		case 'n': needed = true; break;
		default: return 2;
		}
	}
	if (optind == argc)
		die("usage: flpkg install [--nodeps] [--asdeps] [--needed] [--overwrite GLOB] FILE.flpkg...");

	struct txn *t = txn_new(cfg.root, &opts);

	/* Names go through the repos; files are installed as they are. */
	struct strlist names = {0};
	for (int i = optind; i < argc; i++)
		if (!has_suffix(argv[i], ".flpkg"))
			sl_add(&names, argv[i]);
	if (names.n) {
		struct reposet rs;
		struct pkglist inst;
		struct solution sol;
		repos_load(&rs);
		db_load_all(txn_db(t), &inst);
		solve_install(&rs, &inst, names.v, names.n, needed, &sol);
		for (size_t i = 0; i < sol.remove.n; i++)
			txn_remove(t, sol.remove.v[i]);
		for (size_t i = 0; i < sol.n; i++) {
			char *file = repo_fetch(sol.v[i].pkg);
			txn_add_file(t, file, asdeps ? "dep" : sol.v[i].reason);
			free(file);
		}
		solution_free(&sol);
		pkglist_free(&inst);
		reposet_free(&rs);
	}
	sl_free(&names);

	for (int i = optind; i < argc; i++) {
		if (!has_suffix(argv[i], ".flpkg"))
			continue;
		if (needed) {
			struct pkgfile pf;
			char *err = NULL;
			if (pkgfile_open(argv[i], &pf, &err) < 0)
				die("%s", err);
			struct installed ip;
			bool same = db_get(txn_db(t), pf.info.name, &ip) &&
			            !strcmp(ip.info.version, pf.info.version);
			if (same) {
				msg("%s %s is already installed", pf.info.name, pf.info.version);
				installed_free(&ip);
			}
			pkgfile_close(&pf);
			if (same)
				continue;
		}
		txn_add_file(t, argv[i], asdeps ? "dep" : "explicit");
	}
	if (txn_empty(t)) {
		txn_free(t);
		return 0;
	}
	msg("transaction:");
	txn_print_plan(t, stdout);
	if (!confirm(yes)) {
		txn_free(t);
		return 1;
	}
	txn_commit(t);
	txn_free(t);
	msg("done");
	sl_free(&opts.overwrite);
	return 0;
}

/* ---- upgrade ------------------------------------------------------------------ */

int cmd_upgrade(int argc, char **argv)
{
	static const struct option longopts[] = {
		{"sync", no_argument, NULL, 's'}, {"yes", no_argument, NULL, 'y'},
		{"nodeps", no_argument, NULL, 'd'}, {0},
	};
	struct txn_opts opts = {0};
	bool sync = false, yes = false;
	int c;
	optind = 1;
	while ((c = getopt_long(argc, argv, "y", longopts, NULL)) != -1) {
		switch (c) {
		case 's': sync = true; break;
		case 'y': yes = true; break;
		case 'd': opts.nodeps = true; break;
		default: return 2;
		}
	}
	if (sync)
		cmd_sync();
	struct txn *t = txn_new(cfg.root, &opts);
	struct reposet rs;
	struct pkglist inst;
	struct solution sol;
	repos_load(&rs);
	db_load_all(txn_db(t), &inst);
	solve_upgrade(&rs, &inst, &sol);
	int rc = 0;
	if (!sol.n) {
		msg("everything is up to date");
	} else {
		for (size_t i = 0; i < sol.remove.n; i++)
			txn_remove(t, sol.remove.v[i]);
		for (size_t i = 0; i < sol.n; i++) {
			char *file = repo_fetch(sol.v[i].pkg);
			txn_add_file(t, file, sol.v[i].reason);
			free(file);
		}
		msg("transaction:");
		txn_print_plan(t, stdout);
		if (confirm(yes)) {
			txn_commit(t);
			msg("done");
		} else {
			rc = 1;
		}
	}
	solution_free(&sol);
	pkglist_free(&inst);
	reposet_free(&rs);
	txn_free(t);
	return rc;
}

/* ---- remove ------------------------------------------------------------------ */

static bool in_list(const struct strlist *l, const char *s)
{
	return sl_has(l, s);
}

/* Does any package in ALL, other than those in GONE, satisfy D? */
static bool still_satisfied(const struct dep *d, const struct pkglist *all,
                            const struct strlist *gone)
{
	for (size_t i = 0; i < all->n; i++) {
		const struct pkginfo *p = &all->v[i].info;
		if (!in_list(gone, p->name) && dep_satisfied_by(d, p->name, p->version, &p->provide))
			return true;
	}
	return false;
}

/* Is any remaining package depending on P (and only P provides it)? */
static bool needed_by_others(const struct pkginfo *p, const struct pkglist *all,
                             const struct strlist *gone)
{
	for (size_t i = 0; i < all->n; i++) {
		const struct pkginfo *q = &all->v[i].info;
		if (in_list(gone, q->name))
			continue;
		for (size_t j = 0; j < q->depend.n; j++) {
			struct dep d;
			if (dep_parse(q->depend.v[j], &d) < 0)
				continue;
			bool hit = dep_satisfied_by(&d, p->name, p->version, &p->provide) &&
			           !still_satisfied(&d, all, gone);
			dep_free(&d);
			if (hit)
				return true;
		}
	}
	return false;
}

int cmd_remove(int argc, char **argv)
{
	static const struct option longopts[] = {
		{"cascade", no_argument, NULL, 'c'}, {"recursive", no_argument, NULL, 'r'},
		{"nodeps", no_argument, NULL, 'd'},  {"yes", no_argument, NULL, 'y'},
		{0},
	};
	struct txn_opts opts = {0};
	bool cascade = false, recursive = false, yes = false;
	int c;
	optind = 1;
	while ((c = getopt_long(argc, argv, "y", longopts, NULL)) != -1) {
		switch (c) {
		case 'c': cascade = true; break;
		case 'r': recursive = true; break;
		case 'd': opts.nodeps = true; break;
		case 'y': yes = true; break;
		default: return 2;
		}
	}
	if (optind == argc)
		die("usage: flpkg remove [--cascade] [--recursive] [--nodeps] PKG...");

	struct txn *t = txn_new(cfg.root, &opts);
	struct pkglist all;
	db_load_all(txn_db(t), &all);
	struct strlist gone = {0};
	for (int i = optind; i < argc; i++) {
		struct installed ip;
		if (!db_get(txn_db(t), argv[i], &ip))
			die("%s is not installed", argv[i]);
		installed_free(&ip);
		if (!in_list(&gone, argv[i]))
			sl_add(&gone, argv[i]);
	}

	/* --cascade: also everything that would be left without a dependency. */
	for (bool changed = cascade; changed;) {
		changed = false;
		for (size_t i = 0; i < all.n; i++) {
			const struct pkginfo *p = &all.v[i].info;
			if (in_list(&gone, p->name))
				continue;
			for (size_t j = 0; j < p->depend.n && !in_list(&gone, p->name); j++) {
				struct dep d;
				if (dep_parse(p->depend.v[j], &d) < 0)
					continue;
				if (!still_satisfied(&d, &all, &gone)) {
					sl_add(&gone, p->name);
					changed = true;
				}
				dep_free(&d);
			}
		}
	}
	/* --recursive: also dependencies nothing else needs any more. */
	for (bool changed = recursive; changed;) {
		changed = false;
		for (size_t i = 0; i < all.n; i++) {
			const struct installed *ip = &all.v[i];
			if (in_list(&gone, ip->info.name) || strcmp(ip->reason, "dep"))
				continue;
			bool wanted_before = false;
			for (size_t k = 0; k < gone.n && !wanted_before; k++)
				for (size_t m = 0; m < all.n; m++)
					if (!strcmp(all.v[m].info.name, gone.v[k]))
						for (size_t j = 0; j < all.v[m].info.depend.n; j++) {
							struct dep d;
							if (dep_parse(all.v[m].info.depend.v[j], &d) < 0)
								continue;
							wanted_before |= dep_satisfied_by(&d, ip->info.name,
							                                  ip->info.version,
							                                  &ip->info.provide);
							dep_free(&d);
						}
			if (wanted_before && !needed_by_others(&ip->info, &all, &gone)) {
				sl_add(&gone, ip->info.name);
				changed = true;
			}
		}
	}

	for (size_t i = 0; i < gone.n; i++)
		txn_remove(t, gone.v[i]);
	msg("transaction:");
	txn_print_plan(t, stdout);
	int rc = 0;
	if (confirm(yes)) {
		txn_commit(t);
		msg("done");
	} else {
		rc = 1;
	}
	txn_free(t);
	pkglist_free(&all);
	sl_free(&gone);
	return rc;
}

/* ---- queries ------------------------------------------------------------------- */

static struct db *open_ro(void)
{
	txn_recover_pending(cfg.root);
	return db_open(cfg.root, false);
}

int cmd_list(int argc, char **argv)
{
	bool explicit_only = argc > 1 && !strcmp(argv[1], "--explicit");
	bool deps_only = argc > 1 && !strcmp(argv[1], "--deps");
	struct db *db = open_ro();
	struct pkglist all;
	db_load_all(db, &all);
	for (size_t i = 0; i < all.n; i++) {
		bool dep = !strcmp(all.v[i].reason, "dep");
		if ((explicit_only && dep) || (deps_only && !dep))
			continue;
		printf("%s %s\n", all.v[i].info.name, all.v[i].info.version);
	}
	pkglist_free(&all);
	db_close(db);
	return 0;
}

int cmd_info_installed(const char *name)
{
	struct db *db = open_ro();
	struct installed ip;
	if (!db_get(db, name, &ip))
		die("%s is not installed", name);
	print_pkginfo(&ip.info);
	char when[64];
	time_t t = (time_t)ip.installdate;
	strftime(when, sizeof(when), "%Y-%m-%d %H:%M", localtime(&t));
	printf("%-14s: %s\n", "Installed", when);
	printf("%-14s: %s\n", "Reason", !strcmp(ip.reason, "dep") ? "dependency" : "explicitly installed");
	installed_free(&ip);
	db_close(db);
	return 0;
}

int cmd_files_installed(const char *name)
{
	struct db *db = open_ro();
	struct installed ip;
	if (!db_get(db, name, &ip))
		die("%s is not installed", name);
	struct manifest m;
	db_files(db, name, &m);
	for (size_t i = 0; i < m.n; i++)
		printf("%s /%s%s\n", name, m.v[i].path, m.v[i].type == 'd' ? "/" : "");
	manifest_free(&m);
	installed_free(&ip);
	db_close(db);
	return 0;
}

int cmd_owns(int argc, char **argv)
{
	if (argc < 2)
		die("usage: flpkg owns PATH...");
	struct db *db = open_ro();
	int rc = 0;
	for (int i = 1; i < argc; i++) {
		const char *p = argv[i];
		while (*p == '/')
			p++;
		char *rel = xstrdup(p);
		size_t n = strlen(rel);
		while (n > 1 && rel[n - 1] == '/')
			rel[--n] = '\0';
		char *owner = db_file_owner(db, rel);
		if (owner) {
			printf("/%s is owned by %s\n", rel, owner);
			free(owner);
		} else {
			struct strlist owners;
			db_dir_owners(db, rel, &owners);
			if (owners.n) {
				printf("/%s/ is owned by", rel);
				for (size_t j = 0; j < owners.n; j++)
					printf(" %s", owners.v[j]);
				putchar('\n');
			} else {
				fprintf(stderr, "%s: no package owns /%s\n", progname, rel);
				rc = 1;
			}
			sl_free(&owners);
		}
		free(rel);
	}
	db_close(db);
	return rc;
}

/* FAT (the EFI system partition) has no Unix owners or modes to compare. */
static bool on_fat(const char *abs)
{
	struct statfs sf;
	return statfs(abs, &sf) == 0 && sf.f_type == 0x4d44; /* MSDOS_SUPER_MAGIC */
}

/* A directory something is mounted on shows the mounted file system's
 * permissions (/proc, /sys), not its own. */
static bool is_mountpoint(const char *abs, const struct stat *st)
{
	char *parent = xasprintf("%s/..", abs);
	struct stat pst;
	bool mp = stat(parent, &pst) == 0 && (pst.st_dev != st->st_dev || pst.st_ino == st->st_ino);
	free(parent);
	return mp;
}

static int verify_one(struct db *db, const char *name, const struct manifest *pending)
{
	struct manifest m;
	db_files(db, name, &m);
	int problems = 0;
	for (size_t i = 0; i < m.n; i++) {
		const struct manifest_entry *e = &m.v[i];
		char *abs = path_join(cfg.root, e->path);
		struct stat st;
		const char *what = NULL;
		if (lstat(abs, &st) < 0) {
			what = "missing";
		} else if (e->type == 'd' && S_ISLNK(st.st_mode)) {
			/* a directory may be a link to one (the /sysflfs layout) */
			struct stat t;
			if (stat(abs, &t) < 0 || !S_ISDIR(t.st_mode))
				what = "wrong type";
			free(abs);
			if (what) {
				printf("%s: /%s: %s\n", name, e->path, what);
				problems++;
			}
			continue;
		} else if (e->type == 'd' ? !S_ISDIR(st.st_mode)
		           : e->type == 'l' ? !S_ISLNK(st.st_mode) : !S_ISREG(st.st_mode)) {
			what = "wrong type";
		} else if (e->type == 'l') {
			char target[4096];
			ssize_t n = readlink(abs, target, sizeof(target) - 1);
			target[n < 0 ? 0 : n] = '\0';
			if (n < 0 || strcmp(target, e->target))
				what = "symlink target changed";
		} else if (!manifest_find(pending, e->path) &&
		           !(e->type == 'd' && is_mountpoint(abs, &st)) && !on_fat(abs) &&
		           ((st.st_mode & 07777) != e->mode || st.st_uid != e->uid || st.st_gid != e->gid)) {
			what = "permissions or owner changed";
		} else if (e->type == 'f') {
			char hex[SHA256_HEX];
			if ((uint64_t)st.st_size != e->size || sha256_file(abs, hex) < 0 ||
			    strcmp(hex, e->sha256))
				what = has_prefix(e->path, "etc/") ? NULL : "contents changed";
		}
		if (what) {
			printf("%s: /%s: %s\n", name, e->path, what);
			problems++;
		}
		free(abs);
	}
	manifest_free(&m);
	return problems;
}

int cmd_verify(int argc, char **argv)
{
	struct db *db = open_ro();
	struct manifest pending;
	db_pending(db, &pending);
	manifest_sort(&pending);
	int problems = 0;
	if (argc > 1) {
		for (int i = 1; i < argc; i++)
			problems += verify_one(db, argv[i], &pending);
	} else {
		struct pkglist all;
		db_load_all(db, &all);
		for (size_t i = 0; i < all.n; i++)
			problems += verify_one(db, all.v[i].info.name, &pending);
		pkglist_free(&all);
	}
	if (pending.n)
		printf("%zu files wait for 'flpkg fix-owners'\n", pending.n);
	manifest_free(&pending);
	db_close(db);
	printf("%d problems found (changed files in /etc are expected and not listed)\n", problems);
	return problems ? 1 : 0;
}

int cmd_history(void)
{
	struct db *db = open_ro();
	db_history_print(db, stdout);
	db_close(db);
	return 0;
}

/* Nothing deferred: the database is not opened for writing at all (every boot runs this; on the live
 * system a write copies the whole 14 MB database into RAM). --keep (the live system): the owners are
 * set but the list is left as it is, so the database stays read-only and in the squashfs. */
int cmd_fix_owners(int argc, char **argv)
{
	bool keep = argc > 1 && !strcmp(argv[1], "--keep");
	if (argc > 1 && !keep)
		die("usage: flpkg fix-owners [--keep]");
	struct manifest pending;
	struct db *ro = db_open(cfg.root, false);
	if (!ro)
		return 0; /* no database: nothing is installed */
	db_pending(ro, &pending);
	size_t todo = pending.n;
	manifest_free(&pending);
	if (!todo) {
		db_close(ro);
		return 0;
	}
	struct txn *t = NULL;
	struct db *db = ro;
	if (!keep) {
		db_close(ro);
		t = txn_new(cfg.root, NULL); /* for the lock */
		db = txn_db(t);
	}
	db_pending(db, &pending);
	int fixed = 0, failed = 0;
	for (size_t i = 0; i < pending.n; i++) {
		const struct manifest_entry *e = &pending.v[i];
		char *abs = path_join(cfg.root, e->path);
		struct stat st;
		if (lstat(abs, &st) < 0) {
			if (!keep)
				db_drop_pending(db, e->path); /* gone: nothing to fix */
		} else if (lchown(abs, e->uid, e->gid) == 0 &&
		           (S_ISLNK(st.st_mode) || chmod(abs, e->mode & 07777) == 0)) {
			if (!keep)
				db_drop_pending(db, e->path);
			fixed++;
		} else {
			warn("cannot set owner of /%s: %s", e->path, strerror(errno));
			failed++;
		}
		free(abs);
	}
	manifest_free(&pending);
	if (t)
		txn_free(t);
	else
		db_close(db);
	if (fixed || failed)
		msg("set the owner of %d files%s", fixed, failed ? " (some failed)" : "");
	return failed ? 1 : 0;
}
