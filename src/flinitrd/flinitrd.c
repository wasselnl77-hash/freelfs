/* flinitrd -- the init (PID 1) of the FreeLFS live initramfs.
 *
 * It finds the live system and starts it:
 *   case 1  a block device (CD, a dd'd stick, a device-mapper device) that is
 *           ISO 9660 and has /live/root.sfs;
 *   case 2  an ISO file freelfs-live*.iso inside a file system (Ventoy sticks:
 *           exFAT, FAT, ext4, or an ISO 9660 on a partition) at the top or a
 *           few directories down.
 * It loop-mounts root.sfs (squashfs), puts a tmpfs overlay on it and does a
 * switch_root to /sbin/init.
 *
 * It never writes to anything it scans: every block device it touches is
 * first set read-only in the kernel (BLKROSET), and everything is mounted
 * MS_RDONLY (ext4 with noload, so no journal replay) -- the stick holds
 * personal files and the internal disks of the machine are visible.
 *
 * exFAT and FAT sticks are mounted with the owner of the live user (uid 1000, group seat 98) and masks that
 * give that user read and write: those file systems have no owners on disk, the owner is fixed at the first
 * mount, so flmount's later remount-writable would otherwise leave every file root's ("permission denied").
 *
 * Load into RAM: root.sfs is copied into a tmpfs, the system runs from the copy and the stick (or CD) is
 * released completely: unmounted, loop devices detached, BLKROSET cleared. Then it is an ordinary removable
 * drive. Automatic when the machine has 4 GiB more RAM than the image is big, flive.toram=1 forces it (needs
 * the image plus 1 GiB), flive.toram=0 forbids it. A stick with a persistence image is never released
 * automatically (the image is /home); flive.toram=1 wins over it.
 *
 * Kernel command line: flive.timeout=SECONDS (default 20, how long to wait
 * for devices), flive.iso=PATTERN (default freelfs-live*.iso),
 * flive.toram=0|1, flive.debug (say what is tried).
 * SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <linux/loop.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>

#ifndef BLKROSET
#define BLKROSET _IO(0x12, 93)
#endif
#ifndef LOOP_CONFIGURE
#define LOOP_CONFIGURE 0x4C0A
#endif

#define LIVE "/run/live"
#define LIVE_UID "1000" /* tester */
#define LIVE_GID "98"   /* seat */
#define MAX_SEEN 256
#define MAX_DEPTH 3
#define MAX_DIRENTS 20000

static int timeout_s = 20;
static int debug, no_persist, toram = -1; /* toram: -1 automatic, 0 never, 1 always */
static int ram_loaded, persist_present;
static char persist_img[PATH_MAX], persist_dev[272];
static const char *iso_pattern = "freelfs-live*.iso";

static void say(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof buf - 2)
		n = sizeof buf - 2;
	buf[n++] = '\n';
	if (write(2, buf, n) < 0)
		return;
}

#define dbg(...) do { if (debug) say("flinitrd: " __VA_ARGS__); } while (0)

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void msleep(int ms)
{
	struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
	nanosleep(&t, NULL);
}

static void read_cmdline(void)
{
	char buf[4096];
	int fd = open("/proc/cmdline", O_RDONLY);
	ssize_t n = fd < 0 ? 0 : read(fd, buf, sizeof buf - 1);
	if (fd >= 0)
		close(fd);
	if (n <= 0)
		return;
	buf[n] = 0;
	static char pat[256];
	for (char *w = strtok(buf, " \t\n"); w; w = strtok(NULL, " \t\n")) {
		if (!strncmp(w, "flive.timeout=", 14))
			timeout_s = atoi(w + 14);
		else if (!strncmp(w, "flive.iso=", 10)) {
			snprintf(pat, sizeof pat, "%s", w + 10);
			iso_pattern = pat;
		} else if (!strcmp(w, "flive.debug"))
			debug = 1;
		else if (!strcmp(w, "flive.toram=0"))
			toram = 0;
		else if (!strcmp(w, "flive.toram=1") || !strcmp(w, "flive.toram"))
			toram = 1;
		else if (!strcmp(w, "flive.nopersist"))
			no_persist = 1;
	}
}

