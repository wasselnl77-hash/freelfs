/* flnotify -- send a desktop notification from a script, like notify-send
 * SPDX-License-Identifier: MIT
 *
 *   flnotify [options] SUMMARY [BODY]
 *     -a, --app NAME         the sender's name shown on the card (default: flnotify)
 *     -i, --icon NAME        the sender's icon name (an app id such as flterm shows its icon)
 *     -u, --urgency LEVEL    low, normal (default) or critical (stays until dismissed)
 *     -t, --expire MS        how long the card stays; 0 never, default: by urgency
 *     -A, --action KEY=LABEL a button (up to three; the key "default" is the click on the card)
 *     -r, --replace ID       replace the notification with this id
 *     -T, --transient        do not keep it in the history
 *     -w, --wait             wait until it is closed or an action is picked, print what happened:
 *                            "action KEY" or "closed REASON" (1 expired, 2 dismissed, 3 withdrawn)
 *   flnotify --dnd on|off|toggle|status    Do not disturb
 *   flnotify --clear                       empty the history
 *   flnotify --center                      open or close the notification centre
 *   flnotify --count                       entries in the history
 * Prints the id of the new notification. Exit 0; 1 on errors (no session bus, no daemon);
 * 2 for bad usage.
 */
#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NAME "org.freedesktop.Notifications"
#define PATH "/org/freedesktop/Notifications"
#define IFACE "org.freedesktop.Notifications"
#define FL_IFACE "org.freelfs.Notifications"

static DBusConnection *bus;

static DBusMessage *call(DBusMessage *m, int ms)
{
	DBusError err;
	dbus_error_init(&err);
	DBusMessage *r = dbus_connection_send_with_reply_and_block(bus, m, ms, &err);
	dbus_message_unref(m);
	if (!r) {
		fprintf(stderr, "flnotify: %s\n", dbus_error_is_set(&err) ? err.message : "no answer");
		dbus_error_free(&err);
	}
	return r;
}

static DBusMessage *fl_method(const char *name)
{
	return dbus_message_new_method_call(NAME, PATH, FL_IFACE, name);
}

static void usage(void)
{
	fputs("usage: flnotify [-a APP] [-i ICON] [-u low|normal|critical] [-t MS] [-A KEY=LABEL]...\n"
	      "                [-r ID] [-T] [-w] SUMMARY [BODY]\n"
	      "       flnotify --dnd on|off|toggle|status | --clear | --center | --count\n",
	      stderr);
}

