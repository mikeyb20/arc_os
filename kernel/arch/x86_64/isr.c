#include "arch/x86_64/isr.h"
#include "arch/x86_64/pic.h"
#include "lib/kprintf.h"
#include <stddef.h>

static isr_handler_t handlers[ISR_COUNT];

/* Human-readable names for CPU exception vectors 0-31 */
static const char *exception_names[32] = {
    "Division Error",              /* 0 */
    "Debug",                       /* 1 */
    "Non-Maskable Interrupt",      /* 2 */
    "Breakpoint",                  /* 3 */
    "Overflow",                    /* 4 */
    "Bound Range Exceeded",        /* 5 */
    "Invalid Opcode",              /* 6 */
    "Device Not Available",        /* 7 */
    "Double Fault",                /* 8 */
    "Coprocessor Segment Overrun", /* 9 */
    "Invalid TSS",                 /* 10 */
    "Segment Not Present",         /* 11 */
    "Stack-Segment Fault",         /* 12 */
    "General Protection Fault",    /* 13 */
    "Page Fault",                  /* 14 */
    "Reserved",                    /* 15 */
    "x87 Floating-Point",         /* 16 */
    "Alignment Check",             /* 17 */
    "Machine Check",               /* 18 */
    "SIMD Floating-Point",        /* 19 */
    "Virtualization Exception",    /* 20 */
    "Control Protection",          /* 21 */
    "Reserved",                    /* 22 */
    "Reserved",                    /* 23 */
    "Reserved",                    /* 24 */
    "Reserved",                    /* 25 */
    "Reserved",                    /* 26 */
    "Reserved",                    /* 27 */
    "Hypervisor Injection",        /* 28 */
    "VMM Communication",           /* 29 */
    "Security Exception",          /* 30 */
    "Reserved",                    /* 31 */
};

void isr_register_handler(int vector, isr_handler_t handler) {
    if (vector >= 0 && vector < ISR_COUNT) {
        handlers[vector] = handler;
    }
}

static void default_exception_handler(InterruptFrame *frame) {
    kprintf("\n!!! EXCEPTION: %s (vector %lu, error=0x%lx)\n",
            exception_names[frame->vector], frame->vector, frame->error_code);
    kprintf("  RIP = 0x%lx  RSP = 0x%lx\n", frame->rip, frame->rsp);
    kprintf("  RAX = 0x%lx  RBX = 0x%lx  RCX = 0x%lx  RDX = 0x%lx\n",
            frame->rax, frame->rbx, frame->rcx, frame->rdx);
    kprintf("  RSI = 0x%lx  RDI = 0x%lx  RBP = 0x%lx\n",
            frame->rsi, frame->rdi, frame->rbp);
    kprintf("  R8  = 0x%lx  R9  = 0x%lx  R10 = 0x%lx  R11 = 0x%lx\n",
            frame->r8, frame->r9, frame->r10, frame->r11);
    kprintf("  R12 = 0x%lx  R13 = 0x%lx  R14 = 0x%lx  R15 = 0x%lx\n",
            frame->r12, frame->r13, frame->r14, frame->r15);
    kprintf("  CS  = 0x%lx  SS  = 0x%lx  RFLAGS = 0x%lx\n",
            frame->cs, frame->ss, frame->rflags);

    /* Page fault: print CR2 (faulting address) */
    if (frame->vector == 14) {
        uint64_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        kprintf("  CR2 = 0x%lx (faulting address)\n", cr2);
    }

    kprintf("!!! System halted.\n");
    for (;;) {
        __asm__ volatile ("cli; hlt");
    }
}

void isr_dispatch(InterruptFrame *frame) {
    uint64_t vector = frame->vector;

    /* IRQ path (vectors 32-47) */
    if (vector >= IRQ_BASE && vector < IRQ_BASE + IRQ_COUNT) {
        uint8_t irq = (uint8_t)(vector - IRQ_BASE);

        /* Check for spurious IRQ */
        if (pic_is_spurious(irq)) {
            return;
        }

        /* Send EOI before handler — handler may context_switch and never return */
        pic_send_eoi(irq);

        /* Call registered handler if present */
        if (handlers[vector] != NULL) {
            handlers[vector](frame);
        }
        return;
    }

    /* Exception/software interrupt path */
    if (vector < ISR_COUNT && handlers[vector] != NULL) {
        handlers[vector](frame);
        return;
    }

    /* Unhandled CPU exception (0-31) — print diagnostic and halt */
    if (vector < 32) {
        default_exception_handler(frame);
    }
}
