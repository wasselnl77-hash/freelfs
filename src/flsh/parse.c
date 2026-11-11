/* The lexer and the parser: POSIX shell grammar into a tree of nodes.
 *
 * Words stay raw (quotes and all) and are expanded when they run. The
 * parser works on a whole string and hands out one complete command at a
 * time, so scripts run command by command, and an interactive shell can
 * tell "needs another line" (an open quote, an if without fi) from a real
 * syntax error.
 * SPDX-License-Identifier: MIT */
#include "flsh.h"

#include <stdlib.h>
#include <string.h>

/* ---- arenas: every node of one parse lives in one arena --------------------------- */
struct block {
	struct block *next;
	size_t used, size;
	char data[];
};
struct arena {
	struct block *blocks;
	int refs;
};

struct arena *arena_new(void)
{
	struct arena *a = xcalloc(1, sizeof(*a));
	a->refs = 1;
	return a;
}

void *arena_alloc(struct arena *a, size_t n)
{
	n = (n + 15) & ~(size_t)15;
	struct block *b = a->blocks;
	if (!b || b->used + n > b->size) {
		size_t size = n > 8192 ? n : 8192;
		b = xmalloc(sizeof(*b) + size);
		b->size = size;
		b->used = 0;
		b->next = a->blocks;
		a->blocks = b;
	}
	void *p = b->data + b->used;
	b->used += n;
	memset(p, 0, n);
	return p;
}

