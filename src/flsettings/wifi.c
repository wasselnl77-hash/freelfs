/* wifi.c -- the Wi-Fi page: iwd's D-Bus API (net.connman.iwd) through libdbus
 * SPDX-License-Identifier: MIT
 *
 * One private connection to the system bus, polled by flui's event loop (the
 * bus socket is an fd there, so an idle page costs nothing). The page keeps a
 * snapshot of iwd's objects: GetManagedObjects, then GetOrderedNetworks for the
 * signal strengths. Every signal from iwd just schedules a new snapshot.
 *
 * Passwords: iwd asks for them through net.connman.iwd.Agent, which this
 * program exports at AGENT_PATH. RequestPassphrase is answered when the user
 * has typed the password; the typed bytes live in one buffer that is wiped
 * (explicit_bzero) when it is sent, cancelled, or the prompt goes away. They
 * are never logged or written anywhere. libdbus itself copies the string into
 * the outgoing message; it does not wipe its own buffers.
 */
#include <dbus/dbus.h>
#include <errno.h>
#include <dirent.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "wifi.h"

#define IWD "net.connman.iwd"
#define AGENT_PATH "/org/freelfs/flsettings/agent"
#define MAXDEV 4
#define MAXNET 96
#define ROWH 48
#define PWMAX 63
#define PWMIN 8

enum { WS_LOADING, WS_NOBUS, WS_NOIWD, WS_DENIED, WS_OK };
enum {
	ID_SW = 400, ID_SCAN, ID_PW, ID_SHOW, ID_CANCEL, ID_OK, ID_SB, ID_ROW = 500
};

struct wdev {
	char path[96], name[24], state[16], conn[128];
	int powered, station, scanning;
};

struct wnet {
	char path[128], dev[96], name[72], type[12], known[128];
	int connected, signal; /* signal: dBm * 100, INT_MIN when iwd gave none */
};

struct wifi {
	fl_app *app;
	fl_win *win;
	fl_font *head, *small;
	DBusConnection *bus;
	int fd, lost;
	fl_timer *t_refresh, *t_pump, *t_poll;
	int state;
	struct wdev dev[MAXDEV];
	int ndev;
	struct wnet net[MAXNET];
	int nnet;
	int refreshing, again, visible, agent_ok, agent_busy, scanned;
	int wired; /* 0 no adapter, 1 no link, 2 link */
	char wired_name[24];

	char connecting[72]; /* name, while a Connect call is out */
	int canceled;
	DBusMessage *agent_req;
	char prompt_name[72];
	char pw[PWMAX + 1];
	int pwlen, show, focus_pw;

	int first; /* index of the first network row shown */
	char msg[200];
	int msg_err;
};

static void refresh(struct wifi *w);
static void register_agent(struct wifi *w);

static void redraw(struct wifi *w)
{
	if (w->win)
		fl_win_redraw(w->win);
}

static void set_msg(struct wifi *w, int err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(w->msg, sizeof w->msg, fmt, ap);
	va_end(ap);
	w->msg_err = err;
	redraw(w);
}

/* ---- the bus -------------------------------------------------------------------------------- */

static void bus_lost(struct wifi *w)
{
	if (w->bus) {
		fl_fd_remove(w->app, w->fd);
		dbus_connection_close(w->bus);
		dbus_connection_unref(w->bus);
		w->bus = NULL;
	}
	w->state = WS_NOBUS;
	w->ndev = w->nnet = 0;
	redraw(w);
}

static void pump(struct wifi *w)
{
	if (!w->bus)
		return;
	int alive = dbus_connection_read_write(w->bus, 0);
	while (w->bus && dbus_connection_dispatch(w->bus) == DBUS_DISPATCH_DATA_REMAINS)
		;
	if (!alive || w->lost)
		bus_lost(w);
}

static void on_fd(void *ud, int fd, short rev)
{
	pump(ud);
}

static void on_pump(void *ud)
{
	pump(ud);
}

/* send and flush; the answer arrives through the fd. Returns the pending call or NULL. */
static DBusPendingCall *call(struct wifi *w, DBusMessage *m, void (*cb)(DBusPendingCall *, void *))
{
	DBusPendingCall *pc = NULL;
	if (!w->bus || !m)
		return NULL;
	if (!dbus_connection_send_with_reply(w->bus, m, &pc, 120 * 1000) || !pc) {
		dbus_message_unref(m);
		return NULL;
	}
	dbus_message_unref(m);
	if (cb)
		dbus_pending_call_set_notify(pc, cb, w, NULL);
	dbus_connection_flush(w->bus);
	fl_timer_arm(w->t_pump, 2, 0);
	return pc;
}

static DBusMessage *method(const char *path, const char *iface, const char *name)
{
	return dbus_message_new_method_call(IWD, path, iface, name);
}

static DBusMessage *reply_of(DBusPendingCall *pc, const char **err)
{
	DBusMessage *r = dbus_pending_call_steal_reply(pc);
	dbus_pending_call_unref(pc);
	*err = NULL;
	if (r && dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR)
		*err = dbus_message_get_error_name(r);
	return r;
}

static int is_err(const char *err, const char *name)
{
	return err && !strcmp(err, name);
}

/* Reply to a call whose answer only matters when it failed. */
static void on_generic(DBusPendingCall *pc, void *ud)
{
	struct wifi *w = ud;
	const char *err;
	DBusMessage *r = reply_of(pc, &err);
	if (err) {
		if (is_err(err, "org.freedesktop.DBus.Error.AccessDenied"))
			set_msg(w, 1, "You are not allowed to change Wi-Fi settings.");
		else
			set_msg(w, 1, "That did not work (%s).", strrchr(err, '.') ? strrchr(err, '.') + 1 : err);
	}
	if (r)
		dbus_message_unref(r);
	fl_timer_arm(w->t_refresh, 150, 0);
}

