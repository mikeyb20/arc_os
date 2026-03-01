#ifndef ARCHOS_ARCH_X86_64_ISR_H
#define ARCHOS_ARCH_X86_64_ISR_H

#include <stdint.h>

/* Number of ISR vectors */
#define ISR_COUNT 256

/* IRQ range */
#define IRQ_BASE   32
#define IRQ_COUNT  16

/* Interrupt frame pushed by isr_common (must match asm push order) */
typedef struct __attribute__((packed)) {
    /* Pushed by isr_common (in reverse order of pushes) */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    /* Pushed by ISR stub */
    uint64_t vector;
    uint64_t error_code;
    /* Pushed by CPU on interrupt */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} InterruptFrame;

/* ISR handler function type */
typedef void (*isr_handler_t)(InterruptFrame *frame);

/* Register a handler for a specific interrupt vector. */
void isr_register_handler(int vector, isr_handler_t handler);

/* C dispatcher called from assembly. */
void isr_dispatch(InterruptFrame *frame);

#endif /* ARCHOS_ARCH_X86_64_ISR_H */
