/* arc_os — Host-side tests for IPv4 */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>

/* Guard kernel headers */
#define ARCHOS_NET_IPV4_H
#define ARCHOS_NET_NET_UTIL_H
#define ARCHOS_NET_ETHERNET_H
#define ARCHOS_NET_NETIF_H
#define ARCHOS_NET_ARP_H
#define ARCHOS_NET_ICMP_H
#define ARCHOS_LIB_MEM_H
#define ARCHOS_LIB_KPRINTF_H

/* Inline types */
#define ETH_ALEN        6
#define ETH_MTU         1500
#define ETH_TYPE_IPV4   0x0800
#define IPV4_HEADER_SIZE 20
#define IPV4_PROTO_ICMP  1
#define IPV4_PROTO_UDP   17
#define IPV4_DEFAULT_TTL 64

static inline uint16_t htons(uint16_t h) {
    return (uint16_t)((h >> 8) | (h << 8));
}
static inline uint16_t ntohs(uint16_t n) { return htons(n); }
static inline uint32_t htonl(uint32_t h) {
    return ((h >> 24) & 0xFF) | ((h >> 8) & 0xFF00) |
           ((h << 8) & 0xFF0000) | ((h << 24) & 0xFF000000u);
}

static inline uint32_t IP4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
}

/* Include net_checksum (static to avoid linker clash with test_net_util) */
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
    uint8_t  ver_ihl;
    uint8_t  tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t flags_frag;
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint32_t src_ip;
    uint32_t dst_ip;
} __attribute__((packed)) Ipv4Header;

typedef struct NetIf {
    uint8_t  mac[ETH_ALEN];
    uint32_t ip_addr;
    uint32_t netmask;
    uint32_t gateway;
    int (*send)(struct NetIf *nif, const void *frame, uint32_t len);
    void *private_data;
} NetIf;

/* ICMP stub */
static int icmp_rx_called;
static uint32_t icmp_rx_src_ip;
static uint32_t icmp_rx_len;

static void icmp_rx(struct NetIf *nif, uint32_t src_ip, const void *data, uint32_t len) {
    (void)nif; (void)data;
    icmp_rx_called++;
    icmp_rx_src_ip = src_ip;
    icmp_rx_len = len;
}

/* UDP stub */
static int udp_rx_called;
static uint32_t udp_rx_src_ip, udp_rx_dst_ip;

static void udp_rx(struct NetIf *nif, uint32_t src_ip, uint32_t dst_ip,
                   const void *data, uint32_t len) {
    (void)nif; (void)data; (void)len;
    udp_rx_called++;
    udp_rx_src_ip = src_ip;
    udp_rx_dst_ip = dst_ip;
}

/* ARP stub */
static const uint8_t *arp_lookup_result;
static const uint8_t *arp_lookup(uint32_t ip) {
    (void)ip;
    return arp_lookup_result;
}

/* eth_send capture */
static uint8_t eth_send_buf[2048];
static uint32_t eth_send_len;
static int eth_send_called;

static int eth_send(struct NetIf *nif, const uint8_t dst[ETH_ALEN],
                    uint16_t ethertype, const void *payload, uint32_t payload_len) {
    (void)nif; (void)dst; (void)ethertype;
    eth_send_called++;
    eth_send_len = payload_len;
    if (payload_len <= sizeof(eth_send_buf))
        memcpy(eth_send_buf, payload, payload_len);
    return 0;
}

static void kprintf(const char *fmt, ...) { (void)fmt; }

/* Include ipv4.c */
#include "../kernel/net/ipv4.c"

/* Build a valid IPv4 packet with correct checksum */
static void build_ipv4(uint8_t *buf, uint32_t src, uint32_t dst,
                        uint8_t proto, const uint8_t *payload, uint32_t plen) {
    Ipv4Header *hdr = (Ipv4Header *)buf;
    hdr->ver_ihl = 0x45;
    hdr->tos = 0;
    hdr->total_len = htons((uint16_t)(IPV4_HEADER_SIZE + plen));
    hdr->id = htons(1);
    hdr->flags_frag = 0;
    hdr->ttl = 64;
    hdr->protocol = proto;
    hdr->checksum = 0;
    hdr->src_ip = src;
    hdr->dst_ip = dst;
    hdr->checksum = htons(net_checksum(hdr, IPV4_HEADER_SIZE));
    if (plen > 0)
        memcpy(buf + IPV4_HEADER_SIZE, payload, plen);
}

static void reset(void) {
    icmp_rx_called = 0;
    icmp_rx_src_ip = 0;
    icmp_rx_len = 0;
    udp_rx_called = 0;
    udp_rx_src_ip = 0;
    udp_rx_dst_ip = 0;
    eth_send_called = 0;
    eth_send_len = 0;
    arp_lookup_result = NULL;
    ip_id_counter = 1;
    memset(eth_send_buf, 0, sizeof(eth_send_buf));
}

/* --- Tests --- */

