/* arc_os — Host-side tests for kernel/arch/x86_64/isr.c dispatch logic */

#include "test_framework.h"
#include <stdint.h>
#include <stdbool.h>

/* Guard kernel headers that conflict or need stubbing */
#define ARCHOS_LIB_KPRINTF_H

/* Stub kprintf */
static inline void kprintf(const char *fmt, ...) { (void)fmt; }

/* Test-controlled PIC stubs (static to avoid linker clash with test_pic.c) */
static bool test_pic_spurious_result;
static int test_eoi_called;
static uint8_t test_eoi_irq;

static bool pic_is_spurious(uint8_t irq) {
    (void)irq;
    return test_pic_spurious_result;
}

static void pic_send_eoi(uint8_t irq) {
    test_eoi_called++;
    test_eoi_irq = irq;
}

/* Guard against real pic.h and lapic.h */
#define ARCHOS_ARCH_X86_64_PIC_H
#define ARCHOS_ARCH_X86_64_LAPIC_H

/* Stub lapic_eoi */
static int test_lapic_eoi_called;
static void lapic_eoi(void) { test_lapic_eoi_called++; }

/* Panic path stand-ins: no test raises an unhandled exception */
#include <stdlib.h>
#include "../kernel/lib/panic.h"
static int stub_panic_begin(void) {
    return 1;
}
static void stub_panic_report(const PanicInfo *pi) {
    (void)pi;
    abort();
}
static void stub_panic_halt(void) {
    abort();
}
#define panic_begin  stub_panic_begin
#define panic_report stub_panic_report
#define panic_halt   stub_panic_halt

/* M1-07 fault paths: the mm, signal, lock and FPU calls isr.c makes are
 * recorders. The real headers are read first, then the names are pointed at
 * the stubs (as for the panic functions above). */
#include <setjmp.h>
#include "arch/x86_64/fpu.h"
#include "mm/mm.h"
#include "proc/signal.h"
#include "proc/kernel_lock.h"
static int stub_fault_result;
static int stub_fault_calls;
static uint64_t stub_fault_addr;
static uint32_t stub_fault_access;
static int stub_mm_handle_fault(uint64_t addr, uint32_t access) {
    stub_fault_calls++;
    stub_fault_addr = addr;
    stub_fault_access = access;
    return stub_fault_result;
}
static int stub_sig_calls, stub_sig_signo, stub_sig_code;
static uint64_t stub_sig_addr;
static void stub_sig_force_fault(int signo, int code, uint64_t addr) {
    stub_sig_calls++;
    stub_sig_signo = signo;
    stub_sig_code = code;
    stub_sig_addr = addr;
}
static int stub_work_pending;
static int stub_deliver_calls;
static int64_t stub_sig_maybe_deliver(SyscallFrame *f, int64_t ret) {
    stub_deliver_calls++;
    f->rcx = 0x5000;   /* handler entry */
    f->rdi = 11;
    f->rsp -= 0x100;
    return ret;
}
static int stub_sig_work_pending(void) {
    return stub_work_pending;
}
static jmp_buf stub_fatal_jmp;
static int stub_fatal_signo;
__attribute__((noreturn)) static void stub_sig_fatal(int signo) {
    stub_fatal_signo = signo;
    longjmp(stub_fatal_jmp, 1);
}
static int stub_klock_depth, stub_klock_calls;
static void stub_kernel_lock(void) {
    stub_klock_depth++;
    stub_klock_calls++;
}
static void stub_kernel_unlock(void) {
    stub_klock_depth--;
}
static uint8_t stub_fpu_img[1024];
static void *stub_fpu_state_alloc(void) {
    return stub_fpu_img;
}
static void stub_fpu_state_free(void *p) {
    (void)p;
}
static void stub_fpu_save_current(void *p) {
    (void)p;
}
#define mm_handle_fault   stub_mm_handle_fault
#define sig_force_fault   stub_sig_force_fault
#define sig_maybe_deliver stub_sig_maybe_deliver
#define sig_work_pending  stub_sig_work_pending
#define sig_fatal         stub_sig_fatal
#define kernel_lock       stub_kernel_lock
#define kernel_unlock     stub_kernel_unlock
#define fpu_state_alloc   stub_fpu_state_alloc
#define fpu_state_free    stub_fpu_state_free
#define fpu_save_current  stub_fpu_save_current

