/* arc_os — Host-side tests for kernel/time/clock.c and the CMOS RTC reader
 * kernel/arch/x86_64/rtc.c (M1-06)
 *
 * The real clock code over a fake HAL: per-CPU clocks that can be set out of
 * step, a fake boot sync check, and a fake CMOS register file behind the real
 * RTC reader (ports 0x70/0x71), so update-in-progress and changing registers
 * can be played out. */

#include "test_framework.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Guard kernel headers that need stubbing */
#define ARCHOS_ARCH_HAL_H
#define ARCHOS_ARCH_X86_64_IO_H
#define ARCHOS_ARCH_X86_64_CPUID_H
#define ARCHOS_PROC_SCHED_H
#define ARCHOS_PROC_PROCESS_H
#define ARCHOS_DRIVERS_ACPI_H
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_LIB_CMDLINE_H
#define ARCHOS_LIB_TESTMODE_H
#define ARCHOS_LIB_STRING_H
#define ARCHOS_TIME_TIMER_H

/* Unique name: test_thread.c links the real thread.c */
#define thread_current clock_test_thread_current

#include "proc/thread.h"
#include "arch/hal_lock.h"
#include "arch/x86_64/tsc.h"

static inline void kprintf(const char *fmt, ...) {
    (void)fmt;
}

/* ---- Fake CMOS behind outb/inb (io.h) and a fake TSC (cpuid.h) ---- */

static uint8_t cmos[128];
static uint8_t cmos_index;
static int uip_reads;        /* status A reads that still show an update in progress */
static int tick_after;       /* seconds-register reads until the clock ticks (0 = never) */
static uint64_t fake_rdtsc;
static void outb(uint16_t port, uint8_t v) {
    if (port == 0x70) cmos_index = v & 0x7F;
}
static uint8_t inb(uint16_t port) {
    if (port != 0x71) return 0xFF;
    if (cmos_index == 0x0A) {
        if (uip_reads > 0) {
            uip_reads--;
            return 0x80 | 0x26;
        }
        return 0x26;
    }
    if (cmos_index == 0x00 && tick_after > 0 && --tick_after == 0) cmos[0] = (uint8_t)(cmos[0] + 1);
    return cmos[cmos_index];
}
static uint64_t cpu_rdtsc(void) {
    return fake_rdtsc++;
}

#include "../kernel/arch/x86_64/rtc.c"

/* ---- Fake HAL (hal.h) ---- */

#define TEST_CPUS 2
static uint32_t test_cpu;
static uint64_t cpu_clock[TEST_CPUS];   /* each CPU's view of the TSC clock, ns */
static uint64_t fake_warp;              /* what the boot sync check reports */
static TscInfo fake_tsc = {.khz = 2000000, .source = "pit", .invariant = true, .sync_cpus = 2};
static int calibrate_calls, calibrate_force, legacy_stops, rtc_reads;
static uint64_t calibrate_hpet;
static int rtc_fails;

static uint64_t hal_clock_ns(void) {
    return cpu_clock[test_cpu];
}
static const TscInfo *hal_tsc_info(void) {
    return &fake_tsc;
}
static uint64_t hal_tsc_to_ns(uint64_t cycles) {
    return cycles / 2;
}
static uint64_t hal_clock_ns_to_cycles(uint64_t ns) {
    return ns * 2;
}
static void hal_clock_calibrate_late(uint64_t hpet, bool force) {
    calibrate_calls++;
    calibrate_hpet = hpet;
    calibrate_force = force;
}
static uint64_t hal_clock_check_sync(uint32_t ms) {
    (void)ms;
    return fake_warp;
}
static const char *hal_clockevent_mode(void) {
    return "LAPIC one-shot";
}
static void hal_legacy_timer_stop(void) {
    legacy_stops++;
}
static int hal_rtc_read(uint8_t century_reg, uint64_t max_cycles, RtcTime *out) {
    rtc_reads++;
    if (rtc_fails) return -1;
    return rtc_read(century_reg, max_cycles, out);
}
static uint32_t hal_cpu_count(void) {
    return TEST_CPUS;
}

/* ---- Other subsystems ---- */

static int timer_boot_one_shot = -1, timer_cpus_calls;
static void timer_init_boot(int one_shot) {
    timer_boot_one_shot = one_shot;
}
static void timer_init_cpus(void) {
    timer_cpus_calls++;
}

typedef struct {
    uint64_t hpet_address;
    uint8_t century_reg;
} AcpiInfo;
static AcpiInfo fake_acpi = {.hpet_address = 0xFED00000, .century_reg = 0x32};
static const AcpiInfo *acpi_get_info(void) {
    return &fake_acpi;
}

