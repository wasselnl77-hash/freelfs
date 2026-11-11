/* gfx.c -- canvas, rectangles, anti-aliased paths and icons
 * SPDX-License-Identifier: MIT
 *
 * Paths are flattened to polylines and rasterised with a signed-area
 * accumulation buffer: exact coverage, no supersampling, one pass.
 */
#include "internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ---- rect helpers --------------------------------------------------------- */

fl_rect fl_cut_left(fl_rect *r, int w)
{
	fl_rect s = {r->x, r->y, w, r->h};
	r->x += w;
	r->w -= w;
	return s;
}

fl_rect fl_cut_right(fl_rect *r, int w)
{
	fl_rect s = {r->x + r->w - w, r->y, w, r->h};
	r->w -= w;
	return s;
}

fl_rect fl_cut_top(fl_rect *r, int h)
{
	fl_rect s = {r->x, r->y, r->w, h};
	r->y += h;
	r->h -= h;
	return s;
}

fl_rect fl_cut_bottom(fl_rect *r, int h)
{
	fl_rect s = {r->x, r->y + r->h - h, r->w, h};
	r->h -= h;
	return s;
}

fl_rect fl_intersect(fl_rect a, fl_rect b)
{
	int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
	int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
	int y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
	if (x1 < x0)
		x1 = x0;
	if (y1 < y0)
		y1 = y0;
	return (fl_rect){x0, y0, x1 - x0, y1 - y0};
}

fl_color fl_mix(fl_color a, fl_color b, int t)
{
	unsigned out = 0xff000000u;
	for (int s = 0; s <= 16; s += 8) {
		int ca = a >> s & 255, cb = b >> s & 255;
		out |= (unsigned)(ca + (cb - ca) * t / 255) << s;
	}
	return out;
}

/* ---- canvas ---------------------------------------------------------------- */

void fl_clip(fl_canvas *cv, fl_rect r)
{
	cv->clip = fl_intersect(cv->clip, r);
}

void fl_fill_rect(fl_canvas *cv, fl_rect r, fl_color c)
{
	if (!cv->px)
		return;
	r = fl_intersect(r, cv->clip);
	if (r.w <= 0 || r.h <= 0)
		return;
	for (int y = r.y; y < r.y + r.h; y++) {
		uint32_t *p = cv->px + (size_t)y * cv->stride + r.x;
		if ((c >> 24) == 255) {
			uint32_t v = c;
			for (int x = 0; x < r.w; x++)
				p[x] = v;
		} else {
			for (int x = 0; x < r.w; x++)
				p[x] = fl_over(p[x], c, 255);
		}
	}
}

void fl_stroke_rect(fl_canvas *cv, fl_rect r, int t, fl_color c)
{
	fl_fill_rect(cv, (fl_rect){r.x, r.y, r.w, t}, c);
	fl_fill_rect(cv, (fl_rect){r.x, r.y + r.h - t, r.w, t}, c);
	fl_fill_rect(cv, (fl_rect){r.x, r.y + t, t, r.h - 2 * t}, c);
	fl_fill_rect(cv, (fl_rect){r.x + r.w - t, r.y + t, t, r.h - 2 * t}, c);
}

void fl_blend_px(fl_canvas *cv, int x, int y, fl_color c, unsigned cov)
{
	if (!cv->px || !fl_in(cv->clip, x, y))
		return;
	uint32_t *p = cv->px + (size_t)y * cv->stride + x;
	*p = fl_over(*p, c, cov);
}

/* ---- paths ------------------------------------------------------------------ */

struct fl_path {
	float *pt; /* x, y pairs */
	size_t n, cap;
	size_t *sub; /* start index of each subpath, in points */
	char *closed;
	size_t nsub, subcap;
	float cx, cy; /* current point */
	float sx, sy; /* subpath start */
	float lcx, lcy; /* last control point, for S and T */
};

fl_path *fl_path_new(void)
{
	return calloc(1, sizeof(fl_path));
}

void fl_path_free(fl_path *p)
{
	if (!p)
		return;
	free(p->pt);
	free(p->sub);
	free(p->closed);
	free(p);
}

