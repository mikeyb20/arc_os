#ifndef ARCHOS_UAPI_TIME_H
#define ARCHOS_UAPI_TIME_H

#include <stdint.h>

/* Linux x86_64 time ABI (ADR-0001): include/uapi/linux/time.h,
 * include/uapi/linux/time_types.h. */

/* Linux x86_64 `struct timespec` (ADR-0001). */
struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

/* `struct timeval` (gettimeofday): microseconds */
struct timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

/* `struct timezone` (gettimeofday's second argument; always zeros here) */
struct timezone {
    int32_t tz_minuteswest;
    int32_t tz_dsttime;
};

_Static_assert(sizeof(struct timespec) == 16, "struct timespec must be 16 bytes");
_Static_assert(__builtin_offsetof(struct timespec, tv_nsec) == 8, "tv_nsec offset");
_Static_assert(sizeof(struct timeval) == 16, "struct timeval must be 16 bytes");
_Static_assert(__builtin_offsetof(struct timeval, tv_usec) == 8, "tv_usec offset");
_Static_assert(sizeof(struct timezone) == 8, "struct timezone must be 8 bytes");

/* Clock IDs */
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7

/* clock_nanosleep flags */
#define TIMER_ABSTIME 0x01

#endif /* ARCHOS_UAPI_TIME_H */
