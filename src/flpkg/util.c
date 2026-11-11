/* Small helpers: messages, allocation, strings, files.
 * SPDX-License-Identifier: MIT */
#include "flpkg.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char *progname = "flpkg";
int verbose;
void (*die_hook)(void);

static void vreport(const char *prefix, const char *fmt, va_list ap)
{
	fflush(stdout);
	fprintf(stderr, "%s: %s", progname, prefix);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
}

void die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vreport("error: ", fmt, ap);
	va_end(ap);
	if (die_hook) {
		void (*hook)(void) = die_hook;
		die_hook = NULL;
		hook();
	}
	exit(1);
}

void warn(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vreport("warning: ", fmt, ap);
	va_end(ap);
}

void msg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs(":: ", stdout);
	vprintf(fmt, ap);
	fputc('\n', stdout);
	va_end(ap);
}

void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p)
		die("out of memory");
	return p;
}

void *xcalloc(size_t n, size_t size)
{
	void *p = calloc(n ? n : 1, size ? size : 1);
	if (!p)
		die("out of memory");
	return p;
}

void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n ? n : 1);
	if (!p)
		die("out of memory");
	return p;
}

char *xstrdup(const char *s)
{
	return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
	char *p = xmalloc(n + 1);
	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

char *xasprintf(const char *fmt, ...)
{
	struct strbuf sb = {0};
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0)
		die("bad format string");
	sb.buf = xmalloc((size_t)n + 1);
	va_start(ap, fmt);
	vsnprintf(sb.buf, (size_t)n + 1, fmt, ap);
	va_end(ap);
	return sb.buf;
}

/* ---- strbuf -------------------------------------------------------------- */

static void sb_grow(struct strbuf *sb, size_t extra)
{
	if (sb->len + extra + 1 <= sb->cap)
		return;
	size_t cap = sb->cap ? sb->cap : 64;
	while (cap < sb->len + extra + 1)
		cap *= 2;
	sb->buf = xrealloc(sb->buf, cap);
	sb->cap = cap;
}

void sb_add(struct strbuf *sb, const void *data, size_t len)
{
	sb_grow(sb, len);
	memcpy(sb->buf + sb->len, data, len);
	sb->len += len;
	sb->buf[sb->len] = '\0';
}

void sb_addstr(struct strbuf *sb, const char *s)
{
	sb_add(sb, s, strlen(s));
}

void sb_addch(struct strbuf *sb, char c)
{
	sb_add(sb, &c, 1);
}

void sb_addf(struct strbuf *sb, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(NULL, 0, fmt, ap);
	va_end(ap);
	if (n < 0)
		die("bad format string");
	sb_grow(sb, (size_t)n);
	va_start(ap, fmt);
	vsnprintf(sb->buf + sb->len, (size_t)n + 1, fmt, ap);
	va_end(ap);
	sb->len += (size_t)n;
}

char *sb_steal(struct strbuf *sb)
{
	char *s = sb->buf ? sb->buf : xstrdup("");
	sb->buf = NULL;
	sb->len = sb->cap = 0;
	return s;
}

void sb_free(struct strbuf *sb)
{
	free(sb->buf);
	sb->buf = NULL;
	sb->len = sb->cap = 0;
}

/* ---- strlist ------------------------------------------------------------- */

void sl_add_owned(struct strlist *sl, char *s)
{
	if (sl->n == sl->cap) {
		sl->cap = sl->cap ? sl->cap * 2 : 8;
		sl->v = xrealloc(sl->v, sl->cap * sizeof(*sl->v));
	}
	sl->v[sl->n++] = s;
}

void sl_add(struct strlist *sl, const char *s)
{
	sl_add_owned(sl, xstrdup(s));
}

bool sl_has(const struct strlist *sl, const char *s)
{
	for (size_t i = 0; i < sl->n; i++)
		if (strcmp(sl->v[i], s) == 0)
			return true;
	return false;
}

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

void sl_sort(struct strlist *sl)
{
	if (sl->n > 1)
		qsort(sl->v, sl->n, sizeof(*sl->v), cmp_str);
}

void sl_free(struct strlist *sl)
{
	for (size_t i = 0; i < sl->n; i++)
		free(sl->v[i]);
	free(sl->v);
	sl->v = NULL;
	sl->n = sl->cap = 0;
}

/* ---- strings and paths --------------------------------------------------- */

bool has_prefix(const char *s, const char *prefix)
{
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

bool has_suffix(const char *s, const char *suffix)
{
	size_t n = strlen(s), m = strlen(suffix);
	return n >= m && strcmp(s + n - m, suffix) == 0;
}

char *trim(char *s)
{
	while (isspace((unsigned char)*s))
		s++;
	size_t n = strlen(s);
	while (n && isspace((unsigned char)s[n - 1]))
		s[--n] = '\0';
	return s;
}

char *path_join(const char *a, const char *b)
{
	while (*b == '/')
		b++;
	size_t n = strlen(a);
	if (n && a[n - 1] == '/')
		return xasprintf("%s%s", a, b);
	return xasprintf("%s/%s", a, b);
}

int mkdirs(const char *path, mode_t mode)
{
	char *p = xstrdup(path);
	for (char *s = p + 1; *s; s++) {
		if (*s != '/')
			continue;
		*s = '\0';
		if (mkdir(p, mode) < 0 && errno != EEXIST) {
			free(p);
			return -1;
		}
		*s = '/';
	}
	int rc = mkdir(p, mode);
	free(p);
	return rc < 0 && errno != EEXIST ? -1 : 0;
}

ssize_t read_full(int fd, void *buf, size_t len)
{
	size_t done = 0;
	while (done < len) {
		ssize_t n = read(fd, (char *)buf + done, len - done);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0)
			break;
		done += (size_t)n;
	}
	return (ssize_t)done;
}

int write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	while (len) {
		ssize_t n = write(fd, p, len);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

char *read_file(const char *path, size_t *len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	struct strbuf sb = {0};
	char buf[65536];
	ssize_t n;
	while ((n = read(fd, buf, sizeof(buf))) != 0) {
		if (n < 0) {
			if (errno == EINTR)
				continue;
			int e = errno;
			close(fd);
			sb_free(&sb);
			errno = e;
			return NULL;
		}
		sb_add(&sb, buf, (size_t)n);
	}
	close(fd);
	if (len)
		*len = sb.len;
	return sb_steal(&sb);
}

int write_file(const char *path, const void *data, size_t len, mode_t mode)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
	if (fd < 0)
		return -1;
	if (write_all(fd, data, len) < 0) {
		int e = errno;
		close(fd);
		errno = e;
		return -1;
	}
	return close(fd);
}

static int rm_one(const char *path, const struct stat *st, int type, struct FTW *ftw)
{
	(void)st;
	(void)ftw;
	int rc = type == FTW_DP ? rmdir(path) : unlink(path);
	return rc < 0 && errno != ENOENT ? -1 : 0;
}

/* rm -rf, without following symlinks or leaving the file system */
int rmtree(const char *path)
{
	struct stat st;
	if (lstat(path, &st) < 0)
		return errno == ENOENT ? 0 : -1;
	return nftw(path, rm_one, 32, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
}
