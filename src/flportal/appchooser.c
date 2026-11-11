/* flportal -- the app chooser and the Allow / Deny question
 * SPDX-License-Identifier: MIT
 *
 * AppChooser: what OpenURI shows when a link or file has no single obvious program. A list of
 * the programs that have a .desktop file; the ones the frontend recommends (they handle the
 * type) come first. Access: the question xdg-desktop-portal asks before it lets an app do
 * something sensitive (a screenshot without a dialog, ...).
 */
#include "flportal.h"

#include <gio/gdesktopappinfo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { ID_SEARCH = 1, ID_OK, ID_CANCEL, ID_LIST, ID_SCROLL, ID_ROW = 1000 };
enum { ROW_H = 46, HEAD_H = 28 };

struct app {
	char *id;   /* "org.gnome.gedit", without .desktop */
	char *name;
	char *sub;  /* the comment or the command */
	int rec;
};

struct ac {
	struct dlg d;
	fl_font *small, *head;
	struct app *apps;
	int n;
	char *heading, *what;
	char query[64];
	int cur, scroll, inited;
	int pick; /* queued accept */
};

static int cmp_app(const void *a, const void *b)
{
	const struct app *x = a, *y = b;
	if (x->rec != y->rec) /* recommended first, in the frontend's order (default handler first) */
		return y->rec - x->rec;
	return strcasecmp(x->name, y->name);
}

static void apps_free(struct ac *a)
{
	for (int i = 0; i < a->n; i++) {
		g_free(a->apps[i].id);
		g_free(a->apps[i].name);
		g_free(a->apps[i].sub);
	}
	g_free(a->apps);
	a->apps = NULL;
	a->n = 0;
}

static void apps_load(struct ac *a, const char *const *choices, const char *last)
{
	apps_free(a);
	GList *all = g_app_info_get_all();
	int cnt = (int)g_list_length(all);
	a->apps = g_new0(struct app, cnt ? (gsize)cnt : 1);
	for (GList *l = all; l; l = l->next) {
		GAppInfo *ai = l->data;
		if (!g_app_info_should_show(ai))
			continue;
		const char *id = g_app_info_get_id(ai);
		if (!id)
			continue;
		char *sid = g_strdup(id);
		size_t n = strlen(sid);
		if (n > 8 && !strcmp(sid + n - 8, ".desktop"))
			sid[n - 8] = 0;
		struct app *p = &a->apps[a->n];
		p->id = sid;
		p->name = g_strdup(g_app_info_get_name(ai));
		const char *d = g_app_info_get_description(ai);
		p->sub = g_strdup(d && *d ? d : g_app_info_get_executable(ai) ? g_app_info_get_executable(ai) : "");
		for (int i = 0; choices && choices[i]; i++)
			if (!strcmp(choices[i], sid))
				p->rec = 1000 - i;
		a->n++;
	}
	g_list_free_full(all, g_object_unref);
	qsort(a->apps, (size_t)a->n, sizeof *a->apps, cmp_app);
	a->cur = a->n ? 0 : -1;
	if (last && *last)
		for (int i = 0; i < a->n; i++)
			if (!strcmp(a->apps[i].id, last))
				a->cur = i;
	a->scroll = 0;
}

static int visible(const struct ac *a, int i)
{
	if (!a->query[0])
		return 1;
	return strcasestr(a->apps[i].name, a->query) || strcasestr(a->apps[i].id, a->query);
}

static void answer(struct ac *a)
{
	if (a->cur < 0 || !a->d.req)
		return;
	GVariantBuilder b;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", "choice", g_variant_new_string(a->apps[a->cur].id));
	portal_reply(a->d.req, 0, g_variant_builder_end(&b));
}

