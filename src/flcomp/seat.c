/* wl_seat (pointer and keyboard), keyboard layouts, key bindings, the
 * clipboard (wl_data_device_manager) and the software cursor.
 * SPDX-License-Identifier: MIT */
#include "comp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

static struct wl_list pointers, keyboards, data_devices;	/* struct reslink */

/* ---- keymap ------------------------------------------------------------ */

static struct xkb_context *xctx;
static struct xkb_keymap *keymap;
static struct xkb_state *xstate;
static int keymap_fd = -1;
static uint32_t keymap_size;
static char layout_name[64] = "us";

static void load_keymap_fd(void)
{
	if (keymap_fd >= 0)
		close(keymap_fd);
	keymap_fd = -1;
	keymap_size = 0;
	if (!keymap)
		return;
	char *str = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
	if (!str)
		return;
	size_t len = strlen(str) + 1;
	int fd = memfd_create("flcomp-keymap", MFD_CLOEXEC);
	if (fd >= 0 && write(fd, str, len) == (ssize_t)len) {
		keymap_fd = fd;
		keymap_size = (uint32_t)len;
	} else if (fd >= 0) {
		close(fd);
	}
	free(str);
}

static void send_keymap(struct wl_resource *kb)
{
	if (keymap_fd >= 0) {
		wl_keyboard_send_keymap(kb, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, keymap_fd, keymap_size);
	} else {
		int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
		wl_keyboard_send_keymap(kb, WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP, fd, 0);
		if (fd >= 0)
			close(fd);
	}
}

static void send_modifiers_to(struct wl_resource *kb)
{
	if (!xstate)
		return;
	wl_keyboard_send_modifiers(kb, wl_display_next_serial(C.dpy),
		xkb_state_serialize_mods(xstate, XKB_STATE_MODS_DEPRESSED),
		xkb_state_serialize_mods(xstate, XKB_STATE_MODS_LATCHED),
		xkb_state_serialize_mods(xstate, XKB_STATE_MODS_LOCKED),
		xkb_state_serialize_layout(xstate, XKB_STATE_LAYOUT_EFFECTIVE));
}

static void for_focus_keyboards(void (*fn)(struct wl_resource *))
{
	if (!C.kb_focus)
		return;
	struct wl_client *cl = wl_resource_get_client(C.kb_focus->res);
	struct reslink *k;
	wl_list_for_each(k, &keyboards, link)
		if (wl_resource_get_client(k->res) == cl)
			fn(k->res);
}

static bool numlock_on(void)
{
	return xstate && xkb_state_mod_name_is_active(xstate, XKB_MOD_NAME_NUM, XKB_STATE_MODS_LOCKED) > 0;
}

/* the keyboard LEDs follow the xkb state */
static void update_leds(void)
{
	if (!xstate)
		return;
	int leds = 0;
	if (xkb_state_led_name_is_active(xstate, XKB_LED_NAME_NUM) > 0)
		leds |= 1;
	if (xkb_state_led_name_is_active(xstate, XKB_LED_NAME_CAPS) > 0)
		leds |= 2;
	if (xkb_state_led_name_is_active(xstate, XKB_LED_NAME_SCROLL) > 0)
		leds |= 4;
	backend_set_leds(leds);
}

/* Num Lock on or off: the key press and release that toggles it (key 69, +8 for xkb) */
static void numlock_set(bool on)
{
	if (!xstate || numlock_on() == on)
		return;
	xkb_state_update_key(xstate, 69 + 8, XKB_KEY_DOWN);
	xkb_state_update_key(xstate, 69 + 8, XKB_KEY_UP);
	for_focus_keyboards(send_modifiers_to);
	update_leds();
}

/* "de", or "de:nodeadkeys" for layout and variant. */
static bool numlock_wanted;

static bool build_keymap(const char *name, const char *options)
{
	char lay[64], var[64] = "";
	snprintf(lay, sizeof(lay), "%s", name);
	char *colon = strchr(lay, ':');
	if (colon) {
		*colon = 0;
		snprintf(var, sizeof(var), "%s", colon + 1);
	}
	struct xkb_rule_names rn = {
		.layout = lay, .variant = var[0] ? var : NULL, .options = options,
	};
	struct xkb_keymap *km = xkb_keymap_new_from_names(xctx, &rn, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!km)
		return false;
	struct xkb_state *st = xkb_state_new(km);
	if (!st) {
		xkb_keymap_unref(km);
		return false;
	}
	bool was_numlock = xstate ? numlock_on() : numlock_wanted;
	if (xstate)
		xkb_state_unref(xstate);
	if (keymap)
		xkb_keymap_unref(keymap);
	keymap = km;
	xstate = st;
	snprintf(layout_name, sizeof(layout_name), "%s", name);
	load_keymap_fd();
	numlock_set(was_numlock);
	return true;
}

static char kb_options[128];

bool seat_set_layout(const char *name)
{
	if (!name || !*name || !xctx)
		return false;
	if (!build_keymap(name, kb_options[0] ? kb_options : NULL)) {
		fprintf(stderr, "flcomp: cannot load keyboard layout '%s'\n", name);
		return false;
	}
	struct reslink *k;
	wl_list_for_each(k, &keyboards, link)
		send_keymap(k->res);
	for_focus_keyboards(send_modifiers_to);
	fl_emit_layout();
	return true;
}

const char *seat_layout(void)
{
	return layout_name;
}

