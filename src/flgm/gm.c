/* gm.c -- General Manager: window, tabs, process tables, startup apps
 * SPDX-License-Identifier: MIT
 *
 *   general-manager [--tab processes|performance|startup|users|details]
 *                   [--perf cpu|memory|disk|network|gpu] [--sort cpu|memory|disk|name]
 *                   [--expand] [--search TEXT] [--cores]
 *
 * Everything comes from /proc and /sys (sys.c, proc.c); the window redraws once a second.
 * Idle means one timer: process data is only read while a process tab is showing.
 */
#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>

#include "gmui.h"

enum {
	ID_SEARCH = 1, ID_TAB = 10, ID_END = 30, ID_COLLAPSE = 31, ID_TABLE = 40, ID_HDR = 50, ID_SB = 70,
	ID_SU = 1000, ID_PERF = 2000,
};

#define STRIP_H 46
#define STATUS_H 28
#define PAD 20

static const char *const tab_names[TAB_COUNT] = {"Processes", "Performance", "Startup apps", "Users", "Details"};

/* ---- small helpers ------------------------------------------------------------------------ */

fl_color gm_alpha(fl_color c, int a)
{
	return ((uint32_t)a << 24) | (c & 0x00ffffffu);
}

void gm_text(fl_ui *ui, fl_font *f, fl_rect r, int align, fl_color c, const char *s)
{
	fl_text_in(&ui->cv, f, r, align, 0, c, s);
}

void gm_chevron(fl_canvas *cv, int cx, int cy, int dir, float s, fl_color c)
{
	if (!cv->px)
		return;
	fl_path *p = fl_path_new();
	if (!p)
		return;
	float x = (float)cx, y = (float)cy;
	if (dir == 0) {
		fl_path_move(p, x - s * 0.5f, y - s);
		fl_path_line(p, x + s * 0.5f, y);
		fl_path_line(p, x - s * 0.5f, y + s);
	} else if (dir == 1) {
		fl_path_move(p, x - s, y - s * 0.5f);
		fl_path_line(p, x, y + s * 0.5f);
		fl_path_line(p, x + s, y - s * 0.5f);
	} else {
		fl_path_move(p, x - s, y + s * 0.5f);
		fl_path_line(p, x, y - s * 0.5f);
		fl_path_line(p, x + s, y + s * 0.5f);
	}
	fl_path_stroke(cv, p, 1.5f, c);
	fl_path_free(p);
}

void gm_say(struct gm *g, int err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g->msg, sizeof g->msg, fmt, ap);
	va_end(ap);
	g->msg_err = err;
	g->msg_until = now_s() + 4.0;
}

static int contains_ci(const char *hay, const char *needle_lc)
{
	if (!*needle_lc)
		return 1;
	size_t nl = strlen(needle_lc);
	for (; *hay; hay++) {
		size_t i = 0;
		while (i < nl && hay[i] && tolower((unsigned char)hay[i]) == (unsigned char)needle_lc[i])
			i++;
		if (i == nl)
			return 1;
	}
	return 0;
}

/* ---- expanded rows ----------------------------------------------------------------------------- */

static int exp_has(const struct gm *g, const char *key)
{
	for (int i = 0; i < g->nexp; i++)
		if (!strcmp(g->exp[i], key))
			return 1;
	return 0;
}

static void exp_toggle(struct gm *g, const char *key)
{
	for (int i = 0; i < g->nexp; i++)
		if (!strcmp(g->exp[i], key)) {
			memmove(g->exp[i], g->exp[i + 1], (size_t)(g->nexp - i - 1) * sizeof *g->exp);
			g->nexp--;
			return;
		}
	if (g->nexp == g->expcap) {
		g->expcap = g->expcap ? g->expcap * 2 : 32;
		g->exp = realloc(g->exp, (size_t)g->expcap * sizeof *g->exp);
		if (!g->exp) {
			g->nexp = g->expcap = 0;
			return;
		}
	}
	snprintf(g->exp[g->nexp++], sizeof *g->exp, "%s", key);
}

/* ---- models ------------------------------------------------------------------------------------ */

static void model_clear(struct model *m)
{
	m->n = 0;
	m->npool = 0;
}

static struct row *model_row(struct model *m)
{
	if (m->n == m->cap) {
		m->cap = m->cap ? m->cap * 2 : 256;
		m->r = realloc(m->r, (size_t)m->cap * sizeof *m->r);
		if (!m->r) {
			m->n = m->cap = 0;
			return NULL;
		}
	}
	struct row *r = &m->r[m->n++];
	memset(r, 0, sizeof *r);
	r->app = -1;
	return r;
}

static int pool_add(struct model *m, int pid)
{
	if (m->npool == m->poolcap) {
		m->poolcap = m->poolcap ? m->poolcap * 2 : 512;
		m->pool = realloc(m->pool, (size_t)m->poolcap * sizeof *m->pool);
		if (!m->pool) {
			m->npool = m->poolcap = 0;
			return -1;
		}
	}
	m->pool[m->npool] = pid;
	return m->npool++;
}

struct agg {
	char key[96], label[96];
	int app, section, a, b, disk_ok;
	double cpu, disk;
	u64 mem;
	unsigned uid;
};

static int s_col, s_desc;
static struct proc *s_p;

static int cmp_d(double a, double b) { return a < b ? -1 : a > b; }

static int cmp_agg(const void *pa, const void *pb)
{
	const struct agg *x = pa, *y = pb;
	int c;
	switch (s_col) {
	case 1: c = cmp_d(x->cpu, y->cpu); break;
	case 2: c = x->mem < y->mem ? -1 : x->mem > y->mem; break;
	case 3: c = cmp_d(x->disk, y->disk); break;
	default: c = strcasecmp(x->label, y->label); break;
	}
	if (s_desc)
		c = -c;
	return c ? c : strcasecmp(x->label, y->label);
}

static int cmp_proc_idx(const void *pa, const void *pb)
{
	const struct proc *x = &s_p[*(const int *)pa], *y = &s_p[*(const int *)pb];
	int c;
	switch (s_col) {
	case 1: c = cmp_d(x->cpu, y->cpu); break;
	case 2: c = x->priv_kb < y->priv_kb ? -1 : x->priv_kb > y->priv_kb; break;
	case 3: c = cmp_d(x->disk_bps, y->disk_bps); break;
	default: c = strcasecmp(x->name, y->name); break;
	}
	if (s_desc)
		c = -c;
	return c ? c : x->pid - y->pid;
}

/* details sorts on its own columns: 0 name, 1 pid, 2 state, 3 user, 4 cpu, 5 memory, 6 command */
static int cmp_detail(const void *pa, const void *pb)
{
	const struct proc *x = &s_p[*(const int *)pa], *y = &s_p[*(const int *)pb];
	int c;
	switch (s_col) {
	case 1: c = x->pid - y->pid; break;
	case 2: c = strcmp(state_name(x->state), state_name(y->state)); break;
	case 3: c = strcasecmp(user_name(x->uid), user_name(y->uid)); break;
	case 4: c = cmp_d(x->cpu, y->cpu); break;
	case 5: c = x->priv_kb < y->priv_kb ? -1 : x->priv_kb > y->priv_kb; break;
	case 6: c = strcasecmp(x->cmd ? x->cmd : "", y->cmd ? y->cmd : ""); break;
	default: c = strcasecmp(x->name, y->name); break;
	}
	if (s_desc)
		c = -c;
	return c ? c : x->pid - y->pid;
}

static const char *proc_label(const struct proc *p)
{
	return p->kernel ? "Kernel threads" : p->name;
}

static int is_system(const struct proc *p, unsigned me)
{
	if (p->kernel)
		return 1;
	if (p->uid == me)
		return 0;
	return p->uid == 0 || p->uid < 1000;
}

static int proc_matches(const struct proc *p, const char *q)
{
	if (!*q)
		return 1;
	char pid[16];
	snprintf(pid, sizeof pid, "%d", p->pid);
	return contains_ci(p->name, q) || contains_ci(p->comm, q) || (p->cmd && contains_ci(p->cmd, q)) ||
	       contains_ci(pid, q) || contains_ci(user_name(p->uid), q);
}

static void agg_add(struct agg *a, const struct proc *p)
{
	a->cpu += p->cpu;
	a->mem += p->priv_kb;
	a->disk += p->disk_bps;
	a->disk_ok |= p->io_ok;
}

static void fill_row_from_proc(struct model *m, struct row *r, const struct proc *p)
{
	r->cpu = p->cpu;
	r->mem = p->priv_kb;
	r->disk = p->disk_bps;
	r->disk_ok = p->io_ok;
	r->tpos = pool_add(m, p->pid);
	r->tn = 1;
}

