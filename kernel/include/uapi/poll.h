#ifndef ARCHOS_UAPI_POLL_H
#define ARCHOS_UAPI_POLL_H

#include <stdint.h>

/* Linux `struct pollfd` and event bits (poll, ppoll; ADR-0001). */
struct pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

_Static_assert(sizeof(struct pollfd) == 8, "struct pollfd must be 8 bytes");
_Static_assert(__builtin_offsetof(struct pollfd, revents) == 6, "revents offset");

#define POLLIN   0x001
#define POLLPRI  0x002
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

#endif /* ARCHOS_UAPI_POLL_H */
