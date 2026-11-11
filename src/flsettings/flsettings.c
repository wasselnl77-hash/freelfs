/* flsettings -- theme, panel labels, keyboard, Wi-Fi
 * SPDX-License-Identifier: MIT
 *
 * Pages behind a sidebar: General (below), Display, Mouse & touchpad and
 * Keyboard (pages.c), Date & time (datetime.c), Wi-Fi (wifi.c), Snapshots, About (about.c, with
 * "Save a report"). `flsettings --page wifi` opens on the
 * Wi-Fi page; when Settings is already running, the new process tells the old
 * one over $XDG_RUNTIME_DIR/flsettings.sock and exits.
 *
 * Everything lands in ~/.config/freelfs/desktop.conf (theme=, panel_labels=,
 * keyboard=, the pointer and keyboard keys, ...), which the panel, terminal,
 * flcomp and this program re-read when it changes. The keyboard layout is also
 * switched live through fl_shell, and written to /etc/keyboard.conf when that
 * is writable.
 */
#include <errno.h>
#include <flui.h>
#include <signal.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-client.h>

#include "fl-shell-client-protocol.h"
#include "about.h"
#include "datetime.h"
#include "snapshots.h"
#include "pages.h"
#include "wallpaper.h"
#include "wifi.h"

enum { ID_DARK = 1, ID_LIGHT, ID_LABELS, ID_SOUNDS, ID_SOUND_TEST, ID_PAGE_GENERAL, ID_PAGE_WIFI, ID_PAGE_DISPLAY, ID_PAGE_INPUT,
       ID_PAGE_KEYBOARD, ID_PAGE_DATETIME, ID_PAGE_SNAPSHOTS, ID_PAGE_ABOUT, ID_SCROLL, ID_KBD = 20, ID_ACCENT = 60 /* .. 65 */ };
enum { PAGE_GENERAL, PAGE_DISPLAY, PAGE_INPUT, PAGE_KEYBOARD, PAGE_WIFI, PAGE_DATETIME, PAGE_SNAPSHOTS, PAGE_ABOUT };
#define SIDEBAR 168

struct settings {
	fl_app *app;
	struct fl_shell *shell;
	uint32_t shell_ver;
	fl_font *head, *small;
	int page;
	struct pages *pg;
	int scroll, content_h;	/* the pages of pages.c scroll when they do not fit */
	fl_win *win;
	struct wifi *wifi;
	struct datetime *dt;
	struct snapshots *snap;
	struct about *about;
	int sock;
};

static void set_page(struct settings *s, int page)
{
	if (page == s->page)
		return;
	s->page = page;
	s->scroll = 0;
	s->content_h = 0;
	wifi_show(s->wifi, page == PAGE_WIFI);
	dt_show(s->dt, page == PAGE_DATETIME);
	snap_show(s->snap, page == PAGE_SNAPSHOTS);
	if (page == PAGE_ABOUT)
		about_show(s->about);
	pages_show(s->pg, page >= PAGE_DISPLAY && page <= PAGE_KEYBOARD ? page - PAGE_DISPLAY : -1);
	if (s->win)
		fl_win_redraw(s->win);
}

static void on_global(void *ud, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver)
{
	struct settings *s = ud;
	if (!strcmp(iface, "fl_shell")) {
		uint32_t v = ver < 3 ? ver : 3;
		s->shell = wl_registry_bind(reg, name, &fl_shell_interface, v);
		s->shell_ver = v;
		pages_set_shell(s->pg, s->shell, v);
	}
}

static void section(fl_ui *ui, struct settings *s, int x, int y, int w, const char *name)
{
	pg_section(ui, s->head, x, y, w, name);
}

static void row_text(fl_ui *ui, struct settings *s, int x, int y, int w, const char *title, const char *what)
{
	pg_row_text(ui, s->small, x, y, w, title, what);
}

static int choice(fl_ui *ui, uint32_t id, fl_rect r, const char *label, int on)
{
	return pg_choice(ui, id, r, label, on);
}