static void ac_draw(fl_ui *ui, void *ud)
{
	struct ac *a = ud;
	fl_canvas *cv = &ui->cv;
	if (!a->d.req)
		return;
	fl_rect c = fl_win_content(ui->win);
	if (!a->inited) {
		ui->focus = ID_SEARCH;
		a->inited = 1;
	}
	fl_fill_rect(cv, c, fl_th.bg);
	fl_rect area = c;
	fl_rect top = fl_cut_top(&area, 94);
	fl_rect bar = fl_cut_bottom(&area, 58);

	fl_fill_rect(cv, top, fl_th.panel);
	fl_fill_rect(cv, fl_r(top.x, top.y + top.h - 1, top.w, 1), fl_th.line);
	fl_text(cv, ui->f_semi, top.x + 16, top.y + 12, fl_th.strong, a->heading, -1);
	if (a->what && *a->what) {
		char fit[300];
		fl_text_fit(a->small, a->what, top.w - 32, fit, sizeof fit);
		fl_text(cv, a->small, top.x + 16, top.y + 32, fl_th.dim, fit, -1);
	}
	int sr = fl_textfield(ui, ID_SEARCH, fl_r(top.x + 16, top.y + 56, top.w - 32, 30), a->query, sizeof a->query,
	                      "Search", ui->f_reg);
	if (sr & FL_EDIT_CHANGED) {
		a->scroll = 0;
		a->cur = -1;
		for (int i = 0; i < a->n; i++)
			if (visible(a, i)) {
				a->cur = i;
				break;
			}
	}

	/* the list: indexes of the visible apps, with small headers between the groups */
	int res = fl_widget(ui, ID_LIST, area, FL_FOCUSABLE | FL_NO_FOCUS_RING);
	int nvis = 0, vis_cur = -1, rec_n = 0;
	int *idx = g_new(int, a->n ? (gsize)a->n : 1);
	for (int i = 0; i < a->n; i++)
		if (visible(a, i)) {
			if (i == a->cur)
				vis_cur = nvis;
			rec_n += a->apps[i].rec ? 1 : 0;
			idx[nvis++] = i;
		}
	int heads = !a->query[0] && rec_n > 0 && rec_n < nvis;
	int content = nvis * ROW_H + (a->query[0] || rec_n == 0 ? 0 : HEAD_H * (heads ? 2 : 1));
	if ((res & FL_HOVER) && ui->wheel) {
		a->scroll += ui->wheel * 56;
		ui->wheel = 0;
	}
	/* arrows move, Enter chooses (also while the search field has the focus) */
	if (ui->key && nvis) {
		int to = vis_cur, handled = 1;
		switch (ui->key) {
		case XKB_KEY_Down: to = vis_cur < 0 ? 0 : vis_cur + 1; break;
		case XKB_KEY_Up: to = vis_cur < 0 ? 0 : vis_cur - 1; break;
		case XKB_KEY_Page_Down: to = vis_cur + 8; break;
		case XKB_KEY_Page_Up: to = vis_cur - 8; break;
		case XKB_KEY_Return:
		case XKB_KEY_KP_Enter: a->pick = 1; break;
		default: handled = 0;
		}
		if (handled) {
			if (to != vis_cur) {
				if (to < 0)
					to = 0;
				if (to >= nvis)
					to = nvis - 1;
				a->cur = idx[to];
				int y0 = to * ROW_H + (heads ? HEAD_H * (to >= rec_n ? 2 : 1) : 0);
				if (y0 < a->scroll)
					a->scroll = y0;
				if (y0 + ROW_H > a->scroll + area.h)
					a->scroll = y0 + ROW_H - area.h;
			}
			ui->key = 0;
		}
	}
	int max = content > area.h ? content - area.h : 0;
	if (a->scroll > max)
		a->scroll = max;
	if (a->scroll < 0)
		a->scroll = 0;

	fl_rect save = cv->clip;
	fl_clip(cv, area);
	int y = area.y - a->scroll;
	if (heads) {
		fl_text_ex(cv, a->head, area.x + 16, y + 9, fl_th.dim, "RECOMMENDED", -1, 56);
		y += HEAD_H;
	}
	for (int k = 0; k < nvis; k++) {
		if (heads && k == rec_n) {
			fl_text_ex(cv, a->head, area.x + 16, y + 9, fl_th.dim, "OTHER APPLICATIONS", -1, 56);
			y += HEAD_H;
		}
		struct app *p = &a->apps[idx[k]];
		fl_rect r = fl_r(area.x, y, area.w - (max ? 8 : 0), ROW_H);
		y += ROW_H;
		if (r.y + r.h < area.y || r.y > area.y + area.h)
			continue;
		int cr = fl_widget(ui, ID_ROW + (uint32_t)k, r, 0);
		int sel = idx[k] == a->cur;
		if (sel)
			fl_fill_rect(cv, r, fl_th.select);
		else if (cr & FL_HOVER)
			fl_fill_rect(cv, r, fl_th.raised);
		if (sel)
			fl_fill_rect(cv, fl_r(r.x, r.y, 3, r.h), fl_th.accent);
		ui_tile(ui, fl_r(r.x + 16, r.y + 7, 32, 32), p->name);
		char fit[300];
		fl_text_fit(ui->f_med, p->name, r.w - 72, fit, sizeof fit);
		fl_text(cv, ui->f_med, r.x + 60, r.y + 6, sel ? fl_th.strong : fl_th.text, fit, -1);
		fl_text_fit(a->small, p->sub, r.w - 72, fit, sizeof fit);
		fl_text(cv, a->small, r.x + 60, r.y + 25, fl_th.dim, fit, -1);
		if ((cr & FL_HOVER) && (ui->press & FL_BTN_L)) {
			a->cur = idx[k];
			if (ui->clicks >= 2)
				a->pick = 1;
		}
	}
	cv->clip = save;
	if (!nvis)
		fl_text_in(cv, ui->f_reg, area, FL_CENTER, 0, fl_th.dim, a->n ? "No program matches" : "No programs found");
	if (max)
		fl_scrollbar(ui, ID_SCROLL, fl_r(area.x + area.w - 6, area.y + 2, 4, area.h - 4), &a->scroll, content, area.h);
	g_free(idx);

	fl_fill_rect(cv, bar, fl_th.panel);
	fl_fill_rect(cv, fl_r(bar.x, bar.y, bar.w, 1), fl_th.line);
	int by = bar.y + (bar.h - 34) / 2;
	if (fl_button(ui, ID_OK, fl_r(bar.x + bar.w - 14 - 112, by, 112, 34), "Open", FL_BTN_ACCENT))
		a->pick = 1;
	if (fl_button(ui, ID_CANCEL, fl_r(bar.x + bar.w - 14 - 112 - 8 - 96, by, 96, 34), "Cancel", FL_BTN_OUTLINE) ||
	    ui->key == XKB_KEY_Escape) {
		ui->key = 0;
		portal_reply(a->d.req, 1, NULL);
		return;
	}
	if (a->pick) {
		a->pick = 0;
		answer(a);
	}
}

