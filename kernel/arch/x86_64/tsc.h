#ifndef ARCHOS_ARCH_X86_64_TSC_H
#define ARCHOS_ARCH_X86_64_TSC_H

#include <stdint.h>
#include <stdbool.h>

/* TSC frequency (M1-04). Measured once at boot for the boot report, self-test
 * and benchmarks; M1-06 builds the clocksource on it (and adds HPET and the
 * cross-CPU skew check). */
typedef struct {
    uint64_t khz;         /* the value used: CPUID 0x15, else the PIT, else CPUID 0x16; 0 = unknown */
    uint64_t cpuid_khz;   /* CPUID 0x15: crystal x ratio (0 when the CPU doesn't say) */
    uint64_t base_mhz;    /* CPUID 0x16 base frequency (0 when absent) */
    uint64_t pit_khz;     /* measured against PIT channel 2 over 50 ms (0 when the PIT failed) */
    const char *source;   /* "cpuid", "pit", "base" or "none" */
    bool invariant;       /* CPUID 0x80000007 EDX bit 8: constant rate in every C/P state */
    bool deadline;        /* TSC-deadline timer mode (CPUID 1 ECX bit 24) */
} TscInfo;

/* Work out the TSC frequency. Call once on the BSP, after cpu_features_init;
 * polls the PIT for 50 ms. */
void tsc_calibrate(void);

/* What tsc_calibrate found. */
const TscInfo *tsc_info(void);

/* Convert TSC cycles to nanoseconds (0 when the frequency is unknown). */
uint64_t tsc_cycles_to_ns(uint64_t cycles);

#endif /* ARCHOS_ARCH_X86_64_TSC_H */
