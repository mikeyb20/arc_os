/* arc_os -- Host-side tests for socket abstraction */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

/* Guard kernel headers */
#define ARCHOS_NET_SOCKET_H
#define ARCHOS_NET_UDP_H
#define ARCHOS_NET_NET_UTIL_H
#define ARCHOS_NET_IPV4_H
#define ARCHOS_NET_NETIF_H
#define ARCHOS_NET_ETHERNET_H
#define ARCHOS_FS_VFS_H
#define ARCHOS_FS_PIPE_H
#define ARCHOS_MM_KMALLOC_H
#define ARCHOS_LIB_MEM_H
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_PROC_WAITQUEUE_H
#define ARCHOS_PROC_SPINLOCK_H

/* Minimal type definitions */
#define VFS_FILE      0
#define VFS_DIRECTORY 1
#define VFS_PIPE      2
#define VFS_SOCKET    3
#define AF_INET       2
#define SOCK_DGRAM    2
#define IPPROTO_UDP   17
#define INADDR_ANY    0
#define SOCKET_MAX    32
#define SOCK_RXBUF_SIZE    4096
#define SOCK_MAX_DATAGRAMS 16
#define EPHEMERAL_PORT_MIN 49152
#define EPHEMERAL_PORT_MAX 65535
#define EINVAL  22
#define EADDRINUSE 98
#define ENOTCONN  107
#define ENETUNREACH 101
#define EBADF  9
#define ETH_ALEN  6
#define ETH_MTU   1500

static inline uint16_t htons(uint16_t h) {
    return (uint16_t)((h >> 8) | (h << 8));
}
static inline uint16_t ntohs(uint16_t n) { return htons(n); }

static inline uint32_t IP4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return (uint32_t)a | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
}

typedef struct VfsNode VfsNode;
typedef struct {
    int (*read)(VfsNode *node, void *buf, uint32_t offset, uint32_t size);
    int (*write)(VfsNode *node, const void *buf, uint32_t offset, uint32_t size);
    VfsNode *(*lookup)(VfsNode *dir, const char *name);
    VfsNode *(*create)(VfsNode *dir, const char *name, uint8_t type);
    int (*unlink)(VfsNode *dir, const char *name);
    int (*readdir)(VfsNode *dir, void *entries, uint32_t max);
    void (*truncate)(VfsNode *node, uint64_t size);
} VfsOps;

struct VfsNode {
    uint64_t    inode_num;
    uint8_t     type;
    uint64_t    size;
    uint32_t    mode;
    uint32_t    uid;
    uint32_t    gid;
    const VfsOps *ops;
    void       *private_data;
};

/* Spinlock/WaitQueue stubs */
typedef struct { int locked; } Spinlock;
#define SPINLOCK_INIT {0}
typedef struct { int dummy; } WaitQueue;

static void spinlock_init(Spinlock *s) { s->locked = 0; }
static void spinlock_acquire(Spinlock *s) { s->locked = 1; }
static void spinlock_release(Spinlock *s) { s->locked = 0; }
static void wq_init(WaitQueue *wq) { (void)wq; }
static void wq_wake(WaitQueue *wq) { (void)wq; }
static void wq_wake_all(WaitQueue *wq) { (void)wq; }
/* wq_sleep: in tests we never actually block */
static void wq_sleep(WaitQueue *wq, Spinlock *lock) {
    (void)wq; spinlock_release(lock);
}

/* sockaddr_in */
typedef struct {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t  sin_zero[8];
} sockaddr_in;

typedef struct {
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t len;
    uint16_t offset;
} SockDatagram;

typedef struct Socket {
    uint8_t        in_use;
    uint8_t        domain;
    uint8_t        type;
    uint8_t        protocol;
    uint32_t       local_ip;
    uint16_t       local_port;
    uint32_t       remote_ip;
    uint16_t       remote_port;
    uint8_t        rxbuf[SOCK_RXBUF_SIZE];
    uint32_t       rx_head;
    uint32_t       rx_used;
    SockDatagram   rx_dgrams[SOCK_MAX_DATAGRAMS];
    uint32_t       rx_dgram_head;
    uint32_t       rx_dgram_tail;
    uint32_t       rx_dgram_count;
    VfsNode        vnode;
    VfsOps         ops;
    Spinlock       lock;
    WaitQueue      recv_wq;
    uint32_t       ref_count;
} Socket;

/* Stubs for external deps */
static void kprintf(const char *fmt, ...) { (void)fmt; }

/* udp_send capture */
static int udp_send_called;
static uint32_t udp_send_dst_ip;
static uint16_t udp_send_dst_port, udp_send_src_port;

typedef struct NetIf {
    uint8_t mac[ETH_ALEN];
    uint32_t ip_addr;
    uint32_t netmask;
    uint32_t gateway;
    int (*send)(struct NetIf *nif, const void *frame, uint32_t len);
    void *private_data;
} NetIf;

