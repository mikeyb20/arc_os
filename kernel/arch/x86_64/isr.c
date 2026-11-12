#include "arch/x86_64/isr.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/lapic.h"
#include "arch/x86_64/percpu.h"
#include "lib/kprintf.h"
#include "lib/panic.h"
#include <stddef.h>

static isr_handler_t handlers[ISR_COUNT];
static int apic_mode;

void isr_set_apic_mode(int enabled) {
    apic_mode = enabled;
}

/* Human-readable names for CPU exception vectors 0-31 */
static const char *exception_names[EXCEPTION_COUNT] = {
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

/* Backtrace (M1-04): follow the saved-RBP chain. The kernel keeps frame
 * pointers (-fno-omit-frame-pointer). A frame is followed only while it lies
 * on the stack being walked, within BT_STACK_WINDOW above its RSP, so a
 * corrupt RBP ends the walk instead of faulting in the panic path. */
#define BT_STACK_WINDOW   (64u * 1024)
#define KERNEL_HALF_START 0xFFFF800000000000ULL

static uint32_t backtrace_from(uint64_t rbp, uint64_t rsp, uint64_t *out, uint32_t n, uint32_t max) {
    while (n < max) {
        if (rbp < KERNEL_HALF_START || (rbp & 7) != 0 || rbp < rsp || rbp - rsp >= BT_STACK_WINDOW) break;
        const uint64_t *f = (const uint64_t *)rbp;
        if (f[1] == 0) break;
        out[n++] = f[1];   /* return address */
        if (f[0] <= rbp) break;   /* the chain must climb the stack */
        rbp = f[0];
    }
    return n;
}

static void panic_add_reg(PanicInfo *pi, const char *name, uint64_t value) {
    if (pi->nregs < PANIC_MAX_REGS) {
        pi->reg_names[pi->nregs] = name;
        pi->reg_values[pi->nregs] = value;
        pi->nregs++;
    }
}

static uint64_t read_cr2(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

static uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

void arch_panic_capture(PanicInfo *pi, void *frame_address) {
    uint64_t rsp, rflags;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    __asm__ volatile("pushf; pop %0" : "=r"(rflags));
    pi->cpu = percpu_current_index();
    panic_add_reg(pi, "RSP", rsp);
    panic_add_reg(pi, "RBP", (uint64_t)(uintptr_t)frame_address);
    panic_add_reg(pi, "RFLAGS", rflags);
    panic_add_reg(pi, "CR3", read_cr3());
    pi->nframes = backtrace_from((uint64_t)(uintptr_t)frame_address, rsp, pi->frames, 0, PANIC_MAX_FRAMES);
}

/* Unhandled CPU exception: the panic report with the interrupted registers. */
static void default_exception_handler(InterruptFrame *frame) {
    if (!panic_begin()) panic_halt();

    PanicInfo pi = {.reason = exception_names[frame->vector]};
    pi.cpu = percpu_current_index();
    panic_add_reg(&pi, "RIP", frame->rip);
    panic_add_reg(&pi, "RSP", frame->rsp);
    panic_add_reg(&pi, "RBP", frame->rbp);
    panic_add_reg(&pi, "RFLAGS", frame->rflags);
    panic_add_reg(&pi, "RAX", frame->rax);
    panic_add_reg(&pi, "RBX", frame->rbx);
    panic_add_reg(&pi, "RCX", frame->rcx);
    panic_add_reg(&pi, "RDX", frame->rdx);
    panic_add_reg(&pi, "RSI", frame->rsi);
    panic_add_reg(&pi, "RDI", frame->rdi);
    panic_add_reg(&pi, "R8", frame->r8);
    panic_add_reg(&pi, "R9", frame->r9);
    panic_add_reg(&pi, "R10", frame->r10);
    panic_add_reg(&pi, "R11", frame->r11);
    panic_add_reg(&pi, "R12", frame->r12);
    panic_add_reg(&pi, "R13", frame->r13);
    panic_add_reg(&pi, "R14", frame->r14);
    panic_add_reg(&pi, "R15", frame->r15);
    panic_add_reg(&pi, "CS", frame->cs);
    panic_add_reg(&pi, "SS", frame->ss);
    panic_add_reg(&pi, "ERROR", frame->error_code);
    panic_add_reg(&pi, "VECTOR", frame->vector);
    panic_add_reg(&pi, "CR2", read_cr2());
    panic_add_reg(&pi, "CR3", read_cr3());

    pi.frames[pi.nframes++] = frame->rip;
    if ((frame->cs & 3) == 0) {   /* never walk a user stack */
        pi.nframes = backtrace_from(frame->rbp, frame->rsp, pi.frames, pi.nframes, PANIC_MAX_FRAMES);
    }
    panic_report(&pi);
}

void isr_dispatch(InterruptFrame *frame) {
    uint64_t vector = frame->vector;

    /* IRQ path (vectors 32-47) */
    if (vector >= IRQ_BASE && vector < IRQ_BASE + IRQ_COUNT) {
        uint8_t irq = (uint8_t)(vector - IRQ_BASE);

        if (apic_mode) {
            /* APIC mode: LAPIC EOI */
            lapic_eoi();
        } else {
            /* PIC mode: check for spurious, send PIC EOI */
            if (pic_is_spurious(irq)) return;
            pic_send_eoi(irq);
        }

        /* Call registered handler if present */
        if (handlers[vector] != NULL) {
            handlers[vector](frame);
        }
        return;
    }

    /* IPI / high-vector path (0xF0+) — handled by registered handlers */
    if (vector >= 0xF0 && handlers[vector] != NULL) {
        handlers[vector](frame);
        return;
    }

    /* Exception/software interrupt path */
    if (vector < ISR_COUNT && handlers[vector] != NULL) {
        handlers[vector](frame);
        return;
    }

    /* Unhandled CPU exception (0-31) — print diagnostic and halt */
    if (vector < EXCEPTION_COUNT) {
        default_exception_handler(frame);
    }
}