/* ---- block devices ------------------------------------------------------ */

struct dev {
	char name[256];     /* sda, sda1, dm-0 */
	char path[272];     /* /dev/sda1 */
	int is_part;
	int has_parts;      /* a disk with partitions */
	unsigned long long sectors;
};

static unsigned long long read_ull(const char *path)
{
	char b[64] = "";
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	ssize_t n = read(fd, b, sizeof b - 1);
	close(fd);
	return n > 0 ? strtoull(b, NULL, 10) : 0;
}

static int skip_disk(const char *n)
{
	static const char *const skip[] = {"loop", "ram", "zram", "nbd", "fd", "md", NULL};
	for (int i = 0; skip[i]; i++)
		if (!strncmp(n, skip[i], strlen(skip[i])))
			return 1;
	return 0;
}

static int cmp_dev(const void *a, const void *b)
{
	const struct dev *x = a, *y = b;
	/* disks before partitions of the same disk are fine; keep names sorted */
	return strcmp(x->name, y->name);
}

/* All block devices and partitions the kernel shows right now. */
static int list_devs(struct dev *out, int max)
{
	int n = 0;
	DIR *d = opendir("/sys/block");
	if (!d)
		return 0;
	struct dirent *e;
	while ((e = readdir(d)) && n < max - 1) {
		if (e->d_name[0] == '.' || skip_disk(e->d_name))
			continue;
		char p[PATH_MAX];
		struct dev *disk = &out[n++];
		memset(disk, 0, sizeof *disk);
		snprintf(disk->name, sizeof disk->name, "%s", e->d_name);
		snprintf(disk->path, sizeof disk->path, "/dev/%s", e->d_name);
		snprintf(p, sizeof p, "/sys/block/%s/size", e->d_name);
		disk->sectors = read_ull(p);
		snprintf(p, sizeof p, "/sys/block/%s", e->d_name);
		DIR *pd = opendir(p);
		struct dirent *pe;
		while (pd && (pe = readdir(pd)) && n < max) {
			char q[PATH_MAX];
			snprintf(q, sizeof q, "/sys/block/%s/%s/partition", e->d_name, pe->d_name);
			if (access(q, F_OK))
				continue;
			struct dev *part = &out[n++];
			memset(part, 0, sizeof *part);
			part->is_part = 1;
			snprintf(part->name, sizeof part->name, "%s", pe->d_name);
			snprintf(part->path, sizeof part->path, "/dev/%s", pe->d_name);
			snprintf(q, sizeof q, "/sys/block/%s/%s/size", e->d_name, pe->d_name);
			part->sectors = read_ull(q);
			disk->has_parts = 1;
		}
		if (pd)
			closedir(pd);
	}
	closedir(d);
	qsort(out, n, sizeof *out, cmp_dev);
	return n;
}

/* Make the kernel refuse every write to the device, then it is safe to
 * mount whatever is on it. Returns 0 when it is read-only. */
#define MAX_RO 512
static char ro_devs[MAX_RO][64];
static int n_ro;

static int make_readonly(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	int one = 1, rc = ioctl(fd, BLKROSET, &one);
	close(fd);
	if (rc == 0 && n_ro < MAX_RO)
		snprintf(ro_devs[n_ro++], sizeof ro_devs[0], "%s", path);
	return rc;
}

/* The flag was only for probing: clear it on every device we set it on, so the disks of the machine and
 * the stick are writable again once the live system runs (the file systems we mounted stay read-only until
 * flmount remounts the stick). */
