/* xwsel.c -- the clipboard between X11 and Wayland.
 *
 * CLIPBOARD only: an X client that takes the selection shows up as a (foreign) data source in
 * seat.c, so every Wayland client can paste it; a Wayland client's selection makes flcomp's
 * window the X owner, and X clients paste it through the usual SelectionRequest dance. Text
 * (UTF8_STRING, STRING, TEXT <-> text/plain) and every target whose name is a mime type
 * (text/uri-list, image/png, ...) go across; big transfers use INCR in both directions.
 * PRIMARY (select to copy, middle click to paste) stays among the X clients: flcomp has no
 * Wayland primary selection protocol to connect it to.
 * SPDX-License-Identifier: MIT */
#include "xwm-int.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_TARGETS 48
#define CHUNK (64 * 1024)		/* INCR piece size (well under the X request size limit) */
#define MAX_BYTES (64u << 20)		/* a bigger selection is cut off */

/* ---- X owner -> Wayland ---------------------------------------------------------------------- */

struct xsrc {			/* what the X owner offers */
	int n;
	char *mime[MAX_TARGETS];
	xcb_atom_t atom[MAX_TARGETS];
};

struct xfer {			/* a Wayland client reading an X selection */
	struct xfer *next;
	int fd;
	xcb_atom_t prop;
	bool incr, done;
	char *buf;
	size_t len, off;
	struct wl_event_source *src;
};

static struct xsrc *cur_src;
static struct xfer *xfers;
static int xfer_seq;

/* ---- Wayland owner -> X ----------------------------------------------------------------------- */

struct wreq {			/* an X client reading the Wayland selection */
	struct wreq *next;
	xcb_window_t requestor;
	xcb_atom_t selection, target, property, type;
	xcb_timestamp_t time;
	int fd;			/* read end of the pipe the Wayland client writes into */
	struct wl_event_source *src;
	char *buf;
	size_t len, cap, off;
	bool incr, incr_started, eof;
};

static struct wreq *wreqs;
static bool x_owns;		/* flcomp's window is the X owner of CLIPBOARD */
static uint32_t own_time;

static void src_free(struct xsrc *s)
{
	if (!s)
		return;
	for (int i = 0; i < s->n; i++)
		free(s->mime[i]);
	free(s);
}

/* ---- the async writer into a Wayland client's pipe ------------------------------------------------- */

static void xfer_finish(struct xfer *t)
{
	for (struct xfer **pp = &xfers; *pp; pp = &(*pp)->next)
		if (*pp == t) {
			*pp = t->next;
			break;
		}
	if (t->src)
		wl_event_source_remove(t->src);
	if (t->fd >= 0)
		close(t->fd);
	free(t->buf);
	free(t);
}

static int xfer_write_cb(int fd, uint32_t mask, void *data);

static void xfer_flush(struct xfer *t)
{
	while (t->off < t->len) {
		ssize_t n = write(t->fd, t->buf + t->off, t->len - t->off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN) {
				if (!t->src)
					t->src = wl_event_loop_add_fd(C.loop, t->fd, WL_EVENT_WRITABLE, xfer_write_cb, t);
				return;
			}
			t->done = true;		/* the reader went away */
			t->off = t->len;
			break;
		}
		t->off += (size_t)n;
	}
	if (t->src) {
		wl_event_source_remove(t->src);
		t->src = NULL;
	}
	t->len = t->off = 0;
	if (t->done)
		xfer_finish(t);
}

static int xfer_write_cb(int fd, uint32_t mask, void *data)
{
	struct xfer *t = data;
	if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
		t->done = true;
		t->len = t->off = 0;
	}
	if (t->src) {
		wl_event_source_remove(t->src);
		t->src = NULL;
	}
	xfer_flush(t);
	return 0;
}

static void xfer_append(struct xfer *t, const void *p, size_t n)
{
	if (t->off > 0 && t->off == t->len)
		t->len = t->off = 0;
	char *nb = realloc(t->buf, t->len + n);
	if (!nb)
		return;
	t->buf = nb;
	memcpy(t->buf + t->len, p, n);
	t->len += n;
	xfer_flush(t);
}

/* ---- X owner: what it offers ------------------------------------------------------------------ */

