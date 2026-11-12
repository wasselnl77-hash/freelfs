/* flpanel -- the bottom panel: start button, windows, status, clock
 * SPDX-License-Identifier: MIT
 *
 * Layout of the mockup Panel.dc.html: 44 px, start button on the left, one
 * entry per window (icon, or icon and name with panel_labels=1), then the
 * keyboard layout, Wi-Fi, volume, battery and the clock on the right. The
 * start menu is in menu.c; both live in one process and one fl_shell binding.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <flui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <wayland-client.h>

#include "flpanel.h"
#include "sound.h"

/* panel_height= in desktop.conf (36..64, default 44), live: libflui reads it into fl_lk */
#define PANEL_H (fl_lk.panel_h)
/* the icon size of the entries (panel_icon_size=, 16..28, default 20) */
#define ICON_PX (fl_lk.panel_icon)

enum { ID_START = 1, ID_KBD, ID_WIFI, ID_VOL, ID_BAT, ID_CLOCK, ID_WIN = 100, ID_WS = 2000 };

/* ---- helpers --------------------------------------------------------------------------- */

static const char *const layout_ids[4] = {"de", "us", "uk", "fr"};

/* name shown in the indicator for what the compositor reports */
static void layout_label(const char *name, char *out, size_t cap)
{
	const char *n = name;
	if (!strncmp(n, "gb", 2))
		n = "uk";
	size_t i = 0;
	for (; n[i] && n[i] != ':' && i + 1 < cap; i++)
		out[i] = (char)((n[i] >= 'a' && n[i] <= 'z') ? n[i] - 32 : n[i]);
	out[i] = 0;
}

/* ---- the tray: battery, network, volume ----------------------------------------------------------
 *
 * Battery and network come from /sys and /proc: a few small reads every 5 s,
 * redrawn only when something changed. Volume asks wpctl (WirePlumber) in a
 * child process, never waiting for it: the answer arrives on a pipe. It is
 * asked at start, after you change it, and every 25 s; when wpctl keeps
 * failing (no PipeWire) the panel backs off to once a minute. Idle cost is
 * one timer wakeup every 5 s. */

static int read_word_file(const char *path, char *out, size_t cap)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(out, (int)cap, f))
		out[0] = 0;
	fclose(f);
	out[strcspn(out, "\r\n")] = 0;
	return 0;
}

/* Returns 1 when anything the panel shows changed. */
static int tray_read_sys(struct panel *p)
{
	int kind = 0, up = 0;
	struct dirent *e;
	DIR *d;
	int bat_changed = battery_read(p); /* battery.c: sets bat_pct and bat_charging */

	int wifi_seen = 0, wired_seen = 0, wifi_up = 0, wired_up = 0;
	d = opendir("/sys/class/net");
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.' || !strcmp(e->d_name, "lo") || !strncmp(e->d_name, "veth", 4) ||
		    !strncmp(e->d_name, "docker", 6) || !strncmp(e->d_name, "br-", 3))
			continue;
		char path[300], st[32] = "";
		snprintf(path, sizeof path, "/sys/class/net/%s/wireless", e->d_name);
		int wireless = access(path, F_OK) == 0;
		snprintf(path, sizeof path, "/sys/class/net/%s/device", e->d_name);
		if (!wireless && access(path, F_OK) != 0)
			continue; /* virtual interface (tun, bridge, ...) */
		snprintf(path, sizeof path, "/sys/class/net/%s/operstate", e->d_name);
		read_word_file(path, st, sizeof st);
		int isup = !strcmp(st, "up") || (!strcmp(st, "unknown") && !wireless);
		if (wireless) {
			wifi_seen = 1;
			wifi_up |= isup;
		} else {
			wired_seen = 1;
			wired_up |= isup;
		}
	}
	if (d)
		closedir(d);
	if (wifi_up) {
		kind = 1, up = 1;
	} else if (wired_up) {
		kind = 2, up = 1;
	} else if (wifi_seen) {
		kind = 1;
	} else if (wired_seen) {
		kind = 2;
	}
	int changed = bat_changed || kind != p->net_kind || up != p->net_up;
	p->net_kind = kind;
	p->net_up = up;
	return changed;
}

static void vol_parse(struct panel *p)
{
	/* "Volume: 0.62" or "Volume: 0.62 [MUTED]" */
	double v;
	int pct = -1, muted = 0;
	p->vol_buf[p->vol_len < sizeof p->vol_buf ? p->vol_len : sizeof p->vol_buf - 1] = 0;
	if (sscanf(p->vol_buf, "Volume: %lf", &v) == 1) {
		pct = (int)(v * 100 + 0.5);
		muted = strstr(p->vol_buf, "[MUTED]") != NULL;
		p->vol_fails = 0;
	} else if (p->vol_fails < 100) {
		p->vol_fails++;
	}
	/* what the icon already shows (set optimistically) stays unless the reading differs */
	if (pct != p->vol_pct || muted != p->vol_muted) {
		p->vol_pct = pct;
		p->vol_muted = muted;
		fl_win_redraw(p->pw);
		if (p->fly_open)
			fl_win_redraw(p->fw);
	}
}

/* One wpctl child at a time, a question (get-volume) or a change (set-volume,
 * set-mute). Changes made in the panel show at once and are queued as the
 * latest wanted values (vol_want_pct, vol_want_mute), so a drag becomes a few
 * calls, not one per pixel. After the last change comes one more question to
 * correct the display if wpctl ended up elsewhere. */
static void vol_pump(struct panel *p);

