/* libdssl: the resolver (spec chapters 4, 5, 7, 9, 10) and the public sheet/context API.
 * Port of resolve() of resolver.js 0.16.3, without the lint pass. SPDX-License-Identifier: MIT */
#include "internal.h"
#include <math.h>
#include <stdlib.h>

const char *const LAYER_NAMES[5] = { "tokens", "base", "components", "utilities", "pin" };

#define MAX_STEPS 6000000L

typedef struct { sp name; Node *node; int active; } Beh;
VEC(BehVec, Beh);
typedef struct { sp name; Node *node; Motion *parsed; } MotDef;
VEC(MotDefVec, MotDef);

typedef struct WCtx {
	sp sel;
	SpList media, containers;
	SigList signals;
	int layer;
} WCtx;

typedef struct { DeclVec decls; } Pend;

typedef struct W {
	R r;
	struct dssl_sheet *S;
	struct dssl_ctx *cx;
	BehVec behs;
	MotDefVec mots;
	int rule_id, order;
	int aborted;
	long steps;
	DiagVec *errors, *warnings;
	RuleVec *rules, *shadow;
} W;

#define A_(w) ((w)->r.A)

/* ---------------------------------------------------------------- diagnostics helpers */

static void werr(W *w, int line, int col, sp msg, const char *hint)
{
	diag_push(A_(w), w->errors, line, col, msg, hint, mk(NULL, 0), 0);
}
static void werr_h(W *w, int line, int col, sp msg, sp hint, int has)
{
	diag_push(A_(w), w->errors, line, col, msg, NULL, hint, has);
}
static void wwarn(W *w, int line, int col, sp msg, const char *hint)
{
	diag_push(A_(w), w->warnings, line, col, msg, hint, mk(NULL, 0), 0);
}
#define F(w, ...) a_fmt(A_(w), __VA_ARGS__)

/* ---------------------------------------------------------------- selectors */

/* splitTopLevel: top-level commas (not inside ( ) or [ ]), parts trimmed, an empty last part dropped */
static void split_top_level(Arena *a, sp sel, SpVec *out)
{
	int depth = 0;
	size_t st = 0;
	for (size_t i = 0; i < sel.n; i++) {
		char ch = sel.p[i];
		if (ch == '(' || ch == '[') depth++;
		if (ch == ')' || ch == ']') depth--;
		if (ch == ',' && depth == 0) { vpush(a, out, sp_trim(mk(sel.p + st, i - st))); st = i + 1; }
	}
	sp last = sp_trim(mk(sel.p + st, sel.n - st));
	if (last.n) vpush(a, out, last);
}

static int has_amp(sp s) { return sp_findc(s, '&') >= 0; }

static void put_amp_join(SB *b, sp child, sp parent)
{
	/* child.split('&').join(parent) */
	for (size_t i = 0; i < child.n; i++) {
		if (child.p[i] == '&') sb_putsp(b, parent); else sb_putc(b, child.p[i]);
	}
}

static sp combine(Arena *a, sp parent, sp child)
{
	SpVec pp = { 0 }, cp = { 0 };
	split_top_level(a, parent, &pp);
	split_top_level(a, child, &cp);
	SB b;
	sb_init(&b, a);
	if (pp.n <= 1 && cp.n <= 1) {
		if (has_amp(child)) put_amp_join(&b, child, parent);
		else { sb_putsp(&b, parent); sb_putc(&b, ' '); sb_putsp(&b, child); }
		return sb_done(&b);
	}
	int first = 1;
	for (int i = 0; i < pp.n; i++) {
		for (int j = 0; j < cp.n; j++) {
			if (!first) sb_puts(&b, ", ");
			first = 0;
			if (has_amp(cp.v[j])) put_amp_join(&b, cp.v[j], pp.v[i]);
			else { sb_putsp(&b, pp.v[i]); sb_putc(&b, ' '); sb_putsp(&b, cp.v[j]); }
		}
	}
	return sb_done(&b);
}

static sp with_state(Arena *a, sp sel, sp name)
{
	SpVec parts = { 0 };
	split_top_level(a, sel, &parts);
	SB b;
	sb_init(&b, a);
	if (parts.n <= 1) { sb_putsp(&b, sel); sb_putc(&b, ':'); sb_putsp(&b, name); return sb_done(&b); }
	for (int i = 0; i < parts.n; i++) {
		if (i) sb_puts(&b, ", ");
		sb_putsp(&b, parts.v[i]); sb_putc(&b, ':'); sb_putsp(&b, name);
	}
	return sb_done(&b);
}

static const char *const SCAN_STATES[] = {
	"hover", "focus-visible", "focus", "active", "disabled", "checked", "empty", "target", "invalid", "valid", "required", "open",
	"closed", "read-only", "read-write", "placeholder-shown", "first-child", "last-child", "only-child", "seen",
};
#define N_SCAN_STATES 20

const char *const *state_scan_names(int *n) { *n = N_SCAN_STATES; return SCAN_STATES; }

unsigned scan_states(sp sel)
{
	unsigned m = 0;
	for (size_t i = 0; i < sel.n; i++) {
		if (sel.p[i] != ':') continue;
		for (int k = 0; k < N_SCAN_STATES; k++) {
			size_t L = strlen(SCAN_STATES[k]);
			if (i + 1 + L > sel.n || memcmp(sel.p + i + 1, SCAN_STATES[k], L) != 0) continue;
			size_t e = i + 1 + L;
			if (e < sel.n && is_word((unsigned char)sel.p[e])) continue;   /* \b */
			m |= 1u << k;
			break;
		}
	}
	return m;
}

/* analyzePseudo */
typedef struct { sp el, pseudo; int ok; } PSplit;
VEC(PSVec, PSplit);

static int analyze_pseudo(Arena *a, sp sel, PSVec *splits, int *mode_out)
{
	/* top-level comma split, string- and paren-aware, no trimming */
	SpVec parts = { 0 };
	int depth = 0;
	char q = 0;
	size_t st = 0;
	for (size_t i = 0; i < sel.n; i++) {
		char ch = sel.p[i];
		if (q) { if (ch == q) q = 0; continue; }
		if (ch == '"' || ch == '\'') { q = ch; continue; }
		if (ch == '(' || ch == '[') depth++;
		else if (ch == ')' || ch == ']') { if (depth > 0) depth--; }
		if (ch == ',' && depth == 0) { vpush(a, &parts, mk(sel.p + st, i - st)); st = i + 1; }
	}
	vpush(a, &parts, mk(sel.p + st, sel.n - st));
	int with = 0;
	int any_bad = 0;
	for (int pi = 0; pi < parts.n; pi++) {
		sp part = parts.v[pi];
		long idx = -1;
		depth = 0; q = 0;
		for (size_t i = 0; i + 1 < part.n; i++) {
			char ch = part.p[i];
			if (q) { if (ch == q && part.p[i - 1] != '\\') q = 0; continue; }
			if (ch == '"' || ch == '\'') { q = ch; continue; }
			if (ch == '(' || ch == '[') depth++;
			else if (ch == ')' || ch == ']') { if (depth > 0) depth--; }
			else if (depth == 0 && ch == ':' && part.p[i + 1] == ':') { idx = (long)i; break; }
		}
		PSplit ps;
		memset(&ps, 0, sizeof ps);
		if (idx >= 0) {
			with++;
			sp el = sp_trim(sub(part, 0, (size_t)idx)), pseudo = sp_trim(sub(part, (size_t)idx + 2, part.n));
			if (el.n && pseudo.n) { ps.el = el; ps.pseudo = pseudo; ps.ok = 1; }
		}
		vpush(a, splits, ps);
		if (idx >= 0 && !ps.ok) any_bad = 1;
	}
	if (with == 0) { *mode_out = 0; return 0; }
	int all_ok = 1;
	for (int i = 0; i < splits->n; i++) if (!splits->v[i].ok) all_ok = 0;
	(void)any_bad;
	*mode_out = (with != parts.n || !all_ok) ? 2 : 1;
	return with;
}

/* the pseudo name: pseudo.split(/[\s:(]/)[0] */
static sp pseudo_head(sp p)
{
	size_t i = 0;
	while (i < p.n && !is_ws((unsigned char)p.p[i]) && p.p[i] != ':' && p.p[i] != '(') i++;
	return sub(p, 0, i);
}

static int vendor_pseudo(sp name)
{
	return name.n > 1 && name.p[0] == '-' && (sp_starts(sub(name, 1, name.n), "webkit-") || sp_starts(sub(name, 1, name.n), "moz-") || sp_starts(sub(name, 1, name.n), "ms-"));
}

static int has_seen_state(sp el)
{
	for (size_t i = 0; i + 5 <= el.n; i++)
		if (el.p[i] == ':' && memcmp(el.p + i + 1, "seen", 4) == 0 && (i + 5 == el.n || !is_word((unsigned char)el.p[i + 5]))) return 1;
	return 0;
}

/* ---------------------------------------------------------------- media and signals */

static int unit_ahead(sp s, size_t i)
{
	while (i < s.n && is_ws((unsigned char)s.p[i])) i++;
	static const char *const u[] = { "px", "%", "em", "rem", "vh", "vw", "vmin", "vmax", "s", "ms", "deg", "dppx", "x" };
	sp r = sub(s, i, s.n);
	for (int k = 0; k < 13; k++) if (sp_starts(r, u[k])) return 1;
	return 0;
}

static sp px_numbers(Arena *a, sp q)
{
	SB out;
	sb_init(&out, a);
	size_t i = 0, n = q.n;
	while (i < n) {
		char c = q.p[i];
		if (c == '>' || c == '=' || c == '<') {
			size_t j = i + 1;
			while (j < n && is_ws((unsigned char)q.p[j])) j++;
			size_t d0 = j;
			while (j < n && is_digit((unsigned char)q.p[j])) j++;
			if (j > d0) {
				size_t cands[2];
				int nc = 0;
				if (j + 1 < n && q.p[j] == '.' && is_digit((unsigned char)q.p[j + 1])) {
					size_t k = j + 1;
					while (k < n && is_digit((unsigned char)q.p[k])) k++;
					cands[nc++] = k;
				}
				cands[nc++] = j;
				int done = 0;
				for (int ci = 0; ci < nc; ci++) {
					size_t e = cands[ci];
					if (e < n && is_word((unsigned char)q.p[e])) continue;      /* \b */
					if (unit_ahead(q, e)) continue;
					sb_put(&out, q.p + i, e - i);
					sb_puts(&out, "px");
					i = e;
					done = 1;
					break;
				}
				if (done) continue;
			}
		}
		sb_putc(&out, c);
		i++;
	}
	return sb_done(&out);
}