static bool have_mime(struct xsrc *s, const char *m)
{
	for (int i = 0; i < s->n; i++)
		if (!strcmp(s->mime[i], m))
			return true;
	return false;
}

static void src_add(struct xsrc *s, const char *mime, xcb_atom_t atom)
{
	if (s->n >= MAX_TARGETS || have_mime(s, mime))
		return;
	s->mime[s->n] = strdup(mime);
	if (!s->mime[s->n])
		return;
	s->atom[s->n++] = atom;
}

static void src_send(void *data, const char *mime, int fd)
{
	struct xsrc *s = data;
	xcb_atom_t atom = XCB_NONE;
	for (int i = 0; i < s->n; i++)
		if (!strcmp(s->mime[i], mime))
			atom = s->atom[i];
	if (!atom || !X.c) {
		close(fd);
		return;
	}
	struct xfer *t = calloc(1, sizeof *t);
	if (!t) {
		close(fd);
		return;
	}
	char name[32];
	snprintf(name, sizeof name, "_FLCOMP_XFER%d", xfer_seq++ & 7);
	t->fd = fd;
	t->prop = xwm_intern(name);
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	/* an earlier transfer on the same slot is dropped */
	for (struct xfer *o = xfers; o; o = o->next)
		if (o->prop == t->prop) {
			o->done = true;
			xfer_finish(o);
			break;
		}
	t->next = xfers;
	xfers = t;
	xcb_convert_selection(X.c, X.win, X.a.CLIPBOARD, atom, t->prop, XCB_CURRENT_TIME);
	xwm_flush();
}

static void src_cancelled(void *data)
{
	if (data == cur_src)
		cur_src = NULL;
	src_free(data);
}

static const struct fsel_ops src_ops = { .send = src_send, .cancelled = src_cancelled };

static void targets_arrived(void)
{
	xcb_get_property_reply_t *r = xcb_get_property_reply(X.c,
		xcb_get_property(X.c, 1, X.win, X.a._FLCOMP_SEL, XCB_ATOM_ANY, 0, 1024), NULL);
	if (!r)
		return;
	if (r->format != 32 || r->type == XCB_NONE) {
		free(r);
		return;
	}
	struct xsrc *s = calloc(1, sizeof *s);
	if (!s) {
		free(r);
		return;
	}
	const xcb_atom_t *atoms = xcb_get_property_value(r);
	int n = xcb_get_property_value_length(r) / 4;
	xcb_atom_t utf8 = X.a.UTF8_STRING;
	/* text first: the best match for a text/plain request */
	for (int i = 0; i < n; i++)
		if (atoms[i] == utf8) {
			src_add(s, "text/plain;charset=utf-8", utf8);
			src_add(s, "text/plain", utf8);
		}
	for (int i = 0; i < n; i++) {
		if (atoms[i] == XCB_ATOM_STRING || atoms[i] == X.a.TEXT)
			src_add(s, "text/plain", atoms[i]);
	}
	for (int i = 0; i < n; i++) {
		if (atoms[i] == utf8 || atoms[i] == XCB_ATOM_STRING || atoms[i] == X.a.TEXT ||
		    atoms[i] == X.a.TARGETS || atoms[i] == X.a.TIMESTAMP)
			continue;
		char *name = xwm_atom_name(atoms[i]);
		if (name && strchr(name, '/'))
			src_add(s, name, atoms[i]);
		free(name);
	}
	free(r);
	if (s->n == 0) {
		src_free(s);
		return;
	}
	struct xsrc *old = cur_src;
	cur_src = s;
	/* the seat cancels the previous source (our callback frees it) */
	(void)old;
	seat_set_foreign_selection((const char *const *)s->mime, s->n, &src_ops, s);
}

static void data_arrived(struct xfer *t)
{
	/* the property is read in pieces of any size: delete when read, so an INCR sender sees it */
	xcb_get_property_reply_t *r = xcb_get_property_reply(X.c,
		xcb_get_property(X.c, 1, X.win, t->prop, XCB_ATOM_ANY, 0, 0x1fffffff), NULL);
	if (!r) {
		t->done = true;
		xfer_flush(t);
		return;
	}
	if (r->type == X.a.INCR) {
		t->incr = true;		/* chunks follow as PropertyNotify NEW_VALUE */
		free(r);
		xwm_flush();
		return;
	}
	int n = xcb_get_property_value_length(r);
	if (n > 0)
		xfer_append(t, xcb_get_property_value(r), (size_t)n);
	free(r);
	if (!t->incr) {
		t->done = true;
		xfer_flush(t);
	}
	xwm_flush();
}