static void vol_job_end(struct panel *p, int killit)
{
	if (p->vol_fd < 0)
		return;
	if (killit && p->vol_pid > 0)
		kill(p->vol_pid, SIGKILL);
	fl_fd_remove(p->app, p->vol_fd);
	close(p->vol_fd);
	p->vol_fd = -1;
	p->vol_pid = 0;
}

static void vol_ready(void *ud, int fd, short rev)
{
	struct panel *p = ud;
	char tmp[128];
	for (;;) {
		ssize_t r = read(fd, tmp, sizeof tmp);
		if (r > 0) {
			size_t room = sizeof p->vol_buf - 1 - p->vol_len;
			size_t n = (size_t)r < room ? (size_t)r : room;
			memcpy(p->vol_buf + p->vol_len, tmp, n);
			p->vol_len += n;
			continue;
		}
		if (r < 0 && (errno == EAGAIN || errno == EINTR))
			return; /* more to come */
		break;
	}
	int job = p->vol_job;
	vol_job_end(p, 0);
	p->vol_job = VJ_NONE;
	if (job == VJ_QUERY) {
		if (!p->vol_stale)
			vol_parse(p); /* else: a change was made meanwhile, this answer is old */
		p->vol_stale = 0;
	} else {
		p->vol_want_query = 1; /* look again after the changes */
	}
	vol_pump(p);
}

static void vol_spawn(struct panel *p, int job, const char *const *args)
{
	int fds[2];
	if (pipe2(fds, O_CLOEXEC) < 0)
		return;
	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return;
	}
	if (pid == 0) {
		int nul = open("/dev/null", O_RDWR);
		dup2(fds[1], 1);
		if (nul >= 0) {
			dup2(nul, 0);
			dup2(nul, 2);
		}
		setsid();
		signal(SIGCHLD, SIG_DFL);
		execvp("wpctl", (char *const *)args);
		_exit(127);
	}
	close(fds[1]);
	fcntl(fds[0], F_SETFL, O_NONBLOCK);
	p->vol_fd = fds[0];
	p->vol_pid = pid;
	p->vol_job = job;
	p->vol_len = 0;
	p->vol_age = 0;
	p->vol_stale = 0;
	fl_fd_add(p->app, fds[0], POLLIN, vol_ready, p);
}

static void vol_pump(struct panel *p)
{
	if (p->vol_fd >= 0)
		return;
	if (p->demo) {
		p->vol_want_pct = p->vol_want_mute = -1;
		p->vol_want_query = 0;
		return;
	}
	char arg[16];
	if (p->vol_want_pct >= 0) {
		snprintf(arg, sizeof arg, "%d%%", p->vol_want_pct);
		p->vol_want_pct = -1;
		const char *const a[] = {"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", arg, NULL};
		vol_spawn(p, VJ_SET, a);
		panel_sound("tick");	/* the slider steps (flsound limits how often) */
	} else if (p->vol_want_mute >= 0) {
		const char *const a[] = {"wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@",
		                         p->vol_want_mute ? "1" : "0", NULL};
		p->vol_want_mute = -1;
		vol_spawn(p, VJ_SET, a);
	} else if (p->vol_want_query) {
		const char *const a[] = {"wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@", NULL};
		p->vol_want_query = 0;
		vol_spawn(p, VJ_QUERY, a);
	}
}

static void vol_query(struct panel *p)
{
	p->vol_want_query = 1;
	vol_pump(p);
}

/* the user changed something: the display follows now, wpctl later */
static void vol_touch(struct panel *p)
{
	if (p->vol_job == VJ_QUERY)
		p->vol_stale = 1;
	fl_win_redraw(p->pw);
	if (p->fly_open)
		fl_win_redraw(p->fw);
	vol_pump(p);
}

/* The one indicator is the compositor's (icon and bar, fades out by itself). Every volume
 * change shows it: the media keys, the wheel on the icon and the flyout's slider and mute
 * button, so they all look the same. A drag sends one per step; each replaces the last. */
static void vol_osd(struct panel *p)
{
	if (p->shell && p->shell_ver >= 3 && p->vol_pct >= 0)
		fl_shell_show_osd(p->shell, 0, (uint32_t)p->vol_pct, p->vol_muted ? 1 : 0);
}

void vol_set_pct(struct panel *p, int pct)
{
	if (p->vol_pct < 0) { /* no reading yet (or no PipeWire): ask again instead */
		vol_query(p);
		return;
	}
	pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
	if (pct == p->vol_pct && !p->vol_muted) {
		vol_osd(p); /* at the end stop: the key or wheel still answers */
		return;
	}
	p->vol_pct = pct;
	p->vol_want_pct = pct;
	if (p->vol_muted) { /* changing the level of a muted output unmutes it */
		p->vol_muted = 0;
		p->vol_want_mute = 0;
	}
	vol_touch(p);
	vol_osd(p);
}

void vol_toggle_mute(struct panel *p)
{
	if (p->vol_pct < 0) {
		vol_query(p);
		return;
	}
	p->vol_muted = !p->vol_muted;
	p->vol_want_mute = p->vol_muted;
	vol_touch(p);
	vol_osd(p);
}

static void tray_tick(void *ud)
{
	struct panel *p = ud;
	if (tray_read_sys(p))
		fl_win_redraw(p->pw);
	p->tray_tick++;
	/* the volume: every 25 s, or once a minute when wpctl keeps failing */
	int every = p->vol_fails >= 3 ? 12 : 5;
	if (p->vol_fd >= 0 && ++p->vol_age >= 2) { /* it never came back */
		int job = p->vol_job;
		vol_job_end(p, 1);
		p->vol_job = VJ_NONE;
		if (job == VJ_QUERY && p->vol_fails < 100)
			p->vol_fails++;
		vol_pump(p);
	}
	if (p->tray_tick % every == 0)
		vol_query(p);
}

