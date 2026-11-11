/* libdssl: gradient: and gradient-text: (spec 6.6, PRP-23 to PRP-33). Port of buildGradientParts()
 * and friends of resolver.js 0.16.3, with the engine's regexes written out by hand.
 * SPDX-License-Identifier: MIT */
#include "internal.h"

static void gerr(VC *vc, sp msg, const char *hint)
{
	diag_push(vc->r->A, vc->errors, vc->line, vc->col, msg, hint, mk(NULL, 0), 0);
}
static sp px_of(Arena *a, sp x) { return is_number_str(x, 1) ? a_cat2(a, x, LIT("px")) : x; }

static sp plain_value(VC *vc, sp raw)
{
	VC v = *vc;
	v.unitless = 0;
	v.bare = 0;
	return resolve_value(&v, raw);
}

/* ---------------------------------------------------------------- splitters */

/* split on a top-level " + " (white space on both sides), never inside parentheses or quotes */
static void split_layers(Arena *a, sp s, SpVec *out)
{
	int depth = 0;
	char q = 0;
	size_t st = 0;
	for (size_t i = 0; i < s.n; i++) {
		char c = s.p[i];
		if (q) { if (c == q && s.p[i - 1] != '\\') q = 0; continue; }
		if (c == '"' || c == '\'') { q = c; continue; }
		if (c == '(' || c == '[') depth++;
		else if (c == ')' || c == ']') { if (depth > 0) depth--; }
		if (depth == 0 && c == '+' && i > 0 && is_ws((unsigned char)s.p[i - 1]) && i + 1 < s.n && is_ws((unsigned char)s.p[i + 1])) {
			vpush(a, out, sp_trim(mk(s.p + st, i - st)));
			st = i + 1;
		}
	}
	sp last = sp_trim(mk(s.p + st, s.n - st));
	if (last.n) vpush(a, out, last);
}

static void split_arrow(Arena *a, sp s, SpVec *out)
{
	int depth = 0;
	char q = 0;
	size_t st = 0;
	for (size_t i = 0; i < s.n; i++) {
		char c = s.p[i];
		if (q) { if (c == q && s.p[i - 1] != '\\') q = 0; continue; }
		if (c == '"' || c == '\'') { q = c; continue; }
		if (c == '(' || c == '[') depth++;
		else if (c == ')' || c == ']') { if (depth > 0) depth--; }
		if (depth == 0 && c == '-' && i + 1 < s.n && s.p[i + 1] == '>') {
			vpush(a, out, mk(s.p + st, i - st));
			i++;
			st = i + 1;
		}
	}
	vpush(a, out, mk(s.p + st, s.n - st));
}

/* parentheses-aware split, trimmed, empties dropped */
static void split_commas(Arena *a, sp s, SpVec *out)
{
	int depth = 0;
	size_t st = 0;
	for (size_t i = 0; i < s.n; i++) {
		char ch = s.p[i];
		if (ch == '(') depth++;
		else if (ch == ')') { if (depth > 0) depth--; }
		if (ch == ',' && depth == 0) {
			sp x = sp_trim(mk(s.p + st, i - st));
			if (x.n) vpush(a, out, x);
			st = i + 1;
		}
	}
	sp x = sp_trim(mk(s.p + st, s.n - st));
	if (x.n) vpush(a, out, x);
}

/* ---------------------------------------------------------------- small regexes */

/* -?\d+(\.\d+)? from i; returns the end or i when none */
static size_t num_at(sp s, size_t i)
{
	size_t j = i;
	if (j < s.n && s.p[j] == '-') j++;
	size_t d = j;
	while (j < s.n && is_digit((unsigned char)s.p[j])) j++;
	if (j == d) return i;
	if (j + 1 < s.n && s.p[j] == '.' && is_digit((unsigned char)s.p[j + 1])) {
		j++;
		while (j < s.n && is_digit((unsigned char)s.p[j])) j++;
	}
	return j;
}

/* ^-?\d+(\.\d+)?(deg)?$ (case-insensitive when ci) ; *deg tells whether deg was there */
static int number_deg(sp s, int *deg)
{
	size_t e = num_at(s, 0);
	if (e == 0) return 0;
	sp t = sub(s, e, s.n);
	if (!t.n) { if (deg) *deg = 0; return 1; }
	if (sp_ieq(t, "deg")) { if (deg) *deg = 1; return 1; }
	return 0;
}