/* The exception table and the IRET label come from the linker script and
 * syscall_entry.asm in the kernel: two made-up entries here */
__asm__(".section .rodata\n"
        ".balign 8\n"
        ".globl __ex_table_start\n"
        "__ex_table_start:\n"
        ".quad 0x1000, 0x1100\n"
        ".quad 0x2000, 0x2100\n"
        ".globl __ex_table_end\n"
        "__ex_table_end:\n"
        ".globl syscall_iret_user\n"
        "syscall_iret_user:\n"
        ".byte 0\n"
        ".text\n");

/* Include the real ISR implementation */
#include "../kernel/arch/x86_64/isr.c"

/* Test helper: create an InterruptFrame on the stack */
static InterruptFrame make_frame(uint64_t vector) {
    InterruptFrame f;
    memset(&f, 0, sizeof(f));
    f.vector = vector;
    return f;
}

/* Test-controlled handler tracking */
static int handler_called;
static uint64_t handler_vector;

static void test_handler(InterruptFrame *frame) {
    handler_called++;
    handler_vector = frame->vector;
}

static void reset_test_state(void) {
    handler_called = 0;
    handler_vector = 0;
    test_pic_spurious_result = false;
    test_eoi_called = 0;
    test_eoi_irq = 0;
    /* Clear all handlers */
    for (int i = 0; i < ISR_COUNT; i++) {
        handlers[i] = NULL;
    }
}

/* --- Tests --- */

static int test_register_and_dispatch(void) {
    reset_test_state();
    isr_register_handler(48, test_handler);  /* Vector 48, above IRQ range */

    InterruptFrame f = make_frame(48);
    isr_dispatch(&f);

    ASSERT_EQ(handler_called, 1);
    ASSERT_EQ(handler_vector, 48);
    return 0;
}

static int test_irq_dispatch_eoi(void) {
    reset_test_state();
    isr_register_handler(32, test_handler);  /* IRQ 0 = PIT timer */

    InterruptFrame f = make_frame(32);
    isr_dispatch(&f);

    ASSERT_EQ(handler_called, 1);
    ASSERT_EQ(test_eoi_called, 1);
    ASSERT_EQ(test_eoi_irq, 0);  /* IRQ 0 */
    return 0;
}

static int test_spurious_irq_no_handler_no_eoi(void) {
    reset_test_state();
    test_pic_spurious_result = true;
    isr_register_handler(39, test_handler);  /* IRQ 7 — classic spurious */

    InterruptFrame f = make_frame(39);
    isr_dispatch(&f);

    ASSERT_EQ(handler_called, 0);  /* Handler not called */
    ASSERT_EQ(test_eoi_called, 0); /* No EOI sent */
    return 0;
}

static int test_irq_no_handler_still_eoi(void) {
    reset_test_state();
    /* Vector 33 (IRQ 1) with no handler registered */
    InterruptFrame f = make_frame(33);
    isr_dispatch(&f);

    ASSERT_EQ(handler_called, 0);
    ASSERT_EQ(test_eoi_called, 1); /* EOI still sent */
    ASSERT_EQ(test_eoi_irq, 1);   /* IRQ 1 */
    return 0;
}

static int test_exception_no_eoi(void) {
    reset_test_state();
    isr_register_handler(6, test_handler);  /* Invalid Opcode */

    InterruptFrame f = make_frame(6);
    isr_dispatch(&f);

    ASSERT_EQ(handler_called, 1);
    ASSERT_EQ(test_eoi_called, 0);  /* Exceptions don't send EOI */
    return 0;
}

static int test_high_vector_no_handler_no_crash(void) {
    reset_test_state();
    /* Vector 200, not an IRQ, no handler — should silently do nothing */
    InterruptFrame f = make_frame(200);
    isr_dispatch(&f);

    ASSERT_EQ(handler_called, 0);
    ASSERT_EQ(test_eoi_called, 0);
    return 0;
}

