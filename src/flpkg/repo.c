/* Repositories: building them (repo add), syncing their signed indexes,
 * and fetching packages from them.
 *
 * A repo is a directory or URL with packages, their .sig files, and
 * index.zst + index.zst.sig. The index holds each package's .PKGINFO plus
 * filename, csize and sha256, records separated by blank lines, newest
 * version of each name only.
 * SPDX-License-Identifier: MIT */
#include "repo.h"
#include "sign.h"
#include "txn.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static char *cache_path(const char *sub)
{
	char *c = path_join(cfg.root, cfg.cache);
	char *p = path_join(c, sub);
	free(c);
	return p;
}

/* ---- reading and writing indexes ---------------------------------------------- */

static char *read_zst(const char *path, size_t *len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	struct zreader *zr = zr_open(fd);
	struct strbuf sb = {0};
	char buf[65536];
	ssize_t n;
	while ((n = zr_read(zr, buf, sizeof(buf))) > 0)
		sb_add(&sb, buf, (size_t)n);
	zr_close(zr);
	close(fd);
	if (n < 0) {
		sb_free(&sb);
		errno = EINVAL;
		return NULL;
	}
	if (len)
		*len = sb.len;
	return sb_steal(&sb);
}

static void reposet_add(struct reposet *rs, const struct repopkg *rp)
{
	if (rs->n == rs->cap) {
		rs->cap = rs->cap ? rs->cap * 2 : 256;
		rs->v = xrealloc(rs->v, rs->cap * sizeof(*rs->v));
	}
	rs->v[rs->n++] = *rp;
}

static int parse_index(const char *text, const char *where, const char *repo, const char *url,
                       struct reposet *rs)
{
	char *copy = xstrdup(text);
	char *rec = copy;
	int count = 0;
	while (rec && *rec) {
		char *end = strstr(rec, "\n\n");
		if (end)
			*end = '\0';
		if (*trim(rec)) {
			struct repopkg rp = {.repo = repo, .url = url};
			char *err = NULL;
			if (pkginfo_parse(rec, &rp.info, &err) < 0)
				die("%s: bad record: %s", where, err);
			if (!rp.info.filename || !rp.info.sha256 || strchr(rp.info.filename, '/'))
				die("%s: record for %s lacks a valid filename or sha256", where, rp.info.name);
			reposet_add(rs, &rp);
			count++;
		}
		rec = end ? end + 2 : NULL;
	}
	free(copy);
	return count;
}

/* Load the indexes below BASE/NAME/index.zst (NULL: the synced cache). */
static void repos_load_from(struct reposet *rs, const char *base)
{
	memset(rs, 0, sizeof(*rs));
	for (size_t i = 0; i < cfg.nrepos; i++) {
		char *path;
		if (base) {
			char *sub = path_join(base, cfg.repos[i].name);
			path = path_join(sub, "index.zst");
			free(sub);
		} else {
			char *sub = xasprintf("repos/%s/index.zst", cfg.repos[i].name);
			path = cache_path(sub);
			free(sub);
		}
		char *text = read_zst(path, NULL);
		if (!text) {
			warn("no index for repo %s; run 'flpkg sync'", cfg.repos[i].name);
		} else {
			parse_index(text, path, cfg.repos[i].name, cfg.repos[i].url, rs);
			if (!base) {
				char *sigp = xasprintf("%s.sig", path);
				index_warn_cached(cfg.repos[i].name, sigp);
				free(sigp);
			}
		}
		free(text);
		free(path);
	}
}

void repos_load(struct reposet *rs)
{
	repos_load_from(rs, NULL);
}

void reposet_free(struct reposet *rs)
{
	for (size_t i = 0; i < rs->n; i++)
		pkginfo_free(&rs->v[i].info);
	free(rs->v);
	memset(rs, 0, sizeof(*rs));
}

const struct repopkg *repo_find(const struct reposet *rs, const char *name)
{
	for (size_t i = 0; i < rs->n; i++)
		if (!strcmp(rs->v[i].info.name, name))
			return &rs->v[i];
	return NULL;
}

/* ---- fetching ---------------------------------------------------------------- */

static int copy_file(const char *from, const char *to)
{
	int in = open(from, O_RDONLY | O_CLOEXEC);
	if (in < 0)
		return -1;
	char *tmp = xasprintf("%s.part", to);
	int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	int rc = out < 0 ? -1 : 0;
	char buf[65536];
	ssize_t n = 0;
	while (!rc && (n = read(in, buf, sizeof(buf))) > 0)
		rc = write_all(out, buf, (size_t)n);
	if (n < 0)
		rc = -1;
	if (out >= 0 && close(out) < 0)
		rc = -1;
	close(in);
	if (!rc)
		rc = rename(tmp, to);
	if (rc)
		unlink(tmp);
	free(tmp);
	return rc;
}