/* tokens of s split on white space */
static void toks(Arena *a, sp s, SpVec *out) { split_ws(a, s, 0, out); }

static int dir_form(Arena *a, sp s)
{
	/* ^to\s+(top|bottom|left|right)(\s+(top|bottom|left|right))?$ , case-insensitive */
	SpVec t = { 0 };
	toks(a, s, &t);
	if (t.n < 2 || t.n > 3 || !sp_ieq(t.v[0], "to")) return 0;
	for (int i = 1; i < t.n; i++)
		if (!(sp_ieq(t.v[i], "top") || sp_ieq(t.v[i], "bottom") || sp_ieq(t.v[i], "left") || sp_ieq(t.v[i], "right"))) return 0;
	/* s itself must not have leading or trailing white space for the anchors to hold */
	return !is_ws((unsigned char)s.p[0]) && !is_ws((unsigned char)s.p[s.n - 1]);
}

static int radial_atom(sp a, int ci)
{
	static const char *const w[] = { "circle", "ellipse", "center", "top", "bottom", "left", "right", "closest-side", "farthest-side", "closest-corner", "farthest-corner" };
	for (int i = 0; i < 11; i++) if (ci ? sp_ieq(a, w[i]) : sp_eqs(a, w[i])) return 1;
	size_t e = num_at(a, 0);
	if (e == 0) return 0;
	sp u = sub(a, e, a.n);
	return !u.n || sp_eqs(u, "%") || sp_eqs(u, "px") || sp_eqs(u, "em") || sp_eqs(u, "rem");
}

/* the colour-space tail:  \s*\bin\s+(space)(\s+(longer|shorter|increasing|decreasing)\s+hue)?\s*$  (i)
 * returns 1 and the start of the whole match (including the leading white space) and of the text from "in" */
static int interp_match(sp s, size_t *start, size_t *in_start)
{
	static const char *const spaces[] = { "srgb-linear", "srgb", "display-p3", "lab", "oklab", "xyz-d50", "xyz-d65", "xyz", "hsl", "hwb", "lch", "oklch" };
	size_t end = s.n;
	while (end > 0 && is_ws((unsigned char)s.p[end - 1])) end--;
	sp t = sub(s, 0, end);
	/* optional "<ws>+ (longer|shorter|increasing|decreasing) <ws>+ hue" */
	for (int pass = 0; pass < 2; pass++) {
		sp u = t;
		if (pass == 0) {
			if (u.n < 3 || !sp_ieq(sub(u, u.n - 3, u.n), "hue")) continue;
			size_t k = u.n - 3;
			if (!(k > 0 && is_ws((unsigned char)u.p[k - 1]))) continue;
			while (k > 0 && is_ws((unsigned char)u.p[k - 1])) k--;
			static const char *const hw[] = { "longer", "shorter", "increasing", "decreasing" };
			int found = 0;
			for (int h = 0; h < 4; h++) {
				size_t L = strlen(hw[h]);
				if (k >= L && sp_ieq(sub(u, k - L, k), hw[h]) && k - L > 0 && is_ws((unsigned char)u.p[k - L - 1])) {
					size_t kk = k - L;
					while (kk > 0 && is_ws((unsigned char)u.p[kk - 1])) kk--;
					u = sub(u, 0, kk);
					found = 1;
					break;
				}
			}
			if (!found) continue;
		}
		for (int sI = 0; sI < 12; sI++) {
			size_t L = strlen(spaces[sI]);
			if (u.n < L || !sp_ieq(sub(u, u.n - L, u.n), spaces[sI])) continue;
			size_t k = u.n - L;
			if (!(k > 0 && is_ws((unsigned char)u.p[k - 1]))) continue;
			while (k > 0 && is_ws((unsigned char)u.p[k - 1])) k--;
			if (k < 2 || !sp_ieq(sub(u, k - 2, k), "in")) continue;
			size_t in = k - 2;
			/* \b before "in": the character before is not a word character (or none) */
			size_t ws0 = in;
			while (ws0 > 0 && is_ws((unsigned char)u.p[ws0 - 1])) ws0--;
			if (ws0 == in && in > 0 && is_word((unsigned char)u.p[in - 1])) continue;
			*start = ws0;
			*in_start = in;
			return 1;
		}
	}
	return 0;
}

