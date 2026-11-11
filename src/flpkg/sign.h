/* Ed25519 signatures in minisign's file formats.
 * SPDX-License-Identifier: MIT */
#ifndef FLPKG_SIGN_H
#define FLPKG_SIGN_H

#include "flpkg.h"

struct pubkey {
	unsigned char keynum[8];
	unsigned char pk[32];
	char *path;
};

struct seckey {
	unsigned char keynum[8];
	unsigned char seed[32];
	unsigned char pk[32];
};

/* Write BASE.pub and BASE.key (unencrypted, mode 0600). */
void key_generate(const char *base);
int seckey_load(const char *path, struct seckey *k, char **error);
/* Load one public key file. */
int pubkey_load_file(const char *path, struct pubkey *k, char **error);
/* The key number as minisign shows it: 16 hex digits (OUT has room for 17). */
void keyid_hex(const unsigned char keynum[8], char out[17]);
/* Load every *.pub file in DIR. */
void pubkeys_load(const char *dir, struct pubkey **keys, size_t *n);
void pubkeys_free(struct pubkey *keys, size_t n);

/* Write FILE.sig: a prehashed ("ED") signature whose trusted comment
 * names the file, so a signature cannot be moved to another file. */
int sign_file(const struct seckey *k, const char *file, char **error);
/* Same, with extra tab-separated "name:value" fields in the (signed) trusted
 * comment, e.g. "serial:7\tvalid-until:2026-12-01T00:00:00Z". */
int sign_file_ex(const struct seckey *k, const char *file, const char *extra, char **error);
/* Check FILE against SIGPATH with one of KEYS; the trusted comment must
 * name EXPECT_NAME. */
int verify_file(const struct pubkey *keys, size_t n, const char *file, const char *sigpath,
                const char *expect_name, char **error);
/* Same; on success also returns the verified trusted comment (free it) and
 * the key number of the signer (either may be NULL). */
int verify_file_ex(const struct pubkey *keys, size_t n, const char *file, const char *sigpath,
                   const char *expect_name, char **error, char **trusted_out,
                   unsigned char signer[8]);

/* ---- trust.c: index freshness, key list, key rotation ---------------------------- */

#include <time.h>

/* "name:value" field of a trusted comment, malloc'd, or NULL. */
char *trusted_field(const char *trusted, const char *name);
bool iso_parse(const char *s, time_t *out);           /* 2026-11-25T12:00:00Z */
void iso_format(time_t t, char out[32]);

/* The keys that count now: cfg.keydir + learned keys, minus retired ones. */
void keys_effective(struct pubkey **keys, size_t *n);
/* Appended to a "signed by unknown key" error: a hint when a rotation retired keys, else "". */
char *keys_retired_hint(void);
/* Where learned keys go during check-updates (a scratch dir; NULL: the state dir). */
void trust_set_scratch(const char *dir);

/* Verify the freshness of a signed index of repo REPO: it must carry a serial and
 * valid-until, not be expired, and not be older than the last one accepted.
 * Returns the serial, or -1 with *ERR set to a message that says what to do. */
long long index_check(const char *repo, const char *trusted, char **err);
void index_commit(const char *repo, long long serial); /* remember it as accepted */
/* Warn if the cached index of REPO (its .sig) has expired. */
void index_warn_cached(const char *repo, const char *sigpath);
/* The serial to give the next index in DIR: above the one there, and at least the clock. */
long long index_next_serial(const char *dir);

/* Act on the key rotation statement a repo offers (rotate.pub and its .sig): trust the new
 * key and schedule the old one's retirement, if the statement is signed by a trusted key. */
void rotate_apply(const char *repo, const char *pubfile, const char *sigfile);

int cmd_key_introduce(int argc, char **argv);
int cmd_key_list(void);

#endif
