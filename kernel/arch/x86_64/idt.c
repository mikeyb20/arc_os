#include "arch/x86_64/idt.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/isr.h"
#include "lib/mem.h"
#include "lib/kprintf.h"

static IDTEntry idt[IDT_ENTRIES];
static IDTPointer idtr;

/* ISR stub table defined in isr_stubs.asm */
extern uint64_t isr_stub_table[ISR_COUNT];

void idt_set_gate(int vector, uint64_t handler, uint16_t selector,
                  uint8_t type_attr, uint8_t ist) {
    idt[vector].offset_low  = (uint16_t)(handler & 0xFFFF);
    idt[vector].selector    = selector;
    idt[vector].ist         = ist & 0x07;
    idt[vector].type_attr   = type_attr;
    idt[vector].offset_mid  = (uint16_t)((handler >> 16) & 0xFFFF);
    idt[vector].offset_high = (uint32_t)(handler >> 32);
    idt[vector].reserved    = 0;
}

void idt_init(void) {
    memset(idt, 0, sizeof(idt));

    /* Install ISR stubs for all 256 vectors */
    for (int i = 0; i < ISR_COUNT; i++) {
        uint8_t ist = 0;
        /* Double fault (vector 8) uses IST1 */
        if (i == 8) {
            ist = 1;
        }
        idt_set_gate(i, isr_stub_table[i], GDT_KERNEL_CODE,
                     IDT_GATE_INTERRUPT, ist);
    }

    /* Load IDT */
    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64_t)idt;
    __asm__ volatile ("lidt %0" : : "m"(idtr));

    kprintf("[HAL] IDT loaded (%d entries)\n", IDT_ENTRIES);
}