static void add_pt(fl_path *p, float x, float y)
{
	if (p->n == p->cap) {
		p->cap = p->cap ? p->cap * 2 : 64;
		p->pt = realloc(p->pt, p->cap * 2 * sizeof(float));
		if (!p->pt)
			abort();
	}
	p->pt[p->n * 2] = x;
	p->pt[p->n * 2 + 1] = y;
	p->n++;
	p->cx = x;
	p->cy = y;
}

void fl_path_move(fl_path *p, float x, float y)
{
	if (p->nsub == p->subcap) {
		p->subcap = p->subcap ? p->subcap * 2 : 8;
		p->sub = realloc(p->sub, p->subcap * sizeof(size_t));
		p->closed = realloc(p->closed, p->subcap);
		if (!p->sub || !p->closed)
			abort();
	}
	p->sub[p->nsub] = p->n;
	p->closed[p->nsub] = 0;
	p->nsub++;
	add_pt(p, x, y);
	p->sx = x;
	p->sy = y;
	p->lcx = x;
	p->lcy = y;
}

void fl_path_line(fl_path *p, float x, float y)
{
	if (!p->nsub)
		fl_path_move(p, x, y);
	else
		add_pt(p, x, y);
	p->lcx = x;
	p->lcy = y;
}

void fl_path_quad(fl_path *p, float cx, float cy, float x, float y)
{
	float x0 = p->cx, y0 = p->cy;
	float d = fabsf(x0 - 2 * cx + x) + fabsf(y0 - 2 * cy + y);
	int n = 2 + (int)sqrtf(d * 2);
	if (n > 40)
		n = 40;
	for (int i = 1; i <= n; i++) {
		float t = (float)i / n, u = 1 - t;
		add_pt(p, u * u * x0 + 2 * u * t * cx + t * t * x, u * u * y0 + 2 * u * t * cy + t * t * y);
	}
	p->lcx = cx;
	p->lcy = cy;
}

void fl_path_cubic(fl_path *p, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
	float x0 = p->cx, y0 = p->cy;
	float d = fabsf(x0 - c1x) + fabsf(y0 - c1y) + fabsf(c1x - c2x) + fabsf(c1y - c2y) +
	          fabsf(c2x - x) + fabsf(c2y - y);
	int n = 3 + (int)sqrtf(d * 1.5f);
	if (n > 60)
		n = 60;
	for (int i = 1; i <= n; i++) {
		float t = (float)i / n, u = 1 - t;
		float a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, e = t * t * t;
		add_pt(p, a * x0 + b * c1x + c * c2x + e * x, a * y0 + b * c1y + c * c2y + e * y);
	}
	p->lcx = c2x;
	p->lcy = c2y;
}

void fl_path_close(fl_path *p)
{
	if (!p->nsub)
		return;
	p->closed[p->nsub - 1] = 1;
	p->cx = p->sx;
	p->cy = p->sy;
}

/* SVG elliptical arc to (x, y), as in the SVG implementation notes F.6. */
static void path_arc(fl_path *p, float rx, float ry, float rot, int large, int sweep, float x,
                     float y)
{
	float x0 = p->cx, y0 = p->cy;
	if (rx == 0 || ry == 0 || (x0 == x && y0 == y)) {
		fl_path_line(p, x, y);
		return;
	}
	rx = fabsf(rx);
	ry = fabsf(ry);
	float phi = rot * (float)M_PI / 180, cp = cosf(phi), sp = sinf(phi);
	float dx = (x0 - x) / 2, dy = (y0 - y) / 2;
	float x1 = cp * dx + sp * dy, y1 = -sp * dx + cp * dy;
	float lam = x1 * x1 / (rx * rx) + y1 * y1 / (ry * ry);
	if (lam > 1) {
		lam = sqrtf(lam);
		rx *= lam;
		ry *= lam;
	}
	float num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
	float den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
	float co = den ? sqrtf(fmaxf(0, num / den)) : 0;
	if (large == sweep)
		co = -co;
	float cxp = co * rx * y1 / ry, cyp = -co * ry * x1 / rx;
	float cx = cp * cxp - sp * cyp + (x0 + x) / 2, cy = sp * cxp + cp * cyp + (y0 + y) / 2;
	float th1 = atan2f((y1 - cyp) / ry, (x1 - cxp) / rx);
	float th2 = atan2f((-y1 - cyp) / ry, (-x1 - cxp) / rx);
	float dth = th2 - th1;
	if (sweep && dth < 0)
		dth += 2 * (float)M_PI;
	else if (!sweep && dth > 0)
		dth -= 2 * (float)M_PI;
	int n = 4 + (int)(fabsf(dth) * fmaxf(rx, ry) / 2);
	if (n > 90)
		n = 90;
	for (int i = 1; i <= n; i++) {
		float th = th1 + dth * i / n;
		float ex = rx * cosf(th), ey = ry * sinf(th);
		add_pt(p, cp * ex - sp * ey + cx, sp * ex + cp * ey + cy);
	}
	p->cx = x;
	p->cy = y;
	p->pt[(p->n - 1) * 2] = x;
	p->pt[(p->n - 1) * 2 + 1] = y;
}

