/* xwm.c -- Xwayland for flcomp: the X socket, Xwayland on demand, xwayland_shell_v1, and the X11
 * window manager (map, configure, stack, focus, titles, EWMH state).
 *
 * flcomp listens on /tmp/.X11-unix/X<N> from the start and exports DISPLAY=:N. Xwayland is not
 * started until a client connects (the listening socket is handed to it with -listenfd) and
 * leaves again when the last X client is gone (idle_check: Xwayland keeps running while its window
 * manager is connected, so flcomp ends it): an idle desktop never carries an X server. Xwayland talks to flcomp as an ordinary Wayland client over a socketpair
 * (so flcomp knows which client may bind xwayland_shell_v1), and flcomp talks to Xwayland as the
 * window manager over a second socketpair (-wm), with xcb.
 *
 * X windows are matched with their wl_surface by the WL_SURFACE_SERIAL client message and
 * xwayland_surface_v1.set_serial. A managed X window becomes a struct window (so workspaces,
 * Alt-Tab, the overview, the panel, General Manager and performance mode treat it like any other
 * window); override-redirect windows and menus/tooltips are "unmanaged": drawn above the
 * managed windows at the place the X client chose.
 * SPDX-License-Identifier: MIT */
#include "xwm-int.h"
#include "xwayland-shell-protocol.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

struct xwm X;

/* FLCOMP_XDEBUG=1: what the window manager sees, on stderr */
static int xdbg = -1;
#define XDBG(...) do { if (xdbg < 0) xdbg = getenv("FLCOMP_XDEBUG") != NULL; if (xdbg) { fprintf(stderr, "xwm: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

void window_destroy_for_surface(struct surface *s);
void window_commit_xw(struct window *w, bool was_mapped);
bool window_skip_panel(struct window *w);

/* window type bits */
enum {
	T_NORMAL = 1, T_DIALOG = 2, T_UTILITY = 4, T_SPLASH = 8, T_DOCK = 16, T_MENU = 32,
	T_TOOLTIP = 64, T_NOTIF = 128, T_DND = 256, T_DESKTOP = 512,
};
#define T_UNMANAGED (T_MENU | T_TOOLTIP | T_NOTIF | T_DND | T_DOCK)

enum { P_TITLE, P_NETNAME, P_CLASS, P_TRANSIENT, P_NORMAL, P_HINTS, P_PROTOCOLS, P_TYPE, P_MOTIF,
       P_NSTATE, P_END };

#define XSOCK_DIR "/tmp/.X11-unix"

/* ---- state ------------------------------------------------------------------------- */

static int disp_num = -1;
static char sock_path[64];
static char disp_name[16];
static int listen_fd = -1;
static struct wl_event_source *listen_src;
static const char *xwl_bin;

static pid_t xpid;			/* Xwayland while it runs */
static int wm_fd = -1, disp_fd = -1;
static struct wl_client *xclient;
static struct wl_listener xclient_listener;
static struct wl_event_source *disp_src, *x_src;
static struct wl_global *shell_global;
static bool ready;			/* the window manager is connected */
static struct xwin *xfocus;		/* the X window that got the input focus from us */

struct preq {
	unsigned int seq;
	int kind;
	xcb_window_t win;
	struct preq *next;
};
static struct preq *pq_head, *pq_tail;

static void listen_arm(void);
static void idle_check(void);
static struct wl_event_source *idle_timer;

bool xwm_running(void) { return ready; }
const char *xwm_display(void) { return disp_num >= 0 ? disp_name : NULL; }

/* ---- the X socket ---------------------------------------------------------------------- */

static bool display_free(int n)
{
	char lock[64], path[96];
	snprintf(lock, sizeof lock, "/tmp/.X%d-lock", n);
	FILE *f = fopen(lock, "r");
	if (f) {
		int pid = 0;
		if (fscanf(f, "%d", &pid) != 1)
			pid = 0;
		fclose(f);
		if (pid > 0 && (kill(pid, 0) == 0 || errno == EPERM))
			return false;
	}
	/* somebody listening on the file or on the abstract name? */
	snprintf(path, sizeof path, "%s/X%d", XSOCK_DIR, n);
	for (int abs = 0; abs < 2; abs++) {
		int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd < 0)
			continue;
		struct sockaddr_un sa = { .sun_family = AF_UNIX };
		socklen_t len;
		if (abs) {
			sa.sun_path[0] = 0;
			snprintf(sa.sun_path + 1, sizeof sa.sun_path - 1, "%s", path);
			len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + strlen(path));
		} else {
			snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
			len = (socklen_t)sizeof sa;
		}
		int r = connect(fd, (struct sockaddr *)&sa, len);
		close(fd);
		if (r == 0)
			return false;
	}
	return true;
}

static int listen_bind(int n)
{
	if (mkdir(XSOCK_DIR, 01777) == 0)
		chmod(XSOCK_DIR, 01777);
	snprintf(sock_path, sizeof sock_path, "%s/X%d", XSOCK_DIR, n);
	unlink(sock_path);
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return -1;
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock_path);
	mode_t old = umask(0177);
	int r = bind(fd, (struct sockaddr *)&sa, sizeof sa);
	umask(old);
	if (r < 0 || listen(fd, 8) < 0) {
		close(fd);
		return -1;
	}
	chmod(sock_path, 0700);		/* only this user: there is no X authority file */
	return fd;
}

static int listen_cb(int fd, uint32_t mask, void *data);
static void xwl_start(void);

static void listen_arm(void)
{
	if (listen_fd < 0) {
		listen_fd = listen_bind(disp_num);
		if (listen_fd < 0) {
			fprintf(stderr, "flcomp: xwayland: cannot listen on %s: %s\n", sock_path, strerror(errno));
			return;
		}
	}
	if (!listen_src)
		listen_src = wl_event_loop_add_fd(C.loop, listen_fd, WL_EVENT_READABLE, listen_cb, NULL);
}

static int listen_cb(int fd, uint32_t mask, void *data)
{
	if (listen_src) {
		wl_event_source_remove(listen_src);
		listen_src = NULL;
	}
	xwl_start();
	return 0;
}

/* ---- the shell global ------------------------------------------------------------------- */

static void xsurf_destroy_req(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static void xsurf_set_serial(struct wl_client *c, struct wl_resource *r, uint32_t lo, uint32_t hi)
{
	struct surface *s = wl_resource_get_user_data(r);
	if (!s)
		return;
	uint64_t v = (uint64_t)hi << 32 | lo;
	XDBG("surface set_serial %llu", (unsigned long long)v);
	if (v == 0) {
		wl_resource_post_error(r, XWAYLAND_SURFACE_V1_ERROR_INVALID_SERIAL, "serial is zero");
		return;
	}
	if (s->xs_ser || s->xs_has_pend) {
		wl_resource_post_error(r, XWAYLAND_SURFACE_V1_ERROR_ALREADY_ASSOCIATED, "already associated");
		return;
	}
	s->xs_pend = v;
	s->xs_has_pend = true;
}

static const struct xwayland_surface_v1_interface xsurf_impl = {
	.set_serial = xsurf_set_serial, .destroy = xsurf_destroy_req,
};

static void xsurf_res_destroy(struct wl_resource *r)
{
	struct surface *s = wl_resource_get_user_data(r);
	if (s)
		s->xs_res = NULL;
}

static void xshell_destroy_req(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static void xshell_get_surface(struct wl_client *c, struct wl_resource *r, uint32_t id,
			       struct wl_resource *surf_res)
{
	struct surface *s = wl_resource_get_user_data(surf_res);
	if (s->role != ROLE_NONE && s->role != ROLE_XWL) {
		wl_resource_post_error(r, XWAYLAND_SHELL_V1_ERROR_ROLE, "surface has another role");
		return;
	}
	struct wl_resource *res = wl_resource_create(c, &xwayland_surface_v1_interface,
						     wl_resource_get_version(r), id);
	if (!res) {
		wl_client_post_no_memory(c);
		return;
	}
	s->role = ROLE_XWL;
	s->xs_res = res;
	wl_resource_set_implementation(res, &xsurf_impl, s, xsurf_res_destroy);
}

static const struct xwayland_shell_v1_interface xshell_impl = {
	.destroy = xshell_destroy_req, .get_xwayland_surface = xshell_get_surface,
};

static void xshell_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &xwayland_shell_v1_interface, version, id);
	if (!r) {
		wl_client_post_no_memory(c);
		return;
	}
	if (c != xclient) {	/* the filter hides the global from everybody else; this is a client that guessed */
		wl_resource_post_error(r, 0, "only Xwayland may bind xwayland_shell_v1");
		return;
	}
	wl_resource_set_implementation(r, &xshell_impl, NULL, NULL);
}