/* Fetch URL/NAME to DEST: local paths and file:// are copied, http(s)
 * goes through curl. */
static int fetch_q(const char *url, const char *name, const char *dest, bool quiet)
{
	if (has_prefix(url, "file://") || url[0] == '/') {
		char *src = path_join(has_prefix(url, "file://") ? url + 7 : url, name);
		int rc = copy_file(src, dest);
		free(src);
		return rc;
	}
	if (!have_program("curl"))
		die("fetching %s needs curl, which is not installed", url);
	char *src = xasprintf("%s/%s", url, name), *part = xasprintf("%s.part", dest);
	/* only http(s); give up on a dead connection instead of hanging */
	char *argv[] = {"curl", quiet ? "-fsL" : "-fsSL", "--proto", "=http,https", "--proto-redir", "=http,https",
	                "--connect-timeout", "20", "--speed-time", "30", "--speed-limit", "1000",
	                "--retry", "3", "-o", part, src, NULL};
	int rc = run_argv(argv, NULL, NULL) == 0 ? rename(part, dest) : -1;
	if (rc)
		unlink(part);
	free(src);
	free(part);
	return rc;
}

static int fetch(const char *url, const char *name, const char *dest)
{
	return fetch_q(url, name, dest, false);
}

/* Check FILE's signature. With TRUSTED, the verified trusted comment comes back (free it), or
 * NULL if the signature was not required and is missing. */
static void check_signature(const char *file, const char *sig, const char *name, char **trusted)
{
	if (trusted)
		*trusted = NULL;
	if (!strcmp(cfg.signatures, "never"))
		return;
	struct pubkey *keys;
	size_t n;
	keys_effective(&keys, &n);
	char *err = NULL;
	struct stat st;
	if (stat(sig, &st) < 0 && !strcmp(cfg.signatures, "optional")) {
		warn("%s is not signed", name);
	} else if (verify_file_ex(keys, n, file, sig, name, &err, trusted, NULL) < 0) {
		char *hint = keys_retired_hint();
		unlink(file);
		unlink(sig);
		die("%s%s%s", err, n ? "" : " (no trusted keys in the key directory)", hint);
	}
	pubkeys_free(keys, n);
}

char *repo_fetch(const struct repopkg *rp)
{
	char *dir = cache_path("pkg");
	if (mkdirs(dir, 0755) < 0)
		die("cannot create %s: %s", dir, strerror(errno));
	char *dest = path_join(dir, rp->info.filename);
	char *sig = xasprintf("%s.sig", dest), *signame = xasprintf("%s.sig", rp->info.filename);
	char hex[SHA256_HEX];
	bool cached = sha256_file(dest, hex) == 0 && !strcmp(hex, rp->info.sha256);
	if (!cached) {
		msg("fetching %s", rp->info.filename);
		if (fetch(rp->url, rp->info.filename, dest) < 0)
			die("cannot fetch %s from %s", rp->info.filename, rp->url);
		if (sha256_file(dest, hex) < 0 || strcmp(hex, rp->info.sha256)) {
			unlink(dest);
			die("%s: sha256 does not match the repo index", rp->info.filename);
		}
	}
	if (strcmp(cfg.signatures, "never") && fetch(rp->url, signame, sig) < 0 &&
	    !strcmp(cfg.signatures, "required"))
		die("cannot fetch the signature of %s", rp->info.filename);
	check_signature(dest, sig, rp->info.filename, NULL);
	free(signame);
	free(sig);
	free(dir);
	return dest;
}

/* ---- flpkg sync ---------------------------------------------------------------- */

/* Offer the repo's key rotation statement (rotate.pub + .sig), if it has one, to the trust code.
 * Done before the index is checked: after the switch the index is signed by the new key. */
static void offer_rotation(const struct repo_cfg *r, const char *dir)
{
	if (!strcmp(cfg.signatures, "never"))
		return;
	/* the signature names the file "rotate.pub", so it is saved under that name */
	char *pub = path_join(dir, "rotate.pub"), *sig = path_join(dir, "rotate.pub.sig");
	if (fetch_q(r->url, "rotate.pub", pub, true) == 0 &&
	    fetch_q(r->url, "rotate.pub.sig", sig, true) == 0)
		rotate_apply(r->name, pub, sig);
	unlink(pub);
	unlink(sig);
	free(pub);
	free(sig);
}

