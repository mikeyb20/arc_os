#ifndef ARCHOS_DIAG_QR_H
#define ARCHOS_DIAG_QR_H

#include <stddef.h>
#include <stdint.h>

/* QR Code encoder (ISO/IEC 18004) for diagnostic text (M1-04b).
 *
 * Byte mode, error-correction level M, versions 1-30. The caller owns the
 * QrCode, which holds the finished module bitmap and the encoder's work area:
 * the encoder allocates nothing, keeps no globals, takes no locks and uses no
 * floating point, so the panic path can call it. A QrCode is about 7 KiB, so
 * kernel callers keep it static, never on a kernel stack.
 *
 * The output is only modules; drawing them (scale, quiet zone, colours) is the
 * caller's job (M1-04c). */

#define QR_VERSION_MAX   30
#define QR_SIZE_MAX      (17 + 4 * QR_VERSION_MAX)   /* 137 modules per side */
#define QR_ROW_BYTES     ((QR_SIZE_MAX + 7) / 8)     /* 18 bytes per bitmap row */
#define QR_CODEWORDS_MAX 2185                        /* data + ECC codewords, version 30 */
#define QR_BYTES_MAX     1370                        /* byte-mode capacity of version 30-M */

typedef struct {
    int version;                                     /* 1..30 */
    int size;                                        /* modules per side: 17 + 4 * version */
    int mask;                                        /* mask pattern chosen, 0..7 */
    /* Finished symbol: 1 = dark. Row y, column x is bit 7 - (x % 8) of
     * modules[y * QR_ROW_BYTES + x / 8] (most significant bit first, as in PBM). */
    uint8_t modules[QR_SIZE_MAX * QR_ROW_BYTES];
    /* Work area, meaningless after qr_encode returns */
    uint8_t function[QR_SIZE_MAX * QR_ROW_BYTES];    /* 1 = finder, timing, alignment, format, version */
    uint8_t codewords[QR_CODEWORDS_MAX];             /* data blocks, then the ECC blocks */
} QrCode;

/* Encode len bytes as one QR code in byte mode at error-correction level M,
 * using the smallest version from 1 to max_version that holds them.
 * Returns the version (1..30), -E2BIG if the data needs a version above
 * max_version, or -EINVAL for a bad argument (max_version outside 1..30,
 * NULL out, NULL data with len > 0). len 0 is a valid, empty code.
 * Cost: linear in the module count for each of the 8 masks, so diagnostic
 * and panic paths only: measured 4 ms at version 30 (0.6 ms at version 10) on
 * a 2.1 GHz Xeon at -O2, 9 ms at -O0. */
int qr_encode(const uint8_t *data, size_t len, int max_version, QrCode *out);

/* Bytes one code of this version holds in byte mode at level M (14 for
 * version 1, 1370 for version 30); 0 for a version outside 1..30. */
size_t qr_capacity(int version);

/* 1 if module (x, y) of an encoded code is dark, 0 if light or outside it. */
static inline int qr_module(const QrCode *qr, int x, int y) {
    if (x < 0 || y < 0 || x >= qr->size || y >= qr->size) return 0;
    return (qr->modules[y * QR_ROW_BYTES + x / 8] >> (7 - x % 8)) & 1;
}

#endif /* ARCHOS_DIAG_QR_H */
