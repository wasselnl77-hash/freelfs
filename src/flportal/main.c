/* flportal -- the FreeLFS backend of xdg-desktop-portal
 * SPDX-License-Identifier: MIT
 *
 * xdg-desktop-portal (the frontend, D-Bus activated in the session) answers sandboxed apps
 * and asks a backend for everything the desktop itself has to show or know. This is that
 * backend: org.freedesktop.impl.portal.desktop.freelfs on the session bus, started by D-Bus
 * activation on the first call.
 *
 *   FileChooser   filechooser.c   the file dialog (open, save, save several, folders)
 *   AppChooser    appchooser.c    "Open with" list for OpenURI
 *   Access        appchooser.c    the Allow / Deny question (screenshots ask it)
 *   Notification  here            forwarded to org.freedesktop.Notifications (flpanel)
 *   Settings      here            color-scheme and accent-color from desktop.conf
 *   Screenshot    here            runs snappel, when it is installed
 *
 * One process, one main loop: GLib (D-Bus) and libflui (the windows) are polled together
 * (fl_app_prepare / fl_app_dispatch). The Wayland connection is made when the first dialog
 * opens and dropped when the last one is closed, so an idle backend holds no window state.
 *
 * Test hooks (see tests/portal.sh): FLPORTAL_SHOT=PREFIX renders every dialog to
 * PREFIX-N.ppm with no compositor and answers it at once (cancelled, or with the selection
 * when FLPORTAL_TEST_ACCEPT is set).
 */
#include "flportal.h"

#include <errno.h>
#include <glib-unix.h>
#include <gio/gdesktopappinfo.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define IMPL_NAME "org.freedesktop.impl.portal.desktop.freelfs"
#define IMPL_PATH "/org/freedesktop/portal/desktop"

/* ---- state -------------------------------------------------------------------------- */

struct req {
	struct req *next;
	GDBusMethodInvocation *inv;
	char *handle;
	guint reg;
	struct dlg *dlg;
	GSubprocess *proc;
	char *shot;
};

static GDBusConnection *conn;
static fl_app *app;
static struct dlg *dlgs;
static struct req *reqs;
static int quit;
static int shot_n;
static char shot_path[640];
static gboolean verbose;

