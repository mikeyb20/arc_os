/* arc_os — Kernel clocks (M1-06)
 *
 * Owns: CLOCK_MONOTONIC (the TSC clocksource, kept monotonic across CPUs),
 * the wall-clock offset (CMOS RTC read once at boot, UTC), the Linux clock IDs
 * and their resolution, and the order of the clock bring-up.
 *
 * Invariants: the wall-clock offset is written once at boot and never changes
 * (no settimeofday yet), so CLOCK_REALTIME = CLOCK_MONOTONIC + offset moves
 * forward with it. If the boot check finds the CPUs' TSCs out of step,
 * every read goes through mono_last (a shared maximum): slower, but never
 * backwards.
 *
 * Locks: none; mono_last is one atomic word.
 * Cost: a monotonic read is one ordered TSC read and a multiply; with the
 * clamp, one compare-and-swap on a shared cache line as well. */

#include "time/clock.h"
#include "time/timer.h"
#include "proc/sched.h"
#include "proc/process.h"
#include "proc/thread.h"
#include "drivers/acpi.h"
#include "arch/hal.h"
#include "lib/kprintf.h"
#include "lib/cmdline.h"
#include "lib/testmode.h"
#include "lib/string.h"
#include "uapi/errno.h"

#define CLOCK_SYNC_CHECK_MS 20
#define CLOCK_RTC_WAIT_NS   (2 * NSEC_PER_SEC)

static ClockInfo info = {.event_mode = "none"};
static int64_t real_offset;   /* CLOCK_REALTIME - CLOCK_MONOTONIC, ns */
static int mono_clamp;        /* TSCs out of step: keep a shared maximum */
static uint64_t mono_last;

uint64_t clock_mono_ns(void) {
    uint64_t t = hal_clock_ns();
    if (!__atomic_load_n(&mono_clamp, __ATOMIC_RELAXED)) return t;
    uint64_t last = __atomic_load_n(&mono_last, __ATOMIC_RELAXED);
    do {
        if (t <= last) return last;
    } while (!__atomic_compare_exchange_n(&mono_last, &last, t, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED));
    return t;
}

uint64_t clock_real_ns(void) {
    return (uint64_t)((int64_t)clock_mono_ns() + real_offset);
}

int clock_read(int id, uint64_t *ns) {
    switch (id) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
        *ns = clock_real_ns();
        return 0;
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:   /* no frequency correction is ever applied */
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_BOOTTIME:        /* no system suspend (ADR-0017): same as monotonic */
        *ns = clock_mono_ns();
        return 0;
    case CLOCK_THREAD_CPUTIME_ID:
        *ns = sched_thread_cpu_ns(thread_current());
        return 0;
    case CLOCK_PROCESS_CPUTIME_ID: {
        /* One thread per process until M1-16 adds clone threads, which sums them */
        Process *p = proc_current();
        *ns = sched_thread_cpu_ns(p != NULL && p->main_thread != NULL ? p->main_thread : thread_current());
        return 0;
    }
    default:
        return -EINVAL;
    }
}

int clock_resolution(int id, uint64_t *ns) {
    uint64_t dummy;
    if (id == CLOCK_THREAD_CPUTIME_ID || id == CLOCK_PROCESS_CPUTIME_ID) {
        *ns = 1;
        return 0;
    }
    if (clock_read(id, &dummy) != 0) return -EINVAL;
    *ns = 1;   /* the TSC ticks faster than 1 GHz on every CPU in scope */
    return 0;
}

uint64_t clock_to_mono(int id, uint64_t ns) {
    if (id != CLOCK_REALTIME && id != CLOCK_REALTIME_COARSE) return ns;
    if (real_offset >= 0) return ns > (uint64_t)real_offset ? ns - (uint64_t)real_offset : 0;
    uint64_t back = (uint64_t)-real_offset;
    return ns > UINT64_MAX - back ? UINT64_MAX : ns + back;
}

