/* gmui.h -- General Manager, window state shared by gm.c and perf.c
 * SPDX-License-Identifier: MIT
 */
#ifndef GMUI_H
#define GMUI_H

#include <flui.h>

#include "gm.h"

enum { TAB_PROCS, TAB_PERF, TAB_STARTUP, TAB_USERS, TAB_DETAILS, TAB_COUNT };
enum { PERF_CPU, PERF_MEM, PERF_DISK, PERF_NET, PERF_GPU };

enum { ROW_SECTION, ROW_GROUP, ROW_CHILD, ROW_DETAIL };

struct row {
	char key[96];
	char label[96], sub[48];
	int kind, depth;
	int expandable, expanded;
	int app;          /* index of the .desktop entry, or -1 */
	int pidx;         /* ROW_DETAIL: index into procs */
	int disk_ok;
	double cpu, disk;
	u64 mem;
	int tpos, tn;     /* targets: pids in model.pool */
};

struct model {
	struct row *r;
	int n, cap;
	int *pool;
	int npool, poolcap;
};

struct tstate {
	int scroll;
	char sel[96];
};

struct fonts {
	fl_font *sm;    /* 12 */
	fl_font *tiny;  /* 11 */
	fl_font *h1;    /* 20 semibold */
	fl_font *title; /* 24 semibold */
	fl_font *h2;    /* 15 semibold */
	fl_font *big;   /* 28 semibold */
	fl_font *mid;   /* 18 medium */
	fl_font *mono;  /* 12 plex mono */
};

struct perf_entry {
	int kind, index;
	char title[64], line1[64], line2[64];
	const struct hist *h;
	const struct hist *h2; /* second series or NULL */
	float max;             /* 0: auto */
};

struct gm {
	fl_app *app;
	fl_win *win;
	fl_timer *tick, *quick;
	struct fonts f;
	struct sys sys;
	struct procs ps;
	int tab;
	int perf_sel;          /* index into the entry list */
	int cores_view;        /* CPU: logical processors instead of one graph */
	char search[64];
	int interval_ms;

	struct model procs_m, users_m;
	struct model det_m;
	int need_rebuild;

	int sort_col[TAB_COUNT], sort_desc[TAB_COUNT];
	struct tstate ts[TAB_COUNT];
	char (*exp)[96];
	int nexp, expcap;
	int sect_closed[3];

	struct startup *su;
	int nsu;
	int su_scroll;

	char msg[160];
	int msg_err;
	double msg_until;

	int perf_scroll, pane_scroll, pane_need;
	int width, height;     /* --size, for screenshots */
};

/* helpers of gm.c used by perf.c */
void gm_say(struct gm *g, int err, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
fl_color gm_alpha(fl_color c, int a);
void gm_text(fl_ui *ui, fl_font *f, fl_rect r, int align, fl_color c, const char *s);
void gm_chevron(fl_canvas *cv, int cx, int cy, int dir /* 0 right, 1 down, 2 up */, float s, fl_color c);

/* perf.c */
void perf_draw(fl_ui *ui, struct gm *g, fl_rect r);

#endif
