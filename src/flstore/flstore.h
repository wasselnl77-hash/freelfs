/* flstore.h -- the Software Store's state, shared by flstore.c (logic) and views.c (drawing)
 * SPDX-License-Identifier: MIT */
#ifndef FLSTORE_H
#define FLSTORE_H

#include <flui.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#include "store.h"

enum { PG_HOME, PG_CAT, PG_SEARCH, PG_PKGS, PG_APP, PG_INST, PG_UPD };
enum { TAB_EXPLORE, TAB_INST, TAB_UPD };
enum { SRC_FH, SRC_FI, SRC_PR, SRC_PI }; /* Flathub app (in the catalog), Flatpak app not in it, repo package, installed package */
enum { ST_AVAIL, ST_INST, ST_OLDER };
enum { IDX_NONE, IDX_FETCH, IDX_BUILD, IDX_READY, IDX_ERR };

/* widget ids */
enum {
	ID_TAB_EXPLORE = 1, ID_TAB_INST, ID_TAB_UPD, ID_SEARCH, ID_BACK, ID_PRIMARY, ID_SECOND, ID_THIRD, ID_PG_PREV,
	ID_PG_NEXT, ID_SHOT_PREV, ID_SHOT_NEXT, ID_CHIP0, ID_CHIP1, ID_UPD_ALL, ID_CHECK, ID_RETRY, ID_ADD_REMOTE,
	ID_BAR_DETAILS, ID_BAR_CLOSE, ID_BAR_CANCEL, ID_UNUSED, ID_ILIST, ID_ULIST, ID_SCROLL, ID_CONFIRM, ID_CANCEL,
	ID_SIDE = 100, /* + row */
	ID_CARD = 200, /* + card */
};

enum { PAGE_SIZE = 24, NFEAT = 8, NRECENT = 12, MAX_STACK = 16, MAX_Q = 16, MAX_KV = 20 };

struct ref {
	int src, idx;
};

struct pk {
	char *name, *ver, *sum;
	int state;
};

struct fi {
	char *id, *name, *ver;
};

/* what a card or a row shows, whatever the source */
struct rview {
	const char *id, *name, *sum, *ver, *dev;
	int state, src;
	const struct app *app; /* the catalog entry, or NULL */
};

struct nav {
	int page, cat, pg, scroll, shot, inst_mode;
	char query[128];
	struct ref app;
	char app_id[100];
	int app_pkg;
};

struct kv {
	char key[24], val[300];
};

struct chg {
	int kind;
	char id[96], name[96];
};

enum { CK_FINSTALL, CK_FUPDATE, CK_FREMOVE, CK_FUPDATE_ALL, CK_FUNUSED, CK_PKINSTALL, CK_PKREMOVE, CK_ADDREMOTE };

/* child processes: one slot per kind */
enum { J_FETCH, J_BUILD, J_FINST, J_FUPD, J_REMOTE, J_INFO, J_PKLIST, J_PKREPO, J_PKUPD, J_SYNC, J_CHANGE, J_IMG0, J_IMGN = 3,
       J_COUNT = J_IMG0 + J_IMGN };

struct St;

struct job {
	struct St *s;
	int kind, tag;
	pid_t pid;
	int fd[2];
	char *buf[2];
	size_t n[2], cap[2];
	int status;
};

/* images in memory (icons and screenshots), by what they are shown as */
enum { IM_NEW, IM_QUEUED, IM_LOADING, IM_READY, IM_FAILED };
#define IMG_MAX 96
struct ient {
	char key[200], path[640], url[460];
	int w, h, state;
	simg *im;
	unsigned used;
};

struct St {
	fl_app *app;
	fl_win *win;
	fl_font *small, *tiny, *title, *big, *mono;
	fl_timer *anim_timer, *search_timer, *note_timer, *pump_timer;

