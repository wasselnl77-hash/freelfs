/* worldmap.c -- time zones from tzdata, a flat world map and a city search
 * SPDX-License-Identifier: MIT
 *
 * The map is drawn by our own code from worldmap_data.c (Natural Earth 1:110m,
 * public domain, converted by tools/mkworldmap.py) in the Natural Earth
 * projection: a pseudo-cylindrical one whose meridians bend towards the poles.
 * The static layers (sea, graticule, land, borders) are rendered once per size
 * and theme into a private buffer; a selection recomposes the tint and the
 * dots, hovering only draws a ring and a card on top.
 *
 * The "region" of a zone is everything on land that lies nearer to its place
 * (zone.tab coordinates) than to any other zone's place. That is what a click
 * picks, what hovering outlines and what is tinted: the strong tint is the
 * chosen zone's own region, the soft one all regions that show the same time
 * right now.
 */
#include "internal.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern const int16_t fl_wm_land[], fl_wm_borders[];

/* ---- the zone list ------------------------------------------------------------------- */

#define MAXZ 1024
#define LAT_N 84.0
#define LAT_S (-90.0)

static fl_tzone zones[MAXZ];
static float zvx[MAXZ], zvy[MAXZ], zvz[MAXZ]; /* unit vectors of the places */
static int nz = -1;
static time_t stamp;

const char *fl_tz_dir(void)
{
	const char *e = getenv("FL_ZONEINFO");
	return e && *e ? e : "/usr/share/zoneinfo";
}

time_t fl_tz_time(void)
{
	const char *e = getenv("FL_NOW");
	if (e && *e)
		return (time_t)atoll(e);
	return time(NULL);
}

static int parse_coord(const char *s, int dlen, double *out)
{
	/* [+-]DDMM[SS] or [+-]DDDMM[SS]: dlen is the number of degree digits */
	int sign = *s == '-' ? -1 : 1;
	s++;
	int n = (int)strlen(s);
	if (n != dlen + 2 && n != dlen + 4)
		return -1;
	for (int i = 0; i < n; i++)
		if (!isdigit((unsigned char)s[i]))
			return -1;
	char b[8];
	memcpy(b, s, (size_t)dlen);
	b[dlen] = 0;
	double deg = atoi(b);
	memcpy(b, s + dlen, 2);
	b[2] = 0;
	deg += atoi(b) / 60.0;
	if (n == dlen + 4) {
		memcpy(b, s + dlen + 2, 2);
		b[2] = 0;
		deg += atoi(b) / 3600.0;
	}
	*out = sign * deg;
	return 0;
}

static int parse_iso6709(const char *s, float *lat, float *lon)
{
	size_t n = strlen(s), j = 1;
	while (j < n && s[j] != '+' && s[j] != '-')
		j++;
	if (j >= n || (s[0] != '+' && s[0] != '-'))
		return -1;
	char a[16], b[16];
	if (j >= sizeof a || n - j >= sizeof b)
		return -1;
	memcpy(a, s, j);
	a[j] = 0;
	memcpy(b, s + j, n - j);
	b[n - j] = 0;
	double la, lo;
	if (parse_coord(a, 2, &la) < 0 || parse_coord(b, 3, &lo) < 0)
		return -1;
	*lat = (float)la;
	*lon = (float)lo;
	return 0;
}

struct cc { char code[3]; char name[40]; };

static void load_countries(struct cc **out, int *n)
{
	*out = NULL;
	*n = 0;
	char path[400];
	snprintf(path, sizeof path, "%s/iso3166.tab", fl_tz_dir());
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	int cap = 0;
	char line[200];
	while (fgets(line, sizeof line, f)) {
		if (line[0] == '#' || strlen(line) < 4 || line[2] != '\t')
			continue;
		line[strcspn(line, "\r\n")] = 0;
		if (*n == cap) {
			cap = cap ? cap * 2 : 256;
			*out = realloc(*out, (size_t)cap * sizeof **out);
			if (!*out) {
				*n = 0;
				fclose(f);
				return;
			}
		}
		struct cc *c = &(*out)[(*n)++];
		memcpy(c->code, line, 2);
		c->code[2] = 0;
		snprintf(c->name, sizeof c->name, "%.39s", line + 3);
	}
	fclose(f);
}

static void add_zone(const char *cc, const char *coord, const char *tz, const struct cc *ccs, int ncc)
{
	if (nz >= MAXZ || strlen(tz) >= sizeof zones[0].name)
		return;
	char path[400];
	snprintf(path, sizeof path, "%s/%s", fl_tz_dir(), tz);
	if (access(path, R_OK) != 0)
		return;
	fl_tzone *z = &zones[nz];
	memset(z, 0, sizeof *z);
	if (parse_iso6709(coord, &z->lat, &z->lon) < 0)
		return;
	snprintf(z->name, sizeof z->name, "%s", tz);
	const char *slash = strrchr(tz, '/');
	snprintf(z->city, sizeof z->city, "%s", slash ? slash + 1 : tz);
	for (char *p = z->city; *p; p++)
		if (*p == '_')
			*p = ' ';
	char code[3] = {cc[0], cc[0] ? cc[1] : 0, 0};
	for (int i = 0; i < ncc; i++)
		if (!strcmp(ccs[i].code, code)) {
			snprintf(z->country, sizeof z->country, "%s", ccs[i].name);
			break;
		}
	nz++;
}

