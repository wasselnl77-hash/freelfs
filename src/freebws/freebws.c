/* FreeBWS -- the FreeLFS web browser: WPE WebKit in a libflui window.
 *
 * The window is a libflui toplevel: tab row (tabstrip.c), toolbar (back, forward,
 * reload, address bar) drawn with the toolkit, load progress under it. The page is
 * a wl_subsurface of that window: WPE WebKit renders into wpebackend-fdo's private
 * Wayland display, and every finished frame of the active tab is handed to flcomp
 * as a buffer of our own:
 *   shm      pixels are copied into a wl_shm buffer (llvmpipe, no GPU)
 *   dmabuf   the GPU buffer is passed on through zwp_linux_dmabuf_v1 (a GPU
 *            that flcomp announces tiled modifiers for; FREEBWS_BUFFERS=dmabuf
 *            or =shm overrides the choice). wpebackend-fdo needs an EGLDisplay of
 *            this process for that (fdo_egl_start: GBM on a render node): without
 *            one the web process finds no linux-dmabuf global. NOT
 *            wpe_fdo_initialize_dmabuf(): that is the "dmabuf pool" mode, where the
 *            embedder must allocate the buffers itself (wpe_view_backend_dmabuf_pool_fdo);
 *            with the exportable used here the web process's create_buffer gets
 *            wl_display error "no memory" and nothing is ever drawn (the blank browser
 *            on real hardware, FreeBWS before release 9)
 * Tabs: every tab is a WebKitWebView with a view backend of its own; all share
 * one WebKitWebContext and the one network process. Background tabs are not
 * visible to WebKit (it throttles them) and are discarded (web view destroyed,
 * session state kept, reloaded when selected) after a while or when memory runs
 * low. Memory settings: see "memory" below. DSSL: see the DSSL section.
 * SPDX-License-Identifier: MIT */
#include <errno.h>
#include <glib-unix.h>
#include <math.h>
#include <glib.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dirent.h>
#include <fcntl.h>
#include <gbm.h>
#include <wpe/fdo.h>
#include <wpe/fdo-egl.h>
#include <wpe/unstable/fdo-shm.h>
#include <wpe/webkit.h>

#include <dssl.h>
#include <flui.h>

#include "adblock.h"
#include "bookmarks.h"
#include "config.h"
#include "downloads.h"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "pages.h"
#include "popup.h"
#include "tabstrip.h"

#define TOOLBAR_H 44
#define BAR_H 30		/* the bookmarks bar */
#define MAX_SBUF 4
#define MAX_TABS 24
#define MAX_CLOSED 8
#define BTN_LEFT_ 0x110
#define BTN_RIGHT_ 0x111
#define BTN_MIDDLE_ 0x112

enum { ID_BACK = 1, ID_FWD, ID_RELOAD, ID_URL, ID_STAR, ID_SHIELD, ID_DL, ID_MENU, ID_BAR_MORE };
#define ID_BAR_ITEM 0x4000
enum { POP_NONE, POP_MENU, POP_SHIELD, POP_DL, POP_FOLDER };

struct sbuf {
	struct wl_buffer *b;
	void *map;
	size_t size;
	int w, h, busy;
};

struct tab {
	WebKitWebView *view;		/* NULL while the tab is discarded */
	struct wpe_view_backend_exportable_fdo *exp;
	WebKitUserContentManager *ucm;
	WebKitWebViewSessionState *saved;	/* back/forward list of a discarded tab */
	char uri[2048];
	char title[256];
	int loading;
	double progress;
	int discarded;
	struct adb_tab adb;	/* ad blocker: cosmetic sheet, scriptlets, count */
	WebKitUserScript *dark_script;	/* force dark mode: the script of the page shown */
	char dark_host[256];
	int dark_on;
	char err_uri[2048];	/* the address an error page stands for */
	int gating;		/* the page waits for the ad blocker's filters: a notice is shown instead */
	GTlsCertificate *tls_cert;	/* certificate that failed, for "continue anyway" */
	char tls_host[256];
	int reload;		/* the web process died: reload when selected */
	int w, h;		/* last size sent to the page */
	uint32_t last_active;	/* now_ms() when it stopped being the active tab */
	uint8_t *snap;		/* last frame shown (shm mode), for the switch */
	int snap_w, snap_h;
	struct tab *opener;	/* the tab that opened this one with window.open (NULL: none, or it is gone) */
	int want_fg;		/* a popup: it takes the front when its page is ready */
	int shown;		/* ready-to-show came, or the fallback timer ran */
	guint show_src;		/* the fallback timer that shows a popup whose ready-to-show never comes */
};

static struct B {
	fl_app *app;
	fl_win *win;
	GMainLoop *loop;
	struct wl_display *dpy;
	struct wl_compositor *comp;
	struct wl_subcompositor *subc;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct zwp_linux_dmabuf_v1 *dmabuf;
	int tiled_mods;		/* modifiers other than LINEAR that flcomp announced */
	struct wl_surface *page;
	struct wl_subsurface *sub;
	struct wl_pointer *ptr;
	int in_page, page_focus;
	double px, py;
	fl_rect page_rect;
	int use_dmabuf;
	char **argv;		/* for the dmabuf watchdog's restart */
	struct sbuf sb[MAX_SBUF];
	struct sbuf *last;	/* the buffer on screen (shm mode) */
	/* tabs */
	struct tab *tabs[MAX_TABS];
	int ntabs;
	struct tab *cur;
	GHashTable *live_exp;	/* exportables whose dmabuf buffers may still be released */
	char closed[MAX_CLOSED][2048];
	int nclosed;
	int spin;
	guint spin_src;
	/* WPE */
	WebKitWebContext *ctx;
	char url[2048];		/* what the address bar holds */
	int url_focused;	/* the address bar had the keyboard at the end of the last pass */
	int sel_all;		/* select the whole address on the next pass */
	int sel_hold;		/* the click that focused the bar is still down: keep it selected */
	int url_commit;		/* Enter was pressed: do not put the old address back */
	const char *home;	/* the start page; its address is shown as an empty bar */
	uint64_t frames;
	/* memory policy */
	unsigned idle_s;	/* background tab idle this long: discard */
	unsigned avail_mb;	/* MemAvailable below this: discard the oldest background tab */
	const char *features;	/* WebKit features to set, "Name=0,Name=1" */
	char *ua;		/* the user agent every page gets (useragent= in the config) */
	int dssl_native;	/* 0: always use the JS engine (FREEBWS_DSSL=engine) */
	int share_process;	/* tabs are related views: one web process for all (FREEBWS_SHARE=1) */
	/* toolbar extras */
	struct popup *pop;
	int pop_kind, pop_folder;	/* the open popup; for POP_FOLDER: the folder (-1: bar overflow) */
	fl_rect pop_anchor;
	int pop_closed;		/* kind of a popup closed by the press that is still going on */
	int bar_from;		/* first bar item that did not fit (overflow) */
	char toast[96];
	guint toast_src;
	int dl_last_active;
	guint dl_flash_src;
	int dl_flash;		/* a download just finished: ring full for a moment */
	fl_rect dl_btn;
} B;

static void load_uri(struct tab *t, const char *uri);
static void toast(const char *msg);
static void pop_close(void);
static void pop_close_hook(void);
static void bookmark_toggle(void);
static void open_in_tab(const char *url, int new_tab);
struct tab;
static void on_count(WebKitUserContentManager *m, JSCValue *v, struct tab *t);

static uint32_t now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static struct wpe_view_backend *tab_vb(struct tab *t)
{
	return wpe_view_backend_exportable_fdo_get_view_backend(t->exp);
}
static struct wpe_view_backend *cur_vb(void) { return tab_vb(B.cur); }

/* ---- the page: buffers for flcomp ---------------------------------------- */

static void sb_release(void *d, struct wl_buffer *b)
{
	struct sbuf *s = d;
	s->busy = 0;
}
static const struct wl_buffer_listener sb_l = { sb_release };

static struct sbuf *sbuf_get(int w, int h)
{
	struct sbuf *free_slot = NULL;
	for (int i = 0; i < MAX_SBUF; i++) {
		struct sbuf *s = &B.sb[i];
		if (s->b && !s->busy && (s->w != w || s->h != h)) {	/* old size: drop it */
			if (B.last == s)
				B.last = NULL;
			wl_buffer_destroy(s->b);
			munmap(s->map, s->size);
			memset(s, 0, sizeof(*s));
		}
		if (s->b && !s->busy && s->w == w && s->h == h)
			return s;
		if (!s->b && !free_slot)
			free_slot = s;
	}
	if (!free_slot)
		return NULL;
	size_t size = (size_t)w * h * 4;
	int fd = memfd_create("freebws", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, (off_t)size)) {
		if (fd >= 0)
			close(fd);
		return NULL;
	}
	void *m = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED) {
		close(fd);
		return NULL;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(B.shm, fd, (int32_t)size);
	free_slot->b = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	free_slot->map = m;
	free_slot->size = size;
	free_slot->w = w;
	free_slot->h = h;
	wl_buffer_add_listener(free_slot->b, &sb_l, free_slot);
	return free_slot;
}

struct wl_shm_buffer;
const void *fbshm_begin(struct wl_shm_buffer *b, int *w, int *h, int *stride);
void fbshm_end(struct wl_shm_buffer *b);

/* put a frame on screen: pixels with the given stride (bytes) */
static void show_pixels(const uint8_t *p, int w, int h, int stride)
{
	struct sbuf *s = sbuf_get(w, h);
	if (!s)
		return;
	if (stride == w * 4)
		memcpy(s->map, p, (size_t)w * h * 4);
	else
		for (int y = 0; y < h; y++)
			memcpy((uint8_t *)s->map + (size_t)y * w * 4, p + (size_t)y * stride, (size_t)w * 4);
	s->busy = 1;
	B.last = s;
	wl_surface_attach(B.page, s->b, 0, 0);
	wl_surface_damage_buffer(B.page, 0, 0, w, h);
	wl_surface_commit(B.page);
	B.frames++;
}

static void export_shm(void *data, struct wpe_fdo_shm_exported_buffer *eb)
{
	struct tab *t = data;
	struct wl_shm_buffer *src = wpe_fdo_shm_exported_buffer_get_shm_buffer(eb);
	if (t == B.cur) {	/* a background tab's frames are dropped */
		int w, h, stride;
		const uint8_t *p = fbshm_begin(src, &w, &h, &stride);
		show_pixels(p, w, h, stride);
		fbshm_end(src);
	}
	wpe_view_backend_exportable_fdo_dispatch_release_shm_exported_buffer(t->exp, eb);
	wpe_view_backend_exportable_fdo_dispatch_frame_complete(t->exp);
}

struct dmb_release { struct wpe_view_backend_exportable_fdo *exp; struct wl_resource *res; };

static void dmb_buffer_release(void *d, struct wl_buffer *b)
{
	struct dmb_release *r = d;
	if (g_hash_table_contains(B.live_exp, r->exp))	/* its tab may be gone */
		wpe_view_backend_exportable_fdo_dispatch_release_buffer(r->exp, r->res);
	wl_buffer_destroy(b);
	free(r);
}
static const struct wl_buffer_listener dmb_l = { dmb_buffer_release };

static void export_dmabuf(void *data, struct wpe_view_backend_exportable_fdo_dmabuf_resource *r)
{
	struct tab *t = data;
	if (t != B.cur) {	/* a background tab: give the buffer straight back */
		wpe_view_backend_exportable_fdo_dispatch_release_buffer(t->exp, r->buffer_resource);
		wpe_view_backend_exportable_fdo_dispatch_frame_complete(t->exp);
		return;
	}
	struct zwp_linux_buffer_params_v1 *pa = zwp_linux_dmabuf_v1_create_params(B.dmabuf);
	for (int i = 0; i < r->n_planes; i++)
		zwp_linux_buffer_params_v1_add(pa, r->fds[i], i, r->offsets[i], r->strides[i],
					       (uint32_t)(r->modifiers[i] >> 32),
					       (uint32_t)(r->modifiers[i] & 0xffffffffu));
	struct wl_buffer *b = zwp_linux_buffer_params_v1_create_immed(pa, (int32_t)r->width,
								       (int32_t)r->height, r->format, 0);
	zwp_linux_buffer_params_v1_destroy(pa);
	struct dmb_release *rel = calloc(1, sizeof(*rel));
	if (!rel) {
		wl_buffer_destroy(b);
		return;
	}
	rel->exp = t->exp;
	rel->res = r->buffer_resource;
	wl_buffer_add_listener(b, &dmb_l, rel);
	wl_surface_attach(B.page, b, 0, 0);
	wl_surface_damage_buffer(B.page, 0, 0, (int32_t)r->width, (int32_t)r->height);
	wl_surface_commit(B.page);
	B.frames++;
	wpe_view_backend_exportable_fdo_dispatch_frame_complete(t->exp);
}

static const struct wpe_view_backend_exportable_fdo_client fdo_client = {
	.export_dmabuf_resource = export_dmabuf, .export_shm_buffer = export_shm,
};

static void exp_destroy(gpointer exp)
{
	g_hash_table_remove(B.live_exp, exp);
	wpe_view_backend_exportable_fdo_destroy(exp);
}

/* ---- input on the page ---------------------------------------------------- */

static uint32_t wpe_mods(int m)
{
	return (m & FL_MOD_CTRL ? wpe_input_keyboard_modifier_control : 0) |
	       (m & FL_MOD_SHIFT ? wpe_input_keyboard_modifier_shift : 0) |
	       (m & FL_MOD_ALT ? wpe_input_keyboard_modifier_alt : 0) |
	       (m & FL_MOD_SUPER ? wpe_input_keyboard_modifier_meta : 0);
}

static void focus_page(int on)
{
	B.page_focus = on;
	struct wpe_view_backend *vb = cur_vb();
	if (on) {
		wpe_view_backend_add_activity_state(vb, wpe_view_activity_state_focused);
		fl_win_ui(B.win)->focus = 0;	/* the address bar lets go of the keyboard */
		fl_win_redraw(B.win);
	} else {
		wpe_view_backend_remove_activity_state(vb, wpe_view_activity_state_focused);
	}
}

static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x,
		    wl_fixed_t y)
{
	B.in_page = s == B.page;
	B.px = wl_fixed_to_double(x);
	B.py = wl_fixed_to_double(y);
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
	if (s == B.page)
		B.in_page = 0;
}
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
	if (!B.in_page || !B.cur)
		return;
	B.px = wl_fixed_to_double(x);
	B.py = wl_fixed_to_double(y);
	struct wpe_input_pointer_event ev = { wpe_input_pointer_event_type_motion, t, (int)B.px, (int)B.py, 0, 0,
					      wpe_mods(fl_app_mods(B.app)) };
	wpe_view_backend_dispatch_pointer_event(cur_vb(), &ev);
}
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t button, uint32_t state)
{
	if (!B.in_page || !B.cur)
		return;
	if (state == WL_POINTER_BUTTON_STATE_PRESSED)
		pop_close_hook();
	if (state == WL_POINTER_BUTTON_STATE_PRESSED && !B.page_focus)
		focus_page(1);
	uint32_t b = button == BTN_LEFT_ ? 1 : button == BTN_RIGHT_ ? 3 : button == BTN_MIDDLE_ ? 2 : 0;
	if (!b)
		return;
	struct wpe_input_pointer_event ev = { wpe_input_pointer_event_type_button, t, (int)B.px, (int)B.py, b,
					      state == WL_POINTER_BUTTON_STATE_PRESSED, wpe_mods(fl_app_mods(B.app)) };
	wpe_view_backend_dispatch_pointer_event(cur_vb(), &ev);
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t v)
{
	if (!B.in_page || !B.cur)
		return;
	/* one wheel notch is 10 in wl_fixed units; WPE wants 120 per notch, up positive */
	struct wpe_input_axis_event ev = { wpe_input_axis_event_type_motion, t, (int)B.px, (int)B.py, axis,
					   (int32_t)(-wl_fixed_to_double(v) * 12), wpe_mods(fl_app_mods(B.app)) };
	wpe_view_backend_dispatch_axis_event(cur_vb(), &ev);
}
static void p_frame(void *d, struct wl_pointer *p) { }
static void p_axis_source(void *d, struct wl_pointer *p, uint32_t s) { }
static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { }
static void p_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { }
static const struct wl_pointer_listener ptr_l = {
	.enter = p_enter, .leave = p_leave, .motion = p_motion, .button = p_button, .axis = p_axis,
	.frame = p_frame, .axis_source = p_axis_source, .axis_stop = p_axis_stop,
	.axis_discrete = p_axis_discrete,
};

/* ---- tabs ------------------------------------------------------------------ */

static void url_show(const char *uri);
static void tab_signals(struct tab *t);
static void dssl_setup_view(struct tab *t);
static void settings_apply(WebKitSettings *st);

static int tab_index(struct tab *t)
{
	for (int i = 0; i < B.ntabs; i++)
		if (B.tabs[i] == t)
			return i;
	return -1;
}

/* what the tab row and the window title show for a tab */
static void tab_label(const struct tab *t, char *out, size_t cap)
{
	const char *u = t->uri;
	if (!u[0] || !strcmp(u, B.home)) {
		g_strlcpy(out, "New tab", cap);
	} else if (t->title[0]) {
		g_strlcpy(out, t->title, cap);
	} else {
		const char *p = strstr(u, "://");
		g_strlcpy(out, p ? p + 3 : u, cap);
	}
}