/* Fetch, verify and parse the index of R into DIR (index.zst + .sig). With PERSIST the serial
 * is remembered and learned keys are saved; without (check-updates, which may not write the
 * system) neither is. */
static int sync_repo(const struct repo_cfg *r, const char *dir, bool persist)
{
	if (mkdirs(dir, 0755) < 0)
		die("cannot create %s: %s", dir, strerror(errno));
	char *idx = path_join(dir, "index.zst.new"), *sig = path_join(dir, "index.zst.new.sig");
	if (fetch(r->url, "index.zst", idx) < 0)
		die("cannot fetch the index of %s from %s", r->name, r->url);
	if (strcmp(cfg.signatures, "never") && fetch(r->url, "index.zst.sig", sig) < 0 &&
	    !strcmp(cfg.signatures, "required"))
		die("repo %s has no signed index", r->name);
	offer_rotation(r, dir);
	char *trusted = NULL;
	check_signature(idx, sig, "index.zst", &trusted);
	long long serial = -1;
	if (trusted) {
		char *ferr = NULL;
		serial = index_check(r->name, trusted, &ferr);
		free(trusted);
		if (serial < 0) {
			unlink(idx);
			unlink(sig);
			die("%s", ferr);
		}
	}
	char *final = path_join(dir, "index.zst"), *fsig = path_join(dir, "index.zst.sig");
	struct reposet rs = {0};
	char *text = read_zst(idx, NULL);
	int count = text ? parse_index(text, idx, r->name, r->url, &rs) : -1;
	if (count < 0)
		die("%s: cannot read the index", r->name);
	if (rename(idx, final) < 0)
		die("cannot replace %s: %s", final, strerror(errno));
	rename(sig, fsig);
	if (persist && serial >= 0)
		index_commit(r->name, serial);
	reposet_free(&rs);
	free(text);
	free(final);
	free(fsig);
	free(idx);
	free(sig);
	return count;
}

int cmd_sync(void)
{
	if (!cfg.nrepos)
		die("no repos configured in flpkg.conf");
	for (size_t i = 0; i < cfg.nrepos; i++) {
		char *sub = xasprintf("repos/%s", cfg.repos[i].name);
		char *dir = cache_path(sub);
		int count = sync_repo(&cfg.repos[i], dir, true);
		msg("%s: %d packages", cfg.repos[i].name, count);
		free(dir);
		free(sub);
	}
	return 0;
}

/* ---- flpkg check-updates ------------------------------------------------------------ */

/* Machine-readable list of what 'flpkg upgrade' would do, for other tools
 * (the update notifier). One line per package, tab-separated:
 *     NAME  INSTALLED  AVAILABLE  DOWNLOAD_BYTES  REPO
 * INSTALLED is "-" for a package that is new (a dependency, or a package
 * replacing an installed one). Nothing but those lines goes to stdout.
 * By default the indexes are fetched (and their signatures checked) into a
 * private temporary directory, so it needs no write access to the system's
 * cache and never changes it; --offline uses the indexes of the last
 * 'flpkg sync' instead. --count prints only the number of lines.
 * Exit status: 0 checked (whether or not there are updates), 1 error
 * (no network, bad signature, ...), 2 bad usage. */
int cmd_check_updates(int argc, char **argv)
{
	bool offline = false, count_only = false;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--offline"))
			offline = true;
		else if (!strcmp(argv[i], "--count"))
			count_only = true;
		else
			die("usage: flpkg check-updates [--offline] [--count]");
	}
	if (!cfg.nrepos)
		die("no repos configured in flpkg.conf");
	struct reposet rs;
	char *tmp = NULL;
	if (offline) {
		repos_load(&rs);
	} else {
		const char *t = getenv("TMPDIR");
		tmp = xasprintf("%s/flpkg-check.XXXXXX", t && *t ? t : "/tmp");
		if (!mkdtemp(tmp))
			die("cannot create a temporary directory: %s", strerror(errno));
		char *scratch = path_join(tmp, ".trust");
		trust_set_scratch(scratch);
		free(scratch);
		for (size_t i = 0; i < cfg.nrepos; i++) {
			char *dir = path_join(tmp, cfg.repos[i].name);
			sync_repo(&cfg.repos[i], dir, false);
			free(dir);
		}
		repos_load_from(&rs, tmp);
		rmtree(tmp);
		free(tmp);
	}
	struct db *db = db_open(cfg.root, false);
	struct pkglist inst;
	struct solution sol;
	db_load_all(db, &inst);
	solve_upgrade(&rs, &inst, &sol);
	if (count_only)
		printf("%zu\n", sol.n);
	for (size_t i = 0; !count_only && i < sol.n; i++) {
		const struct repopkg *rp = sol.v[i].pkg;
		const char *old = "-";
		for (size_t j = 0; j < inst.n; j++)
			if (!strcmp(inst.v[j].info.name, rp->info.name))
				old = inst.v[j].info.version;
		printf("%s\t%s\t%s\t%llu\t%s\n", rp->info.name, old, rp->info.version,
		       (unsigned long long)rp->info.csize, rp->repo);
	}
	solution_free(&sol);
	pkglist_free(&inst);
	db_close(db);
	reposet_free(&rs);
	return 0;
}

