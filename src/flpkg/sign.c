/* Ed25519 signatures in minisign's file formats, through OpenSSL.
 *
 * Public key file:  "untrusted comment: ..." and base64("Ed" keynum[8] pk[32])
 * Secret key file:  "untrusted comment: ..." and base64("Ed" kdf[2] "B2"
 *                   salt[32] opslimit[8] memlimit[8] keynum[8] sk[64]
 *                   checksum[32]); kdf {0,0} means unencrypted, sk is
 *                   seed || public key, checksum is BLAKE2b-256 of
 *                   "Ed" keynum sk.
 * Signature file:   "untrusted comment: ...", base64("ED" keynum[8] sig[64])
 *                   where sig signs BLAKE2b-512(file), then
 *                   "trusted comment: TEXT" and base64(global[64]) where
 *                   global signs sig[64] || TEXT.
 * SPDX-License-Identifier: MIT */
#include "sign.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SECKEY_LEN 158
#define PUBKEY_LEN 42
#define SIG_LEN 74

/* ---- small crypto helpers ---------------------------------------------------- */

static char *b64_encode(const unsigned char *in, size_t len)
{
	char *out = xmalloc(4 * ((len + 2) / 3) + 1);
	EVP_EncodeBlock((unsigned char *)out, in, (int)len);
	return out;
}

/* Decode exactly WANT bytes, or fail. */
static int b64_decode(const char *in, unsigned char *out, size_t want)
{
	size_t n = strlen(in);
	while (n && (in[n - 1] == '\n' || in[n - 1] == '\r' || in[n - 1] == ' '))
		n--;
	if (n % 4)
		return -1;
	unsigned char *buf = xmalloc(n / 4 * 3 + 1);
	int got = EVP_DecodeBlock(buf, (const unsigned char *)in, (int)n);
	if (got < 0) {
		free(buf);
		return -1;
	}
	size_t pad = (n && in[n - 1] == '=') + (n > 1 && in[n - 2] == '=');
	int rc = (size_t)got - pad == want ? 0 : -1;
	if (!rc)
		memcpy(out, buf, want);
	free(buf);
	return rc;
}

static void blake2b(const void *data, size_t len, unsigned char *out, size_t outlen)
{
	EVP_MD *md = EVP_MD_fetch(NULL, "BLAKE2B-512", NULL);
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	size_t size = outlen;
	OSSL_PARAM params[] = {OSSL_PARAM_construct_size_t(OSSL_DIGEST_PARAM_SIZE, &size),
	                       OSSL_PARAM_construct_end()};
	unsigned int n = 0;
	if (!md || !ctx || !EVP_DigestInit_ex2(ctx, md, outlen == 64 ? NULL : params) ||
	    !EVP_DigestUpdate(ctx, data, len) || !EVP_DigestFinal_ex(ctx, out, &n) || n != outlen)
		die("BLAKE2b-%zu is not available in this OpenSSL", outlen * 8);
	EVP_MD_CTX_free(ctx);
	EVP_MD_free(md);
}

static int blake2b_file(const char *path, unsigned char out[64])
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	struct hasher *h = hasher_new("BLAKE2B-512");
	unsigned char buf[65536];
	ssize_t n;
	while ((n = read(fd, buf, sizeof(buf))) > 0)
		hasher_update(h, buf, (size_t)n);
	hasher_final(h, out);
	close(fd);
	return n < 0 ? -1 : 0;
}

static void ed_sign(const unsigned char seed[32], const unsigned char *msg, size_t len,
                    unsigned char sig[64])
{
	EVP_PKEY *k = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32);
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	size_t siglen = 64;
	if (!k || !ctx || EVP_DigestSignInit(ctx, NULL, NULL, NULL, k) != 1 ||
	    EVP_DigestSign(ctx, sig, &siglen, msg, len) != 1 || siglen != 64)
		die("Ed25519 signing failed");
	EVP_MD_CTX_free(ctx);
	EVP_PKEY_free(k);
}

