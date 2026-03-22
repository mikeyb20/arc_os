/* arc_os — Host-side tests for ARP */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>

/* Guard kernel headers */
#define ARCHOS_NET_ARP_H
#define ARCHOS_NET_NET_UTIL_H
#define ARCHOS_NET_ETHERNET_H
#define ARCHOS_NET_NETIF_H
#define ARCHOS_LIB_MEM_H
#define ARCHOS_LIB_KPRINTF_H

/* Inline types */
#define ETH_ALEN        6
#define ETH_HEADER_SIZE 14
#define ETH_MTU         1500
#define ETH_FRAME_MAX   (ETH_HEADER_SIZE + ETH_MTU)
#define ETH_TYPE_ARP    0x0806

#define ARP_HTYPE_ETHER  1
#define ARP_PTYPE_IPV4   0x0800
#define ARP_OP_REQUEST   1
#define ARP_OP_REPLY     2
#define ARP_PACKET_SIZE  28
#define ARP_CACHE_SIZE   16

static inline uint16_t htons(uint16_t h) {
    return (uint16_t)((h >> 8) | (h << 8));
}
static inline uint16_t ntohs(uint16_t n) { return htons(n); }

static inline uint32_t IP4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
}

typedef struct {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;
    uint8_t  sha[ETH_ALEN];
    uint32_t spa;
    uint8_t  tha[ETH_ALEN];
    uint32_t tpa;
} __attribute__((packed)) ArpPacket;

typedef struct NetIf {
    uint8_t  mac[ETH_ALEN];
    uint32_t ip_addr;
    uint32_t netmask;
    uint32_t gateway;
    int (*send)(struct NetIf *nif, const void *frame, uint32_t len);
    void *private_data;
} NetIf;

/* eth_send capture */
static uint8_t eth_send_capture[2048];
static uint32_t eth_send_len;
static int eth_send_called;
static uint16_t eth_send_ethertype;

static int eth_send(struct NetIf *nif, const uint8_t dst[ETH_ALEN],
                    uint16_t ethertype, const void *payload, uint32_t payload_len) {
    (void)nif; (void)dst;
    eth_send_called++;
    eth_send_ethertype = ethertype;
    eth_send_len = payload_len;
    if (payload_len <= sizeof(eth_send_capture))
        memcpy(eth_send_capture, payload, payload_len);
    return 0;
}

static void kprintf(const char *fmt, ...) { (void)fmt; }

/* Include arp.c */
#include "../kernel/net/arp.c"

static void reset(void) {
    arp_init();
    eth_send_called = 0;
    eth_send_len = 0;
    eth_send_ethertype = 0;
    memset(eth_send_capture, 0, sizeof(eth_send_capture));
}

/* Build an ARP request packet */
static void build_arp_request(ArpPacket *pkt, uint32_t sender_ip,
                               const uint8_t sender_mac[6], uint32_t target_ip) {
    pkt->htype = htons(ARP_HTYPE_ETHER);
    pkt->ptype = htons(ARP_PTYPE_IPV4);
    pkt->hlen = ETH_ALEN;
    pkt->plen = 4;
    pkt->oper = htons(ARP_OP_REQUEST);
    memcpy(pkt->sha, sender_mac, 6);
    pkt->spa = sender_ip;
    memset(pkt->tha, 0, 6);
    pkt->tpa = target_ip;
}

/* --- Tests --- */

TEST(init_clears_cache) {
    reset();
    ASSERT_TRUE(arp_lookup(IP4(10, 0, 0, 1)) == NULL);
    return 0;
}

TEST(insert_and_lookup) {
    reset();
    uint8_t mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    uint32_t ip = IP4(10, 0, 2, 2);
    arp_insert(ip, mac);
    const uint8_t *found = arp_lookup(ip);
    ASSERT_TRUE(found != NULL);
    ASSERT_MEM_EQ(found, mac, 6);
    return 0;
}

TEST(lookup_miss) {
    reset();
    ASSERT_TRUE(arp_lookup(IP4(1, 2, 3, 4)) == NULL);
    return 0;
}

TEST(insert_updates_existing) {
    reset();
    uint32_t ip = IP4(10, 0, 2, 2);
    uint8_t mac1[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint8_t mac2[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    arp_insert(ip, mac1);
    arp_insert(ip, mac2);
    const uint8_t *found = arp_lookup(ip);
    ASSERT_TRUE(found != NULL);
    ASSERT_MEM_EQ(found, mac2, 6);
    return 0;
}

TEST(cache_eviction) {
    reset();
    uint8_t mac[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    /* Fill all 16 slots */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        uint32_t ip = IP4(10, 0, 0, (uint8_t)(i + 1));
        arp_insert(ip, mac);
    }
    /* All should be found */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        ASSERT_TRUE(arp_lookup(IP4(10, 0, 0, (uint8_t)(i + 1))) != NULL);
    }
    /* Insert one more — should evict slot 0 */
    uint32_t new_ip = IP4(10, 0, 0, 100);
    arp_insert(new_ip, mac);
    ASSERT_TRUE(arp_lookup(new_ip) != NULL);
    /* First entry evicted */
    ASSERT_TRUE(arp_lookup(IP4(10, 0, 0, 1)) == NULL);
    return 0;
}

TEST(rx_short_packet_ignored) {
    reset();
    NetIf nif = {0};
    uint8_t data[10] = {0};
    arp_rx(&nif, data, 10);
    ASSERT_EQ(eth_send_called, 0);
    return 0;
}

TEST(rx_bad_htype_ignored) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    ArpPacket pkt;
    uint8_t sender_mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    build_arp_request(&pkt, IP4(10, 0, 2, 2), sender_mac, nif.ip_addr);
    pkt.htype = htons(99);  /* invalid */
    arp_rx(&nif, &pkt, ARP_PACKET_SIZE);
    ASSERT_EQ(eth_send_called, 0);
    return 0;
}