/* a stop position: -?\d+(\.\d+)?(%|px|em|rem|deg|turn)?  as a whole token */
static int pos_token(sp t)
{
	size_t e = num_at(t, 0);
	if (e == 0) return 0;
	sp u = sub(t, e, t.n);
	return !u.n || sp_eqs(u, "%") || sp_eqs(u, "px") || sp_eqs(u, "em") || sp_eqs(u, "rem") || sp_eqs(u, "deg") || sp_eqs(u, "turn");
}

/* \s+(kw)\s+(\S+)(?:\s+(\S+))?$ (ci) , leftmost; kw NULL means the repeat words with no argument tokens.
 * min/max count the argument tokens. Returns the match start (start of the leading white space) or -1. */
static long tail_match(sp s, const char *kw, int mintok, int maxtok, sp tk[2], int *ntok)
{
	for (size_t i = 0; i < s.n; i++) {
		if (!is_ws((unsigned char)s.p[i]) || (i > 0 && is_ws((unsigned char)s.p[i - 1]))) continue;
		size_t j = i;
		while (j < s.n && is_ws((unsigned char)s.p[j])) j++;
		if (kw) {
			size_t L = strlen(kw);
			if (!(j + L <= s.n && sp_ieq(sub(s, j, j + L), kw))) continue;
			j += L;
			int n = 0;
			size_t k = j;
			int ok = 1;
			while (n < maxtok) {
				size_t w0 = k;
				while (k < s.n && is_ws((unsigned char)s.p[k])) k++;
				if (k == w0 || k >= s.n) { ok = 0; break; }
				size_t t0 = k;
				while (k < s.n && !is_ws((unsigned char)s.p[k])) k++;
				tk[n++] = sub(s, t0, k);
				if (k >= s.n) break;
			}
			if (ok && n >= mintok && k == s.n) { *ntok = n; return (long)i; }
			continue;
		}
		static const char *const rep[] = { "no-repeat", "repeat-x", "repeat-y" };
		for (int r = 0; r < 3; r++) {
			size_t L = strlen(rep[r]);
			if (j + L == s.n && sp_ieq(sub(s, j, s.n), rep[r])) { tk[0] = sub(s, j, s.n); *ntok = 1; return (long)i; }
		}
	}
	return -1;
}

/* ---------------------------------------------------------------- mesh */

/* ^(.*?)\s+at\s+(\S+)\s+(\S+)(?:\s+size\s+(\S+))?$ (i): the smallest prefix for which the rest fits */
static int mesh_match(sp it, sp *color, sp *x, sp *y, sp *size, int *has_size)
{
	for (size_t e = 0; e <= it.n; e++) {
		if (e < it.n && !is_ws((unsigned char)it.p[e])) continue;
		if (e >= it.n) break;
		size_t j = e;
		while (j < it.n && is_ws((unsigned char)it.p[j])) j++;
		if (!(j + 2 <= it.n && sp_ieq(sub(it, j, j + 2), "at"))) continue;
		j += 2;
		size_t w = j;
		while (j < it.n && is_ws((unsigned char)it.p[j])) j++;
		if (j == w || j >= it.n) continue;
		size_t t1 = j;
		while (j < it.n && !is_ws((unsigned char)it.p[j])) j++;
		sp a1 = sub(it, t1, j);
		w = j;
		while (j < it.n && is_ws((unsigned char)it.p[j])) j++;
		if (j == w || j >= it.n) continue;
		size_t t2 = j;
		while (j < it.n && !is_ws((unsigned char)it.p[j])) j++;
		sp a2 = sub(it, t2, j);
		if (j == it.n) { *color = sub(it, 0, e); *x = a1; *y = a2; *has_size = 0; return 1; }
		/* optional  \s+size\s+(\S+) */
		size_t k = j;
		while (k < it.n && is_ws((unsigned char)it.p[k])) k++;
		if (k == j) continue;
		if (!(k + 4 <= it.n && sp_ieq(sub(it, k, k + 4), "size"))) continue;
		k += 4;
		size_t w2 = k;
		while (k < it.n && is_ws((unsigned char)it.p[k])) k++;
		if (k == w2 || k >= it.n) continue;
		size_t t3 = k;
		while (k < it.n && !is_ws((unsigned char)it.p[k])) k++;
		if (k != it.n) continue;
		*color = sub(it, 0, e); *x = a1; *y = a2; *size = sub(it, t3, k); *has_size = 1;
		return 1;
	}
	return 0;
}

