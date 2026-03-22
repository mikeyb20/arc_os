/* arc_os — Host-side tests for Ethernet framing */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>

/* Guard kernel headers */
#define ARCHOS_NET_ETHERNET_H
#define ARCHOS_NET_NET_UTIL_H
#define ARCHOS_NET_NETIF_H
#define ARCHOS_NET_ARP_H
#define ARCHOS_NET_IPV4_H
#define ARCHOS_LIB_MEM_H
#define ARCHOS_LIB_KPRINTF_H

/* Inline types and helpers */
#define ETH_ALEN        6
#define ETH_HEADER_SIZE 14
#define ETH_MTU         1500
#define ETH_FRAME_MAX   (ETH_HEADER_SIZE + ETH_MTU)
#define ETH_TYPE_ARP    0x0806
#define ETH_TYPE_IPV4   0x0800

static inline uint16_t htons(uint16_t h) {
    return (uint16_t)((h >> 8) | (h << 8));
}
static inline uint16_t ntohs(uint16_t n) { return htons(n); }

typedef struct {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t ethertype;
} __attribute__((packed)) EthHeader;

typedef struct NetIf {
    uint8_t  mac[ETH_ALEN];
    uint32_t ip_addr;
    uint32_t netmask;
    uint32_t gateway;
    int (*send)(struct NetIf *nif, const void *frame, uint32_t len);
    void *private_data;
} NetIf;

/* Tracking stubs */
static int arp_rx_called;
static const void *arp_rx_data;
static uint32_t arp_rx_len;

static int ipv4_rx_called;
static const void *ipv4_rx_data;
static uint32_t ipv4_rx_len;

static void arp_rx(struct NetIf *nif, const void *data, uint32_t len) {
    (void)nif;
    arp_rx_called++;
    arp_rx_data = data;
    arp_rx_len = len;
}

static void ipv4_rx(struct NetIf *nif, const void *data, uint32_t len) {
    (void)nif;
    ipv4_rx_called++;
    ipv4_rx_data = data;
    ipv4_rx_len = len;
}

/* Send capture */
static uint8_t send_capture[2048];
static uint32_t send_capture_len;
static int send_called;

static int mock_send(NetIf *nif, const void *frame, uint32_t len) {
    (void)nif;
    send_called++;
    send_capture_len = len;
    if (len <= sizeof(send_capture))
        memcpy(send_capture, frame, len);
    return 0;
}

static void kprintf(const char *fmt, ...) { (void)fmt; }

/* Include ethernet.c */
#include "../kernel/net/ethernet.c"

static void reset(void) {
    arp_rx_called = 0;
    ipv4_rx_called = 0;
    send_called = 0;
    send_capture_len = 0;
    memset(send_capture, 0, sizeof(send_capture));
}

/* --- Tests --- */

TEST(rx_short_frame_ignored) {
    reset();
    NetIf nif = {0};
    uint8_t data[10] = {0};
    eth_rx(&nif, data, 10);
    ASSERT_EQ(arp_rx_called, 0);
    ASSERT_EQ(ipv4_rx_called, 0);
    return 0;
}

TEST(rx_arp_dispatch) {
    reset();
    NetIf nif = {0};
    uint8_t frame[64] = {0};
    EthHeader *hdr = (EthHeader *)frame;
    hdr->ethertype = htons(ETH_TYPE_ARP);
    eth_rx(&nif, frame, 64);
    ASSERT_EQ(arp_rx_called, 1);
    ASSERT_EQ(ipv4_rx_called, 0);
    ASSERT_EQ(arp_rx_len, 64 - ETH_HEADER_SIZE);
    return 0;
}

TEST(rx_ipv4_dispatch) {
    reset();
    NetIf nif = {0};
    uint8_t frame[64] = {0};
    EthHeader *hdr = (EthHeader *)frame;
    hdr->ethertype = htons(ETH_TYPE_IPV4);
    eth_rx(&nif, frame, 64);
    ASSERT_EQ(ipv4_rx_called, 1);
    ASSERT_EQ(arp_rx_called, 0);
    ASSERT_EQ(ipv4_rx_len, 64 - ETH_HEADER_SIZE);
    return 0;
}

TEST(rx_unknown_ethertype_ignored) {
    reset();
    NetIf nif = {0};
    uint8_t frame[64] = {0};
    EthHeader *hdr = (EthHeader *)frame;
    hdr->ethertype = htons(0x9999);
    eth_rx(&nif, frame, 64);
    ASSERT_EQ(arp_rx_called, 0);
    ASSERT_EQ(ipv4_rx_called, 0);
    return 0;
}

TEST(rx_exact_header_size) {
    reset();
    NetIf nif = {0};
    uint8_t frame[ETH_HEADER_SIZE] = {0};
    EthHeader *hdr = (EthHeader *)frame;
    hdr->ethertype = htons(ETH_TYPE_ARP);
    eth_rx(&nif, frame, ETH_HEADER_SIZE);
    ASSERT_EQ(arp_rx_called, 1);
    ASSERT_EQ(arp_rx_len, 0);
    return 0;
}

