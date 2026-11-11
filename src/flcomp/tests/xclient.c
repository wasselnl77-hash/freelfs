/* xclient -- a tiny X11 (xcb) test client for flcomp's Xwayland support.
 * Draws one solid colour window and prints what happens to it, one line each.
 *   xclient [-t TITLE] [-n CLASS] [-w W] [-h H] [-x X -y Y] [-c RRGGBB] [-D] [-F] [-O]
 *           [-T] [-a MS:ACTION]... [-s SECONDS]
 * -D: ask for no decoration (_MOTIF_WM_HINTS); -F: fullscreen from the start; -O: an
 * override-redirect window at -x, -y (a menu); -T: a transient dialog 120 x 80 after the main window.
 * -a MS:ACTION runs ACTION MS milliseconds after the window is up:
 *   setclip=TEXT   take CLIPBOARD, serve TEXT as UTF8_STRING
 *   getclip        read CLIPBOARD and print "paste: ..."
 *   popup=X,Y      an override-redirect 80 x 60 window at the root coordinates X, Y (red)
 *   popdown        destroy it
 *   fullscreen=0|1 maximize=0|1    _NET_WM_STATE requests
 *   retitle=TEXT   change the title
 *   close          destroy the main window and exit
 * Prints: "mapped", "configure X Y W H", "button X Y ROOTX ROOTY", "key SYM", "focus in|out",
 * "enter", "delete" (WM_DELETE_WINDOW), "state ..." for _NET_WM_STATE, "paste: TEXT", "clipboard lost".
 * SPDX-License-Identifier: MIT */
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <xcb/xcb.h>

static xcb_connection_t *c;
static xcb_screen_t *scr;
static xcb_window_t win, dlg, pop;
static const char *title = "xclient", *klass = "XClient";
static int W = 240, H = 140, X = 100, Y = 100, have_pos;
static unsigned color = 0x3f9b63;
static int nodeco, fullscreen, override, transient;
static const char *clip_text;
static xcb_atom_t a_proto, a_delete, a_state, a_full, a_maxv, a_maxh, a_utf8, a_clip, a_targets, a_netname,
	a_motif, a_xsel, a_active;

struct act { int ms; char *what; int done; } acts[16];
static int nacts;

static xcb_atom_t atom(const char *n)
{
	xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, (uint16_t)strlen(n), n), NULL);
	xcb_atom_t a = r ? r->atom : 0;
	free(r);
	return a;
}

static uint32_t pixel(unsigned rgb) { return rgb & 0xffffff; }

static xcb_window_t make_window(int x, int y, int w, int h, unsigned col, int or_)
{
	xcb_window_t id = xcb_generate_id(c);
	uint32_t mask = XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, v[3];
	v[0] = pixel(col);
	v[1] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_BUTTON_PRESS |
	       XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_FOCUS_CHANGE | XCB_EVENT_MASK_ENTER_WINDOW |
	       XCB_EVENT_MASK_PROPERTY_CHANGE;
	if (or_) {
		mask = XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK;
		v[1] = 1;
		v[2] = XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_BUTTON_PRESS |
		       XCB_EVENT_MASK_ENTER_WINDOW;
	}
	xcb_create_window(c, XCB_COPY_FROM_PARENT, id, scr->root, (int16_t)x, (int16_t)y, (uint16_t)w, (uint16_t)h, 0,
			  XCB_WINDOW_CLASS_INPUT_OUTPUT, scr->root_visual, mask, v);
	return id;
}

static void set_str(xcb_window_t w, xcb_atom_t prop, xcb_atom_t type, const char *s)
{
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, prop, type, 8, (uint32_t)strlen(s), s);
}

static void set_title(xcb_window_t w, const char *t)
{
	set_str(w, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, t);
	set_str(w, a_netname, a_utf8, t);
}