/* the accent swatches: flat squares, the chosen one framed */
static void draw_accents(fl_ui *ui, struct settings *s, int x, int y, int w)
{
	const char *cur = fl_conf_get("accent", "green");
	int sel = fl_accent_find(cur), sz = 28, gap = 10;
	char sub[96];
	snprintf(sub, sizeof sub, "Buttons, selections and highlights: %s",
	         sel >= 0 ? fl_accents[sel].name : "custom");
	row_text(ui, s, x, y, w - (FL_ACCENT_COUNT * (sz + gap) + 20), "Accent colour", sub);
	for (int i = 0; i < FL_ACCENT_COUNT; i++) {
		fl_rect r = fl_r(x + w - (FL_ACCENT_COUNT - i) * (sz + gap) + gap, y + (56 - sz) / 2, sz, sz);
		int res = fl_widget(ui, ID_ACCENT + (uint32_t)i, r, FL_FOCUSABLE | FL_NO_FOCUS_RING);
		fl_color c = fl_th.dark ? fl_accents[i].dark : fl_accents[i].light;
		fl_fill_rect(&ui->cv, r, c);
		if (i == sel)
			fl_stroke_rect(&ui->cv, fl_inset(r, -4, -4), 2, fl_th.strong);
		else if (res & (FL_HOVER | FL_FOCUS))
			fl_stroke_rect(&ui->cv, fl_inset(r, -3, -3), 1, (res & FL_FOCUS) ? fl_th.strong : fl_th.dim);
		int pick = res & FL_CLICK;
		if ((res & FL_FOCUS) && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_space)) {
			ui->key = 0;
			pick = 1;
		}
		if (pick && i != sel)
			fl_conf_set("accent", fl_accents[i].id);
	}
}

/* run `flsound NAME` (does nothing when sounds are off or it is not installed) */
static void play_test(void)
{
	pid_t p = fork();
	if (p == 0) {
		setsid();
		execlp("flsound", "flsound", "--force", "notification", (char *)NULL);
		_exit(127);
	}
	if (p > 0)
		signal(SIGCHLD, SIG_IGN);
}

static void draw_general(fl_ui *ui, struct settings *s, fl_rect c)
{
	int x = c.x + 28, w = c.w - 56, y = c.y + 24;

	section(ui, s, x, y, w, "Appearance");
	y += 28;
	row_text(ui, s, x, y, w - 190, "Theme", "Colours of the panel, windows and menus");
	int dark = fl_th.dark;
	fl_rect d = fl_r(x + w - 176, y + 12, 86, 32), l = fl_r(x + w - 86, y + 12, 86, 32);
	if (choice(ui, ID_DARK, d, "Dark", dark) && !dark)
		fl_conf_set("theme", "dark");
	if (choice(ui, ID_LIGHT, l, "Light", !dark) && dark)
		fl_conf_set("theme", "light");
	y += 56;
	draw_accents(ui, s, x, y, w);
	y += 56 + 24;

	section(ui, s, x, y, w, "Panel");
	y += 28;
	row_text(ui, s, x, y, w - 60, "Show names", "Put the window's name next to its icon in the panel");
	int labels = fl_conf_int("panel_labels", 0);
	if (fl_switch(ui, ID_LABELS, fl_r(x + w - 40, y + 18, 40, 20), &labels))
		fl_conf_set("panel_labels", labels ? "1" : "0");
	y += 56 + 24;

	section(ui, s, x, y, w, "Sounds");
	y += 28;
	row_text(ui, s, x, y, w - 150, "System sounds", "Quiet sounds for sign-in, alerts, volume and screenshots");
	int snd = fl_conf_int("sounds", 1);
	if (fl_switch(ui, ID_SOUNDS, fl_r(x + w - 40, y + 18, 40, 20), &snd))
		fl_conf_set("sounds", snd ? "1" : "0");
	if (snd && fl_button(ui, ID_SOUND_TEST, fl_r(x + w - 40 - 16 - 72, y + 12, 72, 32), "Test", FL_BTN_OUTLINE))
		play_test();
}

