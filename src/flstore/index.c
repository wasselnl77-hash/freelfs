/* index.c -- the Flathub catalog: appstream XML in, a compact index out, details read on demand
 * SPDX-License-Identifier: MIT
 *
 * Flathub's appstream collection is about 11 MB of gzip, 50 MB of XML with every translation of every
 * string. Loading that as a tree would eat the 1 GB machine. index_build() reads it once as a stream
 * (libxml2's reader) in a child process and writes two files into the cache directory:
 *
 *   index.tsv    one line per desktop app: id, name, summary, developer, categories, licence, version,
 *                release time, verified, number of screenshots, offset and length of its record in
 *                details.bin, English keywords. Loaded whole by the store (about 1 MB), no translations.
 *   details.bin  per app: homepage, bug tracker, screenshot URLs and the long description. Only the
 *                record of the app page being looked at is ever read (pread).
 */
#include <errno.h>
#include <fcntl.h>
#include <libxml/xmlreader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <zlib.h>

#include "store.h"

#define INDEX_MAGIC "#flstore-index 2"

const char *const cat_names[NCATS] = {"Games", "Internet and chat", "Office", "Graphics and photo",
                                      "Audio and video", "Development", "Education and science",
                                      "Utilities and system"};

int valid_appid(const char *s)
{
	size_t n = strlen(s);
	if (n < 3 || n > 79 || s[0] == '.' || s[0] == '-' || !strchr(s, '.'))
		return 0;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
		      c == '.' || c == '-'))
			return 0;
	}
	return 1;
}

/* ---- a growing string -------------------------------------------------------------------- */

struct sb {
	char *s;
	size_t n, cap;
	int pend; /* white space seen, not yet written */
};

static void sb_put(struct sb *b, const char *s, size_t n)
{
	if (b->n + n + 1 > b->cap) {
		size_t nc = b->cap ? b->cap * 2 : 256;
		while (nc < b->n + n + 1)
			nc *= 2;
		char *p = realloc(b->s, nc);
		if (!p)
			return;
		b->s = p;
		b->cap = nc;
	}
	memcpy(b->s + b->n, s, n);
	b->n += n;
	b->s[b->n] = 0;
}

static void sb_str(struct sb *b, const char *s)
{
	sb_put(b, s, strlen(s));
}

static void sb_reset(struct sb *b)
{
	b->n = 0;
	b->pend = 0;
	if (b->s)
		b->s[0] = 0;
}

/* Text with runs of white space collapsed to one space, no leading space. */
static void sb_text(struct sb *b, const char *s)
{
	for (; *s; s++) {
		if (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
			b->pend = 1;
			continue;
		}
		if (b->pend && b->n > 0 && b->s[b->n - 1] != '\n' && b->s[b->n - 1] != ' ')
			sb_put(b, " ", 1);
		b->pend = 0;
		sb_put(b, s, 1);
	}
}

/* One field of the index: tabs and newlines would break the format. */
static void clean_field(char *s)
{
	for (; *s; s++)
		if (*s == '\t' || *s == '\n' || *s == '\r')
			*s = ' ';
}

/* ---- categories -------------------------------------------------------------------------- */

