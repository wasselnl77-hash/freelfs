/* notify.c -- notifications: the D-Bus service, the cards, the notification centre
 * SPDX-License-Identifier: MIT
 *
 * The panel owns org.freedesktop.Notifications on the session bus (spec 1.2: Notify,
 * CloseNotification, GetCapabilities, GetServerInformation, NotificationClosed,
 * ActionInvoked), so there is no second daemon and no second copy of the fonts in
 * memory. libdbus runs on its own private connection whose fd sits in flui's loop. The
 * panel starts before the session script has started dbus-daemon, so connecting is
 * retried every second (then every ten) until the bus is there.
 *
 * Cards: a transparent surface at the top right of the screen (fl_shell
 * set_notifications), at most four cards stacked, newest on top, each with the app's
 * tile, its name, summary, body and up to three action buttons. A card goes away by
 * itself (5 or 6 s; critical ones stay), when the pointer rests on the stack its timers wait,
 * and a click on the card (default action) or its cross dismisses it. While the screen is
 * locked the cards are not shown and their timers wait.
 *
 * Centre: the history as a flyout above the clock (click the clock): a switch for Do not
 * disturb (desktop.conf notify_dnd=1: no cards for low and normal urgency, critical ones
 * still show; everything lands in the history), Clear all, the entries newest first, each
 * with a cross. The history keeps 50 entries and lives in $XDG_RUNTIME_DIR, so it survives a
 * restart of the panel but not the session.
 *
 * Not done: images (image-data, image-path, app icons from a theme: the tile shows a
 * built-in icon for known apps, else the first letter of the app's name), markup (tags are
 * stripped), sound, the "inline-reply" and "x-canonical-*" extensions.
 *
 * Extra D-Bus interface org.freelfs.Notifications on the same object, for flnotify and
 * scripts: SetDoNotDisturb(b), GetDoNotDisturb() -> b, ClearAll(), ToggleCenter(),
 * GetCount() -> u.
 */
#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <flui.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "flpanel.h"
#include "sound.h"

#define NOTIF_PATH "/org/freedesktop/Notifications"
#define NOTIF_IFACE "org.freedesktop.Notifications"
#define NOTIF_NAME "org.freedesktop.Notifications"
#define FL_IFACE "org.freelfs.Notifications"

#define MAX_HIST 50
#define MAX_CARDS 4
#define CARD_W 360
#define CARD_GAP 8
#define NC_W 380
#define NC_HEAD 52
#define NC_DND 44

struct action {
	char key[48], label[64];
};

struct notif {
	uint32_t id;
	char app[64], icon[64], summary[160], body[512];
	int urgency;
	int n_actions;
	struct action act[4];
	int has_default, internal, transient;
	time_t when;
	int unread;
	int card;            /* shown (or waiting to be shown) as a card */
	long deadline;       /* monotonic ms the card goes away, 0: not yet started, -1: never */
	int timeout_ms;      /* what to start the deadline from, 0: never */
};

struct notify {
	struct panel *p;
	struct notif h[MAX_HIST]; /* newest first */
	int n;
	uint32_t next_id;
	int dnd;
	int unread_since_open;
	/* D-Bus */
	DBusConnection *bus;
	int fd, tries, passive;
	fl_timer *retry, *tick, *save;
	/* windows */
	fl_win *cards, *center;
	int cards_hover, cards_w, cards_h, center_open, center_scroll, center_h;
	char path[300];
};

/* ---- small helpers ------------------------------------------------------------------------- */

static long mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Strip the markup the spec allows (<b> <i> <u> <a href> <img>) and decode the five entities. */
static void plain_text(char *dst, size_t cap, const char *src)
{
	size_t n = 0;
	for (const char *s = src; *s && n + 1 < cap; s++) {
		if (*s == '<') {
			const char *e = strchr(s, '>');
			if (e && e - s < 200 && (s[1] == '/' || s[1] == 'b' || s[1] == 'i' || s[1] == 'u' || s[1] == 'a' ||
			                         !strncmp(s + 1, "img", 3) || s[1] == 'B' || s[1] == 'I' || s[1] == 'U')) {
				if (!strncmp(s + 1, "br", 2))
					dst[n++] = '\n';
				s = e;
				continue;
			}
		}
		if (*s == '&') {
			static const struct { const char *e; char c; } ent[] = {
				{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''},
			};
			int hit = 0;
			for (size_t i = 0; i < sizeof ent / sizeof *ent; i++)
				if (!strncmp(s, ent[i].e, strlen(ent[i].e))) {
					dst[n++] = ent[i].c;
					s += strlen(ent[i].e) - 1;
					hit = 1;
					break;
				}
			if (hit)
				continue;
		}
		if ((unsigned char)*s < 32 && *s != '\n' && *s != '\t')
			continue;
		dst[n++] = *s == '\t' ? ' ' : *s;
	}
	dst[n] = 0;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
	size_t n = strlen(src);
	if (n >= cap) {
		n = cap - 1;
		while (n > 0 && ((unsigned char)src[n] & 0xc0) == 0x80)
			n--; /* do not cut a character in half */
	}
	memcpy(dst, src, n);
	dst[n] = 0;
}

/* time of an entry for the centre: "now", "5 min ago", "14:32", "Yesterday 14:32", "5 Nov" */
static void when_text(time_t t, char *out, size_t cap)
{
	time_t now = time(NULL);
	long d = (long)(now - t);
	struct tm a, b;
	localtime_r(&t, &a);
	localtime_r(&now, &b);
	if (d < 60)
		snprintf(out, cap, "now");
	else if (d < 3600)
		snprintf(out, cap, "%ld min ago", d / 60);
	else if (a.tm_yday == b.tm_yday && a.tm_year == b.tm_year)
		strftime(out, cap, "%H:%M", &a);
	else if (d < 172800 && (b.tm_yday - a.tm_yday == 1 || (b.tm_yday == 0 && a.tm_mon == 11 && a.tm_mday == 31)))
		strftime(out, cap, "Yesterday %H:%M", &a);
	else
		strftime(out, cap, "%-d %b", &a);
}

/* ---- text wrapping -------------------------------------------------------------------------- */

struct line {
	char t[200];
};

static int cp_len(const char *s)
{
	unsigned char c = (unsigned char)*s;
	return c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
}

/* At most maxl lines of s that fit maxw; the last one ends in "..." when there is more. */
static int wrap(fl_font *f, const char *s, int maxw, struct line *out, int maxl)
{
	int n = 0;
	const char *p = s;
	while (*p && n < maxl) {
		while (*p == ' ')
			p++;
		if (!*p)
			break;
		if (*p == '\n') { /* an empty line */
			p++;
			out[n++].t[0] = 0;
			continue;
		}
		const char *q = p, *brk = NULL;
		const char *end;
		for (;;) {
			if (!*q || *q == '\n') {
				end = q;
				break;
			}
			const char *q2 = q + cp_len(q);
			if (fl_text_width(f, p, (int)(q2 - p)) > maxw && q > p) {
				end = brk ? brk : q;
				break;
			}
			if (*q == ' ')
				brk = q;
			q = q2;
		}
		size_t len = (size_t)(end - p);
		if (len >= sizeof out[n].t)
			len = sizeof out[n].t - 1;
		memcpy(out[n].t, p, len);
		out[n].t[len] = 0;
		while (len && out[n].t[len - 1] == ' ')
			out[n].t[--len] = 0;
		p = end;
		if (*p == '\n')
			p++;
		n++;
	}
	if (*p && n == maxl && n > 0) { /* more text than lines: the last line ends in "..." */
		char rest[800];
		/* the last line again, with the remainder behind it */
		snprintf(rest, sizeof rest, "%s %s", out[n - 1].t, p);
		for (char *c = rest; *c; c++)
			if (*c == '\n')
				*c = ' ';
		fl_text_fit(f, rest, maxw, out[n - 1].t, sizeof out[n - 1].t);
	}
	return n;
}