static void clear_readonly(void)
{
	for (int i = 0; i < n_ro; i++) {
		int fd = open(ro_devs[i], O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		int zero = 0;
		(void)!ioctl(fd, BLKROSET, &zero);
		close(fd);
	}
	n_ro = 0;
}

/* ---- mounting ----------------------------------------------------------------- */

static int mounted_tmpfs(const char *dir, const char *opts)
{
	mkdir(dir, 0755);
	return mount("tmpfs", dir, "tmpfs", 0, opts);
}

static int try_mount(const char *src, const char *dir, const char *type)
{
	const char *opts = NULL;
	if (!strcmp(type, "ext4"))
		opts = "noload"; /* no journal replay: that would write */
	else if (!strcmp(type, "vfat"))
		opts = "uid=" LIVE_UID ",gid=" LIVE_GID ",fmask=0133,dmask=0022,iocharset=utf8,shortname=mixed,quiet";
	else if (!strcmp(type, "exfat"))
		opts = "uid=" LIVE_UID ",gid=" LIVE_GID ",fmask=0133,dmask=0022,iocharset=utf8";
	mkdir(dir, 0755);
	int rc = mount(src, dir, type, MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, opts);
	if (rc && !strcmp(type, "vfat"))
		rc = mount(src, dir, type, MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, "uid=" LIVE_UID ",gid=" LIVE_GID ",fmask=0133,dmask=0022,quiet");
	if (rc && !strcmp(type, "exfat"))
		rc = mount(src, dir, type, MS_RDONLY | MS_NOSUID | MS_NODEV | MS_NOEXEC, "uid=" LIVE_UID ",gid=" LIVE_GID ",fmask=0133,dmask=0022");
	dbg("mount %s as %s: %s", src, type, rc ? strerror(errno) : "ok");
	return rc;
}

/* A loop device over FILE, read-only; returns the device path in OUT. */
/* No LO_FLAGS_AUTOCLEAR: the device would be cleared when we close it, before the mount holds it. */
static int loop_attach_ex(const char *file, char *out, size_t outsz, int rw)
{
	int ctl = open("/dev/loop-control", O_RDWR | O_CLOEXEC);
	if (ctl < 0)
		return -1;
	int nr = ioctl(ctl, LOOP_CTL_GET_FREE);
	close(ctl);
	if (nr < 0)
		return -1;
	snprintf(out, outsz, "/dev/loop%d", nr);
	for (int i = 0; i < 50 && access(out, F_OK); i++)
		msleep(20);
	int ffd = open(file, (rw ? O_RDWR : O_RDONLY) | O_CLOEXEC);
	if (ffd < 0)
		return -1;
	int lfd = open(out, (rw ? O_RDWR : O_RDONLY) | O_CLOEXEC);
	if (lfd < 0) {
		close(ffd);
		return -1;
	}
	struct loop_config cfg;
	memset(&cfg, 0, sizeof cfg);
	cfg.fd = ffd;
	cfg.info.lo_flags = rw ? 0 : LO_FLAGS_READ_ONLY;
	snprintf((char *)cfg.info.lo_file_name, LO_NAME_SIZE, "%s", file);
	int rc = ioctl(lfd, LOOP_CONFIGURE, &cfg);
	if (rc && errno == EINVAL) { /* kernels before 5.8: not ours, but cheap */
		struct loop_info64 info;
		memset(&info, 0, sizeof info);
		info.lo_flags = rw ? 0 : LO_FLAGS_READ_ONLY;
		rc = ioctl(lfd, LOOP_SET_FD, ffd);
		if (!rc)
			rc = ioctl(lfd, LOOP_SET_STATUS64, &info);
	}
	close(ffd);
	close(lfd);
	dbg("loop %s over %s: %s", out, file, rc ? strerror(errno) : "ok");
	if (debug && !rc) { /* can the data be read, from the file and through the loop? */
		char b[4096];
		int f = open(file, O_RDONLY), l = open(out, O_RDONLY);
		ssize_t a = f >= 0 ? pread(f, b, sizeof b, 0) : -1, c = l >= 0 ? pread(l, b, sizeof b, 0) : -1;
		dbg("read 4 KiB: file %zd (%s), loop %zd (%s)", a, a < 0 ? strerror(errno) : "ok", c,
		    c < 0 ? strerror(errno) : "ok");
		if (f >= 0)
			close(f);
		if (l >= 0)
			close(l);
	}
	return rc;
}

static int loop_attach(const char *file, char *out, size_t outsz)
{
	return loop_attach_ex(file, out, outsz, 0);
}

/* ---- finding the live system -------------------------------------------- */

static int exists(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static unsigned long long meminfo_kb(const char *key)
{
	char buf[2048];
	int fd = open("/proc/meminfo", O_RDONLY);
	ssize_t n = fd < 0 ? 0 : read(fd, buf, sizeof buf - 1);
	if (fd >= 0)
		close(fd);
	if (n <= 0)
		return 0;
	buf[n] = 0;
	char *p = strstr(buf, key);
	return p ? strtoull(p + strlen(key), NULL, 10) : 0;
}

/* Detach a loop device (the file system on it is unmounted already). */
static void loop_detach(const char *dev)
{
	if (!dev[0])
		return;
	int fd = open(dev, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	int rc = ioctl(fd, LOOP_CLR_FD, 0);
	dbg("detach %s: %s", dev, rc ? strerror(errno) : "ok");
	close(fd);
}

/* Copy SFS into a tmpfs at LIVE/ram; the path of the copy in OUT. 0 when done. */
static int copy_to_ram(const char *sfs, unsigned long long size, char *out, size_t outsz)
{
	char opts[64];
	snprintf(opts, sizeof opts, "size=%llu,mode=0700", size + (32ull << 20));
	if (mounted_tmpfs(LIVE "/ram", opts)) {
		say("flinitrd: Load into RAM: cannot make the tmpfs: %s", strerror(errno));
		return -1;
	}
	snprintf(out, outsz, "%s", LIVE "/ram/root.sfs");
	int in = open(sfs, O_RDONLY | O_CLOEXEC), o = open(out, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	char *buf = malloc(1 << 20);
	int ok = in >= 0 && o >= 0 && buf;
	unsigned long long done = 0;
	int last = -1;
	double t0 = now();
	while (ok && done < size) {
		ssize_t n = read(in, buf, 1 << 20);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			ok = 0;
			break;
		}
		for (ssize_t w = 0; w < n;) {
			ssize_t k = write(o, buf + w, n - w);
			if (k < 0 && errno == EINTR)
				continue;
			if (k <= 0) {
				ok = 0;
				break;
			}
			w += k;
		}
		done += n;
		/* the stick's pages would only fill the RAM the copy needs */
		posix_fadvise(in, 0, 0, POSIX_FADV_DONTNEED);
		int pc = (int)(done * 100 / size);
		if (ok && pc != last && pc % 5 == 0) {
			last = pc;
			say("flinitrd: Load into RAM: %d%% (%llu of %llu MiB)", pc, done >> 20, size >> 20);
		}
	}
	free(buf);
	if (in >= 0)
		close(in);
	if (o >= 0) {
		if (ok)
			fsync(o);
		close(o);
	}
	if (!ok || done < size) {
		say("flinitrd: Load into RAM failed (%s): staying on the stick", errno ? strerror(errno) : "short read");
		unlink(out);
		umount(LIVE "/ram");
		return -1;
	}
	dbg("copied %llu MiB in %.1f s", done >> 20, now() - t0);
	return 0;
}

/* Should this image of SIZE bytes be loaded into RAM? */
static int want_toram(unsigned long long size)
{
	unsigned long long mem = meminfo_kb("MemTotal:") * 1024ull;
	if (toram == 0)
		return 0;
	if (toram == 1) {
		if (mem < size + (1ull << 30)) {
			say("flinitrd: Load into RAM needs %llu MiB of RAM and this machine has %llu MiB: staying on the stick",
			    (size + (1ull << 30)) >> 20, mem >> 20);
			return 0;
		}
		return 1;
	}
	if (persist_present)
		return 0;
	return mem >= size + (4ull << 30);
}

/* DIR is a mounted ISO 9660 (or any file system) with live/root.sfs:
 * loop-mount the squashfs at LIVE/rom, from a copy in RAM when that is wanted (ram_loaded is set then, and
 * the caller releases what it mounted). */
static int use_iso(const char *dir)
{
	char sfs[PATH_MAX], loop[64];
	struct stat st;
	snprintf(sfs, sizeof sfs, "%s/live/root.sfs", dir);
	if (!exists(sfs))
		return -1;
	if (stat(sfs, &st) == 0 && want_toram((unsigned long long)st.st_size)) {
		char copy[PATH_MAX];
		if (copy_to_ram(sfs, (unsigned long long)st.st_size, copy, sizeof copy) == 0) {
			if (loop_attach(copy, loop, sizeof loop) == 0) {
				mkdir(LIVE "/rom", 0755);
				if (mount(loop, LIVE "/rom", "squashfs", MS_RDONLY, NULL) == 0) {
					ram_loaded = 1;
					int mk = open(LIVE "/toram", O_WRONLY | O_CREAT | O_CLOEXEC, 0644); /* for flreport and tests */
					if (mk >= 0)
						close(mk);
					return 0;
				}
				loop_detach(loop);
			}
			unlink(copy);
			umount(LIVE "/ram");
			say("flinitrd: Load into RAM failed: staying on the stick");
		}
	}
	if (loop_attach(sfs, loop, sizeof loop))
		return -1;
	mkdir(LIVE "/rom", 0755);
	if (mount(loop, LIVE "/rom", "squashfs", MS_RDONLY, NULL)) {
		say("flinitrd: cannot mount %s as squashfs: %s", sfs, strerror(errno));
		return -1;
	}
	return 0;
}

/* Case 1: DEV itself is the ISO. */
static int try_device_iso(const struct dev *d)
{
	if (make_readonly(d->path))
		return -1;
	if (try_mount(d->path, LIVE "/iso", "iso9660"))
		return -1;
	if (use_iso(LIVE "/iso") == 0) {
		if (ram_loaded) {
			/* nothing of the device is needed any more: let it go completely */
			umount(LIVE "/iso");
			clear_readonly();
			say("flinitrd: the live system is in RAM; %s is free", d->path);
		} else
			say("flinitrd: live system on the device %s", d->path);
		return 0;
	}
	umount(LIVE "/iso");
	return -1;
}

struct search {
	int found;
	int ents;
	char iso[PATH_MAX];
};

static int skip_dir(const char *n)
{
	return n[0] == '.' || !strcmp(n, "$RECYCLE.BIN") || !strcmp(n, "System Volume Information") ||
	       !strcmp(n, "lost+found") || !strcmp(n, "proc") || !strcmp(n, "sys") || !strcmp(n, "dev");
}

/* Look for a file matching the pattern in DIR, at exactly DEPTH levels
 * below (iterative deepening: the top level first). */
static void walk(const char *dir, int depth, struct search *s)
{
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *e;
	while (!s->found && s->ents < MAX_DIRENTS && (e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		s->ents++;
		char p[PATH_MAX];
		if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p)
			continue;
		int type = e->d_type;
		if (type == DT_UNKNOWN) {
			struct stat st;
			if (stat(p, &st) == 0)
				type = S_ISDIR(st.st_mode) ? DT_DIR : S_ISREG(st.st_mode) ? DT_REG : 0;
		}
		if (depth == 0 && type == DT_REG) {
			if (!fnmatch(iso_pattern, e->d_name, FNM_CASEFOLD)) {
				snprintf(s->iso, sizeof s->iso, "%s", p);
				s->found = 1;
			}
		} else if (depth > 0 && type == DT_DIR && !skip_dir(e->d_name)) {
			walk(p, depth - 1, s);
		}
	}
	closedir(d);
}

/* Case 2: an ISO file inside a file system of DEV. */
static int try_device_files(const struct dev *d)
{
	static const char *const types[] = {"exfat", "vfat", "ext4", "iso9660", NULL};
	if (make_readonly(d->path))
		return -1;
	for (int i = 0; types[i]; i++) {
		if (try_mount(d->path, LIVE "/src", types[i]))
			continue;
		struct search s = {0};
		for (int depth = 0; depth <= MAX_DEPTH && !s.found; depth++) {
			s.ents = 0;
			walk(LIVE "/src", depth, &s);
		}
		if (s.found) {
			dbg("found %s", s.iso);
			char loop[64];
			if (loop_attach(s.iso, loop, sizeof loop) == 0 &&
			    try_mount(loop, LIVE "/iso", "iso9660") == 0) {
				/* Live persistence: freelfs-persist.img at the top of the stick, or beside the ISO */
				char beside[PATH_MAX];
				persist_img[0] = 0;
				snprintf(beside, sizeof beside, "%s", s.iso);
				char *sl = strrchr(beside, '/');
				if (sl) {
					snprintf(sl + 1, sizeof beside - (size_t)(sl + 1 - beside), "freelfs-persist.img");
					if (exists(beside))
						snprintf(persist_img, sizeof persist_img, "%s", beside);
				}
				if (!persist_img[0] && exists(LIVE "/src/freelfs-persist.img"))
					snprintf(persist_img, sizeof persist_img, "%s", LIVE "/src/freelfs-persist.img");
				persist_present = persist_img[0] && !no_persist;
				if (use_iso(LIVE "/iso") == 0) {
					snprintf(persist_dev, sizeof persist_dev, "%s", d->path);
					if (ram_loaded) {
						/* the image is in RAM: release the stick completely, it is an ordinary drive now */
						umount(LIVE "/iso");
						loop_detach(loop);
						if (umount(LIVE "/src"))
							say("flinitrd: cannot release %s: %s", d->path, strerror(errno));
						clear_readonly();
						if (persist_present)
							say("flinitrd: Load into RAM was asked for: the persistent home is not used");
						persist_img[0] = 0;
						say("flinitrd: live system loaded into RAM from %s on %s; the stick is free",
						    s.iso + strlen(LIVE "/src"), d->path);
					} else
						say("flinitrd: live system in %s on %s (%s)", s.iso + strlen(LIVE "/src"),
						    d->path, types[i]);
					return 0;
				}
				umount(LIVE "/iso");
			}
		}
		umount(LIVE "/src");
		/* a file system was found; the other types cannot match */
		return -1;
	}
	return -1;
}

static int find_live(void)
{
	struct dev devs[MAX_SEEN];
	struct { char name[256]; unsigned long long sectors; } seen[MAX_SEEN];
	int nseen = 0;
	double start = now(), deadline = start + timeout_s, last = start;
	for (;;) {
		int n = list_devs(devs, MAX_SEEN);
		for (int i = 0; i < n; i++) {
			struct dev *d = &devs[i];
			int known = 0;
			for (int j = 0; j < nseen; j++)
				if (!strcmp(seen[j].name, d->name) && seen[j].sectors == d->sectors)
					known = 1;
			if (known || d->sectors == 0 || access(d->path, F_OK))
				continue;
			if (nseen < MAX_SEEN) {
				snprintf(seen[nseen].name, sizeof seen[nseen].name, "%s", d->name);
				seen[nseen++].sectors = d->sectors;
			}
			dbg("looking at %s (%llu MiB)", d->path, d->sectors / 2048);
			if (try_device_iso(d) == 0)
				return 0;
			if (!(d->has_parts) && try_device_files(d) == 0)
				return 0;
		}
		if (now() >= deadline)
			return -1;
		if (now() - last >= 5) {
			last = now();
			say("flinitrd: still looking for the live system (%d s left)...", (int)(deadline - now()));
		}
		msleep(250);
	}
}

/* ---- switching to the live system -------------------------------------------- */

static void remove_tree(int dirfd, dev_t dev)
{
	DIR *d = fdopendir(dirfd);
	struct dirent *e;
	if (!d)
		return;
	while ((e = readdir(d))) {
		struct stat st;
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (fstatat(dirfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) || st.st_dev != dev)
			continue;
		if (S_ISDIR(st.st_mode)) {
			int sub = openat(dirfd, e->d_name, O_RDONLY | O_DIRECTORY);
			if (sub >= 0)
				remove_tree(sub, dev);
			unlinkat(dirfd, e->d_name, AT_REMOVEDIR);
		} else
			unlinkat(dirfd, e->d_name, 0);
	}
	closedir(d);
}

static int start_live(void)
{
	if (mounted_tmpfs(LIVE "/rw", "mode=0755")) {
		say("flinitrd: cannot make the tmpfs: %s", strerror(errno));
		return -1;
	}
	mkdir(LIVE "/rw/up", 0755);
	mkdir(LIVE "/rw/work", 0755);
	mkdir("/newroot", 0755);
	if (mount("overlay", "/newroot", "overlay", 0,
		  "lowerdir=" LIVE "/rom,upperdir=" LIVE "/rw/up,workdir=" LIVE "/rw/work")) {
		say("flinitrd: cannot mount the overlay: %s", strerror(errno));
		return -1;
	}
	/* the flag was only for probing (the disk's flag and the partitions' own: a partition stays read-only while
	 * its disk is) */
	clear_readonly();
	if (persist_img[0] && !no_persist) {
		/* the stick's data partition has to be writable for this */
		char loop[64];
		int fd = open(persist_dev, O_RDONLY | O_CLOEXEC);
		if (fd >= 0) {
			int zero = 0;
			(void)!ioctl(fd, BLKROSET, &zero);
			close(fd);
		}
		int ok = 0;
		if (mount(NULL, LIVE "/src", NULL, MS_REMOUNT | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL))
			say("flinitrd: persistence: cannot make the stick writable: %s", strerror(errno));
		else if (loop_attach_ex(persist_img, loop, sizeof loop, 1))
			say("flinitrd: persistence: cannot open %s: %s", persist_img, strerror(errno));
		else {
			mkdir(LIVE "/persist", 0755);
			static const char *const types[] = { "ext4", "btrfs", NULL };
			for (int i = 0; types[i] && !ok; i++)
				ok = mount(loop, LIVE "/persist", types[i], MS_NOSUID | MS_NODEV, NULL) == 0;
			if (!ok)
				say("flinitrd: persistence: %s is neither ext4 nor btrfs", persist_img);
		}
		if (ok) {
			mkdir("/newroot/home", 0755);
			if (mount(LIVE "/persist", "/newroot/home", NULL, MS_BIND, NULL))
				say("flinitrd: persistence: cannot use it as /home: %s", strerror(errno));
			else
				say("flinitrd: your files are kept: /home is on %s", persist_img + strlen(LIVE "/src"));
		}
	}
	if (access("/newroot/sbin/init", X_OK)) {
		say("flinitrd: the live system has no /sbin/init");
		return -1;
	}
	/* the loop devices stay mounted under /run/live; /run, /dev, /proc and
	 * /sys move along into the new root */
	static const char *const mv[] = {"dev", "proc", "sys", "run"};
	for (int i = 0; i < 4; i++) {
		char to[64];
		snprintf(to, sizeof to, "/newroot/%s", mv[i]);
		mkdir(to, 0755);
		char from[16];
		snprintf(from, sizeof from, "/%s", mv[i]);
		if (mount(from, to, NULL, MS_MOVE, NULL)) {
			say("flinitrd: cannot move %s: %s", from, strerror(errno));
			return -1;
		}
	}
	struct stat st;
	stat("/", &st);
	int oldroot = open("/", O_RDONLY | O_DIRECTORY);
	if (chdir("/newroot") || mount("/newroot", "/", NULL, MS_MOVE, NULL) || chroot(".") || chdir("/")) {
		say("flinitrd: switch_root failed: %s", strerror(errno));
		return -1;
	}
	if (oldroot >= 0)
		remove_tree(oldroot, st.st_dev); /* frees the initramfs */
	int fd = open("/dev/console", O_RDWR);
	if (fd >= 0) {
		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2)
			close(fd);
	}
	execl("/sbin/init", "init", (char *)NULL);
	say("flinitrd: cannot run /sbin/init: %s", strerror(errno));
	for (;;)
		pause();
}

/* ---- when nothing is found -------------------------------------------------- */

static void list_for_user(void)
{
	struct dev devs[MAX_SEEN];
	int n = list_devs(devs, MAX_SEEN);
	if (!n)
		say("  (the kernel shows no disks at all)");
	for (int i = 0; i < n; i++)
		say("  %s%-10s %8llu MiB", devs[i].is_part ? "  " : "", devs[i].name, devs[i].sectors / 2048);
}

static void prompt(void)
{
	say("\nflinitrd: could not find the FreeLFS live system.");
	say("It looks for /live/root.sfs on a CD or a dd'd stick, or for a file named");
	say("%s in a Ventoy (exFAT, FAT or ext4) partition. Nothing was written to any disk.", iso_pattern);
	say("Disks the kernel sees:");
	list_for_user();
	for (;;) {
		say("\nType r + Enter to look again, l to list the disks, p to power off, b to reboot.");
		char line[64] = "";
		struct pollfd p = {0, POLLIN, 0};
		/* wake up now and then so a stick plugged in later is noticed */
		int pr = poll(&p, 1, 3000);
		if (pr > 0) {
			if (!fgets(line, sizeof line, stdin)) {
				clearerr(stdin);
				msleep(1000);
				continue;
			}
		} else {
			/* no keyboard attached to this console: keep trying quietly */
			int saved = timeout_s;
			timeout_s = 1;
			int rc = find_live();
			timeout_s = saved;
			if (rc == 0 && start_live() == 0)
				return;
			continue;
		}
		if (line[0] == 'r') {
			if (find_live() == 0 && start_live() == 0)
				return;
			say("flinitrd: still nothing.");
		} else if (line[0] == 'l')
			list_for_user();
		else if (line[0] == 'p') {
			sync();
			reboot(RB_POWER_OFF);
		} else if (line[0] == 'b') {
			sync();
			reboot(RB_AUTOBOOT);
		}
	}
}

int main(int argc, char **argv)
{
	mkdir("/proc", 0755);
	mkdir("/sys", 0755);
	mkdir("/dev", 0755);
	mkdir("/run", 0755);
	mount("proc", "/proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);
	mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV, NULL);
	mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
	int fd = open("/dev/console", O_RDWR);
	if (fd >= 0) {
		dup2(fd, 0);
		dup2(fd, 1);
		dup2(fd, 2);
		if (fd > 2)
			close(fd);
	}
	read_cmdline();
	say("\nflinitrd: FreeLFS live: looking for the live system (up to %d s)...", timeout_s);
	if (mounted_tmpfs("/run", "mode=0755")) {
		say("flinitrd: cannot mount /run: %s", strerror(errno));
	}
	mkdir(LIVE, 0755);
	if (find_live() == 0 && start_live() == 0)
		return 0;
	prompt();
	for (;;)
		pause();
}
