/* Syntax colours for the line being typed, the command name lookup
 * behind them, and "did you mean" for mistyped commands.
 *
 * The colours are the 16 standard ANSI ones, so the terminal's theme
 * decides the exact shades (the FreeLFS terminal uses the flboot palette).
 * SPDX-License-Identifier: MIT */
#include "flsh.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ---- command names ---------------------------------------------------------------------- */
static struct strlist path_cmds;
static long path_cmds_gen = -1;
static time_t path_cmds_time;

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static void scan_path(void)
{
	time_t now = time(NULL);
	if (path_cmds_gen == path_changes() && now - path_cmds_time < 30)
		return;
	path_cmds_gen = path_changes();
	path_cmds_time = now;
	sl_free(&path_cmds);
	const char *path = var_get("PATH");
	if (!path)
		return;
	char *copy = xstrdup(path), *save = NULL;
	for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
		DIR *d = opendir(dir);
		if (!d)
			continue;
		struct dirent *de;
		while ((de = readdir(d))) {
			if (de->d_name[0] == '.')
				continue;
			char *full = xasprintf("%s/%s", dir, de->d_name);
			struct stat st;
			if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111))
				sl_add(&path_cmds, de->d_name);
			free(full);
		}
		closedir(d);
	}
	free(copy);
	if (path_cmds.n > 1)
		qsort(path_cmds.v, path_cmds.n, sizeof(char *), cmp_str);
}

void commands_names(struct strlist *out)
{
	size_t n;
	const struct builtin *b = builtins_table(&n);
	for (size_t i = 0; i < n; i++)
		sl_add(out, b[i].name);
	function_names(out);
	struct strlist *al = alias_names();
	for (size_t i = 0; i < al->n; i++)
		sl_add(out, al->v[i]);
	scan_path();
	for (size_t i = 0; i < path_cmds.n; i++)
		sl_add(out, path_cmds.v[i]);
	if (out->n > 1)
		qsort(out->v, out->n, sizeof(char *), cmp_str);
	/* drop duplicates */
	size_t k = 0;
	for (size_t i = 0; i < out->n; i++) {
		if (k && !strcmp(out->v[k - 1], out->v[i])) {
			free(out->v[i]);
			continue;
		}
		out->v[k++] = out->v[i];
	}
	out->n = k;
	if (out->v)
		out->v[k] = NULL;
}

bool command_exists(const char *name)
{
	if (!*name)
		return false;
	if (find_builtin(name) || find_function(name) || alias_get(name) || is_reserved(name))
		return true;
	if (strchr(name, '/')) {
		char *p = name[0] == '~' ? expand_word(name, X_ASSIGN) : xstrdup(name);
		struct stat st;
		bool ok = stat(p, &st) == 0 && S_ISREG(st.st_mode) && access(p, X_OK) == 0;
		free(p);
		return ok;
	}
	char *p = find_command(name, true);
	free(p);
	return p != NULL;
}

static bool command_prefix(const char *name)
{
	size_t n = strlen(name);
	if (!n || strchr(name, '/'))
		return true; /* paths: do not judge while typing */
	struct strlist all = {0};
	commands_names(&all);
	bool found = false;
	for (size_t i = 0; i < all.n && !found; i++)
		found = !strncmp(all.v[i], name, n);
	sl_free(&all);
	return found;
}

/* Damerau-Levenshtein distance (with adjacent swaps), capped */
static int distance(const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b);
	if (la > 40 || lb > 40)
		return 99;
	int d[42][42];
	for (size_t i = 0; i <= la; i++)
		d[i][0] = (int)i;
	for (size_t j = 0; j <= lb; j++)
		d[0][j] = (int)j;
	for (size_t i = 1; i <= la; i++)
		for (size_t j = 1; j <= lb; j++) {
			int cost = a[i - 1] != b[j - 1];
			int v = d[i - 1][j] + 1;
			if (d[i][j - 1] + 1 < v)
				v = d[i][j - 1] + 1;
			if (d[i - 1][j - 1] + cost < v)
				v = d[i - 1][j - 1] + cost;
			if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] && d[i - 2][j - 2] + 1 < v)
				v = d[i - 2][j - 2] + 1;
			d[i][j] = v;
		}
	return d[la][lb];
}