static size_t dnum(sp s, size_t i)
{
	size_t j = i;
	while (j < s.n && is_digit((unsigned char)s.p[j])) j++;
	if (j == i) return i;
	if (j + 1 < s.n && s.p[j] == '.' && is_digit((unsigned char)s.p[j + 1])) {
		j++;
		while (j < s.n && is_digit((unsigned char)s.p[j])) j++;
	}
	return j;
}

static sp normalize_media_w(W *w, sp query, int line, int col)
{
	Arena *A = A_(w);
	sp q = sp_trim(query);
	if (!q.n) {
		werr(w, line, col, F(w, "@media needs a condition"), "e.g. @media 768  or  @media dark  or  @media (width >= 768)");
		return LIT("(min-width: 0)");
	}
	if (sp_ieq(q, "dark")) return LIT("(prefers-color-scheme: dark)");
	if (sp_ieq(q, "light")) return LIT("(prefers-color-scheme: light)");
	if (sp_ieq(q, "reduced-motion")) return LIT("(prefers-reduced-motion: reduce)");
	/* ^(<=|>=)?\s*(\d+(?:\.\d+)?)\s*(?:\.\.\s*(\d+(?:\.\d+)?))?$ */
	{
		size_t i = 0;
		int op = 0;
		if (q.n >= 2 && q.p[0] == '<' && q.p[1] == '=') { op = 1; i = 2; }
		else if (q.n >= 2 && q.p[0] == '>' && q.p[1] == '=') { op = 2; i = 2; }
		while (i < q.n && is_ws((unsigned char)q.p[i])) i++;
		size_t e1 = dnum(q, i);
		if (e1 > i) {
			sp n1 = sub(q, i, e1);
			size_t k = e1;
			while (k < q.n && is_ws((unsigned char)q.p[k])) k++;
			if (k == q.n) {
				if (op == 1) return a_fmt(A, "(max-width: %.*spx)", PF(n1));
				return a_fmt(A, "(min-width: %.*spx)", PF(n1));
			}
			if (k + 1 < q.n && q.p[k] == '.' && q.p[k + 1] == '.') {
				size_t m = k + 2;
				while (m < q.n && is_ws((unsigned char)q.p[m])) m++;
				size_t e2 = dnum(q, m);
				if (e2 > m && e2 == q.n)
					return a_fmt(A, "(min-width: %.*spx) and (max-width: %.*spx)", PF(n1), PF(sub(q, m, e2)));
			}
		}
	}
	return px_numbers(A, q);
}

static int ident_start(int c) { return is_alpha(c) || c == '_'; }
static int ident_cont(int c) { return is_word(c) || c == '-'; }

/* ^([A-Za-z_][\w-]*) at the start; returns the length */
static size_t sig_name(sp s)
{
	if (!s.n || !ident_start((unsigned char)s.p[0])) return 0;
	size_t i = 1;
	while (i < s.n && ident_cont((unsigned char)s.p[i])) i++;
	return i;
}

static sp unquote_pair(sp s)
{
	/* .replace(/^(["'])(.*)\1$/, '$2') */
	if (s.n >= 2 && (s.p[0] == '"' || s.p[0] == '\'') && s.p[s.n - 1] == s.p[0]) {
		int ok = 1;
		for (size_t i = 1; i + 1 < s.n; i++) if (s.p[i] == '\n') { ok = 0; break; }
		if (ok) return sub(s, 1, s.n - 1);
	}
	return s;
}

static Signal parse_signal(W *w, sp expr, int line, int col)
{
	Arena *A = A_(w);
	Signal sg;
	memset(&sg, 0, sizeof sg);
	size_t nl = sig_name(expr);
	/* name\s+(not\s+in|in)\s*\(([^)]*)\)\s*$ */
	if (nl) {
		size_t i = nl;
		size_t w0 = i;
		while (i < expr.n && is_ws((unsigned char)expr.p[i])) i++;
		if (i > w0) {
			int notin = 0;
			size_t j = i;
			int kw = 0;
			if (j + 3 <= expr.n && memcmp(expr.p + j, "not", 3) == 0 && j + 3 < expr.n && is_ws((unsigned char)expr.p[j + 3])) {
				size_t k = j + 3;
				while (k < expr.n && is_ws((unsigned char)expr.p[k])) k++;
				if (k + 2 <= expr.n && memcmp(expr.p + k, "in", 2) == 0) { notin = 1; j = k + 2; kw = 1; }
			}
			if (!kw && j + 2 <= expr.n && memcmp(expr.p + j, "in", 2) == 0) { j += 2; kw = 1; }
			if (kw) {
				while (j < expr.n && is_ws((unsigned char)expr.p[j])) j++;
				if (j < expr.n && expr.p[j] == '(') {
					size_t c0 = j + 1, c1 = c0;
					while (c1 < expr.n && expr.p[c1] != ')') c1++;
					if (c1 < expr.n) {
						size_t t = c1 + 1;
						while (t < expr.n && is_ws((unsigned char)expr.p[t])) t++;
						if (t == expr.n) {
							SpVec vals = { 0 }, raw = { 0 };
							split_char(A, sub(expr, c0, c1), ',', &raw);
							for (int k = 0; k < raw.n; k++) {
								sp x = unquote_pair(sp_trim(raw.v[k]));
								if (x.n) vpush(A, &vals, x);
							}
							sp name = sub(expr, 0, nl);
							if (!vals.n)
								werr(w, line, col, F(w, "@signal %.*s %s ( ) has no values", PF(name), notin ? "not in" : "in"), "@signal screen in (menu, shop)");
							SB b;
							sb_init(&b, A);
							for (int k = 0; k < vals.n; k++) { if (k) sb_putc(&b, ','); sb_putsp(&b, vals.v[k]); }
							sg.name = name;
							sg.op = notin ? OP_NOTIN : OP_IN;
							sg.value = sb_done(&b); sg.has_value = 1;
							sg.values = vals; sg.has_values = 1;
							return sg;
						}
					}
				}
			}
		}
		/* name\s*(==|!=)\s*(.+)$ */
		i = nl;
		while (i < expr.n && is_ws((unsigned char)expr.p[i])) i++;
		if (i + 2 <= expr.n && (memcmp(expr.p + i, "==", 2) == 0 || memcmp(expr.p + i, "!=", 2) == 0)) {
			int eq = expr.p[i] == '=';
			size_t j = i + 2;
			while (j < expr.n && is_ws((unsigned char)expr.p[j])) j++;
			/* (.+): at least one character; a newline cannot be part of it */
			sp rest = sub(expr, j, expr.n);
			int has_nl = sp_findc(rest, '\n') >= 0;
			if (rest.n && !has_nl) {
				/* greedy \s* then .+ : if only white space is left, the last of it is the value */
				sp v = sp_trim(rest);
				if (v.n >= 1 && ((v.p[0] == '"' && v.p[v.n - 1] == '"') || (v.p[0] == '\'' && v.p[v.n - 1] == '\''))) v = v.n >= 2 ? sub(v, 1, v.n - 1) : mk("", 0);
				sg.name = sub(expr, 0, nl);
				sg.op = eq ? OP_EQ : OP_NE;
				sg.value = v; sg.has_value = 1;
				return sg;
			}
		}
	}
	sp t = sp_trim(expr);
	{
		size_t l2 = sig_name(t);
		if (l2 && l2 == t.n) { sg.name = t; sg.op = OP_TRUTHY; return sg; }
	}
	werr(w, line, col, F(w, "bad @signal expression \"%.*s\"", PF(expr)), "try:  @signal theme == dark   or   @signal compact");
	sg.name = t;
	sg.op = OP_TRUTHY;
	return sg;
}

/* ---------------------------------------------------------------- tokens */

static void collect_tokens(W *w, Node *node, sp path, int has_path, SpVec *acc)
{
	Arena *A = A_(w);
	static const char *const kn[] = { "root", "tokens", "pin", "raw", "fonts", "behavior", "motion", "layer", "media", "container", "signal", "bogus", "pseudo", "state", "selector", "decl" };
	for (Node *c = node->first; c; c = c->next) {
		if (c->kind == K_DECL) {
			sp key = has_path ? a_fmt(A, "%.*s.%.*s", PF(path), PF(c->a)) : c->a;
			tm_set(A, &w->r.tokens, key, c->value, c->line, c->col, 1);
			if (acc) vpush(A, acc, key);
		} else if (c->kind == K_SELECTOR || c->kind == K_TOKENS) {
			/* the engine joins child.sel, which is undefined for a nested tokens block (SPEC-NOTES) */
			sp seg = c->kind == K_SELECTOR ? c->a : LIT("undefined");
			if (has_path) collect_tokens(w, c, a_fmt(A, "%.*s.%.*s", PF(path), PF(seg)), 1, acc);
			else if (c->kind == K_SELECTOR) collect_tokens(w, c, seg, 1, acc);
			else collect_tokens(w, c, mk("", 0), 0, acc);
		} else {
			werr(w, c->line, c->col, F(w, "unexpected \"%s\" inside tokens", kn[c->kind]), NULL);
		}
	}
}

static void add_feat(struct dssl_sheet *S, unsigned kind, const char *name, int line, int col)
{
	Feat f = { kind, name, line, col };
	vpush(&S->A, &S->feats, f);
	S->feat_mask |= kind;
}

static void prepass_node(W *w, Node *n)
{
	if (n->kind == K_TOKENS) collect_tokens(w, n, mk("", 0), 0, NULL);
	else if (n->kind == K_BEHAVIOR) {
		int found = -1;
		for (int i = 0; i < w->behs.n; i++) if (sp_eq(w->behs.v[i].name, n->a)) { found = i; break; }
		if (found >= 0) {
			werr(w, n->line, n->col, F(w, "behavior \"%.*s\" is defined twice", PF(n->a)), NULL);
			w->behs.v[found].node = n;
		} else {
			Beh b = { n->a, n, 0 };
			vpush(A_(w), &w->behs, b);
		}
	} else if (n->kind == K_MOTION) {
		int found = -1;
		for (int i = 0; i < w->mots.n; i++) if (sp_eq(w->mots.v[i].name, n->a)) { found = i; break; }
		if (found >= 0) w->mots.v[found].node = n;
		else { MotDef m = { n->a, n, NULL }; vpush(A_(w), &w->mots, m); }
	} else {
		for (Node *c = n->first; c; c = c->next) if (c->kind != K_DECL) prepass_node(w, c);
	}
}

static Rule *new_rule(W *w) { return a_zalloc(A_(w), sizeof(Rule)); }

