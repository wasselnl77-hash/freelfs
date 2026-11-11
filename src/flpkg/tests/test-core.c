/* Unit tests for the core library: versions, deps, hashes, tar, zstd,
 * .PKGINFO and .FILES.
 * SPDX-License-Identifier: MIT */
#include "../flpkg.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures, checks;

#define CHECK(cond)                                                                \
	do {                                                                       \
		checks++;                                                          \
		if (!(cond)) {                                                     \
			failures++;                                                \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
		}                                                                  \
	} while (0)

static void test_vercmp(void)
{
	static const struct {
		const char *a, *b;
		int want;
	} t[] = {
		{"1.0", "1.0", 0},          {"1.0", "1.1", -1},
		{"1.10", "1.9", 1},         {"1.0", "1.0.1", -1},
		{"1.0a", "1.0b", -1},       {"1.0rc1", "1.0", -1},
		{"1.001", "1.1", 0},        {"1:1.0", "2.0", 1},
		{"0:1.0", "1.0", 0},        {"1.0-1", "1.0-2", -1},
		{"1.0-2", "1.0", 0},        {"6.18.59", "6.18.9", 1},
		{"2.47.20260726", "2.47", 1}, {"3530400", "3520000", 1},
		{"1.0.0", "1.0", 1},        {"a", "b", -1},
		{"1.0", "1.a", 1},          {"2026c", "2026b", 1},
		{"5.45.4", "5.45.4", 0},    {"1.2.3-10", "1.2.3-9", 1},
	};
	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		int got = vercmp(t[i].a, t[i].b), back = vercmp(t[i].b, t[i].a);
		if (got != t[i].want || back != -t[i].want)
			fprintf(stderr, "vercmp(%s, %s) = %d/%d, want %d\n", t[i].a, t[i].b, got,
			        back, t[i].want);
		CHECK(got == t[i].want && back == -t[i].want);
	}
}

static bool sat(const char *dep, const char *name, const char *ver, const char *provide)
{
	struct dep d;
	struct strlist prov = {0};
	if (provide)
		sl_add(&prov, provide);
	CHECK(dep_parse(dep, &d) == 0);
	bool ok = dep_satisfied_by(&d, name, ver, &prov);
	dep_free(&d);
	sl_free(&prov);
	return ok;
}

static void test_deps(void)
{
	struct dep d;
	CHECK(dep_parse("glibc>=2.40", &d) == 0 && d.op == DEP_GE && !strcmp(d.version, "2.40"));
	char *s = dep_format(&d);
	CHECK(!strcmp(s, "glibc>=2.40"));
	free(s);
	dep_free(&d);
	CHECK(dep_parse(">=1", &d) < 0);
	CHECK(dep_parse("foo>=", &d) < 0);

	CHECK(sat("glibc>=2.40", "glibc", "2.44-1", NULL));
	CHECK(!sat("glibc>=2.45", "glibc", "2.44-1", NULL));
	CHECK(sat("foo=1.2", "foo", "1.2-3", NULL));
	CHECK(!sat("foo=1.2-2", "foo", "1.2-3", NULL));
	CHECK(sat("foo<2", "foo", "1.9-1", NULL));
	CHECK(sat("libz.so", "zlib", "1.3.2-1", "libz.so=1-64"));
	CHECK(sat("libz.so=1", "zlib", "1.3.2-1", "libz.so=1-64"));
	CHECK(sat("sh", "bash", "5.3-1", "sh"));
	CHECK(!sat("sh>=1", "bash", "5.3-1", "sh")); /* unversioned provide */
	CHECK(!sat("zsh", "bash", "5.3-1", "sh"));
}