const char *did_you_mean(const char *name)
{
	static char best[256];
	size_t n = strlen(name);
	if (n < 2)
		return NULL;
	int limit = n <= 4 ? 1 : 2, bestd = 99;
	struct strlist all = {0};
	commands_names(&all);
	for (size_t i = 0; i < all.n; i++) {
		int d = distance(name, all.v[i]);
		if (d > limit)
			continue;
		/* prefer closer, then the same first letter, then shorter */
		bool better = d < bestd ||
		              (d == bestd && all.v[i][0] == name[0] && best[0] != name[0]) ||
		              (d == bestd && (all.v[i][0] == name[0]) == (best[0] == name[0]) &&
		               strlen(all.v[i]) < strlen(best));
		if (better && strlen(all.v[i]) < sizeof(best)) {
			bestd = d;
			strcpy(best, all.v[i]);
		}
	}
	sl_free(&all);
	return bestd <= limit ? best : NULL;
}

/* ---- the highlighter's lexer -------------------------------------------------------------- */
static bool is_op_char(char c)
{
	return c == ';' || c == '&' || c == '|' || c == '(' || c == ')' || c == '\n';
}

static void add_tok(struct hl_tok **v, size_t *n, size_t *cap, size_t start, size_t end, int kind,
                    int cmd, int argno)
{
	if (*n == *cap) {
		*cap = *cap ? *cap * 2 : 16;
		*v = xrealloc(*v, *cap * sizeof(**v));
	}
	(*v)[*n] = (struct hl_tok){start, end, kind, cmd, argno};
	(*n)++;
}

/* Words that keep the next word in command position */
static bool keeps_cmdpos(const char *w)
{
	static const char *const kw[] = {"if", "then", "else", "elif", "do", "while", "until", "!",
	                                 "{", "}", "fi", "done", "esac", "time", NULL};
	for (int i = 0; kw[i]; i++)
		if (!strcmp(w, kw[i]))
			return true;
	return false;
}

size_t hl_lex(const char *s, size_t n, struct hl_tok **out)
{
	struct hl_tok *v = NULL;
	size_t nt = 0, cap = 0, i = 0;
	bool cmdpos = true, redir_target = false;
	int cmd = -1, argno = 0;
	while (i < n) {
		char c = s[i];
		if (c == ' ' || c == '\t') {
			i++;
			continue;
		}
		if (c == '\\' && i + 1 < n && s[i + 1] == '\n') {
			i += 2;
			continue;
		}
		if (c == '#') {
			size_t j = i;
			while (j < n && s[j] != '\n')
				j++;
			add_tok(&v, &nt, &cap, i, j, HL_COMMENT, -1, 0);
			i = j;
			continue;
		}
		if (is_op_char(c)) {
			size_t j = i + 1;
			if (j < n && (s[j] == c || (c == ';' && s[j] == '&')) && c != '(' && c != ')' && c != '\n')
				j++;
			add_tok(&v, &nt, &cap, i, j, HL_OP, -1, 0);
			cmdpos = c != ')';
			cmd = -1;
			argno = 0;
			i = j;
			continue;
		}
		/* redirections, with an optional fd number */
		size_t j = i;
		while (j < n && s[j] >= '0' && s[j] <= '9')
			j++;
		if (j < n && (s[j] == '<' || s[j] == '>')) {
			size_t k = j + 1;
			while (k < n && (s[k] == '<' || s[k] == '>' || s[k] == '&' || s[k] == '|' || s[k] == '-') &&
			       k - j < 3)
				k++;
			add_tok(&v, &nt, &cap, i, k, HL_REDIR, cmd, 0);
			redir_target = true;
			i = k;
			continue;
		}
		bool unterm = false;
		size_t end = scan_word(s, i, n, &unterm);
		if (end == i)
			end = i + 1;
		char *w = xstrndup(s + i, end - i);
		int kind;
		if (redir_target) {
			kind = HL_ARG;
			redir_target = false;
			add_tok(&v, &nt, &cap, i, end, kind, cmd, -1);
			free(w);
			i = end;
			continue;
		}
		if (cmdpos) {
			const char *eq = strchr(w, '=');
			if (eq && is_name(w, (size_t)(eq - w))) {
				add_tok(&v, &nt, &cap, i, end, HL_ASSIGN, -1, 0);
				free(w);
				i = end;
				continue;
			}
			if (is_reserved(w)) {
				add_tok(&v, &nt, &cap, i, end, HL_RESERVED, -1, 0);
				if (!keeps_cmdpos(w)) {
					cmdpos = false; /* for NAME / case WORD */
					cmd = -1;
					argno = 1;
				}
				free(w);
				i = end;
				continue;
			}
			char *name = dequote(w);
			if (command_exists(name))
				kind = HL_CMD;
			else if (end == n && command_prefix(name))
				kind = HL_CMD_PART;
			else
				kind = HL_CMD_BAD;
			free(name);
			cmd = (int)nt;
			argno = 0;
			add_tok(&v, &nt, &cap, i, end, kind, cmd, 0);
			cmdpos = false;
			argno = 1;
		} else {
			kind = w[0] == '-' ? HL_OPTION : HL_ARG;
			add_tok(&v, &nt, &cap, i, end, kind, cmd, argno++);
		}
		free(w);
		i = end;
	}
	*out = v;
	return nt;
}

