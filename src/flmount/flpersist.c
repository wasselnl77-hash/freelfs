/* flpersist -- keep the live session's files (/home) on the boot stick.
 *
 *   flpersist create SIZE [--btrfs]    makes freelfs-persist.img (SIZE: 512M, 4G ...) on the stick's data
 *                                      partition and puts an ext4 (or btrfs) file system in it
 *   flpersist status                   "active", "ready" (the image exists, used from the next boot) or "none"
 *
 * At the next boot flinitrd finds the file next to the ISO and mounts it as /home. Without the file
 * everything stays in RAM. Only for a live session started from an ISO file on a stick (Ventoy): a stick written
 * with dd has no file system to hold the image. Installed setuid root for the group seat, like flmount.
 * SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef BLKROSET
#define BLKROSET _IO(0x12, 93)
#endif
#define SRC "/run/live/src"
#define IMG SRC "/freelfs-persist.img"

static int fail(const char *fmt, const char *a)
{
	fputs("flpersist: ", stderr);
	fprintf(stderr, fmt, a);
	fputc('\n', stderr);
	return 1;
}

static int mounted(const char *dir, char *src, size_t cap)
{
	FILE *f = fopen("/proc/self/mountinfo", "r");
	char line[4300];
	int hit = 0;
	while (f && fgets(line, sizeof line, f)) {
		char mp[PATH_MAX], s[PATH_MAX], fs[32];
		char *dash = strstr(line, " - ");
		if (!dash || sscanf(line, "%*s %*s %*s %*s %4095s", mp) != 1)
			continue;
		if (strcmp(mp, dir))
			continue;
		if (sscanf(dash + 3, "%31s %4095s", fs, s) == 2) {
			if (src)
				snprintf(src, cap, "%s", s);
			hit = 1;
		}
	}
	if (f)
		fclose(f);
	return hit;
}

static int parse_size(const char *s, unsigned long long *out)
{
	char *end;
	unsigned long long v = strtoull(s, &end, 10);
	if (end == s || v == 0)
		return -1;
	switch (toupper((unsigned char)*end)) {
	case 0: break;
	case 'K': v <<= 10; end++; break;
	case 'M': v <<= 20; end++; break;
	case 'G': v <<= 30; end++; break;
	case 'T': v <<= 40; end++; break;
	default: return -1;
	}
	if (*end == 'B' || *end == 'b' || *end == 'i')
		end++;
	if (*end == 'B' || *end == 'b')
		end++;
	if (*end)
		return -1;
	*out = v;
	return 0;
}

int main(int argc, char **argv)
{
	for (int fd = 3; fd < 256; fd++)
		close(fd);
	if (argc == 2 && !strcmp(argv[1], "status")) {
		if (mounted("/run/live/persist", NULL, 0))
			puts("active");
		else if (access(IMG, F_OK) == 0)
			puts("ready");
		else
			puts("none");
		return 0;
	}
	if (argc < 3 || argc > 4 || strcmp(argv[1], "create") || (argc == 4 && strcmp(argv[3], "--btrfs"))) {
		fputs("usage: flpersist create SIZE [--btrfs] | status\n", stderr);
		return 2;
	}
	int btrfs = argc == 4;
	unsigned long long size;
	if (parse_size(argv[2], &size) < 0 || size < (64ull << 20))
		return fail("%s: not a size of at least 64M (use 4G, 512M)", argv[2]);
	if (geteuid() != 0)
		return fail("%s", "not installed setuid root (the system's file ownership was not applied)");
	if (access("/run/live", F_OK) != 0)
		return fail("%s", "this is not a live session");
	char dev[PATH_MAX] = "";
	if (!mounted(SRC, dev, sizeof dev) || strncmp(dev, "/dev/", 5))
		return fail("%s", "this live session did not start from an ISO file on a stick (Ventoy), so there is no place for the image");
	if (access(IMG, F_OK) == 0)
		return fail("%s", "freelfs-persist.img exists already (remove it from the stick to make a new one)");
	/* the stick is read-only since boot: make it writable */
	int fd = open(dev, O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		int zero = 0;
		(void)!ioctl(fd, BLKROSET, &zero);
		close(fd);
	}
	if (mount(NULL, SRC, NULL, MS_REMOUNT | MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0)
		return fail("cannot make the stick writable: %s", strerror(errno));
	struct statvfs sv;
	if (statvfs(SRC, &sv) < 0)
		return fail("cannot read the stick: %s", strerror(errno));
	unsigned long long freeb = (unsigned long long)sv.f_bavail * sv.f_frsize;
	if (freeb < size + (32ull << 20)) {
		char m[64];
		snprintf(m, sizeof m, "%llu MB", freeb >> 20);
		return fail("not enough room on the stick (only %s free)", m);
	}
	int out = open(IMG, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (out < 0)
		return fail("cannot create the image: %s", strerror(errno));
	int rc = posix_fallocate(out, 0, (off_t)size);
	if (rc != 0 && ftruncate(out, (off_t)size) < 0) {
		close(out);
		unlink(IMG);
		return fail("cannot make room for the image: %s", strerror(rc));
	}
	fsync(out);
	close(out);
	pid_t pid = fork();
	if (pid == 0) {
		clearenv();
		setenv("PATH", "/usr/sbin:/usr/bin", 1);
		int nul = open("/dev/null", O_RDONLY);
		if (nul >= 0)
			dup2(nul, 0);
		if (btrfs)
			execlp("mkfs.btrfs", "mkfs.btrfs", "-q", "-f", "-L", "FLPERSIST", IMG, (char *)NULL);
		else
			execlp("mkfs.ext4", "mkfs.ext4", "-q", "-F", "-m", "0", "-L", "FLPERSIST", IMG, (char *)NULL);
		_exit(127);
	}
	int st = 0;
	if (pid < 0 || waitpid(pid, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
		unlink(IMG);
		return fail("%s", "mkfs failed; the image was removed");
	}
	sync();
	puts("Created freelfs-persist.img on the stick. Restart to keep your files (/home) between sessions.");
	return 0;
}
