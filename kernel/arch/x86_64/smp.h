#ifndef ARCHOS_ARCH_X86_64_SMP_H
#define ARCHOS_ARCH_X86_64_SMP_H

#include <stdint.h>
#include "boot/bootinfo.h"
#include "drivers/acpi.h"

/* Switch interrupt delivery to the local APIC (xAPIC or x2APIC) + I/O APIC
 * (ISA IRQs to the BSP), register the IPI handlers, then give every other
 * enabled MADT CPU a per-CPU block and start it through the bootloader. Call
 * once on the BSP, after the page allocator, with interrupts off. Returns the
 * number of CPUs online. */
uint32_t smp_init(const BootInfo *info, const AcpiInfo *acpi);

/* Let the APs (idling since smp_init) enter sched_ap_start. BSP, once. */
void smp_release_aps(void);

/* Check if SMP is active (more than 1 CPU online). */
int smp_active(void);

/* Get total number of online CPUs. */
uint32_t smp_cpu_count(void);

#endif /* ARCHOS_ARCH_X86_64_SMP_H */
