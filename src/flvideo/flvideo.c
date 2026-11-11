/* flvideo -- the FreeLFS video player ("Videos")
 * SPDX-License-Identifier: MIT
 *
 * libmpv does the work: demuxing, decoding (VA-API where the GPU has it,
 * copied back to memory; software otherwise), audio output and subtitles
 * (libass). It renders through its software render API straight into a buffer
 * of this program, which flui then puts in the window with the controls on top:
 * no GL, no EGL, nothing the cheapest machine would lack.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <dbus/dbus.h>

#include <mpv/client.h>
#include <mpv/render.h>

#include <flui.h>

#define BAR_H 70
#define MAX_TRACKS 24
#define HIDE_MS 2800

enum { ID_VIDEO = 1, ID_PLAY, ID_BACK, ID_FWD, ID_SEEK, ID_SPEED, ID_AUDIO, ID_SUBS, ID_MUTE, ID_VOL, ID_FULL,
       ID_OPEN, ID_MENU = 100 };

struct trk {
	int id, sel;
	char label[120];
};

struct V {
	fl_app *app;
	fl_win *win;
	fl_font *small;
	fl_timer *hide_timer, *swap_timer, *save_timer, *msg_timer;
	mpv_handle *mpv;
	mpv_render_context *rc;
	int wake[2], upd[2];
	char path[PATH_MAX];
	char title[300];
	/* player state */
	double pos, dur, volume, speed;
	int paused, muted, loaded, ended, buffering;
	int vid_w, vid_h;
	char hwdec[48];
	struct trk aud[MAX_TRACKS], sub[MAX_TRACKS];
	int naud, nsub;
	/* video buffer */
	uint32_t *vbuf;
	size_t vcap;
	int vstride, rw, rh, vvalid, need_render;
	/* ui */
	int fullscreen, want_fullscreen_swap;
	int win_w, win_h;
	int menu; /* 0 none, 1 audio, 2 subtitles */
	fl_rect menu_anchor;
	int seeking;
	double seek_frac;
	int last_mx, last_my;
	long long last_move_ms;
	char msg[200];
	int msg_err;
	double start_at;
	int start_paused;
	char resume_path[PATH_MAX];
	/* the file picker: a child process (flvideo --pick-file) asks the portal and prints the path */
	pid_t pick_pid;
	int pick_fd;
	char pick_buf[PATH_MAX + 8];
	size_t pick_len;
};

static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void say(struct V *v, int err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(v->msg, sizeof v->msg, fmt, ap);
	va_end(ap);
	v->msg_err = err;
	fl_timer_arm(v->msg_timer, 3500, 0);
	fl_win_redraw(v->win);
}

static void msg_clear(void *ud)
{
	struct V *v = ud;
	v->msg[0] = 0;
	fl_win_redraw(v->win);
}

