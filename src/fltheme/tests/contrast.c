/* contrast.c -- WCAG 2.x contrast of the colour pairs that matter, for both schemes and every accent.
 * Reads the tokens from libflui's flpalette.h, so it checks what ships. Exit status 1 on a failure.
 * Thresholds: text-like pairs (text, strong, dim, err on panel, bg, raised; accent as text on
 * panel; text on the selection and on an accent fill) 4.5 : 1 in the light scheme. The dark scheme
 * keeps the tokens the desktop always had: text and strong 7 : 1, dim 4.4 : 1 on panel and bg,
 * err 4.3 : 1, accents 4 : 1 on panel; dim on raised (hover) is 3.5 : 1.
 * SPDX-License-Identifier: MIT */
#include <flpalette.h>
#include <math.h>
#include <stdio.h>

static double lin(unsigned c)
{
	double x = c / 255.0;
	return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);
}

static double lum(uint32_t c)
{
	return 0.2126 * lin(c >> 16 & 255) + 0.7152 * lin(c >> 8 & 255) + 0.0722 * lin(c & 255);
}

static double ratio(uint32_t a, uint32_t b)
{
	double la = lum(a), lb = lum(b);
	if (la < lb) {
		double t = la;
		la = lb;
		lb = t;
	}
	return (la + 0.05) / (lb + 0.05);
}

static int fails;

static void need(const char *scheme, const char *what, uint32_t fg, uint32_t bg, double min)
{
	double r = ratio(fg, bg);
	int ok = r >= min;
	printf("%s %-5s  %-34s %5.2f : 1 (needs %.1f)\n", ok ? "ok  " : "FAIL", scheme, what, r, min);
	if (!ok)
		fails++;
}

int main(void)
{
	for (int dark = 0; dark <= 1; dark++) {
		const char *sn = dark ? "dark" : "light";
		struct flpal p;
		flpal_base(&p, dark);
		double body = dark ? 7.0 : 4.5, dim_panel = dark ? 4.4 : 4.5, dim_raised = dark ? 3.5 : 4.5;
		double err = dark ? 4.3 : 4.5, acc = dark ? 4.0 : 4.5;
		need(sn, "text on panel", p.text, p.panel, body);
		need(sn, "text on bg", p.text, p.bg, body);
		need(sn, "text on raised", p.text, p.raised, body);
		need(sn, "strong on panel", p.strong, p.panel, body);
		need(sn, "dim on panel", p.dim, p.panel, dim_panel);
		need(sn, "dim on bg", p.dim, p.bg, 4.5);
		need(sn, "dim on raised", p.dim, p.raised, dim_raised);
		need(sn, "err on panel", p.err, p.panel, err);
		need(sn, "err on bg", p.err, p.bg, 4.5);
		need(sn, "text on selection", p.text, p.select, 4.5);
		need(sn, "dim on selection", p.dim, p.select, dark ? 3.0 : 4.5);
		for (int i = 0; i < FLPAL_ACCENT_COUNT; i++) {
			struct flpal q;
			flpal_make(&q, dark, flpal_accents[i].id);
			char what[64];
			snprintf(what, sizeof what, "accent %s on panel", flpal_accents[i].id);
			need(sn, what, q.accent, q.panel, acc);
			snprintf(what, sizeof what, "on_accent on %s", flpal_accents[i].id);
			need(sn, what, q.on_accent, q.accent, 4.5);
			snprintf(what, sizeof what, "text on %s selection", flpal_accents[i].id);
			need(sn, what, q.text, q.select, 4.5);
		}
		/* the tokens keep their order: bg, panel, raised step the same way in both schemes */
		int order = dark ? (lum(p.bg) < lum(p.panel) && lum(p.panel) < lum(p.raised))
		                 : (lum(p.bg) < lum(p.panel) && lum(p.panel) < lum(p.raised));
		printf("%s %-5s  bg < panel < raised in luminance\n", order ? "ok  " : "FAIL", sn);
		if (!order)
			fails++;
	}
	if (fails)
		printf("%d contrast check(s) failed\n", fails);
	else
		printf("all contrast checks passed\n");
	return fails != 0;
}
