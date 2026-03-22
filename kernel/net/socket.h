#ifndef ARCHOS_NET_SOCKET_H
#define ARCHOS_NET_SOCKET_H

#include <stdint.h>
#include "fs/vfs.h"
#include "proc/waitqueue.h"

/* Address families */
#define AF_INET  2

/* Socket types */
#define SOCK_DGRAM 2

/* Protocols */
#define IPPROTO_UDP 17

/* Special addresses */
#define INADDR_ANY       0
#define INADDR_LOOPBACK  0x0100007F  /* 127.0.0.1 in network byte order */

/* Ephemeral port range */
#define EPHEMERAL_PORT_MIN  49152
#define EPHEMERAL_PORT_MAX  65535

/* Socket pool size */
#define SOCKET_MAX  32

/* Receive buffer limits */
#define SOCK_RXBUF_SIZE    4096
#define SOCK_MAX_DATAGRAMS 16

/* BSD sockaddr_in */
typedef struct {
    uint16_t sin_family;    /* AF_INET */
    uint16_t sin_port;      /* network byte order */
    uint32_t sin_addr;      /* network byte order */
    uint8_t  sin_zero[8];   /* padding to 16 bytes */
} sockaddr_in;

/* Per-datagram metadata in receive queue */
typedef struct {
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t len;
    uint16_t offset;   /* offset into rxbuf */
} SockDatagram;

/* Socket structure */
typedef struct Socket {
    uint8_t        in_use;
    uint8_t        domain;       /* AF_INET */
    uint8_t        type;         /* SOCK_DGRAM */
    uint8_t        protocol;     /* IPPROTO_UDP */

    uint32_t       local_ip;     /* bound IP (0 = INADDR_ANY) */
    uint16_t       local_port;   /* bound port, network byte order */
    uint32_t       remote_ip;    /* connect target (0 = unconnected) */
    uint16_t       remote_port;  /* connect target port */

    /* Datagram receive queue */
    uint8_t        rxbuf[SOCK_RXBUF_SIZE];
    uint32_t       rx_head;         /* write offset in rxbuf */
    uint32_t       rx_used;         /* bytes used in rxbuf */
    SockDatagram   rx_dgrams[SOCK_MAX_DATAGRAMS];
    uint32_t       rx_dgram_head;   /* next write slot */
    uint32_t       rx_dgram_tail;   /* next read slot */
    uint32_t       rx_dgram_count;  /* queued datagrams */

    /* VFS integration */
    VfsNode        vnode;
    VfsOps         ops;

    /* Synchronization */
    Spinlock       lock;
    WaitQueue      recv_wq;

    uint32_t       ref_count;
} Socket;

/* Initialize socket subsystem (called from net_init). */
void socket_init(void);

/* Create a socket. Returns Socket* or NULL. */
Socket *socket_create(int domain, int type, int protocol);

/* Bind socket to local address+port. Returns 0 or -errno. */
int socket_bind(Socket *sock, const sockaddr_in *addr);

/* Send a datagram. Returns bytes sent or -errno. */
int socket_sendto(Socket *sock, const void *buf, uint32_t len,
                  const sockaddr_in *dest_addr);

/* Receive a datagram (blocks if empty). Returns bytes received or -errno. */
int socket_recvfrom(Socket *sock, void *buf, uint32_t len,
                    sockaddr_in *src_addr);

/* Close socket (decrements ref, frees at 0). */
void socket_close(Socket *sock);

/* Add reference (for dup/fork). */
void socket_addref(Socket *sock);

/* Deliver a UDP datagram to matching socket. Returns 0 or -1. */
int socket_udp_deliver(uint32_t src_ip, uint16_t src_port,
                       uint32_t dst_ip, uint16_t dst_port,
                       const void *data, uint32_t len);

/* Get Socket* from a VfsNode (vnode.private_data). */
Socket *socket_from_vnode(VfsNode *node);

#endif /* ARCHOS_NET_SOCKET_H */
