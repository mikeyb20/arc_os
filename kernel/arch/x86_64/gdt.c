#include "arch/x86_64/gdt.h"
#include "lib/mem.h"
#include "lib/kprintf.h"

/* 5 standard entries + 1 TSS descriptor (occupies 2 slots) = 7 slots */
#define GDT_ENTRY_COUNT 7

static GDTEntry gdt[GDT_ENTRY_COUNT];
static TSS tss;
static GDTPointer gdtr;

/* Double-fault IST stack (4 KB) */
static uint8_t df_stack[4096] __attribute__((aligned(16)));

static void gdt_set_entry(int index, uint8_t access, uint8_t flags) {
    /* For 64-bit code/data segments, base and limit are ignored by the CPU.
     * We set limit=0xFFFFF with 4K granularity for convention. */
    gdt[index].limit_low   = 0xFFFF;
    gdt[index].base_low    = 0;
    gdt[index].base_mid    = 0;
    gdt[index].access      = access;
    gdt[index].granularity  = (flags << 4) | 0x0F;  /* flags:4 | limit_high:4 */
    gdt[index].base_high   = 0;
}

static void gdt_set_tss(int index, uint64_t base, uint32_t limit) {
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

void gdt_init(void) {
    /* Zero everything */
    memset(gdt, 0, sizeof(gdt));
    memset(&tss, 0, sizeof(tss));

    /* Entry 0: Null descriptor (required) */

    /* Entry 1 (0x08): Kernel code — DPL 0, executable, readable, long mode */
    gdt_set_entry(1, GDT_ACCESS_PRESENT | GDT_ACCESS_SEGMENT | GDT_ACCESS_EXEC | GDT_ACCESS_RW, GDT_FLAG_LONG_MODE);

    /* Entry 2 (0x10): Kernel data — DPL 0, writable */
    gdt_set_entry(2, GDT_ACCESS_PRESENT | GDT_ACCESS_SEGMENT | GDT_ACCESS_RW, 0);

    /* Entry 3 (0x18): User data — DPL 3, writable */
    gdt_set_entry(3, GDT_ACCESS_PRESENT | GDT_ACCESS_DPL3 | GDT_ACCESS_SEGMENT | GDT_ACCESS_RW, 0);

    /* Entry 4 (0x20): User code — DPL 3, executable, readable, long mode */
    gdt_set_entry(4, GDT_ACCESS_PRESENT | GDT_ACCESS_DPL3 | GDT_ACCESS_SEGMENT | GDT_ACCESS_EXEC | GDT_ACCESS_RW, GDT_FLAG_LONG_MODE);

    /* Entry 5-6 (0x28): TSS descriptor (16 bytes, spans two GDT slots) */
    tss.iomap_base = sizeof(TSS);
    tss.ist1 = (uint64_t)(df_stack + sizeof(df_stack));  /* Top of stack */
    gdt_set_tss(5, (uint64_t)&tss, sizeof(TSS) - 1);

    /* Load GDTR */
    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base  = (uint64_t)gdt;

    gdt_flush(&gdtr, GDT_KERNEL_CODE, GDT_KERNEL_DATA, GDT_TSS);

    kprintf("[HAL] GDT loaded (%d entries + TSS)\n", GDT_ENTRY_COUNT);
}

void gdt_set_kernel_stack(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}
