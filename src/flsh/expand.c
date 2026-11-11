/* Word expansion: tilde, parameters, command substitution, arithmetic,
 * field splitting, pathname expansion and quote removal.
 *
 * A word is walked once. Every character that ends up in the result is
 * appended together with a mode: '0' literal and unquoted, '1' quoted,
 * '2' unquoted from an expansion. Field splitting only cuts at '2'
 * characters, globbing ignores the special meaning of '1' characters, and
 * the quotes themselves are never copied, which is the quote removal.
 * SPDX-License-Identifier: MIT */
#include "flsh.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct xb {
	int flags;
	struct strbuf t, m; /* the current segment: text and modes */
	bool force;         /* had quotes: keep an empty field */
	bool any;           /* something besides an empty "$@" */
	struct strlist *out;
	struct strbuf joined; /* without X_SPLIT */
	bool joined_any;
};

static void walk(struct xb *x, const char *s, size_t n, bool dq, char litmode);

static void put(struct xb *x, const char *s, size_t n, char mode)
{
	sb_add(&x->t, s, n);
	for (size_t i = 0; i < n; i++)
		sb_addc(&x->m, mode);
	x->any = true;
}

static bool in_ifs(char c, const char *ifs)
{
	return c && strchr(ifs, c);
}

static bool ifs_white(char c)
{
	return c == ' ' || c == '\t' || c == '\n';
}

/* ---- pathname expansion ------------------------------------------------------------ */
static int cmp_coll(const void *a, const void *b)
{
	return strcoll(*(char *const *)a, *(char *const *)b);
}

static bool has_glob(const char *p)
{
	for (; *p; p++) {
		if (*p == '\\' && p[1])
			p++;
		else if (*p == '*' || *p == '?' || *p == '[')
			return true;
	}
	return false;
}

static char *unescape(const char *p, size_t n)
{
	struct strbuf sb = {0};
	for (size_t i = 0; i < n; i++) {
		if (p[i] == '\\' && i + 1 < n)
			i++;
		sb_addc(&sb, p[i]);
	}
	return sb_steal(&sb);
}

/* Expand pattern (with backslash escapes) below dir prefix; adds matches. */
static void glob_rec(const char *prefix, const char *pat, struct strlist *out)
{
	while (*pat == '/') {
		char *np = xasprintf("%s/", prefix);
		glob_rec(np, pat + 1, out);
		free(np);
		return;
	}
	const char *slash = pat;
	while (*slash && *slash != '/') {
		if (*slash == '\\' && slash[1])
			slash++;
		slash++;
	}
	size_t clen = (size_t)(slash - pat);
	char *comp = xstrndup(pat, clen);
	if (!has_glob(comp)) {
		char *lit = unescape(comp, clen);
		char *path = xasprintf("%s%s", prefix, lit);
		free(lit);
		if (*slash) {
			char *np = xasprintf("%s/", path);
			glob_rec(np, slash + 1, out);
			free(np);
		} else {
			struct stat st;
			if (lstat(path, &st) == 0)
				sl_add(out, path);
		}
		free(path);
		free(comp);
		return;
	}
	DIR *d = opendir(*prefix ? prefix : ".");
	if (!d) {
		free(comp);
		return;
	}
	struct strlist names = {0};
	struct dirent *de;
	while ((de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			if (comp[0] != '.')
				continue;
		if (fnmatch(comp, de->d_name, FNM_PERIOD) == 0)
			sl_add(&names, de->d_name);
	}
	closedir(d);
	if (names.n > 1)
		qsort(names.v, names.n, sizeof(char *), cmp_coll);
	for (size_t i = 0; i < names.n; i++) {
		char *path = xasprintf("%s%s", prefix, names.v[i]);
		if (*slash) {
			struct stat st;
			if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
				char *np = xasprintf("%s/", path);
				glob_rec(np, slash + 1, out);
				free(np);
			}
		} else
			sl_add(out, path);
		free(path);
	}
	sl_free(&names);
	free(comp);
}