static void title_update(void)
{
	char lab[300], buf[330];
	if (!B.cur)
		return;
	tab_label(B.cur, lab, sizeof lab);
	if (strcmp(lab, "New tab"))
		snprintf(buf, sizeof buf, "%s - FreeBWS", lab);
	else
		snprintf(buf, sizeof buf, "FreeBWS");
	fl_win_set_title(B.win, buf);
}

static gboolean spin_cb(gpointer d)
{
	int any = 0;
	for (int i = 0; i < B.ntabs; i++)
		any |= B.tabs[i]->loading;
	if (!any) {
		B.spin_src = 0;
		fl_win_redraw(B.win);
		return G_SOURCE_REMOVE;
	}
	B.spin = (B.spin + 30) % 360;
	fl_win_redraw(B.win);
	return G_SOURCE_CONTINUE;
}

static void spin_start(void)
{
	if (!B.spin_src)
		B.spin_src = g_timeout_add(80, spin_cb, NULL);
}

static void sync_ui(void)
{
	fl_ui *ui = fl_win_ui(B.win);
	if (B.cur && B.cur->uri[0] && ui->focus != ID_URL)
		url_show(B.cur->uri);
	fl_win_redraw(B.win);
}

/* create the web view and its backend; the tab must not be realized already.
 * With B.share_process every view is related to another live one, and WebKit puts
 * related views in one web process. */
static void tab_realize(struct tab *t, WebKitWebView *related)
{
	if (!related && B.share_process)
		for (int i = 0; i < B.ntabs && !related; i++)
			if (B.tabs[i]->view && B.tabs[i] != t)
				related = B.tabs[i]->view;
	int w = B.page_rect.w > 1 ? B.page_rect.w : 1024, h = B.page_rect.h > 1 ? B.page_rect.h : 600;
	t->exp = wpe_view_backend_exportable_fdo_create(&fdo_client, t, (uint32_t)w, (uint32_t)h);
	g_hash_table_add(B.live_exp, t->exp);
	t->w = w;
	t->h = h;
	struct wpe_view_backend *vb = tab_vb(t);
	wpe_view_backend_add_activity_state(vb, wpe_view_activity_state_in_window);
	WebKitWebViewBackend *wb = webkit_web_view_backend_new(vb, exp_destroy, t->exp);
	t->ucm = webkit_user_content_manager_new();
	dssl_setup_view(t);
	pages_setup_view(t->ucm, t);
	webkit_user_content_manager_register_script_message_handler(t->ucm, "fbcount", NULL);
	g_signal_connect(t->ucm, "script-message-received::fbcount", G_CALLBACK(on_count), t);
	t->view = g_object_new(WEBKIT_TYPE_WEB_VIEW, "backend", wb, "web-context", B.ctx, "user-content-manager", t->ucm,
			       "related-view", related, NULL);
	/* WebKit features are applied to the finished view: set at construction they do not
	 * reach the process pool (measured: the process model stays at its default) */
	settings_apply(webkit_web_view_get_settings(t->view));
	tab_signals(t);
	adb_tab_init(&t->adb, t->ucm);
}

/* destroy the web view (and with it the backend); the tab keeps its address and title */
static void tab_unrealize(struct tab *t)
{
	if (!t->view)
		return;
	g_signal_handlers_disconnect_by_data(t->view, t);
	g_signal_handlers_disconnect_by_data(t->ucm, t);
	adb_tab_free(&t->adb, t->ucm);
	if (t->dark_script) {
		webkit_user_script_unref(t->dark_script);
		t->dark_script = NULL;
	}
	t->dark_on = 0;
	t->dark_host[0] = 0;
	g_object_unref(t->view);
	g_object_unref(t->ucm);
	t->view = NULL;
	t->ucm = NULL;
	t->exp = NULL;
	t->gating = 0;		/* the held navigation went with the ad blocker's state: it is run again when selected */
}

static void snap_store(struct tab *t)
{
	if (B.use_dmabuf || !B.last || !B.last->b)
		return;
	size_t n = (size_t)B.last->w * B.last->h * 4;
	if (t->snap_w != B.last->w || t->snap_h != B.last->h) {
		free(t->snap);
		t->snap = malloc(n);
		t->snap_w = B.last->w;
		t->snap_h = B.last->h;
	}
	if (t->snap)
		memcpy(t->snap, B.last->map, n);
}

static void tab_free(struct tab *t)
{
	tab_unrealize(t);
	if (t->show_src)
		g_source_remove(t->show_src);
	if (t->tls_cert)
		g_object_unref(t->tls_cert);
	if (t->saved)
		webkit_web_view_session_state_unref(t->saved);
	free(t->snap);
	free(t);
}

static void tab_activate(struct tab *t)
{
	struct tab *old = B.cur;
	if (t == old)
		return;
	pop_close_hook();
	if (old) {
		snap_store(old);
		old->last_active = now_ms();
		if (old->view)
			wpe_view_backend_remove_activity_state(tab_vb(old), wpe_view_activity_state_visible |
									    wpe_view_activity_state_focused);
	}
	B.cur = t;
	if (t->discarded) {
		tab_realize(t, NULL);
		t->discarded = 0;
		WebKitBackForwardListItem *item = NULL;
		if (t->saved) {
			webkit_web_view_restore_session_state(t->view, t->saved);
			item = webkit_back_forward_list_get_current_item(webkit_web_view_get_back_forward_list(t->view));
			webkit_web_view_session_state_unref(t->saved);
			t->saved = NULL;
		}
		if (item)
			webkit_web_view_go_to_back_forward_list_item(t->view, item);
		else if (t->uri[0])
			load_uri(t, t->uri);
	} else if (t->reload) {
		t->reload = 0;
		webkit_web_view_reload(t->view);
	}
	struct wpe_view_backend *vb = tab_vb(t);
	wpe_view_backend_add_activity_state(vb, wpe_view_activity_state_visible | wpe_view_activity_state_in_window);
	if (B.page_focus)
		wpe_view_backend_add_activity_state(vb, wpe_view_activity_state_focused);
	if (B.page_rect.w > 0 && (t->w != B.page_rect.w || t->h != B.page_rect.h)) {
		t->w = B.page_rect.w;
		t->h = B.page_rect.h;
		wpe_view_backend_dispatch_set_size(vb, (uint32_t)t->w, (uint32_t)t->h);
	}
	/* until the page paints, show what the tab looked like, or nothing */
	if (!B.use_dmabuf) {
		if (t->snap) {
			show_pixels(t->snap, t->snap_w, t->snap_h, t->snap_w * 4);
		} else {
			wl_surface_attach(B.page, NULL, 0, 0);
			wl_surface_commit(B.page);
			B.last = NULL;
		}
	}
	url_show(t->uri);
	title_update();
	fl_win_redraw(B.win);
}

/* every slot is in use: the oldest background tab that is not loading and is not the opener of the
 * new popup is closed (it stays in the closed list, Ctrl-Shift-T). 0 when there is no such tab. */
static void tab_close(struct tab *t);

static int make_room(WebKitWebView *related)
{
	struct tab *old = NULL;
	for (int i = 0; i < B.ntabs; i++) {
		struct tab *t = B.tabs[i];
		if (t == B.cur || t->loading || (related && t->view == related))
			continue;
		if (!old || (int32_t)(t->last_active - old->last_active) < 0)
			old = t;
	}
	if (!old)
		return 0;
	tab_close(old);
	return 1;
}

static struct tab *tab_create(WebKitWebView *related, const char *uri, int activate)
{
	if (B.ntabs >= MAX_TABS && !make_room(related)) {
		toast("Too many tabs: close one first");
		return NULL;
	}
	struct tab *t = calloc(1, sizeof(*t));
	if (!t)
		return NULL;
	t->last_active = now_ms();
	if (uri)
		g_strlcpy(t->uri, uri, sizeof t->uri);
	/* the new tab goes right of the active one */
	int at = B.cur ? tab_index(B.cur) + 1 : B.ntabs;
	for (int i = B.ntabs; i > at; i--)
		B.tabs[i] = B.tabs[i - 1];
	B.tabs[at] = t;
	B.ntabs++;
	tab_realize(t, related);
	if (uri)
		load_uri(t, uri);
	if (activate || !B.cur)
		tab_activate(t);	/* until then WebKit takes it for hidden */
	fl_win_redraw(B.win);
	return t;
}

static void tab_close(struct tab *t)
{
	int i = tab_index(t);
	if (i < 0)
		return;
	const char *u = t->view ? webkit_web_view_get_uri(t->view) : t->uri;
	if (u && *u && strcmp(u, B.home) && strncmp(u, "about:", 6)) {
		if (B.nclosed == MAX_CLOSED) {
			memmove(B.closed[0], B.closed[1], sizeof(B.closed[0]) * (MAX_CLOSED - 1));
			B.nclosed--;
		}
		g_strlcpy(B.closed[B.nclosed++], u, sizeof B.closed[0]);
	}
	struct tab *next = NULL;
	if (t == B.cur && B.ntabs > 1) {
		/* a popup closed by window.close() goes back to the page that opened it */
		if (t->opener && tab_index(t->opener) >= 0)
			next = t->opener;
		else
			next = B.tabs[i + 1 < B.ntabs ? i + 1 : i - 1];
	}
	for (int j = 0; j < B.ntabs; j++)
		if (B.tabs[j]->opener == t)
			B.tabs[j]->opener = NULL;
	for (int j = i; j + 1 < B.ntabs; j++)
		B.tabs[j] = B.tabs[j + 1];
	B.ntabs--;
	B.tabs[B.ntabs] = NULL;
	if (t == B.cur)
		B.cur = NULL;	/* the old tab is gone: tab_activate has nothing to store or hide */
	if (next)
		tab_activate(next);
	tab_free(t);
	if (!B.ntabs)
		g_main_loop_quit(B.loop);
	fl_win_redraw(B.win);
}

/* destroy a background tab's web view (and so its page, and its process when nothing else
 * lives in it) but keep the tab: the back/forward list is saved, selecting it reloads */
static void tab_discard(struct tab *t)
{
	if (!t->view || t == B.cur)
		return;
	if (t->saved)
		webkit_web_view_session_state_unref(t->saved);
	t->saved = webkit_web_view_get_session_state(t->view);
	const char *u = webkit_web_view_get_uri(t->view);
	if (u)
		g_strlcpy(t->uri, u, sizeof t->uri);
	tab_unrealize(t);
	t->discarded = 1;
	t->loading = 0;
	fl_win_redraw(B.win);
}

static void tab_select_rel(int d)
{
	if (B.ntabs < 2)
		return;
	int i = (tab_index(B.cur) + d + B.ntabs) % B.ntabs;
	tab_activate(B.tabs[i]);
}

/* what the tab row and other callbacks ask for runs from the main loop, not inside a draw pass or a
 * WebKit signal; a tab that is gone by then is ignored */
static gboolean idle_activate(gpointer p)
{
	if (tab_index(p) >= 0)
		tab_activate(p);
	return G_SOURCE_REMOVE;
}
static gboolean idle_close(gpointer p)
{
	if (tab_index(p) >= 0)
		tab_close(p);
	return G_SOURCE_REMOVE;
}

/* ---- keyboard ------------------------------------------------------------- */

static void focus_url(void)
{
	focus_page(0);
	fl_win_ui(B.win)->focus = ID_URL;
	B.sel_all = 1;
	fl_win_redraw(B.win);
}

static void new_tab_focus_url(void)
{
	if (tab_create(NULL, B.home, 1))
		focus_url();
}

static gboolean idle_newtab(gpointer p)
{
	new_tab_focus_url();
	return G_SOURCE_REMOVE;
}

static int on_key(fl_win *w, uint32_t keycode, uint32_t sym, const char *utf8, int mods, int pressed, void *ud)
{
	int ctrl = mods & FL_MOD_CTRL, shift = mods & FL_MOD_SHIFT;
	if (!B.cur)
		return 1;
	if (pressed && sym == XKB_KEY_Escape && B.pop_kind != POP_NONE) {
		pop_close();
		return 1;
	}
	if (pressed && ctrl) {
		if (!shift && (sym == 'd' || sym == 'D')) {
			bookmark_toggle();
			return 1;
		}
		if (shift && (sym == 'b' || sym == 'B')) {
			cfg_set_int("bookmarks_bar", !cfg_int("bookmarks_bar", 1));
			fl_win_redraw(B.win);
			return 1;
		}
		if (shift && (sym == 'o' || sym == 'O')) {
			open_in_tab("freebws://bookmarks", 1);
			return 1;
		}
		if (!shift && (sym == 'j' || sym == 'J')) {
			open_in_tab("freebws://downloads", 1);
			return 1;
		}
		if (sym == 't' || sym == 'T') {
			if (shift) {	/* Ctrl-Shift-T: the tab closed last */
				if (B.nclosed && tab_create(NULL, B.closed[B.nclosed - 1], 1))
					B.nclosed--;
			} else {
				new_tab_focus_url();
			}
			return 1;
		}
		if (sym == 'w' || sym == 'W' || sym == XKB_KEY_F4) {
			tab_close(B.cur);
			return 1;
		}
		if (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab || sym == XKB_KEY_Page_Down ||
		    sym == XKB_KEY_Page_Up) {
			tab_select_rel(sym == XKB_KEY_ISO_Left_Tab || sym == XKB_KEY_Page_Up || shift ? -1 : 1);
			return 1;
		}
	}
	if (pressed && ((mods & FL_MOD_ALT) || ctrl) && sym >= '1' && sym <= '9') {	/* the nth tab, 9: the last */
		int n = sym == '9' ? B.ntabs - 1 : (int)(sym - '1');
		if (n < B.ntabs)
			tab_activate(B.tabs[n]);
		return 1;
	}
	/* Ctrl-L, Alt-D and F6: into the address bar with everything selected */
	if (pressed && ((ctrl && (sym == 'l' || sym == 'L')) || ((mods & FL_MOD_ALT) && (sym == 'd' || sym == 'D')) ||
			sym == XKB_KEY_F6)) {
		focus_url();
		return 1;
	}
	/* Escape in the bar: the current address back; again: back to the page */
	if (pressed && sym == XKB_KEY_Escape && fl_win_ui(w)->focus == ID_URL) {
		char *cur = NULL;
		const char *uri = webkit_web_view_get_uri(B.cur->view);
		if (uri)
			cur = g_strdup(!strcmp(uri, B.home) ? "" : uri);
		if (!strcmp(B.url, cur ? cur : "")) {
			g_free(cur);
			if (B.cur->loading)
				webkit_web_view_stop_loading(B.cur->view);
			focus_page(1);
			return 1;
		}
		g_strlcpy(B.url, cur ? cur : "", sizeof B.url);
		g_free(cur);
		B.sel_all = 1;
		fl_win_redraw(w);
		return 1;
	}
	if (pressed && ((ctrl && (sym == 'r' || sym == 'R')) || sym == XKB_KEY_F5)) {
		webkit_web_view_reload(B.cur->view);
		return 1;
	}
	if (pressed && (mods & FL_MOD_ALT) && sym == XKB_KEY_Left) {
		webkit_web_view_go_back(B.cur->view);
		return 1;
	}
	if (pressed && (mods & FL_MOD_ALT) && sym == XKB_KEY_Right) {
		webkit_web_view_go_forward(B.cur->view);
		return 1;
	}
	if (!B.page_focus)
		return 0;
	struct wpe_input_keyboard_event ev = { now_ms(), sym, keycode + 8, pressed != 0, wpe_mods(mods) };
	wpe_view_backend_dispatch_keyboard_event(cur_vb(), &ev);
	return 1;
}

static void on_focus(fl_win *w, int focused, void *ud)
{
	if (!B.cur)
		return;
	struct wpe_view_backend *vb = cur_vb();
	if (focused && B.page_focus)
		wpe_view_backend_add_activity_state(vb, wpe_view_activity_state_focused);
	else if (!focused)
		wpe_view_backend_remove_activity_state(vb, wpe_view_activity_state_focused);
}

/* ---- toolbar -------------------------------------------------------------- */

static char *url_from_input(const char *in)
{
	while (*in == ' ')
		in++;
	size_t n = strlen(in);
	while (n && in[n - 1] == ' ')
		n--;
	if (!n)
		return NULL;
	char *s = g_strndup(in, n);
	const char *colon = strstr(s, "://");
	gboolean scheme = colon && colon > s;
	for (const char *p = s; scheme && p < colon; p++)
		if (!g_ascii_isalnum(*p) && *p != '+' && *p != '-' && *p != '.')
			scheme = FALSE;
	if (scheme || g_str_has_prefix(s, "about:") || g_str_has_prefix(s, "data:") ||
	    g_str_has_prefix(s, "file:"))
		return s;
	if (s[0] == '/') {
		char *r = g_strconcat("file://", s, NULL);
		g_free(s);
		return r;
	}
	/* a host: no space, a dot inside it (not first or last), or localhost; the
	 * port and path do not matter. Local names and IP addresses get http. */
	if (!strchr(s, ' ')) {
		size_t hl = strcspn(s, "/?#:");
		gboolean local = hl == 9 && !strncmp(s, "localhost", 9);
		gboolean ip = hl > 0;
		for (size_t i = 0; i < hl; i++)
			if (!g_ascii_isdigit(s[i]) && s[i] != '.')
				ip = FALSE;
		gboolean dot = hl > 2 && memchr(s, '.', hl) && s[0] != '.' && s[hl - 1] != '.';
		if (local || dot) {
			char *r = g_strconcat(local || ip ? "http://" : "https://", s, NULL);
			g_free(s);
			return r;
		}
	}
	char *r = search_url(s);
	g_free(s);
	return r;
}

