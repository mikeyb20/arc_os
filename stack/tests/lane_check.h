/*
 * lane_check.h — the check helper every C lane test uses (M2-02, lane-l.md).
 *
 *   lane_check(ok, "what");          prints "ok: what" or "FAIL: what" and counts failures
 *   return lane_check_done("suite"); prints the failure count; exit status 0 or 1
 *
 * Each check is printed, so a failing ctest log shows every result, not just the first.
 * One test program is one translation unit, so the counter lives here.
 */
#ifndef ARCHOS_STACK_TESTS_LANE_CHECK_H
#define ARCHOS_STACK_TESTS_LANE_CHECK_H

#include <stdio.h>

static int lane_check_failures;

/* lane_check: record one check; `what` says what was expected. */
static inline void lane_check(int ok, const char *what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) {
        lane_check_failures++;
    }
}

/* lane_check_done: print the summary line; returns the exit status for main(). */
static inline int lane_check_done(const char *suite) {
    printf("%s: %d failure(s)\n", suite, lane_check_failures);
    return lane_check_failures ? 1 : 0;
}

#endif /* ARCHOS_STACK_TESTS_LANE_CHECK_H */