static void fmt_time(double s, char *out, size_t cap)
{
	if (s < 0 || s != s)
		s = 0;
	long t = (long)s;
	if (t >= 3600)
		snprintf(out, cap, "%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60);
	else
		snprintf(out, cap, "%ld:%02ld", t / 60, t % 60);
}

/* ---- remembered positions ----------------------------------------------------------------- */

static void resume_file(char *out, size_t cap)
{
	const char *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
	if (x && *x)
		snprintf(out, cap, "%s/flvideo/resume.tsv", x);
	else if (h && *h)
		snprintf(out, cap, "%s/.local/share/flvideo/resume.tsv", h);
	else
		out[0] = 0;
}

static void mkdir_p(const char *path)
{
	char t[PATH_MAX];
	snprintf(t, sizeof t, "%s", path);
	for (char *p = t + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			mkdir(t, 0700);
			*p = '/';
		}
}

/* position remembered for path, or 0 */
static double resume_get(const char *path)
{
	char f[PATH_MAX];
	resume_file(f, sizeof f);
	FILE *fp = f[0] ? fopen(f, "r") : NULL;
	if (!fp)
		return 0;
	char *line = NULL;
	size_t cap = 0;
	double pos = 0;
	size_t pl = strlen(path);
	while (getline(&line, &cap, fp) > 0)
		if (!strncmp(line, path, pl) && line[pl] == '\t')
			pos = atof(line + pl + 1);
	free(line);
	fclose(fp);
	return pos;
}

/* secs <= 0 forgets the file */
static void resume_set(const char *path, double secs)
{
	char f[PATH_MAX], tmp[PATH_MAX + 8];
	resume_file(f, sizeof f);
	if (!f[0] || !path[0])
		return;
	snprintf(tmp, sizeof tmp, "%s.tmp", f);
	char dir[PATH_MAX];
	snprintf(dir, sizeof dir, "%s", f);
	char *sl = strrchr(dir, '/');
	if (sl)
		*sl = 0;
	mkdir_p(dir);
	mkdir(dir, 0700);
	FILE *out = fopen(tmp, "w");
	if (!out)
		return;
	FILE *in = fopen(f, "r");
	size_t pl = strlen(path);
	if (in) {
		char *line = NULL;
		size_t cap = 0;
		int kept = 0;
		while (getline(&line, &cap, in) > 0) {
			if (!strncmp(line, path, pl) && line[pl] == '\t')
				continue;
			if (++kept > 400) /* the file stays small */
				break;
			fputs(line, out);
		}
		free(line);
		fclose(in);
	}
	if (secs > 0)
		fprintf(out, "%s\t%.1f\n", path, secs);
	if (fclose(out) == 0)
		rename(tmp, f);
	else
		unlink(tmp);
}

static void save_position(struct V *v)
{
	if (!v->loaded || !v->path[0])
		return;
	/* near the start or the end: forget, so the next play starts from the top */
	if (v->ended || v->pos < 10 || (v->dur > 0 && v->pos > v->dur - 10))
		resume_set(v->path, 0);
	else
		resume_set(v->path, v->pos);
}

static void save_tick(void *ud)
{
	struct V *v = ud;
	if (v->loaded && !v->paused)
		save_position(v);
}

/* ---- mpv --------------------------------------------------------------------------------- */

static void wake_cb(void *ud)
{
	struct V *v = ud;
	(void)!write(v->wake[1], "x", 1);
}

static void upd_cb(void *ud)
{
	struct V *v = ud;
	(void)!write(v->upd[1], "x", 1);
}

static void cmd(struct V *v, const char *a, const char *b, const char *c, const char *d)
{
	const char *args[6] = {a, b, c, d, NULL, NULL};
	if (v->mpv)
		mpv_command_async(v->mpv, 0, args);
}

static void set_prop(struct V *v, const char *name, const char *val)
{
	if (v->mpv)
		mpv_set_property_string(v->mpv, name, val);
}

static char *getp(struct V *v, const char *name)
{
	return v->mpv ? mpv_get_property_string(v->mpv, name) : NULL;
}

static void refresh_tracks(struct V *v)
{
	v->naud = v->nsub = 0;
	char *cnt = getp(v, "track-list/count");
	int n = cnt ? atoi(cnt) : 0;
	mpv_free(cnt);
	for (int i = 0; i < n; i++) {
		char name[64];
		snprintf(name, sizeof name, "track-list/%d/type", i);
		char *type = getp(v, name);
		if (!type)
			continue;
		int is_a = !strcmp(type, "audio"), is_s = !strcmp(type, "sub");
		mpv_free(type);
		if (!is_a && !is_s)
			continue;
		struct trk *t = is_a ? &v->aud[v->naud] : &v->sub[v->nsub];
		if ((is_a ? v->naud : v->nsub) >= MAX_TRACKS)
			continue;
		snprintf(name, sizeof name, "track-list/%d/id", i);
		char *id = getp(v, name);
		snprintf(name, sizeof name, "track-list/%d/lang", i);
		char *lang = getp(v, name);
		snprintf(name, sizeof name, "track-list/%d/title", i);
		char *title = getp(v, name);
		snprintf(name, sizeof name, "track-list/%d/codec", i);
		char *codec = getp(v, name);
		snprintf(name, sizeof name, "track-list/%d/selected", i);
		char *sel = getp(v, name);
		t->id = id ? atoi(id) : 0;
		t->sel = sel && !strcmp(sel, "yes");
		snprintf(t->label, sizeof t->label, "%s%s%s%s%s%s", lang ? lang : "Track ", lang ? "" : id ? id : "?",
		         title ? "  " : "", title ? title : "", codec ? "  (" : "", codec ? codec : "");
		if (codec && strlen(t->label) < sizeof t->label - 2)
			strcat(t->label, ")");
		mpv_free(id);
		mpv_free(lang);
		mpv_free(title);
		mpv_free(codec);
		mpv_free(sel);
		if (is_a)
			v->naud++;
		else
			v->nsub++;
	}
}

static void handle_events(struct V *v)
{
	char buf[64];
	while (read(v->wake[0], buf, sizeof buf) > 0)
		;
	for (;;) {
		mpv_event *e = mpv_wait_event(v->mpv, 0);
		if (e->event_id == MPV_EVENT_NONE)
			break;
		switch (e->event_id) {
		case MPV_EVENT_PROPERTY_CHANGE: {
			mpv_event_property *p = e->data;
			if (!strcmp(p->name, "time-pos")) {
				if (p->format == MPV_FORMAT_DOUBLE)
					v->pos = *(double *)p->data;
			} else if (!strcmp(p->name, "duration")) {
				if (p->format == MPV_FORMAT_DOUBLE)
					v->dur = *(double *)p->data;
			} else if (!strcmp(p->name, "pause")) {
				if (p->format == MPV_FORMAT_FLAG)
					v->paused = *(int *)p->data;
			} else if (!strcmp(p->name, "mute")) {
				if (p->format == MPV_FORMAT_FLAG)
					v->muted = *(int *)p->data;
			} else if (!strcmp(p->name, "volume")) {
				if (p->format == MPV_FORMAT_DOUBLE)
					v->volume = *(double *)p->data;
			} else if (!strcmp(p->name, "speed")) {
				if (p->format == MPV_FORMAT_DOUBLE)
					v->speed = *(double *)p->data;
			} else if (!strcmp(p->name, "paused-for-cache")) {
				if (p->format == MPV_FORMAT_FLAG)
					v->buffering = *(int *)p->data;
			} else if (!strcmp(p->name, "eof-reached")) {
				if (p->format == MPV_FORMAT_FLAG)
					v->ended = *(int *)p->data;
			} else if (!strcmp(p->name, "track-list")) {
				refresh_tracks(v);
			} else if (!strcmp(p->name, "hwdec-current")) {
				if (p->format == MPV_FORMAT_STRING)
					snprintf(v->hwdec, sizeof v->hwdec, "%s", *(char **)p->data);
			} else if (!strcmp(p->name, "media-title")) {
				if (p->format == MPV_FORMAT_STRING) {
					snprintf(v->title, sizeof v->title, "%s", *(char **)p->data);
					fl_win_set_title(v->win, v->title);
				}
			}
			break;
		}
		case MPV_EVENT_FILE_LOADED: {
			v->loaded = 1;
			char *w = getp(v, "width"), *h = getp(v, "height");
			v->vid_w = w ? atoi(w) : 0;
			v->vid_h = h ? atoi(h) : 0;
			mpv_free(w);
			mpv_free(h);
			refresh_tracks(v);
			break;
		}
		case MPV_EVENT_END_FILE: {
			mpv_event_end_file *ef = e->data;
			if (ef->reason == MPV_END_FILE_REASON_ERROR)
				say(v, 1, "Cannot play this file: %s", mpv_error_string(ef->error));
			break;
		}
		case MPV_EVENT_SHUTDOWN: fl_app_quit(v->app); break;
		default: break;
		}
	}
	fl_win_redraw(v->win);
}

static void on_wake_fd(void *ud, int fd, short rev)
{
	handle_events(ud);
}

static void on_upd_fd(void *ud, int fd, short rev)
{
	struct V *v = ud;
	char buf[64];
	while (read(v->upd[0], buf, sizeof buf) > 0)
		;
	if (v->rc && (mpv_render_context_update(v->rc) & MPV_RENDER_UPDATE_FRAME))
		v->need_render = 1;
	fl_win_redraw(v->win);
}

static int mpv_start(struct V *v, const char *path)
{
	v->mpv = mpv_create();
	if (!v->mpv)
		return -1;
	const char *ao = getenv("FLVIDEO_AO");
	const char *hw = getenv("FLVIDEO_HWDEC");
	mpv_set_option_string(v->mpv, "config", "no");
	mpv_set_option_string(v->mpv, "load-scripts", "no");
	mpv_set_option_string(v->mpv, "ytdl", "no");
	mpv_set_option_string(v->mpv, "terminal", "no");
	mpv_set_option_string(v->mpv, "input-default-bindings", "no");
	mpv_set_option_string(v->mpv, "input-vo-keyboard", "no");
	mpv_set_option_string(v->mpv, "osc", "no");
	mpv_set_option_string(v->mpv, "osd-level", "0");
	mpv_set_option_string(v->mpv, "vo", "libmpv");
	mpv_set_option_string(v->mpv, "ao", ao && *ao ? ao : "pipewire,alsa");
	mpv_set_option_string(v->mpv, "audio-client-name", "Videos");
	mpv_set_option_string(v->mpv, "hwdec", hw && *hw ? hw : "auto-copy-safe");
	mpv_set_option_string(v->mpv, "keep-open", "yes");
	mpv_set_option_string(v->mpv, "sub-auto", "fuzzy");
	mpv_set_option_string(v->mpv, "sub-font", "IBM Plex Sans");
	mpv_set_option_string(v->mpv, "sub-font-size", "40");
	mpv_set_option_string(v->mpv, "sub-border-size", "2.4");
	mpv_set_option_string(v->mpv, "sub-color", "#FFFFFF");
	mpv_set_option_string(v->mpv, "sws-scaler", "bilinear");
	mpv_set_option_string(v->mpv, "volume-max", "100");
	mpv_set_option_string(v->mpv, "resume-playback", "no");
	mpv_set_option_string(v->mpv, "msg-level", getenv("FLVIDEO_DEBUG") ? "all=v" : "all=warn");
	if (getenv("FLVIDEO_DEBUG"))
		mpv_set_option_string(v->mpv, "terminal", "yes");
	if (v->start_at > 0) {
		char s[32];
		snprintf(s, sizeof s, "%.2f", v->start_at);
		mpv_set_option_string(v->mpv, "start", s);
	}
	if (v->start_paused)
		mpv_set_option_string(v->mpv, "pause", "yes");
	if (mpv_initialize(v->mpv) < 0)
		return -1;
	mpv_render_param prm[] = {
		{MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_SW},
		{MPV_RENDER_PARAM_INVALID, NULL},
	};
	if (mpv_render_context_create(&v->rc, v->mpv, prm) < 0)
		return -1;
	mpv_render_context_set_update_callback(v->rc, upd_cb, v);
	mpv_set_wakeup_callback(v->mpv, wake_cb, v);
	mpv_observe_property(v->mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
	mpv_observe_property(v->mpv, 0, "duration", MPV_FORMAT_DOUBLE);
	mpv_observe_property(v->mpv, 0, "pause", MPV_FORMAT_FLAG);
	mpv_observe_property(v->mpv, 0, "mute", MPV_FORMAT_FLAG);
	mpv_observe_property(v->mpv, 0, "volume", MPV_FORMAT_DOUBLE);
	mpv_observe_property(v->mpv, 0, "speed", MPV_FORMAT_DOUBLE);
	mpv_observe_property(v->mpv, 0, "paused-for-cache", MPV_FORMAT_FLAG);
	mpv_observe_property(v->mpv, 0, "eof-reached", MPV_FORMAT_FLAG);
	mpv_observe_property(v->mpv, 0, "track-list", MPV_FORMAT_NONE);
	mpv_observe_property(v->mpv, 0, "hwdec-current", MPV_FORMAT_STRING);
	mpv_observe_property(v->mpv, 0, "media-title", MPV_FORMAT_STRING);
	v->volume = 100;
	v->speed = 1;
	if (path) {
		const char *args[] = {"loadfile", path, NULL};
		mpv_command_async(v->mpv, 0, args);
	}
	return 0;
}

/* ---- actions ----------------------------------------------------------------------------- */

static void toggle_pause(struct V *v)
{
	if (!v->loaded)
		return;
	if (v->ended) {
		cmd(v, "seek", "0", "absolute", NULL);
		set_prop(v, "pause", "no");
		return;
	}
	cmd(v, "cycle", "pause", NULL, NULL);
}

static void seek_rel(struct V *v, double secs)
{
	char s[32];
	snprintf(s, sizeof s, "%.1f", secs);
	cmd(v, "seek", s, "relative", NULL);
}

static void set_volume(struct V *v, double vol)
{
	if (vol < 0)
		vol = 0;
	if (vol > 100)
		vol = 100;
	char s[32];
	snprintf(s, sizeof s, "%.0f", vol);
	set_prop(v, "volume", s);
	v->volume = vol;
	if (vol > 0 && v->muted)
		set_prop(v, "mute", "no");
}

static void cycle_speed(struct V *v)
{
	static const double steps[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
	int i = 0;
	for (int k = 0; k < 6; k++)
		if (fabs(steps[k] - v->speed) < 0.01)
			i = k;
	double n = steps[(i + 1) % 6];
	char s[16];
	snprintf(s, sizeof s, "%.2f", n);
	set_prop(v, "speed", s);
	v->speed = n;
}

static void cycle_track(struct V *v, int sub)
{
	struct trk *t = sub ? v->sub : v->aud;
	int n = sub ? v->nsub : v->naud;
	if (n == 0) {
		say(v, 0, sub ? "No subtitles in this file" : "No other audio track");
		return;
	}
	int cur = -1;
	for (int i = 0; i < n; i++)
		if (t[i].sel)
			cur = i;
	int nx = cur + 1;
	char id[16];
	if (nx >= n) {
		if (sub) {
			set_prop(v, "sid", "no");
			say(v, 0, "Subtitles off");
			return;
		}
		nx = 0;
	}
	snprintf(id, sizeof id, "%d", t[nx].id);
	set_prop(v, sub ? "sid" : "aid", id);
	say(v, 0, "%s: %s", sub ? "Subtitles" : "Audio", t[nx].label);
}

static void toggle_fullscreen(struct V *v)
{
	v->want_fullscreen_swap = 1;
	fl_timer_arm(v->swap_timer, 1, 0);
}

/* ---- open a file: the portal's file dialog (flportal) -------------------------------------- */

/* "flvideo --pick-file": ask xdg-desktop-portal's FileChooser for one video and print its path.
 * Runs as a child process of the player so the window never waits for the dialog. Exit 0: path
 * on stdout, 1: cancelled, 2: no dialog available (message on stderr). */
static void put_str(DBusMessageIter *it, const char *k, int type, const char *sig, const void *val)
{
	DBusMessageIter e, var;
	dbus_message_iter_open_container(it, DBUS_TYPE_DICT_ENTRY, NULL, &e);
	dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k);
	dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, sig, &var);
	dbus_message_iter_append_basic(&var, type, val);
	dbus_message_iter_close_container(&e, &var);
	dbus_message_iter_close_container(it, &e);
}

static void put_filters(DBusMessageIter *opts)
{
	/* filters: a(sa(us)); (0, glob) or (1, mime type) */
	static const struct { const char *name, *pat; unsigned kind; } f[] = {
		{"Videos", "video/*", 1}, {"All files", "*", 0},
	};
	DBusMessageIter e, var, arr;
	const char *k = "filters";
	dbus_message_iter_open_container(opts, DBUS_TYPE_DICT_ENTRY, NULL, &e);
	dbus_message_iter_append_basic(&e, DBUS_TYPE_STRING, &k);
	dbus_message_iter_open_container(&e, DBUS_TYPE_VARIANT, "a(sa(us))", &var);
	dbus_message_iter_open_container(&var, DBUS_TYPE_ARRAY, "(sa(us))", &arr);
	for (size_t i = 0; i < sizeof f / sizeof *f; i++) {
		DBusMessageIter st, pats, ps;
		dbus_message_iter_open_container(&arr, DBUS_TYPE_STRUCT, NULL, &st);
		dbus_message_iter_append_basic(&st, DBUS_TYPE_STRING, &f[i].name);
		dbus_message_iter_open_container(&st, DBUS_TYPE_ARRAY, "(us)", &pats);
		dbus_message_iter_open_container(&pats, DBUS_TYPE_STRUCT, NULL, &ps);
		dbus_uint32_t kind = f[i].kind;
		dbus_message_iter_append_basic(&ps, DBUS_TYPE_UINT32, &kind);
		dbus_message_iter_append_basic(&ps, DBUS_TYPE_STRING, &f[i].pat);
		dbus_message_iter_close_container(&pats, &ps);
		dbus_message_iter_close_container(&st, &pats);
		dbus_message_iter_close_container(&arr, &st);
	}
	dbus_message_iter_close_container(&var, &arr);
	dbus_message_iter_close_container(&e, &var);
	dbus_message_iter_close_container(opts, &e);
}

static void uri_to_path(const char *uri, char *out, size_t cap)
{
	if (!strncmp(uri, "file://", 7))
		uri += 7;
	size_t n = 0;
	for (; *uri && n + 1 < cap; uri++) {
		if (*uri == '%' && uri[1] && uri[2]) {
			char h[3] = {uri[1], uri[2], 0}, *end;
			long c = strtol(h, &end, 16);
			if (!*end) {
				out[n++] = (char)c;
				uri += 2;
				continue;
			}
		}
		out[n++] = *uri;
	}
	out[n] = 0;
}

static int pick_child(void)
{
	DBusError err;
	dbus_error_init(&err);
	DBusConnection *c = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
	if (!c) {
		fprintf(stderr, "flvideo: no session bus: %s\n", err.message ? err.message : "?");
		return 2;
	}
	dbus_connection_set_exit_on_disconnect(c, FALSE);
	char sender[96], token[48], rpath[256], rule[400];
	const char *me = dbus_bus_get_unique_name(c);
	snprintf(sender, sizeof sender, "%s", me && *me ? me + 1 : "x");
	for (char *p = sender; *p; p++)
		if (*p == '.')
			*p = '_';
	snprintf(token, sizeof token, "flvideo%d", (int)getpid());
	snprintf(rpath, sizeof rpath, "/org/freedesktop/portal/desktop/request/%s/%s", sender, token);
	snprintf(rule, sizeof rule,
	         "type='signal',interface='org.freedesktop.portal.Request',member='Response',path='%s'", rpath);
	dbus_bus_add_match(c, rule, &err);
	if (dbus_error_is_set(&err)) {
		fprintf(stderr, "flvideo: %s\n", err.message);
		return 2;
	}
	DBusMessage *m = dbus_message_new_method_call("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
	                                              "org.freedesktop.portal.FileChooser", "OpenFile");
	const char *parent = "", *title = "Open a video";
	DBusMessageIter it, opts;
	dbus_message_iter_init_append(m, &it);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &parent);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &title);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &opts);
	const char *tok = token;
	put_str(&opts, "handle_token", DBUS_TYPE_STRING, "s", &tok);
	dbus_bool_t no = FALSE;
	put_str(&opts, "multiple", DBUS_TYPE_BOOLEAN, "b", &no);
	put_filters(&opts);
	dbus_message_iter_close_container(&it, &opts);
	DBusMessage *r = dbus_connection_send_with_reply_and_block(c, m, 30000, &err);
	dbus_message_unref(m);
	if (!r) {
		fprintf(stderr, "flvideo: the file dialog is not available: %s\n", err.message ? err.message : "?");
		return 2;
	}
	dbus_message_unref(r);
	for (;;) {
		DBusMessage *sm;
		while ((sm = dbus_connection_pop_message(c))) {
			if (!dbus_message_is_signal(sm, "org.freedesktop.portal.Request", "Response")) {
				dbus_message_unref(sm);
				continue;
			}
			DBusMessageIter si, dict;
			dbus_uint32_t resp = 2;
			if (!dbus_message_iter_init(sm, &si))
				return 2;
			dbus_message_iter_get_basic(&si, &resp);
			if (resp != 0)
				return resp == 1 ? 1 : 2;
			dbus_message_iter_next(&si);
			dbus_message_iter_recurse(&si, &dict);
			for (; dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY; dbus_message_iter_next(&dict)) {
				DBusMessageIter e, var, arr;
				const char *k = NULL;
				dbus_message_iter_recurse(&dict, &e);
				dbus_message_iter_get_basic(&e, &k);
				if (!k || strcmp(k, "uris"))
					continue;
				dbus_message_iter_next(&e);
				dbus_message_iter_recurse(&e, &var);
				dbus_message_iter_recurse(&var, &arr);
				if (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
					const char *uri;
					char path[PATH_MAX];
					dbus_message_iter_get_basic(&arr, &uri);
					uri_to_path(uri, path, sizeof path);
					printf("%s\n", path);
					fflush(stdout);
					return 0;
				}
			}
			return 1;
		}
		if (!dbus_connection_read_write(c, -1)) {
			fprintf(stderr, "flvideo: lost the session bus\n");
			return 2;
		}
	}
}

