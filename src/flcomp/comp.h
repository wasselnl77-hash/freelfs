/* flcomp -- shared declarations.
 * SPDX-License-Identifier: MIT */
#ifndef FLCOMP_H
#define FLCOMP_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server.h>

#include "flpalette.h"

/* ---- geometry and damage -------------------------------------------- */

struct rect { int x1, y1, x2, y2; };		/* half open */

#define DMG_MAX 12
struct damage { struct rect r[DMG_MAX]; int n; };

static inline int rect_w(struct rect r) { return r.x2 - r.x1; }
static inline int rect_h(struct rect r) { return r.y2 - r.y1; }
static inline bool rect_empty(struct rect r) { return r.x2 <= r.x1 || r.y2 <= r.y1; }

bool rect_intersect(struct rect a, struct rect b, struct rect *out);
struct rect rect_union(struct rect a, struct rect b);
void dmg_add(struct damage *d, struct rect r);

/* ---- objects -------------------------------------------------------- */

enum role { ROLE_NONE, ROLE_TOPLEVEL, ROLE_POPUP, ROLE_SUB, ROLE_PANEL, ROLE_MENU, ROLE_FLYOUT, ROLE_NOTIFY, ROLE_LOCK, ROLE_XWL, ROLE_DESKTOP };

struct regionop { struct rect r; bool add; };
struct regionops { struct regionop *v; int n; };

struct reslink { struct wl_resource *res; struct wl_list link; };

struct frame_cb { struct wl_resource *res; struct wl_list link; };

struct surface {
	struct wl_resource *res;
	enum role role;
	uint32_t *px;			/* our copy of the buffer, XRGB/ARGB */
	int w, h;
	bool alpha;
	struct wl_resource *p_buf;	/* pending state */
	bool p_attached;
	struct wl_listener buf_destroy;
	struct rect p_dmg;
	bool p_dmg_set;
	struct regionops p_input;
	bool p_input_chg, p_input_def;
	struct regionops input;
	bool input_def;
	struct wl_list p_frames, frames;	/* struct frame_cb */
	struct wl_list subs;		/* struct subsurf, bottom to top */
	struct xdg_surf *xs;
	struct window *win;
	struct popup *popup;
	struct subsurf *sub;
	/* ROLE_XWL: a surface of Xwayland (xwm.c) */
	struct xwin *xw;		/* the X window it belongs to, once the serials matched */
	struct wl_resource *xs_res;	/* its xwayland_surface_v1 */
	uint64_t xs_pend, xs_ser;	/* set_serial: pending (until commit), and committed */
	bool xs_has_pend;
	struct wl_list link;		/* C.surfaces */
};

struct subsurf {
	struct wl_resource *res;
	struct surface *surf, *parent;
	int x, y;
	struct wl_list link;
};

struct xdg_surf {
	struct wl_resource *res;
	struct surface *surf;
	struct rect geom, p_geom;
	bool geom_set, p_geom_set;
	bool configured;
	uint32_t serial, acked;
	struct window *win;
	struct popup *popup;
};

struct window {
	uint32_t id;
	struct wl_resource *res;	/* xdg_toplevel */
	struct surface *surf;
	struct xdg_surf *xs;
	char *title, *app_id;
	struct window *parent;
	int x, y;			/* top left of the window geometry */
	bool mapped, announced, minimized, maximized, fullscreen, resizing;
	bool above;			/* always on top (the window menu, popup.c) */
	bool ssd;			/* xdg-decoration: flcomp draws the title bar and frame (xwdeco.c) */
	bool want_csd;			/* the client asked for its own decorations */
	struct wl_resource *deco_res;	/* its zxdg_toplevel_decoration_v1 */
	int deco_ours;			/* 0 not asked, 1 a FreeLFS program draws its title bar (popup.c), 2 not */
	int ws;				/* workspace it is on (0 .. C.ws_cols * C.ws_rows - 1) */
	int snap;			/* -1 left half, 1 right half of the work area, 0 not snapped (restore has the old place) */
	int min_w, min_h, max_w, max_h;
	int cfg_w, cfg_h;		/* last size we configured */
	struct rect entry;		/* its entry in the panel (fl_shell set_window_entry), panel surface coordinates */
	bool has_entry;
	struct rect restore;		/* geometry before maximize */
	bool have_restore;
	unsigned edges;
	int anchor_r, anchor_b;
	uint32_t release_serial;
	struct xwin *xw;		/* an X11 window (xwm.c); res and xs are NULL then */
	struct wl_list link;		/* C.windows, top first */
};

