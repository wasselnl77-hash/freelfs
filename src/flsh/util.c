/* Small helpers: allocation, strings, files, UTF-8, error unwinding.
 * SPDX-License-Identifier: MIT */
#include "flsh.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>

const char *progname = "flsh";
struct jmp *jmp_top;

static void oom(void)
{
	static const char m[] = "flsh: out of memory\n";
	write_all(2, m, sizeof(m) - 1);
	_exit(2);
}

void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p)
		oom();
	return p;
}

void *xcalloc(size_t n, size_t size)
{
	void *p = calloc(n ? n : 1, size ? size : 1);
	if (!p)
		oom();
	return p;
}

void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n ? n : 1);
	if (!p)
		oom();
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
	va_list ap;
	va_start(ap, fmt);
	char *s;
	if (vasprintf(&s, fmt, ap) < 0)
		oom();
	va_end(ap);
	return s;
}

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

void sb_add(struct strbuf *sb, const char *s, size_t n)
{
	sb_grow(sb, n);
	memcpy(sb->buf + sb->len, s, n);
	sb->len += n;
	sb->buf[sb->len] = '\0';
}

void sb_addc(struct strbuf *sb, char c)
{
	sb_grow(sb, 1);
	sb->buf[sb->len++] = c;
	sb->buf[sb->len] = '\0';
}

void sb_addstr(struct strbuf *sb, const char *s)
{
	sb_add(sb, s, strlen(s));
}

void sb_addf(struct strbuf *sb, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	char *s;
	int n = vasprintf(&s, fmt, ap);
	va_end(ap);
	if (n < 0)
		oom();
	sb_add(sb, s, (size_t)n);
	free(s);
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

void sl_push(struct strlist *sl, char *s)
{
	if (sl->n + 2 > sl->cap) {
		sl->cap = sl->cap ? sl->cap * 2 : 8;
		sl->v = xrealloc(sl->v, sl->cap * sizeof(char *));
	}
	sl->v[sl->n++] = s;
	sl->v[sl->n] = NULL;
}

void sl_add(struct strlist *sl, const char *s)
{
	sl_push(sl, xstrdup(s));
}

void sl_free(struct strlist *sl)
{
	for (size_t i = 0; i < sl->n; i++)
		free(sl->v[i]);
	free(sl->v);
	sl->v = NULL;
	sl->n = sl->cap = 0;
}

char **sl_nullterm(struct strlist *sl)
{
	if (!sl->v) {
		sl->cap = 8;
		sl->v = xcalloc(sl->cap, sizeof(char *));
	}
	return sl->v;
}

bool has_prefix(const char *s, const char *p)
{
	return strncmp(s, p, strlen(p)) == 0;
}

char *read_fd(int fd, size_t *len)
{
	struct strbuf sb = {0};
	char buf[8192];
	for (;;) {
		ssize_t r = read(fd, buf, sizeof(buf));
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		sb_add(&sb, buf, (size_t)r);
	}
	if (len)
		*len = sb.len;
	return sb_steal(&sb);
}

char *read_file(const char *path, size_t *len)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	char *s = read_fd(fd, len);
	close(fd);
	return s;
}

int write_all(int fd, const void *buf, size_t n)
{
	const char *p = buf;
	while (n) {
		ssize_t w = write(fd, p, n);
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return -1;
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

int move_fd_high(int fd)
{
	int n = fcntl(fd, F_DUPFD_CLOEXEC, 10);
	if (n >= 0)
		close(fd);
	return n;
}

int str_to_int(const char *s, long *out)
{
	if (!*s)
		return -1;
	char *end;
	errno = 0;
	long v = strtol(s, &end, 10);
	if (errno || *end)
		return -1;
	*out = v;
	return 0;
}

int utf8_len(unsigned char c)
{
	if (c < 0x80)
		return 1;
	if ((c & 0xe0) == 0xc0)
		return 2;
	if ((c & 0xf0) == 0xe0)
		return 3;
	if ((c & 0xf8) == 0xf0)
		return 4;
	return 1;
}

/* Display width of the character at s (n bytes left); *bytes gets its size. */
int utf8_width(const char *s, size_t n, size_t *bytes)
{
	unsigned char c = (unsigned char)s[0];
	if (c < 0x80) {
		*bytes = 1;
		return c < 0x20 || c == 0x7f ? 2 : 1; /* control chars show as ^X */
	}
	size_t len = (size_t)utf8_len(c);
	if (len > n)
		len = 1;
	for (size_t i = 1; i < len; i++)
		if (((unsigned char)s[i] & 0xc0) != 0x80) {
			len = 1;
			break;
		}
	*bytes = len;
	if (len == 1)
		return 1;
	wchar_t wc;
	mbstate_t st = {0};
	if (mbrtowc(&wc, s, len, &st) != len)
		return 1;
	int w = wcwidth(wc);
	return w < 0 ? 1 : w;
}

static void vreport(const char *fmt, va_list ap)
{
	fflush(stdout);
	struct strbuf sb = {0};
	sb_addstr(&sb, progname);
	if (sh.script && !sh.interactive)
		sb_addf(&sb, ": %s: line %d", sh.script, sh.lineno);
	sb_addstr(&sb, ": ");
	char *m;
	if (vasprintf(&m, fmt, ap) >= 0) {
		sb_addstr(&sb, m);
		free(m);
	}
	sb_addc(&sb, '\n');
	write_all(2, sb.buf, sb.len);
	sb_free(&sb);
}

void sh_warn(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vreport(fmt, ap);
	va_end(ap);
}

void sh_raise(int status)
{
	sh.status = status;
	if (!jmp_top)
		sh_exit(status);
	longjmp(jmp_top->buf, 1);
}

void sh_error(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vreport(fmt, ap);
	va_end(ap);
	/* a non-interactive shell exits on errors like these */
	if (!sh.interactive && !jmp_top)
		sh_exit(2);
	sh_raise(2);
}