static const struct { const char *tag; int bit; } catmap[] = {
	{"Game", 0}, {"Network", 1}, {"WebBrowser", 1}, {"Email", 1}, {"Chat", 1}, {"InstantMessaging", 1},
	{"IRCClient", 1}, {"P2P", 1}, {"FileTransfer", 1}, {"Feed", 1}, {"News", 1}, {"Telephony", 1},
	{"VideoConference", 1}, {"RemoteAccess", 1}, {"Office", 2}, {"Spreadsheet", 2}, {"WordProcessor", 2},
	{"Presentation", 2}, {"Calendar", 2}, {"ContactManagement", 2}, {"Finance", 2}, {"Publishing", 2},
	{"ProjectManagement", 2}, {"Graphics", 3}, {"Photography", 3}, {"2DGraphics", 3}, {"3DGraphics", 3},
	{"RasterGraphics", 3}, {"VectorGraphics", 3}, {"Scanning", 3}, {"OCR", 3}, {"Viewer", 3},
	{"AudioVideo", 4}, {"Audio", 4}, {"Video", 4}, {"Music", 4}, {"Player", 4}, {"Recorder", 4},
	{"Mixer", 4}, {"Sequencer", 4}, {"Midi", 4}, {"Tuner", 4}, {"TV", 4}, {"AudioVideoEditing", 4},
	{"Development", 5}, {"IDE", 5}, {"Debugger", 5}, {"WebDevelopment", 5}, {"Building", 5},
	{"GUIDesigner", 5}, {"RevisionControl", 5}, {"Translation", 5}, {"Profiling", 5},
	{"Education", 6}, {"Science", 6}, {"Math", 6}, {"Astronomy", 6}, {"Biology", 6}, {"Chemistry", 6},
	{"Geography", 6}, {"Physics", 6}, {"Languages", 6}, {"ArtificialIntelligence", 6}, {"Robotics", 6},
	{"Electronics", 6}, {"Engineering", 6}, {"DataVisualization", 6}, {"Literature", 6},
	{"Utility", 7}, {"System", 7}, {"Settings", 7}, {"Accessibility", 7}, {"Archiving", 7},
	{"Compression", 7}, {"FileManager", 7}, {"FileTools", 7}, {"TerminalEmulator", 7},
	{"TextEditor", 7}, {"Monitor", 7}, {"Security", 7}, {"PackageManager", 7}, {"Calculator", 7},
	{"Clock", 7}, {"Maps", 7},
};

static uint32_t cat_bit(const char *tag)
{
	size_t n = strlen(tag);
	if (n > 4 && !strcmp(tag + n - 4, "Game"))
		return 1u << 0; /* ActionGame, BoardGame, ... */
	for (size_t i = 0; i < sizeof catmap / sizeof *catmap; i++)
		if (!strcmp(tag, catmap[i].tag))
			return 1u << catmap[i].bit;
	return 0;
}

/* ---- the reader -------------------------------------------------------------------------- */

struct comp {
	char type[24];
	char *id, *name, *sum, *dev, *lic, *ver, *home, *bugs;
	struct sb kw, desc;
	uint32_t cats, ts;
	int verified, nshots;
	char *shot[MAX_SHOTS];
	int shot_best;
	char *cur_shot_url;
	int cur_shot_score;
};

static void comp_fields_free(struct comp *c)
{
	free(c->id);
	free(c->name);
	free(c->sum);
	free(c->dev);
	free(c->lic);
	free(c->ver);
	free(c->home);
	free(c->bugs);
	for (int i = 0; i < MAX_SHOTS; i++)
		free(c->shot[i]);
	free(c->cur_shot_url);
}

static void comp_free(struct comp *c)
{
	comp_fields_free(c);
	free(c->kw.s);
	free(c->desc.s);
	memset(c, 0, sizeof *c);
}

static void comp_reset(struct comp *c)
{
	struct sb kw = c->kw, desc = c->desc;
	comp_fields_free(c);
	memset(c, 0, sizeof *c);
	sb_reset(&kw);
	sb_reset(&desc);
	c->kw = kw;
	c->desc = desc;
}

/* The value of an attribute as a malloc'ed string, NULL if absent. */
static char *attr(xmlTextReaderPtr r, const char *name)
{
	xmlChar *s = xmlTextReaderGetAttribute(r, (const xmlChar *)name);
	char *d = s ? strdup((const char *)s) : NULL;
	xmlFree(s);
	return d;
}

/* Score of a screenshot image: the 752 px thumbnail first, then 624, then anything 500-1300 wide, then
 * the biggest below that, the original last (it can be huge). */
static int image_score(const char *type, int w)
{
	if (type && !strcmp(type, "thumbnail")) {
		if (w == 752)
			return 1000;
		if (w == 624)
			return 900;
		if (w >= 500 && w <= 1300)
			return 800 - (w > 752 ? w - 752 : 752 - w) / 10;
		return w > 0 && w < 500 ? w : 1;
	}
	return 1;
}

struct writer {
	FILE *idx, *det;
	size_t det_off;
	long count;
	/* ids already written */
	char **seen;
	size_t nseen, capseen;
};

static int seen_has(struct writer *w, const char *id)
{
	for (size_t i = 0; i < w->nseen; i++)
		if (!strcmp(w->seen[i], id))
			return 1;
	return 0;
}