static Rule *resolve_token_decls(W *w, const SpVec *keys, const WCtx *ctx)
{
	Arena *A = A_(w);
	DeclVec decls = { 0 };
	for (int i = 0; i < keys->n; i++) {
		sp key = keys->v[i];
		TokEnt *te = tm_find(&w->r.tokens, key);
		SB b;
		sb_init(&b, A);
		sb_puts(&b, "--dssl-");
		for (size_t k = 0; k < key.n; k++) sb_putc(&b, key.p[k] == '.' ? '-' : key.p[k]);
		VC vc;
		memset(&vc, 0, sizeof vc);
		vc.r = &w->r;
		vc.prop = key;
		vc.line = te ? te->line : 0;
		vc.col = te ? te->col : 0;
		w->r.scratch.n = 0;
		vc.errors = (te && te->own) ? w->errors : &w->r.scratch;
		vc.bare = key.n >= 4 && memcmp(key.p, "font", 4) == 0;
		Decl d;
		memset(&d, 0, sizeof d);
		d.prop = sb_done(&b);
		d.value = resolve_value(&vc, te ? te->value : mk("", 0));
		d.line = -1;
		d.kind = DV_STR;
		vpush(A, &decls, d);
	}
	if (!decls.n) return NULL;
	Rule *r = new_rule(w);
	r->id = w->rule_id++;
	r->sel = LIT(":root");
	r->media = ctx->media; r->signals = ctx->signals; r->containers = ctx->containers;
	r->layer = L_TOKENS;
	r->order = w->order++;
	r->decls = decls.v; r->ndecls = decls.n;
	r->line = 0;
	r->has_states = 1;
	r->is_token = 1;
	return r;
}

/* ---------------------------------------------------------------- motions */

static int motion_offset(sp label, double *off)
{
	sp t = sp_trim(label);
	if (sp_eqs(t, "from")) { *off = 0; return 1; }
	if (sp_eqs(t, "to")) { *off = 100; return 1; }
	sp u = t;
	if (sp_starts(u, "at") && u.n > 2 && is_ws((unsigned char)u.p[2])) u = sp_ltrim(sub(u, 2, u.n));
	size_t i = 0;
	while (i < u.n && (is_digit((unsigned char)u.p[i]) || u.p[i] == '.')) i++;
	if (i == 0) return 0;
	sp num = sub(u, 0, i);
	size_t k = i;
	while (k < u.n && is_ws((unsigned char)u.p[k])) k++;
	if (k < u.n && u.p[k] == '%') k++;
	if (k != u.n) return 0;
	*off = js_parse_float(num);
	return 1;
}

typedef struct {
	int has_bend, has_bendy, has_stretch, has_fold, has_bendup;
	double bend, bendy, stretch, fold, bendup;
	int vertical;
} Deform;

static double r4(double v) { return round(v * 1e4) / 1e4; }

static sp compose_deform(Arena *A, const Deform *d, sp base, const char **origin)
{
	SB b;
	sb_init(&b, A);
	int n = 0;
#define SEP() do { if (n++) sb_putc(&b, ' '); } while (0)
	*origin = NULL;
	if (d->bend || d->bendy || d->fold || d->bendup) { SEP(); sb_puts(&b, "perspective(900px)"); }
	if (d->bend) { SEP(); sb_puts(&b, "rotateX("); sb_num(&b, d->bend); sb_puts(&b, "deg)"); }
	if (d->bendy) { SEP(); sb_puts(&b, "rotateY("); sb_num(&b, d->bendy); sb_puts(&b, "deg)"); }
	if (d->fold) { SEP(); sb_puts(&b, "rotateX("); sb_num(&b, d->fold); sb_puts(&b, "deg)"); *origin = "50% 0%"; }
	if (d->bendup) { SEP(); sb_puts(&b, "rotateX("); sb_num(&b, d->bendup); sb_puts(&b, "deg)"); *origin = "50% 100%"; }
	if (d->stretch && d->stretch != 1) {
		double inv = r4(1 / d->stretch);
		SEP();
		if (d->vertical) { sb_puts(&b, "scaleY("); sb_num(&b, d->stretch); sb_puts(&b, ") scaleX("); sb_num(&b, inv); sb_putc(&b, ')'); }
		else { sb_puts(&b, "scaleX("); sb_num(&b, d->stretch); sb_puts(&b, ") scaleY("); sb_num(&b, inv); sb_putc(&b, ')'); }
	}
	sp bt = sp_trim(base);
	if (bt.n && !sp_eqs(bt, "none")) { SEP(); sb_putsp(&b, bt); }
	if (!n) return LIT("none");
	return sb_done(&b);
}

static void props_set(Arena *A, PropVec *pv, sp camel, sp css, sp value)
{
	for (int i = 0; i < pv->n; i++) if (sp_eq(pv->v[i].camel, camel)) { pv->v[i].value = value; pv->v[i].css = css; return; }
	Prop p = { camel, css, value };
	vpush(A, pv, p);
}

static sp camelize(Arena *A, sp s)
{
	SB b;
	sb_init(&b, A);
	for (size_t i = 0; i < s.n; i++) {
		if (s.p[i] == '-' && i + 1 < s.n && s.p[i + 1] >= 'a' && s.p[i + 1] <= 'z') { sb_putc(&b, (char)(s.p[i + 1] - 32)); i++; }
		else sb_putc(&b, s.p[i]);
	}
	return sb_done(&b);
}

static Motion *base_motion(W *w, sp name)
{
	for (int i = 0; i < w->cx->motions.n; i++) if (sp_eq(w->cx->motions.v[i]->name, name)) return w->cx->motions.v[i];
	return NULL;
}

static Motion *copy_motion(Arena *A, const Motion *m)
{
	Motion *c = a_zalloc(A, sizeof *c);
	c->name = a_dup(A, m->name);
	c->essential = m->essential;
	for (int i = 0; i < m->frames.n; i++) {
		Frame f;
		memset(&f, 0, sizeof f);
		f.offset = m->frames.v[i].offset;
		for (int k = 0; k < m->frames.v[i].props.n; k++) {
			const Prop *p = &m->frames.v[i].props.v[k];
			Prop q = { a_dup(A, p->camel), a_dup(A, p->css), a_dup(A, p->value) };
			vpush(A, &f.props, q);
		}
		vpush(A, &c->frames, f);
	}
	return c;
}

static Motion *parse_motion(W *w, sp name, int *from_base)
{
	Arena *A = A_(w);
	*from_base = 0;
	MotDef *def = NULL;
	for (int i = 0; i < w->mots.n; i++) if (sp_eq(w->mots.v[i].name, name)) { def = &w->mots.v[i]; break; }
	if (!def) { Motion *b = base_motion(w, name); if (b) *from_base = 1; return b; }
	if (def->parsed) return def->parsed;
	Motion *m = a_zalloc(A, sizeof *m);
	m->name = name;
	def->parsed = m;
	Node *node = def->node;
	for (Node *child = node->first; child; child = child->next) {
		if (child->kind == K_DECL && sp_eqs(child->a, "essential")) {
			m->essential = !child->value.n || sp_eqs(child->value, "true");
			continue;
		}
		if (child->kind == K_DECL) {
			werr(w, child->line, child->col, F(w, "\"%.*s\" is not allowed inside motion", PF(child->a)), "motions take from/to/at blocks and `essential`");
			continue;
		}
		if (child->kind != K_SELECTOR) continue;
		/* framesOf(label) */
		SpVec raw = { 0 };
		split_char(A, child->a, ',', &raw);
		double offs[64];
		double *ov = offs;
		int no = 0, bad = 0;
		SpVec parts = { 0 };
		for (int i = 0; i < raw.n; i++) { sp x = sp_trim(raw.v[i]); if (x.n) vpush(A, &parts, x); }
		if (parts.n > 64) ov = a_alloc(A, (size_t)parts.n * sizeof(double));
		for (int i = 0; i < parts.n; i++) {
			double o;
			if (!motion_offset(parts.v[i], &o)) { bad = 1; break; }
			if (o < 0 || o > 100) { bad = 1; break; }
			ov[no++] = o;
		}
		if (bad) {
			werr(w, child->line, child->col, F(w, "bad keyframe selector \"%.*s\"", PF(child->a)),
			     "use from, to, at 40%, or just 40% (a list works too: 0%, 100%). A keyframe sits between 0 and 100");
			continue;
		}
		PropVec props = { 0 };
		Deform df;
		memset(&df, 0, sizeof df);
		for (Node *d = child->first; d; d = d->next) {
			if (d->kind != K_DECL) continue;
			w->r.tmp.n = 0;
			resolve_decl(&w->r, d->a, d->value, d->line, d->col, &w->r.tmp);
			for (int k = 0; k < w->r.tmp.n; k++) {
				Decl *r = &w->r.tmp.v[k];
				if (sp_starts(r->prop, "--")) continue;
				if (r->prop.n && r->prop.p[0] == '@') {
					if (sp_eqs(r->prop, "@stretch") && r->kind == DV_STRETCH) {
						df.has_stretch = 1; df.stretch = r->st.factor; df.vertical = sp_eqs(r->st.axis, "vertical");
					} else if (sp_eqs(r->prop, "@bend") && r->kind == DV_NUM) { df.has_bend = 1; df.bend = r->num; }
					else if (sp_eqs(r->prop, "@bend-y") && r->kind == DV_NUM) { df.has_bendy = 1; df.bendy = r->num; }
					else if (sp_eqs(r->prop, "@fold") && r->kind == DV_NUM) { df.has_fold = 1; df.fold = r->num; }
					else if (sp_eqs(r->prop, "@bend-up") && r->kind == DV_NUM) { df.has_bendup = 1; df.bendup = r->num; }
					continue;
				}
				props_set(A, &props, camelize(A, r->prop), r->prop, r->value);
			}
		}
		if (df.has_bend || df.has_bendy || df.has_stretch || df.has_fold || df.has_bendup) {
			sp base = mk("", 0);
			for (int i = 0; i < props.n; i++) if (sp_eqs(props.v[i].camel, "transform")) base = props.v[i].value;
			const char *origin;
			sp t = compose_deform(A, &df, base, &origin);
			props_set(A, &props, LIT("transform"), LIT("transform"), t);
			if (origin) props_set(A, &props, LIT("transformOrigin"), LIT("transform-origin"), mk(origin, strlen(origin)));
		}
		for (int i = 0; i < no; i++) {
			Frame f;
			memset(&f, 0, sizeof f);
			f.offset = ov[i];
			if (props.n) {
				f.props.v = a_alloc(A, (size_t)props.n * sizeof(Prop));
				memcpy(f.props.v, props.v, (size_t)props.n * sizeof(Prop));
				f.props.n = f.props.cap = props.n;
			}
			vpush(A, &m->frames, f);
		}
	}
	/* a stable sort by offset (insertion sort: motions are short) */
	for (int i = 1; i < m->frames.n; i++) {
		Frame f = m->frames.v[i];
		int j = i - 1;
		while (j >= 0 && m->frames.v[j].offset > f.offset) { m->frames.v[j + 1] = m->frames.v[j]; j--; }
		m->frames.v[j + 1] = f;
	}
	return m;
}

