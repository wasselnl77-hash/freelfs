/* icons.c -- app icons: app_id -> .desktop file -> Icon= -> PNG in the icon theme
 * SPDX-License-Identifier: MIT
 *
 * .desktop files are looked at in ~/.local/share/applications, Flatpak's exports
 * (~/.local/share/flatpak/exports/share, /var/lib/flatpak/exports/share) and /usr/share/applications;
 * the first file named app_id.desktop wins. Its Icon= is looked up in the icon theme: the theme
 * named by icon_theme= in desktop.conf (Papirus by default), then hicolor, in each icon directory
 * ($XDG_DATA_HOME/icons, the Flatpak exports, /usr/share/icons), then /usr/share/pixmaps. A theme
 * size at or above the wanted one is taken first, the nearest smaller one otherwise. An absolute
 * Icon= is used as it is.
 *
 * Only PNG is decoded (libpng, which the panel links anyway). SVG is not rendered: there is no
 * SVG renderer in the system, so an SVG-only icon falls back to the glyph. Icon themes should ship
 * PNG at 16, 22, 24, 32 and 48 px (the recipe makes them).
 *
 * Memory: 48 decoded icons at most (size x size x 4 bytes, 9 KB at 48 px), 64 app_id -> Icon=
 * entries, misses cached too (a lookup is never repeated per frame). The menu calls icons_flush
 * when it reloads. FLPANEL_ROOT replaces "/" in the system directories (tests only).
 */
#include <ctype.h>
#include <png.h>
#include <stdarg.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <flui.h>

#include "icons.h"

#define PX_SLOTS 48
#define APP_SLOTS 64
#define NAME_LEN 128
#define APP_LEN 96
#define WANT_MIN 16
#define WANT_MAX 48
#define SRC_MAX 256 /* a source PNG larger than this is skipped (memory) */

static const int theme_sizes[] = {16, 22, 24, 32, 48, 64, 128, 256};
static const char *const contexts[] = {"apps", "categories", "places", "mimetypes", "devices"};
static const char *const default_theme = "Papirus";

/* formats a path into buf; 0 when it did not fit */
static int path_fmt(char *buf, size_t n, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int k = vsnprintf(buf, n, fmt, ap);
	va_end(ap);
	return k >= 0 && (size_t)k < n;
}

#define PATH(buf, ...) path_fmt((buf), sizeof(buf), __VA_ARGS__)

struct px_slot {
	char name[NAME_LEN];
	int size;
	uint32_t *px; /* NULL: looked up, not found */
	int used;
};

struct app_slot {
	char app[APP_LEN];
	char icon[NAME_LEN]; /* "" when the app has no icon */
	int used;
};

static struct px_slot px_tab[PX_SLOTS];
static int px_next;
static struct app_slot app_tab[APP_SLOTS];
static int app_next;

/* The directories, in search order. */
struct where {
	char apps[4][512];  /* .desktop files */
	char icons[4][512]; /* icon theme base directories */
	char pixmaps[512];
};

static void where_init(struct where *w)
{
	const char *root = getenv("FLPANEL_ROOT");
	const char *home = getenv("HOME");
	const char *xdh = getenv("XDG_DATA_HOME");
	char data_home[384], flat_user[512], flat_sys[512], usr[512];
	if (!root)
		root = "";
	if (!home)
		home = "";
	if (xdh && *xdh)
		snprintf(data_home, sizeof data_home, "%s", xdh);
	else
		snprintf(data_home, sizeof data_home, "%s/.local/share", home);
	snprintf(flat_user, sizeof flat_user, "%s/.local/share/flatpak/exports/share", home);
	snprintf(flat_sys, sizeof flat_sys, "%s/var/lib/flatpak/exports/share", root);
	snprintf(usr, sizeof usr, "%s/usr/share", root);

	PATH(w->apps[0], "%s/applications", data_home);
	PATH(w->apps[1], "%s/applications", flat_user);
	PATH(w->apps[2], "%s/applications", flat_sys);
	PATH(w->apps[3], "%s/applications", usr);

	PATH(w->icons[0], "%s/icons", data_home);
	PATH(w->icons[1], "%s/icons", flat_user);
	PATH(w->icons[2], "%s/icons", flat_sys);
	PATH(w->icons[3], "%s/icons", usr);

	PATH(w->pixmaps, "%s/pixmaps", usr);
}

/* A usable app_id: a file name with no slash, no leading dot. */
static int app_ok(const char *a)
{
	if (!*a || *a == '.' || strlen(a) >= APP_LEN)
		return 0;
	for (; *a; a++)
		if (!isalnum((unsigned char)*a) && !strchr("._-", *a))
			return 0;
	return 1;
}

