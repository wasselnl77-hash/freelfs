/* /etc/flpkg.conf: "key = value" lines and "repo NAME URL" lines.
 * SPDX-License-Identifier: MIT */
#include "flpkg.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#ifndef LIBDIR
#define LIBDIR "/usr/lib/flpkg"
#endif

struct config cfg;

static void set(char **field, const char *v)
{
	free(*field);
	*field = *v ? xstrdup(v) : NULL;
}

static bool parse_bool(const char *v, int lineno)
{
	if (!strcmp(v, "yes") || !strcmp(v, "true") || !strcmp(v, "1"))
		return true;
	if (!strcmp(v, "no") || !strcmp(v, "false") || !strcmp(v, "0"))
		return false;
	die("flpkg.conf line %d: expected yes or no, got '%s'", lineno, v);
}

static long parse_long(const char *v, int lineno)
{
	char *end;
	errno = 0;
	long n = strtol(v, &end, 10);
	if (errno || end == v || *end || n < 0)
		die("flpkg.conf line %d: expected a number, got '%s'", lineno, v);
	return n;
}

static void defaults(void)
{
	cfg.root = xstrdup("/");
	cfg.cache = xstrdup("/var/cache/flpkg");
	cfg.builddir = xstrdup("/var/tmp/flpkg");
	cfg.keydir = xstrdup("/etc/flpkg/keys");
	cfg.libdir = xstrdup(getenv("FLPKG_LIBDIR") ? getenv("FLPKG_LIBDIR") : LIBDIR);
	cfg.zstd_level = 19;
	cfg.signatures = xstrdup("required");
	cfg.source_date_epoch = 1767225600; /* 2026-01-01, FreeLFS 0.1 */
}

void config_load(const char *path)
{
	defaults();
	size_t len;
	char *text = read_file(path, &len);
	if (!text) {
		if (errno != ENOENT)
			die("cannot read %s: %s", path, strerror(errno));
		goto env;
	}
	char *save = NULL;
	int lineno = 0;
	for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		lineno++;
		char *hash = strchr(line, '#');
		if (hash)
			*hash = '\0';
		char *s = trim(line);
		if (!*s)
			continue;
		if (has_prefix(s, "repo ") || has_prefix(s, "repo\t")) {
			char *name = trim(s + 5), *url = name + strcspn(name, " \t");
			if (!*url)
				die("flpkg.conf line %d: expected 'repo NAME URL'", lineno);
			*url++ = '\0';
			url = trim(url);
			cfg.repos = xrealloc(cfg.repos, (cfg.nrepos + 1) * sizeof(*cfg.repos));
			cfg.repos[cfg.nrepos++] = (struct repo_cfg){xstrdup(name), xstrdup(url)};
			continue;
		}
		char *eq = strchr(s, '=');
		if (!eq)
			die("flpkg.conf line %d: expected 'key = value'", lineno);
		*eq = '\0';
		char *k = trim(s), *v = trim(eq + 1);
		if (!strcmp(k, "cache"))
			set(&cfg.cache, v);
		else if (!strcmp(k, "builddir"))
			set(&cfg.builddir, v);
		else if (!strcmp(k, "keys"))
			set(&cfg.keydir, v);
		else if (!strcmp(k, "signkey"))
			set(&cfg.signkey, v);
		else if (!strcmp(k, "jobs"))
			cfg.jobs = (int)parse_long(v, lineno);
		else if (!strcmp(k, "snapshots"))
			cfg.snapshots = (int)parse_long(v, lineno);
		else if (!strcmp(k, "compression_level"))
			cfg.zstd_level = (int)parse_long(v, lineno);
		else if (!strcmp(k, "backup_config"))
			cfg.backup_config = parse_bool(v, lineno);
		else if (!strcmp(k, "cflags"))
			set(&cfg.cflags, v);
		else if (!strcmp(k, "cxxflags"))
			set(&cfg.cxxflags, v);
		else if (!strcmp(k, "ldflags"))
			set(&cfg.ldflags, v);
		else if (!strcmp(k, "signatures")) {
			if (strcmp(v, "required") && strcmp(v, "optional") && strcmp(v, "never"))
				die("flpkg.conf line %d: signatures must be required, optional or never",
				    lineno);
			set(&cfg.signatures, v);
		} else if (!strcmp(k, "source_date_epoch"))
			cfg.source_date_epoch = parse_long(v, lineno);
		else
			warn("flpkg.conf line %d: unknown setting '%s'", lineno, k);
	}
	free(text);
env:
	if (!cfg.jobs) {
		long n = sysconf(_SC_NPROCESSORS_ONLN);
		cfg.jobs = n > 0 ? (int)n : 1;
	}
	if (getenv("SOURCE_DATE_EPOCH"))
		cfg.source_date_epoch = strtoll(getenv("SOURCE_DATE_EPOCH"), NULL, 10);
}

char *machine_arch(void)
{
	struct utsname u;
	if (uname(&u) < 0)
		die("uname failed");
	return xstrdup(u.machine);
}
