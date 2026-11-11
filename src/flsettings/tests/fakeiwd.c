/* fakeiwd -- a stand-in for iwd's D-Bus API, for the headless flsettings test
 * SPDX-License-Identifier: MIT
 *
 * Owns net.connman.iwd on whatever bus $DBUS_SYSTEM_BUS_ADDRESS names: one
 * station (wlan0), four networks, an AgentManager, and the calls flsettings
 * uses (Scan, GetOrderedNetworks, Connect, Disconnect, Forget, Powered).
 * Connecting to an unknown WPA network asks the registered agent for the
 * passphrase, like iwd does; "correct-horse" is the right one. Actions go to
 * the log file named by argv[1], one line each; the passphrase itself is never
 * written, only whether it was right.
 *   fakeiwd LOGFILE [nohw]     ("nohw": no device at all)
 */
#include <dbus/dbus.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IWD "net.connman.iwd"
#define DEV "/net/connman/iwd/0/3"
#define KN "/net/connman/iwd/"

static FILE *logf;
static int nohw, powered = 1, scanning;
static char state[16] = "disconnected", agent_sender[64] = "", agent_path[128] = "";
static const char *connected = NULL; /* path of the connected network */

struct net { const char *name, *type, *path; int known, sig; };
static struct net nets[] = {
	{"HomeNet", "psk", DEV "/486f6d654e6574_psk", 1, -4800},
	{"CafeWifi", "open", DEV "/43616665576966_open", 0, -6200},
	{"Neighbor", "psk", DEV "/4e65696768626f72_psk", 0, -7400},
	{"Corp", "8021x", DEV "/436f7270_8021x", 0, -8000},
};
#define NNET ((int)(sizeof nets / sizeof nets[0]))

static void flog(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(logf, fmt, ap);
	va_end(ap);
	fputc('\n', logf);
	fflush(logf);
}

static void sprop(DBusMessageIter *props, const char *k, int type, const char *v)
{
	DBusMessageIter e, var;
	dbus_message_iter_open_container(props, DBUS_TYPE_DICT_ENTRY, NULL, &e);
	dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k);
	dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, type == DBUS_TYPE_STRING ? "s" : "o", &var);
	dbus_message_iter_append_basic(&var, type, &v);
	dbus_message_iter_close_container(&e, &var);
	dbus_message_iter_close_container(props, &e);
}

static void bprop(DBusMessageIter *props, const char *k, int b)
{
	DBusMessageIter e, var;
	dbus_bool_t v = b;
	dbus_message_iter_open_container(props, DBUS_TYPE_DICT_ENTRY, NULL, &e);
	dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k);
	dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "b", &var);
	dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &v);
	dbus_message_iter_close_container(&e, &var);
	dbus_message_iter_close_container(props, &e);
}

static void open_iface(DBusMessageIter *ifs, DBusMessageIter *e, DBusMessageIter *props, const char *name)
{
	dbus_message_iter_open_container(ifs, DBUS_TYPE_DICT_ENTRY, NULL, e);
	dbus_message_iter_append_basic(e, DBUS_TYPE_STRING, &name);
	dbus_message_iter_open_container(e, DBUS_TYPE_ARRAY, "{sv}", props);
}

static void close_iface(DBusMessageIter *ifs, DBusMessageIter *e, DBusMessageIter *props)
{
	dbus_message_iter_close_container(e, props);
	dbus_message_iter_close_container(ifs, e);
}