/* Icon= of app.desktop in the first directory that has the file. Returns 1 when that file has one. */
static int desktop_icon(const struct where *w, const char *app, char *out, size_t n)
{
	for (int i = 0; i < 4; i++) {
		char path[600];
		if (!PATH(path, "%s/%s.desktop", w->apps[i], app))
			continue;
		FILE *f = fopen(path, "r");
		if (!f)
			continue;
		char line[512];
		int in_entry = 0, found = 0;
		while (fgets(line, sizeof line, f)) {
			size_t len = strlen(line);
			while (len && (line[len - 1] == '\n' || line[len - 1] == '\r' || line[len - 1] == ' '))
				line[--len] = 0;
			if (line[0] == '[') {
				in_entry = !strcmp(line, "[Desktop Entry]");
				continue;
			}
			if (in_entry && !strncmp(line, "Icon=", 5) && line[5]) {
				size_t vl = strlen(line + 5);
				if (vl >= n)
					vl = n - 1;
				memcpy(out, line + 5, vl);
				out[vl] = 0;
				found = vl > 0;
				break;
			}
		}
		fclose(f);
		return found; /* the first file with that name decides */
	}
	return 0;
}

static const char *app_icon(const char *app)
{
	for (int i = 0; i < APP_SLOTS; i++)
		if (app_tab[i].used && !strcmp(app_tab[i].app, app))
			return app_tab[i].icon;
	struct app_slot *s = &app_tab[app_next];
	app_next = (app_next + 1) % APP_SLOTS;
	s->icon[0] = 0;
	snprintf(s->app, sizeof s->app, "%s", app);
	s->used = 1;
	if (app_ok(app)) {
		struct where w;
		where_init(&w);
		desktop_icon(&w, app, s->icon, sizeof s->icon);
	}
	return s->icon;
}

/* The PNG file for an Icon= value: the best size of the theme for want px. 1 when found. */
static int icon_file(const char *name, int want, char *out, size_t n)
{
	if (name[0] == '/') {
		if (!path_fmt(out, n, "%s", name))
			return 0;
		return access(out, R_OK) == 0;
	}
	if (strchr(name, '/') || strstr(name, ".."))
		return 0;
	struct where w;
	where_init(&w);
	/* sizes: at or above want, ascending; then the smaller ones, largest first */
	int ord[8], no = 0;
	for (int i = 0; i < 8; i++)
		if (theme_sizes[i] >= want)
			ord[no++] = theme_sizes[i];
	for (int i = 7; i >= 0; i--)
		if (theme_sizes[i] < want)
			ord[no++] = theme_sizes[i];

	const char *themes[2];
	themes[0] = fl_conf_get("icon_theme", default_theme);
	themes[1] = "hicolor";
	for (int t = 0; t < 2; t++) {
		if (!themes[t][0] || strchr(themes[t], '/'))
			continue;
		for (int b = 0; b < 4; b++)
			for (int k = 0; k < no; k++)
				for (size_t c = 0; c < sizeof contexts / sizeof contexts[0]; c++) {
					char p[600];
					int s = ord[k];
					if (!PATH(p, "%s/%s/%dx%d/%s/%s.png", w.icons[b], themes[t], s, s, contexts[c], name))
						continue;
					if (access(p, R_OK) == 0) {
						snprintf(out, n, "%s", p);
						return 1;
					}
				}
	}
	if (path_fmt(out, n, "%s/%s.png", w.pixmaps, name) && access(out, R_OK) == 0)
		return 1;
	return 0;
}

