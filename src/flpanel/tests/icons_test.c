/* icons_test.c -- the icon lookup (icons.c) against a fake tree, no display.
 * SPDX-License-Identifier: MIT
 *
 * Usage: icons_test SCRATCHDIR (an empty directory). HOME and FLPANEL_ROOT point inside it, so the
 * host's own icons and .desktop files are never looked at. Build and run: make icons-test.
 */
#include <errno.h>
#include <png.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <flui.h>

#include "../icons.h"

static int fails;

#define CHECK(cond, what)                                   \
	do {                                                \
		if (cond)                                   \
			printf("ok   %s\n", what);          \
		else {                                      \
			printf("FAIL %s\n", what);          \
			fails++;                            \
		}                                           \
	} while (0)

static char base[512];

static void mkdir_p(const char *path)
{
	char buf[600];
	snprintf(buf, sizeof buf, "%s", path);
	for (char *s = buf + 1; *s; s++)
		if (*s == '/') {
			*s = 0;
			if (mkdir(buf, 0755) && errno != EEXIST)
				exit(2);
			*s = '/';
		}
	if (mkdir(buf, 0755) && errno != EEXIST)
		exit(2);
}

/* a file under the scratch tree: the directory is made, the content written */
static void put(const char *rel, const char *text)
{
	char path[700], dir[700];
	snprintf(path, sizeof path, "%s/%s", base, rel);
	snprintf(dir, sizeof dir, "%s", path);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = 0;
		mkdir_p(dir);
	}
	FILE *f = fopen(path, "w");
	if (!f)
		exit(2);
	fputs(text, f);
	fclose(f);
}

/* a w x h PNG, every pixel the same: gray (RGB) when gray is set, else opaque r,g,b */
static void put_png(const char *rel, int w, int h, int gray, uint8_t r, uint8_t g, uint8_t b)
{
	char path[700], dir[700];
	snprintf(path, sizeof path, "%s/%s", base, rel);
	snprintf(dir, sizeof dir, "%s", path);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = 0;
		mkdir_p(dir);
	}
	FILE *f = fopen(path, "wb");
	if (!f)
		exit(2);
	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_infop info = png_create_info_struct(png);
	if (setjmp(png_jmpbuf(png)))
		exit(2);
	png_init_io(png, f);
	png_set_IHDR(png, info, w, h, 8, gray ? PNG_COLOR_TYPE_GRAY : PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
		     PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_write_info(png, info);
	uint8_t *row = malloc((size_t)w * 3);
	for (int x = 0; x < w; x++) {
		if (gray)
			row[x] = r;
		else {
			row[x * 3] = r;
			row[x * 3 + 1] = g;
			row[x * 3 + 2] = b;
		}
	}
	for (int y = 0; y < h; y++)
		png_write_row(png, row);
	free(row);
	png_write_end(png, NULL);
	png_destroy_write_struct(&png, &info);
	fclose(f);
}

