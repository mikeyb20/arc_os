/* arc_os -- Host-side tests for UDP */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>

/* Guard kernel headers */
#define ARCHOS_NET_UDP_H
#define ARCHOS_NET_NET_UTIL_H
#define ARCHOS_NET_IPV4_H
#define ARCHOS_NET_NETIF_H
#define ARCHOS_NET_ETHERNET_H
#define ARCHOS_NET_SOCKET_H
#define ARCHOS_LIB_MEM_H
#define ARCHOS_LIB_KPRINTF_H

/* Inline types */
#define ETH_ALEN        6
#define ETH_MTU         1500
#define IPV4_HEADER_SIZE 20
#define IPV4_PROTO_ICMP 1
#define IPV4_PROTO_UDP  17
#define UDP_HEADER_SIZE  8

static inline uint16_t htons(uint16_t h) {
    return (uint16_t)((h >> 8) | (h << 8));
}
static inline uint16_t ntohs(uint16_t n) { return htons(n); }

static inline uint32_t IP4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
}

static uint16_t net_checksum(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    while (len > 1) {
        sum += (uint32_t)p[0] << 8 | p[1];
        p += 2; len -= 2;
    }
    if (len == 1) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed)) UdpHeader;

typedef struct NetIf {
    uint8_t  mac[ETH_ALEN];
    uint32_t ip_addr;
    uint32_t netmask;
    uint32_t gateway;
    int (*send)(struct NetIf *nif, const void *frame, uint32_t len);
    void *private_data;
} NetIf;

/* ipv4_send capture */
static uint8_t ipv4_send_buf[2048];
static uint32_t ipv4_send_len;
static uint32_t ipv4_send_dst;
static uint8_t ipv4_send_proto;
static int ipv4_send_called;

static int ipv4_send(struct NetIf *nif, uint32_t dst_ip, uint8_t protocol,
                     const void *payload, uint32_t payload_len) {
    (void)nif;
    ipv4_send_called++;
    ipv4_send_dst = dst_ip;
    ipv4_send_proto = protocol;
    ipv4_send_len = payload_len;
    if (payload_len <= sizeof(ipv4_send_buf))
        memcpy(ipv4_send_buf, payload, payload_len);
    return 0;
}

/* socket_udp_deliver capture */
static uint32_t deliver_src_ip, deliver_dst_ip;
static uint16_t deliver_src_port, deliver_dst_port;
static uint8_t deliver_data[2048];
static uint32_t deliver_data_len;
static int deliver_called;

static int socket_udp_deliver(uint32_t src_ip, uint16_t src_port,
                               uint32_t dst_ip, uint16_t dst_port,
                               const void *data, uint32_t len) {
    deliver_called++;
    deliver_src_ip = src_ip;
    deliver_src_port = src_port;
    deliver_dst_ip = dst_ip;
    deliver_dst_port = dst_port;
    deliver_data_len = len;
    if (len <= sizeof(deliver_data))
        memcpy(deliver_data, data, len);
    return 0;
}

static void kprintf(const char *fmt, ...) { (void)fmt; }

/* Include udp.c */
#include "../kernel/net/udp.c"

static void reset(void) {
    ipv4_send_called = 0; ipv4_send_len = 0;
    ipv4_send_dst = 0; ipv4_send_proto = 0;
    memset(ipv4_send_buf, 0, sizeof(ipv4_send_buf));
    deliver_called = 0; deliver_data_len = 0;
    deliver_src_ip = deliver_dst_ip = 0;
    deliver_src_port = deliver_dst_port = 0;
    memset(deliver_data, 0, sizeof(deliver_data));
}

/* --- Tests --- */

TEST(rx_short_packet_ignored) {
    reset();
    NetIf nif = {0};
    uint8_t data[4] = {0};
    udp_rx(&nif, IP4(10,0,2,2), IP4(10,0,2,15), data, 4);
    ASSERT_EQ(deliver_called, 0);
    return 0;
}

TEST(rx_valid_dispatches) {
    reset();
    NetIf nif = {0};
    uint8_t pkt[32];
    UdpHeader *hdr = (UdpHeader *)pkt;
    hdr->src_port = htons(5000);
    hdr->dst_port = htons(8080);
    hdr->length = htons(12);  /* 8 + 4 */
    hdr->checksum = 0;
    memcpy(pkt + 8, "test", 4);

    udp_rx(&nif, IP4(10,0,2,2), IP4(10,0,2,15), pkt, 12);
    ASSERT_EQ(deliver_called, 1);
    ASSERT_EQ(deliver_src_port, htons(5000));
    ASSERT_EQ(deliver_dst_port, htons(8080));
    ASSERT_EQ(deliver_data_len, (uint32_t)4);
    ASSERT_MEM_EQ(deliver_data, "test", 4);
    return 0;
}

TEST(rx_passes_both_ips) {
    reset();
    NetIf nif = {0};
    uint8_t pkt[8];
    UdpHeader *hdr = (UdpHeader *)pkt;
    hdr->src_port = htons(1234);
    hdr->dst_port = htons(5678);
    hdr->length = htons(8);
    hdr->checksum = 0;

    udp_rx(&nif, IP4(192,168,1,1), IP4(10,0,2,15), pkt, 8);
    ASSERT_EQ(deliver_src_ip, IP4(192,168,1,1));
    ASSERT_EQ(deliver_dst_ip, IP4(10,0,2,15));
    return 0;
}