static void reload_icon(fl_ui *ui, fl_rect r, fl_color c)
{
	fl_path *p = fl_path_new();
	float s = (float)r.h / 24.0f * 0.62f, ox = r.x + r.w * 0.19f, oy = r.y + r.h * 0.19f;
	/* a circular arrow: three quarters of a circle with an arrow head */
	fl_path_svg(p, "M12 3.5a8.5 8.5 0 1 0 8.5 8.5", s, s, ox, oy);
	fl_path_stroke(&ui->cv, p, 1.8f, c);
	fl_path_free(p);
	p = fl_path_new();
	fl_path_svg(p, "M17 2.5 L21 6.5 L16 8 Z", s, s, ox + 3 * s, oy + 8 * s);
	fl_path_fill(&ui->cv, p, c);
	fl_path_free(p);
}

static int nav_button(fl_ui *ui, uint32_t id, fl_rect r, int icon, int enabled)
{
	int st = fl_widget(ui, id, r, 0);
	fl_color c = enabled ? fl_th.text : fl_th.dim;
	if (enabled && (st & (FL_HOVER | FL_DOWN)))
		fl_fill_rect(&ui->cv, r, fl_th.raised);
	if (icon >= 0)
		fl_icon_draw(&ui->cv, (fl_icon)icon, r.x + (r.w - 18) / 2, r.y + (r.h - 18) / 2, 18, c);
	else
		reload_icon(ui, r, c);
	return enabled && (st & FL_CLICK);
}

/* ---- toolbar extras: star, shield, downloads, menu, popups, bookmarks bar ------------------------ */

static int host_in_list(const char *key, const char *host)
{
	char **v = g_strsplit(cfg_get(key, ""), ",", 0);
	int hit = 0;
	size_t hl = strlen(host);
	for (int i = 0; v[i]; i++) {
		size_t dl = strlen(v[i]);
		if (dl && hl >= dl && !strcmp(host + hl - dl, v[i]) && (hl == dl || host[hl - dl - 1] == '.'))
			hit = 1;
	}
	g_strfreev(v);
	return hit;
}