static int build_mesh(VC *vc, sp rest, sp *outp)
{
	Arena *A = vc->r->A;
	SpVec items = { 0 };
	split_commas(A, rest, &items);
	if (items.n < 2) {
		gerr(vc, LIT("mesh needs a base colour and at least one blob"), "try  gradient: mesh #fff4e6, #ff7a1a at 15% 20%, #3949ab at 80% 70%");
		return 0;
	}
	sp base = mk(NULL, 0);
	int has_base = 0;
	SpVec blobs = { 0 };
	for (int i = 0; i < items.n; i++) {
		sp it = items.v[i], color, x, y, size = mk(NULL, 0);
		int has_size = 0;
		if (!mesh_match(it, &color, &x, &y, &size, &has_size)) {
			if (has_base) {
				gerr(vc, a_fmt(A, "mesh item \"%.*s\" needs a position", PF(it)), "write  color at 20% 30%  (and optionally  size 60%)");
				return 0;
			}
			base = plain_value(vc, it);
			has_base = 1;
			continue;
		}
		sp sz = has_size ? px_of(A, size) : LIT("60%");
		vpush(A, &blobs, a_fmt(A, "radial-gradient(circle farthest-side at %.*s %.*s, %.*s, transparent %.*s)", PF(px_of(A, x)), PF(px_of(A, y)), PF(plain_value(vc, sp_trim(color))), PF(sz)));
	}
	if (!has_base) {
		gerr(vc, LIT("mesh needs a base colour first (an item without \"at\")"), "gradient: mesh #fff4e6, #ff7a1a at 15% 20%");
		return 0;
	}
	SB b;
	sb_init(&b, A);
	for (int i = 0; i < blobs.n; i++) { sb_putsp(&b, blobs.v[i]); sb_puts(&b, ", "); }
	sb_fmt(&b, "linear-gradient(%.*s, %.*s)", PF(base), PF(base));
	*outp = sb_done(&b);
	return 1;
}

/* ---------------------------------------------------------------- one layer */

typedef enum { G_LINEAR, G_RADIAL, G_CONIC } GKind;

static int prefix_shaped(Arena *a, GKind kind, sp seg0)
{
	sp low = sp_lower(a, seg0);
	size_t st, in;
	sp s = low;
	int interp = interp_match(seg0, &st, &in);
	if (interp) s = sub(low, 0, st);
	s = sp_trim(s);
	if (!s.n && interp) return 1;
	if (kind == G_LINEAR) return number_deg(s, NULL) || (s.n && dir_form(a, s));
	if (kind == G_CONIC) {
		if (sp_starts(s, "from") && s.n > 4 && is_ws((unsigned char)s.p[4])) {
			size_t j = 4;
			while (j < s.n && is_ws((unsigned char)s.p[j])) j++;
			size_t e = num_at(s, j);
			if (e > j) {
				if (e + 3 <= s.n && memcmp(s.p + e, "deg", 3) == 0) e += 3;
				if (e == s.n) return 1;
				if (is_ws((unsigned char)s.p[e])) {
					size_t k = e;
					while (k < s.n && is_ws((unsigned char)s.p[k])) k++;
					if (k + 2 <= s.n && s.p[k] == 'a' && s.p[k + 1] == 't') {
						k += 2;
						size_t w = k;
						while (k < s.n && is_ws((unsigned char)s.p[k])) k++;
						if (k > w && k < s.n) return 1;
					}
				}
			}
		}
		if (sp_starts(s, "at") && s.n > 2 && is_ws((unsigned char)s.p[2])) {
			size_t k = 2;
			while (k < s.n && is_ws((unsigned char)s.p[k])) k++;
			if (k < s.n) return 1;
		}
		return 0;
	}
	SpVec t = { 0 };
	split_ws(a, s, 1, &t);
	for (int i = 0; i < t.n; i++) {
		if (sp_eqs(t.v[i], "at") || radial_atom(t.v[i], 0)) continue;
		return 0;
	}
	return 1;
}

