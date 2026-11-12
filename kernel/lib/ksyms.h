#ifndef ARCHOS_LIB_KSYMS_H
#define ARCHOS_LIB_KSYMS_H

#include <stdint.h>

/* Kernel function symbols for panic backtraces (M1-04). The table is
 * generated at build time from the first kernel link (tools/gen-ksyms.py) and
 * linked into the second one. */

/* Base the table's 32-bit offsets count from (KERNEL_VMA in linker.ld). */
#define KSYM_BASE 0xFFFFFFFF80000000ULL

/* Name of the function containing addr, with *offset set to addr minus its
 * start. NULL when addr is outside kernel code. O(log n). */
const char *ksym_lookup(uint64_t addr, uint64_t *offset);

#endif /* ARCHOS_LIB_KSYMS_H */
