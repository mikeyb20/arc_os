#include "net/socket.h"
#include "net/udp.h"
#include "net/netif.h"
#include "net/net_util.h"
#include "lib/mem.h"
#include "lib/kprintf.h"

/* Forward declarations */
Socket *socket_from_vnode(VfsNode *node);
int socket_recvfrom(Socket *sock, void *buf, uint32_t len, sockaddr_in *src_addr);
int socket_sendto(Socket *sock, const void *buf, uint32_t len, const sockaddr_in *dest_addr);

/* Static socket pool */
static Socket socket_pool[SOCKET_MAX];
static Spinlock pool_lock = SPINLOCK_INIT;

/* Next ephemeral port candidate (wraps around) */
static uint32_t next_ephemeral = EPHEMERAL_PORT_MIN;

/* VfsOps for socket nodes (read/write wrappers) */
static int socket_vfs_read(VfsNode *node, void *buf, uint32_t offset, uint32_t size) {
    (void)offset;
    Socket *sock = socket_from_vnode(node);
    if (!sock) return -EBADF;
    return socket_recvfrom(sock, buf, size, NULL);
}

static int socket_vfs_write(VfsNode *node, const void *buf, uint32_t offset, uint32_t size) {
    (void)offset;
    Socket *sock = socket_from_vnode(node);
    if (!sock) return -EBADF;
    if (sock->remote_port == 0) return -ENOTCONN;
    sockaddr_in dest;
    dest.sin_family = AF_INET;
    dest.sin_port = sock->remote_port;
    dest.sin_addr = sock->remote_ip;
    return socket_sendto(sock, buf, size, &dest);
}

void socket_init(void) {
    memset(socket_pool, 0, sizeof(socket_pool));
    kprintf("[SOCKET] Socket subsystem initialized (%d slots)\n", SOCKET_MAX);
}

Socket *socket_create(int domain, int type, int protocol) {
    if (domain != AF_INET) return NULL;
    if (type != SOCK_DGRAM) return NULL;
    if (protocol != 0 && protocol != IPPROTO_UDP) return NULL;

    spinlock_acquire(&pool_lock);
    Socket *sock = NULL;
    for (int i = 0; i < SOCKET_MAX; i++) {
        if (!socket_pool[i].in_use) {
            sock = &socket_pool[i];
            memset(sock, 0, sizeof(Socket));
            sock->in_use = 1;
            break;
        }
    }
    spinlock_release(&pool_lock);
    if (!sock) return NULL;
    sock->domain = (uint8_t)domain;
    sock->type = (uint8_t)type;
    sock->protocol = IPPROTO_UDP;
    sock->ref_count = 1;

    /* VFS integration */
    sock->vnode.type = VFS_SOCKET;
    sock->vnode.private_data = sock;
    sock->ops.read = socket_vfs_read;
    sock->ops.write = socket_vfs_write;
    sock->vnode.ops = &sock->ops;

    spinlock_init(&sock->lock);
    wq_init(&sock->recv_wq);

    return sock;
}

/* Check if a port is already bound */
static int port_in_use(uint16_t port_net) {
    for (int i = 0; i < SOCKET_MAX; i++) {
        if (socket_pool[i].in_use && socket_pool[i].local_port == port_net)
            return 1;
    }
    return 0;
}

/* Allocate an ephemeral port (returns network byte order, 0 on failure) */
static uint16_t alloc_ephemeral_port(void) {
    uint16_t start = next_ephemeral;
    do {
        uint16_t port_net = htons(next_ephemeral);
        next_ephemeral++;
        if (next_ephemeral > EPHEMERAL_PORT_MAX)
            next_ephemeral = EPHEMERAL_PORT_MIN;
        if (!port_in_use(port_net))
            return port_net;
    } while (next_ephemeral != start);
    return 0;  /* All ports exhausted */
}

int socket_bind(Socket *sock, const sockaddr_in *addr) {
    if (addr->sin_family != AF_INET) return -EINVAL;

    spinlock_acquire(&pool_lock);

    uint16_t port = addr->sin_port;
    if (port == 0) {
        port = alloc_ephemeral_port();
        if (port == 0) {
            spinlock_release(&pool_lock);
            return -EADDRINUSE;
        }
    } else if (port_in_use(port)) {
        spinlock_release(&pool_lock);
        return -EADDRINUSE;
    }

    sock->local_ip = addr->sin_addr;
    sock->local_port = port;
    spinlock_release(&pool_lock);
    return 0;
}

