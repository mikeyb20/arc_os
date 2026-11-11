/* arc_os — Kernel symbol lookup (M1-04)
 *
 * Owns: address -> function name for backtraces, from the table the build
 * links in (tools/gen-ksyms.py): ksym_offsets sorted ascending, each with an
 * offset into the NUL-separated ksym_names blob.
 * Invariants: read-only data; lookups are safe from any context, panic
 * included. Locks: none. */

#include "lib/ksyms.h"
#include <stddef.h>

extern const uint32_t ksym_count;
extern const uint32_t ksym_offsets[];
extern const uint32_t ksym_name_offsets[];
extern const char ksym_names[];
extern const uint32_t ksym_end;   /* offset of _text_end: past the last function */

const char *ksym_lookup(uint64_t addr, uint64_t *offset) {
    if (ksym_count == 0 || addr < KSYM_BASE || addr - KSYM_BASE >= ksym_end) return NULL;
    uint32_t off = (uint32_t)(addr - KSYM_BASE);
    if (off < ksym_offsets[0]) return NULL;
    /* Last symbol at or below off */
    uint32_t lo = 0, hi = ksym_count - 1;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo + 1) / 2;
        if (ksym_offsets[mid] <= off)
            lo = mid;
        else
            hi = mid - 1;
    }
    if (offset != NULL) *offset = off - ksym_offsets[lo];
    return &ksym_names[ksym_name_offsets[lo]];
}