static int load_tab(const char *file, const struct cc *ccs, int ncc)
{
	char path[400];
	snprintf(path, sizeof path, "%s/%s", fl_tz_dir(), file);
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	char line[400];
	while (fgets(line, sizeof line, f)) {
		if (line[0] == '#')
			continue;
		line[strcspn(line, "\r\n")] = 0;
		char *cc = line, *t1 = strchr(cc, '\t');
		if (!t1)
			continue;
		*t1++ = 0;
		char *t2 = strchr(t1, '\t');
		if (!t2)
			continue;
		*t2++ = 0;
		char *t3 = strchr(t2, '\t');
		if (t3)
			*t3 = 0;
		add_zone(cc, t1, t2, ccs, ncc);
	}
	fclose(f);
	return 1;
}

static int zcmp_name(const void *a, const void *b)
{
	return strcmp(((const fl_tzone *)a)->name, ((const fl_tzone *)b)->name);
}

static void vec(double lat_deg, double lon_deg, float *x, float *y, float *z)
{
	double la = lat_deg * M_PI / 180, lo = lon_deg * M_PI / 180;
	*x = (float)(cos(la) * cos(lo));
	*y = (float)(cos(la) * sin(lo));
	*z = (float)sin(la);
}

static void tz_load(void)
{
	if (nz >= 0)
		return;
	nz = 0;
	struct cc *ccs;
	int ncc;
	load_countries(&ccs, &ncc);
	if (!load_tab("zone.tab", ccs, ncc) || nz == 0)
		load_tab("zone1970.tab", ccs, ncc);
	free(ccs);
	qsort(zones, (size_t)nz, sizeof zones[0], zcmp_name);
	/* UTC has no place: it is found by search and by the system zone only */
	char path[400];
	snprintf(path, sizeof path, "%s/UTC", fl_tz_dir());
	if (nz < MAXZ && access(path, R_OK) == 0) {
		fl_tzone *z = &zones[nz++];
		memset(z, 0, sizeof *z);
		snprintf(z->name, sizeof z->name, "UTC");
		snprintf(z->city, sizeof z->city, "UTC");
		snprintf(z->country, sizeof z->country, "Coordinated Universal Time");
		z->lat = 999;
	}
	for (int i = 0; i < nz; i++)
		if (zones[i].lat <= 90)
			vec(zones[i].lat, zones[i].lon, &zvx[i], &zvy[i], &zvz[i]);
	fl_tz_refresh();
}

int fl_tz_count(void)
{
	tz_load();
	return nz;
}

const fl_tzone *fl_tz_get(int i)
{
	tz_load();
	return i >= 0 && i < nz ? &zones[i] : NULL;
}

int fl_tz_find(const char *name)
{
	tz_load();
	if (!name)
		return -1;
	if (!strcmp(name, "Etc/UTC") || !strcmp(name, "Etc/UCT") || !strcmp(name, "Etc/Universal") ||
	    !strcmp(name, "Etc/Zulu"))
		name = "UTC";
	for (int i = 0; i < nz; i++)
		if (!strcmp(zones[i].name, name))
			return i;
	return -1;
}

int fl_tz_nearest(float lat, float lon)
{
	tz_load();
	float x, y, z;
	vec(lat, lon, &x, &y, &z);
	int best = -1;
	float bd = -2;
	for (int i = 0; i < nz; i++) {
		if (zones[i].lat > 90)
			continue;
		float d = x * zvx[i] + y * zvy[i] + z * zvz[i];
		if (d > bd) {
			bd = d;
			best = i;
		}
	}
	return best;
}

int fl_tz_system(void)
{
	tz_load();
	const char *etc = getenv("FL_TZ_ETC");
	if (!etc || !*etc)
		etc = "/etc";
	char path[400], buf[200];
	snprintf(path, sizeof path, "%s/timezone", etc);
	FILE *f = fopen(path, "r");
	if (f) {
		if (fgets(buf, sizeof buf, f)) {
			buf[strcspn(buf, "\r\n \t")] = 0;
			int i = fl_tz_find(buf);
			if (i >= 0) {
				fclose(f);
				return i;
			}
		}
		fclose(f);
	}
	snprintf(path, sizeof path, "%s/localtime", etc);
	ssize_t n = readlink(path, buf, sizeof buf - 1);
	if (n > 0) {
		buf[n] = 0;
		const char *p = strstr(buf, "zoneinfo/");
		if (p)
			return fl_tz_find(p + 9);
	}
	return -1;
}