static void managed(DBusMessage *rep)
{
	DBusMessageIter it, arr, oe, ifs, ie, props;
	dbus_message_iter_init_append(rep, &it);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{oa{sa{sv}}}", &arr);
	if (!nohw) {
		const char *p = DEV;
		dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, NULL, &oe);
		dbus_message_iter_append_basic(&oe, DBUS_TYPE_OBJECT_PATH, &p);
		dbus_message_iter_open_container(&oe, DBUS_TYPE_ARRAY, "{sa{sv}}", &ifs);
		open_iface(&ifs, &ie, &props, IWD ".Device");
		sprop(&props, "Name", DBUS_TYPE_STRING, "wlan0");
		bprop(&props, "Powered", powered);
		close_iface(&ifs, &ie, &props);
		if (powered) {
			open_iface(&ifs, &ie, &props, IWD ".Station");
			sprop(&props, "State", DBUS_TYPE_STRING, state);
			bprop(&props, "Scanning", scanning);
			if (connected)
				sprop(&props, "ConnectedNetwork", DBUS_TYPE_OBJECT_PATH, connected);
			close_iface(&ifs, &ie, &props);
		}
		dbus_message_iter_close_container(&oe, &ifs);
		dbus_message_iter_close_container(&arr, &oe);
		for (int i = 0; powered && i < NNET; i++) {
			struct net *n = &nets[i];
			char known[160];
			snprintf(known, sizeof known, KN "%s", strrchr(n->path, '/') + 1);
			dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, NULL, &oe);
			dbus_message_iter_append_basic(&oe, DBUS_TYPE_OBJECT_PATH, &n->path);
			dbus_message_iter_open_container(&oe, DBUS_TYPE_ARRAY, "{sa{sv}}", &ifs);
			open_iface(&ifs, &ie, &props, IWD ".Network");
			sprop(&props, "Name", DBUS_TYPE_STRING, n->name);
			sprop(&props, "Type", DBUS_TYPE_STRING, n->type);
			sprop(&props, "Device", DBUS_TYPE_OBJECT_PATH, DEV);
			bprop(&props, "Connected", connected && !strcmp(connected, n->path));
			if (n->known) {
				const char *k = known;
				sprop(&props, "KnownNetwork", DBUS_TYPE_OBJECT_PATH, k);
			}
			close_iface(&ifs, &ie, &props);
			dbus_message_iter_close_container(&oe, &ifs);
			dbus_message_iter_close_container(&arr, &oe);
		}
	}
	dbus_message_iter_close_container(&it, &arr);
}

static void ordered(DBusMessage *rep)
{
	DBusMessageIter it, arr, st;
	dbus_message_iter_init_append(rep, &it);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "(on)", &arr);
	for (int i = 0; i < NNET; i++) {
		int16_t sg = (int16_t)nets[i].sig;
		dbus_message_iter_open_container(&arr, DBUS_TYPE_STRUCT, NULL, &st);
		dbus_message_iter_append_basic(&st, DBUS_TYPE_OBJECT_PATH, &nets[i].path);
		dbus_message_iter_append_basic(&st, DBUS_TYPE_INT16, &sg);
		dbus_message_iter_close_container(&arr, &st);
	}
	dbus_message_iter_close_container(&it, &arr);
}

static void changed(DBusConnection *c)
{
	DBusMessage *s = dbus_message_new_signal(DEV, "org.freedesktop.DBus.Properties", "PropertiesChanged");
	dbus_connection_send(c, s, NULL);
	dbus_message_unref(s);
}

static void send_ok(DBusConnection *c, DBusMessage *m)
{
	DBusMessage *r = dbus_message_new_method_return(m);
	dbus_connection_send(c, r, NULL);
	dbus_message_unref(r);
}

static void send_err(DBusConnection *c, DBusMessage *m, const char *name)
{
	DBusMessage *r = dbus_message_new_error(m, name, "failed");
	dbus_connection_send(c, r, NULL);
	dbus_message_unref(r);
}

static struct net *by_path(const char *p)
{
	for (int i = 0; i < NNET; i++)
		if (!strcmp(nets[i].path, p))
			return &nets[i];
	return NULL;
}

static void do_connect(DBusConnection *c, DBusMessage *m)
{
	struct net *n = by_path(dbus_message_get_path(m));
	if (!n)
		return send_err(c, m, "net.connman.iwd.Failed");
	flog("connect %s", n->name);
	if (!strcmp(n->type, "psk") && !n->known) {
		if (!agent_sender[0])
			return send_err(c, m, "net.connman.iwd.NoAgent");
		DBusMessage *q = dbus_message_new_method_call(agent_sender, agent_path, IWD ".Agent", "RequestPassphrase");
		const char *p = n->path;
		dbus_message_append_args(q, DBUS_TYPE_OBJECT_PATH, &p, DBUS_TYPE_INVALID);
		DBusError e;
		dbus_error_init(&e);
		DBusMessage *a = dbus_connection_send_with_reply_and_block(c, q, 60000, &e);
		dbus_message_unref(q);
		const char *pw = NULL;
		if (!a || !dbus_message_get_args(a, NULL, DBUS_TYPE_STRING, &pw, DBUS_TYPE_INVALID)) {
			flog("agent: %s", e.name ? e.name : "no answer");
			if (a)
				dbus_message_unref(a);
			return send_err(c, m, "net.connman.iwd.Aborted");
		}
		int good = !strcmp(pw, "correct-horse");
		flog("passphrase length %d: %s", (int)strlen(pw), good ? "accepted" : "rejected");
		dbus_message_unref(a);
		if (!good)
			return send_err(c, m, "net.connman.iwd.Failed");
		n->known = 1;
	}
	connected = n->path;
	strcpy(state, "connected");
	send_ok(c, m);
	changed(c);
}

