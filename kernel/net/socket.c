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
static Spinlock socket_table_lock = SPINLOCK_INIT(LOCK_LEVEL_OBJECT_TABLE, "socket table");
static uint16_t ephemeral_port_next = 49152;

int socket_create(uint16_t family, uint16_t type, uint16_t protocol) {
    uint64_t irqf;
    (void)protocol;
    if (family != AF_INET) return -ENOSYS;
    if (type != SOCK_DGRAM && type != SOCK_STREAM) return -EINVAL;

    Socket *sk = kmalloc(sizeof(Socket), GFP_ZERO);
    if (sk == NULL) return -ENOMEM;

    sk->family = family;
    sk->type = type;
    sk->state = SOCK_STATE_CLOSED;
    sk->refs = 1;                     /* the fd socket() installs */
    spin_lock_init(&sk->lock, LOCK_LEVEL_OBJECT, "socket");
    wq_init(&sk->rx_wq);
    wq_init(&sk->accept_wq);

    spin_lock_irqsave(&socket_table_lock, &irqf);
    int idx = -1;
    for (int i = 0; i < SOCKET_MAX; i++) {
        if (socket_table[i] == NULL) {
            socket_table[i] = sk;
            idx = i;
            break;
        }
    }
    spin_unlock_irqrestore(&socket_table_lock, irqf);

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

/* Abortive close: tell the peer of a TCP connection that it is gone (RST).
 * arc_os has no lingering close (FIN, TIME_WAIT) yet. */
static void socket_reset_peer(Socket *sk) {
    if (sk->type != SOCK_STREAM) return;
    switch (sk->state) {
    case SOCK_STATE_SYN_RECV:
    case SOCK_STATE_ESTABLISHED:
    case SOCK_STATE_CLOSE_WAIT: {
        NetIf *nif = netif_get_default();
        if (nif != NULL) {
            tcp_send(nif, sk->local_ip, sk->local_port, sk->remote_ip, sk->remote_port, sk->snd_nxt, sk->rcv_nxt,
                     TCP_RST | TCP_ACK, NULL, 0);
        }
        break;
    }
    default:
        break;
    }
}

void socket_free(int idx) {
    uint64_t irqf;
    if (idx < 0 || idx >= SOCKET_MAX) return;
    spin_lock_irqsave(&socket_table_lock, &irqf);
    Socket *sk = socket_table[idx];
    socket_table[idx] = NULL;
    spin_unlock_irqrestore(&socket_table_lock, irqf);
    if (sk == NULL) return;

    /* Out of the table, so packet delivery no longer finds it. The kernel
     * lock (M1-05) keeps syscalls and the receive interrupt apart, so no
     * receive path holds it either; the socket layer's own locking (lookups
     * under the table lock) is needed before that lock can go. */
    spin_lock_irqsave(&sk->lock, &irqf);
    sk->closed = 1;
    /* Connections a listener received but nobody accepted live only here */
    while (sk->accept_head != sk->accept_tail) {
        Socket *child = sk->accept_queue[sk->accept_head];
        sk->accept_head = (sk->accept_head + 1) % SOCK_BACKLOG_MAX;
        socket_reset_peer(child);
        kfree(child);
    }
    spin_unlock_irqrestore(&sk->lock, irqf);
    socket_reset_peer(sk);
    wq_wake_all(&sk->rx_wq);
    wq_wake_all(&sk->accept_wq);
    kfree(sk);
}

void socket_addref(int idx) {
    Socket *sk = socket_get(idx);
    if (sk != NULL) sk->refs++;
}

void socket_release(int idx) {
    Socket *sk = socket_get(idx);
    if (sk == NULL) return;
    if (--sk->refs <= 0) socket_free(idx);
}

uint16_t socket_alloc_ephemeral_port(void) {
    uint64_t irqf;
    spin_lock_irqsave(&socket_table_lock, &irqf);
    uint16_t port = htons(ephemeral_port_next++);
    if (ephemeral_port_next > 65534) ephemeral_port_next = 49152;
    spin_unlock_irqrestore(&socket_table_lock, irqf);
    return port;
}

int socket_bind(Socket *sk, uint32_t ip, uint16_t port) {
    uint64_t irqf;
    if (sk == NULL) return -EINVAL;
    spin_lock_irqsave(&sk->lock, &irqf);
    sk->local_ip = ip;
    sk->local_port = port;
    spin_unlock_irqrestore(&sk->lock, irqf);
    return 0;
}

int socket_listen(Socket *sk, int backlog) {
    uint64_t irqf;
    (void)backlog;
    if (sk == NULL || sk->type != SOCK_STREAM) return -EINVAL;
    spin_lock_irqsave(&sk->lock, &irqf);
    sk->state = SOCK_STATE_LISTEN;
    spin_unlock_irqrestore(&sk->lock, irqf);
    return 0;
}

int socket_accept(Socket *sk, SockAddrIn *addr_out) {
    uint64_t irqf;
    if (sk == NULL || sk->state != SOCK_STATE_LISTEN) return -EINVAL;

    spin_lock_irqsave(&sk->lock, &irqf);
    while (sk->accept_head == sk->accept_tail && !sk->closed) {
        wq_sleep(&sk->accept_wq, &sk->lock, irqf);
        spin_lock_irqsave(&sk->lock, &irqf);
    }

    if (sk->closed) {
        spin_unlock_irqrestore(&sk->lock, irqf);
        return -EBADF;
    }

    Socket *child = sk->accept_queue[sk->accept_head];
    sk->accept_head = (sk->accept_head + 1) % SOCK_BACKLOG_MAX;
    spin_unlock_irqrestore(&sk->lock, irqf);

    /* Register child socket in the global table */
    spin_lock_irqsave(&socket_table_lock, &irqf);
    int idx = -1;
    for (int i = 0; i < SOCKET_MAX; i++) {
        if (socket_table[i] == NULL) {
            socket_table[i] = child;
            idx = i;
            break;
        }
    }
    spin_unlock_irqrestore(&socket_table_lock, irqf);

    if (idx < 0) {
        socket_reset_peer(child);
        kfree(child);
        return -ENOMEM;
    }
    child->refs = 1;                  /* the fd accept() installs */

    if (addr_out) {
        addr_out->sin_family = AF_INET;
        addr_out->sin_port = child->remote_port;
        addr_out->sin_addr = child->remote_ip;
    }

    return idx;
}

int socket_connect(Socket *sk, uint32_t ip, uint16_t port) {
    uint64_t irqf;
    if (sk == NULL) return -EINVAL;

    spin_lock_irqsave(&sk->lock, &irqf);
    sk->remote_ip = ip;
    sk->remote_port = port;

    if (sk->local_port == 0) {
        sk->local_port = socket_alloc_ephemeral_port();
    }

    if (sk->type == SOCK_DGRAM) {
        /* UDP: just record the remote address */
        sk->state = SOCK_STATE_ESTABLISHED;
        spin_unlock_irqrestore(&sk->lock, irqf);
        return 0;
    }

    /* TCP: send SYN */
    NetIf *nif = netif_get_default();
    if (nif == NULL) {
        spin_unlock_irqrestore(&sk->lock, irqf);
        return -ENETUNREACH;
    }

    sk->local_ip = nif->ip_addr;
    sk->snd_nxt = 1000;  /* Initial sequence number (simplified) */
    sk->snd_una = sk->snd_nxt;
    sk->state = SOCK_STATE_SYN_SENT;
    uint32_t seq = sk->snd_nxt++;
    spin_unlock_irqrestore(&sk->lock, irqf);

    tcp_send(nif, nif->ip_addr, sk->local_port,
             ip, port, seq, 0, TCP_SYN, NULL, 0);

    /* Wait for SYN-ACK */
    spin_lock_irqsave(&sk->lock, &irqf);
    while (sk->state == SOCK_STATE_SYN_SENT && !sk->closed) {
        wq_sleep(&sk->rx_wq, &sk->lock, irqf);
        spin_lock_irqsave(&sk->lock, &irqf);
    }

    int ret = (sk->state == SOCK_STATE_ESTABLISHED) ? 0 : -ECONNREFUSED;
    spin_unlock_irqrestore(&sk->lock, irqf);
    return ret;
}

int socket_send(Socket *sk, const void *buf, uint32_t len) {
    uint64_t irqf;
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

        spin_lock_irqsave(&sk->lock, &irqf);
        uint32_t seq = sk->snd_nxt;
        sk->snd_nxt += chunk;
        spin_unlock_irqrestore(&sk->lock, irqf);

        tcp_send(nif, sk->local_ip, sk->local_port,
                 sk->remote_ip, sk->remote_port,
                 seq, sk->rcv_nxt, TCP_ACK | TCP_PSH,
                 data + sent, chunk);
        sent += chunk;
    }

    return (int)sent;
}

