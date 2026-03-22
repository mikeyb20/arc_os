/* arc_os -- Host-side tests for loopback interface */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>

/* Guard kernel headers */
#define ARCHOS_NET_LOOPBACK_H
#define ARCHOS_NET_NETIF_H
#define ARCHOS_NET_ETHERNET_H
#define ARCHOS_NET_NET_UTIL_H
#define ARCHOS_LIB_MEM_H
#define ARCHOS_LIB_KPRINTF_H

#define ETH_ALEN     6
#define NETIF_MAX    4

static inline uint32_t IP4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
}

typedef struct NetIf {
    uint8_t  mac[ETH_ALEN];
    uint32_t ip_addr;
    uint32_t netmask;
    uint32_t gateway;
    int (*send)(struct NetIf *nif, const void *frame, uint32_t len);
    void *private_data;
} NetIf;

/* Minimal netif stub */
static NetIf *reg_interfaces[NETIF_MAX];
static int reg_count;

static int netif_register(NetIf *nif) {
    if (reg_count >= NETIF_MAX) return -1;
    reg_interfaces[reg_count] = nif;
    return reg_count++;
}

/* eth_rx capture */
static int eth_rx_called;
static const void *eth_rx_frame;
static uint32_t eth_rx_len;

static void eth_rx(struct NetIf *nif, const void *frame, uint32_t len) {
    (void)nif;
    eth_rx_called++;
    eth_rx_frame = frame;
    eth_rx_len = len;
}

static void kprintf(const char *fmt, ...) { (void)fmt; }

/* Include loopback.c */
#include "../kernel/net/loopback.c"

static void reset(void) {
    reg_count = 0;
    eth_rx_called = 0;
    eth_rx_frame = NULL;
    eth_rx_len = 0;
    memset(&lo_nif, 0, sizeof(lo_nif));
}

/* --- Tests --- */

TEST(init_registers) {
    reset();
    loopback_init();
    ASSERT_EQ(reg_count, 1);
    return 0;
}

TEST(loopback_ip) {
    reset();
    loopback_init();
    ASSERT_EQ(lo_nif.ip_addr, IP4(127, 0, 0, 1));
    return 0;
}

TEST(loopback_netmask) {
    reset();
    loopback_init();
    ASSERT_EQ(lo_nif.netmask, IP4(255, 0, 0, 0));
    return 0;
}

TEST(send_loops_to_rx) {
    reset();
    loopback_init();
    uint8_t frame[] = {0xDE, 0xAD, 0xBE, 0xEF};
    lo_nif.send(&lo_nif, frame, 4);
    ASSERT_EQ(eth_rx_called, 1);
    ASSERT_EQ(eth_rx_len, (uint32_t)4);
    ASSERT_MEM_EQ(eth_rx_frame, frame, 4);
    return 0;
}

TEST(mac_is_zero) {
    reset();
    loopback_init();
    uint8_t zero_mac[ETH_ALEN] = {0};
    ASSERT_MEM_EQ(lo_nif.mac, zero_mac, ETH_ALEN);
    return 0;
}

/* --- Suite --- */

TestCase loopback_tests[] = {
    TEST_ENTRY(init_registers),
    TEST_ENTRY(loopback_ip),
    TEST_ENTRY(loopback_netmask),
    TEST_ENTRY(send_loops_to_rx),
    TEST_ENTRY(mac_is_zero),
};
int loopback_test_count = sizeof(loopback_tests) / sizeof(loopback_tests[0]);
