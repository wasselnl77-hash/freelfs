/* libdssl: declarations and the semantic properties (spec chapter 6, 7.2, 8.4 glass and tilt,
 * 8.5 deform). Port of resolveDecl() and SEMANTIC of resolver.js 0.16.3. SPDX-License-Identifier: MIT */
#include "internal.h"
#include <math.h>
#include <stdlib.h>

/* ---------------------------------------------------------------- helpers */

static void verr(VC *vc, sp msg, const char *hint)
{
	diag_push(vc->r->A, vc->errors, vc->line, vc->col, msg, hint, mk(NULL, 0), 0);
}
static void verr_dym(VC *vc, sp msg, Dym *d)
{
	sp h;
	int has = dym_done(d, &h);
	diag_push(vc->r->A, vc->errors, vc->line, vc->col, msg, NULL, has ? h : mk(NULL, 0), has);
}
static void vwarn(VC *vc, sp msg, const char *hint)
{
	diag_push(vc->r->A, vc->r->warnings, vc->line, vc->col, msg, hint, mk(NULL, 0), 0);
}
#define ERRF(vc, hint, ...) verr((vc), a_fmt((vc)->r->A, __VA_ARGS__), (hint))

static Decl *push(VC *vc, DeclVec *out, sp prop, sp value)
{
	Decl d;
	memset(&d, 0, sizeof d);
	d.prop = prop;
	d.value = value;
	d.kind = DV_STR;
	vpush(vc->r->A, out, d);
	return &out->v[out->n - 1];
}
static Decl *pushl(VC *vc, DeclVec *out, const char *prop, const char *value)
{
	return push(vc, out, mk(prop, strlen(prop)), mk(value, strlen(value)));
}
static Decl *push_kind(VC *vc, DeclVec *out, const char *prop, DVKind k)
{
	Decl *d = push(vc, out, mk(prop, strlen(prop)), mk("", 0));
	d->kind = k;
	return d;
}

static sp lenv(VC *vc, sp raw) { VC v = *vc; v.unitless = 0; return resolve_value(&v, raw); }
static sp numv(VC *vc, sp raw) { VC v = *vc; v.unitless = 1; return resolve_value(&v, raw); }
static sp plainv(VC *vc, sp raw) { VC v = *vc; v.unitless = 0; v.bare = 0; return resolve_value(&v, raw); }
static sp S(VC *vc, const char *z) { (void)vc; return mk(z, strlen(z)); }

/* (v || dflt) */
static sp or_default(sp v, const char *dflt) { return v.n ? v : mk(dflt, strlen(dflt)); }

static sp join_sp(Arena *a, const SpVec *v, int from, const char *sep)
{
	SB b;
	sb_init(&b, a);
	for (int i = from; i < v->n; i++) {
		if (i > from) sb_puts(&b, sep);
		sb_putsp(&b, v->v[i]);
	}
	return sb_done(&b);
}

static int num_form(sp t)
{
	/* -?\d*\.?\d+ over the whole text */
	size_t i = 0;
	if (i < t.n && t.p[i] == '-') i++;
	size_t d1 = i;
	while (i < t.n && is_digit((unsigned char)t.p[i])) i++;
	if (i == t.n) return i > d1;
	if (t.p[i] != '.') return 0;
	i++;
	size_t d2 = i;
	while (i < t.n && is_digit((unsigned char)t.p[i])) i++;
	return i == t.n && i > d2;
}

/* ^\d+(\.\d+)?(ms|s)?$ */
static int dur_form(sp t)
{
	size_t i = 0;
	while (i < t.n && is_digit((unsigned char)t.p[i])) i++;
	if (i == 0) return 0;
	if (i < t.n && t.p[i] == '.') {
		size_t j = i + 1;
		while (j < t.n && is_digit((unsigned char)t.p[j])) j++;
		if (j == i + 1) return 0;
		i = j;
	}
	if (i < t.n && t.p[i] == 'm') { if (i + 1 < t.n && t.p[i + 1] == 's') i += 2; else return 0; }
	else if (i < t.n && t.p[i] == 's') i++;
	return i == t.n;
}
/* ^\d*\.?\d+(ms|s)?$ */
static int dur_form2(sp t)
{
	size_t i = 0;
	while (i < t.n && is_digit((unsigned char)t.p[i])) i++;
	size_t d1 = i;
	if (i < t.n && t.p[i] == '.') {
		size_t j = i + 1;
		while (j < t.n && is_digit((unsigned char)t.p[j])) j++;
		if (j == i + 1) return 0;
		i = j;
	} else if (d1 == 0) return 0;
	if (i < t.n && t.p[i] == 'm') { if (i + 1 < t.n && t.p[i + 1] == 's') i += 2; else return 0; }
	else if (i < t.n && t.p[i] == 's') i++;
	return i == t.n;
}

static double to_ms(sp a)
{
	double v = js_parse_float(a);
	if (sp_ends(a, "s") && !sp_ends(a, "ms")) return v * 1000;
	return v;
}

/* topSplit: split on a top-level separator, parentheses only (resolver.js topSplit) */
static void top_split(Arena *a, sp s, char sep, SpVec *out)
{
	int depth = 0;
	size_t st = 0;
	for (size_t i = 0; i < s.n; i++) {
		char ch = s.p[i];
		if (ch == '(') depth++;
		else if (ch == ')') { if (depth > 0) depth--; }
		if (ch == sep && depth == 0) { vpush(a, out, mk(s.p + st, i - st)); st = i + 1; }
	}
	vpush(a, out, mk(s.p + st, s.n - st));
}
/* ... .map(trim).filter(Boolean) */
static void split_trim(Arena *a, sp s, char sep, SpVec *out)
{
	SpVec t = { 0 };
	top_split(a, s, sep, &t);
	for (int i = 0; i < t.n; i++) {
		sp x = sp_trim(t.v[i]);
		if (x.n) vpush(a, out, x);
	}
}

static const char *const EASING_NAMES[] = { "snap", "smooth", "bounce", "linear", "ease", "ease-in", "ease-out", "ease-in-out" };
static const char *const EASING_CSS[] = { "cubic-bezier(0.2, 0.9, 0.3, 1)", "cubic-bezier(0.4, 0, 0.2, 1)", "cubic-bezier(0.34, 1.56, 0.64, 1)", "linear", "ease", "ease-in", "ease-out", "ease-in-out" };
static const char *easing_of(sp a)
{
	for (int i = 0; i < 8; i++) if (sp_eqs(a, EASING_NAMES[i])) return EASING_CSS[i];
	return NULL;
}

static const struct { const char *name, *cross, *main; } PLACE[] = {
	{ "center", "center", "center" }, { "start", "start", "flex-start" }, { "end", "end", "flex-end" },
	{ "between", "center", "space-between" }, { "around", "center", "space-around" }, { "evenly", "center", "space-evenly" },
	{ "stretch", "stretch", "flex-start" }, { "baseline", "baseline", "flex-start" },
};
static int place_of(sp w)
{
	for (int i = 0; i < 8; i++) if (sp_eqs(w, PLACE[i].name)) return i;
	return -1;
}
static const char *const PLACE_NAMES[] = { "center", "start", "end", "between", "around", "evenly", "stretch", "baseline" };

/* ---------------------------------------------------------------- row, column, grid, ... */

