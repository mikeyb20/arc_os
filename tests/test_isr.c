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

/* Guard against real pic.h */
#define ARCHOS_ARCH_X86_64_PIC_H

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

/* --- Test suite export --- */

TestCase isr_tests[] = {
    { "register_and_dispatch",          test_register_and_dispatch },
    { "irq_dispatch_eoi",              test_irq_dispatch_eoi },
    { "spurious_irq_no_handler_no_eoi", test_spurious_irq_no_handler_no_eoi },
    { "irq_no_handler_still_eoi",      test_irq_no_handler_still_eoi },
    { "exception_no_eoi",              test_exception_no_eoi },
    { "high_vector_no_handler",        test_high_vector_no_handler_no_crash },
    { "register_bounds",               test_register_bounds },
    { "multiple_handlers",             test_multiple_handlers },
    { "handler_replacement",           test_handler_replacement },
    { "vector_255_boundary",           test_vector_255_boundary },
    { "vector_0_boundary",             test_vector_0_boundary },
    { "null_handler_default_path",     test_null_handler_default_path },
    { "null_handler_irq_eoi",          test_null_handler_irq_eoi },
    { "replace_handler_with_null",     test_replace_handler_with_null },
    { "irq_range_boundaries",         test_irq_range_boundaries },
};

int isr_test_count = sizeof(isr_tests) / sizeof(isr_tests[0]);
