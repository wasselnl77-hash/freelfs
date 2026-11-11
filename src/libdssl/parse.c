/* libdssl: source text to syntax tree (spec chapter 2, level 1). SPDX-License-Identifier: MIT
 *
 * A port of the reference parser (parser.js 0.16.3), keeping its quirks where the spec leaves
 * room (see SPEC-NOTES.md). Positions: every node and error carries the spec's line and a byte
 * column; the clean (comment-free) text keeps a map back to the source offsets for that. */
#include "internal.h"
#include <strings.h>

#define MAXD 400   /* block nesting limit (the JS parser has none); deeper blocks are skipped with an error */

typedef struct { size_t chunk_off; const char *cptr; int line; } Seg;
VEC(SegVec, Seg);

typedef struct {
	int depth;
	char q, prev, last;
} NM;

typedef struct {
	Arena *A;
	sp file;
	DiagVec *errs;
	const char *clean_base;
	size_t clean_len;
	const uint32_t *map;
	const uint32_t *line_starts;
	size_t nlines;
	Node *stack[MAXD + 2];
	int top;               /* index of the top of the stack; 0 is the root */
	int tokens_depth;
	/* multi-line raw { } capture */
	int raw_active, raw_depth;
	Node *raw_node;
	SB raw_sb;
	/* the chunk being processed */
	const char *chunk_base;
	const Seg *segs;
	int nsegs;
	int too_deep;
} P;

/* ---------------------------------------------------------------- positions and errors */

static int col_of(P *p, const char *ptr)
{
	if (!ptr || !p->segs || ptr < p->chunk_base) return 0;
	size_t off = (size_t)(ptr - p->chunk_base);
	const Seg *s = &p->segs[0];
	for (int i = 1; i < p->nsegs; i++) if (p->segs[i].chunk_off <= off) s = &p->segs[i]; else break;
	const char *cp = s->cptr + (off - s->chunk_off);
	if (cp < p->clean_base || (size_t)(cp - p->clean_base) > p->clean_len) return 1;
	size_t idx = (size_t)(cp - p->clean_base);
	if (s->line < 1 || (size_t)s->line > p->nlines) return 1;
	uint32_t so = p->map[idx];
	return (int)(so - p->line_starts[s->line - 1]) + 1;
}

static void perr(P *p, int line, const char *at, sp msg, const char *hint)
{
	Diag d;
	d.line = line;
	d.col = col_of(p, at);
	d.msg = msg;
	d.has_hint = hint != NULL;
	d.hint = hint ? mk(hint, strlen(hint)) : mk(NULL, 0);
	vpush(p->A, p->errs, d);
}
#define PERR(p, line, at, hint, ...) perr((p), (line), (at), a_fmt((p)->A, __VA_ARGS__), (hint))

static Node *new_node(P *p, NodeKind k, int line, const char *at)
{
	Node *n = a_zalloc(p->A, sizeof *n);
	n->kind = k;
	n->line = line;
	n->col = col_of(p, at);
	return n;
}

static void add_child(Node *parent, Node *n)
{
	if (parent->last) parent->last->next = n; else parent->first = n;
	parent->last = n;
}

/* ---------------------------------------------------------------- scanners (parser.js) */

/* the first top-level `{` or `}`: not inside quotes, parentheses or brackets */
static long scan_brace(sp s, int *which)
{
	int depth = 0;
	char q = 0;
	for (size_t i = 0; i < s.n; i++) {
		char c = s.p[i];
		if (q) { if (c == q && s.p[i - 1] != '\\') q = 0; continue; }
		if (c == '"' || c == '\'') { q = c; continue; }
		if (c == '(' || c == '[') { depth++; continue; }
		if (c == ')' || c == ']') { if (depth > 0) depth--; continue; }
		if (depth == 0 && (c == '{' || c == '}')) { *which = c; return (long)i; }
	}
	return -1;
}

