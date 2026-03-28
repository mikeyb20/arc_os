#ifndef ARCHOS_NET_SOCKET_H
#define ARCHOS_NET_SOCKET_H

#include <stdint.h>
#include "proc/spinlock.h"
#include "proc/waitqueue.h"

/* Address families */
#define AF_INET   2

/* Socket types */
#define SOCK_STREAM  1  /* TCP */
#define SOCK_DGRAM   2  /* UDP */

/* Socket states (TCP) */
#define SOCK_STATE_CLOSED       0
#define SOCK_STATE_LISTEN       1
#define SOCK_STATE_SYN_SENT     2
#define SOCK_STATE_SYN_RECV     3
#define SOCK_STATE_ESTABLISHED  4
#define SOCK_STATE_FIN_WAIT_1   5
#define SOCK_STATE_FIN_WAIT_2   6
#define SOCK_STATE_CLOSE_WAIT   7
#define SOCK_STATE_LAST_ACK     8
#define SOCK_STATE_TIME_WAIT    9

/* Socket address (IPv4) */
typedef struct {
    uint16_t sin_family;
    uint16_t sin_port;      /* Network byte order */
    uint32_t sin_addr;      /* Network byte order */
} SockAddrIn;

/* Receive ring buffer size */
#define SOCK_RXBUF_SIZE  8192
#define SOCK_TXBUF_SIZE  8192

/* Maximum pending connections for listen() */
#define SOCK_BACKLOG_MAX 8

/* Forward declaration */
struct Socket;

/* Per-datagram metadata for UDP (stored inline in rx buffer) */
typedef struct {
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t data_len;
} UdpRxMeta;

/* Socket Control Block */
typedef struct Socket {
    uint16_t  family;
    uint16_t  type;       /* SOCK_STREAM or SOCK_DGRAM */
    uint32_t  state;      /* TCP state machine */

    /* Local and remote addresses */
    uint32_t  local_ip;
    uint16_t  local_port;  /* Network byte order */
    uint32_t  remote_ip;
    uint16_t  remote_port; /* Network byte order */

    /* Receive ring buffer */
    uint8_t   rx_buf[SOCK_RXBUF_SIZE];
    uint32_t  rx_head;
    uint32_t  rx_tail;
    uint32_t  rx_count;

    /* TCP sequence numbers */
    uint32_t  snd_una;    /* Oldest unacknowledged */
    uint32_t  snd_nxt;    /* Next sequence to send */
    uint32_t  rcv_nxt;    /* Next expected from peer */

    /* Synchronization */
    Spinlock  lock;
    WaitQueue rx_wq;      /* Readers sleep here */
    WaitQueue accept_wq;  /* Listeners sleep here for incoming connections */

    /* Accept queue (for listening sockets) */
    struct Socket *accept_queue[SOCK_BACKLOG_MAX];
    int accept_head;
    int accept_tail;

    int closed;           /* Set when socket is being shut down */
} Socket;

/* Maximum number of active sockets */
#define SOCKET_MAX 64

/* Create a new socket.  Returns socket index (>= 0) or negative errno. */
int socket_create(uint16_t family, uint16_t type, uint16_t protocol);

/* Get socket by index.  Returns NULL if invalid. */
Socket *socket_get(int idx);

/* Free a socket by index. */
void socket_free(int idx);

/* Bind a socket to a local address. */
int socket_bind(Socket *sk, uint32_t ip, uint16_t port);

/* Mark socket as listening (TCP only). */
int socket_listen(Socket *sk, int backlog);

/* Accept an incoming connection (TCP only, blocks). */
int socket_accept(Socket *sk, SockAddrIn *addr_out);

/* Connect to a remote address. */
int socket_connect(Socket *sk, uint32_t ip, uint16_t port);

/* Send data on a connected socket. */
int socket_send(Socket *sk, const void *buf, uint32_t len);

/* Receive data from a socket. */
int socket_recv(Socket *sk, void *buf, uint32_t len);

/* Send datagram (UDP, with destination). */
int socket_sendto(Socket *sk, const void *buf, uint32_t len,
                  uint32_t dst_ip, uint16_t dst_port);

/* Receive datagram (UDP, returns sender info). */
int socket_recvfrom(Socket *sk, void *buf, uint32_t len,
                    uint32_t *src_ip, uint16_t *src_port);

/* Deliver received UDP data to the matching socket. Called from udp_rx. */
void socket_udp_deliver(uint32_t src_ip, uint16_t src_port,
                        uint32_t dst_ip, uint16_t dst_port,
                        const void *data, uint32_t len);

/* Deliver received TCP segment to the matching socket. Called from tcp_rx. */
void socket_tcp_deliver(uint32_t src_ip, uint16_t src_port,
                        uint32_t dst_ip, uint16_t dst_port,
                        const void *segment, uint32_t seg_len);

/* Allocate an ephemeral port (49152-65535). Returns port in network byte order. */
uint16_t socket_alloc_ephemeral_port(void);

#endif /* ARCHOS_NET_SOCKET_H */