static char *uri_host(const char *uri)
{
	if (!uri || !(g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://")))
		return NULL;
	GUri *u = g_uri_parse(uri, G_URI_FLAGS_PARSE_RELAXED, NULL);
	char *h = u && g_uri_get_host(u) ? g_ascii_strdown(g_uri_get_host(u), -1) : NULL;
	if (u)
		g_uri_unref(u);
	return h;
}

static gboolean toast_clear(gpointer d)
{
	B.toast_src = 0;
	B.toast[0] = 0;
	fl_win_redraw(B.win);
	return G_SOURCE_REMOVE;
}

static void toast(const char *msg)
{
	g_strlcpy(B.toast, msg, sizeof B.toast);
	if (B.toast_src)
		g_source_remove(B.toast_src);
	B.toast_src = g_timeout_add(2600, toast_clear, NULL);
	fl_win_redraw(B.win);
}

/* ---- force dark mode ------------------------------------------------------------------------------ */
/* A document-start script for the pages that should be dark: it turns the page dark with a
 * filter (invert and rotate the hue back; images, video and canvas inverted a second time so they
 * look as they are) and takes it off again when the page is dark already (its background, a
 * color-scheme of only dark). Web pages only (http, https): file:// and freebws:// pages (the DSSL
 * test pages among them) are never touched. */
#define DARK_JS \
	"(function(){if(window.__fbDark)return;window.__fbDark=1;var D=document,st=null;" \
	"var css='html{filter:invert(.91) hue-rotate(180deg)!important}'+" \
	"'img,picture,video,canvas,svg image,embed,object,iframe[src*=\"youtube\"],iframe[src*=\"vimeo\"],iframe[src*=\"player\"],'+" \
	"'[style*=\"background-image\"],[style*=\"background:url\"],[style*=\"background: url\"]{filter:invert(1) hue-rotate(180deg)!important}';" \
	"function lum(c){var m=/rgba?\\(\\s*([\\d.]+)[ ,]+([\\d.]+)[ ,]+([\\d.]+)(?:[ ,\\/]+([\\d.]+%?))?/.exec(c);if(!m)return -1;" \
	"var a=m[4]===undefined?1:(m[4].slice(-1)=='%'?parseFloat(m[4])/100:+m[4]);if(a<0.15)return -1;return(0.2126*m[1]+0.7152*m[2]+0.0722*m[3])/255;}" \
	"function on(){if(st||!window.__fbDark)return;var p=D.head||D.documentElement;if(!p)return;st=D.createElement('style');st.id='fb-dark';st.textContent=css;p.appendChild(st);}" \
	"function off(){if(st){st.remove();st=null;}}" \
	"function dark(){var h=D.documentElement,b=D.body;if(!h)return false;" \
	"if(getComputedStyle(h).colorScheme==='dark'||(getComputedStyle(h).colorScheme||'').replace(/\\s/g,'')==='only dark')return true;" \
	"var m=D.querySelector('meta[name=color-scheme]');if(m&&/^\\s*(only\\s+)?dark\\s*$/.test(m.content))return true;" \
	"var l=b?lum(getComputedStyle(b).backgroundColor):-1;if(l<0)l=lum(getComputedStyle(h).backgroundColor);" \
	"return l>=0&&l<0.4;}" \
	"function paper(){var h=D.documentElement;if(h&&lum(getComputedStyle(h).backgroundColor)<0&&(!D.body||lum(getComputedStyle(D.body).backgroundColor)<0))h.style.setProperty('background','#fff','important');}" \
	"function check(){if(!window.__fbDark)return;if(dark()){off();}else{on();paper();}}" \
	"on();" \
	"D.addEventListener('DOMContentLoaded',check);window.addEventListener('load',function(){check();setTimeout(check,1200);});" \
	"})();"
#define DARK_OFF_JS "window.__fbDark=0;var s=document.getElementById('fb-dark');if(s)s.remove();document.documentElement.style.removeProperty('background');"

static void dark_navigate(struct tab *t, const char *uri)
{
	char *host = uri_host(uri);
	int want = 0;
	if (host)
		want = host_in_list("dark_on", host) ? 1 : host_in_list("dark_off", host) ? 0 : cfg_int("dark", 0);
	if (want == t->dark_on && (!want || !strcmp(host ? host : "", t->dark_host))) {
		g_free(host);
		return;
	}
	if (t->dark_script) {
		webkit_user_content_manager_remove_script(t->ucm, t->dark_script);
		webkit_user_script_unref(t->dark_script);
		t->dark_script = NULL;
	}
	if (want) {
		t->dark_script = webkit_user_script_new(DARK_JS, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
							WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
		webkit_user_content_manager_add_script(t->ucm, t->dark_script);
	}
	t->dark_on = want;
	g_strlcpy(t->dark_host, host ? host : "", sizeof t->dark_host);
	g_free(host);
}

/* what must be set up for the page a tab is about to show: the ad blocker's sheet, filters and
 * scriptlets for its host, force dark mode. Called before our own loads and when a main-frame
 * load starts or is redirected. Returns 1 when the load has to wait (first start: the filters are
 * still being made); a notice page stands in and fb_adb_changed goes on with the load. */
static int navigate_hook(struct tab *t, const char *uri)
{
	if (!t->view || !uri)
		return 0;
	if (t->gating) {
		/* a navigation waits for the filters: the newest address replaces the held one, so
		 * fb_adb_changed never loads a stale URL over the page the user is on. The same address
		 * (the notice page's own load) goes on as it is. */
		if (t->adb.deferred && !strcmp(t->adb.deferred, uri))
			return 0;
		g_free(t->adb.deferred);
		t->adb.deferred = g_strdup(uri);
		return 1;
	}
	if (adb_navigate(&t->adb, t->ucm, uri)) {
		t->gating = 1;
		if (!adb_building())	/* the compiled filters are only being loaded: a moment, no notice */
			return 1;
		static const char wait[] = "<!doctype html><meta charset=utf-8><title>One moment</title>"
			"<body style=\"background:#1d2125;color:#d8dce0;font:15px 'IBM Plex Sans',sans-serif;display:grid;place-items:center;min-height:100vh;margin:0\">"
			"<div style=\"text-align:center\"><div style=\"font-size:20px;font-weight:600;margin-bottom:6px\">Getting the ad blocker ready</div>"
			"<div style=\"color:#8a9199\">This happens once, on the first start. The page opens by itself.</div></div>";
		webkit_web_view_load_alternate_html(t->view, wait, uri, "freebws://wait/");
		return 1;
	}
	dark_navigate(t, uri);
	return 0;
}

static void load_uri(struct tab *t, const char *uri)
{
	if (!t || !t->view)
		return;
	if (navigate_hook(t, uri))
		return;
	webkit_web_view_load_uri(t->view, uri);
}

/* a setting or a per-site choice changed: every tab gets the new state, the page shown at once */
static void dark_refresh_all(void)
{
	for (int i = 0; i < B.ntabs; i++) {
		struct tab *t = B.tabs[i];
		if (!t->view)
			continue;
		int was = t->dark_on;
		t->dark_host[0] = '?';	/* force a new decision */
		t->dark_host[1] = 0;
		dark_navigate(t, t->uri);
		if (t->dark_on && !was)
			webkit_web_view_evaluate_javascript(t->view, DARK_JS, -1, NULL, NULL, NULL, NULL, NULL);
		else if (!t->dark_on && was)
			webkit_web_view_evaluate_javascript(t->view, DARK_OFF_JS, -1, NULL, NULL, NULL, NULL, NULL);
	}
}

static int dark_here(struct tab *t) { return t && t->dark_on; }

/* ---- popups ---------------------------------------------------------------------------------------- */

enum { A_NONE, A_NEWTAB, A_BOOKMARK, A_MANAGER, A_BAR, A_DLPAGE, A_DARK, A_SETTINGS, A_URL, A_URL_BG, A_DL_OPEN, A_DL_SHOW,
       A_DL_CANCEL, A_DL_RETRY, A_DL_CLEAR, A_ADB_SITE };

struct action { int code, id; char *url; };

static void pop_close(void)
{
	if (B.pop_kind == POP_NONE)
		return;
	B.pop_kind = POP_NONE;
	popup_hide(B.pop);
	fl_win_redraw(B.win);
}

static void pop_close_hook(void) { pop_close(); }

static void bookmark_toggle(void)
{
	struct tab *t = B.cur;
	if (!t || !t->uri[0] || !strncmp(t->uri, "freebws://", 10) || !strncmp(t->uri, "about:", 6)) {
		toast("This page cannot be bookmarked");
		return;
	}
	const struct bm *b = bm_find_url(t->uri);
	if (b) {
		bm_remove(b->id);
		toast("Bookmark removed");
	} else {
		char lab[300];
		tab_label(t, lab, sizeof lab);
		bm_add(lab, t->uri, 0);
		toast("Bookmark added to the bar");
	}
	fl_win_redraw(B.win);
}

static void open_in_tab(const char *url, int new_tab)
{
	if (new_tab || !B.cur) {
		tab_create(NULL, url, 1);
	} else {
		load_uri(B.cur, url);
		focus_page(1);
	}
}

static void do_action(struct action *a)
{
	switch (a->code) {
	case A_NEWTAB:
		new_tab_focus_url();
		break;
	case A_BOOKMARK:
		bookmark_toggle();
		break;
	case A_MANAGER:
		open_in_tab("freebws://bookmarks", 1);
		break;
	case A_BAR:
		cfg_set_int("bookmarks_bar", !cfg_int("bookmarks_bar", 1));
		fl_win_redraw(B.win);
		break;
	case A_DLPAGE:
		open_in_tab("freebws://downloads", 1);
		break;
	case A_SETTINGS:
		open_in_tab("freebws://settings", 1);
		break;
	case A_DARK:
		if (B.cur) {
			char *h = uri_host(B.cur->uri);
			if (h) {
				int now = B.cur->dark_on;
				cfg_list_set("dark_on", h, FALSE);
				cfg_list_set("dark_off", h, FALSE);
				/* the opposite of what the page has now; if that is the global setting, no entry */
				int glob = cfg_int("dark", 0);
				if ((!now) != glob)
					cfg_list_set(!now ? "dark_on" : "dark_off", h, TRUE);
				dark_refresh_all();
				g_free(h);
			}
		}
		break;
	case A_URL:
	case A_URL_BG:
		if (a->url)
			open_in_tab(a->url, a->code == A_URL_BG);
		break;
	case A_DL_OPEN:
		dl_open(a->id);
		break;
	case A_DL_SHOW:
		dl_show_folder(a->id);
		break;
	case A_DL_CANCEL:
		dl_cancel(a->id);
		break;
	case A_DL_RETRY:
		dl_retry(a->id);
		break;
	case A_DL_CLEAR:
		dl_clear_finished();
		break;
	case A_ADB_SITE:
		if (B.cur) {
			char *h = uri_host(B.cur->uri);
			if (h) {
				adb_set_site_off(h, !adb_site_off(h));
				webkit_web_view_reload(B.cur->view);
				g_free(h);
			}
		}
		break;
	}
}

static gboolean idle_action(gpointer p)
{
	struct action *a = p;
	do_action(a);
	g_free(a->url);
	g_free(a);
	return G_SOURCE_REMOVE;
}

/* ask for something to happen once the pass is drawn (never inside a draw pass) */
static void act(int code, int id, const char *url, int close)
{
	struct action *a = g_new0(struct action, 1);
	a->code = code;
	a->id = id;
	a->url = g_strdup(url);
	if (close)
		pop_close();
	g_idle_add(idle_action, a);
}

#define ROW_H 30

/* a row of a popup: hover fill, returns the widget state */
static int pop_row(fl_ui *ui, uint32_t id, fl_rect r)
{
	int st = fl_widget(ui, id, r, 0);
	if (st & (FL_HOVER | FL_DOWN))
		fl_fill_rect(&ui->cv, r, fl_th.raised);
	return st;
}

struct frow { const struct bm *b; int depth; };

static void folder_rows(int parent, int depth, GArray *out)
{
	GPtrArray *ch = bm_children(parent);
	for (guint i = 0; i < ch->len && out->len < 24; i++) {
		struct frow r = { g_ptr_array_index(ch, i), depth };
		g_array_append_val(out, r);
		if (r.b->folder && depth < 3)
			folder_rows(r.b->id, depth + 1, out);
	}
	g_ptr_array_free(ch, TRUE);
}

static GArray *folder_list(void)
{
	GArray *rows = g_array_new(FALSE, FALSE, sizeof(struct frow));
	if (B.pop_folder > 0) {
		folder_rows(B.pop_folder, 0, rows);
	} else {	/* the bar's overflow: the root items that did not fit */
		GPtrArray *root = bm_children(0);
		for (guint i = (guint)B.bar_from; i < root->len && rows->len < 24; i++) {
			struct frow r = { g_ptr_array_index(root, i), 0 };
			g_array_append_val(rows, r);
			if (r.b->folder)
				folder_rows(r.b->id, 1, rows);
		}
		g_ptr_array_free(root, TRUE);
	}
	return rows;
}

static void text_dim(fl_ui *ui, fl_rect r, const char *s, int align)
{
	fl_text_in(&ui->cv, ui->f_reg, r, align, 0, fl_th.dim, s);
}

static void draw_menu(fl_ui *ui, fl_rect r)
{
	struct tab *t = B.cur;
	char *host = t ? uri_host(t->uri) : NULL;
	int bm = t && bm_find_url(t->uri) != NULL;
	struct { const char *label, *key; int code; int check; } m[] = {
		{ "New tab", "Ctrl+T", A_NEWTAB, -1 },
		{ bm ? "Remove bookmark" : "Bookmark this page", "Ctrl+D", A_BOOKMARK, -1 },
		{ "Bookmarks", "Ctrl+Shift+O", A_MANAGER, -1 },
		{ "Show bookmarks bar", "Ctrl+Shift+B", A_BAR, cfg_int("bookmarks_bar", 1) },
		{ "Downloads", "Ctrl+J", A_DLPAGE, -1 },
		{ "Dark mode on this site", "", A_DARK, host ? dark_here(t) : -2 },
		{ NULL, NULL, 0, 0 },
		{ "Settings", "", A_SETTINGS, -1 },
	};
	int y = r.y + 4;
	for (guint i = 0; i < G_N_ELEMENTS(m); i++) {
		if (!m[i].label) {
			fl_fill_rect(&ui->cv, fl_r(r.x + 8, y + 4, r.w - 16, 1), fl_th.line);
			y += 9;
			continue;
		}
		fl_rect row = fl_r(r.x + 1, y, r.w - 2, ROW_H);
		int dis = m[i].check == -2;
		int st = dis ? 0 : pop_row(ui, 0x100 + i, row);
		fl_text_in(&ui->cv, ui->f_reg, fl_r(row.x + 38, row.y, row.w - 52, row.h), FL_LEFT, 0, dis ? fl_th.dim : fl_th.text, m[i].label);
		if (m[i].key[0])
			text_dim(ui, fl_r(row.x, row.y, row.w - 14, row.h), m[i].key, FL_RIGHT);
		if (m[i].check >= 0) {	/* a small square: filled when it is on */
			fl_rect b = fl_r(row.x + 14, row.y + (row.h - 12) / 2, 12, 12);
			fl_stroke_rect(&ui->cv, b, 1, m[i].check ? fl_th.accent : fl_th.dim);
			if (m[i].check)
				fl_fill_rect(&ui->cv, fl_inset(b, 3, 3), fl_th.accent);
		}
		if (st & FL_CLICK)
			act(m[i].code, 0, NULL, 1);
		y += ROW_H;
	}
	g_free(host);
}

static void fmt_num(unsigned n, char *out, size_t cap)
{
	char t[24];
	snprintf(t, sizeof t, "%u", n);
	size_t l = strlen(t), o = 0;
	for (size_t i = 0; i < l && o + 2 < cap; i++) {
		if (i && (l - i) % 3 == 0)
			out[o++] = ',';
		out[o++] = t[i];
	}
	out[o] = 0;
}

static void draw_shield(fl_ui *ui, fl_rect r)
{
	struct tab *t = B.cur;
	char *host = t ? uri_host(t->uri) : NULL;
	int y = r.y + 12;
	fl_text(&ui->cv, ui->f_semi, r.x + 14, y, fl_th.strong, "Ad blocker", -1);
	y += 28;
	if (!adb_enabled()) {
		fl_text(&ui->cv, ui->f_reg, r.x + 14, y, fl_th.dim, "Turned off in Settings.", -1);
		y += 24;
	} else if (!host) {
		fl_text(&ui->cv, ui->f_reg, r.x + 14, y, fl_th.dim, "Nothing to block on this page.", -1);
		y += 24;
	} else {
		int off = adb_site_off(host), on = !off;
		fl_rect row = fl_r(r.x + 14, y, r.w - 28, 26);
		fl_rect sw = fl_cut_right(&row, 44);
		char buf[300];
		fl_text_fit(ui->f_med, host, row.w - 8, buf, sizeof buf);
		fl_text_in(&ui->cv, ui->f_med, row, FL_LEFT, 0, fl_th.text, buf);
		if (fl_switch(ui, 0x200, sw, &on))
			act(A_ADB_SITE, 0, NULL, 1);
		y += 32;
		char line[160];
		if (off)
			snprintf(line, sizeof line, "Blocking is off on this site.");
		else
			snprintf(line, sizeof line, "%u blocked on this page", t->adb.blocked);
		fl_text(&ui->cv, ui->f_reg, r.x + 14, y, off ? fl_th.dim : fl_th.accent, line, -1);
		y += 24;
	}
	fl_fill_rect(&ui->cv, fl_r(r.x + 1, y + 2, r.w - 2, 1), fl_th.line);
	y += 12;
	char *js = adb_status_json();
	/* the numbers are in the JSON; read the two we show without a parser */
	const char *rp = strstr(js, "\"rules\":");
	unsigned rules = rp ? (unsigned)atoi(rp + 8) : 0;
	const char *up = strstr(js, "\"updated\":");
	long long upd = up ? atoll(up + 10) : 0;
	g_free(js);
	char info[200];
	if (rules) {
		char d[32] = "never";
		if (upd) {
			time_t tt = (time_t)upd;
			strftime(d, sizeof d, "%Y-%m-%d", localtime(&tt));
		}
		char nb[24];
		fmt_num(rules, nb, sizeof nb);
		if (upd)
			snprintf(info, sizeof info, "%s rules, lists updated %s", nb, d);
		else
			snprintf(info, sizeof info, "%s rules (lists of the package)", nb);
	} else {
		snprintf(info, sizeof info, adb_ready() ? "No filters loaded." : "Preparing the filters...");
	}
	fl_text(&ui->cv, ui->f_reg, r.x + 14, y, fl_th.dim, info, -1);
	y += 26;
	if (fl_button(ui, 0x201, fl_r(r.x + 14, y, 110, 26), "Settings", FL_BTN_OUTLINE))
		act(A_SETTINGS, 0, NULL, 1);
	g_free(host);
}

static void fmt_size(guint64 n, char *out, size_t cap)
{
	const char *u[] = { "B", "kB", "MB", "GB" };
	double v = (double)n;
	int i = 0;
	while (v >= 1000 && i < 3) {
		v /= 1000;
		i++;
	}
	if (i == 0)
		snprintf(out, cap, "%llu B", (unsigned long long)n);
	else
		snprintf(out, cap, v < 10 ? "%.1f %s" : "%.0f %s", v, u[i]);
}

#define DL_ROW_H 56

static void draw_downloads(fl_ui *ui, fl_rect r)
{
	int y = r.y + 12;
	fl_text(&ui->cv, ui->f_semi, r.x + 14, y, fl_th.strong, "Downloads", -1);
	y = r.y + 40;
	fl_fill_rect(&ui->cv, fl_r(r.x + 1, y - 1, r.w - 2, 1), fl_th.line);
	guint n = dl_count();
	if (!n) {
		fl_text_in(&ui->cv, ui->f_reg, fl_r(r.x, y, r.w, DL_ROW_H), FL_CENTER, 0, fl_th.dim, "Nothing downloaded yet");
		y += DL_ROW_H;
	}
	for (guint i = 0; i < n && i < 5; i++) {
		struct dl *d = dl_at(i);
		fl_rect row = fl_r(r.x + 1, y, r.w - 2, DL_ROW_H);
		int st = pop_row(ui, 0x300 + i, row);
		fl_rect btns = fl_cut_right(&row, 70);
		char nm[300], sub[160], a[32], b[32];
		fl_text_fit(ui->f_med, d->name, row.w - 28, nm, sizeof nm);
		fl_text(&ui->cv, ui->f_med, row.x + 14, row.y + 9, fl_th.text, nm, -1);
		fmt_size(d->got, a, sizeof a);
		fmt_size(d->total, b, sizeof b);
		if (d->state == DL_ACTIVE)
			snprintf(sub, sizeof sub, d->total ? "%s of %s" : "%s", a, b);
		else if (d->state == DL_DONE)
			snprintf(sub, sizeof sub, "%s - done", a);
		else if (d->state == DL_CANCELLED)
			snprintf(sub, sizeof sub, "Cancelled");
		else
			snprintf(sub, sizeof sub, "Failed%s%s", d->error ? ": " : "", d->error ? d->error : "");
		char sf[200];
		fl_text_fit(ui->f_reg, sub, row.w - 28, sf, sizeof sf);
		fl_text(&ui->cv, ui->f_reg, row.x + 14, row.y + 28, d->state >= DL_FAILED ? fl_th.err : fl_th.dim, sf, -1);
		if (d->state == DL_ACTIVE) {
			fl_rect bar = fl_r(row.x + 14, row.y + row.h - 8, row.w - 14, 3);
			fl_fill_rect(&ui->cv, bar, fl_th.bg);
			double f = d->total ? (double)d->got / (double)d->total : 0.05;
			fl_fill_rect(&ui->cv, fl_r(bar.x, bar.y, (int)(bar.w * (f > 1 ? 1 : f)), 3), fl_th.accent);
		}
		/* buttons on the right: cancel / retry, and the folder */
		fl_rect b1 = fl_r(btns.x + 6, btns.y + (btns.h - 28) / 2, 28, 28), b2 = fl_r(btns.x + 36, b1.y, 28, 28);
		int s1 = fl_widget(ui, 0x340 + i, b1, 0), s2 = fl_widget(ui, 0x350 + i, b2, 0);
		if (s1 & (FL_HOVER | FL_DOWN))
			fl_fill_rect(&ui->cv, b1, fl_th.line);
		if (s2 & (FL_HOVER | FL_DOWN))
			fl_fill_rect(&ui->cv, b2, fl_th.line);
		if (d->state == DL_ACTIVE)
			fl_icon_draw(&ui->cv, FL_ICON_CLOSE, b1.x + 8, b1.y + 8, 12, fl_th.text);
		else if (d->state != DL_DONE)
			reload_icon(ui, b1, fl_th.text);
		fl_icon_draw(&ui->cv, FL_ICON_FOLDER, b2.x + 6, b2.y + 6, 16, fl_th.text);
		if (s1 & FL_CLICK)
			act(d->state == DL_ACTIVE ? A_DL_CANCEL : A_DL_RETRY, d->id, NULL, 0);
		else if (s2 & FL_CLICK)
			act(A_DL_SHOW, d->id, NULL, 1);
		else if ((st & FL_CLICK) && d->state == DL_DONE)
			act(A_DL_OPEN, d->id, NULL, 1);
		y += DL_ROW_H;
	}
	fl_fill_rect(&ui->cv, fl_r(r.x + 1, y, r.w - 2, 1), fl_th.line);
	if (fl_button(ui, 0x360, fl_r(r.x + 14, y + 7, 130, 26), "All downloads", FL_BTN_OUTLINE))
		act(A_DLPAGE, 0, NULL, 1);
	if (fl_button(ui, 0x361, fl_r(r.x + r.w - 14 - 110, y + 7, 110, 26), "Clear list", FL_BTN_OUTLINE))
		act(A_DL_CLEAR, 0, NULL, 0);
}

static void draw_folder(fl_ui *ui, fl_rect r)
{
	GArray *rows = folder_list();
	int y = r.y + 4;
	if (!rows->len)
		fl_text_in(&ui->cv, ui->f_reg, fl_r(r.x, y, r.w, ROW_H - 2), FL_CENTER, 0, fl_th.dim, "Empty");
	for (guint i = 0; i < rows->len; i++) {
		struct frow fr = g_array_index(rows, struct frow, i);
		fl_rect row = fl_r(r.x + 1, y, r.w - 2, 28);
		int st = pop_row(ui, 0x400 + i, row);
		int x = row.x + 12 + fr.depth * 16;
		char buf[300];
		fl_color c = fr.b->folder ? fl_th.dim : fl_th.text;
		if (fr.b->folder)
			fl_icon_draw(&ui->cv, FL_ICON_FOLDER, x, row.y + 6, 16, fl_th.dim);
		int tx = x + (fr.b->folder ? 22 : 0);
		fl_text_fit(ui->f_reg, fr.b->title, row.x + row.w - tx - 10, buf, sizeof buf);
		fl_text(&ui->cv, ui->f_reg, tx, row.y + (row.h - fl_font_height(ui->f_reg)) / 2, c, buf, -1);
		if (!fr.b->folder && fr.b->url) {
			if (st & FL_CLICK)
				act(A_URL, 0, fr.b->url, 1);
			else if (ui->release & FL_BTN_M && (st & FL_HOVER))
				act(A_URL_BG, 0, fr.b->url, 0);
		}
		y += 28;
	}
	g_array_free(rows, TRUE);
}

static void pop_draw(fl_ui *ui, void *ud)
{
	fl_canvas *cv = &ui->cv;
	fl_rect r = fl_r(0, 0, cv->w, cv->h);
	fl_fill_rect(cv, r, fl_th.panel);
	switch (B.pop_kind) {
	case POP_MENU:
		draw_menu(ui, r);
		break;
	case POP_SHIELD:
		draw_shield(ui, r);
		break;
	case POP_DL:
		draw_downloads(ui, r);
		break;
	case POP_FOLDER:
		draw_folder(ui, r);
		break;
	}
	fl_stroke_rect(cv, r, 1, fl_th.line);
}

static void pop_size(int kind, int *w, int *h)
{
	switch (kind) {
	case POP_MENU:
		*w = 270;
		*h = 8 + 7 * ROW_H + 9;
		break;
	case POP_SHIELD: {
		char *h_ = B.cur ? uri_host(B.cur->uri) : NULL;
		*w = 330;
		*h = adb_enabled() && h_ ? 184 : 152;
		g_free(h_);
		break;
	}
	case POP_DL: {
		guint n = dl_count();
		*w = 390;
		*h = 40 + (n ? (n > 5 ? 5 : (int)n) : 1) * DL_ROW_H + 40;
		break;
	}
	default: {
		GArray *rows = folder_list();
		*w = 280;
		*h = 8 + (rows->len ? (int)rows->len : 1) * 28;
		g_array_free(rows, TRUE);
	}
	}
}

/* anchor: the button or bar item in window coordinates; popups hang under it, right aligned
 * for toolbar buttons (right_align) and left aligned for bookmark folders */
static void pop_open(int kind, fl_rect anchor, int folder, int right_align)
{
	if (!B.pop)
		return;
	int w, h;
	B.pop_kind = kind;
	B.pop_folder = folder;
	pop_size(kind, &w, &h);
	fl_rect c = fl_win_content(B.win);
	int x = right_align ? anchor.x + anchor.w - w : anchor.x;
	if (x + w > c.x + c.w - 4)
		x = c.x + c.w - 4 - w;
	if (x < c.x + 4)
		x = c.x + 4;
	B.pop_anchor = anchor;
	popup_show(B.pop, x, anchor.y + anchor.h, w, h);
	fl_win_redraw(B.win);
}

static void pop_toggle(int kind, fl_rect anchor, int folder, int right_align)
{
	if (B.pop_closed == kind) {	/* the press that is going on closed it: stay closed */
		B.pop_closed = POP_NONE;
		return;
	}
	if (B.pop_kind == kind && (kind != POP_FOLDER || B.pop_folder == folder)) {
		pop_close();
		return;
	}
	pop_open(kind, anchor, folder, right_align);
}

/* ---- toolbar icons ---------------------------------------------------------------------------------- */

static void star_icon(fl_ui *ui, fl_rect r, fl_color c, int filled)
{
	fl_path *p = fl_path_new();
	float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f + 0.5f, R = 8.6f;
	for (int i = 0; i < 10; i++) {
		float a = -3.14159265f / 2 + i * 3.14159265f / 5, rr = i % 2 ? R * 0.43f : R;
		float x = cx + rr * cosf(a), y = cy + rr * sinf(a);
		if (i)
			fl_path_line(p, x, y);
		else
			fl_path_move(p, x, y);
	}
	fl_path_close(p);
	if (filled)
		fl_path_fill(&ui->cv, p, c);
	else
		fl_path_stroke(&ui->cv, p, 1.5f, c);
	fl_path_free(p);
}

static void shield_icon(fl_ui *ui, fl_rect r, fl_color c, int filled, int slash)
{
	float s = 18.0f / 24.0f, ox = r.x + (r.w - 18) / 2.0f, oy = r.y + (r.h - 18) / 2.0f;
	fl_path *p = fl_path_new();
	fl_path_svg(p, "M12 2.5 L20 5.5 V11.5 C20 16.5 16.5 20.5 12 22 C7.5 20.5 4 16.5 4 11.5 V5.5 Z", s, s, ox, oy);
	if (filled)
		fl_path_fill(&ui->cv, p, c);
	else
		fl_path_stroke(&ui->cv, p, 1.6f, c);
	fl_path_free(p);
	if (slash) {
		p = fl_path_new();
		fl_path_move(p, ox + 3 * s, oy + 21 * s);
		fl_path_line(p, ox + 21 * s, oy + 3 * s);
		fl_path_stroke(&ui->cv, p, 1.8f, c);
		fl_path_free(p);
	}
}

static void download_icon(fl_ui *ui, fl_rect r, fl_color c, double progress, int running)
{
	float s = 16.0f / 24.0f, ox = r.x + (r.w - 16) / 2.0f, oy = r.y + (r.h - 16) / 2.0f;
	fl_path *p = fl_path_new();
	fl_path_svg(p, "M12 3 V15 M6.5 9.5 L12 15 L17.5 9.5 M4.5 20 H19.5", s, s, ox, oy);
	fl_path_stroke(&ui->cv, p, 1.7f, c);
	fl_path_free(p);
	if (running) {	/* the ring: a track, and the part that is done */
		float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f, R = 14.0f;
		p = fl_path_new();
		for (int i = 0; i <= 48; i++) {
			float a = i * 3.14159265f * 2 / 48;
			if (i)
				fl_path_line(p, cx + R * cosf(a), cy + R * sinf(a));
			else
				fl_path_move(p, cx + R, cy);
		}
		fl_path_stroke(&ui->cv, p, 1.5f, fl_th.line);
		fl_path_free(p);
		int n = (int)(progress * 48);
		if (progress < 0)
			n = 0;
		if (n > 0) {
			p = fl_path_new();
			for (int i = 0; i <= n; i++) {
				float a = -3.14159265f / 2 + i * 3.14159265f * 2 / 48;
				if (i)
					fl_path_line(p, cx + R * cosf(a), cy + R * sinf(a));
				else
					fl_path_move(p, cx + R * cosf(a), cy + R * sinf(a));
			}
			fl_path_stroke(&ui->cv, p, 2.0f, fl_th.accent);
			fl_path_free(p);
		} else {	/* size not known yet: a short arc that turns */
			p = fl_path_new();
			for (int i = 0; i <= 12; i++) {
				float a = (float)(B.spin) * 3.14159265f / 180.0f + i * 3.14159265f / 24;
				if (i)
					fl_path_line(p, cx + R * cosf(a), cy + R * sinf(a));
				else
					fl_path_move(p, cx + R * cosf(a), cy + R * sinf(a));
			}
			fl_path_stroke(&ui->cv, p, 2.0f, fl_th.accent);
			fl_path_free(p);
		}
	}
}

static int tool_button(fl_ui *ui, uint32_t id, fl_rect r, int active)
{
	int st = fl_widget(ui, id, r, 0);
	if (active)
		fl_fill_rect(&ui->cv, r, fl_th.raised);
	else if (st & (FL_HOVER | FL_DOWN))
		fl_fill_rect(&ui->cv, r, fl_th.raised);
	return st & FL_CLICK;
}

/* ---- the bookmarks bar ----------------------------------------------------------------------------- */

static void draw_bookmarks_bar(fl_ui *ui, fl_rect bar)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, bar, fl_th.bg);
	fl_fill_rect(cv, fl_r(bar.x, bar.y + bar.h - 1, bar.w, 1), fl_th.line);
	GPtrArray *root = bm_children(0);
	int x = bar.x + 8, right = bar.x + bar.w - 8;
	B.bar_from = -1;
	if (!root->len) {
		fl_text_in(cv, ui->f_reg, fl_r(x, bar.y, bar.w - 16, bar.h), FL_LEFT, 0, fl_th.dim,
			   "Bookmarks bar: press Ctrl+D on a page to put it here");
	}
	for (guint i = 0; i < root->len; i++) {
		const struct bm *b = g_ptr_array_index(root, i);
		char buf[300];
		fl_text_fit(ui->f_reg, b->title, 150, buf, sizeof buf);
		int tw = fl_text_width(ui->f_reg, buf, -1);
		int w = tw + (b->folder ? 36 : 22);
		if (x + w > right - 28 && i + 1 < root->len) {	/* the rest goes into the overflow */
			B.bar_from = (int)i;
			break;
		}
		if (x + w > right && B.bar_from < 0) {
			B.bar_from = (int)i;
			break;
		}
		fl_rect it = fl_r(x, bar.y + 3, w, bar.h - 7);
		int st = fl_widget(ui, ID_BAR_ITEM + i, it, 0);
		int open_here = B.pop_kind == POP_FOLDER && B.pop_folder == b->id;
		if (open_here || (st & (FL_HOVER | FL_DOWN)))
			fl_fill_rect(cv, it, fl_th.raised);
		int tx = it.x + 11;
		if (b->folder) {
			fl_icon_draw(cv, FL_ICON_FOLDER, it.x + 7, it.y + (it.h - 16) / 2, 16, fl_th.dim);
			tx = it.x + 29;
		}
		fl_text(cv, ui->f_reg, tx, it.y + (it.h - fl_font_height(ui->f_reg)) / 2, fl_th.text, buf, -1);
		if (st & FL_CLICK) {
			if (b->folder)
				pop_toggle(POP_FOLDER, fl_r(it.x, bar.y, it.w, bar.h), b->id, 0);
			else
				act(A_URL, 0, b->url, 1);
		} else if ((ui->release & FL_BTN_M) && (st & FL_HOVER) && !b->folder) {
			act(A_URL_BG, 0, b->url, 0);
		}
		x += w + 2;
	}
	if (B.bar_from >= 0) {	/* the chevron */
		fl_rect it = fl_r(bar.x + bar.w - 30, bar.y + 3, 26, bar.h - 7);
		int st = fl_widget(ui, ID_BAR_MORE, it, 0);
		int open_here = B.pop_kind == POP_FOLDER && B.pop_folder < 0;
		if (open_here || (st & (FL_HOVER | FL_DOWN)))
			fl_fill_rect(cv, it, fl_th.raised);
		fl_icon_draw(cv, FL_ICON_FORWARD, it.x + 5, it.y + (it.h - 16) / 2, 16, fl_th.text);
		if (st & FL_CLICK)
			pop_toggle(POP_FOLDER, fl_r(bar.x + bar.w - 280, bar.y, 280, bar.h), -1, 0);
	}
	g_ptr_array_free(root, TRUE);
}

static void draw(fl_ui *ui, void *ud)
{
	fl_canvas *cv = &ui->cv;
	fl_rect c = fl_win_content(ui->win);
	fl_fill_rect(cv, c, fl_th.bg);
	struct tab *cur = B.cur;
	if (!cur)
		return;

	/* a press outside the open popup closes it (on its own button: stays closed after the click) */
	if (B.pop_kind != POP_NONE && (ui->press & FL_BTN_L) && !fl_in(popup_rect(B.pop), ui->mx, ui->my)) {
		B.pop_closed = fl_in(B.pop_anchor, ui->mx, ui->my) ? B.pop_kind : POP_NONE;
		pop_close();
	}

	/* the tab row */
	fl_rect tr = fl_cut_top(&c, TABS_H);
	struct ts_item it[MAX_TABS];
	char lab[MAX_TABS][300];
	for (int i = 0; i < B.ntabs; i++) {
		tab_label(B.tabs[i], lab[i], sizeof lab[i]);
		it[i] = (struct ts_item){ lab[i], B.tabs[i]->loading, B.tabs[i]->discarded };
	}
	struct ts_event ev = ts_draw(ui, tr, it, B.ntabs, tab_index(cur), B.spin);
	if (B.toast[0]) {	/* a short notice at the right end of the row */
		int tw = fl_text_width(ui->f_med, B.toast, -1);
		fl_rect tb_ = fl_r(tr.x + tr.w - tw - 22, tr.y + 5, tw + 16, tr.h - 8);
		fl_fill_rect(cv, tb_, fl_th.panel);
		fl_stroke_rect(cv, tb_, 1, fl_th.accent);
		fl_text_in(cv, ui->f_med, tb_, FL_CENTER, 0, fl_th.text, B.toast);
	}

	fl_rect tb = fl_cut_top(&c, TOOLBAR_H);
	fl_fill_rect(cv, tb, fl_th.panel);
	fl_fill_rect(cv, fl_r(tb.x, tb.y + tb.h - 1, tb.w, 1), fl_th.line);

	fl_rect row = fl_inset(tb, 8, 6);
	int bw = 32;
	if (nav_button(ui, ID_BACK, fl_cut_left(&row, bw), FL_ICON_BACK, webkit_web_view_can_go_back(cur->view)))
		webkit_web_view_go_back(cur->view);
	if (nav_button(ui, ID_FWD, fl_cut_left(&row, bw), FL_ICON_FORWARD, webkit_web_view_can_go_forward(cur->view)))
		webkit_web_view_go_forward(cur->view);
	if (nav_button(ui, ID_RELOAD, fl_cut_left(&row, bw), -1, 1)) {
		if (cur->loading)
			webkit_web_view_stop_loading(cur->view);
		else
			webkit_web_view_reload(cur->view);
	}
	fl_cut_left(&row, 6);
	/* the right end: menu, downloads, shield (with the count), bookmark star */
	char *hst = uri_host(cur->uri);
	int web = hst != NULL;
	int shield_on = adb_enabled() && web && !adb_site_off(hst);
	char cnt[16] = "";
	if (shield_on && cur->adb.blocked)
		snprintf(cnt, sizeof cnt, "%u", cur->adb.blocked > 999 ? 999 : cur->adb.blocked);
	int cw = cnt[0] ? fl_text_width(ui->f_semi, cnt, -1) + 4 : 0;
	fl_rect r_menu = fl_cut_right(&row, 34), r_dl = fl_cut_right(&row, 34), r_sh = fl_cut_right(&row, 34 + cw),
		r_star = fl_cut_right(&row, 34);
	fl_cut_right(&row, 6);
	int had = B.url_focused;
	int ch = fl_textfield(ui, ID_URL, row, B.url, sizeof B.url, "Search or enter address", ui->f_reg);
	int has = ui->focus == ID_URL;
	if (has && !had) {	/* the bar just got the keyboard (click, Tab, shortcut): select all */
		B.sel_all = 1;
		B.sel_hold = ui->btn_held & FL_BTN_L;
	}
	if (has && (B.sel_all || B.sel_hold)) {
		int len = (int)strlen(B.url);
		if (B.sel_all || ui->_sel != 0 || ui->_cur != len) {
			ui->_sel = 0;
			ui->_cur = len;
			ui->_repass = 1;	/* draw it once more with the selection */
		}
		B.sel_all = 0;
		if (!(ui->btn_held & FL_BTN_L))
			B.sel_hold = 0;
	}
	if (!has && had) {	/* left without Enter: show the page's address again */
		if (!B.url_commit)
			url_show(cur->uri);
		B.url_commit = 0;
	}
	B.url_focused = has;
	if (ch & FL_EDIT_ENTER) {
		char *u = url_from_input(B.url);
		if (u) {
			load_uri(cur, u);
			g_strlcpy(B.url, u, sizeof B.url);
			g_free(u);
			B.url_commit = 1;
			focus_page(1);
		}
	}
	{
		int bm = bm_find_url(cur->uri) != NULL;
		if (tool_button(ui, ID_STAR, r_star, 0))
			bookmark_toggle();
		star_icon(ui, r_star, bm ? fl_th.accent : fl_th.text, bm);
		if (tool_button(ui, ID_SHIELD, r_sh, B.pop_kind == POP_SHIELD))
			pop_toggle(POP_SHIELD, r_sh, 0, 1);
		shield_icon(ui, fl_r(r_sh.x, r_sh.y, 34, r_sh.h), shield_on ? fl_th.accent : fl_th.dim, shield_on, adb_enabled() && web && !shield_on);
		if (cnt[0])
			fl_text(cv, ui->f_semi, r_sh.x + 30, r_sh.y + (r_sh.h - fl_font_height(ui->f_semi)) / 2, fl_th.accent, cnt, -1);
		double prog = dl_progress();
		B.dl_btn = r_dl;
		if (tool_button(ui, ID_DL, r_dl, B.pop_kind == POP_DL))
			pop_toggle(POP_DL, r_dl, 0, 1);
		download_icon(ui, r_dl, prog >= 0 || B.dl_flash ? fl_th.accent : fl_th.text, B.dl_flash ? 1.0 : prog, prog >= 0 || B.dl_flash);
		if (tool_button(ui, ID_MENU, r_menu, B.pop_kind == POP_MENU))
			pop_toggle(POP_MENU, r_menu, 0, 1);
		for (int i = 0; i < 3; i++)	/* three equal bars */
			fl_fill_rect(cv, fl_r(r_menu.x + (r_menu.w - 14) / 2, r_menu.y + (r_menu.h - 11) / 2 + i * 5, 14, 2), fl_th.text);
	}
	g_free(hst);
	if (cur->loading && cur->progress > 0.0) {
		int w = (int)(tb.w * cur->progress);
		fl_fill_rect(cv, fl_r(tb.x, tb.y + tb.h - 2, w, 2), fl_th.accent);
	}

	if (cfg_int("bookmarks_bar", 1))
		draw_bookmarks_bar(ui, fl_cut_top(&c, BAR_H));
	if (ui->release & FL_BTN_L)
		B.pop_closed = POP_NONE;

	/* the page: everything below the toolbar belongs to the subsurface */
	if (c.x != B.page_rect.x || c.y != B.page_rect.y || c.w != B.page_rect.w || c.h != B.page_rect.h) {
		B.page_rect = c;
		wl_subsurface_set_position(B.sub, c.x, c.y);
		cur->w = c.w > 1 ? c.w : 1;
		cur->h = c.h > 1 ? c.h : 1;
		wpe_view_backend_dispatch_set_size(tab_vb(cur), (uint32_t)cur->w, (uint32_t)cur->h);
	}

	/* what the tab row asked for happens once the pass is drawn */
	if (ev.select >= 0 && B.tabs[ev.select] != cur)
		g_idle_add(idle_activate, B.tabs[ev.select]);
	if (ev.close >= 0)
		g_idle_add(idle_close, B.tabs[ev.close]);
	if (ev.newtab)
		g_idle_add(idle_newtab, NULL);
}

/* ---- WebKit signals ------------------------------------------------------- */

/* what the bar shows for a page: nothing for the start page */
static void url_show(const char *uri)
{
	if (!uri)
		return;
	g_strlcpy(B.url, !strcmp(uri, B.home) ? "" : uri, sizeof B.url);
}

static void on_uri(WebKitWebView *v, GParamSpec *p, struct tab *t)
{
	const char *u = webkit_web_view_get_uri(v);
	if (u)
		g_strlcpy(t->uri, u, sizeof t->uri);
	if (t == B.cur)
		sync_ui();
	else
		fl_win_redraw(B.win);
}

static void on_title(WebKitWebView *v, GParamSpec *p, struct tab *t)
{
	const char *s = webkit_web_view_get_title(v);
	g_strlcpy(t->title, s ? s : "", sizeof t->title);
	if (t == B.cur)
		title_update();
	fl_win_redraw(B.win);
}

static void on_progress(WebKitWebView *v, GParamSpec *p, struct tab *t)
{
	t->progress = webkit_web_view_get_estimated_load_progress(v);
	if (t == B.cur)
		fl_win_redraw(B.win);
}

/* testing: FREEBWS_EVAL=js runs that script in every page 3 s after it loaded and logs the result */
static void eval_done(GObject *o, GAsyncResult *r, gpointer d)
{
	GError *e = NULL;
	JSCValue *v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(o), r, &e);
	char *s = v ? jsc_value_to_string(v) : NULL;
	fprintf(stderr, "freebws: eval: %s\n", s ? s : e ? e->message : "(none)");
	g_free(s);
	if (v)
		g_object_unref(v);
	g_clear_error(&e);
}

