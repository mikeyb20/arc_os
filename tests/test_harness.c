/* arc_os — host test harness: runs TestSuite lists (test_framework.h) */

#include "test_framework.h"

int test_harness_main(int argc, char **argv, const char *title, const TestSuite *suites, int nsuites) {
    const char *filter = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--suite") == 0 && i + 1 < argc) filter = argv[++i];
    }
    printf("=== %s ===\n\n", title);
    int run = 0, passed = 0, failed = 0, matched = 0;
    for (int s = 0; s < nsuites; s++) {
        if (filter != NULL && strcmp(filter, suites[s].name) != 0) continue;
        matched++;
        int count = *suites[s].count;
        printf("[%s] Running %d tests\n", suites[s].name, count);
        for (int i = 0; i < count; i++) {
            run++;
            if (suites[s].tests[i].fn() == 0) {
                passed++;
                printf("  PASS: %s\n", suites[s].tests[i].name);
            } else {
                failed++;
            }
        }
    }
    if (filter != NULL && matched == 0) {
        printf("no suite named %s\n", filter);
        return 1;
    }
    printf("\n=== Results: %d passed, %d failed, %d total ===\n", passed, failed, run);
    return failed > 0 ? 1 : 0;
}