void portal_say(const char *fmt, ...)
{
	if (!verbose)
		return;
	va_list ap;
	va_start(ap, fmt);
	fputs("flportal: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

fl_app *portal_app_get(void) { return app; }

int portal_test_mode(void)
{
	const char *s = getenv("FLPORTAL_SHOT");
	return s && *s;
}

/* ---- requests ------------------------------------------------------------------------ */

static const char request_xml[] =
	"<node><interface name='org.freedesktop.impl.portal.Request'>"
	"<method name='Close'/></interface></node>";
static GDBusNodeInfo *request_info;

static void req_free(struct req *r)
{
	for (struct req **p = &reqs; *p; p = &(*p)->next)
		if (*p == r) {
			*p = r->next;
			break;
		}
	if (r->reg)
		g_dbus_connection_unregister_object(conn, r->reg);
	if (r->dlg) {
		r->dlg->dead = 1;
		r->dlg->req = NULL;
	}
	if (r->proc) {
		g_subprocess_force_exit(r->proc);
		g_object_unref(r->proc);
	}
	g_free(r->handle);
	g_free(r->shot);
	g_free(r);
}

void portal_reply(struct req *r, unsigned response, GVariant *results)
{
	if (!results)
		results = g_variant_new("a{sv}", NULL);
	if (r->inv) {
		portal_say("reply %s: %u", r->handle, response);
		g_dbus_method_invocation_return_value(r->inv, g_variant_new("(u@a{sv})", response, results));
		r->inv = NULL;
	} else {
		g_variant_ref_sink(results);
		g_variant_unref(results);
	}
	req_free(r);
}

static void req_close(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                      const char *method, GVariant *params, GDBusMethodInvocation *inv, gpointer ud)
{
	struct req *r = ud;
	g_dbus_method_invocation_return_value(inv, NULL);
	portal_say("close %s", r->handle);
	portal_reply(r, 2, NULL);
}

static const GDBusInterfaceVTable request_vtable = { .method_call = req_close };

/* A request for a call; NULL (and the call answered with an error) when the handle is unusable. */
static struct req *req_new(GDBusMethodInvocation *inv, const char *handle)
{
	GError *err = NULL;
	struct req *r = g_new0(struct req, 1);
	r->handle = g_strdup(handle);
	r->inv = inv;
	r->reg = g_dbus_connection_register_object(conn, handle, request_info->interfaces[0],
	                                           &request_vtable, r, NULL, &err);
	if (!r->reg) {
		g_dbus_method_invocation_return_gerror(inv, err);
		g_clear_error(&err);
		g_free(r->handle);
		g_free(r);
		return NULL;
	}
	r->next = reqs;
	reqs = r;
	return r;
}

struct req *portal_req_find(const char *handle)
{
	for (struct req *r = reqs; r; r = r->next)
		if (!strcmp(r->handle, handle))
			return r;
	return NULL;
}

struct dlg *portal_req_dlg(struct req *r) { return r->dlg; }

/* ---- windows -------------------------------------------------------------------------- */

static fl_app *ensure_app(void)
{
	if (app)
		return app;
	if (portal_test_mode()) {
		snprintf(shot_path, sizeof shot_path, "%s-%d.ppm", getenv("FLPORTAL_SHOT"), ++shot_n);
		setenv("FLUI_PPM", shot_path, 1);
	}
	app = fl_app_new(NULL, NULL);
	if (app && !fl_app_ppm(app))
		fl_app_watch_conf(app, NULL, NULL);
	return app;
}

static void reap(void)
{
	for (struct dlg **p = &dlgs; *p;) {
		struct dlg *d = *p;
		if (!d->dead) {
			p = &d->next;
			continue;
		}
		*p = d->next;
		if (d->win)
			fl_win_destroy(d->win);
		d->destroy(d);
	}
	if (app && !dlgs) {
		fl_app_free(app);
		app = NULL;
	}
}

static void win_closed(fl_win *w, void *ud)
{
	struct dlg *d = ud;
	if (d->req)
		portal_reply(d->req, 1, NULL);
	d->dead = 1;
}

int portal_dialog_open(struct req *r, struct dlg *d, const fl_win_opts *o)
{
	fl_app *a = ensure_app();
	fl_win_opts oo = *o;
	oo.ud = d;
	oo.close = win_closed;
	if (a)
		d->win = fl_win_new(a, &oo);
	if (!a || !d->win) {
		fprintf(stderr, "flportal: cannot open a window (no compositor?)\n");
		portal_reply(r, 2, NULL);
		d->destroy(d);
		reap();
		return -1;
	}
	d->req = r;
	r->dlg = d;
	d->next = dlgs;
	dlgs = d;
	if (fl_app_ppm(a)) {
		/* headless test: one frame goes to the PPM file, then the dialog is answered */
		fl_app_run(a);
		if (d->test && d->req)
			d->test(d, getenv("FLPORTAL_TEST_ACCEPT") != NULL);
		if (d->req)
			portal_reply(d->req, 1, NULL);
		d->dead = 1;
		reap();
	}
	return 0;
}

/* ---- Settings: appearance from desktop.conf ------------------------------------------ */

static unsigned cur_scheme = 1;
static double cur_accent[3] = { 0.247, 0.608, 0.388 };

static void settings_compute(void)
{
	fl_conf_load();
	cur_scheme = fl_th.dark ? 1 : 2; /* 1 prefer dark, 2 prefer light */
	fl_color a = fl_accent_current(fl_th.dark);
	cur_accent[0] = ((a >> 16) & 255) / 255.0;
	cur_accent[1] = ((a >> 8) & 255) / 255.0;
	cur_accent[2] = (a & 255) / 255.0;
}

static GVariant *setting_get(const char *ns, const char *key)
{
	if (strcmp(ns, "org.freedesktop.appearance"))
		return NULL;
	if (!strcmp(key, "color-scheme"))
		return g_variant_new_uint32(cur_scheme);
	if (!strcmp(key, "accent-color"))
		return g_variant_new("(ddd)", cur_accent[0], cur_accent[1], cur_accent[2]);
	if (!strcmp(key, "contrast"))
		return g_variant_new_uint32(0);
	return NULL;
}

static const char *const appearance_keys[] = { "color-scheme", "accent-color", "contrast", NULL };

static gboolean ns_match(const char *pattern, const char *ns)
{
	size_t n = strlen(pattern);
	if (n == 0 || !strcmp(pattern, "*"))
		return TRUE;
	if (pattern[n - 1] == '*')
		return !strncmp(ns, pattern, n - 1);
	return !strcmp(ns, pattern);
}

static GVariant *settings_read_all(const char *const *patterns)
{
	GVariantBuilder out;
	g_variant_builder_init(&out, G_VARIANT_TYPE("a{sa{sv}}"));
	const char *ns = "org.freedesktop.appearance";
	gboolean want = !patterns || !patterns[0];
	for (int i = 0; patterns && patterns[i] && !want; i++)
		want = ns_match(patterns[i], ns);
	if (want) {
		GVariantBuilder in;
		g_variant_builder_init(&in, G_VARIANT_TYPE("a{sv}"));
		for (int i = 0; appearance_keys[i]; i++)
			g_variant_builder_add(&in, "{sv}", appearance_keys[i], setting_get(ns, appearance_keys[i]));
		g_variant_builder_add(&out, "{sa{sv}}", ns, &in);
	}
	return g_variant_builder_end(&out);
}

static void settings_emit(const char *key, GVariant *v)
{
	g_dbus_connection_emit_signal(conn, NULL, IMPL_PATH, "org.freedesktop.impl.portal.Settings",
	                              "SettingChanged",
	                              g_variant_new("(ssv)", "org.freedesktop.appearance", key, v), NULL);
}

/* desktop.conf changed: tell the frontend (which tells the apps) what is different */
static void conf_changed(GFileMonitor *m, GFile *f, GFile *other, GFileMonitorEvent ev, gpointer ud)
{
	if (ev != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT && ev != G_FILE_MONITOR_EVENT_CREATED &&
	    ev != G_FILE_MONITOR_EVENT_CHANGED && ev != G_FILE_MONITOR_EVENT_RENAMED &&
	    ev != G_FILE_MONITOR_EVENT_MOVED_IN)
		return;
	unsigned old_scheme = cur_scheme;
	double old_acc[3] = { cur_accent[0], cur_accent[1], cur_accent[2] };
	settings_compute();
	if (cur_scheme != old_scheme) {
		portal_say("color-scheme -> %u", cur_scheme);
		settings_emit("color-scheme", g_variant_new_uint32(cur_scheme));
	}
	if (memcmp(old_acc, cur_accent, sizeof old_acc)) {
		portal_say("accent-color changed");
		settings_emit("accent-color", setting_get("org.freedesktop.appearance", "accent-color"));
	}
}

/* ---- Notification: onto org.freedesktop.Notifications --------------------------------- */

struct note {
	char *app_id, *id;
	guint32 nid;
	char *default_action;
	GVariant *default_target;
	GPtrArray *actions; /* button action names */
	GPtrArray *targets; /* their targets (GVariant or NULL) */
};

static GHashTable *notes; /* "app_id\x1fid" -> struct note */

static void note_free(gpointer p)
{
	struct note *n = p;
	g_free(n->app_id);
	g_free(n->id);
	g_free(n->default_action);
	if (n->default_target)
		g_variant_unref(n->default_target);
	g_ptr_array_free(n->actions, TRUE);
	g_ptr_array_free(n->targets, TRUE);
	g_free(n);
}

static void target_unref(gpointer p)
{
	if (p)
		g_variant_unref(p);
}

static char *note_key(const char *app_id, const char *id) { return g_strdup_printf("%s\x1f%s", app_id, id); }

/* name of the icon in a notification: a themed name, a file path, or "" */
static char *note_icon(GVariant *icon)
{
	if (!icon)
		return g_strdup("");
	GIcon *gi = g_icon_deserialize(icon);
	char *out = NULL;
	if (gi) {
		if (G_IS_THEMED_ICON(gi)) {
			const char *const *names = g_themed_icon_get_names(G_THEMED_ICON(gi));
			if (names && names[0])
				out = g_strdup(names[0]);
		} else if (G_IS_FILE_ICON(gi)) {
			GFile *f = g_file_icon_get_file(G_FILE_ICON(gi));
			out = g_file_get_path(f);
		}
		g_object_unref(gi);
	}
	return out ? out : g_strdup("");
}

static char *app_name_of(const char *app_id)
{
	if (app_id && *app_id) {
		char *did = g_strconcat(app_id, ".desktop", NULL);
		GDesktopAppInfo *ai = g_desktop_app_info_new(did);
		g_free(did);
		if (ai) {
			char *n = g_strdup(g_app_info_get_name(G_APP_INFO(ai)));
			g_object_unref(ai);
			if (n)
				return n;
		}
		return g_strdup(app_id);
	}
	return g_strdup("Application");
}

static void notify_done(GObject *src, GAsyncResult *res, gpointer ud)
{
	char *key = ud;
	GError *err = NULL;
	GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
	struct note *n = g_hash_table_lookup(notes, key);
	if (r) {
		guint32 nid = 0;
		g_variant_get(r, "(u)", &nid);
		if (n)
			n->nid = nid;
		portal_say("notification %s -> %u", key, nid);
		g_variant_unref(r);
	} else {
		portal_say("Notify failed: %s", err->message);
		g_clear_error(&err);
		if (n)
			g_hash_table_remove(notes, key);
	}
	g_free(key);
}

static void notification_add(const char *app_id, const char *id, GVariant *dict)
{
	char *key = note_key(app_id, id);
	struct note *old = g_hash_table_lookup(notes, key);
	struct note *n = g_new0(struct note, 1);
	n->app_id = g_strdup(app_id);
	n->id = g_strdup(id);
	n->actions = g_ptr_array_new_with_free_func(g_free);
	n->targets = g_ptr_array_new_with_free_func(target_unref);
	guint32 replaces = old ? old->nid : 0;
	n->nid = replaces;

	const char *title = "", *body = "", *prio = "normal";
	GVariant *icon = NULL, *buttons = NULL;
	GVariantIter it;
	g_variant_iter_init(&it, dict);
	const char *k;
	GVariant *v;
	while (g_variant_iter_next(&it, "{&sv}", &k, &v)) {
		if (!strcmp(k, "title") && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING))
			title = g_variant_get_string(v, NULL);
		else if (!strcmp(k, "body") && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING))
			body = g_variant_get_string(v, NULL);
		else if (!strcmp(k, "priority") && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING))
			prio = g_variant_get_string(v, NULL);
		else if (!strcmp(k, "icon"))
			icon = g_variant_ref(v);
		else if (!strcmp(k, "buttons") && g_variant_is_of_type(v, G_VARIANT_TYPE("aa{sv}")))
			buttons = g_variant_ref(v);
		else if (!strcmp(k, "default-action") && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING))
			n->default_action = g_variant_dup_string(v, NULL);
		else if (!strcmp(k, "default-action-target"))
			n->default_target = g_variant_ref(v);
		/* g_variant_iter_next hands out a new reference for the keys/values it unpacks */
		g_variant_unref(v);
	}
	/* title/body point into dict, which the caller keeps alive until we return; copy them now */
	GVariantBuilder actions;
	g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
	if (n->default_action) {
		g_variant_builder_add(&actions, "s", "default");
		g_variant_builder_add(&actions, "s", "");
	}
	if (buttons) {
		gsize nb = g_variant_n_children(buttons);
		for (gsize i = 0; i < nb && i < 3; i++) {
			GVariant *b = g_variant_get_child_value(buttons, i);
			const char *label = NULL, *action = NULL;
			GVariant *target = NULL;
			g_variant_lookup(b, "label", "&s", &label);
			g_variant_lookup(b, "action", "&s", &action);
			target = g_variant_lookup_value(b, "target", NULL);
			if (label && action) {
				char ak[16];
				snprintf(ak, sizeof ak, "b%u", n->actions->len);
				g_ptr_array_add(n->actions, g_strdup(action));
				g_ptr_array_add(n->targets, target);
				g_variant_builder_add(&actions, "s", ak);
				g_variant_builder_add(&actions, "s", label);
			} else if (target) {
				g_variant_unref(target);
			}
			g_variant_unref(b);
		}
		g_variant_unref(buttons);
	}
	guchar urgency = !strcmp(prio, "urgent") ? 2 : !strcmp(prio, "low") ? 0 : 1;
	GVariantBuilder hints;
	g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&hints, "{sv}", "urgency", g_variant_new_byte(urgency));
	if (*app_id)
		g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string(app_id));
	char *icon_name = note_icon(icon);
	char *app_name = app_name_of(app_id);
	if (icon)
		g_variant_unref(icon);

	g_hash_table_replace(notes, g_strdup(key), n); /* frees old */
	g_dbus_connection_call(conn, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
	                       "org.freedesktop.Notifications", "Notify",
	                       g_variant_new("(susssasa{sv}i)", app_name, replaces, icon_name, title, body,
	                                     &actions, &hints, -1),
	                       G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, notify_done, key);
	g_free(icon_name);
	g_free(app_name);
}

