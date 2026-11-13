/* POSIX pax tar, reading and writing.
 *
 * The writer produces deterministic archives: ustar headers with no user or
 * group names, plus a pax 'x' header only when a path, link target, size or
 * id does not fit the ustar fields. The reader accepts ustar, pax and GNU
 * long-name archives.
 * SPDX-License-Identifier: MIT */
#include "flpkg.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BLOCK 512

/* ustar header field offsets and sizes */
#define H_NAME 0
#define H_MODE 100
#define H_UID 108
#define H_GID 116
#define H_SIZE 124
#define H_MTIME 136
#define H_CHKSUM 148
#define H_TYPE 156
#define H_LINK 157
#define H_MAGIC 257
#define H_VERSION 263
#define H_PREFIX 345

#define MAX_OCTAL_7 07777777ULL
#define MAX_OCTAL_11 077777777777ULL

static const char zeros[BLOCK];

void tar_entry_clear(struct tar_entry *e)
{
	free(e->path);
	free(e->link);
	memset(e, 0, sizeof(*e));
}

/* ---- writing ------------------------------------------------------------- */

static void put_octal(char *field, size_t width, uint64_t v)
{
	/* width-1 zero-padded digits and a NUL */
	field[width - 1] = '\0';
	for (size_t i = width - 1; i-- > 0;) {
		field[i] = (char)('0' + (v & 7));
		v >>= 3;
	}
}

static void put_str(char *field, size_t width, const char *s)
{
	size_t n = strlen(s);
	memcpy(field, s, n < width ? n : width);
}

static void set_checksum(char *h)
{
	unsigned sum = 0;
	memset(h + H_CHKSUM, ' ', 8);
	for (size_t i = 0; i < BLOCK; i++)
		sum += (unsigned char)h[i];
	put_octal(h + H_CHKSUM, 7, sum); /* six digits, NUL, then the space */
	h[H_CHKSUM + 7] = ' ';
}

static void fill_header(char *h, const char *name, const char *link, char type,
                        mode_t mode, uid_t uid, gid_t gid, uint64_t size, int64_t mtime)
{
	memset(h, 0, BLOCK);
	put_str(h + H_NAME, 100, name);
	put_octal(h + H_MODE, 8, mode & 07777);
	put_octal(h + H_UID, 8, uid > MAX_OCTAL_7 ? 0 : uid);
	put_octal(h + H_GID, 8, gid > MAX_OCTAL_7 ? 0 : gid);
	put_octal(h + H_SIZE, 12, size > MAX_OCTAL_11 ? 0 : size);
	put_octal(h + H_MTIME, 12, mtime < 0 ? 0 : (uint64_t)mtime);
	h[H_TYPE] = type;
	if (link)
		put_str(h + H_LINK, 100, link);
	memcpy(h + H_MAGIC, "ustar", 6);
	memcpy(h + H_VERSION, "00", 2);
	set_checksum(h);
}

static size_t ndigits(size_t n)
{
	size_t d = 1;
	while (n >= 10)
		n /= 10, d++;
	return d;
}

/* "<len> key=value\n", where len counts the whole record. */
static void pax_record(struct strbuf *sb, const char *key, const char *value)
{
	size_t base = strlen(key) + strlen(value) + 3;
	size_t len = base + ndigits(base);
	while (base + ndigits(len) != len)
		len = base + ndigits(len);
	sb_addf(sb, "%zu %s=%s\n", len, key, value);
}

static int put(struct tar_writer *w, const void *buf, size_t len)
{
	return w->write(w->ctx, buf, len);
}