TEST(send_builds_header) {
    reset();
    NetIf nif = {0};
    uint8_t mac_src[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    uint8_t mac_dst[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    memcpy(nif.mac, mac_src, 6);
    nif.send = mock_send;

    uint8_t payload[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    int ret = eth_send(&nif, mac_dst, ETH_TYPE_IPV4, payload, 4);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(send_called, 1);
    ASSERT_EQ(send_capture_len, ETH_HEADER_SIZE + 4);

    EthHeader *hdr = (EthHeader *)send_capture;
    ASSERT_MEM_EQ(hdr->dst, mac_dst, 6);
    ASSERT_MEM_EQ(hdr->src, mac_src, 6);
    ASSERT_EQ(ntohs(hdr->ethertype), ETH_TYPE_IPV4);
    ASSERT_MEM_EQ(send_capture + ETH_HEADER_SIZE, payload, 4);
    return 0;
}

TEST(send_empty_payload) {
    reset();
    NetIf nif = {0};
    nif.send = mock_send;
    uint8_t mac_dst[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

    int ret = eth_send(&nif, mac_dst, ETH_TYPE_ARP, NULL, 0);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(send_capture_len, ETH_HEADER_SIZE);
    return 0;
}

TEST(send_rejects_oversized) {
    reset();
    NetIf nif = {0};
    nif.send = mock_send;
    uint8_t mac_dst[6] = {0};
    uint8_t big[ETH_MTU + 1];
    int ret = eth_send(&nif, mac_dst, ETH_TYPE_IPV4, big, ETH_MTU + 1);
    ASSERT_EQ(ret, -1);
    ASSERT_EQ(send_called, 0);
    return 0;
}

TEST(send_max_payload) {
    reset();
    NetIf nif = {0};
    nif.send = mock_send;
    uint8_t mac_dst[6] = {0};
    uint8_t payload[ETH_MTU];
    memset(payload, 0x42, ETH_MTU);
    int ret = eth_send(&nif, mac_dst, ETH_TYPE_IPV4, payload, ETH_MTU);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(send_capture_len, ETH_FRAME_MAX);
    return 0;
}

TEST(rx_payload_pointer_correct) {
    reset();
    NetIf nif = {0};
    uint8_t frame[ETH_HEADER_SIZE + 4];
    EthHeader *hdr = (EthHeader *)frame;
    hdr->ethertype = htons(ETH_TYPE_IPV4);
    frame[ETH_HEADER_SIZE] = 0xAB;
    frame[ETH_HEADER_SIZE + 1] = 0xCD;
    eth_rx(&nif, frame, sizeof(frame));
    ASSERT_EQ(ipv4_rx_called, 1);
    /* Verify payload starts after header */
    const uint8_t *p = (const uint8_t *)ipv4_rx_data;
    ASSERT_EQ(p[0], 0xAB);
    ASSERT_EQ(p[1], 0xCD);
    return 0;
}

TEST(send_ethertype_network_order) {
    reset();
    NetIf nif = {0};
    nif.send = mock_send;
    uint8_t mac_dst[6] = {0};
    eth_send(&nif, mac_dst, ETH_TYPE_ARP, NULL, 0);
    EthHeader *hdr = (EthHeader *)send_capture;
    /* ethertype stored in network byte order */
    ASSERT_EQ(hdr->ethertype, htons(ETH_TYPE_ARP));
    return 0;
}

TEST(rx_multiple_dispatches) {
    reset();
    NetIf nif = {0};
    uint8_t frame[64] = {0};
    EthHeader *hdr = (EthHeader *)frame;

    hdr->ethertype = htons(ETH_TYPE_ARP);
    eth_rx(&nif, frame, 64);
    hdr->ethertype = htons(ETH_TYPE_IPV4);
    eth_rx(&nif, frame, 64);
    hdr->ethertype = htons(ETH_TYPE_ARP);
    eth_rx(&nif, frame, 64);

    ASSERT_EQ(arp_rx_called, 2);
    ASSERT_EQ(ipv4_rx_called, 1);
    return 0;
}

/* --- Suite --- */

TestCase ethernet_tests[] = {
    TEST_ENTRY(rx_short_frame_ignored),
    TEST_ENTRY(rx_arp_dispatch),
    TEST_ENTRY(rx_ipv4_dispatch),
    TEST_ENTRY(rx_unknown_ethertype_ignored),
    TEST_ENTRY(rx_exact_header_size),
    TEST_ENTRY(send_builds_header),
    TEST_ENTRY(send_empty_payload),
    TEST_ENTRY(send_rejects_oversized),
    TEST_ENTRY(send_max_payload),
    TEST_ENTRY(rx_payload_pointer_correct),
    TEST_ENTRY(send_ethertype_network_order),
    TEST_ENTRY(rx_multiple_dispatches),
};
int ethernet_test_count = sizeof(ethernet_tests) / sizeof(ethernet_tests[0]);