/* ---- the history ----------------------------------------------------------------------------- */

static void save_arm(struct notify *nt)
{
	if (nt->save)
		fl_timer_arm(nt->save, 800, 0);
}

static void esc_write(FILE *f, const char *s)
{
	for (; *s; s++) {
		if (*s == '\\')
			fputs("\\\\", f);
		else if (*s == '\n')
			fputs("\\n", f);
		else if (*s == '\t')
			fputs("\\t", f);
		else
			fputc(*s, f);
	}
}

static void history_save(void *ud)
{
	struct notify *nt = ud;
	if (!nt->path[0])
		return;
	char tmp[320];
	snprintf(tmp, sizeof tmp, "%s.tmp", nt->path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "FLN1 %u\n", nt->next_id);
	for (int i = nt->n - 1; i >= 0; i--) { /* oldest first, so loading inserts in order */
		struct notif *x = &nt->h[i];
		fprintf(f, "%u\t%ld\t%d\t%d\t", x->id, (long)x->when, x->urgency, x->unread);
		esc_write(f, x->app);
		fputc('\t', f);
		esc_write(f, x->icon);
		fputc('\t', f);
		esc_write(f, x->summary);
		fputc('\t', f);
		esc_write(f, x->body);
		fputc('\n', f);
	}
	if (fclose(f) == 0)
		rename(tmp, nt->path);
	else
		unlink(tmp);
}

static void unesc(char *s)
{
	char *d = s;
	for (; *s; s++) {
		if (*s == '\\' && s[1]) {
			s++;
			*d++ = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s;
		} else {
			*d++ = *s;
		}
	}
	*d = 0;
}

static void hist_insert(struct notify *nt, const struct notif *x)
{
	if (nt->n == MAX_HIST)
		nt->n--; /* the oldest falls off */
	memmove(&nt->h[1], &nt->h[0], (size_t)nt->n * sizeof nt->h[0]);
	nt->h[0] = *x;
	nt->n++;
}

static void history_load(struct notify *nt)
{
	FILE *f = nt->path[0] ? fopen(nt->path, "r") : NULL;
	if (!f)
		return;
	char line[1400];
	unsigned next = 0;
	if (!fgets(line, sizeof line, f) || sscanf(line, "FLN1 %u", &next) != 1) {
		fclose(f);
		return;
	}
	if (next > nt->next_id)
		nt->next_id = next;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = 0;
		char *fld[8];
		int k = 0;
		char *s = line;
		while (k < 8) {
			fld[k++] = s;
			char *t = strchr(s, '\t');
			if (!t)
				break;
			*t = 0;
			s = t + 1;
		}
		if (k < 8)
			continue;
		struct notif x;
		memset(&x, 0, sizeof x);
		x.id = (uint32_t)strtoul(fld[0], NULL, 10);
		x.when = (time_t)atol(fld[1]);
		x.urgency = atoi(fld[2]);
		x.unread = atoi(fld[3]);
		for (int i = 4; i < 8; i++)
			unesc(fld[i]);
		copy_str(x.app, sizeof x.app, fld[4]);
		copy_str(x.icon, sizeof x.icon, fld[5]);
		copy_str(x.summary, sizeof x.summary, fld[6]);
		copy_str(x.body, sizeof x.body, fld[7]);
		hist_insert(nt, &x);
		if (x.id >= nt->next_id)
			nt->next_id = x.id + 1;
	}
	fclose(f);
	for (int i = 0; i < nt->n; i++)
		if (nt->h[i].unread)
			nt->unread_since_open++;
}

static struct notif *find_id(struct notify *nt, uint32_t id)
{
	for (int i = 0; i < nt->n; i++)
		if (nt->h[i].id == id)
			return &nt->h[i];
	return NULL;
}

/* the badge counts the unread ones; while the centre is open they are shown there, not counted */
static void recount_unread(struct notify *nt)
{
	nt->unread_since_open = 0;
	if (nt->center_open)
		return;
	for (int i = 0; i < nt->n; i++)
		if (nt->h[i].unread)
			nt->unread_since_open++;
}

/* ---- D-Bus signals ----------------------------------------------------------------------------- */

static void emit_closed(struct notify *nt, uint32_t id, uint32_t reason)
{
	if (!nt->bus)
		return;
	DBusMessage *m = dbus_message_new_signal(NOTIF_PATH, NOTIF_IFACE, "NotificationClosed");
	if (!m)
		return;
	dbus_message_append_args(m, DBUS_TYPE_UINT32, &id, DBUS_TYPE_UINT32, &reason, DBUS_TYPE_INVALID);
	dbus_connection_send(nt->bus, m, NULL);
	dbus_message_unref(m);
	dbus_connection_flush(nt->bus);
}

static void action_invoked(struct notify *nt, struct notif *x, const char *key)
{
	if (x->internal) {
		char k[48];
		snprintf(k, sizeof k, "%s", key);
		battery_action(nt->p, k);
		return;
	}
	if (!nt->bus)
		return;
	DBusMessage *m = dbus_message_new_signal(NOTIF_PATH, NOTIF_IFACE, "ActionInvoked");
	if (!m)
		return;
	uint32_t id = x->id;
	dbus_message_append_args(m, DBUS_TYPE_UINT32, &id, DBUS_TYPE_STRING, &key, DBUS_TYPE_INVALID);
	dbus_connection_send(nt->bus, m, NULL);
	dbus_message_unref(m);
	dbus_connection_flush(nt->bus);
}

void draw_bell(fl_canvas *cv, int x, int y, int size, fl_color c)
{
	fl_path *b = fl_path_new();
	if (!b)
		return;
	float k = (float)size / 16.0f;
	fl_path_svg(b, "M8 1.8c-2.6 0-4.3 1.9-4.3 4.4v2.4L2.2 11.4h11.6l-1.5-2.8V6.2C12.3 3.7 10.6 1.8 8 1.8z"
	               "M6.3 12.6c0 1 .8 1.7 1.7 1.7s1.7-.7 1.7-1.7z", k, k, (float)x, (float)y);
	fl_path_fill(cv, b, c);
	fl_path_free(b);
}

/* ---- the cards ------------------------------------------------------------------------------------ */

static void remove_hist(struct notify *nt, int i)
{
	memmove(&nt->h[i], &nt->h[i + 1], (size_t)(nt->n - i - 1) * sizeof nt->h[0]);
	nt->n--;
}

