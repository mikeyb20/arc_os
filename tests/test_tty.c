/* arc_os — Host-side tests for kernel/drivers/tty.c */

#include "test_framework.h"
#include <stdint.h>

/* Guard headers that conflict or need stubbing */
#define ARCHOS_ARCH_X86_64_SERIAL_H
#define ARCHOS_PROC_SCHED_H
#define ARCHOS_LIB_KPRINTF_H

/* Stub kprintf */
static inline void kprintf(const char *fmt, ...) { (void)fmt; }

/* Serial echo capture */
#define ECHO_BUF_SIZE 512
static char echo_buf[ECHO_BUF_SIZE];
static int echo_pos;

static void serial_putchar(char c) {
    if (echo_pos < ECHO_BUF_SIZE - 1) {
        echo_buf[echo_pos++] = c;
    }
}

static void echo_reset(void) {
    memset(echo_buf, 0, sizeof(echo_buf));
    echo_pos = 0;
}

/* sched_yield stub — no-op in tests */
static void sched_yield(void) {}

/* Include the real TTY implementation */
#include "../kernel/drivers/tty.c"

/* Helper: reset TTY state between tests */
static void tty_reset(void) {
    tty_init();
    echo_reset();
}

/* --- Tests --- */

static int test_tty_echo_char(void) {
    tty_reset();
    tty_input_char('a');
    ASSERT_EQ(echo_pos, 1);
    ASSERT_EQ(echo_buf[0], 'a');
    return 0;
}

static int test_tty_newline_completes_line(void) {
    tty_reset();
    tty_input_char('h');
    tty_input_char('i');
    tty_input_char('\n');

    ASSERT_EQ(lines_ready, 1);

    char buf[16];
    int n = tty_read(buf, sizeof(buf));
    ASSERT_EQ(n, 3);
    ASSERT_EQ(buf[0], 'h');
    ASSERT_EQ(buf[1], 'i');
    ASSERT_EQ(buf[2], '\n');
    ASSERT_EQ(lines_ready, 0);
    return 0;
}

static int test_tty_backspace_removes_char(void) {
    tty_reset();
    tty_input_char('a');
    tty_input_char('b');
    tty_input_char('\b');
    tty_input_char('\n');

    char buf[16];
    int n = tty_read(buf, sizeof(buf));
    ASSERT_EQ(n, 2);  /* 'a' + '\n' */
    ASSERT_EQ(buf[0], 'a');
    ASSERT_EQ(buf[1], '\n');
    return 0;
}

static int test_tty_backspace_at_empty(void) {
    tty_reset();
    tty_input_char('\b');  /* Should be no-op */

    /* echo_buf should be empty — no \b \b sequence */
    ASSERT_EQ(echo_pos, 0);

    tty_input_char('\n');
    char buf[16];
    int n = tty_read(buf, sizeof(buf));
    ASSERT_EQ(n, 1);  /* Just '\n' */
    ASSERT_EQ(buf[0], '\n');
    return 0;
}

static int test_tty_multiple_lines_queued(void) {
    tty_reset();
    tty_input_char('a');
    tty_input_char('\n');
    tty_input_char('b');
    tty_input_char('\n');

    ASSERT_EQ(lines_ready, 2);

    char buf[16];
    int n = tty_read(buf, sizeof(buf));
    ASSERT_EQ(n, 2);
    ASSERT_EQ(buf[0], 'a');
    ASSERT_EQ(buf[1], '\n');
    ASSERT_EQ(lines_ready, 1);

    n = tty_read(buf, sizeof(buf));
    ASSERT_EQ(n, 2);
    ASSERT_EQ(buf[0], 'b');
    ASSERT_EQ(buf[1], '\n');
    ASSERT_EQ(lines_ready, 0);
    return 0;
}

static int test_tty_partial_read(void) {
    tty_reset();
    tty_input_char('h');
    tty_input_char('e');
    tty_input_char('l');
    tty_input_char('l');
    tty_input_char('o');
    tty_input_char('\n');

    /* Read only 3 bytes — should stop before newline */
    char buf[16];
    int n = tty_read(buf, 3);
    ASSERT_EQ(n, 3);
    ASSERT_EQ(buf[0], 'h');
    ASSERT_EQ(buf[1], 'e');
    ASSERT_EQ(buf[2], 'l');
    /* lines_ready still 1 since we didn't consume the newline */
    ASSERT_EQ(lines_ready, 1);
    return 0;
}

static int test_tty_write_serial(void) {
    tty_reset();
    const char *msg = "test";
    int n = tty_write(msg, 4);
    ASSERT_EQ(n, 4);
    ASSERT_EQ(echo_pos, 4);
    ASSERT_EQ(echo_buf[0], 't');
    ASSERT_EQ(echo_buf[1], 'e');
    ASSERT_EQ(echo_buf[2], 's');
    ASSERT_EQ(echo_buf[3], 't');
    return 0;
}

static int test_tty_backspace_echo_sequence(void) {
    tty_reset();
    tty_input_char('x');
    echo_reset();  /* Clear the 'x' echo, focus on backspace */

    tty_input_char('\b');
    /* Should echo: \b, space, \b */
    ASSERT_EQ(echo_pos, 3);
    ASSERT_EQ(echo_buf[0], '\b');
    ASSERT_EQ(echo_buf[1], ' ');
    ASSERT_EQ(echo_buf[2], '\b');
    return 0;
}

static int test_tty_read_zero_count(void) {
    tty_reset();
    char buf[16];
    int n = tty_read(buf, 0);
    ASSERT_EQ(n, 0);
    return 0;
}

static int test_tty_cr_acts_as_newline(void) {
    tty_reset();
    tty_input_char('x');
    tty_input_char('\r');

    ASSERT_EQ(lines_ready, 1);

    char buf[16];
    int n = tty_read(buf, sizeof(buf));
    ASSERT_EQ(n, 2);
    ASSERT_EQ(buf[0], 'x');
    ASSERT_EQ(buf[1], '\n');
    return 0;
}

/* --- Test suite export --- */

TestCase tty_tests[] = {
    { "echo_char",              test_tty_echo_char },
    { "newline_completes_line", test_tty_newline_completes_line },
    { "backspace_removes_char", test_tty_backspace_removes_char },
    { "backspace_at_empty",     test_tty_backspace_at_empty },
    { "multiple_lines_queued",  test_tty_multiple_lines_queued },
    { "partial_read",           test_tty_partial_read },
    { "write_serial",           test_tty_write_serial },
    { "backspace_echo_sequence",test_tty_backspace_echo_sequence },
    { "read_zero_count",        test_tty_read_zero_count },
    { "cr_acts_as_newline",     test_tty_cr_acts_as_newline },
};

int tty_test_count = sizeof(tty_tests) / sizeof(tty_tests[0]);