static void sem_flex(VC *vc, sp value, DeclVec *out, const char *dir)
{
	Arena *A = vc->r->A;
	int base = out->n;
	pushl(vc, out, "display", "flex");
	pushl(vc, out, "flex-direction", dir);
	SpVec atoms = { 0 };
	split_ws(A, value, 0, &atoms);
	sp main_set = mk(NULL, 0), cross_set = mk(NULL, 0);
	int has_main = 0, has_cross = 0;
	for (int k = 0; k < atoms.n; k++) {
		sp a = atoms.v[k];
		if (sp_eqs(a, "gap")) {
			if (k + 1 >= atoms.n) { verr(vc, S(vc, "gap needs a value"), NULL); break; }
			k++;
			push(vc, out, LIT("gap"), lenv(vc, atoms.v[k]));
			continue;
		}
		if (sp_eqs(a, "wrap")) { pushl(vc, out, "flex-wrap", "wrap"); continue; }
		if (sp_eqs(a, "nowrap")) { pushl(vc, out, "flex-wrap", "nowrap"); continue; }
		if (sp_eqs(a, "reverse")) { out->v[base + 1].value = a_fmt(A, "%s-reverse", dir); continue; }
		if (sp_eqs(a, "inline")) { out->v[base].value = LIT("inline-flex"); continue; }
		int role = 0;   /* 1 main, 2 cross, 3 both */
		if (sp_eqs(a, "between") || sp_eqs(a, "around") || sp_eqs(a, "evenly")) role = 1;
		else if (sp_eqs(a, "stretch") || sp_eqs(a, "baseline")) role = 2;
		else if (sp_eqs(a, "center") || sp_eqs(a, "start") || sp_eqs(a, "end")) role = 3;
		if (role) {
			int pc = place_of(a);
			int wrote = 0;
			if (role == 1 || (role == 3 && !has_main)) {
				if (has_main) {
					if (sp_eq(main_set, a))
						vwarn(vc, a_fmt(A, "\"%.*s\" is written twice in %s - the second copy wins", PF(a), dir), "drop the duplicate; one distribution word + one alignment word is the whole grammar");
					else
						vwarn(vc, a_fmt(A, "two words in %s write justify-content: \"%.*s\" overrides \"%.*s\"", dir, PF(a), PF(main_set)), "later words win - drop one, or reorder so the distribution word comes first");
				}
				pushl(vc, out, "justify-content", PLACE[pc].main);
				main_set = a; has_main = 1; wrote = 1;
			}
			if (role == 2 || (role == 3 && !has_cross)) {
				if (has_cross) {
					if (sp_eq(cross_set, a))
						vwarn(vc, a_fmt(A, "\"%.*s\" is written twice in %s - the second copy wins", PF(a), dir), "drop the duplicate; one distribution word + one alignment word is the whole grammar");
					else
						vwarn(vc, a_fmt(A, "two words in %s write align-items: \"%.*s\" overrides \"%.*s\"", dir, PF(a), PF(cross_set)), "later words win - drop one, or reorder so the distribution word comes first");
				}
				pushl(vc, out, "align-items", PLACE[pc].cross);
				cross_set = a; has_cross = 1; wrote = 1;
			}
			if (!wrote) {
				sp msg;
				if ((has_main && sp_eq(main_set, a)) || (has_cross && sp_eq(cross_set, a)))
					msg = a_fmt(A, "\"%.*s\" is written twice in %s - both axes already came from it", PF(a), dir);
				else
					msg = a_fmt(A, "\"%.*s\" has nowhere to go: justify-content came from \"%.*s\", align-items came from \"%.*s\"", PF(a), PF(main_set), PF(cross_set));
				vwarn(vc, msg, "one distribution word + one alignment word is the whole grammar - the extra word is dropped");
			}
			continue;
		}
		static const char *const words[] = { "gap", "center", "start", "end", "between", "around", "evenly", "stretch", "baseline", "wrap", "nowrap", "reverse", "inline" };
		Dym d;
		dym_init(&d, A, a);
		dym_list(&d, words, 13);
		verr_dym(vc, a_fmt(A, "unknown word \"%.*s\" in %s", PF(a), dir), &d);
	}
}

static void sem_grid(VC *vc, sp v, DeclVec *out)
{
	Arena *A = vc->r->A;
	SpVec atoms = { 0 }, rest = { 0 };
	split_ws(A, v, 0, &atoms);
	sp gap = mk(NULL, 0), rows = mk(NULL, 0);
	int has_gap = 0, has_rows = 0;
	for (int i = 0; i < atoms.n; i++) {
		if (sp_eqs(atoms.v[i], "gap") && i + 1 < atoms.n) { gap = atoms.v[++i]; has_gap = 1; continue; }
		if (sp_eqs(atoms.v[i], "rows") && i + 1 < atoms.n) { rows = atoms.v[++i]; has_rows = 1; continue; }
		vpush(A, &rest, atoms.v[i]);
	}
	sp cols;
	if (!rest.n) {
		verr(vc, S(vc, "grid needs columns"), "grid: 3 · grid: auto-fill 200 · grid: 200px 1fr - optionally with gap N and rows N");
		cols = LIT("none");
	} else if (rest.n == 1 && is_int_str(rest.v[0])) {
		cols = a_fmt(A, "repeat(%.*s, 1fr)", PF(rest.v[0]));
	} else if ((sp_eqs(rest.v[0], "auto-fill") || sp_eqs(rest.v[0], "auto-fit")) && rest.n > 1) {
		cols = a_fmt(A, "repeat(%.*s, minmax(%.*s, 1fr))", PF(rest.v[0]), PF(lenv(vc, rest.v[1])));
	} else {
		SpVec m = { 0 };
		for (int i = 0; i < rest.n; i++) vpush(A, &m, lenv(vc, rest.v[i]));
		cols = join_sp(A, &m, 0, " ");
	}
	pushl(vc, out, "display", "grid");
	push(vc, out, LIT("grid-template-columns"), cols);
	if (has_gap) push(vc, out, LIT("gap"), lenv(vc, gap));
	if (has_rows) push(vc, out, LIT("grid-template-rows"), is_int_str(rows) ? a_fmt(A, "repeat(%.*s, 1fr)", PF(rows)) : lenv(vc, rows));
}

static void sem_stack(VC *vc, sp v, DeclVec *out)
{
	Arena *A = vc->r->A;
	sp word = sp_trim(or_default(v, "center"));
	int p = place_of(word);
	if (p < 0) {
		Dym d;
		dym_init(&d, A, word);
		dym_list(&d, PLACE_NAMES, 8);
		verr_dym(vc, a_fmt(A, "unknown place \"%.*s\" in stack", PF(word)), &d);
		pushl(vc, out, "@stack", "1");
		return;
	}
	pushl(vc, out, "display", "grid");
	push(vc, out, LIT("place-items"), a_fmt(A, "%s %s", PLACE[p].cross, PLACE[p].main));
	pushl(vc, out, "@stack", "1");
}

static void sem_place(VC *vc, sp v, DeclVec *out)
{
	Arena *A = vc->r->A;
	int p = place_of(sp_trim(v));
	if (p < 0) {
		Dym d;
		dym_init(&d, A, v);
		dym_list(&d, PLACE_NAMES, 8);
		verr_dym(vc, a_fmt(A, "unknown place \"%.*s\"", PF(v)), &d);
		return;
	}
	sp both = a_fmt(A, "%s %s", PLACE[p].cross, PLACE[p].main);
	push(vc, out, LIT("place-items"), both);
	push(vc, out, LIT("place-content"), both);
}

static void sem_center(VC *vc, sp v, DeclVec *out)
{
	Arena *A = vc->r->A;
	sp mode = sp_lower(A, sp_trim(v));
	if (!mode.n) mode = LIT("both");
	if (sp_eqs(mode, "x")) { pushl(vc, out, "justify-items", "center"); pushl(vc, out, "justify-content", "center"); return; }
	if (sp_eqs(mode, "y")) { pushl(vc, out, "align-items", "center"); pushl(vc, out, "align-content", "center"); return; }
	if (!sp_eqs(mode, "both")) {
		static const char *const ax[] = { "both", "x", "y" };
		Dym d;
		dym_init(&d, A, mode);
		dym_list(&d, ax, 3);
		verr_dym(vc, a_fmt(A, "unknown center axis \"%.*s\"", PF(v)), &d);
		return;
	}
	pushl(vc, out, "place-items", "center center");
	pushl(vc, out, "place-content", "center center");
}

static int starts_word(sp w, const char *const *list, int n)
{
	for (int i = 0; i < n; i++) if (sp_eqs(w, list[i])) return 1;
	return 0;
}

static int sized_form(sp x)
{
	/* ^\d*\.?\d+(px|em|rem|%|pt|vw|vh|ch)$ */
	size_t i = 0;
	while (i < x.n && is_digit((unsigned char)x.p[i])) i++;
	size_t d1 = i;
	if (i < x.n && x.p[i] == '.') {
		size_t j = i + 1;
		while (j < x.n && is_digit((unsigned char)x.p[j])) j++;
		if (j == i + 1) return 0;
		i = j;
	} else if (d1 == 0) return 0;
	sp u = sub(x, i, x.n);
	static const char *const units[] = { "px", "em", "rem", "%", "pt", "vw", "vh", "ch" };
	for (int k = 0; k < 8; k++) if (sp_eqs(u, units[k])) return 1;
	return 0;
}

