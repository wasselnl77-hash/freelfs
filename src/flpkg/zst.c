/* zstd compression and decompression streams over file descriptors.
 * SPDX-License-Identifier: MIT */
#include "flpkg.h"

#include <stdlib.h>
#include <string.h>
#include <zstd.h>

struct zwriter {
	ZSTD_CCtx *cctx;
	int fd;
	void *out;
	size_t out_size;
};

/* The same level and worker mode give byte-identical output whatever the
 * worker count, so packages stay reproducible across machines as long as
 * workers >= 1. */
struct zwriter *zw_open(int fd, int level, int workers)
{
	struct zwriter *zw = xcalloc(1, sizeof(*zw));
	zw->cctx = ZSTD_createCCtx();
	if (!zw->cctx)
		die("cannot create zstd context");
	ZSTD_CCtx_setParameter(zw->cctx, ZSTD_c_compressionLevel, level);
	ZSTD_CCtx_setParameter(zw->cctx, ZSTD_c_checksumFlag, 1);
	if (workers > 0 && ZSTD_isError(ZSTD_CCtx_setParameter(zw->cctx, ZSTD_c_nbWorkers, workers)))
		die("this zstd library was built without multithreading");
	zw->fd = fd;
	zw->out_size = ZSTD_CStreamOutSize();
	zw->out = xmalloc(zw->out_size);
	return zw;
}

static int zw_pump(struct zwriter *zw, const void *buf, size_t len, ZSTD_EndDirective mode)
{
	ZSTD_inBuffer in = {buf, len, 0};
	for (;;) {
		ZSTD_outBuffer out = {zw->out, zw->out_size, 0};
		size_t left = ZSTD_compressStream2(zw->cctx, &out, &in, mode);
		if (ZSTD_isError(left)) {
			warn("zstd: %s", ZSTD_getErrorName(left));
			return -1;
		}
		if (out.pos && write_all(zw->fd, zw->out, out.pos) < 0)
			return -1;
		if (mode == ZSTD_e_end ? left == 0 : in.pos == in.size)
			return 0;
	}
}

int zw_write(struct zwriter *zw, const void *buf, size_t len)
{
	return len ? zw_pump(zw, buf, len, ZSTD_e_continue) : 0;
}

int zw_close(struct zwriter *zw)
{
	int rc = zw_pump(zw, NULL, 0, ZSTD_e_end);
	ZSTD_freeCCtx(zw->cctx);
	free(zw->out);
	free(zw);
	return rc;
}

struct zreader {
	ZSTD_DCtx *dctx;
	int fd;
	void *in;
	size_t in_size;
	ZSTD_inBuffer ib;
	bool eof;
	size_t last; /* ZSTD_decompressStream's last hint: 0 at a frame end */
};

struct zreader *zr_open(int fd)
{
	struct zreader *zr = xcalloc(1, sizeof(*zr));
	zr->dctx = ZSTD_createDCtx();
	if (!zr->dctx)
		die("cannot create zstd context");
	zr->fd = fd;
	zr->in_size = ZSTD_DStreamInSize();
	zr->in = xmalloc(zr->in_size);
	zr->ib = (ZSTD_inBuffer){zr->in, 0, 0};
	return zr;
}

ssize_t zr_read(struct zreader *zr, void *buf, size_t len)
{
	ZSTD_outBuffer out = {buf, len, 0};
	while (out.pos == 0 && len) {
		if (zr->ib.pos == zr->ib.size) {
			if (zr->eof)
				break;
			ssize_t n = read_full(zr->fd, zr->in, zr->in_size);
			if (n < 0)
				return -1;
			if (n == 0) {
				zr->eof = true;
				if (zr->last != 0) {
					warn("zstd: truncated stream");
					return -1;
				}
				break;
			}
			zr->ib = (ZSTD_inBuffer){zr->in, (size_t)n, 0};
		}
		zr->last = ZSTD_decompressStream(zr->dctx, &out, &zr->ib);
		if (ZSTD_isError(zr->last)) {
			warn("zstd: %s", ZSTD_getErrorName(zr->last));
			return -1;
		}
	}
	return (ssize_t)out.pos;
}

void zr_close(struct zreader *zr)
{
	ZSTD_freeDCtx(zr->dctx);
	free(zr->in);
	free(zr);
}
