/* flpkg build: recipe -> packages.
 *
 * Reads the recipe through recipe.sh, finds and verifies the sources,
 * unpacks them, runs prepare/build/check and one package function per
 * package name in a clean environment, cleans up each package root (strip,
 * .la files, forbidden paths) and archives it.
 * SPDX-License-Identifier: MIT */
#include "flpkg.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <getopt.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct recipe {
	char *pkgbase, *version, *release, *summary, *url, *worksrc;
	struct strlist pkgname, arch, license, groups, source, sha256, depends, makedepends,
		checkdepends, provides, conflicts, replaces, sysusers, sysgroups, owner, options,
		function;
};

#define S(f) {#f, offsetof(struct recipe, f), false}
#define L(f) {#f, offsetof(struct recipe, f), true}
static const struct {
	const char *key;
	size_t off;
	bool list;
} fields[] = {
	S(pkgbase), S(version), S(release), S(summary), S(url), S(worksrc),
	L(pkgname), L(arch), L(license), L(groups), L(source), L(sha256), L(depends),
	L(makedepends), L(checkdepends), L(provides), L(conflicts), L(replaces),
	L(sysusers), L(sysgroups), L(owner), L(options), L(function),
};
#undef S
#undef L

static void recipe_free(struct recipe *r)
{
	for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
		void *p = (char *)r + fields[i].off;
		if (fields[i].list)
			sl_free(p);
		else
			free(*(char **)p);
	}
	memset(r, 0, sizeof(*r));
}

/* "key\tvalue\0" records from recipe.sh */
static int parse_vars(const char *buf, size_t len, struct recipe *r, char **error)
{
	memset(r, 0, sizeof(*r));
	const char *end = buf + len;
	while (buf < end) {
		const char *rec_end = memchr(buf, '\0', (size_t)(end - buf));
		if (!rec_end)
			rec_end = end;
		const char *tab = memchr(buf, '\t', (size_t)(rec_end - buf));
		if (!tab) {
			*error = xstrdup("malformed output from recipe.sh");
			return -1;
		}
		size_t klen = (size_t)(tab - buf);
		for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
			if (strlen(fields[i].key) != klen || memcmp(fields[i].key, buf, klen))
				continue;
			char *v = xstrndup(tab + 1, (size_t)(rec_end - tab - 1));
			void *p = (char *)r + fields[i].off;
			if (fields[i].list) {
				sl_add_owned(p, v);
			} else {
				free(*(char **)p);
				*(char **)p = v;
			}
		}
		buf = rec_end + 1;
	}
	return 0;
}

static bool option_off(const struct recipe *r, const char *opt)
{
	for (size_t i = 0; i < r->options.n; i++)
		if (r->options.v[i][0] == '!' && !strcmp(r->options.v[i] + 1, opt))
			return true;
	return false;
}

static bool valid_name(const char *s)
{
	if (!*s || *s == '-' || *s == '.')
		return false;
	for (; *s; s++)
		if (!isalnum((unsigned char)*s) && !strchr("+-._@", *s))
			return false;
	return true;
}