/* load FILE in the running player */
static void open_path(struct V *v, const char *path)
{
	char real[PATH_MAX];
	if (!v->mpv)
		return;
	if (!realpath(path, real)) {
		say(v, 1, "Cannot open %s: %s", path, strerror(errno));
		return;
	}
	save_position(v);
	snprintf(v->path, sizeof v->path, "%s", real);
	v->loaded = v->ended = v->msg_err = 0;
	v->pos = v->dur = 0;
	v->naud = v->nsub = 0;
	v->vvalid = 0;
	v->title[0] = 0;
	v->menu = 0;
	fl_win_set_title(v->win, "Videos");
	double at = resume_get(v->path);
	char start[32];
	snprintf(start, sizeof start, "%.2f", at > 0 ? at : 0.0);
	set_prop(v, "start", start);
	const char *args[] = {"loadfile", v->path, "replace", NULL};
	mpv_command_async(v->mpv, 0, args);
	v->msg[0] = 0;
	fl_win_redraw(v->win);
}

static void pick_done(struct V *v)
{
	fl_fd_remove(v->app, v->pick_fd);
	close(v->pick_fd);
	int st = 0;
	while (waitpid(v->pick_pid, &st, 0) < 0 && errno == EINTR)
		;
	v->pick_pid = 0;
	v->pick_fd = -1;
	int rc = WIFEXITED(st) ? WEXITSTATUS(st) : 2;
	char *nl = memchr(v->pick_buf, '\n', v->pick_len);
	if (nl)
		*nl = 0;
	else
		v->pick_buf[v->pick_len] = 0;
	if (rc == 0 && v->pick_buf[0])
		open_path(v, v->pick_buf);
	else if (rc != 1)
		say(v, 1, "The file dialog did not open (needs the desktop portal). Open the file from Files instead.");
	fl_win_redraw(v->win);
}

