/* The media keys: volume, mute, microphone mute and screen brightness.
 *
 * Volume keys are the panel's job (fl_shell volume_key: it asks wpctl and keeps the
 * tray icon right, then asks us to show the indicator). Without a panel flcomp does it
 * itself with wpctl, so the keys work in a bare session too. Brightness is always done
 * here: the backlight's sysfs file (group seat may write it, see flsettings' udev rule),
 * steps of 5 %, never below 5 %, written to desktop.conf as brightness= like the
 * Display page does. Raise and lower repeat while held (the compositor eats the key,
 * so clients never see a repeat).
 *   XF86AudioRaiseVolume, XF86AudioLowerVolume, XF86AudioMute  (5 % steps)
 *   XF86MonBrightnessUp, XF86MonBrightnessDown                 (5 % steps)
 *   XF86AudioMicMute
 *   XF86AudioPlay, XF86AudioStop, XF86AudioNext, XF86AudioPrev (the music player,
 *                                   `flmusic --ctl play-pause|stop|next|previous`, MPRIS)
 * SPDX-License-Identifier: MIT */
#include "comp.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#define STEP 5
#define MIN_PCT 5

int media_action(uint32_t sym)
{
	switch (sym) {
	case XKB_KEY_XF86AudioRaiseVolume: return MEDIA_VOL_UP;
	case XKB_KEY_XF86AudioLowerVolume: return MEDIA_VOL_DOWN;
	case XKB_KEY_XF86AudioMute: return MEDIA_MUTE;
	case XKB_KEY_XF86MonBrightnessUp: return MEDIA_BRIGHT_UP;
	case XKB_KEY_XF86MonBrightnessDown: return MEDIA_BRIGHT_DOWN;
	case XKB_KEY_XF86AudioMicMute: return MEDIA_MIC;
	case XKB_KEY_XF86AudioPlay: return MEDIA_PLAY;
	case XKB_KEY_XF86AudioStop: return MEDIA_STOP;
	case XKB_KEY_XF86AudioNext: return MEDIA_NEXT;
	case XKB_KEY_XF86AudioPrev: return MEDIA_PREV;
	}
	return -1;
}

/* ---- a shell command whose output we read without blocking -------------------------------- */

struct job {
	int fd, kind;
	char buf[128];
	size_t len;
	struct wl_event_source *src;
};

static int jobs_running;

static void parse_volume(int kind, const char *out)
{
	/* "Volume: 0.45" or "Volume: 0.45 [MUTED]" */
	const char *v = strstr(out, "Volume:");
	if (!v)
		return;
	double f = atof(v + 7);
	osd_show(kind, (int)(f * 100 + 0.5), strstr(out, "[MUTED]") != NULL);
}

static int job_cb(int fd, uint32_t mask, void *data)
{
	struct job *j = data;
	ssize_t n = read(fd, j->buf + j->len, sizeof j->buf - 1 - j->len);
	if (n > 0) {
		j->len += (size_t)n;
		if (j->len < sizeof j->buf - 1)
			return 0;
	}
	j->buf[j->len] = 0;
	parse_volume(j->kind, j->buf);
	wl_event_source_remove(j->src);
	close(fd);
	free(j);
	jobs_running--;
	return 0;
}

static void run_wpctl(int kind, const char *cmd)
{
	if (jobs_running > 0)
		return;	/* a key held down: one at a time is plenty */
	int fds[2];
	if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) < 0)
		return;
	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return;
	}
	if (pid == 0) {
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGPIPE, SIG_DFL);
		int nul = open("/dev/null", O_RDWR);
		dup2(fds[1], 1);
		if (nul >= 0) {
			dup2(nul, 0);
			dup2(nul, 2);
		}
		setsid();
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	struct job *j = calloc(1, sizeof *j);
	if (!j) {
		close(fds[0]);
		return;
	}
	j->fd = fds[0];
	j->kind = kind;
	j->src = wl_event_loop_add_fd(C.loop, fds[0], WL_EVENT_READABLE, job_cb, j);
	jobs_running++;
}

#define SINK "@DEFAULT_AUDIO_SINK@"
#define SRC "@DEFAULT_AUDIO_SOURCE@"

static void volume_without_panel(int action)
{
	switch (action) {
	case MEDIA_VOL_UP:
		run_wpctl(OSD_VOLUME, "wpctl set-volume " SINK " 5%+ -l 1.0 >/dev/null 2>&1; wpctl get-volume " SINK);
		break;
	case MEDIA_VOL_DOWN:
		run_wpctl(OSD_VOLUME, "wpctl set-volume " SINK " 5%- >/dev/null 2>&1; wpctl get-volume " SINK);
		break;
	default:
		run_wpctl(OSD_VOLUME, "wpctl set-mute " SINK " toggle >/dev/null 2>&1; wpctl get-volume " SINK);
	}
}

/* ---- brightness -------------------------------------------------------------------------------- */

static char bl_dir[300];

static int read_int(const char *name)
{
	char p[340];
	snprintf(p, sizeof p, "%s/%s", bl_dir, name);
	FILE *f = fopen(p, "r");
	int v = -1;
	if (f) {
		if (fscanf(f, "%d", &v) != 1)
			v = -1;
		fclose(f);
	}
	return v;
}

/* Which backlight: a laptop can have several (acpi_video0, intel_backlight, amdgpu_bl1, nvidia_0).
 * Prefer the kernel's "type": firmware, then platform, then raw; ties go to the first by name. */