/* text + modes -> pattern with the quoted characters escaped */
static char *to_pattern(const char *t, const char *m, size_t n)
{
	struct strbuf sb = {0};
	for (size_t i = 0; i < n; i++) {
		char c = t[i];
		if (c == '\\' || (m[i] == '1' && (c == '*' || c == '?' || c == '[' || c == ']')))
			sb_addc(&sb, '\\');
		sb_addc(&sb, c);
	}
	return sb_steal(&sb);
}

static void emit_field(struct xb *x, const char *t, const char *m, size_t n)
{
	bool globby = false;
	if (!sh.o_noglob && !(x->flags & X_NOGLOB))
		for (size_t i = 0; i < n && !globby; i++) {
			if (m[i] == '1')
				continue;
			if (t[i] == '*' || t[i] == '?')
				globby = true;
			else if (t[i] == '[') /* only with a closing ] after it */
				for (size_t k = i + 2; k < n && !globby; k++)
					globby = t[k] == ']' && m[k] != '1';
		}
	if (globby) {
		char *pat = to_pattern(t, m, n);
		struct strlist matches = {0};
		glob_rec("", pat, &matches);
		free(pat);
		if (matches.n) {
			for (size_t i = 0; i < matches.n; i++)
				sl_push(x->out, matches.v[i]);
			free(matches.v);
			return;
		}
	}
	sl_push(x->out, xstrndup(t, n));
}

/* Finish the current segment: split it into fields (X_SPLIT) or add it to
 * the joined result. */
static void finish_segment(struct xb *x)
{
	const char *t = x->t.buf ? x->t.buf : "", *m = x->m.buf ? x->m.buf : "";
	size_t n = x->t.len;
	if (!(x->flags & X_SPLIT)) {
		if (x->joined_any)
			sb_addc(&x->joined, ' ');
		sb_add(&x->joined, t, n);
		x->joined_any = true;
	} else if (x->any || x->force) {
		size_t before = x->out->n;
		const char *ifs = var_get("IFS");
		if (!ifs)
			ifs = " \t\n";
		struct strbuf ft = {0}, fm = {0};
		bool have = false; /* the current field has started */
		size_t i = 0;
		while (i < n) {
			if (m[i] == '2' && in_ifs(t[i], ifs) && ifs_white(t[i]) && !have &&
			    x->out->n == before) {
				i++; /* leading IFS white space: ignored */
				continue;
			}
			if (m[i] == '2' && in_ifs(t[i], ifs)) {
				bool white = ifs_white(t[i]);
				if (have)
					emit_field(x, ft.buf ? ft.buf : "", fm.buf ? fm.buf : "", ft.len);
				else if (!white)
					emit_field(x, "", "", 0); /* a:b with IFS=: before a */
				ft.len = fm.len = 0;
				have = false;
				i++;
				/* whitespace around one delimiter counts as part of it */
				while (i < n && m[i] == '2' && in_ifs(t[i], ifs) && ifs_white(t[i]))
					i++;
				if (white && i < n && m[i] == '2' && in_ifs(t[i], ifs) && !ifs_white(t[i])) {
					i++;
					while (i < n && m[i] == '2' && in_ifs(t[i], ifs) && ifs_white(t[i]))
						i++;
				}
				continue;
			}
			sb_addc(&ft, t[i]);
			sb_addc(&fm, m[i]);
			have = true;
			i++;
		}
		if (have || (x->force && x->out->n == before))
			emit_field(x, ft.buf ? ft.buf : "", fm.buf ? fm.buf : "", ft.len);
		sb_free(&ft);
		sb_free(&fm);
	}
	x->t.len = x->m.len = 0;
	if (x->t.buf)
		x->t.buf[0] = '\0';
	x->force = x->any = false;
}