static int udp_send(struct NetIf *nif, uint32_t src_ip, uint32_t dst_ip,
                    uint16_t src_port, uint16_t dst_port,
                    const void *payload, uint32_t payload_len) {
    (void)nif; (void)src_ip; (void)payload; (void)payload_len;
    udp_send_called++;
    udp_send_dst_ip = dst_ip;
    udp_send_dst_port = dst_port;
    udp_send_src_port = src_port;
    return 0;
}

static NetIf fake_nif = { .ip_addr = 0x0F02000A };  /* 10.0.2.15 */

static NetIf *netif_find_by_ip(uint32_t dst_ip) {
    (void)dst_ip;
    return &fake_nif;
}
static NetIf *netif_get_default(void) { return &fake_nif; }

/* Include socket.c */
#include "../kernel/net/socket.c"

static void reset(void) {
    memset(socket_pool, 0, sizeof(socket_pool));
    next_ephemeral = EPHEMERAL_PORT_MIN;
    udp_send_called = 0;
    udp_send_dst_ip = 0;
    udp_send_dst_port = 0;
    udp_send_src_port = 0;
}

/* --- Tests --- */

TEST(create_valid) {
    reset();
    Socket *s = socket_create(AF_INET, SOCK_DGRAM, 0);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ(s->in_use, (uint8_t)1);
    ASSERT_EQ(s->domain, (uint8_t)AF_INET);
    ASSERT_EQ(s->type, (uint8_t)SOCK_DGRAM);
    ASSERT_EQ(s->ref_count, (uint32_t)1);
    return 0;
}

TEST(create_bad_domain) {
    reset();
    Socket *s = socket_create(99, SOCK_DGRAM, 0);
    ASSERT_TRUE(s == NULL);
    return 0;
}

TEST(create_bad_type) {
    reset();
    Socket *s = socket_create(AF_INET, 99, 0);
    ASSERT_TRUE(s == NULL);
    return 0;
}

TEST(create_sets_vnode) {
    reset();
    Socket *s = socket_create(AF_INET, SOCK_DGRAM, 0);
    ASSERT_EQ(s->vnode.type, (uint8_t)VFS_SOCKET);
    ASSERT_EQ(s->vnode.private_data, s);
    return 0;
}

TEST(bind_valid_port) {
    reset();
    Socket *s = socket_create(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(5000), .sin_addr = INADDR_ANY };
    int ret = socket_bind(s, &addr);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(s->local_port, htons(5000));
    return 0;
}