/* ---- flpkg search / info --repo ---------------------------------------------------- */

int cmd_search(int argc, char **argv)
{
	if (argc < 2)
		die("usage: flpkg search TEXT...");
	struct reposet rs;
	repos_load(&rs);
	struct db *db = db_open(cfg.root, false);
	for (size_t i = 0; i < rs.n; i++) {
		const struct pkginfo *p = &rs.v[i].info;
		bool all = true;
		for (int j = 1; j < argc && all; j++)
			all = strcasestr(p->name, argv[j]) || (p->summary && strcasestr(p->summary, argv[j]));
		if (!all)
			continue;
		struct installed ip;
		bool inst = db_get(db, p->name, &ip);
		printf("%s/%s %s%s\n    %s\n", rs.v[i].repo, p->name, p->version,
		       inst ? (strcmp(ip.info.version, p->version) ? " [installed: older]" : " [installed]") : "",
		       p->summary ? p->summary : "");
		if (inst)
			installed_free(&ip);
	}
	db_close(db);
	reposet_free(&rs);
	return 0;
}

int cmd_info_repo(const char *name)
{
	struct reposet rs;
	repos_load(&rs);
	const struct repopkg *rp = repo_find(&rs, name);
	if (!rp)
		die("%s is not in any repo", name);
	printf("%-14s: %s\n", "Repository", rp->repo);
	print_pkginfo(&rp->info);
	printf("%-14s: %.1f KiB\n", "Download size", (double)rp->info.csize / 1024);
	reposet_free(&rs);
	return 0;
}

/* ---- flpkg repo add DIR FILE... ----------------------------------------------------- */

struct entry {
	struct pkginfo info;
	char *file;
};

static int cmp_entry_name(const void *a, const void *b)
{
	return strcmp(((const struct entry *)a)->info.name, ((const struct entry *)b)->info.name);
}

/* Rebuild DIR/index.zst from the packages in DIR (newest version of each
 * name) and sign it. With REMOVE_OLD, delete older package files. */
static void build_index(const char *dir, const struct seckey *key, bool remove_old, long long serial,
                        time_t valid_until)
{
	DIR *d = opendir(dir);
	if (!d)
		die("cannot read %s: %s", dir, strerror(errno));
	struct entry *v = NULL;
	size_t n = 0;
	struct dirent *de;
	while ((de = readdir(d))) {
		if (!has_suffix(de->d_name, ".flpkg"))
			continue;
		char *path = path_join(dir, de->d_name), *err = NULL;
		struct pkgfile pf;
		if (pkgfile_open(path, &pf, &err) < 0)
			die("%s", err);
		struct entry e = {pf.info, path};
		memset(&pf.info, 0, sizeof(pf.info));
		pkgfile_close(&pf);
		struct stat st;
		char hex[SHA256_HEX];
		if (stat(path, &st) < 0 || sha256_file(path, hex) < 0)
			die("cannot read %s: %s", path, strerror(errno));
		e.info.csize = (uint64_t)st.st_size;
		e.info.sha256 = xstrdup(hex);
		e.info.filename = xstrdup(de->d_name);
		v = xrealloc(v, (n + 1) * sizeof(*v));
		v[n++] = e;
	}
	closedir(d);
	if (n > 1)
		qsort(v, n, sizeof(*v), cmp_entry_name);

	struct strbuf text = {0};
	int count = 0;
	for (size_t i = 0; i < n; i++) {
		/* the newest of each name; the others are older or removed */
		size_t best = i;
		while (i + 1 < n && !strcmp(v[i + 1].info.name, v[best].info.name)) {
			i++;
			if (vercmp(v[i].info.version, v[best].info.version) > 0)
				best = i;
		}
		if (count)
			sb_addch(&text, '\n');
		pkginfo_write(&v[best].info, &text, true);
		count++;
		if (remove_old)
			for (size_t j = i + 1; j-- > 0 && !strcmp(v[j].info.name, v[best].info.name);)
				if (j != best) {
					char *sig = xasprintf("%s.sig", v[j].file);
					msg("removing old %s", v[j].info.filename);
					unlink(v[j].file);
					unlink(sig);
					free(sig);
				}
	}

	char *idx = path_join(dir, "index.zst"), *tmp = path_join(dir, ".index.zst.new");
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		die("cannot write %s: %s", tmp, strerror(errno));
	struct zwriter *zw = zw_open(fd, cfg.zstd_level, 1);
	if (zw_write(zw, text.buf, text.len) < 0 || zw_close(zw) < 0 || close(fd) < 0 ||
	    rename(tmp, idx) < 0)
		die("cannot write %s", idx);
	char *err = NULL;
	char until[32];
	iso_format(valid_until, until);
	char *extra = xasprintf("serial:%lld\tvalid-until:%s", serial, until);
	if (key && sign_file_ex(key, idx, extra, &err) < 0)
		die("%s", err);
	free(extra);
	msg("%s: %d packages%s", idx, count, key ? ", signed" : " (unsigned)");
	if (key)
		msg("index serial %lld, valid until %s", serial, until);

	for (size_t i = 0; i < n; i++) {
		pkginfo_free(&v[i].info);
		free(v[i].file);
	}
	free(v);
	sb_free(&text);
	free(idx);
	free(tmp);
}

