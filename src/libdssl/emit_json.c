/* libdssl: the resolved document and the syntax tree as canonical JSON (spec 10.2, 10.3, 2.9).
 * SPDX-License-Identifier: MIT */
#include "internal.h"
#include <math.h>
#include <stdlib.h>

static void js(SB *b, sp s)
{
	sb_putc(b, '"');
	for (size_t i = 0; i < s.n; i++) {
		unsigned char c = (unsigned char)s.p[i];
		switch (c) {
		case '"': sb_puts(b, "\\\""); break;
		case '\\': sb_puts(b, "\\\\"); break;
		case '\b': sb_puts(b, "\\b"); break;
		case '\f': sb_puts(b, "\\f"); break;
		case '\n': sb_puts(b, "\\n"); break;
		case '\r': sb_puts(b, "\\r"); break;
		case '\t': sb_puts(b, "\\t"); break;
		default:
			if (c < 0x20) sb_fmt(b, "\\u%04x", c); else sb_putc(b, (char)c);
		}
	}
	sb_putc(b, '"');
}
static void jz(SB *b, const char *z) { js(b, mk(z, strlen(z))); }

static void jnum(SB *b, double v)
{
	if (isnan(v)) sb_puts(b, "null");
	else if (isinf(v)) sb_puts(b, v < 0 ? "\"-Infinity\"" : "\"Infinity\"");
	else if (v == 0) sb_putc(b, '0');
	else sb_num(b, v);
}

static void jsplist(SB *b, const sp *v, int n)
{
	sb_putc(b, '[');
	for (int i = 0; i < n; i++) { if (i) sb_putc(b, ','); js(b, v[i]); }
	sb_putc(b, ']');
}

static void jdiags(SB *b, const DiagVec *v, int from, int to, sp file)
{
	sb_putc(b, '[');
	for (int i = from; i < to; i++) {
		const Diag *d = &v->v[i];
		if (i > from) sb_putc(b, ',');
		sb_puts(b, "{\"file\":");
		js(b, file);
		if (d->has_hint) { sb_puts(b, ",\"hint\":"); js(b, d->hint); }
		sb_fmt(b, ",\"line\":%d,\"message\":", d->line);
		js(b, d->msg);
		sb_putc(b, '}');
	}
	sb_putc(b, ']');
}

static int cmp_sp_ptr(const void *a, const void *b)
{
	const sp *x = *(const sp *const *)a, *y = *(const sp *const *)b;
	size_t m = x->n < y->n ? x->n : y->n;
	int r = memcmp(x->p, y->p, m);
	if (r) return r;
	return x->n < y->n ? -1 : x->n > y->n;
}

static void jprops(SB *b, const PropVec *pv)
{
	/* object keys in code point order */
	int n = pv->n;
	const Prop **idx = n ? a_alloc(b->a, (size_t)n * sizeof *idx) : NULL;
	for (int i = 0; i < n; i++) idx[i] = &pv->v[i];
	for (int i = 1; i < n; i++) {   /* insertion sort: a handful of properties */
		const Prop *x = idx[i];
		int j = i - 1;
		while (j >= 0 && cmp_sp_ptr(&(const sp *){ &idx[j]->camel }, &(const sp *){ &x->camel }) > 0) { idx[j + 1] = idx[j]; j--; }
		idx[j + 1] = x;
	}
	sb_putc(b, '{');
	for (int i = 0; i < n; i++) {
		if (i) sb_putc(b, ',');
		js(b, idx[i]->camel);
		sb_putc(b, ':');
		js(b, idx[i]->value);
	}
	sb_putc(b, '}');
}

static void jframes(SB *b, const Motion *m)
{
	sb_putc(b, '[');
	for (int i = 0; i < m->frames.n; i++) {
		if (i) sb_putc(b, ',');
		sb_puts(b, "{\"offset\":");
		jnum(b, m->frames.v[i].offset);
		sb_puts(b, ",\"props\":");
		jprops(b, &m->frames.v[i].props);
		sb_putc(b, '}');
	}
	sb_putc(b, ']');
}