static int build_layer(VC *vc, sp v, sp *outp)
{
	Arena *A = vc->r->A;
	sp t = sp_trim(v);
	if (sp_istarts(t, "mesh") && (t.n == 4 || !is_word((unsigned char)t.p[4]))) return build_mesh(vc, sub(t, 4, t.n), outp);

	size_t p = 0;
	int repeating = 0;
	if (sp_istarts(t, "repeating") && t.n > 9 && is_ws((unsigned char)t.p[9])) {
		size_t k = 9;
		while (k < t.n && is_ws((unsigned char)t.p[k])) k++;
		sp after = sub(t, k, t.n);
		if (sp_istarts(after, "linear") || sp_istarts(after, "radial") || sp_istarts(after, "conic")) { repeating = 1; p = k; }
	}
	GKind kind = G_LINEAR;
	int ok = 0;
	size_t klen = 6;
	sp after = sub(t, p, t.n);
	if (sp_istarts(after, "linear")) { kind = G_LINEAR; ok = 1; }
	else if (sp_istarts(after, "radial")) { kind = G_RADIAL; ok = 1; }
	else if (sp_istarts(after, "conic")) { kind = G_CONIC; ok = 1; klen = 5; }
	if (ok && !(after.n == klen || !is_word((unsigned char)after.p[klen]))) ok = 0;
	if (!ok) {
		gerr(vc, LIT("gradient needs a kind: \"linear\", \"radial\", \"conic\" or \"mesh\" (any of them can be \"repeating\")"), "try  gradient: linear 135, color.accent -> color.ok");
		return 0;
	}
	sp rest = sp_trim(sub(after, klen, after.n));
	if (rest.n && rest.p[0] == ',') rest = sp_ltrim(sub(rest, 1, rest.n));

	sp head = mk("", 0);
	SpVec stops = { 0 };
	SpVec arrow = { 0 }, arrow_t = { 0 };
	split_arrow(A, rest, &arrow);
	for (int i = 0; i < arrow.n; i++) { sp x = sp_trim(arrow.v[i]); if (x.n) vpush(A, &arrow_t, x); }
	if (arrow_t.n >= 2) {
		head = arrow_t.v[0];
		for (int i = 1; i < arrow_t.n; i++) vpush(A, &stops, arrow_t.v[i]);
		SpVec hc = { 0 };
		split_commas(A, head, &hc);
		if (hc.n > 1) {
			head = hc.v[0];
			SpVec ns = { 0 };
			for (int i = 1; i < hc.n; i++) vpush(A, &ns, hc.v[i]);
			for (int i = 0; i < stops.n; i++) vpush(A, &ns, stops.v[i]);
			stops = ns;
		}
		/* no direction at all: the first part is a stop */
		int unshift = 0;
		if (head.n) {
			int pre = prefix_shaped(A, kind, head);
			int a1 = !pre;
			int a2 = 0;
			if (a1) {
				if (head.p[0] == '#') a2 = 1;
				else {
					size_t k = 0;
					while (k < head.n && (is_alpha((unsigned char)head.p[k]) || head.p[k] == '-')) k++;
					if (k > 0 && k < head.n && head.p[k] == '(') a2 = 1;
					else {
						k = 0;
						while (k < head.n && is_alpha((unsigned char)head.p[k])) k++;
						if (k > 0 && k + 1 < head.n && head.p[k] == '.' && (is_alpha((unsigned char)head.p[k + 1]) || is_digit((unsigned char)head.p[k + 1]) || head.p[k + 1] == '-')) a2 = 1;
					}
				}
			}
			int cw = 0;
			if (!pre) {
				size_t k = 0;
				while (k < head.n && !is_ws((unsigned char)head.p[k])) k++;
				sp w = sp_lower(A, sub(head, 0, k));
				cw = vset_has(&V_COLOR_WORD, w.p, w.n);
			}
			unshift = (a1 && a2) || cw;
		}
		if (unshift) {
			SpVec ns = { 0 };
			vpush(A, &ns, head);
			for (int i = 0; i < stops.n; i++) vpush(A, &ns, stops.v[i]);
			stops = ns;
			head = mk("", 0);
		}
	} else {
		SpVec segs = { 0 };
		split_commas(A, rest, &segs);
		int s0 = 0;
		while (segs.n - s0 > 1 && prefix_shaped(A, kind, segs.v[s0])) {
			head = head.n ? a_fmt(A, "%.*s %.*s", PF(head), PF(segs.v[s0])) : segs.v[s0];
			s0++;
		}
		for (int i = s0; i < segs.n; i++) vpush(A, &stops, segs.v[i]);
	}
	if (stops.n < 2) {
		gerr(vc, LIT("gradient needs at least two stops"), "try  gradient: linear 135, #ff0055 -> #00d4ff  - commas work too: linear 135, #ff0055, #00d4ff");
		return 0;
	}
	sp interp = mk("", 0);
	{
		size_t st, in;
		if (interp_match(head, &st, &in)) {
			interp = sp_lower(A, sp_trim(sub(head, in, head.n)));
			head = sp_trim(sub(head, 0, st));
		}
	}
	sp lead = mk("", 0);
	if (kind == G_LINEAR) {
		int deg = 0;
		if (!head.n) lead = LIT("180deg");
		else if (number_deg(head, &deg) && !deg) lead = a_cat2(A, head, LIT("deg"));
		else if (number_deg(head, &deg) && deg) lead = sp_lower(A, head);
		else if (dir_form(A, head)) lead = sp_lower(A, head);
		else {
			gerr(vc, a_fmt(A, "\"%.*s\" is not a linear gradient direction", PF(head)), "use an angle (linear 135, \xE2\x80\xA6) or words (linear to bottom right, \xE2\x80\xA6)");
			return 0;
		}
	} else if (kind == G_RADIAL) {
		if (head.n) {
			SpVec at = { 0 };
			split_ws(A, head, 1, &at);
			for (int i = 0; i < at.n; i++) {
				sp a = at.v[i];
				if (sp_ieq(a, "at") || radial_atom(a, 1)) continue;
				gerr(vc, a_fmt(A, "\"%.*s\" is not part of a radial gradient shape", PF(a)), "try  gradient: radial circle at center, #fff -> #000");
				return 0;
			}
			lead = head;
		}
	} else if (head.n) {
		/* ^from\s+(-?\d+(?:\.\d+)?)(deg)?(?:\s+(at\s+[\s\S]+))?$ */
		int matched = 0;
		if (sp_istarts(head, "from") && head.n > 4 && is_ws((unsigned char)head.p[4])) {
			size_t j = 4;
			while (j < head.n && is_ws((unsigned char)head.p[j])) j++;
			size_t e = num_at(head, j);
			if (e > j) {
				sp numtxt = sub(head, j, e);
				size_t k = e;
				if (k + 3 <= head.n && sp_ieq(sub(head, k, k + 3), "deg")) k += 3;
				if (k == head.n) { lead = a_fmt(A, "from %.*sdeg", PF(numtxt)); matched = 1; }
				else if (is_ws((unsigned char)head.p[k])) {
					size_t m = k;
					while (m < head.n && is_ws((unsigned char)head.p[m])) m++;
					if (m + 2 <= head.n && sp_ieq(sub(head, m, m + 2), "at")) {
						size_t m2 = m + 2, w = m2;
						while (m2 < head.n && is_ws((unsigned char)head.p[m2])) m2++;
						if (m2 > w && m2 < head.n) {
							lead = a_fmt(A, "from %.*sdeg %.*s", PF(numtxt), PF(sp_trim(sub(head, m, head.n))));
							matched = 1;
						}
					}
				}
			}
		}
		if (!matched) {
			if (sp_istarts(head, "at") && head.n > 2 && is_ws((unsigned char)head.p[2])) {
				size_t k = 2;
				while (k < head.n && is_ws((unsigned char)head.p[k])) k++;
				if (k < head.n) { lead = head; matched = 1; }
			}
		}
		if (!matched) {
			gerr(vc, a_fmt(A, "\"%.*s\" is not a conic gradient prefix", PF(head)), "use  from <angle>  and/or  at <position> - try conic from 45 at center, \xE2\x80\xA6");
			return 0;
		}
	}

	SpVec sv = { 0 };
	const char *pos_unit = kind == G_CONIC ? "deg" : "px";
	for (int si = 0; si < stops.n; si++) {
		sp s = stops.v[si];
		if (pos_token(s)) {
			if (si == 0 || si == stops.n - 1) {
				gerr(vc, a_fmt(A, "\"%.*s\" is a midpoint, so it has to sit between two colours", PF(s)), "try  gradient: linear 90, #f00 -> 30% -> #00f");
				return 0;
			}
			sp as = is_number_str(s, 1) ? (sp_eqs(s, "0") ? s : a_cat2(A, s, mk(pos_unit, strlen(pos_unit)))) : s;
			vpush(A, &sv, as);
			continue;
		}
		sp color_part = s, pos = mk("", 0);
		int has_pos = 0;
		/* \s+(POS)(?:\s+(POS))?$ , leftmost */
		SpVec tk = { 0 };
		split_ws(A, s, 0, &tk);
		if (tk.n >= 2) {
			int k = tk.n - 1;
			int two = k - 1 >= 1 && pos_token(tk.v[k - 1]) && pos_token(tk.v[k]);
			int one = pos_token(tk.v[k]);
			if (two || one) {
				int first = two ? k - 1 : k;
				const char *last_end = tk.v[first].p;
				size_t idx = (size_t)(last_end - s.p);
				while (idx > 0 && is_ws((unsigned char)s.p[idx - 1])) idx--;
				color_part = sp_trim(sub(s, 0, idx));
				has_pos = 1;
				#define ASPOS(x) (is_number_str((x), 1) ? (sp_eqs((x), "0") ? (x) : a_cat2(A, (x), mk(pos_unit, strlen(pos_unit)))) : (x))
				pos = two ? a_fmt(A, "%.*s %.*s", PF(ASPOS(tk.v[k - 1])), PF(ASPOS(tk.v[k]))) : ASPOS(tk.v[k]);
			}
		}
		if (!color_part.n) {
			gerr(vc, a_fmt(A, "a gradient stop is missing its color (got \"%.*s\")", PF(s)), NULL);
			return 0;
		}
		sp val = resolve_value(vc, color_part);
		int alpha_only = val.n > 0;
		for (size_t i = 0; i < val.n; i++) if (!(is_alpha((unsigned char)val.p[i]) || val.p[i] == '-')) { alpha_only = 0; break; }
		if (alpha_only) {
			sp lv = sp_lower(A, val);
			if (!vset_has(&V_COLOR_WORD, lv.p, lv.n)) {
				Dym d;
				dym_init(&d, A, val);
				dym_set(&d, &V_COLOR_WORD);
				sp h;
				int has = dym_done(&d, &h);
				diag_push(A, vc->errors, vc->line, vc->col, a_fmt(A, "gradient stop \"%.*s\" is not a color", PF(s)),
				          has ? NULL : "colors, tokens (color.ink) and alpha mixes (ink/40) all work", has ? h : mk(NULL, 0), has);
				return 0;
			}
		}
		vpush(A, &sv, has_pos ? a_fmt(A, "%.*s %.*s", PF(val), PF(pos)) : val);
	}
	SB b;
	sb_init(&b, A);
	if (repeating) sb_puts(&b, "repeating-");
	sb_puts(&b, kind == G_LINEAR ? "linear-gradient(" : kind == G_RADIAL ? "radial-gradient(" : "conic-gradient(");
	int first = 1;
	if (lead.n || interp.n) {
		if (lead.n) sb_putsp(&b, lead);
		if (lead.n && interp.n) sb_putc(&b, ' ');
		if (interp.n) sb_putsp(&b, interp);
		first = 0;
	}
	for (int i = 0; i < sv.n; i++) {
		if (!sv.v[i].n) continue;   /* .filter(Boolean) */
		if (!first) sb_puts(&b, ", ");
		sb_putsp(&b, sv.v[i]);
		first = 0;
	}
	sb_putc(&b, ')');
	*outp = sb_done(&b);
	return 1;
}