static bool ed_verify(const unsigned char pk[32], const unsigned char *msg, size_t len,
                      const unsigned char sig[64])
{
	EVP_PKEY *k = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, pk, 32);
	EVP_MD_CTX *ctx = EVP_MD_CTX_new();
	bool ok = k && ctx && EVP_DigestVerifyInit(ctx, NULL, NULL, NULL, k) == 1 &&
	          EVP_DigestVerify(ctx, sig, 64, msg, len) == 1;
	EVP_MD_CTX_free(ctx);
	EVP_PKEY_free(k);
	return ok;
}

/* minisign shows the key number as a little-endian 64-bit hex number */
static void keyid_hex(const unsigned char keynum[8], char out[17])
{
	static const char digits[] = "0123456789ABCDEF";
	for (int i = 0; i < 8; i++) {
		out[2 * i] = digits[keynum[7 - i] >> 4];
		out[2 * i + 1] = digits[keynum[7 - i] & 15];
	}
	out[16] = '\0';
}

/* The base64 line after an "untrusted comment:" line. */
static char *second_line(const char *text, const char *path, char **error)
{
	if (!has_prefix(text, "untrusted comment:")) {
		*error = xasprintf("%s: not a minisign file", path);
		return NULL;
	}
	const char *nl = strchr(text, '\n');
	if (!nl) {
		*error = xasprintf("%s: truncated", path);
		return NULL;
	}
	return xstrndup(nl + 1, strcspn(nl + 1, "\n"));
}

/* ---- keys -------------------------------------------------------------------- */

void key_generate(const char *base)
{
	EVP_PKEY *k = EVP_PKEY_Q_keygen(NULL, NULL, "ED25519");
	struct seckey sk;
	size_t n = 32;
	if (!k || EVP_PKEY_get_raw_private_key(k, sk.seed, &n) != 1 || n != 32 ||
	    EVP_PKEY_get_raw_public_key(k, sk.pk, &n) != 1 || n != 32 ||
	    RAND_bytes(sk.keynum, 8) != 1)
		die("cannot generate an Ed25519 key");
	EVP_PKEY_free(k);

	unsigned char sec[SECKEY_LEN] = {0}, pub[PUBKEY_LEN];
	memcpy(sec, "Ed", 2);   /* signature algorithm */
	                        /* kdf {0,0}: not encrypted */
	memcpy(sec + 4, "B2", 2);
	unsigned char *ks = sec + 2 + 2 + 2 + 32 + 8 + 8;
	memcpy(ks, sk.keynum, 8);
	memcpy(ks + 8, sk.seed, 32);
	memcpy(ks + 40, sk.pk, 32);
	unsigned char chk_in[2 + 8 + 64];
	memcpy(chk_in, "Ed", 2);
	memcpy(chk_in + 2, ks, 72);
	blake2b(chk_in, sizeof(chk_in), ks + 72, 32);
	memcpy(pub, "Ed", 2);
	memcpy(pub + 2, sk.keynum, 8);
	memcpy(pub + 10, sk.pk, 32);

	char id[17];
	keyid_hex(sk.keynum, id);
	char *b = b64_encode(sec, sizeof(sec));
	char *text = xasprintf("untrusted comment: flpkg secret key %s (not encrypted)\n%s\n", id, b);
	char *path = xasprintf("%s.key", base);
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0 || write_all(fd, text, strlen(text)) < 0 || close(fd) < 0)
		die("cannot write %s: %s", path, strerror(errno));
	free(b);
	free(text);
	free(path);
	b = b64_encode(pub, sizeof(pub));
	text = xasprintf("untrusted comment: minisign public key %s\n%s\n", id, b);
	path = xasprintf("%s.pub", base);
	if (write_file(path, text, strlen(text), 0644) < 0)
		die("cannot write %s: %s", path, strerror(errno));
	msg("key %s: secret %s.key, public %s.pub", id, base, base);
	free(b);
	free(text);
	free(path);
	OPENSSL_cleanse(&sk, sizeof(sk));
	OPENSSL_cleanse(sec, sizeof(sec));
}

