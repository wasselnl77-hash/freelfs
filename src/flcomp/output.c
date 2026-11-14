/* The outputs: modes, scale, position, primary, the layout of the desktop, and the 15 second
 * "keep these settings?" of the old Display page.
 *
 * Every connector the backend knows is a struct output (OUT). The enabled ones form the desktop:
 * the shadow buffer is their bounding box in logical pixels (mode size * 100 / scale; any scale
 * from 100 to 200 %, the compositor enlarges the picture per output, so clients always draw at
 * scale 1 and no wp_fractional_scale is needed). The primary output carries the panel, the
 * menus, notifications, maximized and fullscreen windows (C.prim); the others are extra desktop
 * space for windows and the pointer. wl_output tells clients the primary's logical size.
 *
 * Settings per output are kept in desktop.conf as
 *   output.NAME=mode=1920x1080@60 scale=1.25 pos=0,0 enabled=1 primary=1
 * written by flcomp itself after every fl_shell output_configure (fldisplay) and read again at
 * start and on hotplug. The old keys mode= and scale= still apply to the primary output while
 * it has no output.NAME entry.
 * SPDX-License-Identifier: MIT */
#include "comp.h"
#include "fl-shell-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONFIRM_SECS 15

/* what one output is set to: the saved state and the snapshots for revert */
struct ocfg {
	char name[32];
	bool enabled, primary, has;
	int w, h, hz, scale, x, y;
};
static struct ocfg SV[2 * MAX_OUT];	/* saved settings by name, also of outputs not plugged in */
static int NSV;
static struct ocfg snap9[2 * MAX_OUT];	/* before the last output_configure */
static int nsnap9;
static bool laid_out;

/* the old set_output and its confirm */
static struct {
	bool pending;
	struct ocfg snap[2 * MAX_OUT];
	int n;
	struct wl_event_source *timer;
	int deadline_ms;
	struct wl_client *owner;
	struct wl_listener gone;
} P;

/* the desktop must stay usable: 640 x 400 logical at least */
bool output_scale_ok(int pw, int ph, int scale)
{
	return scale >= 100 && scale <= 200 && pw * 100 / scale >= 640 && ph * 100 / scale >= 400;
}

bool output_ready(void) { return laid_out; }

static struct output *find_out(const char *name)
{
	for (int i = 0; i < NOUT; i++)
		if (!strcmp(OUT[i].name, name))
			return &OUT[i];
	return NULL;
}

static struct output *primary(void)
{
	for (int i = 0; i < NOUT; i++)
		if (OUT[i].enabled && OUT[i].primary)
			return &OUT[i];
	for (int i = 0; i < NOUT; i++)
		if (OUT[i].enabled)
			return &OUT[i];
	return NULL;
}

static struct ocfg *sv_find(const char *name)
{
	for (int i = 0; i < NSV; i++)
		if (!strcmp(SV[i].name, name))
			return &SV[i];
	return NULL;
}

static void to_cfg(const struct output *o, struct ocfg *c)
{
	snprintf(c->name, sizeof c->name, "%s", o->name);
	c->enabled = o->enabled;
	c->primary = o->primary;
	c->has = true;
	c->w = o->w;
	c->h = o->h;
	c->hz = o->hz;
	c->scale = o->scale;
	c->x = o->x;
	c->y = o->y;
}

static void fmt_scale(char *b, size_t n, int pct)
{
	if (pct % 100 == 0)
		snprintf(b, n, "%d", pct / 100);
	else if (pct % 10 == 0)
		snprintf(b, n, "%d.%d", pct / 100, pct % 100 / 10);
	else
		snprintf(b, n, "%d.%02d", pct / 100, pct % 100);
}

static void save_one(const struct ocfg *c)
{
	char key[48], val[160], sc[16];
	snprintf(key, sizeof key, "output.%s", c->name);
	fmt_scale(sc, sizeof sc, c->scale);
	snprintf(val, sizeof val, "mode=%dx%d@%d scale=%s pos=%d,%d enabled=%d primary=%d", c->w, c->h, c->hz, sc,
		 c->x, c->y, c->enabled, c->primary);
	config_set(key, val);
}

/* remember the state of every output that has ever been set, and write it to desktop.conf */
static void remember_all(void)
{
	for (int i = 0; i < NOUT; i++) {
		struct output *o = &OUT[i];
		if (!o->connected || !o->w)
			continue;
		struct ocfg *c = sv_find(o->name);
		if (!c && NSV < 2 * MAX_OUT)
			c = &SV[NSV++];
		if (!c)
			continue;
		to_cfg(o, c);
		save_one(c);
	}
}