char *arena_strndup(struct arena *a, const char *s, size_t n)
{
	char *p = arena_alloc(a, n + 1);
	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

void arena_ref(struct arena *a)
{
	a->refs++;
}

void arena_unref(struct arena *a)
{
	if (!a || --a->refs > 0)
		return;
	for (struct block *b = a->blocks, *next; b; b = next) {
		next = b->next;
		free(b);
	}
	free(a);
}

/* ---- word scanning ---------------------------------------------------------------- */
bool is_name(const char *s, size_t n)
{
	if (!n || !(s[0] == '_' || (s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z')))
		return false;
	for (size_t i = 1; i < n; i++)
		if (!(s[i] == '_' || (s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
		      (s[i] >= '0' && s[i] <= '9')))
			return false;
	return true;
}

static const char *const reserved[] = {"if", "then", "else", "elif", "fi", "do", "done", "case",
                                       "esac", "while", "until", "for", "in", "{", "}", "!",
                                       "function", NULL};

bool is_reserved(const char *w)
{
	for (int i = 0; reserved[i]; i++)
		if (!strcmp(w, reserved[i]))
			return true;
	return false;
}

static bool is_meta(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == ';' || c == '&' || c == '|' || c == '<' ||
	       c == '>' || c == '(' || c == ')';
}

static size_t scan_squote(const char *s, size_t i, size_t n, bool *unterm)
{
	i++;
	while (i < n && s[i] != '\'')
		i++;
	if (i >= n) {
		*unterm = true;
		return n;
	}
	return i + 1;
}

static size_t scan_bquote(const char *s, size_t i, size_t n, bool *unterm)
{
	i++;
	while (i < n && s[i] != '`') {
		if (s[i] == '\\' && i + 1 < n)
			i++;
		i++;
	}
	if (i >= n) {
		*unterm = true;
		return n;
	}
	return i + 1;
}

static size_t scan_dquote(const char *s, size_t i, size_t n, bool *unterm)
{
	i++;
	while (i < n && s[i] != '"') {
		if (s[i] == '\\' && i + 1 < n)
			i += 2;
		else if (s[i] == '$')
			i = scan_dollar(s, i, n, unterm);
		else if (s[i] == '`')
			i = scan_bquote(s, i, n, unterm);
		else
			i++;
	}
	if (i >= n) {
		*unterm = true;
		return n;
	}
	return i + 1;
}

/* $(...), $((...)), ${...}, $name: returns the index after it */
size_t scan_dollar(const char *s, size_t i, size_t n, bool *unterm)
{
	i++;
	if (i >= n)
		return i;
	if (s[i] == '{') {
		i++;
		while (i < n && s[i] != '}') {
			if (s[i] == '\\' && i + 1 < n)
				i += 2;
			else if (s[i] == '\'')
				i = scan_squote(s, i, n, unterm);
			else if (s[i] == '"')
				i = scan_dquote(s, i, n, unterm);
			else if (s[i] == '$')
				i = scan_dollar(s, i, n, unterm);
			else if (s[i] == '`')
				i = scan_bquote(s, i, n, unterm);
			else
				i++;
		}
		if (i >= n) {
			*unterm = true;
			return n;
		}
		return i + 1;
	}
	if (s[i] == '(') {
		/* command substitution or arithmetic: match the parentheses,
		 * skipping quotes and comments; "case x in a)" inside counts its ")"
		 * against an "in" so it does not close early */
		int depth = 1, cases = 0;
		i++;
		bool wordstart = true;
		while (i < n && depth > 0) {
			char c = s[i];
			if (c == '\\' && i + 1 < n) {
				i += 2;
				wordstart = false;
				continue;
			}
			if (c == '\'') {
				i = scan_squote(s, i, n, unterm);
				wordstart = false;
				continue;
			}
			if (c == '"') {
				i = scan_dquote(s, i, n, unterm);
				wordstart = false;
				continue;
			}
			if (c == '`') {
				i = scan_bquote(s, i, n, unterm);
				wordstart = false;
				continue;
			}
			if (c == '$') {
				i = scan_dollar(s, i, n, unterm);
				wordstart = false;
				continue;
			}
			if (c == '#' && wordstart) {
				while (i < n && s[i] != '\n')
					i++;
				continue;
			}
			if (wordstart && i + 4 <= n && !strncmp(s + i, "case", 4) &&
			    (i + 4 == n || is_meta(s[i + 4]))) {
				cases++;
				i += 4;
				wordstart = false;
				continue;
			}
			if (wordstart && cases && i + 4 <= n && !strncmp(s + i, "esac", 4) &&
			    (i + 4 == n || is_meta(s[i + 4]))) {
				cases--;
				i += 4;
				wordstart = false;
				continue;
			}
			if (c == '(')
				depth++;
			else if (c == ')') {
				if (cases)
					; /* a case pattern's ")" */
				else
					depth--;
			}
			wordstart = is_meta(c);
			i++;
		}
		if (depth > 0) {
			*unterm = true;
			return n;
		}
		return i;
	}
	if (is_name(s + i, 1)) {
		while (i < n && is_name(s + i, 1) | (s[i] >= '0' && s[i] <= '9'))
			i++;
		return i;
	}
	if (strchr("@*#?-$!0123456789", s[i]))
		return i + 1;
	if (s[i] == '\'') { /* $'...': backslash escapes, \' too */
		i++;
		while (i < n && s[i] != '\'') {
			if (s[i] == '\\' && i + 1 < n)
				i++;
			i++;
		}
		if (i >= n) {
			*unterm = true;
			return n;
		}
		return i + 1;
	}
	return i;
}

size_t scan_word(const char *s, size_t i, size_t n, bool *unterm)
{
	while (i < n && !is_meta(s[i])) {
		char c = s[i];
		if (c == '\\') {
			if (i + 1 >= n) {
				*unterm = true; /* a trailing backslash continues the line */
				return n;
			}
			i += 2;
		} else if (c == '\'')
			i = scan_squote(s, i, n, unterm);
		else if (c == '"')
			i = scan_dquote(s, i, n, unterm);
		else if (c == '`')
			i = scan_bquote(s, i, n, unterm);
		else if (c == '$')
			i = scan_dollar(s, i, n, unterm);
		else
			i++;
	}
	return i;
}

/* ---- the lexer ---------------------------------------------------------------------- */
struct alias_frame {
	struct alias_frame *next;
	const char *name;
	size_t end;
};

static jmp_buf *parse_jmp;
bool parse_quiet;

__attribute__((noreturn)) static void parse_fail(struct parser *p, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char *m;
	if (vasprintf(&m, fmt, ap) < 0)
		m = NULL;
	va_end(ap);
	sh.lineno = p->lineno;
	if (!parse_quiet)
		sh_warn("syntax error: %s", m ? m : "?");
	free(m);
	longjmp(*parse_jmp, 1);
}

__attribute__((noreturn)) static void parse_incomplete(struct parser *p)
{
	p->incomplete = true;
	longjmp(*parse_jmp, 1);
}

void parser_init(struct parser *p, const char *src, size_t len, int lineno)
{
	memset(p, 0, sizeof(*p));
	p->arena = arena_new();
	p->src = arena_strndup(p->arena, src, len);
	p->len = len;
	p->lineno = lineno;
	p->pending_tail = &p->pending_docs;
}

static char *dequote_plain(struct arena *a, const char *w, bool *quoted)
{
	size_t n = strlen(w);
	char *out = arena_alloc(a, n + 1), *o = out;
	*quoted = false;
	for (size_t i = 0; i < n; i++) {
		if (w[i] == '\\' && i + 1 < n) {
			*quoted = true;
			*o++ = w[++i];
		} else if (w[i] == '\'' || w[i] == '"') {
			*quoted = true;
			char q = w[i++];
			while (i < n && w[i] != q)
				*o++ = w[i++];
		} else
			*o++ = w[i];
	}
	*o = '\0';
	return out;
}

struct docref {
	struct docref *next;
	struct redir *r;
};

static void read_heredocs(struct parser *p)
{
	for (struct docref *d = p->pending_docs; d; d = d->next) {
		struct redir *r = d->r;
		struct strbuf body = {0};
		for (;;) {
			if (p->pos >= p->len) {
				sb_free(&body);
				parse_incomplete(p);
			}
			size_t start = p->pos;
			while (p->pos < p->len && p->src[p->pos] != '\n')
				p->pos++;
			size_t end = p->pos;
			bool had_nl = p->pos < p->len;
			if (had_nl)
				p->pos++;
			p->lineno++;
			if (r->strip_tabs)
				while (start < end && p->src[start] == '\t')
					start++;
			if (end - start == strlen(r->delim) && !strncmp(p->src + start, r->delim, end - start))
				break;
			if (!had_nl) {
				sb_free(&body);
				parse_incomplete(p);
			}
			sb_add(&body, p->src + start, end - start);
			sb_addc(&body, '\n');
		}
		r->word = arena_strndup(p->arena, body.buf ? body.buf : "", body.len);
		sb_free(&body);
	}
	p->pending_docs = NULL;
	p->pending_tail = &p->pending_docs;
}

static bool alias_active(struct parser *p, const char *name)
{
	for (struct alias_frame *f = p->alias; f; f = f->next)
		if (p->pos < f->end && !strcmp(f->name, name))
			return true;
	return false;
}

/* Replace the word at [start, end) with an alias's text. */
static void splice_alias(struct parser *p, size_t start, size_t end, const char *name,
                         const char *value)
{
	size_t vlen = strlen(value), nlen = p->len - (end - start) + vlen;
	char *s = arena_alloc(p->arena, nlen + 1);
	memcpy(s, p->src, start);
	memcpy(s + start, value, vlen);
	memcpy(s + start + vlen, p->src + end, p->len - end);
	s[nlen] = '\0';
	for (struct alias_frame *f = p->alias; f; f = f->next)
		if (f->end > start)
			f->end = f->end + vlen - (end - start);
	struct alias_frame *f = arena_alloc(p->arena, sizeof(*f));
	f->name = arena_strndup(p->arena, name, strlen(name));
	f->end = start + vlen;
	f->next = p->alias;
	p->alias = f;
	p->src = s;
	p->len = nlen;
	p->pos = start;
}

static void lex(struct parser *p, bool cmdpos)
{
	const char *s;
again:
	s = p->src; /* a spliced alias replaces the text */
	/* blanks, line continuations, comments */
	for (;;) {
		if (p->pos < p->len && (s[p->pos] == ' ' || s[p->pos] == '\t'))
			p->pos++;
		else if (p->pos + 1 < p->len && s[p->pos] == '\\' && s[p->pos + 1] == '\n') {
			p->pos += 2;
			p->lineno++;
		} else if (p->pos < p->len && s[p->pos] == '#') {
			while (p->pos < p->len && s[p->pos] != '\n')
				p->pos++;
		} else
			break;
	}
	p->tok_start = p->pos;
	p->tok_line = p->lineno;
	p->tokword = NULL;
	p->tok_quoted = false;
	p->tok_io = -1;
	if (p->pos >= p->len) {
		p->tok = T_EOF;
		return;
	}
	char c = s[p->pos], d = p->pos + 1 < p->len ? s[p->pos + 1] : '\0';
	if (c == '\n') {
		p->pos++;
		p->lineno++;
		p->tok = T_NEWLINE;
		if (p->pending_docs)
			read_heredocs(p);
		return;
	}
	if (c == ';') {
		p->pos += d == ';' || d == '&' ? 2 : 1;
		p->tok = d == ';' ? T_DSEMI : d == '&' ? T_SEMIAND : T_SEMI;
		return;
	}
	if (c == '&') {
		p->pos += d == '&' ? 2 : 1;
		p->tok = d == '&' ? T_ANDIF : T_AMP;
		return;
	}
	if (c == '|') {
		p->pos += d == '|' ? 2 : 1;
		p->tok = d == '|' ? T_ORIF : T_PIPE;
		return;
	}
	if (c == '(' || c == ')') {
		p->pos++;
		p->tok = c == '(' ? T_LPAREN : T_RPAREN;
		return;
	}
	/* an io number: digits right before < or > */
	size_t j = p->pos;
	while (j < p->len && s[j] >= '0' && s[j] <= '9')
		j++;
	if (j > p->pos && j < p->len && (s[j] == '<' || s[j] == '>') && j - p->pos < 4) {
		p->tok_io = atoi(s + p->pos);
		p->pos = j;
		c = s[j];
		d = j + 1 < p->len ? s[j + 1] : '\0';
	}
	if (c == '<' || c == '>') {
		static const char *const ops[] = {"<<-", "<<", "<>", "<&", ">>", ">|", ">&", "<", ">", NULL};
		for (int k = 0; ops[k]; k++)
			if (!strncmp(s + p->pos, ops[k], strlen(ops[k]))) {
				p->tokword = (char *)ops[k];
				p->pos += strlen(ops[k]);
				break;
			}
		p->tok = T_REDIR;
		return;
	}
	(void)d;
	bool unterm = false;
	size_t end = scan_word(s, p->pos, p->len, &unterm);
	if (unterm)
		parse_incomplete(p);
	for (size_t k = p->pos; k < end; k++)
		if (s[k] == '\'' || s[k] == '"' || s[k] == '\\')
			p->tok_quoted = true;
		else if (s[k] == '\n')
			p->lineno++;
	p->tokword = arena_strndup(p->arena, s + p->pos, end - p->pos);
	p->tok = T_WORD;
	bool next = p->alias_next && p->tok_start >= p->alias_next_pos;
	if (next)
		p->alias_next = false;
	if ((cmdpos || next) && !p->tok_quoted) {
		const char *v = alias_get(p->tokword);
		if (v && !alias_active(p, p->tokword)) {
			size_t vlen = strlen(v), start = p->pos;
			splice_alias(p, p->pos, end, p->tokword, v);
			/* an alias ending in a blank: the word after it is checked too */
			if (vlen && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t')) {
				p->alias_next = true;
				p->alias_next_pos = start + vlen;
			}
			goto again;
		}
	}
	p->pos = end;
}

static int peek_tok(struct parser *p)
{
	if (!p->peeked) {
		lex(p, false);
		p->peeked = true;
		p->check_alias = false;
	}
	return p->tok;
}

/* Peek at a command position: aliases apply. */
static int peek_cmd(struct parser *p)
{
	if (p->peeked && !p->check_alias && p->tok == T_WORD && !p->tok_quoted) {
		p->pos = p->tok_start; /* lex it again, now as a command word */
		p->lineno = p->tok_line;
		p->peeked = false;
	}
	if (!p->peeked) {
		lex(p, true);
		p->peeked = true;
	}
	p->check_alias = true;
	return p->tok;
}

static void consume(struct parser *p)
{
	p->peeked = false;
}

static bool is_kw(struct parser *p, const char *kw)
{
	return p->tok == T_WORD && !p->tok_quoted && !strcmp(p->tokword, kw);
}

static const char *tok_desc(struct parser *p)
{
	switch (p->tok) {
	case T_EOF: return "end of file";
	case T_NEWLINE: return "newline";
	case T_SEMI: return "';'";
	case T_AMP: return "'&'";
	case T_PIPE: return "'|'";
	case T_ANDIF: return "'&&'";
	case T_ORIF: return "'||'";
	case T_DSEMI: return "';;'";
	case T_SEMIAND: return "';&'";
	case T_LPAREN: return "'('";
	case T_RPAREN: return "')'";
	default: return p->tokword ? p->tokword : "?";
	}
}

__attribute__((noreturn)) static void unexpected(struct parser *p)
{
	if (p->tok == T_EOF)
		parse_incomplete(p);
	parse_fail(p, "unexpected %s%s%s", p->tok == T_WORD || p->tok == T_REDIR ? "'" : "",
	           tok_desc(p), p->tok == T_WORD || p->tok == T_REDIR ? "'" : "");
}

static void expect_kw(struct parser *p, const char *kw)
{
	peek_cmd(p);
	if (!is_kw(p, kw)) {
		if (p->tok == T_EOF)
			parse_incomplete(p);
		parse_fail(p, "expected '%s' but found %s", kw, tok_desc(p));
	}
	consume(p);
}

static void skip_newlines(struct parser *p)
{
	while (peek_tok(p) == T_NEWLINE)
		consume(p);
}

/* ---- the grammar ---------------------------------------------------------------------- */
static struct node *parse_and_or(struct parser *p);
static struct node *parse_command(struct parser *p);

static struct node *new_node(struct parser *p, enum ntype t)
{
	struct node *n = arena_alloc(p->arena, sizeof(*n));
	n->type = t;
	n->lineno = p->lineno;
	return n;
}

static bool is_terminator(struct parser *p, const char *const *terms)
{
	if (p->tok == T_EOF || p->tok == T_RPAREN || p->tok == T_DSEMI || p->tok == T_SEMIAND)
		return true;
	if (p->tok != T_WORD || p->tok_quoted)
		return false;
	for (int i = 0; terms && terms[i]; i++)
		if (!strcmp(p->tokword, terms[i]))
			return true;
	return false;
}

/* A compound list: and-ors separated by ; & or newlines, up to one of terms. */
static struct node *parse_list(struct parser *p, const char *const *terms)
{
	struct node *list = NULL, **tail = &list;
	for (;;) {
		skip_newlines(p);
		peek_cmd(p);
		if (is_terminator(p, terms))
			break;
		struct node *n = parse_and_or(p);
		int t = peek_tok(p);
		if (t == T_AMP) {
			consume(p);
			struct node *bg = new_node(p, N_BG);
			bg->body = n;
			n = bg;
		} else if (t == T_SEMI)
			consume(p);
		else if (t != T_NEWLINE && !is_terminator(p, terms))
			unexpected(p);
		if (!list)
			*tail = n;
		else {
			struct node *seq = new_node(p, N_SEQ);
			seq->bin.left = *tail;
			seq->bin.right = n;
			*tail = seq;
		}
	}
	if (!list) {
		peek_tok(p);
		unexpected(p);
	}
	return list;
}

static struct node *parse_pipeline(struct parser *p)
{
	bool bang = false;
	peek_cmd(p);
	if (is_kw(p, "!")) {
		consume(p);
		bang = true;
	}
	struct node *first = parse_command(p);
	if (peek_tok(p) != T_PIPE && !bang)
		return first;
	struct node *n = new_node(p, N_PIPE);
	size_t cap = 4;
	n->pipe.cmds = arena_alloc(p->arena, cap * sizeof(struct node *));
	n->pipe.cmds[n->pipe.n++] = first;
	n->pipe.bang = bang;
	while (peek_tok(p) == T_PIPE) {
		consume(p);
		skip_newlines(p);
		if (n->pipe.n == cap) {
			struct node **v = arena_alloc(p->arena, cap * 2 * sizeof(struct node *));
			memcpy(v, n->pipe.cmds, cap * sizeof(struct node *));
			n->pipe.cmds = v;
			cap *= 2;
		}
		n->pipe.cmds[n->pipe.n++] = parse_command(p);
	}
	return n;
}

static struct node *parse_and_or(struct parser *p)
{
	struct node *left = parse_pipeline(p);
	for (;;) {
		int t = peek_tok(p);
		if (t != T_ANDIF && t != T_ORIF)
			return left;
		consume(p);
		skip_newlines(p);
		struct node *n = new_node(p, t == T_ANDIF ? N_AND : N_OR);
		n->bin.left = left;
		n->bin.right = parse_pipeline(p);
		left = n;
	}
}

static struct redir *parse_redir(struct parser *p)
{
	const char *op = p->tokword;
	struct redir *r = arena_alloc(p->arena, sizeof(*r));
	r->fd = p->tok_io;
	consume(p);
	if (!strcmp(op, "<")) r->op = R_IN;
	else if (!strcmp(op, ">")) r->op = R_OUT;
	else if (!strcmp(op, ">>")) r->op = R_APPEND;
	else if (!strcmp(op, ">|")) r->op = R_CLOBBER;
	else if (!strcmp(op, "<>")) r->op = R_RDWR;
	else if (!strcmp(op, "<&")) r->op = R_DUPIN;
	else if (!strcmp(op, ">&")) r->op = R_DUPOUT;
	else {
		r->op = R_HEREDOC;
		r->strip_tabs = op[2] == '-';
	}
	if (r->fd < 0)
		r->fd = r->op == R_IN || r->op == R_RDWR || r->op == R_DUPIN || r->op == R_HEREDOC ? 0 : 1;
	if (peek_tok(p) != T_WORD)
		unexpected(p);
	if (r->op == R_HEREDOC) {
		bool quoted;
		r->delim = dequote_plain(p->arena, p->tokword, &quoted);
		r->quoted_doc = quoted;
		struct docref *d = arena_alloc(p->arena, sizeof(*d));
		d->r = r;
		*p->pending_tail = d;
		p->pending_tail = &d->next;
	} else
		r->word = p->tokword;
	consume(p);
	return r;
}

/* Redirections after a compound command */
static void parse_trailing_redirs(struct parser *p, struct node *n)
{
	struct redir **tail = &n->redirs;
	while (peek_tok(p) == T_REDIR) {
		struct redir *r = parse_redir(p);
		*tail = r;
		tail = &r->next;
	}
}

static void add_redir(struct redir ***tail, struct redir *r)
{
	**tail = r;
	*tail = &r->next;
}

static bool assign_word(const char *w)
{
	const char *eq = strchr(w, '=');
	return eq && eq > w && is_name(w, (size_t)(eq - w));
}

static struct node *parse_simple(struct parser *p)
{
	struct node *n = new_node(p, N_SIMPLE);
	struct strlist assigns = {0}, words = {0};
	struct redir *redirs = NULL, **rtail = &redirs;
	for (;;) {
		int t = words.n ? peek_tok(p) : peek_cmd(p);
		if (t == T_REDIR) {
			add_redir(&rtail, parse_redir(p));
			continue;
		}
		if (t != T_WORD)
			break;
		if (!words.n && assign_word(p->tokword))
			sl_add(&assigns, p->tokword);
		else
			sl_add(&words, p->tokword);
		consume(p);
	}
	if (!assigns.n && !words.n && !redirs) {
		peek_tok(p);
		unexpected(p);
	}
	n->simple.nassigns = assigns.n;
	n->simple.assigns = arena_alloc(p->arena, (assigns.n + 1) * sizeof(char *));
	for (size_t i = 0; i < assigns.n; i++)
		n->simple.assigns[i] = arena_strndup(p->arena, assigns.v[i], strlen(assigns.v[i]));
	n->simple.nwords = words.n;
	n->simple.words = arena_alloc(p->arena, (words.n + 1) * sizeof(char *));
	for (size_t i = 0; i < words.n; i++)
		n->simple.words[i] = arena_strndup(p->arena, words.v[i], strlen(words.v[i]));
	n->redirs = redirs;
	sl_free(&assigns);
	sl_free(&words);
	return n;
}

static const char *const T_THEN[] = {"then", NULL};
static const char *const T_ELSE[] = {"elif", "else", "fi", NULL};
static const char *const T_DO[] = {"do", NULL};
static const char *const T_DONE[] = {"done", NULL};
static const char *const T_RBRACE[] = {"}", NULL};
static const char *const T_ESAC[] = {"esac", NULL};

static struct node *parse_if(struct parser *p)
{
	struct node *n = new_node(p, N_IF);
	n->ifn.cond = parse_list(p, T_THEN);
	expect_kw(p, "then");
	n->ifn.then = parse_list(p, T_ELSE);
	peek_cmd(p);
	if (is_kw(p, "elif")) {
		consume(p);
		n->ifn.els = parse_if(p);
		return n;
	}
	if (is_kw(p, "else")) {
		consume(p);
		n->ifn.els = parse_list(p, (const char *const[]){"fi", NULL});
	}
	expect_kw(p, "fi");
	return n;
}

static struct node *parse_for(struct parser *p)
{
	struct node *n = new_node(p, N_FOR);
	if (peek_tok(p) != T_WORD || !is_name(p->tokword, strlen(p->tokword)))
		unexpected(p);
	n->forn.var = p->tokword;
	consume(p);
	skip_newlines(p);
	peek_cmd(p);
	if (is_kw(p, "in")) {
		consume(p);
		n->forn.has_in = true;
		struct strlist w = {0};
		while (peek_tok(p) == T_WORD) {
			sl_add(&w, p->tokword);
			consume(p);
		}
		n->forn.nwords = w.n;
		n->forn.words = arena_alloc(p->arena, (w.n + 1) * sizeof(char *));
		for (size_t i = 0; i < w.n; i++)
			n->forn.words[i] = arena_strndup(p->arena, w.v[i], strlen(w.v[i]));
		sl_free(&w);
		int t = peek_tok(p);
		if (t != T_SEMI && t != T_NEWLINE)
			unexpected(p);
		consume(p);
	} else if (peek_tok(p) == T_SEMI)
		consume(p);
	skip_newlines(p);
	expect_kw(p, "do");
	n->forn.body = parse_list(p, T_DONE);
	expect_kw(p, "done");
	return n;
}

static struct node *parse_case(struct parser *p)
{
	struct node *n = new_node(p, N_CASE);
	if (peek_tok(p) != T_WORD)
		unexpected(p);
	n->casen.word = p->tokword;
	consume(p);
	skip_newlines(p);
	expect_kw(p, "in");
	struct caseitem **tail = &n->casen.items;
	for (;;) {
		skip_newlines(p);
		peek_cmd(p);
		if (is_kw(p, "esac")) {
			consume(p);
			break;
		}
		if (p->tok == T_LPAREN) {
			consume(p);
			peek_tok(p);
		}
		struct caseitem *it = arena_alloc(p->arena, sizeof(*it));
		struct strlist pats = {0};
		for (;;) {
			if (p->tok != T_WORD)
				unexpected(p);
			sl_add(&pats, p->tokword);
			consume(p);
			if (peek_tok(p) == T_PIPE) {
				consume(p);
				peek_tok(p);
				continue;
			}
			if (p->tok != T_RPAREN)
				unexpected(p);
			consume(p);
			break;
		}
		it->npats = pats.n;
		it->pats = arena_alloc(p->arena, pats.n * sizeof(char *));
		for (size_t i = 0; i < pats.n; i++)
			it->pats[i] = arena_strndup(p->arena, pats.v[i], strlen(pats.v[i]));
		sl_free(&pats);
		skip_newlines(p);
		peek_cmd(p);
		if (!is_kw(p, "esac") && p->tok != T_DSEMI && p->tok != T_SEMIAND)
			it->body = parse_list(p, T_ESAC);
		*tail = it;
		tail = &it->next;
		peek_cmd(p);
		if (p->tok == T_DSEMI || p->tok == T_SEMIAND) {
			it->fallthrough = p->tok == T_SEMIAND;
			consume(p);
			continue;
		}
		if (is_kw(p, "esac")) {
			consume(p);
			break;
		}
		unexpected(p);
	}
	return n;
}

static struct node *parse_function_body(struct parser *p, const char *name)
{
	skip_newlines(p);
	struct node *n = new_node(p, N_FUNC);
	n->func.name = arena_strndup(p->arena, name, strlen(name));
	struct node *body = parse_command(p);
	if (body->type == N_SIMPLE || body->type == N_FUNC)
		parse_fail(p, "a function body must be a compound command, like { ...; }");
	n->func.body = body;
	return n;
}

static struct node *parse_command(struct parser *p)
{
	int t = peek_cmd(p);
	struct node *n;
	p->depth++;
	if (t == T_LPAREN) {
		consume(p);
		n = new_node(p, N_SUBSHELL);
		n->body = parse_list(p, NULL);
		if (peek_tok(p) != T_RPAREN)
			unexpected(p);
		consume(p);
		parse_trailing_redirs(p, n);
	} else if (t == T_WORD && !p->tok_quoted && is_reserved(p->tokword)) {
		const char *w = p->tokword;
		if (!strcmp(w, "if")) {
			consume(p);
			n = parse_if(p);
		} else if (!strcmp(w, "while") || !strcmp(w, "until")) {
			consume(p);
			n = new_node(p, w[0] == 'w' ? N_WHILE : N_UNTIL);
			n->loop.cond = parse_list(p, T_DO);
			expect_kw(p, "do");
			n->loop.body = parse_list(p, T_DONE);
			expect_kw(p, "done");
		} else if (!strcmp(w, "for")) {
			consume(p);
			n = parse_for(p);
		} else if (!strcmp(w, "case")) {
			consume(p);
			n = parse_case(p);
		} else if (!strcmp(w, "{")) {
			consume(p);
			n = new_node(p, N_BRACE);
			n->body = parse_list(p, T_RBRACE);
			expect_kw(p, "}");
		} else if (!strcmp(w, "function")) {
			consume(p);
			if (peek_tok(p) != T_WORD)
				unexpected(p);
			const char *name = p->tokword;
			consume(p);
			if (peek_tok(p) == T_LPAREN) {
				consume(p);
				if (peek_tok(p) != T_RPAREN)
					unexpected(p);
				consume(p);
			}
			n = parse_function_body(p, name);
			p->depth--;
			return n;
		} else
			unexpected(p);
		parse_trailing_redirs(p, n);
	} else if (t == T_WORD || t == T_REDIR) {
		/* NAME ( ) is a function definition */
		if (t == T_WORD && !p->tok_quoted && is_name(p->tokword, strlen(p->tokword))) {
			size_t k = p->pos; /* just after the name */
			while (k < p->len && (p->src[k] == ' ' || p->src[k] == '\t'))
				k++;
			if (k < p->len && p->src[k] == '(') {
				const char *name = p->tokword;
				consume(p);
				if (peek_tok(p) != T_LPAREN)
					unexpected(p);
				consume(p);
				if (peek_tok(p) != T_RPAREN)
					unexpected(p);
				consume(p);
				n = parse_function_body(p, name);
				p->depth--;
				return n;
			}
		}
		n = parse_simple(p);
	} else
		unexpected(p);
	p->depth--;
	return n;
}

struct node *parse_next(struct parser *p)
{
	jmp_buf jb, *saved = parse_jmp;
	parse_jmp = &jb;
	p->incomplete = false;
	if (setjmp(jb)) {
		parse_jmp = saved;
		p->alias_next = false;
		if (!p->incomplete)
			sh.status = 2;
		p->eof = true;
		sh_raise(2);
	}
	struct node *list = NULL;
	for (;;) {
		skip_newlines(p);
		if (peek_cmd(p) == T_EOF) {
			p->eof = true;
			break;
		}
		struct node *n = parse_and_or(p);
		int t = peek_tok(p);
		if (t == T_AMP || t == T_SEMI) {
			consume(p);
			if (t == T_AMP) {
				struct node *bg = new_node(p, N_BG);
				bg->body = n;
				n = bg;
			}
		} else if (t != T_NEWLINE && t != T_EOF)
			unexpected(p);
		if (!list)
			list = n;
		else {
			struct node *seq = new_node(p, N_SEQ);
			seq->bin.left = list;
			seq->bin.right = n;
			list = seq;
		}
		/* a complete command ends at a newline */
		t = peek_tok(p);
		if (t == T_NEWLINE) {
			consume(p);
			break;
		}
		if (t == T_EOF)
			break;
	}
	if (p->pending_docs) {
		/* a here-doc on the last line without a newline after it */
		parse_incomplete(p);
	}
	parse_jmp = saved;
	return list;
}

/* Does the text form whole commands? (no open quote, if without fi, ...) */
bool parse_is_complete(const char *src)
{
	struct parser p;
	parser_init(&p, src, strlen(src), 1);
	struct jmp j;
	j.prev = jmp_top;
	jmp_top = &j;
	bool complete = true;
	int saved = sh.status;
	bool quiet = parse_quiet;
	parse_quiet = true;
	if (setjmp(j.buf) == 0) {
		while (parse_next(&p))
			;
	} else
		complete = !p.incomplete;
	parse_quiet = quiet;
	jmp_top = j.prev;
	sh.status = saved;
	arena_unref(p.arena);
	return complete;
}
