#ifndef ARCHOS_UAPI_TIME_H
#define ARCHOS_UAPI_TIME_H

#include <stdint.h>

/* Linux x86_64 `struct timespec` (ADR-0001). */
struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

_Static_assert(sizeof(struct timespec) == 16, "struct timespec must be 16 bytes");
_Static_assert(__builtin_offsetof(struct timespec, tv_nsec) == 8, "tv_nsec offset");

#endif /* ARCHOS_UAPI_TIME_H */