TEST(rx_short_packet_ignored) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    nif.netmask = IP4(255, 255, 255, 0);
    uint8_t data[10] = {0};
    ipv4_rx(&nif, data, 10);
    ASSERT_EQ(icmp_rx_called, 0);
    return 0;
}

TEST(rx_bad_version_ignored) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    nif.netmask = IP4(255, 255, 255, 0);
    uint8_t pkt[40] = {0};
    pkt[0] = 0x65;  /* version 6, IHL 5 */
    ipv4_rx(&nif, pkt, 40);
    ASSERT_EQ(icmp_rx_called, 0);
    return 0;
}

TEST(rx_bad_ihl_ignored) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    nif.netmask = IP4(255, 255, 255, 0);
    uint8_t pkt[40] = {0};
    pkt[0] = 0x43;  /* version 4, IHL 3 (too small) */
    ipv4_rx(&nif, pkt, 40);
    ASSERT_EQ(icmp_rx_called, 0);
    return 0;
}

TEST(rx_bad_checksum_ignored) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    nif.netmask = IP4(255, 255, 255, 0);
    uint8_t pkt[40];
    build_ipv4(pkt, IP4(10, 0, 2, 2), nif.ip_addr, IPV4_PROTO_ICMP, NULL, 0);
    /* Corrupt checksum */
    pkt[10] ^= 0xFF;
    ipv4_rx(&nif, pkt, IPV4_HEADER_SIZE);
    ASSERT_EQ(icmp_rx_called, 0);
    return 0;
}

TEST(rx_wrong_dst_ignored) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    nif.netmask = IP4(255, 255, 255, 0);
    uint8_t pkt[40];
    build_ipv4(pkt, IP4(10, 0, 2, 2), IP4(10, 0, 2, 99), IPV4_PROTO_ICMP, NULL, 0);
    ipv4_rx(&nif, pkt, IPV4_HEADER_SIZE);
    ASSERT_EQ(icmp_rx_called, 0);
    return 0;
}

TEST(rx_icmp_dispatched) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    nif.netmask = IP4(255, 255, 255, 0);
    uint8_t payload[8] = {0x08, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01};
    uint8_t pkt[IPV4_HEADER_SIZE + 8];
    build_ipv4(pkt, IP4(10, 0, 2, 2), nif.ip_addr, IPV4_PROTO_ICMP, payload, 8);
    ipv4_rx(&nif, pkt, IPV4_HEADER_SIZE + 8);
    ASSERT_EQ(icmp_rx_called, 1);
    ASSERT_EQ(icmp_rx_src_ip, IP4(10, 0, 2, 2));
    ASSERT_EQ(icmp_rx_len, 8);
    return 0;
}

TEST(rx_broadcast_accepted) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    nif.netmask = IP4(255, 255, 255, 0);
    uint8_t pkt[IPV4_HEADER_SIZE];
    build_ipv4(pkt, IP4(10, 0, 2, 2), 0xFFFFFFFF, IPV4_PROTO_ICMP, NULL, 0);
    ipv4_rx(&nif, pkt, IPV4_HEADER_SIZE);
    ASSERT_EQ(icmp_rx_called, 1);
    return 0;
}

TEST(rx_subnet_broadcast_accepted) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    nif.netmask = IP4(255, 255, 255, 0);
    /* Subnet broadcast: 10.0.2.255 */
    uint8_t pkt[IPV4_HEADER_SIZE];
    build_ipv4(pkt, IP4(10, 0, 2, 2), IP4(10, 0, 2, 255), IPV4_PROTO_ICMP, NULL, 0);
    ipv4_rx(&nif, pkt, IPV4_HEADER_SIZE);
    ASSERT_EQ(icmp_rx_called, 1);
    return 0;
}

TEST(rx_unknown_protocol_ignored) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    nif.netmask = IP4(255, 255, 255, 0);
    uint8_t pkt[IPV4_HEADER_SIZE];
    build_ipv4(pkt, IP4(10, 0, 2, 2), nif.ip_addr, 99 /* unknown */, NULL, 0);
    ipv4_rx(&nif, pkt, IPV4_HEADER_SIZE);
    ASSERT_EQ(icmp_rx_called, 0);
    ASSERT_EQ(udp_rx_called, 0);
    return 0;
}

TEST(rx_udp_dispatched) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    nif.netmask = IP4(255, 255, 255, 0);
    uint8_t payload[8] = {0};
    uint8_t pkt[IPV4_HEADER_SIZE + 8];
    build_ipv4(pkt, IP4(10, 0, 2, 2), nif.ip_addr, IPV4_PROTO_UDP, payload, 8);
    ipv4_rx(&nif, pkt, IPV4_HEADER_SIZE + 8);
    ASSERT_EQ(udp_rx_called, 1);
    ASSERT_EQ(udp_rx_src_ip, IP4(10, 0, 2, 2));
    ASSERT_EQ(udp_rx_dst_ip, nif.ip_addr);
    ASSERT_EQ(icmp_rx_called, 0);
    return 0;
}