int socket_sendto(Socket *sock, const void *buf, uint32_t len,
                  const sockaddr_in *dest_addr) {
    if (!dest_addr) return -EINVAL;

    /* Auto-bind if not yet bound */
    if (sock->local_port == 0) {
        sockaddr_in any = { .sin_family = AF_INET, .sin_port = 0, .sin_addr = INADDR_ANY };
        int err = socket_bind(sock, &any);
        if (err != 0) return err;
    }

    uint32_t dst_ip = dest_addr->sin_addr;

    /* Find the right interface for this destination */
    NetIf *nif = netif_find_by_ip(dst_ip);
    if (!nif) nif = netif_get_default();
    if (!nif) return -ENETUNREACH;

    uint32_t src_ip = (sock->local_ip == INADDR_ANY) ? nif->ip_addr : sock->local_ip;

    return udp_send(nif, src_ip, dst_ip, sock->local_port, dest_addr->sin_port,
                    buf, len);
}

int socket_recvfrom(Socket *sock, void *buf, uint32_t len,
                    sockaddr_in *src_addr) {
    spinlock_acquire(&sock->lock);

    /* Block until a datagram is available */
    while (sock->rx_dgram_count == 0) {
        wq_sleep(&sock->recv_wq, &sock->lock);
        spinlock_acquire(&sock->lock);
    }

    /* Dequeue front datagram */
    SockDatagram *dg = &sock->rx_dgrams[sock->rx_dgram_tail];
    uint32_t copy_len = (dg->len < len) ? dg->len : len;
    memcpy(buf, &sock->rxbuf[dg->offset], copy_len);

    if (src_addr) {
        src_addr->sin_family = AF_INET;
        src_addr->sin_port = dg->src_port;
        src_addr->sin_addr = dg->src_ip;
        memset(src_addr->sin_zero, 0, 8);
    }

    /* Free slot */
    sock->rx_used -= dg->len;
    sock->rx_dgram_tail = (sock->rx_dgram_tail + 1) % SOCK_MAX_DATAGRAMS;
    sock->rx_dgram_count--;

    /* Reset write head when buffer is empty (no offsets reference it) */
    if (sock->rx_dgram_count == 0) {
        sock->rx_head = 0;
        sock->rx_used = 0;
    }

    spinlock_release(&sock->lock);
    return (int)copy_len;
}

void socket_close(Socket *sock) {
    if (!sock) return;
    spinlock_acquire(&pool_lock);
    sock->ref_count--;
    if (sock->ref_count == 0) {
        sock->in_use = 0;
        spinlock_release(&pool_lock);
        wq_wake_all(&sock->recv_wq);
        return;
    }
    spinlock_release(&pool_lock);
}

void socket_addref(Socket *sock) {
    if (!sock) return;
    spinlock_acquire(&pool_lock);
    sock->ref_count++;
    spinlock_release(&pool_lock);
}

int socket_udp_deliver(uint32_t src_ip, uint16_t src_port,
                       uint32_t dst_ip, uint16_t dst_port,
                       const void *data, uint32_t len) {
    /* Find matching socket */
    for (int i = 0; i < SOCKET_MAX; i++) {
        Socket *s = &socket_pool[i];
        if (!s->in_use || s->local_port != dst_port) continue;
        if (s->local_ip != INADDR_ANY && s->local_ip != dst_ip) continue;

        /* Match found — enqueue datagram */
        spinlock_acquire(&s->lock);

        if (s->rx_dgram_count >= SOCK_MAX_DATAGRAMS ||
            s->rx_used + len > SOCK_RXBUF_SIZE ||
            s->rx_head + len > SOCK_RXBUF_SIZE) {
            /* Buffer full — drop */
            spinlock_release(&s->lock);
            return -1;
        }

        /* Copy payload into rxbuf (contiguous, no wrap for simplicity) */
        memcpy(&s->rxbuf[s->rx_head], data, len);

        SockDatagram *dg = &s->rx_dgrams[s->rx_dgram_head];
        dg->src_ip = src_ip;
        dg->src_port = src_port;
        dg->len = (uint16_t)len;
        dg->offset = (uint16_t)s->rx_head;

        s->rx_head += len;
        s->rx_used += len;
        s->rx_dgram_head = (s->rx_dgram_head + 1) % SOCK_MAX_DATAGRAMS;
        s->rx_dgram_count++;

        spinlock_release(&s->lock);
        wq_wake(&s->recv_wq);
        return 0;
    }
    return -1;  /* No matching socket */
}

Socket *socket_from_vnode(VfsNode *node) {
    if (!node || node->type != VFS_SOCKET) return NULL;
    return (Socket *)node->private_data;
}