int seckey_load(const char *path, struct seckey *k, char **error)
{
	char *text = read_file(path, NULL);
	if (!text) {
		*error = xasprintf("cannot read %s: %s", path, strerror(errno));
		return -1;
	}
	char *line = second_line(text, path, error);
	free(text);
	if (!line)
		return -1;
	unsigned char sec[SECKEY_LEN];
	int rc = b64_decode(line, sec, sizeof(sec));
	free(line);
	if (rc < 0 || memcmp(sec, "Ed", 2) || memcmp(sec + 4, "B2", 2)) {
		*error = xasprintf("%s: not a minisign secret key", path);
		return -1;
	}
	if (sec[2] || sec[3]) {
		*error = xasprintf("%s: encrypted keys are not supported yet", path);
		return -1;
	}
	unsigned char *ks = sec + 2 + 2 + 2 + 32 + 8 + 8, chk_in[74], chk[32];
	memcpy(chk_in, "Ed", 2);
	memcpy(chk_in + 2, ks, 72);
	blake2b(chk_in, sizeof(chk_in), chk, 32);
	if (memcmp(chk, ks + 72, 32)) {
		*error = xasprintf("%s: checksum mismatch (corrupt key)", path);
		return -1;
	}
	memcpy(k->keynum, ks, 8);
	memcpy(k->seed, ks + 8, 32);
	memcpy(k->pk, ks + 40, 32);
	OPENSSL_cleanse(sec, sizeof(sec));
	return 0;
}

static int pubkey_parse(const char *text, const char *path, struct pubkey *k, char **error)
{
	char *line = second_line(text, path, error);
	if (!line)
		return -1;
	unsigned char pub[PUBKEY_LEN];
	int rc = b64_decode(line, pub, sizeof(pub));
	free(line);
	if (rc < 0 || memcmp(pub, "Ed", 2)) {
		*error = xasprintf("%s: not a minisign public key", path);
		return -1;
	}
	memcpy(k->keynum, pub + 2, 8);
	memcpy(k->pk, pub + 10, 32);
	k->path = xstrdup(path);
	return 0;
}

void pubkeys_load(const char *dir, struct pubkey **keys, size_t *n)
{
	*keys = NULL;
	*n = 0;
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *de;
	while ((de = readdir(d))) {
		if (!has_suffix(de->d_name, ".pub"))
			continue;
		char *path = path_join(dir, de->d_name), *err = NULL;
		char *text = read_file(path, NULL);
		struct pubkey k;
		if (text && pubkey_parse(text, path, &k, &err) == 0) {
			*keys = xrealloc(*keys, (*n + 1) * sizeof(**keys));
			(*keys)[(*n)++] = k;
		} else {
			warn("ignoring key %s%s%s", path, err ? ": " : "", err ? err : "");
		}
		free(err);
		free(text);
		free(path);
	}
	closedir(d);
}

void pubkeys_free(struct pubkey *keys, size_t n)
{
	for (size_t i = 0; i < n; i++)
		free(keys[i].path);
	free(keys);
}

/* ---- signatures -------------------------------------------------------------- */

