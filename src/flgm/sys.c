/* sys.c -- machine-wide numbers from /proc and /sys, and number formatting
 * SPDX-License-Identifier: MIT
 */
#include "gm.h"

#include <arpa/inet.h>
#include <stdarg.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <math.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

double now_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* ---- small file helpers ---------------------------------------------------------------- */

static int slurp(const char *path, char *buf, size_t cap)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ssize_t n = read(fd, buf, cap - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = 0;
	return (int)n;
}

static void trim(char *s)
{
	size_t n = strlen(s);
	while (n && isspace((unsigned char)s[n - 1]))
		s[--n] = 0;
	char *p = s;
	while (*p && isspace((unsigned char)*p))
		p++;
	if (p != s)
		memmove(s, p, strlen(p) + 1);
}

static int read_str(char *out, size_t cap, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int read_str(char *out, size_t cap, const char *fmt, ...)
{
	char path[300];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(path, sizeof path, fmt, ap);
	va_end(ap);
	int n = slurp(path, out, cap);
	if (n >= 0)
		trim(out);
	return n;
}

static long long read_ll(const char *fmt, const char *a, const char *b, long long def)
{
	char path[300], buf[64];
	snprintf(path, sizeof path, fmt, a, b);
	if (slurp(path, buf, sizeof buf) <= 0)
		return def;
	return atoll(buf);
}

/* ---- formatting ------------------------------------------------------------------------- */

static char *ring(void)
{
	static char bufs[16][40];
	static int i;
	return bufs[i++ & 15];
}

const char *fmt_bytes(double b)
{
	char *o = ring();
	if (b < 1024)
		snprintf(o, 40, "%.0f B", b);
	else if (b < 1024 * 1024)
		snprintf(o, 40, "%.0f KB", b / 1024);
	else if (b < 1024.0 * 1024 * 1024)
		snprintf(o, 40, "%.1f MB", b / 1048576);
	else if (b < 1024.0 * 1024 * 1024 * 1024)
		snprintf(o, 40, "%.1f GB", b / 1073741824.0);
	else
		snprintf(o, 40, "%.2f TB", b / 1099511627776.0);
	return o;
}

const char *fmt_mem(u64 kb)
{
	char *o = ring();
	double b = (double)kb * 1024;
	if (kb == 0)
		snprintf(o, 40, "0 MB");
	else if (b < 1024.0 * 1024)
		snprintf(o, 40, "%.1f MB", b / 1048576);
	else if (b < 1024.0 * 1024 * 1024)
		snprintf(o, 40, "%.1f MB", b / 1048576);
	else
		snprintf(o, 40, "%.2f GB", b / 1073741824.0);
	return o;
}

const char *fmt_rate(double bps)
{
	char *o = ring();
	if (bps < 1)
		snprintf(o, 40, "0 MB/s");
	else if (bps < 1024.0 * 1024)
		snprintf(o, 40, "%.0f KB/s", bps < 1024 ? 1.0 : bps / 1024);
	else if (bps < 1024.0 * 1024 * 1024)
		snprintf(o, 40, "%.1f MB/s", bps / 1048576);
	else
		snprintf(o, 40, "%.2f GB/s", bps / 1073741824.0);
	return o;
}

const char *fmt_bits(double bps)
{
	char *o = ring();
	if (bps < 1)
		snprintf(o, 40, "0 Kbps");
	else if (bps < 1e3)
		snprintf(o, 40, "%.0f bps", bps);
	else if (bps < 1e6)
		snprintf(o, 40, bps < 1e5 ? "%.1f Kbps" : "%.0f Kbps", bps / 1e3);
	else if (bps < 1e9)
		snprintf(o, 40, bps < 1e8 ? "%.1f Mbps" : "%.0f Mbps", bps / 1e6);
	else
		snprintf(o, 40, "%.2f Gbps", bps / 1e9);
	return o;
}

const char *fmt_pct(double p)
{
	char *o = ring();
	if (p < 0.05)
		snprintf(o, 40, "0%%");
	else if (p < 10)
		snprintf(o, 40, "%.1f%%", p);
	else
		snprintf(o, 40, "%.0f%%", p);
	return o;
}

const char *fmt_uptime(double s)
{
	char *o = ring();
	long t = (long)s;
	snprintf(o, 40, "%ld:%02ld:%02ld:%02ld", t / 86400, t / 3600 % 24, t / 60 % 60, t % 60);
	return o;
}

const char *fmt_ghz(double g)
{
	char *o = ring();
	if (g <= 0)
		snprintf(o, 40, "\xe2\x80\x94");
	else
		snprintf(o, 40, "%.2f GHz", g);
	return o;
}

double nice_ceil(double v)
{
	if (v <= 0)
		return 1;
	double e = pow(10, floor(log10(v)));
	double m = v / e;
	double n = m <= 1 ? 1 : m <= 2 ? 2 : m <= 5 ? 5 : 10;
	return n * e;
}

/* ---- the machine ------------------------------------------------------------------------- */

static void fmt_cache_kb(long kb, char *out, size_t cap)
{
	if (kb >= 1024 && kb % 1024 == 0)
		snprintf(out, cap, "%ld MB", kb / 1024);
	else if (kb >= 1024)
		snprintf(out, cap, "%.1f MB", kb / 1024.0);
	else
		snprintf(out, cap, "%ld KB", kb);
}

static void cpu_static(struct sys *s)
{
	struct cpuinfo *ci = &s->ci;
	static char buf[65536];
	int n = slurp("/proc/cpuinfo", buf, sizeof buf);
	ci->model[0] = 0;
	if (n > 0) {
		static const char *keys[] = {"model name", "Model", "Hardware", "Processor", "cpu model", NULL};
		for (int k = 0; keys[k] && !ci->model[0]; k++) {
			for (char *l = buf; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
				size_t kl = strlen(keys[k]);
				if (!strncmp(l, keys[k], kl) && (l[kl] == '\t' || l[kl] == ' ' || l[kl] == ':')) {
					char *c = strchr(l, ':');
					if (!c)
						continue;
					snprintf(ci->model, sizeof ci->model, "%.*s", (int)strcspn(c + 1, "\n"), c + 1);
					trim(ci->model);
					break;
				}
			}
		}
	}
	if (!ci->model[0])
		snprintf(ci->model, sizeof ci->model, "Processor");
	/* collapse the double spaces some CPUs have in their name */
	for (char *p = ci->model; *p; p++)
		while (p[0] == ' ' && p[1] == ' ')
			memmove(p, p + 1, strlen(p));
	ci->logical = s->ncpu;

	int seen[MAXCPU][2], ns = 0;
	int sock[MAXCPU], nsock = 0;
	for (int i = 0; i < s->ncpu; i++) {
		char a[16];
		snprintf(a, sizeof a, "%d", i);
		long long pk = read_ll("/sys/devices/system/cpu/cpu%s/topology/%s", a, "physical_package_id", -1);
		long long ck = read_ll("/sys/devices/system/cpu/cpu%s/topology/%s", a, "core_id", i);
		if (pk < 0)
			pk = 0;
		int f = 0;
		for (int j = 0; j < ns; j++)
			if (seen[j][0] == pk && seen[j][1] == ck)
				f = 1;
		if (!f && ns < MAXCPU) {
			seen[ns][0] = (int)pk;
			seen[ns][1] = (int)ck;
			ns++;
		}
		f = 0;
		for (int j = 0; j < nsock; j++)
			if (sock[j] == pk)
				f = 1;
		if (!f && nsock < MAXCPU)
			sock[nsock++] = (int)pk;
	}
	ci->cores = ns ? ns : s->ncpu;
	ci->sockets = nsock ? nsock : 1;
	long long mx = read_ll("/sys/devices/system/cpu/cpu%s/cpufreq/%s", "0", "cpuinfo_max_freq", 0);
	ci->max_ghz = mx > 0 ? (double)mx / 1e6 : 0;

	/* caches of cpu0 */
	long l1 = 0, l2 = 0, l3 = 0;
	int shared2 = 0, shared3 = 0;
	for (int i = 0; i < 8; i++) {
		char a[16], lvl[16], type[24], size[24], sh[96];
		snprintf(a, sizeof a, "%d", i);
		char p[200];
		snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu0/cache/index%d", i);
		if (read_str(lvl, sizeof lvl, "%s/level", p) < 0)
			break;
		read_str(type, sizeof type, "%s/type", p);
		read_str(size, sizeof size, "%s/size", p);
		sh[0] = 0;
		read_str(sh, sizeof sh, "%s/shared_cpu_list", p);
		long kb = atol(size);
		if (strchr(size, 'M'))
			kb *= 1024;
		int lv = atoi(lvl);
		int shared = strpbrk(sh, ",-") != NULL;
		if (lv == 1)
			l1 += kb;
		else if (lv == 2) {
			l2 += kb;
			shared2 = shared;
		} else if (lv == 3) {
			l3 += kb;
			shared3 = shared;
		}
	}
	char t[24];
	ci->l1[0] = ci->l2[0] = ci->l3[0] = 0;
	if (l1) {
		fmt_cache_kb(l1, t, sizeof t);
		snprintf(ci->l1, sizeof ci->l1, "%s / core", t);
	}
	if (l2) {
		fmt_cache_kb(l2, t, sizeof t);
		snprintf(ci->l2, sizeof ci->l2, shared2 ? "%s" : "%s / core", t);
	}
	if (l3) {
		fmt_cache_kb(l3, t, sizeof t);
		snprintf(ci->l3, sizeof ci->l3, shared3 ? "%s" : "%s / core", t);
	}
}

/* ---- GPUs ------------------------------------------------------------------------------ */

static void pci_lookup(unsigned ven, unsigned dev, char *vname, size_t vcap, char *dname, size_t dcap)
{
	static const char *files[] = {"/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids",
	                              "/usr/share/pci.ids", NULL};
	vname[0] = dname[0] = 0;
	for (int i = 0; files[i]; i++) {
		FILE *f = fopen(files[i], "re");
		if (!f)
			continue;
		char line[256];
		int in_vendor = 0;
		while (fgets(line, sizeof line, f)) {
			if (line[0] == '#' || line[0] == '\n')
				continue;
			if (line[0] != '\t') {
				if (in_vendor)
					break;
				unsigned v;
				if (sscanf(line, "%x", &v) == 1 && v == ven) {
					in_vendor = 1;
					char *nm = line + 4;
					while (*nm == ' ')
						nm++;
					snprintf(vname, vcap, "%.*s", (int)strcspn(nm, "\n"), nm);
				}
			} else if (in_vendor && line[1] != '\t') {
				unsigned d;
				if (sscanf(line + 1, "%x", &d) == 1 && d == dev) {
					char *nm = line + 5;
					while (*nm == ' ')
						nm++;
					snprintf(dname, dcap, "%.*s", (int)strcspn(nm, "\n"), nm);
					break;
				}
			}
		}
		fclose(f);
		return;
	}
}

static void bracket(char *s) /* "Rembrandt [Radeon 680M]" -> "Radeon 680M" */
{
	char *a = strrchr(s, '['), *b = strrchr(s, ']');
	if (a && b && b > a) {
		*b = 0;
		memmove(s, a + 1, strlen(a + 1) + 1);
	}
}

static void gpu_scan(struct sys *s)
{
	DIR *d = opendir("/sys/class/drm");
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d)) && s->ngpu < MAXGPU) {
		if (strncmp(e->d_name, "card", 4) || strchr(e->d_name, '-') || !isdigit((unsigned char)e->d_name[4]))
			continue;
		struct gpu *g = &s->gpu[s->ngpu];
		memset(g, 0, sizeof *g);
		snprintf(g->path, sizeof g->path, "/sys/class/drm/%s/device", e->d_name);
		char buf[128], link[160];
		snprintf(buf, sizeof buf, "%s/driver", g->path);
		ssize_t n = readlink(buf, link, sizeof link - 1);
		if (n > 0) {
			link[n] = 0;
			const char *b = strrchr(link, '/');
			snprintf(g->driver, sizeof g->driver, "%s", b ? b + 1 : link);
		}
		unsigned ven = 0, dev = 0;
		char t[32];
		if (read_str(t, sizeof t, "%s/vendor", g->path) > 0)
			ven = (unsigned)strtoul(t, NULL, 16);
		if (read_str(t, sizeof t, "%s/device", g->path) > 0)
			dev = (unsigned)strtoul(t, NULL, 16);
		snprintf(g->pci, sizeof g->pci, "%04x:%04x", ven, dev);
		const char *vs = ven == 0x1002 ? "AMD" : ven == 0x10de ? "NVIDIA" : ven == 0x8086 ? "Intel"
		                 : ven == 0x1af4 ? "Virtio" : ven == 0x15ad ? "VMware" : ven == 0x1234 ? "QEMU"
		                 : ven == 0x1b36 ? "QEMU" : ven == 0x106b ? "Apple" : ven ? "Unknown" : "";
		snprintf(g->vendor, sizeof g->vendor, "%s", vs);
		char vn[96], dn[96];
		pci_lookup(ven, dev, vn, sizeof vn, dn, sizeof dn);
		bracket(dn);
		if (dn[0])
			snprintf(g->name, sizeof g->name, "%s %s", vs, dn);
		else if (g->driver[0]) {
			char drv[sizeof g->driver];
			memcpy(drv, g->driver, sizeof drv);
			snprintf(g->name, sizeof g->name, "%s graphics (%s)", vs[0] ? vs : "Display", drv);
		}
		else
			snprintf(g->name, sizeof g->name, "Display adapter");
		snprintf(buf, sizeof buf, "%s/gpu_busy_percent", g->path);
		g->has_busy = access(buf, R_OK) == 0;
		snprintf(buf, sizeof buf, "%s/mem_info_vram_total", g->path);
		g->has_vram = access(buf, R_OK) == 0;
		g->temp_c = -1;
		g->power_w = -1;
		s->ngpu++;
	}
	closedir(d);
}