static bool shell_filter(const struct wl_client *c, const struct wl_global *g, void *data)
{
	if (wl_global_get_interface(g) == &xwayland_shell_v1_interface)
		return c == xclient;
	return true;
}

/* ---- surface association ----------------------------------------------------------------- */

struct xwin *xwin_by_id(xcb_window_t id)
{
	struct xwin *xw;
	wl_list_for_each(xw, &X.all, link)
		if (xw->id == id)
			return xw;
	return NULL;
}

static void xw_surface_ready(struct xwin *xw);

static void xw_link(struct xwin *xw, struct surface *s)
{
	xw->surf = s;
	s->xw = xw;
	xw_surface_ready(xw);
}

static void match_window(struct xwin *xw)
{
	if (!xw->serial || xw->surf)
		return;
	struct surface *s;
	wl_list_for_each(s, &C.surfaces, link)
		if (s->role == ROLE_XWL && s->xs_ser == xw->serial && !s->xw) {
			xw_link(xw, s);
			return;
		}
}

static void match_surface(struct surface *s)
{
	if (!s->xs_ser || s->xw)
		return;
	struct xwin *xw;
	wl_list_for_each(xw, &X.all, link)
		if (xw->serial == s->xs_ser && !xw->surf) {
			xw_link(xw, s);
			return;
		}
}

void xwm_surface_origin(struct surface *s, int *x, int *y)
{
	*x = *y = 0;
	if (!s->xw)
		return;
	if (s->xw->win) {
		*x = s->xw->win->x;
		*y = s->xw->win->y;
	} else {
		*x = s->xw->x;
		*y = s->xw->y;
	}
}

void xwm_surface_gone(struct surface *s)
{
	if (s->xs_res)
		wl_resource_set_user_data(s->xs_res, NULL);
	s->xs_res = NULL;
	if (s->xw) {
		struct xwin *xw = s->xw;
		xw->surf = NULL;
		xw->win = NULL;
		xw->serial = 0;
		s->xw = NULL;
	}
}

/* ---- properties --------------------------------------------------------------------------- */

static void preq_add(unsigned int seq, int kind, xcb_window_t win)
{
	struct preq *p = calloc(1, sizeof *p);
	if (!p)
		return;
	p->seq = seq;
	p->kind = kind;
	p->win = win;
	if (pq_tail)
		pq_tail->next = p;
	else
		pq_head = p;
	pq_tail = p;
}

static void prop_request(struct xwin *xw, int kind, xcb_atom_t prop, uint32_t len)
{
	xcb_get_property_cookie_t ck = xcb_get_property(X.c, 0, xw->id, prop, XCB_ATOM_ANY, 0, len);
	preq_add(ck.sequence, kind, xw->id);
	xw->npend++;
}

static void props_request_all(struct xwin *xw)
{
	prop_request(xw, P_TITLE, X.a.WM_NAME, 256);
	prop_request(xw, P_NETNAME, X.a._NET_WM_NAME, 256);
	prop_request(xw, P_CLASS, X.a.WM_CLASS, 64);
	prop_request(xw, P_TRANSIENT, X.a.WM_TRANSIENT_FOR, 1);
	prop_request(xw, P_NORMAL, X.a.WM_NORMAL_HINTS, 18);
	prop_request(xw, P_HINTS, X.a.WM_HINTS, 9);
	prop_request(xw, P_PROTOCOLS, X.a.WM_PROTOCOLS, 32);
	prop_request(xw, P_TYPE, X.a._NET_WM_WINDOW_TYPE, 32);
	prop_request(xw, P_MOTIF, X.a._MOTIF_WM_HINTS, 5);
	prop_request(xw, P_NSTATE, X.a._NET_WM_STATE, 32);
	prop_request(xw, P_END, X.a._NET_WM_PID, 1);
}

static char *dupn(const void *p, int n)
{
	char *s = malloc((size_t)n + 1);
	if (!s)
		return NULL;
	memcpy(s, p, (size_t)n);
	s[n] = 0;
	return s;
}

/* A title from WM_NAME is Latin-1; widen what is not ASCII so it stays one valid UTF-8 string. */
static char *latin1_to_utf8(const unsigned char *p, int n)
{
	char *s = malloc((size_t)n * 2 + 1), *o = s;
	if (!s)
		return NULL;
	for (int i = 0; i < n; i++) {
		if (p[i] < 0x80) {
			*o++ = (char)p[i];
		} else {
			*o++ = (char)(0xc0 | p[i] >> 6);
			*o++ = (char)(0x80 | (p[i] & 0x3f));
		}
	}
	*o = 0;
	return s;
}

static void names_update(struct xwin *xw)
{
	struct window *w = xw->win;
	if (!w)
		return;
	const char *t = xw->net_title ? xw->net_title : xw->title ? xw->title : "";
	const char *k = xw->klass && *xw->klass ? xw->klass : xw->instance ? xw->instance : "";
	bool chg = false;
	if (!w->title || strcmp(w->title, t)) {
		char *n = strdup(t);
		if (n) {
			free(w->title);
			w->title = n;
			chg = true;
		}
	}
	if (!w->app_id || strcmp(w->app_id, k)) {
		char *n = strdup(k);
		if (n) {
			free(w->app_id);
			w->app_id = n;
		}
	}
	if (chg) {
		if (w->announced)
			fl_emit_changed(w);
		xwm_deco_damage(w);
	}
}

static void write_wm_state(struct xwin *xw, int state)
{
	if (xw->wm_state == state)
		return;
	xw->wm_state = state;
	uint32_t v[2] = { (uint32_t)state, XCB_NONE };
	xcb_change_property(X.c, XCB_PROP_MODE_REPLACE, xw->id, X.a.WM_STATE, X.a.WM_STATE, 32, 2, v);
}

static void write_extents(struct xwin *xw)
{
	struct xinset in = xw->win ? xwm_insets(xw->win) : (struct xinset){ 0 };
	if (!xw->deco && !xw->win)
		return;
	uint32_t v[4] = { (uint32_t)in.l, (uint32_t)in.r, (uint32_t)in.t, (uint32_t)in.b };
	xcb_change_property(X.c, XCB_PROP_MODE_REPLACE, xw->id, X.a._NET_FRAME_EXTENTS, XCB_ATOM_CARDINAL, 32, 4, v);
}

