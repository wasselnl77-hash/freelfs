/* flmount -- mount, unmount and eject removable drives for the person at the machine.
 *
 * Installed setuid root for the group seat (mode 4750), like flpower and flpkg-seat. Nothing here takes a
 * path from the caller: a device is named /dev/NAME and must be one the kernel lists in /sys/class/block.
 *
 *   flmount list [--internal]        one line per partition, TAB separated:
 *                                    DEV  LABEL  FSTYPE  SIZE_BYTES  MOUNTPOINT  CLASS
 *                                    "-" stands for no label, no file system, not mounted;
 *                                    CLASS is removable, boot (the live stick) or internal
 *   flmount mount DEV                mounts at /drives/LABEL (the label made safe), prints the mount point on
 *                                    stdout, exit 0. vfat, exfat and ntfs get the caller's uid and gid; on ext4
 *                                    and btrfs the top directory is given to the caller when root owns it.
 *                                    The live boot stick is made writable and bound to /drives/LABEL.
 *   flmount unmount DEV              unmounts what is under /drives (the boot stick stays mounted for the system)
 *   flmount eject DEV                unmounts every partition of the disk and powers the device off (not the
 *                                    boot stick: the live system runs from it)
 *   --internal                       (list, mount, unmount, eject) also internal disks; Files does not use it
 *
 * Root only (udev, rc.sysinit):
 *   flmount --hotplug DEV            a partition appeared: mount it, tell the person in the session
 *                                    "USB stick 'LABEL' connected" (flnotify, as that user); a click on Open
 *                                    starts Files at the mount point. Returns at once, the work is in the background.
 *   flmount --auto                   mount every removable partition there is, and bind the boot stick (boot time)
 *   flmount --gone DEV               the device was pulled: detach what is left of its mounts
 *
 * Errors go to stderr with a non-zero exit. Mounts are nosuid,nodev,noexec.
 * SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef BLKROSET
#define BLKROSET _IO(0x12, 93)
#endif

#define MAXP 128
#define LIVE_SRC "/run/live/src"
#define LIVE_ISO "/run/live/iso"

enum { C_REMOVABLE, C_BOOT, C_INTERNAL };
static const char *const class_name[] = { "removable", "boot", "internal" };

struct part {
	char name[64];		/* sdb1 */
	char disk[64];		/* sdb */
	char fs[32], label[128];
	unsigned long long size;
	int cls;
	int usb;
};

static int allow_internal;
static uid_t owner_uid;
static gid_t owner_gid;