/* The pages of pages.c can be taller than the window: they scroll (wheel or bar). Widgets that
 * are scrolled out of the content area must not see the pointer, so it is hidden from them
 * while the pointer is somewhere else. */
static void draw_scrolled(fl_ui *ui, struct settings *s, fl_rect view)
{
	int sbw = s->content_h > view.h ? 10 : 0, max = s->content_h > view.h ? s->content_h - view.h : 0;
	if (ui->wheel && fl_in(view, ui->mx, ui->my) && max) {
		s->scroll += ui->wheel * 40;
		ui->wheel = 0;
	}
	if (s->scroll > max)
		s->scroll = max;
	if (s->scroll < 0)
		s->scroll = 0;
	fl_rect old_clip = ui->cv.clip;
	int mx = ui->mx, my = ui->my;
	fl_clip(&ui->cv, view);
	if (!fl_in(view, mx, my))
		ui->mx = ui->my = -10000;
	fl_rect c = fl_r(view.x, view.y - s->scroll, view.w - sbw, view.h);
	if (s->page == PAGE_DATETIME)
		s->content_h = dt_draw(s->dt, ui, c);
	else if (s->page == PAGE_SNAPSHOTS)
		s->content_h = snap_draw(s->snap, ui, c);
	else if (s->page == PAGE_ABOUT)
		s->content_h = about_draw(s->about, ui, c);
	else
		s->content_h = pages_draw(s->pg, s->page - PAGE_DISPLAY, ui, c);
	ui->mx = mx;
	ui->my = my;
	if (s->content_h > view.h) {
		int off = s->scroll;
		fl_scrollbar(ui, ID_SCROLL, fl_r(view.x + view.w - 10, view.y, 10, view.h), &off, s->content_h, view.h);
		s->scroll = off;
	}
	ui->cv.clip = old_clip;
}

static int sidebar_item(fl_ui *ui, uint32_t id, fl_rect r, fl_icon ic, const char *label, int on)
{
	int res = fl_widget(ui, id, r, FL_FOCUSABLE);
	fl_fill_rect(&ui->cv, r, on ? fl_th.select : (res & FL_HOVER) ? fl_th.raised : 0);
	if (on)
		fl_fill_rect(&ui->cv, fl_r(r.x, r.y, 3, r.h), fl_th.accent);
	if (res & FL_FOCUS)
		fl_stroke_rect(&ui->cv, r, 1, fl_th.accent);
	fl_icon_draw(&ui->cv, ic, r.x + 16, r.y + (r.h - 16) / 2, 16, on ? fl_th.accent : fl_th.dim);
	fl_text_in(&ui->cv, on ? ui->f_med : ui->f_reg, fl_r(r.x + 44, r.y, r.w - 44, r.h), FL_LEFT, 0,
	           on ? fl_th.strong : fl_th.text, label);
	if ((res & FL_FOCUS) && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_space)) {
		ui->key = 0;
		return 1;
	}
	return res & FL_CLICK;
}