static int svg_num(const char **s, float *out)
{
	const char *q = *s;
	while (*q == ' ' || *q == ',' || *q == '\t' || *q == '\n')
		q++;
	if (!((*q >= '0' && *q <= '9') || *q == '-' || *q == '+' || *q == '.'))
		return 0;
	char *end;
	*out = strtof(q, &end);
	if (end == q)
		return 0;
	*s = end;
	return 1;
}

static int svg_flag(const char **s, int *out)
{
	const char *q = *s;
	while (*q == ' ' || *q == ',')
		q++;
	if (*q != '0' && *q != '1')
		return 0;
	*out = *q - '0';
	*s = q + 1;
	return 1;
}

int fl_path_svg(fl_path *p, const char *d, float sx, float sy, float ox, float oy)
{
	char cmd = 0;
	float cx = 0, cy = 0, startx = 0, starty = 0, lcx = 0, lcy = 0;
	char prev = 0;
	const char *s = d;
#define X(v) ((v) * sx + ox)
#define Y(v) ((v) * sy + oy)
	for (;;) {
		while (*s == ' ' || *s == ',' || *s == '\n' || *s == '\t')
			s++;
		if (!*s)
			break;
		if ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z')) {
			cmd = *s++;
			if (cmd == 'z' || cmd == 'Z') {
				fl_path_close(p);
				cx = startx;
				cy = starty;
				prev = 'z';
				continue;
			}
		} else if (!cmd || cmd == 'z' || cmd == 'Z') {
			return -1;
		}
		int rel = cmd >= 'a';
		char uc = rel ? cmd - 32 : cmd;
		float a[7];
		float bx = rel ? cx : 0, by = rel ? cy : 0;
		switch (uc) {
		case 'M':
			if (!svg_num(&s, &a[0]) || !svg_num(&s, &a[1]))
				return -1;
			cx = bx + a[0];
			cy = by + a[1];
			fl_path_move(p, X(cx), Y(cy));
			startx = cx;
			starty = cy;
			cmd = rel ? 'l' : 'L'; /* further pairs are line-tos */
			break;
		case 'L':
			if (!svg_num(&s, &a[0]) || !svg_num(&s, &a[1]))
				return -1;
			cx = bx + a[0];
			cy = by + a[1];
			fl_path_line(p, X(cx), Y(cy));
			break;
		case 'H':
			if (!svg_num(&s, &a[0]))
				return -1;
			cx = bx + a[0];
			fl_path_line(p, X(cx), Y(cy));
			break;
		case 'V':
			if (!svg_num(&s, &a[0]))
				return -1;
			cy = by + a[0];
			fl_path_line(p, X(cx), Y(cy));
			break;
		case 'C':
			for (int i = 0; i < 6; i++)
				if (!svg_num(&s, &a[i]))
					return -1;
			fl_path_cubic(p, X(bx + a[0]), Y(by + a[1]), X(bx + a[2]), Y(by + a[3]),
			              X(bx + a[4]), Y(by + a[5]));
			lcx = bx + a[2];
			lcy = by + a[3];
			cx = bx + a[4];
			cy = by + a[5];
			break;
		case 'S': {
			for (int i = 0; i < 4; i++)
				if (!svg_num(&s, &a[i]))
					return -1;
			float c1x = prev == 'C' ? 2 * cx - lcx : cx, c1y = prev == 'C' ? 2 * cy - lcy : cy;
			fl_path_cubic(p, X(c1x), Y(c1y), X(bx + a[0]), Y(by + a[1]), X(bx + a[2]),
			              Y(by + a[3]));
			lcx = bx + a[0];
			lcy = by + a[1];
			cx = bx + a[2];
			cy = by + a[3];
			break;
		}
		case 'Q':
			for (int i = 0; i < 4; i++)
				if (!svg_num(&s, &a[i]))
					return -1;
			fl_path_quad(p, X(bx + a[0]), Y(by + a[1]), X(bx + a[2]), Y(by + a[3]));
			cx = bx + a[2];
			cy = by + a[3];
			break;
		case 'A': {
			int large, sweep;
			if (!svg_num(&s, &a[0]) || !svg_num(&s, &a[1]) || !svg_num(&s, &a[2]) ||
			    !svg_flag(&s, &large) || !svg_flag(&s, &sweep) || !svg_num(&s, &a[3]) ||
			    !svg_num(&s, &a[4]))
				return -1;
			path_arc(p, a[0] * sx, a[1] * sy, a[2], large, sweep, X(bx + a[3]), Y(by + a[4]));
			cx = bx + a[3];
			cy = by + a[4];
			break;
		}
		default:
			return -1;
		}
		prev = uc == 'S' ? 'C' : uc;
	}
