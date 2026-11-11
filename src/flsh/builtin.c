/* The builtin commands.
 * SPDX-License-Identifier: MIT */
#include "flsh.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <termios.h>
#include <unistd.h>

/* ---- aliases ---------------------------------------------------------------------------- */
struct alias {
	struct alias *next;
	char *name, *value;
};
static struct alias *aliases;

const char *alias_get(const char *name)
{
	for (struct alias *a = aliases; a; a = a->next)
		if (!strcmp(a->name, name))
			return a->value;
	return NULL;
}

void alias_set(const char *name, const char *value)
{
	for (struct alias *a = aliases; a; a = a->next)
		if (!strcmp(a->name, name)) {
			free(a->value);
			a->value = xstrdup(value);
			return;
		}
	struct alias *a = xcalloc(1, sizeof(*a));
	a->name = xstrdup(name);
	a->value = xstrdup(value);
	a->next = aliases;
	aliases = a;
}

static bool alias_unset(const char *name)
{
	for (struct alias **pp = &aliases; *pp; pp = &(*pp)->next)
		if (!strcmp((*pp)->name, name)) {
			struct alias *a = *pp;
			*pp = a->next;
			free(a->name);
			free(a->value);
			free(a);
			return true;
		}
	return false;
}

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

struct strlist *alias_names(void)
{
	static struct strlist names;
	sl_free(&names);
	for (struct alias *a = aliases; a; a = a->next)
		sl_add(&names, a->name);
	if (names.n > 1)
		qsort(names.v, names.n, sizeof(char *), cmp_str);
	return &names;
}

/* 'text' with ' escaped, for output that can be read back */
static void print_quoted(const char *s)
{
	struct strbuf sb = {0};
	sb_addc(&sb, '\'');
	for (; *s; s++)
		if (*s == '\'')
			sb_addstr(&sb, "'\\''");
		else
			sb_addc(&sb, *s);
	sb_addc(&sb, '\'');
	fputs(sb.buf, stdout);
	sb_free(&sb);
}

static int bi_alias(int argc, char **argv)
{
	int st = 0;
	if (argc == 1 || (argc == 2 && !strcmp(argv[1], "-p"))) {
		struct strlist *names = alias_names();
		for (size_t i = 0; i < names->n; i++) {
			printf("alias %s=", names->v[i]);
			print_quoted(alias_get(names->v[i]));
			putchar('\n');
		}
		return 0;
	}
	for (int i = 1; i < argc; i++) {
		char *eq = strchr(argv[i], '=');
		if (eq) {
			char *name = xstrndup(argv[i], (size_t)(eq - argv[i]));
			alias_set(name, eq + 1);
			free(name);
		} else if (alias_get(argv[i])) {
			printf("alias %s=", argv[i]);
			print_quoted(alias_get(argv[i]));
			putchar('\n');
		} else {
			sh_warn("alias: %s: not found", argv[i]);
			st = 1;
		}
	}
	return st;
}

static int bi_unalias(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "-a")) {
		while (aliases)
			alias_unset(aliases->name);
		return 0;
	}
	int st = 0;
	for (int i = 1; i < argc; i++)
		if (!alias_unset(argv[i])) {
			sh_warn("unalias: %s: not found", argv[i]);
			st = 1;
		}
	return st;
}

/* ---- simple ones ------------------------------------------------------------------------------ */
static int bi_true(int argc, char **argv)
{
	return 0;
}

static int bi_false(int argc, char **argv)
{
	return 1;
}

static int bi_echo(int argc, char **argv)
{
	bool nl = true, esc = false;
	int i = 1;
	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		const char *p = argv[i] + 1;
		if (strspn(p, "neE") != strlen(p))
			break;
		for (; *p; p++)
			if (*p == 'n')
				nl = false;
			else if (*p == 'e')
				esc = true;
			else
				esc = false;
	}
	struct strbuf sb = {0};
	for (bool first = true; i < argc; i++, first = false) {
		if (!first)
			sb_addc(&sb, ' ');
		for (const char *p = argv[i]; *p; p++) {
			if (!esc || *p != '\\' || !p[1]) {
				sb_addc(&sb, *p);
				continue;
			}
			p++;
			switch (*p) {
			case 'a': sb_addc(&sb, '\a'); break;
			case 'b': sb_addc(&sb, '\b'); break;
			case 'c':
				fwrite(sb.buf, 1, sb.len, stdout);
				sb_free(&sb);
				return 0;
			case 'e': sb_addc(&sb, 033); break;
			case 'f': sb_addc(&sb, '\f'); break;
			case 'n': sb_addc(&sb, '\n'); break;
			case 'r': sb_addc(&sb, '\r'); break;
			case 't': sb_addc(&sb, '\t'); break;
			case 'v': sb_addc(&sb, '\v'); break;
			case '\\': sb_addc(&sb, '\\'); break;
			case 'x': {
				int v = 0, k = 0;
				for (; k < 2 && p[1] && strchr("0123456789abcdefABCDEF", p[1]); k++) {
					char h = *++p;
					v = v * 16 + (h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
				}
				if (k)
					sb_addc(&sb, (char)v);
				else
					sb_addstr(&sb, "\\x");
				break;
			}
			case '0': {
				int v = 0;
				for (int k = 0; k < 3 && p[1] >= '0' && p[1] <= '7'; k++)
					v = v * 8 + (*++p - '0');
				sb_addc(&sb, (char)v);
				break;
			}
			default:
				sb_addc(&sb, '\\');
				sb_addc(&sb, *p);
			}
		}
	}
	if (nl)
		sb_addc(&sb, '\n');
	if (sb.len)
		fwrite(sb.buf, 1, sb.len, stdout);
	sb_free(&sb);
	if (fflush(stdout) != 0 || ferror(stdout)) {
		clearerr(stdout);
		return 1;
	}
	return 0;
}

static int bi_colon(int argc, char **argv)
{
	return 0;
}

