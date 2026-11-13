/* flpanel.h -- shared between the panel and the start menu
 * SPDX-License-Identifier: MIT
 */
#ifndef FLPANEL_H
#define FLPANEL_H

#include <flui.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>

#include "fl-shell-client-protocol.h"

enum { CAT_ALL, CAT_INTERNET, CAT_OFFICE, CAT_SYSTEM, CAT_SETTINGS, CAT_RECENT, CAT_COUNT };

struct entry {
	char name[64], what[96], exec[256], glyph[8];
	unsigned cats; /* bit per CAT_* */
	int terminal;
	int desktop; /* X-FreeLFS-Desktop-Icon=true: also an icon on the desktop (deskicons.c) */
};

struct win {
	uint32_t id;
	char app_id[64], title[128];
	int focused, minimized;
	int ws; /* workspace (fl_shell version 5) */
	int ex, ew, eh; /* where the panel last said this window's entry is (fl_shell 7), ex = ew = 0: not yet */
};

struct panel {
	fl_app *app;
	fl_win *pw, *mw, *fw; /* panel, start menu, volume flyout */
	struct fl_shell *shell;
	struct win *wins;
	int nwins;
	int menu_open;
	char layout[16]; /* as the compositor says: "de", "gb" */
	int demo;
	int out_w;
	int cur_h; /* the panel height the compositor was last told */
	fl_timer *clock;
	fl_font *f_small, *f_mono, *f_what, *f_cap;
	/* menu */
	struct entry *apps;
	int napps;
	int cat;
	char query[96];
	fl_list_state list;
	int *shown; /* indexes into apps for the current category and query */
	int nshown;
	char user[64];
	/* tray (panel.c): -1 means unknown or absent */
	int bat_pct, bat_charging;
	int net_kind;  /* 0 none, 1 Wi-Fi, 2 wired */
	int net_up;
	int vol_pct, vol_muted;
	fl_timer *tray_timer;
	int tray_tick, vol_fails;
	pid_t vol_pid;
	int vol_fd;
	int vol_job, vol_age, vol_stale, vol_want_query; /* the one wpctl child, see panel.c */
	int vol_want_pct, vol_want_mute;                 /* queued changes, -1: none */
	int fly_open, vol_cx;                            /* the flyout, and where the icon is */
	int shell_ver;
	char vol_buf[96];
	size_t vol_len;
	/* notifications (notify.c) and battery (battery.c) */
	struct notify *nt;
	struct battery *bt;
	int clock_cx;      /* where the clock is: the notification centre opens above it */
	int bat_cx;        /* and the battery icon: its flyout */
	int locked;        /* fl_shell lock_state */
	fl_font *f_head, *f_big;
	int ws_cols, ws_rows, ws_cur; /* workspaces (fl_shell version 5): the pager in the panel; 0 cols: none */
	fl_win *wsw;                  /* the pager's right-click menu (wsmenu.c) */
	int ws_open, ws_sel, ws_cx;   /* it is shown, the highlighted item, and where the pager is */
	int ws_mx, ws_my;             /* the pointer in it at the last pass */
};

enum { VJ_NONE, VJ_QUERY, VJ_SET };

/* Hide a raw window and forget where the pointer was in it: the compositor sends no leave when a
 * surface is unmapped, and a stale hover would tint the next card and pause its timer. */
static inline void panel_hide(fl_win *w)
{
	fl_win_hide(w);
	fl_ui *u = fl_win_ui(w);
	u->mouse_in = 0;
	u->active = 0;
	u->btn_held = 0;
}

/* which flyout is open; one at a time (panel.c) */
enum { FLY_NONE, FLY_VOL, FLY_BAT, FLY_NC, FLY_WS, FLY_CTX };
void panel_flyouts_close(struct panel *p, int except);
void panel_open_flyout(struct panel *p, int kind, fl_win *w, int cx); /* closes the others, then shows w */