int fl_tz_local(int idx, time_t t, struct tm *tm, int *off, char *abbr, size_t cap)
{
	tz_load();
	if (idx < 0 || idx >= nz)
		return -1;
	const char *old = getenv("TZ");
	char *save = old ? strdup(old) : NULL;
	char val[400];
	snprintf(val, sizeof val, ":%s/%s", fl_tz_dir(), zones[idx].name);
	setenv("TZ", val, 1);
	tzset();
	struct tm r;
	localtime_r(&t, &r);
	if (off)
		*off = (int)r.tm_gmtoff;
	if (abbr && cap)
		snprintf(abbr, cap, "%s", r.tm_zone ? r.tm_zone : "");
	*tm = r;
	tm->tm_zone = NULL;
	if (save) {
		setenv("TZ", save, 1);
		free(save);
	} else {
		unsetenv("TZ");
	}
	tzset();
	return 0;
}

time_t fl_tz_mktime(int idx, struct tm *tm)
{
	tz_load();
	if (idx < 0 || idx >= nz)
		return 0;
	const char *old = getenv("TZ");
	char *save = old ? strdup(old) : NULL;
	char val[400];
	snprintf(val, sizeof val, ":%s/%s", fl_tz_dir(), zones[idx].name);
	setenv("TZ", val, 1);
	tzset();
	tm->tm_isdst = -1;
	time_t t = mktime(tm);
	if (save) {
		setenv("TZ", save, 1);
		free(save);
	} else {
		unsetenv("TZ");
	}
	tzset();
	return t == (time_t)-1 ? 0 : t;
}

void fl_tz_refresh(void)
{
	if (nz < 0) {
		tz_load();
		return;
	}
	time_t t = fl_tz_time();
	for (int i = 0; i < nz; i++) {
		struct tm tm;
		int off = 0;
		fl_tz_local(i, t, &tm, &off, NULL, 0);
		zones[i].off_now = off;
	}
	stamp = t;
}

void fl_tz_fmt_offset(int off, char *out, size_t cap)
{
	if (off == 0) {
		snprintf(out, cap, "UTC");
		return;
	}
	int a = off < 0 ? -off : off;
	if (a % 3600 == 0)
		snprintf(out, cap, "UTC%c%d", off < 0 ? '-' : '+', a / 3600);
	else
		snprintf(out, cap, "UTC%c%d:%02d", off < 0 ? '-' : '+', a / 3600, a % 3600 / 60);
}

void fl_tz_fmt_time(const struct tm *tm, int h24, char *out, size_t cap)
{
	if (h24)
		snprintf(out, cap, "%02d:%02d", tm->tm_hour, tm->tm_min);
	else
		snprintf(out, cap, "%d:%02d %s", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12, tm->tm_min,
		         tm->tm_hour < 12 ? "AM" : "PM");
}

/* ---- search ---------------------------------------------------------------------------- */

static const char *ci_find(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	for (; *hay; hay++)
		if (!strncasecmp(hay, needle, n))
			return hay;
	return NULL;
}

/* lower is better, -1 no match */
static int score(const fl_tzone *z, const char *q)
{
	const char *p;
	if (!strncasecmp(z->city, q, strlen(q)))
		return 0;
	if ((p = ci_find(z->city, q)) && p > z->city && p[-1] == ' ')
		return 1;
	if (!strncasecmp(z->country, q, strlen(q)))
		return 2;
	if (ci_find(z->city, q))
		return 3;
	if (ci_find(z->name, q))
		return 4;
	if (ci_find(z->country, q))
		return 5;
	return -1;
}

int fl_tz_search(const char *q, int *out, int max)
{
	tz_load();
	while (*q == ' ')
		q++;
	char qq[64];
	snprintf(qq, sizeof qq, "%s", q);
	size_t l = strlen(qq);
	while (l && qq[l - 1] == ' ')
		qq[--l] = 0;
	if (!l)
		return 0;
	if (max > 16)
		max = 16;
	int n = 0, sc[16];
	for (int i = 0; i < nz; i++) {
		int s = score(&zones[i], qq);
		if (s < 0)
			continue;
		/* insertion into the best-first list of at most max */
		int at = n;
		while (at > 0 && sc[at - 1] > s)
			at--;
		if (at >= max)
			continue;
		int last = n < max ? n : max - 1;
		for (int k = last; k > at; k--) {
			out[k] = out[k - 1];
			sc[k] = sc[k - 1];
		}
		out[at] = i;
		sc[at] = s;
		if (n < max)
			n++;
	}
	return n;
}

/* ---- Natural Earth projection ----------------------------------------------------------- */

static void ne_fwd(double lon, double lat, double *x, double *y)
{
	double p2 = lat * lat, p4 = p2 * p2;
	*x = lon * (0.8707 - 0.131979 * p2 + p4 * (-0.013791 + p4 * (0.003971 * p2 - 0.001529 * p4)));
	*y = lat * (1.007226 + p2 * (0.015085 + p4 * (-0.044475 + 0.028874 * p2 - 0.005916 * p4)));
}