static void gpu_sample(struct sys *s)
{
	for (int i = 0; i < s->ngpu; i++) {
		struct gpu *g = &s->gpu[i];
		char p[200], t[48];
		if (g->has_busy) {
			snprintf(p, sizeof p, "%s/gpu_busy_percent", g->path);
			if (slurp(p, t, sizeof t) > 0)
				g->busy = atof(t);
		}
		if (g->has_vram) {
			g->vram_total = (u64)read_ll("%s/%s", g->path, "mem_info_vram_total", 0);
			g->vram_used = (u64)read_ll("%s/%s", g->path, "mem_info_vram_used", 0);
			g->gtt_total = (u64)read_ll("%s/%s", g->path, "mem_info_gtt_total", 0);
			g->gtt_used = (u64)read_ll("%s/%s", g->path, "mem_info_gtt_used", 0);
		}
		/* hwmon: temperature and power, found once */
		snprintf(p, sizeof p, "%s/hwmon", g->path);
		DIR *d = opendir(p);
		if (d) {
			struct dirent *e;
			while ((e = readdir(d))) {
				if (strncmp(e->d_name, "hwmon", 5))
					continue;
				char q[320];
				snprintf(q, sizeof q, "%s/%s/temp1_input", p, e->d_name);
				if (slurp(q, t, sizeof t) > 0)
					g->temp_c = (int)(atol(t) / 1000);
				snprintf(q, sizeof q, "%s/%s/power1_average", p, e->d_name);
				if (slurp(q, t, sizeof t) <= 0) {
					snprintf(q, sizeof q, "%s/%s/power1_input", p, e->d_name);
					if (slurp(q, t, sizeof t) <= 0)
						continue;
				}
				g->power_w = atof(t) / 1e6;
			}
			closedir(d);
		}
		hist_push(&g->h_busy, g->busy);
		hist_push(&g->h_vram, g->vram_total ? 100.0 * (double)g->vram_used / (double)g->vram_total : 0);
	}
}

