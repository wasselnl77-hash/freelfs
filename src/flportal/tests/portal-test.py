#!/usr/bin/env python3
"""One scenario of tests/portal.sh: talk to xdg-desktop-portal (and to flportal directly where the
frontend would get in the way) on the private session bus, and check the answers.
Usage: portal-test.py SCENARIO.   Exit 0 when all checks pass.
SPDX-License-Identifier: MIT"""
import os
import sys
import time

import gi

gi.require_version("Gio", "2.0")
gi.require_version("GLib", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

T = os.environ["TEST_TMP"]
HOME = os.environ["HOME"]
DOCS = HOME + "/Documents"
PORTAL = "org.freedesktop.portal.Desktop"
IMPL = "org.freedesktop.impl.portal.desktop.freelfs"
PATH = "/org/freedesktop/portal/desktop"
bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
ctx = GLib.MainContext.default()
fails = []
tokens = [0]


def check(what, cond, detail=""):
    if not cond:
        fails.append(what)
        print("  check failed: %s %s" % (what, detail), file=sys.stderr)


def pump(until, timeout=20.0):
    end = time.time() + timeout
    while not until() and time.time() < end:
        ctx.iteration(False)
        time.sleep(0.01)
    return until()


def request(iface, method, head, opts, sig, timeout=25.0):
    """Call a portal method that answers with a Request; return (response, results)."""
    tokens[0] += 1
    token = "t%d" % tokens[0]
    opts = dict(opts)
    opts["handle_token"] = GLib.Variant("s", token)
    sender = bus.get_unique_name()[1:].replace(".", "_")
    path = "%s/request/%s/%s" % (PATH, sender, token)
    got = []

    def on_resp(conn, snd, p, i, sig_name, params):
        got.append(params.unpack())

    sub = bus.signal_subscribe(PORTAL, "org.freedesktop.portal.Request", "Response", path, None,
                               Gio.DBusSignalFlags.NONE, on_resp)
    try:
        bus.call_sync(PORTAL, PATH, iface, method, GLib.Variant(sig, tuple(head) + (opts,)), None,
                      Gio.DBusCallFlags.NONE, int(timeout * 1000))
        pump(lambda: got, timeout)
    finally:
        bus.signal_unsubscribe(sub)
    if not got:
        fails.append("%s.%s: no Response" % (iface.split(".")[-1], method))
        print("  no response from %s" % method, file=sys.stderr)
        return None, {}
    return got[0]


def bs(path):
    return GLib.Variant("ay", path.encode() + b"\0")


def filters_text():
    return GLib.Variant("a(sa(us))", [("Text files", [(0, "*.txt"), (0, "*.md")]),
                                      ("Images", [(1, "image/png"), (0, "*.svg")]),
                                      ("All files", [(0, "*")])])


def images_filter():
    return GLib.Variant("(sa(us))", ("Images", [(1, "image/png"), (0, "*.svg")]))


def choices_enc():
    return GLib.Variant("a(ssa(ss)s)", [("encoding", "Encoding", [("utf8", "UTF-8"), ("latin1", "Latin-1")], "utf8"),
                                        ("append", "Add to recent files", [], "true")])


def file_opts(**kw):
    o = {"current_folder": bs(DOCS)}
    o.update(kw)
    return o


# ---- scenarios -------------------------------------------------------------------------------

def sc_settings():
    r = bus.call_sync(PORTAL, PATH, "org.freedesktop.portal.Settings", "ReadAll",
                      GLib.Variant("(as)", (["org.freedesktop.appearance"],)), None, 0, 20000).unpack()[0]
    a = r.get("org.freedesktop.appearance", {})
    check("color-scheme is 1 (dark)", a.get("color-scheme") == 1, str(a))
    acc = a.get("accent-color")
    check("accent-color is the green", acc and abs(acc[0] - 0x3f / 255) < 0.01 and abs(acc[1] - 0x9b / 255) < 0.01, str(acc))
    v = bus.call_sync(PORTAL, PATH, "org.freedesktop.portal.Settings", "Read",
                      GLib.Variant("(ss)", ("org.freedesktop.appearance", "color-scheme")), None, 0, 20000).unpack()[0]
    check("Read color-scheme", v == 1, str(v))
    try:
        bus.call_sync(PORTAL, PATH, "org.freedesktop.portal.Settings", "Read",
                      GLib.Variant("(ss)", ("org.freedesktop.appearance", "nonsense")), None, 0, 20000)
        check("Read of an unknown key fails", False)
    except GLib.Error:
        pass
    changes = []
    sub = bus.signal_subscribe(PORTAL, "org.freedesktop.portal.Settings", "SettingChanged", PATH, None, 0,
                               lambda c, s, p, i, n, params: changes.append(params.unpack()))
    open(T + "/desktop.conf", "w").write("accent=blue\n")
    pump(lambda: any(c[1] == "accent-color" for c in changes), 8)
    ch = [c for c in changes if c[1] == "accent-color"]
    check("SettingChanged accent-color after accent=blue", bool(ch), str(changes))
    if ch:
        check("... to blue", abs(ch[-1][2][2] - 0xd0 / 255) < 0.01, str(ch[-1]))
    open(T + "/desktop.conf", "w").write("accent=blue\ntheme=light\n")
    pump(lambda: any(c[1] == "color-scheme" for c in changes), 8)
    check("SettingChanged color-scheme after theme=light",
          any(c[1] == "color-scheme" and c[2] == 2 for c in changes), str(changes))
    v = bus.call_sync(PORTAL, PATH, "org.freedesktop.portal.Settings", "Read",
                      GLib.Variant("(ss)", ("org.freedesktop.appearance", "accent-color")), None, 0, 20000).unpack()[0]
    check("light theme uses the darker blue", abs(v[2] - 0xae / 255) < 0.01, str(v))
    bus.signal_unsubscribe(sub)


def sc_notification():
    seen = []
    nid = [7]

    def on_call(conn, sender, path, iface, method, params, inv):
        if method == "Notify":
            seen.append(("Notify", params.unpack()))
            inv.return_value(GLib.Variant("(u)", (nid[0],)))
        elif method == "CloseNotification":
            seen.append(("Close", params.unpack()))
            inv.return_value(None)
        else:
            inv.return_value(None)

    xml = """<node><interface name='org.freedesktop.Notifications'>
      <method name='Notify'><arg type='s' direction='in'/><arg type='u' direction='in'/><arg type='s' direction='in'/>
        <arg type='s' direction='in'/><arg type='s' direction='in'/><arg type='as' direction='in'/>
        <arg type='a{sv}' direction='in'/><arg type='i' direction='in'/><arg type='u' direction='out'/></method>
      <method name='CloseNotification'><arg type='u' direction='in'/></method>
      <signal name='ActionInvoked'><arg type='u'/><arg type='s'/></signal>
      <signal name='NotificationClosed'><arg type='u'/><arg type='u'/></signal>
    </interface></node>"""
    info = Gio.DBusNodeInfo.new_for_xml(xml)
    bus.register_object("/org/freedesktop/Notifications", info.interfaces[0], on_call, None, None)
    owned = []
    Gio.bus_own_name_on_connection(bus, "org.freedesktop.Notifications", Gio.BusNameOwnerFlags.NONE,
                                   lambda *a: owned.append(1), None)
    pump(lambda: owned, 5)
    actions = []
    sub = bus.signal_subscribe(IMPL, "org.freedesktop.impl.portal.Notification", "ActionInvoked", PATH, None, 0,
                               lambda c, s, p, i, n, params: actions.append(params.unpack()))

    def add(id_, title, extra=None):
        d = {"title": GLib.Variant("s", title), "body": GLib.Variant("s", "Three new messages"),
             "priority": GLib.Variant("s", "urgent"),
             "icon": GLib.Variant("(sv)", ("themed", GLib.Variant("as", ["mail-unread", "mail"]))),
             "default-action": GLib.Variant("s", "app.show"),
             "default-action-target": GLib.Variant("s", "inbox"),
             "buttons": GLib.Variant("aa{sv}", [{"label": GLib.Variant("s", "Reply"), "action": GLib.Variant("s", "app.reply"),
                                                  "target": GLib.Variant("u", 3)},
                                                 {"label": GLib.Variant("s", "Mark read"), "action": GLib.Variant("s", "app.read")}])}
        bus.call_sync(IMPL, PATH, "org.freedesktop.impl.portal.Notification", "AddNotification",
                      GLib.Variant("(ssa{sv})", ("org.example.Mail", id_, d)), None, 0, 5000)

    add("m1", "Mail")
    pump(lambda: seen, 5)
    check("Notify reached org.freedesktop.Notifications", bool(seen), str(seen))
    if seen:
        a = seen[0][1]
        check("summary and body", a[3] == "Mail" and a[4] == "Three new messages", str(a))
        check("themed icon name", a[2] == "mail-unread", str(a))
        check("urgency 2 for urgent", a[6].get("urgency") == 2, str(a[6]))
        check("actions: default, Reply, Mark read", a[5] == ["default", "", "b0", "Reply", "b1", "Mark read"], str(a[5]))
        check("replaces nothing the first time", a[1] == 0)
    pump(lambda: False, 0.3)  # let the Notify reply (the id) arrive
    add("m1", "Mail again")
    pump(lambda: len(seen) >= 2, 5)
    check("same id replaces notification 7", len(seen) >= 2 and seen[1][1][1] == 7, str(seen))
    bus.emit_signal(None, "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "ActionInvoked",
                    GLib.Variant("(us)", (7, "b0")))
    pump(lambda: actions, 5)
    check("ActionInvoked b0 -> app.reply with its target",
          actions and actions[0][:3] == ("org.example.Mail", "m1", "app.reply") and actions[0][3] == [3], str(actions))
    bus.emit_signal(None, "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "ActionInvoked",
                    GLib.Variant("(us)", (7, "default")))
    pump(lambda: len(actions) >= 2, 5)
    check("ActionInvoked default -> app.show(inbox)",
          len(actions) >= 2 and actions[1][2] == "app.show" and actions[1][3] == ["inbox"], str(actions))
    bus.call_sync(IMPL, PATH, "org.freedesktop.impl.portal.Notification", "RemoveNotification",
                  GLib.Variant("(ss)", ("org.example.Mail", "m1")), None, 0, 5000)
    pump(lambda: any(s[0] == "Close" for s in seen), 5)
    check("RemoveNotification -> CloseNotification(7)", ("Close", (7,)) in seen, str(seen))
    bus.signal_unsubscribe(sub)


def sc_shots():
    """Every dialog once, answered 'cancelled' (FLPORTAL_SHOT writes the pictures)."""
    resp, res = request("org.freedesktop.portal.FileChooser", "OpenFile", ("", "Choose a picture"),
                        file_opts(multiple=GLib.Variant("b", True), filters=filters_text(),
                                  current_filter=images_filter(), choices=choices_enc()), "(ssa{sv})")
    check("OpenFile cancelled gives 1", resp == 1, str(resp))
    resp, res = request("org.freedesktop.portal.FileChooser", "SaveFile", ("", "Save document as"),
                        file_opts(current_name=GLib.Variant("s", "Untitled.txt"), filters=filters_text()), "(ssa{sv})")
    check("SaveFile cancelled gives 1", resp == 1, str(resp))
    resp, res = request("org.freedesktop.portal.FileChooser", "OpenFile", ("", "Choose a folder"),
                        file_opts(directory=GLib.Variant("b", True),
                                  accept_label=GLib.Variant("s", "_Select folder")), "(ssa{sv})")
    check("folder dialog cancelled gives 1", resp == 1, str(resp))
    resp, res = request("org.freedesktop.portal.FileChooser", "SaveFiles", ("", "Save 3 images"),
                        file_opts(files=GLib.Variant("aay", [b"a.png\0", b"b.png\0", b"notes.txt\0"])), "(ssa{sv})")
    check("SaveFiles cancelled gives 1", resp == 1, str(resp))
    resp, res = request("org.freedesktop.portal.OpenURI", "OpenURI", ("", "https://freelfs.example/docs"),
                        {"ask": GLib.Variant("b", True)}, "(ssa{sv})")
    check("OpenURI with the app chooser cancelled gives 1", resp == 1, str(resp))


def sc_open_multiple():
    resp, res = request("org.freedesktop.portal.FileChooser", "OpenFile", ("", "Pick notes"),
                        file_opts(multiple=GLib.Variant("b", True), filters=filters_text(),
                                  current_filter=GLib.Variant("(sa(us))", ("Text files", [(0, "*.txt"), (0, "*.md")])),
                                  choices=choices_enc()), "(ssa{sv})")
    check("accepted: 0", resp == 0, str(resp))
    uris = res.get("uris", [])
    check("two uris, in order", uris == ["file://" + DOCS + "/notes.txt", "file://" + DOCS + "/todo.md"], str(uris))
    check("current_filter returned", res.get("current_filter", ("",))[0] == "Text files", str(res))
    check("choices returned", dict(res.get("choices", [])) == {"encoding": "utf8", "append": "true"}, str(res.get("choices")))
    # the Images filter hides notes.txt and todo.md: nothing to pick, so the dialog stays unanswered (test: 1)
    resp, res = request("org.freedesktop.portal.FileChooser", "OpenFile", ("", "Pick one"),
                        file_opts(filters=filters_text(), current_filter=images_filter()), "(ssa{sv})")
    check("a hidden file cannot be picked", resp == 1, str(resp))


def sc_save_file():
    resp, res = request("org.freedesktop.portal.FileChooser", "SaveFile", ("", "Save as"),
                        file_opts(current_name=GLib.Variant("s", "draft.txt")), "(ssa{sv})")
    check("accepted: 0", resp == 0, str(resp))
    check("uri of the typed name", res.get("uris") == ["file://" + DOCS + "/minutes.txt"], str(res))
    resp, res = request("org.freedesktop.portal.FileChooser", "SaveFile", ("", "Save as"),
                        {"current_file": bs(DOCS + "/notes.txt")}, "(ssa{sv})")
    check("current_file: its folder and name", res.get("uris") in (["file://" + DOCS + "/minutes.txt"],), str(res))


def sc_save_files():
    resp, res = request("org.freedesktop.portal.FileChooser", "SaveFiles", ("", "Save 2 files"),
                        file_opts(files=GLib.Variant("aay", [b"a.txt\0", b"notes.txt\0"])), "(ssa{sv})")
    check("accepted: 0", resp == 0, str(resp))
    check("names, the taken one gets a number",
          res.get("uris") == ["file://" + DOCS + "/a.txt", "file://" + DOCS + "/notes%20(2).txt"], str(res))


def sc_open_folder():
    resp, res = request("org.freedesktop.portal.FileChooser", "OpenFile", ("", "Pick a folder"),
                        file_opts(directory=GLib.Variant("b", True)), "(ssa{sv})")
    check("accepted: 0", resp == 0, str(resp))
    check("the selected folder", res.get("uris") == ["file://" + DOCS + "/Reports"], str(res))


def sc_appchooser():
    """The backend's own answer (the frontend would launch the app it gets back)."""
    h = "/org/freedesktop/portal/desktop/request/x/ac1"
    r = bus.call_sync(IMPL, PATH, "org.freedesktop.impl.portal.AppChooser", "ChooseApplication",
                      GLib.Variant("(ossasa{sv})", (h, "", "", ["freebws"], {"content_type": GLib.Variant("s", "x-scheme-handler/https"),
                                                                              "heading": GLib.Variant("s", "Open link with")})),
                      None, 0, 20000).unpack()
    check("chosen app: the recommended one first", r[0] == 0 and r[1].get("choice") == "freebws", str(r))


def sc_screenshot():
    resp, res = request("org.freedesktop.portal.Screenshot", "Screenshot", ("",), {}, "(sa{sv})")
    check("allowed and taken: 0", resp == 0, str(resp))
    uri = res.get("uri", "")
    check("the picture exists", uri.startswith("file://") and os.path.exists(uri[7:]), str(res))
    check("it is in the pictures folder", uri.startswith("file://" + HOME + "/Pictures/screenshot-"), uri)


def sc_screenshot_none():
    resp, res = request("org.freedesktop.portal.Screenshot", "Screenshot", ("",), {}, "(sa{sv})")
    check("no snappel: response 2", resp == 2, str(resp))


def sc_access_deny():
    resp, res = request("org.freedesktop.portal.Screenshot", "Screenshot", ("",), {}, "(sa{sv})")
    check("Deny: not 0", resp in (1, 2), str(resp))


def sc_overlay():
    resp, res = request("org.freedesktop.portal.FileChooser", "SaveFile", ("", "Save report as"),
                        file_opts(current_name=GLib.Variant("s", "notes.txt"), filters=filters_text()), "(ssa{sv})")
    check("cancelled gives 1", resp == 1, str(resp))


SCEN = {
    "overlay-confirm": sc_overlay, "overlay-popup": sc_overlay, "overlay-prompt": sc_overlay,
    "settings": sc_settings, "notification": sc_notification, "shots": sc_shots,
    "open-multiple": sc_open_multiple, "save-file": sc_save_file, "save-files": sc_save_files,
    "open-folder": sc_open_folder, "appchooser": sc_appchooser, "screenshot": sc_screenshot,
    "screenshot-none": sc_screenshot_none, "access-deny": sc_access_deny,
}

if __name__ == "__main__":
    name = sys.argv[1]
    if name not in SCEN:
        sys.exit("unknown scenario " + name)
    try:
        SCEN[name]()
    except GLib.Error as e:
        fails.append("D-Bus error: " + e.message)
        print("  D-Bus error: " + e.message, file=sys.stderr)
    sys.exit(1 if fails else 0)
