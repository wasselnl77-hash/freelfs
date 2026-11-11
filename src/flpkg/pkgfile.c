/* Reading .flpkg files: metadata and manifest up front, then the payload.
 * SPDX-License-Identifier: MIT */
#include "flpkg.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_META (64u << 20)

static ssize_t zsource_read(void *ctx, void *buf, size_t len)
{
	return zr_read(ctx, buf, len);
}

static char *read_member(struct pkgfile *pf, const char *name, char **error)
{
	struct tar_entry e = {0};
	int rc = tar_next(&pf->tr, &e);
	if (rc <= 0 || e.type != TAR_FILE || strcmp(e.path, name)) {
		*error = rc < 0 ? xasprintf("bad archive: %s", pf->tr.error)
		                : xasprintf("not a flpkg package (no %s)", name);
		tar_entry_clear(&e);
		return NULL;
	}
	tar_entry_clear(&e);
	char *text = tar_read_all(&pf->tr, MAX_META, NULL);
	if (!text)
		*error = xasprintf("cannot read %s: %s", name, pf->tr.error);
	return text;
}

int pkgfile_open(const char *path, struct pkgfile *pf, char **error)
{
	memset(pf, 0, sizeof(*pf));
	pf->fd = open(path, O_RDONLY | O_CLOEXEC);
	if (pf->fd < 0) {
		*error = xasprintf("cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	pf->zr = zr_open(pf->fd);
	pf->tr = (struct tar_reader){.read = zsource_read, .ctx = pf->zr};

	char *err = NULL;
	char *info = read_member(pf, ".PKGINFO", error);
	if (!info)
		goto bad;
	if (pkginfo_parse(info, &pf->info, &err) < 0) {
		*error = xasprintf("%s: .PKGINFO: %s", path, err);
		free(err);
		free(info);
		goto bad;
	}
	free(info);
	char *files = read_member(pf, ".FILES", error);
	if (!files)
		goto bad;
	if (manifest_parse(files, &pf->files, &err) < 0) {
		*error = xasprintf("%s: %s", path, err);
		free(err);
		free(files);
		goto bad;
	}
	free(files);
	return 0;
bad:
	pkgfile_close(pf);
	return -1;
}

void pkgfile_close(struct pkgfile *pf)
{
	if (pf->zr)
		zr_close(pf->zr);
	if (pf->fd >= 0)
		close(pf->fd);
	free(pf->tr.error);
	pkginfo_free(&pf->info);
	manifest_free(&pf->files);
	memset(pf, 0, sizeof(*pf));
	pf->fd = -1;
}