int tar_write_header(struct tar_writer *w, const struct tar_entry *e)
{
	char h[BLOCK];
	char *name = e->type == TAR_DIR ? xasprintf("%s/", e->path) : xstrdup(e->path);
	uint64_t size = e->type == TAR_FILE ? e->size : 0;
	struct strbuf pax = {0};

	if (strlen(name) > 100)
		pax_record(&pax, "path", name);
	if (e->link && strlen(e->link) > 100)
		pax_record(&pax, "linkpath", e->link);
	if (size > MAX_OCTAL_11) {
		char *s = xasprintf("%llu", (unsigned long long)size);
		pax_record(&pax, "size", s);
		free(s);
	}
	if (e->uid > MAX_OCTAL_7) {
		char *s = xasprintf("%u", (unsigned)e->uid);
		pax_record(&pax, "uid", s);
		free(s);
	}
	if (e->gid > MAX_OCTAL_7) {
		char *s = xasprintf("%u", (unsigned)e->gid);
		pax_record(&pax, "gid", s);
		free(s);
	}

	int rc = 0;
	if (pax.len) {
		fill_header(h, "PaxHeader", NULL, 'x', 0644, 0, 0, pax.len, e->mtime);
		size_t pad = (BLOCK - pax.len % BLOCK) % BLOCK;
		rc = put(w, h, BLOCK);
		if (!rc)
			rc = put(w, pax.buf, pax.len);
		if (!rc && pad)
			rc = put(w, zeros, pad);
	}
	if (!rc) {
		fill_header(h, name, e->link, e->type, e->mode, e->uid, e->gid, size, e->mtime);
		rc = put(w, h, BLOCK);
	}
	sb_free(&pax);
	free(name);
	w->remaining = size;
	w->padding = (BLOCK - size % BLOCK) % BLOCK;
	return rc;
}

int tar_write_data(struct tar_writer *w, const void *buf, size_t len)
{
	if (len > w->remaining)
		return -1;
	if (len && put(w, buf, len))
		return -1;
	w->remaining -= len;
	if (!w->remaining && w->padding) {
		if (put(w, zeros, w->padding))
			return -1;
		w->padding = 0;
	}
	return 0;
}

int tar_add_mem(struct tar_writer *w, const struct tar_entry *e, const void *buf)
{
	if (tar_write_header(w, e))
		return -1;
	return e->type == TAR_FILE ? tar_write_data(w, buf, e->size) : 0;
}

int tar_add_fd(struct tar_writer *w, const struct tar_entry *e, int fd)
{
	if (tar_write_header(w, e))
		return -1;
	char buf[65536];
	while (w->remaining) {
		size_t want = w->remaining < sizeof(buf) ? w->remaining : sizeof(buf);
		ssize_t n = read_full(fd, buf, want);
		if (n <= 0)
			return -1; /* the file shrank while being archived */
		if (tar_write_data(w, buf, (size_t)n))
			return -1;
	}
	return 0;
}

int tar_finish(struct tar_writer *w)
{
	if (w->remaining)
		return -1;
	if (put(w, zeros, BLOCK) || put(w, zeros, BLOCK))
		return -1;
	return 0;
}

/* ---- reading ------------------------------------------------------------- */

static int fail(struct tar_reader *r, const char *what)
{
	free(r->error);
	r->error = xstrdup(what);
	return -1;
}

static int read_exact(struct tar_reader *r, void *buf, size_t len)
{
	size_t done = 0;
	while (done < len) {
		ssize_t n = r->read(r->ctx, (char *)buf + done, len - done);
		if (n < 0)
			return fail(r, "read error");
		if (n == 0)
			return fail(r, "unexpected end of archive");
		done += (size_t)n;
	}
	return 0;
}

static int discard(struct tar_reader *r, uint64_t n)
{
	char buf[BLOCK * 16];
	while (n) {
		size_t k = n < sizeof(buf) ? n : sizeof(buf);
		if (read_exact(r, buf, k))
			return -1;
		n -= k;
	}
	return 0;
}