/* ---- cd / pwd ------------------------------------------------------------------------------------- */
/* Make an absolute path logical: drop ".", resolve ".." textually */
static char *canon_path(const char *path)
{
	struct strlist parts = {0};
	char *copy = xstrdup(path), *save = NULL;
	for (char *t = strtok_r(copy, "/", &save); t; t = strtok_r(NULL, "/", &save)) {
		if (!strcmp(t, "."))
			continue;
		if (!strcmp(t, "..")) {
			if (parts.n)
				free(parts.v[--parts.n]);
			continue;
		}
		sl_add(&parts, t);
	}
	free(copy);
	struct strbuf sb = {0};
	for (size_t i = 0; i < parts.n; i++)
		sb_addf(&sb, "/%s", parts.v[i]);
	if (!sb.len)
		sb_addc(&sb, '/');
	sl_free(&parts);
	return sb_steal(&sb);
}

static int bi_cd(int argc, char **argv)
{
	bool physical = false;
	int i = 1;
	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		if (!strcmp(argv[i], "--")) {
			i++;
			break;
		}
		if (!strcmp(argv[i], "-P"))
			physical = true;
		else if (!strcmp(argv[i], "-L"))
			physical = false;
		else
			break;
	}
	const char *dir = i < argc ? argv[i] : NULL;
	bool print = false;
	if (!dir) {
		dir = var_get("HOME");
		if (!dir || !*dir) {
			sh_warn("cd: HOME is not set");
			return 1;
		}
	} else if (!strcmp(dir, "-")) {
		dir = var_get("OLDPWD");
		if (!dir) {
			sh_warn("cd: OLDPWD is not set");
			return 1;
		}
		print = true;
	}
	char *target = NULL;
	/* CDPATH for relative names that do not start with . or .. */
	const char *cdpath = var_get("CDPATH");
	if (dir[0] != '/' && cdpath && strncmp(dir, "./", 2) && strncmp(dir, "../", 3) &&
	    strcmp(dir, ".") && strcmp(dir, "..")) {
		const char *p = cdpath;
		for (;;) {
			const char *colon = strchr(p, ':');
			size_t len = colon ? (size_t)(colon - p) : strlen(p);
			char *cand = len ? xasprintf("%.*s/%s", (int)len, p, dir) : xstrdup(dir);
			struct stat st;
			if (stat(cand, &st) == 0 && S_ISDIR(st.st_mode)) {
				target = cand;
				print = len > 0;
				break;
			}
			free(cand);
			if (!colon)
				break;
			p = colon + 1;
		}
	}
	if (!target)
		target = xstrdup(dir);
	const char *pwd = var_get("PWD");
	char *newpwd;
	if (physical)
		newpwd = NULL;
	else if (target[0] == '/')
		newpwd = canon_path(target);
	else {
		char cwd[PATH_MAX];
		const char *base = pwd && pwd[0] == '/' ? pwd : getcwd(cwd, sizeof(cwd));
		char *joined = xasprintf("%s/%s", base ? base : "", target);
		newpwd = canon_path(joined);
		free(joined);
	}
	if (chdir(newpwd ? newpwd : target) < 0) {
		/* the logical path may not exist (symlink games): try as given */
		if (!newpwd || chdir(target) < 0) {
			sh_warn("cd: %s: %s", dir, strerror(errno));
			free(target);
			free(newpwd);
			return 1;
		}
		free(newpwd);
		newpwd = NULL;
	}
	if (!newpwd) {
		char cwd[PATH_MAX];
		newpwd = xstrdup(getcwd(cwd, sizeof(cwd)) ? cwd : target);
	}
	if (pwd)
		var_set("OLDPWD", pwd, 0);
	var_set("PWD", newpwd, V_EXPORT);
	if (print)
		printf("%s\n", newpwd);
	free(target);
	free(newpwd);
	return 0;
}

static int bi_pwd(int argc, char **argv)
{
	bool physical = argc > 1 && !strcmp(argv[1], "-P");
	const char *pwd = var_get("PWD");
	char cwd[PATH_MAX];
	if (!physical && pwd && pwd[0] == '/') {
		struct stat a, b;
		if (stat(pwd, &a) == 0 && stat(".", &b) == 0 && a.st_ino == b.st_ino && a.st_dev == b.st_dev) {
			printf("%s\n", pwd);
			return 0;
		}
	}
	if (!getcwd(cwd, sizeof(cwd))) {
		sh_warn("pwd: %s", strerror(errno));
		return 1;
	}
	printf("%s\n", cwd);
	return 0;
}

/* ---- variables --------------------------------------------------------------------------------------- */
static int show_flag;

static int filter_flag(struct var *v)
{
	return (v->flags & (unsigned)show_flag) != 0;
}

static void show_export(struct var *v)
{
	printf("%s %s", show_flag == V_EXPORT ? "export" : "readonly", v->name);
	if (v->value) {
		putchar('=');
		print_quoted(v->value);
	}
	putchar('\n');
}

static int declare(int argc, char **argv, unsigned flag)
{
	int i = 1;
	bool unexport = false;
	for (; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "--")) {
			i++;
			break;
		}
		if (!strcmp(argv[i], "-p"))
			continue;
		if (!strcmp(argv[i], "-n") && flag == V_EXPORT)
			unexport = true;
		else {
			sh_warn("%s: %s: unknown option", argv[0], argv[i]);
			return 2;
		}
	}
	if (i == argc) {
		show_flag = (int)flag;
		var_list(filter_flag, show_export);
		return 0;
	}
	int st = 0;
	for (; i < argc; i++) {
		const char *eq = strchr(argv[i], '=');
		size_t len = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
		if (!is_name(argv[i], len)) {
			sh_warn("%s: %s: not a valid name", argv[0], argv[i]);
			st = 1;
			continue;
		}
		if (unexport) {
			char *name = xstrndup(argv[i], len);
			struct var *v = var_lookup(name);
			if (v)
				v->flags &= ~(unsigned)V_EXPORT;
			free(name);
			continue;
		}
		var_assign(argv[i], flag);
	}
	return st;
}

static int bi_export(int argc, char **argv)
{
	return declare(argc, argv, V_EXPORT);
}

static int bi_readonly(int argc, char **argv)
{
	return declare(argc, argv, V_READONLY);
}

