/* text.c -- fonts: fontconfig finds them, FreeType rasterises, HarfBuzz shapes
 * SPDX-License-Identifier: MIT
 *
 * Glyph bitmaps are cached per font (8-bit coverage). Advances are rounded
 * to whole pixels so stems stay crisp; light hinting only moves things
 * vertically. A code point the font lacks comes from a fontconfig fallback.
 */
#include "internal.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>
#include <hb-ft.h>
#include <hb.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct glyph {
	uint32_t gid; /* + 1, 0 = empty slot */
	int w, h, left, top, adv;
	uint8_t *bits;
};

#define CACHE_SLOTS 2048
#define CACHE_MAX 1400
#define NFALL 4

struct fl_font {
	FT_Face face;
	hb_font_t *hb;
	char *family;
	int weight, italic;
	float px;
	int ascent, descent, cell;
	struct glyph *cache;
	int ncache;
	fl_font *fall[NFALL];
	int nfall;
	uint32_t miss[16]; /* code points no font has */
	int nmiss;
};

static FT_Library ft_lib;
static hb_buffer_t *hbuf;
static int text_ready;
static uint8_t lut_light[256], lut_dark[256];

int fl_text_init(void)
{
	if (text_ready)
		return 0;
	if (FT_Init_FreeType(&ft_lib))
		return -1;
	FcInit();
	hbuf = hb_buffer_create();
	for (int i = 0; i < 256; i++) {
		lut_dark[i] = (uint8_t)(255 * powf(i / 255.0f, 1.0f) + 0.5f);
		/* light text on a dark ground looks thin when blended linearly */
		lut_light[i] = (uint8_t)(255 * powf(i / 255.0f, 0.78f) + 0.5f);
	}
	text_ready = 1;
	return 0;
}

static fl_font *open_file(const char *file, int index, const char *family, int weight, int italic,
                          float px)
{
	if (fl_text_init() < 0)
		return NULL;
	fl_font *f = calloc(1, sizeof(*f));
	if (!f)
		return NULL;
	if (FT_New_Face(ft_lib, file, index, &f->face)) {
		free(f);
		return NULL;
	}
	FT_Set_Char_Size(f->face, 0, (FT_F26Dot6)(px * 64 + 0.5f), 72, 72);
	f->hb = hb_ft_font_create_referenced(f->face);
	hb_ft_font_set_load_flags(f->hb, FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP);
	f->family = strdup(family);
	f->weight = weight;
	f->italic = italic;
	f->px = px;
	FT_Size_Metrics *m = &f->face->size->metrics;
	f->ascent = (int)ceilf(m->ascender / 64.0f);
	f->descent = (int)ceilf(-m->descender / 64.0f);
	f->cache = calloc(CACHE_SLOTS, sizeof(struct glyph));
	if (!f->cache || !f->family) {
		fl_font_close(f);
		return NULL;
	}
	/* the cell of a monospace font: advance of "0" */
	if (!FT_Load_Char(f->face, '0', FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP))
		f->cell = (int)((f->face->glyph->advance.x + 32) >> 6);
	return f;
}

fl_font *fl_font_open(const char *family, int weight, int italic, float px)
{
	if (fl_text_init() < 0)
		return NULL;
	FcPattern *pat = FcPatternCreate();
	FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)family);
	FcPatternAddInteger(pat, FC_WEIGHT, FcWeightFromOpenType(weight));
	FcPatternAddInteger(pat, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);
	FcResult res;
	FcPattern *m = FcFontMatch(NULL, pat, &res);
	fl_font *f = NULL;
	FcChar8 *file;
	int index = 0;
	if (m && FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch) {
		FcPatternGetInteger(m, FC_INDEX, 0, &index);
		f = open_file((const char *)file, index & 0xffff, family, weight, italic, px);
	}
	if (m)
		FcPatternDestroy(m);
	FcPatternDestroy(pat);
	return f;
}

fl_font *fl_font_ui(int weight, float px)
{
	return fl_font_open("IBM Plex Sans", weight, 0, px);
}

fl_font *fl_font_mono(int weight, float px)
{
	return fl_font_open("IBM Plex Mono", weight, 0, px);
}

void fl_font_close(fl_font *f)
{
	if (!f)
		return;
	for (int i = 0; i < f->nfall; i++)
		fl_font_close(f->fall[i]);
	if (f->cache) {
		for (int i = 0; i < CACHE_SLOTS; i++)
			free(f->cache[i].bits);
		free(f->cache);
	}
	if (f->hb)
		hb_font_destroy(f->hb);
	if (f->face)
		FT_Done_Face(f->face);
	free(f->family);
	free(f);
}

int fl_font_ascent(const fl_font *f) { return f->ascent; }
int fl_font_height(const fl_font *f) { return f->ascent + f->descent; }
int fl_font_cell(const fl_font *f) { return f->cell; }

/* ---- glyph cache ------------------------------------------------------------- */