static uint32_t at(const uint32_t *px, int stride, int x, int y)
{
	return px[y * stride + x];
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: icons_test SCRATCHDIR\n");
		return 2;
	}
	snprintf(base, sizeof base, "%s", argv[1]);

	char home[600], root[600], conf[600];
	snprintf(home, sizeof home, "%s/home", base);
	snprintf(root, sizeof root, "%s/root", base);
	snprintf(conf, sizeof conf, "%s/no-such-desktop.conf", base);
	setenv("HOME", home, 1);
	unsetenv("XDG_DATA_HOME");
	setenv("FLPANEL_ROOT", root, 1);
	setenv("FLUI_CONF", conf, 1);

	/* the user's .desktop: app_id ftest-app -> Papirus icon ftest-icon (only at 32 px) */
	put("home/.local/share/applications/ftest-app.desktop",
	    "[Desktop Entry]\nName=Test\nExec=true\nIcon=ftest-icon\n");
	put_png("home/.local/share/icons/Papirus/32x32/apps/ftest-icon.png", 32, 32, 0, 255, 0, 0);
	/* hicolor only, 48 px: found for a 16 px icon by going up */
	put("root/usr/share/applications/ftest-hc.desktop", "[Desktop Entry]\nIcon=ftest-hc\n");
	put_png("root/usr/share/icons/hicolor/48x48/apps/ftest-hc.png", 48, 48, 0, 0, 0, 255);
	/* a Flatpak export: 128 px only, for a 32 px icon going down; the Icon= has the app id */
	put("root/var/lib/flatpak/exports/share/applications/org.test.Flat.desktop",
	    "[Desktop Entry]\nIcon=org.test.Flat\n[Desktop Action new]\nIcon=other\n");
	put_png("root/var/lib/flatpak/exports/share/icons/hicolor/128x128/apps/org.test.Flat.png", 128, 128, 0,
		0, 255, 0);
	/* an absolute Icon= (a gray PNG, the gray-to-RGB path) and an entry without Icon= */
	put_png("abs/gray.png", 8, 8, 1, 128, 0, 0);
	char abs[600], line[700];
	snprintf(abs, sizeof abs, "%s/abs/gray.png", base);
	snprintf(line, sizeof line, "[Desktop Entry]\nIcon=%s\n", abs);
	put("root/usr/share/applications/ftest-abs.desktop", line);
	put("root/usr/share/applications/ftest-noicon.desktop", "[Desktop Entry]\nName=None\n");

	static uint32_t pix[64 * 64];
	memset(pix, 0, sizeof pix);
	fl_canvas cv = {.px = pix, .w = 64, .h = 64, .stride = 64, .clip = {0, 0, 64, 64}};

	CHECK(icon_draw_app(&cv, "ftest-app", 0, 0, 16) == 1, "user .desktop Icon= drawn at 16 px");
	CHECK(at(pix, 64, 8, 8) == 0xffff0000u, "Papirus 32 px scaled to 16: red at the centre");
	CHECK(at(pix, 64, 16, 0) == 0u, "nothing drawn outside the 16 px square");

	CHECK(icon_draw_app(&cv, "ftest-hc", 16, 0, 16) == 1, "hicolor 48 px found for a 16 px icon");
	CHECK(at(pix, 64, 24, 8) == 0xff0000ffu, "hicolor icon: blue at the centre");

	CHECK(icon_draw_app(&cv, "org.test.Flat", 32, 0, 32) == 1, "Flatpak export icon (128 px) at 32 px");
	CHECK(at(pix, 64, 48, 16) == 0xff00ff00u, "Flatpak icon: green at the centre");

	CHECK(icon_draw_name(&cv, abs, 0, 32, 16) == 1, "absolute Icon= path drawn");
	CHECK(at(pix, 64, 8, 40) == 0xff808080u, "gray PNG converted to RGB: grey 128 at the centre");

	CHECK(icon_draw_app(&cv, "ftest-noicon", 48, 32, 16) == 0, "a .desktop without Icon= gives 0 (glyph)");
	CHECK(icon_draw_app(&cv, "no-such-app", 48, 32, 16) == 0, "an unknown app_id gives 0 (glyph)");
	CHECK(icon_draw_app(&cv, "../ftest-app", 48, 32, 16) == 0, "an app_id with a slash is refused");
	CHECK(icon_draw_app(&cv, "ftest-app", 0, 0, 64) == 0, "sizes outside 16..48 are refused");

	fl_canvas dry = {.px = NULL, .w = 64, .h = 64, .stride = 64, .clip = {0, 0, 64, 64}};
	CHECK(icon_draw_app(&dry, "ftest-app", 0, 0, 16) == 0, "a dry pass (no pixels) draws nothing");

	icons_flush();
	memset(pix, 0, sizeof pix);
	CHECK(icon_draw_app(&cv, "ftest-app", 0, 0, 16) == 1, "after icons_flush the lookup runs again");
	CHECK(at(pix, 64, 8, 8) == 0xffff0000u, "and gives the same pixels");

	printf("%s (%d failed)\n", fails ? "FAILED" : "all icon tests passed", fails);
	return fails ? 1 : 0;
}