static int bi_local(int argc, char **argv)
{
	if (!var_scope_active()) {
		sh_warn("local: only usable in a function");
		return 1;
	}
	int st = 0;
	for (int i = 1; i < argc; i++) {
		const char *eq = strchr(argv[i], '=');
		size_t len = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
		if (!is_name(argv[i], len)) {
			sh_warn("local: %s: not a valid name", argv[i]);
			st = 1;
			continue;
		}
		char *name = xstrndup(argv[i], len);
		var_make_local(name);
		if (eq)
			var_set(name, eq + 1, 0);
		else {
			struct var *v = var_lookup(name); /* declared, but unset */
			if (v && !(v->flags & V_READONLY)) {
				free(v->value);
				v->value = NULL;
				v->flags &= ~(unsigned)V_EXPORT;
			}
		}
		free(name);
	}
	return st;
}

static int bi_unset(int argc, char **argv)
{
	int mode = 0, st = 0, i = 1;
	for (; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "-f"))
			mode = 'f';
		else if (!strcmp(argv[i], "-v"))
			mode = 'v';
		else if (!strcmp(argv[i], "--")) {
			i++;
			break;
		} else
			break;
	}
	for (; i < argc; i++) {
		if (mode == 'f')
			unset_function(argv[i]);
		else if (mode == 'v' || var_lookup(argv[i]) || !find_function(argv[i]))
			st |= var_unset(argv[i]);
		else
			unset_function(argv[i]);
	}
	return st;
}

static void show_set(struct var *v)
{
	if (!v->value)
		return;
	printf("%s=", v->name);
	print_quoted(v->value);
	putchar('\n');
}

static const struct {
	const char *name;
	char letter;
	bool *flag;
} options[] = {
	{"allexport", 'a', &sh.o_allexport}, {"errexit", 'e', &sh.o_errexit},
	{"ignoreeof", 0, &sh.o_ignoreeof},   {"monitor", 'm', &sh.o_monitor},
	{"noclobber", 'C', &sh.o_noclobber}, {"noexec", 'n', &sh.o_noexec},
	{"noglob", 'f', &sh.o_noglob},       {"notify", 'b', &sh.o_notify},
	{"nounset", 'u', &sh.o_nounset},     {"pipefail", 0, &sh.o_pipefail},
	{"verbose", 'v', &sh.o_verbose},     {"xtrace", 'x', &sh.o_xtrace},
};
#define NOPTIONS (sizeof(options) / sizeof(options[0]))

int set_option_letter(char c, bool on)
{
	for (size_t i = 0; i < NOPTIONS; i++)
		if (options[i].letter == c) {
			if (options[i].flag == &sh.o_monitor && on && !sh.o_monitor && sh.interactive)
				jobs_init();
			else
				*options[i].flag = on;
			return 0;
		}
	return -1;
}

int set_option_name(const char *name, bool on)
{
	if (!strcmp(name, "emacs") || !strcmp(name, "vi"))
		return 0;
	for (size_t i = 0; i < NOPTIONS; i++)
		if (!strcmp(options[i].name, name)) {
			*options[i].flag = on;
			return 0;
		}
	return -1;
}

static int bi_set(int argc, char **argv)
{
	if (argc == 1) {
		var_list(NULL, show_set);
		return 0;
	}
	int i = 1;
	for (; i < argc; i++) {
		const char *a = argv[i];
		if (a[0] != '-' && a[0] != '+')
			break;
		if (!strcmp(a, "--")) {
			i++;
			goto positional;
		}
		bool on = a[0] == '-';
		if (!strcmp(a, "-") || !strcmp(a, "+")) {
			if (on) {
				sh.o_xtrace = sh.o_verbose = false;
				i++;
				goto positional;
			}
			continue;
		}
		for (const char *p = a + 1; *p; p++) {
			if (*p == 'o') {
				if (i + 1 >= argc) {
					for (size_t k = 0; k < NOPTIONS; k++)
						if (on)
							printf("%-12s %s\n", options[k].name, *options[k].flag ? "on" : "off");
						else
							printf("set %co %s\n", *options[k].flag ? '-' : '+', options[k].name);
					return 0;
				}
				if (set_option_name(argv[++i], on) < 0) {
					sh_warn("set: %s: unknown option", argv[i]);
					return 2;
				}
				break;
			}
			if (set_option_letter(*p, on) < 0) {
				sh_warn("set: -%c: unknown option", *p);
				return 2;
			}
		}
	}
	if (i == argc)
		return 0;
positional:
	params_set(argv + i, argc - i);
	return 0;
}

static int bi_shift(int argc, char **argv)
{
	long n = 1;
	if (argc > 1 && (str_to_int(argv[1], &n) < 0 || n < 0)) {
		sh_warn("shift: %s: not a number", argv[1]);
		return 2;
	}
	if (n > params.argc) {
		sh_warn("shift: cannot shift by %ld, there are only %d parameters", n, params.argc);
		return 1;
	}
	for (long k = 0; k < n; k++)
		free(params.argv[k]);
	memmove(params.argv, params.argv + n, (size_t)(params.argc - n + 1) * sizeof(char *));
	params.argc -= (int)n;
	return 0;
}

/* ---- control ------------------------------------------------------------------------------------------- */
static int loop_ctl(int argc, char **argv, bool cont)
{
	long n = 1;
	if (argc > 1 && (str_to_int(argv[1], &n) < 0 || n < 1)) {
		sh_warn("%s: %s: not a positive number", argv[0], argv[1]);
		return 1;
	}
	if (!sh.loopdepth)
		return 0;
	if (n > sh.loopdepth)
		n = sh.loopdepth;
	sh.breaks = (int)n;
	sh.cont = cont;
	return 0;
}

static int bi_break(int argc, char **argv)
{
	return loop_ctl(argc, argv, false);
}

static int bi_continue(int argc, char **argv)
{
	return loop_ctl(argc, argv, true);
}

static int bi_return(int argc, char **argv)
{
	long n = sh.status;
	if (argc > 1 && str_to_int(argv[1], &n) < 0) {
		sh_warn("return: %s: not a number", argv[1]);
		n = 2;
	}
	if (!sh.funcdepth && !sh.sourcedepth) {
		sh_warn("return: only usable in a function or a sourced file");
		return 1;
	}
	sh.status = (int)(n & 0xff);
	sh.returning = true;
	return sh.status;
}

