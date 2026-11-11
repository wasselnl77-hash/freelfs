/* flarchive -- Archives: open, browse and extract zip, tar.*, 7z, rar and more; create zip and tar.zst
 * SPDX-License-Identifier: MIT
 *
 *   flarchive [ARCHIVE]              browse an archive
 *   flarchive --extract-here ARCHIVE...
 *                                    extract next to the archive, show progress, close when done
 *   flarchive --compress PATH...     the "new archive" page with these files in it
 *   flarchive --new                  the same, empty
 *
 * libarchive does the reading and writing. The work (listing, extracting, creating) runs in a
 * child process that reports over a pipe, so the window stays alive, shows progress and can cancel.
 * Extraction never overwrites anything: it goes into a hidden folder first and is then moved to a
 * name that is free (a single top-level item is put next to the archive as it is, anything else in a
 * folder named after the archive).
 */
#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <flui.h>
#include <ftw.h>
#include <poll.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ==== helpers ================================================================================== */

static const char *base_name(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

static void dir_name(const char *p, char *out, size_t cap)
{
	snprintf(out, cap, "%s", p);
	char *s = strrchr(out, '/');
	if (!s)
		snprintf(out, cap, ".");
	else if (s == out)
		out[1] = 0;
	else
		*s = 0;
}

static void join(char *out, size_t cap, const char *dir, const char *name)
{
	snprintf(out, cap, "%s%s%s", dir, !strcmp(dir, "/") ? "" : "/", name);
}

static int ends_with(const char *s, const char *suf)
{
	size_t a = strlen(s), b = strlen(suf);
	return a >= b && !strcasecmp(s + a - b, suf);
}

/* the archive's name without its archive extension: "photos.tar.gz" -> "photos" */
static void strip_ext(const char *name, char *out, size_t cap)
{
	static const char *const exts[] = {".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tar.lz", ".tar.lzma",
	                                   ".tar.z", ".tgz", ".tbz2", ".tbz", ".txz", ".tzst", ".zip", ".7z", ".rar",
	                                   ".tar", ".gz", ".bz2", ".xz", ".zst", ".lz4", ".cab", ".iso", ".jar", ".apk",
	                                   ".cpio", ".deb", ".rpm", ".whl", ".xpi", ".epub", ".z", ".lzma"};
	snprintf(out, cap, "%s", name);
	for (size_t i = 0; i < sizeof exts / sizeof *exts; i++)
		if (ends_with(out, exts[i]) && strlen(out) > strlen(exts[i])) {
			out[strlen(out) - strlen(exts[i])] = 0;
			return;
		}
}

/* a name that is not taken in dir: "x", "x 2", "x 3"; ext (".zip") goes after the number */
static void unique_in(const char *dir, const char *stem, const char *ext, char *out, size_t cap)
{
	char p[PATH_MAX];
	struct stat st;
	for (int n = 1; n < 10000; n++) {
		if (n == 1)
			snprintf(out, cap, "%s%s", stem, ext);
		else
			snprintf(out, cap, "%s %d%s", stem, n, ext);
		join(p, sizeof p, dir, out);
		if (lstat(p, &st) < 0)
			return;
	}
}

static void fmt_size(double n, char *out, size_t cap)
{
	if (n < 1000)
		snprintf(out, cap, "%.0f B", n);
	else if (n < 1e6)
		snprintf(out, cap, "%.1f kB", n / 1e3);
	else if (n < 1e9)
		snprintf(out, cap, "%.1f MB", n / 1e6);
	else
		snprintf(out, cap, "%.2f GB", n / 1e9);
}

static int mkdir_p(const char *path)
{
	char tmp[PATH_MAX];
	snprintf(tmp, sizeof tmp, "%s", path);
	for (char *p = tmp + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			if (mkdir(tmp, 0777) < 0 && errno != EEXIST)
				return -1;
			*p = '/';
		}
	if (mkdir(tmp, 0777) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

static int rm_cb(const char *p, const struct stat *st, int flag, struct FTW *w)
{
	remove(p);
	return 0;
}

/* removes a folder this program made itself (the hidden extraction folder) */
static void rmtree(const char *p)
{
	nftw(p, rm_cb, 16, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
}

/* ==== the work: runs in a child process, talks over a pipe ======================================= */

enum { J_NONE, J_LIST, J_EXTRACT, J_CREATE };
enum { FMT_ZIP, FMT_TZST };

struct jobargs {
	int kind, fmt;
	char arc[PATH_MAX]; /* the archive to read, or the folder to put the new archive in */
	char dest[PATH_MAX];  /* extraction: the folder the result goes into */
	char name[NAME_MAX + 1]; /* creation: the file name of the new archive */
	char **sel; /* extraction: only these paths (and what is under them); creation: the inputs */
	int nsel;
};

static char g_tmp[PATH_MAX]; /* what the child removes when it is stopped */
static int g_tmp_dir;

static void child_term(int sig)
{
	if (g_tmp[0]) {
		if (g_tmp_dir)
			rmtree(g_tmp);
		else
			unlink(g_tmp);
	}
	_exit(1);
}

static double now_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void say_err(int fd, const char *fmt, ...)
{
	char m[500];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(m, sizeof m, fmt, ap);
	va_end(ap);
	for (char *p = m; *p; p++)
		if (*p == '\n' || *p == '\t')
			*p = ' ';
	dprintf(fd, "E\t%s\n", m);
}

static const char *friendly(const char *e)
{
	if (!e)
		return "The archive could not be read.";
	if (strcasestr(e, "passphrase") || strcasestr(e, "encrypted") || strcasestr(e, "password"))
		return "This archive is password protected, which Archives cannot open yet.";
	if (strcasestr(e, "unrecognized") || strcasestr(e, "unsupported") || strcasestr(e, "truncated header"))
		return "This is not an archive Archives can read.";
	return e;
}

/* Open path for reading and read the first header. A compressed single file (x.gz) is an
 * archive of one entry named after it. rc: ARCHIVE_OK with *e set, ARCHIVE_EOF for an empty
 * archive, else NULL is returned with err filled. */
static struct archive *reader_open(const char *path, struct archive_entry **e, int *rc, char *err, size_t ecap)
{
	err[0] = 0;
	for (int raw = 0; raw < 2; raw++) {
		struct archive *a = archive_read_new();
		archive_read_support_filter_all(a);
		archive_read_support_format_all(a);
		if (raw)
			archive_read_support_format_raw(a);
		if (archive_read_open_filename(a, path, 65536) != ARCHIVE_OK) {
			if (!raw)
				snprintf(err, ecap, "%s", friendly(archive_error_string(a)));
			archive_read_free(a);
			continue; /* the second try: a compressed single file */
		}
		*rc = archive_read_next_header(a, e);
		if (*rc == ARCHIVE_OK || *rc == ARCHIVE_WARN || *rc == ARCHIVE_EOF) {
			if (*rc == ARCHIVE_WARN)
				*rc = ARCHIVE_OK;
			/* the "raw" format takes anything: only trust it for something that is compressed */
			int comp = archive_filter_count(a) > 1 || (archive_filter_count(a) == 1 && archive_filter_code(a, 0) != ARCHIVE_FILTER_NONE);
			if (raw && !comp) {
				archive_read_free(a);
				return NULL;
			}
			return a;
		}
		if (!raw)
			snprintf(err, ecap, "%s", friendly(archive_error_string(a)));
		archive_read_free(a);
	}
	if (!err[0])
		snprintf(err, ecap, "This is not an archive Archives can read.");
	return NULL;
}

/* the path of an entry as we show it: no leading ./ or /, no trailing / */
static void norm_path(const char *in, char *out, size_t cap)
{
	while (in[0] == '/' || (in[0] == '.' && in[1] == '/'))
		in += in[0] == '/' ? 1 : 2;
	snprintf(out, cap, "%s", in);
	size_t n = strlen(out);
	while (n && out[n - 1] == '/')
		out[--n] = 0;
}

static void raw_name(const char *arc, char *out, size_t cap)
{
	strip_ext(base_name(arc), out, cap);
	if (!out[0])
		snprintf(out, cap, "data");
}

static int child_list(const struct jobargs *j, int fd)
{
	char err[300];
	struct archive_entry *e;
	int rc;
	struct archive *a = reader_open(j->arc, &e, &rc, err, sizeof err);
	if (!a) {
		say_err(fd, "%s", err);
		return 1;
	}
	while (rc == ARCHIVE_OK) {
		const char *pn = archive_entry_pathname(e);
		char p[PATH_MAX];
		if (archive_format(a) == ARCHIVE_FORMAT_RAW)
			raw_name(j->arc, p, sizeof p);
		else if (pn)
			norm_path(pn, p, sizeof p);
		else
			p[0] = 0;
		if (p[0] && !strchr(p, '\n')) {
			int dir = archive_entry_filetype(e) == AE_IFDIR;
			long long size = archive_entry_size_is_set(e) ? (long long)archive_entry_size(e) : 0;
			dprintf(fd, "I\t%lld\t%lld\t%d\t%s\n", size, (long long)(archive_entry_mtime_is_set(e) ? archive_entry_mtime(e) : 0), dir, p);
		}
		archive_read_data_skip(a);
		rc = archive_read_next_header(a, &e);
		if (rc == ARCHIVE_WARN)
			rc = ARCHIVE_OK;
	}
	if (rc != ARCHIVE_EOF) {
		say_err(fd, "%s", friendly(archive_error_string(a)));
		archive_read_free(a);
		return 1;
	}
	archive_read_free(a);
	dprintf(fd, "D\n");
	return 0;
}

static int in_selection(const char *p, char **sel, int nsel)
{
	if (!nsel)
		return 1;
	for (int i = 0; i < nsel; i++) {
		size_t n = strlen(sel[i]);
		if (!strncmp(p, sel[i], n) && (p[n] == 0 || p[n] == '/'))
			return 1;
	}
	return 0;
}

/* the part of the path the selection cuts off: "a/b/c" selected -> "a/b/" is dropped */
static size_t sel_strip(const char *p, char **sel, int nsel)
{
	for (int i = 0; i < nsel; i++) {
		size_t n = strlen(sel[i]);
		if (!strncmp(p, sel[i], n) && (p[n] == 0 || p[n] == '/')) {
			const char *s = strrchr(sel[i], '/');
			return s ? (size_t)(s - sel[i]) + 1 : 0;
		}
	}
	return 0;
}

static int child_extract(const struct jobargs *j, int fd)
{
	char err[300], tmp[PATH_MAX];
	struct archive_entry *e;
	int rc;
	struct archive *a = reader_open(j->arc, &e, &rc, err, sizeof err);
	if (!a) {
		say_err(fd, "%s", err);
		return 1;
	}
	if (mkdir_p(j->dest) < 0) {
		say_err(fd, "Cannot create %s: %s", j->dest, strerror(errno));
		archive_read_free(a);
		return 1;
	}
	join(tmp, sizeof tmp, j->dest, ".flarchive-XXXXXX");
	if (!mkdtemp(tmp)) {
		say_err(fd, "Cannot write in %s: %s", j->dest, strerror(errno));
		archive_read_free(a);
		return 1;
	}
	snprintf(g_tmp, sizeof g_tmp, "%s", tmp);
	g_tmp_dir = 1;
	/* archive_write_disk writes relative to the working directory: that must be the hidden folder */
	if (chdir(tmp) < 0) {
		say_err(fd, "Cannot enter %s: %s", tmp, strerror(errno));
		rmdir(tmp);
		return 1;
	}
	struct stat ast;
	double total = stat(j->arc, &ast) == 0 && ast.st_size > 0 ? (double)ast.st_size : 1;
	struct archive *w = archive_write_disk_new();
	archive_write_disk_set_options(w, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_SECURE_NODOTDOT |
	                                      ARCHIVE_EXTRACT_SECURE_SYMLINKS | ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS);
	archive_write_disk_set_standard_lookup(w);
	double last = 0;
	int skipped = 0, wrote = 0, fail = 0;
	while (rc == ARCHIVE_OK) {
		const char *pn = archive_entry_pathname(e);
		char p[PATH_MAX], np[PATH_MAX];
		int raw = archive_format(a) == ARCHIVE_FORMAT_RAW;
		if (raw)
			raw_name(j->arc, p, sizeof p);
		else if (pn)
			norm_path(pn, p, sizeof p);
		else
			p[0] = 0;
		if (p[0] && in_selection(p, j->sel, j->nsel)) {
			snprintf(np, sizeof np, "%s", p + sel_strip(p, j->sel, j->nsel));
			archive_entry_set_pathname(e, np);
			const char *hl = archive_entry_hardlink(e);
			if (hl) {
				char hp[PATH_MAX];
				norm_path(hl, hp, sizeof hp);
				if (in_selection(hp, j->sel, j->nsel)) {
					char nh[PATH_MAX];
					snprintf(nh, sizeof nh, "%s", hp + sel_strip(hp, j->sel, j->nsel));
					archive_entry_set_hardlink(e, nh);
				} else {
					archive_entry_set_hardlink(e, NULL);
				}
			}
			/* owners are not ours to set; keep the permission bits that make sense */
			{
				mode_t m = archive_entry_mode(e) & ~(mode_t)(S_ISUID | S_ISGID | S_ISVTX);
				archive_entry_set_mode(e, m | (S_ISDIR(m) ? 0700 : 0600)); /* what we extract, we can use and remove */
			}
			int wr = archive_write_header(w, e);
			if (wr < ARCHIVE_WARN) { /* a path that is not allowed (.. or absolute), a bad link ... */
				skipped++;
			} else if (archive_entry_size(e) > 0 || archive_entry_size_is_set(e) == 0) {
				const void *buf;
				size_t len;
				la_int64_t off;
				int r;
				while ((r = archive_read_data_block(a, &buf, &len, &off)) == ARCHIVE_OK) {
					if (archive_write_data_block(w, buf, len, off) < ARCHIVE_WARN) {
						say_err(fd, "Cannot write %s: %s", np, archive_error_string(w));
						fail = 1;
						break;
					}
					double t = now_s();
					if (t - last > 0.08) {
						last = t;
						dprintf(fd, "P\t%.4f\n", (double)archive_filter_bytes(a, 0) / total);
					}
				}
				if (fail)
					break;
				if (r != ARCHIVE_EOF) {
					say_err(fd, "%s", friendly(archive_error_string(a)));
					fail = 1;
					break;
				}
				archive_write_finish_entry(w);
				wrote++;
			} else {
				archive_write_finish_entry(w);
				wrote++;
			}
		} else {
			archive_read_data_skip(a);
		}
		double t = now_s();
		if (t - last > 0.08) {
			last = t;
			dprintf(fd, "P\t%.4f\n", (double)archive_filter_bytes(a, 0) / total);
		}
		rc = archive_read_next_header(a, &e);
		if (rc == ARCHIVE_WARN)
			rc = ARCHIVE_OK;
	}
	if (!fail && rc != ARCHIVE_EOF) {
		say_err(fd, "%s", friendly(archive_error_string(a)));
		fail = 1;
	}
	archive_read_free(a);
	archive_write_close(w);
	archive_write_free(w);
	if (fail) {
		rmtree(tmp);
		return 1;
	}
	if (!wrote) {
		rmtree(tmp);
		say_err(fd, j->nsel ? "Nothing to extract." : "The archive is empty.");
		return 1;
	}
	/* one top-level item: it goes next to where the archive is; several: into a folder of the archive's name */
	DIR *d = opendir(tmp);
	char only[NAME_MAX + 1] = "";
	int n = 0;
	struct dirent *de;
	while (d && (de = readdir(d)))
		if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) {
			if (!n)
				snprintf(only, sizeof only, "%s", de->d_name);
			n++;
		}
	if (d)
		closedir(d);
	char final[NAME_MAX + 1], from[PATH_MAX], to[PATH_MAX];
	if (n == 1) {
		char stem[NAME_MAX + 1];
		const char *dot = strrchr(only, '.');
		struct stat st;
		join(from, sizeof from, tmp, only);
		int isdir = lstat(from, &st) == 0 && S_ISDIR(st.st_mode);
		if (dot && dot != only && !isdir) {
			snprintf(stem, sizeof stem, "%.*s", (int)(dot - only), only);
			unique_in(j->dest, stem, dot, final, sizeof final);
		} else {
			unique_in(j->dest, only, "", final, sizeof final);
		}
		join(to, sizeof to, j->dest, final);
		if (rename(from, to) < 0) {
			say_err(fd, "Cannot move the result: %s", strerror(errno));
			rmtree(tmp);
			return 1;
		}
		rmdir(tmp);
	} else {
		char stem[NAME_MAX + 1];
		strip_ext(base_name(j->arc), stem, sizeof stem);
		unique_in(j->dest, stem, "", final, sizeof final);
		join(to, sizeof to, j->dest, final);
		if (rename(tmp, to) < 0) {
			say_err(fd, "Cannot move the result: %s", strerror(errno));
			rmtree(tmp);
			return 1;
		}
	}
	g_tmp[0] = 0;
	dprintf(fd, "R\t%s\t%d\n", to, skipped);
	dprintf(fd, "D\n");
	return 0;
}

/* ---- creating ---------------------------------------------------------------------------------- */

struct cctx {
	struct archive *a;
	dev_t odev;
	ino_t oino;
	double done, total, last;
	int fd, skipped, failed;
	char err[400];
};

static double scan_total(const char *p)
{
	struct stat st;
	if (lstat(p, &st) < 0)
		return 0;
	if (S_ISREG(st.st_mode))
		return (double)st.st_size;
	if (!S_ISDIR(st.st_mode))
		return 0;
	double sum = 0;
	DIR *d = opendir(p);
	struct dirent *de;
	while (d && (de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		char c[PATH_MAX];
		join(c, sizeof c, p, de->d_name);
		sum += scan_total(c);
	}
	if (d)
		closedir(d);
	return sum;
}

static int cmp_names(const struct dirent **a, const struct dirent **b)
{
	return strcmp((*a)->d_name, (*b)->d_name);
}

static int add_tree(struct cctx *c, const char *full, const char *arc)
{
	struct stat st;
	if (lstat(full, &st) < 0) {
		c->skipped++;
		return 0;
	}
	if (st.st_dev == c->odev && st.st_ino == c->oino)
		return 0; /* the archive being written */
	if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
		c->skipped++;
		return 0;
	}
	int fdi = -1;
	if (S_ISREG(st.st_mode) && (fdi = open(full, O_RDONLY | O_CLOEXEC)) < 0) {
		c->skipped++; /* unreadable: left out */
		return 0;
	}
	struct archive_entry *en = archive_entry_new();
	archive_entry_copy_stat(en, &st);
	archive_entry_set_pathname(en, arc);
	if (S_ISLNK(st.st_mode)) {
		char tgt[PATH_MAX];
		ssize_t n = readlink(full, tgt, sizeof tgt - 1);
		if (n < 0) {
			archive_entry_free(en);
			c->skipped++;
			return 0;
		}
		tgt[n] = 0;
		archive_entry_set_symlink(en, tgt);
		archive_entry_set_size(en, 0);
	}
	if (S_ISDIR(st.st_mode))
		archive_entry_set_size(en, 0);
	if (archive_write_header(c->a, en) < ARCHIVE_WARN) {
		snprintf(c->err, sizeof c->err, "Cannot add %s: %s", arc, archive_error_string(c->a));
		c->failed = 1;
		archive_entry_free(en);
		if (fdi >= 0)
			close(fdi);
		return -1;
	}
	archive_entry_free(en);
	if (fdi >= 0) {
		static char buf[65536];
		ssize_t n;
		long long left = (long long)st.st_size;
		while (left > 0 && (n = read(fdi, buf, sizeof buf)) > 0) {
			if (n > left)
				n = (ssize_t)left;
			if (archive_write_data(c->a, buf, (size_t)n) < 0) {
				snprintf(c->err, sizeof c->err, "Cannot write: %s", archive_error_string(c->a));
				c->failed = 1;
				close(fdi);
				return -1;
			}
			left -= n;
			c->done += (double)n;
			double t = now_s();
			if (t - c->last > 0.08) {
				c->last = t;
				dprintf(c->fd, "P\t%.4f\n", c->total > 0 ? c->done / c->total : 0);
			}
		}
		close(fdi);
	}
	if (S_ISDIR(st.st_mode)) {
		struct dirent **names;
		int n = scandir(full, &names, NULL, cmp_names);
		for (int i = 0; i < n; i++) {
			if (strcmp(names[i]->d_name, ".") && strcmp(names[i]->d_name, "..") && !c->failed) {
				char cf[PATH_MAX], ca[PATH_MAX];
				join(cf, sizeof cf, full, names[i]->d_name);
				snprintf(ca, sizeof ca, "%s/%s", arc, names[i]->d_name);
				add_tree(c, cf, ca);
			}
			free(names[i]);
		}
		free(names);
	}
	return c->failed ? -1 : 0;
}

static int child_create(const struct jobargs *j, int fd)
{
	char out[PATH_MAX], part[PATH_MAX], final[NAME_MAX + 1];
	const char *ext = j->fmt == FMT_ZIP ? ".zip" : ".tar.zst";
	if (mkdir_p(j->arc) < 0) {
		say_err(fd, "Cannot create %s: %s", j->arc, strerror(errno));
		return 1;
	}
	/* j->name is the name without the extension */
	unique_in(j->arc, j->name, ext, final, sizeof final);
	join(out, sizeof out, j->arc, final);
	char pn[NAME_MAX + 32];
	snprintf(pn, sizeof pn, ".%s.part", final);
	join(part, sizeof part, j->arc, pn);
	int ofd = open(part, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666 & ~(mode_t)0022);
	if (ofd < 0) {
		say_err(fd, "Cannot write in %s: %s", j->arc, strerror(errno));
		return 1;
	}
	snprintf(g_tmp, sizeof g_tmp, "%s", part);
	g_tmp_dir = 0;
	struct stat ost;
	fstat(ofd, &ost);
	struct cctx c = {.odev = ost.st_dev, .oino = ost.st_ino, .fd = fd};
	for (int i = 0; i < j->nsel; i++)
		c.total += scan_total(j->sel[i]);
	c.a = archive_write_new();
	if (j->fmt == FMT_ZIP) {
		archive_write_set_format_zip(c.a);
		archive_write_set_options(c.a, "zip:compression=deflate");
	} else {
		archive_write_set_format_pax_restricted(c.a);
		archive_write_add_filter_zstd(c.a);
		archive_write_set_options(c.a, "zstd:compression-level=6");
	}
	if (archive_write_open_fd(c.a, ofd) != ARCHIVE_OK) {
		say_err(fd, "%s", archive_error_string(c.a));
		close(ofd);
		unlink(part);
		return 1;
	}
	for (int i = 0; i < j->nsel && !c.failed; i++)
		add_tree(&c, j->sel[i], base_name(j->sel[i]));
	if (archive_write_close(c.a) != ARCHIVE_OK && !c.failed) {
		snprintf(c.err, sizeof c.err, "%s", archive_error_string(c.a));
		c.failed = 1;
	}
	archive_write_free(c.a);
	if (!c.failed && (fsync(ofd) < 0 || close(ofd) < 0)) {
		snprintf(c.err, sizeof c.err, "Cannot write %s: %s", final, strerror(errno));
		c.failed = 1;
	} else if (c.failed) {
		close(ofd);
	}
	if (c.failed) {
		unlink(part);
		say_err(fd, "%s", c.err[0] ? c.err : "Could not create the archive.");
		return 1;
	}
	if (rename(part, out) < 0) {
		say_err(fd, "Cannot write %s: %s", final, strerror(errno));
		unlink(part);
		return 1;
	}
	g_tmp[0] = 0;
	dprintf(fd, "R\t%s\t%d\n", out, c.skipped);
	dprintf(fd, "D\n");
	return 0;
}

/* ==== the window ================================================================================= */

enum {
	ID_OPEN = 1, ID_NEW, ID_EXTRACT_ALL, ID_EXTRACT_SEL, ID_UP, ID_LIST, ID_VSB, ID_CANCEL, ID_SHOW,
	ID_ADD, ID_REMOVE, ID_FMT_ZIP, ID_FMT_TZST, ID_NAME, ID_DEST, ID_CREATE, ID_BACK, ID_CLOSE, ID_OPEN_NEW,
	ID_DLG = 100, ID_INLIST = 400
};
enum { TOOL_H = 48, PATH_H = 36, HEAD_H = 28, ROW_H = 30, STATUS_H = 32 };
enum { M_EMPTY, M_BROWSE, M_CREATE };
enum { OV_NONE, OV_PROMPT, OV_ERROR };
enum { PR_OPEN, PR_EXTRACT, PR_ADD };

struct item {
	char *path;
	long long size;
	time_t mtime;
	int dir;
};

struct row {
	const char *name;
	size_t nlen;
	long long size;
	time_t mtime;
	int dir;
};

struct A {
	fl_app *app;
	fl_win *win;
	fl_font *f_title, *f_small, *f_mono;
	fl_timer *close_timer;
	int mode;

	/* the archive */
	char apath[PATH_MAX];
	struct item *items;
	size_t nitems, cap_items;
	double unpacked;
	char cwd[PATH_MAX]; /* folder inside the archive, "" is the top */
	struct row *rows;
	int nrows, sel, scroll, drag_sb;
	char msg[400];
	int msg_err;

	/* the job */
	struct {
		int kind, fd, ok, running, skipped;
		pid_t pid;
		char buf[8192];
		size_t blen;
		double frac;
		char err[400], result[PATH_MAX];
		struct jobargs *args;
		int got_done;
	} job;

	/* extract-here queue */
	char **queue;
	int nq, qi, cli_extract;

	/* creating */
	char **inputs;
	int ninputs, in_sel, in_scroll;
	int fmt;
	char name[NAME_MAX + 1], dest[PATH_MAX];
	int from_cli_compress, created;

	/* overlays */
	int ov, prompt_kind, ov_new, extract_sel;
	char prompt_text[PATH_MAX], prompt_err[200], prompt_title[80], prompt_ok[24];
	char err_text[500];
};

static void say(struct A *a, int err, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(a->msg, sizeof a->msg, fmt, ap);
	va_end(ap);
	a->msg_err = err;
}

/* ---- the rows of the current folder ---------------------------------------------------------------------- */

static int cmp_rows(const void *x, const void *y)
{
	const struct row *a = x, *b = y;
	if (a->dir != b->dir)
		return b->dir - a->dir;
	size_t n = a->nlen < b->nlen ? a->nlen : b->nlen;
	int c = strncasecmp(a->name, b->name, n);
	if (c)
		return c;
	if (a->nlen != b->nlen)
		return a->nlen < b->nlen ? -1 : 1;
	return strncmp(a->name, b->name, n);
}

static void build_rows(struct A *a)
{
	free(a->rows);
	a->rows = NULL;
	a->nrows = 0;
	size_t plen = strlen(a->cwd), cap = 0;
	for (size_t i = 0; i < a->nitems; i++) {
		const char *p = a->items[i].path, *rest;
		if (plen) {
			if (strncmp(p, a->cwd, plen) || p[plen] != '/')
				continue;
			rest = p + plen + 1;
		} else {
			rest = p;
		}
		if (!*rest)
			continue;
		const char *sl = strchr(rest, '/');
		size_t nl = sl ? (size_t)(sl - rest) : strlen(rest);
		if ((size_t)a->nrows == cap) {
			cap = cap ? cap * 2 : 256;
			struct row *nr = realloc(a->rows, cap * sizeof *nr);
			if (!nr)
				return;
			a->rows = nr;
		}
		a->rows[a->nrows++] = (struct row){rest, nl, sl ? 0 : a->items[i].size, a->items[i].mtime, sl != NULL || a->items[i].dir};
	}
	qsort(a->rows, (size_t)a->nrows, sizeof *a->rows, cmp_rows);
	/* a folder that is listed as an entry and also appears as the start of other paths: one row */
	int o = 0;
	for (int i = 0; i < a->nrows; i++) {
		if (o && a->rows[o - 1].dir == a->rows[i].dir && a->rows[o - 1].nlen == a->rows[i].nlen &&
		    !strncmp(a->rows[o - 1].name, a->rows[i].name, a->rows[i].nlen)) {
			if (!a->rows[o - 1].mtime)
				a->rows[o - 1].mtime = a->rows[i].mtime;
			continue;
		}
		a->rows[o++] = a->rows[i];
	}
	a->nrows = o;
	if (a->sel >= a->nrows)
		a->sel = a->nrows - 1;
	if (a->sel < 0)
		a->sel = 0;
}

static void items_clear(struct A *a)
{
	for (size_t i = 0; i < a->nitems; i++)
		free(a->items[i].path);
	free(a->items);
	a->items = NULL;
	a->nitems = a->cap_items = 0;
	a->unpacked = 0;
	free(a->rows);
	a->rows = NULL;
	a->nrows = 0;
}

/* ---- running a job ------------------------------------------------------------------------------------------ */

static void job_free_args(struct A *a)
{
	struct jobargs *j = a->job.args;
	if (!j)
		return;
	for (int i = 0; i < j->nsel; i++)
		free(j->sel[i]);
	free(j->sel);
	free(j);
	a->job.args = NULL;
}

static void job_line(struct A *a, char *line)
{
	if (line[0] == 'I' && line[1] == '\t') {
		long long size, mt;
		int dir, off;
		if (sscanf(line + 2, "%lld\t%lld\t%d\t%n", &size, &mt, &dir, &off) >= 3) {
			if (a->nitems == a->cap_items) {
				size_t nc = a->cap_items ? a->cap_items * 2 : 1024;
				struct item *ni = realloc(a->items, nc * sizeof *ni);
				if (!ni)
					return;
				a->items = ni;
				a->cap_items = nc;
			}
			char *p = strdup(line + 2 + off);
			if (!p)
				return;
			a->items[a->nitems++] = (struct item){p, size, (time_t)mt, dir};
			if (!dir)
				a->unpacked += (double)size;
		}
	} else if (line[0] == 'P' && line[1] == '\t') {
		a->job.frac = atof(line + 2);
	} else if (line[0] == 'R' && line[1] == '\t') {
		char *t = strchr(line + 2, '\t');
		if (t) {
			*t = 0;
			a->job.skipped = atoi(t + 1);
		}
		snprintf(a->job.result, sizeof a->job.result, "%s", line + 2);
	} else if (line[0] == 'D') {
		a->job.got_done = 1;
	} else if (line[0] == 'E' && line[1] == '\t') {
		snprintf(a->job.err, sizeof a->job.err, "%s", line + 2);
	}
}

static void queue_next(struct A *a);
static void open_archive(struct A *a, const char *path);

static void job_finished(struct A *a)
{
	int kind = a->job.kind;
	fl_fd_remove(a->app, a->job.fd);
	close(a->job.fd);
	int st = 0;
	waitpid(a->job.pid, &st, 0);
	a->job.running = 0;
	a->job.ok = a->job.got_done && !a->job.err[0];
	if (!a->job.ok && !a->job.err[0])
		snprintf(a->job.err, sizeof a->job.err, "Stopped.");
	job_free_args(a);
	if (kind == J_LIST) {
		build_rows(a);
		if (!a->job.ok) {
			items_clear(a);
			a->mode = M_EMPTY;
			a->apath[0] = 0;
			char nm[NAME_MAX + 1];
			snprintf(nm, sizeof nm, "%s", base_name(a->apath));
			snprintf(a->err_text, sizeof a->err_text, "%s: %s", nm, a->job.err);
			a->ov = OV_ERROR;
		} else if (!a->nitems) {
			say(a, 0, "This archive is empty.");
		}
	} else if (kind == J_EXTRACT) {
		if (a->job.ok) {
			if (a->job.skipped)
				say(a, 0, "Extracted to %s (%d item%s skipped: unsafe paths)", base_name(a->job.result), a->job.skipped,
				    a->job.skipped == 1 ? "" : "s");
			else
				say(a, 0, "Extracted to %s", base_name(a->job.result));
			if (a->cli_extract) {
				queue_next(a);
				return;
			}
		} else if (a->job.err[0] && strcmp(a->job.err, "Stopped.")) {
			snprintf(a->err_text, sizeof a->err_text, "%s", a->job.err);
			a->ov = OV_ERROR;
		} else {
			say(a, 0, "Cancelled.");
		}
	} else if (kind == J_CREATE) {
		if (a->job.ok) {
			a->created = 1;
			if (a->job.skipped)
				say(a, 0, "Created %s (%d item%s could not be added)", base_name(a->job.result), a->job.skipped,
				    a->job.skipped == 1 ? "" : "s");
			else
				say(a, 0, "Created %s", base_name(a->job.result));
		} else if (strcmp(a->job.err, "Stopped.")) {
			snprintf(a->err_text, sizeof a->err_text, "%s", a->job.err);
			a->ov = OV_ERROR;
		} else {
			say(a, 0, "Cancelled.");
		}
	}
	fl_win_redraw(a->win);
}

static void job_readable(void *ud, int fd, short rev)
{
	struct A *a = ud;
	char *buf = a->job.buf;
	for (;;) {
		ssize_t n = read(fd, buf + a->job.blen, sizeof a->job.buf - a->job.blen - 1);
		if (n > 0) {
			a->job.blen += (size_t)n;
			buf[a->job.blen] = 0;
			char *s = buf, *nl;
			while ((nl = strchr(s, '\n'))) {
				*nl = 0;
				job_line(a, s);
				s = nl + 1;
			}
			size_t rest = a->job.blen - (size_t)(s - buf);
			memmove(buf, s, rest);
			a->job.blen = rest;
			if (a->job.blen >= sizeof a->job.buf - 1) /* a line too long for us: drop it */
				a->job.blen = 0;
			if ((size_t)n < 4096)
				break;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			break;
		job_finished(a); /* EOF: the child is done */
		return;
	}
	if (a->job.kind == J_LIST && a->nitems % 4096 < 64)
		build_rows(a);
	fl_win_redraw(a->win);
}

/* Start the child. args is owned from here on. */
static int job_start(struct A *a, struct jobargs *args)
{
	int p[2];
	if (pipe2(p, O_CLOEXEC) < 0) {
		free(args);
		return -1;
	}
	fflush(NULL);
	pid_t pid = fork();
	if (pid < 0) {
		close(p[0]);
		close(p[1]);
		free(args);
		return -1;
	}
	if (pid == 0) {
		signal(SIGTERM, child_term);
		signal(SIGPIPE, SIG_IGN);
		close(p[0]);
		int rc = args->kind == J_LIST ? child_list(args, p[1]) : args->kind == J_EXTRACT ? child_extract(args, p[1]) : child_create(args, p[1]);
		_exit(rc);
	}
	close(p[1]);
	fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
	a->job.kind = args->kind;
	a->job.fd = p[0];
	a->job.pid = pid;
	a->job.running = 1;
	a->job.ok = 0;
	a->job.got_done = 0;
	a->job.frac = 0;
	a->job.blen = 0;
	a->job.err[0] = 0;
	a->job.result[0] = 0;
	a->job.skipped = 0;
	a->job.args = args;
	fl_fd_add(a->app, p[0], POLLIN, job_readable, a);
	return 0;
}

static struct jobargs *args_new(int kind)
{
	struct jobargs *j = calloc(1, sizeof *j);
	if (j)
		j->kind = kind;
	return j;
}

static void cancel_job(struct A *a)
{
	if (a->job.running)
		kill(a->job.pid, SIGTERM); /* the child removes its temporary files, then the pipe closes */
}

/* ---- actions ---------------------------------------------------------------------------------------------------- */

static void open_archive(struct A *a, const char *path)
{
	char full[PATH_MAX];
	if (!realpath(path, full)) {
		snprintf(a->err_text, sizeof a->err_text, "Cannot open %s: %s", path, strerror(errno));
		a->ov = OV_ERROR;
		return;
	}
	struct stat st;
	if (stat(full, &st) < 0 || !S_ISREG(st.st_mode)) {
		snprintf(a->err_text, sizeof a->err_text, "%s is not a file.", full);
		a->ov = OV_ERROR;
		return;
	}
	if (a->job.running)
		return;
	items_clear(a);
	struct jobargs *j = args_new(J_LIST);
	if (!j)
		return;
	snprintf(j->arc, sizeof j->arc, "%s", full);
	snprintf(a->apath, sizeof a->apath, "%s", full);
	a->cwd[0] = 0;
	a->sel = a->scroll = 0;
	a->mode = M_BROWSE;
	a->msg[0] = 0;
	if (job_start(a, j) < 0)
		say(a, 1, "Cannot start: %s", strerror(errno));
}

static void start_extract(struct A *a, const char *dest, const char *arc, char **sel, int nsel)
{
	if (a->job.running)
		return;
	struct jobargs *j = args_new(J_EXTRACT);
	if (!j)
		return;
	snprintf(j->arc, sizeof j->arc, "%s", arc);
	snprintf(j->dest, sizeof j->dest, "%s", dest);
	j->sel = calloc((size_t)(nsel ? nsel : 1), sizeof *j->sel);
	for (int i = 0; i < nsel; i++)
		j->sel[i] = strdup(sel[i]);
	j->nsel = nsel;
	a->msg[0] = 0;
	if (job_start(a, j) < 0)
		say(a, 1, "Cannot start: %s", strerror(errno));
}

static void queue_next(struct A *a)
{
	if (a->qi >= a->nq) {
		if (a->nq > 1)
			say(a, 0, "Done: %d archives extracted.", a->nq);
		if (a->close_timer)
			fl_timer_arm(a->close_timer, 1300, 0);
		fl_win_redraw(a->win);
		return;
	}
	const char *arc = a->queue[a->qi++];
	snprintf(a->apath, sizeof a->apath, "%s", arc);
	char dest[PATH_MAX];
	dir_name(arc, dest, sizeof dest);
	start_extract(a, dest, arc, NULL, 0);
	fl_win_redraw(a->win);
}

static void enter_folder(struct A *a, const char *name, size_t nl)
{
	size_t l = strlen(a->cwd);
	if (l + nl + 2 >= sizeof a->cwd)
		return;
	if (l)
		a->cwd[l++] = '/';
	memcpy(a->cwd + l, name, nl);
	a->cwd[l + nl] = 0;
	a->sel = a->scroll = 0;
	build_rows(a);
}

static void go_up(struct A *a)
{
	char *s = strrchr(a->cwd, '/');
	char last[NAME_MAX + 1] = "";
	snprintf(last, sizeof last, "%s", s ? s + 1 : a->cwd);
	if (s)
		*s = 0;
	else
		a->cwd[0] = 0;
	a->scroll = 0;
	build_rows(a);
	for (int i = 0; i < a->nrows; i++)
		if (a->rows[i].dir && a->rows[i].nlen == strlen(last) && !strncmp(a->rows[i].name, last, a->rows[i].nlen)) {
			a->sel = i;
			break;
		}
}

static void prompt_open(struct A *a, int kind, const char *title, const char *ok, const char *text)
{
	a->ov = OV_PROMPT;
	a->prompt_kind = kind;
	a->ov_new = 1;
	snprintf(a->prompt_title, sizeof a->prompt_title, "%s", title);
	snprintf(a->prompt_ok, sizeof a->prompt_ok, "%s", ok);
	snprintf(a->prompt_text, sizeof a->prompt_text, "%s", text);
	a->prompt_err[0] = 0;
	a->extract_sel = 0;
}

/* the "extract to" prompt, for everything or for the selected row */
static void prompt_extract(struct A *a, int selected)
{
	char d[PATH_MAX];
	dir_name(a->apath, d, sizeof d);
	strcat(d, "/");
	prompt_open(a, PR_EXTRACT, selected ? "Extract selected to folder" : "Extract all to folder", "Extract", d);
	a->extract_sel = selected;
}

static void create_defaults(struct A *a)
{
	if (a->ninputs == 0) {
		snprintf(a->name, sizeof a->name, "Archive");
		const char *h = getenv("HOME");
		snprintf(a->dest, sizeof a->dest, "%s", h ? h : "/");
		return;
	}
	const char *first = a->inputs[0];
	if (a->ninputs == 1) {
		struct stat st;
		const char *b = base_name(first);
		if (stat(first, &st) == 0 && S_ISREG(st.st_mode) && strrchr(b, '.') > b)
			snprintf(a->name, sizeof a->name, "%.*s", (int)(strrchr(b, '.') - b), b);
		else
			snprintf(a->name, sizeof a->name, "%s", b);
	} else {
		snprintf(a->name, sizeof a->name, "Archive");
	}
	dir_name(first, a->dest, sizeof a->dest);
}

static int add_input(struct A *a, const char *given, char *err, size_t ecap)
{
	char full[PATH_MAX];
	if (!realpath(given, full)) {
		snprintf(err, ecap, "%s: %s", given, strerror(errno));
		return -1;
	}
	for (int i = 0; i < a->ninputs; i++)
		if (!strcmp(a->inputs[i], full))
			return 0;
	char **n = realloc(a->inputs, (size_t)(a->ninputs + 1) * sizeof *n);
	if (!n)
		return -1;
	a->inputs = n;
	a->inputs[a->ninputs] = strdup(full);
	if (!a->inputs[a->ninputs])
		return -1;
	a->ninputs++;
	return 0;
}

static void start_create(struct A *a)
{
	if (a->job.running || !a->ninputs)
		return;
	if (!a->name[0] || strchr(a->name, '/')) {
		snprintf(a->err_text, sizeof a->err_text, "Type a name for the archive (without a slash).");
		a->ov = OV_ERROR;
		return;
	}
	struct jobargs *j = args_new(J_CREATE);
	if (!j)
		return;
	j->fmt = a->fmt;
	snprintf(j->arc, sizeof j->arc, "%s", a->dest);
	snprintf(j->name, sizeof j->name, "%s", a->name);
	j->sel = calloc((size_t)a->ninputs, sizeof *j->sel);
	for (int i = 0; i < a->ninputs; i++)
		j->sel[i] = strdup(a->inputs[i]);
	j->nsel = a->ninputs;
	a->msg[0] = 0;
	a->created = 0;
	if (job_start(a, j) < 0)
		say(a, 1, "Cannot start: %s", strerror(errno));
}

static void show_in_files(const char *path)
{
	char dir[PATH_MAX];
	struct stat st;
	if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
		snprintf(dir, sizeof dir, "%s", path);
	else
		dir_name(path, dir, sizeof dir);
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		signal(SIGCHLD, SIG_DFL);
		execlp("flfiles", "flfiles", dir, (char *)NULL);
		_exit(127);
	}
}

/* ---- path prompt (Tab completes) ------------------------------------------------------------------------------ */

static void expand_home(const char *in, char *out, size_t cap)
{
	const char *home = getenv("HOME");
	if (in[0] == '~' && (in[1] == '/' || !in[1]) && home)
		snprintf(out, cap, "%s%s", home, in + 1);
	else if (in[0] != '/') {
		char cwd[PATH_MAX];
		if (getcwd(cwd, sizeof cwd))
			snprintf(out, cap, "%s/%s", cwd, in);
		else
			snprintf(out, cap, "%s", in);
	} else {
		snprintf(out, cap, "%s", in);
	}
}

static void complete_path(struct A *a, int dirs_only)
{
	char full[PATH_MAX];
	expand_home(a->prompt_text, full, sizeof full);
	char *slash = strrchr(full, '/');
	if (!slash)
		return;
	char dir[PATH_MAX];
	size_t dl = (size_t)(slash - full) + 1;
	memcpy(dir, full, dl);
	dir[dl] = 0;
	const char *part = slash + 1;
	size_t pl = strlen(part);
	DIR *d = opendir(dir);
	if (!d)
		return;
	char best[NAME_MAX + 2] = "";
	int n = 0, best_dir = 0;
	struct dirent *de;
	while ((de = readdir(d))) {
		if (strncmp(de->d_name, part, pl) || !strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (de->d_name[0] == '.' && part[0] != '.')
			continue;
		char p[PATH_MAX];
		join(p, sizeof p, dir, de->d_name);
		struct stat st;
		int isdir = stat(p, &st) == 0 && S_ISDIR(st.st_mode);
		if (dirs_only && !isdir)
			continue;
		if (n == 0) {
			snprintf(best, sizeof best, "%s", de->d_name);
			best_dir = isdir;
		} else {
			size_t k = 0;
			while (best[k] && best[k] == de->d_name[k])
				k++;
			best[k] = 0;
			best_dir = 0;
		}
		n++;
	}
	closedir(d);
	if (!n)
		return;
	const char *rs = strrchr(a->prompt_text, '/');
	size_t keep = rs ? (size_t)(rs - a->prompt_text) + 1 : 0;
	char out[PATH_MAX];
	snprintf(out, sizeof out, "%.*s%s%s", (int)keep, a->prompt_text, best, n == 1 && best_dir ? "/" : "");
	snprintf(a->prompt_text, sizeof a->prompt_text, "%s", out);
}

static void prompt_confirm(struct A *a)
{
	char full[PATH_MAX];
	if (!a->prompt_text[0]) {
		snprintf(a->prompt_err, sizeof a->prompt_err, "Type a path.");
		return;
	}
	expand_home(a->prompt_text, full, sizeof full);
	struct stat st;
	int exists = stat(full, &st) == 0;
	if (a->prompt_kind == PR_OPEN) {
		if (!exists || !S_ISREG(st.st_mode)) {
			snprintf(a->prompt_err, sizeof a->prompt_err, exists ? "That is not a file." : "That file does not exist.");
			return;
		}
		a->ov = OV_NONE;
		open_archive(a, full);
	} else if (a->prompt_kind == PR_ADD) {
		char err[200];
		if (!exists) {
			snprintf(a->prompt_err, sizeof a->prompt_err, "That does not exist.");
			return;
		}
		if (add_input(a, full, err, sizeof err) < 0) {
			snprintf(a->prompt_err, sizeof a->prompt_err, "%s", err);
			return;
		}
		if (a->ninputs == 1)
			create_defaults(a);
		a->ov = OV_NONE;
	} else {
		if (exists && !S_ISDIR(st.st_mode)) {
			snprintf(a->prompt_err, sizeof a->prompt_err, "That is a file, not a folder.");
			return;
		}
		a->ov = OV_NONE;
		char *selp = NULL;
		int nsel = 0;
		char selpath[PATH_MAX];
		/* the selected row, or everything */
		if (a->mode == M_BROWSE && a->extract_sel && a->sel >= 0 && a->sel < a->nrows) {
			const struct row *r = &a->rows[a->sel];
			snprintf(selpath, sizeof selpath, "%s%s%.*s", a->cwd, a->cwd[0] ? "/" : "", (int)r->nlen, r->name);
			selp = selpath;
			nsel = 1;
		}
		start_extract(a, full, a->apath, &selp, nsel);
	}
}

/* ==== drawing ====================================================================================== */

static void draw_progress(fl_ui *ui, fl_rect r, double frac)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, r, fl_th.raised);
	if (frac < 0)
		frac = 0;
	if (frac > 1)
		frac = 1;
	fl_fill_rect(cv, fl_r(r.x, r.y, (int)((double)r.w * frac), r.h), fl_th.accent);
}

static void fmt_date(time_t t, char *out, size_t cap)
{
	if (!t) {
		out[0] = 0;
		return;
	}
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(out, cap, "%Y-%m-%d  %H:%M", &tm);
}

/* the list of the archive's current folder; returns the row activated (double click or Enter) or -1 */
static int draw_rows(fl_ui *ui, struct A *a, fl_rect r)
{
	fl_canvas *cv = &ui->cv;
	int res = fl_widget(ui, ID_LIST, r, FL_FOCUSABLE | FL_NO_FOCUS_RING);
	int content = a->nrows * ROW_H, max = content > r.h ? content - r.h : 0, act = -1;
	if ((res & FL_HOVER) && ui->wheel) {
		a->scroll += ui->wheel * 45;
		ui->wheel = 0;
	}
	if ((res & FL_HOVER) && (ui->press & FL_BTN_L)) {
		int i = (ui->my - r.y + a->scroll) / ROW_H;
		if (i >= 0 && i < a->nrows) {
			a->sel = i;
			if (ui->clicks >= 2)
				act = i;
		}
	}
	if (ui->key && !(ui->mods & FL_MOD_CTRL) && a->nrows) {
		int page = r.h / ROW_H > 1 ? r.h / ROW_H - 1 : 1, moved = 1;
		switch (ui->key) {
		case XKB_KEY_Down: a->sel++; break;
		case XKB_KEY_Up: a->sel--; break;
		case XKB_KEY_Page_Down: a->sel += page; break;
		case XKB_KEY_Page_Up: a->sel -= page; break;
		case XKB_KEY_Home: a->sel = 0; break;
		case XKB_KEY_End: a->sel = a->nrows - 1; break;
		case XKB_KEY_Return:
		case XKB_KEY_KP_Enter:
			act = a->sel;
			moved = 0;
			ui->key = 0;
			break;
		default: moved = 0; break;
		}
		if (moved) {
			ui->key = 0;
			if (a->sel < 0)
				a->sel = 0;
			if (a->sel >= a->nrows)
				a->sel = a->nrows - 1;
			if (a->sel * ROW_H < a->scroll)
				a->scroll = a->sel * ROW_H;
			if ((a->sel + 1) * ROW_H > a->scroll + r.h)
				a->scroll = (a->sel + 1) * ROW_H - r.h;
		}
	}
	if (a->scroll > max)
		a->scroll = max;
	if (a->scroll < 0)
		a->scroll = 0;
	fl_rect save = cv->clip;
	fl_clip(cv, r);
	int first = a->scroll / ROW_H;
	int sizew = 90, datew = 150;
	for (int i = first; i < a->nrows && r.y + i * ROW_H - a->scroll < r.y + r.h; i++) {
		fl_rect ir = fl_r(r.x, r.y + i * ROW_H - a->scroll, r.w - 12, ROW_H);
		const struct row *row = &a->rows[i];
		if (i == a->sel)
			fl_fill_rect(cv, ir, fl_th.select);
		else if ((res & FL_HOVER) && fl_in(ir, ui->mx, ui->my))
			fl_fill_rect(cv, ir, fl_th.panel);
		fl_icon_draw(cv, row->dir ? FL_ICON_FOLDER : FL_ICON_FILE, ir.x + 16, ir.y + (ROW_H - 18) / 2, 18, row->dir ? fl_th.accent : fl_th.dim);
		char name[NAME_MAX + 1], fit[NAME_MAX + 8];
		snprintf(name, sizeof name, "%.*s", (int)row->nlen, row->name);
		int nx = ir.x + 46, nw = ir.w - 46 - sizew - datew - 20;
		fl_text_fit(ui->f_reg, name, nw > 40 ? nw : 40, fit, sizeof fit);
		fl_text_in(cv, ui->f_reg, fl_r(nx, ir.y, nw > 40 ? nw : 40, ROW_H), FL_LEFT, 0, fl_th.text, fit);
		char sz[32] = "", dt[40];
		if (!row->dir)
			fmt_size((double)row->size, sz, sizeof sz);
		fl_text_in(cv, a->f_small, fl_r(ir.x + ir.w - datew - sizew - 8, ir.y, sizew, ROW_H), FL_RIGHT, 0, fl_th.dim, sz);
		fmt_date(row->mtime, dt, sizeof dt);
		fl_text_in(cv, a->f_small, fl_r(ir.x + ir.w - datew + 8, ir.y, datew - 8, ROW_H), FL_LEFT, 0, fl_th.dim, dt);
	}
	cv->clip = save;
	fl_scrollbar(ui, ID_VSB, fl_r(r.x + r.w - 8, r.y + 2, 4, r.h - 4), &a->scroll, content, r.h);
	return act;
}

static void draw_empty(fl_ui *ui, struct A *a, fl_rect r)
{
	fl_canvas *cv = &ui->cv;
	int cx = r.x + r.w / 2, y = r.y + r.h / 2 - 90;
	fl_icon_draw(cv, FL_ICON_FILE_LG, cx - 24, y, 48, fl_th.dim);
	y += 66;
	const char *t = "Open an archive";
	fl_text(cv, a->f_title, cx - fl_text_width(a->f_title, t, -1) / 2, y, fl_th.strong, t, -1);
	y += 32;
	const char *h1 = "zip, tar (gz, bz2, xz, zst), 7z, rar and more.";
	fl_text(cv, ui->f_reg, cx - fl_text_width(ui->f_reg, h1, -1) / 2, y, fl_th.dim, h1, -1);
	y += 22;
	const char *h2 = "Or make a new zip or tar.zst from files and folders.";
	fl_text(cv, ui->f_reg, cx - fl_text_width(ui->f_reg, h2, -1) / 2, y, fl_th.dim, h2, -1);
	y += 44;
	if (fl_button(ui, ID_OPEN_NEW, fl_r(cx - 150, y, 140, 36), "Open archive", FL_BTN_ACCENT))
		prompt_open(a, PR_OPEN, "Open archive", "Open", a->apath[0] ? a->apath : "~/");
	if (fl_button(ui, ID_NEW, fl_r(cx + 10, y, 140, 36), "New archive", FL_BTN_OUTLINE)) {
		a->mode = M_CREATE;
		if (!a->ninputs)
			create_defaults(a);
	}
}

static void draw_browse(fl_ui *ui, struct A *a, fl_rect c)
{
	fl_canvas *cv = &ui->cv;
	fl_rect area = c;
	fl_rect tool = fl_cut_top(&area, TOOL_H);
	fl_rect status = fl_cut_bottom(&area, STATUS_H);
	fl_fill_rect(cv, tool, fl_th.panel);
	fl_fill_rect(cv, fl_r(tool.x, tool.y + tool.h - 1, tool.w, 1), fl_th.line);
	int busy = a->job.running;
	int x = tool.x + 12, y = tool.y + (TOOL_H - 34) / 2;
	if (fl_button(ui, ID_OPEN, fl_r(x, y, 76, 34), "Open", FL_BTN_OUTLINE))
		prompt_open(a, PR_OPEN, "Open archive", "Open", a->apath[0] ? a->apath : "~/");
	x += 84;
	if (fl_button(ui, ID_NEW, fl_r(x, y, 76, 34), "New", FL_BTN_OUTLINE)) {
		a->mode = M_CREATE;
		if (!a->ninputs)
			create_defaults(a);
	}
	x += 92;
	int rx = tool.x + tool.w - 12;
	int can = !busy && a->mode == M_BROWSE && a->nitems > 0;
	rx -= 150;
	if (fl_button(ui, ID_EXTRACT_ALL, fl_r(rx, y, 150, 34), "Extract all", can ? FL_BTN_ACCENT : FL_BTN_OUTLINE) && can)
		prompt_extract(a, 0);
	rx -= 8 + 150;
	if (fl_button(ui, ID_EXTRACT_SEL, fl_r(rx, y, 150, 34), "Extract selected", FL_BTN_OUTLINE) && can && a->nrows)
		prompt_extract(a, 1);
	/* the archive's name between the buttons */
	char nm[NAME_MAX + 1], fit[NAME_MAX + 8];
	snprintf(nm, sizeof nm, "%s", base_name(a->apath));
	int nw = rx - 8 - x;
	if (nw > 60) {
		fl_text_fit(ui->f_semi, nm, nw, fit, sizeof fit);
		fl_text_in(cv, ui->f_semi, fl_r(x, tool.y, nw, TOOL_H), FL_LEFT, 0, fl_th.strong, fit);
	}

	/* where we are in the archive */
	fl_rect path = fl_cut_top(&area, PATH_H);
	fl_fill_rect(cv, fl_r(path.x, path.y + path.h - 1, path.w, 1), fl_th.line);
	int atroot = !a->cwd[0];
	if (fl_icon_button(ui, ID_UP, fl_r(path.x + 8, path.y + 3, 30, 30), FL_ICON_UP, 14, atroot ? fl_th.line : fl_th.text) && !atroot)
		go_up(a);
	char where[PATH_MAX * 2];
	size_t wl = (size_t)snprintf(where, sizeof where, "%s", base_name(a->apath));
	for (const char *p = a->cwd; *p && wl + 6 < sizeof where; p++) {
		if (p == a->cwd || p[-1] == '/') {
			memcpy(where + wl, "  /  ", 5);
			wl += 5;
		}
		if (*p != '/')
			where[wl++] = *p;
	}
	where[wl] = 0;
	fl_text_fit(ui->f_reg, where, path.w - 60, fit, sizeof fit);
	fl_text_in(cv, ui->f_reg, fl_r(path.x + 48, path.y, path.w - 60, path.h), FL_LEFT, 0, fl_th.text, fit);

	/* column heads */
	fl_rect head = fl_cut_top(&area, HEAD_H);
	fl_fill_rect(cv, head, fl_th.bg);
	fl_text_in(cv, a->f_small, fl_r(head.x + 46, head.y, 200, HEAD_H), FL_LEFT, 0, fl_th.dim, "Name");
	fl_text_in(cv, a->f_small, fl_r(head.x + head.w - 150 - 90 - 20, head.y, 90, HEAD_H), FL_RIGHT, 0, fl_th.dim, "Size");
	fl_text_in(cv, a->f_small, fl_r(head.x + head.w - 150 + 8 - 12, head.y, 150, HEAD_H), FL_LEFT, 0, fl_th.dim, "Modified");
	fl_fill_rect(cv, fl_r(head.x, head.y + head.h - 1, head.w, 1), fl_th.line);

	if (a->nrows == 0 && !busy) {
		const char *t = a->nitems ? "This folder is empty." : (a->msg[0] ? a->msg : "Nothing here.");
		fl_text_in(cv, ui->f_reg, area, FL_CENTER, 0, fl_th.dim, t);
	} else {
		int act = draw_rows(ui, a, area);
		if (act >= 0 && act < a->nrows && a->rows[act].dir && !busy)
			enter_folder(a, a->rows[act].name, a->rows[act].nlen);
		else if (act >= 0 && act < a->nrows && !a->rows[act].dir && !busy)
			prompt_extract(a, 1);
		if (ui->key == XKB_KEY_BackSpace && !atroot && !busy) {
			go_up(a);
			ui->key = 0;
		}
	}

	/* status bar: counts, or the progress of the job */
	fl_fill_rect(cv, status, fl_th.panel);
	fl_fill_rect(cv, fl_r(status.x, status.y, status.w, 1), fl_th.line);
	if (busy) {
		char t[96];
		if (a->job.kind == J_LIST) {
			snprintf(t, sizeof t, "Reading the archive... %zu items", a->nitems);
		} else {
			snprintf(t, sizeof t, "%s %d%%", a->job.kind == J_EXTRACT ? "Extracting" : "Creating", (int)(a->job.frac * 100));
		}
		fl_text_in(cv, a->f_small, fl_r(status.x + 14, status.y + 1, 260, status.h - 1), FL_LEFT, 0, fl_th.text, t);
		int cw = 76;
		if (a->job.kind != J_LIST)
			draw_progress(ui, fl_r(status.x + 280, status.y + 12, status.w - 280 - cw - 30, 8), a->job.frac);
		if (fl_button(ui, ID_CANCEL, fl_r(status.x + status.w - cw - 8, status.y + 3, cw, 26), "Cancel", FL_BTN_OUTLINE))
			cancel_job(a);
	} else {
		char left[160], un[32];
		fmt_size(a->unpacked, un, sizeof un);
		snprintf(left, sizeof left, "%zu item%s, %s unpacked", a->nitems, a->nitems == 1 ? "" : "s", un);
		int lw = fl_text_width(a->f_small, left, -1);
		fl_text_in(cv, a->f_small, fl_r(status.x + status.w - 14 - lw, status.y + 1, lw + 14, status.h - 1), FL_LEFT, 0, fl_th.dim, left);
		if (a->msg[0]) {
			fl_text_in(cv, a->f_small, fl_r(status.x + 14, status.y + 1, status.w - lw - 50, status.h - 1), FL_LEFT, 0,
			           a->msg_err ? fl_th.err : fl_th.text, a->msg);
			if (a->job.ok && a->job.kind == J_EXTRACT && a->job.result[0] &&
			    fl_button(ui, ID_SHOW, fl_r(status.x + status.w - lw - 30 - 130, status.y + 3, 120, 26), "Show in Files", FL_BTN_OUTLINE))
				show_in_files(a->job.result);
		}
	}
}

/* ---- the "new archive" page -------------------------------------------------------------------------------------- */

static void input_rm(struct A *a, int i)
{
	if (i < 0 || i >= a->ninputs)
		return;
	free(a->inputs[i]);
	memmove(a->inputs + i, a->inputs + i + 1, (size_t)(a->ninputs - i - 1) * sizeof *a->inputs);
	a->ninputs--;
	if (a->in_sel >= a->ninputs)
		a->in_sel = a->ninputs - 1;
	if (a->in_sel < 0)
		a->in_sel = 0;
}

static void draw_create(fl_ui *ui, struct A *a, fl_rect c)
{
	fl_canvas *cv = &ui->cv;
	fl_rect area = c;
	fl_rect tool = fl_cut_top(&area, TOOL_H);
	fl_rect status = fl_cut_bottom(&area, STATUS_H);
	fl_fill_rect(cv, tool, fl_th.panel);
	fl_fill_rect(cv, fl_r(tool.x, tool.y + tool.h - 1, tool.w, 1), fl_th.line);
	int busy = a->job.running;
	int y = tool.y + (TOOL_H - 34) / 2;
	fl_text_in(cv, ui->f_semi, fl_r(tool.x + 16, tool.y, 300, TOOL_H), FL_LEFT, 0, fl_th.strong, "New archive");
	if (!a->from_cli_compress || a->nitems) {
		/* back to browsing (or the empty page) */
	}
	if (fl_button(ui, ID_BACK, fl_r(tool.x + tool.w - 12 - 96, y, 96, 34), a->from_cli_compress ? "Close" : "Back", FL_BTN_OUTLINE) && !busy) {
		if (a->from_cli_compress)
			fl_app_quit(a->app);
		else
			a->mode = a->apath[0] ? M_BROWSE : M_EMPTY;
	}

	int pad = 28, w = area.w - 2 * pad;
	if (w > 760)
		w = 760;
	int x = area.x + (area.w - w) / 2;
	y = area.y + 20;
	fl_text(cv, ui->f_semi, x, y, fl_th.dim, "Files and folders", -1);
	y += 24;
	int lh = 28, rows = a->ninputs < 5 ? 5 : a->ninputs;
	int lh_total = area.h - 20 - 24 - 24 - 40 - 24 - 36 - 36 - 36 - 36 - 44 - 20;
	int maxrows = lh_total / lh > 3 ? lh_total / lh : 3;
	if (rows > maxrows)
		rows = maxrows;
	fl_rect list = fl_r(x, y, w, rows * lh);
	fl_fill_rect(cv, list, fl_th.panel);
	fl_stroke_rect(cv, list, 1, fl_th.line);
	int res = fl_widget(ui, ID_INLIST, list, 0);
	if ((res & FL_HOVER) && ui->wheel) {
		a->in_scroll += ui->wheel * lh;
		ui->wheel = 0;
	}
	int maxs = a->ninputs * lh > list.h ? a->ninputs * lh - list.h : 0;
	if (a->in_scroll > maxs)
		a->in_scroll = maxs;
	if (a->in_scroll < 0)
		a->in_scroll = 0;
	if ((res & FL_HOVER) && (ui->press & FL_BTN_L)) {
		int i = (ui->my - list.y + a->in_scroll) / lh;
		if (i >= 0 && i < a->ninputs)
			a->in_sel = i;
	}
	fl_rect save = cv->clip;
	fl_clip(cv, fl_inset(list, 1, 1));
	if (!a->ninputs)
		fl_text_in(cv, ui->f_reg, list, FL_CENTER, 0, fl_th.dim, "Nothing added yet. Add a file or folder below.");
	for (int i = 0; i < a->ninputs; i++) {
		fl_rect ir = fl_r(list.x + 1, list.y + i * lh - a->in_scroll, list.w - 2, lh);
		if (ir.y + ir.h < list.y || ir.y > list.y + list.h)
			continue;
		if (i == a->in_sel)
			fl_fill_rect(cv, ir, fl_th.select);
		struct stat st;
		int isdir = stat(a->inputs[i], &st) == 0 && S_ISDIR(st.st_mode);
		fl_icon_draw(cv, isdir ? FL_ICON_FOLDER : FL_ICON_FILE, ir.x + 10, ir.y + (lh - 16) / 2, 16, isdir ? fl_th.accent : fl_th.dim);
		char fit[PATH_MAX];
		fl_text_fit(ui->f_reg, a->inputs[i], ir.w - 40, fit, sizeof fit);
		fl_text_in(cv, ui->f_reg, fl_r(ir.x + 34, ir.y, ir.w - 40, lh), FL_LEFT, 0, fl_th.text, fit);
	}
	cv->clip = save;
	y += list.h + 10;
	if (fl_button(ui, ID_ADD, fl_r(x, y, 150, 32), "Add file or folder", FL_BTN_OUTLINE) && !busy)
		prompt_open(a, PR_ADD, "Add a file or folder", "Add", "~/");
	if (fl_button(ui, ID_REMOVE, fl_r(x + 158, y, 84, 32), "Remove", FL_BTN_OUTLINE) && !busy)
		input_rm(a, a->in_sel);
	y += 32 + 22;

	fl_text(cv, ui->f_semi, x, y, fl_th.dim, "Format", -1);
	y += 24;
	if (fl_button(ui, ID_FMT_ZIP, fl_r(x, y, 110, 34), "Zip", a->fmt == FMT_ZIP ? FL_BTN_ACCENT : FL_BTN_OUTLINE) && !busy)
		a->fmt = FMT_ZIP;
	if (fl_button(ui, ID_FMT_TZST, fl_r(x + 118, y, 150, 34), "Tar + Zstd", a->fmt == FMT_TZST ? FL_BTN_ACCENT : FL_BTN_OUTLINE) && !busy)
		a->fmt = FMT_TZST;
	fl_text_in(cv, a->f_small, fl_r(x + 284, y, w - 284, 34), FL_LEFT, 0, fl_th.dim,
	           a->fmt == FMT_ZIP ? "Opens everywhere." : "Smaller and faster; keeps Unix permissions.");
	y += 34 + 18;

	fl_text(cv, ui->f_semi, x, y, fl_th.dim, "Name", -1);
	fl_text(cv, ui->f_semi, x + w / 2 + 8, y, fl_th.dim, "Save in", -1);
	y += 24;
	const char *ext = a->fmt == FMT_ZIP ? ".zip" : ".tar.zst";
	int extw = fl_text_width(ui->f_reg, ext, -1) + 10;
	int nw = w / 2 - 8;
	fl_textfield(ui, ID_NAME, fl_r(x, y, nw - extw, 36), a->name, sizeof a->name, "Archive", ui->f_reg);
	fl_text_in(cv, ui->f_reg, fl_r(x + nw - extw + 6, y, extw, 36), FL_LEFT, 0, fl_th.dim, ext);
	fl_textfield(ui, ID_DEST, fl_r(x + w / 2 + 8, y, w / 2 - 8, 36), a->dest, sizeof a->dest, "Folder", ui->f_reg);
	y += 36 + 20;
	if (a->created && a->job.result[0] && !busy) {
		if (fl_button(ui, ID_SHOW, fl_r(x, y, 130, 36), "Show in Files", FL_BTN_OUTLINE))
			show_in_files(a->job.result);
		if (fl_button(ui, ID_OPEN_NEW, fl_r(x + 138, y, 130, 36), "Open archive", FL_BTN_OUTLINE)) {
			char p[PATH_MAX];
			snprintf(p, sizeof p, "%s", a->job.result);
			a->mode = M_BROWSE;
			open_archive(a, p);
		}
	}
	int can = a->ninputs > 0 && !busy;
	if (fl_button(ui, ID_CREATE, fl_r(x + w - 140, y, 140, 36), "Create", can ? FL_BTN_ACCENT : FL_BTN_OUTLINE) && can)
		start_create(a);

	fl_fill_rect(cv, status, fl_th.panel);
	fl_fill_rect(cv, fl_r(status.x, status.y, status.w, 1), fl_th.line);
	if (busy) {
		char t[64];
		snprintf(t, sizeof t, "Creating %d%%", (int)(a->job.frac * 100));
		fl_text_in(cv, a->f_small, fl_r(status.x + 14, status.y + 1, 200, status.h - 1), FL_LEFT, 0, fl_th.text, t);
		draw_progress(ui, fl_r(status.x + 200, status.y + 12, status.w - 200 - 76 - 30, 8), a->job.frac);
		if (fl_button(ui, ID_CANCEL, fl_r(status.x + status.w - 76 - 8, status.y + 3, 76, 26), "Cancel", FL_BTN_OUTLINE))
			cancel_job(a);
	} else if (a->msg[0]) {
		fl_text_in(cv, a->f_small, fl_r(status.x + 14, status.y + 1, status.w - 28, status.h - 1), FL_LEFT, 0,
		           a->msg_err ? fl_th.err : fl_th.text, a->msg);
	}
}

/* the "Extracting ..." page of --extract-here */
static void draw_cli_extract(fl_ui *ui, struct A *a, fl_rect c)
{
	fl_canvas *cv = &ui->cv;
	int cx = c.x + c.w / 2, y = c.y + c.h / 2 - 70;
	char t[PATH_MAX + 32], fit[PATH_MAX + 32];
	int busy = a->job.running;
	snprintf(t, sizeof t, "%s", busy ? "Extracting" : (a->job.err[0] && !a->job.ok ? "Could not extract" : "Done"));
	fl_text(cv, a->f_title, cx - fl_text_width(a->f_title, t, -1) / 2, y, fl_th.strong, t, -1);
	y += 34;
	snprintf(t, sizeof t, "%s%s", base_name(a->apath), a->nq > 1 ? "" : "");
	fl_text_fit(ui->f_reg, t, c.w - 80, fit, sizeof fit);
	fl_text(cv, ui->f_reg, cx - fl_text_width(ui->f_reg, fit, -1) / 2, y, fl_th.text, fit, -1);
	y += 30;
	int w = c.w - 120 > 460 ? 460 : c.w - 120;
	double frac = a->job.running ? a->job.frac : 1;
	draw_progress(ui, fl_r(cx - w / 2, y, w, 8), frac);
	y += 18;
	if (a->job.running) {
		snprintf(t, sizeof t, "%d%%", (int)(frac * 100));
		fl_text(cv, a->f_small, cx - fl_text_width(a->f_small, t, -1) / 2, y, fl_th.dim, t, -1);
	}
	y += 20;
	if (a->nq > 1) {
		snprintf(t, sizeof t, "Archive %d of %d", a->qi < a->nq ? a->qi : a->nq, a->nq);
		fl_text(cv, a->f_small, cx - fl_text_width(a->f_small, t, -1) / 2, y, fl_th.dim, t, -1);
		y += 22;
	}
	if (a->msg[0] && !busy)
		fl_text(cv, ui->f_reg, cx - fl_text_width(ui->f_reg, a->msg, -1) / 2, y, a->msg_err ? fl_th.err : fl_th.dim, a->msg, -1);
	if (busy && fl_button(ui, ID_CANCEL, fl_r(cx - 48, y + 24, 96, 34), "Cancel", FL_BTN_OUTLINE)) {
		a->qi = a->nq; /* nothing more after this one */
		cancel_job(a);
	}
	if (!busy && fl_button(ui, ID_CLOSE, fl_r(cx - 48, y + 24, 96, 34), "Close", FL_BTN_OUTLINE))
		fl_app_quit(a->app);
}

static void dialog_frame(fl_ui *ui, int w, int h, fl_rect *box)
{
	fl_canvas *cv = &ui->cv;
	fl_fill_rect(cv, fl_r(0, 0, cv->w, cv->h), fl_th.dark ? 0x99000000 : 0x66000000);
	*box = fl_r((cv->w - w) / 2, (cv->h - h) / 2 - 20, w, h);
	fl_fill_rect(cv, *box, fl_th.panel);
	fl_stroke_rect(cv, *box, 1, fl_th.line);
	fl_fill_rect(cv, fl_r(box->x, box->y, w, 3), fl_th.accent);
}

static void draw_overlay(fl_ui *ui, struct A *a)
{
	fl_canvas *cv = &ui->cv;
	fl_rect box;
	int esc = 0;
	if (ui->key == XKB_KEY_Escape) {
		esc = 1;
		ui->key = 0;
	}
	if (a->ov == OV_PROMPT) {
		dialog_frame(ui, 560, 176, &box);
		int x = box.x + 24, w = box.w - 48, y = box.y + 22;
		fl_text(cv, ui->f_semi, x, y, fl_th.strong, a->prompt_title, -1);
		y += 28;
		if (a->ov_new) {
			ui->focus = ID_DLG;
			a->ov_new = 0;
		}
		if (ui->key == XKB_KEY_Tab) {
			complete_path(a, a->prompt_kind == PR_EXTRACT);
			ui->key = 0;
			ui->focus = ID_DLG;
		}
		int ch = fl_textfield(ui, ID_DLG, fl_r(x, y, w, 36), a->prompt_text, sizeof a->prompt_text, "Path", ui->f_reg);
		if (ch & FL_EDIT_CHANGED)
			a->prompt_err[0] = 0;
		y += 42;
		if (a->prompt_err[0]) {
			fl_text(cv, a->f_small, x, y, fl_th.err, a->prompt_err, -1);
		} else {
			fl_text(cv, a->f_small, x, y, fl_th.dim,
			        a->prompt_kind == PR_EXTRACT ? "A new folder is made in it; nothing is overwritten. Tab completes the name." : "Tab completes the name.", -1);
		}
		int by = box.y + box.h - 24 - 34;
		char ok[24];
		snprintf(ok, sizeof ok, "%s", a->prompt_ok);
		int okc = fl_button(ui, ID_DLG + 1, fl_r(x + w - 110, by, 110, 34), ok, FL_BTN_ACCENT);
		int cancel = fl_button(ui, ID_DLG + 2, fl_r(x + w - 110 - 8 - 96, by, 96, 34), "Cancel", FL_BTN_OUTLINE);
		if (okc || (ch & FL_EDIT_ENTER))
			prompt_confirm(a);
		else if (cancel || esc)
			a->ov = OV_NONE;
	} else if (a->ov == OV_ERROR) {
		dialog_frame(ui, 520, 170, &box);
		int x = box.x + 24, w = box.w - 48, y = box.y + 22;
		fl_text(cv, ui->f_semi, x, y, fl_th.strong, "That did not work", -1);
		y += 30;
		/* wrap the message on two lines */
		char l1[500], fit[500];
		snprintf(l1, sizeof l1, "%s", a->err_text);
		if (fl_text_width(ui->f_reg, l1, -1) > w) {
			size_t cut = strlen(l1);
			while (cut > 0 && fl_text_width(ui->f_reg, l1, (int)cut) > w)
				cut--;
			while (cut > 0 && l1[cut] != ' ')
				cut--;
			if (!cut)
				cut = strlen(l1) / 2;
			fl_text(cv, ui->f_reg, x, y, fl_th.err, l1, (int)cut);
			fl_text_fit(ui->f_reg, l1 + cut + (l1[cut] == ' '), w, fit, sizeof fit);
			fl_text(cv, ui->f_reg, x, y + 22, fl_th.err, fit, -1);
		} else {
			fl_text(cv, ui->f_reg, x, y, fl_th.err, l1, -1);
		}
		int by = box.y + box.h - 24 - 34;
		if (fl_button(ui, ID_DLG + 1, fl_r(x + w - 96, by, 96, 34), "OK", FL_BTN_ACCENT) || esc)
			a->ov = OV_NONE;
	}
	ui->key = 0;
	ui->text[0] = 0;
	ui->press = ui->release = 0;
}

static void global_keys(fl_ui *ui, struct A *a)
{
	if (!ui->key)
		return;
	int ctrl = ui->mods & FL_MOD_CTRL, alt = ui->mods & FL_MOD_ALT;
	int used = 1;
	if (ctrl && (ui->key == 'q' || ui->key == 'Q' || ui->key == 'w' || ui->key == 'W')) {
		cancel_job(a);
		fl_app_quit(a->app);
	} else if (ctrl && (ui->key == 'o' || ui->key == 'O') && !a->cli_extract && !a->job.running) {
		prompt_open(a, PR_OPEN, "Open archive", "Open", a->apath[0] ? a->apath : "~/");
	} else if (ctrl && (ui->key == 'n' || ui->key == 'N') && !a->cli_extract && !a->job.running) {
		a->mode = M_CREATE;
		if (!a->ninputs)
			create_defaults(a);
	} else if (ctrl && (ui->key == 'e' || ui->key == 'E') && a->mode == M_BROWSE && a->nitems && !a->job.running) {
		prompt_extract(a, (ui->mods & FL_MOD_SHIFT) != 0 && a->nrows > 0); /* Ctrl+E all, Ctrl+Shift+E the selected row */
	} else if (alt && ui->key == XKB_KEY_Up && a->mode == M_BROWSE && a->cwd[0] && !a->job.running) {
		go_up(a);
	} else {
		used = 0;
	}
	if (used)
		ui->key = 0;
}

static void draw(fl_ui *ui, void *ud)
{
	struct A *a = ud;
	fl_canvas *cv = &ui->cv;
	fl_rect c = fl_win_content(ui->win);
	int modal = a->ov != OV_NONE;
	int sv_mouse = ui->mouse_in, sv_press = ui->press, sv_rel = ui->release, sv_wheel = ui->wheel, sv_held = ui->btn_held;
	uint32_t sv_key = ui->key;
	char sv_text[16];
	memcpy(sv_text, ui->text, sizeof sv_text);
	if (modal) {
		ui->mouse_in = ui->press = ui->release = ui->wheel = ui->btn_held = 0;
		ui->key = 0;
		ui->text[0] = 0;
	} else {
		global_keys(ui, a);
	}
	fl_fill_rect(cv, c, fl_th.bg);
	if (a->cli_extract)
		draw_cli_extract(ui, a, c);
	else if (a->mode == M_CREATE)
		draw_create(ui, a, c);
	else if (a->mode == M_BROWSE)
		draw_browse(ui, a, c);
	else {
		fl_rect area = c;
		fl_rect tool = fl_cut_top(&area, TOOL_H);
		fl_fill_rect(cv, tool, fl_th.panel);
		fl_fill_rect(cv, fl_r(tool.x, tool.y + tool.h - 1, tool.w, 1), fl_th.line);
		fl_rect status = fl_cut_bottom(&area, STATUS_H);
		fl_fill_rect(cv, status, fl_th.panel);
		fl_fill_rect(cv, fl_r(status.x, status.y, status.w, 1), fl_th.line);
		draw_empty(ui, a, area);
	}
	if (modal) {
		ui->mouse_in = sv_mouse;
		ui->press = sv_press;
		ui->release = sv_rel;
		ui->wheel = sv_wheel;
		ui->btn_held = sv_held;
		ui->key = sv_key;
		memcpy(ui->text, sv_text, sizeof sv_text);
		draw_overlay(ui, a);
		if (a->ov == OV_NONE) {
			ui->focus = 0;
			fl_win_redraw(ui->win);
		}
	} else if (a->ov != OV_NONE) {
		fl_win_redraw(ui->win);
	}
}

static void close_now(void *ud)
{
	struct A *a = ud;
	fl_app_quit(a->app);
}

static void closed(fl_win *w, void *ud)
{
	struct A *a = ud;
	cancel_job(a);
	fl_app_quit(a->app);
}

int main(int argc, char **argv)
{
	signal(SIGCHLD, SIG_DFL);
	signal(SIGPIPE, SIG_IGN);
	int mode = 0; /* 0 browse, 1 extract here, 2 compress, 3 new */
	int first = 1;
	if (argc >= 2 && argv[1][0] == '-' && argv[1][1]) {
		if (!strcmp(argv[1], "--extract-here"))
			mode = 1;
		else if (!strcmp(argv[1], "--compress"))
			mode = 2;
		else if (!strcmp(argv[1], "--new"))
			mode = 3;
		else {
			fprintf(stderr, "usage: flarchive [ARCHIVE]\n       flarchive --extract-here ARCHIVE...\n"
			                "       flarchive --compress FILE...\n       flarchive --new\n"
			                "Opens, extracts and creates zip, tar.*, 7z, rar and more.\n");
			return !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") ? 0 : 2;
		}
		first = 2;
	}
	if (mode == 1 && argc < 3) {
		fprintf(stderr, "flarchive: --extract-here needs an archive\n");
		return 2;
	}
	struct A *a = calloc(1, sizeof *a);
	if (!a)
		return 1;
	a->app = fl_app_new(NULL, NULL);
	if (!a->app) {
		fprintf(stderr, "flarchive: cannot connect to a Wayland compositor\n");
		return 1;
	}
	a->f_title = fl_font_ui(600, 18);
	a->f_small = fl_font_ui(400, 12);
	a->close_timer = fl_timer_new(a->app, close_now, a);
	fl_win_opts o = {
		.title = "Archives", .app_id = "flarchive", .w = mode == 1 ? 520 : 900, .h = mode == 1 ? 300 : 600,
		.min_w = mode == 1 ? 420 : 680, .min_h = mode == 1 ? 240 : 480, .decorated = 1, .resizable = 1,
		.draw = draw, .close = closed, .ud = a,
	};
	a->win = fl_win_new(a->app, &o);
	if (!a->win)
		return 1;
	if (mode == 1) {
		a->cli_extract = 1;
		for (int i = first; i < argc; i++) {
			char full[PATH_MAX];
			if (!realpath(argv[i], full))
				continue;
			a->queue = realloc(a->queue, (size_t)(a->nq + 1) * sizeof *a->queue);
			a->queue[a->nq++] = strdup(full);
		}
		if (!a->nq) {
			fprintf(stderr, "flarchive: no such file\n");
			return 1;
		}
		queue_next(a);
	} else if (mode == 2 || mode == 3) {
		a->mode = M_CREATE;
		a->from_cli_compress = 1;
		for (int i = first; i < argc; i++) {
			char err[200];
			if (add_input(a, argv[i], err, sizeof err) < 0)
				fprintf(stderr, "flarchive: %s\n", err);
		}
		create_defaults(a);
	} else if (argc > first) {
		open_archive(a, argv[first]);
	}
	fl_app_watch_conf(a->app, NULL, NULL);
	fl_app_run(a->app);
	cancel_job(a);
	fl_app_free(a->app);
	return 0;
}
