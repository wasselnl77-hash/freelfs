/* Tab completion: commands, files, variables, and per-command rules.
 *
 * Rules come from the complete builtin, usually in completion files that
 * load the first time a command's arguments are completed:
 * /usr/share/flsh/completions/NAME.flsh, then ~/.config/flsh/completions/.
 *
 *   complete flpkg -s '' -w 'install remove ...'    first argument
 *   complete flpkg -s remove -c 'flpkg list | cut -d" " -f1'
 *   complete cd -d                                   directories only
 * SPDX-License-Identifier: MIT */
#include "flsh.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct rule {
	struct rule *next;
	char *cmd;
	char *sub;   /* NULL: any argument; "": the first; else: after this first argument */
	char *words; /* -w */
	char *gen;   /* -c */
	bool files, dirs, commands;
};
static struct rule *rules;
static struct strlist loaded; /* commands whose completion file was tried */

int bi_complete(int argc, char **argv)
{
	if (argc == 1) {
		for (struct rule *r = rules; r; r = r->next) {
			printf("complete %s", r->cmd);
			if (r->sub)
				printf(" -s '%s'", r->sub);
			if (r->words)
				printf(" -w '%s'", r->words);
			if (r->gen)
				printf(" -c '%s'", r->gen);
			if (r->files)
				printf(" -f");
			if (r->dirs)
				printf(" -d");
			if (r->commands)
				printf(" -C");
			putchar('\n');
		}
		return 0;
	}
	struct rule *r = xcalloc(1, sizeof(*r));
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if ((!strcmp(a, "-s") || !strcmp(a, "-w") || !strcmp(a, "-c")) && i + 1 < argc) {
			char **slot = a[1] == 's' ? &r->sub : a[1] == 'w' ? &r->words : &r->gen;
			free(*slot);
			*slot = xstrdup(argv[++i]);
		} else if (!strcmp(a, "-f"))
			r->files = true;
		else if (!strcmp(a, "-d"))
			r->dirs = true;
		else if (!strcmp(a, "-C"))
			r->commands = true;
		else if (a[0] != '-' && !r->cmd)
			r->cmd = xstrdup(a);
		else {
			sh_warn("complete: %s: unknown option (see help complete)", a);
			free(r->cmd);
			free(r->sub);
			free(r->words);
			free(r->gen);
			free(r);
			return 2;
		}
	}
	if (!r->cmd) {
		sh_warn("complete: which command?");
		free(r);
		return 2;
	}
	/* append, so rules apply in the order given */
	struct rule **tail = &rules;
	while (*tail)
		tail = &(*tail)->next;
	*tail = r;
	return 0;
}

static void load_rules(const char *cmd)
{
	for (size_t i = 0; i < loaded.n; i++)
		if (!strcmp(loaded.v[i], cmd))
			return;
	sl_add(&loaded, cmd);
	if (strchr(cmd, '/'))
		return;
	char *sys = xasprintf("/usr/share/flsh/completions/%s.flsh", cmd);
	const char *home = var_get("HOME");
	char *user = home ? xasprintf("%s/.config/flsh/completions/%s.flsh", home, cmd) : NULL;
	int saved = sh.status;
	struct jmp j;
	j.prev = jmp_top;
	jmp_top = &j;
	if (setjmp(j.buf) == 0) {
		if (access(sys, R_OK) == 0)
			source_file(sys, false);
		if (user && access(user, R_OK) == 0)
			source_file(user, false);
	}
	jmp_top = j.prev;
	sh.status = saved;
	free(sys);
	free(user);
}

/* ---- candidates ----------------------------------------------------------------------------- */
static const char *special_chars = " \t\n'\"\\$`&|;<>()*?[]#!{}";

static char *quote_word(const char *s, bool keep_tilde)
{
	struct strbuf sb = {0};
	for (size_t i = 0; s[i]; i++) {
		if (strchr(special_chars, s[i]) || (s[i] == '~' && !(keep_tilde && i == 0)))
			sb_addc(&sb, '\\');
		sb_addc(&sb, s[i]);
	}
	return sb_steal(&sb);
}

static void add_cand(struct completion *c, const char *word, const char *shown)
{
	for (size_t i = 0; i < c->words.n; i++)
		if (!strcmp(c->words.v[i], word))
			return;
	sl_add(&c->words, word);
	sl_add(&c->shown, shown);
}