static struct glyph *get_glyph(fl_font *f, uint32_t gid)
{
	uint32_t h = (gid * 2654435761u) % CACHE_SLOTS;
	for (;;) {
		struct glyph *g = &f->cache[h];
		if (g->gid == gid + 1)
			return g;
		if (!g->gid)
			break;
		h = (h + 1) % CACHE_SLOTS;
	}
	if (f->ncache >= CACHE_MAX) { /* full: start over */
		for (int i = 0; i < CACHE_SLOTS; i++)
			free(f->cache[i].bits);
		memset(f->cache, 0, CACHE_SLOTS * sizeof(struct glyph));
		f->ncache = 0;
		return get_glyph(f, gid);
	}
	struct glyph *g = &f->cache[h];
	f->ncache++;
	g->gid = gid + 1;
	if (FT_Load_Glyph(f->face, gid, FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP | FT_LOAD_RENDER))
		return g;
	FT_GlyphSlot s = f->face->glyph;
	g->w = (int)s->bitmap.width;
	g->h = (int)s->bitmap.rows;
	g->left = s->bitmap_left;
	g->top = s->bitmap_top;
	g->adv = (int)((s->advance.x + 32) >> 6);
	if (g->w && g->h && s->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
		g->bits = malloc((size_t)g->w * g->h);
		if (g->bits)
			for (int y = 0; y < g->h; y++)
				memcpy(g->bits + (size_t)y * g->w, s->bitmap.buffer + (size_t)y * s->bitmap.pitch,
				       (size_t)g->w);
	}
	return g;
}

static void draw_glyph(fl_canvas *cv, fl_font *f, uint32_t gid, int x, int base, fl_color c)
{
	if (!cv->px)
		return;
	struct glyph *g = get_glyph(f, gid);
	if (!g->bits)
		return;
	const uint8_t *lut = ((c >> 16 & 255) + (c >> 8 & 255) * 2 + (c & 255)) / 4 > 128 ? lut_light : lut_dark;
	int gx = x + g->left, gy = base - g->top;
	fl_rect r = fl_intersect((fl_rect){gx, gy, g->w, g->h}, cv->clip);
	for (int y = r.y; y < r.y + r.h; y++) {
		const uint8_t *src = g->bits + (size_t)(y - gy) * g->w + (r.x - gx);
		uint32_t *dst = cv->px + (size_t)y * cv->stride + r.x;
		for (int i = 0; i < r.w; i++)
			if (src[i])
				dst[i] = fl_over(dst[i], c, lut[src[i]]);
	}
}

/* ---- fallback fonts ------------------------------------------------------------ */

static uint32_t utf8_next(const char **sp, const char *end)
{
	const unsigned char *s = (const unsigned char *)*sp;
	uint32_t c = s[0];
	int n = 0;
	if (c < 0x80) {
		n = 1;
	} else if ((c & 0xe0) == 0xc0) {
		c &= 0x1f, n = 2;
	} else if ((c & 0xf0) == 0xe0) {
		c &= 0x0f, n = 3;
	} else if ((c & 0xf8) == 0xf0) {
		c &= 0x07, n = 4;
	} else {
		*sp += 1;
		return 0xfffd;
	}
	if (*sp + n > end) {
		*sp = end;
		return 0xfffd;
	}
	for (int i = 1; i < n; i++) {
		if ((s[i] & 0xc0) != 0x80) {
			*sp += i;
			return 0xfffd;
		}
		c = c << 6 | (s[i] & 0x3f);
	}
	*sp += n;
	return c;
}

static fl_font *make_fallback(fl_font *f, uint32_t cp)
{
	for (int i = 0; i < f->nmiss; i++)
		if (f->miss[i] == cp)
			return NULL;
	FcPattern *pat = FcPatternCreate();
	FcCharSet *cs = FcCharSetCreate();
	FcCharSetAddChar(cs, cp);
	FcPatternAddCharSet(pat, FC_CHARSET, cs);
	FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)f->family);
	FcPatternAddInteger(pat, FC_WEIGHT, FcWeightFromOpenType(f->weight));
	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);
	FcResult res;
	FcPattern *m = FcFontMatch(NULL, pat, &res);
	fl_font *nf = NULL;
	FcChar8 *file;
	FcCharSet *mcs;
	int index = 0;
	if (m && FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch &&
	    FcPatternGetCharSet(m, FC_CHARSET, 0, &mcs) == FcResultMatch && FcCharSetHasChar(mcs, cp)) {
		FcPatternGetInteger(m, FC_INDEX, 0, &index);
		nf = open_file((const char *)file, index & 0xffff, f->family, f->weight, f->italic, f->px);
	}
	if (m)
		FcPatternDestroy(m);
	FcCharSetDestroy(cs);
	FcPatternDestroy(pat);
	if (nf && f->nfall < NFALL) {
		f->fall[f->nfall++] = nf;
		return nf;
	}
	if (nf)
		fl_font_close(nf);
	f->miss[f->nmiss++ % 16] = cp;
	return NULL;
}