int sign_file(const struct seckey *k, const char *file, char **error)
{
	unsigned char hash[64], sig[SIG_LEN], global[64];
	if (blake2b_file(file, hash) < 0) {
		*error = xasprintf("cannot read %s: %s", file, strerror(errno));
		return -1;
	}
	memcpy(sig, "ED", 2);
	memcpy(sig + 2, k->keynum, 8);
	ed_sign(k->seed, hash, sizeof(hash), sig + 10);

	const char *base = strrchr(file, '/') ? strrchr(file, '/') + 1 : file;
	char *trusted = xasprintf("timestamp:%lld\tfile:%s\thashed",
	                          (long long)cfg.source_date_epoch, base);
	size_t tlen = strlen(trusted);
	unsigned char *gmsg = xmalloc(64 + tlen);
	memcpy(gmsg, sig + 10, 64);
	memcpy(gmsg + 64, trusted, tlen);
	ed_sign(k->seed, gmsg, 64 + tlen, global);

	char id[17];
	keyid_hex(k->keynum, id);
	char *s1 = b64_encode(sig, sizeof(sig)), *s2 = b64_encode(global, sizeof(global));
	char *text = xasprintf("untrusted comment: signature from flpkg key %s\n%s\n"
	                       "trusted comment: %s\n%s\n", id, s1, trusted, s2);
	char *path = xasprintf("%s.sig", file);
	int rc = write_file(path, text, strlen(text), 0644);
	if (rc < 0)
		*error = xasprintf("cannot write %s: %s", path, strerror(errno));
	free(path);
	free(text);
	free(s1);
	free(s2);
	free(gmsg);
	free(trusted);
	return rc;
}

int verify_file(const struct pubkey *keys, size_t n, const char *file, const char *sigpath,
                const char *expect_name, char **error)
{
	char *text = read_file(sigpath, NULL);
	if (!text) {
		*error = xasprintf("%s: no signature (%s)", file, strerror(errno));
		return -1;
	}
	int rc = -1;
	char *lines[4] = {0}, *save = NULL, *copy = xstrdup(text);
	int nl = 0;
	for (char *l = strtok_r(copy, "\n", &save); l && nl < 4; l = strtok_r(NULL, "\n", &save))
		lines[nl++] = l;
	unsigned char sig[SIG_LEN], global[64], hash[64];
	const struct pubkey *key = NULL;
	if (nl < 4 || !has_prefix(lines[0], "untrusted comment:") ||
	    !has_prefix(lines[2], "trusted comment: ") || b64_decode(lines[1], sig, sizeof(sig)) < 0 ||
	    b64_decode(lines[3], global, sizeof(global)) < 0) {
		*error = xasprintf("%s: malformed signature file", sigpath);
		goto out;
	}
	if (memcmp(sig, "ED", 2)) {
		*error = xasprintf("%s: only prehashed (ED) signatures are accepted", sigpath);
		goto out;
	}
	for (size_t i = 0; i < n && !key; i++)
		if (!memcmp(keys[i].keynum, sig + 2, 8))
			key = &keys[i];
	if (!key) {
		char id[17];
		keyid_hex(sig + 2, id);
		*error = xasprintf("%s: signed by unknown key %s", file, id);
		goto out;
	}
	if (blake2b_file(file, hash) < 0) {
		*error = xasprintf("cannot read %s: %s", file, strerror(errno));
		goto out;
	}
	if (!ed_verify(key->pk, hash, sizeof(hash), sig + 10)) {
		*error = xasprintf("%s: BAD SIGNATURE", file);
		goto out;
	}
	const char *trusted = lines[2] + strlen("trusted comment: ");
	size_t tlen = strlen(trusted);
	unsigned char *gmsg = xmalloc(64 + tlen);
	memcpy(gmsg, sig + 10, 64);
	memcpy(gmsg + 64, trusted, tlen);
	bool gok = ed_verify(key->pk, gmsg, 64 + tlen, global);
	free(gmsg);
	if (!gok) {
		*error = xasprintf("%s: BAD SIGNATURE (trusted comment)", file);
		goto out;
	}
	if (expect_name) {
		char *want = xasprintf("file:%s", expect_name);
		bool named = false;
		char *tcopy = xstrdup(trusted), *tsave = NULL;
		for (char *f = strtok_r(tcopy, "\t", &tsave); f; f = strtok_r(NULL, "\t", &tsave))
			named = named || !strcmp(f, want);
		free(tcopy);
		free(want);
		if (!named) {
			*error = xasprintf("%s: the signature is for a different file", file);
			goto out;
		}
	}
	rc = 0;
out:
	free(copy);
	free(text);
	return rc;
}
