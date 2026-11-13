/* arc_os — socket syscalls (Linux x86_64 numbers, ADR-0001).
 *
 * Moved out of arch/x86_64/syscall.c by M0-05. AF_INET only; the old send/recv
 * syscalls are sendto/recvfrom with a NULL address. A socket fd has no VfsNode:
 * its VfsFile stores the socket index in offset and VFS_SOCKET in flags. */

#include "net/net_syscalls.h"
#include "arch/hal.h"
#include "net/socket.h"
#include "proc/process.h"
#include "proc/fd.h"
#include "fs/vfs.h"
#include "mm/kmalloc.h"
#include "user_access.h"
#include "uapi/unistd.h"
#include "uapi/socket.h"

/* socket() type flags arc_os accepts and ignores (no non-blocking I/O yet) */
#define SOCK_TYPE_MASK 0xF

/* send/recv move at most this much per call through a kernel buffer (a
 * short transfer, which both may return) */
#define SOCK_IO_MAX (64u * 1024)

/* Socket behind fd, or NULL if fd is not a socket. */
static Socket *fd_to_socket(int fd) {
    Process *p = proc_current();
    if (p == NULL || p->fd_table == NULL) return NULL;
    VfsFile *f = fd_get(p->fd_table, fd);
    if (f == NULL || f->node != NULL || f->flags != VFS_SOCKET) return NULL;
    return socket_get((int)f->offset);
}

/* Read a user sockaddr_in. Returns 0, -EFAULT, -EINVAL (short) or -EAFNOSUPPORT. */
static int sockaddr_in_from_user(uint64_t addr_ptr, uint64_t addrlen, struct sockaddr_in *out) {
    if (addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
    if (copy_from_user(out, (const void *)addr_ptr, sizeof(*out)) != 0) return -EFAULT;
    if (out->sin_family != AF_INET) return -EAFNOSUPPORT;
    return 0;
}

/* Write a sockaddr_in to (addr_ptr, *addrlen_ptr) the Linux way: copy at most
 * *addrlen bytes, then store the full length. No-op when addr_ptr is 0. */
static int sockaddr_in_to_user(uint64_t addr_ptr, uint64_t addrlen_ptr,
                               uint32_t ip, uint16_t port) {
    if (addr_ptr == 0) return 0;
    uint32_t len;
    if (get_user(len, addrlen_ptr) != 0) return -EFAULT;
    if ((int32_t)len < 0) return -EINVAL;

    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = port, .sin_addr = { ip } };
    uint32_t n = len < sizeof(sa) ? len : (uint32_t)sizeof(sa);
    if (n > 0 && copy_to_user((void *)addr_ptr, &sa, n) != 0) return -EFAULT;
    return put_user((uint32_t)sizeof(sa), addrlen_ptr);
}

/* Put socket index sk_idx in a new fd. Frees the socket on failure. */
static int64_t socket_install_fd(Process *p, int sk_idx) {
    int fd = fd_alloc(p->fd_table);
    if (fd < 0) { socket_free(sk_idx); return -EMFILE; }
    VfsFile *f = fd_get(p->fd_table, fd);
    f->node = NULL;
    f->offset = (uint64_t)sk_idx;
    f->flags = VFS_SOCKET;
    return fd;
}

/* socket(domain, type, protocol) — 41 */
static int64_t sys_socket(uint64_t family, uint64_t type, uint64_t protocol,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    Process *p = proc_current();
    if (p == NULL || p->fd_table == NULL) return -EBADF;

    int sk_idx = socket_create((uint16_t)family, (uint16_t)(type & SOCK_TYPE_MASK), (uint16_t)protocol);
    if (sk_idx < 0) return sk_idx;
    return socket_install_fd(p, sk_idx);
}

/* connect(fd, addr, addrlen) — 42 */
static int64_t sys_connect(uint64_t fd, uint64_t addr_ptr, uint64_t addrlen,
                           uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    Socket *sk = fd_to_socket((int)fd);
    if (sk == NULL) return -EBADF;
    struct sockaddr_in sa;
    int err = sockaddr_in_from_user(addr_ptr, addrlen, &sa);
    if (err != 0) return err;
    return socket_connect(sk, sa.sin_addr.s_addr, sa.sin_port);
}