void launch(const char *exec, int terminal)
{
	char cmd[512];
	size_t n = 0;
	/* drop the %f %u ... field codes of the Exec key */
	for (const char *s = exec; *s && n + 2 < sizeof cmd; s++) {
		if (*s == '%' && s[1]) {
			if (s[1] == '%')
				cmd[n++] = '%';
			s++;
			continue;
		}
		cmd[n++] = *s;
	}
	cmd[n] = 0;
	pid_t pid = fork();
	if (pid != 0)
		return;
	setsid();
	signal(SIGCHLD, SIG_DFL);
	if (terminal) /* in flterm; a failing program leaves its window open for a look */
		execlp("flterm", "flterm", "-e", "sh", "-c",
		       "eval \"$1\"; r=$?; [ $r = 0 ] || { printf '\\n[exit status %d, press Enter]' $r; read x; }",
		       "sh", cmd, (char *)NULL);
	else /* the program's messages go to ~/.local/state/freelfs/log/NAME.log (flreport collects them) */
		execl("/bin/sh", "sh", "-c",
		      "d=${XDG_STATE_HOME:-$HOME/.local/state}/freelfs/log;"
		      "n=${1%% *}; n=${n##*/}; case $n in flatpak) n=${1##* };; esac; n=${n:-app};"
		      "if mkdir -p \"$d\" 2>/dev/null; then f=$d/$n.log;"
		      " if [ -f \"$f\" ] && [ \"$(wc -c < \"$f\")\" -gt 1000000 ]; then mv -f \"$f\" \"$f.1\"; fi;"
		      " { echo \"--- $(date '+%F %T') $1\" >> \"$f\"; } 2>/dev/null && exec 2>> \"$f\"; fi;"
		      "eval \"$1\"",
		      "sh", cmd, (char *)NULL);
	_exit(127);
}

static void spawn(const char *prog)
{
	launch(prog, 0);
}

/* ---- the shell protocol ------------------------------------------------------------------- */

static struct win *find_win(struct panel *p, uint32_t id)
{
	for (int i = 0; i < p->nwins; i++)
		if (p->wins[i].id == id)
			return &p->wins[i];
	return NULL;
}

static void flyout_open(struct panel *p);

static void sh_added(void *d, struct fl_shell *s, uint32_t id, const char *app_id, const char *title)
{
	struct panel *p = d;
	struct win *w = realloc(p->wins, (size_t)(p->nwins + 1) * sizeof(*w));
	if (!w)
		return;
	p->wins = w;
	w = &p->wins[p->nwins++];
	memset(w, 0, sizeof(*w));
	w->id = id;
	snprintf(w->app_id, sizeof w->app_id, "%s", app_id ? app_id : "");
	snprintf(w->title, sizeof w->title, "%s", title ? title : "");
	fl_win_redraw(p->pw);
}

static void sh_changed(void *d, struct fl_shell *s, uint32_t id, const char *title, uint32_t focused,
                       uint32_t minimized)
{
	struct panel *p = d;
	struct win *w = find_win(p, id);
	if (!w)
		return;
	snprintf(w->title, sizeof w->title, "%s", title ? title : "");
	w->focused = (int)focused;
	w->minimized = (int)minimized;
	fl_win_redraw(p->pw);
}

static void sh_removed(void *d, struct fl_shell *s, uint32_t id)
{
	struct panel *p = d;
	for (int i = 0; i < p->nwins; i++)
		if (p->wins[i].id == id) {
			memmove(&p->wins[i], &p->wins[i + 1], (size_t)(p->nwins - i - 1) * sizeof(*p->wins));
			p->nwins--;
			break;
		}
	fl_win_redraw(p->pw);
}

static void sh_dismissed(void *d, struct fl_shell *s)
{
	struct panel *p = d;
	if (p->menu_open)
		menu_close(p);
}

static void sh_flyout_dismissed(void *d, struct fl_shell *s)
{
	panel_flyouts_close(d, FLY_NONE);
}

static void sh_volume_key(void *d, struct fl_shell *s, uint32_t action)
{
	struct panel *p = d;
	if (action == 2)
		vol_toggle_mute(p);
	else
		vol_set_pct(p, p->vol_pct + (action == 0 ? 5 : -5));
	/* vol_set_pct and vol_toggle_mute show the indicator themselves */
}

static void sh_toggle(void *d, struct fl_shell *s)
{
	struct panel *p = d;
	if (p->menu_open)
		menu_close(p);
	else
		menu_open(p);
}

static void sh_layout(void *d, struct fl_shell *s, const char *name)
{
	struct panel *p = d;
	snprintf(p->layout, sizeof p->layout, "%s", name);
	fl_win_redraw(p->pw);
}

/* Version 3 sends the display's modes and state to every binding (Settings' Display page uses
 * them). The panel has no use for them, but libwayland aborts on an event without a listener. */
static void sh_output_mode(void *d, struct fl_shell *s, uint32_t w, uint32_t h, uint32_t hz, uint32_t flags) {}
static void sh_output_state(void *d, struct fl_shell *s, uint32_t scale, uint32_t w, uint32_t h, uint32_t pending) {}
static void sh_output_failed(void *d, struct fl_shell *s) {}

/* the screen was locked or unlocked: the cards wait while it is locked */
static void sh_lock_state(void *d, struct fl_shell *s, uint32_t locked)
{
	struct panel *p = d;
	if (locked)
		panel_flyouts_close(p, FLY_NONE);
	notify_locked(p, (int)locked);
}