/* ---- parameters ---------------------------------------------------------------------- */
static char *special_param(const char *name, bool *found)
{
	*found = true;
	char buf[64];
	if (!strcmp(name, "?"))
		snprintf(buf, sizeof(buf), "%d", sh.status);
	else if (!strcmp(name, "$"))
		snprintf(buf, sizeof(buf), "%ld", (long)sh.pid);
	else if (!strcmp(name, "!")) {
		if (!sh.lastbg) {
			*found = false;
			return NULL;
		}
		snprintf(buf, sizeof(buf), "%ld", (long)sh.lastbg);
	} else if (!strcmp(name, "#"))
		snprintf(buf, sizeof(buf), "%d", params.argc);
	else if (!strcmp(name, "0"))
		return xstrdup(arg0 ? arg0 : "flsh");
	else if (!strcmp(name, "-")) {
		char *p = buf;
		if (sh.o_errexit) *p++ = 'e';
		if (sh.o_noglob) *p++ = 'f';
		if (sh.interactive) *p++ = 'i';
		if (sh.o_monitor) *p++ = 'm';
		if (sh.o_noexec) *p++ = 'n';
		if (sh.o_nounset) *p++ = 'u';
		if (sh.o_verbose) *p++ = 'v';
		if (sh.o_xtrace) *p++ = 'x';
		if (sh.o_noclobber) *p++ = 'C';
		if (sh.o_allexport) *p++ = 'a';
		*p = '\0';
	} else if (name[0] >= '0' && name[0] <= '9') {
		long i = atol(name);
		if (i < 1 || i > params.argc) {
			*found = false;
			return NULL;
		}
		return xstrdup(params.argv[i - 1]);
	} else if (!strcmp(name, "LINENO"))
		snprintf(buf, sizeof(buf), "%d", sh.lineno);
	else if (!strcmp(name, "RANDOM"))
		snprintf(buf, sizeof(buf), "%d", (int)(random() & 0x7fff));
	else {
		const char *v = var_get(name);
		if (!v) {
			*found = false;
			return NULL;
		}
		return xstrdup(v);
	}
	return xstrdup(buf);
}

static char *join_params(char sep_char, bool have_sep)
{
	struct strbuf sb = {0};
	for (int i = 0; i < params.argc; i++) {
		if (i && have_sep)
			sb_addc(&sb, sep_char);
		sb_addstr(&sb, params.argv[i]);
	}
	return sb_steal(&sb);
}

/* Add an expansion's value: quoted, or unquoted and splittable */
static void put_value(struct xb *x, const char *v, bool dq)
{
	put(x, v, strlen(v), dq ? '1' : '2');
}

/* $@ and $* */
static void put_params(struct xb *x, bool star, bool dq)
{
	const char *ifs = var_get("IFS");
	if (star && dq) {
		char *j = join_params(ifs ? ifs[0] : ' ', !ifs || ifs[0]);
		put_value(x, j, true);
		free(j);
		return;
	}
	if (!params.argc)
		return;
	for (int i = 0; i < params.argc; i++) {
		if (i) {
			if (x->flags & X_SPLIT)
				finish_segment(x);
			else
				put(x, " ", 1, dq ? '1' : '2');
		}
		put_value(x, params.argv[i], dq);
		if (dq)
			x->force = true; /* "$@" with an empty argument keeps it */
	}
}

/* Expand a ${...} word part into a plain string (no splitting) */
static char *expand_sub(const char *s, size_t n, bool dq)
{
	struct strlist out = {0};
	struct xb x = {.flags = 0, .out = &out};
	walk(&x, s, n, dq, '0');
	finish_segment(&x);
	sb_free(&x.t);
	sb_free(&x.m);
	sl_free(&out);
	return sb_steal(&x.joined);
}

static char *expand_sub_pattern(const char *s, size_t n, bool dq)
{
	struct strlist out = {0};
	struct xb x = {.flags = 0, .out = &out};
	walk(&x, s, n, dq, '0');
	char *pat = to_pattern(x.t.buf ? x.t.buf : "", x.m.buf ? x.m.buf : "", x.t.len);
	sb_free(&x.t);
	sb_free(&x.m);
	return pat;
}

bool pattern_match(const char *pat, const char *s)
{
	return fnmatch(pat, s, 0) == 0;
}

