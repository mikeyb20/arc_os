#ifndef ARCHOS_ARCH_X86_64_IDT_H
#define ARCHOS_ARCH_X86_64_IDT_H

#include <stdint.h>

/* Gate type constants */
#define IDT_GATE_INTERRUPT  0x8E  /* P=1, DPL=0, type=interrupt gate (0xE) */
#define IDT_GATE_TRAP       0x8F  /* P=1, DPL=0, type=trap gate (0xF) */
#define IDT_GATE_USER_INT   0xEE  /* P=1, DPL=3, type=interrupt gate */

#define IDT_ENTRIES 256

/* Interrupt Stack Table slots (TSS ist1..ist7). These vectors always run on a
 * per-CPU stack of their own, because they can arrive at any instruction:
 * #DF after a stack overflow, NMI and #MC even between SWAPGS and SYSRET while
 * RSP is already the user's (M1-03). */
#define IST_DOUBLE_FAULT  1
#define IST_NMI           2
#define IST_MACHINE_CHECK 3

/* IDT entry (16 bytes in long mode) */
typedef struct __attribute__((packed)) {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;        /* IST offset (bits 0-2), zero otherwise */
    uint8_t  type_attr;  /* gate type, DPL, present */
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} IDTEntry;

/* IDTR pointer loaded by lidt */
typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} IDTPointer;

/* Set a single IDT gate entry. */
void idt_set_gate(int vector, uint64_t handler, uint16_t selector,
                  uint8_t type_attr, uint8_t ist);

/* Build the shared IDT (all 256 stubs; NMI, #DF and #MC on their IST stacks)
 * and load it on this CPU. Call once, on the BSP. */
void idt_init(void);

/* Load the IDT that idt_init built on this CPU (application processors). */
void idt_load(void);

#endif /* ARCHOS_ARCH_X86_64_IDT_H */