static bool exit_warned;

static int bi_exit(int argc, char **argv)
{
	long n = sh.status;
	if (argc > 1 && str_to_int(argv[1], &n) < 0) {
		sh_warn("exit: %s: not a number", argv[1]);
		n = 2;
	}
	if (sh.interactive && !sh.subshell && job_count(true) && !exit_warned) {
		exit_warned = true;
		fprintf(stderr, "There are stopped jobs. Type exit again to end them and leave.\n");
		return 1;
	}
	sh_exit((int)n);
}

static int bi_eval(int argc, char **argv)
{
	struct strbuf sb = {0};
	for (int i = 1; i < argc; i++) {
		if (i > 1)
			sb_addc(&sb, ' ');
		sb_addstr(&sb, argv[i]);
	}
	int st = eval_string(sb.buf ? sb.buf : "", NULL);
	sb_free(&sb);
	return st;
}

static int bi_dot(int argc, char **argv)
{
	if (argc < 2) {
		sh_warn("%s: needs a file name", argv[0]);
		return 2;
	}
	char *path = NULL;
	if (strchr(argv[1], '/'))
		path = xstrdup(argv[1]);
	else {
		/* PATH, for readable files */
		const char *p = var_get("PATH");
		while (p && !path) {
			const char *colon = strchr(p, ':');
			size_t len = colon ? (size_t)(colon - p) : strlen(p);
			char *cand = len ? xasprintf("%.*s/%s", (int)len, p, argv[1]) : xstrdup(argv[1]);
			struct stat st;
			if (stat(cand, &st) == 0 && S_ISREG(st.st_mode) && access(cand, R_OK) == 0)
				path = cand;
			else
				free(cand);
			p = colon ? colon + 1 : NULL;
		}
		if (!path)
			path = xstrdup(argv[1]); /* the current directory, like bash */
	}
	struct params saved = {0};
	bool newparams = argc > 2;
	if (newparams) {
		saved = params;
		params.argc = 0;
		params.argv = NULL;
		params_set(argv + 2, argc - 2);
	}
	int st = source_file(path, true);
	if (newparams) {
		params_free(&params);
		params = saved;
	}
	free(path);
	return st;
}

static int bi_exec(int argc, char **argv)
{
	if (argc < 2)
		return 0;
	int i = 1;
	if (!strcmp(argv[1], "--"))
		i++;
	if (i >= argc)
		return 0;
	char *path = find_command(argv[i], true);
	if (!path) {
		sh_warn("exec: %s: no such command", argv[i]);
		if (!sh.interactive)
			sh_exit(127);
		return 127;
	}
	child_reset_signals();
	if (sh.interactive)
		edit_reset_term();
	char **env = var_environ();
	execve(path, argv + i, env);
	int err = errno;
	sh_warn("exec: %s: %s", argv[i], strerror(err));
	free(path);
	if (!sh.interactive)
		sh_exit(err == ENOENT ? 127 : 126);
	return 126;
}

static int bi_trap(int argc, char **argv)
{
	int i = 1;
	if (argc > 1 && !strcmp(argv[1], "--"))
		i++;
	if (i >= argc || !strcmp(argv[i], "-p")) {
		list_traps();
		return 0;
	}
	if (!strcmp(argv[i], "-l")) {
		for (int sig = 1; sig < NSIG; sig++)
			if (strsig(sig))
				printf("%2d) SIG%s\n", sig, strsig(sig));
		return 0;
	}
	const char *cmd = argv[i];
	bool reset = !strcmp(cmd, "-");
	/* "trap SIG" (a number first) resets, like "trap - SIG" */
	long dummy;
	if (str_to_int(cmd, &dummy) == 0) {
		reset = true;
		i--;
	}
	int st = 0;
	for (i++; i < argc; i++) {
		int sig = sig_number(argv[i]);
		if (sig < 0 || set_trap(sig, reset ? NULL : cmd) < 0) {
			sh_warn("trap: %s: bad signal", argv[i]);
			st = 1;
		}
	}
	return st;
}

static int bi_times(int argc, char **argv)
{
	struct tms t;
	times(&t);
	long hz = sysconf(_SC_CLK_TCK);
	double v[4] = {(double)t.tms_utime / hz, (double)t.tms_stime / hz,
	               (double)t.tms_cutime / hz, (double)t.tms_cstime / hz};
	printf("%dm%.3fs %dm%.3fs\n%dm%.3fs %dm%.3fs\n", (int)(v[0] / 60), v[0] - 60 * (int)(v[0] / 60),
	       (int)(v[1] / 60), v[1] - 60 * (int)(v[1] / 60), (int)(v[2] / 60),
	       v[2] - 60 * (int)(v[2] / 60), (int)(v[3] / 60), v[3] - 60 * (int)(v[3] / 60));
	return 0;
}

/* ---- jobs ----------------------------------------------------------------------------------------------- */
static int bi_jobs(int argc, char **argv)
{
	bool l = false, p = false;
	for (int i = 1; i < argc; i++)
		if (!strcmp(argv[i], "-l"))
			l = true;
		else if (!strcmp(argv[i], "-p"))
			p = true;
	job_list(l, p);
	return 0;
}

static int bi_fg(int argc, char **argv)
{
	if (!sh.o_monitor) {
		sh_warn("fg: no job control in this shell");
		return 1;
	}
	struct job *j = job_find(argc > 1 ? argv[1] : NULL, false);
	return j ? job_fg(j) : 1;
}

static int bi_bg(int argc, char **argv)
{
	if (!sh.o_monitor) {
		sh_warn("bg: no job control in this shell");
		return 1;
	}
	if (argc == 1) {
		struct job *j = job_find(NULL, false);
		return j ? job_bg(j) : 1;
	}
	int st = 0;
	for (int i = 1; i < argc; i++) {
		struct job *j = job_find(argv[i], false);
		if (j)
			job_bg(j);
		else
			st = 1;
	}
	return st;
}