/* ${v#pat} ${v##pat} ${v%pat} ${v%%pat} */
static char *trim(const char *v, const char *pat, bool suffix, bool longest)
{
	size_t n = strlen(v);
	char *tmp = xstrdup(v);
	char *res = NULL;
	if (!suffix) {
		for (size_t k = 0; k <= n; k++) {
			size_t i = longest ? n - k : k;
			char c = tmp[i];
			tmp[i] = '\0';
			bool ok = pattern_match(pat, tmp);
			tmp[i] = c;
			if (ok) {
				res = xstrdup(v + i);
				break;
			}
		}
	} else {
		for (size_t k = 0; k <= n; k++) {
			size_t i = longest ? k : n - k;
			if (pattern_match(pat, v + i)) {
				res = xstrndup(v, i);
				break;
			}
		}
	}
	free(tmp);
	return res ? res : xstrdup(v);
}

/* ${v/pat/rep}, ${v//pat/rep}, ${v/#pat/rep} and ${v/%pat/rep}: the
 * longest match wins at each position */
static char *replace(const char *v, const char *pat, const char *rep, int mode)
{
	struct strbuf sb = {0};
	size_t n = strlen(v), i = 0;
	char *tmp = xstrdup(v);
	bool done = false;
	while (i <= n) {
		if (!done && (mode != '#' || i == 0)) {
			size_t found = (size_t)-1;
			for (size_t j = n;; j--) {
				if (mode == '%' && j != n)
					break;
				char c = tmp[j];
				tmp[j] = '\0';
				bool ok = pattern_match(pat, tmp + i);
				tmp[j] = c;
				if (ok) {
					found = j;
					break;
				}
				if (j == i)
					break;
			}
			/* an empty match only counts when anchored */
			if (found != (size_t)-1 && (found > i || mode == '#' || mode == '%')) {
				sb_addstr(&sb, rep);
				if (mode != '/')
					done = true;
				if (found == i) {
					if (i < n)
						sb_addc(&sb, v[i]);
					i++;
				} else
					i = found;
				continue;
			}
		}
		if (i < n)
			sb_addc(&sb, v[i]);
		i++;
	}
	free(tmp);
	return sb_steal(&sb);
}

static void param_error(const char *name, const char *msg)
{
	sh_error("%s: %s", name, msg && *msg ? msg : "parameter null or not set");
}