/* ---------------------------------------------------------------- layers with options */

static int is_css_fn_layer(sp t)
{
	if (sp_istarts(t, "-webkit-") || sp_istarts(t, "url(") || sp_istarts(t, "image-set(")) return 1;
	sp r = t;
	if (sp_istarts(r, "repeating-")) r = sub(r, 10, r.n);
	static const char *const k[] = { "linear-gradient(", "radial-gradient(", "conic-gradient(" };
	for (int i = 0; i < 3; i++) if (sp_istarts(r, k[i])) return 1;
	/* the non-optional reading: a bare "repeating-" prefix needs the rest to match too */
	return 0;
}

int build_gradient_parts(VC *vc, sp v, GradParts *res)
{
	Arena *A = vc->r->A;
	SpVec layers = { 0 };
	split_layers(A, sp_trim(v), &layers);
	SpVec images = { 0 }, sizes = { 0 }, poss = { 0 }, reps = { 0 };
	int any_size = 0, any_pos = 0, any_rep = 0;
	for (int li = 0; li < layers.n; li++) {
		sp layer = layers.v[li];
		sp size = mk(NULL, 0), pos = mk(NULL, 0), rep = mk(NULL, 0);
		int has_size = 0, has_pos = 0, has_rep = 0;
		for (;;) {
			sp tk[2];
			int nt;
			long m = tail_match(layer, "tile", 1, 2, tk, &nt);
			if (m >= 0 && !(sp_ieq(tk[0], "tile") || sp_ieq(tk[0], "offset"))) {
				sp second = nt > 1 ? tk[1] : tk[0];
				size = a_fmt(A, "%.*s %.*s", PF(px_of(A, tk[0])), PF(px_of(A, second)));
				has_size = 1;
				layer = sub(layer, 0, (size_t)m);
				continue;
			}
			m = tail_match(layer, "offset", 2, 2, tk, &nt);
			if (m >= 0 && nt == 2) {
				pos = a_fmt(A, "%.*s %.*s", PF(px_of(A, tk[0])), PF(px_of(A, tk[1])));
				has_pos = 1;
				layer = sub(layer, 0, (size_t)m);
				continue;
			}
			m = tail_match(layer, NULL, 1, 1, tk, &nt);
			if (m >= 0) {
				rep = sp_lower(A, tk[0]);
				has_rep = 1;
				layer = sub(layer, 0, (size_t)m);
				continue;
			}
			break;
		}
		sp img;
		sp lt = sp_trim(layer);
		if (is_css_fn_layer(lt)) img = plain_value(vc, lt);
		else if (!build_layer(vc, layer, &img)) return 0;
		vpush(A, &images, img);
		vpush(A, &sizes, has_size ? size : mk(NULL, 0));
		vpush(A, &poss, has_pos ? pos : mk(NULL, 0));
		vpush(A, &reps, has_rep ? rep : mk(NULL, 0));
		any_size |= has_size; any_pos |= has_pos; any_rep |= has_rep;
	}
	SB b;
	sb_init(&b, A);
	for (int i = 0; i < images.n; i++) { if (i) sb_puts(&b, ", "); sb_putsp(&b, images.v[i]); }
	res->image = sb_done(&b);
	res->has_size = any_size; res->has_pos = any_pos; res->has_rep = any_rep;
	struct { SpVec *v; const char *dflt; sp *dst; int any; } L[3] = {
		{ &sizes, "auto", &res->size, any_size }, { &poss, "0 0", &res->pos, any_pos }, { &reps, "repeat", &res->rep, any_rep },
	};
	for (int k = 0; k < 3; k++) {
		if (!L[k].any) { *L[k].dst = mk("", 0); continue; }
		SB c;
		sb_init(&c, A);
		for (int i = 0; i < L[k].v->n; i++) {
			if (i) sb_puts(&c, ", ");
			if (L[k].v->v[i].p) sb_putsp(&c, L[k].v->v[i]); else sb_puts(&c, L[k].dflt);
		}
		*L[k].dst = sb_done(&c);
	}
	return 1;
}
