#include "net/socket.h"
#include "net/net_util.h"
#include "net/netif.h"
#include "net/udp.h"
#include "net/tcp.h"
#include "fs/vfs.h"  /* Error codes */
#include "lib/mem.h"
#include "lib/kprintf.h"
#include "mm/kmalloc.h"

/* Additional error codes not in vfs.h */
#ifndef ENOTCONN
#define ENOTCONN     107
#endif
#ifndef ENETUNREACH
#define ENETUNREACH  101
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED 111
#endif

/* Global socket table */
static Socket *socket_table[SOCKET_MAX];
static Spinlock socket_table_lock = SPINLOCK_INIT;
static uint16_t ephemeral_port_next = 49152;

int socket_create(uint16_t family, uint16_t type, uint16_t protocol) {
    (void)protocol;
    if (family != AF_INET) return -ENOSYS;
    if (type != SOCK_DGRAM && type != SOCK_STREAM) return -EINVAL;

    Socket *sk = kmalloc(sizeof(Socket), GFP_ZERO);
    if (sk == NULL) return -ENOMEM;

    sk->family = family;
    sk->type = type;
    sk->state = SOCK_STATE_CLOSED;
    sk->lock = (Spinlock)SPINLOCK_INIT;
    wq_init(&sk->rx_wq);
    wq_init(&sk->accept_wq);

    spinlock_acquire(&socket_table_lock);
    int idx = -1;
    for (int i = 0; i < SOCKET_MAX; i++) {
        if (socket_table[i] == NULL) {
            socket_table[i] = sk;
            idx = i;
            break;
        }
    }
    spinlock_release(&socket_table_lock);

    if (idx < 0) {
        kfree(sk);
        return -ENOMEM;
    }

    return idx;
}

Socket *socket_get(int idx) {
    if (idx < 0 || idx >= SOCKET_MAX) return NULL;
    return socket_table[idx];
}

void socket_free(int idx) {
    if (idx < 0 || idx >= SOCKET_MAX) return;
    spinlock_acquire(&socket_table_lock);
    Socket *sk = socket_table[idx];
    socket_table[idx] = NULL;
    spinlock_release(&socket_table_lock);
    if (sk) {
        sk->closed = 1;
        wq_wake_all(&sk->rx_wq);
        wq_wake_all(&sk->accept_wq);
        kfree(sk);
    }
}

uint16_t socket_alloc_ephemeral_port(void) {
    spinlock_acquire(&socket_table_lock);
    uint16_t port = htons(ephemeral_port_next++);
    if (ephemeral_port_next > 65534) ephemeral_port_next = 49152;
    spinlock_release(&socket_table_lock);
    return port;
}

int socket_bind(Socket *sk, uint32_t ip, uint16_t port) {
    if (sk == NULL) return -EINVAL;
    spinlock_acquire(&sk->lock);
    sk->local_ip = ip;
    sk->local_port = port;
    spinlock_release(&sk->lock);
    return 0;
}

int socket_listen(Socket *sk, int backlog) {
    (void)backlog;
    if (sk == NULL || sk->type != SOCK_STREAM) return -EINVAL;
    spinlock_acquire(&sk->lock);
    sk->state = SOCK_STATE_LISTEN;
    spinlock_release(&sk->lock);
    return 0;
}

int socket_accept(Socket *sk, SockAddrIn *addr_out) {
    if (sk == NULL || sk->state != SOCK_STATE_LISTEN) return -EINVAL;

    spinlock_acquire(&sk->lock);
    while (sk->accept_head == sk->accept_tail && !sk->closed) {
        wq_sleep(&sk->accept_wq, &sk->lock);
        spinlock_acquire(&sk->lock);
    }

    if (sk->closed) {
        spinlock_release(&sk->lock);
        return -EBADF;
    }

    Socket *child = sk->accept_queue[sk->accept_head];
    sk->accept_head = (sk->accept_head + 1) % SOCK_BACKLOG_MAX;
    spinlock_release(&sk->lock);

    /* Register child socket in the global table */
    spinlock_acquire(&socket_table_lock);
    int idx = -1;
    for (int i = 0; i < SOCKET_MAX; i++) {
        if (socket_table[i] == NULL) {
            socket_table[i] = child;
            idx = i;
            break;
        }
    }
    spinlock_release(&socket_table_lock);

    if (idx < 0) {
        kfree(child);
        return -ENOMEM;
    }

    if (addr_out) {
        addr_out->sin_family = AF_INET;
        addr_out->sin_port = child->remote_port;
        addr_out->sin_addr = child->remote_ip;
    }

    return idx;
}