#undef X
#undef Y
	return 0;
}

/* ---- rasteriser ---------------------------------------------------------------- */

struct acc {
	float *a; /* (w + 2) * h */
	int w, h, x0, y0;
};

static void acc_line(struct acc *ac, float x0, float y0, float x1, float y1)
{
	/* to buffer coordinates */
	x0 -= ac->x0;
	x1 -= ac->x0;
	y0 -= ac->y0;
	y1 -= ac->y0;
	if (y0 == y1)
		return;
	float dir = 1;
	if (y0 > y1) {
		float t;
		t = x0, x0 = x1, x1 = t;
		t = y0, y0 = y1, y1 = t;
		dir = -1;
	}
	if (y1 <= 0 || y0 >= ac->h)
		return;
	float dxdy = (x1 - x0) / (y1 - y0);
	float x = x0;
	if (y0 < 0) {
		x -= y0 * dxdy;
		y0 = 0;
	}
	int ys = (int)y0, ye = (int)ceilf(y1 < ac->h ? y1 : ac->h);
	for (int y = ys; y < ye; y++) {
		float *row = ac->a + (size_t)y * (ac->w + 2);
		float dy = fminf((float)(y + 1), y1) - fmaxf((float)y, y0);
		float xn = x + dxdy * dy;
		float d = dy * dir;
		float xa = x < xn ? x : xn, xb = x < xn ? xn : x;
		/* clamp horizontally: outside the buffer only the sign matters */
		if (xa < 0)
			xa = 0;
		if (xb < 0)
			xb = 0;
		if (xa > ac->w)
			xa = ac->w;
		if (xb > ac->w)
			xb = ac->w;
		float xaf = floorf(xa);
		int xai = (int)xaf, xbi = (int)ceilf(xb);
		if (xbi <= xai + 1) {
			float xm = 0.5f * (xa + xb) - xaf;
			row[xai] += d - d * xm;
			row[xai + 1] += d * xm;
		} else {
			float s = 1.0f / (xb - xa);
			float x0f = xa - xaf;
			float a0 = 0.5f * s * (1 - x0f) * (1 - x0f);
			float x1f = xb - ceilf(xb) + 1;
			float am = 0.5f * s * x1f * x1f;
			row[xai] += d * a0;
			if (xbi == xai + 2) {
				row[xai + 1] += d * (1 - a0 - am);
			} else {
				float a1 = s * (1.5f - x0f);
				row[xai + 1] += d * (a1 - a0);
				for (int xi = xai + 2; xi < xbi - 1; xi++)
					row[xi] += d * s;
				float a2 = a1 + (float)(xbi - xai - 3) * s;
				row[xbi - 1] += d * (1 - a2 - am);
			}
			row[xbi] += d * am;
		}
		x = xn;
	}
}

