/* portal-client -- what an app does to open the file chooser: FileChooser.OpenFile on org.freedesktop.portal.Desktop, then it
 * STAYS on the bus and waits for the Response signal (xdg-desktop-portal closes a request whose caller has left the bus, so a
 * gdbus one-shot call shows the dialog for a moment only). Prints "response CODE" and the chosen URIs.
 *   portal-client [open|save|folder]
 * SPDX-License-Identifier: MIT */
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

static GMainLoop *loop;

static void on_response(GDBusConnection *c, const gchar *sender, const gchar *path, const gchar *iface, const gchar *sig,
                        GVariant *params, gpointer ud)
{
	guint32 code;
	GVariant *res;
	g_variant_get(params, "(u@a{sv})", &code, &res);
	printf("response %u\n", code);
	GVariant *uris = g_variant_lookup_value(res, "uris", G_VARIANT_TYPE_STRING_ARRAY);
	if (uris) {
		gsize n;
		const gchar **v = g_variant_get_strv(uris, &n);
		for (gsize i = 0; i < n; i++)
			printf("uri %s\n", v[i]);
		g_free(v);
	}
	fflush(stdout);
	g_main_loop_quit(loop);
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "open";
	GError *err = NULL;
	GDBusConnection *c = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
	if (!c) {
		fprintf(stderr, "no session bus: %s\n", err->message);
		return 2;
	}
	/* the request object path is predictable: /org/freedesktop/portal/desktop/request/SENDER/TOKEN */
	char *sender = g_strdup(g_dbus_connection_get_unique_name(c) + 1);
	for (char *p = sender; *p; p++)
		if (*p == '.')
			*p = '_';
	char *rpath = g_strdup_printf("/org/freedesktop/portal/desktop/request/%s/fltest", sender);
	g_dbus_connection_signal_subscribe(c, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.Request", "Response", rpath, NULL,
	                                   G_DBUS_SIGNAL_FLAGS_NONE, on_response, NULL, NULL);
	GVariantBuilder b;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string("fltest"));
	if (!strcmp(mode, "folder"))
		g_variant_builder_add(&b, "{sv}", "directory", g_variant_new_boolean(TRUE));
	GVariant *r = g_dbus_connection_call_sync(c, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
	                                          "org.freedesktop.portal.FileChooser", !strcmp(mode, "save") ? "SaveFile" : "OpenFile",
	                                          g_variant_new("(ssa{sv})", "", "Choose a file", &b), NULL, G_DBUS_CALL_FLAGS_NONE, 30000, NULL, &err);
	if (!r) {
		fprintf(stderr, "call failed: %s\n", err->message);
		return 3;
	}
	printf("requested\n");
	fflush(stdout);
	loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(loop);
	return 0;
}