static void sem_font(VC *vc, sp v, DeclVec *out)
{
	Arena *A = vc->r->A;
	SpVec slash = { 0 }, head = { 0 };
	split_char(A, v, '/', &slash);
	split_ws(A, sp_trim(slash.v[0]), 0, &head);
	int css_like = 0;
	for (size_t i = 0; i < v.n; i++) if (v.p[i] == '"' || v.p[i] == '\'' || v.p[i] == ',') { css_like = 1; break; }
	static const char *const first_words[] = { "italic", "oblique", "bold", "bolder", "lighter", "normal", "small-caps", "condensed", "expanded", "caption", "icon", "menu", "message-box", "small-caption", "status-bar", "system-ui", "serif", "sans-serif", "monospace" };
	if (!css_like && head.n && starts_word(head.v[0], first_words, 19)) css_like = 1;
	if (!css_like && head.n > 1 && head.v[0].n == 3 && head.v[0].p[0] >= '1' && head.v[0].p[0] <= '9' && head.v[0].p[1] == '0' && head.v[0].p[2] == '0' && sized_form(head.v[1])) css_like = 1;
	if (css_like) {
		verr(vc, a_fmt(A, "font: \"%.*s\" looks like the CSS font shorthand, but DSSL's font: means size, line height and colour", PF(sp_trim(v))),
		     "font: 16/1.5 color.ink  sets size, line-height and colour. Weight and family are their own lines: font-weight: 700  and  font-family: \"Fraunces\", serif");
		return;
	}
	push(vc, out, LIT("font-size"), lenv(vc, head.n ? head.v[0] : LIT("16")));
	if (slash.n > 1 && slash.v[1].n) {
		SpVec tail = { 0 };
		split_ws(A, sp_trim(slash.v[1]), 0, &tail);
		push(vc, out, LIT("line-height"), numv(vc, tail.n ? tail.v[0] : LIT("1.5")));
		if (tail.n > 1) push(vc, out, LIT("color"), resolve_value(vc, join_sp(A, &tail, 1, " ")));
	} else if (head.n > 1) {
		push(vc, out, LIT("color"), resolve_value(vc, join_sp(A, &head, 1, " ")));
	}
}

/* ---------------------------------------------------------------- fluid (3.7) */

static int fluid_size(VC *vc, int present, sp s, double *n, sp *unit)
{
	Arena *A = vc->r->A;
	sp text = present ? s : LIT("undefined");
	/* ^(-?[0-9.]+)\s*(px|rem|em|%)?$ */
	size_t i = 0;
	int ok = 0;
	sp numtxt = mk(NULL, 0);
	if (present) {
		if (i < s.n && s.p[i] == '-') i++;
		size_t st = i;
		while (i < s.n && (is_digit((unsigned char)s.p[i]) || s.p[i] == '.')) i++;
		if (i > st) {
			numtxt = sub(s, 0, i);
			while (i < s.n && is_ws((unsigned char)s.p[i])) i++;
			sp u = sub(s, i, s.n);
			if (!u.n) { ok = 1; *unit = LIT("px"); }
			else if (sp_eqs(u, "px") || sp_eqs(u, "rem") || sp_eqs(u, "em") || sp_eqs(u, "%")) { ok = 1; *unit = u; }
		}
	}
	if (!ok) {
		ERRF(vc, "fluid takes two sizes and optionally the viewport bounds, like: fluid 16 24, or fluid 16 24 360 1200", "fluid: \"%.*s\" is not a size", PF(text));
		return 0;
	}
	if (sp_eqs(*unit, "%")) {
		ERRF(vc, "fluid takes two sizes and optionally the viewport bounds, like: fluid 16 24, or fluid 16 24 360 1200", "fluid: \"%.*s\" cannot be a percentage, use px, rem or em", PF(text));
		return 0;
	}
	*n = js_parse_float(numtxt);
	(void)A;
	return 1;
}

static void fluid_parts(VC *vc, const SpVec *parts, sp prop, DeclVec *out)
{
	Arena *A = vc->r->A;
	static const char *const hint = "fluid takes two sizes and optionally the viewport bounds, like: fluid 16 24, or fluid 16 24 360 1200";
	double an = 0, bn = 0, v0n = 0, v1n = 0;
	sp au = mk(NULL, 0), bu = mk(NULL, 0), v0u = mk(NULL, 0), v1u = mk(NULL, 0);
#define PART(i) ((i) < parts->n)
	int ok_a = fluid_size(vc, PART(0), PART(0) ? parts->v[0] : mk(NULL, 0), &an, &au);
	int ok_b = fluid_size(vc, PART(1), PART(1) ? parts->v[1] : mk(NULL, 0), &bn, &bu);
	int ok_0 = fluid_size(vc, 1, PART(2) ? parts->v[2] : LIT("360"), &v0n, &v0u);
	int ok_1 = fluid_size(vc, 1, PART(3) ? parts->v[3] : LIT("1200"), &v1n, &v1u);
	if (!ok_a || !ok_b || !ok_0 || !ok_1) { push(vc, out, prop, LIT("0px")); return; }
	sp p0 = PART(0) ? parts->v[0] : LIT("undefined"), p1 = PART(1) ? parts->v[1] : LIT("undefined");
	sp p2 = PART(2) ? parts->v[2] : LIT("undefined"), p3 = PART(3) ? parts->v[3] : LIT("undefined");
	if (!sp_eq(au, bu)) {
		ERRF(vc, hint, "fluid: \"%.*s %.*s\" mixes units, give both sizes the same unit", PF(p0), PF(p1));
		push(vc, out, prop, LIT("0px"));
		return;
	}
	if (!sp_eqs(v0u, "px") || !sp_eqs(v1u, "px")) {
		ERRF(vc, hint, "fluid: \"%.*s %.*s\" viewport bounds are px", PF(p2), PF(p3));
		push(vc, out, prop, LIT("0px"));
		return;
	}
	if (v1n <= v0n) {
		ERRF(vc, hint, "fluid: \"%.*s %.*s\" needs the larger viewport second", PF(p2), PF(p3));
		push(vc, out, prop, LIT("0px"));
		return;
	}
	double f = sp_eqs(au, "px") ? 1 : 16;
	double slope = ((bn - an) * f) / (v1n - v0n);
	double vw = slope * 100;
	double intercept = an - (slope * v0n) / f;
	double lo = an < bn ? an : bn, hi = an < bn ? bn : an;
	SB b;
	sb_init(&b, A);
	sb_puts(&b, "clamp(");
	sb_putsp(&b, js_tofixed3(A, lo)); sb_putsp(&b, au);
	sb_puts(&b, ", calc(");
	sb_putsp(&b, js_tofixed3(A, intercept)); sb_putsp(&b, au);
	sb_puts(&b, " + ");
	sb_putsp(&b, js_tofixed3(A, vw));
	sb_puts(&b, "vw), ");
	sb_putsp(&b, js_tofixed3(A, hi)); sb_putsp(&b, au);
	sb_putc(&b, ')');
	push(vc, out, prop, sb_done(&b));
}

static void sem_fluid(VC *vc, sp v, DeclVec *out)
{
	Arena *A = vc->r->A;
	SpVec raw = { 0 }, parts = { 0 };
	split_char(A, v, ',', &raw);
	for (int i = 0; i < raw.n; i++) { sp x = sp_trim(raw.v[i]); if (x.n) vpush(A, &parts, x); }
	fluid_parts(vc, &parts, LIT("font-size"), out);
}

/* ---------------------------------------------------------------- shadow, radius, ... */

static void sem_shadow(VC *vc, sp v, DeclVec *out)
{
	Arena *A = vc->r->A;
	sp t = sp_trim(v);
	if (sp_eqs(t, "sm") || sp_eqs(t, "md") || sp_eqs(t, "lg") || sp_eqs(t, "xl") || sp_eqs(t, "none")) {
		push(vc, out, LIT("box-shadow"), sp_eqs(t, "none") ? LIT("none") : a_fmt(A, "var(--dssl-shadow-%.*s)", PF(t)));
		return;
	}
	if (is_ident_plain(t) && tm_find(&vc->r->tokens, a_cat2(A, LIT("shadow."), t))) {
		push(vc, out, LIT("box-shadow"), a_fmt(A, "var(--dssl-shadow-%.*s)", PF(t)));
		return;
	}
	push(vc, out, LIT("box-shadow"), resolve_value(vc, v));
}