/* accept(fd, addr, addrlen) — 43 */
static int64_t sys_accept(uint64_t fd, uint64_t addr_ptr, uint64_t addrlen_ptr,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    Socket *sk = fd_to_socket((int)fd);
    if (sk == NULL) return -EBADF;
    if (addr_ptr != 0 && !user_ptr_valid((void *)addrlen_ptr, sizeof(uint32_t))) return -EFAULT;

    SockAddrIn peer;
    int child_idx = socket_accept(sk, &peer);
    if (child_idx < 0) return child_idx;

    int64_t new_fd = socket_install_fd(proc_current(), child_idx);
    if (new_fd < 0) return new_fd;
    sockaddr_in_to_user(addr_ptr, addrlen_ptr, peer.sin_addr, peer.sin_port);
    return new_fd;
}

/* sendto(fd, buf, len, flags, dest_addr, addrlen) — 44. A NULL dest_addr sends
 * on the connected socket (the old send syscall). flags are ignored. */
static int64_t sys_sendto(uint64_t fd, uint64_t buf_addr, uint64_t len,
                          uint64_t flags, uint64_t addr_ptr, uint64_t addrlen) {
    (void)flags;
    Socket *sk = fd_to_socket((int)fd);
    if (sk == NULL) return -EBADF;
    if (len == 0) return 0;
    if (!user_ptr_valid((void *)buf_addr, len)) return -EFAULT;

    struct sockaddr_in sa;
    if (addr_ptr != 0) {
        int err = sockaddr_in_from_user(addr_ptr, addrlen, &sa);
        if (err != 0) return err;
    }
    uint32_t n = len < SOCK_IO_MAX ? (uint32_t)len : SOCK_IO_MAX;
    uint8_t *kbuf = kmalloc(n, 0);
    if (kbuf == NULL) return -ENOMEM;
    int64_t ret;
    if (copy_from_user(kbuf, (const void *)buf_addr, n) != 0)
        ret = -EFAULT;
    else if (addr_ptr == 0)
        ret = socket_send(sk, kbuf, n);
    else
        ret = socket_sendto(sk, kbuf, n, sa.sin_addr.s_addr, sa.sin_port);
    kfree(kbuf);
    return ret;
}

/* recvfrom(fd, buf, len, flags, src_addr, addrlen) — 45. A NULL src_addr
 * receives on the connected socket (the old recv syscall). flags are ignored. */
static int64_t sys_recvfrom(uint64_t fd, uint64_t buf_addr, uint64_t len,
                            uint64_t flags, uint64_t addr_ptr, uint64_t addrlen_ptr) {
    (void)flags;
    Socket *sk = fd_to_socket((int)fd);
    if (sk == NULL) return -EBADF;
    if (len == 0) return 0;
    if (!user_ptr_valid((void *)buf_addr, len)) return -EFAULT;
    if (addr_ptr != 0 && !user_ptr_valid((void *)addrlen_ptr, sizeof(uint32_t))) return -EFAULT;

    uint32_t n = len < SOCK_IO_MAX ? (uint32_t)len : SOCK_IO_MAX;
    uint8_t *kbuf = kmalloc(n, 0);
    if (kbuf == NULL) return -ENOMEM;
    uint32_t ip = 0;
    uint16_t port = 0;
    int ret = addr_ptr == 0 ? socket_recv(sk, kbuf, n) : socket_recvfrom(sk, kbuf, n, &ip, &port);
    if (ret > 0 && copy_to_user((void *)buf_addr, kbuf, (size_t)ret) != 0) ret = -EFAULT;
    kfree(kbuf);
    if (ret >= 0 && addr_ptr != 0 && sockaddr_in_to_user(addr_ptr, addrlen_ptr, ip, port) != 0) return -EFAULT;
    return ret;
}

/* bind(fd, addr, addrlen) — 49 */
static int64_t sys_bind(uint64_t fd, uint64_t addr_ptr, uint64_t addrlen,
                        uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a3; (void)a4; (void)a5;
    Socket *sk = fd_to_socket((int)fd);
    if (sk == NULL) return -EBADF;
    struct sockaddr_in sa;
    int err = sockaddr_in_from_user(addr_ptr, addrlen, &sa);
    if (err != 0) return err;
    return socket_bind(sk, sa.sin_addr.s_addr, sa.sin_port);
}

/* listen(fd, backlog) — 50 */
static int64_t sys_listen(uint64_t fd, uint64_t backlog, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    Socket *sk = fd_to_socket((int)fd);
    if (sk == NULL) return -EBADF;
    return socket_listen(sk, (int)backlog);
}

void net_syscalls_init(void) {
    syscall_register(__NR_socket,   sys_socket);
    syscall_register(__NR_connect,  sys_connect);
    syscall_register(__NR_accept,   sys_accept);
    syscall_register(__NR_sendto,   sys_sendto);
    syscall_register(__NR_recvfrom, sys_recvfrom);
    syscall_register(__NR_bind,     sys_bind);
    syscall_register(__NR_listen,   sys_listen);
}