	const char *flpkg, *seat, *conf, *flatpak, *curl, *appstream_url, *icon_url;
	char cache[512], self[512];

	/* the Flathub catalog */
	struct db db;
	int idx_state;
	char idx_err[300];
	uint8_t *st; /* state of every catalog app */

	/* Flatpak */
	int fl_have, fl_remote; /* -1: not asked yet */
	struct fi *fin;
	size_t nfin;
	int fin_loaded;
	char **upd;
	size_t nupd;
	int upd_loaded, checking, upd_net;
	time_t checked;

	/* flpkg */
	struct pk *pkinst, *pkrepo, *pkup;
	size_t npkinst, npkrepo, npkupd, npkup;
	int pkinst_loaded, pkrepo_loaded, repo_cfg;

	/* where we are */
	int tab;
	struct nav cur, stack[MAX_STACK];
	int nstack;
	char query[128], last_query[128];
	int scroll, content_h;

	/* lists of what the current page shows */
	struct ref feat[NFEAT], recent[NRECENT];
	int nfeat, nrecent;
	int catcount[NCATS];
	struct ref *list;
	int nlist;
	struct ref *ilist; /* Installed */
	int nilist, nipk, niapp;
	fl_list_state ils;
	struct ref *ulist; /* Updates */
	int nulist;
	fl_list_state uls;

	/* the app page */
	struct detail det;
	int det_ok;
	struct kv kv[MAX_KV];
	int nkv, info_ok;
	char info_for[100];
	int info_tried;
	int ask_remove;

	/* changes */
	struct chg q[MAX_Q];
	int nq;
	struct chg run;
	int running;
	char bar_text[300], bar_sub[200];
	int bar_visible, bar_err, bar_log;
	/* what a running change downloads: bytes the network interfaces received since it started */
	unsigned long long net0, net_prev;
	long net_t0_ms, net_prev_ms;
	double net_rate;
	long free_mb;		/* free space where the Flatpak data goes, read when an app page opens (-1: unknown) */
	int live;		/* the live system: its disk is memory, installed apps are gone after shutdown */

	struct job job[J_COUNT];
	struct ient im[IMG_MAX];
	unsigned frame;
	int dec_budget;
	int dec_more;

	int act, act_arg;
	int inited;
	char shot_arg[64];
};

/* flstore.c */
void queue(struct St *s, int act, int arg);
const char *state_label(int st);
void rview(const struct St *s, struct ref r, struct rview *v);
int ref_busy(const struct St *s, struct ref r); /* queued or running: 0 no, 1 waiting, 2 running */
const simg *img_get(struct St *s, const char *key, const char *path, const char *url, int w, int h);
const simg *icon_get(struct St *s, const struct rview *v, int px);
const simg *shot_get(struct St *s, int n, int w, int h);
int job_running(const struct job *j);
int steps_of(const char *buf, size_t n, int *cur, int *total);
unsigned long long net_rx_total(void);
void net_tick(struct St *s);
int pct_of(const char *buf, size_t n);
void say(struct St *s, int err, const char *fmt, ...);
int pk_state(const struct St *s, const char *name);
int cat_total(const struct St *s, int cat);
const char *kv_get(const struct St *s, const char *a, const char *b);

void frame_begin(struct St *s);
void frame_end(struct St *s);

/* views.c */
void draw(fl_ui *ui, void *ud);

/* actions */
enum {
	A_NONE, A_TAB, A_HOME, A_CAT, A_PKGS, A_OPEN_REF, A_BACK, A_SEARCH, A_PAGE, A_INSTALL, A_UPDATE, A_ASK_REMOVE,
	A_REMOVE, A_CANCEL, A_OPEN_APP, A_SHOT, A_CHIP, A_UPDATE_ALL, A_CHECK, A_RETRY, A_ADD_REMOTE, A_BAR_CLOSE,
	A_BAR_LOG, A_BAR_CANCEL, A_UNUSED,
};

#endif
