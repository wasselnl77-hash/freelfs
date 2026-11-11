/* mpris.h -- the MPRIS (org.mpris.MediaPlayer2) service of flmusic, on libdbus
 * SPDX-License-Identifier: MIT
 */
#ifndef MPRIS_H
#define MPRIS_H

#include <flui.h>
#include <stdint.h>

struct mpris;

enum { LOOP_NONE, LOOP_TRACK, LOOP_LIST };

struct mpris_state {
	const char *status; /* "Playing", "Paused", "Stopped" */
	int loop, shuffle;
	double volume;      /* 0..1 */
	int64_t position_us;
	int can_next, can_prev, can_play;
	/* metadata of the current track; title NULL when there is none */
	const char *title, *artist, *album, *uri;
	int64_t length_us;
	int track_no;       /* the object path suffix */
};

struct mpris_ops {
	void *ud;
	void (*play)(void *ud);
	void (*pause)(void *ud);
	void (*playpause)(void *ud);
	void (*stop)(void *ud);
	void (*next)(void *ud);
	void (*prev)(void *ud);
	void (*seek)(void *ud, int64_t offset_us);
	void (*set_position)(void *ud, int64_t pos_us);
	void (*set_volume)(void *ud, double v);
	void (*set_shuffle)(void *ud, int on);
	void (*set_loop)(void *ud, int loop);
	void (*open_uri)(void *ud, const char *uri);
	void (*raise)(void *ud);
	void (*quit)(void *ud);
	void (*get)(void *ud, struct mpris_state *out);
};

/* Connect to the session bus and own org.mpris.MediaPlayer2.flmusic. NULL when there is
 * no bus or the name is taken (a second window then simply has no MPRIS). */
struct mpris *mpris_start(fl_app *app, const struct mpris_ops *ops);
/* Something changed: send PropertiesChanged (with the state ops->get returns now). */
void mpris_changed(struct mpris *m);
/* The position jumped (seek): send Seeked. */
void mpris_seeked(struct mpris *m, int64_t pos_us);
void mpris_stop(struct mpris *m);

/* flmusic --ctl CMD: call a method of the first MPRIS player on the bus.
 * CMD: play, pause, play-pause, stop, next, previous, status. Returns an exit status. */
int mpris_ctl(const char *cmd);

#endif
