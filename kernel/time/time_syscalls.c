/* arc_os — Time syscalls (M1-06, Linux x86_64 ABI, ADR-0001)
 *
 * Owns: clock_gettime (228), clock_getres (229), clock_nanosleep (230),
 * nanosleep (35), gettimeofday (96) and time (201). Semantics follow the
 * Linux man pages (section 2): the clock IDs of time/clock.c, -EINVAL for an
 * unknown clock or a bad timespec, -EFAULT for a bad pointer, and the
 * remaining time written only when a sleep is interrupted (signals interrupt
 * sleeps from M1-17; until then a sleep always runs to its end).
 * Locks: none of their own; a sleep gives up the kernel lock while it waits. */

#include "time/time_syscalls.h"
#include "time/clock.h"
#include "time/timer.h"
#include "proc/signal.h"
#include "arch/hal.h"
#include "user_access.h"
#include "uapi/unistd.h"
#include "uapi/errno.h"

/* Copy a timespec in and check it */
static int get_timespec(const void *uptr, uint64_t *ns) {
    struct timespec ts;
    if (uptr == NULL || copy_from_user(&ts, uptr, sizeof(ts)) != 0) return -EFAULT;
    return clock_timespec_to_ns(&ts, ns);
}

static int put_timespec(void *uptr, uint64_t ns) {
    struct timespec ts;
    clock_ns_to_timespec(ns, &ts);
    return copy_to_user(uptr, &ts, sizeof(ts)) != 0 ? -EFAULT : 0;
}

static int64_t sys_clock_gettime(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2, (void)a3, (void)a4, (void)a5;
    uint64_t ns;
    int rc = clock_read((int)a0, &ns);
    if (rc != 0) return rc;
    return put_timespec((void *)a1, ns);
}

static int64_t sys_clock_getres(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2, (void)a3, (void)a4, (void)a5;
    uint64_t ns;
    int rc = clock_resolution((int)a0, &ns);
    if (rc != 0) return rc;
    return a1 != 0 ? put_timespec((void *)a1, ns) : 0;   /* res may be NULL */
}

/* Sleep until the CLOCK_MONOTONIC deadline. Woken early: -EINTR with the
 * time left in *left (only signals can do that, from M1-17). */
static int sleep_until(uint64_t deadline, uint64_t *left) {
    while (timer_sleep_until(deadline) != 0) {
        if (sig_work_pending()) {
            uint64_t now = clock_mono_ns();
            *left = deadline > now ? deadline - now : 0;
            return -EINTR;
        }
    }
    return 0;
}

/* Relative sleep: deadline = now + ns, saturating */
static uint64_t deadline_after(uint64_t ns) {
    uint64_t now = clock_mono_ns();
    return ns > UINT64_MAX - now ? UINT64_MAX : now + ns;
}

static int64_t sys_nanosleep(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2, (void)a3, (void)a4, (void)a5;
    uint64_t ns, left;
    int rc = get_timespec((const void *)a0, &ns);
    if (rc != 0) return rc;
    rc = sleep_until(deadline_after(ns), &left);
    if (rc == -EINTR && a1 != 0 && put_timespec((void *)a1, left) != 0) return -EFAULT;
    return rc;
}

static int64_t sys_clock_nanosleep(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a4, (void)a5;
    int id = (int)a0, flags = (int)a1;
    switch (id) {
    case CLOCK_REALTIME:
    case CLOCK_MONOTONIC:
    case CLOCK_BOOTTIME:
        break;
    case CLOCK_PROCESS_CPUTIME_ID:
        return -EOPNOTSUPP;   /* Linux sleeps on it; no CPU-time timers here */
    default:
        return -EINVAL;       /* THREAD_CPUTIME_ID, the COARSE and RAW clocks, unknown IDs */
    }
    if (flags & ~TIMER_ABSTIME) return -EINVAL;
    uint64_t ns, left;
    int rc = get_timespec((const void *)a2, &ns);
    if (rc != 0) return rc;
    uint64_t deadline = (flags & TIMER_ABSTIME) ? clock_to_mono(id, ns) : deadline_after(ns);
    rc = sleep_until(deadline, &left);
    /* The time left is reported for relative sleeps only */
    if (rc == -EINTR && !(flags & TIMER_ABSTIME) && a3 != 0 && put_timespec((void *)a3, left) != 0) return -EFAULT;
    return rc;
}

static int64_t sys_gettimeofday(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2, (void)a3, (void)a4, (void)a5;
    if (a0 != 0) {
        uint64_t ns = clock_real_ns();
        struct timeval tv = {.tv_sec = (int64_t)(ns / NSEC_PER_SEC),
                             .tv_usec = (int64_t)(ns % NSEC_PER_SEC / NSEC_PER_USEC)};
        if (copy_to_user((void *)a0, &tv, sizeof(tv)) != 0) return -EFAULT;
    }
    if (a1 != 0) {
        struct timezone tz = {0, 0};   /* UTC, no daylight saving: what Linux reports by default */
        if (copy_to_user((void *)a1, &tz, sizeof(tz)) != 0) return -EFAULT;
    }
    return 0;
}

static int64_t sys_time(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a1, (void)a2, (void)a3, (void)a4, (void)a5;
    int64_t secs = (int64_t)(clock_real_ns() / NSEC_PER_SEC);
    if (a0 != 0 && copy_to_user((void *)a0, &secs, sizeof(secs)) != 0) return -EFAULT;
    return secs;
}

void time_syscalls_init(void) {
    syscall_register(__NR_clock_gettime, sys_clock_gettime);
    syscall_register(__NR_clock_getres, sys_clock_getres);
    syscall_register(__NR_clock_nanosleep, sys_clock_nanosleep);
    syscall_register(__NR_nanosleep, sys_nanosleep);
    syscall_register(__NR_gettimeofday, sys_gettimeofday);
    syscall_register(__NR_time, sys_time);
}