static void props(xcb_window_t w, const char *t, int dialog)
{
	set_title(w, t);
	char cls[128];
	int n = snprintf(cls, sizeof cls, "xclient%c%s%c", 0, klass, 0);
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, (uint32_t)n, cls);
	xcb_atom_t protos[1] = { a_delete };
	xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, a_proto, XCB_ATOM_ATOM, 32, 1, protos);
	if (nodeco && !dialog) {
		uint32_t mh[5] = { 2, 0, 0, 0, 0 };	/* MWM_HINTS_DECORATIONS, none */
		xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, a_motif, a_motif, 32, 5, mh);
	}
	if (fullscreen && !dialog) {
		xcb_atom_t st[1] = { a_full };
		xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, a_state, XCB_ATOM_ATOM, 32, 1, st);
	}
	if (dialog)
		xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 32, 1, &win);
}

static void draw(xcb_window_t w, unsigned col, int ww, int hh)
{
	xcb_gcontext_t gc = xcb_generate_id(c);
	uint32_t v[1] = { pixel(col) };
	xcb_create_gc(c, gc, w, XCB_GC_FOREGROUND, v);
	xcb_rectangle_t r[2] = { { 0, 0, (uint16_t)ww, (uint16_t)hh }, { 10, 10, 30, 20 } };
	xcb_poly_fill_rectangle(c, w, gc, 1, r);
	v[0] = 0x1d2125;
	xcb_change_gc(c, gc, XCB_GC_FOREGROUND, v);
	xcb_poly_fill_rectangle(c, w, gc, 1, r + 1);
	xcb_free_gc(c, gc);
}

static void state_request(int add, xcb_atom_t a1, xcb_atom_t a2)
{
	xcb_client_message_event_t ev = { 0 };
	ev.response_type = XCB_CLIENT_MESSAGE;
	ev.format = 32;
	ev.window = win;
	ev.type = a_state;
	ev.data.data32[0] = (uint32_t)add;
	ev.data.data32[1] = a1;
	ev.data.data32[2] = a2;
	xcb_send_event(c, 0, scr->root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, (char *)&ev);
}

static void do_action(const char *a)
{
	if (!strncmp(a, "setclip=", 8)) {
		clip_text = strdup(a + 8);
		xcb_set_selection_owner(c, win, a_clip, XCB_CURRENT_TIME);
	} else if (!strcmp(a, "getclip")) {
		xcb_convert_selection(c, win, a_clip, a_utf8, a_xsel, XCB_CURRENT_TIME);
	} else if (!strncmp(a, "popup=", 6)) {
		int x = 0, y = 0;
		sscanf(a + 6, "%d,%d", &x, &y);
		pop = make_window(x, y, 80, 60, 0xb04a4a, 1);
		xcb_map_window(c, pop);
	} else if (!strcmp(a, "popdown")) {
		if (pop)
			xcb_destroy_window(c, pop);
		pop = 0;
	} else if (!strncmp(a, "fullscreen=", 11)) {
		state_request(a[11] == '1', a_full, 0);
	} else if (!strncmp(a, "maximize=", 9)) {
		state_request(a[9] == '1', a_maxv, a_maxh);
	} else if (!strncmp(a, "retitle=", 8)) {
		set_title(win, a + 8);
	} else if (!strcmp(a, "close")) {
		puts("closing");
		xcb_destroy_window(c, win);
		xcb_flush(c);
		exit(0);
	}
	xcb_flush(c);
}

static void selection_request(xcb_selection_request_event_t *e)
{
	xcb_selection_notify_event_t n = { 0 };
	n.response_type = XCB_SELECTION_NOTIFY;
	n.time = e->time;
	n.requestor = e->requestor;
	n.selection = e->selection;
	n.target = e->target;
	n.property = e->property ? e->property : e->target;
	if (!clip_text) {
		n.property = XCB_NONE;
	} else if (e->target == a_targets) {
		xcb_atom_t t[3] = { a_targets, a_utf8, XCB_ATOM_STRING };
		xcb_change_property(c, XCB_PROP_MODE_REPLACE, e->requestor, n.property, XCB_ATOM_ATOM, 32, 3, t);
	} else if (e->target == a_utf8 || e->target == XCB_ATOM_STRING) {
		xcb_change_property(c, XCB_PROP_MODE_REPLACE, e->requestor, n.property, e->target, 8,
				    (uint32_t)strlen(clip_text), clip_text);
	} else {
		n.property = XCB_NONE;
	}
	xcb_send_event(c, 0, e->requestor, XCB_EVENT_MASK_NO_EVENT, (char *)&n);
	xcb_flush(c);
}

