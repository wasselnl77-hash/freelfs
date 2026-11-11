/* flsound -- the FreeLFS system sounds: synthesized here, played through PipeWire
 * SPDX-License-Identifier: MIT
 *
 *   flsound NAME          play login, notification, error, tick or screenshot and exit
 *   flsound --force NAME  the same even when sounds=0 (Settings' Test button)
 *   flsound --write NAME FILE.wav   write the sound as a 16 bit mono WAV, play nothing
 *   flsound --list
 *
 * Nothing is recorded or sampled: each sound is a few sine partials with an exponential
 * decay (and a pinch of filtered noise for the shutter), computed in memory at 48 kHz, so
 * there are no audio files to ship and nothing to license. Quiet on purpose, peaks at about
 * -11 dBFS. Played as one PipeWire stream (role Event) that ends when the sound has drained;
 * without a PipeWire server it gives up silently after about a second.
 *
 * sounds=0 in ~/.config/freelfs/desktop.conf (Settings > General > System sounds) turns every
 * sound off. "tick" (the volume key) is rate limited: a held key plays at most about 14 a second.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef FLSOUND_NO_PW
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#endif

#define RATE 48000
#define PEAK 0.28f

/* ---- synthesis ------------------------------------------------------------------------------- */

struct snd {
	const char *name;
	float secs;
};

static const struct snd SOUNDS[] = {
	{"login", 1.60f}, {"notification", 0.70f}, {"error", 0.55f}, {"tick", 0.045f}, {"screenshot", 0.26f},
};

static uint32_t rng_state = 0x1d2125u;

static float noise(void)
{
	rng_state = rng_state * 1664525u + 1013904223u;
	return (float)(int32_t)rng_state / 2147483648.0f;
}

/* one note: a fundamental and two softer partials, a short attack and an exponential decay */
static void note(float *buf, int n, float start, float hz, float amp, float attack, float decay, float h2, float h3)
{
	int s0 = (int)(start * RATE);
	for (int i = s0; i < n; i++) {
		float t = (float)(i - s0) / RATE;
		float env = (t < attack ? t / attack : 1.0f) * expf(-t / decay);
		if (env < 1e-5f && t > attack)
			break;
		float ph = 2.0f * (float)M_PI * hz * t;
		buf[i] += amp * env * (sinf(ph) + h2 * sinf(2 * ph) + h3 * sinf(3 * ph)) / (1.0f + h2 + h3);
	}
}

static float *make(const char *name, int *count)
{
	const struct snd *s = NULL;
	for (size_t i = 0; i < sizeof SOUNDS / sizeof *SOUNDS; i++)
		if (!strcmp(SOUNDS[i].name, name))
			s = &SOUNDS[i];
	if (!s)
		return NULL;
	int n = (int)(s->secs * RATE);
	float *b = calloc((size_t)n, sizeof *b);
	if (!b)
		return NULL;
	rng_state = 0x1d2125u;
	if (!strcmp(name, "login")) {
		/* a rising open chord, G C E G, each note a little after the last, soft attack */
		note(b, n, 0.00f, 392.00f, 1.0f, 0.020f, 0.55f, 0.25f, 0.06f);
		note(b, n, 0.13f, 523.25f, 0.9f, 0.020f, 0.55f, 0.22f, 0.05f);
		note(b, n, 0.26f, 659.25f, 0.8f, 0.020f, 0.60f, 0.20f, 0.05f);
		note(b, n, 0.39f, 783.99f, 0.8f, 0.025f, 0.70f, 0.18f, 0.04f);
		note(b, n, 0.39f, 1567.98f, 0.12f, 0.030f, 0.45f, 0.0f, 0.0f);
	} else if (!strcmp(name, "notification")) {
		/* two soft bell-like tones, the second a fourth lower */
		note(b, n, 0.00f, 1318.51f, 1.0f, 0.006f, 0.16f, 0.30f, 0.0f);
		note(b, n, 0.00f, 2637.02f, 0.10f, 0.006f, 0.07f, 0.0f, 0.0f);
		note(b, n, 0.13f, 987.77f, 0.9f, 0.006f, 0.20f, 0.30f, 0.0f);
		note(b, n, 0.13f, 1975.53f, 0.09f, 0.006f, 0.08f, 0.0f, 0.0f);
	} else if (!strcmp(name, "error")) {
		/* two low, round notes falling a minor third: "uh-oh", not an alarm */
		note(b, n, 0.00f, 311.13f, 1.0f, 0.012f, 0.13f, 0.40f, 0.12f);
		note(b, n, 0.15f, 246.94f, 1.0f, 0.012f, 0.17f, 0.40f, 0.12f);
	} else if (!strcmp(name, "tick")) {
		/* a tiny dry pop */
		note(b, n, 0.0f, 1760.0f, 1.0f, 0.0015f, 0.007f, 0.35f, 0.0f);
	} else if (!strcmp(name, "screenshot")) {
		/* a shutter: two bursts of band-limited noise with a short ring, the second lower */
		float fast = 0, slow = 0;
		for (int i = 0; i < n; i++) {
			float t = (float)i / RATE, env = 0;
			float x = noise();
			fast += 0.45f * (x - fast);
			slow += 0.06f * (x - slow);
			float shaped = fast - slow;	/* a band of noise without the rumble and the hiss */
			if (t < 0.012f)
				env = expf(-t / 0.0028f);
			else if (t >= 0.070f && t < 0.100f)
				env = 0.8f * expf(-(t - 0.070f) / 0.0045f);
			b[i] += 0.9f * env * shaped;
		}
		note(b, n, 0.000f, 2400.0f, 0.22f, 0.0008f, 0.006f, 0.0f, 0.0f);
		note(b, n, 0.070f, 1500.0f, 0.22f, 0.0008f, 0.010f, 0.0f, 0.0f);
	}
	/* normalise to the quiet peak, fade the last 20 ms so nothing clicks at the end */
	float pk = 0;
	for (int i = 0; i < n; i++)
		if (fabsf(b[i]) > pk)
			pk = fabsf(b[i]);
	float g = pk > 0 ? PEAK / pk : 1;
	int fade = RATE / 50;
	for (int i = 0; i < n; i++) {
		float f = i > n - fade ? (float)(n - i) / fade : 1.0f;
		b[i] *= g * f;
	}
	*count = n;
	return b;
}

