#ifndef ARCHOS_LIBC_TIME_H
#define ARCHOS_LIBC_TIME_H

#include <stdint.h>
#include <sys/types.h>

/* struct timespec, CLOCK_* IDs and TIMER_ABSTIME (Linux x86_64, ADR-0001) */
#include <uapi/time.h>

typedef int64_t time_t;
typedef int32_t clockid_t;

/* Read clock id into *ts. 0, or -1 with errno (EINVAL, EFAULT). */
int clock_gettime(clockid_t id, struct timespec *ts);

/* Resolution of clock id (res may be NULL). 0, or -1 with errno. */
int clock_getres(clockid_t id, struct timespec *res);

/* Sleep for *req; if interrupted, the time left goes to *rem (may be NULL).
 * 0, or -1 with errno (EINTR, EINVAL, EFAULT). */
int nanosleep(const struct timespec *req, struct timespec *rem);

/* Sleep on clock id until *req (flags TIMER_ABSTIME) or for *req. Returns 0
 * or the error number itself (POSIX), not -1. */
int clock_nanosleep(clockid_t id, int flags, const struct timespec *req, struct timespec *rem);

/* Seconds since 1970-01-01 UTC, also stored in *t when t is not NULL. */
time_t time(time_t *t);

#endif /* ARCHOS_LIBC_TIME_H */