static void notification_remove(const char *app_id, const char *id)
{
	char *key = note_key(app_id, id);
	struct note *n = g_hash_table_lookup(notes, key);
	if (n && n->nid)
		g_dbus_connection_call(conn, "org.freedesktop.Notifications", "/org/freedesktop/Notifications",
		                       "org.freedesktop.Notifications", "CloseNotification",
		                       g_variant_new("(u)", n->nid), NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
		                       NULL, NULL);
	g_hash_table_remove(notes, key);
	g_free(key);
}

static struct note *note_by_nid(guint32 nid)
{
	GHashTableIter it;
	gpointer k, v;
	g_hash_table_iter_init(&it, notes);
	while (g_hash_table_iter_next(&it, &k, &v))
		if (((struct note *)v)->nid == nid)
			return v;
	return NULL;
}

static void note_signal(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                        const char *sig, GVariant *params, gpointer ud)
{
	guint32 nid;
	if (!strcmp(sig, "ActionInvoked")) {
		const char *akey;
		g_variant_get(params, "(u&s)", &nid, &akey);
		struct note *n = note_by_nid(nid);
		if (!n)
			return;
		const char *action = NULL;
		GVariant *target = NULL;
		if (!strcmp(akey, "default")) {
			action = n->default_action;
			target = n->default_target;
		} else if (akey[0] == 'b') {
			guint i = (guint)atoi(akey + 1);
			if (i < n->actions->len) {
				action = g_ptr_array_index(n->actions, i);
				target = g_ptr_array_index(n->targets, i);
			}
		}
		if (!action)
			return;
		GVariantBuilder av;
		g_variant_builder_init(&av, G_VARIANT_TYPE("av"));
		if (target)
			g_variant_builder_add(&av, "v", target);
		portal_say("action %s on %s/%s", action, n->app_id, n->id);
		g_dbus_connection_emit_signal(conn, NULL, IMPL_PATH, "org.freedesktop.impl.portal.Notification",
		                              "ActionInvoked",
		                              g_variant_new("(sssav)", n->app_id, n->id, action, &av), NULL);
	} else if (!strcmp(sig, "NotificationClosed")) {
		guint32 reason;
		g_variant_get(params, "(uu)", &nid, &reason);
		struct note *n = note_by_nid(nid);
		if (n) {
			char *key = note_key(n->app_id, n->id);
			g_hash_table_remove(notes, key);
			g_free(key);
		}
	}
}

