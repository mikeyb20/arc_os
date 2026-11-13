#ifndef ARCHOS_TESTS_ZBAR_DECODE_H
#define ARCHOS_TESTS_ZBAR_DECODE_H

#include <stddef.h>
#include <stdint.h>

/* Decode the QR code in an image file (PGM, PPM, ...) with zbarimg
 * (zbar-tools, the host's reference decoder) into out, at most cap bytes.
 * Returns the byte count, or -1 if zbarimg is missing or found no code.
 * Defined in test_qr.c. */
long zbar_decode_file(const char *path, uint8_t *out, size_t cap);

#endif /* ARCHOS_TESTS_ZBAR_DECODE_H */
