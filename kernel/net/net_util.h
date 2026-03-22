#ifndef ARCHOS_NET_NET_UTIL_H
#define ARCHOS_NET_NET_UTIL_H

#include <stdint.h>

static inline uint16_t htons(uint16_t h) {
    return (uint16_t)((h >> 8) | (h << 8));
}
static inline uint16_t ntohs(uint16_t n) { return htons(n); }

static inline uint32_t htonl(uint32_t h) {
    return ((h >> 24) & 0xFF) | ((h >> 8) & 0xFF00) |
           ((h << 8) & 0xFF0000) | ((h << 24) & 0xFF000000u);
}
static inline uint32_t ntohl(uint32_t n) { return htonl(n); }

/* Internet checksum (RFC 1071). */
uint16_t net_checksum(const void *data, uint32_t len);

/* Build a 32-bit IP from 4 octets: IP4(10,0,2,15) -> network byte order */
static inline uint32_t IP4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
}

#endif /* ARCHOS_NET_NET_UTIL_H */
