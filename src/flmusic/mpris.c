/* mpris.c -- org.mpris.MediaPlayer2 for flmusic (libdbus, driven by flui's poll loop)
 * SPDX-License-Identifier: MIT
 */
#include "mpris.h"

#include <dbus/dbus.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUS_NAME "org.mpris.MediaPlayer2.flmusic"
#define OBJ_PATH "/org/mpris/MediaPlayer2"
#define IF_ROOT "org.mpris.MediaPlayer2"
#define IF_PLAYER "org.mpris.MediaPlayer2.Player"
#define IF_PROPS "org.freedesktop.DBus.Properties"

struct mpris {
	fl_app *app;
	DBusConnection *bus;
	struct mpris_ops ops;
	int fd;
};

static const char *introspect_xml =
	"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
	"\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
	"<node>\n"
	" <interface name=\"org.freedesktop.DBus.Introspectable\"><method name=\"Introspect\">"
	"<arg type=\"s\" direction=\"out\"/></method></interface>\n"
	" <interface name=\"org.freedesktop.DBus.Properties\">"
	"<method name=\"Get\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"out\"/></method>"
	"<method name=\"GetAll\"><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"out\"/></method>"
	"<method name=\"Set\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"in\"/></method>"
	"<signal name=\"PropertiesChanged\"><arg type=\"s\"/><arg type=\"a{sv}\"/><arg type=\"as\"/></signal></interface>\n"
	" <interface name=\"org.mpris.MediaPlayer2\"><method name=\"Raise\"/><method name=\"Quit\"/>"
	"<property name=\"CanQuit\" type=\"b\" access=\"read\"/><property name=\"CanRaise\" type=\"b\" access=\"read\"/>"
	"<property name=\"HasTrackList\" type=\"b\" access=\"read\"/><property name=\"Identity\" type=\"s\" access=\"read\"/>"
	"<property name=\"DesktopEntry\" type=\"s\" access=\"read\"/><property name=\"SupportedUriSchemes\" type=\"as\" access=\"read\"/>"
	"<property name=\"SupportedMimeTypes\" type=\"as\" access=\"read\"/></interface>\n"
	" <interface name=\"org.mpris.MediaPlayer2.Player\">"
	"<method name=\"Next\"/><method name=\"Previous\"/><method name=\"Pause\"/><method name=\"PlayPause\"/>"
	"<method name=\"Stop\"/><method name=\"Play\"/><method name=\"Seek\"><arg type=\"x\" direction=\"in\"/></method>"
	"<method name=\"SetPosition\"><arg type=\"o\" direction=\"in\"/><arg type=\"x\" direction=\"in\"/></method>"
	"<method name=\"OpenUri\"><arg type=\"s\" direction=\"in\"/></method>"
	"<signal name=\"Seeked\"><arg type=\"x\"/></signal>"
	"<property name=\"PlaybackStatus\" type=\"s\" access=\"read\"/><property name=\"LoopStatus\" type=\"s\" access=\"readwrite\"/>"
	"<property name=\"Rate\" type=\"d\" access=\"readwrite\"/><property name=\"Shuffle\" type=\"b\" access=\"readwrite\"/>"
	"<property name=\"Metadata\" type=\"a{sv}\" access=\"read\"/><property name=\"Volume\" type=\"d\" access=\"readwrite\"/>"
	"<property name=\"Position\" type=\"x\" access=\"read\"/><property name=\"MinimumRate\" type=\"d\" access=\"read\"/>"
	"<property name=\"MaximumRate\" type=\"d\" access=\"read\"/><property name=\"CanGoNext\" type=\"b\" access=\"read\"/>"
	"<property name=\"CanGoPrevious\" type=\"b\" access=\"read\"/><property name=\"CanPlay\" type=\"b\" access=\"read\"/>"
	"<property name=\"CanPause\" type=\"b\" access=\"read\"/><property name=\"CanSeek\" type=\"b\" access=\"read\"/>"
	"<property name=\"CanControl\" type=\"b\" access=\"read\"/></interface>\n"
	"</node>\n";

/* ---- value writers ---------------------------------------------------------------------- */

