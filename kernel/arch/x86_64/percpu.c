/* arc_os — Per-CPU blocks (M1-03)
 *
 * Owns: each CPU's PerCpu block and IST stacks, the index -> block table and
 * the APIC ID -> index map.
 * Invariants: GS base = the running CPU's block whenever the CPU is in kernel
 * mode (percpu.h explains the SWAPGS protocol); a block never moves or goes
 * away once its CPU is up; percpu_alloc and the map are written only by the BSP
 * during boot, before the CPU they describe starts, and read-only after.
 * Locks: none. Only the owning CPU writes its block (except the BSP filling it
 * before the CPU runs). */

#include "arch/x86_64/percpu.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/msr.h"
#include "mm/pmm.h"
#include "mm/page.h"
#include "mm/gfp.h"
#include "lib/mem.h"
#include "lib/kprintf.h"

_Static_assert(offsetof(PerCpu, self) == PERCPU_SELF, "PerCpu layout (syscall_entry.asm)");
_Static_assert(offsetof(PerCpu, kernel_rsp) == PERCPU_KERNEL_RSP, "PerCpu layout (syscall_entry.asm)");
_Static_assert(offsetof(PerCpu, user_rsp) == PERCPU_USER_RSP, "PerCpu layout (syscall_entry.asm)");
_Static_assert(offsetof(PerCpu, current_thread) == PERCPU_CURRENT, "PerCpu layout (percpu.h)");
_Static_assert(offsetof(PerCpu, online) < 64, "PerCpu hot fields share one cache line");
_Static_assert(sizeof(PerCpu) <= PERCPU_AREA_SIZE - 3 * PERCPU_IST_SIZE, "PerCpu fits its page");

/* AP area layout: [#DF stack][NMI stack][#MC stack][PerCpu]. The BSP's block
 * and stacks are static (it needs them before the page allocator exists). */
#define AREA_PERCPU_OFFSET (3 * PERCPU_IST_SIZE)

static uint8_t bsp_ist_stacks[3 * PERCPU_IST_SIZE] __attribute__((aligned(16)));
static PerCpu bsp_block;

static PerCpu *percpu_table[MAX_CPUS];
uint32_t cpu_count = 1;  /* BSP counts as 1 */

/* APIC ID -> CPU index, sorted by APIC ID (APIC IDs are sparse 32-bit values) */
typedef struct {
    uint32_t apic_id;
    uint32_t index;
} ApicMapEntry;
static ApicMapEntry apic_map[MAX_CPUS];
static uint32_t apic_map_count;

static void apic_map_add(uint32_t apic_id, uint32_t index) {
    if (apic_map_count >= MAX_CPUS || percpu_index_of_apic(apic_id) >= 0) return;
    uint32_t pos = apic_map_count;
    while (pos > 0 && apic_map[pos - 1].apic_id > apic_id) {   /* boot only, <= 256 entries */
        apic_map[pos] = apic_map[pos - 1];
        pos--;
    }
    apic_map[pos].apic_id = apic_id;
    apic_map[pos].index = index;
    apic_map_count++;
}

int percpu_index_of_apic(uint32_t apic_id) {
    uint32_t lo = 0, hi = apic_map_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (apic_map[mid].apic_id == apic_id) return (int)apic_map[mid].index;
        if (apic_map[mid].apic_id < apic_id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

/* Fill a block: identity, TSS with the three IST stacks (3 * PERCPU_IST_SIZE
 * bytes at ist). */
static PerCpu *percpu_setup(PerCpu *c, uint8_t *ist, uint32_t index, uint32_t apic_id) {
    memset(c, 0, sizeof(*c));
    c->self = c;
    c->cpu_index = index;
    c->apic_id = apic_id;
    c->tss = &c->tss_storage;
    c->tss->ist[IST_DOUBLE_FAULT - 1] = (uint64_t)(ist + 1 * PERCPU_IST_SIZE);
    c->tss->ist[IST_NMI - 1] = (uint64_t)(ist + 2 * PERCPU_IST_SIZE);
    c->tss->ist[IST_MACHINE_CHECK - 1] = (uint64_t)(ist + 3 * PERCPU_IST_SIZE);
    percpu_table[index] = c;
    return c;
}

void percpu_load(PerCpu *cpu) {
    /* GDT first: gdt_flush reloads GS with the null selector, which clears GS base */
    gdt_init_cpu(cpu->gdt, cpu->tss);
    wrmsr(MSR_GS_BASE, (uint64_t)cpu);
    wrmsr(MSR_KERNEL_GS_BASE, 0);   /* the user GS base, swapped in on the way out */
    cpu->core_type = cpu_core_type_detect();   /* runs on the CPU itself */
}

void percpu_init_bsp(void) {
    PerCpu *bsp = percpu_setup(&bsp_block, bsp_ist_stacks, 0, 0);
    percpu_load(bsp);
    bsp->online = 1;
    kprintf("[PERCPU] BSP (CPU 0) block at %p, GS base set\n", (void *)bsp);
}

void percpu_set_bsp_apic_id(uint32_t apic_id) {
    percpu_table[0]->apic_id = apic_id;
    apic_map_add(apic_id, 0);
}

PerCpu *percpu_alloc(uint32_t index, uint32_t apic_id) {
    if (index == 0 || index >= MAX_CPUS || percpu_table[index] != NULL) return NULL;
    struct page *pg = alloc_pages(PERCPU_AREA_ORDER, GFP_KERNEL | GFP_ZERO);
    if (pg == NULL) return NULL;
    uint8_t *area = page_to_virt(pg);
    PerCpu *c = percpu_setup((PerCpu *)(area + AREA_PERCPU_OFFSET), area, index, apic_id);
    apic_map_add(apic_id, index);
    return c;
}

uint32_t percpu_current_index(void) {
    return percpu_table[0] != NULL ? this_cpu()->cpu_index : 0;
}

PerCpu *percpu_get(uint32_t index) {
    return index < MAX_CPUS ? percpu_table[index] : NULL;
}

int cpu_describe(uint32_t cpu, uint32_t *apic_id, uint8_t *core_type) {
    PerCpu *c = percpu_get(cpu);
    if (c == NULL || !c->online) return -1;
    *apic_id = c->apic_id;
    *core_type = c->core_type;
    return 0;
}
