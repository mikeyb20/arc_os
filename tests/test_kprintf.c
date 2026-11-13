/* arc_os — Host-side tests for kernel/lib/kprintf.c and kernel/lib/klog.c
 *
 * Both are compiled in here for real, so the klog_* symbols the procfs,
 * fb_console and keyboard tests use come from this file. */

#include "test_framework.h"
#include <stdint.h>
#include <limits.h>

/* Some tests pass bad formats on purpose */
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wformat-overflow"

/* Capture buffer for what the log drain prints on serial */
static char capture_buf[128 * 1024];
static int capture_pos;

static void capture_reset(void) {
    capture_pos = 0;
    memset(capture_buf, 0, sizeof(capture_buf));
}

static const char *capture_get(void) {
    capture_buf[capture_pos] = '\0';
    return capture_buf;
}

/* hal.h stand-ins: serial goes to the capture buffer */
#define ARCHOS_ARCH_HAL_H
static void hal_serial_putchar(char c) {
    if (capture_pos < (int)sizeof(capture_buf) - 1) {
        capture_buf[capture_pos++] = c;
    }
}
static uint32_t test_cpu_index;   /* the simulated CPU klog runs on */
static uint32_t hal_cpu_index(void) {
    return test_cpu_index;
}
static uint64_t hal_tsc_read(void) {
    return 0;
}
static uint64_t hal_tsc_to_ns(uint64_t cycles) {
    (void)cycles;
    return 0;   /* rate unknown: klog_console_acquire falls back to its pause cap */
}
/* The real spinlock.h: host builds (ARC_HOST_TEST) simulate the interrupt flag */
#include "proc/spinlock.h"

#include "../kernel/lib/klog.c"
#include "../kernel/lib/kprintf.c"

/* --- Tests --- */

static int test_plain_string(void) {
    capture_reset();
    kprintf("hello world");
    ASSERT_STR_EQ(capture_get(), "hello world");
    return 0;
}

static int test_format_s(void) {
    capture_reset();
    kprintf("name=%s", "arc_os");
    ASSERT_STR_EQ(capture_get(), "name=arc_os");
    return 0;
}

static int test_format_s_null(void) {
    capture_reset();
    kprintf("%s", (char *)NULL);
    ASSERT_STR_EQ(capture_get(), "(null)");
    return 0;
}

static int test_format_d_positive(void) {
    capture_reset();
    kprintf("%d", 42);
    ASSERT_STR_EQ(capture_get(), "42");
    return 0;
}

static int test_format_d_negative(void) {
    capture_reset();
    kprintf("%d", -7);
    ASSERT_STR_EQ(capture_get(), "-7");
    return 0;
}

static int test_format_d_zero(void) {
    capture_reset();
    kprintf("%d", 0);
    ASSERT_STR_EQ(capture_get(), "0");
    return 0;
}

static int test_format_ld_int64_min(void) {
    capture_reset();
    kprintf("%ld", (int64_t)INT64_MIN);
    ASSERT_STR_EQ(capture_get(), "-9223372036854775808");
    return 0;
}

static int test_format_u(void) {
    capture_reset();
    kprintf("%u", (uint32_t)4294967295U);
    ASSERT_STR_EQ(capture_get(), "4294967295");
    return 0;
}

static int test_format_lu(void) {
    capture_reset();
    kprintf("%lu", (uint64_t)18446744073709551615ULL);
    ASSERT_STR_EQ(capture_get(), "18446744073709551615");
    return 0;
}

static int test_format_x(void) {
    capture_reset();
    kprintf("%x", (uint32_t)0xDEAD);
    ASSERT_STR_EQ(capture_get(), "dead");
    return 0;
}

static int test_format_lx(void) {
    capture_reset();
    kprintf("%lx", (uint64_t)0xCAFEBABEDEADBEEFULL);
    ASSERT_STR_EQ(capture_get(), "cafebabedeadbeef");
    return 0;
}

static int test_format_p(void) {
    capture_reset();
    kprintf("%p", (void *)0xFFFF800000001000ULL);
    ASSERT_STR_EQ(capture_get(), "0xffff800000001000");
    return 0;
}