struct popup {
	struct wl_resource *res;
	struct xdg_surf *xs;
	struct surface *surf, *parent;
	int x, y, w, h;			/* relative to the parent's geometry */
	bool grab, dismissed;
	struct wl_list link;		/* C.popups, oldest first */
};

/* ---- global state --------------------------------------------------- */

struct comp {
	struct wl_display *dpy;
	struct wl_event_loop *loop;
	int w, h, refresh;		/* the desktop's size (logical; all enabled outputs together) and the primary's refresh in mHz */
	int pw, ph, scale;		/* the primary output's mode in pixels; its scale in percent (100..200) */
	struct rect prim;		/* the primary output on the desktop (logical pixels): panel, maximize, fullscreen */
	uint32_t *shadow;		/* w*h XRGB, always complete */
	bool headless, active;
	struct damage dmg, prev;
	struct wl_list surfaces, windows, popups;
	struct wl_list outputs, shells;	/* struct reslink */
	struct surface *panel, *menu, *flyout, *notify, *desktop;
	int desktop_x, desktop_y;	/* fl_shell set_desktop: the icon layer (surface.c, render.c) */
	bool nolock;			/* --firstboot (the login screen, setup): Super+L and the lid do not lock */
	bool locked;			/* the screen is locked (lock.c): only lock_surf is shown and gets input */
	struct surface *lock_surf;	/* the lock client's surface, NULL while there is none (it died, or is starting) */
	struct wl_client *lock_client;
	int panel_h, flyout_x;
	bool menu_dismissed, flyout_dismissed;
	struct window *focused;
	uint32_t next_id;
	/* seat */
	double px, py;
	int btn_down;
	bool has_pointer;
	struct surface *ptr_focus, *kb_focus;
	int exit_code;
	const char *screenshot;
	int ws_cols, ws_rows, ws_cur;	/* workspaces: the grid and the one in view (xdg.c) */
};
extern struct comp C;

uint32_t now_ms(void);
void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void quit(int code);
struct reslink *reslink_add(struct wl_list *l, struct wl_resource *res);
void reslink_destroy(struct wl_resource *res);

/* surface.c */
void surface_init(void);
void surface_origin(struct surface *s, int *x, int *y);
struct rect surface_rect(struct surface *s);
struct rect tree_bounds(struct surface *s);
void damage_tree(struct surface *s);
void surface_unmap(struct surface *s);
struct surface *root_of(struct surface *s);
struct window *window_of_surface(struct surface *s);
struct surface *pick(int x, int y, int *lx, int *ly);
void flush_frames(uint32_t ms);
bool panel_visible(void);
bool menu_visible(void);
bool flyout_visible(void);
bool notify_visible(void);
bool desktop_visible(void);
bool popup_visible(struct popup *p);		/* not dismissed, and its window is on the workspace in view */

/* popup.c: window menu, clipboard history popup, snap preview */
bool pmenu_active(void);
void pmenu_close(void);
bool pmenu_key(uint32_t sym, bool pressed);
void pmenu_motion(void);
bool pmenu_button(uint32_t button, bool pressed);
void pmenu_draw(struct rect clip);
void window_menu_open(struct window *w, int x, int y);
bool window_framed(struct window *w);	/* flcomp draws a frame around it: an X window without its own, or a Wayland one with server-side decorations */
void cliphist_menu_open(void);
bool title_click(struct window *w, uint32_t button, int x, int y);
void snap_preview_set(int kind);		/* 0 none, -1 left half, 1 right half, 2 whole work area */
int snap_preview_kind(void);
void snap_preview_draw(struct rect clip);
/* lock.c: the built-in font */
int ui_text_width(const char *s);
void ui_text(struct rect clip, int x, int y, const char *s, uint32_t col);
/* seat.c: clipboard history (RAM only, the last 20 text entries) */
int cliphist_count(void);
const char *cliphist_get(int i);		/* 0 = newest */
void cliphist_paste(int i);			/* make it the clipboard and paste it into the focused window */

