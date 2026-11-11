/* SHA-256 and BLAKE2b-512 through OpenSSL's libcrypto.
 * SPDX-License-Identifier: MIT */
#include "flpkg.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <openssl/evp.h>
#include <unistd.h>

struct hasher {
	EVP_MD_CTX *ctx;
};

struct hasher *hasher_new(const char *algo)
{
	const EVP_MD *md = EVP_get_digestbyname(algo);
	if (!md)
		die("hash algorithm %s not available", algo);
	struct hasher *h = xmalloc(sizeof(*h));
	h->ctx = EVP_MD_CTX_new();
	if (!h->ctx || !EVP_DigestInit_ex(h->ctx, md, NULL))
		die("cannot initialise %s", algo);
	return h;
}

void hasher_update(struct hasher *h, const void *data, size_t len)
{
	if (len && !EVP_DigestUpdate(h->ctx, data, len))
		die("hash update failed");
}

size_t hasher_final(struct hasher *h, unsigned char *out)
{
	unsigned int n = 0;
	if (!EVP_DigestFinal_ex(h->ctx, out, &n))
		die("hash final failed");
	EVP_MD_CTX_free(h->ctx);
	free(h);
	return n;
}

void hex_encode(const unsigned char *in, size_t len, char *out)
{
	static const char digits[] = "0123456789abcdef";
	for (size_t i = 0; i < len; i++) {
		out[2 * i] = digits[in[i] >> 4];
		out[2 * i + 1] = digits[in[i] & 15];
	}
	out[2 * len] = '\0';
}

int sha256_fd(int fd, char hex[SHA256_HEX])
{
	struct hasher *h = hasher_new("SHA256");
	unsigned char buf[65536], md[EVP_MAX_MD_SIZE];
	ssize_t n;
	while ((n = read(fd, buf, sizeof(buf))) != 0) {
		if (n < 0) {
			if (errno == EINTR)
				continue;
			hasher_final(h, md);
			return -1;
		}
		hasher_update(h, buf, (size_t)n);
	}
	hasher_final(h, md);
	hex_encode(md, SHA256_LEN, hex);
	return 0;
}

int sha256_file(const char *path, char hex[SHA256_HEX])
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	int rc = sha256_fd(fd, hex);
	close(fd);
	return rc;
}

void sha256_mem(const void *data, size_t len, char hex[SHA256_HEX])
{
	struct hasher *h = hasher_new("SHA256");
	unsigned char md[EVP_MAX_MD_SIZE];
	hasher_update(h, data, len);
	hasher_final(h, md);
	hex_encode(md, SHA256_LEN, hex);
}