/* ---------------------------------------------------------------- fonts (spec 8.7) */

static sp unq(sp w)
{
	if (w.n >= 2 && (w.p[0] == '"' || w.p[0] == '\'') && w.p[w.n - 1] == w.p[0]) return sub(w, 1, w.n - 1);
	return w;
}

static void parse_fonts(W *w, Node *node)
{
	Arena *A = A_(w);
	sp body = node->has_css ? node->css : mk("", 0);
	int firstLine = node->line;
	/* blank block comments, keeping line breaks */
	SB cb;
	sb_init(&cb, A);
	for (size_t i = 0; i < body.n; i++) {
		if (body.p[i] == '/' && i + 1 < body.n && body.p[i + 1] == '*') {
			size_t e = i + 2;
			while (e + 1 < body.n && !(body.p[e] == '*' && body.p[e + 1] == '/')) e++;
			if (e + 1 < body.n) {
				for (size_t k = i; k < e + 2; k++) sb_putc(&cb, body.p[k] == '\n' ? '\n' : ' ');
				i = e + 1;
				continue;
			}
		}
		sb_putc(&cb, body.p[i]);
	}
	sp clean = sb_done(&cb);
	typedef struct { sp fam; SpVec weights; int italic; } G;
	VEC(GVec, G);
	GVec google = { 0 };
	sp display = LIT("swap");
	static const char *const DISPLAYS[] = { "swap", "block", "fallback", "optional", "auto" };
	static const char *const W_WEIGHTS = "a weight (400), several (400 700), or a variable range (100..900)";
	static const char *const W_DISPLAY = "display <swap|block|fallback|optional>: how the text behaves while the font loads (default swap)";
	static const char *const W_FROM = "from \"/path/to/file.woff2\": use your own file instead of Google Fonts";
	SpVec lines = { 0 };
	split_char(A, clean, '\n', &lines);
	for (int li = 0; li < lines.n; li++) {
		int line = firstLine + li;
		sp raw = lines.v[li];
		/* raw.replace(/(^|\s)\/\/.*$/, '') */
		size_t cut = raw.n;
		for (size_t i = 0; i + 1 < raw.n + 0 && i < raw.n; i++) {
			if (i == 0 && raw.n >= 2 && raw.p[0] == '/' && raw.p[1] == '/') { cut = 0; break; }
			if (is_ws((unsigned char)raw.p[i]) && i + 2 < raw.n + 1 && i + 2 <= raw.n - 0 && i + 2 < raw.n + 1 && raw.p[i + 1] == '/' && i + 2 < raw.n && raw.p[i + 2] == '/') { cut = i; break; }
		}
		sp text = sp_trim(sub(raw, 0, cut));
		if (text.n && text.p[text.n - 1] == ';') text = sub(text, 0, text.n - 1);
		if (!text.n) continue;
		long colon = -1;
		char q = 0;
		for (size_t i = 0; i < text.n; i++) {
			char c = text.p[i];
			if (q) { if (c == q) q = 0; continue; }
			if (c == '"' || c == '\'') { q = c; continue; }
			if (c == ':') { colon = (long)i; break; }
		}
		if (colon < 0) {
			werr(w, line, node->col, F(w, "expected \"Family: weights\" in \"%.*s\"", PF(utf16_prefix(text, 40))), "e.g.  Fraunces: 500 700 800");
			continue;
		}
		sp famr = unq(sp_trim(sub(text, 0, (size_t)colon)));
		SB fb;
		sb_init(&fb, A);
		for (size_t i = 0; i < famr.n; i++) {
			if (is_ws((unsigned char)famr.p[i])) {
				sb_putc(&fb, ' ');
				while (i + 1 < famr.n && is_ws((unsigned char)famr.p[i + 1])) i++;
			} else sb_putc(&fb, famr.p[i]);
		}
		sp family = sb_done(&fb);
		if (!family.n) { werr(w, line, node->col, F(w, "a font line needs a family name before the colon"), NULL); continue; }
		/* words */
		SpVec ws = { 0 };
		sp rest = sub(text, (size_t)colon + 1, text.n);
		for (size_t i = 0; i < rest.n;) {
			if (is_ws((unsigned char)rest.p[i])) { i++; continue; }
			if (rest.p[i] == '"' || rest.p[i] == '\'') {
				char qq = rest.p[i];
				size_t j = i + 1;
				while (j < rest.n && rest.p[j] != qq) j++;
				size_t e = j + 1 > rest.n ? rest.n : j + 1;
				vpush(A, &ws, sub(rest, i, e));
				i = j + 1;
				continue;
			}
			size_t j = i;
			while (j < rest.n && !is_ws((unsigned char)rest.p[j])) j++;
			vpush(A, &ws, sub(rest, i, j));
			i = j;
		}
		SpVec weights = { 0 };
		int italic = 0, has_from = 0, has_disp = 0, stop = 0;
		sp from = mk("", 0), disp = mk("", 0);
		for (int i = 0; i < ws.n && !stop; i++) {
			sp x = ws.v[i];
			int is_w = is_int_str(x) && x.n <= 4;
			int is_range = 0;
			long dd = sp_find(x, "..");
			if (!is_w && dd > 0) {
				sp a1 = sub(x, 0, (size_t)dd), b1 = sub(x, (size_t)dd + 2, x.n);
				is_range = is_int_str(a1) && a1.n <= 4 && is_int_str(b1) && b1.n <= 4;
			}
			if (is_w || is_range) {
				double a1 = 0, b1 = 0;
				int hasb = 0;
				if (is_range) {
					long d2 = sp_find(x, "..");
					a1 = js_parse_float(sub(x, 0, (size_t)d2)); b1 = js_parse_float(sub(x, (size_t)d2 + 2, x.n)); hasb = 1;
				} else a1 = js_parse_float(x);
				if (a1 < 1 || a1 > 1000 || (hasb && (b1 < 1 || b1 > 1000 || b1 <= a1))) {
					werr(w, line, node->col, F(w, "\"%.*s\" is not a font weight", PF(x)), W_WEIGHTS);
					stop = 1;
					break;
				}
				vpush(A, &weights, x);
			} else if (sp_eqs(x, "italic")) italic = 1;
			else if (sp_eqs(x, "display")) {
				int ok = 0;
				if (i + 1 < ws.n) for (int k = 0; k < 5; k++) if (sp_eqs(ws.v[i + 1], DISPLAYS[k])) ok = 1;
				if (!ok) {
					werr(w, line, node->col, F(w, "display needs one of: swap, block, fallback, optional, auto"), W_DISPLAY);
					stop = 1;
					break;
				}
				disp = ws.v[++i]; has_disp = 1;
			} else if (sp_eqs(x, "from")) {
				if (i + 1 >= ws.n || !ws.v[i + 1].n) { werr(w, line, node->col, F(w, "from needs a file, e.g. from \"/fonts/brand.woff2\""), W_FROM); stop = 1; break; }
				from = unq(ws.v[++i]); has_from = 1;
			} else {
				Dym d;
				dym_init(&d, A, x);
				dym_list(&d, (const char *const[]){ "italic", "display", "from" }, 3);
				sp h;
				int has = dym_done(&d, &h);
				werr(w, line, node->col, F(w, "unknown font setting \"%.*s\"", PF(x)),
				     has ? a_fmt(A, "did you mean \"%.*s\"?", PF(h)).p : "a line is: Family: weights italic display swap, or Family: from \"file\" weight");
				stop = 1;
				break;
			}
		}
		if (stop) continue;
		if (has_from) {
			if (weights.n > 1) { werr(w, line, node->col, F(w, "one font file holds one weight (or one variable range), so give from a single weight"), "add one line per file"); continue; }
			sp path = from;
			long qm = sp_findc(path, '?');
			if (qm >= 0) path = sub(path, 0, (size_t)qm);
			const char *fmt = NULL;
			long dot = -1;
			for (size_t i = path.n; i > 0; i--) { if (path.p[i - 1] == '.') { dot = (long)i - 1; break; } if (!(is_alpha((unsigned char)path.p[i - 1]) || is_digit((unsigned char)path.p[i - 1]))) break; }
			if (dot >= 0) {
				sp ext = sub(path, (size_t)dot + 1, path.n);
				if (sp_ieq(ext, "woff2")) fmt = "woff2"; else if (sp_ieq(ext, "woff")) fmt = "woff";
				else if (sp_ieq(ext, "ttf")) fmt = "truetype"; else if (sp_ieq(ext, "otf")) fmt = "opentype";
			}
			sp weight = LIT("400");
			if (weights.n) {
				SB wb;
				sb_init(&wb, A);
				sp w0 = weights.v[0];
				long d2 = sp_find(w0, "..");
				if (d2 >= 0) { sb_put(&wb, w0.p, (size_t)d2); sb_putc(&wb, ' '); sb_put(&wb, w0.p + d2 + 2, w0.n - (size_t)d2 - 2); }
				else sb_putsp(&wb, w0);
				weight = sb_done(&wb);
			}
			SB cs;
			sb_init(&cs, A);
			sb_fmt(&cs, "@font-face {\n  font-family: \"%.*s\";\n  src: url(\"%.*s\")", PF(family), PF(from));
			if (fmt) sb_fmt(&cs, " format(\"%s\")", fmt);
			sb_fmt(&cs, ";\n  font-weight: %.*s;\n  font-style: %s;\n  font-display: %.*s;\n}", PF(weight), italic ? "italic" : "normal", PF(has_disp ? disp : LIT("swap")));
			Raw r = { sb_done(&cs), node->line, node->col, w->order++ };
			vpush(A, w->S ? &w->S->raws : NULL, r);
			continue;
		}
		if (has_disp) display = disp;
		G *g = NULL;
		for (int i = 0; i < google.n; i++) if (sp_eq(google.v[i].fam, family)) { g = &google.v[i]; break; }
		if (!g) { G ng; memset(&ng, 0, sizeof ng); ng.fam = family; vpush(A, &google, ng); g = &google.v[google.n - 1]; }
		for (int i = 0; i < weights.n; i++) {
			int dup = 0;
			for (int k = 0; k < g->weights.n; k++) if (sp_eq(g->weights.v[k], weights.v[i])) dup = 1;
			if (!dup) vpush(A, &g->weights, weights.v[i]);
		}
		if (italic) g->italic = 1;
	}
	if (google.n) {
		SB u;
		sb_init(&u, A);
		sb_puts(&u, "https://fonts.googleapis.com/css2?family=");
		for (int gi = 0; gi < google.n; gi++) {
			G *g = &google.v[gi];
			if (gi) sb_puts(&u, "&family=");
			SB nb;
			sb_init(&nb, A);
			for (size_t i = 0; i < g->fam.n; i++) sb_putc(&nb, g->fam.p[i] == ' ' ? '+' : g->fam.p[i]);
			sp name = sb_done(&nb);
			/* sort the weights by parseInt, stable */
			for (int i = 1; i < g->weights.n; i++) {
				sp x = g->weights.v[i];
				double xv = js_parse_float(x);
				int j = i - 1;
				while (j >= 0 && js_parse_float(g->weights.v[j]) > xv) { g->weights.v[j + 1] = g->weights.v[j]; j--; }
				g->weights.v[j + 1] = x;
			}
			if (!g->weights.n) { sb_putsp(&u, name); if (g->italic) sb_puts(&u, ":ital@0;1"); continue; }
			if (g->italic) {
				sb_putsp(&u, name);
				sb_puts(&u, ":ital,wght@");
				int first = 1;
				for (int it = 0; it < 2; it++) for (int k = 0; k < g->weights.n; k++) {
					if (!first) sb_putc(&u, ';');
					first = 0;
					sb_fmt(&u, "%d,%.*s", it, PF(g->weights.v[k]));
				}
			} else {
				sb_putsp(&u, name);
				sb_puts(&u, ":wght@");
				for (int k = 0; k < g->weights.n; k++) { if (k) sb_putc(&u, ';'); sb_putsp(&u, g->weights.v[k]); }
			}
		}
		sb_fmt(&u, "&display=%.*s", PF(display));
		FontLink fl = { sb_done(&u), firstLine };
		vpush(A, &w->S->fontlinks, fl);
	}
}