static void on_pick_fd(void *ud, int fd, short rev)
{
	struct V *v = ud;
	ssize_t n = read(fd, v->pick_buf + v->pick_len, sizeof v->pick_buf - 1 - v->pick_len);
	if (n > 0) {
		v->pick_len += (size_t)n;
		if (v->pick_len < sizeof v->pick_buf - 1)
			return;
	} else if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
		return;
	}
	pick_done(v);
}

extern char **environ;

/* Open action: the dialog runs in a child process; nothing happens while one is already open */
static void pick_start(struct V *v)
{
	if (v->pick_pid)
		return;
	int p[2];
	if (pipe2(p, O_CLOEXEC) < 0)
		return;
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, p[1], 1);
	char *argv[] = {"flvideo", "--pick-file", NULL};
	pid_t pid;
	int rc = posix_spawn(&pid, "/proc/self/exe", &fa, NULL, argv, environ);
	posix_spawn_file_actions_destroy(&fa);
	close(p[1]);
	if (rc) {
		close(p[0]);
		say(v, 1, "Cannot start the file dialog: %s", strerror(rc));
		return;
	}
	v->pick_pid = pid;
	v->pick_fd = p[0];
	v->pick_len = 0;
	fl_fd_add(v->app, p[0], POLLIN, on_pick_fd, v);
}

