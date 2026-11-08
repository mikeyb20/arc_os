#ifndef ARCHOS_NET_NET_SYSCALLS_H
#define ARCHOS_NET_NET_SYSCALLS_H

/* Register the socket syscalls (socket, connect, accept, sendto, recvfrom,
 * bind, listen) under their Linux numbers. Called by syscall_init. */
void net_syscalls_init(void);

#endif /* ARCHOS_NET_NET_SYSCALLS_H */
