/* arc_os — Hardware self-test, arc.selftest=1 (M1-04)
 *
 * Owns: the bring-up checks, one PASS/FAIL report line each: every MADT CPU
 * came online; an AVX2 checksum on every CPU matches the scalar one (XCR0
 * checked first); a 256 MB memory pattern test; the TSC rate against the PIT;
 * the PIT interrupt rate against the TSC; a kernel thread pinned to each CPU
 * runs there (M1-05: this starts the other CPUs' schedulers early).
 * Invariants: runs only in diagnostic mode, where init never starts, so no
 * user thread exists on any CPU while the AVX2 routine runs (ADR-0003 note);
 * the scheduler check runs last, after it. Every wait is bounded by TSC time.
 * Locks: none of its own (boot thread, pinned to the BSP).
 * Cost: about 1 s of memory traffic plus 300 ms of timed waits; one 16 KiB
 * kernel thread per CPU that is never freed (diagnostic mode only). */

#include "diag/diag.h"
#include "arch/hal.h"
#include "drivers/acpi.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "lib/kprintf.h"
#include "lib/testmode.h"
#include <stdarg.h>

#define AVX_ORDER       4                                  /* 64 KiB buffer */
#define AVX_WORDS       ((4096u << AVX_ORDER) / 4)
#define CALL_MAX_CYCLES (1ULL << 32)                  /* 1-4 s for every CPU to answer */
#define MEMTEST_BLOCKS  64                             /* x 4 MiB = 256 MB */
#define TIMER_CHECK_MS  200
#define TIMER_HZ        100                                  /* kmain starts the PIT at 100 Hz */
#define SCHED_CHECK_MS  2000 /* every pinned thread has run by then */

static int passed, failed;