/* the first (last == 0) or last top-level occurrence of ch */
static long scan_for(sp s, char ch, int last)
{
	int depth = 0;
	char q = 0;
	long found = -1;
	for (size_t i = 0; i < s.n; i++) {
		char c = s.p[i];
		if (q) { if (c == q && s.p[i - 1] != '\\') q = 0; continue; }
		if (c == '"' || c == '\'') { q = c; continue; }
		if (c == '(' || c == '[') { depth++; continue; }
		if (c == ')' || c == ']') { if (depth > 0) depth--; continue; }
		if (depth == 0 && c == ch) { if (!last) return (long)i; found = (long)i; }
	}
	return found;
}

static void split_top_semis(Arena *a, sp s, SpVec *out)
{
	int depth = 0;
	char q = 0;
	size_t st = 0;
	for (size_t i = 0; i < s.n; i++) {
		char c = s.p[i];
		if (q) { if (c == q && s.p[i - 1] != '\\') q = 0; continue; }
		if (c == '"' || c == '\'') { q = c; continue; }
		if (c == '(' || c == '[') { depth++; continue; }
		if (c == ')' || c == ']') { if (depth > 0) depth--; continue; }
		if (depth == 0 && c == ';') { vpush(a, out, mk(s.p + st, i - st)); st = i + 1; }
	}
	vpush(a, out, mk(s.p + st, s.n - st));
}

/* the `}` matching the `{` just before `from`, or -1 */
static long match_brace(sp s, size_t from)
{
	int depth = 1;
	char q = 0;
	for (size_t i = from; i < s.n; i++) {
		char c = s.p[i];
		if (q) { if (c == q && s.p[i - 1] != '\\') q = 0; continue; }
		if (c == '"' || c == '\'') { q = c; continue; }
		if (c == '{') depth++;
		else if (c == '}') { depth--; if (depth == 0) return (long)i; }
	}
	return -1;
}

static void nm_feed(NM *m, const char *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		char c = p[i];
		if (m->q) { if (c == m->q && m->prev != '\\') m->q = 0; }
		else if (c == '"' || c == '\'') m->q = c;
		else if (c == '(' || c == '[') m->depth++;
		else if (c == ')' || c == ']') m->depth--;
		m->prev = c;
		if (!is_ws((unsigned char)c)) m->last = c;
	}
}
static int nm_more(const NM *m) { return m->depth != 0 || m->last == ',' || m->last == ':'; }

/* ---------------------------------------------------------------- comments (SYN-3 to SYN-6) */

static sp strip_comments(Arena *A, sp src, uint32_t **map_out)
{
	size_t n = src.n;
	char *out = a_alloc(A, n + 1);
	uint32_t *map = a_alloc(A, (n + 1) * sizeof(uint32_t));
	size_t o = 0;
	char q = 0;
	const char *s = src.p;
	for (size_t i = 0; i < n; i++) {
		char c = s[i], nx = i + 1 < n ? s[i + 1] : 0;
		if (q) {
			map[o] = (uint32_t)i; out[o++] = c;
			if (c == q && s[i - 1] != '\\') q = 0;
			continue;
		}
		if (c == '"' || c == '\'') { q = c; map[o] = (uint32_t)i; out[o++] = c; continue; }
		if ((c == 'u' || c == 'U') && i + 4 <= n && strncasecmp(s + i, "url(", 4) == 0
		    && !(i > 0 && (is_word((unsigned char)s[i - 1]) || s[i - 1] == '-'))) {
			size_t k = i + 4;
			while (k < n && is_ws((unsigned char)s[k])) k++;
			if (k >= n || (s[k] != '"' && s[k] != '\'')) {
				size_t e = k;
				while (e < n && s[e] != ')') e += s[e] == '\\' ? 2 : 1;
				if (e < n) {
					for (size_t j = i; j <= e; j++) { map[o] = (uint32_t)j; out[o++] = s[j]; }
					i = e;
					continue;
				}
			}
		}
		if (c == '/' && nx == '/') {
			while (i < n && s[i] != '\n') i++;
			if (i < n) i--;
			continue;
		}
		if (c == '/' && nx == '*') {
			size_t j = i + 2;
			while (j < n && !(s[j] == '*' && j + 1 < n && s[j + 1] == '/')) {
				map[o] = (uint32_t)j; out[o++] = s[j] == '\n' ? '\n' : ' ';
				j++;
			}
			i = j + 1;
			continue;
		}
		map[o] = (uint32_t)i; out[o++] = c;
	}
	map[o] = (uint32_t)n;
	out[o] = 0;
	*map_out = map;
	return mk(out, o);
}