/* ---------------------------------------------------------------- the walk */

static int word_dash(sp s)
{
	if (!s.n || !is_alpha((unsigned char)s.p[0])) return 0;
	for (size_t i = 1; i < s.n; i++) { int c = (unsigned char)s.p[i]; if (!(is_alpha(c) || is_digit(c) || c == '-')) return 0; }
	return 1;
}

static void flush(W *w, Pend *sub_, const WCtx *ctx, int line, int col);

static void warn_display_clash(W *w, Decl *decls, int n)
{
	Arena *A = A_(w);
	Decl *explicit_ = NULL;
	for (int i = 0; i < n; i++) {
		Decl *d = &decls[i];
		if (!sp_eqs(d->prop, "display")) continue;
		if (!d->has_via) { explicit_ = d; continue; }
		if (explicit_ && !sp_eq(explicit_->value, d->value) && w->S->warnings.n < 400) {
			wwarn(w, d->line, d->col,
			      a_fmt(A, "\"%.*s:\" expands to display: %.*s, which overrides the earlier \"display: %.*s\" (line %d) in the same rule - later wins, so the earlier one is dead",
			            PF(d->via), PF(d->value), PF(explicit_->value), explicit_->line),
			      a_fmt(A, "if the override is intended, delete the dead line; otherwise put \"%.*s:\" first, or move the display you want to win AFTER it", PF(d->via)).p);
		}
	}
}

static void flush(W *w, Pend *sb_, const WCtx *ctx, int line, int col)
{
	Arena *A = A_(w);
	if (!sb_->decls.n) return;
	if (!ctx->sel.n) {
		werr(w, line, col, F(w, "declarations must live inside a selector"), "top-level blocks are tokens, behavior, motion, @layer, @media\xE2\x80\xA6 - or a selector like .card");
		return;
	}
	PSVec splits = { 0 };
	int mode = 0;
	analyze_pseudo(A, ctx->sel, &splits, &mode);
	if (mode == 1) {
		DeclVec decls = { 0 };
		for (int i = 0; i < sb_->decls.n; i++) {
			Decl d = sb_->decls.v[i];
			if (d.prop.n && d.prop.p[0] == '@' && !sp_eqs(d.prop, "@animate")) {
				sp pn = pseudo_head(splits.v[0].pseudo);
				werr_h(w, line, col, F(w, "\"%.*s\" is engine-native and has no meaning on ::%.*s - a pseudo-element has no DOM handle", PF(d.prop), PF(pn)),
				       LIT("put it on the element itself, not inside the pseudo rule"), 1);
				continue;
			}
			if (sp_eqs(d.prop, "content") && !d.value.n) d.value = LIT("\"\"");
			vpush(A, &decls, d);
		}
		for (int i = 0; i < splits.n; i++) {
			sp el = splits.v[i].el, pseudo = splits.v[i].pseudo;
			sp name = pseudo_head(pseudo);
			if (!vset_has(&V_PSEUDO, name.p, name.n) && !vendor_pseudo(name)) {
				werr(w, line, col, F(w, "unknown pseudo-element \"::%.*s\"", PF(pseudo)),
				     "try ::before, ::after, ::selection, ::part(name), or a vendor one like ::-webkit-scrollbar-thumb");
				continue;
			}
			if (sp_eqs(name, "slotted")) {
				werr(w, line, col, F(w, "::slotted() only exists inside a shadow tree, and DSSL styles the light DOM"),
				     "style the slotted element itself with a normal selector - it IS reachable from the page");
				continue;
			}
			if (has_seen_state(el)) {
				werr(w, line, col, F(w, "the engine-native \"seen\" state can't combine with \"::%.*s\" in shadow CSS", PF(name)),
				     "style the element itself in a seen { } block instead");
				continue;
			}
			if (!decls.n) continue;
			Rule *r = new_rule(w);
			r->id = w->rule_id++;
			r->sel = el; r->pseudo = pseudo; r->has_pseudo = 1;
			r->media = ctx->media; r->signals = ctx->signals; r->containers = ctx->containers;
			r->layer = ctx->layer; r->order = w->order++;
			r->decls = decls.v; r->ndecls = decls.n;
			r->line = line; r->col = col; r->is_shadow = 1;
			vpush(A, w->shadow, r);
		}
		return;
	}
	if (mode == 2) {
		werr(w, line, col, F(w, "selector list mixes pseudo-element and plain selectors: \"%.*s\"", PF(ctx->sel)),
		     "split it into two rules - one for the pseudo-element, one for the rest");
		return;
	}
	warn_display_clash(w, sb_->decls.v, sb_->decls.n);
	Rule *r = new_rule(w);
	r->id = w->rule_id++;
	r->sel = ctx->sel;
	r->media = ctx->media; r->signals = ctx->signals; r->containers = ctx->containers;
	r->layer = ctx->layer;
	r->order = w->order++;
	r->decls = sb_->decls.v; r->ndecls = sb_->decls.n;
	r->line = line; r->col = col;
	r->has_states = 1;
	r->states = scan_states(ctx->sel);
	vpush(A, w->rules, r);
}

static void walk(W *w, Node *first, const WCtx *ctx, Pend *rule);

static void flush_pending(W *w, Pend *rule, const WCtx *ctx, int line, int col)
{
	if (rule && rule->decls.n && ctx->sel.n) {
		flush(w, rule, ctx, line, col);
		memset(&rule->decls, 0, sizeof rule->decls);
	}
}

static void walk_sub(W *w, Node *node, const WCtx *child)
{
	Pend sub_;
	memset(&sub_, 0, sizeof sub_);
	walk(w, node->first, child, &sub_);
	flush(w, &sub_, child, node->line, node->col);
}

typedef struct { Rule *rule; DeclVec dv; } Tgt;
VEC(TgtVec, Tgt);

static void pseudo_fill2(W *w, const WCtx *ctx, Node *pn, Tgt *target, Node *kids, int is_top, sp base, TgtVec *states)
{
	Arena *A = A_(w);
	for (Node *d = kids; d; d = d->next) {
		if (d->kind == K_STATE && is_top) {
			if (sp_eqs(d->a, "seen")) {
				werr(w, d->line, d->col, F(w, "\"seen\" is engine-native and has no meaning on a pseudo-element"), "put seen { } on the element itself");
				continue;
			}
			Rule *st = new_rule(w);
			st->id = w->rule_id++;
			st->sel = a_fmt(A, "%.*s:%.*s", PF(base), PF(d->a));
			st->pseudo = pn->a; st->has_pseudo = 1;
			st->media = ctx->media; st->signals = ctx->signals; st->containers = ctx->containers;
			st->layer = ctx->layer; st->order = w->order++;
			st->line = d->line; st->col = d->col; st->is_shadow = 1;
			Tgt t;
			memset(&t, 0, sizeof t);
			t.rule = st;
			pseudo_fill2(w, ctx, pn, &t, d->first, 0, base, states);
			vpush(A, states, t);
			continue;
		}
		if (d->kind != K_DECL) {
			werr(w, d->line, d->col, F(w, "only declarations and states (hover { }) may appear inside a pseudo-element block"), NULL);
			continue;
		}
		if (sp_eqs(d->a, "content") && !d->value.n) {
			Decl x;
			memset(&x, 0, sizeof x);
			x.prop = LIT("content"); x.value = LIT("\"\""); x.line = d->line; x.col = d->col; x.kind = DV_STR;
			vpush(A, &target->dv, x);
			continue;
		}
		w->r.tmp.n = 0;
		resolve_decl(&w->r, d->a, d->value, d->line, d->col, &w->r.tmp);
		for (int k = 0; k < w->r.tmp.n; k++) {
			Decl r = w->r.tmp.v[k];
			if (r.prop.n && r.prop.p[0] == '@' && !sp_eqs(r.prop, "@animate")) {
				werr(w, d->line, d->col, F(w, "\"%.*s\" is engine-native and has no meaning on ::%.*s - a pseudo-element has no DOM handle", PF(d->a), PF(pn->a)),
				     "put it on the element itself, not inside the pseudo block");
				continue;
			}
			r.line = d->line; r.col = d->col;
			vpush(A, &target->dv, r);
		}
	}
}

static Rule *copy_shadow(W *w, const Rule *r, sp sel)
{
	Rule *c = new_rule(w);
	*c = *r;
	c->decls = a_alloc(A_(w), (size_t)(r->ndecls ? r->ndecls : 1) * sizeof(Decl));
	if (r->ndecls) memcpy(c->decls, r->decls, (size_t)r->ndecls * sizeof(Decl));
	c->id = w->rule_id++;
	c->order = w->order++;
	c->sel = sel;
	return c;
}