static void draw(fl_ui *ui, void *ud)
{
	struct settings *s = ud;
	fl_rect c = fl_win_content(ui->win);
	fl_fill_rect(&ui->cv, c, fl_th.panel);
	fl_rect side = fl_cut_left(&c, SIDEBAR);
	fl_fill_rect(&ui->cv, side, fl_th.bg);
	fl_fill_rect(&ui->cv, fl_r(side.x + side.w - 1, side.y, 1, side.h), fl_th.line);
	static const struct { int page, id; fl_icon icon; const char *name; } items[] = {
		{PAGE_GENERAL, ID_PAGE_GENERAL, FL_ICON_SETTINGS, "General"},
		{PAGE_DISPLAY, ID_PAGE_DISPLAY, FL_ICON_DISPLAY, "Display"},
		{PAGE_INPUT, ID_PAGE_INPUT, FL_ICON_MOUSE, "Mouse & touchpad"},
		{PAGE_KEYBOARD, ID_PAGE_KEYBOARD, FL_ICON_KEYBOARD, "Keyboard"},
		{PAGE_DATETIME, ID_PAGE_DATETIME, FL_ICON_CLOCK, "Date & time"},
		{PAGE_WIFI, ID_PAGE_WIFI, FL_ICON_WIFI, "Wi-Fi"},
		{PAGE_SNAPSHOTS, ID_PAGE_SNAPSHOTS, FL_ICON_DRIVE, "Snapshots"},
		{PAGE_ABOUT, ID_PAGE_ABOUT, FL_ICON_DOC, "About"},
	};
	for (int i = 0; i < (int)(sizeof items / sizeof items[0]); i++)
		if (sidebar_item(ui, (uint32_t)items[i].id, fl_r(side.x, side.y + 12 + i * 40, side.w - 1, 40), items[i].icon,
		                 items[i].name, s->page == items[i].page))
			set_page(s, items[i].page);
	if (s->page == PAGE_WIFI) {
		wifi_draw(s->wifi, ui, c);
	} else if (s->page == PAGE_GENERAL) {
		draw_general(ui, s, c);
	} else {
		draw_scrolled(ui, s, c);
	}
}

/* ---- one Settings window: later starts hand their page over ----------------------------------- */

static int sock_path(struct sockaddr_un *a)
{
	const char *rt = getenv("XDG_RUNTIME_DIR");
	memset(a, 0, sizeof *a);
	a->sun_family = AF_UNIX;
	if (!rt || !*rt || snprintf(a->sun_path, sizeof a->sun_path, "%s/flsettings.sock", rt) >= (int)sizeof a->sun_path)
		return -1;
	return 0;
}

static int page_by_name(const char *n)
{
	return !strcmp(n, "wifi") || !strcmp(n, "network") ? PAGE_WIFI
	     : !strcmp(n, "display") ? PAGE_DISPLAY
	     : !strcmp(n, "mouse") || !strcmp(n, "input") || !strcmp(n, "touchpad") ? PAGE_INPUT
	     : !strcmp(n, "keyboard") ? PAGE_KEYBOARD
	     : !strcmp(n, "datetime") || !strcmp(n, "time") || !strcmp(n, "clock") || !strcmp(n, "timezone") ? PAGE_DATETIME
	     : !strcmp(n, "snapshots") || !strcmp(n, "snapshot") || !strcmp(n, "rollback") ? PAGE_SNAPSHOTS
	     : !strcmp(n, "about") || !strcmp(n, "report") ? PAGE_ABOUT
	     : PAGE_GENERAL;
}

/* 1: an older Settings took the page and this process should exit */
static int hand_over(int page)
{
	struct sockaddr_un a;
	if (sock_path(&a) < 0)
		return 0;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return 0;
	if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
		close(fd);
		return 0;
	}
	static const char *const names[] = {"general", "display", "mouse", "keyboard", "wifi", "datetime", "snapshots"};
	char msg[32];
	snprintf(msg, sizeof msg, "page %s\n", names[page]);
	int ok = write(fd, msg, strlen(msg)) > 0;
	close(fd);
	return ok;
}

static void on_client(void *ud, int fd, short rev)
{
	struct settings *s = ud;
	int c = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
	char buf[64] = "";
	if (c < 0)
		return;
	ssize_t n = read(c, buf, sizeof buf - 1);
	close(c);
	if (n > 5 && !strncmp(buf, "page ", 5)) {
		buf[n] = 0;
		buf[strcspn(buf, "\n")] = 0;
		set_page(s, page_by_name(buf + 5));
	}
}