/* ---------------------------------------------------------------- headers (2.5) */

static Node *classify(P *p, sp h, int line, const char *at)
{
	Node *n;
	if (sp_eqs(h, "tokens")) return new_node(p, K_TOKENS, line, at);
	if (sp_eqs(h, "pin")) return new_node(p, K_PIN, line, at);
	if (sp_eqs(h, "raw") || sp_eqs(h, "@css")) return new_node(p, K_RAW, line, at);
	if (sp_eqs(h, "fonts")) return new_node(p, K_FONTS, line, at);
	if (sp_starts(h, "behavior ")) { n = new_node(p, K_BEHAVIOR, line, at); n->a = sp_trim(sub(h, 9, h.n)); return n; }
	if (sp_starts(h, "motion ")) { n = new_node(p, K_MOTION, line, at); n->a = sp_trim(sub(h, 7, h.n)); return n; }
	if (sp_starts(h, "@layer ")) { n = new_node(p, K_LAYER, line, at); n->a = sp_trim(sub(h, 7, h.n)); return n; }
	if (sp_starts(h, "@media ")) { n = new_node(p, K_MEDIA, line, at); n->a = sp_trim(sub(h, 7, h.n)); return n; }
	if (sp_starts(h, "@container")) { n = new_node(p, K_CONTAINER, line, at); n->a = sp_trim(sub(h, 10, h.n)); return n; }
	if (sp_starts(h, "@signal ")) { n = new_node(p, K_SIGNAL, line, at); n->a = sp_trim(sub(h, 8, h.n)); return n; }
	if (h.p[0] == '@') {
		long sp1 = sp_findc(h, ' ');
		sp first = sp1 < 0 ? h : sub(h, 0, (size_t)sp1);
		PERR(p, line, at, "the whole block is skipped - for verbatim CSS like @font-face / @keyframes / @supports, wrap it in raw { }",
		     "unknown at-rule \"%.*s\"", PF(first));
		return new_node(p, K_BOGUS, line, at);
	}
	if (h.n >= 2 && h.p[0] == ':' && h.p[1] == ':') {
		sp rest = sub(h, 2, h.n);
		size_t e = 0;
		while (e < rest.n && !is_ws((unsigned char)rest.p[e]) && rest.p[e] != '(') e++;
		sp name = sub(rest, 0, e);
		int vendor = 0;
		if (name.n > 1 && name.p[0] == '-') {
			if (sp_starts(sub(name, 1, name.n), "webkit-") || sp_starts(sub(name, 1, name.n), "moz-") || sp_starts(sub(name, 1, name.n), "ms-")) vendor = 1;
		}
		if (!vset_has(&V_PSEUDO, name.p, name.n) && !vendor) {
			PERR(p, line, at, "try ::before, ::after, ::selection, or a vendor one like ::-webkit-scrollbar-thumb",
			     "unknown pseudo-element \"::%.*s\"", PF(name));
			return new_node(p, K_BOGUS, line, at);
		}
		n = new_node(p, K_PSEUDO, line, at);
		n->a = name;
		return n;
	}
	if (vset_has(&V_STATE, h.p, h.n)) { n = new_node(p, K_STATE, line, at); n->a = h; return n; }
	n = new_node(p, K_SELECTOR, line, at);
	n->a = h;
	return n;
}