int socket_connect(Socket *sk, uint32_t ip, uint16_t port) {
    if (sk == NULL) return -EINVAL;

    spinlock_acquire(&sk->lock);
    sk->remote_ip = ip;
    sk->remote_port = port;

    if (sk->local_port == 0) {
        sk->local_port = socket_alloc_ephemeral_port();
    }

    if (sk->type == SOCK_DGRAM) {
        /* UDP: just record the remote address */
        sk->state = SOCK_STATE_ESTABLISHED;
        spinlock_release(&sk->lock);
        return 0;
    }

    /* TCP: send SYN */
    NetIf *nif = netif_get_default();
    if (nif == NULL) {
        spinlock_release(&sk->lock);
        return -ENETUNREACH;
    }

    sk->local_ip = nif->ip_addr;
    sk->snd_nxt = 1000;  /* Initial sequence number (simplified) */
    sk->snd_una = sk->snd_nxt;
    sk->state = SOCK_STATE_SYN_SENT;
    uint32_t seq = sk->snd_nxt++;
    spinlock_release(&sk->lock);

    tcp_send(nif, nif->ip_addr, sk->local_port,
             ip, port, seq, 0, TCP_SYN, NULL, 0);

    /* Wait for SYN-ACK */
    spinlock_acquire(&sk->lock);
    while (sk->state == SOCK_STATE_SYN_SENT && !sk->closed) {
        wq_sleep(&sk->rx_wq, &sk->lock);
        spinlock_acquire(&sk->lock);
    }

    int ret = (sk->state == SOCK_STATE_ESTABLISHED) ? 0 : -ECONNREFUSED;
    spinlock_release(&sk->lock);
    return ret;
}

int socket_send(Socket *sk, const void *buf, uint32_t len) {
    if (sk == NULL || buf == NULL) return -EINVAL;
    if (sk->type == SOCK_DGRAM) {
        return socket_sendto(sk, buf, len, sk->remote_ip, sk->remote_port);
    }

    /* TCP send */
    if (sk->state != SOCK_STATE_ESTABLISHED) return -ENOTCONN;

    NetIf *nif = netif_get_default();
    if (nif == NULL) return -ENETUNREACH;

    /* Send data segments (simplified: single segment, no windowing) */
    uint32_t max_seg = 1460 - TCP_HEADER_SIZE;
    uint32_t sent = 0;
    const uint8_t *data = (const uint8_t *)buf;

    while (sent < len) {
        uint32_t chunk = len - sent;
        if (chunk > max_seg) chunk = max_seg;

        spinlock_acquire(&sk->lock);
        uint32_t seq = sk->snd_nxt;
        sk->snd_nxt += chunk;
        spinlock_release(&sk->lock);

        tcp_send(nif, sk->local_ip, sk->local_port,
                 sk->remote_ip, sk->remote_port,
                 seq, sk->rcv_nxt, TCP_ACK | TCP_PSH,
                 data + sent, chunk);
        sent += chunk;
    }

    return (int)sent;
}

int socket_recv(Socket *sk, void *buf, uint32_t len) {
    if (sk == NULL || buf == NULL) return -EINVAL;

    spinlock_acquire(&sk->lock);
    while (sk->rx_count == 0 && !sk->closed &&
           sk->state != SOCK_STATE_CLOSE_WAIT) {
        wq_sleep(&sk->rx_wq, &sk->lock);
        spinlock_acquire(&sk->lock);
    }

    if (sk->rx_count == 0) {
        spinlock_release(&sk->lock);
        return 0;  /* EOF or closed */
    }

    /* Copy from rx ring buffer */
    uint32_t to_copy = len;
    if (to_copy > sk->rx_count) to_copy = sk->rx_count;

    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < to_copy; i++) {
        dst[i] = sk->rx_buf[sk->rx_tail];
        sk->rx_tail = (sk->rx_tail + 1) % SOCK_RXBUF_SIZE;
    }
    sk->rx_count -= to_copy;

    spinlock_release(&sk->lock);
    return (int)to_copy;
}

