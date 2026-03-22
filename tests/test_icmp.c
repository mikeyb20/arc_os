/* arc_os — Host-side tests for ICMP */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>

/* Guard kernel headers */
#define ARCHOS_NET_ICMP_H
#define ARCHOS_NET_NET_UTIL_H
#define ARCHOS_NET_IPV4_H
#define ARCHOS_NET_NETIF_H
#define ARCHOS_NET_ETHERNET_H
#define ARCHOS_LIB_MEM_H
#define ARCHOS_LIB_KPRINTF_H

/* Inline types */
#define ETH_ALEN        6
#define ETH_MTU         1500
#define IPV4_PROTO_ICMP 1
#define ICMP_TYPE_ECHO_REPLY    0
#define ICMP_TYPE_ECHO_REQUEST  8
#define ICMP_HEADER_SIZE        8

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
        p += 2;
        len -= 2;
    }
    if (len == 1)
        sum += (uint32_t)p[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t sequence;
} __attribute__((packed)) IcmpHeader;

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

static void kprintf(const char *fmt, ...) { (void)fmt; }

/* Include icmp.c */
#include "../kernel/net/icmp.c"

/* Build an ICMP echo request with valid checksum */
static void build_echo_request(uint8_t *buf, uint16_t id, uint16_t seq,
                                const uint8_t *payload, uint32_t plen,
                                uint32_t *total_len) {
    IcmpHeader *hdr = (IcmpHeader *)buf;
    hdr->type = ICMP_TYPE_ECHO_REQUEST;
    hdr->code = 0;
    hdr->checksum = 0;
    hdr->id = htons(id);
    hdr->sequence = htons(seq);
    if (plen > 0)
        memcpy(buf + ICMP_HEADER_SIZE, payload, plen);
    *total_len = ICMP_HEADER_SIZE + plen;
    hdr->checksum = htons(net_checksum(buf, *total_len));
}

static void reset(void) {
    ipv4_send_called = 0;
    ipv4_send_len = 0;
    ipv4_send_dst = 0;
    ipv4_send_proto = 0;
    memset(ipv4_send_buf, 0, sizeof(ipv4_send_buf));
}

/* --- Tests --- */

TEST(rx_short_packet_ignored) {
    reset();
    NetIf nif = {0};
    uint8_t data[4] = {0};
    icmp_rx(&nif, IP4(10, 0, 2, 2), data, 4);
    ASSERT_EQ(ipv4_send_called, 0);
    return 0;
}

TEST(rx_bad_checksum_ignored) {
    reset();
    NetIf nif = {0};
    uint8_t pkt[ICMP_HEADER_SIZE];
    uint32_t len;
    build_echo_request(pkt, 1, 1, NULL, 0, &len);
    /* Corrupt checksum */
    pkt[2] ^= 0xFF;
    icmp_rx(&nif, IP4(10, 0, 2, 2), pkt, len);
    ASSERT_EQ(ipv4_send_called, 0);
    return 0;
}

TEST(rx_echo_request_sends_reply) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    uint8_t pkt[64];
    uint32_t len;
    build_echo_request(pkt, 0x1234, 0x0001, NULL, 0, &len);
    icmp_rx(&nif, IP4(10, 0, 2, 2), pkt, len);

    ASSERT_EQ(ipv4_send_called, 1);
    ASSERT_EQ(ipv4_send_dst, IP4(10, 0, 2, 2));
    ASSERT_EQ(ipv4_send_proto, IPV4_PROTO_ICMP);
    ASSERT_EQ(ipv4_send_len, ICMP_HEADER_SIZE);

    IcmpHeader *reply = (IcmpHeader *)ipv4_send_buf;
    ASSERT_EQ(reply->type, ICMP_TYPE_ECHO_REPLY);
    ASSERT_EQ(reply->code, 0);
    ASSERT_EQ(reply->id, htons(0x1234));
    ASSERT_EQ(reply->sequence, htons(0x0001));
    return 0;
}

TEST(reply_preserves_payload) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    uint8_t payload[8] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0xBA, 0xBE};
    uint8_t pkt[64];
    uint32_t len;
    build_echo_request(pkt, 42, 7, payload, 8, &len);
    icmp_rx(&nif, IP4(10, 0, 2, 2), pkt, len);

    ASSERT_EQ(ipv4_send_called, 1);
    ASSERT_EQ(ipv4_send_len, ICMP_HEADER_SIZE + 8);
    ASSERT_MEM_EQ(ipv4_send_buf + ICMP_HEADER_SIZE, payload, 8);
    return 0;
}

TEST(reply_has_valid_checksum) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    uint8_t payload[4] = {0x01, 0x02, 0x03, 0x04};
    uint8_t pkt[64];
    uint32_t len;
    build_echo_request(pkt, 100, 200, payload, 4, &len);
    icmp_rx(&nif, IP4(10, 0, 2, 2), pkt, len);

    /* Verify reply checksum validates */
    ASSERT_EQ(net_checksum(ipv4_send_buf, ipv4_send_len), 0);
    return 0;
}

