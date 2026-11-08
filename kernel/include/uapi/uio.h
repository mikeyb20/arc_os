#ifndef ARCHOS_UAPI_UIO_H
#define ARCHOS_UAPI_UIO_H

#include <stddef.h>

/* Linux `struct iovec` (readv, writev; ADR-0001). */
struct iovec {
    void  *iov_base;
    size_t iov_len;
};

_Static_assert(sizeof(struct iovec) == 16, "struct iovec must be 16 bytes");
_Static_assert(__builtin_offsetof(struct iovec, iov_len) == 8, "iov_len offset");

#endif /* ARCHOS_UAPI_UIO_H */