/* /etc/keyboard.conf: LAYOUT=de, VARIANT=nodeadkeys, OPTIONS=ctrl:nocaps */
static void read_config(char *layout, size_t n)
{
	const char *path = getenv("FLCOMP_KEYBOARD_CONF");
	FILE *f = fopen(path ? path : "/etc/keyboard.conf", "r");
	char var[64] = "";
	char line[256];
	while (f && fgets(line, sizeof(line), f)) {
		char *nl = strchr(line, '\n');
		if (nl)
			*nl = 0;
		char *eq = strchr(line, '=');
		if (!eq || line[0] == '#')
			continue;
		*eq++ = 0;
		if (*eq == '"' || *eq == '\'') {
			eq++;
			char *q = strpbrk(eq, "\"'");
			if (q)
				*q = 0;
		}
		if (!strcmp(line, "LAYOUT"))
			snprintf(layout, n, "%s", eq);
		else if (!strcmp(line, "VARIANT"))
			snprintf(var, sizeof(var), "%s", eq);
		else if (!strcmp(line, "OPTIONS"))
			snprintf(kb_options, sizeof(kb_options), "%s", eq);
	}
	if (f)
		fclose(f);
	/* the person's own choice from Settings wins */
	const char *home = getenv("HOME");
	char conf[512];
	snprintf(conf, sizeof(conf), "%s/.config/freelfs/desktop.conf", home ? home : "");
	if (home && !path && (f = fopen(conf, "r"))) {
		while (fgets(line, sizeof(line), f))
			if (!strncmp(line, "keyboard=", 9)) {
				line[strcspn(line, "\n")] = 0;
				if (line[9])
					snprintf(layout, n, "%s", line + 9);
			}
		fclose(f);
	}
	if (!strncmp(layout, "uk", 2) && (!layout[2] || layout[2] == ':'))
		memcpy(layout, "gb", 2); /* xkb's name for it */
	/* a list "de,us" is not supported: take the first */
	char *comma = strchr(layout, ',');
	if (comma)
		*comma = 0;
	if (var[0] && !strchr(layout, ':')) {
		size_t l = strlen(layout);
		snprintf(layout + l, n - l, ":%s", var);
	}
}

/* ---- cursor --------------------------------------------------------------- */

#define CUR_W 12
#define CUR_H 19
static const char *cursor_img[CUR_H] = {
	"X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ",
	"X....X      ", "X.....X     ", "X......X    ", "X.......X   ", "X........X  ",
	"X.....XXXXX ", "X..X..X     ", "X.X X..X    ", "XX  X..X    ", "X    X..X   ",
	"     X..X   ", "      X..X  ", "      X..X  ", "       XX   ",
};
static struct rect cur_last;

bool cursor_rect(struct rect *r)
{
	if (!C.has_pointer)
		return false;
	r->x1 = (int)C.px;
	r->y1 = (int)C.py;
	r->x2 = r->x1 + CUR_W;
	r->y2 = r->y1 + CUR_H;
	return true;
}

void cursor_damage(void)
{
	struct rect r;
	if (!C.has_pointer)
		return;
	damage_add(cur_last);
	if (cursor_rect(&r)) {
		damage_add(r);
		cur_last = r;
	}
}

void cursor_draw(struct rect clip)
{
	struct rect r, o;
	if (!cursor_rect(&r) || !rect_intersect(r, clip, &o))
		return;
	for (int y = o.y1; y < o.y2; y++)
		for (int x = o.x1; x < o.x2; x++) {
			char c = cursor_img[y - r.y1][x - r.x1];
			if (c == 'X')
				C.shadow[(size_t)y * C.w + x] = 0x001d2125u;
			else if (c == '.')
				C.shadow[(size_t)y * C.w + x] = 0x00f4f5f7u;
		}
}

/* ---- pointer ----------------------------------------------------------------- */

static void ptr_frame(struct wl_client *cl)
{
	struct reslink *p;
	wl_list_for_each(p, &pointers, link)
		if (wl_resource_get_client(p->res) == cl &&
		    wl_resource_get_version(p->res) >= WL_POINTER_FRAME_SINCE_VERSION)
			wl_pointer_send_frame(p->res);
}

void seat_set_ptr_focus(struct surface *s, double sx, double sy)
{
	if (s == C.ptr_focus)
		return;
	struct reslink *p;
	if (C.ptr_focus) {
		struct wl_client *cl = wl_resource_get_client(C.ptr_focus->res);
		uint32_t serial = wl_display_next_serial(C.dpy);
		wl_list_for_each(p, &pointers, link)
			if (wl_resource_get_client(p->res) == cl)
				wl_pointer_send_leave(p->res, serial, C.ptr_focus->res);
		ptr_frame(cl);
	}
	C.ptr_focus = s;
	if (s) {
		struct wl_client *cl = wl_resource_get_client(s->res);
		uint32_t serial = wl_display_next_serial(C.dpy);
		wl_list_for_each(p, &pointers, link)
			if (wl_resource_get_client(p->res) == cl)
				wl_pointer_send_enter(p->res, serial, s->res,
					wl_fixed_from_double(sx), wl_fixed_from_double(sy));
		ptr_frame(cl);
	}
}

static void send_motion(uint32_t ms)
{
	if (!C.ptr_focus)
		return;
	int ox, oy;
	surface_origin(C.ptr_focus, &ox, &oy);
	struct wl_client *cl = wl_resource_get_client(C.ptr_focus->res);
	struct reslink *p;
	wl_list_for_each(p, &pointers, link)
		if (wl_resource_get_client(p->res) == cl)
			wl_pointer_send_motion(p->res, ms, wl_fixed_from_double(C.px - ox),
					       wl_fixed_from_double(C.py - oy));
	ptr_frame(cl);
}

/* Pick the surface under the pointer (unless a button is held: then the
 * surface that got the press keeps the pointer). */
void seat_refocus(void)
{
	if (grab_active()) {
		seat_set_ptr_focus(NULL, 0, 0);
		return;
	}
	if (C.btn_down > 0 && C.ptr_focus)
		return;
	int lx, ly;
	struct surface *t = pick((int)C.px, (int)C.py, &lx, &ly);
	seat_set_ptr_focus(t, lx, ly);
}

