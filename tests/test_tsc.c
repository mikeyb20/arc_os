/* arc_os — Host-side tests for kernel/arch/x86_64/tsc.c (M1-04, M1-06)
 *
 * The real calibration order and cycle <-> ns conversion over fake CPUID, a
 * fake TSC and fake PIT/HPET measurements. */

#include "test_framework.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Guard kernel headers that need stubbing */
#define ARCHOS_ARCH_X86_64_CPUID_H
#define ARCHOS_ARCH_X86_64_PIT_H
#define ARCHOS_ARCH_X86_64_HPET_H
#define ARCHOS_ARCH_X86_64_IPI_H
#define ARCHOS_ARCH_X86_64_PERCPU_H
#define ARCHOS_LIB_KPRINTF_H

/* Unique name: test_cpu-style suites may link the real one */
#define cpu_features tsc_test_cpu_features
#include "../kernel/arch/x86_64/cpu.h"

static inline void kprintf(const char *fmt, ...) {
    (void)fmt;
}

static CpuFeatures fake_features;
const CpuFeatures *tsc_test_cpu_features(void) {
    return &fake_features;
}
static uint32_t leaf15[3], leaf16_eax;   /* 0x15: EAX, EBX, ECX */
static void cpuid(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    (void)sub;
    *a = *b = *c = *d = 0;
    if (leaf == 0x15) {
        *a = leaf15[0];
        *b = leaf15[1];
        *c = leaf15[2];
    } else if (leaf == 0x16) {
        *a = leaf16_eax;
    }
}
static uint64_t fake_rdtsc;
static uint64_t cpu_rdtsc(void) {
    return fake_rdtsc;
}
static uint64_t cpu_rdtsc_ordered(void) {
    return fake_rdtsc;
}
static uint64_t pit_cycles;   /* what PIT channel 2 measures over 50 ms (0: no PIT) */
static uint64_t pit_measure_tsc(uint32_t ms, uint64_t max_cycles) {
    (void)ms, (void)max_cycles;
    return pit_cycles;
}
static uint64_t hpet_khz;
static int hpet_calls;
static uint64_t hpet_measure_tsc_khz(uint64_t phys, uint32_t ms, uint64_t max_cycles) {
    (void)phys, (void)ms, (void)max_cycles;
    hpet_calls++;
    return hpet_khz;
}
static uint32_t cpu_count = 1;
static uint32_t ipi_call_all(void (*fn)(void *arg), void *arg, uint64_t max_cycles) {
    (void)fn, (void)arg, (void)max_cycles;
    return 0;
}

#include "../kernel/arch/x86_64/tsc.c"

static void reset(void) {
    memset(&fake_features, 0, sizeof(fake_features));
    fake_features.max_leaf = 0x16;
    fake_features.ext7_edx = 1u << 8;   /* invariant */
    fake_features.l1_ecx = 1u << 24;    /* TSC-deadline */
    memset(leaf15, 0, sizeof(leaf15));
    leaf16_eax = 0;
    fake_rdtsc = 1000000;
    pit_cycles = 0;
    hpet_khz = 0;
    hpet_calls = 0;
    memset(&info, 0, sizeof(info));
    info.source = "none";
}

TEST(tsc_cpuid15_first) {
    reset();
    leaf15[0] = 1;
    leaf15[1] = 71;
    leaf15[2] = 24000000;      /* 24 MHz x 71 = 1.704 GHz */
    leaf16_eax = 1700;         /* the nominal base: 0.2% off */
    pit_cycles = 50 * 1703000; /* the PIT is recorded for the report, not used */
    tsc_calibrate();
    ASSERT_EQ(info.khz, 1704000);
    ASSERT_TRUE(strcmp(info.source, "cpuid") == 0);
    ASSERT_EQ(info.pit_khz, 1703000);
    ASSERT_EQ(info.base_mhz, 1700);
    ASSERT_TRUE(info.invariant);
    ASSERT_TRUE(info.deadline);
    /* Conversions both ways, to within 1 ns per second */
    uint64_t sec = tsc_ns_to_cycles(1000000000ULL);
    ASSERT_TRUE(sec >= 1703999999ULL && sec <= 1704000001ULL);
    uint64_t ns = tsc_cycles_to_ns(1704000000ULL);
    ASSERT_TRUE(ns >= 999999999ULL && ns <= 1000000001ULL);
    return 0;
}