static void draw(fl_ui *ui, void *ud);
static void on_close(fl_win *w, void *ud);

static void swap_tick(void *ud)
{
	struct V *v = ud;
	if (!v->want_fullscreen_swap)
		return;
	v->want_fullscreen_swap = 0;
	fl_win *old = v->win;
	int to_full = !v->fullscreen;
	fl_win_opts o = {
		.title = v->title[0] ? v->title : "Videos", .app_id = "flvideo", .w = to_full ? 0 : (v->win_w ? v->win_w : 960),
		.h = to_full ? 0 : (v->win_h ? v->win_h : 600), .min_w = 480, .min_h = 320, .decorated = !to_full,
		.fullscreen = to_full, .resizable = 1, .draw = draw, .close = on_close, .ud = v,
	};
	fl_win *nw = fl_win_new(v->app, &o);
	if (!nw)
		return;
	v->win = nw;
	v->fullscreen = to_full;
	v->vvalid = 0;
	v->menu = 0;
	fl_win_destroy(old);
	fl_win_redraw(nw);
}

static void on_close(fl_win *w, void *ud)
{
	struct V *v = ud;
	if (w == v->win)
		fl_app_quit(v->app);
}

static void hide_tick(void *ud)
{
	struct V *v = ud;
	fl_win_redraw(v->win);
}

/* ---- drawing ------------------------------------------------------------------------------ */

static void render_video(struct V *v, int w, int h)
{
	if (!v->rc || w < 2 || h < 2)
		return;
	int stride = ((w * 4 + 63) & ~63);
	size_t need = (size_t)stride * (size_t)h;
	if (v->vcap < need) {
		free(v->vbuf);
		v->vbuf = aligned_alloc(64, (need + 63) & ~(size_t)63);
		v->vcap = v->vbuf ? need : 0;
		if (!v->vbuf)
			return;
	}
	v->vstride = stride / 4;
	size_t bstride = (size_t)stride;
	int size[2] = {w, h}, block = 0;
	char *fmt = "bgr0";
	mpv_render_param prm[] = {
		{MPV_RENDER_PARAM_SW_SIZE, size},
		{MPV_RENDER_PARAM_SW_FORMAT, fmt},
		{MPV_RENDER_PARAM_SW_STRIDE, &bstride},
		{MPV_RENDER_PARAM_SW_POINTER, v->vbuf},
		{MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME, &block},
		{MPV_RENDER_PARAM_INVALID, NULL},
	};
	if (mpv_render_context_render(v->rc, prm) >= 0) {
		v->rw = w;
		v->rh = h;
		v->vvalid = 1;
	}
	v->need_render = 0;
}

static void blit_video(fl_canvas *cv, fl_rect r, const struct V *v)
{
	if (!cv->px || !v->vvalid || v->rw != r.w || v->rh != r.h)
		return;
	fl_rect c = fl_intersect(r, cv->clip);
	for (int y = c.y; y < c.y + c.h; y++) {
		const uint32_t *s = v->vbuf + (size_t)(y - r.y) * (size_t)v->vstride + (size_t)(c.x - r.x);
		uint32_t *d = cv->px + (size_t)y * (size_t)cv->stride + (size_t)c.x;
		for (int x = 0; x < c.w; x++)
			d[x] = s[x] | 0xff000000u;
	}
}