/* ---- init and sampling ----------------------------------------------------------------- */

void sys_init(struct sys *s)
{
	memset(s, 0, sizeof *s);
	char buf[8192];
	int n = slurp("/proc/stat", buf, sizeof buf);
	int cpus = 0;
	if (n > 0)
		for (char *l = buf; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
			if (!strncmp(l, "cpu", 3) && isdigit((unsigned char)l[3]))
				cpus++;
	s->ncpu = cpus > 0 ? (cpus > MAXCPU ? MAXCPU : cpus) : 1;
	cpu_static(s);
	gpu_scan(s);
}

static int skip_disk(const char *n)
{
	static const char *pre[] = {"loop", "ram", "zram", "dm-", "md", "sr", "fd", "nbd", "mtd", NULL};
	for (int i = 0; pre[i]; i++)
		if (!strncmp(n, pre[i], strlen(pre[i])))
			return 1;
	char p[128];
	snprintf(p, sizeof p, "/sys/block/%s", n);
	return access(p, F_OK) != 0;
}

static struct disk *disk_get(struct sys *s, const char *name)
{
	for (int i = 0; i < s->ndisk; i++)
		if (!strcmp(s->disk[i].name, name))
			return &s->disk[i];
	if (s->ndisk >= MAXDISK)
		return NULL;
	struct disk *d = &s->disk[s->ndisk++];
	memset(d, 0, sizeof *d);
	snprintf(d->name, sizeof d->name, "%s", name);
	d->bytes = (u64)read_ll("/sys/block/%s/%s", name, "size", 0) * 512;
	if (read_str(d->model, sizeof d->model, "/sys/block/%s/device/model", name) < 0)
		d->model[0] = 0;
	long long rot = read_ll("/sys/block/%s/queue/%s", name, "rotational", -1);
	if (!strncmp(name, "nvme", 4))
		snprintf(d->type, sizeof d->type, "NVMe SSD");
	else if (!strncmp(name, "mmcblk", 6))
		snprintf(d->type, sizeof d->type, "Flash card");
	else if (rot == 0)
		snprintf(d->type, sizeof d->type, "SSD");
	else if (rot == 1)
		snprintf(d->type, sizeof d->type, "HDD");
	else
		snprintf(d->type, sizeof d->type, "Disk");
	if (!d->model[0] && !strncmp(name, "vd", 2))
		snprintf(d->model, sizeof d->model, "Virtual disk");
	return d;
}

static struct net *net_get(struct sys *s, const char *name)
{
	for (int i = 0; i < s->nnet; i++)
		if (!strcmp(s->net[i].name, name))
			return &s->net[i];
	if (s->nnet >= MAXNET)
		return NULL;
	struct net *n = &s->net[s->nnet++];
	memset(n, 0, sizeof *n);
	snprintf(n->name, sizeof n->name, "%s", name);
	char p[128];
	snprintf(p, sizeof p, "/sys/class/net/%s/wireless", name);
	n->wireless = access(p, F_OK) == 0;
	snprintf(p, sizeof p, "/sys/class/net/%s/phy80211", name);
	if (access(p, F_OK) == 0)
		n->wireless = 1;
	snprintf(n->label, sizeof n->label, "%s", n->wireless ? "Wi-Fi" : "Ethernet");
	return n;
}

static void net_refresh_ips(struct sys *s)
{
	struct ifaddrs *ifa, *p;
	for (int i = 0; i < s->nnet; i++)
		s->net[i].ip4[0] = 0;
	if (getifaddrs(&ifa) < 0)
		return;
	for (p = ifa; p; p = p->ifa_next) {
		if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
			continue;
		for (int i = 0; i < s->nnet; i++)
			if (!strcmp(s->net[i].name, p->ifa_name) && !s->net[i].ip4[0])
				inet_ntop(AF_INET, &((struct sockaddr_in *)p->ifa_addr)->sin_addr, s->net[i].ip4,
				          sizeof s->net[i].ip4);
	}
	freeifaddrs(ifa);
}

static void sample_cpu(struct sys *s)
{
	char buf[16384];
	if (slurp("/proc/stat", buf, sizeof buf) <= 0)
		return;
	for (char *l = buf; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
		if (strncmp(l, "cpu", 3))
			continue;
		int idx;
		char *q;
		if (l[3] == ' ')
			idx = 0;
		else if (isdigit((unsigned char)l[3]))
			idx = 1 + (int)strtol(l + 3, &q, 10);
		else
			continue;
		if (idx > MAXCPU)
			continue;
		const char *c = strchr(l, ' ');
		u64 f[8] = {0};
		sscanf(c, "%lu %lu %lu %lu %lu %lu %lu %lu", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6], &f[7]);
		u64 tot = 0;
		for (int i = 0; i < 8; i++)
			tot += f[i];
		u64 idle = f[3] + f[4];
		double util = 0;
		if (s->primed && tot > s->p_tot[idx]) {
			double dt = (double)(tot - s->p_tot[idx]), di = (double)(idle - s->p_idle[idx]);
			util = 100.0 * (1.0 - di / dt);
			if (util < 0)
				util = 0;
		}
		s->p_tot[idx] = tot;
		s->p_idle[idx] = idle;
		if (idx == 0) {
			s->cpu_total = util;
			if (s->primed)
				hist_push(&s->h_cpu, util);
		} else if (idx - 1 < s->ncpu) {
			s->core[idx - 1] = util;
			if (s->primed)
				hist_push(&s->h_core[idx - 1], util);
		}
	}
	/* speed: mean of the cores' current frequency */
	double sum = 0;
	int k = 0;
	for (int i = 0; i < s->ncpu; i++) {
		char a[16];
		snprintf(a, sizeof a, "%d", i);
		long long khz = read_ll("/sys/devices/system/cpu/cpu%s/cpufreq/%s", a, "scaling_cur_freq", -1);
		if (khz > 0) {
			sum += (double)khz;
			k++;
		}
	}
	s->ghz = k ? sum / k / 1e6 : 0;
	if (s->primed)
		hist_push(&s->h_ghz, s->ghz);
}