/* ${...}: s points after "${", n is the length up to the closing '}' */
static void expand_brace(struct xb *x, const char *s, size_t n, bool dq)
{
	size_t i = 0;
	bool length = false;
	if (n > 1 && s[0] == '#') {
		length = true;
		i = 1;
	}
	/* the name */
	size_t ns = i;
	if (i < n && (s[i] == '_' || (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z'))) {
		while (i < n && is_name(s + i, 1) | (s[i] >= '0' && s[i] <= '9'))
			i++;
	} else if (i < n && s[i] >= '0' && s[i] <= '9') {
		while (i < n && s[i] >= '0' && s[i] <= '9')
			i++;
	} else if (i < n && strchr("@*#?-$!", s[i]))
		i++;
	else
		sh_error("${%.*s}: bad substitution", (int)n, s);
	char *name = xstrndup(s + ns, i - ns);
	bool is_at = !strcmp(name, "@"), is_star = !strcmp(name, "*");
	bool found;
	char *val = NULL;
	if (is_at || is_star) {
		found = params.argc > 0;
		val = join_params(' ', true);
	} else
		val = special_param(name, &found);

	if (length) {
		if (i != n)
			sh_error("${%.*s}: bad substitution", (int)n, s);
		if (!found && sh.o_nounset && !is_at && !is_star)
			param_error(name, "unbound variable");
		char buf[32];
		size_t chars = 0;
		if (is_at || is_star)
			chars = (size_t)params.argc;
		else
			for (const char *p = val ? val : ""; *p; p += utf8_len((unsigned char)*p))
				chars++;
		snprintf(buf, sizeof(buf), "%zu", chars);
		put_value(x, buf, dq);
		free(val);
		free(name);
		return;
	}

	if (i == n) { /* plain ${name} */
		if (!found && sh.o_nounset && !is_at && !is_star)
			param_error(name, "unbound variable");
		if (is_at || is_star)
			put_params(x, is_star, dq);
		else if (val)
			put_value(x, val, dq);
		free(val);
		free(name);
		return;
	}

	bool colon = false;
	if (s[i] == ':' && i + 1 < n && strchr("-=?+", s[i + 1])) {
		colon = true;
		i++;
	}
	char op = s[i++];
	const char *word = s + i;
	size_t wn = n - i;
	bool set = found && !(colon && val && !*val);
	switch (op) {
	case '-':
	case '+':
		if ((op == '-') == set) {
			if (is_at || is_star)
				put_params(x, is_star, dq);
			else if (val && set)
				put_value(x, val, dq);
		} else
			walk(x, word, wn, dq, dq ? '1' : '2');
		break;
	case '=':
		if (!set) {
			if (!is_name(name, strlen(name)))
				sh_error("${%s=...}: cannot assign this way", name);
			char *w = expand_sub(word, wn, dq);
			var_set(name, w, 0);
			put_value(x, w, dq);
			free(w);
		} else
			put_value(x, val, dq);
		break;
	case '?':
		if (!set)
			param_error(name, expand_sub(word, wn, dq));
		put_value(x, val, dq);
		break;
	case '#':
	case '%': {
		bool longest = i < n && s[i] == op;
		if (longest) {
			word++;
			wn--;
		}
		if (!found && sh.o_nounset)
			param_error(name, "unbound variable");
		char *pat = expand_sub_pattern(word, wn, false); /* "..." around ${} does not quote it */
		char *r = trim(val ? val : "", pat, op == '%', longest);
		put_value(x, r, dq);
		free(r);
		free(pat);
		break;
	}
	case '/': {
		int mode = 0;
		if (wn && (word[0] == '/' || word[0] == '#' || word[0] == '%')) {
			mode = word[0];
			word++;
			wn--;
		}
		/* split pattern / replacement at the first unescaped '/' */
		size_t k = 0;
		while (k < wn && word[k] != '/') {
			if (word[k] == '\\' && k + 1 < wn)
				k++;
			k++;
		}
		char *pat = expand_sub_pattern(word, k, false);
		char *rep = k < wn ? expand_sub(word + k + 1, wn - k - 1, dq) : xstrdup("");
		char *r = replace(val ? val : "", pat, rep, mode);
		put_value(x, r, dq);
		free(r);
		free(pat);
		free(rep);
		break;
	}
	case ':': { /* ${v:offset[:length]} */
		char *spec = expand_sub(word, wn, true);
		char *colon2 = strchr(spec, ':');
		if (colon2)
			*colon2 = '\0';
		long off = arith_eval(spec), len = colon2 ? arith_eval(colon2 + 1) : -1;
		const char *v = val ? val : "";
		long vl = (long)strlen(v);
		if (off < 0)
			off = vl + off < 0 ? 0 : vl + off;
		if (off > vl)
			off = vl;
		if (len < 0 && colon2)
			len = vl - off + len < 0 ? 0 : vl - off + len;
		if (!colon2 || off + len > vl)
			len = vl - off;
		char *r = xstrndup(v + off, (size_t)len);
		put_value(x, r, dq);
		free(r);
		free(spec);
		break;
	}
	case '^':
	case ',': {
		bool all = i < n && s[i] == op;
		char *r = xstrdup(val ? val : "");
		for (char *p = r; *p; p++) {
			if (op == '^' && *p >= 'a' && *p <= 'z')
				*p = (char)(*p - 32);
			else if (op == ',' && *p >= 'A' && *p <= 'Z')
				*p = (char)(*p + 32);
			if (!all)
				break;
		}
		put_value(x, r, dq);
		free(r);
		break;
	}
	default:
		sh_error("${%.*s}: bad substitution", (int)n, s);
	}
	free(val);
	free(name);
}

/* ---- command substitution -------------------------------------------------------------- */
char *command_subst(const char *cmd, size_t len)
{
	/* $(< file) reads the file */
	size_t k = 0;
	while (k < len && (cmd[k] == ' ' || cmd[k] == '\t' || cmd[k] == '\n'))
		k++;
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) < 0)
		sh_error("cannot make a pipe: %s", strerror(errno));
	fflush(stdout);
	pid_t pid = fork();
	if (pid < 0)
		sh_error("cannot fork: %s", strerror(errno));
	if (pid == 0) {
		close(fds[0]);
		if (fds[1] != 1) {
			dup2(fds[1], 1);
			close(fds[1]);
		}
		become_child();
		char *c = xstrndup(cmd, len);
		int st;
		if (k < len && cmd[k] == '<' && k + 1 < len && cmd[k + 1] != '<' && cmd[k + 1] != '&' &&
		    cmd[k + 1] != '>') {
			char *buf = xasprintf("cat %s", c + k);
			st = eval_string(buf, NULL);
		} else
			st = eval_string(c, NULL);
		sh_exit(st);
	}
	close(fds[1]);
	sh.cmdsubsts++;
	size_t n;
	char *out = read_fd(fds[0], &n);
	close(fds[0]);
	sh.status = job_wait_pid(pid);
	while (n && out[n - 1] == '\n')
		out[--n] = '\0';
	return out;
}