/* The font that has cp: f itself, or one of its fallbacks. */
static fl_font *font_for(fl_font *f, uint32_t cp)
{
	if (cp < 0x80 || FT_Get_Char_Index(f->face, cp))
		return f;
	for (int i = 0; i < f->nfall; i++)
		if (FT_Get_Char_Index(f->fall[i]->face, cp))
			return f->fall[i];
	fl_font *nf = make_fallback(f, cp);
	return nf ? nf : f;
}

/* ---- shaping and drawing --------------------------------------------------------- */

static int text_run(fl_canvas *cv, fl_font *f, int x, int y, fl_color c, const char *s, int len,
                    int spacing64, int draw)
{
	if (!f || !s)
		return 0;
	if (len < 0)
		len = (int)strlen(s);
	const char *end = s + len, *p = s;
	int base = y + f->ascent;
	long pen = (long)x * 64;
	while (p < end) {
		/* a run: consecutive code points that come from one font */
		const char *rs = p, *q = p;
		uint32_t cp = utf8_next(&q, end);
		fl_font *rf = font_for(f, cp);
		p = q;
		while (p < end) {
			const char *q2 = p;
			uint32_t c2 = utf8_next(&q2, end);
			int combining = (c2 >= 0x300 && c2 < 0x370) || c2 == 0x200d || (c2 >= 0xfe00 && c2 < 0xfe10);
			if (!combining && font_for(f, c2) != rf)
				break;
			p = q2;
		}
		hb_buffer_clear_contents(hbuf);
		hb_buffer_add_utf8(hbuf, rs, (int)(p - rs), 0, (int)(p - rs));
		hb_buffer_guess_segment_properties(hbuf);
		hb_shape(rf->hb, hbuf, NULL, 0);
		unsigned n;
		hb_glyph_info_t *gi = hb_buffer_get_glyph_infos(hbuf, &n);
		hb_glyph_position_t *gp = hb_buffer_get_glyph_positions(hbuf, &n);
		for (unsigned i = 0; i < n; i++) {
			if (draw)
				draw_glyph(cv, rf, gi[i].codepoint, (int)((pen + 32) >> 6) + ((gp[i].x_offset + 32) >> 6),
				           base - ((gp[i].y_offset + 32) >> 6), c);
			if (spacing64)
				pen += gp[i].x_advance + (gp[i].x_advance ? spacing64 : 0);
			else
				pen += (gp[i].x_advance + 32) & ~63L;
		}
	}
	return (int)(((pen + 32) >> 6) - x);
}

int fl_text_width(fl_font *f, const char *s, int len)
{
	return text_run(NULL, f, 0, 0, 0, s, len, 0, 0);
}

int fl_text(fl_canvas *cv, fl_font *f, int x, int y, fl_color c, const char *s, int len)
{
	return text_run(cv, f, x, y, c, s, len, 0, 1);
}

int fl_text_ex(fl_canvas *cv, fl_font *f, int x, int y, fl_color c, const char *s, int len, int sp)
{
	return text_run(cv, f, x, y, c, s, len, sp, 1);
}

void fl_text_in(fl_canvas *cv, fl_font *f, fl_rect r, int align, int pad, fl_color c, const char *s)
{
	if (!cv->px || !s || !*s)
		return;
	fl_rect save = cv->clip;
	fl_clip(cv, r);
	int w = fl_text_width(f, s, -1);
	int x = r.x + pad;
	if (align == FL_CENTER)
		x = r.x + (r.w - w) / 2;
	else if (align == FL_RIGHT)
		x = r.x + r.w - pad - w;
	int fh = fl_font_height(f);
	fl_text(cv, f, x, r.y + (r.h - fh) / 2, c, s, -1);
	cv->clip = save;
}

int fl_glyph(fl_canvas *cv, fl_font *f, int x, int y, fl_color c, uint32_t cp)
{
	fl_font *rf = font_for(f, cp);
	uint32_t gid = FT_Get_Char_Index(rf->face, cp);
	if (!gid && cp != ' ')
		gid = FT_Get_Char_Index(f->face, 0xfffd);
	if (cp == ' ' || cp == 0)
		return f->cell;
	draw_glyph(cv, rf, gid, x, y + f->ascent, c);
	return f->cell;
}

int fl_text_fit(fl_font *f, const char *s, int w, char *out, size_t cap)
{
	size_t n = strlen(s);
	if (fl_text_width(f, s, -1) <= w) {
		if (n >= cap)
			n = cap - 1;
		memcpy(out, s, n);
		out[n] = 0;
		return (int)n;
	}
	int ew = fl_text_width(f, "\xe2\x80\xa6", -1);
	size_t best = 0, i = 0;
	while (i < n) {
		const char *q = s + i;
		utf8_next(&q, s + n);
		size_t j = (size_t)(q - s);
		if (fl_text_width(f, s, (int)j) + ew > w)
			break;
		best = j;
		i = j;
	}
	if (best + 4 > cap)
		best = cap > 4 ? cap - 4 : 0;
	memcpy(out, s, best);
	memcpy(out + best, "\xe2\x80\xa6", 4);
	return (int)best + 3;
}