static double ne_y(double lat)
{
	double p2 = lat * lat, p4 = p2 * p2;
	return lat * (1.007226 + p2 * (0.015085 + p4 * (-0.044475 + 0.028874 * p2 - 0.005916 * p4)));
}

/* 0 when (x, y) lies on the map */
static int ne_inv(double x, double y, double *lon, double *lat)
{
	double phi = y;
	for (int i = 0; i < 8; i++) {
		double f = ne_y(phi) - y, d = (ne_y(phi + 1e-4) - ne_y(phi - 1e-4)) / 2e-4;
		phi -= f / d;
		if (phi > M_PI / 2)
			phi = M_PI / 2;
		if (phi < -M_PI / 2)
			phi = -M_PI / 2;
	}
	double cx, cy;
	ne_fwd(1, phi, &cx, &cy);
	*lat = phi;
	*lon = x / cx;
	return fabs(*lon) <= M_PI;
}

/* ---- the widget ------------------------------------------------------------------------ */

struct fl_worldmap {
	int w, h;		/* size of the picture in pixels (0: nothing built) */
	double sc, xmax, ytop;	/* projection to pixels */
	uint32_t theme_sig;
	uint32_t *base, *comp;
	uint8_t *mask;		/* land coverage */
	uint16_t *near;		/* 1 + zone of the land pixel, 0 elsewhere */
	int comp_sel, comp_valid;
	time_t comp_stamp;
	int h24;
	int hover_last;
};

fl_worldmap *fl_worldmap_new(void)
{
	fl_worldmap *m = calloc(1, sizeof *m);
	if (m) {
		m->h24 = 1;
		m->hover_last = -1;
	}
	return m;
}

static void release(fl_worldmap *m)
{
	free(m->base);
	free(m->comp);
	free(m->mask);
	free(m->near);
	m->base = m->comp = NULL;
	m->mask = NULL;
	m->near = NULL;
	m->w = m->h = 0;
	m->comp_valid = 0;
}

void fl_worldmap_free(fl_worldmap *m)
{
	if (!m)
		return;
	release(m);
	free(m);
}

void fl_worldmap_set_24h(fl_worldmap *m, int h24)
{
	m->h24 = h24 != 0;
}

static double xmax_const(void)
{
	double x, y;
	ne_fwd(M_PI, 0, &x, &y);
	return x;
}

int fl_worldmap_height(int w)
{
	double xm = xmax_const();
	double yn = ne_y(LAT_N * M_PI / 180), ys = ne_y(LAT_S * M_PI / 180);
	return (int)((yn - ys) / (2 * xm) * w + 0.5);
}

/* geometry for a picture that fits in w x h */
static void geometry(fl_worldmap *m, int fw, int fh, int *pw, int *ph)
{
	double xm = xmax_const(), yn = ne_y(LAT_N * M_PI / 180), ys = ne_y(LAT_S * M_PI / 180);
	double sc = fmin(fw / (2 * xm), fh / (yn - ys));
	m->sc = sc;
	m->xmax = xm;
	m->ytop = yn;
	*pw = (int)(2 * xm * sc);
	*ph = (int)((yn - ys) * sc);
}

static void project(const fl_worldmap *m, double lon_deg, double lat_deg, float *px, float *py)
{
	double x, y;
	ne_fwd(lon_deg * M_PI / 180, lat_deg * M_PI / 180, &x, &y);
	*px = (float)((x + m->xmax) * m->sc);
	*py = (float)((m->ytop - y) * m->sc);
}

static uint32_t theme_sig(void)
{
	return fl_th.bg * 31u ^ fl_th.raised * 17u ^ fl_th.accent * 7u ^ fl_th.dim ^ fl_th.line * 3u;
}

static fl_color land_col(void) { return fl_th.raised; }

static void disc(fl_canvas *cv, float x, float y, float r, fl_color c)
{
	char d[160];
	snprintf(d, sizeof d, "M%g %gA%g %g 0 1 0 %g %gA%g %g 0 1 0 %g %gZ", x - r, y, r, r, x + r, y, r, r, x - r, y);
	fl_path *p = fl_path_new();
	if (!p)
		return;
	fl_path_svg(p, d, 1, 1, 0, 0);
	fl_path_fill(cv, p, c);
	fl_path_free(p);
}

static void ring(fl_canvas *cv, float x, float y, float r, float width, fl_color c)
{
	char d[160];
	snprintf(d, sizeof d, "M%g %gA%g %g 0 1 0 %g %gA%g %g 0 1 0 %g %gZ", x - r, y, r, r, x + r, y, r, r, x - r, y);
	fl_path *p = fl_path_new();
	if (!p)
		return;
	fl_path_svg(p, d, 1, 1, 0, 0);
	fl_path_stroke(cv, p, width, c);
	fl_path_free(p);
}