static void walk(W *w, Node *first, const WCtx *ctx, Pend *rule)
{
	Arena *A = A_(w);
	for (Node *node = first; node; node = node->next) {
		if (w->aborted) return;
		if (++w->steps > MAX_STEPS) {
			w->aborted = 1;
			werr(w, node->line, node->col, F(w, "this sheet is too large or too deeply reused (behaviors) to resolve; the rest is skipped"), NULL);
			return;
		}
		switch (node->kind) {
		case K_DECL: {
			if (sp_eqs(node->a, "use")) {
				sp name = sp_trim(node->value);
				Beh *bh = NULL;
				for (int i = 0; i < w->behs.n; i++) if (sp_eq(w->behs.v[i].name, name)) { bh = &w->behs.v[i]; break; }
				if (!bh) {
					Dym d;
					dym_init(&d, A, name);
					for (int i = 0; i < w->behs.n; i++) dym_sp(&d, w->behs.v[i].name);
					sp h;
					int has = dym_done(&d, &h);
					werr_h(w, node->line, node->col, F(w, "unknown behavior \"%.*s\"", PF(name)), h, has);
					break;
				}
				if (bh->active) { werr(w, node->line, node->col, F(w, "behavior cycle involving \"%.*s\"", PF(name)), NULL); break; }
				bh->active = 1;
				walk(w, bh->node->first, ctx, rule);
				bh->active = 0;
				break;
			}
			int start = rule->decls.n;
			resolve_decl(&w->r, node->a, node->value, node->line, node->col, &rule->decls);
			for (int i = start; i < rule->decls.n; i++) {
				Decl *r = &rule->decls.v[i];
				if (sp_eqs(r->prop, "display") && !sp_eqs(node->a, "display")) { r->via = node->a; r->has_via = 1; }
				r->line = node->line; r->col = node->col;
			}
			break;
		}
		case K_SELECTOR: {
			if (word_dash(node->a) && ctx->sel.n) {
				sp low = sp_lower(A, node->a);
				if (!vset_has(&V_HTML, low.p, low.n)) {
					Dym d;
					dym_init(&d, A, low);
					dym_set(&d, &V_STATE);
					sp near;
					if (dym_done(&d, &near)) {
						wwarn(w, node->line, node->col,
						      F(w, "\"%.*s\" compiled as a descendant selector (\"%.*s\")", PF(node->a), PF(combine(A, ctx->sel, node->a))),
						      a_fmt(A, "if you meant the state, it is spelled \"%.*s\" - this block currently styles descendants called <%.*s>", PF(near), PF(node->a)).p);
					}
				}
			}
			flush_pending(w, rule, ctx, node->line, node->col);
			WCtx child = *ctx;
			child.sel = ctx->sel.n == 0 ? node->a : combine(A, ctx->sel, node->a);
			walk_sub(w, node, &child);
			break;
		}
		case K_STATE: {
			if (!ctx->sel.n) { werr(w, node->line, node->col, F(w, "\"%.*s\" needs a selector around it", PF(node->a)), NULL); break; }
			flush_pending(w, rule, ctx, node->line, node->col);
			WCtx child = *ctx;
			child.sel = with_state(A, ctx->sel, node->a);
			if (sp_eqs(node->a, "seen")) {
				for (Node *d = node->first; d; d = d->next)
					if (d->kind == K_DECL && sp_has(d->value, "->"))
						werr(w, d->line, d->col, F(w, "\"%.*s: %.*s\" - the -> arrow is motion syntax and does nothing inside seen { }", PF(d->a), PF(d->value)),
						     "write the final style (seen { opacity: 1 }) or attach a motion (seen { animate: rise-in 400 })");
			}
			walk_sub(w, node, &child);
			break;
		}
		case K_MEDIA: {
			sp q = normalize_media_w(w, node->a, node->line, node->col);
			flush_pending(w, rule, ctx, node->line, node->col);
			WCtx child = *ctx;
			child.media.v = a_alloc(A, (size_t)(ctx->media.n + 1) * sizeof(sp));
			if (ctx->media.n) memcpy(child.media.v, ctx->media.v, (size_t)ctx->media.n * sizeof(sp));
			child.media.v[ctx->media.n] = q;
			child.media.n = ctx->media.n + 1;
			walk_sub(w, node, &child);
			break;
		}
		case K_CONTAINER: {
			sp q = normalize_media_w(w, node->a.n ? node->a : LIT("(min-width: 0)"), node->line, node->col);
			flush_pending(w, rule, ctx, node->line, node->col);
			WCtx child = *ctx;
			child.containers.v = a_alloc(A, (size_t)(ctx->containers.n + 1) * sizeof(sp));
			if (ctx->containers.n) memcpy(child.containers.v, ctx->containers.v, (size_t)ctx->containers.n * sizeof(sp));
			child.containers.v[ctx->containers.n] = q;
			child.containers.n = ctx->containers.n + 1;
			walk_sub(w, node, &child);
			break;
		}
		case K_SIGNAL: {
			Signal sg = parse_signal(w, node->a, node->line, node->col);
			add_feat(w->S, DSSL_FEAT_SIGNAL, "signal", node->line, node->col);
			flush_pending(w, rule, ctx, node->line, node->col);
			WCtx child = *ctx;
			child.signals.v = a_alloc(A, (size_t)(ctx->signals.n + 1) * sizeof(Signal));
			if (ctx->signals.n) memcpy(child.signals.v, ctx->signals.v, (size_t)ctx->signals.n * sizeof(Signal));
			child.signals.v[ctx->signals.n] = sg;
			child.signals.n = ctx->signals.n + 1;
			walk_sub(w, node, &child);
			break;
		}
		case K_LAYER:
		case K_PIN: {
			int layer = L_COMPONENTS;
			if (node->kind == K_PIN) layer = L_PIN;
			else {
				int found = -1;
				for (int i = 0; i < 5; i++) if (sp_eqs(node->a, LAYER_NAMES[i])) found = i;
				if (found >= 0) layer = found;
				else {
					Dym d;
					dym_init(&d, A, node->a);
					dym_list(&d, LAYER_NAMES, 5);
					sp h;
					int has = dym_done(&d, &h);
					wwarn(w, node->line, node->col, F(w, "unknown layer \"%.*s\", treated as \"components\"", PF(node->a)),
					      has ? h.p : "the layers are tokens, base, components, utilities, pin");
				}
			}
			flush_pending(w, rule, ctx, node->line, node->col);
			WCtx child = *ctx;
			child.layer = layer;
			walk_sub(w, node, &child);
			break;
		}
		case K_PSEUDO: {
			sp base = ctx->sel.n ? ctx->sel : LIT("*");
			SpVec parts = { 0 };
			split_top_level(A, base, &parts);
			Rule *srule = new_rule(w);
			srule->id = w->rule_id++;
			srule->sel = base; srule->pseudo = node->a; srule->has_pseudo = 1;
			srule->media = ctx->media; srule->signals = ctx->signals; srule->containers = ctx->containers;
			srule->layer = ctx->layer; srule->order = w->order++;
			srule->line = node->line; srule->col = node->col; srule->is_shadow = 1;
			Tgt top;
			memset(&top, 0, sizeof top);
			top.rule = srule;
			TgtVec states = { 0 };
			pseudo_fill2(w, ctx, node, &top, node->first, 1, base, &states);
			srule->decls = top.dv.v; srule->ndecls = top.dv.n;
			if (srule->ndecls) vpush(A, w->shadow, srule);
			for (int i = 0; i < states.n; i++) {
				Rule *st = states.v[i].rule;
				st->decls = states.v[i].dv.v; st->ndecls = states.v[i].dv.n;
				if (st->ndecls) vpush(A, w->shadow, st);
			}
			if (parts.n > 1) {
				RuleVec built = { 0 };
				if (srule->ndecls) vpush(A, &built, srule);
				for (int i = 0; i < states.n; i++) if (states.v[i].rule->ndecls) vpush(A, &built, states.v[i].rule);
				for (int pi = 1; pi < parts.n; pi++) {
					for (int bi = 0; bi < built.n; bi++) {
						Rule *r = built.v[bi];
						sp sel = a_cat2(A, parts.v[pi], sub(r->sel, base.n, r->sel.n));
						vpush(A, w->shadow, copy_shadow(w, r, sel));
					}
				}
				for (int bi = 0; bi < built.n; bi++) built.v[bi]->sel = a_cat2(A, parts.v[0], sub(built.v[bi]->sel, base.n, built.v[bi]->sel.n));
			}
			break;
		}
		case K_TOKENS: {
			if (ctx->sel.n) { werr(w, node->line, node->col, F(w, "tokens must be top-level or inside @media / @signal"), NULL); break; }
			SpVec own = { 0 };
			collect_tokens(w, node, mk("", 0), 0, &own);
			if (own.n) {
				Rule *tr = resolve_token_decls(w, &own, ctx);
				if (tr) { tr->line = 0; vpush(A, w->rules, tr); }
			}
			break;
		}
		case K_FONTS: {
			if (ctx->sel.n) break;
			parse_fonts(w, node);
			break;
		}
		case K_RAW: {
			sp css = node->has_css ? node->css : mk("", 0);
			sp t = sp_trim(css);
			if (t.n) { Raw r = { t, node->line, node->col, w->order++ }; vpush(A, &w->S->raws, r); }
			/* raw is real CSS: a declaration line with no ";" before another declaration or "}" is almost always a slip */
			{
				SB cb;
				sb_init(&cb, A);
				for (size_t i = 0; i < css.n; i++) {
					if (css.p[i] == '/' && i + 1 < css.n && css.p[i + 1] == '*') {
						size_t e = i + 2;
						while (e + 1 < css.n && !(css.p[e] == '*' && css.p[e + 1] == '/')) e++;
						if (e + 1 < css.n) {
							for (size_t k = i; k < e + 2; k++) sb_putc(&cb, css.p[k] == '\n' ? '\n' : ' ');
							i = e + 1;
							continue;
						}
					}
					sb_putc(&cb, css.p[i]);
				}
				SpVec rl = { 0 };
				split_char(A, sb_done(&cb), '\n', &rl);
				for (int i = 0; i < rl.n; i++) {
					sp tl = sp_trim(rl.v[i]);
					/* ^[a-zA-Z-]+\s*:\s*\S */
					#define IS_DECL(x) ({ sp _x = (x); size_t _k = 0; while (_k < _x.n && (is_alpha((unsigned char)_x.p[_k]) || _x.p[_k] == '-')) _k++; int _ok = 0; if (_k > 0) { while (_k < _x.n && is_ws((unsigned char)_x.p[_k])) _k++; if (_k < _x.n && _x.p[_k] == ':') { _k++; while (_k < _x.n && is_ws((unsigned char)_x.p[_k])) _k++; _ok = _k < _x.n; } } _ok; })
					if (!IS_DECL(tl)) continue;
					char lc = tl.p[tl.n - 1];
					if (lc == ';' || lc == '{' || lc == '}' || lc == ',') continue;
					sp nx = LIT("}");
					for (int j = i + 1; j < rl.n; j++) { sp tj = sp_trim(rl.v[j]); if (tj.n) { nx = tj; break; } }
					if (nx.p[0] == '}' || IS_DECL(nx))
						wwarn(w, node->line + i, node->col, F(w, "raw { } is verbatim CSS, so this line needs a \";\" at the end (\"%.*s\")", PF(utf16_prefix(tl, 40))),
						      "unlike the rest of DSSL, semicolons are required inside raw { }");
				}
			}
			break;
		}
		default:
			break;
		}
	}
}

