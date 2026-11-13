#include "arch/x86_64/isr.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/lapic.h"
#include "arch/x86_64/percpu.h"
#include "arch/x86_64/fpu.h"
#include "arch/hal_lock.h"
#include "mm/mm.h"
#include "proc/signal.h"
#include "proc/kernel_lock.h"
#include "uapi/sigcontext.h"
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

static void isr_handle(InterruptFrame *frame);

/* --- Faults on user memory and user exceptions (M1-07) --- */

/* Page-fault error code bits */
#define PF_PRESENT 0x01u
#define PF_WRITE   0x02u
#define PF_RSVD    0x08u
#define PF_INSN    0x10u

#define USER_HALF_END 0x0000800000000000ULL

/* Exception table (linker.ld): an instruction allowed to fault on user
 * memory, and where it continues when the fault can't be resolved */
typedef struct ExTableEntry {
    uint64_t insn;
    uint64_t fixup;
} ExTableEntry;
extern const ExTableEntry __ex_table_start[], __ex_table_end[];

/* The IRETQ in syscall_entry.asm that returns through IRET */
extern const char syscall_iret_user[];

/* Fixup address for a faulting kernel RIP, or 0. One entry today (the user
 * copy routine, uaccess.asm), so a scan is the lookup. */
static uint64_t ex_table_fixup(uint64_t rip) {
    for (const ExTableEntry *e = __ex_table_start; e < __ex_table_end; e++) {
        if (e->insn == rip) return e->fixup;
    }
    return 0;
}

/* Raise signo on the current process for a user exception. */
static void user_fault_signal(int signo, int code, uint64_t addr) {
    kernel_lock();
    sig_force_fault(signo, code, addr);
    kernel_unlock();
}

/* #PF at addr (CR2): resolve it, fix up a user copy, or signal the user */
static void page_fault(InterruptFrame *frame, uint64_t addr) {
    uint64_t err = frame->error_code;
    if (err & PF_RSVD) {   /* a reserved bit in a page-table entry: corrupt tables */
        default_exception_handler(frame);
        return;
    }
    uint32_t access = 0;
    if (err & PF_WRITE) access |= MM_ACCESS_WRITE;
    if (err & PF_INSN) access |= MM_ACCESS_EXEC;
    if (err & PF_PRESENT) access |= MM_ACCESS_PRESENT;

    if ((frame->cs & 3) == 0) {
        /* The kernel may touch user memory only through the copy routine */
        uint64_t fixup = ex_table_fixup(frame->rip);
        if (fixup == 0 || addr >= USER_HALF_END) {
            default_exception_handler(frame);
            return;
        }
        /* Resolving may sleep: not under a spinlock (the copy fails instead) */
        if (hal_lock_depth() == 0 && mm_handle_fault(addr, access) == MM_FAULT_OK) return;
        frame->rip = fixup;   /* the copy returns what it couldn't copy: -EFAULT */
        return;
    }
    int r = mm_handle_fault(addr, access);
    if (r == MM_FAULT_OK || r == MM_FAULT_KILLED) return;   /* KILLED: SIGKILL goes out on return */
    user_fault_signal(SIGSEGV, r == MM_FAULT_ACCERR ? SEGV_ACCERR : SEGV_MAPERR, addr);
}

static void page_fault_handler(InterruptFrame *frame) {
    page_fault(frame, read_cr2());
}

/* si_code for a floating-point exception from the unmasked pending flags
 * (Linux's decode); 0 = spurious */
static int fpe_code(uint32_t err) {
    if (err & 0x001) return FPE_FLTINV;
    if (err & 0x004) return FPE_FLTDIV;
    if (err & 0x008) return FPE_FLTOVF;
    if (err & 0x012) return FPE_FLTUND;
    if (err & 0x020) return FPE_FLTRES;
    return 0;
}

/* #MF (x87) or #XM (SIMD): read the user's status from a saved image */
static int fpu_fault_code(int simd) {
    uint8_t *img = fpu_state_alloc();
    if (img == NULL) return FPE_FLTINV;
    fpu_save_current(img);
    uint16_t fcw = (uint16_t)(img[0] | (img[1] << 8));
    uint16_t fsw = (uint16_t)(img[2] | (img[3] << 8));
    uint32_t mxcsr =
        (uint32_t)img[24] | ((uint32_t)img[25] << 8) | ((uint32_t)img[26] << 16) | ((uint32_t)img[27] << 24);
    fpu_state_free(img);
    return fpe_code(simd ? (~(mxcsr >> 7) & mxcsr) : (uint32_t)(fsw & ~fcw));
}

