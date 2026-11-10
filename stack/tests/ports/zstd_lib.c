/*
 * lane.zstd_lib (M2-02): the zstd library the way arc_os programs use it.
 *
 * A 3 MB buffer goes through the one-shot API and through the streaming API with
 * 128 KiB buffers (the path arcpkg uses for packages of any size, M5-02). Every frame is
 * decompressed with ZSTD_d_windowLogMax = 23, the 8 MiB window cap that keeps
 * decompression memory small on a 4 GB machine, and a frame that needs a 16 MiB window
 * must be refused under that cap.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "../lane_check.h"

#define INPUT_BYTES    (3u * 1000u * 1000u)
#define CHUNK          (128u * 1024u)
#define WINDOW_LOG_MAX 23
#define BIG_WINDOW_LOG 24

/* fill: deterministic, compressible text (an xorshift generator picks words). */
static void fill(unsigned char *buf, size_t n) {
    static const char *const words[] = {"water ", "boil ",   "minute ", "safe ",        "drink ",
                                        "warm ",  "slowly ", "body ",   "temperature ", "\n"};
    uint32_t x = 2463534242u;
    size_t i = 0;
    while (i < n) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        const char *w = words[x % (sizeof(words) / sizeof(words[0]))];
        size_t len = strlen(w);
        if (len > n - i) {
            len = n - i;
        }
        memcpy(buf + i, w, len);
        i += len;
    }
}

/*
 * stream_compress: compress `in` with level 3 (and window_log if non-zero), feeding
 * CHUNK-sized pieces through a CHUNK-sized output buffer into dst. The content size is
 * not pledged, so the frame header carries the window size. Returns the compressed size,
 * 0 on error.
 */
static size_t stream_compress(const unsigned char *in, size_t n, unsigned char *dst, size_t cap, int window_log) {
    ZSTD_CCtx *cctx = ZSTD_createCCtx();
    unsigned char *buf = malloc(CHUNK);
    size_t total = 0;
    size_t pos = 0;
    int ok = cctx != NULL && buf != NULL && !ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 3));
    if (ok && window_log) {
        ok = !ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_windowLog, window_log));
    }
    while (ok) {
        size_t take = n - pos < CHUNK ? n - pos : CHUNK;
        ZSTD_EndDirective mode = pos + take == n ? ZSTD_e_end : ZSTD_e_continue;
        ZSTD_inBuffer ib = {in + pos, take, 0};
        size_t left;
        do {
            ZSTD_outBuffer ob = {buf, CHUNK, 0};
            left = ZSTD_compressStream2(cctx, &ob, &ib, mode);
            if (ZSTD_isError(left) || total + ob.pos > cap) {
                ok = 0;
                break;
            }
            memcpy(dst + total, buf, ob.pos);
            total += ob.pos;
        } while (mode == ZSTD_e_end ? left != 0 : ib.pos < ib.size);
        pos += take;
        if (mode == ZSTD_e_end) {
            break;
        }
    }
    free(buf);
    ZSTD_freeCCtx(cctx);
    return ok ? total : 0;
}

/*
 * stream_decompress: decompress the frame in src, CHUNK bytes in and out at a time, with
 * the window capped at 2^window_log_max, and compare the output with expect. Returns 0 if
 * the frame decoded completely to exactly `expect`, else the zstd error code
 * (ZSTD_error_GENERIC for a mismatch or an incomplete frame).
 */
static ZSTD_ErrorCode stream_decompress(const unsigned char *src, size_t n, const unsigned char *expect,
                                        size_t expect_n, int window_log_max) {
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    unsigned char *buf = malloc(CHUNK);
    ZSTD_ErrorCode err = ZSTD_error_GENERIC;
    size_t done = 0;
    size_t pos = 0;
    size_t ret = 1;
    int match = 1;
    if (dctx == NULL || buf == NULL ||
        ZSTD_isError(ZSTD_DCtx_setParameter(dctx, ZSTD_d_windowLogMax, window_log_max))) {
        goto out;
    }
    while (pos < n) {
        size_t take = n - pos < CHUNK ? n - pos : CHUNK;
        ZSTD_inBuffer ib = {src + pos, take, 0};
        ZSTD_outBuffer ob;
        do {
            ob = (ZSTD_outBuffer){buf, CHUNK, 0};
            ret = ZSTD_decompressStream(dctx, &ob, &ib);
            if (ZSTD_isError(ret)) {
                err = ZSTD_getErrorCode(ret);
                goto out;
            }
            if (done + ob.pos > expect_n || memcmp(expect + done, buf, ob.pos) != 0) {
                match = 0;
            }
            done += ob.pos;
        } while (ib.pos < ib.size || ob.pos == ob.size);
        pos += take;
    }
    if (ret == 0 && match && done == expect_n) {
        err = ZSTD_error_no_error;
    }
out:
    free(buf);
    ZSTD_freeDCtx(dctx);
    return err;
}

/* one_shot: ZSTD_compress / ZSTD_decompress round trip, then the same frame streamed. */
static void one_shot(const unsigned char *in, size_t n, unsigned char *z, size_t cap, unsigned char *out) {
    size_t zn = ZSTD_compress(z, cap, in, n, 3);
    lane_check(!ZSTD_isError(zn) && zn < n / 2, "one-shot compress");
    if (ZSTD_isError(zn)) {
        return;
    }
    lane_check(ZSTD_getFrameContentSize(z, zn) == n, "one-shot frame records the content size");
    size_t dn = ZSTD_decompress(out, n, z, zn);
    lane_check(!ZSTD_isError(dn) && dn == n && memcmp(in, out, n) == 0, "one-shot round trip");
    lane_check(stream_decompress(z, zn, in, n, WINDOW_LOG_MAX) == ZSTD_error_no_error,
               "one-shot frame streams back with an 8 MiB window cap");
}

/* streaming: compress in pieces; decode under the cap; a 16 MiB-window frame is refused. */
static void streaming(const unsigned char *in, size_t n, unsigned char *z, size_t cap) {
    size_t zn = stream_compress(in, n, z, cap, 0);
    lane_check(zn != 0 && zn < n / 2, "streaming compress");
    lane_check(stream_decompress(z, zn, in, n, WINDOW_LOG_MAX) == ZSTD_error_no_error,
               "streaming round trip with an 8 MiB window cap");

    zn = stream_compress(in, n, z, cap, BIG_WINDOW_LOG);
    lane_check(zn != 0 && stream_decompress(z, zn, in, n, BIG_WINDOW_LOG) == ZSTD_error_no_error,
               "16 MiB-window frame decodes with a 16 MiB cap");
    lane_check(stream_decompress(z, zn, in, n, WINDOW_LOG_MAX) == ZSTD_error_frameParameter_windowTooLarge,
               "16 MiB-window frame refused under the 8 MiB cap");
}

int main(void) {
    size_t cap = ZSTD_compressBound(INPUT_BYTES);
    unsigned char *in = malloc(INPUT_BYTES);
    unsigned char *out = malloc(INPUT_BYTES);
    unsigned char *z = malloc(cap);
    if (in == NULL || out == NULL || z == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    lane_check(ZSTD_versionNumber() == ZSTD_VERSION_NUMBER, "library and header versions match");
    fill(in, INPUT_BYTES);
    one_shot(in, INPUT_BYTES, z, cap, out);
    streaming(in, INPUT_BYTES, z, cap);
    free(z);
    free(out);
    free(in);
    return lane_check_done("zstd_lib");
}