static void prop_apply(struct xwin *xw, int kind, xcb_get_property_reply_t *r)
{
	int n = r ? xcb_get_property_value_length(r) : 0;
	const void *v = r ? xcb_get_property_value(r) : NULL;
	bool have = r && r->type != XCB_NONE && n > 0;
	const uint32_t *u = v;
	int nl = r && r->format == 32 ? n / 4 : 0;
	switch (kind) {
	case P_TITLE:
		free(xw->title);
		xw->title = !have ? NULL : r->type == X.a.UTF8_STRING ? dupn(v, n) : latin1_to_utf8(v, n);
		names_update(xw);
		break;
	case P_NETNAME:
		free(xw->net_title);
		xw->net_title = have ? dupn(v, n) : NULL;
		names_update(xw);
		break;
	case P_CLASS:
		free(xw->instance);
		free(xw->klass);
		xw->instance = xw->klass = NULL;
		if (have) {
			const char *s = v;
			size_t l0 = strnlen(s, (size_t)n);
			xw->instance = dupn(s, (int)l0);
			if ((int)l0 + 1 < n)
				xw->klass = dupn(s + l0 + 1, (int)strnlen(s + l0 + 1, (size_t)n - l0 - 1));
			for (char *p = xw->klass; p && *p; p++)	/* app id: "Steam" -> "steam" */
				*p = (char)tolower((unsigned char)*p);
		}
		names_update(xw);
		break;
	case P_TRANSIENT:
		xw->transient_for = nl >= 1 ? u[0] : XCB_NONE;
		break;
	case P_NORMAL:
		xw->min_w = xw->min_h = xw->max_w = xw->max_h = xw->inc_w = xw->inc_h = xw->base_w = xw->base_h = 0;
		if (nl >= 18) {
			uint32_t f = u[0];
			if (f & 16) { xw->min_w = (int)u[5]; xw->min_h = (int)u[6]; }
			if (f & 32) { xw->max_w = (int)u[7]; xw->max_h = (int)u[8]; }
			if (f & 64) { xw->inc_w = (int)u[9]; xw->inc_h = (int)u[10]; }
			if (f & 256) { xw->base_w = (int)u[15]; xw->base_h = (int)u[16]; }
			else if (f & 16) { xw->base_w = xw->min_w; xw->base_h = xw->min_h; }
		}
		if (xw->win) {
			xw->win->min_w = xw->min_w;
			xw->win->min_h = xw->min_h;
			xw->win->max_w = xw->max_w;
			xw->win->max_h = xw->max_h;
		}
		break;
	case P_HINTS:
		xw->has_input_hint = false;
		if (nl >= 2 && (u[0] & 1)) {
			xw->has_input_hint = true;
			xw->input_hint = u[1] != 0;
		}
		break;
	case P_PROTOCOLS:
		xw->take_focus = xw->delete_win = false;
		for (int i = 0; i < nl; i++) {
			if (u[i] == X.a.WM_TAKE_FOCUS)
				xw->take_focus = true;
			else if (u[i] == X.a.WM_DELETE_WINDOW)
				xw->delete_win = true;
		}
		break;
	case P_TYPE:
		xw->types = 0;
		for (int i = 0; i < nl; i++) {
			xcb_atom_t a = u[i];
			if (a == X.a._NET_WM_WINDOW_TYPE_NORMAL) xw->types |= T_NORMAL;
			else if (a == X.a._NET_WM_WINDOW_TYPE_DIALOG) xw->types |= T_DIALOG;
			else if (a == X.a._NET_WM_WINDOW_TYPE_UTILITY) xw->types |= T_UTILITY;
			else if (a == X.a._NET_WM_WINDOW_TYPE_SPLASH) xw->types |= T_SPLASH;
			else if (a == X.a._NET_WM_WINDOW_TYPE_DOCK) xw->types |= T_DOCK;
			else if (a == X.a._NET_WM_WINDOW_TYPE_MENU || a == X.a._NET_WM_WINDOW_TYPE_DROPDOWN_MENU ||
				 a == X.a._NET_WM_WINDOW_TYPE_POPUP_MENU || a == X.a._NET_WM_WINDOW_TYPE_COMBO)
				xw->types |= T_MENU;
			else if (a == X.a._NET_WM_WINDOW_TYPE_TOOLTIP) xw->types |= T_TOOLTIP;
			else if (a == X.a._NET_WM_WINDOW_TYPE_NOTIFICATION) xw->types |= T_NOTIF;
			else if (a == X.a._NET_WM_WINDOW_TYPE_DND) xw->types |= T_DND;
			else if (a == X.a._NET_WM_WINDOW_TYPE_DESKTOP) xw->types |= T_DESKTOP;
		}
		break;
	case P_MOTIF:
		xw->nodeco_hint = false;
		if (nl >= 3 && (u[0] & 2) && u[2] == 0)	/* MWM_HINTS_DECORATIONS and none wanted */
			xw->nodeco_hint = true;
		break;
	case P_NSTATE:
		xw->want_full = xw->want_max = xw->skip_taskbar = false;
		{
			bool vert = false, horz = false;
			for (int i = 0; i < nl; i++) {
				if (u[i] == X.a._NET_WM_STATE_FULLSCREEN) xw->want_full = true;
				else if (u[i] == X.a._NET_WM_STATE_MAXIMIZED_VERT) vert = true;
				else if (u[i] == X.a._NET_WM_STATE_MAXIMIZED_HORZ) horz = true;
				else if (u[i] == X.a._NET_WM_STATE_SKIP_TASKBAR) xw->skip_taskbar = true;
			}
			xw->want_max = vert && horz;
		}
		break;
	default:
		break;
	}
}

/* decide what the window is and map it (the properties are in) */
static void do_map(struct xwin *xw)
{
	xw->map_wanted = false;
	xw->typed = true;
	XDBG("map 0x%x override=%d types=0x%x", xw->id, xw->override, xw->types);
	xw->unmanaged = xw->override || (xw->types & T_UNMANAGED);
	xw->deco = !xw->unmanaged && !xw->nodeco_hint && !(xw->types & T_SPLASH);
	if (!xw->unmanaged) {
		uint32_t bw = 0, em = XCB_EVENT_MASK_PROPERTY_CHANGE;	/* titles and hints can change later */
		xcb_change_window_attributes(X.c, xw->id, XCB_CW_EVENT_MASK, &em);
		xcb_configure_window(X.c, xw->id, XCB_CONFIG_WINDOW_BORDER_WIDTH, &bw);
		struct xinset in = xw->deco ? (struct xinset){ XD_BORDER, XD_BORDER, XD_TITLE, XD_BORDER } : (struct xinset){ 0 };
		uint32_t v[4] = { (uint32_t)in.l, (uint32_t)in.r, (uint32_t)in.t, (uint32_t)in.b };
		xcb_change_property(X.c, XCB_PROP_MODE_REPLACE, xw->id, X.a._NET_FRAME_EXTENTS,
				    XCB_ATOM_CARDINAL, 32, 4, v);
	}
	xcb_map_window(X.c, xw->id);
	xw->mapped_x = true;
	write_wm_state(xw, 1);
	xwm_flush();
}

static void preq_done(struct preq *p, xcb_get_property_reply_t *r)
{
	struct xwin *xw = xwin_by_id(p->win);
	if (!xw)
		return;
	xw->npend--;
	prop_apply(xw, p->kind, r);
	if (p->kind == P_END && xw->map_wanted && xw->npend == 0)
		do_map(xw);
	else if (p->kind != P_END && xw->win) {
		/* a property changed on a window that is up: what depends on it follows */
		if (p->kind == P_TYPE || p->kind == P_MOTIF)
			write_extents(xw);
	}
}

static void pending_replies(void)
{
	while (pq_head) {
		struct preq *p = pq_head;
		xcb_get_property_reply_t *r = NULL;
		xcb_generic_error_t *err = NULL;
		xcb_get_property_cookie_t ck = { p->seq };
		if (!xcb_poll_for_reply(X.c, ck.sequence, (void **)&r, &err))
			break;
		pq_head = p->next;
		if (!pq_head)
			pq_tail = NULL;
		free(err);
		preq_done(p, r);
		free(r);
		free(p);
	}
}

static void pq_clear(void)
{
	while (pq_head) {
		struct preq *p = pq_head;
		pq_head = p->next;
		free(p);
	}
	pq_tail = NULL;
}

/* ---- windows ------------------------------------------------------------------------------ */

void xwm_flush(void)
{
	if (X.c)
		xcb_flush(X.c);
}

static void xwin_free(struct xwin *xw)
{
	if (xw == xfocus)
		xfocus = NULL;
	if (xw->surf) {
		if (xw->win)
			window_destroy_for_surface(xw->surf);	/* the window goes now, the surface a moment later */
		xw->surf->xw = NULL;
	}
	wl_list_remove(&xw->link);
	free(xw->title);
	free(xw->net_title);
	free(xw->klass);
	free(xw->instance);
	free(xw);
}

static struct xwin *xwin_new(xcb_window_t id, bool override, int x, int y, int w, int h)
{
	struct xwin *xw = calloc(1, sizeof *xw);
	if (!xw)
		return NULL;
	xw->id = id;
	xw->override = xw->unmanaged = override;
	xw->x = x;
	xw->y = y;
	xw->w = w;
	xw->h = h;
	xw->ws_seen = -1;
	wl_list_insert(X.all.prev, &xw->link);
	return xw;
}