static int test_format_p_null(void) {
    capture_reset();
    kprintf("%p", (void *)NULL);
    ASSERT_STR_EQ(capture_get(), "0x0000000000000000");
    return 0;
}

static int test_format_percent_literal(void) {
    capture_reset();
    kprintf("100%%");
    ASSERT_STR_EQ(capture_get(), "100%");
    return 0;
}

static int test_unknown_specifier(void) {
    capture_reset();
    kprintf("%q");
    ASSERT_STR_EQ(capture_get(), "%q");
    return 0;
}

static int test_unknown_specifier_with_length(void) {
    capture_reset();
    kprintf("%lq");
    ASSERT_STR_EQ(capture_get(), "%lq");
    return 0;
}

static int test_format_x_zero(void) {
    capture_reset();
    kprintf("%x", (uint32_t)0);
    ASSERT_STR_EQ(capture_get(), "0");
    return 0;
}

static int test_format_s_empty(void) {
    capture_reset();
    kprintf("%s", "");
    ASSERT_STR_EQ(capture_get(), "");
    return 0;
}

static int test_mixed_format(void) {
    capture_reset();
    kprintf("[%s] %d pages (%lu KB) at 0x%lx",
            "PMM", 42, (uint64_t)168, (uint64_t)0x1000);
    ASSERT_STR_EQ(capture_get(), "[PMM] 42 pages (168 KB) at 0x1000");
    return 0;
}


/* --- Width, precision, ksnprintf --- */

static int test_width_and_flags(void) {
    char b[64];
    ksnprintf(b, sizeof(b), "[%5d|%-5d|%05d|%-6s|%6s|%.3s|%3c]", 42, 42, -42, "ab", "ab", "abcdef", 'x');
    ASSERT_STR_EQ(b, "[   42|42   |-0042|ab    |    ab|abc|  x]");
    ksnprintf(b, sizeof(b), "%08lx %-4u|", (uint64_t)0xBEEF, 7u);
    ASSERT_STR_EQ(b, "0000beef 7   |");
    return 0;
}

static int test_ksnprintf_truncates(void) {
    char b[8];
    int n = ksnprintf(b, sizeof(b), "%s-%d", "abcdef", 1234);
    ASSERT_EQ(n, 11);                 /* the full length, like snprintf */
    ASSERT_STR_EQ(b, "abcdef-");      /* 7 bytes and the NUL */
    ASSERT_EQ(ksnprintf(b, 0, "xyz"), 3);
    return 0;
}

static int test_long_message_is_whole(void) {
    /* More than kprintf's 128-byte buffer: flushed in pieces, nothing lost */
    char want[301];
    for (int i = 0; i < 300; i++) want[i] = (char)('a' + i % 26);
    want[300] = '\0';
    capture_reset();
    kprintf("%s\n", want);
    ASSERT_EQ(strlen(capture_get()), 301u);
    ASSERT_EQ(strncmp(capture_get(), want, 300), 0);
    return 0;
}

/* --- Log ring --- */

/* Read the log from pos to the head into out (NUL-terminated). */
static size_t read_from(uint64_t pos, char *out, size_t n) {
    size_t got = klog_read(&pos, out, n - 1);
    out[got] = '\0';
    return got;
}

static int test_ring_keeps_level_markers(void) {
    capture_reset();
    kprintf("\n");   /* earlier tests may have left a line open */
    uint64_t start = klog_head();
    kprintf("one\n");
    kprintf_level(KLOG_ERR, "two %d\n", 2);
    char out[64];
    read_from(start, out, sizeof(out));
    ASSERT_STR_EQ(out, "<6>one\n<3>two 2\n");
    return 0;
}

static int test_console_level_filters(void) {
    kprintf("\n");
    capture_reset();
    uint64_t start = klog_head();
    kdebug("hidden %d\n", 1);
    kprintf("shown\n");
    ASSERT_STR_EQ(capture_get(), "shown\n");
    klog_set_console_level(KLOG_DEBUG);
    kdebug("now shown\n");
    klog_set_console_level(KLOG_INFO);
    ASSERT_STR_EQ(capture_get(), "shown\nnow shown\n");
    char out[128];
    read_from(start, out, sizeof(out));   /* the ring keeps debug lines */
    ASSERT_STR_EQ(out, "<7>hidden 1\n<6>shown\n<7>now shown\n");
    return 0;
}

