/* genlockfont -- writes lockfont.h: the glyphs flcomp draws on its own while the screen is
 * locked and the lock program is not running (lock.c). Run at development time, the output
 * is checked in:  cc genlockfont.c $(pkg-config --cflags --libs freetype2) -o genlockfont &&
 * ./genlockfont /usr/share/fonts/ibm-plex/IBMPlexSans-Regular.otf > ../lockfont.h
 * Two sizes of one face, ASCII 32..126, 2 bits per pixel (alpha 0, 85, 170, 255), packed
 * four to a byte, row after row of one cell.
 * SPDX-License-Identifier: MIT */
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void emit(FT_Face face, const char *name, int px, int cw, int ch, int base, const char *set)
{
	int n = set ? (int)strlen(set) : 95;
	FT_Set_Pixel_Sizes(face, 0, (FT_UInt)px);
	printf("/* %s: %d px, cell %d x %d, baseline %d */\n", name, px, cw, ch, base);
	printf("#define %s_W %d\n#define %s_H %d\n#define %s_BYTES %d\n", name, cw, name, ch, name, (cw * ch + 3) / 4);
	printf("static const unsigned char %s_DATA[%d][%d] = {\n", name, n, (cw * ch + 3) / 4);
	for (int k0 = 0; k0 < n; k0++) {
		int c = set ? (unsigned char)set[k0] : 32 + k0;
		unsigned char cell[64 * 64] = {0};
		int adv = 0;
		if (!FT_Load_Char(face, (FT_ULong)c, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL)) {
			FT_GlyphSlot g = face->glyph;
			adv = (int)(g->advance.x >> 6);
			for (int y = 0; y < (int)g->bitmap.rows; y++)
				for (int x = 0; x < (int)g->bitmap.width; x++) {
					int cx = g->bitmap_left + x, cy = base - g->bitmap_top + y;
					if (cx < 0 || cy < 0 || cx >= cw || cy >= ch)
						continue;
					cell[cy * cw + cx] = g->bitmap.buffer[y * g->bitmap.pitch + x];
				}
		}
		printf("\t{ /* '%c' adv %d */ ", c == '\\' || c == '*' || c == '/' ? '.' : c, adv);
		for (int i = 0; i < (cw * ch + 3) / 4; i++) {
			int b = 0;
			for (int k = 0; k < 4; k++) {
				int idx = i * 4 + k, v = idx < cw * ch ? cell[idx] : 0;
				int q = (v + 42) / 85;	/* 0..3 */
				b |= (q > 3 ? 3 : q) << (6 - 2 * k);
			}
			printf("%d,", b);
		}
		printf(" },\n");
	}
	printf("};\n");
	if (set)
		printf("#define %s_SET \"%s\"\n", name, set);
	printf("static const unsigned char %s_ADV[%d] = {", name, n);
	for (int k0 = 0; k0 < n; k0++) {
		int c = set ? (unsigned char)set[k0] : 32 + k0;
		int adv = 0;
		if (!FT_Load_Char(face, (FT_ULong)c, FT_LOAD_DEFAULT))
			adv = (int)(face->glyph->advance.x >> 6);
		printf("%d,", adv);
	}
	printf("};\n\n");
}

int main(int argc, char **argv)
{
	FT_Library lib;
	FT_Face face;
	if (argc < 2 || FT_Init_FreeType(&lib) || FT_New_Face(lib, argv[1], 0, &face)) {
		fprintf(stderr, "usage: genlockfont FONT.otf > lockfont.h\n");
		return 1;
	}
	printf("/* lockfont.h -- made by tools/genlockfont.c, do not edit. SPDX-License-Identifier: MIT */\n\n");
	emit(face, "LF_BODY", 18, 20, 26, 20, NULL);
	emit(face, "LF_HEAD", 44, 48, 62, 48, "Screnlokd ");
	return 0;
}
