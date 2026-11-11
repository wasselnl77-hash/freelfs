/* libdssl: lowering to CSS (spec 11.3, INT-6). A port of toCSS() of static.js 0.16.3, the
 * engine's own static lane, with two additions that are marked in the README:
 * animate: and stack: are lowered (a CSS animation, a child rule). SPDX-License-Identifier: MIT */
#include "internal.h"
#include <math.h>
#include <stdlib.h>

typedef struct { int layer; sp css; int order; int idx; } Chunk;
VEC(ChunkVec, Chunk);

static void put_decl(SB *b, const Decl *d, int pin)
{
	sb_puts(b, "  ");
	sb_putsp(b, d->prop);
	sb_puts(b, ": ");
	sb_putsp(b, d->value);
	if (pin && !sp_starts(d->prop, "--")) sb_puts(b, " !important");
	sb_putc(b, ';');
}

/* emitRule: `:where(sel)` (zero specificity, order decides) in container and media wrappers */
static sp emit_rule(Arena *A, sp sel, const Decl *decls, int n, int pin, sp pseudo, int has_pseudo, SpList media, SpList containers, const char *extra_media)
{
	SB b;
	sb_init(&b, A);
	sb_puts(&b, ":where(");
	sb_putsp(&b, sel);
	sb_putc(&b, ')');
	if (has_pseudo) { sb_puts(&b, "::"); sb_putsp(&b, pseudo); }
	sb_puts(&b, " {\n");
	for (int i = 0; i < n; i++) { if (i) sb_putc(&b, '\n'); put_decl(&b, &decls[i], pin); }
	sb_puts(&b, "\n}");
	sp out = sb_done(&b);
	for (int i = containers.n - 1; i >= 0; i--) out = a_fmt(A, "@container %.*s {\n%.*s\n}", PF(containers.v[i]), PF(out));
	/* the media list wraps last, reversed: the first query ends up outermost; the reduced-motion gate is the innermost */
	if (extra_media) out = a_fmt(A, "@media %s {\n%.*s\n}", extra_media, PF(out));
	for (int i = media.n - 1; i >= 0; i--) out = a_fmt(A, "@media %.*s {\n%.*s\n}", PF(media.v[i]), PF(out));
	return out;
}

static sp strip_seen(Arena *A, sp sel)
{
	/* .replace(/:seen\b/g, '').replace(/\s{2,}/g, ' ').trim() */
	SB b;
	sb_init(&b, A);
	for (size_t i = 0; i < sel.n; i++) {
		if (sel.p[i] == ':' && i + 5 <= sel.n && memcmp(sel.p + i + 1, "seen", 4) == 0 && (i + 5 == sel.n || !is_word((unsigned char)sel.p[i + 5]))) { i += 4; continue; }
		sb_putc(&b, sel.p[i]);
	}
	sp t = sb_done(&b);
	SB c;
	sb_init(&c, A);
	for (size_t i = 0; i < t.n; i++) {
		if (is_ws((unsigned char)t.p[i]) && i + 1 < t.n && is_ws((unsigned char)t.p[i + 1])) {
			while (i + 1 < t.n && is_ws((unsigned char)t.p[i + 1])) i++;
			sb_putc(&c, ' ');
		} else sb_putc(&c, t.p[i]);
	}
	return sp_trim(sb_done(&c));
}

static sp animation_shorthand(Arena *A, const Anim *a)
{
	SB b;
	sb_init(&b, A);
	sb_putsp(&b, a->motion);
	sb_putc(&b, ' '); sb_num(&b, a->duration); sb_puts(&b, "ms ");
	sb_putsp(&b, a->easing);
	sb_putc(&b, ' '); sb_num(&b, a->delay ? a->delay : 0); sb_puts(&b, "ms ");
	if (a->has_iter && isinf(a->iter)) sb_puts(&b, "infinite");
	else sb_num(&b, (a->has_iter && a->iter) ? a->iter : 1);
	if (a->has_direction && !sp_eqs(a->direction, "normal")) { sb_putc(&b, ' '); sb_putsp(&b, a->direction); }
	sb_putc(&b, ' ');
	sb_putsp(&b, a->fill.n ? a->fill : LIT("both"));
	return sb_done(&b);
}

static int cmp_chunk(const void *a, const void *b)
{
	const Chunk *x = a, *y = b;
	if (x->order != y->order) return x->order < y->order ? -1 : 1;
	return x->idx - y->idx;
}

