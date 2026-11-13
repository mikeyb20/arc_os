#ifndef ARCHOS_ARCH_X86_64_TLB_H
#define ARCHOS_ARCH_X86_64_TLB_H

#include <stdint.h>
#include "proc/cpumask.h"

/* TLB shootdown (M1-07, ADR-0004).
 *
 * A CPU that changed or removed page-table entries flushes its own TLB and
 * asks every other CPU that may cache them to flush too, then waits until
 * each has answered. Each CPU has one request slot (PerCpu.tlb_req) and a set
 * of CPUs whose requests it still has to run (PerCpu.tlb_from); the IPI
 * handler and every wait loop run the pending ones, so two CPUs shooting at
 * each other with interrupts off can't deadlock.
 *
 * Rules: call with no spinlock held (a CPU spinning on that lock with
 * interrupts off could never answer; debug builds panic). A CPU that waits
 * with interrupts off for something another CPU holds must call
 * tlb_shootdown_poll() in its loop (kernel_lock_irq does). */

/* Flush [start, end) of address space pgd (PML4 physical address) on this CPU
 * if it has pgd loaded and on every CPU in cpus; pgd 0 means kernel mappings,
 * flushed on every online CPU. Returns the number of other CPUs that flushed.
 * Waits at most 10 s for them, then panics (a CPU that never answers is a
 * kernel bug). */
uint32_t tlb_shootdown(const CpuMask *cpus, uint64_t pgd, uint64_t start, uint64_t end);

/* Run the requests other CPUs left for this one. Interrupts off. */
void tlb_shootdown_poll(void);

/* Register the shootdown IPI handler (boot). */
void tlb_init(void);

#endif /* ARCHOS_ARCH_X86_64_TLB_H */