static struct window *xw_make_window(struct xwin *xw)
{
	struct window *w = calloc(1, sizeof *w);
	if (!w)
		return NULL;
	w->id = ++C.next_id;
	w->surf = xw->surf;
	w->xw = xw;
	w->title = strdup("");
	w->app_id = strdup("");
	w->min_w = xw->min_w;
	w->min_h = xw->min_h;
	w->max_w = xw->max_w;
	w->max_h = xw->max_h;
	if (xw->transient_for) {
		struct xwin *p = xwin_by_id(xw->transient_for);
		if (p && p->win)
			w->parent = p->win;
	}
	xw->surf->win = w;
	xw->win = w;
	wl_list_insert(&C.windows, &w->link);
	names_update(xw);
	return w;
}

/* The wl_surface of an X window got its serial: show it when it has a buffer. */
static void xw_surface_ready(struct xwin *xw)
{
	if (xw->surf && xw->surf->px)
		xwm_surface_commit(xw->surf, false);
}

void xwm_surface_commit(struct surface *s, bool was_mapped)
{
	if (s->xs_has_pend) {
		s->xs_ser = s->xs_pend;
		s->xs_has_pend = false;
		match_surface(s);
	}
	struct xwin *xw = s->xw;
	XDBG("surface commit px=%d serial=%llu window=0x%x", s->px != NULL, (unsigned long long)s->xs_ser, xw ? xw->id : 0);
	if (!xw)
		return;
	if (xw->unmanaged) {
		if (s->px && xw->ws_seen < 0)
			xw->ws_seen = C.ws_cur;
		else if (!s->px)
			xw->ws_seen = -1;
		return;
	}
	if (!xw->win) {
		if (!s->px)
			return;
		if (!xw_make_window(xw))
			return;
		window_commit_xw(xw->win, false);
		if (xw->win && (xw->want_full || xw->want_max))
			window_request_state(xw->win, xw->want_max ? 1 : -1, xw->want_full ? 1 : -1);
		if (xw->win)
			xwm_window_changed(xw->win);
		return;
	}
	window_commit_xw(xw->win, was_mapped);
}

void xwm_window_gone(struct window *w)
{
	xwm_deco_forget(w);
	if (w->xw) {
		w->xw->win = NULL;
		w->xw = NULL;
	}
}

bool window_skip_panel(struct window *w)
{
	return w->xw && w->xw->skip_taskbar;
}

