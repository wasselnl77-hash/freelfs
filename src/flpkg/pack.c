/* Writing .flpkg files: walk a package root, build the manifest, archive it.
 *
 * The result depends only on the files' contents, names, types and
 * permission bits: owners come from the overrides (default 0:0), every mtime
 * is SOURCE_DATE_EPOCH, entries are sorted bytewise, and compression uses
 * fixed parameters.
 * SPDX-License-Identifier: MIT */
#include "flpkg.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct node {
	char *path; /* relative to the package root */
	struct stat st;
};

struct walk {
	const char *root;
	struct node *v;
	size_t n, cap;
	char *error;
};

static int walk_dir(struct walk *w, const char *rel)
{
	char *abs = *rel ? path_join(w->root, rel) : xstrdup(w->root);
	DIR *d = opendir(abs);
	if (!d) {
		w->error = xasprintf("cannot read %s: %s", abs, strerror(errno));
		free(abs);
		return -1;
	}
	struct dirent *de;
	int rc = 0;
	while (rc == 0 && (de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		char *child = *rel ? xasprintf("%s/%s", rel, de->d_name) : xstrdup(de->d_name);
		char *cabs = path_join(w->root, child);
		struct node nd = {child, {0}};
		if (lstat(cabs, &nd.st) < 0) {
			w->error = xasprintf("cannot stat %s: %s", cabs, strerror(errno));
			free(child);
			rc = -1;
		} else if (!S_ISREG(nd.st.st_mode) && !S_ISDIR(nd.st.st_mode) &&
		           !S_ISLNK(nd.st.st_mode)) {
			w->error = xasprintf("%s: device nodes, fifos and sockets cannot be packaged",
			                     child);
			free(child);
			rc = -1;
		} else {
			if (w->n == w->cap) {
				w->cap = w->cap ? w->cap * 2 : 256;
				w->v = xrealloc(w->v, w->cap * sizeof(*w->v));
			}
			w->v[w->n++] = nd;
			if (S_ISDIR(nd.st.st_mode))
				rc = walk_dir(w, child);
		}
		free(cabs);
	}
	closedir(d);
	free(abs);
	return rc;
}

static int cmp_node(const void *a, const void *b)
{
	return strcmp(((const struct node *)a)->path, ((const struct node *)b)->path);
}

static const struct owner_override *find_override(const struct owner_override *o, size_t n,
                                                  const char *path)
{
	for (size_t i = 0; i < n; i++)
		if (!strcmp(o[i].path, path))
			return &o[i];
	return NULL;
}

struct zsink {
	struct zwriter *zw;
};

static int zsink_write(void *ctx, const void *buf, size_t len)
{
	return zw_write(((struct zsink *)ctx)->zw, buf, len);
}

int pack_create(const char *root, struct pkginfo *p, const struct owner_override *overrides,
                size_t noverrides, const char *outpath, char **error)
{
	struct walk w = {.root = root};
	struct manifest m = {0};
	struct strbuf info = {0}, files = {0};
	char *tmp = xasprintf("%s.part", outpath);
	int fd = -1, rc = -1;

	if (walk_dir(&w, "") < 0) {
		*error = w.error;
		goto out;
	}
	qsort(w.v, w.n, sizeof(*w.v), cmp_node);

	for (size_t i = 0; i < noverrides; i++) {
		bool found = false;
		for (size_t j = 0; j < w.n && !found; j++)
			found = !strcmp(w.v[j].path, overrides[i].path);
		if (!found) {
			*error = xasprintf("owner=() names %s, which the package does not contain",
			                   overrides[i].path);
			goto out;
		}
	}

	/* Manifest: hard links point at the first path (in sorted order) of
	 * their inode, which is archived first. */
	p->size = 0;
	for (size_t i = 0; i < w.n; i++) {
		struct node *nd = &w.v[i];
		struct manifest_entry e = {0};
		const struct owner_override *o = find_override(overrides, noverrides, nd->path);
		e.mode = o ? o->mode : nd->st.st_mode & 07777;
		e.uid = o ? o->uid : 0;
		e.gid = o ? o->gid : 0;
		e.path = xstrdup(nd->path);
		char *abs = path_join(root, nd->path);
		if (S_ISDIR(nd->st.st_mode)) {
			e.type = 'd';
		} else if (S_ISLNK(nd->st.st_mode)) {
			char target[4096];
			ssize_t n = readlink(abs, target, sizeof(target) - 1);
			if (n < 0) {
				*error = xasprintf("cannot read link %s: %s", abs, strerror(errno));
				free(abs);
				free(e.path);
				goto out;
			}
			target[n] = '\0';
			e.type = 'l';
			e.mode = 0777;
			e.target = xstrdup(target);
		} else {
			/* w.v and m.v share indexes, so m.v[j] is the first link. */
			size_t j = 0;
			if (nd->st.st_nlink > 1)
				for (; j < i; j++)
					if (w.v[j].st.st_ino == nd->st.st_ino &&
					    w.v[j].st.st_dev == nd->st.st_dev)
						break;
			if (nd->st.st_nlink > 1 && j < i) {
				/* a hard link has its target's inode, so its attributes */
				e.type = 'h';
				e.target = xstrdup(w.v[j].path);
				e.mode = m.v[j].mode;
				e.uid = m.v[j].uid;
				e.gid = m.v[j].gid;
			} else {
				e.type = 'f';
				e.size = (uint64_t)nd->st.st_size;
				if (sha256_file(abs, e.sha256) < 0) {
					*error = xasprintf("cannot read %s: %s", abs, strerror(errno));
					free(abs);
					free(e.path);
					goto out;
				}
				p->size += e.size;
			}
		}
		free(abs);
		manifest_add(&m, &e);
	}

	if (!p->builddate)
		p->builddate = cfg.source_date_epoch;
	pkginfo_write(p, &info, false);
	manifest_write(&m, &files);

	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		*error = xasprintf("cannot create %s: %s", tmp, strerror(errno));
		goto out;
	}
	struct zsink sink = {zw_open(fd, cfg.zstd_level, cfg.jobs > 0 ? cfg.jobs : 1)};
	struct tar_writer tw = {zsink_write, &sink, 0, 0};
	int64_t mtime = cfg.source_date_epoch;
	struct tar_entry te = {.path = ".PKGINFO", .type = TAR_FILE, .mode = 0644,
	                       .size = info.len, .mtime = mtime};
	int wrc = tar_add_mem(&tw, &te, info.buf);
	te = (struct tar_entry){.path = ".FILES", .type = TAR_FILE, .mode = 0644,
	                        .size = files.len, .mtime = mtime};
	if (!wrc)
		wrc = tar_add_mem(&tw, &te, files.buf);

	for (size_t i = 0; i < m.n && !wrc; i++) {
		struct manifest_entry *e = &m.v[i];
		te = (struct tar_entry){.path = e->path, .link = e->target, .mode = e->mode,
		                        .uid = e->uid, .gid = e->gid, .mtime = mtime};
		switch (e->type) {
		case 'd':
			te.type = TAR_DIR;
			wrc = tar_add_mem(&tw, &te, NULL);
			break;
		case 'l':
			te.type = TAR_SYMLINK;
			wrc = tar_add_mem(&tw, &te, NULL);
			break;
		case 'h':
			te.type = TAR_HARDLINK;
			wrc = tar_add_mem(&tw, &te, NULL);
			break;
		default: {
			te.type = TAR_FILE;
			te.size = e->size;
			char *abs = path_join(root, e->path);
			int in = open(abs, O_RDONLY | O_CLOEXEC);
			free(abs);
			wrc = in < 0 ? -1 : tar_add_fd(&tw, &te, in);
			if (in >= 0)
				close(in);
		}
		}
	}
	if (!wrc)
		wrc = tar_finish(&tw);
	if (zw_close(sink.zw) || wrc || fsync(fd) < 0) {
		*error = xasprintf("writing %s failed", tmp);
		goto out;
	}
	close(fd);
	fd = -1;
	if (rename(tmp, outpath) < 0) {
		*error = xasprintf("cannot rename %s: %s", tmp, strerror(errno));
		goto out;
	}
	rc = 0;
out:
	if (fd >= 0)
		close(fd);
	if (rc)
		unlink(tmp);
	free(tmp);
	for (size_t i = 0; i < w.n; i++)
		free(w.v[i].path);
	free(w.v);
	manifest_free(&m);
	sb_free(&info);
	sb_free(&files);
	return rc;
}