/* the visible cards, newest first: indexes into h, at most MAX_CARDS */
static int visible_cards(struct notify *nt, int *idx)
{
	int c = 0;
	for (int i = 0; i < nt->n && c < MAX_CARDS; i++)
		if (nt->h[i].card)
			idx[c++] = i;
	return c;
}

/* heights: padding, app line, summary, body lines, action row */
static int card_body_lines(struct notify *nt, const struct notif *x, struct line *lines)
{
	return x->body[0] ? wrap(nt->p->f_what, x->body, CARD_W - 14 - 36 - 12 - 14 - 8, lines, 3) : 0;
}

static int card_height(struct notify *nt, const struct notif *x)
{
	struct line l[3];
	int bl = card_body_lines(nt, x, l);
	int nb = 0;
	for (int i = 0; i < x->n_actions && nb < 3; i++)
		if (strcmp(x->act[i].key, "default"))
			nb++;
	int h = 12 + 16 + 20 + (bl ? 2 + bl * 17 : 0) + 12;
	if (nb)
		h += 10 + 28;
	if (h < 12 + 36 + 12 + 4)
		h = 12 + 36 + 12 + 4;
	return h;
}

static void cards_layout(struct notify *nt)
{
	if (!nt->cards)
		return;
	int idx[MAX_CARDS];
	int c = visible_cards(nt, idx);
	long now = mono_ms();
	int h = 0;
	for (int k = 0; k < c; k++) {
		struct notif *x = &nt->h[idx[k]];
		if (x->deadline == 0)
			x->deadline = x->timeout_ms > 0 ? now + x->timeout_ms : -1;
		h += card_height(nt, x) + (k ? CARD_GAP : 0);
	}
	if (nt->p->locked || c == 0) {
		if (nt->cards_h || c == 0) {
			panel_hide(nt->cards);
			nt->cards_h = 0;
		}
	} else {
		if (h != nt->cards_h || nt->cards_w != CARD_W) {
			fl_win_resize(nt->cards, CARD_W, h);
			nt->cards_h = h;
			nt->cards_w = CARD_W;
		}
		fl_win_map(nt->cards);
		fl_win_redraw(nt->cards);
	}
	/* wake up at the next deadline */
	long next = -1;
	for (int k = 0; k < c; k++) {
		long d = nt->h[idx[k]].deadline;
		if (d > 0 && (next < 0 || d < next))
			next = d;
	}
	if (next < 0 || nt->p->locked)
		fl_timer_arm(nt->tick, 0, 0);
	else
		fl_timer_arm(nt->tick, (int)(next > now ? next - now + 5 : 5), 0);
}

static void card_drop(struct notify *nt, struct notif *x, int reason, int send)
{
	if (!x->card)
		return;
	x->card = 0;
	x->deadline = 0;
	if (send && reason)
		emit_closed(nt, x->id, (uint32_t)reason);
}

/* transient notifications leave the history with their card */
static void prune(struct notify *nt)
{
	for (int i = nt->n - 1; i >= 0; i--)
		if (nt->h[i].transient && !nt->h[i].card)
			remove_hist(nt, i);
	recount_unread(nt);
}

static void cards_tick(void *ud)
{
	struct notify *nt = ud;
	int idx[MAX_CARDS];
	int c = visible_cards(nt, idx);
	long now = mono_ms();
	for (int k = 0; k < c; k++) {
		struct notif *x = &nt->h[idx[k]];
		if (x->deadline > 0 && x->deadline <= now) {
			if (nt->cards_hover || nt->p->locked)
				x->deadline = now + 2000; /* the pointer is on the stack: it waits */
			else
				card_drop(nt, x, 1, 1);
		}
	}
	prune(nt);
	cards_layout(nt);
}

/* a card's tile: a built-in icon for known apps, else the first letter */
static void app_tile(fl_ui *ui, fl_rect r, const struct notif *x, fl_color bg, fl_color fg)
{
	fl_fill_rect(&ui->cv, r, bg);
	const char *id = x->icon[0] ? x->icon : x->app;
	char low[64];
	size_t i = 0;
	for (; id[i] && i + 1 < sizeof low; i++)
		low[i] = (char)((id[i] >= 'A' && id[i] <= 'Z') ? id[i] + 32 : id[i]);
	low[i] = 0;
	fl_icon ic = fl_icon_for_app(low);
	if (ic != FL_ICON_WINDOW || !strcmp(low, "window")) {
		fl_icon_draw(&ui->cv, ic, r.x + (r.w - 20) / 2, r.y + (r.h - 20) / 2, 20, fg);
		return;
	}
	char one[8] = {0};
	const char *nm = x->app[0] ? x->app : x->summary;
	int n = cp_len(nm);
	memcpy(one, nm, (size_t)n < sizeof one ? (size_t)n : sizeof one - 1);
	if (n == 1 && one[0] >= 'a' && one[0] <= 'z')
		one[0] = (char)(one[0] - 32);
	fl_text_in(&ui->cv, ui->f_semi, r, FL_CENTER, 0, fg, one[0] ? one : "?");
}

static fl_color urgency_color(int u)
{
	return u >= 2 ? fl_th.err : u == 0 ? fl_th.line : fl_th.accent;
}