static void glyph_play(fl_canvas *cv, fl_rect r, fl_color c)
{
	fl_path *p = fl_path_new();
	float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f, s = r.w * 0.22f;
	fl_path_move(p, cx - s * 0.8f, cy - s * 1.2f);
	fl_path_line(p, cx + s * 1.3f, cy);
	fl_path_line(p, cx - s * 0.8f, cy + s * 1.2f);
	fl_path_close(p);
	fl_path_fill(cv, p, c);
	fl_path_free(p);
}

static void glyph_pause(fl_canvas *cv, fl_rect r, fl_color c)
{
	int cx = r.x + r.w / 2, cy = r.y + r.h / 2, s = r.w / 5;
	fl_fill_rect(cv, fl_r(cx - s - s / 2, cy - s * 5 / 4, s, s * 5 / 2), c);
	fl_fill_rect(cv, fl_r(cx + s / 2, cy - s * 5 / 4, s, s * 5 / 2), c);
}

static int slider(fl_ui *ui, uint32_t id, fl_rect r, double value, double *frac, int *released)
{
	int res = fl_widget(ui, id, r, 0);
	int ty = r.y + r.h / 2 - 2;
	fl_fill_rect(&ui->cv, fl_r(r.x, ty, r.w, 4), fl_mix(fl_th.line, 0xff000000, 40));
	int dragging = (res & FL_DOWN) != 0;
	if (dragging || (res & FL_CLICK)) {
		double f = (double)(ui->mx - r.x) / (r.w > 1 ? r.w : 1);
		*frac = f < 0 ? 0 : f > 1 ? 1 : f;
		value = *frac;
	}
	*released = (res & FL_CLICK) ? 1 : 0;
	int fw = (int)(value * r.w);
	fl_fill_rect(&ui->cv, fl_r(r.x, ty, fw, 4), fl_th.accent);
	if (res & (FL_HOVER | FL_DOWN))
		fl_fill_rect(&ui->cv, fl_r(r.x + fw - 5, r.y + r.h / 2 - 7, 10, 14), fl_th.strong);
	return dragging;
}

static int flat_button(fl_ui *ui, uint32_t id, fl_rect r, const char *label)
{
	return fl_button(ui, id, r, label, FL_BTN_PLAIN);
}

static void draw_menu(fl_ui *ui, struct V *v, fl_rect area)
{
	fl_canvas *cv = &ui->cv;
	struct trk *t = v->menu == 2 ? v->sub : v->aud;
	int n = v->menu == 2 ? v->nsub : v->naud;
	int items = n + (v->menu == 2 ? 1 : 0);
	int w = 340, h = items * 32 + 8;
	if (items == 0) {
		items = 1;
		h = 40;
	}
	int x = v->menu_anchor.x + v->menu_anchor.w - w;
	if (x < area.x + 8)
		x = area.x + 8;
	int y = v->menu_anchor.y - h - 6;
	fl_rect box = fl_r(x, y, w, h);
	fl_fill_rect(cv, box, fl_th.raised);
	fl_stroke_rect(cv, box, 1, fl_th.line);
	int pick = -2, any_sel = 0;
	if (n == 0) {
		fl_text_in(cv, ui->f_reg, box, FL_CENTER, 0, fl_th.dim, v->menu == 2 ? "No subtitles" : "No audio tracks");
	}
	int row = 0;
	if (v->menu == 2) {
		for (int i = 0; i < n; i++)
			any_sel |= t[i].sel;
		fl_rect r = fl_r(box.x + 4, box.y + 4, w - 8, 32);
		int res = fl_widget(ui, ID_MENU, r, 0);
		if (res & FL_HOVER)
			fl_fill_rect(cv, r, fl_th.select);
		fl_text_in(cv, ui->f_reg, r, FL_LEFT, 12, !any_sel ? fl_th.accent : fl_th.text, "Off");
		if (res & FL_CLICK)
			pick = -1;
		row = 1;
	}
	for (int i = 0; i < n; i++, row++) {
		fl_rect r = fl_r(box.x + 4, box.y + 4 + row * 32, w - 8, 32);
		int res = fl_widget(ui, ID_MENU + 1 + (uint32_t)i, r, 0);
		if (res & FL_HOVER)
			fl_fill_rect(cv, r, fl_th.select);
		char fit[200];
		fl_text_fit(ui->f_reg, t[i].label, w - 32, fit, sizeof fit);
		fl_text_in(cv, ui->f_reg, r, FL_LEFT, 12, t[i].sel ? fl_th.accent : fl_th.text, fit);
		if (res & FL_CLICK)
			pick = i;
	}
	if (pick >= -1) {
		if (pick < 0)
			set_prop(v, "sid", "no");
		else {
			char id[16];
			snprintf(id, sizeof id, "%d", t[pick].id);
			set_prop(v, v->menu == 2 ? "sid" : "aid", id);
		}
		v->menu = 0;
	} else if ((ui->press & (FL_BTN_L | FL_BTN_R)) && !fl_in(box, ui->mx, ui->my) && !fl_in(v->menu_anchor, ui->mx, ui->my)) {
		v->menu = 0;
	}
}

static void keys(fl_ui *ui, struct V *v)
{
	uint32_t k = ui->key;
	if (!k)
		return;
	int mods = fl_app_mods(v->app), shift = mods & FL_MOD_SHIFT, ctrl = mods & FL_MOD_CTRL;
	int handled = 1;
	switch (k) {
	case XKB_KEY_space:
	case XKB_KEY_k: toggle_pause(v); break;
	case XKB_KEY_Right: seek_rel(v, ctrl ? 60 : shift ? 30 : 5); break;
	case XKB_KEY_Left: seek_rel(v, ctrl ? -60 : shift ? -30 : -5); break;
	case XKB_KEY_l: seek_rel(v, 10); break;
	case XKB_KEY_j: seek_rel(v, -10); break;
	case XKB_KEY_Up: set_volume(v, v->volume + 5); break;
	case XKB_KEY_Down: set_volume(v, v->volume - 5); break;
	case XKB_KEY_m: cmd(v, "cycle", "mute", NULL, NULL); break;
	case XKB_KEY_f:
	case XKB_KEY_F11: toggle_fullscreen(v); break;
	case XKB_KEY_Escape:
		if (v->menu)
			v->menu = 0;
		else if (v->fullscreen)
			toggle_fullscreen(v);
		else
			handled = 0;
		break;
	case XKB_KEY_v: cycle_track(v, 1); break;
	case XKB_KEY_a: cycle_track(v, 0); break;
	case XKB_KEY_bracketleft: {
		char s[16];
		v->speed = v->speed > 0.55 ? v->speed - 0.25 : v->speed;
		snprintf(s, sizeof s, "%.2f", v->speed);
		set_prop(v, "speed", s);
		break;
	}
	case XKB_KEY_bracketright: {
		char s[16];
		v->speed = v->speed < 3.9 ? v->speed + 0.25 : v->speed;
		snprintf(s, sizeof s, "%.2f", v->speed);
		set_prop(v, "speed", s);
		break;
	}
	case XKB_KEY_o:
		if (ctrl)
			pick_start(v);
		else
			handled = 0;
		break;
	case XKB_KEY_q: fl_app_quit(v->app); break;
	default: handled = 0;
	}
	if (handled)
		ui->key = 0;
}

