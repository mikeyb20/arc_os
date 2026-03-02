/* arc_os — Minimal host-side test framework */

#include "test_framework.h"

/* Test result tracking */
static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

/* Collect tests from test files */
extern TestCase mem_tests[];
extern int mem_test_count;
extern TestCase pmm_tests[];
extern int pmm_test_count;
extern TestCase kprintf_tests[];
extern int kprintf_test_count;
extern TestCase kmalloc_tests[];
extern int kmalloc_test_count;
extern TestCase isr_tests[];
extern int isr_test_count;
extern TestCase thread_tests[];
extern int thread_test_count;
extern TestCase sched_tests[];
extern int sched_test_count;
extern TestCase process_tests[];
extern int process_test_count;
extern TestCase pci_tests[];
extern int pci_test_count;
extern TestCase virtio_tests[];
extern int virtio_test_count;

static void run_suite(const char *suite_name, TestCase *tests, int count) {
    printf("[%s] Running %d tests\n", suite_name, count);
    for (int i = 0; i < count; i++) {
        tests_run++;
        int result = tests[i].fn();
        if (result == 0) {
            tests_passed++;
            printf("  PASS: %s\n", tests[i].name);
        } else {
            tests_failed++;
        }
    }
}

typedef struct {
    const char *name;
    TestCase *tests;
    int *count;
} Suite;

int main(int argc, char **argv) {
    Suite suites[] = {
        { "mem",     mem_tests,     &mem_test_count },
        { "pmm",     pmm_tests,     &pmm_test_count },
        { "kprintf", kprintf_tests, &kprintf_test_count },
        { "kmalloc", kmalloc_tests, &kmalloc_test_count },
        { "isr",     isr_tests,     &isr_test_count },
        { "thread",  thread_tests,  &thread_test_count },
        { "sched",   sched_tests,   &sched_test_count },
        { "process", process_tests, &process_test_count },
        { "pci",     pci_tests,     &pci_test_count },
        { "virtio",  virtio_tests,  &virtio_test_count },
    };
    int suite_count = (int)(sizeof(suites) / sizeof(suites[0]));

    /* Optional --suite filter */
    const char *filter = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--suite") == 0 && i + 1 < argc) {
            filter = argv[++i];
        }
    }

    printf("=== arc_os host-side tests ===\n\n");

    for (int s = 0; s < suite_count; s++) {
        if (filter == NULL || strcmp(filter, suites[s].name) == 0) {
            run_suite(suites[s].name, suites[s].tests, *suites[s].count);
        }
    }

    printf("\n=== Results: %d passed, %d failed, %d total ===\n",
           tests_passed, tests_failed, tests_run);

    return tests_failed > 0 ? 1 : 0;
}