static int test_console_level_clamped(void) {
    klog_set_console_level(99);
    ASSERT_EQ(klog_console_level(), KLOG_DEBUG);
    klog_set_console_level(-3);
    ASSERT_EQ(klog_console_level(), 0);
    klog_set_console_level(KLOG_INFO);
    return 0;
}

static int test_ring_wraps_and_read_clamps(void) {
    kprintf("\n");
    uint64_t start = klog_head();
    /* Write two ring sizes of numbered lines */
    char line[16];
    uint32_t lines = (uint32_t)(2 * (ring_mask + 1) / 12);
    for (uint32_t i = 0; i < lines; i++) {
        ksnprintf(line, sizeof(line), "L%07u\n", i);
        kprintf_level(KLOG_DEBUG, "%s", line);   /* debug: keeps the capture quiet */
    }
    ASSERT_EQ(klog_head() - klog_tail(), ring_mask + 1);   /* full ring */
    ASSERT_TRUE(klog_tail() > start);
    /* A reader behind the tail starts at the tail; the newest line is last */
    uint64_t pos = start;
    char out[64];
    size_t got = klog_read(&pos, out, 4);
    ASSERT_EQ(got, 4u);
    ASSERT_EQ(pos, klog_tail() + 4);
    pos = klog_head() - 12;
    got = klog_read(&pos, out, sizeof(out) - 1);
    out[got] = '\0';
    ksnprintf(line, sizeof(line), "<7>L%07u\n", lines - 1);
    ASSERT_STR_EQ(out, line);
    ASSERT_EQ(klog_read(&pos, out, sizeof(out)), 0u);   /* at the head */
    return 0;
}

static char big_ring[64 * 1024];

static int test_set_buffer_keeps_newest(void) {
    kprintf("\n");
    uint64_t head_before = klog_head();
    char before[32];
    uint64_t pos = head_before - 12;
    read_from(pos, before, sizeof(before));
    klog_set_buffer(big_ring, 1000);   /* not a power of two: refused */
    ASSERT_EQ(ring_mask + 1, (uint64_t)KLOG_EARLY_SIZE);
    klog_set_buffer(big_ring, sizeof(big_ring));
    ASSERT_EQ(ring_mask + 1, (uint64_t)sizeof(big_ring));
    ASSERT_EQ(klog_head(), head_before);
    ASSERT_EQ(klog_tail(), head_before > sizeof(big_ring) ? head_before - sizeof(big_ring) : 0);
    char after[32];
    read_from(head_before - 12, after, sizeof(after));
    ASSERT_STR_EQ(after, before);
    return 0;
}

static int test_console_owner_and_resync(void) {
    kprintf("\n");
    capture_reset();
    ASSERT_EQ(klog_console_try_acquire(), 1);
    ASSERT_EQ(klog_console_try_acquire(), 0);   /* busy */
    kprintf("queued\n");
    ASSERT_STR_EQ(capture_get(), "");            /* the owner prints it */
    klog_console_release();
    ASSERT_STR_EQ(capture_get(), "queued\n");

    /* Owner away while more than a ring is logged: the console loses the
     * oldest bytes, prints a newline and restarts at a whole line */
    capture_reset();
    ASSERT_EQ(klog_console_try_acquire(), 1);
    uint32_t lines = (uint32_t)((ring_mask + 1) / 8 + 16);
    for (uint32_t i = 0; i < lines; i++) kprintf("x%04u\n", i % 10000);   /* "<6>x0000\n" = 9 bytes */
    kprintf("last line\n");
    klog_console_release();
    const char *c = capture_get();
    ASSERT_EQ(c[0], '\n');
    ASSERT_TRUE(c[1] == 'x');                      /* a whole line, not a fragment */
    ASSERT_EQ(strlen(c + 1) % 6, 4u);         /* whole "xNNNN\n" lines + "last line\n" */
    ASSERT_TRUE(strstr(c, "last line\n") != NULL);
    ASSERT_TRUE(strchr(c, '<') == NULL);           /* no marker leaked */
    return 0;
}