static void draw(fl_ui *ui, void *ud)
{
	struct V *v = ud;
	fl_canvas *cv = &ui->cv;
	fl_rect c = fl_win_content(ui->win);
	if (!v->fullscreen) {
		v->win_w = c.w;
		v->win_h = c.h + 34;
	}
	long long now = now_ms();
	if (ui->mx != v->last_mx || ui->my != v->last_my) {
		v->last_mx = ui->mx;
		v->last_my = ui->my;
		v->last_move_ms = now;
		fl_timer_arm(v->hide_timer, HIDE_MS + 50, 0);
	}
	int show_bar = !v->fullscreen || v->paused || v->menu || v->seeking || !v->loaded ||
	               now - v->last_move_ms < HIDE_MS || !v->mpv;
	keys(ui, v);

	fl_rect video = c, bar = {0};
	if (v->fullscreen) {
		bar = fl_r(c.x, c.y + c.h - BAR_H, c.w, BAR_H);
	} else {
		bar = fl_cut_bottom(&video, BAR_H);
	}
	fl_fill_rect(cv, c, FL_HEX(0x000000));

	/* the picture */
	int vres = fl_widget(ui, ID_VIDEO, video, FL_FOCUSABLE | FL_NO_FOCUS_RING);
	if (v->loaded && v->rc) {
		if (v->need_render || !v->vvalid || v->rw != video.w || v->rh != video.h)
			if (cv->px)
				render_video(v, video.w, video.h);
		blit_video(cv, video, v);
	}
	if ((vres & FL_CLICK) && !v->menu) {
		if (ui->clicks >= 2)
			toggle_fullscreen(v);
		else
			toggle_pause(v);
	}
	if (!v->path[0] || !v->mpv) {
		fl_text_in(cv, ui->f_reg, video, FL_CENTER, 0, fl_th.dim,
		           v->mpv ? "Open a video: the Open button or Ctrl+O" : "The video engine did not start");
	} else if (!v->loaded && !v->msg_err) {
		fl_text_in(cv, ui->f_reg, video, FL_CENTER, 0, fl_th.dim, "Opening...");
	}
	if (v->buffering)
		fl_text_in(cv, ui->f_reg, video, FL_CENTER, 0, fl_th.text, "Buffering...");

	/* the control bar */
	if (show_bar) {
		fl_color bg = v->fullscreen ? fl_mix(fl_th.panel, 0xff000000, 30) : fl_th.panel;
		fl_fill_rect(cv, bar, bg);
		fl_fill_rect(cv, fl_r(bar.x, bar.y, bar.w, 1), fl_th.line);
		fl_rect in = fl_inset(bar, 14, 8);
		/* the seek row */
		fl_rect row1 = fl_cut_top(&in, 22);
		char a[24], b[24];
		double dur = v->dur > 0 ? v->dur : 0;
		double frac = dur > 0 ? v->pos / dur : 0;
		if (frac > 1)
			frac = 1;
		fmt_time(v->seeking ? v->seek_frac * dur : v->pos, a, sizeof a);
		fmt_time(dur, b, sizeof b);
		fl_rect tl = fl_cut_left(&row1, 52);
		fl_rect tr = fl_cut_right(&row1, 52);
		fl_text_in(cv, v->small, tl, FL_LEFT, 0, fl_th.dim, a);
		fl_text_in(cv, v->small, tr, FL_RIGHT, 0, fl_th.dim, b);
		double sf = frac;
		int released;
		int drag = slider(ui, ID_SEEK, fl_inset(row1, 6, 0), frac, &sf, &released);
		if (drag) {
			v->seeking = 1;
			v->seek_frac = sf;
		}
		if (released && dur > 0) {
			char s[32];
			snprintf(s, sizeof s, "%.2f", sf * dur);
			cmd(v, "seek", s, "absolute", NULL);
			v->pos = sf * dur;
			v->seeking = 0;
		}
		if (!drag && !released)
			v->seeking = 0;
		/* buttons */
		fl_rect row2 = in;
		row2.y += 2;
		row2.h = 34;
		int x = row2.x;
		fl_rect pb = fl_r(x, row2.y, 42, 34);
		int res = fl_widget(ui, ID_PLAY, pb, FL_FOCUSABLE | FL_NO_FOCUS_RING);
		fl_color pbg = fl_th.accent;
		if (res & FL_DOWN)
			pbg = fl_mix(pbg, 0xff000000, 40);
		else if (res & FL_HOVER)
			pbg = fl_mix(pbg, 0xffffffff, 28);
		fl_fill_rect(cv, pb, pbg);
		if (v->paused || v->ended)
			glyph_play(cv, pb, fl_th.on_accent);
		else
			glyph_pause(cv, pb, fl_th.on_accent);
		if (res & FL_CLICK)
			toggle_pause(v);
		x += 50;
		if (flat_button(ui, ID_BACK, fl_r(x, row2.y, 52, 34), "-10 s"))
			seek_rel(v, -10);
		x += 54;
		if (flat_button(ui, ID_FWD, fl_r(x, row2.y, 52, 34), "+10 s"))
			seek_rel(v, 10);
		x += 60;
		if (flat_button(ui, ID_OPEN, fl_r(x, row2.y, 52, 34), "Open"))
			pick_start(v);
		x += 56;
		/* the right side */
		int rx = row2.x + row2.w;
		rx -= 88;
		fl_rect fb = fl_r(rx, row2.y, 88, 34);
		if (flat_button(ui, ID_FULL, fb, v->fullscreen ? "Exit full" : "Fullscreen"))
			toggle_fullscreen(v);
		rx -= 4 + 90;
		fl_rect vol = fl_r(rx, row2.y + 5, 90, 24);
		double vf = v->muted ? 0 : v->volume / 100.0, vfr = vf;
		int rel;
		if (slider(ui, ID_VOL, vol, vf, &vfr, &rel))
			set_volume(v, vfr * 100);
		rx -= 38;
		if (fl_icon_button(ui, ID_MUTE, fl_r(rx, row2.y, 34, 34), v->muted ? FL_ICON_MUTE : FL_ICON_VOLUME, 18, fl_th.text))
			cmd(v, "cycle", "mute", NULL, NULL);
		rx -= 66;
		fl_rect sb = fl_r(rx, row2.y, 62, 34);
		if (flat_button(ui, ID_SUBS, sb, "Subtitles")) {
			v->menu = v->menu == 2 ? 0 : 2;
			v->menu_anchor = sb;
		}
		int sel_sub = 0;
		for (int i = 0; i < v->nsub; i++)
			sel_sub |= v->sub[i].sel;
		if (sel_sub)
			fl_fill_rect(cv, fl_r(sb.x + 8, sb.y + sb.h - 3, sb.w - 16, 2), fl_th.accent);
		rx -= 4 + 54;
		fl_rect ab = fl_r(rx, row2.y, 54, 34);
		if (flat_button(ui, ID_AUDIO, ab, "Audio")) {
			v->menu = v->menu == 1 ? 0 : 1;
			v->menu_anchor = ab;
		}
		rx -= 4 + 52;
		char sp[16];
		snprintf(sp, sizeof sp, "%.2gx", v->speed);
		if (flat_button(ui, ID_SPEED, fl_r(rx, row2.y, 52, 34), sp))
			cycle_speed(v);
		/* the file name in the middle */
		int tx = x + 8, tw = rx - tx - 8;
		if (tw > 80) {
			const char *name = v->title[0] ? v->title : v->path;
			const char *sl = !v->title[0] ? strrchr(name, '/') : NULL;
			if (sl)
				name = sl + 1;
			char fit[400];
			fl_text_fit(ui->f_med, name, tw, fit, sizeof fit);
			fl_text_in(cv, ui->f_med, fl_r(tx, row2.y, tw, 34), FL_LEFT, 0, fl_th.text, fit);
		}
		if (v->menu)
			draw_menu(ui, v, c);
	}
	if (v->msg[0]) {
		int mw = fl_text_width(ui->f_reg, v->msg, -1) + 32;
		if (mw > c.w - 40)
			mw = c.w - 40;
		fl_rect m = fl_r(c.x + (c.w - mw) / 2, video.y + 20, mw, 34);
		fl_fill_rect(cv, m, fl_th.raised);
		fl_stroke_rect(cv, m, 1, v->msg_err ? fl_th.err : fl_th.line);
		fl_text_in(cv, ui->f_reg, m, FL_CENTER, 0, v->msg_err ? fl_th.err : fl_th.text, v->msg);
	}
}

