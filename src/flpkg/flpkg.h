/* flpkg -- the FreeLFS package manager. Shared declarations.
 * SPDX-License-Identifier: MIT */
#ifndef FLPKG_H
#define FLPKG_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#define FLPKG_VERSION "0.1"

#define ATTR_PRINTF(f, a) __attribute__((format(printf, f, a)))

/* ---- util.c ------------------------------------------------------------ */

extern const char *progname;
extern int verbose;
extern void (*die_hook)(void); /* run once by die() before exiting */

_Noreturn void die(const char *fmt, ...) ATTR_PRINTF(1, 2);
void warn(const char *fmt, ...) ATTR_PRINTF(1, 2);
void msg(const char *fmt, ...) ATTR_PRINTF(1, 2);

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xasprintf(const char *fmt, ...) ATTR_PRINTF(1, 2);

/* A growable byte buffer, always NUL-terminated. */
struct strbuf {
	char *buf;
	size_t len, cap;
};
void sb_add(struct strbuf *sb, const void *data, size_t len);
void sb_addstr(struct strbuf *sb, const char *s);
void sb_addch(struct strbuf *sb, char c);
void sb_addf(struct strbuf *sb, const char *fmt, ...) ATTR_PRINTF(2, 3);
char *sb_steal(struct strbuf *sb);
void sb_free(struct strbuf *sb);

/* A growable list of owned strings. */
struct strlist {
	char **v;
	size_t n, cap;
};
void sl_add(struct strlist *sl, const char *s);
void sl_add_owned(struct strlist *sl, char *s);
bool sl_has(const struct strlist *sl, const char *s);
void sl_sort(struct strlist *sl);
void sl_free(struct strlist *sl);

bool has_prefix(const char *s, const char *prefix);
bool has_suffix(const char *s, const char *suffix);
char *trim(char *s);
char *path_join(const char *a, const char *b);
int mkdirs(const char *path, mode_t mode);
char *read_file(const char *path, size_t *len);
int write_file(const char *path, const void *data, size_t len, mode_t mode);
int write_all(int fd, const void *buf, size_t len);
ssize_t read_full(int fd, void *buf, size_t len);

int rmtree(const char *path);

/* ---- spawn.c: running other programs ----------------------------------- */

/* Run ARGV (PATH lookup) with ENVP (NULL: inherit) in CWD (NULL: current).
 * Returns the exit status, 128+N for a signal, or -1 if it could not run. */
int run_argv(char *const argv[], char *const envp[], const char *cwd);
/* Same, capturing standard output into OUT. */
int run_capture(char *const argv[], char *const envp[], const char *cwd, struct strbuf *out);
bool have_program(const char *name);

/* ---- config.c: /etc/flpkg.conf ----------------------------------------- */

struct repo_cfg {
	char *name, *url;
};

struct config {
	char *root;       /* everything below is relative to it */
	char *cache;      /* package and source cache */
	char *builddir;   /* where flpkg build works */
	char *keydir;     /* trusted public keys */
	char *signkey;    /* secret key for signing, or NULL */
	char *libdir;     /* recipe.sh, triggers */
	struct repo_cfg *repos;
	size_t nrepos;
	int jobs;
	int zstd_level;
	bool backup_config;
	char *signatures; /* required, optional or never */
	int snapshots;    /* btrfs snapshots of / to keep; 0: none */
	char *cflags, *cxxflags, *ldflags;
	int64_t source_date_epoch;
};
extern struct config cfg;
void config_load(const char *path); /* a missing file means defaults */
char *machine_arch(void);

/* ---- hash.c ------------------------------------------------------------ */

#define SHA256_LEN 32
#define SHA256_HEX (2 * SHA256_LEN + 1)
#define BLAKE2B_LEN 64

struct hasher;
struct hasher *hasher_new(const char *algo); /* "sha256" or "blake2b512" */
void hasher_update(struct hasher *h, const void *data, size_t len);
size_t hasher_final(struct hasher *h, unsigned char *out); /* frees h */
void hex_encode(const unsigned char *in, size_t len, char *out);
int sha256_fd(int fd, char hex[SHA256_HEX]);
int sha256_file(const char *path, char hex[SHA256_HEX]);
void sha256_mem(const void *data, size_t len, char hex[SHA256_HEX]);

/* ---- version.c --------------------------------------------------------- */

int vercmp(const char *a, const char *b);

enum dep_op { DEP_ANY, DEP_EQ, DEP_GE, DEP_LE, DEP_GT, DEP_LT };

struct dep {
	char *name;
	enum dep_op op;
	char *version; /* NULL for DEP_ANY */
};
int dep_parse(const char *s, struct dep *d);
void dep_free(struct dep *d);
bool dep_version_ok(const struct dep *d, const char *version);
/* Does a package NAME/VERSION with PROVIDES ("n" or "n=v") satisfy D? */
bool dep_satisfied_by(const struct dep *d, const char *name, const char *version,
                      const struct strlist *provides);
char *dep_format(const struct dep *d);

/* ---- zst.c: zstd streams over file descriptors ------------------------- */

struct zwriter;
struct zwriter *zw_open(int fd, int level, int workers);
int zw_write(struct zwriter *zw, const void *buf, size_t len);
int zw_close(struct zwriter *zw); /* flushes and frees; does not close fd */

struct zreader;
struct zreader *zr_open(int fd);
ssize_t zr_read(struct zreader *zr, void *buf, size_t len);
void zr_close(struct zreader *zr); /* frees; does not close fd */