/* Decodes a PNG to want x want ARGB (straight alpha), nearest-neighbour. NULL on any error. */
static uint32_t *load_png(const char *path, int want)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_infop info = png ? png_create_info_struct(png) : NULL;
	if (!info) {
		if (png)
			png_destroy_read_struct(&png, NULL, NULL);
		fclose(f);
		return NULL;
	}
	/* modified after setjmp: volatile, so the error path can free them */
	uint8_t *volatile src = NULL;
	png_bytepp volatile rows = NULL;
	uint32_t *volatile out = NULL;
	if (setjmp(png_jmpbuf(png)))
		goto done;

	png_init_io(png, f);
	png_read_info(png, info);
	png_uint_32 pw = 0, ph = 0;
	int bd = 0, ct = 0;
	png_get_IHDR(png, info, &pw, &ph, &bd, &ct, NULL, NULL, NULL);
	if (pw == 0 || ph == 0 || pw > SRC_MAX || ph > SRC_MAX)
		goto done;
	png_set_expand(png);
	png_set_strip_16(png);
	if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA)
		png_set_gray_to_rgb(png);
	if (!(ct & PNG_COLOR_MASK_ALPHA) && !png_get_valid(png, info, PNG_INFO_tRNS))
		png_set_add_alpha(png, 0xff, PNG_FILLER_AFTER);
	png_read_update_info(png, info);
	if (png_get_channels(png, info) != 4)
		goto done;

	size_t rb = png_get_rowbytes(png, info);
	src = malloc(rb * ph);
	rows = malloc(sizeof(png_bytep) * ph);
	if (!src || !rows)
		goto done;
	for (png_uint_32 y = 0; y < ph; y++)
		rows[y] = src + y * rb;
	png_read_image(png, rows);

	out = malloc((size_t)want * want * sizeof *out);
	if (!out)
		goto done;
	for (int y = 0; y < want; y++) {
		size_t sy = (size_t)y * ph / (size_t)want;
		for (int x = 0; x < want; x++) {
			size_t sx = (size_t)x * pw / (size_t)want;
			const uint8_t *p = src + sy * rb + sx * 4;
			out[(size_t)y * want + x] = (uint32_t)p[3] << 24 | (uint32_t)p[0] << 16 |
						    (uint32_t)p[1] << 8 | p[2];
		}
	}
done:
	png_destroy_read_struct(&png, &info, NULL);
	fclose(f);
	free(src);
	free(rows);
	if (!out)
		return NULL;
	return out;
}

/* The decoded icon for an Icon= value at want px; NULL when none. Cached, misses too. */
static uint32_t *icon_pixels(const char *name, int want)
{
	if (strlen(name) >= NAME_LEN)
		return NULL;
	for (int i = 0; i < PX_SLOTS; i++)
		if (px_tab[i].used && px_tab[i].size == want && !strcmp(px_tab[i].name, name))
			return px_tab[i].px;
	char path[600];
	uint32_t *px = NULL;
	if (icon_file(name, want, path, sizeof path))
		px = load_png(path, want);
	struct px_slot *s = &px_tab[px_next];
	px_next = (px_next + 1) % PX_SLOTS;
	free(s->px);
	s->px = px;
	s->size = want;
	snprintf(s->name, sizeof s->name, "%s", name);
	s->used = 1;
	return px;
}

/* source over destination; the destination's alpha is kept where the source is see-through */
static uint32_t over(uint32_t d, uint32_t s)
{
	uint32_t a = s >> 24;
	if (a == 0)
		return d;
	if (a == 255)
		return s;
	uint32_t ia = 255 - a;
	uint32_t r = ((s >> 16 & 255) * a + (d >> 16 & 255) * ia + 127) / 255;
	uint32_t g = ((s >> 8 & 255) * a + (d >> 8 & 255) * ia + 127) / 255;
	uint32_t b = ((s & 255) * a + (d & 255) * ia + 127) / 255;
	uint32_t da = (d >> 24) * ia / 255;
	return (a + da) << 24 | r << 16 | g << 8 | b;
}

static void blit(fl_canvas *cv, const uint32_t *src, int size, int x, int y)
{
	int x0 = x > cv->clip.x ? x : cv->clip.x;
	int y0 = y > cv->clip.y ? y : cv->clip.y;
	int x1 = x + size < cv->clip.x + cv->clip.w ? x + size : cv->clip.x + cv->clip.w;
	int y1 = y + size < cv->clip.y + cv->clip.h ? y + size : cv->clip.y + cv->clip.h;
	for (int py = y0; py < y1; py++) {
		uint32_t *d = cv->px + (size_t)py * cv->stride;
		const uint32_t *s = src + (size_t)(py - y) * size;
		for (int px = x0; px < x1; px++)
			d[px] = over(d[px], s[px - x]);
	}
}

int icon_draw_name(fl_canvas *cv, const char *name, int x, int y, int size)
{
	if (!cv->px || !name || !name[0] || size < WANT_MIN || size > WANT_MAX)
		return 0;
	uint32_t *px = icon_pixels(name, size);
	if (!px)
		return 0;
	blit(cv, px, size, x, y);
	return 1;
}

int icon_draw_app(fl_canvas *cv, const char *app_id, int x, int y, int size)
{
	if (!cv->px || !app_id || size < WANT_MIN || size > WANT_MAX)
		return 0;
	return icon_draw_name(cv, app_icon(app_id), x, y, size);
}

void icons_flush(void)
{
	for (int i = 0; i < PX_SLOTS; i++) {
		free(px_tab[i].px);
		px_tab[i].px = NULL;
	}
	memset(px_tab, 0, sizeof px_tab);
	memset(app_tab, 0, sizeof app_tab);
	px_next = app_next = 0;
}