static const char *fake_cmdline = "";
static bool fake_testmode;
static const char *cmdline_raw(void) {
    return fake_cmdline;
}
/* key=value from a space-separated line (enough for these tests) */
static int cmdline_get(const char *line, const char *key, char *out, size_t outsz) {
    size_t klen = strlen(key);
    for (const char *p = line; (p = strstr(p, key)) != NULL; p++) {
        if ((p != line && p[-1] != ' ') || p[klen] != '=') continue;
        const char *v = p + klen + 1;
        size_t n = strcspn(v, " ");
        if (n >= outsz) n = outsz - 1;
        memcpy(out, v, n);
        out[n] = '\0';
        return 1;
    }
    return 0;
}
static bool testmode_active(void) {
    return fake_testmode;
}

/* Threads and processes: CPU time is the thread ID x 1000 ns */
typedef struct Process {
    Thread *main_thread;
} Process;
static Thread thr_main, thr_other;
static Thread *cur_thread = &thr_other;
static Process cur_proc = {.main_thread = &thr_main};
Thread *clock_test_thread_current(void) {
    return cur_thread;
}
static Process *proc_current(void) {
    return &cur_proc;
}
static uint64_t sched_thread_cpu_ns(Thread *t) {
    return (uint64_t)t->tid * 1000;
}

#include "../kernel/time/clock.c"

/* ---- Fixtures ---- */

/* 2026-11-14 10:20:30 in BCD, 24-hour, century register 0x32 */
static void cmos_set_2026(void) {
    memset(cmos, 0, sizeof(cmos));
    cmos[0x00] = 0x30;
    cmos[0x02] = 0x20;
    cmos[0x04] = 0x10;
    cmos[0x07] = 0x14;
    cmos[0x08] = 0x11;
    cmos[0x09] = 0x26;
    cmos[0x32] = 0x20;
    cmos[0x0B] = RTC_B_24H;
}

static void reset(void) {
    test_cpu = 0;
    cpu_clock[0] = cpu_clock[1] = 5 * NSEC_PER_SEC;
    fake_warp = 0;
    calibrate_calls = calibrate_force = legacy_stops = rtc_reads = rtc_fails = 0;
    calibrate_hpet = 0;
    timer_boot_one_shot = -1;
    timer_cpus_calls = 0;
    fake_cmdline = "";
    fake_testmode = false;
    uip_reads = tick_after = 0;
    cmos_set_2026();
    /* clock.c's state */
    real_offset = 0;
    mono_clamp = 0;
    mono_last = 0;
    memset(&info, 0, sizeof(info));
    info.event_mode = "none";
}

/* ---- Calendar and timespec helpers ---- */

TEST(clock_civil_to_unix_dates) {
    ASSERT_EQ(clock_civil_to_unix(1970, 1, 1, 0, 0, 0), 0);
    ASSERT_EQ(clock_civil_to_unix(1969, 12, 31, 23, 59, 59), -1);
    ASSERT_EQ(clock_civil_to_unix(1999, 12, 31, 23, 59, 59), 946684799);
    ASSERT_EQ(clock_civil_to_unix(2000, 3, 1, 0, 0, 0), 951868800);    /* after a leap day of a /400 year */
    ASSERT_EQ(clock_civil_to_unix(2024, 2, 29, 12, 34, 56), 1709210096);
    ASSERT_EQ(clock_civil_to_unix(2026, 11, 14, 0, 0, 0), 1794614400);
    ASSERT_EQ(clock_civil_to_unix(2100, 3, 1, 0, 0, 0), 4107542400);   /* 2100 is no leap year */
    return 0;
}

TEST(clock_timespec_conversions) {
    struct timespec ts = {.tv_sec = 3, .tv_nsec = 5};
    uint64_t ns = 0;
    ASSERT_EQ(clock_timespec_to_ns(&ts, &ns), 0);
    ASSERT_EQ(ns, 3000000005);
    ts.tv_nsec = 999999999;
    ASSERT_EQ(clock_timespec_to_ns(&ts, &ns), 0);
    ts.tv_nsec = 1000000000;
    ASSERT_EQ(clock_timespec_to_ns(&ts, &ns), -EINVAL);
    ts.tv_nsec = -1;
    ASSERT_EQ(clock_timespec_to_ns(&ts, &ns), -EINVAL);
    ts.tv_sec = -1;
    ts.tv_nsec = 0;
    ASSERT_EQ(clock_timespec_to_ns(&ts, &ns), -EINVAL);
    ts.tv_sec = INT64_MAX;   /* saturates instead of wrapping */
    ASSERT_EQ(clock_timespec_to_ns(&ts, &ns), 0);
    ASSERT_EQ(ns, UINT64_MAX);
    clock_ns_to_timespec(7000000123ULL, &ts);
    ASSERT_EQ(ts.tv_sec, 7);
    ASSERT_EQ(ts.tv_nsec, 123);
    return 0;
}

