/* xwm-int.h -- what xwm.c, xwsel.c and xwdeco.c share. SPDX-License-Identifier: MIT */
#ifndef FLCOMP_XWM_INT_H
#define FLCOMP_XWM_INT_H

#include "comp.h"

#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <xcb/composite.h>
#include <xcb/res.h>
#include <xcb/xfixes.h>

/* every atom the window manager uses by name; a field of X.a */
#define XWM_ATOMS(A) \
	A(WM_S0) A(WM_PROTOCOLS) A(WM_DELETE_WINDOW) A(WM_TAKE_FOCUS) A(WM_STATE) A(WM_CHANGE_STATE) \
	A(WM_NORMAL_HINTS) A(WM_HINTS) A(WM_TRANSIENT_FOR) A(WM_CLASS) A(WM_NAME) \
	A(UTF8_STRING) A(WL_SURFACE_SERIAL) A(_MOTIF_WM_HINTS) \
	A(_NET_SUPPORTED) A(_NET_SUPPORTING_WM_CHECK) A(_NET_WM_NAME) A(_NET_WM_PID) \
	A(_NET_WM_STATE) A(_NET_WM_STATE_FULLSCREEN) A(_NET_WM_STATE_MAXIMIZED_VERT) \
	A(_NET_WM_STATE_MAXIMIZED_HORZ) A(_NET_WM_STATE_HIDDEN) A(_NET_WM_STATE_FOCUSED) \
	A(_NET_WM_STATE_SKIP_TASKBAR) A(_NET_WM_STATE_MODAL) A(_NET_WM_STATE_ABOVE) \
	A(_NET_ACTIVE_WINDOW) A(_NET_CLIENT_LIST) A(_NET_CLIENT_LIST_STACKING) \
	A(_NET_NUMBER_OF_DESKTOPS) A(_NET_CURRENT_DESKTOP) A(_NET_DESKTOP_GEOMETRY) A(_NET_WORKAREA) \
	A(_NET_FRAME_EXTENTS) A(_NET_REQUEST_FRAME_EXTENTS) A(_NET_CLOSE_WINDOW) A(_NET_WM_MOVERESIZE) \
	A(_NET_WM_DESKTOP) A(_NET_WM_WINDOW_TYPE) A(_NET_WM_WINDOW_TYPE_NORMAL) \
	A(_NET_WM_WINDOW_TYPE_DIALOG) A(_NET_WM_WINDOW_TYPE_UTILITY) A(_NET_WM_WINDOW_TYPE_SPLASH) \
	A(_NET_WM_WINDOW_TYPE_DOCK) A(_NET_WM_WINDOW_TYPE_MENU) A(_NET_WM_WINDOW_TYPE_DROPDOWN_MENU) \
	A(_NET_WM_WINDOW_TYPE_POPUP_MENU) A(_NET_WM_WINDOW_TYPE_TOOLTIP) \
	A(_NET_WM_WINDOW_TYPE_NOTIFICATION) A(_NET_WM_WINDOW_TYPE_COMBO) A(_NET_WM_WINDOW_TYPE_DND) \
	A(_NET_WM_WINDOW_TYPE_DESKTOP) \
	A(CLIPBOARD) A(TARGETS) A(TEXT) A(INCR) A(TIMESTAMP) A(_FLCOMP_SEL)

struct xatoms {
#define A(n) xcb_atom_t n;
	XWM_ATOMS(A)
#undef A
};

#define XD_TITLE 30		/* the title bar flcomp draws above an X window that has none */
#define XD_BORDER 3		/* and the frame around the sides and below */

/* an X window the window manager knows about */
struct xwin {
	xcb_window_t id;
	struct wl_list link;		/* xw_all */
	bool override;			/* override-redirect: nobody manages it */
	bool unmanaged;			/* override, or a menu, tooltip, dock...: placed where the client wants it */
	bool mapped_x;			/* mapped in the X server */
	bool map_wanted;		/* MapRequest seen, waiting for its properties */
	bool typed;			/* the type and hints were read (unmanaged is final) */
	int npend;			/* property requests in flight */
	int x, y, w, h;			/* the X geometry as last known */
	int sx, sy;			/* last position flcomp told the server */
	bool sent_pos;
	int cw, ch;			/* last size flcomp asked for */
	uint64_t serial;		/* WL_SURFACE_SERIAL, 0: none yet */
	struct surface *surf;		/* its wl_surface once Xwayland made one */
	struct window *win;		/* the managed window (NULL: unmanaged, or no surface yet) */
	char *title, *net_title, *klass, *instance;
	xcb_window_t transient_for;
	uint32_t types;			/* bit mask, see xwm.c */
	bool deco;			/* flcomp draws the frame */
	bool nodeco_hint;		/* _MOTIF_WM_HINTS asked for no decoration */
	bool input_hint, has_input_hint, take_focus, delete_win;
	int min_w, min_h, max_w, max_h, inc_w, inc_h, base_w, base_h;
	bool want_full, want_max, skip_taskbar;
	uint32_t shown_state;		/* the _NET_WM_STATE flags last written */
	int wm_state;			/* WM_STATE last written: 0 none, 1 normal, 3 iconic */
	int ws_seen;			/* workspace the unmanaged window was mapped on */
	/* decoration state (xwdeco.c) */
	int hover, pressed;
};

struct xwm {
	xcb_connection_t *c;
	xcb_screen_t *scr;
	xcb_window_t root, win;		/* win: our own window (selection, _NET_SUPPORTING_WM_CHECK) */
	struct xatoms a;
	const xcb_query_extension_reply_t *xfixes;
	struct wl_list all;		/* struct xwin, creation order */
	uint32_t last_time;		/* of the last X event with a timestamp */
};
extern struct xwm X;

struct xwin *xwin_by_id(xcb_window_t id);
xcb_atom_t xwm_intern(const char *name);	/* blocking, for the clipboard's mime types */
char *xwm_atom_name(xcb_atom_t a);		/* malloc'ed */
void xwm_flush(void);

/* xwdeco.c needs these from xdg.c */
void window_request_state(struct window *w, int max, int full);	/* -1: leave it */
void window_move_begin(struct window *w);
void window_resize_begin(struct window *w, unsigned edges);
void xwm_deco_forget(struct window *w);		/* the window is going away */

#endif