static int test_register_bounds(void) {
    reset_test_state();
    /* Out-of-range vector — should be silently ignored */
    isr_register_handler(-1, test_handler);
    isr_register_handler(256, test_handler);

    /* Verify no handler was set for boundary vectors */
    ASSERT_TRUE(handlers[0] == NULL);
    ASSERT_TRUE(handlers[255] == NULL);
    return 0;
}

static int test_multiple_handlers(void) {
    reset_test_state();

    /* Register test_handler on vector 48, verify it's called */
    isr_register_handler(48, test_handler);

    InterruptFrame f1 = make_frame(48);
    isr_dispatch(&f1);
    ASSERT_EQ(handler_called, 1);
    ASSERT_EQ(handler_vector, 48);

    /* Register same handler on vector 49, verify independent dispatch */
    isr_register_handler(49, test_handler);

    handler_called = 0;
    InterruptFrame f2 = make_frame(49);
    isr_dispatch(&f2);
    ASSERT_EQ(handler_called, 1);
    ASSERT_EQ(handler_vector, 49);

    return 0;
}

/* --- Additional ISR tests --- */

/* Second handler for replacement tests */
static int handler2_called;
static uint64_t handler2_vector;

static void test_handler2(InterruptFrame *frame) {
    handler2_called++;
    handler2_vector = frame->vector;
}

static int test_handler_replacement(void) {
    reset_test_state();
    handler2_called = 0;
    handler2_vector = 0;

    isr_register_handler(48, test_handler);
    InterruptFrame f = make_frame(48);
    isr_dispatch(&f);
    ASSERT_EQ(handler_called, 1);

    /* Replace with handler2 */
    handler_called = 0;
    isr_register_handler(48, test_handler2);
    isr_dispatch(&f);
    ASSERT_EQ(handler_called, 0);   /* Original not called */
    ASSERT_EQ(handler2_called, 1);  /* Replacement called */
    ASSERT_EQ(handler2_vector, 48);
    return 0;
}

static int test_vector_255_boundary(void) {
    reset_test_state();
    isr_register_handler(255, test_handler);
    InterruptFrame f = make_frame(255);
    isr_dispatch(&f);
    ASSERT_EQ(handler_called, 1);
    ASSERT_EQ(handler_vector, 255);
    return 0;
}

static int test_vector_0_boundary(void) {
    reset_test_state();
    isr_register_handler(0, test_handler);  /* Division Error */
    InterruptFrame f = make_frame(0);
    isr_dispatch(&f);
    ASSERT_EQ(handler_called, 1);
    ASSERT_EQ(handler_vector, 0);
    ASSERT_EQ(test_eoi_called, 0);  /* Exception, no EOI */
    return 0;
}

static int test_null_handler_default_path(void) {
    reset_test_state();
    /* Vector 100: not an IRQ, not an exception (>31), no handler */
    InterruptFrame f = make_frame(100);
    isr_dispatch(&f);
    ASSERT_EQ(handler_called, 0);
    ASSERT_EQ(test_eoi_called, 0);
    return 0;
}

static int test_null_handler_irq_eoi(void) {
    reset_test_state();
    /* Vector 35 = IRQ 3, no handler registered */
    InterruptFrame f = make_frame(35);
    isr_dispatch(&f);
    ASSERT_EQ(handler_called, 0);
    ASSERT_EQ(test_eoi_called, 1);  /* EOI still sent for IRQ */
    ASSERT_EQ(test_eoi_irq, 3);
    return 0;
}

static int test_replace_handler_with_null(void) {
    reset_test_state();
    isr_register_handler(48, test_handler);
    isr_register_handler(48, NULL);

    InterruptFrame f = make_frame(48);
    isr_dispatch(&f);
    ASSERT_EQ(handler_called, 0);
    return 0;
}

