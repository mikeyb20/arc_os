#ifndef ARCHOS_ARCH_X86_64_TSC_H
#define ARCHOS_ARCH_X86_64_TSC_H

#include <stdint.h>
#include <stdbool.h>

/* TSC frequency (M1-04) and the TSC clocksource (M1-06): monotonic
 * nanoseconds since boot from the time-stamp counter, the conversions both
 * ways, and the boot check that every CPU's TSC runs in step. */
typedef struct {
    uint64_t khz;         /* the value used: CPUID 0x15, else the PIT, else the HPET, else CPUID 0x16 */
    uint64_t cpuid_khz;   /* CPUID 0x15: crystal x ratio (0 when the CPU doesn't say) */
    uint64_t base_mhz;    /* CPUID 0x16 base frequency (0 when absent) */
    uint64_t pit_khz;     /* measured against PIT channel 2 over 50 ms (0 when the PIT failed) */
    uint64_t hpet_khz;    /* measured against the HPET (0 unless the PIT failed or a test forced it) */
    const char *source;   /* "cpuid", "pit", "hpet", "base" or "guess" */
    bool invariant;       /* CPUID 0x80000007 EDX bit 8: constant rate in every C/P state */
    bool deadline;        /* TSC-deadline timer mode (CPUID 1 ECX bit 24) */
    uint32_t sync_cpus;   /* CPUs in the boot sync check (0 = not run) */
    uint64_t max_warp;    /* largest step back one CPU's TSC showed against another's, cycles */
} TscInfo;

/* Work out the TSC frequency and start the clock at 0. Call once on the BSP,
 * after cpu_features_init; polls the PIT for 50 ms. */
void tsc_calibrate(void);

/* Second chance once ACPI is read: when neither CPUID 0x15 nor the PIT gave
 * the frequency (or force, test mode only), time the TSC against the HPET at
 * physical address hpet_phys (0 = none) for 20 ms. The clock carries on
 * from the time it shows. BSP, before the APs read the clock. */
void tsc_calibrate_late(uint64_t hpet_phys, bool force);

/* What the calibration and the sync check found. */
const TscInfo *tsc_info(void);

/* Convert a TSC cycle count to nanoseconds and back (multiply and shift,
 * exact to about 1 ns per second; no overflow for centuries of uptime). */
uint64_t tsc_cycles_to_ns(uint64_t cycles);
uint64_t tsc_ns_to_cycles(uint64_t ns);

/* The clock: nanoseconds since tsc_calibrate. Any CPU, any context. */
uint64_t tsc_clock_ns(void);

/* The TSC value at which tsc_clock_ns() reaches ns (for the deadline timer). */
uint64_t tsc_clock_to_tsc(uint64_t ns);

/* Boot check (M1-06): every online CPU reads the TSC in turn for ms
 * milliseconds under one lock and records any value smaller than the one
 * before it (Linux's check_tsc_warp). Interrupts off on every CPU meanwhile.
 * Returns the largest step back in cycles (0 = the TSCs run in step). BSP. */
uint64_t tsc_check_sync(uint32_t ms);

#endif /* ARCHOS_ARCH_X86_64_TSC_H */