static void complete_files(struct completion *c, const char *word, bool dirs_only, bool exec_only)
{
	const char *slash = strrchr(word, '/');
	char *dirpart = slash ? xstrndup(word, (size_t)(slash - word + 1)) : xstrdup("");
	const char *base = slash ? slash + 1 : word;
	char *dir;
	if (dirpart[0] == '~' && (dirpart[1] == '/' || !dirpart[1])) {
		const char *home = var_get("HOME");
		dir = xasprintf("%s%s", home ? home : "", dirpart + 1);
	} else if (dirpart[0] == '~') {
		dir = expand_word(dirpart, X_ASSIGN);
	} else
		dir = xstrdup(*dirpart ? dirpart : ".");
	DIR *d = opendir(dir);
	if (!d) {
		free(dirpart);
		free(dir);
		return;
	}
	size_t blen = strlen(base);
	struct dirent *de;
	while ((de = readdir(d))) {
		const char *name = de->d_name;
		if (!strcmp(name, ".") || !strcmp(name, ".."))
			continue;
		if (name[0] == '.' && base[0] != '.')
			continue;
		if (strncmp(name, base, blen))
			continue;
		char *full = xasprintf("%s/%s", dir, name);
		struct stat st;
		bool isdir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
		bool isexec = !isdir && access(full, X_OK) == 0;
		free(full);
		if ((dirs_only && !isdir) || (exec_only && !isdir && !isexec))
			continue;
		char *rawword = xasprintf("%s%s%s", dirpart, name, isdir ? "/" : "");
		char *q = quote_word(rawword, true);
		char *shown = xasprintf("%s%s", name, isdir ? "/" : "");
		add_cand(c, q, shown);
		free(rawword);
		free(q);
		free(shown);
	}
	closedir(d);
	free(dirpart);
	free(dir);
}

static void add_words(struct completion *c, const char *text, const char *prefix)
{
	size_t plen = strlen(prefix);
	char *copy = xstrdup(text), *save = NULL;
	for (char *w = strtok_r(copy, " \t\n", &save); w; w = strtok_r(NULL, " \t\n", &save))
		if (!strncmp(w, prefix, plen)) {
			char *q = quote_word(w, false);
			add_cand(c, q, w);
			free(q);
		}
	free(copy);
}