TEST(send_builds_valid_header) {
    reset();
    uint8_t gw_mac[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x02};
    arp_lookup_result = gw_mac;

    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    nif.netmask = IP4(255, 255, 255, 0);
    nif.gateway = IP4(10, 0, 2, 2);
    nif.send = NULL;

    uint8_t payload[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    int ret = ipv4_send(&nif, IP4(10, 0, 2, 2), IPV4_PROTO_ICMP, payload, 4);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(eth_send_called, 1);

    Ipv4Header *hdr = (Ipv4Header *)eth_send_buf;
    ASSERT_EQ(hdr->ver_ihl, 0x45);
    ASSERT_EQ(hdr->ttl, IPV4_DEFAULT_TTL);
    ASSERT_EQ(hdr->protocol, IPV4_PROTO_ICMP);
    ASSERT_EQ(hdr->src_ip, IP4(10, 0, 2, 15));
    ASSERT_EQ(hdr->dst_ip, IP4(10, 0, 2, 2));
    /* Verify checksum is valid */
    ASSERT_EQ(net_checksum(hdr, IPV4_HEADER_SIZE), 0);
    return 0;
}

TEST(send_no_arp_drops) {
    reset();
    arp_lookup_result = NULL;

    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    nif.netmask = IP4(255, 255, 255, 0);
    nif.gateway = IP4(10, 0, 2, 2);

    int ret = ipv4_send(&nif, IP4(10, 0, 2, 2), IPV4_PROTO_ICMP, NULL, 0);
    ASSERT_EQ(ret, -1);
    ASSERT_EQ(eth_send_called, 0);
    return 0;
}

TEST(send_oversized_rejected) {
    reset();
    uint8_t gw_mac[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x02};
    arp_lookup_result = gw_mac;

    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    nif.netmask = IP4(255, 255, 255, 0);

    uint8_t big[ETH_MTU];
    int ret = ipv4_send(&nif, IP4(10, 0, 2, 2), IPV4_PROTO_ICMP, big, ETH_MTU);
    ASSERT_EQ(ret, -1);
    return 0;
}

TEST(send_increments_id) {
    reset();
    uint8_t gw_mac[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x02};
    arp_lookup_result = gw_mac;

    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    nif.netmask = IP4(255, 255, 255, 0);
    nif.gateway = IP4(10, 0, 2, 2);

    ipv4_send(&nif, IP4(10, 0, 2, 2), IPV4_PROTO_ICMP, NULL, 0);
    Ipv4Header *hdr1 = (Ipv4Header *)eth_send_buf;
    uint16_t id1 = ntohs(hdr1->id);

    ipv4_send(&nif, IP4(10, 0, 2, 2), IPV4_PROTO_ICMP, NULL, 0);
    Ipv4Header *hdr2 = (Ipv4Header *)eth_send_buf;
    uint16_t id2 = ntohs(hdr2->id);

    ASSERT_EQ(id2, id1 + 1);
    return 0;
}

TEST(send_payload_appended) {
    reset();
    uint8_t gw_mac[6] = {0x52, 0x55, 0x0A, 0x00, 0x02, 0x02};
    arp_lookup_result = gw_mac;

    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    nif.netmask = IP4(255, 255, 255, 0);
    nif.gateway = IP4(10, 0, 2, 2);

    uint8_t payload[4] = {0xCA, 0xFE, 0xBA, 0xBE};
    ipv4_send(&nif, IP4(10, 0, 2, 2), IPV4_PROTO_ICMP, payload, 4);
    ASSERT_EQ(eth_send_len, IPV4_HEADER_SIZE + 4);
    ASSERT_MEM_EQ(eth_send_buf + IPV4_HEADER_SIZE, payload, 4);
    return 0;
}

/* --- Suite --- */

TestCase ipv4_tests[] = {
    TEST_ENTRY(rx_short_packet_ignored),
    TEST_ENTRY(rx_bad_version_ignored),
    TEST_ENTRY(rx_bad_ihl_ignored),
    TEST_ENTRY(rx_bad_checksum_ignored),
    TEST_ENTRY(rx_wrong_dst_ignored),
    TEST_ENTRY(rx_icmp_dispatched),
    TEST_ENTRY(rx_broadcast_accepted),
    TEST_ENTRY(rx_subnet_broadcast_accepted),
    TEST_ENTRY(rx_unknown_protocol_ignored),
    TEST_ENTRY(send_builds_valid_header),
    TEST_ENTRY(send_no_arp_drops),
    TEST_ENTRY(send_oversized_rejected),
    TEST_ENTRY(send_increments_id),
    TEST_ENTRY(send_payload_appended),
    TEST_ENTRY(rx_udp_dispatched),
};
int ipv4_test_count = sizeof(ipv4_tests) / sizeof(ipv4_tests[0]);