/* ---- notify.c: org.freedesktop.Notifications, the cards and the notification centre ---- */
struct notify_req {
	const char *app, *icon, *summary, *body;
	int urgency;       /* 0 low, 1 normal, 2 critical */
	int timeout_ms;    /* -1 default for the urgency, 0 never */
	uint32_t replaces; /* 0: a new one */
	int transient;     /* not kept in the history */
	int internal;      /* actions go to battery_action, not to a D-Bus client */
	int n_actions;
	const char *akey[3], *alabel[3];
};
void notify_init(struct panel *p);
void notify_free(struct panel *p);
uint32_t notify_post(struct panel *p, const struct notify_req *r);
void notify_close(struct panel *p, uint32_t id, int reason); /* 3: closed by the sender */
int notify_unread(const struct panel *p);
int notify_count(const struct panel *p);
int notify_dnd(const struct panel *p);
void notify_center_toggle(struct panel *p);
void notify_center_close(struct panel *p);
int notify_center_is_open(const struct panel *p);
void notify_locked(struct panel *p, int locked);
void notify_conf_changed(struct panel *p);
void notify_demo(struct panel *p, int center);
void draw_bell(fl_canvas *cv, int x, int y, int size, fl_color c); /* the bell of the clock button */

/* ---- battery.c: the battery reading, its flyout, power saving, low battery warnings ---- */
void battery_init(struct panel *p);
int battery_read(struct panel *p); /* sets bat_pct and bat_charging; 1 when something changed */
void battery_flyout_toggle(struct panel *p);
void battery_flyout_close(struct panel *p);
int battery_flyout_is_open(const struct panel *p);
void battery_action(struct panel *p, const char *key);
void battery_conf_changed(struct panel *p);
int battery_saver(const struct panel *p);
void battery_demo(struct panel *p, int open);

/* ---- wsmenu.c: the pager's right-click menu ---- */
#define WSM_W 232
#define WSM_H 150 /* 2 (edge) + 6 + 4 rows of 30 + 2 rules of 9 + 4: the rows are laid out in wsmenu.c */
void wsmenu_init(struct panel *p);
void wsmenu_draw(fl_ui *ui, void *ud);
void wsmenu_open(struct panel *p);
void wsmenu_close(struct panel *p);
void wsmenu_toggle(struct panel *p);
int wsmenu_is_open(const struct panel *p);

/* ---- ctxmenu.c: right-click menus (taskbar, desktop, desktop icons), a flyout above the panel ---- */
struct ctx_item {
	const char *label; /* NULL: a separator line */
	int on;            /* 0: shown, not chosen */
	int code;          /* what the pick callback gets */
};
typedef void (*ctx_pick_cb)(struct panel *p, uintptr_t ud, int code);
void ctx_open(struct panel *p, int cx, const struct ctx_item *items, int n, ctx_pick_cb pick, uintptr_t ud);
void ctx_close(struct panel *p);

/* ---- dialog.c: the power confirmation (30 s countdown, then flpower), an error with the command's
 * stderr, and a name (new folder, new file, rename). Decorated windows, one at a time. ---- */
enum { POW_OFF, POW_RESTART, POW_LOGOUT };
typedef void (*dlg_name_cb)(struct panel *p, int kind, const char *ctx, const char *name);
void dlg_power(struct panel *p, int what);
void dlg_error(struct panel *p, const char *title, const char *text);
void dlg_name(struct panel *p, const char *title, const char *initial, int kind, const char *ctx, dlg_name_cb done);

/* A shell command for launch(): the string in single quotes, and % doubled, because launch()
 * drops the Exec field codes. 0 when it does not fit in cap. */
static inline int fl_shq(char *out, size_t cap, const char *in)
{
	size_t n = 0;
	if (n + 1 >= cap)
		return 0;
	out[n++] = '\'';
	for (const char *s = in; *s; s++) {
		const char *rep = *s == '\'' ? "'\\''" : *s == '%' ? "%%" : NULL;
		size_t k = rep ? strlen(rep) : 1;
		if (n + k + 2 >= cap)
			return 0;
		if (rep)
			memcpy(out + n, rep, k);
		else
			out[n] = *s;
		n += k;
	}
	out[n++] = '\'';
	out[n] = 0;
	return 1;
}

void menu_init(struct panel *p);
void desk_reposition(struct panel *p); /* the panel moved or changed height: the icons keep clear of it */
void desk_init(struct panel *p); /* deskicons.c: icons on the desktop (fl_shell version 6) */
#define FLY_W 260
#define FLY_H 76
void vol_set_pct(struct panel *p, int pct);
void vol_toggle_mute(struct panel *p);
void flyout_draw(fl_ui *ui, void *ud);
void flyout_close(struct panel *p);
void menu_open(struct panel *p);
void menu_close(struct panel *p);
void menu_draw(fl_ui *ui, void *ud);
void launch(const char *exec, int terminal);

#endif
