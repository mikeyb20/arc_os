/* arc_os — Host-side tests for net_util (byte order + checksum) */

#include "test_framework.h"
#include <stdint.h>

/* Guard kernel headers */
#define ARCHOS_NET_NET_UTIL_H

/* Inline the byte-order helpers directly for host tests */
static inline uint16_t htons(uint16_t h) {
    return (uint16_t)((h >> 8) | (h << 8));
}
static inline uint16_t ntohs(uint16_t n) { return htons(n); }

static inline uint32_t htonl(uint32_t h) {
    return ((h >> 24) & 0xFF) | ((h >> 8) & 0xFF00) |
           ((h << 8) & 0xFF0000) | ((h << 24) & 0xFF000000u);
}
static inline uint32_t ntohl(uint32_t n) { return htonl(n); }

static inline uint32_t IP4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
}

/* Include the .c for net_checksum */
#include "../kernel/net/net_util.c"

/* --- Tests --- */

TEST(htons_known_value) {
    ASSERT_EQ(htons(0x1234), 0x3412);
    return 0;
}

TEST(htons_roundtrip) {
    uint16_t val = 0xABCD;
    ASSERT_EQ(ntohs(htons(val)), val);
    return 0;
}

TEST(htons_zero) {
    ASSERT_EQ(htons(0), 0);
    return 0;
}

TEST(htonl_known_value) {
    ASSERT_EQ(htonl(0x12345678), 0x78563412);
    return 0;
}

TEST(htonl_roundtrip) {
    uint32_t val = 0xDEADBEEF;
    ASSERT_EQ(ntohl(htonl(val)), val);
    return 0;
}

TEST(htonl_zero) {
    ASSERT_EQ(htonl(0), 0);
    return 0;
}

TEST(ip4_macro) {
    uint32_t ip = IP4(10, 0, 2, 15);
    uint8_t *bytes = (uint8_t *)&ip;
    ASSERT_EQ(bytes[0], 10);
    ASSERT_EQ(bytes[1], 0);
    ASSERT_EQ(bytes[2], 2);
    ASSERT_EQ(bytes[3], 15);
    return 0;
}

TEST(ip4_loopback) {
    uint32_t ip = IP4(127, 0, 0, 1);
    uint8_t *bytes = (uint8_t *)&ip;
    ASSERT_EQ(bytes[0], 127);
    ASSERT_EQ(bytes[3], 1);
    return 0;
}

TEST(checksum_all_zeros) {
    uint8_t data[4] = {0, 0, 0, 0};
    uint16_t csum = net_checksum(data, 4);
    ASSERT_EQ(csum, 0xFFFF);
    return 0;
}

TEST(checksum_all_ones) {
    uint8_t data[2] = {0xFF, 0xFF};
    uint16_t csum = net_checksum(data, 2);
    ASSERT_EQ(csum, 0);
    return 0;
}

TEST(checksum_odd_length) {
    /* Single byte 0x01 -> sum = 0x0100, complement = 0xFEFF */
    uint8_t data[1] = {0x01};
    uint16_t csum = net_checksum(data, 1);
    ASSERT_EQ(csum, 0xFEFF);
    return 0;
}

TEST(checksum_ipv4_header) {
    /* Real IPv4 header (from RFC 1071 style): version=4, IHL=5, total_len=40,
     * id=0, flags_frag=0x4000, TTL=64, proto=6(TCP), checksum=0,
     * src=192.168.1.1, dst=192.168.1.2 */
    uint8_t hdr[20] = {
        0x45, 0x00, 0x00, 0x28,  /* ver/ihl, tos, total_len */
        0x00, 0x00, 0x40, 0x00,  /* id, flags/frag */
        0x40, 0x06, 0x00, 0x00,  /* ttl, proto, checksum=0 */
        0xC0, 0xA8, 0x01, 0x01,  /* src: 192.168.1.1 */
        0xC0, 0xA8, 0x01, 0x02,  /* dst: 192.168.1.2 */
    };
    uint16_t csum = net_checksum(hdr, 20);
    /* Fill in the checksum and verify it validates */
    hdr[10] = (uint8_t)(csum >> 8);
    hdr[11] = (uint8_t)(csum & 0xFF);
    ASSERT_EQ(net_checksum(hdr, 20), 0);
    return 0;
}

/* --- Suite --- */

TestCase net_util_tests[] = {
    TEST_ENTRY(htons_known_value),
    TEST_ENTRY(htons_roundtrip),
    TEST_ENTRY(htons_zero),
    TEST_ENTRY(htonl_known_value),
    TEST_ENTRY(htonl_roundtrip),
    TEST_ENTRY(htonl_zero),
    TEST_ENTRY(ip4_macro),
    TEST_ENTRY(ip4_loopback),
    TEST_ENTRY(checksum_all_zeros),
    TEST_ENTRY(checksum_all_ones),
    TEST_ENTRY(checksum_odd_length),
    TEST_ENTRY(checksum_ipv4_header),
};
int net_util_test_count = sizeof(net_util_tests) / sizeof(net_util_tests[0]);