static void janim(SB *b, const Anim *a)
{
	sb_putc(b, '{');
	if (a->broken) sb_puts(b, "\"broken\":true,");
	sb_puts(b, "\"delay\":"); jnum(b, a->delay);
	if (a->has_direction) { sb_puts(b, ",\"direction\":"); js(b, a->direction); }
	sb_puts(b, ",\"duration\":"); jnum(b, a->duration);
	sb_puts(b, ",\"easing\":"); js(b, a->easing);
	if (a->linked) sb_fmt(b, ",\"essential\":%s", a->link_essential ? "true" : "false");
	if (a->essential_here) sb_puts(b, ",\"essentialHere\":true");
	sb_puts(b, ",\"fill\":"); js(b, a->fill);
	if (a->linked) { sb_puts(b, ",\"frames\":"); jframes(b, a->linked); }
	if (a->has_iter) { sb_puts(b, ",\"iterations\":"); jnum(b, a->iter); }
	if (a->has_jitter) { sb_puts(b, ",\"jitter\":"); jnum(b, a->jitter); }
	sb_puts(b, ",\"motion\":"); js(b, a->motion);
	sb_putc(b, '}');
}

static void jdecl(SB *b, const Decl *d)
{
	sb_putc(b, '{');
	int first = 1;
	if (d->from_gradient) { sb_puts(b, "\"fromGradient\":true"); first = 0; }
	if (d->line >= 0) { if (!first) sb_putc(b, ','); sb_fmt(b, "\"line\":%d", d->line); first = 0; }
	if (!first) sb_putc(b, ',');
	sb_puts(b, "\"prop\":");
	js(b, d->prop);
	sb_puts(b, ",\"value\":");
	switch (d->kind) {
	case DV_STR: js(b, d->value); break;
	case DV_NULL: sb_puts(b, "null"); break;
	case DV_NUM: jnum(b, d->num); break;
	case DV_STRETCH: sb_puts(b, "{\"axis\":"); js(b, d->st.axis); sb_puts(b, ",\"factor\":"); jnum(b, d->st.factor); sb_putc(b, '}'); break;
	case DV_NAMES: jsplist(b, d->names.v, d->names.n); break;
	case DV_ANIM: janim(b, d->anim); break;
	case DV_TILT:
		sb_fmt(b, "{\"glare\":%s,\"invert\":%s,\"lift\":", d->tilt.glare ? "true" : "false", d->tilt.invert ? "true" : "false");
		jnum(b, d->tilt.lift); sb_puts(b, ",\"max\":"); jnum(b, d->tilt.max);
		sb_puts(b, ",\"perspective\":"); jnum(b, d->tilt.perspective);
		sb_puts(b, ",\"speed\":"); jnum(b, d->tilt.speed);
		sb_putc(b, '}');
		break;
	case DV_GFLOW:
		sb_puts(b, "{\"direction\":"); js(b, d->gf.direction);
		sb_puts(b, ",\"duration\":"); jnum(b, d->gf.duration);
		sb_puts(b, ",\"easing\":"); js(b, d->gf.easing);
		sb_putc(b, '}');
		break;
	case DV_ENGINE: sb_puts(b, "\"<engine>\""); break;
	}
	if (d->has_via) { sb_puts(b, ",\"via\":"); js(b, d->via); }
	sb_putc(b, '}');
}

