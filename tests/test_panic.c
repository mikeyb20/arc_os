/* arc_os — Host-side tests for kernel/lib/ksyms.c and kernel/lib/panic.c (M1-04) */

#include "test_framework.h"
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>

/* --- A small symbol table, as tools/gen-ksyms.py writes it --- */

const uint32_t ksym_count = 3;
const uint32_t ksym_end = 0x3000;
const uint32_t ksym_offsets[] = {0x1000, 0x1100, 0x2000};
const uint32_t ksym_name_offsets[] = {0, 4, 8};
const char ksym_names[] = "foo\0bar\0baz";

#include "../kernel/lib/ksyms.c"

/* --- panic.c with its outside calls recorded --- */

#include "../kernel/lib/panic.h"

/* kprintf / kprintf_level write into this buffer */
#define ARCHOS_LIB_KPRINTF_H
#define KLOG_EMERG 0
#define KLOG_INFO  6
static char out[8192];
static size_t out_len;
static void out_vadd(const char *fmt, va_list ap) {
    int n = vsnprintf(out + out_len, sizeof(out) - out_len, fmt, ap);
    if (n > 0) out_len += (size_t)n < sizeof(out) - out_len ? (size_t)n : sizeof(out) - out_len - 1;
}
static void kprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    out_vadd(fmt, ap);
    va_end(ap);
}
static void kprintf_level(int level, const char *fmt, ...) {
    (void)level;
    va_list ap;
    va_start(ap, fmt);
    out_vadd(fmt, ap);
    va_end(ap);
}

#define ARCHOS_LIB_KLOG_H
static int panic_mode_calls;
static void klog_panic_mode(void) {
    panic_mode_calls++;
}

#define ARCHOS_LIB_TESTMODE_H
#define TESTMODE_PANIC_CODE 0x7f
static int test_mode;
static jmp_buf stop_jmp;   /* testmode_exit and the halt unwind to here */
static int exit_code;
static int halted;
static int testmode_active(void) {
    return test_mode;
}
static const char *testmode_suite(void) {
    return "smoke";
}
__attribute__((noreturn)) static void testmode_exit(int code) {
    exit_code = code;
    longjmp(stop_jmp, 1);
}

#define ARCHOS_ARCH_HAL_H
static uint32_t this_cpu;
static int others_stopped;
static void hal_disable_interrupts(void) {}
static uint32_t hal_cpu_index(void) {
    return this_cpu;
}
static void hal_serial_puts(const char *s) {
    kprintf("%s", s);
}
static void hal_stop_other_cpus(void) {
    others_stopped++;
}
static void hal_halt_forever(void) {
    halted = 1;
    longjmp(stop_jmp, 1);
}
static void hal_panic_capture(PanicInfo *pi, void *frame_address) {
    (void)frame_address;
    pi->cpu = this_cpu;
    pi->reg_names[0] = "RIP";
    pi->reg_values[0] = KSYM_BASE + 0x1104;
    pi->nregs = 1;
    pi->frames[0] = KSYM_BASE + 0x1104;
    pi->nframes = 1;
}

#include "../kernel/lib/panic.c"

static int screen_calls;
static const PanicInfo *screen_info;
static size_t screen_out_len;   /* log length when the screen was drawn */
static void count_screen(const PanicInfo *info) {
    screen_calls++;
    screen_info = info;
    screen_out_len = out_len;
}

static void reset(int testmode) {
    panic_cpu = PANIC_NOBODY;
    out_len = 0;
    out[0] = '\0';
    panic_mode_calls = others_stopped = screen_calls = halted = 0;
    exit_code = -1;
    test_mode = testmode;
    this_cpu = 0;
    panic_set_screen(count_screen);
}

/* --- ksym_lookup --- */

static int test_ksym_lookup(void) {
    uint64_t off = 99;
    ASSERT_STR_EQ(ksym_lookup(KSYM_BASE + 0x1000, &off), "foo");
    ASSERT_EQ(off, 0);
    ASSERT_STR_EQ(ksym_lookup(KSYM_BASE + 0x10ff, &off), "foo");
    ASSERT_EQ(off, 0xff);
    ASSERT_STR_EQ(ksym_lookup(KSYM_BASE + 0x1100, &off), "bar");
    ASSERT_EQ(off, 0);
    ASSERT_STR_EQ(ksym_lookup(KSYM_BASE + 0x2fff, &off), "baz");
    ASSERT_EQ(off, 0xfff);
    ASSERT_STR_EQ(ksym_lookup(KSYM_BASE + 0x1234, NULL), "bar");
    return 0;
}