static void validate(const struct recipe *r, const char *arch)
{
	if (!r->pkgname.n)
		die("recipe: pkgname is not set");
	for (size_t i = 0; i < r->pkgname.n; i++)
		if (!valid_name(r->pkgname.v[i]))
			die("recipe: invalid package name '%s'", r->pkgname.v[i]);
	if (!r->version || !*r->version || strpbrk(r->version, "-: /"))
		die("recipe: version must be set and contain no '-', ':', '/' or spaces");
	if (!r->release || !*r->release || strspn(r->release, "0123456789") != strlen(r->release))
		die("recipe: release must be a number");
	if (!r->summary)
		die("recipe: summary is not set");
	if (r->source.n != r->sha256.n)
		die("recipe: %zu sources but %zu sha256 sums", r->source.n, r->sha256.n);
	for (size_t i = 0; i < r->sha256.n; i++) {
		const char *h = r->sha256.v[i];
		if (strlen(h) != 64 || strspn(h, "0123456789abcdef") != 64)
			die("recipe: sha256 #%zu is not a lowercase SHA-256 hex sum", i + 1);
	}
	if (!sl_has(&r->arch, arch) && !sl_has(&r->arch, "any"))
		die("recipe: does not build for %s (arch=%s)", arch,
		    r->arch.n ? r->arch.v[0] : "unset");
	for (size_t i = 0; i < r->pkgname.n; i++) {
		char *f = xasprintf("package_%s", r->pkgname.v[i]);
		bool ok = sl_has(&r->function, f) ||
		          (r->pkgname.n == 1 && sl_has(&r->function, "package"));
		if (!ok)
			die("recipe: no %s() for package %s",
			    r->pkgname.n == 1 ? "package" : f, r->pkgname.v[i]);
		free(f);
	}
}

/* ---- the build environment ------------------------------------------------ */

struct env {
	struct strlist v;
};

static void env_set(struct env *e, const char *k, const char *v)
{
	if (v)
		sl_add_owned(&e->v, xasprintf("%s=%s", k, v));
}

static char **env_array(struct env *e)
{
	char **a = xcalloc(e->v.n + 1, sizeof(char *));
	memcpy(a, e->v.v, e->v.n * sizeof(char *));
	return a;
}

static char *helper_path(void)
{
	return path_join(cfg.libdir, "recipe.sh");
}

static int run_helper(struct env *env, const char *cmd, const char *recipe, const char *func,
                      const char *out, struct strbuf *capture)
{
	char *helper = helper_path();
	char *argv[] = {"bash", "--noprofile", "--norc", helper, (char *)cmd, (char *)recipe,
	                (char *)func, (char *)out, NULL};
	if (!func)
		argv[6] = NULL;
	else if (!out)
		argv[7] = NULL;
	char **envp = env_array(env);
	int rc = capture ? run_capture(argv, envp, NULL, capture) : run_argv(argv, envp, NULL);
	free(envp);
	free(helper);
	return rc;
}

/* ---- sources ---------------------------------------------------------------- */

struct source {
	char *name; /* file name in $srcdir */
	char *path; /* where it is on disk */
};

static bool is_url(const char *s)
{
	return strstr(s, "://") != NULL;
}

static char *find_in_dirs(const struct strlist *dirs, const char *name)
{
	for (size_t i = 0; i < dirs->n; i++) {
		char *p = path_join(dirs->v[i], name);
		if (access(p, R_OK) == 0)
			return p;
		free(p);
	}
	return NULL;
}

static char *download(const char *url, const char *name, const char *dir)
{
	if (has_prefix(url, "file://"))
		return xstrdup(url + 7);
	if (!have_program("curl"))
		die("%s is not in the source cache and curl is not installed to fetch it\n"
		    "  (put it in %s or pass --sources DIR)", name, dir);
	if (mkdirs(dir, 0755) < 0)
		die("cannot create %s: %s", dir, strerror(errno));
	char *dest = path_join(dir, name), *part = xasprintf("%s.part", dest);
	char *argv[] = {"curl", "-fL", "--retry", "3", "-o", part, (char *)url, NULL};
	msg("downloading %s", name);
	if (run_argv(argv, NULL, NULL) != 0 || rename(part, dest) < 0) {
		unlink(part);
		die("download failed: %s", url);
	}
	free(part);
	return dest;
}