static void result(const char *name, int ok, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void result(const char *name, int ok, const char *fmt, ...) {
    char detail[96];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(detail, sizeof(detail), fmt, ap);
    va_end(ap);
    diag_line("SELFTEST %s %-8s %s", ok ? "PASS" : "FAIL", name, detail);
    if (testmode_active()) {
        if (ok)
            kprintf("ARC_TEST PASS selftest.%s\n", name);
        else
            kprintf("ARC_TEST FAIL selftest.%s %s\n", name, detail);
    }
    if (ok)
        passed++;
    else
        failed++;
}

static void check_cpus(void) {
    const AcpiInfo *acpi = acpi_get_info();
    uint32_t madt = acpi ? acpi->cpu_count : 1;
    uint32_t online = hal_cpu_count();
    result("cpus", online == madt, "%u of %u MADT CPUs online", online, madt);
}

/* ---- AVX2 on every CPU ---- */

typedef struct {
    uint64_t xcr0;
    uint32_t sum;
    int rc;
    int ran;
} AvxResult;

static const uint32_t *avx_buf;
static AvxResult *avx_res;

static void avx_on_this_cpu(void *arg) {
    (void)arg;
    uint32_t i = hal_cpu_index();
    if (i >= HAL_MAX_CPUS) return;
    avx_res[i].rc = hal_avx2_selftest(avx_buf, AVX_WORDS, &avx_res[i].sum, &avx_res[i].xcr0);
    avx_res[i].ran = 1;
}

static void check_avx2(void) {
    struct page *pg = alloc_pages(AVX_ORDER, GFP_KERNEL);
    avx_res = kmalloc(sizeof(AvxResult) * HAL_MAX_CPUS, GFP_ZERO);
    if (pg == NULL || avx_res == NULL) {
        result("avx2", 0, "out of memory");
        if (pg) free_pages(pg, AVX_ORDER);
        kfree(avx_res);
        return;
    }
    uint32_t *buf = page_to_virt(pg);
    uint32_t want = 0;
    for (uint32_t i = 0; i < AVX_WORDS; i++) {
        buf[i] = i * 2654435761u ^ 0x5A5A5A5Au;
        want += buf[i];
    }
    avx_buf = buf;

    avx_on_this_cpu(NULL);
    uint32_t answered = hal_run_on_other_cpus(avx_on_this_cpu, NULL, CALL_MAX_CYCLES);

    uint32_t cpus = 0, bad = 0, first_bad = 0;
    const char *why = "";
    for (uint32_t i = 0; i < HAL_MAX_CPUS; i++) {
        uint32_t apic;
        const char *core;
        if (hal_cpu_info(i, &apic, &core) != 0) continue;
        cpus++;
        const char *w = !avx_res[i].ran          ? "no answer"
                        : avx_res[i].rc != 0     ? "AVX off in XCR0"
                        : avx_res[i].sum != want ? "wrong sum"
                                                 : NULL;
        if (w != NULL) {
            if (bad == 0) {
                first_bad = i;
                why = w;
            }
            bad++;
        }
    }
    if (bad == 0) {
        result("avx2", 1, "%u CPUs (%u answered the IPI), sum %08x, XCR0 %lx", cpus, answered + 1, want,
               avx_res[0].xcr0);
    } else {
        result("avx2", 0, "%u of %u CPUs bad; first CPU %u: %s (XCR0 %lx)", bad, cpus, first_bad, why,
               avx_res[first_bad].xcr0);
    }
    /* A CPU that answers after the timeout still reads the buffer and writes
     * avx_res: free the buffer only when every CPU answered; avx_res stays */
    if (answered + 1 >= hal_cpu_count()) free_pages(pg, AVX_ORDER);
}

/* ---- Memory pattern ---- */

static inline uint64_t pattern(uint64_t phys, int pass) {
    uint64_t v = phys * 0x9E3779B97F4A7C15ULL;
    return pass ? ~v : v;
}

static void check_memory(void) {
    struct page *blocks[MEMTEST_BLOCKS];
    uint32_t n = 0;
    while (n < MEMTEST_BLOCKS && (blocks[n] = alloc_pages(PAGE_MAX_ORDER, GFP_KERNEL)) != NULL) n++;

    const uint64_t words = (PAGE_SIZE << PAGE_MAX_ORDER) / 8;
    uint64_t errors = 0, first_bad = 0;
    for (int pass = 0; pass < 2; pass++) {
        /* Write every block, then read every block: catches address lines
         * that alias one block onto another */
        for (uint32_t b = 0; b < n; b++) {
            volatile uint64_t *w = page_to_virt(blocks[b]);
            uint64_t phys = page_to_phys(blocks[b]);
            for (uint64_t j = 0; j < words; j++) w[j] = pattern(phys + j * 8, pass);
        }
        for (uint32_t b = 0; b < n; b++) {
            volatile uint64_t *w = page_to_virt(blocks[b]);
            uint64_t phys = page_to_phys(blocks[b]);
            for (uint64_t j = 0; j < words; j++) {
                if (w[j] != pattern(phys + j * 8, pass)) {
                    if (errors == 0) first_bad = phys + j * 8;
                    errors++;
                }
            }
        }
    }
    for (uint32_t b = 0; b < n; b++) free_pages(blocks[b], PAGE_MAX_ORDER);

    uint64_t mb = (uint64_t)n * (PAGE_SIZE << PAGE_MAX_ORDER) >> 20;
    if (errors != 0) {
        result("memory", 0, "%lu bad words in %lu MB, first at phys 0x%lx", errors, mb, first_bad);
    } else {
        result("memory", n == MEMTEST_BLOCKS, "%lu MB, 2 patterns, no errors%s", mb,
               n == MEMTEST_BLOCKS ? "" : " (could not get 256 MB)");
    }
}

/* ---- Timers ---- */

static uint64_t diff_permille(uint64_t a, uint64_t b) {
    uint64_t d = a > b ? a - b : b - a;
    return b ? d * 1000 / b : 1000;
}

static void check_tsc(void) {
    const TscInfo *t = hal_tsc_info();
    if (t->pit_khz == 0) {
        result("tsc", 0, "PIT channel 2 never finished: no PIT or clock-gated (TSC %lu kHz from %s)", t->khz,
               t->source);
    } else if (t->cpuid_khz != 0) {
        uint64_t pm = diff_permille(t->pit_khz, t->cpuid_khz);
        result("tsc", pm <= 10, "PIT says %lu kHz, CPUID 0x15 %lu kHz (%lu.%lu%% apart)", t->pit_khz, t->cpuid_khz,
               pm / 10, pm % 10);
    } else if (t->base_mhz != 0) {
        uint64_t pm = diff_permille(t->pit_khz, t->base_mhz * 1000);
        result("tsc", pm <= 50, "PIT says %lu kHz, CPUID base %lu MHz (%lu.%lu%% apart)", t->pit_khz, t->base_mhz,
               pm / 10, pm % 10);
    } else {
        result("tsc", 1, "%lu kHz from the PIT (the CPU gives no frequency to compare)", t->pit_khz);
    }
}

static void check_timer_irq(void) {
    if (hal_tsc_info()->khz == 0) {
        result("timer", 0, "TSC frequency unknown: cannot time the PIT");
        return;
    }
    uint64_t start = hal_timer_ticks();
    diag_delay_ms(TIMER_CHECK_MS);
    uint64_t ticks = hal_timer_ticks() - start;
    uint64_t want = TIMER_HZ * TIMER_CHECK_MS / 1000;
    /* +-25%: emulators deliver timer interrupts late under load */
    int ok = ticks * 4 >= want * 3 && ticks * 4 <= want * 5;
    result("timer", ok, "%lu PIT interrupts in %u ms by the TSC (want %lu)%s", ticks, TIMER_CHECK_MS, want,
           ticks == 0 ? ": IRQ0 never fired" : "");
}

/* ---- SMP scheduler (M1-05): a thread pinned to each CPU runs there ---- */

static volatile uint32_t sched_ran; /* pinned threads that ran on their own CPU */
static volatile uint32_t sched_misplaced; /* pinned threads that ran anywhere else */

static void pinned_entry(void *arg) {
    uint32_t want = (uint32_t)(uintptr_t)arg;
    if (hal_cpu_index() == want)
        __atomic_fetch_add(&sched_ran, 1, __ATOMIC_RELAXED);
    else
        __atomic_fetch_add(&sched_misplaced, 1, __ATOMIC_RELAXED);
} /* returns: the thread ends DEAD (thread_trampoline) */

static void check_sched(void) {
    sched_start_aps();
    uint32_t started = 0, p_cores = 0, e_cores = 0;
    for (uint32_t cpu = 0; cpu < HAL_MAX_CPUS; cpu++) {
        SchedCpuStats st;
        if (sched_cpu_stats(cpu, &st) != 0) continue;
        Thread *t = thread_create(pinned_entry, (void *)(uintptr_t)cpu);
        if (t == NULL) break;
        CpuMask only;
        cpumask_clear(&only);
        cpumask_set(&only, cpu);
        sched_set_affinity(t, &only);
        sched_add_thread(t);
        started++;
        p_cores += st.core[0] == 'P';
        e_cores += st.core[0] == 'E';
    }
    uint64_t start = hal_tsc_read();
    uint64_t khz = hal_tsc_info()->khz;
    uint64_t limit = khz != 0 ? khz * SCHED_CHECK_MS : CALL_MAX_CYCLES;
    while (sched_ran + sched_misplaced < started && hal_tsc_read() - start < limit) {
        sched_yield(); /* CPU 0's pinned thread runs when this one yields */
        hal_cpu_relax();
    }
    uint32_t online = hal_cpu_count();
    int ok = started == online && sched_ran == online && sched_misplaced == 0;
    result("sched", ok, "%u of %u CPUs ran a pinned thread (%u P, %u E), %u elsewhere", sched_ran, online, p_cores,
           e_cores, sched_misplaced);
}

void diag_selftest(int *passes, int *fails) {
    passed = failed = 0;
    diag_line("Self-test");
    check_cpus();
    check_avx2();
    check_memory();
    check_tsc();
    check_timer_irq();
    check_sched(); /* last: starts the other CPUs' schedulers */
    diag_line("Self-test: %d PASS, %d FAIL", passed, failed);
    *passes = passed;
    *fails = failed;
}