/* ---- Screenshot: snappel -------------------------------------------------------------- */

static char *shot_target(void)
{
	const char *dir = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
	char *d = dir ? g_strdup(dir) : g_build_filename(g_get_home_dir(), "pictures", NULL);
	g_mkdir_with_parents(d, 0755);
	char stamp[32];
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(stamp, sizeof stamp, "%Y-%m-%d-%H%M%S", &tm);
	char *p = g_strdup_printf("%s/screenshot-%s.png", d, stamp);
	for (int i = 2; g_file_test(p, G_FILE_TEST_EXISTS) && i < 100; i++) {
		g_free(p);
		p = g_strdup_printf("%s/screenshot-%s-%d.png", d, stamp, i);
	}
	g_free(d);
	return p;
}

static void shot_done(GObject *src, GAsyncResult *res, gpointer ud)
{
	struct req *r = ud;
	GError *err = NULL;
	if (!g_subprocess_wait_finish(G_SUBPROCESS(src), res, &err)) {
		g_clear_error(&err);
		return; /* cancelled: the request is gone already */
	}
	/* the request may be gone (closed by the caller) and r freed: only compare the pointer
	 * and the process, never touch r before it is known to be alive */
	struct req *l = reqs;
	while (l && l != r)
		l = l->next;
	if (!l || r->proc != G_SUBPROCESS(src))
		return;
	gint st = g_subprocess_get_if_exited(r->proc) ? g_subprocess_get_exit_status(r->proc) : -1;
	if (st == 0 && g_file_test(r->shot, G_FILE_TEST_EXISTS)) {
		GVariantBuilder b;
		g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
		char *uri = g_filename_to_uri(r->shot, NULL, NULL);
		g_variant_builder_add(&b, "{sv}", "uri", g_variant_new_take_string(uri));
		portal_reply(r, 0, g_variant_builder_end(&b));
	} else if (st == 0 || st == 1) {
		portal_reply(r, 1, NULL); /* snappel closed without saving: the person cancelled */
	} else {
		portal_reply(r, 2, NULL);
	}
}