static int cmp_shown(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* sort both lists by the shown text */
static void sort_cands(struct completion *c)
{
	size_t n = c->words.n;
	if (n < 2)
		return;
	char **pairs = xmalloc(n * 2 * sizeof(char *));
	char **keys = xmalloc(n * sizeof(char *));
	for (size_t i = 0; i < n; i++) {
		keys[i] = xasprintf("%s%c%zu", c->shown.v[i], '\001', i);
	}
	qsort(keys, n, sizeof(char *), cmp_shown);
	for (size_t i = 0; i < n; i++) {
		size_t idx = (size_t)strtoul(strrchr(keys[i], '\001') + 1, NULL, 10);
		pairs[i] = c->words.v[idx];
		pairs[n + i] = c->shown.v[idx];
		free(keys[i]);
	}
	for (size_t i = 0; i < n; i++) {
		c->words.v[i] = pairs[i];
		c->shown.v[i] = pairs[n + i];
	}
	free(pairs);
	free(keys);
}

void complete_line(const char *line, size_t cursor, struct completion *c)
{
	memset(c, 0, sizeof(*c));
	struct hl_tok *toks;
	size_t nt = hl_lex(line, cursor, &toks);
	/* the word being completed: a token ending at the cursor, or a new one */
	size_t start = cursor;
	int tok = -1;
	if (nt && toks[nt - 1].end == cursor && toks[nt - 1].kind != HL_OP &&
	    toks[nt - 1].kind != HL_COMMENT && toks[nt - 1].kind != HL_REDIR) {
		tok = (int)nt - 1;
		start = toks[tok].start;
	}
	if (nt && toks[nt - 1].kind == HL_COMMENT && toks[nt - 1].end == cursor) {
		free(toks);
		return;
	}
	bool cmdpos, after_redir = false;
	int cmd_tok = -1, argno = 0;
	if (tok >= 0) {
		int k = toks[tok].kind;
		cmdpos = k == HL_CMD || k == HL_CMD_BAD || k == HL_CMD_PART || k == HL_RESERVED ||
		         k == HL_ASSIGN;
		if (k == HL_ASSIGN) {
			/* complete the value after NAME= as a file */
			cmdpos = false;
			const char *eq = memchr(line + start, '=', cursor - start);
			start = (size_t)(eq - line) + 1;
		}
		cmd_tok = toks[tok].cmd_tok;
		argno = toks[tok].argno;
		after_redir = argno < 0;
	} else {
		/* a new word: what does the previous token make it? */
		if (!nt)
			cmdpos = true;
		else {
			struct hl_tok *p = &toks[nt - 1];
			if (p->kind == HL_OP || p->kind == HL_ASSIGN)
				cmdpos = true;
			else if (p->kind == HL_RESERVED) {
				char *w = xstrndup(line + p->start, p->end - p->start);
				cmdpos = strcmp(w, "for") && strcmp(w, "case") && strcmp(w, "in");
				free(w);
			} else if (p->kind == HL_REDIR) {
				cmdpos = false;
				after_redir = true;
			} else {
				cmdpos = false;
				cmd_tok = p->cmd_tok;
				argno = p->argno + 1;
				if (p->argno < 0) { /* after a redirection target */
					cmd_tok = p->cmd_tok;
					argno = 1;
					for (size_t i = 0; i < nt; i++)
						if (toks[i].cmd_tok == cmd_tok && toks[i].argno >= argno)
							argno = toks[i].argno + 1;
				}
			}
		}
	}
	c->start = start;
	c->end = cursor;
	char *raw = xstrndup(line + start, cursor - start);
	char *word = dequote(raw);

	if (word[0] == '$' && !strchr(word, '/')) {
		/* variable names */
		struct strlist names = {0};
		size_t plen = strlen(word + 1);
		var_names(&names);
		for (size_t i = 0; i < names.n; i++)
			if (!strncmp(names.v[i], word + 1, plen)) {
				char *w = xasprintf("$%s", names.v[i]);
				add_cand(c, w, names.v[i]);
				free(w);
			}
		sl_free(&names);
	} else if (cmdpos && !strchr(word, '/')) {
		struct strlist all = {0};
		commands_names(&all);
		size_t plen = strlen(word);
		for (size_t i = 0; i < all.n; i++)
			if (!strncmp(all.v[i], word, plen)) {
				char *q = quote_word(all.v[i], false);
				add_cand(c, q, all.v[i]);
				free(q);
			}
		sl_free(&all);
		/* directories too, for "cd"-less habits like ./script */
	} else if (cmdpos) {
		complete_files(c, word, false, true);
	} else if (after_redir || cmd_tok < 0) {
		complete_files(c, word, false, false);
	} else {
		/* the command's rules */
		char *cmdraw = xstrndup(line + toks[cmd_tok].start, toks[cmd_tok].end - toks[cmd_tok].start);
		char *cmd = dequote(cmdraw);
		free(cmdraw);
		const char *base = strrchr(cmd, '/') ? strrchr(cmd, '/') + 1 : cmd;
		load_rules(base);
		char *first = NULL; /* the first argument, for -s rules */
		for (size_t i = 0; i < nt; i++)
			if (toks[i].cmd_tok == cmd_tok && toks[i].argno == 1 && (int)i != tok) {
				char *fr = xstrndup(line + toks[i].start, toks[i].end - toks[i].start);
				first = dequote(fr);
				free(fr);
				break;
			}
		bool any = false, files = false, dirs = false, commands = false;
		for (struct rule *r = rules; r; r = r->next) {
			if (strcmp(r->cmd, base))
				continue;
			if (r->sub && (*r->sub ? !(argno >= 2 && first && !strcmp(first, r->sub)) : argno != 1))
				continue;
			any = true;
			if (r->words)
				add_words(c, r->words, word);
			if (r->gen) {
				char *out = command_subst(r->gen, strlen(r->gen));
				add_words(c, out, word);
				free(out);
			}
			files |= r->files;
			dirs |= r->dirs;
			commands |= r->commands;
		}
		if (commands && !strchr(word, '/')) {
			struct strlist all = {0};
			commands_names(&all);
			for (size_t i = 0; i < all.n; i++)
				if (!strncmp(all.v[i], word, strlen(word)))
					add_cand(c, all.v[i], all.v[i]);
			sl_free(&all);
		}
		if (!any || files || (dirs && !files))
			complete_files(c, word, dirs && !files, false);
		free(first);
		free(cmd);
	}
	sort_cands(c);
	/* a single directory keeps going: no space after it */
	if (c->words.n == 1) {
		size_t l = strlen(c->words.v[0]);
		c->nospace = l && c->words.v[0][l - 1] == '/';
	}
	free(raw);
	free(word);
	free(toks);
}

void completion_free(struct completion *c)
{
	sl_free(&c->words);
	sl_free(&c->shown);
}
