#ifndef ARCHOS_ARCH_X86_64_IOAPIC_H
#define ARCHOS_ARCH_X86_64_IOAPIC_H

#include <stdint.h>

/* I/O APIC register offsets (indirect access) */
#define IOAPIC_REGSEL   0x00
#define IOAPIC_IOWIN    0x10

/* I/O APIC registers */
#define IOAPIC_REG_ID      0x00
#define IOAPIC_REG_VER     0x01
#define IOAPIC_REG_REDIR   0x10  /* Redirection table base (entry N = 0x10 + 2*N) */

/* Redirection table entry flags */
#define IOAPIC_MASKED      (1ULL << 16)
#define IOAPIC_LEVEL       (1ULL << 15)
#define IOAPIC_ACTIVE_LOW  (1ULL << 13)

/* Initialize the I/O APIC. base_virt is the HHDM virtual address. */
void ioapic_init(uint64_t base_virt);

/* Route an IRQ to a specific vector on a specific APIC ID.
 * Applies polarity and trigger mode from MADT ISOs. */
void ioapic_route_irq(uint8_t irq, uint8_t vector, uint8_t dest_apic,
                       int active_low, int level_triggered);

/* Mask/unmask an I/O APIC entry */
void ioapic_mask(uint8_t irq);
void ioapic_unmask(uint8_t irq);

/* Read/write I/O APIC registers */
uint32_t ioapic_read(uint32_t reg);
void ioapic_write(uint32_t reg, uint32_t val);

#endif /* ARCHOS_ARCH_X86_64_IOAPIC_H */