static void ac_destroy(struct dlg *d)
{
	struct ac *a = (struct ac *)d;
	apps_free(a);
	g_free(a->heading);
	g_free(a->what);
	if (a->small)
		fl_font_close(a->small);
	if (a->head)
		fl_font_close(a->head);
	g_free(a);
}

static void ac_test(struct dlg *d, int accept)
{
	if (accept)
		answer((struct ac *)d);
}

static void ac_update(struct dlg *d, GVariant *p)
{
	struct ac *a = (struct ac *)d;
	char **choices;
	const char *handle;
	g_variant_get(p, "(&o^as)", &handle, &choices);
	char *keep = a->cur >= 0 ? g_strdup(a->apps[a->cur].id) : NULL;
	apps_load(a, (const char *const *)choices, keep);
	g_free(keep);
	g_strfreev(choices);
	fl_win_redraw(d->win);
}

void ac_open(struct req *r, const char *const *choices, GVariant *options)
{
	struct ac *a = g_new0(struct ac, 1);
	a->d.destroy = ac_destroy;
	a->d.test = ac_test;
	a->d.update = ac_update;
	a->small = fl_font_ui(400, 12);
	a->head = fl_font_ui(600, 10.5f);
	const char *heading = NULL, *last = NULL, *fname = NULL, *ctype = NULL;
	g_variant_lookup(options, "heading", "&s", &heading);
	g_variant_lookup(options, "last_choice", "&s", &last);
	g_variant_lookup(options, "filename", "&s", &fname);
	g_variant_lookup(options, "content_type", "&s", &ctype);
	a->heading = g_strdup(heading && *heading ? heading : "Open with");
	if (fname && *fname)
		a->what = g_strdup(fname);
	else if (ctype && *ctype)
		a->what = g_strdup(ctype);
	apps_load(a, choices, last);
	fl_win_opts o = {
		.title = "Open with", .app_id = "flportal", .w = 440, .h = 560, .min_w = 340, .min_h = 360,
		.decorated = 1, .resizable = 1, .draw = ac_draw,
	};
	portal_dialog_open(r, &a->d, &o);
}

/* ---- Access ------------------------------------------------------------------------------ */