static void incr_chunk(struct xfer *t)
{
	xcb_get_property_reply_t *r = xcb_get_property_reply(X.c,
		xcb_get_property(X.c, 1, X.win, t->prop, XCB_ATOM_ANY, 0, 0x1fffffff), NULL);
	if (!r) {
		t->done = true;
		xfer_flush(t);
		return;
	}
	int n = xcb_get_property_value_length(r);
	if (n > 0) {
		xfer_append(t, xcb_get_property_value(r), (size_t)n);
	} else {
		t->done = true;		/* the empty piece ends it */
		xfer_flush(t);
	}
	free(r);
	xwm_flush();
}

/* ---- Wayland owner: serving an X client ---------------------------------------------------------- */

static void wreq_free(struct wreq *q)
{
	for (struct wreq **pp = &wreqs; *pp; pp = &(*pp)->next)
		if (*pp == q) {
			*pp = q->next;
			break;
		}
	if (q->src)
		wl_event_source_remove(q->src);
	if (q->fd >= 0)
		close(q->fd);
	free(q->buf);
	free(q);
}

static void notify_requestor(struct wreq *q, bool ok)
{
	xcb_selection_notify_event_t ev = { 0 };
	ev.response_type = XCB_SELECTION_NOTIFY;
	ev.time = q->time;
	ev.requestor = q->requestor;
	ev.selection = q->selection;
	ev.target = q->target;
	ev.property = ok ? q->property : XCB_NONE;
	xcb_send_event(X.c, 0, q->requestor, XCB_EVENT_MASK_NO_EVENT, (const char *)&ev);
}

static void wreq_deliver(struct wreq *q)
{
	if (q->len <= CHUNK) {
		xcb_change_property(X.c, XCB_PROP_MODE_REPLACE, q->requestor, q->property, q->type, 8,
				    (uint32_t)q->len, q->buf ? q->buf : "");
		notify_requestor(q, true);
		xwm_flush();
		wreq_free(q);
		return;
	}
	/* INCR: announce the size, then one piece each time the requestor deletes the property */
	uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
	xcb_change_window_attributes(X.c, q->requestor, XCB_CW_EVENT_MASK, &mask);
	uint32_t size = (uint32_t)q->len;
	q->incr = true;
	xcb_change_property(X.c, XCB_PROP_MODE_REPLACE, q->requestor, q->property, X.a.INCR, 32, 1, &size);
	notify_requestor(q, true);
	xwm_flush();
}

static void wreq_piece(struct wreq *q)
{
	size_t n = q->len - q->off;
	if (n > CHUNK)
		n = CHUNK;
	xcb_change_property(X.c, XCB_PROP_MODE_REPLACE, q->requestor, q->property, q->type, 8, (uint32_t)n,
			    q->buf + q->off);
	q->off += n;
	if (n == 0) {		/* the empty piece was sent: done */
		uint32_t none = 0;
		xcb_change_window_attributes(X.c, q->requestor, XCB_CW_EVENT_MASK, &none);
		xwm_flush();
		wreq_free(q);
		return;
	}
	xwm_flush();
}