static int16_t *to_pcm(const float *f, int n)
{
	int16_t *p = malloc((size_t)n * sizeof *p);
	if (!p)
		return NULL;
	for (int i = 0; i < n; i++) {
		float v = f[i] * 32767.0f;
		p[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : lrintf(v));
	}
	return p;
}

static int write_wav(const char *path, const int16_t *pcm, int n)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	uint32_t data = (uint32_t)n * 2, riff = 36 + data, rate = RATE, brate = RATE * 2, fmtlen = 16;
	uint16_t pcmfmt = 1, ch = 1, align = 2, bits = 16;
	fwrite("RIFF", 1, 4, f);
	fwrite(&riff, 4, 1, f);
	fwrite("WAVEfmt ", 1, 8, f);
	fwrite(&fmtlen, 4, 1, f);
	fwrite(&pcmfmt, 2, 1, f);
	fwrite(&ch, 2, 1, f);
	fwrite(&rate, 4, 1, f);
	fwrite(&brate, 4, 1, f);
	fwrite(&align, 2, 1, f);
	fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f);
	fwrite(&data, 4, 1, f);
	fwrite(pcm, 2, (size_t)n, f);
	return fclose(f);
}

/* ---- desktop.conf: sounds=0 -------------------------------------------------------------------- */

static int sounds_enabled(void)
{
	char path[512];
	const char *e = getenv("FLUI_CONF"), *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
	if (e && *e)
		snprintf(path, sizeof path, "%s", e);
	else if (x && *x)
		snprintf(path, sizeof path, "%s/freelfs/desktop.conf", x);
	else
		snprintf(path, sizeof path, "%s/.config/freelfs/desktop.conf", h ? h : "");
	FILE *f = fopen(path, "r");
	if (!f)
		return 1;
	char line[200];
	int on = 1;
	while (fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '=');
		if (line[0] == '#' || !eq)
			continue;
		*eq = 0;
		if (!strcmp(line, "sounds"))
			on = atoi(eq + 1) != 0;
	}
	fclose(f);
	return on;
}

