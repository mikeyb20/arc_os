#ifndef ARCHOS_ARCH_X86_64_SMP_H
#define ARCHOS_ARCH_X86_64_SMP_H

#include <stdint.h>
#include "boot/bootinfo.h"

/* Initialize SMP: bring up Application Processors using Limine SMP protocol.
 * Must be called after LAPIC, I/O APIC, and per-CPU data are initialized.
 * Returns total number of CPUs online. */
uint32_t smp_init(void);

/* Check if SMP is active (more than 1 CPU online). */
int smp_active(void);

/* Get total number of online CPUs. */
uint32_t smp_cpu_count(void);

#endif /* ARCHOS_ARCH_X86_64_SMP_H */