static void send_client_message(xcb_window_t win, xcb_atom_t type, uint32_t d0, uint32_t d1)
{
	xcb_client_message_event_t ev = { 0 };
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.format = 32;
	ev.window = win;
	ev.type = type;
	ev.data.data32[0] = d0;
	ev.data.data32[1] = d1;
	xcb_send_event(X.c, 0, win, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
}

void xwm_window_close(struct window *w)
{
	struct xwin *xw = w->xw;
	if (!xw || !X.c)
		return;
	if (xw->delete_win)
		send_client_message(xw->id, X.a.WM_PROTOCOLS, X.a.WM_DELETE_WINDOW, XCB_CURRENT_TIME);
	else
		xcb_kill_client(X.c, xw->id);
	xwm_flush();
}

/* the size flcomp wants for the window: min, max and increments of the hints are respected */
void xwm_window_configure(struct window *w, int cw, int ch)
{
	struct xwin *xw = w->xw;
	if (!xw || !X.c || cw <= 0 || ch <= 0)
		return;
	if (xw->min_w > 0 && cw < xw->min_w) cw = xw->min_w;
	if (xw->min_h > 0 && ch < xw->min_h) ch = xw->min_h;
	if (xw->max_w > 0 && cw > xw->max_w) cw = xw->max_w;
	if (xw->max_h > 0 && ch > xw->max_h) ch = xw->max_h;
	if (!w->fullscreen && !w->maximized) {	/* terminals: whole cells (a maximized window may leave a margin) */
		if (xw->inc_w > 1) cw = xw->base_w + (cw - xw->base_w) / xw->inc_w * xw->inc_w;
		if (xw->inc_h > 1) ch = xw->base_h + (ch - xw->base_h) / xw->inc_h * xw->inc_h;
	}
	if (cw == xw->cw && ch == xw->ch && cw == xw->w && ch == xw->h)
		return;
	xw->cw = cw;
	xw->ch = ch;
	uint32_t v[4] = { (uint32_t)w->x, (uint32_t)w->y, (uint32_t)cw, (uint32_t)ch };
	xcb_configure_window(X.c, xw->id, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y |
			     XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, v);
	xw->sx = w->x;
	xw->sy = w->y;
	xw->sent_pos = true;
	xw->w = cw;		/* what the server will say */
	xw->h = ch;
	xwm_flush();
}

/* _NET_WM_STATE, WM_STATE and the frame extents follow the window */
void xwm_window_changed(struct window *w)
{
	struct xwin *xw = w->xw;
	if (!xw || !X.c)
		return;
	uint32_t st[6];
	int n = 0;
	if (w->fullscreen) st[n++] = X.a._NET_WM_STATE_FULLSCREEN;
	if (w->maximized) {
		st[n++] = X.a._NET_WM_STATE_MAXIMIZED_VERT;
		st[n++] = X.a._NET_WM_STATE_MAXIMIZED_HORZ;
	}
	if (w->minimized) st[n++] = X.a._NET_WM_STATE_HIDDEN;
	if (C.focused == w) st[n++] = X.a._NET_WM_STATE_FOCUSED;
	if (xw->skip_taskbar) st[n++] = X.a._NET_WM_STATE_SKIP_TASKBAR;
	uint32_t sig = 0;
	for (int i = 0; i < n; i++)
		sig = sig * 31 + st[i];
	sig ^= (uint32_t)n << 28;
	if (sig != xw->shown_state || !xw->shown_state) {
		xw->shown_state = sig ? sig : 1;
		xcb_change_property(X.c, XCB_PROP_MODE_REPLACE, xw->id, X.a._NET_WM_STATE, XCB_ATOM_ATOM, 32,
				    (uint32_t)n, st);
	}
	write_wm_state(xw, w->minimized ? 3 : 1);
	write_extents(xw);
	xwm_deco_damage(w);
	xwm_flush();
}

/* ---- focus -------------------------------------------------------------------------------- */

void xwm_focus_sync(void)
{
	if (!X.c || !ready)
		return;
	struct window *f = C.focused;
	struct xwin *want = NULL;
	if (f && f->xw && f->mapped && !f->minimized && !C.locked && f->xw->mapped_x)
		want = f->xw;
	if (want == xfocus)
		return;
	if (xfocus && xfocus->win)
		xwm_window_changed(xfocus->win);
	xfocus = want;
	if (want) {
		if (!want->has_input_hint || want->input_hint)
			xcb_set_input_focus(X.c, XCB_INPUT_FOCUS_POINTER_ROOT, want->id, XCB_CURRENT_TIME);
		if (want->take_focus)
			send_client_message(want->id, X.a.WM_PROTOCOLS, X.a.WM_TAKE_FOCUS, XCB_CURRENT_TIME);
		uint32_t id = want->id;
		xcb_change_property(X.c, XCB_PROP_MODE_REPLACE, X.root, X.a._NET_ACTIVE_WINDOW, XCB_ATOM_WINDOW, 32, 1, &id);
		xwm_window_changed(want->win);
	} else {
		xcb_set_input_focus(X.c, XCB_INPUT_FOCUS_NONE, XCB_NONE, XCB_CURRENT_TIME);
		uint32_t none = XCB_NONE;
		xcb_change_property(X.c, XCB_PROP_MODE_REPLACE, X.root, X.a._NET_ACTIVE_WINDOW, XCB_ATOM_WINDOW, 32, 1, &none);
	}
	xwm_flush();
}

/* ---- geometry and stacking, once per repaint -------------------------------------------------- */

#define MAXORD 256
static xcb_window_t sent_order[MAXORD];
static int sent_n;

void xwm_sync(void)
{
	if (!X.c || !ready)
		return;
	struct xwin *xw;
	bool any = false;
	/* positions: the window moved, the server has to know (menus are placed from it) */
	wl_list_for_each(xw, &X.all, link) {
		struct window *w = xw->win;
		if (!w || xw->unmanaged || !w->mapped)
			continue;
		if (!xw->sent_pos || w->x != xw->sx || w->y != xw->sy) {
			uint32_t v[2] = { (uint32_t)w->x, (uint32_t)w->y };
			xcb_configure_window(X.c, xw->id, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, v);
			xw->sx = w->x;
			xw->sy = w->y;
			xw->sent_pos = true;
			any = true;
		}
	}
	/* stacking: hidden windows at the bottom, then the visible ones in flcomp's order, then the
	 * unmanaged ones: the server picks the window under the pointer by its own stack */
	xcb_window_t want[MAXORD];
	int n = 0;
	struct window *w;
	wl_list_for_each_reverse(w, &C.windows, link)	/* bottom of flcomp's stack first */
		if (w->xw && w->xw->mapped_x && !w->xw->unmanaged && !window_visible(w) && n < MAXORD)
			want[n++] = w->xw->id;
	wl_list_for_each_reverse(w, &C.windows, link)
		if (w->xw && w->xw->mapped_x && !w->xw->unmanaged && window_visible(w) && n < MAXORD)
			want[n++] = w->xw->id;
	wl_list_for_each(xw, &X.all, link)
		if (xw->unmanaged && xw->mapped_x && n < MAXORD)
			want[n++] = xw->id;
	int i = 0;
	while (i < n && i < sent_n && want[i] == sent_order[i])
		i++;
	for (int k = i; k < n; k++) {
		uint32_t v[2];
		if (k == 0) {
			v[0] = XCB_STACK_MODE_BELOW;
			xcb_configure_window(X.c, want[k], XCB_CONFIG_WINDOW_STACK_MODE, v);
		} else {
			v[0] = want[k - 1];
			v[1] = XCB_STACK_MODE_ABOVE;
			xcb_configure_window(X.c, want[k], XCB_CONFIG_WINDOW_SIBLING | XCB_CONFIG_WINDOW_STACK_MODE, v);
		}
		any = true;
	}
	if (i < n || n != sent_n) {
		memcpy(sent_order, want, (size_t)n * sizeof want[0]);
		sent_n = n;
		/* _NET_CLIENT_LIST for pagers and tools */
		xcb_change_property(X.c, XCB_PROP_MODE_REPLACE, X.root, X.a._NET_CLIENT_LIST_STACKING,
				    XCB_ATOM_WINDOW, 32, (uint32_t)n, want);
		xcb_change_property(X.c, XCB_PROP_MODE_REPLACE, X.root, X.a._NET_CLIENT_LIST,
				    XCB_ATOM_WINDOW, 32, (uint32_t)n, want);
		any = true;
	}
	/* the current workspace */
	static int cur_ws = -1, nws = -1;
	if (cur_ws != C.ws_cur || nws != C.ws_cols * C.ws_rows) {
		cur_ws = C.ws_cur;
		nws = C.ws_cols * C.ws_rows;
		uint32_t cur = (uint32_t)cur_ws, num = (uint32_t)nws;
		xcb_change_property(X.c, XCB_PROP_MODE_REPLACE, X.root, X.a._NET_CURRENT_DESKTOP, XCB_ATOM_CARDINAL, 32, 1, &cur);
		xcb_change_property(X.c, XCB_PROP_MODE_REPLACE, X.root, X.a._NET_NUMBER_OF_DESKTOPS, XCB_ATOM_CARDINAL, 32, 1, &num);
		any = true;
	}
	if (any)
		xwm_flush();
}


/* ---- unmanaged windows --------------------------------------------------------------------- */

int xwm_or_list(struct surface **out, int max)
{
	int n = 0;
	struct xwin *xw;
	if (!ready)
		return 0;
	wl_list_for_each(xw, &X.all, link)
		if (xw->unmanaged && xw->surf && xw->surf->px && xw->ws_seen == C.ws_cur && n < max)
			out[n++] = xw->surf;
	return n;
}

bool xwm_or_visible(void)
{
	struct surface *l[1];
	return xwm_or_list(l, 1) > 0;
}

/* ---- events ------------------------------------------------------------------------------- */

static void ev_configure_request(xcb_configure_request_event_t *e)
{
	struct xwin *xw = xwin_by_id(e->window);
	if (!xw)
		return;
	uint16_t m = e->value_mask;
	struct window *w = xw->win;
	bool managed_up = w && !xw->unmanaged;
	uint32_t v[7];
	int n = 0;
	uint16_t out = 0;
	if (managed_up) {
		/* flcomp places managed windows; a size request is granted unless the window is maximized,
		 * fullscreen or snapped */
		bool fixed = w->maximized || w->fullscreen || w->snap;
		if ((m & (XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT)) && !fixed) {
			int nw = (m & XCB_CONFIG_WINDOW_WIDTH) ? e->width : xw->w;
			int nh = (m & XCB_CONFIG_WINDOW_HEIGHT) ? e->height : xw->h;
			xw->cw = xw->ch = 0;
			xwm_window_configure(w, nw, nh);
			return;
		}
		/* nothing changes: say where the window is (ICCCM 4.1.5) */
		xcb_configure_notify_event_t ev = { 0 };
		ev.response_type = XCB_CONFIGURE_NOTIFY;
		ev.event = ev.window = xw->id;
		ev.x = (int16_t)w->x;
		ev.y = (int16_t)w->y;
		ev.width = (uint16_t)xw->w;
		ev.height = (uint16_t)xw->h;
		xcb_send_event(X.c, 0, xw->id, XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *)&ev);
		xwm_flush();
		return;
	}
	/* unmanaged, or not up yet: the client gets what it asks for */
	if (m & XCB_CONFIG_WINDOW_X) { v[n++] = (uint32_t)(int32_t)e->x; out |= XCB_CONFIG_WINDOW_X; xw->x = e->x; }
	if (m & XCB_CONFIG_WINDOW_Y) { v[n++] = (uint32_t)(int32_t)e->y; out |= XCB_CONFIG_WINDOW_Y; xw->y = e->y; }
	if (m & XCB_CONFIG_WINDOW_WIDTH) { v[n++] = e->width; out |= XCB_CONFIG_WINDOW_WIDTH; xw->w = e->width; }
	if (m & XCB_CONFIG_WINDOW_HEIGHT) { v[n++] = e->height; out |= XCB_CONFIG_WINDOW_HEIGHT; xw->h = e->height; }
	if (m & XCB_CONFIG_WINDOW_BORDER_WIDTH) { v[n++] = 0; out |= XCB_CONFIG_WINDOW_BORDER_WIDTH; }
	if (n)
		xcb_configure_window(X.c, xw->id, out, v);
	xwm_flush();
}

static void ev_configure_notify(xcb_configure_notify_event_t *e)
{
	struct xwin *xw = xwin_by_id(e->window);
	if (!xw)
		return;
	bool moved = e->x != xw->x || e->y != xw->y;
	if (xw->unmanaged && moved && xw->surf && xw->surf->px)
		damage_tree(xw->surf);
	xw->w = e->width;
	xw->h = e->height;
	if (xw->unmanaged || !xw->win) {
		xw->x = e->x;
		xw->y = e->y;
	}
	if (xw->unmanaged && moved && xw->surf && xw->surf->px)
		damage_tree(xw->surf);
	/* stacking order of the unmanaged ones */
	struct xwin *above = e->above_sibling ? xwin_by_id(e->above_sibling) : NULL;
	wl_list_remove(&xw->link);
	if (above)
		wl_list_insert(&above->link, &xw->link);
	else
		wl_list_insert(&X.all, &xw->link);
	if (xw->unmanaged && xw->surf && xw->surf->px)
		damage_tree(xw->surf);
}

static void ev_create(xcb_create_notify_event_t *e)
{
	if (e->window == X.win || xwin_by_id(e->window))
		return;
	XDBG("create 0x%x %dx%d%+d%+d override=%d", e->window, e->width, e->height, e->x, e->y, e->override_redirect);
	xwin_new(e->window, e->override_redirect, e->x, e->y, e->width, e->height);
}