static void cards_draw(fl_ui *ui, void *ud)
{
	struct notify *nt = ud;
	struct panel *p = nt->p;
	fl_canvas *cv = &ui->cv;
	if (cv->px) /* a transparent surface: clear what the buffer held two frames ago */
		for (int y = 0; y < cv->h; y++)
			memset(cv->px + (size_t)y * cv->stride, 0, (size_t)cv->w * 4);
	nt->cards_hover = ui->mouse_in;
	int idx[MAX_CARDS];
	int c = visible_cards(nt, idx);
	int y = 0;
	uint32_t drop_id = 0, act_id = 0;
	char act_key[48] = "";
	int drop_reason = 0;
	for (int k = 0; k < c; k++) {
		struct notif *x = &nt->h[idx[k]];
		int ch = card_height(nt, x);
		fl_rect cr = fl_r(0, y, CARD_W, ch);
		int hover = ui->mouse_in && fl_in(cr, ui->mx, ui->my);
		fl_fill_rect(cv, cr, hover ? fl_mix(fl_th.panel, fl_th.raised, 110) : fl_th.panel);
		fl_stroke_rect(cv, cr, 1, fl_th.line);
		fl_fill_rect(cv, fl_r(0, y, 3, ch), urgency_color(x->urgency));
		uint32_t base = 100 + (uint32_t)k * 10;
		/* the pieces on top first (their ids win the click), the card itself last */
		fl_rect cx = fl_r(CARD_W - 30, y + 6, 24, 24);
		int cres = fl_widget(ui, base + 1, cx, 0);
		if (cres & (FL_HOVER | FL_DOWN))
			fl_fill_rect(cv, cx, fl_th.raised);
		fl_icon_draw(cv, FL_ICON_CLOSE, cx.x + 6, cx.y + 6, 12, (cres & FL_HOVER) ? fl_th.text : fl_th.dim);
		if (cres & FL_CLICK) {
			drop_id = x->id;
			drop_reason = 2;
		}
		app_tile(ui, fl_r(14, y + 12, 36, 36), x, x->urgency >= 2 ? fl_mix(fl_th.raised, fl_th.err, 70) : fl_th.raised,
		         x->urgency >= 2 ? fl_th.strong : fl_th.text);
		int tx = 14 + 36 + 12, tw = CARD_W - tx - 14 - 8;
		int ty = y + 12;
		char fit[200];
		fl_text(cv, p->f_small, tx, ty + 1, fl_th.dim, x->app[0] ? x->app : "Notification", -1);
		ty += 16;
		fl_text_fit(ui->f_med, x->summary, tw - 20, fit, sizeof fit);
		fl_text(cv, ui->f_med, tx, ty, fl_th.strong, fit, -1);
		ty += 20;
		struct line lines[3];
		int bl = card_body_lines(nt, x, lines);
		if (bl)
			ty += 2;
		for (int i = 0; i < bl; i++, ty += 17)
			fl_text(cv, p->f_what, tx, ty, fl_th.text, lines[i].t, -1);
		int bx = tx;
		int shown = 0;
		for (int i = 0; i < x->n_actions && shown < 3; i++) {
			if (!strcmp(x->act[i].key, "default"))
				continue;
			int bw = fl_text_width(ui->f_reg, x->act[i].label, -1) + 24;
			if (bw < 64)
				bw = 64;
			if (bx + bw > CARD_W - 14)
				break;
			if (fl_button(ui, base + 2 + (uint32_t)shown, fl_r(bx, ty + 10 - 2, bw, 28), x->act[i].label,
			              shown == 0 ? FL_BTN_ACCENT : FL_BTN_OUTLINE)) {
				drop_id = x->id;
				drop_reason = 2;
				act_id = x->id;
				snprintf(act_key, sizeof act_key, "%s", x->act[i].key);
			}
			bx += bw + 8;
			shown++;
		}
		fl_rect body = cr;
		int bres = fl_widget(ui, base, body, 0);
		if (bres & FL_CLICK) {
			drop_id = x->id;
			drop_reason = 2;
			if (x->has_default) {
				act_id = x->id;
				snprintf(act_key, sizeof act_key, "default");
			}
		}
		y += ch + CARD_GAP;
	}
	(void)act_id;
	if (drop_id) {
		struct notif *x = find_id(nt, drop_id);
		if (x) {
			if (act_key[0])
				action_invoked(nt, x, act_key);
			card_drop(nt, x, drop_reason, 1);
			prune(nt);
			cards_layout(nt);
		}
	}
}

static void center_clear_all(struct notify *nt);

/* ---- the notification centre ------------------------------------------------------------------------ */

#define IT_PAD 12

static int item_lines(struct notify *nt, const struct notif *x, struct line *l)
{
	return x->body[0] ? wrap(nt->p->f_what, x->body, NC_W - 16 - 32 - 12 - 16 - 28, l, 3) : 0;
}

static int item_height(struct notify *nt, const struct notif *x)
{
	struct line l[3];
	int bl = item_lines(nt, x, l);
	int h = IT_PAD + 14 + 4 + 20 + (bl ? 2 + bl * 17 : 0) + IT_PAD;
	return h < 64 ? 64 : h;
}

static int center_content(struct notify *nt)
{
	int h = 0;
	for (int i = 0; i < nt->n; i++)
		h += item_height(nt, &nt->h[i]) + 1;
	return h;
}

static int center_want_h(struct notify *nt)
{
	int list = nt->n ? center_content(nt) : 140;
	int h = NC_HEAD + NC_DND + 1 + list;
	int ow, oh;
	fl_app_output_size(nt->p->app, &ow, &oh);
	int max = oh - 44 - 12; /* the panel and a little air above the flyout */
	if (max > 560)
		max = 560;
	if (h > max)
		h = max;
	if (h < NC_HEAD + NC_DND + 1 + 140)
		h = NC_HEAD + NC_DND + 1 + 140;
	return h;
}

static void center_resize(struct notify *nt)
{
	int h = center_want_h(nt);
	if (h != nt->center_h) {
		nt->center_h = h;
		fl_win_resize(nt->center, NC_W, h);
	}
	fl_win_redraw(nt->center);
}