static int test_irq_range_boundaries(void) {
    reset_test_state();

    /* Vector 32 = first IRQ (IRQ 0) */
    isr_register_handler(32, test_handler);
    InterruptFrame f = make_frame(32);
    isr_dispatch(&f);
    ASSERT_EQ(handler_called, 1);
    ASSERT_EQ(test_eoi_called, 1);
    ASSERT_EQ(test_eoi_irq, 0);

    /* Vector 47 = last IRQ (IRQ 15) */
    reset_test_state();
    isr_register_handler(47, test_handler);
    f = make_frame(47);
    isr_dispatch(&f);
    ASSERT_EQ(handler_called, 1);
    ASSERT_EQ(test_eoi_called, 1);
    ASSERT_EQ(test_eoi_irq, 15);

    /* Vector 48 = NOT an IRQ — should not send EOI */
    reset_test_state();
    isr_register_handler(48, test_handler);
    f = make_frame(48);
    isr_dispatch(&f);
    ASSERT_EQ(handler_called, 1);
    ASSERT_EQ(test_eoi_called, 0);
    return 0;
}

/* --- M1-07: faults on user memory, user exceptions, signal delivery --- */

static void reset_fault_stubs(void) {
    reset_test_state();
    stub_fault_result = MM_FAULT_OK;
    stub_fault_calls = 0;
    stub_fault_addr = 0;
    stub_fault_access = 0;
    stub_sig_calls = stub_sig_signo = stub_sig_code = 0;
    stub_sig_addr = 0;
    stub_work_pending = 0;
    stub_deliver_calls = 0;
    stub_fatal_signo = 0;
    stub_klock_depth = stub_klock_calls = 0;
    memset(stub_fpu_img, 0, sizeof(stub_fpu_img));
}

static InterruptFrame make_user_frame(uint64_t vector) {
    InterruptFrame f = make_frame(vector);
    f.cs = 0x23;
    f.rip = 0x401000;
    f.rsp = 0x7FFF0000;
    return f;
}

static int test_ex_table_lookup(void) {
    ASSERT_EQ(ex_table_fixup(0x1000), 0x1100);
    ASSERT_EQ(ex_table_fixup(0x2000), 0x2100);
    ASSERT_EQ(ex_table_fixup(0x1001), 0);
    ASSERT_EQ(ex_table_fixup(0), 0);
    return 0;
}

/* Error code bits become MM_ACCESS_*; a resolved user fault raises nothing */
static int test_user_pf_resolved(void) {
    reset_fault_stubs();
    InterruptFrame f = make_user_frame(EXCEPTION_PAGE_FAULT);
    f.error_code = PF_WRITE | PF_PRESENT | 0x4;   /* user write to a present page */
    page_fault(&f, 0x500123);
    ASSERT_EQ(stub_fault_calls, 1);
    ASSERT_EQ(stub_fault_addr, 0x500123);
    ASSERT_EQ(stub_fault_access, MM_ACCESS_WRITE | MM_ACCESS_PRESENT);
    ASSERT_EQ(stub_sig_calls, 0);
    f.error_code = PF_INSN;
    page_fault(&f, 0x600000);
    ASSERT_EQ(stub_fault_access, MM_ACCESS_EXEC);
    return 0;
}

/* SIGSEGV with SEGV_MAPERR (no mapping) or SEGV_ACCERR (not allowed), at
 * the faulting address, under the kernel lock */
static int test_user_pf_signals(void) {
    reset_fault_stubs();
    InterruptFrame f = make_user_frame(EXCEPTION_PAGE_FAULT);
    stub_fault_result = MM_FAULT_MAPERR;
    page_fault(&f, 0xdead000);
    ASSERT_EQ(stub_sig_calls, 1);
    ASSERT_EQ(stub_sig_signo, SIGSEGV);
    ASSERT_EQ(stub_sig_code, SEGV_MAPERR);
    ASSERT_EQ(stub_sig_addr, 0xdead000);
    ASSERT_EQ(stub_klock_calls, 1);
    ASSERT_EQ(stub_klock_depth, 0);
    stub_fault_result = MM_FAULT_ACCERR;
    page_fault(&f, 0x400010);
    ASSERT_EQ(stub_sig_code, SEGV_ACCERR);
    /* KILLED: the pending SIGKILL is the answer, no SIGSEGV on top */
    stub_fault_result = MM_FAULT_KILLED;
    page_fault(&f, 0x400010);
    ASSERT_EQ(stub_sig_calls, 2);
    return 0;
}

