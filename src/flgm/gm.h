/* gm.h -- General Manager, shared declarations
 * SPDX-License-Identifier: MIT
 */
#ifndef GM_H
#define GM_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint64_t u64;

/* where the kernel's files are; tests/fake-test.c builds against a fake tree */
#ifndef PROC_DIR
#define PROC_DIR "/proc"
#endif
#ifndef SYS_DIR
#define SYS_DIR "/sys"
#endif

/* ---- history (60 one-second samples, oldest first) ---------------------------------- */

#define HIST 61
#define MAXCPU 128
#define MAXDISK 8
#define MAXNET 8
#define MAXGPU 4

struct hist {
	float v[HIST];
	int n; /* valid samples, at the end of v */
};

static inline void hist_push(struct hist *h, double x)
{
	memmove(h->v, h->v + 1, (HIST - 1) * sizeof(float));
	h->v[HIST - 1] = (float)x;
	if (h->n < HIST)
		h->n++;
}

static inline float hist_max(const struct hist *h)
{
	float m = 0;
	for (int i = HIST - h->n; i < HIST; i++)
		if (h->v[i] > m)
			m = h->v[i];
	return m;
}

/* ---- system numbers (sys.c) -------------------------------------------------------- */

struct cpuinfo {
	char model[96];
	int logical, cores, sockets;
	double max_ghz; /* 0: unknown */
	char l1[24], l2[24], l3[24];
};

struct disk {
	char name[24], model[56], type[24];
	u64 bytes;
	u64 p_rsec, p_wsec, p_ticks, p_ios, p_wt;
	int have_prev;
	double active, rd_bps, wr_bps, resp_ms;
	struct hist h_active, h_rd, h_wr;
};

struct net {
	char name[24], label[40], ip4[48], state[16];
	int wireless;
	long speed_mbps; /* -1 unknown */
	u64 rx, tx, p_rx, p_tx;
	int have_prev;
	double rx_bps, tx_bps;
	struct hist h_rx, h_tx;
};

struct gpu {
	char path[96], name[112], driver[32], vendor[24], pci[16];
	int has_busy, has_vram;
	double busy;
	u64 vram_used, vram_total, gtt_used, gtt_total;
	int temp_c; /* -1 unknown */
	double power_w; /* <0 unknown */
	struct hist h_busy, h_vram;
};

struct sys {
	double dt, t_prev;
	int primed;
	struct cpuinfo ci;
	int ncpu;
	double cpu_total, ghz;
	double core[MAXCPU];
	struct hist h_cpu, h_ghz, h_core[MAXCPU];
	u64 p_tot[MAXCPU + 1], p_idle[MAXCPU + 1];
	double load[3];
	u64 mem_total, mem_avail, mem_free, mem_cached, mem_buffers, mem_slab, mem_shmem;
	u64 swap_total, swap_free, commit, commit_limit;
	struct hist h_mem;
	int nproc, nthreads;
	double uptime;
	struct disk disk[MAXDISK];
	int ndisk;
	double disk_active; /* busiest disk, 0-100 */
	struct net net[MAXNET];
	int nnet;
	double net_bps; /* all adapters, bits per second */
	struct gpu gpu[MAXGPU];
	int ngpu;
};

void sys_init(struct sys *s);
void sys_sample(struct sys *s);
double now_s(void);

/* formatting into a static ring of buffers; fine for a few calls per statement */
const char *fmt_bytes(double b);   /* "1.5 GB" */
const char *fmt_mem(u64 kb);       /* "245.3 MB" */
const char *fmt_rate(double bps);  /* "0 MB/s" (bytes per second) */
const char *fmt_bits(double bps);  /* "12.3 Mbps" (input is bits per second) */
const char *fmt_pct(double p);     /* "0%" "3.2%" */
const char *fmt_uptime(double s);  /* "0:05:12:33" */
const char *fmt_ghz(double g);     /* "3.42 GHz" */
double nice_ceil(double v);        /* 1, 2, 5 times a power of ten above v */

/* ---- processes (proc.c) ------------------------------------------------------------ */

struct proc {
	int pid, ppid;
	unsigned uid;
	char comm[20], name[64], state;
	char *cmd;
	int threads, kernel;
	u64 ticks, p_ticks;
	u64 priv_kb; /* resident minus shared */
	u64 start;   /* starttime from stat: tells a reused pid apart */
	u64 io, p_io;
	int io_ok, have_prev;
	double cpu, disk_bps;
	int own_app; /* the program's own .desktop entry, or -1 */
	int app; /* the app it belongs to (own or an ancestor's), or -1 */
	int root_pid; /* the app process this one belongs to, or 0 */
	int seen;
};

struct procs {
	struct proc *p;
	int n, cap;
	double t_prev;
	int ncpu;
	long hz;
	int failed; /* the last scan could not read the process table */
};

struct appinfo {
	char exec[64], name[64], icon[32];
};

void apps_load(void);
int apps_count(void);
const struct appinfo *apps_get(int i);
void procs_init(struct procs *ps, int ncpu);
void procs_scan(struct procs *ps);
void gm_clean_utf8(char *s); /* control bytes and invalid UTF-8 become '?' */
const char *user_name(unsigned uid);
const char *state_name(char s);
struct proc *procs_find(struct procs *ps, int pid);

/* ---- startup apps (startup.c) ------------------------------------------------------ */

struct startup {
	char file[128];    /* file name inside an autostart directory */
	char path[512];    /* where it was read from */
	char name[96], comment[160], exec[256];
	int user;          /* 1: in ~/.config/autostart, 0: system wide */
	int enabled;
};

int startup_load(struct startup **out); /* returns the count; caller frees *out */
int startup_set(const struct startup *s, int enabled); /* 0, or -1 with errno */
const char *startup_user_dir(void);

#endif