static int acc_open(struct acc *ac, fl_canvas *cv, float minx, float miny, float maxx, float maxy)
{
	fl_rect b = {(int)floorf(minx), (int)floorf(miny), (int)ceilf(maxx) - (int)floorf(minx),
	             (int)ceilf(maxy) - (int)floorf(miny)};
	b = fl_intersect(b, cv->clip);
	if (b.w <= 0 || b.h <= 0)
		return -1;
	ac->w = b.w;
	ac->h = b.h;
	ac->x0 = b.x;
	ac->y0 = b.y;
	ac->a = calloc((size_t)(b.w + 2) * b.h, sizeof(float));
	return ac->a ? 0 : -1;
}

static void acc_flush(struct acc *ac, fl_canvas *cv, fl_color c)
{
	for (int y = 0; y < ac->h; y++) {
		float *row = ac->a + (size_t)y * (ac->w + 2), sum = 0;
		uint32_t *dst = cv->px + (size_t)(y + ac->y0) * cv->stride + ac->x0;
		for (int x = 0; x < ac->w; x++) {
			sum += row[x];
			float v = fabsf(sum);
			if (v > 1)
				v = 1;
			unsigned cov = (unsigned)(v * 255 + 0.5f);
			if (cov)
				dst[x] = fl_over(dst[x], c, cov);
		}
	}
	free(ac->a);
}

static void bounds(const fl_path *p, float pad, float *x0, float *y0, float *x1, float *y1)
{
	*x0 = *y0 = 1e9f;
	*x1 = *y1 = -1e9f;
	for (size_t i = 0; i < p->n; i++) {
		float x = p->pt[i * 2], y = p->pt[i * 2 + 1];
		*x0 = fminf(*x0, x);
		*y0 = fminf(*y0, y);
		*x1 = fmaxf(*x1, x);
		*y1 = fmaxf(*y1, y);
	}
	*x0 -= pad;
	*y0 -= pad;
	*x1 += pad;
	*y1 += pad;
}

void fl_path_fill(fl_canvas *cv, const fl_path *p, fl_color c)
{
	if (!cv->px || !p->n)
		return;
	float x0, y0, x1, y1;
	bounds(p, 1, &x0, &y0, &x1, &y1);
	struct acc ac;
	if (acc_open(&ac, cv, x0, y0, x1, y1) < 0)
		return;
	for (size_t s = 0; s < p->nsub; s++) {
		size_t b = p->sub[s], e = s + 1 < p->nsub ? p->sub[s + 1] : p->n;
		for (size_t i = b; i + 1 < e; i++)
			acc_line(&ac, p->pt[i * 2], p->pt[i * 2 + 1], p->pt[i * 2 + 2], p->pt[i * 2 + 3]);
		if (e > b + 1) /* fills close every subpath */
			acc_line(&ac, p->pt[(e - 1) * 2], p->pt[(e - 1) * 2 + 1], p->pt[b * 2], p->pt[b * 2 + 1]);
	}
	acc_flush(&ac, cv, c);
}

/* Add polygon pts[0..n) with a fixed winding so overlapping pieces add up. */
static void poly(struct acc *ac, const float *pts, int n)
{
	float area = 0;
	for (int i = 0; i < n; i++) {
		int j = (i + 1) % n;
		area += pts[i * 2] * pts[j * 2 + 1] - pts[j * 2] * pts[i * 2 + 1];
	}
	if (area < 0) {
		for (int i = n - 1; i >= 0; i--) {
			int j = (i + n - 1) % n;
			acc_line(ac, pts[i * 2], pts[i * 2 + 1], pts[j * 2], pts[j * 2 + 1]);
		}
	} else {
		for (int i = 0; i < n; i++) {
			int j = (i + 1) % n;
			acc_line(ac, pts[i * 2], pts[i * 2 + 1], pts[j * 2], pts[j * 2 + 1]);
		}
	}
}

