#ifndef ARCHOS_LIBC_FCNTL_H
#define ARCHOS_LIBC_FCNTL_H

/* Open flags: the Linux values the kernel takes (ADR-0001) */
#include <uapi/fcntl.h>

int open(const char *path, int flags, ...);

#endif /* ARCHOS_LIBC_FCNTL_H */