static int test_ksym_outside_code(void) {
    uint64_t off;
    ASSERT_TRUE(ksym_lookup(KSYM_BASE + 0xfff, &off) == NULL);    /* before the first function */
    ASSERT_TRUE(ksym_lookup(KSYM_BASE + 0x3000, &off) == NULL);   /* _text_end */
    ASSERT_TRUE(ksym_lookup(KSYM_BASE - 1, &off) == NULL);
    ASSERT_TRUE(ksym_lookup(0x401000, &off) == NULL);             /* user address */
    ASSERT_TRUE(ksym_lookup(UINT64_MAX, &off) == NULL);
    return 0;
}

/* --- panic sequence --- */

static int test_report_in_test_mode(void) {
    reset(1);
    this_cpu = 2;
    PanicInfo pi = {.reason = "Page Fault", .cpu = 2, .nregs = 2, .nframes = 2};
    pi.reg_names[0] = "RIP";
    pi.reg_values[0] = KSYM_BASE + 0x1010;
    pi.reg_names[1] = "CR2";
    pi.reg_values[1] = 0x10;
    pi.frames[0] = KSYM_BASE + 0x1010;
    pi.frames[1] = 0x401000;
    if (setjmp(stop_jmp) == 0) {
        ASSERT_EQ(panic_begin(), 1);
        panic_report(&pi);
    }
    ASSERT_EQ(panic_mode_calls, 1);
    ASSERT_EQ(others_stopped, 1);
    ASSERT_EQ(screen_calls, 1);
    ASSERT_TRUE(screen_info == &pi);   /* the screen gets the report's registers */
    ASSERT_EQ(exit_code, TESTMODE_PANIC_CODE);
    ASSERT_TRUE(strstr(out, "PANIC: Page Fault on CPU 2\n") != NULL);
    ASSERT_TRUE(strstr(out, "RIP    ffffffff80001010   CR2    0000000000000010\n") != NULL);
    ASSERT_TRUE(strstr(out, "#0  ffffffff80001010 foo+0x10\n") != NULL);
    ASSERT_TRUE(strstr(out, "#1  0000000000401000 ?\n") != NULL);
    ASSERT_TRUE(strstr(out, "ARC_TEST PANIC suite=smoke\n") != NULL);
    ASSERT_EQ(screen_out_len, strstr(out, "ARC_TEST PANIC") - out + strlen("ARC_TEST PANIC suite=smoke\n"));
    return 0;
}

static int test_report_halts_outside_test_mode(void) {
    reset(0);
    PanicInfo pi = {.reason = "General Protection"};
    if (setjmp(stop_jmp) == 0) {
        ASSERT_EQ(panic_begin(), 1);
        panic_report(&pi);
    }
    ASSERT_EQ(halted, 1);
    ASSERT_EQ(exit_code, -1);
    ASSERT_TRUE(strstr(out, "ARC_TEST") == NULL);
    return 0;
}

static int test_second_panic_stops(void) {
    reset(1);
    ASSERT_EQ(panic_begin(), 1);
    /* Another CPU panics meanwhile: it stops quietly */
    this_cpu = 1;
    ASSERT_EQ(panic_begin(), 0);
    ASSERT_TRUE(strstr(out, "fault while reporting") == NULL);
    /* The panicking CPU faults inside its own report: one line, then stop */
    this_cpu = 0;
    ASSERT_EQ(panic_begin(), 0);
    ASSERT_TRUE(strstr(out, "fault while reporting a panic") != NULL);
    ASSERT_EQ(panic_mode_calls, 1);
    ASSERT_EQ(others_stopped, 1);
    return 0;
}

static int test_kernel_panic_halt(void) {
    reset(1);
    this_cpu = 3;
    if (setjmp(stop_jmp) == 0) {
        kernel_panic_halt();
    }
    ASSERT_EQ(exit_code, TESTMODE_PANIC_CODE);
    ASSERT_TRUE(strstr(out, "PANIC: KERNEL_PANIC()") != NULL);
    ASSERT_TRUE(strstr(out, "on CPU 3\n") != NULL);
    ASSERT_TRUE(strstr(out, "bar+0x4\n") != NULL);
    /* A second KERNEL_PANIC() while the first is reported only halts */
    out_len = 0;
    exit_code = -1;
    if (setjmp(stop_jmp) == 0) {
        kernel_panic_halt();
    }
    ASSERT_EQ(halted, 1);
    ASSERT_EQ(exit_code, -1);
    return 0;
}

TestCase panic_tests[] = {
    {"ksym_lookup", test_ksym_lookup},
    {"ksym_outside_code", test_ksym_outside_code},
    {"report_in_test_mode", test_report_in_test_mode},
    {"report_halts_outside_test_mode", test_report_halts_outside_test_mode},
    {"second_panic_stops", test_second_panic_stops},
    {"kernel_panic_halt", test_kernel_panic_halt},
};

int panic_test_count = sizeof(panic_tests) / sizeof(panic_tests[0]);