/* xdg.c */
void xdg_init(void);
void xdg_surface_commit(struct surface *s, bool was_mapped);
void shell_surface_commit(struct surface *s, bool was_mapped, bool attached);
struct rect work_area(void);
void work_area_changed(void);
struct window *window_by_id(uint32_t id);
void window_focus(struct window *w);
void window_raise(struct window *w);
void window_minimize(struct window *w);
void window_activate(struct window *w);
void window_close(struct window *w);
void update_kb_focus(void);
void menu_dismiss(void);
void flyout_dismiss(void);
void popups_dismiss(void);
bool grab_active(void);
void grab_motion(void);
void grab_end(void);
void cycle_next(void);
void cycle_prev(void);
void cycle_end(void);
bool cycling(void);
struct window **cycle_windows(int *n, int *sel);	/* the Alt-Tab list, newest first; entries of closed windows are NULL */
enum { WA_SNAP_LEFT, WA_SNAP_RIGHT, WA_MAXIMIZE, WA_RESTORE_OR_MINIMIZE, WA_TOGGLE_MAXIMIZE };
void window_action(int act);				/* keyboard: the focused window */
void window_action_on(struct window *w, int act);	/* the same for any window (popup.c, flshell.c) */
void window_set_above(struct window *w, bool on);
struct rect window_rect(struct window *w);
int window_geom_h_pub(struct window *w);				/* keyboard: the focused window */

/* workspaces (xdg.c): a grid of virtual desktops; a window is on exactly one. Only the
 * windows of C.ws_cur are drawn, picked, focused and in Alt-Tab. */
struct rect window_rect(struct window *w);		/* its window geometry on the desktop */
bool window_visible(struct window *w);			/* mapped, not minimized, on the workspace in view */
void ws_configure(int cols, int rows);			/* desktop.conf workspaces=COLSxROWS; windows past the end move to the last */
void ws_switch(int idx);
void ws_step(int dx, int dy);				/* Super+Ctrl+arrows: one workspace along the grid, no wrap */
void ws_move_focused(int dx, int dy);			/* Super+Shift+arrows: the focused window goes there, the view follows */
void ws_move_window(struct window *w, int idx, bool follow);
int ws_count(void);
bool window_on_ws(struct window *w, int idx);		/* mapped, not minimized, on workspace idx (overview) */

/* overview.c: Super+Tab, every workspace as a tile with its windows (drawn by the compositor) */
bool overview_on(void);
void overview_toggle(void);
void overview_close(void);
void overview_changed(void);				/* the grid, the windows or the view changed: redraw */
bool overview_key(uint32_t sym, bool pressed);		/* while open: every key is ours; true */
void overview_motion(void);				/* the pointer moved (C.px, C.py) */
bool overview_button(bool pressed);			/* true: the press or release was ours (the pointer is over the overview) */
bool overview_has_pointer(void);			/* the pointer is over the overview, not the panel */
void overview_draw(struct rect clip);
const uint32_t *wall_pixels(void);			/* render.c: the wallpaper at C.w x C.h, or NULL */

/* switcher.c: the Alt-Tab row of window pictures */
void switcher_changed(void);
void switcher_draw(struct rect clip);

/* lock.c: the lock screen, enforced here (fl_shell lock/unlock) */
bool lock_available(void);			/* there is a lock program to start */
void lock_start(void);				/* lock now and start the lock program (Super+L, lid) */
void lock_begin(struct surface *s, struct wl_client *c);	/* fl_shell lock */
void lock_end(struct wl_client *c);		/* fl_shell unlock, from the lock client only */
void lock_surface_gone(struct surface *s);
void lock_draw_base(struct rect clip);		/* solid colour, and the hint while nothing runs */
bool lock_retry(void);				/* Enter on the hint screen: start the lock program again */
bool lock_hint_shown(void);
void lock_lid(bool closed);			/* the lid switch */
void lock_surface_committed(struct surface *s);

/* shot.c: Print saves the screen as a PNG in ~/pictures */
void screenshot_take(void);
/* Print and friends hand over to Snappel when it is installed (mode: 0 whole screen, 1 region,
 * 2 the Snappel window); without it Print saves the whole screen itself and the others do nothing */
enum { SHOT_FULL, SHOT_REGION, SHOT_WINDOW };
void screenshot_key(int mode);

/* screencopy.c: fl_screencopy, the screen, a window or a part of the desktop into a client's wl_shm buffer */
void screencopy_init(void);

/* main.c: start a program of the session (flfiles, flterm, fllock) */
void run_program(const char *prog);