/* ---------------------------------------------------------------- declarations (2.6, 2.8) */

static int name_ok_start(int c) { return is_alpha(c) || is_digit(c); }

/* does the text from i look like `   name:` (not `::`)? */
static int looks_like_pair(sp t, size_t i)
{
	while (i < t.n && is_ws((unsigned char)t.p[i])) i++;
	if (i >= t.n || !name_ok_start((unsigned char)t.p[i])) return 0;
	i++;
	while (i < t.n && (is_word((unsigned char)t.p[i]) || t.p[i] == '.' || t.p[i] == '-')) i++;
	while (i < t.n && is_ws((unsigned char)t.p[i])) i++;
	if (i >= t.n || t.p[i] != ':') return 0;
	return !(i + 1 < t.n && t.p[i + 1] == ':');
}

static void split_token_pairs(Arena *a, sp text, SpVec *out)
{
	int depth = 0, seen_colon = 0;
	char q = 0;
	size_t start = 0;
	for (size_t i = 0; i < text.n; i++) {
		char c = text.p[i];
		if (q) { if (c == q && text.p[i - 1] != '\\') q = 0; continue; }
		if (c == '"' || c == '\'') { q = c; continue; }
		if (c == '(' || c == '[') { depth++; continue; }
		if (c == ')' || c == ']') { if (depth > 0) depth--; continue; }
		if (depth) continue;
		if (c == ':' && !(i + 1 < text.n && text.p[i + 1] == ':')) { seen_colon = 1; continue; }
		if (seen_colon && is_ws((unsigned char)c) && looks_like_pair(text, i)) {
			vpush(a, out, mk(text.p + start, i - start));
			start = i + 1;
			seen_colon = 0;
		}
	}
	vpush(a, out, mk(text.p + start, text.n - start));
}

static int is_prop_name(sp s)
{
	/* ^(-{1,2}[a-z][a-z0-9.-]*|[a-z][a-z0-9-]*)$ (the text is lower-cased already) */
	size_t i = 0;
	int dashes = 0;
	while (i < s.n && s.p[i] == '-' && dashes < 2) { i++; dashes++; }
	if (i >= s.n || !(s.p[i] >= 'a' && s.p[i] <= 'z')) return 0;
	i++;
	for (; i < s.n; i++) {
		int c = (unsigned char)s.p[i];
		int ok = (c >= 'a' && c <= 'z') || is_digit(c) || c == '-' || (dashes && c == '.');
		if (!ok) return 0;
	}
	return 1;
}

static int is_num_prop(sp s)
{
	/* ^[0-9]+(\.[0-9]+)?$ */
	return is_number_str(s, 0);
}

static void add_decl(P *p, sp prop, sp value, int line, const char *at)
{
	Node *n = new_node(p, K_DECL, line, at);
	n->a = prop;
	n->value = value;
	add_child(p->stack[p->top], n);
}

