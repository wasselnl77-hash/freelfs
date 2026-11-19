/* flpkg command line: subcommands, with pacman-style flags as aliases.
 * SPDX-License-Identifier: MIT */
#include "flpkg.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(FILE *f)
{
	fputs("usage: flpkg [--root DIR] [--config FILE] COMMAND [ARGS]\n\n"
	      "Packages:\n"
	      "  install PKG...      (-S)   install packages and their dependencies (a terminal user: every source,\n"
	      "                             FreeLFS first, --from=freelfs|flatpak|nix|apt|pacman|dnf picks one)\n"
	      "  remove PKG...       (-R)   remove packages\n"
	      "  upgrade             (-Su)  upgrade all packages\n"
	      "  sync                (-Sy)  refresh the repo indexes\n"
	      "  search TEXT         (-Ss)  search the repos\n"
	      "  check-updates       [--offline] [--count]  list available upgrades (tab-separated)\n"
	      "Queries:\n"
	      "  list                (-Q)   list installed packages\n"
	      "  info PKG|-f FILE    (-Qi)  show package details\n"
	      "  files PKG|-f FILE   (-Ql)  list a package's files\n"
	      "  owns PATH           (-Qo)  which package owns a file\n"
	      "  verify [PKG]        (-Qk)  check installed files\n"
	      "Building:\n"
	      "  build [DIR]                build the recipe in DIR\n"
	      "  repo add REPO FILE...      add packages to a repo\n"
	      "  key generate PATH          create a signing key pair (PATH.key and PATH.pub)\n"
	      "  key introduce --key OLD.key --new NEW.pub DIR   rotation statement (see docs/SECURITY.md)\n"
	      "  key list                   the trusted keys and their retirement dates\n"
	      "System:\n"
	      "  fix-owners [--keep]        apply ownership a rootless install deferred (--keep: leave the database alone)\n"
	      "  snapshots                  list the btrfs snapshots taken before transactions\n"
	      "  history                    show past transactions\n"
	      "  version\n",
	      f);
}

/* ---- inspecting package files ---------------------------------------------- */

static void print_list(const char *label, const struct strlist *l)
{
	printf("%-14s: ", label);
	if (!l->n)
		fputs("None", stdout);
	for (size_t i = 0; i < l->n; i++)
		printf("%s%s", i ? "  " : "", l->v[i]);
	putchar('\n');
}

void print_pkginfo(const struct pkginfo *p)
{
	printf("%-14s: %s\n", "Name", p->name);
	printf("%-14s: %s\n", "Version", p->version);
	printf("%-14s: %s\n", "Summary", p->summary ? p->summary : "");
	printf("%-14s: %s\n", "URL", p->url ? p->url : "None");
	printf("%-14s: %s\n", "Architecture", p->arch);
	print_list("Licenses", &p->license);
	print_list("Groups", &p->group);
	print_list("Provides", &p->provide);
	print_list("Depends on", &p->depend);
	print_list("Conflicts with", &p->conflict);
	print_list("Replaces", &p->replace);
	printf("%-14s: %.1f KiB\n", "Installed size", (double)p->size / 1024);
	printf("%-14s: %s\n", "Base", p->base);
}

static int file_query(int argc, char **argv, bool files)
{
	if (argc == 2 && argv[1][0] != '-')
		return files ? cmd_files_installed(argv[1]) : cmd_info_installed(argv[1]);
	if (!files && argc == 3 && !strcmp(argv[1], "--repo"))
		return cmd_info_repo(argv[2]);
	if (argc != 3 || strcmp(argv[1], "-f"))
		die("usage: flpkg %s PKG | flpkg %s -f FILE.flpkg", argv[0], argv[0]);
	struct pkgfile pf;
	char *err = NULL;
	if (pkgfile_open(argv[2], &pf, &err) < 0)
		die("%s", err);
	if (files) {
		for (size_t i = 0; i < pf.files.n; i++) {
			const struct manifest_entry *e = &pf.files.v[i];
			printf("%s /%s%s", pf.info.name, e->path, e->type == 'd' ? "/" : "");
			if (e->type == 'l')
				printf(" -> %s", e->target);
			putchar('\n');
		}
	} else {
		print_pkginfo(&pf.info);
	}
	pkgfile_close(&pf);
	return 0;
}

/* ---- pacman-style aliases ----------------------------------------------------- */

static const struct {
	const char *flag, *cmd, *extra;
} aliases[] = {
	{"-S", "install", NULL},       {"-U", "install", NULL},
	{"-Sy", "sync", NULL},         {"-Su", "upgrade", NULL},
	{"-Syu", "upgrade", "--sync"}, {"-Ss", "search", NULL},
	{"-Si", "info", "--repo"},     {"-R", "remove", NULL},
	{"-Rs", "remove", "--recursive"}, {"-Rc", "remove", "--cascade"},
	{"-Q", "list", NULL},          {"-Qi", "info", NULL},
	{"-Ql", "files", NULL},        {"-Qo", "owns", NULL},
	{"-Qk", "verify", NULL},
};

/* Rewrite argv[0] (a pacman-style flag) as a subcommand. */
static char **translate(int *argc, char **argv)
{
	for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
		if (strcmp(argv[0], aliases[i].flag))
			continue;
		char **nv = xcalloc((size_t)*argc + 2, sizeof(char *));
		int n = 0;
		nv[n++] = (char *)aliases[i].cmd;
		if (aliases[i].extra)
			nv[n++] = (char *)aliases[i].extra;
		for (int j = 1; j < *argc; j++)
			nv[n++] = argv[j];
		*argc = n;
		return nv;
	}
	die("unknown option %s (see 'flpkg help')", argv[0]);
}