TEST(rx_bad_length_ignored) {
    reset();
    NetIf nif = {0};
    uint8_t pkt[16];
    UdpHeader *hdr = (UdpHeader *)pkt;
    hdr->src_port = htons(1000);
    hdr->dst_port = htons(2000);
    hdr->length = htons(100);  /* claims 100 but only 16 bytes */
    hdr->checksum = 0;

    udp_rx(&nif, IP4(10,0,2,2), IP4(10,0,2,15), pkt, 16);
    ASSERT_EQ(deliver_called, 0);
    return 0;
}

TEST(rx_zero_payload) {
    reset();
    NetIf nif = {0};
    uint8_t pkt[8];
    UdpHeader *hdr = (UdpHeader *)pkt;
    hdr->src_port = htons(1111);
    hdr->dst_port = htons(2222);
    hdr->length = htons(8);  /* header only */
    hdr->checksum = 0;

    udp_rx(&nif, IP4(10,0,2,2), IP4(10,0,2,15), pkt, 8);
    ASSERT_EQ(deliver_called, 1);
    ASSERT_EQ(deliver_data_len, (uint32_t)0);
    return 0;
}

TEST(send_builds_valid_header) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10,0,2,15);

    uint8_t payload[] = "hello";
    udp_send(&nif, nif.ip_addr, IP4(10,0,2,2),
             htons(5000), htons(8080), payload, 5);

    ASSERT_EQ(ipv4_send_called, 1);
    ASSERT_EQ(ipv4_send_proto, IPV4_PROTO_UDP);
    ASSERT_EQ(ipv4_send_dst, IP4(10,0,2,2));
    ASSERT_EQ(ipv4_send_len, (uint32_t)(8 + 5));

    UdpHeader *hdr = (UdpHeader *)ipv4_send_buf;
    ASSERT_EQ(hdr->src_port, htons(5000));
    ASSERT_EQ(hdr->dst_port, htons(8080));
    ASSERT_EQ(ntohs(hdr->length), (uint16_t)13);
    return 0;
}

TEST(send_payload_appended) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10,0,2,15);
    uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    udp_send(&nif, nif.ip_addr, IP4(10,0,2,2),
             htons(1), htons(2), payload, 4);

    ASSERT_MEM_EQ(ipv4_send_buf + 8, payload, 4);
    return 0;
}

TEST(send_has_nonzero_checksum) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10,0,2,15);
    uint8_t payload[] = "data";
    udp_send(&nif, nif.ip_addr, IP4(10,0,2,2),
             htons(100), htons(200), payload, 4);

    UdpHeader *hdr = (UdpHeader *)ipv4_send_buf;
    ASSERT_TRUE(hdr->checksum != 0);
    return 0;
}

TEST(send_oversized_rejected) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10,0,2,15);
    uint8_t big[ETH_MTU];
    int ret = udp_send(&nif, nif.ip_addr, IP4(10,0,2,2),
                       htons(1), htons(2), big, ETH_MTU);
    ASSERT_EQ(ret, -1);
    ASSERT_EQ(ipv4_send_called, 0);
    return 0;
}

TEST(send_zero_payload) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10,0,2,15);
    udp_send(&nif, nif.ip_addr, IP4(10,0,2,2),
             htons(1), htons(2), NULL, 0);

    ASSERT_EQ(ipv4_send_called, 1);
    ASSERT_EQ(ipv4_send_len, (uint32_t)8);

    UdpHeader *hdr = (UdpHeader *)ipv4_send_buf;
    ASSERT_EQ(ntohs(hdr->length), (uint16_t)8);
    return 0;
}

TEST(rx_undersized_udp_length) {
    reset();
    NetIf nif = {0};
    uint8_t pkt[8];
    UdpHeader *hdr = (UdpHeader *)pkt;
    hdr->src_port = htons(1);
    hdr->dst_port = htons(2);
    hdr->length = htons(4);  /* < 8 = invalid */
    hdr->checksum = 0;

    udp_rx(&nif, IP4(10,0,2,2), IP4(10,0,2,15), pkt, 8);
    ASSERT_EQ(deliver_called, 0);
    return 0;
}

TEST(send_multiple_packets) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10,0,2,15);

    udp_send(&nif, nif.ip_addr, IP4(10,0,2,2), htons(1), htons(2), "a", 1);
    udp_send(&nif, nif.ip_addr, IP4(10,0,2,2), htons(1), htons(2), "b", 1);
    udp_send(&nif, nif.ip_addr, IP4(10,0,2,2), htons(1), htons(2), "c", 1);

    ASSERT_EQ(ipv4_send_called, 3);
    return 0;
}

/* --- Suite --- */

TestCase udp_tests[] = {
    TEST_ENTRY(rx_short_packet_ignored),
    TEST_ENTRY(rx_valid_dispatches),
    TEST_ENTRY(rx_passes_both_ips),
    TEST_ENTRY(rx_bad_length_ignored),
    TEST_ENTRY(rx_zero_payload),
    TEST_ENTRY(send_builds_valid_header),
    TEST_ENTRY(send_payload_appended),
    TEST_ENTRY(send_has_nonzero_checksum),
    TEST_ENTRY(send_oversized_rejected),
    TEST_ENTRY(send_zero_payload),
    TEST_ENTRY(rx_undersized_udp_length),
    TEST_ENTRY(send_multiple_packets),
};
int udp_test_count = sizeof(udp_tests) / sizeof(udp_tests[0]);