static gboolean eval_later(gpointer p)
{
	struct tab *t = p;
	if (tab_index(t) >= 0 && t->view)
		webkit_web_view_evaluate_javascript(t->view, getenv("FREEBWS_EVAL"), -1, NULL, NULL, NULL, eval_done, NULL);
	return G_SOURCE_REMOVE;
}

static void on_load(WebKitWebView *v, WebKitLoadEvent ev, struct tab *t)
{
	if (ev == WEBKIT_LOAD_FINISHED && getenv("FREEBWS_EVAL"))
		g_timeout_add(3000, eval_later, t);
	t->loading = ev != WEBKIT_LOAD_FINISHED;
	if (ev == WEBKIT_LOAD_STARTED || ev == WEBKIT_LOAD_REDIRECTED) {
		if (ev == WEBKIT_LOAD_STARTED)
			t->adb.blocked = 0;
		/* link clicks, redirects, scripts: the page's host gets its blocker settings (a navigation we
		 * started ourselves did this already; for the same host it does nothing) */
		const char *u = webkit_web_view_get_uri(v);
		if (u && navigate_hook(t, u))
			webkit_web_view_stop_loading(v);
	}
	if (t->loading)
		spin_start();
	if (ev == WEBKIT_LOAD_FINISHED)
		fprintf(stderr, "freebws: loaded %s\n", webkit_web_view_get_uri(v));
	if (t == B.cur)
		sync_ui();
	else
		fl_win_redraw(B.win);
}

static gboolean on_load_failed(WebKitWebView *v, WebKitLoadEvent ev, const char *uri, GError *err, struct tab *t)
{
	fprintf(stderr, "freebws: load failed: %s: %s (%s %d)\n", uri, err->message, g_quark_to_string(err->domain), err->code);
	t->loading = 0;
	/* a navigation that was stopped, or turned into a download, is not an error to show */
	if (err->domain == WEBKIT_NETWORK_ERROR && err->code == WEBKIT_NETWORK_ERROR_CANCELLED)
		return TRUE;
	/* policy errors: 102 is a navigation that became a download or was replaced; 104 is the content
	 * blocker stopping the page itself */
	int blocked = err->domain == WEBKIT_POLICY_ERROR && err->code == 104;
	if (err->domain == WEBKIT_POLICY_ERROR && err->code == WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE)
		return TRUE;
	if (!uri || !strncmp(uri, "freebws://", 10) || !strncmp(uri, "about:", 6))
		return FALSE;
	int kind = blocked ? ERR_BLOCKED : pages_classify(err);
	g_strlcpy(t->err_uri, uri, sizeof t->err_uri);
	pages_show_error(v, kind, uri, err->message, 0);
	if (t == B.cur)
		focus_page(1);	/* Enter and Esc on the page work */
	return TRUE;
}

/* the certificate of a site is not trusted: the error page offers "continue anyway" */
static gboolean on_tls_failed(WebKitWebView *v, const char *uri, GTlsCertificate *cert, GTlsCertificateFlags errors, struct tab *t)
{
	char *host = uri_host(uri);
	fprintf(stderr, "freebws: TLS errors (0x%x) for %s\n", (unsigned)errors, uri);
	t->loading = 0;
	if (t->tls_cert)
		g_object_unref(t->tls_cert);
	t->tls_cert = g_object_ref(cert);
	g_strlcpy(t->tls_host, host ? host : "", sizeof t->tls_host);
	g_strlcpy(t->err_uri, uri, sizeof t->err_uri);
	GString *d = g_string_new("certificate:");
	if (errors & G_TLS_CERTIFICATE_UNKNOWN_CA)
		g_string_append(d, " issuer not trusted;");
	if (errors & G_TLS_CERTIFICATE_BAD_IDENTITY)
		g_string_append(d, " name does not match;");
	if (errors & G_TLS_CERTIFICATE_NOT_ACTIVATED)
		g_string_append(d, " not valid yet;");
	if (errors & G_TLS_CERTIFICATE_EXPIRED)
		g_string_append(d, " expired;");
	if (errors & G_TLS_CERTIFICATE_REVOKED)
		g_string_append(d, " revoked;");
	if (errors & (G_TLS_CERTIFICATE_INSECURE | G_TLS_CERTIFICATE_GENERIC_ERROR))
		g_string_append(d, " insecure;");
	pages_show_error(v, ERR_TLS, uri, d->str, 1);
	if (t == B.cur)
		focus_page(1);
	g_string_free(d, TRUE);
	g_free(host);
	return TRUE;
}

/* the popup's page is ready (or the fallback timer ran): it takes the front unless it was a background one */
static gboolean popup_show_late(gpointer p)
{
	struct tab *t = p;
	t->show_src = 0;
	if (!t->shown) {
		t->shown = 1;
		if (t->want_fg)
			tab_activate(t);
	}
	return G_SOURCE_REMOVE;
}