static void load_saved(void)
{
	NSV = 0;
	for (int i = 0; i < NOUT; i++) {
		char key[48], buf[200];
		snprintf(key, sizeof key, "output.%s", OUT[i].name);
		if (!config_get_raw(key, buf, sizeof buf) || NSV >= 2 * MAX_OUT)
			continue;
		struct ocfg *c = &SV[NSV++];
		memset(c, 0, sizeof *c);
		snprintf(c->name, sizeof c->name, "%s", OUT[i].name);
		c->scale = 100;
		c->enabled = true;
		c->has = true;
		for (char *t = strtok(buf, " "); t; t = strtok(NULL, " ")) {
			int a, b, hz = 0;
			double sc;
			if (sscanf(t, "mode=%dx%d@%d", &a, &b, &hz) >= 2) {
				c->w = a;
				c->h = b;
				c->hz = hz;
			} else if (sscanf(t, "scale=%lf", &sc) == 1) {
				c->scale = (int)(sc * 100 + 0.5);
			} else if (sscanf(t, "pos=%d,%d", &a, &b) == 2) {
				c->x = a;
				c->y = b;
			} else if (sscanf(t, "enabled=%d", &a) == 1) {
				c->enabled = a != 0;
			} else if (sscanf(t, "primary=%d", &a) == 1) {
				c->primary = a != 0;
			}
		}
	}
}

/* the shadow and everything that depends on the desktop's size */
static bool resize_shadow(int w, int h)
{
	uint32_t *sh = calloc((size_t)w * h, 4);
	if (!sh)
		return false;
	free(C.shadow);
	C.shadow = sh;
	C.w = w;
	C.h = h;
	C.dmg.n = C.prev.n = 0;
	if (laid_out)
		wall_reload();
	if (C.px > C.w - 1)
		C.px = C.w - 1;
	if (C.py > C.h - 1)
		C.py = C.h - 1;
	return true;
}

/* positions -> shadow coordinates, C.w/C.h, C.prim. false: no enabled output (nothing changes) */
static bool layout(void)
{
	int minx = 1 << 30, miny = 1 << 30, maxx = -(1 << 30), maxy = -(1 << 30), n = 0;
	for (int i = 0; i < NOUT; i++) {
		struct output *o = &OUT[i];
		if (!o->enabled)
			continue;
		o->lw = o->w * 100 / o->scale;
		o->lh = o->h * 100 / o->scale;
		if (o->x < minx) minx = o->x;
		if (o->y < miny) miny = o->y;
		if (o->x + o->lw > maxx) maxx = o->x + o->lw;
		if (o->y + o->lh > maxy) maxy = o->y + o->lh;
		n++;
	}
	if (!n)
		return false;
	for (int i = 0; i < NOUT; i++) {
		OUT[i].dx = OUT[i].x - minx;
		OUT[i].dy = OUT[i].y - miny;
	}
	int W = maxx - minx, H = maxy - miny;
	if (W > 16384 || H > 16384 || (size_t)W * H > (size_t)64 << 20)
		return false;
	if (W != C.w || H != C.h || !C.shadow)
		if (!resize_shadow(W, H))
			return false;
	struct output *p = primary();
	for (int i = 0; i < NOUT; i++)
		OUT[i].primary = &OUT[i] == p;
	C.prim = (struct rect){ p->dx, p->dy, p->dx + p->lw, p->dy + p->lh };
	C.pw = p->w;
	C.ph = p->h;
	C.scale = p->scale;
	C.refresh = p->hz * 1000;
	CF.scale = p->scale;
	laid_out = true;
	return true;
}

static void announce(void)
{
	outputs_send_mode();
	fl_emit_output();
}

static int last_pw, last_ph, last_px, last_py;

/* the part after the screen changed: windows, panel, picture */
static void after_change(void)
{
	wall_reload();
	windows_fit_output();
	work_area_changed();
	/* the panel sizes itself to the primary output once, when it starts */
	if (C.prim.x2 - C.prim.x1 != last_pw || C.prim.y2 - C.prim.y1 != last_ph || C.prim.x1 != last_px ||
	    C.prim.y1 != last_py) {
		last_pw = C.prim.x2 - C.prim.x1;
		last_ph = C.prim.y2 - C.prim.y1;
		last_px = C.prim.x1;
		last_py = C.prim.y1;
		output_restart_panel();
	}
	damage_all();
	night_outputs_changed();
	announce();
}

