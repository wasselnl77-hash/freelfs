/* The prompt: user, folder, git branch, the last command's failure,
 * and how long slow commands took.
 *
 *   tortina ~/p/geometry-clone (main) ✗1 ›
 *
 * PS1, when set, replaces it (with \u \h \w \W \$ \n \e and $expansions),
 * and a function named flsh_prompt replaces both: its output is the prompt.
 * SPDX-License-Identifier: MIT */
#include "flsh.h"

#include <limits.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

bool use_color;
static struct timespec started;
static bool timing;
static int last_status;

bool term_is_linux(void)
{
	const char *t = var_get("TERM");
	return t && !strcmp(t, "linux");
}

static const char *user_name(void)
{
	const char *u = var_get("USER");
	if (u && *u)
		return u;
	struct passwd *pw = getpwuid(geteuid());
	return pw ? pw->pw_name : "?";
}

/* ~/projects/geometry-clone -> ~/p/geometry-clone */
static char *short_cwd(void)
{
	char buf[PATH_MAX];
	const char *pwd = var_get("PWD");
	if (!pwd || pwd[0] != '/')
		pwd = getcwd(buf, sizeof(buf)) ? buf : "?";
	const char *home = var_get("HOME");
	struct strbuf sb = {0};
	const char *rest = pwd;
	size_t hl = home ? strlen(home) : 0;
	if (hl > 1 && !strncmp(pwd, home, hl) && (pwd[hl] == '/' || !pwd[hl])) {
		sb_addc(&sb, '~');
		rest = pwd + hl;
	}
	const char *last = strrchr(rest, '/');
	for (const char *p = rest; *p;) {
		if (*p == '/') {
			sb_addc(&sb, '/');
			p++;
			continue;
		}
		const char *slash = strchr(p, '/');
		size_t len = slash ? (size_t)(slash - p) : strlen(p);
		if (p - 1 == last || !slash)
			sb_add(&sb, p, len);
		else {
			size_t keep = p[0] == '.' && len > 1 ? 2 : 1; /* .config -> .c */
			keep = (size_t)utf8_len((unsigned char)p[keep - 1]) + keep - 1;
			sb_add(&sb, p, keep < len ? keep : len);
		}
		p += len;
	}
	if (!sb.len)
		sb_addc(&sb, '/');
	return sb_steal(&sb);
}

/* The git branch, from .git/HEAD in this folder or above it */
static char *git_branch(void)
{
	char buf[PATH_MAX];
	if (!getcwd(buf, sizeof(buf)))
		return NULL;
	for (;;) {
		char *dotgit = xasprintf("%s/.git", buf);
		struct stat st;
		char *head = NULL;
		if (stat(dotgit, &st) == 0) {
			if (S_ISDIR(st.st_mode)) {
				char *hp = xasprintf("%s/HEAD", dotgit);
				head = read_file(hp, NULL);
				free(hp);
			} else {
				/* a worktree: "gitdir: PATH" */
				char *t = read_file(dotgit, NULL);
				if (t && has_prefix(t, "gitdir: ")) {
					char *dir = t + 8;
					dir[strcspn(dir, "\n")] = '\0';
					char *hp = dir[0] == '/' ? xasprintf("%s/HEAD", dir) : xasprintf("%s/%s/HEAD", buf, dir);
					head = read_file(hp, NULL);
					free(hp);
				}
				free(t);
			}
		}
		free(dotgit);
		if (head) {
			head[strcspn(head, "\n")] = '\0';
			char *b;
			if (has_prefix(head, "ref: refs/heads/"))
				b = xstrdup(head + 16);
			else if (has_prefix(head, "ref: "))
				b = xstrdup(head + 5);
			else
				b = xstrndup(head, 7);
			free(head);
			return b;
		}
		char *slash = strrchr(buf, '/');
		if (!slash || slash == buf)
			return NULL;
		*slash = '\0';
	}
}

