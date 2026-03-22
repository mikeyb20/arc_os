#include "net/net_util.h"

uint16_t net_checksum(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;

    /* Sum 16-bit words */
    while (len > 1) {
        sum += (uint32_t)p[0] << 8 | p[1];
        p += 2;
        len -= 2;
    }
    /* Odd byte */
    if (len == 1) {
        sum += (uint32_t)p[0] << 8;
    }
    /* Fold 32-bit carry into 16 bits */
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)~sum;
}
