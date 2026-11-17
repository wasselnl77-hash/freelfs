/* dmlinear NAME DEV -- a device-mapper "linear" device over all of DEV (test helper for build/m20-features.sh).
 * A Ventoy boot maps the ISO with device-mapper over the stick; this makes the same holder relationship
 * (/sys/block/sda/holders/dm-0) in the QEMU test without Ventoy. Built statically on the host by the test script,
 * not part of the system.  SPDX-License-Identifier: MIT */
#include <fcntl.h>
#include <linux/dm-ioctl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void init(struct dm_ioctl *d, size_t size, const char *name, unsigned flags)
{
	memset(d, 0, size);
	d->version[0] = 4;
	d->version[1] = 0;
	d->version[2] = 0;
	d->data_size = (unsigned)size;
	d->data_start = sizeof *d;
	d->flags = flags;
	if (name)
		snprintf(d->name, sizeof d->name, "%s", name);
}

int main(int argc, char **argv)
{
	if (argc != 3)
		return 2;
	int fd = open("/dev/mapper/control", O_RDWR);
	if (fd < 0) {
		perror("/dev/mapper/control");
		return 1;
	}
	int bd = open(argv[2], O_RDONLY);
	unsigned long long bytes = 0;
	if (bd < 0 || ioctl(bd, BLKGETSIZE64, &bytes) < 0) {
		perror(argv[2]);
		return 1;
	}
	char buf[4096];
	struct dm_ioctl *d = (struct dm_ioctl *)buf;
	init(d, sizeof buf, argv[1], 0);
	if (ioctl(fd, DM_DEV_CREATE, d) < 0) {
		perror("DM_DEV_CREATE");
		return 1;
	}
	init(d, sizeof buf, argv[1], 0);
	d->target_count = 1;
	struct dm_target_spec *t = (struct dm_target_spec *)(buf + sizeof *d);
	memset(t, 0, sizeof *t);
	t->sector_start = 0;
	t->length = bytes / 512;
	snprintf(t->target_type, sizeof t->target_type, "linear");
	char *params = (char *)(t + 1);
	int n = snprintf(params, 256, "%s 0", argv[2]) + 1;
	t->next = (unsigned)((sizeof *t + (size_t)n + 7) & ~7u);
	if (ioctl(fd, DM_TABLE_LOAD, d) < 0) {
		perror("DM_TABLE_LOAD");
		return 1;
	}
	init(d, sizeof buf, argv[1], 0);
	if (ioctl(fd, DM_DEV_SUSPEND, d) < 0) {	/* without DM_SUSPEND_FLAG: resume, the table goes live */
		perror("DM_DEV_SUSPEND");
		return 1;
	}
	puts("ok");
	return 0;
}