static void center_draw(fl_ui *ui, void *ud)
{
	struct notify *nt = ud;
	struct panel *p = nt->p;
	fl_canvas *cv = &ui->cv;
	int W = cv->w, H = cv->h;
	fl_fill_rect(cv, fl_r(0, 0, W, H), fl_th.panel);
	fl_fill_rect(cv, fl_r(0, 0, 1, H), fl_th.line);
	fl_fill_rect(cv, fl_r(W - 1, 0, 1, H), fl_th.line);
	fl_fill_rect(cv, fl_r(0, 0, W, 2), fl_th.accent);
	if (ui->key == XKB_KEY_Escape) {
		ui->key = 0;
		notify_center_close(p);
		return;
	}
	/* head: title and Clear all */
	fl_text(cv, p->f_head, 16, 17, fl_th.strong, "Notifications", -1);
	if (nt->n) {
		if (fl_button(ui, 2, fl_r(W - 16 - 88, 12, 88, 30), "Clear all", FL_BTN_OUTLINE)) {
			center_clear_all(nt);
			return;
		}
	}
	/* do not disturb */
	fl_fill_rect(cv, fl_r(0, NC_HEAD - 1, W, 1), fl_th.line);
	fl_text(cv, ui->f_reg, 16, NC_HEAD + 7, fl_th.text, "Do not disturb", -1);
	fl_text(cv, p->f_small, 16, NC_HEAD + 7 + 18, fl_th.dim, nt->dnd ? "On: only urgent ones appear" : "Off", -1);
	int dnd = nt->dnd;
	if (fl_switch(ui, 3, fl_r(W - 16 - 40, NC_HEAD + (NC_DND - 24) / 2, 40, 24), &dnd)) {
		nt->dnd = dnd;
		fl_conf_set("notify_dnd", dnd ? "1" : "0");
		fl_win_redraw(p->pw);
	}
	fl_fill_rect(cv, fl_r(0, NC_HEAD + NC_DND, W, 1), fl_th.line);

	/* the list */
	int top = NC_HEAD + NC_DND + 1, view = H - top;
	fl_rect lr = fl_r(0, top, W, view);
	fl_rect save = cv->clip;
	fl_clip(cv, lr);
	if (!nt->n) {
		const char *a = "No notifications", *b = "You are all caught up.";
		fl_text(cv, p->f_head, (W - fl_text_width(p->f_head, a, -1)) / 2, top + 44, fl_th.text, a, -1);
		fl_text(cv, ui->f_reg, (W - fl_text_width(ui->f_reg, b, -1)) / 2, top + 44 + 28, fl_th.dim, b, -1);
		cv->clip = save;
		return;
	}
	int content = center_content(nt);
	int res = fl_widget(ui, 4, lr, 0);
	if ((res & FL_HOVER) && ui->wheel) {
		nt->center_scroll += ui->wheel * 48;
		ui->wheel = 0;
	}
	int maxs = content > view ? content - view : 0;
	if (nt->center_scroll > maxs)
		nt->center_scroll = maxs;
	if (nt->center_scroll < 0)
		nt->center_scroll = 0;
	int y = top - nt->center_scroll;
	int del = -1, invoke = -1;
	for (int i = 0; i < nt->n; i++) {
		struct notif *x = &nt->h[i];
		int ih = item_height(nt, x);
		fl_rect ir = fl_r(0, y, W - (maxs ? 6 : 0), ih);
		if (y + ih >= top && y <= top + view) {
			uint32_t base = 1000 + (uint32_t)i * 4;
			int hover = ui->mouse_in && fl_in(lr, ui->mx, ui->my) && fl_in(ir, ui->mx, ui->my);
			if (hover)
				fl_fill_rect(cv, ir, fl_mix(fl_th.panel, fl_th.raised, 110));
			fl_rect xr = fl_r(W - (maxs ? 6 : 0) - 16 - 24, y + 8, 24, 24);
			int xres = fl_widget(ui, base + 1, xr, 0);
			if (hover || (xres & FL_HOVER)) {
				if (xres & (FL_HOVER | FL_DOWN))
					fl_fill_rect(cv, xr, fl_th.raised);
				fl_icon_draw(cv, FL_ICON_CLOSE, xr.x + 6, xr.y + 6, 12, (xres & FL_HOVER) ? fl_th.text : fl_th.dim);
			}
			if (xres & FL_CLICK)
				del = i;
			app_tile(ui, fl_r(16, y + IT_PAD, 32, 32), x, x->urgency >= 2 ? fl_mix(fl_th.raised, fl_th.err, 70) : fl_th.raised,
			         fl_th.text);
			if (x->unread)
				fl_fill_rect(cv, fl_r(0, y, 3, ih), fl_th.accent);
			int tx = 16 + 32 + 12, ty = y + IT_PAD;
			char when[40], head[140];
			when_text(x->when, when, sizeof when);
			snprintf(head, sizeof head, "%s  \xc2\xb7  %s", x->app[0] ? x->app : "Notification", when);
			char fit[200];
			fl_text_fit(p->f_small, head, W - tx - 16 - 28, fit, sizeof fit);
			fl_text(cv, p->f_small, tx, ty, fl_th.dim, fit, -1);
			ty += 14 + 4;
			fl_text_fit(ui->f_med, x->summary, W - tx - 16 - 28, fit, sizeof fit);
			fl_text(cv, ui->f_med, tx, ty, fl_th.strong, fit, -1);
			ty += 20;
			struct line lines[3];
			int bl = item_lines(nt, x, lines);
			if (bl)
				ty += 2;
			for (int k = 0; k < bl; k++, ty += 17)
				fl_text(cv, p->f_what, tx, ty, fl_th.text, lines[k].t, -1);
			int ires = fl_widget(ui, base, ir, 0);
			if ((ires & FL_CLICK) && x->has_default)
				invoke = i;
		}
		y += ih;
		fl_fill_rect(cv, fl_r(0, y, W, 1), fl_th.line);
		y += 1;
	}
	cv->clip = save;
	if (maxs) {
		int off = nt->center_scroll;
		fl_scrollbar(ui, 5, fl_r(W - 6, top, 6, view), &off, content, view);
		nt->center_scroll = off;
	}
	if (invoke >= 0) {
		struct notif *x = &nt->h[invoke];
		action_invoked(nt, x, "default");
		notify_center_close(p);
	} else if (del >= 0) {
		uint32_t id = nt->h[del].id;
		int was_card = nt->h[del].card;
		remove_hist(nt, del);
		if (was_card)
			emit_closed(nt, id, 2);
		recount_unread(nt);
		save_arm(nt);
		cards_layout(nt);
		center_resize(nt);
		fl_win_redraw(p->pw);
	}
}

static void center_clear_all(struct notify *nt)
{
	for (int i = 0; i < nt->n; i++)
		if (nt->h[i].card)
			emit_closed(nt, nt->h[i].id, 2);
	nt->n = 0;
	nt->unread_since_open = 0;
	nt->center_scroll = 0;
	save_arm(nt);
	cards_layout(nt);
	if (nt->center_open)
		center_resize(nt);
	fl_win_redraw(nt->p->pw);
}

void notify_center_close(struct panel *p)
{
	struct notify *nt = p->nt;
	if (!nt || !nt->center_open)
		return;
	nt->center_open = 0;
	if (nt->center)
		panel_hide(nt->center);
	for (int i = 0; i < nt->n; i++) /* seen now */
		nt->h[i].unread = 0;
	recount_unread(nt);
	save_arm(nt);
	fl_win_redraw(p->pw);
}

void notify_center_toggle(struct panel *p)
{
	struct notify *nt = p->nt;
	if (!nt || !nt->center || p->shell_ver < 2)
		return;
	if (nt->center_open) {
		notify_center_close(p);
		return;
	}
	nt->center_open = 1;
	nt->center_scroll = 0;
	nt->center_h = 0;
	center_resize(nt);
	recount_unread(nt); /* 0 while it is open: the entries show their own marks */
	panel_open_flyout(p, FLY_NC, nt->center, p->clock_cx);
	fl_win_ui(nt->center)->focus = 0;
	fl_win_redraw(p->pw);
}

int notify_center_is_open(const struct panel *p)
{
	return p->nt && p->nt->center_open;
}

/* ---- posting ------------------------------------------------------------------------------------------ */

uint32_t notify_post(struct panel *p, const struct notify_req *r)
{
	struct notify *nt = p->nt;
	if (!nt)
		return 0;
	struct notif x;
	memset(&x, 0, sizeof x);
	struct notif *old = r->replaces ? find_id(nt, r->replaces) : NULL;
	x.id = old ? old->id : nt->next_id++;
	if (!nt->next_id)
		nt->next_id = 1;
	copy_str(x.app, sizeof x.app, r->app ? r->app : "");
	copy_str(x.icon, sizeof x.icon, r->icon ? r->icon : "");
	char tmp[600];
	plain_text(tmp, sizeof x.summary, r->summary ? r->summary : "");
	copy_str(x.summary, sizeof x.summary, tmp);
	for (char *c = x.summary; *c; c++)
		if (*c == '\n')
			*c = ' ';
	plain_text(tmp, sizeof tmp, r->body ? r->body : "");
	copy_str(x.body, sizeof x.body, tmp);
	x.urgency = r->urgency < 0 ? 1 : r->urgency > 2 ? 2 : r->urgency;
	x.when = time(NULL);
	x.unread = 1;
	x.internal = r->internal;
	x.transient = r->transient;
	for (int i = 0; i < r->n_actions && x.n_actions < 4; i++) {
		copy_str(x.act[x.n_actions].key, sizeof x.act[0].key, r->akey[i]);
		copy_str(x.act[x.n_actions].label, sizeof x.act[0].label, r->alabel[i]);
		if (!strcmp(r->akey[i], "default"))
			x.has_default = 1;
		x.n_actions++;
	}
	int to = r->timeout_ms;
	if (to < 0)
		to = x.urgency >= 2 ? 0 : x.urgency == 0 ? 4000 : 6000;
	if (to > 120000)
		to = 120000;
	x.timeout_ms = to;
	/* a card unless Do not disturb is on (critical ones come through anyway) */
	int show = !nt->dnd || x.urgency >= 2;
	x.card = show;
	if (show && !p->demo)
		panel_sound(x.urgency >= 2 ? "error" : "notification");
	x.deadline = 0;
	if (old) {
		remove_hist(nt, (int)(old - nt->h));
	}
	if (!(r->transient && !show))
		hist_insert(nt, &x);
	recount_unread(nt);
	save_arm(nt);
	cards_layout(nt);
	if (nt->center_open) {
		nt->center_scroll = 0;
		center_resize(nt);
	}
	fl_win_redraw(p->pw);
	return x.id;
}

