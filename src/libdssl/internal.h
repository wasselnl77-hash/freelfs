/* libdssl internals. SPDX-License-Identifier: MIT */
#ifndef DSSL_INTERNAL_H
#define DSSL_INTERNAL_H

#include "dssl.h"
#include "vocab.h"
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ memory and strings */

typedef struct { const char *p; size_t n; } sp;
#define LIT(s) ((sp){ "" s, sizeof(s) - 1 })
#define PF(s) (int)((s).n), ((s).p ? (s).p : "")
static inline sp mk(const char *p, size_t n) { sp s = { p, n }; return s; }
static inline sp sub(sp s, size_t from, size_t to) { return mk(s.p + from, to - from); }  /* [from, to) */
static inline int sp_eq(sp a, sp b) { return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0); }
static inline int sp_eqs(sp a, const char *z) { size_t n = strlen(z); return a.n == n && (n == 0 || memcmp(a.p, z, n) == 0); }
static inline int sp_starts(sp a, const char *z) { size_t n = strlen(z); return a.n >= n && memcmp(a.p, z, n) == 0; }
static inline int sp_ends(sp a, const char *z) { size_t n = strlen(z); return a.n >= n && memcmp(a.p + a.n - n, z, n) == 0; }

typedef struct Blk Blk;
typedef struct Arena {
	Blk *head;
	size_t total, limit;
	jmp_buf *jb;   /* where to go when memory runs out or the limit is hit */
} Arena;