static void build_procs(struct gm *g)
{
	struct procs *ps = &g->ps;
	struct model *m = &g->procs_m;
	model_clear(m);
	int n = ps->n;
	if (n == 0)
		return;
	unsigned me = getuid();
	char q[64];
	snprintf(q, sizeof q, "%s", g->search);
	for (char *c = q; *c; c++)
		*c = (char)tolower((unsigned char)*c);
	int searching = q[0] != 0;

	int *gi = malloc((size_t)n * sizeof *gi), *ord = malloc((size_t)n * sizeof *ord);
	struct agg *gr = calloc((size_t)n, sizeof *gr);
	if (!gi || !ord || !gr) {
		free(gi);
		free(ord);
		free(gr);
		return;
	}
	int ng = 0;
	for (int i = 0; i < n; i++) {
		struct proc *p = &ps->p[i];
		int sec = p->app >= 0 ? 0 : is_system(p, me) ? 2 : 1;
		char key[96];
		if (sec == 0)
			snprintf(key, sizeof key, "a:%d", p->app);
		else
			snprintf(key, sizeof key, "s%d:%s", sec, p->kernel ? "[kernel]" : p->name);
		int k = -1;
		for (int j = ng - 1; j >= 0; j--)
			if (!strcmp(gr[j].key, key)) {
				k = j;
				break;
			}
		if (k < 0) {
			k = ng++;
			snprintf(gr[k].key, sizeof gr[k].key, "%s", key);
			const struct appinfo *ai = sec == 0 ? apps_get(p->app) : NULL;
			snprintf(gr[k].label, sizeof gr[k].label, "%s", ai ? ai->name : proc_label(p));
			gr[k].app = sec == 0 ? p->app : -1;
			gr[k].section = sec;
		}
		gi[i] = k;
		gr[k].b++; /* count for now */
		agg_add(&gr[k], p);
	}
	int off = 0;
	for (int j = 0; j < ng; j++) {
		gr[j].a = off;
		off += gr[j].b;
		gr[j].b = gr[j].a; /* fill cursor */
	}
	for (int i = 0; i < n; i++)
		ord[gr[gi[i]].b++] = i;

	static const char *const sec_name[3] = {"Apps", "Background processes", "System processes"};
	s_p = ps->p;
	for (int sec = 0; sec < 3; sec++) {
		/* the groups of this section that match the search */
		int vis[1024], nv = 0;
		for (int j = 0; j < ng && nv < 1024; j++) {
			if (gr[j].section != sec)
				continue;
			int hit = !searching || contains_ci(gr[j].label, q);
			for (int x = gr[j].a; x < gr[j].b && !hit; x++)
				hit = proc_matches(&ps->p[ord[x]], q);
			if (hit)
				vis[nv++] = j;
		}
		if (!nv)
			continue;
		struct agg *sorted = malloc((size_t)nv * sizeof *sorted);
		if (!sorted)
			continue;
		for (int i = 0; i < nv; i++)
			sorted[i] = gr[vis[i]];
		s_col = g->sort_col[TAB_PROCS];
		s_desc = g->sort_desc[TAB_PROCS];
		qsort(sorted, (size_t)nv, sizeof *sorted, cmp_agg);

		struct row *h = model_row(m);
		if (!h)
			break;
		h->kind = ROW_SECTION;
		snprintf(h->key, sizeof h->key, "S:%d", sec);
		snprintf(h->label, sizeof h->label, "%s", sec_name[sec]);
		snprintf(h->sub, sizeof h->sub, "(%d)", nv);
		h->expandable = 1;
		h->expanded = !g->sect_closed[sec];
		h->depth = 0;
		if (g->sect_closed[sec]) {
			free(sorted);
			continue;
		}
		for (int i = 0; i < nv; i++) {
			struct agg *a = &sorted[i];
			int nmem = a->b - a->a;
			struct row *r = model_row(m);
			if (!r)
				break;
			r->kind = ROW_GROUP;
			r->app = a->app;
			snprintf(r->key, sizeof r->key, "%s", a->key);
			snprintf(r->label, sizeof r->label, "%s", a->label);
			if (nmem > 1)
				snprintf(r->sub, sizeof r->sub, "(%d)", nmem);
			r->cpu = a->cpu;
			r->mem = a->mem;
			r->disk = a->disk;
			r->disk_ok = a->disk_ok;
			r->expandable = nmem > 1;
			r->expanded = r->expandable && (searching || exp_has(g, a->key));
			r->tpos = m->npool;
			r->tn = nmem;
			for (int x = a->a; x < a->b; x++)
				pool_add(m, ps->p[ord[x]].pid);
			if (!r->expanded)
				continue;
			int *kids = malloc((size_t)nmem * sizeof *kids);
			if (!kids)
				continue;
			int nk = 0, all = !searching || contains_ci(a->label, q);
			for (int x = a->a; x < a->b; x++)
				if (all || proc_matches(&ps->p[ord[x]], q))
					kids[nk++] = ord[x];
			qsort(kids, (size_t)nk, sizeof *kids, cmp_proc_idx);
			for (int k = 0; k < nk; k++) {
				struct proc *p = &ps->p[kids[k]];
				struct row *c = model_row(m);
				if (!c)
					break;
				c->kind = ROW_CHILD;
				c->depth = 1;
				snprintf(c->key, sizeof c->key, "p:%d", p->pid);
				snprintf(c->label, sizeof c->label, "%s", p->name);
				snprintf(c->sub, sizeof c->sub, "PID %d", p->pid);
				fill_row_from_proc(m, c, p);
			}
			free(kids);
		}
		free(sorted);
	}
	free(gi);
	free(ord);
	free(gr);
}

static int cmp_uid_name(const void *pa, const void *pb)
{
	const struct proc *x = &s_p[*(const int *)pa], *y = &s_p[*(const int *)pb];
	if (x->uid != y->uid)
		return x->uid < y->uid ? -1 : 1;
	int c = strcasecmp(proc_label(x), proc_label(y));
	return c ? c : x->pid - y->pid;
}

static const char *app_or_name(const struct proc *p, const char **icon_app, int *app)
{
	const struct appinfo *ai = p->app >= 0 ? apps_get(p->app) : NULL;
	*app = ai ? p->app : -1;
	(void)icon_app;
	return ai ? ai->name : proc_label(p);
}

static void build_users(struct gm *g)
{
	struct procs *ps = &g->ps;
	struct model *m = &g->users_m;
	model_clear(m);
	int n = ps->n;
	if (!n)
		return;
	char q[64];
	snprintf(q, sizeof q, "%s", g->search);
	for (char *c = q; *c; c++)
		*c = (char)tolower((unsigned char)*c);
	int searching = q[0] != 0;
	int *ord = malloc((size_t)n * sizeof *ord);
	if (!ord)
		return;
	for (int i = 0; i < n; i++)
		ord[i] = i;
	s_p = ps->p;
	qsort(ord, (size_t)n, sizeof *ord, cmp_uid_name);

	struct agg *us = calloc((size_t)n, sizeof *us);
	if (!us) {
		free(ord);
		return;
	}
	int nu = 0;
	unsigned me = getuid();
	for (int i = 0; i < n;) {
		unsigned uid = ps->p[ord[i]].uid;
		int j = i;
		struct agg *u = &us[nu];
		u->uid = uid;
		u->a = i;
		while (j < n && ps->p[ord[j]].uid == uid) {
			agg_add(u, &ps->p[ord[j]]);
			j++;
		}
		u->b = j;
		snprintf(u->label, sizeof u->label, "%s", user_name(uid));
		snprintf(u->key, sizeof u->key, "u:%u", uid);
		int hit = !searching || contains_ci(u->label, q);
		for (int x = u->a; x < u->b && !hit; x++)
			hit = proc_matches(&ps->p[ord[x]], q);
		if (hit)
			nu++;
		else
			memset(u, 0, sizeof *u);
		i = j;
	}
	s_col = g->sort_col[TAB_USERS];
	s_desc = g->sort_desc[TAB_USERS];
	/* you first, then the rest in the chosen order */
	qsort(us, (size_t)nu, sizeof *us, cmp_agg);
	for (int i = 0; i < nu; i++) {
		struct agg *u = &us[i];
		int nmem = u->b - u->a;
		struct row *r = model_row(m);
		if (!r)
			break;
		r->kind = ROW_GROUP;
		snprintf(r->key, sizeof r->key, "%s", u->key);
		snprintf(r->label, sizeof r->label, "%s", u->label);
		snprintf(r->sub, sizeof r->sub, "%s%d process%s", u->uid == me ? "you, " : "", nmem, nmem == 1 ? "" : "es");
		r->cpu = u->cpu;
		r->mem = u->mem;
		r->disk = u->disk;
		r->disk_ok = u->disk_ok;
		r->expandable = 1;
		r->expanded = searching || exp_has(g, u->key);
		r->tpos = m->npool;
		r->tn = nmem;
		for (int x = u->a; x < u->b; x++)
			pool_add(m, ps->p[ord[x]].pid);
		if (!r->expanded)
			continue;
		/* the user's processes, grouped by app or program */
		struct agg *sub = calloc((size_t)nmem, sizeof *sub);
		if (!sub)
			continue;
		int ns = 0;
		for (int x = u->a; x < u->b; x++) {
			struct proc *p = &ps->p[ord[x]];
			int app;
			const char *lab = app_or_name(p, NULL, &app);
			if (searching && !contains_ci(u->label, q) && !proc_matches(p, q))
				continue;
			int k = -1;
			for (int y = ns - 1; y >= 0; y--)
				if (!strcmp(sub[y].label, lab)) {
					k = y;
					break;
				}
			if (k < 0) {
				k = ns++;
				snprintf(sub[k].label, sizeof sub[k].label, "%s", lab);
				snprintf(sub[k].key, sizeof sub[k].key, "k:%u:%s", u->uid, lab);
				sub[k].app = app;
				sub[k].a = m->npool; /* unused until emit */
			}
			sub[k].b++;
			agg_add(&sub[k], p);
		}
		s_col = g->sort_col[TAB_USERS];
		s_desc = g->sort_desc[TAB_USERS];
		qsort(sub, (size_t)ns, sizeof *sub, cmp_agg);
		for (int k = 0; k < ns; k++) {
			struct row *c = model_row(m);
			if (!c)
				break;
			c->kind = ROW_CHILD;
			c->depth = 1;
			c->app = sub[k].app;
			snprintf(c->key, sizeof c->key, "%s", sub[k].key);
			snprintf(c->label, sizeof c->label, "%s", sub[k].label);
			if (sub[k].b > 1)
				snprintf(c->sub, sizeof c->sub, "(%d)", sub[k].b);
			c->cpu = sub[k].cpu;
			c->mem = sub[k].mem;
			c->disk = sub[k].disk;
			c->disk_ok = sub[k].disk_ok;
			c->tpos = m->npool;
			/* its pids: same user, same label */
			for (int x = u->a; x < u->b; x++) {
				struct proc *p = &ps->p[ord[x]];
				int app;
				if (!strcmp(app_or_name(p, NULL, &app), sub[k].label)) {
					pool_add(m, p->pid);
					c->tn++;
				}
			}
		}
		free(sub);
	}
	free(us);
	free(ord);
}

