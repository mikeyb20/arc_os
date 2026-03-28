/* arc_os — Per-CPU data infrastructure
 *
 * Each CPU has a PerCpu struct accessible via the GS segment base MSR.
 * BSP initializes first; APs call percpu_init_ap on startup. */

#include "arch/x86_64/percpu.h"
#include "arch/x86_64/gdt.h"
#include "lib/mem.h"
#include "lib/kprintf.h"

PerCpu percpu_data[MAX_CPUS];
uint32_t cpu_count = 1;  /* BSP counts as 1 */

void percpu_init_bsp(void) {
    PerCpu *bsp = &percpu_data[0];
    memset(bsp, 0, sizeof(PerCpu));

    bsp->cpu_id = 0;
    bsp->apic_id = 0;  /* Will be updated by LAPIC init */
    bsp->current_thread = thread_current();
    bsp->kernel_rsp = 0;
    bsp->idle_thread = NULL;
    bsp->run_queue_head = NULL;
    bsp->run_queue_tail = NULL;
    bsp->online = 1;

    /* Set GS base to point to this CPU's data */
    percpu_set_gs_base(bsp);

    kprintf("[PERCPU] BSP (CPU 0) initialized\n");
}

void percpu_init_ap(uint32_t cpu_id, uint32_t apic_id) {
    if (cpu_id >= MAX_CPUS) return;

    PerCpu *ap = &percpu_data[cpu_id];
    memset(ap, 0, sizeof(PerCpu));

    ap->cpu_id = cpu_id;
    ap->apic_id = apic_id;
    ap->current_thread = NULL;
    ap->kernel_rsp = 0;
    ap->idle_thread = NULL;
    ap->run_queue_head = NULL;
    ap->run_queue_tail = NULL;
    ap->online = 0;

    percpu_set_gs_base(ap);
}
