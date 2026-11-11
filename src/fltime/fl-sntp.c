/* fl-sntp -- a small SNTP client that keeps the clock right (RFC 4330).
 * SPDX-License-Identifier: MIT
 *
 *   fl-sntp                 daemon: ask the servers, set the clock, ask again later
 *   fl-sntp -1              ask once, set the clock, exit
 *   fl-sntp -q SERVER[:PORT]   ask one server and print offset and delay in ms; the
 *                           clock is not touched (needs no privileges)
 *
 * Servers: SERVERS="a b" in /etc/fltime.conf (default pool.ntp.org), each may be
 * HOST or HOST:PORT. A clock that is off by 0.2 s or more is stepped, a smaller
 * error is slewed with adjtime(3). The state goes to /run/fltime.status
 * (state=synced|waiting, last=EPOCH, offset_ms=, server=); the file is removed when
 * the daemon ends. Until the first answer it asks again after 10 s, then every
 * 20, 40, ... up to 5 minutes; afterwards once an hour. The hardware clock is
 * saved by rc.shutdown, as it is for a clock set by hand.
 *
 * -c FILE and -s FILE replace the paths of the configuration and the status file.
 */
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define NTP_UNIX 2208988800u
#define STEP_LIMIT 0.2

static const char *conf_path = "/etc/fltime.conf";
static const char *status_path = "/run/fltime.status";
static volatile sig_atomic_t stop;

struct pkt {
	uint8_t flags, stratum, poll;
	int8_t prec;
	uint32_t root_delay, root_disp, ref_id;
	uint32_t ref[2], orig[2], rx[2], tx[2];
};
_Static_assert(sizeof(struct pkt) == 48, "NTP packet");