static char *bash_style(const char *ps1)
{
	struct strbuf sb = {0};
	char host[256] = "";
	gethostname(host, sizeof(host) - 1);
	for (const char *p = ps1; *p; p++) {
		if (*p != '\\' || !p[1]) {
			sb_addc(&sb, *p);
			continue;
		}
		p++;
		switch (*p) {
		case 'u': sb_addstr(&sb, user_name()); break;
		case 'h': sb_add(&sb, host, strcspn(host, ".")); break;
		case 'H': sb_addstr(&sb, host); break;
		case 'w': {
			char buf[PATH_MAX];
			const char *pwd = var_get("PWD");
			if (!pwd)
				pwd = getcwd(buf, sizeof(buf)) ? buf : "?";
			const char *home = var_get("HOME");
			size_t hl = home ? strlen(home) : 0;
			if (hl > 1 && !strncmp(pwd, home, hl) && (pwd[hl] == '/' || !pwd[hl]))
				sb_addf(&sb, "~%s", pwd + hl);
			else
				sb_addstr(&sb, pwd);
			break;
		}
		case 'W': {
			const char *pwd = var_get("PWD");
			const char *s = pwd ? strrchr(pwd, '/') : NULL;
			sb_addstr(&sb, s && s[1] ? s + 1 : pwd ? pwd : "?");
			break;
		}
		case '$': sb_addc(&sb, geteuid() ? '$' : '#'); break;
		case 'n': sb_addc(&sb, '\n'); break;
		case 'e': sb_addc(&sb, 033); break;
		case 'a': sb_addc(&sb, '\a'); break;
		case '[':
		case ']': break;
		case '\\': sb_addc(&sb, '\\'); break;
		default:
			sb_addc(&sb, '\\');
			sb_addc(&sb, *p);
		}
	}
	return sb_steal(&sb);
}

#define C(s) (use_color ? s : "")

char *make_prompt(void)
{
	if (find_function("flsh_prompt")) {
		int saved = sh.status;
		char *p = command_subst("flsh_prompt", 11);
		sh.status = saved;
		return p;
	}
	const char *ps1 = var_get("PS1");
	if (ps1) {
		char *e = bash_style(ps1);
		char *p = expand_string(e);
		free(e);
		return p;
	}
	bool linux_con = term_is_linux();
	bool root = geteuid() == 0;
	struct strbuf sb = {0};
	sb_addstr(&sb, root ? C("\033[31m") : C("\033[32m"));
	sb_addstr(&sb, user_name());
	if (var_get("SSH_CONNECTION")) {
		char host[256] = "";
		gethostname(host, sizeof(host) - 1);
		sb_addf(&sb, "@%.*s", (int)strcspn(host, "."), host);
	}
	sb_addstr(&sb, C("\033[0m"));
	char *cwd = short_cwd();
	sb_addf(&sb, " %s%s%s", C("\033[34m"), cwd, C("\033[0m"));
	free(cwd);
	char *branch = git_branch();
	if (branch) {
		sb_addf(&sb, " %s(%s)%s", C("\033[90m"), branch, C("\033[0m"));
		free(branch);
	}
	if (last_status) {
		const char *mark = linux_con ? "\xc3\x97" : "\xe2\x9c\x97"; /* × or ✗ */
		const char *sig = last_status > 128 ? strsig(last_status - 128) : NULL;
		if (sig)
			sb_addf(&sb, " %s%s%s%s", C("\033[31m"), mark, sig, C("\033[0m"));
		else
			sb_addf(&sb, " %s%s%d%s", C("\033[31m"), mark, last_status, C("\033[0m"));
	}
	if (root)
		sb_addf(&sb, " %s#%s ", C("\033[31m"), C("\033[0m"));
	else
		sb_addf(&sb, " %s\xe2\x80\xba%s ", C("\033[90m"), C("\033[0m")); /* › */
	return sb_steal(&sb);
}

const char *ps2_prompt(void)
{
	static char *p;
	free(p);
	const char *ps2 = var_get("PS2");
	if (!ps2 || !strcmp(ps2, "> "))
		p = xasprintf("%s\xe2\x80\xa6%s ", C("\033[90m"), C("\033[0m")); /* … */
	else
		p = expand_string(ps2);
	return p;
}

void prompt_command_started(void)
{
	clock_gettime(CLOCK_MONOTONIC, &started);
	timing = true;
}

void prompt_command_finished(int status)
{
	last_status = status;
	if (!timing)
		return;
	timing = false;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long secs = (long)(now.tv_sec - started.tv_sec);
	if (secs < 5)
		return;
	char t[64];
	if (secs >= 3600)
		snprintf(t, sizeof(t), "%ldh %ldm", secs / 3600, secs / 60 % 60);
	else if (secs >= 60)
		snprintf(t, sizeof(t), "%ldm %lds", secs / 60, secs % 60);
	else
		snprintf(t, sizeof(t), "%lds", secs);
	if (status == 0)
		fprintf(stderr, "%sdone%s %sin %s%s\n", C("\033[32m"), C("\033[0m"), C("\033[90m"), t, C("\033[0m"));
	else
		fprintf(stderr, "%sfailed%s %safter %s%s\n", C("\033[31m"), C("\033[0m"), C("\033[90m"), t,
		        C("\033[0m"));
}

void prompt_reset_status(void)
{
	last_status = 0;
}