static void simple_call(struct wifi *w, const char *path, const char *iface, const char *name)
{
	call(w, method(path, iface, name), on_generic);
}

/* ---- the snapshot --------------------------------------------------------------------------- */

static void copy_str(char *out, size_t cap, DBusMessageIter *v)
{
	int t = dbus_message_iter_get_arg_type(v);
	if (t == DBUS_TYPE_STRING || t == DBUS_TYPE_OBJECT_PATH) {
		const char *s;
		dbus_message_iter_get_basic(v, &s);
		snprintf(out, cap, "%s", s);
	}
}

static int get_bool(DBusMessageIter *v)
{
	dbus_bool_t b = 0;
	if (dbus_message_iter_get_arg_type(v) == DBUS_TYPE_BOOLEAN)
		dbus_message_iter_get_basic(v, &b);
	return b ? 1 : 0;
}

static struct wdev *dev_for(struct wifi *w, const char *path)
{
	for (int i = 0; i < w->ndev; i++)
		if (!strcmp(w->dev[i].path, path))
			return &w->dev[i];
	if (w->ndev >= MAXDEV)
		return NULL;
	struct wdev *d = &w->dev[w->ndev++];
	memset(d, 0, sizeof *d);
	snprintf(d->path, sizeof d->path, "%s", path);
	return d;
}

static void parse_objects(struct wifi *w, DBusMessageIter *top)
{
	DBusMessageIter arr, ent;
	w->ndev = w->nnet = 0;
	if (dbus_message_iter_get_arg_type(top) != DBUS_TYPE_ARRAY)
		return;
	dbus_message_iter_recurse(top, &arr);
	for (; dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&arr)) {
		DBusMessageIter ifs;
		const char *path = "";
		struct wdev *d = NULL;
		struct wnet *n = NULL;
		dbus_message_iter_recurse(&arr, &ent);
		dbus_message_iter_get_basic(&ent, &path);
		dbus_message_iter_next(&ent);
		dbus_message_iter_recurse(&ent, &ifs);
		for (; dbus_message_iter_get_arg_type(&ifs) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&ifs)) {
			DBusMessageIter ife, props;
			const char *iface = "";
			dbus_message_iter_recurse(&ifs, &ife);
			dbus_message_iter_get_basic(&ife, &iface);
			dbus_message_iter_next(&ife);
			dbus_message_iter_recurse(&ife, &props);
			int kind = !strcmp(iface, IWD ".Device") ? 1 : !strcmp(iface, IWD ".Station") ? 2
			         : !strcmp(iface, IWD ".Network") ? 3 : 0;
			if (!kind)
				continue;
			if (kind < 3 && !d)
				d = dev_for(w, path);
			if (kind == 3 && !n && w->nnet < MAXNET) {
				n = &w->net[w->nnet++];
				memset(n, 0, sizeof *n);
				n->signal = INT_MIN;
				snprintf(n->path, sizeof n->path, "%s", path);
			}
			if ((kind < 3 && !d) || (kind == 3 && !n))
				continue;
			if (kind == 2)
				d->station = 1;
			for (; dbus_message_iter_get_arg_type(&props) == DBUS_TYPE_DICT_ENTRY;
			     dbus_message_iter_next(&props)) {
				DBusMessageIter pe, var;
				const char *key = "";
				dbus_message_iter_recurse(&props, &pe);
				dbus_message_iter_get_basic(&pe, &key);
				dbus_message_iter_next(&pe);
				dbus_message_iter_recurse(&pe, &var);
				if (kind == 1 && !strcmp(key, "Name"))
					copy_str(d->name, sizeof d->name, &var);
				else if (kind == 1 && !strcmp(key, "Powered"))
					d->powered = get_bool(&var);
				else if (kind == 2 && !strcmp(key, "State"))
					copy_str(d->state, sizeof d->state, &var);
				else if (kind == 2 && !strcmp(key, "Scanning"))
					d->scanning = get_bool(&var);
				else if (kind == 2 && !strcmp(key, "ConnectedNetwork"))
					copy_str(d->conn, sizeof d->conn, &var);
				else if (kind == 3 && !strcmp(key, "Name"))
					copy_str(n->name, sizeof n->name, &var);
				else if (kind == 3 && !strcmp(key, "Type"))
					copy_str(n->type, sizeof n->type, &var);
				else if (kind == 3 && !strcmp(key, "Device"))
					copy_str(n->dev, sizeof n->dev, &var);
				else if (kind == 3 && !strcmp(key, "KnownNetwork"))
					copy_str(n->known, sizeof n->known, &var);
				else if (kind == 3 && !strcmp(key, "Connected"))
					n->connected = get_bool(&var);
			}
		}
	}
}

static const struct wdev *primary(const struct wifi *w)
{
	for (int i = 0; i < w->ndev; i++)
		if (w->dev[i].station)
			return &w->dev[i];
	return NULL;
}

static int net_cmp(const void *a, const void *b)
{
	const struct wnet *x = a, *y = b;
	if (x->connected != y->connected)
		return y->connected - x->connected;
	if (x->signal != y->signal)
		return x->signal < y->signal ? 1 : -1;
	return strcmp(x->name, y->name);
}