static double now_real(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void to_ntp(double t, uint32_t out[2])
{
	double s = t + NTP_UNIX;
	uint32_t sec = (uint32_t)s;
	out[0] = htonl(sec);
	out[1] = htonl((uint32_t)((s - sec) * 4294967296.0));
}

static double from_ntp(const uint32_t in[2])
{
	return (double)ntohl(in[0]) - NTP_UNIX + ntohl(in[1]) / 4294967296.0;
}

/* one exchange; 0 and offset/delay in seconds on success */
static int query(const char *host, const char *port, double *offset, double *delay, int timeout_ms)
{
	struct addrinfo hints = {.ai_socktype = SOCK_DGRAM, .ai_family = AF_UNSPEC}, *ai, *a;
	if (getaddrinfo(host, port, &hints, &ai) != 0)
		return -1;
	int ok = -1;
	for (a = ai; a && ok < 0; a = a->ai_next) {
		int fd = socket(a->ai_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
		if (fd < 0)
			continue;
		if (connect(fd, a->ai_addr, a->ai_addrlen) < 0) {
			close(fd);
			continue;
		}
		struct pkt q;
		memset(&q, 0, sizeof q);
		q.flags = 0x23; /* LI 0, version 4, mode 3 (client) */
		double t1 = now_real();
		to_ntp(t1, q.tx);
		if (send(fd, &q, sizeof q, 0) == (ssize_t)sizeof q) {
			struct pollfd pf = {.fd = fd, .events = POLLIN};
			struct pkt r;
			if (poll(&pf, 1, timeout_ms) == 1 && recv(fd, &r, sizeof r, 0) >= (ssize_t)sizeof r) {
				double t4 = now_real();
				/* an answer to our question, from a server that knows the time */
				if ((r.flags & 7) == 4 && (r.flags >> 6) != 3 && r.stratum >= 1 && r.stratum <= 15 &&
				    r.orig[0] == q.tx[0] && r.orig[1] == q.tx[1] && (r.tx[0] || r.tx[1])) {
					double t2 = from_ntp(r.rx), t3 = from_ntp(r.tx);
					*offset = ((t2 - t1) + (t3 - t4)) / 2;
					*delay = (t4 - t1) - (t3 - t2);
					ok = 0;
				}
			}
		}
		close(fd);
	}
	freeaddrinfo(ai);
	return ok;
}

/* HOST or HOST:PORT (a name with more than one colon is an IPv6 address: no port) */
static int ask(const char *server, double *offset, double *delay)
{
	char host[256], *port = "123";
	snprintf(host, sizeof host, "%s", server);
	char *c = strchr(host, ':');
	if (c && !strchr(c + 1, ':')) {
		*c = 0;
		port = c + 1;
	}
	double bo = 0, bd = 1e9;
	int got = 0;
	for (int i = 0; i < 3; i++) { /* three tries, the quickest answer is the most exact */
		double o, d;
		if (query(host, port, &o, &d, 2000) == 0 && d >= 0 && d < 3) {
			if (d < bd) {
				bd = d;
				bo = o;
			}
			got = 1;
		}
	}
	if (!got)
		return -1;
	*offset = bo;
	*delay = bd;
	return 0;
}

static void write_status(const char *state, double offset_s, const char *server, time_t last)
{
	char tmp[300];
	snprintf(tmp, sizeof tmp, "%s.tmp", status_path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "state=%s\nlast=%lld\noffset_ms=%.0f\nserver=%s\n", state, (long long)last, offset_s * 1000, server);
	if (fclose(f) == 0) {
		chmod(tmp, 0644);
		rename(tmp, status_path);
	}
}

static int apply(double offset)
{
	if (offset >= STEP_LIMIT || offset <= -STEP_LIMIT) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		double t = ts.tv_sec + ts.tv_nsec / 1e9 + offset;
		ts.tv_sec = (time_t)t;
		ts.tv_nsec = (long)((t - (double)ts.tv_sec) * 1e9);
		return clock_settime(CLOCK_REALTIME, &ts);
	}
	if (offset > -0.001 && offset < 0.001)
		return 0;
	struct timeval d = {.tv_sec = (time_t)offset, .tv_usec = (suseconds_t)((offset - (double)(time_t)offset) * 1e6)};
	return adjtime(&d, NULL);
}

/* SERVERS="a b c" from the configuration, else pool.ntp.org */
static int servers(char out[8][128])
{
	int n = 0;
	FILE *f = fopen(conf_path, "r");
	char line[512];
	while (f && fgets(line, sizeof line, f)) {
		if (strncmp(line, "SERVERS=", 8))
			continue;
		char *v = line + 8;
		v[strcspn(v, "\r\n")] = 0;
		n = 0;
		for (char *p = strtok(v, " \t\"'"); p && n < 8; p = strtok(NULL, " \t\"'"))
			snprintf(out[n++], 128, "%s", p);
	}
	if (f)
		fclose(f);
	if (!n)
		snprintf(out[n++], 128, "pool.ntp.org");
	return n;
}

static void on_term(int sig)
{
	(void)sig;
	stop = 1;
}

static void nap(int seconds)
{
	for (int i = 0; i < seconds * 4 && !stop; i++)
		usleep(250000);
}

int main(int argc, char **argv)
{
	int once = 0;
	const char *only = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-1"))
			once = 1;
		else if (!strcmp(argv[i], "-q") && i + 1 < argc)
			only = argv[++i];
		else if (!strcmp(argv[i], "-c") && i + 1 < argc)
			conf_path = argv[++i];
		else if (!strcmp(argv[i], "-s") && i + 1 < argc)
			status_path = argv[++i];
		else {
			fputs("usage: fl-sntp [-1] [-q SERVER[:PORT]] [-c CONF] [-s STATUSFILE]\n", stderr);
			return 2;
		}
	}
	if (only) {
		double o, d;
		if (ask(only, &o, &d) < 0) {
			fprintf(stderr, "fl-sntp: no answer from %s\n", only);
			return 1;
		}
		printf("offset_ms=%.1f delay_ms=%.1f\n", o * 1000, d * 1000);
		return 0;
	}
	struct sigaction sa = {.sa_handler = on_term};
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	int wait_s = 10;
	while (!stop) {
		char list[8][128];
		int n = servers(list), done = 0;
		for (int i = 0; i < n && !done && !stop; i++) {
			double o, d;
			if (ask(list[i], &o, &d) < 0)
				continue;
			if (apply(o) == 0) {
				write_status("synced", o, list[i], time(NULL));
				done = 1;
			} else {
				fprintf(stderr, "fl-sntp: cannot set the clock: %s\n", strerror(errno));
			}
		}
		if (once)
			return done ? 0 : 1;
		if (done) {
			wait_s = 10;
			nap(3600);
		} else {
			write_status("waiting", 0, "", 0);
			nap(wait_s);
			wait_s = wait_s * 2 > 300 ? 300 : wait_s * 2;
		}
	}
	unlink(status_path);
	return 0;
}
