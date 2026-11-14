/* arc_os — TLB shootdown across CPUs (tlb.h, M1-07)
 *
 * Owns: each CPU's request slot (PerCpu.tlb_req) and its set of senders
 * still to serve (PerCpu.tlb_from), and the shootdown IPI.
 * Invariants: a CPU has at most one request out (it waits for all answers
 * with interrupts off before it can send another); a sender fills tlb_req
 * and its acks count before it marks any target's tlb_from, and a target
 * reads the request only after clearing that mark, so every request is run
 * exactly once per target.
 * Locks: none. The sender keeps interrupts off from its local flush to the
 * last answer and serves incoming requests while it waits; callers hold no
 * spinlock (checked in debug builds).
 * Cost: one invlpg per page up to TLB_FULL_FLUSH_PAGES, else a CR3 reload;
 * one IPI per target CPU (only CPUs that have the address space loaded,
 * except for kernel mappings); the wait is bounded by the targets' answer. */

#include "arch/x86_64/tlb.h"
#include "arch/x86_64/percpu.h"
#include "arch/x86_64/lapic.h"
#include "arch/x86_64/ipi.h"
#include "arch/x86_64/isr.h"
#include "arch/x86_64/paging.h"
#include "arch/x86_64/cpuid.h"
#include "arch/x86_64/tsc.h"
#include "arch/hal_lock.h"
#include "lib/kprintf.h"

/* Above this many pages one CR3 reload is cheaper than invlpg per page */
#define TLB_FULL_FLUSH_PAGES 32

/* A target that hasn't answered after this long is stuck */
#define TLB_WAIT_LIMIT_NS 10000000000ULL

static void tlb_flush_local(uint64_t pgd, uint64_t start, uint64_t end) {
    uint64_t cr3 = paging_read_cr3() & PTE_ADDR_MASK;
    if (pgd != 0 && cr3 != pgd) return;   /* not loaded: the CR3 switch flushed it */
    if (end <= start) return;
    if ((end - start) / 4096 > TLB_FULL_FLUSH_PAGES) {
        paging_write_cr3(paging_read_cr3());   /* no global pages: this flushes everything */
        return;
    }
    for (uint64_t a = start & ~0xFFFULL; a < end; a += 4096) paging_invlpg(a);
}

void tlb_shootdown_poll(void) {
    PerCpu *me = this_cpu();
    for (uint32_t w = 0; w < CPUMASK_WORDS; w++) {
        if (__atomic_load_n(&me->tlb_from.bits[w], __ATOMIC_RELAXED) == 0) continue;
        uint64_t senders = __atomic_exchange_n(&me->tlb_from.bits[w], 0, __ATOMIC_ACQUIRE);
        while (senders != 0) {
            uint32_t sender = w * 64 + (uint32_t)__builtin_ctzll(senders);
            senders &= senders - 1;
            PerCpu *s = percpu_get(sender);
            if (s == NULL) continue;
            TlbRequest *req = &s->tlb_req;
            tlb_flush_local(req->pgd, req->start, req->end);
            __atomic_sub_fetch(&req->acks, 1, __ATOMIC_RELEASE);
        }
    }
}

static void tlb_ipi_handler(InterruptFrame *frame) {
    (void)frame;
    tlb_shootdown_poll();
    lapic_eoi();
}

void tlb_init(void) {
    isr_register_handler(IPI_VEC_TLB_SHOOTDOWN, tlb_ipi_handler);
}

uint32_t tlb_shootdown(const CpuMask *cpus, uint64_t pgd, uint64_t start, uint64_t end) {
#if defined(ARC_DEBUG) && ARC_DEBUG
    if (hal_lock_depth() != 0) {
        kprintf("[TLB] FATAL: shootdown with a spinlock held\n");
        KERNEL_PANIC();
    }
#endif
    uint64_t flags = irq_save();   /* stay on this CPU until every answer is in */
    PerCpu *me = this_cpu();
    uint32_t self = me->cpu_index;

    /* Order the caller's page-table stores before reading who has pgd
     * loaded: a CPU that loads it after this point walks the new tables */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    tlb_flush_local(pgd, start, end);

    uint32_t targets[CPUMASK_WORDS * 64];
    uint32_t n = 0;
    for (uint32_t cpu = 0; cpu < cpu_count && cpu < CPUMASK_WORDS * 64; cpu++) {
        if (cpu == self) continue;
        if (pgd != 0) {
            uint64_t word = __atomic_load_n(&cpus->bits[cpu / 64], __ATOMIC_RELAXED);
            if (!((word >> (cpu % 64)) & 1)) continue;
        }
        PerCpu *c = percpu_get(cpu);
        if (c == NULL || !c->online) continue;
        targets[n++] = cpu;
    }
    if (n == 0) {
        irq_restore(flags);
        return 0;
    }

    TlbRequest *req = &me->tlb_req;
    req->pgd = pgd;
    req->start = start;
    req->end = end;
    __atomic_store_n(&req->acks, n, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < n; i++) {
        PerCpu *c = percpu_get(targets[i]);
        __atomic_fetch_or(&c->tlb_from.bits[self / 64], 1ULL << (self % 64), __ATOMIC_SEQ_CST);
        lapic_send_ipi(c->apic_id, IPI_VEC_TLB_SHOOTDOWN);
    }

    uint64_t t0 = cpu_rdtsc();
    while (__atomic_load_n(&req->acks, __ATOMIC_ACQUIRE) != 0) {
        tlb_shootdown_poll();   /* a target may be waiting on us with interrupts off */
        __asm__ volatile("pause");
        if (tsc_cycles_to_ns(cpu_rdtsc() - t0) > TLB_WAIT_LIMIT_NS) {
            kprintf("[TLB] FATAL: %u of %u CPUs never answered a shootdown\n",
                    __atomic_load_n(&req->acks, __ATOMIC_ACQUIRE), n);
            KERNEL_PANIC();
        }
    }
    irq_restore(flags);
    return n;
}