int main(int argc, char **argv)
{
	const char *app = "flnotify", *icon = "", *dnd = NULL;
	int urgency = 1, expire = -1, wait = 0, transient = 0, clear = 0, center = 0, count = 0;
	dbus_uint32_t replace = 0;
	const char *act_key[3], *act_label[3];
	int nact = 0;
	const char *pos[2];
	int npos = 0;
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
#define ARG(s, l) ((!strcmp(a, s) || !strcmp(a, l)) && i + 1 < argc)
		if (ARG("-a", "--app"))
			app = argv[++i];
		else if (ARG("-i", "--icon"))
			icon = argv[++i];
		else if (ARG("-u", "--urgency")) {
			const char *u = argv[++i];
			urgency = !strcmp(u, "low") ? 0 : !strcmp(u, "critical") ? 2 : !strcmp(u, "normal") ? 1 : -1;
			if (urgency < 0) {
				usage();
				return 2;
			}
		} else if (ARG("-t", "--expire"))
			expire = atoi(argv[++i]);
		else if (ARG("-r", "--replace"))
			replace = (dbus_uint32_t)strtoul(argv[++i], NULL, 10);
		else if (ARG("-A", "--action")) {
			char *kv = strdup(argv[++i]);
			char *eq = kv ? strchr(kv, '=') : NULL;
			if (!eq || nact >= 3) {
				usage();
				return 2;
			}
			*eq = 0;
			act_key[nact] = kv;
			act_label[nact] = eq + 1;
			nact++;
		} else if (!strcmp(a, "-T") || !strcmp(a, "--transient"))
			transient = 1;
		else if (!strcmp(a, "-w") || !strcmp(a, "--wait"))
			wait = 1;
		else if (ARG("--dnd", "--dnd"))
			dnd = argv[++i];
		else if (!strcmp(a, "--clear"))
			clear = 1;
		else if (!strcmp(a, "--center"))
			center = 1;
		else if (!strcmp(a, "--count"))
			count = 1;
		else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage();
			return 0;
		} else if (a[0] != '-' && npos < 2)
			pos[npos++] = a;
		else {
			usage();
			return 2;
		}
	}
	int control = dnd || clear || center || count;
	if (!control && npos < 1) {
		usage();
		return 2;
	}
	DBusError err;
	dbus_error_init(&err);
	bus = dbus_bus_get(DBUS_BUS_SESSION, &err);
	if (!bus) {
		fprintf(stderr, "flnotify: no session bus (%s)\n", dbus_error_is_set(&err) ? err.message : "?");
		return 1;
	}
	dbus_connection_set_exit_on_disconnect(bus, FALSE);

	if (control) {
		DBusMessage *r;
		if (dnd) {
			if (!strcmp(dnd, "status") || !strcmp(dnd, "toggle")) {
				r = call(fl_method("GetDoNotDisturb"), 3000);
				if (!r)
					return 1;
				dbus_bool_t b = 0;
				dbus_message_get_args(r, NULL, DBUS_TYPE_BOOLEAN, &b, DBUS_TYPE_INVALID);
				dbus_message_unref(r);
				if (!strcmp(dnd, "status")) {
					puts(b ? "on" : "off");
					return 0;
				}
				dnd = b ? "off" : "on";
			}
			if (strcmp(dnd, "on") && strcmp(dnd, "off")) {
				usage();
				return 2;
			}
			dbus_bool_t v = !strcmp(dnd, "on");
			DBusMessage *m = fl_method("SetDoNotDisturb");
			dbus_message_append_args(m, DBUS_TYPE_BOOLEAN, &v, DBUS_TYPE_INVALID);
			r = call(m, 3000);
			if (!r)
				return 1;
			dbus_message_unref(r);
		}
		if (clear) {
			r = call(fl_method("ClearAll"), 3000);
			if (!r)
				return 1;
			dbus_message_unref(r);
		}
		if (center) {
			r = call(fl_method("ToggleCenter"), 3000);
			if (!r)
				return 1;
			dbus_message_unref(r);
		}
		if (count) {
			r = call(fl_method("GetCount"), 3000);
			if (!r)
				return 1;
			dbus_uint32_t n = 0;
			dbus_message_get_args(r, NULL, DBUS_TYPE_UINT32, &n, DBUS_TYPE_INVALID);
			dbus_message_unref(r);
			printf("%u\n", n);
		}
		return 0;
	}

	if (wait) { /* listen before sending, or a quick answer could slip by */
		dbus_bus_add_match(bus, "type='signal',interface='" IFACE "'", &err);
		dbus_error_free(&err);
	}
	DBusMessage *m = dbus_message_new_method_call(NAME, PATH, IFACE, "Notify");
	DBusMessageIter it, arr, dict, e, v;
	const char *summary = pos[0], *body = npos > 1 ? pos[1] : "";
	dbus_message_iter_init_append(m, &it);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &app);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_UINT32, &replace);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &icon);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &summary);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &body);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &arr);
	for (int i = 0; i < nact; i++) {
		dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &act_key[i]);
		dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &act_label[i]);
	}
	dbus_message_iter_close_container(&it, &arr);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &dict);
	{
		const char *k = "urgency";
		unsigned char u = (unsigned char)urgency;
		dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &e);
		dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k);
		dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "y", &v);
		dbus_message_iter_append_basic(&v, DBUS_TYPE_BYTE, &u);
		dbus_message_iter_close_container(&e, &v);
		dbus_message_iter_close_container(&dict, &e);
	}
	if (transient) {
		const char *k = "transient";
		dbus_bool_t t = 1;
		dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &e);
		dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k);
		dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "b", &v);
		dbus_message_iter_append_basic(&v, DBUS_TYPE_BOOLEAN, &t);
		dbus_message_iter_close_container(&e, &v);
		dbus_message_iter_close_container(&dict, &e);
	}
	dbus_message_iter_close_container(&it, &dict);
	dbus_int32_t to = expire;
	dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &to);
	DBusMessage *r = call(m, 5000);
	if (!r)
		return 1;
	dbus_uint32_t id = 0;
	dbus_message_get_args(r, NULL, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID);
	dbus_message_unref(r);
	printf("%u\n", id);
	fflush(stdout);
	if (!wait)
		return 0;
	time_t end = time(NULL) + (expire > 0 ? expire / 1000 + 5 : 130);
	while (time(NULL) < end) {
		dbus_connection_read_write(bus, 200);
		DBusMessage *s;
		while ((s = dbus_connection_pop_message(bus))) {
			dbus_uint32_t sid = 0;
			if (dbus_message_is_signal(s, IFACE, "NotificationClosed")) {
				dbus_uint32_t reason = 0;
				dbus_message_get_args(s, NULL, DBUS_TYPE_UINT32, &sid, DBUS_TYPE_UINT32, &reason, DBUS_TYPE_INVALID);
				if (sid == id) {
					printf("closed %u\n", reason);
					return 0;
				}
			} else if (dbus_message_is_signal(s, IFACE, "ActionInvoked")) {
				const char *key = "";
				dbus_message_get_args(s, NULL, DBUS_TYPE_UINT32, &sid, DBUS_TYPE_STRING, &key, DBUS_TYPE_INVALID);
				if (sid == id) {
					printf("action %s\n", key);
					fflush(stdout);
				}
			}
			dbus_message_unref(s);
		}
	}
	return 1;
}