static struct source *resolve_sources(const struct recipe *r, const char *recipedir,
                                      const struct strlist *dirs, const char *cachedir)
{
	struct source *s = xcalloc(r->source.n + 1, sizeof(*s));
	for (size_t i = 0; i < r->source.n; i++) {
		const char *src = r->source.v[i], *sep = strstr(src, "::");
		const char *url = sep ? sep + 2 : src;
		if (is_url(url)) {
			const char *base = strrchr(url, '/');
			s[i].name = sep ? xstrndup(src, (size_t)(sep - src)) : xstrdup(base + 1);
			if (!*s[i].name)
				die("source %s has no file name", url);
			s[i].path = find_in_dirs(dirs, s[i].name);
			if (!s[i].path)
				s[i].path = download(url, s[i].name, cachedir);
		} else {
			if (strchr(src, '/'))
				die("local source %s must be a plain file name next to the recipe", src);
			s[i].name = xstrdup(src);
			s[i].path = path_join(recipedir, src);
		}
		char hex[SHA256_HEX];
		if (sha256_file(s[i].path, hex) < 0)
			die("cannot read %s: %s", s[i].path, strerror(errno));
		if (strcmp(hex, r->sha256.v[i]))
			die("%s: sha256 mismatch\n  expected %s\n  got      %s", s[i].name,
			    r->sha256.v[i], hex);
	}
	return s;
}

static bool is_archive(const char *name)
{
	static const char *ext[] = {".tar", ".tar.gz", ".tgz", ".tar.xz", ".txz", ".tar.bz2",
	                            ".tbz2", ".tar.zst", ".tar.lz", NULL};
	for (int i = 0; ext[i]; i++)
		if (has_suffix(name, ext[i]))
			return true;
	return false;
}

static void unpack_sources(const struct source *s, size_t n, const char *srcdir)
{
	for (size_t i = 0; i < n; i++) {
		char *link = path_join(srcdir, s[i].name);
		char *abs = realpath(s[i].path, NULL);
		if (!abs || symlink(abs, link) < 0)
			die("cannot link %s into %s: %s", s[i].name, srcdir, strerror(errno));
		free(abs);
		free(link);
		if (!is_archive(s[i].name))
			continue;
		char *argv[] = {"tar", "--no-same-owner", "-xf", s[i].path, "-C", (char *)srcdir, NULL};
		msg("unpacking %s", s[i].name);
		if (run_argv(argv, NULL, NULL) != 0)
			die("cannot unpack %s", s[i].name);
	}
}

/* ---- package clean-up -------------------------------------------------------- */

static struct {
	bool strip, purge_la;
	int stripped, la;
	struct {
		dev_t dev;
		ino_t ino;
	} *seen; /* hard-linked files already stripped */
	size_t nseen;
} post;

static bool seen_before(const struct stat *st)
{
	if (st->st_nlink < 2)
		return false;
	for (size_t i = 0; i < post.nseen; i++)
		if (post.seen[i].dev == st->st_dev && post.seen[i].ino == st->st_ino)
			return true;
	post.seen = xrealloc(post.seen, (post.nseen + 1) * sizeof(*post.seen));
	post.seen[post.nseen].dev = st->st_dev;
	post.seen[post.nseen].ino = st->st_ino;
	post.nseen++;
	return false;
}

static void strip_file(const char *path, const char *how)
{
	struct stat st;
	if (lstat(path, &st) < 0)
		return;
	char *argv[] = {"strip", (char *)how, (char *)path, NULL};
	if (run_argv(argv, NULL, NULL) != 0)
		warn("could not strip %s", path);
	else
		post.stripped++;
	chmod(path, st.st_mode & 07777); /* strip may drop setuid bits */
}

static int post_one(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
	(void)ftw;
	if (type != FTW_F || !S_ISREG(st->st_mode))
		return 0;
	if (post.purge_la && has_suffix(path, ".la")) {
		unlink(path);
		post.la++;
		return 0;
	}
	if (!post.strip || has_suffix(path, ".dbg") || seen_before(st))
		return 0;
	unsigned char h[20];
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return 0;
	ssize_t n = read_full(fd, h, sizeof(h));
	close(fd);
	if (n >= 8 && !memcmp(h, "!<arch>\n", 8)) {
		strip_file(path, "--strip-debug");
	} else if (n >= 18 && !memcmp(h, "\177ELF", 4)) {
		unsigned type16 = h[5] == 2 ? (unsigned)(h[16] << 8 | h[17])
		                            : (unsigned)(h[17] << 8 | h[16]);
		strip_file(path, type16 == 1 ? "--strip-debug" : "--strip-unneeded");
	}
	return 0;
}