static void emit(struct writer *w, struct comp *c)
{
	if (!c->id || !valid_appid(c->id) || !c->name)
		return;
	if (strcmp(c->type, "desktop-application") && strcmp(c->type, "desktop"))
		return;
	if (seen_has(w, c->id))
		return;
	if (w->nseen == w->capseen) {
		size_t nc = w->capseen ? w->capseen * 2 : 1024;
		char **p = realloc(w->seen, nc * sizeof *p);
		if (!p)
			return;
		w->seen = p;
		w->capseen = nc;
	}
	w->seen[w->nseen++] = strdup(c->id);

	/* the details record: H, B, S lines, a blank line, the description */
	struct sb rec = {0};
	if (c->home) {
		sb_str(&rec, "H\t");
		sb_str(&rec, c->home);
		sb_str(&rec, "\n");
	}
	if (c->bugs) {
		sb_str(&rec, "B\t");
		sb_str(&rec, c->bugs);
		sb_str(&rec, "\n");
	}
	for (int i = 0; i < c->nshots; i++) {
		sb_str(&rec, "S\t");
		sb_str(&rec, c->shot[i]);
		sb_str(&rec, "\n");
	}
	sb_str(&rec, "\n");
	while (c->desc.n && (c->desc.s[c->desc.n - 1] == '\n' || c->desc.s[c->desc.n - 1] == ' '))
		c->desc.s[--c->desc.n] = 0;
	if (c->desc.n > 3000) { /* long texts: cut at a paragraph or word */
		size_t cut = 3000;
		while (cut > 2000 && c->desc.s[cut] != '\n' && c->desc.s[cut] != ' ')
			cut--;
		c->desc.s[cut] = 0;
		c->desc.n = cut;
	}
	if (c->desc.n)
		sb_put(&rec, c->desc.s, c->desc.n);
	fwrite(rec.s, 1, rec.n, w->det);

	char *f[] = {c->id, c->name, c->sum, c->dev, c->lic, c->ver, c->kw.s};
	for (size_t i = 0; i < sizeof f / sizeof *f; i++)
		if (f[i])
			clean_field(f[i]);
	fprintf(w->idx, "%s\t%s\t%s\t%s\t%x\t%s\t%s\t%u\t%d\t%d\t%zu\t%zu\t%s\n", c->id, c->name, c->sum ? c->sum : "",
	        c->dev ? c->dev : "", c->cats, c->lic ? c->lic : "", c->ver ? c->ver : "", c->ts, c->verified,
	        c->nshots, w->det_off, rec.n, c->kw.s ? c->kw.s : "");
	w->det_off += rec.n;
	w->count++;
	free(rec.s);
}

static int gz_read(void *ctx, char *buf, int len)
{
	return gzread((gzFile)ctx, buf, (unsigned)len);
}