void notify_close(struct panel *p, uint32_t id, int reason)
{
	struct notify *nt = p->nt;
	if (!nt)
		return;
	struct notif *x = find_id(nt, id);
	if (!x)
		return;
	int was_card = x->card;
	remove_hist(nt, (int)(x - nt->h)); /* withdrawn by its sender: gone from the history too */
	if (was_card || reason)
		emit_closed(nt, id, (uint32_t)reason);
	recount_unread(nt);
	save_arm(nt);
	cards_layout(nt);
	if (nt->center_open)
		center_resize(nt);
	fl_win_redraw(p->pw);
}

int notify_unread(const struct panel *p) { return p->nt ? p->nt->unread_since_open : 0; }
int notify_count(const struct panel *p) { return p->nt ? p->nt->n : 0; }
int notify_dnd(const struct panel *p) { return p->nt ? p->nt->dnd : 0; }

void notify_locked(struct panel *p, int locked)
{
	struct notify *nt = p->nt;
	p->locked = locked;
	if (!nt)
		return;
	if (!locked) { /* what came in meanwhile gets its full time on the screen */
		for (int i = 0; i < nt->n; i++)
			if (nt->h[i].card)
				nt->h[i].deadline = 0;
	}
	if (locked && nt->center_open)
		notify_center_close(p);
	cards_layout(nt);
}

void notify_conf_changed(struct panel *p)
{
	struct notify *nt = p->nt;
	if (!nt)
		return;
	int d = fl_conf_int("notify_dnd", 0) != 0;
	if (d != nt->dnd) {
		nt->dnd = d;
		if (nt->center_open)
			fl_win_redraw(nt->center);
		fl_win_redraw(p->pw);
	}
}

/* ---- D-Bus ------------------------------------------------------------------------------------------------ */

static const char introspect_xml[] =
	"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
	"\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
	"<node>\n"
	" <interface name=\"org.freedesktop.Notifications\">\n"
	"  <method name=\"GetCapabilities\"><arg type=\"as\" direction=\"out\"/></method>\n"
	"  <method name=\"Notify\">\n"
	"   <arg type=\"s\" name=\"app_name\" direction=\"in\"/><arg type=\"u\" name=\"replaces_id\" direction=\"in\"/>\n"
	"   <arg type=\"s\" name=\"app_icon\" direction=\"in\"/><arg type=\"s\" name=\"summary\" direction=\"in\"/>\n"
	"   <arg type=\"s\" name=\"body\" direction=\"in\"/><arg type=\"as\" name=\"actions\" direction=\"in\"/>\n"
	"   <arg type=\"a{sv}\" name=\"hints\" direction=\"in\"/><arg type=\"i\" name=\"expire_timeout\" direction=\"in\"/>\n"
	"   <arg type=\"u\" name=\"id\" direction=\"out\"/>\n"
	"  </method>\n"
	"  <method name=\"CloseNotification\"><arg type=\"u\" name=\"id\" direction=\"in\"/></method>\n"
	"  <method name=\"GetServerInformation\"><arg type=\"s\" direction=\"out\"/><arg type=\"s\" direction=\"out\"/>"
	"<arg type=\"s\" direction=\"out\"/><arg type=\"s\" direction=\"out\"/></method>\n"
	"  <signal name=\"NotificationClosed\"><arg type=\"u\"/><arg type=\"u\"/></signal>\n"
	"  <signal name=\"ActionInvoked\"><arg type=\"u\"/><arg type=\"s\"/></signal>\n"
	" </interface>\n"
	" <interface name=\"org.freelfs.Notifications\">\n"
	"  <method name=\"SetDoNotDisturb\"><arg type=\"b\" direction=\"in\"/></method>\n"
	"  <method name=\"GetDoNotDisturb\"><arg type=\"b\" direction=\"out\"/></method>\n"
	"  <method name=\"ClearAll\"/>\n"
	"  <method name=\"ToggleCenter\"/>\n"
	"  <method name=\"GetCount\"><arg type=\"u\" direction=\"out\"/></method>\n"
	" </interface>\n"
	" <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
	"  <method name=\"Introspect\"><arg type=\"s\" direction=\"out\"/></method>\n"
	" </interface>\n"
	"</node>\n";

static void reply_ok(struct notify *nt, DBusMessage *msg, DBusMessage *rep)
{
	(void)msg;
	if (rep) {
		dbus_connection_send(nt->bus, rep, NULL);
		dbus_message_unref(rep);
	}
}

static int hint_int(DBusMessageIter *v)
{
	switch (dbus_message_iter_get_arg_type(v)) {
	case DBUS_TYPE_BYTE: {
		unsigned char b;
		dbus_message_iter_get_basic(v, &b);
		return b;
	}
	case DBUS_TYPE_UINT32: {
		dbus_uint32_t b;
		dbus_message_iter_get_basic(v, &b);
		return (int)b;
	}
	case DBUS_TYPE_INT32: {
		dbus_int32_t b;
		dbus_message_iter_get_basic(v, &b);
		return b;
	}
	case DBUS_TYPE_BOOLEAN: {
		dbus_bool_t b;
		dbus_message_iter_get_basic(v, &b);
		return b;
	}
	}
	return -1;
}