/* seat.c */
void seat_init(void);
void seat_motion(uint32_t ms, double dx, double dy);
void seat_motion_abs(uint32_t ms, double x, double y);
void seat_button(uint32_t ms, uint32_t button, bool pressed);
void seat_axis(uint32_t ms, int axis, double value, int discrete, int source);
void seat_key(uint32_t ms, uint32_t key, bool pressed);
void seat_set_ptr_focus(struct surface *s, double sx, double sy);
void seat_set_kb_focus(struct surface *s);
void seat_refocus(void);
void seat_surface_gone(struct surface *s);
void seat_apply_config(void);	/* key repeat, Num Lock from CF */
bool seat_set_layout(const char *name);
const char *seat_layout(void);
void cursor_damage(void);
bool cursor_rect(struct rect *r);
void cursor_draw(struct rect clip);

/* flshell.c */
void flshell_init(void);
void fl_emit_added(struct window *w);
void fl_emit_changed(struct window *w);
void fl_emit_removed(struct window *w);
void fl_emit_menu_dismissed(void);
void fl_emit_menu_toggle(void);
void fl_emit_flyout_dismissed(void);
void fl_emit_volume_key(uint32_t action);
void fl_emit_layout(void);
void fl_emit_lock_state(void);
void fl_emit_output(void);	/* modes and state to every binding of version 3 */
void fl_emit_workspaces(void);	/* version 5: the grid and the current one */
void fl_emit_window_ws(struct window *w);	/* version 5 */

/* anim.c: short eased motion for windows, the workspace switch, the overview and the shell's popups.
 * All of it is driven by frame_done (the page flip), so nothing runs and nothing wakes while idle. */
enum { ANIM_OFF, ANIM_REDUCED, ANIM_ON };
void anim_init(void);
bool anim_active(void);				/* something is moving: the next frame is wanted at once */
int anim_gap_ms(void);				/* minimum time between frames while moving (headless has no vblank) */
void anim_frame(void);				/* a frame reached the screen: advance, damage what moves, ask for the next */
void anim_conf_changed(void);			/* animations=, animation_speed= */
/* hooks from the window code; each is a no-op when animations are off */
void anim_window_open(struct window *w);			/* it has just been mapped */
void anim_window_closing(struct window *w);			/* about to be destroyed: keep a picture and fade it out */
void anim_window_gone(struct window *w);			/* the struct is going away: forget it */
void anim_window_minimize(struct window *w);			/* just minimized (still has pixels) */
void anim_window_restore(struct window *w);			/* just restored from the panel */
void anim_window_geometry(struct window *w, struct rect from);	/* it moved or resized to the place w now has (maximize, snap, restore) */
void anim_ws_switch(int from, int to);				/* the workspace in view changed */
void anim_overview(bool opening);				/* the overview is opening or closing (it draws itself at the end state) */
void anim_surface_show(struct surface *s);			/* menu, flyout, notification surface shown */
void anim_surface_hide(struct surface *s);			/* ... about to be hidden: keep a picture */
void anim_surface_gone(struct surface *s);
/* the compositing side (render.c) */
struct anim_xf { struct rect dst; int alpha; };			/* where a window's geometry goes and how opaque (0..255) */
bool anim_window_xf(struct window *w, struct anim_xf *xf);	/* the window is animating: draw it this way */
bool anim_window_drawn_hidden(struct window *w);		/* not window_visible, but still drawn (minimizing, sliding out) */
bool anim_draw_window(struct window *w, struct rect clip);	/* draws it through its animation; false: no animation */
void anim_draw_win_ghosts(struct rect clip);			/* the last picture of windows that were closed, fading */
bool anim_draw_overview(struct rect clip);			/* the zooming overview; false: draw it the plain way */
bool anim_overview_busy(void);
bool anim_draw_surface(struct surface *s, struct rect clip);	/* menu, flyout, cards while they come in; false: plain */
void anim_draw_surf_ghosts(struct rect clip);			/* ... and while they leave */
bool anim_ws_active(void);
void anim_draw_begin(void);					/* FLCOMP_ANIM_LOG: the cost of a frame that has motion in it */
void anim_draw_end(void);
void overview_draw_snapshot(struct rect clip);			/* the overview as it is now, open or not (anim.c draws it off screen) */

/* render.c */
void damage_add(struct rect r);
void damage_all(void);
void wall_init(void);