static void bl_pick(char *out, size_t n)
{
	static const char *const rank[] = { "firmware", "platform", "raw" };
	int best = 99;
	DIR *d = opendir("/sys/class/backlight");
	struct dirent *de;
	out[0] = 0;
	while (d && (de = readdir(d))) {
		char p[300], t[32] = "";
		if (de->d_name[0] == '.')
			continue;
		snprintf(p, sizeof p, "/sys/class/backlight/%s/type", de->d_name);
		FILE *f = fopen(p, "r");
		if (f) {
			if (fgets(t, sizeof t, f))
				t[strcspn(t, "\n")] = 0;
			fclose(f);
		}
		int r = 3;
		for (int i = 0; i < 3; i++)
			if (!strcmp(t, rank[i]))
				r = i;
		if (r < best || (r == best && strcmp(de->d_name, out + 21) < 0)) {
			best = r;
			snprintf(out, n, "/sys/class/backlight/%s", de->d_name);
		}
	}
	if (d)
		closedir(d);
}

static bool bl_find(void)
{
	if (bl_dir[0])
		return true;
	const char *e = getenv("FLCOMP_BACKLIGHT");
	if (e && *e) {
		snprintf(bl_dir, sizeof bl_dir, "%s", e);
		return true;
	}
	bl_pick(bl_dir, sizeof bl_dir);
	return bl_dir[0] != 0;
}

static void brightness_step(int dir)
{
	if (!bl_find())
		return;	/* a desktop monitor: nothing to adjust */
	int max = read_int("max_brightness"), cur = read_int("brightness");
	if (max <= 0 || cur < 0)
		return;
	int pct = (cur * 100 + max / 2) / max;
	int want = pct + dir * STEP;
	want = want < MIN_PCT ? MIN_PCT : want > 100 ? 100 : want;
	int raw = max * want / 100;
	if (raw < 1)
		raw = 1;
	if (raw == cur && want != pct)	/* a coarse backlight: at least one step */
		raw = cur + dir < 1 ? 1 : cur + dir > max ? max : cur + dir;
	char p[340];
	snprintf(p, sizeof p, "%s/brightness", bl_dir);
	FILE *f = fopen(p, "w");
	if (!f || fprintf(f, "%d\n", raw) < 0 || fclose(f)) {
		static bool said;
		if (!said)
			fprintf(stderr, "flcomp: cannot set the brightness: %s (the account needs the group seat)\n",
				strerror(errno));
		said = true;
		return;
	}
	pct = (raw * 100 + max / 2) / max;
	osd_show(OSD_BRIGHTNESS, pct, false);
	char b[16];
	snprintf(b, sizeof b, "%d", pct);
	config_set("brightness", b);
}

/* Power saving: the backlight goes to 70 % of what it is now (the desktop.conf brightness= is not
 * touched, so a restart dims from the real value again), and back when the mode ends, unless the
 * brightness was changed by hand meanwhile. */
static int saver_base = -1, saver_dim = -1;

void backlight_saver(bool on)
{
	if (C.headless && !getenv("FLCOMP_BACKLIGHT"))
		return;
	if (!bl_find())
		return;
	int max = read_int("max_brightness"), cur = read_int("brightness");
	if (max <= 0 || cur < 0)
		return;
	int want;
	if (on) {
		if (saver_base >= 0)
			return;
		saver_base = cur;
		saver_dim = cur * 70 / 100;
		if (saver_dim < 1)
			saver_dim = 1;
		want = saver_dim;
	} else {
		if (saver_base < 0)
			return;
		want = cur == saver_dim ? saver_base : cur;
		saver_base = saver_dim = -1;
	}
	char p[340];
	snprintf(p, sizeof p, "%s/brightness", bl_dir);
	FILE *f = fopen(p, "w");
	if (f) {
		fprintf(f, "%d\n", want);
		fclose(f);
	}
}

/* ---- keys ------------------------------------------------------------------------------------- */

static struct wl_event_source *rep;
static int rep_action;

static void fire(int action)
{
	switch (action) {
	case MEDIA_VOL_UP:
	case MEDIA_VOL_DOWN:
	case MEDIA_MUTE:
		if (C.panel)
			fl_emit_volume_key((uint32_t)action);	/* the panel does it and asks for the indicator */
		else
			volume_without_panel(action);
		break;
	case MEDIA_BRIGHT_UP: brightness_step(1); break;
	case MEDIA_BRIGHT_DOWN: brightness_step(-1); break;
	case MEDIA_PLAY: run_wpctl(OSD_VOLUME, "flmusic --ctl play-pause >/dev/null 2>&1"); break;
	case MEDIA_STOP: run_wpctl(OSD_VOLUME, "flmusic --ctl stop >/dev/null 2>&1"); break;
	case MEDIA_NEXT: run_wpctl(OSD_VOLUME, "flmusic --ctl next >/dev/null 2>&1"); break;
	case MEDIA_PREV: run_wpctl(OSD_VOLUME, "flmusic --ctl previous >/dev/null 2>&1"); break;
	case MEDIA_MIC:
		run_wpctl(OSD_MIC, "wpctl set-mute " SRC " toggle >/dev/null 2>&1; wpctl get-volume " SRC);
		break;
	}
}

static int rep_cb(void *data)
{
	fire(rep_action);
	wl_event_source_timer_update(rep, 80);
	return 0;
}

void media_key(int action, bool pressed)
{
	if (!rep && C.loop)
		rep = wl_event_loop_add_timer(C.loop, rep_cb, NULL);
	if (pressed) {
		fire(action);
		bool steps = action == MEDIA_VOL_UP || action == MEDIA_VOL_DOWN || action == MEDIA_BRIGHT_UP ||
			     action == MEDIA_BRIGHT_DOWN;
		if (steps && rep) {
			rep_action = action;
			wl_event_source_timer_update(rep, 450);
		}
	} else if (rep) {
		wl_event_source_timer_update(rep, 0);
	}
}