void arena_init(Arena *a, size_t limit);
void arena_free(Arena *a);
void *a_alloc(Arena *a, size_t n);     /* never returns NULL: longjmps to a->jb */
void *a_zalloc(Arena *a, size_t n);
sp a_dup(Arena *a, sp s);                       /* copy, NUL terminated */
sp a_dups(Arena *a, const char *z);
sp a_fmt(Arena *a, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
sp a_cat2(Arena *a, sp x, sp y);
sp a_cat3(Arena *a, sp x, sp y, sp z);

typedef struct { Arena *a; char *p; size_t n, cap; } SB;
void sb_init(SB *b, Arena *a);
void sb_grow(SB *b, size_t need);
static inline void sb_putc(SB *b, char c) { if (b->n + 1 >= b->cap) sb_grow(b, 1); b->p[b->n++] = c; }
void sb_put(SB *b, const char *p, size_t n);
static inline void sb_puts(SB *b, const char *z) { sb_put(b, z, strlen(z)); }
static inline void sb_putsp(SB *b, sp s) { sb_put(b, s.p, s.n); }
void sb_fmt(SB *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sb_num(SB *b, double v);                    /* a JS number, as String(v) */
sp sb_done(SB *b);                               /* NUL terminated view, valid in the arena */

#define VEC(name, T) typedef struct { T *v; int n, cap; } name
void vec_grow(Arena *a, void **v, int *cap, size_t sz);
#define vpush(A, vec, val) do { if ((vec)->n == (vec)->cap) vec_grow((A), (void **)&(vec)->v, &(vec)->cap, sizeof(*(vec)->v)); (vec)->v[(vec)->n++] = (val); } while (0)

VEC(SpVec, sp);

/* JS-flavoured string helpers (ASCII classes, as the engine's regexes use) */
static inline int is_ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r'; }
static inline int is_digit(int c) { return c >= '0' && c <= '9'; }
static inline int is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static inline int is_word(int c) { return is_alpha(c) || is_digit(c) || c == '_'; }
static inline int is_hex(int c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static inline int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
sp sp_trim(sp s);
sp sp_ltrim(sp s);
sp sp_rtrim(sp s);
sp sp_lower(Arena *a, sp s);
int sp_ieq(sp a, const char *z);                 /* ASCII case-insensitive equality */
int sp_istarts(sp a, const char *z);
int sp_has(sp hay, const char *z);               /* substring */
long sp_find(sp hay, const char *z);             /* index or -1 */
long sp_findc(sp hay, char c);
void split_ws(Arena *a, sp s, int keep_empty, SpVec *out);   /* s.split(/\s+/) (keep_empty) or .filter(Boolean) */
void split_char(Arena *a, sp s, char c, SpVec *out);         /* s.split(c) */
sp utf16_prefix(sp s, size_t units);             /* s.slice(0, units), cut on a character boundary */
int is_ident_plain(sp s);                        /* /^[A-Za-z][A-Za-z0-9_-]*$/ */
int is_int_str(sp s);                            /* /^[0-9]+$/ */
int is_number_str(sp s, int allow_sign);         /* /^-?\d+(\.\d+)?$/ (sign optional) */
double js_parse_float(sp s);                     /* parseFloat; NaN when no number */
sp js_tofixed3(Arena *a, double v);              /* String(+v.toFixed(3)) */
size_t utf8_sanitize(const char *src, size_t n, char *dst);  /* invalid bytes -> U+FFFD; dst has room for 3n */

/* did-you-mean (spec DIA-8): feed candidates in the engine's order, then ask */
typedef struct {
	Arena *a; char *lw; size_t wn; int tol, best; sp best_s; int found; int *rows;
} Dym;
void dym_init(Dym *d, Arena *a, sp word);
void dym_one(Dym *d, const char *p, size_t n);
void dym_set(Dym *d, const VSet *s);
void dym_list(Dym *d, const char *const *list, int n);
void dym_sp(Dym *d, sp s);
int dym_done(Dym *d, sp *out);        /* 1 and *out set when there is a suggestion */

/* ------------------------------------------------------------------ diagnostics */

typedef struct { int line, col; sp msg, hint; int has_hint; } Diag;
VEC(DiagVec, Diag);

/* ------------------------------------------------------------------ syntax tree (chapter 2) */

typedef enum {
	K_ROOT, K_TOKENS, K_PIN, K_RAW, K_FONTS, K_BEHAVIOR, K_MOTION, K_LAYER, K_MEDIA, K_CONTAINER,
	K_SIGNAL, K_BOGUS, K_PSEUDO, K_STATE, K_SELECTOR, K_DECL
} NodeKind;

typedef struct Node Node;
struct Node {
	NodeKind kind;
	int line, col;
	sp a;        /* name, query, expr, sel, state/pseudo name, decl prop */
	sp value;    /* decl value */
	sp css;      /* raw/fonts body */
	int has_css;
	Node *first, *last, *next;
};

/* ------------------------------------------------------------------ resolved document (chapter 10) */

typedef enum { L_TOKENS, L_BASE, L_COMPONENTS, L_UTILITIES, L_PIN } Layer;
extern const char *const LAYER_NAMES[5];

typedef enum { OP_TRUTHY, OP_EQ, OP_NE, OP_IN, OP_NOTIN } SigOp;
typedef struct { sp name; SigOp op; sp value; int has_value; SpVec values; int has_values; } Signal;
typedef struct { sp *v; int n; } SpList;
typedef struct { Signal *v; int n; } SigList;

typedef struct { double factor; sp axis; } Stretch;
typedef struct { double max, lift, perspective, speed; int invert, glare; } Tilt;
typedef struct { double duration; sp easing, direction; } GFlow;

typedef struct Motion Motion;
typedef struct {
	sp motion;
	double duration; sp easing; sp fill; double delay;
	int has_iter; double iter;
	int has_jitter; double jitter;
	sp direction; int has_direction;
	int essential_here;
	Motion *linked; int broken; int link_essential;
} Anim;

typedef enum { DV_STR, DV_NULL, DV_NUM, DV_STRETCH, DV_NAMES, DV_ANIM, DV_TILT, DV_GFLOW, DV_ENGINE } DVKind;
typedef struct {
	sp prop, value;
	int line, col;
	sp via; int has_via;
	int from_gradient;
	DVKind kind;
	double num;
	Stretch st;
	SpList names;
	Anim *anim;
	Tilt tilt;
	GFlow gf;
} Decl;
VEC(DeclVec, Decl);

typedef struct {
	int id;
	sp sel;
	SpList media, containers;
	SigList signals;
	int layer, order;
	Decl *decls; int ndecls;
	int line, col;
	int is_shadow;
	sp pseudo; int has_pseudo;
	unsigned states; int has_states;
	int is_token;
} Rule;
typedef Rule *RulePtr;
VEC(RuleVec, RulePtr);

typedef struct { sp css; int line, col, order; } Raw;
VEC(RawVec, Raw);
typedef struct { sp url; int line; } FontLink;
VEC(FontLinkVec, FontLink);

typedef struct { sp camel, css, value; } Prop;
VEC(PropVec, Prop);
typedef struct { double offset; PropVec props; } Frame;
VEC(FrameVec, Frame);
struct Motion { sp name; int essential; FrameVec frames; };
typedef Motion *MotionPtr;
VEC(MotionVec, MotionPtr);

typedef struct { sp key, value; uint32_t hash; int line, col; int own; } TokEnt;
typedef struct {
	TokEnt *e; int n, cap;
	int *idx; int idx_cap;    /* open addressing, -1 empty */
} TokMap;
void tm_init(TokMap *m);
TokEnt *tm_find(TokMap *m, sp key);
void tm_set(Arena *a, TokMap *m, sp key, sp value, int line, int col, int own);   /* replaces in place */
void tm_clone(Arena *a, TokMap *dst, const TokMap *src);                  /* shallow: shares the strings */
void tm_copy(Arena *a, TokMap *dst, const TokMap *src);                   /* deep copy of the strings */

typedef struct { unsigned kind; const char *name; int line, col; } Feat;
VEC(FeatVec, Feat);

struct dssl_ctx {
	Arena A;
	TokMap tokens;
	MotionVec motions;
	long next_order;
	int installed;      /* the registry rule was already handed out for an earlier sheet */
};

struct dssl_sheet {
	Arena A;
	sp file;
	Node *root;
	DiagVec errors, warnings;
	int n_parse_errors;
	RuleVec rules, shadow;
	RawVec raws;
	FontLinkVec fontlinks;
	SpVec motion_names;
	MotionVec motions;       /* those with frames, in definition order */
	SpVec media_queries;
	TokMap tokens;           /* the final map */
	Rule *registry;
	Rule *registry_css;      /* the registry rule as the CSS lane writes it (only what changed) */
	FeatVec feats;
	unsigned feat_mask;
	long order_base, max_order;
};

/* the resolver state shared by value.c and resolve.c */
struct R {
	struct dssl_sheet *S;
	Arena *A;
	sp file;
	TokMap tokens;           /* the working token map (starts as the registry) */
	DiagVec *errors, *warnings;
	DiagVec scratch;         /* errors that are thrown away (tokens that came from an earlier sheet) */
	DeclVec tmp;
	long steps;
};
typedef struct VC {
	struct R *r;
	sp prop;
	int unitless, bare;
	int line, col;
	DiagVec *errors;
	int depth;
} VC;
sp resolve_value(VC *vc, sp raw);
typedef struct { sp image, size, pos, rep; int has_size, has_pos, has_rep; } GradParts;
int build_gradient_parts(VC *vc, sp v, GradParts *out);
void resolve_decl(struct R *r, sp prop, sp value, int line, int col, DeclVec *out);
void diag_push(Arena *a, DiagVec *v, int line, int col, sp msg, const char *hint, sp hint_sp, int has_hint);
int js_number(sp s, double *out);     /* Number(s) for the forms the engine meets; 0 for NaN */

/* parse.c */
Node *parse_source(Arena *a, sp file, sp src, DiagVec *errs, int *depth_exceeded);
/* value.c / resolve.c */
typedef struct R R;
void resolve_sheet(struct dssl_sheet *s, struct dssl_ctx *ctx, Node *root);

/* emit */
char *emit_json(const struct dssl_sheet *s);
char *emit_tree_json(const struct dssl_sheet *s);
char *emit_css(const struct dssl_sheet *s, unsigned flags);

/* misc shared */
sp feat_name(unsigned kind);
extern const char *const STATE_SCAN_WORDS[];
unsigned scan_states(sp sel);
const char *const *state_scan_names(int *n);

#endif