void fl_path_stroke(fl_canvas *cv, const fl_path *p, float width, fl_color c)
{
	if (!cv->px || !p->n)
		return;
	float hw = width / 2;
	float x0, y0, x1, y1;
	bounds(p, hw * 4 + 1, &x0, &y0, &x1, &y1);
	struct acc ac;
	if (acc_open(&ac, cv, x0, y0, x1, y1) < 0)
		return;
	for (size_t s = 0; s < p->nsub; s++) {
		size_t b = p->sub[s], e = s + 1 < p->nsub ? p->sub[s + 1] : p->n;
		/* drop zero-length segments */
		size_t cnt = e - b;
		float *q = malloc((cnt + 1) * 2 * sizeof(float));
		if (!q)
			continue;
		size_t m = 0;
		for (size_t i = b; i < e; i++) {
			float x = p->pt[i * 2], y = p->pt[i * 2 + 1];
			if (m && fabsf(q[(m - 1) * 2] - x) < 1e-4f && fabsf(q[(m - 1) * 2 + 1] - y) < 1e-4f)
				continue;
			q[m * 2] = x;
			q[m * 2 + 1] = y;
			m++;
		}
		int closed = p->closed[s];
		if (closed && m > 2 && fabsf(q[0] - q[(m - 1) * 2]) < 1e-4f &&
		    fabsf(q[1] - q[(m - 1) * 2 + 1]) < 1e-4f)
			m--;
		if (m == 1) { /* a dot: a square */
			float sq[8] = {q[0] - hw, q[1] - hw, q[0] + hw, q[1] - hw, q[0] + hw, q[1] + hw,
			               q[0] - hw, q[1] + hw};
			poly(&ac, sq, 4);
		}
		size_t nseg = closed ? m : (m ? m - 1 : 0);
		for (size_t i = 0; i < nseg; i++) {
			float ax = q[i * 2], ay = q[i * 2 + 1];
			size_t j = (i + 1) % m;
			float bx = q[j * 2], by = q[j * 2 + 1];
			float dx = bx - ax, dy = by - ay, len = sqrtf(dx * dx + dy * dy);
			float nx = -dy / len * hw, ny = dx / len * hw;
			float quad[8] = {ax + nx, ay + ny, bx + nx, by + ny, bx - nx, by - ny, ax - nx, ay - ny};
			poly(&ac, quad, 4);
			/* miter join at b with the next segment */
			if (closed || i + 1 < nseg) {
				size_t k = (j + 1) % m;
				float cx2 = q[k * 2], cy2 = q[k * 2 + 1];
				float ex = cx2 - bx, ey = cy2 - by, el = sqrtf(ex * ex + ey * ey);
				if (el < 1e-6f)
					continue;
				float mx = -ey / el * hw, my = ex / el * hw;
				float cross = dx * ey - dy * ex;
				if (fabsf(cross) < 1e-6f * len * el)
					continue;
				float sgn = cross > 0 ? -1 : 1; /* outer side */
				float p1x = bx + sgn * nx, p1y = by + sgn * ny;
				float p2x = bx + sgn * mx, p2y = by + sgn * my;
				float cs = (nx * mx + ny * my) / (hw * hw); /* cos of the turn */
				float k2 = 1 / (1 + cs);
				float mpx = bx + sgn * (nx + mx) * k2, mpy = by + sgn * (ny + my) * k2;
				if (k2 * 2 > 16) { /* miter limit 4: bevel */
					float tri[6] = {bx, by, p1x, p1y, p2x, p2y};
					poly(&ac, tri, 3);
				} else {
					float qd[8] = {bx, by, p1x, p1y, mpx, mpy, p2x, p2y};
					poly(&ac, qd, 4);
				}
			}
		}
		free(q);
	}
	acc_flush(&ac, cv, c);
}

/* ---- icons ---------------------------------------------------------------------- */

/* Paths in the mockups' own viewBoxes. stroke == 0: filled. */
struct icon_part {
	const char *d;
	float stroke;
};
struct icon_def {
	float vw, vh;
	struct icon_part part[3];
};