static const char *forbidden[] = {
	"bin", "lib", "sbin", "usr/local", "var/run", "var/lock", "run", "tmp", "dev",
	"proc", "sys", "home", "root", "mnt", "media", "lost+found", NULL,
};

static void clean_package(const struct recipe *r, const char *pkgdir, const char *name)
{
	/* only the base file system package may own /bin, /tmp and the like */
	for (int i = 0; forbidden[i] && !option_off(r, "pathcheck"); i++) {
		char *p = path_join(pkgdir, forbidden[i]);
		struct stat st;
		if (lstat(p, &st) == 0)
			die("package %s installs into /%s, which packages must not use", name,
			    forbidden[i]);
		free(p);
	}
	memset(&post, 0, sizeof(post));
	post.strip = !option_off(r, "strip");
	post.purge_la = !option_off(r, "purge_la");
	if (nftw(pkgdir, post_one, 32, FTW_PHYS) < 0)
		die("cannot walk %s: %s", pkgdir, strerror(errno));
	if (!option_off(r, "purge_infodir")) {
		char *dir = path_join(pkgdir, "usr/share/info/dir");
		unlink(dir);
		free(dir);
	}
	free(post.seen);
	if (post.stripped || post.la)
		msg("%s: stripped %d files, removed %d libtool archives", name, post.stripped, post.la);
}

static bool has_entries(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *de;
	bool any = false;
	while (d && !any && (de = readdir(d)))
		any = strcmp(de->d_name, ".") && strcmp(de->d_name, "..");
	if (d)
		closedir(d);
	return any;
}

static void to_overrides(const struct strlist *owner, struct owner_override **out, size_t *n)
{
	*out = xcalloc(owner->n + 1, sizeof(**out));
	*n = owner->n;
	for (size_t i = 0; i < owner->n; i++) {
		char path[PATH_MAX];
		unsigned uid, gid, mode;
		if (sscanf(owner->v[i], "%4095s %u %u %o", path, &uid, &gid, &mode) != 4)
			die("owner entry '%s' is not 'PATH UID GID MODE'", owner->v[i]);
		const char *p = path;
		while (*p == '/')
			p++;
		(*out)[i] = (struct owner_override){xstrdup(p), uid, gid, (mode_t)mode};
	}
}

/* ---- the command ------------------------------------------------------------- */

static void usage(void)
{
	puts("usage: flpkg build [options] [DIR]\n"
	     "Build the recipe in DIR (default: .) into packages.\n\n"
	     "  -o, --outdir DIR    where to write the packages (default: .)\n"
	     "  -s, --sources DIR   look for source files here first (repeatable)\n"
	     "      --nocheck       skip check()\n"
	     "      --keep          keep the work directory");
}