static void ev_destroy(xcb_destroy_notify_event_t *e)
{
	struct xwin *xw = xwin_by_id(e->window);
	if (!xw)
		return;
	if (xw->unmanaged && xw->surf && xw->surf->px)
		damage_tree(xw->surf);
	xwin_free(xw);
	idle_check();
}

static void ev_map_request(xcb_map_request_event_t *e)
{
	struct xwin *xw = xwin_by_id(e->window);
	if (!xw)
		xw = xwin_new(e->window, false, 0, 0, 1, 1);
	if (!xw)
		return;
	xw->map_wanted = true;
	props_request_all(xw);
}

static void ev_map_notify(xcb_map_notify_event_t *e)
{
	struct xwin *xw = xwin_by_id(e->window);
	if (!xw)
		return;
	xw->mapped_x = true;
	if (xw->override) {
		/* no MapRequest for these: read what draws or places them (the type decides nothing) */
		xw->typed = true;
	}
}

static void ev_unmap_notify(xcb_unmap_notify_event_t *e)
{
	struct xwin *xw = xwin_by_id(e->window);
	if (!xw)
		return;
	xw->mapped_x = false;
	xw->map_wanted = false;
	if (xw->unmanaged && xw->surf && xw->surf->px)
		damage_tree(xw->surf);
	if (!xw->override)
		write_wm_state(xw, 0);
	if (xw == xfocus)
		xwm_focus_sync();
}

static void ev_property(xcb_property_notify_event_t *e)
{
	struct xwin *xw = xwin_by_id(e->window);
	if (!xw || e->state == XCB_PROPERTY_DELETE)
		return;
	xcb_atom_t a = e->atom;
	if (a == X.a.WM_NAME) prop_request(xw, P_TITLE, a, 256);
	else if (a == X.a._NET_WM_NAME) prop_request(xw, P_NETNAME, a, 256);
	else if (a == X.a.WM_CLASS) prop_request(xw, P_CLASS, a, 64);
	else if (a == X.a.WM_TRANSIENT_FOR) prop_request(xw, P_TRANSIENT, a, 1);
	else if (a == X.a.WM_NORMAL_HINTS) prop_request(xw, P_NORMAL, a, 18);
	else if (a == X.a.WM_HINTS) prop_request(xw, P_HINTS, a, 9);
	else if (a == X.a.WM_PROTOCOLS) prop_request(xw, P_PROTOCOLS, a, 32);
	else if (a == X.a._NET_WM_WINDOW_TYPE) prop_request(xw, P_TYPE, a, 32);
	else if (a == X.a._MOTIF_WM_HINTS) prop_request(xw, P_MOTIF, a, 5);
}

static void ev_client_message(xcb_client_message_event_t *e)
{
	struct xwin *xw = xwin_by_id(e->window);
	if (e->type == X.a.WL_SURFACE_SERIAL) {
		if (!xw)
			return;
		xw->serial = (uint64_t)e->data.data32[1] << 32 | e->data.data32[0];
		XDBG("window 0x%x serial %llu", xw->id, (unsigned long long)xw->serial);
		match_window(xw);
		return;
	}
	if (!xw)
		return;
	struct window *w = xw->win;
	if (e->type == X.a._NET_WM_STATE) {
		uint32_t act = e->data.data32[0];	/* 0 remove, 1 add, 2 toggle */
		for (int i = 1; i <= 2; i++) {
			xcb_atom_t a = e->data.data32[i];
			if (!a)
				continue;
			if (a == X.a._NET_WM_STATE_FULLSCREEN) {
				bool cur = w ? w->fullscreen : xw->want_full;
				bool nv = act == 2 ? !cur : act == 1;
				if (w)
					window_request_state(w, -1, nv);
				else
					xw->want_full = nv;
			} else if (a == X.a._NET_WM_STATE_MAXIMIZED_VERT || a == X.a._NET_WM_STATE_MAXIMIZED_HORZ) {
				bool cur = w ? w->maximized : xw->want_max;
				bool nv = act == 2 ? !cur : act == 1;
				if (w)
					window_request_state(w, nv, -1);
				else
					xw->want_max = nv;
				i = 2;	/* both halves in one message: once */
			} else if (a == X.a._NET_WM_STATE_SKIP_TASKBAR) {
				xw->skip_taskbar = act == 2 ? !xw->skip_taskbar : act == 1;
			}
		}
	} else if (e->type == X.a._NET_ACTIVE_WINDOW) {
		if (w)
			window_activate(w);
	} else if (e->type == X.a._NET_CLOSE_WINDOW) {
		if (w)
			xwm_window_close(w);
	} else if (e->type == X.a.WM_CHANGE_STATE) {
		if (w && e->data.data32[0] == 3)
			window_minimize(w);
	} else if (e->type == X.a._NET_WM_DESKTOP) {
		if (w && (int)e->data.data32[0] >= 0 && (int)e->data.data32[0] < ws_count())
			ws_move_window(w, (int)e->data.data32[0], false);
	} else if (e->type == X.a._NET_REQUEST_FRAME_EXTENTS) {
		struct xinset in = (!xw->unmanaged && !xw->nodeco_hint) ? (struct xinset){ XD_BORDER, XD_BORDER, XD_TITLE, XD_BORDER } : (struct xinset){ 0 };
		uint32_t v[4] = { (uint32_t)in.l, (uint32_t)in.r, (uint32_t)in.t, (uint32_t)in.b };
		xcb_change_property(X.c, XCB_PROP_MODE_REPLACE, xw->id, X.a._NET_FRAME_EXTENTS, XCB_ATOM_CARDINAL, 32, 4, v);
		xwm_flush();
	} else if (e->type == X.a._NET_WM_MOVERESIZE) {
		/* the client's own title bar (Steam, GTK): 8 move, 0..7 the edges clockwise from the top left */
		if (!w || !C.btn_down)
			return;
		uint32_t dir = e->data.data32[2];
		static const unsigned edges[8] = { 1 | 4, 1, 1 | 8, 8, 2 | 8, 2, 2 | 4, 4 };	/* top, bottom, left, right bits of xdg.c */
		if (dir == 8)
			window_move_begin(w);
		else if (dir < 8)
			window_resize_begin(w, edges[dir]);
	}
}

static void event_dispatch(xcb_generic_event_t *ev)
{
	if (xwsel_event(ev))
		return;
	switch (ev->response_type & 0x7f) {
	case XCB_CREATE_NOTIFY: ev_create((void *)ev); break;
	case XCB_DESTROY_NOTIFY: ev_destroy((void *)ev); break;
	case XCB_MAP_REQUEST: ev_map_request((void *)ev); break;
	case XCB_MAP_NOTIFY: ev_map_notify((void *)ev); break;
	case XCB_UNMAP_NOTIFY: ev_unmap_notify((void *)ev); break;
	case XCB_CONFIGURE_REQUEST: ev_configure_request((void *)ev); break;
	case XCB_CONFIGURE_NOTIFY: ev_configure_notify((void *)ev); break;
	case XCB_PROPERTY_NOTIFY: ev_property((void *)ev); break;
	case XCB_CLIENT_MESSAGE: ev_client_message((void *)ev); break;
	default: break;
	}
}

/* ---- connection and teardown ------------------------------------------------------------- */

static void xwl_stop(void);

/* Xwayland does not leave by itself while the window manager is connected, so flcomp ends it:
 * when no X window is left, and (XRes) the only client of the server is the window manager,
 * after a few seconds of grace for a client that has connected and not made a window yet. */
#define IDLE_MS 3000

static int idle_cb(void *data)
{
	if (!X.c || !ready)
		return 0;
	if (!wl_list_empty(&X.all)) {
		idle_check();
		return 0;
	}
	xcb_res_query_clients_reply_t *r = xcb_res_query_clients_reply(X.c, xcb_res_query_clients(X.c), NULL);
	int n = r ? xcb_res_query_clients_clients_length(r) : 3;
	free(r);
	XDBG("idle check: %d client(s) in XRes", n);
	if (n > 2) {		/* XRes counts the server itself and our window manager: somebody else is connected */
		wl_event_source_timer_update(idle_timer, IDLE_MS);
		return 0;
	}
	fprintf(stderr, "flcomp: xwayland: no X client left, stopping Xwayland\n");
	xwl_stop();
	return 0;
}