static void process_content(P *p, sp content, int line)
{
	Arena *A = p->A;
	SpVec pieces = { 0 };
	SpVec semis = { 0 };
	split_top_semis(A, content, &semis);
	if (p->tokens_depth) {
		for (int i = 0; i < semis.n; i++) split_token_pairs(A, semis.v[i], &pieces);
	} else pieces = semis;
	for (int i = 0; i < pieces.n; i++) {
		sp part = sp_trim(pieces.v[i]);
		if (!part.n) continue;
		long colon = scan_for(part, ':', 0);
		if (colon < 0) {
			/* use <name> */
			if (part.n > 4 && memcmp(part.p, "use", 3) == 0 && is_ws((unsigned char)part.p[3])) {
				sp nm = sp_ltrim(sub(part, 3, part.n));
				int ok = nm.n && is_alpha((unsigned char)nm.p[0]);
				for (size_t k = 1; ok && k < nm.n; k++) if (!(is_word((unsigned char)nm.p[k]) || nm.p[k] == '-')) ok = 0;
				if (ok) { add_decl(p, LIT("use"), nm, line, part.p); continue; }
			}
			/* a bare word: ^[a-z][a-z0-9-]*$ */
			int ok = is_alpha((unsigned char)part.p[0]);
			for (size_t k = 1; ok && k < part.n; k++) { int c = (unsigned char)part.p[k]; if (!(is_alpha(c) || is_digit(c) || c == '-')) ok = 0; }
			if (ok) { add_decl(p, sp_lower(A, part), mk("", 0), line, part.p); continue; }
			sp pre = utf16_prefix(part, 40);
			PERR(p, line, part.p, "declarations look like  prop: value", "expected \":\" in \"%.*s\"", PF(pre));
			continue;
		}
		sp prop = sp_lower(A, sp_trim(sub(part, 0, (size_t)colon)));
		sp value = sp_trim(sub(part, (size_t)colon + 1, part.n));
		if (!prop.n) { PERR(p, line, part.p, NULL, "declaration is missing a property name"); continue; }
		if (is_num_prop(prop)) {
			if (p->tokens_depth) { add_decl(p, prop, value, line, part.p); continue; }
			PERR(p, line, part.p,
			     "numeric names are token keys only inside tokens { } - e.g. tokens { space { 9: 96 } } - and then referenced as space.9; property names must start with a letter",
			     "\"%.*s\" is not a valid property name", PF(prop));
			continue;
		}
		if (!is_prop_name(prop)) {
			PERR(p, line, part.p, "property names are lower-case words with dashes (my-prop), or dotted tokens (color.accent)",
			     "\"%.*s\" is not a valid property name", PF(prop));
			continue;
		}
		add_decl(p, prop, value, line, part.p);
	}
}

/* ---------------------------------------------------------------- raw capture (2.7) */

static void process_chunk(P *p, sp chunk, int line);

static void feed_raw(P *p, sp raw, int line)
{
	char q = 0;
	for (size_t i = 0; i < raw.n; i++) {
		char c = raw.p[i];
		if (q) { if (c == q && raw.p[i - 1] != '\\') q = 0; continue; }
		if (c == '"' || c == '\'') { q = c; continue; }
		if (c == '{') p->raw_depth++;
		else if (c == '}') {
			p->raw_depth--;
			if (p->raw_depth == 0) {
				sb_put(&p->raw_sb, raw.p, i);
				p->raw_node->css = sb_done(&p->raw_sb);
				p->raw_node->has_css = 1;
				sp rest = sub(raw, i + 1, raw.n);
				p->raw_active = 0;
				p->raw_node = NULL;
				if (sp_trim(rest).n) {
					Seg seg = { 0, rest.p, line };
					const char *ob = p->chunk_base;
					const Seg *os = p->segs;
					int on = p->nsegs;
					p->chunk_base = rest.p; p->segs = &seg; p->nsegs = 1;
					process_chunk(p, rest, line);
					p->chunk_base = ob; p->segs = os; p->nsegs = on;
				}
				return;
			}
		}
	}
	sb_put(&p->raw_sb, raw.p, raw.n);
	sb_putc(&p->raw_sb, '\n');
}

/* what happens right after a block header opens; returns 1 and *after when the rest of the chunk
 * is already decided, 0 when the caller should push the node and go on after the "{" */