static DBusHandlerResult handle(DBusConnection *c, DBusMessage *m, void *ud __attribute__((unused)))
{
	if (dbus_message_get_type(m) != DBUS_MESSAGE_TYPE_METHOD_CALL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	const char *i = dbus_message_get_interface(m), *n = dbus_message_get_member(m), *p = dbus_message_get_path(m);
	if (!i || !n)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	if (!strcmp(n, "GetManagedObjects")) {
		DBusMessage *r = dbus_message_new_method_return(m);
		managed(r);
		dbus_connection_send(c, r, NULL);
		dbus_message_unref(r);
	} else if (!strcmp(n, "GetOrderedNetworks")) {
		DBusMessage *r = dbus_message_new_method_return(m);
		ordered(r);
		dbus_connection_send(c, r, NULL);
		dbus_message_unref(r);
	} else if (!strcmp(n, "RegisterAgent")) {
		const char *ap = "";
		dbus_message_get_args(m, NULL, DBUS_TYPE_OBJECT_PATH, &ap, DBUS_TYPE_INVALID);
		snprintf(agent_sender, sizeof agent_sender, "%s", dbus_message_get_sender(m));
		snprintf(agent_path, sizeof agent_path, "%s", ap);
		flog("register agent");
		send_ok(c, m);
	} else if (!strcmp(n, "UnregisterAgent")) {
		agent_sender[0] = 0;
		flog("unregister agent");
		send_ok(c, m);
	} else if (!strcmp(n, "Scan")) {
		flog("scan");
		send_ok(c, m);
		changed(c);
	} else if (!strcmp(n, "Disconnect")) {
		flog("disconnect");
		connected = NULL;
		strcpy(state, "disconnected");
		send_ok(c, m);
		changed(c);
	} else if (!strcmp(n, "Connect")) {
		do_connect(c, m);
	} else if (!strcmp(n, "Forget")) {
		for (int k = 0; k < NNET; k++)
			if (strstr(p, strrchr(nets[k].path, '/') + 1)) {
				flog("forget %s", nets[k].name);
				nets[k].known = 0;
				if (connected && !strcmp(connected, nets[k].path)) {
					connected = NULL;
					strcpy(state, "disconnected");
				}
			}
		send_ok(c, m);
		changed(c);
	} else if (!strcmp(n, "Set")) {
		DBusMessageIter it, var;
		const char *ifc, *prop;
		dbus_bool_t b = 1;
		dbus_message_iter_init(m, &it);
		dbus_message_iter_get_basic(&it, &ifc);
		dbus_message_iter_next(&it);
		dbus_message_iter_get_basic(&it, &prop);
		dbus_message_iter_next(&it);
		dbus_message_iter_recurse(&it, &var);
		dbus_message_iter_get_basic(&var, &b);
		powered = b;
		if (!b) {
			connected = NULL;
			strcpy(state, "disconnected");
		}
		flog("power %s", b ? "on" : "off");
		send_ok(c, m);
		changed(c);
	} else {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	return DBUS_HANDLER_RESULT_HANDLED;
}

int main(int argc, char **argv)
{
	if (argc < 2)
		return 2;
	nohw = argc > 2 && !strcmp(argv[2], "nohw");
	logf = fopen(argv[1], "a");
	DBusError e;
	dbus_error_init(&e);
	DBusConnection *c = dbus_bus_get(DBUS_BUS_SYSTEM, &e);
	if (!c || dbus_bus_request_name(c, IWD, 0, &e) != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		fprintf(stderr, "fakeiwd: %s\n", e.message ? e.message : "no bus");
		return 1;
	}
	dbus_connection_add_filter(c, handle, NULL, NULL);
	flog("up");
	while (dbus_connection_read_write_dispatch(c, -1))
		;
	return 0;
}