TEST(rx_bad_ptype_ignored) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    ArpPacket pkt;
    uint8_t sender_mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    build_arp_request(&pkt, IP4(10, 0, 2, 2), sender_mac, nif.ip_addr);
    pkt.ptype = htons(0x1234);  /* not IPv4 */
    arp_rx(&nif, &pkt, ARP_PACKET_SIZE);
    ASSERT_EQ(eth_send_called, 0);
    return 0;
}

TEST(rx_request_learns_sender) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    uint8_t sender_mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint32_t sender_ip = IP4(10, 0, 2, 2);
    ArpPacket pkt;
    build_arp_request(&pkt, sender_ip, sender_mac, nif.ip_addr);
    arp_rx(&nif, &pkt, ARP_PACKET_SIZE);
    const uint8_t *found = arp_lookup(sender_ip);
    ASSERT_TRUE(found != NULL);
    ASSERT_MEM_EQ(found, sender_mac, 6);
    return 0;
}

TEST(rx_request_sends_reply) {
    reset();
    NetIf nif = {0};
    uint8_t our_mac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    memcpy(nif.mac, our_mac, 6);
    nif.ip_addr = IP4(10, 0, 2, 15);
    nif.send = NULL;  /* eth_send is stubbed */

    uint8_t sender_mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint32_t sender_ip = IP4(10, 0, 2, 2);
    ArpPacket pkt;
    build_arp_request(&pkt, sender_ip, sender_mac, nif.ip_addr);
    arp_rx(&nif, &pkt, ARP_PACKET_SIZE);

    ASSERT_EQ(eth_send_called, 1);
    ASSERT_EQ(eth_send_ethertype, ETH_TYPE_ARP);
    ASSERT_EQ(eth_send_len, ARP_PACKET_SIZE);

    /* Verify reply content */
    ArpPacket *reply = (ArpPacket *)eth_send_capture;
    ASSERT_EQ(ntohs(reply->oper), ARP_OP_REPLY);
    ASSERT_MEM_EQ(reply->sha, our_mac, 6);
    ASSERT_EQ(reply->spa, nif.ip_addr);
    ASSERT_MEM_EQ(reply->tha, sender_mac, 6);
    ASSERT_EQ(reply->tpa, sender_ip);
    return 0;
}

TEST(rx_request_wrong_target_no_reply) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    uint8_t sender_mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    ArpPacket pkt;
    build_arp_request(&pkt, IP4(10, 0, 2, 2), sender_mac, IP4(10, 0, 2, 99));
    arp_rx(&nif, &pkt, ARP_PACKET_SIZE);
    /* Should still learn sender but NOT send reply */
    ASSERT_EQ(eth_send_called, 0);
    ASSERT_TRUE(arp_lookup(IP4(10, 0, 2, 2)) != NULL);
    return 0;
}

TEST(rx_reply_learns_sender) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    uint8_t sender_mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    uint32_t sender_ip = IP4(10, 0, 2, 2);

    ArpPacket pkt;
    pkt.htype = htons(ARP_HTYPE_ETHER);
    pkt.ptype = htons(ARP_PTYPE_IPV4);
    pkt.hlen = ETH_ALEN;
    pkt.plen = 4;
    pkt.oper = htons(ARP_OP_REPLY);
    memcpy(pkt.sha, sender_mac, 6);
    pkt.spa = sender_ip;
    memset(pkt.tha, 0, 6);
    pkt.tpa = nif.ip_addr;

    arp_rx(&nif, &pkt, ARP_PACKET_SIZE);
    /* Reply should NOT trigger a response, but should learn */
    ASSERT_EQ(eth_send_called, 0);
    const uint8_t *found = arp_lookup(sender_ip);
    ASSERT_TRUE(found != NULL);
    ASSERT_MEM_EQ(found, sender_mac, 6);
    return 0;
}

TEST(rx_bad_hlen_ignored) {
    reset();
    NetIf nif = {0};
    nif.ip_addr = IP4(10, 0, 2, 15);
    uint8_t sender_mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    ArpPacket pkt;
    build_arp_request(&pkt, IP4(10, 0, 2, 2), sender_mac, nif.ip_addr);
    pkt.hlen = 8;  /* wrong */
    arp_rx(&nif, &pkt, ARP_PACKET_SIZE);
    ASSERT_EQ(eth_send_called, 0);
    return 0;
}

/* --- Suite --- */

TestCase arp_tests[] = {
    TEST_ENTRY(init_clears_cache),
    TEST_ENTRY(insert_and_lookup),
    TEST_ENTRY(lookup_miss),
    TEST_ENTRY(insert_updates_existing),
    TEST_ENTRY(cache_eviction),
    TEST_ENTRY(rx_short_packet_ignored),
    TEST_ENTRY(rx_bad_htype_ignored),
    TEST_ENTRY(rx_bad_ptype_ignored),
    TEST_ENTRY(rx_request_learns_sender),
    TEST_ENTRY(rx_request_sends_reply),
    TEST_ENTRY(rx_request_wrong_target_no_reply),
    TEST_ENTRY(rx_reply_learns_sender),
    TEST_ENTRY(rx_bad_hlen_ignored),
};
int arp_test_count = sizeof(arp_tests) / sizeof(arp_tests[0]);