/* ---- main ------------------------------------------------------------------------------- */

static void usage(void)
{
	fprintf(stderr, "usage: flvideo [--start SECONDS] [--pause] [FILE]\n"
	                "Play a video. Space pauses, F is fullscreen, V and A switch subtitles and audio,\n"
	                "Ctrl+O or the Open button picks a file.\n");
}

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--pick-file"))
		return pick_child();
	struct V *v = calloc(1, sizeof *v);
	if (!v)
		return 1;
	v->pick_fd = -1;
	const char *file = NULL;
	int explicit_start = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			usage();
			return 0;
		} else if (!strcmp(argv[i], "--start") && i + 1 < argc) {
			v->start_at = atof(argv[++i]);
			explicit_start = 1;
		} else if (!strcmp(argv[i], "--pause")) {
			v->start_paused = 1;
		} else if (argv[i][0] == '-' && argv[i][1]) {
			usage();
			return 2;
		} else if (!file) {
			file = argv[i];
		} else {
			usage();
			return 2;
		}
	}
	if (file) {
		char real[PATH_MAX];
		if (!realpath(file, real)) {
			fprintf(stderr, "flvideo: cannot open %s: %s\n", file, strerror(errno));
			return 1;
		}
		snprintf(v->path, sizeof v->path, "%s", real);
		if (!explicit_start)
			v->start_at = resume_get(v->path);
	}
	if (pipe2(v->wake, O_NONBLOCK | O_CLOEXEC) < 0 || pipe2(v->upd, O_NONBLOCK | O_CLOEXEC) < 0)
		return 1;
	/* mpv needs the C numeric locale: this program never calls setlocale, so it has it */
	v->app = fl_app_new(NULL, NULL);
	if (!v->app) {
		fprintf(stderr, "flvideo: cannot connect to a Wayland compositor\n");
		return 1;
	}
	v->small = fl_font_ui(400, 12);
	v->hide_timer = fl_timer_new(v->app, hide_tick, v);
	v->swap_timer = fl_timer_new(v->app, swap_tick, v);
	v->save_timer = fl_timer_new(v->app, save_tick, v);
	v->msg_timer = fl_timer_new(v->app, msg_clear, v);
	v->last_move_ms = now_ms();
	fl_win_opts o = {
		.title = "Videos", .app_id = "flvideo", .w = 960, .h = 600, .min_w = 480, .min_h = 320,
		.decorated = 1, .resizable = 1, .draw = draw, .close = on_close, .ud = v,
	};
	v->win = fl_win_new(v->app, &o);
	if (!v->win)
		return 1;
	if (mpv_start(v, file ? v->path : NULL) < 0) {
		fprintf(stderr, "flvideo: libmpv did not start\n");
		if (v->mpv) {
			mpv_terminate_destroy(v->mpv);
			v->mpv = NULL;
		}
	} else {
		fl_fd_add(v->app, v->wake[0], POLLIN, on_wake_fd, v);
		fl_fd_add(v->app, v->upd[0], POLLIN, on_upd_fd, v);
		fl_timer_arm(v->save_timer, 15000, 15000);
	}
	fl_app_watch_conf(v->app, NULL, NULL);
	fl_app_run(v->app);
	save_position(v);
	if (v->pick_pid)
		kill(v->pick_pid, SIGTERM);
	if (v->rc)
		mpv_render_context_free(v->rc);
	if (v->mpv)
		mpv_terminate_destroy(v->mpv);
	fl_app_free(v->app);
	return 0;
}