static void screenshot_call(struct req *r, GVariant *options)
{
	gboolean interactive = FALSE;
	g_variant_lookup(options, "interactive", "b", &interactive);
	const char *prog = g_getenv("FLPORTAL_SNAPPEL");
	char *path = g_find_program_in_path(prog && *prog ? prog : "snappel");
	if (!path) {
		portal_say("snappel is not installed: no screenshot");
		portal_reply(r, 2, NULL);
		return;
	}
	r->shot = shot_target();
	const char *argv[] = { path, interactive ? "--region" : "--full", "--output", r->shot, NULL };
	GError *err = NULL;
	r->proc = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_NONE, &err);
	g_free(path);
	if (!r->proc) {
		portal_say("cannot run snappel: %s", err->message);
		g_clear_error(&err);
		portal_reply(r, 2, NULL);
		return;
	}
	g_subprocess_wait_async(r->proc, NULL, shot_done, r);
}

/* ---- the interfaces ------------------------------------------------------------------- */

#define HDR \
	"<arg type='o' name='handle' direction='in'/><arg type='s' name='app_id' direction='in'/>" \
	"<arg type='s' name='parent_window' direction='in'/>"
#define OUT \
	"<arg type='u' name='response' direction='out'/><arg type='a{sv}' name='results' direction='out'/>"
