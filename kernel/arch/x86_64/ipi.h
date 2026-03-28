#ifndef ARCHOS_ARCH_X86_64_IPI_H
#define ARCHOS_ARCH_X86_64_IPI_H

#include <stdint.h>

/* IPI vector numbers (reserved range 0xF0-0xF3) */
#define IPI_VEC_TLB_SHOOTDOWN  0xF0
#define IPI_VEC_SCHEDULE       0xF1
#define IPI_VEC_HALT           0xF2

/* Initialize IPI handlers. Call after IDT and LAPIC are set up. */
void ipi_init(void);

/* Send a TLB shootdown IPI to all other CPUs.
 * Called after vmm_unmap_page to ensure stale TLB entries are flushed. */
void ipi_tlb_shootdown(void);

/* Send a reschedule IPI to a specific CPU. */
void ipi_reschedule(uint32_t cpu_id);

/* Send a halt IPI to all other CPUs. */
void ipi_halt_all(void);

#endif /* ARCHOS_ARCH_X86_64_IPI_H */