static int bi_wait(int argc, char **argv)
{
	if (argc == 1)
		return job_wait_all();
	int st = 0;
	for (int i = 1; i < argc; i++) {
		long pid;
		if (argv[i][0] == '%') {
			struct job *j = job_find(argv[i], false);
			if (!j) {
				st = 127;
				continue;
			}
			pid = j->procs[j->nprocs - 1].pid;
		} else if (str_to_int(argv[i], &pid) < 0) {
			sh_warn("wait: %s: not a process id", argv[i]);
			st = 2;
			continue;
		}
		st = job_wait_pid((pid_t)pid);
	}
	return st;
}

static int bi_kill(int argc, char **argv)
{
	int sig = SIGTERM, i = 1;
	if (argc > 1 && !strcmp(argv[1], "-l")) {
		if (argc > 2) {
			long n;
			if (str_to_int(argv[2], &n) == 0) {
				if (n > 128)
					n -= 128;
				const char *name = strsig((int)n);
				if (!name) {
					sh_warn("kill: %s: bad signal", argv[2]);
					return 1;
				}
				puts(name);
			} else {
				int s = sig_number(argv[2]);
				if (s <= 0) {
					sh_warn("kill: %s: bad signal", argv[2]);
					return 1;
				}
				printf("%d\n", s);
			}
			return 0;
		}
		for (int s = 1; s < NSIG; s++)
			if (strsig(s))
				printf("%s%c", strsig(s), s % 8 ? ' ' : '\n');
		putchar('\n');
		return 0;
	}
	if (argc > 2 && !strcmp(argv[1], "-s")) {
		sig = sig_number(argv[2]);
		i = 3;
	} else if (argc > 1 && argv[1][0] == '-' && argv[1][1]) {
		if (!strcmp(argv[1], "--"))
			i = 2;
		else {
			sig = sig_number(argv[1] + 1);
			i = 2;
		}
	}
	if (sig < 0) {
		sh_warn("kill: bad signal");
		return 1;
	}
	if (i >= argc) {
		sh_warn("kill: usage: kill [-s SIGNAL | -SIGNAL] PID|%%JOB ...");
		return 2;
	}
	int st = 0;
	for (; i < argc; i++) {
		pid_t pid;
		if (argv[i][0] == '%') {
			struct job *j = job_find(argv[i], false);
			if (!j) {
				st = 1;
				continue;
			}
			pid = j->pgid ? -j->pgid : j->procs[0].pid;
		} else {
			long n;
			if (str_to_int(argv[i], &n) < 0) {
				sh_warn("kill: %s: not a process id or job", argv[i]);
				st = 1;
				continue;
			}
			pid = (pid_t)n;
		}
		if (kill(pid, sig) < 0) {
			sh_warn("kill: %s: %s", argv[i], strerror(errno));
			st = 1;
		} else if (argv[i][0] == '%' && (sig == SIGTERM || sig == SIGHUP)) {
			struct job *j = job_find(argv[i], true);
			if (j && j->pgid && !job_running(j))
				kill(-j->pgid, SIGCONT); /* a stopped job has to run to die */
		}
	}
	return st;
}

/* ---- read ------------------------------------------------------------------------------------------- */
static int bi_read(int argc, char **argv)
{
	bool raw = false, silent = false;
	const char *prompt = NULL;
	char delim = '\n';
	int i = 1;
	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		if (!strcmp(argv[i], "--")) {
			i++;
			break;
		}
		for (const char *p = argv[i] + 1; *p; p++) {
			if (*p == 'r')
				raw = true;
			else if (*p == 's')
				silent = true;
			else if (*p == 'p' || *p == 'd') {
				const char *val = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : NULL);
				if (!val) {
					sh_warn("read: -%c needs a value", *p);
					return 2;
				}
				if (*p == 'p')
					prompt = val;
				else
					delim = *val;
				break;
			} else {
				sh_warn("read: -%c: unknown option", *p);
				return 2;
			}
		}
	}
	struct termios old;
	bool restore = false;
	if (prompt && isatty(0)) {
		fputs(prompt, stderr);
		fflush(stderr);
	}
	if (silent && tcgetattr(0, &old) == 0) {
		struct termios t = old;
		t.c_lflag &= ~(tcflag_t)ECHO;
		tcsetattr(0, TCSAFLUSH, &t);
		restore = true;
	}
	struct strbuf line = {0}, mask = {0}; /* mask: '1' for escaped characters */
	bool eof = false;
	for (;;) {
		char c;
		ssize_t r = read(0, &c, 1);
		if (r < 0 && errno == EINTR) {
			check_traps();
			continue;
		}
		if (r <= 0) {
			eof = true;
			break;
		}
		if (c == delim)
			break;
		if (c == '\\' && !raw) {
			r = read(0, &c, 1);
			if (r <= 0) {
				eof = true;
				break;
			}
			if (c == '\n')
				continue; /* a line continuation */
			sb_addc(&line, c);
			sb_addc(&mask, '1');
			continue;
		}
		sb_addc(&line, c);
		sb_addc(&mask, '0');
	}
	if (restore) {
		tcsetattr(0, TCSAFLUSH, &old);
		fputc('\n', stderr);
	}
	const char *l = line.buf ? line.buf : "", *m = mask.buf ? mask.buf : "";
	size_t n = line.len;
	if (i >= argc) {
		var_set("REPLY", l, 0);
		sb_free(&line);
		sb_free(&mask);
		return eof ? 1 : 0;
	}
	const char *ifs = var_get("IFS");
	if (!ifs)
		ifs = " \t\n";
#define IS_IFS(k) (m[k] == '0' && l[k] && strchr(ifs, l[k]))
#define IS_WS(k) (IS_IFS(k) && (l[k] == ' ' || l[k] == '\t' || l[k] == '\n'))
	size_t pos = 0;
	while (pos < n && IS_WS(pos))
		pos++;
	for (; i < argc; i++) {
		if (!is_name(argv[i], strlen(argv[i]))) {
			sh_warn("read: %s: not a valid name", argv[i]);
			sb_free(&line);
			sb_free(&mask);
			return 2;
		}
		struct strbuf val = {0};
		if (i == argc - 1) {
			/* the last name takes the rest, minus trailing IFS white space */
			size_t end = n;
			while (end > pos && IS_WS(end - 1))
				end--;
			sb_add(&val, l + pos, end - pos);
			pos = n;
		} else {
			while (pos < n && !IS_IFS(pos))
				sb_addc(&val, l[pos++]);
			if (pos < n) {
				bool white = IS_WS(pos);
				pos++;
				while (pos < n && IS_WS(pos))
					pos++;
				if (white && pos < n && IS_IFS(pos) && !IS_WS(pos)) {
					pos++;
					while (pos < n && IS_WS(pos))
						pos++;
				}
			}
		}
		var_set(argv[i], val.buf ? val.buf : "", 0);
		sb_free(&val);
	}