static void handle_notify(struct notify *nt, DBusMessage *msg)
{
	DBusMessageIter it;
	const char *app = "", *icon = "", *summary = "", *body = "";
	dbus_uint32_t replaces = 0;
	dbus_int32_t timeout = -1;
	if (!dbus_message_iter_init(msg, &it))
		goto bad;
#define NEXT_STR(var)                                                                                          \
	if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING)                                           \
		goto bad;                                                                                      \
	dbus_message_iter_get_basic(&it, &var);                                                                \
	dbus_message_iter_next(&it);
	NEXT_STR(app)
	if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_UINT32)
		goto bad;
	dbus_message_iter_get_basic(&it, &replaces);
	dbus_message_iter_next(&it);
	NEXT_STR(icon)
	NEXT_STR(summary)
	NEXT_STR(body)
	struct notify_req r;
	memset(&r, 0, sizeof r);
	r.app = app;
	r.icon = icon;
	r.summary = summary;
	r.body = body;
	r.urgency = 1;
	r.replaces = replaces;
	/* actions: key, label, key, label ... */
	char keys[3][48], labels[3][64];
	if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY)
		goto bad;
	{
		DBusMessageIter a;
		dbus_message_iter_recurse(&it, &a);
		int pos = 0;
		char key[48] = "";
		while (dbus_message_iter_get_arg_type(&a) == DBUS_TYPE_STRING) {
			const char *s;
			dbus_message_iter_get_basic(&a, &s);
			if (pos % 2 == 0) {
				copy_str(key, sizeof key, s);
			} else if (r.n_actions < 3 || !strcmp(key, "default")) {
				/* "default" has no button: it is the click on the card, so it does not use a slot */
				int slot = r.n_actions < 3 ? r.n_actions : 2;
				if (r.n_actions < 3) {
					copy_str(keys[slot], sizeof keys[0], key);
					copy_str(labels[slot], sizeof labels[0], s);
					r.akey[slot] = keys[slot];
					r.alabel[slot] = labels[slot];
					r.n_actions++;
				}
			}
			pos++;
			dbus_message_iter_next(&a);
		}
	}
	dbus_message_iter_next(&it);
	if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY)
		goto bad;
	{
		DBusMessageIter d;
		dbus_message_iter_recurse(&it, &d);
		while (dbus_message_iter_get_arg_type(&d) == DBUS_TYPE_DICT_ENTRY) {
			DBusMessageIter e, v;
			dbus_message_iter_recurse(&d, &e);
			const char *k = NULL;
			dbus_message_iter_get_basic(&e, &k);
			dbus_message_iter_next(&e);
			if (dbus_message_iter_get_arg_type(&e) == DBUS_TYPE_VARIANT) {
				dbus_message_iter_recurse(&e, &v);
				if (!strcmp(k, "urgency")) {
					int u = hint_int(&v);
					if (u >= 0)
						r.urgency = u > 2 ? 2 : u;
				} else if (!strcmp(k, "transient")) {
					r.transient = hint_int(&v) > 0;
				}
			}
			dbus_message_iter_next(&d);
		}
	}
	dbus_message_iter_next(&it);
	if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_INT32)
		dbus_message_iter_get_basic(&it, &timeout);
	r.timeout_ms = timeout;
	uint32_t id = notify_post(nt->p, &r);
	DBusMessage *rep = dbus_message_new_method_return(msg);
	if (rep) {
		dbus_message_append_args(rep, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID);
		reply_ok(nt, msg, rep);
	}
	return;
bad:
	reply_ok(nt, msg, dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS, "bad Notify arguments"));
#undef NEXT_STR
}