/* theme.c: theme=, accent= and wallpaper= from desktop.conf, watched. PAL is the colour scheme
 * (flpalette.h: the same tokens as libflui's apps); everything the compositor draws takes its
 * colours from it, and a change repaints the screen. P(x) is a token as 0xRRGGBB. */
extern struct flpal PAL;
#define P(x) (PAL.x & 0xffffffu)
void theme_init(void);
uint32_t theme_accent(void);		/* 0xRRGGBB */
const char *theme_wallpaper(void);	/* .flw file to show */
bool theme_sounds(void);		/* sounds=1 (default) */

/* sound.c: system sounds through the flsound program (login, notification, error, tick, screenshot) */
void sound_play(const char *name);
void schedule_repaint(void);
void frame_done(void);
void composite_all(void);
/* The desktop in C.shadow without the pointer, the switcher and the indicator (the overview stays), and without
 * the windows of the client skip (NULL: all of them): for a capture. composite_restore puts everything back. */
void composite_clean_for(int skip_pid);
void composite_restore(void);
int write_screenshot(const char *path);

/* dmabuf.c, gl.c: buffers from GPU clients, copied into the surface at commit */
struct dbuf {
	int fd;			/* plane 0 (all there is for a linear buffer) */
	int w, h;
	uint32_t format, offset, stride;
	uint64_t modifier;
	int nplanes;		/* more than one: compression metadata (AMD DCC), GPU import only */
	struct { int fd; uint32_t offset, stride; } extra[3];	/* planes 1..3 */
};
void dmabuf_init(void);
struct dbuf *dbuf_get(struct wl_resource *buf);
bool dbuf_has_alpha(const struct dbuf *b);
bool dbuf_read(struct dbuf *b, uint32_t *dst, struct rect dmg);
void gl_start(void);
bool gl_usable(void);
int gl_modifiers(uint32_t format, uint64_t *mods, int max);
bool gl_modifier_ok(uint32_t format, uint64_t modifier, int nplanes);
bool gl_read(struct dbuf *b, uint32_t *dst, struct rect dmg);

/* config.c: ~/.config/freelfs/desktop.conf, read at start and when it changes */
struct conf {
	int pointer_speed;		/* -100..100 */
	bool accel_flat, mouse_natural, touchpad_natural, tap, typing_disable, left_handed;
	int repeat_delay, repeat_rate;	/* ms, keys per second */
	bool numlock;
	bool night;
	int night_warmth;		/* 0..100 (old keys night_light, night_warmth: used when nightlight= is absent) */
	int nl_mode;			/* nightlight=off|on|auto: 0, 1, 2; -1: key absent (wins over night_light) */
	int nl_old;			/* night_light=0|1|auto, the older key: 0, 1, 2; -1: absent */
	int nl_temp;			/* nightlight_temp=2700..6500, 0: absent */
	int nl_from, nl_to;		/* nightlight_from/_to as minutes after midnight (20:00, 07:00) */
	int brightness;			/* percent, -1: leave alone */
	int mode_w, mode_h, mode_hz;	/* mode=WxH@HZ, 0: the display's preferred */
	int scale;
	bool power_saver;		/* power_saver=1: frame cap and dimmed backlight (power.c) */
	bool lock_on_lid;		/* lock_on_lid=1 (default): closing the lid locks the screen */
	int ws_cols, ws_rows;		/* workspaces=COLSxROWS, default 2x2 */
	int perf;			/* performance=auto|on|off (PERF_*, perf.c); 0 is auto */
	int anim;			/* animations=on|reduced|off (ANIM_*, anim.c) */
	int anim_speed;			/* animation_speed= in percent: 100 is normal, 200 twice as fast */
	bool anim_log;			/* anim_log=1: log the frames of every burst of motion (same as FLCOMP_ANIM_LOG) */
	bool panel_top;			/* panel_position=top: the panel is at the top edge */
	int title_h;			/* titlebar=normal|compact: 34 or 28 (the frame of X windows) */
	bool buttons_left;		/* buttons=left: window buttons at the left (X window frames) */
	int panel_h;			/* panel_height= (the panel itself also sends set_panel), 44 */
};
extern struct conf CF;
void config_init(void);
void config_apply_start(void);
int config_set(const char *key, const char *val);	/* rewrite one key of desktop.conf; 0 or -1 */	/* after the backend and the seat exist */

