/* arc_os — SMP AP bringup via Limine SMP protocol
 *
 * Limine provides a goto_address field per CPU. The BSP writes
 * our ap_entry function pointer to it, and the AP wakes up. */

#include "arch/x86_64/smp.h"
#include "arch/x86_64/percpu.h"
#include "arch/x86_64/lapic.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "mm/vmm.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "lib/kprintf.h"
#include "lib/mem.h"
#include <limine.h>
#include <stddef.h>

/* Limine SMP request — defined in limine_requests.c */
extern volatile struct limine_smp_request smp_request;

static volatile uint32_t aps_online;
static uint32_t total_cpus;

/* AP entry point — called by Limine when BSP writes goto_address.
 * Runs on the AP's bootstrap stack provided by Limine. */
static void ap_entry(struct limine_smp_info *info) {
    uint32_t cpu_id = (uint32_t)info->extra_argument;
    uint32_t apic_id = info->lapic_id;

    /* Initialize per-CPU data for this AP */
    percpu_init_ap(cpu_id, apic_id);

    /* Load GDT and IDT (shared IDT, per-CPU GDT could be done later) */
    /* For now, APs use the BSP's GDT and IDT since they're the same */

    /* Enable LAPIC on this AP */
    uint64_t hhdm = vmm_get_hhdm_offset();
    lapic_init(0xFEE00000 + hhdm);  /* Standard LAPIC address */

    /* Mark this AP as online */
    percpu_data[cpu_id].online = 1;
    __atomic_add_fetch(&aps_online, 1, __ATOMIC_SEQ_CST);

    kprintf("[SMP] CPU %u (APIC %u) online\n", cpu_id, apic_id);

    /* AP idle loop — wait for scheduler to assign work */
    __asm__ volatile ("sti");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

uint32_t smp_init(void) {
    struct limine_smp_response *resp = smp_request.response;
    if (!resp) {
        kprintf("[SMP] No SMP response from bootloader\n");
        return 1;
    }

    total_cpus = (uint32_t)resp->cpu_count;
    aps_online = 0;

    kprintf("[SMP] Detected %u CPUs, BSP LAPIC ID=%u\n",
            total_cpus, resp->bsp_lapic_id);

    if (total_cpus <= 1) {
        kprintf("[SMP] Single-CPU system, SMP not needed\n");
        return 1;
    }

    /* Wake up each AP */
    uint32_t ap_count = 0;
    for (uint64_t i = 0; i < resp->cpu_count; i++) {
        struct limine_smp_info *cpu = resp->cpus[i];

        /* Skip BSP */
        if (cpu->lapic_id == resp->bsp_lapic_id) continue;

        ap_count++;
        uint32_t cpu_id = ap_count; /* CPU 0 = BSP, 1+ = APs */
        cpu->extra_argument = cpu_id;

        /* Write goto_address to wake the AP */
        __atomic_store_n(&cpu->goto_address, ap_entry, __ATOMIC_SEQ_CST);
    }

    /* Wait for all APs to come online (with timeout) */
    for (volatile int wait = 0; wait < 10000000; wait++) {
        if (__atomic_load_n(&aps_online, __ATOMIC_SEQ_CST) >= ap_count) break;
        __asm__ volatile ("pause");
    }

    cpu_count = 1 + __atomic_load_n(&aps_online, __ATOMIC_SEQ_CST);
    kprintf("[SMP] All %u CPUs online\n", cpu_count);

    return cpu_count;
}

int smp_active(void) {
    return cpu_count > 1;
}

uint32_t smp_cpu_count(void) {
    return cpu_count;
}