static void listen_for_pages(struct settings *s)
{
	struct sockaddr_un a;
	s->sock = -1;
	if (sock_path(&a) < 0)
		return;
	unlink(a.sun_path); /* stale: hand_over found nobody behind it */
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;
	if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 4) < 0) {
		close(fd);
		return;
	}
	s->sock = fd;
	fl_fd_add(s->app, fd, POLLIN, on_client, s);
}

int main(int argc, char **argv)
{
	int page = PAGE_GENERAL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--page") && i + 1 < argc) {
			page = page_by_name(argv[++i]);
		} else if (!strcmp(argv[i], "--set-wallpaper") && i + 1 < argc) {
			/* PNG or JPEG in, wallpaper= out; no window (the image viewer and scripts use it) */
			char err[200];
			fl_conf_load();
			if (wp_set_from_file(argv[i + 1], err, sizeof err) < 0) {
				fprintf(stderr, "flsettings: %s\n", err);
				return 1;
			}
			return 0;
		} else {
			fprintf(stderr, "usage: flsettings [--page general|display|mouse|keyboard|datetime|wifi|snapshots|about]\n"
			                "       flsettings --set-wallpaper FILE.png|FILE.jpg\n"
			                "Theme, panel labels, display, mouse and touchpad, keyboard, date and time, Wi-Fi\n");
			return !strcmp(argv[i], "-h") || !strcmp(argv[i], "--help") ? 0 : 2;
		}
	}
	if (!getenv("FLUI_PPM") && hand_over(page))
		return 0;
	struct settings s = {0};
	s.page = page;
	s.sock = -1;
	s.pg = pages_new();
	if (!s.pg)
		return 1;
	s.app = fl_app_new(on_global, &s);
	if (!s.app) {
		fprintf(stderr, "flsettings: cannot connect to a Wayland compositor\n");
		return 1;
	}
	s.head = fl_font_ui(500, 11);
	s.small = fl_font_ui(400, 12);

	fl_win_opts o = {
		.title = "Settings", .app_id = "flsettings", .w = 780, .h = 640, .min_w = 660, .min_h = 460,
		.decorated = 1, .resizable = 1, .draw = draw, .ud = &s,
	};
	pages_start(s.pg, s.app, s.head, s.small);
	const char *sz = getenv("FLSETTINGS_SIZE"); /* WxH, for screenshots */
	if (sz && getenv("FLUI_PPM"))
		sscanf(sz, "%dx%d", &o.w, &o.h);
	fl_win *w = fl_win_new(s.app, &o);
	if (!w)
		return 1;
	s.win = w;
	pages_set_win(s.pg, w);
	if (s.page >= PAGE_DISPLAY && s.page <= PAGE_KEYBOARD)
		pages_show(s.pg, s.page - PAGE_DISPLAY);
	s.wifi = wifi_new(s.app, s.head, s.small);
	wifi_set_win(s.wifi, w);
	s.dt = dt_new(s.app, s.head, s.small);
	dt_set_win(s.dt, w);
	dt_show(s.dt, s.page == PAGE_DATETIME);
	s.snap = snap_new(s.app, s.head, s.small);
	snap_set_win(s.snap, w);
	snap_show(s.snap, s.page == PAGE_SNAPSHOTS);
	s.about = about_new(s.app, s.head, s.small);
	about_set_win(s.about, w);
	if (s.page == PAGE_ABOUT)
		about_show(s.about);
	wifi_show(s.wifi, s.page == PAGE_WIFI);
	if (!getenv("FLUI_PPM"))
		listen_for_pages(&s);
	fl_app_watch_conf(s.app, NULL, NULL);
	fl_app_run(s.app);
	if (s.sock >= 0) {
		struct sockaddr_un a;
		if (sock_path(&a) == 0)
			unlink(a.sun_path);
		close(s.sock);
	}
	wifi_free(s.wifi);
	dt_free(s.dt);
	snap_free(s.snap);
	about_free(s.about);
	pages_free(s.pg);
	fl_app_free(s.app);
	return 0;
}