/* klog_console_acquire: free -> taken; owned by this CPU (an interrupt
 * echoing a key over a drain) -> 0 at once; owned elsewhere -> 0 after the
 * bound (review finding 4) */
static int test_console_acquire_same_cpu(void) {
    kprintf("\n");
    test_cpu_index = 0;
    ASSERT_EQ(klog_console_acquire(), 1);
    ASSERT_EQ(console_owner, 1u);
    ASSERT_EQ(klog_console_acquire(), 0);   /* this CPU: no wait */
    test_cpu_index = 2;
    ASSERT_EQ(klog_console_acquire(), 0);   /* another CPU: bounded wait, then 0 */
    test_cpu_index = 0;
    klog_console_release();
    test_cpu_index = 2;
    ASSERT_EQ(klog_console_acquire(), 1);
    ASSERT_EQ(console_owner, 3u);
    klog_console_release();
    test_cpu_index = 0;
    return 0;
}

/* --- Test suite export --- */

/* klog_for_each_line from a line start keeps that line; from mid-line skips it */
static char seen_lines[256];

static void collect_line(const char *line, size_t len, void *ctx) {
    (void)ctx;
    size_t pos = strlen(seen_lines);
    if (pos + len + 2 > sizeof(seen_lines)) return;
    memcpy(seen_lines + pos, line, len);
    seen_lines[pos + len] = '|';
    seen_lines[pos + len + 1] = '\0';
}

TEST(klog_lines_from_line_start) {
    klog_write(6, "\n", 1);
    uint64_t at = klog_head();
    klog_write(6, "lsA\nlsB\n", 8);
    seen_lines[0] = '\0';
    klog_for_each_line(at, KLOG_INFO, collect_line, NULL);
    ASSERT_STR_EQ(seen_lines, "lsA|lsB|");
    seen_lines[0] = '\0';
    klog_for_each_line(at + 1, KLOG_INFO, collect_line, NULL);
    ASSERT_STR_EQ(seen_lines, "lsB|");
    return 0;
}

TestCase kprintf_tests[] = {
    TEST_ENTRY(klog_lines_from_line_start),
    {"plain_string", test_plain_string},
    {"format_s", test_format_s},
    {"format_s_null", test_format_s_null},
    {"format_d_positive", test_format_d_positive},
    {"format_d_negative", test_format_d_negative},
    {"format_d_zero", test_format_d_zero},
    {"format_ld_int64_min", test_format_ld_int64_min},
    {"format_u", test_format_u},
    {"format_lu", test_format_lu},
    {"format_x", test_format_x},
    {"format_lx", test_format_lx},
    {"format_p", test_format_p},
    {"format_p_null", test_format_p_null},
    {"format_percent_literal", test_format_percent_literal},
    {"unknown_specifier", test_unknown_specifier},
    {"unknown_specifier_with_len", test_unknown_specifier_with_length},
    {"format_x_zero", test_format_x_zero},
    {"format_s_empty", test_format_s_empty},
    {"mixed_format", test_mixed_format},
    {"width_and_flags", test_width_and_flags},
    {"ksnprintf_truncates", test_ksnprintf_truncates},
    {"long_message_is_whole", test_long_message_is_whole},
    {"ring_keeps_level_markers", test_ring_keeps_level_markers},
    {"console_level_filters", test_console_level_filters},
    {"console_level_clamped", test_console_level_clamped},
    {"ring_wraps_and_read_clamps", test_ring_wraps_and_read_clamps},
    {"set_buffer_keeps_newest", test_set_buffer_keeps_newest},
    {"console_owner_and_resync", test_console_owner_and_resync},
    {"console_acquire_same_cpu", test_console_acquire_same_cpu},
};

int kprintf_test_count = sizeof(kprintf_tests) / sizeof(kprintf_tests[0]);
