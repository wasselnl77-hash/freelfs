/* Pre-transaction hooks: before flpkg changes anything it runs every
 * executable in /usr/libexec/flpkg/pre-transaction.d (sorted by name). The
 * FreeLFS one, 10-flsnap from the flsnap package, takes a btrfs snapshot of /
 * (see src/flsnap). A hook that fails is a warning, not a reason to stop.
 *
 * The hooks get FLPKG_ROOT, FLPKG_SUMMARY ("upgrade foo, remove bar"),
 * FLPKG_PACKAGES (a file of "name version" lines: what is installed now, which
 * is only valid while the hook runs) and FLPKG_SNAPSHOTS ("snapshots =" of
 * flpkg.conf; 0 turns the snapshot hook off, as on the live image).
 * Only for the real root; for another --root only if FLPKG_HOOKDIR names
 * a directory (tests).
 * SPDX-License-Identifier: MIT */
#include "txn.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HOOKDIR "/usr/libexec/flpkg/pre-transaction.d"

static int cmp_name(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* what is installed, as a temporary file of "name version" lines; NULL if none */
static char *write_package_list(void)
{
	struct db *db = db_open(cfg.root, false);
	if (!db)
		return NULL;
	struct pkglist all = {0};
	db_load_all(db, &all);
	db_close(db);
	struct strbuf sb = {0};
	for (size_t i = 0; i < all.n; i++)
		sb_addf(&sb, "%s %s\n", all.v[i].info.name, all.v[i].info.version);
	pkglist_free(&all);
	char *path = xstrdup(access("/run", W_OK) == 0 ? "/run/flpkg-packages.XXXXXX" : "/tmp/flpkg-packages.XXXXXX");
	int fd = mkstemp(path);
	if (fd < 0 || write_all(fd, sb.buf ? sb.buf : "", sb.len) < 0) {
		if (fd >= 0) {
			close(fd);
			unlink(path);
		}
		free(path);
		path = NULL;
	} else {
		close(fd);
	}
	sb_free(&sb);
	return path;
}

void snapshot_before(const char *summary)
{
	const char *dir = getenv("FLPKG_HOOKDIR");
	if (!dir)
		dir = !strcmp(cfg.root, "/") ? HOOKDIR : NULL;
	DIR *d = dir ? opendir(dir) : NULL;
	if (!d)
		return;
	struct strlist hooks = {0};
	struct dirent *de;
	while ((de = readdir(d))) {
		char *p = xasprintf("%s/%s", dir, de->d_name);
		struct stat st;
		if (de->d_name[0] != '.' && stat(p, &st) == 0 && S_ISREG(st.st_mode) && access(p, X_OK) == 0)
			sl_add_owned(&hooks, p);
		else
			free(p);
	}
	closedir(d);
	if (!hooks.n) {
		sl_free(&hooks);
		return;
	}
	qsort(hooks.v, hooks.n, sizeof(char *), cmp_name);

	char *pkgs = write_package_list();
	char *snaps = xasprintf("%d", cfg.snapshots);
	setenv("FLPKG_ROOT", cfg.root, 1);
	setenv("FLPKG_SUMMARY", summary, 1);
	setenv("FLPKG_SNAPSHOTS", snaps, 1);
	if (pkgs)
		setenv("FLPKG_PACKAGES", pkgs, 1);
	else
		unsetenv("FLPKG_PACKAGES");
	for (size_t i = 0; i < hooks.n; i++) {
		char *argv[] = {hooks.v[i], NULL};
		if (run_argv(argv, NULL, NULL) != 0)
			warn("pre-transaction hook %s failed (going on)", hooks.v[i]);
	}
	if (pkgs)
		unlink(pkgs);
	unsetenv("FLPKG_PACKAGES");
	free(pkgs);
	free(snaps);
	sl_free(&hooks);
}

/* "flpkg snapshots": what flsnap list says */
int cmd_snapshots(void)
{
	char *argv[] = {"flsnap", "list", NULL};
	int rc = run_argv(argv, NULL, NULL);
	if (rc == 127 || rc < 0) {
		puts("flsnap is not installed");
		return 1;
	}
	return rc;
}