static void motion_common(uint32_t ms)
{
	cursor_damage();
	if (pmenu_active()) {	/* the compositor's popup (popup.c) owns the pointer */
		pmenu_motion();
		return;
	}
	if (grab_active()) {
		grab_motion();
		return;
	}
	if (overview_on() && overview_has_pointer()) {	/* Super+Tab: the pointer belongs to the overview */
		overview_motion();
		seat_set_ptr_focus(NULL, 0, 0);
		return;
	}
	if (!(C.btn_down > 0 && C.ptr_focus)) {
		int lx, ly;
		struct surface *t = pick((int)C.px, (int)C.py, &lx, &ly);
		seat_set_ptr_focus(t, lx, ly);
		xwm_deco_hover();	/* the buttons of an X window's frame light up */
	}
	send_motion(ms);
}

static void clamp_pointer(void)
{
	if (C.px < 0)
		C.px = 0;
	if (C.py < 0)
		C.py = 0;
	if (C.px > C.w - 1)
		C.px = C.w - 1;
	if (C.py > C.h - 1)
		C.py = C.h - 1;
}

void seat_motion(uint32_t ms, double dx, double dy)
{
	C.px += dx;
	C.py += dy;
	clamp_pointer();
	motion_common(ms);
}

void seat_motion_abs(uint32_t ms, double x, double y)
{
	C.px = x;
	C.py = y;
	clamp_pointer();
	motion_common(ms);
}

/* A press outside the menu or the grabbing popups closes them. */
static void press_outside(struct surface *t)
{
	struct surface *root = root_of(t);
	if (menu_visible() && !(root && (root->role == ROLE_MENU || root->role == ROLE_PANEL)))
		menu_dismiss();
	if (flyout_visible() && !(root && (root->role == ROLE_FLYOUT || root->role == ROLE_PANEL)))
		flyout_dismiss();
	if (!root || root->role != ROLE_POPUP)
		popups_dismiss();
}

void seat_button(uint32_t ms, uint32_t button, bool pressed)
{
	if (pmenu_button(button, pressed))
		return;
	if (button == 272 && overview_button(pressed))	/* BTN_LEFT on the workspace overview */
		return;
	if (pressed) {
		if (C.btn_down == 0) {
			int lx, ly;
			struct surface *t = pick((int)C.px, (int)C.py, &lx, &ly);
			press_outside(t);
			struct window *dw = t ? NULL : xwm_deco_target();	/* the frame of an X window */
			if (dw) {
				window_focus(dw);
				C.btn_down++;
				if (button == 272)
					xwm_deco_press(dw);
				return;
			}
			struct window *w = t ? window_of_surface(t) : NULL;
			if (w && (button == 273 || button == 274) && title_click(w, button, (int)C.px, (int)C.py))
				return;	/* right: window menu, middle: close (popup.c) */
			if (w)
				window_focus(w);
			if (!grab_active() && t != C.ptr_focus)
				seat_refocus();
		}
		C.btn_down++;
	} else if (C.btn_down > 0) {
		C.btn_down--;
		if (xwm_deco_active())
			xwm_deco_release();
	}
	if (grab_active()) {
		if (!pressed && C.btn_down == 0) {
			grab_end();
			seat_refocus();
		}
		return;
	}
	if (C.ptr_focus) {
		struct wl_client *cl = wl_resource_get_client(C.ptr_focus->res);
		uint32_t serial = wl_display_next_serial(C.dpy);
		struct reslink *p;
		wl_list_for_each(p, &pointers, link)
			if (wl_resource_get_client(p->res) == cl)
				wl_pointer_send_button(p->res, serial, ms, button,
					pressed ? WL_POINTER_BUTTON_STATE_PRESSED
						: WL_POINTER_BUTTON_STATE_RELEASED);
		ptr_frame(cl);
	}
	if (!pressed && C.btn_down == 0)
		seat_refocus();
}

/* axis: 0 vertical, 1 horizontal; source: 0 wheel, 1 finger, 2 continuous.
 * value in pixels; 0 with source finger means the scroll stopped. */
void seat_axis(uint32_t ms, int axis, double value, int discrete, int source)
{
	if (!C.ptr_focus || grab_active())
		return;
	struct wl_client *cl = wl_resource_get_client(C.ptr_focus->res);
	struct reslink *p;
	wl_list_for_each(p, &pointers, link) {
		if (wl_resource_get_client(p->res) != cl)
			continue;
		int v = wl_resource_get_version(p->res);
		if (v >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION)
			wl_pointer_send_axis_source(p->res, source);
		if (value == 0.0 && source == 1) {
			if (v >= WL_POINTER_AXIS_STOP_SINCE_VERSION)
				wl_pointer_send_axis_stop(p->res, ms, axis);
			continue;
		}
		wl_pointer_send_axis(p->res, ms, axis, wl_fixed_from_double(value));
		if (discrete && v >= WL_POINTER_AXIS_DISCRETE_SINCE_VERSION)
			wl_pointer_send_axis_discrete(p->res, axis, discrete);
	}
	ptr_frame(cl);
}

/* ---- clipboard ------------------------------------------------------------ */

static void capture_selection(void);

struct mime { char *s; struct wl_list link; };
struct dsource {
	struct wl_resource *res;		/* NULL for a foreign source (an X11 client's selection, xwsel.c) */
	struct wl_list mimes, offers;		/* struct mime, struct doffer */
	bool foreign;
	const struct fsel_ops *fops;
	void *fdata;
};
struct doffer {
	struct wl_resource *res;
	struct dsource *src;
	struct wl_list link;
};

static struct dsource *selection;
static void (*sel_watch)(void);		/* xwsel.c: told when a Wayland client owns the selection, or nobody does */

static void sel_changed(void)
{
	if (sel_watch)
		sel_watch();
}

void seat_selection_watch(void (*cb)(void))
{
	sel_watch = cb;
}

static void ds_offer(struct wl_client *c, struct wl_resource *r, const char *mime)
{
	struct dsource *s = wl_resource_get_user_data(r);
	struct mime *m = calloc(1, sizeof(*m));
	if (m && (m->s = strdup(mime)))
		wl_list_insert(s->mimes.prev, &m->link);
	else
		free(m);
}