int socket_sendto(Socket *sk, const void *buf, uint32_t len,
                  uint32_t dst_ip, uint16_t dst_port) {
    if (sk == NULL || buf == NULL) return -EINVAL;
    if (sk->type != SOCK_DGRAM) return -EINVAL;

    NetIf *nif = netif_get_default();
    if (nif == NULL) return -ENETUNREACH;

    if (sk->local_port == 0) {
        sk->local_port = socket_alloc_ephemeral_port();
    }

    int ret = udp_send(nif, nif->ip_addr, sk->local_port,
                       dst_ip, dst_port, buf, len);
    return (ret == 0) ? (int)len : ret;
}

int socket_recvfrom(Socket *sk, void *buf, uint32_t len,
                    uint32_t *src_ip, uint16_t *src_port) {
    if (sk == NULL || buf == NULL) return -EINVAL;

    spinlock_acquire(&sk->lock);
    while (sk->rx_count == 0 && !sk->closed) {
        wq_sleep(&sk->rx_wq, &sk->lock);
        spinlock_acquire(&sk->lock);
    }

    if (sk->rx_count == 0) {
        spinlock_release(&sk->lock);
        return 0;
    }

    /* For UDP, each datagram is prefixed with UdpRxMeta */
    if (sk->rx_count < sizeof(UdpRxMeta)) {
        spinlock_release(&sk->lock);
        return -EIO;
    }

    /* Read metadata */
    UdpRxMeta meta;
    uint8_t *mp = (uint8_t *)&meta;
    for (uint32_t i = 0; i < sizeof(meta); i++) {
        mp[i] = sk->rx_buf[sk->rx_tail];
        sk->rx_tail = (sk->rx_tail + 1) % SOCK_RXBUF_SIZE;
    }
    sk->rx_count -= sizeof(meta);

    /* Read data */
    uint32_t to_copy = meta.data_len;
    if (to_copy > len) to_copy = len;
    if (to_copy > sk->rx_count) to_copy = sk->rx_count;

    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < to_copy; i++) {
        dst[i] = sk->rx_buf[sk->rx_tail];
        sk->rx_tail = (sk->rx_tail + 1) % SOCK_RXBUF_SIZE;
    }
    /* Discard any remaining bytes if buf was smaller */
    uint32_t discard = meta.data_len - to_copy;
    for (uint32_t i = 0; i < discard; i++) {
        sk->rx_tail = (sk->rx_tail + 1) % SOCK_RXBUF_SIZE;
    }
    sk->rx_count -= meta.data_len;

    if (src_ip) *src_ip = meta.src_ip;
    if (src_port) *src_port = meta.src_port;

    spinlock_release(&sk->lock);
    return (int)to_copy;
}

/* --- Internal delivery functions (called from protocol handlers) --- */

/* Find a socket matching the given local port and type. */
static Socket *find_socket(uint16_t local_port, uint16_t type,
                            uint32_t remote_ip, uint16_t remote_port) {
    /* First try exact match (connected socket) */
    for (int i = 0; i < SOCKET_MAX; i++) {
        Socket *sk = socket_table[i];
        if (sk && sk->type == type && sk->local_port == local_port &&
            sk->remote_ip == remote_ip && sk->remote_port == remote_port) {
            return sk;
        }
    }
    /* Then try wildcard match (listening or unconnected) */
    for (int i = 0; i < SOCKET_MAX; i++) {
        Socket *sk = socket_table[i];
        if (sk && sk->type == type && sk->local_port == local_port &&
            (sk->remote_ip == 0 || sk->state == SOCK_STATE_LISTEN)) {
            return sk;
        }
    }
    return NULL;
}

