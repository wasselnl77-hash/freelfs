/* flpalette.h -- the FreeLFS colour tokens, the one place they are written down
 * SPDX-License-Identifier: MIT
 *
 * Header only, C17, no dependencies (flcomp includes it too, it does not link libflui; the
 * flcomp recipe copies this file next to its sources). libflui's fl_theme, the compositor's
 * indicator, Alt-Tab row, overview, lock screen, X window frames and pointer all take their
 * colours from here, so a colour scheme is defined exactly once.
 *
 * Two schemes: FreeLFS Dark (the default and the identity: 1d2125 / 272c33 / 343a43) and
 * FreeLFS Light (flat, sober, readable). Contrast (WCAG 2.x, computed by fltheme's `make
 * check`): text, strong, dim and err reach at least 4.5:1 on panel, bg and raised in the light
 * scheme (text 15:1, dim 6.8:1 on panel), every accent 4.8:1 or better on panel as text, and
 * the text on an accent fill (on_accent) 5.2:1 or better. The dark scheme keeps the tokens the
 * desktop has always had (dim 4.4:1 on panel is the one value just under 4.5; it is the
 * brand colour and stays).
 *
 * Tokens (what they are for):
 *   bg       the desktop and the darkest surface (an app's page behind its panels)
 *   panel    window chrome, the panel, cards: the main surface
 *   raised   hover and pressed fills, inputs, a step above panel
 *   line     hairlines and borders
 *   text     body text;  strong: headings;  dim: secondary text
 *   accent   the user's accent;  on_accent: text on an accent fill;  select: selection fill
 *   term_bg  terminal background (dark in both schemes)
 *   err      errors, close hover, low battery
 *   back     the overview's backdrop, a step beyond bg
 * Corner radius is not a token: there is none, windows and widgets are square.
 */
#ifndef FLPALETTE_H
#define FLPALETTE_H

#include <stdint.h>
#include <string.h>

struct flpal {
	uint32_t bg, panel, raised, line, text, dim, accent, on_accent, select, strong, term_bg, err, back;
	int dark;
};

#define FLPAL_ACCENT_COUNT 6
static const struct { const char *id, *name; uint32_t dark, light; } flpal_accents[FLPAL_ACCENT_COUNT] = {
	{"green",  "Green",  0x3f9b63, 0x2c7a4b},
	{"teal",   "Teal",   0x35a09b, 0x1f7773},
	{"blue",   "Blue",   0x4d8fd0, 0x2f6cae},
	{"amber",  "Amber",  0xd0a032, 0x8a6300},
	{"orange", "Orange", 0xe0793f, 0xa84c12},
	{"rose",   "Rose",   0xd4678c, 0xa43f63},
};

static inline uint32_t flpal_opaque(uint32_t rgb) { return 0xff000000u | (rgb & 0xffffffu); }

/* a + (b - a) * t / 255, per channel, opaque */
static inline uint32_t flpal_mix(uint32_t a, uint32_t b, int t)
{
	uint32_t o = 0xff000000u;
	for (int s = 0; s <= 16; s += 8) {
		int ca = (int)(a >> s & 255), cb = (int)(b >> s & 255);
		o |= (uint32_t)(ca + (cb - ca) * t / 255) << s;
	}
	return o;
}

/* relative luminance 0..1000 (sRGB, WCAG); needs libm */
static inline unsigned flpal_lum(uint32_t c)
{
	static const unsigned w[3] = {2126, 7152, 722};
	unsigned l = 0;
	for (int i = 0; i < 3; i++) {
		float x = (float)((c >> (16 - 8 * i)) & 255) / 255.0f;
		x = x <= 0.04045f ? x / 12.92f : __builtin_powf((x + 0.055f) / 1.055f, 2.4f);
		l += w[i] * (unsigned)(x * 1000.0f);
	}
	return l / 10000;
}

/* the text colour of the two with the better contrast on a fill */
static inline uint32_t flpal_on_fill(uint32_t fill, uint32_t dark_text, uint32_t light_text)
{
	unsigned l = flpal_lum(fill) + 50, d = flpal_lum(dark_text) + 50, w = flpal_lum(light_text) + 50;
	unsigned cd = l > d ? l * 100 / d : d * 100 / l, cw = l > w ? l * 100 / w : w * 100 / l;
	return cd >= cw ? dark_text : light_text;
}

/* "#RRGGBB" or "RRGGBB": 0 and *out set, else -1 */
static inline int flpal_parse_hex(const char *s, uint32_t *out)
{
	if (*s == '#')
		s++;
	if (strlen(s) != 6)
		return -1;
	uint32_t v = 0;
	for (int i = 0; i < 6; i++) {
		char c = s[i];
		int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
		        c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
		if (d < 0)
			return -1;
		v = v << 4 | (uint32_t)d;
	}
	*out = flpal_opaque(v);
	return 0;
}

/* The accent for a desktop.conf value (an id or a hex colour, NULL/empty/unknown: green), as
 * shown on a dark or a light scheme: a hex colour is used as is on dark and darkened on light. */
static inline uint32_t flpal_accent(const char *v, int dark)
{
	for (int i = 0; v && i < FLPAL_ACCENT_COUNT; i++)
		if (!strcmp(v, flpal_accents[i].id))
			return flpal_opaque(dark ? flpal_accents[i].dark : flpal_accents[i].light);
	uint32_t c;
	if (v && flpal_parse_hex(v, &c) == 0)
		return dark ? c : flpal_mix(c, 0xff000000u, 56);
	return flpal_opaque(dark ? flpal_accents[0].dark : flpal_accents[0].light);
}

/* the scheme without an accent choice: green */
static inline void flpal_base(struct flpal *p, int dark)
{
	if (dark) {
		*p = (struct flpal){
			.bg = 0xff1d2125u, .panel = 0xff272c33u, .raised = 0xff343a43u, .line = 0xff3a4048u,
			.text = 0xffd8dce0u, .dim = 0xff8a9199u, .accent = 0xff3f9b63u, .on_accent = 0xff0e1a13u,
			.select = 0xff2f4a3bu, .strong = 0xfff0f2f4u, .term_bg = 0xff16191cu, .err = 0xffe06c6cu,
			.back = 0xff15181cu, .dark = 1,
		};
	} else {
		*p = (struct flpal){
			.bg = 0xffe1e5e9u, .panel = 0xfff5f6f8u, .raised = 0xffffffffu, .line = 0xffc4cad2u,
			.text = 0xff1d2125u, .dim = 0xff4f5761u, .accent = 0xff2c7a4bu, .on_accent = 0xffffffffu,
			.select = 0xffcfe6d8u, .strong = 0xff14181bu, .term_bg = 0xff16191cu, .err = 0xffb02f2fu,
			.back = 0xffcdd2d8u, .dark = 0,
		};
	}
}

/* the scheme with the user's accent (accent= from desktop.conf, may be NULL): the default
 * green leaves the tokens exactly as flpal_base has them */
static inline void flpal_make(struct flpal *p, int dark, const char *accent)
{
	flpal_base(p, dark);
	if (!accent || !*accent || !strcmp(accent, "green"))
		return;
	uint32_t a = flpal_accent(accent, dark);
	p->accent = a;
	p->on_accent = dark ? flpal_on_fill(a, 0xff0e1418u, 0xfff0f2f4u) : flpal_on_fill(a, 0xff0e1418u, 0xffffffffu);
	p->select = flpal_mix(p->panel, a, dark ? 70 : 56);
}

#endif
