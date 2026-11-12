/* arc_os — TSC frequency at boot (M1-04)
 *
 * Owns: the boot-time TSC frequency. CPUID 0x15 (crystal clock x ratio) is
 * exact when the CPU reports the crystal; otherwise the TSC is timed against
 * PIT channel 2 for 50 ms; CPUID 0x16's base frequency is the last resort.
 * The PIT figure is measured even when CPUID gives one, so the boot report and
 * self-test can compare them (a gated or missing PIT shows up as 0).
 * Invariants: written once by tsc_calibrate on the BSP before the APs start.
 * Locks: none. */

#include "arch/x86_64/tsc.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/cpuid.h"
#include "arch/x86_64/pit.h"
#include "lib/kprintf.h"

#define TSC_CALIBRATE_MS   50
/* Give up on the PIT after 2^32 TSC cycles (1-4 s on real and emulated CPUs) */
#define TSC_PIT_MAX_CYCLES (1ULL << 32)

static TscInfo info = {.source = "none"};

void tsc_calibrate(void) {
    const CpuFeatures *f = cpu_features();
    uint32_t a, b, c, d;

    info.deadline = (f->l1_ecx >> 24) & 1;
    info.invariant = (f->ext7_edx >> 8) & 1;

    if (f->max_leaf >= 0x15) {
        cpuid(0x15, 0, &a, &b, &c, &d);   /* EAX denominator, EBX numerator, ECX crystal Hz */
        if (a != 0 && b != 0 && c != 0) info.cpuid_khz = (uint64_t)c / 1000 * b / a;
    }
    if (f->max_leaf >= 0x16) {
        cpuid(0x16, 0, &a, &b, &c, &d);
        info.base_mhz = a & 0xFFFF;
    }
    uint64_t cycles = pit_measure_tsc(TSC_CALIBRATE_MS, TSC_PIT_MAX_CYCLES);
    info.pit_khz = cycles / TSC_CALIBRATE_MS;

    if (info.cpuid_khz != 0) {
        info.khz = info.cpuid_khz;
        info.source = "cpuid";
    } else if (info.pit_khz != 0) {
        info.khz = info.pit_khz;
        info.source = "pit";
    } else if (info.base_mhz != 0) {
        info.khz = info.base_mhz * 1000;
        info.source = "base";
    }
    kprintf("[TSC] %lu kHz from %s (CPUID 0x15: %lu kHz, PIT: %lu kHz, base %lu MHz)%s%s\n", info.khz, info.source,
            info.cpuid_khz, info.pit_khz, info.base_mhz, info.invariant ? ", invariant" : "",
            info.deadline ? ", deadline timer" : "");
}

const TscInfo *tsc_info(void) {
    return &info;
}

uint64_t tsc_cycles_to_ns(uint64_t cycles) {
    if (info.khz == 0) return 0;
    /* cycles * 10^6 / khz without overflow for runs under ~5 hours at 1 GHz+ */
    return cycles / info.khz * 1000000 + (cycles % info.khz) * 1000000 / info.khz;
}