/* Enqueue data into a socket's rx ring buffer. */
static int rx_enqueue(Socket *sk, const void *data, uint32_t len) {
    if (sk->rx_count + len > SOCK_RXBUF_SIZE) {
        return -1;  /* Buffer full, drop */
    }
    const uint8_t *src = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++) {
        sk->rx_buf[sk->rx_head] = src[i];
        sk->rx_head = (sk->rx_head + 1) % SOCK_RXBUF_SIZE;
    }
    sk->rx_count += len;
    return 0;
}

void socket_udp_deliver(uint32_t src_ip, uint16_t src_port,
                        uint32_t dst_ip, uint16_t dst_port,
                        const void *data, uint32_t len) {
    (void)dst_ip;
    Socket *sk = find_socket(dst_port, SOCK_DGRAM, 0, 0);
    if (sk == NULL) return;  /* No matching socket — drop */

    spinlock_acquire(&sk->lock);

    /* Store metadata + data in rx buffer */
    UdpRxMeta meta = {
        .src_ip = src_ip,
        .src_port = src_port,
        .data_len = (uint16_t)len,
    };

    if (rx_enqueue(sk, &meta, sizeof(meta)) == 0) {
        rx_enqueue(sk, data, len);
    }

    spinlock_release(&sk->lock);
    wq_wake(&sk->rx_wq);
}

