/* flportal -- shared declarations
 * SPDX-License-Identifier: MIT
 */
#ifndef FLPORTAL_H
#define FLPORTAL_H

#include <flui.h>
#include <gio/gio.h>

/* One pending portal request (a call of the frontend, xdg-desktop-portal). It is exported as
 * org.freedesktop.impl.portal.Request at the handle the frontend gave, so that the frontend can
 * Close it, and ends with one reply (response code and results). */
struct req;

/* A window on the screen for a request (file chooser, app chooser, access question). */
struct dlg {
	struct dlg *next;
	struct req *req;
	fl_win *win;
	int dead;                                   /* answered: the window goes at the next loop turn */
	void (*update)(struct dlg *d, GVariant *p); /* UpdateChoices; may be NULL */
	void (*test)(struct dlg *d, int accept);    /* FLPORTAL_SHOT: after the first frame */
	void (*destroy)(struct dlg *d);             /* frees d and what it owns */
};

/* ---- main.c ---- */
/* Answer the request: response 0 ok, 1 cancelled, 2 other. results is a{sv} (floating or NULL). */
void portal_reply(struct req *r, unsigned response, GVariant *results);
/* Attach a window to the request and show it. Takes over d (freed with d->destroy). Returns 0, or -1
 * when there is no compositor (the request is answered with 2 then). */
int portal_dialog_open(struct req *r, struct dlg *d, const fl_win_opts *o);
fl_app *portal_app_get(void); /* the compositor connection, or NULL when no dialog is open */
int portal_test_mode(void); /* 1 when FLPORTAL_SHOT is set: render, answer, no window stays */
struct req *portal_req_find(const char *handle);
struct dlg *portal_req_dlg(struct req *r);
void portal_say(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

/* ---- filechooser.c ---- */
void fc_open(struct req *r, int kind, const char *title, GVariant *options);
enum { FC_OPEN, FC_SAVE, FC_SAVEFILES };

/* ---- appchooser.c ---- */
void ac_open(struct req *r, const char *const *choices, GVariant *options);
void ac_access(struct req *r, const char *title, const char *subtitle, const char *body,
               GVariant *options);

/* ---- small things the dialogs share (ui.c) ---- */
struct inmask {
	int mouse_in, press, release, wheel, btn_held;
	uint32_t key;
	char text[16];
};
void ui_mask(fl_ui *ui, struct inmask *m);
void ui_unmask(fl_ui *ui, const struct inmask *m);
/* the dimmed backdrop and the panel of a modal box with the accent bar on top */
void ui_modal_frame(fl_ui *ui, int w, int h, fl_rect *box);
/* Word-wrapped text in r; returns the height used. Draws nothing when ui is NULL. */
int ui_wrap(fl_ui *ui, fl_font *f, fl_rect r, fl_color c, const char *s, int max_lines);
/* byte length of the longest prefix of s that fits in w px (at least one character) */
int ui_fit_prefix(fl_font *f, const char *s, int w);
/* a letter on a raised tile (apps have no icons in flui) */
void ui_tile(fl_ui *ui, fl_rect r, const char *name);

#endif