/* ---- snapshots ------------------------------------------------------------------------------ */

static int snapshot(struct ocfg *out)
{
	int n = 0;
	for (int i = 0; i < NOUT && n < 2 * MAX_OUT; i++)
		if (OUT[i].connected && OUT[i].w)
			to_cfg(&OUT[i], &out[n++]);
	return n;
}

/* put the outputs back as in a snapshot: hardware first (a failure of one does not stop the rest) */
static void restore(const struct ocfg *s, int n)
{
	for (int i = 0; i < n; i++) {
		struct output *o = find_out(s[i].name);
		if (!o || !o->connected)
			continue;
		if (s[i].enabled != o->enabled || s[i].w != o->w || s[i].h != o->h || s[i].hz != o->hz) {
			if (!backend_out_set(o, s[i].enabled, s[i].w, s[i].h, s[i].hz))
				fprintf(stderr, "flcomp: cannot restore %s\n", o->name);
		}
		o->scale = s[i].scale;
		o->x = s[i].x;
		o->y = s[i].y;
		o->primary = s[i].primary;
	}
	if (!layout()) {	/* nothing lit: light the first connected output */
		for (int i = 0; i < NOUT; i++)
			if (OUT[i].connected && OUT[i].nmodes &&
			    backend_out_set(&OUT[i], true, OUT[i].modes[0].w, OUT[i].modes[0].h, OUT[i].modes[0].hz)) {
				OUT[i].scale = 100;
				break;
			}
		layout();
	}
	after_change();
}

/* ---- the old set_output: the primary's mode and scale, with the 15 second confirm ------------ */

bool output_set(int w, int h, int hz, int scale)
{
	struct output *o = primary();
	if (!o)
		return false;
	if (!w || !h) {
		w = o->w;
		h = o->h;
		hz = o->hz;
	}
	if (!output_scale_ok(w, h, scale))
		return false;
	if (w == o->w && h == o->h && hz == o->hz && scale == o->scale)
		return true;
	struct ocfg snap[2 * MAX_OUT];
	int n = snapshot(snap);
	if ((w != o->w || h != o->h || hz != o->hz) && !backend_out_set(o, true, w, h, hz))
		return false;
	int os = o->scale;
	o->scale = scale;
	if (!layout()) {
		o->scale = os;
		return false;
	}
	if (!P.pending) {
		P.pending = true;
		memcpy(P.snap, snap, sizeof snap);
		P.n = n;
	}
	P.deadline_ms = (int)now_ms() + CONFIRM_SECS * 1000;
	if (P.timer)
		wl_event_source_timer_update(P.timer, CONFIRM_SECS * 1000);
	after_change();
	return true;
}

static void settle(void)
{
	P.pending = false;
	if (P.timer)
		wl_event_source_timer_update(P.timer, 0);
	if (P.owner) {
		wl_list_remove(&P.gone.link);
		P.owner = NULL;
	}
}

void output_confirm(void)
{
	if (!P.pending)
		return;
	settle();
	announce();
}

void output_revert(void)
{
	if (!P.pending)
		return;
	struct ocfg snap[2 * MAX_OUT];
	int n = P.n;
	memcpy(snap, P.snap, sizeof snap);
	settle();
	restore(snap, n);
}

static int timer_cb(void *data)
{
	output_revert();
	return 0;
}

static void owner_gone(struct wl_listener *l, void *data)
{
	P.owner = NULL;	/* the listener is unlinked by libwayland */
	output_revert();
}

/* a client asked for the change: if it dies before confirming, go back */
void output_watch_client(struct wl_client *c)
{
	if (!P.pending)
		return;
	if (P.owner)
		wl_list_remove(&P.gone.link);
	P.owner = c;
	P.gone.notify = owner_gone;
	wl_client_add_destroy_listener(c, &P.gone);
}

int output_pending_secs(void)
{
	if (!P.pending)
		return 0;
	int left = (P.deadline_ms - (int)now_ms() + 999) / 1000;
	return left < 1 ? 1 : left > CONFIRM_SECS ? CONFIRM_SECS : left;
}

/* ---- fl_shell 9: several outputs -------------------------------------------------------------- */