static int wreq_read_cb(int fd, uint32_t mask, void *data)
{
	struct wreq *q = data;
	for (;;) {
		if (q->len == q->cap) {
			size_t nc = q->cap ? q->cap * 2 : 4096;
			if (nc > MAX_BYTES + 4096)
				goto eof;
			char *nb = realloc(q->buf, nc);
			if (!nb)
				goto eof;
			q->buf = nb;
			q->cap = nc;
		}
		ssize_t n = read(fd, q->buf + q->len, q->cap - q->len);
		if (n > 0) {
			q->len += (size_t)n;
			if (q->len >= MAX_BYTES)
				goto eof;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			return 0;
		goto eof;
	}
eof:
	wl_event_source_remove(q->src);
	q->src = NULL;
	close(q->fd);
	q->fd = -1;
	q->eof = true;
	wreq_deliver(q);
	return 0;
}

/* the mime type that answers an X target, among the ones the Wayland client offers */
static const char *mime_for_target(xcb_atom_t target, const char **mimes, int n, char **tofree)
{
	static const char *const text[] = { "text/plain;charset=utf-8", "text/plain;charset=UTF-8", "text/plain", "UTF8_STRING", "STRING" };
	*tofree = NULL;
	if (target == X.a.UTF8_STRING || target == XCB_ATOM_STRING || target == X.a.TEXT) {
		for (unsigned k = 0; k < sizeof text / sizeof text[0]; k++)
			for (int i = 0; i < n; i++)
				if (!strcmp(mimes[i], text[k]))
					return mimes[i];
		return NULL;
	}
	char *name = xwm_atom_name(target);
	if (!name)
		return NULL;
	for (int i = 0; i < n; i++)
		if (!strcmp(mimes[i], name)) {
			*tofree = name;
			return mimes[i];
		}
	free(name);
	return NULL;
}

static void handle_request(xcb_selection_request_event_t *e)
{
	struct wreq tmp = { .requestor = e->requestor, .selection = e->selection, .target = e->target,
			    .property = e->property ? e->property : e->target, .time = e->time };
	if (e->selection != X.a.CLIPBOARD) {
		notify_requestor(&tmp, false);
		xwm_flush();
		return;
	}
	const char *mimes[MAX_TARGETS];
	int n = seat_wayland_selection_mimes(mimes, MAX_TARGETS);
	if (n == 0) {
		notify_requestor(&tmp, false);
		xwm_flush();
		return;
	}
	if (e->target == X.a.TARGETS) {
		xcb_atom_t out[MAX_TARGETS + 8];
		int k = 0;
		out[k++] = X.a.TARGETS;
		out[k++] = X.a.TIMESTAMP;
		bool text = false;
		for (int i = 0; i < n; i++)
			if (!strncmp(mimes[i], "text/plain", 10) || !strcmp(mimes[i], "UTF8_STRING") || !strcmp(mimes[i], "STRING"))
				text = true;
		if (text) {
			out[k++] = X.a.UTF8_STRING;
			out[k++] = XCB_ATOM_STRING;
			out[k++] = X.a.TEXT;
		}
		for (int i = 0; i < n && k < MAX_TARGETS + 8; i++)
			if (strchr(mimes[i], '/') && strncmp(mimes[i], "text/plain", 10)) {
				xcb_atom_t a = xwm_intern(mimes[i]);
				if (a)
					out[k++] = a;
			}
		xcb_change_property(X.c, XCB_PROP_MODE_REPLACE, e->requestor, tmp.property, XCB_ATOM_ATOM, 32, (uint32_t)k, out);
		notify_requestor(&tmp, true);
		xwm_flush();
		return;
	}
	if (e->target == X.a.TIMESTAMP) {
		uint32_t t = own_time;
		xcb_change_property(X.c, XCB_PROP_MODE_REPLACE, e->requestor, tmp.property, XCB_ATOM_INTEGER, 32, 1, &t);
		notify_requestor(&tmp, true);
		xwm_flush();
		return;
	}
	char *tofree;
	const char *mime = mime_for_target(e->target, mimes, n, &tofree);
	int p[2];
	if (!mime || pipe2(p, O_CLOEXEC | O_NONBLOCK) < 0) {
		free(tofree);
		notify_requestor(&tmp, false);
		xwm_flush();
		return;
	}
	struct wreq *q = calloc(1, sizeof *q);
	if (!q) {
		free(tofree);
		close(p[0]);
		close(p[1]);
		notify_requestor(&tmp, false);
		xwm_flush();
		return;
	}
	*q = tmp;
	q->type = e->target;
	q->fd = p[0];
	/* the write end goes to the Wayland client; its copy is closed when it is done */
	fcntl(p[1], F_SETFL, fcntl(p[1], F_GETFL) & ~O_NONBLOCK);
	seat_wayland_selection_send(mime, p[1]);
	close(p[1]);
	free(tofree);
	q->next = wreqs;
	wreqs = q;
	q->src = wl_event_loop_add_fd(C.loop, q->fd, WL_EVENT_READABLE, wreq_read_cb, q);
}

/* ---- events and entry points ---------------------------------------------------------------------- */

bool xwsel_event(void *evp)
{
	xcb_generic_event_t *ev = evp;
	int type = ev->response_type & 0x7f;
	if (X.xfixes && type == X.xfixes->first_event + XCB_XFIXES_SELECTION_NOTIFY) {
		xcb_xfixes_selection_notify_event_t *e = evp;
		if (e->selection != X.a.CLIPBOARD)
			return true;
		if (e->owner == X.win)
			return true;			/* our own claim */
		x_owns = false;
		if (e->owner == XCB_NONE) {
			if (cur_src)
				seat_clear_foreign_selection(cur_src);
			return true;
		}
		xcb_convert_selection(X.c, X.win, X.a.CLIPBOARD, X.a.TARGETS, X.a._FLCOMP_SEL, XCB_CURRENT_TIME);
		xwm_flush();
		return true;
	}
	switch (type) {
	case XCB_SELECTION_NOTIFY: {
		xcb_selection_notify_event_t *e = evp;
		if (e->requestor != X.win)
			return false;
		if (e->target == X.a.TARGETS && e->selection == X.a.CLIPBOARD) {
			if (e->property != XCB_NONE)
				targets_arrived();
			return true;
		}
		for (struct xfer *t = xfers; t; t = t->next)
			if (t->prop == e->property) {
				data_arrived(t);
				return true;
			}
		/* a refusal (property None): the transfer that waits for this target ends */
		if (e->property == XCB_NONE) {
			for (struct xfer *t = xfers; t; t = t->next)
				if (!t->incr && !t->len) {
					t->done = true;
					xfer_finish(t);
					break;
				}
		}
		return true;
	}
	case XCB_SELECTION_REQUEST:
		handle_request(evp);
		return true;
	case XCB_SELECTION_CLEAR: {
		xcb_selection_clear_event_t *e = evp;
		if (e->selection == X.a.CLIPBOARD && e->owner == X.win)
			x_owns = false;
		return true;
	}
	case XCB_PROPERTY_NOTIFY: {
		xcb_property_notify_event_t *e = evp;
		if (e->window == X.win && e->state == XCB_PROPERTY_NEW_VALUE) {
			for (struct xfer *t = xfers; t; t = t->next)
				if (t->prop == e->atom && t->incr) {
					incr_chunk(t);
					return true;
				}
		} else if (e->state == XCB_PROPERTY_DELETE) {
			for (struct wreq *q = wreqs; q; q = q->next)
				if (q->incr && q->requestor == e->window && q->property == e->atom && q->eof) {
					wreq_piece(q);
					return true;
				}
		}
		return false;
	}
	default:
		return false;
	}
}

/* seat.c: a Wayland client owns the selection now (or nobody does) */
void xwsel_wayland_changed(void)
{
	if (!X.c)
		return;
	const char *mimes[MAX_TARGETS];
	int n = seat_wayland_selection_mimes(mimes, MAX_TARGETS);
	if (n > 0) {
		xcb_set_selection_owner(X.c, X.win, X.a.CLIPBOARD, XCB_CURRENT_TIME);
		x_owns = true;
		own_time = (uint32_t)(now_ms() & 0x7fffffff);
	} else if (x_owns) {
		xcb_set_selection_owner(X.c, XCB_NONE, X.a.CLIPBOARD, XCB_CURRENT_TIME);
		x_owns = false;
	}
	xwm_flush();
}

void xwsel_init(void)
{
	cur_src = NULL;
	x_owns = false;
	xcb_xfixes_select_selection_input(X.c, X.win, X.a.CLIPBOARD,
		XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
		XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
		XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE);
	xwm_flush();
	xwsel_wayland_changed();	/* something copied before the first X program started */
}

void xwsel_fini(void)
{
	while (xfers) {
		xfers->done = true;
		xfer_finish(xfers);
	}
	while (wreqs)
		wreq_free(wreqs);
	if (cur_src)
		seat_clear_foreign_selection(cur_src);
	cur_src = NULL;
	x_owns = false;
}
