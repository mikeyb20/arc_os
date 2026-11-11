/* arc_os — GDT and TSS setup, one pair per CPU (M1-03)
 *
 * Owns: filling and loading a CPU's GDT (null, kernel code/data, user data/code
 * in the order SYSRET needs, then the 16-byte TSS descriptor).
 * Invariants: the storage lives in that CPU's PerCpu block and outlives the CPU;
 * each CPU loads only its own GDT, so LTR never finds a busy descriptor.
 * Locks: none (each CPU touches only its own block). */

#include "arch/x86_64/gdt.h"
#include "lib/mem.h"

static void gdt_set_entry(GDTEntry *gdt, int index, uint8_t access, uint8_t flags) {
    /* For 64-bit code/data segments, base and limit are ignored by the CPU.
     * We set limit=0xFFFFF with 4K granularity for convention. */
    gdt[index].limit_low   = 0xFFFF;
    gdt[index].base_low    = 0;
    gdt[index].base_mid    = 0;
    gdt[index].access      = access;
    gdt[index].granularity  = (flags << 4) | 0x0F;  /* flags:4 | limit_high:4 */
    gdt[index].base_high   = 0;
}

static void gdt_set_tss(GDTEntry *gdt, int index, uint64_t base, uint32_t limit) {
    TSSDescriptor *desc = (TSSDescriptor *)&gdt[index];
    desc->limit_low   = (uint16_t)(limit & 0xFFFF);
    desc->base_low    = (uint16_t)(base & 0xFFFF);
    desc->base_mid    = (uint8_t)((base >> 16) & 0xFF);
    desc->access      = GDT_ACCESS_TSS64;
    desc->granularity  = (uint8_t)((limit >> 16) & 0x0F);
    desc->base_high   = (uint8_t)((base >> 24) & 0xFF);
    desc->base_upper  = (uint32_t)(base >> 32);
    desc->reserved    = 0;
}

void gdt_init_cpu(GDTEntry gdt[GDT_ENTRY_COUNT], TSS *tss) {
    memset(gdt, 0, sizeof(GDTEntry) * GDT_ENTRY_COUNT);

    /* Entry 0: Null descriptor (required) */

    /* Entry 1 (0x08): Kernel code — DPL 0, executable, readable, long mode */
    gdt_set_entry(gdt, 1, GDT_ACCESS_PRESENT | GDT_ACCESS_SEGMENT | GDT_ACCESS_EXEC | GDT_ACCESS_RW,
                  GDT_FLAG_LONG_MODE);

    /* Entry 2 (0x10): Kernel data — DPL 0, writable */
    gdt_set_entry(gdt, 2, GDT_ACCESS_PRESENT | GDT_ACCESS_SEGMENT | GDT_ACCESS_RW, 0);

    /* Entry 3 (0x18): User data — DPL 3, writable */
    gdt_set_entry(gdt, 3, GDT_ACCESS_PRESENT | GDT_ACCESS_DPL3 | GDT_ACCESS_SEGMENT | GDT_ACCESS_RW, 0);

    /* Entry 4 (0x20): User code — DPL 3, executable, readable, long mode */
    gdt_set_entry(gdt, 4, GDT_ACCESS_PRESENT | GDT_ACCESS_DPL3 | GDT_ACCESS_SEGMENT | GDT_ACCESS_EXEC | GDT_ACCESS_RW,
                  GDT_FLAG_LONG_MODE);

    /* Entry 5-6 (0x28): TSS descriptor (16 bytes, spans two GDT slots) */
    tss->iomap_base = sizeof(TSS);
    gdt_set_tss(gdt, 5, (uint64_t)tss, sizeof(TSS) - 1);

    /* LGDT copies the pointer, so it can live on the stack */
    GDTPointer gdtr = {.limit = sizeof(GDTEntry) * GDT_ENTRY_COUNT - 1, .base = (uint64_t)gdt};
    gdt_flush(&gdtr, GDT_KERNEL_CODE, GDT_KERNEL_DATA, GDT_TSS);
}
