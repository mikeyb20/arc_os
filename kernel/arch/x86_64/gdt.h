#ifndef ARCHOS_ARCH_X86_64_GDT_H
#define ARCHOS_ARCH_X86_64_GDT_H

#include <stdint.h>

/* Segment selector constants */
#define GDT_KERNEL_CODE  0x08
#define GDT_KERNEL_DATA  0x10
#define GDT_USER_DATA    0x18  /* Must be before user code for SYSRET */
#define GDT_USER_CODE    0x20
#define GDT_TSS          0x28

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
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0;       /* Stack for ring 0 transitions */
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;       /* IST1: double-fault stack */
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} TSS;

/* GDTR pointer loaded by lgdt */
typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} GDTPointer;

/* Initialize and load the GDT with TSS. */
void gdt_init(void);

/* Set the kernel stack pointer in TSS (for ring 3→0 transitions). */
void gdt_set_kernel_stack(uint64_t rsp0);

/* Assembly routine: load GDT, reload segment registers, load TSS. */
extern void gdt_flush(const GDTPointer *gdtr, uint16_t code_sel, uint16_t data_sel, uint16_t tss_sel);

#endif /* ARCHOS_ARCH_X86_64_GDT_H */
