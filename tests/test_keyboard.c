/* arc_os — Host-side tests for kernel/drivers/keyboard.c scancode translation */

#include "test_framework.h"
#include <stdint.h>
#include <stdbool.h>

/* Guard headers that conflict or need stubbing */
#define ARCHOS_ARCH_X86_64_ISR_H
#define ARCHOS_ARCH_X86_64_PIC_H
#define ARCHOS_ARCH_X86_64_IO_H
#define ARCHOS_ARCH_X86_64_SERIAL_H
#define ARCHOS_LIB_KPRINTF_H
#define ARCHOS_DRIVERS_TTY_H

/* Stub kprintf */
static inline void kprintf(const char *fmt, ...) { (void)fmt; }

/* --- ISR type stubs --- */

typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
} InterruptFrame;

typedef void (*isr_handler_t)(InterruptFrame *frame);

#define IRQ_BASE 32

/* ISR stubs */
static isr_handler_t registered_handler;
static int registered_vector;

static void isr_register_handler(int vector, isr_handler_t handler) {
    registered_handler = handler;
    registered_vector = vector;
}

/* PIC stub */
static int unmask_irq;
static void pic_unmask(uint8_t irq) { unmask_irq = irq; }

/* IO stub: test-controlled scancode */
static uint8_t test_scancode;
static uint8_t inb(uint16_t port) {
    (void)port;
    return test_scancode;
}

/* TTY input capture */
#define TTY_CAP_SIZE 64
static char tty_captured[TTY_CAP_SIZE];
static int tty_cap_pos;

static void tty_input_char(char c) {
    if (tty_cap_pos < TTY_CAP_SIZE - 1) {
        tty_captured[tty_cap_pos++] = c;
    }
}

static void tty_cap_reset(void) {
    memset(tty_captured, 0, sizeof(tty_captured));
    tty_cap_pos = 0;
}

/* Include the real keyboard implementation */
#include "../kernel/drivers/keyboard.c"

/* Helper: simulate a key press (send scancode through IRQ handler) */
static void press_key(uint8_t scancode) {
    test_scancode = scancode;
    InterruptFrame f;
    memset(&f, 0, sizeof(f));
    keyboard_irq_handler(&f);
}

/* Helper: reset all state */
static void kb_reset(void) {
    shift_held = 0;
    ctrl_held = 0;
    caps_lock = 0;
    tty_cap_reset();
}

/* --- Tests --- */

static int test_kb_a_key(void) {
    kb_reset();
    press_key(0x1E);  /* 'a' scancode */
    ASSERT_EQ(tty_cap_pos, 1);
    ASSERT_EQ(tty_captured[0], 'a');
    return 0;
}

static int test_kb_shift_a(void) {
    kb_reset();
    press_key(0x2A);  /* Left shift press */
    press_key(0x1E);  /* 'a' scancode */
    ASSERT_EQ(tty_cap_pos, 1);
    ASSERT_EQ(tty_captured[0], 'A');
    return 0;
}

static int test_kb_release_ignored(void) {
    kb_reset();
    press_key(0x1E);          /* 'a' press */
    press_key(0x1E | 0x80);   /* 'a' release */
    ASSERT_EQ(tty_cap_pos, 1);  /* Only the press generated a char */
    ASSERT_EQ(tty_captured[0], 'a');
    return 0;
}

static int test_kb_enter(void) {
    kb_reset();
    press_key(0x1C);  /* Enter scancode */
    ASSERT_EQ(tty_cap_pos, 1);
    ASSERT_EQ(tty_captured[0], '\n');
    return 0;
}

static int test_kb_backspace(void) {
    kb_reset();
    press_key(0x0E);  /* Backspace scancode */
    ASSERT_EQ(tty_cap_pos, 1);
    ASSERT_EQ(tty_captured[0], '\b');
    return 0;
}

static int test_kb_space(void) {
    kb_reset();
    press_key(0x39);  /* Space scancode */
    ASSERT_EQ(tty_cap_pos, 1);
    ASSERT_EQ(tty_captured[0], ' ');
    return 0;
}

static int test_kb_ctrl_c(void) {
    kb_reset();
    press_key(0x1D);  /* Left ctrl press */
    press_key(0x2E);  /* 'c' scancode */
    ASSERT_EQ(tty_cap_pos, 1);
    ASSERT_EQ(tty_captured[0], 0x03);  /* ETX control code */
    return 0;
}

static int test_kb_shift_1_exclaim(void) {
    kb_reset();
    press_key(0x2A);  /* Left shift press */
    press_key(0x02);  /* '1' scancode */
    ASSERT_EQ(tty_cap_pos, 1);
    ASSERT_EQ(tty_captured[0], '!');
    return 0;
}

static int test_kb_init_registers_vector_33(void) {
    registered_handler = NULL;
    registered_vector = 0;
    unmask_irq = 255;

    keyboard_init();

    ASSERT_TRUE(registered_handler != NULL);
    ASSERT_EQ(registered_vector, 33);  /* IRQ_BASE + 1 */
    ASSERT_EQ(unmask_irq, 1);
    return 0;
}

static int test_kb_e0_prefix_ignored(void) {
    kb_reset();
    press_key(0xE0);  /* Extended prefix */
    ASSERT_EQ(tty_cap_pos, 0);  /* No character generated */
    return 0;
}

static int test_kb_shift_release_unsets(void) {
    kb_reset();
    press_key(0x2A);  /* Left shift press */
    press_key(0x1E);  /* 'a' → 'A' */
    press_key(0xAA);  /* Left shift release */
    press_key(0x1E);  /* 'a' → 'a' */
    ASSERT_EQ(tty_cap_pos, 2);
    ASSERT_EQ(tty_captured[0], 'A');
    ASSERT_EQ(tty_captured[1], 'a');
    return 0;
}

static int test_kb_caps_lock_toggle(void) {
    kb_reset();
    press_key(0x3A);  /* Caps Lock press */
    press_key(0x1E);  /* 'a' → 'A' (caps on) */
    press_key(0x3A);  /* Caps Lock press again — toggle off */
    press_key(0x1E);  /* 'a' → 'a' */
    ASSERT_EQ(tty_cap_pos, 2);
    ASSERT_EQ(tty_captured[0], 'A');
    ASSERT_EQ(tty_captured[1], 'a');
    return 0;
}

/* --- Test suite export --- */

TestCase keyboard_tests[] = {
    { "a_key",                   test_kb_a_key },
    { "shift_a",                 test_kb_shift_a },
    { "release_ignored",         test_kb_release_ignored },
    { "enter",                   test_kb_enter },
    { "backspace",               test_kb_backspace },
    { "space",                   test_kb_space },
    { "ctrl_c",                  test_kb_ctrl_c },
    { "shift_1_exclaim",         test_kb_shift_1_exclaim },
    { "init_registers_vector_33",test_kb_init_registers_vector_33 },
    { "e0_prefix_ignored",      test_kb_e0_prefix_ignored },
    { "shift_release_unsets",    test_kb_shift_release_unsets },
    { "caps_lock_toggle",        test_kb_caps_lock_toggle },
};

int keyboard_test_count = sizeof(keyboard_tests) / sizeof(keyboard_tests[0]);