static void build(fl_worldmap *m, int fw, int fh)
{
	release(m);
	int w, h;
	geometry(m, fw, fh, &w, &h);
	if (w < 40 || h < 20)
		return;
	size_t n = (size_t)w * h;
	m->base = calloc(n, 4);
	m->comp = calloc(n, 4);
	m->mask = calloc(n, 1);
	m->near = calloc(n, 2);
	uint32_t *mk = calloc(n, 4);
	if (!m->base || !m->comp || !m->mask || !m->near || !mk) {
		free(mk);
		release(m);
		return;
	}
	m->w = w;
	m->h = h;
	m->theme_sig = theme_sig();
	fl_canvas cv = {.px = m->base, .w = w, .h = h, .stride = w, .clip = {0, 0, w, h}};

	/* sea: the outline of the projection (the edge meridians) */
	fl_path *p = fl_path_new();
	float x, y;
	for (double lat = LAT_N; lat >= LAT_S - 0.001; lat -= 2) {
		project(m, -180, lat, &x, &y);
		if (lat == LAT_N)
			fl_path_move(p, x, y);
		else
			fl_path_line(p, x, y);
	}
	for (double lat = LAT_S; lat <= LAT_N + 0.001; lat += 2) {
		project(m, 180, lat, &x, &y);
		fl_path_line(p, x, y);
	}
	fl_path_close(p);
	fl_path_fill(&cv, p, fl_th.bg);
	fl_path_free(p);

	/* graticule every 30 degrees, drawn on the sea only (the land covers it) */
	fl_color gl = (fl_th.line & 0x00ffffffu) | 0xa0000000u;
	p = fl_path_new();
	for (int lon = -150; lon <= 150; lon += 30) {
		for (double lat = LAT_N; lat >= LAT_S - 0.001; lat -= 3) {
			project(m, lon, lat, &x, &y);
			if (lat == LAT_N)
				fl_path_move(p, x, y);
			else
				fl_path_line(p, x, y);
		}
	}
	for (int lat = -60; lat <= 60; lat += 30) {
		for (int lon = -180; lon <= 180; lon += 6) {
			project(m, lon, lat, &x, &y);
			if (lon == -180)
				fl_path_move(p, x, y);
			else
				fl_path_line(p, x, y);
		}
	}
	fl_path_stroke(&cv, p, 1.0f, gl);
	fl_path_free(p);

	/* land */
	p = fl_path_new();
	for (const int16_t *q = fl_wm_land; *q;) {
		int cnt = *q++;
		for (int i = 0; i < cnt; i++, q += 2) {
			project(m, q[0] / 20.0, q[1] / 20.0, &x, &y);
			if (i == 0)
				fl_path_move(p, x, y);
			else
				fl_path_line(p, x, y);
		}
		fl_path_close(p);
	}
	fl_path_fill(&cv, p, land_col());
	fl_canvas mc = {.px = mk, .w = w, .h = h, .stride = w, .clip = {0, 0, w, h}};
	fl_path_fill(&mc, p, 0xffffffffu);
	fl_path_free(p);
	for (size_t i = 0; i < n; i++)
		m->mask[i] = (uint8_t)(mk[i] >> 24);
	free(mk);

	/* borders between countries */
	p = fl_path_new();
	for (const int16_t *q = fl_wm_borders; *q;) {
		int cnt = *q++;
		for (int i = 0; i < cnt; i++, q += 2) {
			project(m, q[0] / 20.0, q[1] / 20.0, &x, &y);
			if (i == 0)
				fl_path_move(p, x, y);
			else
				fl_path_line(p, x, y);
		}
	}
	fl_path_stroke(&cv, p, 0.8f, fl_mix(land_col(), fl_th.dim, 70));
	fl_path_free(p);

	/* which zone's region every land pixel lies in */
	tz_load();
	for (int gy = 0; gy < h; gy++)
		for (int gx = 0; gx < w; gx++) {
			size_t o = (size_t)gy * w + gx;
			if (!m->mask[o])
				continue;
			double lon, lat;
			double X = (gx + 0.5) / m->sc - m->xmax, Y = m->ytop - (gy + 0.5) / m->sc;
			if (!ne_inv(X, Y, &lon, &lat))
				continue;
			float vx = (float)(cos(lat) * cos(lon)), vy = (float)(cos(lat) * sin(lon)), vz = (float)sin(lat);
			int best = -1;
			float bd = -2;
			for (int i = 0; i < nz; i++) {
				if (zones[i].lat > 90)
					continue;
				float d = vx * zvx[i] + vy * zvy[i] + vz * zvz[i];
				if (d > bd) {
					bd = d;
					best = i;
				}
			}
			m->near[o] = (uint16_t)(best + 1);
		}
}

static uint32_t blend_px(uint32_t d, fl_color c, unsigned a)
{
	return fl_over(d, c, a);
}