/* Round 22: install, remove, upgrade, search and list on the command line of a person go through
 * the sources front end (lib/flpkg/sources.sh: FreeLFS repo, Flathub, Nix, Distrobox). The native
 * commands stay for everything else: -y (flpkg-seat, build scripts), --root, package files, a
 * different config, FLPKG_NO_SOURCES=1. search and list show every source only on a terminal (or
 * with --all): what other programs read from a pipe stays the plain FreeLFS output. */
static void maybe_sources(const char *cmd, int argc, char **argv, const char *config, const char *root)
{
	static const char *const cmds[] = {"install", "remove", "upgrade", "search", "list"};
	bool hit = false, all = false, from = false;
	for (size_t i = 0; i < sizeof cmds / sizeof *cmds; i++)
		hit |= !strcmp(cmd, cmds[i]);
	if (!hit || root || strcmp(config, "/etc/flpkg.conf") || getenv("FLPKG_NO_SOURCES"))
		return;
	const char *script = getenv("FLPKG_SOURCES") ? getenv("FLPKG_SOURCES") : "/usr/lib/flpkg/sources.sh";
	if (access(script, X_OK) != 0)
		return;
	bool priv = !strcmp(cmd, "install") || !strcmp(cmd, "remove") || !strcmp(cmd, "upgrade");
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--all"))
			all = true;
		else if (!strncmp(argv[i], "--from=", 7))
			from = true; /* a named source (Nix, Flathub...) is the front end's business, even into a pipe */
		else if (!strcmp(argv[i], "--explicit") || !strcmp(argv[i], "--deps") || !strcmp(argv[i], "--repo"))
			return; /* native queries */
		else if (geteuid() == 0 && priv &&
		         (!strcmp(argv[i], "-y") || !strcmp(argv[i], "--yes") || !strcmp(argv[i], "--nodeps") ||
		          !strcmp(argv[i], "--unsigned") || has_suffix(argv[i], ".flpkg")))
			return; /* root with a file or -y: flpkg-seat, build scripts. A person without root goes to the front end,
			         * which sends it through flpkg-seat (flauth) or says what cannot be done that way */
	}
	bool view = !strcmp(cmd, "search") || !strcmp(cmd, "list");
	if (view && !all && !from && !isatty(1) && !getenv("FLPKG_ASSUME_TTY"))
		return;
	if (!strcmp(cmd, "search") && argc == 2 && !argv[1][0])
		return; /* the Store lists everything with an empty search */
	char **nv = xcalloc((size_t)argc + 2, sizeof(char *));
	nv[0] = (char *)script;
	for (int i = 0; i < argc; i++)
		nv[i + 1] = argv[i];
	char self[4096];
	ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
	if (n > 0 && !getenv("FLPKG_NATIVE")) {
		self[n] = 0;
		setenv("FLPKG_NATIVE", self, 1);
	}
	execv(script, nv);
}

int main(int argc, char **argv)
{
	const char *config = "/etc/flpkg.conf", *root = NULL;
	int i = 1;
	for (; i < argc; i++) {
		if (!strcmp(argv[i], "--root") && i + 1 < argc)
			root = argv[++i];
		else if (!strcmp(argv[i], "--config") && i + 1 < argc)
			config = argv[++i];
		else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose"))
			verbose++;
		else
			break;
	}
	if (i >= argc) {
		usage(stderr);
		return 2;
	}
	config_load(config);
	if (root) {
		free(cfg.root);
		cfg.root = xstrdup(root);
	}

	argc -= i;
	argv += i;
	if (argv[0][0] == '-' && strcmp(argv[0], "--help") && strcmp(argv[0], "--version"))
		argv = translate(&argc, argv);

	const char *cmd = argv[0];
	maybe_sources(cmd, argc, argv, config, root);
	if (!strcmp(cmd, "help") || !strcmp(cmd, "--help")) {
		usage(stdout);
		return 0;
	}
	if (!strcmp(cmd, "version") || !strcmp(cmd, "--version")) {
		printf("flpkg %s\n", FLPKG_VERSION);
		return 0;
	}
	if (!strcmp(cmd, "build"))
		return cmd_build(argc, argv);
	if (!strcmp(cmd, "info"))
		return file_query(argc, argv, false);
	if (!strcmp(cmd, "files"))
		return file_query(argc, argv, true);
	if (!strcmp(cmd, "install"))
		return cmd_install(argc, argv);
	if (!strcmp(cmd, "remove"))
		return cmd_remove(argc, argv);
	if (!strcmp(cmd, "list"))
		return cmd_list(argc, argv);
	if (!strcmp(cmd, "owns"))
		return cmd_owns(argc, argv);
	if (!strcmp(cmd, "verify"))
		return cmd_verify(argc, argv);
	if (!strcmp(cmd, "history"))
		return cmd_history();
	if (!strcmp(cmd, "fix-owners"))
		return cmd_fix_owners(argc, argv);
	if (!strcmp(cmd, "upgrade"))
		return cmd_upgrade(argc, argv);
	if (!strcmp(cmd, "sync"))
		return cmd_sync();
	if (!strcmp(cmd, "check-updates"))
		return cmd_check_updates(argc, argv);
	if (!strcmp(cmd, "search"))
		return cmd_search(argc, argv);
	if (!strcmp(cmd, "repo"))
		return cmd_repo(argc, argv);
	if (!strcmp(cmd, "key"))
		return cmd_key(argc, argv);
	if (!strcmp(cmd, "snapshots"))
		return cmd_snapshots();
	die("unknown command '%s' (see 'flpkg help')", cmd);
}