static int open_block(P *p, Node *node, sp rest, size_t open, int line, sp *after)
{
	if (node->kind == K_RAW || node->kind == K_FONTS) {
		const char *nm = node->kind == K_RAW ? "raw" : "fonts";
		if (p->top > 0) {
			PERR(p, line, rest.p,
			     "move it out of the enclosing block",
			     "%s { } must be top-level%s", nm,
			     node->kind == K_RAW ? " - it drops verbatim CSS into the page stylesheet" : " - fonts load for the whole page");
			long close = match_brace(rest, open + 1);
			*after = close < 0 ? mk("", 0) : sub(rest, (size_t)close + 1, rest.n);
			return 1;
		}
		long close = match_brace(rest, open + 1);
		if (close >= 0) {
			node->css = sub(rest, open + 1, (size_t)close);
			node->has_css = 1;
			*after = sub(rest, (size_t)close + 1, rest.n);
			return 1;
		}
		node->css = mk("", 0);
		node->has_css = 1;
		p->raw_active = 1;
		p->raw_depth = 1;
		p->raw_node = node;
		sb_init(&p->raw_sb, p->A);
		feed_raw(p, sub(rest, open + 1, rest.n), line);
		*after = mk("", 0);
		return 1;
	}
	if (node->kind == K_BOGUS) {
		long close = match_brace(rest, open + 1);
		if (close < 0) {
			PERR(p, line, rest.p, "every \"{\" needs a matching \"}\"", "the \"block\" block was never closed with \"}\"");
			*after = mk("", 0);
			return 1;
		}
		*after = sub(rest, (size_t)close + 1, rest.n);
		return 1;
	}
	return 0;
}

static void process_chunk(P *p, sp chunk, int line)
{
	sp rest = chunk;
	while (rest.n) {
		rest = sp_trim(rest);
		if (!rest.n) return;
		int which = 0;
		long pos = scan_brace(rest, &which);
		if (pos >= 0 && which == '}') {
			sp before = sp_trim(sub(rest, 0, (size_t)pos));
			if (before.n) process_content(p, before, line);
			if (p->top == 0) PERR(p, line, rest.p + pos, NULL, "unexpected \"}\"");
			else {
				Node *n = p->stack[p->top--];
				if (n->kind == K_TOKENS) p->tokens_depth--;
			}
			rest = sub(rest, (size_t)pos + 1, rest.n);
			continue;
		}
		if (pos >= 0) {
			size_t open = (size_t)pos;
			sp header = sp_trim(sub(rest, 0, open));
			if (header.n) {
				long semi = scan_for(header, ';', 1);
				if (semi >= 0) {
					process_content(p, sub(header, 0, (size_t)semi), line);
					header = sp_trim(sub(header, (size_t)semi + 1, header.n));
				}
			}
			if (!header.n) {
				PERR(p, line, rest.p + open, NULL, "block is missing a header before \"{\"");
				rest = sub(rest, open + 1, rest.n);
				continue;
			}
			Node *node = classify(p, header, line, header.p);
			add_child(p->stack[p->top], node);
			sp after;
			if (open_block(p, node, rest, open, line, &after)) { rest = after; continue; }
			rest = sub(rest, open + 1, rest.n);
			if (p->top >= MAXD) {
				if (!p->too_deep) PERR(p, line, header.p, "the block and everything in it is skipped", "blocks are nested more than %d deep", MAXD);
				p->too_deep = 1;
				node->kind = K_BOGUS;
				/* skip to the matching brace: depth 1 starts after the "{" */
				long c2 = match_brace(rest, 0);
				rest = c2 < 0 ? mk("", 0) : sub(rest, (size_t)c2 + 1, rest.n);
				continue;
			}
			p->stack[++p->top] = node;
			if (node->kind == K_TOKENS) p->tokens_depth++;
			continue;
		}
		process_content(p, rest, line);
		return;
	}
}

/* ---------------------------------------------------------------- the line loop */