/* base + tint + dots for the selection */
static void compose(fl_worldmap *m, int sel)
{
	size_t n = (size_t)m->w * m->h;
	memcpy(m->comp, m->base, n * 4);
	int soff = sel >= 0 ? zones[sel].off_now : 0;
	/* tint levels (strong: the zone's own region, soft: the same time right now), softened
	 * among land pixels so the edges of the regions are not stair steps */
	int W = m->w, H = m->h, top_strong = fl_th.dark ? 170 : 150, top_soft = fl_th.dark ? 84 : 70;
	if (sel >= 0) {
		uint8_t *lvl = malloc(n);
		if (lvl) {
			for (size_t i = 0; i < n; i++) {
				unsigned id = m->near[i];
				int z = (int)id - 1;
				lvl[i] = !id ? 0 : z == sel ? (uint8_t)top_strong : zones[z].off_now == soff ? (uint8_t)top_soft : 0;
			}
			fl_color tint = fl_th.accent & 0x00ffffffu;
			for (int y = 0; y < H; y++)
				for (int x = 0; x < W; x++) {
					size_t i = (size_t)y * W + x;
					if (!m->mask[i])
						continue;
					unsigned num = 0, den = 0;
					for (int dy = -1; dy <= 1; dy++)
						for (int dx = -1; dx <= 1; dx++) {
							int xx = x + dx, yy = y + dy;
							if (xx < 0 || yy < 0 || xx >= W || yy >= H)
								continue;
							size_t j = (size_t)yy * W + xx;
							if (!m->mask[j])
								continue;
							unsigned wt = (unsigned)((2 - abs(dx)) * (2 - abs(dy)));
							num += wt * lvl[j];
							den += wt;
						}
					unsigned a = den ? num / den : 0;
					if (a)
						m->comp[i] = blend_px(m->comp[i], tint | a << 24, m->mask[i]);
				}
			free(lvl);
		}
	}
	fl_canvas cv = {.px = m->comp, .w = m->w, .h = m->h, .stride = m->w, .clip = {0, 0, m->w, m->h}};
	fl_color dim = (fl_th.dark ? fl_th.text : fl_th.dim);
	dim = (dim & 0x00ffffffu) | 0x50000000u;
	fl_color lit = fl_th.dark ? fl_th.strong : fl_th.accent;
	for (int i = 0; i < nz; i++) {
		if (zones[i].lat > 90)
			continue;
		float x, y;
		project(m, zones[i].lon, zones[i].lat, &x, &y);
		if (i == sel)
			continue;
		if (sel >= 0 && zones[i].off_now == soff)
			disc(&cv, x, y, 1.6f, lit);
		else
			disc(&cv, x, y, 1.2f, dim);
	}
	m->comp_sel = sel;
	m->comp_valid = 1;
	m->comp_stamp = stamp;
}

/* a small flat card: name, then the time in dim; (ax, ay) is where it hangs */
static void card(fl_ui *ui, fl_rect bounds, int x, int y, const char *name, const char *info, int accent)
{
	fl_font *fn = ui->f_semi, *fi = ui->f_reg;
	int nw = fl_text_width(fn, name, -1), iw = fl_text_width(fi, info, -1);
	int w = nw + 12 + iw + 20, h = 26;
	fl_rect r = {x, y, w, h};
	if (r.x + r.w > bounds.x + bounds.w - 2)
		r.x = bounds.x + bounds.w - 2 - r.w;
	if (r.x < bounds.x + 2)
		r.x = bounds.x + 2;
	if (r.y + r.h > bounds.y + bounds.h - 2)
		r.y = bounds.y + bounds.h - 2 - r.h;
	if (r.y < bounds.y + 2)
		r.y = bounds.y + 2;
	fl_fill_rect(&ui->cv, fl_r(r.x + 1, r.y + 2, r.w, r.h), 0x40000000u);
	fl_fill_rect(&ui->cv, r, fl_th.panel);
	fl_stroke_rect(&ui->cv, r, 1, accent ? fl_th.accent : fl_th.line);
	fl_text_in(&ui->cv, fn, fl_r(r.x + 10, r.y, nw + 2, r.h), FL_LEFT, 0, fl_th.strong, name);
	fl_text_in(&ui->cv, fi, fl_r(r.x + 10 + nw + 12, r.y, iw + 2, r.h), FL_LEFT, 0, fl_th.dim, info);
}

static void zone_info(fl_worldmap *m, int idx, char *info, size_t cap)
{
	struct tm tm;
	int off;
	char t[24], o[24];
	if (fl_tz_local(idx, fl_tz_time(), &tm, &off, NULL, 0) < 0) {
		info[0] = 0;
		return;
	}
	fl_tz_fmt_time(&tm, m->h24, t, sizeof t);
	fl_tz_fmt_offset(off, o, sizeof o);
	snprintf(info, cap, "%s  %s", t, o);
}

static void pin(fl_canvas *cv, float x, float y)
{
	fl_path *p = fl_path_new();
	if (!p)
		return;
	/* teardrop, tip at (x, y), head 16 px above */
	fl_path_svg(p, "M0 0C-2.5 -5 -7.5 -8.5 -7.5 -14A7.5 7.5 0 1 1 7.5 -14C7.5 -8.5 2.5 -5 0 0Z", 1, 1, x, y);
	fl_path_fill(cv, p, fl_th.accent);
	fl_path_stroke(cv, p, 1.5f, (fl_th.bg & 0x00ffffffu) | 0xd0000000u);
	fl_path_free(p);
	disc(cv, x, y - 14, 2.6f, fl_th.on_accent);
}