static void on_ready(WebKitWebView *v, struct tab *t)
{
	if (t->shown)
		return;
	t->shown = 1;
	if (t->show_src) {
		g_source_remove(t->show_src);
		t->show_src = 0;
	}
	if (t->want_fg)
		g_idle_add(idle_activate, t);
}

/* target=_blank, window.open (also an OAuth popup): a new tab related to the opener (so window.opener
 * works). It is made hidden and shown when WebKit says its page is ready (ready-to-show). */
static WebKitWebView *on_create(WebKitWebView *v, WebKitNavigationAction *na, struct tab *t)
{
	guint btn = webkit_navigation_action_get_mouse_button(na);
	guint mods = webkit_navigation_action_get_modifiers(na);
	int bg = btn == 2 || (mods & wpe_input_keyboard_modifier_control);
	struct tab *n = tab_create(v, NULL, 0);
	if (!n)
		return NULL;
	n->opener = t;
	n->want_fg = !bg;
	n->show_src = g_timeout_add_seconds(5, popup_show_late, n);
	return n->view;
}

/* window.close() */
static void on_close_req(WebKitWebView *v, struct tab *t)
{
	g_idle_add(idle_close, t);
}

/* middle click and Ctrl-click on a link: a new tab in the background */
static gboolean on_policy(WebKitWebView *v, WebKitPolicyDecision *d, WebKitPolicyDecisionType type, struct tab *t)
{
	/* a response the page cannot show (a file, application/octet-stream) or that the server marks as an
	 * attachment is a download: decided here, not left to the engine's default (WPE 2.54 ended such a
	 * navigation with "Frame load interrupted" and no download at all) */
	if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
		WebKitResponsePolicyDecision *rd = WEBKIT_RESPONSE_POLICY_DECISION(d);
		WebKitURIResponse *resp = webkit_response_policy_decision_get_response(rd);
		SoupMessageHeaders *hd = resp ? webkit_uri_response_get_http_headers(resp) : NULL;
		const char *cd = hd ? soup_message_headers_get_one(hd, "Content-Disposition") : NULL;
		if (!webkit_response_policy_decision_is_mime_type_supported(rd) || (cd && !g_ascii_strncasecmp(cd, "attachment", 10))) {
			if (getenv("FREEBWS_POLICY_LOG"))
				fprintf(stderr, "freebws: policy: download %s\n", resp ? webkit_uri_response_get_uri(resp) : "?");
			webkit_policy_decision_download(d);
			return TRUE;
		}
		return FALSE;
	}
	if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION && type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
		return FALSE;
	WebKitNavigationAction *na = webkit_navigation_policy_decision_get_navigation_action(
		WEBKIT_NAVIGATION_POLICY_DECISION(d));
	if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION && na) {
		const char *u0 = webkit_uri_request_get_uri(webkit_navigation_action_get_request(na));
		if (getenv("FREEBWS_POLICY_LOG"))
			fprintf(stderr, "freebws: policy: nav %s frame=%s type=%d\n", u0 ? u0 : "?",
				webkit_navigation_action_get_frame_name(na) ? webkit_navigation_action_get_frame_name(na) : "(null)",
				(int)webkit_navigation_action_get_navigation_type(na));
	}
	if (!na || webkit_navigation_action_get_navigation_type(na) != WEBKIT_NAVIGATION_TYPE_LINK_CLICKED)
		return FALSE;
	guint btn = webkit_navigation_action_get_mouse_button(na);
	guint mods = webkit_navigation_action_get_modifiers(na);
	if (btn != 2 && !(btn <= 1 && (mods & wpe_input_keyboard_modifier_control)))
		return FALSE;
	const char *uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(na));
	if (!uri)
		return FALSE;
	char *u = g_strdup(uri);
	webkit_policy_decision_ignore(d);
	tab_create(NULL, u, 0);
	g_free(u);
	return TRUE;
}

/* the web process died (crash, memory limit, or the single process took the others with it) */
static void on_terminated(WebKitWebView *v, WebKitWebProcessTerminationReason r, struct tab *t)
{
	fprintf(stderr, "freebws: web process of \"%s\" ended (%d)\n", t->title, (int)r);
	t->loading = 0;
	if (t == B.cur) {
		const char *u = webkit_web_view_get_uri(v);
		g_strlcpy(t->err_uri, u && strncmp(u, "freebws://", 10) ? u : t->uri, sizeof t->err_uri);
		pages_show_error(v, ERR_CRASH, t->err_uri, r == WEBKIT_WEB_PROCESS_CRASHED ? "the web process crashed" :
				 r == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "the web process used too much memory" : "the web process ended", 0);
		focus_page(1);
	} else {
		t->reload = 1;
	}
}

/* the page's script (count.js, in every frame) reports requests the content filters stopped */
static void on_count(WebKitUserContentManager *m, JSCValue *v, struct tab *t)
{
	if (!jsc_value_is_string(v))
		return;
	char *str = jsc_value_to_string(v);
	t->adb.blocked += (unsigned)atoi(str);
	g_free(str);
	if (t == B.cur)
		fl_win_redraw(B.win);
}

static void tab_signals(struct tab *t)
{
	WebKitWebView *v = t->view;
	g_signal_connect(v, "notify::uri", G_CALLBACK(on_uri), t);
	g_signal_connect(v, "notify::title", G_CALLBACK(on_title), t);
	g_signal_connect(v, "notify::estimated-load-progress", G_CALLBACK(on_progress), t);
	g_signal_connect(v, "load-changed", G_CALLBACK(on_load), t);
	g_signal_connect(v, "load-failed", G_CALLBACK(on_load_failed), t);
	g_signal_connect(v, "load-failed-with-tls-errors", G_CALLBACK(on_tls_failed), t);
	g_signal_connect(v, "create", G_CALLBACK(on_create), t);
	g_signal_connect(v, "ready-to-show", G_CALLBACK(on_ready), t);
	g_signal_connect(v, "close", G_CALLBACK(on_close_req), t);
	g_signal_connect(v, "decide-policy", G_CALLBACK(on_policy), t);
	g_signal_connect(v, "web-process-terminated", G_CALLBACK(on_terminated), t);
}

/* ---- memory ----------------------------------------------------------------
 * For machines under 3 GB of memory (bigger ones keep WebKit's defaults, which are
 * faster to go back and safer: a process per site).
 * Measured in QEMU at 1 GB (docs/M8-BROWSER.md, "Memory"). WebKit's defaults keep
 * one web process per origin visited: the page cache holds the old page, the
 * process cache and the prewarmed spares keep processes (65 MB each even when
 * empty): 1.27 GB after five origins. What keeps it flat:
 *   cache model DOCUMENT_VIEWER      no page cache, no back/forward cache: going back
 *                                    reloads, and the old pages' processes go
 *   ProcessSwapOnCrossSiteNavigation=0
 *                                    a tab keeps its process across origins instead
 *                                    of a new process per site (and the old one
 *                                    lingering)
 *   WEBKIT_SKIA_ENABLE_CPU_RENDERING no GL context per web process: this machine
 *                                    class has llvmpipe only, and its shader
 *                                    compiler costs 50 to 100 MB per process
 *   tabs in one web process (under 3 GB: related views, FREEBWS_SHARE=0|1)
 *                                    3 live tabs: 566 MB rss instead of about 900
 *   memory pressure settings (under 3 GB: 300 MB limit, cleanup from 150 MB, strict
 *                                    from 210 MB, polled every 10 s, nothing is killed)
 *                                    in the web context (the call on the network session
 *                                    only reaches the network process): after background
 *                                    tabs were discarded the shared web process gives
 *                                    memory back (22 MB, against 9 without)
 * No effect measured: page cache setting on top of the cache model, Skia CPU
 * painting threads, MALLOC_ARENA_MAX, the JS JIT off (-14 MB on a DSSL page only).
 * Environment overrides (testing): FREEBWS_CACHE=viewer|document|browser,
 * FREEBWS_FEATURES=Name=0|1,... (replaces the feature list), FREEBWS_MP=limit_mb,
 * conservative,strict,kill,poll_s or 0 (WebKitMemoryPressureSettings), FREEBWS_SHARE,
 * FREEBWS_DISCARD_IDLE=seconds (0: never), FREEBWS_DISCARD_MB=free MB below which the
 * oldest background tab is discarded. */
static unsigned mem_total_mb(void)
{
	unsigned long total = 0;
	FILE *f = fopen("/proc/meminfo", "r");
	char line[128];
	while (f && fgets(line, sizeof line, f))
		if (sscanf(line, "MemTotal: %lu kB", &total) == 1)
			break;
	if (f)
		fclose(f);
	return (unsigned)(total / 1024);
}

/* ---- web process sandbox ----------------------------------------------------
 * WPE WebKit built with ENABLE_BUBBLEWRAP_SANDBOX (recipes/wpewebkit, release 2) starts every web
 * process inside bubblewrap by itself (user, mount, pid and uts namespaces; ipc stays shared for WPE, a seccomp filter, a private
 * /tmp and a runtime dir of its own, / not visible: only /usr, /etc, /lib*, the GPU nodes /dev/dri
 * and /dev/udmabuf, fonts, the GStreamer data and what add_path_to_sandbox() names; the session bus
 * only through xdg-dbus-proxy). The network process is not sandboxed by WebKit (it reads and writes
 * the cache, cookies and the download folder). The Wayland link is not a file: the nested
 * display's socket is handed to the web process over the IPC connection, so dmabuf and shm buffers
 * need nothing from the file system except the render node.
 *
 * The one thing WebKit does not do is check that bubblewrap can run here: where it cannot (no user
 * namespaces, a container, bwrap or xdg-dbus-proxy missing), WebKit aborts the UI process at the first
 * web process. So the browser tries bwrap itself first, with the same kind of namespaces.
 *   sandbox=auto (default)  works: on. Does not: off, with a warning in the log (stderr) and on the
 *                           Settings page, so nobody believes in a sandbox that is not there.
 *   sandbox=on              does not work: freebws exits with the reason (for people who prefer
 *                           no browser to an unsandboxed one).
 *   sandbox=off             switched off on purpose, also warned about.
 * FREEBWS_SANDBOX=auto|on|off overrides the config file key "sandbox". `freebws --sandbox-check`
 * prints the verdict and exits 0 (usable) or 1. Extra read-only paths for the web process (testing):
 * FREEBWS_SANDBOX_RO=/a:/b. */
#ifndef BWRAP_PATH
#define BWRAP_PATH "/usr/bin/bwrap"
#endif
#ifndef DBUS_PROXY_PATH
#define DBUS_PROXY_PATH "/usr/bin/xdg-dbus-proxy"
#endif

/* 1 when WebKit's sandbox can start here; else 0 with the reason in WHY */
static int sandbox_probe(char *why, size_t n)
{
	if (access(BWRAP_PATH, X_OK) != 0) {
		snprintf(why, n, "%s is not installed", BWRAP_PATH);
		return 0;
	}
	if (access(DBUS_PROXY_PATH, X_OK) != 0) {
		snprintf(why, n, "%s is not installed (WebKit needs it for the session bus)", DBUS_PROXY_PATH);
		return 0;
	}
	char self[400];
	ssize_t l = readlink("/proc/self/exe", self, sizeof self - 1);
	if (l <= 0) {
		snprintf(why, n, "cannot find my own executable");
		return 0;
	}
	self[l] = 0;
	/* As WebKit starts its web process: new user namespace (bwrap does that itself), uts, pid and
	 * ipc namespaces, the usual mounts; the program is this one, in a mode that just exits.
	 *
	 * The binds are the ones WebKit makes (BubblewrapLauncher.cpp): /usr/lib, /usr/bin, /lib, /lib64,
	 * /bin one by one, never /usr as a whole. Up to base-files release 3 FreeLFS had the homes in
	 * /usr (the @home subvolume) and /usr/bin, /usr/lib were relative links to ../sysflfs/...:
	 * binding /usr would have shown every home in the sandbox, and /proc/self/exe was
	 * /sysflfs/bin/freebws, which only exists in the new root if its directory is bound too
	 * ("execvp /sysflfs/bin/freebws: No such file" in the first version of this probe). Since
	 * release 4 /usr is the real system directory and /sysflfs a link to it; binding the directory
	 * of the real executable at its real path stays, for builds run from elsewhere (bwrap resolves
	 * the links of a source path itself). */
	char exedir[400];
	snprintf(exedir, sizeof exedir, "%s", self);
	char *slash = strrchr(exedir, '/');
	if (slash && slash != exedir)
		*slash = 0;
	else
		snprintf(exedir, sizeof exedir, "/usr/bin");
	char *argv[] = { BWRAP_PATH, "--unshare-uts", "--unshare-pid", "--unshare-ipc",
			 "--ro-bind-try", "/usr/lib", "/usr/lib", "--ro-bind-try", "/usr/bin", "/usr/bin",
			 "--ro-bind-try", "/lib", "/lib", "--ro-bind-try", "/lib64", "/lib64",
			 "--ro-bind-try", "/bin", "/bin", "--dev", "/dev", "--proc", "/proc", "--tmpfs", "/tmp",
			 "--ro-bind-try", exedir, exedir,	/* after the tmpfs: a build in /tmp must stay visible */
			 self, "--sandbox-child", NULL };
	gchar *err = NULL;
	gint st = 0;
	GError *ge = NULL;
	if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, NULL, &err, &st, &ge)) {
		snprintf(why, n, "cannot run bwrap: %s", ge ? ge->message : "?");
		g_clear_error(&ge);
		return 0;
	}
	if (!g_spawn_check_wait_status(st, NULL)) {
		char *e = err ? g_strstrip(err) : NULL;
		char *nl = e ? strchr(e, '\n') : NULL;
		if (nl)
			*nl = 0;
		snprintf(why, n, "bwrap failed%s%s", e && *e ? ": " : "", e && *e ? e : " (no user namespaces?)");
		g_free(err);
		return 0;
	}
	g_free(err);
	return 1;
}

/* Decide before the WebKitWebContext exists (that is when WebKit reads the environment).
 * Returns 0 normally, 1 when sandbox=on cannot be honoured. */
static int sandbox_setup(void)
{
	const char *mode = getenv("FREEBWS_SANDBOX");
	if (!mode || !*mode)
		mode = cfg_get("sandbox", "auto");
	if (!strcmp(mode, "off") || !strcmp(mode, "0")) {
		fprintf(stderr, "freebws: WARNING: the web process sandbox is switched OFF (sandbox=off): pages run "
			"with all your rights\n");
		setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", 1);
		pages_sandbox_status = "Web process sandbox: OFF. It was switched off by the sandbox setting; pages run with all your rights.";
		return 0;
	}
	char why[400];
	if (sandbox_probe(why, sizeof why)) {
		unsetenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS");
		fprintf(stderr, "freebws: web process sandbox on (bubblewrap)\n");
		pages_sandbox_status = "Web process sandbox: on (bubblewrap). Web pages run in their own namespaces, "
				 "with a syscall filter and without your files.";
		return 0;
	}
	if (!strcmp(mode, "on") || !strcmp(mode, "1")) {
		fprintf(stderr, "freebws: the web process sandbox is required (sandbox=on) but cannot start: %s\n", why);
		return 1;
	}
	fprintf(stderr, "freebws: WARNING: the web process sandbox is NOT available (%s); continuing WITHOUT a sandbox, "
		"pages run with all your rights. Set sandbox=on in ~/.config/freebws/config to refuse that.\n", why);
	setenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS", "1", 1);
	pages_sandbox_status = "Web process sandbox: OFF, it is not available here (bubblewrap does not start). Pages run with all your rights.";
	return 0;
}

/* paths the web process may read besides what WebKit grants by itself; after the context exists,
 * before the first web process */
static void sandbox_paths(void)
{
	if (!getenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS") || !strcmp(getenv("WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS"), "0")) {
		webkit_web_context_add_path_to_sandbox(B.ctx, share_dir(), TRUE);
		const char *e = getenv("FREEBWS_SANDBOX_RO");
		char **v = e ? g_strsplit(e, ":", 0) : NULL;
		for (int i = 0; v && v[i]; i++)
			if (v[i][0] == '/')
				webkit_web_context_add_path_to_sandbox(B.ctx, v[i], TRUE);
		g_strfreev(v);
	}
}

/* useragent= in the config replaces it. The default is a Chrome on Linux string with WebKit's own
 * version: sign-in pages (Google) refuse browsers they do not know, and an embedded WebKit's default
 * string (with its WPE token) is one of them. The Chrome version is a constant to move up now and then. */
#define CHROME_UA_VERSION "146.0.0.0"
static void user_agent_setup(void)
{
	const char *c = cfg_get("useragent", "");
	if (*c)
		B.ua = g_strdup(c);
	else
		B.ua = g_strdup_printf("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/%u.%u.%u (KHTML, like Gecko) "
				       "Chrome/%s Safari/537.36", webkit_get_major_version(), webkit_get_minor_version(),
				       webkit_get_micro_version(), CHROME_UA_VERSION);
}

/* cookies=always (default) or cookies=no-third-party. The default keeps every cookie: WebKit's
 * no-third-party policy is what breaks some sign-in flows (a Google or Cloudflare frame, a popup
 * that sets cookies for the page that opened it). See src/freebws/README, "Cookies". */
static void cookies_setup(void)
{
	const char *c = getenv("FREEBWS_COOKIES");
	if (!c || !*c)
		c = cfg_get("cookies", "always");
	WebKitCookieManager *cm = webkit_network_session_get_cookie_manager(webkit_network_session_get_default());
	webkit_cookie_manager_set_accept_policy(cm, !strcmp(c, "no-third-party") ? WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY
										 : WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);
}

