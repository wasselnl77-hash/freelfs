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
/* Load every *.pub file in DIR. */
void pubkeys_load(const char *dir, struct pubkey **keys, size_t *n);
void pubkeys_free(struct pubkey *keys, size_t n);

/* Write FILE.sig: a prehashed ("ED") signature whose trusted comment
 * names the file, so a signature cannot be moved to another file. */
int sign_file(const struct seckey *k, const char *file, char **error);
/* Check FILE against SIGPATH with one of KEYS; the trusted comment must
 * name EXPECT_NAME. */
int verify_file(const struct pubkey *keys, size_t n, const char *file, const char *sigpath,
                const char *expect_name, char **error);

#endif
