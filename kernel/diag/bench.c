/* arc_os — Hot-path benchmarks, arc.bench=1 (M1-04, ADR-0019)
 *
 * Owns: TSC-timed runs of the kernel's hot paths, printed as one table in
 * the boot report: 4 KiB and 2 MiB page allocation, kmalloc, and a thread
 * context switch. Later blocks add a row for their hot path (page fault,
 * read, mmap fault). Results are for photographs and docs/perf/, never for
 * CI: CI budgets count, they never time (ADR-0019).
 * Invariants: every run frees what it allocates; the context-switch partner
 * thread ends DEAD (its stack is not reclaimed: there is no reaper for kernel
 * threads yet). Locks: none (diagnostic mode, BSP).
 * Cost: well under a second. */

#include "diag/diag.h"
#include "arch/hal.h"
#include "mm/pmm.h"
#include "mm/kmalloc.h"
#include "proc/thread.h"
#include "proc/sched.h"

static uint64_t run_page_4k(uint32_t n) {
    uint64_t start = hal_tsc_read();
    for (uint32_t i = 0; i < n; i++) {
        struct page *p = alloc_pages(0, GFP_KERNEL);
        if (p != NULL) free_pages(p, 0);
    }
    return hal_tsc_read() - start;
}

static uint64_t run_page_2m(uint32_t n) {
    uint64_t start = hal_tsc_read();
    for (uint32_t i = 0; i < n; i++) {
        struct page *p = alloc_pages(PAGE_ORDER_2MB, GFP_KERNEL);
        if (p != NULL) free_pages(p, PAGE_ORDER_2MB);
    }
    return hal_tsc_read() - start;
}

static uint64_t run_kmalloc_64(uint32_t n) {
    uint64_t start = hal_tsc_read();
    for (uint32_t i = 0; i < n; i++) kfree(kmalloc(64, GFP_KERNEL));
    return hal_tsc_read() - start;
}

/* Context switch: this thread and a partner yield to each other; every round
 * is two switches. */
static volatile uint32_t rounds_left;

static void switch_partner(void *arg) {
    (void)arg;
    while (rounds_left > 0) {
        rounds_left--;
        sched_yield();
    }
    thread_current()->state = THREAD_DEAD;   /* never scheduled again */
    sched_yield();
}

static uint64_t run_switch(uint32_t n) {
    Thread *t = thread_create(switch_partner, NULL);
    if (t == NULL) return 0;
    CpuMask cpu0;   /* same CPU as this thread, even once the self-test started the others */
    cpumask_clear(&cpu0);
    cpumask_set(&cpu0, 0);
    sched_set_affinity(t, &cpu0);
    rounds_left = n;
    sched_add_thread(t);
    uint64_t start = hal_tsc_read();
    while (rounds_left > 0) sched_yield();
    uint64_t cycles = hal_tsc_read() - start;
    sched_yield();   /* let the partner finish */
    return cycles / 2;   /* two switches per round */
}

typedef struct {
    const char *name;
    uint64_t (*run)(uint32_t n);
    uint32_t n;
} BenchCase;

static const BenchCase cases[] = {
    {"page alloc+free 4 KiB", run_page_4k, 10000},
    {"page alloc+free 2 MiB", run_page_2m, 1000},
    {"kmalloc+kfree 64 B", run_kmalloc_64, 10000},
    {"context switch", run_switch, 10000},
};

void diag_bench(void) {
    diag_line("Benchmark (TSC %lu kHz; per operation, best of 3 runs)", hal_tsc_info()->khz);
    for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint64_t best = UINT64_MAX;
        for (int r = 0; r < 3; r++) {
            uint64_t c = cases[i].run(cases[i].n);
            if (c != 0 && c < best) best = c;
        }
        if (best == UINT64_MAX) {
            diag_line("BENCH %-24s failed (out of memory)", cases[i].name);
            continue;
        }
        uint64_t per = best / cases[i].n;
        diag_line("BENCH %-24s %8lu cycles %8lu ns  (%u runs)", cases[i].name, per, hal_tsc_to_ns(best) / cases[i].n,
                  cases[i].n);
    }
}
