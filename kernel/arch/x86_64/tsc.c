/* arc_os — TSC frequency and the TSC clocksource (M1-04, M1-06)
 *
 * Owns: the TSC frequency, the cycle <-> nanosecond conversion and the
 * monotonic clock built on them, and the boot check that the TSCs of all
 * CPUs run in step.
 * Frequency, in this order: CPUID 0x15 (crystal x ratio, exact when the CPU
 * reports the crystal); else PIT channel 2 timed for 50 ms; else the HPET for
 * 20 ms (tsc_calibrate_late, once ACPI is read); else CPUID 0x16's base
 * frequency. The PIT comes before 0x16 because 0x16 is the nominal base clock
 * in whole MHz, which can be 0.2% off the real TSC rate (a 1.7 GHz part whose
 * TSC runs at 24 MHz x 71 = 1.704 GHz); 0.2% is three minutes a day. The PIT
 * figure is the median of 3 tries, and one more than 2% off 0x16 loses to it.
 * Invariants: the conversion is written on the BSP before any other CPU reads
 * the clock (tsc_calibrate, tsc_calibrate_late) and never changes after.
 * The clock is clock_base_ns + (TSC - clock_base_tsc) * mult >> 32, so a late
 * recalibration carries on from the time already shown instead of jumping.
 * Locks: none (the sync check's own spin flag, interrupts off).
 * Cost: a clock read is one LFENCE + RDTSC and one 64x64 multiply. */

#include "arch/x86_64/tsc.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/cpuid.h"
#include "arch/x86_64/pit.h"
#include "arch/x86_64/hpet.h"
#include "arch/x86_64/ipi.h"
#include "arch/x86_64/msr.h"
#include "arch/x86_64/percpu.h"
#include "lib/kprintf.h"
#include "lib/string.h"

#define TSC_CALIBRATE_MS          50
#define TSC_HPET_CALIBRATE_MS     20
/* Give up on the PIT after 2^32 TSC cycles (1-4 s on real and emulated CPUs) */
#define TSC_PIT_MAX_CYCLES (1ULL << 32)
#define TSC_PIT_TRIES             3   /* odd: the median is one of them */
/* With a CPUID 0x15 rate, give the PIT cross-check 100 ms (a gated PIT costs no more) */
#define TSC_PIT_CHECK_MS          100
/* PIT vs CPUID 0x16 beyond 2%: trust 0x16 (real parts differ by about 0.2%) */
#define TSC_PIT_BASE_MAX_PERMILLE 20
/* No frequency from anywhere: assume 1 GHz and say so */
#define TSC_GUESS_KHZ             1000000ULL
#define TSC_SHIFT                 32

static TscInfo info = {.source = "none"};

static uint64_t c2n_mult;   /* ns per cycle << TSC_SHIFT */
static uint64_t n2c_mult;   /* cycles per ns << TSC_SHIFT */
static uint64_t clock_base_tsc;
static uint64_t clock_base_ns;

static inline uint64_t mul_shift(uint64_t a, uint64_t mult) {
    return (uint64_t)(((unsigned __int128)a * mult) >> TSC_SHIFT);
}

/* Use khz from now on; the clock carries on from now_ns at TSC value now. */
static void tsc_set_rate(uint64_t khz, const char *source, uint64_t now, uint64_t now_ns) {
    info.khz = khz;
    info.source = source;
    /* 10^6 << 32 < 2^52 and khz < 2^24 (16 THz): both fit in 64 bits */
    c2n_mult = (1000000ULL << TSC_SHIFT) / khz;
    n2c_mult = (khz << TSC_SHIFT) / 1000000ULL;
    clock_base_tsc = now;
    clock_base_ns = now_ns;
}

static uint64_t tsc_permille_apart(uint64_t a, uint64_t b) {
    uint64_t d = a > b ? a - b : b - a;
    return b != 0 ? d * 1000 / b : 1000;
}

/* The TSC rate measured against PIT channel 2, in kHz; 0 when the PIT never
 * finished. With a rate from CPUID 0x15 it is only the report's cross-check:
 * one try, given up after TSC_PIT_CHECK_MS. Otherwise it is the clock: the
 * median of TSC_PIT_TRIES tries, so one try stretched by an SMI or a busy
 * emulator host can't set the rate. */