int index_build(const char *gz, const char *dir)
{
	gzFile g = gzopen(gz, "rb");
	if (!g) {
		fprintf(stderr, "flstore: cannot open %s: %s\n", gz, strerror(errno));
		return 1;
	}
	char idx_tmp[600], det_tmp[600], idx_path[600], det_path[600];
	snprintf(idx_tmp, sizeof idx_tmp, "%s/index.tsv.tmp", dir);
	snprintf(det_tmp, sizeof det_tmp, "%s/details.bin.tmp", dir);
	snprintf(idx_path, sizeof idx_path, "%s/index.tsv", dir);
	snprintf(det_path, sizeof det_path, "%s/details.bin", dir);
	struct writer w = {0};
	w.idx = fopen(idx_tmp, "w");
	w.det = fopen(det_tmp, "w");
	if (!w.idx || !w.det) {
		fprintf(stderr, "flstore: cannot write into %s\n", dir);
		return 1;
	}
	fprintf(w.idx, "%s\n", INDEX_MAGIC);
	xmlTextReaderPtr r = xmlReaderForIO(gz_read, NULL, g, "appstream.xml", NULL,
	                                    XML_PARSE_NOENT | XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING |
	                                    XML_PARSE_HUGE);
	if (!r) {
		fprintf(stderr, "flstore: not an XML file\n");
		return 1;
	}
	struct comp c = {0};
	char path[10][40] = {{0}}; /* element names by depth */
	int in_comp = 0, skip_depth = -1, in_desc = 0, in_dev = 0, rc;
	enum { COL_NONE, COL_ID, COL_NAME, COL_SUM, COL_DEV, COL_LIC, COL_HOME, COL_BUGS, COL_CAT, COL_KW,
	       COL_IMG, COL_VERIFIED } col = COL_NONE;
	int col_depth = -1, img_score = 0;
	struct sb text = {0};
	while ((rc = xmlTextReaderRead(r)) == 1) {
		int type = xmlTextReaderNodeType(r), depth = xmlTextReaderDepth(r);
		const char *name = (const char *)xmlTextReaderConstName(r);
		if (!name)
			continue;
		if (type == XML_READER_TYPE_ELEMENT) {
			int empty = xmlTextReaderIsEmptyElement(r);
			if (depth < 10)
				snprintf(path[depth], sizeof path[depth], "%s", name);
			if (skip_depth >= 0 && depth > skip_depth)
				continue;
			skip_depth = -1;
			col = COL_NONE;
			if (depth == 1 && !strcmp(name, "component")) {
				comp_reset(&c);
				in_comp = 1;
				in_desc = in_dev = 0;
				char *t = attr(r, "type");
				snprintf(c.type, sizeof c.type, "%s", t ? t : "");
				free(t);
				if (empty) {
					emit(&w, &c);
					in_comp = 0;
				}
				continue;
			}
			if (!in_comp || depth < 2)
				continue;
			/* anything in a language other than the default: skipped with its children */
			char *lang = attr(r, "xml:lang");
			if (lang) {
				free(lang);
				if (!empty)
					skip_depth = depth;
				continue;
			}
			if (empty && depth >= 2 && strcmp(name, "release"))
				continue; /* nothing to read (releases carry their data in attributes) */
			if (depth == 2) {
				in_desc = !strcmp(name, "description");
				in_dev = !strcmp(name, "developer");
				if (!strcmp(name, "id") && !c.id)
					col = COL_ID;
				else if (!strcmp(name, "name") && !c.name)
					col = COL_NAME;
				else if (!strcmp(name, "summary") && !c.sum)
					col = COL_SUM;
				else if (!strcmp(name, "developer_name") && !c.dev)
					col = COL_DEV;
				else if (!strcmp(name, "project_license") && !c.lic)
					col = COL_LIC;
				else if (!strcmp(name, "url")) {
					char *t = attr(r, "type");
					if (t && !strcmp(t, "homepage") && !c.home)
						col = COL_HOME;
					else if (t && !strcmp(t, "bugtracker") && !c.bugs)
						col = COL_BUGS;
					free(t);
				}
			} else if (in_desc) {
				if (!strcmp(name, "li"))
					sb_str(&c.desc, "- ");
			} else if (depth == 3 && !strcmp(path[2], "categories") && !strcmp(name, "category")) {
				col = COL_CAT;
			} else if (depth == 3 && !strcmp(path[2], "keywords") && !strcmp(name, "keyword")) {
				col = COL_KW;
			} else if (depth == 3 && in_dev && !strcmp(name, "name") && !c.dev) {
				col = COL_DEV; /* appstream 1.0: <developer><name> */
			} else if (depth == 3 && !strcmp(path[2], "releases") && !strcmp(name, "release")) {
				char *ts = attr(r, "timestamp"), *v = attr(r, "version");
				uint32_t t = ts ? (uint32_t)strtoul(ts, NULL, 10) : 0;
				if (t >= c.ts && (t > c.ts || !c.ver)) {
					c.ts = t;
					free(c.ver);
					c.ver = v;
					v = NULL;
				}
				free(ts);
				free(v);
			} else if (depth == 3 && !strcmp(path[2], "screenshots") && !strcmp(name, "screenshot")) {
				free(c.cur_shot_url);
				c.cur_shot_url = NULL;
				c.cur_shot_score = 0;
			} else if (depth == 4 && !strcmp(path[2], "screenshots") && !strcmp(name, "image")) {
				char *t = attr(r, "type"), *wd = attr(r, "width");
				img_score = image_score(t, wd ? atoi(wd) : 0);
				free(t);
				free(wd);
				if (img_score > c.cur_shot_score)
					col = COL_IMG;
			} else if (depth == 3 && !strcmp(path[2], "custom") && !strcmp(name, "value")) {
				char *k = attr(r, "key");
				if (k && !strcmp(k, "flathub::verification::verified"))
					col = COL_VERIFIED;
				free(k);
			}
			if (col != COL_NONE) {
				col_depth = depth;
				sb_reset(&text);
			}
		} else if (type == XML_READER_TYPE_END_ELEMENT) {
			if (skip_depth >= 0) {
				if (depth == skip_depth)
					skip_depth = -1;
				continue;
			}
			if (col != COL_NONE && depth == col_depth) {
				while (text.n && text.s[text.n - 1] == ' ')
					text.s[--text.n] = 0;
				if (text.n) {
					switch (col) {
					case COL_ID: c.id = strdup(text.s); break;
					case COL_NAME: c.name = strdup(text.s); break;
					case COL_SUM: c.sum = strdup(text.s); break;
					case COL_DEV: if (!c.dev) c.dev = strdup(text.s); break;
					case COL_LIC: c.lic = strdup(text.s); break;
					case COL_HOME: c.home = strdup(text.s); break;
					case COL_BUGS: c.bugs = strdup(text.s); break;
					case COL_CAT: c.cats |= cat_bit(text.s); break;
					case COL_KW:
						if (c.kw.n < 160) {
							if (c.kw.n)
								sb_str(&c.kw, " ");
							sb_str(&c.kw, text.s);
						}
						break;
					case COL_IMG:
						if (!strncmp(text.s, "https://", 8)) {
							free(c.cur_shot_url);
							c.cur_shot_url = strdup(text.s);
							c.cur_shot_score = img_score;
						}
						break;
					case COL_VERIFIED: if (!strcmp(text.s, "true")) c.verified = 1; break;
					default: break;
					}
				}
				col = COL_NONE;
			}
			if (depth == 1 && !strcmp(name, "component") && in_comp) {
				emit(&w, &c);
				in_comp = 0;
				continue;
			}
			if (!in_comp)
				continue;
			if (in_desc && depth == 2 && !strcmp(name, "description")) {
				in_desc = 0;
			} else if (in_desc && (!strcmp(name, "p") || !strcmp(name, "li"))) {
				if (c.desc.n && c.desc.s[c.desc.n - 1] != '\n')
					sb_str(&c.desc, !strcmp(name, "p") ? "\n\n" : "\n");
				c.desc.pend = 0;
			} else if (in_desc && (!strcmp(name, "ul") || !strcmp(name, "ol"))) {
				if (c.desc.n && c.desc.s[c.desc.n - 1] == '\n' &&
				    (c.desc.n < 2 || c.desc.s[c.desc.n - 2] != '\n'))
					sb_str(&c.desc, "\n");
			} else if (depth == 2 && !strcmp(name, "developer")) {
				in_dev = 0;
			} else if (depth == 3 && !strcmp(path[2], "screenshots") && !strcmp(name, "screenshot")) {
				if (c.cur_shot_url && c.nshots < MAX_SHOTS) {
					c.shot[c.nshots++] = c.cur_shot_url;
					c.cur_shot_url = NULL;
				}
			}
		} else if ((type == XML_READER_TYPE_TEXT || type == XML_READER_TYPE_CDATA) && in_comp &&
		           skip_depth < 0) {
			const char *v = (const char *)xmlTextReaderConstValue(r);
			if (!v)
				continue;
			if (col != COL_NONE && text.n < 2000)
				sb_text(&text, v);
			else if (in_desc && c.desc.n < 8000)
				sb_text(&c.desc, v);
		}
	}
	free(text.s);
	int bad = rc < 0;
	xmlFreeTextReader(r);
	gzclose(g);
	comp_free(&c);
	for (size_t i = 0; i < w.nseen; i++)
		free(w.seen[i]);
	free(w.seen);
	int werr = ferror(w.idx) || ferror(w.det);
	werr |= fclose(w.idx) != 0;
	werr |= fclose(w.det) != 0;
	if (bad || werr || w.count < 1) {
		fprintf(stderr, "flstore: could not read the catalog (%s)\n", bad ? "XML error" : "no apps");
		unlink(idx_tmp);
		unlink(det_tmp);
		return 1;
	}
	/* details first: an index that points into a missing details file would be worse than an old pair */
	if (rename(det_tmp, det_path) < 0 || rename(idx_tmp, idx_path) < 0) {
		perror("flstore: rename");
		return 1;
	}
	fprintf(stderr, "flstore: %ld apps indexed\n", w.count);
	return 0;
}