bool output_configure(const char *name, unsigned flags, int w, int h, int hz, int scale, int x, int y,
		      int enabled, int prim)
{
	struct output *o = find_out(name);
	if (!o || !o->connected)
		return false;
	int nw = o->w, nh = o->h, nhz = o->hz, ns = o->scale, nx = o->x, ny = o->y;
	bool ne = o->enabled, np = o->primary;
	if (flags & 1) {
		nw = w;
		nh = h;
		nhz = hz;
	}
	if (flags & 2)
		ns = scale;
	if (flags & 4) {
		nx = x;
		ny = y;
	}
	if (flags & 8)
		ne = enabled != 0;
	if (flags & 16) {
		if (!prim)
			return false;
		np = true;
		ne = true;
	}
	if (ne && !output_scale_ok(nw, nh, ns))
		return false;
	if (!ne) {	/* the last lit output stays */
		int lit = 0;
		for (int i = 0; i < NOUT; i++)
			lit += OUT[i].enabled && &OUT[i] != o;
		if (!lit)
			return false;
	}
	if (nw <= 0) {	/* never lit and no mode asked for: the display's preferred */
		if (!o->nmodes)
			return false;
		nw = o->modes[0].w;
		nh = o->modes[0].h;
		nhz = o->modes[0].hz;
		if (!output_scale_ok(nw, nh, ns))
			return false;
	}
	struct ocfg before[2 * MAX_OUT];
	int nb = snapshot(before);
	if ((ne != o->enabled || nw != o->w || nh != o->h || nhz != o->hz) && !backend_out_set(o, ne, nw, nh, nhz))
		return false;
	bool oldp = o->primary;
	o->scale = ns;
	o->x = nx;
	o->y = ny;
	o->enabled = ne;
	if (np && !oldp) {
		for (int i = 0; i < NOUT; i++)
			OUT[i].primary = false;
		o->primary = true;
	} else if (!ne) {
		o->primary = false;
	}
	if (!layout()) {
		restore(before, nb);
		return false;
	}
	memcpy(snap9, before, sizeof before);
	nsnap9 = nb;
	remember_all();
	after_change();
	return true;
}

bool output_revert_config(void)
{
	if (!nsnap9)
		return false;
	struct ocfg s[2 * MAX_OUT];
	int n = nsnap9;
	memcpy(s, snap9, sizeof s);
	nsnap9 = 0;
	restore(s, n);
	remember_all();
	return true;
}

static void send_one(struct wl_resource *r, const struct output *o)
{
	uint32_t fl = (o->connected ? 1 : 0) | (o->enabled ? 2 : 0) | (o->primary ? 4 : 0);
	fl_shell_send_output_info(r, o->name, fl, o->x, o->y, (uint32_t)o->w, (uint32_t)o->h, (uint32_t)o->hz,
				  (uint32_t)o->scale);
	for (int i = 0; i < o->nmodes; i++) {
		uint32_t f = (o->enabled && o->modes[i].w == o->w && o->modes[i].h == o->h && o->modes[i].hz == o->hz ? 1 : 0) |
			     (o->modes[i].preferred ? 2 : 0);
		fl_shell_send_output_mode_info(r, o->modes[i].w, o->modes[i].h, o->modes[i].hz, f);
	}
}

/* every output; the old events (output_mode of the primary, then output_state) follow */
void output_send_list(struct wl_resource *r)
{
	for (int i = 0; i < NOUT; i++)
		send_one(r, &OUT[i]);
}

void output_send_modes(struct wl_resource *r)
{
	if (wl_resource_get_version(r) >= 9)
		output_send_list(r);
	struct output *p = primary();
	if (p)
		for (int i = 0; i < p->nmodes; i++) {
			const struct outmode *m = &p->modes[i];
			uint32_t flags = (m->w == p->w && m->h == p->h && m->hz == p->hz ? 1 : 0) | (m->preferred ? 2 : 0);
			fl_shell_send_output_mode(r, m->w, m->h, m->hz, flags);
		}
	fl_shell_send_output_state(r, C.scale, C.prim.x2 - C.prim.x1, C.prim.y2 - C.prim.y1, output_pending_secs());
}

/* ---- start and hotplug ---------------------------------------------------------------------------- */

/* the state a connected output should have: saved, else the old mode=/scale= keys (primary), else
 * the preferred mode at 100 % to the right of the others */