static uint64_t tsc_pit_khz(uint64_t cpuid_khz) {
    if (cpuid_khz != 0) {
        return pit_measure_tsc(TSC_CALIBRATE_MS, cpuid_khz * TSC_PIT_CHECK_MS) / TSC_CALIBRATE_MS;
    }
    uint64_t k[TSC_PIT_TRIES];
    for (int i = 0; i < TSC_PIT_TRIES; i++) {
        k[i] = pit_measure_tsc(TSC_CALIBRATE_MS, TSC_PIT_MAX_CYCLES) / TSC_CALIBRATE_MS;
        if (k[i] == 0) return 0;   /* no PIT: don't wait out the limit again */
        for (int j = i; j > 0 && k[j] < k[j - 1]; j--) {
            uint64_t t = k[j];
            k[j] = k[j - 1];
            k[j - 1] = t;
        }
    }
    return k[TSC_PIT_TRIES / 2];
}

/* The BSP's microcode revision, as Linux reads it: Intel wants 0 written and a
 * CPUID first; AMD's patch level is read only. Other vendors: 0. */
static uint32_t tsc_microcode(const CpuFeatures *f) {
    uint32_t a, b, c, d;
    if (strcmp(f->vendor, "GenuineIntel") == 0) {
        wrmsr(MSR_UCODE_REV, 0);
        cpuid(1, 0, &a, &b, &c, &d);
        return (uint32_t)(rdmsr(MSR_UCODE_REV) >> 32);
    }
    if (strcmp(f->vendor, "AuthenticAMD") == 0) return (uint32_t)rdmsr(MSR_UCODE_REV);
    return 0;
}

void tsc_calibrate(void) {
    const CpuFeatures *f = cpu_features();
    uint32_t a, b, c, d;

    info.deadline = (f->l1_ecx >> 24) & 1;
    info.invariant = (f->ext7_edx >> 8) & 1;
    info.microcode = tsc_microcode(f);

    if (f->max_leaf >= 0x15) {
        cpuid(0x15, 0, &a, &b, &c, &d);   /* EAX denominator, EBX numerator, ECX crystal Hz */
        if (a != 0 && b != 0 && c != 0) info.cpuid_khz = (uint64_t)c / 1000 * b / a;
    }
    if (f->max_leaf >= 0x16) {
        cpuid(0x16, 0, &a, &b, &c, &d);
        info.base_mhz = a & 0xFFFF;
    }
    info.pit_khz = tsc_pit_khz(info.cpuid_khz);

    uint64_t now = cpu_rdtsc();
    if (info.cpuid_khz != 0) {
        tsc_set_rate(info.cpuid_khz, "cpuid", now, 0);
    } else if (info.pit_khz != 0 && info.base_mhz != 0 &&
               tsc_permille_apart(info.pit_khz, info.base_mhz * 1000) > TSC_PIT_BASE_MAX_PERMILLE) {
        /* On the client parts that report 0x16 without a 0x15 crystal, the base
         * clock is the TSC's nominal rate (Linux's native_calibrate_tsc); a PIT
         * figure this far off it was disturbed (an SMI, a busy VM host) */
        kprintf("[TSC] WARNING: PIT says %lu kHz, CPUID base %lu MHz: using the base\n", info.pit_khz, info.base_mhz);
        tsc_set_rate(info.base_mhz * 1000, "base", now, 0);
    } else if (info.pit_khz != 0) {
        tsc_set_rate(info.pit_khz, "pit", now, 0);
    } else if (info.base_mhz != 0) {
        tsc_set_rate(info.base_mhz * 1000, "base", now, 0);   /* the HPET may still replace it */
    } else {
        tsc_set_rate(TSC_GUESS_KHZ, "guess", now, 0);
    }
    kprintf("[TSC] %lu kHz from %s (CPUID 0x15: %lu kHz, PIT: %lu kHz, base %lu MHz)%s%s, microcode 0x%x\n", info.khz,
            info.source, info.cpuid_khz, info.pit_khz, info.base_mhz,
            info.invariant ? ", invariant" : ", NOT invariant", info.deadline ? ", deadline timer" : "",
            info.microcode);
    if (!info.invariant) {
        /* Emulators (QEMU TCG) and old CPUs: the TSC is still the clock, but its
         * rate may follow P-states and stop in deep C-states on such a CPU */
        kprintf("[TSC] WARNING: TSC not invariant; using it anyway (no other clocksource)\n");
    }
}

