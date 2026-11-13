/* flinstall-helper -- the privileged half of the FreeLFS installer
 * SPDX-License-Identifier: MIT
 *
 * Talks to the installer window in lines over stdin and stdout (see README).
 * It runs as root on the live system: installed setuid root for the group seat
 * (recipe owner= "/usr/libexec/flinstall-helper 0 98 4750"), like flpower and
 * fltime-set. Everything it is asked is checked again here:
 *
 *   - a real installation needs euid 0, the live system (/run/live/rom is a
 *     squashfs, / an overlay) and a whole disk that /sys lists as a disk that
 *     is not the live medium, not read-only and has nothing mounted;
 *   - a name the window sends is never a path: disks are picked from the list
 *     this program made itself;
 *   - the test hooks (--image, --source, --kernel, --iwd, --workdir,
 *     FLINSTALL_FAKE) are refused when the program runs with more privilege than
 *     the person who started it (setuid). --image writes only a regular file, never
 *     a device.
 *   --dry-run runs the questions (sfdisk -d, blkid -p, /sys, /proc: read only) and
 *   prints every command and file edit it would do, as CMD lines; nothing is
 *   written, mounted or formatted.
 *
 * The system is not copied file by file onto a mounted disk. As in
 * build/50-image.sh, an overlay of the live squashfs with the few changes (the
 * first user instead of "tester", fstab, keyboard, zone, ...) is assembled in a
 * private mount namespace and mkfs.btrfs --rootdir fills the new file system
 * with the subvolumes @, @home and @snapshots in one go. The EFI system partition
 * is filled with mtools from a staged directory that flboot-install made.
 */
#include <crypt.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/random.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

/* ---- state --------------------------------------------------------------------------------- */

int rpath(const char *root, const char *p, char *outp, size_t cap);

static int dry;            /* --dry-run */
static int elevated;       /* euid != uid: setuid; hooks are off */
static char image_path[PATH_MAX]; /* --image: a regular file instead of a disk */
static char source_root[PATH_MAX] = "/run/live/rom";
static char kernel_arg[PATH_MAX];
static char iwd_arg[PATH_MAX];
static char work_arg[PATH_MAX];
static char fake_dir[PATH_MAX];
static int real_mode;      /* writes to a real disk */

#define MAXDISK 16
#define MAXPART 128
#define MAXOP 64

struct pt {
	char dev[64];
	int num;
	long long start, size; /* bytes */
	char type[40], uuid[40], name[72];
	char fs[24], label[40];
	char kind[8]; /* esp swap linux other */
	int busy;
};

struct disk {
	char dev[256], name[32];
	long long size; /* bytes */
	int ssz;        /* logical sector size */
	int removable, live, busy, ro;
	char model[64];
	char label[8]; /* gpt, dos or "" */
	long long first, last_end; /* usable range in bytes (first usable, end of last usable) */
	struct pt p[MAXPART];
	int np;
};

static struct disk disks[MAXDISK];
static int ndisks;

/* the configuration the window sends */
static struct {
	char mode[8];        /* whole free manual */
	char disk[256];
	char user[40], fullname[80], password[FI_PW_MAX + 4];
	char lang[40], keyboard[8], tz[56];
} cfg;

enum { OP_DELETE, OP_NEWTABLE, OP_CREATE, OP_FORMAT, OP_MOUNT };
struct op {
	int k;
	char a[64];       /* device or new id */
	long long start, size;
	char kind[8];
	char fs[16], label[40], mp[64];
};
static struct op ops[MAXOP];
static int nops;

/* ---- output ----------------------------------------------------------------------------------- */

static void out(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
}

static void clean(char *s)
{
	for (; *s; s++)
		if (*s == '\t' || *s == '\n' || *s == '\r')
			*s = ' ';
}

static void err(const char *fmt, ...)
{
	char b[400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	clean(b);
	out("ERR\t%s", b);
}

static void cmdf(const char *fmt, ...)
{
	char b[1200];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(b, sizeof b, fmt, ap);
	va_end(ap);
	clean(b);
	out("CMD\t%s", b);
}

static void say_argv(char *const argv[])
{
	char b[1200] = "";
	size_t n = 0;
	for (int i = 0; argv[i] && n < sizeof b - 80; i++) {
		const char *a = argv[i];
		int plain = *a != 0;
		for (const char *c = a; *c; c++)
			if (!isalnum((unsigned char)*c) && !strchr("/._-=:,+@%", *c))
				plain = 0;
		n += (size_t)snprintf(b + n, sizeof b - n, plain ? "%s%s" : "%s'%s'", i ? " " : "", a);
	}
	cmdf("%s", b);
}

static void logline(const char *s)
{
	char b[300];
	snprintf(b, sizeof b, "%s", s);
	clean(b);
	out("LOG\t%s", b);
}

/* ---- running programs ---------------------------------------------------------------------- */

/* Run argv (PATH lookup); input goes to its stdin. Prints the command; dry: that is all. */
static int run(char *const argv[], const char *input)
{
	say_argv(argv);
	if (input) {
		char *copy = strdup(input), *sv = NULL;
		for (char *l = strtok_r(copy, "\n", &sv); l; l = strtok_r(NULL, "\n", &sv))
			cmdf("    | %s", l);
		free(copy);
	}
	if (dry)
		return 0;
	int in[2] = {-1, -1}, op[2];
	if (pipe(op) < 0 || (input && pipe(in) < 0))
		return -1;
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		dup2(op[1], 1);
		dup2(op[1], 2);
		close(op[0]);
		close(op[1]);
		if (input) {
			dup2(in[0], 0);
			close(in[0]);
			close(in[1]);
		} else {
			int n = open("/dev/null", O_RDONLY);
			dup2(n, 0);
		}
		execvp(argv[0], argv);
		fprintf(stderr, "cannot run %s: %s\n", argv[0], strerror(errno));
		_exit(127);
	}
	close(op[1]);
	if (input) {
		close(in[0]);
		size_t len = strlen(input), off = 0;
		signal(SIGPIPE, SIG_IGN);
		while (off < len) {
			ssize_t w = write(in[1], input + off, len - off);
			if (w <= 0)
				break;
			off += (size_t)w;
		}
		close(in[1]);
	}
	FILE *f = fdopen(op[0], "r");
	char line[400];
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0])
			logline(line);
	}
	fclose(f);
	int st = 0;
	waitpid(pid, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* A question: runs even in a dry run (it only reads). Output to buf; returns the exit status or -1. */
static int ask(char *const argv[], char *buf, size_t cap)
{
	int p[2];
	if (pipe(p) < 0)
		return -1;
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		dup2(p[1], 1);
		int n = open("/dev/null", O_RDWR);
		dup2(n, 2);
		dup2(n, 0);
		close(p[0]);
		close(p[1]);
		execvp(argv[0], argv);
		_exit(127);
	}
	close(p[1]);
	size_t len = 0;
	ssize_t r;
	while ((r = read(p[0], buf + len, cap - 1 - len)) > 0 && len + (size_t)r < cap - 1)
		len += (size_t)r;
	if (r > 0)
		len += (size_t)r;
	buf[len] = 0;
	close(p[0]);
	int st = 0;
	waitpid(pid, &st, 0);
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int tool_exists(const char *name)
{
	const char *path = getenv("PATH");
	char dirs[1024], full[1100];
	snprintf(dirs, sizeof dirs, "%s", path ? path : "");
	for (char *d = strtok(dirs, ":"); d; d = strtok(NULL, ":")) {
		snprintf(full, sizeof full, "%s/%s", d, name);
		if (access(full, X_OK) == 0)
			return 1;
	}
	return 0;
}

/* ---- small helpers --------------------------------------------------------------------------- */

static int read_file(const char *path, char *buf, size_t cap)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	size_t n = fread(buf, 1, cap - 1, f);
	fclose(f);
	buf[n] = 0;
	return (int)n;
}

static void read_line(const char *path, char *buf, size_t cap)
{
	buf[0] = 0;
	if (read_file(path, buf, cap) >= 0)
		buf[strcspn(buf, "\r\n")] = 0;
}