int cmd_build(int argc, char **argv)
{
	static const struct option longopts[] = {
		{"outdir", required_argument, NULL, 'o'}, {"sources", required_argument, NULL, 's'},
		{"nocheck", no_argument, NULL, 'C'},      {"keep", no_argument, NULL, 'K'},
		{"help", no_argument, NULL, 'h'},         {0},
	};
	struct strlist dirs = {0};
	const char *outdir = ".";
	bool nocheck = false, keep = false;
	int c;
	optind = 1;
	while ((c = getopt_long(argc, argv, "o:s:h", longopts, NULL)) != -1) {
		switch (c) {
		case 'o': outdir = optarg; break;
		case 's': sl_add(&dirs, optarg); break;
		case 'C': nocheck = true; break;
		case 'K': keep = true; break;
		case 'h': usage(); return 0;
		default: usage(); return 2;
		}
	}
	if (argc - optind > 1) {
		usage();
		return 2;
	}
	char *recipedir = realpath(optind < argc ? argv[optind] : ".", NULL);
	if (!recipedir)
		die("cannot find recipe directory: %s", strerror(errno));
	char *recipe = path_join(recipedir, "recipe");
	if (access(recipe, R_OK) < 0)
		die("no recipe in %s", recipedir);
	char *out = realpath(outdir, NULL);
	if (!out)
		die("output directory %s: %s", outdir, strerror(errno));

	char *arch = machine_arch();
	char *cachedir = path_join(cfg.cache, "sources");
	sl_add(&dirs, cachedir);

	/* Read the recipe. */
	struct env env = {0};
	env_set(&env, "PATH", "/usr/bin:/usr/sbin");
	env_set(&env, "LC_ALL", "POSIX");
	struct strbuf dump = {0};
	if (run_helper(&env, "dump", recipe, NULL, NULL, &dump) != 0)
		die("cannot read %s", recipe);
	struct recipe r;
	char *err = NULL;
	if (parse_vars(dump.buf, dump.len, &r, &err) < 0)
		die("%s: %s", recipe, err);
	sb_free(&dump);
	if (!r.pkgbase && r.pkgname.n)
		r.pkgbase = xstrdup(r.pkgname.v[0]);
	validate(&r, arch);
	const char *pkgarch = sl_has(&r.arch, "any") ? "any" : arch;
	msg("building %s %s-%s for %s", r.pkgbase, r.version, r.release, pkgarch);

	/* Sources, work directory. */
	struct source *src = resolve_sources(&r, recipedir, &dirs, cachedir);
	char *work = path_join(cfg.builddir, r.pkgbase);
	char *srcdir = path_join(work, "src");
	if (rmtree(work) < 0 || mkdirs(srcdir, 0755) < 0)
		die("cannot prepare %s: %s", work, strerror(errno));
	umask(022);
	unpack_sources(src, r.source.n, srcdir);

	/* The environment the recipe's functions see. */
	char *jobs = xasprintf("%d", cfg.jobs);
	char *makeflags = xasprintf("-j%d", cfg.jobs);
	char *epoch = xasprintf("%lld", (long long)cfg.source_date_epoch);
	char *worksrc = xasprintf("%s-%s", r.pkgbase, r.version);
	env_set(&env, "HOME", work);
	env_set(&env, "TERM", getenv("TERM") ? getenv("TERM") : "dumb");
	env_set(&env, "MAKEFLAGS", makeflags);
	env_set(&env, "NINJAJOBS", jobs);
	env_set(&env, "TESTSUITEFLAGS", makeflags);
	env_set(&env, "SOURCE_DATE_EPOCH", epoch);
	env_set(&env, "TAR_OPTIONS", "--no-same-owner");
	env_set(&env, "CFLAGS", cfg.cflags);
	env_set(&env, "CXXFLAGS", cfg.cxxflags);
	env_set(&env, "LDFLAGS", cfg.ldflags);
	env_set(&env, "CARCH", arch);
	env_set(&env, "srcdir", srcdir);
	env_set(&env, "startdir", recipedir);
	env_set(&env, "worksrc", worksrc);

	static const char *steps[] = {"prepare", "build", "check"};
	for (size_t i = 0; i < 3; i++) {
		if (!sl_has(&r.function, steps[i]))
			continue;
		if (i == 2 && nocheck) {
			msg("skipping check()");
			continue;
		}
		msg("%s()", steps[i]);
		if (run_helper(&env, "run", recipe, steps[i], NULL, NULL) != 0)
			die("%s() failed", steps[i]);
	}

	/* One package per name. */
	for (size_t i = 0; i < r.pkgname.n; i++) {
		const char *name = r.pkgname.v[i];
		char *pkgdir = xasprintf("%s/pkg/%s", work, name);
		char *vars = xasprintf("%s/pkg/%s.vars", work, name);
		char *func = r.pkgname.n == 1 && sl_has(&r.function, "package")
		                 ? xstrdup("package")
		                 : xasprintf("package_%s", name);
		if (mkdirs(pkgdir, 0755) < 0)
			die("cannot create %s: %s", pkgdir, strerror(errno));
		struct env penv = {0};
		for (size_t j = 0; j < env.v.n; j++)
			sl_add(&penv.v, env.v.v[j]);
		env_set(&penv, "pkgdir", pkgdir);
		env_set(&penv, "pkgname", name);
		msg("%s()", func);
		if (run_helper(&penv, "run", recipe, func, vars, NULL) != 0)
			die("%s() failed", func);
		sl_free(&penv.v);

		/* package_NAME() may have changed the metadata. */
		size_t vlen;
		char *vbuf = read_file(vars, &vlen);
		struct recipe pr;
		if (!vbuf || parse_vars(vbuf, vlen, &pr, &err) < 0)
			die("cannot read the metadata of %s", name);
		free(vbuf);

		clean_package(&pr, pkgdir, name);
		if (!has_entries(pkgdir))
			die("package %s is empty: package() installed nothing", name);

		struct pkginfo info = {0};
		info.name = xstrdup(name);
		info.base = xstrdup(r.pkgbase);
		info.version = xasprintf("%s-%s", r.version, r.release);
		info.summary = xstrdup(pr.summary ? pr.summary : r.summary);
		info.url = pr.url ? xstrdup(pr.url) : NULL;
		info.arch = xstrdup(pkgarch);
		info.builddate = cfg.source_date_epoch;
		info.license = pr.license, pr.license = (struct strlist){0};
		info.depend = pr.depends, pr.depends = (struct strlist){0};
		info.provide = pr.provides, pr.provides = (struct strlist){0};
		info.conflict = pr.conflicts, pr.conflicts = (struct strlist){0};
		info.replace = pr.replaces, pr.replaces = (struct strlist){0};
		info.group = pr.groups, pr.groups = (struct strlist){0};
		info.sysuser = pr.sysusers, pr.sysusers = (struct strlist){0};
		info.sysgroup = pr.sysgroups, pr.sysgroups = (struct strlist){0};
		for (size_t j = 0; j < info.depend.n; j++) {
			struct dep d;
			if (dep_parse(info.depend.v[j], &d) < 0)
				die("%s: bad dependency '%s'", name, info.depend.v[j]);
			dep_free(&d);
		}

		struct owner_override *ov;
		size_t nov;
		to_overrides(&pr.owner, &ov, &nov);
		char *file = xasprintf("%s/%s-%s-%s.flpkg", out, name, info.version, pkgarch);
		if (pack_create(pkgdir, &info, ov, nov, file, &err) < 0)
			die("%s: %s", name, err);
		char hex[SHA256_HEX];
		sha256_file(file, hex);
		msg("created %s", file);
		printf("   %llu bytes installed, sha256 %s\n", (unsigned long long)info.size, hex);

		for (size_t j = 0; j < nov; j++)
			free(ov[j].path);
		free(ov);
		free(file);
		pkginfo_free(&info);
		recipe_free(&pr);
		free(func);
		free(vars);
		free(pkgdir);
	}

	if (!keep && rmtree(work) < 0)
		warn("cannot remove %s: %s", work, strerror(errno));

	for (size_t i = 0; i < r.source.n; i++) {
		free(src[i].name);
		free(src[i].path);
	}
	free(src);
	recipe_free(&r);
	sl_free(&env.v);
	sl_free(&dirs);
	free(jobs);
	free(makeflags);
	free(epoch);
	free(worksrc);
	free(work);
	free(srcdir);
	free(cachedir);
	free(arch);
	free(out);
	free(recipe);
	free(recipedir);
	return 0;
}