#undef IS_IFS
#undef IS_WS
	sb_free(&line);
	sb_free(&mask);
	return eof ? 1 : 0;
}

/* ---- type / command / hash ----------------------------------------------------------------------------------- */
static int describe(const char *name, int how) /* how: 'V' verbose, 'v' short, 't' type word */
{
	const char *a = alias_get(name);
	if (a) {
		if (how == 'v') {
			printf("alias %s=", name);
			print_quoted(a);
			putchar('\n');
		} else if (how == 't')
			puts("alias");
		else {
			printf("%s is an alias for ", name);
			print_quoted(a);
			putchar('\n');
		}
		return 0;
	}
	if (is_reserved(name)) {
		if (how == 'v')
			puts(name);
		else if (how == 't')
			puts("keyword");
		else
			printf("%s is a shell keyword\n", name);
		return 0;
	}
	struct node *f = find_function(name);
	if (f) {
		if (how == 'v')
			puts(name);
		else if (how == 't')
			puts("function");
		else {
			struct strbuf sb = {0};
			sb_addf(&sb, "%s is a function:\n%s() ", name, name);
			print_node(&sb, f);
			puts(sb.buf);
			sb_free(&sb);
		}
		return 0;
	}
	const struct builtin *b = find_builtin(name);
	if (b) {
		if (how == 'v')
			puts(name);
		else if (how == 't')
			puts("builtin");
		else
			printf("%s is a %sshell builtin\n", name, b->special ? "special " : "");
		return 0;
	}
	char *path = find_command(name, false);
	if (path && (strchr(name, '/') ? access(path, X_OK) == 0 : true)) {
		if (how == 'v')
			puts(path);
		else if (how == 't')
			puts("file");
		else
			printf("%s is %s\n", name, path);
		free(path);
		return 0;
	}
	free(path);
	if (how == 'V')
		sh_warn("%s: not found", name);
	return 1;
}

static int bi_type(int argc, char **argv)
{
	int st = 0, how = 'V', i = 1;
	if (argc > 1 && !strcmp(argv[1], "-t")) {
		how = 't';
		i++;
	}
	for (; i < argc; i++)
		st |= describe(argv[i], how);
	return st;
}

static int bi_command(int argc, char **argv)
{
	int i = 1, how = 0;
	for (; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "-v"))
			how = 'v';
		else if (!strcmp(argv[i], "-V"))
			how = 'V';
		else if (!strcmp(argv[i], "-p"))
			;
		else if (!strcmp(argv[i], "--")) {
			i++;
			break;
		} else
			break;
	}
	if (i >= argc)
		return 0;
	if (how) {
		int st = 0;
		for (; i < argc; i++)
			st |= describe(argv[i], how);
		return st;
	}
	/* command -p NAME ...: run it, skipping functions */
	const struct builtin *b = find_builtin(argv[i]);
	if (b)
		return run_builtin(b, argc - i, argv + i);
	char *path = find_command(argv[i], true);
	if (!path) {
		sh_warn("%s: no such command", argv[i]);
		return 127;
	}
	/* build a node-less external run: fork and exec */
	struct strbuf sb = {0};
	for (int k = i; k < argc; k++) {
		sb_addstr(&sb, k > i ? " '" : "'");
		for (const char *p = argv[k]; *p; p++)
			if (*p == '\'')
				sb_addstr(&sb, "'\\''");
			else
				sb_addc(&sb, *p);
		sb_addc(&sb, '\'');
	}
	free(path);
	char *cmd = xasprintf("command %s", sb.buf);
	sb_free(&sb);
	int st = eval_string(cmd, NULL);
	free(cmd);
	return st;
}

static int bi_hash(int argc, char **argv)
{
	if (argc == 1) {
		hash_list();
		return 0;
	}
	if (!strcmp(argv[1], "-r")) {
		hash_clear();
		return 0;
	}
	int st = 0;
	for (int i = 1; i < argc; i++) {
		char *p = find_command(argv[i], true);
		if (!p) {
			sh_warn("hash: %s: not found", argv[i]);
			st = 1;
		}
		free(p);
	}
	return st;
}

static int optpos;

static int bi_getopts(int argc, char **argv)
{
	if (argc < 3) {
		sh_warn("getopts: usage: getopts OPTSTRING NAME [ARG...]");
		return 2;
	}
	const char *opts = argv[1], *name = argv[2];
	char **args = argc > 3 ? argv + 3 : params.argv;
	int nargs = argc > 3 ? argc - 3 : params.argc;
	bool quiet = opts[0] == ':';
	if (quiet)
		opts++;
	long ind = 1;
	const char *s = var_get("OPTIND");
	if (!s || str_to_int(s, &ind) < 0 || ind < 1)
		ind = 1;
	static long last_ind = 1;
	if (ind != last_ind)
		optpos = 0; /* OPTIND was changed from outside */
	if (ind > nargs) {
		var_set(name, "?", 0);
		last_ind = ind;
		return 1;
	}
	const char *arg = args[ind - 1];
	if (!optpos) {
		if (arg[0] != '-' || !arg[1]) {
			var_set(name, "?", 0);
			last_ind = ind;
			return 1;
		}
		if (!strcmp(arg, "--")) {
			var_set_int("OPTIND", ind + 1);
			last_ind = ind + 1;
			var_set(name, "?", 0);
			return 1;
		}
		optpos = 1;
	}
	char c = arg[optpos++];
	char buf[2] = {c, '\0'};
	const char *spec = c != ':' ? strchr(opts, c) : NULL;
	bool end_of_arg = arg[optpos] == '\0';
	if (!spec) {
		if (quiet) {
			var_set("OPTARG", buf, 0);
		} else {
			sh_warn("-%c: unknown option", c);
			var_unset("OPTARG");
		}
		var_set(name, "?", 0);
	} else if (spec[1] == ':') {
		const char *val = NULL;
		if (!end_of_arg) {
			val = arg + optpos;
			end_of_arg = true;
		} else if (ind < nargs) {
			val = args[ind];
			ind++;
		}
		if (!val) {
			if (quiet) {
				var_set("OPTARG", buf, 0);
				var_set(name, ":", 0);
			} else {
				sh_warn("-%c: needs a value", c);
				var_unset("OPTARG");
				var_set(name, "?", 0);
			}
		} else {
			var_set("OPTARG", val, 0);
			var_set(name, buf, 0);
		}
	} else {
		var_unset("OPTARG");
		var_set(name, buf, 0);
	}
	if (end_of_arg) {
		ind++;
		optpos = 0;
	}
	var_set_int("OPTIND", ind);
	last_ind = ind;
	return 0;
}