int fl_worldmap_draw(fl_ui *ui, uint32_t id, fl_rect r, fl_worldmap *m, int sel, int *hover)
{
	tz_load();
	int pw, ph;
	geometry(m, r.w, r.h, &pw, &ph);
	int bx = r.x + (r.w - pw) / 2, by = r.y + (r.h - ph) / 2;
	fl_rect pic = fl_r(bx, by, pw, ph);
	int res = fl_widget(ui, id, pic, 0);
	int hov = -1, ret = -1;
	if (res & (FL_HOVER | FL_DOWN)) {
		double lon, lat;
		double X = (ui->mx - bx) / m->sc - m->xmax, Y = m->ytop - (ui->my - by) / m->sc;
		if (ne_inv(X, Y, &lon, &lat))
			hov = fl_tz_nearest((float)(lat * 180 / M_PI), (float)(lon * 180 / M_PI));
	}
	if ((res & FL_CLICK) && hov >= 0)
		ret = hov;
	if (hover)
		*hover = hov;
	if (!ui->cv.px)
		return ret;

	if (m->w != pw || m->h != ph || m->theme_sig != theme_sig() || !m->base) {
		build(m, r.w, r.h);
	}
	if (!m->base)
		return ret;
	if (fl_tz_time() - stamp > 1800 || fl_tz_time() < stamp) {
		fl_tz_refresh();
		m->comp_valid = 0;
	}
	if (!m->comp_valid || m->comp_sel != sel || m->comp_stamp != stamp)
		compose(m, sel);

	fl_canvas *cv = &ui->cv;
	fl_rect vis = fl_intersect(pic, cv->clip);
	for (int y = vis.y; y < vis.y + vis.h; y++) {
		const uint32_t *src = m->comp + (size_t)(y - by) * m->w + (vis.x - bx);
		uint32_t *dst = cv->px + (size_t)y * cv->stride + vis.x;
		for (int x = 0; x < vis.w; x++) {
			uint32_t s = src[x]; /* the picture's edge pixels are premultiplied */
			unsigned a = s >> 24;
			if (a == 255) {
				dst[x] = s;
			} else if (a) {
				uint32_t d = dst[x];
				unsigned ia = 255 - a;
				unsigned rr = (s >> 16 & 255) + (d >> 16 & 255) * ia / 255;
				unsigned gg = (s >> 8 & 255) + (d >> 8 & 255) * ia / 255;
				unsigned bb = (s & 255) + (d & 255) * ia / 255;
				dst[x] = 0xff000000u | rr << 16 | gg << 8 | bb;
			}
		}
	}
	fl_canvas sub = *cv;
	sub.clip = fl_intersect(pic, cv->clip);

	/* the hovered region */
	if (hov >= 0 && hov != sel) {
		fl_color hc = (fl_th.accent & 0x00ffffffu) | 0x58000000u;
		for (int y = vis.y; y < vis.y + vis.h; y++) {
			const uint16_t *nr = m->near + (size_t)(y - by) * m->w + (vis.x - bx);
			const uint8_t *mk = m->mask + (size_t)(y - by) * m->w + (vis.x - bx);
			uint32_t *dst = cv->px + (size_t)y * cv->stride + vis.x;
			for (int x = 0; x < vis.w; x++)
				if (nr[x] == hov + 1 && mk[x])
					dst[x] = fl_over(dst[x], hc, mk[x]);
		}
	}
	float hx = 0, hy = 0;
	char name[80], info[64];
	if (hov >= 0 && zones[hov].lat <= 90 && hov != sel) {
		project(m, zones[hov].lon, zones[hov].lat, &hx, &hy);
		hx += bx;
		hy += by;
		ring(&sub, hx, hy, 4.5f, 1.4f, fl_th.strong);
		disc(&sub, hx, hy, 2.0f, fl_th.strong);
	}
	if (sel >= 0 && zones[sel].lat <= 90) {
		float px, py;
		project(m, zones[sel].lon, zones[sel].lat, &px, &py);
		px += bx;
		py += by;
		pin(&sub, px, py);
		zone_info(m, sel, info, sizeof info);
		snprintf(name, sizeof name, "%s", zones[sel].city);
		int w = fl_text_width(ui->f_semi, name, -1) + 12 + fl_text_width(ui->f_reg, info, -1) + 20;
		int cy = (int)py - 14 - 8 - 6 - 26; /* above the pin's head, else beside it */
		int cx = (int)px - w / 2;
		if (cy < pic.y + 2) {
			cy = (int)py - 14 - 13;
			cx = (int)px - 14 - w;
			if (cx < pic.x + 2)
				cx = (int)px + 14;
		}
		card(ui, pic, cx, cy, name, info, 1);
	}
	if (hov >= 0 && hov != sel && zones[hov].lat <= 90) {
		zone_info(m, hov, info, sizeof info);
		snprintf(name, sizeof name, "%s", zones[hov].city);
		int w = fl_text_width(ui->f_semi, name, -1) + 12 + fl_text_width(ui->f_reg, info, -1) + 20;
		card(ui, pic, (int)hx - w / 2, (int)hy - 12 - 26, name, info, 0);
	}
	return ret;
}