static void idle_check(void)
{
	if (!ready)
		return;
	if (!wl_list_empty(&X.all)) {
		if (idle_timer)
			wl_event_source_timer_update(idle_timer, 0);
		return;
	}
	if (!idle_timer)
		idle_timer = wl_event_loop_add_timer(C.loop, idle_cb, NULL);
	if (idle_timer)
		wl_event_source_timer_update(idle_timer, IDLE_MS);
}

static void xclient_gone(struct wl_listener *l, void *data)
{
	wl_list_remove(&l->link);
	xclient = NULL;
}

static void x_lost(void)
{
	xwl_stop();
}

static int x_cb(int fd, uint32_t mask, void *data)
{
	if (!X.c)
		return 0;
	if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
		x_lost();
		return 0;
	}
	xcb_generic_event_t *ev;
	while (X.c && (ev = xcb_poll_for_event(X.c))) {
		event_dispatch(ev);
		free(ev);
	}
	if (X.c)
		pending_replies();
	if (X.c && xcb_connection_has_error(X.c)) {
		x_lost();
		return 0;
	}
	if (X.c)
		xcb_flush(X.c);
	return 0;
}

static bool atoms_intern(void)
{
	static const char *const names[] = {
#define A(n) #n,
		XWM_ATOMS(A)
#undef A
	};
	int n = (int)(sizeof names / sizeof names[0]);
	xcb_intern_atom_cookie_t ck[sizeof names / sizeof names[0]];
	for (int i = 0; i < n; i++)
		ck[i] = xcb_intern_atom(X.c, 0, (uint16_t)strlen(names[i]), names[i]);
	xcb_atom_t *dst = (xcb_atom_t *)&X.a;
	for (int i = 0; i < n; i++) {
		xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(X.c, ck[i], NULL);
		if (!r)
			return false;
		dst[i] = r->atom;
		free(r);
	}
	return true;
}

xcb_atom_t xwm_intern(const char *name)
{
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(X.c, xcb_intern_atom(X.c, 0, (uint16_t)strlen(name), name), NULL);
	xcb_atom_t a = r ? r->atom : XCB_NONE;
	free(r);
	return a;
}

char *xwm_atom_name(xcb_atom_t a)
{
	xcb_get_atom_name_reply_t *r = xcb_get_atom_name_reply(X.c, xcb_get_atom_name(X.c, a), NULL);
	char *s = r ? dupn(xcb_get_atom_name_name(r), xcb_get_atom_name_name_length(r)) : NULL;
	free(r);
	return s;
}

static bool xwm_setup(void)
{
	X.scr = xcb_setup_roots_iterator(xcb_get_setup(X.c)).data;
	X.root = X.scr->root;
	if (!atoms_intern())
		return false;
	xcb_xfixes_query_version_reply_t *xf = xcb_xfixes_query_version_reply(X.c, xcb_xfixes_query_version(X.c, 5, 0), NULL);
	free(xf);
	X.xfixes = xcb_get_extension_data(X.c, &xcb_xfixes_id);
	/* Rootless Xwayland makes a wl_surface only for windows that are redirected by Composite
	 * (manual): the window manager redirects all children of the root, as every XWM does. */
	xcb_composite_query_version_reply_t *cv = xcb_composite_query_version_reply(X.c, xcb_composite_query_version(X.c, 0, 4), NULL);
	free(cv);
	/* we are the window manager: whoever else holds the redirect makes this fail */
	uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
			XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_void_cookie_t ck = xcb_change_window_attributes_checked(X.c, X.root, XCB_CW_EVENT_MASK, &mask);
	xcb_generic_error_t *err = xcb_request_check(X.c, ck);
	if (err) {
		free(err);
		return false;
	}
	xcb_composite_redirect_subwindows(X.c, X.root, XCB_COMPOSITE_REDIRECT_MANUAL);
	X.win = xcb_generate_id(X.c);
	uint32_t vals[2] = { 1, XCB_EVENT_MASK_PROPERTY_CHANGE };
	xcb_create_window(X.c, XCB_COPY_FROM_PARENT, X.win, X.root, -10, -10, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
			  XCB_COPY_FROM_PARENT, XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, vals);
	xcb_change_property(X.c, XCB_PROP_MODE_REPLACE, X.win, X.a._NET_WM_NAME, X.a.UTF8_STRING, 8, 6, "flcomp");
	xcb_change_property(X.c, XCB_PROP_MODE_REPLACE, X.win, X.a._NET_SUPPORTING_WM_CHECK, XCB_ATOM_WINDOW, 32, 1, &X.win);
	xcb_change_property(X.c, XCB_PROP_MODE_REPLACE, X.root, X.a._NET_SUPPORTING_WM_CHECK, XCB_ATOM_WINDOW, 32, 1, &X.win);
	xcb_atom_t sup[] = {
		X.a._NET_SUPPORTED, X.a._NET_SUPPORTING_WM_CHECK, X.a._NET_WM_NAME, X.a._NET_WM_STATE,
		X.a._NET_WM_STATE_FULLSCREEN, X.a._NET_WM_STATE_MAXIMIZED_VERT, X.a._NET_WM_STATE_MAXIMIZED_HORZ,
		X.a._NET_WM_STATE_HIDDEN, X.a._NET_WM_STATE_FOCUSED, X.a._NET_WM_STATE_SKIP_TASKBAR,
		X.a._NET_ACTIVE_WINDOW, X.a._NET_CLIENT_LIST, X.a._NET_CLIENT_LIST_STACKING,
		X.a._NET_NUMBER_OF_DESKTOPS, X.a._NET_CURRENT_DESKTOP, X.a._NET_DESKTOP_GEOMETRY,
		X.a._NET_FRAME_EXTENTS, X.a._NET_REQUEST_FRAME_EXTENTS, X.a._NET_CLOSE_WINDOW,
		X.a._NET_WM_MOVERESIZE, X.a._NET_WM_WINDOW_TYPE, X.a._NET_WM_DESKTOP,
	};
	xcb_change_property(X.c, XCB_PROP_MODE_REPLACE, X.root, X.a._NET_SUPPORTED, XCB_ATOM_ATOM, 32,
			    sizeof sup / sizeof sup[0], sup);
	uint32_t geo[2] = { (uint32_t)C.w, (uint32_t)C.h };
	xcb_change_property(X.c, XCB_PROP_MODE_REPLACE, X.root, X.a._NET_DESKTOP_GEOMETRY, XCB_ATOM_CARDINAL, 32, 2, geo);
	/* Xwayland does not accept X clients until the window manager owns WM_S0: that is its "ready" */
	xcb_set_selection_owner(X.c, X.win, X.a.WM_S0, XCB_CURRENT_TIME);
	xcb_flush(X.c);
	return true;
}

static void xwl_teardown_windows(void)
{
	/* the clients are gone with the server: take their windows down now, the surfaces follow */
	struct surface *s;
	wl_list_for_each(s, &C.surfaces, link) {
		if (s->role != ROLE_XWL)
			continue;
		if (s->win)
			window_destroy_for_surface(s);
		s->xw = NULL;
	}
	struct xwin *xw, *tmp;
	wl_list_for_each_safe(xw, tmp, &X.all, link) {
		wl_list_remove(&xw->link);
		free(xw->title);
		free(xw->net_title);
		free(xw->klass);
		free(xw->instance);
		free(xw);
	}
	xfocus = NULL;
	sent_n = 0;
	pq_clear();
}

static void xwl_stop(void)
{
	if (idle_timer) {
		wl_event_source_remove(idle_timer);
		idle_timer = NULL;
	}
	if (disp_src) {
		wl_event_source_remove(disp_src);
		disp_src = NULL;
	}
	if (x_src) {
		wl_event_source_remove(x_src);
		x_src = NULL;
	}
	if (disp_fd >= 0) {
		close(disp_fd);
		disp_fd = -1;
	}
	if (X.c) {
		xwsel_fini();
		xwl_teardown_windows();
		xcb_disconnect(X.c);
		X.c = NULL;
		wm_fd = -1;
	} else if (wm_fd >= 0) {
		close(wm_fd);
		wm_fd = -1;
	}
	ready = false;
	if (xpid > 0)
		kill(xpid, SIGTERM);	/* its exit comes through SIGCHLD and re-arms the socket */
	if (xclient) {
		wl_list_remove(&xclient_listener.link);
		wl_client_destroy(xclient);
		xclient = NULL;
	}
}

