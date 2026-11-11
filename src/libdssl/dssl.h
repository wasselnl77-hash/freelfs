/* libdssl: a native front end for DSSL (the style language of EHS-Studio/dssl, MIT).
 *
 * Step 1 of native DSSL support: parse DSSL text, resolve it the way chapters 2 to 7 of the
 * native spec (native/spec in the DSSL repo, v0.16.3) say, and lower the part that has a CSS
 * equivalent to CSS text (spec 11.3, INT-6). What needs engine work (spec 11.4) is detected
 * and listed, never half-implemented: ask dssl_needs_engine() and fall back to the JavaScript
 * engine for that sheet.
 *
 * C11, no dependencies beyond libc and libm. All functions are safe on any input (arbitrary
 * bytes, truncated files); out of memory and the size limits make dssl_parse() return NULL.
 * Not thread-safe per object; distinct sheets and contexts may be used from distinct threads.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef DSSL_H
#define DSSL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__)
#define DSSL_API __attribute__((visibility("default")))
#else
#define DSSL_API
#endif

#define DSSL_SPEC_VERSION "0.16.3"
#define DSSL_VERSION "0.1"

typedef struct dssl_ctx dssl_ctx;
typedef struct dssl_sheet dssl_sheet;

/* Context for a page: the token registry and the motion registry that flow from sheet to sheet
 * in load order (spec 4.5, 7.3, INT-3), and the running rule order (CAS-16). Create one per
 * document, pass it to every dssl_parse() of that document in load order. NULL means "a fresh
 * context for this one sheet". A sheet must not be used after its context is freed. */
DSSL_API dssl_ctx *dssl_ctx_new(void);
DSSL_API void dssl_ctx_free(dssl_ctx *ctx);

/* Parse and resolve one sheet. `url` is the name used in diagnostics (and the "file" field of
 * the rule objects); it may be NULL. The source need not be NUL terminated and may hold any
 * bytes (invalid UTF-8 becomes U+FFFD). With a context, the sheet's tokens and motions are
 * folded into it afterwards, even when the sheet has errors, as the JS engine does.
 * Returns NULL only on out-of-memory or when the input exceeds the size limits. */
DSSL_API dssl_sheet *dssl_parse(const char *src, size_t len, const char *url, dssl_ctx *ctx);
DSSL_API void dssl_free(dssl_sheet *sheet);

/* Diagnostics: errors first (parser errors, then resolver errors, as in spec DIA-4), then
 * warnings. line is the spec's line (1-based, 0 for the file top level); col is the 1-based
 * byte column where the construct starts (0 when unknown). hint may be NULL. The strings
 * live as long as the sheet. */
typedef struct {
	int line, col;
	const char *file, *message, *hint;
} dssl_diag;
DSSL_API size_t dssl_error_count(const dssl_sheet *s);
DSSL_API size_t dssl_warning_count(const dssl_sheet *s);
DSSL_API int dssl_error_at(const dssl_sheet *s, size_t i, dssl_diag *out);
DSSL_API int dssl_warning_at(const dssl_sheet *s, size_t i, dssl_diag *out);

/* Features that need the engine (spec 11.4). A bit per kind; dssl_needs_engine() returns the
 * mask of "hard" ones: those the CSS lowering cannot reproduce, so a browser should fall back
 * to the JS engine for the whole sheet. "Soft" ones are lowered approximately (see README). */
enum {
	DSSL_FEAT_SIGNAL = 1u << 0,        /* @signal blocks (hard) */
	DSSL_FEAT_SEEN = 1u << 1,          /* seen { } / :seen (hard; the CSS drops the condition) */
	DSSL_FEAT_ANIMATE_JITTER = 1u << 2,/* animate: ... jitter N (hard) */
	DSSL_FEAT_STAGGER = 1u << 3,       /* stagger: (hard) */
	DSSL_FEAT_TEXTURE = 1u << 4,       /* texture: (hard) */
	DSSL_FEAT_AURA = 1u << 5,          /* aura, aura-glow, aura-particle, glow-particle... (hard) */
	DSSL_FEAT_DEFORM = 1u << 6,        /* bend, bend-y, bend-up, fold, stretch, bend-around (hard) */
	DSSL_FEAT_TILT = 1u << 7,          /* tilt: (hard) */
	DSSL_FEAT_SLIDER = 1u << 8,        /* slider: (hard) */
	DSSL_FEAT_OVERLAP = 1u << 9,       /* overlap: (hard) */
	DSSL_FEAT_GRADIENT_FLOW = 1u << 10,/* gradient-flow: (hard) */
	DSSL_FEAT_ANIMATE = 1u << 16,      /* animate: lowered to a CSS animation (soft) */
	DSSL_FEAT_STACK = 1u << 17,        /* stack: lowered with a child rule (soft) */
	DSSL_FEAT_ALIGN_PROMOTION = 1u << 18, /* CAS-47: alignment words promote display; CSS cannot (soft) */
	DSSL_FEAT_HARD_MASK = 0xffffu
};
typedef struct {
	unsigned kind;      /* one DSSL_FEAT_* bit */
	const char *name;   /* "signal", "seen", "texture", ... */
	int line, col;
} dssl_feature;
DSSL_API unsigned dssl_needs_engine(const dssl_sheet *s);   /* mask of hard features, 0: lowering is complete */
DSSL_API unsigned dssl_features(const dssl_sheet *s);       /* mask of all features (hard and soft) */
DSSL_API size_t dssl_feature_count(const dssl_sheet *s);    /* every occurrence, in source order */
DSSL_API int dssl_feature_at(const dssl_sheet *s, size_t i, dssl_feature *out);

/* Lowering to CSS (spec 11.3, INT-6; same output as the JS engine's static lane, `dssl build`).
 * Returns a malloc'ed NUL-terminated string (free with free()), or NULL on out-of-memory.
 * Rules the CSS cannot express (signals, engine-native declarations) are skipped. */
enum {
	DSSL_CSS_BANNER = 1u << 0,   /* start with a comment naming the file */
	DSSL_CSS_STRIP_SEEN = 1u << 1/* reserved: seen rules are always emitted without :seen */
};
DSSL_API char *dssl_to_css(const dssl_sheet *s, unsigned flags);

/* The resolved document of spec chapter 10 as canonical JSON (10.3): keys sorted, no white
 * space. Rule objects are `sel, decls, layer, media, containers, order` and the rest of 5.1.
 * Engine-native values for texture, aura and slider are not computed: they are the string
 * "<engine>". Lint warnings are not produced (DIA-12). Malloc'ed, free with free(). */
DSSL_API char *dssl_to_json(const dssl_sheet *s);
/* The syntax tree and parser errors of chapter 2 (level 1) as canonical JSON. */
DSSL_API char *dssl_tree_json(const dssl_sheet *s);

/* Number of rules (without the registry rule), shadow rules, and the order base of this sheet
 * in its context (CAS-16: rule orders in the JSON are file-local, add this for the page). */
DSSL_API size_t dssl_rule_count(const dssl_sheet *s);
DSSL_API size_t dssl_shadow_rule_count(const dssl_sheet *s);
DSSL_API long dssl_order_base(const dssl_sheet *s);

#ifdef __cplusplus
}
#endif
#endif