int cmd_repo(int argc, char **argv)
{
	if (argc < 2)
		die("usage: flpkg repo add [--key FILE] [--remove-old] [--valid-days N] [--serial N] DIR [FILE.flpkg...]");
	if (strcmp(argv[1], "add"))
		die("unknown repo command '%s'", argv[1]);
	static const struct option longopts[] = {
		{"key", required_argument, NULL, 'k'},
		{"remove-old", no_argument, NULL, 'r'},
		{"valid-days", required_argument, NULL, 'd'},
		{"serial", required_argument, NULL, 's'},
		{0},
	};
	long valid_days = 14; /* how long clients accept this index: sign again before it runs out */
	long long serial = -1;
	const char *keypath = cfg.signkey;
	bool remove_old = false;
	int c;
	optind = 2;
	while ((c = getopt_long(argc, argv, "", longopts, NULL)) != -1) {
		switch (c) {
		case 'k': keypath = optarg; break;
		case 'r': remove_old = true; break;
		case 'd': valid_days = strtol(optarg, NULL, 10); break; /* negative: expired, for tests */
		case 's': serial = strtoll(optarg, NULL, 10); break;
		default: return 2;
		}
	}
	if (optind >= argc)
		die("usage: flpkg repo add [--key FILE] [--remove-old] [--valid-days N] [--serial N] DIR [FILE.flpkg...]");
	const char *dir = argv[optind++];
	if (mkdirs(dir, 0755) < 0)
		die("cannot create %s: %s", dir, strerror(errno));

	struct seckey key;
	char *err = NULL;
	if (keypath && seckey_load(keypath, &key, &err) < 0)
		die("%s", err);
	if (!keypath)
		warn("no signing key (signkey in flpkg.conf or --key): the repo will be unsigned");

	for (int i = optind; i < argc; i++) {
		const char *base = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
		if (!has_suffix(base, ".flpkg"))
			die("%s is not a .flpkg file", argv[i]);
		char *dest = path_join(dir, base);
		char *rs = realpath(argv[i], NULL), *rd = realpath(dest, NULL);
		bool same = rs && rd && !strcmp(rs, rd);
		if (!same && copy_file(argv[i], dest) < 0)
			die("cannot copy %s into %s: %s", argv[i], dir, strerror(errno));
		if (keypath && sign_file(&key, dest, &err) < 0)
			die("%s", err);
		free(rs);
		free(rd);
		free(dest);
	}
	if (serial < 0)
		serial = index_next_serial(dir); /* above the previous index, and at least the clock */
	build_index(dir, keypath ? &key : NULL, remove_old, serial, time(NULL) + (time_t)valid_days * 86400);
	return 0;
}

int cmd_key(int argc, char **argv)
{
	if (argc >= 2 && !strcmp(argv[1], "introduce"))
		return cmd_key_introduce(argc, argv);
	if (argc == 2 && !strcmp(argv[1], "list"))
		return cmd_key_list();
	if (argc != 3 || strcmp(argv[1], "generate"))
		die("usage: flpkg key generate PATH   (writes PATH.key and PATH.pub)\n"
		    "       flpkg key introduce --key OLD.key --new NEW.pub [--retire-days N] DIR\n"
		    "       flpkg key list");
	key_generate(argv[2]);
	return 0;
}