/* ---- umask / ulimit -------------------------------------------------------------------------------------------- */
static int bi_umask(int argc, char **argv)
{
	mode_t cur = umask(0);
	umask(cur);
	bool sym = argc > 1 && !strcmp(argv[1], "-S");
	int i = sym ? 2 : 1;
	if (i >= argc) {
		if (sym) {
			mode_t m = ~cur & 0777;
			printf("u=%s%s%s,g=%s%s%s,o=%s%s%s\n", m & 0400 ? "r" : "", m & 0200 ? "w" : "",
			       m & 0100 ? "x" : "", m & 040 ? "r" : "", m & 020 ? "w" : "", m & 010 ? "x" : "",
			       m & 04 ? "r" : "", m & 02 ? "w" : "", m & 01 ? "x" : "");
		} else
			printf("%04o\n", (unsigned)cur);
		return 0;
	}
	const char *s = argv[i];
	if (s[0] >= '0' && s[0] <= '7') {
		char *end;
		long v = strtol(s, &end, 8);
		if (*end || v < 0 || v > 0777) {
			sh_warn("umask: %s: bad mode", s);
			return 1;
		}
		umask((mode_t)v);
		return 0;
	}
	/* symbolic: who[+-=]perms,... applied to the allowed bits */
	mode_t allowed = ~cur & 0777;
	char *copy = xstrdup(s), *save = NULL;
	for (char *clause = strtok_r(copy, ",", &save); clause; clause = strtok_r(NULL, ",", &save)) {
		mode_t who = 0;
		char *p = clause;
		for (; *p && strchr("ugoa", *p); p++)
			who |= *p == 'u' ? 0700 : *p == 'g' ? 070 : *p == 'o' ? 07 : 0777;
		if (!who)
			who = 0777;
		char op = *p ? *p++ : 0;
		mode_t perm = 0;
		for (; *p; p++)
			perm |= *p == 'r' ? 0444 : *p == 'w' ? 0222 : *p == 'x' ? 0111 : 0;
		if (op == '+')
			allowed |= perm & who;
		else if (op == '-')
			allowed &= ~(perm & who);
		else if (op == '=')
			allowed = (allowed & ~who) | (perm & who);
		else {
			free(copy);
			sh_warn("umask: %s: bad mode", s);
			return 1;
		}
	}
	free(copy);
	umask(~allowed & 0777);
	return 0;
}

static const struct {
	char opt;
	int res;
	int unit;
	const char *what;
} limits[] = {
	{'c', RLIMIT_CORE, 512, "core file size (blocks)"},
	{'d', RLIMIT_DATA, 1024, "data seg size (kB)"},
	{'f', RLIMIT_FSIZE, 512, "file size (blocks)"},
	{'l', RLIMIT_MEMLOCK, 1024, "max locked memory (kB)"},
	{'n', RLIMIT_NOFILE, 1, "open files"},
	{'s', RLIMIT_STACK, 1024, "stack size (kB)"},
	{'t', RLIMIT_CPU, 1, "cpu time (seconds)"},
	{'u', RLIMIT_NPROC, 1, "max user processes"},
	{'v', RLIMIT_AS, 1024, "virtual memory (kB)"},
};

static void show_limit(int k, bool hard, bool label)
{
	struct rlimit rl;
	getrlimit(limits[k].res, &rl);
	rlim_t v = hard ? rl.rlim_max : rl.rlim_cur;
	if (label)
		printf("%-28s (-%c) ", limits[k].what, limits[k].opt);
	if (v == RLIM_INFINITY)
		puts("unlimited");
	else
		printf("%llu\n", (unsigned long long)(v / (rlim_t)limits[k].unit));
}

static int bi_ulimit(int argc, char **argv)
{
	bool hard = false, soft = false, all = false;
	int which = 2; /* -f, the POSIX default */
	int i = 1;
	for (; i < argc && argv[i][0] == '-'; i++)
		for (const char *p = argv[i] + 1; *p; p++) {
			if (*p == 'H')
				hard = true;
			else if (*p == 'S')
				soft = true;
			else if (*p == 'a')
				all = true;
			else {
				int k = -1;
				for (size_t m = 0; m < sizeof(limits) / sizeof(limits[0]); m++)
					if (limits[m].opt == *p)
						k = (int)m;
				if (k < 0) {
					sh_warn("ulimit: -%c: unknown option", *p);
					return 2;
				}
				which = k;
			}
		}
	if (all) {
		for (size_t m = 0; m < sizeof(limits) / sizeof(limits[0]); m++)
			show_limit((int)m, hard, true);
		return 0;
	}
	if (i >= argc) {
		show_limit(which, hard, false);
		return 0;
	}
	struct rlimit rl;
	getrlimit(limits[which].res, &rl);
	rlim_t v;
	if (!strcmp(argv[i], "unlimited"))
		v = RLIM_INFINITY;
	else {
		long n;
		if (str_to_int(argv[i], &n) < 0 || n < 0) {
			sh_warn("ulimit: %s: bad limit", argv[i]);
			return 1;
		}
		v = (rlim_t)n * (rlim_t)limits[which].unit;
	}
	if (hard || !soft)
		rl.rlim_max = hard || v > rl.rlim_max ? v : rl.rlim_max;
	if (soft || !hard)
		rl.rlim_cur = v;
	if (setrlimit(limits[which].res, &rl) < 0) {
		sh_warn("ulimit: %s", strerror(errno));
		return 1;
	}
	return 0;
}