void socket_tcp_deliver(uint32_t src_ip, uint16_t src_port,
                        uint32_t dst_ip, uint16_t dst_port,
                        const void *segment, uint32_t seg_len) {
    (void)dst_ip;
    const TcpHeader *hdr = (const TcpHeader *)segment;
    uint8_t flags = hdr->flags;
    uint32_t seq = ntohl(hdr->seq);
    uint32_t ack = ntohl(hdr->ack);
    uint8_t data_off = (hdr->data_off >> 4) * 4;
    const uint8_t *payload = (const uint8_t *)segment + data_off;
    uint32_t payload_len = seg_len - data_off;

    Socket *sk = find_socket(dst_port, SOCK_STREAM, src_ip, src_port);
    if (sk == NULL) {
        /* Check for listening socket */
        sk = find_socket(dst_port, SOCK_STREAM, 0, 0);
    }
    if (sk == NULL) {
        /* Send RST for unsolicited segments */
        NetIf *nif = netif_get_default();
        if (nif && !(flags & TCP_RST)) {
            tcp_send(nif, dst_ip, dst_port, src_ip, src_port,
                     ack, seq + 1, TCP_RST | TCP_ACK, NULL, 0);
        }
        return;
    }

    spinlock_acquire(&sk->lock);

    switch (sk->state) {
    case SOCK_STATE_LISTEN:
        if (flags & TCP_SYN) {
            /* Create child socket for this connection */
            Socket *child = kmalloc(sizeof(Socket), GFP_ZERO);
            if (child == NULL) { spinlock_release(&sk->lock); return; }

            child->family = AF_INET;
            child->type = SOCK_STREAM;
            child->local_ip = sk->local_ip;
            child->local_port = sk->local_port;
            child->remote_ip = src_ip;
            child->remote_port = src_port;
            child->rcv_nxt = seq + 1;
            child->snd_nxt = 2000;  /* ISN (simplified) */
            child->snd_una = child->snd_nxt;
            child->state = SOCK_STATE_SYN_RECV;
            child->lock = (Spinlock)SPINLOCK_INIT;
            wq_init(&child->rx_wq);
            wq_init(&child->accept_wq);

            /* Send SYN-ACK */
            NetIf *nif = netif_get_default();
            if (nif) {
                uint32_t syn_seq = child->snd_nxt++;
                tcp_send(nif, child->local_ip, child->local_port,
                         src_ip, src_port,
                         syn_seq, child->rcv_nxt,
                         TCP_SYN | TCP_ACK, NULL, 0);
            }

            /* Enqueue in accept queue */
            int next = (sk->accept_tail + 1) % SOCK_BACKLOG_MAX;
            if (next != sk->accept_head) {
                sk->accept_queue[sk->accept_tail] = child;
                sk->accept_tail = next;
            } else {
                kfree(child);  /* Backlog full */
            }
        }
        spinlock_release(&sk->lock);
        return;

    case SOCK_STATE_SYN_RECV:
        if (flags & TCP_ACK) {
            sk->snd_una = ack;
            sk->state = SOCK_STATE_ESTABLISHED;
            /* Wake accept() caller on parent */
            spinlock_release(&sk->lock);
            /* Find parent listening socket and wake it */
            for (int i = 0; i < SOCKET_MAX; i++) {
                Socket *parent = socket_table[i];
                if (parent && parent->state == SOCK_STATE_LISTEN &&
                    parent->local_port == sk->local_port) {
                    wq_wake(&parent->accept_wq);
                    break;
                }
            }
            return;
        }
        break;

    case SOCK_STATE_SYN_SENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            sk->snd_una = ack;
            sk->rcv_nxt = seq + 1;
            sk->state = SOCK_STATE_ESTABLISHED;

            /* Send ACK */
            NetIf *nif = netif_get_default();
            if (nif) {
                tcp_send(nif, sk->local_ip, sk->local_port,
                         src_ip, src_port,
                         sk->snd_nxt, sk->rcv_nxt,
                         TCP_ACK, NULL, 0);
            }

            spinlock_release(&sk->lock);
            wq_wake(&sk->rx_wq);
            return;
        }
        if (flags & TCP_RST) {
            sk->state = SOCK_STATE_CLOSED;
            spinlock_release(&sk->lock);
            wq_wake(&sk->rx_wq);
            return;
        }
        break;

    case SOCK_STATE_ESTABLISHED:
        if (flags & TCP_RST) {
            sk->state = SOCK_STATE_CLOSED;
            spinlock_release(&sk->lock);
            wq_wake(&sk->rx_wq);
            return;
        }

        /* ACK for our sent data */
        if (flags & TCP_ACK) {
            sk->snd_una = ack;
        }

        /* Incoming data */
        if (payload_len > 0) {
            rx_enqueue(sk, payload, payload_len);
            sk->rcv_nxt += payload_len;

            /* Send ACK */
            NetIf *nif = netif_get_default();
            if (nif) {
                tcp_send(nif, sk->local_ip, sk->local_port,
                         src_ip, src_port,
                         sk->snd_nxt, sk->rcv_nxt,
                         TCP_ACK, NULL, 0);
            }
        }

        /* FIN from peer */
        if (flags & TCP_FIN) {
            sk->rcv_nxt++;
            sk->state = SOCK_STATE_CLOSE_WAIT;

            /* Send ACK for FIN */
            NetIf *nif = netif_get_default();
            if (nif) {
                tcp_send(nif, sk->local_ip, sk->local_port,
                         src_ip, src_port,
                         sk->snd_nxt, sk->rcv_nxt,
                         TCP_ACK, NULL, 0);
            }
        }

        spinlock_release(&sk->lock);
        if (payload_len > 0 || (flags & TCP_FIN)) {
            wq_wake(&sk->rx_wq);
        }
        return;

    case SOCK_STATE_FIN_WAIT_1:
        if ((flags & TCP_ACK) && (flags & TCP_FIN)) {
            sk->rcv_nxt = seq + 1;
            sk->state = SOCK_STATE_TIME_WAIT;
            NetIf *nif = netif_get_default();
            if (nif) {
                tcp_send(nif, sk->local_ip, sk->local_port,
                         src_ip, src_port,
                         sk->snd_nxt, sk->rcv_nxt,
                         TCP_ACK, NULL, 0);
            }
        } else if (flags & TCP_ACK) {
            sk->state = SOCK_STATE_FIN_WAIT_2;
        }
        break;

    case SOCK_STATE_FIN_WAIT_2:
        if (flags & TCP_FIN) {
            sk->rcv_nxt = seq + 1;
            sk->state = SOCK_STATE_TIME_WAIT;
            NetIf *nif = netif_get_default();
            if (nif) {
                tcp_send(nif, sk->local_ip, sk->local_port,
                         src_ip, src_port,
                         sk->snd_nxt, sk->rcv_nxt,
                         TCP_ACK, NULL, 0);
            }
        }
        break;

    case SOCK_STATE_LAST_ACK:
        if (flags & TCP_ACK) {
            sk->state = SOCK_STATE_CLOSED;
        }
        break;

    default:
        break;
    }

    spinlock_release(&sk->lock);
}