/* The kernel resolves a user-address fault only in the copy routine; when it
 * can't (or a spinlock is held) the copy continues at its fixup */
static int test_kernel_pf_fixup(void) {
    reset_fault_stubs();
    InterruptFrame f = make_frame(EXCEPTION_PAGE_FAULT);
    f.rip = 0x1000;                    /* in the exception table */
    page_fault(&f, 0x500000);
    ASSERT_EQ(stub_fault_calls, 1);
    ASSERT_EQ(f.rip, 0x1000);          /* resolved: the copy retries */
    stub_fault_result = MM_FAULT_MAPERR;
    page_fault(&f, 0x500000);
    ASSERT_EQ(f.rip, 0x1100);          /* fixup: the copy returns -EFAULT */
    ASSERT_EQ(stub_sig_calls, 0);      /* no signal for a kernel copy */

    stub_fault_result = MM_FAULT_OK;
    f.rip = 0x2000;
    hal_test_lock_depth = 1;           /* under a spinlock: no sleeping */
    int calls = stub_fault_calls;
    page_fault(&f, 0x500000);
    hal_test_lock_depth = 0;
    ASSERT_EQ(stub_fault_calls, calls);
    ASSERT_EQ(f.rip, 0x2100);
    return 0;
}

/* CPU exceptions in user mode map to Linux's signals and codes */
static int test_user_exception_signals(void) {
    static const struct {
        uint64_t vector;
        int signo, code;
        int has_addr;
    } cases[] = {
        {0, SIGFPE, FPE_INTDIV, 1},  {1, SIGTRAP, TRAP_TRACE, 1}, {3, SIGTRAP, SI_KERNEL, 0},
        {6, SIGILL, ILL_ILLOPN, 1},  {13, SIGSEGV, SI_KERNEL, 0}, {12, SIGBUS, SI_KERNEL, 0},
        {17, SIGBUS, BUS_ADRALN, 0}, {4, SIGSEGV, SI_KERNEL, 0},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        reset_fault_stubs();
        InterruptFrame f = make_user_frame(cases[i].vector);
        isr_dispatch(&f);
        ASSERT_EQ(stub_sig_calls, 1);
        ASSERT_EQ(stub_sig_signo, cases[i].signo);
        ASSERT_EQ(stub_sig_code, cases[i].code);
        ASSERT_EQ(stub_sig_addr, cases[i].has_addr ? 0x401000 : 0);
    }
    return 0;
}

/* #XM: the code comes from the unmasked MXCSR flags; #MF from FSW & ~FCW */
static int test_fpu_exception_codes(void) {
    reset_fault_stubs();
    uint32_t mxcsr = 0x1F80 & ~(1u << 9);   /* divide-by-zero unmasked */
    mxcsr |= 0x4;                           /* ZE flag */
    memcpy(stub_fpu_img + 24, &mxcsr, 4);
    InterruptFrame f = make_user_frame(19);
    isr_dispatch(&f);
    ASSERT_EQ(stub_sig_signo, SIGFPE);
    ASSERT_EQ(stub_sig_code, FPE_FLTDIV);
    ASSERT_EQ(stub_sig_addr, 0x401000);

    reset_fault_stubs();
    uint16_t fcw = 0x037F & ~0x08;   /* overflow unmasked */
    uint16_t fsw = 0x08;
    memcpy(stub_fpu_img, &fcw, 2);
    memcpy(stub_fpu_img + 2, &fsw, 2);
    f = make_user_frame(16);
    isr_dispatch(&f);
    ASSERT_EQ(stub_sig_code, FPE_FLTOVF);

    reset_fault_stubs();   /* masked flag only: spurious, no signal */
    fsw = 0x08;
    fcw = 0x037F;
    memcpy(stub_fpu_img, &fcw, 2);
    memcpy(stub_fpu_img + 2, &fsw, 2);
    f = make_user_frame(16);
    isr_dispatch(&f);
    ASSERT_EQ(stub_sig_calls, 0);
    return 0;
}