static int cmp_cstr(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

static void jrule(SB *b, const Rule *r, sp file)
{
	sb_puts(b, "{\"containers\":");
	jsplist(b, r->containers.v, r->containers.n);
	sb_puts(b, ",\"decls\":[");
	for (int i = 0; i < r->ndecls; i++) { if (i) sb_putc(b, ','); jdecl(b, &r->decls[i]); }
	sb_puts(b, "],\"file\":");
	js(b, file);
	sb_fmt(b, ",\"id\":%d,\"layer\":\"%s\",\"line\":%d,\"media\":", r->id, LAYER_NAMES[r->layer], r->line);
	jsplist(b, r->media.v, r->media.n);
	sb_fmt(b, ",\"order\":%d,\"pseudo\":", r->order);
	if (r->has_pseudo) js(b, r->pseudo); else sb_puts(b, "null");
	sb_puts(b, ",\"sel\":");
	js(b, r->sel);
	sb_puts(b, ",\"signals\":[");
	for (int i = 0; i < r->signals.n; i++) {
		const Signal *s = &r->signals.v[i];
		static const char *const ops[] = { "truthy", "==", "!=", "in", "notin" };
		if (i) sb_putc(b, ',');
		sb_puts(b, "{\"name\":");
		js(b, s->name);
		sb_fmt(b, ",\"op\":\"%s\",\"value\":", ops[s->op]);
		if (s->has_value) js(b, s->value); else sb_puts(b, "null");
		if (s->has_values) { sb_puts(b, ",\"values\":"); jsplist(b, s->values.v, s->values.n); }
		sb_putc(b, '}');
	}
	sb_putc(b, ']');
	if (r->has_states) {
		int n;
		const char *const *names = state_scan_names(&n);
		const char *sel[32];
		int k = 0;
		for (int i = 0; i < n; i++) if (r->states & (1u << i)) sel[k++] = names[i];
		qsort(sel, (size_t)k, sizeof sel[0], cmp_cstr);
		sb_puts(b, ",\"states\":[");
		for (int i = 0; i < k; i++) { if (i) sb_putc(b, ','); jz(b, sel[i]); }
		sb_putc(b, ']');
	}
	sb_putc(b, '}');
}

static char *finish(SB *b, Arena *a)
{
	sp r = sb_done(b);
	char *out = malloc(r.n + 1);
	if (out) { memcpy(out, r.p, r.n); out[r.n] = 0; }
	arena_free(a);
	return out;
}

char *emit_json(const struct dssl_sheet *s)
{
	Arena a;
	jmp_buf jb;
	arena_init(&a, (size_t)1 << 31);
	a.jb = &jb;
	if (setjmp(jb)) { arena_free(&a); return NULL; }
	SB bb;
	sb_init(&bb, &a);
	SB *b = &bb;
	sp file = s->file;
	sb_puts(b, "{\"errors\":");
	jdiags(b, &s->errors, 0, s->errors.n, file);
	sb_puts(b, ",\"fontLinks\":[");
	for (int i = 0; i < s->fontlinks.n; i++) {
		if (i) sb_putc(b, ',');
		sb_puts(b, "{\"file\":"); js(b, file);
		sb_fmt(b, ",\"line\":%d,\"url\":", s->fontlinks.v[i].line);
		js(b, s->fontlinks.v[i].url);
		sb_putc(b, '}');
	}
	sb_puts(b, "],\"mediaQueries\":");
	jsplist(b, s->media_queries.v, s->media_queries.n);
	sb_puts(b, ",\"motionFrames\":{");
	{
		int n = s->motions.n;
		const Motion **idx = n ? a_alloc(&a, (size_t)n * sizeof *idx) : NULL;
		for (int i = 0; i < n; i++) idx[i] = s->motions.v[i];
		for (int i = 1; i < n; i++) {
			const Motion *x = idx[i];
			int j = i - 1;
			while (j >= 0 && cmp_sp_ptr(&(const sp *){ &idx[j]->name }, &(const sp *){ &x->name }) > 0) { idx[j + 1] = idx[j]; j--; }
			idx[j + 1] = x;
		}
		for (int i = 0; i < n; i++) {
			if (i) sb_putc(b, ',');
			js(b, idx[i]->name);
			sb_fmt(b, ":{\"essential\":%s,\"frames\":", idx[i]->essential ? "true" : "false");
			jframes(b, idx[i]);
			sb_putc(b, '}');
		}
	}
	sb_puts(b, "},\"motionNames\":");
	jsplist(b, s->motion_names.v, s->motion_names.n);
	sb_puts(b, ",\"raws\":[");
	for (int i = 0; i < s->raws.n; i++) {
		if (i) sb_putc(b, ',');
		sb_puts(b, "{\"css\":"); js(b, s->raws.v[i].css);
		sb_puts(b, ",\"file\":"); js(b, file);
		sb_fmt(b, ",\"line\":%d,\"order\":%d}", s->raws.v[i].line, s->raws.v[i].order);
	}
	sb_puts(b, "],\"registryRule\":");
	if (s->registry) jrule(b, s->registry, file); else sb_puts(b, "null");
	sb_puts(b, ",\"rules\":[");
	for (int i = 0; i < s->rules.n; i++) { if (i) sb_putc(b, ','); jrule(b, s->rules.v[i], file); }
	sb_puts(b, "],\"shadowRules\":[");
	for (int i = 0; i < s->shadow.n; i++) { if (i) sb_putc(b, ','); jrule(b, s->shadow.v[i], file); }
	sb_puts(b, "],\"tokens\":{");
	{
		int n = s->tokens.n;
		const sp **idx = n ? a_alloc(&a, (size_t)n * sizeof *idx) : NULL;
		for (int i = 0; i < n; i++) idx[i] = &s->tokens.e[i].key;
		qsort((void *)idx, (size_t)n, sizeof *idx, cmp_sp_ptr);
		for (int i = 0; i < n; i++) {
			TokEnt *e = (TokEnt *)((const char *)idx[i] - offsetof(TokEnt, key));
			if (i) sb_putc(b, ',');
			js(b, e->key);
			sb_putc(b, ':');
			js(b, e->value);
		}
	}
	sb_puts(b, "},\"warnings\":");
	jdiags(b, &s->warnings, 0, s->warnings.n, file);
	sb_putc(b, '}');
	return finish(b, &a);
}

/* ---------------------------------------------------------------- the tree */

static void jnode(SB *b, const Node *n)
{
	static const char *const kn[] = { "root", "tokens", "pin", "raw", "fonts", "behavior", "motion", "layer", "media", "container", "signal", "bogus", "pseudo", "state", "selector", "decl" };
	if (n->kind == K_DECL) {
		sb_fmt(b, "{\"kind\":\"decl\",\"line\":%d,\"prop\":", n->line);
		js(b, n->a);
		sb_puts(b, ",\"value\":");
		js(b, n->value);
		sb_putc(b, '}');
		return;
	}
	sb_puts(b, "{\"children\":[");
	for (const Node *c = n->first; c; c = c->next) { if (c != n->first) sb_putc(b, ','); jnode(b, c); }
	sb_putc(b, ']');
	if (n->has_css) { sb_puts(b, ",\"css\":"); js(b, n->css); }
	if (n->kind == K_SIGNAL) { sb_puts(b, ",\"expr\":"); js(b, n->a); }
	sb_fmt(b, ",\"kind\":\"%s\"", kn[n->kind]);
	if (n->kind != K_ROOT) sb_fmt(b, ",\"line\":%d", n->line);
	if (n->kind == K_BEHAVIOR || n->kind == K_MOTION || n->kind == K_LAYER || n->kind == K_PSEUDO || n->kind == K_STATE) { sb_puts(b, ",\"name\":"); js(b, n->a); }
	if (n->kind == K_MEDIA || n->kind == K_CONTAINER) { sb_puts(b, ",\"query\":"); js(b, n->a); }
	if (n->kind == K_SELECTOR) { sb_puts(b, ",\"sel\":"); js(b, n->a); }
	sb_putc(b, '}');
}

char *emit_tree_json(const struct dssl_sheet *s)
{
	Arena a;
	jmp_buf jb;
	arena_init(&a, (size_t)1 << 31);
	a.jb = &jb;
	if (setjmp(jb)) { arena_free(&a); return NULL; }
	SB bb;
	sb_init(&bb, &a);
	sb_puts(&bb, "{\"errors\":");
	jdiags(&bb, &s->errors, 0, s->n_parse_errors, s->file);
	sb_puts(&bb, ",\"tree\":");
	jnode(&bb, s->root);
	sb_putc(&bb, '}');
	return finish(&bb, &a);
}
