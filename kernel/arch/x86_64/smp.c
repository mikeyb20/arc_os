/* arc_os — Interrupt controllers and application processor bring-up (M1-03)
 *
 * Owns: switching interrupt delivery from the 8259 PIC to the local APIC +
 * I/O APIC, giving each CPU an index and a PerCpu block, and starting the APs.
 * CPU indexes: 0 = BSP, then the MADT's enabled CPUs in table order (MADT types
 * 0 and 9). The bootloader starts each AP (boot_start_cpu); the kernel never
 * sees which bootloader that is.
 * Invariants: every CPU runs in the APIC mode the BSP chose; an AP is counted
 * in cpu_count only after its GDT, TSS, IDT, GS, SYSCALL MSRs, FPU and APIC are
 * set up. APs stay in HLT with interrupts on until M1-05 gives them threads.
 * Locks: none. smp_init runs on the BSP before interrupts are enabled; APs only
 * write their own PerCpu block and the aps_online counter (atomic). */

#include "arch/x86_64/smp.h"
#include "arch/x86_64/fpu.h"
#include "arch/x86_64/percpu.h"
#include "arch/x86_64/lapic.h"
#include "arch/x86_64/ioapic.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/isr.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/ipi.h"
#include "arch/x86_64/syscall.h"
#include "arch/x86_64/cpuid.h"
#include "mm/vmm.h"
#include "lib/kprintf.h"

_Static_assert(ACPI_MAX_CPUS == MAX_CPUS, "MADT CPU list and per-CPU table sizes");
_Static_assert(BOOTINFO_MAX_CPUS == MAX_CPUS, "boot CPU list and per-CPU table sizes");

/* How long the BSP waits for the APs: 2^32 TSC cycles, about 1-4 s at the TSC
 * rates of real and emulated CPUs. Bounded by time, not by a loop count, so
 * TCG (slow) and KVM (fast) wait alike. */
#define AP_WAIT_TSC_CYCLES (1ULL << 32)

static volatile uint32_t aps_online;

/* CPU index each AP read back through its own GS base (proves GS is per CPU).
 * The BSP prints them: APs don't print, so the log stays readable. */
static uint8_t gs_index_seen[MAX_CPUS];

/* AP entry: called through the bootloader on its small stack, interrupts off.
 * arg = this CPU's index. */
static void ap_entry(uint64_t arg) {
    PerCpu *cpu = percpu_get((uint32_t)arg);

    percpu_load(cpu);      /* GDT + TSS (IST stacks), GS base */
    idt_load();            /* the shared IDT */
    syscall_cpu_init();    /* SYSCALL MSRs are per CPU */
    fpu_cpu_init(0);       /* XSAVE on, FS base 0, like the BSP */
    lapic_init_ap();       /* in the BSP's APIC mode */

    gs_index_seen[cpu->cpu_index] = (uint8_t)this_cpu()->cpu_index;   /* < MAX_CPUS = 256 */
    cpu->apic_id = lapic_id();   /* what the hardware says (the MADT agreed) */
    cpu->online = 1;
    __atomic_add_fetch(&aps_online, 1, __ATOMIC_SEQ_CST);   /* publishes the stores above */

    /* AP idle loop: M1-05 gives application processors threads */
    __asm__ volatile ("sti");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

/* Switch from the PIC to the local APIC + I/O APIC on the BSP. */
static void smp_apic_setup(const AcpiInfo *acpi) {
    lapic_init_bsp(acpi->local_apic_address);
    uint32_t bsp_apic = lapic_id();
    percpu_set_bsp_apic_id(bsp_apic);

    if (acpi->io_apic_address != 0) {
        ioapic_init(acpi->io_apic_address + vmm_get_hhdm_offset());
        if (bsp_apic > 0xFF) {
            /* I/O APIC destinations are 8-bit without interrupt remapping (M1-09) */
            kprintf("[SMP] WARNING: BSP APIC ID %u above 255, ISA IRQs not routed\n", bsp_apic);
        } else {
            ioapic_route_isa(acpi, (uint8_t)bsp_apic);
        }
        pic_disable();
        isr_set_apic_mode(1);
        kprintf("[SMP] Switched from PIC to APIC mode\n");
    }
    ipi_init();
}

/* Index of APIC ID apic_id in the bootloader's CPU list, or -1. Boot only. */
static int boot_cpu_slot(const BootInfo *info, uint32_t apic_id) {
    for (uint32_t i = 0; i < info->cpu_count; i++) {
        if (info->cpu_apic_ids[i] == apic_id) return (int)i;
    }
    return -1;
}

/* Allocate blocks for the MADT's other CPUs and start them. Returns how many
 * were started. */
static uint32_t smp_start_aps(const BootInfo *info, const AcpiInfo *acpi) {
    uint32_t bsp_apic = this_cpu()->apic_id;
    uint32_t next_index = 1;
    uint32_t started = 0;

    for (uint32_t i = 0; i < acpi->cpu_count; i++) {
        uint32_t apic_id = acpi->cpu_apic_ids[i];
        if (apic_id == bsp_apic) continue;
        if (!lapic_x2apic() && apic_id > 0xFF) continue;   /* unreachable in xAPIC mode */
        int slot = boot_cpu_slot(info, apic_id);
        if (slot < 0) {
            kprintf("[SMP] CPU with APIC ID %u: the bootloader cannot start it\n", apic_id);
            continue;
        }
        PerCpu *cpu = percpu_alloc(next_index, apic_id);
        if (cpu == NULL) {
            kprintf("[SMP] Out of memory for CPU %u's per-CPU block\n", next_index);
            break;
        }
        next_index++;
        if (boot_start_cpu((uint32_t)slot, ap_entry, cpu->cpu_index) == 0) started++;
    }
    return started;
}

uint32_t smp_init(const BootInfo *info, const AcpiInfo *acpi) {
    smp_apic_setup(acpi);

    kprintf("[SMP] MADT lists %u CPUs, bootloader %u, BSP APIC ID %u\n", acpi->cpu_count, info->cpu_count,
            this_cpu()->apic_id);
    if (acpi->cpu_count <= 1 || info->cpu_count <= 1) {
        kprintf("[SMP] Single-CPU system, SMP not needed\n");
        return cpu_count;
    }

    uint32_t started = smp_start_aps(info, acpi);

    /* Wait for the started APs to come online, bounded by time */
    uint64_t start = cpu_rdtsc();
    while (__atomic_load_n(&aps_online, __ATOMIC_SEQ_CST) < started && cpu_rdtsc() - start < AP_WAIT_TSC_CYCLES) {
        __asm__ volatile ("pause");
    }

    uint32_t online = __atomic_load_n(&aps_online, __ATOMIC_SEQ_CST);
    cpu_count = 1 + online;
    if (online < started) {
        kprintf("[SMP] WARNING: %u of %u APs came online\n", online, started);
    }
    for (uint32_t i = 1; i < MAX_CPUS; i++) {
        PerCpu *cpu = percpu_get(i);
        if (cpu == NULL) break;
        if (cpu->online) {
            kprintf("[SMP] CPU %u (APIC %u) online, GS index %u\n", i, cpu->apic_id, gs_index_seen[i]);
        }
    }
    kprintf("[SMP] All %u CPUs online (%s)\n", cpu_count, lapic_x2apic() ? "x2APIC" : "xAPIC");
    return cpu_count;
}

int smp_active(void) {
    return cpu_count > 1;
}

uint32_t smp_cpu_count(void) {
    return cpu_count;
}