static void v_basic(DBusMessageIter *it, int type, const char *sig, const void *val)
{
	DBusMessageIter v;
	dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, sig, &v);
	dbus_message_iter_append_basic(&v, type, val);
	dbus_message_iter_close_container(it, &v);
}

static void dict_entry(DBusMessageIter *dict, const char *key, int type, const char *sig, const void *val)
{
	DBusMessageIter e;
	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &e);
	dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &key);
	v_basic(&e, type, sig, val);
	dbus_message_iter_close_container(dict, &e);
}

static void v_strarray(DBusMessageIter *it, const char *const *s, int n)
{
	DBusMessageIter v, a;
	dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "as", &v);
	dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "s", &a);
	for (int i = 0; i < n; i++)
		dbus_message_iter_append_basic(&a, DBUS_TYPE_STRING, &s[i]);
	dbus_message_iter_close_container(&v, &a);
	dbus_message_iter_close_container(it, &v);
}

static void v_metadata(DBusMessageIter *it, const struct mpris_state *st)
{
	DBusMessageIter v, a;
	dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "a{sv}", &v);
	dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "{sv}", &a);
	if (st->title) {
		char path[64];
		snprintf(path, sizeof path, "/org/flmusic/track/%d", st->track_no);
		const char *p = path;
		DBusMessageIter e, ov;
		dbus_message_iter_open_container(&a, DBUS_TYPE_DICT_ENTRY, NULL, &e);
		const char *k = "mpris:trackid";
		dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k);
		dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "o", &ov);
		dbus_message_iter_append_basic(&ov, DBUS_TYPE_OBJECT_PATH, &p);
		dbus_message_iter_close_container(&e, &ov);
		dbus_message_iter_close_container(&a, &e);
		int64_t len = st->length_us;
		dict_entry(&a, "mpris:length", DBUS_TYPE_INT64, "x", &len);
		dict_entry(&a, "xesam:title", DBUS_TYPE_STRING, "s", &st->title);
		if (st->album)
			dict_entry(&a, "xesam:album", DBUS_TYPE_STRING, "s", &st->album);
		if (st->uri)
			dict_entry(&a, "xesam:url", DBUS_TYPE_STRING, "s", &st->uri);
		if (st->artist) {
			DBusMessageIter e2, av, aa;
			dbus_message_iter_open_container(&a, DBUS_TYPE_DICT_ENTRY, NULL, &e2);
			const char *k2 = "xesam:artist";
			dbus_message_iter_append_basic(&e2, DBUS_TYPE_STRING, &k2);
			dbus_message_iter_open_container(&e2, DBUS_TYPE_VARIANT, "as", &av);
			dbus_message_iter_open_container(&av, DBUS_TYPE_ARRAY, "s", &aa);
			dbus_message_iter_append_basic(&aa, DBUS_TYPE_STRING, &st->artist);
			dbus_message_iter_close_container(&av, &aa);
			dbus_message_iter_close_container(&e2, &av);
			dbus_message_iter_close_container(&a, &e2);
		}
	}
	dbus_message_iter_close_container(&v, &a);
	dbus_message_iter_close_container(it, &v);
}

static const char *loop_name(int l)
{
	return l == LOOP_TRACK ? "Track" : l == LOOP_LIST ? "Playlist" : "None";
}