/* A CPU exception in user mode becomes a signal, as Linux maps them.
 * Returns 0 for the ones that are never the user's (NMI, #DF, #MC, ...). */
static int user_exception(InterruptFrame *frame) {
    int signo;
    int code = SI_KERNEL;
    uint64_t addr = 0;
    switch (frame->vector) {
    case 0:
        signo = SIGFPE, code = FPE_INTDIV, addr = frame->rip;
        break;
    case 1:
        signo = SIGTRAP, code = TRAP_TRACE, addr = frame->rip;
        break;
    case 3:
        signo = SIGTRAP;
        break;
    case 4: /* into */
    case 5: /* bound */
    case 10: /* invalid TSS */
    case 13:
        signo = SIGSEGV;
        break;
    case 6:
        signo = SIGILL, code = ILL_ILLOPN, addr = frame->rip;
        break;
    case 9:
        signo = SIGFPE;
        break;
    case 11: /* segment not present */
    case 12:
        signo = SIGBUS;
        break;
    case 16:
    case 19:
        code = fpu_fault_code(frame->vector == 19);
        if (code == 0) return 1;   /* spurious: nothing to report */
        signo = SIGFPE, addr = frame->rip;
        break;
    case 17:
        signo = SIGBUS, code = BUS_ADRALN;
        break;
    default:
        return 0;
    }
    user_fault_signal(signo, code, addr);
    return 1;
}

/* Return to user mode: deliver a pending signal first (M1-07; syscalls do
 * this at syscall exit). The interrupted context goes through a SyscallFrame
 * so signal.c has one frame format. */
static void user_return_work(InterruptFrame *frame) {
    if (!sig_work_pending()) return;
    kernel_lock();
    SyscallFrame sf = {
        .ret_rcx = frame->rcx,
        .ret_r11 = frame->r11,
        .r9 = frame->r9,
        .r8 = frame->r8,
        .r10 = frame->r10,
        .rdx = frame->rdx,
        .rsi = frame->rsi,
        .rdi = frame->rdi,
        .r15 = frame->r15,
        .r14 = frame->r14,
        .r13 = frame->r13,
        .r12 = frame->r12,
        .rbp = frame->rbp,
        .rbx = frame->rbx,
        .rcx = frame->rip,
        .r11 = frame->rflags,
        .rsp = frame->rsp,
    };
    frame->rax = (uint64_t)sig_maybe_deliver(&sf, (int64_t)frame->rax);
    frame->rcx = sf.ret_rcx;
    frame->r11 = sf.ret_r11;
    frame->r9 = sf.r9;
    frame->r8 = sf.r8;
    frame->r10 = sf.r10;
    frame->rdx = sf.rdx;
    frame->rsi = sf.rsi;
    frame->rdi = sf.rdi;
    frame->r15 = sf.r15;
    frame->r14 = sf.r14;
    frame->r13 = sf.r13;
    frame->r12 = sf.r12;
    frame->rbp = sf.rbp;
    frame->rbx = sf.rbx;
    frame->rip = sf.rcx;
    frame->rflags = sf.r11;
    frame->rsp = sf.rsp;
    kernel_unlock();
}

/* NMI, #DF and #MC can land anywhere: never deliver from them */
static int vector_may_deliver(uint64_t vector) {
    return vector != EXCEPTION_NMI && vector != EXCEPTION_DOUBLE_FAULT && vector != EXCEPTION_MACHINE_CHECK;
}

void isr_dispatch(InterruptFrame *frame) {
    isr_handle(frame);
    if ((frame->cs & 3) != 0 && vector_may_deliver(frame->vector)) user_return_work(frame);
}

static void isr_handle(InterruptFrame *frame) {
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

    if (vector == EXCEPTION_PAGE_FAULT) {
        page_fault_handler(frame);
        return;
    }
    if (vector < EXCEPTION_COUNT && (frame->cs & 3) != 0 && user_exception(frame)) return;

    /* IRETQ to user mode refused the context rt_sigreturn restored: the
     * user's fault (GS is the kernel's again, isr_paranoid) */
    if (vector == 13 && frame->rip == (uint64_t)(uintptr_t)syscall_iret_user) {
        kernel_lock();
        kprintf("[SIG] bad user context on return: SIGSEGV\n");
        sig_fatal(SIGSEGV);
    }

    /* Unhandled CPU exception (0-31) — print diagnostic and halt */
    if (vector < EXCEPTION_COUNT) {
        default_exception_handler(frame);
    }
}