static void put_cmdsubst(struct xb *x, const char *cmd, size_t len, bool dq)
{
	char *out = command_subst(cmd, len);
	put_value(x, out, dq);
	free(out);
}

/* `...`: drop the backslashes before $ ` \ (and " when quoted) */
static void put_backquote(struct xb *x, const char *s, size_t n, bool dq)
{
	struct strbuf sb = {0};
	for (size_t i = 0; i < n; i++) {
		if (s[i] == '\\' && i + 1 < n &&
		    (s[i + 1] == '$' || s[i + 1] == '`' || s[i + 1] == '\\' || (dq && s[i + 1] == '"')))
			i++;
		sb_addc(&sb, s[i]);
	}
	put_cmdsubst(x, sb.buf ? sb.buf : "", sb.len, dq);
	sb_free(&sb);
}

static void put_arith(struct xb *x, const char *s, size_t n, bool dq)
{
	char *expr = expand_sub(s, n, true);
	long v = arith_eval(expr);
	free(expr);
	char buf[32];
	snprintf(buf, sizeof(buf), "%ld", v);
	put_value(x, buf, dq);
}

/* ---- tilde ------------------------------------------------------------------------------ */
static size_t tilde(struct xb *x, const char *s, size_t n, size_t i)
{
	size_t j = i + 1;
	while (j < n && s[j] != '/' && !((x->flags & X_ASSIGN) && s[j] == ':')) {
		char c = s[j];
		if (c == '\\' || c == '\'' || c == '"' || c == '$' || c == '`')
			return i; /* quoted or expanded: not a tilde prefix */
		j++;
	}
	char *user = xstrndup(s + i + 1, j - i - 1);
	const char *dir = NULL;
	struct passwd *pw;
	if (!*user) {
		dir = var_get("HOME");
		if (!dir && (pw = getpwuid(getuid())))
			dir = pw->pw_dir;
	} else if (!strcmp(user, "+"))
		dir = var_get("PWD");
	else if (!strcmp(user, "-"))
		dir = var_get("OLDPWD");
	else if ((pw = getpwnam(user)))
		dir = pw->pw_dir;
	free(user);
	if (!dir)
		return i;
	put(x, dir, strlen(dir), '1');
	return j;
}

/* ---- the walker ------------------------------------------------------------------------ */
/* litmode: the mode of plain unquoted characters ('0', or '2' inside an
 * unquoted ${v:-word}, whose result is split like any expansion). With
 * X_HEREDOC, double quotes are plain and only $ ` \ are special. */