#define OPTS "<arg type='a{sv}' name='options' direction='in'/>"
#define TITLE "<arg type='s' name='title' direction='in'/>"

static const char portal_xml[] =
	"<node>"
	"<interface name='org.freedesktop.impl.portal.FileChooser'>"
	"<method name='OpenFile'>" HDR TITLE OPTS OUT "</method>"
	"<method name='SaveFile'>" HDR TITLE OPTS OUT "</method>"
	"<method name='SaveFiles'>" HDR TITLE OPTS OUT "</method>"
	"<property name='version' type='u' access='read'/>"
	"</interface>"
	"<interface name='org.freedesktop.impl.portal.AppChooser'>"
	"<method name='ChooseApplication'>" HDR "<arg type='as' name='choices' direction='in'/>" OPTS OUT "</method>"
	"<method name='UpdateChoices'><arg type='o' name='handle' direction='in'/>"
	"<arg type='as' name='choices' direction='in'/></method>"
	"<property name='version' type='u' access='read'/>"
	"</interface>"
	"<interface name='org.freedesktop.impl.portal.Access'>"
	"<method name='AccessDialog'>" HDR TITLE "<arg type='s' name='subtitle' direction='in'/>"
	"<arg type='s' name='body' direction='in'/>" OPTS OUT "</method>"
	"</interface>"
	"<interface name='org.freedesktop.impl.portal.Notification'>"
	"<method name='AddNotification'><arg type='s' name='app_id' direction='in'/>"
	"<arg type='s' name='id' direction='in'/><arg type='a{sv}' name='notification' direction='in'/></method>"
	"<method name='RemoveNotification'><arg type='s' name='app_id' direction='in'/>"
	"<arg type='s' name='id' direction='in'/></method>"
	"<signal name='ActionInvoked'><arg type='s' name='app_id'/><arg type='s' name='id'/>"
	"<arg type='s' name='action'/><arg type='av' name='parameter'/></signal>"
	"<property name='version' type='u' access='read'/>"
	"</interface>"
	"<interface name='org.freedesktop.impl.portal.Settings'>"
	"<method name='ReadAll'><arg type='as' name='namespaces' direction='in'/>"
	"<arg type='a{sa{sv}}' name='value' direction='out'/></method>"
	"<method name='Read'><arg type='s' name='namespace' direction='in'/>"
	"<arg type='s' name='key' direction='in'/><arg type='v' name='value' direction='out'/></method>"
	"<signal name='SettingChanged'><arg type='s' name='namespace'/><arg type='s' name='key'/>"
	"<arg type='v' name='value'/></signal>"
	"<property name='version' type='u' access='read'/>"
	"</interface>"
	"<interface name='org.freedesktop.impl.portal.Screenshot'>"
	"<method name='Screenshot'>" HDR OPTS OUT "</method>"
	"<method name='PickColor'>" HDR OPTS OUT "</method>"
	"<property name='version' type='u' access='read'/>"
	"</interface>"
	"</node>";

/* Only the frontend (the owner of org.freedesktop.portal.Desktop) may call the impl interfaces;
 * FLPORTAL_ANY_SENDER=1 turns the check off for the tests. */
static gboolean sender_ok(const char *sender)
{
	if (g_getenv("FLPORTAL_ANY_SENDER"))
		return TRUE;
	GError *err = NULL;
	GVariant *v = g_dbus_connection_call_sync(conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
	                                          "org.freedesktop.DBus", "GetNameOwner",
	                                          g_variant_new("(s)", "org.freedesktop.portal.Desktop"),
	                                          G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &err);
	if (!v) {
		g_clear_error(&err);
		return FALSE;
	}
	const char *owner = NULL;
	g_variant_get(v, "(&s)", &owner);
	gboolean ok = owner && sender && !strcmp(owner, sender);
	g_variant_unref(v);
	return ok;
}