/* ---- rendering ---------------------------------------------------------------------------- */
static const char *color_of(int kind)
{
	switch (kind) {
	case HL_CMD: return "\033[32m";
	case HL_CMD_BAD: return "\033[31m";
	case HL_RESERVED: return "\033[34m";
	case HL_ARG: return "\033[33m";
	case HL_OPTION: return "\033[36m";
	case HL_STRING: return "\033[33m";
	case HL_VAR: return "\033[35m";
	case HL_OP:
	case HL_REDIR:
	case HL_COMMENT: return "\033[90m";
	case HL_ASSIGN: return "\033[35m";
	default: return "";
	}
}

static bool path_exists(const char *s, size_t n)
{
	char *w = xstrndup(s, n);
	if (strpbrk(w, "$`*?[")) {
		free(w);
		return false;
	}
	char *d = dequote(w);
	free(w);
	if (d[0] == '~' && (d[1] == '/' || !d[1])) {
		const char *home = var_get("HOME");
		char *e = xasprintf("%s%s", home ? home : "", d + 1);
		free(d);
		d = e;
	}
	struct stat st;
	bool ok = *d && stat(d, &st) == 0;
	free(d);
	return ok;
}

/* Copy one character (or a control character as ^X) */
static size_t put_char(struct strbuf *out, const char *s, size_t i, size_t n)
{
	unsigned char c = (unsigned char)s[i];
	if (c == '\n') {
		sb_addstr(out, "\033[90m");
		sb_addstr(out, term_is_linux() ? "\xc2\xb6" : "\xe2\x86\xb5"); /* ¶ or ↵ */
		sb_addstr(out, "\033[0m");
		return 1;
	}
	if (c < 0x20 || c == 0x7f) {
		sb_addc(out, '^');
		sb_addc(out, c == 0x7f ? '?' : (char)(c + '@'));
		return 1;
	}
	size_t len = (size_t)utf8_len(c);
	if (i + len > n)
		len = n - i;
	sb_add(out, s + i, len);
	return len;
}

/* Inside a word: strings and $variables get their own colour */
static void render_word(struct strbuf *out, const char *s, size_t start, size_t end, int kind)
{
	const char *base = color_of(kind);
	bool underline = kind == HL_ARG && path_exists(s + start, end - start);
	sb_addstr(out, base);
	if (underline)
		sb_addstr(out, "\033[4m");
	size_t i = start;
	while (i < end) {
		char c = s[i];
		if ((c == '\'' || c == '"') && kind != HL_COMMENT) {
			size_t j = i + 1;
			while (j < end && s[j] != c) {
				if (c == '"' && s[j] == '\\' && j + 1 < end)
					j++;
				j++;
			}
			if (j < end)
				j++;
			sb_addstr(out, color_of(HL_STRING));
			while (i < j)
				i += put_char(out, s, i, end);
			sb_addstr(out, "\033[0m");
			sb_addstr(out, base);
			if (underline)
				sb_addstr(out, "\033[4m");
			continue;
		}
		if (c == '$' && i + 1 < end && kind != HL_COMMENT) {
			bool un = false;
			size_t j = scan_dollar(s, i, end, &un);
			if (j <= i + 1)
				j = i + 1;
			sb_addstr(out, color_of(HL_VAR));
			while (i < j)
				i += put_char(out, s, i, end);
			sb_addstr(out, "\033[0m");
			sb_addstr(out, base);
			if (underline)
				sb_addstr(out, "\033[4m");
			continue;
		}
		i += put_char(out, s, i, end);
	}
	sb_addstr(out, "\033[0m");
}

void hl_render(const char *s, size_t n, size_t cursor, struct strbuf *out, unsigned char *kinds)
{
	struct hl_tok *toks;
	size_t nt = hl_lex(s, n, &toks);
	size_t i = 0;
	for (size_t t = 0; t < nt; t++) {
		while (i < toks[t].start)
			i += put_char(out, s, i, n);
		int kind = toks[t].kind;
		if (kind == HL_CMD_PART && toks[t].end != cursor)
			kind = HL_CMD_BAD;
		render_word(out, s, toks[t].start, toks[t].end, kind);
		i = toks[t].end;
	}
	while (i < n)
		i += put_char(out, s, i, n);
	free(toks);
	(void)kinds;
}