static uint64_t get_number(const char *f, size_t width)
{
	uint64_t v = 0;
	if ((unsigned char)f[0] & 0x80) { /* GNU base-256 */
		v = (unsigned char)f[0] & 0x7f;
		for (size_t i = 1; i < width; i++)
			v = (v << 8) | (unsigned char)f[i];
		return v;
	}
	size_t i = 0;
	while (i < width && f[i] == ' ')
		i++;
	for (; i < width && f[i] >= '0' && f[i] <= '7'; i++)
		v = (v << 3) | (uint64_t)(f[i] - '0');
	return v;
}

static bool checksum_ok(const char *h)
{
	unsigned u = 0;
	int s = 0;
	for (size_t i = 0; i < BLOCK; i++) {
		char c = (i >= H_CHKSUM && i < H_CHKSUM + 8) ? ' ' : h[i];
		u += (unsigned char)c;
		s += (signed char)c;
	}
	uint64_t want = get_number(h + H_CHKSUM, 8);
	return want == u || want == (uint64_t)(unsigned)s;
}

static char *field_str(const char *f, size_t width)
{
	return xstrndup(f, strnlen(f, width));
}

/* Normalise to a relative path without "./" or a trailing slash. */
static char *clean_path(char *p)
{
	char *s = p;
	while (s[0] == '.' && s[1] == '/')
		s += 2;
	while (*s == '/')
		s++;
	size_t n = strlen(s);
	while (n > 1 && s[n - 1] == '/')
		s[--n] = '\0';
	char *out = xstrdup(s);
	free(p);
	return out;
}

struct pax_state {
	char *path, *link;
	bool has_size, has_uid, has_gid;
	uint64_t size;
	unsigned long uid, gid;
};

static int parse_pax(struct tar_reader *r, const char *buf, size_t len, struct pax_state *ps)
{
	size_t off = 0;
	while (off < len) {
		char *end;
		unsigned long n = strtoul(buf + off, &end, 10);
		if (end == buf + off || *end != ' ' || n == 0 || n > len - off)
			return fail(r, "bad pax header");
		const char *kv = end + 1, *stop = buf + off + n - 1; /* at '\n' */
		const char *eq = memchr(kv, '=', (size_t)(stop - kv));
		if (!eq || *stop != '\n')
			return fail(r, "bad pax record");
		size_t klen = (size_t)(eq - kv);
		char *val = xstrndup(eq + 1, (size_t)(stop - eq - 1));
		if (klen == 4 && !memcmp(kv, "path", 4)) {
			free(ps->path);
			ps->path = val;
		} else if (klen == 8 && !memcmp(kv, "linkpath", 8)) {
			free(ps->link);
			ps->link = val;
		} else {
			if (klen == 4 && !memcmp(kv, "size", 4))
				ps->has_size = true, ps->size = strtoull(val, NULL, 10);
			else if (klen == 3 && !memcmp(kv, "uid", 3))
				ps->has_uid = true, ps->uid = strtoul(val, NULL, 10);
			else if (klen == 3 && !memcmp(kv, "gid", 3))
				ps->has_gid = true, ps->gid = strtoul(val, NULL, 10);
			free(val);
		}
		off += n;
	}
	return 0;
}