static void build_details(struct gm *g)
{
	struct procs *ps = &g->ps;
	struct model *m = &g->det_m;
	model_clear(m);
	char q[64];
	snprintf(q, sizeof q, "%s", g->search);
	for (char *c = q; *c; c++)
		*c = (char)tolower((unsigned char)*c);
	int *ord = malloc((size_t)(ps->n + 1) * sizeof *ord), no = 0;
	if (!ord)
		return;
	for (int i = 0; i < ps->n; i++)
		if (proc_matches(&ps->p[i], q))
			ord[no++] = i;
	s_p = ps->p;
	s_col = g->sort_col[TAB_DETAILS];
	s_desc = g->sort_desc[TAB_DETAILS];
	qsort(ord, (size_t)no, sizeof *ord, cmp_detail);
	for (int i = 0; i < no; i++) {
		struct proc *p = &ps->p[ord[i]];
		struct row *r = model_row(m);
		if (!r)
			break;
		r->kind = ROW_DETAIL;
		r->pidx = ord[i];
		snprintf(r->key, sizeof r->key, "p:%d", p->pid);
		snprintf(r->label, sizeof r->label, "%s", p->name);
		fill_row_from_proc(m, r, p);
	}
	free(ord);
}

static void rebuild(struct gm *g)
{
	g->need_rebuild = 0;
	if (g->tab == TAB_PROCS)
		build_procs(g);
	else if (g->tab == TAB_USERS)
		build_users(g);
	else if (g->tab == TAB_DETAILS)
		build_details(g);
}

static int wants_procs(const struct gm *g)
{
	return g->tab == TAB_PROCS || g->tab == TAB_USERS || g->tab == TAB_DETAILS;
}

/* ---- ending tasks ------------------------------------------------------------------------------ */

static void end_task(struct gm *g, const struct model *m, int idx, int force)
{
	if (idx < 0 || idx >= m->n || m->r[idx].kind == ROW_SECTION)
		return;
	const struct row *r = &m->r[idx];
	int ok = 0, fail = 0, err = 0;
	for (int i = 0; i < r->tn; i++) {
		int pid = m->pool[r->tpos + i];
		if (pid <= 2)
			continue;
		if (kill(pid, force ? SIGKILL : SIGTERM) == 0)
			ok++;
		else {
			fail++;
			err = errno;
		}
	}
	if (fail && !ok)
		gm_say(g, 1, "Could not end %s: %s", r->label, strerror(err));
	else if (fail)
		gm_say(g, 1, "Ended %d of %d processes of %s (%s)", ok, ok + fail, r->label, strerror(err));
	else if (ok)
		gm_say(g, 0, "%s %s%s", force ? "Killed" : "Asked to end", r->label,
		       ok > 1 ? " (all its processes)" : "");
	else
		gm_say(g, 1, "%s cannot be ended", r->label);
	g->need_rebuild = 1;
	if (g->quick)
		fl_timer_arm(g->quick, 350, 0);
}

/* ---- the shared table machinery ---------------------------------------------------------------- */

struct tinfo {
	int idx;     /* selected row, -1 none */
	int toggle;  /* row whose expansion to flip, -1 none */
	int end;     /* Delete was pressed */
};

static int find_key(const struct row *r, int n, const char *key)
{
	if (!key[0])
		return -1;
	for (int i = 0; i < n; i++)
		if (!strcmp(r[i].key, key))
			return i;
	return -1;
}

static struct tinfo table_input(fl_ui *ui, struct gm *g, fl_rect view, int rowh, const struct model *m,
                                struct tstate *t)
{
	struct tinfo ti = {-1, -1, 0};
	const struct row *rows = m->r;
	int n = m->n;
	int res = fl_widget(ui, ID_TABLE, view, FL_FOCUSABLE | FL_NO_FOCUS_RING);
	int max = n * rowh > view.h ? n * rowh - view.h : 0;
	if ((res & FL_HOVER) && ui->wheel) {
		t->scroll += ui->wheel * 48;
		ui->wheel = 0;
	}
	int idx = find_key(rows, n, t->sel);
	if ((res & FL_HOVER) && (ui->press & FL_BTN_L)) {
		int i = (ui->my - view.y + t->scroll) / rowh;
		if (i >= 0 && i < n) {
			idx = i;
			snprintf(t->sel, sizeof t->sel, "%s", rows[i].key);
			if (rows[i].kind == ROW_SECTION || (rows[i].expandable && ui->mx < view.x + 38 + rows[i].depth * 22) ||
			    (ui->clicks >= 2 && rows[i].expandable))
				ti.toggle = i;
		}
	}
	/* while the search field has the focus, only the keys it does not use go to the table */
	int typing = ui->focus == ID_SEARCH;
	if (typing && (ui->key == XKB_KEY_Down || ui->key == XKB_KEY_Up || ui->key == XKB_KEY_Page_Down ||
	               ui->key == XKB_KEY_Page_Up))
		typing = 0;
	if (ui->key && !typing && !(ui->mods & (FL_MOD_CTRL | FL_MOD_ALT)) && n > 0) {
		int page = view.h / rowh > 1 ? view.h / rowh - 1 : 1, ni = idx, used = 1;
		switch (ui->key) {
		case XKB_KEY_Down: ni = idx < 0 ? 0 : idx + 1; break;
		case XKB_KEY_Up: ni = idx < 0 ? 0 : idx - 1; break;
		case XKB_KEY_Page_Down: ni = idx + page; break;
		case XKB_KEY_Page_Up: ni = idx - page; break;
		case XKB_KEY_Home: ni = 0; break;
		case XKB_KEY_End: ni = n - 1; break;
		case XKB_KEY_Right:
			if (idx >= 0 && rows[idx].expandable && !rows[idx].expanded)
				ti.toggle = idx;
			else if (idx >= 0 && rows[idx].expandable && idx + 1 < n)
				ni = idx + 1;
			break;
		case XKB_KEY_Left:
			if (idx >= 0 && rows[idx].expandable && rows[idx].expanded)
				ti.toggle = idx;
			else if (idx >= 0 && rows[idx].depth > 0) {
				ni = idx;
				while (ni > 0 && rows[ni].depth >= rows[idx].depth)
					ni--;
			}
			break;
		case XKB_KEY_Return:
		case XKB_KEY_KP_Enter:
			if (idx >= 0 && rows[idx].expandable)
				ti.toggle = idx;
			break;
		case XKB_KEY_Delete:
		case XKB_KEY_KP_Delete:
			ti.end = 1;
			break;
		default: used = 0; break;
		}
		if (used) {
			ui->key = 0;
			if (ui->focus == ID_SEARCH)
				ui->focus = ID_TABLE;
			if (ni < 0)
				ni = 0;
			if (ni >= n)
				ni = n - 1;
			if (ni != idx) {
				idx = ni;
				snprintf(t->sel, sizeof t->sel, "%s", rows[ni].key);
			}
			if (idx >= 0) {
				if (idx * rowh < t->scroll)
					t->scroll = idx * rowh;
				if ((idx + 1) * rowh > t->scroll + view.h)
					t->scroll = (idx + 1) * rowh - view.h;
			}
		}
	}
	if (t->scroll > max)
		t->scroll = max;
	if (t->scroll < 0)
		t->scroll = 0;
	ti.idx = idx;
	return ti;
}