/* ---- loading ------------------------------------------------------------------------------ */

static int cmp_app_id(const void *a, const void *b)
{
	return strcmp(((const struct app *)a)->id, ((const struct app *)b)->id);
}

int db_load(struct db *db, const char *dir)
{
	memset(db, 0, sizeof *db);
	snprintf(db->dir, sizeof db->dir, "%s", dir);
	char path[600];
	snprintf(path, sizeof path, "%s/index.tsv", dir);
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return -1;
	fseek(fp, 0, SEEK_END);
	long n = ftell(fp);
	rewind(fp);
	if (n < 32 || n > (64L << 20)) {
		fclose(fp);
		return -1;
	}
	db->buf = malloc((size_t)n + 1);
	if (!db->buf || fread(db->buf, 1, (size_t)n, fp) != (size_t)n) {
		fclose(fp);
		db_free(db);
		return -1;
	}
	fclose(fp);
	db->buf[n] = 0;
	size_t lines = 0;
	for (long i = 0; i < n; i++)
		if (db->buf[i] == '\n')
			lines++;
	db->a = calloc(lines + 1, sizeof *db->a);
	if (!db->a) {
		db_free(db);
		return -1;
	}
	char *s = db->buf;
	int first = 1;
	while (*s) {
		char *e = strchr(s, '\n');
		if (!e)
			break;
		*e = 0;
		if (first) {
			first = 0;
			if (strcmp(s, INDEX_MAGIC)) {
				db_free(db);
				return -1; /* an older format: rebuild */
			}
			s = e + 1;
			continue;
		}
		char *f[13];
		int nf = 0;
		f[nf++] = s;
		for (char *p = s; *p && nf < 13; p++)
			if (*p == '\t') {
				*p = 0;
				f[nf++] = p + 1;
			}
		if (nf == 13 && valid_appid(f[0])) {
			struct app *a = &db->a[db->n++];
			a->id = f[0];
			a->name = f[1];
			a->sum = f[2];
			a->dev = f[3];
			a->cats = (uint32_t)strtoul(f[4], NULL, 16);
			a->lic = f[5];
			a->ver = f[6];
			a->ts = (uint32_t)strtoul(f[7], NULL, 10);
			a->verified = (uint8_t)atoi(f[8]);
			a->nshots = (uint8_t)atoi(f[9]);
			a->off = (uint32_t)strtoul(f[10], NULL, 10);
			a->len = (uint32_t)strtoul(f[11], NULL, 10);
			a->kw = f[12];
		}
		s = e + 1;
	}
	if (db->n == 0) {
		db_free(db);
		return -1;
	}
	qsort(db->a, db->n, sizeof *db->a, cmp_app_id);
	return 0;
}