int tar_next(struct tar_reader *r, struct tar_entry *e)
{
	struct pax_state ps = {0};
	char *gnu_name = NULL, *gnu_link = NULL;
	char h[BLOCK];
	int rc = -1;

	tar_entry_clear(e);
	if (tar_skip(r))
		return -1;

	for (;;) {
		ssize_t n = 0;
		size_t got = 0;
		while (got < BLOCK) {
			n = r->read(r->ctx, h + got, BLOCK - got);
			if (n <= 0)
				break;
			got += (size_t)n;
		}
		if (n < 0) {
			fail(r, "read error");
			goto out;
		}
		if (got == 0 || (got == BLOCK && !memcmp(h, zeros, BLOCK))) {
			rc = 0; /* end of archive */
			goto out;
		}
		if (got < BLOCK) {
			fail(r, "truncated header");
			goto out;
		}
		if (!checksum_ok(h)) {
			fail(r, "header checksum mismatch");
			goto out;
		}

		char type = h[H_TYPE] ? h[H_TYPE] : TAR_FILE;
		uint64_t size = ps.has_size ? ps.size : get_number(h + H_SIZE, 12);
		uint64_t pad = (BLOCK - size % BLOCK) % BLOCK;

		if (type == 'x' || type == 'g' || type == 'L' || type == 'K') {
			if (size > (16u << 20)) {
				fail(r, "oversized extended header");
				goto out;
			}
			char *buf = xmalloc(size + 1);
			if (read_exact(r, buf, size) || discard(r, pad)) {
				free(buf);
				goto out;
			}
			buf[size] = '\0';
			int prc = 0;
			if (type == 'x')
				prc = parse_pax(r, buf, size, &ps);
			else if (type == 'L')
				free(gnu_name), gnu_name = xstrdup(buf);
			else if (type == 'K')
				free(gnu_link), gnu_link = xstrdup(buf);
			free(buf);
			if (prc)
				goto out;
			continue;
		}

		if (ps.path) {
			e->path = ps.path, ps.path = NULL;
		} else if (gnu_name) {
			e->path = gnu_name, gnu_name = NULL;
		} else {
			char *name = field_str(h + H_NAME, 100);
			char *prefix = field_str(h + H_PREFIX, 155);
			e->path = *prefix && !memcmp(h + H_MAGIC, "ustar", 5)
			              ? xasprintf("%s/%s", prefix, name)
			              : xstrdup(name);
			free(name);
			free(prefix);
		}
		e->path = clean_path(e->path);
		if (ps.link)
			e->link = ps.link, ps.link = NULL;
		else if (gnu_link)
			e->link = gnu_link, gnu_link = NULL;
		else if (h[H_LINK])
			e->link = field_str(h + H_LINK, 100);
		if (type == TAR_HARDLINK && e->link)
			e->link = clean_path(e->link);

		e->type = type == '7' ? TAR_FILE : type;
		e->mode = (mode_t)(get_number(h + H_MODE, 8) & 07777);
		e->uid = (uid_t)(ps.has_uid ? ps.uid : get_number(h + H_UID, 8));
		e->gid = (gid_t)(ps.has_gid ? ps.gid : get_number(h + H_GID, 8));
		e->mtime = (int64_t)get_number(h + H_MTIME, 12);
		bool data = e->type != TAR_DIR && e->type != TAR_SYMLINK && e->type != TAR_HARDLINK;
		e->size = data ? size : 0;
		r->remaining = data ? size : 0;
		r->padding = data ? pad : 0;
		rc = 1;
		goto out;
	}
out:
	free(ps.path);
	free(ps.link);
	free(gnu_name);
	free(gnu_link);
	return rc;
}

ssize_t tar_read(struct tar_reader *r, void *buf, size_t len)
{
	if (len > r->remaining)
		len = (size_t)r->remaining;
	if (!len)
		return 0;
	ssize_t n = r->read(r->ctx, buf, len);
	if (n < 0)
		return fail(r, "read error");
	if (n == 0)
		return fail(r, "unexpected end of archive");
	r->remaining -= (uint64_t)n;
	return n;
}

int tar_skip(struct tar_reader *r)
{
	if (discard(r, r->remaining + r->padding))
		return -1;
	r->remaining = r->padding = 0;
	return 0;
}

char *tar_read_all(struct tar_reader *r, size_t max, size_t *len)
{
	if (r->remaining > max) {
		fail(r, "entry too large");
		return NULL;
	}
	size_t n = (size_t)r->remaining;
	char *buf = xmalloc(n + 1);
	if (read_exact(r, buf, n)) {
		free(buf);
		return NULL;
	}
	r->remaining = 0;
	buf[n] = '\0';
	if (len)
		*len = n;
	return buf;
}
