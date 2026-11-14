#ifndef ARCHOS_TIME_CLOCK_H
#define ARCHOS_TIME_CLOCK_H

#include <stdint.h>
#include "uapi/time.h"
#include "uapi/errno.h"

/* Kernel clocks (M1-06): monotonic time from the TSC clocksource, the wall
 * clock (CMOS RTC at boot + monotonic time), and the clock IDs of the Linux
 * clock_* syscalls. */

#define NSEC_PER_SEC  ((uint64_t)1000000000)
#define NSEC_PER_MSEC ((uint64_t)1000000)
#define NSEC_PER_USEC ((uint64_t)1000)

/* CLOCK_MONOTONIC: nanoseconds since boot. Never goes backwards, on any CPU
 * and between CPUs. Any context. */
uint64_t clock_mono_ns(void);

/* CLOCK_REALTIME: nanoseconds since 1970-01-01 00:00 UTC. */
uint64_t clock_real_ns(void);

/* Read clock id (CLOCK_*) into *ns. Returns 0 or -EINVAL. */
int clock_read(int id, uint64_t *ns);

/* Resolution of clock id. Returns 0 or -EINVAL. */
int clock_resolution(int id, uint64_t *ns);

/* Turn a monotonic deadline for clock id into a CLOCK_MONOTONIC deadline:
 * the wall clock subtracts its offset (a wall time before boot is 0).
 * id must be REALTIME, MONOTONIC or BOOTTIME (and their variants). */
uint64_t clock_to_mono(int id, uint64_t ns);

/* Seconds since 1970 of a UTC calendar date (proleptic Gregorian). */
int64_t clock_civil_to_unix(int64_t year, unsigned month, unsigned day, unsigned hour, unsigned minute,
                            unsigned second);

/* ns -> timespec. */
static inline void clock_ns_to_timespec(uint64_t ns, struct timespec *ts) {
    ts->tv_sec = (int64_t)(ns / NSEC_PER_SEC);
    ts->tv_nsec = (int64_t)(ns % NSEC_PER_SEC);
}

/* timespec -> ns: -EINVAL when tv_nsec is outside [0, 10^9) or tv_sec < 0,
 * else 0 with the value saturated at UINT64_MAX. */
static inline int clock_timespec_to_ns(const struct timespec *ts, uint64_t *ns) {
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || (uint64_t)ts->tv_nsec >= NSEC_PER_SEC) return -EINVAL;
    uint64_t sec = (uint64_t)ts->tv_sec;
    if (sec > (UINT64_MAX - (uint64_t)ts->tv_nsec) / NSEC_PER_SEC)
        *ns = UINT64_MAX;
    else
        *ns = sec * NSEC_PER_SEC + (uint64_t)ts->tv_nsec;
    return 0;
}

/* Busy-wait ns nanoseconds on the clock (boot, diagnostics and panic paths
 * only: everything else sleeps or arms a timer). */
void clock_spin_ns(uint64_t ns);

/* Boot, BSP, after the APs are up and before devices: finish the TSC
 * calibration (HPET), check the TSCs are in step, start every CPU's timer
 * queue, silence the PIT and read the wall clock from the RTC. */
void clock_init(void);

/* What clock_init found, for the boot report and the self-test. */
typedef struct {
    const char *event_mode;   /* "TSC-deadline" or "LAPIC one-shot" */
    int rtc_ok;               /* the RTC gave a valid date */
    int clamped;              /* TSCs out of step: reads are kept monotonic by a shared maximum */
    uint64_t boot_real_ns;    /* wall clock when the RTC was read */
} ClockInfo;
const ClockInfo *clock_info(void);

#endif /* ARCHOS_TIME_CLOCK_H */
