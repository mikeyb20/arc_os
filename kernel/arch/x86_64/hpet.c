/* arc_os — HPET as a TSC calibration reference (M1-06)
 *
 * Owns: one read-only use of the HPET main counter at boot. Recent laptops
 * may clock-gate the PIT; there the HPET (when firmware leaves it on) still
 * gives a reference for the TSC rate.
 * Invariants: the counter is only read; firmware's configuration is kept,
 * except that a stopped counter is started (ENABLE_CNF) and stays running.
 * Locks: none (BSP, boot). */

#include "arch/x86_64/hpet.h"
#include "arch/x86_64/cpuid.h"
#include "drivers/mmio.h"

#define HPET_REGS_SIZE   0x400
#define HPET_CAP         0x000   /* bits 63:32 period in femtoseconds, bit 13 64-bit counter */
#define HPET_CONF        0x010
#define HPET_CONF_ENABLE 1ULL
#define HPET_COUNTER     0x0F0
#define HPET_CAP_64BIT   (1ULL << 13)
#define HPET_MAX_PERIOD  100000000ULL   /* 100 ns: the spec's slowest allowed counter */

static uint64_t hpet_rd(volatile uint8_t *regs, uint32_t off) {
    return *(volatile uint64_t *)(regs + off);
}

uint64_t hpet_measure_tsc_khz(uint64_t phys, uint32_t ms, uint64_t max_cycles) {
    volatile uint8_t *regs = (volatile uint8_t *)mmio_map(phys, HPET_REGS_SIZE);
    if (regs == NULL) return 0;
    uint64_t cap = hpet_rd(regs, HPET_CAP);
    uint64_t period_fs = cap >> 32;
    if (period_fs == 0 || period_fs > HPET_MAX_PERIOD) return 0;
    uint64_t conf = hpet_rd(regs, HPET_CONF);
    if (!(conf & HPET_CONF_ENABLE)) *(volatile uint64_t *)(regs + HPET_CONF) = conf | HPET_CONF_ENABLE;
    uint64_t mask = (cap & HPET_CAP_64BIT) ? ~0ULL : 0xFFFFFFFFULL;

    uint64_t ticks = (uint64_t)ms * 1000000000000ULL / period_fs;   /* ms in fs / period */
    uint64_t h0 = hpet_rd(regs, HPET_COUNTER) & mask;
    uint64_t t0 = cpu_rdtsc();
    uint64_t h = h0, t = t0;
    while (((h - h0) & mask) < ticks) {
        h = hpet_rd(regs, HPET_COUNTER) & mask;
        t = cpu_rdtsc();
        if (t - t0 > max_cycles) return 0;   /* the counter doesn't run */
    }
    /* elapsed ns = HPET ticks x period (fs) / 10^6; ticks x period < 2^64 for ms < 18 s */
    uint64_t ns = ((h - h0) & mask) * period_fs / 1000000ULL;
    if (ns == 0) return 0;
    return (t - t0) * 1000000ULL / ns;   /* cycles per ms; (t - t0) < 2^44 for any real CPU */
}