static void decide(struct output *o, bool first_primary, struct ocfg *c)
{
	struct ocfg *sv = sv_find(o->name);
	memset(c, 0, sizeof *c);
	if (sv) {
		*c = *sv;
	} else {
		c->enabled = true;
		c->scale = 100;
		c->primary = first_primary;
		int right = 0;
		bool any = false;
		for (int i = 0; i < NOUT; i++)
			if (&OUT[i] != o && OUT[i].enabled) {
				int e = OUT[i].x + OUT[i].w * 100 / OUT[i].scale;
				if (!any || e > right)
					right = e;
				any = true;
			}
		c->x = any ? right : 0;
		if (first_primary && CF.mode_w) {	/* the old keys */
			c->w = CF.mode_w;
			c->h = CF.mode_h;
			c->hz = CF.mode_hz;
			c->scale = CF.scale;
		}
	}
	/* a mode the display does not have: its preferred (and the old hz-less mode=WxH) */
	bool ok = false;
	for (int i = 0; i < o->nmodes; i++)
		ok |= o->modes[i].w == c->w && o->modes[i].h == c->h && o->modes[i].hz == c->hz;
	if (!ok && c->w && !c->hz) {
		int best = 0;
		for (int i = 0; i < o->nmodes; i++)
			if (o->modes[i].w == c->w && o->modes[i].h == c->h && o->modes[i].hz > best)
				best = o->modes[i].hz;
		if (best) {
			c->hz = best;
			ok = true;
		}
	}
	if (!ok && o->nmodes) {
		c->w = o->modes[0].w;
		c->h = o->modes[0].h;
		c->hz = o->modes[0].hz;
	}
	if (c->scale < 100 || c->scale > 200 || !output_scale_ok(c->w, c->h, c->scale))
		c->scale = 100;
}

/* make every connected output as decided; disconnected ones dark. At start nothing is lit yet. */
static void settle_outputs(void)
{
	bool have_primary = false;
	for (int i = 0; i < NOUT; i++)
		if (OUT[i].enabled && OUT[i].primary && OUT[i].connected)
			have_primary = true;
	for (int i = 0; i < NOUT; i++) {
		struct output *o = &OUT[i];
		if (!o->connected) {
			if (o->enabled)
				backend_out_set(o, false, 0, 0, 0);
			o->enabled = false;
			continue;
		}
		struct ocfg c;
		bool was = o->enabled;
		if (was && o->w)
			continue;	/* already lit: hotplug of another output leaves it alone */
		struct ocfg *sv = sv_find(o->name);
		bool fp = !have_primary && !(sv && !sv->primary && NSV);
		decide(o, fp && !have_primary, &c);
		if (!c.enabled)
			continue;
		if (!backend_out_set(o, true, c.w, c.h, c.hz)) {
			/* the saved mode is refused: the preferred one, 100 % */
			if (!o->nmodes || !backend_out_set(o, true, o->modes[0].w, o->modes[0].h, o->modes[0].hz))
				continue;
			c.scale = 100;
		}
		o->scale = c.scale;
		o->x = c.x;
		o->y = c.y;
		o->primary = c.primary || (!have_primary && c.primary);
		if (o->primary)
			have_primary = true;
	}
	if (!primary())	/* nothing is lit: the first connected output, preferred mode */
		for (int i = 0; i < NOUT; i++)
			if (OUT[i].connected && OUT[i].nmodes &&
			    backend_out_set(&OUT[i], true, OUT[i].modes[0].w, OUT[i].modes[0].h, OUT[i].modes[0].hz)) {
				OUT[i].scale = 100;
				OUT[i].x = OUT[i].y = 0;
				break;
			}
	struct output *p = primary();
	for (int i = 0; i < NOUT; i++)
		OUT[i].primary = &OUT[i] == p;
}

/* a connector appeared or went (backend_probe said so) */
void output_hotplug(void)
{
	load_saved();
	settle_outputs();
	if (!layout())
		return;
	fprintf(stderr, "flcomp: outputs changed, desktop %dx%d\n", C.w, C.h);
	after_change();
}

/* At start: outputs lit as saved (desktop.conf output.NAME=), else as the old mode= and scale=. */
void output_init(void)
{
	P.timer = wl_event_loop_add_timer(C.loop, timer_cb, NULL);
	C.scale = 100;
	load_saved();
	settle_outputs();
	if (!layout()) {
		fprintf(stderr, "flcomp: no output could be lit\n");
		C.w = 1024;
		C.h = 768;
		C.prim = (struct rect){ 0, 0, 1024, 768 };
		return;
	}
	last_pw = C.prim.x2 - C.prim.x1;
	last_ph = C.prim.y2 - C.prim.y1;
	for (int i = 0; i < NOUT; i++)
		if (OUT[i].enabled)
			fprintf(stderr, "flcomp: output %s %dx%d@%d scale %d%% at %d,%d%s\n", OUT[i].name, OUT[i].w,
				OUT[i].h, OUT[i].hz, OUT[i].scale, OUT[i].dx, OUT[i].dy, OUT[i].primary ? " (primary)" : "");
}