/* "tick" at most every 70 ms: the modification time of a file in the runtime dir is the clock */
static int tick_allowed(void)
{
	const char *rt = getenv("XDG_RUNTIME_DIR");
	if (!rt || !*rt)
		return 1;
	char path[512];
	snprintf(path, sizeof path, "%s/flsound-tick", rt);
	struct stat st;
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	if (stat(path, &st) == 0) {
		long ms = (now.tv_sec - st.st_mtim.tv_sec) * 1000L + (now.tv_nsec - st.st_mtim.tv_nsec) / 1000000L;
		if (ms >= 0 && ms < 70)
			return 0;
	} else {
		int fd = open(path, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
		if (fd >= 0)
			close(fd);
	}
	struct timespec ts[2] = {now, now};
	utimensat(AT_FDCWD, path, ts, 0);
	return 1;
}

/* ---- PipeWire ------------------------------------------------------------------------------------- */

#ifndef FLSOUND_NO_PW
struct play {
	struct pw_main_loop *loop;
	struct pw_stream *stream;
	const int16_t *pcm;
	int n, pos, flushed, ok;
};

static void on_process(void *ud)
{
	struct play *p = ud;
	struct pw_buffer *b = pw_stream_dequeue_buffer(p->stream);
	if (!b)
		return;
	struct spa_buffer *sb = b->buffer;
	int16_t *dst = sb->datas[0].data;
	if (!dst) {
		pw_stream_queue_buffer(p->stream, b);
		return;
	}
	int max = (int)(sb->datas[0].maxsize / sizeof(int16_t));
	if (b->requested && (int)b->requested < max)
		max = (int)b->requested;
	int n = p->n - p->pos < max ? p->n - p->pos : max;
	if (n > 0)
		memcpy(dst, p->pcm + p->pos, (size_t)n * sizeof *dst);
	p->pos += n > 0 ? n : 0;
	sb->datas[0].chunk->offset = 0;
	sb->datas[0].chunk->stride = sizeof(int16_t);
	sb->datas[0].chunk->size = (uint32_t)(n > 0 ? n : 0) * sizeof(int16_t);
	pw_stream_queue_buffer(p->stream, b);
	p->ok = 1;
	if (p->pos >= p->n && !p->flushed) {
		p->flushed = 1;
		pw_stream_flush(p->stream, true);	/* drain: the drained event ends the program */
	}
}

static void on_drained(void *ud)
{
	pw_main_loop_quit(((struct play *)ud)->loop);
}

static void on_state(void *ud, enum pw_stream_state old, enum pw_stream_state now, const char *err)
{
	struct play *p = ud;
	if (now == PW_STREAM_STATE_ERROR || now == PW_STREAM_STATE_UNCONNECTED)
		pw_main_loop_quit(p->loop);
}

static void on_timeout(void *ud, uint64_t exp)
{
	pw_main_loop_quit(((struct play *)ud)->loop);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.process = on_process,
	.drained = on_drained,
	.state_changed = on_state,
};

static int play_pw(const int16_t *pcm, int n)
{
	int argc = 1;
	char *argv[] = {"flsound", NULL}, **av = argv;
	pw_init(&argc, &av);
	struct play p = {.pcm = pcm, .n = n};
	p.loop = pw_main_loop_new(NULL);
	if (!p.loop)
		return 1;
	p.stream = pw_stream_new_simple(pw_main_loop_get_loop(p.loop), "flsound",
	                                pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
	                                                  PW_KEY_MEDIA_ROLE, "Event", PW_KEY_APP_NAME, "FreeLFS",
	                                                  PW_KEY_NODE_NAME, "flsound", NULL),
	                                &stream_events, &p);
	if (!p.stream)
		return 1;
	uint8_t buf[1024];
	struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(buf, sizeof buf);
	const struct spa_pod *params[1];
	params[0] = spa_format_audio_raw_build(&pb, SPA_PARAM_EnumFormat,
	                                       &SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_S16, .channels = 1,
	                                                                .rate = RATE, .position = {SPA_AUDIO_CHANNEL_MONO}));
	if (pw_stream_connect(p.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
	                      PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
	                      params, 1) < 0)
		return 1;
	/* never hang: the sound's length plus a few seconds, whatever happens */
	struct spa_source *t = pw_loop_add_timer(pw_main_loop_get_loop(p.loop), on_timeout, &p);
	struct timespec first = {.tv_sec = 3 + n / RATE}, none = {0};
	pw_loop_update_timer(pw_main_loop_get_loop(p.loop), t, &first, &none, false);
	pw_main_loop_run(p.loop);
	int ok = p.ok;
	pw_stream_destroy(p.stream);
	pw_main_loop_destroy(p.loop);
	pw_deinit();
	return ok ? 0 : 1;
}
#endif

/* ---- main ---------------------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	int force = 0, list = 0;
	const char *name = NULL, *wav = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--force"))
			force = 1;
		else if (!strcmp(argv[i], "--list"))
			list = 1;
		else if (!strcmp(argv[i], "--write") && i + 2 < argc) {
			name = argv[++i];
			wav = argv[++i];
		} else if (argv[i][0] != '-' && !name)
			name = argv[i];
		else {
			fprintf(stderr, "usage: flsound [--force] NAME | flsound --write NAME FILE.wav | flsound --list\n");
			return !strcmp(argv[i], "-h") || !strcmp(argv[i], "--help") ? 0 : 2;
		}
	}
	if (list) {
		for (size_t i = 0; i < sizeof SOUNDS / sizeof *SOUNDS; i++)
			printf("%s\n", SOUNDS[i].name);
		return 0;
	}
	if (!name) {
		fprintf(stderr, "usage: flsound [--force] NAME  (flsound --list)\n");
		return 2;
	}
	if (!wav && !force && !sounds_enabled())
		return 0;
	if (!wav && !strcmp(name, "tick") && !tick_allowed())
		return 0;
	int n = 0;
	float *f = make(name, &n);
	if (!f) {
		fprintf(stderr, "flsound: no sound called %s\n", name);
		return 2;
	}
	int16_t *pcm = to_pcm(f, n);
	free(f);
	if (!pcm)
		return 1;
	if (wav) {
		int rc = write_wav(wav, pcm, n);
		free(pcm);
		if (rc) {
			perror(wav);
			return 1;
		}
		return 0;
	}
#ifndef FLSOUND_NO_PW
	int rc = play_pw(pcm, n);
#else
	int rc = 1;
#endif
	free(pcm);
	return rc;
}