static void walk(struct xb *x, const char *s, size_t n, bool dq, char litmode)
{
	bool heredoc = x->flags & X_HEREDOC;
	size_t i = 0;
	if (!dq && !heredoc && n && s[0] == '~' && (x->flags & (X_SPLIT | X_ASSIGN)))
		i = tilde(x, s, n, 0);
	while (i < n) {
		char c = s[i];
		if (c == '\\') {
			if (i + 1 >= n) {
				put(x, "\\", 1, dq ? '1' : litmode);
				i++;
				continue;
			}
			char d = s[i + 1];
			if (d == '\n') {
				i += 2;
				continue;
			}
			if (dq || heredoc) {
				if (d == '$' || d == '`' || d == '\\' || (d == '"' && !heredoc)) {
					put(x, &d, 1, '1');
					i += 2;
				} else {
					put(x, "\\", 1, '1');
					i++;
				}
				continue;
			}
			put(x, &d, 1, '1');
			i += 2;
			continue;
		}
		if (c == '\'' && !dq && !heredoc) {
			size_t j = i + 1;
			while (j < n && s[j] != '\'')
				j++;
			put(x, s + i + 1, j - i - 1, '1');
			x->force = true;
			i = j + 1;
			continue;
		}
		if (c == '"' && !heredoc) {
			if (dq) { /* closing quote of a "..." inside ${...} */
				i++;
				continue;
			}
			bool un = false;
			/* find the matching quote */
			size_t j = i + 1;
			while (j < n && s[j] != '"') {
				if (s[j] == '\\' && j + 1 < n)
					j += 2;
				else if (s[j] == '$')
					j = scan_dollar(s, j, n, &un);
				else if (s[j] == '`') {
					j++;
					while (j < n && s[j] != '`') {
						if (s[j] == '\\' && j + 1 < n)
							j++;
						j++;
					}
					j++;
				} else
					j++;
			}
			size_t len = (j < n ? j : n) - i - 1;
			/* "$@" with no parameters is no field at all */
			if (!((len == 2 && !strncmp(s + i + 1, "$@", 2)) ||
			      (len == 4 && !strncmp(s + i + 1, "${@}", 4))))
				x->force = true;
			walk(x, s + i + 1, len, true, '1');
			i = j + 1;
			continue;
		}
		if (c == '$' && i + 1 < n) {
			char d = s[i + 1];
			bool un = false;
			if (d == '(' && i + 2 < n && s[i + 2] == '(') {
				size_t end = scan_dollar(s, i, n, &un);
				/* $((expr)): end is after "))" for arithmetic */
				if (end >= i + 4 && s[end - 1] == ')' && s[end - 2] == ')') {
					put_arith(x, s + i + 3, end - i - 5, dq);
					i = end;
					continue;
				}
				put_cmdsubst(x, s + i + 2, end - i - 3, dq);
				i = end;
				continue;
			}
			if (d == '(') {
				size_t end = scan_dollar(s, i, n, &un);
				put_cmdsubst(x, s + i + 2, end - i - 3, dq);
				i = end;
				continue;
			}
			if (d == '{') {
				size_t end = scan_dollar(s, i, n, &un);
				expand_brace(x, s + i + 2, end - i - 3, dq);
				i = end;
				continue;
			}
			if (d == '@' || d == '*') {
				put_params(x, d == '*', dq);
				i += 2;
				continue;
			}
			if (strchr("#?-$!0123456789", d) || is_name(&d, 1)) {
				size_t end = scan_dollar(s, i, n, &un);
				if (d >= '0' && d <= '9')
					end = i + 2;
				char *name = xstrndup(s + i + 1, end - i - 1);
				bool found;
				char *v = special_param(name, &found);
				if (!found && sh.o_nounset)
					param_error(name, "unbound variable");
				if (v)
					put_value(x, v, dq);
				free(v);
				free(name);
				i = end;
				continue;
			}
			if (d == '\'' && !dq && !heredoc) { /* $'...' with C escapes */
				size_t j = i + 2;
				struct strbuf sb = {0};
				while (j < n && s[j] != '\'') {
					if (s[j] == '\\' && j + 1 < n) {
						char e = s[++j];
						switch (e) {
						case 'n': sb_addc(&sb, '\n'); break;
						case 't': sb_addc(&sb, '\t'); break;
						case 'r': sb_addc(&sb, '\r'); break;
						case 'a': sb_addc(&sb, '\a'); break;
						case 'b': sb_addc(&sb, '\b'); break;
						case 'e':
						case 'E': sb_addc(&sb, 033); break;
						case 'f': sb_addc(&sb, '\f'); break;
						case 'v': sb_addc(&sb, '\v'); break;
						case 'x': {
							int v = 0, k = 0;
							while (k < 2 && j + 1 < n && strchr("0123456789abcdefABCDEF", s[j + 1]) && s[j + 1]) {
								char h = s[++j];
								v = v * 16 + (h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
								k++;
							}
							sb_addc(&sb, (char)v);
							break;
						}
						default:
							if (e >= '0' && e <= '7') {
								int v = e - '0', k = 1;
								while (k < 3 && j + 1 < n && s[j + 1] >= '0' && s[j + 1] <= '7') {
									v = v * 8 + (s[++j] - '0');
									k++;
								}
								sb_addc(&sb, (char)v);
							} else
								sb_addc(&sb, e);
						}
						j++;
						continue;
					}
					sb_addc(&sb, s[j++]);
				}
				put(x, sb.buf ? sb.buf : "", sb.len, '1');
				sb_free(&sb);
				x->force = true;
				i = j + 1;
				continue;
			}
		}
		if (c == '`') {
			size_t j = i + 1;
			while (j < n && s[j] != '`') {
				if (s[j] == '\\' && j + 1 < n)
					j++;
				j++;
			}
			put_backquote(x, s + i + 1, (j < n ? j : n) - i - 1, dq);
			i = j + 1;
			continue;
		}
		if (c == ':' && (x->flags & X_ASSIGN) && !dq && i + 1 < n && s[i + 1] == '~') {
			put(x, ":", 1, litmode);
			i = tilde(x, s, n, i + 1);
			if (i < n && s[i] == '~') {
				put(x, "~", 1, litmode);
				i++;
			}
			continue;
		}
		put(x, &c, 1, dq || heredoc ? '1' : litmode);
		i++;
	}
}

/* ---- the entry points --------------------------------------------------------------------- */
void expand_words(char **words, size_t n, struct strlist *out)
{
	for (size_t i = 0; i < n; i++) {
		struct xb x = {.flags = X_SPLIT, .out = out};
		walk(&x, words[i], strlen(words[i]), false, '0');
		finish_segment(&x);
		sb_free(&x.t);
		sb_free(&x.m);
	}
	sl_nullterm(out);
}

char *expand_word(const char *w, int flags)
{
	struct strlist out = {0};
	struct xb x = {.flags = flags & ~X_SPLIT, .out = &out};
	walk(&x, w, strlen(w), false, '0');
	finish_segment(&x);
	sb_free(&x.t);
	sb_free(&x.m);
	sl_free(&out);
	return sb_steal(&x.joined);
}

char *expand_pattern(const char *w)
{
	struct strlist out = {0};
	struct xb x = {.flags = X_PATTERN, .out = &out};
	walk(&x, w, strlen(w), false, '0');
	char *pat = to_pattern(x.t.buf ? x.t.buf : "", x.m.buf ? x.m.buf : "", x.t.len);
	sb_free(&x.t);
	sb_free(&x.m);
	return pat;
}

char *expand_heredoc(const char *body)
{
	return expand_word(body, X_HEREDOC);
}

char *expand_string(const char *s)
{
	return expand_word(s, X_HEREDOC);
}

char *dequote(const char *w)
{
	struct strbuf sb = {0};
	size_t n = strlen(w);
	for (size_t i = 0; i < n; i++) {
		if (w[i] == '\\' && i + 1 < n)
			sb_addc(&sb, w[++i]);
		else if (w[i] == '\'' || w[i] == '"') {
			char q = w[i++];
			while (i < n && w[i] != q) {
				if (q == '"' && w[i] == '\\' && i + 1 < n && strchr("$`\"\\", w[i + 1]))
					i++;
				sb_addc(&sb, w[i++]);
			}
		} else
			sb_addc(&sb, w[i]);
	}
	return sb_steal(&sb);
}
