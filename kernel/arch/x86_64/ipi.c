/* arc_os — Inter-Processor Interrupt infrastructure
 *
 * Provides the TLB shootdown vector (handled in tlb.c), cross-core reschedule (M1-05: wakes an idle CPU or
 * makes a CPU move a thread its affinity no longer allows), halt and
 * call-function IPIs. */

#include "arch/x86_64/ipi.h"
#include "arch/x86_64/isr.h"
#include "arch/x86_64/lapic.h"
#include "arch/x86_64/percpu.h"
#include "arch/x86_64/cpuid.h"
#include "arch/x86_64/tlb.h"
#include "proc/sched.h"
#include "lib/kprintf.h"

/* Reschedule handler: another CPU queued a thread here (or moved one away) */
static void ipi_schedule_handler(InterruptFrame *frame) {
    (void)frame;
    lapic_eoi();   /* first: sched_preempt may switch threads */
    sched_preempt();
}

/* Halt handler — stop this CPU */
static void ipi_halt_handler(InterruptFrame *frame) {
    (void)frame;
    lapic_eoi();
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

/* ipi_call_others: the function to run and how many CPUs finished it */
static void (*call_fn)(void *arg);
static void *call_arg;
static uint32_t call_done;

static void ipi_call_handler(InterruptFrame *frame) {
    (void)frame;
    void (*fn)(void *) = __atomic_load_n(&call_fn, __ATOMIC_ACQUIRE);
    if (fn != NULL) {
        fn(call_arg);
        __atomic_add_fetch(&call_done, 1, __ATOMIC_SEQ_CST);
    }
    lapic_eoi();
}

uint32_t ipi_call_others(void (*fn)(void *arg), void *arg, uint64_t max_cycles) {
    if (cpu_count <= 1) return 0;
    call_arg = arg;
    __atomic_store_n(&call_done, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&call_fn, fn, __ATOMIC_RELEASE);   /* publishes call_arg */
    lapic_send_ipi_all_excluding_self(IPI_VEC_CALL);

    uint64_t start = cpu_rdtsc();
    while (__atomic_load_n(&call_done, __ATOMIC_SEQ_CST) < cpu_count - 1 && cpu_rdtsc() - start < max_cycles) {
        __asm__ volatile("pause");
    }
    __atomic_store_n(&call_fn, NULL, __ATOMIC_RELEASE);   /* a late CPU runs nothing */
    return __atomic_load_n(&call_done, __ATOMIC_SEQ_CST);
}

void ipi_init(void) {
    tlb_init();   /* IPI_VEC_TLB_SHOOTDOWN: range shootdowns (tlb.c, M1-07) */
    isr_register_handler(IPI_VEC_SCHEDULE, ipi_schedule_handler);
    isr_register_handler(IPI_VEC_HALT, ipi_halt_handler);
    isr_register_handler(IPI_VEC_CALL, ipi_call_handler);

    kprintf("[IPI] Handlers registered (TLB=0x%x, Sched=0x%x, Halt=0x%x)\n",
            IPI_VEC_TLB_SHOOTDOWN, IPI_VEC_SCHEDULE, IPI_VEC_HALT);
}

void ipi_reschedule(uint32_t cpu_id) {
    PerCpu *c = percpu_get(cpu_id);
    if (c == NULL || !c->online) return;
    lapic_send_ipi(c->apic_id, IPI_VEC_SCHEDULE);
}

void ipi_halt_all(void) {
    lapic_send_ipi_all_excluding_self(IPI_VEC_HALT);
}