static void ds_destroy_req(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static void ds_set_actions(struct wl_client *c, struct wl_resource *r, uint32_t a) { }

static const struct wl_data_source_interface dsource_impl = {
	.offer = ds_offer, .destroy = ds_destroy_req, .set_actions = ds_set_actions,
};

static void send_selection_to(struct wl_resource *dev);

static void selection_broadcast(void)
{
	if (!C.kb_focus)
		return;
	struct wl_client *cl = wl_resource_get_client(C.kb_focus->res);
	struct reslink *d;
	wl_list_for_each(d, &data_devices, link)
		if (wl_resource_get_client(d->res) == cl)
			send_selection_to(d->res);
}

static void dsource_destroy(struct wl_resource *res)
{
	struct dsource *s = wl_resource_get_user_data(res);
	struct doffer *o;
	wl_list_for_each(o, &s->offers, link)
		o->src = NULL;
	struct mime *m, *t;
	wl_list_for_each_safe(m, t, &s->mimes, link) {
		wl_list_remove(&m->link);
		free(m->s);
		free(m);
	}
	if (selection == s) {
		selection = NULL;
		selection_broadcast();
		sel_changed();
	}
	free(s);
}

static void offer_accept(struct wl_client *c, struct wl_resource *r, uint32_t serial,
			 const char *mime) { }
static void offer_finish(struct wl_client *c, struct wl_resource *r) { }
static void offer_set_actions(struct wl_client *c, struct wl_resource *r, uint32_t a,
			      uint32_t p) { }

static void offer_receive(struct wl_client *c, struct wl_resource *r, const char *mime, int32_t fd)
{
	struct doffer *o = wl_resource_get_user_data(r);
	if (o && o->src && o->src->foreign) {
		o->src->fops->send(o->src->fdata, mime, fd);	/* takes the descriptor */
		return;
	}
	if (o && o->src)
		wl_data_source_send_send(o->src->res, mime, fd);
	close(fd);
}

static void offer_destroy_req(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static const struct wl_data_offer_interface offer_impl = {
	.accept = offer_accept, .receive = offer_receive, .destroy = offer_destroy_req,
	.finish = offer_finish, .set_actions = offer_set_actions,
};

static void offer_destroy(struct wl_resource *res)
{
	struct doffer *o = wl_resource_get_user_data(res);
	if (o->src)
		wl_list_remove(&o->link);
	free(o);
}

static void send_selection_to(struct wl_resource *dev)
{
	if (!selection) {
		wl_data_device_send_selection(dev, NULL);
		return;
	}
	struct doffer *o = calloc(1, sizeof(*o));
	if (!o)
		return;
	o->res = wl_resource_create(wl_resource_get_client(dev), &wl_data_offer_interface,
				    wl_resource_get_version(dev), 0);
	if (!o->res) {
		free(o);
		return;
	}
	o->src = selection;
	wl_list_insert(&selection->offers, &o->link);
	wl_resource_set_implementation(o->res, &offer_impl, o, offer_destroy);
	wl_data_device_send_data_offer(dev, o->res);
	struct mime *m;
	wl_list_for_each(m, &selection->mimes, link)
		wl_data_offer_send_offer(o->res, m->s);
	wl_data_device_send_selection(dev, o->res);
}

/* a foreign source goes away: its offers lose it, its owner (xwsel.c) hears of it */
static void fsrc_free(struct dsource *s)
{
	struct doffer *o;
	wl_list_for_each(o, &s->offers, link)
		o->src = NULL;
	struct mime *m, *t;
	wl_list_for_each_safe(m, t, &s->mimes, link) {
		wl_list_remove(&m->link);
		free(m->s);
		free(m);
	}
	free(s);
}

static void selection_cancel(struct dsource *s)
{
	if (s->foreign) {
		const struct fsel_ops *ops = s->fops;
		void *d = s->fdata;
		fsrc_free(s);
		ops->cancelled(d);
	} else {
		wl_data_source_send_cancelled(s->res);
	}
}

void seat_set_foreign_selection(const char *const *mimes, int n, const struct fsel_ops *ops, void *data)
{
	struct dsource *s = calloc(1, sizeof(*s));
	if (!s) {
		ops->cancelled(data);
		return;
	}
	s->foreign = true;
	s->fops = ops;
	s->fdata = data;
	wl_list_init(&s->mimes);
	wl_list_init(&s->offers);
	for (int i = 0; i < n; i++) {
		struct mime *m = calloc(1, sizeof(*m));
		if (m && (m->s = strdup(mimes[i])))
			wl_list_insert(s->mimes.prev, &m->link);
		else
			free(m);
	}
	struct dsource *old = selection;
	selection = s;
	if (old)
		selection_cancel(old);
	selection_broadcast();
	capture_selection();
}

void seat_clear_foreign_selection(void *data)
{
	if (!selection || !selection->foreign || selection->fdata != data)
		return;
	struct dsource *s = selection;
	selection = NULL;
	selection_cancel(s);
	selection_broadcast();
}

int seat_wayland_selection_mimes(const char **out, int max)
{
	if (!selection || selection->foreign)
		return 0;
	int n = 0;
	struct mime *m;
	wl_list_for_each(m, &selection->mimes, link)
		if (n < max)
			out[n++] = m->s;
	return n;
}

bool seat_wayland_selection_send(const char *mime, int fd)
{
	if (!selection || selection->foreign)
		return false;
	wl_data_source_send_send(selection->res, mime, fd);
	return true;
}

/* ---- clipboard history: the last 20 text entries, in RAM only ---------------------------------- */

#define HIST_N 20
#define HIST_MAX (32 * 1024)		/* longer text is not kept */
static char *hist[HIST_N];
static int hist_n;
static bool hist_off;			/* FLCOMP_NO_CLIPHIST=1: no history, Ctrl+V is never delayed */

int cliphist_count(void)
{
	return hist_n;
}

const char *cliphist_get(int i)
{
	return i >= 0 && i < hist_n ? hist[i] : "";
}

static void hist_push(char *t)
{
	size_t k = 0;
	while (t[k] == ' ' || t[k] == '\n' || t[k] == '\t' || t[k] == '\r')
		k++;
	if (!t[k]) {
		free(t);
		return;
	}
	for (int i = 0; i < hist_n; i++)
		if (!strcmp(hist[i], t)) {
			free(hist[i]);
			memmove(&hist[i], &hist[i + 1], (size_t)(hist_n - i - 1) * sizeof hist[0]);
			hist_n--;
			break;
		}
	if (hist_n == HIST_N)
		free(hist[--hist_n]);
	memmove(&hist[1], &hist[0], (size_t)hist_n * sizeof hist[0]);
	hist[0] = t;
	hist_n++;
}

/* what the current owner of the clipboard offers, read through a pipe while it is still alive */
static struct {
	int fd;
	char *buf;
	size_t len;
	struct wl_event_source *src, *timer;
} cap = { .fd = -1 };

static void cap_end(bool keep)
{
	if (cap.src)
		wl_event_source_remove(cap.src);
	if (cap.timer)
		wl_event_source_timer_update(cap.timer, 0);
	cap.src = NULL;
	if (cap.fd >= 0)
		close(cap.fd);
	cap.fd = -1;
	if (keep && cap.buf && cap.len) {
		char *t = realloc(cap.buf, cap.len + 1);
		if (t) {
			t[cap.len] = 0;
			hist_push(t);
			cap.buf = NULL;
		}
	}
	free(cap.buf);
	cap.buf = NULL;
	cap.len = 0;
}

static int cap_cb(int fd, uint32_t mask, void *data)
{
	char tmp[4096];
	for (;;) {
		ssize_t n = read(fd, tmp, sizeof tmp);
		if (n > 0) {
			if (cap.len + (size_t)n > HIST_MAX) {
				cap_end(false);	/* too big: not kept */
				return 0;
			}
			char *nb = realloc(cap.buf, cap.len + (size_t)n);
			if (!nb) {
				cap_end(false);
				return 0;
			}
			cap.buf = nb;
			memcpy(cap.buf + cap.len, tmp, (size_t)n);
			cap.len += (size_t)n;
			continue;
		}
		if (n < 0 && errno == EAGAIN)
			return 0;
		cap_end(n == 0);
		return 0;
	}
}

static int cap_timeout(void *data)
{
	cap_end(false);
	return 0;
}

static const struct fsel_ops hist_ops;

static void capture_selection(void)
{
	cap_end(false);
	if (hist_off || !selection || (selection->foreign && selection->fops == &hist_ops) || !C.loop)
		return;
	const char *want[] = { "text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", NULL };
	const char *mime = NULL;
	for (int i = 0; want[i] && !mime; i++) {
		struct mime *m;
		wl_list_for_each(m, &selection->mimes, link)
			if (!strcmp(m->s, want[i])) {
				mime = m->s;
				break;
			}
	}
	if (!mime)
		return;
	int p[2];
	if (pipe2(p, O_CLOEXEC | O_NONBLOCK) < 0)
		return;
	if (selection->foreign) {
		fcntl(p[1], F_SETFL, 0);
		selection->fops->send(selection->fdata, mime, p[1]);	/* takes it */
	} else {
		wl_data_source_send_send(selection->res, mime, p[1]);
		close(p[1]);
	}
	cap.fd = p[0];
	cap.src = wl_event_loop_add_fd(C.loop, p[0], WL_EVENT_READABLE, cap_cb, NULL);
	if (!cap.timer)
		cap.timer = wl_event_loop_add_timer(C.loop, cap_timeout, NULL);
	if (cap.timer)
		wl_event_source_timer_update(cap.timer, 1500);
}

/* the history's own clipboard source */
static char *self_text;

static void hs_send(void *d, const char *mime, int fd)
{
	if (self_text) {
		size_t n = strlen(self_text), off = 0;
		fcntl(fd, F_SETFL, O_NONBLOCK);
		while (off < n) {
			ssize_t w = write(fd, self_text + off, n - off);
			if (w <= 0 && errno != EINTR)
				break;
			if (w > 0)
				off += (size_t)w;
		}
	}
	close(fd);
}

static void hs_cancelled(void *d) { }

static const struct fsel_ops hist_ops = { .send = hs_send, .cancelled = hs_cancelled };

static uint32_t paste_code = 47;	/* the key code of the V that opened the popup */

static void synth_paste(void);

void cliphist_paste(int i)
{
	if (i < 0 || i >= hist_n)
		return;
	free(self_text);
	self_text = strdup(hist[i]);
	if (!self_text)
		return;
	static const char *const mimes[] = { "text/plain;charset=utf-8", "text/plain" };
	seat_set_foreign_selection(mimes, 2, &hist_ops, NULL);
	synth_paste();
}

static void dd_start_drag(struct wl_client *c, struct wl_resource *r, struct wl_resource *src,
			  struct wl_resource *origin, struct wl_resource *icon, uint32_t serial)
{
	/* drag and drop is not supported yet */
}

static void dd_set_selection(struct wl_client *c, struct wl_resource *r, struct wl_resource *src,
			     uint32_t serial)
{
	struct dsource *s = src ? wl_resource_get_user_data(src) : NULL;
	if (s == selection)
		return;
	struct dsource *old = selection;
	selection = s;
	if (old)
		selection_cancel(old);
	selection_broadcast();
	sel_changed();
	capture_selection();
}

static void dd_release(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static const struct wl_data_device_interface ddev_impl = {
	.start_drag = dd_start_drag, .set_selection = dd_set_selection, .release = dd_release,
};

static void ddm_create_source(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	struct dsource *s = calloc(1, sizeof(*s));
	if (!s) {
		wl_client_post_no_memory(c);
		return;
	}
	s->res = wl_resource_create(c, &wl_data_source_interface, wl_resource_get_version(r), id);
	if (!s->res) {
		free(s);
		wl_client_post_no_memory(c);
		return;
	}
	wl_list_init(&s->mimes);
	wl_list_init(&s->offers);
	wl_resource_set_implementation(s->res, &dsource_impl, s, dsource_destroy);
}

static void ddm_get_device(struct wl_client *c, struct wl_resource *r, uint32_t id,
			   struct wl_resource *seat)
{
	struct wl_resource *res = wl_resource_create(c, &wl_data_device_interface,
						     wl_resource_get_version(r), id);
	if (!res) {
		wl_client_post_no_memory(c);
		return;
	}
	struct reslink *l = reslink_add(&data_devices, res);
	if (!l) {
		wl_client_post_no_memory(c);
		return;
	}
	wl_resource_set_implementation(res, &ddev_impl, l, reslink_destroy);
	if (C.kb_focus && wl_resource_get_client(C.kb_focus->res) == c)
		send_selection_to(res);
}

static const struct wl_data_device_manager_interface ddm_impl = {
	.create_data_source = ddm_create_source, .get_data_device = ddm_get_device,
};

static void ddm_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &wl_data_device_manager_interface, version, id);
	if (!r) {
		wl_client_post_no_memory(c);
		return;
	}
	wl_resource_set_implementation(r, &ddm_impl, NULL, NULL);
}

/* ---- keyboard focus ------------------------------------------------------ */

void seat_set_kb_focus(struct surface *s)
{
	if (s == C.kb_focus)
		return;
	struct reslink *k;
	if (C.kb_focus) {
		struct wl_client *cl = wl_resource_get_client(C.kb_focus->res);
		uint32_t serial = wl_display_next_serial(C.dpy);
		wl_list_for_each(k, &keyboards, link)
			if (wl_resource_get_client(k->res) == cl)
				wl_keyboard_send_leave(k->res, serial, C.kb_focus->res);
	}
	C.kb_focus = s;
	if (!s)
		return;
	struct wl_client *cl = wl_resource_get_client(s->res);
	uint32_t serial = wl_display_next_serial(C.dpy);
	struct wl_array keys;
	wl_array_init(&keys);
	wl_list_for_each(k, &keyboards, link)
		if (wl_resource_get_client(k->res) == cl) {
			wl_keyboard_send_enter(k->res, serial, s->res, &keys);
			send_modifiers_to(k->res);
		}
	wl_array_release(&keys);
	struct reslink *d;
	wl_list_for_each(d, &data_devices, link)
		if (wl_resource_get_client(d->res) == cl)
			send_selection_to(d->res);
}

void seat_surface_gone(struct surface *s)
{
	if (C.ptr_focus == s)
		C.ptr_focus = NULL;
	if (C.kb_focus == s)
		C.kb_focus = NULL;
}

/* ---- keys and bindings ------------------------------------------------------- */

static bool consumed[768];
static bool super_pending;

static bool mod_active(const char *name)
{
	return xstate && xkb_state_mod_name_is_active(xstate, name, XKB_STATE_MODS_DEPRESSED) > 0;
}

static bool handle_binding(xkb_keysym_t sym, bool pressed)
{
	if (pmenu_active())
		return pmenu_key(sym, pressed);
	int ma = media_action(sym);
	if (ma >= 0) {
		media_key(ma, pressed);	/* volume, brightness, microphone: media.c */
		return true;
	}
	if (overview_on() && !(mod_active(XKB_MOD_NAME_ALT) && mod_active(XKB_MOD_NAME_CTRL)))
		return overview_key(sym, pressed);	/* Super+Tab: every key is the overview's until it is closed */
	if (sym == XKB_KEY_Super_L || sym == XKB_KEY_Super_R) {
		if (pressed) {
			super_pending = true;
		} else if (super_pending) {
			super_pending = false;
			fl_emit_menu_toggle();
		}
		return true;
	}
	if (!pressed)
		return false;
	super_pending = false;
	bool alt = mod_active(XKB_MOD_NAME_ALT), ctrl = mod_active(XKB_MOD_NAME_CTRL);
	bool shift = mod_active(XKB_MOD_NAME_SHIFT), logo = mod_active(XKB_MOD_NAME_LOGO);
	xkb_keysym_t low = xkb_keysym_to_lower(sym);
	if (sym == XKB_KEY_Print && !C.locked && !alt && !ctrl && !logo) {	/* Snappel, or the whole screen to ~/pictures (shot.c) */
		screenshot_key(shift ? SHOT_REGION : SHOT_FULL);
		return true;
	}
	if (alt && !ctrl && !logo && (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab)) {
		if (shift || sym == XKB_KEY_ISO_Left_Tab)	/* Alt-Shift-Tab: backwards */
			cycle_prev();
		else
			cycle_next();
		return true;
	}
	if (alt && ctrl && !logo && low == XKB_KEY_t && !C.nolock) {	/* Ctrl-Alt-T: a terminal */
		run_program("flterm");
		return true;
	}
	if (ctrl && shift && !alt && !logo && sym == XKB_KEY_Escape && !C.nolock) {	/* Ctrl-Shift-Esc: General Manager */
		run_program("general-manager");
		return true;
	}
	if (logo && !alt && (low == XKB_KEY_Left || low == XKB_KEY_Right || low == XKB_KEY_Up || low == XKB_KEY_Down) &&
	    ctrl != shift) {	/* workspaces: Super+Ctrl+arrows switch, Super+Shift+arrows take the window along */
		int dx = low == XKB_KEY_Left ? -1 : low == XKB_KEY_Right ? 1 : 0;
		int dy = low == XKB_KEY_Up ? -1 : low == XKB_KEY_Down ? 1 : 0;
		if (ctrl)
			ws_step(dx, dy);
		else
			ws_move_focused(dx, dy);
		return true;
	}
	if (logo && !alt && !ctrl && (sym == XKB_KEY_Tab || sym == XKB_KEY_ISO_Left_Tab)) {	/* the workspace overview */
		overview_toggle();
		return true;
	}
	if (logo && shift && !alt && !ctrl && low == XKB_KEY_s) {	/* Super+Shift+S: the Snappel window */
		screenshot_key(SHOT_WINDOW);
		return true;
	}
	if (logo && !alt && !ctrl) {
		switch (low) {
		case XKB_KEY_Left: window_action(WA_SNAP_LEFT); return true;
		case XKB_KEY_Right: window_action(WA_SNAP_RIGHT); return true;
		case XKB_KEY_Up: window_action(WA_MAXIMIZE); return true;
		case XKB_KEY_Down: window_action(WA_RESTORE_OR_MINIMIZE); return true;
		case XKB_KEY_e: if (C.nolock) break; run_program("flfiles"); return true;
		case XKB_KEY_t: if (C.nolock) break; run_program("flterm"); return true;
		case XKB_KEY_l:		/* only where there is something to lock with (the panel's Lock button too) */
			if (lock_available()) {
				lock_start();	/* locks at once; the lock program shows the clock and the password field */
				return true;
			}
			break;
		}
	}
	if (alt && ctrl && (sym == XKB_KEY_BackSpace || sym == XKB_KEY_Terminate_Server)) {
		quit(0);
		return true;
	}
	if (alt && ctrl) {
		int vt = 0;
		if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12)
			vt = sym - XKB_KEY_XF86Switch_VT_1 + 1;
		else if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F12)
			vt = sym - XKB_KEY_F1 + 1;
		if (vt) {
			backend_switch_vt(vt);
			return true;
		}
	}
	return false;
}