/* On return to user mode a pending signal is delivered through a
 * SyscallFrame copy of the interrupted context; never from NMI */
static int test_user_return_delivers(void) {
    reset_fault_stubs();
    isr_register_handler(32, test_handler);
    InterruptFrame f = make_user_frame(32);
    isr_dispatch(&f);
    ASSERT_EQ(stub_deliver_calls, 0);   /* nothing pending */
    stub_work_pending = 1;
    f.rcx = 0x1234;
    f.rax = 77;
    isr_dispatch(&f);
    ASSERT_EQ(stub_deliver_calls, 1);
    ASSERT_EQ(f.rip, 0x5000);
    ASSERT_EQ(f.rdi, 11);
    ASSERT_EQ(f.rsp, 0x7FFF0000 - 0x100);
    ASSERT_EQ(f.rcx, 0x1234);           /* untouched registers survive */
    ASSERT_EQ(f.rax, 77);
    ASSERT_EQ(stub_klock_depth, 0);

    InterruptFrame k = make_frame(32);  /* kernel mode: never */
    isr_dispatch(&k);
    ASSERT_EQ(stub_deliver_calls, 1);
    isr_register_handler(EXCEPTION_NMI, test_handler);
    InterruptFrame n = make_user_frame(EXCEPTION_NMI);
    isr_dispatch(&n);
    ASSERT_EQ(stub_deliver_calls, 1);
    return 0;
}

/* A #GP on the IRETQ to user mode is the user's bad context: SIGSEGV */
static int test_gp_at_iret_is_sigsegv(void) {
    reset_fault_stubs();
    InterruptFrame f = make_frame(13);
    f.rip = (uint64_t)(uintptr_t)syscall_iret_user;
    if (setjmp(stub_fatal_jmp) == 0) {
        isr_dispatch(&f);
        ASSERT_TRUE(0);   /* sig_fatal never returns */
    }
    ASSERT_EQ(stub_fatal_signo, SIGSEGV);
    ASSERT_EQ(stub_klock_depth, 1);    /* held into sig_fatal, as in the kernel */
    return 0;
}

/* --- Test suite export --- */

TestCase isr_tests[] = {
    {"register_and_dispatch", test_register_and_dispatch},
    {"irq_dispatch_eoi", test_irq_dispatch_eoi},
    {"spurious_irq_no_handler_no_eoi", test_spurious_irq_no_handler_no_eoi},
    {"irq_no_handler_still_eoi", test_irq_no_handler_still_eoi},
    {"exception_no_eoi", test_exception_no_eoi},
    {"high_vector_no_handler", test_high_vector_no_handler_no_crash},
    {"register_bounds", test_register_bounds},
    {"multiple_handlers", test_multiple_handlers},
    {"handler_replacement", test_handler_replacement},
    {"vector_255_boundary", test_vector_255_boundary},
    {"vector_0_boundary", test_vector_0_boundary},
    {"null_handler_default_path", test_null_handler_default_path},
    {"null_handler_irq_eoi", test_null_handler_irq_eoi},
    {"replace_handler_with_null", test_replace_handler_with_null},
    {"irq_range_boundaries", test_irq_range_boundaries},
    {"ex_table_lookup", test_ex_table_lookup},
    {"user_pf_resolved", test_user_pf_resolved},
    {"user_pf_signals", test_user_pf_signals},
    {"kernel_pf_fixup", test_kernel_pf_fixup},
    {"user_exception_signals", test_user_exception_signals},
    {"fpu_exception_codes", test_fpu_exception_codes},
    {"user_return_delivers", test_user_return_delivers},
    {"gp_at_iret_is_sigsegv", test_gp_at_iret_is_sigsegv},
};

int isr_test_count = sizeof(isr_tests) / sizeof(isr_tests[0]);
