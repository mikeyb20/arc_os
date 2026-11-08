#ifndef ARCHOS_LIB_BUDGET_H
#define ARCHOS_LIB_BUDGET_H

/* Budget metrics (ADR-0019, M0-08). ARC_BUDGET(name, value) prints
 *     ARC_TEST budget <name> <value>
 * over serial in kernel test mode only; tools/budget-check.py compares the value
 * with its limit in tests/budgets.txt. Counts and sizes only, never time.
 * Adding a metric = one ARC_BUDGET line + one budgets.txt line
 * (docs/kernel/budgets.md). */

#include <stdint.h>
#include "lib/kprintf.h"
#include "lib/testmode.h"

#define ARC_BUDGET(name, value)                                              \
    do {                                                                     \
        if (testmode_active())                                               \
            kprintf("ARC_TEST budget %s %lu\n", (name), (uint64_t)(value)); \
    } while (0)

#endif /* ARCHOS_LIB_BUDGET_H */