/* ---------------------------------------------------------------- the end of the resolve */

static int rule_has_decl(const Rule *r, const char *prop)
{
	for (int i = 0; i < r->ndecls; i++) if (sp_eqs(r->decls[i].prop, prop)) return 1;
	return 0;
}

static void note_engine_decls(struct dssl_sheet *S, const Rule *r)
{
	for (int i = 0; i < r->ndecls; i++) {
		const Decl *d = &r->decls[i];
		if (!d->prop.n || d->prop.p[0] != '@') continue;
		sp p = d->prop;
		if (sp_eqs(p, "@animate")) {
			if (d->kind == DV_ANIM && d->anim->has_jitter) add_feat(S, DSSL_FEAT_ANIMATE_JITTER, "animate-jitter", d->line, d->col);
			else if (d->kind == DV_ANIM) add_feat(S, DSSL_FEAT_ANIMATE, "animate", d->line, d->col);
		} else if (sp_eqs(p, "@stagger")) add_feat(S, DSSL_FEAT_STAGGER, "stagger", d->line, d->col);
		else if (sp_eqs(p, "@texture")) add_feat(S, DSSL_FEAT_TEXTURE, "texture", d->line, d->col);
		else if (sp_starts(p, "@aura")) add_feat(S, DSSL_FEAT_AURA, "aura", d->line, d->col);
		else if (sp_eqs(p, "@bend") || sp_eqs(p, "@bend-y") || sp_eqs(p, "@bend-up") || sp_eqs(p, "@fold") || sp_eqs(p, "@stretch") || sp_eqs(p, "@bend-around"))
			add_feat(S, DSSL_FEAT_DEFORM, "deform", d->line, d->col);
		else if (sp_eqs(p, "@tilt")) add_feat(S, DSSL_FEAT_TILT, "tilt", d->line, d->col);
		else if (sp_eqs(p, "@slider")) add_feat(S, DSSL_FEAT_SLIDER, "slider", d->line, d->col);
		else if (sp_eqs(p, "@overlap")) add_feat(S, DSSL_FEAT_OVERLAP, "overlap", d->line, d->col);
		else if (sp_eqs(p, "@gradient-flow")) add_feat(S, DSSL_FEAT_GRADIENT_FLOW, "gradient-flow", d->line, d->col);
		else if (sp_eqs(p, "@stack")) add_feat(S, DSSL_FEAT_STACK, "stack", d->line, d->col);
	}
}

static int cmp_feat(const void *a, const void *b)
{
	const Feat *x = a, *y = b;
	if (x->line != y->line) return x->line < y->line ? -1 : 1;
	if (x->col != y->col) return x->col < y->col ? -1 : 1;
	return 0;
}

static void link_animation(W *w, Decl *d)
{
	Arena *A = A_(w);
	Anim *an = d->anim;
	if (!an->motion.n) return;
	int from_base;
	Motion *m = parse_motion(w, an->motion, &from_base);
	if (!m) {
		Dym dm;
		dym_init(&dm, A, an->motion);
		for (int i = 0; i < w->mots.n; i++) dym_sp(&dm, w->mots.v[i].name);
		sp h;
		if (dym_done(&dm, &h)) werr_h(w, d->line, d->col, F(w, "unknown motion \"%.*s\"", PF(an->motion)), h, 1);
		else werr(w, d->line, d->col, F(w, "unknown motion \"%.*s\"", PF(an->motion)),
		          a_fmt(A, "define it first:  motion %.*s { from { } to { } }  - then \"animate: %.*s 400\" runs it", PF(an->motion), PF(an->motion)).p);
		an->broken = 1;
		return;
	}
	an->linked = from_base ? copy_motion(A, m) : m;
	an->link_essential = m->essential || an->essential_here;
}

static int known_motion(W *w, sp name)
{
	for (int i = 0; i < w->mots.n; i++) if (sp_eq(w->mots.v[i].name, name)) return 1;
	return base_motion(w, name) != NULL;
}

static void final_pass(W *w)
{
	Arena *A = A_(w);
	struct dssl_sheet *S = w->S;
	RuleVec *lists[2] = { w->rules, w->shadow };
	for (int li = 0; li < 2; li++) {
		for (int ri = 0; ri < lists[li]->n; ri++) {
			Rule *r = lists[li]->v[ri];
			/* gradient: next to another background image */
			{
				Decl *g = NULL;
				for (int i = 0; i < r->ndecls; i++) if (r->decls[i].from_gradient) { g = &r->decls[i]; break; }
				if (g) {
					Decl *other = NULL;
					for (int i = 0; i < r->ndecls; i++) {
						Decl *x = &r->decls[i];
						if (x == g || x->from_gradient) continue;
						if (sp_eqs(x->prop, "background-image")) { other = x; break; }
						if (sp_eqs(x->prop, "background") && (sp_has(x->value, "url(") || sp_has(x->value, "gradient("))) { other = x; break; }
					}
					if (other) {
						int ln = g->line > other->line ? g->line : other->line;
						if (!ln) ln = g->line;
						wwarn(w, ln, g->col,
						      F(w, "\"%.*s\" sets the background image twice (gradient: and %.*s): the later one replaces the earlier, so one of them never shows", PF(r->sel), PF(other->prop)),
						      "layer them in one gradient: sentence, joined with +, first on top:  gradient: radial circle, #fff4 -> transparent tile 24 + linear 180, #e33 -> #a11  (a CSS radial-gradient(...) or url(...) works as a layer too)");
					}
				}
			}
			/* animation-* longhands with nothing to tune */
			{
				int has_css_anim = 0, uses_animate = 0;
				sp first_motion = LIT("name");
				int have_first = 0;
				for (int i = 0; i < r->ndecls; i++) {
					Decl *x = &r->decls[i];
					if (sp_eqs(x->prop, "animation") || sp_eqs(x->prop, "animation-name")) has_css_anim = 1;
					if (sp_eqs(x->prop, "@animate") && x->kind == DV_ANIM) { uses_animate = 1; if (!have_first) { first_motion = x->anim->motion; have_first = 1; } }
				}
				static const char *const kinds[] = { "delay", "duration", "iteration-count", "direction", "fill-mode", "timing-function", "play-state" };
				static const char *const words[] = { "delay", "a number after the name", "infinite, or iterations 3", "alternate", "both, forwards", "an easing name", "" };
				for (int i = 0; i < r->ndecls; i++) {
					Decl *x = &r->decls[i];
					if (!sp_starts(x->prop, "animation-")) continue;
					sp tail = sub(x->prop, 10, x->prop.n);
					int k = -1;
					for (int q = 0; q < 7; q++) if (sp_eqs(tail, kinds[q])) k = q;
					if (k < 0 || has_css_anim) continue;
					wwarn(w, x->line, x->col,
					      F(w, "%.*s does nothing here: this rule has no CSS animation to tune%s", PF(x->prop), uses_animate ? ", it runs its motion through animate:" : ""),
					      a_fmt(A, "put it in the animate: line, e.g.  animate: %.*s 400 %s  (words: delay N, jitter N, iterations N, infinite, alternate, an easing, essential). Or write the whole  animation: name 400ms ...  shorthand",
					            PF(first_motion), k == 0 ? "delay 200" : words[k]).p);
				}
			}
			for (int i = 0; i < r->ndecls; i++) {
				Decl *d = &r->decls[i];
				if (sp_eqs(d->prop, "@animate") && d->kind == DV_ANIM) link_animation(w, d);
				if ((sp_eqs(d->prop, "animation") || sp_eqs(d->prop, "animation-name")) && d->kind == DV_STR) {
					int nwords = 0, named = 0;
					size_t k = 0;
					while (k < d->value.n) {
						while (k < d->value.n && (is_ws((unsigned char)d->value.p[k]) || d->value.p[k] == ',')) k++;
						size_t st = k;
						while (k < d->value.n && !(is_ws((unsigned char)d->value.p[k]) || d->value.p[k] == ',')) k++;
						if (k > st) {
							sp wd = mk(d->value.p + st, k - st);
							int ok = is_alpha((unsigned char)wd.p[0]);
							for (size_t q = 1; ok && q < wd.n; q++) if (!(is_word((unsigned char)wd.p[q]) || wd.p[q] == '-')) ok = 0;
							if (ok) { nwords++; if (known_motion(w, wd)) named = 1; }
						}
					}
					if (nwords && !named)
						wwarn(w, d->line, d->col, F(w, "animation \"%.*s\" references no motion defined in this sheet - it will not run", PF(d->value)),
						      "motions are defined with  motion name { from { } to { } }  and run with  animate: name 400  - @keyframes does not exist in DSSL; raw { } can carry verbatim keyframes");
				}
			}
		}
	}
	(void)S;
}

static void dedupe_errors(W *w, int from)
{
	DiagVec *e = w->errors;
	int n = e->n;
	if (n - from < 2) return;
	int cap = 64;
	while (cap < (n - from) * 2) cap *= 2;
	int *tab = a_alloc(A_(w), (size_t)cap * sizeof(int));
	for (int i = 0; i < cap; i++) tab[i] = -1;
	char *drop = a_zalloc(A_(w), (size_t)n);
	for (int i = n - 1; i >= from; i--) {
		uint32_t h = (uint32_t)e->v[i].line * 2654435761u;
		for (size_t k = 0; k < e->v[i].msg.n; k++) { h ^= (unsigned char)e->v[i].msg.p[k]; h *= 16777619u; }
		uint32_t pos = h & (uint32_t)(cap - 1);
		int dup = 0;
		while (tab[pos] >= 0) {
			int j = tab[pos];
			if (e->v[j].line == e->v[i].line && sp_eq(e->v[j].msg, e->v[i].msg)) { dup = 1; break; }
			pos = (pos + 1) & (uint32_t)(cap - 1);
		}
		if (dup) drop[i] = 1; else tab[pos] = i;
	}
	int o = from;
	for (int i = from; i < n; i++) if (!drop[i]) e->v[o++] = e->v[i];
	e->n = o;
}