/* Version 5: the workspace grid and the one in view; and the workspace of every window. */
static void sh_workspaces(void *d, struct fl_shell *s, uint32_t cols, uint32_t rows, uint32_t cur)
{
	struct panel *p = d;
	p->ws_cols = cols > 8 ? 8 : (int)cols;
	p->ws_rows = rows > 8 ? 8 : (int)rows;
	p->ws_cur = (int)cur;
	fl_win_redraw(p->pw);
}

static void sh_window_ws(void *d, struct fl_shell *s, uint32_t id, uint32_t idx)
{
	struct panel *p = d;
	struct win *w = find_win(p, id);
	if (w) {
		w->ws = (int)idx;
		fl_win_redraw(p->pw);
	}
}

static const struct fl_shell_listener shell_listener = {
	.window_added = sh_added, .window_changed = sh_changed, .window_removed = sh_removed,
	.menu_dismissed = sh_dismissed, .menu_toggle = sh_toggle, .keyboard_layout = sh_layout,
	.flyout_dismissed = sh_flyout_dismissed, .volume_key = sh_volume_key,
	.output_mode = sh_output_mode, .output_state = sh_output_state, .output_failed = sh_output_failed,
	.lock_state = sh_lock_state, .workspaces = sh_workspaces, .window_workspace = sh_window_ws,
};

static void on_global(void *ud, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver)
{
	struct panel *p = ud;
	if (!strcmp(iface, "fl_shell")) {
		p->shell_ver = ver >= 7 ? 7 : ver >= 6 ? 6 : ver >= 5 ? 5 : ver >= 4 ? 4 : ver >= 3 ? 3 : ver >= 2 ? 2 : 1;
		p->shell = wl_registry_bind(reg, name, &fl_shell_interface, (uint32_t)p->shell_ver);
		fl_shell_add_listener(p->shell, &shell_listener, p);
	}
}

/* ---- menu and flyout open / close ------------------------------------------------------------- */

/* One flyout at a time: the volume slider, the battery, the notification centre. */
void panel_flyouts_close(struct panel *p, int except)
{
	if (except != FLY_VOL)
		flyout_close(p);
	if (except != FLY_BAT)
		battery_flyout_close(p);
	if (except != FLY_NC)
		notify_center_close(p);
	if (except != FLY_WS)
		wsmenu_close(p);
}

void panel_open_flyout(struct panel *p, int kind, fl_win *w, int cx)
{
	panel_flyouts_close(p, kind);
	if (p->menu_open)
		menu_close(p);
	fl_shell_set_flyout(p->shell, fl_win_surface(w), cx);
	fl_win_map(w);
}

static void flyout_open(struct panel *p)
{
	if (p->fly_open || !p->fw || !p->shell || p->shell_ver < 2)
		return;
	p->fly_open = 1;
	panel_open_flyout(p, FLY_VOL, p->fw, p->vol_cx);
	fl_win_ui(p->fw)->focus = 2; /* the slider */
	fl_win_redraw(p->pw);
	vol_query(p); /* the slider starts from what the system says now */
}

void flyout_close(struct panel *p)
{
	if (!p->fly_open)
		return;
	p->fly_open = 0;
	if (p->fw)
		panel_hide(p->fw);
	fl_win_redraw(p->pw);
}

void menu_open(struct panel *p)
{
	if (p->menu_open || !p->mw)
		return;
	panel_flyouts_close(p, FLY_NONE);
	p->menu_open = 1;
	menu_init(p);
	fl_win_map(p->mw);
	fl_win_redraw(p->pw);
}

void menu_close(struct panel *p)
{
	if (!p->menu_open)
		return;
	p->menu_open = 0;
	if (p->mw)
		fl_win_hide(p->mw);
	fl_win_redraw(p->pw);
}

/* ---- drawing -------------------------------------------------------------------------------- */

static void clock_text(struct panel *p, char *out, size_t cap)
{
	if (p->demo) {
		snprintf(out, cap, "Thu 5 Nov  14:32");
		return;
	}
	time_t t = time(NULL);
	struct tm tm;
	tzset(); /* localtime_r does not look at /etc/localtime again; Settings > Date & time changes it */
	localtime_r(&t, &tm);
	strftime(out, cap, fl_conf_int("clock_24h", 1) ? "%a %-d %b  %H:%M" : "%a %-d %b  %-I:%M %p", &tm);
}

static void clock_arm(struct panel *p)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long ms = (60 - ts.tv_sec % 60) * 1000L - ts.tv_nsec / 1000000L + 30;
	fl_timer_arm(p->clock, (int)ms, 0);
}

static void clock_tick(void *ud)
{
	struct panel *p = ud;
	clock_arm(p);
	fl_win_redraw(p->pw);
}