static void apply_toggle(struct gm *g, const struct model *m, int i)
{
	const struct row *r = &m->r[i];
	if (r->kind == ROW_SECTION) {
		int sec = atoi(r->key + 2);
		if (sec >= 0 && sec < 3)
			g->sect_closed[sec] = !g->sect_closed[sec];
	} else {
		exp_toggle(g, r->key);
	}
	g->need_rebuild = 1;
}

/* heat: the accent over the cell, four steps */
static void heat_cell(fl_ui *ui, fl_rect r, double v, const double th[4])
{
	static const int alpha[4] = {30, 66, 112, 160};
	int lv = -1;
	for (int i = 0; i < 4; i++)
		if (v >= th[i])
			lv = i;
	if (lv >= 0)
		fl_fill_rect(&ui->cv, r, gm_alpha(fl_th.accent, alpha[lv]));
}

static const double TH_CPU[4] = {0.3, 3, 10, 30};
static const double TH_DISK[4] = {50e3, 1e6, 10e6, 50e6};

static void sort_click(struct gm *g, int col, int numeric)
{
	int t = g->tab;
	if (g->sort_col[t] == col)
		g->sort_desc[t] = !g->sort_desc[t];
	else {
		g->sort_col[t] = col;
		g->sort_desc[t] = numeric;
	}
	g->need_rebuild = 1;
}

/* A header cell: returns 1 when clicked. top is the big line (may be NULL), label the small one. */
static int header_cell(fl_ui *ui, struct gm *g, uint32_t id, fl_rect r, const char *top, const char *label,
                       int sorted, int desc, int align_left)
{
	int res = fl_widget(ui, id, r, 0);
	if (res & (FL_HOVER | FL_DOWN))
		fl_fill_rect(&ui->cv, r, fl_th.raised);
	fl_color dim = fl_th.dim;
	if (align_left) {
		fl_rect lr = fl_r(r.x + PAD, r.y, r.w - PAD, r.h);
		gm_text(ui, ui->f_med, lr, FL_LEFT, sorted ? fl_th.strong : dim, label);
		if (sorted)
			gm_chevron(&ui->cv, r.x + PAD + fl_text_width(ui->f_med, label, -1) + 12, r.y + r.h / 2, desc ? 1 : 2,
			           3, fl_th.accent);
	} else if (top) {
		fl_rect tr = fl_r(r.x, r.y + 8, r.w - 14, 20);
		gm_text(ui, g->f.h2, tr, FL_RIGHT, fl_th.strong, top);
		fl_rect lr = fl_r(r.x, r.y + 28, r.w - 14, 18);
		gm_text(ui, g->f.sm, lr, FL_RIGHT, sorted ? fl_th.strong : dim, label);
		if (sorted)
			gm_chevron(&ui->cv, r.x + r.w - 14 - fl_text_width(g->f.sm, label, -1) - 12, r.y + 37, desc ? 1 : 2, 3,
			           fl_th.accent);
	} else {
		fl_rect lr = fl_r(r.x, r.y, r.w - 14, r.h);
		gm_text(ui, ui->f_med, lr, FL_RIGHT, sorted ? fl_th.strong : dim, label);
		if (sorted)
			gm_chevron(&ui->cv, r.x + r.w - 14 - fl_text_width(ui->f_med, label, -1) - 12, r.y + r.h / 2,
			           desc ? 1 : 2, 3, fl_th.accent);
	}
	return res & FL_CLICK;
}

/* "End task" outlined in the error colour; dim and dead when nothing is selected. */
static int end_button(fl_ui *ui, fl_rect r, int enabled)
{
	int res = fl_widget(ui, ID_END, r, enabled ? FL_FOCUSABLE : 0);
	fl_color c = enabled ? fl_th.err : fl_th.dim;
	if (enabled && (res & (FL_HOVER | FL_DOWN)))
		fl_fill_rect(&ui->cv, r, gm_alpha(fl_th.err, (res & FL_DOWN) ? 70 : 38));
	fl_stroke_rect(&ui->cv, r, 1, enabled ? c : fl_th.line);
	if (res & FL_FOCUS)
		fl_stroke_rect(&ui->cv, fl_inset(r, -2, -2), 1, fl_th.accent);
	gm_text(ui, ui->f_med, r, FL_CENTER, c, "End task");
	int act = enabled && (res & FL_CLICK);
	if (enabled && (res & FL_FOCUS) && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_space)) {
		ui->key = 0;
		act = 1;
	}
	return act;
}

static void draw_scroll(fl_ui *ui, fl_rect view, struct tstate *t, int n, int rowh)
{
	int content = n * rowh;
	if (content > view.h) {
		fl_rect sb = fl_r(view.x + view.w - 8, view.y + 2, 6, view.h - 4);
		fl_scrollbar(ui, ID_SB, sb, &t->scroll, content, view.h);
	}
}

/* ---- Processes and Users --------------------------------------------------------------------- */

static fl_icon row_icon(const struct row *r, int *has)
{
	*has = 0;
	if (r->app < 0)
		return FL_ICON_WINDOW;
	const struct appinfo *ai = apps_get(r->app);
	*has = 1;
	return ai ? fl_icon_for_app(ai->exec) : FL_ICON_WINDOW;
}