static int bi_builtin(int argc, char **argv)
{
	if (argc < 2)
		return 0;
	const struct builtin *b = find_builtin(argv[1]);
	if (!b) {
		sh_warn("builtin: %s: not a builtin", argv[1]);
		return 1;
	}
	return run_builtin(b, argc - 1, argv + 1);
}

static int bi_help(int argc, char **argv);

/* ---- the table --------------------------------------------------------------------------------------------------- */
static const struct builtin table[] = {
	{".", bi_dot, true, "FILE [ARG...]: run FILE's commands in this shell"},
	{":", bi_colon, true, "do nothing, successfully"},
	{"[", bi_test, false, "EXPR ]: same as test"},
	{"alias", bi_alias, false, "[NAME[=VALUE]...]: define or show aliases"},
	{"bg", bi_bg, false, "[JOB...]: continue stopped jobs in the background"},
	{"break", bi_break, true, "[N]: leave N loops"},
	{"builtin", bi_builtin, false, "NAME [ARG...]: run a builtin, skipping functions"},
	{"cd", bi_cd, false, "[-L|-P] [DIR|-]: change directory (no DIR: home, -: the previous one)"},
	{"command", bi_command, false, "[-v|-V] NAME [ARG...]: run or describe a command, skipping functions"},
	{"complete", bi_complete, false, "NAME [-s SUB] [-w WORDS] [-c CMD] [-f] [-d]: Tab completion for NAME"},
	{"continue", bi_continue, true, "[N]: next round of the Nth loop"},
	{"echo", bi_echo, false, "[-neE] [ARG...]: print the arguments"},
	{"eval", bi_eval, true, "[ARG...]: run the arguments as commands"},
	{"exec", bi_exec, true, "[COMMAND [ARG...]]: replace the shell, or redirect it for good"},
	{"exit", bi_exit, true, "[N]: leave the shell"},
	{"export", bi_export, true, "[-n] [NAME[=VALUE]...]: give variables to commands that run"},
	{"false", bi_false, false, "fail, doing nothing"},
	{"fg", bi_fg, false, "[JOB]: bring a job to the foreground"},
	{"getopts", bi_getopts, false, "OPTSTRING NAME [ARG...]: parse options in scripts"},
	{"hash", bi_hash, false, "[-r] [NAME...]: remembered command locations"},
	{"help", bi_help, false, "[NAME]: this list"},
	{"history", bi_history, false, "[N] [clear] [search TEXT]: commands typed so far"},
	{"jobs", bi_jobs, false, "[-l] [-p]: list jobs"},
	{"kill", bi_kill, false, "[-s SIGNAL | -SIGNAL] PID|%JOB...: send a signal (-l: list)"},
	{"local", bi_local, false, "NAME[=VALUE]...: variables that end with the function"},
	{"pwd", bi_pwd, false, "[-L|-P]: print the current directory"},
	{"read", bi_read, false, "[-r] [-s] [-p PROMPT] [-d DELIM] [NAME...]: read a line into variables"},
	{"readonly", bi_readonly, true, "[NAME[=VALUE]...]: variables that cannot change"},
	{"return", bi_return, true, "[N]: leave a function or sourced file"},
	{"set", bi_set, true, "[-+eufxvnCam] [-o OPTION] [--] [ARG...]: options and positional parameters"},
	{"shift", bi_shift, true, "[N]: drop the first N positional parameters"},
	{"source", bi_dot, false, "FILE [ARG...]: same as ."},
	{"test", bi_test, false, "EXPR: check files, strings and numbers"},
	{"times", bi_times, true, "time used by the shell and its children"},
	{"trap", bi_trap, true, "[COMMAND] SIGNAL...: run COMMAND on SIGNAL (- resets, '' ignores)"},
	{"true", bi_true, false, "succeed, doing nothing"},
	{"type", bi_type, false, "[-t] NAME...: what a name runs"},
	{"ulimit", bi_ulimit, false, "[-HSa] [-cdflnstuv] [LIMIT]: resource limits"},
	{"umask", bi_umask, false, "[-S] [MODE]: permissions taken away from new files"},
	{"unalias", bi_unalias, false, "[-a] NAME...: remove aliases"},
	{"unset", bi_unset, true, "[-f|-v] NAME...: remove variables or functions"},
	{"wait", bi_wait, false, "[PID|%JOB...]: wait for background jobs"},
};
#define NTABLE (sizeof(table) / sizeof(table[0]))

static int bi_help(int argc, char **argv)
{
	if (argc > 1) {
		const struct builtin *b = find_builtin(argv[1]);
		if (!b) {
			sh_warn("help: %s: not a builtin", argv[1]);
			return 1;
		}
		printf("%s %s\n", b->name, b->help);
		return 0;
	}
	printf("flsh %s, the FreeLFS shell. Builtin commands:\n\n", FLSH_VERSION);
	for (size_t i = 0; i < NTABLE; i++)
		printf("  %-9s %s\n", table[i].name, table[i].help);
	printf("\nKeys: Tab completes, Up/Down search history by what you typed, Right takes the grey\n"
	       "suggestion, Ctrl-R searches all history, Ctrl-A/E go to start/end, Ctrl-W/U/K delete.\n");
	return 0;
}

const struct builtin *find_builtin(const char *name)
{
	size_t lo = 0, hi = NTABLE;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		int c = strcmp(name, table[mid].name);
		if (!c)
			return &table[mid];
		if (c < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	return NULL;
}

const struct builtin *builtins_table(size_t *n)
{
	*n = NTABLE;
	return table;
}

int run_builtin(const struct builtin *b, int argc, char **argv)
{
	int st = b->fn(argc, argv);
	fflush(stdout);
	return st;
}
