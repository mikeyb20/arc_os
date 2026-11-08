#ifndef ARCHOS_LIBC_IOCTL_H
#define ARCHOS_LIBC_IOCTL_H

/* Terminal ioctl requests and structs (Linux values, ADR-0001) */
#include <uapi/termios.h>

/* Device control. Returns the request's result, or -1 with errno set. */
int ioctl(int fd, unsigned long request, ...);

#endif /* ARCHOS_LIBC_IOCTL_H */
