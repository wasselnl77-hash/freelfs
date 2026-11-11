/* Write a sample archive to stdout, for checking it with other tar tools.
 * SPDX-License-Identifier: MIT */
#include "../flpkg.h"

#include <string.h>
#include <unistd.h>

static int out(void *ctx, const void *buf, size_t len)
{
	return write_all(STDOUT_FILENO, buf, len);
}

int main(void)
{
	struct tar_writer w = {out, NULL, 0, 0};
	char longpath[300];
	memset(longpath, 'p', sizeof(longpath) - 1);
	longpath[sizeof(longpath) - 1] = '\0';
	memcpy(longpath, "usr/share/", 10);
	struct tar_entry e[] = {
		{.path = "usr", .type = TAR_DIR, .mode = 0755},
		{.path = "usr/bin", .type = TAR_DIR, .mode = 0755},
		{.path = "usr/bin/wall", .type = TAR_FILE, .mode = 02755, .gid = 5, .size = 6},
		{.path = "usr/bin/write", .link = "usr/bin/wall", .type = TAR_HARDLINK, .mode = 02755},
		{.path = "usr/bin/sh", .link = "bash", .type = TAR_SYMLINK, .mode = 0777},
		{.path = longpath, .type = TAR_FILE, .mode = 0644, .uid = 3000000, .size = 6},
	};
	for (size_t i = 0; i < sizeof(e) / sizeof(e[0]); i++) {
		e[i].mtime = 1767225600;
		if (tar_add_mem(&w, &e[i], "hello\n"))
			die("write failed");
	}
	return tar_finish(&w) ? 1 : 0;
}
