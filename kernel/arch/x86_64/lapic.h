#ifndef ARCHOS_ARCH_X86_64_LAPIC_H
#define ARCHOS_ARCH_X86_64_LAPIC_H

#include <stdint.h>

/* LAPIC register offsets (memory-mapped) */
#define LAPIC_ID          0x020
#define LAPIC_VERSION     0x030
#define LAPIC_TPR         0x080
#define LAPIC_EOI         0x0B0
#define LAPIC_SVR         0x0F0
#define LAPIC_ESR         0x280
#define LAPIC_ICR_LOW     0x300
#define LAPIC_ICR_HIGH    0x310
#define LAPIC_TIMER_LVT   0x320
#define LAPIC_TIMER_ICR   0x380
#define LAPIC_TIMER_CCR   0x390
#define LAPIC_TIMER_DCR   0x3E0

/* SVR flags */
#define LAPIC_SVR_ENABLE  0x100
#define LAPIC_SPURIOUS_VEC 0xFF

/* ICR delivery modes */
#define ICR_FIXED         0x00000
#define ICR_INIT          0x00500
#define ICR_STARTUP       0x00600

/* ICR destination shorthand */
#define ICR_DEST_FIELD    0x00000
#define ICR_DEST_SELF     0x40000
#define ICR_DEST_ALL      0x80000
#define ICR_DEST_ALL_EX   0xC0000

/* ICR level/trigger */
#define ICR_LEVEL_ASSERT  0x04000
#define ICR_LEVEL_DEASSERT 0x00000

/* Timer divide values */
#define LAPIC_TIMER_DIV_16  0x03

/* Initialize the local APIC. addr is the HHDM virtual address. */
void lapic_init(uint64_t base_virt);

/* Send End-of-Interrupt. Must be called after handling every LAPIC interrupt. */
void lapic_eoi(void);

/* Read the current CPU's APIC ID. */
uint32_t lapic_id(void);

/* Send an IPI to a specific APIC ID with the given vector. */
void lapic_send_ipi(uint32_t apic_id, uint32_t vector);

/* Send an IPI to all CPUs (including self). */
void lapic_send_ipi_all(uint32_t vector);

/* Send an IPI to all CPUs except self. */
void lapic_send_ipi_all_excluding_self(uint32_t vector);

/* Initialize the LAPIC timer in periodic mode with the given vector.
 * freq_hz is the desired tick frequency (e.g., 100 for 100 Hz). */
void lapic_timer_init(uint32_t vector, uint32_t freq_hz);

/* Read a LAPIC register. */
uint32_t lapic_read(uint32_t reg);

/* Write a LAPIC register. */
void lapic_write(uint32_t reg, uint32_t val);

#endif /* ARCHOS_ARCH_X86_64_LAPIC_H */