static int64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
	int secs = 30, opt;
	setvbuf(stdout, NULL, _IOLBF, 0);
	extern char *optarg;
	while ((opt = getopt(argc, argv, "t:n:w:h:x:y:c:DFOTa:s:")) != -1) {
		switch (opt) {
		case 't': title = optarg; break;
		case 'n': klass = optarg; break;
		case 'w': W = atoi(optarg); break;
		case 'h': H = atoi(optarg); break;
		case 'x': X = atoi(optarg); have_pos = 1; break;
		case 'y': Y = atoi(optarg); have_pos = 1; break;
		case 'c': color = (unsigned)strtoul(optarg, NULL, 16); break;
		case 'D': nodeco = 1; break;
		case 'F': fullscreen = 1; break;
		case 'O': override = 1; break;
		case 'T': transient = 1; break;
		case 's': secs = atoi(optarg); break;
		case 'a':
			if (nacts < 16 && strchr(optarg, ':')) {
				acts[nacts].ms = atoi(optarg);
				acts[nacts++].what = strdup(strchr(optarg, ':') + 1);
			}
			break;
		default:
			return 2;
		}
	}
	c = xcb_connect(NULL, NULL);
	if (xcb_connection_has_error(c)) {
		fprintf(stderr, "xclient: cannot connect to the X server\n");
		return 1;
	}
	scr = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
	a_proto = atom("WM_PROTOCOLS");
	a_delete = atom("WM_DELETE_WINDOW");
	a_state = atom("_NET_WM_STATE");
	a_full = atom("_NET_WM_STATE_FULLSCREEN");
	a_maxv = atom("_NET_WM_STATE_MAXIMIZED_VERT");
	a_maxh = atom("_NET_WM_STATE_MAXIMIZED_HORZ");
	a_utf8 = atom("UTF8_STRING");
	a_clip = atom("CLIPBOARD");
	a_targets = atom("TARGETS");
	a_netname = atom("_NET_WM_NAME");
	a_motif = atom("_MOTIF_WM_HINTS");
	a_xsel = atom("XCLIENT_SEL");
	a_active = atom("_NET_ACTIVE_WINDOW");
	win = make_window(X, Y, W, H, color, override);
	if (!override)
		props(win, title, 0);
	xcb_map_window(c, win);
	xcb_flush(c);
	printf("root %dx%d\n", scr->width_in_pixels, scr->height_in_pixels);
	int64_t start = now_ms(), shown = 0;
	int dlg_made = 0;
	for (;;) {
		int64_t t = now_ms();
		if (t - start > secs * 1000)
			break;
		if (shown)
			for (int i = 0; i < nacts; i++)
				if (!acts[i].done && t - shown >= acts[i].ms) {
					acts[i].done = 1;
					do_action(acts[i].what);
				}
		if (transient && shown && !dlg_made) {
			dlg_made = 1;
			dlg = make_window(0, 0, 120, 80, 0x3a4048, 0);
			props(dlg, "xclient dialog", 1);
			xcb_map_window(c, dlg);
			xcb_flush(c);
		}
		xcb_generic_event_t *ev = xcb_poll_for_event(c);
		if (!ev) {
			if (xcb_connection_has_error(c))
				break;
			struct pollfd p = { xcb_get_file_descriptor(c), POLLIN, 0 };
			poll(&p, 1, 40);
			continue;
		}
		switch (ev->response_type & 0x7f) {
		case XCB_EXPOSE: {
			xcb_expose_event_t *e = (void *)ev;
			if (e->window == win)
				draw(win, color, e->x + e->width + 400, e->y + e->height + 400);
			else if (e->window == dlg)
				draw(dlg, 0x3a4048, 200, 200);
			else
				draw(e->window, 0xb04a4a, 200, 200);
			break;
		}
		case XCB_MAP_NOTIFY: {
			xcb_map_notify_event_t *e = (void *)ev;
			if (e->window == win && !shown) {
				shown = now_ms();
				puts("mapped");
			} else if (e->window == dlg) {
				puts("dialog mapped");
			}
			break;
		}
		case XCB_CONFIGURE_NOTIFY: {
			xcb_configure_notify_event_t *e = (void *)ev;
			if (e->window == win)
				printf("configure %d %d %d %d\n", e->x, e->y, e->width, e->height);
			else if (e->window == dlg)
				printf("dialog configure %d %d %d %d\n", e->x, e->y, e->width, e->height);
			else if (e->window == pop)
				printf("popup configure %d %d %d %d\n", e->x, e->y, e->width, e->height);
			break;
		}
		case XCB_BUTTON_PRESS: {
			xcb_button_press_event_t *e = (void *)ev;
			printf("%sbutton %d %d %d %d\n", e->event == pop ? "popup " : e->event == dlg ? "dialog " : "",
			       e->event_x, e->event_y, e->root_x, e->root_y);
			break;
		}
		case XCB_KEY_PRESS: {
			xcb_key_press_event_t *e = (void *)ev;
			printf("key %d\n", e->detail);
			break;
		}
		case XCB_FOCUS_IN:
			printf("%sfocus in\n", ((xcb_focus_in_event_t *)ev)->event == dlg ? "dialog " : "");
			break;
		case XCB_FOCUS_OUT:
			printf("%sfocus out\n", ((xcb_focus_out_event_t *)ev)->event == dlg ? "dialog " : "");
			break;
		case XCB_ENTER_NOTIFY:
			printf("enter %s\n", ((xcb_enter_notify_event_t *)ev)->event == pop ? "popup" : "window");
			break;
		case XCB_PROPERTY_NOTIFY: {
			xcb_property_notify_event_t *e = (void *)ev;
			if (e->atom == a_state && e->window == win) {
				xcb_get_property_reply_t *r = xcb_get_property_reply(c,
					xcb_get_property(c, 0, win, a_state, XCB_ATOM_ATOM, 0, 32), NULL);
				printf("state");
				if (r) {
					xcb_atom_t *a = xcb_get_property_value(r);
					for (int i = 0; i < xcb_get_property_value_length(r) / 4; i++)
						printf(" %s", a[i] == a_full ? "fullscreen" : a[i] == a_maxv ? "maxv" :
							      a[i] == a_maxh ? "maxh" : "other");
					free(r);
				}
				putchar('\n');
			}
			break;
		}
		case XCB_CLIENT_MESSAGE: {
			xcb_client_message_event_t *e = (void *)ev;
			if (e->type == a_proto && e->data.data32[0] == a_delete) {
				puts("delete");
				xcb_destroy_window(c, win);
				xcb_flush(c);
				return 0;
			}
			break;
		}
		case XCB_SELECTION_REQUEST:
			selection_request((void *)ev);
			break;
		case XCB_SELECTION_CLEAR:
			puts("clipboard lost");
			clip_text = NULL;
			break;
		case XCB_SELECTION_NOTIFY: {
			xcb_selection_notify_event_t *e = (void *)ev;
			if (e->property == XCB_NONE) {
				puts("paste: (none)");
				break;
			}
			xcb_get_property_reply_t *r = xcb_get_property_reply(c,
				xcb_get_property(c, 1, win, e->property, XCB_ATOM_ANY, 0, 0x1fffff), NULL);
			if (r) {
				printf("paste: %.*s\n", xcb_get_property_value_length(r), (char *)xcb_get_property_value(r));
				free(r);
			}
			break;
		}
		case XCB_DESTROY_NOTIFY:
			break;
		default:
			break;
		}
		free(ev);
		xcb_flush(c);
	}
	xcb_disconnect(c);
	return 0;
}