/* Append the value of property NAME of IFACE as a variant; 0 when there is no such property. */
static int prop_value(const struct mpris *m, DBusMessageIter *it, const char *iface, const char *name)
{
	struct mpris_state st;
	memset(&st, 0, sizeof st);
	m->ops.get(m->ops.ud, &st);
	dbus_bool_t t = TRUE, f = FALSE, b;
	double d;
	int64_t x;
	if (!strcmp(iface, IF_ROOT)) {
		static const char *schemes[] = {"file"};
		static const char *mimes[] = {"audio/mpeg", "audio/flac", "audio/ogg", "audio/x-vorbis+ogg",
		                              "audio/opus", "audio/mp4", "audio/x-wav", "audio/aac"};
		const char *s;
		if (!strcmp(name, "CanQuit") || !strcmp(name, "CanRaise"))
			v_basic(it, DBUS_TYPE_BOOLEAN, "b", &t);
		else if (!strcmp(name, "HasTrackList"))
			v_basic(it, DBUS_TYPE_BOOLEAN, "b", &f);
		else if (!strcmp(name, "Identity")) {
			s = "Music";
			v_basic(it, DBUS_TYPE_STRING, "s", &s);
		} else if (!strcmp(name, "DesktopEntry")) {
			s = "flmusic";
			v_basic(it, DBUS_TYPE_STRING, "s", &s);
		} else if (!strcmp(name, "SupportedUriSchemes"))
			v_strarray(it, schemes, 1);
		else if (!strcmp(name, "SupportedMimeTypes"))
			v_strarray(it, mimes, 8);
		else
			return 0;
		return 1;
	}
	if (strcmp(iface, IF_PLAYER))
		return 0;
	if (!strcmp(name, "PlaybackStatus"))
		v_basic(it, DBUS_TYPE_STRING, "s", &st.status);
	else if (!strcmp(name, "LoopStatus")) {
		const char *s = loop_name(st.loop);
		v_basic(it, DBUS_TYPE_STRING, "s", &s);
	} else if (!strcmp(name, "Rate") || !strcmp(name, "MinimumRate") || !strcmp(name, "MaximumRate")) {
		d = 1.0;
		v_basic(it, DBUS_TYPE_DOUBLE, "d", &d);
	} else if (!strcmp(name, "Shuffle")) {
		b = st.shuffle ? TRUE : FALSE;
		v_basic(it, DBUS_TYPE_BOOLEAN, "b", &b);
	} else if (!strcmp(name, "Metadata"))
		v_metadata(it, &st);
	else if (!strcmp(name, "Volume")) {
		d = st.volume;
		v_basic(it, DBUS_TYPE_DOUBLE, "d", &d);
	} else if (!strcmp(name, "Position")) {
		x = st.position_us;
		v_basic(it, DBUS_TYPE_INT64, "x", &x);
	} else if (!strcmp(name, "CanGoNext")) {
		b = st.can_next ? TRUE : FALSE;
		v_basic(it, DBUS_TYPE_BOOLEAN, "b", &b);
	} else if (!strcmp(name, "CanGoPrevious")) {
		b = st.can_prev ? TRUE : FALSE;
		v_basic(it, DBUS_TYPE_BOOLEAN, "b", &b);
	} else if (!strcmp(name, "CanPlay")) {
		b = st.can_play ? TRUE : FALSE;
		v_basic(it, DBUS_TYPE_BOOLEAN, "b", &b);
	} else if (!strcmp(name, "CanPause") || !strcmp(name, "CanSeek") || !strcmp(name, "CanControl"))
		v_basic(it, DBUS_TYPE_BOOLEAN, "b", &t);
	else
		return 0;
	return 1;
}

static const char *const root_props[] = {"CanQuit", "CanRaise", "HasTrackList", "Identity", "DesktopEntry",
                                         "SupportedUriSchemes", "SupportedMimeTypes", NULL};
static const char *const player_props[] = {"PlaybackStatus", "LoopStatus", "Rate", "Shuffle", "Metadata", "Volume",
                                           "Position", "MinimumRate", "MaximumRate", "CanGoNext", "CanGoPrevious",
                                           "CanPlay", "CanPause", "CanSeek", "CanControl", NULL};

static void all_props(const struct mpris *m, DBusMessageIter *dict, const char *iface, const char *const *names)
{
	(void)iface;
	for (int i = 0; names[i]; i++) {
		DBusMessageIter e;
		dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &e);
		dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &names[i]);
		prop_value(m, &e, iface, names[i]);
		dbus_message_iter_close_container(dict, &e);
	}
}

/* ---- incoming calls --------------------------------------------------------------------- */

static void reply(struct mpris *m, DBusMessage *msg, DBusMessage *r)
{
	if (!r)
		return;
	(void)msg;
	dbus_connection_send(m->bus, r, NULL);
	dbus_message_unref(r);
	dbus_connection_flush(m->bus);
}