static void draw_groups_page(fl_ui *ui, struct gm *g, fl_rect r, int tab, const char *title)
{
	struct model *m = tab == TAB_PROCS ? &g->procs_m : &g->users_m;
	struct tstate *t = &g->ts[tab];
	fl_canvas *cv = &ui->cv;

	fl_rect tb = fl_r(r.x, r.y, r.w, 56);
	fl_rect hdr = fl_r(r.x, r.y + tb.h, r.w, 56);
	fl_rect view = fl_r(r.x, hdr.y + hdr.h, r.w, r.h - tb.h - hdr.h);
	const int ROWH = 32;

	/* input first, so the toolbar below sees this pass's selection */
	struct tinfo ti = table_input(ui, g, view, ROWH, m, t);
	if (ti.toggle >= 0)
		apply_toggle(g, m, ti.toggle);
	if (ti.end)
		end_task(g, m, ti.idx, (ui->mods & FL_MOD_SHIFT) != 0);
	if (g->need_rebuild) {
		rebuild(g);
		m = tab == TAB_PROCS ? &g->procs_m : &g->users_m;
		ti.idx = find_key(m->r, m->n, t->sel);
		int max = m->n * ROWH > view.h ? m->n * ROWH - view.h : 0;
		if (t->scroll > max)
			t->scroll = max;
	}

	gm_text(ui, g->f.h1, fl_r(r.x + PAD, tb.y, 300, tb.h), FL_LEFT, fl_th.strong, title);
	fl_rect bx = fl_r(r.x + r.w - PAD - 100, tb.y + 13, 100, 30);
	int can_end = ti.idx >= 0 && ti.idx < m->n && m->r[ti.idx].kind != ROW_SECTION;
	if (end_button(ui, bx, can_end))
		end_task(g, m, ti.idx, (ui->mods & FL_MOD_SHIFT) != 0);
	if (fl_button(ui, ID_COLLAPSE, fl_r(bx.x - 112, bx.y, 104, 30), "Collapse all", FL_BTN_PLAIN)) {
		g->nexp = 0;
		g->need_rebuild = 1;
	}
	fl_fill_rect(cv, hdr, fl_th.panel);
	fl_fill_rect(cv, fl_r(hdr.x, hdr.y + hdr.h - 1, hdr.w, 1), fl_th.line);

	/* columns: Name, CPU, Memory, Disk, Network */
	int cw[4] = {96, 112, 104, 104};
	int numw = cw[0] + cw[1] + cw[2] + cw[3];
	int namew = r.w - numw;
	if (namew < 180) {
		cw[0] = cw[1] = cw[2] = cw[3] = (r.w - 180) / 4;
		numw = cw[0] * 4;
		namew = 180;
	}
	int cx[5] = {r.x, r.x + namew, r.x + namew + cw[0], r.x + namew + cw[0] + cw[1], r.x + namew + cw[0] + cw[1] + cw[2]};
	double tot_cpu = g->sys.cpu_total;
	double mem_pct = g->sys.mem_total ? 100.0 * (double)(g->sys.mem_total - g->sys.mem_avail) / (double)g->sys.mem_total : 0;
	char t_cpu[24], t_mem[24], t_disk[24], t_net[24];
	snprintf(t_cpu, sizeof t_cpu, "%s", fmt_pct(tot_cpu));
	snprintf(t_mem, sizeof t_mem, "%s", fmt_pct(mem_pct));
	snprintf(t_disk, sizeof t_disk, "%s", fmt_pct(g->sys.disk_active));
	snprintf(t_net, sizeof t_net, "%s", fmt_bits(g->sys.net_bps));
	/* header heat, then the cells on top */
	heat_cell(ui, fl_r(cx[1], hdr.y, cw[0], hdr.h - 1), tot_cpu, TH_CPU);
	{
		static const double th[4] = {10, 30, 60, 85};
		heat_cell(ui, fl_r(cx[2], hdr.y, cw[1], hdr.h - 1), mem_pct, th);
		static const double thd[4] = {1, 10, 40, 80};
		heat_cell(ui, fl_r(cx[3], hdr.y, cw[2], hdr.h - 1), g->sys.disk_active, thd);
	}
	int sc = g->sort_col[tab], sd = g->sort_desc[tab];
	if (header_cell(ui, g, ID_HDR + 0, fl_r(cx[0], hdr.y, namew, hdr.h - 1), NULL, tab == TAB_USERS ? "User" : "Name",
	                sc == 0, sd, 1))
		sort_click(g, 0, 0);
	if (header_cell(ui, g, ID_HDR + 1, fl_r(cx[1], hdr.y, cw[0], hdr.h - 1), t_cpu, "CPU", sc == 1, sd, 0))
		sort_click(g, 1, 1);
	if (header_cell(ui, g, ID_HDR + 2, fl_r(cx[2], hdr.y, cw[1], hdr.h - 1), t_mem, "Memory", sc == 2, sd, 0))
		sort_click(g, 2, 1);
	if (header_cell(ui, g, ID_HDR + 3, fl_r(cx[3], hdr.y, cw[2], hdr.h - 1), t_disk, "Disk", sc == 3, sd, 0))
		sort_click(g, 3, 1);
	header_cell(ui, g, ID_HDR + 4, fl_r(cx[4], hdr.y, cw[3], hdr.h - 1), t_net, "Network", 0, 0, 0);

	/* rows */
	fl_rect save = cv->clip;
	fl_clip(cv, view);
	int first = t->scroll / ROWH, last = (t->scroll + view.h) / ROWH;
	int hover_i = -1;
	if (fl_in(view, ui->mx, ui->my) && ui->mouse_in && (!ui->active || ui->active == ID_TABLE))
		hover_i = (ui->my - view.y + t->scroll) / ROWH;
	int sb = m->n * ROWH > view.h;
	for (int i = first; i <= last && i < m->n; i++) {
		const struct row *row = &m->r[i];
		fl_rect rr = fl_r(view.x, view.y + i * ROWH - t->scroll, view.w, ROWH);
		int sel = i == ti.idx;
		if (row->kind == ROW_SECTION) {
			fl_fill_rect(cv, rr, sel ? fl_th.select : fl_th.panel);
			fl_fill_rect(cv, fl_r(rr.x, rr.y + rr.h - 1, rr.w, 1), gm_alpha(fl_th.line, 140));
			gm_chevron(cv, rr.x + PAD + 4, rr.y + rr.h / 2, row->expanded ? 1 : 0, 3.5f, fl_th.dim);
			int tw = fl_text_width(ui->f_semi, row->label, -1);
			gm_text(ui, ui->f_semi, fl_r(rr.x + PAD + 22, rr.y, 400, rr.h), FL_LEFT, fl_th.text, row->label);
			gm_text(ui, g->f.sm, fl_r(rr.x + PAD + 22 + tw + 8, rr.y, 100, rr.h), FL_LEFT, fl_th.dim, row->sub);
			if (sel)
				fl_fill_rect(cv, fl_r(rr.x, rr.y, 3, rr.h), fl_th.accent);
			continue;
		}
		if (sel)
			fl_fill_rect(cv, rr, fl_th.select);
		else if (i == hover_i)
			fl_fill_rect(cv, rr, gm_alpha(fl_th.raised, 150));
		/* heat */
		double memfrac = g->sys.mem_total ? (double)row->mem / (double)g->sys.mem_total : 0;
		static const double th_mem[4] = {0.004, 0.02, 0.06, 0.15};
		heat_cell(ui, fl_r(cx[1], rr.y, cw[0], rr.h), row->cpu, TH_CPU);
		heat_cell(ui, fl_r(cx[2], rr.y, cw[1], rr.h), memfrac, th_mem);
		if (row->disk_ok)
			heat_cell(ui, fl_r(cx[3], rr.y, cw[2], rr.h), row->disk, TH_DISK);
		/* name */
		int x = rr.x + PAD + row->depth * 34;
		if (row->expandable)
			gm_chevron(cv, x + 4, rr.y + rr.h / 2, row->expanded ? 1 : 0, 3.5f, fl_th.dim);
		x += 22;
		int has;
		fl_icon ic = row_icon(row, &has);
		if (has && row->kind == ROW_GROUP && tab == TAB_PROCS) {
			fl_icon_draw(cv, ic, x, rr.y + (rr.h - 16) / 2, 16, fl_th.accent);
			x += 26;
		}
		int avail = cx[1] - x - 12;
		char fit[128];
		fl_text_fit(ui->f_reg, row->label, avail > 20 ? avail : 20, fit, sizeof fit);
		gm_text(ui, ui->f_reg, fl_r(x, rr.y, avail, rr.h), FL_LEFT, fl_th.text, fit);
		if (row->sub[0]) {
			int tw = fl_text_width(ui->f_reg, fit, -1) + 8;
			if (tw < avail - 20)
				gm_text(ui, g->f.sm, fl_r(x + tw, rr.y, avail - tw, rr.h), FL_LEFT, fl_th.dim, row->sub);
		}
		/* numbers */
		int rpad = sb ? 14 : 14;
		(void)rpad;
		fl_color zero = fl_th.dim;
		fl_rect c0 = fl_r(cx[1], rr.y, cw[0] - 14, rr.h), c1 = fl_r(cx[2], rr.y, cw[1] - 14, rr.h),
		        c2 = fl_r(cx[3], rr.y, cw[2] - 14, rr.h), c3 = fl_r(cx[4], rr.y, cw[3] - 14, rr.h);
		gm_text(ui, ui->f_reg, c0, FL_RIGHT, row->cpu < 0.05 ? zero : fl_th.text, fmt_pct(row->cpu));
		gm_text(ui, ui->f_reg, c1, FL_RIGHT, row->mem == 0 ? zero : fl_th.text, fmt_mem(row->mem));
		if (row->disk_ok)
			gm_text(ui, ui->f_reg, c2, FL_RIGHT, row->disk < 1 ? zero : fl_th.text, fmt_rate(row->disk));
		else
			gm_text(ui, ui->f_reg, c2, FL_RIGHT, zero, "\xe2\x80\x94");
		gm_text(ui, ui->f_reg, c3, FL_RIGHT, zero, "\xe2\x80\x94");
		if (sel)
			fl_fill_rect(cv, fl_r(rr.x, rr.y, 3, rr.h), fl_th.accent);
	}
	cv->clip = save;
	draw_scroll(ui, view, t, m->n, ROWH);
	if (m->n == 0)
		gm_text(ui, ui->f_reg, fl_r(view.x, view.y + 30, view.w, 30), FL_CENTER, fl_th.dim,
		        g->search[0] ? "No process matches the search" : "Reading processes...");
}

/* ---- Details ----------------------------------------------------------------------------------- */

