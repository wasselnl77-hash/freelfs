/* store.h -- flstore's parts: images (img.c) and the Flathub catalog index (index.c)
 * SPDX-License-Identifier: MIT */
#ifndef STORE_H
#define STORE_H

#include <flui.h>
#include <stddef.h>
#include <stdint.h>

/* ---- images ------------------------------------------------------------------------------ */

/* A decoded picture, premultiplied ARGB, scaled down to fit (never up). */
typedef struct simg {
	int w, h;
	uint32_t px[];
} simg;

/* PNG, JPEG or WebP (by the file's first bytes) scaled to fit max_w x max_h. NULL if it is none of
 * them or broken. */
simg *simg_load(const char *path, int max_w, int max_h);
/* Draw at x, y (clipped by cv->clip). */
void simg_draw(fl_canvas *cv, const simg *im, int x, int y);

/* ---- the catalog --------------------------------------------------------------------------- */

#define NCATS 8
extern const char *const cat_names[NCATS]; /* "Games", "Internet and chat", ... */

struct app {
	char *id, *name, *sum, *dev, *lic, *ver, *kw;
	uint32_t cats;             /* bit per category */
	uint32_t ts;               /* newest release, epoch seconds */
	uint32_t off, len;         /* the details record in details.bin */
	uint8_t verified, nshots;
};

struct db {
	struct app *a; /* sorted by id */
	size_t n;
	char *buf;
	char dir[512];
};

#define MAX_SHOTS 6
struct detail {
	char *desc;           /* plain text, paragraphs split by blank lines, list items start with "- " */
	char *home, *bugs;
	char *shot[MAX_SHOTS];
	int nshot;
	char *raw;
};

/* index.tsv and details.bin in dir. 0 on success. */
int db_load(struct db *db, const char *dir);
void db_free(struct db *db);
const struct app *db_find(const struct db *db, const char *id);
int db_detail(const struct db *db, const struct app *a, struct detail *d);
void detail_free(struct detail *d);

/* Reads the appstream collection (gzip XML) and writes index.tsv and details.bin into dir,
 * streaming, so memory stays small. 0 on success. */
int index_build(const char *gz, const char *dir);

/* A Flatpak application ID: dotted, [A-Za-z0-9_.-], at most 79 characters, no leading '.' or '-'. */
int valid_appid(const char *s);

#endif