static void sem_radius(VC *vc, sp v, DeclVec *out)
{
	Arena *A = vc->r->A;
	SpVec atoms = { 0 }, res = { 0 };
	split_ws(A, sp_trim(v), 0, &atoms);
	for (int i = 0; i < atoms.n; i++) {
		sp a = atoms.v[i];
		if (sp_eqs(a, "sm") || sp_eqs(a, "md") || sp_eqs(a, "lg") || sp_eqs(a, "full") || sp_eqs(a, "xl")) {
			if (!tm_find(&vc->r->tokens, a_cat2(A, LIT("radius."), a)))
				vwarn(vc, a_fmt(A, "radius \"%.*s\" is not defined, so it will not apply", PF(a)), a_fmt(A, "add it with  tokens { radius { %.*s: 32 } }", PF(a)).p);
			vpush(A, &res, a_fmt(A, "var(--dssl-radius-%.*s)", PF(a)));
			continue;
		}
		if (is_ident_plain(a) && tm_find(&vc->r->tokens, a_cat2(A, LIT("radius."), a))) { vpush(A, &res, a_fmt(A, "var(--dssl-radius-%.*s)", PF(a))); continue; }
		vpush(A, &res, resolve_value(vc, a));
	}
	push(vc, out, LIT("border-radius"), join_sp(A, &res, 0, " "));
}

static void sem_size(VC *vc, sp v, DeclVec *out)
{
	SpVec parts = { 0 };
	split_ws(vc->r->A, v, 0, &parts);
	sp w = lenv(vc, parts.n ? parts.v[0] : LIT("auto"));
	sp h = parts.n > 1 ? lenv(vc, parts.v[1]) : w;
	push(vc, out, LIT("width"), w);
	push(vc, out, LIT("height"), h);
}

static void sem_circle(VC *vc, sp v, DeclVec *out)
{
	sp s = lenv(vc, v);
	push(vc, out, LIT("width"), s);
	push(vc, out, LIT("height"), s);
	pushl(vc, out, "border-radius", "50%");
}

static void sem_align(VC *vc, sp v, DeclVec *out)
{
	static const char *const map[] = { "center", "start", "end", "stretch", "baseline" };
	sp t = sp_trim(v);
	for (int i = 0; i < 5; i++) if (sp_eqs(t, map[i])) { push(vc, out, LIT("align-items"), t); return; }
	Dym d;
	dym_init(&d, vc->r->A, v);
	dym_list(&d, map, 5);
	verr_dym(vc, a_fmt(vc->r->A, "unknown align \"%.*s\"", PF(v)), &d);
}

/* ---------------------------------------------------------------- transition (6.5) */

static int deform_name(sp raw)
{
	return sp_eqs(raw, "bend") || sp_eqs(raw, "bend-y") || sp_eqs(raw, "bend-up") || sp_eqs(raw, "fold") || sp_eqs(raw, "stretch");
}

static void sem_transition(VC *vc, sp v, DeclVec *out)
{
	Arena *A = vc->r->A;
	sp t = sp_trim(v);
	if (sp_eqs(t, "none")) { pushl(vc, out, "transition", "none"); return; }
	SpVec parts = { 0 }, res = { 0 };
	split_trim(A, t, ',', &parts);
	for (int pi = 0; pi < parts.n; pi++) {
		sp part = parts.v[pi];
		SpVec atoms = { 0 };
		split_ws(A, part, 1, &atoms);
		sp dur = atoms.v[0];
		if (!dur_form2(dur)) {
			sp time_atom = mk(NULL, 0), ease_atom = mk(NULL, 0), prop_atom = mk(NULL, 0);
			int has_time = 0, has_ease = 0, has_prop = 0;
			for (int i = 0; i < atoms.n; i++) if (dur_form2(atoms.v[i])) { time_atom = atoms.v[i]; has_time = 1; break; }
			for (int i = 0; i < atoms.n; i++) if (easing_of(atoms.v[i])) { ease_atom = atoms.v[i]; has_ease = 1; break; }
			for (int i = 0; i < atoms.n; i++) {
				sp x = atoms.v[i];
				if ((has_time && sp_eq(x, time_atom)) || (has_ease && sp_eq(x, ease_atom))) continue;
				if (vset_has(&V_PROP, x.p, x.n) || deform_name(x)) { prop_atom = x; has_prop = 1; break; }
			}
			if (has_time) {
				sp ms_out;
				if (is_int_str(time_atom)) ms_out = time_atom;
				else if (sp_ends(time_atom, "ms")) ms_out = sub(time_atom, 0, time_atom.n - 2);
				else { SB b; sb_init(&b, A); sb_num(&b, js_parse_float(time_atom) * 1000); ms_out = sb_done(&b); }
				SB hb;
				sb_init(&hb, A);
				sb_puts(&hb, "write  transition: ");
				sb_putsp(&hb, ms_out);
				if (has_ease) { sb_putc(&hb, ' '); sb_putsp(&hb, ease_atom); }
				if (has_prop) { sb_putc(&hb, ' '); sb_putsp(&hb, prop_atom); }
				sb_puts(&hb, "  - time first, easing, property last");
				verr(vc, S(vc, "transition: the CSS form (property first) is not DSSL"), sb_done(&hb).p);
				continue;
			}
			verr(vc, a_fmt(A, "transition duration \"%.*s\" is not a time", PF(dur)), "order is duration · easing · property - try 200 or 0.2s");
			continue;
		}
		sp ms = is_int_str(dur) ? a_cat2(A, dur, LIT("ms")) : dur;
		int k = 1;
		sp easing = LIT("ease");
		if (k < atoms.n && easing_of(atoms.v[k])) { easing = a_dups(A, easing_of(atoms.v[k])); k++; }
		else if (k < atoms.n && (sp_starts(atoms.v[k], "cubic-bezier(") || sp_starts(atoms.v[k], "steps("))) {
			VC v2 = *vc; v2.unitless = 1;
			easing = resolve_value(&v2, atoms.v[k]);
			k++;
		}
		sp prop = LIT("all");
		if (atoms.n > k) {
			sp raw = sp_lower(A, join_sp(A, &atoms, k, " "));
			sp p = deform_name(raw) ? LIT("transform") : raw;
			if (!sp_eqs(p, "all") && !vset_has(&V_PROP, p.p, p.n)) {
				Dym d;
				dym_init(&d, A, raw);
				dym_set(&d, &V_PROP);
				dym_list(&d, (const char *const[]){ "bend", "bend-y", "bend-up", "fold", "stretch" }, 5);
				verr_dym(vc, a_fmt(A, "unknown transition property \"%.*s\"", PF(raw)), &d);
				continue;
			}
			prop = p;
		}
		vpush(A, &res, a_fmt(A, "%.*s %.*s %.*s", PF(prop), PF(ms), PF(easing)));
	}
	if (!res.n) return;
	push(vc, out, LIT("transition"), join_sp(A, &res, 0, ", "));
}

/* ---------------------------------------------------------------- animate, stagger, overlap (chapter 7) */