static void wired_scan(struct wifi *w)
{
	DIR *d = opendir("/sys/class/net");
	struct dirent *e;
	w->wired = 0;
	w->wired_name[0] = 0;
	while (d && (e = readdir(d))) {
		char p[300], st[32] = "";
		if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo"))
			continue;
		snprintf(p, sizeof p, "/sys/class/net/%s/wireless", e->d_name);
		if (access(p, F_OK) == 0)
			continue;
		snprintf(p, sizeof p, "/sys/class/net/%s/device", e->d_name);
		if (access(p, F_OK) != 0)
			continue; /* virtual interface */
		snprintf(p, sizeof p, "/sys/class/net/%s/operstate", e->d_name);
		FILE *f = fopen(p, "r");
		if (f) {
			if (fscanf(f, "%31s", st) != 1)
				st[0] = 0;
			fclose(f);
		}
		int up = !strcmp(st, "up") || !strcmp(st, "unknown");
		if (up || w->wired == 0) {
			w->wired = up ? 2 : 1;
			snprintf(w->wired_name, sizeof w->wired_name, "%.23s", e->d_name);
		}
		if (up)
			break;
	}
	if (d)
		closedir(d);
}

static void refresh_done(struct wifi *w)
{
	w->refreshing = 0;
	qsort(w->net, (size_t)w->nnet, sizeof w->net[0], net_cmp);
	int vis_max = w->nnet > 0 ? w->nnet - 1 : 0;
	if (w->first > vis_max)
		w->first = vis_max;
	redraw(w);
	if (w->again) {
		w->again = 0;
		refresh(w);
	}
}

static void on_ordered(DBusPendingCall *pc, void *ud)
{
	struct wifi *w = ud;
	const char *err;
	DBusMessage *r = reply_of(pc, &err);
	DBusMessageIter it, arr, st;
	if (r && !err && dbus_message_iter_init(r, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
		dbus_message_iter_recurse(&it, &arr);
		for (; dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRUCT; dbus_message_iter_next(&arr)) {
			const char *path = "";
			int16_t sig = 0;
			dbus_message_iter_recurse(&arr, &st);
			dbus_message_iter_get_basic(&st, &path);
			dbus_message_iter_next(&st);
			if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_INT16)
				dbus_message_iter_get_basic(&st, &sig);
			for (int i = 0; i < w->nnet; i++)
				if (!strcmp(w->net[i].path, path))
					w->net[i].signal = sig;
		}
	}
	if (r)
		dbus_message_unref(r);
	refresh_done(w);
}

static void on_objects(DBusPendingCall *pc, void *ud)
{
	struct wifi *w = ud;
	const char *err;
	DBusMessage *r = reply_of(pc, &err);
	DBusMessageIter it;
	if (!r || err) {
		w->ndev = w->nnet = 0;
		if (is_err(err, "org.freedesktop.DBus.Error.AccessDenied"))
			w->state = WS_DENIED;
		else
			w->state = WS_NOIWD; /* ServiceUnknown, NameHasNoOwner, NoReply */
		w->agent_ok = 0;
		if (r)
			dbus_message_unref(r);
		w->refreshing = 0;
		w->again = 0;
		redraw(w);
		return;
	}
	w->state = WS_OK;
	if (dbus_message_iter_init(r, &it))
		parse_objects(w, &it);
	dbus_message_unref(r);
	if (!w->agent_ok && !w->agent_busy)
		register_agent(w);
	const struct wdev *p = primary(w);
	if (p && p->powered) {
		call(w, method(p->path, IWD ".Station", "GetOrderedNetworks"), on_ordered);
		/* a visible page asks for one scan when it first sees the station */
		if (w->visible && !w->scanned && !p->scanning) {
			w->scanned = 1;
			simple_call(w, p->path, IWD ".Station", "Scan");
		}
	} else {
		refresh_done(w);
	}
}

static void refresh(struct wifi *w)
{
	if (!w->bus)
		return;
	if (w->refreshing) {
		w->again = 1;
		return;
	}
	w->refreshing = 1;
	wired_scan(w);
	if (!call(w, dbus_message_new_method_call(IWD, "/", "org.freedesktop.DBus.ObjectManager", "GetManagedObjects"),
	          on_objects))
		w->refreshing = 0;
}

static void on_refresh_timer(void *ud)
{
	refresh(ud);
}

static void on_poll_timer(void *ud)
{
	refresh(ud);
}

/* ---- the agent ------------------------------------------------------------------------------ */

static void on_agent_registered(DBusPendingCall *pc, void *ud)
{
	struct wifi *w = ud;
	const char *err;
	DBusMessage *r = reply_of(pc, &err);
	w->agent_busy = 0;
	w->agent_ok = !err;
	if (r)
		dbus_message_unref(r);
}

static void register_agent(struct wifi *w)
{
	DBusMessage *m = method("/net/connman/iwd", IWD ".AgentManager", "RegisterAgent");
	const char *path = AGENT_PATH;
	if (!m)
		return;
	dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID);
	w->agent_busy = call(w, m, on_agent_registered) != NULL;
}

static void wipe_pw(struct wifi *w)
{
	explicit_bzero(w->pw, sizeof w->pw);
	w->pwlen = 0;
}

/* Close the prompt; answer iwd's pending request with "cancelled" when it is still open. */
static void drop_prompt(struct wifi *w, int answer_cancel)
{
	if (w->agent_req) {
		if (answer_cancel && w->bus) {
			DBusMessage *e = dbus_message_new_error(w->agent_req, "net.connman.iwd.Agent.Error.Canceled", "canceled");
			if (e) {
				dbus_connection_send(w->bus, e, NULL);
				dbus_message_unref(e);
				dbus_connection_flush(w->bus);
			}
		}
		dbus_message_unref(w->agent_req);
		w->agent_req = NULL;
	}
	wipe_pw(w);
	w->show = 0;
	redraw(w);
}