static void tune_webkit(void)
{
	user_agent_setup();
	const char *e = getenv("FREEBWS_MP");	/* limit_mb,conservative,strict,kill,poll_s */
	if (!e && mem_total_mb() < 3072)	/* cleans up a web process that holds many tabs */
		e = "300,0.5,0.7,0,10";
	if (e && !strcmp(e, "0"))
		e = NULL;
	WebKitMemoryPressureSettings *ms = NULL;
	if (e) {
		double v[5] = { 0, 0, 0, 0, 0 };
		int n = sscanf(e, "%lf,%lf,%lf,%lf,%lf", &v[0], &v[1], &v[2], &v[3], &v[4]);
		ms = webkit_memory_pressure_settings_new();
		if (n >= 1 && v[0] > 0)
			webkit_memory_pressure_settings_set_memory_limit(ms, (guint)v[0]);
		if (n >= 2 && v[1] > 0)
			webkit_memory_pressure_settings_set_conservative_threshold(ms, v[1]);
		if (n >= 3 && v[2] > 0)
			webkit_memory_pressure_settings_set_strict_threshold(ms, v[2]);
		if (n >= 4 && v[3] > 0)
			webkit_memory_pressure_settings_set_kill_threshold(ms, v[3]);
		if (n >= 5 && v[4] > 0)
			webkit_memory_pressure_settings_set_poll_interval(ms, v[4]);
		webkit_network_session_set_memory_pressure_settings(ms);	/* the network process */
	}
	/* the web processes take theirs from the context, set when it is made */
	B.ctx = ms ? g_object_new(WEBKIT_TYPE_WEB_CONTEXT, "memory-pressure-settings", ms, NULL) :
		     g_object_new(WEBKIT_TYPE_WEB_CONTEXT, NULL);
	if (ms)
		webkit_memory_pressure_settings_free(ms);
	sandbox_paths();
	/* the memory-saving model below is for machines under 3 GB; bigger ones keep WebKit's
	 * defaults (back/forward cache, a process per site) */
	int small = mem_total_mb() < 3072;
	e = getenv("FREEBWS_CACHE");
	if (!e)
		e = small ? "viewer" : "browser";
	webkit_web_context_set_cache_model(B.ctx, !strcmp(e, "viewer") ? WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER :
					   !strcmp(e, "document") ? WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER :
								    WEBKIT_CACHE_MODEL_WEB_BROWSER);
	B.features = getenv("FREEBWS_FEATURES");
	if (!B.features)
		B.features = small ? "ProcessSwapOnCrossSiteNavigation=0" : "";
	/* policy: discard background tabs after this many seconds, or below this much free memory */
	/* under 3 GB: one web process for all tabs (about 65 MB of base each otherwise);
	 * FREEBWS_SHARE=0|1 decides by hand */
	e = getenv("FREEBWS_SHARE");
	B.share_process = e ? atoi(e) : mem_total_mb() < 3072;
	e = getenv("FREEBWS_DISCARD_IDLE");
	B.idle_s = e ? (unsigned)atoi(e) : 1800;
	e = getenv("FREEBWS_DISCARD_MB");
	if (e) {
		B.avail_mb = (unsigned)atoi(e);
	} else {
		unsigned mb = mem_total_mb() / 5;	/* a fifth of the memory, at most 384 MB */
		B.avail_mb = mb > 384 ? 384 : mb;
	}
}

static void settings_apply(WebKitSettings *st)
{
	/* window.open from a script (after an await, a few seconds after the click): WebKit drops it unless
	 * this is set; the popup is then a tab (on_create) that keeps window.opener */
	webkit_settings_set_javascript_can_open_windows_automatically(st, TRUE);
	if (B.ua)
		webkit_settings_set_user_agent(st, B.ua);
	char **p = g_strsplit(B.features, ",", 0);
	WebKitFeatureList *fl = webkit_settings_get_all_features();
	for (int i = 0; p[i]; i++) {	/* Name=0,Name=1 */
		char *eq = strchr(p[i], '=');
		if (!eq)
			continue;
		*eq = 0;
		for (gsize j = 0; j < webkit_feature_list_get_length(fl); j++) {
			WebKitFeature *f = webkit_feature_list_get(fl, j);
			if (!strcmp(webkit_feature_get_identifier(f), p[i]))
				webkit_settings_set_feature_enabled(st, f, atoi(eq + 1));
		}
	}
	webkit_feature_list_unref(fl);
	g_strfreev(p);
	const char *e = getenv("FREEBWS_PAGECACHE");
	if (e)
		webkit_settings_set_enable_page_cache(st, atoi(e));
}

static gboolean mem_available_mb(unsigned long *mb)
{
	FILE *f = fopen("/proc/meminfo", "r");
	char line[128];
	unsigned long kb;
	gboolean ok = FALSE;
	while (f && fgets(line, sizeof line, f))
		if (sscanf(line, "MemAvailable: %lu kB", &kb) == 1) {
			*mb = kb / 1024;
			ok = TRUE;
			break;
		}
	if (f)
		fclose(f);
	return ok;
}

static void discard_log(struct tab *t, const char *why)
{
	fprintf(stderr, "freebws: discarding tab \"%s\" (%s)\n", t->title[0] ? t->title : t->uri, why);
}

/* the opener of a popup that is still open: its script waits for the popup, so it stays */
static int has_popup(struct tab *t)
{
	for (int i = 0; i < B.ntabs; i++)
		if (B.tabs[i]->opener == t)
			return 1;
	return 0;
}

static gboolean mem_tick(gpointer d)
{
	uint32_t now = now_ms();
	struct tab *oldest = NULL;
	for (int i = 0; i < B.ntabs; i++) {
		struct tab *t = B.tabs[i];
		if (t == B.cur || !t->view || t->loading || has_popup(t) || webkit_web_view_is_playing_audio(t->view))
			continue;
		if (B.idle_s && now - t->last_active > B.idle_s * 1000u) {
			discard_log(t, "unused");
			tab_discard(t);
			continue;
		}
		if (!oldest || (int32_t)(t->last_active - oldest->last_active) < 0)
			oldest = t;
	}
	unsigned long mb;
	if (oldest && mem_available_mb(&mb) && mb < B.avail_mb) {
		discard_log(oldest, "low memory");
		tab_discard(oldest);
	}
	return G_SOURCE_CONTINUE;
}

/* ---- DSSL ------------------------------------------------------------------
 * DSSL (huggingface.co/EHS-Studio/dssl, MIT) is a style language, an alternative
 * to CSS. Two ways to apply it to a page, decided per page:
 *
 * Native (libdssl, src/libdssl): a user script at the end of the document looks
 * for DSSL tags (<link rel="stylesheet/dssl">, <link rel="stylesheet"
 * type="text/dssl">, <style type="text/dssl">, <script type="text/dssl">), fetches the
 * linked sheets in the page (same rules as the engine: cross-origin, file://) and
 * posts all sources, in the engine's load order (links, then inline blocks), to the
 * script message handler "dssl". Here every sheet goes through one dssl_ctx; when
 * none needs the engine (dssl_needs_engine() == 0) the reply is the lowered CSS of
 * each sheet and the script inserts it as <style data-dssl-native> after the
 * original tag, then fires `dssl-ready` as the engine does. No engine is loaded.
 *
 * Engine (JavaScript, bundled under /usr/share/freebws/dssl and served through
 * the URI scheme freebws-dssl://engine/..., never the network): when a sheet uses
 * what CSS cannot express (texture, aura, @signal, seen, ... see dssl.h), when the
 * page scripts the engine (a `DSSL` identifier in an inline script) or ships its
 * own, when the page's CSP blocks inline styles, or with FREEBWS_DSSL=engine, the
 * script imports freebws-dssl://engine/boot.js as before. The whole page then
 * goes through the engine (its inline styles beat the page's CSS, lowered CSS
 * lives in cascade layers: mixing the two would change who wins).
 *
 * <html data-dssl-mode="native|engine"> and window.__freebwsDssl say which, and
 * stderr gets a "freebws: dssl: ..." line per page. Pages with a strict script-src
 * CSP do not get the engine (the import is subject to it); .dssl files next to a
 * file:// page cannot be fetched (WebKit blocks it), so use inline blocks there.
 * DSSL tags added to the DOM after load are not seen. The scheme is secure and
 * CORS-enabled. */
#define DSSL_DIR "/usr/share/freebws/dssl"
#define DSSL_MAX_BYTES (8u << 20)
#define DSSL_PROBE \
	"(function(NATIVE){try{" \
	"var w=window,d=document;if(w.DSSL||w.__freebwsDssl)return;" \
	"var Q='link[rel=\"stylesheet/dssl\"],link[type=\"text/dssl\"],style[type=\"text/dssl\"],script[type=\"text/dssl\"]';" \
	"if(!d.querySelector(Q))return;" \
	"if(d.querySelector('script[src*=\"dssl\"]'))return;" \
	"w.__freebwsDssl=1;var de=d.documentElement;" \
	"function engine(){de.setAttribute('data-dssl-mode','engine');w.__freebwsDssl='engine';" \
	"var l=d.querySelectorAll('link[type=\"text/dssl\"]');" \
	"for(var i=0;i<l.length;i++)if(/stylesheet/i.test(l[i].rel))l[i].setAttribute('rel','stylesheet/dssl');" \
	"import('freebws-dssl://engine/boot.js').catch(function(e){console.warn('FreeBWS: DSSL engine did not load: '+e);});}" \
	"var h=w.webkit&&w.webkit.messageHandlers&&w.webkit.messageHandlers.dssl;" \
	"if(!NATIVE||!h){engine();return;}" \
	"var sc=d.scripts;for(var i=0;i<sc.length;i++){var t=sc[i].type;if(!sc[i].src&&(!t||/javascript|module/i.test(t))&&/\\bDSSL\\s*[.\\[]/.test(sc[i].textContent)){engine();return;}}" \
	"var links=[].slice.call(d.querySelectorAll('link[rel=\"stylesheet/dssl\"],link[type=\"text/dssl\"][rel~=\"stylesheet\"]'));" \
	"var inl=[].slice.call(d.querySelectorAll('style[type=\"text/dssl\"],script[type=\"text/dssl\"]'));" \
	"Promise.all(links.map(function(l){var u=l.getAttribute('href');" \
	"return fetch(u).then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);return r.text();})" \
	".then(function(t){return{el:l,url:u,text:t};},function(e){console.warn('[DSSL] could not load '+u+': '+e);return null;});}))" \
	".then(function(got){var src=got.filter(Boolean);" \
	"inl.forEach(function(s,i){src.push({el:s,url:s.getAttribute('data-dssl-name')||'inline-'+(i+1),text:s.textContent});});" \
	"return h.postMessage({sheets:src.map(function(s){return{url:s.url,text:s.text};})}).then(function(r){" \
	"if(String(r[0]).indexOf('native')!==0){engine();return;}" \
	"var made=[],csp=false;" \
	"function onv(e){if(/^style-src/.test(e.violatedDirective)&&!csp){csp=true;made.forEach(function(m){m.remove();});engine();}}" \
	"d.addEventListener('securitypolicyviolation',onv);" \
	"src.forEach(function(s,i){var css=r[i+1];if(!css)return;var m=d.createElement('style');" \
	"m.setAttribute('data-dssl-native','');m.textContent=css;s.el.parentNode.insertBefore(m,s.el.nextSibling);made.push(m);});" \
	"setTimeout(function(){d.removeEventListener('securitypolicyviolation',onv);},3000);" \
	"de.setAttribute('data-dssl-mode','native');w.__freebwsDssl='native';" \
	"w.dispatchEvent(new CustomEvent('dssl-ready',{detail:{version:String(r[0]).slice(7),native:true}}));" \
	"de.classList.add('dssl-ready');});})" \
	".catch(function(e){console.warn('[DSSL] native path failed ('+e+'), using the engine');engine();});" \
	"}catch(e){}})(%d);"

static void dssl_scheme(WebKitURISchemeRequest *req, gpointer ud)
{
	GError *err = NULL;
	GUri *uri = g_uri_parse(webkit_uri_scheme_request_get_uri(req), G_URI_FLAGS_PARSE_RELAXED, NULL);
	const char *path = uri ? g_uri_get_path(uri) : "";
	gboolean ok = uri && !g_strcmp0(g_uri_get_host(uri), "engine") && path[0] == '/' && path[1] &&
		      !strstr(path, "..") && !strstr(path, "//");
	for (const char *c = path; ok && *c; c++)
		if (!g_ascii_isalnum(*c) && !strchr("/._-", *c))
			ok = FALSE;
	char *file = ok ? g_strconcat(DSSL_DIR, path, NULL) : NULL;
	gchar *data = NULL;
	gsize len = 0;
	if (!file || !g_file_get_contents(file, &data, &len, &err)) {
		GError *e = g_error_new(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "no such DSSL engine file");
		webkit_uri_scheme_request_finish_error(req, e);
		g_error_free(e);
		g_clear_error(&err);
	} else {
		if (!strcmp(path, "/boot.js"))	/* the engine's entry point: it is about to run */
			fprintf(stderr, "freebws: dssl: JS engine loaded\n");
		const char *ct = g_str_has_suffix(path, ".js") || g_str_has_suffix(path, ".mjs") ?
					 "text/javascript" : "text/plain; charset=utf-8";
		GInputStream *in = g_memory_input_stream_new_from_data(data, (gssize)len, g_free);
		WebKitURISchemeResponse *rs = webkit_uri_scheme_response_new(in, (gint64)len);
		webkit_uri_scheme_response_set_content_type(rs, ct);
		SoupMessageHeaders *h = soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE);
		soup_message_headers_append(h, "Access-Control-Allow-Origin", "*");
		webkit_uri_scheme_response_set_http_headers(rs, h);
		webkit_uri_scheme_request_finish_with_response(req, rs);
		g_object_unref(rs);
		g_object_unref(in);
	}
	g_free(file);
	if (uri)
		g_uri_unref(uri);
}

static char *jsc_str(JSCValue *o, const char *prop)
{
	JSCValue *v = jsc_value_object_get_property(o, prop);
	char *s = v && jsc_value_is_string(v) ? jsc_value_to_string(v) : NULL;
	if (v)
		g_object_unref(v);
	return s;
}

/* the page posted its DSSL sources: lower them, or send it to the engine.
 * Reply: ["native <version>", css of sheet 1, css of sheet 2, ...] or ["engine"]. */
static gboolean on_dssl_message(WebKitUserContentManager *ucm, JSCValue *msg, WebKitScriptMessageReply *reply,
				struct tab *t)
{
	JSCContext *jc = jsc_value_get_context(msg);
	JSCValue *sheets = jsc_value_is_object(msg) ? jsc_value_object_get_property(msg, "sheets") : NULL;
	GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
	int native = 0;
	if (sheets && jsc_value_is_array(sheets)) {
		JSCValue *lv = jsc_value_object_get_property(sheets, "length");
		int n = lv ? jsc_value_to_int32(lv) : 0;
		if (lv)
			g_object_unref(lv);
		dssl_ctx *ctx = dssl_ctx_new();
		GPtrArray *css = g_ptr_array_new_with_free_func(free);
		size_t total = 0, bytes = 0;
		int hard = 0;
		if (ctx && n > 0 && n <= 256) {
			for (int i = 0; i < n; i++) {	/* all of them, so the log names everything that needs the engine */
				JSCValue *it = jsc_value_object_get_property_at_index(sheets, (unsigned)i);
				char *url = it && jsc_value_is_object(it) ? jsc_str(it, "url") : NULL;
				char *text = it && jsc_value_is_object(it) ? jsc_str(it, "text") : NULL;
				if (it)
					g_object_unref(it);
				size_t len = text ? strlen(text) : 0;
				total += len;
				if (!text || total > DSSL_MAX_BYTES) {
					hard = 1;
				} else {
					dssl_sheet *sh = dssl_parse(text, len, url ? url : "inline", ctx);
					if (!sh) {
						hard = 1;
					} else {
						dssl_diag dg;
						for (size_t k = 0; dssl_error_at(sh, k, &dg); k++)
							fprintf(stderr, "freebws: dssl: %s:%d:%d: %s\n", dg.file ? dg.file : "?",
								dg.line, dg.col, dg.message);
						if (dssl_needs_engine(sh)) {
							GString *names = g_string_new(NULL);
							unsigned seen = 0;
							dssl_feature ft;
							for (size_t k = 0; dssl_feature_at(sh, k, &ft); k++)
								if ((ft.kind & DSSL_FEAT_HARD_MASK) && !(seen & ft.kind)) {
									seen |= ft.kind;
									g_string_append_printf(names, "%s%s at %d:%d", names->len ? ", " : "",
											       ft.name, ft.line, ft.col);
								}
							fprintf(stderr, "freebws: dssl: %s needs the JS engine (%s)\n", url ? url : "inline",
								names->str);
							g_string_free(names, TRUE);
							hard = 1;
						} else if (!hard) {
							char *c = dssl_to_css(sh, 0);
							if (!c) {
								hard = 1;
							} else {
								bytes += strlen(c);
								g_ptr_array_add(css, c);
							}
						}
						dssl_free(sh);
					}
				}
				g_free(url);
				g_free(text);
			}
			if (!hard && (int)css->len == n) {
				native = 1;
				g_ptr_array_add(out, g_strdup("native " DSSL_SPEC_VERSION));
				for (guint i = 0; i < css->len; i++)
					g_ptr_array_add(out, g_strdup(g_ptr_array_index(css, i)));
				fprintf(stderr, "freebws: dssl: native, %d sheet(s), %zu bytes of DSSL -> %zu bytes of CSS\n", n,
					total, bytes);
			}
		}
		g_ptr_array_free(css, TRUE);
		if (ctx)
			dssl_ctx_free(ctx);
	}
	if (sheets)
		g_object_unref(sheets);
	if (!native) {
		g_ptr_array_add(out, g_strdup("engine"));
		fprintf(stderr, "freebws: dssl: page goes to the JS engine\n");
	}
	g_ptr_array_add(out, NULL);
	JSCValue *rv = jsc_value_new_array_from_strv(jc, (const char *const *)out->pdata);
	webkit_script_message_reply_return_value(reply, rv);
	g_object_unref(rv);
	g_ptr_array_free(out, TRUE);
	return TRUE;
}

