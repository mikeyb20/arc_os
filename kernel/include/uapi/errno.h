#ifndef ARCHOS_UAPI_ERRNO_H
#define ARCHOS_UAPI_ERRNO_H

/* Linux errno values (ADR-0001), shared by the kernel and libarc.
 * Syscalls return -errno in [-4095, -1]. tests/test_abi_layout.c checks every
 * value against the host's <asm-generic/errno*.h>. */

#define EPERM            1
#define ENOENT           2
#define ESRCH            3
#define EINTR            4
#define EIO              5
#define ENXIO            6
#define E2BIG            7
#define ENOEXEC          8
#define EBADF            9
#define ECHILD          10
#define EAGAIN          11
#define ENOMEM          12
#define EACCES          13
#define EFAULT          14
#define EBUSY           16
#define EEXIST          17
#define EXDEV           18
#define ENODEV          19
#define ENOTDIR         20
#define EISDIR          21
#define EINVAL          22
#define ENFILE          23
#define EMFILE          24
#define ENOTTY          25
#define EFBIG           27
#define ENOSPC          28
#define ESPIPE          29
#define EROFS           30
#define EMLINK          31
#define EPIPE           32
#define ERANGE          34
#define EDEADLK         35
#define ENAMETOOLONG    36
#define ENOLCK          37
#define ENOSYS          38
#define ENOTEMPTY       39
#define ELOOP           40
#define EWOULDBLOCK     EAGAIN
#define ENOTSOCK        88
#define EDESTADDRREQ    89
#define EMSGSIZE        90
#define EPROTOTYPE      91
#define EPROTONOSUPPORT 93
#define EOPNOTSUPP      95
#define EAFNOSUPPORT    97
#define EADDRINUSE      98
#define EADDRNOTAVAIL   99
#define ENETUNREACH    101
#define ECONNRESET     104
#define ENOBUFS        105
#define EISCONN        106
#define ENOTCONN       107
#define ETIMEDOUT      110
#define ECONNREFUSED   111

/* Largest errno a syscall may return (as -errno): [-4095, -1] is an error. */
#define ARC_MAX_ERRNO 4095

#endif /* ARCHOS_UAPI_ERRNO_H */
