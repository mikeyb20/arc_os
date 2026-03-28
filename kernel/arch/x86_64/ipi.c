/* arc_os — Inter-Processor Interrupt infrastructure
 *
 * Provides TLB shootdown, cross-core reschedule, and halt IPIs. */

#include "arch/x86_64/ipi.h"
#include "arch/x86_64/isr.h"
#include "arch/x86_64/lapic.h"
#include "arch/x86_64/percpu.h"
#include "lib/kprintf.h"

/* TLB shootdown handler — flush TLB on this CPU */
static void ipi_tlb_handler(InterruptFrame *frame) {
    (void)frame;
    /* Flush entire TLB by reloading CR3 */
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
    lapic_eoi();
}

/* Reschedule handler — trigger scheduler on this CPU */
static void ipi_schedule_handler(InterruptFrame *frame) {
    (void)frame;
    lapic_eoi();
    /* The timer IRQ handler normally calls sched_schedule().
     * For cross-core wakeup, we just need to mark that a reschedule
     * is needed. The next timer tick will pick it up. */
}

/* Halt handler — stop this CPU */
static void ipi_halt_handler(InterruptFrame *frame) {
    (void)frame;
    lapic_eoi();
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

void ipi_init(void) {
    isr_register_handler(IPI_VEC_TLB_SHOOTDOWN, ipi_tlb_handler);
    isr_register_handler(IPI_VEC_SCHEDULE, ipi_schedule_handler);
    isr_register_handler(IPI_VEC_HALT, ipi_halt_handler);

    kprintf("[IPI] Handlers registered (TLB=0x%x, Sched=0x%x, Halt=0x%x)\n",
            IPI_VEC_TLB_SHOOTDOWN, IPI_VEC_SCHEDULE, IPI_VEC_HALT);
}

void ipi_tlb_shootdown(void) {
    if (cpu_count <= 1) return;
    lapic_send_ipi_all_excluding_self(IPI_VEC_TLB_SHOOTDOWN);
}

void ipi_reschedule(uint32_t cpu_id) {
    if (cpu_id >= cpu_count) return;
    lapic_send_ipi(percpu_data[cpu_id].apic_id, IPI_VEC_SCHEDULE);
}

void ipi_halt_all(void) {
    lapic_send_ipi_all_excluding_self(IPI_VEC_HALT);
}