static void uuid4(char *out_, size_t cap)
{
	unsigned char b[16];
	if (getrandom(b, sizeof b, 0) != (ssize_t)sizeof b)
		memset(b, 0x5a, sizeof b);
	b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
	b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
	snprintf(out_, cap, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3],
	         b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static void human(long long b, char *o, size_t cap)
{
	if (b >= FI_GIB)
		snprintf(o, cap, "%.1f GiB", (double)b / FI_GIB);
	else
		snprintf(o, cap, "%lld MiB", b / FI_MIB);
}

static int ieq(const char *a, const char *b)
{
	return strcasecmp(a, b) == 0;
}

/* ---- finding the disks ------------------------------------------------------------------------------ */

#define T_ESP "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
#define T_SWAP "0657FD6D-A4AB-43C4-84E5-0933C84B4F4F"
#define T_LINUX "0FC63DAF-8483-4772-8E79-3D69D8477DE4"
#define T_ROOT_X86 "4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709"
#define T_ROOT_ARM "B921B045-1DF0-41C3-AF44-4C6F280D3FAE"

static const char *root_type(void)
{
	static char m[32];
	if (!m[0]) {
		FILE *f = popen("uname -m", "r");
		if (f) {
			if (!fgets(m, sizeof m, f))
				m[0] = 0;
			pclose(f);
		}
		m[strcspn(m, "\r\n")] = 0;
		if (!m[0])
			strcpy(m, "x86_64");
	}
	return !strcmp(m, "aarch64") ? T_ROOT_ARM : T_ROOT_X86;
}

static void part_kind(struct pt *p)
{
	if (ieq(p->type, T_ESP))
		strcpy(p->kind, "esp");
	else if (ieq(p->type, T_SWAP))
		strcpy(p->kind, "swap");
	else if (ieq(p->type, T_LINUX) || ieq(p->type, T_ROOT_X86) || ieq(p->type, T_ROOT_ARM))
		strcpy(p->kind, "linux");
	else
		strcpy(p->kind, "other");
}

static int num_of(const char *dev)
{
	size_t n = strlen(dev);
	while (n > 0 && isdigit((unsigned char)dev[n - 1]))
		n--;
	return atoi(dev + n);
}

/* sfdisk -d output into d */
static void parse_dump(struct disk *d, char *txt)
{
	d->np = 0;
	d->label[0] = 0;
	d->first = d->last_end = 0;
	long long first_lba = -1, last_lba = -1;
	d->ssz = 512;
	char *sv = NULL;
	for (char *l = strtok_r(txt, "\n", &sv); l; l = strtok_r(NULL, "\n", &sv)) {
		if (!strncmp(l, "label:", 6)) {
			char *v = l + 6;
			while (*v == ' ')
				v++;
			snprintf(d->label, sizeof d->label, "%s", v);
		} else if (!strncmp(l, "first-lba:", 10))
			first_lba = atoll(l + 10);
		else if (!strncmp(l, "last-lba:", 9))
			last_lba = atoll(l + 9);
		else if (!strncmp(l, "sector-size:", 12))
			d->ssz = atoi(l + 12) > 0 ? atoi(l + 12) : 512;
		else if (l[0] == '/' && d->np < MAXPART) {
			struct pt *p = &d->p[d->np];
			memset(p, 0, sizeof *p);
			char *c = strstr(l, " : ");
			if (!c)
				continue;
			*c = 0;
			snprintf(p->dev, sizeof p->dev, "%s", l);
			p->num = num_of(p->dev);
			char *s;
			if ((s = strstr(c + 3, "start=")))
				p->start = atoll(s + 6);
			if ((s = strstr(c + 3, "size=")))
				p->size = atoll(s + 5);
			if ((s = strstr(c + 3, "type=")))
				sscanf(s + 5, "%39[^, \n]", p->type);
			if ((s = strstr(c + 3, "uuid=")))
				sscanf(s + 5, "%39[^, \n]", p->uuid);
			if ((s = strstr(c + 3, "name=\""))) {
				snprintf(p->name, sizeof p->name, "%s", s + 6);
				p->name[strcspn(p->name, "\"")] = 0;
			}
			p->start *= d->ssz;
			p->size *= d->ssz;
			part_kind(p);
			if (p->size > 0)
				d->np++;
		}
	}
	if (!strcmp(d->label, "gpt")) {
		d->first = (first_lba >= 0 ? first_lba : 34) * d->ssz;
		d->last_end = (last_lba >= 0 ? last_lba + 1 : d->size / d->ssz - 33) * d->ssz;
	} else {
		d->first = 1 * FI_MIB;
		d->last_end = d->size;
	}
}

static void mark_fs(struct disk *d)
{
	for (int i = 0; i < d->np; i++) {
		struct pt *p = &d->p[i];
		char b[1024];
		if (fake_dir[0]) {
			char path[PATH_MAX];
			snprintf(path, sizeof path, "%s/%s.fs", fake_dir, d->name);
			FILE *f = fopen(path, "r");
			char line[256];
			while (f && fgets(line, sizeof line, f)) {
				char dev[64], fs[24], label[40] = "";
				if (sscanf(line, "%63s %23s %39[^\n]", dev, fs, label) >= 2 && !strcmp(dev, p->dev)) {
					snprintf(p->fs, sizeof p->fs, "%s", fs);
					snprintf(p->label, sizeof p->label, "%s", strcmp(label, "-") ? label : "");
				}
			}
			if (f)
				fclose(f);
			continue;
		}
		char off[32], sz[32];
		snprintf(off, sizeof off, "%lld", p->start);
		snprintf(sz, sizeof sz, "%lld", p->size);
		char *argv_img[] = {"blkid", "-p", "-o", "export", "-O", off, "-S", sz, image_path, NULL};
		char *argv_dev[] = {"blkid", "-p", "-o", "export", p->dev, NULL};
		if (ask(image_path[0] ? argv_img : argv_dev, b, sizeof b) == 0) {
			char *sv = NULL;
			for (char *l = strtok_r(b, "\n", &sv); l; l = strtok_r(NULL, "\n", &sv)) {
				if (!strncmp(l, "TYPE=", 5))
					snprintf(p->fs, sizeof p->fs, "%s", l + 5);
				else if (!strncmp(l, "LABEL=", 6))
					snprintf(p->label, sizeof p->label, "%s", l + 6);
			}
		}
	}
}

/* something sits on the device: an unlocked LUKS, an LVM or md set (/sys/class/block/NAME/holders) */
static int has_holders(const char *dev)
{
	const char *n = strrchr(dev, '/');
	n = n ? n + 1 : dev;
	char path[PATH_MAX];
	snprintf(path, sizeof path, "/sys/class/block/%s/holders", n);
	DIR *d = opendir(path);
	if (!d)
		return 0;
	int r = 0;
	struct dirent *e;
	while ((e = readdir(d)))
		if (e->d_name[0] != '.')
			r = 1;
	closedir(d);
	return r;
}

static int mounted_source(const char *dev)
{
	if (has_holders(dev))
		return 1;
	FILE *f = fopen("/proc/mounts", "r");
	char line[512];
	int r = 0;
	while (f && fgets(line, sizeof line, f)) {
		char src[256];
		if (sscanf(line, "%255s", src) == 1 && !strcmp(src, dev))
			r = 1;
	}
	if (f)
		fclose(f);
	f = fopen("/proc/swaps", "r");
	while (f && fgets(line, sizeof line, f)) {
		char src[256];
		if (sscanf(line, "%255s", src) == 1 && !strcmp(src, dev))
			r = 1;
	}
	if (f)
		fclose(f);
	return r;
}

/* the whole disk a device name (sdb, sdb1) belongs to */
static void parent_disk(const char *dev, char *out_, size_t cap)
{
	const char *n = strrchr(dev, '/');
	n = n ? n + 1 : dev;
	char link[PATH_MAX], tgt[PATH_MAX];
	snprintf(link, sizeof link, "/sys/class/block/%s", n);
	ssize_t r = readlink(link, tgt, sizeof tgt - 1);
	snprintf(out_, cap, "%s", n);
	if (r <= 0)
		return;
	tgt[r] = 0;
	char test[PATH_MAX];
	snprintf(test, sizeof test, "%s/partition", link);
	if (access(test, F_OK) == 0) {
		char *slash = strrchr(tgt, '/');
		if (slash) {
			*slash = 0;
			slash = strrchr(tgt, '/');
			snprintf(out_, cap, "%s", slash ? slash + 1 : tgt);
		}
	}
}

static void live_disk_names(char names[][32], int *n)
{
	*n = 0;
	FILE *f = fopen("/proc/mounts", "r");
	char line[512];
	while (f && fgets(line, sizeof line, f) && *n < 8) {
		char src[256], mp[256];
		if (sscanf(line, "%255s %255s", src, mp) != 2)
			continue;
		if (strncmp(mp, "/run/live", 9) || strncmp(src, "/dev/", 5) || !strncmp(src, "/dev/loop", 9))
			continue;
		parent_disk(src, names[*n], 32);
		(*n)++;
	}
	if (f)
		fclose(f);
}

static void add_disk(const char *name, long long size, int removable, const char *model, int ssz_hint)
{
	if (ndisks >= MAXDISK)
		return;
	struct disk *d = &disks[ndisks++];
	memset(d, 0, sizeof *d);
	snprintf(d->name, sizeof d->name, "%s", name);
	if (image_path[0])
		snprintf(d->dev, sizeof d->dev, "%s", image_path);
	else
		snprintf(d->dev, sizeof d->dev, "/dev/%s", name);
	d->size = size;
	d->removable = removable;
	d->ssz = ssz_hint > 0 ? ssz_hint : 512;
	snprintf(d->model, sizeof d->model, "%s", model);
	char *p = d->model + strlen(d->model);
	while (p > d->model && p[-1] == ' ')
		*--p = 0;
}

static void load_dump_for(struct disk *d)
{
	char buf[65536];
	buf[0] = 0;
	if (fake_dir[0]) {
		char path[PATH_MAX];
		snprintf(path, sizeof path, "%s/%s.dump", fake_dir, d->name);
		if (read_file(path, buf, sizeof buf) < 0)
			buf[0] = 0;
	} else {
		char *argv[] = {"sfdisk", "-d", d->dev, NULL};
		if (ask(argv, buf, sizeof buf) != 0)
			buf[0] = 0;
	}
	parse_dump(d, buf);
	if (!d->label[0]) {
		d->np = 0;
		d->first = 1 * FI_MIB;
		d->last_end = d->size;
	}
	mark_fs(d);
	for (int i = 0; i < d->np; i++) {
		if (fake_dir[0]) {
			d->p[i].busy = 0;
			continue;
		}
		d->p[i].busy = mounted_source(d->p[i].dev);
		d->busy |= d->p[i].busy;
	}
	if (!fake_dir[0])
		d->busy |= has_holders(d->dev);	/* LUKS/LVM/md on the whole disk */
}

static void load_disks(void)
{
	ndisks = 0;
	if (image_path[0]) {
		struct stat st;
		if (stat(image_path, &st) == 0 && S_ISREG(st.st_mode))
			add_disk(strrchr(image_path, '/') ? strrchr(image_path, '/') + 1 : image_path, st.st_size, 0, "disk image file", 512);
	} else if (fake_dir[0]) {
		char path[PATH_MAX], line[512];
		snprintf(path, sizeof path, "%s/disks.txt", fake_dir);
		FILE *f = fopen(path, "r");
		while (f && fgets(line, sizeof line, f)) {
			char name[32], model[64] = "", flags[64] = "-";
			long long size;
			int rm;
			line[strcspn(line, "\r\n")] = 0;
			if (sscanf(line, "%31[^\t]\t%lld\t%d\t%63[^\t]\t%63s", name, &size, &rm, model, flags) >= 3 && name[0] != '#') {
				add_disk(name, size, rm, model, 512);
				disks[ndisks - 1].live = strstr(flags, "live") != NULL;
				disks[ndisks - 1].ro = strstr(flags, "ro") != NULL;
			}
		}
		if (f)
			fclose(f);
	} else {
		DIR *dir = opendir("/sys/block");
		struct dirent *e;
		char live[8][32];
		int nlive;
		live_disk_names(live, &nlive);
		while (dir && (e = readdir(dir))) {
			const char *n = e->d_name;
			if (n[0] == '.' || !strncmp(n, "loop", 4) || !strncmp(n, "ram", 3) || !strncmp(n, "zram", 4) ||
			    !strncmp(n, "sr", 2) || !strncmp(n, "fd", 2) || !strncmp(n, "md", 2) || !strncmp(n, "dm-", 3) ||
			    !strncmp(n, "nbd", 3))
				continue;
			char p[PATH_MAX], b[128];
			snprintf(p, sizeof p, "/sys/block/%s/size", n);
			read_line(p, b, sizeof b);
			long long sectors = atoll(b);
			if (sectors < 2048)
				continue;
			snprintf(p, sizeof p, "/sys/block/%s/removable", n);
			read_line(p, b, sizeof b);
			int rm = atoi(b);
			char model[64];
			snprintf(p, sizeof p, "/sys/block/%s/device/model", n);
			read_line(p, model, sizeof model);
			if (!model[0] && !strncmp(n, "nvme", 4)) {
				snprintf(p, sizeof p, "/sys/block/%s/device/model", n);
				read_line(p, model, sizeof model);
			}
			if (!model[0])
				snprintf(model, sizeof model, "Disk");
			snprintf(p, sizeof p, "/sys/block/%s/queue/logical_block_size", n);
			read_line(p, b, sizeof b);
			add_disk(n, sectors * 512, rm, model, atoi(b));
			struct disk *d = &disks[ndisks - 1];
			snprintf(p, sizeof p, "/sys/block/%s/ro", n);
			read_line(p, b, sizeof b);
			d->ro = atoi(b);
			for (int i = 0; i < nlive; i++)
				if (!strcmp(live[i], n))
					d->live = 1;
		}
		if (dir)
			closedir(dir);
	}
	for (int i = 0; i < ndisks; i++)
		load_dump_for(&disks[i]);
	/* a plain-name sort keeps the list steady */
	for (int i = 0; i < ndisks; i++)
		for (int j = i + 1; j < ndisks; j++)
			if (strcmp(disks[j].name, disks[i].name) < 0) {
				struct disk t = disks[i];
				disks[i] = disks[j];
				disks[j] = t;
			}
}

static struct disk *find_disk(const char *dev)
{
	for (int i = 0; i < ndisks; i++)
		if (!strcmp(disks[i].dev, dev))
			return &disks[i];
	return NULL;
}

/* free gaps of the disk (aligned to 1 MiB, at least 1 MiB), partitions sorted by start */
struct gap {
	long long start, size;
};

static int cmp_pt(const void *a, const void *b)
{
	const struct pt *x = a, *y = b;
	return x->start < y->start ? -1 : x->start > y->start;
}

static int gaps_of(const struct pt *parts, int np, long long first, long long end, struct gap *g, int max)
{
	struct pt tmp[MAXPART];
	memcpy(tmp, parts, (size_t)np * sizeof *tmp);
	qsort(tmp, (size_t)np, sizeof *tmp, cmp_pt);
	long long pos = (first + FI_MIB - 1) / FI_MIB * FI_MIB;
	end = end / FI_MIB * FI_MIB;
	int n = 0;
	for (int i = 0; i <= np; i++) {
		long long to = i < np ? tmp[i].start / FI_MIB * FI_MIB : end;
		if (to - pos >= FI_MIB && n < max) {
			g[n].start = pos;
			g[n].size = to - pos;
			n++;
		}
		if (i < np) {
			long long e = (tmp[i].start + tmp[i].size + FI_MIB - 1) / FI_MIB * FI_MIB;
			if (e > pos)
				pos = e;
		}
	}
	return n;
}

static void cmd_disks(void)
{
	load_disks();
	for (int i = 0; i < ndisks; i++) {
		struct disk *d = &disks[i];
		char flags[64] = "";
		if (d->live)
			strcat(flags, "live,");
		if (d->busy)
			strcat(flags, "busy,");
		if (d->ro)
			strcat(flags, "ro,");
		strcat(flags, d->label[0] ? d->label : "empty");
		out("DISK\t%s\t%lld\t%d\t%s\t%s", d->dev, d->size, d->removable, flags, d->model);
		struct gap g[MAXPART + 1];
		int ng = gaps_of(d->p, d->np, d->first, d->last_end, g, MAXPART);
		struct pt tmp[MAXPART];
		memcpy(tmp, d->p, (size_t)d->np * sizeof *tmp);
		qsort(tmp, (size_t)d->np, sizeof *tmp, cmp_pt);
		int gi = 0;
		for (int k = 0; k <= d->np; k++) {
			long long at = k < d->np ? tmp[k].start : d->last_end;
			while (gi < ng && g[gi].start < at) {
				out("FREE\t%lld\t%lld", g[gi].start, g[gi].size);
				gi++;
			}
			if (k < d->np) {
				struct pt *p = &tmp[k];
				out("PART\t%s\t%lld\t%lld\t%s\t%s\t%s\t%s\t%d", p->dev, p->start, p->size, p->kind,
				    p->fs[0] ? p->fs : "-", p->label[0] ? p->label : (p->name[0] ? p->name : "-"),
				    p->uuid[0] ? p->uuid : "-", p->busy);
			}
		}
	}
	out("END");
}

/* ---- the plan ---------------------------------------------------------------------------------- */

struct fp {
	char key[64];      /* existing device, or new1 ... */
	int is_new, num;
	long long start, size;
	char kind[8], type[40], uuid[40], name[40];
	int fmt;
	char fs[16], label[40], mp[64];
	char dev[96];      /* the node to use, once it exists */
	char fsnow[24];    /* what it holds now (existing partitions) */
};

static struct {
	struct disk *d;
	int newtable;
	struct fp f[MAXPART];
	int nf;
	int ndel, del_num[MAXPART];
	char del_dev[MAXPART][64];
	int root, boot;     /* indexes into f */
	char btrfs_uuid[40];
	char volid[16];
	char errbuf[300];
} plan;

static int fp_find(const char *key)
{
	for (int i = 0; i < plan.nf; i++)
		if (!strcmp(plan.f[i].key, key))
			return i;
	return -1;
}

static const char *kind_type(const char *kind)
{
	if (!strcmp(kind, "esp"))
		return T_ESP;
	if (!strcmp(kind, "root"))
		return root_type();
	if (!strcmp(kind, "swap"))
		return T_SWAP;
	return T_LINUX;
}

static const char *kind_name(const char *kind)
{
	if (!strcmp(kind, "esp"))
		return "EFI system partition";
	if (!strcmp(kind, "root"))
		return "FreeLFS";
	if (!strcmp(kind, "swap"))
		return "swap";
	return "data";
}

static int perr(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(plan.errbuf, sizeof plan.errbuf, fmt, ap);
	va_end(ap);
	return -1;
}

static int overlaps(long long s, long long z)
{
	for (int i = 0; i < plan.nf; i++)
		if (s < plan.f[i].start + plan.f[i].size && plan.f[i].start < s + z)
			return 1;
	return 0;
}

static int add_new(const char *kind, long long start, long long size)
{
	struct disk *d = plan.d;
	if (plan.nf >= MAXPART)
		return perr("Too many partitions.");
	if (start % FI_MIB || size % FI_MIB || size < FI_MIB)
		return perr("Partitions start and end on whole MiB.");
	if (start < (d->first + FI_MIB - 1) / FI_MIB * FI_MIB || start + size > d->last_end / FI_MIB * FI_MIB)
		return perr("A new partition lies outside the disk.");
	if (overlaps(start, size))
		return perr("A new partition would overlap another one.");
	int next = 1;
	for (int i = 0; i < plan.nf; i++)
		if (plan.f[i].is_new)
			next++;
	struct fp *f = &plan.f[plan.nf++];
	memset(f, 0, sizeof *f);
	snprintf(f->key, sizeof f->key, "new%d", next);
	f->is_new = 1;
	f->start = start;
	f->size = size;
	snprintf(f->kind, sizeof f->kind, "%s", kind);
	snprintf(f->type, sizeof f->type, "%s", kind_type(kind));
	snprintf(f->name, sizeof f->name, "%s", kind_name(kind));
	uuid4(f->uuid, sizeof f->uuid);
	f->fmt = 1;
	return plan.nf - 1;
}

/* the ops of the whole-disk and free-space modes */
static int make_auto_ops(void)
{
	struct disk *d = find_disk(cfg.disk);
	if (!d)
		return perr("No such disk.");
	nops = 0;
	long long start, avail;
	int whole = !strcmp(cfg.mode, "whole") || !d->label[0];
	if (whole) {
		start = 1 * FI_MIB;
		long long end = (d->size - 34 * d->ssz) / FI_MIB * FI_MIB;
		avail = end - start;
		ops[nops++] = (struct op){.k = OP_NEWTABLE};
	} else {
		if (strcmp(d->label, "gpt"))
			return perr("This disk has an old (MBR) partition table. Use the whole disk or edit the partitions by hand.");
		struct gap g[MAXPART + 1];
		int ng = gaps_of(d->p, d->np, d->first, d->last_end, g, MAXPART);
		int best = -1;
		for (int i = 0; i < ng; i++)
			if (best < 0 || g[i].size > g[best].size)
				best = i;
		if (best < 0 || g[best].size < FI_ESP_BYTES + FI_MIN_ROOT_BYTES) {
			char a[32], b[32];
			human(best >= 0 ? g[best].size : 0, a, sizeof a);
			human(FI_ESP_BYTES + FI_MIN_ROOT_BYTES, b, sizeof b);
			return perr("The largest free space is %s; FreeLFS needs at least %s in one piece.", a, b);
		}
		start = g[best].start;
		avail = g[best].size;
	}
	if (avail < FI_ESP_BYTES + FI_MIN_ROOT_BYTES) {
		char a[32], b[32];
		human(avail, a, sizeof a);
		human(FI_ESP_BYTES + FI_MIN_ROOT_BYTES, b, sizeof b);
		return perr("The disk is %s; FreeLFS needs at least %s.", a, b);
	}
	ops[nops++] = (struct op){.k = OP_CREATE, .start = start, .size = FI_ESP_BYTES, .kind = "esp"};
	ops[nops++] = (struct op){.k = OP_CREATE, .start = start + FI_ESP_BYTES, .size = avail - FI_ESP_BYTES, .kind = "root"};
	ops[nops++] = (struct op){.k = OP_FORMAT, .a = "new1", .fs = "vfat", .label = "ESP"};
	ops[nops++] = (struct op){.k = OP_MOUNT, .a = "new1", .mp = "/boot"};
	ops[nops++] = (struct op){.k = OP_FORMAT, .a = "new2", .fs = "btrfs", .label = "freelfs"};
	ops[nops++] = (struct op){.k = OP_MOUNT, .a = "new2", .mp = "/"};
	return 0;
}

static int data_mp_ok(const char *mp)
{
	if (strncmp(mp, "/drives/", 8) || !mp[8] || strlen(mp) > 40)
		return 0;
	for (const char *c = mp + 8; *c; c++)
		if (!((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' || *c == '_'))
			return 0;
	return 1;
}

/* Apply the ops to a copy of the disk's table and check the result. 0 or -1 with plan.errbuf. */
static int build_plan(void)
{
	memset(&plan, 0, sizeof plan);
	if (cfg.disk[0] == 0)
		return perr("No disk chosen.");
	load_disks(); /* always fresh: the table may have changed since the window looked */
	struct disk *d = find_disk(cfg.disk);
	if (!d)
		return perr("That disk is not in the list.");
	plan.d = d;
	if (d->live)
		return perr("That disk holds the FreeLFS you are running from.");
	if (d->ro)
		return perr("That disk is read-only.");
	if (d->busy && !image_path[0])
		return perr("Something on that disk is in use (mounted or used as swap).");
	if (!strcmp(cfg.mode, "whole") || !strcmp(cfg.mode, "free")) {
		if (make_auto_ops() < 0)
			return -1;
	} else if (strcmp(cfg.mode, "manual"))
		return perr("Unknown mode.");
	for (int i = 0; i < d->np; i++) {
		struct fp *f = &plan.f[plan.nf++];
		memset(f, 0, sizeof *f);
		snprintf(f->key, sizeof f->key, "%s", d->p[i].dev);
		f->num = d->p[i].num;
		f->start = d->p[i].start;
		f->size = d->p[i].size;
		snprintf(f->kind, sizeof f->kind, "%s", d->p[i].kind);
		snprintf(f->type, sizeof f->type, "%s", d->p[i].type);
		snprintf(f->uuid, sizeof f->uuid, "%s", d->p[i].uuid);
		snprintf(f->dev, sizeof f->dev, "%s", d->p[i].dev);
		snprintf(f->fsnow, sizeof f->fsnow, "%s", d->p[i].fs);
		snprintf(f->label, sizeof f->label, "%s", d->p[i].label);
	}
	for (int i = 0; i < nops; i++) {
		struct op *o = &ops[i];
		int k;
		switch (o->k) {
		case OP_NEWTABLE:
			for (int j = 0; j < plan.nf; j++)
				if (!plan.f[j].is_new && plan.ndel < MAXPART) {
					plan.del_num[plan.ndel] = plan.f[j].num;
					snprintf(plan.del_dev[plan.ndel++], 64, "%s", plan.f[j].key);
				}
			plan.nf = 0;
			plan.newtable = 1;
			break;
		case OP_DELETE:
			k = fp_find(o->a);
			if (k < 0 || plan.f[k].is_new)
				return perr("Cannot delete %s: not a partition of this disk.", o->a);
			for (int j = 0; j < d->np; j++)
				if (!strcmp(d->p[j].dev, o->a) && d->p[j].busy && !image_path[0])
					return perr("%s is in use and cannot be deleted.", o->a);
			plan.del_num[plan.ndel] = plan.f[k].num;
			snprintf(plan.del_dev[plan.ndel++], 64, "%s", o->a);
			memmove(&plan.f[k], &plan.f[k + 1], (size_t)(plan.nf - k - 1) * sizeof plan.f[0]);
			plan.nf--;
			break;
		case OP_CREATE:
			if (!plan.newtable && strcmp(d->label, "gpt") && d->label[0])
				return perr("Make a new GPT partition table first.");
			if (!plan.newtable && !d->label[0])
				return perr("Make a new GPT partition table first.");
			if (strcmp(o->kind, "esp") && strcmp(o->kind, "root") && strcmp(o->kind, "swap") && strcmp(o->kind, "data"))
				return perr("Unknown partition kind.");
			if (add_new(o->kind, o->start, o->size) < 0)
				return -1;
			break;
		case OP_FORMAT:
			k = fp_find(o->a);
			if (k < 0)
				return perr("Cannot format %s: not a partition of this disk.", o->a);
			if (strcmp(o->fs, "vfat") && strcmp(o->fs, "btrfs") && strcmp(o->fs, "ext4") && strcmp(o->fs, "swap"))
				return perr("Cannot make a %s file system.", o->fs);
			plan.f[k].fmt = 1;
			snprintf(plan.f[k].fs, sizeof plan.f[k].fs, "%s", o->fs);
			snprintf(plan.f[k].label, sizeof plan.f[k].label, "%s", o->label);
			break;
		case OP_MOUNT:
			k = fp_find(o->a);
			if (k < 0)
				return perr("Cannot use %s: not a partition of this disk.", o->a);
			snprintf(plan.f[k].mp, sizeof plan.f[k].mp, "%s", o->mp);
			break;
		}
	}
	/* for a kept existing partition, "format" is off unless asked: new ones are always formatted */
	plan.root = plan.boot = -1;
	for (int i = 0; i < plan.nf; i++) {
		struct fp *f = &plan.f[i];
		if (!f->mp[0])
			continue;
		if (!strcmp(f->mp, "/")) {
			if (plan.root >= 0)
				return perr("Two partitions are set to be \"/\".");
			plan.root = i;
		} else if (!strcmp(f->mp, "/boot")) {
			if (plan.boot >= 0)
				return perr("Two partitions are set to be \"/boot\".");
			plan.boot = i;
		} else if (strcmp(f->mp, "swap") && !data_mp_ok(f->mp))
			return perr("\"%s\" is not a place FreeLFS can mount a disk (use /, /boot, swap or /drives/NAME).", f->mp);
		for (int j = 0; j < i; j++)
			if (plan.f[j].mp[0] && !strcmp(plan.f[j].mp, f->mp) && strcmp(f->mp, "swap"))
				return perr("Two partitions use the place \"%s\".", f->mp);
	}
	if (plan.root < 0)
		return perr("No partition is set to be the system (\"/\").");
	if (plan.boot < 0)
		return perr("No partition is set to be the boot partition (\"/boot\").");
	struct fp *r = &plan.f[plan.root], *b = &plan.f[plan.boot];
	if (!r->fmt || strcmp(r->fs, "btrfs"))
		return perr("The system partition must be formatted as btrfs.");
	if (r->size < FI_MIN_ROOT_BYTES) {
		char a[32], c[32];
		human(r->size, a, sizeof a);
		human(FI_MIN_ROOT_BYTES, c, sizeof c);
		return perr("The system partition is %s; it needs at least %s.", a, c);
	}
	if (b->size < FI_MIN_BOOT_BYTES)
		return perr("The boot partition needs at least 200 MiB.");
	if (b->fmt && strcmp(b->fs, "vfat"))
		return perr("The boot partition must be FAT32.");
	if (!b->fmt && strcmp(b->fsnow, "vfat"))
		return perr("The boot partition must be FAT32: format it, or pick another.");
	if (plan.root == plan.boot)
		return perr("The system and boot partitions must be different.");
	if (!plan.newtable && strcmp(d->label, "gpt"))
		return perr("The disk needs a GPT partition table.");
	for (int i = 0; i < plan.nf; i++) {
		struct fp *f = &plan.f[i];
		if (!f->mp[0] || i == plan.root || i == plan.boot)
			continue;
		if (!strcmp(f->mp, "swap")) {
			if (!f->fmt && strcmp(f->fsnow, "swap"))
				return perr("Format %s as swap, or leave it alone.", f->key);
			if (f->fmt && strcmp(f->fs, "swap"))
				return perr("A swap partition must be formatted as swap.");
		} else if (f->fmt && !strcmp(f->fs, "swap") && strcmp(f->mp, "swap"))
			return perr("A swap partition cannot be mounted at %s.", f->mp);
		else if (!f->fmt && !f->fsnow[0])
			return perr("%s has no file system: format it before using it at %s.", f->key, f->mp);
	}
	/* a partition that is formatted but has no place is formatted anyway: refuse the surprise */
	for (int i = 0; i < plan.nf; i++)
		if (!plan.f[i].is_new && plan.f[i].fmt && !plan.f[i].mp[0])
			return perr("%s is set to be formatted but has no place to be used.", plan.f[i].key);
	/* only the root (always), the boot partition and what the window asked for touch their data */
	uuid4(plan.btrfs_uuid, sizeof plan.btrfs_uuid);
	unsigned char vb[4];
	getrandom(vb, 4, 0);
	snprintf(plan.volid, sizeof plan.volid, "%02X%02X%02X%02X", vb[0], vb[1], vb[2], vb[3]);
	return 0;
}

/* ---- the first account ------------------------------------------------------------------------ */

static int check_config(char *why, size_t cap)
{
	struct fi_reserved *r = calloc(1, sizeof *r);
	char p[PATH_MAX], buf[200];
	/* names the system already has: read from the source tree, links resolved by hand */
	(void)p;
	char rp[PATH_MAX];
	if (rpath(source_root, "/etc/passwd", rp, sizeof rp) == 0)
		fi_reserved_file(r, rp);
	if (rpath(source_root, "/etc/group", rp, sizeof rp) == 0)
		fi_reserved_file(r, rp);
	if (rpath(source_root, "/home", rp, sizeof rp) == 0)
		fi_reserved_dir(r, rp);
	const char *e = fi_user_error(cfg.user, r, buf, sizeof buf);
	free(r);
	if (e) {
		snprintf(why, cap, "User name: %s", e);
		return -1;
	}
	if ((e = fi_password_error(cfg.password))) {
		snprintf(why, cap, "Password: %s", e);
		return -1;
	}
	if (!fi_keyboard_ok(cfg.keyboard)) {
		snprintf(why, cap, "Keyboard layout not set.");
		return -1;
	}
	if (!fi_locale_ok(cfg.lang)) {
		snprintf(why, cap, "Language not set.");
		return -1;
	}
	if (!fi_zone_ok(cfg.tz)) {
		snprintf(why, cap, "Time zone not set.");
		return -1;
	}
	char zp[PATH_MAX], zr[PATH_MAX];
	snprintf(zp, sizeof zp, "/usr/share/zoneinfo/%s", cfg.tz);
	struct stat st;
	if (rpath(source_root, zp, zr, sizeof zr) || stat(zr, &st) || !S_ISREG(st.st_mode)) {
		snprintf(why, cap, "Time zone %s does not exist.", cfg.tz);
		return -1;
	}
	return 0;
}

/* ---- files inside the new system -------------------------------------------------------------- */

/* Resolve P (absolute inside ROOT) following links inside ROOT. The result is a host path; the
 * last part need not exist. */
int rpath(const char *root, const char *p, char *outp, size_t cap)
{
	char todo[PATH_MAX * 2], cur[PATH_MAX] = "";
	snprintf(todo, sizeof todo, "%s", p);
	int links = 0;
	while (todo[0]) {
		char *s = todo;
		while (*s == '/')
			s++;
		if (!*s)
			break;
		char comp[NAME_MAX + 1];
		size_t n = strcspn(s, "/");
		if (n > NAME_MAX)
			return -1;
		memcpy(comp, s, n);
		comp[n] = 0;
		char rest[PATH_MAX * 2];
		snprintf(rest, sizeof rest, "%s", s + n);
		snprintf(todo, sizeof todo, "%s", rest);
		if (!strcmp(comp, "."))
			continue;
		if (!strcmp(comp, "..")) {
			char *sl = strrchr(cur, '/');
			if (sl)
				*sl = 0;
			continue;
		}
		size_t cl = strlen(cur);
		if (cl + n + 2 > sizeof cur)
			return -1;
		cur[cl] = '/';
		strcpy(cur + cl + 1, comp);
		char host[PATH_MAX * 2], tgt[PATH_MAX];
		snprintf(host, sizeof host, "%s%s", root, cur);
		ssize_t r = readlink(host, tgt, sizeof tgt - 1);
		if (r > 0) {
			if (++links > 40)
				return -1;
			tgt[r] = 0;
			cur[cl] = 0;
			char nt[PATH_MAX * 2];
			if (tgt[0] == '/')
				cur[0] = 0;
			snprintf(nt, sizeof nt, "%s%s%s", tgt, "/", todo);
			snprintf(todo, sizeof todo, "%s", nt);
		}
	}
	return snprintf(outp, cap, "%s%s", root, cur) < (int)cap ? 0 : -1;
}

static char stage_at[PATH_MAX]; /* the overlay mount: the new root */
static char stage_home[PATH_MAX];
static uid_t new_uid = 1000;
static gid_t new_gid = 999;

static int write_whole(const char *host, const char *data, mode_t mode)
{
	int fd = open(host, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (fd < 0)
		return -1;
	size_t len = strlen(data), off = 0;
	while (off < len) {
		ssize_t w = write(fd, data + off, len - off);
		if (w <= 0) {
			close(fd);
			return -1;
		}
		off += (size_t)w;
	}
	return close(fd);
}

/* write FILE (a path inside the new root) with TEXT */
static int put_file(const char *inroot, const char *text, mode_t mode, const char *what)
{
	cmdf("# write %s (%s)", inroot, what);
	if (dry)
		return 0;
	char host[PATH_MAX];
	if (rpath(stage_at, inroot, host, sizeof host))
		return -1;
	char dir[PATH_MAX];
	snprintf(dir, sizeof dir, "%s", host);
	char *sl = strrchr(dir, '/');
	if (sl) {
		*sl = 0;
		mkdir(dir, 0755);
	}
	if (write_whole(host, text, mode) < 0) {
		err("cannot write %s: %s", inroot, strerror(errno));
		return -1;
	}
	return 0;
}

static char *slurp(const char *host)
{
	FILE *f = fopen(host, "r");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *b = malloc((size_t)n + 1);
	size_t got = fread(b, 1, (size_t)n, f);
	b[got] = 0;
	fclose(f);
	return b;
}

struct sbuf {
	char *p;
	size_t n, cap;
};

static void sb_add(struct sbuf *b, const char *s)
{
	size_t l = strlen(s);
	if (b->n + l + 1 > b->cap) {
		b->cap = (b->n + l + 1) * 2;
		b->p = realloc(b->p, b->cap);
	}
	memcpy(b->p + b->n, s, l + 1);
	b->n += l;
}

/* Rewrite an account file: drop lines of the user "tester", let FIX change a line (or return NULL to
 * keep it), then append the extra lines. kind: 'p' passwd, 's' shadow, 'g' group/gshadow. */
static int edit_accounts(const char *inroot, const char *extra, const char *rootpw_hash, int is_group)
{
	char host[PATH_MAX];
	if (rpath(stage_at, inroot, host, sizeof host))
		return -1;
	char *text = slurp(host);
	if (!text)
		return -1;
	struct sbuf nb = {0};
	char *sv = NULL;
	char *copy = strdup(text);
	for (char *l = strtok_r(copy, "\n", &sv); l; l = strtok_r(NULL, "\n", &sv)) {
		char line[1024];
		snprintf(line, sizeof line, "%s", l);
		if (!strncmp(line, "tester:", 7) && !is_group)
			continue;
		if (is_group) {
			/* drop tester from the member list (last field), add the new user to chosen groups */
			char *last = strrchr(line, ':');
			char name[64];
			sscanf(line, "%63[^:]", name);
			if (last) {
				char members[512] = "";
				char *ms = NULL, mcopy[512];
				snprintf(mcopy, sizeof mcopy, "%s", last + 1);
				for (char *m = strtok_r(mcopy, ",", &ms); m; m = strtok_r(NULL, ",", &ms)) {
					if (!strcmp(m, "tester"))
						continue;
					strncat(members, members[0] ? "," : "", sizeof members - strlen(members) - 1);
					strncat(members, m, sizeof members - strlen(members) - 1);
				}
				static const char *const join[] = {"seat", "audio", "video", "render", "wheel", "netdev", NULL};
				for (int i = 0; join[i]; i++)
					if (!strcmp(name, join[i])) {
						strncat(members, members[0] ? "," : "", sizeof members - strlen(members) - 1);
						strncat(members, cfg.user, sizeof members - strlen(members) - 1);
					}
				last[1] = 0;
				strncat(line, members, sizeof line - strlen(line) - 1);
			}
		} else if (rootpw_hash && !strncmp(line, "root:", 5)) {
			/* root:HASH:rest -- the same password as the first user */
			char *c2 = strchr(line + 5, ':');
			char tail[512] = "";
			if (c2)
				snprintf(tail, sizeof tail, "%s", c2);
			snprintf(line, sizeof line, "root:%s%s", rootpw_hash, tail);
		}
		sb_add(&nb, line);
		sb_add(&nb, "\n");
	}
	if (extra)
		sb_add(&nb, extra);
	free(copy);
	free(text);
	int r = write_whole(host, nb.p ? nb.p : "", 0644);
	free(nb.p);
	return r;
}

static int first_free_uid(void)
{
	char host[PATH_MAX];
	if (rpath(source_root, "/etc/passwd", host, sizeof host))
		return 1000;
	FILE *f = fopen(host, "r");
	char line[512];
	int used[2048] = {0};
	while (f && fgets(line, sizeof line, f)) {
		char name[64];
		int u;
		if (sscanf(line, "%63[^:]:%*[^:]:%d:", name, &u) == 2 && strcmp(name, "tester") && u >= 1000 && u < 3048)
			used[u - 1000] = 1;
	}
	if (f)
		fclose(f);
	for (int i = 0; i < 2048; i++)
		if (!used[i])
			return 1000 + i;
	return 3048;
}

static int group_gid(const char *name, int def)
{
	char host[PATH_MAX];
	if (rpath(source_root, "/etc/group", host, sizeof host))
		return def;
	FILE *f = fopen(host, "r");
	char line[512];
	int g = def;
	while (f && fgets(line, sizeof line, f)) {
		char n[64];
		int v;
		if (sscanf(line, "%63[^:]:%*[^:]:%d:", n, &v) == 2 && !strcmp(n, name))
			g = v;
	}
	if (f)
		fclose(f);
	return g;
}

static void crypt_pw(const char *pw, char *outp, size_t cap)
{
	static const char alpha[] = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
	unsigned char r[16];
	char salt[32] = "$6$";
	getrandom(r, sizeof r, 0);
	for (int i = 0; i < 16; i++)
		salt[3 + i] = alpha[r[i] & 63];
	salt[19] = '$';
	salt[20] = 0;
	struct crypt_data *cd = calloc(1, sizeof *cd);
	char *h = crypt_r(pw, salt, cd);
	if (h && h[0] != '*')
		snprintf(outp, cap, "%s", h);
	else
		outp[0] = 0;
	explicit_bzero(cd, sizeof *cd);
	free(cd);
}

static int copy_tree(const char *src, const char *dst, int own)
{
	struct stat st;
	if (lstat(src, &st) < 0)
		return -1;
	if (S_ISLNK(st.st_mode)) {
		char t[PATH_MAX];
		ssize_t r = readlink(src, t, sizeof t - 1);
		if (r < 0)
			return -1;
		t[r] = 0;
		if (symlink(t, dst) < 0)
			return -1;
		if (own)
			lchown(dst, new_uid, new_gid);
		return 0;
	}
	if (S_ISDIR(st.st_mode)) {
		if (mkdir(dst, st.st_mode & 07777) < 0 && errno != EEXIST)
			return -1;
		DIR *d = opendir(src);
		struct dirent *e;
		while (d && (e = readdir(d))) {
			if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
				continue;
			char s2[PATH_MAX], d2[PATH_MAX];
			snprintf(s2, sizeof s2, "%s/%s", src, e->d_name);
			snprintf(d2, sizeof d2, "%s/%s", dst, e->d_name);
			copy_tree(s2, d2, own);
		}
		if (d)
			closedir(d);
		if (own)
			chown(dst, new_uid, new_gid);
		return 0;
	}
	if (!S_ISREG(st.st_mode))
		return 0;
	int in = open(src, O_RDONLY), o = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 07777);
	if (in < 0 || o < 0) {
		if (in >= 0)
			close(in);
		if (o >= 0)
			close(o);
		return -1;
	}
	char buf[65536];
	ssize_t n;
	while ((n = read(in, buf, sizeof buf)) > 0)
		if (write(o, buf, (size_t)n) != n)
			break;
	close(in);
	close(o);
	if (own)
		chown(dst, new_uid, new_gid);
	return 0;
}

/* ---- the new system: what differs from the live one ---------------------------------------------- */

static const char *fstab_tail =
    "proc            /proc           proc      nosuid,noexec,nodev          0    0\n"
    "sysfs           /sys            sysfs     nosuid,noexec,nodev          0    0\n"
    "devpts          /dev/pts        devpts    gid=5,mode=620               0    0\n"
    "tmpfs           /run            tmpfs     defaults                     0    0\n"
    "devtmpfs        /dev            devtmpfs  mode=0755,nosuid             0    0\n"
    "tmpfs           /dev/shm        tmpfs     nosuid,nodev                 0    0\n"
    "tmpfs           /tmp            tmpfs     nosuid,nodev,mode=1777       0    0\n"
    "cgroup2         /sys/fs/cgroup  cgroup2   nosuid,noexec,nodev          0    0\n";

#define VT_COLORS                                                                                                       \
	"vt.default_red=29,224,63,216,111,198,86,216,122,239,95,232,143,215,127,240 "                                       \
	"vt.default_grn=33,108,155,181,168,120,182,220,129,128,191,201,192,160,208,242 "                                    \
	"vt.default_blu=37,108,99,106,220,221,194,224,137,128,131,138,236,232,218,244"

static int customise(void)
{
	char buf[8192];
	struct fp *r = &plan.f[plan.root], *b = &plan.f[plan.boot];
	/* fstab: file systems by UUID, never by label (a second FreeLFS must not clash) */
	snprintf(buf, sizeof buf,
	         "# /etc/fstab -- FreeLFS (written by the installer)\n#\n"
	         "# The root file system is btrfs: subvolume @ (the default one) is /, @home is /home, where\n"
	         "# people's homes are, @snapshots holds snapshots of /. The EFI system partition is /boot.\n#\n"
	         "# file system   mount point     type      options                      dump pass\n"
	         "UUID=%s  /     btrfs  defaults,noatime,compress=zstd:1 0 0\n"
	         "UUID=%s  /home  btrfs  defaults,noatime,compress=zstd:1,subvol=@home,nofail 0 0\n"
	         "UUID=%s  /.snapshots  btrfs  defaults,noatime,subvol=@snapshots,nofail 0 0\n"
	         "PARTUUID=%s  /boot  vfat  defaults,umask=0022,quiet,nofail 0 2\n",
	         plan.btrfs_uuid, plan.btrfs_uuid, plan.btrfs_uuid, b->uuid);
	for (int i = 0; i < plan.nf; i++) {
		struct fp *f = &plan.f[i];
		size_t n = strlen(buf);
		if (!f->mp[0] || i == plan.root || i == plan.boot)
			continue;
		const char *fs = f->fmt ? f->fs : f->fsnow;
		if (!strcmp(f->mp, "swap"))
			snprintf(buf + n, sizeof buf - n, "PARTUUID=%s  none  swap  sw,nofail 0 0\n", f->uuid);
		else
			snprintf(buf + n, sizeof buf - n, "PARTUUID=%s  %s  %s  defaults,noatime,nofail 0 2\n", f->uuid, f->mp,
			         !strcmp(fs, "ntfs") ? "ntfs3" : fs);
	}
	strncat(buf, fstab_tail, sizeof buf - strlen(buf) - 1);
	(void)r;
	if (put_file("/etc/fstab", buf, 0644, "file systems by UUID") < 0)
		return -1;
	/* data partitions need their mount points */
	for (int i = 0; i < plan.nf; i++) {
		struct fp *f = &plan.f[i];
		if (f->mp[0] && !strncmp(f->mp, "/drives/", 8)) {
			cmdf("mkdir %s", f->mp);
			if (!dry) {
				char host[PATH_MAX];
				if (!rpath(stage_at, f->mp, host, sizeof host))
					mkdir(host, 0755);
			}
		}
	}
	snprintf(buf, sizeof buf, "LAYOUT=%s\n", cfg.keyboard);
	if (put_file("/etc/keyboard.conf", buf, 0644, "keyboard: the first boot does not ask again") < 0)
		return -1;
	snprintf(buf, sizeof buf,
	         "# language, set by the installer; a text console keeps C.UTF-8 (it lacks fonts)\n"
	         "[ \"$TERM\" = linux ] || export LANG=%s\n",
	         cfg.lang);
	if (put_file("/etc/profile.d/20-lang.sh", buf, 0644, "language") < 0)
		return -1;
	snprintf(buf, sizeof buf, "%s\n", cfg.tz);
	if (put_file("/etc/timezone", buf, 0644, "time zone") < 0)
		return -1;
	cmdf("# link /etc/localtime to /usr/share/zoneinfo/%s", cfg.tz);
	if (!dry) {
		char host[PATH_MAX], tgt[PATH_MAX], tmp[PATH_MAX];
		/* the link itself, not what it points to: resolve the folder only */
		if (rpath(stage_at, "/etc", host, sizeof host) == 0) {
			size_t hl = strlen(host);
			snprintf(host + hl, sizeof host - hl, "/localtime");
			snprintf(tgt, sizeof tgt, "/usr/share/zoneinfo/%s", cfg.tz);
			snprintf(tmp, sizeof tmp, "%s.new", host);
			unlink(tmp);
			if (symlink(tgt, tmp) < 0 || rename(tmp, host) < 0)
				return err("cannot link /etc/localtime: %s", strerror(errno)), -1;
		}
	}
	/* the live system turned snapshots off and added a note to /etc/issue */
	cmdf("# edit /etc/flpkg.conf: snapshots = 5 (the live system has 0)");
	cmdf("# edit /etc/issue: drop the live system note");
	if (!dry) {
		char host[PATH_MAX];
		if (rpath(stage_at, "/etc/flpkg.conf", host, sizeof host) == 0) {
			char *t = slurp(host);
			if (t) {
				struct sbuf nb = {0};
				char *sv = NULL;
				for (char *l = strtok_r(t, "\n", &sv); l; l = strtok_r(NULL, "\n", &sv)) {
					if (!strncmp(l, "snapshots = 0", 13))
						sb_add(&nb, "snapshots = 5                   # btrfs snapshots of / to keep (0: none);");
					else
						sb_add(&nb, l);
					sb_add(&nb, "\n");
				}
				write_whole(host, nb.p ? nb.p : "", 0644);
				free(nb.p);
				free(t);
			}
		}
		if (rpath(stage_at, "/etc/issue", host, sizeof host) == 0) {
			char *t = slurp(host);
			if (t) {
				struct sbuf nb = {0};
				char *sv = NULL;
				for (char *l = strtok_r(t, "\n", &sv); l; l = strtok_r(NULL, "\n", &sv)) {
					if (!strncmp(l, "Live system:", 12) || !strncmp(l, "written to the disks of this computer", 37) ||
					    !strncmp(l, "Development image:", 18))
						continue;
					sb_add(&nb, l);
					sb_add(&nb, "\n");
				}
				while (nb.n > 1 && nb.p[nb.n - 1] == '\n' && nb.p[nb.n - 2] == '\n')
					nb.p[--nb.n] = 0;
				write_whole(host, nb.p ? nb.p : "", 0644);
				free(nb.p);
				free(t);
			}
		}
	}
	/* accounts */
	char hash[256];
	crypt_pw(cfg.password, hash, sizeof hash);
	if (!hash[0] && !dry)
		return err("cannot hash the password"), -1;
	new_uid = (uid_t)first_free_uid();
	new_gid = (gid_t)group_gid("users", 999);
	char line[600];
	const char *shell = "/usr/bin/flsh";
	cmdf("# edit /etc/passwd /etc/shadow /etc/group /etc/gshadow: remove the live user, add %s (uid %u), "
	     "%s",
	     cfg.user, (unsigned)new_uid, FI_ROOT_SAME_PASSWORD ? "root gets the same password" : "root is locked");
	if (!dry) {
		char hp[PATH_MAX];
		if (rpath(stage_at, "/usr/bin/flsh", hp, sizeof hp) || access(hp, X_OK))
			shell = "/usr/bin/bash";
		char full[100];
		snprintf(full, sizeof full, "%s", cfg.fullname);
		for (char *c = full; *c; c++)
			if (*c == ':' || *c == ',' || *c == '\n')
				*c = ' ';
		snprintf(line, sizeof line, "%s:x:%u:%u:%s:/home/%s:%s\n", cfg.user, (unsigned)new_uid, (unsigned)new_gid, full,
		         cfg.user, shell);
		if (edit_accounts("/etc/passwd", line, NULL, 0) < 0)
			return err("cannot edit /etc/passwd"), -1;
		snprintf(line, sizeof line, "%s:%s:%ld:0:99999:7:::\n", cfg.user, hash, (long)(time(NULL) / 86400));
		if (edit_accounts("/etc/shadow", line, FI_ROOT_SAME_PASSWORD ? hash : "!", 0) < 0)
			return err("cannot edit /etc/shadow"), -1;
		if (edit_accounts("/etc/group", NULL, NULL, 1) < 0)
			return err("cannot edit /etc/group"), -1;
		if (edit_accounts("/etc/gshadow", NULL, NULL, 1) < 0)
			return err("cannot edit /etc/gshadow"), -1;
	}
	explicit_bzero(hash, sizeof hash);
	/* the home (in @home, which is /home) */
	cmdf("# make /home/%s from /etc/skel, owned by uid %u", cfg.user, (unsigned)new_uid);
	if (!dry) {
		char skel[PATH_MAX], home[PATH_MAX];
		snprintf(home, sizeof home, "%s/%s", stage_home, cfg.user);
		int own = geteuid() == 0 && !image_path[0];
		if (mkdir(home, 0700) < 0)
			return err("cannot make the home: %s", strerror(errno)), -1;
		if (own)
			chown(home, new_uid, new_gid);
		if (rpath(source_root, "/etc/skel", skel, sizeof skel) == 0) {
			DIR *d = opendir(skel);
			struct dirent *e;
			while (d && (e = readdir(d))) {
				if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
					continue;
				char s2[PATH_MAX], d2[PATH_MAX];
				snprintf(s2, sizeof s2, "%s/%s", skel, e->d_name);
				snprintf(d2, sizeof d2, "%s/%s", home, e->d_name);
				copy_tree(s2, d2, own);
			}
			if (d)
				closedir(d);
		}
		if (!own)
			logline("image mode: the home keeps the owner of the person running the test");
	}
	/* Wi-Fi networks the live system knows */
	const char *iwd = iwd_arg[0] ? iwd_arg : (image_path[0] ? NULL : "/var/lib/iwd");
	if (iwd) {
		cmdf("# copy known Wi-Fi networks from %s to /var/lib/iwd (mode 0600)", iwd);
		if (!dry) {
			DIR *d = opendir(iwd);
			struct dirent *e;
			char dstd[PATH_MAX];
			if (d && rpath(stage_at, "/var/lib/iwd", dstd, sizeof dstd) == 0) {
				mkdir(dstd, 0700);
				while ((e = readdir(d))) {
					size_t n = strlen(e->d_name);
					if (n > 4 && (!strcmp(e->d_name + n - 4, ".psk") || !strcmp(e->d_name + n - 5, ".open") ||
					              !strcmp(e->d_name + n - 5, ".8021x"))) {
						char s2[PATH_MAX], d2[PATH_MAX];
						snprintf(s2, sizeof s2, "%s/%s", iwd, e->d_name);
						snprintf(d2, sizeof d2, "%s/%s", dstd, e->d_name);
						copy_tree(s2, d2, 0);
						chmod(d2, 0600);
					}
				}
			}
			if (d)
				closedir(d);
		}
	}
	return 0;
}

/* ---- mounts in the private namespace ------------------------------------------------------------ */

static int x_mount(const char *src, const char *tgt, const char *type, unsigned long fl, const char *opts)
{
	if (opts && *opts)
		cmdf("mount -t %s -o %s %s %s", type ? type : "none", opts, src, tgt);
	else
		cmdf("mount -t %s %s %s", type ? type : "none", src, tgt);
	if (dry)
		return 0;
	if (mount(src, tgt, type, fl, opts) < 0) {
		err("mount %s on %s: %s", type ? type : "", tgt, strerror(errno));
		return -1;
	}
	return 0;
}

static int in_userns(void)
{
	uid_t uid = getuid();
	gid_t gid = getgid();
	if (unshare(CLONE_NEWUSER | CLONE_NEWNS) < 0)
		return -1;
	char m[64];
	int fd = open("/proc/self/setgroups", O_WRONLY);
	if (fd >= 0) {
		(void)!write(fd, "deny", 4);
		close(fd);
	}
	snprintf(m, sizeof m, "0 %u 1", (unsigned)uid);
	fd = open("/proc/self/uid_map", O_WRONLY);
	if (fd < 0 || write(fd, m, strlen(m)) < 0)
		return -1;
	close(fd);
	snprintf(m, sizeof m, "0 %u 1", (unsigned)gid);
	fd = open("/proc/self/gid_map", O_WRONLY);
	if (fd < 0 || write(fd, m, strlen(m)) < 0)
		return -1;
	close(fd);
	return 0;
}

/* ---- the install ----------------------------------------------------------------------------------- */

static char workdir[PATH_MAX];
static long long src_bytes;
static int nstep, total_steps = 5;
struct tmpimg {
	char path[PATH_MAX];
	long long start, size;
	int used;
};
static struct tmpimg timg[MAXPART];
static int ntimg;

static void step(const char *text)
{
	out("STEP\t%d\t%d\t%s", ++nstep, total_steps, text);
}

static int sum_cb(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
	if (type == FTW_F)
		src_bytes += st->st_size;
	return 0;
}

/* The node to write a partition through: its device, or in image mode a scratch file that is copied
 * into the image at the end. */
static const char *node_for(struct fp *f)
{
	if (!image_path[0])
		return f->dev;
	for (int i = 0; i < ntimg; i++)
		if (timg[i].start == f->start)
			return timg[i].path;
	struct tmpimg *t = &timg[ntimg++];
	t->start = f->start;
	t->size = f->size;
	snprintf(t->path, sizeof t->path, "%s/part-%lld.img", workdir, f->start / FI_MIB);
	char sz[32];
	snprintf(sz, sizeof sz, "%lld", f->size);
	char *tr[] = {"truncate", "-s", sz, t->path, NULL};
	run(tr, NULL);
	if (!f->is_new) { /* keep what is there: copy it out first */
		char skip[32], cnt[32], ifa[PATH_MAX + 8], ofa[PATH_MAX + 8];
		snprintf(skip, sizeof skip, "skip=%lld", f->start / FI_MIB);
		snprintf(cnt, sizeof cnt, "count=%lld", f->size / FI_MIB);
		snprintf(ifa, sizeof ifa, "if=%s", image_path);
		snprintf(ofa, sizeof ofa, "of=%s", t->path);
		char *dd[] = {"dd", ifa, ofa, "bs=1M", skip, cnt, "conv=notrunc", "status=none", NULL};
		run(dd, NULL);
	}
	return t->path;
}

static int commit_images(void)
{
	for (int i = 0; i < ntimg; i++) {
		char seek[32], ifa[PATH_MAX + 8], ofa[PATH_MAX + 8];
		snprintf(seek, sizeof seek, "seek=%lld", timg[i].start / FI_MIB);
		snprintf(ifa, sizeof ifa, "if=%s", timg[i].path);
		snprintf(ofa, sizeof ofa, "of=%s", image_path);
		char *dd[] = {"dd", ifa, ofa, "bs=1M", seek, "conv=notrunc,sparse", "status=none", NULL};
		if (run(dd, NULL))
			return -1;
	}
	return 0;
}

static int find_kernel(char *outp, size_t cap)
{
	static const char *const pats[] = {"/run/live/iso/live", "/boot", "/run/live/rom/boot", NULL};
	if (fake_dir[0] && !kernel_arg[0]) {
		snprintf(outp, cap, "/run/live/iso/live/vmlinuz-X");
		return 0;
	}
	if (kernel_arg[0]) {
		if (access(kernel_arg, R_OK))
			return -1;
		snprintf(outp, cap, "%s", kernel_arg);
		return 0;
	}
	for (int i = 0; pats[i]; i++) {
		DIR *d = opendir(pats[i]);
		struct dirent *e;
		char best[PATH_MAX] = "";
		while (d && (e = readdir(d)))
			if (!strncmp(e->d_name, "vmlinuz-", 8) && strcmp(e->d_name, best) > 0)
				snprintf(best, sizeof best, "%s", e->d_name);
		if (d)
			closedir(d);
		if (best[0]) {
			snprintf(outp, cap, "%s/%s", pats[i], best);
			return 0;
		}
	}
	return -1;
}

static int partition_disk(void)
{
	struct disk *d = plan.d;
	char script[16384] = "";
	size_t n = 0;
	int any_new = 0;
	for (int i = 0; i < plan.nf; i++)
		if (plan.f[i].is_new)
			any_new = 1;
	if (plan.newtable)
		n += (size_t)snprintf(script + n, sizeof script - n, "label: gpt\nunit: sectors\nfirst-lba: %lld\n\n", 2048LL * 512 / d->ssz);
	for (int i = 0; i < plan.nf; i++) {
		struct fp *f = &plan.f[i];
		if (!f->is_new)
			continue;
		n += (size_t)snprintf(script + n, sizeof script - n, "start=%lld, size=%lld, type=%s, uuid=%s, name=\"%s\"\n",
		                      f->start / d->ssz, f->size / d->ssz, f->type, f->uuid, f->name);
	}
	if (plan.newtable) {
		char *argv[] = {"sfdisk", "--quiet", "--wipe", "always", "--wipe-partitions", "always", d->dev, NULL};
		if (run(argv, script))
			return err("sfdisk could not write the partition table"), -1;
	} else {
		for (int i = 0; i < plan.ndel; i++) {
			char num[16];
			snprintf(num, sizeof num, "%d", plan.del_num[i]);
			char *argv[] = {"sfdisk", "--quiet", "--delete", d->dev, num, NULL};
			if (run(argv, NULL))
				return err("sfdisk could not delete partition %s", num), -1;
		}
		if (any_new) {
			char *argv[] = {"sfdisk", "--quiet", "--append", "--wipe", "always", "--wipe-partitions", "always", d->dev, NULL};
			if (run(argv, script))
				return err("sfdisk could not add the partitions"), -1;
		}
	}
	/* find the nodes of the new partitions by their PARTUUID in a fresh dump */
	if (dry) {
		int next = 1;
		for (int i = 0; i < d->np; i++)
			if (d->p[i].num >= next)
				next = d->p[i].num + 1;
		const char *sep = isdigit((unsigned char)d->dev[strlen(d->dev) - 1]) ? "p" : "";
		for (int i = 0; i < plan.nf; i++)
			if (plan.f[i].is_new)
				snprintf(plan.f[i].dev, sizeof plan.f[i].dev, "%s%s%d", d->dev, sep, plan.newtable ? (i + 1) : next++);
		return 0;
	}
	if (!image_path[0]) {
		char *settle[] = {"udevadm", "settle", "--timeout=10", NULL};
		run(settle, NULL);
	}
	struct disk fresh = *d;
	char buf[65536];
	char *dump[] = {"sfdisk", "-d", d->dev, NULL};
	for (int tries = 0; tries < 50; tries++) {
		if (ask(dump, buf, sizeof buf) == 0) {
			parse_dump(&fresh, buf);
			int ok = 1;
			for (int i = 0; i < plan.nf; i++) {
				struct fp *f = &plan.f[i];
				if (!f->is_new)
					continue;
				int found = 0;
				for (int k = 0; k < fresh.np; k++)
					if (ieq(fresh.p[k].uuid, f->uuid)) {
						found = 1;
						snprintf(f->dev, sizeof f->dev, "%s", fresh.p[k].dev);
						f->num = fresh.p[k].num;
						if (!image_path[0] && access(f->dev, F_OK))
							found = 0;
					}
				ok &= found;
			}
			if (ok)
				return 0;
		}
		usleep(200000);
	}
	return err("the new partitions did not show up"), -1;
}

static int format_one(struct fp *f)
{
	const char *node = node_for(f);
	if (f == &plan.f[plan.root])
		return 0; /* mkfs.btrfs below, with the files */
	if (!f->fmt)
		return 0;
	char *wipe[] = {"wipefs", "--all", "--quiet", (char *)node, NULL};
	run(wipe, NULL);
	int r;
	if (!strcmp(f->fs, "vfat")) {
		char *argv[] = {"mkfs.fat", "-F", "32", "-n", f->label[0] ? f->label : "ESP", "-i", plan.volid, (char *)node, NULL};
		r = run(argv, NULL);
	} else if (!strcmp(f->fs, "ext4")) {
		char *argv[] = {"mkfs.ext4", "-F", "-q", "-L", f->label[0] ? f->label : "data", (char *)node, NULL};
		r = run(argv, NULL);
	} else if (!strcmp(f->fs, "btrfs")) {
		char *argv[] = {"mkfs.btrfs", "-f", "-q", "-L", f->label[0] ? f->label : "data", (char *)node, NULL};
		r = run(argv, NULL);
	} else {
		char *argv[] = {"mkswap", "-L", "swap", "-U", f->uuid, (char *)node, NULL};
		r = run(argv, NULL);
	}
	if (r)
		return err("formatting %s failed", f->key), -1;
	return 0;
}

static void progress(int pct)
{
	out("PROGRESS\t%d", pct);
}

/* Fill the root partition: the namespace, the overlay, the edits, mkfs.btrfs --rootdir. */
static int populate_root(void)
{
	struct fp *r = &plan.f[plan.root];
	const char *node = node_for(r);
	char stage[PATH_MAX], ovl[PATH_MAX], tmp[PATH_MAX], up[PATH_MAX], wk[PATH_MAX], opts[PATH_MAX * 3];
	snprintf(stage, sizeof stage, "%s/stage", workdir);
	snprintf(ovl, sizeof ovl, "%s/ovl", workdir);
	snprintf(stage_at, sizeof stage_at, "%s/@", stage);
	snprintf(stage_home, sizeof stage_home, "%s/@home", stage);
	(void)tmp;
	if (!dry) {
		mkdir(stage, 0755);
		mkdir(ovl, 0755);
		src_bytes = 0;
		nftw(source_root, sum_cb, 64, FTW_PHYS | FTW_MOUNT);
		if (src_bytes < 1)
			src_bytes = 1;
	}
	int pfd[2];
	if (!dry && pipe(pfd) < 0)
		return -1;
	pid_t pid = dry ? 0 : fork();
	if (pid < 0)
		return -1;
	if (pid > 0) {
		close(pfd[1]);
		struct pollfd p = {.fd = pfd[0], .events = POLLIN};
		char line[512];
		size_t len = 0;
		int last = -1, st = 0, done = 0;
		while (!done) {
			int pr = poll(&p, 1, 500);
			if (pr > 0) {
				char c;
				ssize_t n = read(pfd[0], &c, 1);
				if (n <= 0)
					done = 1;
				else if (c == '\n') {
					line[len] = 0;
					if (len && (!strncmp(line, "CMD\t", 4) || !strncmp(line, "LOG\t", 4) || !strncmp(line, "ERR\t", 4)))
						out("%s", line);
					else if (len)
						logline(line);
					len = 0;
				} else if (len < sizeof line - 1)
					line[len++] = c;
				continue;
			}
			char io[64], b[256];
			snprintf(io, sizeof io, "/proc/%d/io", (int)pid);
			FILE *f = fopen(io, "r");
			while (f && fgets(b, sizeof b, f))
				if (!strncmp(b, "rchar:", 6)) {
					int pct = (int)(atoll(b + 6) * 100 / src_bytes);
					if (pct > 99)
						pct = 99;
					if (pct != last) {
						progress(pct);
						last = pct;
					}
				}
			if (f)
				fclose(f);
		}
		close(pfd[0]);
		waitpid(pid, &st, 0);
		if (!WIFEXITED(st) || WEXITSTATUS(st))
			return err("copying the system failed (exit %d)", WIFEXITED(st) ? WEXITSTATUS(st) : -1), -1;
		progress(100);
		return 0;
	}
	if (!dry) { /* the child: its output goes to the pipe */
		close(pfd[0]);
		dup2(pfd[1], 1);
		dup2(pfd[1], 2);
		close(pfd[1]);
		if (image_path[0] || geteuid() != 0 ? in_userns() < 0 : unshare(CLONE_NEWNS) < 0) {
			fprintf(stderr, "cannot make a private mount namespace: %s\n", strerror(errno));
			_exit(1);
		}
		mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL);
	} else {
		cmdf("# (in a private mount namespace: unshare --mount%s)", image_path[0] ? " --user" : "");
	}
	int fail = 0;
	snprintf(up, sizeof up, "%s/up", ovl);
	snprintf(wk, sizeof wk, "%s/wk", ovl);
	if (!dry) {
		/* from here on every message is a line in the pipe; err() is out(), which is stdout: fine */
	}
	fail |= x_mount("tmpfs", ovl, "tmpfs", 0, "mode=0755") < 0;
	if (!dry && !fail) {
		mkdir(up, 0755);
		mkdir(wk, 0755);
	}
	fail |= !fail && x_mount("tmpfs", stage, "tmpfs", 0, "mode=0755") < 0;
	if (!dry && !fail) {
		mkdir(stage_at, 0755);
		mkdir(stage_home, 0755);
		char sn[PATH_MAX];
		snprintf(sn, sizeof sn, "%s/@snapshots", stage);
		mkdir(sn, 0755);
	}
	if (dry)
		cmdf("mkdir %s %s %s/@snapshots", stage_at, stage_home, stage);
	snprintf(opts, sizeof opts, "lowerdir=%s,upperdir=%s,workdir=%s", source_root, up, wk);
	fail |= !fail && x_mount("overlay", stage_at, "overlay", 0, opts) < 0;
	char boot[PATH_MAX];
	snprintf(boot, sizeof boot, "%s/boot", stage_at);
	fail |= !fail && x_mount("tmpfs", boot, "tmpfs", 0, "mode=0755") < 0; /* /boot is the ESP: no files of it in @ */
	fail |= !fail && x_mount("tmpfs", stage_home, "tmpfs", 0, "mode=0755") < 0;
	/* /home is @home on the installed system: the live user's home stays out of @ */
	char lhome[PATH_MAX];
	snprintf(lhome, sizeof lhome, "%s/home", stage_at);
	cmdf("# leave /home of the live system (the live user's home) out of @");
	fail |= !fail && x_mount("tmpfs", lhome, "tmpfs", 0, "mode=0755") < 0;
	if (!fail) {
		fail |= customise() < 0;
	}
	if (!fail) {
		char *argv[] = {"mkfs.btrfs", "-f", "-q", "-L", "freelfs", "-U", plan.btrfs_uuid, "--rootdir", stage, "--subvol",
		                "default:@", "--subvol", "@home", "--subvol", "@snapshots", (char *)node, NULL};
		fail |= run(argv, NULL) != 0;
	}
	if (dry)
		return fail ? -1 : 0;
	_exit(fail ? 1 : 0);
}

static int install_boot(void)
{
	struct fp *b = &plan.f[plan.boot], *r = &plan.f[plan.root];
	char espdir[PATH_MAX], conf[PATH_MAX * 2], kernel[PATH_MAX];
	snprintf(espdir, sizeof espdir, "%s/esp", workdir);
	if (find_kernel(kernel, sizeof kernel) < 0)
		return err("no kernel found for the new system"), -1;
	const char *kname = strrchr(kernel, '/') + 1;
	/* CPU microcode (build/recipes: package "microcode") sits next to the kernel; the EFI stub loads it
	 * as an initrd before the kernel starts, only when the file exists (a missing initrd= file fails boot) */
	char ucode[PATH_MAX];
	snprintf(ucode, sizeof ucode, "%.*s/ucode.img", (int)(kname - kernel - 1), kernel);
	int has_ucode = access(ucode, R_OK) == 0;
	cmdf("mkdir -p %s/flboot", espdir);
	if (!dry) {
		mkdir(espdir, 0755);
		char d2[PATH_MAX];
		snprintf(d2, sizeof d2, "%s/flboot", espdir);
		mkdir(d2, 0755);
	}
	snprintf(conf, sizeof conf,
	         "# flboot.conf -- written by the installer; see /usr/share/doc/flboot/README\n"
	         "timeout 5\n"
	         "cmdline %sroot=PARTUUID=%s rootwait ro quiet panic=30 console=tty0 %s\n",
	         has_ucode ? "initrd=\\ucode.img " : "", r->uuid, VT_COLORS);
	cmdf("# write %s/flboot/flboot.conf: root=PARTUUID=%s", espdir, r->uuid);
	if (!dry) {
		char p[PATH_MAX];
		snprintf(p, sizeof p, "%s/flboot/flboot.conf", espdir);
		if (write_whole(p, conf, 0644) < 0)
			return err("cannot write flboot.conf"), -1;
	}
	char *fbi[] = {"flboot-install", "--esp", espdir, NULL};
	if (run(fbi, NULL))
		return err("flboot-install failed"), -1;
	char to[PATH_MAX];
	snprintf(to, sizeof to, "%s/%s", espdir, kname);
	char *cp[] = {"cp", kernel, to, NULL};
	if (run(cp, NULL))
		return err("cannot stage the kernel"), -1;
	char tu[PATH_MAX];
	snprintf(tu, sizeof tu, "%s/ucode.img", espdir);
	if (has_ucode) {
		char *cu[] = {"cp", ucode, tu, NULL};
		if (run(cu, NULL))
			return err("cannot stage the CPU microcode"), -1;
	}
	const char *node = node_for(b);
	char dst[PATH_MAX + 8], d1[PATH_MAX], d2[PATH_MAX], d3[PATH_MAX], img[PATH_MAX + 4];
	snprintf(img, sizeof img, "%s", node);
	snprintf(d1, sizeof d1, "%s/EFI", espdir);
	snprintf(d2, sizeof d2, "%s/flboot", espdir);
	snprintf(d3, sizeof d3, "%s", to);
	snprintf(dst, sizeof dst, "::/");
	setenv("MTOOLS_SKIP_CHECK", "1", 1);
	setenv("MTOOLSRC", "/dev/null", 1);
	char *mc[] = {"mcopy", "-i", img, "-s", "-o", "-Q", d1, d2, d3, has_ucode ? tu : dst, has_ucode ? dst : NULL, NULL};
	if (run(mc, NULL))
		return err("cannot copy the boot files to the boot partition"), -1;
	return 0;
}

static void do_install(void)
{
	char why[300] = "";
	if (check_config(why, sizeof why) < 0) {
		out("FAIL\t%s", why);
		return;
	}
	if (build_plan() < 0) {
		out("FAIL\t%s", plan.errbuf);
		return;
	}
	char kernel[PATH_MAX];
	if (find_kernel(kernel, sizeof kernel) < 0) {
		out("FAIL\tThis installer medium has no kernel for the new system (looked in /run/live/iso/live and /boot).");
		return;
	}
	static const char *const need[] = {"sfdisk", "wipefs", "mkfs.btrfs", "mkfs.fat", "mcopy", "flboot-install", "mkswap",
	                                   "mkfs.ext4", NULL};
	for (int i = 0; need[i] && !fake_dir[0]; i++)
		if (!tool_exists(need[i])) {
			out("FAIL\tThe tool %s is missing on this system.", need[i]);
			return;
		}
	if (!dry && !image_path[0] && !fake_dir[0] && access("/usr/lib/flboot/flboot.efi", R_OK)) {
		out("FAIL\tflboot.efi is missing on this system.");
		return;
	}
	nstep = 0;
	/* the work directory */
	if (dry)
		snprintf(workdir, sizeof workdir, "/run/flinstall.XXXXXX");
	else {
		const char *base = work_arg[0] ? work_arg : (image_path[0] ? NULL : "/run");
		char t[PATH_MAX];
		if (base)
			snprintf(t, sizeof t, "%s/flinstall.XXXXXX", base);
		else {
			snprintf(t, sizeof t, "%s.work.XXXXXX", image_path);
		}
		if (!mkdtemp(t)) {
			out("FAIL\tcannot make a work directory: %s", strerror(errno));
			return;
		}
		snprintf(workdir, sizeof workdir, "%s", t);
	}
	int bad = 0;
	step("Preparing the disk");
	bad = partition_disk() < 0;
	if (!bad) {
		step("Formatting");
		for (int i = 0; i < plan.nf && !bad; i++)
			if (plan.f[i].is_new || plan.f[i].fmt)
				bad = format_one(&plan.f[i]) < 0;
	}
	if (!bad) {
		step("Copying the system");
		progress(0);
		bad = populate_root() < 0;
	}
	if (!bad) {
		step("Installing the boot loader");
		bad = install_boot() < 0;
	}
	if (!bad) {
		step("Writing to the disk");
		if (image_path[0] && !dry)
			bad = commit_images() < 0;
		else if (!dry) {
			char *sync_[] = {"sync", NULL};
			run(sync_, NULL);
		} else
			cmdf("sync");
	}
	if (!dry && workdir[0]) {
		char *rm[] = {"rm", "-rf", workdir, NULL};
		pid_t pid = fork();
		if (pid == 0) {
			int n = open("/dev/null", O_RDWR);
			dup2(n, 1);
			dup2(n, 2);
			execvp("rm", rm);
			_exit(127);
		}
		if (pid > 0)
			waitpid(pid, NULL, 0);
	}
	explicit_bzero(cfg.password, sizeof cfg.password);
	if (bad)
		out("FAIL\tThe installation stopped. The messages above say where.");
	else
		out(dry ? "DONE\tdry run" : "DONE");
}

static void cmd_check(void)
{
	char why[300] = "";
	if (check_config(why, sizeof why) < 0) {
		err("%s", why);
		return;
	}
	if (build_plan() < 0) {
		err("%s", plan.errbuf);
		return;
	}
	struct disk *d = plan.d;
	char sz[32];
	if (plan.newtable)
		out("PLAN\tnewtable\t%s\t-\t-\tThe partition table of %s is replaced; everything on it is erased.", d->dev, d->dev);
	for (int i = 0; i < plan.ndel && !plan.newtable; i++)
		out("PLAN\tdelete\t%s\t-\t-\tPartition %s is deleted.", plan.del_dev[i], plan.del_dev[i]);
	for (int i = 0; i < plan.nf; i++) {
		struct fp *f = &plan.f[i];
		human(f->size, sz, sizeof sz);
		const char *act = f->is_new ? (f->fmt ? "new" : "new") : (f->fmt ? "format" : "keep");
		out("PLAN\t%s\t%s\t%s\t%s\t%s", act, f->key, sz, f->fmt ? f->fs : (f->fsnow[0] ? f->fsnow : "-"),
		    f->mp[0] ? f->mp : "-");
	}
	out("OK");
}

/* ---- the protocol loop ------------------------------------------------------------------------ */

static void setcfg(char *key, char *val)
{
	if (!strcmp(key, "mode"))
		snprintf(cfg.mode, sizeof cfg.mode, "%s", val);
	else if (!strcmp(key, "disk"))
		snprintf(cfg.disk, sizeof cfg.disk, "%s", val);
	else if (!strcmp(key, "user"))
		snprintf(cfg.user, sizeof cfg.user, "%s", val);
	else if (!strcmp(key, "fullname"))
		snprintf(cfg.fullname, sizeof cfg.fullname, "%s", val);
	else if (!strcmp(key, "password"))
		snprintf(cfg.password, sizeof cfg.password, "%s", val);
	else if (!strcmp(key, "lang"))
		snprintf(cfg.lang, sizeof cfg.lang, "%s", val);
	else if (!strcmp(key, "keyboard"))
		snprintf(cfg.keyboard, sizeof cfg.keyboard, "%s", val);
	else if (!strcmp(key, "tz"))
		snprintf(cfg.tz, sizeof cfg.tz, "%s", val);
	else {
		err("unknown setting");
		return;
	}
	out("OK");
}

static void cmd_part(char *args)
{
	struct op o = {0};
	char *a[8];
	int n = 0;
	for (char *s = strtok(args, " \t"); s && n < 8; s = strtok(NULL, " \t"))
		a[n++] = s;
	if (!n) {
		err("part what?");
		return;
	}
	if (nops >= MAXOP) {
		err("too many changes");
		return;
	}
	if (!strcmp(a[0], "reset")) {
		nops = 0;
		out("OK");
		return;
	}
	if (!strcmp(a[0], "newtable") && n == 1) {
		o.k = OP_NEWTABLE;
	} else if (!strcmp(a[0], "delete") && n == 2) {
		o.k = OP_DELETE;
		snprintf(o.a, sizeof o.a, "%s", a[1]);
	} else if (!strcmp(a[0], "create") && n == 4) {
		o.k = OP_CREATE;
		o.start = atoll(a[1]);
		o.size = atoll(a[2]);
		snprintf(o.kind, sizeof o.kind, "%s", a[3]);
	} else if (!strcmp(a[0], "format") && n >= 3) {
		o.k = OP_FORMAT;
		snprintf(o.a, sizeof o.a, "%s", a[1]);
		snprintf(o.fs, sizeof o.fs, "%s", a[2]);
		if (n > 3)
			snprintf(o.label, sizeof o.label, "%s", a[3]);
	} else if (!strcmp(a[0], "mount") && n == 3) {
		o.k = OP_MOUNT;
		snprintf(o.a, sizeof o.a, "%s", a[1]);
		snprintf(o.mp, sizeof o.mp, "%s", a[2]);
	} else {
		err("bad part command");
		return;
	}
	ops[nops++] = o;
	out("OK");
}

static void usage(void)
{
	fprintf(stderr,
	        "usage: flinstall-helper [--dry-run] [--image FILE [--source DIR] [--kernel FILE] [--iwd DIR] [--workdir DIR]]\n"
	        "Reads commands on stdin (see /usr/share/doc/flinstall/README), answers on stdout.\n");
}

static int live_system(void)
{
	FILE *f = fopen("/proc/mounts", "r");
	char line[512], mp[256], type[64], src[256];
	int rom = 0, ovl = 0;
	while (f && fgets(line, sizeof line, f))
		if (sscanf(line, "%255s %255s %63s", src, mp, type) == 3) {
			if (!strcmp(mp, "/run/live/rom") && !strcmp(type, "squashfs"))
				rom = 1;
			if (!strcmp(mp, "/") && !strcmp(type, "overlay"))
				ovl = 1;
		}
	if (f)
		fclose(f);
	return rom && ovl;
}

int main(int argc, char **argv)
{
	elevated = geteuid() != getuid();
	if (elevated) {
		/* a clean world: nothing the caller set is trusted */
		clearenv();
		setenv("PATH", "/usr/sbin:/usr/bin:/sbin:/bin", 1);
		umask(022);
		for (int fd = 3; fd < 64; fd++)
			close(fd);
	}
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "--dry-run"))
			dry = 1;
		else if (!strcmp(a, "--image") && i + 1 < argc)
			snprintf(image_path, sizeof image_path, "%s", argv[++i]);
		else if (!strcmp(a, "--source") && i + 1 < argc)
			snprintf(source_root, sizeof source_root, "%s", argv[++i]);
		else if (!strcmp(a, "--kernel") && i + 1 < argc)
			snprintf(kernel_arg, sizeof kernel_arg, "%s", argv[++i]);
		else if (!strcmp(a, "--iwd") && i + 1 < argc)
			snprintf(iwd_arg, sizeof iwd_arg, "%s", argv[++i]);
		else if (!strcmp(a, "--workdir") && i + 1 < argc)
			snprintf(work_arg, sizeof work_arg, "%s", argv[++i]);
		else {
			usage();
			return 2;
		}
	}
	const char *fk = getenv("FLINSTALL_FAKE");
	if (fk && *fk)
		snprintf(fake_dir, sizeof fake_dir, "%s", fk);
	if (elevated && (image_path[0] || fake_dir[0] || strcmp(source_root, "/run/live/rom") || kernel_arg[0] || iwd_arg[0] ||
	                 work_arg[0])) {
		fprintf(stderr, "flinstall-helper: the test options are not available when run with extra privileges\n");
		return 2;
	}
	if (fake_dir[0] && !dry) {
		fprintf(stderr, "flinstall-helper: FLINSTALL_FAKE only works with --dry-run\n");
		return 2;
	}
	/* Run as root all the way: a setuid program has euid 0 but the caller's real uid, and the
	 * shell scripts it starts (flboot-install) drop the effective uid back to the real one, so
	 * they could not even make a directory in the root-only work directory. All the options
	 * that could redirect the helper were refused above for the elevated case. */
	if (elevated && geteuid() == 0 && setuid(0) < 0) {
		perror("flinstall-helper: setuid");
		return 1;
	}
	if (image_path[0]) {
		struct stat st;
		if (stat(image_path, &st) < 0 || !S_ISREG(st.st_mode)) {
			fprintf(stderr, "flinstall-helper: --image needs an existing regular file (never a device)\n");
			return 2;
		}
	}
	real_mode = !image_path[0] && !fake_dir[0];
	if (real_mode && !dry) {
		if (geteuid() != 0) {
			fprintf(stderr, "flinstall-helper: must run as root (it is installed setuid root for the group seat)\n");
			return 1;
		}
		if (!live_system()) {
			fprintf(stderr, "flinstall-helper: this is not the live system; refusing to touch disks\n");
			return 1;
		}
	}
	signal(SIGPIPE, SIG_IGN);
	out("HELLO\t%s\t%s", dry ? "dry-run" : "real", image_path[0] ? "image" : (fake_dir[0] ? "fake" : "disk"));
	char line[1024];
	while (fgets(line, sizeof line, stdin)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!strcmp(line, "disks"))
			cmd_disks();
		else if (!strncmp(line, "set ", 4)) {
			char *k = line + 4, *v = strchr(k, ' ');
			if (v)
				*v++ = 0;
			else
				v = k + strlen(k);
			setcfg(k, v);
		} else if (!strncmp(line, "part ", 5))
			cmd_part(line + 5);
		else if (!strcmp(line, "check"))
			cmd_check();
		else if (!strcmp(line, "install")) {
			if (!dry && real_mode && !live_system()) {
				out("FAIL\tThis is not the live system.");
				continue;
			}
			do_install();
			break;
		} else if (!strcmp(line, "quit"))
			break;
		else if (line[0])
			err("unknown command");
	}
	explicit_bzero(cfg.password, sizeof cfg.password);
	explicit_bzero(line, sizeof line);
	return 0;
}