void tsc_calibrate_late(uint64_t hpet_phys, bool force) {
    int need = info.cpuid_khz == 0 && info.pit_khz == 0;
    if (hpet_phys == 0 || (!need && !force)) return;
    info.hpet_khz = hpet_measure_tsc_khz(hpet_phys, TSC_HPET_CALIBRATE_MS, TSC_PIT_MAX_CYCLES);
    kprintf("[TSC] HPET says %lu kHz%s\n", info.hpet_khz, force && !need ? " (test: HPET calibration forced)" : "");
    if (info.hpet_khz == 0) return;
    uint64_t now = cpu_rdtsc_ordered();
    tsc_set_rate(info.hpet_khz, "hpet", now, tsc_clock_ns());
    kprintf("[TSC] %lu kHz from hpet\n", info.khz);
}

const TscInfo *tsc_info(void) {
    return &info;
}

uint64_t tsc_cycles_to_ns(uint64_t cycles) {
    return mul_shift(cycles, c2n_mult);
}

/* ns -> cycles, saturating: a deadline centuries away must not wrap to the past
 * (a deadline in the past fires at once, and again on every reprogram) */
static inline uint64_t mul_shift_sat(uint64_t a, uint64_t mult) {
    unsigned __int128 r = ((unsigned __int128)a * mult) >> TSC_SHIFT;
    return r > UINT64_MAX ? UINT64_MAX : (uint64_t)r;
}

uint64_t tsc_ns_to_cycles(uint64_t ns) {
    return mul_shift_sat(ns, n2c_mult);
}

uint64_t tsc_clock_ns(void) {
    uint64_t now = cpu_rdtsc_ordered();
    /* A CPU whose TSC is a little behind the BSP's right after boot reads 0 */
    if (now < clock_base_tsc) return clock_base_ns;
    return clock_base_ns + mul_shift(now - clock_base_tsc, c2n_mult);
}

uint64_t tsc_clock_to_tsc(uint64_t ns) {
    if (ns <= clock_base_ns) return clock_base_tsc;
    uint64_t cycles = mul_shift_sat(ns - clock_base_ns, n2c_mult);
    return cycles > UINT64_MAX - clock_base_tsc ? UINT64_MAX : clock_base_tsc + cycles;
}

/* ---- Cross-CPU sync check ---- */

static volatile uint32_t sync_lock;      /* test-and-set, interrupts off */
static volatile uint64_t sync_last;      /* last TSC value read under it */
static volatile uint64_t sync_warp;      /* largest step back seen */
static volatile uint32_t sync_arrived;   /* start barrier */
static uint32_t sync_cpus;
static uint64_t sync_cycles;

static void sync_loop(void *arg) {
    (void)arg;
    /* Start together (bounded: a CPU that never comes doesn't hang the rest) */
    __atomic_add_fetch(&sync_arrived, 1, __ATOMIC_ACQ_REL);
    uint64_t t0 = cpu_rdtsc();
    while (__atomic_load_n(&sync_arrived, __ATOMIC_ACQUIRE) < sync_cpus && cpu_rdtsc() - t0 < sync_cycles) {
        __asm__ volatile("pause");
    }
    uint64_t start = cpu_rdtsc();
    for (;;) {
        while (__atomic_exchange_n(&sync_lock, 1, __ATOMIC_ACQUIRE) != 0) __asm__ volatile("pause");
        uint64_t prev = sync_last;
        uint64_t now = cpu_rdtsc_ordered();
        sync_last = now;
        if (now < prev && prev - now > sync_warp) sync_warp = prev - now;
        __atomic_store_n(&sync_lock, 0, __ATOMIC_RELEASE);
        if (now - start >= sync_cycles) break;
    }
}

uint64_t tsc_check_sync(uint32_t ms) {
    sync_cpus = cpu_count;
    if (sync_cpus <= 1) {
        info.sync_cpus = 1;
        return 0;
    }
    sync_cycles = tsc_ns_to_cycles((uint64_t)ms * 1000000ULL);
    sync_last = 0;
    sync_warp = 0;
    __atomic_store_n(&sync_arrived, 0, __ATOMIC_RELEASE);
    uint32_t others = ipi_call_all(sync_loop, NULL, 4 * sync_cycles + TSC_PIT_MAX_CYCLES);
    info.sync_cpus = others + 1;
    info.max_warp = sync_warp;
    return sync_warp;
}