static void send_passphrase(struct wifi *w)
{
	if (!w->agent_req || !w->bus)
		return;
	DBusMessage *rep = dbus_message_new_method_return(w->agent_req);
	if (rep) {
		const char *p = w->pw;
		dbus_message_append_args(rep, DBUS_TYPE_STRING, &p, DBUS_TYPE_INVALID);
		dbus_connection_send(w->bus, rep, NULL);
		dbus_message_unref(rep);
		dbus_connection_flush(w->bus);
	}
	dbus_message_unref(w->agent_req);
	w->agent_req = NULL;
	wipe_pw(w);
	w->show = 0;
	redraw(w);
}

static void agent_reply_error(DBusConnection *c, DBusMessage *m, const char *name)
{
	DBusMessage *e = dbus_message_new_error(m, name, "not supported");
	if (e) {
		dbus_connection_send(c, e, NULL);
		dbus_message_unref(e);
	}
}

static DBusHandlerResult agent_msg(DBusConnection *c, DBusMessage *m, void *ud)
{
	struct wifi *w = ud;
	const char *iface = dbus_message_get_interface(m), *mem = dbus_message_get_member(m);
	if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL || !iface || !mem || strcmp(iface, IWD ".Agent"))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (!strcmp(mem, "RequestPassphrase")) {
		const char *np = NULL;
		dbus_message_get_args(m, NULL, DBUS_TYPE_OBJECT_PATH, &np, DBUS_TYPE_INVALID);
		if (w->agent_req)
			drop_prompt(w, 1);
		w->agent_req = dbus_message_ref(m);
		snprintf(w->prompt_name, sizeof w->prompt_name, "%s", w->connecting);
		for (int i = 0; np && i < w->nnet; i++)
			if (!strcmp(w->net[i].path, np))
				snprintf(w->prompt_name, sizeof w->prompt_name, "%s", w->net[i].name);
		wipe_pw(w);
		w->show = 0;
		w->focus_pw = 1;
		redraw(w);
		return DBUS_HANDLER_RESULT_HANDLED; /* answered by send_passphrase or drop_prompt */
	}
	if (!strcmp(mem, "Cancel") || !strcmp(mem, "Release")) {
		drop_prompt(w, 0);
		if (!strcmp(mem, "Release"))
			w->agent_ok = 0;
		DBusMessage *r = dbus_message_new_method_return(m);
		if (r) {
			dbus_connection_send(c, r, NULL);
			dbus_message_unref(r);
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (!strcmp(mem, "RequestPrivateKeyPassphrase") || !strcmp(mem, "RequestUserNameAndPassword") ||
	    !strcmp(mem, "RequestUserPassword")) {
		set_msg(w, 1, "This network needs sign-in details that Settings cannot ask for yet.");
		agent_reply_error(c, m, "net.connman.iwd.Agent.Error.Canceled");
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	agent_reply_error(c, m, "org.freedesktop.DBus.Error.UnknownMethod");
	return DBUS_HANDLER_RESULT_HANDLED;
}

/* ---- signals ------------------------------------------------------------------------------- */

static DBusHandlerResult filter(DBusConnection *c, DBusMessage *m, void *ud)
{
	struct wifi *w = ud;
	if (dbus_message_get_type(m) == DBUS_MESSAGE_TYPE_SIGNAL) {
		if (dbus_message_is_signal(m, "org.freedesktop.DBus.Local", "Disconnected")) {
			w->lost = 1;
		} else if (dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")) {
			const char *name = "", *old = "", *nw = "";
			dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &name, DBUS_TYPE_STRING, &old, DBUS_TYPE_STRING, &nw,
			                      DBUS_TYPE_INVALID);
			if (!strcmp(name, IWD)) {
				w->agent_ok = 0;
				w->agent_busy = 0;
				w->scanned = 0;
				if (!*nw) {
					w->state = WS_NOIWD;
					w->ndev = w->nnet = 0;
					drop_prompt(w, 0);
					w->connecting[0] = 0;
					redraw(w);
				} else {
					fl_timer_arm(w->t_refresh, 300, 0);
				}
			}
		} else if (dbus_message_get_path(m)) { /* anything else is from iwd (see the match rule) */
			fl_timer_arm(w->t_refresh, 150, 0);
		}
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ---- actions -------------------------------------------------------------------------------- */

static void on_connect(DBusPendingCall *pc, void *ud)
{
	struct wifi *w = ud;
	const char *err;
	DBusMessage *r = reply_of(pc, &err);
	char name[72];
	snprintf(name, sizeof name, "%s", w->connecting);
	w->connecting[0] = 0;
	if (w->agent_req)
		drop_prompt(w, 1); /* iwd gave up on the question */
	if (!err) {
		w->msg[0] = 0;
	} else if (w->canceled) {
		w->msg[0] = 0;
	} else if (is_err(err, "net.connman.iwd.Failed") || is_err(err, "net.connman.iwd.InvalidArguments")) {
		set_msg(w, 1, "Could not connect to %s. The password may be wrong.", name);
	} else if (is_err(err, "net.connman.iwd.InProgress") || is_err(err, "net.connman.iwd.Busy")) {
		set_msg(w, 1, "Wi-Fi is busy. Try again in a moment.");
	} else if (is_err(err, "net.connman.iwd.NoAgent")) {
		set_msg(w, 1, "Could not ask for the password: no password agent.");
	} else if (is_err(err, "net.connman.iwd.NotSupported")) {
		set_msg(w, 1, "%s uses a kind of security that is not supported.", name);
	} else if (is_err(err, "net.connman.iwd.Aborted")) {
		w->msg[0] = 0;
	} else if (is_err(err, "org.freedesktop.DBus.Error.AccessDenied")) {
		set_msg(w, 1, "You are not allowed to use Wi-Fi.");
	} else {
		set_msg(w, 1, "Could not connect to %s.", name);
	}
	w->canceled = 0;
	if (r)
		dbus_message_unref(r);
	refresh(w);
	redraw(w);
}

static void do_connect(struct wifi *w, const struct wnet *n)
{
	if (w->connecting[0])
		return;
	snprintf(w->connecting, sizeof w->connecting, "%s", n->name);
	w->canceled = 0;
	w->msg[0] = 0;
	if (!call(w, method(n->path, IWD ".Network", "Connect"), on_connect)) {
		w->connecting[0] = 0;
		set_msg(w, 1, "Cannot reach the Wi-Fi service.");
	}
	redraw(w);
}

static void do_power(struct wifi *w, const struct wdev *d, int on)
{
	DBusMessage *m = method(d->path, "org.freedesktop.DBus.Properties", "Set");
	DBusMessageIter it, v;
	const char *iface = IWD ".Device", *prop = "Powered";
	dbus_bool_t b = on;
	if (!m)
		return;
	dbus_message_iter_init_append(m, &it);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &prop);
	dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "b", &v);
	dbus_message_iter_append_basic(&v, DBUS_TYPE_BOOLEAN, &b);
	dbus_message_iter_close_container(&it, &v);
	w->scanned = 0;
	call(w, m, on_generic);
}

/* ---- public setup --------------------------------------------------------------------------- */

struct wifi *wifi_new(fl_app *app, fl_font *head, fl_font *small)
{
	struct wifi *w = calloc(1, sizeof *w);
	if (!w)
		return NULL;
	w->app = app;
	w->head = head;
	w->small = small;
	w->fd = -1;
	w->state = WS_LOADING;
	w->t_refresh = fl_timer_new(app, on_refresh_timer, w);
	w->t_pump = fl_timer_new(app, on_pump, w);
	w->t_poll = fl_timer_new(app, on_poll_timer, w);
	wired_scan(w);

	DBusError err;
	dbus_error_init(&err);
	w->bus = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
	dbus_error_free(&err);
	if (!w->bus || !dbus_connection_get_unix_fd(w->bus, &w->fd)) {
		if (w->bus) {
			dbus_connection_close(w->bus);
			dbus_connection_unref(w->bus);
			w->bus = NULL;
		}
		w->state = WS_NOBUS;
		return w;
	}
	dbus_connection_set_exit_on_disconnect(w->bus, FALSE);
	static const DBusObjectPathVTable vt = {.message_function = agent_msg};
	dbus_connection_register_object_path(w->bus, AGENT_PATH, &vt, w);
	dbus_connection_add_filter(w->bus, filter, w, NULL);
	dbus_error_init(&err);
	dbus_bus_add_match(w->bus,
	                   "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
	                   "member='NameOwnerChanged',arg0='" IWD "'", &err);
	dbus_error_free(&err);
	dbus_error_init(&err);
	dbus_bus_add_match(w->bus, "type='signal',sender='" IWD "'", &err);
	dbus_error_free(&err);
	fl_fd_add(app, w->fd, POLLIN, on_fd, w);
	dbus_connection_flush(w->bus);
	refresh(w);
	return w;
}

void wifi_set_win(struct wifi *w, fl_win *win)
{
	w->win = win;
}

void wifi_free(struct wifi *w)
{
	if (!w)
		return;
	if (w->bus) {
		if (w->agent_ok) {
			DBusMessage *m = method("/net/connman/iwd", IWD ".AgentManager", "UnregisterAgent");
			const char *path = AGENT_PATH;
			if (m) {
				dbus_message_append_args(m, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INVALID);
				dbus_connection_send(w->bus, m, NULL);
				dbus_message_unref(m);
				dbus_connection_flush(w->bus);
			}
		}
		drop_prompt(w, 1);
		dbus_connection_close(w->bus);
		dbus_connection_unref(w->bus);
	}
	explicit_bzero(w->pw, sizeof w->pw);
	fl_timer_free(w->t_refresh);
	fl_timer_free(w->t_pump);
	fl_timer_free(w->t_poll);
	free(w);
}

void wifi_show(struct wifi *w, int on)
{
	if (!w)
		return;
	w->visible = on;
	if (on) {
		w->scanned = 0;
		fl_timer_arm(w->t_poll, 5000, 5000);
		refresh(w);
	} else {
		fl_timer_arm(w->t_poll, 0, 0);
	}
}

int wifi_prompting(const struct wifi *w)
{
	return w && w->agent_req != NULL;
}

/* ---- drawing -------------------------------------------------------------------------------- */

static int bars_for(int signal)
{
	if (signal == INT_MIN)
		return 0;
	int dbm = signal / 100;
	return dbm >= -55 ? 4 : dbm >= -65 ? 3 : dbm >= -75 ? 2 : 1;
}

static void draw_bars(fl_canvas *cv, int x, int y, int n, fl_color on, fl_color off)
{
	for (int i = 0; i < 4; i++) {
		int h = 4 + 3 * i;
		fl_fill_rect(cv, fl_r(x + i * 5, y + 13 - h, 3, h), i < n ? on : off);
	}
}

static void draw_lock(fl_canvas *cv, int x, int y, fl_color c)
{
	fl_stroke_rect(cv, fl_r(x + 2, y, 7, 7), 1, c);
	fl_fill_rect(cv, fl_r(x, y + 5, 11, 8), c);
}

static void section(fl_ui *ui, struct wifi *w, int x, int y, int cw, const char *name)
{
	fl_text(&ui->cv, w->head, x, y, fl_th.dim, name, -1);
	fl_fill_rect(&ui->cv, fl_r(x, y + 20, cw, 1), fl_th.line);
}

/* title and a dim line under it in a row h px high, x to x + maxw */
static void row_text(fl_ui *ui, struct wifi *w, int x, int y, int h, int maxw, const char *title, const char *what)
{
	int th = fl_font_height(ui->f_med), sh = fl_font_height(w->small);
	int top = y + (h - th - 2 - sh) / 2;
	char fit[160];
	fl_text_fit(ui->f_med, title, maxw, fit, sizeof fit);
	fl_text(&ui->cv, ui->f_med, x, top, fl_th.text, fit, -1);
	fl_text_fit(w->small, what, maxw, fit, sizeof fit);
	fl_text(&ui->cv, w->small, x, top + th + 2, fl_th.dim, fit, -1);
}

static const struct wnet *net_by_path(const struct wifi *w, const char *path)
{
	for (int i = 0; i < w->nnet; i++)
		if (!strcmp(w->net[i].path, path))
			return &w->net[i];
	return NULL;
}

static void status_text(const struct wifi *w, const struct wdev *p, char *out, size_t cap)
{
	const struct wnet *c = p->conn[0] ? net_by_path(w, p->conn) : NULL;
	if (!p->powered)
		snprintf(out, cap, "Turned off");
	else if (!strcmp(p->state, "connected") || !strcmp(p->state, "roaming"))
		snprintf(out, cap, "Connected to %s", c ? c->name : "a network");
	else if (!strcmp(p->state, "connecting") || w->connecting[0])
		snprintf(out, cap, "Connecting%s%s...", w->connecting[0] ? " to " : "", w->connecting);
	else if (!strcmp(p->state, "disconnecting"))
		snprintf(out, cap, "Disconnecting...");
	else
		snprintf(out, cap, "Not connected");
	if (p->powered && p->scanning)
		snprintf(out + strlen(out), cap - strlen(out), " (scanning)");
}

/* a full-width message in the middle of the list area */
static void centered(fl_ui *ui, struct wifi *w, fl_rect a, const char *l1, const char *l2)
{
	int h1 = fl_font_height(ui->f_med);
	int y = a.y + a.h / 2 - h1;
	fl_text_in(&ui->cv, ui->f_med, fl_r(a.x, y, a.w, h1), FL_CENTER, 0, fl_th.text, l1);
	fl_text_in(&ui->cv, w->small, fl_r(a.x, y + h1 + 6, a.w, fl_font_height(w->small)), FL_CENTER, 0, fl_th.dim, l2);
}

static void draw_prompt(fl_ui *ui, struct wifi *w, fl_rect a)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, a, fl_th.raised);
	fl_stroke_rect(cv, a, 1, fl_th.line);
	int x = a.x + 20, cw = a.w - 40, y = a.y + 18;
	char t[160], fit[160];
	snprintf(t, sizeof t, "Password for %s", w->prompt_name);
	fl_text_fit(ui->f_med, t, cw, fit, sizeof fit);
	fl_text(cv, ui->f_med, x, y, fl_th.strong, fit, -1);
	y += fl_font_height(ui->f_med) + 4;
	snprintf(t, sizeof t, "%d to %d characters", PWMIN, PWMAX);
	fl_text(cv, w->small, x, y, fl_th.dim, t, -1);
	y += fl_font_height(w->small) + 12;

	/* the masked field */
	fl_rect r = fl_r(x, y, cw, 36);
	if (w->focus_pw) { /* before the widget, or the pass drops a focus it has not met */
		ui->focus = ID_PW;
		w->focus_pw = 0;
	}
	int res = fl_widget(ui, ID_PW, r, FL_FOCUSABLE | FL_NO_FOCUS_RING);
	fl_fill_rect(cv, r, fl_th.bg);
	fl_stroke_rect(cv, r, 1, (res & FL_FOCUS) ? fl_th.accent : fl_th.line);
	if (res & FL_FOCUS) {
		if (ui->key == XKB_KEY_BackSpace) {
			if (w->pwlen > 0) {
				/* back over one UTF-8 character */
				while (w->pwlen > 0) {
					unsigned char b = (unsigned char)w->pw[--w->pwlen];
					w->pw[w->pwlen] = 0;
					if ((b & 0xc0) != 0x80)
						break;
				}
			}
			ui->key = 0;
		} else if ((ui->mods & FL_MOD_CTRL) && (ui->key == 'v' || ui->key == 'V')) {
			size_t n = 0;
			char *clip = fl_clipboard_get(w->app, &n);
			for (size_t i = 0; clip && i < n && clip[i] != '\n' && clip[i] != '\r' && w->pwlen < PWMAX; i++)
				w->pw[w->pwlen++] = clip[i];
			if (clip) {
				explicit_bzero(clip, n);
				free(clip);
			}
			ui->key = 0;
		} else if (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_KP_Enter) {
			ui->key = 0;
			if (w->pwlen >= PWMIN)
				send_passphrase(w);
		} else if (ui->text[0] && !(ui->mods & (FL_MOD_CTRL | FL_MOD_ALT)) && (unsigned char)ui->text[0] >= 0x20 &&
		           ui->text[0] != 0x7f) {
			size_t n = strlen(ui->text);
			if (w->pwlen + (int)n <= PWMAX) {
				memcpy(w->pw + w->pwlen, ui->text, n);
				w->pwlen += (int)n;
				w->pw[w->pwlen] = 0;
			}
			explicit_bzero(ui->text, sizeof ui->text);
			ui->key = 0;
		}
	}
	if (w->agent_req) { /* send_passphrase may have ended the prompt above */
		char shown[PWMAX * 3 + 1];
		const char *s = w->pw;
		if (!w->show) {
			size_t n = 0, chars = 0;
			for (int i = 0; i < w->pwlen; i++)
				chars += ((unsigned char)w->pw[i] & 0xc0) != 0x80;
			for (size_t i = 0; i < chars; i++) {
				memcpy(shown + n, "\xe2\x80\xa2", 3); /* U+2022 */
				n += 3;
			}
			shown[n] = 0;
			s = shown;
		}
		fl_canvas saved = *cv;
		fl_clip(cv, fl_inset(r, 2, 2));
		int tw = fl_text_width(ui->f_reg, s, -1), avail = r.w - 20;
		int tx = r.x + 10 + (tw > avail ? avail - tw : 0);
		fl_text_in(cv, ui->f_reg, fl_r(tx, r.y, tw + 4, r.h), FL_LEFT, 0, fl_th.text, s);
		if (res & FL_FOCUS)
			fl_fill_rect(cv, fl_r(tx + tw + 1, r.y + 9, 1, r.h - 18), fl_th.accent);
		*cv = saved;
		if (s == shown)
			explicit_bzero(shown, sizeof shown);
	}
	y += 36 + 14;
	fl_rect row = fl_r(x, y, cw, 32);
	fl_rect ok = fl_cut_right(&row, 96);
	fl_cut_right(&row, 8);
	fl_rect cancel = fl_cut_right(&row, 84);
	fl_cut_right(&row, 8);
	fl_rect show = fl_cut_right(&row, 76);
	if (fl_button(ui, ID_SHOW, show, w->show ? "Hide" : "Show", FL_BTN_OUTLINE))
		w->show = !w->show;
	if (fl_button(ui, ID_CANCEL, cancel, "Cancel", FL_BTN_OUTLINE) ||
	    (ui->key == XKB_KEY_Escape && w->agent_req)) {
		ui->key = 0;
		w->canceled = 1;
		drop_prompt(w, 1);
	} else if (fl_button(ui, ID_OK, ok, "Connect", FL_BTN_ACCENT) && w->pwlen >= PWMIN) {
		send_passphrase(w);
	}
}