static void animate_one(VC *vc, sp t, DeclVec *out)
{
	Arena *A = vc->r->A;
	SpVec atoms = { 0 };
	split_ws(A, t, 1, &atoms);
	Anim *sp_ = a_zalloc(A, sizeof *sp_);
	sp_->motion = atoms.v[0];
	sp_->duration = 400;
	sp_->easing = a_dups(A, EASING_CSS[1]);
	sp_->fill = LIT("both");
	sp_->delay = 0;
	for (int k = 1; k < atoms.n; k++) {
		sp a = atoms.v[k];
		if (sp_eqs(a, "delay") || sp_eqs(a, "jitter") || sp_eqs(a, "iterations")) {
			int have = k + 1 < atoms.n;
			if (!have || !dur_form(atoms.v[k + 1])) {
				verr(vc, a_fmt(A, "%.*s needs a number", PF(a)),
				     sp_eqs(a, "iterations") ? "animate: boing 300 iterations 3" : a_fmt(A, "animate: bob 1200 infinite %.*s 600", PF(a)).p);
				continue;
			}
			sp val = atoms.v[k + 1];
			if (sp_eqs(a, "iterations")) { sp_->has_iter = 1; sp_->iter = js_parse_float(val); }
			else if (sp_eqs(a, "jitter")) { sp_->has_jitter = 1; sp_->jitter = to_ms(val); }
			else sp_->delay = to_ms(val);
			k++;
			continue;
		}
		if (sp_eqs(a, "essential")) { sp_->essential_here = 1; continue; }
		{
			/* ^\d+(ms|s)?$ */
			size_t i = 0;
			while (i < a.n && is_digit((unsigned char)a.p[i])) i++;
			int ok = i > 0;
			if (ok) { sp u = sub(a, i, a.n); ok = !u.n || sp_eqs(u, "ms") || sp_eqs(u, "s"); }
			if (ok) {
				double ms = to_ms(a);
				if (k == 1) sp_->duration = ms; else sp_->delay = ms;
				continue;
			}
		}
		if (easing_of(a)) { sp_->easing = a_dups(A, easing_of(a)); continue; }
		if (sp_eqs(a, "both") || sp_eqs(a, "forwards") || sp_eqs(a, "backwards") || sp_eqs(a, "none") || sp_eqs(a, "auto")) { sp_->fill = a; continue; }
		if (sp_eqs(a, "infinite")) { sp_->has_iter = 1; sp_->iter = INFINITY; continue; }
		if (sp_eqs(a, "alternate") || sp_eqs(a, "alternate-reverse") || sp_eqs(a, "reverse") || sp_eqs(a, "normal")) { sp_->direction = a; sp_->has_direction = 1; continue; }
		{
			static const char *const extra[] = { "both", "forwards", "backwards", "infinite", "alternate", "delay", "jitter", "iterations", "essential" };
			Dym d;
			dym_init(&d, A, a);
			dym_list(&d, EASING_NAMES, 8);
			dym_list(&d, extra, 9);
			verr_dym(vc, a_fmt(A, "unknown animate part \"%.*s\"", PF(a)), &d);
		}
	}
	Decl *d = push_kind(vc, out, "@animate", DV_ANIM);
	d->anim = sp_;
}

static void sem_animate(VC *vc, sp v, DeclVec *out)
{
	Arena *A = vc->r->A;
	sp t = sp_trim(v);
	if (sp_eqs(t, "none")) { push_kind(vc, out, "@animate", DV_NULL); return; }
	SpVec parts = { 0 };
	split_trim(A, t, ',', &parts);
	if (parts.n > 1) {
		for (int i = 0; i < parts.n; i++) animate_one(vc, parts.v[i], out);
		return;
	}
	animate_one(vc, t, out);
}

static void sem_stagger(VC *vc, sp v, DeclVec *out)
{
	sp t = sp_trim(v);
	if (sp_eqs(t, "none")) { push_kind(vc, out, "@stagger", DV_NULL); return; }
	if (!dur_form(t)) {
		verr(vc, S(vc, "stagger needs a duration like \"60\" or \"120ms\", or \"none\""), "stagger offsets each child's animation delay by its position");
		return;
	}
	Decl *d = push_kind(vc, out, "@stagger", DV_NUM);
	d->num = to_ms(t);
}

static void sem_overlap(VC *vc, sp v, DeclVec *out)
{
	Arena *A = vc->r->A;
	sp t = sp_trim(v);
	sp lo = sp_lower(A, t);
	if (sp_eqs(lo, "allow") || sp_eqs(lo, "none")) { pushl(vc, out, "@overlap", "allow"); return; }
	if (sp_eqs(lo, "deny")) { pushl(vc, out, "@overlap", "deny"); return; }
	if (sp_eqs(lo, "auto")) { pushl(vc, out, "@overlap", "auto"); return; }
	verr(vc, a_fmt(A, "unknown overlap policy \"%.*s\"", PF(t)), "overlap is allow (default) · deny (always yield out of overlaps) · auto (yield only while below 100% opacity)");
}

/* ---------------------------------------------------------------- deform (8.5) */

static int deg_num(VC *vc, sp v, const char *what, double *out)
{
	Arena *A = vc->r->A;
	sp t = sp_trim(v);
	sp body = t;
	if (t.n >= 3 && sp_ieq(sub(t, t.n - 3, t.n), "deg")) {
		sp b2 = sub(t, 0, t.n - 3);
		if (num_form(b2)) body = b2;
	}
	if (!num_form(body)) {
		verr(vc, a_fmt(A, "%s takes a bare number of degrees", what), a_fmt(A, "try  %s: 20  or  %s: -12  - bare numbers are degrees here", what, what).p);
		return 0;
	}
	*out = js_parse_float(body);
	return 1;
}

static void sem_deform(VC *vc, sp v, DeclVec *out, const char *name, const char *at)
{
	double n;
	if (!deg_num(vc, v, name, &n)) return;
	Decl *d = push_kind(vc, out, at, DV_NUM);
	d->num = n;
}

static void sem_bend_around(VC *vc, sp v, DeclVec *out)
{
	double n;
	if (!deg_num(vc, v, "bend-around", &n)) return;
	if (fabs(n) > 170) {
		verr(vc, S(vc, "bend-around caps at \xC2\xB1" "170\xC2\xB0 - past that the arc folds back on itself"), "try 30..120 for a visible arc");
		return;
	}
	Decl *d = push_kind(vc, out, "@bend-around", DV_NUM);
	d->num = n;
}

static void sem_stretch(VC *vc, sp v, DeclVec *out)
{
	Arena *A = vc->r->A;
	sp t = sp_trim(v);
	/* ^(\d*\.?\d+)(?:\s+(horizontal|vertical|x|y))?$ , case-insensitive */
	size_t i = 0;
	while (i < t.n && is_digit((unsigned char)t.p[i])) i++;
	size_t d1 = i;
	int ok = 0;
	size_t numend = 0;
	if (i < t.n && t.p[i] == '.') {
		size_t j = i + 1;
		while (j < t.n && is_digit((unsigned char)t.p[j])) j++;
		if (j > i + 1) { numend = j; ok = 1; }
	} else if (d1 > 0) { numend = d1; ok = 1; }
	sp axis = mk("", 0);
	int has_axis = 0;
	if (ok) {
		sp restt = sub(t, numend, t.n);
		if (restt.n) {
			if (!is_ws((unsigned char)restt.p[0])) ok = 0;
			else {
				sp w = sp_ltrim(restt);
				if (sp_ieq(w, "horizontal") || sp_ieq(w, "vertical") || sp_ieq(w, "x") || sp_ieq(w, "y")) { axis = w; has_axis = 1; }
				else ok = 0;
			}
		}
	}
	if (!ok) {
		verr(vc, S(vc, "stretch needs a positive factor, like \"stretch: 1.2\" or \"stretch: 1.4 vertical\""), "1 is neutral · the other axis shrinks so volume stays constant");
		return;
	}
	sp numtxt = sub(t, 0, numend);
	double f = js_parse_float(numtxt);
	if (f <= 0) {
		verr(vc, a_fmt(A, "stretch: %.*s would flatten the element to nothing", PF(numtxt)), "use a factor above 0 - 1.2 is a gentle pull, 2 is cartoonish");
		return;
	}
	sp ax = has_axis ? sp_lower(A, axis) : LIT("horizontal");
	if (sp_eqs(ax, "y")) ax = LIT("vertical"); else if (sp_eqs(ax, "x")) ax = LIT("horizontal");
	Decl *d = push_kind(vc, out, "@stretch", DV_STRETCH);
	d->st.factor = f;
	d->st.axis = ax;
}

/* ---------------------------------------------------------------- text-stroke, outline-shape, rotate, yield */

static int stroke_width_form(sp w, int allow_units)
{
	/* ^\d+(\.\d+)?(px|em|rem)?$  (allow_units 2) or ^\d+(\.\d+)?(px)?$ (1) */
	size_t i = 0;
	while (i < w.n && is_digit((unsigned char)w.p[i])) i++;
	if (!i) return 0;
	if (i < w.n && w.p[i] == '.') {
		size_t j = i + 1;
		while (j < w.n && is_digit((unsigned char)w.p[j])) j++;
		if (j == i + 1) return 0;
		i = j;
	}
	sp u = sub(w, i, w.n);
	if (!u.n || sp_eqs(u, "px")) return 1;
	if (allow_units == 2 && (sp_eqs(u, "em") || sp_eqs(u, "rem"))) return 1;
	return 0;
}