static void err(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("flmount: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static int read_str(const char *path, char *out, size_t cap)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	if (!fgets(out, (int)cap, f))
		out[0] = 0;
	fclose(f);
	out[strcspn(out, "\r\n")] = 0;
	return 0;
}

/* ---- the kernel's and udev's view ------------------------------------------------------------- */

struct mnt {
	char src[PATH_MAX], dir[PATH_MAX], fs[32], opts[256];
};

static int read_mounts(struct mnt **out)
{
	FILE *f = fopen("/proc/self/mountinfo", "r");
	char *line = NULL;
	size_t cap = 0;
	int n = 0, room = 0;
	*out = NULL;
	while (f && getline(&line, &cap, f) > 0) {
		char *dash = strstr(line, " - ");
		if (!dash)
			continue;
		struct mnt m = { "", "", "", "" };
		/* id parent maj:min root mountpoint options ... - fstype source superopts */
		char *sv = NULL, *tok = strtok_r(line, " ", &sv);
		for (int i = 0; tok && i < 4; i++)
			tok = strtok_r(NULL, " ", &sv);
		if (!tok)
			continue;
		/* tok = mount point, with \040 style escapes */
		char *d = m.dir;
		for (const char *s = tok; *s && d < m.dir + sizeof m.dir - 1; s++) {
			if (s[0] == '\\' && isdigit((unsigned char)s[1]) && isdigit((unsigned char)s[2]) && isdigit((unsigned char)s[3])) {
				*d++ = (char)(((s[1] - '0') << 6) | ((s[2] - '0') << 3) | (s[3] - '0'));
				s += 3;
			} else
				*d++ = *s;
		}
		*d = 0;
		sscanf(dash + 3, "%31s %4095s %255s", m.fs, m.src, m.opts);
		if (n == room) {
			room = room ? room * 2 : 64;
			struct mnt *nm = realloc(*out, (size_t)room * sizeof **out);
			if (!nm)
				break;
			*out = nm;
		}
		(*out)[n++] = m;
	}
	if (f)
		fclose(f);
	free(line);
	return n;
}

/* the udev database entry of a device: ID_FS_TYPE, ID_FS_LABEL, ID_BUS */
static void udev_props(dev_t dev, char *fs, size_t fscap, char *label, size_t lcap, char *bus, size_t bcap)
{
	char path[64], line[512];
	snprintf(path, sizeof path, "/run/udev/data/b%u:%u", major(dev), minor(dev));
	FILE *f = fopen(path, "r");
	while (f && fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = 0;
		if (!strncmp(line, "E:ID_FS_TYPE=", 13))
			snprintf(fs, fscap, "%s", line + 13);
		else if (!strncmp(line, "E:ID_FS_LABEL=", 14))
			snprintf(label, lcap, "%s", line + 14);
		else if (!strncmp(line, "E:ID_BUS=", 9))
			snprintf(bus, bcap, "%s", line + 9);
	}
	if (f)
		fclose(f);
}

/* the file system of a device the udev database does not know (yet): blkid, with a clean environment */
static void blkid_props(const char *dev, char *fs, size_t fscap, char *label, size_t lcap)
{
	int p[2];
	if (pipe(p) < 0)
		return;
	pid_t pid = fork();
	if (pid == 0) {
		dup2(p[1], 1);
		close(p[0]);
		close(p[1]);
		int nul = open("/dev/null", O_WRONLY);
		if (nul >= 0)
			dup2(nul, 2);
		clearenv();
		setenv("PATH", "/usr/sbin:/usr/bin", 1);
		execlp("blkid", "blkid", "-p", "-o", "export", dev, (char *)NULL);
		_exit(127);
	}
	close(p[1]);
	if (pid > 0) {
		char buf[2048];
		ssize_t n = read(p[0], buf, sizeof buf - 1);
		if (n > 0) {
			buf[n] = 0;
			char *sv = NULL;
			for (char *l = strtok_r(buf, "\n", &sv); l; l = strtok_r(NULL, "\n", &sv)) {
				if (!strncmp(l, "TYPE=", 5))
					snprintf(fs, fscap, "%s", l + 5);
				else if (!strncmp(l, "LABEL=", 6))
					snprintf(label, lcap, "%s", l + 6);
			}
		}
		int st;
		waitpid(pid, &st, 0);
	}
	close(p[0]);
}

/* the device the live system runs from (mounted at /run/live/src or /run/live/iso), "" when this is no live system */
static void boot_device(char *out, size_t cap)
{
	out[0] = 0;
	struct mnt *m;
	int n = read_mounts(&m);
	for (int i = 0; i < n; i++)
		if ((!strcmp(m[i].dir, LIVE_SRC) || !strcmp(m[i].dir, LIVE_ISO)) && !strncmp(m[i].src, "/dev/", 5) &&
		    strncmp(m[i].src, "/dev/loop", 9))
			snprintf(out, cap, "%s", m[i].src);
	free(m);
}

static int sys_is_disk_with_parts(const char *name)
{
	char path[PATH_MAX];
	snprintf(path, sizeof path, "/sys/class/block/%s", name);
	DIR *d = opendir(path);
	int r = 0;
	struct dirent *e;
	while (d && (e = readdir(d))) {
		char q[PATH_MAX];
		snprintf(q, sizeof q, "/sys/class/block/%s/%s/partition", name, e->d_name);
		if (e->d_name[0] != '.' && !access(q, F_OK))
			r = 1;
	}
	if (d)
		closedir(d);
	return r;
}

static int name_ok(const char *n)
{
	if (!*n || strlen(n) > 40)
		return 0;
	for (; *n; n++)
		if (!islower((unsigned char)*n) && !isdigit((unsigned char)*n))
			return 0;
	return 1;
}

/* Fill P for the block device NAME; 0 when it is not something to offer (no partition, loop, ram ...). */
static int describe(const char *name, struct part *p, const char *boot_dev)
{
	if (!name_ok(name) || !strncmp(name, "loop", 4) || !strncmp(name, "ram", 3) || !strncmp(name, "zram", 4) ||
	    !strncmp(name, "dm", 2) || !strncmp(name, "md", 2) || !strncmp(name, "sr", 2) || !strncmp(name, "nbd", 3))
		return 0;
	memset(p, 0, sizeof *p);
	snprintf(p->name, sizeof p->name, "%s", name);
	char path[PATH_MAX], real[PATH_MAX], q[PATH_MAX];
	snprintf(path, sizeof path, "/sys/class/block/%s", name);
	if (!realpath(path, real))
		return 0;
	snprintf(q, sizeof q, "%s/partition", real);
	int ispart = access(q, F_OK) == 0;
	if (ispart) {
		char *slash = strrchr(real, '/');
		char parent[PATH_MAX];
		snprintf(parent, sizeof parent, "%.*s", (int)(slash - real), real);
		const char *dn = strrchr(parent, '/');
		snprintf(p->disk, sizeof p->disk, "%s", dn ? dn + 1 : parent);
	} else {
		if (sys_is_disk_with_parts(name))
			return 0;	/* the partitions are the rows */
		snprintf(p->disk, sizeof p->disk, "%s", name);
	}
	char b[64];
	snprintf(q, sizeof q, "%s/size", real);
	p->size = read_str(q, b, sizeof b) == 0 ? strtoull(b, NULL, 10) * 512ull : 0;
	if (p->size == 0)
		return 0;
	char dp[PATH_MAX], bus[32] = "";
	snprintf(dp, sizeof dp, "/dev/%s", name);
	struct stat st;
	if (stat(dp, &st) < 0 || !S_ISBLK(st.st_mode))
		return 0;
	udev_props(st.st_rdev, p->fs, sizeof p->fs, p->label, sizeof p->label, bus, sizeof bus);
	if (!p->fs[0])
		blkid_props(dp, p->fs, sizeof p->fs, p->label, sizeof p->label);
	/* removable: the disk says so, or it hangs off the USB bus */
	char rpath[PATH_MAX];
	snprintf(rpath, sizeof rpath, "/sys/class/block/%s/removable", p->disk);
	int removable = read_str(rpath, b, sizeof b) == 0 && b[0] == '1';
	p->usb = !strcmp(bus, "usb") || strstr(real, "/usb") != NULL;
	p->cls = removable || p->usb ? C_REMOVABLE : C_INTERNAL;
	if (boot_dev && boot_dev[0] && !strcmp(dp, boot_dev))
		p->cls = C_BOOT;
	/* the boot stick's helper partitions (Ventoy's VTOYEFI, the ISO's FLLIVE) are not drives to browse */
	if (p->cls == C_REMOVABLE && (!strcmp(p->label, "VTOYEFI") || !strcmp(p->label, "FLLIVE")))
		p->cls = C_INTERNAL;
	return 1;
}

static int enumerate(struct part *out, int max)
{
	char boot[256];
	boot_device(boot, sizeof boot);
	DIR *d = opendir("/sys/class/block");
	int n = 0;
	struct dirent *e;
	while (d && (e = readdir(d)) && n < max) {
		if (e->d_name[0] == '.')
			continue;
		if (describe(e->d_name, &out[n], boot))
			n++;
	}
	if (d)
		closedir(d);
	/* by name */
	for (int i = 1; i < n; i++)
		for (int j = i; j > 0 && strcmp(out[j - 1].name, out[j].name) > 0; j--) {
			struct part t = out[j];
			out[j] = out[j - 1];
			out[j - 1] = t;
		}
	return n;
}

/* ---- mounting ------------------------------------------------------------------------------------ */

/* the drive's mount point for the list: one under /drives first, else any */
static const char *mountpoint_of(const struct mnt *m, int nm, const char *dev, char *buf, size_t cap)
{
	buf[0] = 0;
	for (int i = 0; i < nm; i++) {
		if (strcmp(m[i].src, dev))
			continue;
		if (!strncmp(m[i].dir, "/drives/", 8)) {
			snprintf(buf, cap, "%s", m[i].dir);
			return buf;
		}
		if (!buf[0])
			snprintf(buf, cap, "%s", m[i].dir);
	}
	return buf[0] ? buf : NULL;
}

static int cmd_list(void)
{
	struct part *v = calloc(MAXP, sizeof *v);
	if (!v) {
		err("out of memory");
		return 1;
	}
	int n = enumerate(v, MAXP);
	struct mnt *m;
	int nm = read_mounts(&m);
	for (int i = 0; i < n; i++) {
		if (v[i].cls == C_INTERNAL && !allow_internal)
			continue;
		char dev[80], mp[PATH_MAX];
		snprintf(dev, sizeof dev, "/dev/%s", v[i].name);
		const char *mpp = mountpoint_of(m, nm, dev, mp, sizeof mp);
		const char *label = v[i].label[0] ? v[i].label : "-";
		/* no TAB or newline can sit in a label */
		char lab[160];
		snprintf(lab, sizeof lab, "%s", label);
		for (char *s = lab; *s; s++)
			if (*s == '\t' || *s == '\n' || *s == '\r')
				*s = ' ';
		printf("%s\t%s\t%s\t%llu\t%s\t%s\n", dev, lab, v[i].fs[0] ? v[i].fs : "-", v[i].size, mpp ? mpp : "-",
		       class_name[v[i].cls]);
	}
	free(m);
	free(v);
	return 0;
}

/* DEV as the caller gave it -> its entry; NULL (and a message) when it is not on offer */
static struct part *find_part(struct part *v, int n, const char *dev)
{
	if (strncmp(dev, "/dev/", 5) || strchr(dev + 5, '/') || !name_ok(dev + 5)) {
		err("%s: not a device name (use /dev/sdb1)", dev);
		return NULL;
	}
	for (int i = 0; i < n; i++)
		if (!strcmp(v[i].name, dev + 5)) {
			if (v[i].cls == C_INTERNAL && !allow_internal) {
				err("%s is not a removable drive", dev);
				return NULL;
			}
			return &v[i];
		}
	err("%s: no such partition", dev);
	return NULL;
}

/* a mount point name from the label: letters, digits, . _ - (a space becomes _), at most 40 characters */
static void safe_name(const struct part *p, char *out, size_t cap)
{
	size_t k = 0;
	for (const char *s = p->label; *s && k < 40 && k + 1 < cap; s++) {
		unsigned char c = (unsigned char)*s;
		if (isalnum(c) || c == '.' || c == '_' || c == '-')
			out[k++] = (char)c;
		else if (c == ' ')
			out[k++] = '_';
	}
	out[k] = 0;
	if (!k || !strcmp(out, ".") || !strcmp(out, "..") || out[0] == '.')
		snprintf(out, cap, "%s", p->name);
}

static int set_rw(const char *dev)
{
	int fd = open(dev, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	int zero = 0, rc = ioctl(fd, BLKROSET, &zero);
	close(fd);
	return rc;
}

/* a free /drives/NAME: made, or the same name when it is ours already (not mounted) */
static int make_mountpoint(const char *name, char *out, size_t cap)
{
	mkdir("/drives", 0755);
	struct mnt *m;
	int nm = read_mounts(&m);
	for (int tries = 1; tries < 20; tries++) {
		if (tries == 1)
			snprintf(out, cap, "/drives/%s", name);
		else
			snprintf(out, cap, "/drives/%s-%d", name, tries);
		int used = 0;
		for (int i = 0; i < nm; i++)
			if (!strcmp(m[i].dir, out))
				used = 1;
		if (used)
			continue;
		struct stat st;
		if (lstat(out, &st) == 0) {
			if (!S_ISDIR(st.st_mode)) {
				continue;
			}
			free(m);
			return 0;
		}
		if (mkdir(out, 0755) == 0) {
			free(m);
			return 0;
		}
		if (errno != EEXIST) {
			err("cannot make %s: %s", out, strerror(errno));
			free(m);
			return -1;
		}
	}
	free(m);
	err("no free place under /drives for %s", name);
	return -1;
}

static int is_listed_ok_fs(const char *fs)
{
	static const char *const ok[] = { "vfat", "exfat", "ext2", "ext3", "ext4", "btrfs", "iso9660", "udf", "xfs", "f2fs", NULL };
	for (int i = 0; ok[i]; i++)
		if (!strcmp(fs, ok[i]))
			return 1;
	return 0;
}

static int lockfd = -1;

/* one mounting at a time (udev's hotplug and rc.sysinit's --auto can meet) */
static void lock_mounts(void)
{
	if (lockfd < 0)
		lockfd = open("/run/flmount.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (lockfd >= 0)
		(void)!flock(lockfd, LOCK_EX);
}

/* 0: mounted now, 2: it was mounted already (MP says where), -1: failed */
static int do_mount(const struct part *p, char *mp, size_t mpcap)
{
	lock_mounts();
	char dev[80], cur[PATH_MAX];
	snprintf(dev, sizeof dev, "/dev/%s", p->name);
	struct mnt *m;
	int nm = read_mounts(&m);
	/* already under /drives */
	for (int i = 0; i < nm; i++)
		if (!strcmp(m[i].src, dev) && !strncmp(m[i].dir, "/drives/", 8)) {
			snprintf(mp, mpcap, "%s", m[i].dir);
			free(m);
			return 2;
		}
	char name[64];
	safe_name(p, name, sizeof name);
	if (p->cls == C_BOOT) {
		/* the live stick: writable, and bound where the other drives are */
		const char *src = LIVE_SRC;
		for (int i = 0; i < nm; i++)
			if (!strcmp(m[i].src, dev) && !strcmp(m[i].dir, LIVE_ISO) && strcmp(m[i].dir, LIVE_SRC))
				src = LIVE_ISO;	/* a dd'd ISO: it can only be read */
		free(m);
		if (!strcmp(src, LIVE_SRC)) {
			char whole[80];
			snprintf(whole, sizeof whole, "/dev/%s", p->disk);
			(void)set_rw(whole);	/* a partition stays read-only while its disk is */
			if (set_rw(dev) < 0)
				err("%s: cannot make the device writable: %s", dev, strerror(errno));
			if (mount(NULL, LIVE_SRC, NULL, MS_REMOUNT | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0) {
				err("cannot make the boot stick writable: %s", strerror(errno));
				return -1;
			}
		}
		if (make_mountpoint(name, mp, mpcap) < 0)
			return -1;
		if (mount(src, mp, NULL, MS_BIND, NULL) < 0) {
			err("cannot bind %s to %s: %s", src, mp, strerror(errno));
			rmdir(mp);
			return -1;
		}
		mount(NULL, mp, NULL, MS_BIND | MS_REMOUNT | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
		return 0;
	}
	free(m);
	(void)cur;
	if (!p->fs[0] || !is_listed_ok_fs(p->fs)) {
		err("%s: cannot mount a '%s' file system", dev, p->fs[0] ? p->fs : "unknown");
		return -1;
	}
	(void)set_rw(dev);	/* a device flagged read-only by an old live boot: clear it */
	if (make_mountpoint(name, mp, mpcap) < 0)
		return -1;
	char opts[256] = "";
	unsigned long flags = MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_NOATIME;
	if (!strcmp(p->fs, "vfat"))
		snprintf(opts, sizeof opts, "uid=%u,gid=%u,fmask=0133,dmask=0022,iocharset=utf8,shortname=mixed,flush,quiet",
			 (unsigned)owner_uid, (unsigned)owner_gid);
	else if (!strcmp(p->fs, "exfat"))
		snprintf(opts, sizeof opts, "uid=%u,gid=%u,fmask=0133,dmask=0022,iocharset=utf8", (unsigned)owner_uid,
			 (unsigned)owner_gid);
	else if (!strcmp(p->fs, "iso9660") || !strcmp(p->fs, "udf")) {
		flags |= MS_RDONLY;
		snprintf(opts, sizeof opts, "uid=%u,gid=%u", (unsigned)owner_uid, (unsigned)owner_gid);
	}
	int rc = mount(dev, mp, p->fs, flags, opts);
	if (rc < 0 && errno == EROFS) {	/* a write-protected medium: read only then */
		rc = mount(dev, mp, p->fs, flags | MS_RDONLY, opts);
	}
	if (rc < 0 && !strcmp(p->fs, "vfat") && errno == EINVAL)
		rc = mount(dev, mp, p->fs, flags, "uid=0,quiet") == 0 ? 0 : -1;
	if (rc < 0) {
		err("cannot mount %s on %s: %s", dev, mp, strerror(errno));
		rmdir(mp);
		return -1;
	}
	if (!strncmp(p->fs, "ext", 3) || !strcmp(p->fs, "btrfs") || !strcmp(p->fs, "xfs") || !strcmp(p->fs, "f2fs")) {
		struct stat st;
		if (stat(mp, &st) == 0 && st.st_uid == 0 && owner_uid != 0 && !(flags & MS_RDONLY))
			(void)!chown(mp, owner_uid, owner_gid);	/* else the drive stays root's, readable */
	}
	return 0;
}

static int cmd_mount(const char *dev)
{
	struct part *v = calloc(MAXP, sizeof *v);
	int n = v ? enumerate(v, MAXP) : 0;
	struct part *p = v ? find_part(v, n, dev) : NULL;
	int rc = 1;
	if (p) {
		char mp[PATH_MAX];
		if (do_mount(p, mp, sizeof mp) >= 0) {
			printf("%s\n", mp);
			rc = 0;
		}
	}
	free(v);
	return rc;
}

/* unmount everything of DEV under /drives; lazy (the device is gone) or not */
static int unmount_dev(const char *dev, int lazy, int *count)
{
	int rc = 0;
	for (int round = 0; round < 8; round++) {
		struct mnt *m;
		int nm = read_mounts(&m), did = 0;
		for (int i = nm - 1; i >= 0; i--) {
			if (strcmp(m[i].src, dev) || strncmp(m[i].dir, "/drives/", 8))
				continue;
			if (umount2(m[i].dir, lazy ? MNT_DETACH : 0) < 0) {
				err("cannot unmount %s: %s", m[i].dir, errno == EBUSY ? "in use (close the windows that show it)" : strerror(errno));
				rc = 1;
			} else {
				rmdir(m[i].dir);
				did++;
				if (count)
					(*count)++;
			}
		}
		free(m);
		if (!did || rc)
			break;
	}
	return rc;
}

static int cmd_unmount(const char *dev)
{
	struct part *v = calloc(MAXP, sizeof *v);
	int n = v ? enumerate(v, MAXP) : 0;
	struct part *p = v ? find_part(v, n, dev) : NULL;
	int rc = 1, cnt = 0;
	if (p) {
		sync();
		rc = unmount_dev(dev, 0, &cnt);
		if (!rc && !cnt)
			err("%s is not mounted under /drives", dev);
		if (!cnt && !rc)
			rc = 1;
	}
	free(v);
	return rc;
}

static void write_file(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd >= 0) {
		(void)!write(fd, val, strlen(val));
		close(fd);
	}
}

static int cmd_eject(const char *dev)
{
	struct part *v = calloc(MAXP, sizeof *v);
	int n = v ? enumerate(v, MAXP) : 0;
	struct part *p = v ? find_part(v, n, dev) : NULL;
	int rc = 1;
	if (!p)
		goto out;
	if (p->cls == C_BOOT) {
		err("%s is the stick the live system runs from; shut down instead", dev);
		goto out;
	}
	char disk[PATH_MAX];
	snprintf(disk, sizeof disk, "/dev/%s", p->disk);
	sync();
	int bad = 0;
	for (int i = 0; i < n; i++) {
		if (strcmp(v[i].disk, p->disk))
			continue;
		if (v[i].cls == C_BOOT) {
			err("the live system runs from this disk");
			goto out;
		}
		char d[80];
		snprintf(d, sizeof d, "/dev/%s", v[i].name);
		if (unmount_dev(d, 0, NULL))
			bad = 1;
	}
	if (bad)
		goto out;
	/* flush the device's cache, then remove it from the SCSI layer (the kernel stops the unit) and, for USB, cut it off */
	int fd = open(disk, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
	char link[PATH_MAX], real[PATH_MAX], auth[PATH_MAX] = "";
	snprintf(link, sizeof link, "/sys/class/block/%s", p->disk);
	if (realpath(link, real)) {
		/* the nearest ancestor with an "authorized" file is the USB device */
		for (char *s = real + strlen(real); s > real + 1;) {
			while (s > real && *s != '/')
				s--;
			*s = 0;
			char t[PATH_MAX];
			snprintf(t, sizeof t, "%s/authorized", real);
			if (!access(t, F_OK)) {
				snprintf(auth, sizeof auth, "%s", t);
				break;
			}
		}
	}
	char del[PATH_MAX];
	snprintf(del, sizeof del, "/sys/block/%s/device/delete", p->disk);
	if (access(del, W_OK) == 0)
		write_file(del, "1");
	else if (!auth[0]) {
		err("%s: cannot power it off (the kernel offers no way for this device); it is safe to unplug", disk);
		rc = 0;
		goto out;
	}
	if (auth[0]) {
		usleep(300 * 1000);
		write_file(auth, "0");
	}
	rc = 0;
out:
	free(v);
	return rc;
}

/* ---- root only: udev and boot --------------------------------------------------------------------- */

/* the user of the desktop session: a /run/user/UID with a session bus, uid 1000 and up */
static int session_user(uid_t *uid, gid_t *gid, char *name, size_t cap)
{
	DIR *d = opendir("/run/user");
	struct dirent *e;
	int found = 0;
	while (d && (e = readdir(d)) && !found) {
		char *end;
		unsigned long u = strtoul(e->d_name, &end, 10);
		if (*end || u < 1000 || u > 60000)
			continue;
		char b[64];
		snprintf(b, sizeof b, "/run/user/%lu/bus", u);
		struct passwd *pw = getpwuid((uid_t)u);
		if (access(b, F_OK) == 0 && pw) {
			*uid = pw->pw_uid;
			*gid = pw->pw_gid;
			snprintf(name, cap, "%s", pw->pw_name);
			found = 1;
		}
	}
	if (d)
		closedir(d);
	return found;
}

/* who gets the files on a drive mounted without anybody asking: the session's user, else the first normal account */
static void default_owner(void)
{
	uid_t u;
	gid_t g;
	char nm[64];
	if (session_user(&u, &g, nm, sizeof nm)) {
		owner_uid = u;
		owner_gid = g;
		return;
	}
	setpwent();
	struct passwd *pw;
	while ((pw = getpwent()))
		if (pw->pw_uid >= 1000 && pw->pw_uid < 60000) {
			owner_uid = pw->pw_uid;
			owner_gid = pw->pw_gid;
			break;
		}
	endpwent();
}

static void run_as(uid_t uid, gid_t gid, const char *name, const char *const *argv, int out_fd)
{
	if (initgroups(name, gid) < 0 || setgid(gid) < 0 || setuid(uid) < 0)
		_exit(126);
	char b[96];
	clearenv();
	setenv("PATH", "/usr/bin:/usr/sbin", 1);
	snprintf(b, sizeof b, "/run/user/%u", (unsigned)uid);
	setenv("XDG_RUNTIME_DIR", b, 1);
	snprintf(b, sizeof b, "unix:path=/run/user/%u/bus", (unsigned)uid);
	setenv("DBUS_SESSION_BUS_ADDRESS", b, 1);
	setenv("WAYLAND_DISPLAY", "wayland-0", 1);
	struct passwd *pw = getpwuid(uid);
	if (pw)
		setenv("HOME", pw->pw_dir, 1);
	if (out_fd >= 0)
		dup2(out_fd, 1);
	int nul = open("/dev/null", O_RDWR);
	if (nul >= 0) {
		dup2(nul, 0);
		dup2(nul, 2);
	}
	for (int fd = 3; fd < 256; fd++)
		close(fd);
	execvp(argv[0], (char *const *)argv);
	_exit(127);
}

/* tell the session; wait for a click on Open (up to a minute) and open Files there */
static void notify_user(const struct part *p, const char *mp)
{
	uid_t uid;
	gid_t gid;
	char user[64];
	if (!session_user(&uid, &gid, user, sizeof user))
		return;
	char summary[256];
	snprintf(summary, sizeof summary, "USB stick '%s' connected", p->label[0] ? p->label : "USB drive");
	const char *argv[] = { "flnotify", "-w", "-a", "FreeLFS", "-i", "drive-removable-media", "-A", "open=Open", summary,
			       "Open it in Files", NULL };
	int pp[2];
	if (pipe(pp) < 0)
		return;
	pid_t pid = fork();
	if (pid == 0) {
		close(pp[0]);
		run_as(uid, gid, user, argv, pp[1]);
	}
	close(pp[1]);
	if (pid < 0) {
		close(pp[0]);
		return;
	}
	char buf[256];
	size_t n = 0;
	struct pollfd pf = { pp[0], POLLIN, 0 };
	while (n < sizeof buf - 1 && poll(&pf, 1, 70 * 1000) > 0) {
		ssize_t r = read(pp[0], buf + n, sizeof buf - 1 - n);
		if (r <= 0)
			break;
		n += (size_t)r;
	}
	buf[n] = 0;
	close(pp[0]);
	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);
	if (strstr(buf, "action open")) {
		const char *fa[] = { "flfiles", mp, NULL };
		pid_t f = fork();
		if (f == 0)
			run_as(uid, gid, user, fa, -1);
		if (f > 0)
			waitpid(f, NULL, WNOHANG);
	}
}

static int cmd_hotplug(const char *dev, int quiet)
{
	if (strncmp(dev, "/dev/", 5) || !name_ok(dev + 5))
		return 1;
	if (!quiet) {
		pid_t pid = fork();	/* udev must not wait for us */
		if (pid < 0)
			return 1;
		if (pid > 0)
			return 0;
		setsid();
		if (fork() > 0)
			_exit(0);
		int nul = open("/dev/null", O_RDWR);
		if (nul >= 0) {
			dup2(nul, 0);
			dup2(nul, 1);
			dup2(nul, 2);
		}
	}
	default_owner();
	/* the udev database is written around now; give blkid a moment to have an answer */
	struct part p;
	char boot[256];
	boot_device(boot, sizeof boot);
	int ok = 0;
	for (int i = 0; i < 20 && !ok; i++) {
		ok = describe(dev + 5, &p, boot) && p.fs[0];
		if (!ok)
			usleep(250 * 1000);
	}
	if (!ok || p.cls == C_INTERNAL || !is_listed_ok_fs(p.fs) || (p.cls == C_BOOT && quiet == 2))
		return quiet ? 0 : 0;
	char mp[PATH_MAX];
	int r = do_mount(&p, mp, sizeof mp);
	if (r < 0)
		return 1;
	if (!quiet && r == 0 && p.cls == C_REMOVABLE)
		notify_user(&p, mp);
	return 0;
}

static int cmd_auto(void)
{
	default_owner();
	struct part *v = calloc(MAXP, sizeof *v);
	int n = v ? enumerate(v, MAXP) : 0;
	for (int i = 0; i < n; i++) {
		if (v[i].cls == C_INTERNAL || !v[i].fs[0])
			continue;
		if (v[i].cls == C_REMOVABLE && !is_listed_ok_fs(v[i].fs))
			continue;
		char mp[PATH_MAX];
		do_mount(&v[i], mp, sizeof mp);
	}
	free(v);
	return 0;
}

static int cmd_gone(const char *dev)
{
	if (strncmp(dev, "/dev/", 5) || !name_ok(dev + 5))
		return 1;
	unmount_dev(dev, 1, NULL);
	/* empty leftover directories */
	DIR *d = opendir("/drives");
	struct dirent *e;
	while (d && (e = readdir(d))) {
		char p[PATH_MAX];
		if (e->d_name[0] == '.')
			continue;
		snprintf(p, sizeof p, "/drives/%s", e->d_name);
		rmdir(p);	/* only succeeds for an empty, unmounted directory */
	}
	if (d)
		closedir(d);
	return 0;
}

int main(int argc, char **argv)
{
	for (int fd = 3; fd < 256; fd++)	/* the caller's status pipe (fd 3) must not stay open in anything we start */
		close(fd);
	if (geteuid() != 0) {
		err("not installed setuid root (the system's file ownership was not applied)");
		return 1;
	}
	/* a clean environment, a fixed PATH */
	clearenv();
	setenv("PATH", "/usr/sbin:/usr/bin", 1);
	umask(022);
	owner_uid = getuid();
	owner_gid = getgid();
	int a = 1;
	const char *verb = NULL, *dev = NULL;
	for (; a < argc; a++) {
		if (!strcmp(argv[a], "--internal"))
			allow_internal = 1;
		else if (!verb)
			verb = argv[a];
		else if (!dev)
			dev = argv[a];
		else
			verb = NULL, a = argc;
	}
	if (!verb) {
		fputs("usage: flmount list | mount DEV | unmount DEV | eject DEV   (--internal: also internal disks)\n", stderr);
		return 2;
	}
	int root_caller = getuid() == 0;
	if (!strcmp(verb, "list") && !dev)
		return cmd_list();
	if (!strcmp(verb, "mount") && dev)
		return cmd_mount(dev);
	if (!strcmp(verb, "unmount") && dev)
		return cmd_unmount(dev);
	if (!strcmp(verb, "eject") && dev)
		return cmd_eject(dev);
	if (root_caller && !strcmp(verb, "--hotplug") && dev)
		return cmd_hotplug(dev, 0);
	if (root_caller && !strcmp(verb, "--auto") && !dev)
		return cmd_auto();
	if (root_caller && !strcmp(verb, "--gone") && dev)
		return cmd_gone(dev);
	fputs("usage: flmount list | mount DEV | unmount DEV | eject DEV   (--internal: also internal disks)\n", stderr);
	return 2;
}