static void draw_details(fl_ui *ui, struct gm *g, fl_rect r)
{
	struct model *m = &g->det_m;
	struct tstate *t = &g->ts[TAB_DETAILS];
	fl_canvas *cv = &ui->cv;

	fl_rect tb = fl_r(r.x, r.y, r.w, 56);
	fl_rect hdr = fl_r(r.x, r.y + tb.h, r.w, 36);
	fl_rect view = fl_r(r.x, hdr.y + hdr.h, r.w, r.h - tb.h - hdr.h);
	const int ROWH = 28;
	struct tinfo ti = table_input(ui, g, view, ROWH, m, t);
	if (ti.end)
		end_task(g, m, ti.idx, (ui->mods & FL_MOD_SHIFT) != 0);
	if (g->need_rebuild) {
		rebuild(g);
		ti.idx = find_key(m->r, m->n, t->sel);
	}
	gm_text(ui, g->f.h1, fl_r(r.x + PAD, tb.y, 300, tb.h), FL_LEFT, fl_th.strong, "Details");
	if (end_button(ui, fl_r(r.x + r.w - PAD - 100, tb.y + 13, 100, 30), ti.idx >= 0))
		end_task(g, m, ti.idx, (ui->mods & FL_MOD_SHIFT) != 0);
	char cnt[48];
	snprintf(cnt, sizeof cnt, "%d processes", m->n);
	gm_text(ui, g->f.sm, fl_r(r.x + r.w - PAD - 100 - 8 - 160, tb.y, 160, tb.h), FL_RIGHT, fl_th.dim, cnt);
	fl_fill_rect(cv, hdr, fl_th.panel);
	fl_fill_rect(cv, fl_r(hdr.x, hdr.y + hdr.h - 1, hdr.w, 1), fl_th.line);

	/* Name | PID | Status | User name | CPU | Memory | Command line */
	int w_pid = 76, w_state = 128, w_user = 112, w_cpu = 76, w_mem = 104;
	int w_name = 210;
	int w_cmd = r.w - w_name - w_pid - w_state - w_user - w_cpu - w_mem;
	if (w_cmd < 160) {
		w_name = r.w - 160 - w_pid - w_state - w_user - w_cpu - w_mem;
		if (w_name < 120)
			w_name = 120;
		w_cmd = 160;
	}
	int cx[7];
	cx[0] = r.x;
	cx[1] = cx[0] + w_name;
	cx[2] = cx[1] + w_pid;
	cx[3] = cx[2] + w_state;
	cx[4] = cx[3] + w_user;
	cx[5] = cx[4] + w_cpu;
	cx[6] = cx[5] + w_mem;
	int cwid[7] = {w_name, w_pid, w_state, w_user, w_cpu, w_mem, w_cmd};
	static const char *const names[7] = {"Name", "PID", "Status", "User name", "CPU", "Memory", "Command line"};
	static const int right[7] = {0, 1, 0, 0, 1, 1, 0};
	int sc = g->sort_col[TAB_DETAILS], sd = g->sort_desc[TAB_DETAILS];
	for (int c = 0; c < 7; c++) {
		fl_rect hc = fl_r(cx[c], hdr.y, cwid[c], hdr.h - 1);
		int click;
		if (right[c])
			click = header_cell(ui, g, ID_HDR + c, hc, NULL, names[c], sc == c, sd, 0);
		else {
			click = header_cell(ui, g, ID_HDR + c, hc, NULL, names[c], sc == c, sd, 1);
		}
		if (click) {
			if (g->sort_col[TAB_DETAILS] == c)
				g->sort_desc[TAB_DETAILS] = !g->sort_desc[TAB_DETAILS];
			else {
				g->sort_col[TAB_DETAILS] = c;
				g->sort_desc[TAB_DETAILS] = c == 4 || c == 5;
			}
			g->need_rebuild = 1;
		}
		if (c > 0)
			fl_fill_rect(cv, fl_r(cx[c], hdr.y + 8, 1, hdr.h - 17), gm_alpha(fl_th.line, 160));
	}

	fl_rect save = cv->clip;
	fl_clip(cv, view);
	int first = t->scroll / ROWH, last = (t->scroll + view.h) / ROWH;
	int hover_i = -1;
	if (fl_in(view, ui->mx, ui->my) && ui->mouse_in && (!ui->active || ui->active == ID_TABLE))
		hover_i = (ui->my - view.y + t->scroll) / ROWH;
	for (int i = first; i <= last && i < m->n; i++) {
		const struct row *row = &m->r[i];
		if (row->pidx >= g->ps.n)
			break;
		const struct proc *p = &g->ps.p[row->pidx];
		fl_rect rr = fl_r(view.x, view.y + i * ROWH - t->scroll, view.w, ROWH);
		int sel = i == ti.idx;
		if (sel)
			fl_fill_rect(cv, rr, fl_th.select);
		else if (i == hover_i)
			fl_fill_rect(cv, rr, gm_alpha(fl_th.raised, 150));
		else if (i & 1)
			fl_fill_rect(cv, rr, gm_alpha(fl_th.panel, 90));
		char b[96], fit[160];
		fl_text_fit(ui->f_reg, p->name, cwid[0] - PAD - 8, fit, sizeof fit);
		gm_text(ui, ui->f_reg, fl_r(cx[0] + PAD, rr.y, cwid[0] - PAD, rr.h), FL_LEFT, fl_th.text, fit);
		snprintf(b, sizeof b, "%d", p->pid);
		gm_text(ui, ui->f_reg, fl_r(cx[1], rr.y, cwid[1] - 14, rr.h), FL_RIGHT, fl_th.dim, b);
		gm_text(ui, ui->f_reg, fl_r(cx[2] + PAD, rr.y, cwid[2] - PAD, rr.h), FL_LEFT, p->state == 'R' ? fl_th.accent : fl_th.dim,
		        state_name(p->state));
		fl_text_fit(ui->f_reg, user_name(p->uid), cwid[3] - PAD - 6, fit, sizeof fit);
		gm_text(ui, ui->f_reg, fl_r(cx[3] + PAD, rr.y, cwid[3] - PAD, rr.h), FL_LEFT, fl_th.text, fit);
		gm_text(ui, ui->f_reg, fl_r(cx[4], rr.y, cwid[4] - 14, rr.h), FL_RIGHT, p->cpu < 0.05 ? fl_th.dim : fl_th.text,
		        fmt_pct(p->cpu));
		gm_text(ui, ui->f_reg, fl_r(cx[5], rr.y, cwid[5] - 14, rr.h), FL_RIGHT, fl_th.text, fmt_mem(p->priv_kb));
		fl_text_fit(ui->f_reg, p->cmd ? p->cmd : "", cwid[6] - PAD - 28, fit, sizeof fit);
		gm_text(ui, ui->f_reg, fl_r(cx[6] + PAD, rr.y, cwid[6] - PAD, rr.h), FL_LEFT, fl_th.dim, fit);
		if (sel)
			fl_fill_rect(cv, fl_r(rr.x, rr.y, 3, rr.h), fl_th.accent);
	}
	cv->clip = save;
	draw_scroll(ui, view, t, m->n, ROWH);
}

/* ---- Startup apps ------------------------------------------------------------------------------ */

static void startup_reload(struct gm *g)
{
	free(g->su);
	g->su = NULL;
	g->nsu = startup_load(&g->su);
}