static void emit_one(Arena *A, ChunkVec *chunks, const Rule *r0, int is_shadow, int *idx)
{
	if (r0->signals.n) return;
	Decl *keep = a_alloc(A, (size_t)(r0->ndecls + 2) * sizeof(Decl));
	int nk = 0;
	DeclVec anims = { 0 };
	int has_stack = 0;
	for (int i = 0; i < r0->ndecls; i++) {
		const Decl *d = &r0->decls[i];
		if (sp_eqs(d->prop, "@yield")) continue;
		if (sp_eqs(d->prop, "@stack")) { has_stack = 1; continue; }
		if (sp_eqs(d->prop, "@animate") && d->kind == DV_ANIM && d->anim->motion.n && !d->anim->broken && !d->anim->has_jitter) { vpush(A, &anims, *d); continue; }
		if (d->prop.n && d->prop.p[0] == '@') continue;
		keep[nk++] = *d;
	}
	sp sel = r0->sel;
	if (!is_shadow && !r0->is_token && sp_has(sel, ":seen")) {
		int has = 0;
		for (size_t i = 0; i + 5 <= sel.n; i++) if (sel.p[i] == ':' && memcmp(sel.p + i + 1, "seen", 4) == 0 && (i + 5 == sel.n || !is_word((unsigned char)sel.p[i + 5]))) has = 1;
		if (has) {
			sel = strip_seen(A, sel);
			if (!sel.n) sel = LIT("*");
		}
	}
	int pin = r0->layer == L_PIN;
	/* animate: the essential motions always run, the rest wait for the reduced-motion check */
	Decl essential_anim;
	int have_ess = 0, n_ess = 0;
	SB all, ess;
	sb_init(&all, A);
	sb_init(&ess, A);
	for (int i = 0; i < anims.n; i++) {
		sp one = animation_shorthand(A, anims.v[i].anim);
		if (i) sb_puts(&all, ", ");
		sb_putsp(&all, one);
		if (anims.v[i].anim->linked && anims.v[i].anim->link_essential) {
			if (n_ess) sb_puts(&ess, ", ");
			sb_putsp(&ess, one);
			n_ess++;
		}
	}
	if (anims.n && n_ess) {
		memset(&essential_anim, 0, sizeof essential_anim);
		essential_anim.prop = LIT("animation"); essential_anim.value = sb_done(&ess); essential_anim.kind = DV_STR; essential_anim.line = -1;
		keep[nk++] = essential_anim;
		have_ess = 1;
	}
	if (nk) {
		Chunk c = { (int)r0->layer, emit_rule(A, sel, keep, nk, pin, r0->pseudo, r0->has_pseudo, r0->media, r0->containers, NULL), r0->order, (*idx)++ };
		vpush(A, chunks, c);
	}
	(void)have_ess;
	if (has_stack && !is_shadow) {
		SpVec parts = { 0 };
		int depth = 0;
		size_t st = 0;
		for (size_t i = 0; i < sel.n; i++) {
			char ch = sel.p[i];
			if (ch == '(' || ch == '[') depth++;
			if (ch == ')' || ch == ']') depth--;
			if (ch == ',' && depth == 0) { vpush(A, &parts, sp_trim(mk(sel.p + st, i - st))); st = i + 1; }
		}
		sp last = sp_trim(mk(sel.p + st, sel.n - st));
		if (last.n) vpush(A, &parts, last);
		SB cs;
		sb_init(&cs, A);
		for (int i = 0; i < parts.n; i++) { if (i) sb_puts(&cs, ", "); sb_putsp(&cs, parts.v[i]); sb_puts(&cs, " > *"); }
		Decl d;
		memset(&d, 0, sizeof d);
		d.prop = LIT("grid-area"); d.value = LIT("1 / 1"); d.kind = DV_STR; d.line = -1;
		Chunk c = { (int)r0->layer, emit_rule(A, sb_done(&cs), &d, 1, pin, mk("", 0), 0, r0->media, r0->containers, NULL), r0->order, (*idx)++ };
		vpush(A, chunks, c);
	}
	if (anims.n && n_ess < anims.n) {
		Decl d;
		memset(&d, 0, sizeof d);
		d.prop = LIT("animation"); d.value = sb_done(&all); d.kind = DV_STR; d.line = -1;
		Chunk c = { (int)r0->layer, emit_rule(A, sel, &d, 1, pin, r0->pseudo, r0->has_pseudo, r0->media, r0->containers, "(prefers-reduced-motion: no-preference)"), r0->order, (*idx)++ };
		vpush(A, chunks, c);
	}
}