static void sample_mem(struct sys *s)
{
	char buf[4096];
	if (slurp("/proc/meminfo", buf, sizeof buf) <= 0)
		return;
	static const struct { const char *k; size_t off; } map[] = {
		{"MemTotal:", offsetof(struct sys, mem_total)}, {"MemFree:", offsetof(struct sys, mem_free)},
		{"MemAvailable:", offsetof(struct sys, mem_avail)}, {"Buffers:", offsetof(struct sys, mem_buffers)},
		{"Cached:", offsetof(struct sys, mem_cached)}, {"Slab:", offsetof(struct sys, mem_slab)},
		{"Shmem:", offsetof(struct sys, mem_shmem)}, {"SwapTotal:", offsetof(struct sys, swap_total)},
		{"SwapFree:", offsetof(struct sys, swap_free)}, {"Committed_AS:", offsetof(struct sys, commit)},
		{"CommitLimit:", offsetof(struct sys, commit_limit)},
	};
	for (char *l = buf; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL)
		for (size_t i = 0; i < sizeof map / sizeof *map; i++)
			if (!strncmp(l, map[i].k, strlen(map[i].k)))
				*(u64 *)((char *)s + map[i].off) = strtoull(l + strlen(map[i].k), NULL, 10);
	if (!s->mem_avail)
		s->mem_avail = s->mem_free + s->mem_cached + s->mem_buffers;
	double used = (double)(s->mem_total - s->mem_avail);
	hist_push(&s->h_mem, s->mem_total ? 100.0 * used / (double)s->mem_total : 0);
}