static void draw_panel(fl_ui *ui, void *ud)
{
	struct panel *p = ud;
	fl_canvas *cv = &ui->cv;
	int W = cv->w;
	int labels = fl_conf_int("panel_labels", 0);
	fl_fill_rect(cv, fl_r(0, 0, W, PANEL_H), fl_th.panel);
	/* the hairline is on the edge that faces the desktop: the top edge for a panel at the bottom */
	fl_fill_rect(cv, fl_r(0, fl_lk.panel_top ? PANEL_H - 1 : 0, W, 1), fl_th.line);

	/* start button */
	{
		fl_rect r = fl_r(0, 1, 52, PANEL_H - 1);
		int res = fl_widget(ui, ID_START, r, FL_FOCUSABLE);
		fl_color bg = fl_th.accent;
		if (res & FL_DOWN)
			bg = fl_mix(bg, 0xff000000, 40);
		else if (res & FL_HOVER)
			bg = fl_mix(bg, 0xffffffff, 28);
		fl_fill_rect(cv, r, bg);
		if (res & FL_FOCUS)
			fl_stroke_rect(cv, fl_inset(r, 3, 3), 1, fl_th.on_accent);
		fl_icon_draw(cv, FL_ICON_MENU, r.x + (r.w - ICON_PX) / 2, r.y + (r.h - ICON_PX) / 2, ICON_PX, fl_th.on_accent);
		if ((res & FL_CLICK) || ((res & FL_FOCUS) && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_space))) {
			ui->key = 0;
			if (p->menu_open)
				menu_close(p);
			else
				menu_open(p);
		}
	}

	/* the right side first, so the window list knows how much room it has. The clock is a button:
	 * it opens the notification centre; left of the time a badge shows the unread ones, or a
	 * slashed bell when Do not disturb is on. */
	char clk[48];
	clock_text(p, clk, sizeof clk);
	int cw = fl_text_width(ui->f_reg, clk, -1);
	int unread = notify_unread(p), dnd = notify_dnd(p);
	char badge[8] = "";
	int ind_w = 0;
	if (dnd)
		ind_w = 26;
	else if (unread > 0) {
		snprintf(badge, sizeof badge, "%d", unread > 99 ? 99 : unread);
		ind_w = fl_text_width(p->f_small, badge, -1) + 14 + 10;
		if (ind_w < 26)
			ind_w = 26;
	}
	int bw = 10 + ind_w + cw + 14;
	fl_rect clr = fl_r(W - bw, 1, bw, PANEL_H - 1);
	int cres = fl_widget(ui, ID_CLOCK, clr, FL_FOCUSABLE);
	int copen = notify_center_is_open(p);
	if (cres & (FL_HOVER | FL_DOWN) || copen)
		fl_fill_rect(cv, clr, fl_th.raised);
	if (cres & FL_FOCUS)
		fl_stroke_rect(cv, clr, 1, fl_th.accent);
	p->clock_cx = clr.x + clr.w / 2;
	if (dnd) {
		draw_bell(cv, clr.x + 10, 1 + (PANEL_H - 1 - 16) / 2, 16, fl_th.dim);
		fl_path *sl = fl_path_new();
		if (sl) {
			fl_path_move(sl, (float)clr.x + 11, (float)(1 + (PANEL_H - 1 - 16) / 2) + 15);
			fl_path_line(sl, (float)clr.x + 25, (float)(1 + (PANEL_H - 1 - 16) / 2) + 1);
			fl_path_stroke(cv, sl, 1.4f, fl_th.err);
			fl_path_free(sl);
		}
	} else if (badge[0]) {
		int bwid = ind_w - 10 - 8;
		fl_rect br = fl_r(clr.x + 10, 1 + (PANEL_H - 1 - 18) / 2, bwid, 18);
		fl_fill_rect(cv, br, fl_th.accent);
		fl_text_in(cv, p->f_small, br, FL_CENTER, 0, fl_th.on_accent, badge);
	}
	fl_text_in(cv, ui->f_reg, fl_r(clr.x + 10 + ind_w, 1, cw, PANEL_H - 1), FL_LEFT, 0, fl_th.text, clk);
	if ((cres & FL_CLICK) || ((cres & FL_FOCUS) && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_space))) {
		ui->key = 0;
		notify_center_toggle(p);
	}
	int x = clr.x - 4;

	/* tray, right to left: battery (if there is one), volume, network */
	int bat = p->bat_pct;
	for (int i = bat < 0 ? 1 : 0; i < 3; i++) {
		uint32_t id = i == 0 ? ID_BAT : i == 1 ? ID_VOL : ID_WIFI;
		char txt[16] = "";
		fl_color base = fl_th.dim;
		if (i == 0)
			snprintf(txt, sizeof txt, "%d%%", bat);
		else if (i == 1 && p->vol_pct >= 0 && !p->vol_muted)
			snprintf(txt, sizeof txt, "%d%%", p->vol_pct);
		int tw = txt[0] ? fl_text_width(p->f_small, txt, -1) + 4 : 0;
		int w = 34 + tw;
		x -= w;
		fl_rect r = fl_r(x, 1 + (PANEL_H - 1 - 34) / 2, w, 34);
		int res = fl_widget(ui, id, r, FL_FOCUSABLE);
		if (i == 1)
			p->vol_cx = r.x + r.w / 2;
		if (i == 0)
			p->bat_cx = r.x + r.w / 2;
		int open = (i == 1 && p->fly_open) || (i == 0 && battery_flyout_is_open(p));
		if (res & (FL_HOVER | FL_DOWN) || open)
			fl_fill_rect(cv, r, fl_th.raised);
		if (res & FL_FOCUS)
			fl_stroke_rect(cv, r, 1, fl_th.accent);
		fl_color c = (res & FL_HOVER) || open ? fl_th.text : base;
		int ix = r.x + (tw ? 9 : (r.w - 16) / 2), iy = r.y + 9;
		if (i == 0) {
			if (p->bat_charging)
				c = fl_th.accent;
			else if (bat <= 15)
				c = fl_th.err;
			fl_icon_battery(cv, ix - 2, iy, 16, c, bat);
			if (battery_saver(p)) /* power saving is on: a small accent mark on the icon */
				fl_fill_rect(cv, fl_r(ix + 14, iy - 3, 5, 5), fl_th.accent);
		} else if (i == 1) {
			int off = p->vol_pct < 0;
			fl_icon_draw(cv, p->vol_muted ? FL_ICON_MUTE : FL_ICON_VOLUME, ix, iy, 16, off ? fl_th.line : c);
		} else {
			fl_icon_draw(cv, p->net_kind == 2 ? FL_ICON_ETHERNET : FL_ICON_WIFI, ix, iy, 16,
			             c);
			if (!p->net_up) { /* no connection: a slash through the icon */
				fl_path *sl = fl_path_new();
				if (sl) {
					fl_path_move(sl, (float)ix + 2, (float)iy + 14);
					fl_path_line(sl, (float)ix + 14, (float)iy + 2);
					fl_path_stroke(cv, sl, 1.4f, fl_th.err);
					fl_path_free(sl);
				}
			}
		}
		if (txt[0])
			fl_text_in(cv, p->f_small, fl_r(r.x + 30, r.y, tw + 8, r.h), FL_LEFT, 0, c, txt);
		/* volume: wheel changes it, click opens the flyout, right click mutes; the
		 * others open the settings */
		if (i == 1 && (res & FL_HOVER) && ui->wheel) {
			vol_set_pct(p, p->vol_pct + (ui->wheel < 0 ? 5 : -5));
			ui->wheel = 0;
		}
		if (i == 1 && (res & FL_RCLICK))
			vol_toggle_mute(p);
		if ((res & FL_CLICK) || ((res & FL_FOCUS) && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_space))) {
			ui->key = 0;
			if (i == 0) {
				battery_flyout_toggle(p);
			} else if (i == 1) {
				if (p->fly_open)
					flyout_close(p);
				else
					flyout_open(p);
			} else {
				/* the network icon opens Settings on the Wi-Fi page. A Settings window that is
				 * already open comes to the front; the new process only tells it which page. */
				for (int k = 0; k < p->nwins && p->shell; k++)
					if (!strcmp(p->wins[k].app_id, "flsettings"))
						fl_shell_activate(p->shell, p->wins[k].id);
				spawn("flsettings --page wifi");
			}
		}
		x -= 4;
	}

	/* keyboard layout */
	{
		char lab[16];
		layout_label(p->layout, lab, sizeof lab);
		int tw = fl_text_width(p->f_small, lab, -1) + (int)strlen(lab) * 28 / 64;
		int w = tw + 14 > 34 ? tw + 14 : 34;
		x -= w;
		fl_rect r = fl_r(x, 1 + (PANEL_H - 1 - 28) / 2, w, 28);
		int res = fl_widget(ui, ID_KBD, r, FL_FOCUSABLE);
		if (res & (FL_HOVER | FL_DOWN))
			fl_fill_rect(cv, r, fl_th.raised);
		fl_stroke_rect(cv, r, 1, (res & FL_FOCUS) ? fl_th.accent : fl_th.line);
		int fh = fl_font_height(p->f_small);
		fl_text_ex(cv, p->f_small, r.x + (w - tw) / 2, r.y + (r.h - fh) / 2, fl_th.text, lab, -1, 28);
		if ((res & FL_CLICK) || ((res & FL_FOCUS) && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_space))) {
			ui->key = 0;
			int cur = 0;
			char key[16];
			snprintf(key, sizeof key, "%.2s", p->layout);
			if (!strcmp(key, "gb"))
				snprintf(key, sizeof key, "uk");
			for (int i = 0; i < 4; i++)
				if (!strcmp(layout_ids[i], key))
					cur = i;
			const char *next = fl_kbd_layouts[(cur + 1) % 4].xkb;
			if (p->shell)
				fl_shell_set_keyboard_layout(p->shell, next);
			else
				snprintf(p->layout, sizeof p->layout, "%s", next);
		}
		x -= 4;
	}
	/* the workspace pager: a small grid, the one in view filled with the accent colour, the ones with
	 * windows marked; a click goes there, the wheel steps through them, a right click opens a menu
	 * (wsmenu.c: the overview, add or remove a workspace, settings) */
	if ((p->shell_ver >= 5 || p->demo) && p->ws_cols * p->ws_rows > 1) {
		int cols = p->ws_cols, rows = p->ws_rows;
		/* up to 4 rows the cells are as big as they can be in 22 px; with more the gap is 1 px and they
		 * shrink so that 8 rows (the most Settings allows) still fit in the panel */
		int gap = rows > 4 ? 1 : 2;
		int ch = rows > 4 ? (34 - (rows - 1)) / rows : (22 - (rows - 1) * 2) / rows;
		if (ch > 14)
			ch = 14;
		if (ch < (rows > 4 ? 3 : 5))
			ch = rows > 4 ? 3 : 5;
		int cwid = ch * 8 / 5;
		int gw = cols * cwid + (cols - 1) * gap, gh = rows * ch + (rows - 1) * gap;
		int boxw = gw + 10;
		x -= boxw;
		p->ws_cx = x + boxw / 2;
		fl_rect box = fl_r(x, 1 + (PANEL_H - 1 - (gh + 8)) / 2, boxw, gh + 8);
		if (ui->mouse_in && fl_in(box, ui->mx, ui->my))
			fl_fill_rect(cv, box, fl_th.raised);
		for (int i = 0; i < cols * rows && i < 64; i++) {
			fl_rect c = fl_r(box.x + 5 + (i % cols) * (cwid + gap), box.y + 4 + (i / cols) * (ch + gap), cwid, ch);
			int res = fl_widget(ui, ID_WS + (uint32_t)i, c, 0);
			int cur = i == p->ws_cur, used = 0;
			for (int k = 0; k < p->nwins; k++)
				used += p->wins[k].ws == i;
			fl_color bg = cur ? fl_th.accent : fl_th.raised;
			if (!cur && (res & (FL_HOVER | FL_DOWN)))
				bg = fl_mix(bg, 0xffffffff, 36);
			fl_fill_rect(cv, c, bg);
			if (!cur)
				fl_stroke_rect(cv, c, 1, (res & FL_HOVER) ? fl_th.dim : fl_th.line);
			if (used && cwid >= 8 && ch >= 6) /* a window in it */
				fl_fill_rect(cv, fl_inset(c, cwid / 4 + 1, ch / 4 + 1), cur ? fl_th.on_accent : fl_th.dim);
			if ((res & FL_CLICK) && p->shell && p->shell_ver >= 5)
				fl_shell_set_workspace(p->shell, (uint32_t)i);
		}
		int bres = fl_widget(ui, ID_WS + 64, box, 0); /* after the cells: they take the press */
		if (bres & FL_HOVER) {
			if (ui->wheel && p->shell && p->shell_ver >= 5) {
				int n = p->ws_cols * p->ws_rows, to = p->ws_cur + (ui->wheel > 0 ? 1 : -1);
				if (to >= 0 && to < n)
					fl_shell_set_workspace(p->shell, (uint32_t)to);
				ui->wheel = 0;
			}
			if (bres & FL_RCLICK)
				wsmenu_toggle(p);	/* overview, add or remove a workspace, settings */
		}
		x -= 4;
	}
	int limit = x - 8;

	/* windows */
	int wx = 52 + 6;
	for (int i = 0; i < p->nwins; i++) {
		struct win *w = &p->wins[i];
		int focused = w->focused && !w->minimized && !p->menu_open;
		const char *name = w->title[0] ? w->title : w->app_id;
		int tw = labels ? fl_text_width(ui->f_reg, name, -1) : 0;
		int ew = labels ? 12 + ICON_PX + 8 + tw + 12 : ICON_PX + 24;
		if (ew < 36)
			ew = 36;
		if (ew > 180)
			ew = 180;
		if (wx + ew > limit)
			break;
		fl_rect r = fl_r(wx, 1, ew, PANEL_H - 1);
		int res = fl_widget(ui, ID_WIN + (uint32_t)i, r, FL_FOCUSABLE);
		fl_color fg = focused ? fl_th.text : ((res & FL_HOVER) ? fl_th.text : fl_th.dim);
		if (focused)
			fl_fill_rect(cv, r, fl_th.raised);
		else if (res & (FL_HOVER | FL_DOWN))
			fl_fill_rect(cv, r, fl_mix(fl_th.panel, fl_th.raised, 150));
		if (focused)
			fl_fill_rect(cv, fl_r(r.x, r.y + r.h - 2, r.w, 2), fl_th.accent);
		else if (res & FL_FOCUS)
			fl_fill_rect(cv, fl_r(r.x, r.y + r.h - 2, r.w, 2), fl_th.line);
		int iy = r.y + (r.h - 2 - ICON_PX) / 2;
		if (labels) {
			fl_icon_draw(cv, fl_icon_for_app(w->app_id), r.x + 12, iy, ICON_PX, fg);
			char fit[160];
			fl_text_fit(ui->f_reg, name, ew - 12 - ICON_PX - 8 - 12, fit, sizeof fit);
			fl_text_in(cv, ui->f_reg, fl_r(r.x + 12 + ICON_PX + 8, r.y, ew - 12 - ICON_PX - 8, r.h - 2), FL_LEFT, 0, fg, fit);
		} else {
			fl_icon_draw(cv, fl_icon_for_app(w->app_id), r.x + (r.w - ICON_PX) / 2, iy, ICON_PX, fg);
		}
		/* tell the compositor where this entry is: minimise and restore move towards it (fl_shell 7) */
		if (p->shell && p->shell_ver >= 7 && (w->ex != r.x || w->ew != r.w || w->eh != r.h)) {
			w->ex = r.x;
			w->ew = r.w;
			w->eh = r.h;
			fl_shell_set_window_entry(p->shell, w->id, r.x, r.y, r.w, r.h);
		}
		if ((res & FL_CLICK) || ((res & FL_FOCUS) && (ui->key == XKB_KEY_Return || ui->key == XKB_KEY_space))) {
			ui->key = 0;
			if (p->shell) {
				if (w->focused && !w->minimized)
					fl_shell_minimize(p->shell, w->id);
				else
					fl_shell_activate(p->shell, w->id);
			}
		}
		wx += ew + 2;
	}
}