static void sem_text_stroke(VC *vc, sp v, DeclVec *out)
{
	Arena *A = vc->r->A;
	SpVec atoms = { 0 }, rest = { 0 };
	split_ws(A, sp_trim(v), 0, &atoms);
	if (atoms.n == 1 && sp_eqs(atoms.v[0], "none")) { pushl(vc, out, "-webkit-text-stroke", "0"); pushl(vc, out, "paint-order", "normal"); return; }
	int inside = 0;
	for (int i = 0; i < atoms.n; i++) {
		if (sp_eqs(atoms.v[i], "inside")) inside = 1; else vpush(A, &rest, atoms.v[i]);
	}
	if (!rest.n || !stroke_width_form(rest.v[0], 2)) {
		verr(vc, S(vc, "text-stroke needs a width first"), "text-stroke: 6 color.ink");
		return;
	}
	sp w = rest.v[0];
	int bare = 1;
	for (size_t i = 0; i < w.n; i++) if (!(is_digit((unsigned char)w.p[i]) || w.p[i] == '.')) bare = 0;
	sp width = bare ? a_cat2(A, w, LIT("px")) : w;
	sp color = rest.n > 1 ? plainv(vc, join_sp(A, &rest, 1, " ")) : LIT("currentcolor");
	push(vc, out, LIT("-webkit-text-stroke"), a_fmt(A, "%.*s %.*s", PF(width), PF(color)));
	if (!inside) pushl(vc, out, "paint-order", "stroke fill");
}

static void sem_outline_shape(VC *vc, sp v, DeclVec *out)
{
	Arena *A = vc->r->A;
	SpVec atoms = { 0 };
	split_ws(A, sp_trim(v), 0, &atoms);
	if (atoms.n == 1 && sp_eqs(atoms.v[0], "none")) { pushl(vc, out, "filter", "none"); return; }
	if (!atoms.n || !stroke_width_form(atoms.v[0], 1)) {
		verr(vc, S(vc, "outline-shape needs a width first"), "outline-shape: 3 color.ink");
		return;
	}
	double w = js_parse_float(atoms.v[0]);
	sp color = atoms.n > 1 ? plainv(vc, join_sp(A, &atoms, 1, " ")) : LIT("currentcolor");
	SB b;
	sb_init(&b, A);
	double xs[4] = { w, -w, 0, 0 }, ys[4] = { 0, 0, w, -w };
	for (int i = 0; i < 4; i++) {
		if (i) sb_putc(&b, ' ');
		sb_puts(&b, "drop-shadow(");
		sb_num(&b, xs[i]); sb_puts(&b, "px ");
		sb_num(&b, ys[i]); sb_puts(&b, "px 0 ");
		sb_putsp(&b, color);
		sb_putc(&b, ')');
	}
	push(vc, out, LIT("filter"), sb_done(&b));
}

static void sem_rotate(VC *vc, sp v, DeclVec *out)
{
	Arena *A = vc->r->A;
	sp t = sp_trim(v);
	SpVec tok = { 0 };
	split_ws(A, t, 0, &tok);
	int deg = 0;
	if (num_form(t)) deg = 1;
	else if (tok.n == 2 && tok.v[0].n == 1 && (tok.v[0].p[0] == 'x' || tok.v[0].p[0] == 'y' || tok.v[0].p[0] == 'z') && num_form(tok.v[1])) deg = 1;
	else if (tok.n == 4 && num_form(tok.v[0]) && num_form(tok.v[1]) && num_form(tok.v[2]) && num_form(tok.v[3])) deg = 1;
	push(vc, out, LIT("rotate"), deg ? a_cat2(A, t, LIT("deg")) : numv(vc, t));
}

static void sem_yield(VC *vc, sp v, DeclVec *out)
{
	Arena *A = vc->r->A;
	sp t = sp_trim(v);
	if (sp_eqs(t, "none")) { push_kind(vc, out, "@yield", DV_NULL); return; }
	SpVec names = { 0 };
	size_t i = 0;
	while (i < t.n) {
		while (i < t.n && (is_ws((unsigned char)t.p[i]) || t.p[i] == ',')) i++;
		size_t st = i;
		while (i < t.n && !(is_ws((unsigned char)t.p[i]) || t.p[i] == ',')) i++;
		if (i > st) vpush(A, &names, sp_lower(A, mk(t.p + st, i - st)));
	}
	if (!names.n) { verr(vc, S(vc, "yield needs the properties to hand back"), "yield: padding-top padding-bottom"); return; }
	for (int k = 0; k < names.n; k++) {
		sp n = names.v[k];
		if (!vset_has(&V_PROP, n.p, n.n) && !vset_has(&V_SEMANTIC, n.p, n.n)) {
			Dym d;
			dym_init(&d, A, n);
			dym_set(&d, &V_PROP);
			verr_dym(vc, a_fmt(A, "yield: \"%.*s\" is not a CSS property", PF(n)), &d);
			return;
		}
	}
	Decl *d = push_kind(vc, out, "@yield", DV_NAMES);
	d->names.v = names.v;
	d->names.n = names.n;
}

static void sem_gradient_flow(VC *vc, sp v, DeclVec *out)
{
	Arena *A = vc->r->A;
	sp t = sp_trim(v);
	if (!t.n || sp_eqs(t, "none")) { push_kind(vc, out, "@gradient-flow", DV_NULL); return; }
	SpVec atoms = { 0 };
	split_ws(A, t, 1, &atoms);
	sp dur = atoms.v[0];
	if (!dur_form(dur)) {
		verr(vc, S(vc, "gradient-flow takes a duration like \"6s\" or \"6000ms\", or \"none\""), "gradient-flow loops the element's gradient sideways forever");
		return;
	}
	GFlow gf;
	gf.duration = to_ms(dur);
	gf.easing = LIT("linear");
	gf.direction = LIT("normal");
	for (int k = 1; k < atoms.n; k++) {
		sp a = atoms.v[k];
		if (easing_of(a)) { gf.easing = a_dups(A, easing_of(a)); continue; }
		if (sp_eqs(a, "alternate") || sp_eqs(a, "alternate-reverse")) { gf.direction = a; continue; }
		Dym d;
		dym_init(&d, A, a);
		dym_list(&d, EASING_NAMES, 8);
		dym_list(&d, (const char *const[]){ "alternate", "alternate-reverse" }, 2);
		verr_dym(vc, a_fmt(A, "unknown gradient-flow part \"%.*s\"", PF(a)), &d);
	}
	Decl *d = push_kind(vc, out, "@gradient-flow", DV_GFLOW);
	d->gf = gf;
}

/* ---------------------------------------------------------------- glass and tilt (8.4) */

static int split_words_paren(Arena *a, sp s, SpVec *out)
{
	/* texture.js splitWords: white space outside parentheses */
	s = sp_trim(s);
	int depth = 0;
	size_t st = 0;
	int in_word = 0;
	for (size_t i = 0; i < s.n; i++) {
		char ch = s.p[i];
		if (ch == '(') depth++;
		if (ch == ')') depth--;
		if (is_ws((unsigned char)ch) && depth == 0) {
			if (in_word) { vpush(a, out, mk(s.p + st, i - st)); in_word = 0; }
			continue;
		}
		if (!in_word) { st = i; in_word = 1; }
	}
	if (in_word) vpush(a, out, mk(s.p + st, s.n - st));
	return out->n;
}

static int is_num_word(sp w, double *v)
{
	if (!w.n) return 0;
	return js_number(w, v);
}

static int looks_like_color(sp w)
{
	if (w.n && (w.p[0] == '#' || sp_starts(w, "rgb") || sp_starts(w, "hsl") || sp_starts(w, "oklch") || sp_starts(w, "oklab") || sp_starts(w, "color"))) return 1;
	if (sp_findc(w, '/') >= 0) return 1;
	size_t i = 0;
	while (i < w.n && w.p[i] >= 'a' && w.p[i] <= 'z') i++;
	if (i > 0 && i < w.n && w.p[i] == '.' && i + 1 < w.n && ((w.p[i + 1] >= 'a' && w.p[i + 1] <= 'z') || is_digit((unsigned char)w.p[i + 1]) || w.p[i + 1] == '-')) return 1;
	return sp_eqs(w, "white") || sp_eqs(w, "black") || sp_eqs(w, "transparent");
}