Node *parse_source(Arena *A, sp file, sp src, DiagVec *errs, int *depth_exceeded)
{
	P *p = a_zalloc(A, sizeof *p);
	p->A = A;
	p->file = file;
	p->errs = errs;
	Node *root = a_zalloc(A, sizeof *root);
	root->kind = K_ROOT;
	p->stack[0] = root;

	/* source line starts, for columns */
	size_t nl = 1;
	for (size_t i = 0; i < src.n; i++) if (src.p[i] == '\n') nl++;
	uint32_t *ls = a_alloc(A, nl * sizeof(uint32_t));
	{
		size_t k = 0;
		ls[k++] = 0;
		for (size_t i = 0; i < src.n; i++) if (src.p[i] == '\n') ls[k++] = (uint32_t)(i + 1);
	}
	p->line_starts = ls;
	p->nlines = nl;

	uint32_t *map;
	sp clean = strip_comments(A, src, &map);
	p->clean_base = clean.p;
	p->clean_len = clean.n;
	p->map = map;

	SB buf;
	sb_init(&buf, A);
	SegVec segs = { 0 };
	NM nm = { 0, 0, 0, 0 };
	int have_buf = 0, buf_line = 0;
	int total_lines = 0;

	size_t pos = 0;
	int line = 0;
	for (;;) {
		/* one physical line [pos, e) */
		size_t e = pos;
		while (e < clean.n && clean.p[e] != '\n') e++;
		line++;
		total_lines = line;
		sp raw = mk(clean.p + pos, e - pos);
		sp trimmed = sp_trim(raw);
		if (!trimmed.n) {
			if (p->raw_active) { sb_put(&p->raw_sb, raw.p, raw.n); sb_putc(&p->raw_sb, '\n'); }
		} else if (p->raw_active) {
			feed_raw(p, raw, line);
		} else if (have_buf) {
			/* candidate = buf + ' ' + trimmed */
			size_t off = buf.n + 1;
			sb_putc(&buf, ' ');
			sb_put(&buf, trimmed.p, trimmed.n);
			Seg sg = { off, trimmed.p, line };
			vpush(A, &segs, sg);
			nm_feed(&nm, " ", 1);
			nm_feed(&nm, trimmed.p, trimmed.n);
			if (!nm_more(&nm)) {
				sp chunk = sb_done(&buf);
				p->chunk_base = chunk.p; p->segs = segs.v; p->nsegs = segs.n;
				process_chunk(p, chunk, buf_line ? buf_line : line);
				have_buf = 0;
				buf.p = NULL; buf.n = buf.cap = 0;
				segs.n = 0;
			}
		} else {
			NM m2 = { 0, 0, 0, 0 };
			nm_feed(&m2, raw.p, raw.n);
			if (nm_more(&m2)) {
				sb_init(&buf, A);
				sb_put(&buf, raw.p, raw.n);
				segs.n = 0;
				Seg sg = { 0, raw.p, line };
				vpush(A, &segs, sg);
				nm = m2;
				have_buf = 1;
				buf_line = line;
			} else {
				Seg sg = { 0, raw.p, line };
				p->chunk_base = raw.p; p->segs = &sg; p->nsegs = 1;
				process_chunk(p, raw, line);
			}
		}
		if (e >= clean.n) break;
		pos = e + 1;
	}
	if (have_buf && sp_trim(sb_done(&buf)).n) {
		sp chunk = sb_done(&buf);
		p->chunk_base = chunk.p; p->segs = segs.v; p->nsegs = segs.n;
		process_chunk(p, chunk, buf_line);
	}
	p->segs = NULL; p->nsegs = 0;
	if (p->raw_active) {
		/* the engine says "discarded" but keeps what it captured (SPEC-NOTES) */
		p->raw_node->css = sb_done(&p->raw_sb);
		p->raw_node->has_css = 1;
		perr(p, total_lines, NULL, a_dups(A, "the raw { } block was never closed with \"}\""), "every \"{\" needs a matching \"}\" - the CSS captured so far is discarded");
	}
	if (p->top > 0) {
		perr(p, total_lines, NULL, a_fmt(A, "%d block(s) were never closed with \"}\"", p->top), "every \"{\" needs a matching \"}\"");
	}
	if (depth_exceeded) *depth_exceeded = p->too_deep;
	return root;
}