void db_free(struct db *db)
{
	free(db->a);
	free(db->buf);
	memset(db, 0, sizeof *db);
}

const struct app *db_find(const struct db *db, const char *id)
{
	size_t lo = 0, hi = db->n;
	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		int c = strcmp(db->a[mid].id, id);
		if (c == 0)
			return &db->a[mid];
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return NULL;
}

int db_detail(const struct db *db, const struct app *a, struct detail *d)
{
	memset(d, 0, sizeof *d);
	char path[600];
	snprintf(path, sizeof path, "%s/details.bin", db->dir);
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (a->len == 0 || a->len > (1u << 20)) {
		close(fd);
		return -1;
	}
	char *buf = malloc(a->len + 1);
	if (!buf || pread(fd, buf, a->len, (off_t)a->off) != (ssize_t)a->len) {
		free(buf);
		close(fd);
		return -1;
	}
	close(fd);
	buf[a->len] = 0;
	d->raw = buf;
	char *s = buf;
	while (*s && *s != '\n') {
		char *e = strchr(s, '\n');
		if (!e)
			break;
		*e = 0;
		if (s[0] && s[1] == '\t') {
			char *v = s + 2;
			if (s[0] == 'H')
				d->home = v;
			else if (s[0] == 'B')
				d->bugs = v;
			else if (s[0] == 'S' && d->nshot < MAX_SHOTS)
				d->shot[d->nshot++] = v;
		}
		s = e + 1;
	}
	d->desc = *s == '\n' ? s + 1 : s;
	return 0;
}

void detail_free(struct detail *d)
{
	free(d->raw);
	memset(d, 0, sizeof *d);
}