static void draw_startup(fl_ui *ui, struct gm *g, fl_rect r)
{
	fl_canvas *cv = &ui->cv;
	gm_text(ui, g->f.h1, fl_r(r.x + PAD, r.y, 300, 56), FL_LEFT, fl_th.strong, "Startup apps");
	char dir[300];
	snprintf(dir, sizeof dir, "%s", startup_user_dir());
	{
		const char *h = getenv("HOME");
		if (h && !strncmp(dir, h, strlen(h)) && dir[0] == '/') {
			char t[300];
			snprintf(t, sizeof t, "~%s", dir + strlen(h));
			snprintf(dir, sizeof dir, "%s", t);
		}
	}
	{
		char fit[320];
		fl_text_fit(g->f.sm, dir[0] ? dir : "no home folder", r.w - 2 * PAD - 220, fit, sizeof fit);
		gm_text(ui, g->f.sm, fl_r(r.x + PAD + 180, r.y, r.w - 2 * PAD - 180, 56), FL_RIGHT, fl_th.dim, fit);
	}

	fl_rect hdr = fl_r(r.x, r.y + 56, r.w, 36);
	fl_fill_rect(cv, hdr, fl_th.panel);
	fl_fill_rect(cv, fl_r(hdr.x, hdr.y + hdr.h - 1, hdr.w, 1), fl_th.line);
	int w_stat = 170, w_src = 100;
	int w_cmd = r.w * 30 / 100;
	if (w_cmd < 160)
		w_cmd = 160;
	int w_name = r.w - w_cmd - w_src - w_stat;
	if (w_name < 200)
		w_name = 200;
	int x_cmd = r.x + w_name, x_src = x_cmd + w_cmd, x_stat = x_src + w_src;
	gm_text(ui, ui->f_med, fl_r(r.x + PAD, hdr.y, w_name, hdr.h - 1), FL_LEFT, fl_th.dim, "Name");
	gm_text(ui, ui->f_med, fl_r(x_cmd, hdr.y, w_cmd, hdr.h - 1), FL_LEFT, fl_th.dim, "Command");
	gm_text(ui, ui->f_med, fl_r(x_src, hdr.y, w_src, hdr.h - 1), FL_LEFT, fl_th.dim, "Source");
	gm_text(ui, ui->f_med, fl_r(x_stat, hdr.y, w_stat, hdr.h - 1), FL_LEFT, fl_th.dim, "Status");

	fl_rect view = fl_r(r.x, hdr.y + hdr.h, r.w, r.h - 56 - hdr.h);
	char q[64];
	snprintf(q, sizeof q, "%s", g->search);
	for (char *c = q; *c; c++)
		*c = (char)tolower((unsigned char)*c);
	int idx[512], nv = 0;
	for (int i = 0; i < g->nsu && nv < 512; i++)
		if (!q[0] || contains_ci(g->su[i].name, q) || contains_ci(g->su[i].exec, q) || contains_ci(g->su[i].comment, q))
			idx[nv++] = i;
	const int ROWH = 58;
	int max = nv * ROWH > view.h ? nv * ROWH - view.h : 0;
	if (fl_in(view, ui->mx, ui->my) && ui->wheel) {
		g->su_scroll += ui->wheel * 48;
		ui->wheel = 0;
	}
	if (g->su_scroll > max)
		g->su_scroll = max;
	if (g->su_scroll < 0)
		g->su_scroll = 0;
	fl_rect save = cv->clip;
	fl_clip(cv, view);
	int toggled = -1, to = 0;
	for (int k = 0; k < nv; k++) {
		struct startup *s = &g->su[idx[k]];
		fl_rect rr = fl_r(view.x, view.y + k * ROWH - g->su_scroll, view.w, ROWH);
		if (rr.y + rr.h < view.y || rr.y > view.y + view.h)
			continue;
		int hov = ui->mouse_in && fl_in(fl_intersect(rr, view), ui->mx, ui->my) && !ui->active;
		if (hov)
			fl_fill_rect(cv, rr, gm_alpha(fl_th.raised, 150));
		fl_fill_rect(cv, fl_r(rr.x, rr.y + rr.h - 1, rr.w, 1), gm_alpha(fl_th.line, 110));
		fl_color nc = s->enabled ? fl_th.text : fl_th.dim;
		char fit[200];
		fl_text_fit(ui->f_med, s->name, w_name - PAD - 12, fit, sizeof fit);
		gm_text(ui, ui->f_med, fl_r(r.x + PAD, rr.y + 9, w_name - PAD, 20), FL_LEFT, nc, fit);
		fl_text_fit(g->f.sm, s->comment[0] ? s->comment : s->file, w_name - PAD - 12, fit, sizeof fit);
		gm_text(ui, g->f.sm, fl_r(r.x + PAD, rr.y + 30, w_name - PAD, 18), FL_LEFT, fl_th.dim, fit);
		fl_text_fit(g->f.mono, s->exec, w_cmd - 16, fit, sizeof fit);
		gm_text(ui, g->f.mono, fl_r(x_cmd, rr.y, w_cmd, rr.h), FL_LEFT, fl_th.dim, fit);
		gm_text(ui, ui->f_reg, fl_r(x_src, rr.y, w_src, rr.h), FL_LEFT, fl_th.dim, s->user ? "User" : "System");
		int on = s->enabled;
		fl_rect sw = fl_r(x_stat, rr.y + (rr.h - 24) / 2, 40, 24);
		if (fl_switch(ui, ID_SU + idx[k], sw, &on)) {
			toggled = idx[k];
			to = on;
		}
		gm_text(ui, ui->f_reg, fl_r(x_stat + 54, rr.y, w_stat - 54, rr.h), FL_LEFT, s->enabled ? fl_th.accent : fl_th.dim,
		        s->enabled ? "Enabled" : "Disabled");
	}
	cv->clip = save;
	if (nv * ROWH > view.h) {
		fl_rect sb = fl_r(view.x + view.w - 8, view.y + 2, 6, view.h - 4);
		fl_scrollbar(ui, ID_SB, sb, &g->su_scroll, nv * ROWH, view.h);
	}
	if (nv == 0) {
		fl_rect c = fl_r(view.x, view.y + 40, view.w, 24);
		gm_text(ui, g->f.h2, c, FL_CENTER, fl_th.text, g->nsu ? "No startup app matches the search" : "No startup apps");
		if (!g->nsu)
			gm_text(ui, ui->f_reg, fl_r(view.x, view.y + 70, view.w, 22), FL_CENTER, fl_th.dim,
			        "Put a .desktop file into the folder above and it starts at login.");
	}
	if (toggled >= 0) {
		struct startup s = g->su[toggled];
		if (startup_set(&s, to) < 0)
			gm_say(g, 1, "Could not change %s: %s", s.name, strerror(errno));
		else
			gm_say(g, 0, "%s will %s at login", s.name, to ? "start" : "no longer start");
		startup_reload(g);
	}
}

/* ---- chrome ------------------------------------------------------------------------------------ */

static void set_tab(struct gm *g, int tab)
{
	if (tab == g->tab)
		return;
	g->tab = tab;
	if (tab == TAB_STARTUP)
		startup_reload(g);
	if (wants_procs(g)) {
		procs_scan(&g->ps);
		rebuild(g);
		fl_timer_arm(g->quick, 400, 0);
	}
	fl_win_redraw(g->win);
}

static void draw_status(fl_ui *ui, struct gm *g, fl_rect r)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, r, fl_th.panel);
	fl_fill_rect(cv, fl_r(r.x, r.y, r.w, 1), fl_th.line);
	const struct sys *s = &g->sys;
	double mem_pct = s->mem_total ? 100.0 * (double)(s->mem_total - s->mem_avail) / (double)s->mem_total : 0;
	char b[160];
	int x = r.x + PAD;
	struct { const char *k; char v[40]; } it[4];
	it[0].k = "Processes";
	snprintf(it[0].v, sizeof it[0].v, "%d", s->nproc);
	it[1].k = "CPU";
	snprintf(it[1].v, sizeof it[1].v, "%s", fmt_pct(s->cpu_total));
	it[2].k = "Memory";
	snprintf(it[2].v, sizeof it[2].v, "%s", fmt_pct(mem_pct));
	it[3].k = "Up";
	snprintf(it[3].v, sizeof it[3].v, "%s", fmt_uptime(s->uptime));
	for (int i = 0; i < 4; i++) {
		int kw = fl_text_width(g->f.sm, it[i].k, -1), vw = fl_text_width(g->f.sm, it[i].v, -1);
		gm_text(ui, g->f.sm, fl_r(x, r.y, kw + 4, r.h), FL_LEFT, fl_th.dim, it[i].k);
		gm_text(ui, g->f.sm, fl_r(x + kw + 6, r.y, vw + 4, r.h), FL_LEFT, fl_th.text, it[i].v);
		x += kw + vw + 28;
	}
	if (g->msg[0] && now_s() < g->msg_until) {
		int w = fl_text_width(g->f.sm, g->msg, -1);
		gm_text(ui, g->f.sm, fl_r(r.x + r.w - PAD - w - 4, r.y, w + 8, r.h), FL_RIGHT, g->msg_err ? fl_th.err : fl_th.accent, g->msg);
	} else if (g->tab == TAB_PROCS || g->tab == TAB_DETAILS || g->tab == TAB_USERS) {
		snprintf(b, sizeof b, "Delete ends the selected task, Shift+Delete forces it");
		int w = fl_text_width(g->f.sm, b, -1);
		gm_text(ui, g->f.sm, fl_r(r.x + r.w - PAD - w - 4, r.y, w + 8, r.h), FL_RIGHT, fl_th.dim, b);
	}
}

