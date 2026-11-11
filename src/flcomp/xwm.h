/* xwm.h -- Xwayland and flcomp's X11 window manager (xwm.c, xwsel.c, xwdeco.c).
 * Included at the end of comp.h. SPDX-License-Identifier: MIT */
#ifndef FLCOMP_XWM_H
#define FLCOMP_XWM_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

struct window;
struct surface;
struct rect;
struct xwin;

/* xwm.c: lifecycle. Xwayland runs only while an X client is connected: flcomp listens on the
 * X socket from the start (DISPLAY is set for every program of the session) and starts
 * Xwayland on the first connection; it ends itself when the last client is gone. */
void xwm_init(void);				/* after the Wayland socket exists; not for --firstboot */
void xwm_fini(void);				/* flcomp ends: stop Xwayland, remove the socket */
bool xwm_child_exited(pid_t pid, int status);	/* SIGCHLD: true if it was Xwayland */
bool xwm_running(void);				/* Xwayland is up and the window manager is connected */
const char *xwm_display(void);			/* ":0", or NULL when X support is off */

/* surface side (surface.c): ROLE_XWL surfaces belong to Xwayland */
void xwm_surface_commit(struct surface *s, bool was_mapped);
void xwm_surface_gone(struct surface *s);
void xwm_surface_origin(struct surface *s, int *x, int *y);

/* window side (xdg.c): a struct window with ->xw set is an X window */
void xwm_window_configure(struct window *w, int cw, int ch);	/* the size flcomp wants (0, 0: the client's choice) */
void xwm_window_close(struct window *w);
void xwm_window_gone(struct window *w);
void xwm_window_changed(struct window *w);		/* state (maximized, fullscreen, minimized) or title changed */
bool window_skip_panel(struct window *w);		/* _NET_WM_STATE_SKIP_TASKBAR: not in the panel */
void window_commit_xw(struct window *w, bool was_mapped);	/* xwm.c -> xdg.c: the surface of an X window committed */
void xwm_focus_sync(void);				/* C.focused or the focus rules changed */
void xwm_sync(void);					/* before every repaint: X geometry and stacking follow flcomp's */

/* unmanaged X windows (menus, tooltips, override-redirect): drawn over the managed ones */
int xwm_or_list(struct surface **out, int max);		/* the ones on screen, bottom to top */
bool xwm_or_visible(void);				/* one is on screen: a fullscreen window cannot own the screen */

/* frames drawn by flcomp around X windows that have none (xwdeco.c) */
struct xinset { int l, t, r, b; };
struct xinset xwm_insets(struct window *w);		/* all 0 for windows without a frame of ours */
void xwm_deco_draw(struct window *w, struct rect clip);
void xwm_deco_damage(struct window *w);
struct rect xwm_deco_outer(struct window *w);		/* the frame and the window, for damage */
void xwm_deco_note(struct window *w);			/* pick(): the frame of this window is under the pointer */
bool xwm_deco_contains(struct window *w, int x, int y);
struct window *xwm_deco_target(void);			/* what pick() noted since it was last cleared */
void xwm_deco_hover(void);				/* the pointer moved */
void xwm_deco_press(struct window *w);			/* left button on a frame */
void xwm_deco_release(void);				/* the button was let go */
bool xwm_deco_active(void);				/* a press on a frame is under way */

/* xwsel.c: the clipboard between X and Wayland (CLIPBOARD; PRIMARY stays among X clients) */
void xwsel_init(void);					/* the X connection is up */
void xwsel_fini(void);
bool xwsel_event(void *ev);				/* an xcb event: true if the clipboard code took it */
void xwsel_wayland_changed(void);			/* seat.c: a Wayland client owns, or no longer owns, the selection */

/* seat.c: the clipboard seen by xwsel.c. The "foreign" source is an X client's selection. */
struct fsel_ops {
	void (*send)(void *data, const char *mime, int fd);	/* write the data to fd, then close it */
	void (*cancelled)(void *data);				/* another selection replaced this one */
};
void seat_set_foreign_selection(const char *const *mimes, int n, const struct fsel_ops *ops, void *data);
void seat_clear_foreign_selection(void *data);		/* only if it is still the current one */
int seat_wayland_selection_mimes(const char **out, int max);	/* the Wayland client's selection, 0: none (or an X one) */
bool seat_wayland_selection_send(const char *mime, int fd);
void seat_selection_watch(void (*cb)(void));

#endif