/* ---- The RTC ---- */

TEST(rtc_decode_formats) {
    RtcTime t;
    uint8_t bcd24[7] = {0x59, 0x07, 0x23, 0x31, 0x12, 0x25, 0x20};
    ASSERT_EQ(rtc_decode(bcd24, RTC_B_24H, &t), 0);
    ASSERT_EQ(t.year, 2025);
    ASSERT_EQ(t.month, 12);
    ASSERT_EQ(t.day, 31);
    ASSERT_EQ(t.hour, 23);
    ASSERT_EQ(t.minute, 7);
    ASSERT_EQ(t.second, 59);
    uint8_t bin12pm[7] = {0, 30, 0x80 | 1, 2, 3, 26, 0};   /* 1:30 PM, binary, no century register */
    ASSERT_EQ(rtc_decode(bin12pm, RTC_B_BINARY, &t), 0);
    ASSERT_EQ(t.hour, 13);
    ASSERT_EQ(t.year, 2026);
    uint8_t bcd12am[7] = {0, 0, 0x12, 1, 1, 0x99, 0};   /* 12 AM = 0 h; 99 -> 1999 */
    ASSERT_EQ(rtc_decode(bcd12am, 0, &t), 0);
    ASSERT_EQ(t.hour, 0);
    ASSERT_EQ(t.year, 1999);
    uint8_t bcd12noon[7] = {0, 0, 0x80 | 0x12, 1, 1, 0x26, 0x20};   /* 12 PM = 12 h */
    ASSERT_EQ(rtc_decode(bcd12noon, 0, &t), 0);
    ASSERT_EQ(t.hour, 12);
    uint8_t bad_month[7] = {0, 0, 0, 1, 0x13, 0x26, 0x20};
    ASSERT_EQ(rtc_decode(bad_month, RTC_B_24H, &t), -1);
    uint8_t blank[7] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};   /* no RTC: the bus floats high */
    ASSERT_EQ(rtc_decode(blank, 0, &t), -1);
    return 0;
}

TEST(rtc_read_waits_out_updates) {
    reset();
    RtcTime t;
    uip_reads = 3;   /* an update in progress first */
    ASSERT_EQ(rtc_read(0x32, 1000000, &t), 0);
    ASSERT_EQ(uip_reads, 0);
    ASSERT_EQ(t.year, 2026);
    ASSERT_EQ(t.month, 11);
    ASSERT_EQ(t.day, 14);
    ASSERT_EQ(t.hour, 10);
    ASSERT_EQ(t.minute, 20);
    ASSERT_EQ(t.second, 30);
    /* The seconds tick between the two reads: read again, take the new value */
    tick_after = 1;
    ASSERT_EQ(rtc_read(0x32, 1000000, &t), 0);
    ASSERT_EQ(t.second, 31);
    /* An update that never ends: give up in time */
    uip_reads = 1 << 30;
    fake_rdtsc = 0;
    ASSERT_EQ(rtc_read(0x32, 1000, &t), -1);
    ASSERT_TRUE(fake_rdtsc < 5000);
    uip_reads = 0;
    return 0;
}

/* ---- Clock bring-up and reads ---- */

TEST(clock_init_sets_wall_clock) {
    reset();
    clock_init();
    ASSERT_EQ(calibrate_calls, 1);
    ASSERT_EQ(calibrate_hpet, 0xFED00000);
    ASSERT_EQ(calibrate_force, 0);
    ASSERT_EQ(timer_boot_one_shot, 0);
    ASSERT_EQ(timer_cpus_calls, 1);
    ASSERT_EQ(legacy_stops, 1);
    ASSERT_EQ(clock_info()->rtc_ok, 1);
    ASSERT_EQ(clock_info()->clamped, 0);
    ASSERT_TRUE(strcmp(clock_info()->event_mode, "LAPIC one-shot") == 0);
    uint64_t boot = (uint64_t)clock_civil_to_unix(2026, 11, 14, 10, 20, 30) * NSEC_PER_SEC;
    ASSERT_EQ(clock_info()->boot_real_ns, boot);
    ASSERT_EQ(clock_real_ns(), boot);          /* read at the same instant */
    cpu_clock[0] += 1500 * NSEC_PER_MSEC;      /* both clocks move together */
    ASSERT_EQ(clock_real_ns(), boot + 1500 * NSEC_PER_MSEC);
    uint64_t ns;
    ASSERT_EQ(clock_read(CLOCK_REALTIME, &ns), 0);
    ASSERT_EQ(ns, boot + 1500 * NSEC_PER_MSEC);
    ASSERT_EQ(clock_read(CLOCK_REALTIME_COARSE, &ns), 0);
    ASSERT_EQ(ns, boot + 1500 * NSEC_PER_MSEC);
    /* An absolute wall-clock deadline becomes a monotonic one */
    ASSERT_EQ(clock_to_mono(CLOCK_REALTIME, boot + 2 * NSEC_PER_SEC), 5 * NSEC_PER_SEC + 2 * NSEC_PER_SEC);
    ASSERT_EQ(clock_to_mono(CLOCK_REALTIME, 1000), 0);   /* before boot: due at once */
    ASSERT_EQ(clock_to_mono(CLOCK_MONOTONIC, 1234), 1234);
    return 0;
}