static void sample_disk(struct sys *s)
{
	char buf[32768];
	if (slurp("/proc/diskstats", buf, sizeof buf) <= 0)
		return;
	double busiest = 0;
	for (char *l = buf; l && *l; l = strchr(l, '\n') ? strchr(l, '\n') + 1 : NULL) {
		unsigned maj, min;
		char name[32];
		u64 f[11] = {0};
		if (sscanf(l, "%u %u %31s %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu", &maj, &min, name, &f[0], &f[1],
		           &f[2], &f[3], &f[4], &f[5], &f[6], &f[7], &f[8], &f[9], &f[10]) < 14 || skip_disk(name))
			continue;
		struct disk *d = disk_get(s, name);
		if (!d)
			continue;
		u64 ios = f[0] + f[4], wt = f[3] + f[7];
		if (d->have_prev && s->dt > 0) {
			d->active = 100.0 * (double)(f[9] - d->p_ticks) / (s->dt * 1000.0);
			if (d->active > 100)
				d->active = 100;
			d->rd_bps = (double)(f[2] - d->p_rsec) * 512.0 / s->dt;
			d->wr_bps = (double)(f[6] - d->p_wsec) * 512.0 / s->dt;
			u64 di = ios - d->p_ios;
			d->resp_ms = di ? (double)(wt - d->p_wt) / (double)di : 0;
		}
		d->p_rsec = f[2];
		d->p_wsec = f[6];
		d->p_ticks = f[9];
		d->p_ios = ios;
		d->p_wt = wt;
		if (d->have_prev) {
			hist_push(&d->h_active, d->active);
			hist_push(&d->h_rd, d->rd_bps);
			hist_push(&d->h_wr, d->wr_bps);
		}
		d->have_prev = 1;
		if (d->active > busiest)
			busiest = d->active;
	}
	s->disk_active = busiest;
}