TEST(tsc_pit_before_cpuid16) {
    reset();
    leaf16_eax = 1700;
    pit_cycles = 50 * 1704000;
    tsc_calibrate();
    ASSERT_EQ(info.khz, 1704000);
    ASSERT_TRUE(strcmp(info.source, "pit") == 0);
    tsc_calibrate_late(0xFED00000, false);   /* the PIT worked: no HPET */
    ASSERT_EQ(hpet_calls, 0);
    return 0;
}

/* No PIT: CPUID 0x16 first, then the HPET replaces it without a jump */
TEST(tsc_hpet_replaces_base) {
    reset();
    leaf16_eax = 2000;
    tsc_calibrate();
    ASSERT_TRUE(strcmp(info.source, "base") == 0);
    ASSERT_EQ(tsc_clock_ns(), 0);
    fake_rdtsc += 2000000000ULL;   /* 1 s at 2 GHz */
    uint64_t before = tsc_clock_ns();
    ASSERT_TRUE(before >= 999999999ULL && before <= 1000000001ULL);
    hpet_khz = 2500000;   /* the real rate */
    tsc_calibrate_late(0xFED00000, false);
    ASSERT_EQ(hpet_calls, 1);
    ASSERT_TRUE(strcmp(info.source, "hpet") == 0);
    ASSERT_EQ(tsc_clock_ns(), before);   /* carries on from the time shown */
    fake_rdtsc += 2500000000ULL;          /* 1 s at the new rate */
    uint64_t after = tsc_clock_ns() - before;
    ASSERT_TRUE(after >= 999999999ULL && after <= 1000000001ULL);
    return 0;
}

TEST(tsc_guess_without_any_source) {
    reset();
    fake_features.max_leaf = 0x0D;
    fake_features.ext7_edx = 0;
    tsc_calibrate();
    ASSERT_TRUE(strcmp(info.source, "guess") == 0);
    ASSERT_EQ(info.khz, TSC_GUESS_KHZ);
    ASSERT_FALSE(info.invariant);
    tsc_calibrate_late(0, false);   /* no HPET in ACPI */
    ASSERT_EQ(hpet_calls, 0);
    return 0;
}

/* A deadline centuries away saturates instead of wrapping into the past */
TEST(tsc_conversions_saturate) {
    reset();
    pit_cycles = 50 * 5000000;   /* 5 GHz */
    tsc_calibrate();
    ASSERT_EQ(tsc_ns_to_cycles(UINT64_MAX), UINT64_MAX);
    ASSERT_EQ(tsc_ns_to_cycles(4000000000000000000ULL), UINT64_MAX);   /* 127 years at 5 GHz > 2^64 cycles */
    ASSERT_EQ(tsc_clock_to_tsc(UINT64_MAX - 1), UINT64_MAX);
    uint64_t year = 365ULL * 86400 * 1000000000ULL;
    ASSERT_EQ(tsc_clock_to_tsc(year), fake_rdtsc + 5000000ULL * 1000 * 365 * 86400);
    ASSERT_EQ(tsc_clock_to_tsc(0), fake_rdtsc);   /* at or before the base: the base */
    /* A CPU whose TSC is a little behind the base reads the base time */
    fake_rdtsc -= 10;
    ASSERT_EQ(tsc_clock_ns(), 0);
    return 0;
}

TEST(tsc_sync_one_cpu) {
    reset();
    pit_cycles = 50 * 3000000;
    tsc_calibrate();
    cpu_count = 1;
    ASSERT_EQ(tsc_check_sync(20), 0);
    ASSERT_EQ(info.sync_cpus, 1);
    return 0;
}

TestCase tsc_tests[] = {
    TEST_ENTRY(tsc_cpuid15_first),        TEST_ENTRY(tsc_pit_before_cpuid16),
    TEST_ENTRY(tsc_hpet_replaces_base),   TEST_ENTRY(tsc_guess_without_any_source),
    TEST_ENTRY(tsc_conversions_saturate), TEST_ENTRY(tsc_sync_one_cpu),
};

int tsc_test_count = sizeof(tsc_tests) / sizeof(tsc_tests[0]);