static void method_call(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                        const char *method, GVariant *p, GDBusMethodInvocation *inv, gpointer ud)
{
	const char *ifn = iface + strlen("org.freedesktop.impl.portal.");
	portal_say("%s.%s from %s", ifn, method, sender);
	if (!sender_ok(sender)) {
		portal_say("refused: %s is not xdg-desktop-portal", sender);
		g_dbus_method_invocation_return_error_literal(inv, G_DBUS_ERROR, G_DBUS_ERROR_ACCESS_DENIED,
		                                              "only xdg-desktop-portal may call the impl interfaces");
		return;
	}
	if (!strcmp(ifn, "FileChooser")) {
		const char *handle, *app_id, *parent, *title;
		GVariant *opts;
		g_variant_get(p, "(&o&s&s&s@a{sv})", &handle, &app_id, &parent, &title, &opts);
		struct req *r = req_new(inv, handle);
		if (r)
			fc_open(r, !strcmp(method, "OpenFile") ? FC_OPEN : !strcmp(method, "SaveFile") ? FC_SAVE : FC_SAVEFILES,
			        title, opts);
		g_variant_unref(opts);
	} else if (!strcmp(ifn, "AppChooser")) {
		if (!strcmp(method, "ChooseApplication")) {
			const char *handle, *app_id, *parent;
			char **choices;
			GVariant *opts;
			g_variant_get(p, "(&o&s&s^as@a{sv})", &handle, &app_id, &parent, &choices, &opts);
			struct req *r = req_new(inv, handle);
			if (r)
				ac_open(r, (const char *const *)choices, opts);
			g_strfreev(choices);
			g_variant_unref(opts);
		} else { /* UpdateChoices */
			const char *handle;
			g_variant_get(p, "(&o^as)", &handle, NULL);
			struct req *r = portal_req_find(handle);
			if (r && r->dlg && r->dlg->update)
				r->dlg->update(r->dlg, p);
			g_dbus_method_invocation_return_value(inv, NULL);
		}
	} else if (!strcmp(ifn, "Access")) {
		const char *handle, *app_id, *parent, *title, *sub, *body;
		GVariant *opts;
		g_variant_get(p, "(&o&s&s&s&s&s@a{sv})", &handle, &app_id, &parent, &title, &sub, &body, &opts);
		struct req *r = req_new(inv, handle);
		if (r)
			ac_access(r, title, sub, body, opts);
		g_variant_unref(opts);
	} else if (!strcmp(ifn, "Notification")) {
		const char *app_id, *id;
		if (!strcmp(method, "AddNotification")) {
			GVariant *d;
			g_variant_get(p, "(&s&s@a{sv})", &app_id, &id, &d);
			notification_add(app_id, id, d);
			g_variant_unref(d);
		} else {
			g_variant_get(p, "(&s&s)", &app_id, &id);
			notification_remove(app_id, id);
		}
		g_dbus_method_invocation_return_value(inv, NULL);
	} else if (!strcmp(ifn, "Settings")) {
		if (!strcmp(method, "ReadAll")) {
			char **ns;
			g_variant_get(p, "(^as)", &ns);
			GVariant *all = settings_read_all((const char *const *)ns);
			g_strfreev(ns);
			g_dbus_method_invocation_return_value(inv, g_variant_new("(@a{sa{sv}})", all));
		} else {
			const char *ns, *key;
			g_variant_get(p, "(&s&s)", &ns, &key);
			GVariant *v = setting_get(ns, key);
			if (v)
				g_dbus_method_invocation_return_value(inv, g_variant_new("(v)", v));
			else
				g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.portal.Error.NotFound",
				                                           "Requested setting not found");
		}
	} else if (!strcmp(ifn, "Screenshot")) {
		const char *handle, *app_id, *parent;
		GVariant *opts;
		g_variant_get(p, "(&o&s&s@a{sv})", &handle, &app_id, &parent, &opts);
		struct req *r = req_new(inv, handle);
		if (r) {
			if (!strcmp(method, "Screenshot"))
				screenshot_call(r, opts);
			else /* PickColor: there is no picker yet */
				portal_reply(r, 2, NULL);
		}
		g_variant_unref(opts);
	}
}

static GVariant *get_property(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                              const char *prop, GError **err, gpointer ud)
{
	guint v = 1;
	if (!strcmp(iface, "org.freedesktop.impl.portal.FileChooser"))
		v = 3;
	else if (!strcmp(iface, "org.freedesktop.impl.portal.Screenshot"))
		v = 2;
	return g_variant_new_uint32(v);
}

static const GDBusInterfaceVTable portal_vtable = { .method_call = method_call, .get_property = get_property };

static void name_lost(GDBusConnection *c, const char *name, gpointer ud)
{
	fprintf(stderr, "flportal: lost the name %s (another backend running?)\n", name);
	quit = 1;
}

static void name_acquired(GDBusConnection *c, const char *name, gpointer ud) { portal_say("owns %s", name); }

static gboolean on_term(gpointer ud)
{
	quit = 1;
	return G_SOURCE_REMOVE;
}

/* ---- the loop: GLib and libflui in one poll ---------------------------------------------- */

static void connection_lost(void)
{
	fprintf(stderr, "flportal: the compositor went away\n");
	for (struct dlg *d = dlgs; d; d = d->next)
		if (d->req)
			portal_reply(d->req, 2, NULL);
		else
			d->dead = 1;
	for (struct dlg *d = dlgs; d; d = d->next)
		d->win = NULL; /* the windows died with the connection */
	reap();
	if (app) {
		fl_app_free(app);
		app = NULL;
	}
}