static const char *const GLASS_BLUR = "a number: how many px to blur what is behind the element (default 16)";
static const char *const GLASS_SAT = "saturate <n>: colour boost for what shows through, 1 is unchanged (default 1.5)";
static const char *const GLASS_BRIGHT = "bright <n>: brightness of what shows through, 1 is unchanged";
static const char *const GLASS_TINT = "tint <color> | tint none: the milky layer on top (default: a faint wash of the text colour)";

/* the hook the engine gives glass and tilt: resolve a colour word, null when it was an error */
static int hook_color(VC *vc, sp word, sp *css)
{
	DiagVec *errs = vc->errors;
	int before = errs->n;
	*css = plainv(vc, word);
	return errs->n == before;
}

static void sem_glass(VC *vc, sp v, DeclVec *out)
{
	Arena *A = vc->r->A;
	SpVec w = { 0 };
	split_words_paren(A, v, &w);
	if (w.n && sp_eqs(w.v[0], "none")) { pushl(vc, out, "backdrop-filter", "none"); return; }
	double blur = 16, sat = 1.5, bright = 0;
	int has_bright = 0;
	sp tint = mk(NULL, 0);
	int tint_state = 0;   /* 0 undefined, 1 null, 2 set */
	for (int i = 0; i < w.n; i++) {
		sp x = w.v[i];
		double nv;
		if (is_num_word(x, &nv)) { blur = nv; continue; }
		if (sp_eqs(x, "saturate") || sp_eqs(x, "bright")) {
			if (i + 1 >= w.n || !is_num_word(w.v[i + 1], &nv)) { verr(vc, a_fmt(A, "%.*s needs a number", PF(x)), sp_eqs(x, "saturate") ? GLASS_SAT : GLASS_BRIGHT); return; }
			if (sp_eqs(x, "saturate")) sat = nv; else { bright = nv; has_bright = 1; }
			i++;
			continue;
		}
		if (sp_eqs(x, "tint")) {
			if (i + 1 >= w.n) { verr(vc, S(vc, "tint needs a color or \"none\""), GLASS_TINT); return; }
			sp c = w.v[i + 1];
			if (sp_eqs(c, "none")) tint_state = 1;
			else { sp css; if (!hook_color(vc, c, &css)) return; tint = css; tint_state = 2; }
			i++;
			continue;
		}
		if (looks_like_color(x)) { sp css; if (!hook_color(vc, x, &css)) return; tint = css; tint_state = 2; continue; }
		Dym d;
		dym_init(&d, A, x);
		dym_list(&d, (const char *const[]){ "saturate", "bright", "tint", "none" }, 4);
		sp h;
		int has = dym_done(&d, &h);
		diag_push(A, vc->errors, vc->line, vc->col, a_fmt(A, "unknown glass setting \"%.*s\"", PF(x)),
		          has ? a_fmt(A, "did you mean \"%.*s\"?", PF(h)).p : "settings: a blur number, saturate, bright, tint", mk(NULL, 0), 0);
		return;
	}
	if (blur < 0) { verr(vc, S(vc, "glass blur cannot be negative"), GLASS_BLUR); return; }
	SB b;
	sb_init(&b, A);
	sb_puts(&b, "blur("); sb_num(&b, blur); sb_puts(&b, "px) saturate("); sb_num(&b, sat); sb_putc(&b, ')');
	if (has_bright) { sb_puts(&b, " brightness("); sb_num(&b, bright); sb_putc(&b, ')'); }
	push(vc, out, LIT("backdrop-filter"), sb_done(&b));
	if (tint_state == 0) tint = LIT("color-mix(in srgb, currentColor 8%, transparent)");
	if (tint_state != 1) push(vc, out, LIT("background-color"), tint);
}

static void sem_tilt(VC *vc, sp v, DeclVec *out)
{
	Arena *A = vc->r->A;
	static const char *const T_MAX = "a number: the largest tilt in degrees (default 8)";
	static const char *const T_LIFT = "lift <n>: scale while the pointer is over it, e.g. 1.04";
	static const char *const T_PERSP = "perspective <px>: how strong the 3D depth looks (default 900)";
	static const char *const T_SPEED = "speed <0.05 to 1>: how quickly it follows (default 0.18)";
	SpVec w = { 0 };
	split_words_paren(A, v, &w);
	if (!w.n) { verr(vc, S(vc, "tilt needs a number of degrees"), "e.g. tilt: 10"); return; }
	if (sp_eqs(w.v[0], "none")) { push_kind(vc, out, "@tilt", DV_NULL); return; }
	Tilt t = { 8, 1, 900, 0.18, 0, 0 };
	for (int i = 0; i < w.n; i++) {
		sp x = w.v[i];
		double nv;
		if (is_num_word(x, &nv)) { t.max = nv; continue; }
		if (sp_eqs(x, "invert")) { t.invert = 1; continue; }
		if (sp_eqs(x, "glare")) { t.glare = 1; continue; }
		if (sp_eqs(x, "lift") || sp_eqs(x, "perspective") || sp_eqs(x, "speed")) {
			if (i + 1 >= w.n || !is_num_word(w.v[i + 1], &nv)) {
				verr(vc, a_fmt(A, "%.*s needs a number", PF(x)), sp_eqs(x, "lift") ? T_LIFT : sp_eqs(x, "perspective") ? T_PERSP : T_SPEED);
				return;
			}
			if (sp_eqs(x, "lift")) t.lift = nv; else if (sp_eqs(x, "perspective")) t.perspective = nv; else t.speed = nv;
			i++;
			continue;
		}
		Dym d;
		dym_init(&d, A, x);
		dym_list(&d, (const char *const[]){ "lift", "perspective", "speed", "invert", "glare", "none" }, 6);
		sp h;
		int has = dym_done(&d, &h);
		diag_push(A, vc->errors, vc->line, vc->col, a_fmt(A, "unknown tilt setting \"%.*s\"", PF(x)),
		          has ? a_fmt(A, "did you mean \"%.*s\"?", PF(h)).p : "settings: a degrees number, lift, perspective, speed, invert, glare", mk(NULL, 0), 0);
		return;
	}
	if (t.max < 0 || t.max > 45) { verr(vc, S(vc, "tilt degrees should be between 0 and 45"), T_MAX); return; }
	if (t.lift <= 0) { verr(vc, S(vc, "lift must be above 0"), T_LIFT); return; }
	if (t.perspective < 100) { verr(vc, S(vc, "perspective must be at least 100px"), T_PERSP); return; }
	if (t.speed < 0.02 || t.speed > 1) { verr(vc, S(vc, "speed must be between 0.02 and 1"), T_SPEED); return; }
	Decl *d = push_kind(vc, out, "@tilt", DV_TILT);
	d->tilt = t;
}

/* ---------------------------------------------------------------- gradient (gradient.c) */

int build_gradient_parts(VC *vc, sp v, GradParts *out);

static void sem_gradient(VC *vc, sp v, DeclVec *out)
{
	GradParts g;
	if (!build_gradient_parts(vc, v, &g)) return;
	Decl *d = push(vc, out, LIT("background-image"), g.image);
	d->from_gradient = 1;
	if (g.has_size) push(vc, out, LIT("background-size"), g.size);
	if (g.has_pos) push(vc, out, LIT("background-position"), g.pos);
	if (g.has_rep) push(vc, out, LIT("background-repeat"), g.rep);
}

static void sem_gradient_text(VC *vc, sp v, DeclVec *out)
{
	GradParts g;
	if (!build_gradient_parts(vc, v, &g)) return;
	push(vc, out, LIT("background-image"), g.image);
	pushl(vc, out, "background-clip", "text");
	pushl(vc, out, "-webkit-background-clip", "text");
	pushl(vc, out, "color", "transparent");
	pushl(vc, out, "-webkit-text-fill-color", "transparent");
}

/* ---------------------------------------------------------------- the dispatch */

