#ifndef ARCHOS_ARCH_X86_64_IRQ_VECTOR_H
#define ARCHOS_ARCH_X86_64_IRQ_VECTOR_H

#include <stdint.h>

/* Device interrupt vectors, handed out per CPU (M1-09).
 *
 * 0x30-0xDF on every CPU belong to MSI and MSI-X interrupts: the same vector
 * number on two CPUs is two different interrupts. Below them are the CPU
 * exceptions (0-31) and the ISA lines from the PIC or I/O APIC (32-47); above
 * them the slice timer (0xEF), the IPIs (0xF0-0xF3) and the spurious vector
 * (0xFF). Portable code reaches this through arch/hal.h. */

#define IRQ_VECTOR_FIRST 0x30
#define IRQ_VECTOR_LAST  0xDF
#define IRQ_VECTOR_COUNT (IRQ_VECTOR_LAST - IRQ_VECTOR_FIRST + 1)

/* Called with interrupts off, before the local APIC EOI, for every device
 * vector that has a cookie. */
typedef void (*irq_vector_entry_t)(void *cookie);

/* Set the function every bound device vector calls (the generic IRQ layer). */
void irq_vector_set_entry(irq_vector_entry_t fn);

/* Take a free vector on CPU index cpu and bind cookie (not NULL) to it.
 * Returns the vector, -EINVAL (no such CPU), -ENOMEM or -ENOSPC. Thread
 * context only: may allocate the CPU's table. */
int irq_vector_alloc(uint32_t cpu, void *cookie);

/* Give a vector back. A late interrupt on it is counted as spurious. */
void irq_vector_free(uint32_t cpu, int vector);

/* isr_dispatch: run the vector's cookie through the entry function, then EOI. */
void irq_vector_dispatch(uint8_t vector);

/* MSI address and data that deliver vector to CPU index cpu: fixed delivery,
 * edge, physical destination. Returns 0, -EINVAL, or -ERANGE when the CPU's
 * APIC ID is above 255 (that needs interrupt remapping, out of scope). The
 * format is the same in xAPIC and x2APIC mode for IDs below 256. */
int irq_msi_compose(uint32_t cpu, int vector, uint64_t *addr, uint32_t *data);

/* Device vectors that arrived with nothing bound to them. */
uint64_t irq_vector_spurious(void);

/* Pure helpers (host-tested): the MSI address and data words for an APIC ID
 * below 256 and a vector. */
static inline uint64_t irq_msi_address(uint32_t apic_id) {
    return 0xFEE00000ULL | ((uint64_t)(apic_id & 0xFF) << 12);
}
static inline uint32_t irq_msi_data(int vector) {
    return (uint32_t)vector & 0xFF;   /* fixed delivery, edge trigger */
}

#endif /* ARCHOS_ARCH_X86_64_IRQ_VECTOR_H */