/* ---- tar.c: POSIX pax tar ---------------------------------------------- */

enum tar_type {
	TAR_FILE = '0',
	TAR_HARDLINK = '1',
	TAR_SYMLINK = '2',
	TAR_DIR = '5',
};

struct tar_entry {
	char *path;  /* relative, no leading "./", no trailing '/' */
	char *link;  /* symlink target or hard link path */
	char type;   /* enum tar_type */
	mode_t mode; /* permission bits incl. setuid/setgid/sticky */
	uid_t uid;
	gid_t gid;
	uint64_t size;
	int64_t mtime;
};
void tar_entry_clear(struct tar_entry *e);

typedef int (*tar_write_fn)(void *ctx, const void *buf, size_t len);
typedef ssize_t (*tar_read_fn)(void *ctx, void *buf, size_t len);

struct tar_writer {
	tar_write_fn write;
	void *ctx;
	uint64_t remaining; /* data bytes the current entry still expects */
	uint64_t padding;   /* zero bytes to write after them */
};
/* Write the header, then exactly e->size bytes with tar_write_data(). */
int tar_write_header(struct tar_writer *w, const struct tar_entry *e);
int tar_write_data(struct tar_writer *w, const void *buf, size_t len);
int tar_add_mem(struct tar_writer *w, const struct tar_entry *e, const void *buf);
int tar_add_fd(struct tar_writer *w, const struct tar_entry *e, int fd);
int tar_finish(struct tar_writer *w);

struct tar_reader {
	tar_read_fn read;
	void *ctx;
	uint64_t remaining; /* data bytes left in the current entry */
	uint64_t padding;   /* padding after it */
	char *error;
};
/* 1: entry read into E, 0: end of archive, -1: error (r->error) */
int tar_next(struct tar_reader *r, struct tar_entry *e);
ssize_t tar_read(struct tar_reader *r, void *buf, size_t len);
int tar_skip(struct tar_reader *r);
/* Read the whole current entry into a NUL-terminated buffer (max bytes). */
char *tar_read_all(struct tar_reader *r, size_t max, size_t *len);

/* ---- pkginfo.c: .PKGINFO and .FILES ------------------------------------ */

struct pkginfo {
	char *name, *base, *version, *summary, *url, *arch;
	int64_t builddate;
	uint64_t size;
	struct strlist license, depend, provide, conflict, replace, group;
	struct strlist sysuser, sysgroup;
	/* repo index only */
	char *filename, *sha256;
	uint64_t csize;
};
void pkginfo_free(struct pkginfo *p);
int pkginfo_parse(const char *text, struct pkginfo *p, char **error);
void pkginfo_write(const struct pkginfo *p, struct strbuf *out, bool repo_fields);

struct manifest_entry {
	char type; /* d f l h */
	mode_t mode;
	uid_t uid;
	gid_t gid;
	uint64_t size;
	char sha256[SHA256_HEX]; /* "" unless type f */
	char *target;            /* l and h */
	char *path;
};
struct manifest {
	struct manifest_entry *v;
	size_t n, cap;
};
void manifest_add(struct manifest *m, const struct manifest_entry *e);
void manifest_sort(struct manifest *m);
/* Binary search in a sorted manifest; NULL if PATH is not in it. */
const struct manifest_entry *manifest_find(const struct manifest *m, const char *path);
void manifest_free(struct manifest *m);
int manifest_parse(const char *text, struct manifest *m, char **error);
void manifest_write(const struct manifest *m, struct strbuf *out);
/* mtree-style escaping: backslash, space and newline become \\, \040, \012 */
void escape_path(struct strbuf *out, const char *s);
char *unescape_path(const char *s); /* NULL if malformed */

/* ---- pkgfile.c: reading .flpkg files ----------------------------------- */

struct pkgfile {
	int fd;
	struct zreader *zr;
	struct tar_reader tr; /* positioned at the payload after pkgfile_open */
	struct pkginfo info;
	struct manifest files;
};
int pkgfile_open(const char *path, struct pkgfile *pf, char **error);
void pkgfile_close(struct pkgfile *pf);

/* ---- pack.c: writing .flpkg files -------------------------------------- */

struct owner_override {
	char *path;
	uid_t uid;
	gid_t gid;
	mode_t mode;
};
/* Archive the tree at ROOT as a package with metadata P (size is filled in).
 * Files are owned by 0:0 unless OVERRIDES say otherwise. */
int pack_create(const char *root, struct pkginfo *p, const struct owner_override *overrides,
                size_t noverrides, const char *outpath, char **error);

/* ---- commands ----------------------------------------------------------- */

int cmd_build(int argc, char **argv);
int cmd_install(int argc, char **argv);
int cmd_remove(int argc, char **argv);
int cmd_list(int argc, char **argv);
int cmd_info_installed(const char *name);
int cmd_files_installed(const char *name);
int cmd_owns(int argc, char **argv);
int cmd_verify(int argc, char **argv);
int cmd_history(void);
int cmd_fix_owners(int argc, char **argv);
int cmd_upgrade(int argc, char **argv);
int cmd_sync(void);
int cmd_check_updates(int argc, char **argv);
int cmd_search(int argc, char **argv);
int cmd_info_repo(const char *name);
int cmd_repo(int argc, char **argv);
int cmd_key(int argc, char **argv);
int cmd_snapshots(void);
void print_pkginfo(const struct pkginfo *p);

#endif