char *emit_css(const struct dssl_sheet *s, unsigned flags)
{
	Arena a;
	jmp_buf jb;
	arena_init(&a, (size_t)1 << 31);
	a.jb = &jb;
	if (setjmp(jb)) { arena_free(&a); return NULL; }
	Arena *A = &a;
	SB out;
	sb_init(&out, A);
	if (flags & DSSL_CSS_BANNER) {
		sb_fmt(&out, "/* generated by libdssl from %.*s - the part of DSSL that lowers to CSS (spec %s).\n   @signal rules and engine-native properties (aura, tilt, texture, ...) stay in the runtime. */\n", PF(s->file), DSSL_SPEC_VERSION);
	}
	/* fonts { } links first: an @import has to come before every rule but @layer statements */
	{
		SpVec urls = { 0 };
		for (int i = 0; i < s->fontlinks.n; i++) {
			int dup = 0;
			for (int k = 0; k < urls.n; k++) if (sp_eq(urls.v[k], s->fontlinks.v[i].url)) dup = 1;
			if (!dup) vpush(A, &urls, s->fontlinks.v[i].url);
		}
		for (int i = 0; i < urls.n; i++) sb_fmt(&out, "@import url(\"%.*s\");\n", PF(urls.v[i]));
		if (urls.n) sb_putc(&out, '\n');
	}
	ChunkVec chunks = { 0 };
	int idx = 0;
	/* keyframes for every motion, so `animation: name` works too */
	for (int i = 0; i < s->motions.n; i++) {
		const Motion *m = s->motions.v[i];
		if (!m->frames.n) continue;
		SB k;
		sb_init(&k, A);
		sb_fmt(&k, "@keyframes %.*s {\n", PF(m->name));
		for (int f = 0; f < m->frames.n; f++) {
			if (f) sb_putc(&k, '\n');
			sb_num(&k, m->frames.v[f].offset);
			sb_puts(&k, "% {\n");
			for (int p = 0; p < m->frames.v[f].props.n; p++) {
				if (p) sb_putc(&k, '\n');
				sb_fmt(&k, "  %.*s: %.*s;", PF(m->frames.v[f].props.v[p].css), PF(m->frames.v[f].props.v[p].value));
			}
			sb_puts(&k, "\n}");
		}
		sb_puts(&k, "\n}");
		Chunk c = { -1, sb_done(&k), 0, idx++ };
		vpush(A, &chunks, c);
	}
	if (s->registry_css) emit_one(A, &chunks, s->registry_css, 0, &idx);
	for (int i = 0; i < s->rules.n; i++) emit_one(A, &chunks, s->rules.v[i], 0, &idx);
	for (int i = 0; i < s->shadow.n; i++) emit_one(A, &chunks, s->shadow.v[i], 1, &idx);

	if (chunks.n > 1) qsort(chunks.v, (size_t)chunks.n, sizeof(Chunk), cmp_chunk);
	int used[5] = { 0 };
	int any = 0;
	for (int i = 0; i < chunks.n; i++) if (chunks.v[i].layer >= 0) { used[chunks.v[i].layer] = 1; any = 1; }
	if (any) {
		sb_puts(&out, "@layer tokens, base, components, utilities, pin;\n\n");
		for (int l = 0; l < 5; l++) {
			if (!used[l]) continue;
			sb_fmt(&out, "@layer %s {\n", LAYER_NAMES[l]);
			int n = 0, total = 0;
			for (int i = 0; i < chunks.n; i++) if (chunks.v[i].layer == l) total++;
			for (int i = 0; i < chunks.n; i++) {
				if (chunks.v[i].layer != l) continue;
				sb_putsp(&out, chunks.v[i].css);
				if (++n < total) sb_puts(&out, "\n\n");
			}
			sb_puts(&out, "\n}\n\n");
		}
	}
	/* keyframes sit outside the layers, in definition order (the sort above keeps their index order) */
	for (int i = 0; i < chunks.n; i++) if (chunks.v[i].layer < 0) { sb_putsp(&out, chunks.v[i].css); sb_puts(&out, "\n\n"); }
	for (int i = 0; i < s->raws.n; i++) {
		if (!s->raws.v[i].css.n) continue;
		sb_fmt(&out, "/* raw @ %.*s:%d */\n", PF(s->file), s->raws.v[i].line);
		sb_putsp(&out, s->raws.v[i].css);
		sb_puts(&out, "\n\n");
	}
	sp r = sp_rtrim(sb_done(&out));
	char *res = malloc(r.n + 2);
	if (res) { memcpy(res, r.p, r.n); res[r.n] = '\n'; res[r.n + 1] = 0; }
	arena_free(&a);
	return res;
}