int64_t clock_civil_to_unix(int64_t year, unsigned month, unsigned day, unsigned hour, unsigned minute,
                            unsigned second) {
    /* Days from 1970-01-01 (H. Hinnant's days_from_civil): years start in
     * March so the leap day is the last day of the year */
    year -= month <= 2;
    int64_t era = (year >= 0 ? year : year - 399) / 400;
    int64_t yoe = year - era * 400;                                               /* [0, 399] */
    int64_t doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;  /* [0, 365] */
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                          /* [0, 146096] */
    int64_t days = era * 146097 + doe - 719468;
    return days * 86400 + hour * 3600 + minute * 60 + second;
}

void clock_spin_ns(uint64_t ns) {
    uint64_t end = clock_mono_ns() + ns;
    while (clock_mono_ns() < end) hal_cpu_relax();
}

const ClockInfo *clock_info(void) {
    return &info;
}

/* --- Bring-up --- */

static void clock_check_sync(void) {
    uint64_t warp = hal_clock_check_sync(CLOCK_SYNC_CHECK_MS);
    uint32_t cpus = hal_tsc_info()->sync_cpus;
    if (warp == 0) {
        kprintf("[CLOCK] TSC in step on %u CPUs (sync check %u ms)\n", cpus, CLOCK_SYNC_CHECK_MS);
        return;
    }
    /* Firmware left the TSCs apart (or an emulator drifts): keep reads monotonic */
    mono_last = hal_clock_ns();
    __atomic_store_n(&mono_clamp, 1, __ATOMIC_RELEASE);
    info.clamped = 1;
    kprintf("[CLOCK] WARNING: TSCs out of step on %u CPUs: up to %lu cycles (%lu ns) back; clock reads are "
            "clamped to stay monotonic\n",
            cpus, warp, hal_tsc_to_ns(warp));
}

static void clock_read_rtc(void) {
    const AcpiInfo *acpi = acpi_get_info();
    uint8_t century = acpi != NULL ? acpi->century_reg : 0;
    RtcTime rtc;
    uint64_t mono = clock_mono_ns();
    if (hal_rtc_read(century, hal_clock_ns_to_cycles(CLOCK_RTC_WAIT_NS), &rtc) != 0) {
        kprintf("[CLOCK] WARNING: the RTC gave no valid date; the wall clock starts at 1970\n");
        return;
    }
    int64_t secs = clock_civil_to_unix(rtc.year, rtc.month, rtc.day, rtc.hour, rtc.minute, rtc.second);
    real_offset = secs * (int64_t)NSEC_PER_SEC - (int64_t)mono;
    info.rtc_ok = 1;
    info.boot_real_ns = (uint64_t)(secs * (int64_t)NSEC_PER_SEC);
    kprintf("[CLOCK] Wall clock %04u-%02u-%02u %02u:%02u:%02u UTC from the RTC\n", rtc.year, rtc.month, rtc.day,
            rtc.hour, rtc.minute, rtc.second);
}

void clock_init(void) {
    const AcpiInfo *acpi = acpi_get_info();
    /* Test mode can force the HPET reference (QEMU's PIT always works) */
    char cal[8];
    int force_hpet =
        testmode_active() && cmdline_get(cmdline_raw(), "arc.tsccal", cal, sizeof(cal)) && strcmp(cal, "hpet") == 0;
    hal_clock_calibrate_late(acpi != NULL ? acpi->hpet_address : 0, force_hpet);
    clock_check_sync();
    /* arc.timer=oneshot: LAPIC one-shot even where TSC-deadline exists (a
     * fallback for CPUs whose deadline timer misbehaves, and for tests) */
    char mode[12];
    int one_shot = cmdline_get(cmdline_raw(), "arc.timer", mode, sizeof(mode)) && strcmp(mode, "oneshot") == 0;
    timer_init_boot(one_shot);
    timer_init_cpus();
    hal_legacy_timer_stop();
    info.event_mode = hal_clockevent_mode();
    clock_read_rtc();
    kprintf("[CLOCK] Monotonic clock from the TSC (%lu kHz, %s); %s timers on %u CPUs; no periodic tick\n",
            hal_tsc_info()->khz, hal_tsc_info()->source, info.event_mode, hal_cpu_count());
}