static void draw_row(fl_ui *ui, struct wifi *w, fl_rect r, int i)
{
	fl_canvas *cv = &ui->cv;
	const struct wnet *n = &w->net[i];
	int busy = w->connecting[0] != 0;
	int is_conn = n->connected;
	int this_busy = busy && !strcmp(w->connecting, n->name);
	if (is_conn) {
		fl_fill_rect(cv, r, fl_th.select);
		fl_fill_rect(cv, fl_r(r.x, r.y, 3, r.h), fl_th.accent);
	}
	fl_fill_rect(cv, fl_r(r.x, r.y + r.h - 1, r.w, 1), fl_th.line);

	int bw = 84, gap = 8;
	int bx = r.x + r.w - 10 - bw;
	fl_rect main_btn = fl_r(bx, r.y + 10, bw, 28);
	fl_rect forget_btn = main_btn;
	int has_forget = n->known[0] != 0;
	if (has_forget)
		main_btn.x -= bw + gap;
	int left_end = main_btn.x - 14;

	/* lock and signal bars, right before the buttons */
	int open = !strcmp(n->type, "open");
	draw_bars(cv, left_end - 18, r.y + (r.h - 13) / 2, bars_for(n->signal), fl_th.text, fl_th.line);
	if (!open)
		draw_lock(cv, left_end - 18 - 12 - 12, r.y + (r.h - 13) / 2, fl_th.dim);

	const char *kind = open ? "Open" : !strcmp(n->type, "psk") ? "WPA" : !strcmp(n->type, "8021x") ? "Enterprise"
	                 : !strcmp(n->type, "wep") ? "WEP" : n->type;
	char sub[96];
	snprintf(sub, sizeof sub, "%s%s%s", is_conn ? "Connected, " : this_busy ? "Connecting..., " : n->known[0] ? "Saved, " : "",
	         kind, "");
	row_text(ui, w, r.x + 14, r.y, r.h, left_end - 18 - 24 - 14 - r.x - 14, n->name[0] ? n->name : "(hidden)", sub);

	if (is_conn) {
		if (fl_button(ui, ID_ROW + i * 4, main_btn, "Disconnect", FL_BTN_OUTLINE)) {
			const struct wdev *p = primary(w);
			if (p)
				simple_call(w, p->path, IWD ".Station", "Disconnect");
		}
	} else if (fl_button(ui, ID_ROW + i * 4, main_btn, "Connect", this_busy ? FL_BTN_PLAIN : FL_BTN_ACCENT) && !busy) {
		do_connect(w, n);
	}
	if (has_forget && fl_button(ui, ID_ROW + i * 4 + 1, forget_btn, "Forget", FL_BTN_OUTLINE)) {
		simple_call(w, n->known, IWD ".KnownNetwork", "Forget");
		set_msg(w, 0, "Forgot %s.", n->name);
	}
}