static DBusHandlerResult on_message(DBusConnection *c, DBusMessage *msg, void *ud)
{
	struct notify *nt = ud;
	if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	const char *iface = dbus_message_get_interface(msg), *member = dbus_message_get_member(msg);
	if (!member)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	if (iface && !strcmp(iface, "org.freedesktop.DBus.Introspectable") && !strcmp(member, "Introspect")) {
		DBusMessage *rep = dbus_message_new_method_return(msg);
		const char *x = introspect_xml;
		if (rep) {
			dbus_message_append_args(rep, DBUS_TYPE_STRING, &x, DBUS_TYPE_INVALID);
			reply_ok(nt, msg, rep);
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	if (!iface || !strcmp(iface, NOTIF_IFACE)) {
		if (!strcmp(member, "Notify")) {
			handle_notify(nt, msg);
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		if (!strcmp(member, "GetCapabilities")) {
			DBusMessage *rep = dbus_message_new_method_return(msg);
			if (rep) {
				static const char *caps[] = {"actions", "body", "icon-static", "persistence"};
				const char **cp = caps;
				dbus_message_append_args(rep, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &cp, 4, DBUS_TYPE_INVALID);
				reply_ok(nt, msg, rep);
			}
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		if (!strcmp(member, "CloseNotification")) {
			dbus_uint32_t id = 0;
			if (dbus_message_get_args(msg, NULL, DBUS_TYPE_UINT32, &id, DBUS_TYPE_INVALID)) {
				notify_close(nt->p, id, 3);
				reply_ok(nt, msg, dbus_message_new_method_return(msg));
			} else {
				reply_ok(nt, msg, dbus_message_new_error(msg, DBUS_ERROR_INVALID_ARGS, "expected u"));
			}
			return DBUS_HANDLER_RESULT_HANDLED;
		}
		if (!strcmp(member, "GetServerInformation")) {
			DBusMessage *rep = dbus_message_new_method_return(msg);
			const char *n = "flpanel", *v = "FreeLFS", *ver = "0.1", *spec = "1.2";
			if (rep) {
				dbus_message_append_args(rep, DBUS_TYPE_STRING, &n, DBUS_TYPE_STRING, &v, DBUS_TYPE_STRING,
				                         &ver, DBUS_TYPE_STRING, &spec, DBUS_TYPE_INVALID);
				reply_ok(nt, msg, rep);
			}
			return DBUS_HANDLER_RESULT_HANDLED;
		}
	}
	if (iface && !strcmp(iface, FL_IFACE)) {
		DBusMessage *rep = NULL;
		if (!strcmp(member, "SetDoNotDisturb")) {
			dbus_bool_t b = 0;
			if (dbus_message_get_args(msg, NULL, DBUS_TYPE_BOOLEAN, &b, DBUS_TYPE_INVALID)) {
				nt->dnd = b ? 1 : 0;
				fl_conf_set("notify_dnd", b ? "1" : "0");
				fl_win_redraw(nt->p->pw);
				if (nt->center_open)
					fl_win_redraw(nt->center);
			}
			rep = dbus_message_new_method_return(msg);
		} else if (!strcmp(member, "GetDoNotDisturb")) {
			dbus_bool_t b = nt->dnd;
			rep = dbus_message_new_method_return(msg);
			if (rep)
				dbus_message_append_args(rep, DBUS_TYPE_BOOLEAN, &b, DBUS_TYPE_INVALID);
		} else if (!strcmp(member, "ClearAll")) {
			center_clear_all(nt);
			rep = dbus_message_new_method_return(msg);
		} else if (!strcmp(member, "ToggleCenter")) {
			notify_center_toggle(nt->p);
			rep = dbus_message_new_method_return(msg);
		} else if (!strcmp(member, "GetCount")) {
			dbus_uint32_t n = (dbus_uint32_t)nt->n;
			rep = dbus_message_new_method_return(msg);
			if (rep)
				dbus_message_append_args(rep, DBUS_TYPE_UINT32, &n, DBUS_TYPE_INVALID);
		} else {
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		}
		reply_ok(nt, msg, rep);
		return DBUS_HANDLER_RESULT_HANDLED;
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void bus_close(struct notify *nt)
{
	if (!nt->bus)
		return;
	fl_fd_remove(nt->p->app, nt->fd);
	dbus_connection_close(nt->bus);
	dbus_connection_unref(nt->bus);
	nt->bus = NULL;
	nt->fd = -1;
}

static void pump(struct notify *nt)
{
	if (!nt->bus)
		return;
	int alive = dbus_connection_read_write(nt->bus, 0);
	while (nt->bus && dbus_connection_dispatch(nt->bus) == DBUS_DISPATCH_DATA_REMAINS)
		;
	if (!alive || (nt->bus && !dbus_connection_get_is_connected(nt->bus))) {
		bus_close(nt);
		nt->tries = 0;
		fl_timer_arm(nt->retry, 1000, 0);
	}
}

static void on_bus_fd(void *ud, int fd, short rev)
{
	pump(ud);
}

static void bus_connect(void *ud)
{
	struct notify *nt = ud;
	if (nt->bus || nt->passive)
		return;
	const char *addr = getenv("DBUS_SESSION_BUS_ADDRESS");
	if (!addr || !*addr)
		return; /* no session bus at all */
	DBusError err;
	dbus_error_init(&err);
	DBusConnection *c = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
	dbus_error_free(&err);
	if (!c) {
		nt->tries++;
		fl_timer_arm(nt->retry, nt->tries < 90 ? 1000 : 10000, 0);
		return;
	}
	dbus_connection_set_exit_on_disconnect(c, FALSE);
	int fd;
	if (!dbus_connection_get_unix_fd(c, &fd)) {
		dbus_connection_close(c);
		dbus_connection_unref(c);
		return;
	}
	dbus_error_init(&err);
	int r = dbus_bus_request_name(c, NOTIF_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
	dbus_error_free(&err);
	if (r != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		/* another notification daemon runs: stay out of its way */
		fprintf(stderr, "flpanel: %s is owned by another program, notifications are off\n", NOTIF_NAME);
		dbus_connection_close(c);
		dbus_connection_unref(c);
		nt->passive = 1;
		return;
	}
	static const DBusObjectPathVTable vt = {.message_function = on_message};
	dbus_connection_register_object_path(c, NOTIF_PATH, &vt, nt);
	nt->bus = c;
	nt->fd = fd;
	nt->tries = 0;
	fl_fd_add(nt->p->app, fd, POLLIN, on_bus_fd, nt);
	dbus_connection_flush(c);
	pump(nt);
}

/* ---- setup ----------------------------------------------------------------------------------------------------- */

void notify_init(struct panel *p)
{
	struct notify *nt = calloc(1, sizeof *nt);
	if (!nt)
		return;
	p->nt = nt;
	nt->p = p;
	nt->fd = -1;
	nt->next_id = 1;
	nt->dnd = fl_conf_int("notify_dnd", 0) != 0;
	const char *rt = getenv("XDG_RUNTIME_DIR");
	if (rt && *rt && !p->demo)
		snprintf(nt->path, sizeof nt->path, "%s/flpanel-notifications", rt);
	history_load(nt);
	nt->tick = fl_timer_new(p->app, cards_tick, nt);
	nt->retry = fl_timer_new(p->app, bus_connect, nt);
	nt->save = fl_timer_new(p->app, history_save, nt);

	fl_win_opts co = {
		.title = "notifications", .app_id = "flpanel-notify", .w = CARD_W, .h = 100, .raw = 1,
		.draw = cards_draw, .ud = nt,
	};
	nt->cards = fl_win_new(p->app, &co);
	fl_win_opts no = {
		.title = "notification centre", .app_id = "flpanel-center", .w = NC_W, .h = 300, .raw = 1,
		.draw = center_draw, .ud = nt,
	};
	nt->center = fl_win_new(p->app, &no);
	if (p->shell && p->shell_ver >= 4 && nt->cards)
		fl_shell_set_notifications(p->shell, fl_win_surface(nt->cards));
	else if (nt->cards && !fl_app_ppm(p->app)) {
		fl_win_destroy(nt->cards); /* an older compositor: no cards, the centre still works */
		nt->cards = NULL;
	}
	if (nt->cards)
		panel_hide(nt->cards);
	if (nt->center)
		panel_hide(nt->center);
	if (!fl_app_ppm(p->app) && !p->demo)
		fl_timer_arm(nt->retry, 50, 0);
}

void notify_free(struct panel *p)
{
	struct notify *nt = p->nt;
	if (!nt)
		return;
	history_save(nt);
	bus_close(nt);
	/* the timers are the panel's fl_app's: freed here, or their fd entries would point at freed memory */
	fl_timer_free(nt->tick);
	fl_timer_free(nt->retry);
	fl_timer_free(nt->save);
	free(nt);
	p->nt = NULL;
}

/* screenshots: made-up history (and cards, and the centre) */
void notify_demo(struct panel *p, int center)
{
	struct notify *nt = p->nt;
	if (!nt)
		return;
	/* only the window being shown gets written to the picture files */
	if (center != 0 && nt->cards) {
		fl_win_destroy(nt->cards);
		nt->cards = NULL;
	}
	if (center != 1 && nt->center) {
		fl_win_destroy(nt->center);
		nt->center = NULL;
	}
	if (center == -2)
		return; /* no made-up history: just the plain panel */
	time_t now = time(NULL);
	struct {
		const char *app, *sum, *body;
		int urg, ago;
		const char *icon;
	} d[] = {
		{"Software", "Updates are ready", "3 packages can be updated: freebws, flui and linux-firmware.", 1, 120, ""},
		{"Terminal", "Build finished", "make -j4 in ~/src/flcomp finished in 41 s with no warnings.", 1, 1500, "flterm"},
		{"Battery", "Battery is at 10%", "Plug in the charger, or turn on power saving to make it last longer.", 2, 4000, ""},
		{"Files", "Copy complete", "photos-2026.tar (1.2 GB) was copied to the USB stick.", 0, 30000, "flfiles"},
		{"Settings", "Wi-Fi connected", "Connected to Home-5G.", 0, 90000, "flsettings"},
	};
	for (int i = (int)(sizeof d / sizeof *d) - 1; i >= 0; i--) {
		struct notif x;
		memset(&x, 0, sizeof x);
		x.id = nt->next_id++;
		copy_str(x.app, sizeof x.app, d[i].app);
		copy_str(x.icon, sizeof x.icon, d[i].icon);
		copy_str(x.summary, sizeof x.summary, d[i].sum);
		copy_str(x.body, sizeof x.body, d[i].body);
		x.urgency = d[i].urg;
		x.when = now - d[i].ago;
		x.unread = i < 2;
		hist_insert(nt, &x);
	}
	if (center == 1)
		nt->center_open = 1;
	recount_unread(nt);
	if (center < 0)
		return;
	if (center == 1) {
		nt->center_h = 0;
		center_resize(nt);
		fl_win_map(nt->center);
	} else {
		/* two cards, one with actions */
		struct notif *x = &nt->h[0];
		x->card = 1;
		x->timeout_ms = 0;
		x->n_actions = 2;
		snprintf(x->act[0].key, sizeof x->act[0].key, "open");
		snprintf(x->act[0].label, sizeof x->act[0].label, "Open Software");
		snprintf(x->act[1].key, sizeof x->act[1].key, "later");
		snprintf(x->act[1].label, sizeof x->act[1].label, "Later");
		nt->h[1].card = 1;
		nt->h[1].timeout_ms = 0;
		nt->h[2].card = 1;
		nt->h[2].timeout_ms = 0;
		nt->h[2].n_actions = 1;
		snprintf(nt->h[2].act[0].key, sizeof nt->h[2].act[0].key, "saver");
		snprintf(nt->h[2].act[0].label, sizeof nt->h[2].act[0].label, "Turn on power saving");
		cards_layout(nt);
		fl_win_map(nt->cards);
	}
}