typedef void (*SemFn)(VC *, sp, DeclVec *);
#define WRAP(name, body) static void name(VC *vc, sp v, DeclVec *out) { (void)v; body; }
WRAP(s_row, sem_flex(vc, v, out, "row"))
WRAP(s_column, sem_flex(vc, v, out, "column"))
WRAP(s_cover, push(vc, out, LIT("position"), sp_trim(or_default(v, "absolute"))); pushl(vc, out, "inset", "0"))
WRAP(s_sticky, pushl(vc, out, "position", "sticky"); push(vc, out, LIT("top"), lenv(vc, or_default(v, "0"))))
WRAP(s_z, push(vc, out, LIT("z-index"), numv(vc, v)))
WRAP(s_grow, push(vc, out, LIT("flex-grow"), numv(vc, or_default(v, "1"))))
WRAP(s_shrink, push(vc, out, LIT("flex-shrink"), numv(vc, or_default(v, "1"))))
WRAP(s_basis, push(vc, out, LIT("flex-basis"), lenv(vc, v)))
WRAP(s_truncate, pushl(vc, out, "overflow", "hidden"); pushl(vc, out, "text-overflow", "ellipsis"); pushl(vc, out, "white-space", "nowrap"))
WRAP(s_vhidden,
     pushl(vc, out, "position", "absolute"); pushl(vc, out, "width", "1px"); pushl(vc, out, "height", "1px");
     pushl(vc, out, "padding", "0"); pushl(vc, out, "margin", "-1px"); pushl(vc, out, "overflow", "hidden");
     pushl(vc, out, "clip-path", "inset(50%)"); pushl(vc, out, "white-space", "nowrap"); pushl(vc, out, "border", "0"))
WRAP(s_bend, sem_deform(vc, v, out, "bend", "@bend"))
WRAP(s_bend_y, sem_deform(vc, v, out, "bend-y", "@bend-y"))
WRAP(s_bend_up, sem_deform(vc, v, out, "bend-up", "@bend-up"))
WRAP(s_fold, sem_deform(vc, v, out, "fold", "@fold"))
WRAP(s_texture, push_kind(vc, out, "@texture", DV_ENGINE))
WRAP(s_slider, push_kind(vc, out, "@slider", DV_ENGINE))
WRAP(s_aura_both, push_kind(vc, out, "@aura-glow", DV_ENGINE); push_kind(vc, out, "@aura-particles", DV_ENGINE))
WRAP(s_aura_glow, push_kind(vc, out, "@aura-glow", DV_ENGINE))
WRAP(s_aura_part, push_kind(vc, out, "@aura-particles", DV_ENGINE))

static const struct { const char *name; SemFn fn; } SEM[] = {
	{ "size", sem_size }, { "row", s_row }, { "column", s_column }, { "grid", sem_grid }, { "stack", sem_stack },
	{ "place", sem_place }, { "center", sem_center }, { "cover", s_cover }, { "sticky", s_sticky }, { "font", sem_font },
	{ "fluid", sem_fluid }, { "shadow", sem_shadow }, { "radius", sem_radius }, { "circle", sem_circle }, { "align", sem_align },
	{ "z", s_z }, { "grow", s_grow }, { "shrink", s_shrink }, { "basis", s_basis }, { "truncate", s_truncate },
	{ "visually-hidden", s_vhidden }, { "transition", sem_transition }, { "animate", sem_animate }, { "stagger", sem_stagger },
	{ "overlap", sem_overlap }, { "bend", s_bend }, { "bend-y", s_bend_y }, { "bend-up", s_bend_up }, { "fold", s_fold },
	{ "bend-around", sem_bend_around }, { "stretch", sem_stretch }, { "gradient", sem_gradient }, { "text-stroke", sem_text_stroke },
	{ "yield", sem_yield }, { "outline-shape", sem_outline_shape }, { "rotate", sem_rotate }, { "gradient-text", sem_gradient_text },
	{ "gradient-flow", sem_gradient_flow }, { "texture", s_texture }, { "aura", s_aura_both }, { "aura-glow", s_aura_glow },
	{ "aura-particle", s_aura_part }, { "aura-particles", s_aura_part }, { "glow-particle", s_aura_both }, { "glow-particles", s_aura_both },
	{ "aura-glow-particle", s_aura_both }, { "aura-glow-particles", s_aura_both }, { "glass", sem_glass }, { "tilt", sem_tilt },
	{ "slider", s_slider },
};

/* a string-aware !important scan: a "!" outside quotes with the word "important" anywhere after it (as the engine's regex) */
static int has_important(sp v)
{
	char q = 0;
	for (size_t i = 0; i < v.n; i++) {
		char c = v.p[i];
		if (q) { if (c == q && v.p[i - 1] != '\\') q = 0; continue; }
		if (c == '"' || c == '\'') { q = c; continue; }
		if (c == '!') {
			sp rest = sub(v, i + 1, v.n);
			for (size_t k = 0; k + 9 <= rest.n; k++) if (sp_istarts(sub(rest, k, rest.n), "important")) return 1;
		}
	}
	return 0;
}

/* value.replace(/\s*!\s*important\s*\/i, '') */
static sp strip_important(Arena *a, sp v)
{
	for (size_t i = 0; i < v.n; i++) {
		if (v.p[i] != '!') continue;
		size_t j = i + 1;
		while (j < v.n && is_ws((unsigned char)v.p[j])) j++;
		if (j + 9 <= v.n && sp_istarts(sub(v, j, v.n), "important")) {
			size_t e = j + 9;
			while (e < v.n && is_ws((unsigned char)v.p[e])) e++;
			size_t st = i;
			while (st > 0 && is_ws((unsigned char)v.p[st - 1])) st--;
			return a_cat2(a, sub(v, 0, st), sub(v, e, v.n));
		}
	}
	return v;
}

void resolve_decl(R *r, sp prop, sp value, int line, int col, DeclVec *out)
{
	Arena *A = r->A;
	VC vc;
	memset(&vc, 0, sizeof vc);
	vc.r = r; vc.prop = prop; vc.line = line; vc.col = col; vc.errors = r->errors;

	if (has_important(value)) {
		sp shown = strip_important(A, value);
		diag_push(A, r->errors, line, col, a_fmt(A, "!important does not exist in DSSL - \"%.*s: %.*s\" was NOT applied", PF(prop), PF(shown)),
		          "the cascade is (layer, order) and later wins - put the rule in pin { } when it genuinely must win", mk(NULL, 0), 0);
		return;
	}

	if (sp_starts(prop, "--")) {
		sp outp = prop;
		if (sp_findc(prop, '.') >= 0) {
			SB b;
			sb_init(&b, A);
			sb_puts(&b, "--dssl-");
			for (size_t i = 2; i < prop.n; i++) sb_putc(&b, prop.p[i] == '.' ? '-' : prop.p[i]);
			outp = sb_done(&b);
		}
		VC v2 = vc;
		v2.unitless = 0; v2.bare = 1;
		push(&vc, out, outp, resolve_value(&v2, value));
		return;
	}

	for (size_t i = 0; i < sizeof SEM / sizeof SEM[0]; i++) {
		if (sp_eqs(prop, SEM[i].name)) { SEM[i].fn(&vc, value, out); return; }
	}

	if (sp_eqs(prop, "animation") || sp_eqs(prop, "animation-name")) {
		VC v2 = vc;
		v2.unitless = vset_has(&V_UNITLESS_PROP, prop.p, prop.n);
		v2.bare = 1;
		push(&vc, out, prop, resolve_value(&v2, value));
		return;
	}

	{
		sp t = sp_trim(value);
		if (sp_starts(t, "fluid") && t.n > 5 && is_ws((unsigned char)t.p[5]) && vset_has(&V_FLUID_PROP, prop.p, prop.n)) {
			sp rest = sp_trim(sub(t, 5, t.n));
			if (rest.n) {
				SpVec parts = { 0 };
				size_t i = 0;
				while (i < rest.n) {
					while (i < rest.n && (is_ws((unsigned char)rest.p[i]) || rest.p[i] == ',')) i++;
					size_t st = i;
					while (i < rest.n && !(is_ws((unsigned char)rest.p[i]) || rest.p[i] == ',')) i++;
					if (i > st) vpush(A, &parts, mk(rest.p + st, i - st));
				}
				fluid_parts(&vc, &parts, prop, out);
				return;
			}
		}
	}

	if (!vset_has(&V_PROP, prop.p, prop.n)) {
		Dym d;
		dym_init(&d, A, prop);
		dym_set(&d, &V_PROP);
		dym_set(&d, &V_SEMANTIC);
		verr_dym(&vc, a_fmt(A, "unknown property \"%.*s\"", PF(prop)), &d);
		return;
	}
	VC v2 = vc;
	v2.unitless = vset_has(&V_UNITLESS_PROP, prop.p, prop.n);
	v2.bare = sp_eqs(prop, "font-family") || sp_starts(prop, "counter-");
	push(&vc, out, prop, resolve_value(&v2, value));
}