void resolve_sheet(struct dssl_sheet *S, struct dssl_ctx *cx, Node *root)
{
	Arena *A = &S->A;
	W *w = a_zalloc(A, sizeof *w);
	w->S = S;
	w->cx = cx;
	w->r.S = S;
	w->r.A = A;
	w->r.file = S->file;
	w->errors = w->r.errors = &S->errors;
	w->warnings = w->r.warnings = &S->warnings;
	w->rules = &S->rules;
	w->shadow = &S->shadow;
	tm_clone(A, &w->r.tokens, &cx->tokens);
	int err_from = S->errors.n;

	for (Node *n = root->first; n; n = n->next) prepass_node(w, n);

	/* the registry rule: every token in the map, first order number */
	WCtx top;
	memset(&top, 0, sizeof top);
	top.layer = L_COMPONENTS;
	{
		SpVec keys = { 0 };
		for (int i = 0; i < w->r.tokens.n; i++) vpush(A, &keys, w->r.tokens.e[i].key);
		S->registry = resolve_token_decls(w, &keys, &top);
	}

	Pend rootp;
	memset(&rootp, 0, sizeof rootp);
	walk(w, root->first, &top, &rootp);
	flush(w, &rootp, &top, 0, 0);

	final_pass(w);

	/* every motion is built, not only the referenced ones */
	for (int i = 0; i < w->mots.n; i++) {
		int fb;
		Motion *m = parse_motion(w, w->mots.v[i].name, &fb);
		vpush(A, &S->motion_names, w->mots.v[i].name);
		if (m && m->frames.n) vpush(A, &S->motions, m);
	}

	/* media queries, distinct, rules first */
	for (int li = 0; li < 2; li++) {
		RuleVec *l = li ? w->shadow : w->rules;
		for (int i = 0; i < l->n; i++)
			for (int k = 0; k < l->v[i]->media.n; k++) {
				sp q = l->v[i]->media.v[k];
				int seen = 0;
				for (int j = 0; j < S->media_queries.n; j++) if (sp_eq(S->media_queries.v[j], q)) { seen = 1; break; }
				if (!seen) vpush(A, &S->media_queries, q);
			}
	}
	dedupe_errors(w, err_from);

	/* features: engine-native declarations, seen, alignment promotion */
	for (int li = 0; li < 2; li++) {
		RuleVec *l = li ? w->shadow : w->rules;
		for (int i = 0; i < l->n; i++) {
			Rule *r = l->v[i];
			note_engine_decls(S, r);
			if (r->has_states && (r->states & (1u << 19))) add_feat(S, DSSL_FEAT_SEEN, "seen", r->line, r->col);
			if (li == 0 && !r->is_token && !r->media.n && !r->signals.n && !r->containers.n && !r->states) {
				int align = -1;
				static const char *const AP[] = { "place-items", "place-content", "align-items", "justify-items", "align-content", "justify-content" };
				for (int k = 0; k < r->ndecls && align < 0; k++) for (int q = 0; q < 6; q++) if (sp_eqs(r->decls[k].prop, AP[q])) { align = k; break; }
				if (align >= 0 && !rule_has_decl(r, "display"))
					add_feat(S, DSSL_FEAT_ALIGN_PROMOTION, "align-promotion", r->decls[align].line, r->decls[align].col);
			}
		}
	}
	if (S->feats.n > 1) qsort(S->feats.v, (size_t)S->feats.n, sizeof(Feat), cmp_feat);

	/* the final token map */
	tm_copy(A, &S->tokens, &w->r.tokens);

	/* the CSS lane's registry rule: everything the first sheet of a context, only new keys after */
	{
		Rule *reg = S->registry;
		if (reg) {
			Rule *rc = new_rule(w);
			*rc = *reg;
			rc->decls = a_alloc(A, (size_t)(reg->ndecls ? reg->ndecls : 1) * sizeof(Decl));
			int o = 0;
			for (int i = 0; i < reg->ndecls; i++) {
				int include = !cx->installed;
				if (!include) {
					sp key = S->tokens.e[i].key;
					include = tm_find(&cx->tokens, key) == NULL;
				}
				if (include) rc->decls[o++] = reg->decls[i];
			}
			rc->ndecls = o;
			S->registry_css = o ? rc : NULL;
		}
	}
	long maxo = -1;
	for (int i = 0; i < S->rules.n; i++) if (S->rules.v[i]->order > maxo) maxo = S->rules.v[i]->order;
	for (int i = 0; i < S->shadow.n; i++) if (S->shadow.v[i]->order > maxo) maxo = S->shadow.v[i]->order;
	for (int i = 0; i < S->raws.n; i++) if (S->raws.v[i].order > maxo) maxo = S->raws.v[i].order;
	if (S->registry && S->registry->order > maxo) maxo = S->registry->order;
	S->max_order = maxo;
}

/* ---------------------------------------------------------------- the public API */

#define LIMIT_BASE ((size_t)48 << 20)
#define INPUT_MAX ((size_t)32 << 20)

dssl_ctx *dssl_ctx_new(void)
{
	dssl_ctx *volatile c = calloc(1, sizeof *c);
	if (!c) return NULL;
	jmp_buf jb;
	arena_init(&c->A, (size_t)256 << 20);
	c->A.jb = &jb;
	if (setjmp(jb)) { arena_free(&c->A); free(c); return NULL; }
	tm_init(&c->tokens);
	for (unsigned i = 0; i < V_DEFAULT_TOKENS_N; i++)
		tm_set(&c->A, &c->tokens, mk(V_DEFAULT_TOKENS[i].key, strlen(V_DEFAULT_TOKENS[i].key)), mk(V_DEFAULT_TOKENS[i].value, strlen(V_DEFAULT_TOKENS[i].value)), 0, 0, 0);
	c->A.jb = NULL;
	return c;
}

void dssl_ctx_free(dssl_ctx *c)
{
	if (!c) return;
	arena_free(&c->A);
	free(c);
}

void dssl_free(dssl_sheet *s)
{
	if (!s) return;
	arena_free(&s->A);
	free(s);
}

static void fold_into_ctx(struct dssl_sheet *S, struct dssl_ctx *cx)
{
	Arena *A = &cx->A;
	/* the tokens of the sheet's final map, the motions that have frames */
	for (int i = 0; i < S->tokens.n; i++) {
		TokEnt *e = tm_find(&cx->tokens, S->tokens.e[i].key);
		if (e && sp_eq(e->value, S->tokens.e[i].value)) continue;
		tm_set(A, &cx->tokens, a_dup(A, S->tokens.e[i].key), a_dup(A, S->tokens.e[i].value), 0, 0, 0);
	}
	for (int i = 0; i < S->motions.n; i++) {
		Motion *m = copy_motion(A, S->motions.v[i]);
		int found = -1;
		for (int k = 0; k < cx->motions.n; k++) if (sp_eq(cx->motions.v[k]->name, m->name)) { found = k; break; }
		if (found >= 0) cx->motions.v[found] = m; else vpush(A, &cx->motions, m);
	}
	cx->next_order += S->max_order + 1;
	cx->installed = 1;
}

dssl_sheet *dssl_parse(const char *src, size_t len, const char *url, dssl_ctx *ctx_in)
{
	if (len > INPUT_MAX) return NULL;
	if (!src) len = 0;
	dssl_sheet *volatile S = calloc(1, sizeof *S);
	if (!S) return NULL;
	dssl_ctx *volatile own = NULL;
	dssl_ctx *volatile cx = ctx_in;
	if (!cx) { own = cx = dssl_ctx_new(); if (!cx) { free(S); return NULL; } }
	jmp_buf jb;
	arena_init(&S->A, LIMIT_BASE + len * 64);
	S->A.jb = &jb;
	cx->A.jb = &jb;
	if (setjmp(jb)) {
		/* out of memory or past the limit: give nothing back */
		arena_free(&S->A);
		free(S);
		cx->A.jb = NULL;
		dssl_ctx_free(own);
		return NULL;
	}
	Arena *A = &S->A;
	S->file = a_dups(A, url ? url : "inline.dssl");
	S->order_base = cx->next_order;
	/* decode: invalid UTF-8 becomes U+FFFD, a leading BOM is dropped (as TextDecoder does) */
	char *buf = a_alloc(A, len * 3 + 1);
	size_t n = utf8_sanitize(src, len, buf);
	sp text = mk(buf, n);
	if (n >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) text = sub(text, 3, n);
	int deep = 0;
	DiagVec perrs = { 0 };
	S->root = parse_source(A, S->file, text, &perrs, &deep);
	for (int i = 0; i < perrs.n; i++) {
		Diag d = perrs.v[i];
		vpush(A, &S->errors, d);
	}
	S->n_parse_errors = S->errors.n;
	resolve_sheet(S, cx, S->root);
	fold_into_ctx(S, cx);
	S->A.jb = NULL;
	cx->A.jb = NULL;
	dssl_ctx_free(own);
	return S;
}

/* ---------------------------------------------------------------- accessors */

size_t dssl_error_count(const dssl_sheet *s) { return s ? (size_t)s->errors.n : 0; }
size_t dssl_warning_count(const dssl_sheet *s) { return s ? (size_t)s->warnings.n : 0; }

static int diag_at(const dssl_sheet *s, const DiagVec *v, size_t i, dssl_diag *out)
{
	if (!s || i >= (size_t)v->n) return 0;
	const Diag *d = &v->v[i];
	out->line = d->line;
	out->col = d->col;
	out->file = s->file.p;
	out->message = d->msg.p;
	out->hint = d->has_hint ? d->hint.p : NULL;
	return 1;
}
int dssl_error_at(const dssl_sheet *s, size_t i, dssl_diag *out) { return s && diag_at(s, &s->errors, i, out); }
int dssl_warning_at(const dssl_sheet *s, size_t i, dssl_diag *out) { return s && diag_at(s, &s->warnings, i, out); }

unsigned dssl_needs_engine(const dssl_sheet *s) { return s ? (s->feat_mask & DSSL_FEAT_HARD_MASK) : 0; }
unsigned dssl_features(const dssl_sheet *s) { return s ? s->feat_mask : 0; }
size_t dssl_feature_count(const dssl_sheet *s) { return s ? (size_t)s->feats.n : 0; }
int dssl_feature_at(const dssl_sheet *s, size_t i, dssl_feature *out)
{
	if (!s || i >= (size_t)s->feats.n) return 0;
	out->kind = s->feats.v[i].kind;
	out->name = s->feats.v[i].name;
	out->line = s->feats.v[i].line;
	out->col = s->feats.v[i].col;
	return 1;
}
size_t dssl_rule_count(const dssl_sheet *s) { return s ? (size_t)s->rules.n : 0; }
size_t dssl_shadow_rule_count(const dssl_sheet *s) { return s ? (size_t)s->shadow.n : 0; }
long dssl_order_base(const dssl_sheet *s) { return s ? s->order_base : 0; }

char *dssl_to_json(const dssl_sheet *s) { return s ? emit_json(s) : NULL; }
char *dssl_tree_json(const dssl_sheet *s) { return s ? emit_tree_json(s) : NULL; }
char *dssl_to_css(const dssl_sheet *s, unsigned flags) { return s ? emit_css(s, flags) : NULL; }