static void test_hash(void)
{
	char hex[SHA256_HEX];
	sha256_mem("abc", 3, hex);
	CHECK(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

/* ---- tar round trip through memory ---------------------------------------- */

static int mem_write(void *ctx, const void *buf, size_t len)
{
	sb_add(ctx, buf, len);
	return 0;
}

struct memin {
	const char *buf;
	size_t len, off;
};

static ssize_t mem_read(void *ctx, void *buf, size_t len)
{
	struct memin *m = ctx;
	size_t n = m->len - m->off < len ? m->len - m->off : len;
	/* hand out odd-sized pieces to exercise partial reads */
	if (n > 300)
		n = 300;
	memcpy(buf, m->buf + m->off, n);
	m->off += n;
	return (ssize_t)n;
}

static char longpath[300], longlink[200];

static void build_tar(struct strbuf *out)
{
	struct tar_writer w = {mem_write, out, 0, 0};
	struct tar_entry e = {0};

	e = (struct tar_entry){.path = "usr", .type = TAR_DIR, .mode = 0755, .mtime = 1767225600};
	CHECK(tar_add_mem(&w, &e, NULL) == 0);
	e = (struct tar_entry){.path = "usr/bin/wall", .type = TAR_FILE, .mode = 02755,
	                       .uid = 0, .gid = 5, .size = 5, .mtime = 1767225600};
	CHECK(tar_add_mem(&w, &e, "hello") == 0);
	e = (struct tar_entry){.path = "usr/lib/libz.so", .link = "libz.so.1.3.2",
	                       .type = TAR_SYMLINK, .mode = 0777, .mtime = 1767225600};
	CHECK(tar_add_mem(&w, &e, NULL) == 0);
	e = (struct tar_entry){.path = "usr/bin/perl5.44.0", .link = "usr/bin/wall",
	                       .type = TAR_HARDLINK, .mode = 0755, .mtime = 1767225600};
	CHECK(tar_add_mem(&w, &e, NULL) == 0);
	e = (struct tar_entry){.path = longpath, .link = longlink, .type = TAR_SYMLINK,
	                       .mode = 0777, .uid = 3000000, .mtime = 1767225600};
	CHECK(tar_add_mem(&w, &e, NULL) == 0);
	static char big[1000];
	memset(big, 'x', sizeof(big));
	e = (struct tar_entry){.path = "a file with spaces", .type = TAR_FILE, .mode = 0644,
	                       .size = sizeof(big), .mtime = 1767225600};
	CHECK(tar_add_mem(&w, &e, big) == 0);
	CHECK(tar_finish(&w) == 0);
}

static void test_tar(void)
{
	memset(longpath, 'p', sizeof(longpath) - 1);
	memcpy(longpath, "usr/share/", 10);
	memset(longlink, 'l', sizeof(longlink) - 1);

	struct strbuf a = {0}, b = {0};
	build_tar(&a);
	build_tar(&b);
	CHECK(a.len % 512 == 0);
	CHECK(a.len == b.len && !memcmp(a.buf, b.buf, a.len)); /* deterministic */

	struct memin in = {a.buf, a.len, 0};
	struct tar_reader r = {.read = mem_read, .ctx = &in};
	struct tar_entry e = {0};

	CHECK(tar_next(&r, &e) == 1 && !strcmp(e.path, "usr") && e.type == TAR_DIR && e.mode == 0755);
	CHECK(tar_next(&r, &e) == 1 && !strcmp(e.path, "usr/bin/wall") && e.mode == 02755 &&
	      e.gid == 5 && e.size == 5 && e.mtime == 1767225600);
	char buf[16];
	CHECK(tar_read(&r, buf, sizeof(buf)) == 5 && !memcmp(buf, "hello", 5));
	CHECK(tar_next(&r, &e) == 1 && e.type == TAR_SYMLINK && !strcmp(e.link, "libz.so.1.3.2"));
	CHECK(tar_next(&r, &e) == 1 && e.type == TAR_HARDLINK && !strcmp(e.link, "usr/bin/wall"));
	CHECK(tar_next(&r, &e) == 1 && !strcmp(e.path, longpath) && !strcmp(e.link, longlink) &&
	      e.uid == 3000000);
	/* skip the data of the last file without reading it */
	CHECK(tar_next(&r, &e) == 1 && !strcmp(e.path, "a file with spaces") && e.size == 1000);
	CHECK(tar_next(&r, &e) == 0);
	tar_entry_clear(&e);
	free(r.error);
	sb_free(&a);
	sb_free(&b);
}

/* ---- zstd round trip through a temporary file ------------------------------ */

static void zstd_file(const char *path, const char *data, size_t len, int workers)
{
	FILE *f = fopen(path, "w");
	struct zwriter *zw = zw_open(fileno(f), 19, workers);
	for (size_t off = 0; off < len; off += 7777)
		CHECK(zw_write(zw, data + off, len - off < 7777 ? len - off : 7777) == 0);
	CHECK(zw_close(zw) == 0);
	fclose(f);
}

static void test_zstd(const char *tmpdir)
{
	size_t len = 3 << 20;
	char *data = xmalloc(len);
	uint32_t x = 12345;
	for (size_t i = 0; i < len; i++) {
		x = x * 1103515245 + 12345;
		data[i] = (char)("abcdefgh"[(x >> 16) & 7]);
	}
	char *p1 = path_join(tmpdir, "one.zst"), *p4 = path_join(tmpdir, "four.zst");
	zstd_file(p1, data, len, 1);
	zstd_file(p4, data, len, 4);
	size_t n1, n4;
	char *c1 = read_file(p1, &n1), *c4 = read_file(p4, &n4);
	CHECK(c1 && c4 && n1 == n4 && !memcmp(c1, c4, n1)); /* worker count does not matter */
	CHECK(n1 < len / 2);

	FILE *f = fopen(p1, "r");
	struct zreader *zr = zr_open(fileno(f));
	struct strbuf out = {0};
	char buf[5000];
	ssize_t n;
	while ((n = zr_read(zr, buf, sizeof(buf))) > 0)
		sb_add(&out, buf, (size_t)n);
	CHECK(n == 0);
	CHECK(out.len == len && !memcmp(out.buf, data, len));
	zr_close(zr);
	fclose(f);
	unlink(p1);
	unlink(p4);
	free(p1);
	free(p4);
	free(c1);
	free(c4);
	free(data);
	sb_free(&out);
}

static void test_pkginfo(void)
{
	const char *text = "name = zlib\nversion = 1.3.2-1\narch = x86_64\n"
	                   "summary = Compression library\nlicense = Zlib\n"
	                   "builddate = 1767225600\nsize = 386264\n"
	                   "depend = glibc>=2.44\nprovide = libz.so=1-64\ngroup = base\n"
	                   "future-key = ignored\n";
	struct pkginfo p;
	char *err = NULL;
	CHECK(pkginfo_parse(text, &p, &err) == 0);
	CHECK(!strcmp(p.base, "zlib") && p.size == 386264 && p.builddate == 1767225600);
	CHECK(p.depend.n == 1 && !strcmp(p.depend.v[0], "glibc>=2.44"));
	struct strbuf out = {0};
	pkginfo_write(&p, &out, false);
	struct pkginfo q;
	CHECK(pkginfo_parse(out.buf, &q, &err) == 0);
	struct strbuf out2 = {0};
	pkginfo_write(&q, &out2, false);
	CHECK(!strcmp(out.buf, out2.buf));
	pkginfo_free(&p);
	pkginfo_free(&q);
	sb_free(&out);
	sb_free(&out2);
	CHECK(pkginfo_parse("name = x\n", &p, &err) < 0);
	free(err);
}

static void test_manifest(void)
{
	struct manifest m = {0};
	struct manifest_entry e = {.type = 'd', .mode = 0755, .path = xstrdup("usr")};
	manifest_add(&m, &e);
	e = (struct manifest_entry){.type = 'f', .mode = 02755, .gid = 5, .size = 5,
	                            .path = xstrdup("usr/share/a b\\c")};
	strcpy(e.sha256, "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
	manifest_add(&m, &e);
	e = (struct manifest_entry){.type = 'l', .mode = 0777, .target = xstrdup("x y"),
	                            .path = xstrdup("usr/lib/link")};
	manifest_add(&m, &e);

	struct strbuf out = {0};
	manifest_write(&m, &out);
	struct manifest n;
	char *err = NULL;
	CHECK(manifest_parse(out.buf, &n, &err) == 0);
	CHECK(n.n == 3);
	CHECK(n.n == 3 && !strcmp(n.v[1].path, "usr/share/a b\\c") && n.v[1].mode == 02755 &&
	      n.v[1].gid == 5 && !strcmp(n.v[1].sha256, m.v[1].sha256));
	CHECK(n.n == 3 && !strcmp(n.v[2].target, "x y"));
	CHECK(strstr(out.buf, "a\\040b\\\\c") != NULL);
	manifest_free(&m);
	manifest_free(&n);
	sb_free(&out);
	CHECK(manifest_parse("f 0644 0 0 5 short x\n", &n, &err) < 0);
	free(err);
}

int main(void)
{
	char tmpl[] = "/tmp/flpkg-test-XXXXXX";
	char *tmpdir = mkdtemp(tmpl);
	if (!tmpdir)
		die("mkdtemp failed");

	test_vercmp();
	test_deps();
	test_hash();
	test_tar();
	test_zstd(tmpdir);
	test_pkginfo();
	test_manifest();

	rmdir(tmpdir);
	printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