TEST(clock_init_options) {
    reset();
    fake_cmdline = "arc.timer=oneshot arc.tsccal=hpet";
    clock_init();
    ASSERT_EQ(timer_boot_one_shot, 1);
    ASSERT_EQ(calibrate_force, 0);   /* forcing the HPET is for test mode only */
    reset();
    fake_cmdline = "arc.tsccal=hpet";
    fake_testmode = true;
    clock_init();
    ASSERT_EQ(calibrate_force, 1);
    ASSERT_EQ(timer_boot_one_shot, 0);
    return 0;
}

TEST(clock_init_without_rtc) {
    reset();
    rtc_fails = 1;
    clock_init();
    ASSERT_EQ(rtc_reads, 1);
    ASSERT_EQ(clock_info()->rtc_ok, 0);
    ASSERT_EQ(clock_real_ns(), clock_mono_ns());   /* 1970 + uptime */
    return 0;
}

TEST(clock_ids) {
    reset();
    clock_init();
    thr_main.tid = 7;
    thr_other.tid = 3;
    uint64_t ns;
    int mono_ids[] = {CLOCK_MONOTONIC, CLOCK_MONOTONIC_RAW, CLOCK_MONOTONIC_COARSE, CLOCK_BOOTTIME};
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ(clock_read(mono_ids[i], &ns), 0);
        ASSERT_EQ(ns, 5 * NSEC_PER_SEC);
    }
    ASSERT_EQ(clock_read(CLOCK_THREAD_CPUTIME_ID, &ns), 0);
    ASSERT_EQ(ns, 3000);   /* the calling thread */
    ASSERT_EQ(clock_read(CLOCK_PROCESS_CPUTIME_ID, &ns), 0);
    ASSERT_EQ(ns, 7000);   /* the process: its main thread until M1-16 */
    ASSERT_EQ(clock_read(8, &ns), -EINVAL);
    ASSERT_EQ(clock_read(-1, &ns), -EINVAL);
    for (int id = 0; id <= 7; id++) {
        ASSERT_EQ(clock_resolution(id, &ns), 0);
        ASSERT_EQ(ns, 1);
    }
    ASSERT_EQ(clock_resolution(11, &ns), -EINVAL);
    return 0;
}

/* TSCs out of step at boot: reads on any CPU never go below one already seen */
TEST(clock_clamped_when_tscs_disagree) {
    reset();
    fake_warp = 4000;
    clock_init();
    ASSERT_EQ(clock_info()->clamped, 1);
    cpu_clock[0] = 10 * NSEC_PER_SEC;
    cpu_clock[1] = 10 * NSEC_PER_SEC - 3000;   /* CPU 1 runs behind */
    test_cpu = 0;
    uint64_t a = clock_mono_ns();
    test_cpu = 1;
    uint64_t b = clock_mono_ns();
    ASSERT_EQ(a, 10 * NSEC_PER_SEC);
    ASSERT_EQ(b, a);   /* not backwards */
    cpu_clock[1] += 5000;
    ASSERT_EQ(clock_mono_ns(), 10 * NSEC_PER_SEC + 2000);
    /* In step: no clamp, each CPU reads its own clock */
    reset();
    clock_init();
    ASSERT_EQ(clock_info()->clamped, 0);
    cpu_clock[1] = 1;
    test_cpu = 1;
    ASSERT_EQ(clock_mono_ns(), 1);
    return 0;
}

TestCase clock_tests[] = {
    TEST_ENTRY(clock_civil_to_unix_dates),
    TEST_ENTRY(clock_timespec_conversions),
    TEST_ENTRY(rtc_decode_formats),
    TEST_ENTRY(rtc_read_waits_out_updates),
    TEST_ENTRY(clock_init_sets_wall_clock),
    TEST_ENTRY(clock_init_options),
    TEST_ENTRY(clock_init_without_rtc),
    TEST_ENTRY(clock_ids),
    TEST_ENTRY(clock_clamped_when_tscs_disagree),
};

int clock_test_count = sizeof(clock_tests) / sizeof(clock_tests[0]);