struct acc {
	struct dlg d;
	char *title, *subtitle, *body, *grant, *deny;
	GVariant *choices; /* a(ssa(ss)s) or NULL: answered with their initial values, there is no UI for them */
	fl_font *small;
};

static void acc_answer(struct acc *a, unsigned response)
{
	if (!a->d.req)
		return;
	GVariantBuilder b;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	if (response == 0 && a->choices) {
		GVariantBuilder cb;
		g_variant_builder_init(&cb, G_VARIANT_TYPE("a(ss)"));
		for (gsize i = 0; i < g_variant_n_children(a->choices); i++) {
			const char *id, *label, *initial;
			GVariant *opts;
			g_variant_get_child(a->choices, i, "(&s&s@a(ss)&s)", &id, &label, &opts, &initial);
			g_variant_builder_add(&cb, "(ss)", id, initial);
			g_variant_unref(opts);
		}
		g_variant_builder_add(&b, "{sv}", "choices", g_variant_builder_end(&cb));
	}
	portal_reply(a->d.req, response, g_variant_builder_end(&b));
}

static void acc_draw(fl_ui *ui, void *ud)
{
	struct acc *a = ud;
	fl_canvas *cv = &ui->cv;
	if (!a->d.req)
		return;
	fl_rect c = fl_win_content(ui->win);
	fl_fill_rect(cv, c, fl_th.panel);
	fl_fill_rect(cv, fl_r(c.x, c.y, c.w, 3), fl_th.accent);
	int x = c.x + 24, w = c.w - 48, y = c.y + 26;
	char fit[400];
	fl_text_fit(ui->f_semi, a->title, w, fit, sizeof fit);
	fl_text(cv, ui->f_semi, x, y, fl_th.strong, fit, -1);
	y += fl_font_height(ui->f_semi) + 8;
	if (*a->subtitle)
		y += ui_wrap(ui, ui->f_reg, fl_r(x, y, w, 100), fl_th.text, a->subtitle, 3) + 6;
	if (*a->body)
		ui_wrap(ui, a->small, fl_r(x, y, w, 100), fl_th.dim, a->body, 5);
	int by = c.y + c.h - 24 - 34;
	if (fl_button(ui, ID_OK, fl_r(c.x + c.w - 24 - 120, by, 120, 34), a->grant, FL_BTN_ACCENT))
		acc_answer(a, 0);
	if (fl_button(ui, ID_CANCEL, fl_r(c.x + c.w - 24 - 120 - 8 - 120, by, 120, 34), a->deny, FL_BTN_OUTLINE) ||
	    ui->key == XKB_KEY_Escape) {
		ui->key = 0;
		acc_answer(a, 1);
	}
}

static void acc_destroy(struct dlg *d)
{
	struct acc *a = (struct acc *)d;
	g_free(a->title);
	g_free(a->subtitle);
	g_free(a->body);
	g_free(a->grant);
	g_free(a->deny);
	if (a->choices)
		g_variant_unref(a->choices);
	if (a->small)
		fl_font_close(a->small);
	g_free(a);
}

static void acc_test(struct dlg *d, int accept)
{
	acc_answer((struct acc *)d, accept ? 0 : 1);
}

void ac_access(struct req *r, const char *title, const char *subtitle, const char *body, GVariant *options)
{
	struct acc *a = g_new0(struct acc, 1);
	a->d.destroy = acc_destroy;
	a->d.test = acc_test;
	a->small = fl_font_ui(400, 12);
	a->title = g_strdup(title);
	a->subtitle = g_strdup(subtitle);
	a->body = g_strdup(body);
	const char *grant = NULL, *deny = NULL;
	g_variant_lookup(options, "grant_label", "&s", &grant);
	g_variant_lookup(options, "deny_label", "&s", &deny);
	a->grant = g_strdup(grant && *grant ? grant : "Allow");
	a->deny = g_strdup(deny && *deny ? deny : "Deny");
	a->choices = g_variant_lookup_value(options, "choices", G_VARIANT_TYPE("a(ssa(ss)s)"));
	fl_win_opts o = {
		.title = "Permission", .app_id = "flportal", .w = 480, .h = 250, .min_w = 480, .min_h = 250,
		.decorated = 1, .resizable = 0, .draw = acc_draw,
	};
	portal_dialog_open(r, &a->d, &o);
}