static const struct icon_def icons[FL_ICON_COUNT] = {
	[FL_ICON_MENU] = {20, 20, {{"M3.77 2.31l13.92 1.46 -0.36 3.38 -13.92 -1.46zM3.22 7.58l9.95 1.05 -0.36 3.38 -9.95 -1.05zM2.66 12.85l5.97 0.63 -0.36 3.38 -5.97 -0.63z", 0}}},
	[FL_ICON_FOLDER] = {16, 16, {{"M1.5 3.5h5l1.5 2h6.5v7.5h-13z", 1.2f}}},
	[FL_ICON_TERMINAL] = {16, 16, {{"M2 3.5l4 4-4 4M8 12.5h6", 1.2f}}},
	[FL_ICON_WIFI] = {16, 16,
	                  {{"M1.5 6a9.5 9.5 0 0 1 13 0M4 8.8a6 6 0 0 1 8 0M6.4 11.3a2.4 2.4 0 0 1 3.2 0", 1.4f}}},
	[FL_ICON_VOLUME] = {16, 16,
	                    {{"M2 6v4h2.5L8 13V3L4.5 6zM10.5 5.5a3.5 3.5 0 0 1 0 5M12.5 3.5a6.3 6.3 0 0 1 0 9", 1.4f}}},
	[FL_ICON_BATTERY] = {20, 16, {{"M1.5 4h15v8h-15zM18.5 6.5v3", 1.4f}, {"M3.5 6h8v4h-8z", 0}}},
	[FL_ICON_MINIMIZE] = {12, 12, {{"M2 9.5h8", 1}}},
	[FL_ICON_MAXIMIZE] = {12, 12, {{"M2.5 2.5h7v7h-7z", 1}}},
	[FL_ICON_RESTORE] = {12, 12, {{"M4.5 4.5h5v5h-5zM4.5 4.5v-2h5v5h-2", 1}}},
	[FL_ICON_CLOSE] = {12, 12, {{"M2.5 2.5l7 7M9.5 2.5l-7 7", 1}}},
	[FL_ICON_BACK] = {14, 14, {{"M9 2.5L4.5 7 9 11.5", 1.4f}}},
	[FL_ICON_FORWARD] = {14, 14, {{"M5 2.5L9.5 7 5 11.5", 1.4f}}},
	[FL_ICON_SETTINGS] = {16, 16,
	                      {{"M1.5 4h13M1.5 8h13M1.5 12h13", 1.2f}, {"M4 2.5h2.5v3H4zM9.5 6.5H12v3H9.5zM5.5 10.5H8v3H5.5z", 0}}},
	[FL_ICON_WINDOW] = {16, 16, {{"M2 3h12v10H2zM2 6h12", 1.2f}}},
	[FL_ICON_FILE] = {16, 16, {{"M3.5 1.5h6l3 3v10h-9zM9.5 1.5v3h3", 1.2f}}},
	[FL_ICON_DOC] = {16, 16, {{"M3.5 1.5h6l3 3v10h-9zM9.5 1.5v3h3M5.5 8h5M5.5 10.5h5", 1.2f}}},
	[FL_ICON_DRIVE] = {16, 16, {{"M1.5 9h13v4.5h-13zM3 9l1.5-5.5h7L13 9", 1.2f}, {"M11 11h1.8v1.2H11z", 0}}},
	[FL_ICON_HOME] = {16, 16, {{"M2 7.5L8 2l6 5.5M3.5 6.5v7h3.5v-4h2v4h3.5v-7", 1.2f}}},
	[FL_ICON_TRASH] = {16, 16, {{"M2.5 4h11M6 4V2.5h4V4M4 4l.7 9.5h6.6L12 4M6.8 6.5v5M9.2 6.5v5", 1.2f}}},
	[FL_ICON_MUTE] = {16, 16, {{"M2 6v4h2.5L8 13V3L4.5 6zM10.5 6l4 4M14.5 6l-4 4", 1.4f}}},
	[FL_ICON_ETHERNET] = {16, 16, {{"M2.5 3h11v6.5h-11zM8 9.5v3M4 12.5h8M5.5 5.5v2M8 5.5v2M10.5 5.5v2", 1.2f}}},
	[FL_ICON_UP] = {14, 14, {{"M2.5 9L7 4.5 11.5 9", 1.4f}}},
	[FL_ICON_PLUS] = {14, 14, {{"M7 2.5v9M2.5 7h9", 1.4f}}},
	[FL_ICON_FOLDER_LG] = {24, 24, {{"M2 5h7.5l2.5 3H22v12H2z", 0}}},
	[FL_ICON_FILE_LG] = {24, 24, {{"M5 2h9l5 5v15H5zM14 2v5h5", 1.4f}}},
	[FL_ICON_DOC_LG] = {24, 24, {{"M5 2h9l5 5v15H5zM14 2v5h5M8.5 12h7M8.5 15.5h7M8.5 19h4", 1.4f}}},
	[FL_ICON_DISPLAY] = {16, 16, {{"M2 3h12v8.5H2zM5.5 14h5M8 11.5V14", 1.2f}}},
	[FL_ICON_MOUSE] = {16, 16, {{"M4.5 6.5a3.5 3.5 0 0 1 7 0v3a3.5 3.5 0 0 1 -7 0zM8 3v4", 1.2f}}},
	[FL_ICON_KEYBOARD] = {16, 16, {{"M1.5 4h13v8h-13zM4 6.8h1M7.5 6.8h1M11 6.8h1M4.5 9.5h7", 1.2f}}},
	[FL_ICON_CLOCK] = {16, 16, {{"M2 8a6 6 0 1 0 12 0a6 6 0 1 0 -12 0M8 4.5V8l2.5 1.6", 1.2f}}},
};