static void run(void)
{
	GMainContext *ctx = g_main_context_default();
	GPollFD *gfd = NULL;
	int gcap = 0;
	struct pollfd *pf = NULL;
	int pcap = 0;
	g_main_context_acquire(ctx);
	while (!quit) {
		reap();
		gint prio, tmo, ng;
		g_main_context_prepare(ctx, &prio);
		while ((ng = g_main_context_query(ctx, prio, &tmo, gfd, gcap)) > gcap) {
			gcap = ng + 8;
			gfd = g_realloc(gfd, (gsize)gcap * sizeof *gfd);
		}
		int fn = 0;
		fl_app *a = app && !fl_app_ppm(app) ? app : NULL;
		if (pcap < ng + 16) {
			pcap = ng + 16;
			pf = g_realloc(pf, (gsize)pcap * sizeof *pf);
		}
		if (a) {
			for (;;) {
				fn = fl_app_prepare(a, pf, pcap - ng);
				if (fn != -2)
					break;
				pcap += 16;
				pf = g_realloc(pf, (gsize)pcap * sizeof *pf);
			}
			if (fn < 0) {
				g_main_context_release(ctx);
				connection_lost();
				g_main_context_acquire(ctx);
				continue;
			}
		}
		for (int i = 0; i < ng; i++) {
			pf[fn + i].fd = gfd[i].fd;
			pf[fn + i].events = (short)gfd[i].events;
			pf[fn + i].revents = 0;
		}
		int r = poll(pf, (nfds_t)(fn + ng), tmo);
		if (a && fl_app_dispatch(a, pf, fn, r) < 0) {
			for (int i = 0; i < ng; i++)
				gfd[i].revents = 0;
			g_main_context_check(ctx, prio, gfd, 0);
			g_main_context_release(ctx);
			connection_lost();
			g_main_context_acquire(ctx);
			continue;
		}
		for (int i = 0; i < ng; i++)
			gfd[i].revents = r > 0 ? (gushort)pf[fn + i].revents : 0;
		if (g_main_context_check(ctx, prio, gfd, ng))
			g_main_context_dispatch(ctx);
	}
	g_main_context_release(ctx);
	g_free(gfd);
	g_free(pf);
}

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) {
			verbose = TRUE;
		} else {
			fprintf(stderr, "usage: flportal [-v]\n"
			                "The FreeLFS backend of xdg-desktop-portal. D-Bus starts it; -v logs requests.\n");
			return !strcmp(argv[i], "-h") || !strcmp(argv[i], "--help") ? 0 : 2;
		}
	}
	if (getenv("FLPORTAL_VERBOSE"))
		verbose = TRUE;
	signal(SIGPIPE, SIG_IGN);

	GError *err = NULL;
	conn = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
	if (!conn) {
		fprintf(stderr, "flportal: no session bus: %s\n", err->message);
		return 1;
	}
	g_dbus_connection_set_exit_on_close(conn, TRUE);
	GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(portal_xml, &err);
	request_info = g_dbus_node_info_new_for_xml(request_xml, NULL);
	if (!info) {
		fprintf(stderr, "flportal: bad interface XML: %s\n", err->message);
		return 1;
	}
	for (guint i = 0; info->interfaces[i]; i++) {
		if (!g_dbus_connection_register_object(conn, IMPL_PATH, info->interfaces[i], &portal_vtable, NULL,
		                                       NULL, &err)) {
			fprintf(stderr, "flportal: cannot export %s: %s\n", info->interfaces[i]->name, err->message);
			return 1;
		}
	}
	notes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, note_free);
	g_dbus_connection_signal_subscribe(conn, "org.freedesktop.Notifications", "org.freedesktop.Notifications",
	                                   NULL, "/org/freedesktop/Notifications", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
	                                   note_signal, NULL, NULL);

	settings_compute();
	const char *cp = fl_conf_path();
	if (cp) {
		GFile *f = g_file_new_for_path(cp);
		GFileMonitor *m = g_file_monitor_file(f, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
		if (m)
			g_signal_connect(m, "changed", G_CALLBACK(conf_changed), NULL);
		g_object_unref(f);
	}

	g_unix_signal_add(SIGTERM, on_term, NULL);
	g_unix_signal_add(SIGINT, on_term, NULL);
	g_bus_own_name_on_connection(conn, IMPL_NAME, G_BUS_NAME_OWNER_FLAGS_NONE, name_acquired, name_lost, NULL,
	                             NULL);
	run();
	return 0;
}