static void handle_set(struct mpris *m, DBusMessage *msg)
{
	const char *iface, *name;
	DBusMessageIter it, v;
	if (!dbus_message_iter_init(msg, &it))
		goto bad;
	dbus_message_iter_get_basic(&it, &iface);
	dbus_message_iter_next(&it);
	dbus_message_iter_get_basic(&it, &name);
	dbus_message_iter_next(&it);
	if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_VARIANT)
		goto bad;
	dbus_message_iter_recurse(&it, &v);
	if (strcmp(iface, IF_PLAYER))
		goto bad;
	if (!strcmp(name, "Volume") && dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_DOUBLE) {
		double d;
		dbus_message_iter_get_basic(&v, &d);
		if (m->ops.set_volume)
			m->ops.set_volume(m->ops.ud, d < 0 ? 0 : d > 1 ? 1 : d);
	} else if (!strcmp(name, "Shuffle") && dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_BOOLEAN) {
		dbus_bool_t b;
		dbus_message_iter_get_basic(&v, &b);
		if (m->ops.set_shuffle)
			m->ops.set_shuffle(m->ops.ud, b);
	} else if (!strcmp(name, "LoopStatus") && dbus_message_iter_get_arg_type(&v) == DBUS_TYPE_STRING) {
		const char *s;
		dbus_message_iter_get_basic(&v, &s);
		if (m->ops.set_loop)
			m->ops.set_loop(m->ops.ud, !strcmp(s, "Track") ? LOOP_TRACK : !strcmp(s, "Playlist") ? LOOP_LIST : LOOP_NONE);
	} else if (!strcmp(name, "Rate")) {
		/* fixed at 1.0 */
	} else
		goto bad;
	reply(m, msg, dbus_message_new_method_return(msg));
	return;
bad:
	reply(m, msg, dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS, "cannot set that"));
}

