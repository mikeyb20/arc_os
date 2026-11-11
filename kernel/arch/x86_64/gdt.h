#ifndef ARCHOS_ARCH_X86_64_GDT_H
#define ARCHOS_ARCH_X86_64_GDT_H

#include <stdint.h>

/* Segment selector constants */
#define GDT_KERNEL_CODE  0x08
#define GDT_KERNEL_DATA  0x10
#define GDT_USER_DATA    0x18  /* Must be before user code for SYSRET */
#define GDT_USER_CODE    0x20
#define GDT_TSS          0x28

/* GDT access byte fields */
#define GDT_ACCESS_PRESENT    0x80  /* Segment present */
#define GDT_ACCESS_DPL3       0x60  /* Descriptor privilege level 3 */
#define GDT_ACCESS_SEGMENT    0x10  /* Code/data segment (not system) */
#define GDT_ACCESS_EXEC       0x08  /* Executable (code segment) */
#define GDT_ACCESS_RW         0x02  /* Readable (code) / Writable (data) */

/* GDT flags (upper nibble of granularity byte) */
#define GDT_FLAG_LONG_MODE    0x2   /* 64-bit code segment */

/* TSS access byte */
#define GDT_ACCESS_TSS64      0x89  /* Present, 64-bit TSS (available) */

/* GDT entry (8 bytes) */
typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;  /* flags:4 | limit_high:4 */
    uint8_t  base_high;
} GDTEntry;

/* TSS descriptor is 16 bytes in long mode (two consecutive GDT slots) */
typedef struct __attribute__((packed)) {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} TSSDescriptor;

/* Task State Segment — used for RSP0 (ring 3→0) and IST entries */
#define TSS_SIZE 104
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;       /* Stack for ring 0 transitions */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];     /* IST1..IST7 stack tops (ist[0] = IST1; slots in idt.h) */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} TSS;
_Static_assert(sizeof(TSS) == TSS_SIZE, "64-bit TSS layout");

/* GDTR pointer loaded by lgdt */
typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} GDTPointer;

/* 5 segment descriptors + the TSS descriptor (two slots) */
#define GDT_ENTRY_COUNT 7

/* Fill this CPU's GDT (kernel/user code and data, a TSS descriptor for tss)
 * and load it: LGDT, segment reloads, LTR. Each CPU has its own GDT and TSS
 * (in its PerCpu block, M1-03), because LTR marks the descriptor busy. The
 * caller sets the TSS fields (RSP0, IST pointers); tss->iomap_base is set here. */
void gdt_init_cpu(GDTEntry gdt[GDT_ENTRY_COUNT], TSS *tss);

/* Assembly routine: load GDT, reload segment registers, load TSS. */
extern void gdt_flush(const GDTPointer *gdtr, uint16_t code_sel, uint16_t data_sel, uint16_t tss_sel);

#endif /* ARCHOS_ARCH_X86_64_GDT_H */
