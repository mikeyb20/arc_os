#ifndef ARCHOS_LIB_CRC32_H
#define ARCHOS_LIB_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* CRC-32 as used by Ethernet, gzip, zlib and PNG (IEEE 802.3: reflected
 * polynomial 0xEDB88320, initial value and final XOR 0xFFFFFFFF).
 *
 * zlib-style chaining: pass 0 to start, then the previous result to continue,
 * so crc32(crc32(0, a, n), b, m) equals the CRC of a followed by b.
 * crc32(0, "123456789", 9) == 0xCBF43926. Table driven, no locks, no state:
 * safe from any context, including the panic path. */
uint32_t crc32(uint32_t crc, const void *data, size_t len);

#endif /* ARCHOS_LIB_CRC32_H */