TEST(reply_preserves_id_and_seq) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    uint8_t pkt[64];
    uint32_t len;
    build_echo_request(pkt, 0xABCD, 0x1234, NULL, 0, &len);
    icmp_rx(&nif, IP4(10, 0, 2, 2), pkt, len);

    IcmpHeader *reply = (IcmpHeader *)ipv4_send_buf;
    ASSERT_EQ(ntohs(reply->id), 0xABCD);
    ASSERT_EQ(ntohs(reply->sequence), 0x1234);
    return 0;
}

TEST(rx_echo_reply_ignored) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    /* Build an echo reply (type 0) */
    uint8_t pkt[ICMP_HEADER_SIZE];
    IcmpHeader *hdr = (IcmpHeader *)pkt;
    hdr->type = ICMP_TYPE_ECHO_REPLY;
    hdr->code = 0;
    hdr->checksum = 0;
    hdr->id = htons(1);
    hdr->sequence = htons(1);
    hdr->checksum = htons(net_checksum(pkt, ICMP_HEADER_SIZE));
    icmp_rx(&nif, IP4(10, 0, 2, 2), pkt, ICMP_HEADER_SIZE);
    ASSERT_EQ(ipv4_send_called, 0);
    return 0;
}

TEST(rx_nonzero_code_ignored) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    uint8_t pkt[ICMP_HEADER_SIZE];
    IcmpHeader *hdr = (IcmpHeader *)pkt;
    hdr->type = ICMP_TYPE_ECHO_REQUEST;
    hdr->code = 1;  /* non-zero code */
    hdr->checksum = 0;
    hdr->id = htons(1);
    hdr->sequence = htons(1);
    hdr->checksum = htons(net_checksum(pkt, ICMP_HEADER_SIZE));
    icmp_rx(&nif, IP4(10, 0, 2, 2), pkt, ICMP_HEADER_SIZE);
    ASSERT_EQ(ipv4_send_called, 0);
    return 0;
}

TEST(reply_sent_to_sender_ip) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    uint8_t pkt[64];
    uint32_t len;
    build_echo_request(pkt, 1, 1, NULL, 0, &len);
    uint32_t sender = IP4(192, 168, 1, 100);
    icmp_rx(&nif, sender, pkt, len);
    ASSERT_EQ(ipv4_send_dst, sender);
    return 0;
}

TEST(multiple_requests_multiple_replies) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    uint8_t pkt[64];
    uint32_t len;

    build_echo_request(pkt, 1, 1, NULL, 0, &len);
    icmp_rx(&nif, IP4(10, 0, 2, 2), pkt, len);
    build_echo_request(pkt, 1, 2, NULL, 0, &len);
    icmp_rx(&nif, IP4(10, 0, 2, 2), pkt, len);
    build_echo_request(pkt, 1, 3, NULL, 0, &len);
    icmp_rx(&nif, IP4(10, 0, 2, 2), pkt, len);

    ASSERT_EQ(ipv4_send_called, 3);
    return 0;
}

TEST(large_payload_preserved) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    uint8_t payload[64];
    for (int i = 0; i < 64; i++) payload[i] = (uint8_t)i;
    uint8_t pkt[128];
    uint32_t len;
    build_echo_request(pkt, 1, 1, payload, 64, &len);
    icmp_rx(&nif, IP4(10, 0, 2, 2), pkt, len);

    ASSERT_EQ(ipv4_send_len, ICMP_HEADER_SIZE + 64);
    ASSERT_MEM_EQ(ipv4_send_buf + ICMP_HEADER_SIZE, payload, 64);
    return 0;
}

TEST(request_checksum_validates) {
    /* Verify our build helper produces valid checksums */
    uint8_t pkt[64];
    uint32_t len;
    uint8_t payload[4] = {0x01, 0x02, 0x03, 0x04};
    build_echo_request(pkt, 1, 1, payload, 4, &len);
    ASSERT_EQ(net_checksum(pkt, len), 0);
    return 0;
}

/* --- Suite --- */

TestCase icmp_tests[] = {
    TEST_ENTRY(rx_short_packet_ignored),
    TEST_ENTRY(rx_bad_checksum_ignored),
    TEST_ENTRY(rx_echo_request_sends_reply),
    TEST_ENTRY(reply_preserves_payload),
    TEST_ENTRY(reply_has_valid_checksum),
    TEST_ENTRY(reply_preserves_id_and_seq),
    TEST_ENTRY(rx_echo_reply_ignored),
    TEST_ENTRY(rx_nonzero_code_ignored),
    TEST_ENTRY(reply_sent_to_sender_ip),
    TEST_ENTRY(multiple_requests_multiple_replies),
    TEST_ENTRY(large_payload_preserved),
    TEST_ENTRY(request_checksum_validates),
};
int icmp_test_count = sizeof(icmp_tests) / sizeof(icmp_tests[0]);