TEST(bind_ephemeral) {
    reset();
    Socket *s = socket_create(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr = { .sin_family = AF_INET, .sin_port = 0, .sin_addr = INADDR_ANY };
    int ret = socket_bind(s, &addr);
    ASSERT_EQ(ret, 0);
    ASSERT_TRUE(s->local_port != 0);
    ASSERT_EQ(ntohs(s->local_port) >= EPHEMERAL_PORT_MIN, 1);
    return 0;
}

TEST(bind_duplicate_fails) {
    reset();
    Socket *s1 = socket_create(AF_INET, SOCK_DGRAM, 0);
    Socket *s2 = socket_create(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(9999), .sin_addr = INADDR_ANY };
    ASSERT_EQ(socket_bind(s1, &addr), 0);
    ASSERT_EQ(socket_bind(s2, &addr), -EADDRINUSE);
    return 0;
}

TEST(bind_different_ports_ok) {
    reset();
    Socket *s1 = socket_create(AF_INET, SOCK_DGRAM, 0);
    Socket *s2 = socket_create(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a1 = { .sin_family = AF_INET, .sin_port = htons(1000) };
    sockaddr_in a2 = { .sin_family = AF_INET, .sin_port = htons(2000) };
    ASSERT_EQ(socket_bind(s1, &a1), 0);
    ASSERT_EQ(socket_bind(s2, &a2), 0);
    return 0;
}

TEST(close_frees) {
    reset();
    Socket *s = socket_create(AF_INET, SOCK_DGRAM, 0);
    ASSERT_EQ(s->in_use, (uint8_t)1);
    socket_close(s);
    ASSERT_EQ(s->in_use, (uint8_t)0);
    return 0;
}

TEST(addref_prevents_free) {
    reset();
    Socket *s = socket_create(AF_INET, SOCK_DGRAM, 0);
    socket_addref(s);
    ASSERT_EQ(s->ref_count, (uint32_t)2);
    socket_close(s);
    ASSERT_EQ(s->in_use, (uint8_t)1);
    socket_close(s);
    ASSERT_EQ(s->in_use, (uint8_t)0);
    return 0;
}

TEST(deliver_enqueues) {
    reset();
    Socket *s = socket_create(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(8080), .sin_addr = INADDR_ANY };
    socket_bind(s, &addr);

    int ret = socket_udp_deliver(IP4(10,0,2,2), htons(5000),
                                  IP4(10,0,2,15), htons(8080),
                                  "hello", 5);
    ASSERT_EQ(ret, 0);
    ASSERT_EQ(s->rx_dgram_count, (uint32_t)1);
    return 0;
}

TEST(deliver_no_match) {
    reset();
    int ret = socket_udp_deliver(IP4(10,0,2,2), htons(5000),
                                  IP4(10,0,2,15), htons(9999),
                                  "data", 4);
    ASSERT_EQ(ret, -1);
    return 0;
}

TEST(recvfrom_returns_data) {
    reset();
    Socket *s = socket_create(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(7000), .sin_addr = INADDR_ANY };
    socket_bind(s, &addr);

    socket_udp_deliver(IP4(10,0,2,2), htons(3000),
                       IP4(10,0,2,15), htons(7000), "test", 4);

    char buf[64];
    sockaddr_in src;
    int ret = socket_recvfrom(s, buf, 64, &src);
    ASSERT_EQ(ret, 4);
    ASSERT_MEM_EQ(buf, "test", 4);
    ASSERT_EQ(src.sin_port, htons(3000));
    ASSERT_EQ(src.sin_addr, IP4(10,0,2,2));
    return 0;
}

TEST(recvfrom_truncates) {
    reset();
    Socket *s = socket_create(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(6000), .sin_addr = INADDR_ANY };
    socket_bind(s, &addr);

    socket_udp_deliver(IP4(10,0,2,2), htons(1000),
                       IP4(10,0,2,15), htons(6000), "abcdef", 6);

    char buf[3];
    int ret = socket_recvfrom(s, buf, 3, NULL);
    ASSERT_EQ(ret, 3);
    ASSERT_MEM_EQ(buf, "abc", 3);
    return 0;
}

TEST(deliver_full_drops) {
    reset();
    Socket *s = socket_create(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(4000), .sin_addr = INADDR_ANY };
    socket_bind(s, &addr);

    for (int i = 0; i < SOCK_MAX_DATAGRAMS; i++) {
        ASSERT_EQ(socket_udp_deliver(IP4(10,0,2,2), htons(1000),
                                      IP4(10,0,2,15), htons(4000), "x", 1), 0);
    }
    /* 17th should fail */
    ASSERT_EQ(socket_udp_deliver(IP4(10,0,2,2), htons(1000),
                                  IP4(10,0,2,15), htons(4000), "y", 1), -1);
    return 0;
}

TEST(fifo_order) {
    reset();
    Socket *s = socket_create(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(3000), .sin_addr = INADDR_ANY };
    socket_bind(s, &addr);

    socket_udp_deliver(IP4(10,0,2,2), htons(100),
                       IP4(10,0,2,15), htons(3000), "first", 5);
    socket_udp_deliver(IP4(10,0,2,2), htons(100),
                       IP4(10,0,2,15), htons(3000), "second", 6);

    char buf[64];
    ASSERT_EQ(socket_recvfrom(s, buf, 64, NULL), 5);
    ASSERT_MEM_EQ(buf, "first", 5);
    ASSERT_EQ(socket_recvfrom(s, buf, 64, NULL), 6);
    ASSERT_MEM_EQ(buf, "second", 6);
    return 0;
}

TEST(from_vnode_roundtrip) {
    reset();
    Socket *s = socket_create(AF_INET, SOCK_DGRAM, 0);
    Socket *got = socket_from_vnode(&s->vnode);
    ASSERT_EQ(got, s);
    return 0;
}

TEST(sendto_auto_binds) {
    reset();
    Socket *s = socket_create(AF_INET, SOCK_DGRAM, 0);
    ASSERT_EQ(s->local_port, (uint16_t)0);

    sockaddr_in dest = { .sin_family = AF_INET, .sin_port = htons(9999),
                         .sin_addr = IP4(10,0,2,2) };
    socket_sendto(s, "hi", 2, &dest);

    ASSERT_TRUE(s->local_port != 0);
    ASSERT_EQ(udp_send_called, 1);
    return 0;
}

/* --- Suite --- */

TestCase socket_tests[] = {
    TEST_ENTRY(create_valid),
    TEST_ENTRY(create_bad_domain),
    TEST_ENTRY(create_bad_type),
    TEST_ENTRY(create_sets_vnode),
    TEST_ENTRY(bind_valid_port),
    TEST_ENTRY(bind_ephemeral),
    TEST_ENTRY(bind_duplicate_fails),
    TEST_ENTRY(bind_different_ports_ok),
    TEST_ENTRY(close_frees),
    TEST_ENTRY(addref_prevents_free),
    TEST_ENTRY(deliver_enqueues),
    TEST_ENTRY(deliver_no_match),
    TEST_ENTRY(recvfrom_returns_data),
    TEST_ENTRY(recvfrom_truncates),
    TEST_ENTRY(deliver_full_drops),
    TEST_ENTRY(fifo_order),
    TEST_ENTRY(from_vnode_roundtrip),
    TEST_ENTRY(sendto_auto_binds),
};
int socket_test_count = sizeof(socket_tests) / sizeof(socket_tests[0]);