static int skip_net(const char *n)
{
	static const char *pre[] = {"lo", "veth", "docker", "br-", "virbr", "vnet", "tun", "tap", NULL};
	for (int i = 0; pre[i]; i++)
		if (!strncmp(n, pre[i], strlen(pre[i])) && (strlen(n) == strlen(pre[i]) || strcmp(pre[i], "lo")))
			return 1;
	return 0;
}

static void sample_net(struct sys *s, int tick)
{
	char buf[16384];
	if (slurp("/proc/net/dev", buf, sizeof buf) <= 0)
		return;
	double total = 0;
	for (char *l = strchr(buf, '\n'); l && *(l + 1); l = strchr(l + 1, '\n')) {
		char *line = l + 1;
		char *eol = strchr(line, '\n');
		char *colon = strchr(line, ':');
		if (!colon || (eol && colon > eol))
			continue;
		char name[32];
		snprintf(name, sizeof name, "%.*s", (int)(colon - line), line);
		trim(name);
		if (skip_net(name))
			continue;
		u64 f[16] = {0};
		sscanf(colon + 1, "%lu %lu %lu %lu %lu %lu %lu %lu %lu", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6],
		       &f[7], &f[8]);
		char st[16] = "";
		read_str(st, sizeof st, "/sys/class/net/%s/operstate", name);
		if (!strcmp(st, "down") && f[0] + f[8] == 0)
			continue;
		struct net *n = net_get(s, name);
		if (!n)
			continue;
		snprintf(n->state, sizeof n->state, "%s", st);
		if (n->have_prev && s->dt > 0) {
			n->rx_bps = (double)(f[0] - n->p_rx) * 8.0 / s->dt;
			n->tx_bps = (double)(f[8] - n->p_tx) * 8.0 / s->dt;
		}
		n->rx = f[0];
		n->tx = f[8];
		n->p_rx = f[0];
		n->p_tx = f[8];
		n->speed_mbps = read_ll("/sys/class/net/%s/%s", name, "speed", -1);
		if (n->have_prev) {
			hist_push(&n->h_rx, n->rx_bps);
			hist_push(&n->h_tx, n->tx_bps);
		}
		n->have_prev = 1;
		total += n->rx_bps + n->tx_bps;
	}
	s->net_bps = total;
	if (tick % 10 == 0)
		net_refresh_ips(s);
}

void sys_sample(struct sys *s)
{
	static int tick;
	double t = now_s();
	s->dt = s->t_prev > 0 ? t - s->t_prev : 0;
	s->t_prev = t;
	sample_cpu(s);
	sample_mem(s);
	sample_disk(s);
	sample_net(s, tick++);
	gpu_sample(s);
	char b[128];
	if (slurp("/proc/loadavg", b, sizeof b) > 0) {
		int run, tot = 0;
		sscanf(b, "%lf %lf %lf %d/%d", &s->load[0], &s->load[1], &s->load[2], &run, &tot);
		s->nthreads = tot;
	}
	if (slurp("/proc/uptime", b, sizeof b) > 0)
		s->uptime = atof(b);
	int np = 0;
	DIR *d = opendir("/proc");
	if (d) {
		struct dirent *e;
		while ((e = readdir(d)))
			if (e->d_name[0] >= '1' && e->d_name[0] <= '9')
				np++;
		closedir(d);
	}
	s->nproc = np;
	s->primed = 1;
}