void fl_icon_draw(fl_canvas *cv, fl_icon ic, int x, int y, int size, fl_color c)
{
	if (!cv->px || (unsigned)ic >= FL_ICON_COUNT)
		return;
	const struct icon_def *d = &icons[ic];
	float sc = (float)size / (d->vw > d->vh ? d->vw : d->vh);
	/* centre the viewBox in the square */
	float ox = x + (size - d->vw * sc) / 2, oy = y + (size - d->vh * sc) / 2;
	for (int i = 0; i < 3 && d->part[i].d; i++) {
		fl_path *p = fl_path_new();
		if (!p)
			return;
		fl_path_svg(p, d->part[i].d, sc, sc, ox, oy);
		if (d->part[i].stroke > 0)
			fl_path_stroke(cv, p, d->part[i].stroke * sc, c);
		else
			fl_path_fill(cv, p, c);
		fl_path_free(p);
	}
}

void fl_icon_battery(fl_canvas *cv, int x, int y, int size, fl_color c, int pct)
{
	if (!cv->px)
		return;
	float sc = (float)size / 16, ox = x, oy = y;
	fl_path *p = fl_path_new();
	if (!p)
		return;
	fl_path_svg(p, "M1.5 4h15v8h-15zM18.5 6.5v3", sc, sc, ox, oy);
	fl_path_stroke(cv, p, 1.4f * sc, c);
	fl_path_free(p);
	if (pct < 0)
		pct = 0;
	if (pct > 100)
		pct = 100;
	int w = (int)(11 * sc * pct / 100 + 0.5f); /* the inside is 11 units wide */
	if (w < 1 && pct > 0)
		w = 1;
	fl_fill_rect(cv, (fl_rect){(int)(ox + 3.5f * sc + 0.5f), (int)(oy + 6 * sc + 0.5f), w,
	                           (int)(4 * sc + 0.5f)}, c);
}

fl_icon fl_icon_for_app(const char *id)
{
	if (!id)
		return FL_ICON_WINDOW;
	if (!strcmp(id, "flterm") || strstr(id, "terminal"))
		return FL_ICON_TERMINAL;
	if (!strcmp(id, "flsettings") || strstr(id, "settings"))
		return FL_ICON_SETTINGS;
	if (!strcmp(id, "flfiles") || strstr(id, "files"))
		return FL_ICON_FOLDER;
	if (!strcmp(id, "fledit") || strstr(id, "edit"))
		return FL_ICON_DOC;
	return FL_ICON_WINDOW;
}
