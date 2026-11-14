/* arc_os libc — clock_gettime/clock_getres/nanosleep/clock_nanosleep/
 * gettimeofday/time (M1-06) */

#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include "syscall_arch.h"

int clock_gettime(clockid_t id, struct timespec *ts) {
    return (int)syscall_ret(syscall2(__NR_clock_gettime, (uint64_t)(int64_t)id, (uint64_t)ts));
}

int clock_getres(clockid_t id, struct timespec *res) {
    return (int)syscall_ret(syscall2(__NR_clock_getres, (uint64_t)(int64_t)id, (uint64_t)res));
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
    return (int)syscall_ret(syscall2(__NR_nanosleep, (uint64_t)req, (uint64_t)rem));
}

int clock_nanosleep(clockid_t id, int flags, const struct timespec *req, struct timespec *rem) {
    int64_t ret =
        syscall4(__NR_clock_nanosleep, (uint64_t)(int64_t)id, (uint64_t)(int64_t)flags, (uint64_t)req, (uint64_t)rem);
    return ret < 0 ? (int)-ret : 0;   /* POSIX: the error number, errno untouched */
}

int gettimeofday(struct timeval *tv, struct timezone *tz) {
    return (int)syscall_ret(syscall2(__NR_gettimeofday, (uint64_t)tv, (uint64_t)tz));
}

time_t time(time_t *t) {
    return (time_t)syscall_ret(syscall1(__NR_time, (uint64_t)t));
}
