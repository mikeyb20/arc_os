#ifndef ARCHOS_ARCH_X86_64_LAPIC_H
#define ARCHOS_ARCH_X86_64_LAPIC_H

#include <stdint.h>

/* Local APIC access layer (M1-03): xAPIC (MMIO) or x2APIC (MSRs), chosen once
 * on the BSP and used by every CPU. Callers use register offsets as in the
 * xAPIC MMIO page; in x2APIC mode register r is MSR 0x800 + (r >> 4). */

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

/* ICR delivery status (xAPIC only: x2APIC sends at once) */
#define ICR_SEND_PENDING 0x01000

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

/* Choose the mode on the BSP and enable its local APIC. x2APIC when CPUID
 * reports it (and always when the firmware or bootloader already enabled it);
 * xAPIC otherwise, through the HHDM mapping of mmio_phys (from the MADT). */
void lapic_init_bsp(uint64_t mmio_phys);

/* Enable this application processor's local APIC in the BSP's mode. */
void lapic_init_ap(void);

/* 1 when the APIC layer runs in x2APIC mode. */
int lapic_x2apic(void);

/* 1 once lapic_init_bsp ran: interrupts (MSI included) go through the local APIC. */
int lapic_active(void);

/* Send End-of-Interrupt. Must be called after handling every LAPIC interrupt. */
void lapic_eoi(void);

/* Read the current CPU's APIC ID: 32 bits in x2APIC mode, 8 in xAPIC mode. */
uint32_t lapic_id(void);

/* Send a fixed IPI to one APIC ID. In xAPIC mode only IDs below 256 exist. */
void lapic_send_ipi(uint32_t apic_id, uint32_t vector);

/* Send a fixed IPI to all CPUs except self. */
void lapic_send_ipi_all_excluding_self(uint32_t vector);

/* Vector of the local APIC timer: each CPU's clock-event interrupt (M1-06) */
#define LAPIC_TIMER_VEC 0xEF

/* Clock events (M1-06): one interrupt per CPU at a programmed TSC value.
 * TSC-deadline mode when the CPU has it, else one-shot mode with the count
 * worked out from the TSC (the APIC timer is measured against it once).
 * lapic_timer_setup: BSP, once, after the TSC is calibrated: measures the
 * timer if needed and makes fn the handler on every CPU (interrupts off,
 * after the EOI). The rest act on the calling CPU, interrupts off. */
void lapic_timer_setup(void (*fn)(void), int use_deadline);
void lapic_timer_cpu_init(void);
/* Interrupt once the TSC reaches tsc (at once if it already has). A one-shot
 * timer that can't reach that far fires early; the caller reprograms. */
void lapic_timer_program(uint64_t tsc);
void lapic_timer_stop(void);
/* "TSC-deadline" or "LAPIC one-shot" (after lapic_timer_setup). */
const char *lapic_timer_mode(void);
/* Timer ticks per second at divide 16 (0 in TSC-deadline mode: not measured). */
uint64_t lapic_timer_hz(void);

/* Read a LAPIC register (xAPIC MMIO offset). */
uint32_t lapic_read(uint32_t reg);

/* Write a LAPIC register (xAPIC MMIO offset). */
void lapic_write(uint32_t reg, uint32_t val);

#endif /* ARCHOS_ARCH_X86_64_LAPIC_H */