int socket_recv(Socket *sk, void *buf, uint32_t len) {
    uint64_t irqf;
    if (sk == NULL || buf == NULL) return -EINVAL;

    spin_lock_irqsave(&sk->lock, &irqf);
    while (sk->rx_count == 0 && !sk->closed &&
           sk->state != SOCK_STATE_CLOSE_WAIT) {
        wq_sleep(&sk->rx_wq, &sk->lock, irqf);
        spin_lock_irqsave(&sk->lock, &irqf);
    }

    if (sk->rx_count == 0) {
        spin_unlock_irqrestore(&sk->lock, irqf);
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

    spin_unlock_irqrestore(&sk->lock, irqf);
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
    uint64_t irqf;
    if (sk == NULL || buf == NULL) return -EINVAL;

    spin_lock_irqsave(&sk->lock, &irqf);
    while (sk->rx_count == 0 && !sk->closed) {
        wq_sleep(&sk->rx_wq, &sk->lock, irqf);
        spin_lock_irqsave(&sk->lock, &irqf);
    }

    if (sk->rx_count == 0) {
        spin_unlock_irqrestore(&sk->lock, irqf);
        return 0;
    }

    /* For UDP, each datagram is prefixed with UdpRxMeta */
    if (sk->rx_count < sizeof(UdpRxMeta)) {
        spin_unlock_irqrestore(&sk->lock, irqf);
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

    spin_unlock_irqrestore(&sk->lock, irqf);
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
    uint64_t irqf;
    (void)dst_ip;
    Socket *sk = find_socket(dst_port, SOCK_DGRAM, 0, 0);
    if (sk == NULL) return;  /* No matching socket — drop */

    spin_lock_irqsave(&sk->lock, &irqf);

    /* Store metadata + data in rx buffer */
    UdpRxMeta meta = {
        .src_ip = src_ip,
        .src_port = src_port,
        .data_len = (uint16_t)len,
    };

    if (rx_enqueue(sk, &meta, sizeof(meta)) == 0) {
        rx_enqueue(sk, data, len);
    }

    spin_unlock_irqrestore(&sk->lock, irqf);
    wq_wake(&sk->rx_wq);
}

void socket_tcp_deliver(uint32_t src_ip, uint16_t src_port,
                        uint32_t dst_ip, uint16_t dst_port,
                        const void *segment, uint32_t seg_len) {
    uint64_t irqf;
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

    spin_lock_irqsave(&sk->lock, &irqf);

    switch (sk->state) {
    case SOCK_STATE_LISTEN:
        if (flags & TCP_SYN) {
            /* Create child socket for this connection */
            Socket *child = kmalloc(sizeof(Socket), GFP_ZERO);
            if (child == NULL) {
                spin_unlock_irqrestore(&sk->lock, irqf);
                return;
            }

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
            spin_lock_init(&child->lock, LOCK_LEVEL_OBJECT, "socket");
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
        spin_unlock_irqrestore(&sk->lock, irqf);
        return;

    case SOCK_STATE_SYN_RECV:
        if (flags & TCP_ACK) {
            sk->snd_una = ack;
            sk->state = SOCK_STATE_ESTABLISHED;
            /* Wake accept() caller on parent */
            spin_unlock_irqrestore(&sk->lock, irqf);
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

            spin_unlock_irqrestore(&sk->lock, irqf);
            wq_wake(&sk->rx_wq);
            return;
        }
        if (flags & TCP_RST) {
            sk->state = SOCK_STATE_CLOSED;
            spin_unlock_irqrestore(&sk->lock, irqf);
            wq_wake(&sk->rx_wq);
            return;
        }
        break;

    case SOCK_STATE_ESTABLISHED:
        if (flags & TCP_RST) {
            sk->state = SOCK_STATE_CLOSED;
            spin_unlock_irqrestore(&sk->lock, irqf);
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

        spin_unlock_irqrestore(&sk->lock, irqf);
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

    spin_unlock_irqrestore(&sk->lock, irqf);
}