/* desktop.conf changed: Do not disturb and power saving may have */
static void conf_changed(void *ud)
{
	struct panel *p = ud;
	notify_conf_changed(p);
	battery_conf_changed(p);
	/* panel_height=, panel_position=: the panel resizes itself, the compositor moves it (it reads
	 * panel_position= from the same file) and makes room; the menu and flyouts follow by themselves */
	if (p->shell && p->pw && fl_lk.panel_h != p->cur_h) {
		p->cur_h = fl_lk.panel_h;
		fl_win_resize(p->pw, p->out_w, fl_lk.panel_h);
		fl_shell_set_panel(p->shell, fl_win_surface(p->pw), (uint32_t)fl_lk.panel_h);
		for (int i = 0; i < p->nwins; i++)
			p->wins[i].ew = 0;	/* the entries moved: tell the compositor again */
	}
	desk_reposition(p);
	fl_win_redraw(p->pw);
}

/* ---- main ------------------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
	struct panel p = {0};
	int open_menu = 0, open_fly = 0, open_ws = 0, show_cards = 0, show_center = 0, show_bat = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--demo"))
			p.demo = 1;
		else if (!strcmp(argv[i], "--cards"))
			show_cards = p.demo = 1;
		else if (!strcmp(argv[i], "--center"))
			show_center = p.demo = 1;
		else if (!strcmp(argv[i], "--battery"))
			show_bat = p.demo = 1;
		else if (!strcmp(argv[i], "--menu"))
			open_menu = 1;
		else if (!strcmp(argv[i], "--volume"))
			open_fly = 1;
		else if (!strcmp(argv[i], "--workspaces"))
			open_ws = p.demo = 1;
		else {
			fprintf(stderr, "usage: flpanel [--demo] [--menu] [--volume] [--workspaces] [--cards] [--center] [--battery]\n"
			                "  --demo  fake windows and a fixed clock (for screenshots)\n"
			                "  --menu  start with the menu open\n"
			                "  --volume  start with the volume flyout open\n"
			                "  --workspaces  demo with the pager's menu open (FLUI_PPM=FILE writes FILE and FILE.N)\n"
			                "  --cards, --center, --battery  demo with notification cards, the centre or the\n"
			                "          battery flyout shown (FLUI_PPM=FILE writes file, file.3, file.4, file.5)\n");
			return !strcmp(argv[i], "-h") || !strcmp(argv[i], "--help") ? 0 : 2;
		}
	}
	signal(SIGCHLD, SIG_IGN);
	snprintf(p.layout, sizeof p.layout, "%s", "us");
	p.bat_pct = p.vol_pct = p.vol_fd = -1;
	p.vol_want_pct = p.vol_want_mute = -1;
	int k = fl_kbd_conf_read();
	if (k >= 0)
		snprintf(p.layout, sizeof p.layout, "%s", fl_kbd_layouts[k].id);

	p.app = fl_app_new(on_global, &p);
	if (!p.app) {
		fprintf(stderr, "flpanel: cannot connect to a Wayland compositor\n");
		return 1;
	}
	if (!p.shell && !fl_app_ppm(p.app)) {
		fprintf(stderr, "flpanel: the compositor has no fl_shell (run it under flcomp)\n");
		return 1;
	}
	int oh;
	fl_app_output_size(p.app, &p.out_w, &oh);
	p.f_small = fl_font_ui(500, 11);
	p.f_mono = fl_font_mono(500, 13);
	p.f_what = fl_font_ui(400, 12);
	p.f_cap = fl_font_ui(500, 11);
	p.f_head = fl_font_ui(600, 15);
	p.f_big = fl_font_ui(500, 30);

	fl_win_opts po = {
		.title = "flpanel", .app_id = "flpanel", .w = p.out_w, .h = PANEL_H, .raw = 1,
		.draw = draw_panel, .ud = &p,
	};
	p.pw = fl_win_new(p.app, &po);
	fl_win_opts mo = {
		.title = "start menu", .app_id = "flpanel-menu", .w = 500, .h = 520, .raw = 1,
		.draw = menu_draw, .ud = &p,
	};
	p.mw = fl_win_new(p.app, &mo);
	fl_win_opts fo = {
		.title = "volume", .app_id = "flpanel-volume", .w = FLY_W, .h = FLY_H, .raw = 1,
		.draw = flyout_draw, .ud = &p,
	};
	p.fw = fl_win_new(p.app, &fo);
	if (!p.pw || !p.mw || !p.fw)
		return 1;
	if (p.shell) {
		p.cur_h = PANEL_H;
		fl_shell_set_panel(p.shell, fl_win_surface(p.pw), PANEL_H);
		fl_shell_set_menu(p.shell, fl_win_surface(p.mw));
	}
	fl_win_map(p.pw);
	notify_init(&p);
	battery_init(&p);
	desk_init(&p);
	wsmenu_init(&p); /* last: the other windows keep their numbers in FLUI_PPM file names */

	if (p.demo) {
		struct win demo[2] = {
			{1, "flfiles", "Files", 0, 0, 0},
			{2, "flterm", "Terminal", 1, 0, 1},
		};
		p.wins = malloc(sizeof demo);
		memcpy(p.wins, demo, sizeof demo);
		p.nwins = 2;
		snprintf(p.layout, sizeof p.layout, "de");
		p.ws_cols = p.ws_rows = 2;
	}
	if (fl_app_ppm(p.app)) {
		int any = show_cards || show_center || show_bat;
		notify_demo(&p, !any ? -2 : show_cards ? 0 : show_center ? 1 : -1);
		battery_demo(&p, !any ? -1 : show_bat);
		if (open_menu)
			menu_open(&p);
		else {
			fl_win_destroy(p.mw);
			p.mw = NULL;
		}
		if (!open_fly) {
			fl_win_destroy(p.fw);
			p.fw = NULL;
		}
		if (open_ws) {
			p.ws_open = 1;
			p.ws_sel = 1;
			fl_win_map(p.wsw);
		} else {
			fl_win_destroy(p.wsw);
			p.wsw = NULL;
		}
	} else if (open_menu) {
		menu_open(&p);
	}
	p.clock = fl_timer_new(p.app, clock_tick, &p);
	clock_arm(&p);
	p.tray_timer = fl_timer_new(p.app, tray_tick, &p);
	if (p.demo) {
		p.bat_pct = 70;
		p.net_kind = 1;
		p.net_up = 1;
		p.vol_pct = 62;
		p.vol_cx = p.out_w - 150;
		if (open_fly) {
			p.fly_open = 1;
			fl_win_map(p.fw);
		}
	} else {
		tray_read_sys(&p);
		vol_query(&p);
		fl_timer_arm(p.tray_timer, 5000, 5000);
	}
	fl_app_watch_conf(p.app, conf_changed, &p);
	fl_app_run(p.app);
	notify_free(&p);
	fl_app_free(p.app);
	return 0;
}