bool xwm_child_exited(pid_t pid, int status)
{
	if (pid != xpid || pid <= 0)
		return false;
	fprintf(stderr, "flcomp: xwayland: Xwayland ended (%s %d)\n", WIFEXITED(status) ? "status" : "signal",
		WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
	xpid = 0;
	xwl_stop();
	if (disp_num >= 0)
		listen_arm();	/* the next X client starts it again */
	return true;
}

static int disp_cb(int fd, uint32_t mask, void *data)
{
	char buf[32];
	ssize_t n = read(fd, buf, sizeof buf - 1);
	if (disp_src) {
		wl_event_source_remove(disp_src);
		disp_src = NULL;
	}
	close(disp_fd);
	disp_fd = -1;
	if (n <= 0) {	/* Xwayland died before it was ready */
		fprintf(stderr, "flcomp: xwayland: Xwayland did not become ready\n");
		return 0;		/* SIGCHLD takes care of the rest */
	}
	X.c = xcb_connect_to_fd(wm_fd, NULL);
	if (!X.c || xcb_connection_has_error(X.c)) {
		fprintf(stderr, "flcomp: xwayland: cannot connect the window manager\n");
		if (X.c) {
			xcb_disconnect(X.c);
			X.c = NULL;
			wm_fd = -1;
		}
		xwl_stop();
		return 0;
	}
	wl_list_init(&X.all);
	if (!xwm_setup()) {
		fprintf(stderr, "flcomp: xwayland: window manager setup failed\n");
		xwl_stop();
		return 0;
	}
	x_src = wl_event_loop_add_fd(C.loop, xcb_get_file_descriptor(X.c), WL_EVENT_READABLE, x_cb, NULL);
	ready = true;
	xwsel_init();
	idle_check();
	fprintf(stderr, "flcomp: xwayland: ready on %s\n", disp_name);
	x_cb(0, 0, NULL);	/* what arrived while we set up */
	return 0;
}

static void xwl_start(void)
{
	int wl[2], wm[2], dp[2];
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wl) < 0)
		return;
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wm) < 0) {
		close(wl[0]); close(wl[1]);
		return;
	}
	if (pipe2(dp, O_CLOEXEC) < 0) {
		close(wl[0]); close(wl[1]); close(wm[0]); close(wm[1]);
		return;
	}
	xclient = wl_client_create(C.dpy, wl[0]);
	if (!xclient) {
		close(wl[0]); close(wl[1]); close(wm[0]); close(wm[1]); close(dp[0]); close(dp[1]);
		return;
	}
	xclient_listener.notify = xclient_gone;
	wl_client_add_destroy_listener(xclient, &xclient_listener);
	pid_t pid = fork();
	if (pid < 0) {
		wl_list_remove(&xclient_listener.link);
		wl_client_destroy(xclient);
		xclient = NULL;
		close(wl[1]); close(wm[0]); close(wm[1]); close(dp[0]); close(dp[1]);
		listen_arm();
		return;
	}
	if (pid == 0) {
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGPIPE, SIG_DFL);
		int keep[] = { wl[1], wm[1], dp[1], listen_fd };
		for (unsigned i = 0; i < sizeof keep / sizeof keep[0]; i++)
			fcntl(keep[i], F_SETFD, 0);
		char a_disp[16], a_listen[16], a_wm[16], a_dp[16], a_sock[16];
		snprintf(a_disp, sizeof a_disp, ":%d", disp_num);
		snprintf(a_listen, sizeof a_listen, "%d", listen_fd);
		snprintf(a_wm, sizeof a_wm, "%d", wm[1]);
		snprintf(a_dp, sizeof a_dp, "%d", dp[1]);
		snprintf(a_sock, sizeof a_sock, "%d", wl[1]);
		const char *rt = getenv("XDG_RUNTIME_DIR");
		if (rt && !getenv("FLCOMP_XWAYLAND_LOG_STDERR")) {	/* xkbcomp's warnings are noise: a file of its own */
			char lp[256];
			snprintf(lp, sizeof lp, "%s/xwayland.log", rt);
			int lf = open(lp, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
			if (lf >= 0) {
				dup2(lf, 2);
				close(lf);
			}
		}
		setenv("WAYLAND_SOCKET", a_sock, 1);
		unsetenv("WAYLAND_DISPLAY");
		unsetenv("DISPLAY");
		execlp(xwl_bin, "Xwayland", a_disp, "-rootless", "-listenfd", a_listen, "-wm", a_wm,
		       "-displayfd", a_dp, "-nolisten", "tcp", (char *)NULL);
		fprintf(stderr, "flcomp: cannot run Xwayland: %s\n", strerror(errno));
		_exit(127);
	}
	xpid = pid;
	close(wl[1]);
	close(wm[1]);
	close(dp[1]);
	wm_fd = wm[0];
	disp_fd = dp[0];
	/* Xwayland holds the listening socket now; ours is closed so a stale copy never accepts */
	close(listen_fd);
	listen_fd = -1;
	disp_src = wl_event_loop_add_fd(C.loop, disp_fd, WL_EVENT_READABLE, disp_cb, NULL);
	fprintf(stderr, "flcomp: xwayland: starting Xwayland (pid %d) for %s\n", pid, disp_name);
}

/* ---- init and exit ------------------------------------------------------------------------ */

static const char *find_bin(void)
{
	static char path[512];
	const char *e = getenv("FLCOMP_XWAYLAND");
	if (e && *e)
		return access(e, X_OK) == 0 ? e : NULL;
	const char *p = getenv("PATH");
	if (!p)
		p = "/usr/bin";
	char *cp = strdup(p), *save = NULL;
	for (char *d = cp ? strtok_r(cp, ":", &save) : NULL; d; d = strtok_r(NULL, ":", &save)) {
		snprintf(path, sizeof path, "%s/Xwayland", d);
		if (access(path, X_OK) == 0) {
			free(cp);
			return path;
		}
	}
	free(cp);
	return NULL;
}

void xwm_init(void)
{
	wl_list_init(&X.all);
	const char *off = getenv("FLCOMP_NO_XWAYLAND");
	if (off && *off && strcmp(off, "0"))
		return;
	/* headless runs (tests) have no X unless they ask: FLCOMP_XWAYLAND=/path/to/Xwayland */
	if (C.headless && !getenv("FLCOMP_XWAYLAND"))
		return;
	xwl_bin = find_bin();
	if (!xwl_bin) {
		fprintf(stderr, "flcomp: xwayland: Xwayland is not installed, X11 programs will not run\n");
		return;
	}
	for (int n = 0; n < 64; n++)
		if (display_free(n)) {
			int fd = listen_bind(n);
			if (fd < 0)
				continue;
			disp_num = n;
			listen_fd = fd;
			break;
		}
	if (disp_num < 0) {
		fprintf(stderr, "flcomp: xwayland: no free X display\n");
		return;
	}
	snprintf(disp_name, sizeof disp_name, ":%d", disp_num);
	setenv("DISPLAY", disp_name, 1);
	shell_global = wl_global_create(C.dpy, &xwayland_shell_v1_interface, 1, NULL, xshell_bind);
	wl_display_set_global_filter(C.dpy, shell_filter, NULL);
	seat_selection_watch(xwsel_wayland_changed);
	listen_arm();
	fprintf(stderr, "flcomp: xwayland: DISPLAY=%s, Xwayland starts with the first X client\n", disp_name);
}

void xwm_fini(void)
{
	pid_t p = xpid;
	xpid = 0;		/* its exit is expected: no re-arming, no second kill */
	xwl_stop();
	if (p > 0) {
		kill(p, SIGTERM);
		waitpid(p, NULL, 0);
	}
	if (listen_src) {
		wl_event_source_remove(listen_src);
		listen_src = NULL;
	}
	if (listen_fd >= 0) {
		close(listen_fd);
		listen_fd = -1;
	}
	if (disp_num >= 0) {
		unlink(sock_path);
		disp_num = -1;
	}
}