/* ---- the search field ----------------------------------------------------------------------- */

#define POP_ROW 34

int fl_tzsearch_field(fl_ui *ui, uint32_t id, fl_rect r, fl_tzsearch *s, char *buf, size_t cap)
{
	s->field = r;
	int ed = fl_textfield(ui, id, r, buf, cap, "Search for a city or country", ui->f_reg);
	int focused = ui->focus == id;
	if (strcmp(buf, s->last)) {
		snprintf(s->last, sizeof s->last, "%s", buf);
		s->n = fl_tz_search(buf, s->res, 8);
		s->row = 0;
	}
	int picked = -1;
	if (focused && s->n > 0) {
		if (ui->key == XKB_KEY_Down) {
			s->row = (s->row + 1) % s->n;
			ui->key = 0;
		} else if (ui->key == XKB_KEY_Up) {
			s->row = (s->row + s->n - 1) % s->n;
			ui->key = 0;
		}
	}
	if (focused && ui->key == XKB_KEY_Escape && buf[0]) {
		buf[0] = 0;
		ui->key = 0;
		s->n = 0;
		s->last[0] = 0;
	}
	if ((ed & FL_EDIT_ENTER) && s->n > 0)
		picked = s->res[s->row];
	if (picked >= 0) {
		buf[0] = 0;
		s->last[0] = 0;
		s->n = 0;
	}
	s->shown = buf[0] && focused;
	return picked;
}

int fl_tzsearch_covers(const fl_tzsearch *s, int x, int y)
{
	if (!s->shown)
		return 0;
	int rows = s->n ? s->n : 1;
	fl_rect pop = fl_r(s->field.x, s->field.y + s->field.h + 2, s->field.w, rows * POP_ROW + 2);
	return fl_in(pop, x, y);
}

int fl_tzsearch_popup(fl_ui *ui, uint32_t id_base, fl_tzsearch *s, char *buf)
{
	int picked = -1;
	int rows = s->n ? s->n : 1;
	fl_rect pop = fl_r(s->field.x, s->field.y + s->field.h + 2, s->field.w, rows * POP_ROW + 2);
	/* pressing a row takes the focus off the field first: keep the list until the click is done */
	int busy = ui->active >= id_base && ui->active < id_base + 8;
	if (!(s->shown || busy) || !buf[0])
		return -1;
	fl_fill_rect(&ui->cv, fl_r(pop.x + 1, pop.y + 3, pop.w, pop.h), 0x50000000u);
	fl_fill_rect(&ui->cv, pop, fl_th.panel);
	fl_stroke_rect(&ui->cv, pop, 1, fl_th.line);
	if (!s->n) {
		fl_text_in(&ui->cv, ui->f_reg, fl_r(pop.x + 1, pop.y + 1, pop.w - 2, POP_ROW), FL_LEFT, 12, fl_th.dim,
		           "No place with that name");
		return -1;
	}
	for (int i = 0; i < s->n; i++) {
		const fl_tzone *z = &zones[s->res[i]];
		fl_rect row = fl_r(pop.x + 1, pop.y + 1 + i * POP_ROW, pop.w - 2, POP_ROW);
		int res = fl_widget(ui, id_base + (uint32_t)i, row, 0);
		if (res & FL_HOVER)
			s->row = i;
		if (i == s->row)
			fl_fill_rect(&ui->cv, row, fl_th.select);
		fl_text_in(&ui->cv, ui->f_med, fl_r(row.x + 12, row.y, row.w - 24, row.h), FL_LEFT, 0, fl_th.strong, z->city);
		int cw = fl_text_width(ui->f_med, z->city, -1);
		char off[24], right[80];
		fl_tz_fmt_offset(z->off_now, off, sizeof off);
		snprintf(right, sizeof right, "%s", off);
		int rw = fl_text_width(ui->f_reg, right, -1);
		char fit[80];
		fl_text_fit(ui->f_reg, z->country, row.w - 24 - cw - 14 - rw - 14, fit, sizeof fit);
		fl_text_in(&ui->cv, ui->f_reg, fl_r(row.x + 12 + cw + 12, row.y, row.w - 24 - cw - 12, row.h), FL_LEFT, 0,
		           fl_th.dim, fit);
		fl_text_in(&ui->cv, ui->f_reg, fl_r(row.x, row.y, row.w - 12, row.h), FL_RIGHT, 0, fl_th.dim, right);
		if (res & FL_CLICK)
			picked = s->res[i];
	}
	if (picked >= 0) {
		buf[0] = 0;
		s->last[0] = 0;
		s->n = 0;
		s->shown = 0;
	}
	return picked;
}