/* While the screen is locked only these keys do anything here; the rest goes to the lock screen:
 * the media keys (volume and brightness), Ctrl-Alt-F1..F12 to switch the virtual terminal, and
 * Enter on the "lock program is not running" screen. No Super, Alt-Tab, Print or
 * Ctrl-Alt-Backspace. */
static bool locked_binding(xkb_keysym_t sym, bool pressed)
{
	int ma = media_action(sym);
	if (ma >= 0) {
		media_key(ma, pressed);
		return true;
	}
	if (!pressed)
		return false;
	if (mod_active(XKB_MOD_NAME_ALT) && mod_active(XKB_MOD_NAME_CTRL)) {
		int vt = 0;
		if (sym >= XKB_KEY_XF86Switch_VT_1 && sym <= XKB_KEY_XF86Switch_VT_12)
			vt = sym - XKB_KEY_XF86Switch_VT_1 + 1;
		else if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F12)
			vt = sym - XKB_KEY_F1 + 1;
		if (vt) {
			backend_switch_vt(vt);
			return true;
		}
	}
	if ((sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) && lock_hint_shown())
		return lock_retry();
	return false;
}

static void forward_key(uint32_t ms, uint32_t key, bool pressed)
{
	if (!C.kb_focus)
		return;
	struct wl_client *cl = wl_resource_get_client(C.kb_focus->res);
	uint32_t serial = wl_display_next_serial(C.dpy);
	struct reslink *k;
	wl_list_for_each(k, &keyboards, link)
		if (wl_resource_get_client(k->res) == cl)
			wl_keyboard_send_key(k->res, serial, ms, key,
				pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
}

/* Ctrl+V and the clipboard history: the press of V is held back for up to 450 ms. Released before that, the
 * press and the release go to the program unchanged (a quick Ctrl+V pastes as always, only a little later);
 * still held after that, the history popup opens and the key is not sent at all. */
#define HOLD_MS 450
static struct {
	bool on;
	uint32_t key, ms;
} pend;
static struct wl_event_source *pend_timer;

static void pend_flush(void)	/* hand the held press on */
{
	if (!pend.on)
		return;
	pend.on = false;
	if (pend_timer)
		wl_event_source_timer_update(pend_timer, 0);
	forward_key(pend.ms, pend.key, true);
}

static int pend_cb(void *data)
{
	if (!pend.on)
		return 0;
	pend.on = false;
	paste_code = pend.key;
	consumed[pend.key] = true;	/* its release is the popup's */
	cliphist_menu_open();
	if (!pmenu_active())
		forward_key(pend.ms, pend.key, true), consumed[pend.key] = false;
	return 0;
}

/* Ctrl+V into the focused window, whatever the keys held now: the program sees Control down, V down, V up */
static void synth_paste(void)
{
	if (!C.kb_focus || !xstate)
		return;
	struct wl_client *cl = wl_resource_get_client(C.kb_focus->res);
	xkb_mod_index_t ci = xkb_keymap_mod_get_index(xkb_state_get_keymap(xstate), XKB_MOD_NAME_CTRL);
	if (ci == XKB_MOD_INVALID)
		return;
	struct reslink *k;
	wl_list_for_each(k, &keyboards, link) {
		if (wl_resource_get_client(k->res) != cl)
			continue;
		uint32_t ctrl = 1u << ci;
		wl_keyboard_send_modifiers(k->res, wl_display_next_serial(C.dpy), ctrl, 0, 0,
			xkb_state_serialize_layout(xstate, XKB_STATE_LAYOUT_EFFECTIVE));
		uint32_t t = now_ms();
		wl_keyboard_send_key(k->res, wl_display_next_serial(C.dpy), t, paste_code, WL_KEYBOARD_KEY_STATE_PRESSED);
		wl_keyboard_send_key(k->res, wl_display_next_serial(C.dpy), t, paste_code, WL_KEYBOARD_KEY_STATE_RELEASED);
		send_modifiers_to(k->res);
	}
}

void seat_key(uint32_t ms, uint32_t key, bool pressed)
{
	if (key >= sizeof(consumed))
		return;
	xkb_keysym_t sym = xstate ? xkb_state_key_get_one_sym(xstate, key + 8) : XKB_KEY_NoSymbol;
	bool eat = false;
	if (pend.on && key != pend.key)
		pend_flush();	/* another key: it was a plain Ctrl+V after all */
	if (pend.on && key == pend.key && !pressed) {
		pend_flush();	/* released in time: press, then the release below */
	} else if (pressed && !hist_off && !C.locked && !pmenu_active() && C.kb_focus && !pend.on &&
		   (sym == XKB_KEY_v || sym == XKB_KEY_V) && mod_active(XKB_MOD_NAME_CTRL) &&
		   !mod_active(XKB_MOD_NAME_ALT) && !mod_active(XKB_MOD_NAME_SHIFT) && !mod_active(XKB_MOD_NAME_LOGO) &&
		   C.loop) {
		pend.on = true;
		pend.key = key;
		pend.ms = ms;
		if (!pend_timer)
			pend_timer = wl_event_loop_add_timer(C.loop, pend_cb, NULL);
		if (pend_timer) {
			wl_event_source_timer_update(pend_timer, HOLD_MS);
			xkb_state_update_key(xstate, key + 8, XKB_KEY_DOWN);
			return;
		}
		pend.on = false;
	}
	if (pressed) {
		eat = C.locked ? locked_binding(sym, true) : handle_binding(sym, true);
		consumed[key] = eat;
	} else {
		if (consumed[key]) {
			if (C.locked)
				locked_binding(sym, false);
			else
				handle_binding(sym, false);
			eat = true;
			consumed[key] = false;
		}
		if (cycling() && (sym == XKB_KEY_Alt_L || sym == XKB_KEY_Alt_R || sym == XKB_KEY_Meta_L ||
				  sym == XKB_KEY_Meta_R))
			cycle_end();
	}
	bool mods_changed = false;
	if (xstate) {
		enum xkb_state_component ch = xkb_state_update_key(xstate, key + 8,
			pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
		mods_changed = ch & (XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED |
				     XKB_STATE_MODS_LOCKED | XKB_STATE_LAYOUT_EFFECTIVE);
	}
	if (!C.kb_focus)
		return;
	if (!eat)
		forward_key(ms, key, pressed);
	if (mods_changed) {
		for_focus_keyboards(send_modifiers_to);
		update_leds();
	}
}

/* desktop.conf: key repeat for every client, Num Lock on or off when the setting changes */
void seat_apply_config(void)
{
	static int applied_numlock = -1;
	struct reslink *k;
	wl_list_for_each(k, &keyboards, link)
		if (wl_resource_get_version(k->res) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
			wl_keyboard_send_repeat_info(k->res, CF.repeat_rate, CF.repeat_delay);
	if (applied_numlock != CF.numlock) {
		applied_numlock = CF.numlock;
		numlock_wanted = CF.numlock;
		numlock_set(CF.numlock);
	}
	update_leds();
}

/* ---- wl_seat -------------------------------------------------------------------- */

static void ptr_set_cursor(struct wl_client *c, struct wl_resource *r, uint32_t serial,
			   struct wl_resource *surf, int32_t x, int32_t y)
{
	/* One built-in cursor for everything. */
}

static void ptr_release(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static const struct wl_pointer_interface pointer_impl = {
	.set_cursor = ptr_set_cursor, .release = ptr_release,
};

static void kb_release(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static const struct wl_keyboard_interface keyboard_impl = { .release = kb_release };

static void seat_get_pointer(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	struct wl_resource *res = wl_resource_create(c, &wl_pointer_interface,
						     wl_resource_get_version(r), id);
	struct reslink *l = res ? reslink_add(&pointers, res) : NULL;
	if (!l) {
		wl_client_post_no_memory(c);
		return;
	}
	wl_resource_set_implementation(res, &pointer_impl, l, reslink_destroy);
}

static void seat_get_keyboard(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	struct wl_resource *res = wl_resource_create(c, &wl_keyboard_interface,
						     wl_resource_get_version(r), id);
	struct reslink *l = res ? reslink_add(&keyboards, res) : NULL;
	if (!l) {
		wl_client_post_no_memory(c);
		return;
	}
	wl_resource_set_implementation(res, &keyboard_impl, l, reslink_destroy);
	send_keymap(res);
	if (wl_resource_get_version(res) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
		wl_keyboard_send_repeat_info(res, CF.repeat_rate, CF.repeat_delay);
	if (C.kb_focus && wl_resource_get_client(C.kb_focus->res) == c) {
		struct wl_array keys;
		wl_array_init(&keys);
		wl_keyboard_send_enter(res, wl_display_next_serial(C.dpy), C.kb_focus->res, &keys);
		wl_array_release(&keys);
		send_modifiers_to(res);
	}
}

static void seat_get_touch(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
	wl_resource_post_error(r, WL_SEAT_ERROR_MISSING_CAPABILITY, "no touch device");
}

static void seat_release(struct wl_client *c, struct wl_resource *r)
{
	wl_resource_destroy(r);
}

static const struct wl_seat_interface seat_impl = {
	.get_pointer = seat_get_pointer, .get_keyboard = seat_get_keyboard,
	.get_touch = seat_get_touch, .release = seat_release,
};

static void seat_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *r = wl_resource_create(c, &wl_seat_interface, version, id);
	if (!r) {
		wl_client_post_no_memory(c);
		return;
	}
	wl_resource_set_implementation(r, &seat_impl, NULL, NULL);
	wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
	if (version >= WL_SEAT_NAME_SINCE_VERSION)
		wl_seat_send_name(r, "seat0");
}

void seat_init(void)
{
	hist_off = getenv("FLCOMP_NO_CLIPHIST") != NULL;
	wl_list_init(&pointers);
	wl_list_init(&keyboards);
	wl_list_init(&data_devices);
	C.px = C.w / 2.0;
	C.py = C.h / 2.0;
	xctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	char layout[64] = "us";
	numlock_wanted = CF.numlock;
	read_config(layout, sizeof(layout));
	if (!xctx || (!build_keymap(layout, kb_options[0] ? kb_options : NULL) &&
		      !build_keymap("us", NULL)))
		fprintf(stderr, "flcomp: no keymap, keyboard input will be raw\n");
	wl_global_create(C.dpy, &wl_seat_interface, 5, NULL, seat_bind);
	wl_global_create(C.dpy, &wl_data_device_manager_interface, 3, NULL, ddm_bind);
}