static void dssl_setup_view(struct tab *t)
{
	char *js = g_strdup_printf(DSSL_PROBE, B.dssl_native);
	WebKitUserScript *us = webkit_user_script_new(js, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
						      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, NULL, NULL);
	g_free(js);
	webkit_user_content_manager_add_script(t->ucm, us);
	webkit_user_script_unref(us);
	webkit_user_content_manager_register_script_message_handler_with_reply(t->ucm, "dssl", NULL);
	g_signal_connect(t->ucm, "script-message-with-reply-received::dssl", G_CALLBACK(on_dssl_message), t);
}

static void dssl_init(void)
{
	webkit_web_context_register_uri_scheme(B.ctx, "freebws-dssl", dssl_scheme, NULL, NULL);
	WebKitSecurityManager *sm = webkit_web_context_get_security_manager(B.ctx);
	webkit_security_manager_register_uri_scheme_as_secure(sm, "freebws-dssl");
	webkit_security_manager_register_uri_scheme_as_cors_enabled(sm, "freebws-dssl");
	const char *e = getenv("FREEBWS_DSSL");
	B.dssl_native = !(e && !strcmp(e, "engine"));
}

static void on_close(fl_win *w, void *ud) { g_main_loop_quit(B.loop); }

static gboolean dl_flash_end(gpointer d)
{
	B.dl_flash_src = 0;
	B.dl_flash = 0;
	fl_win_redraw(B.win);
	return G_SOURCE_REMOVE;
}

static void on_dl_changed(void)
{
	int act = dl_active_count();
	if (act > B.dl_last_active && B.pop_kind != POP_DL && B.dl_btn.w)	/* a new download: show the panel */
		pop_open(POP_DL, B.dl_btn, 0, 1);
	if (act < B.dl_last_active) {	/* one finished: the ring is full for a moment */
		B.dl_flash = 1;
		if (B.dl_flash_src)
			g_source_remove(B.dl_flash_src);
		B.dl_flash_src = g_timeout_add(2500, dl_flash_end, NULL);
	}
	B.dl_last_active = act;
	if (B.pop_kind == POP_DL) {	/* the list grew or shrank */
		int w, h;
		pop_size(POP_DL, &w, &h);
		fl_rect pr = popup_rect(B.pop);
		popup_show(B.pop, pr.x + pr.w - w, pr.y, w, h);
	}
	popup_redraw(B.pop);
	fl_win_redraw(B.win);
}

void fb_open_url(void *tab, const char *url, int new_tab)
{
	struct tab *t = tab;
	if (!new_tab && t && t->view) {
		load_uri(t, url);
		return;
	}
	open_in_tab(url, 1);
}

void fb_tab_retry(void *tab)
{
	struct tab *t = tab;
	if (!t || !t->view)
		return;
	if (t->err_uri[0])
		load_uri(t, t->err_uri);
	else
		webkit_web_view_reload(t->view);
}

void fb_tab_tls_proceed(void *tab)
{
	struct tab *t = tab;
	if (!t || !t->view || !t->tls_cert || !t->tls_host[0])
		return;
	webkit_network_session_allow_tls_certificate_for_host(webkit_web_view_get_network_session(t->view), t->tls_cert, t->tls_host);
	load_uri(t, t->err_uri);
}

void fb_tab_blocked_proceed(void *tab)
{
	struct tab *t = tab;
	char *h = t ? uri_host(t->err_uri) : NULL;
	if (!h)
		return;
	adb_set_site_off(h, 1);
	load_uri(t, t->err_uri);
	g_free(h);
}

void fb_setting_changed(const char *key)
{
	if (!strcmp(key, "dark"))
		dark_refresh_all();
	fl_win_redraw(B.win);
}

/* the ad blocker (re)loaded its filters or its settings changed: every tab gets them, and a
 * navigation that waited for them goes on */
void fb_adb_changed(void)
{
	for (int i = 0; i < B.ntabs; i++) {
		struct tab *t = B.tabs[i];
		if (!t->view)
			continue;
		adb_tab_refilter(&t->adb, t->ucm);
		if (t->adb.deferred && adb_ready()) {
			char *u = t->adb.deferred;
			t->adb.deferred = NULL;
			t->gating = 0;
			webkit_web_view_load_uri(t->view, u);
			g_free(u);
		}
	}
	fl_win_redraw(B.win);
}

/* ---- the libflui main loop inside GLib's ---------------------------------- */

struct flsrc {
	GSource base;
	struct pollfd pf[64];
	gpointer tag[64];
	int n, ntags;
	int fds[64];
};

static gboolean src_prepare(GSource *gs, gint *timeout)
{
	struct flsrc *s = (struct flsrc *)gs;
	*timeout = -1;
	s->n = fl_app_prepare(B.app, s->pf, 64);
	if (s->n < 0) {
		g_main_loop_quit(B.loop);
		s->n = 0;
		return FALSE;
	}
	int same = s->n == s->ntags;
	for (int i = 0; same && i < s->n; i++)
		same = s->fds[i] == s->pf[i].fd;
	if (!same) {
		for (int i = 0; i < s->ntags; i++)
			g_source_remove_unix_fd(gs, s->tag[i]);
		for (int i = 0; i < s->n; i++) {
			s->fds[i] = s->pf[i].fd;
			s->tag[i] = g_source_add_unix_fd(gs, s->pf[i].fd,
				(GIOCondition)((s->pf[i].events & POLLIN ? G_IO_IN : 0) |
					       (s->pf[i].events & POLLOUT ? G_IO_OUT : 0)));
		}
		s->ntags = s->n;
	}
	return FALSE;
}

static time_t start_time;
static void dmabuf_fallback(const char *why);

static gboolean src_check(GSource *gs)
{
	struct flsrc *s = (struct flsrc *)gs;
	if (!s->n)
		return FALSE;
	int r = 0;
	for (int i = 0; i < s->n; i++) {
		GIOCondition c = g_source_query_unix_fd(gs, s->tag[i]);
		s->pf[i].revents = (short)((c & G_IO_IN ? POLLIN : 0) | (c & G_IO_OUT ? POLLOUT : 0) |
					   (c & G_IO_ERR ? POLLERR : 0) | (c & G_IO_HUP ? POLLHUP : 0));
		if (s->pf[i].revents)
			r = 1;
	}
	int bad = fl_app_dispatch(B.app, s->pf, s->n, r);
	if (bad && B.use_dmabuf && B.argv && time(NULL) - start_time < 60 && wl_display_get_error(B.dpy) == EPROTO)
		dmabuf_fallback("the compositor refused a GPU buffer");
	if (bad || fl_app_quitting(B.app))
		g_main_loop_quit(B.loop);
	s->n = 0;
	return FALSE;
}

static gboolean src_dispatch(GSource *gs, GSourceFunc cb, gpointer d) { return G_SOURCE_CONTINUE; }

static GSourceFuncs src_funcs = { src_prepare, src_check, src_dispatch, NULL };

/* The GPU path is only proven on the hardware it was tried on. When it does not deliver, the
 * browser remembers "buffers=shm" in its config and starts again with the CPU path, once:
 *   - no frame has reached the window 12 seconds after the start (a GPU, driver or compositor
 *     combination that does not hand buffers over), or
 *   - the compositor ended our connection with a protocol error in the first minute (it refused a
 *     buffer: a modifier or format it cannot import).
 * A wrong guess costs 12 seconds, not a blank or vanishing browser. */
static void dmabuf_fallback(const char *why)
{
	fprintf(stderr, "freebws: %s, restarting with shm buffers\n", why);
	cfg_set("buffers", "shm");
	setenv("FREEBWS_BUFFERS", "shm", 1);
	execv("/proc/self/exe", B.argv);
	fprintf(stderr, "freebws: restart failed: %s\n", strerror(errno));
}

static gboolean dmabuf_watchdog(gpointer d)
{
	if (B.frames > 0 || !B.use_dmabuf)
		return G_SOURCE_REMOVE;
	dmabuf_fallback("no frame from the GPU path after 12 s");
	return G_SOURCE_REMOVE;
}

/* flcomp writes $XDG_RUNTIME_DIR/flcomp-dmabuf-planes when its GPU path takes buffers of several planes
 * (what a Radeon's web process makes). An older flcomp would disconnect us at the first frame, so without
 * the note the CPU path is used. */
static int flcomp_takes_planes(void)
{
	const char *rt = getenv("XDG_RUNTIME_DIR");
	char p[400];
	if (!rt || !*rt)
		return 0;
	snprintf(p, sizeof p, "%s/flcomp-dmabuf-planes", rt);
	return access(p, F_OK) == 0;
}

/* ---- dmabuf mode: an EGLDisplay for wpebackend-fdo ------------------------ */

/* wpebackend-fdo only serves zwp_linux_dmabuf_v1 to the web process (with the GPU's formats and
 * modifiers) when it is given an EGLDisplay that can import dma-bufs. GBM on the first render
 * node that works; the display lives as long as the process. Returns 1 when wpebackend-fdo
 * runs in dmabuf mode. */
static struct gbm_device *fdo_gbm;

static int fdo_egl_start(void)
{
	PFNEGLGETPLATFORMDISPLAYEXTPROC get_dpy =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	if (!get_dpy)
		return 0;
	const char *want = getenv("FREEBWS_RENDER_NODE");
	DIR *d = opendir("/dev/dri");
	struct dirent *de;
	int ok = 0;
	while (!ok && d && (de = readdir(d))) {
		char path[300];
		if (strncmp(de->d_name, "renderD", 7) != 0)
			continue;
		snprintf(path, sizeof path, "/dev/dri/%s", de->d_name);
		if (want && *want && strcmp(want, path) != 0)
			continue;
		int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		struct gbm_device *g = gbm_create_device(fd);
		if (!g) {
			close(fd);
			continue;
		}
		EGLDisplay dpy = get_dpy(EGL_PLATFORM_GBM_KHR, g, NULL);
		EGLint maj, min;
		const char *ex;
		if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &maj, &min) ||
		    !(ex = eglQueryString(dpy, EGL_EXTENSIONS)) || !strstr(ex, "EGL_EXT_image_dma_buf_import") ||
		    !strstr(ex, "EGL_EXT_image_dma_buf_import_modifiers") || !wpe_fdo_initialize_for_egl_display(dpy)) {
			fprintf(stderr, "freebws: %s: EGL display cannot be used for dmabuf\n", path);
			if (dpy != EGL_NO_DISPLAY)
				eglTerminate(dpy);
			gbm_device_destroy(g);
			close(fd);
			continue;
		}
		fdo_gbm = g;
		fprintf(stderr, "freebws: dmabuf through %s\n", path);
		ok = 1;
	}
	if (d)
		closedir(d);
	return ok;
}

/* ---- wayland globals ------------------------------------------------------ */

static void dmb_format(void *d, struct zwp_linux_dmabuf_v1 *x, uint32_t f) { }
static void dmb_modifier(void *d, struct zwp_linux_dmabuf_v1 *x, uint32_t f, uint32_t hi, uint32_t lo)
{
	if (hi || lo)
		B.tiled_mods++;
}
static const struct zwp_linux_dmabuf_v1_listener dmabuf_l = { dmb_format, dmb_modifier };

static void on_global(void *ud, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver)
{
	if (!strcmp(iface, "wl_compositor"))
		B.comp = wl_registry_bind(reg, name, &wl_compositor_interface, ver < 4 ? ver : 4);
	else if (!strcmp(iface, "wl_subcompositor"))
		B.subc = wl_registry_bind(reg, name, &wl_subcompositor_interface, 1);
	else if (!strcmp(iface, "wl_shm"))
		B.shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, "wl_seat") && !B.seat)
		B.seat = wl_registry_bind(reg, name, &wl_seat_interface, ver < 5 ? ver : 5);
	else if (!strcmp(iface, "zwp_linux_dmabuf_v1")) {
		B.dmabuf = wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, ver < 3 ? ver : 3);
		zwp_linux_dmabuf_v1_add_listener(B.dmabuf, &dmabuf_l, NULL);
	}
}

int main(int argc, char **argv)
{
	setvbuf(stderr, NULL, _IOLBF, 0);
	if (argc > 1 && !strcmp(argv[1], "--adblock-update"))
		return adb_child_main("update");
	if (argc > 1 && !strcmp(argv[1], "--adblock-build"))
		return adb_child_main("build");
	if (argc > 1 && !strcmp(argv[1], "--sandbox-child"))	/* what sandbox_probe runs inside bwrap */
		return 0;
	cfg_load();
	if (argc > 1 && !strcmp(argv[1], "--sandbox-check")) {
		char why[400];
		int ok = sandbox_probe(why, sizeof why);
		printf("%s\n", ok ? "sandbox: bubblewrap works, the web process sandbox can be on" : why);
		return ok ? 0 : 1;
	}
	if (sandbox_setup())
		return 1;
	const char *start = argc > 1 ? argv[1] : "freebws://start";
	B.home = "freebws://start";
	B.app = fl_app_new(on_global, NULL);
	if (!B.app) {
		fprintf(stderr, "freebws: no Wayland compositor\n");
		return 1;
	}
	B.dpy = fl_app_display(B.app);
	wl_display_roundtrip(B.dpy);	/* modifiers of the dmabuf global */
	if (!B.comp || !B.subc || !B.shm) {
		fprintf(stderr, "freebws: the compositor lacks wl_subcompositor or wl_shm\n");
		return 1;
	}
	const char *mode = getenv("FREEBWS_BUFFERS");
	if (!mode) {	/* buffers=shm|dmabuf in the config file; "shm" is also what the watchdog writes */
		mode = cfg_get("buffers", "auto");
		if (strcmp(mode, "shm") && strcmp(mode, "dmabuf"))
			mode = NULL;
	}
	B.use_dmabuf = mode ? !strcmp(mode, "dmabuf") : (B.dmabuf && B.tiled_mods > 0 && flcomp_takes_planes());
	if (B.use_dmabuf && !B.dmabuf) {
		fprintf(stderr, "freebws: no zwp_linux_dmabuf_v1, using shm\n");
		B.use_dmabuf = 0;
	}

	B.loop = g_main_loop_new(NULL, FALSE);
	if (B.use_dmabuf && !fdo_egl_start()) {
		fprintf(stderr, "freebws: no usable GPU in this process, using shm\n");
		B.use_dmabuf = 0;
	}
	if (!B.use_dmabuf && !wpe_fdo_initialize_shm()) {
		fprintf(stderr, "freebws: wpebackend-fdo does not start (shm)\n");
		return 1;
	}
	B.live_exp = g_hash_table_new(g_direct_hash, g_direct_equal);

	/* the Skia renderer in the web processes: CPU unless a GPU is announced (see "memory") */
	if (!B.use_dmabuf)
		setenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1", 0);
	tune_webkit();
	dssl_init();
	pages_init(B.ctx);
	bm_load();
	dl_init(webkit_network_session_get_default(), on_dl_changed);
	cookies_setup();
	adb_init();

	fl_win_opts o = {
		.title = "FreeBWS", .app_id = "freebws", .w = 1100, .h = 720, .min_w = 480, .min_h = 320,
		.decorated = 1, .resizable = 1, .draw = draw, .close = on_close, .key = on_key,
		.focus = on_focus,
	};
	B.win = fl_win_new(B.app, &o);
	if (!B.win)
		return 1;
	B.page = wl_compositor_create_surface(B.comp);
	B.sub = wl_subcompositor_get_subsurface(B.subc, B.page, fl_win_surface(B.win));
	wl_subsurface_set_desync(B.sub);
	if (B.seat) {
		B.ptr = wl_seat_get_pointer(B.seat);
		wl_pointer_add_listener(B.ptr, &ptr_l, NULL);
	}
	B.pop = popup_new(B.app, B.subc, fl_win_surface(B.win), B.page, pop_draw, NULL);
	if (!tab_create(NULL, start, 1))
		return 1;
	if (argc <= 1)
		focus_url();	/* like a new tab: the address bar is ready to type in */
	fprintf(stderr, "freebws: %s buffers, loading %s\n", B.use_dmabuf ? "dmabuf" : "shm", start);

	start_time = time(NULL);
	if (B.use_dmabuf) {
		B.argv = argv;
		g_timeout_add_seconds(12, dmabuf_watchdog, NULL);
	}
	g_timeout_add_seconds(10, mem_tick, NULL);
	GSource *src = g_source_new(&src_funcs, sizeof(struct flsrc));
	g_source_attach(src, NULL);
	fl_win_redraw(B.win);
	g_main_loop_run(B.loop);
	g_source_destroy(src);
	while (B.ntabs)
		tab_free(B.tabs[--B.ntabs]);
	fl_app_free(B.app);
	return 0;
}