static void draw(fl_ui *ui, void *ud)
{
	struct gm *g = ud;
	fl_canvas *cv = &ui->cv;
	fl_rect c = fl_win_content(g->win);
	fl_fill_rect(cv, c, fl_th.bg);

	/* window-wide keys */
	if (ui->key && (ui->mods & FL_MOD_CTRL)) {
		uint32_t k = ui->key;
		if ((k == 'f' || k == 'F') && g->tab != TAB_PERF) {
			ui->focus = ID_SEARCH;
			ui->key = 0;
		} else if (k >= '1' && k <= '5') {
			set_tab(g, (int)(k - '1'));
			ui->key = 0;
		} else if (k == XKB_KEY_Page_Down || (k == XKB_KEY_Tab && !(ui->mods & FL_MOD_SHIFT))) {
			set_tab(g, (g->tab + 1) % TAB_COUNT);
			ui->key = 0;
		} else if (k == XKB_KEY_Page_Up || k == XKB_KEY_ISO_Left_Tab) {
			set_tab(g, (g->tab + TAB_COUNT - 1) % TAB_COUNT);
			ui->key = 0;
		}
	}
	if (ui->key == XKB_KEY_Escape && ui->focus == ID_SEARCH) {
		g->search[0] = 0;
		g->need_rebuild = 1;
		ui->focus = 0;
		ui->key = 0;
	}

	/* tab strip */
	fl_rect strip = fl_r(c.x, c.y, c.w, STRIP_H);
	fl_fill_rect(cv, strip, fl_th.panel);
	fl_fill_rect(cv, fl_r(strip.x, strip.y + strip.h - 1, strip.w, 1), fl_th.line);
	int x = c.x + 8;
	for (int i = 0; i < TAB_COUNT; i++) {
		int w = fl_text_width(ui->f_med, tab_names[i], -1) + 36;
		fl_rect tr = fl_r(x, strip.y, w, strip.h - 1);
		int res = fl_widget(ui, ID_TAB + i, tr, 0);
		if (res & FL_CLICK)
			set_tab(g, i);
		int on = i == g->tab;
		if (!on && (res & (FL_HOVER | FL_DOWN)))
			fl_fill_rect(cv, fl_r(tr.x + 2, tr.y + 6, tr.w - 4, tr.h - 6), fl_th.raised);
		gm_text(ui, ui->f_med, tr, FL_CENTER, on ? fl_th.strong : fl_th.dim, tab_names[i]);
		if (on)
			fl_fill_rect(cv, fl_r(tr.x + 10, strip.y + strip.h - 3, tr.w - 20, 3), fl_th.accent);
		x += w;
	}
	if (g->tab != TAB_PERF) {
		int sw = c.w > 900 ? 280 : 200;
		fl_rect sr = fl_r(c.x + c.w - sw - 14, strip.y + (strip.h - 30) / 2, sw, 30);
		const char *ph = g->tab == TAB_STARTUP ? "Search startup apps" : g->tab == TAB_USERS ? "Search users" : "Search";
		if (fl_textfield(ui, ID_SEARCH, sr, g->search, sizeof g->search, ph, ui->f_reg) & FL_EDIT_CHANGED) {
			g->need_rebuild = 1;
			g->ts[g->tab].scroll = 0;
			g->su_scroll = 0;
		}
	}

	fl_rect page = fl_r(c.x, c.y + STRIP_H, c.w, c.h - STRIP_H - STATUS_H);
	fl_rect save = cv->clip;
	fl_clip(cv, page);
	switch (g->tab) {
	case TAB_PROCS: draw_groups_page(ui, g, page, TAB_PROCS, "Processes"); break;
	case TAB_USERS: draw_groups_page(ui, g, page, TAB_USERS, "Users"); break;
	case TAB_DETAILS: draw_details(ui, g, page); break;
	case TAB_STARTUP: draw_startup(ui, g, page); break;
	case TAB_PERF: perf_draw(ui, g, page); break;
	}
	cv->clip = save;
	draw_status(ui, g, fl_r(c.x, c.y + c.h - STATUS_H, c.w, STATUS_H));
}

/* ---- timers and main ---------------------------------------------------------------------------- */

static void on_tick(void *ud)
{
	struct gm *g = ud;
	sys_sample(&g->sys);
	if (wants_procs(g)) {
		procs_scan(&g->ps);
		rebuild(g);
	}
	fl_win_redraw(g->win);
}

static void on_quick(void *ud)
{
	struct gm *g = ud;
	if (wants_procs(g)) {
		procs_scan(&g->ps);
		rebuild(g);
	}
	fl_win_redraw(g->win);
}

static int tab_from(const char *s)
{
	static const char *const n[TAB_COUNT] = {"processes", "performance", "startup", "users", "details"};
	for (int i = 0; i < TAB_COUNT; i++)
		if (!strncasecmp(s, n[i], strlen(s)) && *s)
			return i;
	return -1;
}

static void usage(void)
{
	fprintf(stderr,
	        "usage: general-manager [--tab processes|performance|startup|users|details]\n"
	        "                       [--perf cpu|memory|disk|network|gpu] [--sort cpu|memory|disk|name]\n"
	        "                       [--expand APP|all] [--select TEXT] [--search TEXT] [--cores] [--size WxH]\n"
	        "Ctrl+Shift+Esc opens it from the desktop.\n");
}

int main(int argc, char **argv)
{
	struct gm *g = calloc(1, sizeof *g);
	if (!g)
		return 1;
	int tab = TAB_PROCS, perf = 0;
	const char *sortby = NULL, *expand = NULL, *select_text = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--tab") && i + 1 < argc) {
			tab = tab_from(argv[++i]);
			if (tab < 0) {
				usage();
				return 2;
			}
		} else if (!strcmp(argv[i], "--perf") && i + 1 < argc) {
			const char *p = argv[++i];
			perf = !strcmp(p, "cpu") ? 0 : !strcmp(p, "memory") ? 1 : !strcmp(p, "disk") ? 2 : !strcmp(p, "network") ? 3 : 4;
		} else if (!strcmp(argv[i], "--sort") && i + 1 < argc) {
			sortby = argv[++i];
		} else if (!strcmp(argv[i], "--search") && i + 1 < argc) {
			snprintf(g->search, sizeof g->search, "%s", argv[++i]);
		} else if (!strcmp(argv[i], "--expand") && i + 1 < argc) {
			expand = argv[++i];
		} else if (!strcmp(argv[i], "--select") && i + 1 < argc) {
			select_text = argv[++i];
		} else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
			sscanf(argv[++i], "%dx%d", &g->width, &g->height);
		} else if (!strcmp(argv[i], "--cores")) {
			g->cores_view = 1;
		} else {
			usage();
			return !strcmp(argv[i], "-h") || !strcmp(argv[i], "--help") ? 0 : 2;
		}
	}
	g->interval_ms = 1000;
	const char *iv = getenv("GM_INTERVAL_MS"); /* tests and screenshots */
	if (iv && atoi(iv) >= 50)
		g->interval_ms = atoi(iv);

	g->app = fl_app_new(NULL, NULL);
	if (!g->app) {
		fprintf(stderr, "general-manager: cannot connect to a Wayland compositor\n");
		return 1;
	}
	g->f.sm = fl_font_ui(400, 12);
	g->f.tiny = fl_font_ui(400, 11);
	g->f.h1 = fl_font_ui(600, 20);
	g->f.title = fl_font_ui(600, 24);
	g->f.h2 = fl_font_ui(600, 15);
	g->f.big = fl_font_ui(600, 28);
	g->f.mid = fl_font_ui(500, 18);
	g->f.mono = fl_font_mono(400, 12);

	apps_load();
	sys_init(&g->sys);
	procs_init(&g->ps, g->sys.ncpu);
	sys_sample(&g->sys);
	g->sort_col[TAB_PROCS] = g->sort_col[TAB_USERS] = 0;
	g->sort_col[TAB_DETAILS] = 4;
	g->sort_desc[TAB_DETAILS] = 1;
	g->sort_desc[TAB_USERS] = 0;
	if (sortby) {
		int col = !strcmp(sortby, "cpu") ? 1 : !strcmp(sortby, "memory") ? 2 : !strcmp(sortby, "disk") ? 3 : 0;
		g->sort_col[TAB_PROCS] = g->sort_col[TAB_USERS] = col;
		g->sort_desc[TAB_PROCS] = g->sort_desc[TAB_USERS] = col != 0;
		g->sort_col[TAB_DETAILS] = col == 1 ? 4 : col == 2 ? 5 : 0;
		g->sort_desc[TAB_DETAILS] = col != 0;
	}
	g->perf_sel = perf;
	g->tab = -1;
	g->tick = fl_timer_new(g->app, on_tick, g);
	g->quick = fl_timer_new(g->app, on_quick, g);

	fl_win_opts o = {
		.title = "General Manager", .app_id = "general-manager", .w = g->width > 0 ? g->width : 1040,
		.h = g->height > 0 ? g->height : 700, .min_w = 760, .min_h = 480,
		.decorated = 1, .resizable = 1, .draw = draw, .ud = g,
	};
	g->win = fl_win_new(g->app, &o);
	if (!g->win)
		return 1;
	if (expand) {
		/* groups opened up front: for screenshots */
		procs_scan(&g->ps);
		struct procs *ps = &g->ps;
		for (int i = 0; i < ps->n; i++) {
			const struct appinfo *ai = apps_get(ps->p[i].app);
			char key[96];
			if (ai && (!strcmp(expand, "all") || !strcasecmp(expand, ai->exec) || !strcasecmp(expand, ai->name))) {
				snprintf(key, sizeof key, "a:%d", ps->p[i].app);
				if (!exp_has(g, key))
					exp_toggle(g, key);
			}
		}
	}
	if (expand && (!strcmp(expand, "all") || !strcmp(expand, user_name(getuid())))) {
		char key[96];
		snprintf(key, sizeof key, "u:%u", getuid());
		exp_toggle(g, key);
	}
	set_tab(g, tab);
	if (select_text && wants_procs(g)) {
		struct model *m = tab == TAB_PROCS ? &g->procs_m : tab == TAB_USERS ? &g->users_m : &g->det_m;
		char needle[96];
		snprintf(needle, sizeof needle, "%s", select_text);
		for (char *c = needle; *c; c++)
			*c = (char)tolower((unsigned char)*c);
		for (int i = 0; i < m->n; i++)
			if (m->r[i].kind != ROW_SECTION && contains_ci(m->r[i].label, needle)) {
				snprintf(g->ts[tab].sel, sizeof g->ts[tab].sel, "%s", m->r[i].key);
				break;
			}
	}
	fl_timer_arm(g->tick, g->interval_ms, g->interval_ms);
	fl_app_watch_conf(g->app, NULL, NULL);
	fl_app_run(g->app);
	fl_app_free(g->app);
	return 0;
}
