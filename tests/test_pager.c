/* arc_os — Host-side tests for kernel/diag/pager.c (bring-up page hold)
 *
 * The countdown figure and the key hint the text and QR pagers show. The
 * wait itself (diag_page_hold) needs the clock, timers and the keyboard and
 * is checked in QEMU (qemu.pager). ksnprintf comes from test_kprintf.c. */

#include "test_framework.h"
#include <stdint.h>
#include <string.h>

#include "../kernel/diag/pager.c"

#define SEC NSEC_PER_SEC

TEST(pager_seconds_left_rounds_up) {
    ASSERT_EQ(pager_seconds_left(0, 60 * SEC), 60);
    ASSERT_EQ(pager_seconds_left(1, 60 * SEC), 60);             /* just started: still the full figure */
    ASSERT_EQ(pager_seconds_left(SEC, 60 * SEC), 59);           /* a whole second gone */
    ASSERT_EQ(pager_seconds_left(59 * SEC, 60 * SEC), 1);
    ASSERT_EQ(pager_seconds_left(60 * SEC - 1, 60 * SEC), 1);   /* never 0 before the deadline */
    return 0;
}

TEST(pager_seconds_left_at_and_after_end) {
    ASSERT_EQ(pager_seconds_left(60 * SEC, 60 * SEC), 0);
    ASSERT_EQ(pager_seconds_left(61 * SEC, 60 * SEC), 0);
    ASSERT_EQ(pager_seconds_left(UINT64_MAX, 5), 0);
    return 0;
}

TEST(pager_seconds_left_huge_span) {
    /* Far beyond any countdown: clamped, never wrapped */
    ASSERT_EQ(pager_seconds_left(0, UINT64_MAX), (uint32_t)UINT32_MAX);
    return 0;
}

/* Walking the countdown the way diag_page_hold does (wake when the figure
 * changes: end - (left - 1) s) shows every figure from 60 down to 1 once. */
TEST(pager_countdown_steps) {
    uint64_t start = 123456789, end = start + PAGER_AUTO_SECONDS * SEC, now = start;
    uint32_t want = PAGER_AUTO_SECONDS, steps = 0;
    for (uint32_t left; (left = pager_seconds_left(now, end)) != 0; steps++) {
        ASSERT_EQ(left, want);
        want--;
        now = end - (uint64_t)(left - 1) * SEC;   /* the wake-up time */
    }
    ASSERT_EQ(steps, PAGER_AUTO_SECONDS);
    ASSERT_EQ(now, end);
    return 0;
}

TEST(pager_hint_text) {
    char buf[64];
    ASSERT_EQ(pager_hint(buf, sizeof(buf), 0, 60), (int)strlen("any key: next page (auto in 60 s)"));
    ASSERT_STR_EQ(buf, "any key: next page (auto in 60 s)");
    pager_hint(buf, sizeof(buf), 0, 1);
    ASSERT_STR_EQ(buf, "any key: next page (auto in 1 s)");
    pager_hint(buf, sizeof(buf), 1, 42);   /* manual: no countdown, whatever the figure */
    ASSERT_STR_EQ(buf, "any key: next page");
    return 0;
}

TEST(pager_hint_cut) {
    char buf[8];
    int n = pager_hint(buf, sizeof(buf), 0, 60);
    ASSERT_EQ(n, (int)strlen("any key: next page (auto in 60 s)"));   /* the full length, as ksnprintf */
    ASSERT_STR_EQ(buf, "any key");                                     /* cut, NUL-terminated */
    return 0;
}

TestCase pager_tests[] = {
    TEST_ENTRY(pager_seconds_left_rounds_up),
    TEST_ENTRY(pager_seconds_left_at_and_after_end),
    TEST_ENTRY(pager_seconds_left_huge_span),
    TEST_ENTRY(pager_countdown_steps),
    TEST_ENTRY(pager_hint_text),
    TEST_ENTRY(pager_hint_cut),
};

int pager_test_count = sizeof(pager_tests) / sizeof(pager_tests[0]);