void wifi_draw(struct wifi *w, fl_ui *ui, fl_rect c)
{
	fl_canvas *cv = &ui->cv;
	int x = c.x + 28, cw = c.w - 56, y = c.y + 24;
	char buf[200];

	/* ---- connection status: wired, then Wi-Fi */
	section(ui, w, x, y, cw, "Connection");
	y += 28;
	snprintf(buf, sizeof buf, "%s", w->wired == 2 ? "Connected" : w->wired == 1 ? "No cable plugged in" : "No wired adapter");
	if (w->wired_name[0])
		snprintf(buf + strlen(buf), sizeof buf - strlen(buf), " (%s)", w->wired_name);
	fl_icon_draw(cv, FL_ICON_ETHERNET, x, y + 16, 16, w->wired == 2 ? fl_th.accent : fl_th.dim);
	row_text(ui, w, x + 28, y, 48, cw - 28, "Wired", buf);
	y += 48;

	const struct wdev *p = w->state == WS_OK ? primary(w) : NULL;
	fl_icon_draw(cv, FL_ICON_WIFI, x, y + 18, 16, p && p->powered && !strcmp(p->state, "connected") ? fl_th.accent : fl_th.dim);
	if (p) {
		status_text(w, p, buf, sizeof buf);
		row_text(ui, w, x + 28, y, 56, cw - 28 - 56, "Wi-Fi", buf);
		int on = p->powered;
		if (fl_switch(ui, ID_SW, fl_r(x + cw - 40, y + 18, 40, 20), &on)) {
			do_power(w, p, on);
			w->msg[0] = 0;
		}
	} else {
		const char *what = w->state == WS_LOADING ? "Looking for the Wi-Fi service..."
		                 : w->state == WS_OK ? "No Wi-Fi hardware found"
		                 : w->state == WS_DENIED ? "Not allowed to use Wi-Fi"
		                 : w->state == WS_NOBUS ? "System message bus not available" : "Wi-Fi service not running";
		row_text(ui, w, x + 28, y, 56, cw - 28, "Wi-Fi", what);
	}
	y += 56 + 16;

	/* ---- networks */
	section(ui, w, x, y, cw, "Networks");
	if (p && p->powered) {
		fl_rect sb = fl_r(x + cw - 96, y - 6, 96, 25);
		if (fl_button(ui, ID_SCAN, sb, p->scanning ? "Scanning..." : "Scan", FL_BTN_OUTLINE) && !p->scanning) {
			simple_call(w, p->path, IWD ".Station", "Scan");
			w->msg[0] = 0;
		}
	}
	y += 26;
	int bottom = c.y + c.h - 40;
	fl_rect area = fl_r(x, y, cw, bottom - y);

	if (w->agent_req) {
		draw_prompt(ui, w, fl_r(area.x, area.y, area.w, area.h < 190 ? area.h : 190));
	} else if (w->state != WS_OK) {
		const char *l1, *l2;
		switch (w->state) {
		case WS_NOBUS:
			l1 = "The system message bus is not running";
			l2 = "Wi-Fi needs D-Bus. An administrator can start it: /etc/rc.d/init.d/dbus start";
			break;
		case WS_NOIWD:
			l1 = "The Wi-Fi service (iwd) is not running";
			l2 = "An administrator can start it: /etc/rc.d/init.d/iwd start";
			break;
		case WS_DENIED:
			l1 = "You are not allowed to use Wi-Fi";
			l2 = "Your account needs to be in the group seat (or netdev).";
			break;
		default:
			l1 = "Looking for the Wi-Fi service...";
			l2 = "";
		}
		centered(ui, w, area, l1, l2);
	} else if (!p) {
		centered(ui, w, area, "No Wi-Fi hardware found",
		         "This computer has no Wi-Fi adapter, or its driver or firmware did not load.");
	} else if (!p->powered) {
		centered(ui, w, area, "Wi-Fi is turned off", "Use the switch above to turn it on.");
	} else {
		/* only the networks of the primary device; whole rows only, scrolled by rows */
		int idx[MAXNET], n = 0;
		for (int i = 0; i < w->nnet; i++)
			if (!strcmp(w->net[i].dev, p->path))
				idx[n++] = i;
		int rows = area.h / ROWH;
		if (rows < 1)
			rows = 1;
		if (n == 0) {
			centered(ui, w, area, p->scanning ? "Looking for networks..." : "No networks found",
			         p->scanning ? "" : "Press Scan to look again.");
		} else {
			int maxfirst = n > rows ? n - rows : 0;
			if (ui->wheel && fl_in(area, ui->mx, ui->my)) {
				w->first += ui->wheel;
				ui->wheel = 0;
			}
			if (w->first > maxfirst)
				w->first = maxfirst;
			if (w->first < 0)
				w->first = 0;
			int sbw = n > rows ? 10 : 0;
			for (int k = 0; k < rows && w->first + k < n; k++)
				draw_row(ui, w, fl_r(area.x, area.y + k * ROWH, area.w - sbw, ROWH), idx[w->first + k]);
			if (sbw) {
				int off = w->first * ROWH;
				fl_scrollbar(ui, ID_SB, fl_r(area.x + area.w - sbw, area.y, sbw, rows * ROWH), &off, n * ROWH, rows * ROWH);
				w->first = (off + ROWH / 2) / ROWH;
			}
		}
	}

	if (w->msg[0])
		fl_text_in(cv, w->small, fl_r(x, c.y + c.h - 34, cw, 22), FL_LEFT, 0, w->msg_err ? fl_th.err : fl_th.dim, w->msg);
}