static DBusHandlerResult on_message(DBusConnection *conn, DBusMessage *msg, void *ud)
{
	struct mpris *m = ud;
	if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	const char *iface = dbus_message_get_interface(msg), *member = dbus_message_get_member(msg);
	const struct mpris_ops *o = &m->ops;
	if (!member)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	if (iface && !strcmp(iface, "org.freedesktop.DBus.Introspectable") && !strcmp(member, "Introspect")) {
		DBusMessage *r = dbus_message_new_method_return(msg);
		dbus_message_append_args(r, DBUS_TYPE_STRING, &introspect_xml, DBUS_TYPE_INVALID);
		reply(m, msg, r);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (iface && !strcmp(iface, IF_PROPS)) {
		if (!strcmp(member, "Get")) {
			const char *i, *n;
			if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &i, DBUS_TYPE_STRING, &n, DBUS_TYPE_INVALID)) {
				reply(m, msg, dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS, "expected ss"));
				return DBUS_HANDLER_RESULT_HANDLED;
			}
			DBusMessage *r = dbus_message_new_method_return(msg);
			DBusMessageIter it;
			dbus_message_iter_init_append(r, &it);
			if (!prop_value(m, &it, i, n)) {
				dbus_message_unref(r);
				reply(m, msg, dbus_message_new_error(msg, DBUS_ERROR_UNKNOWN_PROPERTY, "no such property"));
			} else
				reply(m, msg, r);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		if (!strcmp(member, "GetAll")) {
			const char *i;
			if (!dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &i, DBUS_TYPE_INVALID)) {
				reply(m, msg, dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS, "expected s"));
				return DBUS_HANDLER_RESULT_HANDLED;
			}
			DBusMessage *r = dbus_message_new_method_return(msg);
			DBusMessageIter it, dict;
			dbus_message_iter_init_append(r, &it);
			dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
			if (!strcmp(i, IF_ROOT))
				all_props(m, &dict, i, root_props);
			else if (!strcmp(i, IF_PLAYER))
				all_props(m, &dict, i, player_props);
			dbus_message_iter_close_container(&it, &dict);
			reply(m, msg, r);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		if (!strcmp(member, "Set")) {
			handle_set(m, msg);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	if (iface && !strcmp(iface, IF_ROOT)) {
		if (!strcmp(member, "Raise") && o->raise)
			o->raise(o->ud);
		else if (!strcmp(member, "Quit") && o->quit)
			o->quit(o->ud);
		else
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		reply(m, msg, dbus_message_new_method_return(msg));
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (iface && !strcmp(iface, IF_PLAYER)) {
		if (!strcmp(member, "Play"))
			o->play(o->ud);
		else if (!strcmp(member, "Pause"))
			o->pause(o->ud);
		else if (!strcmp(member, "PlayPause"))
			o->playpause(o->ud);
		else if (!strcmp(member, "Stop"))
			o->stop(o->ud);
		else if (!strcmp(member, "Next"))
			o->next(o->ud);
		else if (!strcmp(member, "Previous"))
			o->prev(o->ud);
		else if (!strcmp(member, "Seek")) {
			dbus_int64_t off = 0;
			if (dbus_message_get_args(msg, NULL, DBUS_TYPE_INT64, &off, DBUS_TYPE_INVALID))
				o->seek(o->ud, off);
		} else if (!strcmp(member, "SetPosition")) {
			const char *path;
			dbus_int64_t pos = 0;
			if (dbus_message_get_args(msg, NULL, DBUS_TYPE_OBJECT_PATH, &path, DBUS_TYPE_INT64, &pos,
			                          DBUS_TYPE_INVALID))
				o->set_position(o->ud, pos);
		} else if (!strcmp(member, "OpenUri")) {
			const char *uri;
			if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &uri, DBUS_TYPE_INVALID) && o->open_uri)
				o->open_uri(o->ud, uri);
		} else
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		reply(m, msg, dbus_message_new_method_return(msg));
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	(void)conn;
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void on_fd(void *ud, int fd, short rev)
{
	struct mpris *m = ud;
	(void)fd;
	if (rev & (POLLHUP | POLLERR)) {
		fl_fd_remove(m->app, m->fd);
		return;
	}
	dbus_connection_read_write(m->bus, 0);
	while (dbus_connection_dispatch(m->bus) == DBUS_DISPATCH_DATA_REMAINS)
		;
}

struct mpris *mpris_start(fl_app *app, const struct mpris_ops *ops)
{
	DBusError err;
	dbus_error_init(&err);
	DBusConnection *bus = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
	if (!bus) {
		dbus_error_free(&err);
		return NULL;
	}
	dbus_connection_set_exit_on_disconnect(bus, FALSE);
	int rc = dbus_bus_request_name(bus, BUS_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
	if (rc != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		dbus_error_free(&err);
		dbus_connection_close(bus);
		dbus_connection_unref(bus);
		return NULL;
	}
	struct mpris *m = calloc(1, sizeof *m);
	if (!m)
		return NULL;
	m->app = app;
	m->bus = bus;
	m->ops = *ops;
	static const DBusObjectPathVTable vt = {.message_function = on_message};
	dbus_connection_register_object_path(bus, OBJ_PATH, &vt, m);
	if (!dbus_connection_get_unix_fd(bus, &m->fd)) {
		free(m);
		return NULL;
	}
	fl_fd_add(app, m->fd, POLLIN, on_fd, m);
	return m;
}

void mpris_changed(struct mpris *m)
{
	if (!m)
		return;
	DBusMessage *s = dbus_message_new_signal(OBJ_PATH, IF_PROPS, "PropertiesChanged");
	DBusMessageIter it, dict, arr;
	dbus_message_iter_init_append(s, &it);
	const char *iface = IF_PLAYER;
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
	static const char *const changed[] = {"PlaybackStatus", "LoopStatus", "Shuffle", "Metadata", "Volume",
	                                      "CanGoNext", "CanGoPrevious", "CanPlay", NULL};
	all_props(m, &dict, IF_PLAYER, changed);
	dbus_message_iter_close_container(&it, &dict);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &arr);
	dbus_message_iter_close_container(&it, &arr);
	dbus_connection_send(m->bus, s, NULL);
	dbus_message_unref(s);
	dbus_connection_flush(m->bus);
}

void mpris_seeked(struct mpris *m, int64_t pos_us)
{
	if (!m)
		return;
	DBusMessage *s = dbus_message_new_signal(OBJ_PATH, IF_PLAYER, "Seeked");
	dbus_int64_t x = pos_us;
	dbus_message_append_args(s, DBUS_TYPE_INT64, &x, DBUS_TYPE_INVALID);
	dbus_connection_send(m->bus, s, NULL);
	dbus_message_unref(s);
	dbus_connection_flush(m->bus);
}

void mpris_stop(struct mpris *m)
{
	if (!m)
		return;
	fl_fd_remove(m->app, m->fd);
	dbus_connection_close(m->bus);
	dbus_connection_unref(m->bus);
	free(m);
}

/* ---- flmusic --ctl ------------------------------------------------------------------------- */

int mpris_ctl(const char *cmd)
{
	static const struct { const char *cmd, *method; } map[] = {
		{"play", "Play"}, {"pause", "Pause"}, {"play-pause", "PlayPause"}, {"toggle", "PlayPause"},
		{"stop", "Stop"}, {"next", "Next"}, {"previous", "Previous"}, {"prev", "Previous"},
	};
	const char *method = NULL;
	int status = !strcmp(cmd, "status");
	for (size_t i = 0; i < sizeof map / sizeof *map; i++)
		if (!strcmp(cmd, map[i].cmd))
			method = map[i].method;
	if (!method && !status) {
		fprintf(stderr, "flmusic --ctl: unknown command %s (play, pause, play-pause, stop, next, previous, status)\n", cmd);
		return 2;
	}
	DBusError err;
	dbus_error_init(&err);
	DBusConnection *bus = dbus_bus_get(DBUS_BUS_SESSION, &err);
	if (!bus) {
		fprintf(stderr, "flmusic --ctl: no session bus: %s\n", err.message);
		return 1;
	}
	DBusMessage *q = dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus",
	                                              "org.freedesktop.DBus", "ListNames");
	DBusMessage *r = dbus_connection_send_with_reply_and_block(bus, q, 2000, &err);
	dbus_message_unref(q);
	if (!r) {
		fprintf(stderr, "flmusic --ctl: %s\n", err.message);
		return 1;
	}
	char target[256] = "";
	DBusMessageIter it, a;
	dbus_message_iter_init(r, &it);
	dbus_message_iter_recurse(&it, &a);
	while (dbus_message_iter_get_arg_type(&a) == DBUS_TYPE_STRING) {
		const char *n;
		dbus_message_iter_get_basic(&a, &n);
		if (!strncmp(n, "org.mpris.MediaPlayer2.", 23) && !target[0])
			snprintf(target, sizeof target, "%s", n);
		dbus_message_iter_next(&a);
	}
	dbus_message_unref(r);
	if (!target[0]) {
		fprintf(stderr, "flmusic --ctl: no media player is running\n");
		return 1;
	}
	if (status) {
		q = dbus_message_new_method_call(target, OBJ_PATH, IF_PROPS, "Get");
		const char *i = IF_PLAYER, *n = "PlaybackStatus";
		dbus_message_append_args(q, DBUS_TYPE_STRING, &i, DBUS_TYPE_STRING, &n, DBUS_TYPE_INVALID);
		r = dbus_connection_send_with_reply_and_block(bus, q, 2000, &err);
		dbus_message_unref(q);
		if (!r) {
			fprintf(stderr, "flmusic --ctl: %s\n", err.message);
			return 1;
		}
		DBusMessageIter ri, v;
		dbus_message_iter_init(r, &ri);
		dbus_message_iter_recurse(&ri, &v);
		const char *s = "";
		dbus_message_iter_get_basic(&v, &s);
		printf("%s %s\n", target, s);
		dbus_message_unref(r);
		return 0;
	}
	q = dbus_message_new_method_call(target, OBJ_PATH, IF_PLAYER, method);
	r = dbus_connection_send_with_reply_and_block(bus, q, 2000, &err);
	dbus_message_unref(q);
	if (!r) {
		fprintf(stderr, "flmusic --ctl: %s\n", err.message);
		return 1;
	}
	dbus_message_unref(r);
	return 0;
}