/* media.c, osd.c: volume, brightness and microphone keys and the one on-screen indicator */
enum { OSD_VOLUME, OSD_BRIGHTNESS, OSD_MIC, OSD_SHOT };	/* OSD_SHOT: the screenshot was saved (muted: it was not) */
enum { MEDIA_VOL_UP, MEDIA_VOL_DOWN, MEDIA_MUTE, MEDIA_BRIGHT_UP, MEDIA_BRIGHT_DOWN, MEDIA_MIC,
       MEDIA_PLAY, MEDIA_STOP, MEDIA_NEXT, MEDIA_PREV };
int media_action(uint32_t sym);			/* -1: not a media key */
void backlight_saver(bool on);			/* power saving: 70 % of the brightness, and back */
void media_key(int action, bool pressed);	/* repeats for the keys that step */
void osd_show(int kind, int percent, bool muted);
void osd_draw(struct rect clip);

/* power.c: power-saving mode, from desktop.conf power_saver= */
void power_apply(bool first);			/* CF.power_saver changed: backlight, frame cap */
int power_frame_gap_ms(void);			/* minimum time between two frames, 0: none */

/* perf.c: performance mode, from desktop.conf performance= (auto: while a fullscreen window is on top) */
enum { PERF_AUTO, PERF_ON, PERF_OFF };
int perf_parse(const char *v);			/* "on", "off", anything else is auto */
bool perf_active(void);				/* the frame cap is lifted, see perf.c */
struct window *perf_unredirected(void);		/* the fullscreen window that alone is drawn, or NULL */

/* output.c: modes, scale, the 15 second confirm */
void output_init(void);
bool output_set(int w, int h, int hz, int scale);	/* w = 0: keep the mode */
void output_confirm(void);
void output_watch_client(struct wl_client *c);
void output_revert(void);
void output_send_modes(struct wl_resource *shell);	/* a shell binding: all modes, then the state */
void output_restart_panel(void);			/* main.c */
void wall_reload(void);
void windows_fit_output(void);				/* xdg.c */
void outputs_send_mode(void);				/* main.c: wl_output after a change */
int output_pending_secs(void);
/* fl_shell 9 (fldisplay): flags 1 mode, 2 scale, 4 position, 8 enabled, 16 primary */
bool output_configure(const char *name, unsigned flags, int w, int h, int hz, int scale, int x, int y, int enabled, int primary);
bool output_revert_config(void);
void output_send_list(struct wl_resource *shell);	/* version 9 bindings: every output and its modes */
bool output_scale_ok(int pw, int ph, int scale);

/* drm.c: backend (also the headless one) */
struct outmode { int w, h, hz; bool preferred; };
#define MAX_OUT 8
/* One display output (a connector). The backend fills the connector facts, output.c the policy
 * (scale, position, primary) and the layout: dx, dy, lw, lh say where its picture is in the
 * shadow. Positions are logical pixels (mode size * 100 / scale). */
struct output {
	char name[32];
	bool connected, enabled, primary;
	int w, h, hz;			/* current mode in pixels, the last one while the output is off */
	int scale;			/* percent */
	int x, y;			/* configured position on the desktop */
	int dx, dy, lw, lh;		/* in the shadow, and the logical size (enabled outputs only) */
	struct outmode modes[64];	/* no duplicates, preferred first */
	int nmodes;
};
extern struct output OUT[MAX_OUT];
extern int NOUT;
int backend_init(void);
void backend_start(void);
void backend_present(const struct damage *d);
bool backend_busy(void);
void backend_switch_vt(int n);
void backend_fini(void);
int backend_drm_fd(void);	/* -1: headless */
bool backend_probe(void);	/* (re)read the connectors into OUT; true: something changed */
bool backend_out_set(struct output *o, bool enable, int w, int h, int hz);	/* false: refused, nothing changed */
void backend_tint(const double rgb[3]);		/* night light: colour factors 0..1 on every output */
void backend_input_apply(void);			/* libinput settings from CF, every device */
void backend_set_leds(int leds);			/* 1 num, 2 caps, 4 scroll */
void output_hotplug(void);				/* output.c: the connectors changed (backend_probe said so) */
bool output_ready(void);				/* output.c: layout done */
/* nightlight.c */
void night_init(void);
void night_conf_changed(void);
void night_outputs_changed(void);
const char *config_get_raw(const char *key, char *buf, size_t n);	/* config.c: value of KEY in desktop.conf, or NULL */

#include "xwm.h"

#endif
